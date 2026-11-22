#include "level_overlay.hpp"

#include "../debug.hpp"
#include "../overlay/player_overlay.hpp"
#include "../overlay/render_pass.hpp"
#include "../settings.hpp"
#include "vanilla.hpp"

#include <Geode/cocos/platform/win32/CCGL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

using namespace geode::prelude;

namespace layoutfeed::level_overlay {
    namespace {
        // Value of GJLevelType for created levels: PlayLayer then keeps no
        // online statistics for the private copy of level B.
        constexpr int kCreatedLevelType = 2;
        constexpr std::size_t kMaxPendingCheckpoints = 64;

        Ref<PlayLayer> s_levelB;
        Ref<GJGameLevel> s_levelData;
        WeakRef<PlayLayer> s_levelA;
        unsigned s_depth = 0;
        // Chosen when level B is loaded; the setting applies from the next level.
        bool s_static = false;
        bool s_started = false;
        bool s_loggedFirstDraw = false;
        std::uint64_t s_ticks = 0;
        std::uint64_t s_draws = 0;
        std::uint64_t s_presses = 0;
        // Checkpoints level A created and has not stored yet, with level B's
        // checkpoint of the same moment.
        std::vector<std::pair<CheckpointObject*, Ref<CheckpointObject>>> s_checkpoints;

        // Makes GameManager report level B while its code runs (GD and mods
        // look the current level up through it) and restores level A after.
        class LevelBScope final {
        public:
            explicit LevelBScope(PlayLayer* levelB) : m_manager(GameManager::sharedState()) {
                ++s_depth;
                if (!m_manager) return;
                m_playLayer = m_manager->m_playLayer;
                m_gameLayer = m_manager->m_gameLayer;
                if (levelB) {
                    m_manager->m_playLayer = levelB;
                    m_manager->m_gameLayer = levelB;
                }
            }

            ~LevelBScope() {
                if (m_manager) {
                    m_manager->m_playLayer = m_playLayer;
                    m_manager->m_gameLayer = m_gameLayer;
                }
                --s_depth;
            }

            LevelBScope(LevelBScope const&) = delete;
            LevelBScope& operator=(LevelBScope const&) = delete;

        private:
            GameManager* m_manager = nullptr;
            PlayLayer* m_playLayer = nullptr;
            GJBaseGameLayer* m_gameLayer = nullptr;
        };

        std::string lower(std::string text) {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return text;
        }

        std::string trimmed(std::string const& text) {
            auto const first = text.find_first_not_of(" \t");
            if (first == std::string::npos) return {};
            auto const last = text.find_last_not_of(" \t");
            return text.substr(first, last - first + 1);
        }

        GJGameLevel* findLevel() {
            if (auto const id = settings::levelOverlayLevelId(); id > 0) {
                auto* level = GameLevelManager::sharedState()->getSavedLevel(id);
                if (level && level->m_levelString.size()) return level;
                log::warn(
                    "Level Overlay: level ID {} is {}", id,
                    level ? "saved but not downloaded (open it once)" : "not in your saved levels"
                );
            }

            auto const name = lower(trimmed(settings::levelOverlayLevelName()));
            if (name.empty()) return nullptr;
            auto* manager = LocalLevelManager::sharedState();
            if (!manager || !manager->m_localLevels) return nullptr;
            for (auto* level : CCArrayExt<GJGameLevel*>(manager->m_localLevels)) {
                if (level && lower(std::string(level->m_levelName)) == name) {
                    if (level->m_levelString.size()) return level;
                    log::warn("Level Overlay: created level '{}' has no level data", name);
                    return nullptr;
                }
            }
            log::warn("Level Overlay: no created level is named '{}'", name);
            return nullptr;
        }

