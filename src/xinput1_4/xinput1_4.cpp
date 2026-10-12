// xinput1_4.dll HiJack Project - Dynamic System Wrapper using ProxyBootstrap
#include <windows.h>
#include <atomic>
#include <mutex>
#include "../Common/ProxyBootstrap.hpp"

// ─── 1. Real XInput Function Pointers ───────────────────────────
static std::once_flag g_xinputInitOnce;
static std::atomic<bool> g_xinputReady{false};

using XInputGetState_t = DWORD(WINAPI*)(DWORD, void*);
using XInputSetState_t = DWORD(WINAPI*)(DWORD, void*);
using XInputGetCapabilities_t = DWORD(WINAPI*)(DWORD, DWORD, void*);
using XInputEnable_t = void(WINAPI*)(BOOL);
using XInputGetAudioDeviceIds_t = DWORD(WINAPI*)(DWORD, LPWSTR, UINT*, LPWSTR, UINT*);
using XInputGetBatteryInformation_t = DWORD(WINAPI*)(DWORD, BYTE, void*);
using XInputGetKeystroke_t = DWORD(WINAPI*)(DWORD, DWORD, void*);

static XInputGetState_t o_XInputGetState = nullptr;
static XInputSetState_t o_XInputSetState = nullptr;
static XInputGetCapabilities_t o_XInputGetCapabilities = nullptr;
static XInputEnable_t o_XInputEnable = nullptr;
static XInputGetAudioDeviceIds_t o_XInputGetAudioDeviceIds = nullptr;
static XInputGetBatteryInformation_t o_XInputGetBatteryInformation = nullptr;
static XInputGetKeystroke_t o_XInputGetKeystroke = nullptr;

// Undocumented Ordinals (Required for Steam Big Picture / Guide Button)
static FARPROC o_100 = nullptr;
static FARPROC o_101 = nullptr;
static FARPROC o_102 = nullptr;
static FARPROC o_103 = nullptr;
static FARPROC o_104 = nullptr;
static FARPROC o_108 = nullptr;

// ─── 2. Core Initialization (Binding to real System32 DLL via SystemDllLoader) ───
static void LoadRealXInput() noexcept {
    std::call_once(g_xinputInitOnce, []() noexcept {
        if (const HMODULE hRealXInput = OST::Proxy::SystemDllLoader::Load(L"xinput1_4.dll")) {
            o_XInputGetState = reinterpret_cast<XInputGetState_t>(GetProcAddress(hRealXInput, "XInputGetState"));
            o_XInputSetState = reinterpret_cast<XInputSetState_t>(GetProcAddress(hRealXInput, "XInputSetState"));
            o_XInputGetCapabilities = reinterpret_cast<XInputGetCapabilities_t>(GetProcAddress(hRealXInput, "XInputGetCapabilities"));
            o_XInputEnable = reinterpret_cast<XInputEnable_t>(GetProcAddress(hRealXInput, "XInputEnable"));
            o_XInputGetAudioDeviceIds = reinterpret_cast<XInputGetAudioDeviceIds_t>(GetProcAddress(hRealXInput, "XInputGetAudioDeviceIds"));
            o_XInputGetBatteryInformation = reinterpret_cast<XInputGetBatteryInformation_t>(GetProcAddress(hRealXInput, "XInputGetBatteryInformation"));
            o_XInputGetKeystroke = reinterpret_cast<XInputGetKeystroke_t>(GetProcAddress(hRealXInput, "XInputGetKeystroke"));

            o_100 = GetProcAddress(hRealXInput, reinterpret_cast<LPCSTR>(100));
            o_101 = GetProcAddress(hRealXInput, reinterpret_cast<LPCSTR>(101));
            o_102 = GetProcAddress(hRealXInput, reinterpret_cast<LPCSTR>(102));
            o_103 = GetProcAddress(hRealXInput, reinterpret_cast<LPCSTR>(103));
            o_104 = GetProcAddress(hRealXInput, reinterpret_cast<LPCSTR>(104));
            o_108 = GetProcAddress(hRealXInput, reinterpret_cast<LPCSTR>(108));
        }
        g_xinputReady.store(true, std::memory_order_release);
    });
}

inline void EnsureRealXInputLoaded() noexcept {
    if (!g_xinputReady.load(std::memory_order_acquire)) {
        LoadRealXInput();
    }
}

