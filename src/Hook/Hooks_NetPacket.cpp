#include "Hooks_NetPacket.h"
#include "Hooks_Package.h"
#include "Utils/SteamMetadata/ManifestClient.h"
#include "Hooks_Misc.h"
#include "HookMacros.h"
#include "dllmain.h"
#include "Utils/Tickets/AppTicket.h"
#include "Utils/Tickets/EticketClient.h"
#include "Pipe/PipeManager.h"
#include "Pipe/Features/DenuvoAuth/DenuvoSync.h"
#include "Utils/Support/FnvHash.h"
#include "Utils/CloudRedirect/CloudRedirectHost.h"
#include "Utils/Config/LuaConfig.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <future>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "steam_messages.pb.h"

// ════════════════════════════════════════════════════════════════
//  Shared infrastructure
// ════════════════════════════════════════════════════════════════
namespace {

    constexpr uint32 kMaxBodySize   = 65536;
    constexpr uint32 kMaxHdrSize    = 1024;
    constexpr uint32 kMaxPacketSize = 8 + kMaxHdrSize + kMaxBodySize;
    constexpr size_t kPacketPoolSize = 32;

    // ── High-performance, cacheline-aligned ring buffer pool ─────
    template <size_t PoolSize, size_t SlotSize>
    class PacketBufferPool {
        static_assert((PoolSize & (PoolSize - 1)) == 0, "PoolSize must be power of 2");
        alignas(64) uint8 m_storage[PoolSize][SlotSize]{};
        std::atomic<uint32> m_index{0};

    public:
        [[nodiscard]] std::span<uint8> AcquireSlot() noexcept {
            const uint32 idx = m_index.fetch_add(1, std::memory_order_relaxed) & static_cast<uint32>(PoolSize - 1);
            return std::span<uint8>(m_storage[idx], SlotSize);
        }
    };

    static PacketBufferPool<kPacketPoolSize, kMaxPacketSize> g_RecvPool;
    static PacketBufferPool<kPacketPoolSize, kMaxPacketSize> g_SendPool;

    // ── RAII Carrier Scope Guard (Guarantees no-fail restoration) ───
    class [[nodiscard]] CarrierScopeGuard final {
        CNetPacket* const m_carrier;
        uint8* const      m_origPubData;
        const uint32      m_origCubData;

    public:
        CarrierScopeGuard(CNetPacket* carrier, uint8* borrowedData, uint32 borrowedSize) noexcept
            : m_carrier(carrier),
              m_origPubData(carrier ? carrier->m_pubData : nullptr),
              m_origCubData(carrier ? carrier->m_cubData : 0)
        {
            if (carrier) {
                carrier->m_pubData = borrowedData;
                carrier->m_cubData = borrowedSize;
            }
        }

        ~CarrierScopeGuard() noexcept {
            if (m_carrier) {
                m_carrier->m_pubData = m_origPubData;
                m_carrier->m_cubData = m_origCubData;
            }
        }

        CarrierScopeGuard(const CarrierScopeGuard&) = delete;
        CarrierScopeGuard& operator=(const CarrierScopeGuard&) = delete;
        CarrierScopeGuard(CarrierScopeGuard&&) = delete;
        CarrierScopeGuard& operator=(CarrierScopeGuard&&) = delete;
    };

    // ── EMsg -> name lookup ──────────────────────────────────────────
    RESOLVE_FUNC(PchMsgNameFromEMsg, char*, EMsg eMsg);
    [[nodiscard]] inline const char* MsgName(EMsg eMsg) noexcept {
        if (oPchMsgNameFromEMsg) return oPchMsgNameFromEMsg(eMsg);
        return "?";
    }

    // ── Packet layout ──────────────────────────────────────────
    [[nodiscard]] inline bool UnpackRaw(const uint8* data, uint32 size,
                                        EMsg& eMsg, const uint8*& pHdr, uint32& cbHdr,
                                        const uint8*& pBody, uint32& cbBody) noexcept
    {
        if (!data || size < sizeof(MsgHdr)) {
            eMsg   = static_cast<EMsg>(0);
            cbHdr  = 0;
            pHdr   = nullptr;
            pBody  = nullptr;
            cbBody = 0;
            return false;
        }
        const auto* hdr = reinterpret_cast<const MsgHdr*>(data);
        if (!(hdr->eMsg & kMsgHdrProtoFlag)) {
            eMsg   = static_cast<EMsg>(0);
            cbHdr  = 0;
            pHdr   = nullptr;
            pBody  = nullptr;
            cbBody = 0;
            return false;
        }

        eMsg  = static_cast<EMsg>(hdr->eMsg & ~kMsgHdrProtoFlag);
        cbHdr = hdr->headerLength;
        if (cbHdr > size - sizeof(MsgHdr)) {
            eMsg   = static_cast<EMsg>(0);
            cbHdr  = 0;
            pHdr   = nullptr;
            pBody  = nullptr;
            cbBody = 0;
            return false;
        }
        const uint32 off = sizeof(MsgHdr) + cbHdr;
        pHdr   = data + sizeof(MsgHdr);
        pBody  = data + off;
        cbBody = size - off;
        return true;
    }

    // ── Hash constants for target_job_name dispatch ─────────────
    constexpr uint32 HASH_JOB_NotifyRunningApps = Fnv1aHash("FamilyGroupsClient.NotifyRunningApps#1");
    constexpr uint32 HASH_JOB_GetUserStats = Fnv1aHash("Player.GetUserStats#1");
    constexpr uint32 HASH_JOB_GetManifestRequestCode = Fnv1aHash("ContentServerDirectory.GetManifestRequestCode#1");

} // anonymous namespace


// ════════════════════════════════════════════════════════════════
//  Hooks_NetPacket_AccessToken
//
//  Outgoing: CMsgClientPICSProductInfoRequest (eMsg 8903)
// ════════════════════════════════════════════════════════════════
namespace Hooks_NetPacket_AccessToken {

    bool HandleSend(const uint8* pBody, uint32 cbBody,
                    uint8* outBody, uint32 maxBody, uint32& outCbBody)
    {
        CMsgClientPICSProductInfoRequest req;
        if (!req.ParseFromArray(pBody, static_cast<int>(cbBody))) {
            LOG_PICS_WARN("Failed to ParseFromArray CMsgClientPICSProductInfoRequest");
            return false;
        }
        LOG_PICS_DEBUG("CMsgClientPICSProductInfoRequest original body:\n{}", req.DebugString());

        bool needsPatch = false;
        for (const auto& app : req.apps()) {
            if (LuaConfig::HasDepot(app.appid()) && LuaConfig::GetAccessToken(app.appid())) {
                needsPatch = true;
                LOG_PICS_DEBUG("CMsgClientPICSProductInfoRequest: found appid {} with access_token, need patching", app.appid());
                break;
            }
        }
        if (!needsPatch) {
            LOG_PICS_TRACE("CMsgClientPICSProductInfoRequest: no apps need token injection, skip");
            return false;
        }

        int injected = 0, noToken = 0, notAddAppId = 0;
        for (auto& app : *req.mutable_apps()) {
            if (LuaConfig::HasDepot(app.appid())) {
                uint64_t token = LuaConfig::GetAccessToken(app.appid());
                if (token) {
                    LOG_PICS_DEBUG("CMsgClientPICSProductInfoRequest: inject appid={}: {} -> {}", app.appid(),
                               app.has_access_token() ? std::to_string(app.access_token()) : "absent",
                               token);
                    app.set_access_token(token);
                    ++injected;
                } else {
                    LOG_PICS_WARN("CMsgClientPICSProductInfoRequest: skip appid={}: in depot, no token configured", app.appid());
                    ++noToken;
                }
            } else {
                ++notAddAppId;
            }
        }
        LOG_PICS_DEBUG("CMsgClientPICSProductInfoRequest: injected={} no_token={} not_in_add_appid={} total={}",
                   injected, noToken, notAddAppId, req.apps_size());

        outCbBody = static_cast<uint32>(req.ByteSizeLong());
        if (outCbBody > maxBody) {
            LOG_PICS_WARN("CMsgClientPICSProductInfoRequest: encoded size {} exceeds buffer", outCbBody);
            return false;
        }
        if (!req.SerializeToArray(outBody, static_cast<int>(maxBody))) {
            LOG_PICS_WARN("CMsgClientPICSProductInfoRequest: Failed to encode modified request");
            return false;
        }

        LOG_PICS_DEBUG("CMsgClientPICSProductInfoRequest: modified body: {}", req.DebugString());
        return true;
    }

} // namespace Hooks_NetPacket_AccessToken


// ════════════════════════════════════════════════════════════════
//  Hooks_NetPacket_UserStats
//
//  Outgoing: CPlayer_GetUserStats_Request  (eMsg 151 -> target: Player.GetUserStats#1)
//            CMsgClientGetUserStats        (eMsg 818)
//  Incoming: CPlayer_GetUserStats_Response (eMsg 147 ← target: Player.GetUserStats#1)
//            CMsgClientGetUserStatsResponse(eMsg 819)
// ════════════════════════════════════════════════════════════════
namespace Hooks_NetPacket_UserStats {

    struct PendingStatsJob {
        AppId_t     appId = 0;
        std::string shaSchema;
    };

    // jobid_source -> PendingStatsJob mapping (eMsg 151 request -> eMsg 147 response)
    std::mutex g_JobMutex;
    std::unordered_map<uint64, PendingStatsJob> g_JobIdToPendingStats;

