#pragma once

#include <ctype.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>

// The BC-250's eight GDDR6 chips report their own temperatures, but nothing
// in the kernel reads them: the value lives behind the SMU (the GPU's system
// management microcontroller), which has no stock command that returns it.
// BC250-Telemetry (github.com/onlinermm/BC250-Telemetry, MIT), building on
// pan-Rijovich/bc250-memory-temperature (MIT), works around that by uploading
// a small program into the SMU's SRAM and pointing an unused command slot at
// it; that program asks the memory controller for each chip's temperature.
// PAYLOAD, its addresses and the command protocol below are theirs.
//
// This is a real patch of a live microcontroller, so it is off unless the
// config turns it on (README "VRAM temperatures"): without enable() nothing
// here ever opens the SMU. Two more things guard it. First, the patch needs
// the SMU's secure-access gate already open, which the daemon does NOT do
// itself — it is the risky half, an exploit, and the reader refuses to run
// unless something else (a patched BIOS such as RescueMei's DXEv3 build) has
// opened it. Second, the platform is checked: only a BC-250 on stock P3.0
// firmware, whose SMU byte-for-byte matches what PAYLOAD was built against.
//
// Once patched, one poller thread reads the eight chips every few seconds
// while anything asks (the same want()/WANT_S idle-out as pmbus.hpp), and the
// daemon takes the latest values. A chip's code is JEDEC 0..80 → -40..120 °C;
// 80 is the sensor's ceiling and means "at least 120". The hotspot and the
// average are computed from the chips that read.
namespace smu
{
// the top-level config key that opts in: the SMU is patched only when this is
// true (README "VRAM temperatures"). The module owns the name.
inline const char* CONFIG_KEY = "vram_temps";

// the SMU is reached through the root complex's PCI config space: a register
// address is written to one dword, the data read or written at the next
inline const char* PCI_DEVICE = "/sys/bus/pci/devices/0000:00:00.0/config";
inline const int PCI_REG = 0xB8; // the SMN address window
inline const int PCI_DATA = 0xBC; // the data dword

// the two command mailboxes used here (cmd / response / first-arg dwords),
// as BC250-Telemetry established them
struct Mailbox
{
    uint32_t cmd, rsp, arg;
    int argCount;
};
inline const Mailbox Q2 = {0x03B10528, 0x03B10564, 0x03B10998, 6};
inline const Mailbox Q3 = {0x03B10A20, 0x03B10A80, 0x03B10A88, 1};

// a response dword is one of these once the SMU has finished a command
inline const uint32_t RET_OK = 0x01;
inline const uint32_t RET_FAILED = 0xFF;
inline const uint32_t RET_UNKNOWN_CMD = 0xFE;
inline const uint32_t RET_REJECTED_PREREQ = 0xFD;
inline const uint32_t RET_REJECTED_BUSY = 0xFC;
inline bool done(uint32_t s)
{
    return s == RET_OK || s == RET_FAILED || s == RET_UNKNOWN_CMD ||
           s == RET_REJECTED_PREREQ || s == RET_REJECTED_BUSY;
}

// Q3 messages
inline const uint32_t MSG_TEST = 0x01;      // liveness probe
inline const uint32_t MSG_READ_CHIP = 0x05; // the uploaded handler: one chip's temp code
inline const uint32_t MSG_SEC_WRITE_PTR = 0x28;
inline const uint32_t MSG_SEC_WRITE_THROUGH = 0x29;
inline const uint32_t MSG_SEC_SMN_READ = 0x2A;
// Q2 message
inline const uint32_t MSG_TRANSFER = 0x0A;
inline const uint32_t XFER_SRAM_LOAD = 0x1F; // stage SMU SRAM for a DMA out
inline const uint32_t XFER_SMU_TO_DRAM = 0x14;

inline const uint32_t TEST_TOKEN = 0x5EED0000; // MSG_TEST echoes this + 1
inline const uint32_t SECURE_PROBE_ADDR = 0x0005A870;

// where PAYLOAD lives in SMU SRAM, the command-slot register that dispatches
// MSG_READ_CHIP, and the handler entry the slot must point at once installed
inline const uint32_t PAYLOAD_ADDR = 0x3AA9C;
inline const uint32_t PAYLOAD_END = 0x3E000;
inline const uint32_t HANDLER_REG = 0x748C;
inline const uint32_t HANDLER = 0x3AAC4;
inline const uint32_t SRAM_LIMIT = 0x40000;

// The program uploaded into SMU SRAM (176 bytes, 44 little-endian dwords).
// Unmodified from BC250-Telemetry's payload/SMUPayload.bin, whose SHA-256 is
// b31908460e932a615d9eafb6b3112e6448994f9f6fa656d1d80a8616ac1df4df; that
// project pins it to stock P3.0 firmware and this reader will not run on
// anything else.
inline const uint32_t PAYLOAD[] = {
    0x00053A24, 0x00053A1C, 0x00053A20, 0x00053A2C, 0x00001234, 0x00000FFC,
    0x00002A74, 0x000029F8, 0x00000FE4, 0x00000FA8, 0x20004136, 0xF98120A2,
    0x0008E0FF, 0xC0FFF3B1, 0x2D0C014A, 0xB4B00DCD, 0x810A0C20, 0x08E0FFF5,
    0xFFEEB100, 0xA0C22D0C, 0x20B4B005, 0xF0810A0C, 0x0008E0FF, 0x30FFEB31,
    0x2C0C2034, 0xA220B330, 0xEC8100A0, 0x0008E0FF, 0x31EE5A66, 0x3430FFE6,
    0xBD2C0C20, 0x810A0C03, 0x08E0FFE7, 0xFFE24100, 0x0CED1A47, 0x20B3302C,
    0xE2810A0C, 0x0008E0FF, 0x02AD0ABD, 0xE0FFE181, 0x1B0C0008, 0xDF8102AD,
    0x0008E0FF, 0x0000F01D,
};
inline const int PAYLOAD_WORDS = (int)(sizeof(PAYLOAD) / sizeof(PAYLOAD[0]));

inline const int CHIP_COUNT = 8;
inline const int CODE_MAX = 80; // JEDEC ceiling: code 80 = 120 °C, saturated

// -1 unless a code is in range; 80 saturates at 120 °C
inline float codeToC(uint32_t code)
{
    return code <= (uint32_t)CODE_MAX ? (float)code * 2 - 40 : -1;
}

// the sources this exposes and the label each carries in a "smu:LABEL" spec.
// The eight chips, plus the hottest and the mean of whatever read.
enum Kind { Hotspot, Average, Chip0 };
struct Source
{
    const char* label;
    int kind; // Hotspot, Average, or Chip0 + n
};
inline const Source SOURCES[] = {
    {"VRAM hotspot", Hotspot}, {"VRAM average", Average},
    {"VRAM chip 0", Chip0 + 0}, {"VRAM chip 1", Chip0 + 1},
    {"VRAM chip 2", Chip0 + 2}, {"VRAM chip 3", Chip0 + 3},
    {"VRAM chip 4", Chip0 + 4}, {"VRAM chip 5", Chip0 + 5},
    {"VRAM chip 6", Chip0 + 6}, {"VRAM chip 7", Chip0 + 7},
};
inline const int SOURCE_COUNT = (int)(sizeof(SOURCES) / sizeof(SOURCES[0]));

// the source a label names, or -1
inline int sourceOf(const std::string& label)
{
    for (int i = 0; i < SOURCE_COUNT; i++)
        if (label == SOURCES[i].label)
            return i;
    return -1;
}

// the bus to the SMU: one open PCI config fd, register reads and writes
// through its window. Not thread-safe on its own; the Reader serializes it.
class Bus
{
public:
    bool open()
    {
        if (fd_ >= 0)
            return true;
        fd_ = ::open(PCI_DEVICE, O_RDWR | O_CLOEXEC);
        return fd_ >= 0;
    }
    void close()
    {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = -1;
    }
    ~Bus() { close(); }

