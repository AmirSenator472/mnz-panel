#include <cstdint>
#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstdio>

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#pragma comment(lib, "d3d9.lib")

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
#define GTA_CALC_SCREEN    0x70CE30

// ==================== STRUCTS ====================
struct CVector { float x, y, z; };

struct stPlayerInfo {
    uint32_t uiVehicleID;   // 0x00
    uint32_t uiPlayerID;    // 0x04
    uint32_t uiScore;       // 0x08
    float    fHealth;       // 0x0C
    float    fArmour;       // 0x10
    uint32_t uiWeapon;      // 0x14
    uint32_t uiState;       // 0x18
    uint32_t uiPing;        // 0x1C
    char     szName[24];    // 0x20
    uint32_t uiCustomData;  // 0x38
    // ped pointer around 0x3C
    uintptr_t pPed;         // 0x3C guess
};

typedef bool(__cdecl* CalcScreenCoors_t)(CVector*, CVector*, float*, float*, bool);

// ==================== CONFIG ====================
struct Config {
    bool  aimbot = true;
    bool  silent = false;
    bool  visible_only = true;
    float smooth = 12.0f;
    float fov = 22.0f;
    float max_dist = 80.0f;
    int   bone = 1;

    bool  esp = false;
    bool  esp_box = true;
    bool  esp_name = true;
    bool  esp_hp = true;
    bool  esp_dist = false;
    bool  esp_line = false;
    float esp_max_dist = 150.0f;

    bool  no_recoil = true;
    bool  no_spread = true;
    bool  no_flash = false;
    bool  full_bright = false;
    bool  anti_afk = false;

    bool  show_menu = true;
} g_Cfg;

// ==================== GLOBALS ====================
HMODULE  g_hModule = nullptr;
HWND     g_hWnd = nullptr;
bool     g_Init = false;
int      g_ScreenW = 0, g_ScreenH = 0;
int      g_LocalID = -1;
int      g_TargetID = -1;
bool     g_F9Pressed = false;

typedef HRESULT(WINAPI* EndScene_t)(IDirect3DDevice9*);
EndScene_t oEndScene = nullptr;
CalcScreenCoors_t pCalcScreen = (CalcScreenCoors_t)GTA_CALC_SCREEN;

// ==================== CONFIG FILE ====================
static char g_IniPath[MAX_PATH] = {0};

static void InitIniPath() {
    GetModuleFileNameA(g_hModule, g_IniPath, MAX_PATH);
    char* dot = strrchr(g_IniPath, '.');
    if (dot) strcpy_s(dot, 5, ".ini");
}

static void SaveConfig() {
    #define W_BOOL(k) WritePrivateProfileStringA("config", #k, g_Cfg.k ? "1" : "0", g_IniPath)
    #define W_INT(k)  { char b[16]; sprintf_s(b, "%d", g_Cfg.k); WritePrivateProfileStringA("config", #k, b, g_IniPath); }
    #define W_FLT(k)  { char b[32]; sprintf_s(b, "%.2f", g_Cfg.k); WritePrivateProfileStringA("config", #k, b, g_IniPath); }

    W_BOOL(aimbot); W_BOOL(silent); W_BOOL(visible_only);
    W_FLT(smooth); W_FLT(fov); W_FLT(max_dist); W_INT(bone);

    W_BOOL(esp); W_BOOL(esp_box); W_BOOL(esp_name); W_BOOL(esp_hp);
    W_BOOL(esp_dist); W_BOOL(esp_line); W_FLT(esp_max_dist);

    W_BOOL(no_recoil); W_BOOL(no_spread); W_BOOL(no_flash); W_BOOL(full_bright);
    W_BOOL(anti_afk);
}

