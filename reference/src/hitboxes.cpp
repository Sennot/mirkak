// Hitbox categories and visible-section traversal were informed by Eclipse Menu
// (EPL-2.0); this implementation is purpose-built for the shared overlay root.
#include "hitboxes.hpp"

#include "overlay.hpp"
#include "overlay_draw_node.hpp"
#include "settings.hpp"

#include <algorithm>
#include <array>
#include <chrono>

using namespace geode::prelude;

namespace cleanfeed::hitboxes {
    namespace {
        using Clock = std::chrono::steady_clock;

        cocos2d::CCDrawNode* s_objectNode = nullptr;
        cocos2d::CCDrawNode* s_playerNode = nullptr;
        GJBaseGameLayer* s_layer = nullptr;
        Clock::time_point s_nextObjectRefresh{};
        std::array<int, 4> s_lastView{-1, -1, -1, -1};
        bool s_objectsDirty = true;
        bool s_wasEnabled = false;
        // Object geometry is built for the camera view plus a margin and is
        // rebuilt as soon as the live view leaves that area, so culling never
        // hides a hitbox that is on screen.
        cocos2d::CCRect s_builtArea;

        struct Palette {
            cocos2d::ccColor4F solid;
            cocos2d::ccColor4F hazard;
            cocos2d::ccColor4F interactable;
            cocos2d::ccColor4F solidFill;
            cocos2d::ccColor4F hazardFill;
            cocos2d::ccColor4F interactableFill;
        };

        struct PlayerPalette {
            cocos2d::ccColor4F outer;
            cocos2d::ccColor4F inner;
            cocos2d::ccColor4F rotated;
            cocos2d::ccColor4F outerFill;
            cocos2d::ccColor4F innerFill;
            cocos2d::ccColor4F rotatedFill;
        };

        cocos2d::ccColor4F withFillAlpha(cocos2d::ccColor4F color, float fillAlpha) {
            color.a *= fillAlpha;
            return color;
        }

        bool contains(cocos2d::CCRect const& outer, cocos2d::CCRect const& inner) {
            return inner.getMinX() >= outer.getMinX() && inner.getMaxX() <= outer.getMaxX() &&
                   inner.getMinY() >= outer.getMinY() && inner.getMaxY() <= outer.getMaxY();
        }

        bool inBuiltArea(cocos2d::CCRect const& bounds) {
            return bounds.getMaxX() >= s_builtArea.getMinX() &&
                   bounds.getMinX() <= s_builtArea.getMaxX() &&
                   bounds.getMaxY() >= s_builtArea.getMinY() &&
                   bounds.getMinY() <= s_builtArea.getMaxY();
        }

        template <class Points>
        cocos2d::CCRect boundsOf(Points const& points) {
            auto minX = points[0].x, maxX = points[0].x, minY = points[0].y, maxY = points[0].y;
            for (auto const& point : points) {
                minX = std::min(minX, point.x);
                maxX = std::max(maxX, point.x);
                minY = std::min(minY, point.y);
                maxY = std::max(maxY, point.y);
            }
            return {minX, minY, maxX - minX, maxY - minY};
        }

        // The window rectangle in object-layer space. Converting all four
        // corners keeps the bounds correct for rotated and zoomed cameras.
        cocos2d::CCRect visibleArea(GJBaseGameLayer* layer) {
            auto const size = cocos2d::CCDirector::sharedDirector()->getWinSize();
            auto* objectLayer = layer->m_objectLayer;
            return boundsOf(std::array{
                objectLayer->convertToNodeSpace({0.f, 0.f}),
                objectLayer->convertToNodeSpace({size.width, 0.f}),
                objectLayer->convertToNodeSpace({0.f, size.height}),
                objectLayer->convertToNodeSpace({size.width, size.height}),
            });
        }

        cocos2d::CCRect withMargin(cocos2d::CCRect rect) {
            auto const margin = 30.f + 0.2f * std::max(rect.size.width, rect.size.height);
            rect.origin.x -= margin;
            rect.origin.y -= margin;
            rect.size.width += margin * 2.f;
            rect.size.height += margin * 2.f;
            return rect;
        }

        void drawRect(
            cocos2d::CCDrawNode* node,
            cocos2d::CCRect const& rect,
            cocos2d::ccColor4F const& fill,
            float width,
            cocos2d::ccColor4F const& border
        ) {
            std::array<cocos2d::CCPoint, 4> vertices = {
                cocos2d::CCPoint{rect.getMinX(), rect.getMinY()},
                cocos2d::CCPoint{rect.getMinX(), rect.getMaxY()},
                cocos2d::CCPoint{rect.getMaxX(), rect.getMaxY()},
                cocos2d::CCPoint{rect.getMaxX(), rect.getMinY()},
            };
            node->drawPolygon(
                vertices.data(), static_cast<unsigned int>(vertices.size()), fill, width, border
            );
        }

