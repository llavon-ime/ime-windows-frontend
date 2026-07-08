#pragma once

#if 1

class DebugSink {
public:
    static DebugSink& instance() {
        static DebugSink s;
        return s;
    }

    inline void connect(const char* host = "239.255.42.99", unsigned short port = 9876) {
        (void)host;
        (void)port;
    }

    inline void disconnect() {}

    template <typename Text>
    inline void send(const wchar_t* tag, const Text& text) {
        (void)tag;
        (void)text;
    }

    bool isConnected() const {
        return false;
    }

private:
    DebugSink() = default;
    DebugSink(const DebugSink&) = delete;
    DebugSink& operator=(const DebugSink&) = delete;
};

#else

#include <winsock2.h>
#include <ws2tcpip.h>

#include <iterator>
#include <string>

#include "utf8cpp/utf8/cpp20.h"

class DebugSink {
public:
    static DebugSink& instance() {
        static DebugSink s;
        return s;
    }

    inline void connect(const char* host = "239.255.42.99", unsigned short port = 9876) {
        if (_sock != INVALID_SOCKET) return;

        if (!_wsaStarted) {
            WSADATA wsaData{};
            if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return;
            _wsaStarted = true;
        }

        SOCKET sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET) return;

        in_addr multicastAddr{};
        if (inet_pton(AF_INET, host, &multicastAddr) != 1) {
            ::closesocket(sock);
            return;
        }

        _dest = {};
        _dest.sin_family = AF_INET;
        _dest.sin_port = htons(port);
        _dest.sin_addr = multicastAddr;

        const int ttl = 1;
        setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl));

        const int loop = 1;
        setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&loop), sizeof(loop));

        in_addr loopbackIf{};
        if (inet_pton(AF_INET, "127.0.0.1", &loopbackIf) == 1) {
            setsockopt(
                sock, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&loopbackIf), sizeof(loopbackIf));
        }

        _sock = sock;
    }

    inline void disconnect() {
        if (_sock != INVALID_SOCKET) {
            ::closesocket(_sock);
            _sock = INVALID_SOCKET;
        }
        if (_wsaStarted) {
            WSACleanup();
            _wsaStarted = false;
        }
    }

    inline void send(const wchar_t* tag, const std::string& text) {
        sendUtf8(toUtf8(tag), toUtf8(text));
    }

    inline void send(const wchar_t* tag, const std::wstring& text) {
        sendUtf8(toUtf8(tag), toUtf8(text));
    }

    inline void send(const wchar_t* tag, const std::u16string& text) {
        sendUtf8(toUtf8(tag), toUtf8(text));
    }

    inline void send(const wchar_t* tag, const std::u32string& text) {
        sendUtf8(toUtf8(tag), toUtf8(text));
    }

    inline void send(const wchar_t* tag, const char* text) {
        send(tag, std::string(text != nullptr ? text : ""));
    }

    inline void send(const wchar_t* tag, const wchar_t* text) {
        send(tag, std::wstring(text != nullptr ? text : L""));
    }

    inline void send(const wchar_t* tag, const char16_t* text) {
        send(tag, std::u16string(text != nullptr ? text : u""));
    }

    inline void send(const wchar_t* tag, const char32_t* text) {
        send(tag, std::u32string(text != nullptr ? text : U""));
    }

    inline void send(const wchar_t* tag, char ch) {
        send(tag, std::string(1, ch));
    }

    inline void send(const wchar_t* tag, wchar_t ch) {
        send(tag, std::wstring(1, ch));
    }

    inline void send(const wchar_t* tag, char16_t ch) {
        send(tag, std::u16string(1, ch));
    }

    inline void send(const wchar_t* tag, char32_t ch) {
        send(tag, std::u32string(1, ch));
    }

    bool isConnected() const {
        return _sock != INVALID_SOCKET;
    }

private:
    DebugSink() = default;
    ~DebugSink() {
        disconnect();
    }

    DebugSink(const DebugSink&) = delete;
    DebugSink& operator=(const DebugSink&) = delete;

    bool _wsaStarted = false;
    SOCKET _sock = INVALID_SOCKET;
    sockaddr_in _dest{};

    inline void sendUtf8(const std::string& tag, const std::string& text) {
        if (_sock == INVALID_SOCKET) {
            connect();
            if (_sock == INVALID_SOCKET) return;
        }

        std::string msg;
        msg.reserve(tag.size() + text.size() + 4);
        msg += "[";
        msg += tag;
        msg += "] ";
        msg += text;
        msg += "\n";

        const int sent = ::sendto(_sock, msg.data(), static_cast<int>(msg.size()), 0,
                                  reinterpret_cast<const sockaddr*>(&_dest), static_cast<int>(sizeof(_dest)));

        if (sent == SOCKET_ERROR) {
            ::closesocket(_sock);
            _sock = INVALID_SOCKET;
        }
    }

    static std::string toUtf8(const wchar_t* text) {
        return text != nullptr ? toUtf8(std::wstring(text)) : std::string{};
    }

    static std::string toUtf8(const std::string& text) {
        std::string result;
        utf8::replace_invalid(text.begin(), text.end(), std::back_inserter(result));
        return result;
    }

    static std::string toUtf8(const std::wstring& text) {
        static_assert(sizeof(wchar_t) == sizeof(char16_t), "This Windows build expects UTF-16 wchar_t");
        return fromUtf16(text.begin(), text.end());
    }

    static std::string toUtf8(const std::u16string& text) {
        return fromUtf16(text.begin(), text.end());
    }

    static std::string toUtf8(const std::u32string& text) {
        return fromUtf32(text.begin(), text.end());
    }

    template <typename Iterator>
    static std::string fromUtf16(Iterator first, Iterator last) {
        std::string result;
        try {
            utf8::utf16to8(first, last, std::back_inserter(result));
        } catch (const utf8::exception&) {
            result.clear();
        }
        return result;
    }

    template <typename Iterator>
    static std::string fromUtf32(Iterator first, Iterator last) {
        std::string result;
        try {
            utf8::utf32to8(first, last, std::back_inserter(result));
        } catch (const utf8::exception&) {
            result.clear();
        }
        return result;
    }
};

#endif
