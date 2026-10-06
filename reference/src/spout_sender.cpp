#include "spout_sender.hpp"

#include "settings.hpp"
#include "spout_bridge.hpp"

#include <Geode/cocos/platform/win32/CCGL.h>

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

using namespace geode::prelude;

namespace cleanfeed {
    namespace {
        struct CursorBitmap final {
            HCURSOR handle = nullptr;
            std::vector<std::uint8_t> pixels;
            unsigned int width = 0;
            unsigned int height = 0;
            DWORD hotspotX = 0;
            DWORD hotspotY = 0;
        };

        struct CursorFrame final {
            CursorBitmap const* bitmap = nullptr;
            bool bitmapChanged = false;
            unsigned int pixelWidth = 0;
            unsigned int pixelHeight = 0;
            float left = 0.f;
            float top = 0.f;
            float width = 0.f;
            float height = 0.f;
        };

        struct IconInfo final {
            ICONINFO value{};

            ~IconInfo() {
                if (value.hbmColor) DeleteObject(value.hbmColor);
                if (value.hbmMask) DeleteObject(value.hbmMask);
            }
        };

        CursorBitmap s_cursorBitmap;

        struct OpenGLState final {
            GLint readFbo = 0;
            GLint drawFbo = 0;
            GLint readBuffer = 0;
            GLint drawBuffer = 0;
            GLint activeTexture = 0;
            GLint texture2D = 0;
            GLint unpackAlignment = 0;

            OpenGLState() {
                glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
                glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
                glGetIntegerv(GL_READ_BUFFER, &readBuffer);
                glGetIntegerv(GL_DRAW_BUFFER, &drawBuffer);
                glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture2D);
                glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackAlignment);
            }

