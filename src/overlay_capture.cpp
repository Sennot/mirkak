#include "overlay_capture.hpp"

#include "debug.hpp"
#include "spout/spout_sender.hpp"

#include <Geode/cocos/platform/win32/CCGL.h>

#include <array>
#include <string>

using namespace geode::prelude;

namespace layoutfeed::overlay {
    namespace {
        // Fullscreen triangle generated from gl_VertexID; no vertex buffers,
        // so no game or overlay attribute state is involved.
        constexpr char const* kVertexShader = R"(#version 130
out vec2 v_texCoord;
void main() {
    vec2 position = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    v_texCoord = position;
    gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
}
)";

        constexpr char const* kFragmentShader = R"(#version 130
in vec2 v_texCoord;
uniform sampler2D u_overlay;
void main() {
    vec4 color = texture(u_overlay, v_texCoord);
    // Premultiplied color can never exceed its alpha. Overlays that mask
    // alpha writes leave alpha at 0; their color then defines coverage.
    color.a = max(color.a, max(color.r, max(color.g, color.b)));
    gl_FragColor = color;
}
)";

        struct Target {
            GLuint fbo = 0;
            GLuint texture = 0;
            // Depth and stencil like the default framebuffer: menu renderers
            // may clip with the stencil buffer or use depth testing.
            GLuint depthStencil = 0;
        };

        enum class State { Untried, Ready, Failed };

        State s_state = State::Untried;
        GLuint s_program = 0;
        GLint s_overlayUniform = -1;
        Target s_normal;  // copy of the normal frame (what OBS sees)
        Target s_layout;  // layout frame (what the monitor sees)
        Target s_overlay; // everything the menus and overlays drew
        GLsizei s_width = 0;
        GLsizei s_height = 0;
        bool s_redirecting = false;

        // How menus are separated from the game picture:
        //   DefaultAlpha: the window's own back buffer is cleared to
        //     transparent and the menus draw on it. This also catches menus
        //     and the Steam overlay that bind the default framebuffer
        //     themselves (they never reached an offscreen target).
        //   Offscreen: the back buffer has no alpha channel, so a
        //     transparent offscreen target is bound instead.
        enum class Method { Unknown, DefaultAlpha, Offscreen };
        Method s_method = Method::Unknown;

        GLuint compile(GLenum type, char const* source) {
            auto const shader = glCreateShader(type);
            glShaderSource(shader, 1, &source, nullptr);
            glCompileShader(shader);
            GLint status = GL_FALSE;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
            if (status != GL_TRUE) {
                std::array<char, 2048> text{};
                glGetShaderInfoLog(shader, static_cast<GLsizei>(text.size()), nullptr, text.data());
                log::error("Overlay capture: shader failed to compile:\n{}", text.data());
                glDeleteShader(shader);
                return 0;
            }
            return shader;
        }

        bool buildProgram() {
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
            glLinkProgram(s_program);
            glDeleteShader(vertex);
            glDeleteShader(fragment);
            GLint status = GL_FALSE;
            glGetProgramiv(s_program, GL_LINK_STATUS, &status);
            if (status != GL_TRUE) {
                std::array<char, 2048> text{};
                glGetProgramInfoLog(s_program, static_cast<GLsizei>(text.size()), nullptr, text.data());
                log::error("Overlay capture: program failed to link:\n{}", text.data());
                glDeleteProgram(s_program);
                s_program = 0;
                return false;
            }
            s_overlayUniform = glGetUniformLocation(s_program, "u_overlay");
            log::info("Overlay capture ready: mod menus are composited into the Spout2 feed and the layout screen");
            return true;
        }

        void release(Target& target) {
            if (target.fbo) glDeleteFramebuffers(1, &target.fbo);
            if (target.texture) glDeleteTextures(1, &target.texture);
            if (target.depthStencil) glDeleteRenderbuffers(1, &target.depthStencil);
            target = {};
        }

        bool create(Target& target, GLsizei width, GLsizei height) {
            release(target);
            GLint texture = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
            glGenTextures(1, &target.texture);
            glBindTexture(GL_TEXTURE_2D, target.texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture));

            GLint drawFbo = 0;
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
            glGenFramebuffers(1, &target.fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target.fbo);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture, 0);
            GLint renderbuffer = 0;
            glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
            glGenRenderbuffers(1, &target.depthStencil);
            glBindRenderbuffer(GL_RENDERBUFFER, target.depthStencil);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
            glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(renderbuffer));
            glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, target.depthStencil);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            auto const complete = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFbo));
            if (!complete) {
                log::error("Overlay capture: framebuffer {}x{} is incomplete", width, height);
                release(target);
            }
            return complete;
        }

        bool ensureTargets(GLsizei width, GLsizei height) {
            if (s_state == State::Failed) return false;
            if (s_state == State::Untried) {
                s_state = buildProgram() ? State::Ready : State::Failed;
                if (s_state == State::Failed) {
                    log::error("Overlay capture unavailable: mod menus stay on the monitor only in Layout Mode");
                    return false;
                }
            }
            if (s_normal.fbo && s_layout.fbo && s_overlay.fbo && s_width == width && s_height == height) return true;
            if (!create(s_normal, width, height) || !create(s_layout, width, height) ||
                !create(s_overlay, width, height)) {
                release(s_normal);
                release(s_layout);
                release(s_overlay);
                s_width = s_height = 0;
                return false;
            }
            s_width = width;
            s_height = height;
            LF_DEBUG("Overlay capture targets sized {}x{}", width, height);
            debug::checkGL("overlay::ensureTargets");
            return true;
        }

        // Draws the overlay texture with premultiplied-alpha blending into the
        // currently bound draw framebuffer. Raw GL with full save/restore: the
        // menus that ran before may have left state the cocos2d cache does not
        // know about, so the cache is neither trusted nor changed here.
        void composite() {
            GLint program = 0, activeTexture = 0, texture = 0, vertexArray = 0, arrayBuffer = 0;
            GLint blendSrcRgb = 0, blendDstRgb = 0, blendSrcAlpha = 0, blendDstAlpha = 0;
            GLint viewport[4]{};
            std::array<GLint, 3> attributes{};
            glGetIntegerv(GL_CURRENT_PROGRAM, &program);
            glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
            glActiveTexture(GL_TEXTURE0);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertexArray);
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
            glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRgb);
            glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRgb);
            glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
            glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);
            glGetIntegerv(GL_VIEWPORT, viewport);
            auto const blend = glIsEnabled(GL_BLEND);
            auto const scissor = glIsEnabled(GL_SCISSOR_TEST);
            auto const depth = glIsEnabled(GL_DEPTH_TEST);
            auto const cull = glIsEnabled(GL_CULL_FACE);

            glBindVertexArray(0);
            for (GLuint index = 0; index < attributes.size(); ++index) {
                glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attributes[index]);
                glDisableVertexAttribArray(index);
            }

            glUseProgram(s_program);
            glUniform1i(s_overlayUniform, 0);
            glBindTexture(GL_TEXTURE_2D, s_overlay.texture);
            glViewport(0, 0, s_width, s_height);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glDrawArrays(GL_TRIANGLES, 0, 3);

            for (GLuint index = 0; index < attributes.size(); ++index) {
                if (attributes[index]) glEnableVertexAttribArray(index);
            }
            glBindVertexArray(static_cast<GLuint>(vertexArray));
            glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(arrayBuffer));
            glBlendFuncSeparate(
                static_cast<GLenum>(blendSrcRgb), static_cast<GLenum>(blendDstRgb),
                static_cast<GLenum>(blendSrcAlpha), static_cast<GLenum>(blendDstAlpha)
            );
            if (!blend) glDisable(GL_BLEND);
            if (scissor) glEnable(GL_SCISSOR_TEST);
            if (depth) glEnable(GL_DEPTH_TEST);
            if (cull) glEnable(GL_CULL_FACE);
            glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture));
            glActiveTexture(static_cast<GLenum>(activeTexture));
            glUseProgram(static_cast<GLuint>(program));
        }
    }

    bool begin(void (*renderLayout)()) {
        if (s_redirecting) abandon("overlay::begin");

        GLint viewport[4]{};
        glGetIntegerv(GL_VIEWPORT, viewport);
        auto const width = viewport[2];
        auto const height = viewport[3];
        if (width <= 0 || height <= 0 || !ensureTargets(width, height)) return false;

        if (s_method == Method::Unknown) {
            GLint alphaBits = 0;
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glGetIntegerv(GL_ALPHA_BITS, &alphaBits);
            s_method = alphaBits >= 8 ? Method::DefaultAlpha : Method::Offscreen;
            log::info(
                "Overlay capture method: {} (back buffer alpha bits {})",
                s_method == Method::DefaultAlpha ? "window back buffer" : "offscreen target", alphaBits
            );
        }

        // 1. Keep the normal frame for OBS (GPU blit, no CPU copy).
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glReadBuffer(GL_BACK);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_normal.fbo);
        glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

        // 2. Draw the layout version: offscreen when the back buffer is about
        //    to become the menus' canvas, otherwise straight onto it.
        glBindFramebuffer(GL_FRAMEBUFFER, s_method == Method::DefaultAlpha ? s_layout.fbo : 0);
        renderLayout();

        // 3. Give the menus a transparent canvas.
        GLfloat clear[4]{};
        GLboolean colorMask[4]{};
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
        glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
        auto const scissor = glIsEnabled(GL_SCISSOR_TEST);
        if (scissor) glDisable(GL_SCISSOR_TEST);
        GLint stencilMask = 0;
        GLboolean depthMask = GL_TRUE;
        glGetIntegerv(GL_STENCIL_WRITEMASK, &stencilMask);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
        if (s_method == Method::DefaultAlpha) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDrawBuffer(GL_BACK);
        } else {
            glBindFramebuffer(GL_FRAMEBUFFER, s_overlay.fbo);
        }
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glStencilMask(0xFF);
        glDepthMask(GL_TRUE);
        glClearColor(0.f, 0.f, 0.f, 0.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        glClearColor(clear[0], clear[1], clear[2], clear[3]);
        glDepthMask(depthMask);
        glStencilMask(static_cast<GLuint>(stencilMask));
        glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
        if (scissor) glEnable(GL_SCISSOR_TEST);

        s_redirecting = true;
        debug::checkGL("overlay::begin");
        return true;
    }

    void finish() {
        if (!s_redirecting) return;
        s_redirecting = false;

        if (s_method == Method::DefaultAlpha) {
            // The back buffer holds only what menus and overlays drew, with
            // their coverage in alpha. Lift it out, then put the layout frame
            // back on the monitor.
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            glReadBuffer(GL_BACK);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_overlay.fbo);
            glBlitFramebuffer(0, 0, s_width, s_height, 0, 0, s_width, s_height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

            glBindFramebuffer(GL_READ_FRAMEBUFFER, s_layout.fbo);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            glDrawBuffer(GL_BACK);
            glBlitFramebuffer(0, 0, s_width, s_height, 0, 0, s_width, s_height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        } else {
            GLint drawFbo = 0;
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
            if (static_cast<GLuint>(drawFbo) != s_overlay.fbo) {
                // A menu rebound the default framebuffer itself; whatever it
                // drew is already on the monitor only.
                LF_DEBUG("Overlay capture: draw framebuffer changed to {} before finish", drawFbo);
            }
        }

        // Monitor: layout + menus.
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDrawBuffer(GL_BACK);
        composite();

        // OBS: normal frame + menus, sent from the copy.
        glBindFramebuffer(GL_FRAMEBUFFER, s_normal.fbo);
        composite();
        {
            debug::CpuScope cpu(debug::Timer::Spout);
            debug::GpuScope gpu(debug::Timer::Spout);
            spout::SpoutSender::get().captureBackBuffer();
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDrawBuffer(GL_BACK);
        glReadBuffer(GL_BACK);
        debug::checkGL("overlay::finish");
    }

    bool redirecting() {
        return s_redirecting;
    }

    void abandon(char const* where) {
        if (!s_redirecting) return;
        s_redirecting = false;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDrawBuffer(GL_BACK);
        if (s_method == Method::DefaultAlpha) {
            // Never leave the transparent canvas on screen.
            glBindFramebuffer(GL_READ_FRAMEBUFFER, s_layout.fbo);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glBlitFramebuffer(0, 0, s_width, s_height, 0, 0, s_width, s_height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            glReadBuffer(GL_BACK);
        }
        log::warn("Overlay capture: the previous frame was never presented (detected at {}); redirect reset", where);
    }
}
