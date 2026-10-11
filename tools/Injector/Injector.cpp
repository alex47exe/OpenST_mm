#include "Injector.h"

#if __has_include(<OSTPlatform/SteamLocator.hpp>)
#include <OSTPlatform/SteamLocator.hpp>
#else
#include "../../src/OSTPlatform/include/OSTPlatform/SteamLocator.hpp"
#endif

#include <shellapi.h>
#include <tlhelp32.h>
#include <iostream>
#include <fstream>
#include <chrono>
#include <thread>
#include <vector>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <optional>
#include <utility>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <climits>
#include <type_traits>
#include <system_error>

namespace Injector {

namespace {

    // Move-only RAII Windows handle wrapper
    class ScopedHandle {
    public:
        constexpr ScopedHandle() noexcept = default;
        explicit ScopedHandle(HANDLE h) noexcept : handle_(IsValid(h) ? h : nullptr) {}
        ~ScopedHandle() noexcept { Reset(); }

        ScopedHandle(const ScopedHandle&) = delete;
        ScopedHandle& operator=(const ScopedHandle&) = delete;

        ScopedHandle(ScopedHandle&& other) noexcept : handle_(other.handle_) {
            other.handle_ = nullptr;
        }
        ScopedHandle& operator=(ScopedHandle&& other) noexcept {
            if (this != &other) {
                Reset(other.handle_);
                other.handle_ = nullptr;
            }
            return *this;
        }

        [[nodiscard]] HANDLE Get() const noexcept { return handle_; }
        [[nodiscard]] bool IsValid() const noexcept { return handle_ != nullptr; }
        explicit operator bool() const noexcept { return IsValid(); }

        void Reset(HANDLE h = nullptr) noexcept {
            HANDLE normalized = IsValid(h) ? h : nullptr;
            if (handle_ == normalized) {
                return;
            }
            if (handle_) {
                ::CloseHandle(handle_);
            }
            handle_ = normalized;
        }

        HANDLE* Put() noexcept {
            Reset();
            return &handle_;
        }

    private:
        static bool IsValid(HANDLE h) noexcept {
            return h != nullptr && h != INVALID_HANDLE_VALUE;
        }
        HANDLE handle_ = nullptr;
    };

    // Lightweight ScopeGuard for RAII cleanups with dismiss capability
    template <typename F>
    class ScopeGuard {
    public:
        explicit ScopeGuard(F&& func) noexcept(std::is_nothrow_move_constructible_v<F>)
            : func_(std::forward<F>(func)) {}
        ~ScopeGuard() noexcept {
            if (active_) func_();
        }

        ScopeGuard(const ScopeGuard&) = delete;
        ScopeGuard& operator=(const ScopeGuard&) = delete;
        ScopeGuard& operator=(ScopeGuard&&) = delete;
        ScopeGuard(ScopeGuard&& other) noexcept(std::is_nothrow_move_constructible_v<F>)
            : func_(std::move(other.func_)), active_(other.active_) {
            other.active_ = false;
        }

        void Dismiss() noexcept { active_ = false; }

    private:
        F func_;
        bool active_ = true;
    };

    // Global stop flag and process-lifetime manual-reset kernel event for zero-polling instant wakeup
    std::atomic<bool> g_stopRequested{ false };
    std::atomic<HANDLE> g_stopEventHandle{ nullptr };

    BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) noexcept {
        switch (ctrlType) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_stopRequested.store(true, std::memory_order_release);
            if (HANDLE hEvent = g_stopEventHandle.load(std::memory_order_acquire)) {
                ::SetEvent(hEvent);
            }
            return TRUE;
        default:
            return FALSE;
        }
    }

    // Trivially-destructible C++20 magic-static initialization: avoids atexit destructor lock
    // inversion and guarantees the kernel event remains valid for asynchronous OS CtrlRoutine callbacks.
    void EnsureStopHandlerRegistered() noexcept {
        static const bool s_registered = []() noexcept {
            HANDLE hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            g_stopEventHandle.store(hEvent, std::memory_order_release);
            ::SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
            if (g_stopRequested.load(std::memory_order_acquire) && hEvent != nullptr) {
                ::SetEvent(hEvent);
            }
            return true;
        }();
        (void)s_registered;
    }

    // Zero-polling interruptible wait backed by manual-reset kernel event
    bool SleepInterruptible(std::chrono::milliseconds duration) noexcept {
        if (g_stopRequested.load(std::memory_order_acquire)) {
            return false;
        }
        if (duration <= std::chrono::milliseconds::zero()) {
            return true;
        }
        if (HANDLE hEvent = g_stopEventHandle.load(std::memory_order_acquire)) {
            const DWORD waitMs = (duration.count() > static_cast<long long>(INFINITE - 1))
                ? (INFINITE - 1)
                : static_cast<DWORD>(duration.count());
            return ::WaitForSingleObject(hEvent, waitMs) != WAIT_OBJECT_0 &&
                   !g_stopRequested.load(std::memory_order_acquire);
        }

        constexpr auto kStep = std::chrono::milliseconds(50);
        auto remaining = duration;
        while (remaining > std::chrono::milliseconds::zero()) {
            if (g_stopRequested.load(std::memory_order_acquire)) {
                return false;
            }
            const auto slice = (remaining < kStep) ? remaining : kStep;
            std::this_thread::sleep_for(slice);
            remaining -= slice;
        }
        return !g_stopRequested.load(std::memory_order_acquire);
    }

    // Single-instance protection for the background watcher daemon
    class SingleInstanceGuard {
    public:
        SingleInstanceGuard() noexcept {
            // Using Local namespace guarantees seamless execution under standard user privileges
            // without requiring SeCreateGlobalPrivilege.
            HANDLE rawMutex = ::CreateMutexW(nullptr, FALSE, L"Local\\OpenSteamTool_AutoInject_Watcher");
            const DWORD gle = ::GetLastError();
            hMutex_.Reset(rawMutex);
            isAlreadyRunning_ = (gle == ERROR_ALREADY_EXISTS);
        }

        [[nodiscard]] bool IsConflict() const noexcept {
            return isAlreadyRunning_ || !hMutex_.IsValid();
        }

    private:
        ScopedHandle hMutex_;
        bool isAlreadyRunning_ = false;
    };

    [[nodiscard]] std::filesystem::path NormalizePathNoThrow(const std::filesystem::path& p) noexcept {
        if (p.empty()) {
            return {};
        }
        std::error_code ec;
        std::filesystem::path full = std::filesystem::absolute(p, ec);
        if (ec || full.empty()) {
            full = p;
        }
        full = full.lexically_normal();
        full.make_preferred();
        return full;
    }

    [[nodiscard]] std::string WideToUtf8(std::wstring_view wide) noexcept {
        if (wide.empty() || wide.size() > static_cast<size_t>(INT_MAX)) {
            return {};
        }
        const int size = ::WideCharToMultiByte(
            CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        if (size <= 0) {
            return {};
        }
        std::string utf8(static_cast<size_t>(size), '\0');
        ::WideCharToMultiByte(
            CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), size, nullptr, nullptr);
        return utf8;
    }

    [[nodiscard]] std::string PathToUtf8(const std::filesystem::path& path) noexcept {
        return WideToUtf8(path.native());
    }

    struct ProcessIdentity {
        DWORD pid = 0;
        ULONGLONG creationTime = 0;
    };

    // Query active process liveness and 64-bit FILETIME creation timestamp to prevent PID reuse collisions
    [[nodiscard]] std::optional<ULONGLONG> QueryActiveProcessCreationTime(DWORD pid) noexcept {
        ScopedHandle h(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        if (!h) {
            return std::nullopt;
        }
        DWORD exitCode = 0;
        if (!::GetExitCodeProcess(h.Get(), &exitCode) || exitCode != STILL_ACTIVE) {
            return std::nullopt;
        }
        FILETIME ftCreate{}, ftExit{}, ftKernel{}, ftUser{};
        if (!::GetProcessTimes(h.Get(), &ftCreate, &ftExit, &ftKernel, &ftUser)) {
            return 0ULL;
        }
        ULARGE_INTEGER uli{};
        uli.LowPart = ftCreate.dwLowDateTime;
        uli.HighPart = ftCreate.dwHighDateTime;
        return uli.QuadPart;
    }

    // Query Steam installation executable with 5-tier fast short-circuit detection
    [[nodiscard]] std::optional<std::filesystem::path> QuerySteamRegistryPath() noexcept {
        if (auto dir = OSTPlatform::SteamLocator::ResolvePath()) {
            return *dir / L"steam.exe";
        }
        return std::nullopt;
    }

    // Attach or allocate console for interactive sessions and force UTF-8 output
    void EnsureInteractiveConsole() noexcept {
        HANDLE hOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
        const DWORD fileType = (hOut != nullptr && hOut != INVALID_HANDLE_VALUE)
            ? ::GetFileType(hOut)
            : FILE_TYPE_UNKNOWN;
        if (fileType == FILE_TYPE_UNKNOWN) {
            if (::AttachConsole(ATTACH_PARENT_PROCESS) || ::AllocConsole()) {
                FILE* fp = nullptr;
                freopen_s(&fp, "CONOUT$", "w", stdout);
                freopen_s(&fp, "CONOUT$", "w", stderr);
                freopen_s(&fp, "CONIN$", "r", stdin);
                std::cout.clear();
                std::cerr.clear();
                std::cin.clear();
            }
        }
        ::SetConsoleOutputCP(CP_UTF8);
        ::SetConsoleCP(CP_UTF8);
        std::ios::sync_with_stdio(false);
        EnsureStopHandlerRegistered();
    }

    // Successor process detection with active liveness, creation timestamp, and exclusion verification
    template <typename ExcludePred>
    [[nodiscard]] std::optional<ProcessIdentity> PollSuccessorProcessWhere(
        DWORD oldPid,
        std::chrono::milliseconds timeout,
        ExcludePred&& isExcluded) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (g_stopRequested.load(std::memory_order_acquire)) {
                return std::nullopt;
            }

            auto pids = SnapshotProcessIds(L"steam.exe");
            PrioritizeCandidatePids(pids);
            for (DWORD pid : pids) {
                if (pid == oldPid) {
                    continue;
                }
                const auto createTime = QueryActiveProcessCreationTime(pid);
                if (!createTime.has_value()) {
                    continue;
                }
                if (isExcluded(pid, *createTime)) {
                    continue;
                }
                return ProcessIdentity{ pid, *createTime };
            }
            if (!SleepInterruptible(std::chrono::milliseconds(300))) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<ProcessIdentity> PollSuccessorProcess(
        DWORD oldPid,
        std::chrono::milliseconds timeout = std::chrono::seconds(15)) {
        return PollSuccessorProcessWhere(oldPid, timeout, [](DWORD, ULONGLONG) noexcept {
            return false;
        });
    }

    void PrintUsage() {
        std::cout << "OpenSteamTool Injector (ost-Injector)\n";
        std::cout << "Usage: ost-Injector.exe [options]\n\n";
        std::cout << "Options:\n";
        std::cout << "  -watch, --watch, -daemon, /watch  Run as background auto-injection watcher\n";
        std::cout << "  -silent, --silent, -s, /s         Perform silent injection once and exit\n";
        std::cout << "  -help, --help, -h, /?             Display this help message and exit\n";
    }

} // namespace

// ============================================================================
// Public Interface Implementations
// ============================================================================

std::wstring Utf8ToWide(std::string_view utf8) {
    if (utf8.empty() || utf8.size() > static_cast<size_t>(INT_MAX)) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring wide(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
    return wide;
}

[[nodiscard]] inline bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size() || a.size() > static_cast<size_t>(INT_MAX)) {
        return false;
    }
    if (a.empty()) {
        return true;
    }
    return ::CompareStringOrdinal(
        a.data(), static_cast<int>(a.size()),
        b.data(), static_cast<int>(b.size()),
        TRUE) == CSTR_EQUAL;
}

