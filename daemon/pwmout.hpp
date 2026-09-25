#pragma once

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <string>
#include <vector>

#include "hwmon.hpp"

// Driving a PWM output of the HOST — a hwmon pwmN, like the BC-250's own fan
// header on the NCT6686D (README "Fans", "host outputs"). A fan whose output
// is "chip:pwmN" is run by this daemon writing the chip's registers, the way
// CoolerControl or lm-sensors' fancontrol do: pwmN_enable = 1 takes the
// output over (manual), pwmN = 0..255 sets it; the value found in
// pwmN_enable beforehand (2 on the nct6687 driver: the board's own curve) is
// what hands it back.
//
// The one thing that must never happen is an output left in manual mode by a
// daemon that is no longer there to drive it — the board's own curve would
// then be off for good, at whatever duty the last write left. So every
// write to a pwmN_enable goes through Claims, and the rules are:
//
//  - write-ahead: before an output is taken over, the claim — its name
//    ("nct6686:pwm2") and path, and the pwmN_enable and pwmN values found —
//    is recorded in a file under the state dir (/var/lib/led-controller,
//    systemd's StateDirectory), flushed to disk. No record, no takeover.
//  - a claim leaves the record only once the output has verifiably been
//    handed back (read back as restored), or failing that driven to full
//    speed; an output that can be neither is said loudly and kept, so the
//    next attempt tries again (every REASSERT_S while this daemon runs, and
//    at the next start and stop).
//  - the output's name outlives its path: a hwmon driver reloaded while it
//    was driven comes back under another hwmonN — with the chip's registers
//    still in manual mode, since a driver's unload doesn't touch them. A
//    hand-back finds the output by name wherever it is now; one found
//    nowhere (the driver not back yet) stays in the record with the values
//    found at the first takeover, and a takeover of the same name reuses
//    them — so what this daemon wrote is never mistaken for the board's own
//    setting.
//  - handed back: when no fan drives it any more (an edit, a reload, a fan
//    deleted — end() of the tick that stopped naming it), on every return
//    from main (the destructor), and — for the exits no code runs on (a
//    crash, SIGKILL, the systemd watchdog's SIGABRT) — by `led
//    --release-fans`, the unit's ExecStopPost, which reads the record. A
//    daemon starting up hands back whatever the record still holds before it
//    claims anything (a run outside systemd that crashed), so the "values
//    found" are always the board's own, never a dead run's leftovers.
//  - one owner: a lock file in the same dir. A second daemon on the box (a
//    `./led cfg aurora` next to the service) never takes an output over.
//  - re-asserted: every REASSERT_S the output is read back; a pwmN_enable
//    something else reset (a resume from suspend, the chip reloading its own
//    settings) is taken back, and a pwmN value that isn't ours (another fan
//    controller writing the same output) is said once.
//
// Handing back follows fancontrol's pwmdisable, the reference for this:
// restore pwmN first (some chips need it before they return to automatic),
// then pwmN_enable, and check it read back as restored; if not, try
// pwmN_enable = 0 (full speed / no control on most drivers), then manual at
// 255 — an output that can't be handed back ends at FULL speed, never frozen
// at a low duty.
//
// LED_STATE_DIR moves the state dir (tests), as LED_HWMON_ROOT moves the
// hwmon tree (hwmon.hpp).
namespace pwmout
{
static const double REASSERT_S = 5.0;
static const int PWM_ROUNDING = 2;        // a pwmN register may read back this far off a write
static const double TAKE_RETRY_S = 30.0; // a failed takeover is retried this often
static const char* CLAIMS_FILE = "pwm-claims";
static const char* LOCK_FILE = "pwm.lock";

inline std::string stateDir()
{
    const char* env = getenv("LED_STATE_DIR");
    return env && *env ? env : "/var/lib/led-controller";
}

// the pwmN file's pwmN_enable beside it
inline std::string enableOf(const std::string& pwmPath) { return pwmPath + "_enable"; }

// can this daemon take the output over? Both files present and writable by
// their owner (root, which this runs as). The mode bits, not access(): root
// passes access() on a read-only sysfs attribute, and the in-kernel nct6683
// driver's pwm files are exactly that (0444) — it can read the board's fan
// duty but never set it.
inline bool writable(const std::string& pwmPath)
{
    struct stat a, b;
    if (stat(pwmPath.c_str(), &a) != 0 || stat(enableOf(pwmPath).c_str(), &b) != 0)
        return false;
    return (a.st_mode & S_IWUSR) && (b.st_mode & S_IWUSR);
}

// one integer from a sysfs file; false when unreadable or not a number
inline bool readInt(const std::string& path, int& v)
{
    std::string s = hwmon::readFileLine(path);
    if (s.empty())
        return false;
    char* end = nullptr;
    long n = strtol(s.c_str(), &end, 10);
    if (end == s.c_str())
        return false;
    v = (int)n;
    return true;
}

// write one integer to a sysfs file, the way `echo N > file` does; false (with
// errno) when the write is refused
inline bool writeInt(const std::string& path, int v)
{
    int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    char buf[16];
    int n = snprintf(buf, sizeof buf, "%d\n", v);
    bool ok = ::write(fd, buf, n) == n;
    int e = errno;
    ::close(fd);
    errno = e;
    return ok;
}

// percent → the register's 0..255
inline int rawOf(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return (pct * 255 + 50) / 100;
}

// one output as the record keeps it
struct Claim
{
    std::string path; // the pwmN file
    int enable = 2;   // pwmN_enable as found
    int pwm = 255;    // pwmN as found
    std::string spec; // its name, "chip:pwmN" ("" = unknown: found by path only)
};

// where the output named "chip:pwmN" is now; "" when nowhere
inline std::string findOutput(const std::string& spec)
{
    size_t colon = spec.find(':');
    if (colon == std::string::npos)
        return "";
    return hwmon::findChipFile(spec.substr(0, colon), spec.substr(colon + 1));
}

// hand one output back (see the header comment for the order). Returns true
// when it is safe: restored as found, or failing that at full speed. `why`
// holds what happened when it isn't a clean restore.
inline bool handBack(const Claim& c, std::string& why)
{
    // the record outlives a reboot, and hwmonN numbers are handed out at
    // boot: the path may be another chip now. Its name says — a path that
    // isn't the named chip's any more is treated as gone, and the name
    // followed (never write the old values into whatever holds the number)
    const std::string chip = c.spec.substr(0, c.spec.find(':'));
    bool here = hwmon::statExists(c.path) || hwmon::statExists(enableOf(c.path));
    if (here && !c.spec.empty() && hwmon::chipOfFile(c.path) != chip)
        here = false;
    if (!here)
    {
        // the driver is gone from under it — reloaded under another hwmonN,
        // or unloaded (the chip's registers keep our manual setting either
        // way): follow the name, or keep the claim until it shows up again
        std::string now = c.spec.empty() ? "" : findOutput(c.spec);
        if (now.empty() || now == c.path)
        {
            why = "gone (the driver was unloaded?) — kept until " +
                  (c.spec.empty() ? std::string("it") : c.spec) + " shows up again";
            return false;
        }
        Claim moved = c;
        moved.path = now;
        bool ok = handBack(moved, why);
        why = "found again as " + now + (why.empty() ? "" : " — " + why);
        return ok;
    }

    const std::string en = enableOf(c.path);
    int v;

    writeInt(c.path, c.pwm); // best effort; checked below where it matters
    if (c.enable != 1)
    {
        writeInt(en, c.enable);
        if (readInt(en, v) && v == c.enable)
            return true;
    }
    else if (readInt(c.path, v) && abs(v - c.pwm) <= PWM_ROUNDING)
        return true; // it was manual already (someone else's): left at their duty

    // the ladder: automatic didn't stick. No control at all is full speed on
    // most drivers...
    if (writeInt(en, 0) && readInt(en, v) && v == 0)
    {
        why = "pwm_enable would not go back to " + std::to_string(c.enable) + " — set to 0 (no control, full speed)";
        return true;
    }
    // ...and manual at the top is full speed on all of them
    writeInt(en, 1);
    writeInt(c.path, 255);
    int p = 0;
    if (readInt(en, v) && v == 1 && readInt(c.path, p) && p >= 190)
    {
        why = "pwm_enable would not go back to " + std::to_string(c.enable) + " — left in manual at full speed";
        return true;
    }

    why = "STUCK: pwm_enable reads " + (readInt(en, v) ? std::to_string(v) : std::string("nothing")) +
          " and nothing restores it — the output is not being driven by anyone";
    return false;
}

// the record: one line per claim, "path\tenable\tpwm\tspec"
inline std::vector<Claim> readRecord(const std::string& file)
{
    std::vector<Claim> out;
    FILE* f = fopen(file.c_str(), "r");
    if (!f)
        return out;
    char line[1024];
    while (fgets(line, sizeof line, f))
    {
        line[strcspn(line, "\n")] = 0;
        char* t1 = strchr(line, '\t');
        char* t2 = t1 ? strchr(t1 + 1, '\t') : nullptr;
        char* t3 = t2 ? strchr(t2 + 1, '\t') : nullptr;
        if (!t1 || !t2)
            continue;
        Claim c;
        c.path.assign(line, t1 - line);
        c.enable = atoi(t1 + 1);
        c.pwm = atoi(t2 + 1);
        if (t3)
            c.spec = t3 + 1;
        if (!c.path.empty())
            out.push_back(c);
    }
    fclose(f);
    return out;
}

// replace the record with `claims`, durably: a temp file written and synced,
// renamed over, the directory synced — a crash at any point leaves either the
// old record or the new one, never half of either. An empty set removes it.
inline bool writeRecord(const std::string& dir, const std::vector<Claim>& claims)
{
    std::string file = dir + "/" + CLAIMS_FILE;
    if (claims.empty())
    {
        if (unlink(file.c_str()) != 0 && errno != ENOENT)
            return false;
    }
    else
    {
        std::string tmp = file + ".tmp";
        int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0)
            return false;
        std::string text;
        for (auto& c : claims)
            text += c.path + "\t" + std::to_string(c.enable) + "\t" + std::to_string(c.pwm) + "\t" + c.spec + "\n";
        bool ok = ::write(fd, text.data(), text.size()) == (ssize_t)text.size() && fsync(fd) == 0;
        ::close(fd);
        if (!ok || rename(tmp.c_str(), file.c_str()) != 0)
        {
            unlink(tmp.c_str());
            return false;
        }
    }
    int d = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (d >= 0)
    {
        fsync(d);
        ::close(d);
    }
    return true;
}