// ─── 3. Native Exports (Safely passing calls to the system DLL) ──
extern "C" {
    DWORD WINAPI XInputGetState(DWORD dwUserIndex, void* pState) {
        EnsureRealXInputLoaded();
        return o_XInputGetState ? o_XInputGetState(dwUserIndex, pState) : ERROR_DEVICE_NOT_CONNECTED;
    }
    DWORD WINAPI XInputSetState(DWORD dwUserIndex, void* pVibration) {
        EnsureRealXInputLoaded();
        return o_XInputSetState ? o_XInputSetState(dwUserIndex, pVibration) : ERROR_DEVICE_NOT_CONNECTED;
    }
    DWORD WINAPI XInputGetCapabilities(DWORD dwUserIndex, DWORD dwFlags, void* pCapabilities) {
        EnsureRealXInputLoaded();
        return o_XInputGetCapabilities ? o_XInputGetCapabilities(dwUserIndex, dwFlags, pCapabilities) : ERROR_DEVICE_NOT_CONNECTED;
    }
    void WINAPI XInputEnable(BOOL enable) {
        EnsureRealXInputLoaded();
        if (o_XInputEnable) o_XInputEnable(enable);
    }
    DWORD WINAPI XInputGetAudioDeviceIds(DWORD dwUserIndex, LPWSTR pRenderDeviceId, UINT* pRenderCount, LPWSTR pCaptureDeviceId, UINT* pCaptureCount) {
        EnsureRealXInputLoaded();
        return o_XInputGetAudioDeviceIds ? o_XInputGetAudioDeviceIds(dwUserIndex, pRenderDeviceId, pRenderCount, pCaptureDeviceId, pCaptureCount) : ERROR_DEVICE_NOT_CONNECTED;
    }
    DWORD WINAPI XInputGetBatteryInformation(DWORD dwUserIndex, BYTE devType, void* pBatteryInformation) {
        EnsureRealXInputLoaded();
        return o_XInputGetBatteryInformation ? o_XInputGetBatteryInformation(dwUserIndex, devType, pBatteryInformation) : ERROR_DEVICE_NOT_CONNECTED;
    }
    DWORD WINAPI XInputGetKeystroke(DWORD dwUserIndex, DWORD dwReserved, void* pKeystroke) {
        EnsureRealXInputLoaded();
        return o_XInputGetKeystroke ? o_XInputGetKeystroke(dwUserIndex, dwReserved, pKeystroke) : ERROR_DEVICE_NOT_CONNECTED;
    }

    // Undocumented Ordinals
    DWORD WINAPI XInputOrdinal100(DWORD a1, void* a2) {
        EnsureRealXInputLoaded();
        return o_100 ? ((DWORD(WINAPI*)(DWORD, void*))o_100)(a1, a2) : ERROR_DEVICE_NOT_CONNECTED;
    }
    DWORD WINAPI XInputOrdinal101(DWORD a1, DWORD a2, void* a3) {
        EnsureRealXInputLoaded();
        return o_101 ? ((DWORD(WINAPI*)(DWORD, DWORD, void*))o_101)(a1, a2, a3) : ERROR_DEVICE_NOT_CONNECTED;
    }
    DWORD WINAPI XInputOrdinal102(DWORD a1) {
        EnsureRealXInputLoaded();
        return o_102 ? ((DWORD(WINAPI*)(DWORD))o_102)(a1) : ERROR_DEVICE_NOT_CONNECTED;
    }
    DWORD WINAPI XInputOrdinal103(DWORD a1) {
        EnsureRealXInputLoaded();
        return o_103 ? ((DWORD(WINAPI*)(DWORD))o_103)(a1) : ERROR_DEVICE_NOT_CONNECTED;
    }
    DWORD WINAPI XInputOrdinal104(DWORD a1, void* a2) {
        EnsureRealXInputLoaded();
        return o_104 ? ((DWORD(WINAPI*)(DWORD, void*))o_104)(a1, a2) : ERROR_DEVICE_NOT_CONNECTED;
    }
    DWORD WINAPI XInputOrdinal108(DWORD a1, void* a2, void* a3, void* a4, void* a5) {
        EnsureRealXInputLoaded();
        return o_108 ? ((DWORD(WINAPI*)(DWORD, void*, void*, void*, void*))o_108)(a1, a2, a3, a4, a5) : ERROR_DEVICE_NOT_CONNECTED;
    }
}

// ─── 4. DllMain with Unified ProxyBootstrap ───────────────────────
BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, PVOID /*pvReserved*/) {
    if (dwReason == DLL_PROCESS_ATTACH) {
        return OST::Proxy::Bootstrap::OnAttach(hModule);
    }
    return TRUE;
}