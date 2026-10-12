#include "SteamAuthService.h"
#include "AppInfoParser.h"
#include "I18n.h"
#include "Log.h"
#include "SteamCmClient.h"
#include "TuiEngine.h"
#include "Utils.h"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <unordered_set>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <conio.h>
#endif

namespace OST::ExtractTickets {

namespace {
    std::string UrlEncode(std::string_view str) {
        std::ostringstream escaped;
        escaped.fill('0');
        escaped << std::hex;

        for (char c : str) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
                escaped << c;
            } else {
                escaped << '%' << std::setw(2) << std::uppercase
                        << static_cast<int>(static_cast<unsigned char>(c));
            }
        }
        return escaped.str();
    }

    SteamLoginResult RunDualTrack2FaLoop(
        SteamAuthService& authService,
        const SteamAuthSession& session,
        std::string_view accountName,
        bool hasDeviceCode,
        bool hasDeviceConfirmation,
        bool hasEmailCode,
        std::string_view emailAddress) {

        SteamLoginResult result;
        result.accountName = accountName;
        result.steamId = session.steamId;

        int w = 80, h = 25;
        TuiEngine::GetScreenSize(w, h);
        const int modalW = std::clamp(w - 12, 64, 84);
        const int modalH = 10;
        const int top = (h - modalH) / 2;
        const int left = (w - modalW) / 2;
        const size_t innerW = static_cast<size_t>(modalW - 8);

        std::string title = hasEmailCode
            ? std::string(TR(MsgKey::GuardEmailTitle))
            : std::string(TR(MsgKey::GuardMobileTitle));

        std::string prompt;
        if (hasEmailCode) {
            prompt = emailAddress.empty()
                ? std::string(TR(MsgKey::GuardEmailTitle))
                : TR_FMT(MsgKey::GuardEmailPrompt, MaskEmail(emailAddress));
        } else if (hasDeviceConfirmation || hasDeviceCode) {
            prompt = std::string(TR(MsgKey::GuardMobileDualPrompt));
        } else {
            prompt = std::string(TR(MsgKey::GuardMobilePrompt));
        }

        TuiEngine::ClearScreen();
        TuiEngine::DrawBox(top, left, modalW, modalH, title);
        TuiEngine::PrintBounded(top + 2, left + 4, prompt, innerW, "\x1b[1;37m");

        TuiEngine::DrawFooter(TR(MsgKey::Guard2FaFooter));

        std::string code;
        std::string statusMsg;
        std::string statusStyle = "\x1b[90m";
        bool inputAllowed = true;

        int elapsedMs = 0;
        int pollTimerMs = 0;
        constexpr int kMaxDurationMs = 120000; // 2 minutes
        constexpr int kPollIntervalMs = 1500;  // 1.5s

        auto drawInputAndStatus = [&]() {
            if (inputAllowed) {
                TuiEngine::MoveCursor(top + 4, left + 4);
                std::string displayCode = std::format("[ {:<8} ]", code + "_");
                std::cout << "\x1b[1;30;47m" << displayCode << "\x1b[0m";
            }
            TuiEngine::MoveCursor(top + 6, left + 4);
            std::string currentStatus = statusMsg;
            if (currentStatus.empty()) {
                if (hasDeviceConfirmation) {
                    currentStatus = TR_FMT(MsgKey::Guard2FaWaitingMobile, elapsedMs / 1000, kMaxDurationMs / 1000);
                    statusStyle = "\x1b[90m";
                } else {
                    currentStatus = std::string(TR(MsgKey::Guard2FaEnterCodeHint));
                    statusStyle = "\x1b[90m";
                }
            }
            std::cout << statusStyle << TuiEngine::Pad(TuiEngine::TruncateToWidth(currentStatus, innerW), innerW) << "\x1b[0m";
            std::cout.flush();
        };

        drawInputAndStatus();

        while (elapsedMs < kMaxDurationMs) {
            auto keyOpt = TuiEngine::PollKey(50);
            elapsedMs += 50;
            pollTimerMs += 50;

            if (keyOpt) {
                KeyEvent ev = *keyOpt;
                if (ev.code == KeyCode::Escape) {
                    result.cancelled = true;
                    result.errorMessage = std::string(TR(MsgKey::ErrUserCancelledInput));
                    LOG_INFO("SteamAuth", MsgKey::LogAuthEscCancel2Fa);
                    return result;
                }

                if (inputAllowed) {
                    if (ev.code == KeyCode::Backspace) {
                        if (!code.empty()) {
                            code.pop_back();
                            statusMsg.clear();
                            drawInputAndStatus();
                        }
                    } else if (ev.code == KeyCode::Char) {
                        std::string toAdd = !ev.text.empty() ? ev.text : (ev.ch != 0 ? std::string(1, ev.ch) : "");
                        bool changed = false;
                        for (char c : toAdd) {
                            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                                if (code.size() < 8) {
                                    code.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
                                    changed = true;
                                }
                            }
                        }
                        if (changed) {
                            statusMsg.clear();
                            drawInputAndStatus();
                        }
                    } else if (ev.code == KeyCode::Enter) {
                        if (!code.empty()) {
                            statusMsg = std::string(TR(MsgKey::Guard2FaSubmitting));
                            statusStyle = "\x1b[1;33m";
                            drawInputAndStatus();

                            int codeType = hasEmailCode ? 2 : 3;
                            if (authService.SubmitSteamGuardCode(session, code, codeType)) {
                                auto pollRes = authService.PollAuthSessionOnce(session, 2500);
                                if (pollRes) {
                                    result.success = true;
                                    result.refreshToken = std::move(pollRes->first);
                                    result.accessToken = std::move(pollRes->second);
                                    SecureWipeString(pollRes->first);
                                    SecureWipeString(pollRes->second);
                                    LOG_INFO("SteamAuth", MsgKey::LogAuth2FaCodeVerified);
                                    return result;
                                }
                            }
                            code.clear();
                            statusMsg = std::string(TR(MsgKey::GuardCodeIncorrect));
                            statusStyle = "\x1b[1;31m";
                            drawInputAndStatus();
                        } else if (hasDeviceConfirmation) {
                            statusMsg = std::string(TR(MsgKey::Guard2FaCheckingMobile));
                            statusStyle = "\x1b[1;36m";
                            drawInputAndStatus();

                            auto pollRes = authService.PollAuthSessionOnce(session, 2500);
                            if (pollRes) {
                                LOG_INFO("SteamAuth", MsgKey::LogAuthMobileApprovedEnter);
                                result.success = true;
                                result.refreshToken = std::move(pollRes->first);
                                result.accessToken = std::move(pollRes->second);
                                SecureWipeString(pollRes->first);
                                SecureWipeString(pollRes->second);
                                return result;
                            }
                            statusMsg = std::string(TR(MsgKey::Guard2FaNotApprovedYet));
                            statusStyle = "\x1b[1;33m";
                            drawInputAndStatus();
                        }
                    }
                }
            }

            if (hasDeviceConfirmation && pollTimerMs >= kPollIntervalMs) {
                pollTimerMs = 0;
                drawInputAndStatus();

                auto pollRes = authService.PollAuthSessionOnce(session, 2500);
                if (pollRes) {
                    LOG_INFO("SteamAuth", MsgKey::LogAuthMobileApprovedZeroKey);
                    result.success = true;
                    result.refreshToken = std::move(pollRes->first);
                    result.accessToken = std::move(pollRes->second);
                    SecureWipeString(pollRes->first);
                    SecureWipeString(pollRes->second);
                    return result;
                }
            }
        }

        result.errorMessage = std::string(TR(MsgKey::ErrAuthTimeoutOrCancelled));
        LOG_WARN("SteamAuth", "{}", result.errorMessage);
        return result;
    }
} // namespace

