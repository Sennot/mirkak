// Physics/prediction behavior adapted from Silicate commit f183dbc5
// (GPL-3.0). See LICENSE and THIRD_PARTY_NOTICES.md.
#pragma once

#include <Geode/Geode.hpp>

#include "checkpoint/checkpoint.hpp"
#include "overlay_draw_node.hpp"

#include <unordered_set>

namespace cleanfeed {
    class Trajectory final {
    public:
        enum Mode {
            Hold = 0x1,
            Swift = 0x2,
            Release = 0x4,
            Left = 0x8,
            Right = 0x10,
            Player1 = 0x20,
            Player2 = 0x40,
        };

        static Trajectory& get();

        void init(GJBaseGameLayer* layer);
        void shutdown(GJBaseGameLayer* layer);
        void update(GJBaseGameLayer* layer);
        void handleButton(bool player1, bool holding);
        void handlePortal(PlayerObject* player, GameObject* object);
        void rememberActivatedObject(EnhancedGameObject* object, PlayerObject* player);

        bool isFakePlayer(PlayerObject* player) const;
        bool drawing() const;
        bool hasDied(PlayerObject* player);
        bool playerHasActivated(PlayerObject* player, EnhancedGameObject* object) const;
        bool realPlayerHasActivated(PlayerObject* player, EnhancedGameObject* object) const;
        PlayerObject* getOtherPlayer(PlayerObject* player) const;

    private:
        struct Signature {
            float p1[7]{};
            float p2[7]{};
            uint64_t p1Flags = 0;
            uint64_t p2Flags = 0;
            float timeWarp = 0.f;
            float cameraZoom = 0.f;
            float lineWidth = 0.f;
            float length = 0.f;
            int tps = 0;
            uint32_t colors[2]{};
            uint32_t boolPack = 0;

            bool operator==(Signature const&) const = default;
        };

        // Consecutive simulation steps that continue in the same direction are
        // drawn as one segment. The covered area is unchanged, but the draw
        // node holds far fewer vertices and translucent colors no longer
        // darken where the rounded caps of neighbouring steps overlap.
        struct Track {
            cocos2d::CCPoint start;
            cocos2d::CCPoint end;
            cocos2d::CCPoint direction;
            cocos2d::ccColor4F color{};
            int stepCount = 0;
            bool active = false;
            bool hasDirection = false;
        };

        struct PredictionSettings {
            int tps = 240;
            float length = 1.f;
            float lineWidth = 0.65f;
            bool highPerformance = true;
            cocos2d::ccColor4F holdColor{0.f, 1.f, 0.1f, 1.f};
            cocos2d::ccColor4F releaseColor{1.f, 0.f, 0.1f, 1.f};
        };

        PlayerObject* createFakePlayer(GJBaseGameLayer* layer, std::string const& id);
        Signature computeSignature(
            GJBaseGameLayer* layer,
            PredictionSettings const& predictionSettings
        ) const;
        void simulate(
            GJBaseGameLayer* layer,
            bool player1,
            int mode,
            bool clickBothPlayers,
            GJGameState const& frameGameState,
            EffectManagerState& frameEffectState,
            SavedPlayerCheckpoint const& playerCheckpoint,
            SavedPlayerCheckpoint const* otherCheckpoint
        );
        void runPrediction(
            GJBaseGameLayer* layer,
            PlayerObject* player,
            PlayerObject* other,
            int mode,
            bool both
        );
        bool iterate(
            GJBaseGameLayer* layer,
            PlayerObject* player,
            int mode,
            Track& track
        );
        void extendTrack(Track& track, cocos2d::CCPoint const& from, cocos2d::CCPoint const& to);
        void flushTrack(Track& track);
        void drawHitbox(PlayerObject* player);
        void clearActivatedObjects();

        GJBaseGameLayer* m_layer = nullptr;
        geode::WeakRef<GJBaseGameLayer> m_layerLifetime;
        OverlayDrawNode* m_node = nullptr;
        geode::WeakRef<OverlayDrawNode> m_nodeLifetime;
        PlayerObject* m_fakePlayer1 = nullptr;
        PlayerObject* m_fakePlayer2 = nullptr;
        std::unordered_set<uintptr_t> m_activatedObjectsP1;
        std::unordered_set<uintptr_t> m_activatedObjectsP2;
        EffectManagerState m_reusableEffectState;
        SavedPlayerCheckpoint m_reusablePlayer1Checkpoint;
        SavedPlayerCheckpoint m_reusablePlayer2Checkpoint;
        PredictionSettings m_predictionSettings;
        Signature m_lastSignature;
        bool m_calculated = false;
        bool m_drawing = false;
        bool m_deadP1 = false;
        bool m_deadP2 = false;
        bool m_p1Holding = false;
        bool m_p2Holding = false;
        float m_physicsDt = 1.f / 240.f;
        float m_playerDelta = 0.25f;
        float m_trajectoryWidth = 0.65f;
        float m_cachedTimeWarp = -1.f;
        int m_cachedProgressIncrement = 0;
        float m_cachedCameraZoom = -1.f;
        float m_cachedLineRadius = 0.65f;
    };
}
