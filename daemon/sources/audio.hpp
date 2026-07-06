#pragma once

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string>

// System audio playback detection and capture, shared by the `audio`
// rule condition and the audio_* effects.
//
// Detection reads /proc/asound (a few procfs lines per rule tick, no
// deps): whenever anything is actually feeding the sound card, ALSA
// marks a playback substream RUNNING. PipeWire suspends idle devices a
// few seconds after the last stream stops, so the flag drops shortly
// after silence — a natural hold.
//
// One trap: our own monitor capture counts as a client of the sink, so
// while it runs PipeWire never sees the sink as idle and the RUNNING
// flag never drops (the "pavucontrol keeps devices awake" effect) —
// detection alone would latch true forever once an audio effect is up.
// While the capture is live we can hear the truth directly instead:
// sustained digital silence on the monitor means playback stopped, and
// Levels::hearsSignal() reports exactly that to the rule condition.
//
// Capture can't be that cheap: samples only exist inside the user's
// PipeWire/PulseAudio session, so we spawn its standard capture client
// (`parec`, falling back to `pw-record`) on the default sink monitor
// and read raw s16 mono off the pipe. The daemon runs as a root system
// service with no session env, so the child is pointed at the first
// /run/user/*/pulse socket found (pipewire-pulse doesn't do cookie
// auth, and root can traverse the runtime dir). No client → the effect
// still runs, it just sees silence.
namespace audio
{

// true while any ALSA playback substream is RUNNING
inline bool playing()
{
    DIR* cards = opendir("/proc/asound");
    if (!cards) return false;

    bool found = false;

    while (!found)
    {
        dirent* c = readdir(cards);
        if (!c) break;

        if (strncmp(c->d_name, "card", 4) != 0 || !isdigit(c->d_name[4]))
            continue;

        std::string base = std::string("/proc/asound/") + c->d_name;

        DIR* card = opendir(base.c_str());
        if (!card) continue;

        while (!found)
        {
            dirent* p = readdir(card);
            if (!p) break;

            // pcm<N>p is a playback stream (pcm<N>c would be capture)
            size_t len = strlen(p->d_name);

            if (strncmp(p->d_name, "pcm", 3) != 0 || p->d_name[len - 1] != 'p')
                continue;

            std::string pcm = base + "/" + p->d_name;

            DIR* sub = opendir(pcm.c_str());
            if (!sub) continue;

            while (!found)
            {
                dirent* s = readdir(sub);
                if (!s) break;

                if (strncmp(s->d_name, "sub", 3) != 0)
                    continue;

                std::string status = pcm + "/" + s->d_name + "/status";

                FILE* f = fopen(status.c_str(), "r");
                if (!f) continue;

                char line[128];

                while (fgets(line, sizeof line, f))
                {
                    if (strncmp(line, "state:", 6) == 0)
                    {
                        found = strstr(line, "RUNNING") != nullptr;
                        break;
                    }
                }

                fclose(f);
            }

            closedir(sub);
        }

        closedir(card);
    }

    closedir(cards);
    return found;
}

// absolute path of an executable on PATH, or ""
inline std::string findInPath(const char* name)
{
    const char* path = getenv("PATH");
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";

    std::string dirs = path;
    size_t start = 0;

    while (start <= dirs.size())
    {
        size_t end = dirs.find(':', start);
        if (end == std::string::npos) end = dirs.size();

        if (end > start)
        {
            std::string full = dirs.substr(start, end - start) + "/" + name;

            if (access(full.c_str(), X_OK) == 0)
                return full;
        }

        start = end + 1;
    }

    return "";
}

// locate a user audio session for the capture child. A session env of
// our own (running by hand on a desktop) wins; otherwise scan
// /run/user/* for a pulse socket — on the BC-250 that's the gamer
// user's pipewire-pulse.
inline void findUserSession(std::string& server, std::string& runtimeDir)
{
    if (getenv("PULSE_SERVER") || getenv("XDG_RUNTIME_DIR"))
        return;

    DIR* dir = opendir("/run/user");
    if (!dir) return;

    while (dirent* e = readdir(dir))
    {
        if (!isdigit(e->d_name[0]))
            continue;

        std::string rd = std::string("/run/user/") + e->d_name;
        std::string sock = rd + "/pulse/native";

        struct stat st;

        if (stat(sock.c_str(), &st) == 0)
        {
            runtimeDir = rd;
            server = "unix:" + sock;
            break;
        }
    }

    closedir(dir);
}

// the default sink's monitor as raw s16le mono samples, via a spawned
// parec/pw-record child read non-blocking. Dies (EOF) when the session
// goes away; the owner just calls start() again later.
class Capture
{
public:
    static constexpr int RATE = 44100;

    ~Capture() { stop(); }

    bool running() const { return fd >= 0; }

