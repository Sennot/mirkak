#include "hidden_objects.hpp"

#include "layout_pass.hpp"
#include "layout_renderer.hpp"
#include "object_roles.hpp"
#include "../debug.hpp"
#include "../settings.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>
#include <vector>

using namespace geode::prelude;

namespace layoutfeed::hidden {
    namespace {
        using Clock = std::chrono::steady_clock;

        constexpr int kRevealZOrder = 1 << 29;
        // The candidate list (gameplay objects in view) is rebuilt at most
        // this often; whether each candidate is hidden, and where it is, is
        // still evaluated every frame, so nothing lags behind the game.
        constexpr auto kCandidateRefresh = std::chrono::microseconds(8333);

        // Visible-section traversal, the same walk the reference clean-feed
        // mod uses for hitboxes: every object in the sections on screen,
        // whether or not GD is currently drawing it.
        template <class Callback>
        void forEachObjectOnScreen(GJBaseGameLayer* layer, Callback&& callback) {
            auto const columnCount = static_cast<int>(layer->m_sections.size());
            if (columnCount <= 0) return;

            auto const firstColumn = std::max(0, layer->m_leftSectionIndex);
            auto const lastColumn = std::min(columnCount - 1, layer->m_rightSectionIndex);
            for (int x = firstColumn; x <= lastColumn; ++x) {
                auto* column = layer->m_sections[x];
                if (!column || x >= static_cast<int>(layer->m_sectionSizes.size())) continue;
                auto* sizes = layer->m_sectionSizes[x];
                if (!sizes) continue;

                auto const rowCount = static_cast<int>(column->size());
                auto const firstRow = std::max(0, layer->m_bottomSectionIndex);
                auto const lastRow = std::min(rowCount - 1, layer->m_topSectionIndex);
                for (int y = firstRow; y <= lastRow; ++y) {
                    auto* section = column->at(y);
                    if (!section || y >= static_cast<int>(sizes->size())) continue;
                    auto const count = std::min(static_cast<int>(section->size()), sizes->at(y));
                    for (int index = 0; index < count; ++index) {
                        callback(section->at(index));
                    }
                }
            }
        }

        // The window rectangle in object-layer space (all four corners, so
        // rotated and zoomed cameras are covered), with a margin that grows
        // with the view so large objects never pop in at the edge.
        CCRect visibleArea(GJBaseGameLayer* layer) {
            auto const size = CCDirector::sharedDirector()->getWinSize();
            auto* objectLayer = layer->m_objectLayer;
            std::array<CCPoint, 4> const corners{
                objectLayer->convertToNodeSpace({0.f, 0.f}),
                objectLayer->convertToNodeSpace({size.width, 0.f}),
                objectLayer->convertToNodeSpace({0.f, size.height}),
                objectLayer->convertToNodeSpace({size.width, size.height}),
            };
            auto minX = corners[0].x, maxX = corners[0].x, minY = corners[0].y, maxY = corners[0].y;
            for (auto const& point : corners) {
                minX = std::min(minX, point.x);
                maxX = std::max(maxX, point.x);
                minY = std::min(minY, point.y);
                maxY = std::max(maxY, point.y);
            }
            auto const margin = 60.f + 0.25f * std::max(maxX - minX, maxY - minY);
            return {minX - margin, minY - margin, maxX - minX + margin * 2.f, maxY - minY + margin * 2.f};
        }

        // Properties that never change while a level runs. Most objects in
        // view are decoration or triggers; filtering them here keeps the
        // per-frame work to gameplay objects only.
        bool candidate(GameObject* object) {
            if (!object || object->m_isTrigger || object->m_isStartPos || object->m_isUIObject) return false;
            auto const type = object->m_objectType;
            if (type == GameObjectType::Decoration || type == GameObjectType::CollisionObject ||
                type == GameObjectType::EnterEffectObject) {
                return false;
            }
            return !roles::hiddenInLayout(object);
        }

        bool collapsed(CCSprite* sprite) {
            auto const& quad = sprite->m_sQuad;
            return quad.bl.vertices.x == quad.tr.vertices.x && quad.bl.vertices.y == quad.tr.vertices.y;
        }

        // Every node from the sprite up to (not including) the object layer
        // must be visible for the game to draw it.
        bool visibleChain(CCNode* node, CCNode* objectLayer) {
            for (int depth = 0; node && node != objectLayer && depth < 6; ++depth) {
                if (!node->isVisible()) return false;
                node = node->getParent();
            }
            return true;
        }