SteamAuthService::SteamAuthService() = default;

std::optional<SteamRsaKey> SteamAuthService::GetPasswordRsaKey(std::string_view accountName) {
    std::string url = "https://api.steampowered.com/IAuthenticationService/GetPasswordRSAPublicKey/v1?account_name=" + UrlEncode(accountName);
    LOG_DEBUG("SteamAuth", MsgKey::LogAuthRequestingRsa, MaskAccount(accountName));

    HttpResponse resp = m_http.Get(url);
    if (!resp.IsSuccess()) {
        LOG_ERROR("SteamAuth", MsgKey::LogAuthGetRsaFailed, resp.statusCode, resp.errorMessage);
        return std::nullopt;
    }

    auto modOpt = JsonHelper::GetString(resp.body, "publickey_mod");
    auto expOpt = JsonHelper::GetString(resp.body, "publickey_exp");
    auto timeOpt = JsonHelper::GetUInt64(resp.body, "timestamp");

    if (!modOpt || !expOpt || !timeOpt) {
        LOG_ERROR("SteamAuth", MsgKey::LogAuthParseRsaFailed);
        return std::nullopt;
    }

    SteamRsaKey rsaKey;
    rsaKey.modHex = *modOpt;
    rsaKey.expHex = *expOpt;
    rsaKey.timestamp = *timeOpt;
    LOG_DEBUG("SteamAuth", MsgKey::LogAuthGetRsaSuccess, rsaKey.timestamp);
    return rsaKey;
}

