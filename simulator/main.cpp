// 2026-10-26 · Codex；GPL-2.0-or-later，复用 VCMI 服务器和 BattleAI。
#include "Global.h"
#include "lib/callback/AIFactory.h"
#include "lib/callback/CBattleGameInterface.h"
#include "lib/GameLibrary.h"
#include "lib/StartInfo.h"
#include "lib/LoadProgress.h"
#include "lib/CStack.h"
#include "lib/callback/CBattleCallback.h"
#include "lib/callback/GameRandomizer.h"
#include "lib/callback/IClient.h"
#include "lib/gameState/CGameState.h"
#include "lib/mapObjects/CGHeroInstance.h"
#include "lib/mapping/CMap.h"
#include "lib/mapping/CMapService.h"
#include "lib/modding/ModScope.h"
#include "lib/networkPacks/PacksForClientBattle.h"
#include "lib/networkPacks/PacksForServer.h"
#include "lib/serializer/JsonDeserializer.h"
#include "server/CGameHandler.h"
#include "server/IGameServer.h"
#include "server/battles/BattleProcessor.h"
#include <fstream>
#include <iostream>

namespace
{
// 地图生成与原生仅战斗界面共享，内存装载避免临时文件覆盖。
class ScenarioMapService final : public CMapService
{
	const BattleOnlyModeStartInfo & scenario;
	int seed;
public:
	ScenarioMapService(const BattleOnlyModeStartInfo & scenario, int seed) : scenario(scenario), seed(seed) {}
	std::unique_ptr<CMap> loadMap(const ResourcePath &, IGameInfoCallback * cb) const override
	{
		return scenario.createMap(cb, seed);
	}
};

class Simulation final : public IGameServer, public IClient
{
	EServerState state = EServerState::GAMEPLAY;
	std::shared_ptr<CGameState> gs;
	std::shared_ptr<CGameHandler> handler;
	std::array<std::shared_ptr<CBattleCallback>, 2> callbacks;
	std::array<std::shared_ptr<CBattleGameInterface>, 2> ai;
	std::optional<BattleResult> result;
	std::optional<std::pair<PlayerColor, BattleAction>> pending;
	int rounds = 0;
	int actions = 0;
	JsonNode events;
	JsonNode initialUnits;
	JsonNode finalUnits;
	JsonNode spells;
	JsonNode mana;
	JsonNode damageEvents;
	int packetSequence = 0;

	JsonNode describeDamage(const BattleStackAttacked & hit, const std::string & kind, int fallbackSource = -1) const
	{
		const auto & battle = *gs->currentBattles.front();
		const auto * target = battle.battleGetUnitByID(hit.stackAttacked);
		const int sourceID = hit.attackerID == static_cast<ui32>(-1) ? fallbackSource : static_cast<int>(hit.attackerID);
		const auto * source = sourceID < 0 ? nullptr : battle.battleGetUnitByID(sourceID);
		JsonNode row;
		row["round"].Integer() = battle.getRound();
		row["packet"].Integer() = packetSequence;
		row["kind"].String() = kind;
		row["source"].Integer() = sourceID;
		row["source_side"].Integer() = static_cast<int>(source ? source->unitSide() : hit.sourceSide);
		row["hero_damage"].Bool() = hit.sourceHero || (source && source->unitSlot() == SlotID::COMMANDER_SLOT_PLACEHOLDER);
		row["source_creature"].String() = source ? source->unitType()->getJsonKey() : "";
		row["spell"].String() = hit.spellID.hasValue() ? SpellID::encode(hit.spellID.getNum()) : "";
		row["target"].Integer() = hit.stackAttacked;
		row["target_side"].Integer() = static_cast<int>(target->unitSide());
		row["target_hero_body"].Bool() = target->unitSlot() == SlotID::COMMANDER_SLOT_PLACEHOLDER;
		row["target_clone"].Bool() = target->isClone();
		row["health_before"].Integer() = target->getAvailableHealth();
		row["count_before"].Integer() = target->getCount();
		row["reported_damage"].Integer() = hit.damageAmount;
		return row;
	}

	JsonNode unitSnapshot(const BattleInfo & battle) const
	{
		JsonNode snapshot;
		for(const auto & unit : battle.stacks)
		{
			JsonNode row;
			row["id"].Integer() = unit->unitId();
			row["side"].Integer() = static_cast<int>(unit->unitSide());
			row["hero_body"].Bool() = unit->unitSlot() == SlotID::COMMANDER_SLOT_PLACEHOLDER;
			row["creature"].String() = unit->unitType()->getJsonKey();
			row["count"].Integer() = unit->getCount();
			row["health"].Integer() = unit->getAvailableHealth();
			row["hex"].Integer() = unit->getPosition().toInt();
			snapshot.Vector().push_back(row);
		}
		return snapshot;
	}