namespace {

    struct ModuleProbeResult {
        bool snapshotOk = false;
        bool hasPayload = false;
        bool hasClient = false;
        bool hasUI = false;
        bool hasTier0 = false;
    };

    // Single-pass cross-process module inspection with transient PEB_LDR retry
    [[nodiscard]] ModuleProbeResult InspectProcessModules(DWORD pid, std::wstring_view payloadName = {}) noexcept {
        ModuleProbeResult result{};
        for (int attempt = 0; attempt < 5; ++attempt) {
            ScopedHandle snap(::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid));
            if (!snap) {
                const DWORD gle = ::GetLastError();
                if ((gle == ERROR_BAD_LENGTH || gle == ERROR_PARTIAL_COPY) && attempt + 1 < 5) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
                    continue;
                }
                return result;
            }

            result.snapshotOk = true;
            MODULEENTRY32W me{};
            me.dwSize = sizeof(me);
            if (::Module32FirstW(snap.Get(), &me)) {
                const bool checkPayload = !payloadName.empty();
                do {
                    if (checkPayload && EqualsIgnoreCase(payloadName, me.szModule)) {
                        result.hasPayload = true;
                        break;
                    }
                    if (!result.hasClient && EqualsIgnoreCase(L"steamclient64.dll", me.szModule)) {
                        result.hasClient = true;
                    } else if (!result.hasUI && EqualsIgnoreCase(L"steamui.dll", me.szModule)) {
                        result.hasUI = true;
                    } else if (!result.hasTier0 && EqualsIgnoreCase(L"tier0_s64.dll", me.szModule)) {
                        result.hasTier0 = true;
                    }
                } while (::Module32NextW(snap.Get(), &me) &&
                         (!result.hasClient || !result.hasUI || !result.hasTier0));
            }
            break;
        }
        return result;
    }

} // namespace

bool IsModulePresent(DWORD pid, std::wstring_view moduleName) {
    if (moduleName.empty()) {
        return false;
    }
    return InspectProcessModules(pid, moduleName).hasPayload;
}

std::vector<DWORD> SnapshotProcessIds(std::wstring_view processName) {
    std::vector<DWORD> pids;
    ScopedHandle snap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snap) return pids;

    DWORD currentSessionId = 0;
    const bool hasSession = ProcessIdToSessionId(GetCurrentProcessId(), &currentSessionId);

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap.Get(), &pe)) {
        do {
            if (EqualsIgnoreCase(processName, pe.szExeFile)) {
                if (hasSession) {
                    DWORD targetSessionId = 0;
                    if (ProcessIdToSessionId(pe.th32ProcessID, &targetSessionId) &&
                        targetSessionId != currentSessionId) {
                        continue; // Skip processes from other terminal sessions (Session 0, other users)
                    }
                }
                pids.push_back(pe.th32ProcessID);
            }
        } while (Process32NextW(snap.Get(), &pe));
    }
    return pids;
}