std::optional<SteamAuthSession> SteamAuthService::BeginAuthSession(
    std::string_view accountName,
    const SecureString& password,
    const SteamRsaKey& rsaKey) {

    std::string encryptedPassword = EncryptPasswordWithRSA(password, rsaKey.modHex, rsaKey.expHex);
    if (encryptedPassword.empty()) {
        LOG_ERROR("SteamAuth", MsgKey::LogAuthRsaEncryptFailed);
        return std::nullopt;
    }

    std::string deviceName = "OpenSteamTool";
#if defined(_WIN32)
    char compName[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD compLen = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameA(compName, &compLen) && compLen > 0) {
        deviceName.assign(compName, compLen);
    }
#endif

    std::string postData = "account_name=" + UrlEncode(accountName) +
                           "&encrypted_password=" + UrlEncode(encryptedPassword) +
                           "&encryption_timestamp=" + std::to_string(rsaKey.timestamp) +
                           "&remember_login=true&platform_type=1&persistence=1&website_id=Client" +
                           "&device_friendly_name=" + UrlEncode(deviceName);

    std::string url = "https://api.steampowered.com/IAuthenticationService/BeginAuthSessionViaCredentials/v1";
    LOG_DEBUG("SteamAuth", MsgKey::LogAuthBeginSession, MaskAccount(accountName));

    HttpResponse resp = m_http.Post(url, postData, "application/x-www-form-urlencoded");
    if (!resp.IsSuccess()) {
        LOG_ERROR("SteamAuth", MsgKey::LogAuthBeginSessionHttpFailed, resp.statusCode, resp.errorMessage);
        return std::nullopt;
    }

    auto clientIdOpt = JsonHelper::GetString(resp.body, "client_id");
    auto requestIdOpt = JsonHelper::GetString(resp.body, "request_id");
    auto steamIdOpt = JsonHelper::GetUInt64(resp.body, "steamid");

    if (!clientIdOpt || !requestIdOpt) {
        LOG_ERROR("SteamAuth", MsgKey::LogAuthMissingClientOrReqId, resp.statusCode);
        return std::nullopt;
    }

    SteamAuthSession session;
    session.clientId = *clientIdOpt;
    session.requestId = *requestIdOpt;
    session.steamId = steamIdOpt.value_or(0);
    session.allowedConfirmations = JsonHelper::GetConfirmations(resp.body);

    LOG_DEBUG("SteamAuth", MsgKey::LogAuthSessionCreated,
              MaskSteamId(session.steamId), session.allowedConfirmations.size());
    return session;
}