        void drawObject(GJBaseGameLayer* layer, GameObject* object, float width, Palette const& palette) {
            if (!object || object->m_objectType == GameObjectType::Decoration ||
                !object->m_isActivated || object->m_isGroupDisabled) {
                return;
            }

            switch (object->m_objectType) {
                case GameObjectType::CollisionObject:
                case GameObjectType::Decoration:
                    return;

                case GameObjectType::Solid: {
                    auto const rect = object->getObjectRect();
                    if (!inBuiltArea(rect)) return;
                    drawRect(s_objectNode, rect, palette.solidFill, width, palette.solid);
                    return;
                }

                case GameObjectType::Slope: {
                    auto const rect = object->getObjectRect();
                    if (!inBuiltArea(rect)) return;
                    std::array<cocos2d::CCPoint, 3> vertices = {
                        cocos2d::CCPoint{rect.getMinX(), rect.getMinY()},
                        cocos2d::CCPoint{rect.getMinX(), rect.getMaxY()},
                        cocos2d::CCPoint{rect.getMaxX(), rect.getMinY()},
                    };
                    auto const topRight = cocos2d::CCPoint{rect.getMaxX(), rect.getMaxY()};
                    switch (object->m_slopeDirection) {
                        case 0:
                        case 7: vertices[1] = topRight; break;
                        case 1:
                        case 5: vertices[0] = topRight; break;
                        case 3:
                        case 6: vertices[2] = topRight; break;
                        default: break;
                    }
                    s_objectNode->drawPolygon(
                        vertices.data(), static_cast<unsigned int>(vertices.size()),
                        palette.solidFill, width, palette.solid
                    );
                    return;
                }

                case GameObjectType::AnimatedHazard:
                case GameObjectType::Hazard: {
                    if (object == layer->m_anticheatSpike) return;
                    auto const radius = std::max(object->m_scaleX, object->m_scaleY) * object->m_objectRadius;
                    if (radius > 0.f) {
                        auto const center = object->getPosition();
                        if (!inBuiltArea(cocos2d::CCRect{
                            center.x - radius, center.y - radius, radius * 2.f, radius * 2.f
                        })) return;
                        s_objectNode->drawCircle(
                            center, radius, palette.hazardFill,
                            width, palette.hazard, 12
                        );
                    } else if (auto* oriented = layer->m_isEditor ? object->getOrientedBox() : object->m_orientedBox) {
                        if (!inBuiltArea(boundsOf(oriented->m_corners))) return;
                        s_objectNode->drawPolygon(
                            oriented->m_corners.data(), 4,
                            palette.hazardFill, width, palette.hazard
                        );
                    } else {
                        auto const rectDirty = object->m_isObjectRectDirty;
                        auto const offsetCalculated = object->m_boxOffsetCalculated;
                        auto const rect = object->getObjectRect();
                        if (inBuiltArea(rect)) {
                            drawRect(s_objectNode, rect, palette.hazardFill, width, palette.hazard);
                        }
                        object->m_isObjectRectDirty = rectDirty;
                        object->m_boxOffsetCalculated = offsetCalculated;
                    }
                    return;
                }

                default: {
                    if (object == layer->m_player1 || object == layer->m_player2) return;
                    if (object->m_objectType == GameObjectType::Modifier &&
                        !static_cast<EffectGameObject*>(object)->m_isTouchTriggered) {
                        return;
                    }
                    if (auto* oriented = layer->m_isEditor ? object->getOrientedBox() : object->m_orientedBox) {
                        if (!inBuiltArea(boundsOf(oriented->m_corners))) return;
                        s_objectNode->drawPolygon(
                            oriented->m_corners.data(), 4,
                            palette.interactableFill, width, palette.interactable
                        );
                    } else {
                        auto const rectDirty = object->m_isObjectRectDirty;
                        auto const offsetCalculated = object->m_boxOffsetCalculated;
                        auto const rect = object->getObjectRect();
                        if (inBuiltArea(rect)) {
                            drawRect(
                                s_objectNode, rect,
                                palette.interactableFill, width, palette.interactable
                            );
                        }
                        object->m_isObjectRectDirty = rectDirty;
                        object->m_boxOffsetCalculated = offsetCalculated;
                    }
                    return;
                }
            }
        }

        template <class Callback>
        void forEachVisibleObject(GJBaseGameLayer* layer, Callback&& callback) {
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

        void drawPlayer(PlayerObject* player, float width, PlayerPalette const& palette) {
            if (!player) return;

            if (auto* oriented = player->m_orientedBox) {
                s_playerNode->drawPolygon(
                    oriented->m_corners.data(), 4,
                    palette.rotatedFill, width, palette.rotated
                );
            }
            drawRect(
                s_playerNode, player->getObjectRect(),
                palette.outerFill, width, palette.outer
            );
            drawRect(
                s_playerNode, player->getObjectRect(0.3f, 0.3f),
                palette.innerFill, width, palette.inner
            );
        }
    }

