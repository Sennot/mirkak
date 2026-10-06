#include <Geode/Geode.hpp>

#include <Geode/modify/CCEGLView.hpp>
#include <Geode/modify/EnhancedGameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

#include "hitboxes.hpp"
#include "overlay.hpp"
#include "physics/collisions.hpp"
#include "physics/gjbasegamelayer.hpp"
#include "physics/gravity.hpp"
#include "physics/player.hpp"
#include "present_hook.hpp"
#include "spout_sender.hpp"
#include "trajectory/trajectory.hpp"
#include "xdbot_filter.hpp"

using namespace geode::prelude;

namespace cleanfeed {
    namespace {
        void setupForLayer(GJBaseGameLayer* layer) {
            overlay::attach(layer);
            hitboxes::attach(layer);
            Trajectory::get().init(layer);
        }

        void teardownForLayer(GJBaseGameLayer* layer) {
            Trajectory::get().shutdown(layer);
            hitboxes::detach(layer);
            overlay::detach(layer);
        }

        void updateOverlays(GJBaseGameLayer* layer) {
            hitboxes::update(layer);
            Trajectory::get().update(layer);
        }

        // Sends the clean frame, then adds what only the player should see.
        void composeFrame() {
            SpoutSender::get().captureBackBuffer();
            overlay::drawForPlayer();
            xdbot_filter::drawForPlayer();
        }
    }

    class $modify(CleanFeedView, cocos2d::CCEGLView) {
        static void onModify(auto& self) {
            constexpr auto hook = "cocos2d::CCEGLView::swapBuffers";
            (void)self.setHookPriority(hook, geode::Priority::NormalPre);

            // Capture after Mega Hack has drawn its overlay so the complete
            // Mega Hack UI is present in the Spout sender in every scene.
            // Both IDs exist in current Windows distributions.
            for (auto const id : {"absolllute.hackmega", "absolllute.megahack"}) {
                if (auto* mod = Loader::get()->getInstalledMod(id)) {
                    (void)self.setHookPriorityAfterPre(hook, mod);
                }
            }
        }

        void swapBuffers() override {
            if (!present_hook::deferFrame(&composeFrame)) composeFrame();
            cocos2d::CCEGLView::swapBuffers();
        }
    };

    class $modify(CleanFeedPlayLayer, PlayLayer) {
        static void onModify(auto& self) {
            constexpr auto hook = "PlayLayer::destroyPlayer";
            (void)self.setHookPriority(hook, geode::Priority::FirstPre);

            // Fake prediction deaths must be consumed before noclip, death
            // counters and replay mods can mistake them for the real player.
            for (auto const id : {
                "absolllute.hackmega",
                "absolllute.megahack",
                "eclipse.eclipse-menu",
                "elohmrow.death_tracker",
                "zilko.xdbot"
            }) {
                if (auto* mod = Loader::get()->getInstalledMod(id)) {
                    (void)self.setHookPriorityBeforePre(hook, mod);
                }
            }
        }

        bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
            if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
            setupForLayer(this);
            return true;
        }

        void updateVisibility(float dt) override {
            PlayLayer::updateVisibility(dt);
            updateOverlays(this);
        }

        void destroyPlayer(PlayerObject* player, GameObject* object) override {
            if (Trajectory::get().hasDied(player)) return;
            PlayLayer::destroyPlayer(player, object);
        }