    // ── Send: CPlayer_GetUserStats_Request (eMsg 151) ──────────
    bool HandleSend_GetUserStats(const uint8* pBody, uint32 cbBody,
                                 const CMsgProtoBufHeader& hdr,
                                 uint8* outBody, uint32 maxBody, uint32& outCbBody)
    {
        CPlayer_GetUserStats_Request req;
        if (!req.ParseFromArray(pBody, static_cast<int>(cbBody))) {
            LOG_ACHIEVEMENT_WARN("Player::GetUserStats request: failed to ParseFromArray");
            return false;
        }
        if (!req.has_appid()) {
            LOG_ACHIEVEMENT_WARN("Player::GetUserStats request: missing appid");
            return false;
        }

        LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats request: original body:\n{}", req.DebugString());

        AppId_t appId = req.appid();
        const bool isOnlineFix = (appId == kOnlineFixAppId && Hooks_Misc::IsOnlineFixActive());
        if (isOnlineFix) {
            const AppId_t resolved = Hooks_Misc::ResolveAppId();
            if (resolved != 0) {
                appId = resolved;
                req.set_appid(appId);
            }
        }

        // Truly owned and family shared games must pass through to official servers untouched
        if (Hooks_Package::HasValidLicense(appId)) {
            return false;
        }

        // Only manage apps that are in LuaConfig depot or CloudRedirect
        if (!LuaConfig::HasDepot(appId) && !CloudRedirectHost::IsApp(appId)) {
            LOG_ACHIEVEMENT_WARN("Player::GetUserStats request: appid={} is not managed", appId);
            return false;
        }

        const bool hasShaSchema = req.has_sha_schema() && !req.sha_schema().empty();
        const std::string shaSchema = hasShaSchema ? req.sha_schema() : "";

        // Save jobid_source -> PendingStatsJob for the response handler
        if (hdr.has_jobid_source()) {
            uint64 jobId = hdr.jobid_source();
            std::lock_guard<std::mutex> lock(g_JobMutex);
            if (g_JobIdToPendingStats.size() >= 256) {
                g_JobIdToPendingStats.erase(g_JobIdToPendingStats.begin());
            }
            g_JobIdToPendingStats[jobId] = {appId, shaSchema};
            LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats request: stored jobid={} -> appid={}, sha_len={}",
                jobId, appId, shaSchema.size());
        }

        // When sha_schema is present, the client already has the schema definitions cached locally.
        // DO NOT spoof steamid! Spoofing steamid causes Valve to return AccessDenied with an empty body,
        // which strips the schema and causes achievements to disappear entirely in the UI.
        if (hasShaSchema) {
            if (isOnlineFix) {
                outCbBody = static_cast<uint32>(req.ByteSizeLong());
                if (outCbBody <= maxBody && req.SerializeToArray(outBody, static_cast<int>(maxBody))) {
                    return true;
                }
            }
            LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats request: sha_schema is present, preserve client request");
            return false;
        }

        // First time retrieval without sha_schema: borrow donor steamid to fetch full schema from Valve
        uint64_t newSteamId = LuaConfig::GetStatSteamId(appId);
        if (newSteamId != 0) {
            req.set_steamid(newSteamId);
        }

        outCbBody = static_cast<uint32>(req.ByteSizeLong());
        if (outCbBody > maxBody || !req.SerializeToArray(outBody, static_cast<int>(maxBody))) {
            LOG_ACHIEVEMENT_WARN("Player::GetUserStats request: failed to encode");
            return false;
        }

        LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats request: modified body:\n{}", req.DebugString());
        return true;
    }

    // ── Recv: CPlayer_GetUserStats_Response (eMsg 147) ─────────
    //     Header: set eresult=OK. Body: strip donor stats, overlay CR achievements.
    void HandleRecv_GetUserStatsResponse(const CMsgProtoBufHeader& inHdr,
                                         const uint8* pBody, uint32 cbBody,
                                         uint8* outHdr, uint32 maxHdr, uint32& outCbHdr, bool& outNeedHdr,
                                         uint8* outBody, uint32 maxBody, uint32& outCbBody, bool& outNeedBody)
    {
        LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats response: original header:\n{}", inHdr.DebugString());

        // Look up pending stats info via jobid_target -> jobid_source match
        PendingStatsJob pending;
        bool hasPending = false;
        if (inHdr.has_jobid_target()) {
            uint64 jobId = inHdr.jobid_target();
            std::lock_guard<std::mutex> lock(g_JobMutex);
            auto it = g_JobIdToPendingStats.find(jobId);
            if (it != g_JobIdToPendingStats.end()) {
                pending = it->second;
                hasPending = true;
                LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats response: matched jobid={} -> appid={}", jobId, pending.appId);
                g_JobIdToPendingStats.erase(it);
            }
        }

        // Truly owned / family shared games and unmanaged games must pass untouched
        if (!hasPending || Hooks_Package::HasValidLicense(pending.appId) ||
            (!LuaConfig::HasDepot(pending.appId) && !CloudRedirectHost::IsApp(pending.appId))) {
            LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats response: unmanaged appid {} (or no match), skip", pending.appId);
            return;
        }

        const AppId_t appId = pending.appId;

        CPlayer_GetUserStats_Response resp;
        if (pBody && cbBody > 0) {
            if (!resp.ParseFromArray(pBody, static_cast<int>(cbBody))) {
                LOG_ACHIEVEMENT_WARN("Player::GetUserStats response: failed to ParseFromArray original response");
                resp.Clear();
            }
        }
        LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats response: original body:\n{}", resp.DebugString());

        // Protect sha_schema: if Valve's response lacks sha_schema (e.g. AccessDenied empty body),
        // restore the sha_schema that the client originally requested so local schema cache remains active!
        if ((!resp.has_sha_schema() || resp.sha_schema().empty()) && !pending.shaSchema.empty()) {
            resp.set_sha_schema(pending.shaSchema);
            LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats response: restored sha_schema (len={}) for app {}",
                pending.shaSchema.size(), appId);
        }

        resp.clear_stats();

        // Inject CloudRedirect achievements
        CloudRedirectHost::AchievementBlock blocks[64]{};
        uint32_t n = CloudRedirectHost::GetAchievements(appId, blocks, 64);
        if (n > 0) {
            uint32_t crc = 0;
            for (uint32_t i = 0; i < n; i++) {
                auto* s = resp.add_stats();
                s->set_stat_id(blocks[i].statId);
                s->set_stat_value(blocks[i].bits);
                for (uint32_t bit = 0; bit < 32; bit++) {
                    if ((blocks[i].bits & (1u << bit)) != 0) {
                        auto* ut = s->add_unlock_times();
                        ut->set_achievement_bit(bit);
                        ut->set_unlock_time(blocks[i].unlockTimes[bit]);
                    }
                }
                crc ^= blocks[i].bits ^ blocks[i].statId;
            }
            resp.set_crc_stats(crc);
            LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats response: injected {} CR achievement blocks for app {}", n, appId);
        } else {
            LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats response: cleared stats (no CR achievements for app {})", appId);
        }

        // Transactional serialization: only commit both if both succeed
        CMsgProtoBufHeader hdrMsg = inHdr;
        hdrMsg.set_eresult(static_cast<int32_t>(k_EResultOK));
        if (hdrMsg.has_error_message()) {
            hdrMsg.clear_error_message();
        }
        uint32 cbHdrNew = static_cast<uint32>(hdrMsg.ByteSizeLong());
        if (cbHdrNew > maxHdr || !hdrMsg.SerializeToArray(outHdr, static_cast<int>(maxHdr)))
            return;

        uint32 cbBodyNew = static_cast<uint32>(resp.ByteSizeLong());
        if (cbBodyNew > maxBody || !resp.SerializeToArray(outBody, static_cast<int>(maxBody))) {
            LOG_ACHIEVEMENT_WARN("Player::GetUserStats response: failed to SerializeToArray modified response");
            return;
        }

        outCbHdr = cbHdrNew;
        outNeedHdr = true;
        outCbBody = cbBodyNew;
        outNeedBody = true;
        LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats response: successfully committed modified header and body");
    }

    // ── Send: CMsgClientGetUserStats (eMsg 818) ────────────────
    bool HandleSend_ClientGetUserStats(const uint8* pBody, uint32 cbBody,
                                      uint8* outBody, uint32 maxBody, uint32& outCbBody)
    {
        CMsgClientGetUserStats req;
        if (!req.ParseFromArray(pBody, static_cast<int>(cbBody))) {
            LOG_ACHIEVEMENT_WARN("ClientGetUserStats request: failed to ParseFromArray");
            return false;
        }
        LOG_ACHIEVEMENT_DEBUG("ClientGetUserStats request: original body:\n{}", req.DebugString());

        if (!req.has_game_id()) {
            LOG_ACHIEVEMENT_WARN("ClientGetUserStats request: missing game_id");
            return false;
        }
        AppId_t appId = static_cast<AppId_t>(req.game_id());
        if (appId == kOnlineFixAppId && Hooks_Misc::IsOnlineFixActive()) {
            const AppId_t resolved = Hooks_Misc::ResolveAppId();
            if (resolved != 0) {
                appId = resolved;
                req.set_game_id(appId);
            }
        }
        if (Hooks_Package::HasValidLicense(appId)) {
            return false;
        }
        if (!LuaConfig::HasDepot(appId) && !CloudRedirectHost::IsApp(appId)) {
            LOG_ACHIEVEMENT_WARN("ClientGetUserStats request: appid={} is not managed", appId);
            return false;
        }
        if (req.schema_local_version() != -1) {
            req.set_schema_local_version(-1);
            LOG_ACHIEVEMENT_DEBUG("ClientGetUserStats request: forced schema_local_version to -1");
        }

        uint64_t newSteamId = LuaConfig::GetStatSteamId(appId);
        if (newSteamId != 0) {
            req.set_steam_id_for_user(newSteamId);
        }

        outCbBody = static_cast<uint32>(req.ByteSizeLong());
        if (outCbBody > maxBody || !req.SerializeToArray(outBody, static_cast<int>(maxBody))) {
            LOG_ACHIEVEMENT_WARN("ClientGetUserStats request: failed to SerializeToArray");
            return false;
        }

        LOG_ACHIEVEMENT_DEBUG("ClientGetUserStats request: modified body:\n{}", req.DebugString());
        return true;
    }

    // ── Recv: CMsgClientGetUserStatsResponse (eMsg 819) ────────
    //     Clear donor stats, overlay CR achievements, patch eresult->OK.
    bool HandleRecv_ClientGetUserStatsResponse(const uint8* pBody, uint32 cbBody,
                                              uint8* outBody, uint32 maxBody, uint32& outCbBody)
    {
        if (!pBody || cbBody == 0)
            return false;
        CMsgClientGetUserStatsResponse resp;
        if (!resp.ParseFromArray(pBody, static_cast<int>(cbBody)))
            return false;
        LOG_ACHIEVEMENT_DEBUG("ClientGetUserStats response: original body:\n{}", resp.DebugString());
        if (!resp.has_game_id()) {
            return false;
        }

        AppId_t appId = static_cast<AppId_t>(resp.game_id());
        if (appId == kOnlineFixAppId && Hooks_Misc::IsOnlineFixActive()) {
            const AppId_t resolved = Hooks_Misc::ResolveAppId();
            if (resolved != 0) {
                appId = resolved;
            }
        }
        if (Hooks_Package::HasValidLicense(appId)) {
            return false;
        }
        if (!LuaConfig::HasDepot(appId) && !CloudRedirectHost::IsApp(appId)) {
            LOG_ACHIEVEMENT_DEBUG("ClientGetUserStats response: appid={} not managed, skip", appId);
            return false;
        }

        resp.clear_stats();
        resp.clear_achievement_blocks();
        resp.set_eresult(1);  // k_EResultOK

        // Overlay CR's cloud-synced achievement state
        CloudRedirectHost::AchievementBlock blocks[64]{};
        uint32_t n = CloudRedirectHost::GetAchievements(appId, blocks, 64);
        if (n > 0) {
            uint32_t crc = 0;
            for (uint32_t i = 0; i < n; i++) {
                auto* s = resp.add_stats();
                s->set_stat_id(blocks[i].statId);
                s->set_stat_value(blocks[i].bits);
                auto* ab = resp.add_achievement_blocks();
                ab->set_achievement_id(blocks[i].statId);
                for (uint32_t bit = 0; bit < 32; bit++)
                    ab->add_unlock_time(blocks[i].unlockTimes[bit]);
                crc ^= blocks[i].bits ^ blocks[i].statId;
            }
            resp.set_crc_stats(crc);
            LOG_ACHIEVEMENT_DEBUG("ClientGetUserStats response: injected {} CR achievement blocks for app {}", n, appId);
        } else {
            LOG_ACHIEVEMENT_DEBUG("ClientGetUserStats response: cleared stats/achievements (no CR data for app {})", appId);
        }

        auto newSize = resp.ByteSizeLong();
        if (newSize > maxBody) {
            LOG_ACHIEVEMENT_WARN("ClientGetUserStats response: modified message too large ({} bytes)", newSize);
            return false;
        }
        if (!resp.SerializeToArray(outBody, static_cast<int>(maxBody)))
            return false;

        outCbBody = static_cast<uint32>(newSize);
        LOG_ACHIEVEMENT_DEBUG("ClientGetUserStats response: modified body:\n{}", resp.DebugString());
        return true;
    }

    // ── Recv: CMsgClientStoreUserStatsResponse (eMsg 821) ────────
    void HandleRecv_ClientStoreUserStatsResponse(const uint8* pHdr, uint32 cbHdr,
                                                 const uint8* pBody, uint32 cbBody,
                                                 uint8* outHdr, uint32 maxHdr, uint32& outCbHdr, bool& outNeedHdr,
                                                 uint8* outBody, uint32 maxBody, uint32& outCbBody, bool& outNeedBody)
    {
        if (!pBody || cbBody == 0)
            return;
        CMsgClientStoreUserStatsResponse resp;
        if (!resp.ParseFromArray(pBody, static_cast<int>(cbBody)))
            return;
        LOG_ACHIEVEMENT_DEBUG("ClientStoreUserStats response: original body:\n{}", resp.DebugString());
        if (!resp.has_game_id())
            return;

        AppId_t appId = static_cast<AppId_t>(resp.game_id());
        if (appId == kOnlineFixAppId && Hooks_Misc::IsOnlineFixActive()) {
            const AppId_t resolved = Hooks_Misc::ResolveAppId();
            if (resolved != 0) {
                appId = resolved;
            }
        }
        if (Hooks_Package::HasValidLicense(appId)) {
            return;
        }
        if (!LuaConfig::HasDepot(appId) && !CloudRedirectHost::IsApp(appId)) {
            return;
        }

        // If header has non-OK eresult, patch header to OK
        if (pHdr && cbHdr > 0) {
            CMsgProtoBufHeader hdrMsg;
            if (hdrMsg.ParseFromArray(pHdr, static_cast<int>(cbHdr)) && hdrMsg.has_eresult() && hdrMsg.eresult() != 1) {
                hdrMsg.set_eresult(1);
                if (hdrMsg.has_error_message()) {
                    hdrMsg.clear_error_message();
                }
                uint32 cb = static_cast<uint32>(hdrMsg.ByteSizeLong());
                if (cb <= maxHdr && hdrMsg.SerializeToArray(outHdr, static_cast<int>(maxHdr))) {
                    outCbHdr = cb;
                    outNeedHdr = true;
                    LOG_ACHIEVEMENT_DEBUG("ClientStoreUserStats response: patched header eresult to OK for app {}", appId);
                }
            }
        }

        bool bodyChanged = false;
        if (resp.eresult() != 1) { // 1 = k_EResultOK
            resp.set_eresult(1);
            bodyChanged = true;
        }
        if (resp.has_stats_failed_validation()) {
            resp.clear_stats_failed_validation();
            bodyChanged = true;
        }
        if (bodyChanged) {
            auto newSize = resp.ByteSizeLong();
            if (newSize <= maxBody && resp.SerializeToArray(outBody, static_cast<int>(maxBody))) {
                outCbBody = static_cast<uint32>(newSize);
                outNeedBody = true;
                LOG_ACHIEVEMENT_DEBUG("ClientStoreUserStats response: patched body eresult to OK for app {}", appId);
            }
        }
    }

} // namespace Hooks_NetPacket_UserStats



