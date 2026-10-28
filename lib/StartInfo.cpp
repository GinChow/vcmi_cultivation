/*
 * StartInfo.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "StartInfo.h"
#include "CRandomGenerator.h"
#include "CSkillHandler.h"
#include "entities/hero/CHero.h"
#include "entities/hero/CHeroClass.h"
#include "entities/faction/CTown.h"
#include "mapObjects/CGHeroInstance.h"
#include "mapObjects/CGTownInstance.h"
#include "mapObjectConstructors/AObjectTypeHandler.h"
#include "mapObjectConstructors/CObjectClassesHandler.h"
#include "mapping/CMap.h"
#include "mapping/CMapEditManager.h"
#include "mapping/MapFormat.h"
#include "callback/EditorCallback.h"

#include "texts/CGeneralTextHandler.h"
#include "GameLibrary.h"
#include "entities/faction/CFaction.h"
#include "entities/faction/CTownHandler.h"
#include "entities/hero/CHeroHandler.h"
#include "rmg/CMapGenOptions.h"
#include "mapping/CMapInfo.h"
#include "campaign/CampaignState.h"
#include "mapping/CMapHeader.h"
#include "mapping/CMapService.h"
#include "modding/ModIncompatibility.h"
#include "serializer/JsonSerializeFormat.h"

PlayerSettings::PlayerSettings()
	: bonus(PlayerStartingBonus::RANDOM), color(0), compOnly(false)
{
}

FactionID PlayerSettings::getCastleValidated() const
{
	if (!castle.isValid())
		return FactionID(0);
	if (castle.getNum() < LIBRARY->townh->size() && castle.toEntity(LIBRARY)->hasTown())
		return castle;

	return FactionID(0);
}

HeroTypeID PlayerSettings::getHeroValidated() const
{
	if (!hero.isValid())
		return HeroTypeID(0);
	if (hero.getNum() < LIBRARY->heroh->size())
		return hero;

	return HeroTypeID(0);
}

bool PlayerSettings::isControlledByAI() const
{
	return connectedPlayerIDs.empty();
}

bool PlayerSettings::isControlledByHuman() const
{
	return !connectedPlayerIDs.empty();
}

PlayerSettings & StartInfo::getIthPlayersSettings(const PlayerColor & no)
{
	if(playerInfos.find(no) != playerInfos.end())
		return playerInfos[no];
	logGlobal->error("Cannot find info about player %s. Throwing...", no.toString());
	throw std::runtime_error("Cannot find info about player");
}
const PlayerSettings & StartInfo::getIthPlayersSettings(const PlayerColor & no) const
{
	return const_cast<StartInfo &>(*this).getIthPlayersSettings(no);
}

PlayerSettings * StartInfo::getPlayersSettings(PlayerConnectionID connectedPlayerId)
{
	for(auto & elem : playerInfos)
	{
		if(vstd::contains(elem.second.connectedPlayerIDs, connectedPlayerId))
			return &elem.second;
	}

	return nullptr;
}

std::string StartInfo::getCampaignName(const ITranslator * translator) const
{
	if(!campState->getNameTranslated(translator).empty())
		return campState->getNameTranslated(translator);
	else
		return LIBRARY->generaltexth->allTexts[508];
}

bool StartInfo::restrictedGarrisonsForAI() const
{
	return campState && campState->restrictedGarrisonsForAI();
}

EMapDifficulty StartInfo::getDifficulty() const
{
	return static_cast<EMapDifficulty>(difficulty);
}

void LobbyInfo::verifyStateBeforeStart(bool ignoreNoHuman) const
{
	if(!mi || !mi->mapHeader)
		throw std::domain_error(LIBRARY->generaltexth->translate("core.genrltxt.529"));

	auto missingMods = CMapService::verifyMapHeaderMods(*mi->mapHeader);
	ModIncompatibility::ModList modList;
	for(const auto & m : missingMods)
		modList.push_back(m.second.name);

	if(!modList.empty())
		throw ModIncompatibility(modList);

	//there must be at least one human player before game can be started
	std::map<PlayerColor, PlayerSettings>::const_iterator i;
	for(i = si->playerInfos.cbegin(); i != si->playerInfos.cend(); i++)
		if(i->second.isControlledByHuman())
			break;

	if(i == si->playerInfos.cend() && !ignoreNoHuman)
		throw std::domain_error(LIBRARY->generaltexth->translate("core.genrltxt.530"));

	if(si->mapGenOptions && si->mode == EStartMode::NEW_GAME)
	{
		if(!si->mapGenOptions->checkOptions())
			throw std::domain_error(LIBRARY->generaltexth->translate("core.genrltxt.751"));
	}
}

bool LobbyInfo::isClientHost(GameConnectionID clientId) const
{
	return clientId == hostClientId;
}

bool LobbyInfo::isPlayerHost(const PlayerColor & color) const
{
	return vstd::contains(getAllClientPlayers(hostClientId), color);
}

std::set<PlayerColor> LobbyInfo::getAllClientPlayers(GameConnectionID clientId) const
{
	std::set<PlayerColor> players;
	for(auto & elem : si->playerInfos)
	{
		if(isClientHost(clientId) && elem.second.isControlledByAI())
			players.insert(elem.first);

		for(PlayerConnectionID id : elem.second.connectedPlayerIDs)
		{
			if(vstd::contains(getConnectedPlayerIdsForClient(clientId), id))
				players.insert(elem.first);
		}
	}
	if(isClientHost(clientId))
		players.insert(PlayerColor::NEUTRAL);

	return players;
}

std::vector<PlayerConnectionID> LobbyInfo::getConnectedPlayerIdsForClient(GameConnectionID clientId) const
{
	std::vector<PlayerConnectionID> ids;

	for(const auto & pair : playerNames)
	{
		if(pair.second.connection == clientId)
		{
			for(auto & elem : si->playerInfos)
			{
				if(vstd::contains(elem.second.connectedPlayerIDs, pair.first))
					ids.push_back(pair.first);
			}
		}
	}
	return ids;
}

std::set<PlayerColor> LobbyInfo::clientHumanColors(GameConnectionID clientId)
{
	std::set<PlayerColor> players;
	for(auto & elem : si->playerInfos)
	{
		for(PlayerConnectionID id : elem.second.connectedPlayerIDs)
		{
			if(vstd::contains(getConnectedPlayerIdsForClient(clientId), id))
			{
				players.insert(elem.first);
			}
		}
	}

	return players;
}


PlayerColor LobbyInfo::clientFirstColor(GameConnectionID clientId) const
{
	for(auto & pair : si->playerInfos)
	{
		if(isClientColor(clientId, pair.first))
			return pair.first;
	}

	return PlayerColor::CANNOT_DETERMINE;
}

bool LobbyInfo::isClientColor(GameConnectionID clientId, const PlayerColor & color) const
{
	if(si->playerInfos.find(color) != si->playerInfos.end())
	{
		for(PlayerConnectionID id : si->playerInfos.find(color)->second.connectedPlayerIDs)
		{
			if(playerNames.find(id) != playerNames.end())
			{
				if(playerNames.find(id)->second.connection == clientId)
					return true;
			}
		}
	}
	return false;
}

PlayerConnectionID LobbyInfo::clientFirstId(GameConnectionID clientId) const
{
	for(const auto & pair : playerNames)
	{
		if(pair.second.connection == clientId)
			return pair.first;
	}

	throw std::runtime_error("LobbyInfo::clientFirstId: invalid GameConnectionID!");
}

PlayerInfo & LobbyInfo::getPlayerInfo(PlayerColor color)
{
	return mi->mapHeader->players[color.getNum()];
}

TeamID LobbyInfo::getPlayerTeamId(const PlayerColor & color)
{
	if(color.isValidPlayer())
		return getPlayerInfo(color).team;
	else
		return TeamID::NO_TEAM;
}

std::unique_ptr<CMap> BattleOnlyModeStartInfo::createMap(IGameInfoCallback * callback, int seed) const
{
	auto map = std::make_unique<CMap>(callback);
	if(auto * editor = dynamic_cast<EditorCallback *>(callback))
		editor->setMap(map.get());
	map->version = EMapFormat::VCMI;
	map->creationDateTime = 0;
	map->width = 10;
	map->height = 10;
	map->mapLayers = {MapLayerId::SURFACE};
	map->battleOnly = true;
	map->name = MetaString::createFromTextID("vcmi.lobby.battleOnlyMode");
	CRandomGenerator random(seed);
	auto rng = &random;

	map->initTerrain();
	map->getEditManager()->clearTerrain(rng);

	map->getEditManager()->getTerrainSelection().selectAll();
	map->getEditManager()->drawTerrain(selectedTerrain == TerrainId::NONE ? TerrainId::DIRT : selectedTerrain, 0, rng);

	map->players[0].canComputerPlay = true;
	map->players[0].canHumanPlay = true;
	map->players[1] = map->players[0];

	auto addHero = [&](int sel, PlayerColor color, const int3 & position)
	{
		auto factory = LIBRARY->objtypeh->getHandlerFor(Obj::HERO, selectedHero[sel].toHeroType()->heroClass->getId());
		auto templates = factory->getTemplates();
		auto obj = std::dynamic_pointer_cast<CGHeroInstance>(factory->create(callback, templates.front()));
		obj->setHeroType(selectedHero[sel]);

		obj->setOwner(color);
		obj->pos = position;
		if(commanderType[sel].hasValue())
			obj->setCommanderType(commanderType[sel]);
		obj->mana = startingMana[sel];
		for(size_t i=0; i<GameConstants::PRIMARY_SKILLS; i++)
			obj->pushPrimSkill(PrimarySkill(i), primSkillLevel[sel][i]);
		obj->clearSlots();
		for(int slot = 0; slot < GameConstants::ARMY_SIZE; slot++)
			if(selectedArmy[sel][slot].getId() != CreatureID::NONE && selectedArmy[sel][slot].getCount() > 0)
				obj->setCreature(SlotID(slot), selectedArmy[sel][slot].getId(), selectedArmy[sel][slot].getCount());

		// 场景明确控制法术书，避免英雄默认装备改变输入。
		if(!obj->getArt(ArtifactPosition::SPELLBOOK) && spellBook[sel])
			obj->putArtifact(ArtifactPosition::SPELLBOOK, map->createArtifact(ArtifactID::SPELLBOOK));
		else if(obj->getArt(ArtifactPosition::SPELLBOOK) && !spellBook[sel])
			obj->removeArtifact(ArtifactPosition::SPELLBOOK);

		if(warMachines[sel])
		{
			obj->putArtifact(ArtifactPosition::MACH1, map->createArtifact(ArtifactID::BALLISTA));
			obj->putArtifact(ArtifactPosition::MACH2, map->createArtifact(ArtifactID::AMMO_CART));
			obj->putArtifact(ArtifactPosition::MACH3, map->createArtifact(ArtifactID::FIRST_AID_TENT));
		}

		for(const auto & spell : spells[sel])
			obj->addSpellToSpellbook(spell);

		for(auto & artifact : artifacts[sel])
			if(artifact.second != ArtifactID::NONE)
				obj->putArtifact(artifact.first, map->createArtifact(artifact.second));

		for(const auto & skill : LIBRARY->skillh->objects)
			obj->setSecSkillLevel(SecondarySkill(skill->getId()), MasteryLevel::NONE, ChangeValueMode::ABSOLUTE);
		for(int skillSlot = 0; skillSlot < 8; skillSlot++)
			obj->setSecSkillLevel(secSkillLevel[sel][skillSlot].first, secSkillLevel[sel][skillSlot].second, ChangeValueMode::ABSOLUTE);

		map->getEditManager()->insertObject(obj);
	};

	addHero(0, PlayerColor(0), int3(5, 6, 0));
	if(selectedTown == FactionID::ANY)
		addHero(1, PlayerColor(1), int3(5, 5, 0));
	else
	{
		auto factory = LIBRARY->objtypeh->getHandlerFor(Obj::TOWN, selectedTown);
		auto templates = factory->getTemplates();
		auto obj = factory->create(callback, templates.front());
		auto townObj = std::dynamic_pointer_cast<CGTownInstance>(obj);
		obj->setOwner(PlayerColor(1));
		obj->pos = int3(5, 5, 0);
		for (const auto & building : townObj->getTown()->getAllBuildings())
			townObj->addBuilding(building);
		if(selectedHero[1] == HeroTypeID::NONE)
		{
			for(int slot = 0; slot < GameConstants::ARMY_SIZE; slot++)
				if(selectedArmy[1][slot].getId() != CreatureID::NONE && selectedArmy[1][slot].getCount() > 0)
					townObj->getArmy()->setCreature(SlotID(slot), selectedArmy[1][slot].getId(), selectedArmy[1][slot].getCount());
		}
		else
			addHero(1, PlayerColor(1), int3(5, 5, 0));

		map->getEditManager()->insertObject(townObj);
	}

	return map;
}

BattleOnlyModeStartInfo::BattleOnlyModeStartInfo()
	: selectedTerrain(TerrainId::DIRT)
	, selectedTown(FactionID::ANY)
{
	for(auto & element : primSkillLevel)
		for(size_t i=0; i<GameConstants::PRIMARY_SKILLS; i++)
			element[i] = 0;
	for(auto & element : warMachines)
		element = false;
	for(auto & element : spellBook)
		element = true;
	for(auto & element : secSkillLevel)
		element.fill(std::make_pair(SecondarySkill::NONE, MasteryLevel::NONE));
}

void BattleOnlyModeStartInfo::serializeJson(JsonSerializeFormat & handler)
{
	handler.serializeId("selectedTerrain", selectedTerrain);
	handler.serializeId("selectedTown", selectedTown);
	if(!handler.saving && selectedTown == FactionID::NONE)
		selectedTown = FactionID::ANY;

	auto slots = handler.enterArray("slots");
	slots.resize(2);
	for(int i = 0; i < 2; i++)
	{
		auto s = slots.enterStruct(i);
		s->serializeId("selectedHero", selectedHero[i]);
		{
			auto selectedArmyArray = s->enterArray("selectedArmy");
			selectedArmyArray.resize(GameConstants::ARMY_SIZE, JsonNode::JsonType::DATA_VECTOR);
			for(size_t j = 0; j < std::min(selectedArmyArray.size(), static_cast<size_t>(GameConstants::ARMY_SIZE)); j++)
			{
				JsonArraySerializer inner = selectedArmyArray.enterArray(j);
				selectedArmy[i][j].serializeJson(handler);
			}
		}
		{
			auto primSkillLevelArray = s->enterArray("primSkillLevel");
			primSkillLevelArray.resize(4, JsonNode::JsonType::DATA_VECTOR);
			for(size_t j = 0; j < std::min(primSkillLevelArray.size(), size_t(4)); j++)
				primSkillLevelArray.serializeInt(j, primSkillLevel[i][j]);
		}
		{
			auto secSkillLevelArray = s->enterArray("secSkillLevel");
			secSkillLevelArray.resize(8, JsonNode::JsonType::DATA_VECTOR);
			for(size_t j = 0; j < std::min(secSkillLevelArray.size(), size_t(8)); j++)
			{
				JsonArraySerializer inner = secSkillLevelArray.enterArray(j);
				inner->serializeId("skill", secSkillLevel[i][j].first);
				inner->serializeEnum("masteryLevel", secSkillLevel[i][j].second, MasteryLevel::NONE, {"none", "basic", "advanced", "expert"});
				if(!handler.saving && secSkillLevel[i][j].first == SecondarySkill::NONE)
					secSkillLevel[i][j].second = MasteryLevel::NONE;
			}
		}
		if(handler.saving)
		{
			auto reversed = vstd::reverseMap(artifacts[i]);
			std::map<ArtifactID, uint16_t> tmp;
			for (auto& [id, pos] : reversed)
    			tmp[id] = static_cast<uint16_t>(pos);
			tmp.erase(ArtifactID::NONE);
			s->serializeIdMap("artifacts", tmp);
		}
		else
		{
			std::map<ArtifactID, uint16_t> tmp;
			s->serializeIdMap("artifacts", tmp);
			std::map<ArtifactID, ArtifactPosition> converted;
			for (auto &[id, pos] : tmp)
				if(id != ArtifactID::NONE)
					converted[id] = ArtifactPosition(pos);
			artifacts[i] = vstd::reverseMap(converted);
		}
		s->serializeIdArray("spells", spells[i]);
		if(!handler.saving)
			spells[i].erase(std::remove(spells[i].begin(), spells[i].end(), SpellID::NONE), spells[i].end());
		s->serializeBool("warMachines", warMachines[i]);
		s->serializeBool("spellBook", spellBook[i]);
		s->serializeId("commander", commanderType[i]);
		s->serializeInt("mana", startingMana[i], -1);
	}
}
