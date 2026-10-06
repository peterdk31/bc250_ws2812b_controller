#pragma once

#include <stdint.h>
#include <memory>
#include <string>
#include <vector>

#include "protocol.hpp"
#include "sink.hpp"

// The BLE dashboard's views, this side (common/protocol.hpp CMD_VIEW): each
// daemon module that shows something on the phone owns a view id and sends
// its JSON through here; the receiver keeps the last one per id and serves
// it without reading it. The way back — a phone's edit, MSG_EDIT — is routed
// by the same id in main.cpp.
namespace views
{
// view `id`'s JSON onto every link. False, with nothing sent, when it is over
// the receiver's ceiling (VIEW_MAX) — the receiver would drop it, so the
// caller says so in the journal instead.
inline bool send(std::vector<std::unique_ptr<Sink>>& sinks, uint8_t id, const std::string& json)
{
    if (json.size() > proto::VIEW_MAX)
        return false;
    std::vector<uint8_t> p;
    p.reserve(1 + json.size());
    p.push_back(id);
    p.insert(p.end(), json.begin(), json.end());
    for (auto& s : sinks)
        s->sendCommand(proto::CMD_VIEW, p.data(), (uint16_t)p.size());
    return true;
}
} // namespace views
