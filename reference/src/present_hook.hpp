#pragma once

namespace cleanfeed::present_hook {
    // When system overlay capture is enabled and the OpenGL driver's present
    // entry points could be hooked, defers `compose` to the driver present.
    // It then runs after in-process overlays (Steam, RivaTuner, legacy
    // Discord) have drawn into the back buffer. Returns false when the caller
    // must compose the frame itself, which is also the automatic fallback
    // after the driver hook misses several frames.
    bool deferFrame(void (*compose)());
}
