#include "dllmain.h"
#include "Hook/HookManager.h"
#include "Hook/Hooks_Decryption.h"
#include "Hook/Hooks_Misc.h"
#include "Hook/Hooks_Package.h"
#include "Hook/Hooks_SteamUI.h"
#include "Utils/Tickets/AppTicket.h"
#include "Utils/Config/ConfigFileWatcher.h"
#include "Utils/Config/LuaFileWatcher.h"
#include "Utils/CloudRedirect/CloudRedirectHost.h"
#include "Utils/SteamMetadata/IPCLoader.h"
#include "Utils/SteamMetadata/ManifestClient.h"
#include "Utils/SteamMetadata/PatternLoader.h"
#include "Utils/SteamMetadata/StatsClient.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"
#include "OSTPlatform/include/Thread.h"

#include <cctype>
#include <cstdio>
#include <windows.h>

template <typename... Args>
static inline bool FormatPath(char (&dst)[kRuntimePathCapacity], const char* fmt, Args... args) noexcept {
    const int n = std::snprintf(dst, kRuntimePathCapacity, fmt, args...);
    if (n <= 0 || static_cast<size_t>(n) >= kRuntimePathCapacity) {
        dst[0] = '\0';
        return false;
    }
    return true;
}

// Prepare key runtime paths.
// Portable: Steam components (steamclient64.dll, steamui.dll, etc.) are located
// in Steam's real installation directory, while configuration and Lua scripts can be
// loaded from the portable DLL directory or fallback to Steam's installation directory.
static bool InitializeSteamComponents(OSTPlatform::DynamicLibrary::ModuleHandle selfModule)
{
    using OSTPlatform::Encoding::PathFromUtf8;
    using OSTPlatform::Encoding::PathToUtf8;
    using OSTPlatform::Encoding::Utf8ToWide;

    // 1. Locate Steam's actual install directory (where steam.exe and steamclient64.dll reside).
    // Injected into steam.exe: GetModuleDirectory(nullptr) returns the directory of steam.exe.
    auto steamExeDir = OSTPlatform::DynamicLibrary::GetModuleDirectory(nullptr);
    std::string steamPath = PathToUtf8(steamExeDir);
    if (steamPath.empty()) {
        steamPath = OSTPlatform::DynamicLibrary::GetCurrentDirectoryPath();
    }
    while (steamPath.size() > 1 && (steamPath.back() == '\\' || steamPath.back() == '/') &&
           !(steamPath.size() == 3 && steamPath[1] == ':')) {
        steamPath.pop_back();
    }
    if (steamPath.empty()) {
        return false;
    }
    if (!FormatPath(SteamInstallPath, "%s", steamPath.c_str()) ||
        !FormatPath(SteamclientPath,  "%s\\steamclient64.dll", SteamInstallPath) ||
        !FormatPath(SteamUIPath,      "%s\\steamui.dll",       SteamInstallPath)) {
        LOG_ERROR("Steam install path exceeds runtime buffer capacity ({})", kRuntimePathCapacity);
        return false;
    }

    // 2. Locate OpenSteamTool DLL directory (portable mode support).
    auto dllDir = OSTPlatform::DynamicLibrary::GetModuleDirectory(selfModule);
    std::string dllPath = PathToUtf8(dllDir);
    if (dllPath.empty()) {
        dllPath = steamPath;
    }
    while (dllPath.size() > 1 && (dllPath.back() == '\\' || dllPath.back() == '/') &&
           !(dllPath.size() == 3 && dllPath[1] == ':')) {
        dllPath.pop_back();
    }
    if (!FormatPath(DllDir, "%s", dllPath.c_str())) {
        LOG_ERROR("DLL directory path exceeds runtime buffer capacity ({})", kRuntimePathCapacity);
        return false;
    }

    // Evaluate portable mode once after SteamInstallPath and DllDir are set.
    auto dllFsPath   = PathFromUtf8(DllDir).lexically_normal();
    auto steamFsPath = PathFromUtf8(SteamInstallPath).lexically_normal();
    if (!dllFsPath.has_filename() && dllFsPath != dllFsPath.root_path()) {
        dllFsPath = dllFsPath.parent_path();
    }
    if (!steamFsPath.has_filename() && steamFsPath != steamFsPath.root_path()) {
        steamFsPath = steamFsPath.parent_path();
    }
    std::error_code ecEquiv;
    const bool sameDir = std::filesystem::equivalent(dllFsPath, steamFsPath, ecEquiv);
    g_IsPortableMode.store(!sameDir && (_wcsicmp(dllFsPath.c_str(), steamFsPath.c_str()) != 0),
                           std::memory_order_release);

    if (!FormatPath(DiversionPath, "%s\\bin\\diversion64.dll", GetStorageDirectory())) {
        LOG_ERROR("Diversion path exceeds runtime buffer capacity ({})", kRuntimePathCapacity);
        return false;
    }

    // 3. Resolve config and lua directory:
    // Check DllDir first (portable folder), fallback to SteamInstallPath.
    std::filesystem::path tomlFs = dllFsPath / "opensteamtool.toml";
    std::string tomlPath = PathToUtf8(tomlFs);
    std::error_code ecToml;
    if (IsPortableMode() && (!std::filesystem::is_regular_file(tomlFs, ecToml) || ecToml)) {
        std::filesystem::path steamTomlFs = steamFsPath / "opensteamtool.toml";
        std::error_code ecSteamToml;
        if (std::filesystem::is_regular_file(steamTomlFs, ecSteamToml) && !ecSteamToml) {
            tomlPath = PathToUtf8(steamTomlFs);
        }
    }

    const auto storageBase = PathFromUtf8(GetStorageDirectory());
    const auto luaFs = storageBase / "config" / "lua";
    std::error_code ec;
    std::filesystem::create_directories(luaFs, ec);
    if (ec) {
        LOG_WARN("Failed to create default Lua directory (err={})", ec.value());
    }
    const std::string luaPath = PathToUtf8(luaFs);
    if (!FormatPath(ConfigPath, "%s", tomlPath.c_str()) ||
        !FormatPath(LuaDir,     "%s", luaPath.c_str())) {
        LOG_ERROR("Config or Lua directory path exceeds runtime buffer capacity ({})", kRuntimePathCapacity);
        return false;
    }

    // 4. Diversion shadow module cloning & loading:
    // Clone steamclient64.dll into bin\diversion64.dll so all hooks and patches
    // are isolated to the diversion module while original steamclient64.dll stays 100% clean.
    WIN32_FILE_ATTRIBUTE_DATA origAttr{}, divAttr{};
    std::wstring wideSteamclientPath = Utf8ToWide(SteamclientPath);
    std::wstring wideDiversionPath   = Utf8ToWide(DiversionPath);
    const bool origExists = GetFileAttributesExW(wideSteamclientPath.c_str(), GetFileExInfoStandard, &origAttr) != 0 &&
                            !(origAttr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    const bool divExists  = GetFileAttributesExW(wideDiversionPath.c_str(), GetFileExInfoStandard, &divAttr) != 0 &&
                            !(divAttr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);

    bool isUpToDate = false;
    if (origExists && divExists) {
        if ((origAttr.nFileSizeHigh != 0 || origAttr.nFileSizeLow != 0) &&
            origAttr.nFileSizeHigh == divAttr.nFileSizeHigh &&
            origAttr.nFileSizeLow  == divAttr.nFileSizeLow &&
            origAttr.ftLastWriteTime.dwLowDateTime  == divAttr.ftLastWriteTime.dwLowDateTime &&
            origAttr.ftLastWriteTime.dwHighDateTime == divAttr.ftLastWriteTime.dwHighDateTime)
        {
            isUpToDate = true;
        }
    }

    bool copyOk = isUpToDate;
    if (isUpToDate) {
        LOG_DEBUG("Diversion module is already up to date ({}), skipping copy", DiversionPath);
    } else if (origExists) {
        std::filesystem::path diversionFsPath = PathFromUtf8(DiversionPath);
        std::error_code ecDir;
        std::filesystem::create_directories(diversionFsPath.parent_path(), ecDir);
        if (ecDir) {
            LOG_WARN("Failed to create diversion directory (err={})", ecDir.value());
        }
        if (divExists && (divAttr.dwFileAttributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) {
            SetFileAttributesW(wideDiversionPath.c_str(), FILE_ATTRIBUTE_NORMAL);
        }

        // Retry up to 3 times in case the old Steam process is still releasing the file handle
        constexpr int kMaxCopyRetries = 3;
        DWORD gle = ERROR_SUCCESS;
        int actualAttempts = 0;
        for (int attempt = 1; attempt <= kMaxCopyRetries; ++attempt) {
            actualAttempts = attempt;
            if (CopyFileW(wideSteamclientPath.c_str(), wideDiversionPath.c_str(), FALSE)) {
                copyOk = true;
                LOG_INFO("Cloned steamclient64.dll -> {}", DiversionPath);
                break;
            }
            gle = GetLastError();
            if (gle != ERROR_SHARING_VIOLATION && gle != ERROR_ACCESS_DENIED) {
                break;
            }
            if (attempt < kMaxCopyRetries) {
                Sleep(50);
            }
        }

        if (!copyOk) {
            LOG_WARN("CopyFileW to diversion64.dll failed after {} attempt(s) (err={})", actualAttempts, gle);
        }
    }

    if (copyOk) {
        client_hModule = OSTPlatform::DynamicLibrary::Load(PathFromUtf8(DiversionPath));
        if (client_hModule) {
            g_IsDiversionActive.store(true, std::memory_order_release);
            LOG_INFO("Loaded diversion module from {}", DiversionPath);
        } else {
            LOG_WARN("Load diversion module failed (path={}, err={}), falling back to real steamclient64.dll",
                     DiversionPath, OSTPlatform::DynamicLibrary::GetLastErrorCode());
        }
    }

    if (!client_hModule) {
        g_IsDiversionActive.store(false, std::memory_order_release);
        client_hModule = OSTPlatform::DynamicLibrary::Load(PathFromUtf8(SteamclientPath));
        if (!client_hModule) {
            LOG_ERROR("Load steamclient64.dll failed: {} (err={})",
                      SteamclientPath, OSTPlatform::DynamicLibrary::GetLastErrorCode());
            return false;
        }
        LOG_INFO("Loaded fallback steamclient64.dll from {} (Diversion inactive)", SteamclientPath);
    }

    ui_hModule = OSTPlatform::DynamicLibrary::Load(PathFromUtf8(SteamUIPath));
    if (!ui_hModule) {
        LOG_ERROR("Load failed for steamui.dll: {} (err={})",
                  SteamUIPath, OSTPlatform::DynamicLibrary::GetLastErrorCode());
        g_IsDiversionActive.store(false, std::memory_order_release);
        if (client_hModule) {
            OSTPlatform::DynamicLibrary::Unload(client_hModule);
            client_hModule = nullptr;
        }
        return false;
    }
    return true;
}

// All initialisation that touches the filesystem, loads modules, scans
// memory, or installs detours runs here on a worker thread — we MUST NOT do
// any of that from inside DllMain (loader lock).
static uint32_t InitThread(OSTPlatform::DynamicLibrary::ModuleHandle selfModule) noexcept {
    try {
        Log::Init(selfModule);
        LOG_INFO("OpenSteamTool init thread started");

        if (!InitializeSteamComponents(selfModule)) {
            LOG_ERROR("InitializeSteamComponents failed");
            return 1;
        }

        Config::Load(ConfigPath);
        Log::InitModules();
        Log::InstallPlatformLogSink();
        SteamDiagnostics::Initialize(SteamclientPath, SteamUIPath);

        std::vector<std::string> watchDirs;
        std::unordered_set<std::string> seenDirs;
        auto addWatchDir = [&](std::string_view dir) {
            if (dir.empty()) return;
            std::string norm = OSTPlatform::Encoding::PathToUtf8(
                OSTPlatform::Encoding::PathFromUtf8(dir).lexically_normal());
            while (norm.size() > 1 && (norm.back() == '\\' || norm.back() == '/') &&
                   !(norm.size() == 3 && norm[1] == ':')) {
                norm.pop_back();
            }
            std::string key = norm;
            for (char& ch : key) {
                ch = (ch == '/') ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            if (seenDirs.insert(std::move(key)).second) {
                watchDirs.push_back(std::move(norm));
            }
        };
        for (const auto& p : Config::GetLuaPaths()) {
            addWatchDir(p);
        }
        addWatchDir(LuaDir);
        // In portable mode, also watch Steam's config/lua if it already exists
        if (IsPortableMode()) {
            const auto steamLuaFs = OSTPlatform::Encoding::PathFromUtf8(SteamInstallPath) / "config" / "lua";
            std::error_code ecLua;
            if (std::filesystem::is_directory(steamLuaFs, ecLua) && !ecLua) {
                addWatchDir(OSTPlatform::Encoding::PathToUtf8(steamLuaFs));
            }
        }

        // Pre-parse Lua directory in Phase 0 (before PatternLoader / hooks),
        // ensuring Lua configuration is immutable and ready before any UI overview generation.
        try {
            for (const auto& dir : watchDirs) {
                LuaConfig::ParseDirectory(dir);
            }
        } catch (const std::exception& ex) {
            LOG_ERROR("LuaConfig::ParseDirectory exception in Phase 0: {}", ex.what());
        } catch (...) {
            LOG_ERROR("LuaConfig::ParseDirectory unknown exception in Phase 0");
        }

        // Load pattern files for steamclient64.dll and steamui.dll.
        // Each call computes the SHA-256 of the DLL on disk, checks the local
        // cache, and downloads from GitHub if needed.  Both calls are synchronous
        // but run on this worker thread, never under the loader lock.
        PatternLoader::Load(ui_hModule, SteamUIPath, "steamui");
        PatternLoader::Load(client_hModule, SteamclientPath, "steamclient");

        // Install only the Diversion/module redirection hooks early. The library
        // hooks must wait until Lua is parsed; installing them before ParseDirectory
        // creates a startup race where SteamUI can build its app overview while the
        // configured app set is still empty.
        HookManager::InstallEarlyUIHooks();

        // IPC method metadata (funcHash, fencepost, argc, ...)
        IPCLoader::Load(SteamclientPath);

        // Wire up lower-layer callbacks to hook notifications (IoC decoupling)
        ConfigFileWatcher::SetLicenseChangedCallback(&Hooks_Package::NotifyLicenseChanged);
        LuaFileWatcher::SetLicenseChangedCallback(&Hooks_Package::NotifyLicenseChanged);
        AppTicket::SetSourceTicketProvider(&Hooks_Decryption::GetCacheAppOwnershipTicket);
        CloudRedirectHost::SetLicenseChecker(&Hooks_Package::HasValidLicense);
        CloudRedirectHost::SetOnlineFixAppResolver([]() -> uint32_t {
            return Hooks_Misc::IsOnlineFixActive() ? Hooks_Misc::ResolveAppId() : 0;
        });

        // Match the proven pre-Diversion startup ordering for library/UI hooks:
        // Lua is fully loaded before SteamUI can evaluate ownership/overview data.
        HookManager::InstallUIHooks();

        // Replay configured apps via batch queue for eventual consistency across late injections.
        if (auto configuredApps = LuaConfig::GetConfiguredAppIdsSnapshot()) {
            Hooks_SteamUI::QueueAdditions(*configuredApps);
        }

        // Awaken installed scanner after its worker has been started by Install().
        Hooks_SteamUI::TriggerInstalledScanner();

        HookManager::InstallClientHooks();

        // Surface any functions that FindPattern() could not locate.
        PatternLoader::ReportMissingFunctions();

        // Optional Steam Cloud save redirection (CloudRedirect). No-op unless
        // [cloud].enabled is set and cloud_redirect.dll is present.
        CloudRedirectHost::Initialize(SteamInstallPath);

        g_HooksInstalled.store(true, std::memory_order_release);

        // Start watchers strictly after all hooks and subsystems are fully ready,
        // ensuring no dynamic file changes trigger notifications before callbacks/hooks exist.
        LuaFileWatcher::Start(watchDirs);
        ConfigFileWatcher::Start(ConfigPath, LuaDir);

        LOG_INFO("OpenSteamTool init complete ({})",
                 g_IsDiversionActive.load(std::memory_order_acquire) ? "Diversion active" : "Diversion bypassed, using original steamclient64");

        // Only refresh an exact-SHA pattern cache when this session actually found
        // a requested function entry missing. The parsed map is not hot-reloaded.
        PatternLoader::RefreshMissingMetadata();
        return 0;
    } catch (const std::exception& ex) {
        LOG_ERROR("Unhandled exception in InitThread: {}", ex.what());
        return 1;
    } catch (...) {
        LOG_ERROR("Unhandled unknown exception in InitThread");
        return 1;
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, PVOID pvReserved)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);

        // Keep this module pinned so explicit FreeLibrary cannot unload code
        // while hooks and worker threads may still reference it.
        HMODULE pinnedModule = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&DllMain), &pinnedModule);

        // Hand off all real work to a worker thread to avoid running file I/O,
        // module loading and detour transactions under the loader lock.
        OSTPlatform::Thread::StartDetached([module = reinterpret_cast<OSTPlatform::DynamicLibrary::ModuleHandle>(hModule)] {
            return InitThread(module);
        });
    }
    else if (dwReason == DLL_PROCESS_DETACH)
    {
        g_HooksInstalled.store(false, std::memory_order_release);
        // During process termination (pvReserved != nullptr), OS terminates all threads
        // before calling DllMain; avoid joining dead threads or performing loader-lock unhooks.
        if (pvReserved == nullptr) {
            ConfigFileWatcher::Stop();
            LuaFileWatcher::Stop();
            HookManager::UninstallClientHooks();
            HookManager::UninstallUIHooks();
            g_IsDiversionActive.store(false, std::memory_order_release);
            ConfigFileWatcher::SetLicenseChangedCallback(nullptr);
            LuaFileWatcher::SetLicenseChangedCallback(nullptr);
            AppTicket::SetSourceTicketProvider(nullptr);
            CloudRedirectHost::SetLicenseChecker(nullptr);
            CloudRedirectHost::SetOnlineFixAppResolver(nullptr);
            CloudRedirectHost::Shutdown();
            ManifestClient::Shutdown();
            StatsClient::Shutdown();
            LuaConfig::Shutdown();
            Log::Shutdown();
        } else {
            g_IsDiversionActive.store(false, std::memory_order_release);
            // Process termination (ExitProcess): The OS has already terminated all secondary
            // threads. Never join or wait on mutexes held by terminated threads under the loader
            // lock. Proactively detach all joinable worker threads so CRT static destruction
            // does not invoke std::terminate() -> abort().
            ConfigFileWatcher::Detach();
            LuaFileWatcher::Detach();
            HookManager::DetachWorkerThreads();
        }
    }

    return TRUE;
}