static void LoadConfig() {
    #define R_BOOL(k) g_Cfg.k = GetPrivateProfileIntA("config", #k, g_Cfg.k ? 1 : 0, g_IniPath) != 0
    #define R_INT(k)  g_Cfg.k = GetPrivateProfileIntA("config", #k, g_Cfg.k, g_IniPath)
    #define R_FLT(k)  { char b[32]; GetPrivateProfileStringA("config", #k, "", b, 32, g_IniPath); if (b[0]) g_Cfg.k = (float)atof(b); }

    R_BOOL(aimbot); R_BOOL(silent); R_BOOL(visible_only);
    R_FLT(smooth); R_FLT(fov); R_FLT(max_dist); R_INT(bone);

    R_BOOL(esp); R_BOOL(esp_box); R_BOOL(esp_name); R_BOOL(esp_hp);
    R_BOOL(esp_dist); R_BOOL(esp_line); R_FLT(esp_max_dist);

    R_BOOL(no_recoil); R_BOOL(no_spread); R_BOOL(no_flash); R_BOOL(full_bright);
    R_BOOL(anti_afk);
}

// ==================== HELPERS ====================
static int GetLocalID() {
    uintptr_t info = *(uintptr_t*)SAMP_INFO;
    if (!info) return -1;
    return *(int*)(info + 0x08);
}

static bool IsConnected(int id) {
    uintptr_t pool = *(uintptr_t*)SAMP_PLAYER_POOL;
    if (!pool) return false;
    uintptr_t p = *(uintptr_t*)(pool + 4 + (id * 4));
    return p != 0;
}

static stPlayerInfo* GetPlayer(int id) {
    uintptr_t pool = *(uintptr_t*)SAMP_PLAYER_POOL;
    if (!pool) return nullptr;
    uintptr_t p = *(uintptr_t*)(pool + 4 + (id * 4));
    if (!p) return nullptr;
    return (stPlayerInfo*)p;
}

static CVector GetPlayerPos(int id) {
    if (id == g_LocalID) {
        uintptr_t p = *(uintptr_t*)GTA_PLAYER_PTR;
        if (p) return *(CVector*)(p + 0x14);
        return {0,0,0};
    }
    stPlayerInfo* info = GetPlayer(id);
    if (!info) return {0,0,0};
    uintptr_t ped = info->pPed;
    if (!ped) return {0,0,0};
    return *(CVector*)(ped + 0x14);
}

static float Dist3D(CVector a, CVector b) {
    float dx = b.x-a.x, dy = b.y-a.y, dz = b.z-a.z;
    return sqrtf(dx*dx+dy*dy+dz*dz);
}

static bool IsInFOV(CVector local, CVector target, float fov) {
    float camX = *(float*)GTA_CAMERA_X;
    float dx = target.x - local.x;
    float dy = target.y - local.y;
    float angleToTarget = atan2f(dy, dx) * 57.2958f;
    float diff = angleToTarget - camX;
    while (diff > 180.0f) diff -= 360.0f;
    while (diff < -180.0f) diff += 360.0f;
    return fabsf(diff) <= fov / 2.0f;
}

static bool WorldToScreen(CVector world, CVector& screen) {
    float w = 0, h = 0;
    return pCalcScreen(&world, &screen, &w, &h, true);
}

// ==================== CHEATS ====================
static void ApplyMemory() {
    if (g_Cfg.no_recoil) *(float*)GTA_RECOIL = 0.0f;
    if (g_Cfg.no_spread) *(float*)GTA_SPREAD = 0.0f;
    if (g_Cfg.no_flash)  *(float*)GTA_FLASH  = 0.0f;
}

static int FindBestTarget() {
    if (g_LocalID < 0) return -1;
    CVector local = GetPlayerPos(g_LocalID);
    int best = -1;
    float bestDist = g_Cfg.max_dist;

    for (int i = 0; i < 1000; i++) {
        if (i == g_LocalID) continue;
        if (!IsConnected(i)) continue;
        stPlayerInfo* p = GetPlayer(i);
        if (!p || p->fHealth <= 0.0f) continue;

        CVector t = GetPlayerPos(i);
        float d = Dist3D(local, t);
        if (d >= bestDist) continue;
        if (!IsInFOV(local, t, g_Cfg.fov)) continue;

        bestDist = d;
        best = i;
    }
    return best;
}

