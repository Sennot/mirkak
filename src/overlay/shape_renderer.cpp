#include "shape_renderer.hpp"

#include "../debug.hpp"

#include <Geode/cocos/platform/win32/CCGL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>

using namespace geode::prelude;

namespace layoutfeed::shapes {
    namespace {
        enum ShapeType : int { Polygon = 0, Circle = 1, Segment = 2 };

        // Attribute slots of the shape program.
        enum Attribute : GLuint {
            AttrPosition = 0,
            AttrShape0 = 1,
            AttrShape1 = 2,
            AttrParams = 3,
            AttrFill = 4,
            AttrBorder = 5,
        };

        constexpr char const* kVertexShader = R"(#version 130
in vec2 a_position;
in vec4 a_shape0;
in vec4 a_shape1;
in vec4 a_params;
in vec4 a_fill;
in vec4 a_border;

uniform mat4 u_mvp;

out vec2 v_position;
flat out vec4 v_shape0;
flat out vec4 v_shape1;
flat out vec4 v_params;
flat out vec4 v_fill;
flat out vec4 v_border;

void main() {
    gl_Position = u_mvp * vec4(a_position, 0.0, 1.0);
    v_position = a_position;
    v_shape0 = a_shape0;
    v_shape1 = a_shape1;
    v_params = a_params;
    v_fill = a_fill;
    v_border = a_border;
}
)";

