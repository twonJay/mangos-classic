/*
 * This file is part of the CMaNGOS Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "HomeMovementGenerator.h"
#include "Maps/Map.h"
#include "Entities/Creature.h"
#include "AI/BaseAI/CreatureAI.h"
#include "Movement/MoveSplineInit.h"
#include "Movement/MoveSpline.h"

void HomeMovementGenerator<Creature>::Initialize(Creature& owner)
{
    wasActive = owner.isActiveObject();
    if (!wasActive)
        owner.SetActiveObjectState(true);
    owner.GetCombatManager().SetEvadeState(EVADE_HOME);

    // Resolve the return point once, here: MotionMaster::Mutate() runs Initialize()
    // BEFORE pushing this generator, so top() is still the generator being returned
    // to (waypoint path, formation, follow) and its reset position is the real home.
    // Once this generator is on top the same lookup fails and falls back to the
    // combat start or spawn point, which is not where an escort or patrol resumes.
    if (owner.GetMotionMaster()->empty() || !owner.GetMotionMaster()->top()->GetResetPosition(owner, m_home.x, m_home.y, m_home.z, m_home.o))
        owner.GetCombatStartPosition(m_home);

    if (m_home.IsEmpty())
        owner.GetRespawnCoord(m_home.x, m_home.y, m_home.z, &m_home.o);

    _setTargetLocation(owner);
}

void HomeMovementGenerator<Creature>::Reset(Creature&)
{
}

void HomeMovementGenerator<Creature>::_setTargetLocation(Creature& owner)
{
    if (owner.hasUnitState(UNIT_STAT_NOT_MOVE))
        return;

    Position const& pos = m_home;
    // No distance cap: creatures kited any distance run all the way home like on
    // classic (the old >150y despawn+respawn looked like a vanish to players).
    // Returns longer than one spline's point budget are handled by Update(),
    // which keeps launching further legs until the creature stands at home.

    PathFinder path(&owner);

    Position curPos = owner.GetPosition(owner.GetTransport());
    m_legStart = curPos;
    // source and target pos must be local coords
    path.calculate(G3D::Vector3(curPos.GetPositionX(), curPos.GetPositionY(), curPos.GetPositionZ()), G3D::Vector3(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ()), true);

    Movement::MoveSplineInit init(owner);
    init.MovebyPath(path.getPath());
    init.SetWalk(!runHome);
    init.SetFacing(pos.GetPositionO());
    if (path.getPathType() & (PATHFIND_NOPATH | PATHFIND_SHORTCUT))
        init.SetVelocity(400.f);
    init.Launch();

    arrived = false;
    owner.clearUnitState(static_cast<uint32>(UNIT_STAT_ALL_DYN_STATES));
}

bool HomeMovementGenerator<Creature>::Update(Creature& owner, const uint32& /*time_diff*/)
{
    if (owner.movespline->Finalized())
    {
        // A long return home exceeds a single spline's point budget and ends
        // mid-route: if the creature is not standing at its return point yet,
        // launch the next leg instead of declaring arrival.
        // Only while the last leg actually moved the creature: a leg that ended where
        // it started means the point cannot be reached from here, and relaunching
        // would freeze the creature in EVADE_HOME forever (immune, never resuming).
        if (!owner.hasUnitState(UNIT_STAT_NOT_MOVE))
        {
            Position const cur = owner.GetPosition(owner.GetTransport());
            bool const progressed = cur.GetDistance(m_legStart) > 1.f;                      // squared: > 1y
            bool const farFromHome = owner.GetDistance(m_home.x, m_home.y, m_home.z, DIST_CALC_NONE, owner.GetTransport()) > 5.f * 5.f; // squared: > 5y
            if (progressed && farFromHome)
            {
                _setTargetLocation(owner);
                return true;
            }
        }
        arrived = true;
    }
    return !arrived;
}

void HomeMovementGenerator<Creature>::Finalize(Creature& owner)
{
    owner.GetCombatManager().SetEvadeState(EVADE_NONE);
    if (arrived)
    {
        if (owner.GetTemporaryFactionFlags() & TEMPFACTION_RESTORE_REACH_HOME)
            owner.ClearTemporaryFaction();

        owner.LoadCreatureAddon(true);
        owner.TriggerHomeEvents();
        if (owner.IsTemporarySummon())
        {
            if (owner.GetSpawnerGuid().IsCreatureOrPet())
                if (Creature* pSummoner = owner.GetMap()->GetAnyTypeCreature(owner.GetSpawnerGuid()))
                    if (pSummoner->AI())
                        pSummoner->AI()->SummonedJustReachedHome(&owner);
        }

        if (!wasActive && owner.IsInWorld())
            owner.SetActiveObjectState(false);

        owner.SetCombatStartPosition(Position());
    }
}