        // Whether the game (with the layout shader forcing opacity) shows this
        // part by itself. Checks the actual outcome rather than single flags:
        // a visible sprite in a hidden parent, or a batched sprite whose quad
        // GD collapsed, is not drawn either.
        bool drawnByGame(CCSprite* sprite, CCNode* objectLayer) {
            if (!sprite->getParent()) return false;
            if (!visibleChain(sprite, objectLayer)) return false;
            return !(sprite->m_pobBatchNode && collapsed(sprite));
        }

        // Composite objects (DontDraw frame, art in children) are judged by
        // their first sprite child.
        bool objectDrawnByGame(GameObject* object, CCNode* objectLayer) {
            if (object->m_isHide) return false;
            if (!object->getDontDraw()) return drawnByGame(object, objectLayer);
            if (!object->getParent() || !visibleChain(object, objectLayer)) return false;
            if (auto* children = object->getChildren()) {
                for (auto* child : CCArrayExt<CCNode*>(children)) {
                    if (auto* sprite = typeinfo_cast<CCSprite*>(child)) return drawnByGame(sprite, objectLayer);
                }
            }
            return true;
        }

        class RevealNode final : public CCNode {
        public:
            static RevealNode* create(GJBaseGameLayer* layer) {
                auto* node = new RevealNode(layer);
                if (node->init()) {
                    node->autorelease();
                    return node;
                }
                delete node;
                return nullptr;
            }

            void visit() override {
                if (!pass::active() || !settings::forceOpacity() || !m_layer) return;
                CCNode::visit();
            }

            void draw() override {
                for (auto& batch : m_batches) batch.second.clear();

                auto const area = visibleArea(m_layer);
                auto const now = Clock::now();
                if (now - m_lastRefresh >= kCandidateRefresh) {
                    m_lastRefresh = now;
                    m_candidates.clear();
                    forEachObjectOnScreen(m_layer, [&](GameObject* object) {
                        if (candidate(object)) m_candidates.push_back(object);
                    });
                }

                auto* objectLayer = m_layer->m_objectLayer;
                std::uint32_t revealed = 0;
                for (auto* object : m_candidates) {
                    if (reveal(object, objectLayer, area)) ++revealed;
                }

                for (auto& [texture, vertices] : m_batches) {
                    if (!vertices.empty()) renderer::drawRevealed(texture, vertices);
                }
                auto& counters = debug::counters();
                counters.objectsRevealed += revealed;
                counters.revealCandidates += static_cast<std::uint32_t>(m_candidates.size());
            }

            void forget() {
                m_layer = nullptr;
                m_candidates.clear();
            }

        private:
            explicit RevealNode(GJBaseGameLayer* layer) : m_layer(layer) {}

            bool reveal(GameObject* object, CCNode* objectLayer, CCRect const& area) {
                // Toggled-off groups stay hidden, as in GDH's Layout Mode.
                if (object->m_isGroupDisabled) return false;

                auto const active = object->getParent() != nullptr;
                // GD keeps every object in view active; inactive objects are
                // only revealed when they are really on screen.
                if (!active && !area.containsPoint({
                    static_cast<float>(object->m_positionX), static_cast<float>(object->m_positionY)
                })) {
                    return false;
                }

                auto const mainHidden = !objectDrawnByGame(object, objectLayer);
                auto* detail = object->m_colorSprite;
                auto const separateDetail = detail && detail->getParent() != object;
                auto const detailHidden = separateDetail && !detail->getDontDraw() &&
                    !drawnByGame(detail, objectLayer);
                if (!mainHidden && !detailHidden) return false;

                // Picked-up coins and broken blocks are hidden by gameplay, not
                // by the level design; they must not come back.
                if (mainHidden && !object->isVisible() && !object->m_isHide) {
                    auto const type = object->m_objectType;
                    if (type == GameObjectType::Collectible || type == GameObjectType::SecretCoin ||
                        type == GameObjectType::UserCoin || type == GameObjectType::Breakable) {
                        return false;
                    }
                }

                // GD does not keep the node position of objects it is not
                // drawing in sync while move triggers change their real
                // position, so revealed parts go where the game has them.
                auto const position = object->getPosition();
                auto const base = CCAffineTransformMake(
                    1.f, 0.f, 0.f, 1.f,
                    static_cast<float>(object->m_positionX) - position.x,
                    static_cast<float>(object->m_positionY) - position.y
                );
                if (mainHidden) {
                    appendSprite(object, base, roles::revealColor(object, false));
                    appendChildren(object, object, CCAffineTransformConcat(object->nodeToParentTransform(), base), 0);
                }
                // A detail sprite added beside the object in its batch follows
                // the object but is not its child.
                if (detailHidden || (mainHidden && separateDetail)) {
                    appendSprite(detail, base, roles::revealColor(object, true));
                }
                return true;
            }