// hand back everything the record holds, keeping only what can't be; the
// number of outputs still stuck (0 = all safe). Said line by line on `log`.
inline int releaseRecord(const std::string& dir, FILE* log)
{
    std::vector<Claim> left;
    for (auto& c : readRecord(dir + "/" + CLAIMS_FILE))
    {
        std::string why;
        bool ok = handBack(c, why);
        if (ok)
            fprintf(log, "fans: %s handed back%s%s\n", c.path.c_str(), why.empty() ? "" : " — ", why.c_str());
        else
        {
            fprintf(log, "fans: could not hand %s back — %s\n", c.path.c_str(), why.c_str());
            left.push_back(c);
        }
    }
    if (!writeRecord(dir, left))
        fprintf(log, "fans: could not update %s/%s: %s\n", dir.c_str(), CLAIMS_FILE, strerror(errno));
    return (int)left.size();
}

class Claims
{
public:
    Claims() = default;
    Claims(const Claims&) = delete;
    Claims& operator=(const Claims&) = delete;

    ~Claims()
    {
        releaseAll();
        if (lockFd_ >= 0)
            ::close(lockFd_);
    }

    // become the one daemon that may drive host outputs: create the state dir,
    // take its lock, and hand back whatever a previous run left claimed. False
    // (said once) when another daemon holds the lock or the dir can't be kept
    // — this daemon then drives no host output at all, and the board's own
    // curves run them.
    bool open()
    {
        tried_ = true;
        return tryOpen(true);
    }

