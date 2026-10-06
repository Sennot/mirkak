#include <Geode/Geode.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/modify/CCEGLView.hpp>

#include "debug.hpp"
#include "layout/layout_pass.hpp"
#include "settings.hpp"
#include "spout/spout_sender.hpp"

using namespace geode::prelude;

namespace layoutfeed {
    namespace {
        // One presented frame:
        //   1. the game has rendered the normal frame (decoration, colors,
        //      shaders, camera effects) into the back buffer;
        //   2. that frame is shared with OBS through Spout2 (GPU copy);
        //   3. in a level with Layout Mode on, the scene is drawn again on top
        //      in layout style, and only that version reaches the monitor.
        void present() {
            {
                debug::CpuScope cpu(debug::Timer::Spout);
                debug::GpuScope gpu(debug::Timer::Spout);
                spout::SpoutSender::get().captureBackBuffer();
            }
            debug::checkGL("spout::capture");

            if (pass::shouldRender()) pass::render();
            debug::endFrame();
        }
    }

    class $modify(LayoutFeedView, cocos2d::CCEGLView) {
        static void onModify(auto& self) {
            // Run before every other swapBuffers hook. Overlays drawn later by
            // other mods (ImGui menus, Mega Hack) stay on the monitor and are
            // neither captured into Spout2 nor covered by the layout pass.
            if (auto result = self.setHookPriority("cocos2d::CCEGLView::swapBuffers", Priority::FirstPre); result.isErr()) {
                log::warn("Unable to set swapBuffers hook priority: {}", result.unwrapErr());
            }
        }

        void swapBuffers() override {
            present();
            cocos2d::CCEGLView::swapBuffers();
        }
    };
}

$on_mod(Loaded) {
    log::info("Spout2 Layout Feed {} loaded", Mod::get()->getVersion().toVString());
    layoutfeed::settings::logSummary();
}

$on_game(Loaded) {
    listenForKeybindSettingPresses(
        "layout-keybind",
        [](Keybind const&, bool down, bool repeat, double) {
            if (!down || repeat) return;
            auto* mod = Mod::get();
            auto const enabled = !mod->getSettingValue<bool>("layout-enabled");
            mod->setSettingValue<bool>("layout-enabled", enabled);
            log::info("Layout Mode {} by keybind", enabled ? "enabled" : "disabled");
        }
    );
}