// ════════════════════════════════════════════════════════════════
//  Hooks_NetPacket_OwnershipTicket
//
//  Incoming: MsgClientGetAppOwnershipTicketResponse (eMsg 858).
//  Some Denuvo titles (e.g. Suicide Squad: KTJL) verify ownership via this
//  network message instead of the IPC GetAppOwnershipTicketExtendedData hook,
//  so OST's IPC ownership spoof never engages and the real (non-owning) account
//  leaks through -> 88500012. 858 is a legacy NON-protobuf message with no
//  schema in-tree and responses of varying size, so log the raw layout first;
//  the spoof (inject the owner's signed ticket from the credential store) is
//  wired once the exact field offsets are confirmed from a live capture.
// ════════════════════════════════════════════════════════════════
namespace Hooks_NetPacket_OwnershipTicket {

    void HandleRecv(const uint8* pBody, uint32 cbBody,
                    uint8* outBody, uint32 maxBody, uint32& outCbBody, bool& outNeedBody)
    {
        if (!pBody || cbBody == 0) return;
        CMsgClientGetAppOwnershipTicketResponse resp;
        if (!resp.ParseFromArray(pBody, static_cast<int>(cbBody))) {
            LOG_NETPACKET_WARN("OwnershipTicketResponse[858]: failed to ParseFromArray (cbBody={})", cbBody);
            return;
        }

        // Steam already returned a valid ticket (account owns it) — sync to Lua if present.
        if (resp.eresult() == k_EResultOK) {
            if (resp.has_ticket() && !resp.ticket().empty()) {
                const auto& t = resp.ticket();
                const auto span = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(t.data()), t.size());
                if (PipeManager::IsToolActiveForApp(resp.app_id())) {
                    LOG_NETPACKET_INFO("OwnershipTicketResponse[858]: appId={} requested by active tool — skipping Lua disk sync", resp.app_id());
                    AppTicket::WriteAppOwnershipTicket(resp.app_id(), span);
                } else {
                    PipeManager::DenuvoAuth::SyncAppTicketToLua(resp.app_id(), span.data(), span.size());
                }
            }
            return;
        }
        if (!LuaConfig::HasDepot(resp.app_id())) return;

        const int32 origEresult = resp.eresult();

        // Prefer the cached ticket when it is already valid: that
        // ensures GetAppOwnershipTicketExtendedData and the 858 response hand
        // Denuvo the identical bytes. Serving a different (backend-minted) ticket
        // here caused a cross-check mismatch → 012 even when the SteamID was the
        // same account. Only mint from the backend when the in-memory cache has
        // no valid ticket (existingSteamId == 0).
        auto stored = AppTicket::GetCachedAppOwnershipTicket(resp.app_id());
        const uint64_t existingSteamId = AppTicket::ExtractSteamIdFromTicketBytes(stored);

        std::vector<uint8_t> ticketBytes;
        if (existingSteamId != 0) {
            ticketBytes = std::move(stored);
        } else {
            auto minted = EticketClient::FetchOwnershipTicket(resp.app_id(), {}, 0);
            if (!minted) {
                LOG_NETPACKET_WARN("OwnershipTicketResponse[858]: appid={} eresult={} but no owner ticket available",
                                   resp.app_id(), origEresult);
                return;
            }
            ticketBytes = std::move(*minted);
        }

        resp.set_ticket(ticketBytes.data(), ticketBytes.size());
        resp.set_eresult(k_EResultOK);

        const auto encSize = resp.ByteSizeLong();
        if (encSize > maxBody) {
            LOG_NETPACKET_WARN("OwnershipTicketResponse[858]: modified message too large ({})", encSize);
            return;
        }
        if (!resp.SerializeToArray(outBody, static_cast<int>(maxBody))) {
            LOG_NETPACKET_WARN("OwnershipTicketResponse[858]: failed to SerializeToArray");
            return;
        }

        outCbBody = static_cast<uint32>(encSize);
        outNeedBody = true;
        LOG_NETPACKET_INFO("OwnershipTicketResponse[858]: spoofed appid={} ticket_bytes={} (orig eresult={} -> OK)",
                           resp.app_id(), ticketBytes.size(), origEresult);
    }

} // namespace Hooks_NetPacket_OwnershipTicket


// ════════════════════════════════════════════════════════════════
//  Hooks_NetPacket_FamilySharing
// ════════════════════════════════════════════════════════════════
namespace Hooks_NetPacket_FamilySharing {

    // Sanitizes FamilyGroupsClient.NotifyRunningApps RPC notification:
    // Retains the valid family_groupid while clearing running_apps entries to 0.
    // Steam client receives a structurally sound RPC message informing it that
    // 0 apps are currently running, actively clearing library lock state.
    void HandleRecv_NotifyRunningApps(const uint8* pBody, uint32 cbBody,
                                      uint8* outBody, uint32 maxBody, uint32& outCbBody, bool& outNeedBody)
    {
        if (!pBody || cbBody == 0) {
            // Pass through untouched if body is empty
            return;
        }

        CFamilyGroupsClient_NotifyRunningApps_Notification notify;
        if (notify.ParseFromArray(pBody, static_cast<int>(cbBody))) {
            const int runningCount = notify.running_apps_size();
            if (runningCount == 0) {
                // If there are already 0 running apps, pass through untouched.
                return;
            }

            notify.clear_running_apps();

            const auto encSize = notify.ByteSizeLong();
            if (encSize <= maxBody && notify.SerializeToArray(outBody, static_cast<int>(maxBody))) {
                outCbBody = static_cast<uint32>(encSize);
                outNeedBody = true;
                LOG_NETPACKET_INFO("FamilySharing: Sanitized NotifyRunningApps (family_groupid {}, cleared {} running apps)",
                                   notify.family_groupid(), runningCount);
                return;
            }
        }

        // Never clear the entire body on failure — pass through unmodified to preserve family context.
        LOG_NETPACKET_WARN("FamilySharing: Failed to sanitize NotifyRunningApps, passing through untouched");
    }

