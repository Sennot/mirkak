#include "spout_bridge.hpp"

#include <Spout.h>

namespace cleanfeed::bridge {
    void* create() {
        return new Spout();
    }

    void destroy(void* handle) {
        delete static_cast<Spout*>(handle);
    }

    void setSenderName(void* handle, char const* name) {
        if (handle) static_cast<Spout*>(handle)->SetSenderName(name);
    }

    void releaseSender(void* handle) {
        if (handle) static_cast<Spout*>(handle)->ReleaseSender();
    }

    bool sendFbo(
        void* handle,
        unsigned int fbo,
        unsigned int width,
        unsigned int height,
        bool invert
    ) {
        return handle && static_cast<Spout*>(handle)->SendFbo(fbo, width, height, invert);
    }

    int shareMode(void* handle) {
        return handle ? static_cast<Spout*>(handle)->GetShareMode() : -1;
    }
}
