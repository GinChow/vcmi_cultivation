// 2026-10-26 · Codex；验证模拟器与原生界面共享的场景生成。
#include "StdInc.h"
#include "../../lib/StartInfo.h"
#include "../../lib/callback/EditorCallback.h"
#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/mapObjects/army/CCommanderInstance.h"
#include "../../lib/mapping/CMap.h"
#include "../../lib/serializer/JsonDeserializer.h"
#include "../../lib/modding/ModScope.h"
#include "../server/battles/BattleTestFixture.h"
#include "../../lib/spells/CSpell.h"
#include "../../server/processors/NewTurnProcessor.h"
#include "../../lib/modding/ModManager.h"
#include "../../lib/battle/BattleAction.h"

TEST(BattleOnlyScenario, FixedPresetLoadsSubmodExactlyOnce)
{
	ModsPresetState preset(false, {"vcmi", "chinese-translation", "chinese-translation.chinesetruetypefonts"});
	const auto mods = preset.getActiveMods();
	EXPECT_EQ(std::count(mods.begin(), mods.end(), "chinese-translation"), 1);
	EXPECT_EQ(std::count(mods.begin(), mods.end(), "chinese-translation.chinesetruetypefonts"), 1);
}

TEST(BattleOnlyScenario, JsonArmiesBecomeTwoHeroesOnBattleOnlyMap)
{
	const std::string input = R"({"selectedTerrain":"core:dirt","slots":[
		{"selectedHero":"core:orrin","selectedArmy":[{"type":"core:pikeman","amount":20}],"primSkillLevel":[2,2,1,1]},
		{"selectedHero":"core:valeska","selectedArmy":[{"type":"core:archer","amount":10}],"primSkillLevel":[2,2,1,1]}
	]})";
	JsonNode node(input.data(), input.size(), "battle-test");
	node.setModScope(ModScope::scopeGame());
	JsonDeserializer decoder(nullptr, node);
	BattleOnlyModeStartInfo scenario;
	scenario.serializeJson(decoder);
	EditorCallback callback(nullptr);
	auto map = scenario.createMap(&callback, 1234);
	callback.setMap(map.get());
	ASSERT_TRUE(map->battleOnly);
	auto heroes = map->getObjects<CGHeroInstance>();
	ASSERT_EQ(heroes.size(), 2);
	EXPECT_EQ(heroes[0]->getOwner(), PlayerColor(0));
	EXPECT_EQ(heroes[1]->getOwner(), PlayerColor(1));
	EXPECT_EQ(heroes[0]->getStackCount(SlotID(0)), 20);
	EXPECT_EQ(heroes[1]->getStackCount(SlotID(0)), 10);
	EXPECT_EQ(heroes[0]->getHeroTypeID(), HeroTypeID(0));
	EXPECT_EQ(map->creationDateTime, 0);
}

TEST(BattleOnlyScenario, CommanderAndManaArePartOfScenario)
{
	BattleOnlyModeStartInfo scenario;
	scenario.selectedHero = {HeroTypeID(0), HeroTypeID(1)};
	scenario.selectedArmy[0][0] = CStackBasicDescriptor(CreatureID(0), 20);
	scenario.selectedArmy[1][0] = CStackBasicDescriptor(CreatureID(0), 20);
	scenario.commanderType = {CreatureID(12), CreatureID(12)};
	scenario.startingMana = {3, 0};
	EditorCallback callback(nullptr);
	auto map = scenario.createMap(&callback, 42);
	auto heroes = map->getObjects<CGHeroInstance>();
	ASSERT_EQ(heroes.size(), 2);
	for(const auto * hero : heroes)
	{
		ASSERT_NE(hero->getCommander(), nullptr);
		EXPECT_TRUE(hero->getCommander()->alive);
		EXPECT_EQ(hero->getCommander()->getId(), CreatureID(12));
		EXPECT_EQ(hero->getCommander()->getCount(), 1);
	}
	EXPECT_EQ(heroes[0]->mana, 3);
	EXPECT_EQ(heroes[1]->mana, 0);
}

class HeroBodyBattleTest : public BattleTestFixture {};

