// Hooks of the Level Overlay. Level A drives level B; level B is kept away
// from audio, statistics, death and the end of the level.
#include "level_overlay.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GameStatsManager.hpp>
#include <Geode/modify/PlayLayer.hpp>

using namespace geode::prelude;

namespace layoutfeed {
    class $modify(LevelOverlayPlayLayer, PlayLayer) {
        static void onModify(auto& self) {
            // Outermost: level B is created before any other init hook sees
            // level A, so mods that remember the PlayLayer end with level A.
            if (auto result = self.setHookPriority("PlayLayer::init", Priority::First); result.isErr()) {
                log::warn("Level Overlay: unable to set PlayLayer::init priority: {}", result.unwrapErr());
            }
        }

        bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
            if (level_overlay::constructing()) return PlayLayer::init(level, useReplay, dontCreateObjects);
            level_overlay::beforeLevelInit();
            auto const success = PlayLayer::init(level, useReplay, dontCreateObjects);
            level_overlay::afterLevelInit(this, success);
            return success;
        }

        void onQuit() {
            if (level_overlay::isLevelA(this)) level_overlay::release("level quit");
            PlayLayer::onQuit();
        }

        void resetLevel() override {
            PlayLayer::resetLevel();
            if (level_overlay::isLevelA(this)) level_overlay::onReset(this);
        }

        void storeCheckpoint(CheckpointObject* checkpoint) {
            PlayLayer::storeCheckpoint(checkpoint);
            if (level_overlay::isLevelA(this)) level_overlay::onCheckpointStored(this);
        }

        void removeCheckpoint(bool first) {
            PlayLayer::removeCheckpoint(first);
            if (level_overlay::isLevelA(this)) level_overlay::onCheckpointRemoved(this, first);
        }

        void togglePracticeMode(bool practiceMode) {
            PlayLayer::togglePracticeMode(practiceMode);
            if (level_overlay::isLevelA(this)) level_overlay::onPracticeToggled(this);
        }

        // Level B never finishes: the end screen, its effects and the level
        // statistics belong to level A.
        void levelComplete() {
            if (!level_overlay::isLevelB(this)) PlayLayer::levelComplete();
        }

        void playEndAnimationToPos(CCPoint position) {
            if (!level_overlay::isLevelB(this)) PlayLayer::playEndAnimationToPos(position);
        }

        void playPlatformerEndAnimationToPos(CCPoint position, bool instant) {
            if (!level_overlay::isLevelB(this)) PlayLayer::playPlatformerEndAnimationToPos(position, instant);
        }

        void showCompleteText() {
            if (!level_overlay::isLevelB(this)) PlayLayer::showCompleteText();
        }

        void showCompleteEffect() {
            if (!level_overlay::isLevelB(this)) PlayLayer::showCompleteEffect();
        }

        // Pausing (focus loss, the back key) is for level A only.
        void pauseGame(bool unfocused) {
            if (!level_overlay::isLevelB(this)) PlayLayer::pauseGame(unfocused);
        }

        // Only level A plays music.
        void startMusic() {
            if (!level_overlay::isLevelB(this)) PlayLayer::startMusic();
        }

        void prepareMusic(bool dontWait) {
            if (!level_overlay::isLevelB(this)) PlayLayer::prepareMusic(dontWait);
        }

        void toggleMusicInPractice() override {
            if (!level_overlay::isLevelB(this)) PlayLayer::toggleMusicInPractice();
        }
    };

    class $modify(LevelOverlayBaseGameLayer, GJBaseGameLayer) {
        void update(float dt) override {
            auto* levelA = level_overlay::isLevelA(this) ? static_cast<PlayLayer*>(static_cast<GJBaseGameLayer*>(this)) : nullptr;
            if (levelA) level_overlay::beforeUpdate(levelA);
            GJBaseGameLayer::update(dt);
            if (levelA) level_overlay::afterUpdate(levelA, dt);
        }

        // Song and SFX triggers of level B stay silent.
        void processActivatedAudioTriggers(float levelTime) {
            if (!level_overlay::isLevelB(this)) GJBaseGameLayer::processActivatedAudioTriggers(levelTime);
        }

        void processQueuedAudioTriggers() {
            if (!level_overlay::isLevelB(this)) GJBaseGameLayer::processQueuedAudioTriggers();
        }

        void activateSFXTrigger(SFXTriggerGameObject* object) {
            if (!level_overlay::isLevelB(this)) GJBaseGameLayer::activateSFXTrigger(object);
        }

        void activateSFXEditTrigger(SFXTriggerGameObject* object) {
            if (!level_overlay::isLevelB(this)) GJBaseGameLayer::activateSFXEditTrigger(object);
        }

        void pauseAudio() {
            if (!level_overlay::isLevelB(this)) GJBaseGameLayer::pauseAudio();
        }

        void resumeAudio() {
            if (!level_overlay::isLevelB(this)) GJBaseGameLayer::resumeAudio();
        }

        void resetAudio() {
            if (!level_overlay::isLevelB(this)) GJBaseGameLayer::resetAudio();
        }
    };

    // Attempts, jumps and other totals of level B are not counted.
    class $modify(LevelOverlayStats, GameStatsManager) {
        void incrementStat(char const* key, int amount) {
            if (!level_overlay::inLevelB()) GameStatsManager::incrementStat(key, amount);
        }
    };
}
