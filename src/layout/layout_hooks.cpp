#include "hidden_objects.hpp"
#include "layout_pass.hpp"
#include "layout_renderer.hpp"
#include "object_roles.hpp"
#include "../debug.hpp"
#include "../settings.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/CCCircleWave.hpp>
#include <Geode/modify/CCNodeContainer.hpp>
#include <Geode/modify/CCParticleSystemQuad.hpp>
#include <Geode/modify/CCSprite.hpp>
#include <Geode/modify/CCSpriteBatchNode.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/ShaderLayer.hpp>

#include <algorithm>
#include <array>

using namespace geode::prelude;

// Every render hook starts with pass::active(): outside the layout re-render
// (menus, the editor and the normal frame that goes to Spout2) they forward
// to the original function after a single boolean check.
namespace layoutfeed {
    namespace {
        // Sprites drawn through CCSprite::draw (outside batch nodes), counted
        // so the special-layer containers can tell whether they drew anything.
        std::uint64_t s_spriteDraws = 0;
        unsigned s_containerDepth = 0;
    }

    class $modify(LayoutBatchNode, CCSpriteBatchNode) {
        void draw() override {
            if (!pass::active()) {
                CCSpriteBatchNode::draw();
                return;
            }

            switch (pass::classify(this)) {
                case pass::BatchKind::Other:
                    CCSpriteBatchNode::draw();
                    return;
                case pass::BatchKind::Glow:
                    // Glow sprites are hidden per sprite by their role.
                    ++debug::counters().glowBatches;
                    [[fallthrough]];
                case pass::BatchKind::Object:
                    if (!renderer::drawObjectBatch(this)) {
                        ++debug::counters().fallbackDraws;
                        CCSpriteBatchNode::draw();
                    }
                    return;
            }
        }
    };

    // Objects outside batch nodes (animated objects in the special layers)
    // draw themselves. Their quad colors are swapped for this draw only.
    class $modify(LayoutSprite, CCSprite) {
        void draw() override {
            ++s_spriteDraws;
            if (!pass::active() || m_pobBatchNode) {
                if (s_containerDepth && !m_pobBatchNode) ++debug::counters().containerSpritesNormal;
                CCSprite::draw();
                return;
            }
            if (s_containerDepth) ++debug::counters().containerSpritesLayout;

            auto const role = roles::resolve(this);
            if (role == roles::Role::Keep) {
                CCSprite::draw();
                return;
            }
            if (role == roles::Role::Hide) {
                ++debug::counters().spritesHidden;
                return;
            }

            auto const forceOpacity = settings::forceOpacity();
            if (role == roles::Role::Opaque && !forceOpacity) {
                CCSprite::draw();
                return;
            }

            auto const tint = role == roles::Role::Main ? settings::objectColor() : settings::detailColor();
            std::array<ccV3F_C4B_T2F*, 4> const vertices{&m_sQuad.bl, &m_sQuad.br, &m_sQuad.tl, &m_sQuad.tr};
            std::array<ccColor4B, 4> saved{};
            for (std::size_t i = 0; i < vertices.size(); ++i) {
                auto const original = vertices[i]->colors;
                saved[i] = original;
                if (role == roles::Role::Opaque) {
                    // Same colors at full opacity (undo premultiplication).
                    auto const unpremultiply = [&](GLubyte value) {
                        return original.a
                            ? static_cast<GLubyte>(std::min(255u, value * 255u / original.a))
                            : static_cast<GLubyte>(255);
                    };
                    vertices[i]->colors = {
                        unpremultiply(original.r), unpremultiply(original.g), unpremultiply(original.b), 255,
                    };
                    continue;
                }
                auto const alpha = forceOpacity ? 255u : static_cast<unsigned>(original.a);
                vertices[i]->colors = {
                    static_cast<GLubyte>(tint.r * alpha / 255u),
                    static_cast<GLubyte>(tint.g * alpha / 255u),
                    static_cast<GLubyte>(tint.b * alpha / 255u),
                    static_cast<GLubyte>(alpha),
                };
            }
            CCSprite::draw();
            for (std::size_t i = 0; i < vertices.size(); ++i) vertices[i]->colors = saved[i];
            ++debug::counters().spritesTinted;
        }
    };

    // GD keeps animated and rotating objects (saws, some structures) in
    // CCNodeContainer special layers with their own visit(). In the second,
    // screen-only pass of a frame that visit drew nothing, so these objects
    // vanished in Layout Mode while OBS showed them. When it draws nothing
    // although it has visible children, the children are visited the
    // standard cocos2d way, which goes through the layout roles above.
    class $modify(LayoutNodeContainer, CCNodeContainer) {
        void visit() override {
            struct Depth {
                Depth() { ++s_containerDepth; }
                ~Depth() { --s_containerDepth; }
            } depth;

            if (!pass::active()) {
                CCNodeContainer::visit();
                return;
            }

            auto const before = s_spriteDraws;
            CCNodeContainer::visit();
            if (s_spriteDraws != before || !isVisible() || !hasVisibleChild()) return;

            ++debug::counters().containerFallbacks;
            CCNode::visit();
        }

        bool hasVisibleChild() {
            auto* children = getChildren();
            if (!children) return false;
            for (auto* child : CCArrayExt<CCNode*>(children)) {
                if (child && child->isVisible()) return true;
            }
            return false;
        }
    };

    class $modify(LayoutParticles, CCParticleSystemQuad) {
        void draw() override {
            if (pass::active() && settings::hideParticles()) {
                ++debug::counters().particlesSkipped;
                return;
            }
            CCParticleSystemQuad::draw();
        }
    };

    // 2.2 shader triggers render the level through ShaderLayer. During the
    // layout pass the shader state is switched off; the normal pass (and so
    // the Spout2 feed) keeps every shader effect.
    class $modify(LayoutShaderLayer, ShaderLayer) {
        void performCalculations() {
            if (pass::active() && settings::disableShaders()) {
                m_state.m_usesShaders = false;
                return;
            }
            ShaderLayer::performCalculations();
        }

        void visit() override {
            if (!pass::active() || !settings::disableShaders()) {
                ShaderLayer::visit();
                return;
            }
            auto const usesShaders = m_state.m_usesShaders;
            m_state.m_usesShaders = false;
            ShaderLayer::visit();
            m_state.m_usesShaders = usesShaders;
        }
    };

    class $modify(LayoutPlayLayer, PlayLayer) {
        bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
            // Objects are created inside init, so start collecting first.
            pass::onLevelEnter(this);
            if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
            hidden::attach(this);
            roles::logSummary();
            return true;
        }

        void addObject(GameObject* object) {
            PlayLayer::addObject(object);
            roles::registerObject(object);
        }

        void onQuit() {
            hidden::detach();
            pass::onLevelExit(this);
            PlayLayer::onQuit();
        }
    };

    class $modify(LayoutGameObject, GameObject) {
        void addGlow(gd::string frame) {
            GameObject::addGlow(frame);
            roles::registerGlow(this);
        }

        // GD creates or replaces detail and glow sprites when an object comes
        // into view. Without this, detail sprites placed beside their object
        // in a batch kept their level colors and opacity (invisible fills in
        // invisible levels).
        void activateObject() override {
            GameObject::activateObject();
            roles::refreshParts(this);
        }
    };

    class $modify(LayoutCircleWave, CCCircleWave) {
        void draw() override {
            if (pass::active() && settings::hideEffects()) {
                ++debug::counters().effectsSkipped;
                return;
            }
            CCCircleWave::draw();
        }
    };
}