    bool writeReg(uint32_t reg, uint32_t value)
    {
        return dword(reg, PCI_REG) && dword(value, PCI_DATA);
    }
    // false on an I/O error; *value holds the dword otherwise
    bool readReg(uint32_t reg, uint32_t* value)
    {
        if (!dword(reg, PCI_REG))
            return false;
        uint32_t v = 0;
        if (pread(fd_, &v, 4, PCI_DATA) != 4)
            return false;
        *value = v;
        return true;
    }

private:
    bool dword(uint32_t v, off_t off) { return pwrite(fd_, &v, 4, off) == 4; }
    int fd_ = -1;
};

class Reader
{
public:
    static Reader& get()
    {
        static Reader* r = new Reader; // never destroyed (detached poller)
        return *r;
    }

    // the config opts in. Idempotent. The poller itself starts lazily on the
    // first ask (want()), so its first pass runs with a fresh wantedAt_ rather
    // than idling until the next tick.
    void enable() { enabled_.store(true); }
    bool enabled() const { return enabled_.load(); }

    // the latest temperature of a source (SOURCES index), false when it has
    // none — the feature is off, the patch never took, or that chip's code
    // was out of range. Asking keeps the poller going.
    bool temp(int source, float& v)
    {
        want();
        if (source < 0 || source >= SOURCE_COUNT)
            return false;
        std::lock_guard<std::mutex> g(m_);
        return value(SOURCES[source].kind, v);
    }

