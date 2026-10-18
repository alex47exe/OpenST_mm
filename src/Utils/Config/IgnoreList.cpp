#include "Utils/Config/IgnoreList.h"
#include "OSTPlatform/include/Encoding.h"
#include "Utils/Logging/Log.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// IgnoreList — per-file tracked set of suppressed appids.
//
// File format (ignore.txt):
//   - One decimal appid per line.
//   - Lines starting with # (after optional whitespace) are comments.
//   - Blank lines are skipped.
//   - Duplicate entries within or across files are silently deduplicated.
//
// Example:
//   # suppress updates for these games
//   1091500
//   1172470

namespace IgnoreList {
namespace {

    std::mutex g_mutex;

    // Per-file set of appids it contributes.
    std::unordered_map<std::string, std::unordered_set<AppId_t>> g_fileAppIds;

    // Flat union rebuilt from g_fileAppIds on every change.
    std::unordered_set<AppId_t> g_activeSet;

    static void RebuildActiveLocked() {
        g_activeSet.clear();
        for (const auto& [file, ids] : g_fileAppIds) {
            for (AppId_t id : ids) {
                g_activeSet.insert(id);
            }
        }
    }

    // Parse a single ignore.txt file and return the set of appids inside it.
    static std::unordered_set<AppId_t> ParseFile(const std::string& filePath) {
        std::unordered_set<AppId_t> result;
        std::ifstream f(OSTPlatform::Encoding::PathFromUtf8(filePath));
        if (!f) return result;

        std::string line;
        uint32_t lineNo = 0;
        while (std::getline(f, line)) {
            ++lineNo;
            // Strip CR
            if (!line.empty() && line.back() == '\r') line.pop_back();

            // Trim leading whitespace
            size_t start = 0;
            while (start < line.size() && std::isspace(static_cast<unsigned char>(line[start])))
                ++start;
            if (start == line.size()) continue;       // blank line
            if (line[start] == '#') continue;          // comment

            // Take only the part before " - " (optional name annotation).
            // Supports both plain "1091500" and "1091500 - Game Name" formats.
            size_t dashPos = line.find(" - ", start);
            size_t end = (dashPos != std::string::npos) ? dashPos : line.size();

            // Trim trailing whitespace from the token
            while (end > start && std::isspace(static_cast<unsigned char>(line[end - 1])))
                --end;

            std::string_view tok(line.data() + start, end - start);

            // Must be all digits
            bool valid = !tok.empty();
            for (char c : tok) {
                if (c < '0' || c > '9') { valid = false; break; }
            }
            if (!valid) {
                LOG_PACKAGE_WARN("IgnoreList: {}:{}: invalid entry '{}', skipping",
                                 filePath, lineNo, std::string(tok));
                continue;
            }

            // Parse as uint32
            uint64_t val = 0;
            for (char c : tok) val = val * 10 + static_cast<uint64_t>(c - '0');
            if (val == 0 || val > UINT32_MAX) {
                LOG_PACKAGE_WARN("IgnoreList: {}:{}: appid {} out of range, skipping",
                                 filePath, lineNo, val);
                continue;
            }

            result.insert(static_cast<AppId_t>(val));
        }
        return result;
    }

    static std::string NormPath(const std::string& raw) {
        return OSTPlatform::Encoding::PathToUtf8(
            OSTPlatform::Encoding::PathFromUtf8(raw).lexically_normal());
    }

} // anonymous namespace

void Reload(const std::vector<std::string>& directories) {
    std::lock_guard lock(g_mutex);
    g_fileAppIds.clear();

    for (const auto& dir : directories) {
        std::filesystem::path ignorePath =
            OSTPlatform::Encoding::PathFromUtf8(dir) / "ignore.txt";
        std::error_code ec;
        if (!std::filesystem::exists(ignorePath, ec)) continue;

        std::string normPath = NormPath(
            OSTPlatform::Encoding::PathToUtf8(ignorePath.lexically_normal()));
        auto ids = ParseFile(normPath);
        if (!ids.empty()) {
            LOG_PACKAGE_INFO("IgnoreList: loaded {} appid(s) from '{}'",
                             ids.size(), normPath);
            g_fileAppIds[normPath] = std::move(ids);
        }
    }

    RebuildActiveLocked();
    LOG_PACKAGE_INFO("IgnoreList: {} appid(s) suppressed total", g_activeSet.size());
}

void ReloadFile(const std::string& filePath) {
    std::string norm = NormPath(filePath);
    auto ids = ParseFile(norm);

    std::lock_guard lock(g_mutex);
    if (ids.empty()) {
        g_fileAppIds.erase(norm);
        LOG_PACKAGE_INFO("IgnoreList: '{}' now empty, cleared", norm);
    } else {
        g_fileAppIds[norm] = std::move(ids);
        LOG_PACKAGE_INFO("IgnoreList: reloaded '{}' ({} appid(s))", norm,
                         g_fileAppIds[norm].size());
    }
    RebuildActiveLocked();
    LOG_PACKAGE_INFO("IgnoreList: {} appid(s) suppressed total", g_activeSet.size());
}

void UnloadFile(const std::string& filePath) {
    std::string norm = NormPath(filePath);
    std::lock_guard lock(g_mutex);
    if (g_fileAppIds.erase(norm) > 0) {
        RebuildActiveLocked();
        LOG_PACKAGE_INFO("IgnoreList: unloaded '{}', {} appid(s) suppressed total",
                         norm, g_activeSet.size());
    }
}

bool IsIgnored(AppId_t appId) {
    std::lock_guard lock(g_mutex);
    return g_activeSet.count(appId) > 0;
}

} // namespace IgnoreList