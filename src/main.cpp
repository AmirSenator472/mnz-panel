#include <cstdint>
#include <windows.h>
#include <d3d9.h>
#include <cmath>

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#pragma comment(lib, "d3d9.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

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

// ==================== OFFSETS ====================
#define SAMP_INFO          0x21A0F8
#define SAMP_PLAYER_POOL   0x21A100
#define GTA_PLAYER_PTR     0xB6F5F0
#define GTA_CAMERA_X       0xB6F258
#define GTA_CAMERA_Z       0xB6F248
#define GTA_RECOIL         0x732E14
#define GTA_SPREAD         0x732E18
#define GTA_FLASH          0x732E1C

// ==================== STRUCTS ====================
struct CVector { float x, y, z; };

struct stPlayerInfo {
    uint32_t uiVehicleID;
    uint32_t uiPlayerID;
    uint32_t uiScore;
    float    fHealth;
    float    fArmour;
    uint32_t uiWeapon;
    uint32_t uiState;
    uint32_t uiPing;
    char     szName[24];
};

// ==================== GLOBALS ====================
HMODULE  g_hModule = nullptr;
HWND     g_hWnd = nullptr;
WNDPROC  oWndProc = nullptr;
bool     g_Init = false;
bool     g_ShowMenu = true;
int      g_ScreenW = 0, g_ScreenH = 0;
int      g_LocalID = -1;
int      g_TargetID = -1;

typedef HRESULT(WINAPI* EndScene_t)(IDirect3DDevice9*);
EndScene_t oEndScene = nullptr;

// ==================== HELPERS ====================
static CVector GetLocalPos() {
    uintptr_t p = *(uintptr_t*)GTA_PLAYER_PTR;
    if (!p) return {0,0,0};
    return *(CVector*)(p + 0x14);
}

static bool IsConnected(int id) {
    uintptr_t pool = *(uintptr_t*)SAMP_PLAYER_POOL;
    if (!pool) return false;
    return *(uintptr_t*)(pool + 4 + (id * 4)) != 0;
}

static stPlayerInfo* GetPlayer(int id) {
    uintptr_t pool = *(uintptr_t*)SAMP_PLAYER_POOL;
    if (!pool) return nullptr;
    uintptr_t p = *(uintptr_t*)(pool + 4 + (id * 4));
    if (!p) return nullptr;
    return (stPlayerInfo*)p;
}

static int GetLocalID() {
    uintptr_t info = *(uintptr_t*)SAMP_INFO;
    if (!info) return -1;
    return *(int*)(info + 0x08);
}

static float Dist3D(CVector a, CVector b) {
    float dx = b.x-a.x, dy = b.y-a.y, dz = b.z-a.z;
    return sqrtf(dx*dx+dy*dy+dz*dz);
}

// ==================== CHEATS ====================
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

        if (ImGui::BeginTabItem("ESP")) {
            ImGui::Checkbox("Enable ESP", &g_Cfg.esp);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Info")) {
            ImGui::Text("MNZ Panel v1.0");
            ImGui::Text("F7 = menu");
            ImGui::Text("RMB = aim");
            ImGui::Text("ALT = silent aim");
            ImGui::Separator();
            ImGui::Text("Made for baby");
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
    if (!g_Init) {
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

        oWndProc = (WNDPROC)SetWindowLongPtr(g_hWnd, GWLP_WNDPROC, (LONG_PTR)hkWndProc);

        g_Init = true;
    }

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    DrawMenu();

    // Memory cheats
    ApplyMemory();

    // Local ID
    g_LocalID = GetLocalID();

    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

    return oEndScene(pDevice);
}

// ==================== INSTALL ====================
static void InstallHooks() {
    IDirect3D9* pD3D = Direct3DCreate9(D3D_SDK_VERSION);
    if (!pD3D) return;

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

    if (SUCCEEDED(hr) && pDevice) {
        void** vtable = *(void***)pDevice;
        oEndScene = (EndScene_t)vtable[42];

        DWORD old;
        VirtualProtect(&vtable[42], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
        vtable[42] = (void*)hkEndScene;
        VirtualProtect(&vtable[42], sizeof(void*), old, &old);

        pDevice->Release();
    }
    pD3D->Release();
}

// ==================== MAIN ====================
static DWORD WINAPI MainThread(LPVOID) {
    while (!GetModuleHandleA("samp.dll")) Sleep(500);
    Sleep(2000);
    InstallHooks();
    return 0;
}

BOOL WINAPI DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
