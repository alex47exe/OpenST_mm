#include "Hooks_Package.h"
#include "HookMacros.h"
#include "Hooks_SteamUI.h"
#include "Steam/Enums.h"
#include "Steam/Structs.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/HookSupport/VehCommon.h"
#include "Utils/Logging/Log.h"
#include "dllmain.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <span>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
    // ── Trivially destructible wrapper for process-lifetime globals ──
    // Prevents CRT atexit static destruction crashes during ExitProcess
    // when secondary threads are terminated mid-operation by the OS.
    template <typename T>
    class NoDestructor final {
        alignas(T) std::byte m_storage[sizeof(T)];

    public:
        template <typename... Args>
        explicit NoDestructor(Args&&... args) {
            std::construct_at(reinterpret_cast<T*>(m_storage), std::forward<Args>(args)...);
        }

        ~NoDestructor() = default;

        NoDestructor(const NoDestructor&) = delete;
        NoDestructor& operator=(const NoDestructor&) = delete;
        NoDestructor(NoDestructor&&) = delete;
        NoDestructor& operator=(NoDestructor&&) = delete;

        [[nodiscard]] T* get() noexcept { return std::launder(reinterpret_cast<T*>(m_storage)); }
        [[nodiscard]] const T* get() const noexcept { return std::launder(reinterpret_cast<const T*>(m_storage)); }
        [[nodiscard]] T& operator*() noexcept { return *get(); }
        [[nodiscard]] const T& operator*() const noexcept { return *get(); }
        [[nodiscard]] T* operator->() noexcept { return get(); }
        [[nodiscard]] const T* operator->() const noexcept { return get(); }
    };

    RESOLVE_FUNC(CUtlMemoryGrow,               void*, CUtlVector<AppId_t>*, int);
    RESOLVE_FUNC(MarkLicenseAsChanged,         int64, void*, uint32, bool);
    RESOLVE_FUNC(ProcessPendingLicenseUpdates, bool,  void*);

    CAPTURE_THIS_FUNC(GetPackageInfo, PackageInfo*, g_pCPackageInfo, void* pThis, uint32 packageId, uint64 accessToken);
    
    std::mutex                                  g_packageInfoMutex;
    std::atomic<void*>                          g_pCUser{nullptr};
    std::atomic<PackageInfo*>                   g_pInjectedPackageInfo{nullptr};
    std::atomic<bool>                           g_licenseInitialized{false};
    std::atomic<bool>                           g_licenseRefreshPending{false};
    std::shared_mutex                           g_sharedLicensesMutex;
    NoDestructor<std::unordered_set<AppId_t>>   g_sharedLicenses;
    std::atomic<bool>                           g_hasSharedLicenses{false};
    NoDestructor<std::unordered_set<AppId_t>>   g_unlicensedApps;
    std::atomic<bool>                           g_hasUnlicensedApps{false};

    constexpr PackageId_t kInjectedPackageId = 0;
    constexpr uint64_t kInjectedPkgAccessToken = 10660652434190618804ull;

    bool MarkLicenseAsChangedAndProcessUpdates() {
        void* pUser = g_pCUser.load(std::memory_order_relaxed);
        if (!pUser || !oMarkLicenseAsChanged || !oProcessPendingLicenseUpdates) {
            LOG_PACKAGE_WARN("MarkLicenseAsChangedAndProcessUpdates: dependencies not ready, skipping");
            return false;
        }
        oMarkLicenseAsChanged(pUser, kInjectedPackageId, true);
        oProcessPendingLicenseUpdates(pUser);
        LOG_PACKAGE_DEBUG("MarkLicenseAsChangedAndProcessUpdates: marked package {} as changed and processed updates", kInjectedPackageId);
        return true;
    }

    void TryProcessPendingLicenseRefresh() {
        if (!g_licenseRefreshPending.load(std::memory_order_relaxed))
            return;
        if (MarkLicenseAsChangedAndProcessUpdates())
            g_licenseRefreshPending.store(false, std::memory_order_relaxed);
    }

    bool CUtlMemoryGrowWrap(CUtlVector<AppId_t>* pVec, int grow_size) {
        if (!oCUtlMemoryGrow) {
            LOG_PACKAGE_WARN("CUtlMemoryGrow: oCUtlMemoryGrow not ready, cannot grow");
            return false;
        }
        return oCUtlMemoryGrow(pVec, grow_size);
    }

    [[nodiscard]] bool AppendUniqueAppIdsToPackage(PackageInfo* pPkg,
                                                   std::span<const AppId_t> candidateIds,
                                                   uint32_t* outAddedCount = nullptr) {
        if (outAddedCount) {
            *outAddedCount = 0;
        }
        if (!pPkg || candidateIds.empty()) {
            return true;
        }

        const AppId_t* existingData = pPkg->AppIdVec.m_Memory.m_pMemory;
        const uint32 existingSize = existingData ? pPkg->AppIdVec.m_Size : 0u;
        const std::span<const AppId_t> existingSpan(existingData, existingSize);

        std::vector<AppId_t> toAdd;
        toAdd.reserve(candidateIds.size());
        for (AppId_t id : candidateIds) {
            if (id == 0 || id == k_uAppIdInvalid) {
                continue;
            }
            if (std::ranges::find(existingSpan, id) == existingSpan.end() &&
                std::ranges::find(toAdd, id) == toAdd.end()) {
                toAdd.push_back(id);
            }
        }

        if (toAdd.empty()) {
            return true;
        }

        const uint32 oldSize = pPkg->AppIdVec.m_Size;
        const uint32 numToAdd = static_cast<uint32>(toAdd.size());
        if (!CUtlMemoryGrowWrap(&pPkg->AppIdVec, static_cast<int>(numToAdd))) {
            return false;
        }

        for (uint32 i = 0; i < numToAdd; ++i) {
            pPkg->AppIdVec.m_Memory.m_pMemory[oldSize + i] = toAdd[i];
        }
        pPkg->AppIdVec.m_Size = oldSize + numToAdd;
        if (outAddedCount) {
            *outAddedCount = numToAdd;
        }
        return true;
    }

    bool InitFakeLicenseOnce(PackageInfo* pPkg) {
        std::lock_guard lock(g_packageInfoMutex);
        if (g_licenseInitialized.load(std::memory_order_relaxed)) return true;

        // check package status before injecting
        if (pPkg->Status != EPackageStatus::Available) {
            LOG_PACKAGE_WARN("InitFakeLicenseOnce: package status is not Available ({}), skipping injection", static_cast<int>(pPkg->Status));
            return false;
        }

        // Inject all depots from config into the fake license. 
        const std::vector<AppId_t> appIds = LuaConfig::GetAllDepotIds();
        if (!appIds.empty()) {
            const uint32 oldSize = pPkg->AppIdVec.m_Size;
            uint32 addedCount = 0;
            if (!AppendUniqueAppIdsToPackage(pPkg, appIds, &addedCount)) {
                LOG_PACKAGE_WARN("InitFakeLicense(PackageId={}): failed to grow AppId vector", kInjectedPackageId);
                return false;
            }
            if (addedCount > 0) {
                LOG_PACKAGE_INFO("InitFakeLicense(PackageId={}): added {} apps, oldSize={}", kInjectedPackageId, addedCount, oldSize);
            }
        }

        g_licenseInitialized.store(true, std::memory_order_release);
        g_licenseRefreshPending.store(true, std::memory_order_relaxed);
        TryProcessPendingLicenseRefresh();
        return true;
    }

    bool TryInitFakeLicenseOnce() {
        if (g_licenseInitialized.load(std::memory_order_relaxed)) return true;
        if (CAPTURE_READY(GetPackageInfo)) {
            PackageInfo* pPkg = oGetPackageInfo(g_pCPackageInfo, kInjectedPackageId, kInjectedPkgAccessToken);
            if (!pPkg) {
                LOG_PACKAGE_WARN("TryInitFakeLicenseOnce: GetPackageInfo returned null for injected package");
                return false;
            }
            if (!g_pInjectedPackageInfo.load(std::memory_order_relaxed)) {
                g_pInjectedPackageInfo.store(pPkg, std::memory_order_release);
            }
            return InitFakeLicenseOnce(pPkg);
        }
        return false;
    }

    struct LicenseClassification {
        bool isTrulyOwned = false;
        bool isShared = false;
    };

    inline LicenseClassification EvaluateOwnership(bool result, const AppOwnership& own) {
        const bool isSharedLicense = (own.bFamilyShared || own.bBorrowed) &&
                                     (own.PackageId != 0) &&
                                     (own.PackageId != kInjectedPackageId) &&
                                     (own.ExistInPackageNums >= 1) &&
                                     !own.bLicenseExpired;
        if (isSharedLicense) {
            return { .isTrulyOwned = false, .isShared = true };
        }

        const bool isTrulyOwned = result &&
                                  (own.PackageId != 0) &&
                                  (own.PackageId != kInjectedPackageId) &&
                                  (own.ExistInPackageNums >= 1) &&
                                  own.bOwnsLicense &&
                                  !own.bLicenseExpired &&
                                  !own.bFamilyShared &&
                                  !own.bBorrowed;
        if (isTrulyOwned) {
            return { .isTrulyOwned = true, .isShared = false };
        }

        return {};
    }

    inline void RecordLicenseClassification(AppId_t appId, const LicenseClassification& lc) {
        if (lc.isShared) {
            if (g_hasSharedLicenses.load(std::memory_order_acquire)) {
                std::shared_lock rlock(g_sharedLicensesMutex);
                if (g_sharedLicenses->contains(appId)) {
                    return;
                }
            }
            if (LuaConfig::IsOwned(appId)) {
                LuaConfig::UnmarkOwned(appId);
            }
            std::unique_lock wlock(g_sharedLicensesMutex);
            if (g_sharedLicenses->insert(appId).second) {
                g_hasSharedLicenses.store(true, std::memory_order_release);
            }
            if (g_hasUnlicensedApps.load(std::memory_order_acquire)) {
                if (g_unlicensedApps->erase(appId) > 0) {
                    g_hasUnlicensedApps.store(!g_unlicensedApps->empty(), std::memory_order_release);
                }
            }
        } else if (lc.isTrulyOwned) {
            if (LuaConfig::IsOwned(appId)) {
                return;
            }
            LuaConfig::MarkOwned(appId);
            if (g_hasSharedLicenses.load(std::memory_order_acquire) ||
                g_hasUnlicensedApps.load(std::memory_order_acquire)) {
                std::unique_lock wlock(g_sharedLicensesMutex);
                if (g_sharedLicenses->erase(appId) > 0) {
                    g_hasSharedLicenses.store(!g_sharedLicenses->empty(), std::memory_order_release);
                }
                if (g_unlicensedApps->erase(appId) > 0) {
                    g_hasUnlicensedApps.store(!g_unlicensedApps->empty(), std::memory_order_release);
                }
            }
        } else {
            // Neither shared nor owned: check negative cache fast path
            if (g_hasUnlicensedApps.load(std::memory_order_acquire)) {
                std::shared_lock rlock(g_sharedLicensesMutex);
                if (g_unlicensedApps->contains(appId)) {
                    return;
                }
            }
            if (LuaConfig::IsOwned(appId)) {
                LuaConfig::UnmarkOwned(appId);
            }
            std::unique_lock wlock(g_sharedLicensesMutex);
            if (g_sharedLicenses->erase(appId) > 0) {
                g_hasSharedLicenses.store(!g_sharedLicenses->empty(), std::memory_order_release);
            }
            if (g_unlicensedApps->insert(appId).second) {
                g_hasUnlicensedApps.store(true, std::memory_order_release);
            }
        }
    }

    HOOK_FUNC(CheckAppOwnership, bool, void* pObj, AppId_t appId, AppOwnership* pOwn) {
        void* currentCUser = g_pCUser.load(std::memory_order_acquire);
        if (currentCUser != pObj) {
            g_pCUser.store(pObj, std::memory_order_release);
            LOG_PACKAGE_DEBUG("CheckAppOwnership: captured CUser {}", pObj);
            if (currentCUser != nullptr) {
                LuaConfig::ClearOwned();
                std::unique_lock wlock(g_sharedLicensesMutex);
                g_sharedLicenses->clear();
                g_unlicensedApps->clear();
                g_hasSharedLicenses.store(false, std::memory_order_release);
                g_hasUnlicensedApps.store(false, std::memory_order_release);
            }
        }

        AppOwnership localOwn{};
        AppOwnership* pEffectiveOwn = pOwn ? pOwn : &localOwn;

        bool result = oCheckAppOwnership(pObj, appId, pEffectiveOwn);
        TryInitFakeLicenseOnce();
        TryProcessPendingLicenseRefresh();

        const auto lc = EvaluateOwnership(result, *pEffectiveOwn);
        RecordLicenseClassification(appId, lc);
        const bool hasValidLicense = lc.isTrulyOwned || lc.isShared;

        if (lc.isShared) {
            if (Hooks_SteamUI::IsRemoved(appId)) {
                Hooks_SteamUI::CancelRemoval(appId);
            }
            if (pEffectiveOwn->bLicenseLocked) {
                LOG_PACKAGE_DEBUG("CheckAppOwnership: Clearing bLicenseLocked for shared AppId={}", appId);
                pEffectiveOwn->bLicenseLocked = false;
            }
            pEffectiveOwn->bOwnsLicense = true;
            result = true;
        }

        if (LuaConfig::HasDepot(appId, false)) {
            if (!pOwn) {
                return true;
            }
            if (hasValidLicense) {
                pOwn->ReleaseState = EAppReleaseState::Released;
                return result;
            }
            pOwn->PackageId    = kInjectedPackageId;
            pOwn->ReleaseState = EAppReleaseState::Released;
            pOwn->bOwnsLicense = true; // This forces DLCs on steam family shared games that u dont own when adding their appid via .lua
            pOwn->bFreeLicense = false;
            return true;
        }

        // App is not active in LuaConfig:
        // 1. If explicitly marked as removed in the UI session (and not genuinely owned or family shared):
        if (Hooks_SteamUI::IsRemoved(appId) && !hasValidLicense) {
            if (pOwn) {
                pOwn->bOwnsLicense = false;
                pOwn->PackageId = 0;
            }
            return false;
        }
        if (!result && pOwn) {
            pOwn->bOwnsLicense = false;
        }

        return result;
    }

    static LicenseClassification ClassifyAppLicense(AppId_t appId) {
        if (appId == 0 || appId == k_uAppIdInvalid) return {};

        if (LuaConfig::IsOwned(appId)) {
            return { .isTrulyOwned = true, .isShared = false };
        }

        if (g_hasSharedLicenses.load(std::memory_order_acquire) ||
            g_hasUnlicensedApps.load(std::memory_order_acquire)) {
            std::shared_lock lock(g_sharedLicensesMutex);
            if (g_sharedLicenses->contains(appId)) {
                return { .isTrulyOwned = false, .isShared = true };
            }
            if (g_unlicensedApps->contains(appId)) {
                return {};
            }
        }

        void* pUser = g_pCUser.load(std::memory_order_acquire);
        if (!pUser || !oCheckAppOwnership) return {};

        AppOwnership own{};
        bool result = oCheckAppOwnership(pUser, appId, &own);
        const auto lc = EvaluateOwnership(result, own);
        RecordLicenseClassification(appId, lc);
        return lc;
    }
}

