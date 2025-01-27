/*
* Copyright (C) 2012 CVMagic <http://www.trinitycore.org/f/topic/6551-vas-autobalance/>
* Copyright (C) 2008-2010 TrinityCore <http://www.trinitycore.org/>
* Copyright (C) 2006-2009 ScriptDev2 <https://scriptdev2.svn.sourceforge.net/>
* Copyright (C) 1985-2010 {VAS} KalCorp  <http://vasserver.dyndns.org/>
*
* This program is free software; you can redistribute it and/or modify it
* under the terms of the GNU General Public License as published by the
* Free Software Foundation; either version 2 of the License, or (at your
* option) any later version.
*
* This program is distributed in the hope that it will be useful, but WITHOUT
* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
* FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
* more details.
*
* You should have received a copy of the GNU General Public License along
* with this program. If not, see <http://www.gnu.org/licenses/>.
*/

/*
* Script Name: AutoBalance
* Original Authors: KalCorp and Vaughner
* Maintainer(s): CVMagic
* Original Script Name: VAS.AutoBalance
* Description: This script is intended to scale based on number of players, instance mobs & world bosses' health, mana, and damage.
*/
/*
Touched up by SPP MDic for Trinitycore
*/

#include "Configuration/Config.h"
#include "Unit.h"
#include "Chat.h"
#include "Creature.h"
#include "Player.h"
#include "ObjectMgr.h"
#include "MapManager.h"
#include "World.h"
#include "Map.h"
#include "ScriptMgr.h"
#include "Language.h"
#include <vector>
#include "Log.h"
#include "Group.h"
#include "DataMap.h"
#include "DB2Stores.h"
//#include "DB2Structure.h"
#include "LFGMgr.h"
//#include "LFGMgr.cpp"


class AutoBalanceCreatureInfo : public DataMap::Base {
public:
    AutoBalanceCreatureInfo() {}

    AutoBalanceCreatureInfo(uint32 count, float dmg, float hpRate, float manaRate, float armorRate, uint8 selLevel) :
        instancePlayerCount(count), selectedLevel(selLevel), DamageMultiplier(dmg), HealthMultiplier(hpRate), ManaMultiplier(manaRate),
        ArmorMultiplier(armorRate) {}

    uint32 instancePlayerCount = 0;
    uint8 selectedLevel = 0;
    // this is used to detect creatures that update their entry
    uint32 entry = 0;
    float DamageMultiplier = 1;
    float HealthMultiplier = 1;
    float ManaMultiplier = 1;
    float ArmorMultiplier = 1;

};

class AutoBalanceMapInfo : public DataMap::Base {
public:
    AutoBalanceMapInfo() {}

    AutoBalanceMapInfo(uint32 count, uint8 selLevel) : playerCount(count), mapLevel(selLevel) {}

    uint32 playerCount = 0;
    uint8 mapLevel = 0;

};

// The map values correspond with the .AutoBalance.XX.Name entries in the configuration file.
static std::map<int, int> forcedCreatureIds;
static int8 PlayerCountDifficultyOffset, LevelScaling, higherOffset, lowerOffset;
static bool enabled, LevelEndGameBoost, DungeonsOnly, PlayerChangeNotify, LevelUseDb, DungeonScaleDownXP;
static float globalRate, healthMultiplier, manaMultiplier, armorMultiplier, damageMultiplier, MinHPModifier, MinManaModifier, MinDamageModifier,
InflectionPoint, InflectionPointRaid, InflectionPointRaid10M, InflectionPointRaid25M, InflectionPointRaid30M, InflectionPointHeroic, InflectionPointRaidHeroic,
InflectionPointRaid10MHeroic, InflectionPointRaid25MHeroic, InflectionPointRaid30MHeroic, BossInflectionMult;

int GetValidDebugLevel() {
    int debugLevel = sConfigMgr->GetIntDefault("AutoBalance.DebugLevel", 2);
    if ((debugLevel < 0) || (debugLevel > 3))
    {
        return 1;

    }
    return debugLevel;

}

// Used for reading the string from the configuration file to for those creatures who need to be scaled for XX number of players.
void LoadForcedCreatureIdsFromString(std::string creatureIds, int forcedPlayerCount) {
    std::string delimitedValue;
    std::stringstream creatureIdsStream;

    creatureIdsStream.str(creatureIds);
    // Process each Creature ID in the string, delimited by the comma - ","
    while (std::getline(creatureIdsStream, delimitedValue, ','))
    {
        int creatureId = atoi(delimitedValue.c_str());
        if (creatureId >= 0)
        {
            forcedCreatureIds[creatureId] = forcedPlayerCount;

        }

    }
}