    // Clears incoming lock/stop notifications (eMsg 9405 / 9406) to suppress kick timers
    void ClearBody(uint32& outCbBody, bool& outNeedBody)
    {
        LOG_NETPACKET_DEBUG("FamilySharing: Clearing incoming family sharing lock/stop notification...");
        outCbBody = 0;
        outNeedBody = true;
    }

} // namespace Hooks_NetPacket_FamilySharing


// ════════════════════════════════════════════════════════════════
//  Hooks_NetPacket_Manifest
//
//  Outgoing: ContentServerDirectory.GetManifestRequestCode#1  (eMsg 151)
//  Incoming: ContentServerDirectory.GetManifestRequestCode#1  (eMsg 147)
//
//  Launches an async HTTP fetch on send; the recv handler waits up to
//  400 ms for the result and patches both header (eresult=OK) and body
//  (manifest_request_code). Results are cached in g_ManifestCodeCache
//  so that if network latency exceeds 400ms on first attempt, subsequent
//  retries hit the cache instantly without stalling the network thread.
// ════════════════════════════════════════════════════════════════
namespace Hooks_NetPacket_Manifest {

    struct PendingManifestJob {
        uint64 manifestGid = 0;
        uint64 cachedCode  = 0;
        std::shared_future<uint64> future;
    };

    std::mutex g_CodeMutex;
    std::unordered_map<uint64, uint64> g_ManifestCodeCache; // manifestGid -> code
    std::unordered_map<uint64, std::shared_future<uint64>> g_InFlightFutures; // manifestGid -> future
    std::unordered_map<uint64, PendingManifestJob> g_PendingJobs; // jobId -> PendingManifestJob
    constexpr uint32 kMaxWaitMilliseconds = 400;

    bool HandleSend(const uint8* pBody, uint32 cbBody,
                    const CMsgProtoBufHeader& hdr)
    {
        CContentServerDirectory_GetManifestRequestCode_Request req;
        if (!req.ParseFromArray(pBody, static_cast<int>(cbBody))) {
            LOG_MANIFEST_WARN("GetManifestRequestCode: failed to parse request");
            return false;
        }
        if (!req.has_depot_id() || !req.has_manifest_id()) return false;
        if (!LuaConfig::HasDepot(req.depot_id())) return false;

        if (!hdr.has_jobid_source()) {
            LOG_MANIFEST_WARN("GetManifestRequestCode: missing jobid_source in header");
            return false;
        }

        const uint64 jobId       = hdr.jobid_source();
        const uint64 manifestGid = req.manifest_id();
        const uint32 depotId     = req.depot_id();
        const uint32 appId       = req.has_app_id() ? req.app_id() : 0;

        LOG_MANIFEST_DEBUG("GetManifestRequestCode send: depot={} gid={} jobid={} app_id={}",
                           depotId, manifestGid, jobId, appId);

        std::lock_guard<std::mutex> lock(g_CodeMutex);

        if (g_PendingJobs.size() >= 256) {
            g_PendingJobs.erase(g_PendingJobs.begin());
        }

        // Bounded pruning of stale in-flight futures
        if (g_InFlightFutures.size() >= 64) {
            std::erase_if(g_InFlightFutures, [](const auto& item) {
                return item.second.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
            });
            if (g_InFlightFutures.size() >= 64) {
                g_InFlightFutures.clear();
            }
        }

        // 1. Check if already cached
        auto itCache = g_ManifestCodeCache.find(manifestGid);
        if (itCache != g_ManifestCodeCache.end() && itCache->second != 0) {
            PendingManifestJob job;
            job.manifestGid = manifestGid;
            job.cachedCode  = itCache->second;
            g_PendingJobs[jobId] = std::move(job);
            LOG_MANIFEST_DEBUG("GetManifestRequestCode send: cache hit for gid={} (code={})", manifestGid, itCache->second);
            return false;
        }

        // 2. Check if an async fetch for this manifest is already in flight
        auto itFlight = g_InFlightFutures.find(manifestGid);
        if (itFlight != g_InFlightFutures.end()) {
            PendingManifestJob job;
            job.manifestGid = manifestGid;
            job.future      = itFlight->second;
            g_PendingJobs[jobId] = std::move(job);
            LOG_MANIFEST_DEBUG("GetManifestRequestCode send: attaching to in-flight fetch for gid={}", manifestGid);
            return false;
        }

        // 3. Launch async fetch
        auto task = std::async(std::launch::async,
            [manifestGid, depotId, appId]() -> uint64 {
                uint64 code = 0;
                ManifestClient::FetchManifestRequestCode(manifestGid, &code, appId, depotId);
                {
                    std::lock_guard<std::mutex> lk(g_CodeMutex);
                    if (code != 0) {
                        if (g_ManifestCodeCache.size() >= 512) {
                            g_ManifestCodeCache.erase(g_ManifestCodeCache.begin());
                        }
                        g_ManifestCodeCache[manifestGid] = code;
                    }
                    g_InFlightFutures.erase(manifestGid);
                }
                return code;
            });

        auto sharedFuture = task.share();
        g_InFlightFutures[manifestGid] = sharedFuture;

        PendingManifestJob job;
        job.manifestGid = manifestGid;
        job.future      = sharedFuture;
        g_PendingJobs[jobId] = std::move(job);

        return false; // Don't modify the outgoing request body
    }

    void HandleRecv(const uint8* pBody, uint32 cbBody,
                    const CMsgProtoBufHeader& inHdr,
                    uint8* outHdr, uint32 maxHdr, uint32& outCbHdr, bool& outNeedHdr,
                    uint8* outBody, uint32 maxBody, uint32& outCbBody, bool& outNeedBody)
    {
        if (!inHdr.has_jobid_target()) return;
        const uint64 jobId = inHdr.jobid_target();

        PendingManifestJob job;
        {
            std::lock_guard<std::mutex> lock(g_CodeMutex);
            auto it = g_PendingJobs.find(jobId);
            if (it == g_PendingJobs.end()) return;
            job = std::move(it->second);
            g_PendingJobs.erase(it); // Always clean up immediately
        }

        uint64 code = job.cachedCode;
        if (code == 0) {
            std::lock_guard<std::mutex> lock(g_CodeMutex);
            auto it = g_ManifestCodeCache.find(job.manifestGid);
            if (it != g_ManifestCodeCache.end()) {
                code = it->second;
            }
        }
        if (code == 0 && job.future.valid()) {
            auto status = job.future.wait_for(std::chrono::milliseconds(kMaxWaitMilliseconds));
            if (status != std::future_status::ready) {
                LOG_MANIFEST_WARN("GetManifestRequestCode recv: HTTP timed out ({}ms) for jobid={}, gid={}, passing through",
                                  kMaxWaitMilliseconds, jobId, job.manifestGid);
                return;
            }
            code = job.future.get();
        }

        if (!code) {
            LOG_MANIFEST_WARN("GetManifestRequestCode recv: HTTP returned 0 for jobid={}, gid={}", jobId, job.manifestGid);
            return;
        }

        LOG_MANIFEST_DEBUG("GetManifestRequestCode recv: injecting code={} for jobid={}, gid={}",
                           code, jobId, job.manifestGid);

        // Header: set eresult=OK and purge any error_message from official rejection
        CMsgProtoBufHeader hdr = inHdr;
        hdr.set_eresult(static_cast<int32_t>(k_EResultOK));
        if (hdr.has_error_message()) {
            hdr.clear_error_message();
        }
        uint32 cbHdrNew = static_cast<uint32>(hdr.ByteSizeLong());
        if (cbHdrNew > maxHdr || !hdr.SerializeToArray(outHdr, static_cast<int>(maxHdr))) {
            LOG_MANIFEST_WARN("GetManifestRequestCode recv: failed to SerializeToArray modified header, cbHdrNew: {}, maxHdr: {}",
                              cbHdrNew, maxHdr);
            return;
        }

        // Body: set manifest_request_code
        CContentServerDirectory_GetManifestRequestCode_Response resp;
        resp.set_manifest_request_code(code);

        uint32 cbBodyNew = static_cast<uint32>(resp.ByteSizeLong());
        if (cbBodyNew > maxBody || !resp.SerializeToArray(outBody, static_cast<int>(maxBody))) {
            LOG_MANIFEST_WARN("GetManifestRequestCode recv: failed to SerializeToArray modified body, cbBodyNew: {}, maxBody: {}",
                              cbBodyNew, maxBody);
            return;
        }
        outCbHdr = cbHdrNew;
        outNeedHdr = true;
        outCbBody = cbBodyNew;
        outNeedBody = true;
    }

} // namespace Hooks_NetPacket_Manifest


// ════════════════════════════════════════════════════════════════
//  Hooks_NetPacket_RichPresence
//
//  Outgoing: CMsgClientGamesPlayed   (eMsg 742 / 5410)
//  Incoming: CMsgClientPersonaState  (eMsg 766)
//
//  The server drops friend-side broadcasts for unowned AppIds, so the
//  banner stays on the last cached state.  Cache real self-pushes and
//  re-deliver a patched copy through oRecvPkt by borrowing the next
//  carrier packet's data pointer.
// ════════════════════════════════════════════════════════════════
namespace Hooks_NetPacket_RichPresence {

    std::mutex           g_RPMutex;
    std::atomic<AppId_t> g_PlayingAppId{0};
    std::atomic<uint64>  g_LocalSteamId{0};

    // Most recent self-PersonaState bytes captured from a real server push.
    // Reused as the template every game launch.
    uint8   g_SelfHdr [kMaxHdrSize];
    uint32  g_cbSelfHdr      = 0;
    uint8   g_SelfBody[kMaxBodySize];
    uint32  g_cbSelfBody     = 0;
    bool    g_HaveSelfCached = false;

