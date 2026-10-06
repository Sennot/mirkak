// Adapted from Silicate commit f183dbc5 (GPL-3.0).
// See LICENSE and THIRD_PARTY_NOTICES.md.
#pragma once

#include <Geode/Geode.hpp>

#include "object.hpp"
#include "trajectory/trajectory.hpp"

namespace phys {

int checkPlayerCollisions(GJBaseGameLayer* gameLayer, PlayerObject* player);
int collidedWithObjectInternal(PlayerObject* player, float dt,
                               GameObject* object, cocos2d::CCRect* p4,
                               bool p5);
void preSlopeCollision(PlayerObject* player, float dt, GameObject* slope);
void collidedWithSlopeInternal(PlayerObject* player);
void activateForTrajectory(EffectGameObject* obj, PlayerObject* player);

inline bool activatingPortal(GJBaseGameLayer* pl, PlayerObject* player,
                             EffectGameObject* portal) {
    if (pl->canBeActivatedByPlayer(player, portal)) {
        if (cleanfeed::Trajectory::get().playerHasActivated(player, portal)) {
            return false;
        }

        pl->playerWillSwitchMode(player, portal);

        bool isTheSame =
            ((portal->m_objectType == GameObjectType::ShipPortal) &&
             player->m_isShip) ||
            ((portal->m_objectType == GameObjectType::BallPortal) &&
             player->m_isBall) ||
            ((portal->m_objectType == GameObjectType::UfoPortal) &&
             player->m_isBird) ||
            ((portal->m_objectType == GameObjectType::WavePortal) &&
             player->m_isDart) ||
            ((portal->m_objectType == GameObjectType::SpiderPortal) &&
             player->m_isSpider) ||
            ((portal->m_objectType == GameObjectType::SwingPortal) &&
             player->m_isSwing) ||
            ((portal->m_objectType == GameObjectType::RobotPortal) &&
             player->m_isRobot) ||
            ((portal->m_objectType == GameObjectType::CubePortal) &&
             (!(player->m_isShip || player->m_isBall || player->m_isBird ||
                player->m_isDart || player->m_isSpider || player->m_isSwing)));

        cocos2d::CCPoint position = player->getPosition();
        // if (portal->m_objectType == GameObjectType::CubePortal) {
        player->switchedToMode(portal->m_objectType);
        // }

        switch (portal->m_objectType) {
            case GameObjectType::ShipPortal:
                player->toggleFlyMode(true, true);
                break;
            case GameObjectType::BallPortal:
                player->toggleRollMode(true, true);
                break;
            case GameObjectType::UfoPortal:
                player->toggleBirdMode(true, true);
                break;
            case GameObjectType::WavePortal:
                player->toggleDartMode(true, true);
                break;
            case GameObjectType::SpiderPortal:
                player->toggleSpiderMode(true, true);
                break;
            case GameObjectType::SwingPortal:
                player->toggleSwingMode(true, true);
                break;
            case GameObjectType::RobotPortal:
                player->toggleRobotMode(true, true);
                break;
            default:
                break;
        }

        player->setPosition(position);
        player->m_iconSprite->setPosition(position);
        player->m_vehicleSprite->setPosition(position);

        player->m_lastPortalPos = portal->getPosition();
        player->m_lastActivatedPortal = portal;
        activateForTrajectory(portal, player);

        return !isTheSame;
    }

    return false;
}

void bumpPlayerFromGJBGL(GJBaseGameLayer* pl, PlayerObject* player,
                         EffectGameObject* object);

// int handleRotatedCollisionInternal(
//     PlayerObject* player,
//     float dt,
//     GameObject* object,
//     cocos2d::CCRect* rect,
//     bool p3,
//     bool p4,
//     bool isSlope
// ) {
//     cocos2d::CCPoint p1 = player->getPosition();
//     player->rotateGameplayObject(object);
//     for (auto& obj : player->m_maybeRotatedObjectsMap) {
//         player->rotateGameplayObject(obj.second);
//     }

//     int ret = 0;
//     if (isSlope) {
//         player->collidedWithSlopeInternal(dt, object, p4);
//     } else {
//         ret = player->collidedWithObjectInternal(dt, object, *rect, p3);
//     }

//     cocos2d::CCPoint newPos = {
//         player->getPosition().y - p1.y + p1.x,
//         p1.y - player->getPosition().x + p1.x,
//     };
//     player->setPosition(newPos);
//     player->unrotateGameplayObject(object);
//     for (auto& obj : player->m_maybeRotatedObjectsMap) {
//         player->unrotateGameplayObject(obj.second);
//     }

//     return ret;
// }

void collisionCheckObjects(GJBaseGameLayer* pl, PlayerObject* player,
                           gd::vector<GameObject*>* objects, int objectCount,
                           float dt);

void triggerObject(EffectGameObject* obj, GJBaseGameLayer* pl,
                   PlayerObject* player);
void checkSpawnObjects(GJBaseGameLayer* pl, PlayerObject* player);
}  // namespace phys