int GetForcedNumPlayers(int creatureId) {
    // Don't want the forcedCreatureIds map to blowup to a massive empty array
    if (forcedCreatureIds.find(creatureId) == forcedCreatureIds.end())
    {
        return -1;

    }
    return forcedCreatureIds[creatureId];
}

void getAreaLevel(Map* map, uint8 areaid, uint8& min, uint8& max, Player* player)
{
    lfg::LFGDungeonData const* dungeon = sLFGMgr->GetLFGDungeon(map->GetId(), map->GetDifficultyID(), player->GetTeam());

    if (dungeon && (map->IsDungeon() || map->IsRaid()))
    {
        min = dungeon->minlevel;
        max = dungeon->targetlevel ? dungeon->targetlevel : dungeon->maxlevel;

    }

    if (!min && !max)
    {
        AreaTableEntry const* areaEntry = sAreaTableStore.LookupEntry(areaid);
        if (areaEntry && areaEntry->ExplorationLevel > 0)
        {
            min = areaEntry->ExplorationLevel;
            max = areaEntry->ExplorationLevel;

        }

    }
}

class AutoBalance_WorldScript : public WorldScript
{
public:
    AutoBalance_WorldScript() : WorldScript("AutoBalance_WorldScript") {}

    void OnConfigLoad(bool /*reload*/) override
    {
        SetInitialWorldSettings();
    }

    void OnStartup() override { }

    void SetInitialWorldSettings() override
    {
        forcedCreatureIds.clear();
        LoadForcedCreatureIdsFromString(sConfigMgr->GetStringDefault("AutoBalance.ForcedID40", ""), 40);
        LoadForcedCreatureIdsFromString(sConfigMgr->GetStringDefault("AutoBalance.ForcedID25", ""), 25);
        LoadForcedCreatureIdsFromString(sConfigMgr->GetStringDefault("AutoBalance.ForcedID10", ""), 10);
        LoadForcedCreatureIdsFromString(sConfigMgr->GetStringDefault("AutoBalance.ForcedID5", ""), 5);
        LoadForcedCreatureIdsFromString(sConfigMgr->GetStringDefault("AutoBalance.ForcedID2", ""), 2);
        LoadForcedCreatureIdsFromString(sConfigMgr->GetStringDefault("AutoBalance.DisabledID", ""), 0);

        enabled = sConfigMgr->GetBoolDefault("AutoBalance.enable", 1);
        LevelEndGameBoost = sConfigMgr->GetBoolDefault("AutoBalance.LevelEndGameBoost", 1);
        DungeonsOnly = sConfigMgr->GetBoolDefault("AutoBalance.DungeonsOnly", 1);
        PlayerChangeNotify = sConfigMgr->GetBoolDefault("AutoBalance.PlayerChangeNotify", 1);
        LevelUseDb = sConfigMgr->GetBoolDefault("AutoBalance.levelUseDbValuesWhenExists", 1);
        DungeonScaleDownXP = sConfigMgr->GetBoolDefault("AutoBalance.DungeonScaleDownXP", 0);

        LevelScaling = sConfigMgr->GetIntDefault("AutoBalance.levelScaling", 1);
        PlayerCountDifficultyOffset = sConfigMgr->GetIntDefault("AutoBalance.playerCountDifficultyOffset", 0);
        higherOffset = sConfigMgr->GetIntDefault("AutoBalance.levelHigherOffset", 3);
        lowerOffset = sConfigMgr->GetIntDefault("AutoBalance.levelLowerOffset", 0);

        InflectionPoint = sConfigMgr->GetFloatDefault("AutoBalance.InflectionPoint", 0.5f);
        InflectionPointRaid = sConfigMgr->GetFloatDefault("AutoBalance.InflectionPointRaid", InflectionPoint);
        InflectionPointRaid30M = sConfigMgr->GetFloatDefault("AutoBalance.InflectionPointRaid30M", InflectionPointRaid);
        InflectionPointRaid25M = sConfigMgr->GetFloatDefault("AutoBalance.InflectionPointRaid25M", InflectionPointRaid);
        InflectionPointRaid10M = sConfigMgr->GetFloatDefault("AutoBalance.InflectionPointRaid10M", InflectionPointRaid);
        InflectionPointHeroic = sConfigMgr->GetFloatDefault("AutoBalance.InflectionPointHeroic", InflectionPoint);
        InflectionPointRaidHeroic = sConfigMgr->GetFloatDefault("AutoBalance.InflectionPointRaidHeroic", InflectionPointRaid);
        InflectionPointRaid30MHeroic = sConfigMgr->GetFloatDefault("AutoBalance.InflectionPointRaid30MHeroic", InflectionPointRaid30M);
        InflectionPointRaid25MHeroic = sConfigMgr->GetFloatDefault("AutoBalance.InflectionPointRaid25MHeroic", InflectionPointRaid25M);
        InflectionPointRaid10MHeroic = sConfigMgr->GetFloatDefault("AutoBalance.InflectionPointRaid10MHeroic", InflectionPointRaid10M);
        BossInflectionMult = sConfigMgr->GetFloatDefault("AutoBalance.BossInflectionMult", 1.0f);
        globalRate = sConfigMgr->GetFloatDefault("AutoBalance.rate.global", 3.0f);
        healthMultiplier = sConfigMgr->GetFloatDefault("AutoBalance.rate.health", 1.0f);
        manaMultiplier = sConfigMgr->GetFloatDefault("AutoBalance.rate.mana", 1.0f);
        armorMultiplier = sConfigMgr->GetFloatDefault("AutoBalance.rate.armor", 1.0f);
        damageMultiplier = sConfigMgr->GetFloatDefault("AutoBalance.rate.damage", 1.0f);
        MinHPModifier = sConfigMgr->GetFloatDefault("AutoBalance.MinHPModifier", 0.1f);
        MinManaModifier = sConfigMgr->GetFloatDefault("AutoBalance.MinManaModifier", 0.1f);
        MinDamageModifier = sConfigMgr->GetFloatDefault("AutoBalance.MinDamageModifier", 0.1f);
    }
};

