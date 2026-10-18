#pragma once

#include "Steam/Types.h"
#include <span>

namespace Hooks_Package {
    // Observer callback for license net changes: notifies subscribers about added and removed AppIds.
    using LicenseChangeObserver = void(*)(std::span<const AppId_t> additions, std::span<const AppId_t> removals);
    void SetLicenseChangeObserver(LicenseChangeObserver observer) noexcept;

    // LoadPackage + CheckAppOwnership — patches the package store so that
    // user-supplied depots appear owned and accessible.
    void Install();
    void Uninstall();

    // Mark package 0 as changed and trigger CClientAppManager_ProcessPendingLicenseUpdates.
    void NotifyLicenseChanged();

    // Queries whether the current Steam account genuinely owns the license for this AppId/DLC.
    // Checks memory cache first, falling back to direct oCheckAppOwnership if CUser is captured.
    [[nodiscard]] bool IsAppTrulyOwned(AppId_t appId);

    // Queries whether the current Steam account has a valid family sharing license for this AppId.
    [[nodiscard]] bool IsSharedLicense(AppId_t appId);

    // Fast memory-only query: returns true if the AppId was confirmed as family shared in memory cache.
    // Safe to call from any thread (including UI threads) without calling origCheckAppOwnership or acquiring heavy locks.
    [[nodiscard]] bool IsSharedLicenseCached(AppId_t appId) noexcept;

    // Explicitly registers an AppId as having an official family shared license (called by Hooks_SteamUI)
    // and clears any negative unlicensed cache entry.
    void RecordFamilySharedApp(AppId_t appId);

    // Returns true if the account either genuinely owns the license or has a valid family shared license.
    [[nodiscard]] bool HasValidLicense(AppId_t appId);
}