        // Private copy: level B's PlayLayer counts attempts, jumps and
        // progress on its level, which must never reach the saved level.
        GJGameLevel* copyOf(GJGameLevel* source) {
            auto* copy = GJGameLevel::create();
            if (!copy) return nullptr;
            copy->m_levelName = source->m_levelName;
            copy->m_levelString = source->m_levelString;
            copy->m_audioTrack = source->m_audioTrack;
            copy->m_songID = source->m_songID;
            copy->m_songIDs = source->m_songIDs;
            copy->m_sfxIDs = source->m_sfxIDs;
            copy->m_twoPlayerMode = source->m_twoPlayerMode;
            copy->m_capacityString = source->m_capacityString;
            copy->m_levelLength = source->m_levelLength;
            copy->m_gameVersion = source->m_gameVersion;
            copy->m_levelVersion = source->m_levelVersion;
            copy->m_lowDetailMode = source->m_lowDetailMode;
            copy->m_levelType = static_cast<GJLevelType>(kCreatedLevelType);
            return copy;
        }

        void notify(std::string const& text) {
            log::warn("Level Overlay: {}", text);
            if (auto* notification = Notification::create("Level Overlay: " + text, NotificationIcon::Warning)) {
                notification->show();
            }
        }

        bool dualBoth(PlayLayer* levelA, PlayLayer* levelB) {
            return levelA->m_gameState.m_isDualMode && levelB->m_gameState.m_isDualMode;
        }

        bool hasCheckpoints(PlayLayer* layer) {
            return layer->m_checkpointArray && layer->m_checkpointArray->count();
        }

        // Game mode, gravity and size through GD's own switches (isolation.cpp
        // sends them straight to GD), only when level B did not switch itself.
        void matchMode(PlayerObject* from, PlayerObject* to) {
            auto const same = to->m_isShip == from->m_isShip && to->m_isBird == from->m_isBird &&
                to->m_isBall == from->m_isBall && to->m_isDart == from->m_isDart &&
                to->m_isRobot == from->m_isRobot && to->m_isSpider == from->m_isSpider &&
                to->m_isSwing == from->m_isSwing;
            if (!same) {
                if (to->m_isShip) to->toggleFlyMode(false, true);
                if (to->m_isBird) to->toggleBirdMode(false, true);
                if (to->m_isBall) to->toggleRollMode(false, true);
                if (to->m_isDart) to->toggleDartMode(false, true);
                if (to->m_isRobot) to->toggleRobotMode(false, true);
                if (to->m_isSpider) to->toggleSpiderMode(false, true);
                if (to->m_isSwing) to->toggleSwingMode(false, true);
                if (from->m_isShip) to->toggleFlyMode(true, true);
                else if (from->m_isBird) to->toggleBirdMode(true, true);
                else if (from->m_isBall) to->toggleRollMode(true, true);
                else if (from->m_isDart) to->toggleDartMode(true, true);
                else if (from->m_isRobot) to->toggleRobotMode(true, true);
                else if (from->m_isSpider) to->toggleSpiderMode(true, true);
                else if (from->m_isSwing) to->toggleSwingMode(true, true);
            }
            if (to->m_isUpsideDown != from->m_isUpsideDown) to->flipGravity(from->m_isUpsideDown, true);
            auto const mini = from->m_vehicleSize < 1.f;
            if ((to->m_vehicleSize < 1.f) != mini) to->togglePlayerScale(mini, true);
        }

        void place(PlayerObject* from, PlayerObject* to) {
            if (!from || !to) return;
            to->setPosition(from->getPosition());
            to->m_position = from->m_position;
        }