    // Manufactured PersonaState packet (eMsg 766) ready to inject.
    uint8   g_InjectPkt[kMaxPacketSize];
    uint32  g_cbInjectPkt   = 0;
    bool    g_InjectPending = false;

    // Rich presence KVs per AppId, captured from outbound
    // CMsgClientRichPresenceUpload.  Per-AppId so a multi-game stack
    // does not conflate KV state.
    std::unordered_map<AppId_t, std::vector<std::pair<std::string, std::string>>> g_RPKvsByAppId;

    // Walk Steam's binary KV1 stream (a top-level "RP" struct around
    // string KVs) and collect every string KV at any depth.  Type 0x00
    // starts a struct, 0x01 a string KV, 0x08 ends a struct.  String KVs
    // are null-terminated key + null-terminated value.
    static void ExtractStringKVs(const uint8* data, uint32 size,
                                 std::vector<std::pair<std::string, std::string>>& out)
    {
        uint32 pos = 0;
        int depth = 0;
        auto readCStr = [&](std::string& s) -> bool {
            uint32 start = pos;
            while (pos < size && data[pos] != 0) ++pos;
            if (pos >= size) return false;
            s.assign(reinterpret_cast<const char*>(data + start), pos - start);
            ++pos;
            return true;
        };
        while (pos < size) {
            uint8 type = data[pos++];
            if (type == 0x08) {
                if (depth > 0) { --depth; continue; }
                break;
            }
            if (type == 0x00) {
                std::string name;
                if (!readCStr(name)) return;
                ++depth;
            } else if (type == 0x01) {
                std::string key, value;
                if (!readCStr(key) || !readCStr(value)) return;
                out.emplace_back(std::move(key), std::move(value));
            } else {
                return;
            }
        }
    }

    // Patch the self Friend entry with the appid (0 = stopped) and per-app
    // KVs.  Mask status_flags's RichPresence bit (0x1000) on appid + empty
    // KVs so a freshly launched game's first inject does not wipe the UI's
    // m_mapRichPresence, which is rebuilt from rich_presence() whenever
    // that bit is set.
    static void ApplyGameFields(CMsgClientPersonaState& msg,
                                CMsgClientPersonaState::Friend* entry,
                                AppId_t appid)
    {
        // EClientPersonaStateFlag::k_EClientPersonaStateFlagRichPresence
        constexpr uint32 kStatusFlagRichPresence = 0x1000;

        if (appid) {
            entry->set_game_played_app_id(appid);
            entry->set_gameid(static_cast<uint64>(appid));
            std::string name = Hooks_Misc::GetGameNameByAppID(appid);
            if (!name.empty()) entry->set_game_name(name);
            entry->clear_rich_presence();
            auto it = g_RPKvsByAppId.find(appid);
            const bool hasKvs = (it != g_RPKvsByAppId.end()) && !it->second.empty();
            if (hasKvs) {
                for (const auto& [k, v] : it->second) {
                    auto* kv = entry->add_rich_presence();
                    kv->set_key(k);
                    kv->set_value(v);
                }
                msg.set_status_flags(msg.status_flags() | kStatusFlagRichPresence);
            } else {
                msg.set_status_flags(msg.status_flags() & ~kStatusFlagRichPresence);
            }
        } else {
            entry->clear_game_played_app_id();
            entry->clear_gameid();
            entry->clear_game_name();
            entry->clear_rich_presence();
            msg.set_status_flags(msg.status_flags() | kStatusFlagRichPresence);
        }
    }

    static bool BuildInject(AppId_t appid)
    {
        if (!g_HaveSelfCached) return false;

        CMsgClientPersonaState msg;
        if (!msg.ParseFromArray(g_SelfBody, g_cbSelfBody)) return false;

        // Find our entry (a self-push always contains it).
        CMsgClientPersonaState::Friend* entry = nullptr;
        for (int i = 0; i < msg.friends_size(); ++i) {
            auto* f = msg.mutable_friends(i);
            if (f->has_friendid() && f->friendid() == g_LocalSteamId) {
                entry = f;
                break;
            }
        }
        if (!entry) return false;

        ApplyGameFields(msg, entry, appid);

        uint32 hdrSize  = g_cbSelfHdr;
        uint32 bodySize = static_cast<uint32>(msg.ByteSizeLong());
        uint32 total    = sizeof(MsgHdr) + hdrSize + bodySize;
        if (total > sizeof(g_InjectPkt) || bodySize > kMaxBodySize) {
            LOG_RICHPRESENCE_WARN("Inject packet too large ({} bytes)", total);
            return false;
        }

        auto* mhdr = reinterpret_cast<MsgHdr*>(g_InjectPkt);
        mhdr->eMsg = static_cast<EMsg>(
            static_cast<uint32>(k_EMsgClientPersonaState) | kMsgHdrProtoFlag);
        mhdr->headerLength = hdrSize;
        memcpy(g_InjectPkt + sizeof(MsgHdr), g_SelfHdr, hdrSize);
        if (!msg.SerializeToArray(g_InjectPkt + sizeof(MsgHdr) + hdrSize, bodySize))
            return false;

        g_cbInjectPkt = total;
        LOG_RICHPRESENCE_INFO("Built inject for appid {} ({} bytes)", appid, total);
        return true;
    }

    // Decode outbound CMsgClientRichPresenceUpload into per-AppId KVs
    // and stage a fresh PersonaState inject.
    void TrackRPSend(const uint8* pBody, uint32 cbBody)
    {
        const AppId_t playingAppId = g_PlayingAppId.load(std::memory_order_relaxed);
        if (playingAppId == 0) return;

        CMsgClientRichPresenceUpload up;
        if (!up.ParseFromArray(pBody, cbBody)) return;
        if (!up.has_rich_presence_kv()) return;

        const std::string& kv = up.rich_presence_kv();
        std::vector<std::pair<std::string, std::string>> kvs;
        ExtractStringKVs(reinterpret_cast<const uint8*>(kv.data()),
                         static_cast<uint32>(kv.size()), kvs);
        LOG_RICHPRESENCE_DEBUG("RP upload appid={}: kv_bytes={} extracted={} pairs",
            playingAppId, kv.size(), kvs.size());

        std::lock_guard lock(g_RPMutex);
        g_RPKvsByAppId[playingAppId] = std::move(kvs);
        if (BuildInject(playingAppId)) g_InjectPending = true;
    }

    void TrackSend(const CMsgClientGamesPlayed& msg, const uint8* pHdr, uint32 cbHdr)
    {
        uint64 localSteamId = 0;
        {
            std::lock_guard lock(g_RPMutex);
            if (g_LocalSteamId == 0) {
                CMsgProtoBufHeader hdr;
                if (hdr.ParseFromArray(pHdr, cbHdr) && hdr.has_steamid() && hdr.steamid()) {
                    g_LocalSteamId = hdr.steamid();
                    localSteamId = g_LocalSteamId;
                    LOG_RICHPRESENCE_DEBUG("Captured local SteamID 0x{:X}", localSteamId);
                }
            }
        }
        if (localSteamId != 0) {
            CloudRedirectHost::SetAccountId(
                static_cast<uint32_t>(localSteamId & 0xFFFFFFFF));
        }

        // Steam stacks running games in games_played; the banner follows
        // the tail (most recently launched).  Mirror that — only the
        // tail's appid drives our inject.
        AppId_t topmost = 0;
        if (msg.games_played_size() > 0) {
            topmost = static_cast<AppId_t>(
                msg.games_played(msg.games_played_size() - 1).game_id() & UINT32_MAX);
        }

        // For -onlinefix games, map topmost 480 to real AppId so CloudRedirectHost
        // can track and sync cloud saves for the real game.
        AppId_t effectiveTopmost = topmost;
        if (topmost == kOnlineFixAppId && Hooks_Misc::IsOnlineFixActive()) {
            effectiveTopmost = Hooks_Misc::ResolveAppId();
        }

        // Only track when the topmost is an app configured in Lua config or active -onlinefix.
        // Owned games on top without Lua config let the server's natural broadcast paint the cache.
        const bool isOnlineFixApp = Hooks_Misc::IsOnlineFixActive() && (effectiveTopmost == Hooks_Misc::ResolveAppId());
        AppId_t newTracked = 0;
        if (effectiveTopmost != 0 && effectiveTopmost != kOnlineFixAppId &&
            (LuaConfig::HasDepot(effectiveTopmost, false) || isOnlineFixApp))
        {
            newTracked = effectiveTopmost;
        }

        const AppId_t oldTracked = g_PlayingAppId.exchange(newTracked);
        if (oldTracked == newTracked) return;

        if (oldTracked != 0) {
            try {
                CloudRedirectHost::NotifyAppRunning(oldTracked, false);
            } catch (const std::exception& e) {
                LOG_RICHPRESENCE_ERROR("NotifyAppRunning({}, false) failed: {}", oldTracked, e.what());
            } catch (...) {
                LOG_RICHPRESENCE_ERROR("NotifyAppRunning({}, false) failed with unknown exception", oldTracked);
            }
        }
        if (newTracked != 0) {
            try {
                CloudRedirectHost::NotifyAppRunning(newTracked, true);
            } catch (const std::exception& e) {
                LOG_RICHPRESENCE_ERROR("NotifyAppRunning({}, true) failed: {}", newTracked, e.what());
            } catch (...) {
                LOG_RICHPRESENCE_ERROR("NotifyAppRunning({}, true) failed with unknown exception", newTracked);
            }
            LuaConfig::PrewarmStatSteamId(newTracked);
        }

        std::lock_guard lock(g_RPMutex);
        if (newTracked != 0) {
            LOG_RICHPRESENCE_INFO("Tracking topmost appid {}", newTracked);
            if (BuildInject(newTracked)) g_InjectPending = true;
        } else if (topmost == 0) {
            // Stack went empty — inject a clear so the cache reverts.
            LOG_RICHPRESENCE_DEBUG("GamesPlayed empty, scheduling cache clear");
            if (BuildInject(0)) g_InjectPending = true;
        } else {
            // Topmost is owned (or -onlinefix); let the server's broadcast
            // paint it.  Skipping the clear-inject here avoids a brief
            // "Online" flicker between our drop and the server's push.
            LOG_RICHPRESENCE_DEBUG("Topmost is appid {} (owned or onlinefix); deferring to server", topmost);
        }
    }