            ~OpenGLState() {
                glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFbo));
                glReadBuffer(static_cast<GLenum>(readBuffer));
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFbo));
                glDrawBuffer(static_cast<GLenum>(drawBuffer));
                glActiveTexture(static_cast<GLenum>(activeTexture));
                glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture2D));
                glPixelStorei(GL_UNPACK_ALIGNMENT, unpackAlignment);
            }
        };

        bool readBitmap(
            HDC device,
            HBITMAP bitmap,
            int width,
            int height,
            std::vector<std::uint8_t>& pixels
        ) {
            if (!device || !bitmap || width <= 0 || height <= 0) return false;

            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = width;
            info.bmiHeader.biHeight = -height;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;

            pixels.assign(
                static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4,
                0
            );
            return GetDIBits(
                device, bitmap, 0, static_cast<UINT>(height), pixels.data(), &info,
                DIB_RGB_COLORS
            ) == height;
        }

        bool loadCursorBitmap(HCURSOR handle, CursorBitmap& bitmap) {
            IconInfo icon;
            if (!handle || !GetIconInfo(handle, &icon.value)) return false;

            BITMAP colorBitmap{};
            BITMAP maskBitmap{};
            if (icon.value.hbmColor) GetObject(icon.value.hbmColor, sizeof(colorBitmap), &colorBitmap);
            if (icon.value.hbmMask) GetObject(icon.value.hbmMask, sizeof(maskBitmap), &maskBitmap);

            auto const bitmapWidth = icon.value.hbmColor ? colorBitmap.bmWidth : maskBitmap.bmWidth;
            auto const bitmapHeight = icon.value.hbmColor
                ? std::abs(colorBitmap.bmHeight)
                : std::abs(maskBitmap.bmHeight) / 2;
            if (bitmapWidth <= 0 || bitmapHeight <= 0) return false;

            auto* device = GetDC(nullptr);
            if (!device) return false;

            std::vector<std::uint8_t> pixels;
            auto success = false;
            if (icon.value.hbmColor) {
                success = readBitmap(
                    device, icon.value.hbmColor, bitmapWidth, bitmapHeight, pixels
                );
                if (success) {
                    auto hasAlpha = false;
                    for (std::size_t i = 3; i < pixels.size(); i += 4) {
                        if (pixels[i] != 0) {
                            hasAlpha = true;
                            break;
                        }
                    }

                    // Legacy color cursors store transparency only in the AND mask.
                    if (!hasAlpha && icon.value.hbmMask) {
                        std::vector<std::uint8_t> mask;
                        if (readBitmap(device, icon.value.hbmMask, bitmapWidth, bitmapHeight, mask)) {
                            for (std::size_t i = 0; i < pixels.size(); i += 4) {
                                pixels[i + 3] = mask[i] > 127 ? 0 : 255;
                            }
                        }
                    }
                }
            } else if (icon.value.hbmMask) {
                std::vector<std::uint8_t> mask;
                success = readBitmap(
                    device, icon.value.hbmMask, bitmapWidth, bitmapHeight * 2, mask
                );
                if (success) {
                    pixels.assign(
                        static_cast<std::size_t>(bitmapWidth) *
                            static_cast<std::size_t>(bitmapHeight) * 4,
                        0
                    );
                    auto const halfBytes = static_cast<std::size_t>(bitmapWidth) *
                        static_cast<std::size_t>(bitmapHeight) * 4;
                    for (std::size_t i = 0; i < halfBytes; i += 4) {
                        auto const andBit = mask[i] > 127;
                        auto const xorBit = mask[halfBytes + i] > 127;
                        if (andBit && !xorBit) continue;

                        auto const color = static_cast<std::uint8_t>(xorBit ? 255 : 0);
                        pixels[i] = color;
                        pixels[i + 1] = color;
                        pixels[i + 2] = color;
                        pixels[i + 3] = 255;
                    }
                }
            }
            ReleaseDC(nullptr, device);
            if (!success) return false;

            bitmap.handle = handle;
            bitmap.pixels = std::move(pixels);
            bitmap.width = static_cast<unsigned int>(bitmapWidth);
            bitmap.height = static_cast<unsigned int>(bitmapHeight);
            bitmap.hotspotX = icon.value.xHotspot;
            bitmap.hotspotY = icon.value.yHotspot;
            return true;
        }

        bool cursorFrame(
            unsigned int framebufferWidth,
            unsigned int framebufferHeight,
            CursorFrame& frame
        ) {
            CURSORINFO cursor{};
            cursor.cbSize = sizeof(cursor);
            if (!GetCursorInfo(&cursor) || !(cursor.flags & CURSOR_SHOWING) || !cursor.hCursor) {
                return false;
            }

            auto* window = WindowFromDC(wglGetCurrentDC());
            if (!window) return false;

            POINT position = cursor.ptScreenPos;
            RECT client{};
            if (!ScreenToClient(window, &position) || !GetClientRect(window, &client)) return false;

            auto const clientWidth = client.right - client.left;
            auto const clientHeight = client.bottom - client.top;
            if (clientWidth <= 0 || clientHeight <= 0) return false;
            if (
                position.x < client.left || position.y < client.top ||
                position.x >= client.right || position.y >= client.bottom
            ) {
                return false;
            }

            auto const bitmapChanged = s_cursorBitmap.handle != cursor.hCursor;
            if (bitmapChanged && !loadCursorBitmap(cursor.hCursor, s_cursorBitmap)) return false;

            auto const scaleX = static_cast<float>(framebufferWidth) /
                static_cast<float>(clientWidth);
            auto const scaleY = static_cast<float>(framebufferHeight) /
                static_cast<float>(clientHeight);
            frame.bitmap = &s_cursorBitmap;
            frame.bitmapChanged = bitmapChanged;
            frame.pixelWidth = s_cursorBitmap.width;
            frame.pixelHeight = s_cursorBitmap.height;
            frame.left = (static_cast<float>(position.x) - s_cursorBitmap.hotspotX) * scaleX;
            frame.top = (static_cast<float>(position.y) - s_cursorBitmap.hotspotY) * scaleY;
            frame.width = static_cast<float>(s_cursorBitmap.width) * scaleX;
            frame.height = static_cast<float>(s_cursorBitmap.height) * scaleY;
            return true;
        }

        void drawCursorTexture(
            GLuint texture,
            CursorFrame const& cursor,
            unsigned int width,
            unsigned int height
        ) {
            GLint previousProgram = 0;
            GLint previousMatrixMode = 0;
            glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
            glGetIntegerv(GL_MATRIX_MODE, &previousMatrixMode);

            glPushAttrib(
                GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT | GL_ENABLE_BIT |
                GL_SCISSOR_BIT | GL_TEXTURE_BIT | GL_TRANSFORM_BIT | GL_VIEWPORT_BIT
            );
            glUseProgram(0);
            glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
            glDisable(GL_ALPHA_TEST);
            glDisable(GL_CULL_FACE);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_SCISSOR_TEST);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
            glColor4f(1.f, 1.f, 1.f, 1.f);

            glMatrixMode(GL_PROJECTION);
            glPushMatrix();
            glLoadIdentity();
            glMatrixMode(GL_MODELVIEW);
            glPushMatrix();
            glLoadIdentity();

            auto const left = cursor.left / static_cast<float>(width) * 2.f - 1.f;
            auto const right = (cursor.left + cursor.width) / static_cast<float>(width) * 2.f - 1.f;
            auto const top = 1.f - cursor.top / static_cast<float>(height) * 2.f;
            auto const bottom = 1.f - (cursor.top + cursor.height) /
                static_cast<float>(height) * 2.f;

            // GetDIBits returns top-down pixels, so v=0 belongs to the top edge.
            glBegin(GL_QUADS);
            glTexCoord2f(0.f, 1.f); glVertex2f(left, bottom);
            glTexCoord2f(1.f, 1.f); glVertex2f(right, bottom);
            glTexCoord2f(1.f, 0.f); glVertex2f(right, top);
            glTexCoord2f(0.f, 0.f); glVertex2f(left, top);
            glEnd();

            glPopMatrix();
            glMatrixMode(GL_PROJECTION);
            glPopMatrix();
            glMatrixMode(static_cast<GLenum>(previousMatrixMode));
            glUseProgram(static_cast<GLuint>(previousProgram));
            glPopAttrib();
        }
    }

    SpoutSender& SpoutSender::get() {
        static auto* instance = new SpoutSender();
        return *instance;
    }

    bool SpoutSender::ensureCompositionTarget(unsigned int width, unsigned int height) {
        if (
            m_compositionFbo && m_compositionTexture &&
            m_compositionWidth == width && m_compositionHeight == height
        ) {
            return true;
        }

        releaseCompositionTarget();
        glGenTextures(1, &m_compositionTexture);
        glBindTexture(GL_TEXTURE_2D, m_compositionTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(
            GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(width),
            static_cast<GLsizei>(height), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr
        );

        glGenFramebuffers(1, &m_compositionFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, m_compositionFbo);
        glFramebufferTexture2D(
            GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_compositionTexture, 0
        );
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            if (!m_warnedCursorFailure) {
                log::error("Unable to create the cursor composition framebuffer");
                m_warnedCursorFailure = true;
            }
            releaseCompositionTarget();
            return false;
        }

        m_compositionWidth = width;
        m_compositionHeight = height;
        m_warnedCursorFailure = false;
        return true;
    }

    bool SpoutSender::ensureCursorTexture(unsigned int width, unsigned int height) {
        if (m_cursorTexture && m_cursorWidth == width && m_cursorHeight == height) return true;

        if (m_cursorTexture) glDeleteTextures(1, &m_cursorTexture);
        m_cursorTexture = 0;
        m_cursorWidth = 0;
        m_cursorHeight = 0;

        glGenTextures(1, &m_cursorTexture);
        if (!m_cursorTexture) return false;
        glBindTexture(GL_TEXTURE_2D, m_cursorTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(
            GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(width),
            static_cast<GLsizei>(height), 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr
        );
        m_cursorWidth = width;
        m_cursorHeight = height;
        m_cursorTextureNeedsUpload = true;
        return true;
    }

    void SpoutSender::releaseCompositionTarget() {
        if (m_compositionFbo) glDeleteFramebuffers(1, &m_compositionFbo);
        if (m_compositionTexture) glDeleteTextures(1, &m_compositionTexture);
        if (m_cursorTexture) glDeleteTextures(1, &m_cursorTexture);
        m_compositionFbo = 0;
        m_compositionTexture = 0;
        m_compositionWidth = 0;
        m_compositionHeight = 0;
        m_cursorTexture = 0;
        m_cursorWidth = 0;
        m_cursorHeight = 0;
        m_cursorTextureNeedsUpload = true;
    }

    void SpoutSender::captureBackBuffer() {
        if (!settings::enabled()) {
            releaseSender();
            return;
        }

        GLint viewport[4]{};
        glGetIntegerv(GL_VIEWPORT, viewport);
        auto const width = static_cast<unsigned int>(std::max(0, viewport[2]));
        auto const height = static_cast<unsigned int>(std::max(0, viewport[3]));
        if (!width || !height) return;

        auto const& desiredName = settings::senderName();
        if (desiredName != m_senderName) {
            releaseSender();
            m_senderName = desiredName;
            if (!m_spout) m_spout = bridge::create();
            bridge::setSenderName(m_spout, m_senderName.c_str());
        }

        OpenGLState const state;
        auto const captureFbo = static_cast<GLuint>(state.drawFbo);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, captureFbo);
        glReadBuffer(captureFbo == 0 ? GL_BACK : static_cast<GLenum>(state.drawBuffer));

        auto const captureCursor = settings::captureCursor();
        if (!captureCursor && m_compositionFbo) releaseCompositionTarget();

        auto sendFbo = captureFbo;
        CursorFrame cursor;
        if (
            captureCursor && cursorFrame(width, height, cursor) &&
            ensureCompositionTarget(width, height) &&
            ensureCursorTexture(cursor.pixelWidth, cursor.pixelHeight)
        ) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, captureFbo);
            glReadBuffer(captureFbo == 0 ? GL_BACK : static_cast<GLenum>(state.drawBuffer));
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, m_compositionTexture);
            glCopyTexSubImage2D(
                GL_TEXTURE_2D, 0, 0, 0, viewport[0], viewport[1],
                static_cast<GLsizei>(width), static_cast<GLsizei>(height)
            );

            glBindFramebuffer(GL_FRAMEBUFFER, m_compositionFbo);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            glBindTexture(GL_TEXTURE_2D, m_cursorTexture);
            if (cursor.bitmapChanged) m_cursorTextureNeedsUpload = true;
            if (m_cursorTextureNeedsUpload && cursor.bitmap) {
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                glTexSubImage2D(
                    GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(cursor.pixelWidth),
                    static_cast<GLsizei>(cursor.pixelHeight), GL_BGRA, GL_UNSIGNED_BYTE,
                    cursor.bitmap->pixels.data()
                );
                m_cursorTextureNeedsUpload = false;
            }
            drawCursorTexture(m_cursorTexture, cursor, width, height);

            sendFbo = m_compositionFbo;
            glBindFramebuffer(GL_READ_FRAMEBUFFER, sendFbo);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
        }

        glBindFramebuffer(GL_READ_FRAMEBUFFER, sendFbo);
        glReadBuffer(sendFbo == captureFbo
            ? (captureFbo == 0 ? GL_BACK : static_cast<GLenum>(state.drawBuffer))
            : GL_COLOR_ATTACHMENT0
        );
        auto const sent = bridge::sendFbo(m_spout, sendFbo, width, height, true);

        if (!sent) {
            if (!m_warnedSendFailure) {
                log::error("Spout2 rejected the clean-feed framebuffer");
                m_warnedSendFailure = true;
            }
            return;
        }

        m_warnedSendFailure = false;
        if (!m_loggedPublishing) {
            log::info(
                "Spout2 sender '{}' accepted {}x{} frames (share mode {})",
                m_senderName, width, height, bridge::shareMode(m_spout)
            );
            m_loggedPublishing = true;
        }
    }

    void SpoutSender::releaseSender() {
        if (!m_senderName.empty()) bridge::releaseSender(m_spout);
        m_senderName.clear();
        m_warnedSendFailure = false;
        m_loggedPublishing = false;
    }

    void SpoutSender::shutdown() {
        releaseSender();
        releaseCompositionTarget();
        bridge::destroy(m_spout);
        m_spout = nullptr;
    }
}
