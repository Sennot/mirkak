#pragma once

#include <Geode/Geode.hpp>

#include <vector>

namespace layoutfeed::shapes {
    // GPU-drawn outline shapes for hitboxes and trajectories. Per shape the
    // CPU only writes its description (corners or centre and radius, colors,
    // border width) into one bounding quad; fill, border and antialiasing are
    // evaluated per pixel from the signed distance to the shape. This
    // replaces CCDrawNode, which builds and extrudes every outline on the CPU.
    //
    // Geometry matches CCDrawNode: a border of half width `border` on both
    // sides of the edge, the fill inside the inner edge, and segments as
    // capsules of the given radius. Coordinates are in the space of the node
    // the batch is drawn from (object-layer space for the overlay).
    struct Vertex {
        float position[2];
        float shape0[4];  // polygon p0,p1 | circle centre,radius | segment a,b
        float shape1[4];  // polygon p2,p3
        float params[4];  // type, border half width, corner count | radius, unused
        GLubyte fill[4];
        GLubyte border[4];
    };

    class Batch final {
    public:
        Batch() = default;
        ~Batch();
        Batch(Batch const&) = delete;
        Batch& operator=(Batch const&) = delete;

        void clear();
        bool empty() const { return m_vertices.empty(); }
        std::size_t shapeCount() const { return m_vertices.size() / 6; }

        // Convex polygon with 3 or 4 corners in any winding.
        void polygon(cocos2d::CCPoint const* corners, int count, cocos2d::ccColor4F const& fill,
                     float border, cocos2d::ccColor4F const& borderColor);
        void rect(cocos2d::CCRect const& rect, cocos2d::ccColor4F const& fill,
                  float border, cocos2d::ccColor4F const& borderColor);
        void circle(cocos2d::CCPoint const& centre, float radius, cocos2d::ccColor4F const& fill,
                    float border, cocos2d::ccColor4F const& borderColor);
        void segment(cocos2d::CCPoint const& from, cocos2d::CCPoint const& to, float radius,
                     cocos2d::ccColor4F const& color);

        // Draws with the current cocos2d modelview/projection. The vertex data
        // is uploaded only after it changed.
        void draw();

    private:
        void emit(float minX, float minY, float maxX, float maxY, Vertex const& shape);

        std::vector<Vertex> m_vertices;
        GLuint m_buffer = 0;
        bool m_dirty = true;
    };

    // Overlay node drawing one batch while `enabled()` is true.
    class Node final : public cocos2d::CCNode {
    public:
        static Node* create(bool (*enabled)());

        void draw() override;
        Batch& batch() { return m_batch; }

    private:
        explicit Node(bool (*enabled)()) : m_enabled(enabled) {}

        bool (*m_enabled)();
        Batch m_batch;
    };

    bool ready();
}
