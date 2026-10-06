#include "layout_pass.hpp"
#include "layout_renderer.hpp"
#include "object_roles.hpp"
#include "../debug.hpp"
#include "../settings.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/CCParticleSystemQuad.hpp>
#include <Geode/modify/CCSprite.hpp>
#include <Geode/modify/CCSpriteBatchNode.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/ShaderLayer.hpp>

#include <array>

using namespace geode::prelude;

// Every render hook starts with pass::active(): outside the layout re-render
// (menus, the editor and the normal frame that goes to Spout2) they forward
// to the original function after a single boolean check.
namespace layoutfeed {
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
                    if (settings::hideGlow()) {
                        ++debug::counters().glowBatchesSkipped;
                        return;
                    }
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
            if (!pass::active() || m_pobBatchNode) {
                CCSprite::draw();
                return;
            }

            auto const role = roles::resolve(this);
            if (role == roles::Role::Keep) {
                CCSprite::draw();
                return;
            }
            if (role == roles::Role::Hide) {
                ++debug::counters().spritesHidden;
                return;
            }

            auto const tint = role == roles::Role::Main ? settings::objectColor() : settings::detailColor();
            auto const forceOpacity = settings::forceOpacity();
            std::array<ccV3F_C4B_T2F*, 4> const vertices{&m_sQuad.bl, &m_sQuad.br, &m_sQuad.tl, &m_sQuad.tr};
            std::array<ccColor4B, 4> saved{};
            for (std::size_t i = 0; i < vertices.size(); ++i) {
                saved[i] = vertices[i]->colors;
                auto const alpha = forceOpacity ? 255u : static_cast<unsigned>(saved[i].a);
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
            roles::logSummary();
            return true;
        }

        void addObject(GameObject* object) {
            PlayLayer::addObject(object);
            roles::registerObject(object);
        }

        void onQuit() {
            pass::onLevelExit(this);
            PlayLayer::onQuit();
        }
    };

    class $modify(LayoutGameObject, GameObject) {
        void addGlow(gd::string frame) {
            GameObject::addGlow(frame);
            roles::registerGlow(this);
        }
    };
}
