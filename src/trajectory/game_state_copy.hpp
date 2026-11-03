#pragma once

#include <Geode/Geode.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>

// Trajectory prediction snapshots the whole GJGameState before simulating and
// restores it afterwards (Silicate approach). Since geode-sdk/bindings#1347
// renamed members keep their old names through [[renamed_from]], which the
// code generator emits as anonymous unions of CCPoints, so GJGameState has no
// implicit copy constructor or copy assignment any more.
//
// Every member is either plain data (numbers, flags, pointers, points,
// colors), which is copied bytewise, or one of the containers below, which
// are copied with their own copy operations so the copy owns its memory.
// Keep this list in sync with GJGameState in the pinned bindings
// (.github/workflows/build.yml); a container missing here would be shared.
#define LAYOUTFEED_GAME_STATE_CONTAINERS(X) \
    X(m_spawnChannelRelated0)               \
    X(m_spawnChannelRelated1)               \
    X(m_tweenActions)                       \
    X(m_gameObjectPhysics)                  \
    X(m_unkVecFloat1)                       \
    X(m_activatedObjectIDs)                 \
    X(m_stateObjects)                       \
    X(m_unkMapPairGJGameEventIntVectorEventTriggerInstance) \
    X(m_unkMapPairGJGameEventIntInt)        \
    X(m_enterEffectInstanceVectors)         \
    X(m_exitEffectInstanceVectors)          \
    X(m_enterChannelMap)                    \
    X(m_exitChannelMap)                     \
    X(m_moveEffectInstances)                \
    X(m_rotateEffectInstances)              \
    X(m_scaleEffectInstances)               \
    X(m_fadeEffectInstances)                \
    X(m_tintEffectInstances)                \
    X(m_unsortedAreaEffects)                \
    X(m_advanceFollowInstances)             \
    X(m_dynamicMoveActions)                 \
    X(m_dynamicRotateActions)               \
    X(m_unkUMap8)                           \
    X(m_proximityVolumeRelated)             \
    X(m_songChannelStates)                  \
    X(m_songTriggerStateVectors)            \
    X(m_sfxTriggerStates)

namespace layoutfeed::game_state {
    template <class Member>
    std::size_t offsetIn(GJGameState const& state, Member const& member) {
        return static_cast<std::size_t>(
            reinterpret_cast<unsigned char const*>(std::addressof(member)) -
            reinterpret_cast<unsigned char const*>(std::addressof(state))
        );
    }

    // dst = src, member by member.
    inline void assign(GJGameState& dst, GJGameState const& src) {
        if (std::addressof(dst) == std::addressof(src)) return;
        // Keep dst's own container objects, take every byte from src, then put
        // dst's containers back and copy-assign them from src.
        auto saved = std::make_unique<std::array<unsigned char, sizeof(GJGameState)>>();
        std::memcpy(saved->data(), static_cast<void const*>(std::addressof(dst)), sizeof(GJGameState));
        std::memcpy(static_cast<void*>(std::addressof(dst)), static_cast<void const*>(std::addressof(src)), sizeof(GJGameState));
#define LAYOUTFEED_ASSIGN(name)                                                                     \
        std::memcpy(static_cast<void*>(std::addressof(dst.name)), saved->data() + offsetIn(dst, dst.name), \
                    sizeof(dst.name));                                                              \
        dst.name = src.name;
        LAYOUTFEED_GAME_STATE_CONTAINERS(LAYOUTFEED_ASSIGN)
#undef LAYOUTFEED_ASSIGN
    }

    // An owning copy of a GJGameState, built without its deleted copy
    // constructor.
    class Snapshot final {
    public:
        explicit Snapshot(GJGameState const& src) {
            std::memcpy(m_storage, static_cast<void const*>(std::addressof(src)), sizeof(GJGameState));
#define LAYOUTFEED_CONSTRUCT(name)                                                                  \
            {                                                                                       \
                using Type = std::remove_cvref_t<decltype(src.name)>;                               \
                ::new (static_cast<void*>(m_storage + offsetIn(src, src.name))) Type(src.name);     \
            }
            LAYOUTFEED_GAME_STATE_CONTAINERS(LAYOUTFEED_CONSTRUCT)
#undef LAYOUTFEED_CONSTRUCT
        }

        ~Snapshot() {
            std::destroy_at(&get());
        }

        Snapshot(Snapshot const&) = delete;
        Snapshot& operator=(Snapshot const&) = delete;

        GJGameState const& get() const {
            return *std::launder(reinterpret_cast<GJGameState const*>(m_storage));
        }

    private:
        GJGameState& get() {
            return *std::launder(reinterpret_cast<GJGameState*>(m_storage));
        }

        alignas(GJGameState) unsigned char m_storage[sizeof(GJGameState)];
    };
}
