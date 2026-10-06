#include "present_hook.hpp"

#include "debug.hpp"
#include "settings.hpp"

#include <Geode/Geode.hpp>
#include <Geode/cocos/platform/win32/CCGL.h>

#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <array>
#include <cstddef>

using namespace geode::prelude;

namespace layoutfeed::present_hook {
    namespace {
        // Overlays hook gdi32 SwapBuffers or opengl32 wglSwapBuffers. Both end
        // in the installable client driver, so its exports are the last point
        // where the finished back buffer, overlays included, can be read.
        using SwapBuffersFn = BOOL(WINAPI*)(HDC);
        using PresentBuffersFn = BOOL(WINAPI*)(HDC, void*);

        // Hybrid-GPU systems can load more than one OpenGL driver.
        constexpr std::size_t kMaxDrivers = 4;
        // Consecutive frames the driver hook may miss before falling back.
        constexpr int kMaxMissedFrames = 3;

        enum class State { Untried, Ready, Unavailable };

        // cocos2d caches bound programs, textures, blending and vertex
        // attributes. Overlays drawing between the cocos frame and the driver
        // present may leave different GL state behind, so the state that
        // matches the cocos cache is restored before player-only drawing.
        struct GLSnapshot final {
            GLint program = 0;
            GLint activeTexture = GL_TEXTURE0;
            GLint texture2D = 0;
            GLint arrayBuffer = 0;
            GLint vertexArray = 0;
            GLint readFbo = 0;
            GLint drawFbo = 0;
            GLint blendSrc = GL_ONE;
            GLint blendDst = GL_ZERO;
            GLint viewport[4]{};
            GLint scissorBox[4]{};
            GLboolean blend = GL_FALSE;
            GLboolean scissorTest = GL_FALSE;
            GLboolean depthTest = GL_FALSE;
            GLboolean cullFace = GL_FALSE;
            std::array<GLint, 3> attributes{};