class AutoBalance_PlayerScript : public PlayerScript
{
public:
    AutoBalance_PlayerScript() : PlayerScript("AutoBalance_PlayerScript") { }

    void OnLogin(Player* Player, bool /*firstLogin*/) override
    {
        if (sConfigMgr->GetBoolDefault("AutoBalanceAnnounce.enable", true))
        {
            ChatHandler(Player->GetSession()).SendSysMessage("This server is running the |cff4CFF00AutoBalance |rmodule.");
        }

    }

    virtual void OnLevelChanged(Player* player, uint8 /*oldlevel*/) override
    {
        if (!enabled || !player)
        {
            return;
        }

        if (LevelScaling == 0)
        {
            return;

        }

        AutoBalanceMapInfo* mapABInfo = player->GetMap()->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");
        if (mapABInfo->mapLevel < player->getLevel())
        {
            mapABInfo->mapLevel = player->getLevel();
        }

    }

    void OnGiveXP(Player* player, uint32& amount, Unit* victim) override
    {
        if (victim && DungeonScaleDownXP)
        {
            Map* map = player->GetMap();

            if (map->IsDungeon())
            {
                // Ensure that the players always get the same XP, even when entering the dungeon alone
                uint32 maxPlayerCount = ((InstanceMap*)sMapMgr->FindMap(map->GetId(), map->GetInstanceId()))->GetMaxPlayers();
                uint32 currentPlayerCount = map->GetPlayersCountExceptGMs();
                amount *= (float)currentPlayerCount / maxPlayerCount;
            }
        }
    }
};

class AutoBalance_UnitScript : public UnitScript {
public:
    AutoBalance_UnitScript() : UnitScript("AutoBalance_UnitScript") {

    }

    //    uint32 DealDamage(Unit *AttackerUnit, Unit *playerVictim, uint32 damage, DamageEffectType /*damagetype*/) {
    //        return _Modifer_DealDamage(playerVictim, AttackerUnit, damage);
    //    }

    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage) override
    {
        damage = _Modifer_DealDamage(target, attacker, damage);
    }

    void ModifySpellDamageTaken(Unit* target, Unit* attacker, float& damage, SpellInfo const* /**/) override
    {
        damage = _Modifer_DealDamage(target, attacker, damage);
    }

    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) override
    {
        damage = _Modifer_DealDamage(target, attacker, damage);
    }

    void ModifyHealRecieved(Unit* target, Unit* attacker, uint32& damage) override
    {
        damage = _Modifer_DealDamage(target, attacker, damage);
    }

    uint32 _Modifer_DealDamage(Unit* target, Unit* attacker, uint32 damage)
    {
        if (!enabled)
        {
            return damage;
        }

        if (!attacker || attacker->GetTypeId() == TYPEID_PLAYER || !attacker->IsInWorld())
        {
            return damage;
        }

        float damageMultiplier = attacker->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo")->DamageMultiplier;
        if (damageMultiplier == 1)
        {
            return damage;

        }

        if (!(!DungeonsOnly || (target->GetMap()->IsDungeon() && attacker->GetMap()->IsDungeon()) || (attacker->GetMap()->IsBattleground() && target->GetMap()->IsBattleground())))
        {
            return damage;
        }

        if ((attacker->isHunterPet() || attacker->isPet() || attacker->isSummon()) && attacker->IsControlledByPlayer())
        {
            return damage;
        }

        return damage * damageMultiplier;
    }
};

