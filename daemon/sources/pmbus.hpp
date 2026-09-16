#pragma once

#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The BC-250's VRM controller: a PMBus device at 0x60 on the board's main
// SMBus. That bus reaches a header only through a two-wire mod (I2C_HEADER1's
// SCL/SDA bridged to TPMS1 pins 4 and 6 — README "VRM temperatures"); with
// the wires in and i2c-dev loaded the kernel exposes it as /dev/i2c-N, and
// the controller's page 0 is the CPU rail, page 1 the GPU rail. Its register
// formats are undocumented; that READ_TEMP1's low 11 bits are whole °C is
// what BC250-Telemetry (github.com/onlinermm/BC250-Telemetry, MIT)
// established against a multimeter, and the reads here follow theirs.
//
// One poller thread owns the bus. A PAGE write wants a few ms to settle
// before the read, which the render loop can't afford, so the thread reads
// both rails every half second while anything has asked recently and the
// daemon takes the latest values (README "Resource budget": nothing is read
// that nothing wants). The bus is found by probing every /dev/i2c-* for a
// device at 0x60 whose READ_VOUT answers sensibly — a read only; nothing is
// written to a bus until one has answered. A device that stops answering is
// dropped and the scan repeats.
namespace pmbus
{
inline const uint8_t ADDR = 0x60;
inline const uint8_t CMD_PAGE = 0x00;
inline const uint8_t CMD_READ_VOUT = 0x8B;
inline const uint8_t CMD_READ_TEMP1 = 0x8D;
inline const uint16_t TEMP_MASK = 0x07FF; // the low 11 bits of READ_TEMP1 are the value

struct Rail
{
    const char* label; // as the config names it: "pmbus:CPU VRM"
    uint8_t page;
};
inline const Rail RAILS[] = {{"CPU VRM", 0}, {"GPU VRM", 1}};
inline const int RAIL_COUNT = 2;

// the rail a label names, or -1
inline int railOf(const std::string& label)
{
    for (int i = 0; i < RAIL_COUNT; i++)
        if (label == RAILS[i].label)
            return i;
    return -1;
}

inline int32_t smbus(int fd, uint8_t rw, uint8_t cmd, uint32_t size, i2c_smbus_data* data)
{
    i2c_smbus_ioctl_data a;
    a.read_write = rw;
    a.command = cmd;
    a.size = size;
    a.data = data;
    return ioctl(fd, I2C_SMBUS, &a);
}

// a 16-bit register, or -1 when the device did not answer
inline int32_t readWord(int fd, uint8_t cmd)
{
    i2c_smbus_data d;
    memset(&d, 0, sizeof d);
    if (smbus(fd, I2C_SMBUS_READ, cmd, I2C_SMBUS_WORD_DATA, &d) < 0)
        return -1;
    return d.word;
}

inline int32_t writeByte(int fd, uint8_t cmd, uint8_t v)
{
    i2c_smbus_data d;
    memset(&d, 0, sizeof d);
    d.byte = v;
    return smbus(fd, I2C_SMBUS_WRITE, cmd, I2C_SMBUS_BYTE_DATA, &d);
}

class Reader
{
public:
    // one bus, one thread, for the process; never destroyed (a detached
    // thread must not outlive its object at exit)
    static Reader& get()
    {
        static Reader* r = new Reader;
        return *r;
    }

    // the latest temperature of a rail in °C; false while none is being
    // read. Asking keeps the poller going.
    bool temp(int rail, float& v)
    {
        want();
        if (rail < 0 || rail >= RAIL_COUNT)
            return false;
        std::lock_guard<std::mutex> g(m_);
        if (!ok_[rail])
            return false;
        v = temp_[rail];
        return true;
    }

    // a controller has answered on some bus. Waits (bounded) for the first
    // scan so a one-shot caller such as --fan-status sees the answer.
    bool present()
    {
        want();
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait_for(lk, std::chrono::milliseconds(300), [&] { return scanned_; });
        return fd_ >= 0;
    }

