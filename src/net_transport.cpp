#include "net_transport.h"

#include <stdexcept>
#include <string>
#include <vector>
#include <cstring>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <winhttp.h>
#else
#  include <cerrno>
#  include <curl/curl.h>
#  include <mutex>
#  include <random>
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  include <openssl/bio.h>
#  include <openssl/err.h>
#  include <openssl/evp.h>
#  include <openssl/sha.h>
#  include <openssl/ssl.h>
#endif

namespace
{
#ifdef _WIN32
    std::wstring utf8_to_wide(const std::string& s)
    {
        if (s.empty())
            return {};
        const int size = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring out(size, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), size);
        return out;
    }

    std::string winerr(const char* what)
    {
        return std::string(what) + " (GetLastError=" + std::to_string(GetLastError()) + ")";
    }
#else
    std::once_flag g_curl_once;
    std::once_flag g_ssl_once;

    void ensure_curl()
    {
        std::call_once(g_curl_once, [] {
            if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0)
                throw std::runtime_error("curl_global_init failed");
        });
    }

    void ensure_ssl()
    {
        std::call_once(g_ssl_once, [] {
            OPENSSL_init_ssl(0, nullptr);
        });
    }

    size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata)
    {
        auto* out = static_cast<std::string*>(userdata);
        out->append(ptr, size * nmemb);
        return size * nmemb;
    }

    std::string b64_encode(const unsigned char* data, size_t len)
    {
        static constexpr char kTable[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve((len + 2) / 3 * 4);
        for (size_t i = 0; i < len; i += 3)
        {
            const unsigned int n = (static_cast<unsigned int>(data[i]) << 16)
                | ((i + 1 < len ? data[i + 1] : 0u) << 8)
                | (i + 2 < len ? data[i + 2] : 0u);
            out.push_back(kTable[(n >> 18) & 63]);
            out.push_back(kTable[(n >> 12) & 63]);
            out.push_back(i + 1 < len ? kTable[(n >> 6) & 63] : '=');
            out.push_back(i + 2 < len ? kTable[n & 63] : '=');
        }
        return out;
    }

    bool ssl_write_all(SSL* ssl, const void* data, size_t len)
    {
        const char* p = static_cast<const char*>(data);
        size_t off = 0;
        while (off < len)
        {
            const int n = SSL_write(ssl, p + off, static_cast<int>(len - off));
            if (n <= 0)
                return false;
            off += static_cast<size_t>(n);
        }
        return true;
    }

    int tcp_connect(const std::string& host, int port)
    {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        const std::string port_s = std::to_string(port);
        if (getaddrinfo(host.c_str(), port_s.c_str(), &hints, &res) != 0 || !res)
            throw std::runtime_error("getaddrinfo failed for " + host);

        int fd = -1;
        for (addrinfo* ai = res; ai; ai = ai->ai_next)
        {
            fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd < 0)
                continue;
            const int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            timeval tv{};
            tv.tv_sec = 10;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
                break;
            ::close(fd);
            fd = -1;
        }
        freeaddrinfo(res);
        if (fd < 0)
            throw std::runtime_error("TCP connect failed to " + host);
        // User Data 可能长时间无推送；5s 会被误判断线。读超时放宽到 60s，由调用方区分 idle/close
        timeval rtv{};
        rtv.tv_sec = 60;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));
        return fd;
    }
#endif
}

