// Outermost hooks (priority INT32_MIN) that keep level B to itself. While
// level B's code runs (level_overlay::inLevelB()), each of these functions
// either goes straight to GD's original code, past every other mod's hook, or
// does nothing at all:
//   - deaths, inputs, jumps, statistics, the end of the level, pausing and
//     audio never happen for level B;
//   - everything else that Mega Hack, bots, CPS counters and death trackers
//     hook runs as plain GD code, so none of them counts level B's frames,
//     clicks, attempts or checkpoints as the player's.
// For level A (and everything else) the hooks only pass the call on.
#include "level_overlay.hpp"
#include "vanilla.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/CheckpointObject.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GJEffectManager.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/GameStatsManager.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string_view>

using namespace geode::prelude;

// Runs GD's original code for level B and returns its result. Falls through
// to the normal hook chain when the original cannot be reached safely.
#define LF_LEVEL_B_DIRECT(Ret, name, ...)                                         \
    if (::layoutfeed::level_overlay::inLevelB()) {                                \
        static void* const s_direct = ::layoutfeed::vanilla::find(name);          \
        if (s_direct) return ::layoutfeed::vanilla::call<Ret>(s_direct, __VA_ARGS__); \
    }

// Level B never does this.
#define LF_LEVEL_B_SKIP(...)                                                      \
    if (::layoutfeed::level_overlay::inLevelB() || ::layoutfeed::level_overlay::owns(this)) { \
        return __VA_ARGS__;                                                       \
    }

namespace layoutfeed {
    namespace {
        constexpr std::int32_t kOutermost = std::numeric_limits<std::int32_t>::min();

        template <class Self>
        void outermost(Self& self, std::initializer_list<std::string_view> names) {
            for (auto const name : names) {
                if (auto result = self.setHookPriority(name, kOutermost); result.isErr()) {
                    log::warn("Level Overlay: unable to set the priority of {}: {}", name, result.unwrapErr());
                }
            }
        }
    }

    class $modify(LevelBIsolationPlayLayer, PlayLayer) {
        static void onModify(auto& self) {
            outermost(self, {
                "PlayLayer::destroyPlayer", "PlayLayer::levelComplete", "PlayLayer::playEndAnimationToPos",
                "PlayLayer::playPlatformerEndAnimationToPos", "PlayLayer::showCompleteText",
                "PlayLayer::showCompleteEffect", "PlayLayer::showEndLayer", "PlayLayer::showNewBest",
                "PlayLayer::pauseGame", "PlayLayer::startMusic", "PlayLayer::prepareMusic",
                "PlayLayer::toggleMusicInPractice", "PlayLayer::storeCheckpoint",
                "PlayLayer::postUpdate", "PlayLayer::updateVisibility", "PlayLayer::addObject",
                "PlayLayer::setupHasCompleted", "PlayLayer::createCheckpoint", "PlayLayer::loadFromCheckpoint",
                "PlayLayer::resetLevel", "PlayLayer::removeCheckpoint", "PlayLayer::removeAllCheckpoints",
                "PlayLayer::startGame", "PlayLayer::updateAttempts", "PlayLayer::updateProgressbar",
                "PlayLayer::updateTestModeLabel", "PlayLayer::toggleProgressbar", "PlayLayer::getCurrentPercent",
                "PlayLayer::spawnCircle", "PlayLayer::updateTimeWarp", "PlayLayer::updateInfoLabel",
            });
        }

        // Level B follows the real player and cannot die.
        void destroyPlayer(PlayerObject* player, GameObject* object) override {
            LF_LEVEL_B_SKIP()
            PlayLayer::destroyPlayer(player, object);
        }

        void levelComplete() {
            LF_LEVEL_B_SKIP()
            PlayLayer::levelComplete();
        }

        void playEndAnimationToPos(CCPoint position) {
            LF_LEVEL_B_SKIP()
            PlayLayer::playEndAnimationToPos(position);
        }

        void playPlatformerEndAnimationToPos(CCPoint position, bool instant) {
            LF_LEVEL_B_SKIP()
            PlayLayer::playPlatformerEndAnimationToPos(position, instant);
        }

        void showCompleteText() {
            LF_LEVEL_B_SKIP()
            PlayLayer::showCompleteText();
        }

        void showCompleteEffect() {
            LF_LEVEL_B_SKIP()
            PlayLayer::showCompleteEffect();
        }

