// Adapted from Silicate commit f183dbc5 (GPL-3.0).
// See LICENSE and THIRD_PARTY_NOTICES.md.
#include "object.hpp"

#include <Geode/Geode.hpp>

namespace phys {
bool activatedPlatformer(EnhancedGameObject* object, bool isPlatformer) {
    if (!isPlatformer) {
        if (object->m_isMultiActivate) {
            return true;
        }
    } else if (object->m_isNoMultiActivate) {
        return true;
    }

    return false;
}

void* hasBeenActivatedByPlayerOrig = nullptr;

$execute {
    hasBeenActivatedByPlayerOrig =
        reinterpret_cast<void*>(geode::base::get() + 0x1a1b70);
}

// this doesn't work in trajectory for some odd reason
bool hasBeenActivatedByPlayer(PlayerObject* player,
                              EnhancedGameObject* object) {
    bool activatedPlatformer =
        phys::activatedPlatformer(object, player->m_isPlatformer);
    if (activatedPlatformer) {
        return false;
    }

    if (!player->m_isSecondPlayer) {
        return object->m_activatedByPlayer1;
    }

    return object->m_activatedByPlayer2;

    // auto func = reinterpret_cast<bool (*)(EnhancedGameObject*,
    // PlayerObject*)>(hasBeenActivatedByPlayerOrig); return func(object,
    // player);
}
}  // namespace phys