HttpsResult https_call(const std::string& host,
                       const std::string& method,
                       const std::string& path,
                       const std::string& extra_headers)
{
#ifdef _WIN32
    const std::wstring whost = utf8_to_wide(host);
    const std::wstring wpath = utf8_to_wide(path);
    const std::wstring wmethod = utf8_to_wide(method);
    const std::wstring wheaders = utf8_to_wide(extra_headers);

    HINTERNET session = WinHttpOpen(
        L"Progressiv/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!session)
        throw std::runtime_error(winerr("WinHttpOpen failed"));

    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    WinHttpSetTimeouts(session, 10000, 10000, 15000, 15000);

    HINTERNET connect = WinHttpConnect(session, whost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect)
    {
        WinHttpCloseHandle(session);
        throw std::runtime_error(winerr("WinHttpConnect failed"));
    }

    HINTERNET request = WinHttpOpenRequest(
        connect,
        wmethod.c_str(),
        wpath.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!request)
    {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        throw std::runtime_error(winerr("WinHttpOpenRequest failed"));
    }

    LPCWSTR headers = wheaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wheaders.c_str();
    const DWORD header_len = wheaders.empty() ? 0 : static_cast<DWORD>(-1);
    if (!WinHttpSendRequest(request, headers, header_len, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        || !WinHttpReceiveResponse(request, nullptr))
    {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        throw std::runtime_error(winerr("HTTPS request failed"));
    }

    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    WinHttpQueryHeaders(
        request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &status_code,
        &status_size,
        WINHTTP_NO_HEADER_INDEX);

    std::string body;
    for (;;)
    {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0)
            break;
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read))
            break;
        chunk.resize(read);
        body += chunk;
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    HttpsResult result;
    result.status = static_cast<long>(status_code);
    result.body = std::move(body);
    return result;
#else
    ensure_curl();
    CURL* curl = curl_easy_init();
    if (!curl)
        throw std::runtime_error("curl_easy_init failed");

    const std::string url = "https://" + host + path;
    std::string body;
    long status = 0;
    struct curl_slist* hdrs = nullptr;
    if (!extra_headers.empty())
    {
        std::string h = extra_headers;
        while (!h.empty() && (h.back() == '\n' || h.back() == '\r'))
            h.pop_back();
        size_t start = 0;
        while (start < h.size())
        {
            auto end = h.find("\r\n", start);
            if (end == std::string::npos)
                end = h.size();
            const std::string line = h.substr(start, end - start);
            if (!line.empty())
                hdrs = curl_slist_append(hdrs, line.c_str());
            start = (end == h.size()) ? end : end + 2;
        }
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 15000L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Progressiv/1.0");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    const CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    if (hdrs)
        curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK)
        throw std::runtime_error(std::string("HTTPS request failed: ") + curl_easy_strerror(rc));

    HttpsResult result;
    result.status = status;
    result.body = std::move(body);
    return result;
#endif
}

std::string https_get(const std::string& host, const std::string& path)
{
    const auto result = https_call(host, "GET", path);
    if (result.status != 200)
        throw std::runtime_error("HTTPS GET status " + std::to_string(result.status) + ": " + result.body);
    return result.body;
}

HttpsResult https_api_key(const std::string& host,
                          const std::string& method,
                          const std::string& path,
                          const std::string& api_key)
{
    return https_call(host, method, path, "X-MBX-APIKEY: " + api_key + "\r\n");
}

struct WsConnection::Impl
{
#ifdef _WIN32
    HINTERNET session = nullptr;
    HINTERNET connect = nullptr;
    HINTERNET socket = nullptr;
#else
    int fd = -1;
    SSL_CTX* ctx = nullptr;
    SSL* ssl = nullptr;
    std::string rx;
#endif
};

WsConnection::WsConnection(WsConnection&& other) noexcept
    : impl_(other.impl_)
{
    other.impl_ = nullptr;
}