        void showEndLayer() {
            LF_LEVEL_B_SKIP()
            PlayLayer::showEndLayer();
        }

        void showNewBest(bool newReward, int orbs, int diamonds, bool demonKey, bool noRetry, bool noTitle) {
            LF_LEVEL_B_SKIP()
            PlayLayer::showNewBest(newReward, orbs, diamonds, demonKey, noRetry, noTitle);
        }

        void pauseGame(bool unfocused) {
            LF_LEVEL_B_SKIP()
            PlayLayer::pauseGame(unfocused);
        }

        // Only level A plays music.
        void startMusic() {
            LF_LEVEL_B_SKIP()
            PlayLayer::startMusic();
        }

        void prepareMusic(bool dontWait) {
            LF_LEVEL_B_SKIP()
            PlayLayer::prepareMusic(dontWait);
        }

        void toggleMusicInPractice() override {
            LF_LEVEL_B_SKIP()
            PlayLayer::toggleMusicInPractice();
        }

        // Level B stores only the checkpoints mirrored from level A; those
        // calls come from level A's own storeCheckpoint and skip this hook.
        void storeCheckpoint(CheckpointObject* checkpoint) {
            LF_LEVEL_B_SKIP()
            PlayLayer::storeCheckpoint(checkpoint);
        }

        void postUpdate(float dt) override {
            LF_LEVEL_B_DIRECT(void, "PlayLayer::postUpdate", static_cast<PlayLayer*>(this), dt)
            PlayLayer::postUpdate(dt);
        }

        void updateVisibility(float dt) override {
            LF_LEVEL_B_DIRECT(void, "PlayLayer::updateVisibility", static_cast<PlayLayer*>(this), dt)
            PlayLayer::updateVisibility(dt);
        }

        void addObject(GameObject* object) {
            LF_LEVEL_B_DIRECT(void, "PlayLayer::addObject", static_cast<PlayLayer*>(this), object)
            PlayLayer::addObject(object);
        }

        void setupHasCompleted() {
            LF_LEVEL_B_DIRECT(void, "PlayLayer::setupHasCompleted", static_cast<PlayLayer*>(this))
            PlayLayer::setupHasCompleted();
        }

        CheckpointObject* createCheckpoint() {
            LF_LEVEL_B_DIRECT(CheckpointObject*, "PlayLayer::createCheckpoint", static_cast<PlayLayer*>(this))
            return PlayLayer::createCheckpoint();
        }

        void loadFromCheckpoint(CheckpointObject* checkpoint) {
            LF_LEVEL_B_DIRECT(void, "PlayLayer::loadFromCheckpoint", static_cast<PlayLayer*>(this), checkpoint)
            PlayLayer::loadFromCheckpoint(checkpoint);
        }

        void resetLevel() override {
            LF_LEVEL_B_DIRECT(void, "PlayLayer::resetLevel", static_cast<PlayLayer*>(this))
            PlayLayer::resetLevel();
        }

        void removeCheckpoint(bool first) {
            LF_LEVEL_B_DIRECT(void, "PlayLayer::removeCheckpoint", static_cast<PlayLayer*>(this), first)
            PlayLayer::removeCheckpoint(first);
        }

        void removeAllCheckpoints() override {
            LF_LEVEL_B_DIRECT(void, "PlayLayer::removeAllCheckpoints", static_cast<PlayLayer*>(this))
            PlayLayer::removeAllCheckpoints();
        }

        void startGame() {
            LF_LEVEL_B_DIRECT(void, "PlayLayer::startGame", static_cast<PlayLayer*>(this))
            PlayLayer::startGame();
        }

        void updateAttempts() {
            LF_LEVEL_B_DIRECT(void, "PlayLayer::updateAttempts", static_cast<PlayLayer*>(this))
            PlayLayer::updateAttempts();
        }

        void updateProgressbar() {
            LF_LEVEL_B_DIRECT(void, "PlayLayer::updateProgressbar", static_cast<PlayLayer*>(this))
            PlayLayer::updateProgressbar();
        }

        void updateTestModeLabel() {
            LF_LEVEL_B_DIRECT(void, "PlayLayer::updateTestModeLabel", static_cast<PlayLayer*>(this))
            PlayLayer::updateTestModeLabel();
        }