    // open() on first need: a daemon whose fans name no host output never
    // takes the lock (or says it can't) at all. A lock busy then is tried
    // again every TAKE_RETRY_S (retryOpen). True when this daemon may drive.
    bool want(double now)
    {
        if (active_)
            return true;
        if (!tried_)
        {
            openTried_ = now;
            return open();
        }
        retryOpen(now);
        return active_;
    }

    // does a record from an earlier run exist? Then open() belongs at
    // startup whatever the config names: what it holds is handed back there
    static bool leftover() { return hwmon::statExists(stateDir() + "/" + CLAIMS_FILE); }

    // may this daemon drive host outputs at all?
    bool active() const { return active_; }

    // why not, when it may not ("" when it may, or before open())
    const std::string& why() const { return why_; }

    // open() failed on a lock another process held (a hand-run ./led, the
    // old service still exiting): try again now and then, so the outputs
    // are ours once it is gone rather than never for this whole run
    void retryOpen(double now)
    {
        if (active_ || !lockBusy_ || now - openTried_ < TAKE_RETRY_S)
            return;
        openTried_ = now;
        if (tryOpen(false))
            fprintf(stderr, "fans: the host-output lock is free now — this daemon drives them\n");
    }

private:
    bool tryOpen(bool say)
    {
        lockBusy_ = false;
        dir_ = stateDir();
        if (mkdir(dir_.c_str(), 0755) != 0 && errno != EEXIST)
        {
            why_ = "can't create " + dir_ + ": " + strerror(errno);
            fprintf(stderr, "fans: %s — host outputs are left to the board\n", why_.c_str());
            return false;
        }
        std::string lock = dir_ + "/" + LOCK_FILE;
        // close-on-exec: a command this daemon runs (the power button's
        // poweroff, a popen) must not inherit the lock and outlive us with it
        lockFd_ = ::open(lock.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (lockFd_ < 0)
        {
            why_ = "can't open " + lock + ": " + strerror(errno);
            fprintf(stderr, "fans: %s — host outputs are left to the board\n", why_.c_str());
            return false;
        }
        if (flock(lockFd_, LOCK_EX | LOCK_NB) != 0)
        {
            ::close(lockFd_);
            lockFd_ = -1;
            lockBusy_ = true;
            why_ = "another led daemon drives the host outputs";
            if (say)
                fprintf(stderr, "fans: %s (%s is locked) — this one leaves them alone until it is "
                                "free\n", why_.c_str(), lock.c_str());
            return false;
        }
        if (releaseRecord(dir_, stderr) > 0)
            fprintf(stderr, "fans: a host output from an earlier run could not be handed back — "
                            "see above; retrying while this daemon runs\n");
        // what couldn't be handed back is ours to keep trying (end()), and
        // a takeover of it reuses the values it was found with (take())
        stuck_ = readRecord(dir_ + "/" + CLAIMS_FILE);
        why_.clear();
        active_ = true;
        return true;
    }

public:

    // a tick starts: every output some fan drives is named by drive() before
    // end(), and the ones that weren't are handed back there
    void begin()
    {
        for (auto& o : outs_)
            o.named = false;
    }

    // drive the output `spec` ("nct6686:pwm2"), found at `path` (its
    // resolved pwmN file), at `pct` percent this tick, taking it over first
    // if it isn't ours yet. False when it can't be (not ours to take, the
    // record can't be written, the driver refused the takeover) — the caller
    // shows the output as the board's. `now` is the daemon's clock, for the
    // re-assert cadence.
    bool drive(const std::string& path, const std::string& spec, int pct, double now)
    {
        if (!active_)
            return false;

        int raw = rawOf(pct);
        Out* o = find(path);
        if (!o && !spec.empty())
            for (auto& x : outs_)
                if (x.c.spec == spec)
                {
                    // ours already, under the path it had before its driver
                    // came back as another hwmonN: the claim moves with it,
                    // the values it was first found with included (reading
                    // them now would read our own manual setting)
                    fprintf(stderr, "fans: %s moved from %s to %s — the claim follows\n", spec.c_str(),
                            x.c.path.c_str(), path.c_str());
                    x.c.path = path;
                    x.written = -1;       // write the duty there
                    x.asserted = -1e9;    // and check its enable now
                    if (!writeRecord(dir_, claims()))
                        fprintf(stderr, "fans: could not update %s/%s: %s\n", dir_.c_str(), CLAIMS_FILE,
                                strerror(errno));
                    o = &x;
                    break;
                }
        if (!o)
            return take(path, spec, raw, now);

        o->named = true;
        if (raw != o->written)
        {
            if (!writeInt(path, raw))
                sayOnce(*o, o->warnedWrite, "fans: writing " + path + " failed: " + strerror(errno));
            o->written = raw;
        }

        if (now - o->asserted >= REASSERT_S)
            reassert(*o, now);
        return true;
    }

    // the tick is over: hand back what no fan named, and retry what couldn't
    // be handed back before (every REASSERT_S — a gone driver coming back).
    // A lock that was busy is retried here too, not only on a fan's want():
    // a leftover record opened at startup against another daemon's lock
    // must still be handed back once it is gone, whatever the config names
    void end(double now = 0)
    {
        retryOpen(now);
        for (size_t i = 0; i < outs_.size();)
        {
            if (outs_[i].named)
                i++;
            else
                giveBack(i);
        }

        if (stuck_.empty() || now - retried_ < REASSERT_S)
            return;
        retried_ = now;
        bool changed = false;
        for (size_t i = 0; i < stuck_.size();)
        {
            std::string why;
            if (handBack(stuck_[i], why))
            {
                fprintf(stderr, "fans: %s handed back at last%s%s\n", stuck_[i].path.c_str(),
                        why.empty() ? "" : " — ", why.c_str());
                stuck_.erase(stuck_.begin() + i);
                changed = true;
            }
            else
                i++;
        }
        if (changed && !writeRecord(dir_, claims()))
            fprintf(stderr, "fans: could not update %s/%s: %s\n", dir_.c_str(), CLAIMS_FILE, strerror(errno));
    }

    // hand back every output (shutdown, a reload that drops them all)
    void releaseAll()
    {
        while (!outs_.empty())
            giveBack(0);
    }

    // is `path` driven by this daemon right now?
    bool driving(const std::string& path) const { return findC(path) != nullptr; }

private:
    struct Out
    {
        Claim c;
        int written = -1;     // the pwmN value last written
        double asserted = 0;  // when it was last read back
        bool named = false;   // drive() this tick
        bool warnedWrite = false, warnedOther = false, warnedReset = false;
    };

    Out* find(const std::string& path)
    {
        for (auto& o : outs_)
            if (o.c.path == path)
                return &o;
        return nullptr;
    }
    const Out* findC(const std::string& path) const
    {
        for (auto& o : outs_)
            if (o.c.path == path)
                return &o;
        return nullptr;
    }

    static void sayOnce(Out& o, bool& flag, const std::string& msg)
    {
        (void)o;
        if (!flag)
            fprintf(stderr, "%s\n", msg.c_str());
        flag = true;
    }

    // what the record holds: every output driven, and every one that
    // couldn't be handed back
    std::vector<Claim> claims() const
    {
        std::vector<Claim> v;
        for (auto& o : outs_)
            v.push_back(o.c);
        for (auto& c : stuck_)
            v.push_back(c);
        return v;
    }

    bool take(const std::string& path, const std::string& spec, int raw, double now)
    {
        // a takeover that failed is tried again only every TAKE_RETRY_S: each
        // attempt rewrites the record twice (fsyncs) and pokes pwm_enable,
        // not something to repeat on every 0.5 s tick for a driver that
        // always says no
        if (Failed* f = failed(path))
            if (now < f->retryAt)
                return false;

        Out o;
        o.c.path = path;
        o.c.spec = spec;
        int stuck = -1;
        for (size_t i = 0; i < stuck_.size(); i++)
            // the same output: by name where both have one (a path may be
            // another chip's after a reboot), by path otherwise
            if (!spec.empty() && !stuck_[i].spec.empty() ? stuck_[i].spec == spec : stuck_[i].path == path)
            {
                // an output that couldn't be handed back (or went away while
                // ours): what it holds now is our own setting, not the
                // board's — keep the values it was first found with. It stays
                // in stuck_ until this takeover has either succeeded or handed
                // it back, so a failure below never forgets it
                o.c.enable = stuck_[i].enable;
                o.c.pwm = stuck_[i].pwm;
                stuck = (int)i;
                break;
            }
        if (stuck < 0 && (!readInt(enableOf(path), o.c.enable) || !readInt(path, o.c.pwm)))
        {
            if (!warnedTake(path, now))
                fprintf(stderr, "fans: %s can't be read — not taking it over\n", path.c_str());
            return false;
        }
        if (o.c.enable == 1 && stuck < 0)
            // manual already: someone else's (CoolerControl, fancontrol) —
            // taken over all the same (the config says so), and handed back
            // to them exactly as found
            fprintf(stderr, "fans: %s was already in manual mode (another fan controller?) — "
                            "taking it over; it goes back to manual at %d when released\n",
                    path.c_str(), o.c.pwm);

        // write-ahead: the record first, or nothing (the stuck entry, if any,
        // is this claim — recorded once)
        std::vector<Claim> next;
        for (auto& x : outs_)
            next.push_back(x.c);
        for (size_t i = 0; i < stuck_.size(); i++)
            if ((int)i != stuck)
                next.push_back(stuck_[i]);
        next.push_back(o.c);
        if (!writeRecord(dir_, next))
        {
            if (!warnedTake(path, now))
                fprintf(stderr, "fans: can't record the claim on %s in %s (%s) — not taking it over\n",
                        path.c_str(), dir_.c_str(), strerror(errno));
            return false;
        }

        int v = -1;
        bool manual = writeInt(enableOf(path), 1) && readInt(enableOf(path), v) && v == 1;
        bool ok = manual && writeInt(path, raw);
        std::string refused = manual ? std::string("took manual mode but refused the duty (") + strerror(errno) + ")"
                                     : "refused manual control (pwm_enable reads " + std::to_string(v) + ")";
        if (!ok)
        {
            // refused (a read-only driver, a chip that won't go manual):
            // restore what was there and drop the record again. A stuck
            // entry is this claim: it leaves stuck_ only once handed back,
            // under the path it has now
            std::string why;
            bool back = handBack(o.c, why);
            if (stuck >= 0)
            {
                if (back)
                    stuck_.erase(stuck_.begin() + stuck);
                else
                    stuck_[stuck].path = path;
            }
            else if (!back)
                stuck_.push_back(o.c);
            if (!writeRecord(dir_, claims()))
                fprintf(stderr, "fans: could not update %s/%s: %s\n", dir_.c_str(), CLAIMS_FILE,
                        strerror(errno));
            if (!warnedTake(path, now))
                fprintf(stderr, "fans: %s %s — left to the board\n", path.c_str(), refused.c_str());
            return false;
        }

        if (stuck >= 0)
            stuck_.erase(stuck_.begin() + stuck); // ours again: outs_ holds it now
        o.written = raw;
        o.asserted = now;
        o.named = true;
        outs_.push_back(o);
        forgetTake(path);
        fprintf(stderr, "fans: took over %s (was pwm_enable %d, pwm %d)\n", path.c_str(), o.c.enable, o.c.pwm);
        return true;
    }

    // the read-back (see the header comment)
    void reassert(Out& o, double now)
    {
        o.asserted = now;
        int en = -1, p = -1;
        if (readInt(enableOf(o.c.path), en) && en != 1)
        {
            sayOnce(o, o.warnedReset, "fans: " + o.c.path + "_enable was reset to " + std::to_string(en) +
                                          " (a resume, or another program) — taking it back");
            writeInt(enableOf(o.c.path), 1);
            writeInt(o.c.path, o.written);
            return;
        }
        // a register may round a write; further apart is someone else's
        // value (another program, or a resume that reset the duty but not
        // the mode) — the config says this output is ours: write it again
        if (readInt(o.c.path, p) && o.written >= 0 && abs(p - o.written) > PWM_ROUNDING)
        {
            sayOnce(o, o.warnedOther, "fans: " + o.c.path + " reads " + std::to_string(p) + ", not the " +
                                          std::to_string(o.written) + " written — rewriting it (a resume, or "
                                          "another fan controller like CoolerControl or fancontrol driving it too?)");
            writeInt(o.c.path, o.written);
        }
    }

    // hand back outs_[i] and drop it (record updated). True when it left the
    // record; a stuck output stays in the record but leaves outs_ (nothing
    // drives it), so the ExecStopPost and the next start retry it
    bool giveBack(size_t i)
    {
        Out o = outs_[i];
        outs_.erase(outs_.begin() + i);
        std::string why;
        bool ok = handBack(o.c, why);
        if (ok)
            fprintf(stderr, "fans: handed %s back%s%s\n", o.c.path.c_str(), why.empty() ? "" : " — ", why.c_str());
        else
            fprintf(stderr, "fans: could not hand %s back — %s\n", o.c.path.c_str(), why.c_str());
        if (!ok)
        {
            stuck_.push_back(o.c);
            fprintf(stderr, "fans: %s stays in %s/%s for the next attempt\n", o.c.path.c_str(), dir_.c_str(),
                    CLAIMS_FILE);
        }
        if (!writeRecord(dir_, claims()))
            fprintf(stderr, "fans: could not update %s/%s: %s\n", dir_.c_str(), CLAIMS_FILE, strerror(errno));
        return ok;
    }

    // a failed takeover: said once per output until one succeeds, and not
    // tried again before retryAt
    struct Failed
    {
        std::string path;
        double retryAt;
    };
    Failed* failed(const std::string& path)
    {
        for (auto& f : failed_)
            if (f.path == path)
                return &f;
        return nullptr;
    }
    // true when this output's failure was said already
    bool warnedTake(const std::string& path, double now)
    {
        if (Failed* f = failed(path))
        {
            f->retryAt = now + TAKE_RETRY_S;
            return true;
        }
        failed_.push_back({path, now + TAKE_RETRY_S});
        return false;
    }
    void forgetTake(const std::string& path)
    {
        for (size_t i = 0; i < failed_.size(); i++)
            if (failed_[i].path == path)
            {
                failed_.erase(failed_.begin() + i);
                return;
            }
    }

    std::string dir_;
    std::string why_;
    int lockFd_ = -1;
    bool active_ = false;
    bool tried_ = false;      // open() has run (want() opens on first need)
    bool lockBusy_ = false;   // open() failed on the lock: retryOpen() tries again
    double openTried_ = 0;
    double retried_ = -1e9;
    std::vector<Out> outs_;
    std::vector<Claim> stuck_; // handed back and failed: kept in the record
    std::vector<Failed> failed_;
};

// `led --release-fans` (the unit's ExecStopPost): hand back whatever the
// record holds. Leaves them alone while a daemon holds the lock — it is the
// one driving them (a hand-run ./led beside the service), would take them
// straight back, and releases them itself; that is no failure of the stop.
// The exit code: 0 safe (handed back, or another daemon's), 1 stuck.
inline int releaseCommand()
{
    std::string dir = stateDir();
    std::string file = dir + "/" + CLAIMS_FILE;
    if (!hwmon::statExists(file))
        return 0; // nothing claimed: the common case after a clean stop

    int fd = ::open((dir + "/" + LOCK_FILE).c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) != 0)
    {
        fprintf(stderr, "fans: another led daemon holds the host outputs — leaving them to it\n");
        ::close(fd);
        return 0;
    }
    int stuck = releaseRecord(dir, stderr);
    if (fd >= 0)
        ::close(fd);
    return stuck ? 1 : 0;
}
} // namespace pwmout