        // The state level A's player starts this update with: level B's
        // player then takes the same physics step through level B. Object
        // references (ground, slopes, rings, dash orbs) stay level B's own.
        void copyPlayer(PlayerObject* from, PlayerObject* to) {
            if (!from || !to) return;
            matchMode(from, to);
            place(from, to);
            to->setRotation(from->getRotation());
            to->m_yVelocity = from->m_yVelocity;
            to->m_fallSpeed = from->m_fallSpeed;
            to->m_platformerXVelocity = from->m_platformerXVelocity;
            to->m_isOnGround = from->m_isOnGround;
            to->m_isOnGround2 = from->m_isOnGround2;
            to->m_isOnGround3 = from->m_isOnGround3;
            to->m_isOnGround4 = from->m_isOnGround4;
            to->m_stateOnGround = from->m_stateOnGround;
            to->m_lastGroundedPos = from->m_lastGroundedPos;
            to->m_isGoingLeft = from->m_isGoingLeft;
            to->m_playerSpeed = from->m_playerSpeed;
            to->m_gravityMod = from->m_gravityMod;
            to->m_totalTime = from->m_totalTime;
            to->m_lastJumpTime = from->m_lastJumpTime;
            to->m_lastFlipTime = from->m_lastFlipTime;
            to->m_lastLandTime = from->m_lastLandTime;
            // Held buttons are normally equal already (presses are mirrored);
            // this catches anything that changed them without a press.
            if (to->m_holdingButtons != from->m_holdingButtons) to->m_holdingButtons = from->m_holdingButtons;
            to->m_holdingLeft = from->m_holdingLeft;
            to->m_holdingRight = from->m_holdingRight;
            to->m_leftPressedFirst = from->m_leftPressedFirst;
        }

        void copyPlayers(PlayLayer* levelA, PlayLayer* levelB) {
            copyPlayer(levelA->m_player1, levelB->m_player1);
            if (dualBoth(levelA, levelB)) copyPlayer(levelA->m_player2, levelB->m_player2);
        }

        void copyTransform(CCNode* from, CCNode* to) {
            to->setPosition(from->getPosition());
            to->setScaleX(from->getScaleX());
            to->setScaleY(from->getScaleY());
            to->setRotationX(from->getRotationX());
            to->setRotationY(from->getRotationY());
            to->setSkewX(from->getSkewX());
            to->setSkewY(from->getSkewY());
        }

        // Copies the transforms from a node up to its level layer (camera,
        // zoom, rotation and shake live somewhere on that chain).
        void copyChain(CCNode* from, CCNode* to, CCNode* rootA, CCNode* rootB) {
            for (int depth = 0; from && to && from != rootA && to != rootB && depth < 8; ++depth) {
                copyTransform(from, to);
                from = from->getParent();
                to = to->getParent();
            }
        }

        // Ground, background and middleground scroll through their children;
        // they are copied as long as both levels build them the same way.
        void copySubtree(CCNode* from, CCNode* to, int depth = 0) {
            if (!from || !to || depth > 3) return;
            copyTransform(from, to);
            auto* a = from->getChildren();
            auto* b = to->getChildren();
            if (!a || !b || a->count() != b->count()) return;
            for (unsigned i = 0; i < a->count(); ++i) {
                copySubtree(static_cast<CCNode*>(a->objectAtIndex(i)), static_cast<CCNode*>(b->objectAtIndex(i)), depth + 1);
            }
        }

        // Static objects only: level B is not simulated. It takes level A's
        // camera and activates the objects in view.
        void followStatically(PlayLayer* levelA, PlayLayer* levelB, float dt) {
            auto& a = levelA->m_gameState;
            auto& b = levelB->m_gameState;
            b.m_cameraPosition = a.m_cameraPosition;
            b.m_cameraLastPosition = a.m_cameraLastPosition;
            b.m_cameraDeltaPosition = a.m_cameraDeltaPosition;
            b.m_cameraStepDiff = a.m_cameraStepDiff;
            b.m_cameraOffset = a.m_cameraOffset;
            b.m_cameraZoom = a.m_cameraZoom;
            b.m_targetCameraZoom = a.m_targetCameraZoom;
            b.m_cameraAngle = a.m_cameraAngle;
            b.m_targetCameraAngle = a.m_targetCameraAngle;

            place(levelA->m_player1, levelB->m_player1);
            if (dualBoth(levelA, levelB)) place(levelA->m_player2, levelB->m_player2);

            levelB->updateVisibility(dt);

            copyChain(levelA->m_objectLayer, levelB->m_objectLayer, levelA, levelB);
            copyChain(levelA->m_inShaderObjectLayer, levelB->m_inShaderObjectLayer, levelA, levelB);
            copyChain(levelA->m_aboveShaderObjectLayer, levelB->m_aboveShaderObjectLayer, levelA, levelB);
            copySubtree(levelA->m_groundLayer, levelB->m_groundLayer);
            copySubtree(levelA->m_groundLayer2, levelB->m_groundLayer2);
            copySubtree(levelA->m_middleground, levelB->m_middleground);
            if (levelA->m_background && levelB->m_background) {
                copyTransform(levelA->m_background, levelB->m_background);
            }
        }

