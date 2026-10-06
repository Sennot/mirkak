// Physics/prediction behavior adapted from Silicate commit f183dbc5
// (GPL-3.0). Bot, replay, renderer and UI coupling were removed.
#include "trajectory.hpp"
#include "visibility.hpp"

#include "overlay.hpp"
#include "physics/collisions.hpp"
#include "physics/object.hpp"
#include "settings.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

using namespace geode::prelude;

namespace cleanfeed {
    namespace {
        uint32_t packColor(cocos2d::ccColor4F const& color) {
            auto const channel = [](float value) {
                return static_cast<uint32_t>(std::clamp(value, 0.f, 1.f) * 255.f + 0.5f);
            };
            return channel(color.r) | (channel(color.g) << 8u) |
                   (channel(color.b) << 16u) | (channel(color.a) << 24u);
        }

        void clearEffectState(EffectManagerState& state) {
            state.m_unkVecCAState.clear();
            state.m_unkVecPulseEffectAction.clear();
            state.m_unorderedMapInt_vectorPulseEffectAction.clear();
            state.m_unorderedMapInt_vectorCountTriggerAction.clear();
            state.m_unorderedSet_int1.clear();
            state.m_mapInt_Int.clear();
            state.m_unorderedMapInt_OpacityEffectAction.clear();
            state.m_vectorTouchToggleAction.clear();
            state.m_vectorCollisionTriggerAction.clear();
            state.m_vectorToggleTriggerAction.clear();
            state.m_vectorSpawnTriggerAction.clear();
            state.m_itemCountMap.clear();
            state.m_unorderedMapInt_bool.clear();
            state.m_vectorGroupCommandObject2.clear();
            state.m_unorderedMapInt_pair_double_double.clear();
            state.m_unorderedSet_int2.clear();
            state.m_timerItemMap.clear();
            state.m_unorderedMapInt_vectorTimerTriggerAction.clear();
        }

        uint64_t packPlayerFlags(PlayerObject* player) {
            if (!player) return 0;
            uint64_t flags = 0;
            unsigned int bit = 0;
            auto set = [&](bool value) {
                if (value) flags |= uint64_t{1} << bit;
                ++bit;
            };
            set(player->m_isShip);
            set(player->m_isBird);
            set(player->m_isDart);
            set(player->m_isSwing);
            set(player->m_isBall);
            set(player->m_isSpider);
            set(player->m_isRobot);
            set(player->m_isUpsideDown);
            set(player->m_isDead);
            set(player->m_isOnGround2);
            set(player->m_isSideways);
            set(player->m_isDashing);
            set(player->m_isAccelerating);
            set(player->m_holdingLeft);
            set(player->m_holdingRight);
            set(player->m_jumpBuffered);
            set(player->m_isPlatformer);
            set(player->m_isGoingLeft);
            set(player->m_maybeIsBoosted);
            return flags;
        }

        cocos2d::CCRect shrink(cocos2d::CCRect rect, float amount) {
            rect.origin.x += amount;
            rect.origin.y += amount;
            rect.size.width -= amount * 2.f;
            rect.size.height -= amount * 2.f;
            return rect;
        }

        void drawRotatedRect(
            cocos2d::CCDrawNode* node,
            cocos2d::CCRect const& rect,
            float angle,
            cocos2d::ccColor4F const& color,
            float width
        ) {
            std::array<cocos2d::CCPoint, 4> vertices = {
                cocos2d::CCPoint{rect.getMinX(), rect.getMinY()},
                cocos2d::CCPoint{rect.getMaxX(), rect.getMinY()},
                cocos2d::CCPoint{rect.getMaxX(), rect.getMaxY()},
                cocos2d::CCPoint{rect.getMinX(), rect.getMaxY()},
            };
            for (auto& vertex : vertices) {
                vertex = vertex.rotateByAngle(
                    {rect.getMidX(), rect.getMidY()},
                    -CC_DEGREES_TO_RADIANS(angle)
                );
            }
            node->drawPolygon(
                vertices.data(), static_cast<unsigned int>(vertices.size()),
                {0.f, 0.f, 0.f, 0.f}, width, color
            );
        }
    }