        void toggleProgressbar() override {
            LF_LEVEL_B_DIRECT(void, "PlayLayer::toggleProgressbar", static_cast<PlayLayer*>(this))
            PlayLayer::toggleProgressbar();
        }

        float getCurrentPercent() {
            LF_LEVEL_B_DIRECT(float, "PlayLayer::getCurrentPercent", static_cast<PlayLayer*>(this))
            return PlayLayer::getCurrentPercent();
        }

        void spawnCircle() {
            LF_LEVEL_B_DIRECT(void, "PlayLayer::spawnCircle", static_cast<PlayLayer*>(this))
            PlayLayer::spawnCircle();
        }

        void updateTimeWarp(float timeWarp) override {
            LF_LEVEL_B_DIRECT(void, "PlayLayer::updateTimeWarp", static_cast<PlayLayer*>(this), timeWarp)
            PlayLayer::updateTimeWarp(timeWarp);
        }

        void updateInfoLabel() {
            LF_LEVEL_B_DIRECT(void, "PlayLayer::updateInfoLabel", static_cast<PlayLayer*>(this))
            PlayLayer::updateInfoLabel();
        }
    };

    class $modify(LevelBIsolationBaseLayer, GJBaseGameLayer) {
        static void onModify(auto& self) {
            outermost(self, {
                "GJBaseGameLayer::handleButton", "GJBaseGameLayer::activateSFXTrigger",
                "GJBaseGameLayer::activateSFXEditTrigger", "GJBaseGameLayer::activateSongEditTrigger",
                "GJBaseGameLayer::activateSongTrigger", "GJBaseGameLayer::processActivatedAudioTriggers",
                "GJBaseGameLayer::processQueuedAudioTriggers", "GJBaseGameLayer::processSFXObjects",
                "GJBaseGameLayer::updateProximityVolumeEffects", "GJBaseGameLayer::pauseAudio",
                "GJBaseGameLayer::resumeAudio", "GJBaseGameLayer::resetAudio",
                "GJBaseGameLayer::checkpointActivated",
                "GJBaseGameLayer::update", "GJBaseGameLayer::processCommands", "GJBaseGameLayer::getModifiedDelta",
                "GJBaseGameLayer::checkCollisions", "GJBaseGameLayer::collisionCheckObjects",
                "GJBaseGameLayer::updateCamera", "GJBaseGameLayer::resetLevelVariables", "GJBaseGameLayer::init",
                "GJBaseGameLayer::bumpPlayer", "GJBaseGameLayer::playerTouchedRing",
                "GJBaseGameLayer::playerTouchedTrigger", "GJBaseGameLayer::canBeActivatedByPlayer",
                "GJBaseGameLayer::gameEventTriggered", "GJBaseGameLayer::toggleFlipped",
                "GJBaseGameLayer::updateGradientLayers", "GJBaseGameLayer::updateShaderLayer",
                "GJBaseGameLayer::updateAudioVisualizer", "GJBaseGameLayer::teleportPlayer",
                "GJBaseGameLayer::flipGravity", "GJBaseGameLayer::destroyObject",
                "GJBaseGameLayer::playerWillSwitchMode", "GJBaseGameLayer::toggleDualMode",
            });
        }

