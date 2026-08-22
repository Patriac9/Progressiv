#ifndef PROGRESSIV_NET_TRANSPORT_H
#define PROGRESSIV_NET_TRANSPORT_H

#include <string>

struct HttpsResult
{
    long status = 0;
    std::string body;
};

// 同步 HTTPS；path 可含 query；extra_headers 如 "X-MBX-APIKEY: xxx\r\n"
HttpsResult https_call(const std::string& host,
                       const std::string& method,
                       const std::string& path,
                       const std::string& extra_headers = {});

std::string https_get(const std::string& host, const std::string& path);

HttpsResult https_api_key(const std::string& host,
                          const std::string& method,
                          const std::string& path,
                          const std::string& api_key);

// WebSocket（WSS）长连接：每线程独立一个
struct WsConnection
{
    WsConnection() = default;
    WsConnection(const WsConnection&) = delete;
    WsConnection& operator=(const WsConnection&) = delete;
    WsConnection(WsConnection&& other) noexcept;
    WsConnection& operator=(WsConnection&& other) noexcept;
    ~WsConnection();

    void close();
    explicit operator bool() const { return impl_ != nullptr; }

private:
    friend WsConnection ws_connect(const std::string& host, const std::string& path);
    friend void ws_send(WsConnection& ws, const std::string& msg);
    friend std::string ws_recv(WsConnection& ws, bool* timed_out);

    struct Impl;
    Impl* impl_ = nullptr;
};

WsConnection ws_connect(const std::string& host, const std::string& path);
void ws_send(WsConnection& ws, const std::string& msg);
// 对端关闭返回空串；读超时也返回空串，若 timed_out!=nullptr 则 *timed_out=true
std::string ws_recv(WsConnection& ws, bool* timed_out = nullptr);

#endif