class AutoBalance_AllMapScript : public AllMapScript
{
public:
    AutoBalance_AllMapScript() : AllMapScript("AutoBalance_AllMapScript") { }

    void OnPlayerEnterAll(Map* map, Player* player)
    {
        if (!enabled)
        {
            return;
        }

        if (player->isGameMaster())
        {
            return;

        }

        AutoBalanceMapInfo* mapABInfo = map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");
        // always check level, even if not conf enabled
        // because we can enable at runtime and we need this information
        if (player)
        {
            if (player->getLevel() > mapABInfo->mapLevel)
            {
                mapABInfo->mapLevel = player->getLevel();

            }

        }
        else {
            Map::PlayerList const& playerList = map->GetPlayers();
            if (!playerList.isEmpty()) {
                for (Map::PlayerList::const_iterator playerIteration = playerList.begin(); playerIteration != playerList.end(); ++playerIteration)
                {
                    if (Player* playerHandle = playerIteration->getSource()) {
                        if (!playerHandle->isGameMaster() && playerHandle->getLevel() > mapABInfo->mapLevel)
                        {
                            mapABInfo->mapLevel = playerHandle->getLevel();

                        }

                    }
                }
            }
        }

        mapABInfo->playerCount++; //(maybe we've to found a safe solution to avoid player recount each time)
        //mapABInfo->playerCount = map->GetPlayersCountExceptGMs();

        if (PlayerChangeNotify)
        {
            if (map->GetEntry()->IsDungeon() && player)
            {
                Map::PlayerList const& playerList = map->GetPlayers();
                if (!playerList.isEmpty())
                {
                    for (Map::PlayerList::const_iterator playerIteration = playerList.begin(); playerIteration != playerList.end(); ++playerIteration)
                    {
                        if (Player* playerHandle = playerIteration->getSource())
                        {
                            std::ostringstream ss;
                            ss << "|cffFF0000[AutoBalance] |cffFF8000" << player->GetName() << " entered the Instance %s - Auto setting player count to %u (Player Difficulty Offset = %u) |r";
                            ChatHandler(player->GetSession()).PSendSysMessage(ss.str().c_str(), map->GetMapName(), mapABInfo->playerCount, PlayerCountDifficultyOffset);

                            /*ChatHandler chatHandle = ChatHandler(playerHandle->GetSession());
                            chatHandle.PSendSysMessage("|cffFF0000 [AutoBalance]|r|cffFF8000 %s entered the Instance %s. Auto setting player count to %u (Player Difficulty Offset = %u) |r",
                                player->GetName().c_str(), map->GetMapName(), mapABInfo->playerCount + PlayerCountDifficultyOffset, PlayerCountDifficultyOffset);*/
                        }
                    }
                }
            }
        }
    }

    void OnPlayerLeaveAll(Map* map, Player* player)
    {
        if (!enabled)
        {
            return;
        }

        if (player->isGameMaster())
        {
            return;
        }

        AutoBalanceMapInfo* mapABInfo = map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");
        // (maybe we've to found a safe solution to avoid player recount each time)
        mapABInfo->playerCount--;
        mapABInfo->playerCount = map->GetPlayersCountExceptGMs();

        // always check level, even if not conf enabled
        // because we can enable at runtime and we need this information
        if (!mapABInfo->playerCount)
        {
            mapABInfo->mapLevel = 0;
            return;
        }

        if (PlayerChangeNotify)
        {
            if (map->GetEntry()->IsDungeon() && player)
            {
                Map::PlayerList const& playerList = map->GetPlayers();
                if (!playerList.isEmpty())
                {
                    for (Map::PlayerList::const_iterator playerIteration = playerList.begin(); playerIteration != playerList.end(); ++playerIteration)
                    {
                        if (Player* playerHandle = playerIteration->getSource())
                        {
                            std::ostringstream ss;
                            ss << "|cffFF0000[AutoBalance] |cffFF8000" << player->GetName() << " left the Instance %s - Auto setting player count to %u (Player Difficulty Offset = %u) |r";
                            ChatHandler(player->GetSession()).PSendSysMessage(ss.str().c_str(), map->GetMapName(), mapABInfo->playerCount, PlayerCountDifficultyOffset);
                            /*ChatHandler chatHandle = ChatHandler(playerHandle->GetSession());
                            chatHandle.PSendSysMessage("|cffFF0000 [-AutoBalance]|r|cffFF8000 %s left the Instance %s. Auto setting player count to %u (Player Difficulty Offset = %u) |r",
                                player->GetName().c_str(), map->GetMapName(), mapABInfo->playerCount, PlayerCountDifficultyOffset);*/
                        }
                    }
                }
            }
        }
    }
};