    // Cache real self-pushes as the inject template; if we are tracking
    // an unowned game, patch the live message in place so a periodic
    // refresh does not overwrite the injected game info.
    bool HandleRecv(const uint8* pBody, uint32 cbBody, const uint8* pHdr, uint32 cbHdr,
                    uint8* outBody, uint32 maxBody, uint32& outCbBody)
    {
        CMsgClientPersonaState msg;
        if (!msg.ParseFromArray(pBody, static_cast<int>(cbBody))) return false;

        std::lock_guard lock(g_RPMutex);
        CMsgClientPersonaState::Friend* selfEntry = nullptr;
        for (int i = 0; i < msg.friends_size(); ++i) {
            auto* f = msg.mutable_friends(i);
            if (f->has_friendid() && f->friendid() == g_LocalSteamId) {
                selfEntry = f;
                break;
            }
        }
        if (!selfEntry) return false;

        LOG_RICHPRESENCE_DEBUG(
            "Recv self PersonaState: status_flags=0x{:X} friends_size={}",
            msg.status_flags(), msg.friends_size());

        if (cbHdr <= sizeof(g_SelfHdr) && cbBody <= sizeof(g_SelfBody)) {
            memcpy(g_SelfHdr,  pHdr,  cbHdr);
            memcpy(g_SelfBody, pBody, cbBody);
            g_cbSelfHdr      = cbHdr;
            g_cbSelfBody     = cbBody;
            g_HaveSelfCached = true;
        }

        const AppId_t playingAppId = g_PlayingAppId.load(std::memory_order_relaxed);
        if (playingAppId == 0) return false;

        ApplyGameFields(msg, selfEntry, playingAppId);
        outCbBody = static_cast<uint32>(msg.ByteSizeLong());
        if (outCbBody > maxBody) {
            LOG_RICHPRESENCE_WARN("In-place patch too large ({} bytes)", outCbBody);
            return false;
        }
        if (!msg.SerializeToArray(outBody, static_cast<int>(maxBody))) {
            LOG_RICHPRESENCE_WARN("In-place patch SerializeToArray failed");
            return false;
        }
        LOG_RICHPRESENCE_INFO("Patched live self push with appid {}", playingAppId);
        return true;
    }

    // Deliver the pending manufactured PersonaState by borrowing the
    // carrier's data pointer for one oRecvPkt call, then restore.
    void TryInject(void* pThis, CNetPacket* pCarrier,
                   bool (*invokeOriginal)(void*, CNetPacket*))
    {
        uint32 injectSize = 0;
        std::span<uint8> slot;
        {
            std::lock_guard lock(g_RPMutex);
            if (!g_InjectPending || g_cbInjectPkt == 0) return;
            g_InjectPending = false;
            injectSize = g_cbInjectPkt;
            slot = g_RecvPool.AcquireSlot();
            std::memcpy(slot.data(), g_InjectPkt, injectSize);
        }

        {
            CarrierScopeGuard guard(pCarrier, slot.data(), injectSize);
            invokeOriginal(pThis, pCarrier);
        }
        LOG_RICHPRESENCE_INFO("Delivered manufactured self-PersonaState ({} bytes)", injectSize);
    }

} // namespace Hooks_NetPacket_RichPresence


// ════════════════════════════════════════════════════════════════
//  Hooks_NetPacket_OnlineFix
//
//  Outgoing: CMsgClientGamesPlayed (eMsg 742 / 5410)
//
//  When a game launched with -onlinefix reports appid 480, replace
//  game_extra_info with the real game's localized name so friends
//  see the correct title.
// ════════════════════════════════════════════════════════════════
namespace Hooks_NetPacket_OnlineFix {

    bool HandleSend(const uint8* pBody, uint32 cbBody,
                    const uint8* pHdr, uint32 cbHdr,
                    uint8* outBody, uint32 maxBody, uint32& outCbBody)
    {
        if (!pBody || cbBody == 0) return false;
        CMsgClientGamesPlayed msg;
        if (!msg.ParseFromArray(pBody, static_cast<int>(cbBody))) {
            LOG_ONLINEFIX_WARN("OnlineFix: failed to parse CMsgClientGamesPlayed");
            return false;
        }
        LOG_ONLINEFIX_DEBUG("OnlineFix: original body:\n{}", msg.DebugString());

        Hooks_NetPacket_RichPresence::TrackSend(msg, pHdr, cbHdr);

        uint32 localAccountId = static_cast<uint32>(
            Hooks_NetPacket_RichPresence::g_LocalSteamId.load(std::memory_order_relaxed) & 0xFFFFFFFF);
        if (localAccountId == 0 && pHdr && cbHdr > 0) {
            CMsgProtoBufHeader hdr;
            if (hdr.ParseFromArray(pHdr, static_cast<int>(cbHdr)) && hdr.has_steamid() && hdr.steamid() != 0) {
                Hooks_NetPacket_RichPresence::g_LocalSteamId.store(hdr.steamid(), std::memory_order_relaxed);
                localAccountId = static_cast<uint32>(hdr.steamid() & 0xFFFFFFFF);
            }
        }

        bool patched = false;
        for (int i = 0; i < msg.games_played_size(); ++i) {
            auto* game = msg.mutable_games_played(i);
            AppId_t appid = static_cast<AppId_t>(game->game_id() & UINT32_MAX);

            // SpawnProcess rewrites pGameID to 480, so game_id is already 480.
            // Fill game_extra_info with the real game name.
            if (appid == kOnlineFixAppId) {
                AppId_t realAppId = Hooks_Misc::ResolveAppId();
                if (realAppId && realAppId != kOnlineFixAppId) {
                    std::string name = Hooks_Misc::GetGameNameByAppID(realAppId);
                    if (!name.empty()) {
                        game->set_game_extra_info(name);
                        patched = true;
                        LOG_ONLINEFIX_INFO("OnlineFix: 480 -> name '{}' (real appid {})",
                            name, realAppId);
                    }
                }
            }

            // Family sharing concurrency / anti-lock protection:
            // Mask lender's owner_id to 1 so Steam server does not lock out the lender.
            // Only mask when owner_id represents an external lender, preserving own-account ownership.
            if (game->has_owner_id() && game->owner_id() != 0 && game->owner_id() != 1 &&
                (localAccountId == 0 || game->owner_id() != localAccountId))
            {
                LOG_NETPACKET_INFO("FamilySharing: Masking lender owner_id {} -> 1 for game_id {}",
                                   game->owner_id(), game->game_id());
                game->set_owner_id(1);
                patched = true;
            }
        }

        if (!patched) return false;

        outCbBody = static_cast<uint32>(msg.ByteSizeLong());
        if (outCbBody > maxBody) {
            LOG_ONLINEFIX_WARN("OnlineFix: encoded size {} exceeds buffer", outCbBody);
            return false;
        }
        if (!msg.SerializeToArray(outBody, static_cast<int>(maxBody))) {
            LOG_ONLINEFIX_WARN("OnlineFix: failed to SerializeToArray");
            return false;
        }

        LOG_ONLINEFIX_DEBUG("OnlineFix: modified body:\n{}", msg.DebugString());
        return true;
    }

} // namespace Hooks_NetPacket_OnlineFix


// ════════════════════════════════════════════════════════════════
//  Hooks_NetPacket_Cloud
//
//  Steam Cloud save redirection via CloudRedirect (cloud_redirect.dll).
//
//  Outgoing: ServiceMethodCallFromClient (eMsg 151) with target_job_name
//            "Cloud.*" for an addappid()-unlocked game.
//  Incoming: a synthesized ServiceMethodResponse (eMsg 147) carrying the
//            answer produced by CloudRedirect, correlated by jobid.
//
//  CloudRedirect answers the RPC locally (reading/writing the real save
//  bytes to the user's cloud provider). We therefore SUPPRESS the outbound
//  request (it must not reach Valve) and DELIVER the response the same way
//  the RichPresence path injects packets: by borrowing the next inbound
//  "carrier" packet for one oRecvPkt call (CloudRedirect's "Approach D").
// ════════════════════════════════════════════════════════════════
namespace Hooks_NetPacket_Cloud {

    std::mutex                     g_queueMutex;
    std::deque<std::vector<uint8>> g_pending;        // ready-to-inject 147 packets
    std::atomic<uint64>            g_localSteamId{0};

    // ── minimal top-level protobuf varint field reader ──────────
    // Avoids pulling the Cloud.* request message definitions into the
    // proto set just to read one appid field.
    [[nodiscard]] static bool ReadVarint(const uint8* d, uint32 size, uint32& pos, uint64& out) noexcept {
        out = 0;
        int shift = 0;
        while (pos < size) {
            uint8 b = d[pos++];
            out |= static_cast<uint64>(b & 0x7F) << shift;
            if (!(b & 0x80)) return true;
            shift += 7;
            if (shift >= 64) return false;
        }
        return false;
    }

    [[nodiscard]] static bool FindVarintField(const uint8* d, uint32 size, uint32 target, uint64& out) noexcept {
        uint32 pos = 0;
        while (pos < size) {
            uint64 tag;
            if (!ReadVarint(d, size, pos, tag)) return false;
            const uint32 field   = static_cast<uint32>(tag >> 3);
            const uint32 wireType = static_cast<uint32>(tag & 7);
            if (field == target && wireType == 0)
                return ReadVarint(d, size, pos, out);

            switch (wireType) {
            case 0: { uint64 tmp; if (!ReadVarint(d, size, pos, tmp)) return false; break; }
            case 1: if (pos + 8 > size) return false; pos += 8; break;
            case 5: if (pos + 4 > size) return false; pos += 4; break;
            case 2: {
                uint64 len;
                if (!ReadVarint(d, size, pos, len)) return false;
                if (len > size - pos) return false;
                pos += static_cast<uint32>(len);
                break;
            }
            default: return false;   // groups (3/4) — bail
            }
        }
        return false;
    }

    // appid lives in field 1 of every Cloud.* request except
    // ClientCommitFileUpload, where it is field 2 (mirrors CloudRedirect's
    // CloudRpcUtils::ExtractAppId).
    [[nodiscard]] static uint32 ExtractAppId(const char* jobName, const uint8* body, uint32 cbBody) noexcept {
        uint32 fieldNum = 1;
        if (std::strcmp(jobName, "Cloud.ClientCommitFileUpload#1") == 0)
            fieldNum = 2;
        uint64 v = 0;
        if (FindVarintField(body, cbBody, fieldNum, v))
            return static_cast<uint32>(v);
        return 0;
    }