    // the SMU is patched and reading. Waits (bounded) for the first attempt so
    // a one-shot caller such as --fan-status sees a settled answer.
    bool present()
    {
        if (!enabled_.load())
            return false;
        want();
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait_for(lk, std::chrono::seconds(2), [&] { return tried_; });
        return patched_;
    }

    // once the first attempt has finished, why it did not patch (""
    // while it did, or has not yet been tried)
    std::string status()
    {
        std::lock_guard<std::mutex> g(m_);
        return patched_ ? "" : status_;
    }

private:
    static constexpr double WANT_S = 15;    // poll while asked within this long
    static constexpr int POLL_MS = 3000;    // between chip sweeps
    static constexpr int RETRY_MS = 30000;  // between patch attempts while it fails
    static constexpr int MAILBOX_MS = 1000; // a command's own timeout — generous
                                            // (a healthy SMU answers in ms; this
                                            // only bites a wedged one, and the
                                            // poller it blocks is detached)
    static constexpr int LOST_AFTER = 5;    // failed sweeps in a row = give up until re-asked

    Reader() = default;

    static double mono()
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec + ts.tv_nsec / 1e9;
    }

    // record interest and, while enabled, start the poller on the first ask
    void want()
    {
        wantedAt_.store(mono());
        if (enabled_.load() && !started_.exchange(true))
            std::thread(&Reader::loop, this).detach();
    }

    // a kind's current value from the per-chip cache under m_
    bool value(int kind, float& v)
    {
        if (kind >= Chip0)
        {
            int c = kind - Chip0;
            if (!chipOk_[c])
                return false;
            v = chipC_[c];
            return true;
        }
        float sum = 0, hot = -1000;
        int n = 0;
        for (int i = 0; i < CHIP_COUNT; i++)
            if (chipOk_[i])
            {
                sum += chipC_[i];
                hot = std::max(hot, chipC_[i]);
                n++;
            }
        if (n == 0)
            return false;
        v = kind == Hotspot ? hot : sum / n;
        return true;
    }