namespace Hooks_Package {
    void Install() {
        RESOLVE_C(CUtlMemoryGrow);
        RESOLVE_C(MarkLicenseAsChanged);
        RESOLVE_C(ProcessPendingLicenseUpdates);

        ARM_CAPTURE_C(GetPackageInfo);

        HOOK_BEGIN();
        INSTALL_HOOK_C(CheckAppOwnership);
        HOOK_END();
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK_C(CheckAppOwnership);
        UNHOOK_END();
    }

    void NotifyLicenseChanged() {
        PackageInfo* pPkg = g_pInjectedPackageInfo.load(std::memory_order_acquire);
        if (!pPkg) {
            TryInitFakeLicenseOnce();
            pPkg = g_pInjectedPackageInfo.load(std::memory_order_acquire);
        }
        if (!pPkg) {
            LOG_PACKAGE_WARN("NotifyLicenseChanged: injected PackageInfo not ready, cannot notify");
            return;
        }

        // ── Remove depots that were unloaded ──
        std::vector<AppId_t> removals = LuaConfig::TakePendingRemovals();
        std::vector<AppId_t> additions = LuaConfig::TakePendingAdditions();
        uint32_t removedCount = 0;
        std::unordered_set<AppId_t> addedIds;
        std::vector<AppId_t> uniqueAdditions;

        if (!additions.empty()) {
            addedIds.reserve(additions.size());
            uniqueAdditions.reserve(additions.size());
            for (AppId_t id : additions) {
                if (addedIds.insert(id).second) {
                    uniqueAdditions.push_back(id);
                }
            }
        }

        {
            std::lock_guard lock(g_packageInfoMutex);
            LOG_PACKAGE_DEBUG("NotifyLicenseChanged: processing {} removals", removals.size());
            for (AppId_t id : removals) {
                while (pPkg->AppIdVec.FindAndFastRemove(id)) {
                    ++removedCount;
                    LOG_PACKAGE_DEBUG("NotifyLicenseChanged: removed AppId {}", id);
                }
            }

            // ── Add depots that are newly loaded ──
            LOG_PACKAGE_DEBUG("NotifyLicenseChanged: processing {} additions", additions.size());
            if (!uniqueAdditions.empty()) {
                Hooks_SteamUI::QueueAdditions(uniqueAdditions);
                uint32 addedToPkgCount = 0;
                if (!AppendUniqueAppIdsToPackage(pPkg, uniqueAdditions, &addedToPkgCount)) {
                    LOG_PACKAGE_WARN("NotifyLicenseChanged: failed to grow AppId vector for additions");
                }
            }
        }

        if (addedIds.empty() && removals.empty()) {
            LOG_PACKAGE_DEBUG("NotifyLicenseChanged: no changes");
            return;
        }

        // Mark package 0 as changed and trigger library refresh.
        if (!MarkLicenseAsChangedAndProcessUpdates()) {
            LOG_PACKAGE_WARN("NotifyLicenseChanged: failed to mark license as changed");
        } else {
            LOG_PACKAGE_INFO("NotifyLicenseChanged: {} added, {} removed", addedIds.size(), removedCount);
        }

        // Queue UI removals for the main-thread RunFrame hook to drain.
        // Never touch MarkAppChange from this (FileWatcher) thread.
        std::unordered_set<AppId_t> queuedRemovals;
        for (AppId_t id : removals) {
            // ParseFile unloads the old file before parsing the replacement.
            // Do not queue that transient removal when the id was added again.
            if (!addedIds.contains(id) && !Hooks_Package::HasValidLicense(id) && queuedRemovals.insert(id).second) {
                Hooks_SteamUI::QueueRemoval(id);
            }
        }
        LOG_PACKAGE_DEBUG("NotifyLicenseChanged: queued {} UI removals, skipped {} transient removals",
                          queuedRemovals.size(), removals.size() - queuedRemovals.size());
    }

    bool IsAppTrulyOwned(AppId_t appId) {
        return ClassifyAppLicense(appId).isTrulyOwned;
    }

    bool IsSharedLicense(AppId_t appId) {
        return ClassifyAppLicense(appId).isShared;
    }

    bool HasValidLicense(AppId_t appId) {
        const auto license = ClassifyAppLicense(appId);
        return license.isTrulyOwned || license.isShared;
    }
}
