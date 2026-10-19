#include "OutputWriter.h"
#include "I18n.h"
#include "Log.h"
#include "TuiEngine.h"
#include "Utils.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <unordered_set>

namespace OST::ExtractTickets {

namespace {
std::string TicketLine(const char* name, const std::optional<std::vector<uint8_t>>& ticket) {
    if (!ticket || ticket->empty()) return std::format("{}:null\n", name);
    return std::format("{}({}bytes):{}\n", name, ticket->size(), ToHexString(*ticket));
}

class EmitTracker {
public:
    [[nodiscard]] bool TryEmitAppId(uint32_t id) noexcept {
        return m_emittedAppIds.insert(id).second;
    }
    [[nodiscard]] bool TryEmitManifest(uint32_t depotId) noexcept {
        return m_emittedManifests.insert(depotId).second;
    }
    [[nodiscard]] bool TryEmitToken(uint32_t id) noexcept {
        return m_emittedTokens.insert(id).second;
    }

private:
    std::unordered_set<uint32_t> m_emittedAppIds;
    std::unordered_set<uint32_t> m_emittedManifests;
    std::unordered_set<uint32_t> m_emittedTokens;
};

// Maps companion item type string into normalized category tag
[[nodiscard]] inline std::string_view GetDlcCategoryTag(std::string_view type) noexcept {
    if (EqualIgnoreCase(type, "config")) return "[Config]";
    if (EqualIgnoreCase(type, "music")) return "[Music]";
    if (EqualIgnoreCase(type, "tool")) return "[Tool]";
    if (EqualIgnoreCase(type, "demo")) return "[Demo]";
    if (EqualIgnoreCase(type, "beta")) return "[Beta]";
    if (EqualIgnoreCase(type, "application")) return "[App]";
    return "[DLC]";
}

// Determines grouping and sort order for companion categories
[[nodiscard]] inline int GetCategoryOrder(std::string_view type) noexcept {
    if (EqualIgnoreCase(type, "config")) return 0;
    if (EqualIgnoreCase(type, "dlc") || type.empty()) return 1;
    if (EqualIgnoreCase(type, "music")) return 2;
    if (EqualIgnoreCase(type, "tool")) return 3;
    return 4;
}

// Appends a level-1 major section banner (clean single-line comment)
inline void AppendMajorSection(std::string& out, std::string_view title, bool leadingNewline = true) {
    if (leadingNewline) {
        if (!out.empty() && out.back() != '\n') {
            out += '\n';
        }
        out += '\n';
    }
    std::format_to(std::back_inserter(out), "-- {}\n", title);
}

// Appends a level-2 companion category sub-header
inline void AppendCategoryHeader(std::string& out, std::string_view categoryTag) {
    std::format_to(std::back_inserter(out), "\n-- {}\n", categoryTag);
}

} // namespace

bool WriteOutputs(uint32_t appId,
                  const std::optional<std::vector<uint8_t>>& ownership,
                  const std::optional<std::vector<uint8_t>>& encrypted,
                  const std::vector<DepotKeyInfo>& depotKeys,
                  const std::vector<DlcInfo>& dlcs,
                  const std::unordered_map<uint32_t, uint64_t>& appTokens,
                  bool isOnlineMode) {
    const std::string dir{std::to_string(appId)};
    std::filesystem::path dirPath = Utf8Path(dir);
    std::error_code ec;
    std::filesystem::create_directories(dirPath, ec);
    if (ec) {
        if (!TuiEngine::IsActive()) {
            std::cerr << TR_FMT(MsgKey::OutCreateDirFailed, dir, ec.message()) << "\n";
        }
        LOG_ERROR("OutputWriter", MsgKey::LogOutCreateDirFailed, dir, ec.message());
        return false;
    }

    bool ok{true};
    if (ownership && !ownership->empty()) ok = WriteBinaryFile(dirPath / "appticket.bin", *ownership) && ok;
    if (encrypted && !encrypted->empty()) ok = WriteBinaryFile(dirPath / "eticket.bin", *encrypted) && ok;

    // Copy manifest files (.manifest) if found in depotcache
    std::vector<std::string> copiedManifests;
    copiedManifests.reserve(depotKeys.size());
    for (const auto& dk : depotKeys) {
        if (!dk.manifestFilePath.empty()) {
            std::filesystem::path srcPath = Utf8Path(dk.manifestFilePath);
            std::string fname = PathToUtf8(srcPath.filename());
            if (std::ranges::find(copiedManifests, fname) == copiedManifests.end()) {
                std::filesystem::path destPath = dirPath / fname;
                std::error_code copyEc;
                if (std::filesystem::equivalent(srcPath, destPath, copyEc)) {
                    copiedManifests.push_back(fname);
                } else if (std::filesystem::copy_file(srcPath, destPath, std::filesystem::copy_options::overwrite_existing, copyEc)) {
                    copiedManifests.push_back(fname);
                } else {
                    if (std::filesystem::exists(destPath, copyEc)) {
                        copiedManifests.push_back(fname);
                    } else {
                        if (!TuiEngine::IsActive()) {
                            std::cerr << TR_FMT(MsgKey::OutCopyManifestFailed, fname, copyEc.message()) << "\n";
                        }
                        LOG_WARN("OutputWriter", MsgKey::LogOutCopyManifestFailed, fname, copyEc.message());
                    }
                }
            }
        }
    }

    uint64_t baseToken = 0;
    auto baseTokIt = appTokens.find(appId);
    if (baseTokIt != appTokens.end()) {
        baseToken = baseTokIt->second;
    }

    std::map<uint32_t, uint64_t> relevantTokens;
    if (baseToken != 0) {
        relevantTokens[appId] = baseToken;
    }
    for (const auto& dlc : dlcs) {
        auto it = appTokens.find(dlc.dlcId);
        if (it != appTokens.end() && it->second != 0) {
            relevantTokens[dlc.dlcId] = it->second;
        }
    }

    // Build tickets.txt summary
    std::string text;
    text.reserve(2048);
    std::format_to(std::back_inserter(text), "appid:{}\n", appId);
    std::unordered_set<uint32_t> emittedTxtDlc;
    for (const auto& dlc : dlcs) {
        if (emittedTxtDlc.insert(dlc.dlcId).second) {
            if (!dlc.name.empty()) {
                std::format_to(std::back_inserter(text), "dlc({}):{}\n", dlc.dlcId, SanitizeComment(dlc.name));
            } else {
                std::format_to(std::back_inserter(text), "dlc({})\n", dlc.dlcId);
            }
        }
    }
    std::unordered_set<uint32_t> emittedTxtKeys;
    for (const auto& dk : depotKeys) {
        if (!dk.hexKey.empty() && emittedTxtKeys.insert(dk.depotId).second) {
            std::format_to(std::back_inserter(text), "depotkey({}):{}\n", dk.depotId, dk.hexKey);
        }
    }
    std::unordered_set<uint32_t> emittedTxtMans;
    for (const auto& dk : depotKeys) {
        if (IsValidManifestId(dk.manifestId) && emittedTxtMans.insert(dk.depotId).second) {
            std::format_to(std::back_inserter(text), "manifest({}):{}\n", dk.depotId, dk.manifestId);
        }
    }
    if (baseToken != 0) {
        std::format_to(std::back_inserter(text), "token({}):{}\n", appId, baseToken);
    } else {
        std::format_to(std::back_inserter(text), "token({}):null\n", appId);
    }
    for (const auto& [tId, tVal] : relevantTokens) {
        if (tId != appId) {
            std::format_to(std::back_inserter(text), "token({}):{}\n", tId, tVal);
        }
    }
    text += TicketLine("appticket", ownership);
    text += TicketLine("eticket", encrypted);

    const std::filesystem::path textPath = dirPath / "tickets.txt";
    if (!WriteTextFileAtomic(textPath, text, false)) {
        const std::string textPathUtf8 = PathToUtf8(textPath);
        if (!TuiEngine::IsActive()) {
            std::cerr << TR_FMT(MsgKey::OutWriteFileFailed, textPathUtf8) << "\n";
        }
        LOG_ERROR("OutputWriter", MsgKey::LogOutWriteTicketsTxtFailed, textPathUtf8);
        return false;
    }

    // Generate ready-to-use Lua script
    std::string luaText;
    luaText.reserve(8192);
    std::format_to(std::back_inserter(luaText), "-- Auto-generated by extract_tickets for AppID: {}\n", appId);

    std::unordered_set<uint32_t> ownedDlcIdSet;
    for (const auto& dlc : dlcs) {
        ownedDlcIdSet.insert(dlc.dlcId);
    }

    const std::unordered_set<std::string> copiedManifestSet(copiedManifests.begin(), copiedManifests.end());
    auto hasManifestOnDisk = [&](uint32_t depotId, const std::string& manifestId) -> bool {
        std::string fname = std::format("{}_{}.manifest", depotId, manifestId);
        if (copiedManifestSet.contains(fname)) return true;
        std::filesystem::path p = dirPath / fname;
        std::error_code diskEc;
        return std::filesystem::exists(p, diskEc);
    };

    EmitTracker emitTracker;

    // 1. Base Game Unlocks (AppID and base game depots)
    AppendMajorSection(luaText, "Base Game");
    const DepotKeyInfo* baseAppDk = nullptr;
    for (const auto& dk : depotKeys) {
        if (dk.depotId == appId) {
            baseAppDk = &dk;
            break;
        }
    }

    if (emitTracker.TryEmitAppId(appId)) {
        if (baseAppDk && !baseAppDk->hexKey.empty()) {
            std::format_to(std::back_inserter(luaText), "addappid({}, 1, \"{}\")\n", appId, baseAppDk->hexKey);
        } else {
            std::format_to(std::back_inserter(luaText), "addappid({})\n", appId);
        }
    }

    if (baseToken != 0 && emitTracker.TryEmitToken(appId)) {
        std::format_to(std::back_inserter(luaText), "addtoken({}, \"{}\")\n", appId, baseToken);
    }

    for (const auto& dk : depotKeys) {
        const bool isDlcDepot = ownedDlcIdSet.contains(dk.depotId) ||
                                (dk.dlcId != 0 && ownedDlcIdSet.contains(dk.dlcId));
        if (dk.depotId != appId && !isDlcDepot) {
            if (!dk.hexKey.empty() && emitTracker.TryEmitAppId(dk.depotId)) {
                std::format_to(std::back_inserter(luaText), "addappid({}, 1, \"{}\")\n", dk.depotId, dk.hexKey);
            }
        }
    }

    // 2. Base Game Manifests
    bool hasBaseManifests = false;
    if (baseAppDk && IsValidManifestId(baseAppDk->manifestId)) {
        hasBaseManifests = true;
    }
    if (!hasBaseManifests) {
        for (const auto& dk : depotKeys) {
            const bool isDlcDepot = ownedDlcIdSet.contains(dk.depotId) ||
                                    (dk.dlcId != 0 && ownedDlcIdSet.contains(dk.dlcId));
            if (dk.depotId != appId && !isDlcDepot) {
                if (IsValidManifestId(dk.manifestId)) {
                    hasBaseManifests = true;
                    break;
                }
            }
        }
    }

    if (hasBaseManifests) {
        AppendMajorSection(luaText, "Base Game Manifests");
        if (baseAppDk && IsValidManifestId(baseAppDk->manifestId) && emitTracker.TryEmitManifest(appId)) {
            if (hasManifestOnDisk(appId, baseAppDk->manifestId)) {
                std::format_to(std::back_inserter(luaText), "setManifestid({}, \"{}\")\n", appId, baseAppDk->manifestId);
            } else {
                std::format_to(std::back_inserter(luaText), "-- setManifestid({}, \"{}\") -- {}\n", appId, baseAppDk->manifestId, TR(MsgKey::LuaManifestMissing));
            }
        }
        for (const auto& dk : depotKeys) {
            const bool isDlcDepot = ownedDlcIdSet.contains(dk.depotId) ||
                                    (dk.dlcId != 0 && ownedDlcIdSet.contains(dk.dlcId));
            if (dk.depotId != appId && !isDlcDepot) {
                if (IsValidManifestId(dk.manifestId) && emitTracker.TryEmitManifest(dk.depotId)) {
                    if (hasManifestOnDisk(dk.depotId, dk.manifestId)) {
                        std::format_to(std::back_inserter(luaText), "setManifestid({}, \"{}\")\n", dk.depotId, dk.manifestId);
                    } else {
                        std::format_to(std::back_inserter(luaText), "-- setManifestid({}, \"{}\") -- {}\n", dk.depotId, dk.manifestId, TR(MsgKey::LuaManifestMissing));
                    }
                }
            }
        }
    }

    // 3. Owned DLCs & Companions
    if (!dlcs.empty()) {
        struct DlcDepotData {
            const DepotKeyInfo* dlcDk = nullptr;
            std::vector<const DepotKeyInfo*> subDepots;
            std::vector<const DepotKeyInfo*> associatedDepots;
            bool hasPhysicalDepot = false;
            bool hasAnyManifest = false;
        };
        std::vector<DlcDepotData> dlcDepotList(dlcs.size());
        bool hasDlcManifests = false;

        std::unordered_map<uint32_t, const DepotKeyInfo*> primaryDepots;
        std::unordered_map<uint32_t, std::vector<const DepotKeyInfo*>> subDepotsMap;
        primaryDepots.reserve(depotKeys.size());
        for (const auto& dk : depotKeys) {
            primaryDepots[dk.depotId] = &dk;
            if (dk.dlcId != 0 && dk.dlcId != dk.depotId) {
                subDepotsMap[dk.dlcId].push_back(&dk);
            }
        }

        // Single pass over dlcs to build indexed association table in strictly O(N + M) time
        for (size_t i = 0; i < dlcs.size(); ++i) {
            auto& data = dlcDepotList[i];
            const uint32_t dlcId = dlcs[i].dlcId;
            if (auto it = primaryDepots.find(dlcId); it != primaryDepots.end()) {
                data.dlcDk = it->second;
                data.associatedDepots.push_back(it->second);
            }
            if (auto it = subDepotsMap.find(dlcId); it != subDepotsMap.end()) {
                data.subDepots = it->second;
                data.associatedDepots.insert(data.associatedDepots.end(), it->second.begin(), it->second.end());
            }
            for (const auto* dk : data.associatedDepots) {
                if (IsValidManifestId(dk->manifestId)) {
                    data.hasPhysicalDepot = true;
                    data.hasAnyManifest = true;
                    hasDlcManifests = true;
                }
            }
        }

        std::vector<size_t> validDlcIndices;
        validDlcIndices.reserve(dlcs.size());

        for (size_t i = 0; i < dlcs.size(); ++i) {
            const auto& dlc = dlcs[i];
            const auto& data = dlcDepotList[i];

            if (!data.hasPhysicalDepot && dlc.name.empty()) {
                LOG_DEBUG("OutputWriter", MsgKey::LogOutDroppedOrphanGhostDlc, dlc.dlcId);
                continue;
            }
            validDlcIndices.push_back(i);
        }

        // Sort by companion category, then by AppID
        std::stable_sort(validDlcIndices.begin(), validDlcIndices.end(),
                         [&](size_t a, size_t b) {
                             int catA = GetCategoryOrder(dlcs[a].type);
                             int catB = GetCategoryOrder(dlcs[b].type);
                             if (catA != catB) return catA < catB;
                             return dlcs[a].dlcId < dlcs[b].dlcId;
                         });

        // 3. Owned DLCs & Companions Unlocks
        if (!validDlcIndices.empty()) {
            AppendMajorSection(luaText, "Owned DLCs & Companions");
            std::string_view currentCategory;

            for (size_t idx : validDlcIndices) {
                const auto& dlc = dlcs[idx];
                const auto& data = dlcDepotList[idx];
                const auto categoryTag = GetDlcCategoryTag(dlc.type);

                if (categoryTag != currentCategory) {
                    currentCategory = categoryTag;
                    AppendCategoryHeader(luaText, categoryTag);
                }

                bool missingKey = false;
                for (const auto* dk : data.associatedDepots) {
                    if (IsValidManifestId(dk->manifestId) && dk->hexKey.empty()) {
                        missingKey = true;
                        break;
                    }
                }

                const std::string_view prefix = missingKey ? "-- " : "";
                std::string commentSuffix;
                if (!dlc.name.empty()) {
                    commentSuffix += " -- " + SanitizeComment(dlc.name);
                }
                if (missingKey) {
                    commentSuffix += " -- " + std::string(TR(MsgKey::LuaDlcDepotMissing));
                }

                if (emitTracker.TryEmitAppId(dlc.dlcId)) {
                    if (data.dlcDk && !data.dlcDk->hexKey.empty()) {
                        std::format_to(std::back_inserter(luaText), "{}addappid({}, 1, \"{}\"){}\n", prefix, dlc.dlcId, data.dlcDk->hexKey, commentSuffix);
                    } else {
                        std::format_to(std::back_inserter(luaText), "{}addappid({}, 1){}\n", prefix, dlc.dlcId, commentSuffix);
                    }
                } else {
                    LOG_DEBUG("OutputWriter", MsgKey::LogOutDeduplicatedAppId, dlc.dlcId);
                }

                auto dlcTokIt = appTokens.find(dlc.dlcId);
                if (dlcTokIt != appTokens.end() && dlcTokIt->second != 0 && emitTracker.TryEmitToken(dlc.dlcId)) {
                    std::format_to(std::back_inserter(luaText), "{}addtoken({}, \"{}\")\n", prefix, dlc.dlcId, dlcTokIt->second);
                }

                // Any subdepots of this DLC with keys
                for (const auto* subDk : data.subDepots) {
                    if (!subDk->hexKey.empty()) {
                        if (emitTracker.TryEmitAppId(subDk->depotId)) {
                            std::format_to(std::back_inserter(luaText), "{}addappid({}, 1, \"{}\"){}\n", prefix, subDk->depotId, subDk->hexKey, commentSuffix);
                        } else {
                            LOG_DEBUG("OutputWriter", MsgKey::LogOutDeduplicatedAppId, subDk->depotId);
                        }
                    } else if (missingKey) {
                        if (emitTracker.TryEmitAppId(subDk->depotId)) {
                            std::format_to(std::back_inserter(luaText), "-- addappid({}, 1){}\n", subDk->depotId, commentSuffix);
                        }
                    }
                }
            }
        }

        // 4. DLC & Companion Manifests
        if (hasDlcManifests && !validDlcIndices.empty()) {
            AppendMajorSection(luaText, "DLC & Companion Manifests");
            std::string_view currentCategory;

            for (size_t idx : validDlcIndices) {
                const auto& dlc = dlcs[idx];
                const auto& data = dlcDepotList[idx];
                if (!data.hasAnyManifest) continue;

                const auto categoryTag = GetDlcCategoryTag(dlc.type);
                if (categoryTag != currentCategory) {
                    currentCategory = categoryTag;
                    AppendCategoryHeader(luaText, categoryTag);
                }

                auto emitManifest = [&](const DepotKeyInfo* dk) {
                    if (!dk || !IsValidManifestId(dk->manifestId) || !emitTracker.TryEmitManifest(dk->depotId)) {
                        return;
                    }
                    const bool hasKey = !dk->hexKey.empty();
                    const bool onDisk = hasManifestOnDisk(dk->depotId, dk->manifestId);
                    if (hasKey && onDisk) {
                        if (!dlc.name.empty()) {
                            std::format_to(std::back_inserter(luaText), "setManifestid({}, \"{}\") -- {}\n", dk->depotId, dk->manifestId, SanitizeComment(dlc.name));
                        } else {
                            std::format_to(std::back_inserter(luaText), "setManifestid({}, \"{}\")\n", dk->depotId, dk->manifestId);
                        }
                    } else if (!onDisk) {
                        if (!dlc.name.empty()) {
                            std::format_to(std::back_inserter(luaText), "-- setManifestid({}, \"{}\") -- {} -- {}\n", dk->depotId, dk->manifestId, TR(MsgKey::LuaManifestMissing), SanitizeComment(dlc.name));
                        } else {
                            std::format_to(std::back_inserter(luaText), "-- setManifestid({}, \"{}\") -- {}\n", dk->depotId, dk->manifestId, TR(MsgKey::LuaManifestMissing));
                        }
                    } else {
                        if (!dlc.name.empty()) {
                            std::format_to(std::back_inserter(luaText), "-- setManifestid({}, \"{}\") -- {} -- {}\n", dk->depotId, dk->manifestId, TR(MsgKey::LuaDepotKeyMissing), SanitizeComment(dlc.name));
                        } else {
                            std::format_to(std::back_inserter(luaText), "-- setManifestid({}, \"{}\") -- {}\n", dk->depotId, dk->manifestId, TR(MsgKey::LuaDepotKeyMissing));
                        }
                    }
                };

                // DLC itself manifest
                if (data.dlcDk) {
                    emitManifest(data.dlcDk);
                }

                // Any subdepots of this DLC with manifests
                for (const auto* subDk : data.subDepots) {
                    emitManifest(subDk);
                }
            }
        }
    }

    // 5. App Tickets (at the very end of the file)
    const bool hasOwnership = (ownership && !ownership->empty());
    const bool hasEncrypted = (encrypted && !encrypted->empty());

    AppendMajorSection(luaText, "App Ownership Ticket (AppTicket)");
    if (hasOwnership) {
        std::format_to(std::back_inserter(luaText), "setAppTicket({}, \"{}\")\n",
                       appId,
                       ToHexString(*ownership));
    } else {
        std::format_to(std::back_inserter(luaText), "-- setAppTicket({}, \"null\") -- {}\n", appId, TR(MsgKey::LuaAppTicketMissing));
    }

    AppendMajorSection(luaText, "Encrypted App Ticket (ETicket)");
    if (hasEncrypted) {
        std::format_to(std::back_inserter(luaText), "setETicket({}, \"{}\")\n", appId, ToHexString(*encrypted));
    } else {
        std::format_to(std::back_inserter(luaText), "-- setETicket({}, \"null\") -- {}\n", appId, TR(MsgKey::LuaETicketNote));
    }

    const std::filesystem::path luaPath = dirPath / (std::to_string(appId) + ".lua");
    if (!WriteTextFileAtomic(luaPath, luaText, true)) {
        const std::string luaPathUtf8 = PathToUtf8(luaPath);
        if (!TuiEngine::IsActive()) {
            std::cerr << TR_FMT(MsgKey::OutWriteFileFailed, luaPathUtf8) << "\n";
        }
        LOG_ERROR("OutputWriter", MsgKey::LogOutWriteLuaFailed, luaPathUtf8);
        ok = false;
    }

    const std::string luaPathUtf8 = PathToUtf8(luaPath);

    if (!TuiEngine::IsActive()) {
        std::string fileList = std::to_string(appId) + ".lua, tickets.txt";
        if (ownership && !ownership->empty()) fileList += ", appticket.bin";
        if (encrypted && !encrypted->empty()) fileList += ", eticket.bin";
        for (const auto& mName : copiedManifests) {
            fileList += ", " + mName;
        }
        std::cout << TR_FMT(MsgKey::OutWroteSummary, dir, fileList) << "\n";

        if (!dlcs.empty()) {
            std::cout << TR_FMT(MsgKey::OutExtractedDlcs, dlcs.size()) << "\n";
            for (const auto& dlc : dlcs) {
                std::cout << "       DLC " << dlc.dlcId;
                if (!dlc.name.empty()) {
                    std::cout << ": " << dlc.name;
                }
                std::cout << "\n";
            }
        } else {
            std::cout << TR_FMT(MsgKey::OutNoDlcs, appId) << "\n";
        }

        const size_t keyCount = std::count_if(depotKeys.begin(), depotKeys.end(), [](const DepotKeyInfo& dk) {
            return !dk.hexKey.empty();
        });
        if (keyCount > 0) {
            std::cout << TR_FMT(MsgKey::OutExtractedDepotKeys, keyCount) << "\n";
            for (const auto& dk : depotKeys) {
                if (!dk.hexKey.empty()) {
                    std::cout << "       Depot " << dk.depotId << ": " << dk.hexKey << "\n";
                }
            }
        } else {
            std::cout << TR_FMT(MsgKey::OutNoDepotKeys, appId) << "\n";
            std::cout << TR(MsgKey::OutDepotKeysTip) << "\n";
        }

        if (!copiedManifests.empty()) {
            std::cout << TR_FMT(MsgKey::OutExtractedManifests, copiedManifests.size()) << "\n";
            for (const auto& mName : copiedManifests) {
                std::cout << "       " << mName << "\n";
            }
        } else {
            std::cout << TR_FMT(MsgKey::OutNoManifests, appId) << "\n";
        }

        if (!relevantTokens.empty()) {
            std::cout << TR_FMT(MsgKey::OutExtractedTokens, relevantTokens.size()) << "\n";
            for (const auto& [tId, tVal] : relevantTokens) {
                std::cout << "       AppID " << tId << ": " << tVal << "\n";
            }
        } else if (isOnlineMode) {
            std::cout << TR(MsgKey::OutPublicProductTokenZero) << "\n";
        } else {
            std::cout << TR_FMT(MsgKey::OutNoAccessTokenLocal, appId) << "\n";
        }

        std::cout << TR_FMT(MsgKey::OutConfigSaved, luaPathUtf8) << "\n";
    }

    const size_t keyCount = std::count_if(depotKeys.begin(), depotKeys.end(), [](const DepotKeyInfo& dk) {
        return !dk.hexKey.empty();
    });
    LOG_INFO("OutputWriter", MsgKey::LogOutFilesWritten,
             dir, dlcs.size(), keyCount, copiedManifests.size(), relevantTokens.size());
    LOG_INFO("OutputWriter", MsgKey::LogOutLuaSaved, luaPathUtf8);
    return ok;
}

} // namespace OST::ExtractTickets
