<div align="center">
  <img src="docs/logo-animated.svg" width="180" alt="OpenSteamTool logo">

  <h1>OpenSteamTool</h1>

  <p>
    <strong>Open-Source Steam Unlock Tool</strong>
  </p>

  <p>
    <img src="https://img.shields.io/badge/C%2B%2B-20%2B-2ea44f?logo=cplusplus&logoColor=white" alt="C++ 20+">
    <img src="https://img.shields.io/badge/CMake-3.20%2B-2ea44f?logo=cmake&logoColor=white" alt="CMake 3.20+">
    <img src="https://img.shields.io/badge/Windows-only-d73a49?logo=windows&logoColor=white" alt="Windows only">
    <a href="https://deepwiki.com/OpenSteam001/OpenSteamTool">
      <img src="https://deepwiki.com/badge.svg" alt="Ask DeepWiki">
    </a>
  </p>

  <p>
    <a href="README.md">
      <img src="https://flagcdn.com/w40/us.png" width="22" alt="United States flag">
      English
    </a>
    &nbsp;|&nbsp;
    <a href="README_ES.md">
      <img src="https://flagcdn.com/w40/es.png" width="22" alt="Spain flag">
      Español
    </a>
    &nbsp;|&nbsp;
    <a href="README_ZH.md">
      <img src="https://flagcdn.com/w40/cn.png" width="22" alt="China flag">
      中文
    </a>
  </p>
</div>

## Features

### Core Unlocks & Manifest Management
- Unlock unowned games and DLCs without limits
- Auto-load depot decryption keys and PICS access tokens (`addtoken`) from Lua configs
- Auto-download manifests via `manifestdex` (default), `opensteamtool`, `steamrun`, `wudrm`, or custom Lua endpoints
- Lock manifest versions to prevent game updates
- Recursive multi-level subdirectories in `config/lua/` with automatic `.manifest` mirroring to `Steam/depotcache/`

### Hot Reload
- Watched `.lua` directories reload additions and modifications instantly without restarting Steam

