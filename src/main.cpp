#include <cstdint>
#include <windows.h>
#include <d3d9.h>
#include <cmath>

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#pragma comment(lib, "d3d9.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ==================== OFFSETS ====================
#define SAMP_INFO          0x21A0F8
#define SAMP_PLAYER_POOL   0x21A100
#define GTA_PLAYER_PTR     0xB6F5F0
#define GTA_CAMERA_X       0xB6F258
#define GTA_CAMERA_Z       0xB6F248
#define GTA_RECOIL         0x732E14
#define GTA_SPREAD         0x732E18
#define GTA_FLASH          0x732E1C
#define GTA_D3D_DEVICE     0xC97C28

struct CVector { float x, y, z; };

// ==================== CONFIG ====================
struct Config {
    bool  aimbot = false;
    bool  visible_only = true;
    float smooth = 20.0f;
    float fov = 10.0f;
    float max_dist = 40.0f;
    int   bone = 1;

    bool  no_recoil = false;
    bool  no_spread = false;
    bool  no_flash = false;

    bool  show_menu = true;
} g_Cfg;

// ==================== GLOBALS ====================
HMODULE  g_hModule = nullptr;
HWND     g_hWnd = nullptr;
WNDPROC  oWndProc = nullptr;
bool     g_Init = false;
bool     g_F9Pressed = false;
int      g_LocalID = -1;
int      g_TargetID = -1;
int      g_ScreenW = 0, g_ScreenH = 0;

typedef HRESULT(WINAPI* EndScene_t)(IDirect3DDevice9*);
EndScene_t oEndScene = nullptr;

