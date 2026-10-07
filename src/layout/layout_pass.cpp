#include "layout_pass.hpp"

#include "layout_renderer.hpp"
#include "object_roles.hpp"
#include "../debug.hpp"
#include "../settings.hpp"

#include <Geode/cocos/platform/win32/CCGL.h>

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

using namespace geode::prelude;

namespace layoutfeed::pass {
    namespace {
        bool s_active = false;

        // Batch membership, rebuilt only when the layer or its batch count
        // changes. Sorted for binary search; pointers are never dereferenced.
        struct BatchSets {
            GJBaseGameLayer* layer = nullptr;
            unsigned int count = 0;
            std::vector<void const*> objects;
            std::array<void const*, 9> glow{};
            // Object batches are children of these layers; this also covers
            // batches that are not listed in m_batchNodes.
            std::array<void const*, 3> objectLayers{};
        } s_sets;

        bool s_loggedFirstPass = false;
        std::uint32_t s_levelPasses = 0;

        void rebuildSets(GJBaseGameLayer* layer) {
            auto* batches = layer->m_batchNodes;
            auto const count = batches && batches->data ? batches->data->num : 0u;
            if (s_sets.layer == layer && s_sets.count == count) return;

            s_sets.layer = layer;
            s_sets.count = count;
            s_sets.objects.assign(batches && count ? batches->data->arr : nullptr,
                                  batches && count ? batches->data->arr + count : nullptr);
            std::sort(s_sets.objects.begin(), s_sets.objects.end());
            s_sets.glow = {
                layer->m_glowLayerT4, layer->m_glowLayerT3, layer->m_glowLayerT2,
                layer->m_glowLayerT1, layer->m_glowLayerB1, layer->m_glowLayerB2,
                layer->m_glowLayerB3, layer->m_glowLayerB4, layer->m_glowLayerB5,
            };
            s_sets.objectLayers = {
                layer->m_objectLayer, layer->m_inShaderObjectLayer, layer->m_aboveShaderObjectLayer,
            };
            LF_DEBUG("layout: tracking {} level batch nodes (9 glow layers)", count);
        }

        void tintSubtree(CCNode* node, ccColor3B const& color,
                         std::vector<std::pair<Ref<CCSprite>, ccColor3B>>& saved, int depth = 0) {
            if (!node || depth > 6) return;
            if (auto* sprite = typeinfo_cast<CCSprite*>(node)) {
                saved.emplace_back(sprite, sprite->getColor());
                sprite->setColor(color);
            }
            if (auto* children = node->getChildren()) {
                for (auto* child : CCArrayExt<CCNode*>(children)) {
                    tintSubtree(child, color, saved, depth + 1);
                }
            }
        }

        // Applies every screen-only change for the duration of one layout
        // pass and restores the exact previous state afterwards, so the game
        // and the next Spout2 frame never observe it.
        class PassScope final {
        public:
            explicit PassScope(PlayLayer* layer) {
                s_active = true;

                if (auto* background = layer->m_background) {
                    m_background = background;
                    m_backgroundVisible = background->isVisible();
                    background->setVisible(false); // replaced by the clear color
                }

                if (settings::hideMiddleground()) {
                    if (auto* middleground = layer->m_middleground) {
                        m_middleground = middleground;
                        m_middlegroundVisible = middleground->isVisible();
                        middleground->setVisible(false);
                    }
                }

                auto const ground = settings::groundColor();
                auto const line = settings::lineColor();
                for (auto* groundLayer : {layer->m_groundLayer, layer->m_groundLayer2}) {
                    if (!groundLayer) continue;
                    tintSubtree(groundLayer->m_ground1Sprite, ground, m_colors);
                    tintSubtree(groundLayer->m_ground2Sprite, ground, m_colors);
                    tintSubtree(groundLayer->m_lineSprite, line, m_colors);
                }

                if (settings::alwaysShowPlayer() && !layer->m_levelEndAnimationStarted) {
                    revealPlayer(layer->m_player1);
                    if (layer->m_gameState.m_isDualMode) revealPlayer(layer->m_player2);
                }

                if (settings::disableShaders()) {
                    if (auto* shaderLayer = layer->m_shaderLayer) {
                        m_shaderLayer = shaderLayer;
                        m_usesShaders = shaderLayer->m_state.m_usesShaders;
                        shaderLayer->m_state.m_usesShaders = false;
                    }
                }
            }

            ~PassScope() {
                if (m_shaderLayer) m_shaderLayer->m_state.m_usesShaders = m_usesShaders;
                for (auto it = m_players.rbegin(); it != m_players.rend(); ++it) {
                    auto& saved = *it;
                    if (saved.opacity != 255) saved.player->setOpacity(saved.opacity);
                    if (saved.mainLayer) saved.mainLayer->setVisible(saved.mainLayerVisible);
                    saved.player->CCSprite::setVisible(saved.visible);
                }
                for (auto it = m_colors.rbegin(); it != m_colors.rend(); ++it) {
                    it->first->setColor(it->second);
                }
                if (m_middleground) m_middleground->setVisible(m_middlegroundVisible);
                if (m_background) m_background->setVisible(m_backgroundVisible);
                s_active = false;
            }

