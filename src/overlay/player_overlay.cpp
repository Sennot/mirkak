#include "player_overlay.hpp"

#include "render_pass.hpp"
#include "../layout/layout_pass.hpp"
#include "../settings.hpp"

using namespace geode::prelude;

namespace layoutfeed::player_overlay {
    namespace {
        constexpr int kOverlayZOrder = 1 << 30;

        class OverlayRoot final : public CCNode {
        public:
            static OverlayRoot* create() {
                auto* result = new OverlayRoot();
                if (result->init()) {
                    result->autorelease();
                    return result;
                }
                delete result;
                return nullptr;
            }

            void visit() override {
                if (pass::active() || render::isPlayerOverlayPass()) CCNode::visit();
            }
        };

        WeakRef<CCNode> s_root;
        WeakRef<GJBaseGameLayer> s_layer;
    }

    void attach(GJBaseGameLayer* gameLayer) {
        if (!gameLayer || !gameLayer->m_objectLayer) return;
        if (auto previous = s_root.lock(); previous && previous->getParent()) previous->removeFromParent();
        s_root = nullptr;
        s_layer = nullptr;

        auto* root = OverlayRoot::create();
        if (!root) return;
        root->setID("player-overlay"_spr);
        gameLayer->m_objectLayer->addChild(root, kOverlayZOrder);
        s_root = root;
        s_layer = gameLayer;
    }

    void detach(GJBaseGameLayer* gameLayer) {
        auto current = s_layer.lock();
        if (current && gameLayer != current.data()) return;
        s_root = nullptr;
        s_layer = nullptr;
    }

    CCNode* root() {
        return s_root.lock().data();
    }

    GJBaseGameLayer* layer() {
        return s_layer.lock().data();
    }

    void drawForPlayer() {
        if (!settings::showHitboxes() && !settings::showTrajectory()) return;

        // swapBuffers is global and may run once more after a level or the
        // editor released its nodes; weak references make that a no-op.
        auto root = s_root.lock();
        auto layer = s_layer.lock();
        if (!root || !layer || !root->getParent() || !layer->m_objectLayer) return;

        kmGLMatrixMode(KM_GL_MODELVIEW);
        kmGLPushMatrix();
        auto const transform = layer->m_objectLayer->nodeToWorldTransform();
        kmMat4 matrix{{
            transform.a, transform.b, 0.f, 0.f,
            transform.c, transform.d, 0.f, 0.f,
            0.f, 0.f, 1.f, 0.f,
            transform.tx, transform.ty, 0.f, 1.f,
        }};
        kmGLMultMatrix(&matrix);
        {
            render::PlayerOverlayPass overlayPass;
            root->visit();
        }
        kmGLPopMatrix();
    }
}