class AutoBalance_AllCreatureScript : public AllCreatureScript {
public:
    AutoBalance_AllCreatureScript() : AllCreatureScript("AutoBalance_AllCreatureScript") { }

    void Creature_SelectLevel(const CreatureTemplate* /*creatureTemplate*/, Creature* creature) override
    {
        if (!enabled)
        {
            return;
        }
        ModifyCreatureAttributes(creature, true);
        TC_LOG_ERROR(LOG_FILTER_GENERAL, "MODDED CREATURES, %u", creature->GetName());

    }

    void OnAllCreatureUpdate(Creature* creature, uint32 /*diff*/) override
    {
        if (!enabled)
        {
            return;

        }
        ModifyCreatureAttributes(creature);

    }

    bool checkLevelOffset(uint8 selectedLevel, uint8 targetLevel) {
        return selectedLevel && ((targetLevel >= selectedLevel && targetLevel <= (selectedLevel + higherOffset)) || (targetLevel <= selectedLevel && targetLevel >= (selectedLevel - lowerOffset)));

    }

    void ModifyCreatureAttributes(Creature* creature, bool resetSelLevel = false)
    {
        if (!creature || !creature->GetMap())
        {
            return;
        }

        if (!creature->GetMap()->IsDungeon() && !creature->GetMap()->IsBattleground() && DungeonsOnly)
        {
            return;
        }

        if (((creature->isHunterPet() || creature->isPet() || creature->isSummon()) && creature->IsControlledByPlayer()))
        {
            return;
        }

        AutoBalanceMapInfo* mapABInfo = creature->GetMap()->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");
        if (!mapABInfo->mapLevel)
        {
            return;
        }

        CreatureTemplate const* creatureTemplate = creature->GetCreatureTemplate();
        InstanceMap* instanceMap = ((InstanceMap*)sMapMgr->FindMap(creature->GetMapId(), creature->GetInstanceId()));
        uint32 maxNumberOfPlayers = instanceMap->GetMaxPlayers();
        int forcedNumPlayers = GetForcedNumPlayers(creatureTemplate->Entry);

        if (forcedNumPlayers > 0)
        {
            // Force maxNumberOfPlayers to be changed to match the Configuration entries ForcedID2, ForcedID5, ForcedID10, ForcedID20, ForcedID25, ForcedID40
            maxNumberOfPlayers = forcedNumPlayers;
        }
        else if (forcedNumPlayers == 0)
        {
            // forcedNumPlayers 0 means that the creature is contained in DisabledID -> no scaling
            return;
        }

        AutoBalanceCreatureInfo* creatureABInfo = creature->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");
        // force resetting selected level.
        // this is also a "workaround" to fix bug of not recalculated
        // attributes when UpdateEntry has been used.
        // TODO: It's better and faster to implement a core hook
        // in that position and force a recalculation then
        if ((creatureABInfo->entry != 0 && creatureABInfo->entry != creature->GetEntry()) || resetSelLevel)
        {
            // force a recalculation
            creatureABInfo->selectedLevel = 0;
        }

        if (!creature->isAlive())
        {
            return;
        }

        uint32 curCount = mapABInfo->playerCount + PlayerCountDifficultyOffset;
        uint8 bonusLevel = creatureTemplate->isWorldBoss() /*== CREATURE_ELITE_WORLDBOSS ? 3 : 0*/;

        // already scaled
        if (creatureABInfo->selectedLevel > 0)
        {
            if (LevelScaling)
            {
                if (checkLevelOffset(mapABInfo->mapLevel + bonusLevel, creature->getLevel()) &&
                    checkLevelOffset(creatureABInfo->selectedLevel, creature->getLevel()) &&
                    creatureABInfo->instancePlayerCount == curCount)
                {
                    return;
                }
            }
            else if (creatureABInfo->instancePlayerCount == curCount)
            {
                return;
            }
        }

        creatureABInfo->instancePlayerCount = curCount;
        // no players in map, do not modify attributes
        if (!creatureABInfo->instancePlayerCount)
        {
            return;
        }

        uint8 originalLevel = creatureTemplate->maxlevel;
        uint8 level = mapABInfo->mapLevel;
        uint8 areaMinLvl, areaMaxLvl;
        Player* player;
        getAreaLevel(creature->GetMap(), creature->GetAreaId(), areaMinLvl, areaMaxLvl, player);

        // avoid level changing for critters and special creatures (spell summons etc.) in instances
        bool skipLevel = false;
        if (originalLevel <= 1 && areaMinLvl >= 5)
        {
            skipLevel = true;
        }

        if (LevelScaling && creature->GetMap()->IsDungeon() && !skipLevel && !checkLevelOffset(level, originalLevel))
        {
            // change level only whithin the offsets and when in dungeon/raid
            if (level != creatureABInfo->selectedLevel || creatureABInfo->selectedLevel != creature->getLevel())
            {
                // keep bosses +3 level
                creatureABInfo->selectedLevel = level + bonusLevel;
                creature->SetLevel(creatureABInfo->selectedLevel);
            }
        }
        else
        {
            creatureABInfo->selectedLevel = creature->getLevel();
        }

        creatureABInfo->entry = creature->GetEntry();
        bool useDefStats = false;
        if (LevelUseDb && creature->getLevel() >= creatureTemplate->minlevel && creature->getLevel() <= creatureTemplate->maxlevel)
        {
            useDefStats = true;
        }

        CreatureBaseStats const* origCreatureStats = sObjectMgr->GetCreatureBaseStats(originalLevel, creatureTemplate->unit_class);
        CreatureBaseStats const* creatureStats = sObjectMgr->GetCreatureBaseStats(creatureABInfo->selectedLevel, creatureTemplate->unit_class);

        uint32 baseHealth = origCreatureStats->GenerateHealth(creatureTemplate);
        uint32 baseMana = origCreatureStats->GenerateMana(creatureTemplate);
        uint32 scaledHealth = 0;
        uint32 scaledMana = 0;

        // Note: InflectionPoint handle the number of players required to get 50% health.
        //       you'd adjust this to raise or lower the hp modifier for per additional player in a non-whole group.
        //
        //       diff modify the rate of percentage increase between
        //       number of players. Generally the closer to the value of 1 you have this
        //       the less gradual the rate will be. For example in a 5 man it would take 3
        //       total players to face a mob at full health.
        //
        //       The +1 and /2 values raise the TanH function to a positive range and make
        //       sure the modifier never goes above the value or 1.0 or below 0.
        //
        float defaultMultiplier = 1.0f;
        if (creatureABInfo->instancePlayerCount < maxNumberOfPlayers)
        {
            float inflectionValue = (float)maxNumberOfPlayers;

            if (instanceMap->IsHeroic())
            {
                if (instanceMap->IsRaid())
                {
                    switch (instanceMap->GetMaxPlayers())
                    {
                    case 10:
                        inflectionValue *= InflectionPointRaid10MHeroic;
                        break;
                    case 25:
                        inflectionValue *= InflectionPointRaid25MHeroic;
                        break;
                    case 30:
                        inflectionValue *= InflectionPointRaid30MHeroic;
                        break;
                    default:
                        inflectionValue *= InflectionPointRaidHeroic;
                    }
                }
                else
                {
                    inflectionValue *= InflectionPointHeroic;
                }
            }
            else
            {
                if (instanceMap->IsRaid())
                {
                    switch (instanceMap->GetMaxPlayers())
                    {
                    case 10:
                        inflectionValue *= InflectionPointRaid10M;
                        break;
                    case 25:
                        inflectionValue *= InflectionPointRaid25M;
                        break;
                    case 30:
                        inflectionValue *= InflectionPointRaid30M;
                        break;
                    default:
                        inflectionValue *= InflectionPointRaid;

                    }
                }
                else
                {
                    inflectionValue *= InflectionPoint;
                }
            }
            if (creature->IsDungeonBoss())
            {
                inflectionValue *= BossInflectionMult;
            }

            float diff = ((float)maxNumberOfPlayers / 5) * 1.5f;
            defaultMultiplier = (tanh(((float)creatureABInfo->instancePlayerCount - inflectionValue) / diff) + 1.0f) / 2.0f;

        }

        creatureABInfo->HealthMultiplier = healthMultiplier * defaultMultiplier * globalRate;

        if (creatureABInfo->HealthMultiplier <= MinHPModifier)
        {
            creatureABInfo->HealthMultiplier = MinHPModifier;
        }

        float hpStatsRate = 1.0f;
        if (!useDefStats && LevelScaling && !skipLevel)
        {
            float newBaseHealth = 0;
            if (level <= 60)
            {
                newBaseHealth = creatureStats->BaseHealth[0];
            }
            else if (level <= 70)
            {
                newBaseHealth = creatureStats->BaseHealth[1];
            }
            else
            {
                newBaseHealth = creatureStats->BaseHealth[2];
                // special increasing for end-game contents
                if (LevelEndGameBoost)
                {
                    newBaseHealth *= creatureABInfo->selectedLevel >= 75 && originalLevel < 75 ? float(creatureABInfo->selectedLevel - 70) * 0.3f : 1;
                }
            }

            float newHealth = newBaseHealth * creatureTemplate->ModHealth;
            // allows health to be different with creatures that originally
            // differentiate their health by different level instead of multiplier field.
            // expecially in dungeons. The health reduction decrease if original level is similar to the area max level
            // AAAA
            if (originalLevel >= areaMinLvl && originalLevel < areaMaxLvl)
            {
                // never more than 30%
                float reduction = newHealth / float(areaMaxLvl - areaMinLvl) * (float(areaMaxLvl - originalLevel) * 0.3f);
                if (reduction > 0 && reduction < newHealth)
                {
                    newHealth -= reduction;
                }
            }
            hpStatsRate = newHealth / float(baseHealth);
        }

        creatureABInfo->HealthMultiplier *= hpStatsRate;
        scaledHealth = round(((float)baseHealth * creatureABInfo->HealthMultiplier) + 1.0f);

        // Getting the list of Classes in this group
        // This will be used later on to determine what additional scaling will be required based on the ratio of tank/dps/healer
        // Update playerClassList with the list of all the participating Classes
        // GetPlayerClassList(creature, playerClassList);

        float manaStatsRate = 1.0f;
        if (!useDefStats && LevelScaling && !skipLevel)
        {
            float newMana = creatureStats->GenerateMana(creatureTemplate);
            manaStatsRate = newMana / float(baseMana);
        }

        creatureABInfo->ManaMultiplier = manaStatsRate * manaMultiplier * defaultMultiplier * globalRate;
        if (creatureABInfo->ManaMultiplier <= MinManaModifier)
        {
            creatureABInfo->ManaMultiplier = MinManaModifier;
        }

        scaledMana = round(baseMana * creatureABInfo->ManaMultiplier);
        float damageMul = defaultMultiplier * globalRate * damageMultiplier;
        // Can not be less then Min_D_Mod
        if (damageMul <= MinDamageModifier)
        {
            damageMul = MinDamageModifier;
        }

        if (!useDefStats && LevelScaling && !skipLevel)
        {
            float origDmgBase = origCreatureStats->GenerateBaseDamage(creatureTemplate);
            float newDmgBase = 0;
            if (level <= 60)
            {
                newDmgBase = creatureStats->BaseDamage[0];
            }
            else if (level <= 70)
            {
                newDmgBase = creatureStats->BaseDamage[1];
            }
            else
            {
                newDmgBase = creatureStats->BaseDamage[2];
                // special increasing for end-game contents
                if (LevelEndGameBoost && !creature->GetMap()->IsRaid())
                    newDmgBase *= creatureABInfo->selectedLevel >= 75 && originalLevel < 75 ? float(creatureABInfo->selectedLevel - 70) * 0.3f : 1;
            }

            damageMul *= newDmgBase / origDmgBase;

        }

        creatureABInfo->ArmorMultiplier = defaultMultiplier * globalRate * armorMultiplier;
        uint32 newBaseArmor = round(creatureABInfo->ArmorMultiplier *
            ((useDefStats || !LevelScaling || skipLevel) ? origCreatureStats->GenerateArmor(creatureTemplate)
                : creatureStats->GenerateArmor(creatureTemplate)));
        uint32 prevMaxHealth = creature->GetMaxHealth();
        uint32 prevMaxPower = creature->GetMaxPower(POWER_MANA);
        uint32 prevHealth = creature->GetHealth();
        uint32 prevPower = creature->GetPower(POWER_MANA);
        Powers pType = creature->getPowerType();

        creature->SetArmor(newBaseArmor);
        creature->SetModifierValue(UNIT_MOD_ARMOR, BASE_VALUE, (float)newBaseArmor);
        creature->SetCreateHealth(scaledHealth);
        creature->SetMaxHealth(scaledHealth);
        creature->ResetPlayerDamageReq();
        creature->SetCreateMana(scaledMana);
        creature->SetMaxPower(POWER_MANA, scaledMana);
        creature->SetModifierValue(UNIT_MOD_ENERGY, BASE_VALUE, (float)100.0f);
        creature->SetModifierValue(UNIT_MOD_RAGE, BASE_VALUE, (float)100.0f);
        creature->SetModifierValue(UNIT_MOD_HEALTH, BASE_VALUE, (float)scaledHealth);
        creature->SetModifierValue(UNIT_MOD_MANA, BASE_VALUE, (float)scaledMana);
        creatureABInfo->DamageMultiplier = damageMul;

        uint32 scaledCurHealth = prevHealth && prevMaxHealth ? float(scaledHealth) / float(prevMaxHealth) * float(prevHealth) : 0;
        uint32 scaledCurPower = prevPower && prevMaxPower ? float(scaledMana) / float(prevMaxPower) * float(prevPower) : 0;

        creature->SetHealth(scaledCurHealth);
        if (pType == POWER_MANA)
        {
            creature->SetPower(POWER_MANA, scaledCurPower);
        }
        else
        {
            // fix creatures with different power types
            creature->setPowerType(pType);
        }
        creature->UpdateAllStats();
    }
};

