<div align="center">
  <img src="docs/logo-animated.svg" width="180" alt="OpenSteamTool logo">

  <h1>OpenSteamTool</h1>

  <p>
    <strong>开源 Steam 解锁工具</strong>
  </p>

  <p>
    <img src="https://img.shields.io/badge/C%2B%2B-20%2B-2ea44f?logo=cplusplus&logoColor=white" alt="C++ 20+">
    <img src="https://img.shields.io/badge/CMake-3.20%2B-2ea44f?logo=cmake&logoColor=white" alt="CMake 3.20+">
    <img src="https://img.shields.io/badge/Windows-only-d73a49?logo=windows&logoColor=white" alt="仅 Windows">
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

## 功能特性

### 核心解锁与清单管理
- 解锁任意数量未拥有的游戏及全部 DLC
- 支持从 Lua 配置自动加载仓库（Depot）解密密钥与 PICS 访问令牌（`addtoken`）
- 支持通过 `manifestdex`（默认）、`opensteamtool`、`steamrun`、`wudrm` API 或自定义 Lua 端点自动下载 Manifest
- 支持固定 Manifest 版本以阻止游戏更新
- `config/lua/` 支持多层级嵌套子目录，且其中的 `*.manifest` 文件会自动镜像同步至 `Steam/depotcache/`

### 热重载
- 自动监视配置目录中的 `.lua` 文件，新增或修改即刻生效，无需重启 Steam

