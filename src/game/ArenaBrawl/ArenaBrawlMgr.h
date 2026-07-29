/*
 * Gurubashi Arena Brawl manager.
 *
 * When the Gurubashi Arena Booty Run chest is up and a player approaches it,
 * this manager borrows a handful of level-capped random bots, teleports them
 * into the arena (an AREA_FLAG_ARENA free-for-all zone, so everyone there is
 * mutually attackable regardless of faction) and turns them loose with the
 * "pvp" strategy, so they brawl each other and any players present. The chest
 * therefore has to be fought for instead of freely grabbed.
 *
 * Anti-abuse: the brawl fires once per chest instance (no walk-out/in farming),
 * and if no real player remains in the arena for a short grace period the chest
 * is despawned (no triggering it then waiting for the bots to kill themselves).
 *
 * The manager is driven from World::Update and keys off two hooks in the
 * eastern-kingdoms continent script (OnObjectCreate for the chest, and the
 * Gurubashi game-event end).
 */

#ifndef ARENA_BRAWL_MGR_H
#define ARENA_BRAWL_MGR_H

#include "Common.h"
#include "Policies/Singleton.h"
#include "Entities/ObjectGuid.h"

#include <ctime>
#include <string>
#include <vector>

class GameObject;
class Player;
class Map;

struct BrawlBot
{
    ObjectGuid guid;                   // bot player guid
    uint32 originMap = 0;              // where it was before we borrowed it
    float originX = 0.f;
    float originY = 0.f;
    float originZ = 0.f;
    float originO = 0.f;
};

struct BarrierDef                      // one runtime barrier piece (ramp seal / anti-jump)
{
    uint32 entry = 0;                  // gameobject_template entry (force field / collision wall)
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
    float o = 0.f;
    float scale = 1.f;                 // model scale; <1 shrinks (optional 6th config field)
    float pitch = 0.f;                 // rotation about Y (radians); PI flips upside down (7th)
    float roll  = 0.f;                 // rotation about X (radians); PI flips upside down (8th)
};

class ArenaBrawlMgr
{
    public:
        ArenaBrawlMgr() = default;
        ~ArenaBrawlMgr() = default;

        void Initialize();                 // read config
        void Update(uint32 diff);          // called every World::Update tick

        // hooks from world_eastern_kingdoms.cpp
        void OnChestSpawned(GameObject* chest);
        void OnArenaEventEnd();

        bool IsEnabled() const { return m_enabled; }

    private:
        enum State : uint8
        {
            STATE_IDLE   = 0,              // no chest tracked
            STATE_ARMED  = 1,              // chest up, waiting for a player to approach
            STATE_ACTIVE = 2,              // brawl underway, players present
            STATE_GRACE  = 3,             // no players present, counting down to despawn
        };

        void Deploy();                     // recruit + teleport + engage bots
        void Cleanup(bool despawnChest);   // release bots, optionally remove the chest
        void ReserveBot(ObjectGuid guid, uint32 seconds);
        void ReleaseBot(const BrawlBot& b);
        void ParseBarrierConfig(const std::string& cfg);
        void SpawnBarriers();              // seal the ramps for the duration of the brawl
        void DespawnBarriers();

        Map* GetArenaMap() const;
        GameObject* GetChest() const;
        int  CountRealPlayersNear(float radius) const;
        bool AnyRealPlayerWithin(float radius) const;

        bool   m_enabled = false;

        // config
        uint32 m_botCount       = 10;
        uint32 m_botLevel       = 60;
        float  m_triggerRadius  = 20.f;    // player-near-chest distance that starts the brawl
        float  m_arenaRadius    = 80.f;    // "still in the arena" distance for presence checks
        float  m_scatterRadius  = 18.f;    // bot spawn ring around the chest
        uint32 m_graceSecs      = 5;       // no-players grace before chest despawns
        uint32 m_maxEventSecs   = 900;     // hard timeout for a single brawl
        bool   m_despawnOnAbandon = true;
        bool   m_factionMix     = true;

        // runtime
        uint8      m_state = STATE_IDLE;
        uint32     m_mapId = 0;
        ObjectGuid m_chestGuid;
        float      m_chestX = 0.f;
        float      m_chestY = 0.f;
        float      m_chestZ = 0.f;
        time_t     m_eventStart = 0;       // when the brawl went ACTIVE (timeout base)
        time_t     m_graceUntil = 0;       // when the grace period expires
        std::vector<BrawlBot> m_bots;

        std::vector<BarrierDef>  m_barrierDefs;    // configured ramp barriers
        std::vector<ObjectGuid>  m_barrierGuids;   // currently-spawned barrier GOs

        uint32 m_sinceCheckMs = 0;
};

#define sArenaBrawlMgr MaNGOS::Singleton<ArenaBrawlMgr>::Instance()

#endif