//using namespace Trinity::ChatCommands;

class AutoBalance_CommandScript : public CommandScript
{
public:
    AutoBalance_CommandScript() : CommandScript("AutoBalance_CommandScript") { }

    std::vector<ChatCommand> GetCommands() const override
    {
        static std::vector<ChatCommand> ABCommandTable =
        {
            { "setoffset",      SEC_GAMEMASTER,     true,       &HandleABSetOffsetCommand,     ""},
            { "getoffset",      SEC_GAMEMASTER,     true,       &HandleABGetOffsetCommand,  ""},
            { "checkmap",       SEC_GAMEMASTER,     true,       &HandleABCheckMapCommand,   ""},
            { "mapstat",        SEC_GAMEMASTER,     true,       &HandleABMapStatsCommand,   ""},
            { "crstat",         SEC_GAMEMASTER,     true,       &HandleABCreatureStatsCommand,  ""},
        };
        static std::vector<ChatCommand> commandTable =
        {
            { "vas",    SEC_GAMEMASTER,     true,       NULL,       "",       ABCommandTable}
        };
        return commandTable;
    }

    static bool HandleABSetOffsetCommand(ChatHandler* handler, const char* args)
    {
        handler->PSendSysMessage("Changing Player Difficulty Offset to %i.",*args);
        uint32 arguments = *args;
        PlayerCountDifficultyOffset = arguments;
        return true;
    }