// ==================== SAFE MEMORY ====================
static int GetLocalID() {
    __try {
        uintptr_t info = *(uintptr_t*)SAMP_INFO;
        if (!info) return -1;
        int id = *(int*)(info + 0x08);
        if (id < 0 || id > 999) return -1;
        return id;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

static bool IsConnected(int id) {
    __try {
        uintptr_t pool = *(uintptr_t*)SAMP_PLAYER_POOL;
        if (!pool) return false;
        return *(uintptr_t*)(pool + 4 + (id * 4)) != 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static float GetHealth(int id) {
    __try {
        uintptr_t pool = *(uintptr_t*)SAMP_PLAYER_POOL;
        if (!pool) return 0.0f;
        uintptr_t p = *(uintptr_t*)(pool + 4 + (id * 4));
        if (!p) return 0.0f;
        return *(float*)(p + 0x0C);
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0.0f; }
}

static CVector GetLocalPos() {
    __try {
        uintptr_t p = *(uintptr_t*)GTA_PLAYER_PTR;
        if (!p) return {0,0,0};
        return *(CVector*)(p + 0x14);
    } __except(EXCEPTION_EXECUTE_HANDLER) { return {0,0,0}; }
}

static CVector GetRemotePos(int id) {
    __try {
        uintptr_t pool = *(uintptr_t*)SAMP_PLAYER_POOL;
        if (!pool) return {0,0,0};
        uintptr_t info = *(uintptr_t*)(pool + 4 + (id * 4));
        if (!info) return {0,0,0};

        uintptr_t offsets[] = { 0x40, 0x44, 0x48, 0x4C, 0x50, 0x54, 0x38, 0x3C };
        for (int i = 0; i < 8; i++) {
            uintptr_t ped = *(uintptr_t*)(info + offsets[i]);
            if (ped < 0x10000 || ped > 0xF0000000) continue;
            CVector v = *(CVector*)(ped + 0x14);
            if (isnan(v.x) || isnan(v.y) || isnan(v.z)) continue;
            if (fabsf(v.x) > 10000.0f || fabsf(v.y) > 10000.0f) continue;
            if (fabsf(v.z) > 1000.0f) continue;
            return v;
        }
        return {0,0,0};
    } __except(EXCEPTION_EXECUTE_HANDLER) { return {0,0,0}; }
}

static CVector GetPlayerPos(int id) {
    if (id == g_LocalID) return GetLocalPos();
    return GetRemotePos(id);
}

static float Dist3D(CVector a, CVector b) {
    float dx = b.x-a.x, dy = b.y-a.y, dz = b.z-a.z;
    return sqrtf(dx*dx+dy*dy+dz*dz);
}

static bool IsInFOV(CVector local, CVector target, float fov) {
    __try {
        float camX = *(float*)GTA_CAMERA_X;
        float dx = target.x - local.x;
        float dy = target.y - local.y;
        float ang = atan2f(dy, dx) * 57.2958f;
        float diff = ang - camX;
        while (diff > 180.0f) diff -= 360.0f;
        while (diff < -180.0f) diff += 360.0f;
        return fabsf(diff) <= fov / 2.0f;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ==================== LINE OF SIGHT ====================
typedef bool(__cdecl* LineOfSight_t)(CVector*, CVector*, bool, bool, bool, bool, bool);
#define GTA_LINE_OF_SIGHT 0x56A490

static bool IsVisible(CVector from, CVector to) {
    __try {
        LineOfSight_t fn = (LineOfSight_t)GTA_LINE_OF_SIGHT;
        return fn(&from, &to, true, false, false, true, true);
    } __except(EXCEPTION_EXECUTE_HANDLER) { return true; }
}

// ==================== AIMBOT ====================
static int FindBestTarget() {
    if (g_LocalID < 0) return -1;
    CVector local = GetLocalPos();
    int best = -1;
    float bestDist = g_Cfg.max_dist;

    for (int i = 0; i < 1000; i++) {
        if (i == g_LocalID) continue;
        if (!IsConnected(i)) continue;
        if (GetHealth(i) <= 0.0f) continue;

        CVector t = GetRemotePos(i);
        if (t.x == 0 && t.y == 0 && t.z == 0) continue;

        float d = Dist3D(local, t);
        if (d >= bestDist) continue;
        if (!IsInFOV(local, t, g_Cfg.fov)) continue;

        if (g_Cfg.visible_only) {
            CVector chest = { t.x, t.y, t.z + 0.4f };
            if (!IsVisible(local, chest)) continue;
        }

        bestDist = d;
        best = i;
    }
    return best;
}

static void AimAtTarget() {
    if (g_TargetID < 0) return;

    CVector local = GetLocalPos();
    CVector t = GetPlayerPos(g_TargetID);
    if (t.x == 0 && t.y == 0 && t.z == 0) return;

    float boneZ = 0.4f;
    if (g_Cfg.bone == 0) boneZ = 0.7f;
    else if (g_Cfg.bone == 2) boneZ = 0.1f;
    t.z += boneZ;

    float dx = t.x - local.x;
    float dy = t.y - local.y;
    float dz = t.z - local.z;
    float dist = sqrtf(dx*dx + dy*dy);

    float targetX = atan2f(dy, dx) * 57.2958f;
    float targetZ = atan2f(dz, dist) * -57.2958f;

    float smoothValue = g_Cfg.smooth;
    if (smoothValue < 2.0f) smoothValue = 2.0f;

    __try {
        float* camX = (float*)GTA_CAMERA_X;
        float* camZ = (float*)GTA_CAMERA_Z;

        float diffX = targetX - *camX;
        while (diffX > 180.0f) diffX -= 360.0f;
        while (diffX < -180.0f) diffX += 360.0f;

        *camX += diffX / smoothValue;
        *camZ += (targetZ - *camZ) / smoothValue;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

static void ApplyAimbot() {
    if (!g_Cfg.aimbot) return;
    if (g_LocalID < 0) return;

    g_TargetID = FindBestTarget();
    if (g_TargetID < 0) return;

    AimAtTarget();
}

// ==================== MEMORY ====================
static void ApplyMemory() {
    __try {
        if (g_Cfg.no_recoil) *(float*)GTA_RECOIL = 0.0f;
        if (g_Cfg.no_spread) *(float*)GTA_SPREAD = 0.0f;
        if (g_Cfg.no_flash)  *(float*)GTA_FLASH  = 0.0f;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

// ==================== THEME ====================
static void ApplyTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 8.0f;
    s.FrameRounding = 4.0f;
    s.GrabRounding = 4.0f;
    s.TabRounding = 4.0f;
    s.ScrollbarRounding = 4.0f;
    s.WindowPadding = ImVec2(14, 14);
    s.FramePadding = ImVec2(8, 5);
    s.ItemSpacing = ImVec2(8, 7);

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]         = ImVec4(0.05f, 0.05f, 0.08f, 0.96f);
    c[ImGuiCol_Border]           = ImVec4(0.30f, 0.20f, 0.50f, 0.60f);
    c[ImGuiCol_TitleBg]          = ImVec4(0.08f, 0.08f, 0.12f, 1.00f);
    c[ImGuiCol_TitleBgActive]    = ImVec4(0.15f, 0.08f, 0.28f, 1.00f);
    c[ImGuiCol_Tab]              = ImVec4(0.10f, 0.10f, 0.15f, 1.00f);
    c[ImGuiCol_TabHovered]       = ImVec4(0.25f, 0.15f, 0.45f, 1.00f);
    c[ImGuiCol_TabActive]        = ImVec4(0.35f, 0.20f, 0.60f, 1.00f);
    c[ImGuiCol_Button]           = ImVec4(0.20f, 0.15f, 0.35f, 1.00f);
    c[ImGuiCol_ButtonHovered]    = ImVec4(0.30f, 0.20f, 0.55f, 1.00f);
    c[ImGuiCol_ButtonActive]     = ImVec4(0.40f, 0.25f, 0.70f, 1.00f);
    c[ImGuiCol_FrameBg]          = ImVec4(0.10f, 0.10f, 0.15f, 1.00f);
    c[ImGuiCol_FrameBgHovered]   = ImVec4(0.15f, 0.15f, 0.25f, 1.00f);
    c[ImGuiCol_SliderGrab]       = ImVec4(0.55f, 0.35f, 0.85f, 1.00f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.65f, 0.45f, 0.95f, 1.00f);
    c[ImGuiCol_CheckMark]        = ImVec4(0.65f, 0.45f, 0.95f, 1.00f);
    c[ImGuiCol_Header]           = ImVec4(0.20f, 0.15f, 0.35f, 1.00f);
    c[ImGuiCol_HeaderHovered]    = ImVec4(0.30f, 0.20f, 0.55f, 1.00f);
    c[ImGuiCol_HeaderActive]     = ImVec4(0.40f, 0.25f, 0.70f, 1.00f);
    c[ImGuiCol_Separator]        = ImVec4(0.30f, 0.20f, 0.50f, 0.60f);
    c[ImGuiCol_Text]             = ImVec4(0.95f, 0.95f, 1.00f, 1.00f);
    c[ImGuiCol_TextDisabled]     = ImVec4(0.50f, 0.50f, 0.55f, 1.00f);
}

// ==================== WNDPROC ====================
static LRESULT WINAPI hkWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_Init) {
        ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
    }
    return CallWindowProc(oWndProc, hWnd, msg, wParam, lParam);
}

// ==================== MENU ====================
static void DrawMenu() {
    if (!g_Cfg.show_menu) return;

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus;

    ImGui::SetNextWindowSize(ImVec2(480, 460), ImGuiCond_FirstUseEver);
    ImGui::Begin("MNZ Panel v2.0", &g_Cfg.show_menu, flags);

    ImGui::TextColored(ImVec4(0.7f, 0.5f, 1.0f, 1.0f), "MNZ Panel v2.0");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "| F9 = toggle");
    ImGui::Separator();

    if (ImGui::BeginTabBar("##tabs")) {

        if (ImGui::BeginTabItem("Aimbot")) {
            ImGui::Spacing();

            if (g_Cfg.aimbot) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.55f, 0.20f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.65f, 0.25f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.45f, 0.15f, 1.0f));
                if (ImGui::Button("AIMBOT: ON", ImVec2(220, 42))) g_Cfg.aimbot = false;
                ImGui::PopStyleColor(3);
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.40f, 0.15f, 0.15f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.55f, 0.20f, 0.20f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.30f, 0.10f, 0.10f, 1.0f));
                if (ImGui::Button("AIMBOT: OFF", ImVec2(220, 42))) g_Cfg.aimbot = true;
                ImGui::PopStyleColor(3);
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Checkbox("Visible Only (anti-wallshot)", &g_Cfg.visible_only);

            ImGui::Spacing();
            ImGui::Text("Smooth (higher = smoother/legit)");
            ImGui::SliderFloat("##smooth", &g_Cfg.smooth, 2.0f, 25.0f, "%.1f");
            ImGui::Text("FOV");
            ImGui::SliderFloat("##fov", &g_Cfg.fov, 5.0f, 90.0f, "%.0f");
            ImGui::Text("Max Distance");
            ImGui::SliderFloat("##maxdist", &g_Cfg.max_dist, 10.0f, 200.0f, "%.0f m");

            const char* bones[] = { "Head", "Chest", "Pelvis" };
            ImGui::Text("Target Bone");
            ImGui::Combo("##bone", &g_Cfg.bone, bones, 3);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            if (ImGui::Button("Apply War Preset", ImVec2(220, 32))) {
                g_Cfg.aimbot = true;
                g_Cfg.visible_only = true;
                g_Cfg.smooth = 20.0f;
                g_Cfg.fov = 10.0f;
                g_Cfg.max_dist = 40.0f;
                g_Cfg.bone = 1;
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Visual")) {
            ImGui::Spacing();
            ImGui::Checkbox("No Recoil", &g_Cfg.no_recoil);
            ImGui::Checkbox("No Spread", &g_Cfg.no_spread);
            ImGui::Checkbox("No Flash", &g_Cfg.no_flash);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Info")) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.7f, 0.5f, 1.0f, 1.0f), "MNZ Panel v2.0");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::Text("F9      - Toggle menu");
            ImGui::Text("Aimbot  - Toggle ON/OFF in Aimbot tab");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.6f, 0.4f, 0.9f, 1.0f), "Made for baby");
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
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

        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;

        ImGui_ImplWin32_Init(g_hWnd);
        ImGui_ImplDX9_Init(pDevice);

        ApplyTheme();

        oWndProc = (WNDPROC)SetWindowLongPtr(g_hWnd, GWLP_WNDPROC, (LONG_PTR)hkWndProc);

        g_Init = true;
    }

    bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (f9 && !g_F9Pressed) g_Cfg.show_menu = !g_Cfg.show_menu;
    g_F9Pressed = f9;

    g_LocalID = GetLocalID();

    // فقط اگر منو بازه ImGui رندر کن — که وقتی بسته‌ست، D3D state اصلاً دست نخوره
    if (g_Cfg.show_menu) {
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();

        ImGui::GetIO().WantCaptureKeyboard = false;
        ImGui::GetIO().WantCaptureMouse = ImGui::GetIO().WantCaptureMouse;

        ImGui::NewFrame();

        DrawMenu();

        ImGui::EndFrame();
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    }

    // cheats جدا از ImGui رندر
    if (g_LocalID >= 0 && g_Cfg.aimbot) ApplyAimbot();
    if (g_Cfg.no_recoil || g_Cfg.no_spread || g_Cfg.no_flash) ApplyMemory();

    return oEndScene(pDevice);
}

// ==================== INSTALL ====================
static void InstallHooks() {
    IDirect3DDevice9* pDevice = nullptr;
    for (int i = 0; i < 120; i++) {
        pDevice = *(IDirect3DDevice9**)GTA_D3D_DEVICE;
        if (pDevice) break;
        Sleep(500);
    }
    if (!pDevice) return;

    void** vtable = *(void***)pDevice;
    oEndScene = (EndScene_t)vtable[42];

    DWORD old;
    VirtualProtect(&vtable[42], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    vtable[42] = (void*)hkEndScene;
    VirtualProtect(&vtable[42], sizeof(void*), old, &old);
}

static DWORD WINAPI MainThread(LPVOID) {
    while (!GetModuleHandleA("samp.dll")) Sleep(500);
    Sleep(5000);
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