            PassScope(PassScope const&) = delete;
            PassScope& operator=(PassScope const&) = delete;

            std::size_t tintedSprites() const { return m_colors.size(); }
            bool shaderWasActive() const { return m_shaderLayer && m_usesShaders; }

        private:
            CCSprite* m_background = nullptr;
            bool m_backgroundVisible = true;
            CCNode* m_middleground = nullptr;
            bool m_middlegroundVisible = true;
            ShaderLayer* m_shaderLayer = nullptr;
            bool m_usesShaders = false;
            std::vector<std::pair<Ref<CCSprite>, ccColor3B>> m_colors;

            struct SavedPlayer {
                PlayerObject* player = nullptr;
                CCNode* mainLayer = nullptr;
                bool visible = true;
                bool mainLayerVisible = true;
                GLubyte opacity = 255;
            };
            std::vector<SavedPlayer> m_players;

            // Hide Player triggers (toggleVisibility) and player fades hide
            // the icon from the level; on the monitor it stays visible. The
            // base CCSprite::setVisible is used so PlayerObject's override
            // (trails, particles) never runs for this temporary change. A dead
            // player keeps its death effect.
            void revealPlayer(PlayerObject* player) {
                if (!player || player->m_isDead) return;
                SavedPlayer saved;
                saved.player = player;
                saved.visible = player->isVisible();
                saved.mainLayer = player->m_mainLayer;
                saved.mainLayerVisible = saved.mainLayer ? saved.mainLayer->isVisible() : true;
                saved.opacity = player->getOpacity();
                if (saved.visible && saved.mainLayerVisible && saved.opacity == 255) return;

                player->CCSprite::setVisible(true);
                if (saved.mainLayer) saved.mainLayer->setVisible(true);
                if (saved.opacity != 255) player->setOpacity(255);
                m_players.push_back(saved);
            }
        };
    }

    bool active() {
        return s_active;
    }

    BatchKind classify(CCSpriteBatchNode* batch) {
        void const* key = batch;
        if (std::find(s_sets.glow.begin(), s_sets.glow.end(), key) != s_sets.glow.end()) {
            return BatchKind::Glow;
        }
        if (std::binary_search(s_sets.objects.begin(), s_sets.objects.end(), key)) {
            return BatchKind::Object;
        }
        auto* parentNode = batch->getParent();
        void const* parent = parentNode;
        if (parent && std::find(s_sets.objectLayers.begin(), s_sets.objectLayers.end(), parent) !=
                s_sets.objectLayers.end()) {
            return BatchKind::Object;
        }
        // Letters of text objects live in a label batch owned by the object.
        if (parentNode && roles::isLevelObject(parentNode)) return BatchKind::Object;
        return BatchKind::Other;
    }

    bool shouldRender() {
        if (!settings::layoutEnabled()) return false;
        auto* layer = PlayLayer::get();
        return layer && layer->isRunning() && layer->getParent() &&
            CCDirector::sharedDirector()->getRunningScene();
    }

    void render() {
        auto* layer = PlayLayer::get();
        auto* director = CCDirector::sharedDirector();
        auto* scene = director->getRunningScene();
        if (!layer || !scene || s_active) return;

        renderer::ensureReady();
        rebuildSets(layer);

        debug::CpuScope cpu(debug::Timer::Layout);
        debug::GpuScope gpu(debug::Timer::Layout);

        PassScope scope(layer);

        GLfloat clear[4]{};
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
        auto const background = settings::backgroundColor();
        glClearColor(background.r / 255.f, background.g / 255.f, background.b / 255.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glClearColor(clear[0], clear[1], clear[2], clear[3]);

        renderer::beginPass();

        // Same matrix discipline as CCDirector::drawScene: the stack is back
        // at its base after the normal frame, so this reproduces its visit.
        kmGLMatrixMode(KM_GL_MODELVIEW);
        kmGLPushMatrix();
        scene->visit();
        if (auto* notifications = director->getNotificationNode()) notifications->visit();
        kmGLPopMatrix();

        ++debug::counters().layoutFrames;
        ++s_levelPasses;
        if (!s_loggedFirstPass) {
            s_loggedFirstPass = true;
            log::info(
                "Layout Mode active: {} level batches, renderer {}, shader layer {} (shaders {}), "
                "{} ground sprites recolored",
                s_sets.objects.size(), renderer::ready() ? "GPU" : "fallback",
                layer->m_shaderLayer ? "present" : "absent",
                scope.shaderWasActive() ? "suppressed" : "inactive", scope.tintedSprites()
            );
        }
        debug::checkGL("layout::render");
    }

    void onLevelEnter(PlayLayer* layer) {
        LF_DEBUG("layout: entering level (PlayLayer {})", static_cast<void*>(layer));
        roles::reset("PlayLayer::init");
        renderer::releaseCaches("level enter");
        s_sets = {};
        s_loggedFirstPass = false;
        s_levelPasses = 0;
    }

    void onLevelExit(PlayLayer* layer) {
        LF_DEBUG("layout: leaving level (PlayLayer {}) after {} layout frames", static_cast<void*>(layer), s_levelPasses);
        roles::logSummary();
        roles::stopCollecting();
        renderer::releaseCaches("level exit");
        s_sets = {};
    }
}
