#include "overlay.hpp"

#include "render_pass.hpp"
#include "settings.hpp"

using namespace geode::prelude;

namespace cleanfeed::overlay {
    namespace {
        class OverlayRoot final : public cocos2d::CCNode {
        public:
            static OverlayRoot* create() {
                auto* result = new OverlayRoot();
                if (result && result->init()) {
                    result->autorelease();
                    return result;
                }
                CC_SAFE_DELETE(result);
                return nullptr;
            }

            void visit() override {
                if (render::isPlayerOverlayPass()) {
                    cocos2d::CCNode::visit();
                }
            }
        };

        geode::WeakRef<cocos2d::CCNode> s_root;
        geode::WeakRef<GJBaseGameLayer> s_layer;
    }

    void attach(GJBaseGameLayer* gameLayer) {
        if (!gameLayer || !gameLayer->m_objectLayer) return;

        if (auto previousRoot = s_root.lock(); previousRoot && previousRoot->getParent()) {
            previousRoot->removeFromParent();
        }

        s_root = nullptr;
        s_layer = nullptr;

        auto* root = OverlayRoot::create();
        if (!root) return;

        root->setID("overlay-root"_spr);
        gameLayer->addChild(root, 100000);
        s_root = root;
        s_layer = gameLayer;
    }

    void detach(GJBaseGameLayer* gameLayer) {
        auto currentLayer = s_layer.lock();
        if (currentLayer && gameLayer != currentLayer.data()) return;
        s_root = nullptr;
        s_layer = nullptr;
    }

    void drawForPlayer() {
        if (!settings::showHitboxes() && !settings::showTrajectory()) return;

        // swapBuffers is global and may run once more after an editor/game
        // scene has released its nodes. Weak references make that transition
        // frame a no-op instead of dereferencing the former overlay root.
        auto root = s_root.lock();
        auto layer = s_layer.lock();
        if (!root || !layer || !root->getParent() || !layer->m_objectLayer) return;

        kmGLMatrixMode(KM_GL_MODELVIEW);
        kmGLPushMatrix();

        auto const objectLayerTransform = layer->m_objectLayer->nodeToWorldTransform();
        kmMat4 matrix{{
            objectLayerTransform.a, objectLayerTransform.b, 0.f, 0.f,
            objectLayerTransform.c, objectLayerTransform.d, 0.f, 0.f,
            0.f, 0.f, 1.f, 0.f,
            objectLayerTransform.tx, objectLayerTransform.ty, 0.f, 1.f,
        }};
        kmGLMultMatrix(&matrix);

        {
            render::PlayerOverlayPass overlayPass;
            root->visit();
        }

        kmGLPopMatrix();
    }

    cocos2d::CCNode* root() {
        return s_root.lock().data();
    }

    GJBaseGameLayer* layer() {
        return s_layer.lock().data();
    }
}
