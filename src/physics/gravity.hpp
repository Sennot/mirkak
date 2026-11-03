// Adapted from Silicate commit f183dbc5 (GPL-3.0).
// See LICENSE and THIRD_PARTY_NOTICES.md.
#pragma once

#include <Geode/Geode.hpp>

using namespace geode::prelude;

namespace phys {
void flipGravity(PlayerObject* player, bool gravity);
void propellPlayer(PlayerObject* player, float force, bool playBumpEffect,
                   int p2);
void bumpPlayer(PlayerObject* player, float force, int p2, bool playBumpEffect,
                GameObject* object);
}  // namespace phys
