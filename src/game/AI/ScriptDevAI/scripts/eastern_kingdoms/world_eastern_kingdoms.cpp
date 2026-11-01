/* This file is part of the ScriptDev2 Project. See AUTHORS file for Copyright information
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

#include "world_eastern_kingdoms.h"
#include "AI/ScriptDevAI/include/sc_common.h"
#include "AI/ScriptDevAI/scripts/world/world_map_scripts.h"
#include "World/WorldState.h"
#include "World/WorldStateDefines.h"
#include "GameEvents/GameEventMgr.h"
#include "AI/ScriptDevAI/scripts/world/scourge_invasion.h"
#include "ArenaBrawl/ArenaBrawlMgr.h"
#include "Entities/GossipDef.h"
#include "FishingDerby/FishingDerbyMgr.h"

/* *********************************************************
 *                  EASTERN KINGDOMS
 */
struct world_map_eastern_kingdoms : public ScriptedMap
{
    world_map_eastern_kingdoms(Map* pMap) : ScriptedMap(pMap)
    {
        Initialize();
    }

    void Initialize() override
    {
        instance->GetVariableManager().SetVariable(WORLD_STATE_CUSTOM_STV_GRP_01, urand(0, 1));
        instance->GetVariableManager().SetVariable(WORLD_STATE_CUSTOM_STV_GRP_02, urand(0, 1));
    }

    void OnCreatureGroupDespawn(CreatureGroup* creatureGroup, Creature* /*creature*/) override
    {
        switch (creatureGroup->GetGroupEntry().WorldStateCondition)
        {
            case 9900: case 9901: // TODO: Propagate respawn info so they do not instant respawn
                instance->GetVariableManager().SetVariable(WORLD_STATE_CUSTOM_STV_GRP_01, urand(0, 1));
                break;
            case 9902: case 9903:
                instance->GetVariableManager().SetVariable(WORLD_STATE_CUSTOM_STV_GRP_02, urand(0, 1));
                break;
        }

        // Quest: Stranglethorn Fever Wave Died
        switch (creatureGroup->GetGroupId())
        {
            case STV_FEVER_GROUP_01: 
                HandleSTVFeverEvent(AI_EVENT_CUSTOM_EVENTAI_A);
                break;
            case STV_FEVER_GROUP_02: 
                HandleSTVFeverEvent(AI_EVENT_CUSTOM_EVENTAI_B); 
                break;
            case STV_FEVER_GROUP_03: 
                HandleSTVFeverEvent(AI_EVENT_CUSTOM_EVENTAI_C); 
                break;             
        }
        
    }

    void OnCreatureCreate(Creature* pCreature) override
    {
        switch (pCreature->GetEntry())
        {
            case NPC_JONATHAN:
            case NPC_WRYNN:
            case NPC_BOLVAR:
            case NPC_PRESTOR:
            case NPC_WINDSOR:
            case NPC_FALSTAD_WILDHAMMER:
            case NPC_SHORT_JOHN_MITHRIL:
                m_npcEntryGuidStore[pCreature->GetEntry()] = pCreature->GetObjectGuid();
                break;
            case NPC_NECROPOLIS_HEALTH:
                m_npcEntryGuidCollection[pCreature->GetEntry()].push_back(pCreature->GetObjectGuid());
                break;
        }
    }

    void OnCreatureDeath(Creature* pCreature) override
    {
        switch (pCreature->GetEntry())
        {
            case NPC_NECROPOLIS_HEALTH:
                m_npcEntryGuidCollection.erase(pCreature->GetObjectGuid());
                break;
            default:
                break;
        }
    }

    void OnObjectCreate(GameObject* pGo) override
    {
        switch (pGo->GetEntry())
        {
            case GO_SUMMON_CIRCLE:
                m_goEntryGuidCollection[pGo->GetEntry()].push_back(pGo->GetObjectGuid());
                break;
            case GO_ARENA_TREASURE_CHEST:
                sArenaBrawlMgr.OnChestSpawned(pGo);
                break;
        }
    }

    void OnEventHappened(uint16 event_id, bool activate, bool resume) override
    {
        if (event_id == GAME_EVENT_GURUBASHI_ARENA && activate)
        {
            if (Creature* creature = GetSingleCreatureFromStorage(NPC_SHORT_JOHN_MITHRIL))
                creature->GetMotionMaster()->MoveWaypoint();
        }
        else if (event_id == GAME_EVENT_GURUBASHI_ARENA && !activate)
        {
            sArenaBrawlMgr.OnArenaEventEnd();
        }
    }

    void SetData(uint32 /*uiType*/, uint32 /*uiData*/) override {}

    // Quest: Stranglethorn Fever Wave Died
    // Inform Quest Handler to spawn next group if still alive
    void HandleSTVFeverEvent(AIEventType eventType)
    {
        std::vector<Creature*> const* witchDoctor = instance->GetCreatures("STV_WITCH_DOCTOR_UNBAGWA");
        if (witchDoctor)
        {
            for (Creature* creature : *witchDoctor)
                if (creature->IsAlive())
                    creature->AI()->SendAIEvent(eventType, creature, creature);
        }
    }
};

