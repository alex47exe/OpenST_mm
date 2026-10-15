#include "PatternLoader.h"
#include "dllmain.h"
#include "OSTPlatform/include/Encoding.h"
#include "OSTPlatform/include/Memory.h"
#include "OSTPlatform/include/Numbers.h"
#include "Utils/Logging/Log.h"
#include "Utils/SteamMetadata/RemoteToml.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"
#include "Utils/Support/FnvHash.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <toml++/toml.hpp>

// ---- compile-time sanity checks for FNV-1a table keys ----
// If the steam-monitor bot uses the same algorithm these must hold.
static_assert(Fnv1aHash("BBuildAndAsyncSendFrame") == 0x82428E37u,
              "FNV-1a mismatch for BBuildAndAsyncSendFrame");
static_assert(Fnv1aHash("BuildDepotDependency") == 0xC37F2D8Eu,
              "FNV-1a mismatch for BuildDepotDependency");

namespace {

// ---- per-function pattern record ----
struct PatternEntry {
    std::string name;
    uintptr_t   rva = 0;   // 0 = not present in file
};

// key = Fnv1aHash(funcName)
using PatternMap = std::unordered_map<uint32_t, PatternEntry>;

struct PatternSource {
    RemoteToml::Request request;
    bool fromCache = false;
    bool refreshNeeded = false;
    bool refreshAttempted = false;
};

// Global state synchronization
static std::shared_mutex g_patternMutex;

// module → its pattern map
static std::unordered_map<OSTPlatform::DynamicLibrary::ModuleHandle, PatternMap> g_moduleMaps;

// Metadata origin for each loaded module. A cached TOML is refreshed at most
// once per session, and only when a requested function entry is absent.
static std::unordered_map<OSTPlatform::DynamicLibrary::ModuleHandle, PatternSource> g_moduleSources;

// Modules whose Load() call failed (popup already shown). FindPattern
// silently returns nullptr for these — without re-logging or adding the
// function to g_missingFunctions — so we don't follow one "TOML missing"
// popup with a second popup listing every dependent hook.
static std::unordered_set<OSTPlatform::DynamicLibrary::ModuleHandle> g_failedModules;

// functions whose names were not found during FindPattern
static std::unordered_set<std::string> g_missingFunctions;

// ---- TOML pattern parser ----

// Section keys are hex literals like "0x82428E37"; each section is a table
// with optional `name` and `rva` (hex string or integer).
static PatternMap TableToPatternMap(const toml::table& tbl)
{
    PatternMap map;
    map.reserve(tbl.size());
    for (auto& [rawKey, val] : tbl) {
        if (!val.is_table()) continue;
        auto& sub = *val.as_table();

        const auto parsedKey = OSTPlatform::Numbers::ParseHexUInt32(rawKey.str());
        if (!parsedKey) continue;
        const uint32_t hashKey = *parsedKey;

        PatternEntry entry;
        if (auto v = sub["name"].value<std::string>()) entry.name = *v;

        // Dual-mode compatibility for string hex or native integer RVA
        if (auto v = sub["rva"].value<std::string>()) {
            if (const auto rva = OSTPlatform::Numbers::ParseHexUInt64(*v)) {
                entry.rva = static_cast<uintptr_t>(*rva);
            }
        } else if (auto v = sub["rva"].value<int64_t>()) {
            if (*v > 0) {
                entry.rva = static_cast<uintptr_t>(*v);
            }
        }

        map[hashKey] = std::move(entry);
    }
    return map;
}

static PatternMap ParsePatternString(std::string_view body,
                                     std::string* outError = nullptr)
{
    try {
        return TableToPatternMap(toml::parse(body));
    } catch (const toml::parse_error& e) {
        if (outError) *outError = e.description();
        return {};
    }
}

// ---- popup helpers (detached threads so we never block Steam) ----

static void ShowDownloadFailedPopup(const std::string& dllName,
                                    const std::string& sha256,
                                    const std::string& component)
{
    const std::string rootLabel = IsPortableMode() ? "<PortableDir>" : "<Steam>";
    SteamDiagnostics::ShowWarning(
        "OpenSteamTool - Unsupported Steam Version",
        "OpenSteamTool: signature file not found for " + dllName + ".\n\n"
        "Hooks that depend on " + dllName + " are disabled for this session; "
        "other modules are unaffected.\n\n"
        "You can:\n"
        "  1. Wait for the next signature update, then restart Steam.\n"
        "  2. Drop a matching TOML at:\n"
        "       " + rootLabel + "\\opensteamtool\\pattern\\" + component + "\\" + sha256 + ".toml\n"
        "  3. Check tracker repository:\n"
        "       https://github.com/mmxlyo/steam-monitor/tree/pattern/" + component + "\n"
        "  4. Report the diagnostics below:\n"
        "       https://github.com/mmxlyo/OpenSteamTool/issues");
}

} // namespace

// ---- public API ----

namespace PatternLoader {