bool SteamAuthService::SubmitSteamGuardCode(
    const SteamAuthSession& session,
    std::string_view code,
    int codeType) {

    std::string postData = "client_id=" + UrlEncode(session.clientId) +
                           "&steamid=" + std::to_string(session.steamId) +
                           "&code=" + UrlEncode(code) +
                           "&code_type=" + std::to_string(codeType);

    std::string url = "https://api.steampowered.com/IAuthenticationService/UpdateAuthSessionWithSteamGuardCode/v1";
    LOG_DEBUG("SteamAuth", MsgKey::LogAuthSubmitting2Fa, codeType);

    HttpResponse resp = m_http.Post(url, postData, "application/x-www-form-urlencoded", {}, 5000);
    if (!resp.IsSuccess()) {
        LOG_WARN("SteamAuth", MsgKey::LogAuthSubmit2FaHttpFailed, resp.statusCode, resp.errorMessage);
        return false;
    }

    LOG_DEBUG("SteamAuth", MsgKey::LogAuthSubmit2FaSuccess);
    return true;
}

std::optional<std::pair<std::string, std::string>> SteamAuthService::PollAuthSessionOnce(
    const SteamAuthSession& session,
    int timeoutMs) {

    std::string postData = "client_id=" + UrlEncode(session.clientId) +
                           "&request_id=" + UrlEncode(session.requestId);
    std::string url = "https://api.steampowered.com/IAuthenticationService/PollAuthSessionStatus/v1";

    HttpResponse resp = m_http.Post(url, postData, "application/x-www-form-urlencoded", {}, timeoutMs);
    if (!resp.IsSuccess()) {
        SecureWipeString(resp.body);
        return std::nullopt;
    }

    auto refreshOpt = JsonHelper::GetString(resp.body, "refresh_token");
    auto accessOpt = JsonHelper::GetString(resp.body, "access_token");
    SecureWipeString(resp.body);

    if (refreshOpt && !refreshOpt->empty()) {
        std::pair<std::string, std::string> out{std::move(*refreshOpt), accessOpt ? std::move(*accessOpt) : std::string{}};
        SecureWipeString(*refreshOpt);
        if (accessOpt) SecureWipeString(*accessOpt);
        return out;
    }
    if (refreshOpt) SecureWipeString(*refreshOpt);
    if (accessOpt) SecureWipeString(*accessOpt);
    return std::nullopt;
}

SteamLoginResult SteamAuthService::PollAuthSession(
    const SteamAuthSession& session,
    std::string_view accountName,
    int maxAttempts,
    int delayMs) {

    SteamLoginResult result;
    result.accountName = accountName;
    result.steamId = session.steamId;

    if (TuiEngine::IsActive()) {
        int w = 80, h = 25;
        TuiEngine::GetScreenSize(w, h);
        const int modalW = std::clamp(w - 12, 60, 80);
        const int modalH = 6;
        const int top = (h - modalH) / 2, left = (w - modalW) / 2;
        TuiEngine::DrawBox(top, left, modalW, modalH, TR(MsgKey::ConnectingTitle));
        TuiEngine::MoveCursor(top + 3, left + 4);
        std::cout << "\x1b[90m" << TR(MsgKey::GuardDeviceDetail) << "\x1b[0m";
        std::cout.flush();
    }

    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        if (TuiEngine::IsActive()) {
            int w = 80, h = 25;
            TuiEngine::GetScreenSize(w, h);
            const int modalW = std::clamp(w - 12, 60, 80);
            const int modalH = 6;
            const int top = (h - modalH) / 2, left = (w - modalW) / 2;
            std::string pollMsg = std::format("{} ({}/{}s)", TR(MsgKey::ConnectingGatewayMsg), (attempt + 1) * delayMs / 1000, (maxAttempts * delayMs) / 1000);
            TuiEngine::PrintBounded(top + 2, left + 4, pollMsg, static_cast<size_t>(modalW - 8), "\x1b[1;36m");
            std::cout.flush();
        }

        constexpr int sliceMs = 50;
        const int slices = delayMs / sliceMs;
        for (int s = 0; s < slices; ++s) {
            auto keyOpt = TuiEngine::PollKey(sliceMs);
            if (keyOpt && keyOpt->code == KeyCode::Escape) {
                TuiEngine::FlushInputBuffer();
                result.cancelled = true;
                result.errorMessage = std::string(TR(MsgKey::ErrUserCancelledPoll));
                LOG_INFO("SteamAuth", MsgKey::LogAuthEscCancelPoll);
                return result;
            }
        }

        auto tokens = PollAuthSessionOnce(session, 2500);
        if (tokens) {
            result.success = true;
            result.refreshToken = std::move(tokens->first);
            result.accessToken = std::move(tokens->second);
            SecureWipeString(tokens->first);
            SecureWipeString(tokens->second);
            LOG_DEBUG("SteamAuth", MsgKey::LogAuthPollSuccess,
                      MaskToken(result.refreshToken), MaskToken(result.accessToken));
            return result;
        }
    }

    result.errorMessage = std::string(TR(MsgKey::ErrAuthTimeoutOrCancelled));
    LOG_WARN("SteamAuth", "{}", result.errorMessage);
    return result;
}

