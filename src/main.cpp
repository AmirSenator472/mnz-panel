#include <cstdint>
#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstdio>

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#pragma comment(lib, "d3d9.lib")

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
#define GTA_D3D_DEVICE     0xC97C28

struct CVector { float x, y, z; };

// ==================== GLOBALS ====================
HMODULE  g_hModule = nullptr;
HWND     g_hWnd = nullptr;
bool     g_Init = false;
bool     g_ShowMenu = true;
bool     g_EndSceneLogged = false;
bool     g_FirstDrawLogged = false;
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
    ImGui::Begin("MNZ Panel v1.0");
    ImGui::TextColored(ImVec4(0,1,0,1), "INSERT = toggle menu");
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
            ImGui::Text("INSERT = toggle");
            ImGui::Text("Made for baby");
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}

// ==================== ENDSCENE ====================
static HRESULT WINAPI hkEndScene(IDirect3DDevice9* pDevice) {
    if (!g_EndSceneLogged) {
        Log("EndScene called");
        g_EndSceneLogged = true;
    }

    if (!g_Init) {
        Log("Init: start");
        D3DDEVICE_CREATION_PARAMETERS params;
        pDevice->GetCreationParameters(&params);
        g_hWnd = params.hFocusWindow;

        D3DVIEWPORT9 vp;
        pDevice->GetViewport(&vp);
        g_ScreenW = vp.Width;
        g_ScreenH = vp.Height;

        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = nullptr;

        ImGui_ImplWin32_Init(g_hWnd);
        ImGui_ImplDX9_Init(pDevice);

        g_Init = true;
        Log("Init: done");
    }

    if (GetAsyncKeyState(VK_INSERT) & 1) {
        g_ShowMenu = !g_ShowMenu;
    }

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    DrawMenu();
    ApplyMemory();

    if (!g_FirstDrawLogged) {
        Log("First draw done");
        g_FirstDrawLogged = true;
    }

    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

    return oEndScene(pDevice);
}

// ==================== INSTALL ====================
static void InstallHooks() {
    Log("InstallHooks: waiting for game D3D device");

    IDirect3DDevice9* pDevice = nullptr;
    for (int i = 0; i < 120; i++) {
        pDevice = *(IDirect3DDevice9**)GTA_D3D_DEVICE;
        if (pDevice) break;
        Sleep(500);
    }

    if (!pDevice) {
        Log("InstallHooks: device not found");
        return;
    }

    void** vtable = *(void***)pDevice;
    oEndScene = (EndScene_t)vtable[42];

    DWORD old;
    VirtualProtect(&vtable[42], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    vtable[42] = (void*)hkEndScene;
    VirtualProtect(&vtable[42], sizeof(void*), old, &old);

    Log("InstallHooks: done");
}

// ==================== MAIN ====================
static DWORD WINAPI MainThread(LPVOID) {
    Log("MainThread: started");

    while (!GetModuleHandleA("samp.dll")) Sleep(500);
    Log("MainThread: samp.dll found");

    Sleep(5000);

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
