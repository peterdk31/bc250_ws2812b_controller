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
#include <cmath>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hwmon_tree.hpp"

// The BC-250's VRM controller: a PMBus device at 0x60 on the board's main
// SMBus. That bus reaches a header only through a two-wire mod (I2C_HEADER1's
// SCL/SDA bridged to TPMS1 pins 4 and 6 — README "VRM temperatures"); with
// the wires in and i2c-dev loaded the kernel exposes it as /dev/i2c-N, and
// the controller's page 0 is the CPU rail, page 1 the GPU rail. Its register
// formats are undocumented; that READ_TEMP1's low 11 bits are whole °C is
// what BC250-Telemetry (github.com/onlinermm/BC250-Telemetry, MIT)
// established against a multimeter, and the reads here follow theirs. The
// voltage and current scales are the bc250_vrm kernel driver's
// (github.com/Hexxeh/bc250-vrm-dkms).
//
// Two ways in, one at a time. A kernel with the bc250_vrm driver (the
// linux-cachyos-bc250 kernels load it on every BC-250) owns the controller:
// i2c-dev refuses an address a driver has bound, and two masters' PAGE
// writes would interleave anyway. So while that driver's hwmon chip is
// registered the controller is read through its sysfs files and the bus is
// never opened; without it the daemon finds the bus itself. A driver that
// turns up later (a modprobe after the daemon started) takes over within
// DRIVER_CHECK_S, the bus released first.
//
// One poller thread owns either way in. A PAGE write wants a few ms to
// settle before the read — the driver sleeps that inside every sysfs read —
// which the render loop can't afford, so the thread reads both rails every
// half second while anything has asked recently and the daemon takes the
// latest values (README "Resource budget": nothing is read that nothing
// wants). The temperatures are what a fan or a rule asks for; the voltages
// and currents only a watching phone does (power()), and they are read only
// then. The bus is found by probing every /dev/i2c-* for a device at 0x60
// whose READ_VOUT answers sensibly — a read only; nothing is written to a bus
// until one has answered. A device that stops answering is dropped and the
// scan repeats.
namespace pmbus
{
inline const uint8_t ADDR = 0x60;
inline const uint8_t CMD_PAGE = 0x00;
inline const uint8_t CMD_READ_VIN = 0x88;
inline const uint8_t CMD_READ_VOUT = 0x8B;
inline const uint8_t CMD_READ_IOUT = 0x8C;
inline const uint8_t CMD_READ_TEMP1 = 0x8D;
inline const uint16_t TEMP_MASK = 0x07FF; // the low 11 bits of READ_TEMP1 are the value
inline const float VIN_SCALE = 0.01f;     // READ_VIN counts 10 mV
inline const float VOUT_SCALE = 0.001f;   // READ_VOUT counts 1 mV
inline const float IOUT_SCALE = 0.1f;     // READ_IOUT counts 100 mA
inline const int IOUT_MAX = 2500;         // a larger count is a misread (the driver's bound)

// the kernel driver: its hwmon chip name, and the label of its 12 V input
inline const char* DRIVER = "bc250_vrm";
inline const char* DRIVER_VIN = "VIN (12V Input)";

struct Rail
{
    const char* label; // as the config names it: "pmbus:CPU VRM"
    uint8_t page;
    // the driver's labels for this rail's temperature, output voltage, current
    const char* driverTemp;
    const char* driverVolts;
    const char* driverAmps;
};
inline const Rail RAILS[] = {
    {"CPU VRM", 0, "CPU VRM Temp", "CPU Voltage", "CPU Current"},
    {"GPU VRM", 1, "GPU VRM Temp", "GPU Core Voltage", "GPU Current"},
};
inline const int RAIL_COUNT = 2;

// the rail a label names, or -1
inline int railOf(const std::string& label)
{
    for (int i = 0; i < RAIL_COUNT; i++)
        if (label == RAILS[i].label)
            return i;
    return -1;
}

// the rail whose temperature the driver labels so (a bare chip name is its
// first input), or -1
inline int railOfDriverLabel(const std::string& label)
{
    if (label.empty())
        return 0;
    for (int i = 0; i < RAIL_COUNT; i++)
        if (label == RAILS[i].driverTemp)
            return i;
    return -1;
}

// what the controller reports, NAN where it has no reading
struct Power
{
    float vin = NAN;                        // the 12 V input, V
    float volts[RAIL_COUNT] = {NAN, NAN};   // each rail's output, V
    float amps[RAIL_COUNT] = {NAN, NAN};    // each rail's output current, A
    float temp[RAIL_COUNT] = {NAN, NAN};    // °C
};

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
    // one controller, one thread, for the process; never destroyed (a
    // detached thread must not outlive its object at exit)
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
        if (std::isnan(now_.temp[rail]))
            return false;
        v = now_.temp[rail];
        return true;
    }

    // everything the controller reports. Asking keeps the poller reading
    // the voltages and currents as well as the temperatures.
    Power power()
    {
        powerWantedAt_.store(mono());
        want();
        std::lock_guard<std::mutex> g(m_);
        return now_;
    }

    // a controller is being read, by either way in. Waits (bounded) for the
    // first look so a one-shot caller such as --fan-status sees the answer.
    bool present()
    {
        want();
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait_for(lk, std::chrono::milliseconds(300), [&] { return scanned_; });
        return !source_.empty();
    }

    // which way in: "/dev/i2c-4", "the bc250_vrm driver (/sys/class/hwmon/hwmon5)",
    // "" while neither
    std::string source()
    {
        std::lock_guard<std::mutex> g(m_);
        return source_;
    }

