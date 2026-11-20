#pragma once

#include <string_view>

// Calls into the original code of a hooked function, past every hook of
// every mod. Level B uses it so that other mods (Mega Hack, bots, counters)
// never see its simulation.
//
// TulipHook writes a jump at the start of a hooked function to its handler.
// The handler loads a pointer to its list of detours, which always ends with
// the trampoline (the relocated original prologue followed by a jump back).
// That trampoline is read from the handler and checked before it is used.
namespace layoutfeed::vanilla {
    // Trampoline of the function this mod hooks under `hookName` (for example
    // "PlayLayer::postUpdate"), or nullptr when it cannot be found safely. The
    // result stays valid while this mod's hook on the function is enabled.
    void* find(std::string_view hookName);

    // Calls a trampoline found by find(). Member functions on Win64 take
    // `this` as the first argument; only scalar and pointer results are used.
    template <class R, class... Args>
    R call(void* function, Args... args) {
        return reinterpret_cast<R (*)(Args...)>(function)(args...);
    }

    // How many trampolines were resolved and rejected, for the log.
    void logSummary();
}