    // Returns true when CloudRedirect handled the request — the caller must
    // then suppress the outbound frame.
    bool HandleSend(const char* jobName,
                    const uint8* pBody, uint32 cbBody,
                    const CMsgProtoBufHeader& reqHdr)
    {
        if (!CloudRedirectHost::IsActive()) return false;

        if (reqHdr.has_steamid() && reqHdr.steamid())
            g_localSteamId.store(reqHdr.steamid(), std::memory_order_relaxed);

        const uint32 rawAppId = ExtractAppId(jobName, pBody, cbBody);
        uint32 appId = rawAppId;
        if (appId == kOnlineFixAppId && Hooks_Misc::IsOnlineFixActive()) {
            appId = Hooks_Misc::ResolveAppId();
            LOG_NETPACKET_INFO("Cloud RPC {}: mapped OnlineFix AppID {} -> {}", jobName, rawAppId, appId);
        }
        if (appId == 0 || !CloudRedirectHost::IsApp(appId)) return false;

        const uint32 accountId = static_cast<uint32>(g_localSteamId.load(std::memory_order_relaxed) & 0xFFFFFFFFull);

        static thread_local uint8 respBuf[kMaxBodySize];
        uint32  respLen  = 0;
        int32_t eresult  = 2;   // EResult::Fail
        if (!CloudRedirectHost::HandleCloudRpc(jobName, appId, accountId,
                                               pBody, cbBody,
                                               respBuf, static_cast<uint32_t>(sizeof(respBuf)),
                                               &respLen, &eresult)) {
            return false;   // not a namespace app / unrecognised — let it pass through
        }

        // Build the 147 ServiceMethodResponse correlated to the request job.
        CMsgProtoBufHeader respHdr;
        if (reqHdr.has_jobid_source()) respHdr.set_jobid_target(reqHdr.jobid_source());
        respHdr.set_eresult(eresult);
        respHdr.set_target_job_name(jobName);

        const uint32 cbRespHdr = static_cast<uint32>(respHdr.ByteSizeLong());
        const uint32 total     = sizeof(MsgHdr) + cbRespHdr + respLen;
        if (cbRespHdr > kMaxHdrSize || respLen > kMaxBodySize || total > kMaxPacketSize) {
            // Can't deliver a response this big — fall through so Steam errors
            // normally instead of leaving the job hung.
            LOG_NETPACKET_WARN("Cloud: {} response too large ({} bytes), passing through",
                               jobName, total);
            return false;
        }

        std::vector<uint8> pkt(total);
        auto* mhdr = reinterpret_cast<MsgHdr*>(pkt.data());
        mhdr->eMsg = static_cast<EMsg>(
            static_cast<uint32>(k_EMsgServiceMethodResponse) | kMsgHdrProtoFlag);
        mhdr->headerLength = cbRespHdr;
        if (!respHdr.SerializeToArray(pkt.data() + sizeof(MsgHdr), static_cast<int>(cbRespHdr)))
            return false;
        if (respLen)
            memcpy(pkt.data() + sizeof(MsgHdr) + cbRespHdr, respBuf, respLen);

        {
            std::lock_guard lk(g_queueMutex);
            if (g_pending.size() < 64)
                g_pending.push_back(std::move(pkt));
        }
        LOG_NETPACKET_DEBUG("Cloud: handled {} app={} -> queued {}-byte response (eresult={})",
                            jobName, appId, total, eresult);
        return true;
    }

    // Deliver any queued cloud responses by borrowing the carrier packet for
    // one oRecvPkt call each (same trick as RichPresence::TryInject). Runs on
    // the network thread from inside the RecvPkt hook.
    void Drain(void* pThis, CNetPacket* pCarrier,
               bool (*invokeOriginal)(void*, CNetPacket*))
    {
        constexpr size_t kMaxBurstDrain = 8;
        for (size_t i = 0; i < kMaxBurstDrain; ++i) {
            std::vector<uint8> pkt;
            {
                std::lock_guard lk(g_queueMutex);
                if (g_pending.empty()) return;
                pkt = std::move(g_pending.front());
                g_pending.pop_front();
            }

            {
                CarrierScopeGuard guard(pCarrier, pkt.data(), static_cast<uint32>(pkt.size()));
                invokeOriginal(pThis, pCarrier);
            }
            LOG_NETPACKET_DEBUG("Cloud: delivered {}-byte response", pkt.size());
        }
    }

} // namespace Hooks_NetPacket_Cloud


// ════════════════════════════════════════════════════════════════
//  Dispatch
// ════════════════════════════════════════════════════════════════
namespace {

    bool SendServiceJob(const CMsgProtoBufHeader& hdr,
                        const uint8* pBody, uint32 cbBody,
                        uint8* outBody, uint32 maxBody, uint32& outCbBody,
                        bool& outSuppress)
    {
        const std::string& targetJobName = hdr.target_job_name();
        LOG_NETPACKET_DEBUG("Send target_job_name: {}", targetJobName);

        // Steam Cloud save redirection: hand every "Cloud.*" request to
        // CloudRedirect. If it answers, suppress the outbound frame (the
        // synthesized response is delivered from the RecvPkt hook).
        // ExitSyncDone/ConflictResolution are notifications that must reach
        // Steam's internal cloud state machine untouched.
        const std::string_view jobView(targetJobName);
        if (jobView.starts_with("Cloud.")) {
            if (jobView == "Cloud.SignalAppExitSyncDone#1") {
                Hooks_Misc::ResetOnlineFixState();
                return false;
            }
            if (jobView == "Cloud.ClientConflictResolution#1")
                return false;
            if (Hooks_NetPacket_Cloud::HandleSend(targetJobName.c_str(), pBody, cbBody, hdr))
                outSuppress = true;
            return false;   // never body-replace a cloud frame
        }

        switch (Fnv1aHash(targetJobName.c_str())) {

        case HASH_JOB_GetUserStats:
            return Hooks_NetPacket_UserStats::HandleSend_GetUserStats(pBody, cbBody, hdr,
                                                                      outBody, maxBody, outCbBody);

        case HASH_JOB_GetManifestRequestCode:
            return Hooks_NetPacket_Manifest::HandleSend(pBody, cbBody, hdr);

        default:
            return false;
        }
    }

    void SendJob(EMsg eMsg, const uint8* pBody, uint32 cbBody,
                 const uint8* pHdr, uint32 cbHdr,
                 uint8* outBody, uint32 maxBody, uint32& outCbBody,
                 bool& outNeedReplace, bool& outSuppress)
    {
        outNeedReplace = false;
        outSuppress    = false;
        outCbBody      = 0;

        if (pHdr && cbHdr > 0 && Hooks_NetPacket_RichPresence::g_LocalSteamId.load(std::memory_order_relaxed) == 0) {
            CMsgProtoBufHeader hdr;
            if (hdr.ParseFromArray(pHdr, static_cast<int>(cbHdr)) && hdr.has_steamid() && hdr.steamid()) {
                std::lock_guard lock(Hooks_NetPacket_RichPresence::g_RPMutex);
                if (Hooks_NetPacket_RichPresence::g_LocalSteamId.load(std::memory_order_relaxed) == 0) {
                    Hooks_NetPacket_RichPresence::g_LocalSteamId.store(hdr.steamid(), std::memory_order_relaxed);
                    CloudRedirectHost::SetAccountId(static_cast<uint32_t>(hdr.steamid() & 0xFFFFFFFF));
                    LOG_ACHIEVEMENT_DEBUG("Captured local SteamID 0x{:X} from outbound packet", hdr.steamid());
                }
            }
        }

        LOG_NETPACKET_DEBUG("Send eMsg {}({}) (cbBody={}, cbHdr={})",
                        MsgName(eMsg), static_cast<uint32>(eMsg), cbBody, cbHdr);

        switch (eMsg) {

        case k_EMsgServiceMethodCallFromClient: {   // 151
            CMsgProtoBufHeader hdr;
            if (pHdr && cbHdr > 0 && hdr.ParseFromArray(pHdr, static_cast<int>(cbHdr)) && hdr.has_target_job_name()) {
                outNeedReplace = SendServiceJob(hdr, pBody, cbBody,
                                                outBody, maxBody, outCbBody, outSuppress);
            }
            return;
        }

        case k_EMsgClientPICSProductInfoRequest:     // 8903
            outNeedReplace = Hooks_NetPacket_AccessToken::HandleSend(pBody, cbBody, outBody, maxBody, outCbBody);
            return;

        case k_EMsgClientGamesPlayed:                 // 742
        case k_EMsgClientGamesPlayedWithDataBlob:     // 5410
            outNeedReplace = Hooks_NetPacket_OnlineFix::HandleSend(pBody, cbBody, pHdr, cbHdr, outBody, maxBody, outCbBody);
            return;

        case k_EMsgClientRichPresenceUpload:           // 7501
            Hooks_NetPacket_RichPresence::TrackRPSend(pBody, cbBody);
            return;

        case k_EMsgClientGetUserStats:               // 818
            outNeedReplace = Hooks_NetPacket_UserStats::HandleSend_ClientGetUserStats(pBody, cbBody, outBody, maxBody, outCbBody);
            return;

        case k_EMsgClientStoreUserStats:              // 820
        case k_EMsgClientStoreUserStats2: {          // 5466
            AppId_t appId = 0;
            if (pBody && cbBody > 0) {
                if (eMsg == k_EMsgClientStoreUserStats) {
                    CMsgClientStoreUserStats msg;
                    if (msg.ParseFromArray(pBody, static_cast<int>(cbBody)) && msg.has_game_id()) {
                        appId = static_cast<AppId_t>(msg.game_id() & 0xFFFFFFFF);
                    }
                } else {
                    CMsgClientStoreUserStats2 msg2;
                    if (msg2.ParseFromArray(pBody, static_cast<int>(cbBody)) && msg2.has_game_id()) {
                        appId = static_cast<AppId_t>(msg2.game_id() & 0xFFFFFFFF);
                    }
                }
            }
            if (appId == 0) {
                appId = Hooks_NetPacket_RichPresence::g_PlayingAppId.load(std::memory_order_relaxed);
            }
            if (appId == kOnlineFixAppId && Hooks_Misc::IsOnlineFixActive()) {
                const AppId_t resolved = Hooks_Misc::ResolveAppId();
                if (resolved != 0) appId = resolved;
            }
            if (appId != 0 && !Hooks_Package::HasValidLicense(appId) &&
                (LuaConfig::HasDepot(appId) || CloudRedirectHost::IsApp(appId))) {
                CloudRedirectHost::NotifyStatsStored(appId);
            }
            return;
        }

        default:
            return;
        }
    }