void PrioritizeCandidatePids(std::vector<DWORD>& pids) {
    if (pids.size() <= 1) return;

    struct PidScore {
        DWORD pid = 0;
        int score = 0;
        ULONGLONG creationTime = 0;
    };

    std::vector<PidScore> scored;
    scored.reserve(pids.size());

    for (DWORD pid : pids) {
        int score = 0;
        ULONGLONG createTime = 0;

        ScopedHandle h(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        if (h) {
            DWORD exitCode = 0;
            if (!GetExitCodeProcess(h.Get(), &exitCode) || exitCode != STILL_ACTIVE) {
                score = -100; // Deprioritize terminated/zombie processes
            } else {
                BOOL isWow64 = FALSE;
                if (IsWow64Process(h.Get(), &isWow64) && !isWow64) {
                    score += 10; // Prefer native 64-bit host processes over 32-bit helpers
                }

                FILETIME ftCreate{}, ftExit{}, ftKernel{}, ftUser{};
                if (GetProcessTimes(h.Get(), &ftCreate, &ftExit, &ftKernel, &ftUser)) {
                    ULARGE_INTEGER uli{};
                    uli.LowPart = ftCreate.dwLowDateTime;
                    uli.HighPart = ftCreate.dwHighDateTime;
                    createTime = uli.QuadPart;
                }
            }
        }

        if (score >= 0) {
            const auto mods = InspectProcessModules(pid);
            if (mods.hasClient) score += 100;
            if (mods.hasUI)     score += 50;
            if (mods.hasTier0)  score += 20;
        }

        scored.push_back({ pid, score, createTime });
    }

    std::sort(scored.begin(), scored.end(), [](const PidScore& a, const PidScore& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.creationTime > b.creationTime; // Newer process first
    });

    for (size_t i = 0; i < pids.size(); ++i) {
        pids[i] = scored[i].pid;
    }
}

RuntimeConfig ResolveConfig(const std::filesystem::path& baseDir) {
    std::error_code ec;
    const std::filesystem::path effectiveBase = baseDir.empty()
        ? NormalizePathNoThrow(std::filesystem::current_path(ec))
        : NormalizePathNoThrow(baseDir);

    const std::filesystem::path iniPath = effectiveBase / L"config.ini";
    const std::filesystem::path defaultDll = effectiveBase / L"OpenSteamTool.dll";
    std::filesystem::path resolvedExe;
    std::filesystem::path resolvedDll = defaultDll;

    if (std::filesystem::is_regular_file(iniPath, ec) && !ec) {
        wchar_t bufExe[1024]{};
        wchar_t bufDll[1024]{};
        GetPrivateProfileStringW(L"Settings", L"ExePath", L"", bufExe, static_cast<DWORD>(std::size(bufExe)), iniPath.c_str());
        GetPrivateProfileStringW(L"Settings", L"DllPath", L"", bufDll, static_cast<DWORD>(std::size(bufDll)), iniPath.c_str());

        if (bufExe[0] != L'\0') {
            std::filesystem::path rawExe(bufExe);
            resolvedExe = rawExe.is_absolute() ? rawExe : (effectiveBase / rawExe);
        }
        if (bufDll[0] != L'\0') {
            std::filesystem::path rawDll(bufDll);
            resolvedDll = rawDll.is_absolute() ? rawDll : (effectiveBase / rawDll);
        }
    }

    // Fallback to default sibling OpenSteamTool.dll if configured DllPath is stale/missing on disk
    std::error_code ecDll;
    if (!std::filesystem::is_regular_file(resolvedDll, ecDll) || ecDll) {
        std::error_code ecDefDll;
        if (std::filesystem::is_regular_file(defaultDll, ecDefDll) && !ecDefDll) {
            resolvedDll = defaultDll;
        }
    }

    // Fallback to 5-tier SteamLocator when ExePath is unset or points to a non-existent path on disk
    std::error_code ecExe;
    if (resolvedExe.empty() || !std::filesystem::is_regular_file(resolvedExe, ecExe) || ecExe) {
        if (auto regPath = QuerySteamRegistryPath()) {
            resolvedExe = *regPath;
        } else if (resolvedExe.empty()) {
            resolvedExe = L"C:\\Program Files (x86)\\Steam\\steam.exe";
        }

        std::error_code ecIni;
        if (!std::filesystem::exists(iniPath, ecIni) && !ecIni) {
            // Atomic config creation (INV-04): write UTF-16 LE BOM + INI payload to PID-isolated .tmp then MoveFileExW
            std::filesystem::path tmpPath = iniPath;
            tmpPath += L"." + std::to_wstring(::GetCurrentProcessId()) + L".tmp";

            std::wstring iniContent = L"\xFEFF[Settings]\r\nExePath=";
            iniContent += resolvedExe.wstring();
            iniContent += L"\r\nDllPath=OpenSteamTool.dll\r\n";

            ScopedHandle hTmp(::CreateFileW(
                tmpPath.c_str(),
                GENERIC_WRITE,
                0,
                nullptr,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                nullptr));
            if (hTmp) {
                const DWORD bytesToWrite = static_cast<DWORD>(iniContent.size() * sizeof(wchar_t));
                DWORD bytesWritten = 0;
                const BOOL writeOk = ::WriteFile(hTmp.Get(), iniContent.data(), bytesToWrite, &bytesWritten, nullptr);
                hTmp.Reset();
                if (writeOk && bytesWritten == bytesToWrite) {
                    if (!::MoveFileExW(tmpPath.c_str(), iniPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                        ::DeleteFileW(tmpPath.c_str());
                    }
                } else {
                    ::DeleteFileW(tmpPath.c_str());
                }
            }
        }
    }

    resolvedExe = NormalizePathNoThrow(resolvedExe);
    resolvedDll = NormalizePathNoThrow(resolvedDll);
    return { resolvedExe, resolvedDll, effectiveBase };
}

ExecutionResult InjectPayload(DWORD pid, const std::filesystem::path& dllPath, bool isSilent) {
    const std::filesystem::path fullDllPath = NormalizePathNoThrow(dllPath);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(fullDllPath, ec) || ec) {
        return { Status::PayloadNotFound, ERROR_FILE_NOT_FOUND, "Payload DLL not found: " + PathToUtf8(fullDllPath) };
    }

    const std::wstring payloadName = fullDllPath.filename().wstring();
    if (IsModulePresent(pid, payloadName)) {
        return { Status::AlreadyInjected, 0, "Module already loaded in target process." };
    }

    constexpr DWORD kAccess = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                              PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;

    ScopedHandle hProcess(OpenProcess(kAccess, FALSE, pid));
    if (!hProcess) {
        DWORD gle = GetLastError();
        if (!isSilent) {
            std::cerr << "[-] OpenProcess failed (PID=" << pid << ", Error=" << gle << ").\n";
        }
        return { Status::ProcessAccessDenied, gle, "OpenProcess failed. Administrator privilege may be required." };
    }

    BOOL isWow64 = FALSE;
    if (IsWow64Process(hProcess.Get(), &isWow64) && isWow64) {
        return { Status::IncompatibleArchitecture, ERROR_IMAGE_MACHINE_TYPE_MISMATCH_EXE,
            "Target process is 32-bit (WOW64), but OpenSteamTool payload is 64-bit." };
    }

    const std::wstring nativeDllPath = fullDllPath.wstring();
    const SIZE_T byteCount = (nativeDllPath.size() + 1) * sizeof(wchar_t);

    void* remoteMem = VirtualAllocEx(hProcess.Get(), nullptr, byteCount, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        DWORD gle = GetLastError();
        return { Status::RemoteAllocFailed, gle, "VirtualAllocEx failed." };
    }

    ScopeGuard freeRemoteGuard([&] {
        VirtualFreeEx(hProcess.Get(), remoteMem, 0, MEM_RELEASE);
    });

    SIZE_T bytesWritten = 0;
    if (!WriteProcessMemory(hProcess.Get(), remoteMem, nativeDllPath.c_str(), byteCount, &bytesWritten) ||
        bytesWritten != byteCount) {
        DWORD gle = GetLastError();
        return { Status::RemoteAllocFailed, gle, "WriteProcessMemory failed." };
    }

    // In a 64-bit Windows environment, system DLLs share identical virtual base addresses across 64-bit processes
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC pfnLoadLibraryW = hKernel32 ? GetProcAddress(hKernel32, "LoadLibraryW") : nullptr;
    if (!pfnLoadLibraryW) {
        return { Status::RemoteThreadFailed, GetLastError(), "Failed to resolve LoadLibraryW." };
    }

    ScopedHandle hThread(CreateRemoteThread(hProcess.Get(), nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(pfnLoadLibraryW), remoteMem, 0, nullptr));
    if (!hThread) {
        DWORD gle = GetLastError();
        return { Status::RemoteThreadFailed, gle, "CreateRemoteThread failed." };
    }

    DWORD waitRes = WaitForSingleObject(hThread.Get(), 15000);
    if (waitRes != WAIT_OBJECT_0) {
        // Critical defense: Do NOT free remote memory on timeout to avoid crashing target process (UAF)
        freeRemoteGuard.Dismiss();
        return { Status::ComponentTimeout, waitRes, "Remote LoadLibraryW execution timed out." };
    }

    // Verify target process did not terminate or crash during remote thread execution
    DWORD procExitCode = 0;
    if (GetExitCodeProcess(hProcess.Get(), &procExitCode) && procExitCode != STILL_ACTIVE) {
        return { Status::TargetExited, procExitCode, "Target process terminated or crashed during injection." };
    }

    DWORD exitCode = 0;
    const BOOL hasExitCode = GetExitCodeThread(hThread.Get(), &exitCode);

    // Ground-truth verification: regardless of the truncated 32-bit thread exit code,
    // verify the payload module actually resides in the target process PEB_LDR module list.
    bool moduleLoaded = IsModulePresent(pid, payloadName);
    for (int retry = 0; !moduleLoaded && retry < 3; ++retry) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (GetExitCodeProcess(hProcess.Get(), &procExitCode) && procExitCode != STILL_ACTIVE) {
            return { Status::TargetExited, procExitCode, "Target process terminated or crashed during injection." };
        }
        moduleLoaded = IsModulePresent(pid, payloadName);
    }

    if (!moduleLoaded) {
        const DWORD diagCode = hasExitCode ? exitCode : GetLastError();
        return { Status::LoadLibraryFailed, diagCode,
            "LoadLibraryW failed to load payload in target process (ThreadExitCode=" +
            std::to_string(diagCode) +
            "). Check if OpenSteamTool.dll is blocked by antivirus or missing dependencies." };
    }

    return { Status::Success, 0, "Injection succeeded." };
}

