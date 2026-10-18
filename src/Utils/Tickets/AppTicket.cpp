#include "AppTicket.h"
#include "Utils/Logging/Log.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace AppTicket {
    namespace {
        struct AppTicketEntry {
            std::vector<uint8_t> appTicket;
            std::vector<uint8_t> eTicket;
        };

        std::atomic<SourceTicketProvider> g_sourceTicketProvider{nullptr};
        std::atomic<BaselineTicketProvider> g_baselineTicketProvider{nullptr};
        std::shared_mutex g_ticketMutex;
        std::unordered_map<AppId_t, AppTicketEntry> g_tickets;
    } // namespace

    void SetSourceTicketProvider(SourceTicketProvider provider) noexcept {
        g_sourceTicketProvider.store(provider, std::memory_order_release);
    }

    void SetBaselineTicketProvider(BaselineTicketProvider provider) noexcept {
        g_baselineTicketProvider.store(provider, std::memory_order_release);
    }

    std::vector<uint8_t> GetSteamConfigStoreTicket(AppId_t appId) {
        if (appId == 0) return {};
        if (auto provider = g_sourceTicketProvider.load(std::memory_order_acquire)) {
            return provider(appId);
        }
        return {};
    }

    std::vector<uint8_t> GetReliableBaselineTicket() {
        if (auto provider = g_baselineTicketProvider.load(std::memory_order_acquire)) {
            return provider();
        }
        return {};
    }

    std::vector<uint8_t> GetCachedAppOwnershipTicket(AppId_t appId) {
        if (appId == 0) return {};
        std::shared_lock lock(g_ticketMutex);
        auto it = g_tickets.find(appId);
        if (it != g_tickets.end() && !it->second.appTicket.empty()) {
            LOG_INFO("Successfully retrieved App Ownership Ticket, AppId: {}, Ticket Size: {}",
                     appId, it->second.appTicket.size());
            return it->second.appTicket;
        }
        LOG_TRACE("Read App Ownership Ticket for AppId {}: ticket unavailable", appId);
        return {};
    }

    // Exploit steamdrmp's off-by-four ticket parsing vulnerability:
    std::vector<uint8_t> ForgeTicketFromSource(AppId_t targetAppId, std::span<const uint8_t> source) noexcept {
        if (targetAppId == 0 || source.size() <= kAppTicketSignatureSize) {
            return {};
        }

        const size_t signedSize = source.size() - kAppTicketSignatureSize;
        if (signedSize < kAppTicketMinimumValidSize) {
            return {};
        }

        std::vector<uint8_t> ticket;
        ticket.resize(source.size() + sizeof(AppId_t));

        auto* dst = ticket.data();
        std::memcpy(dst, source.data(), signedSize);
        std::memcpy(dst + signedSize, &targetAppId, sizeof(AppId_t));
        std::memcpy(dst + signedSize + sizeof(AppId_t), source.data() + signedSize, kAppTicketSignatureSize);

        LOG_INFO("Forged App Ownership Ticket, AppId: {}, Physical Size: {}, Total Size: {}",
                 targetAppId, ticket.size(), source.size());
        return ticket;
    }

    bool GetAppOwnershipTicket(AppId_t appId, AppOwnershipTicket& ticket, AppTicketSource source) {
        ticket = {};
        if (appId == 0) return false;
        
        if (source == AppTicketSource::MemoryOnly || source == AppTicketSource::MemoryThenForge) {
            ticket.data = GetCachedAppOwnershipTicket(appId);
            if (!ticket.data.empty() && ticket.data.size() >= kSteamIdTicketMinimumSize) {
                ticket.totalSize = static_cast<uint32>(ticket.data.size());
                ticket.appIdOffset = kAppTicketAppIdOffset;
                ticket.steamIdOffset = kAppTicketSteamIdOffset;
                std::memcpy(&ticket.signatureOffset, ticket.data.data(), sizeof(uint32));
                ticket.signatureSize = kAppTicketSignatureSize;
                if (ticket.signatureOffset > ticket.totalSize ||
                    ticket.signatureSize > (ticket.totalSize - ticket.signatureOffset) ||
                    (ticket.appIdOffset + sizeof(AppId_t)) > ticket.totalSize) {
                    LOG_WARN("AppTicket: corrupted signature or layout bounds for appId={}, sigOffset={}, sigSize={}, totalSize={}",
                             appId, ticket.signatureOffset, ticket.signatureSize, ticket.totalSize);
                    return false;
                }
                return true;
            }
        }

        if (source == AppTicketSource::MemoryOnly) return false;

        const std::vector<uint8_t> baseline = GetReliableBaselineTicket();
        if (baseline.empty()) {
            LOG_DEBUG("ForgeLocalAppOwnershipTicket for AppId {}: no baseline source appticket", appId);
            return false;
        }

        ticket.data = ForgeTicketFromSource(appId, baseline);
        if (ticket.data.empty()) return false;

        ticket.totalSize = static_cast<uint32>(ticket.data.size() - sizeof(AppId_t));
        ticket.appIdOffset = ticket.totalSize - kAppTicketSignatureSize;
        ticket.steamIdOffset = kAppTicketSteamIdOffset;
        ticket.signatureOffset = ticket.appIdOffset + sizeof(AppId_t);
        ticket.signatureSize = kAppTicketSignatureSize;
        return true;
    }

    std::vector<uint8_t> GetCachedEncryptedTicket(AppId_t appId) {
        if (appId == 0) return {};
        std::shared_lock lock(g_ticketMutex);
        auto it = g_tickets.find(appId);
        if (it != g_tickets.end() && !it->second.eTicket.empty()) {
            LOG_INFO("Successfully retrieved Encrypted App Ticket, AppId: {}, Ticket Size: {}",
                     appId, it->second.eTicket.size());
            return it->second.eTicket;
        }
        LOG_TRACE("Read Encrypted App Ticket for AppId {}: ticket unavailable", appId);
        return {};
    }

    bool HasCachedEncryptedTicket(AppId_t appId) {
        if (appId == 0) return false;
        std::shared_lock lock(g_ticketMutex);
        auto it = g_tickets.find(appId);
        return it != g_tickets.end() && !it->second.eTicket.empty();
    }

    bool WriteAppOwnershipTicket(AppId_t appId, std::span<const uint8_t> data) {
        if (appId == 0 || data.size() < kSteamIdTicketMinimumSize) {
            LOG_WARN("WriteAppOwnershipTicket: invalid parameters (appId={}, size={})", appId, data.size());
            return false;
        }

        // Fast-path: if the identical ticket is already cached in memory, skip redundant write
        {
            std::shared_lock lock(g_ticketMutex);
            auto it = g_tickets.find(appId);
            if (it != g_tickets.end() &&
                it->second.appTicket.size() == data.size() &&
                std::memcmp(it->second.appTicket.data(), data.data(), data.size()) == 0) {
                LOG_DEBUG("WriteAppOwnershipTicket: AppId {} ticket already up-to-date, skipping redundant write", appId);
                return true;
            }
        }

        {
            std::unique_lock lock(g_ticketMutex);
            auto& entry = g_tickets[appId];
            entry.appTicket.assign(data.begin(), data.end());
        }
        LOG_INFO("Wrote AppTicket for AppId {} ({} bytes)", appId, data.size());
        return true;
    }

    bool WriteAppOwnershipTicket(AppId_t appId, const std::vector<uint8_t>& data) {
        return WriteAppOwnershipTicket(appId, std::span<const uint8_t>(data));
    }

    bool RemoveAppOwnershipTicket(AppId_t appId) {
        if (appId == 0) return false;

        std::unique_lock lock(g_ticketMutex);
        auto it = g_tickets.find(appId);
        if (it != g_tickets.end()) {
            it->second.appTicket.clear();
        }
        return true;
    }

    bool WriteEncryptedTicket(AppId_t appId, std::span<const uint8_t> data) {
        if (appId == 0 || data.empty()) return false;

        // Fast-path: if the identical ticket is already cached in memory, skip redundant write
        {
            std::shared_lock lock(g_ticketMutex);
            auto it = g_tickets.find(appId);
            if (it != g_tickets.end() &&
                it->second.eTicket.size() == data.size() &&
                std::memcmp(it->second.eTicket.data(), data.data(), data.size()) == 0) {
                LOG_DEBUG("WriteEncryptedTicket: AppId {} ticket already up-to-date, skipping redundant write", appId);
                return true;
            }
        }

        {
            std::unique_lock lock(g_ticketMutex);
            auto& entry = g_tickets[appId];
            entry.eTicket.assign(data.begin(), data.end());
        }
        LOG_INFO("Wrote ETicket for AppId {} ({} bytes)", appId, data.size());
        return true;
    }

    bool WriteEncryptedTicket(AppId_t appId, const std::vector<uint8_t>& data) {
        return WriteEncryptedTicket(appId, std::span<const uint8_t>(data));
    }

    bool RemoveEncryptedTicket(AppId_t appId) {
        if (appId == 0) return false;

        std::unique_lock lock(g_ticketMutex);
        auto it = g_tickets.find(appId);
        if (it != g_tickets.end()) {
            it->second.eTicket.clear();
        }
        return true;
    }

    bool ClearCachedTickets(AppId_t appId) {
        if (appId == 0) return false;

        std::unique_lock lock(g_ticketMutex);
        return g_tickets.erase(appId) > 0;
    }

    uint64_t ExtractSteamIdFromTicketBytes(std::span<const uint8_t> ticket) noexcept {
        // Layout: ticket bytes start with [uint32 Size][uint32 Version][uint64 SteamID][...].
        if (ticket.size() < kSteamIdTicketMinimumSize) return 0;
        uint64_t steamId = 0;
        std::memcpy(&steamId, ticket.data() + kAppTicketSteamIdOffset, sizeof(uint64_t));
        return steamId;
    }

    uint64_t ExtractSteamIdFromTicketBytes(const uint8_t* data, size_t size) noexcept {
        if (!data || size == 0) return 0;
        return ExtractSteamIdFromTicketBytes(std::span<const uint8_t>(data, size));
    }

    uint64_t ExtractSteamIdFromTicketBytes(const std::vector<uint8_t>& ticket) noexcept {
        return ExtractSteamIdFromTicketBytes(std::span<const uint8_t>(ticket));
    }

    uint64_t GetTicketSteamID(AppId_t appId) {
        if (appId == 0) return 0;
        std::shared_lock lock(g_ticketMutex);
        auto it = g_tickets.find(appId);
        if (it != g_tickets.end() && !it->second.appTicket.empty()) {
            return ExtractSteamIdFromTicketBytes(it->second.appTicket);
        }
        return 0;
    }
}