	void snapshotEnd()
	{
		if(gs->currentBattles.empty())
			return;
		const auto & battle = *gs->currentBattles.front();
		finalUnits = unitSnapshot(battle);
		mana.clear();
		for(BattleSide side : {BattleSide::ATTACKER, BattleSide::DEFENDER})
			mana.Vector().push_back(JsonNode(battle.getSideHero(side)->mana));
	}

public:
	void setState(EServerState value) override { state = value; }
	EServerState getState() const override { return state; }
	bool isPlayerHost(const PlayerColor &) const override { return true; }
	bool hasPlayerAt(PlayerColor, GameConnectionID) const override { return true; }
	bool hasBothPlayersAtSameConnection(PlayerColor, PlayerColor) const override { return true; }
	void sendPack(CPackForClient &, GameConnectionID) override {}
	std::optional<BattleAction> makeSurrenderRetreatDecision(PlayerColor, const BattleID &, const BattleStateInfoForRetreat &) override
	{
		return std::nullopt;
	}
	int sendRequest(const CPackForServer & request, PlayerColor player, bool) override
	{
		const auto * action = dynamic_cast<const MakeAction *>(&request);
		if(!action || pending)
			throw std::runtime_error("Unexpected or overlapping AI request");
		pending = std::make_pair(player, action->ba);
		return actions;
	}
	void applyPack(CPackForClient & pack) override
	{
		++packetSequence;
		std::vector<JsonNode> damage;
		if(const auto * attack = dynamic_cast<const BattleAttack *>(&pack))
			for(const auto & hit : attack->bsa)
				damage.push_back(describeDamage(hit, attack->counter() ? "counter" : attack->shot() ? "shot" : "attack", attack->stackAttacking));
		if(const auto * injury = dynamic_cast<const StacksInjured *>(&pack))
			for(const auto & hit : injury->stacks)
				damage.push_back(describeDamage(hit, hit.isSpell() ? "spell" : "effect"));
		if(const auto * completed = dynamic_cast<const BattleResult *>(&pack))
		{
			snapshotEnd();
			result = *completed;
		}
		if(const auto * cast = dynamic_cast<const BattleSpellCast *>(&pack))
		{
			JsonNode event;
			event["round"].Integer() = rounds;
			event["side"].Integer() = static_cast<int>(cast->side);
			event["spell"].String() = SpellID::encode(cast->spellID.getNum());
			event["hero"].Bool() = cast->castByHero;
			const auto * active = gs->currentBattles.front()->battleActiveUnit();
			event["active_unit"].Integer() = active ? static_cast<int>(active->unitId()) : -1;
			spells.Vector().push_back(event);
		}
		gs->apply(pack);
		if(dynamic_cast<const BattleStart *>(&pack))
			initialUnits = unitSnapshot(*gs->currentBattles.front());
		for(auto & event : damage)
		{
			const auto * target = gs->currentBattles.front()->battleGetUnitByID(event["target"].Integer());
			event["health_after"].Integer() = target->getAvailableHealth();
			event["count_after"].Integer() = target->getCount();
			event["damage"].Integer() = std::max<int64_t>(0, event["health_before"].Integer() - target->getAvailableHealth());
			event["killed"].Integer() = std::max<int64_t>(0, event["count_before"].Integer() - target->getCount());
			event["sequence"].Integer() = damageEvents.Vector().size();
			damageEvents.Vector().push_back(event);
		}
		if(!gs->currentBattles.empty())
			rounds = std::max(rounds, gs->currentBattles.front()->getRound());
	}