        // --- drawing -------------------------------------------------------

        // Hides nodes of level B for one draw and restores them after.
        class HiddenNodes final {
        public:
            void hide(CCNode* node) {
                if (!node || !node->isVisible()) return;
                m_nodes.push_back(node);
                // Base class call: PlayerObject's override also hides trails
                // and particles for good, which this temporary change must not.
                if (auto* sprite = typeinfo_cast<CCSprite*>(node)) sprite->CCSprite::setVisible(false);
                else node->CCNode::setVisible(false);
            }

            ~HiddenNodes() {
                for (auto it = m_nodes.rbegin(); it != m_nodes.rend(); ++it) {
                    if (auto* sprite = typeinfo_cast<CCSprite*>(*it)) sprite->CCSprite::setVisible(true);
                    else (*it)->CCNode::setVisible(true);
                }
            }

        private:
            std::vector<CCNode*> m_nodes;
        };

        template <class F>
        void forEachPlayerPart(PlayerObject* player, F&& callback) {
            if (!player) return;
            callback(player->m_regularTrail);
            callback(player->m_shipStreak);
            callback(player->m_waveTrail);
            callback(player->m_playerGroundParticles);
            callback(player->m_trailingParticles);
            callback(player->m_shipClickParticles);
            callback(player->m_vehicleGroundParticles);
            callback(player->m_ufoClickParticles);
            callback(player->m_robotBurstParticles);
            callback(player->m_dashParticles);
            callback(player->m_swingBurstParticles1);
            callback(player->m_swingBurstParticles2);
            callback(player);
        }

        void hideLevelBExtras(PlayLayer* levelB, HiddenNodes& hidden) {
            // Level A's players and interface are drawn instead.
            auto const hidePart = [&](CCNode* node) { hidden.hide(node); };
            forEachPlayerPart(levelB->m_player1, hidePart);
            forEachPlayerPart(levelB->m_player2, hidePart);
            hidden.hide(levelB->m_uiLayer);
            hidden.hide(levelB->m_progressBar);
            hidden.hide(levelB->m_percentageLabel);
            hidden.hide(levelB->m_attemptLabel);
            hidden.hide(levelB->m_infoLabel);
            hidden.hide(levelB->m_statusLabel);

            // Anything player-like GD or a mod put straight into level B's
            // object layers (trails, robot and spider sprites, extra players).
            for (auto* layer : {levelB->m_objectLayer, levelB->m_inShaderObjectLayer, levelB->m_aboveShaderObjectLayer}) {
                if (!layer || !layer->getChildren()) continue;
                for (auto* child : CCArrayExt<CCNode*>(layer->getChildren())) {
                    if (typeinfo_cast<PlayerObject*>(child) || typeinfo_cast<CCMotionStreak*>(child) ||
                        typeinfo_cast<HardStreak*>(child) || typeinfo_cast<GJRobotSprite*>(child) ||
                        typeinfo_cast<GJSpiderSprite*>(child)) {
                        hidden.hide(child);
                    }
                }
            }

            // Labels and other nodes mods attach to the level itself; level
            // A's own are drawn on top instead.
            if (auto* children = levelB->getChildren()) {
                for (auto* child : CCArrayExt<CCNode*>(children)) {
                    if (!child) continue;
                    auto const id = std::string_view(child->getID());
                    if (typeinfo_cast<CCLabelProtocol*>(child) || id.find('/') != std::string_view::npos) {
                        hidden.hide(child);
                    }
                }
            }
        }

        // GD draws a node only when it and every parent are visible.
        bool drawnByGame(CCNode* node, CCNode* root) {
            for (auto* up = node; up && up != root; up = up->getParent()) {
                if (!up->isVisible()) return false;
            }
            return true;
        }

