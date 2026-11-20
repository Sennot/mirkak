#include "level_overlay.hpp"

#include "../debug.hpp"
#include "../overlay/player_overlay.hpp"
#include "../overlay/render_pass.hpp"
#include "../settings.hpp"

#include <Geode/cocos/platform/win32/CCGL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace layoutfeed::level_overlay {
    namespace {
        // Value of GJLevelType for created levels: PlayLayer then keeps no
        // online statistics for the private copy of level B.
        constexpr int kCreatedLevelType = 2;
        constexpr std::size_t kMaxCheckpointRemovals = 4096;

        Ref<PlayLayer> s_levelB;
        Ref<GJGameLevel> s_levelData;
        WeakRef<PlayLayer> s_levelA;
        bool s_constructing = false;
        unsigned s_depth = 0;
        unsigned s_pendingCheckpoints = 0;
        bool s_started = false;
        bool s_loggedFirstDraw = false;
        std::uint64_t s_ticks = 0;
        std::uint64_t s_draws = 0;

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

        // Position, speed and mode of one player, captured from level A
        // before its update so level B steps from the same state.
        struct PlayerState {
            bool valid = false;
            CCPoint node;
            CCPoint position;
            float rotation = 0.f;
            double yVelocity = 0.0;
            float speed = 0.f;
            bool onGround = false;
            bool upsideDown = false;
            bool mini = false;
            bool goingLeft = false;
            bool ship = false, bird = false, ball = false, dart = false;
            bool robot = false, spider = false, swing = false;
        };

        std::array<PlayerState, 2> s_players{};

        PlayerState capture(PlayerObject* player) {
            PlayerState state;
            if (!player) return state;
            state.valid = true;
            state.node = player->getPosition();
            state.position = player->m_position;
            state.rotation = player->getRotation();
            state.yVelocity = player->m_yVelocity;
            state.speed = player->m_playerSpeed;
            state.onGround = player->m_isOnGround;
            state.upsideDown = player->m_isUpsideDown;
            state.mini = player->m_vehicleSize < 1.f;
            state.goingLeft = player->m_isGoingLeft;
            state.ship = player->m_isShip;
            state.bird = player->m_isBird;
            state.ball = player->m_isBall;
            state.dart = player->m_isDart;
            state.robot = player->m_isRobot;
            state.spider = player->m_isSpider;
            state.swing = player->m_isSwing;
            return state;
        }

        // Switches level B's player to level A's game mode when level B did not
        // (a portal missing in level B, or a teleport past it).
        void applyMode(PlayerObject* player, PlayerState const& state) {
            auto const same = player->m_isShip == state.ship && player->m_isBird == state.bird &&
                player->m_isBall == state.ball && player->m_isDart == state.dart &&
                player->m_isRobot == state.robot && player->m_isSpider == state.spider &&
                player->m_isSwing == state.swing;
            if (!same) {
                if (player->m_isShip) player->toggleFlyMode(false, true);
                if (player->m_isBird) player->toggleBirdMode(false, true);
                if (player->m_isBall) player->toggleRollMode(false, true);
                if (player->m_isDart) player->toggleDartMode(false, true);
                if (player->m_isRobot) player->toggleRobotMode(false, true);
                if (player->m_isSpider) player->toggleSpiderMode(false, true);
                if (player->m_isSwing) player->toggleSwingMode(false, true);
                if (state.ship) player->toggleFlyMode(true, true);
                else if (state.bird) player->toggleBirdMode(true, true);
                else if (state.ball) player->toggleRollMode(true, true);
                else if (state.dart) player->toggleDartMode(true, true);
                else if (state.robot) player->toggleRobotMode(true, true);
                else if (state.spider) player->toggleSpiderMode(true, true);
                else if (state.swing) player->toggleSwingMode(true, true);
            }
            if (player->m_isUpsideDown != state.upsideDown) player->flipGravity(state.upsideDown, true);
            if ((player->m_vehicleSize < 1.f) != state.mini) player->togglePlayerScale(state.mini, true);
        }

        void apply(PlayerObject* player, PlayerState const& state) {
            if (!player || !state.valid) return;
            applyMode(player, state);
            player->m_playerSpeed = state.speed;
            player->m_isGoingLeft = state.goingLeft;
            player->m_yVelocity = state.yVelocity;
            player->m_isOnGround = state.onGround;
            player->setPosition(state.node);
            player->m_position = state.position;
            player->setRotation(state.rotation);
        }

        bool holding(PlayerObject* player, PlayerButton button) {
            auto const& buttons = player->m_holdingButtons;
            auto const it = buttons.find(static_cast<int>(button));
            return it != buttons.end() && it->second;
        }

        // Level B gets the held buttons of level A. Its own orbs, pads and
        // touch triggers then react as they would for the real player.
        void mirrorButtons(PlayerObject* from, PlayerObject* to) {
            if (!from || !to) return;
            for (auto const button : {PlayerButton::Jump, PlayerButton::Left, PlayerButton::Right}) {
                auto const down = holding(from, button);
                if (down == holding(to, button)) continue;
                if (down) to->pushButton(button);
                else to->releaseButton(button);
            }
        }

        bool dualBoth(PlayLayer* levelA, PlayLayer* levelB) {
            return levelA->m_gameState.m_isDualMode && levelB->m_gameState.m_isDualMode;
        }

        void clearCheckpoints(PlayLayer* levelB) {
            for (std::size_t i = 0; i < kMaxCheckpointRemovals; ++i) {
                auto* checkpoints = levelB->m_checkpointArray;
                if (!checkpoints || !checkpoints->count()) break;
                auto const before = checkpoints->count();
                levelB->removeCheckpoint(false);
                if (checkpoints->count() >= before) break;
            }
            s_pendingCheckpoints = 0;
        }

        void mirrorPractice(PlayLayer* levelA, PlayLayer* levelB) {
            levelB->m_isPracticeMode = levelA->m_isPracticeMode;
            if (!levelA->m_isPracticeMode) clearCheckpoints(levelB);
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

            apply(levelB->m_player1, capture(levelA->m_player1));
            if (dualBoth(levelA, levelB)) apply(levelB->m_player2, capture(levelA->m_player2));

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
            if (!node || !node->isVisible()) return;
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

        // What level A shows of the player: icons, trails, particles, and the
        // death and orb effects GD adds next to them in the object layer.
        std::vector<CCNode*> playerNodes(PlayLayer* levelA) {
            std::vector<CCNode*> nodes;
            auto const add = [&](CCNode* node) {
                if (node && std::find(nodes.begin(), nodes.end(), node) == nodes.end()) nodes.push_back(node);
            };
            if (auto* layer = levelA->m_objectLayer; layer && layer->getChildren()) {
                for (auto* child : CCArrayExt<CCNode*>(layer->getChildren())) {
                    if (typeinfo_cast<PlayerObject*>(child) || typeinfo_cast<CCMotionStreak*>(child) ||
                        typeinfo_cast<HardStreak*>(child) || typeinfo_cast<CCCircleWave*>(child) ||
                        typeinfo_cast<ExplodeItemNode*>(child)) {
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

    bool constructing() {
        return s_constructing;
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

        PlayLayer* levelB = nullptr;
        {
            LevelBScope scope(nullptr);
            s_constructing = true;
            levelB = PlayLayer::create(data, false, false);
            s_constructing = false;
        }
        if (!levelB) {
            notify("level B '" + name + "' failed to load");
            return;
        }
        s_levelB = levelB;
        s_levelData = data;
        log::info("Level Overlay: level B '{}' loaded ({})", name,
                  settings::levelOverlayStatic() ? "static objects" : "full simulation");
    }

    void afterLevelInit(PlayLayer* levelA, bool success) {
        if (!s_levelB) return;
        if (!success) {
            release("level A failed to load");
            return;
        }
        s_levelA = levelA;
        s_pendingCheckpoints = 0;
        s_started = false;
        s_loggedFirstDraw = false;
        s_ticks = 0;
        s_draws = 0;
        s_players = {};
    }

    void beforeUpdate(PlayLayer* levelA) {
        s_players[0] = capture(levelA->m_player1);
        s_players[1] = levelA->m_gameState.m_isDualMode ? capture(levelA->m_player2) : PlayerState{};
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

        if (settings::levelOverlayStatic()) {
            followStatically(levelA, levelB, dt);
            ++s_ticks;
            return;
        }

        if (!s_started) {
            if (!levelA->m_started) return;
            s_started = true;
            if (!levelB->m_started) levelB->startGame();
            LF_DEBUG("Level Overlay: level B started with level A");
        }

        apply(levelB->m_player1, s_players[0]);
        mirrorButtons(levelA->m_player1, levelB->m_player1);
        if (dualBoth(levelA, levelB)) {
            apply(levelB->m_player2, s_players[1]);
            mirrorButtons(levelA->m_player2, levelB->m_player2);
        }

        levelB->update(dt);

        // Level A stored a checkpoint during its update; level B stores its
        // own now that it reached the same moment.
        for (; s_pendingCheckpoints; --s_pendingCheckpoints) {
            if (auto* checkpoint = levelB->createCheckpoint()) levelB->storeCheckpoint(checkpoint);
        }
        ++s_ticks;
    }

    void onReset(PlayLayer* levelA) {
        auto* levelB = s_levelB.data();
        if (!levelB || settings::levelOverlayStatic() || !s_started) return;
        LevelBScope scope(levelB);
        mirrorPractice(levelA, levelB);
        s_pendingCheckpoints = 0;
        levelB->resetLevel();
        apply(levelB->m_player1, capture(levelA->m_player1));
        if (dualBoth(levelA, levelB)) apply(levelB->m_player2, capture(levelA->m_player2));
        LF_DEBUG("Level Overlay: level B reset with level A (practice {}, {} checkpoints)",
                 levelA->m_isPracticeMode,
                 levelB->m_checkpointArray ? levelB->m_checkpointArray->count() : 0u);
    }

    void onCheckpointStored(PlayLayer* levelA) {
        auto* levelB = s_levelB.data();
        if (!levelB || settings::levelOverlayStatic()) return;
        levelB->m_isPracticeMode = levelA->m_isPracticeMode;
        ++s_pendingCheckpoints;
    }

    void onCheckpointRemoved(PlayLayer*, bool first) {
        auto* levelB = s_levelB.data();
        if (!levelB || settings::levelOverlayStatic()) return;
        if (s_pendingCheckpoints && !first) {
            --s_pendingCheckpoints;
            return;
        }
        auto* checkpoints = levelB->m_checkpointArray;
        if (!checkpoints || !checkpoints->count()) return;
        LevelBScope scope(levelB);
        levelB->removeCheckpoint(first);
    }

    void onPracticeToggled(PlayLayer* levelA) {
        auto* levelB = s_levelB.data();
        if (!levelB) return;
        LevelBScope scope(levelB);
        mirrorPractice(levelA, levelB);
    }

    void release(char const* reason) {
        if (!s_levelB) return;
        LF_DEBUG("Level Overlay: releasing level B ({}) after {} updates and {} frames", reason, s_ticks, s_draws);
        {
            // The destructor may clear GameManager's level pointers.
            LevelBScope scope(s_levelB.data());
            s_levelB = nullptr;
        }
        s_levelData = nullptr;
        s_levelA = nullptr;
        s_pendingCheckpoints = 0;
        s_started = false;
        s_players = {};
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
                settings::levelOverlayStatic() ? "static objects" : "simulated",
                players.size(), levelB->m_shaderLayer ? "present" : "absent"
            );
        }
        debug::checkGL("level_overlay::render");
    }
}