        // Level B's player is driven by level A; no input reaches it from
        // the game, a bot or another mod.
        void handleButton(bool down, int button, bool isPlayer1) {
            LF_LEVEL_B_SKIP()
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        // Song and SFX triggers of level B stay silent and never touch the
        // music of level A.
        void activateSFXTrigger(SFXTriggerGameObject* object) {
            LF_LEVEL_B_SKIP()
            GJBaseGameLayer::activateSFXTrigger(object);
        }

        void activateSFXEditTrigger(SFXTriggerGameObject* object) {
            LF_LEVEL_B_SKIP()
            GJBaseGameLayer::activateSFXEditTrigger(object);
        }

        void activateSongEditTrigger(SongTriggerGameObject* object) {
            LF_LEVEL_B_SKIP()
            GJBaseGameLayer::activateSongEditTrigger(object);
        }

        void activateSongTrigger(SongTriggerGameObject* object) {
            LF_LEVEL_B_SKIP()
            GJBaseGameLayer::activateSongTrigger(object);
        }

        void processActivatedAudioTriggers(float levelTime) {
            LF_LEVEL_B_SKIP()
            GJBaseGameLayer::processActivatedAudioTriggers(levelTime);
        }

        void processQueuedAudioTriggers() {
            LF_LEVEL_B_SKIP()
            GJBaseGameLayer::processQueuedAudioTriggers();
        }

        void processSFXObjects() {
            LF_LEVEL_B_SKIP()
            GJBaseGameLayer::processSFXObjects();
        }

        void updateProximityVolumeEffects() {
            LF_LEVEL_B_SKIP()
            GJBaseGameLayer::updateProximityVolumeEffects();
        }

        void pauseAudio() {
            LF_LEVEL_B_SKIP()
            GJBaseGameLayer::pauseAudio();
        }

        void resumeAudio() {
            LF_LEVEL_B_SKIP()
            GJBaseGameLayer::resumeAudio();
        }

        void resetAudio() {
            LF_LEVEL_B_SKIP()
            GJBaseGameLayer::resetAudio();
        }

        // Platformer checkpoints of level B follow level A's checkpoints.
        void checkpointActivated(CheckpointGameObject* object) override {
            LF_LEVEL_B_SKIP()
            GJBaseGameLayer::checkpointActivated(object);
        }

        void update(float dt) override {
            LF_LEVEL_B_DIRECT(void, "GJBaseGameLayer::update", static_cast<GJBaseGameLayer*>(this), dt)
            GJBaseGameLayer::update(dt);
        }

        void processCommands(float dt, bool isHalfTick, bool isLastTick) {
            LF_LEVEL_B_DIRECT(void, "GJBaseGameLayer::processCommands", static_cast<GJBaseGameLayer*>(this), dt,
                              isHalfTick, isLastTick)
            GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        }

        double getModifiedDelta(float dt) {
            LF_LEVEL_B_DIRECT(double, "GJBaseGameLayer::getModifiedDelta", static_cast<GJBaseGameLayer*>(this), dt)
            return GJBaseGameLayer::getModifiedDelta(dt);
        }

        int checkCollisions(PlayerObject* player, float dt, bool ignoreDamage) {
            LF_LEVEL_B_DIRECT(int, "GJBaseGameLayer::checkCollisions", static_cast<GJBaseGameLayer*>(this), player,
                              dt, ignoreDamage)
            return GJBaseGameLayer::checkCollisions(player, dt, ignoreDamage);
        }

        void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects, int count, float dt) {
            LF_LEVEL_B_DIRECT(void, "GJBaseGameLayer::collisionCheckObjects", static_cast<GJBaseGameLayer*>(this),
                              player, objects, count, dt)
            GJBaseGameLayer::collisionCheckObjects(player, objects, count, dt);
        }

        // Level B's camera follows exactly where level A's player is.
        void updateCamera(float dt) {
            if (level_overlay::inLevelB()) level_overlay::placeLevelBPlayers(this);
            LF_LEVEL_B_DIRECT(void, "GJBaseGameLayer::updateCamera", static_cast<GJBaseGameLayer*>(this), dt)
            GJBaseGameLayer::updateCamera(dt);
        }

        void resetLevelVariables() {
            LF_LEVEL_B_DIRECT(void, "GJBaseGameLayer::resetLevelVariables", static_cast<GJBaseGameLayer*>(this))
            GJBaseGameLayer::resetLevelVariables();
        }

        bool init() override {
            LF_LEVEL_B_DIRECT(bool, "GJBaseGameLayer::init", static_cast<GJBaseGameLayer*>(this))
            return GJBaseGameLayer::init();
        }

        void bumpPlayer(PlayerObject* player, EffectGameObject* object) {
            LF_LEVEL_B_DIRECT(void, "GJBaseGameLayer::bumpPlayer", static_cast<GJBaseGameLayer*>(this), player, object)
            GJBaseGameLayer::bumpPlayer(player, object);
        }

        void playerTouchedRing(PlayerObject* player, RingObject* object) {
            LF_LEVEL_B_DIRECT(void, "GJBaseGameLayer::playerTouchedRing", static_cast<GJBaseGameLayer*>(this),
                              player, object)
            GJBaseGameLayer::playerTouchedRing(player, object);
        }

        void playerTouchedTrigger(PlayerObject* player, EffectGameObject* object) {
            LF_LEVEL_B_DIRECT(void, "GJBaseGameLayer::playerTouchedTrigger", static_cast<GJBaseGameLayer*>(this),
                              player, object)
            GJBaseGameLayer::playerTouchedTrigger(player, object);
        }

        bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* object) {
            LF_LEVEL_B_DIRECT(bool, "GJBaseGameLayer::canBeActivatedByPlayer", static_cast<GJBaseGameLayer*>(this),
                              player, object)
            return GJBaseGameLayer::canBeActivatedByPlayer(player, object);
        }

        void gameEventTriggered(GJGameEvent event, int material, int playerID) {
            LF_LEVEL_B_DIRECT(void, "GJBaseGameLayer::gameEventTriggered", static_cast<GJBaseGameLayer*>(this),
                              event, material, playerID)
            GJBaseGameLayer::gameEventTriggered(event, material, playerID);
        }

        void toggleFlipped(bool flip, bool noEffects) {
            LF_LEVEL_B_DIRECT(void, "GJBaseGameLayer::toggleFlipped", static_cast<GJBaseGameLayer*>(this), flip,
                              noEffects)
            GJBaseGameLayer::toggleFlipped(flip, noEffects);
        }

        void updateGradientLayers() {
            LF_LEVEL_B_DIRECT(void, "GJBaseGameLayer::updateGradientLayers", static_cast<GJBaseGameLayer*>(this))
            GJBaseGameLayer::updateGradientLayers();
        }

        void updateShaderLayer(float dt) {
            LF_LEVEL_B_DIRECT(void, "GJBaseGameLayer::updateShaderLayer", static_cast<GJBaseGameLayer*>(this), dt)
            GJBaseGameLayer::updateShaderLayer(dt);
        }

        void updateAudioVisualizer() {
            LF_LEVEL_B_DIRECT(void, "GJBaseGameLayer::updateAudioVisualizer", static_cast<GJBaseGameLayer*>(this))
            GJBaseGameLayer::updateAudioVisualizer();
        }

        void teleportPlayer(TeleportPortalObject* object, PlayerObject* player) {
            LF_LEVEL_B_DIRECT(void, "GJBaseGameLayer::teleportPlayer", static_cast<GJBaseGameLayer*>(this), object,
                              player)
            GJBaseGameLayer::teleportPlayer(object, player);
        }

        void flipGravity(PlayerObject* player, bool flip, bool noEffects) {
            LF_LEVEL_B_DIRECT(void, "GJBaseGameLayer::flipGravity", static_cast<GJBaseGameLayer*>(this), player,
                              flip, noEffects)
            GJBaseGameLayer::flipGravity(player, flip, noEffects);
        }

        void destroyObject(GameObject* object) {
            LF_LEVEL_B_DIRECT(void, "GJBaseGameLayer::destroyObject", static_cast<GJBaseGameLayer*>(this), object)
            GJBaseGameLayer::destroyObject(object);
        }

        void playerWillSwitchMode(PlayerObject* player, GameObject* object) {
            LF_LEVEL_B_DIRECT(void, "GJBaseGameLayer::playerWillSwitchMode", static_cast<GJBaseGameLayer*>(this),
                              player, object)
            GJBaseGameLayer::playerWillSwitchMode(player, object);
        }