	JsonNode run(const BattleOnlyModeStartInfo & scenario, int seed, int maxRounds)
	{
		gs = std::make_shared<CGameState>();
		gs->preInit(LIBRARY);
		StartInfo start;
		start.mode = EStartMode::NEW_GAME;
		start.mapname = "cultivation-simulation";
		start.difficulty = 1;
		for(int i = 0; i < 2; ++i)
		{
			auto & player = start.playerInfos[PlayerColor(i)];
			player.color = PlayerColor(i);
			player.name = "Simulation";
			player.hero = scenario.selectedHero[i];
			player.castle = FactionID::CASTLE;
			player.bonus = PlayerStartingBonus::GOLD;
		}
		ScenarioMapService maps(scenario, seed);
		GameRandomizer randomizer(*gs);
		randomizer.setSeed(seed);
		Load::ProgressAccumulator progress;
		gs->init(&maps, &start, randomizer, progress, false);
		handler = std::make_shared<CGameHandler>(*this, gs);
		handler->randomizer->setSeed(seed);
		auto heroes = gs->getMap().getObjects<CGHeroInstance>();
		if(heroes.size() != 2)
			throw std::runtime_error("Simulation requires exactly two heroes");
		handler->battles->startBattle(heroes.at(0), heroes.at(1));
		auto * battle = gs->currentBattles.front().get();
		const BattleID id = battle->getBattleID();
		for(int i = 0; i < 2; ++i)
		{
			callbacks[i] = std::make_shared<CBattleCallback>(PlayerColor(i), this);
			callbacks[i]->onBattleStarted(battle);
			ai[i] = AIFactory::createBattleAI("BattleAI");
			ai[i]->initBattleInterface(handler, callbacks[i]);
			ai[i]->battleStart(id, heroes.at(0), heroes.at(1), heroes.at(1)->visitablePos(), heroes.at(0), heroes.at(1), BattleSide(i), false);
		}
		if(battle->tacticDistance > 0)
		{
			auto action = BattleAction::makeEndOFTacticPhase(battle->tacticsSide);
			if(!handler->battles->makePlayerBattleAction(id, battle->sideToPlayer(action.side), action))
				throw std::runtime_error("Cannot finish tactics phase");
		}
		while(!result && battle->getRound() <= maxRounds && actions < maxRounds * 100)
		{
			const auto * active = dynamic_cast<const CStack *>(battle->battleActiveUnit());
			if(!active)
				throw std::runtime_error("Battle stalled without an active unit");
			const int side = static_cast<int>(active->unitSide());
			ai.at(side)->activeStack(id, active);
			if(!pending)
				throw std::runtime_error("BattleAI returned without an action");
			auto [player, action] = *pending;
			pending.reset();
			JsonNode event;
			event["round"].Integer() = battle->getRound();
			event["side"].Integer() = side;
			event["unit"].Integer() = active->unitId();
			event["action"].Integer() = static_cast<int>(action.actionType);
			events.Vector().push_back(event);
			++actions;
			if(!handler->battles->makePlayerBattleAction(id, player, action))
				throw std::runtime_error("Server rejected BattleAI action");
		}
		JsonNode output;
		if(!result)
			snapshotEnd();
		output["schema"].Integer() = 3;
		output["engine"].String() = "VCMI";
		output["seed"].Integer() = seed;
		output["status"].String() = result ? "completed" : "round_limit";
		if(result)
			output["winner"].Integer() = static_cast<int>(result->winner);
		output["rounds"].Integer() = rounds;
		output["actions"].Integer() = actions;
		output["events"] = events;
		output["units_initial"] = initialUnits;
		output["units_final"] = finalUnits;
		output["spells"] = spells;
		output["mana_remaining"] = mana;
		output["damage_events"] = damageEvents;
		for(int side = 0; side < 2; ++side)
			output["heroes"].Vector().emplace_back(HeroTypeID::encode(scenario.selectedHero[side].getNum()));
		return output;
	}
};

JsonNode readJson(const std::string & path)
{
	std::ifstream stream(path);
	if(!stream)
		throw std::runtime_error("Cannot open scenario: " + path);
	std::string text((std::istreambuf_iterator<char>(stream)), {});
	return JsonNode(text.data(), text.size(), path);
}
}

int main(int argc, char ** argv)
{
	try
	{
		if(argc < 3 || argc > 6)
		{
			std::cerr << "Usage: battle_sim SCENARIO.json RESULTS.jsonl [SEED=1] [COUNT=1] [MAX_ROUNDS=100]\n";
			return 2;
		}
		const int seed = argc > 3 ? std::stoi(argv[3]) : 1;
		const int count = argc > 4 ? std::stoi(argv[4]) : 1;
		const int maxRounds = argc > 5 ? std::stoi(argv[5]) : 100;
		if(seed < 1 || count < 1 || count > 100000 || maxRounds < 1 || maxRounds > 10000 || seed > INT_MAX - count)
			throw std::runtime_error("Invalid seed, count or round limit");
		auto node = readJson(argv[1]);
		GameLibrary library;
		LIBRARY = &library;
		library.initializeFilesystem(false, false, {"vcmi", "cultivation"});
		library.initializeLibrary();
		node.setModScope(ModScope::scopeGame());
		JsonDeserializer decoder(nullptr, node);
		BattleOnlyModeStartInfo scenario;
		scenario.serializeJson(decoder);
		if(scenario.selectedTown != FactionID::ANY)
			throw std::runtime_error("The simulator currently accepts field battles only");
		for(int side = 0; side < 2; ++side)
		{
			if(!scenario.selectedHero[side].hasValue())
				throw std::runtime_error("Both sides require a valid hero");
			bool hasArmy = false;
			for(const auto & stack : scenario.selectedArmy[side])
				hasArmy |= stack.getId().hasValue() && stack.getCount() > 0;
			if(!hasArmy)
				throw std::runtime_error("Both sides require an army");
		}
		std::ofstream output(argv[2]);
		if(!output)
			throw std::runtime_error("Cannot open result file");
		for(int i = 0; i < count; ++i)
		{
			Simulation simulation;
			auto line = simulation.run(scenario, seed + i, maxRounds).toCompactString();
			// JsonWriter 会美化复杂节点；字符串内换行已转义，可去掉排版换行。
			std::erase(line, '\n');
			std::erase(line, '\r');
			output << line << '\n';
			output.flush();
			if(!output)
				throw std::runtime_error("Cannot write result file");
			std::cerr << "Completed " << i + 1 << '/' << count << '\n';
		}
		return 0;
	}
	catch(const std::exception & error)
	{
		std::cerr << "Simulation failed: " << error.what() << '\n';
		return 1;
	}
}
