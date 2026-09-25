#include <cstdint>
#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstdio>

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#pragma comment(lib, "d3d9.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ==================== LOG ====================
static FILE* g_Log = nullptr;

static void Log(const char* msg) {
    if (!g_Log) g_Log = fopen("MNZPanel.log", "a");
    if (g_Log) { fprintf(g_Log, "%s\n", msg); fflush(g_Log); }
}

// ==================== CONFIG ====================
struct Config {
    bool  aimbot = true;
    bool  silent = false;
    bool  esp = false;
    bool  no_recoil = true;
    bool  no_spread = true;
    bool  no_flash = false;
    float smooth = 12.0f;
    float fov = 25.0f;
    float max_dist = 100.0f;
    int   bone = 1;
    bool  visible_only = true;
} g_Cfg;

#define SAMP_INFO          0x21A0F8
#define SAMP_PLAYER_POOL   0x21A100
#define GTA_PLAYER_PTR     0xB6F5F0
#define GTA_RECOIL         0x732E14
#define GTA_SPREAD         0x732E18
#define GTA_FLASH          0x732E1C

struct CVector { float x, y, z; };

// ==================== GLOBALS ====================
HMODULE  g_hModule = nullptr;
HWND     g_hWnd = nullptr;
WNDPROC  oWndProc = nullptr;
bool     g_Init = false;
bool     g_ShowMenu = true;
bool     g_EndSceneLogged = false;
int      g_ScreenW = 0, g_ScreenH = 0;
int      g_LocalID = -1;

typedef HRESULT(WINAPI* EndScene_t)(IDirect3DDevice9*);
EndScene_t oEndScene = nullptr;

// ==================== HELPERS ====================
static int GetLocalID() {
    uintptr_t info = *(uintptr_t*)SAMP_INFO;
    if (!info) return -1;
    return *(int*)(info + 0x08);
}

static void ApplyMemory() {
    if (g_Cfg.no_recoil) *(float*)GTA_RECOIL = 0.0f;
    if (g_Cfg.no_spread) *(float*)GTA_SPREAD = 0.0f;
    if (g_Cfg.no_flash)  *(float*)GTA_FLASH  = 0.0f;
}

// ==================== MENU ====================
static void DrawMenu() {
    if (!g_ShowMenu) return;

    ImGui::SetNextWindowSize(ImVec2(420, 460), ImGuiCond_FirstUseEver);
    ImGui::Begin("MNZ Panel v1.0", &g_ShowMenu);
    ImGui::TextColored(ImVec4(0,1,0,1), "F7 = toggle menu");
    ImGui::Separator();

    if (ImGui::BeginTabBar("##tabs")) {

        if (ImGui::BeginTabItem("Aimbot")) {
            ImGui::Checkbox("Enable Aimbot", &g_Cfg.aimbot);
            ImGui::Checkbox("Silent Aim (ALT)", &g_Cfg.silent);
            ImGui::Checkbox("Visible Only", &g_Cfg.visible_only);
            ImGui::SliderFloat("Smooth", &g_Cfg.smooth, 1.0f, 20.0f);
            ImGui::SliderFloat("FOV", &g_Cfg.fov, 5.0f, 180.0f);
            ImGui::SliderFloat("Max Dist", &g_Cfg.max_dist, 10.0f, 500.0f);
            const char* bones[] = { "Head", "Chest", "Pelvis" };
            ImGui::Combo("Bone", &g_Cfg.bone, bones, 3);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Visual")) {
            ImGui::Checkbox("No Recoil", &g_Cfg.no_recoil);
            ImGui::Checkbox("No Spread", &g_Cfg.no_spread);
            ImGui::Checkbox("No Flash", &g_Cfg.no_flash);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Info")) {
            ImGui::Text("MNZ Panel v1.0");
            ImGui::Text("F7 = menu");
            ImGui::Text("RMB = aim");
            ImGui::Text("ALT = silent");
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}

// ==================== WNDPROC ====================
static LRESULT WINAPI hkWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN && wParam == VK_F7) {
        g_ShowMenu = !g_ShowMenu;
    }
    if (g_ShowMenu) {
        ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
    }
    return CallWindowProc(oWndProc, hWnd, msg, wParam, lParam);
}

// ==================== ENDSCENE ====================
static HRESULT WINAPI hkEndScene(IDirect3DDevice9* pDevice) {
    if (!g_EndSceneLogged) {
        Log("EndScene called");
        g_EndSceneLogged = true;
    }

    if (!g_Init) {
        Log("Init: getting params");
        D3DDEVICE_CREATION_PARAMETERS params;
        pDevice->GetCreationParameters(&params);
        g_hWnd = params.hFocusWindow;

        D3DVIEWPORT9 vp;
        pDevice->GetViewport(&vp);
        g_ScreenW = vp.Width;
        g_ScreenH = vp.Height;

        Log("Init: creating ImGui context");
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = nullptr;

        Log("Init: ImGui_ImplWin32_Init");
        ImGui_ImplWin32_Init(g_hWnd);

        Log("Init: ImGui_ImplDX9_Init");
        ImGui_ImplDX9_Init(pDevice);

        Log("Init: setting wndproc");
        oWndProc = (WNDPROC)SetWindowLongPtr(g_hWnd, GWLP_WNDPROC, (LONG_PTR)hkWndProc);

        g_Init = true;
        Log("Init: done");
    }

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    DrawMenu();
    ApplyMemory();

    g_LocalID = GetLocalID();

    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

    return oEndScene(pDevice);
}

// ==================== INSTALL ====================
static void InstallHooks() {
    Log("InstallHooks: start");

    IDirect3D9* pD3D = Direct3DCreate9(D3D_SDK_VERSION);
    if (!pD3D) { Log("Direct3DCreate9 failed"); return; }

    D3DPRESENT_PARAMETERS d3dpp = {};
    d3dpp.Windowed = TRUE;
    d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    d3dpp.hDeviceWindow = GetForegroundWindow();

    IDirect3DDevice9* pDevice = nullptr;
    HRESULT hr = pD3D->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
        d3dpp.hDeviceWindow, D3DCREATE_SOFTWARE_VERTEXPROCESSING,
        &d3dpp, &pDevice
    );

    if (FAILED(hr) || !pDevice) {
        Log("CreateDevice failed");
        pD3D->Release();
        return;
    }

    Log("InstallHooks: got device, hooking vtable[42]");

    void** vtable = *(void***)pDevice;
    oEndScene = (EndScene_t)vtable[42];

    DWORD old;
    VirtualProtect(&vtable[42], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    vtable[42] = (void*)hkEndScene;
    VirtualProtect(&vtable[42], sizeof(void*), old, &old);

    pDevice->Release();
    pD3D->Release();

    Log("InstallHooks: done");
}

// ==================== MAIN ====================
static DWORD WINAPI MainThread(LPVOID) {
    Log("MainThread: started");

    while (!GetModuleHandleA("samp.dll")) Sleep(500);
    Log("MainThread: samp.dll found");

    Sleep(2000);

    InstallHooks();
    return 0;
}

BOOL WINAPI DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        DeleteFileA("MNZPanel.log");
        CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