    bool start()
    {
        if (fd >= 0)
            return true;

        std::string parec = findInPath("parec");
        std::string pwrec = parec.empty() ? findInPath("pw-record") : "";

        if (parec.empty() && pwrec.empty())
        {
            if (!warned)
                fprintf(stderr, "audio capture: neither parec nor "
                                "pw-record found; audio effects will "
                                "show silence\n");

            warned = true;
            return false;
        }

        std::string server, runtimeDir;
        findUserSession(server, runtimeDir);

        int p[2];

        if (pipe(p) != 0)
            return false;

        pid = fork();

        if (pid < 0)
        {
            close(p[0]);
            close(p[1]);
            pid = -1;
            return false;
        }

        if (pid == 0)
        {
            dup2(p[1], 1);
            close(p[0]);
            close(p[1]);

            int null = open("/dev/null", O_RDWR);

            if (null >= 0)
            {
                dup2(null, 0);
                dup2(null, 2);
                close(null);
            }

            if (!server.empty())
                setenv("PULSE_SERVER", server.c_str(), 1);

            if (!runtimeDir.empty())
                setenv("XDG_RUNTIME_DIR", runtimeDir.c_str(), 1);

            // the "44100"s below are RATE; execl wants string literals
            if (!parec.empty())
                execl(parec.c_str(), "parec", "--raw", "--format=s16le",
                      "--rate=44100", "--channels=1", "--latency-msec=30",
                      "-d", "@DEFAULT_MONITOR@", (char*)nullptr);
            else
                execl(pwrec.c_str(), "pw-record", "--raw", "--format", "s16",
                      "--rate", "44100", "--channels", "1",
                      "-P", "stream.capture.sink=true", "-", (char*)nullptr);

            _exit(127);
        }

        close(p[1]);
        fcntl(p[0], F_SETFL, O_NONBLOCK);
        fd = p[0];
        return true;
    }

    void stop()
    {
        if (pid > 0)
        {
            kill(pid, SIGTERM);
            waitpid(pid, nullptr, 0);
            pid = -1;
        }

        if (fd >= 0)
        {
            close(fd);
            fd = -1;
        }

        partialLen = 0;
    }

    // drain whatever the child has produced into out, up to maxSamples;
    // 0 when nothing new yet, -1 when not running. A read can split a
    // sample across calls, so a dangling byte is carried to the next.
    int read(int16_t* out, int maxSamples)
    {
        if (fd < 0)
            return -1;

        uint8_t* buf = (uint8_t*)out;
        size_t have = 0;
        size_t want = (size_t)maxSamples * 2;

        if (partialLen)
        {
            buf[0] = partial;
            have = 1;
            partialLen = 0;
        }

        while (have < want)
        {
            ssize_t n = ::read(fd, buf + have, want - have);

            if (n > 0)
            {
                have += (size_t)n;
                continue;
            }

            if (n < 0 && errno == EINTR)
                continue;

            if (n == 0) // child exited; reap it, owner may retry start()
            {
                stop();
                break;
            }

            break; // EAGAIN: drained
        }

        // don't carry a byte across a dead child into the next session
        if ((have & 1) && fd >= 0)
        {
            partial = buf[have - 1];
            partialLen = 1;
            have--;
        }

        return (int)(have / 2);
    }

private:
    pid_t pid = -1;
    int fd = -1;
    uint8_t partial = 0;
    int partialLen = 0;
    bool warned = false;
};

// Shared, self-normalising levels for the audio_* effects: one
// Capture and one analysis behind a refcount. Effects acquire() in
// init() and release() in their destructor, and whichever is active
// calls update() once per frame — only one effect renders at a time
// (a cycle runs a single child), so there is exactly one updater.
//
// The envelope and auto-gain state deliberately outlives the capture:
// when a cycle hops between music effects the stream respawns, but
// "how loud is loud" carries over, so the next effect opens correctly
// driven instead of re-learning the volume from scratch.
//
// Bands are one-pole splits: bass is everything under ~160 Hz, treble
// everything over ~2.4 kHz, mid the remainder. Each is an RMS envelope
// (fast attack, slower release) normalised against its own slowly
// decaying running peak, so all read 0..1 at any system volume.
class Levels
{
public:
    static Levels& shared()
    {
        static Levels s;
        return s;
    }

    // analysis tuning; the most recently activated effect's settings win
    void configure(float attackS, float releaseS, float gainS)
    {
        attackSec = attackS > 0.001f ? attackS : 0.001f;
        releaseSec = releaseS > 0.001f ? releaseS : 0.001f;
        gainSeconds = gainS > 0.1f ? gainS : 0.1f;
    }

    void acquire()
    {
        if (users++ == 0)
        {
            cap.start();
            retryAt = clk;      // an immediate respawn attempt is fine
            lastData = clk;     // don't read the inactive gap as starvation
            lastSignal = clk;   // fresh capture starts with a grace period
        }
    }

    void release()
    {
        if (users > 0 && --users == 0)
            cap.stop();
    }

