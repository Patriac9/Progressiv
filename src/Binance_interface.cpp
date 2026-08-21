//
// Created by zagym on 09/08/2026.
//

#include "Binance_interface.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "loader.h"
#include "ed25519_sign.h"

namespace
{
    std::wstring utf8_to_wide(const std::string& s)
    {
        if (s.empty())
            return {};
        const int size = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring out(size, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), size);
        return out;
    }

    int b64_value(char c)
    {
        if (c >= 'A' && c <= 'Z')
            return c - 'A';
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 26;
        if (c >= '0' && c <= '9')
            return c - '0' + 52;
        if (c == '+')
            return 62;
        if (c == '/')
            return 63;
        return -1;
    }

    std::vector<unsigned char> base64_decode(const std::string& input)
    {
        std::vector<unsigned char> out;
        out.reserve(input.size() * 3 / 4);
        int val = 0;
        int valb = -8;
        for (unsigned char c : input)
        {
            if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t')
                continue;
            const int d = b64_value(static_cast<char>(c));
            if (d < 0)
                continue;
            val = (val << 6) + d;
            valb += 6;
            if (valb >= 0)
            {
                out.push_back(static_cast<unsigned char>((val >> valb) & 0xFF));
                valb -= 8;
            }
        }
        return out;
    }

    std::string base64_encode(const unsigned char* data, size_t len)
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

    std::vector<unsigned char> pem_to_der(const std::string& pem)
    {
        const auto begin = pem.find("-----BEGIN");
        const auto end = pem.find("-----END");
        if (begin == std::string::npos || end == std::string::npos || end <= begin)
            throw std::runtime_error("Invalid Ed25519 PEM private key");

        size_t i = begin;
        while (i < end && pem[i] != '\n' && pem[i] != '\r')
            ++i;
        if (i >= end)
            throw std::runtime_error("Invalid Ed25519 PEM private key body");
        while (i < end && (pem[i] == '\n' || pem[i] == '\r'))
            ++i;
        return base64_decode(pem.substr(i, end - i));
    }

    std::string json_escape(const std::string& s)
    {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s)
        {
            switch (c)
            {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
            }
        }
        return out;
    }

    std::string extract_json_string(const std::string& obj, const std::string& key)
    {
        const std::string pattern = "\"" + key + "\"";
        const auto key_pos = obj.find(pattern);
        if (key_pos == std::string::npos)
            return {};

        const auto colon = obj.find(':', key_pos + pattern.size());
        if (colon == std::string::npos)
            return {};

        size_t i = colon + 1;
        while (i < obj.size() && (obj[i] == ' ' || obj[i] == '\t'))
            ++i;
        if (i >= obj.size() || obj[i] != '"')
            return {};

        std::string value;
        for (size_t j = i + 1; j < obj.size(); ++j)
        {
            if (obj[j] == '\\' && j + 1 < obj.size())
            {
                value.push_back(obj[j + 1]);
                ++j;
                continue;
            }
            if (obj[j] == '"')
                break;
            value.push_back(obj[j]);
        }
        return value;
    }

    // 字符串或裸数字/布尔，统一成 string（合约字段常为 DECIMAL 字符串，偶发裸数字）
    std::string extract_json_scalar(const std::string& obj, const std::string& key)
    {
        const std::string pattern = "\"" + key + "\"";
        const auto key_pos = obj.find(pattern);
        if (key_pos == std::string::npos)
            return {};

        const auto colon = obj.find(':', key_pos + pattern.size());
        if (colon == std::string::npos)
            return {};

        size_t i = colon + 1;
        while (i < obj.size() && (obj[i] == ' ' || obj[i] == '\t'))
            ++i;
        if (i >= obj.size())
            return {};

        if (obj[i] == '"')
        {
            std::string value;
            for (size_t j = i + 1; j < obj.size(); ++j)
            {
                if (obj[j] == '\\' && j + 1 < obj.size())
                {
                    value.push_back(obj[j + 1]);
                    ++j;
                    continue;
                }
                if (obj[j] == '"')
                    break;
                value.push_back(obj[j]);
            }
            return value;
        }

        size_t end = i;
        while (end < obj.size() && obj[end] != ',' && obj[end] != '}' && obj[end] != ']'
               && obj[end] != ' ' && obj[end] != '\n' && obj[end] != '\r' && obj[end] != '\t')
            ++end;
        return obj.substr(i, end - i);
    }

    float extract_json_float(const std::string& obj, const std::string& key)
    {
        const std::string s = extract_json_scalar(obj, key);
        if (s.empty() || s == "null")
            return 0.f;
        try
        {
            return std::stof(s);
        }
        catch (const std::exception&)
        {
            return 0.f;
        }
    }

    std::vector<std::string> extract_json_object_array(const std::string& json, const std::string& array_key)
    {
        std::vector<std::string> objs;
        const std::string pattern = "\"" + array_key + "\"";
        const auto key_pos = json.find(pattern);
        if (key_pos == std::string::npos)
            return objs;

        const auto arr_begin = json.find('[', key_pos + pattern.size());
        if (arr_begin == std::string::npos)
            return objs;

        size_t i = arr_begin + 1;
        while (i < json.size())
        {
            while (i < json.size()
                   && (json[i] == ' ' || json[i] == '\n' || json[i] == '\r' || json[i] == '\t' || json[i] == ','))
                ++i;
            if (i >= json.size() || json[i] == ']')
                break;
            if (json[i] != '{')
                break;

            int depth = 0;
            size_t j = i;
            for (; j < json.size(); ++j)
            {
                if (json[j] == '{')
                    ++depth;
                else if (json[j] == '}')
                {
                    --depth;
                    if (depth == 0)
                    {
                        ++j;
                        break;
                    }
                }
            }
            objs.push_back(json.substr(i, j - i));
            i = j;
        }
        return objs;
    }

    bool extract_json_bool(const std::string& obj, const std::string& key, bool default_value = false)
    {
        const std::string pattern = "\"" + key + "\"";
        const auto key_pos = obj.find(pattern);
        if (key_pos == std::string::npos)
            return default_value;

        const auto colon = obj.find(':', key_pos + pattern.size());
        if (colon == std::string::npos)
            return default_value;

        size_t i = colon + 1;
        while (i < obj.size() && (obj[i] == ' ' || obj[i] == '\t'))
            ++i;
        if (i + 4 <= obj.size() && obj.compare(i, 4, "true") == 0)
            return true;
        if (i + 5 <= obj.size() && obj.compare(i, 5, "false") == 0)
            return false;
        return default_value;
    }

    bool is_zero_amount(const std::string& value)
    {
        if (value.empty())
            return true;
        for (char c : value)
        {
            if (c != '0' && c != '.' && c != '-')
                return false;
        }
        return true;
    }

    void trim_inplace(std::string& s)
    {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
            s.pop_back();
        size_t start = 0;
        while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\n' || s[start] == '\r'))
            ++start;
        if (start > 0)
            s.erase(0, start);
    }

    long long extract_json_int(const std::string& json, const std::string& key)
    {
        const std::string pattern = "\"" + key + "\"";
        const auto key_pos = json.find(pattern);
        if (key_pos == std::string::npos)
            return 0;

        const auto colon = json.find(':', key_pos + pattern.size());
        if (colon == std::string::npos)
            return 0;

        size_t i = colon + 1;
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t'))
            ++i;

        const auto end = json.find_first_not_of("0123456789-", i);
        if (i >= json.size() || end == i)
            return 0;
        return std::stoll(json.substr(i, end == std::string::npos ? std::string::npos : end - i));
    }

    bool is_json_number_param(const std::string& key)
    {
        return key == "timestamp"
            || key == "orderId"
            || key == "cancelOrderId"
            || key == "limit"
            || key == "recvWindow"
            || key == "strategyId"
            || key == "strategyType"
            || key == "trailingDelta"
            || key == "pegOffsetValue";
    }

    bool is_json_bool_param(const std::string& key)
    {
        return key == "omitZeroBalances"
            || key == "reduceOnly";
    }

    void fill_trade_params(std::map<std::string, std::string>& params, const order_request& req)
    {
        params["side"] = req.side;
        params["type"] = req.type;
        if (!req.time_in_force.empty())
            params["timeInForce"] = req.time_in_force;
        else if (req.type == "LIMIT" || req.type == "STOP_LOSS_LIMIT" || req.type == "TAKE_PROFIT_LIMIT")
            params["timeInForce"] = "GTC";
        if (!req.price.empty())
            params["price"] = req.price;
        if (!req.quantity.empty())
            params["quantity"] = req.quantity;
        if (!req.quote_order_qty.empty())
            params["quoteOrderQty"] = req.quote_order_qty;
        if (!req.client_order_id.empty())
            params["newClientOrderId"] = req.client_order_id;
        if (req.reduce_only)
            params["reduceOnly"] = "true";
    }

    std::vector<orderbook_level> parse_price_levels(const std::string& json, const std::string& key)
    {
        std::vector<orderbook_level> levels;
        // 用 "key": 精确匹配，避免单字母 key（如 b/a）误命中
        const std::string pattern = "\"" + key + "\":";
        const auto key_pos = json.find(pattern);
        if (key_pos == std::string::npos)
            return levels;

        const auto arr_begin = json.find('[', key_pos + pattern.size());
        if (arr_begin == std::string::npos)
            return levels;

        size_t i = arr_begin + 1;
        int depth = 1;
        while (i < json.size() && depth > 0)
        {
            if (json[i] == '[')
            {
                if (depth == 1)
                {
                    const auto level_end = json.find(']', i + 1);
                    if (level_end == std::string::npos)
                        break;

                    const std::string entry = json.substr(i, level_end - i + 1);
                    orderbook_level level;
                    const auto p1 = entry.find('"');
                    const auto p2 = entry.find('"', p1 + 1);
                    const auto p3 = entry.find('"', p2 + 1);
                    const auto p4 = entry.find('"', p3 + 1);
                    if (p1 != std::string::npos && p2 != std::string::npos && p3 != std::string::npos && p4 != std::string::npos)
                    {
                        level.price = std::stof(entry.substr(p1 + 1, p2 - p1 - 1));
                        level.quantity = std::stof(entry.substr(p3 + 1, p4 - p3 - 1));
                        levels.push_back(std::move(level));
                    }
                    i = level_end + 1;
                    continue;
                }
                ++depth;
            }
            else if (json[i] == ']')
            {
                --depth;
            }
            ++i;
        }
        return levels;
    }

    order_info parse_order_fields(const std::string& obj)
    {
        order_info order;
        order.symbol = extract_json_string(obj, "symbol");
        order.order_id = extract_json_int(obj, "orderId");
        order.order_list_id = extract_json_int(obj, "orderListId");
        if (order.order_list_id == 0 && obj.find("\"orderListId\"") == std::string::npos)
            order.order_list_id = -1;
        order.client_order_id = extract_json_string(obj, "clientOrderId");
        if (order.client_order_id.empty())
            order.client_order_id = extract_json_string(obj, "origClientOrderId");
        order.status = extract_json_string(obj, "status");
        order.side = extract_json_string(obj, "side");
        order.type = extract_json_string(obj, "type");
        order.time_in_force = extract_json_string(obj, "timeInForce");
        order.price = extract_json_string(obj, "price");
        order.orig_qty = extract_json_string(obj, "origQty");
        order.executed_qty = extract_json_string(obj, "executedQty");
        order.cummulative_quote_qty = extract_json_string(obj, "cummulativeQuoteQty");
        if (order.cummulative_quote_qty.empty())
            order.cummulative_quote_qty = extract_json_scalar(obj, "cumQuote");
        order.position_side = extract_json_string(obj, "positionSide");
        order.transact_time = extract_json_int(obj, "transactTime");
        if (order.transact_time == 0)
            order.transact_time = extract_json_int(obj, "updateTime");
        order.raw_json = obj;
        return order;
    }

    std::string winerr(const char* what)
    {
        return std::string(what) + " (GetLastError=" + std::to_string(GetLastError()) + ")";
    }

    std::string url_encode(const std::string& s)
    {
        static const char kHex[] = "0123456789ABCDEF";
        std::string out;
        out.reserve(s.size() * 3);
        for (unsigned char c : s)
        {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
                out.push_back(static_cast<char>(c));
            else
            {
                out.push_back('%');
                out.push_back(kHex[c >> 4]);
                out.push_back(kHex[c & 15]);
            }
        }
        return out;
    }

    struct HttpsResult
    {
        DWORD status = 0;
        std::string body;
    };

    HttpsResult https_call(
        const std::string& host,
        const std::wstring& method,
        const std::string& path,
        const std::wstring& extra_headers = {})
    {
        const std::wstring whost = utf8_to_wide(host);
        const std::wstring wpath = utf8_to_wide(path);

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
            method.c_str(),
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

        LPCWSTR headers = extra_headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : extra_headers.c_str();
        const DWORD header_len = extra_headers.empty() ? 0 : static_cast<DWORD>(-1);
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
            if (!WinHttpQueryDataAvailable(request, &available))
                break;
            if (available == 0)
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
        result.status = status_code;
        result.body = std::move(body);
        return result;
    }

    std::string https_get(const std::string& host, const std::string& path)
    {
        const auto result = https_call(host, L"GET", path);
        if (result.status != 200)
            throw std::runtime_error("HTTPS GET status " + std::to_string(result.status) + ": " + result.body);
        return result.body;
    }

    int tick_string_decimals(const std::string& s)
    {
        const auto dot = s.find('.');
        if (dot == std::string::npos)
            return 0;
        size_t end = s.size();
        while (end > dot + 1 && s[end - 1] == '0')
            --end;
        return static_cast<int>(end - dot - 1);
    }

    binance_interface::tick_filter parse_tick_filter(const std::string& json)
    {
        const auto filter_pos = json.find("\"PRICE_FILTER\"");
        if (filter_pos == std::string::npos)
            throw std::runtime_error("PRICE_FILTER not found in exchangeInfo");

        size_t obj_begin = filter_pos;
        while (obj_begin > 0 && json[obj_begin] != '{')
            --obj_begin;

        const auto obj_end = json.find('}', filter_pos);
        if (obj_end == std::string::npos || obj_begin >= obj_end)
            throw std::runtime_error("invalid PRICE_FILTER object in exchangeInfo");

        const std::string filter_obj = json.substr(obj_begin, obj_end - obj_begin + 1);
        const std::string tick_size = extract_json_string(filter_obj, "tickSize");
        if (tick_size.empty())
            throw std::runtime_error("tickSize not found in PRICE_FILTER");
        try
        {
            binance_interface::tick_filter f;
            f.tick_size = std::stod(tick_size);
            f.decimals = tick_string_decimals(tick_size);
            return f;
        }
        catch (const std::exception&)
        {
            throw std::runtime_error("invalid tickSize value: " + tick_size);
        }
    }

    struct WsHandles
    {
        HINTERNET session = nullptr;
        HINTERNET connect = nullptr;
        HINTERNET socket = nullptr;

        void close()
        {
            if (socket)
            {
                WinHttpWebSocketClose(socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
                WinHttpCloseHandle(socket);
                socket = nullptr;
            }
            if (connect)
            {
                WinHttpCloseHandle(connect);
                connect = nullptr;
            }
            if (session)
            {
                WinHttpCloseHandle(session);
                session = nullptr;
            }
        }
    };

    WsHandles ws_connect(const std::string& host, const std::string& path)
    {
        WsHandles h;
        const std::wstring whost = utf8_to_wide(host);
        const std::wstring wpath = utf8_to_wide(path);

        h.session = WinHttpOpen(
            L"Progressiv/1.0",
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);
        if (!h.session)
            throw std::runtime_error(winerr("WinHttpOpen failed"));

        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
        protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
        WinHttpSetOption(h.session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
        // 解析/连接/发送/接收超时；接收超时用于行情流断线检测与重连
        WinHttpSetTimeouts(h.session, 10000, 10000, 15000, 5000);

        h.connect = WinHttpConnect(h.session, whost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!h.connect)
        {
            h.close();
            throw std::runtime_error(winerr("WinHttpConnect failed"));
        }

        HINTERNET request = WinHttpOpenRequest(
            h.connect,
            L"GET",
            wpath.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE);
        if (!request)
        {
            h.close();
            throw std::runtime_error(winerr("WinHttpOpenRequest failed"));
        }

        if (!WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)
            || !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
            || !WinHttpReceiveResponse(request, nullptr))
        {
            WinHttpCloseHandle(request);
            h.close();
            throw std::runtime_error(winerr("WebSocket handshake failed"));
        }

        h.socket = WinHttpWebSocketCompleteUpgrade(request, 0);
        WinHttpCloseHandle(request);
        if (!h.socket)
        {
            h.close();
            throw std::runtime_error(winerr("WinHttpWebSocketCompleteUpgrade failed"));
        }
        return h;
    }

    void ws_send(HINTERNET socket, const std::string& msg)
    {
        const DWORD st = WinHttpWebSocketSend(
            socket,
            WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
            reinterpret_cast<PVOID>(const_cast<char*>(msg.data())),
            static_cast<DWORD>(msg.size()));
        if (st != ERROR_SUCCESS)
            throw std::runtime_error("WinHttpWebSocketSend failed (" + std::to_string(st) + ")");
    }

    std::string ws_recv(HINTERNET socket)
    {
        std::string response;
        std::vector<BYTE> buffer(16 * 1024);
        for (;;)
        {
            DWORD bytes_read = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE buffer_type = WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
            const DWORD st = WinHttpWebSocketReceive(
                socket,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &bytes_read,
                &buffer_type);
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
    }
}

struct binance_interface::RpcChannel
{
    std::string host;
    std::string path = "/ws-api/v3";
    std::atomic<bool> running{false};
    std::thread worker;
    std::mutex mutex;
    std::condition_variable cv;

    struct Job
    {
        std::string id;
        std::string request;
        std::promise<std::string> promise;
    };
    std::queue<Job> jobs;

    void start(std::string h, std::string p = "/ws-api/v3")
    {
        host = std::move(h);
        path = std::move(p);
        if (running.exchange(true))
            return;
        worker = std::thread([this] { run(); });
    }

    void stop()
    {
        if (!running.exchange(false))
            return;
        cv.notify_all();
        if (worker.joinable())
            worker.join();

        std::lock_guard lock(mutex);
        while (!jobs.empty())
        {
            jobs.front().promise.set_exception(
                std::make_exception_ptr(std::runtime_error("RpcChannel stopped")));
            jobs.pop();
        }
    }

    std::string call(std::string id, std::string request)
    {
        if (!running)
            throw std::runtime_error("RpcChannel not started");

        std::promise<std::string> promise;
        auto future = promise.get_future();
        {
            std::lock_guard lock(mutex);
            jobs.push(Job{std::move(id), std::move(request), std::move(promise)});
        }
        cv.notify_one();

        if (future.wait_for(std::chrono::seconds(30)) != std::future_status::ready)
            throw std::runtime_error("WebSocket RPC timeout");
        return future.get();
    }

    void run()
    {
        while (running)
        {
            try
            {
                WsHandles ws = ws_connect(host, path);
                while (running)
                {
                    Job job;
                    {
                        std::unique_lock lock(mutex);
                        cv.wait_for(lock, std::chrono::seconds(1), [this] {
                            return !running || !jobs.empty();
                        });
                        if (!running)
                            break;
                        if (jobs.empty())
                            continue;
                        job = std::move(jobs.front());
                        jobs.pop();
                    }

                    try
                    {
                        ws_send(ws.socket, job.request);
                        std::string response;
                        for (;;)
                        {
                            response = ws_recv(ws.socket);
                            if (response.empty())
                                throw std::runtime_error("Empty RPC response");
                            const bool has_id = response.find("\"id\"") != std::string::npos;
                            const bool id_match = response.find(job.id) != std::string::npos;
                            const bool id_null = response.find("\"id\":null") != std::string::npos;
                            if (!has_id || id_match || id_null)
                                break;
                        }
                        job.promise.set_value(std::move(response));
                    }
                    catch (...)
                    {
                        try
                        {
                            job.promise.set_exception(std::current_exception());
                        }
                        catch (...)
                        {
                        }
                        throw; // 触发外层重连
                    }
                }
                ws.close();
            }
            catch (...)
            {
                if (!running)
                    break;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }
};

struct binance_interface::MarketChannel
{
    std::string host;
    std::string rest_host;
    std::string symbol;
    uint32_t levels = 20;
    std::atomic<bool> running{false};
    std::thread worker;
    std::mutex mutex;
    std::condition_variable cv;
    orderbook_info latest;
    bool has_data = false;
    uint64_t generation_ = 0;
    uint64_t last_consumed_gen_ = 0;

    void start(std::string h, std::string rest_h, std::string sym, uint32_t depth_levels)
    {
        host = std::move(h);
        rest_host = std::move(rest_h);
        symbol = std::move(sym);
        levels = (depth_levels <= 5) ? 5 : (depth_levels <= 10) ? 10 : 20;
        if (running.exchange(true))
            return;
        worker = std::thread([this] { run(); });
    }

    void stop()
    {
        if (!running.exchange(false))
            return;
        cv.notify_all();
        if (worker.joinable())
            worker.join();
    }

    orderbook_info get_latest(std::chrono::milliseconds wait = std::chrono::seconds(30))
    {
        std::unique_lock lock(mutex);
        const bool ok = cv.wait_for(lock, wait, [this] {
            return !running || (has_data && generation_ > last_consumed_gen_);
        });
        if (!ok)
            throw std::runtime_error("MarketChannel: wait orderbook timeout");
        if (!has_data)
            throw std::runtime_error("MarketChannel stopped without data");
        last_consumed_gen_ = generation_;
        return latest;
    }

    orderbook_info get_cached()
    {
        std::lock_guard lock(mutex);
        if (!has_data)
            throw std::runtime_error("MarketChannel: no orderbook yet");
        return latest;
    }

    void publish(orderbook_info book)
    {
        book.message_time = binance_interface::now_ms();
        if (book.transact_time == 0)
            book.transact_time = book.message_time;
        {
            std::lock_guard lock(mutex);
            latest = std::move(book);
            has_data = true;
            ++generation_;
        }
        cv.notify_all();
    }

    void run()
    {
        std::string stream_symbol = symbol;
        std::transform(stream_symbol.begin(), stream_symbol.end(), stream_symbol.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        // USD-M: payload 使用 b/a（不是现货 bids/asks）
        const std::string path = "/ws/" + stream_symbol + "@depth" + std::to_string(levels) + "@0ms";

        // REST 快照先解阻塞，避免 WS 首包/解析异常时主线程一直等
        try
        {
            const std::string rest_path =
                "/fapi/v1/depth?symbol=" + symbol + "&limit=" + std::to_string(levels);
            const std::string json = https_get(rest_host, rest_path);
            publish(binance_interface::parse_orderbook(json, symbol));
        }
        catch (const std::exception& e)
        {
            std::cerr << "MarketChannel REST depth bootstrap failed: " << e.what() << std::endl;
        }

        while (running)
        {
            try
            {
                WsHandles ws = ws_connect(host, path);
                while (running)
                {
                    const std::string msg = ws_recv(ws.socket);
                    if (msg.empty())
                        break;

                    publish(binance_interface::parse_orderbook(msg, symbol));
                }
                ws.close();
            }
            catch (const std::exception& e)
            {
                std::cerr << "MarketChannel WS error: " << e.what() << std::endl;
                if (!running)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            catch (...)
            {
                std::cerr << "MarketChannel WS unknown error" << std::endl;
                if (!running)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
    }
};

struct binance_interface::FundingChannel
{
    std::string host;
    std::string rest_host;
    std::string symbol;
    std::atomic<bool> running{false};
    std::thread worker;
    std::mutex mutex;
    funding_info latest;
    bool has_data = false;

    void start(std::string h, std::string rest_h, std::string sym)
    {
        host = std::move(h);
        rest_host = std::move(rest_h);
        symbol = std::move(sym);
        if (running.exchange(true))
            return;
        worker = std::thread([this] { run(); });
    }

    void stop()
    {
        if (!running.exchange(false))
            return;
        if (worker.joinable())
            worker.join();
    }

    funding_info get_latest()
    {
        std::lock_guard lock(mutex);
        if (!has_data)
            throw std::runtime_error("FundingChannel: no funding data yet");
        return latest;
    }

    void publish(funding_info info)
    {
        info.message_time = binance_interface::now_ms();
        {
            std::lock_guard lock(mutex);
            latest = std::move(info);
            has_data = true;
        }
    }

    void run()
    {
        std::string stream_symbol = symbol;
        std::transform(stream_symbol.begin(), stream_symbol.end(), stream_symbol.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        // markPrice 属于 /market 路由；含资金费率字段 r
        const std::string path = "/market/ws/" + stream_symbol + "@markPrice@1s";

        try
        {
            const std::string rest_path = "/fapi/v1/premiumIndex?symbol=" + symbol;
            const std::string json = https_get(rest_host, rest_path);
            publish(binance_interface::parse_funding(json, symbol));
        }
        catch (const std::exception& e)
        {
            std::cerr << "FundingChannel REST premiumIndex bootstrap failed: " << e.what() << std::endl;
        }

        while (running)
        {
            try
            {
                WsHandles ws = ws_connect(host, path);
                while (running)
                {
                    const std::string msg = ws_recv(ws.socket);
                    if (msg.empty())
                        break;
                    publish(binance_interface::parse_funding(msg, symbol));
                }
                ws.close();
            }
            catch (const std::exception& e)
            {
                std::cerr << "FundingChannel WS error: " << e.what() << std::endl;
                if (!running)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            catch (...)
            {
                std::cerr << "FundingChannel WS unknown error" << std::endl;
                if (!running)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
    }
};

struct binance_interface::AggTradeChannel
{
    static constexpr long long kMaxKeepMs = 60000;

    std::string host;
    std::string symbol;
    std::atomic<bool> running{false};
    std::thread worker;
    std::mutex mutex;
    std::deque<agg_trade_info> trades;

    void start(std::string h, std::string sym)
    {
        host = std::move(h);
        symbol = std::move(sym);
        if (running.exchange(true))
            return;
        worker = std::thread([this] { run(); });
    }

    void stop()
    {
        if (!running.exchange(false))
            return;
        if (worker.joinable())
            worker.join();
    }

    aggressor_flow get_flow(long long window_ms)
    {
        const long long now = binance_interface::now_ms();
        std::lock_guard lock(mutex);
        while (!trades.empty())
        {
            const long long t = trades.front().trade_time != 0
                ? trades.front().trade_time
                : trades.front().message_time;
            if (now - t <= kMaxKeepMs)
                break;
            trades.pop_front();
        }

        aggressor_flow flow;
        for (auto it = trades.rbegin(); it != trades.rend(); ++it)
        {
            const long long t = it->trade_time != 0 ? it->trade_time : it->message_time;
            if (now - t > window_ms)
                break;
            if (it->buyer_is_maker)
                flow.sell_qty += it->quantity; // 买方是 maker → 主动卖
            else
                flow.buy_qty += it->quantity;  // 买方是 taker → 主动买
        }
        flow.net_qty = flow.buy_qty - flow.sell_qty;
        const float sum = flow.buy_qty + flow.sell_qty;
        flow.imbalance = sum > 1e-12f ? flow.net_qty / sum : 0.f;
        return flow;
    }

    agg_trade_info peek_last()
    {
        std::lock_guard lock(mutex);
        if (trades.empty())
            return {};
        return trades.back();
    }

    void push(agg_trade_info trade)
    {
        trade.message_time = binance_interface::now_ms();
        if (trade.trade_time == 0)
            trade.trade_time = trade.message_time;
        const long long now = trade.message_time;
        std::lock_guard lock(mutex);
        trades.push_back(std::move(trade));
        while (!trades.empty())
        {
            const long long t = trades.front().trade_time != 0
                ? trades.front().trade_time
                : trades.front().message_time;
            if (now - t <= kMaxKeepMs)
                break;
            trades.pop_front();
        }
    }

    void run()
    {
        std::string stream_symbol = symbol;
        std::transform(stream_symbol.begin(), stream_symbol.end(), stream_symbol.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        // aggTrade 属于 /market 路由
        const std::string path = "/market/ws/" + stream_symbol + "@aggTrade";

        while (running)
        {
            try
            {
                WsHandles ws = ws_connect(host, path);
                while (running)
                {
                    const std::string msg = ws_recv(ws.socket);
                    if (msg.empty())
                        break;
                    push(binance_interface::parse_agg_trade(msg, symbol));
                }
                ws.close();
            }
            catch (const std::exception& e)
            {
                std::cerr << "AggTradeChannel WS error: " << e.what() << std::endl;
                if (!running)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            catch (...)
            {
                std::cerr << "AggTradeChannel WS unknown error" << std::endl;
                if (!running)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
    }
};

binance_interface::binance_interface() = default;

binance_interface::~binance_interface()
{
    stop();
}

void binance_interface::init(std::string credential_path)
{
    std::string content = loader::load_file(std::move(credential_path));
    while (!content.empty() && (content.back() == '\n' || content.back() == '\r'))
        content.pop_back();

    const auto first_end = content.find('\n');
    std::string api = (first_end == std::string::npos)
        ? content
        : content.substr(0, first_end);
    if (!api.empty() && api.back() == '\r')
        api.pop_back();

    std::string remainder = (first_end == std::string::npos)
        ? std::string{}
        : content.substr(first_end + 1);

    std::string testnet_line;
    const auto last_end = remainder.rfind('\n');
    if (last_end == std::string::npos)
    {
        testnet_line = remainder;
    }
    else
    {
        testnet_line = remainder.substr(last_end + 1);
    }
    if (!testnet_line.empty() && testnet_line.back() == '\r')
        testnet_line.pop_back();

    const bool last_is_flag = (testnet_line == "0" || testnet_line == "1"
        || testnet_line == "true" || testnet_line == "TRUE"
        || testnet_line == "false" || testnet_line == "FALSE");
    if (last_is_flag)
    {
        if (last_end == std::string::npos)
            remainder.clear();
        else
        {
            remainder.resize(last_end);
            if (!remainder.empty() && remainder.back() == '\r')
                remainder.pop_back();
        }
    }

    use_testnet_ = (testnet_line == "1" || testnet_line == "true" || testnet_line == "TRUE");
    // 账户/下单仍走现货 WS API；行情与合约账户查询走 USD-M
    ws_api_host_ = use_testnet_ ? "ws-api.testnet.binance.vision" : "ws-api.binance.com";
    ws_stream_host_ = use_testnet_ ? "stream.binancefuture.com" : "fstream.binance.com";
    ws_fapi_host_ = use_testnet_ ? "testnet.binancefuture.com" : "ws-fapi.binance.com";

    set_credentials(api, remainder);
}

void binance_interface::set_credentials(const std::string& api_key, const std::string& ed25519_private_key_pem)
{
    api_key_ = api_key;
    private_key_pem_ = ed25519_private_key_pem;
}

void binance_interface::start(uint32_t depth_levels)
{
    if (started_)
        return;

    const auto symbols = parse_inst_id(loader::load_file("instId.cfg"));
    signal_symbol_ = symbols.first;
    trade_symbol_ = symbols.second;
    split_trade_market_ = (signal_symbol_ != trade_symbol_);

    balance_channel_ = std::make_unique<RpcChannel>();
    order_channel_ = std::make_unique<RpcChannel>();
    futures_channel_ = std::make_unique<RpcChannel>();
    market_channel_ = std::make_unique<MarketChannel>();
    funding_channel_ = std::make_unique<FundingChannel>();
    agg_trade_channel_ = std::make_unique<AggTradeChannel>();

    balance_channel_->start(ws_api_host_);
    order_channel_->start(ws_api_host_);
    futures_channel_->start(ws_fapi_host_, "/ws-fapi/v1");
    const std::string rest_host = use_testnet_ ? "testnet.binancefuture.com" : "fapi.binance.com";
    market_channel_->start(ws_stream_host_, rest_host, signal_symbol_, depth_levels);
    funding_channel_->start(ws_stream_host_, rest_host, signal_symbol_);
    agg_trade_channel_->start(ws_stream_host_, signal_symbol_);

    if (split_trade_market_)
    {
        trade_market_channel_ = std::make_unique<MarketChannel>();
        trade_agg_trade_channel_ = std::make_unique<AggTradeChannel>();
        trade_market_channel_->start(ws_stream_host_, rest_host, trade_symbol_, depth_levels);
        trade_agg_trade_channel_->start(ws_stream_host_, trade_symbol_);
    }

    started_ = true;
}

void binance_interface::stop()
{
    if (!started_)
        return;
    started_ = false;

    if (market_channel_)
        market_channel_->stop();
    if (trade_market_channel_)
        trade_market_channel_->stop();
    if (funding_channel_)
        funding_channel_->stop();
    if (agg_trade_channel_)
        agg_trade_channel_->stop();
    if (trade_agg_trade_channel_)
        trade_agg_trade_channel_->stop();
    if (balance_channel_)
        balance_channel_->stop();
    if (order_channel_)
        order_channel_->stop();
    if (futures_channel_)
        futures_channel_->stop();

    market_channel_.reset();
    trade_market_channel_.reset();
    funding_channel_.reset();
    agg_trade_channel_.reset();
    trade_agg_trade_channel_.reset();
    balance_channel_.reset();
    order_channel_.reset();
    futures_channel_.reset();
}

std::pair<std::string, std::string> binance_interface::parse_inst_id(const std::string& content)
{
    std::vector<std::string> symbols;
    std::istringstream in(content);
    std::string line;
    while (std::getline(in, line))
    {
        trim_inplace(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        std::transform(line.begin(), line.end(), line.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        symbols.push_back(std::move(line));
        if (symbols.size() == 2)
            break;
    }
    if (symbols.empty())
        throw std::runtime_error("instId.cfg has no symbol");
    if (symbols.size() == 1)
        return {symbols[0], symbols[0]};
    return {symbols[0], symbols[1]};
}

std::string binance_interface::resolve_symbol(std::string symbol) const
{
    if (symbol.empty())
    {
        if (!trade_symbol_.empty())
            symbol = trade_symbol_;
        else
            symbol = parse_inst_id(loader::load_file("instId.cfg")).second;
    }
    trim_inplace(symbol);
    std::transform(symbol.begin(), symbol.end(), symbol.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (symbol.empty())
        throw std::runtime_error("symbol is empty");
    return symbol;
}

void binance_interface::ensure_ws_ok(const std::string& json, const char* what)
{
    const auto status = extract_json_int(json, "status");
    if (status != 0 && status != 200)
        throw std::runtime_error(std::string(what) + " failed: " + json);
}

std::string binance_interface::extract_result_object(const std::string& json)
{
    const auto result_pos = json.find("\"result\"");
    if (result_pos == std::string::npos)
        return json;
    const auto brace = json.find('{', result_pos);
    if (brace == std::string::npos)
        return json;
    return json.substr(brace);
}

orderbook_info binance_interface::get_ws_orderbook(uint32_t /*data_amount*/)
{
    if (!market_channel_)
        throw std::runtime_error("market channel not started; call start() first");
    return market_channel_->get_latest();
}

orderbook_info binance_interface::get_ws_trade_orderbook()
{
    if (trade_market_channel_)
        return trade_market_channel_->get_cached();
    if (!market_channel_)
        throw std::runtime_error("market channel not started; call start() first");
    return market_channel_->get_cached();
}

agg_trade_info binance_interface::get_ws_last_agg_trade(bool trade_market)
{
    AggTradeChannel* channel = (trade_market && trade_agg_trade_channel_)
        ? trade_agg_trade_channel_.get()
        : agg_trade_channel_.get();
    if (!channel)
        throw std::runtime_error("aggTrade channel not started; call start() first");
    return channel->peek_last();
}

funding_info binance_interface::get_ws_funding_rate()
{
    if (!funding_channel_)
        throw std::runtime_error("funding channel not started; call start() first");
    return funding_channel_->get_latest();
}

aggressor_flow binance_interface::get_ws_aggressor_flow(long long window_ms)
{
    if (!agg_trade_channel_)
        throw std::runtime_error("aggTrade channel not started; call start() first");
    return agg_trade_channel_->get_flow(window_ms);
}

std::vector<asset_balance> binance_interface::get_ws_balance(bool omit_zero_balances)
{
    if (api_key_.empty() || private_key_pem_.empty())
        throw std::runtime_error("Binance API credentials are not set");
    if (!balance_channel_)
        throw std::runtime_error("balance channel not started; call start() first");

    param_map params;
    if (omit_zero_balances)
        params["omitZeroBalances"] = "true";

    const std::string response = signed_ws_call(*balance_channel_, "account.status", std::move(params));
    ensure_ws_ok(response, "account.status");
    return parse_balances(response, omit_zero_balances);
}

binance_interface::tick_filter binance_interface::get_tick_filter(std::string symbol)
{
    symbol = resolve_symbol(std::move(symbol));
    const std::string host = use_testnet_ ? "testnet.binancefuture.com" : "fapi.binance.com";
    const std::string path = "/fapi/v1/exchangeInfo?symbol=" + symbol;
    return parse_tick_filter(https_get(host, path));
}

float binance_interface::get_tick_size(std::string symbol)
{
    return static_cast<float>(get_tick_filter(std::move(symbol)).tick_size);
}

order_info binance_interface::ws_place_order(const order_request& req)
{
    if (api_key_.empty() || private_key_pem_.empty())
        throw std::runtime_error("Binance API credentials are not set");
    if (!futures_channel_)
        throw std::runtime_error("futures channel not started; call start() first");
    if (req.side.empty() || req.type.empty())
        throw std::runtime_error("order side/type are required");

    param_map params;
    params["symbol"] = resolve_symbol(req.symbol);
    fill_trade_params(params, req);

    const std::string response = signed_ws_call(*futures_channel_, "order.place", std::move(params));
    ensure_ws_ok(response, "order.place");
    return parse_order(response);
}

order_info binance_interface::ws_modify_order(long long order_id, const order_request& req)
{
    param_map ids;
    ids["orderId"] = std::to_string(order_id);
    return modify_order_ws(std::move(ids), req);
}

order_info binance_interface::ws_modify_order(const std::string& client_order_id, const order_request& req)
{
    if (client_order_id.empty())
        throw std::runtime_error("client_order_id is required");
    param_map ids;
    ids["origClientOrderId"] = client_order_id;
    return modify_order_ws(std::move(ids), req);
}

order_info binance_interface::modify_order_ws(param_map cancel_id_params, order_request req)
{
    if (api_key_.empty() || private_key_pem_.empty())
        throw std::runtime_error("Binance API credentials are not set");
    if (!futures_channel_)
        throw std::runtime_error("futures channel not started; call start() first");
    if (req.side.empty())
        throw std::runtime_error("order side is required");

    param_map params = std::move(cancel_id_params);
    params["symbol"] = resolve_symbol(std::move(req.symbol));
    params["side"] = req.side;
    if (!req.quantity.empty())
        params["quantity"] = req.quantity;
    if (!req.price.empty())
        params["price"] = req.price;

    const std::string response = signed_ws_call(*futures_channel_, "order.modify", std::move(params));
    ensure_ws_ok(response, "order.modify");
    return parse_order(response);
}

order_info binance_interface::ws_get_order(long long order_id, std::string symbol)
{
    if (api_key_.empty() || private_key_pem_.empty())
        throw std::runtime_error("Binance API credentials are not set");
    if (!futures_channel_)
        throw std::runtime_error("futures channel not started; call start() first");

    param_map params;
    params["symbol"] = resolve_symbol(std::move(symbol));
    params["orderId"] = std::to_string(order_id);

    const std::string response = signed_ws_call(*futures_channel_, "order.status", std::move(params));
    ensure_ws_ok(response, "order.status");
    return parse_order(response);
}

order_info binance_interface::ws_get_order(const std::string& client_order_id, std::string symbol)
{
    if (api_key_.empty() || private_key_pem_.empty())
        throw std::runtime_error("Binance API credentials are not set");
    if (!futures_channel_)
        throw std::runtime_error("futures channel not started; call start() first");
    if (client_order_id.empty())
        throw std::runtime_error("client_order_id is required");

    param_map params;
    params["symbol"] = resolve_symbol(std::move(symbol));
    params["origClientOrderId"] = client_order_id;

    const std::string response = signed_ws_call(*futures_channel_, "order.status", std::move(params));
    ensure_ws_ok(response, "order.status");
    return parse_order(response);
}

std::vector<order_info> binance_interface::ws_open_orders(std::string symbol)
{
    if (api_key_.empty() || private_key_pem_.empty())
        throw std::runtime_error("Binance API credentials are not set");

    param_map params;
    if (!symbol.empty())
        params["symbol"] = resolve_symbol(std::move(symbol));

    const std::string response = signed_fapi_rest(L"GET", "/fapi/v1/openOrders", std::move(params));
    return parse_open_orders(response);
}

std::vector<position_info> binance_interface::ws_get_positions(std::string symbol, bool only_open)
{
    if (api_key_.empty() || private_key_pem_.empty())
        throw std::runtime_error("Binance API credentials are not set");
    if (!futures_channel_)
        throw std::runtime_error("futures channel not started; call start() first");

    param_map params;
    if (!symbol.empty())
        params["symbol"] = resolve_symbol(std::move(symbol));

    const std::string response = signed_ws_call(*futures_channel_, "account.position", std::move(params));
    ensure_ws_ok(response, "account.position");
    return parse_positions(response, only_open);
}

order_info binance_interface::ws_cancel_order(long long order_id, std::string symbol)
{
    if (api_key_.empty() || private_key_pem_.empty())
        throw std::runtime_error("Binance API credentials are not set");
    if (!futures_channel_)
        throw std::runtime_error("futures channel not started; call start() first");

    param_map params;
    params["symbol"] = resolve_symbol(std::move(symbol));
    params["orderId"] = std::to_string(order_id);

    const std::string response = signed_ws_call(*futures_channel_, "order.cancel", std::move(params));
    ensure_ws_ok(response, "order.cancel");
    return parse_order(response);
}

order_info binance_interface::ws_cancel_order(const std::string& client_order_id, std::string symbol)
{
    if (api_key_.empty() || private_key_pem_.empty())
        throw std::runtime_error("Binance API credentials are not set");
    if (!futures_channel_)
        throw std::runtime_error("futures channel not started; call start() first");
    if (client_order_id.empty())
        throw std::runtime_error("client_order_id is required");

    param_map params;
    params["symbol"] = resolve_symbol(std::move(symbol));
    params["origClientOrderId"] = client_order_id;

    const std::string response = signed_ws_call(*futures_channel_, "order.cancel", std::move(params));
    ensure_ws_ok(response, "order.cancel");
    return parse_order(response);
}

std::vector<order_info> binance_interface::ws_cancel_all_open_orders(std::string symbol)
{
    if (api_key_.empty() || private_key_pem_.empty())
        throw std::runtime_error("Binance API credentials are not set");

    param_map params;
    params["symbol"] = resolve_symbol(std::move(symbol));

    const std::string response = signed_fapi_rest(L"DELETE", "/fapi/v1/allOpenOrders", std::move(params));
    if (!response.empty() && response.front() == '[')
        return parse_open_orders(response);
    return {};
}

orderbook_info binance_interface::parse_orderbook(const std::string& json, const std::string& symbol)
{
    if (json.find("\"status\"") != std::string::npos)
        ensure_ws_ok(json, "depth");

    std::string payload = json;
    if (json.find("\"result\"") != std::string::npos)
        payload = extract_result_object(json);

    // 现货 Partial Depth: bids/asks + lastUpdateId
    // 合约 Depth: b/a + u（最终更新 ID）
    const bool spot_keys = payload.find("\"bids\":") != std::string::npos
        && payload.find("\"asks\":") != std::string::npos;
    const bool futures_keys = payload.find("\"b\":") != std::string::npos
        && payload.find("\"a\":") != std::string::npos;
    if (!spot_keys && !futures_keys)
        throw std::runtime_error("orderbook fields not found in response: " + json);

    orderbook_info book;
    book.symbol = symbol;
    book.last_update_id = extract_json_int(payload, "lastUpdateId");
    if (book.last_update_id == 0)
        book.last_update_id = extract_json_int(payload, "u");
    book.transact_time = extract_json_int(payload, "T");
    if (book.transact_time == 0)
        book.transact_time = extract_json_int(payload, "transactTime");
    if (book.transact_time == 0)
        book.transact_time = extract_json_int(payload, "E");
    if (book.transact_time == 0)
        book.transact_time = extract_json_int(payload, "eventTime");
    if (book.transact_time == 0)
        book.transact_time = extract_json_int(json, "T");
    if (book.transact_time == 0)
        book.transact_time = extract_json_int(json, "E");

    if (spot_keys)
    {
        book.bids = parse_price_levels(payload, "bids");
        book.asks = parse_price_levels(payload, "asks");
    }
    else
    {
        book.bids = parse_price_levels(payload, "b");
        book.asks = parse_price_levels(payload, "a");
    }
    return book;
}

funding_info binance_interface::parse_funding(const std::string& json, const std::string& symbol)
{
    auto parse_float_field = [](const std::string& payload, const std::string& key) -> float
    {
        const std::string s = extract_json_string(payload, key);
        if (s.empty())
            return 0.f;
        try
        {
            return std::stof(s);
        }
        catch (const std::exception&)
        {
            return 0.f;
        }
    };

    funding_info info;
    info.symbol = symbol;
    // WS markPrice: r/p/i/T/E；REST premiumIndex: lastFundingRate/markPrice/indexPrice/nextFundingTime
    info.funding_rate = parse_float_field(json, "r");
    if (info.funding_rate == 0.f)
        info.funding_rate = parse_float_field(json, "lastFundingRate");
    info.mark_price = parse_float_field(json, "p");
    if (info.mark_price == 0.f)
        info.mark_price = parse_float_field(json, "markPrice");
    info.index_price = parse_float_field(json, "i");
    if (info.index_price == 0.f)
        info.index_price = parse_float_field(json, "indexPrice");
    info.next_funding_time = extract_json_int(json, "T");
    if (info.next_funding_time == 0)
        info.next_funding_time = extract_json_int(json, "nextFundingTime");
    info.event_time = extract_json_int(json, "E");
    if (info.event_time == 0)
        info.event_time = extract_json_int(json, "time");

    const std::string sym = extract_json_string(json, "s");
    if (sym.empty())
    {
        const std::string sym2 = extract_json_string(json, "symbol");
        if (!sym2.empty())
            info.symbol = sym2;
    }
    else
    {
        info.symbol = sym;
    }
    return info;
}

agg_trade_info binance_interface::parse_agg_trade(const std::string& json, const std::string& symbol)
{
    auto parse_float_field = [](const std::string& payload, const std::string& key) -> float
    {
        const std::string s = extract_json_string(payload, key);
        if (s.empty())
            return 0.f;
        try
        {
            return std::stof(s);
        }
        catch (const std::exception&)
        {
            return 0.f;
        }
    };

    agg_trade_info trade;
    trade.symbol = symbol;
    const std::string sym = extract_json_string(json, "s");
    if (!sym.empty())
        trade.symbol = sym;
    trade.price = parse_float_field(json, "p");
    trade.quantity = parse_float_field(json, "q");
    trade.buyer_is_maker = extract_json_bool(json, "m", false);
    trade.trade_time = extract_json_int(json, "T");
    if (trade.trade_time == 0)
        trade.trade_time = extract_json_int(json, "E");
    return trade;
}

order_info binance_interface::parse_order(const std::string& json)
{
    return parse_order_fields(extract_result_object(json));
}

std::vector<order_info> binance_interface::parse_open_orders(const std::string& json)
{
    std::vector<order_info> orders;
    std::string payload = json;
    if (!payload.empty() && payload.front() == '[')
        payload = "{\"result\":" + payload + "}";
    auto objs = extract_json_object_array(payload, "result");
    if (objs.empty())
    {
        // 个别环境下 result 可能是单对象
        const auto result_pos = json.find("\"result\"");
        if (result_pos != std::string::npos)
        {
            const auto brace = json.find('{', result_pos);
            const auto bracket = json.find('[', result_pos);
            if (brace != std::string::npos && (bracket == std::string::npos || brace < bracket))
                objs.push_back(extract_result_object(json));
        }
    }
    orders.reserve(objs.size());
    for (const auto& obj : objs)
        orders.push_back(parse_order_fields(obj));
    return orders;
}

std::vector<position_info> binance_interface::parse_positions(const std::string& json, bool only_open)
{
    std::vector<position_info> positions;
    const auto objs = extract_json_object_array(json, "result");
    positions.reserve(objs.size());
    for (const auto& obj : objs)
    {
        position_info pos;
        pos.symbol = extract_json_string(obj, "symbol");
        pos.position_amt = extract_json_float(obj, "positionAmt");
        pos.entry_price = extract_json_float(obj, "entryPrice");
        pos.mark_price = extract_json_float(obj, "markPrice");
        pos.unrealized_profit = extract_json_float(obj, "unRealizedProfit");
        if (pos.unrealized_profit == 0.f)
            pos.unrealized_profit = extract_json_float(obj, "unrealizedProfit");
        pos.liquidation_price = extract_json_float(obj, "liquidationPrice");
        pos.leverage = extract_json_float(obj, "leverage");
        pos.notional = extract_json_float(obj, "notional");
        if (pos.notional == 0.f)
            pos.notional = extract_json_float(obj, "notionalValue");
        pos.isolated_margin = extract_json_float(obj, "isolatedMargin");
        pos.margin_type = extract_json_string(obj, "marginType");
        pos.position_side = extract_json_string(obj, "positionSide");
        pos.update_time = extract_json_int(obj, "updateTime");
        pos.raw_json = obj;

        if (only_open && std::fabs(pos.position_amt) < 1e-12f)
            continue;
        if (pos.symbol.empty())
            continue;
        positions.push_back(std::move(pos));
    }
    return positions;
}

std::string binance_interface::signed_ws_call(RpcChannel& channel, const std::string& method, param_map params) const
{
    params["apiKey"] = api_key_;
    params["timestamp"] = std::to_string(now_ms());

    std::string payload;
    for (const auto& [key, value] : params)
    {
        if (!payload.empty())
            payload += '&';
        payload += key;
        payload += '=';
        payload += value;
    }
    params["signature"] = ed25519_sign_base64(payload);

    std::string req_id = std::to_string(now_ms()) + "-";
    req_id.reserve(req_id.size() + method.size());
    for (char c : method)
    {
        const unsigned char uc = static_cast<unsigned char>(c);
        req_id.push_back((std::isalnum(uc) || c == '-' || c == '_') ? c : '-');
    }
    if (req_id.size() > 36)
        req_id.resize(36);
    std::ostringstream req;
    req << "{\"id\":\"" << json_escape(req_id) << "\",\"method\":\"" << json_escape(method) << "\",\"params\":{";
    bool first = true;
    for (const auto& [key, value] : params)
    {
        if (!first)
            req << ',';
        first = false;
        req << '"' << json_escape(key) << "\":";
        if (is_json_number_param(key) || is_json_bool_param(key))
            req << value;
        else
            req << '"' << json_escape(value) << '"';
    }
    req << "}}";

    return channel.call(req_id, req.str());
}

std::string binance_interface::signed_fapi_rest(const wchar_t* http_method, const std::string& path, param_map params) const
{
    params["timestamp"] = std::to_string(now_ms());

    std::string payload;
    for (const auto& [key, value] : params)
    {
        if (!payload.empty())
            payload += '&';
        payload += key;
        payload += '=';
        payload += value;
    }
    const std::string signature = ed25519_sign_base64(payload);
    const std::string host = use_testnet_ ? "testnet.binancefuture.com" : "fapi.binance.com";
    const std::string full_path = path + "?" + payload + "&signature=" + url_encode(signature);
    const std::wstring headers = L"X-MBX-APIKEY: " + utf8_to_wide(api_key_) + L"\r\n";

    const auto result = https_call(host, http_method, full_path, headers);

    if (result.status != 200)
        throw std::runtime_error(std::string("fapi REST ") + result.body);
    if (!result.body.empty() && result.body.front() == '{')
    {
        const auto code = extract_json_int(result.body, "code");
        if (code < 0)
            throw std::runtime_error("fapi REST error: " + result.body);
    }
    return result.body;
}

long long binance_interface::now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string binance_interface::ed25519_sign_base64(const std::string& message) const
{
    const auto der = pem_to_der(private_key_pem_);
    const auto seed = ed25519_seed_from_pkcs8_der(der);
    return ed25519_sign_b64(seed, message);
}

std::vector<asset_balance> binance_interface::parse_balances(const std::string& json, bool omit_zero)
{
    std::vector<asset_balance> result;
    const auto balances_key = json.find("\"balances\"");
    if (balances_key == std::string::npos)
    {
        if (json.find("\"code\"") != std::string::npos || json.find("\"msg\"") != std::string::npos)
            throw std::runtime_error("Binance API error: " + json);
        throw std::runtime_error("balances field not found in response");
    }

    const auto arr_begin = json.find('[', balances_key);
    if (arr_begin == std::string::npos)
        throw std::runtime_error("balances array not found");

    size_t i = arr_begin + 1;
    while (i < json.size())
    {
        while (i < json.size() && (json[i] == ' ' || json[i] == '\n' || json[i] == '\r' || json[i] == '\t' || json[i] == ','))
            ++i;
        if (i >= json.size() || json[i] == ']')
            break;
        if (json[i] != '{')
            break;

        const auto obj_end = json.find('}', i);
        if (obj_end == std::string::npos)
            break;

        const std::string obj = json.substr(i, obj_end - i + 1);
        asset_balance bal;
        bal.asset = extract_json_string(obj, "asset");
        bal.free = extract_json_string(obj, "free");
        bal.locked = extract_json_string(obj, "locked");

        if (!bal.asset.empty())
        {
            if (!omit_zero || !is_zero_amount(bal.free) || !is_zero_amount(bal.locked))
                result.push_back(std::move(bal));
        }

        i = obj_end + 1;
    }

    return result;
}