    constexpr const char* kPatternChannel = "pattern";

bool Load(OSTPlatform::DynamicLibrary::ModuleHandle module, const std::string& dllPath, const std::string& component)
{
    if (!module) return false;

    // Delegate fetch + cache + mirror fallback to RemoteToml.
    RemoteToml::Request request{
        kPatternChannel,
        component,
        dllPath,
    };
    RemoteToml::Result r = RemoteToml::Fetch(request);

    if (r.ok) {
        std::string parseErr;
        PatternMap map = ParsePatternString(r.body, &parseErr);
        if (!map.empty()) {
            LOG_INFO("PatternLoader: loaded {} patterns for {} ({})",
                     map.size(), component, r.fromCache ? "cache" : "remote");
            {
                std::unique_lock<std::shared_mutex> lock(g_patternMutex);
                g_moduleMaps[module] = std::move(map);
                g_moduleSources[module] = {
                    std::move(request),
                    r.fromCache,
                    false,
                    false,
                };
                g_failedModules.erase(module);
            }
            return true;
        }
        LOG_WARN("PatternLoader: TOML for {} parsed empty ({})",
                 component, parseErr.empty() ? "no entries" : parseErr);
    }

    // Total failure — popup + disable module's hooks.
    std::string dllName = OSTPlatform::Encoding::PathToUtf8(
        OSTPlatform::Encoding::PathFromUtf8(dllPath).filename());
    std::string sha     = r.sha256.empty() ? "(hash failed)" : r.sha256;
    ShowDownloadFailedPopup(dllName, sha, component);

    {
        std::unique_lock<std::shared_mutex> lock(g_patternMutex);
        g_failedModules.insert(module);
    }
    return false;
}

void* FindPattern(OSTPlatform::DynamicLibrary::ModuleHandle module, const char* funcName)
{
    if (!module || !funcName || !*funcName) {
        return nullptr;
    }

    uint32_t key = Fnv1aHash(funcName);
    PatternEntry entry;
    bool found = false;

    // Fast read path under shared lock
    {
        std::shared_lock<std::shared_mutex> readLock(g_patternMutex);
        if (g_failedModules.contains(module)) {
            return nullptr;
        }

        auto mapIt = g_moduleMaps.find(module);
        if (mapIt == g_moduleMaps.end()) {
            readLock.unlock();
            // Module was never loaded
            LOG_WARN("PatternLoader: FindPattern called for module that was never loaded ('{}')", funcName);
            std::unique_lock<std::shared_mutex> writeLock(g_patternMutex);
            g_missingFunctions.insert(funcName);
            return nullptr;
        }

        auto entryIt = mapIt->second.find(key);
        if (entryIt != mapIt->second.end()) {
            entry = entryIt->second;
            found = true;
        }
    }

    if (!found) {
        LOG_WARN("PatternLoader: no entry for '{}' (key=0x{:08X})", funcName, key);
        std::unique_lock<std::shared_mutex> writeLock(g_patternMutex);
        auto sourceIt = g_moduleSources.find(module);
        if (sourceIt != g_moduleSources.end() &&
            sourceIt->second.fromCache &&
            !sourceIt->second.refreshAttempted) {
            sourceIt->second.refreshNeeded = true;
        }
        g_missingFunctions.insert(funcName);
        return nullptr;
    }

    // Direct RVA offset with module bounds validation
    if (entry.rva != 0) {
        if (const auto image = OSTPlatform::Memory::GetModuleImage(module)) {
            if (entry.rva >= image->size) {
                LOG_WARN("PatternLoader: {} RVA 0x{:X} exceeds module image size 0x{:X}",
                         funcName, entry.rva, image->size);
            } else {
                void* addr = reinterpret_cast<void*>(
                    reinterpret_cast<uintptr_t>(module) + entry.rva);
                LOG_DEBUG("PatternLoader: {} resolved via RVA 0x{:X}", funcName, entry.rva);
                return addr;
            }
        }
    } else {
        LOG_WARN("PatternLoader: entry for '{}' has no valid RVA", funcName);
    }

    {
        std::unique_lock<std::shared_mutex> writeLock(g_patternMutex);
        g_missingFunctions.insert(funcName);
    }
    return nullptr;
}

void RefreshMissingMetadata()
{
    std::vector<RemoteToml::Request> requestsToRefresh;
    {
        std::unique_lock<std::shared_mutex> lock(g_patternMutex);
        for (auto& item : g_moduleSources) {
            PatternSource& source = item.second;
            if (!source.refreshNeeded || source.refreshAttempted)
                continue;

            source.refreshNeeded = false;
            source.refreshAttempted = true;
            requestsToRefresh.push_back(source.request);
        }
    }

    // Perform lazy fetch outside the mutex lock
    for (const auto& req : requestsToRefresh) {
        LOG_DEBUG("PatternLoader: lazy refresh requested for {}", req.component);
        (void)RemoteToml::Fetch(req, RemoteToml::FetchMode::RemoteOnly);
    }
}

void ReportMissingFunctions()
{
    std::vector<std::string> missingList;
    {
        std::unique_lock<std::shared_mutex> lock(g_patternMutex);
        if (g_missingFunctions.empty()) return;
        missingList.assign(g_missingFunctions.begin(), g_missingFunctions.end());
        g_missingFunctions.clear();
    }

    std::sort(missingList.begin(), missingList.end());

    std::string list;
    for (const auto& name : missingList) {
        list += "  - " + name + "\n";
    }

    SteamDiagnostics::ShowWarning(
        "OpenSteamTool - Missing Signatures",
        "OpenSteamTool: some functions could not be located.\n\n"
        "The following functions were not found in the signature file:\n" +
        list +
        "\nHooks for these functions are disabled for this session.\n\n"
        "Please report this at:\n"
        "https://github.com/mmxlyo/OpenSteamTool/issues");
}

} // namespace PatternLoader
