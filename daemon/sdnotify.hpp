#pragma once

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

// The systemd watchdog's keep-alive (sd_notify "WATCHDOG=1"), without
// libsystemd: one datagram to $NOTIFY_SOCKET. The unit's WatchdogSec= is what
// turns it on — systemd then sets WATCHDOG_USEC, and kills a daemon whose pings
// stop (SIGABRT), which runs ExecStopPost and so hands the host fan outputs
// back (daemon/pwmout.hpp): a hung daemon can't leave a fan frozen at its
// last duty. Outside systemd (a run by hand) the variables are absent and
// this does nothing.
namespace sdnotify
{
inline double monotonic()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// ping, at most a few times per watchdog period; cheap to call every frame
inline void watchdog()
{
    static int state = 0; // 0 = not looked yet, 1 = on, -1 = off
    static int fd = -1;
    static sockaddr_un addr;
    static socklen_t addrLen = 0;
    static double every = 0, last = -1e9;

    if (state == 0)
    {
        state = -1;
        const char* sock = getenv("NOTIFY_SOCKET");
        const char* usec = getenv("WATCHDOG_USEC");
        const char* pid = getenv("WATCHDOG_PID");
        if (!sock || !*sock || !usec || atoll(usec) <= 0)
            return;
        if (pid && *pid && atoll(pid) != (long long)getpid())
            return; // meant for another process
        size_t n = strlen(sock);
        if (n >= sizeof addr.sun_path)
            return;
        memset(&addr, 0, sizeof addr);
        addr.sun_family = AF_UNIX;
        memcpy(addr.sun_path, sock, n);
        if (addr.sun_path[0] == '@')
            addr.sun_path[0] = 0; // an abstract socket
        addrLen = (socklen_t)(offsetof(sockaddr_un, sun_path) + n);
        fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd < 0)
            return;
        every = atoll(usec) / 1e6 / 4; // a quarter of the period
        state = 1;
        fprintf(stderr, "watchdog: pinging systemd every %.1f s\n", every);
    }

    if (state < 0)
        return;
    double now = monotonic();
    if (now - last < every)
        return;
    last = now;
    static const char msg[] = "WATCHDOG=1";
    sendto(fd, msg, sizeof msg - 1, MSG_NOSIGNAL, (const sockaddr*)&addr, addrLen);
}
} // namespace sdnotify
