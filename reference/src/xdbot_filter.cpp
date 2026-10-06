#include "xdbot_filter.hpp"

#include "settings.hpp"
#include "ui_visibility.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/modify/CCScheduler.hpp>

#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <typeinfo>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;

namespace cleanfeed::xdbot_filter {
    namespace {
        enum class NodeKind { UI, Leaf, Game, Xdbot };

        struct DeferredNode {
            Ref<CCNode> node;
            WeakRef<CCNode> parent;
            WeakRef<CCScene> scene;
            kmMat4 parentTransform;
        };

        std::vector<DeferredNode> s_deferred;
        std::vector<CCNode*> s_levelContainers;
        std::vector<CCScene*> s_visitedScenes;
        std::unordered_map<std::type_info const*, NodeKind> s_types;
        uintptr_t s_moduleBegin = 0;
        uintptr_t s_moduleEnd = 0;
        unsigned int s_frameDepth = 0;
        unsigned int s_updateDepth = 0;
        bool s_localPass = false;

        bool findModule() {
            if (s_moduleBegin) return true;
            auto module = GetModuleHandleW(L"zilko.xdbot.dll");
            if (!module) return false;
            MODULEINFO info{};
            if (!K32GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info))) return false;
            s_moduleBegin = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
            s_moduleEnd = s_moduleBegin + info.SizeOfImage;
            log::info("Spout UI filter found zilko.xdbot.dll; local UI stays enabled");
            return true;
        }

        NodeKind kind(CCNode* node) {
            auto* type = &typeid(*node);
            if (auto found = s_types.find(type); found != s_types.end()) return found->second;

            // A custom popup's RTTI descriptor lives in its defining DLL. This
            // covers xDBot's own popup classes even when they have no node ID,
            // without fixed addresses, binary patches or class-name guesses.
            auto const address = reinterpret_cast<uintptr_t>(type);
            auto result = NodeKind::UI;
            if (typeinfo_cast<GJBaseGameLayer*>(node)) result = NodeKind::Game;
            else if (address >= s_moduleBegin && address < s_moduleEnd) result = NodeKind::Xdbot;
            else if (typeinfo_cast<CCSprite*>(node) || typeinfo_cast<CCSpriteBatchNode*>(node) ||
                     typeinfo_cast<CCParticleSystem*>(node) || typeinfo_cast<CCClippingNode*>(node) ||
                     typeinfo_cast<CCScrollLayerExt*>(node)) result = NodeKind::Leaf;
            s_types.emplace(type, result);
            return result;
        }

        kmMat4 matrixFor(CCAffineTransform const& transform) {
            return kmMat4{{
                transform.a, transform.b, 0.f, 0.f,
                transform.c, transform.d, 0.f, 0.f,
                0.f, 0.f, 1.f, 0.f,
                transform.tx, transform.ty, 0.f, 1.f,
            }};
        }

        void restoreVisibility() {
            for (auto const& entry : s_deferred) entry.node->setVisible(true);
        }

        void restoreFrame() {
            restoreVisibility();
            s_deferred.clear();
        }

        bool collectScene(CCScene* scene, ui_visibility::TraversalBudget& budget, unsigned depth = 0) {
            if (!scene || !scene->isVisible()) return true;
            if (depth > 8) return false;
            if (std::find(s_visitedScenes.begin(), s_visitedScenes.end(), scene) != s_visitedScenes.end()) {
                return true;
            }
            s_visitedScenes.push_back(scene);

            // Transition scenes render these separately; they are not children
            // of the transition root. Preserve their native front/back order.
            if (auto* transition = typeinfo_cast<CCTransitionScene*>(scene)) {
                auto* first = transition->m_bIsInSceneOnTop ? transition->m_pOutScene : transition->m_pInScene;
                auto* second = transition->m_bIsInSceneOnTop ? transition->m_pInScene : transition->m_pOutScene;
                if (!collectScene(first, budget, depth + 1) || !collectScene(second, budget, depth + 1)) {
                    return false;
                }
            }

            // Discover each layer in this scene, including the outgoing layer
            // during editor/play transitions after our gameplay overlay detached.
            // Reuse capacity, but keep no level pointers between traversals.
            s_levelContainers.clear();
            auto isLevelContainer = [](CCNode* node) {
                return std::find(s_levelContainers.begin(), s_levelContainers.end(), node) !=
                    s_levelContainers.end();
            };
            bool validParents = true;
            auto const complete = ui_visibility::collect<CCNode>(scene,
                [isLevelContainer](CCNode* node) {
                    // Never classify the actual level or its object container as
                    // UI, regardless of a third-party ID or subclass.
                    if (isLevelContainer(node)) return false;
                    auto const nodeKind = kind(node);
                    if (nodeKind == NodeKind::Game) {
                        s_levelContainers.push_back(static_cast<GJBaseGameLayer*>(node)->m_objectLayer);
                        return false;
                    }
                    return nodeKind == NodeKind::Xdbot ||
                        ui_visibility::isXdbotID(std::string_view(node->getID()));
                },
                [isLevelContainer](CCNode* node) {
                    return isLevelContainer(node) || kind(node) == NodeKind::Leaf;
                },
                [](CCNode* node) {
                    // In particular, do not change child zero of a CCScene.
                    // GD and other mods can use it as the main menu/list layer.
                    return CCArrayExt<CCNode*>(node->getChildren());
                },
                [&](CCNode* node) {
                    auto* parent = node->getParent();
                    if (!parent) return;
                    // nodeToWorldTransform itself follows every parent. Check
                    // that chain before invoking it on a third-party UI node.
                    if (!ui_visibility::boundedParents(parent)) {
                        validParents = false;
                        return;
                    }
                    // Retain only for this render pass; no nodes survive scene
                    // teardown through a persistent raw-pointer cache.
                    s_deferred.push_back({node, parent, scene, matrixFor(parent->nodeToWorldTransform())});
                    node->setVisible(false);
                }, budget
            );
            s_levelContainers.clear();
            return complete && validParents;
        }

        void prepareFrame(CCDirector* director) {
            if (!s_frameDepth || s_updateDepth || s_localPass) return;
            restoreFrame();
            if (!settings::enabled() || !settings::hideXdbotUI() || !findModule()) return;
            // setNextScene will prepare the new scene AFTER its onEnter hooks.
            if (director->getNextScene()) return;
            ui_visibility::TraversalBudget budget;
            s_visitedScenes.clear();
            auto const complete = collectScene(director->getRunningScene(), budget);
            s_visitedScenes.clear();
            s_levelContainers.clear();
            if (!complete) {
                restoreFrame();
                static bool warned = false;
                if (!warned) {
                    log::warn("Spout xDBot filter skipped an oversized/deep UI tree; visibility restored");
                    warned = true;
                }
            }
        }

        class FrameScope final {
        public:
            FrameScope() { ++s_frameDepth; }
            ~FrameScope() {
                if (--s_frameDepth == 0) {
                    // Also restore if another renderer skipped swapBuffers.
                    restoreFrame();
                }
            }
        };
    }

    void drawForPlayer() {
        if (s_deferred.empty() || s_localPass) return;
        struct LocalPassScope {
            LocalPassScope() { s_localPass = true; }
            ~LocalPassScope() { s_localPass = false; }
        } localPass;
        restoreVisibility();
        // drawScene has now popped its scene matrix. Use the same base matrix
        // as the ordinary player-only overlay, then apply each UI parent's
        // world transform. No rendering or GL calls run in scheduler callbacks.
        kmMat4 baseView;
        kmMat4 projection;
        kmGLGetMatrix(KM_GL_MODELVIEW, &baseView);
        kmGLGetMatrix(KM_GL_PROJECTION, &projection);
        kmGLMatrixMode(KM_GL_PROJECTION);
        kmGLPushMatrix();
        kmGLMatrixMode(KM_GL_MODELVIEW);
        kmGLPushMatrix();

        for (auto const& entry : s_deferred) {
            auto parent = entry.parent.lock();
            auto scene = entry.scene.lock();
            if (!parent || !scene || entry.node->getParent() != parent.data() ||
                !ui_visibility::visibleUnder<CCNode>(entry.node.data(), scene.data())) continue;
            kmMat4 view;
            kmMat4Multiply(&view, &baseView, &entry.parentTransform);
            kmGLMatrixMode(KM_GL_PROJECTION);
            kmGLLoadMatrix(&projection);
            kmGLMatrixMode(KM_GL_MODELVIEW);
            kmGLLoadMatrix(&view);
            entry.node->visit();
        }

        kmGLMatrixMode(KM_GL_PROJECTION);
        kmGLPopMatrix();
        kmGLMatrixMode(KM_GL_MODELVIEW);
        kmGLPopMatrix();
        s_deferred.clear();
    }

    class $modify(CleanFeedXdbotDirector, CCDirector) {
        void drawScene() {
            FrameScope frame;
            // A paused director skips the scheduler but still renders scenes.
            if (isPaused()) prepareFrame(this);
            CCDirector::drawScene();
        }

        void setNextScene() {
            if (s_frameDepth && !s_localPass) restoreFrame();
            CCDirector::setNextScene();
            prepareFrame(this);
        }
    };

    class $modify(CleanFeedXdbotScheduler, CCScheduler) {
        static void onModify(auto& self) {
            // Outermost wrapper: defer UI only after other update hooks finish.
            (void)self.setHookPriority("cocos2d::CCScheduler::update", geode::Priority::FirstPre);
        }

        void update(float dt) {
            auto* director = CCDirector::sharedDirector();
            auto const isMainScheduler = this == director->getScheduler();
            auto const prepare = s_frameDepth && !s_localPass && isMainScheduler;
            // Also covers renderers which run several updates before one frame:
            // no scheduled game logic should observe our temporary visibility.
            if (prepare) restoreFrame();
            {
                struct UpdateScope {
                    bool active;
                    explicit UpdateScope(bool value) : active(value) { if (active) ++s_updateDepth; }
                    ~UpdateScope() { if (active) --s_updateDepth; }
                } updateScope(isMainScheduler);
                CCScheduler::update(dt);
            }
            if (prepare) prepareFrame(director);
        }
    };
}
