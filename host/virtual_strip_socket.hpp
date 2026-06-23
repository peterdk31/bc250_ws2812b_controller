#pragma once

// The default AF_UNIX datagram socket that the daemon's VirtualSink
// (host/virtual_sink.hpp) and the standalone viewer (host/virtual_strip.cpp)
// rendezvous on. The two processes must agree on it, so it lives in exactly
// one place rather than being duplicated in both. Kept dependency-free on
// purpose: the viewer is plain g++ with no other project includes.
inline constexpr char kVirtualStripSocket[] = "/tmp/led-strip.sock";