std::optional<std::string> SteamAuthService::RefreshAccessToken(
    uint64_t steamId,
    std::string_view refreshToken,
    std::string* outNewRefreshToken) {

    if (outNewRefreshToken) {
        SecureWipeString(*outNewRefreshToken);
    }

    if (steamId == 0) {
        steamId = GetJwtSteamId(refreshToken);
    }

    std::string postData = "refresh_token=" + UrlEncode(refreshToken) +
                           "&steamid=" + std::to_string(steamId) +
                           "&renewal_type=1";

    std::string url = "https://api.steampowered.com/IAuthenticationService/GenerateAccessTokenForApp/v1";
    LOG_DEBUG("SteamAuth", MsgKey::LogAuthRefreshingAccess, MaskSteamId(steamId));

    HttpResponse resp = m_http.Post(url, postData, "application/x-www-form-urlencoded");
    SecureWipeString(postData);
    if (!resp.IsSuccess()) {
        LOG_WARN("SteamAuth", MsgKey::LogAuthRefreshHttpFailed, resp.statusCode, resp.errorMessage);
        SecureWipeString(resp.body);
        return std::nullopt;
    }

    LOG_DEBUG("SteamAuth", MsgKey::LogAuthGenAccessHttpOk, resp.statusCode);

    auto accessOpt = JsonHelper::GetString(resp.body, "access_token");
    auto refreshOpt = JsonHelper::GetString(resp.body, "refresh_token");
    SecureWipeString(resp.body);

    if (refreshOpt && !refreshOpt->empty()) {
        if (outNewRefreshToken) {
            *outNewRefreshToken = std::move(*refreshOpt);
        }
        SecureWipeString(*refreshOpt);
    }

    if (accessOpt && !accessOpt->empty()) {
        LOG_DEBUG("SteamAuth", MsgKey::LogAuthRefreshSuccess, MaskToken(*accessOpt));
        return accessOpt;
    }

    LOG_WARN("SteamAuth", MsgKey::LogAuthNoAccessInResp, resp.statusCode);
    return std::nullopt;
}