        kmMat4 toMatrix(CCAffineTransform const& t) {
            return {{
                t.a, t.b, 0.f, 0.f,
                t.c, t.d, 0.f, 0.f,
                0.f, 0.f, 1.f, 0.f,
                t.tx, t.ty, 0.f, 1.f,
            }};
        }

        // Pairs of level layers: a node inside one of level A's layers is
        // moved into the same place of level B's layer, so it follows level
        // B's camera.
        struct LayerMap {
            std::array<CCNode*, 3> layersA{};
            std::array<CCAffineTransform, 3> toB{};
            std::array<bool, 3> valid{};
        };

        LayerMap buildMap(PlayLayer* levelA, PlayLayer* levelB) {
            LayerMap map;
            std::array<CCNode*, 3> const a{levelA->m_objectLayer, levelA->m_inShaderObjectLayer, levelA->m_aboveShaderObjectLayer};
            std::array<CCNode*, 3> const b{levelB->m_objectLayer, levelB->m_inShaderObjectLayer, levelB->m_aboveShaderObjectLayer};
            for (std::size_t i = 0; i < a.size(); ++i) {
                map.layersA[i] = a[i];
                if (!a[i] || !b[i]) continue;
                map.toB[i] = CCAffineTransformConcat(
                    CCAffineTransformInvert(a[i]->nodeToWorldTransform()), b[i]->nodeToWorldTransform()
                );
                map.valid[i] = true;
            }
            return map;
        }

        // Visits a node of level A where it belongs: in level B's world when
        // it lives in a level layer, at its own screen place otherwise.
        void visitFromLevelA(CCNode* node, PlayLayer* levelA, LayerMap const& map) {
            if (!node || !drawnByGame(node, levelA)) return;
            auto* parent = node->getParent();
            if (!parent) return;
            auto transform = parent->nodeToWorldTransform();
            for (auto* up = parent; up && up != levelA; up = up->getParent()) {
                auto const it = std::find(map.layersA.begin(), map.layersA.end(), up);
                if (it == map.layersA.end()) continue;
                auto const index = static_cast<std::size_t>(it - map.layersA.begin());
                if (map.valid[index]) transform = CCAffineTransformConcat(transform, map.toB[index]);
                break;
            }
            kmGLPushMatrix();
            auto matrix = toMatrix(transform);
            kmGLMultMatrix(&matrix);
            node->visit();
            kmGLPopMatrix();
        }

        bool hasAncestorIn(CCNode* node, std::vector<CCNode*> const& nodes) {
            for (auto* up = node ? node->getParent() : nullptr; up; up = up->getParent()) {
                if (std::find(nodes.begin(), nodes.end(), up) != nodes.end()) return true;
            }
            return false;
        }

        // What level A shows of its real players (not the trajectory's
        // hidden simulation players): icons, trails, particles, and the
        // death and orb effects GD adds next to them in the object layer.
        std::vector<CCNode*> playerNodes(PlayLayer* levelA) {
            std::vector<CCNode*> nodes;
            auto const add = [&](CCNode* node) {
                if (node && std::find(nodes.begin(), nodes.end(), node) == nodes.end()) nodes.push_back(node);
            };
            if (auto* layer = levelA->m_objectLayer; layer && layer->getChildren()) {
                for (auto* child : CCArrayExt<CCNode*>(layer->getChildren())) {
                    if (typeinfo_cast<CCCircleWave*>(child) || typeinfo_cast<ExplodeItemNode*>(child)) {
                        add(child);
                    }
                }
            }
            forEachPlayerPart(levelA->m_player1, add);
            if (levelA->m_gameState.m_isDualMode) forEachPlayerPart(levelA->m_player2, add);

            // A node inside another listed node is drawn by its parent.
            std::vector<CCNode*> result;
            result.reserve(nodes.size());
            for (auto* node : nodes) {
                if (!hasAncestorIn(node, nodes)) result.push_back(node);
            }
            return result;
        }
    }

    bool inLevelB() {
        return s_depth != 0;
    }