### 游戏进程注入
- 通过 `opensteamtool.toml` 中的 `[[inject]]` 将第三方 DLL 注入到游戏进程
- 支持配置多个 DLL、启动参数过滤及 AppID 限制（参见 [第三方 DLL 注入](#第三方-dll-注入)）
- 内置全局反作弊保护排除名单（`[injects].exclude_appids`），默认保护约 20 款主流竞技游戏（CS2、Dota 2、Apex、PUBG 等），严格隔离注入风险

### 家庭共享支持
- 自动解锁家庭共享限制，互不影响，无需配置

### 云存档重定向 (CloudRedirect)
- 集成 [CloudRedirect](https://github.com/Selectively11/CloudRedirect)，支持 OST 托管的游戏使用 CloudRedirect 获得存档、游戏时长与成就数据云同步的能力（参见 [配置](#配置可选) 中的 `[cloud]`）

### Denuvo 和 SteamStub 兼容
- **SteamStub**：无需配置票据，自动通过本地 ConfigStore 伪造 AppID
- **显式票据**：在 Lua 中通过 `setAppTicket` / `setETicket` 注入，直接在内存中管理，无需生成磁盘 bin 文件
- **切号离线授权**：正版账号运行后自动将凭据同步至 `<AppId>.lua`（自动写入 `setAppTicket`），切换无游戏账号即可离线游玩
- **清单锁定与更新控制**：默认写入活跃的 `setManifestid` 锁定已安装版本以保护离线授权。在已授权的账号上，OST 会自动放行官方更新并自动静默同步最新清单（亦可通过 `[manifest] lock_owned_games` 与 `auto_sync_on_update` 配置）；若需完全跳过某游戏的 Denuvo 处理，可传入 `-nodenuvo` 或在 Lua 中配置 `nodenuvo(appid)`
- **多阶段自适应租约 (Scheme 2)**：针对启动握手窗口严苛的游戏，传入 `-dauth2` 或在 Lua 中配置 `dauth2(appid)` 即可启用自适应时间窗口动态续期
- **辅助参数**：
  - `-d+`（别名 `-lua`）：正版账号启动项参数，一键自动生成该游戏的 `<AppId>.lua` 并锁定清单版本
  - `-dauth2`：启用 Scheme 2 自适应时间窗口动态续期
  - `-forcedenuvo`：强制将游戏按 Denuvo 保护处理（Lua 中配置 `forcedenuvo(appid)`）
  - `-nodenuvo`：跳过 Denuvo 处理（Lua 中配置 `nodenuvo(appid)`）

### 使用 `extract_tickets` 提取凭据与配置
用于在拥有游戏的账号上提取 AppTicket、ETicket、DLC 列表、Depot 密钥、Manifest 并生成开箱即用的 `<appid>.lua`。

* **交互式终端界面 (TUI)**：直接无参运行 `extract_tickets.exe` 即可进入全功能终端图形界面（支持一级快捷键 AppID 录入、二级本地多账号切换管理、三级在线游戏库模糊检索、分页与批量提取）。
* **免客户端在线云端提取 (`--online` / `-o`)**：直连 Valve CM（连接管理器）服务器，支持扫码登录和保存凭据提取，**本地无需安装或运行 Steam 客户端**！
* **获取方式**：[GitHub Actions Tools 页面](https://github.com/mmxlyo/OpenSteamTool/actions/workflows/tools.yml) 或本地 `build.bat` 编译（产物位于 `build/tools/<Config>/extract_tickets.exe`）
* **使用方法**：
  ```powershell
  # 交互式 TUI 终端模式（直接运行即可进入）
  extract_tickets.exe

  # 标准命令行提取（本地已安装游戏）
  extract_tickets.exe 1361510

  # 未安装游戏强制提取 ETicket
  extract_tickets.exe 1361510 --force-eticket

  # 免 Steam 客户端云端直接提取
  extract_tickets.exe --online --appid 1361510
  ```

### 统计和成就
- 为未拥有游戏启用成就与统计
- 优先级：Lua `setStat(appid, "steamid")` > stats API（`https://stats.opensteamtool.com/{appid}`）> 默认 SteamID（`76561198028121353`）

### 联机修复 (Online Fix)
- 在 Steam 启动选项中添加 `-onlinefix` 启用基于 480 (Spacewar) 的在线联机，自动保护真实存档与 AppID（同一时间仅运行一个此类游戏）
- 极少数需要匹配 480 证书的游戏可使用 `-onlinefix -p2pflip`（可能存在兼容性问题，非必要不建议使用）

---

## 使用方法

### 方式一：便携模式（推荐）
无需向 Steam 目录复制任何文件，完全绿色便携运行：
1. 解压发布包到任意独立目录（如 `D:\OpenSteamTool_Portable`）
2. 在该目录下创建 `config/lua/` 文件夹并放入 `.lua` 解锁脚本；若需自定义配置，可将 `opensteamtool.toml` 直接放在此目录下
3. 运行 `ost-manager.exe`：
   - **冷启动拉起并注入**：点击主界面「冷启动拉起并注入」，或命令行执行 `ost-manager.exe /inject`
   - **便携 IFEO 自动劫持**：点击「安装 IFEO (便携模式)」，直接启动官方 Steam 快捷方式即可自动无感注入
   - **开机自动监听**：点击「启动后台守护监听」开启后台静默监听，在系统登录时自动捕获 Steam 启动并注入（无需管理员权限，注册表项 `ost_watcher`），或命令行执行 `ost-manager.exe /watch`

### 方式二：Steam 根目录专属模式
在 `ost-manager.exe` 界面中点击「安装 IFEO (根目录模式)」：
- 自动将 `OpenSteamTool.dll` 与专属安全加载器 `ost-loader.exe` 部署至 Steam 根目录
- 自动绑定系统 IFEO 劫持，双击官方 Steam 即可由轻量级加载器实现毫秒级注入
- 支持随时在管理器中点击「卸载并清理 IFEO」一键还原清理

### 方式三：标准代理 DLL 模式（免注册表 DLL 劫持 / 手动放置）
适合受组策略或安全策略限制、不希望修改系统 IFEO 注册表的环境：
- **管理器一键部署**：在 `ost-manager.exe` 界面中点击「部署标准DLL」（或命令行执行 `ost-manager.exe /deploy-dll <SteamDir> <PayloadDll>`），支持随时点击「移除DLL」完全卸载清理
- **手动放置位置**：
  - `OpenSteamTool.dll`：复制到 Steam 根目录（如 `C:\Program Files (x86)\Steam\OpenSteamTool.dll`）
  - `version.dll`：复制到 Steam 根目录（如 `C:\Program Files (x86)\Steam\version.dll`）
  - `dwmapi.dll`：复制到 Steam 的 `bin` 子目录（如 `C:\Program Files (x86)\Steam\bin\dwmapi.dll`，**请勿**放置于根目录）
  - Lua 解锁脚本放置于 `<Steam>\config\lua\` 目录下
- 双击官方 Steam 快捷方式即可通过标准 Windows PE 动态库转发链自动加载

### 管理器命令行参考 (`ost-manager.exe`)
| 参数 | 说明 | 所需权限 |
| :--- | :--- | :--- |
| `/inject`, `-i`, `/silent` | 无窗口单次静默拉起与注入（未运行则启动 Steam，已运行则热注入） | 普通用户 |
| `/watch`, `-w`, `/daemon` | 无窗口后台守护监听循环 | 普通用户 |
| `/enable-autostart` | 启用登录开机自启后台守护 (`HKCU\...\Run\ost_watcher`) | 普通用户 |
| `/disable-autostart` | 禁用登录开机自启后台守护 | 普通用户 |
| `/install-portable <dll>` | 注册指向管理器的便携模式 IFEO 拦截 | 管理员 |
| `/install-root <dir> <dll>` | 部署专属加载器到 Steam 根目录并注册根目录模式 IFEO | 管理员 |
| `/uninstall` | 注销系统 IFEO 拦截并清理根目录加载残留 | 管理员 |
| `/deploy-dll <dir> <dll>` | 部署代理加载器（根目录 `version.dll`，`bin\dwmapi.dll`） | 普通用户 / 管理员 |
| `/remove-dll <dir>` | 从 Steam 目录中彻底清理代理 DLL | 普通用户 / 管理员 |

---

## Lua 配置示例

```lua
addappid(1361510) -- 解锁游戏
addappid(1361511, 0, "5954562e7f5260400040a818bc29b60b335bb690066ff767e20d145a3b6b4af0") -- 解锁 depot 并配置密钥
addtoken(1361510, "2764735786934684318") -- 添加 PICS 访问令牌

setManifestid(1361511, "5656605350306673283") -- 锁定 depot 清单版本
setManifestid(1361511, "5656605350306673283", 12345678) -- 锁定清单并指定大小

setAppTicket(1361510, "0100000000000000...") -- 存入内存 AppTicket
setETicket(1361510, "0100000000000000...")   -- 存入内存 ETicket
setStat(1361510, "76561197960287930")        -- 指定成就数据拉取源 SteamID

addprocess(1361510, "CustomGame.exe")         -- 为无 SteamAppId 环境变量的进程映射 AppID
seteticketurl("https://example.com/eticket")  -- 在线动态获取 ETicket 端点（可选）

dauth2(1361510)      -- 为 Denuvo 启用 Scheme 2 自适应租约续期
nodenuvo(1361510)    -- 跳过 Denuvo 处理（等价于启动项 -nodenuvo，别名: disallowdenuvo）
forcedenuvo(1361510) -- 强制标记为 Denuvo（等价于启动项 -forcedenuvo）

setlegacycdkey(1361510, "ABCD-EFGH-JKMN-PQRS") -- 在“Updating product key”步骤中提供此第三方零售 CD 密钥
-- 如果未配置，则会为每个应用+账户生成一个确定性的合成密钥（仅满足 Steam 的本地步骤）
```
所有函数名**不区分大小写**。

---

## 配置（可选）

将 `opensteamtool.example.toml`（或中文注释版 `opensteamtool.example_zh.toml`）重命名为 `opensteamtool.toml`，放置于便携目录根目录或 Steam 根目录。修改后自动热重载。

```toml
[log]
# 仅调试构建有效：trace, debug, info, warn, error
level = "info"
# 日志输出目录（可选，仅调试构建有效）
# dir = "opensteamtool"

[manifest]
# 上游 API："manifestdex"（默认）、"opensteamtool"、"steamrun"、"wudrm"
url = "manifestdex"
timeout_resolve_ms = 5000
timeout_connect_ms = 5000
timeout_send_ms    = 10000
timeout_recv_ms    = 10000
# 是否对当前账号已拥有的游戏/DLC放行官方更新（false 为放行；true 为强制锁定版本，默认：false）
lock_owned_games = false
# 拥有授权的游戏在 Steam 完成后台下载更新后自动静默同步清单与 Lua（默认：true）
auto_sync_on_update = true

[stats]
# 未配置 setStat 时查询 https://stats.opensteamtool.com/{appid}
enable_api = true

[lua]
# 额外加载的 Lua 配置目录列表（可选）
paths = []

[cloud]
# 启用基于 CloudRedirect 的云存档透明重定向（需配合其伴侣客户端使用）
enabled = false
# library = "cloud_redirect.dll"

# 全局游戏注入配置（排除名单）
[injects]
# 排除注入的 AppID 名单（显式配置严格按实际排除，不再合并默认列表；未配置则默认保护 ~20 款竞技反作弊游戏）
# 注意：排除仅跳过向游戏进程注入 DLL，绝不影响游戏的正常解锁与 DLC 模拟！
exclude_appids = [
    730,      # Counter-Strike 2 (VAC)
    570,      # Dota 2 (VAC)
    1172470,  # Apex Legends (EAC)
    578080,   # PUBG: BATTLEGROUNDS (BattlEye)
    # 更多预置项参见 opensteamtool.example_zh.toml
]

# 独立 DLL 注入规则（表数组，可配置多个，默认全部注释）
# [[inject]]
# path = "OnlineFix.dll"
# all_games = false
# when_cmdline = "-onlinefix"
# when_appids = [1361510]

[remote]
# 可选特征码元数据镜像（默认优先 GitHub，自动回退 jsDelivr）
# url_template = "https://your.server/{channel}/{component}/{sha256}.toml"
```

### 全局注入设置 (`[injects]`)
| 字段 | 说明 |
| :--- | :--- |
| `exclude_appids` | 排除向其注入第三方 DLL 的目标 AppID 列表。<br>- **全局保底保护**：无论注入规则是全局还是局部，只要目标在此名单中，一律严格跳过注入（除非在具体规则的 `when_appids` 中被显式加入白名单）；<br>- **显式覆盖原则**：若显式配置了本项（即使列表为空 `[]` 或仅配置单款游戏），则严格按用户实际配置排除，**不再包含默认排除名单**；<br>- **默认保底原则**：若完全未配置本项，自动采用内置的 20 余款竞技/反作弊网游保底名单（涵盖 CS2、Dota 2、Apex、PUBG、R6S 等）；<br>- **白名单优先原则**：若某 AppID 在此被排除，但在具体 `[[inject]]` 规则的 `when_appids` 中被显式指定，则**白名单优先于排除名单**，该 AppID 在该规则下**不被排除**；<br>**注意：此排除严格仅排除向游戏进程注入第三方 DLL，绝不影响任何游戏的正常解锁与 DLC 模拟（`addappid`、清单下载、票据伪造等 100% 正常运行）！** |

<a id="第三方-dll-注入"></a>
### 第三方 DLL 注入规则 (`[[inject]]`)
| 字段 | 说明 |
| :--- | :--- |
| `path` | 待注入的目标 DLL。纯文件名优先从 toml 所在目录、DLL 所在目录或 Steam 根目录解析；绝对路径直接使用 |
| `all_games` | 可选，默认为 `false`（强烈推荐）。<br>- **`false`**：仅针对通过 Lua 解锁的游戏。**注意：若未配置 `when_appids`，启动任何 Lua 解锁游戏均会触发注入！强烈建议配合 `when_appids`（白名单 AppID）或 `when_cmdline`（如 `"-onlinefix"`）精准限定**；<br>- **`true`**：全局注入开关。向 Steam 启动的所有游戏（含已拥有正版与免费游戏）注入，受上方 `[injects].exclude_appids` 严格防护 |
| `when_cmdline` | 可选。启动参数中必须包含的子字符串（如 `"-onlinefix"`） |
| `when_appids` | 可选。精确限定注入的目标 AppID 白名单列表（如 `[1361510]`）。**优先级高于排除名单**：即使目标 AppID 位于排除名单中，在此显式指定仍会对此规则放行注入 |

### 通过 Lua 自定义 Manifest
若在 `config/lua/` 中定义了以下函数，将优先于配置的远程 HTTP API 执行：

- `fetch_manifest_code_ex(app_id, depot_id, gid)` *(推荐)*：扩展版本，接收 `app_id`、`depot_id` 和 `gid`，适用于需要应用上下文的 API 端点
- `fetch_manifest_code(gid)`：基础版本，仅接收清单 GID

C++ 运行时提供的内置 HTTP 辅助函数：
| 函数 | 签名 | 返回值 |
| :--- | :--- | :--- |
| `http_get` | `http_get(url [, headers])` | `body, status_code` |
| `http_post` | `http_post(url, body [, headers])` | `body, status_code` |

`headers` 为可选表结构：`{["Key"]="Value", ...}`

---

## 调试日志

调试构建会将模块日志输出至 `<Steam 或便携目录>/opensteamtool/`：

| 文件名 | 日志宏 | 内容说明 |
| :--- | :--- | :--- |
| `main.log` | 通用 | 初始化、配置加载、Lua 解析 |
| `ipc.log` | `LOG_IPC_*` | IPC 命令、接口调度与伪造 |
| `netpacket.log` | `LOG_NETPACKET_*` | 网络封包拦截与 eMsg 派发 |
| `manifest.log` | `LOG_MANIFEST_*` | 清单下载与 depotcache 镜像同步 |
| `decryptionkey.log` | `LOG_DECRYPTIONKEY_*` | Depot 解密密钥注入 |
| `keyvalue.log` | `LOG_KEYVALUE_*` | KeyValues 清单热修补 |
| `misc.log` | `LOG_MISC_*` | 引擎指针捕获与 AppID 映射 |
| `achievement.log` | `LOG_ACHIEVEMENT_*` | 统计与成就处理 |
| `pics.log` | `LOG_PICS_*` | PICS 访问令牌注入 |
| `package.log` | `LOG_PACKAGE_*` | Package 0 许可证与动态吊销 |
| `onlinefix.log` | `LOG_ONLINEFIX_*` | 480 联机修复与真实 AppID 保护 |
| `richpresence.log` | `LOG_RICHPRESENCE_*` | 富状态（Rich Presence）数据包注入 |
| `steamui.log` | `LOG_STEAMUI_*` | SteamUI 界面状态同步 |
| `inject.log` | `LOG_INJECT_*` | 第三方 DLL 注入规则匹配与执行结果 |
| `pipe.log` | `LOG_PIPE_*` | 管道握手与 Denuvo 授权处理 |
| `platform.log` | `LOG_PLATFORM_*` | 平台底层辅助与跨进程注入 |

---

## 编译构建

### 环境要求
- Windows 10/11
- CMake 3.20+
- Visual Studio 2022（需安装 MSVC x64 工具链）

### 快速编译
```powershell
build.bat
```

### 输出产物
* **核心组件**（位于 `build/Release/` 或 `build/Debug/`）：
  - `ost-manager.exe`：全功能自包含图形化管理与注入器（内部透明压缩内嵌 `OpenSteamTool.dll`、`ost-loader.exe`、`version.dll`、`dwmapi.dll`，实现单文件开箱即用分发）
  - `OpenSteamTool.dll`：核心劫持与解锁动态库
  - `version.dll` & `dwmapi.dll`：专供标准代理模式的加载器（`version.dll` 位于 Steam 根目录，`dwmapi.dll` 位于 `Steam\bin\`）
* **独立伴侣工具**（位于 `build/tools/Release/` 或 `build/tools/Debug/`）：
  - `extract_tickets.exe`：全功能交互式 TUI 与脱机直连 Steam CM 凭据提取工具（主发布包默认排除，由 `build.bat` 显式编译）

---

## 鸣谢
特别感谢上游项目、贡献者及开源组件：
- [OpenSteam001/OpenSteamTool](https://github.com/OpenSteam001/OpenSteamTool) — 上游项目基石
- [Selectively11/CloudRedirect](https://github.com/Selectively11/CloudRedirect) — Steam 云存档重定向引擎
- [Berkecann](https://github.com/Berkecann) — 贡献默认 ManifestDeX 清单服务提供者 ([PR #200](https://github.com/OpenSteam001/OpenSteamTool/pull/200))
- [microsoft/Detours](https://github.com/microsoft/Detours) — 二进制 API 挂钩库
- [marzer/tomlplusplus](https://github.com/marzer/tomlplusplus) — 仅头文件现代 TOML 解析器
- [gabime/spdlog](https://github.com/gabime/spdlog) — 高性能 C++ 日志库

---

## 免责声明
本项目仅供研究与学习使用。用户须自行承担使用风险，并遵守当地法律法规、平台服务条款以及软件许可协议。
