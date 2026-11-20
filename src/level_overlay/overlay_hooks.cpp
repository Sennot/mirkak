// Innermost hooks (priority INT32_MAX - 1, right before GD's own code) that
// let level A drive level B. Each one first runs level A's call, then makes
// the same call on level B. TulipHook continues a nested call of a hooked
// function at the next detour, which from here is GD's original function, so
// level B's call skips every hook of every mod (including the ones that count
// clicks, attempts, frames and checkpoints).
#include "level_overlay.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string_view>

using namespace geode::prelude;

namespace layoutfeed {
    namespace {
        // INT32_MAX is TulipHook's own priority for the original function.
        constexpr std::int32_t kInnermost = std::numeric_limits<std::int32_t>::max() - 1;

        template <class Self>
        void innermost(Self& self, std::initializer_list<std::string_view> names) {
            for (auto const name : names) {
                if (auto result = self.setHookPriority(name, kInnermost); result.isErr()) {
                    log::warn("Level Overlay: unable to set the priority of {}: {}", name, result.unwrapErr());
                }
            }
        }
    }

    class $modify(LevelOverlayDrivePlayLayer, PlayLayer) {
        static void onModify(auto& self) {
            innermost(self, {
                "PlayLayer::init", "PlayLayer::startGame", "PlayLayer::resetLevel", "PlayLayer::createCheckpoint",
                "PlayLayer::storeCheckpoint", "PlayLayer::removeCheckpoint", "PlayLayer::removeAllCheckpoints",
                "PlayLayer::togglePracticeMode",
            });
        }

        bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
            if (level_overlay::inLevelB()) return PlayLayer::init(level, useReplay, dontCreateObjects);
            // Level B first: its init is a nested call and runs GD's code only.
            level_overlay::beforeLevelInit();
            auto const success = PlayLayer::init(level, useReplay, dontCreateObjects);
            level_overlay::afterLevelInit(this, success);
            return success;
        }

        void startGame() {
            PlayLayer::startGame();
            if (level_overlay::isLevelA(this)) level_overlay::afterStart(this);
        }

        void resetLevel() override {
            PlayLayer::resetLevel();
            if (level_overlay::isLevelA(this)) level_overlay::afterReset(this);
        }

        CheckpointObject* createCheckpoint() {
            auto* checkpoint = PlayLayer::createCheckpoint();
            if (level_overlay::isLevelA(this)) level_overlay::afterCreateCheckpoint(this, checkpoint);
            return checkpoint;
        }

        void storeCheckpoint(CheckpointObject* checkpoint) {
            PlayLayer::storeCheckpoint(checkpoint);
            if (level_overlay::isLevelA(this)) level_overlay::afterStoreCheckpoint(this, checkpoint);
        }

        void removeCheckpoint(bool first) {
            PlayLayer::removeCheckpoint(first);
            if (level_overlay::isLevelA(this)) level_overlay::afterRemoveCheckpoint(this, first);
        }

        void removeAllCheckpoints() override {
            PlayLayer::removeAllCheckpoints();
            if (level_overlay::isLevelA(this)) level_overlay::afterRemoveAllCheckpoints(this);
        }

        void togglePracticeMode(bool practiceMode) {
            PlayLayer::togglePracticeMode(practiceMode);
            if (level_overlay::isLevelA(this)) level_overlay::afterPracticeToggle(this);
        }
    };

    class $modify(LevelOverlayDriveBaseLayer, GJBaseGameLayer) {
        static void onModify(auto& self) {
            innermost(self, {"GJBaseGameLayer::update"});
        }

        void update(float dt) override {
            auto* levelA = level_overlay::drivenBy(this);
            if (!levelA) {
                GJBaseGameLayer::update(dt);
                return;
            }
            level_overlay::beforeUpdate(levelA);
            GJBaseGameLayer::update(dt);
            level_overlay::afterUpdate(levelA, dt);
        }
    };

    // Presses and releases of level A's players reach level B's players at
    // the moment GD applies them to level A.
    class $modify(LevelOverlayDrivePlayer, PlayerObject) {
        static void onModify(auto& self) {
            innermost(self, {"PlayerObject::pushButton", "PlayerObject::releaseButton"});
        }

        bool pushButton(PlayerButton button) {
            auto const result = PlayerObject::pushButton(button);
            if (!level_overlay::inLevelB()) level_overlay::mirrorButton(this, button, true);
            return result;
        }

        bool releaseButton(PlayerButton button) {
            auto const result = PlayerObject::releaseButton(button);
            if (!level_overlay::inLevelB()) level_overlay::mirrorButton(this, button, false);
            return result;
        }
    };

    // Quitting level A releases level B (both are owned by this mod).
    class $modify(LevelOverlayQuit, PlayLayer) {
        void onQuit() {
            if (level_overlay::isLevelA(this)) level_overlay::release("level quit");
            PlayLayer::onQuit();
        }
    };
}