TEST_F(HeroBodyBattleTest, ServerOnlyAllowsMagicDuringOwnBodyTurn)
{
	startGame();
	attackerSideHero->setCommanderType(creatureByName("cultivation:swordQi"));
	defenderSideHero->setCommanderType(creatureByName("cultivation:demonQi"));
	teachSpell(attackerSideHero, SpellID::MAGIC_ARROW);
	startBattle();
	beginCombat();
	const auto all = battle()->getStacksIf([](const CStack * unit) { return unit->alive(); });
	const CStack * ownBody = nullptr;
	const CStack * ownArmy = nullptr;
	const CStack * enemyBody = nullptr;
	for(const auto * unit : all)
	{
		if(unit->unitSlot() == SlotID::COMMANDER_SLOT_PLACEHOLDER)
		{
			if(unit->unitSide() == BattleSide::ATTACKER) ownBody = unit;
			else enemyBody = unit;
		}
		else if(unit->unitSide() == BattleSide::ATTACKER) ownArmy = unit;
	}
	ASSERT_NE(ownBody, nullptr);
	ASSERT_NE(ownArmy, nullptr);
	ASSERT_NE(enemyBody, nullptr);
	BattleAction action;
	action.actionType = EActionType::HERO_SPELL;
	action.side = BattleSide::ATTACKER;
	action.stackNumber = -1;
	action.spell = SpellID::MAGIC_ARROW;
	action.aimToUnit(enemyBody);
	const auto mana = attackerSideHero->mana;
	const auto hp = enemyBody->getAvailableHealth();
	for(const auto * active : {ownArmy, enemyBody})
	{
		battle()->activeStack = active->unitId();
		EXPECT_EQ(battle()->battleCanCastSpell(attackerSideHero, spells::Mode::HERO), ESpellCastProblem::INVALID);
		EXPECT_FALSE(act(action));
		EXPECT_EQ(attackerSideHero->mana, mana);
		EXPECT_EQ(enemyBody->getAvailableHealth(), hp);
	}
	battle()->activeStack = ownBody->unitId();
	EXPECT_EQ(battle()->battleCanCastSpell(attackerSideHero, spells::Mode::HERO), ESpellCastProblem::OK);
	ASSERT_TRUE(act(action));
	EXPECT_LT(attackerSideHero->mana, mana);
	EXPECT_LT(enemyBody->getAvailableHealth(), hp);
	EXPECT_EQ(battle()->battleActiveUnit()->unitId(), ownBody->unitId());
	EXPECT_FALSE(act(action));
	ASSERT_TRUE(defend(ownBody));
	EXPECT_NE(battle()->battleActiveUnit()->unitId(), ownBody->unitId());
}

TEST_F(HeroBodyBattleTest, NoActiveUnitCannotOpenBodySpellWindow)
{
	startGame();
	attackerSideHero->setCommanderType(creatureByName("cultivation:swordQi"));
	teachSpell(attackerSideHero, SpellID::MAGIC_ARROW);
	startBattle();
	battle()->activeStack = -1;
	EXPECT_EQ(battle()->battleCanCastSpell(attackerSideHero, spells::Mode::HERO), ESpellCastProblem::INVALID);
}

TEST_F(HeroBodyBattleTest, BattleOnlyOpeningPreservesExplicitMana)
{
	startGame();
	gameState()->getMap().battleOnly = true;
	attackerSideHero->mana = 60;
	defenderSideHero->mana = 0;
	NewTurnProcessor(gameHandler.get()).onNewTurn();
	EXPECT_EQ(attackerSideHero->mana, 60);
	EXPECT_EQ(defenderSideHero->mana, 0);
}

TEST_F(HeroBodyBattleTest, BodyDeathBlocksCastingButLeavesArmyInBattle)
{
	startGame();
	attackerSideHero->setCommanderType(CreatureID(12));
	teachSpell(attackerSideHero, SpellID::MAGIC_ARROW);
	startBattle();
	beginCombat();
	activateHeroCaster(attackerSideHero);
	EXPECT_EQ(battle()->battleCanCastSpell(attackerSideHero, spells::Mode::HERO), ESpellCastProblem::OK);
	const CStack * body = nullptr;
	for(const auto & unit : battle()->stacks)
		if(unit->unitSide() == BattleSide::ATTACKER && unit->unitSlot() == SlotID::COMMANDER_SLOT_PLACEHOLDER)
			body = unit.get();
	ASSERT_NE(body, nullptr);
	injure(body, body->getAvailableHealth());
	EXPECT_FALSE(body->alive());
	EXPECT_EQ(battle()->battleCanCastSpell(attackerSideHero, spells::Mode::HERO), ESpellCastProblem::NO_HERO_TO_CAST_SPELL);
	EXPECT_TRUE(server.battleResults.empty());
	EXPECT_TRUE(std::any_of(battle()->stacks.begin(), battle()->stacks.end(), [](const auto & unit)
	{
		return unit->unitSide() == BattleSide::ATTACKER && unit->unitSlot() != SlotID::COMMANDER_SLOT_PLACEHOLDER && unit->alive();
	}));
}
