<div align="center">
  <img src="docs/logo-animated.svg" width="180" alt="OpenSteamTool logo">

  <h1>OpenSteamTool</h1>

  <p>
    <strong>Herramienta de desbloqueo de Steam de código abierto</strong>
  </p>

  <p>
    <img src="https://img.shields.io/badge/C%2B%2B-20%2B-2ea44f?logo=cplusplus&logoColor=white" alt="C++ 20+">
    <img src="https://img.shields.io/badge/CMake-3.20%2B-2ea44f?logo=cmake&logoColor=white" alt="CMake 3.20+">
    <img src="https://img.shields.io/badge/Windows-only-d73a49?logo=windows&logoColor=white" alt="Solo Windows">
    <a href="https://deepwiki.com/OpenSteam001/OpenSteamTool">
      <img src="https://deepwiki.com/badge.svg" alt="Ask DeepWiki">
    </a>
  </p>

  <p>
    <a href="README.md">
      <img src="https://flagcdn.com/w40/us.png" width="22" alt="Bandera de Estados Unidos">
      English
    </a>
    &nbsp;|&nbsp;
    <a href="README_ES.md">
      <img src="https://flagcdn.com/w40/es.png" width="22" alt="Bandera de España">
      Español
    </a>
    &nbsp;|&nbsp;
    <a href="README_ZH.md">
      <img src="https://flagcdn.com/w40/cn.png" width="22" alt="Bandera de China">
      中文
    </a>
  </p>
</div>

## Características

### Desbloqueo principal y gestión de manifiestos
- Desbloquea juegos y DLCs no adquiridos sin límites
- Carga automática de claves de descifrado de depósitos y tokens de acceso PICS (`addtoken`) desde configuraciones Lua
- Descarga automática de manifiestos a través de `manifestdex` (predeterminado), `opensteamtool`, `steamrun`, `wudrm` o extremos personalizados en Lua
- Bloqueo de versiones de manifiesto para prevenir actualizaciones no deseadas del juego
- Soporte para subdirectorios recursivos en `config/lua/` con sincronización automática de archivos `*.manifest` en `Steam/depotcache/`

### Recarga en caliente (Hot Reload)
- Supervisión en tiempo real de los directorios `.lua`: las adiciones y modificaciones surten efecto al instante sin reiniciar Steam

