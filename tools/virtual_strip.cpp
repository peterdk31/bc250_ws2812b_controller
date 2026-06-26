// Standalone on-screen "virtual LED strip".
//
// Binds a local datagram socket and renders whatever wire frames arrive as
// a row of truecolor blocks in the terminal. The `led` daemon mirrors every
// frame it sends to the ESP32 onto this socket (see host/virtual_sink.hpp),
// so this shows exactly what the physical strip shows — the same bytes,
// already brightness/gamma/white-balance corrected — with no hardware.
//
// Decoupled by design: when this isn't running the daemon's mirror send is
// a no-op, so you can leave the daemon as-is and just launch this to preview
// an effect before deploying to the BC-250.
//
//   make virtual-strip
//   ./virtual-strip                      # then start `led`, or run an effect:
//   LED_PORT=none ./led config.json aurora   # (in another terminal)
//
// LED_PORT=none runs the daemon headless (no serial device), so it works on a
// dev box with no hardware; drop it to also drive a real strip while you watch.
//
// Build: plain g++, no dependencies. Needs a truecolor-capable terminal.
//
// Wire frame (common/protocol.hpp): AA 55 pin lo hi anim(2) xms(2) <R G B>*n
// checksum. anim/xms drive the crossfade this viewer mirrors (see Viewer).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string>
#include <vector>
#include "receiver.hpp"
#include "fade.hpp"
#include "virtual_strip_socket.hpp"

// the daemon's VirtualSink and this viewer share kVirtualStripSocket so the
// default path can't drift between them; an argv override still wins
static const char* SOCK_DEFAULT = kVirtualStripSocket;

static volatile sig_atomic_t g_stop = 0;
static std::string g_sockPath;

static void onStop(int) { g_stop = 1; }

static void restoreTerminal()
{
    // show the cursor again and drop our socket file so the next run (and
    // the daemon's no-op detection) starts clean
    printf("\x1b[?25h\n");
    fflush(stdout);

    if (!g_sockPath.empty())
        unlink(g_sockPath.c_str());
}

static double nowSeconds()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// Renders decoded pixel frames as a row of truecolor blocks. As a
// proto::FrameHandler it shares the daemon's/ESP32's wire parser
// (common/receiver.hpp), so checksum validation and AA-55 framing live in one
// place; this just paints whatever valid pixel frame the Receiver hands it.
// Command frames fall through to the default no-op — the viewer ignores them,
// just as they aren't mirrored onto the socket in the first place.
//
// It is a faithful software ESP32 for transitions too: it keeps the last frame
// it showed and, when a frame's anim id changes, dissolves the new one in over
// it with the same shared fader the firmware uses (common/fade.hpp), so the
// preview shows the exact crossfades the strip will.
struct Viewer : proto::FrameHandler
{
    double fps = 0;    // updated by the loop, shown in the header line
    bool drew = false; // set on each painted frame; the loop polls and clears it

    void onPixels(uint8_t pin, uint16_t count, uint16_t anim, uint16_t xms,
                  const uint8_t* rgb) override
    {
        size_t n = (size_t)count * 3;
        work_.assign(rgb, rgb + n); // a mutable copy we can blend in place

        uint32_t now = (uint32_t)(nowSeconds() * 1000.0);

        // a new animation: freeze the frame on screen and dissolve into this
        // one. Skip the very first frame (nothing to fade from) and any
        // geometry change (the held frame no longer lines up).
        if (haveLast_ && anim != lastAnim_ && lastShown_.size() == n)
            fader_.begin(lastShown_.data(), count, xms, now);

        fader_.apply(work_.data(), count, now);

        std::string out = "\x1b[H"; // home the cursor and repaint over last frame

        char hdr[128];
        snprintf(hdr, sizeof hdr,
                 "\x1b[2K virtual led strip  \x1b[2m%d LEDs \xc2\xb7 pin %d \xc2\xb7 "
                 "%.0f fps\x1b[0m\n\n  ",
                 count, pin, fps);
        out += hdr;

        for (int i = 0; i < count; i++)
        {
            const uint8_t* p = &work_[i * 3];
            char cell[40];
            // two background-colored spaces per LED — a solid block of its color
            snprintf(cell, sizeof cell, "\x1b[48;2;%d;%d;%dm  ", p[0], p[1], p[2]);
            out += cell;
        }

        out += "\x1b[0m\x1b[K\n";

        fputs(out.c_str(), stdout);
        fflush(stdout);
        drew = true;

        lastShown_ = work_;
        lastAnim_ = anim;
        haveLast_ = true;
    }

private:
    fade::Fader fader_;
    std::vector<uint8_t> work_;     // incoming frame, blended in place
    std::vector<uint8_t> lastShown_; // the frame currently on screen
    uint16_t lastAnim_ = 0;
    bool haveLast_ = false;
};

// overwrite just the header line with a status note, leaving the last drawn
// frame on screen below it
static void status(const char* msg)
{
    printf("\x1b[H\x1b[2K virtual led strip  \x1b[2m%s\x1b[0m\n", msg);
    fflush(stdout);
}

int main(int argc, char** argv)
{
    g_sockPath = (argc > 1) ? argv[1] : SOCK_DEFAULT;

    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);

    if (fd < 0)
    {
        perror("socket");
        return 1;
    }

    sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, g_sockPath.c_str(), sizeof addr.sun_path - 1);

    unlink(g_sockPath.c_str()); // clear a stale socket left by a crash

    if (bind(fd, (const sockaddr*)&addr, sizeof addr) < 0)
    {
        fprintf(stderr, "bind %s: %s\n", g_sockPath.c_str(), strerror(errno));
        return 1;
    }

    // wake up even with no traffic, so we can show an idle status and notice
    // when the daemon stops sending
    timeval tv{1, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    signal(SIGINT, onStop);
    signal(SIGTERM, onStop);
    atexit(restoreTerminal);

    printf("\x1b[2J\x1b[?25l"); // clear screen, hide cursor
    status("waiting for frames\xe2\x80\xa6  start `led`, or run an effect");

    std::vector<uint8_t> buf(proto::PIX_HEADER + 2048 * 3 + 1); // max ESP32 frame

    // each datagram is one whole wire frame, but feed it through the shared
    // streaming parser anyway: same framing/checksum as the daemon and ESP32,
    // with no duplicated decode here
    Viewer viewer;
    proto::Receiver receiver(viewer);

    double winStart = nowSeconds();
    int frames = 0;
    bool everDrew = false;

    while (!g_stop)
    {
        ssize_t n = recv(fd, buf.data(), buf.size(), 0);
        double now = nowSeconds();

        if (n > 0)
        {
            viewer.drew = false;
            receiver.feed(buf.data(), (size_t)n);

            if (viewer.drew)
            {
                frames++;
                everDrew = true;
            }

            if (now - winStart >= 0.5)
            {
                viewer.fps = frames / (now - winStart);
                frames = 0;
                winStart = now;
            }
        }
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            // no frame for a second: reset the rate and note we've gone idle
            viewer.fps = 0;
            frames = 0;
            winStart = now;
            status(everDrew ? "idle \xe2\x80\x94 no frames (strip blanked or daemon stopped)"
                            : "waiting for frames\xe2\x80\xa6  start `led`, or run an effect");
        }
        // other errors (e.g. EINTR from our signal) just re-check g_stop
    }

    return 0;
}