    static bool HandleABGetOffsetCommand(ChatHandler* handler, const char* args)
    {
        handler->PSendSysMessage("Current Player Difficulty Offset = %i", PlayerCountDifficultyOffset);
        return true;
    }

    static bool HandleABCheckMapCommand(ChatHandler* handler, const char* args)
    {
        Player* pl = handler->getSelectedPlayer();

        if (!pl)
        {
            handler->SendSysMessage(LANG_SELECT_PLAYER_OR_PET);
            handler->SetSentErrorMessage(true);
            return false;

        }

        AutoBalanceMapInfo* mapABInfo = pl->GetMap()->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");
        mapABInfo->playerCount = pl->GetMap()->GetPlayersCountExceptGMs();

        Map::PlayerList const& playerList = pl->GetMap()->GetPlayers();
        uint8 level = 0;
        if (!playerList.isEmpty())
        {
            for (Map::PlayerList::const_iterator playerIteration = playerList.begin();
                playerIteration != playerList.end(); ++playerIteration)
            {
                if (Player* playerHandle = playerIteration->getSource())
                {
                    if (playerHandle->getLevel() > level)
                    {
                        mapABInfo->mapLevel = level = playerHandle->getLevel();

                    }

                }

            }

        }
        HandleABMapStatsCommand(handler, NULL);
        return true;

    }