            std::vector<ccV3F_C4B_T2F>& verticesFor(CCTexture2D* texture) {
                for (auto& batch : m_batches) {
                    if (batch.first == texture) return batch.second;
                }
                return m_batches.emplace_back(texture, std::vector<ccV3F_C4B_T2F>{}).second;
            }

            // Rebuilds the sprite quad in object-layer space from its local
            // transform; cocos2d keeps the texture coordinates of invisible
            // sprites, only their positions are zeroed.
            void appendSprite(CCSprite* sprite, CCAffineTransform const& parent, ccColor3B const& color) {
                // DontDraw (a RobTop addition to CCSprite) marks sprites the
                // game never shows, such as the frame of composite objects
                // like spike slopes whose art is in their children.
                if (sprite->getDontDraw()) return;
                auto* texture = sprite->getTexture();
                if (!texture) return;

                auto const transform = CCAffineTransformConcat(sprite->nodeToParentTransform(), parent);
                auto const& rect = sprite->getTextureRect();
                auto const offset = sprite->getOffsetPosition();
                auto const x1 = offset.x;
                auto const y1 = offset.y;
                auto const x2 = x1 + rect.size.width;
                auto const y2 = y1 + rect.size.height;
                if (x1 == x2 || y1 == y2) return;

                auto const corner = [&](float x, float y, ccTex2F const& uv) {
                    auto const point = CCPointApplyAffineTransform(CCPoint{x, y}, transform);
                    return ccV3F_C4B_T2F{
                        vertex3(point.x, point.y, 0.f),
                        ccc4(color.r, color.g, color.b, 255),
                        uv,
                    };
                };

                auto const& quad = sprite->m_sQuad;
                auto const bl = corner(x1, y1, quad.bl.texCoords);
                auto const br = corner(x2, y1, quad.br.texCoords);
                auto const tl = corner(x1, y2, quad.tl.texCoords);
                auto const tr = corner(x2, y2, quad.tr.texCoords);

                auto& vertices = verticesFor(texture);
                vertices.insert(vertices.end(), {bl, br, tl, tl, br, tr});
            }

            void appendChildren(GameObject* owner, CCNode* node, CCAffineTransform const& transform, int depth) {
                if (depth > 3) return;
                auto* children = node->getChildren();
                if (!children) return;

                // GD may hide every child of a hidden object individually;
                // then all of them are revealed. When some are visible, the
                // invisible ones are hidden on purpose (animation frames).
                auto anyVisible = false;
                for (auto* child : CCArrayExt<CCNode*>(children)) {
                    if (child && child->isVisible()) {
                        anyVisible = true;
                        break;
                    }
                }

                for (auto* child : CCArrayExt<CCNode*>(children)) {
                    if (!child) continue;
                    auto const detail = child == owner->m_colorSprite;
                    if (anyVisible && !detail && !child->isVisible()) continue;
                    // Non-sprite children are walked through as well: text
                    // objects keep their letters in a label batch node.
                    if (auto* sprite = typeinfo_cast<CCSprite*>(child)) {
                        appendSprite(sprite, transform, roles::revealColor(owner, detail));
                    }
                    auto const childTransform = CCAffineTransformConcat(child->nodeToParentTransform(), transform);
                    appendChildren(owner, child, childTransform, depth + 1);
                }
            }

            GJBaseGameLayer* m_layer;
            std::vector<GameObject*> m_candidates;
            Clock::time_point m_lastRefresh{};
            std::vector<std::pair<CCTexture2D*, std::vector<ccV3F_C4B_T2F>>> m_batches;
        };

        WeakRef<RevealNode> s_node;
    }

    void attach(GJBaseGameLayer* layer) {
        detach();
        if (!layer || !layer->m_objectLayer) return;
        auto* node = RevealNode::create(layer);
        if (!node) return;
        node->setID("reveal-hidden-objects"_spr);
        layer->m_objectLayer->addChild(node, kRevealZOrder);
        s_node = node;
        LF_DEBUG("hidden objects: reveal node attached to the object layer");
    }

    void detach() {
        // The node is owned by the object layer and dies with it; it only has
        // to stop touching the layer once the level is quitting.
        if (auto node = s_node.lock()) node->forget();
        s_node = nullptr;
    }
}