ExecutionResult AwaitAndInject(DWORD pid, const std::filesystem::path& dllPath,
                               std::chrono::milliseconds timeout,
                               bool isSilent) {
    const std::filesystem::path fullDllPath = NormalizePathNoThrow(dllPath);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(fullDllPath, ec) || ec) {
        return { Status::PayloadNotFound, ERROR_FILE_NOT_FOUND, "Payload DLL not found: " + PathToUtf8(fullDllPath) };
    }

    const std::wstring payloadName = fullDllPath.filename().wstring();
    const auto initialMods = InspectProcessModules(pid, payloadName);
    if (initialMods.hasPayload) {
        return { Status::AlreadyInjected, 0, "Module already loaded in target process." };
    }

    // Hot-Attach Detection: If steamclient64.dll is already loaded, Steam is running stably
    // and past the UI-loading phase. Inject immediately without waiting.
    if (initialMods.hasClient) {
        if (!isSilent) {
            std::cout << "[!] 检测到 Steam 已完全启动（steamclient64.dll 已加载），执行热补丁注入...\n";
        }
        return InjectPayload(pid, fullDllPath, isSilent);
    }

    ScopedHandle hWatch(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
    if (!hWatch) {
        return { Status::ProcessAccessDenied, GetLastError(), "Failed to open process for monitoring." };
    }

    const auto waitTargetOrStop = [&](DWORD waitMs) noexcept -> DWORD {
        if (HANDLE hStop = g_stopEventHandle.load(std::memory_order_acquire)) {
            const HANDLE waitHandles[2] = { hWatch.Get(), hStop };
            return ::WaitForMultipleObjects(2, waitHandles, FALSE, waitMs);
        }
        return ::WaitForSingleObject(hWatch.Get(), waitMs);
    };

    if (initialMods.hasUI) {
        const DWORD settleRes = waitTargetOrStop(20);
        if (settleRes == WAIT_OBJECT_0) {
            return { Status::TargetExited, 0, "Target process terminated before injection." };
        }
        if (settleRes == WAIT_OBJECT_0 + 1 || g_stopRequested.load(std::memory_order_acquire)) {
            return { Status::TargetExited, ERROR_CANCELLED, "Operation cancelled by user or system shutdown." };
        }
        return InjectPayload(pid, fullDllPath, isSilent);
    }

    const auto startTime = std::chrono::steady_clock::now();
    bool beaconSeen = initialMods.hasTier0;

    while (std::chrono::steady_clock::now() - startTime < timeout) {
        if (g_stopRequested.load(std::memory_order_acquire)) {
            return { Status::TargetExited, ERROR_CANCELLED, "Operation cancelled by user or system shutdown." };
        }

        // Staged dynamic interval:
        //   Phase 1 (before beacon seen): 200ms gentle polling to avoid burning CPU during early startup
        //   Phase 2 (after beacon seen):  50ms tight burst to precisely catch the UI loading moment
        const DWORD currentInterval = beaconSeen ? 50 : 200;

        // Kernel wait: Sleep without active spinning. Wakes immediately in 0ms on either
        // target process termination or process-wide stop event.
        const DWORD waitRes = waitTargetOrStop(currentInterval);
        if (waitRes == WAIT_OBJECT_0) {
            return { Status::TargetExited, 0, "Target process terminated before steamui.dll loaded." };
        }
        if (waitRes == WAIT_OBJECT_0 + 1 || g_stopRequested.load(std::memory_order_acquire)) {
            return { Status::TargetExited, ERROR_CANCELLED, "Operation cancelled by user or system shutdown." };
        }

        // Single-snapshot probe for payload, tier0_s64.dll, and steamui.dll
        const auto mods = InspectProcessModules(pid, payloadName);
        if (mods.hasPayload) {
            return { Status::AlreadyInjected, 0, "Module already loaded in target process." };
        }
        if (mods.hasTier0) {
            beaconSeen = true;
        }
        if (mods.hasUI || mods.hasClient) {
            // Smooth 20ms kernel wait to let PE mapping and loader linking settle
            const DWORD settleRes = waitTargetOrStop(20);
            if (settleRes == WAIT_OBJECT_0) {
                return { Status::TargetExited, 0, "Target process terminated before injection." };
            }
            if (settleRes == WAIT_OBJECT_0 + 1 || g_stopRequested.load(std::memory_order_acquire)) {
                return { Status::TargetExited, ERROR_CANCELLED, "Operation cancelled by user or system shutdown." };
            }
            return InjectPayload(pid, fullDllPath, isSilent);
        }
    }

    return { Status::ComponentTimeout, 0, "Timed out waiting for steamui.dll to load." };
}