    void RecvServiceJob(const CMsgProtoBufHeader& hdr,
                        const uint8* pBody, uint32 cbBody,
                        uint8* outHdr, uint32 maxHdr, uint32& outCbHdr, bool& outNeedHdr,
                        uint8* outBody, uint32 maxBody, uint32& outCbBody, bool& outNeedBody)
    {
        const std::string& targetJobName = hdr.target_job_name();
        LOG_NETPACKET_DEBUG("Recv target_job_name: {}", targetJobName);

        switch (Fnv1aHash(targetJobName.c_str())) {

        case HASH_JOB_NotifyRunningApps:
            Hooks_NetPacket_FamilySharing::HandleRecv_NotifyRunningApps(
                pBody, cbBody, outBody, maxBody, outCbBody, outNeedBody);
            return;

        case HASH_JOB_GetUserStats:
            Hooks_NetPacket_UserStats::HandleRecv_GetUserStatsResponse(
                hdr, pBody, cbBody,
                outHdr, maxHdr, outCbHdr, outNeedHdr,
                outBody, maxBody, outCbBody, outNeedBody);
            return;

        case HASH_JOB_GetManifestRequestCode:
            Hooks_NetPacket_Manifest::HandleRecv(
                pBody, cbBody, hdr,
                outHdr, maxHdr, outCbHdr, outNeedHdr,
                outBody, maxBody, outCbBody, outNeedBody);
            return;

        default:
            break;
        }
    }

    void RecvJob(EMsg eMsg, const uint8* pBody, uint32 cbBody,
                 const uint8* pHdr, uint32 cbHdr,
                 uint8* outHdr, uint32 maxHdr, uint32& outCbHdr, bool& outNeedHdr,
                 uint8* outBody, uint32 maxBody, uint32& outCbBody, bool& outNeedBody)
    {
        outNeedHdr  = false;
        outNeedBody = false;
        outCbHdr    = 0;
        outCbBody   = 0;

        if (eMsg == k_EMsgMulti) {
            LOG_NETPACKET_TRACE("Received k_EMsgMulti, skipping dispatch");
            return;
        }
        LOG_NETPACKET_DEBUG("Recv eMsg {}({}) (cbBody={}, cbHdr={})",
                        MsgName(eMsg), static_cast<uint32>(eMsg), cbBody, cbHdr);

        switch (eMsg) {

        case k_EMsgServiceMethod:                      // 146
        case k_EMsgServiceMethodResponse:              // 147
        case k_EMsgServiceMethodSendToClient: {        // 152
            CMsgProtoBufHeader hdr;
            if (pHdr && cbHdr > 0 && hdr.ParseFromArray(pHdr, static_cast<int>(cbHdr)) && hdr.has_target_job_name()) {
                RecvServiceJob(hdr, pBody, cbBody,
                               outHdr, maxHdr, outCbHdr, outNeedHdr,
                               outBody, maxBody, outCbBody, outNeedBody);
            }
            return;
        }

        case k_EMsgClientGetUserStatsResponse:     // 819
            outNeedBody = Hooks_NetPacket_UserStats::HandleRecv_ClientGetUserStatsResponse(
                pBody, cbBody, outBody, maxBody, outCbBody);
            return;

        case k_EMsgClientStoreUserStatsResponse:   // 821
            Hooks_NetPacket_UserStats::HandleRecv_ClientStoreUserStatsResponse(
                pHdr, cbHdr, pBody, cbBody,
                outHdr, maxHdr, outCbHdr, outNeedHdr,
                outBody, maxBody, outCbBody, outNeedBody);
            return;

        case k_EMsgClientSharedLibraryLockStatus:      // 9405
        case k_EMsgClientSharedLibraryStopPlaying:     // 9406
            Hooks_NetPacket_FamilySharing::ClearBody(outCbBody, outNeedBody);
            return;

        case k_EMsgClientPersonaState:                 // 766
            outNeedBody = Hooks_NetPacket_RichPresence::HandleRecv(
                pBody, cbBody, pHdr, cbHdr, outBody, maxBody, outCbBody);
            return;

        case k_EMsgClientGetAppOwnershipTicketResponse:   // 858
            Hooks_NetPacket_OwnershipTicket::HandleRecv(
                pBody, cbBody, outBody, maxBody, outCbBody, outNeedBody);
            return;

        default:
            return;
        }
    }

    // ════════════════════════════════════════════════════════════
    //  Hooks
    // ════════════════════════════════════════════════════════════

    HOOK_FUNC(BBuildAndAsyncSendFrame, bool,
              void* pObject, EWebSocketOpCode eWebSocketOpCode,
              uint8* pubData, uint32 cubData)
    {
        if (eWebSocketOpCode != k_eWebSocketOpCode_Binary || !pubData || cubData < sizeof(MsgHdr))
            return oBBuildAndAsyncSendFrame(pObject, eWebSocketOpCode, pubData, cubData);

        EMsg eMsg;
        const uint8 *pHdr, *pBody;
        uint32 cbHdr, cbBody;
        if (!UnpackRaw(pubData, cubData, eMsg, pHdr, cbHdr, pBody, cbBody)) {
            return oBBuildAndAsyncSendFrame(pObject, eWebSocketOpCode, pubData, cubData);
        }

        auto slot = g_SendPool.AcquireSlot();
        uint8* newBodyBuf = slot.data() + sizeof(MsgHdr) + kMaxHdrSize;
        uint32 cbNewBody = 0;
        bool needReplace = false;
        bool suppress = false;

        SendJob(eMsg, pBody, cbBody, pHdr, cbHdr, newBodyBuf, kMaxBodySize, cbNewBody, needReplace, suppress);

        if (suppress) {
            // CloudRedirect answered this RPC locally; drop the frame.
            return true;
        }

        if (needReplace && cbHdr <= kMaxHdrSize && cbNewBody <= kMaxBodySize) {
            const uint32 newSize = sizeof(MsgHdr) + cbHdr + cbNewBody;
            auto* out = reinterpret_cast<MsgHdr*>(slot.data());
            const auto* orig = reinterpret_cast<const MsgHdr*>(pubData);
            out->eMsg = orig->eMsg;
            out->headerLength = cbHdr;
            std::memcpy(slot.data() + sizeof(MsgHdr), pHdr, cbHdr);
            if (cbNewBody > 0) {
                uint8* finalBodyPos = slot.data() + sizeof(MsgHdr) + cbHdr;
                if (finalBodyPos != newBodyBuf) {
                    std::memmove(finalBodyPos, newBodyBuf, cbNewBody);
                }
            }
            return oBBuildAndAsyncSendFrame(pObject, eWebSocketOpCode, slot.data(), newSize);
        }

        return oBBuildAndAsyncSendFrame(pObject, eWebSocketOpCode, pubData, cubData);
    }

    HOOK_FUNC(RecvPkt, void*, void* pThis, CNetPacket* pPacket)
    {
        if (!pPacket || !pPacket->m_pubData) {
            return oRecvPkt(pThis, pPacket);
        }

        // Deliver manufactured/queued responses in lockless mode with RAII restoration
        Hooks_NetPacket_RichPresence::TryInject(
            pThis, pPacket,
            [](void* pT, CNetPacket* pP) -> bool { return oRecvPkt(pT, pP) != nullptr; });

        Hooks_NetPacket_Cloud::Drain(
            pThis, pPacket,
            [](void* pT, CNetPacket* pP) -> bool { return oRecvPkt(pT, pP) != nullptr; });

        EMsg eMsg;
        const uint8 *pBody, *pHdr;
        uint32 cbBody, cbHdr;
        if (!UnpackRaw(pPacket->m_pubData, pPacket->m_cubData, eMsg, pHdr, cbHdr, pBody, cbBody)) {
            return oRecvPkt(pThis, pPacket);
        }

        auto slot = g_RecvPool.AcquireSlot();
        uint8* newHdrBuf = slot.data() + sizeof(MsgHdr);
        uint8* newBodyBuf = slot.data() + sizeof(MsgHdr) + kMaxHdrSize;
        uint32 cbNewHdr = 0;
        uint32 cbNewBody = 0;
        bool needReplaceHdr = false;
        bool needReplaceBody = false;

        RecvJob(eMsg, pBody, cbBody, pHdr, cbHdr,
                newHdrBuf, kMaxHdrSize, cbNewHdr, needReplaceHdr,
                newBodyBuf, kMaxBodySize, cbNewBody, needReplaceBody);

        if (needReplaceHdr || needReplaceBody) {
            const uint8* finalHdr = needReplaceHdr ? newHdrBuf : pHdr;
            const uint32 finalCbHdr = needReplaceHdr ? cbNewHdr : cbHdr;
            const uint8* finalBody = needReplaceBody ? newBodyBuf : pBody;
            const uint32 finalCbBody = needReplaceBody ? cbNewBody : cbBody;

            if (finalCbHdr <= kMaxHdrSize && finalCbBody <= kMaxBodySize) {
                const uint32 newSize = sizeof(MsgHdr) + finalCbHdr + finalCbBody;
                auto* out = reinterpret_cast<MsgHdr*>(slot.data());
                const auto* orig = reinterpret_cast<const MsgHdr*>(pPacket->m_pubData);
                out->eMsg = orig->eMsg;
                out->headerLength = finalCbHdr;
                if (!needReplaceHdr) {
                    std::memcpy(slot.data() + sizeof(MsgHdr), finalHdr, finalCbHdr);
                }
                uint8* finalBodyPos = slot.data() + sizeof(MsgHdr) + finalCbHdr;
                if (!needReplaceBody || finalBodyPos != finalBody) {
                    std::memmove(finalBodyPos, finalBody, finalCbBody);
                }
                pPacket->m_pubData = slot.data();
                pPacket->m_cubData = newSize;
            }
        }

        return oRecvPkt(pThis, pPacket);
    }

} // anonymous namespace


namespace Hooks_NetPacket {
    void Install() {
        RESOLVE_C(PchMsgNameFromEMsg);
        HOOK_BEGIN();
        INSTALL_HOOK_C(BBuildAndAsyncSendFrame);
        INSTALL_HOOK_C(RecvPkt);
        HOOK_END();
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK(BBuildAndAsyncSendFrame);
        UNINSTALL_HOOK(RecvPkt);
        UNHOOK_END();
    }
}
