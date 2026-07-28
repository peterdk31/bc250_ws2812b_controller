#pragma once

#include <cstdint>

// A tiny in-RAM debug log with a host backchannel. Any module can drop a line
// into a small ring buffer with dbglog::line(); nothing is transmitted on its
// own. When the daemon has its debug backchannel on it periodically sends
// CMD_LOG_DRAIN (protocol.hpp), and onDrain() replies with a LOG_SYNC frame per
// buffered line newer than the seq the host has — so the lines surface in
// `journalctl -u led-controller`. See daemon/output/serial_sink.hpp.
//
// The point of buffering in RAM (rather than streaming) is that the receiver
// runs on 5VSB and outlives the host: events logged while the daemon is down
// (a follow-down as the board shuts off) survive and drain when it comes back.
//
// Cost when nobody listens is near zero: line() is a bounded vsnprintf into a
// static ring, and callers gate their chatty periodic logging on active() —
// true only if the host has drained recently — so an un-drained board only
// ever formats the occasional event line.
namespace dbglog
{
// append one line (printf-style; truncated to the slot size). Thread-safe.
void line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// has the host drained within the last ~30 s? Gate high-rate periodic logging
// on this so a board no one is watching stays quiet.
bool active();

// handle CMD_LOG_DRAIN: transmit every buffered line with seq > sinceSeq, in
// order, as LOG_SYNC frames over the link. Called from the receive path.
void onDrain(uint32_t sinceSeq);
} // namespace dbglog