    void update(float dt)
    {
        clk += dt;

        // the capture child dies with the user session; keep trying to
        // bring it back while a music effect is up
        if (!cap.running() && clk >= retryAt)
        {
            if (cap.start())
                lastSignal = clk;

            retryAt = clk + 2.0f;
        }

        measure(dt);

        // envelopes chase the measured levels: fast up so hits land,
        // slower down so the light falls away instead of snapping
        float aUp = 1.0f - expf(-dt / attackSec);
        float aDn = 1.0f - expf(-dt / releaseSec);

        lev += (rmsT > lev ? aUp : aDn) * (rmsT - lev);
        bassE += (bassT > bassE ? aUp : aDn) * (bassT - bassE);
        midE += (midT > midE ? aUp : aDn) * (midT - midE);
        trebE += (trebT > trebE ? aUp : aDn) * (trebT - trebE);

        if (lev > 0.06f)
            lastLoud = clk;
    }

    float level() const { return lev; }
    float bass() const { return bassE; }
    float mid() const { return midE; }
    float treble() const { return trebE; }

    // seconds since the audio was last audibly loud; effects use this
    // to swell into their idle wash during in-condition silence
    float quietSeconds() const { return clk - lastLoud; }

    // false once a live capture has heard nothing but digital silence
    // for holdSec: the capture itself keeps the sink's RUNNING flag up
    // (see the detection note at the top), so the monitor going flat is
    // the only sign that playback actually stopped. With no capture the
    // flag is trustworthy and this stays true.
    bool hearsSignal(float holdSec) const
    {
        return !cap.running() || clk - lastSignal < holdSec;
    }

private:
    Levels()
    {
        kBass = 1.0f - expf(-2.0f * 3.14159265f * 160.0f / Capture::RATE);
        kTreb = 1.0f - expf(-2.0f * 3.14159265f * 2400.0f / Capture::RATE);
    }

    // drain the capture pipe and refresh the level targets: overall,
    // bass, mid and treble RMS of the new samples, each normalised
    // against its own running peak (auto-gain)
    void measure(float dt)
    {
        int16_t buf[2048];
        double sq = 0, bsq = 0, msq = 0, tsq = 0;
        int total = 0;
        bool signal = false;

        // bounded drain: a stall's worth of backlog, not a whole pipe
        while (total < 16384)
        {
            int n = cap.read(buf, 2048);

            if (n <= 0)
                break;

            for (int i = 0; i < n; i++)
            {
                // above ±1 LSB of dither counts as someone playing
                if (buf[i] > 3 || buf[i] < -3)
                    signal = true;

                float s = buf[i] / 32768.0f;

                lpBass += kBass * (s - lpBass);
                lpTreb += kTreb * (s - lpTreb);

                float m = lpTreb - lpBass; // the ~160 Hz .. ~2.4 kHz band
                float hp = s - lpTreb;

                sq += s * s;
                bsq += lpBass * lpBass;
                msq += m * m;
                tsq += hp * hp;
            }

            total += n;

            if (n < 2048) // drained
                break;
        }

        // the client delivers ~30 ms chunks against ~16 ms frames, so
        // empty frames are normal: hold the last measurement briefly,
        // and only read sustained starvation as silence
        if (total == 0)
        {
            if (clk - lastData > 0.25f)
                rmsT = bassT = midT = trebT = 0;

            return;
        }

        lastData = clk;

        if (signal)
            lastSignal = clk;

        float decay = expf(-dt / gainSeconds);

        float rms = (float)sqrt(sq / total);
        float bas = (float)sqrt(bsq / total);
        float mid = (float)sqrt(msq / total);
        float trb = (float)sqrt(tsq / total);

        // the peaks rise instantly and sag slowly; the floor keeps
        // silence/noise from being amplified into a full strip
        rmsPeak = fmaxf(fmaxf(rms, rmsPeak * decay), 0.02f);
        bassPeak = fmaxf(fmaxf(bas, bassPeak * decay), 0.02f);
        midPeak = fmaxf(fmaxf(mid, midPeak * decay), 0.02f);
        trebPeak = fmaxf(fmaxf(trb, trebPeak * decay), 0.02f);

        rmsT = fminf(rms / (rmsPeak * 1.05f), 1.0f);
        bassT = fminf(bas / (bassPeak * 1.05f), 1.0f);
        midT = fminf(mid / (midPeak * 1.05f), 1.0f);
        trebT = fminf(trb / (trebPeak * 1.05f), 1.0f);
    }

    Capture cap;
    int users = 0;
    float clk = 0;
    float retryAt = 0;
    float attackSec = 0.035f;
    float releaseSec = 0.3f;
    float gainSeconds = 6.0f;
    float kBass = 0.02f, kTreb = 0.3f;
    float lpBass = 0, lpTreb = 0;
    float rmsT = 0, bassT = 0, midT = 0, trebT = 0;
    float rmsPeak = 0.02f, bassPeak = 0.02f, midPeak = 0.02f,
          trebPeak = 0.02f;
    float lev = 0, bassE = 0, midE = 0, trebE = 0;
    float lastData = 0;
    float lastLoud = 0;
    float lastSignal = 0;
};

} // namespace audio
