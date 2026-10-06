#pragma once

namespace cleanfeed::bridge {
    void* create();
    void destroy(void* handle);
    void setSenderName(void* handle, char const* name);
    void releaseSender(void* handle);
    bool sendFbo(
        void* handle,
        unsigned int fbo,
        unsigned int width,
        unsigned int height,
        bool invert
    );
    int shareMode(void* handle);
}
