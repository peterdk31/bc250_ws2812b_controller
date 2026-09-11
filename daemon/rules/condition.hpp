#pragma once

#include <memory>
#include <string>
#include "config_loader.hpp"

// a rule condition, evaluated on the daemon's slow tick
class Condition
{
public:
    virtual ~Condition() = default;

    virtual bool eval() = 0;
};

// parse a condition spec:
//   always         true
//   temp>N         sensor temperature above N °C (sensor from `sensors` key)
//   temp<N         sensor temperature below N °C
//   gpu_load>N     GPU busy above N % (amdgpu); bare `gpu_load` means >20
//   gpu_load<N     GPU busy below N %
//   cpu_load>N     CPU busy above N % (/proc/stat); bare `cpu_load` means >20
//   cpu_load<N     CPU busy below N %
//   proc:NAME      a process with that name is running
//   file:/PATH     the file exists (external event injection: touch/rm it)
//   !COND          negation, e.g. !proc:steam
//   A & B          conjunction, all terms must hold
//   A | B          disjunction; & binds tighter, no parentheses
// returns nullptr for unknown syntax
std::unique_ptr<Condition> parseCondition(const std::string& spec,
                                          const Config& cfg);

// is `spec` exactly one file: atom — no &, |, or ! around it — i.e. a rule
// that is a plain on/off switch? Then `path` is the file it watches, read
// with the same trimming parseCondition applies. The phone's dashboard lists
// these as scenes (daemon/strip_remote.hpp); keeping the test here means the
// two can't disagree about what a switch is.
bool fileSwitchPath(const std::string& spec, std::string& path);