    // "/dev/i2c-4" once found, "" before
    std::string device()
    {
        std::lock_guard<std::mutex> g(m_);
        return dev_;
    }

private:
    static constexpr double WANT_S = 10;   // poll while asked within this long
    static constexpr double RESCAN_S = 30; // between bus scans while none answers
    static constexpr int LOST_AFTER = 10;  // failed transactions in a row = the device is gone
    static constexpr int POLL_MS = 500;
    static constexpr int SETTLE_US = 5000; // after a PAGE write

    Reader() = default;

    static double mono()
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec + ts.tv_nsec / 1e9;
    }

    void want()
    {
        wantedAt_.store(mono());
        bool started = started_.exchange(true);
        if (!started)
            std::thread(&Reader::loop, this).detach();
    }

    void loop()
    {
        for (;;)
        {
            double now = mono();
            if (now - wantedAt_.load() < WANT_S)
            {
                if (fd_ < 0 && now - lastScan_ >= RESCAN_S)
                    scan(now);
                if (fd_ >= 0)
                    readRails();
            }
            {
                std::lock_guard<std::mutex> g(m_);
                scanned_ = true;
            }
            cv_.notify_all();
            std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
        }
    }

    // every /dev/i2c-N, lowest first; the first with the controller wins
    void scan(double now)
    {
        lastScan_ = now;
        DIR* dir = opendir("/dev");
        if (!dir)
            return;
        std::vector<int> buses;
        while (dirent* e = readdir(dir))
            if (strncmp(e->d_name, "i2c-", 4) == 0)
                buses.push_back(atoi(e->d_name + 4));
        closedir(dir);
        std::sort(buses.begin(), buses.end());

        for (int n : buses)
        {
            std::string path = "/dev/i2c-" + std::to_string(n);
            int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
            if (fd < 0)
                continue;
            if (ioctl(fd, I2C_SLAVE, ADDR) < 0)
            {
                close(fd);
                continue;
            }
            // a rail's output voltage is never 0 nor all ones; a device that
            // answers 0x8B with either (or not at all) is not the controller
            int32_t vout = readWord(fd, CMD_READ_VOUT);
            if (vout <= 0 || vout == 0xFFFF)
            {
                close(fd);
                continue;
            }
            fprintf(stderr, "pmbus: VRM controller at 0x%02x on %s\n", ADDR, path.c_str());
            {
                std::lock_guard<std::mutex> g(m_);
                dev_ = path;
            }
            failures_ = 0;
            fd_ = fd;
            return;
        }
    }

    void readRails()
    {
        for (int i = 0; i < RAIL_COUNT; i++)
        {
            bool ok = false;
            float t = 0;
            if (writeByte(fd_, CMD_PAGE, RAILS[i].page) >= 0)
            {
                usleep(SETTLE_US);
                int32_t w = readWord(fd_, CMD_READ_TEMP1);
                if (w >= 0)
                {
                    failures_ = 0;
                    if (w != 0xFFFF)
                    {
                        t = (float)(w & TEMP_MASK);
                        ok = t > 0 && t <= 120;
                    }
                }
                else
                    failures_++;
            }
            else
                failures_++;

            std::lock_guard<std::mutex> g(m_);
            ok_[i] = ok;
            temp_[i] = t;
        }

        if (failures_ >= LOST_AFTER)
        {
            std::string dev;
            {
                std::lock_guard<std::mutex> g(m_);
                dev = dev_;
                dev_.clear();
                for (int i = 0; i < RAIL_COUNT; i++)
                    ok_[i] = false;
            }
            fprintf(stderr, "pmbus: %s stopped answering — looking for the controller again\n",
                    dev.c_str());
            close(fd_);
            fd_ = -1;
            lastScan_ = 0; // now
        }
    }

    std::atomic<bool> started_{false};
    std::atomic<double> wantedAt_{0};
    std::atomic<int> fd_{-1};
    double lastScan_ = -1e9; // poller-only
    int failures_ = 0;       // poller-only

    std::mutex m_;
    std::condition_variable cv_;
    bool scanned_ = false;
    std::string dev_;
    bool ok_[RAIL_COUNT] = {};
    float temp_[RAIL_COUNT] = {};
};

} // namespace pmbus
