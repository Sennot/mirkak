#pragma once

namespace cleanfeed::trajectory_visibility {
    // The editor singleton can survive while a different PlayLayer is active.
    // Do not even read its playback state unless it owns this prediction.
    template <class Layer, class Editor, class IsIdle>
    bool suppressForIdleEditor(Layer const* currentLayer, Editor const* editor, IsIdle const& isIdle) {
        return currentLayer && editor && currentLayer == editor && isIdle(*editor);
    }
}
