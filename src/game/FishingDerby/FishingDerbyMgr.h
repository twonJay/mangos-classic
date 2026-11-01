/*
 * Stranglethorn Fishing Extravaganza derby.
 *
 * On a solo/botted server nobody competes for the Master Angler title, so the
 * weekly contest is a formality. This manager simulates the race: a field of
 * named rival anglers accumulates catches on a randomized pace while the
 * fishing window (game event 15) runs, zone chatter reports the standings, and
 * if a rival reaches 40 Speckled Tastyfish before the player turns in, Riggle
 * declares that rival the winner and the Master Angler quest locks for the
 * week (npc_riggle_bassbait reads IsWinnerDeclared()). Consolation turn-ins
 * (Apprentice Angler, the rare fish) are never locked. Rival fishing also has
 * a physical footprint: Tastyfish schools are drained at a fraction of the
 * rivals' modeled catch rate, and visible angler NPCs work the shoreline.
 */

#ifndef FISHING_DERBY_MGR_H
#define FISHING_DERBY_MGR_H

#include "Common.h"
#include "Entities/ObjectGuid.h"
#include "Policies/Singleton.h"

#include <ctime>
#include <string>
#include <vector>

class Creature;
class Map;
class Player;

class FishingDerbyMgr
{
    public:
        FishingDerbyMgr() = default;
        ~FishingDerbyMgr() = default;

        void Initialize();             // read config + load pool spawn points
        void Update(uint32 diff);      // called every World::Update tick

        bool IsEnabled() const { return m_enabled; }
        bool IsWinnerDeclared() const { return m_winnerDeclared; }
        void OnPlayerWin(Player* player);   // called by npc_riggle_bassbait on Master Angler reward

    private:
        struct Rival
        {
            std::string name;
            uint32 finishSecs = 0;     // seconds after race start this rival lands fish #40
            float curve = 1.f;         // pacing exponent: <1 starts hot and fades, >1 surges late
            uint32 plateau = 0;        // announced counts cap here (the hare stalls in the low 30s)
            uint32 reported = 0;       // last catch count announced (keeps chatter monotonic)
        };
        struct PoolSpot
        {
            uint32 dbGuid = 0;         // gameobject table guid of a School of Tastyfish
            float x = 0.f;
            float y = 0.f;
        };
        struct Angler
        {
            ObjectGuid guid;
            uint32 poolIdx = 0;
            time_t nextCatch = 0;      // when this angler pulls its next fish from its pool
        };

        Map* GetContinent() const;
        void StartRace(time_t now);
        void ResetAll();
        void TickRace(time_t now);
        void DeclareWinner(const std::string& name);
        void AnnounceToZones(const std::string& text) const;
        void RiggleYell(const std::string& text) const;
        void DrainPools(uint32 catches);
        void AnglersFish(time_t now);
        void EnforcePoolFloor();
        bool DrainPoolOnce(Map* map, uint32 dbGuid) const;
        void SpawnAnglers();
        void DespawnAnglers();
        void RelocateIdleAnglers();
        bool FindShorePoint(const PoolSpot& pool, float& sx, float& sy, float& sz, float& so) const;

        bool m_enabled = false;
        uint32 m_rivalCount = 6;
        uint32 m_paceMinSecs = 18 * 60;
        uint32 m_paceMaxSecs = 30 * 60;
        uint32 m_chatterSecs = 240;
        uint32 m_anglerCount = 10;
        uint32 m_anglerCatchMinSecs = 15;  // a visible angler pulls a fish from its school
        uint32 m_anglerCatchMaxSecs = 30;  // every 15-30 seconds
        uint32 m_minActivePools = 25;      // fished-out schools respawn at once below this floor
        float m_drainRate = 0.35f;     // fraction of the rivals' modeled catch rate applied to real pools

        bool m_racing = false;
        bool m_winnerDeclared = false;
        time_t m_raceStart = 0;
        time_t m_lastChatter = 0;
        time_t m_lastDrain = 0;
        float m_drainCarry = 0.f;

        uint32 m_winnerIdx = 0;        // rival with the earliest hidden finish
        bool m_surgeCalled = false;    // the winner's guaranteed pre-finish call-out went out
        std::string m_lastAnnouncedName;   // for "has taken the lead" flavor on a change

        uint32 m_riggleDbGuid = 0;
        std::vector<Rival> m_rivals;
        std::vector<PoolSpot> m_pools;
        std::vector<Angler> m_anglers;
        uint32 m_sinceTickMs = 0;
};

#define sFishingDerbyMgr MaNGOS::Singleton<FishingDerbyMgr>::Instance()

#endif