std::vector<OwnedGameInfo> SteamAuthService::FetchOwnedGames(
    uint64_t steamId,
    std::string_view accessToken,
    SteamCmClient* cmClient) {

    std::string url = "https://api.steampowered.com/IPlayerService/GetOwnedGames/v1/?access_token=" +
                      UrlEncode(accessToken) +
                      "&steamid=" + std::to_string(steamId) +
                      "&include_appinfo=1&include_played_free_games=0&language=" +
                      std::string(I18n::GetSteamLanguageCode());

    LOG_DEBUG("SteamAuth", MsgKey::LogAuthFetchingOwnedGames);
    HttpResponse resp = m_http.Get(url);

    std::vector<OwnedGameInfo> games;
    if (resp.IsSuccess()) {
        games = JsonHelper::ParseOwnedGames(resp.body);
        LOG_DEBUG("SteamAuth", MsgKey::LogAuthFetchedOwnedGames, games.size());
    } else {
        LOG_WARN("SteamAuth", MsgKey::LogAuthFetchOwnedGamesFailed, resp.statusCode, resp.errorMessage);
    }

    // 查询当前账号的 Steam 家庭组 (IFamilyGroupsService)
    std::string familyUrl = "https://api.steampowered.com/IFamilyGroupsService/GetFamilyGroupForUser/v1/?access_token=" +
                            UrlEncode(accessToken) +
                            "&include_family_group_response=true";
    HttpResponse famResp = m_http.Get(familyUrl);
    if (famResp.IsSuccess()) {
        auto familyGroupIdOpt = JsonHelper::GetString(famResp.body, "family_groupid");
        if (!familyGroupIdOpt || familyGroupIdOpt->empty() || *familyGroupIdOpt == "0") {
            auto famGroupNum = JsonHelper::GetUInt64(famResp.body, "family_groupid");
            if (famGroupNum && *famGroupNum > 0) {
                familyGroupIdOpt = std::to_string(*famGroupNum);
            }
        }

        if (familyGroupIdOpt && !familyGroupIdOpt->empty() && *familyGroupIdOpt != "0") {
            LOG_INFO("SteamAuth", MsgKey::LogAuthDetectedFamilyGroup, MaskGroupId(*familyGroupIdOpt));
            std::string sharedUrl = "https://api.steampowered.com/IFamilyGroupsService/GetSharedLibraryApps/v1/?access_token=" +
                                    UrlEncode(accessToken) +
                                    "&family_groupid=" + *familyGroupIdOpt +
                                    "&include_own=false&include_non_games=false&language=" +
                                    std::string(I18n::GetSteamLanguageCode());
            HttpResponse sharedResp = m_http.Get(sharedUrl);
            if (sharedResp.IsSuccess()) {
                auto sharedGames = JsonHelper::ParseSharedLibraryApps(sharedResp.body);
                LOG_INFO("SteamAuth", MsgKey::LogAuthFoundSharedGames, sharedGames.size());

                std::unordered_set<uint32_t> existing;
                for (const auto& g : games) {
                    existing.insert(g.appId);
                }

                // Collect all shared game AppIDs not already owned directly
                std::unordered_set<uint32_t> sharedAppIds;
                std::unordered_set<uint32_t> missingNameAppIds;
                for (const auto& sg : sharedGames) {
                    if (!existing.contains(sg.appId)) {
                        sharedAppIds.insert(sg.appId);
                        if (sg.name.empty()) {
                            missingNameAppIds.insert(sg.appId);
                        }
                    }
                }

                std::unordered_map<uint32_t, std::string> resolvedNames;
                if (!sharedAppIds.empty()) {
                    auto steamPathOpt = FindSteamInstallPath();
                    if (steamPathOpt && !steamPathOpt->empty()) {
                        // Fast local appinfo.vdf lookup (<1ms) prioritizing name_localized for all shared games
                        resolvedNames = ParseAppNames(*steamPathOpt, sharedAppIds);
                    }

                    std::vector<uint32_t> stillMissing;
                    for (uint32_t mId : missingNameAppIds) {
                        if (!resolvedNames.contains(mId) || resolvedNames[mId].empty()) {
                            stillMissing.push_back(mId);
                        }
                    }

                    // 1. High-speed batch PICS resolution via CM Client (50 apps/batch, ~100ms, immune to Store HTTP 429)
                    if (!stillMissing.empty() && cmClient && cmClient->EnsureConnected()) {
                        LOG_INFO("SteamAuth", MsgKey::LogAuthBatchResolvingSharedNames, stillMissing.size());
                        auto picsNames = cmClient->RequestPicsAppNames(stillMissing);
                        for (auto& [pAppId, pName] : picsNames) {
                            if (!pName.empty()) {
                                resolvedNames[pAppId] = std::move(pName);
                            }
                        }
                    } else if (!stillMissing.empty()) {
                        // 2. Safe fallback to Store WebAPI only if CM Client unavailable, with rate limit cap
                        size_t fallbackCount = 0;
                        for (uint32_t mId : stillMissing) {
                            if (resolvedNames.contains(mId) && !resolvedNames[mId].empty()) continue;
                            if (++fallbackCount > 10) break; // Hard cap to prevent UI freeze and HTTP 429 flood
                            std::string storeUrl = std::format("https://store.steampowered.com/api/appdetails?appids={}&filters=basic&l={}",
                                                               mId, I18n::GetSteamLanguageCode());
                            HttpResponse sResp = m_http.Get(storeUrl);
                            if (sResp.IsSuccess()) {
                                auto nameOpt = JsonHelper::GetString(sResp.body, "name");
                                if (nameOpt && !nameOpt->empty()) {
                                    resolvedNames[mId] = std::move(*nameOpt);
                                }
                            }
                        }
                    }
                }

                for (auto& sg : sharedGames) {
                    if (!existing.contains(sg.appId)) {
                        existing.insert(sg.appId);
                        auto it = resolvedNames.find(sg.appId);
                        if (it != resolvedNames.end() && !it->second.empty()) {
                            sg.name = it->second;
                        } else if (sg.name.empty()) {
                            sg.name = "App " + std::to_string(sg.appId);
                        }
                        sg.isShared = true;
                        games.push_back(std::move(sg));
                    }
                }
            } else {
                LOG_WARN("SteamAuth", MsgKey::LogAuthFetchSharedGamesFailed, sharedResp.statusCode, sharedResp.errorMessage);
            }
        }
    }

    return games;
}

