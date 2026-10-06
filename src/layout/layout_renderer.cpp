#include "layout_renderer.hpp"

#include "object_roles.hpp"
#include "../debug.hpp"
#include "../settings.hpp"

#include <Geode/cocos/platform/win32/CCGL.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;

namespace layoutfeed::renderer {
    namespace {
        // Role texture layout: one byte per quad, kFlagsWidth quads per row.
        constexpr GLint kFlagsWidth = 2048;

        // The atlas indices map quad N to vertices 4N..4N+3, so gl_VertexID / 4
        // is the quad index. Hidden quads are moved outside the clip volume and
        // never reach the rasterizer; colors are replaced without touching the
        // game's vertex data. Roles: 0 keep, 1 hide, 2 main color, 3 detail,
        // 4 original colors at full opacity.
        constexpr char const* kVertexShader = R"(#version 130
in vec4 a_position;
in vec4 a_color;
in vec2 a_texCoord;

uniform mat4 u_mvp;
uniform sampler2D u_flags;
uniform int u_flagsWidth;
uniform vec3 u_mainTint;
uniform vec3 u_detailTint;
uniform float u_forceOpaque;
uniform int u_roleOverride;

out vec4 v_color;
out vec2 v_texCoord;

void main() {
    int role = u_roleOverride;
    if (role < 0) {
        int quad = gl_VertexID / 4;
        role = int(texelFetch(u_flags, ivec2(quad % u_flagsWidth, quad / u_flagsWidth), 0).r * 255.0 + 0.5);
    }
    if (role == 1) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        v_color = vec4(0.0);
        v_texCoord = vec2(0.0);
        return;
    }

    vec4 color = a_color;
    if (role == 2 || role == 3) {
        float alpha = mix(a_color.a, 1.0, u_forceOpaque);
        vec3 tint = role == 2 ? u_mainTint : u_detailTint;
        // Game textures and vertex colors use premultiplied alpha.
        color = vec4(tint * alpha, alpha);
    } else if (role == 4 && u_forceOpaque > 0.5) {
        // Undo the premultiplication: same colors, fully opaque.
        color = a_color.a > 0.004 ? vec4(a_color.rgb / a_color.a, 1.0) : vec4(1.0);
    }

    gl_Position = u_mvp * a_position;
    v_color = color;
    v_texCoord = a_texCoord;
}
)";

        constexpr char const* kFragmentShader = R"(#version 130
in vec4 v_color;
in vec2 v_texCoord;

uniform sampler2D u_texture;

