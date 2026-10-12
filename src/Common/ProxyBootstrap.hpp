#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string_view>

namespace OST::Proxy {

/**
 * @brief Defensive system DLL loader.
 * Loads original DLL directly from the Windows System32 directory using LOAD_WITH_ALTERED_SEARCH_PATH,
 * completely immune to local DLL sideloading and recursive self-loading.
 */
class SystemDllLoader {
public:
    [[nodiscard]] static HMODULE Load(std::wstring_view dllName) noexcept {
        if (dllName.empty()) {
            return nullptr;
        }

        wchar_t fullPath[MAX_PATH];
        const UINT len = ::GetSystemDirectoryW(fullPath, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) {
            return nullptr;
        }

        size_t pos = static_cast<size_t>(len);
        if (fullPath[pos - 1] != L'\\' && fullPath[pos - 1] != L'/') {
            fullPath[pos++] = L'\\';
        }

        if (dllName.size() >= MAX_PATH - pos) {
            return nullptr;
        }

        std::char_traits<wchar_t>::copy(fullPath + pos, dllName.data(), dllName.size());
        fullPath[pos + dllName.size()] = L'\0';

        return ::LoadLibraryExW(fullPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }
};

/**
 * @brief Unified proxy DLL bootstrap and process filtering anchor.
 * Manages thread library call disabling, host process inspection, and guarded payload injection.
 */
/**
 * @brief Static ASCII ecosystem signature anchored into the binary image.
 * Third-party ecosystem tools (such as CloudRedirect's ThirdPartyDetector) scan proxy DLLs
 * (dwmapi.dll, xinput1_4.dll) for exact ASCII string markers ("OpenSteamTool") to recognize
 * compatible loaders and authorize automated deployment.
 */
inline constexpr char kEcosystemSignature[] = "OpenSteamTool";

class Bootstrap {
public:
    [[nodiscard]] static constexpr wchar_t ToLowerAscii(wchar_t ch) noexcept {
        return (ch >= L'A' && ch <= L'Z') ? static_cast<wchar_t>(ch + (L'a' - L'A')) : ch;
    }

    [[nodiscard]] static constexpr bool EqualIgnoreCase(std::wstring_view a, std::wstring_view b) noexcept {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (ToLowerAscii(a[i]) != ToLowerAscii(b[i])) {
                return false;
            }
        }
        return true;
    }

    static BOOL OnAttach(
        HMODULE hModule,
        std::wstring_view targetHost = L"steam.exe",
        std::wstring_view payloadDll = L"OpenSteamTool.dll") noexcept {

        // Anchor the ASCII ecosystem signature so third-party detectors (e.g. CloudRedirect)
        // find the "OpenSteamTool" marker in the compiled binary image even under /O2 and /OPT:REF.
        static volatile const char* volatile s_ecosystemSignature = kEcosystemSignature;
        (void)s_ecosystemSignature;

        ::DisableThreadLibraryCalls(hModule);

        if (targetHost.empty() || payloadDll.empty()) {
            return TRUE;
        }

        constexpr DWORD kPathBufCapacity = 1024;
        wchar_t pathBuf[kPathBufCapacity];
        const DWORD exeLen = ::GetModuleFileNameW(nullptr, pathBuf, kPathBufCapacity);
        if (exeLen == 0 || exeLen >= kPathBufCapacity) {
            return TRUE;
        }

        const std::wstring_view fullExePath(pathBuf, exeLen);
        const auto lastSlash = fullExePath.find_last_of(L"\\/");
        const std::wstring_view exeName = (lastSlash != std::wstring_view::npos)
            ? fullExePath.substr(lastSlash + 1)
            : fullExePath;

        // If not target host, allow the proxy DLL to load peacefully without injecting
        if (!EqualIgnoreCase(exeName, targetHost)) {
            return TRUE;
        }

        // Host is verified: prefer loading payload from the proxy DLL's own directory
        // to avoid CWD drift or DLL search-order hijacking.
        HMODULE hPayload = nullptr;
        const DWORD modLen = ::GetModuleFileNameW(hModule, pathBuf, kPathBufCapacity);
        if (modLen > 0 && modLen < kPathBufCapacity) {
            const std::wstring_view fullModPath(pathBuf, modLen);
            const auto modSlash = fullModPath.find_last_of(L"\\/");
            if (modSlash != std::wstring_view::npos) {
                const size_t dirLen = modSlash + 1;
                if (payloadDll.size() < kPathBufCapacity - dirLen) {
                    std::char_traits<wchar_t>::copy(pathBuf + dirLen, payloadDll.data(), payloadDll.size());
                    pathBuf[dirLen + payloadDll.size()] = L'\0';
                    hPayload = ::LoadLibraryW(pathBuf);
                }
            }
        }

        if (!hPayload && payloadDll.size() < kPathBufCapacity) {
            std::char_traits<wchar_t>::copy(pathBuf, payloadDll.data(), payloadDll.size());
            pathBuf[payloadDll.size()] = L'\0';
            ::LoadLibraryW(pathBuf);
        }

        return TRUE;
    }
};

} // namespace OST::Proxy