void LogMessage(const std::filesystem::path& baseDir, const std::string& msg, bool isSilent) {
    if (!isSilent) {
        std::cout << msg << '\n';
    }
    try {
        std::filesystem::path logFile = baseDir / "inject.log";
        std::ofstream ofs(logFile, std::ios::app | std::ios::binary);
        if (ofs.is_open()) {
            const auto now = std::chrono::system_clock::now();
            const auto timeT = std::chrono::system_clock::to_time_t(now);
            std::tm tmNow{};
            localtime_s(&tmNow, &timeT);

            char timeBuf[32]{};
            const int timeLen = std::snprintf(
                timeBuf, sizeof(timeBuf), "[%04d-%02d-%02d %02d:%02d:%02d] ",
                tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday,
                tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec);
            if (timeLen > 0) {
                ofs.write(timeBuf, static_cast<std::streamsize>(timeLen));
            }
            ofs.write(msg.data(), static_cast<std::streamsize>(msg.size()));
            ofs.write("\r\n", 2);
        }
    } catch (...) {}
}

void ShowErrorAlert(const std::wstring& message) {
    MessageBoxW(nullptr, message.c_str(), L"OpenSteamTool Injector Error", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

int RunWatcher(const RuntimeConfig& config) {
    SingleInstanceGuard guard;
    if (guard.IsConflict()) {
        return 0; // Instance already running
    }

    EnsureStopHandlerRegistered();
    LogMessage(config.baseDir, "[Watcher] 后台自动注入监听已启动，等待 steam.exe 启动...", true);

    // Map PID -> creationTime (FILETIME QuadPart) to defend against Windows PID reuse
    std::unordered_map<DWORD, ULONGLONG> injectedPids;

    struct FailureRecord {
        ULONGLONG creationTime = 0;
        int count = 0;
        bool permanent = false;
    };
    std::unordered_map<DWORD, FailureRecord> failedPids;

    const auto isPidExcluded = [&](DWORD candidatePid, ULONGLONG candidateCreateTime) noexcept -> bool {
        if (auto itInj = injectedPids.find(candidatePid); itInj != injectedPids.end()) {
            if (itInj->second == candidateCreateTime) {
                return true;
            }
        }
        if (auto itFail = failedPids.find(candidatePid); itFail != failedPids.end()) {
            if (itFail->second.creationTime == candidateCreateTime &&
                (itFail->second.permanent || itFail->second.count >= 2)) {
                return true;
            }
        }
        return false;
    };

    const auto recordInjected = [&](DWORD targetPid, ULONGLONG targetCreateTime, Status status) {
        injectedPids[targetPid] = targetCreateTime;
        failedPids.erase(targetPid);
        if (status == Status::AlreadyInjected) {
            LogMessage(config.baseDir, "[Watcher] 检测到 Steam (PID: " + std::to_string(targetPid) + ") 已驻留 OpenSteamTool.dll，跳过重复注入。", true);
        } else {
            LogMessage(config.baseDir, "[Watcher] 成功自动注入 OpenSteamTool 到 Steam (PID: " + std::to_string(targetPid) + ")", true);
        }
    };

    while (!g_stopRequested.load(std::memory_order_acquire)) {
        auto pids = SnapshotProcessIds(L"steam.exe");
        if (!pids.empty()) {
            PrioritizeCandidatePids(pids);

            for (DWORD pid : pids) {
                if (g_stopRequested.load(std::memory_order_acquire)) {
                    break;
                }

                const auto createTimeOpt = QueryActiveProcessCreationTime(pid);
                if (!createTimeOpt.has_value()) {
                    continue;
                }
                const ULONGLONG createTime = *createTimeOpt;

                if (auto itInj = injectedPids.find(pid); itInj != injectedPids.end()) {
                    if (itInj->second == createTime) {
                        continue;
                    }
                    injectedPids.erase(itInj);
                }
                if (auto itFail = failedPids.find(pid); itFail != failedPids.end()) {
                    if (itFail->second.creationTime != createTime) {
                        failedPids.erase(itFail);
                    } else if (itFail->second.permanent || itFail->second.count >= 2) {
                        continue;
                    }
                }

                auto res = AwaitAndInject(pid, config.payloadDll, std::chrono::seconds(15), true);
                if (g_stopRequested.load(std::memory_order_acquire)) {
                    break;
                }

                if (res.status == Status::TargetExited) {
                    // Handle Steam cold-start updater restart (bootstrap exit & successor spawn)
                    auto successor = PollSuccessorProcessWhere(pid, std::chrono::seconds(8), isPidExcluded);
                    if (successor && !g_stopRequested.load(std::memory_order_acquire)) {
                        LogMessage(config.baseDir, "[Watcher] 捕获到 Steam 更新继任进程 (PID: " + std::to_string(successor->pid) + ")，继续注入...", true);
                        res = AwaitAndInject(successor->pid, config.payloadDll, std::chrono::seconds(25), true);
                        if (g_stopRequested.load(std::memory_order_acquire)) {
                            break;
                        }
                        if (res.IsOk()) {
                            recordInjected(successor->pid, successor->creationTime, res.status);
                        } else {
                            LogMessage(config.baseDir, "[Watcher] 注入继任进程失败 (PID: " + std::to_string(successor->pid) + "): " + res.message, true);
                            failedPids[successor->pid] = { successor->creationTime, 2, true };
                        }
                    }
                } else if (res.IsOk()) {
                    recordInjected(pid, createTime, res.status);
                } else {
                    if (res.status == Status::ProcessAccessDenied) {
                        failedPids[pid] = { createTime, 1, true };
                        LogMessage(config.baseDir, "[Watcher] 注入失败 (PID: " + std::to_string(pid) + "): 权限不足 (Access Denied)。如果 Steam 以管理员权限运行，请以管理员身份启动 Injector。", true);
                    } else if (res.status == Status::IncompatibleArchitecture || res.status == Status::PayloadNotFound) {
                        failedPids[pid] = { createTime, 1, true };
                        LogMessage(config.baseDir, "[Watcher] 注入失败 (PID: " + std::to_string(pid) + "): " + res.message, true);
                    } else {
                        auto& rec = failedPids[pid];
                        rec.creationTime = createTime;
                        rec.count++;
                        if (rec.count >= 2) {
                            rec.permanent = true;
                            LogMessage(config.baseDir, "[Watcher] PID: " + std::to_string(pid) + " 注入超时，可能为后台辅助进程，已挂起重试。", true);
                        } else {
                            LogMessage(config.baseDir, "[Watcher] 注入失败 (PID: " + std::to_string(pid) + "): " + res.message, true);
                        }
                    }
                }
            }
        }

        // Direct kernel liveness & creation-time checks to purge exited or reused PIDs
        std::erase_if(injectedPids, [](const auto& item) {
            const auto ct = QueryActiveProcessCreationTime(item.first);
            return !ct.has_value() || *ct != item.second;
        });
        std::erase_if(failedPids, [](const auto& item) {
            const auto ct = QueryActiveProcessCreationTime(item.first);
            return !ct.has_value() || *ct != item.second.creationTime;
        });

        if (!SleepInterruptible(std::chrono::milliseconds(1500))) {
            break;
        }
    }

    LogMessage(config.baseDir, "[Watcher] 后台监听服务正常退出。", true);
    return 0;
}

int RunSilentOnce(const RuntimeConfig& config) {
    EnsureStopHandlerRegistered();

    auto pids = SnapshotProcessIds(L"steam.exe");
    if (pids.empty()) return 0;
    PrioritizeCandidatePids(pids);

    DWORD activePid = pids.front();
    auto res = AwaitAndInject(activePid, config.payloadDll, std::chrono::seconds(30), true);
    if (g_stopRequested.load(std::memory_order_acquire)) {
        return 1;
    }

    if (res.status == Status::TargetExited) {
        auto successor = PollSuccessorProcess(activePid, std::chrono::seconds(10));
        if (successor && !g_stopRequested.load(std::memory_order_acquire)) {
            activePid = successor->pid;
            res = AwaitAndInject(activePid, config.payloadDll, std::chrono::seconds(25), true);
        }
    }

    if (res.IsOk()) {
        if (res.status == Status::AlreadyInjected) {
            LogMessage(config.baseDir, "[Silent] 检测到 Steam (PID: " + std::to_string(activePid) + ") 已驻留 OpenSteamTool.dll，跳过重复注入。", true);
        } else {
            LogMessage(config.baseDir, "[Silent] 成功静默注入 OpenSteamTool 到 Steam (PID: " + std::to_string(activePid) + ")", true);
        }
        return 0;
    } else {
        LogMessage(config.baseDir, "[Silent] 注入失败 (PID: " + std::to_string(activePid) + "): " + res.message, true);
        return 1;
    }
}

int RunInteractive(const RuntimeConfig& config) {
    EnsureStopHandlerRegistered();
    SetConsoleTitleW(L"OpenSteamTool Injector (ost-Injector)");

    std::cout << "=================================================\n";
    std::cout << "       OpenSteamTool Injector (ost-Injector)     \n";
    std::cout << "       Supported modes: manual, -silent, -watch   \n";
    std::cout << "=================================================\n\n";

    const std::string targetExeUtf8 = PathToUtf8(config.targetExe);
    const std::string payloadDllUtf8 = PathToUtf8(config.payloadDll);

    std::cout << "[+] Target Executable : " << targetExeUtf8 << "\n";
    std::cout << "[+] Payload DLL       : " << payloadDllUtf8 << "\n\n";

    std::error_code ec;
    if (!std::filesystem::is_regular_file(config.payloadDll, ec) || ec) {
        std::wstring err = L"Error: Payload DLL was not found at:\n" + config.payloadDll.wstring() + L"\n\nPlease ensure OpenSteamTool.dll exists.";
        std::cerr << "[-] " << payloadDllUtf8 << " not found!\n";
        ShowErrorAlert(err);
        return 1;
    }

    auto existingPids = SnapshotProcessIds(L"steam.exe");
    if (!existingPids.empty()) {
        PrioritizeCandidatePids(existingPids);
        DWORD pid = existingPids.front();
        std::cout << "[+] Found running Steam process (PID: " << pid << ")\n";

        std::cout << "[+] Waiting for Steam UI initialization...\n";
        auto res = AwaitAndInject(pid, config.payloadDll, std::chrono::seconds(30), false);
        if (res.status == Status::TargetExited && !g_stopRequested.load(std::memory_order_acquire)) {
            std::cout << "[!] 检测到 Steam 引导进程退出，正在捕获自更新后的继任主进程...\n";
            auto successor = PollSuccessorProcess(pid, std::chrono::seconds(15));
            if (successor && !g_stopRequested.load(std::memory_order_acquire)) {
                std::cout << "[+] 成功捕获继任 Steam 进程 (PID: " << successor->pid << ")，继续等待注入...\n";
                res = AwaitAndInject(successor->pid, config.payloadDll, std::chrono::seconds(30), false);
            }
        }

        if (g_stopRequested.load(std::memory_order_acquire)) {
            return 1;
        }
        if (res.status == Status::AlreadyInjected) {
            std::cout << "[!] 当前 Steam 进程已加载过 OpenSteamTool.dll！\n";
            std::cout << "[!] 无需重复注入。\n";
            std::cout << "This console will close in 3 seconds...\n";
            SleepInterruptible(std::chrono::seconds(3));
            return 0;
        }
        if (res.IsOk()) {
            std::cout << "[+] Injection completed successfully.\n";
            std::cout << "This console will close in 3 seconds...\n";
            SleepInterruptible(std::chrono::seconds(3));
            return 0;
        } else {
            std::cerr << "[-] Injection failed: " << res.message << "\n";
            ShowErrorAlert(L"DLL injection into running Steam process failed: " + Utf8ToWide(res.message));
            return 1;
        }
    }

    // Steam is not running: launch it
    if (!std::filesystem::is_regular_file(config.targetExe, ec) || ec) {
        std::wstring err = L"Error: Target Steam executable does not exist at:\n" + config.targetExe.wstring();
        std::cerr << "[-] " << targetExeUtf8 << " not found!\n";
        ShowErrorAlert(err);
        return 1;
    }

    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    std::wstring quotedCmd = L"\"" + config.targetExe.wstring() + L"\"";

    std::cout << "[+] Launching Steam executable...\n";
    std::wstring workingDir = config.targetExe.parent_path().wstring();

    if (!CreateProcessW(config.targetExe.c_str(), quotedCmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        workingDir.empty() ? nullptr : workingDir.c_str(), &si, &pi)) {
        DWORD gle = GetLastError();
        std::wstring err = L"CreateProcessW failed. Error: " + std::to_wstring(gle);
        std::cerr << "[-] CreateProcessW failed. Error: " << gle << "\n";
        ShowErrorAlert(err);
        return 1;
    }

    ScopedHandle hProcess(pi.hProcess);
    ScopedHandle hThread(pi.hThread);
    DWORD currentPid = pi.dwProcessId;

    std::cout << "[+] Waiting for Steam UI to initialize (PID: " << currentPid << ")...\n";
    auto res = AwaitAndInject(currentPid, config.payloadDll, std::chrono::seconds(15), false);

    // Handle Steam cold-start updater restart (bootstrap exit & successor spawn)
    if (res.status == Status::TargetExited && !g_stopRequested.load(std::memory_order_acquire)) {
        std::cout << "[!] 检测到 Steam 引导进程退出，正在捕获自更新后的继任主进程...\n";
        auto successor = PollSuccessorProcess(currentPid, std::chrono::seconds(15));
        if (successor && !g_stopRequested.load(std::memory_order_acquire)) {
            std::cout << "[+] 成功捕获继任 Steam 进程 (PID: " << successor->pid << ")，继续等待注入...\n";
            res = AwaitAndInject(successor->pid, config.payloadDll, std::chrono::seconds(30), false);
        }
    }

    if (g_stopRequested.load(std::memory_order_acquire)) {
        return 1;
    }
    if (res.IsOk()) {
        std::cout << "[+] Injection completed successfully.\n";
        std::cout << "This console will close in 3 seconds...\n";
        SleepInterruptible(std::chrono::seconds(3));
        return 0;
    } else {
        std::cerr << "[-] Injection failed: " << res.message << "\n";
        ShowErrorAlert(L"DLL injection failed: " + Utf8ToWide(res.message));
        return 1;
    }
}

} // namespace Injector

// Windows standard entry point
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool isWatchMode = false;
    bool isSilentMode = false;
    bool isHelpRequested = false;

    if (argvW) {
        for (int i = 1; i < argc; ++i) {
            const std::wstring_view arg(argvW[i]);
            if (Injector::EqualsIgnoreCase(arg, L"-watch") ||
                Injector::EqualsIgnoreCase(arg, L"--watch") ||
                Injector::EqualsIgnoreCase(arg, L"-daemon") ||
                Injector::EqualsIgnoreCase(arg, L"/watch")) {
                isWatchMode = true;
            } else if (Injector::EqualsIgnoreCase(arg, L"-silent") ||
                       Injector::EqualsIgnoreCase(arg, L"--silent") ||
                       Injector::EqualsIgnoreCase(arg, L"-s") ||
                       Injector::EqualsIgnoreCase(arg, L"/s") ||
                       Injector::EqualsIgnoreCase(arg, L"/silent")) {
                isSilentMode = true;
            } else if (Injector::EqualsIgnoreCase(arg, L"-help") ||
                       Injector::EqualsIgnoreCase(arg, L"--help") ||
                       Injector::EqualsIgnoreCase(arg, L"-h") ||
                       Injector::EqualsIgnoreCase(arg, L"/?") ||
                       Injector::EqualsIgnoreCase(arg, L"/h") ||
                       Injector::EqualsIgnoreCase(arg, L"/help")) {
                isHelpRequested = true;
            }
        }
        LocalFree(argvW);
    }

    if (isHelpRequested) {
        Injector::EnsureInteractiveConsole();
        Injector::PrintUsage();
        return 0;
    }

    if (!isWatchMode && !isSilentMode) {
        Injector::EnsureInteractiveConsole();
    }

    std::vector<wchar_t> pathBuffer(1024, L'\0');
    DWORD len = GetModuleFileNameW(nullptr, pathBuffer.data(), static_cast<DWORD>(pathBuffer.size()));
    while (len >= pathBuffer.size() - 1 && pathBuffer.size() < 32768) {
        pathBuffer.resize(pathBuffer.size() * 2, L'\0');
        len = GetModuleFileNameW(nullptr, pathBuffer.data(), static_cast<DWORD>(pathBuffer.size()));
    }
    std::error_code ecCwd;
    std::filesystem::path baseDir = (len > 0 && len < pathBuffer.size())
        ? std::filesystem::path(std::wstring_view(pathBuffer.data(), len)).parent_path()
        : std::filesystem::current_path(ecCwd);

    if (!baseDir.empty()) {
        std::filesystem::current_path(baseDir, ecCwd);
    }

    auto config = Injector::ResolveConfig(baseDir);

    if (isWatchMode) {
        return Injector::RunWatcher(config);
    }
    if (isSilentMode) {
        return Injector::RunSilentOnce(config);
    }

    return Injector::RunInteractive(config);
}