    bool isLevelA(GJBaseGameLayer const* layer) {
        if (!layer || !s_levelB) return false;
        auto levelA = s_levelA.lock();
        return levelA && static_cast<GJBaseGameLayer const*>(levelA.data()) == layer;
    }

    bool isLevelB(GJBaseGameLayer const* layer) {
        return layer && s_levelB && static_cast<GJBaseGameLayer const*>(s_levelB.data()) == layer;
    }

    bool owns(GJBaseGameLayer const* layer) {
        return isLevelB(layer);
    }

    bool owns(PlayerObject const* player) {
        auto* levelB = s_levelB.data();
        return player && levelB && (player == levelB->m_player1 || player == levelB->m_player2);
    }

    void beforeLevelInit() {
        release("entering a level");
        if (!settings::levelOverlayEnabled()) return;

        auto* source = findLevel();
        if (!source) {
            notify("level B not found, check its ID or name");
            return;
        }
        auto* data = copyOf(source);
        if (!data) return;
        auto const name = std::string(source->m_levelName);

        // Called from level A's innermost init hook: level B's init runs GD's
        // code only, and everything it calls is kept from other mods.
        PlayLayer* levelB = nullptr;
        {
            LevelBScope scope(nullptr);
            levelB = PlayLayer::create(data, false, false);
        }
        if (!levelB) {
            notify("level B '" + name + "' failed to load");
            return;
        }
        s_levelB = levelB;
        s_levelData = data;
        s_static = settings::levelOverlayStatic();
        log::info("Level Overlay: level B '{}' loaded ({})", name, s_static ? "static objects" : "full simulation");
        vanilla::logSummary();
    }

    void afterLevelInit(PlayLayer* levelA, bool success) {
        if (!s_levelB) return;
        if (!success) {
            release("level A failed to load");
            return;
        }
        s_levelA = levelA;
        s_checkpoints.clear();
        s_started = false;
        s_loggedFirstDraw = false;
        s_ticks = 0;
        s_draws = 0;
        s_presses = 0;
    }

    PlayLayer* drivenBy(GJBaseGameLayer* layer) {
        return isLevelA(layer) ? static_cast<PlayLayer*>(layer) : nullptr;
    }

    void beforeUpdate(PlayLayer* levelA) {
        auto* levelB = s_levelB.data();
        if (!levelB || s_static || !s_started || levelA->m_isPaused) return;
        LevelBScope scope(levelB);
        copyPlayers(levelA, levelB);
    }

    void afterUpdate(PlayLayer* levelA, float dt) {
        auto* levelB = s_levelB.data();
        if (!levelB) return;
        if (!settings::levelOverlayEnabled()) {
            // Turned off in the level: stop paying for the simulation.
            release("disabled in settings");
            return;
        }
        if (levelA->m_isPaused) return;

        debug::CpuScope cpu(debug::Timer::LevelBSim);
        LevelBScope scope(levelB);

        if (s_static) {
            followStatically(levelA, levelB, dt);
            ++s_ticks;
            return;
        }

        if (!s_started) {
            // Level A started without going through its startGame hook.
            if (!levelA->m_started) return;
            s_started = true;
            if (!levelB->m_started) levelB->startGame();
            copyPlayers(levelA, levelB);
            LF_DEBUG("Level Overlay: level B started late, with level A's first update");
        }

        // Nested call of the hooked function: GD's own update for level B.
        levelB->update(dt);
        ++s_ticks;
    }

    void afterStart(PlayLayer* levelA) {
        auto* levelB = s_levelB.data();
        if (!levelB || s_static || s_started) return;
        LevelBScope scope(levelB);
        s_started = true;
        if (!levelB->m_started) levelB->startGame();
        copyPlayers(levelA, levelB);
        LF_DEBUG("Level Overlay: level B started with level A");
    }

