#include "hidden_objects.hpp"

#include "layout_pass.hpp"
#include "layout_renderer.hpp"
#include "object_roles.hpp"
#include "../debug.hpp"
#include "../settings.hpp"

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

using namespace geode::prelude;

namespace layoutfeed::hidden {
    namespace {
        constexpr int kRevealZOrder = 1 << 29;

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
        // rotated and zoomed cameras are covered), with a small margin.
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
            // Large objects can reach into view from far outside it.
            auto const margin = 60.f + 0.25f * std::max(maxX - minX, maxY - minY);
            return {minX - margin, minY - margin, maxX - minX + margin * 2.f, maxY - minY + margin * 2.f};
        }

        bool revealable(GameObject* object, CCRect const& area) {
            if (!object || object->m_isTrigger || object->m_isStartPos || object->m_isUIObject ||
                object->m_isGroupDisabled) {
                // Toggled-off groups stay hidden, as in GDH's Layout Mode.
                return false;
            }

            auto const type = object->m_objectType;
            if (type == GameObjectType::Decoration || type == GameObjectType::CollisionObject ||
                type == GameObjectType::EnterEffectObject) {
                return false;
            }

            // Objects that are drawn but partly transparent are revealed by
            // the layout shader (forced opacity); geometry GD may not draw at
            // all is rebuilt here.
            auto const visible = object->isVisible();
            auto const active = object->getParent() != nullptr;
            // Opacity 0 counts only when GD also dropped the quad (cocos2d
            // collapses it to a point); a quad that still exists is revealed
            // by the layout shader in place, with the game's exact geometry.
            auto const& quad = object->m_sQuad;
            auto const collapsed = quad.bl.vertices.x == quad.tr.vertices.x &&
                quad.bl.vertices.y == quad.tr.vertices.y;
            auto const hidden = object->m_isHide || !visible || !active ||
                (object->getOpacity() == 0 && collapsed);
            if (!hidden) return false;

            // GD keeps every object in view active. An inactive object is only
            // revealed when it is really on screen; objects at the edge of the
            // visible sections switch between active and inactive and were
            // drawn twice or in old places, which flickered.
            if (!active && !area.containsPoint({
                static_cast<float>(object->m_positionX), static_cast<float>(object->m_positionY)
            })) {
                return false;
            }

            // Picked-up coins and broken blocks are hidden by gameplay, not by
            // the level design; they must not come back.
            if (!visible && !object->m_isHide &&
                (type == GameObjectType::Collectible || type == GameObjectType::SecretCoin ||
                 type == GameObjectType::UserCoin || type == GameObjectType::Breakable)) {
                return false;
            }

            return !roles::hiddenInLayout(object);
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

                std::uint32_t revealed = 0;
                auto const area = visibleArea(m_layer);
                forEachObjectOnScreen(m_layer, [&](GameObject* object) {
                    if (!revealable(object, area)) return;
                    ++revealed;
                    appendObject(object);
                });

                for (auto& [texture, vertices] : m_batches) {
                    renderer::drawRevealed(texture, vertices);
                }
                debug::counters().objectsRevealed += revealed;
            }

            void forget() { m_layer = nullptr; }

        private:
            explicit RevealNode(GJBaseGameLayer* layer) : m_layer(layer) {}

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
                // like spike slopes whose art is in their children. Drawing it
                // put a rotated placeholder over the real object.
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
                for (auto* child : CCArrayExt<CCNode*>(children)) {
                    if (!child) continue;
                    auto const detail = child == owner->m_colorSprite;
                    if (!detail && !child->isVisible()) continue;
                    // Non-sprite children are walked through as well: text
                    // objects keep their letters in a label batch node.
                    if (auto* sprite = typeinfo_cast<CCSprite*>(child)) {
                        appendSprite(sprite, transform, roles::revealColor(owner, detail));
                    }
                    auto const childTransform = CCAffineTransformConcat(child->nodeToParentTransform(), transform);
                    appendChildren(owner, child, childTransform, depth + 1);
                }
            }

            void appendObject(GameObject* object) {
                // Object batches sit at the origin of the object layer, so the
                // object's own transform is its object-layer transform. GD
                // does not keep the node position of objects it is not
                // drawing in sync while move triggers change their real
                // position, so revealed objects are shifted to where the game
                // currently has them.
                auto const position = object->getPosition();
                auto const base = CCAffineTransformMake(
                    1.f, 0.f, 0.f, 1.f,
                    static_cast<float>(object->m_positionX) - position.x,
                    static_cast<float>(object->m_positionY) - position.y
                );
                appendSprite(object, base, roles::revealColor(object, false));
                appendChildren(object, object, CCAffineTransformConcat(object->nodeToParentTransform(), base), 0);

                // A detail sprite added beside the object in its batch follows
                // the object but is not its child.
                if (auto* detail = object->m_colorSprite; detail && detail->getParent() != object) {
                    appendSprite(detail, base, roles::revealColor(object, true));
                }
            }

            GJBaseGameLayer* m_layer;
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
