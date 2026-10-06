#pragma once

namespace layoutfeed::overlay {
    // Mod menus such as Mega Hack draw inside their own swapBuffers hooks,
    // after the layout pass has replaced the back buffer. To show them both on
    // the monitor and in OBS, the frame is split:
    //
    //   begin()  (before the menus)  normal frame -> saved copy N, layout pass
    //                                -> back buffer, then a transparent overlay
    //                                target O is bound for the menus to draw on
    //   finish() (after the menus)   O is composited over the back buffer
    //                                (monitor) and over N, and N goes to Spout2
    //
    // Returns false when the targets cannot be created; the caller then uses
    // the simple path (menus only on the monitor).
    bool begin(void (*renderLayout)());

    // Composites the overlay and sends the result. Must follow begin().
    void finish();

    bool redirecting();

    // Unbinds a redirect that was never finished (another hook skipped the
    // swap), so the next frame cannot render into the overlay target.
    void abandon(char const* where);
}