        // Signed distance (negative inside). Convex polygons use the largest
        // edge distance, which is exact inside and on the edges, where the
        // border and the fill boundary are.
        constexpr char const* kFragmentShader = R"(#version 130
in vec2 v_position;
flat in vec4 v_shape0;
flat in vec4 v_shape1;
flat in vec4 v_params;
flat in vec4 v_fill;
flat in vec4 v_border;

float cross2(vec2 a, vec2 b) { return a.x * b.y - a.y * b.x; }

float edgeDistance(vec2 p, vec2 a, vec2 b, float orientation) {
    vec2 e = b - a;
    float len = length(e);
    if (len < 1e-6) return -1e9;
    vec2 n = vec2(e.y, -e.x) * (orientation / len);
    return dot(p - a, n);
}

void main() {
    int type = int(v_params.x + 0.5);
    vec2 p = v_position;
    float d;
    if (type == 0) {
        vec2 p0 = v_shape0.xy;
        vec2 p1 = v_shape0.zw;
        vec2 p2 = v_shape1.xy;
        vec2 p3 = v_shape1.zw;
        bool quad = v_params.z > 3.5;
        float area = cross2(p1 - p0, p2 - p0) + (quad ? cross2(p2 - p0, p3 - p0) : 0.0);
        float orientation = area >= 0.0 ? 1.0 : -1.0;
        d = max(edgeDistance(p, p0, p1, orientation), edgeDistance(p, p1, p2, orientation));
        if (quad) {
            d = max(d, edgeDistance(p, p2, p3, orientation));
            d = max(d, edgeDistance(p, p3, p0, orientation));
        } else {
            d = max(d, edgeDistance(p, p2, p0, orientation));
        }
    } else if (type == 1) {
        d = length(p - v_shape0.xy) - v_shape0.z;
    } else {
        vec2 pa = p - v_shape0.xy;
        vec2 ba = v_shape0.zw - v_shape0.xy;
        float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-12), 0.0, 1.0);
        d = length(pa - ba * h) - v_params.z;
    }

    float aa = max(fwidth(d), 1e-5) * 0.5;
    vec4 color;
    if (type == 2) {
        float coverage = 1.0 - smoothstep(-aa, aa, d);
        color = vec4(v_fill.rgb * v_fill.a, v_fill.a) * coverage;
    } else {
        float w = v_params.y;
        float inner = 1.0 - smoothstep(-w - aa, -w + aa, d);
        float outer = 1.0 - smoothstep(w - aa, w + aa, d);
        float fillCoverage = w > 0.0 ? inner : outer;
        float borderCoverage = w > 0.0 ? max(outer - inner, 0.0) : 0.0;
        color = vec4(v_fill.rgb * v_fill.a, v_fill.a) * fillCoverage +
                vec4(v_border.rgb * v_border.a, v_border.a) * borderCoverage;
    }
    if (color.a < 0.002) discard;
    // Premultiplied alpha.
    gl_FragColor = color;
}
)";

        enum class State { Untried, Ready, Failed };
        State s_state = State::Untried;
        GLuint s_program = 0;
        GLint s_mvp = -1;

        std::string infoLog(GLuint object, bool program) {
            GLint length = 0;
            if (program) glGetProgramiv(object, GL_INFO_LOG_LENGTH, &length);
            else glGetShaderiv(object, GL_INFO_LOG_LENGTH, &length);
            if (length <= 1) return {};
            std::string text(static_cast<std::size_t>(length), '\0');
            if (program) glGetProgramInfoLog(object, length, nullptr, text.data());
            else glGetShaderInfoLog(object, length, nullptr, text.data());
            return text;
        }

        GLuint compile(GLenum type, char const* source) {
            auto const shader = glCreateShader(type);
            glShaderSource(shader, 1, &source, nullptr);
            glCompileShader(shader);
            GLint status = GL_FALSE;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
            if (status != GL_TRUE) {
                log::error("Shape renderer: shader failed to compile:\n{}", infoLog(shader, false));
                glDeleteShader(shader);
                return 0;
            }
            return shader;
        }

        bool build() {
            auto const vertex = compile(GL_VERTEX_SHADER, kVertexShader);
            auto const fragment = compile(GL_FRAGMENT_SHADER, kFragmentShader);
            if (!vertex || !fragment) {
                if (vertex) glDeleteShader(vertex);
                if (fragment) glDeleteShader(fragment);
                return false;
            }
            s_program = glCreateProgram();
            glAttachShader(s_program, vertex);
            glAttachShader(s_program, fragment);
            glBindAttribLocation(s_program, AttrPosition, "a_position");
            glBindAttribLocation(s_program, AttrShape0, "a_shape0");
            glBindAttribLocation(s_program, AttrShape1, "a_shape1");
            glBindAttribLocation(s_program, AttrParams, "a_params");
            glBindAttribLocation(s_program, AttrFill, "a_fill");
            glBindAttribLocation(s_program, AttrBorder, "a_border");
            glLinkProgram(s_program);
            glDetachShader(s_program, vertex);
            glDetachShader(s_program, fragment);
            glDeleteShader(vertex);
            glDeleteShader(fragment);
            GLint status = GL_FALSE;
            glGetProgramiv(s_program, GL_LINK_STATUS, &status);
            if (status != GL_TRUE) {
                log::error("Shape renderer: program failed to link:\n{}", infoLog(s_program, true));
                glDeleteProgram(s_program);
                s_program = 0;
                return false;
            }
            s_mvp = glGetUniformLocation(s_program, "u_mvp");
            log::info("Shape renderer ready: hitboxes and trajectories are drawn by the GPU shape shader");
            return true;
        }

        bool ensureReady() {
            if (s_state == State::Untried) {
                s_state = build() ? State::Ready : State::Failed;
                if (s_state == State::Failed) {
                    log::error("Shape renderer unavailable: hitboxes and trajectories cannot be drawn");
                }
            }
            return s_state == State::Ready;
        }

        void packColor(GLubyte (&out)[4], ccColor4F const& color) {
            auto const channel = [](float value) {
                return static_cast<GLubyte>(std::clamp(value, 0.f, 1.f) * 255.f + 0.5f);
            };
            out[0] = channel(color.r);
            out[1] = channel(color.g);
            out[2] = channel(color.b);
            out[3] = channel(color.a);
        }

        // Room for the border and for the antialiasing ramp around the shape.
        constexpr float kPadding = 2.f;
    }

    Batch::~Batch() {
        // The GL context outlives every overlay node during the game.
        if (m_buffer) glDeleteBuffers(1, &m_buffer);
    }

    void Batch::clear() {
        if (m_vertices.empty()) return;
        m_vertices.clear();
        m_dirty = true;
    }

    void Batch::emit(float minX, float minY, float maxX, float maxY, Vertex const& shape) {
        std::array<std::array<float, 2>, 4> const corners{{
            {minX, minY}, {maxX, minY}, {maxX, maxY}, {minX, maxY},
        }};
        for (auto const index : {0, 1, 2, 0, 2, 3}) {
            auto vertex = shape;
            vertex.position[0] = corners[index][0];
            vertex.position[1] = corners[index][1];
            m_vertices.push_back(vertex);
        }
        m_dirty = true;
    }

    void Batch::polygon(CCPoint const* corners, int count, ccColor4F const& fill,
                        float border, ccColor4F const& borderColor) {
        if (!corners || count < 3) return;
        count = std::min(count, 4);
        Vertex shape{};
        auto const point = [&](int index) { return corners[std::min(index, count - 1)]; };
        shape.shape0[0] = point(0).x;
        shape.shape0[1] = point(0).y;
        shape.shape0[2] = point(1).x;
        shape.shape0[3] = point(1).y;
        shape.shape1[0] = point(2).x;
        shape.shape1[1] = point(2).y;
        shape.shape1[2] = point(3).x;
        shape.shape1[3] = point(3).y;
        shape.params[0] = static_cast<float>(Polygon);
        shape.params[1] = std::max(0.f, border);
        shape.params[2] = static_cast<float>(count);
        packColor(shape.fill, fill);
        packColor(shape.border, borderColor);

        auto minX = corners[0].x, maxX = corners[0].x, minY = corners[0].y, maxY = corners[0].y;
        for (int i = 1; i < count; ++i) {
            minX = std::min(minX, corners[i].x);
            maxX = std::max(maxX, corners[i].x);
            minY = std::min(minY, corners[i].y);
            maxY = std::max(maxY, corners[i].y);
        }
        auto const pad = shape.params[1] + kPadding;
        emit(minX - pad, minY - pad, maxX + pad, maxY + pad, shape);
    }

    void Batch::rect(CCRect const& rect, ccColor4F const& fill, float border, ccColor4F const& borderColor) {
        std::array<CCPoint, 4> const corners{
            CCPoint{rect.getMinX(), rect.getMinY()},
            CCPoint{rect.getMinX(), rect.getMaxY()},
            CCPoint{rect.getMaxX(), rect.getMaxY()},
            CCPoint{rect.getMaxX(), rect.getMinY()},
        };
        polygon(corners.data(), 4, fill, border, borderColor);
    }

    void Batch::circle(CCPoint const& centre, float radius, ccColor4F const& fill,
                       float border, ccColor4F const& borderColor) {
        if (radius <= 0.f) return;
        Vertex shape{};
        shape.shape0[0] = centre.x;
        shape.shape0[1] = centre.y;
        shape.shape0[2] = radius;
        shape.params[0] = static_cast<float>(Circle);
        shape.params[1] = std::max(0.f, border);
        packColor(shape.fill, fill);
        packColor(shape.border, borderColor);
        auto const extent = radius + shape.params[1] + kPadding;
        emit(centre.x - extent, centre.y - extent, centre.x + extent, centre.y + extent, shape);
    }

    void Batch::segment(CCPoint const& from, CCPoint const& to, float radius, ccColor4F const& color) {
        if (radius <= 0.f) return;
        Vertex shape{};
        shape.shape0[0] = from.x;
        shape.shape0[1] = from.y;
        shape.shape0[2] = to.x;
        shape.shape0[3] = to.y;
        shape.params[0] = static_cast<float>(Segment);
        shape.params[2] = radius;
        packColor(shape.fill, color);
        auto const extent = radius + kPadding;
        emit(std::min(from.x, to.x) - extent, std::min(from.y, to.y) - extent,
             std::max(from.x, to.x) + extent, std::max(from.y, to.y) + extent, shape);
    }

    void Batch::draw() {
        if (m_vertices.empty() || !ensureReady()) return;

        kmMat4 projection;
        kmMat4 modelView;
        kmMat4 mvp;
        kmGLGetMatrix(KM_GL_PROJECTION, &projection);
        kmGLGetMatrix(KM_GL_MODELVIEW, &modelView);
        kmMat4Multiply(&mvp, &projection, &modelView);

        ccGLUseProgram(s_program);
        glUniformMatrix4fv(s_mvp, 1, GL_FALSE, mvp.mat);
        ccGLBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        ccGLBindVAO(0);

        if (!m_buffer) glGenBuffers(1, &m_buffer);
        glBindBuffer(GL_ARRAY_BUFFER, m_buffer);
        if (m_dirty) {
            glBufferData(
                GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(m_vertices.size() * sizeof(Vertex)),
                m_vertices.data(), GL_DYNAMIC_DRAW
            );
            m_dirty = false;
        }

        // Slots 0-2 through the cocos2d cache so it stays consistent; 3-5 are
        // only used here and are disabled again afterwards.
        ccGLEnableVertexAttribs(kCCVertexAttribFlag_PosColorTex);
        glEnableVertexAttribArray(AttrParams);
        glEnableVertexAttribArray(AttrFill);
        glEnableVertexAttribArray(AttrBorder);
        auto const stride = static_cast<GLsizei>(sizeof(Vertex));
        auto const offset = [](std::size_t value) { return reinterpret_cast<void const*>(value); };
        glVertexAttribPointer(AttrPosition, 2, GL_FLOAT, GL_FALSE, stride, offset(offsetof(Vertex, position)));
        glVertexAttribPointer(AttrShape0, 4, GL_FLOAT, GL_FALSE, stride, offset(offsetof(Vertex, shape0)));
        glVertexAttribPointer(AttrShape1, 4, GL_FLOAT, GL_FALSE, stride, offset(offsetof(Vertex, shape1)));
        glVertexAttribPointer(AttrParams, 4, GL_FLOAT, GL_FALSE, stride, offset(offsetof(Vertex, params)));
        glVertexAttribPointer(AttrFill, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, offset(offsetof(Vertex, fill)));
        glVertexAttribPointer(AttrBorder, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, offset(offsetof(Vertex, border)));

        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(m_vertices.size()));

        glDisableVertexAttribArray(AttrParams);
        glDisableVertexAttribArray(AttrFill);
        glDisableVertexAttribArray(AttrBorder);
        // cocos2d draws plain sprites from client-side arrays.
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        debug::checkGL("shapes::draw");
    }

    Node* Node::create(bool (*enabled)()) {
        auto* node = new Node(enabled);
        if (node->init()) {
            node->autorelease();
            return node;
        }
        delete node;
        return nullptr;
    }

    void Node::draw() {
        if (m_enabled && !m_enabled()) return;
        m_batch.draw();
    }

    bool ready() {
        return ensureReady();
    }
}
