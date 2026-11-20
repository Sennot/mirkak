#include "vanilla.hpp"

#include "../debug.hpp"

#include <Geode/Geode.hpp>

#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

using namespace geode::prelude;

namespace layoutfeed::vanilla {
    namespace {
        unsigned s_resolved = 0;
        unsigned s_rejected = 0;

        bool readMemory(std::uintptr_t address, void* out, std::size_t size) {
            SIZE_T read = 0;
            return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void const*>(address), out, size, &read) &&
                read == size;
        }

        bool executable(std::uintptr_t address) {
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(reinterpret_cast<void const*>(address), &info, sizeof(info))) return false;
            if (info.State != MEM_COMMIT) return false;
            switch (info.Protect & 0xff) {
                case PAGE_EXECUTE:
                case PAGE_EXECUTE_READ:
                case PAGE_EXECUTE_READWRITE:
                case PAGE_EXECUTE_WRITECOPY:
                    return true;
                default:
                    return false;
            }
        }

        // Detours live in mod DLLs; TulipHook's trampolines live in memory it
        // allocated itself.
        bool insideModule(std::uintptr_t address) {
            HMODULE module = nullptr;
            return GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(address), &module
            ) && module;
        }

        // The file of the module containing `address`, mapped as an image
        // without running it, to read a function's bytes before hooking.
        std::uintptr_t diskImageFor(std::uintptr_t address, std::uintptr_t& moduleBase) {
            HMODULE module = nullptr;
            if (!GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(address), &module
                ) || !module) {
                return 0;
            }
            moduleBase = reinterpret_cast<std::uintptr_t>(module);

            static std::unordered_map<std::uintptr_t, std::uintptr_t> s_images;
            if (auto const it = s_images.find(moduleBase); it != s_images.end()) return it->second;

            std::array<wchar_t, 1024> path{};
            auto const length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
            std::uintptr_t image = 0;
            if (length && length < path.size()) {
                if (auto* mapped = LoadLibraryExW(path.data(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE)) {
                    // Data and image-resource handles carry flags in the low bits.
                    image = reinterpret_cast<std::uintptr_t>(mapped) & ~static_cast<std::uintptr_t>(3);
                }
            }
            s_images.emplace(moduleBase, image);
            return image;
        }

        // The handler starts with `push rbp; mov rbp, rsp; sub rsp, 0xc0`,
        // saves the argument registers and then loads its two parameters:
        //   mov rcx, [rip + content]   48 8B 0D disp32
        //   mov rdx, [rip + original]  48 8B 15 disp32
        // `original` is the hooked address itself, which identifies the
        // handler; `content` points to the detour list (a std::vector whose
        // last entry, priority INT32_MAX, is the trampoline).
        void* resolveTarget(std::uintptr_t target, std::string_view name) {
            auto const reject = [&](char const* why) -> void* {
                ++s_rejected;
                log::warn("Level Overlay: no direct call for {} ({}); other mods can see level B there", name, why);
                return nullptr;
            };

            std::array<std::uint8_t, 14> entry{};
            if (!readMemory(target, entry.data(), entry.size())) return reject("unreadable entry");
            std::uintptr_t handler = 0;
            if (entry[0] == 0xE9) {
                std::int32_t relative = 0;
                std::memcpy(&relative, entry.data() + 1, sizeof(relative));
                handler = target + 5 + static_cast<std::intptr_t>(relative);
            } else if (entry[0] == 0xFF && entry[1] == 0x25 && !entry[2] && !entry[3] && !entry[4] && !entry[5]) {
                std::memcpy(&handler, entry.data() + 6, sizeof(handler));
            } else {
                return reject("entry is not a hook jump");
            }
            if (!executable(handler) || insideModule(handler)) return reject("unexpected handler");

            std::array<std::uint8_t, 0x140> code{};
            if (!readMemory(handler, code.data(), code.size())) return reject("unreadable handler");
            if (code[0] != 0x55) return reject("unexpected handler prologue");

            std::uintptr_t content = 0;
            for (std::size_t i = 1; i + 14 <= code.size(); ++i) {
                if (code[i] != 0x48 || code[i + 1] != 0x8B || code[i + 2] != 0x0D) continue;
                if (code[i + 7] != 0x48 || code[i + 8] != 0x8B || code[i + 9] != 0x15) continue;
                std::int32_t contentDisp = 0;
                std::int32_t originalDisp = 0;
                std::memcpy(&contentDisp, code.data() + i + 3, sizeof(contentDisp));
                std::memcpy(&originalDisp, code.data() + i + 10, sizeof(originalDisp));
                auto const contentSlot = handler + i + 7 + static_cast<std::intptr_t>(contentDisp);
                auto const originalSlot = handler + i + 14 + static_cast<std::intptr_t>(originalDisp);
                std::uintptr_t original = 0;
                if (!readMemory(contentSlot, &content, sizeof(content)) ||
                    !readMemory(originalSlot, &original, sizeof(original))) {
                    return reject("unreadable handler data");
                }
                if (original != target) return reject("handler belongs to another function");
                break;
            }
            if (!content) return reject("handler layout not recognised");

            // std::vector<void*> on MSVC: first, last, end of storage.
            std::array<std::uintptr_t, 3> vector{};
            if (!readMemory(content, vector.data(), sizeof(vector))) return reject("unreadable detour list");
            auto const first = vector[0];
            auto const last = vector[1];
            auto const capacity = vector[2];
            if (!first || last <= first || capacity < last || (last - first) % sizeof(void*) ||
                (last - first) / sizeof(void*) > 4096) {
                return reject("invalid detour list");
            }
            std::uintptr_t trampoline = 0;
            if (!readMemory(last - sizeof(void*), &trampoline, sizeof(trampoline))) return reject("unreadable trampoline");
            if (trampoline == target || !executable(trampoline) || insideModule(trampoline)) {
                return reject("last detour is not a trampoline");
            }

            // The trampoline begins with the function's own first instruction.
            std::uintptr_t moduleBase = 0;
            if (auto const image = diskImageFor(target, moduleBase)) {
                std::array<std::uint8_t, 2> onDisk{};
                std::array<std::uint8_t, 2> relocated{};
                if (!readMemory(image + (target - moduleBase), onDisk.data(), onDisk.size()) ||
                    !readMemory(trampoline, relocated.data(), relocated.size()) || onDisk != relocated) {
                    return reject("trampoline does not start like the function");
                }
            }

            ++s_resolved;
            LF_DEBUG("Level Overlay: direct call for {} at {:#x}", name, trampoline);
            return reinterpret_cast<void*>(trampoline);
        }
    }

    void* find(std::string_view hookName) {
        for (auto* hook : Mod::get()->getHooks()) {
            if (hook && hook->getDisplayName() == hookName && hook->isEnabled()) {
                return resolveTarget(hook->getAddress(), hookName);
            }
        }
        ++s_rejected;
        log::warn("Level Overlay: hook {} not found", hookName);
        return nullptr;
    }

    void logSummary() {
        log::info("Level Overlay: {} functions called directly for level B, {} fall back to the hook chain",
                  s_resolved, s_rejected);
    }
}