        void toggleDualMode(GameObject* object, bool dual, PlayerObject* player, bool noEffects) {
            LF_LEVEL_B_DIRECT(void, "GJBaseGameLayer::toggleDualMode", static_cast<GJBaseGameLayer*>(this), object,
                              dual, player, noEffects)
            GJBaseGameLayer::toggleDualMode(object, dual, player, noEffects);
        }
    };

    class $modify(LevelBIsolationPlayer, PlayerObject) {
        static void onModify(auto& self) {
            outermost(self, {
                "PlayerObject::pushButton", "PlayerObject::releaseButton", "PlayerObject::incrementJumps",
                "PlayerObject::tryPlaceCheckpoint",
                "PlayerObject::update", "PlayerObject::ringJump", "PlayerObject::collidedWithObjectInternal",
                "PlayerObject::collidedWithSlopeInternal", "PlayerObject::createRobot", "PlayerObject::createSpider",
                "PlayerObject::fadeOutStreak2", "PlayerObject::playBumpEffect", "PlayerObject::playSpawnEffect",
                "PlayerObject::playSpiderDashEffect", "PlayerObject::setupStreak", "PlayerObject::spawnCircle",
                "PlayerObject::spawnDualCircle", "PlayerObject::spawnPortalCircle", "PlayerObject::startDashing",
                "PlayerObject::stopDashing", "PlayerObject::toggleBirdMode", "PlayerObject::toggleDartMode",
                "PlayerObject::toggleFlyMode", "PlayerObject::toggleGhostEffect", "PlayerObject::togglePlayerScale",
                "PlayerObject::toggleRobotMode", "PlayerObject::toggleRollMode", "PlayerObject::toggleSpiderMode",
                "PlayerObject::toggleSwingMode", "PlayerObject::updateDashAnimation", "PlayerObject::updatePlayerFrame",
                "PlayerObject::updatePlayerBirdFrame", "PlayerObject::updatePlayerDartFrame",
                "PlayerObject::updatePlayerJetpackFrame", "PlayerObject::updatePlayerRollFrame",
                "PlayerObject::updatePlayerShipFrame", "PlayerObject::updatePlayerSwingFrame",
                "PlayerObject::updateStreakBlend", "PlayerObject::bumpPlayer", "PlayerObject::propellPlayer",
                "PlayerObject::createFadeOutDartStreak", "PlayerObject::flipGravity", "PlayerObject::switchedDirTo",
                "PlayerObject::playDeathEffect", "PlayerObject::playerDestroyed",
            });
        }

        // Presses of level B's players come only from level A's presses, which
        // call into GD directly and skip this hook.
        bool pushButton(PlayerButton button) {
            LF_LEVEL_B_SKIP(false)
            return PlayerObject::pushButton(button);
        }

        bool releaseButton(PlayerButton button) {
            LF_LEVEL_B_SKIP(false)
            return PlayerObject::releaseButton(button);
        }

        void incrementJumps() {
            LF_LEVEL_B_SKIP()
            PlayerObject::incrementJumps();
        }

        void tryPlaceCheckpoint() {
            LF_LEVEL_B_SKIP()
            PlayerObject::tryPlaceCheckpoint();
        }

        void update(float dt) override {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::update", static_cast<PlayerObject*>(this), dt)
            PlayerObject::update(dt);
        }

        void ringJump(RingObject* object, bool skipCheck) {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::ringJump", static_cast<PlayerObject*>(this), object, skipCheck)
            PlayerObject::ringJump(object, skipCheck);
        }

        bool collidedWithObjectInternal(float dt, GameObject* object, CCRect rect, bool skipCheck) {
            LF_LEVEL_B_DIRECT(bool, "PlayerObject::collidedWithObjectInternal", static_cast<PlayerObject*>(this), dt,
                              object, rect, skipCheck)
            return PlayerObject::collidedWithObjectInternal(dt, object, rect, skipCheck);
        }

        void collidedWithSlopeInternal(float dt, GameObject* object, bool forced) {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::collidedWithSlopeInternal", static_cast<PlayerObject*>(this), dt,
                              object, forced)
            PlayerObject::collidedWithSlopeInternal(dt, object, forced);
        }

        void createRobot(int frame) {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::createRobot", static_cast<PlayerObject*>(this), frame)
            PlayerObject::createRobot(frame);
        }

        void createSpider(int frame) {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::createSpider", static_cast<PlayerObject*>(this), frame)
            PlayerObject::createSpider(frame);
        }

        void fadeOutStreak2(float duration) {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::fadeOutStreak2", static_cast<PlayerObject*>(this), duration)
            PlayerObject::fadeOutStreak2(duration);
        }

        void playBumpEffect(int objectType, GameObject* object) {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::playBumpEffect", static_cast<PlayerObject*>(this), objectType,
                              object)
            PlayerObject::playBumpEffect(objectType, object);
        }

        void playSpawnEffect() {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::playSpawnEffect", static_cast<PlayerObject*>(this))
            PlayerObject::playSpawnEffect();
        }

        void playSpiderDashEffect(CCPoint from, CCPoint to) {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::playSpiderDashEffect", static_cast<PlayerObject*>(this), from, to)
            PlayerObject::playSpiderDashEffect(from, to);
        }

        void setupStreak() {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::setupStreak", static_cast<PlayerObject*>(this))
            PlayerObject::setupStreak();
        }

        void spawnCircle() {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::spawnCircle", static_cast<PlayerObject*>(this))
            PlayerObject::spawnCircle();
        }

        void spawnDualCircle() {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::spawnDualCircle", static_cast<PlayerObject*>(this))
            PlayerObject::spawnDualCircle();
        }

        void spawnPortalCircle(ccColor3B color, float startRadius) {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::spawnPortalCircle", static_cast<PlayerObject*>(this), color,
                              startRadius)
            PlayerObject::spawnPortalCircle(color, startRadius);
        }

        void startDashing(DashRingObject* object) {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::startDashing", static_cast<PlayerObject*>(this), object)
            PlayerObject::startDashing(object);
        }

        void stopDashing() {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::stopDashing", static_cast<PlayerObject*>(this))
            PlayerObject::stopDashing();
        }

        void toggleBirdMode(bool enable, bool noEffects) {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::toggleBirdMode", static_cast<PlayerObject*>(this), enable, noEffects)
            PlayerObject::toggleBirdMode(enable, noEffects);
        }

        void toggleDartMode(bool enable, bool noEffects) {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::toggleDartMode", static_cast<PlayerObject*>(this), enable, noEffects)
            PlayerObject::toggleDartMode(enable, noEffects);
        }

        void toggleFlyMode(bool enable, bool noEffects) {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::toggleFlyMode", static_cast<PlayerObject*>(this), enable, noEffects)
            PlayerObject::toggleFlyMode(enable, noEffects);
        }

        void toggleGhostEffect(GhostType type) {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::toggleGhostEffect", static_cast<PlayerObject*>(this), type)
            PlayerObject::toggleGhostEffect(type);
        }

        void togglePlayerScale(bool enable, bool noEffects) {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::togglePlayerScale", static_cast<PlayerObject*>(this), enable,
                              noEffects)
            PlayerObject::togglePlayerScale(enable, noEffects);
        }

        void toggleRobotMode(bool enable, bool noEffects) {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::toggleRobotMode", static_cast<PlayerObject*>(this), enable,
                              noEffects)
            PlayerObject::toggleRobotMode(enable, noEffects);
        }

        void toggleRollMode(bool enable, bool noEffects) {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::toggleRollMode", static_cast<PlayerObject*>(this), enable, noEffects)
            PlayerObject::toggleRollMode(enable, noEffects);
        }

        void toggleSpiderMode(bool enable, bool noEffects) {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::toggleSpiderMode", static_cast<PlayerObject*>(this), enable,
                              noEffects)
            PlayerObject::toggleSpiderMode(enable, noEffects);
        }

        void toggleSwingMode(bool enable, bool noEffects) {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::toggleSwingMode", static_cast<PlayerObject*>(this), enable,
                              noEffects)
            PlayerObject::toggleSwingMode(enable, noEffects);
        }

        void updateDashAnimation() {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::updateDashAnimation", static_cast<PlayerObject*>(this))
            PlayerObject::updateDashAnimation();
        }

        void updatePlayerFrame(int frame) {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::updatePlayerFrame", static_cast<PlayerObject*>(this), frame)
            PlayerObject::updatePlayerFrame(frame);
        }

        void updatePlayerBirdFrame(int frame) {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::updatePlayerBirdFrame", static_cast<PlayerObject*>(this), frame)
            PlayerObject::updatePlayerBirdFrame(frame);
        }

        void updatePlayerDartFrame(int frame) {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::updatePlayerDartFrame", static_cast<PlayerObject*>(this), frame)
            PlayerObject::updatePlayerDartFrame(frame);
        }

        void updatePlayerJetpackFrame(int frame) {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::updatePlayerJetpackFrame", static_cast<PlayerObject*>(this), frame)
            PlayerObject::updatePlayerJetpackFrame(frame);
        }

        void updatePlayerRollFrame(int frame) {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::updatePlayerRollFrame", static_cast<PlayerObject*>(this), frame)
            PlayerObject::updatePlayerRollFrame(frame);
        }

        void updatePlayerShipFrame(int frame) {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::updatePlayerShipFrame", static_cast<PlayerObject*>(this), frame)
            PlayerObject::updatePlayerShipFrame(frame);
        }

        void updatePlayerSwingFrame(int frame) {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::updatePlayerSwingFrame", static_cast<PlayerObject*>(this), frame)
            PlayerObject::updatePlayerSwingFrame(frame);
        }

        void updateStreakBlend(bool blend) {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::updateStreakBlend", static_cast<PlayerObject*>(this), blend)
            PlayerObject::updateStreakBlend(blend);
        }

        void bumpPlayer(float bumpMod, int objectType, bool noEffects, GameObject* object) {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::bumpPlayer", static_cast<PlayerObject*>(this), bumpMod, objectType,
                              noEffects, object)
            PlayerObject::bumpPlayer(bumpMod, objectType, noEffects, object);
        }

        void propellPlayer(float yVelocity, bool noEffects, int objectType) {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::propellPlayer", static_cast<PlayerObject*>(this), yVelocity,
                              noEffects, objectType)
            PlayerObject::propellPlayer(yVelocity, noEffects, objectType);
        }

        void createFadeOutDartStreak() {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::createFadeOutDartStreak", static_cast<PlayerObject*>(this))
            PlayerObject::createFadeOutDartStreak();
        }

        void flipGravity(bool flip, bool noEffects) {
            LF_LEVEL_B_DIRECT(void, "PlayerObject::flipGravity", static_cast<PlayerObject*>(this), flip, noEffects)
            PlayerObject::flipGravity(flip, noEffects);
        }

        bool switchedDirTo(PlayerButton button) {
            LF_LEVEL_B_DIRECT(bool, "PlayerObject::switchedDirTo", static_cast<PlayerObject*>(this), button)
            return PlayerObject::switchedDirTo(button);
        }

        void playDeathEffect() {
            LF_LEVEL_B_SKIP()
            PlayerObject::playDeathEffect();
        }

        void playerDestroyed(bool noEffects) {
            LF_LEVEL_B_SKIP()
            PlayerObject::playerDestroyed(noEffects);
        }
    };

    class $modify(LevelBIsolationEffectManager, GJEffectManager) {
        static void onModify(auto& self) {
            outermost(self, {"GJEffectManager::updateSpawnTriggers"});
        }

        void updateSpawnTriggers(float dt) {
            LF_LEVEL_B_DIRECT(void, "GJEffectManager::updateSpawnTriggers", static_cast<GJEffectManager*>(this), dt)
            GJEffectManager::updateSpawnTriggers(dt);
        }
    };

    class $modify(LevelBIsolationEffectObject, EffectGameObject) {
        static void onModify(auto& self) {
            outermost(self, {"EffectGameObject::triggerObject"});
        }

        void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) override {
            LF_LEVEL_B_DIRECT(void, "EffectGameObject::triggerObject", static_cast<EffectGameObject*>(this), layer,
                              uniqueID, remapKeys)
            EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
        }
    };

    class $modify(LevelBIsolationGameObject, GameObject) {
        static void onModify(auto& self) {
            outermost(self, {"GameObject::playDestroyObjectAnim", "GameObject::playShineEffect"});
        }

        void playDestroyObjectAnim(GJBaseGameLayer* layer) {
            LF_LEVEL_B_DIRECT(void, "GameObject::playDestroyObjectAnim", static_cast<GameObject*>(this), layer)
            GameObject::playDestroyObjectAnim(layer);
        }

        void playShineEffect() {
            LF_LEVEL_B_DIRECT(void, "GameObject::playShineEffect", static_cast<GameObject*>(this))
            GameObject::playShineEffect();
        }
    };

    class $modify(LevelBIsolationStreak, HardStreak) {
        static void onModify(auto& self) {
            outermost(self, {"HardStreak::addPoint", "HardStreak::updateStroke"});
        }

        void addPoint(CCPoint point) {
            LF_LEVEL_B_DIRECT(void, "HardStreak::addPoint", static_cast<HardStreak*>(this), point)
            HardStreak::addPoint(point);
        }

        void updateStroke(float dt) {
            LF_LEVEL_B_DIRECT(void, "HardStreak::updateStroke", static_cast<HardStreak*>(this), dt)
            HardStreak::updateStroke(dt);
        }
    };

    class $modify(LevelBIsolationCheckpoint, CheckpointObject) {
        static void onModify(auto& self) {
            outermost(self, {"CheckpointObject::init"});
        }

        bool init() override {
            LF_LEVEL_B_DIRECT(bool, "CheckpointObject::init", static_cast<CheckpointObject*>(this))
            return CheckpointObject::init();
        }
    };

    // Attempts, jumps and other totals of level B are not counted.
    class $modify(LevelBIsolationStats, GameStatsManager) {
        static void onModify(auto& self) {
            outermost(self, {"GameStatsManager::incrementStat"});
        }

        void incrementStat(char const* key, int amount) {
            if (level_overlay::inLevelB()) return;
            GameStatsManager::incrementStat(key, amount);
        }
    };
}