    static bool HandleABMapStatsCommand(ChatHandler* handler, const char* /*args*/)
    {
        Player* pl = handler->getSelectedPlayer();
        if (!pl)
        {
            handler->SendSysMessage(LANG_SELECT_PLAYER_OR_PET);
            handler->SetSentErrorMessage(true);
            return false;

        }

        AutoBalanceMapInfo* mapABInfo = pl->GetMap()->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");
        handler->PSendSysMessage("Players on map: %u", mapABInfo->playerCount);
        handler->PSendSysMessage("Max level of players in this map: %u", mapABInfo->mapLevel);
        return true;

    }

    static bool HandleABCreatureStatsCommand(ChatHandler* handler, const char* args)
    {
        Creature* target = handler->getSelectedCreature();

        if (!target)
        {
            handler->SendSysMessage(LANG_SELECT_CREATURE);
            handler->SetSentErrorMessage(true);
            return false;

        }

        AutoBalanceCreatureInfo* creatureABInfo = target->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");
        handler->PSendSysMessage("Instance player Count: %u", creatureABInfo->instancePlayerCount);
        handler->PSendSysMessage("Selected level: %u", creatureABInfo->selectedLevel);
        handler->PSendSysMessage("Damage multiplier: %.6f", creatureABInfo->DamageMultiplier);
        handler->PSendSysMessage("Health multiplier: %.6f", creatureABInfo->HealthMultiplier);
        handler->PSendSysMessage("Mana multiplier: %.6f", creatureABInfo->ManaMultiplier);
        handler->PSendSysMessage("Armor multiplier: %.6f", creatureABInfo->ArmorMultiplier);
        return true;
    }
};

void AddSC_AutoBalance()
{
    new AutoBalance_WorldScript;
    new AutoBalance_PlayerScript;
    new AutoBalance_UnitScript;
    new AutoBalance_AllCreatureScript;
    new AutoBalance_AllMapScript;
    new AutoBalance_CommandScript;
}