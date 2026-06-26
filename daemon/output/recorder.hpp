#pragma once

#include <stdint.h>
#include <stdio.h>
#include <vector>
#include <string>
#include <memory>
#include "config_loader.hpp"
#include "effect.hpp"
#include "strip.hpp"
#include "sink.hpp"
#include "protocol.hpp"
#include "recording.hpp"

// Recording an effect for the ESP32 to replay. The receiver used to compile
// every effect and render the power-on/shutdown animations itself, which meant
// a reflash for every effect tweak. Instead the daemon renders the configured
// slot here — through the same Strip the live loop uses, so the pixels are
// byte-identical to what the hardware shows — and streams the finished frames
// over serial. The receiver just stores and replays them; it runs no effect
// code at all.
//
// The Recording type, its byte layout and the replay math live in
// common/recording.hpp (shared with the firmware so they can't drift); this
// file is only the host-side capture and upload.
namespace rec
{

// Render `slot` (an "esp32.*" config block) into `out`, using `canvas` for the
// same brightness/gamma/white-balance/dither correction the live strip applies.
// `canvas` is taken by value so recording never disturbs the daemon's own Strip.
//
// A slot is either a single effect block or a "sequence" of them played back to
// back, captured into one recording. Each segment is rendered until it reports
// finished() (a finite animation like shutdown — captured to completion) or it
// hits the segment's `record_seconds` cap (an open-ended effect like boot).
// The final segment decides the tail: with "loop": true the recording loops
// from that segment's first frame (a one-shot intro leading into a looping
// idle — e.g. CRT boot then a breathing glow), otherwise it holds its last
// frame. Earlier segments always play once.
//
// Rendering simulates the receiver's clock (t advances by frameMs per frame)
// rather than running in real time, so capturing seconds of animation takes
// microseconds and is deterministic. The whole recording plays at one rate —
// the first segment's frame_ms — so every segment is sampled at that step.
inline bool record(const Config& cfg, Strip canvas, const std::string& slot,
                   Recording& out)
{
    const json::Value* block = cfg.find(slot);

    if (!block)
        return false;

    // an explicit "sequence" array, or the block itself as a single segment
    std::vector<const json::Value*> segs;
    const json::Value* seq = block->find("sequence");

    if (seq && seq->isArray())
        for (const json::Value& e : seq->items)
            segs.push_back(&e);
    else
        segs.push_back(block);

    out = Recording();
    out.count = (uint16_t)canvas.size();

    // a hard frame ceiling across the whole sequence so a misconfigured effect
    // can't capture forever; generous enough that no real slot reaches it
    const uint16_t kMaxFrames = 40000;

    for (size_t si = 0; si < segs.size(); si++)
    {
        const json::Value* seg = segs[si];
        const json::Value* eff = seg->find("effect");

        if (!eff)
            continue;

        std::string name = json::toString(*eff);
        std::unique_ptr<Effect> effect = createEffect(name);

        if (!effect)
        {
            fprintf(stderr, "record %s: unknown effect %s\n", slot.c_str(),
                    name.c_str());
            return false;
        }

        effect->init(EffectConfig(cfg, seg->find("settings")), canvas.size());

        // the recording plays at a single rate: the first segment's
        if (out.frameCount == 0)
            out.frameMs = (uint16_t)effect->frameDelayMs();

        float cap = seg->find("record_seconds")
                        ? json::toFloat(*seg->find("record_seconds"), 6.0f)
                        : 6.0f;

        float dt = out.frameMs / 1000.0f;
        float t = 0.0f;
        uint16_t segStart = out.frameCount;

        while (out.frameCount < kMaxFrames)
        {
            canvas.beginFrame();
            effect->render(canvas, t);
            const std::vector<uint8_t>& wire = canvas.endFrame();

            // wire = header (proto::PIX_HEADER bytes) + pixels + checksum; the
            // pixels are the exact corrected bytes the strip shows. pin comes
            // straight off the frame so a recording always names the pin it was
            // rendered for. The anim id / crossfade header fields are live-only,
            // so a recording stores just the pixels.
            out.pin = wire[2];
            out.data.insert(out.data.end(), wire.begin() + proto::PIX_HEADER,
                            wire.begin() + proto::PIX_HEADER + out.count * 3);
            out.frameCount++;

            bool finished = effect->finished();
            t += dt;

            if (finished || t >= cap)
                break;
        }

        // the last segment sets the tail: loop from its start, or hold its
        // last frame
        if (si + 1 == segs.size())
        {
            out.loop = seg->find("loop") &&
                       json::toBool(*seg->find("loop"), false);
            out.loopStart = out.loop ? segStart : 0;
        }
    }

    return out.valid();
}

// Stream a Recording to every sink as CMD_REC_BEGIN / N×CMD_REC_FRAME /
// CMD_REC_END (common/protocol.hpp). Only the serial transport acts on
// commands; the viewer and any other sink no-op (see host/sink.hpp).
inline void upload(std::vector<std::unique_ptr<Sink>>& sinks, uint8_t slotId,
                   const Recording& r)
{
    uint8_t begin[Recording::kBeginLen];
    r.encodeBegin(begin, slotId);

    for (auto& s : sinks)
        s->sendCommand(proto::CMD_REC_BEGIN, begin, sizeof begin);

    for (uint16_t i = 0; i < r.frameCount; i++)
        for (auto& s : sinks)
            s->sendCommand(proto::CMD_REC_FRAME, r.frame(i),
                           (uint16_t)r.frameBytes());

    for (auto& s : sinks)
        s->sendCommand(proto::CMD_REC_END, &slotId, 1);
}

} // namespace rec
