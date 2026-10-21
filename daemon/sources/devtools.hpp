#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <random>
#include <string>
#include "json.hpp"

// Minimal Chromium DevTools client: evaluate JavaScript in one page of a
// local browser. The WebSocket is the client subset of RFC 6455 this needs
// (loopback, no TLS or extensions); Sec-WebSocket-Accept isn't checked, as
// it only guards against proxies. Blocking with a timeout: keep it off
// threads that mustn't wait.
namespace devtools
{

class Session
{
public:
    ~Session() { close(); }

    bool open() const { return fd_ >= 0; }

    void close()
    {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = -1;
        rbuf_.clear();
    }

    bool connect(int port, const std::string& title, std::string& err)
    {
        close();

        std::string body;
        if (!httpGet(port, "/json", body, err))
            return false;

        json::Value targets;
        std::string perr;
        if (!json::parse(body, targets, perr) || !targets.isArray())
            return fail(err, "the debug port's target list isn't JSON (" + perr + ")");

        std::string ws;
        for (const auto& t : targets.items)
        {
            const json::Value* name = t.find("title");
            const json::Value* url = t.find("webSocketDebuggerUrl");
            if (name && url && json::toString(*name) == title)
                ws = json::toString(*url);
        }

        if (ws.empty())
            return fail(err, "no " + title + " page on the debug port");

        // ws://host:port/devtools/page/<id> -> the path
        size_t scheme = ws.find("://");
        size_t slash = scheme == std::string::npos ? std::string::npos : ws.find('/', scheme + 3);
        if (slash == std::string::npos)
            return fail(err, "unexpected WebSocket URL " + ws);

        return upgrade(port, ws.substr(slash), err);
    }

    // on failure the caller closes the session
    bool evaluate(const char* js, json::Value& out, std::string& err)
    {
        int id = ++nextId_;
        std::string req = "{\"id\":" + std::to_string(id) +
                          ",\"method\":\"Runtime.evaluate\",\"params\":{\"expression\":" +
                          quote(js) + ",\"returnByValue\":true}}";

        if (!send(0x1, req))
            return fail(err, "the debug port dropped the connection");

        // no domain is enabled, so anything but our reply is stray
        for (int tries = 0; tries < 32; tries++)
        {
            std::string msg;
            if (!recvMessage(msg))
                return fail(err, "the debug port dropped the connection");

            json::Value reply;
            std::string perr;
            if (!json::parse(msg, reply, perr))
                continue;

            const json::Value* rid = reply.find("id");
            if (!rid || !rid->isNumber() || (int)rid->number != id)
                continue;

            const json::Value* result = reply.find("result");
            if (!result)
                return fail(err, "Runtime.evaluate failed");
            if (result->find("exceptionDetails"))
                return fail(err, "the script threw in the page");

            const json::Value* inner = result->find("result");
            const json::Value* value = inner ? inner->find("value") : nullptr;
            out = value ? *value : json::Value{};
            return true;
        }

        return fail(err, "no reply from Runtime.evaluate");
    }

private:
    static constexpr int TIMEOUT_MS = 2000;
    static constexpr size_t MAX_MESSAGE = 4u << 20;

    int fd_ = -1;
    int nextId_ = 0;
    std::string rbuf_;     // received, not yet consumed
    std::mt19937 rng_{std::random_device{}()};

    static bool fail(std::string& err, const std::string& why)
    {
        err = why;
        return false;
    }

    static std::string quote(const char* s)
    {
        std::string out = "\"";

        for (; *s; s++)
        {
            unsigned char c = (unsigned char)*s;
            if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
            else if (c == '\n') out += "\\n";
            else if (c < 0x20)
            {
                char u[8];
                snprintf(u, sizeof u, "\\u%04x", c);
                out += u;
            }
            else out += (char)c;
        }

        return out + "\"";
    }

    static int tcp(int port)
    {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0)
            return -1;

        timeval tv{TIMEOUT_MS / 1000, (TIMEOUT_MS % 1000) * 1000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (::connect(fd, (sockaddr*)&a, sizeof a) != 0)
        {
            ::close(fd);
            return -1;
        }

        return fd;
    }

    static bool sendAll(int fd, const std::string& s)
    {
        for (size_t off = 0; off < s.size();)
        {
            ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
            if (n <= 0)
                return false;
            off += (size_t)n;
        }
        return true;
    }