        void onQuit() {
            teardownForLayer(this);
            PlayLayer::onQuit();
        }
    };

    class $modify(CleanFeedEditorLayer, LevelEditorLayer) {
        bool init(GJGameLevel* level, bool unknown) {
            if (!LevelEditorLayer::init(level, unknown)) return false;
            setupForLayer(this);
            return true;
        }

        void updateVisibility(float dt) override {
            LevelEditorLayer::updateVisibility(dt);
            updateOverlays(this);
        }

        void onExit() override {
            teardownForLayer(this);
            LevelEditorLayer::onExit();
        }
    };

    class $modify(CleanFeedBaseGameLayer, GJBaseGameLayer) {
        void handleButton(bool down, int button, bool player1) {
            if (button == static_cast<int>(PlayerButton::Jump)) {
                Trajectory::get().handleButton(player1, down);
            }
            GJBaseGameLayer::handleButton(down, button, player1);
        }

        void collisionCheckObjects(
            PlayerObject* player,
            gd::vector<GameObject*>* objects,
            int length,
            float dt
        ) {
            if (Trajectory::get().isFakePlayer(player)) {
                phys::collisionCheckObjects(this, player, objects, length, dt);
                return;
            }
            GJBaseGameLayer::collisionCheckObjects(player, objects, length, dt);
        }

        void teleportPlayer(TeleportPortalObject* object, PlayerObject* player) {
            if (Trajectory::get().isFakePlayer(player)) {
                phys::teleportPlayer(this, object, player);
                return;
            }
            GJBaseGameLayer::teleportPlayer(object, player);
        }

        void flipGravity(PlayerObject* player, bool gravity, bool unknown) {
            if (Trajectory::get().isFakePlayer(player)) {
                phys::flipGravity(player, gravity);
                return;
            }
            GJBaseGameLayer::flipGravity(player, gravity, unknown);
        }

        void gameEventTriggered(GJGameEvent event, int first, int second) {
            if (!Trajectory::get().drawing()) {
                GJBaseGameLayer::gameEventTriggered(event, first, second);
            }
        }

        void destroyObject(GameObject* object) {
            if (!Trajectory::get().drawing()) {
                GJBaseGameLayer::destroyObject(object);
            }
        }
    };

    class $modify(CleanFeedPlayerObject, PlayerObject) {
        void playSpiderDashEffect(cocos2d::CCPoint from, cocos2d::CCPoint to) {
            if (!Trajectory::get().drawing()) PlayerObject::playSpiderDashEffect(from, to);
        }

        void incrementJumps() {
            if (!Trajectory::get().drawing()) PlayerObject::incrementJumps();
        }

        void ringJump(RingObject* ring, bool unknown) {
            if (Trajectory::get().isFakePlayer(this)) phys::ringJump(this, ring);
            else PlayerObject::ringJump(ring, unknown);
        }

        void bumpPlayer(float force, int objectType, bool playEffect, GameObject* object) {
            if (Trajectory::get().isFakePlayer(this)) {
                phys::bumpPlayer(this, force, objectType, playEffect, object);
            } else {
                PlayerObject::bumpPlayer(force, objectType, playEffect, object);
            }
        }

        void propellPlayer(float force, bool dontPlayEffect, int objectType) {
            if (Trajectory::get().isFakePlayer(this)) {
                phys::propellPlayer(this, force, dontPlayEffect, objectType);
            } else {
                PlayerObject::propellPlayer(force, dontPlayEffect, objectType);
            }
        }

        void startDashing(DashRingObject* object) {
            if (Trajectory::get().isFakePlayer(this)) phys::startDashing(this, object);
            else PlayerObject::startDashing(object);
        }

        void stopDashing() {
            if (Trajectory::get().isFakePlayer(this)) phys::stopDashing(this);
            else PlayerObject::stopDashing();
        }
    };

    class $modify(CleanFeedEnhancedGameObject, EnhancedGameObject) {
        void activatedByPlayer(PlayerObject* player) {
            if (Trajectory::get().isFakePlayer(player)) {
                phys::activateForTrajectory(reinterpret_cast<EffectGameObject*>(this), player);
            } else {
                EnhancedGameObject::activatedByPlayer(player);
            }
        }
    };

    class $modify(CleanFeedHardStreak, HardStreak) {
        void addPoint(cocos2d::CCPoint point) {
            if (!Trajectory::get().drawing()) HardStreak::addPoint(point);
        }
    };
}

$on_mod(Loaded) {
    geode::log::info(
        "Spout2 Clean Feed loaded; sender captures all scenes and overlays are level-only"
    );
}