    void loop()
    {
        double lastTry = -1e9;
        for (;;)
        {
            double now = mono();
            if (now - wantedAt_.load() < WANT_S)
            {
                if (!patched_)
                {
                    // patch() reads the chips once itself, so no sweep follows
                    if (now - lastTry >= RETRY_MS / 1000.0)
                    {
                        lastTry = now;
                        patch();
                    }
                }
                else
                    sweep();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
        }
    }

    // read all eight chips through the installed handler into local arrays;
    // returns false if any chip's mailbox failed (a lost SMU, distinct from a
    // chip whose code is merely out of range). A code is a temperature only in
    // the daemon's usual plausible range (> 0 °C); 80 saturates at 120.
    bool readChips(float* c, bool* ok)
    {
        bool allOk = true;
        for (int i = 0; i < CHIP_COUNT; i++)
        {
            uint32_t code = 0, ret = 0;
            if (message(Q3, MSG_READ_CHIP, {(uint32_t)i}, &ret, &code) && ret == RET_OK)
            {
                float t = codeToC(code);
                ok[i] = t > 0;
                c[i] = ok[i] ? t : 0;
            }
            else
            {
                ok[i] = false;
                c[i] = 0;
                allOk = false;
            }
        }
        return allOk;
    }

    void storeChips(const float* c, const bool* ok)
    {
        std::lock_guard<std::mutex> g(m_);
        for (int i = 0; i < CHIP_COUNT; i++)
        {
            chipOk_[i] = ok[i];
            chipC_[i] = c[i];
        }
    }

    // one periodic read; drop the patch after too many failed sweeps so the
    // next ask re-patches (a lost SMU is not silently trusted)
    void sweep()
    {
        float c[CHIP_COUNT];
        bool ok[CHIP_COUNT];
        bool allOk = readChips(c, ok);
        storeChips(c, ok);
        fails_ = allOk ? 0 : fails_ + 1;
        if (fails_ >= LOST_AFTER)
        {
            fprintf(stderr, "smu: the SMU stopped answering — will re-patch when next asked\n");
            lastLogged_ = "\x01"; // let the re-patch outcome log afresh
            drop("the SMU stopped answering");
        }
    }

    // platform-gate, open the bus, install PAYLOAD if it is not already there.
    // Sets patched_ on success; records why in status_ otherwise.
    void patch()
    {
        std::string why = tryPatch();
        bool ok = why.empty();
        // read the chips once before announcing readiness, so the first
        // present() that unblocks already has values — no spurious "no
        // reading" fallback on the tick right after the patch
        float c[CHIP_COUNT];
        bool chok[CHIP_COUNT] = {};
        if (ok)
        {
            readChips(c, chok);
            storeChips(c, chok);
            fails_ = 0;
        }
        {
            std::lock_guard<std::mutex> g(m_);
            patched_ = ok;
            status_ = why;
            tried_ = true;
            if (!ok)
                for (int i = 0; i < CHIP_COUNT; i++)
                    chipOk_[i] = false;
            cv_.notify_all();
        }
        // say what happened once per distinct outcome, so the journal explains
        // a locked SMU or the wrong board without repeating it every retry
        if (why != lastLogged_)
        {
            lastLogged_ = why;
            if (why.empty())
                fprintf(stderr, "smu: GDDR6 temperature handler installed\n");
            else
                fprintf(stderr, "smu: VRAM temperatures unavailable — %s\n", why.c_str());
        }
    }

    std::string tryPatch()
    {
        std::string why = checkPlatform();
        if (!why.empty())
            return why;
        if (!bus_.open())
            return std::string("cannot open ") + PCI_DEVICE + " (needs root)";

        uint32_t token = 0;
        if (!message(Q3, MSG_TEST, {TEST_TOKEN}, nullptr, &token) || token != TEST_TOKEN + 1)
            return "the SMU did not answer its liveness probe";

        // the secure-access gate must already be open — the daemon never opens
        // it (that exploit belongs to the BIOS). Closed = unsupported here.
        uint32_t ret = 0;
        if (!message(Q3, MSG_SEC_SMN_READ, {SECURE_PROBE_ADDR}, &ret, nullptr))
            return "the SMU secure-access probe failed";
        if (ret == RET_REJECTED_PREREQ)
            return "the SMU is locked — a BIOS that opens SMU secure access is required "
                   "(RescueMei DXEv3 or equivalent); this daemon will not run the unlock itself";
        if (ret != RET_OK)
            return "the SMU secure-access probe was rejected";

        // already installed? verify the payload byte-for-byte before trusting it
        uint32_t cur = 0;
        if (!sramRead(HANDLER_REG, &cur, 1))
            return "could not read the SMU handler slot";
        if (cur == HANDLER)
            return payloadMatches() ? "" : "the handler slot points at an unexpected payload; reboot first";

        // an empty slot is fine to fill; a stray nonzero one is not ours to move
        bool sane = cur == 0 || ((cur >= 0x800 && cur < 0x40000) &&
                                 !(cur >= PAYLOAD_ADDR && cur < PAYLOAD_END));
        if (!sane)
        {
            char b[80];
            snprintf(b, sizeof b, "unexpected existing handler 0x%08X; reboot before retrying", cur);
            return b;
        }

        for (int i = 0; i < PAYLOAD_WORDS; i++)
            if (!sramWrite(PAYLOAD_ADDR + i * 4, PAYLOAD[i]))
                return "writing the payload failed";
        if (!payloadMatches())
            return "the payload did not read back — handler not switched";
        if (!sramWrite(HANDLER_REG, HANDLER))
            return "switching the handler failed";
        uint32_t back = 0;
        if (!sramRead(HANDLER_REG, &back, 1) || back != HANDLER)
            return "the handler did not read back";
        return "";
    }

    bool payloadMatches()
    {
        uint32_t got[PAYLOAD_WORDS];
        if (!sramRead(PAYLOAD_ADDR, got, PAYLOAD_WORDS))
            return false;
        return memcmp(got, PAYLOAD, sizeof PAYLOAD) == 0;
    }

    // the board must be a BC-250 on the stock P3.0 firmware PAYLOAD was built
    // against — a coarse gate (identical strings on custom firmware still
    // pass), which is why the payload readback is verified regardless
    std::string checkPlatform()
    {
        std::string board = trim(readLine("/sys/class/dmi/id/board_name"));
        std::string bios = trim(readLine("/sys/class/dmi/id/bios_version"));
        std::string id = board;
        id.erase(std::remove_if(id.begin(), id.end(),
                                [](char c) { return c == '-' || c == ' '; }),
                 id.end());
        for (auto& c : id)
            c = (char)toupper((unsigned char)c);
        std::string biosU = bios;
        for (auto& c : biosU)
            c = (char)toupper((unsigned char)c);
        if (id != "BC250" && id != "AMDBC250")
            return "not a BC-250 (board \"" + board + "\") — VRAM temperatures are BC-250 only";
        if (biosU != "P3.0" && biosU != "P3.00")
            return "BIOS \"" + bios + "\" is not P3.0 — the SMU payload is pinned to that firmware";
        return "";
    }

    // one mailbox command: write args, write the command, wait for a done
    // response. false on I/O error or timeout. *ret gets the response dword,
    // *arg0 the first argument dword after completion (either may be null).
    bool message(const Mailbox& mb, uint32_t msg, std::initializer_list<uint32_t> args,
                 uint32_t* ret, uint32_t* arg0)
    {
        if ((int)args.size() > mb.argCount)
            return false;
        if (!bus_.writeReg(mb.rsp, 0))
            return false;
        for (int i = 0; i < mb.argCount; i++)
            if (!bus_.writeReg(mb.arg + 4 * i, i < (int)args.size() ? args.begin()[i] : 0))
                return false;
        if (!bus_.writeReg(mb.cmd, msg))
            return false;
        double deadline = mono() + MAILBOX_MS / 1000.0;
        for (;;)
        {
            uint32_t status = 0;
            if (!bus_.readReg(mb.rsp, &status))
                return false;
            if (done(status))
            {
                if (ret)
                    *ret = status;
                if (arg0 && !bus_.readReg(mb.arg, arg0))
                    return false;
                return true;
            }
            if (mono() >= deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // write one dword to SMU-local SRAM through the (already open) secure gate
    bool sramWrite(uint32_t addr, uint32_t value)
    {
        if (addr % 4 || addr + 4 > SRAM_LIMIT)
            return false;
        uint32_t r1 = 0, r2 = 0;
        return message(Q3, MSG_SEC_WRITE_PTR, {addr}, &r1, nullptr) && r1 == RET_OK &&
               message(Q3, MSG_SEC_WRITE_THROUGH, {value}, &r2, nullptr) && r2 == RET_OK;
    }

    // read n dwords (n up to 18 per transfer) from SMU SRAM into out, staged
    // into SMU memory then DMA'd to a locked host page. Two differently
    // pre-filled reads must agree, so a DMA that never wrote the page is not
    // mistaken for its contents.
    bool sramRead(uint32_t addr, uint32_t* out, int n)
    {
        for (int off = 0; off < n;)
        {
            int chunk = std::min(18, n - off);
            if (!sramReadChunk(addr + off * 4, out + off, chunk))
                return false;
            off += chunk;
        }
        return true;
    }

    bool sramReadChunk(uint32_t addr, uint32_t* out, int n)
    {
        if (n < 1 || n > 18 || addr % 4 || addr + n * 4 > SRAM_LIMIT)
            return false;
        long pageSize = sysconf(_SC_PAGE_SIZE);
        void* page = mmap(nullptr, pageSize, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED)
            return false;
        bool ok = false;
        if (mlock(page, pageSize) == 0)
        {
            uint64_t phys = 0;
            if (physAddr(page, &phys))
            {
                uint32_t r = 0;
                // stage the SRAM for the transfer engine
                if (message(Q2, MSG_TRANSFER, {XFER_SRAM_LOAD, 0, addr, (uint32_t)n}, &r, nullptr) &&
                    r == RET_OK)
                {
                    uint8_t got[2][18 * 4];
                    ok = true;
                    for (int pass = 0; pass < 2 && ok; pass++)
                    {
                        memset(page, pass ? 0x5A : 0xA5, n * 4);
                        uint32_t hi = (uint32_t)(phys >> 32), lo = (uint32_t)(phys & 0xFFFFFFFF);
                        if (message(Q2, MSG_TRANSFER, {XFER_SMU_TO_DRAM, hi, lo, (uint32_t)n, 0, 0},
                                    &r, nullptr) &&
                            r == RET_OK)
                            memcpy(got[pass], page, n * 4);
                        else
                            ok = false;
                    }
                    if (ok && memcmp(got[0], got[1], n * 4) == 0)
                        memcpy(out, got[0], n * 4);
                    else
                        ok = false;
                }
            }
            munlock(page, pageSize);
        }
        munmap(page, pageSize);
        return ok;
    }

    // the physical address a locked virtual page maps to, via pagemap
    static bool physAddr(void* va, uint64_t* phys)
    {
        long pageSize = sysconf(_SC_PAGE_SIZE);
        int fd = ::open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return false;
        uint64_t entry = 0;
        off_t idx = ((uintptr_t)va / pageSize) * 8;
        bool ok = pread(fd, &entry, 8, idx) == 8;
        ::close(fd);
        if (!ok || !(entry & (1ULL << 63)))
            return false; // page not present
        uint64_t pfn = entry & ((1ULL << 55) - 1);
        if (!pfn)
            return false;
        *phys = pfn * (uint64_t)pageSize;
        return true;
    }

    void drop(const std::string& why)
    {
        std::lock_guard<std::mutex> g(m_);
        patched_ = false;
        status_ = why;
        for (int i = 0; i < CHIP_COUNT; i++)
            chipOk_[i] = false;
        fails_ = 0;
    }

    static std::string readLine(const char* path)
    {
        FILE* f = fopen(path, "r");
        if (!f)
            return "";
        char buf[128] = {0};
        if (!fgets(buf, sizeof buf, f))
            buf[0] = 0;
        fclose(f);
        return buf;
    }
    static std::string trim(std::string s)
    {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
            s.pop_back();
        size_t i = 0;
        while (i < s.size() && s[i] == ' ')
            i++;
        return s.substr(i);
    }

    std::atomic<bool> enabled_{false};
    std::atomic<bool> started_{false};
    std::atomic<double> wantedAt_{0};
    Bus bus_;                  // poller-only
    int fails_ = 0;            // poller-only
    std::string lastLogged_ = "\x01"; // poller-only; a sentinel so the first outcome always logs

    std::mutex m_;
    std::condition_variable cv_;
    bool tried_ = false;
    bool patched_ = false;
    std::string status_ = "not yet attempted";
    bool chipOk_[CHIP_COUNT] = {};
    float chipC_[CHIP_COUNT] = {};
};

} // namespace smu