    void afterReset(PlayLayer* levelA) {
        auto* levelB = s_levelB.data();
        if (!levelB || s_static || !s_started) return;
        LevelBScope scope(levelB);
        levelB->m_isPracticeMode = levelA->m_isPracticeMode;
        s_checkpoints.clear();
        // Leaving practice mode: level A restarts from the beginning, so must B.
        if (!levelA->m_isPracticeMode && !levelA->m_isPlatformer && !hasCheckpoints(levelA) &&
            hasCheckpoints(levelB)) {
            levelB->removeAllCheckpoints();
        }
        // Nested call of the hooked function: GD's own reset for level B.
        levelB->resetLevel();
        copyPlayers(levelA, levelB);
        LF_DEBUG("Level Overlay: level B reset with level A (practice {}, {} / {} checkpoints)",
                 levelA->m_isPracticeMode,
                 levelA->m_checkpointArray ? levelA->m_checkpointArray->count() : 0u,
                 levelB->m_checkpointArray ? levelB->m_checkpointArray->count() : 0u);
    }

    void afterCreateCheckpoint(PlayLayer*, CheckpointObject* checkpoint) {
        auto* levelB = s_levelB.data();
        if (!levelB || s_static || !s_started || !checkpoint) return;
        LevelBScope scope(levelB);
        // Level B is one update behind level A at this point, so both
        // checkpoints are within a frame of each other.
        if (auto* mirrored = levelB->createCheckpoint()) {
            if (s_checkpoints.size() >= kMaxPendingCheckpoints) s_checkpoints.erase(s_checkpoints.begin());
            s_checkpoints.emplace_back(checkpoint, mirrored);
        }
    }

    void afterStoreCheckpoint(PlayLayer*, CheckpointObject* checkpoint) {
        auto* levelB = s_levelB.data();
        if (!levelB || s_static) return;
        auto const it = std::find_if(s_checkpoints.begin(), s_checkpoints.end(), [&](auto const& pair) {
            return pair.first == checkpoint;
        });
        if (it == s_checkpoints.end()) return;
        Ref<CheckpointObject> const mirrored = it->second;
        s_checkpoints.erase(it);
        LevelBScope scope(levelB);
        levelB->storeCheckpoint(mirrored.data());
    }

    void afterRemoveCheckpoint(PlayLayer*, bool first) {
        auto* levelB = s_levelB.data();
        if (!levelB || s_static || !hasCheckpoints(levelB)) return;
        LevelBScope scope(levelB);
        levelB->removeCheckpoint(first);
    }

    void afterRemoveAllCheckpoints(PlayLayer*) {
        auto* levelB = s_levelB.data();
        if (!levelB || s_static) return;
        s_checkpoints.clear();
        if (!hasCheckpoints(levelB)) return;
        LevelBScope scope(levelB);
        levelB->removeAllCheckpoints();
    }

    // GD's togglePracticeMode restarts the music, so level B only takes the
    // flag; its checkpoints go when level A's go.
    void afterPracticeToggle(PlayLayer* levelA) {
        auto* levelB = s_levelB.data();
        if (!levelB || s_static) return;
        LevelBScope scope(levelB);
        levelB->m_isPracticeMode = levelA->m_isPracticeMode;
        if (!levelA->m_isPracticeMode && !levelA->m_isPlatformer && !hasCheckpoints(levelA) &&
            hasCheckpoints(levelB)) {
            levelB->removeAllCheckpoints();
        }
    }

    void mirrorButton(PlayerObject* player, PlayerButton button, bool down) {
        auto* levelB = s_levelB.data();
        if (!levelB || s_static || !s_started) return;
        auto const index = static_cast<int>(button);
        if (index < 1 || index > 3) return; // "both directions" releases arrive as 2 and 3
        auto levelA = s_levelA.lock();
        if (!levelA) return;
        PlayerObject* target = nullptr;
        if (player == levelA->m_player1) target = levelB->m_player1;
        else if (player == levelA->m_player2) target = levelB->m_player2;
        if (!target) return;

        LevelBScope scope(levelB);
        // Nested call of the hooked function: GD's own press for level B.
        if (down) {
            target->pushButton(button);
            ++s_presses;
        } else {
            target->releaseButton(button);
        }
    }