SteamLoginResult SteamAuthService::LoginWithCredentials(
    std::string_view accountName,
    const SecureString& password) {

    SteamLoginResult failResult;
    failResult.accountName = accountName;

    auto rsaKey = GetPasswordRsaKey(accountName);
    if (!rsaKey) {
        failResult.errorMessage = std::string(TR(MsgKey::ErrRsaKeyFailed));
        LOG_WARN("SteamAuth", "{}", failResult.errorMessage);
        return failResult;
    }

    auto session = BeginAuthSession(accountName, password, *rsaKey);
    if (!session) {
        failResult.errorMessage = std::string(TR(MsgKey::ErrAuthSessionFailed));
        LOG_WARN("SteamAuth", "{}", failResult.errorMessage);
        return failResult;
    }

    bool hasDeviceCode = false;
    bool hasDeviceConfirmation = false;
    bool hasEmailCode = false;
    std::string emailAddress;

    for (const auto& conf : session->allowedConfirmations) {
        LOG_INFO("SteamAuth", MsgKey::LogAuth2FaRequired, conf.type,
                 conf.type == 2 ? MaskEmail(conf.associatedMessage) : conf.associatedMessage);
        if (conf.type == 3) {
            hasDeviceCode = true;
        } else if (conf.type == 4) {
            hasDeviceConfirmation = true;
        } else if (conf.type == 2) {
            hasEmailCode = true;
            if (!conf.associatedMessage.empty()) {
                emailAddress = conf.associatedMessage;
            }
        }
    }

    SteamLoginResult result;
    if (hasDeviceCode || hasDeviceConfirmation || hasEmailCode) {
        if (TuiEngine::IsActive()) {
            result = RunDualTrack2FaLoop(*this, *session, accountName,
                                         hasDeviceCode, hasDeviceConfirmation,
                                         hasEmailCode, emailAddress);
        } else {
            if (hasDeviceCode || hasEmailCode) {
                std::string promptMsg = hasEmailCode
                    ? (emailAddress.empty() ? std::string(TR(MsgKey::GuardEmailTitle)) : TR_FMT(MsgKey::GuardEmailPrompt, MaskEmail(emailAddress)))
                    : std::string(TR(MsgKey::GuardMobilePrompt));
                std::cout << "[2FA] " << promptMsg << " ";
                std::string code;
                std::cin >> code;
                SubmitSteamGuardCode(*session, code, hasDeviceCode ? 3 : 2);
            }
            result = PollAuthSession(*session, accountName);
        }
    } else {
        result = PollAuthSession(*session, accountName);
    }
    if (!result.success) {
        if (!result.cancelled) {
            LOG_WARN("SteamAuth", "{}", result.errorMessage);
        }
        return result;
    }

    if (result.steamId == 0) {
        result.steamId = GetJwtSteamId(result.refreshToken);
    }

    CachedAccount cached;
    cached.accountName = std::string{accountName};
    cached.steamId = result.steamId;
    cached.refreshToken = result.refreshToken;
    cached.accessToken = result.accessToken;
    cached.lastLoginTime = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

    TokenStorage::UpsertAccount(cached);
    return result;
}

} // namespace OST::ExtractTickets
