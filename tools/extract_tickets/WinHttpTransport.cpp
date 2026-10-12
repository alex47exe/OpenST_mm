#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include "WinHttpTransport.h"
#include "I18n.h"
#include "Log.h"
#include "Utils.h"

#include <array>
#include <chrono>
#include <type_traits>

#pragma comment(lib, "winhttp.lib")

namespace OST::ExtractTickets {

static_assert(std::is_same_v<HINTERNET, void*>, "HINTERNET must be void*");
static_assert(std::is_same_v<INTERNET_PORT, uint16_t>, "INTERNET_PORT must be uint16_t");
static_assert(sizeof(DWORD) == sizeof(uint32_t), "DWORD must be 32-bit unsigned");

namespace {

    struct ParsedUrl {
        std::wstring host;
        std::wstring path;
        INTERNET_PORT port{INTERNET_DEFAULT_HTTPS_PORT};
        bool isHttps{true};
    };

    bool ParseUrl(std::string_view urlStr, ParsedUrl& outParsed) {
        std::string normalizedUrl(urlStr);
        bool isWss = false;
        bool isWs = false;
        if (normalizedUrl.starts_with("wss://")) {
            isWss = true;
            normalizedUrl.replace(0, 6, "https://");
        } else if (normalizedUrl.starts_with("ws://")) {
            isWs = true;
            normalizedUrl.replace(0, 5, "http://");
        }

        std::wstring wideUrl = Utf8ToWide(normalizedUrl);
        URL_COMPONENTS urlComp{};
        urlComp.dwStructSize = sizeof(urlComp);
        urlComp.dwHostNameLength = static_cast<DWORD>(-1);
        urlComp.dwUrlPathLength = static_cast<DWORD>(-1);
        urlComp.dwExtraInfoLength = static_cast<DWORD>(-1);

        if (!WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0, &urlComp)) {
            LOG_WARN("WinHttp", MsgKey::LogHttpCrackUrlFailed, MaskUrl(urlStr), GetLastError());
            return false;
        }

        outParsed.host.assign(urlComp.lpszHostName, urlComp.dwHostNameLength);
        outParsed.path.assign(urlComp.lpszUrlPath, urlComp.dwUrlPathLength);
        if (urlComp.dwExtraInfoLength > 0) {
            outParsed.path.append(urlComp.lpszExtraInfo, urlComp.dwExtraInfoLength);
        }
        if (outParsed.path.empty()) {
            outParsed.path = L"/";
        }

        outParsed.port = urlComp.nPort;
        if (isWss) {
            outParsed.isHttps = true;
            if (outParsed.port == 0 || outParsed.port == INTERNET_DEFAULT_HTTPS_PORT) {
                outParsed.port = INTERNET_DEFAULT_HTTPS_PORT;
            }
        } else if (isWs) {
            outParsed.isHttps = false;
            if (outParsed.port == 0 || outParsed.port == INTERNET_DEFAULT_HTTP_PORT) {
                outParsed.port = INTERNET_DEFAULT_HTTP_PORT;
            }
        } else {
            outParsed.isHttps = (urlComp.nScheme == INTERNET_SCHEME_HTTPS);
        }
        return true;
    }

    struct ScopedHInternet {
        HINTERNET handle{nullptr};
        ScopedHInternet() = default;
        explicit ScopedHInternet(HINTERNET h) noexcept : handle(h) {}
        ~ScopedHInternet() noexcept {
            if (handle) WinHttpCloseHandle(handle);
        }
        ScopedHInternet(const ScopedHInternet&) = delete;
        ScopedHInternet& operator=(const ScopedHInternet&) = delete;
        ScopedHInternet(ScopedHInternet&& o) noexcept : handle(o.handle) { o.handle = nullptr; }
        ScopedHInternet& operator=(ScopedHInternet&& o) noexcept {
            if (this != &o) {
                if (handle) WinHttpCloseHandle(handle);
                handle = o.handle;
                o.handle = nullptr;
            }
            return *this;
        }
        operator HINTERNET() const noexcept { return handle; }
        void Reset(HINTERNET h = nullptr) noexcept {
            if (handle) WinHttpCloseHandle(handle);
            handle = h;
        }
    };
} // namespace

// ============================================================================
// WinHttpTransport (HTTPS Client)
// ============================================================================

