#pragma once

#include <cstdint>

// The receiver→host *request* channel: the one thing the receiver asks the host
// to do, as opposed to the debug log it merely answers when asked (dbglog.hpp).
//
// Only the power switch uses it. A short press on the button while the machine
// is up should shut the OS down gracefully, and nothing but the OS can do that
// — so the receiver sends a REQ_SYNC frame (protocol.hpp), the daemon replies
// with CMD_REQ_ACK and runs its poweroff command, and the power switch's
// ordinary follow-down releases PS_ON# once the board's rail collapses. Nothing
// here touches the PSU.
//
// A request repeats until it is answered: a single 8-byte frame can be lost to
// noise or to a daemon that is mid-reopen, and "please power off" is idempotent
// so re-asking is free. The requester owns the giving-up policy — it calls
// cancel() when the request is answered, when it times out, or when the reason
// for asking has gone away.
namespace hostreq
{
// ask the host. Callable from any task; the frame itself goes out from the task
// that owns link writes (see tick). A second request supersedes the first.
void request(uint8_t req);

// stop repeating. Idempotent, and safe with nothing outstanding.
void cancel();

// has the outstanding request been answered?
bool acked();

// count an outstanding request of this type as answered without an explicit
// ack — the host's actions can answer for it (CMD_SHUTDOWN means it *is*
// shutting down, which is all REQ_HOST_SHUTDOWN ever asked for).
void satisfy(uint8_t req);

// transmit a repeat if one is due. Must be called from the task that owns link
// writes (led_service's loop), because two tasks writing would interleave bytes
// and corrupt each other's frames. A no-op with nothing outstanding, which is
// approximately always.
void tick(uint32_t now);

// handle CMD_REQ_ACK from the host: payload req(1) nonce(4). Called from the
// receive path.
void onAck(const uint8_t* payload, uint16_t len);
} // namespace hostreq