### Game Process Injection
- Inject third-party DLLs into game processes via `[[inject]]` in `opensteamtool.toml`
- Supports multiple DLL rules, command-line filtering, and AppID restrictions (see [Third-party DLL injection rules](#third-party-dll-injection))
- Built-in global anti-cheat exclusion list (`[injects].exclude_appids`) protecting ~20 competitive titles (CS2, Dota 2, Apex, PUBG, etc.) from accidental injection

### Family Sharing Support
- Automatically unlocks Family Sharing restrictions with zero configuration and no conflicts

### Cloud Save Redirection (CloudRedirect)
- Integrates [CloudRedirect](https://github.com/Selectively11/CloudRedirect) to enable cloud saves, playtime tracking, and achievement sync for OST-managed games (see `[cloud]` in [Configuration](#configuration-optional))

### Denuvo & SteamStub Compatibility
- **SteamStub**: AppID spoofing via local ConfigStore tickets without game process injection
- **Explicit Tickets**: Memory-only management via `setAppTicket` / `setETicket` in Lua; no disk `.bin` files generated
- **Account Switching Offline Auth**: Auto-syncs credentials directly into `<AppId>.lua` (via `setAppTicket`) after running on a genuine account; switch to unowned accounts for offline play
- **Manifest Locking & Updates**: Defaults to active `setManifestid` to lock versions for offline authorization. On authorized accounts, OST automatically permits official updates and syncs manifests (configurable via `[manifest] lock_owned_games` and `auto_sync_on_update`); pass `-nodenuvo` or set `nodenuvo(appid)` in Lua to bypass Denuvo handling entirely
- **Multi-Stage Adaptive Leasing (Scheme 2)**: Pass `-dauth2` in launch options or declare `dauth2(appid)` in Lua to enable adaptive multi-stage ticket lease extensions for titles with strict timeout windows
- **Helper Flags**:
  - `-d+` (alias `-lua`): Steam launch option on genuine accounts to auto-generate `<AppId>.lua` and lock manifests
  - `-dauth2`: Enable Scheme 2 adaptive ticket lease extension
  - `-forcedenuvo`: Force treat game as Denuvo-protected (or `forcedenuvo(appid)`)
  - `-nodenuvo`: Bypass Denuvo handling (or `nodenuvo(appid)`)

### Third-party product keys ("Updating product key")
- For titles that carry a Steam-issued third-party retail key (Ubisoft Connect, Rockstar, etc.), OpenSteamTool answers the client's legacy key request locally, so the "Updating product key" step completes for added games instead of failing. Supply a real key with `setlegacycdkey(appid, "KEY")`; when none is configured a deterministic synthetic key is generated. A synthetic key satisfies Steam's local step only — titles that validate the key online still require a real one.

### Credentials & Config Extraction with `extract_tickets`
Extracts AppTicket, ETicket, DLC lists, depot keys, manifests, and generates a ready-to-use `<appid>.lua` on accounts owning the game.

* **Interactive Terminal UI (TUI)**: Run `extract_tickets.exe` directly without arguments to launch a terminal interface with account switching, game library search, and batch extraction.
* **Clientless Online Mode (`--online` / `-o`)**: Directly connects to Valve Connection Manager (Steam CM) servers over TCP/TLS for headless ticket/key extraction with QR-code or saved token login—no local Steam client required!
* **Download**: [GitHub Actions Tools Workflow](https://github.com/mmxlyo/OpenSteamTool/actions/workflows/tools.yml) or build locally via `build.bat` (`build/tools/<Config>/extract_tickets.exe`)
* **Usage**:
  ```powershell
  # Interactive TUI mode (default when run without CLI arguments)
  extract_tickets.exe

  # Standard CLI extraction (locally installed game)
  extract_tickets.exe 1361510

  # Force ETicket extraction for uninstalled game
  extract_tickets.exe 1361510 --force-eticket

  # Headless online CM extraction without local Steam client
  extract_tickets.exe --online --appid 1361510
  ```

### Stats and Achievements
- Enable stats and achievements for unowned games
- Priority: Lua `setStat(appid, "steamid")` > stats API (`https://stats.opensteamtool.com/{appid}`) > default SteamID (`76561198028121353`)

### Online Fix
- Add `-onlinefix` to Steam launch options for 480 (Spacewar) multiplayer with automatic real AppID and save protection (one active game at a time)
- Rare titles requiring 480 certificate match can use `-onlinefix -p2pflip` (use only if necessary due to compatibility risks)

---

## Usage

### Method 1: Portable Mode (Recommended)
No files copied to the Steam directory; runs completely green and portable:
1. Extract the release package to any standalone folder (e.g. `D:\OpenSteamTool_Portable`)
2. Create `config/lua/` and add your `.lua` unlock scripts; `opensteamtool.toml` can be placed directly in this portable folder
3. Launch and automation options with `ost-manager.exe`:
   - **One-shot Launch / Inject**: Click "Launch & Inject Steam" in the GUI, or execute `ost-manager.exe /inject` via CLI
   - **Portable IFEO Interception**: Click "Install IFEO (Portable Mode)"; launching Steam normally will automatically trigger zero-window injection
   - **Auto-Start Watcher**: Click "Start Background Watcher" to start background monitoring on login (no administrator privileges needed, registry key `ost_watcher`), or run `ost-manager.exe /watch` via CLI

### Method 2: Steam Root Dedicated Mode
In `ost-manager.exe`, click "Install IFEO (Steam Root)":
- Automatically deploys `OpenSteamTool.dll` and the dedicated security loader `ost-loader.exe` to Steam root
- Binds system IFEO interception so double-clicking official Steam directly triggers instant injection via the lightweight loader
- Allows one-click clean uninstallation at any time via "Uninstall IFEO"

### Method 3: Standard DLL Proxy Mode (Traditional Hijacking / Manual Placement)
For environments where IFEO registry modification is restricted or not preferred:
- **Using Manager**: In `ost-manager.exe`, click "Deploy Standard DLL" (or run `ost-manager.exe /deploy-dll <SteamDir> <PayloadDll>`). Cleanly removable at any time via "Remove DLL" (or `ost-manager.exe /remove-dll <SteamDir>`).
- **Manual Placement**:
  - `OpenSteamTool.dll`: Copy directly to Steam root directory (e.g. `C:\Program Files (x86)\Steam\OpenSteamTool.dll`)
  - `version.dll`: Copy directly to Steam root directory (e.g. `C:\Program Files (x86)\Steam\version.dll`)
  - `dwmapi.dll`: Copy to Steam `bin` subfolder (e.g. `C:\Program Files (x86)\Steam\bin\dwmapi.dll`, **not** in root)
  - Lua unlock scripts: Place in `<Steam>\config\lua\`
- Launching official Steam normally will load OpenSteamTool automatically via standard Windows PE proxy forwarding.

### Manager Command-Line Reference (`ost-manager.exe`)
| Switch | Description | Privilege |
| :--- | :--- | :--- |
| `/inject`, `-i`, `/silent` | Headless one-shot launch & injection (launches Steam if not running, or hot-injects if running) | Standard User |
| `/watch`, `-w`, `/daemon` | Headless background watcher daemon loop | Standard User |
| `/enable-autostart` | Enable background watcher on user logon (`HKCU\...\Run\ost_watcher`) | Standard User |
| `/disable-autostart` | Disable background watcher auto-start entry | Standard User |
| `/install-portable <dll>` | Register IFEO interception in portable mode pointing to manager | Administrator |
| `/install-root <dir> <dll>` | Deploy loader to Steam root and register IFEO root mode | Administrator |
| `/uninstall` | Unregister IFEO and clean deployed root binaries | Administrator |
| `/deploy-dll <dir> <dll>` | Deploy `version.dll` to Steam root and `dwmapi.dll` to `Steam\bin\` | Standard / Admin |
| `/remove-dll <dir>` | Remove proxy DLLs (`version.dll`, `dwmapi.dll`) from Steam directories | Standard / Admin |

---

## Lua Configuration Example

```lua
addappid(1361510) -- unlock game
addappid(1361511, 0, "5954562e7f5260400040a818bc29b60b335bb690066ff767e20d145a3b6b4af0") -- unlock depot with key
addtoken(1361510, "2764735786934684318") -- add PICS access token

setManifestid(1361511, "5656605350306673283") -- pin depot manifest
setManifestid(1361511, "5656605350306673283", 12345678) -- pin depot manifest with size

setAppTicket(1361510, "0100000000000000...") -- memory AppTicket
setETicket(1361510, "0100000000000000...")   -- memory ETicket
setStat(1361510, "76561197960287930")        -- achievement source SteamID

addprocess(1361510, "CustomGame.exe")        -- map AppID for processes without SteamAppId env
seteticketurl("https://example.com/eticket") -- online ETicket minting endpoint (optional)

dauth2(1361510)      -- enable Scheme 2 adaptive ticket leasing for Denuvo
nodenuvo(1361510)    -- bypass Denuvo handling (same as -nodenuvo, alias: disallowdenuvo)
forcedenuvo(1361510) -- force treat as Denuvo (same as -forcedenuvo)

setlegacycdkey(1361510, "ABCD-EFGH-JKMN-PQRS") -- serve this third-party retail CD key at the "Updating product key" step
-- If not configured, a deterministic synthetic key is generated per app+account (satisfies Steam's local step only)
```
All function names are **case-insensitive**.

---

## Configuration (optional)

Rename `opensteamtool.example.toml` (or localized templates `opensteamtool.example_zh.toml` / `opensteamtool.example_es.toml`) to `opensteamtool.toml` and place it in the portable directory or Steam root. Hot-reloaded on changes.

```toml
[log]
# Debug build only: trace, debug, info, warn, error
level = "info"
# Relative or absolute directory for debug logs (optional, Debug build only)
# dir = "opensteamtool"

[manifest]
# Upstream API: "manifestdex" (default), "opensteamtool", "steamrun", "wudrm"
url = "manifestdex"
timeout_resolve_ms = 5000
timeout_connect_ms = 5000
timeout_send_ms    = 10000
timeout_recv_ms    = 10000
# Allow official Steam updates for owned games/DLCs (default: false)
lock_owned_games = false
# Auto-sync Lua scripts and manifests in background after Steam completes updates (default: true)
auto_sync_on_update = true

[stats]
# Query stats API when setStat is absent
enable_api = true

[lua]
# Extra Lua directories to load (optional)
paths = []

[cloud]
# Steam Cloud redirection via CloudRedirect companion app
enabled = false
# library = "cloud_redirect.dll"

# Global DLL injection configuration (exclusion list)
[injects]
# AppIDs to strictly exclude from DLL injection (explicit config overrides built-in defaults; defaults protect ~20 competitive anti-cheat games)
# Note: Exclusion ONLY skips third-party DLL injection into game processes; game unlocking and DLC simulation are never affected!
exclude_appids = [
    730,      # Counter-Strike 2 (VAC)
    570,      # Dota 2 (VAC)
    1172470,  # Apex Legends (EAC)
    578080,   # PUBG: BATTLEGROUNDS (BattlEye)
    # See opensteamtool.example.toml for the full default list
]

# Individual library injection rules (array of tables, multiple allowed, commented out by default)
# [[inject]]
# path = "OnlineFix.dll"
# all_games = false
# when_cmdline = "-onlinefix"
# when_appids = [1361510]

[remote]
# Optional pattern metadata mirror (default GitHub with jsDelivr fallback)
# url_template = "https://your.server/{channel}/{component}/{sha256}.toml"
```

### Global Injection Settings (`[injects]`)
| Key | Explanation |
| :--- | :--- |
| `exclude_appids` | List of AppIDs to strictly exclude from third-party DLL injection into game processes.<br>- **Universal Protection**: Strictly blocks DLL injection across rules unless explicitly overridden by a rule's `when_appids` whitelist;<br>- **Explicit Override Principle**: If explicitly configured (even with an empty list `[]` or a single game), strictly only these AppIDs are excluded (**default list is not merged**);<br>- **Default Fallback Principle**: If omitted entirely, automatically uses the built-in protection list (~20 competitive/anti-cheat titles including CS2, Dota 2, Apex, PUBG, etc.);<br>- **Whitelist Priority**: If an AppID is excluded here but explicitly specified in an `[[inject]]` rule's `when_appids`, the **whitelist takes priority** and the AppID will **not be excluded** for that rule.<br>**Note: This exclusion strictly prevents DLL injection into game processes; game and DLC unlocking (`addappid`, manifests, tickets, etc.) are 100% active and unaffected!** |

<a id="third-party-dll-injection"></a>
### Third-party DLL injection rules (`[[inject]]`)
| Key | Explanation |
| :--- | :--- |
| `path` | DLL to load. Bare file names resolve next to toml, DLL directory, or Steam root; absolute paths used as-is |
| `all_games` | Optional. Defaults to `false` (strongly recommended).<br>- **`false`**: Only targets Lua-unlocked games. **Important: If `when_appids` is omitted, ALL Lua-unlocked games will be injected upon launch! It is strongly recommended to specify `when_appids` (whitelist target AppIDs) or `when_cmdline` (e.g. `"-onlinefix"`) to restrict injection**;<br>- **`true`**: Global injection switch. Injects into all games (including genuine owned and free titles) launched via Steam, protected by `[injects].exclude_appids` above |
| `when_cmdline` | Optional. Substring required in launch command line (e.g. `"-onlinefix"`) |
| `when_appids` | Optional. Whitelist mechanism targeting specific AppIDs (e.g. `[1361510]`). **Takes priority over exclusion list**: even if the target AppID is in the exclusion list, explicitly specifying it here ensures this rule injects into it |

### Manifest via Lua
If defined in `config/lua/`, these functions take priority over configured remote HTTP APIs:

- `fetch_manifest_code_ex(app_id, depot_id, gid)` *(Recommended)*: Extended variant receiving `app_id`, `depot_id`, and `gid` for app-aware API endpoints
- `fetch_manifest_code(gid)`: Base variant receiving manifest GID only

Built-in HTTP helper functions provided by the C++ runtime:
| Function | Signature | Return Value |
| :--- | :--- | :--- |
| `http_get` | `http_get(url [, headers])` | `body, status_code` |
| `http_post` | `http_post(url, body [, headers])` | `body, status_code` |

`headers` is an optional table: `{["Key"]="Value", ...}`

---

## Debug logging

Debug builds write module logs under `<Steam or Portable Dir>/opensteamtool/`:

| File | Source | Content |
| :--- | :--- | :--- |
| `main.log` | General | Init, config loading, Lua parsing |
| `ipc.log` | `LOG_IPC_*` | IPC commands, interface dispatch, spoofing |
| `netpacket.log` | `LOG_NETPACKET_*` | Network packet interception, eMsg dispatch |
| `manifest.log` | `LOG_MANIFEST_*` | Manifest downloads, depotcache mirroring |
| `decryptionkey.log` | `LOG_DECRYPTIONKEY_*` | Depot decryption key injection |
| `keyvalue.log` | `LOG_KEYVALUE_*` | KeyValues manifest patching |
| `misc.log` | `LOG_MISC_*` | Engine pointer capture, AppID mapping |
| `achievement.log` | `LOG_ACHIEVEMENT_*` | Stats and achievement handling |
| `pics.log` | `LOG_PICS_*` | PICS access token injection |
| `package.log` | `LOG_PACKAGE_*` | Package 0 licenses and dynamic revocation |
| `onlinefix.log` | `LOG_ONLINEFIX_*` | 480 online fix and AppID protection |
| `richpresence.log` | `LOG_RICHPRESENCE_*` | Rich Presence packet injection |
| `steamui.log` | `LOG_STEAMUI_*` | SteamUI state synchronization |
| `inject.log` | `LOG_INJECT_*` | Third-party DLL injection matching & results |
| `pipe.log` | `LOG_PIPE_*` | Pipe handshakes, Denuvo authorization |
| `platform.log` | `LOG_PLATFORM_*` | Platform helpers and remote injection |

---

## Build

### Requirements
- Windows 10/11
- CMake 3.20+
- Visual Studio 2022 with MSVC (x64 toolchain)

### Quick build
```powershell
build.bat
```

### Output
* **Core Components** (in `build/Release/` or `build/Debug/`):
  - `ost-manager.exe`: All-in-One graphical manager and injector utility. Implements self-contained single-binary distribution by internally embedding `OpenSteamTool.dll`, `ost-loader.exe`, `version.dll`, and `dwmapi.dll`
  - `OpenSteamTool.dll`: Core interception and unlocking payload DLL
  - `version.dll` & `dwmapi.dll`: Proxy loaders compiled for Standard DLL Deployment mode (`version.dll` in Steam root, `dwmapi.dll` in `Steam\bin\`)
* **Standalone Companion Tool** (in `build/tools/Release/` or `build/tools/Debug/`):
  - `extract_tickets.exe`: Interactive TUI and clientless online Steam CM credentials extraction utility (excluded from default release archive, explicitly built by `build.bat`)

---

## Acknowledgments
Special thanks to upstream, contributors, and open-source projects:
- [OpenSteam001/OpenSteamTool](https://github.com/OpenSteam001/OpenSteamTool) — Upstream project foundation
- [Selectively11/CloudRedirect](https://github.com/Selectively11/CloudRedirect) — Steam Cloud redirection engine
- [Berkecann](https://github.com/Berkecann) — Contributed the default ManifestDeX manifest provider ([PR #200](https://github.com/OpenSteam001/OpenSteamTool/pull/200))
- [microsoft/Detours](https://github.com/microsoft/Detours) — Binary API hooking library
- [marzer/tomlplusplus](https://github.com/marzer/tomlplusplus) — Header-only TOML parser
- [gabime/spdlog](https://github.com/gabime/spdlog) — Fast C++ logging library

---

## Disclaimer
This project is provided for research and educational purposes only. You are responsible for complying with local laws, platform terms of service, and software licenses.