WinHttpTransport::WinHttpTransport() {
    m_hSession = WinHttpOpen(
        L"OpenSteamTool-ExtractTickets/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);

    if (!m_hSession) {
        m_hSession = WinHttpOpen(
            L"OpenSteamTool-ExtractTickets/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);
    }

    if (!m_hSession) {
        LOG_ERROR("WinHttp", MsgKey::LogHttpOpenFailed, GetLastError());
    } else {
        // Enforce TLS 1.2 and TLS 1.3
        DWORD secureProtocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        WinHttpSetOption(m_hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &secureProtocols, sizeof(secureProtocols));

        // Timeouts: resolve, connect, send, receive (all 15s)
        WinHttpSetTimeouts(m_hSession, 15000, 15000, 15000, 15000);
    }
}

WinHttpTransport::~WinHttpTransport() {
    InvalidateConnection();
    if (m_hSession) {
        WinHttpCloseHandle(m_hSession);
        m_hSession = nullptr;
    }
}

void WinHttpTransport::SetTimeouts(int resolveMs, int connectMs, int sendMs, int receiveMs) {
    if (m_hSession) {
        WinHttpSetTimeouts(m_hSession, resolveMs, connectMs, sendMs, receiveMs);
    }
}

void* WinHttpTransport::GetOrCreateConnection(const std::wstring& host, uint16_t port) {
    if (m_hCachedConnect && m_cachedConnectHost == host && m_cachedConnectPort == port) {
        return m_hCachedConnect;
    }
    InvalidateConnection();
    m_hCachedConnect = WinHttpConnect(m_hSession, host.c_str(), port, 0);
    if (m_hCachedConnect) {
        m_cachedConnectHost = host;
        m_cachedConnectPort = port;
    }
    return m_hCachedConnect;
}

void WinHttpTransport::InvalidateConnection() {
    if (m_hCachedConnect) {
        WinHttpCloseHandle(m_hCachedConnect);
        m_hCachedConnect = nullptr;
    }
    m_cachedConnectHost.clear();
    m_cachedConnectPort = 0;
}

HttpResponse WinHttpTransport::ExecuteRequest(
    const wchar_t* verb,
    std::string_view url,
    std::string_view postData,
    std::string_view contentType,
    const std::vector<std::string>& extraHeaders,
    uint32_t timeoutMs) {

    HttpResponse resp;
    if (!m_hSession) {
        resp.errorMessage = std::string(TR(MsgKey::HttpErrSessionNotReady));
        return resp;
    }

    ParsedUrl parsed;
    if (!ParseUrl(url, parsed)) {
        resp.errorMessage = std::string(TR(MsgKey::HttpErrInvalidUrl));
        return resp;
    }

    void* hConnect = GetOrCreateConnection(parsed.host, parsed.port);
    if (!hConnect) {
        resp.errorMessage = TR_FMT(MsgKey::HttpErrConnectHostFailed, WideToUtf8(parsed.host), GetLastError());
        LOG_WARN("WinHttp", "{}", resp.errorMessage);
        return resp;
    }

    const bool isPost = (std::wcscmp(verb, L"POST") == 0);
    const DWORD flags = parsed.isHttps ? WINHTTP_FLAG_SECURE : 0;
    DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                     SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                     SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                     SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;

    const DWORD postLen = isPost ? static_cast<DWORD>(postData.size()) : 0;
    void* pData = (isPost && postLen > 0) ? const_cast<char*>(postData.data()) : WINHTTP_NO_REQUEST_DATA;

    auto openAndSend = [&](void* conn) -> ScopedHInternet {
        ScopedHInternet req{WinHttpOpenRequest(
            conn,
            verb,
            parsed.path.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            flags)};
        if (!req) return req;

        if (timeoutMs > 0) {
            const int t = static_cast<int>(timeoutMs);
            WinHttpSetTimeouts(req, t, t, t, t);
        }

        if (parsed.isHttps) {
            WinHttpSetOption(req, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));
        }

        if (isPost && !contentType.empty()) {
            std::string ct = "Content-Type: " + std::string{contentType};
            std::wstring wCt = Utf8ToWide(ct);
            WinHttpAddRequestHeaders(req, wCt.c_str(), static_cast<DWORD>(wCt.size()), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        }

        for (const auto& hdr : extraHeaders) {
            std::wstring wHdr = Utf8ToWide(hdr);
            WinHttpAddRequestHeaders(req, wHdr.c_str(), static_cast<DWORD>(wHdr.size()), WINHTTP_ADDREQ_FLAG_ADD);
        }

        if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, pData, postLen, postLen, 0)) {
            req.Reset();
        }
        return req;
    };

    const auto startT = std::chrono::steady_clock::now();
    ScopedHInternet hRequest = openAndSend(hConnect);

    // If request failed on a cached connection (e.g. server closed idle keep-alive socket), retry once on fresh connection
    if (!hRequest) {
        InvalidateConnection();
        hConnect = GetOrCreateConnection(parsed.host, parsed.port);
        if (hConnect) {
            hRequest = openAndSend(hConnect);
        }
    }

    if (!hRequest) {
        resp.errorMessage = TR_FMT(isPost ? MsgKey::HttpErrSendPostFailed : MsgKey::HttpErrSendGetFailed, GetLastError());
        LOG_WARN("WinHttp", "{}", resp.errorMessage);
        return resp;
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        resp.errorMessage = TR_FMT(MsgKey::HttpErrRecvHeadersFailed, GetLastError());
        LOG_WARN("WinHttp", "{}", resp.errorMessage);
        return resp;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(
        hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &statusCode,
        &statusSize,
        WINHTTP_NO_HEADER_INDEX);
    resp.statusCode = static_cast<int>(statusCode);

    // Fast memory pre-allocation if server returns Content-Length
    DWORD contentLength = 0;
    DWORD clSize = sizeof(contentLength);
    if (WinHttpQueryHeaders(
            hRequest,
            WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &contentLength,
            &clSize,
            WINHTTP_NO_HEADER_INDEX) && contentLength > 0) {
        resp.body.reserve(contentLength);
    }

    std::array<char, 32768> buffer{};
    while (true) {
        DWORD bytesRead = 0;
        if (!WinHttpReadData(hRequest, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead) || bytesRead == 0) {
            break;
        }
        resp.body.append(buffer.data(), bytesRead);
    }

    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startT).count();
    LOG_DEBUG("WinHttp", isPost ? MsgKey::LogHttpPostDone : MsgKey::LogHttpGetDone, resp.statusCode, resp.body.size(), elapsedMs);

    return resp;
}