void main() {
    gl_FragColor = texture(u_texture, v_texCoord) * v_color;
}
)";

        enum class State { Untried, Ready, Failed };

        struct Uniforms {
            GLint mvp = -1;
            GLint texture = -1;
            GLint flags = -1;
            GLint flagsWidth = -1;
            GLint mainTint = -1;
            GLint detailTint = -1;
            GLint forceOpaque = -1;
            GLint roleOverride = -1;
        };

        struct BatchCache {
            CCTextureAtlas* atlas = nullptr;
            GLuint texture = 0;
            GLsizei rows = 0;
            unsigned int quads = 0;
            std::uint32_t rolesGeneration = 0;
            std::uint32_t settingsGeneration = 0;
            std::uint32_t hidden = 0;
            std::vector<CCObject*> sprites;
            std::vector<std::uint8_t> flags;
        };

        State s_state = State::Untried;
        GLuint s_program = 0;
        Uniforms s_uniforms;
        std::unordered_map<CCSpriteBatchNode*, BatchCache> s_caches;
        GLint s_maxTextureSize = 0;
        GLuint s_revealBuffer = 0;

        std::string infoLog(GLuint object, bool program) {
            GLint length = 0;
            if (program) glGetProgramiv(object, GL_INFO_LOG_LENGTH, &length);
            else glGetShaderiv(object, GL_INFO_LOG_LENGTH, &length);
            if (length <= 1) return {};
            std::string text(static_cast<std::size_t>(length), '\0');
            if (program) glGetProgramInfoLog(object, length, nullptr, text.data());
            else glGetShaderInfoLog(object, length, nullptr, text.data());
            while (!text.empty() && (text.back() == '\0' || text.back() == '\n')) text.pop_back();
            return text;
        }

        GLuint compile(GLenum type, char const* source, char const* name) {
            auto const shader = glCreateShader(type);
            if (!shader) {
                log::error("Layout renderer: glCreateShader({}) failed", name);
                return 0;
            }
            glShaderSource(shader, 1, &source, nullptr);
            glCompileShader(shader);
            GLint status = GL_FALSE;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
            auto const messages = infoLog(shader, false);
            if (status != GL_TRUE) {
                log::error("Layout renderer: {} shader failed to compile:\n{}", name, messages);
                glDeleteShader(shader);
                return 0;
            }
            if (!messages.empty()) LF_DEBUG("Layout renderer: {} shader compiler output:\n{}", name, messages);
            return shader;
        }

        bool build() {
            debug::logGLInfo();
            glGetIntegerv(GL_MAX_TEXTURE_SIZE, &s_maxTextureSize);

            if (!glCreateShader || !glCreateProgram || !glUniformMatrix4fv) {
                log::error("Layout renderer: OpenGL 2.0 shader entry points are missing");
                return false;
            }

            auto const vertex = compile(GL_VERTEX_SHADER, kVertexShader, "vertex");
            auto const fragment = compile(GL_FRAGMENT_SHADER, kFragmentShader, "fragment");
            if (!vertex || !fragment) {
                if (vertex) glDeleteShader(vertex);
                if (fragment) glDeleteShader(fragment);
                return false;
            }

            s_program = glCreateProgram();
            glAttachShader(s_program, vertex);
            glAttachShader(s_program, fragment);
            // Same attribute slots as cocos2d, so CCTextureAtlas can bind its
            // vertex buffers exactly as it does for the game's own shader.
            glBindAttribLocation(s_program, kCCVertexAttrib_Position, "a_position");
            glBindAttribLocation(s_program, kCCVertexAttrib_Color, "a_color");
            glBindAttribLocation(s_program, kCCVertexAttrib_TexCoords, "a_texCoord");
            glLinkProgram(s_program);
            glDetachShader(s_program, vertex);
            glDetachShader(s_program, fragment);
            glDeleteShader(vertex);
            glDeleteShader(fragment);

            GLint status = GL_FALSE;
            glGetProgramiv(s_program, GL_LINK_STATUS, &status);
            if (status != GL_TRUE) {
                log::error("Layout renderer: shader program failed to link:\n{}", infoLog(s_program, true));
                glDeleteProgram(s_program);
                s_program = 0;
                return false;
            }

            s_uniforms.mvp = glGetUniformLocation(s_program, "u_mvp");
            s_uniforms.texture = glGetUniformLocation(s_program, "u_texture");
            s_uniforms.flags = glGetUniformLocation(s_program, "u_flags");
            s_uniforms.flagsWidth = glGetUniformLocation(s_program, "u_flagsWidth");
            s_uniforms.mainTint = glGetUniformLocation(s_program, "u_mainTint");
            s_uniforms.detailTint = glGetUniformLocation(s_program, "u_detailTint");
            s_uniforms.forceOpaque = glGetUniformLocation(s_program, "u_forceOpaque");
            s_uniforms.roleOverride = glGetUniformLocation(s_program, "u_roleOverride");
            if (s_uniforms.mvp < 0 || s_uniforms.flags < 0 || s_uniforms.texture < 0) {
                log::error(
                    "Layout renderer: missing uniforms (mvp {}, texture {}, flags {})",
                    s_uniforms.mvp, s_uniforms.texture, s_uniforms.flags
                );
                glDeleteProgram(s_program);
                s_program = 0;
                return false;
            }

            log::info(
                "Layout renderer ready: GPU role shader program {}, role texture width {}, max texture {}",
                s_program, kFlagsWidth, s_maxTextureSize
            );
            debug::checkGL("renderer::build");
            return true;
        }

        void setTint(GLint location, ccColor3B const& color) {
            glUniform3f(location, color.r / 255.f, color.g / 255.f, color.b / 255.f);
        }

        // Binds a role texture on unit 1 and returns to unit 0, which cocos2d
        // assumes is active. The cocos2d cache is updated first and the bind
        // is then issued unconditionally: other code (2.2 shaders, overlays)
        // binds unit 1 behind the cache's back, and trusting a stale cache
        // made uploads hit the wrong texture (GL_INVALID_VALUE) and made the
        // shader read wrong roles, so objects vanished at random.
        void bindFlags(GLuint texture) {
            ccGLBindTexture2DN(1, texture);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, texture);
            glActiveTexture(GL_TEXTURE0);
        }

        bool allocate(BatchCache& cache, GLsizei rows) {
            // Grow with headroom so a level that keeps activating objects does
            // not reallocate the texture every few frames.
            auto const target = std::max<GLsizei>(rows + rows / 4, 4);
            if (s_maxTextureSize > 0 && target > s_maxTextureSize) {
                log::error(
                    "Layout renderer: batch needs {} role rows, above GL_MAX_TEXTURE_SIZE {}",
                    target, s_maxTextureSize
                );
                return false;
            }
            if (!cache.texture) glGenTextures(1, &cache.texture);
            if (!cache.texture) return false;

            bindFlags(cache.texture);
            glActiveTexture(GL_TEXTURE1);
            // texelFetch needs a complete texture: no mipmaps, nearest filter.
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, kFlagsWidth, target, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
            glActiveTexture(GL_TEXTURE0);
            LF_DEBUG("Layout renderer: role texture {} sized {}x{} ({} quads)", cache.texture, kFlagsWidth, target, cache.quads);
            cache.rows = target;
            debug::checkGL("renderer::allocate");
            return true;
        }

        // Rebuilds the role bytes only when the batch content changed. Most
        // frames compare one pointer array and reuse the uploaded texture.
        bool updateFlags(BatchCache& cache, CCSpriteBatchNode* batch, CCTextureAtlas* atlas, unsigned int quads) {
            auto* descendants = batch->getDescendants();
            auto const count = descendants && descendants->data ? descendants->data->num : 0u;
            auto** items = count ? descendants->data->arr : nullptr;
            auto const rolesGeneration = roles::generation();
            auto const settingsGeneration = settings::generation();

            auto const unchanged = cache.texture && cache.atlas == atlas && cache.quads == quads &&
                cache.rolesGeneration == rolesGeneration &&
                cache.settingsGeneration == settingsGeneration &&
                cache.sprites.size() == count &&
                (count == 0 || std::memcmp(cache.sprites.data(), items, count * sizeof(CCObject*)) == 0);
            if (unchanged) {
                ++debug::counters().flagReuses;
                return true;
            }

            if (cache.atlas != atlas) {
                cache.atlas = atlas;
                cache.sprites.clear();
            }
            cache.quads = quads;

            auto const rows = static_cast<GLsizei>((quads + kFlagsWidth - 1) / kFlagsWidth);
            if ((!cache.texture || rows > cache.rows) && !allocate(cache, rows)) return false;

            cache.flags.assign(static_cast<std::size_t>(rows) * kFlagsWidth, 0);
            cache.hidden = 0;
            for (unsigned int i = 0; i < count; ++i) {
                auto* sprite = static_cast<CCSprite*>(items[i]);
                if (!sprite) continue;
                auto index = sprite->m_uAtlasIndex;
                if (index >= quads) index = i;
                if (index >= quads) continue;
                auto const role = roles::resolve(sprite);
                cache.flags[index] = static_cast<std::uint8_t>(role);
                if (role == roles::Role::Hide) ++cache.hidden;
            }
            cache.sprites.assign(items, items + count);
            cache.rolesGeneration = rolesGeneration;
            cache.settingsGeneration = settingsGeneration;

            // Client-memory upload: no pixel unpack buffer, tight rows.
            GLint alignment = 4;
            GLint rowLength = 0;
            GLint unpackBuffer = 0;
            glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
            glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
            glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            if (unpackBuffer) glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            bindFlags(cache.texture);
            glActiveTexture(GL_TEXTURE1);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kFlagsWidth, rows, GL_RED, GL_UNSIGNED_BYTE, cache.flags.data());
            glActiveTexture(GL_TEXTURE0);
            if (unpackBuffer) glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(unpackBuffer));
            glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
            glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);

            ++debug::counters().flagRebuilds;
            debug::checkGL("renderer::updateFlags");
            return true;
        }
    }

    bool ensureReady() {
        if (s_state == State::Untried) {
            s_state = build() ? State::Ready : State::Failed;
            if (s_state == State::Failed) {
                log::error(
                    "Layout renderer unavailable: objects are drawn with their normal colors in "
                    "Layout Mode. Background, ground, glow, particles and shaders are still handled"
                );
            }
        }
        return s_state == State::Ready;
    }

    bool ready() {
        return s_state == State::Ready;
    }

    void beginPass() {
        if (s_state != State::Ready) return;
        ccGLUseProgram(s_program);
        glUniform1i(s_uniforms.texture, 0);
        glUniform1i(s_uniforms.flags, 1);
        glUniform1i(s_uniforms.flagsWidth, kFlagsWidth);
        setTint(s_uniforms.mainTint, settings::objectColor());
        setTint(s_uniforms.detailTint, settings::detailColor());
        glUniform1f(s_uniforms.forceOpaque, settings::forceOpacity() ? 1.f : 0.f);
        glUniform1i(s_uniforms.roleOverride, -1);
    }

    bool drawObjectBatch(CCSpriteBatchNode* batch) {
        if (s_state != State::Ready || !batch) return false;
        auto* atlas = batch->getTextureAtlas();
        if (!atlas) return true;
        auto const quads = atlas->getTotalQuads();
        if (!quads) return true;

        auto& cache = s_caches[batch];
        if (!updateFlags(cache, batch, atlas, quads)) return false;

        // Same matrix cocos2d's setUniformsForBuiltins would upload.
        kmMat4 projection;
        kmMat4 modelView;
        kmMat4 mvp;
        kmGLGetMatrix(KM_GL_PROJECTION, &projection);
        kmGLGetMatrix(KM_GL_MODELVIEW, &modelView);
        kmMat4Multiply(&mvp, &projection, &modelView);

        ccGLUseProgram(s_program);
        glUniformMatrix4fv(s_uniforms.mvp, 1, GL_FALSE, mvp.mat);
        bindFlags(cache.texture);

        auto const blend = batch->getBlendFunc();
        if (settings::normalBlending() && blend.dst == GL_ONE) {
            ccGLBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        } else {
            ccGLBlendFunc(blend.src, blend.dst);
        }

        // The atlas binds its own buffers, attributes and texture on unit 0.
        // Its quads were already uploaded by the normal pass this frame.
        atlas->drawQuads();

        auto& counters = debug::counters();
        ++counters.objectBatchDraws;
        counters.quadsDrawn += quads - cache.hidden;
        counters.quadsHidden += cache.hidden;
        return true;
    }

    void drawRevealed(CCTexture2D* texture, std::vector<ccV3F_C4B_T2F> const& vertices) {
        if (s_state != State::Ready || !texture || vertices.empty()) return;

        kmMat4 projection;
        kmMat4 modelView;
        kmMat4 mvp;
        kmGLGetMatrix(KM_GL_PROJECTION, &projection);
        kmGLGetMatrix(KM_GL_MODELVIEW, &modelView);
        kmMat4Multiply(&mvp, &projection, &modelView);

        ccGLUseProgram(s_program);
        glUniformMatrix4fv(s_uniforms.mvp, 1, GL_FALSE, mvp.mat);
        // Vertex colors are final (premultiplied, opaque): role "keep".
        glUniform1i(s_uniforms.roleOverride, 0);
        ccGLBindTexture2D(texture->getName());
        glBindTexture(GL_TEXTURE_2D, texture->getName());
        ccGLBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        if (!s_revealBuffer) glGenBuffers(1, &s_revealBuffer);
        ccGLBindVAO(0);
        glBindBuffer(GL_ARRAY_BUFFER, s_revealBuffer);
        glBufferData(
            GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(ccV3F_C4B_T2F)),
            vertices.data(), GL_STREAM_DRAW
        );
        ccGLEnableVertexAttribs(kCCVertexAttribFlag_PosColorTex);
        auto const stride = static_cast<GLsizei>(sizeof(ccV3F_C4B_T2F));
        glVertexAttribPointer(kCCVertexAttrib_Position, 3, GL_FLOAT, GL_FALSE, stride,
            reinterpret_cast<void const*>(offsetof(ccV3F_C4B_T2F, vertices)));
        glVertexAttribPointer(kCCVertexAttrib_Color, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride,
            reinterpret_cast<void const*>(offsetof(ccV3F_C4B_T2F, colors)));
        glVertexAttribPointer(kCCVertexAttrib_TexCoords, 2, GL_FLOAT, GL_FALSE, stride,
            reinterpret_cast<void const*>(offsetof(ccV3F_C4B_T2F, texCoords)));
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size()));
        // cocos2d draws plain sprites from client-side arrays, which requires
        // buffer 0 to be bound again.
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glUniform1i(s_uniforms.roleOverride, -1);
        debug::checkGL("renderer::drawRevealed");
    }

    void releaseCaches(char const* reason) {
        if (s_caches.empty()) return;
        LF_DEBUG("Layout renderer: releasing {} batch role textures ({})", s_caches.size(), reason);
        for (auto& [batch, cache] : s_caches) {
            (void)batch;
            if (cache.texture) ccGLDeleteTextureN(1, cache.texture);
        }
        glActiveTexture(GL_TEXTURE0);
        s_caches.clear();
    }
}