            void capture() {
                glGetIntegerv(GL_CURRENT_PROGRAM, &program);
                glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
                glActiveTexture(GL_TEXTURE0);
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture2D);
                glActiveTexture(static_cast<GLenum>(activeTexture));
                glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
                glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertexArray);
                glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
                glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
                glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrc);
                glGetIntegerv(GL_BLEND_DST_RGB, &blendDst);
                glGetIntegerv(GL_VIEWPORT, viewport);
                glGetIntegerv(GL_SCISSOR_BOX, scissorBox);
                blend = glIsEnabled(GL_BLEND);
                scissorTest = glIsEnabled(GL_SCISSOR_TEST);
                depthTest = glIsEnabled(GL_DEPTH_TEST);
                cullFace = glIsEnabled(GL_CULL_FACE);
                for (GLuint index = 0; index < attributes.size(); ++index) {
                    glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attributes[index]);
                }
            }

            void apply() const {
                auto const toggle = [](GLenum capability, GLboolean enabled) {
                    if (enabled) glEnable(capability);
                    else glDisable(capability);
                };

                glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFbo));
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFbo));
                glUseProgram(static_cast<GLuint>(program));
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture2D));
                glActiveTexture(static_cast<GLenum>(activeTexture));
                glBindVertexArray(static_cast<GLuint>(vertexArray));
                glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(arrayBuffer));
                for (GLuint index = 0; index < attributes.size(); ++index) {
                    if (attributes[index]) glEnableVertexAttribArray(index);
                    else glDisableVertexAttribArray(index);
                }
                glBlendFunc(static_cast<GLenum>(blendSrc), static_cast<GLenum>(blendDst));
                toggle(GL_BLEND, blend);
                toggle(GL_SCISSOR_TEST, scissorTest);
                toggle(GL_DEPTH_TEST, depthTest);
                toggle(GL_CULL_FACE, cullFace);
                glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
                glScissor(scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3]);
            }
        };

        State s_state = State::Untried;
        void (*s_compose)() = nullptr;
        DWORD s_thread = 0;
        bool s_pending = false;
        bool s_composing = false;
        int s_missedFrames = 0;
        GLSnapshot s_cocosState;

        std::array<SwapBuffersFn, kMaxDrivers> s_swapBuffers{};
        std::array<PresentBuffersFn, kMaxDrivers> s_presentBuffers{};

        void composeDeferred() {
            if (!s_pending || s_composing || GetCurrentThreadId() != s_thread) return;
            s_pending = false;
            s_missedFrames = 0;
            s_composing = true;
            // The driver presents the default framebuffer regardless of GL
            // state, so the cocos-consistent state is left in place for the
            // next frame instead of restoring what the overlays left behind.
            s_cocosState.apply();
            if (s_compose) s_compose();
            s_composing = false;
        }

        // Calling the original address inside a Geode detour continues the
        // hook chain into the driver.
        template <std::size_t Index>
        BOOL WINAPI swapBuffersDetour(HDC device) {
            composeDeferred();
            return s_swapBuffers[Index](device);
        }

        template <std::size_t Index>
        BOOL WINAPI presentBuffersDetour(HDC device, void* data) {
            composeDeferred();
            return s_presentBuffers[Index](device, data);
        }

        template <std::size_t... Indices>
        constexpr auto swapDetours(std::index_sequence<Indices...>) {
            return std::array<SwapBuffersFn, sizeof...(Indices)>{&swapBuffersDetour<Indices>...};
        }

        template <std::size_t... Indices>
        constexpr auto presentDetours(std::index_sequence<Indices...>) {
            return std::array<PresentBuffersFn, sizeof...(Indices)>{&presentBuffersDetour<Indices>...};
        }

        constexpr auto kSwapDetours = swapDetours(std::make_index_sequence<kMaxDrivers>{});
        constexpr auto kPresentDetours = presentDetours(std::make_index_sequence<kMaxDrivers>{});

        std::string moduleName(HMODULE module) {
            char path[MAX_PATH]{};
            auto const length = GetModuleFileNameA(module, path, MAX_PATH);
            std::string_view const name{path, length};
            auto const slash = name.find_last_of("\\/");
            return std::string(slash == std::string_view::npos ? name : name.substr(slash + 1));
        }

        template <class Function>
        bool hookExport(Function detour, void* address, std::string const& name) {
            auto result = Mod::get()->hook(address, detour, name);
            if (result.isErr()) {
                log::warn("Unable to hook {}: {}", name, result.unwrapErr());
                return false;
            }
            return true;
        }

        void install() {
            s_state = State::Unavailable;

            std::array<HMODULE, 1024> modules{};
            DWORD needed = 0;
            if (!K32EnumProcessModules(
                GetCurrentProcess(), modules.data(),
                static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &needed
            )) {
                log::warn("System overlay capture: unable to enumerate loaded modules");
                return;
            }

            auto const count = std::min<std::size_t>(needed / sizeof(HMODULE), modules.size());
            auto* const opengl = GetModuleHandleW(L"opengl32.dll");
            std::size_t slot = 0;
            for (std::size_t index = 0; index < count && slot < kMaxDrivers; ++index) {
                auto* const module = modules[index];
                if (!module || module == opengl) continue;

                auto* const swap = reinterpret_cast<void*>(GetProcAddress(module, "DrvSwapBuffers"));
                auto* const present = reinterpret_cast<void*>(GetProcAddress(module, "DrvPresentBuffers"));
                if (!swap && !present) continue;

                auto const name = moduleName(module);
                auto hooked = false;
                if (swap) {
                    s_swapBuffers[slot] = reinterpret_cast<SwapBuffersFn>(swap);
                    hooked |= hookExport(kSwapDetours[slot], swap, name + "!DrvSwapBuffers");
                }
                if (present) {
                    s_presentBuffers[slot] = reinterpret_cast<PresentBuffersFn>(present);
                    hooked |= hookExport(kPresentDetours[slot], present, name + "!DrvPresentBuffers");
                }
                if (!hooked) continue;

                log::info("System overlay capture hooked OpenGL driver {}", name);
                s_state = State::Ready;
                ++slot;
            }

            if (s_state != State::Ready) {
                log::warn("System overlay capture: no OpenGL driver present hook available");
            }
        }
    }

    bool deferFrame(void (*compose)()) {
        if (!settings::captureSystemOverlays()) {
            s_pending = false;
            s_missedFrames = 0;
            return false;
        }

        // The driver is loaded once the GL context exists, which is
        // guaranteed by the time the first frame is swapped.
        if (s_state == State::Untried) install();
        if (s_state != State::Ready) return false;

        if (s_pending && ++s_missedFrames >= kMaxMissedFrames) {
            s_state = State::Unavailable;
            s_pending = false;
            log::warn(
                "System overlay capture: the driver present hook is not reached; "
                "using normal capture for this session"
            );
            return false;
        }

        s_compose = compose;
        s_thread = GetCurrentThreadId();
        s_cocosState.capture();
        s_pending = true;
        return true;
    }
}