static void ApplyAimbot() {
    if (!g_Cfg.aimbot) return;
    if (!(GetAsyncKeyState(VK_RBUTTON) & 0x8000)) return;
    if (g_TargetID < 0) return;

    CVector local = GetPlayerPos(g_LocalID);
    CVector t = GetPlayerPos(g_TargetID);
    if (t.x == 0 && t.y == 0) return;

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

    float* camX = (float*)GTA_CAMERA_X;
    float* camZ = (float*)GTA_CAMERA_Z;

    float diffX = targetX - *camX;
    while (diffX > 180.0f) diffX -= 360.0f;
    while (diffX < -180.0f) diffX += 360.0f;

    *camX += diffX / g_Cfg.smooth;
    *camZ += (targetZ - *camZ) / g_Cfg.smooth;
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

// ==================== MENU ====================
static void DrawMenu() {
    if (!g_Cfg.show_menu) return;

    ImGui::SetNextWindowSize(ImVec2(500, 520), ImGuiCond_FirstUseEver);
    ImGui::Begin("MNZ Panel v2.0", &g_Cfg.show_menu, ImGuiWindowFlags_NoCollapse);

    ImGui::TextColored(ImVec4(0.7f, 0.5f, 1.0f, 1.0f), "MNZ Panel v2.0");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "| F9 = toggle");
    ImGui::Separator();

    if (ImGui::BeginTabBar("##tabs")) {

        if (ImGui::BeginTabItem("Aimbot")) {
            ImGui::Spacing();
            ImGui::Checkbox("Enable Aimbot", &g_Cfg.aimbot);
            ImGui::Checkbox("Silent Aim (ALT)", &g_Cfg.silent);
            ImGui::Checkbox("Visible Only", &g_Cfg.visible_only);
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::Text("Smooth (higher = smoother)");
            ImGui::SliderFloat("##smooth", &g_Cfg.smooth, 1.0f, 20.0f, "%.1f");
            ImGui::Text("FOV");
            ImGui::SliderFloat("##fov", &g_Cfg.fov, 5.0f, 180.0f, "%.0f");
            ImGui::Text("Max Distance");
            ImGui::SliderFloat("##maxdist", &g_Cfg.max_dist, 10.0f, 300.0f, "%.0f m");
            const char* bones[] = { "Head", "Chest", "Pelvis" };
            ImGui::Text("Target Bone");
            ImGui::Combo("##bone", &g_Cfg.bone, bones, 3);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("ESP")) {
            ImGui::Spacing();
            ImGui::Checkbox("Enable ESP", &g_Cfg.esp);
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::Checkbox("Box", &g_Cfg.esp_box);
            ImGui::Checkbox("Name", &g_Cfg.esp_name);
            ImGui::Checkbox("Health", &g_Cfg.esp_hp);
            ImGui::Checkbox("Distance", &g_Cfg.esp_dist);
            ImGui::Checkbox("Snapline", &g_Cfg.esp_line);
            ImGui::Spacing();
            ImGui::Text("Max Distance");
            ImGui::SliderFloat("##espdist", &g_Cfg.esp_max_dist, 50.0f, 500.0f, "%.0f m");
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Visual")) {
            ImGui::Spacing();
            ImGui::Checkbox("No Recoil", &g_Cfg.no_recoil);
            ImGui::Checkbox("No Spread", &g_Cfg.no_spread);
            ImGui::Checkbox("No Flash", &g_Cfg.no_flash);
            ImGui::Checkbox("Full Bright", &g_Cfg.full_bright);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Misc")) {
            ImGui::Spacing();
            ImGui::Checkbox("Anti AFK", &g_Cfg.anti_afk);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Config")) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.7f, 0.5f, 1.0f, 1.0f), "Settings saved to MNZPanel.ini");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            if (ImGui::Button("Save Config", ImVec2(150, 30))) SaveConfig();
            ImGui::SameLine();
            if (ImGui::Button("Load Config", ImVec2(150, 30))) LoadConfig();
            ImGui::SameLine();
            if (ImGui::Button("Reset", ImVec2(100, 30))) g_Cfg = Config();

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "War Preset (safe/legit)");
            if (ImGui::Button("Apply War Preset", ImVec2(200, 32))) {
                g_Cfg.aimbot = true;
                g_Cfg.silent = true;
                g_Cfg.visible_only = true;
                g_Cfg.smooth = 14.0f;
                g_Cfg.fov = 18.0f;
                g_Cfg.max_dist = 60.0f;
                g_Cfg.bone = 1;
                g_Cfg.esp = false;
                g_Cfg.no_recoil = true;
                g_Cfg.no_spread = true;
                g_Cfg.no_flash = false;
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Info")) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.7f, 0.5f, 1.0f, 1.0f), "MNZ Panel v2.0");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::Text("F9      - Toggle menu");
            ImGui::Text("RMB     - Aimbot (hold)");
            ImGui::Text("ALT     - Silent aim (hold)");
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

// ==================== ESP DRAW ====================
static void DrawESP() {
    if (!g_Cfg.esp) return;
    if (g_LocalID < 0) return;

    CVector local = GetPlayerPos(g_LocalID);
    ImDrawList* draw = ImGui::GetBackgroundDrawList();

    for (int i = 0; i < 1000; i++) {
        if (i == g_LocalID) continue;
        if (!IsConnected(i)) continue;
        stPlayerInfo* p = GetPlayer(i);
        if (!p || p->fHealth <= 0.0f) continue;

        CVector pos = GetPlayerPos(i);
        if (pos.x == 0 && pos.y == 0 && pos.z == 0) continue;

        float d = Dist3D(local, pos);
        if (d > g_Cfg.esp_max_dist) continue;

        CVector head = { pos.x, pos.y, pos.z + 0.9f };
        CVector feet = { pos.x, pos.y, pos.z - 0.9f };
        CVector sHead, sFeet;

        if (!WorldToScreen(head, sHead)) continue;
        if (!WorldToScreen(feet, sFeet)) continue;

        float h = sFeet.y - sHead.y;
        float w = h / 3.0f;

        ImU32 colBox = IM_COL32(180, 100, 255, 220);
        ImU32 colText = IM_COL32(255, 255, 255, 255);

        if (g_Cfg.esp_box) {
            draw->AddRect(
                ImVec2(sHead.x - w/2, sHead.y),
                ImVec2(sHead.x + w/2, sFeet.y),
                colBox, 0.0f, 0, 1.5f
            );
        }

        if (g_Cfg.esp_name) {
            const char* name = p->szName;
            ImVec2 ts = ImGui::CalcTextSize(name);
            draw->AddText(ImVec2(sHead.x - ts.x/2, sHead.y - 16), colText, name);
        }

        if (g_Cfg.esp_hp) {
            char buf[32];
            sprintf_s(buf, "HP: %.0f", p->fHealth);
            draw->AddText(ImVec2(sHead.x + w/2 + 4, sHead.y), IM_COL32(0,255,100,255), buf);
        }

        if (g_Cfg.esp_dist) {
            char buf[32];
            sprintf_s(buf, "%.0fm", d);
            ImVec2 ts = ImGui::CalcTextSize(buf);
            draw->AddText(ImVec2(sHead.x - ts.x/2, sFeet.y + 2), IM_COL32(255,220,100,255), buf);
        }

        if (g_Cfg.esp_line) {
            draw->AddLine(
                ImVec2((float)g_ScreenW/2, (float)g_ScreenH),
                ImVec2(sFeet.x, sFeet.y),
                IM_COL32(180,100,255,150), 1.0f
            );
        }
    }
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
        io.LogFilename = nullptr;

        ImGui_ImplWin32_Init(g_hWnd);
        ImGui_ImplDX9_Init(pDevice);

        ApplyTheme();
        InitIniPath();
        LoadConfig();

        g_Init = true;
    }

    bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (f9 && !g_F9Pressed) g_Cfg.show_menu = !g_Cfg.show_menu;
    g_F9Pressed = f9;

    g_LocalID = GetLocalID();

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (g_LocalID >= 0) {
        g_TargetID = FindBestTarget();
        ApplyAimbot();
    }

    DrawESP();
    DrawMenu();
    ApplyMemory();

    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

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
