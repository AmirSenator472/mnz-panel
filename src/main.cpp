#include <cstdint>
#include <windows.h>
#include <d3d9.h>
#include <cmath>

#pragma comment(lib, "d3d9.lib")

#define SAMP_INFO          0x21A0F8
#define SAMP_PLAYER_POOL   0x21A100
#define GTA_PLAYER_PTR     0xB6F5F0
#define GTA_RECOIL         0x732E14
#define GTA_SPREAD         0x732E18
#define GTA_FLASH          0x732E1C

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

HMODULE    g_hModule = nullptr;
bool       g_Initialized = false;
int        g_LocalID = -1;

typedef HRESULT(WINAPI* EndScene_t)(IDirect3DDevice9*);
EndScene_t oEndScene = nullptr;

static CVector GetLocalPos() {
    uintptr_t pPlayer = *(uintptr_t*)GTA_PLAYER_PTR;
    CVector v = { 0.0f, 0.0f, 0.0f };
    if (!pPlayer) return v;
    return *(CVector*)(pPlayer + 0x14);
}

static bool IsPlayerConnected(int id) {
    uintptr_t pool = *(uintptr_t*)SAMP_PLAYER_POOL;
    if (!pool) return false;
    uintptr_t player = *(uintptr_t*)(pool + 4 + (id * 4));
    return player != 0;
}

static stPlayerInfo* GetPlayer(int id) {
    uintptr_t pool = *(uintptr_t*)SAMP_PLAYER_POOL;
    if (!pool) return nullptr;
    uintptr_t player = *(uintptr_t*)(pool + 4 + (id * 4));
    if (!player) return nullptr;
    return (stPlayerInfo*)player;
}

static float Dist3D(CVector a, CVector b) {
    float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static int GetLocalID() {
    uintptr_t sampInfo = *(uintptr_t*)SAMP_INFO;
    if (!sampInfo) return -1;
    return *(int*)(sampInfo + 0x08);
}

static HRESULT WINAPI hkEndScene(IDirect3DDevice9* pDevice) {
    if (!g_Initialized) {
        g_Initialized = true;
    }

    *(float*)GTA_RECOIL = 0.0f;
    *(float*)GTA_SPREAD = 0.0f;

    g_LocalID = GetLocalID();

    return oEndScene(pDevice);
}

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

        DWORD oldProtect;
        VirtualProtect(&vtable[42], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
        vtable[42] = (void*)hkEndScene;
        VirtualProtect(&vtable[42], sizeof(void*), oldProtect, &oldProtect);

        pDevice->Release();
    }
    pD3D->Release();
}

static DWORD WINAPI MainThread(LPVOID) {
    while (!GetModuleHandleA("samp.dll")) Sleep(500);
    Sleep(3000);
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