    static bool httpGet(int port, const char* path, std::string& body, std::string& err)
    {
        int fd = tcp(port);
        if (fd < 0)
            return fail(err, "nothing listening on 127.0.0.1:" + std::to_string(port));

        std::string req = std::string("GET ") + path + " HTTP/1.1\r\nHost: 127.0.0.1:" +
                          std::to_string(port) + "\r\nConnection: close\r\n\r\n";

        std::string resp;
        bool ok = sendAll(fd, req);
        size_t want = std::string::npos;

        while (ok && resp.size() < MAX_MESSAGE)
        {
            char buf[4096];
            ssize_t n = recv(fd, buf, sizeof buf, 0);
            if (n <= 0)
                break;
            resp.append(buf, (size_t)n);

            size_t head = resp.find("\r\n\r\n");
            if (head != std::string::npos && want == std::string::npos)
            {
                const char* cl = strcasestr(resp.c_str(), "\r\ncontent-length:");
                if (cl && cl < resp.c_str() + head)
                    want = head + 4 + strtoull(cl + 17, nullptr, 10);
            }
            if (want != std::string::npos && resp.size() >= want)
                break;
        }

        ::close(fd);

        size_t head = resp.find("\r\n\r\n");
        if (!ok || head == std::string::npos || resp.compare(0, 12, "HTTP/1.1 200") != 0)
            return fail(err, "the debug port didn't answer GET " + std::string(path));

        body = resp.substr(head + 4);
        return true;
    }

    static std::string base64(const uint8_t* p, size_t n)
    {
        static const char* A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;

        for (size_t i = 0; i < n; i += 3)
        {
            uint32_t v = p[i] << 16 | (i + 1 < n ? p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
            out += A[v >> 18 & 63];
            out += A[v >> 12 & 63];
            out += i + 1 < n ? A[v >> 6 & 63] : '=';
            out += i + 2 < n ? A[v & 63] : '=';
        }

        return out;
    }

    bool upgrade(int port, const std::string& path, std::string& err)
    {
        fd_ = tcp(port);

        uint8_t key[16];
        for (auto& b : key)
            b = (uint8_t)rng_();

        // no Origin header: DevTools only checks the origin when one is sent
        std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                          "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " +
                          base64(key, sizeof key) + "\r\nSec-WebSocket-Version: 13\r\n\r\n";

        size_t head = std::string::npos;
        bool ok = fd_ >= 0 && sendAll(fd_, req);

        while (ok && (head = rbuf_.find("\r\n\r\n")) == std::string::npos)
            ok = rbuf_.size() <= 16384 && fill(rbuf_.size() + 1);

        if (!ok || rbuf_.compare(0, 12, "HTTP/1.1 101") != 0)
        {
            close();
            return fail(err, "the debug port refused the WebSocket");
        }

        rbuf_.erase(0, head + 4); // frames may follow the headers in the same read
        return true;
    }

    bool fill(size_t need)
    {
        while (rbuf_.size() < need)
        {
            char buf[16384];
            ssize_t n = recv(fd_, buf, sizeof buf, 0);
            if (n <= 0)
                return false;
            rbuf_.append(buf, (size_t)n);
        }
        return true;
    }

    // client frames must be masked
    bool send(int opcode, const std::string& payload)
    {
        std::string f;
        size_t n = payload.size();

        f += (char)(0x80 | opcode);
        if (n < 126)
            f += (char)(0x80 | n);
        else if (n < 65536)
        {
            f += (char)(0x80 | 126);
            f += (char)(n >> 8);
            f += (char)(n & 0xFF);
        }
        else
        {
            f += (char)(0x80 | 127);
            for (int i = 7; i >= 0; i--)
                f += (char)((uint64_t)n >> (8 * i) & 0xFF);
        }

        uint8_t mask[4];
        for (auto& b : mask)
            b = (uint8_t)rng_();
        f.append((const char*)mask, 4);

        for (size_t i = 0; i < n; i++)
            f += (char)(payload[i] ^ mask[i & 3]);

        return sendAll(fd_, f);
    }

    // joins continuations, answers pings; a close frame is a failure
    bool recvMessage(std::string& msg)
    {
        msg.clear();

        for (;;)
        {
            if (!fill(2))
                return false;

            uint8_t b0 = (uint8_t)rbuf_[0], b1 = (uint8_t)rbuf_[1];
            uint64_t len = b1 & 0x7F;
            size_t hdr = 2;

            if (len == 126)
            {
                if (!fill(4)) return false;
                len = (uint64_t)(uint8_t)rbuf_[2] << 8 | (uint8_t)rbuf_[3];
                hdr = 4;
            }
            else if (len == 127)
            {
                if (!fill(10)) return false;
                len = 0;
                for (int i = 2; i < 10; i++)
                    len = len << 8 | (uint8_t)rbuf_[i];
                hdr = 10;
            }

            bool masked = b1 & 0x80;
            if (masked)
                hdr += 4;

            if (len > MAX_MESSAGE || msg.size() + len > MAX_MESSAGE || !fill(hdr + len))
                return false;

            std::string payload = rbuf_.substr(hdr, len);
            if (masked)
                for (size_t i = 0; i < payload.size(); i++)
                    payload[i] ^= rbuf_[hdr - 4 + (i & 3)];
            rbuf_.erase(0, hdr + len);

            int op = b0 & 0x0F;
            if (op == 0x8)
                return false;           // close
            if (op == 0x9)
            {
                if (!send(0xA, payload)) return false;
                continue;               // ping -> pong
            }
            if (op == 0xA)
                continue;               // unsolicited pong

            msg += payload;
            if (b0 & 0x80)
                return true;
        }
    }
};

} // namespace devtools
