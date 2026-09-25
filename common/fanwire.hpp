#pragma once

#include <stdint.h>
#include <string.h>

#include "protocol.hpp"

// One receiver slot's standalone record — the unit of CMD_FAN_STANDALONE
// (six of them), of the receiver's fancfg partition and NVS blob, and of the
// phone's control op (one). Encoded and decoded here, on both ends of the
// wire, so the layout is written down exactly once; protocol.hpp says what
// each field means. tools/fancfg.py and docs/ble.js mirror this by hand.
namespace fanwire
{
static const uint8_t NONE = proto::FAN_NONE;
static const int POINTS = proto::FAN_CURVE_POINTS;
static const uint16_t LEN = proto::FAN_HEADER_LEN;

struct Header
{
    uint8_t fallback = NONE; // resting duty percent
    uint8_t boost = NONE;    // NONE = sits the boost out
    uint8_t boostSecs = proto::FAN_DEFAULT_BOOST_SECS;
    uint8_t ramp = proto::FAN_DEFAULT_RAMP;
    uint8_t kind = proto::FAN_KIND_HOST;
    uint8_t gpio = NONE;     // a gpio fan's input pin
    uint8_t npts = 0;        // a gpio fan's curve: points (0 = none)
    uint8_t pts[POINTS][2] = {}; // (input %, duty %), sorted by input
    uint8_t outKind = NONE;  // FAN_OUT_HEADER / FAN_OUT_GPIO; anything else = no output
    uint8_t out = NONE;      // the header number (1-based) or the GPIO

    // does this record drive anything? A slot whose record doesn't is unused
    bool used() const
    {
        return outKind == proto::FAN_OUT_HEADER || outKind == proto::FAN_OUT_GPIO;
    }

    // the record of an unused slot: 0xFF throughout
    static Header unused()
    {
        uint8_t p[LEN];
        memset(p, NONE, sizeof p);
        return decode(p);
    }

    void encode(uint8_t* p) const
    {
        p[0] = fallback;
        p[1] = boost;
        p[2] = boostSecs;
        p[3] = ramp;
        p[4] = kind;
        p[5] = gpio;
        p[6] = npts;
        memcpy(p + 7, pts, sizeof pts);
        p[7 + sizeof pts] = outKind;
        p[8 + sizeof pts] = out;
    }

    static Header decode(const uint8_t* p)
    {
        Header h;
        h.fallback = p[0];
        h.boost = p[1];
        h.boostSecs = p[2];
        h.ramp = p[3];
        h.kind = p[4];
        h.gpio = p[5];
        h.npts = p[6];
        memcpy(h.pts, p + 7, sizeof h.pts);
        h.outKind = p[7 + sizeof h.pts];
        h.out = p[8 + sizeof h.pts];
        return h;
    }

    // a curve as it arrives, checked: 1..POINTS points, inputs strictly
    // rising, everything 0..100
    bool curveOk() const
    {
        if (npts < 1 || npts > POINTS)
            return false;
        for (int j = 0; j < npts; j++)
        {
            if (pts[j][0] > 100 || pts[j][1] > 100)
                return false;
            if (j && pts[j][0] <= pts[j - 1][0])
                return false;
        }
        return true;
    }

    bool operator==(const Header& o) const
    {
        uint8_t a[LEN], b[LEN];
        encode(a);
        o.encode(b);
        return memcmp(a, b, LEN) == 0;
    }
    bool operator!=(const Header& o) const { return !(*this == o); }
};

static_assert(LEN == 9 + 2 * POINTS, "the record's layout is protocol.hpp's");
} // namespace fanwire