HttpResponse WinHttpTransport::Get(
    std::string_view url,
    const std::vector<std::string>& extraHeaders,
    uint32_t timeoutMs) {
    return ExecuteRequest(L"GET", url, {}, {}, extraHeaders, timeoutMs);
}

HttpResponse WinHttpTransport::Post(
    std::string_view url,
    std::string_view postData,
    std::string_view contentType,
    const std::vector<std::string>& extraHeaders,
    uint32_t timeoutMs) {
    return ExecuteRequest(L"POST", url, postData, contentType, extraHeaders, timeoutMs);
}

// ============================================================================
// WebSocketClient Implementation
// ============================================================================

WebSocketClient::WebSocketClient() {
    m_hSession = WinHttpOpen(
        L"OpenSteamTool-WebSocket/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);

    if (!m_hSession) {
        m_hSession = WinHttpOpen(
            L"OpenSteamTool-WebSocket/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);
    }

    if (m_hSession) {
        DWORD secureProtocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        WinHttpSetOption(m_hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &secureProtocols, sizeof(secureProtocols));
        WinHttpSetTimeouts(m_hSession, 5000, 5000, 5000, 5000);
    }
}

WebSocketClient::~WebSocketClient() {
    Close();
    if (m_hSession) {
        WinHttpCloseHandle(m_hSession);
        m_hSession = nullptr;
    }
}

void WebSocketClient::Close() {
    if (m_hWebSocket) {
        WinHttpWebSocketClose(m_hWebSocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        WinHttpCloseHandle(m_hWebSocket);
        m_hWebSocket = nullptr;
    }
    if (m_hRequest) {
        WinHttpCloseHandle(m_hRequest);
        m_hRequest = nullptr;
    }
    if (m_hConnect) {
        WinHttpCloseHandle(m_hConnect);
        m_hConnect = nullptr;
    }
}

bool WebSocketClient::Connect(std::string_view wssUrl, uint32_t timeoutMs) {
    Close();
    if (!m_hSession) {
        LOG_ERROR("WebSocket", MsgKey::LogWsSessionInvalid);
        return false;
    }

    ParsedUrl parsed;
    if (!ParseUrl(wssUrl, parsed)) {
        LOG_ERROR("WebSocket", MsgKey::LogWsInvalidUrl, wssUrl);
        return false;
    }

    LOG_DEBUG("WebSocket", MsgKey::LogWsConnecting,
              WideToUtf8(parsed.host), parsed.port, WideToUtf8(parsed.path), parsed.isHttps);

    m_hConnect = WinHttpConnect(m_hSession, parsed.host.c_str(), parsed.port, 0);
    if (!m_hConnect) {
        LOG_ERROR("WebSocket", MsgKey::LogWsConnectHostFailed, WideToUtf8(parsed.host), GetLastError());
        return false;
    }

    DWORD flags = parsed.isHttps ? WINHTTP_FLAG_SECURE : 0;
    m_hRequest = WinHttpOpenRequest(
        m_hConnect,
        L"GET",
        parsed.path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags);

    if (!m_hRequest) {
        LOG_ERROR("WebSocket", MsgKey::LogWsOpenReqFailed, GetLastError());
        Close();
        return false;
    }

    // Request WebSocket Upgrade
    if (!WinHttpSetOption(m_hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
        LOG_ERROR("WebSocket", MsgKey::LogWsUpgradeOptFailed, GetLastError());
        Close();
        return false;
    }

    if (parsed.isHttps) {
        DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                         SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                         SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                         SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(m_hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));
    }

    const int t = static_cast<int>(timeoutMs);
    WinHttpSetTimeouts(m_hRequest, t, t, t, t);

    auto startT = std::chrono::steady_clock::now();
    if (!WinHttpSendRequest(m_hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0)) {
        LOG_ERROR("WebSocket", MsgKey::LogWsSendHandshakeFailed, GetLastError());
        Close();
        return false;
    }

    if (!WinHttpReceiveResponse(m_hRequest, nullptr)) {
        LOG_ERROR("WebSocket", MsgKey::LogWsRecvHandshakeFailed, GetLastError());
        Close();
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(
        m_hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &statusCode,
        &statusSize,
        WINHTTP_NO_HEADER_INDEX);

    if (statusCode != 101) {
        LOG_ERROR("WebSocket", MsgKey::LogWsHandshakeRejected, statusCode);
        Close();
        return false;
    }

    m_hWebSocket = WinHttpWebSocketCompleteUpgrade(m_hRequest, 0);
    WinHttpCloseHandle(m_hRequest);
    m_hRequest = nullptr;

    if (!m_hWebSocket) {
        LOG_ERROR("WebSocket", MsgKey::LogWsCompleteUpgradeFailed, GetLastError());
        Close();
        return false;
    }

    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startT).count();
    LOG_DEBUG("WebSocket", MsgKey::LogWsConnected, wssUrl, elapsedMs);
    return true;
}

bool WebSocketClient::Send(std::span<const uint8_t> data, bool isBinary) {
    if (!m_hWebSocket) return false;

    WINHTTP_WEB_SOCKET_BUFFER_TYPE bufType = isBinary ?
        WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE :
        WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;

    DWORD error = WinHttpWebSocketSend(
        m_hWebSocket,
        bufType,
        const_cast<void*>(reinterpret_cast<const void*>(data.data())),
        static_cast<DWORD>(data.size()));

    if (error != ERROR_SUCCESS) {
        LOG_ERROR("WebSocket", MsgKey::LogWsSendFailed, error);
        Close();
        return false;
    }
    return true;
}

bool WebSocketClient::Receive(std::vector<uint8_t>& outData, bool& isBinary, uint32_t timeoutMs) {
    if (!m_hWebSocket) return false;

    if (timeoutMs > 0) {
        DWORD t = timeoutMs;
        WinHttpSetOption(m_hWebSocket, WINHTTP_OPTION_RECEIVE_TIMEOUT, &t, sizeof(t));
    }

    outData.clear();
    if (m_recvChunk.size() < 65536) {
        m_recvChunk.resize(65536);
    }

    while (true) {
        DWORD bytesRead = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE bufType;

        DWORD error = WinHttpWebSocketReceive(
            m_hWebSocket,
            m_recvChunk.data(),
            static_cast<DWORD>(m_recvChunk.size()),
            &bytesRead,
            &bufType);

        if (error != ERROR_SUCCESS) {
            LOG_ERROR("WebSocket", MsgKey::LogWsRecvError, error);
            if (error != ERROR_WINHTTP_TIMEOUT) {
                Close();
            }
            return false;
        }

        if (bufType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            LOG_WARN("WebSocket", MsgKey::LogWsPeerClosed);
            Close();
            return false;
        }

        if (bytesRead > 0) {
            outData.insert(outData.end(), m_recvChunk.begin(), m_recvChunk.begin() + bytesRead);
        }

        if (bufType == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
            isBinary = true;
            break;
        } else if (bufType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
            isBinary = false;
            break;
        }
        // If buffer type is fragment (..._FRAGMENT_BUFFER_TYPE), continue reading
    }

    return true;
}

} // namespace OST::ExtractTickets
