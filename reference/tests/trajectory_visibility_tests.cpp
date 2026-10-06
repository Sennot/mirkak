#include "trajectory/visibility.hpp"

#include <iostream>
#include <stdexcept>

namespace {
    enum class Playback { Not, Playing, Paused };
    struct Layer { int value = 0; };
    struct Editor : Layer { Playback playback = Playback::Not; };

    void require(bool condition, char const* message) {
        if (!condition) throw std::runtime_error(message);
    }
}

int main() {
    try {
        using cleanfeed::trajectory_visibility::suppressForIdleEditor;
        Layer playLayer;
        Editor editor, otherEditor;
        unsigned reads = 0;
        auto const idle = [&](Editor const& value) {
            ++reads;
            return value.playback == Playback::Not;
        };

        require(!suppressForIdleEditor(&playLayer, static_cast<Editor*>(nullptr), idle),
            "Ordinary level without an editor was suppressed");
        require(!suppressForIdleEditor(static_cast<Layer*>(nullptr), &editor, idle),
            "Missing current layer was treated as an active editor");
        require(!suppressForIdleEditor(static_cast<Layer*>(nullptr), static_cast<Editor*>(nullptr), idle),
            "Null layers matched");
        require(!suppressForIdleEditor(&playLayer, &editor, idle),
            "Retained idle editor hid the ordinary level's trajectory");
        require(!suppressForIdleEditor(static_cast<Layer*>(&editor), &otherEditor, idle),
            "Unrelated editor hid the current layer's trajectory");
        require(reads == 0, "Unrelated editor playback state was accessed");

        // Editing -> playtest -> pause -> stop -> play an ordinary level.
        auto* current = static_cast<Layer*>(&editor);
        require(suppressForIdleEditor(current, &editor, idle), "Idle active editor was not suppressed");
        require(reads == 1, "Active editor playback state was not checked");
        editor.playback = Playback::Playing;
        require(!suppressForIdleEditor(current, &editor, idle), "Editor playtest was suppressed");
        editor.playback = Playback::Paused;
        require(!suppressForIdleEditor(current, &editor, idle), "Paused playtest behavior changed");
        editor.playback = Playback::Not;
        require(suppressForIdleEditor(current, &editor, idle), "Stopped playtest remained visible");
        current = &playLayer;
        auto const readsBeforeReturn = reads;
        require(!suppressForIdleEditor(current, &editor, idle),
            "Trajectory stayed hidden after returning from the editor to a level");
        require(reads == readsBeforeReturn, "Returning to gameplay still reads the old editor");

        std::cout << "PASS: retained editor, null/unrelated layers, editor play/pause/stop, return to gameplay\n";
        return 0;
    } catch (std::exception const& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
