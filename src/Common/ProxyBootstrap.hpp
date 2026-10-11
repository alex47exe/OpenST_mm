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

        ::DisableThreadLibraryCalls(hModule);

        wchar_t exePath[MAX_PATH];
        const DWORD len = ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            const std::wstring_view fullPath(exePath, len);
            const auto lastSlash = fullPath.find_last_of(L"\\/");
            const std::wstring_view exeName = (lastSlash != std::wstring_view::npos)
                ? fullPath.substr(lastSlash + 1)
                : fullPath;

            // If not target host, allow the proxy DLL to load peacefully without injecting
            if (!EqualIgnoreCase(exeName, targetHost)) {
                return TRUE;
            }
        }

        // Host is verified: inject payload DLL
        // Windows loader guarantees that LoadLibraryW on the same module in the same process
        // runs DllMain only once, ensuring thread-safe idempotent injection.
        if (payloadDll.empty() || payloadDll.size() >= MAX_PATH) {
            return FALSE;
        }
        std::char_traits<wchar_t>::copy(exePath, payloadDll.data(), payloadDll.size());
        exePath[payloadDll.size()] = L'\0';
        return ::LoadLibraryW(exePath) != nullptr ? TRUE : FALSE;
    }
};

} // namespace OST::Proxy