    Trajectory& Trajectory::get() {
        static Trajectory instance;
        return instance;
    }

    PlayerObject* Trajectory::createFakePlayer(GJBaseGameLayer* layer, std::string const& id) {
        auto* player = PlayerObject::create(1, 1, layer, layer, true);
        if (!player) return nullptr;

        // copyAttributes aliases engine-managed PlayerObject internals. The
        // original Silicate implementation deliberately retains simulation
        // players for the process lifetime so scene teardown never destroys an
        // aliased object. Releasing one here crashes in PlayerObject::~PlayerObject.
        player->retain();
        player->setPosition({0.f, 105.f});
        player->setVisible(false);
        player->setID(id);
        layer->m_objectLayer->addChild(player);
        return player;
    }

    void Trajectory::init(GJBaseGameLayer* layer) {
        auto previousLayer = m_layerLifetime.lock();
        shutdown(previousLayer.data());
        if (!layer || !overlay::root()) return;

        m_layer = layer;
        m_layerLifetime = layer;
        m_node = OverlayDrawNode::create(settings::showTrajectory);
        if (!m_node) {
            m_layer = nullptr;
            m_layerLifetime = nullptr;
            return;
        }
        m_nodeLifetime = m_node;
        m_node->setID("trajectory"_spr);
        m_node->setBlendFunc({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
        overlay::root()->addChild(m_node, 20);

        m_fakePlayer1 = createFakePlayer(layer, "trajectory-fake-player-1"_spr);
        m_fakePlayer2 = createFakePlayer(layer, "trajectory-fake-player-2"_spr);
        m_activatedObjectsP1.reserve(64);
        m_activatedObjectsP2.reserve(64);
        m_calculated = false;
        m_p1Holding = false;
        m_p2Holding = false;
    }

    void Trajectory::shutdown(GJBaseGameLayer* layer) {
        auto liveLayer = m_layerLifetime.lock();
        if (liveLayer && layer && layer != liveLayer.data()) return;

        // The editor can disappear without our LevelEditorLayer::onExit hook
        // being the final hook in a heavily modded chain. Only touch scene
        // children while their owning layer is still alive.
        if (liveLayer) {
            if (m_fakePlayer1 && m_fakePlayer1->getParent()) m_fakePlayer1->removeFromParent();
            if (m_fakePlayer2 && m_fakePlayer2->getParent()) m_fakePlayer2->removeFromParent();
            if (auto liveNode = m_nodeLifetime.lock(); liveNode && liveNode->getParent()) {
                liveNode->removeFromParent();
            }
        }

        m_layer = nullptr;
        m_layerLifetime = nullptr;
        m_node = nullptr;
        m_nodeLifetime = nullptr;
        m_fakePlayer1 = nullptr;
        m_fakePlayer2 = nullptr;
        clearActivatedObjects();
        m_calculated = false;
        m_drawing = false;
    }

    bool Trajectory::drawing() const {
        return m_drawing;
    }

    bool Trajectory::isFakePlayer(PlayerObject* player) const {
        return player && (player == m_fakePlayer1 || player == m_fakePlayer2);
    }

    PlayerObject* Trajectory::getOtherPlayer(PlayerObject* player) const {
        return player == m_fakePlayer1 ? m_fakePlayer2 : m_fakePlayer1;
    }

    bool Trajectory::hasDied(PlayerObject* player) {
        if (player == m_fakePlayer1) {
            m_deadP1 = true;
            return true;
        }
        if (player == m_fakePlayer2) {
            m_deadP2 = true;
            return true;
        }
        return false;
    }

    void Trajectory::handleButton(bool player1, bool holding) {
        if (player1) m_p1Holding = holding;
        else m_p2Holding = holding;
        m_calculated = false;
    }

    void Trajectory::rememberActivatedObject(EnhancedGameObject* object, PlayerObject* player) {
        if (!object) return;
        if (player == m_fakePlayer1) m_activatedObjectsP1.insert(reinterpret_cast<uintptr_t>(object));
        else if (player == m_fakePlayer2) m_activatedObjectsP2.insert(reinterpret_cast<uintptr_t>(object));
    }

    bool Trajectory::playerHasActivated(PlayerObject* player, EnhancedGameObject* object) const {
        if (!object || object->m_isMultiActivate) return false;
        auto const address = reinterpret_cast<uintptr_t>(object);
        if (player == m_fakePlayer1) return m_activatedObjectsP1.contains(address);
        if (player == m_fakePlayer2) return m_activatedObjectsP2.contains(address);
        return false;
    }

    bool Trajectory::realPlayerHasActivated(PlayerObject* player, EnhancedGameObject* object) const {
        if (!object || !m_layer) return false;
        if (!isFakePlayer(player)) return phys::hasBeenActivatedByPlayer(player, object);
        auto* realPlayer = player == m_fakePlayer1 ? m_layer->m_player1 : m_layer->m_player2;
        return realPlayer && phys::hasBeenActivatedByPlayer(realPlayer, object);
    }

    void Trajectory::clearActivatedObjects() {
        m_activatedObjectsP1.clear();
        m_activatedObjectsP2.clear();
    }

    Trajectory::Signature Trajectory::computeSignature(
        GJBaseGameLayer* layer,
        PredictionSettings const& predictionSettings
    ) const {
        Signature signature;
        auto const storePlayer = [](PlayerObject* player, float (&values)[7], uint64_t& flags) {
            if (!player) return;
            auto const position = player->getPosition();
            values[0] = position.x;
            values[1] = position.y;
            values[2] = player->getRotation();
            values[3] = static_cast<float>(player->m_yVelocity);
            values[4] = static_cast<float>(player->m_platformerXVelocity);
            values[5] = player->m_gravityMod;
            values[6] = player->m_vehicleSize;
            flags = packPlayerFlags(player);
        };
        storePlayer(layer->m_player1, signature.p1, signature.p1Flags);
        storePlayer(layer->m_player2, signature.p2, signature.p2Flags);

        signature.timeWarp = layer->m_gameState.m_timeWarp;
        signature.cameraZoom = layer->m_gameState.m_cameraZoom;
        signature.lineWidth = predictionSettings.lineWidth;
        signature.length = predictionSettings.length;
        signature.tps = predictionSettings.tps;
        signature.colors[0] = packColor(predictionSettings.holdColor);
        signature.colors[1] = packColor(predictionSettings.releaseColor);

        uint32_t flags = 1u;
        flags |= layer->m_gameState.m_isDualMode ? 2u : 0u;
        flags |= layer->m_isPlatformer ? 4u : 0u;
        flags |= layer->m_levelSettings->m_twoPlayerMode ? 8u : 0u;
        flags |= m_p1Holding ? 16u : 0u;
        flags |= m_p2Holding ? 32u : 0u;
        flags |= predictionSettings.highPerformance ? 64u : 0u;
        signature.boolPack = flags;
        return signature;
    }

    bool Trajectory::iterate(
        GJBaseGameLayer* layer,
        PlayerObject* player,
        int mode,
        Track& track
    ) {
        auto const previousPosition = player->getPosition();

        layer->m_gameState.m_totalTime += m_physicsDt;
        auto const timeWarp = std::max(0.001f, layer->m_gameState.m_timeWarp);
        if (timeWarp != m_cachedTimeWarp) {
            m_cachedTimeWarp = timeWarp;
            m_cachedProgressIncrement = static_cast<int>(std::round(timeWarp * 1000.f));
        }
        layer->m_gameState.m_unkDouble3 += m_physicsDt / timeWarp;
        ++layer->m_gameState.m_currentProgress;
        layer->m_gameState.m_unkUint5 += m_cachedProgressIncrement;
        player->m_totalTime += m_physicsDt;

        auto const playerSpeed = *reinterpret_cast<float*>(&layer->m_gameState.m_timeModRelated);
        if (playerSpeed != 0.f) {
            layer->m_gameState.m_timeModRelated = 0;
            layer->m_gameState.m_timeModRelated2 = 0;
            player->updateTimeMod(playerSpeed, true);
        }

        player->m_collisionLogTop->removeAllObjects();
        player->m_collisionLogBottom->removeAllObjects();
        player->m_collisionLogLeft->removeAllObjects();
        player->m_collisionLogRight->removeAllObjects();

        auto const dead = ((mode & Player1) && m_deadP1) || ((mode & Player2) && m_deadP2);
        if (dead) {
            if (track.stepCount > 1) drawHitbox(player);
            return true;
        }

        player->m_playEffects = false;
        player->update(m_playerDelta);
        player->m_unkUnused3 = player->getRotation();
        player->updateRotation(m_playerDelta);
        player->m_shipRotation = player->getPosition();

        if (layer->checkCollisions(player, m_playerDelta, false) == 1) hasDied(player);
        phys::checkSpawnObjects(layer, player);
        layer->m_effectManager->postCollisionCheck();

        auto const zoom = std::max(0.01f, layer->m_gameState.m_cameraZoom);
        if (zoom != m_cachedCameraZoom) {
            m_cachedCameraZoom = zoom;
            m_cachedLineRadius = m_trajectoryWidth / zoom;
        }
        extendTrack(track, previousPosition, player->getPosition());
        ++track.stepCount;
        return false;
    }

    void Trajectory::extendTrack(
        Track& track,
        cocos2d::CCPoint const& from,
        cocos2d::CCPoint const& to
    ) {
        auto const step = to - from;
        auto const stepLength = step.getLength();
        if (track.active && from.equals(track.end)) {
            // A zero-length step only repeats the rounded cap at track.end.
            if (stepLength == 0.f) return;
            if (!track.hasDirection) {
                track.direction = step / stepLength;
                track.hasDirection = true;
                track.end = to;
                return;
            }

            // Merge only while every point stays on the original line within a
            // tiny fraction of the line radius and keeps moving forward.
            auto const offset = to - track.start;
            auto const tolerance = m_cachedLineRadius * 0.02f;
            if (std::abs(offset.cross(track.direction)) <= tolerance &&
                offset.dot(track.direction) >= (track.end - track.start).dot(track.direction)) {
                track.end = to;
                return;
            }
        }

        flushTrack(track);
        track.start = from;
        track.end = to;
        track.active = true;
        track.hasDirection = stepLength > 0.f;
        if (track.hasDirection) track.direction = step / stepLength;
    }

    void Trajectory::flushTrack(Track& track) {
        if (track.active) m_node->drawSegment(track.start, track.end, m_cachedLineRadius, track.color);
        track.active = false;
        track.hasDirection = false;
    }

    void Trajectory::runPrediction(
        GJBaseGameLayer* layer,
        PlayerObject* player,
        PlayerObject* other,
        int mode,
        bool both
    ) {
        std::array<PlayerObject*, 2> players = {player, other};
        auto const count = both && layer->m_gameState.m_isDualMode ? 2 : 1;
        for (int index = 0; index < count; ++index) {
            auto* current = players[index];
            switch (mode & (Hold | Swift | Release)) {
                case Hold: current->pushButton(PlayerButton::Jump); break;
                case Swift:
                    current->pushButton(PlayerButton::Jump);
                    current->releaseButton(PlayerButton::Jump);
                    break;
                case Release:
                    current->releaseButton(PlayerButton::Jump);
                    current->m_jumpBuffered = false;
                    break;
                default: break;
            }

            if (mode & Left) {
                current->releaseButton(PlayerButton::Right);
                current->pushButton(PlayerButton::Left);
            } else if (mode & Right) {
                current->releaseButton(PlayerButton::Left);
                current->pushButton(PlayerButton::Right);
            }
        }

        auto const iterations = static_cast<int>(
            m_predictionSettings.length * static_cast<float>(m_predictionSettings.tps) /
            std::max(0.001f, layer->m_gameState.m_timeWarp)
        );
        auto const color = (mode & Hold)
            ? m_predictionSettings.holdColor
            : m_predictionSettings.releaseColor;
        auto inverse = color;
        inverse.r = 1.f - inverse.r;
        inverse.g = 1.f - inverse.g;
        inverse.b = 1.f - inverse.b;
        auto const playerMask = player == m_fakePlayer1 ? Player1 : Player2;
        Track playerTrack{.color = color};
        Track otherTrack{.color = inverse};
        bool playerDone = false;
        bool otherDone = false;

        // Once all active players finish, the remaining iterations do no work.
        // Check dual mode each time: a simulated portal can change it.
        for (int index = 0; index < iterations &&
            (!playerDone || (both && layer->m_gameState.m_isDualMode && !otherDone)); ++index) {
            if (!playerDone) playerDone = iterate(layer, player, mode | playerMask, playerTrack);
            if (both && layer->m_gameState.m_isDualMode && !otherDone) {
                otherDone = iterate(layer, other, mode | Player2, otherTrack);
            }
        }
        flushTrack(playerTrack);
        flushTrack(otherTrack);
    }

    void Trajectory::simulate(
        GJBaseGameLayer* layer,
        bool player1,
        int mode,
        bool clickBothPlayers,
        GJGameState const& frameGameState,
        EffectManagerState& frameEffectState,
        SavedPlayerCheckpoint const& playerCheckpoint,
        SavedPlayerCheckpoint const* otherCheckpoint
    ) {
        auto* player = player1 ? m_fakePlayer1 : m_fakePlayer2;
        auto* realPlayer = player1 ? layer->m_player1 : layer->m_player2;
        auto* other = player1 ? m_fakePlayer2 : m_fakePlayer1;
        auto* otherReal = player1 ? layer->m_player2 : layer->m_player1;
        if (!player || !realPlayer || !other) return;

        player->copyAttributes(realPlayer);
        player->m_maybeReducedEffects = true;
        playerCheckpoint.apply(player);
        player->setPosition(realPlayer->m_position);
        player->setRotation(realPlayer->getRotation());

        if (clickBothPlayers && otherReal && otherCheckpoint) {
            other->copyAttributes(otherReal);
            other->m_maybeReducedEffects = true;
            otherCheckpoint->apply(other);
            other->setPosition(otherReal->m_position);
            other->setRotation(otherReal->getRotation());
        }

        m_deadP1 = false;
        m_deadP2 = false;
        runPrediction(layer, player, other, mode, clickBothPlayers);
        player->setVisible(false);
        other->setVisible(false);

        clearActivatedObjects();
        layer->m_gameState = frameGameState;
        layer->m_effectManager->loadFromState(frameEffectState);
    }

    void Trajectory::drawHitbox(PlayerObject* player) {
        if (!player || !m_node || !m_layer) return;
        auto const zoom = std::max(0.01f, m_layer->m_gameState.m_cameraZoom);
        auto const width = settings::hitboxWidth() / zoom;
        auto const outerColor = settings::color(settings::Color::Player);
        auto const innerColor = settings::color(settings::Color::PlayerInner);
        auto const rotatedColor = settings::color(settings::Color::PlayerRotated);
        auto const outer = shrink(player->getObjectRect(), width);
        auto const inner = shrink(player->getObjectRect(0.3f, 0.3f), width);
        drawRotatedRect(m_node, outer, player->getRotation(), rotatedColor, width);
        m_node->drawRect(outer, {0.f, 0.f, 0.f, 0.f}, width, outerColor);
        m_node->drawRect(inner, {0.f, 0.f, 0.f, 0.f}, width, innerColor);
    }

    void Trajectory::handlePortal(PlayerObject* player, GameObject* object) {
        if (!isFakePlayer(player) || !object) return;
        rememberActivatedObject(static_cast<EnhancedGameObject*>(object), player);
    }

    void Trajectory::update(GJBaseGameLayer* layer) {
        if (!layer || layer != m_layer || !m_node || !m_fakePlayer1 || !m_fakePlayer2) return;
        m_fakePlayer1->setVisible(false);
        m_fakePlayer2->setVisible(false);

        if (trajectory_visibility::suppressForIdleEditor(layer, LevelEditorLayer::get(),
            [](LevelEditorLayer const& editor) { return editor.m_playbackMode == PlaybackMode::Not; })) {
            m_node->clear();
            m_node->setVisible(false);
            m_calculated = false;
            return;
        }

        if (!settings::showTrajectory()) {
            m_node->clear();
            m_node->setVisible(false);
            m_calculated = false;
            return;
        }

        m_node->setVisible(true);
        PredictionSettings const predictionSettings{
            .tps = settings::trajectoryTps(),
            .length = settings::trajectoryLength(),
            .lineWidth = settings::trajectoryWidth(),
            .highPerformance = settings::highPerformanceTrajectory(),
            .holdColor = settings::color(settings::Color::TrajectoryHold),
            .releaseColor = settings::color(settings::Color::TrajectoryRelease),
        };
        auto const signature = computeSignature(layer, predictionSettings);
        if (m_calculated && signature == m_lastSignature) return;

        m_drawing = true;
        m_node->clear();
        m_predictionSettings = predictionSettings;
        m_physicsDt = 1.f / static_cast<float>(m_predictionSettings.tps);
        m_playerDelta = m_physicsDt * 60.f;
        m_trajectoryWidth = m_predictionSettings.lineWidth;
        m_cachedTimeWarp = -1.f;
        m_cachedCameraZoom = -1.f;

        // Every hold/release branch starts from the same real frame. Capturing
        // these large states once avoids repeating container allocations and
        // hundreds of player-field copies without changing any simulated step.
        GJGameState const frameGameState = layer->m_gameState;
        EffectManagerState localEffectState;
        auto& frameEffectState = m_predictionSettings.highPerformance
            ? m_reusableEffectState
            : localEffectState;
        if (m_predictionSettings.highPerformance) clearEffectState(frameEffectState);
        layer->m_effectManager->saveToState(frameEffectState);

        std::optional<SavedPlayerCheckpoint> localPlayer1Checkpoint;
        std::optional<SavedPlayerCheckpoint> localPlayer2Checkpoint;
        SavedPlayerCheckpoint* player1Checkpoint = nullptr;
        SavedPlayerCheckpoint* player2Checkpoint = nullptr;
        if (layer->m_player1) {
            if (m_predictionSettings.highPerformance) {
                m_reusablePlayer1Checkpoint.capture(layer->m_player1);
                player1Checkpoint = &m_reusablePlayer1Checkpoint;
            } else {
                localPlayer1Checkpoint.emplace(SavedPlayerCheckpoint::create(layer->m_player1));
                player1Checkpoint = &*localPlayer1Checkpoint;
            }
        }
        if (layer->m_player2 && layer->m_gameState.m_isDualMode) {
            if (m_predictionSettings.highPerformance) {
                m_reusablePlayer2Checkpoint.capture(layer->m_player2);
                player2Checkpoint = &m_reusablePlayer2Checkpoint;
            } else {
                localPlayer2Checkpoint.emplace(SavedPlayerCheckpoint::create(layer->m_player2));
                player2Checkpoint = &*localPlayer2Checkpoint;
            }
        }

        auto const bothPlayers = !layer->m_levelSettings->m_twoPlayerMode;
        if (player1Checkpoint) {
            simulate(
                layer, true, Hold, bothPlayers,
                frameGameState, frameEffectState, *player1Checkpoint, player2Checkpoint
            );
            simulate(
                layer, true, Release, bothPlayers,
                frameGameState, frameEffectState, *player1Checkpoint, player2Checkpoint
            );
        }
        if (player2Checkpoint && layer->m_levelSettings->m_twoPlayerMode) {
            simulate(
                layer, false, Hold, false,
                frameGameState, frameEffectState, *player2Checkpoint, nullptr
            );
            simulate(
                layer, false, Release, false,
                frameGameState, frameEffectState, *player2Checkpoint, nullptr
            );
        }

        m_fakePlayer1->setVisible(false);
        m_fakePlayer2->setVisible(false);
        m_lastSignature = signature;
        m_calculated = true;
        m_drawing = false;
    }
}
