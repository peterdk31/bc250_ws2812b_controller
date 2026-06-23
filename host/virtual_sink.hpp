#pragma once

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>
#include <memory>
#include <vector>
#include "sink.hpp"
#include "virtual_strip_socket.hpp"

// Sink that mirrors every wire frame onto a local AF_UNIX datagram socket,
// where the standalone on-screen "virtual LED strip" viewer
// (host/virtual_strip.cpp) renders it. This is how an effect is previewed
// with no hardware: run the viewer, then run `led` with no serial port.
//
// Best-effort and fully decoupled: the viewer binds the socket path and this
// sendto()s to it. When no viewer is running the send simply fails and is
// swallowed, so a missing viewer is a true no-op. The send is non-blocking
// (MSG_DONTWAIT), so a slow or gone viewer can never stall the render loop;
// such frames just drop. send() therefore never reports a fatal error.
//
// Config-less: there is nothing to tune. The socket path is a fixed shared
// constant (virtual_strip_socket.hpp) the daemon and viewer agree on, and
// because mirroring is a no-op when no viewer is bound, this sink is always
// attached — you never edit config to preview, you just launch the viewer.
class VirtualSink : public Sink
{
public:
    // Create the sink, or nullptr if the socket can't be made (never fatal;
    // the daemon just runs without an on-screen mirror).
    static std::unique_ptr<VirtualSink> create()
    {
        int fd = socket(AF_UNIX, SOCK_DGRAM, 0);

        if (fd < 0)
            return nullptr;

        return std::unique_ptr<VirtualSink>(
            new VirtualSink(fd, kVirtualStripSocket));
    }

    VirtualSink(const VirtualSink&) = delete;
    VirtualSink& operator=(const VirtualSink&) = delete;

    ~VirtualSink()
    {
        if (fd_ >= 0) close(fd_);
    }

    // fire-and-forget the exact bytes that just went to the strip. Any
    // failure (no viewer bound, its buffer full, viewer gone) is ignored:
    // the mirror is a no-op whenever nothing is listening, and never fatal.
    bool send(const std::vector<uint8_t>& frame) override
    {
        if (fd_ >= 0 && !frame.empty())
            (void)sendto(fd_, frame.data(), frame.size(), MSG_DONTWAIT,
                         (const sockaddr*)&addr_, sizeof addr_);

        return true;
    }

private:
    VirtualSink(int fd, const char* path) : fd_(fd)
    {
        memset(&addr_, 0, sizeof addr_);
        addr_.sun_family = AF_UNIX;
        strncpy(addr_.sun_path, path, sizeof addr_.sun_path - 1);
    }

    int fd_ = -1;
    sockaddr_un addr_{};
};