    void placeLevelBPlayers(GJBaseGameLayer* layer) {
        auto* levelB = s_levelB.data();
        if (!levelB || s_static || static_cast<GJBaseGameLayer*>(levelB) != layer) return;
        auto levelA = s_levelA.lock();
        if (!levelA) return;
        place(levelA->m_player1, levelB->m_player1);
        if (dualBoth(levelA.data(), levelB)) place(levelA->m_player2, levelB->m_player2);
    }

    void release(char const* reason) {
        if (!s_levelB) return;
        LF_DEBUG("Level Overlay: releasing level B ({}) after {} updates, {} frames and {} mirrored presses",
                 reason, s_ticks, s_draws, s_presses);
        s_checkpoints.clear();
        {
            // The destructor may clear GameManager's level pointers.
            LevelBScope scope(s_levelB.data());
            s_levelB = nullptr;
        }
        s_levelData = nullptr;
        s_levelA = nullptr;
        s_started = false;
    }

    bool shouldRender() {
        if (!s_levelB || !settings::levelOverlayEnabled() || !settings::levelOverlayShown()) return false;
        auto levelA = s_levelA.lock();
        if (!levelA) {
            // Level A was destroyed without onQuit.
            release("level A is gone");
            return false;
        }
        if (levelA.data() != PlayLayer::get()) return false;
        return levelA->isRunning() && levelA->getParent() && CCDirector::sharedDirector()->getRunningScene();
    }

    void render() {
        auto levelA = s_levelA.lock();
        auto* levelB = s_levelB.data();
        auto* director = CCDirector::sharedDirector();
        auto* scene = director->getRunningScene();
        if (!levelA || !levelB || !scene) return;

        debug::CpuScope cpu(debug::Timer::LevelB);
        debug::GpuScope gpu(debug::Timer::LevelB);

        GLfloat clear[4]{};
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glClearColor(clear[0], clear[1], clear[2], clear[3]);

        // Same matrix discipline as CCDirector::drawScene and the layout pass.
        kmGLMatrixMode(KM_GL_MODELVIEW);
        kmGLPushMatrix();
        {
            HiddenNodes hidden;
            hideLevelBExtras(levelB, hidden);
            LevelBScope scope(levelB);
            levelB->visit();
        }

        auto const map = buildMap(levelA.data(), levelB);
        auto const players = playerNodes(levelA.data());
        for (auto* node : players) visitFromLevelA(node, levelA.data(), map);

        // Hitboxes and trajectory of level A (it is the level being played).
        if (auto* root = player_overlay::root(); root && player_overlay::layer() == levelA.data()) {
            render::PlayerOverlayPass overlayPass;
            visitFromLevelA(root, levelA.data(), map);
        }

        // Level A's interface: pause button, progress bar, percentage,
        // attempt counter.
        std::vector<CCNode*> uiNodes{
            levelA->m_uiLayer, levelA->m_progressBar, levelA->m_percentageLabel,
            levelA->m_attemptLabel, levelA->m_infoLabel, levelA->m_statusLabel,
        };
        for (auto* node : uiNodes) {
            if (node && !hasAncestorIn(node, uiNodes)) visitFromLevelA(node, levelA.data(), map);
        }

        // Pause menu, end screen and popups sit above the level in the scene.
        if (auto* children = scene->getChildren()) {
            auto above = false;
            for (auto* child : CCArrayExt<CCNode*>(children)) {
                if (child == levelA.data()) {
                    above = true;
                    continue;
                }
                if (above && child) child->visit();
            }
        }
        if (auto* notifications = director->getNotificationNode()) notifications->visit();
        kmGLPopMatrix();

        ++s_draws;
        if (!s_loggedFirstDraw) {
            s_loggedFirstDraw = true;
            log::info(
                "Level Overlay active: level B drawn on screen ({}), {} player nodes of level A on top, "
                "shader layer {}",
                s_static ? "static objects" : "simulated",
                players.size(), levelB->m_shaderLayer ? "present" : "absent"
            );
        }
        debug::checkGL("level_overlay::render");
    }
}