    void attach(GJBaseGameLayer* layer) {
        auto* root = overlay::root();
        if (!layer || !root) return;

        s_layer = layer;
        s_objectNode = OverlayDrawNode::create(settings::showHitboxes);
        if (!s_objectNode) return;
        s_objectNode->m_bUseArea = false;
        s_objectNode->setBlendFunc({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
        s_objectNode->setID("object-hitboxes"_spr);
        root->addChild(s_objectNode, 10);

        s_playerNode = OverlayDrawNode::create(settings::showHitboxes);
        if (!s_playerNode) return;
        s_playerNode->m_bUseArea = false;
        s_playerNode->setBlendFunc({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
        s_playerNode->setID("player-hitboxes"_spr);
        root->addChild(s_playerNode, 11);

        s_nextObjectRefresh = {};
        s_lastView = {-1, -1, -1, -1};
        s_objectsDirty = true;
        s_wasEnabled = false;
    }

    void detach(GJBaseGameLayer* layer) {
        if (layer != s_layer) return;
        s_objectNode = nullptr;
        s_playerNode = nullptr;
        s_layer = nullptr;
        s_nextObjectRefresh = {};
        s_lastView = {-1, -1, -1, -1};
        s_objectsDirty = true;
        s_wasEnabled = false;
    }

    void update(GJBaseGameLayer* layer) {
        if (!s_objectNode || !s_playerNode || layer != s_layer) return;

        auto const enabled = settings::showHitboxes();
        if (!enabled) {
            if (s_wasEnabled) {
                s_objectNode->clear();
                s_playerNode->clear();
            }
            s_wasEnabled = false;
            s_objectsDirty = true;
            return;
        }

        if (!s_wasEnabled) s_objectsDirty = true;
        s_wasEnabled = true;

        auto const zoom = std::max(0.01f, layer->m_gameState.m_cameraZoom);
        auto const width = settings::hitboxWidth() / zoom;
        auto const fillAlpha = settings::hitboxFillOpacity();

        auto const visibleSections = std::array{
            layer->m_leftSectionIndex,
            layer->m_rightSectionIndex,
            layer->m_bottomSectionIndex,
            layer->m_topSectionIndex,
        };
        if (visibleSections != s_lastView) {
            s_lastView = visibleSections;
            s_objectsDirty = true;
        }
        auto const view = visibleArea(layer);
        if (!contains(s_builtArea, view)) s_objectsDirty = true;

        // Environment geometry runs at the display rate when it is cheap and
        // automatically backs off on dense sections. Player boxes remain at
        // the display rate regardless of the environment cost.
        auto const now = Clock::now();
        if (s_objectsDirty || now >= s_nextObjectRefresh) {
            auto const started = now;
            auto const solid = settings::color(settings::Color::Solid);
            auto const hazard = settings::color(settings::Color::Hazard);
            auto const interactable = settings::color(settings::Color::Interactable);
            Palette const palette = {
                .solid = solid,
                .hazard = hazard,
                .interactable = interactable,
                .solidFill = withFillAlpha(solid, fillAlpha),
                .hazardFill = withFillAlpha(hazard, fillAlpha),
                .interactableFill = withFillAlpha(interactable, fillAlpha),
            };
            s_builtArea = withMargin(view);
            s_objectNode->clear();
            forEachVisibleObject(layer, [&](GameObject* object) {
                drawObject(layer, object, width, palette);
            });

            auto const cost = Clock::now() - started;
            auto const basePeriod = std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(1.0 / 60.0)
            );
            auto const adaptivePeriod = std::max(basePeriod, cost * 3);
            s_nextObjectRefresh = started + adaptivePeriod;
            s_objectsDirty = false;
        }

        s_playerNode->clear();
        auto const outer = settings::color(settings::Color::Player);
        auto const inner = settings::color(settings::Color::PlayerInner);
        auto const rotated = settings::color(settings::Color::PlayerRotated);
        PlayerPalette const playerPalette{
            .outer = outer,
            .inner = inner,
            .rotated = rotated,
            .outerFill = withFillAlpha(outer, fillAlpha),
            .innerFill = withFillAlpha(inner, fillAlpha),
            .rotatedFill = withFillAlpha(rotated, fillAlpha),
        };
        drawPlayer(layer->m_player1, width, playerPalette);
        if (layer->m_gameState.m_isDualMode) {
            drawPlayer(layer->m_player2, width, playerPalette);
        }
    }
}
