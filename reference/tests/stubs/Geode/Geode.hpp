#pragma once

// Minimal host for the real settings, keybind and drawing-gate code. These tests
// model stopped gameplay updates; they do not emulate Geometry Dash physics.
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cocos2d {
    struct ccColor4B { uint8_t r, g, b, a; };
    struct ccColor4F { float r, g, b, a; };

    class CCNode {
    public:
        virtual ~CCNode() = default;
        virtual void visit() {}
    };

    class CCDrawNode : public CCNode {
    public:
        bool m_bUseArea = true;
        int drawCalls = 0;
        bool visible = true;
        bool init() { return true; }
        void autorelease() {}
        void setVisible(bool value) { visible = value; }
        void visit() override { if (visible) ++drawCalls; }
    };
}

class GJBaseGameLayer {};

namespace geode {
    struct SettingV3 {};
    struct Keybind {};
    struct ListenerHandle {};
    using SettingCallback = std::function<void(std::string_view, std::shared_ptr<SettingV3>)>;
    using KeybindCallback = std::function<void(Keybind const&, bool, bool, double)>;
    inline std::vector<SettingCallback> settingListeners;
    inline std::map<std::string, KeybindCallback, std::less<>> keybindListeners;
    inline ListenerHandle listenerHandle;

    class Mod {
    public:
        using Value = std::variant<bool, int64_t, double, std::string, cocos2d::ccColor4B>;
        std::map<std::string, Value, std::less<>> values;
        int settingReads = 0;
        static Mod* get() { static Mod instance; return &instance; }

        template <class T>
        T getSettingValue(std::string_view key) {
            ++settingReads;
            return std::get<T>(values.at(std::string(key)));
        }

        template <class T>
        void setSettingValue(std::string_view key, T value) {
            values[std::string(key)] = value;
            for (auto const& listener : settingListeners) listener(key, nullptr);
        }
    };

    template <class Callback>
    ListenerHandle* listenForAllSettingChanges(Callback callback) {
        settingListeners.emplace_back(callback);
        return &listenerHandle;
    }

    template <class Callback>
    ListenerHandle* listenForKeybindSettingPresses(std::string key, Callback callback) {
        keybindListeners.emplace(std::move(key), callback);
        return &listenerHandle;
    }

    namespace prelude { using namespace geode; }
}

#define $on_game(event) void registerTestKeybinds()