WsConnection& WsConnection::operator=(WsConnection&& other) noexcept
{
    if (this != &other)
    {
        close();
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

WsConnection::~WsConnection()
{
    close();
}

void WsConnection::close()
{
    if (!impl_)
        return;
#ifdef _WIN32
    if (impl_->socket)
    {
        WinHttpWebSocketClose(impl_->socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        WinHttpCloseHandle(impl_->socket);
        impl_->socket = nullptr;
    }
    if (impl_->connect)
    {
        WinHttpCloseHandle(impl_->connect);
        impl_->connect = nullptr;
    }
    if (impl_->session)
    {
        WinHttpCloseHandle(impl_->session);
        impl_->session = nullptr;
    }
#else
    if (impl_->ssl)
    {
        SSL_shutdown(impl_->ssl);
        SSL_free(impl_->ssl);
        impl_->ssl = nullptr;
    }
    if (impl_->ctx)
    {
        SSL_CTX_free(impl_->ctx);
        impl_->ctx = nullptr;
    }
    if (impl_->fd >= 0)
    {
        ::close(impl_->fd);
        impl_->fd = -1;
    }
#endif
    delete impl_;
    impl_ = nullptr;
}

WsConnection ws_connect(const std::string& host, const std::string& path)
{
    WsConnection ws;
    ws.impl_ = new WsConnection::Impl();

#ifdef _WIN32
    const std::wstring whost = utf8_to_wide(host);
    const std::wstring wpath = utf8_to_wide(path);

    ws.impl_->session = WinHttpOpen(
        L"Progressiv/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!ws.impl_->session)
    {
        ws.close();
        throw std::runtime_error(winerr("WinHttpOpen failed"));
    }

    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    WinHttpSetOption(ws.impl_->session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    WinHttpSetTimeouts(ws.impl_->session, 10000, 10000, 15000, 5000);

    ws.impl_->connect = WinHttpConnect(ws.impl_->session, whost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!ws.impl_->connect)
    {
        ws.close();
        throw std::runtime_error(winerr("WinHttpConnect failed"));
    }

    HINTERNET request = WinHttpOpenRequest(
        ws.impl_->connect,
        L"GET",
        wpath.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!request)
    {
        ws.close();
        throw std::runtime_error(winerr("WinHttpOpenRequest failed"));
    }

    if (!WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)
        || !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        || !WinHttpReceiveResponse(request, nullptr))
    {
        WinHttpCloseHandle(request);
        ws.close();
        throw std::runtime_error(winerr("WebSocket handshake failed"));
    }

    ws.impl_->socket = WinHttpWebSocketCompleteUpgrade(request, 0);
    WinHttpCloseHandle(request);
    if (!ws.impl_->socket)
    {
        ws.close();
        throw std::runtime_error(winerr("WinHttpWebSocketCompleteUpgrade failed"));
    }
#else
    // Ubuntu 自带 libcurl 未启用 ws/wss，这里用 OpenSSL 手写 WSS
    ensure_ssl();
    try
    {
        ws.impl_->fd = tcp_connect(host, 443);
        ws.impl_->ctx = SSL_CTX_new(TLS_client_method());
        if (!ws.impl_->ctx)
            throw std::runtime_error("SSL_CTX_new failed");
        SSL_CTX_set_default_verify_paths(ws.impl_->ctx);
        SSL_CTX_set_verify(ws.impl_->ctx, SSL_VERIFY_PEER, nullptr);

        ws.impl_->ssl = SSL_new(ws.impl_->ctx);
        if (!ws.impl_->ssl)
            throw std::runtime_error("SSL_new failed");
        SSL_set_tlsext_host_name(ws.impl_->ssl, host.c_str());
        SSL_set1_host(ws.impl_->ssl, host.c_str());
        SSL_set_fd(ws.impl_->ssl, ws.impl_->fd);
        if (SSL_connect(ws.impl_->ssl) != 1)
            throw std::runtime_error("SSL_connect failed");

        unsigned char key_raw[16];
        std::random_device rd;
        for (unsigned char& b : key_raw)
            b = static_cast<unsigned char>(rd());
        const std::string key_b64 = b64_encode(key_raw, sizeof(key_raw));

        std::string req;
        req += "GET " + path + " HTTP/1.1\r\n";
        req += "Host: " + host + "\r\n";
        req += "Upgrade: websocket\r\n";
        req += "Connection: Upgrade\r\n";
        req += "Sec-WebSocket-Key: " + key_b64 + "\r\n";
        req += "Sec-WebSocket-Version: 13\r\n";
        req += "User-Agent: Progressiv/1.0\r\n";
        req += "\r\n";
        if (!ssl_write_all(ws.impl_->ssl, req.data(), req.size()))
            throw std::runtime_error("WS handshake write failed");

        std::string hdr;
        char tmp[1024];
        while (hdr.find("\r\n\r\n") == std::string::npos)
        {
            const int n = SSL_read(ws.impl_->ssl, tmp, sizeof(tmp));
            if (n <= 0)
                throw std::runtime_error("WS handshake read failed");
            hdr.append(tmp, static_cast<size_t>(n));
            if (hdr.size() > 64 * 1024)
                throw std::runtime_error("WS handshake response too large");
        }
        if (hdr.find(" 101 ") == std::string::npos && hdr.find("101 Switching") == std::string::npos)
            throw std::runtime_error("WS handshake rejected: " + hdr.substr(0, 200));

        const auto sep = hdr.find("\r\n\r\n");
        if (sep != std::string::npos && sep + 4 < hdr.size())
            ws.impl_->rx.assign(hdr.begin() + static_cast<std::ptrdiff_t>(sep + 4), hdr.end());
    }
    catch (...)
    {
        ws.close();
        throw;
    }
#endif
    return ws;
}

void ws_send(WsConnection& ws, const std::string& msg)
{
    if (!ws.impl_)
        throw std::runtime_error("ws_send on closed socket");
#ifdef _WIN32
    const DWORD st = WinHttpWebSocketSend(
        ws.impl_->socket,
        WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        reinterpret_cast<PVOID>(const_cast<char*>(msg.data())),
        static_cast<DWORD>(msg.size()));
    if (st != ERROR_SUCCESS)
        throw std::runtime_error("WinHttpWebSocketSend failed (" + std::to_string(st) + ")");
#else
    std::vector<unsigned char> frame;
    frame.push_back(0x81); // FIN + text
    const size_t n = msg.size();
    if (n < 126)
    {
        frame.push_back(static_cast<unsigned char>(0x80 | n));
    }
    else if (n <= 0xFFFF)
    {
        frame.push_back(0x80 | 126);
        frame.push_back(static_cast<unsigned char>((n >> 8) & 0xFF));
        frame.push_back(static_cast<unsigned char>(n & 0xFF));
    }
    else
    {
        frame.push_back(0x80 | 127);
        for (int i = 7; i >= 0; --i)
            frame.push_back(static_cast<unsigned char>((n >> (8 * i)) & 0xFF));
    }

    unsigned char mask[4];
    std::random_device rd;
    for (unsigned char& b : mask)
        b = static_cast<unsigned char>(rd());
    frame.insert(frame.end(), mask, mask + 4);
    for (size_t i = 0; i < n; ++i)
        frame.push_back(static_cast<unsigned char>(msg[i]) ^ mask[i % 4]);

    if (!ssl_write_all(ws.impl_->ssl, frame.data(), frame.size()))
        throw std::runtime_error("ws_send failed");
#endif
}

std::string ws_recv(WsConnection& ws, bool* timed_out)
{
    if (timed_out)
        *timed_out = false;
    if (!ws.impl_)
        return {};
#ifdef _WIN32
    std::string response;
    std::vector<BYTE> buffer(16 * 1024);
    for (;;)
    {
        DWORD bytes_read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE buffer_type = WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
        const DWORD st = WinHttpWebSocketReceive(
            ws.impl_->socket,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &bytes_read,
            &buffer_type);
        if (st == ERROR_WINHTTP_TIMEOUT)
        {
            if (timed_out)
                *timed_out = true;
            return {};
        }
        if (st != ERROR_SUCCESS)
            throw std::runtime_error("WinHttpWebSocketReceive failed (" + std::to_string(st) + ")");

        if (buffer_type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
            break;

        response.append(reinterpret_cast<char*>(buffer.data()), bytes_read);
        if (buffer_type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE
            || buffer_type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE)
        {
            break;
        }
    }
    return response;
#else
    auto& rx = ws.impl_->rx;
    bool idle = false;
    auto fill = [&]() -> bool {
        char buf[16 * 1024];
        const int n = SSL_read(ws.impl_->ssl, buf, sizeof(buf));
        if (n > 0)
        {
            rx.append(buf, static_cast<size_t>(n));
            return true;
        }
        const int err = SSL_get_error(ws.impl_->ssl, n);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE
            || errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT)
        {
            idle = true;
            return false;
        }
        return false;
    };

    std::string message;
    for (;;)
    {
        while (rx.size() < 2)
        {
            if (!fill())
            {
                if (timed_out)
                    *timed_out = idle;
                return {};
            }
        }

        const auto b0 = static_cast<unsigned char>(rx[0]);
        const auto b1 = static_cast<unsigned char>(rx[1]);
        const bool fin = (b0 & 0x80) != 0;
        const int opcode = b0 & 0x0F;
        const bool masked = (b1 & 0x80) != 0;
        uint64_t plen = b1 & 0x7F;
        size_t hdr = 2;
        if (plen == 126)
        {
            while (rx.size() < 4)
            {
                if (!fill())
                {
                    if (timed_out)
                        *timed_out = idle;
                    return {};
                }
            }
            plen = (static_cast<unsigned char>(rx[2]) << 8) | static_cast<unsigned char>(rx[3]);
            hdr = 4;
        }
        else if (plen == 127)
        {
            while (rx.size() < 10)
            {
                if (!fill())
                {
                    if (timed_out)
                        *timed_out = idle;
                    return {};
                }
            }
            plen = 0;
            for (int i = 0; i < 8; ++i)
                plen = (plen << 8) | static_cast<unsigned char>(rx[2 + i]);
            hdr = 10;
        }

        const size_t mask_len = masked ? 4u : 0u;
        const size_t need = hdr + mask_len + static_cast<size_t>(plen);
        while (rx.size() < need)
        {
            if (!fill())
            {
                if (timed_out)
                    *timed_out = idle;
                return {};
            }
        }

        unsigned char mkey[4]{};
        if (masked)
            std::memcpy(mkey, rx.data() + hdr, 4);
        std::string payload(rx.begin() + static_cast<std::ptrdiff_t>(hdr + mask_len),
                            rx.begin() + static_cast<std::ptrdiff_t>(need));
        if (masked)
        {
            for (size_t i = 0; i < payload.size(); ++i)
                payload[i] = static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mkey[i % 4]);
        }
        rx.erase(0, need);

        if (opcode == 0x8) // close
            return {};
        if (opcode == 0x9) // ping -> pong
        {
            std::vector<unsigned char> pong;
            pong.push_back(0x8A);
            if (payload.size() < 126)
            {
                pong.push_back(static_cast<unsigned char>(0x80 | payload.size()));
            }
            else
            {
                pong.push_back(0x80 | 126);
                pong.push_back(static_cast<unsigned char>((payload.size() >> 8) & 0xFF));
                pong.push_back(static_cast<unsigned char>(payload.size() & 0xFF));
            }
            unsigned char mask[4];
            std::random_device rd;
            for (unsigned char& b : mask)
                b = static_cast<unsigned char>(rd());
            pong.insert(pong.end(), mask, mask + 4);
            for (size_t i = 0; i < payload.size(); ++i)
                pong.push_back(static_cast<unsigned char>(payload[i]) ^ mask[i % 4]);
            ssl_write_all(ws.impl_->ssl, pong.data(), pong.size());
            continue;
        }
        if (opcode == 0xA) // pong
            continue;

        if (opcode == 0x1 || opcode == 0x2 || opcode == 0x0)
        {
            message += payload;
            if (fin)
                return message;
            continue;
        }
    }
#endif
}