private:
    static constexpr double WANT_S = 10;        // poll while asked within this long
    static constexpr double RESCAN_S = 30;      // between looks while none answers
    static constexpr double DRIVER_CHECK_S = 5; // on the bus: between looks for the driver
    static constexpr int LOST_AFTER = 10;       // failed transactions in a row = the device is gone
    static constexpr int POLL_MS = 500;
    static constexpr int SETTLE_US = 5000; // after a PAGE write

    // the driver's files, resolved once its chip is found
    struct Files
    {
        std::string dir, vin;
        std::string temp[RAIL_COUNT], volts[RAIL_COUNT], amps[RAIL_COUNT];
    };

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
                bool full = now - powerWantedAt_.load() < WANT_S;
                if (fd_ >= 0 && now - lastDriverCheck_ >= DRIVER_CHECK_S)
                {
                    lastDriverCheck_ = now;
                    if (!hwmon::chipDir(DRIVER).empty())
                    {
                        fprintf(stderr, "pmbus: the %s driver has the controller now — releasing the bus\n",
                                DRIVER);
                        drop();
                    }
                }
                if (fd_ < 0 && files_.dir.empty() && now - lastScan_ >= RESCAN_S)
                    scan(now);
                if (!files_.dir.empty())
                    readDriver(full);
                else if (fd_ >= 0)
                    readBus(full);
            }
            {
                std::lock_guard<std::mutex> g(m_);
                scanned_ = true;
            }
            cv_.notify_all();
            std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
        }
    }

    // the driver's chip if it is registered, else every /dev/i2c-N, lowest
    // first; the first with the controller wins
    void scan(double now)
    {
        lastScan_ = now;
        lastDriverCheck_ = now;
        if (findDriver())
            return;

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
                source_ = path;
            }
            failures_ = 0;
            fd_ = fd;
            return;
        }
    }

    // the driver's chip, its files resolved by label; false when it is not
    // registered or carries neither rail's temperature
    bool findDriver()
    {
        std::string dir = hwmon::chipDir(DRIVER);
        if (dir.empty())
            return false;
        Files f;
        f.dir = dir;
        f.vin = hwmon::labelledInput(dir, "in", DRIVER_VIN);
        bool any = false;
        for (int i = 0; i < RAIL_COUNT; i++)
        {
            f.temp[i] = hwmon::labelledInput(dir, "temp", RAILS[i].driverTemp);
            f.volts[i] = hwmon::labelledInput(dir, "in", RAILS[i].driverVolts);
            f.amps[i] = hwmon::labelledInput(dir, "curr", RAILS[i].driverAmps);
            any |= !f.temp[i].empty();
        }
        if (!any)
            return false;
        fprintf(stderr, "pmbus: reading the VRM controller through the %s driver (%s)\n", DRIVER,
                dir.c_str());
        files_ = f;
        std::lock_guard<std::mutex> g(m_);
        source_ = std::string("the ") + DRIVER + " driver (" + dir + ")";
        return true;
    }

    // one sysfs value in milli-units, NAN when it did not read or is not positive
    static float milli(const std::string& path)
    {
        long v;
        return hwmon::readLong(path, v) && v > 0 ? v / 1000.0f : NAN;
    }

    void readDriver(bool full)
    {
        // the module was unloaded: look again (the bus, if it answers)
        if (!hwmon::statExists(files_.dir))
        {
            fprintf(stderr, "pmbus: the %s driver went away — looking for the controller again\n", DRIVER);
            drop();
            return;
        }
        Power p;
        for (int i = 0; i < RAIL_COUNT; i++)
        {
            p.temp[i] = milli(files_.temp[i]);
            if (full)
            {
                p.volts[i] = milli(files_.volts[i]);
                p.amps[i] = milli(files_.amps[i]);
            }
        }
        if (full)
            p.vin = milli(files_.vin);
        std::lock_guard<std::mutex> g(m_);
        now_ = p;
    }

    // a register read after the PAGE write: the raw count, or -1 (and a
    // failure counted) when the device did not answer
    int32_t word(uint8_t cmd)
    {
        int32_t w = readWord(fd_, cmd);
        if (w < 0)
            failures_++;
        else
            failures_ = 0;
        return w;
    }

    void readBus(bool full)
    {
        Power p;
        for (int i = 0; i < RAIL_COUNT; i++)
        {
            if (writeByte(fd_, CMD_PAGE, RAILS[i].page) < 0)
            {
                failures_++;
                continue;
            }
            usleep(SETTLE_US);
            int32_t w = word(CMD_READ_TEMP1);
            // the believable range is hwmon::readTempOk's to judge
            if (w >= 0 && w != 0xFFFF && (w & TEMP_MASK) > 0)
                p.temp[i] = (float)(w & TEMP_MASK);
            if (!full)
                continue;
            w = word(CMD_READ_VOUT);
            if (w > 0 && w != 0xFFFF)
                p.volts[i] = w * VOUT_SCALE;
            w = word(CMD_READ_IOUT);
            if (w >= 0 && w <= IOUT_MAX)
                p.amps[i] = w * IOUT_SCALE;
            if (RAILS[i].page == 0) // the input is read on page 0
            {
                w = word(CMD_READ_VIN);
                if (w > 0 && w != 0xFFFF)
                    p.vin = w * VIN_SCALE;
            }
        }
        {
            std::lock_guard<std::mutex> g(m_);
            now_ = p;
        }

        if (failures_ >= LOST_AFTER)
        {
            fprintf(stderr, "pmbus: %s stopped answering — looking for the controller again\n",
                    source().c_str());
            drop();
        }
    }

    // forget the way in (closing the bus) and look again on the next poll
    void drop()
    {
        if (fd_ >= 0)
            close(fd_);
        fd_ = -1;
        files_ = Files();
        lastScan_ = 0; // now
        std::lock_guard<std::mutex> g(m_);
        source_.clear();
        now_ = Power();
    }

    std::atomic<bool> started_{false};
    std::atomic<double> wantedAt_{0};
    std::atomic<double> powerWantedAt_{0};
    int fd_ = -1;                   // poller-only: the bus, while read that way
    Files files_;                   // poller-only: the driver's files, while read that way
    double lastScan_ = -1e9;        // poller-only
    double lastDriverCheck_ = -1e9; // poller-only
    int failures_ = 0;              // poller-only

    std::mutex m_;
    std::condition_variable cv_;
    bool scanned_ = false;
    std::string source_;
    Power now_;
};

} // namespace pmbus