### Inyección en procesos de juego
- Inyección de DLLs de terceros en procesos de juegos mediante `[[inject]]` en `opensteamtool.toml`
- Soporte para múltiples reglas de DLL, filtrado por línea de comandos y restricciones por AppID (consulte [Reglas de inyección de DLLs de terceros](#inyección-de-dll-de-terceros))
- Lista global integrada de exclusión contra antitrampas (`[injects].exclude_appids`) que protege ~20 títulos competitivos (CS2, Dota 2, Apex, PUBG, etc.) de inyecciones accidentales

### Soporte para Préstamo Familiar (Family Sharing)
- Desbloquea automáticamente las restricciones de préstamo familiar con cero configuración y sin conflictos

### Redirección de guardado en la nube (CloudRedirect)
- Integra [CloudRedirect](https://github.com/Selectively11/CloudRedirect) para permitir guardado en la nube, seguimiento del tiempo de juego y sincronización de logros para juegos gestionados por OST (consulte `[cloud]` en [Configuración](#configuración-opcional))

### Compatibilidad con Denuvo y SteamStub
- **SteamStub**: Suplantación de AppID mediante tickets de ConfigStore local sin inyectar procesos de juego
- **Tickets explícitos**: Gestión exclusivamente en memoria mediante `setAppTicket` / `setETicket` en Lua; sin generar archivos `.bin` en disco
- **Autorización sin conexión entre cuentas**: Sincroniza credenciales directamente en `<AppId>.lua` (escribiendo `setAppTicket`) tras ejecutar en una cuenta legítima; cambie a cuentas sin el juego para jugar sin conexión
- **Bloqueo y actualización de manifiestos**: Escribe `setManifestid` activo de forma predeterminada para fijar versiones en autorizaciones offline. En cuentas con licencia, OST autoriza actualizaciones oficiales y sincroniza manifiestos en segundo plano (configurable mediante `[manifest] lock_owned_games` y `auto_sync_on_update`); use `-nodenuvo` o defina `nodenuvo(appid)` en Lua para omitir Denuvo por completo
- **Concesión adaptativa multietapa (Scheme 2)**: Pase `-dauth2` en los parámetros de lanzamiento o declare `dauth2(appid)` en Lua para activar la extensión dinámica de concesión de tickets en juegos con ventanas de tiempo estrictas
- **Parámetros auxiliares**:
  - `-d+` (alias `-lua`): Parámetro de lanzamiento en cuentas con licencia para autogenerar `<AppId>.lua` y bloquear versiones de manifiesto
  - `-dauth2`: Activa la extensión adaptativa de concesión de tickets (Scheme 2)
  - `-forcedenuvo`: Fuerza el tratamiento del juego como protegido por Denuvo (o `forcedenuvo(appid)`)
  - `-nodenuvo`: Omite el tratamiento de Denuvo (o `nodenuvo(appid)`)

### Extracción de credenciales y configuración con `extract_tickets`
Extrae AppTicket, ETicket, listas de DLCs, claves de depósitos, manifiestos y genera un archivo `<appid>.lua` listo para usar en cuentas con el juego.

* **Interfaz de terminal interactiva (TUI)**: Ejecute `extract_tickets.exe` directamente sin argumentos para abrir una interfaz en consola con cambio de cuentas, exploración de biblioteca y extracción por lotes.
* **Modo en línea sin cliente (`--online` / `-o`)**: Se conecta directamente a los servidores de Steam Connection Manager (CM) a través de TCP/TLS para extraer tickets y claves mediante código QR o token guardado, ¡**sin necesidad de tener Steam instalado o en ejecución**!
* **Descarga**: [Flujo de herramientas en GitHub Actions](https://github.com/mmxlyo/OpenSteamTool/actions/workflows/tools.yml) o compilación local mediante `build.bat` (`build/tools/<Config>/extract_tickets.exe`)
* **Uso**:
  ```powershell
  # Modo TUI interactivo (predeterminado sin argumentos)
  extract_tickets.exe

  # Extracción estándar en línea de comandos (juego instalado localmente)
  extract_tickets.exe 1361510

  # Forzar extracción de ETicket en juegos no instalados
  extract_tickets.exe 1361510 --force-eticket

  # Extracción directa en la nube de Steam CM sin cliente
  extract_tickets.exe --online --appid 1361510
  ```

### Estadísticas y logros
- Habilita estadísticas y logros para juegos no adquiridos
- Prioridad: Lua `setStat(appid, "steamid")` > API de estadísticas (`https://stats.opensteamtool.com/{appid}`) > SteamID predeterminado (`76561198028121353`)

### Reparación multijugador (Online Fix)
- Añada `-onlinefix` a los parámetros de lanzamiento de Steam para multijugador basado en 480 (Spacewar) con protección automática del AppID y guardado real (un solo juego activo a la vez)
- Títulos excepcionales que requieren coincidencia estricta de certificados 480 pueden usar `-onlinefix -p2pflip` (utilizar solo si es estrictamente necesario debido a riesgos de compatibilidad)

---

## Uso

### Método 1: Modo portátil (Recomendado)
No copia ningún archivo a la carpeta de Steam y funciona de manera limpia y portátil:
1. Extraiga el paquete en cualquier carpeta independiente (ej. `D:\OpenSteamTool_Portable`)
2. Cree la carpeta `config/lua/` y agregue sus scripts `.lua`; `opensteamtool.toml` puede ubicarse directamente en este directorio portátil
3. Opciones de lanzamiento y automatización con `ost-manager.exe`:
   - **Lanzamiento / Inyección única**: Haga clic en "Iniciar e inyectar Steam" en la GUI, o ejecute `ost-manager.exe /inject` desde la consola
   - **Intercepción IFEO portátil**: Haga clic en "Instalar IFEO (Modo Portátil)"; al abrir Steam normalmente se inyectará de forma invisible y automática
   - **Monitor de inicio automático**: Haga clic en "Iniciar monitor en segundo plano" para supervisar el inicio de sesión del usuario (sin permisos de administrador, clave `ost_watcher`), o ejecute `ost-manager.exe /watch` por consola

### Método 2: Modo dedicado de la raíz de Steam
En `ost-manager.exe`, haga clic en "Instalar IFEO (Modo Raíz Steam)":
- Despliega automáticamente `OpenSteamTool.dll` y el cargador de seguridad dedicado `ost-loader.exe` en la raíz de Steam
- Vincula la intercepción IFEO para que al iniciar el Steam oficial se inyecte al instante a través del cargador ligero
- Permite la desinstalación limpia con un solo clic en cualquier momento mediante "Desinstalar IFEO"

### Método 3: Modo estándar mediante DLL proxy (Secuestro tradicional de DLL)
Para entornos donde la modificación del registro IFEO esté restringida o no se desee:
- En `ost-manager.exe`, haga clic en "Desplegar DLL estándar" (o ejecute `ost-manager.exe /deploy-dll <SteamDir> <PayloadDll>`)
- Despliega `OpenSteamTool.dll` y `version.dll` en la raíz de Steam, y `dwmapi.dll` en `Steam\bin\`
- Al hacer doble clic en el Steam oficial, se cargará automáticamente mediante la cadena de reenvío PE estándar de Windows
- Se puede desinstalar limpiamente en cualquier momento haciendo clic en "Eliminar DLL" (o ejecutando `ost-manager.exe /remove-dll <SteamDir>`)

### Referencia de línea de comandos del administrador (`ost-manager.exe`)
| Parámetro | Descripción | Permisos |
| :--- | :--- | :--- |
| `/inject`, `-i`, `/silent` | Lanzamiento e inyección silenciosa (inicia Steam si está cerrado, o inyecta en caliente si está abierto) | Usuario estándar |
| `/watch`, `-w`, `/daemon` | Ciclo del demonio de supervisión en segundo plano | Usuario estándar |
| `/enable-autostart` | Habilita el inicio automático del monitor al iniciar sesión (`HKCU\...\Run\ost_watcher`) | Usuario estándar |
| `/disable-autostart` | Deshabilita la entrada de inicio automático del monitor | Usuario estándar |
| `/install-portable <dll>` | Registra la intercepción IFEO portátil apuntando al administrador | Administrador |
| `/install-root <dir> <dll>` | Despliega el cargador en la raíz de Steam y registra IFEO en modo raíz | Administrador |
| `/uninstall` | Desregistra IFEO y elimina los binarios del cargador en la raíz | Administrador |
| `/deploy-dll <dir> <dll>` | Despliega los cargadores proxy (`version.dll` en la raíz, `dwmapi.dll` en `bin\`) | Usuario / Admin |
| `/remove-dll <dir>` | Elimina las DLLs proxy de las carpetas de Steam | Usuario / Admin |

---

## Ejemplo de configuración Lua

```lua
addappid(1361510) -- desbloquea el juego
addappid(1361511, 0, "5954562e7f5260400040a818bc29b60b335bb690066ff767e20d145a3b6b4af0") -- desbloquea depósito con clave
addtoken(1361510, "2764735786934684318") -- añade token de acceso PICS

setManifestid(1361511, "5656605350306673283") -- fija versión de manifiesto
setManifestid(1361511, "5656605350306673283", 12345678) -- fija versión de manifiesto con tamaño

setAppTicket(1361510, "0100000000000000...") -- AppTicket en memoria
setETicket(1361510, "0100000000000000...")   -- ETicket en memoria
setStat(1361510, "76561197960287930")        -- SteamID de origen para logros

addprocess(1361510, "CustomGame.exe")        -- asigna AppID a procesos sin variable SteamAppId
seteticketurl("https://example.com/eticket") -- extremo para generación dinámica de ETicket (opcional)

dauth2(1361510)      -- activa concesión adaptativa Scheme 2 para Denuvo
nodenuvo(1361510)    -- omite Denuvo (equivalente a -nodenuvo, alias: disallowdenuvo)
forcedenuvo(1361510) -- fuerza tratamiento Denuvo (equivalente a -forcedenuvo)
```
Todos los nombres de funciones **no distinguen entre mayúsculas y minúsculas**.

---

## Configuración (opcional)

Cambie el nombre de `opensteamtool.example.toml` (o plantillas localizadas `opensteamtool.example_zh.toml` / `opensteamtool.example_es.toml`) a `opensteamtool.toml` y colóquelo en el directorio portátil o en la raíz de Steam. Se recarga en caliente automáticamente tras cualquier cambio.

```toml
[log]
# Solo en compilaciones Debug: trace, debug, info, warn, error
level = "info"
# Directorio para archivos de registro (opcional, solo Debug)
# dir = "opensteamtool"

[manifest]
# API remota: "manifestdex" (predeterminado), "opensteamtool", "steamrun", "wudrm"
url = "manifestdex"
timeout_resolve_ms = 5000
timeout_connect_ms = 5000
timeout_send_ms    = 10000
timeout_recv_ms    = 10000
# Permitir actualizaciones oficiales de Steam para juegos/DLCs en propiedad (predeterminado: false)
lock_owned_games = false
# Sincronización automática de Lua y manifiestos en segundo plano tras actualizar (predeterminado: true)
auto_sync_on_update = true

[stats]
# Consultar API de estadísticas cuando setStat no esté presente
enable_api = true

[lua]
# Directorios Lua adicionales a cargar (opcional)
paths = []

[cloud]
# Redirección de Steam Cloud a través de CloudRedirect
enabled = false
# library = "cloud_redirect.dll"

# Configuración global de inyección de DLL (lista de exclusión)
[injects]
# AppIDs estrictamente excluidos de la inyección de DLL en procesos de juego
# Nota: La exclusión SOLO omite la inyección de DLLs en el juego; ¡el desbloqueo y la emulación de DLCs nunca se ven afectados!
exclude_appids = [
    730,      # Counter-Strike 2 (VAC)
    570,      # Dota 2 (VAC)
    1172470,  # Apex Legends (EAC)
    578080,   # PUBG: BATTLEGROUNDS (BattlEye)
    # Consulte opensteamtool.example_es.toml para la lista completa
]

# Reglas individuales de inyección de bibliotecas (tabla de matrices, comentadas por defecto)
# [[inject]]
# path = "OnlineFix.dll"
# all_games = false
# when_cmdline = "-onlinefix"
# when_appids = [1361510]

[remote]
# Espejo de metadatos de patrones (por defecto GitHub con respaldo jsDelivr)
# url_template = "https://your.server/{channel}/{component}/{sha256}.toml"
```

### Ajustes globales de inyección (`[injects]`)
| Clave | Explicación |
| :--- | :--- |
| `exclude_appids` | Lista de AppIDs estrictamente excluidos de la inyección de DLL de terceros en los procesos de juego.<br>- **Protección universal**: Bloquea estrictamente la inyección de DLL en todas las reglas salvo que se anule explícitamente en la lista blanca `when_appids` de una regla;<br>- **Principio de anulación explícita**: Si se configura explícitamente (incluso con una lista vacía `[]`), se excluyen estrictamente solo esos AppIDs (**la lista predeterminada no se combina**);<br>- **Principio de respaldo predeterminado**: Si se omite por completo, utiliza automáticamente la lista integrada de protección (~20 títulos competitivos como CS2, Dota 2, Apex, PUBG, etc.);<br>- **Prioridad de lista blanca**: Si un AppID está excluido aquí pero se especifica en el `when_appids` de una regla `[[inject]]`, la **lista blanca tiene prioridad** y el AppID **no será excluido** para esa regla.<br>**Nota: ¡Esta exclusión solo impide la inyección de DLLs en procesos de juego; el desbloqueo de juegos y DLCs (`addappid`, manifiestos, tickets, etc.) se mantiene 100% activo!** |

<a id="inyección-de-dll-de-terceros"></a>
### Reglas de inyección de DLL de terceros (`[[inject]]`)
| Clave | Explicación |
| :--- | :--- |
| `path` | DLL a cargar. Los nombres de archivo simples se resuelven junto al toml, el directorio de la DLL o la raíz de Steam; las rutas absolutas se usan tal cual |
| `all_games` | Opcional. Predeterminado en `false` (muy recomendado).<br>- **`false`**: Solo apunta a juegos desbloqueados mediante Lua. **Importante: Si se omite `when_appids`, ¡se inyectarán TODOS los juegos desbloqueados por Lua! Se recomienda encarecidamente especificar `when_appids` o `when_cmdline` (ej. `"-onlinefix"`) para restringir la inyección**;<br>- **`true`**: Interruptor global. Inyecta en todos los juegos iniciados mediante Steam, protegido por `[injects].exclude_appids` |
| `when_cmdline` | Opcional. Subcadena requerida en la línea de comandos de inicio (ej. `"-onlinefix"`) |
| `when_appids` | Opcional. Mecanismo de lista blanca para AppIDs específicos (ej. `[1361510]`). **Tiene prioridad sobre la lista de exclusión** |

### Manifiesto mediante Lua
Si se definen en `config/lua/`, estas funciones tienen prioridad sobre las APIs HTTP remotas configuradas:

- `fetch_manifest_code_ex(app_id, depot_id, gid)` *(Recomendado)*: Variante extendida que recibe `app_id`, `depot_id` y `gid`
- `fetch_manifest_code(gid)`: Variante base que recibe solo el GID del manifiesto

Funciones auxiliares HTTP integradas proporcionadas por el entorno de ejecución C++:
| Función | Firma | Valor de retorno |
| :--- | :--- | :--- |
| `http_get` | `http_get(url [, headers])` | `body, status_code` |
| `http_post` | `http_post(url, body [, headers])` | `body, status_code` |

`headers` es una tabla opcional: `{["Key"]="Value", ...}`

---

## Registros de depuración (Debug logs)

Las compilaciones Debug escriben registros modulares en `<Steam o Carpeta Portátil>/opensteamtool/`:

| Archivo | Origen | Contenido |
| :--- | :--- | :--- |
| `main.log` | General | Inicio, carga de configuración, análisis Lua |
| `ipc.log` | `LOG_IPC_*` | Comandos IPC, despacho de interfaces, suplantación |
| `netpacket.log` | `LOG_NETPACKET_*` | Intercepción de paquetes de red, despacho eMsg |
| `manifest.log` | `LOG_MANIFEST_*` | Descarga de manifiestos, reflejo en depotcache |
| `decryptionkey.log` | `LOG_DECRYPTIONKEY_*` | Inyección de claves de descifrado de depósitos |
| `keyvalue.log` | `LOG_KEYVALUE_*` | Parcheo de KeyValues de manifiestos |
| `misc.log` | `LOG_MISC_*` | Punteros de motor, mapeo de AppID |
| `achievement.log` | `LOG_ACHIEVEMENT_*` | Gestión de estadísticas y logros |
| `pics.log` | `LOG_PICS_*` | Inyección de tokens de acceso PICS |
| `package.log` | `LOG_PACKAGE_*` | Licencias de Package 0 y revocación dinámica |
| `onlinefix.log` | `LOG_ONLINEFIX_*` | Corrección online 480 y protección de AppID |
| `richpresence.log` | `LOG_RICHPRESENCE_*` | Inyección de paquetes Rich Presence |
| `steamui.log` | `LOG_STEAMUI_*` | Sincronización de estado de SteamUI |
| `inject.log` | `LOG_INJECT_*` | Coincidencias y resultados de inyección de DLLs |
| `pipe.log` | `LOG_PIPE_*` | Comunicación por tuberías, autorización Denuvo |
| `platform.log` | `LOG_PLATFORM_*` | Utilidades de plataforma e inyección remota |

---

## Compilación

### Requisitos
- Windows 10/11
- CMake 3.20+
- Visual Studio 2022 con MSVC (cadena de herramientas x64)

### Compilación rápida
```powershell
build.bat
```

### Salida
* **Componentes principales** (en `build/Release/` o `build/Debug/`):
  - `ost-manager.exe`: Utilidad gráfica integral de administración e inyección (All-in-One). Integra empaquetado autónomo mediante compresión transparente de `OpenSteamTool.dll`, `ost-loader.exe`, `version.dll` y `dwmapi.dll`
  - `OpenSteamTool.dll`: DLL de carga útil principal para interceptación y desbloqueo
  - `version.dll` y `dwmapi.dll`: Cargadores proxy para el modo de despliegue DLL estándar (`version.dll` en la raíz de Steam, `dwmapi.dll` en `Steam\bin\`)
* **Herramienta independiente complementaria** (en `build/tools/Release/` o `build/tools/Debug/`):
  - `extract_tickets.exe`: Utilidad de extracción de credenciales con interfaz TUI interactiva y conexión directa a Steam CM sin cliente (excluida del paquete principal, compilada mediante `build.bat`)

---

## Agradecimientos
Agradecimiento especial al proyecto original, colaboradores y proyectos de código abierto:
- [OpenSteam001/OpenSteamTool](https://github.com/OpenSteam001/OpenSteamTool) — Base del proyecto original
- [Selectively11/CloudRedirect](https://github.com/Selectively11/CloudRedirect) — Motor de redirección de Steam Cloud
- [Berkecann](https://github.com/Berkecann) — Contribuyó con el proveedor de manifiestos ManifestDeX ([PR #200](https://github.com/OpenSteam001/OpenSteamTool/pull/200))
- [microsoft/Detours](https://github.com/microsoft/Detours) — Biblioteca de enlace binario de APIs
- [marzer/tomlplusplus](https://github.com/marzer/tomlplusplus) — Analizador TOML moderno basado en encabezados
- [gabime/spdlog](https://github.com/gabime/spdlog) — Biblioteca rápida de registro en C++

---

## Descargo de responsabilidad
Este proyecto se proporciona únicamente con fines educativos y de investigación. Usted es responsable de cumplir con las leyes locales, los términos de servicio de la plataforma y las licencias de software.