/*######
## npc_riggle_bassbait - Stranglethorn Fishing Extravaganza host
##
## The stock data locks his ENTIRE quest menu after the first Master Angler
## turn-in (dbscript stripped the questgiver flag) and shows the "we already
## have a winner" gossip on a pure timer. Instead the FishingDerby manager
## tracks the actual race: once a winner (player or simulated rival) is
## declared, only the Master Angler quest is withheld - Apprentice Angler and
## the rare fish consolation turn-ins stay open for the whole turn-in window.
######*/

enum
{
    QUEST_MASTER_ANGLER          = 8193,
    TEXT_ID_RIGGLE_CONTEST       = 7614,        // welcome / contest briefing
    TEXT_ID_RIGGLE_WINNER_KNOWN  = 7714,        // "We already have a winner..."
};

bool GossipHello_npc_riggle_bassbait(Player* pPlayer, Creature* pCreature)
{
    pPlayer->GetPlayerMenu()->ClearMenus();
    if (pCreature->isQuestGiver())
        pPlayer->PrepareQuestMenu(pCreature->GetObjectGuid());

    if (sFishingDerbyMgr.IsWinnerDeclared())
    {
        // Race decided for this week: withhold only the Master Angler quest.
        QuestMenu& questMenu = pPlayer->GetPlayerMenu()->GetQuestMenu();
        std::vector<QuestMenuItem> keep;
        for (uint16 i = 0; i < questMenu.MenuItemCount(); ++i)
        {
            QuestMenuItem const& item = questMenu.GetItem(i);
            if (item.m_qId != QUEST_MASTER_ANGLER)
                keep.push_back(item);
        }
        questMenu.ClearMenu();
        for (QuestMenuItem const& item : keep)
            questMenu.AddMenuItem(item.m_qId, item.m_qIcon);

        pPlayer->GetPlayerMenu()->SendGossipMenu(TEXT_ID_RIGGLE_WINNER_KNOWN, pCreature->GetObjectGuid());
    }
    else
        pPlayer->GetPlayerMenu()->SendGossipMenu(TEXT_ID_RIGGLE_CONTEST, pCreature->GetObjectGuid());

    return true;
}

bool QuestRewarded_npc_riggle_bassbait(Player* pPlayer, Creature* /*pCreature*/, Quest const* pQuest)
{
    if (pQuest->GetQuestId() == QUEST_MASTER_ANGLER)
        sFishingDerbyMgr.OnPlayerWin(pPlayer);
    return false;
}


/*######
## npc_jang - Fishmaster Apprentice
##
## As Riggle's winner gossip promises ("...but Jang is willing to purchase any
## Tastyfish that you have fished up!"), Jang's repeatable Tastyfish-for-silver
## turn-in (Apprentice Angler) opens only once a Master Angler is declared.
## The menu item is added explicitly so the auto-complete quest is always
## visible to hand in - completion still validates the 5 Speckled Tastyfish.
######*/

enum
{
    QUEST_APPRENTICE_ANGLER = 8194,
    TEXT_ID_JANG            = 7696,
};

bool GossipHello_npc_jang(Player* pPlayer, Creature* pCreature)
{
    pPlayer->GetPlayerMenu()->ClearMenus();

    if (sFishingDerbyMgr.IsWinnerDeclared() || !sFishingDerbyMgr.IsEnabled())
    {
        pPlayer->PrepareQuestMenu(pCreature->GetObjectGuid());

        QuestMenu& questMenu = pPlayer->GetPlayerMenu()->GetQuestMenu();
        if (!questMenu.HasItem(QUEST_APPRENTICE_ANGLER))
            questMenu.AddMenuItem(QUEST_APPRENTICE_ANGLER, DIALOG_STATUS_REWARD_REP);
    }

    pPlayer->GetPlayerMenu()->SendGossipMenu(TEXT_ID_JANG, pCreature->GetObjectGuid());
    return true;
}


InstanceData* GetInstanceData_world_map_eastern_kingdoms(Map* pMap)
{
    return new world_map_eastern_kingdoms(pMap);
}

void AddSC_world_eastern_kingdoms()
{
    Script* pNewScript = new Script;
    pNewScript->Name = "world_map_eastern_kingdoms";
    pNewScript->GetInstanceData = &GetInstanceData_world_map_eastern_kingdoms;
    pNewScript->RegisterSelf();

    pNewScript = new Script;
    pNewScript->Name = "npc_riggle_bassbait";
    pNewScript->pGossipHello = &GossipHello_npc_riggle_bassbait;
    pNewScript->pQuestRewardedNPC = &QuestRewarded_npc_riggle_bassbait;
    pNewScript->RegisterSelf();

    pNewScript = new Script;
    pNewScript->Name = "npc_jang";
    pNewScript->pGossipHello = &GossipHello_npc_jang;
    pNewScript->RegisterSelf();
}
