// 2026-10-26 · Codex。通过原生伤害预估及服务器攻击验证境界公式。
#include "StdInc.h"
#include "../server/battles/BattleTestFixture.h"
#include "../../lib/battle/BattleAttackInfo.h"
#include "../../lib/bonuses/Bonus.h"
#include "../../lib/bonuses/BonusCustomTypes.h"
#include "../../server/CGameHandler.h"
#include "../../server/battles/BattleProcessor.h"
#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/spells/CSpell.h"
#include "../../lib/spells/ISpellMechanics.h"
#include "../../lib/battle/CObstacleInstance.h"
#include "../../lib/spells/Problem.h"

namespace
{
void grant(CStack * unit, const std::string & name, int value)
{
	const auto type = BonusTypeID(BonusTypeID::decode("cultivation:" + name)).toEnum();
	unit->addNewBonus(std::make_shared<Bonus>(BonusDuration::PERMANENT, type, BonusSource::OTHER, value, BonusSourceID()));
}

void realm(CStack * unit, int value)
{
	grant(unit, "REALM", value);
	grant(unit, "REALM_SUPPRESSION_STEP", 50);
	grant(unit, "REALM_SUPPRESSION_CAP", 3);
}
}

class CultivationSectTest : public BattleTestFixture, public ::testing::WithParamInterface<int>
{
public:
	CStack * setup(const std::string & school, const std::string & skill = "", const std::string & realmName = "Qi")
	{
		startGame();
		attackerSideHero->setCommanderType(creatureByName("cultivation:" + school + realmName));
		if(!skill.empty())
			attackerSideHero->setSecSkillLevel(skillByName("cultivation:" + skill), GetParam(), ChangeValueMode::ABSOLUTE);
		teachSpell(attackerSideHero, SpellID::MAGIC_ARROW);
		attackerSideHero->setCreature(SlotID(0), creatureByName("cultivation:disciple"), 50);
		defenderSideHero->setCreature(SlotID(0), creatureByName("cultivation:disciple"), 100);
		defenderSideHero->setCreature(SlotID(1), creatureByName("cultivation:disciple"), 10);
		startBattle();
		auto bodies = battle()->getStacksIf([](const CStack * unit) { return unit->unitSlot() == SlotID::COMMANDER_SLOT_PLACEHOLDER; });
		auto * body = battle()->getStack(bodies.front()->unitId());
		body->setPosition(BattleHex(leftHex));
		return body;
	}
	bool cast(const std::string & name, spells::Target targets)
	{
		activateHeroCaster(attackerSideHero);
		if(targets.empty()) targets.emplace_back(BattleHex::INVALID);
		const auto * spell = spellByName("cultivation:" + name).toSpell();
		spells::BattleCast cast(battle(), attackerSideHero, spells::Mode::HERO, spell);
		auto mechanics = spell->battleMechanics(&cast);
		spells::detail::ProblemImpl problem;
		if(!mechanics->canBeCast(problem) || !mechanics->canBeCastAt(targets, problem))
		{
			std::vector<std::string> messages;
			problem.getAll(messages);
			for(const auto & message : messages) std::cout << name << ": " << message << std::endl;
			return false;
		}
		cast.cast(gameHandler->spellEnv.get(), targets);
		return true;
	}
};

TEST_P(CultivationSectTest, SwordEscapeMovesOnlyBodyAndConsumesNextStrikeBonus)
{
	auto * body = setup("sword", "swordEscapeArt");
	auto * enemy = addStack(BattleSide::DEFENDER, creatureByName("cultivation:swordDisciple"), BattleHex(rightHex + 2), 1000);
	beginCombat();
	EXPECT_FALSE(cast("swordEscape", {spells::Destination(enemy), spells::Destination(BattleHex(rightHex + 1))}));
	const auto mana = attackerSideHero->mana;
	ASSERT_TRUE(cast("swordEscape", {spells::Destination(body), spells::Destination(BattleHex(rightHex + 1))}));
	EXPECT_EQ(body->getPosition(), BattleHex(rightHex + 1));
	EXPECT_EQ(attackerSideHero->mana, mana - (14 - 2 * GetParam()));
	EXPECT_EQ(body->valOfBonuses(BonusType::DOUBLE_DAMAGE_CHANCE), 100);
	const auto hp = body->getAvailableHealth();
	ASSERT_TRUE(attack(body, enemy->getPosition()));
	EXPECT_EQ(body->getAvailableHealth(), hp);
	EXPECT_EQ(body->valOfBonuses(BonusType::DOUBLE_DAMAGE_CHANCE), 0);
}

TEST_P(CultivationSectTest, SwordCloneIsTemporary)
{
	auto * body = setup("sword", "swordCloneArt");
	beginCombat();
	ASSERT_TRUE(cast("swordClone", {spells::Destination(body)}));
	auto clones = battle()->getStacksIf([](const CStack * unit) { return unit->isClone(); });
	ASSERT_EQ(clones.size(), 1);
	EXPECT_EQ(clones.front()->creatureId(), body->creatureId());
	EXPECT_EQ(clones.front()->getCount(), 1);
	for(int i = 0; i < GetParam(); ++i) endRound();
	EXPECT_TRUE(battle()->getStacksIf([](const CStack * unit) { return unit->alive() && unit->isClone(); }).empty());
}

TEST_P(CultivationSectTest, DemonBloodHealsOwnFactionAndDamagesRighteousEnemy)
{
	auto * body = setup("demon", "demonBloodArt");
	injure(body, 100);
	auto * enemy = addStack(BattleSide::DEFENDER, creatureByName("cultivation:disciple"), BattleHex(rightHex + 2), 100);
	auto * demon = addStack(BattleSide::DEFENDER, creatureByName("cultivation:demonDisciple"), BattleHex(rightHex + 3), 100);
	beginCombat();
	EXPECT_FALSE(cast("demonBlood", {spells::Destination(demon)}));
	const auto hp = body->getAvailableHealth();
	ASSERT_TRUE(cast("demonBlood", {spells::Destination(body)}));
	EXPECT_EQ(body->getAvailableHealth() - hp, 15 + 20 * GetParam());
	endRound(); endRound();
	const auto enemyHP = enemy->getAvailableHealth();
	ASSERT_TRUE(cast("demonBlood", {spells::Destination(enemy)}));
	EXPECT_EQ(enemyHP - enemy->getAvailableHealth(), 15 + 20 * GetParam());
}

TEST_P(CultivationSectTest, EvilWindHasRadiusDurationAndDoesNotStack)
{
	setup("demon", "evilWindArt");
	auto * near = addStack(BattleSide::ATTACKER, creatureByName("cultivation:demonDisciple"), BattleHex(leftHex - 1), 10);
	auto * far = addStack(BattleSide::ATTACKER, creatureByName("cultivation:demonDisciple"), BattleHex(leftHex - 4), 10);
	beginCombat();
	const auto before = near->valOfBonuses(BonusType::STACKS_SPEED);
	const auto farBefore = far->valOfBonuses(BonusType::STACKS_SPEED);
	ASSERT_TRUE(cast("evilWind", {}));
	EXPECT_EQ(near->valOfBonuses(BonusType::STACKS_SPEED), before + GetParam());
	EXPECT_EQ(far->valOfBonuses(BonusType::STACKS_SPEED), farBefore);
	EXPECT_FALSE(cast("evilWind", {}));
	endRound(); endRound();
	EXPECT_EQ(near->valOfBonuses(BonusType::STACKS_SPEED), before);
}

TEST_P(CultivationSectTest, TalismanSummonsRankedNumberOfTemporarySoldiers)
{
	setup("talisman", "talismanSoldierArt");
	beginCombat();
	ASSERT_TRUE(cast("summonTalisman", {}));
	auto summons = battle()->getStacksIf([](const CStack * unit) { return unit->isSummoned(); });
	ASSERT_EQ(summons.size(), 1);
	EXPECT_EQ(summons.front()->getCount(), 1 + 2 * GetParam());
	EXPECT_EQ(summons.front()->creatureId(), creatureByName("cultivation:talismanSoldier"));
}

TEST_P(CultivationSectTest, ThunderHitsAreaWithoutFriendlyFire)
{
	setup("talisman", "thunderTalismanArt");
	auto * center = addStack(BattleSide::DEFENDER, creatureByName("cultivation:disciple"), BattleHex(rightHex + 2), 100);
	auto * near = addStack(BattleSide::DEFENDER, creatureByName("cultivation:disciple"), BattleHex(rightHex + 3), 100);
	auto * ally = addStack(BattleSide::ATTACKER, creatureByName("cultivation:disciple"), BattleHex(rightHex + 1), 100);
	beginCombat();
	const auto hp = center->getAvailableHealth();
	ASSERT_TRUE(cast("thunderTalisman", {spells::Destination(center)}));
	EXPECT_EQ(hp - center->getAvailableHealth(), 10 + 15 * GetParam());
	EXPECT_EQ(center->getAvailableHealth(), near->getAvailableHealth());
	EXPECT_EQ(ally->getAvailableHealth(), hp);
}

TEST_P(CultivationSectTest, PurificationDispelsAndUsesRankedMana)
{
	auto * body = setup("talisman", "purifyTalismanArt");
	attackerSideHero->setPrimarySkill(PrimarySkill::SPELL_POWER, 3, ChangeValueMode::ABSOLUTE);
	teachSpell(attackerSideHero, SpellID::HASTE);
	beginCombat();
	ASSERT_TRUE(castOn(attackerSideHero, SpellID::HASTE, body));
	endRound();
	const auto mana = attackerSideHero->mana;
	ASSERT_TRUE(cast("purifyTalisman", {spells::Destination(body)}));
	EXPECT_EQ(attackerSideHero->mana, mana - (10 - 2 * GetParam()));
	EXPECT_FALSE(body->hasBonusFrom(BonusSource::SPELL_EFFECT, SpellID(SpellID::HASTE)));
}

TEST_P(CultivationSectTest, CorpsePuppetConsumesSharedResurrectionBudgetAndCorpse)
{
	auto * body = setup("demon", "", "Core");
	teachSpell(attackerSideHero, spellByName("cultivation:bloodPuppet"));
	addStack(BattleSide::ATTACKER, creatureByName("cultivation:demonDisciple"), BattleHex(leftHex - 1), 50);
	auto corpses = battle()->getStacksIf([](const CStack * unit) { return unit->unitSide() == BattleSide::DEFENDER && unit->unitSlot() == SlotID(0); });
	auto * corpse = battle()->getStack(corpses.front()->unitId());
	corpse->setPosition(BattleHex(rightHex + 2));
	const auto corpseId = corpse->unitId();
	beginCombat();
	const auto budgetType = BonusTypeID(BonusTypeID::decode("cultivation:RESURRECTION_BUDGET")).toEnum();
	const auto usedType = BonusTypeID(BonusTypeID::decode("cultivation:RESURRECTION_USED")).toEnum();
	const auto budget = body->valOfBonuses(budgetType);
	ASSERT_GT(budget, 0);
	injure(corpse, corpse->getAvailableHealth());
	ASSERT_FALSE(corpse->alive());
	ASSERT_FALSE(corpse->isSummoned());
	ASSERT_FALSE(corpse->isClone());
	ASSERT_EQ(corpse->unitBaseAmount(), 100);
	ASSERT_TRUE(cast("bloodPuppet", {spells::Destination(corpse)}));
	auto puppets = battle()->getStacksIf([&](const CStack * unit) { return unit->creatureId() == creatureByName("cultivation:bloodPuppet"); });
	ASSERT_EQ(puppets.size(), 1);
	EXPECT_EQ(puppets.front()->getCount(), budget);
	EXPECT_EQ(body->valOfBonuses(usedType), budget);
	EXPECT_FALSE(battle()->getStack(corpseId, false)->isValidTarget(true));
	endRound(); endRound(); endRound();
	auto others = battle()->getStacksIf([](const CStack * unit) { return unit->unitSide() == BattleSide::DEFENDER && unit->unitSlot() == SlotID(1); });
	auto * other = battle()->getStack(others.front()->unitId());
	injure(other, other->getAvailableHealth());
	EXPECT_FALSE(cast("bloodPuppet", {spells::Destination(other)}));
	grant(body, "RESURRECTION_BUDGET", 20);
	EXPECT_FALSE(cast("bloodPuppet", {spells::Destination(corpse)}));
}

TEST_P(CultivationSectTest, FormationCreatesSevenVisibleTilesAndExpires)
{
	setup("talisman", "", "Core");
	teachSpell(attackerSideHero, spellByName("cultivation:talismanFormation"));
	beginCombat();
	const auto before = battle()->obstacles.size();
	ASSERT_TRUE(cast("talismanFormation", {spells::Destination(BattleHex(8, 2))}));
	EXPECT_EQ(battle()->obstacles.size(), before + 7);
	for(int i = 0; i < 3; ++i) endRound();
	EXPECT_EQ(battle()->obstacles.size(), before);
}

TEST_P(CultivationSectTest, SwordIntentOnlyBoostsOwnBodyMelee)
{
	auto * body = setup("sword", "swordIntentArt");
	auto * ally = addStack(BattleSide::ATTACKER, creatureByName("cultivation:swordDisciple"), BattleHex(leftHex - 1), 10);
	EXPECT_EQ(body->valOfBonuses(BonusType::PERCENTAGE_DAMAGE_BOOST), 10 * GetParam());
	EXPECT_EQ(ally->valOfBonuses(BonusType::PERCENTAGE_DAMAGE_BOOST), 0);
}

TEST_P(CultivationSectTest, BloodDrainRestoresFractionOfActualDamage)
{
	auto * body = setup("demon", "bloodDrainArt");
	auto * enemy = addStack(BattleSide::DEFENDER, creatureByName("cultivation:disciple"), BattleHex(rightHex), 100);
	blockRetaliation(body);
	forceMaximumDamage(body);
	injure(body, 100);
	beginCombat();
	const auto hp = body->getAvailableHealth();
	const auto enemyHP = enemy->getAvailableHealth();
	ASSERT_TRUE(attack(body, enemy->getPosition()));
	const auto damage = enemyHP - enemy->getAvailableHealth();
	EXPECT_EQ(body->getAvailableHealth() - hp, damage * (5 + 15 * GetParam()) / 100);
}

TEST_P(CultivationSectTest, CloneCannotServeAsTechniqueCaster)
{
	auto * body = setup("sword", "swordEscapeArt");
	makeClone(body);
	beginCombat();
	const auto mana = attackerSideHero->mana;
	EXPECT_FALSE(cast("swordEscape", {spells::Destination(body), spells::Destination(BattleHex(rightHex + 1))}));
	EXPECT_EQ(attackerSideHero->mana, mana);
}

TEST_P(CultivationSectTest, FormationDamagesUnitsEnteringItsTiles)
{
	setup("talisman", "", "Core");
	teachSpell(attackerSideHero, spellByName("cultivation:talismanFormation"));
	auto * enemy = addStack(BattleSide::DEFENDER, creatureByName("cultivation:disciple"), BattleHex(10, 2), 100);
	beginCombat();
	ASSERT_TRUE(cast("talismanFormation", {spells::Destination(BattleHex(8, 2))}));
	const auto hp = enemy->getAvailableHealth();
	ASSERT_TRUE(move(enemy, BattleHex(9, 2)));
	EXPECT_LT(enemy->getAvailableHealth(), hp);
	EXPECT_FALSE(server.castsOf(spellByName("cultivation:formationTrigger")).empty());
	int attributed = 0;
	for(const auto & pack : server.injuries)
		for(const auto & hit : pack.stacks)
			if(hit.spellID == spellByName("cultivation:formationTrigger"))
			{
				EXPECT_EQ(hit.sourceSide, BattleSide::ATTACKER);
				EXPECT_TRUE(hit.sourceHero);
				++attributed;
			}
	EXPECT_GT(attributed, 0);
}

TEST_P(CultivationSectTest, EnemyDomainSuppressesEvilWindWithoutSpendingMana)
{
	auto * body = setup("demon", "evilWindArt", "Soul");
	addStack(BattleSide::DEFENDER, creatureByName("cultivation:talismanSoul"), BattleHex(rightHex), 1);
	beginCombat();
	const auto mana = attackerSideHero->mana;
	const auto speed = body->valOfBonuses(BonusType::STACKS_SPEED);
	EXPECT_FALSE(cast("evilWind", {}));
	EXPECT_EQ(attackerSideHero->mana, mana);
	EXPECT_EQ(body->valOfBonuses(BonusType::STACKS_SPEED), speed);
}

INSTANTIATE_TEST_SUITE_P(ThreeRanks, CultivationSectTest, ::testing::Values(1, 2, 3));

class CultivationRealmTest : public BattleTestFixture, public ::testing::WithParamInterface<std::tuple<int, int, int>> {};

TEST_P(CultivationRealmTest, EstimateAndServerDamageFollowCappedRatio)
{
	startGame();
	startBattle();
	auto * attacker = addStack(BattleSide::ATTACKER, creatureByName("vcmi-test:testStackExperience"), BattleHex(leftHex), 1);
	auto * defender = addStack(BattleSide::DEFENDER, creatureByName("vcmi-test:testStackExperience"), BattleHex(rightHex), 100);
	forceMaximumDamage(attacker);
	blockRetaliation(attacker);
	auto [attackRealm, defenseRealm, expected] = GetParam();
	realm(attacker, attackRealm);
	realm(defender, defenseRealm);
	const auto predicted = battle()->calculateDmgRange(BattleAttackInfo(attacker, defender, 0, false));
	EXPECT_EQ(predicted.damage.min, expected);
	EXPECT_EQ(predicted.damage.max, expected);
	beginCombat();
	const auto health = defender->getAvailableHealth();
	ASSERT_TRUE(attack(attacker, defender->getPosition()));
	EXPECT_EQ(health - defender->getAvailableHealth(), expected);
}

INSTANTIATE_TEST_SUITE_P(RealmDifferences, CultivationRealmTest, ::testing::Values(
	std::make_tuple(0, 0, 10), std::make_tuple(3, 3, 10),
	std::make_tuple(2, 1, 15), std::make_tuple(3, 1, 20),
	std::make_tuple(5, 0, 25), std::make_tuple(1, 2, 6),
	std::make_tuple(1, 3, 5), std::make_tuple(0, 5, 4)));

class CultivationInteractionTest : public BattleTestFixture {};

class CultivationTechniqueTest : public BattleTestFixture {};

class CultivationDiamondTest : public BattleTestFixture, public ::testing::WithParamInterface<int> {};

TEST_P(CultivationDiamondTest, DomainDefenseAndCounterBonusMatchServerDamage)
{
	startGame();
	const auto rank = GetParam();
	defenderSideHero->setCommanderType(creatureByName("cultivation:bodySoul"));
	defenderSideHero->setSecSkillLevel(skillByName("cultivation:diamondArt"), rank, ChangeValueMode::ABSOLUTE);
	startBattle();
	auto bodies = battle()->getStacksIf([](const CStack * unit) { return unit->unitSlot() == SlotID::COMMANDER_SLOT_PLACEHOLDER; });
	ASSERT_EQ(bodies.size(), 1);
	auto * body = battle()->getStack(bodies.front()->unitId());
	body->setPosition(BattleHex(rightHex + 2));
	auto * attacker = addStack(BattleSide::ATTACKER, creatureByName("vcmi-test:testStackExperience"), BattleHex(leftHex), 100);
	auto * defender = addStack(BattleSide::DEFENDER, creatureByName("vcmi-test:testStackExperience"), BattleHex(rightHex), 100);
	forceMaximumDamage(attacker);
	forceMaximumDamage(defender);
	const auto defenseType = BonusTypeID(BonusTypeID::decode("cultivation:DOMAIN_DEFENSE")).toEnum();
	EXPECT_EQ(body->valOfBonuses(defenseType), 20 + 5 * rank);
	EXPECT_EQ(defender->valOfBonuses(defenseType), 0);
	BattleAttackInfo normal(defender, attacker, 0, false);
	EXPECT_EQ(battle()->calculateDmgRange(normal).damage.max, 1000);
	normal.isCounter = true;
	EXPECT_EQ(battle()->calculateDmgRange(normal).damage.max, 1000 + 100 * rank);
	beginCombat();
	DamageEstimation retaliation;
	const auto expected = battle()->battleEstimateDamage(BattleAttackInfo(attacker, defender, 0, false), &retaliation);
	EXPECT_EQ(expected.damage.max, 800 - 50 * rank);
	const auto attackerHealth = attacker->getAvailableHealth();
	const auto defenderHealth = defender->getAvailableHealth();
	ASSERT_TRUE(attack(attacker, defender->getPosition()));
	EXPECT_EQ(defenderHealth - defender->getAvailableHealth(), expected.damage.max);
	EXPECT_EQ(attackerHealth - attacker->getAvailableHealth(), retaliation.damage.max);
	const auto boosted = battle()->calculateDmgRange(normal).damage.max;
	body->setPosition(BattleHex(rightHex + 6));
	const auto outside = battle()->calculateDmgRange(normal).damage.max;
	EXPECT_LT(outside, boosted);
	body->setPosition(BattleHex(rightHex + 2));
	makeClone(body);
	EXPECT_EQ(battle()->calculateDmgRange(normal).damage.max, outside);
}

INSTANTIATE_TEST_SUITE_P(ThreeRanks, CultivationDiamondTest, ::testing::Values(1, 2, 3));

class CultivationGuardArtTest : public BattleTestFixture, public ::testing::WithParamInterface<int> {};

TEST_P(CultivationGuardArtTest, GuardsProtectOnlyBodyAndBonusVanishesWhenSeparated)
{
	startGame();
	const auto rank = GetParam();
	defenderSideHero->setCommanderType(creatureByName("cultivation:bodyQi"));
	defenderSideHero->setSecSkillLevel(skillByName("cultivation:guardArt"), rank, ChangeValueMode::ABSOLUTE);
	startBattle();
	auto bodies = battle()->getStacksIf([](const CStack * unit) { return unit->unitSlot() == SlotID::COMMANDER_SLOT_PLACEHOLDER; });
	ASSERT_EQ(bodies.size(), 1);
	auto * body = battle()->getStack(bodies.front()->unitId());
	body->setPosition(BattleHex(rightHex));
	auto * attacker = addStack(BattleSide::ATTACKER, creatureByName("vcmi-test:testStackExperience"), BattleHex(leftHex), 10);
	auto damage = [&]() { return battle()->calculateDmgRange(BattleAttackInfo(attacker, body, 0, false)).damage.max; };
	const auto alone = damage();
	auto * guard = addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(rightHex + 1), 100);
	const auto type = BonusTypeID(BonusTypeID::decode("cultivation:GUARD_REDUCTION")).toEnum();
	EXPECT_EQ(body->valOfBonuses(type), 10 + 2 * rank);
	EXPECT_EQ(guard->valOfBonuses(type), 0);
	EXPECT_EQ(damage(), alone * (90 - 2 * rank) / 100);
	guard->setPosition(BattleHex(rightHex + 3));
	EXPECT_EQ(damage(), alone);
}

INSTANTIATE_TEST_SUITE_P(ThreeRanks, CultivationGuardArtTest, ::testing::Values(1, 2, 3));

class CultivationRankedTechniqueTest : public BattleTestFixture, public ::testing::WithParamInterface<int> {};

TEST_P(CultivationRankedTechniqueTest, SkillGrantsRankedShockAndOneRoundStun)
{
	startGame();
	const auto rank = GetParam();
	const auto spell = spellByName("cultivation:groundShock");
	attackerSideHero->setCommanderType(creatureByName("cultivation:bodyQi"));
	attackerSideHero->setSecSkillLevel(skillByName("cultivation:earthShakingArt"), rank, ChangeValueMode::ABSOLUTE);
	teachSpell(attackerSideHero, SpellID::MAGIC_ARROW);
	ASSERT_TRUE(attackerSideHero->canCastThisSpell(spell.toSpell()));
	EXPECT_EQ(attackerSideHero->getSpellSchoolLevel(spell.toSpell()), rank);
	startBattle();
	auto * center = addStack(BattleSide::DEFENDER, creatureByName("cultivation:disciple"), BattleHex(rightHex), 100);
	auto * adjacent = addStack(BattleSide::DEFENDER, creatureByName("cultivation:disciple"), BattleHex(rightHex + 1), 100);
	auto * outer = addStack(BattleSide::DEFENDER, creatureByName("cultivation:disciple"), BattleHex(rightHex + 2), 100);
	auto * ally = addStack(BattleSide::ATTACKER, creatureByName("cultivation:disciple"), BattleHex(leftHex), 100);
	beginCombat();
	const auto health = center->getAvailableHealth();
	const auto mana = attackerSideHero->mana;
	ASSERT_TRUE(castAsHero(attackerSideHero, spell, center));
	EXPECT_EQ(health - center->getAvailableHealth(), 5 + 10 * rank);
	EXPECT_EQ(center->getAvailableHealth(), adjacent->getAvailableHealth());
	EXPECT_EQ(health - outer->getAvailableHealth(), rank == 3 ? 35 : 0);
	EXPECT_EQ(ally->getAvailableHealth(), health);
	EXPECT_EQ(attackerSideHero->mana, mana - (6 + 2 * rank));
	EXPECT_FALSE(center->canMove());
	EXPECT_FALSE(adjacent->canMove());
	EXPECT_EQ(outer->canMove(), rank != 3);
	EXPECT_TRUE(ally->canMove());
	endRound();
	EXPECT_TRUE(center->canMove());
	EXPECT_FALSE(castOn(attackerSideHero, spell, center));
	endRound();
	EXPECT_TRUE(castAsHero(attackerSideHero, spell, center));
}

INSTANTIATE_TEST_SUITE_P(ThreeRanks, CultivationRankedTechniqueTest, ::testing::Values(1, 2, 3));

TEST_F(CultivationTechniqueTest, KnowingShockWithoutItsCultivationSkillIsNotEnough)
{
	startGame();
	const auto spell = spellByName("cultivation:groundShock");
	attackerSideHero->setCommanderType(creatureByName("cultivation:bodyQi"));
	teachSpell(attackerSideHero, spell);
	startBattle();
	auto * target = addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(rightHex), 100);
	beginCombat();
	EXPECT_FALSE(castAsHero(attackerSideHero, spell, target));
}

TEST_F(CultivationTechniqueTest, SwordStormHitsAllEnemiesAndRespectsCooldown)
{
	startGame();
	const auto spell = spellByName("cultivation:swordStorm");
	attackerSideHero->setCommanderType(creatureByName("cultivation:swordCore"));
	teachSpell(attackerSideHero, spell);
	attackerSideHero->setPrimarySkill(PrimarySkill::SPELL_POWER, 2, ChangeValueMode::ABSOLUTE);
	startBattle();
	auto * first = addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(rightHex), 100);
	auto * second = addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(rightHex + 2), 100);
	auto * ally = addStack(BattleSide::ATTACKER, CreatureID(0), BattleHex(leftHex - 2), 100);
	beginCombat();
	const auto health = first->getAvailableHealth();
	const auto allyHealth = ally->getAvailableHealth();
	const auto mana = attackerSideHero->mana;
	ASSERT_TRUE(castAsHero(attackerSideHero, spell, first));
	EXPECT_EQ(health - first->getAvailableHealth(), 125);
	EXPECT_EQ(first->getAvailableHealth(), second->getAvailableHealth());
	EXPECT_EQ(ally->getAvailableHealth(), allyHealth);
	EXPECT_EQ(attackerSideHero->mana, mana - 16);
	for(int i = 0; i < 2; ++i)
	{
		endRound();
		EXPECT_FALSE(castOn(attackerSideHero, spell, first));
		EXPECT_FALSE(castAsHero(attackerSideHero, spell, first));
		EXPECT_EQ(attackerSideHero->mana, mana - 16);
	}
	endRound();
	ASSERT_TRUE(castAsHero(attackerSideHero, spell, first));
	EXPECT_EQ(attackerSideHero->mana, mana - 32);
}

class CultivationTechniqueUnlockTest : public BattleTestFixture, public ::testing::WithParamInterface<std::string> {};

TEST_P(CultivationTechniqueUnlockTest, UnqualifiedCasterCannotSpendManaOrDealDamage)
{
	startGame();
	const auto spell = spellByName("cultivation:swordStorm");
	const auto body = GetParam();
	if(!body.empty())
		attackerSideHero->setCommanderType(creatureByName(body));
	teachSpell(attackerSideHero, spell);
	startBattle();
	auto * target = addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(rightHex), 100);
	beginCombat();
	const auto mana = attackerSideHero->mana;
	const auto health = target->getAvailableHealth();
	EXPECT_FALSE(castOn(attackerSideHero, spell, target));
	EXPECT_FALSE(castAsHero(attackerSideHero, spell, target));
	EXPECT_EQ(attackerSideHero->mana, mana);
	EXPECT_EQ(target->getAvailableHealth(), health);
}

INSTANTIATE_TEST_SUITE_P(RequiredBody, CultivationTechniqueUnlockTest,
	::testing::Values("", "cultivation:swordQi", "cultivation:swordFoundation", "cultivation:bodyCore"));

TEST_F(CultivationTechniqueTest, InsufficientManaDoesNotStartCooldown)
{
	startGame();
	const auto spell = spellByName("cultivation:swordStorm");
	attackerSideHero->setCommanderType(creatureByName("cultivation:swordCore"));
	teachSpell(attackerSideHero, spell);
	startBattle();
	auto * target = addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(rightHex), 100);
	beginCombat();
	attackerSideHero->mana = 15;
	EXPECT_FALSE(castAsHero(attackerSideHero, spell, target));
	EXPECT_EQ(attackerSideHero->mana, 15);
	attackerSideHero->mana = 16;
	EXPECT_TRUE(castAsHero(attackerSideHero, spell, target));
	EXPECT_EQ(attackerSideHero->mana, 0);
}

TEST_F(CultivationInteractionTest, SpellDamageCarriesHeroSource)
{
	startGame();
	teachSpell(attackerSideHero, SpellID::MAGIC_ARROW);
	startBattle();
	auto * enemy = addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(rightHex), 100);
	beginCombat();
	ASSERT_TRUE(castOn(attackerSideHero, SpellID::MAGIC_ARROW, enemy));
	bool found = false;
	for(const auto & pack : server.injuries)
		for(const auto & hit : pack.stacks)
		{
			found = true;
			EXPECT_EQ(hit.stackAttacked, enemy->unitId());
			EXPECT_EQ(hit.sourceSide, BattleSide::ATTACKER);
			EXPECT_TRUE(hit.sourceHero);
			EXPECT_EQ(hit.attackerID, static_cast<ui32>(-1));
			EXPECT_EQ(hit.spellID, SpellID::MAGIC_ARROW);
		}
	EXPECT_TRUE(found);
}

TEST_F(CultivationInteractionTest, FireShieldDamageCarriesReflectingUnitSource)
{
	startGame();
	startBattle();
	auto * attacker = addStack(BattleSide::ATTACKER, CreatureID(0), BattleHex(leftHex), 100);
	auto * defender = addStack(BattleSide::DEFENDER, creatureByName("core:efreetSultan"), BattleHex(rightHex), 10);
	forceMaximumDamage(attacker);
	blockRetaliation(attacker);
	beginCombat();
	ASSERT_TRUE(attack(attacker, defender->getPosition()));
	bool found = false;
	for(const auto & pack : server.injuries)
		for(const auto & hit : pack.stacks)
			if(hit.stackAttacked == attacker->unitId())
			{
				found = true;
				EXPECT_EQ(hit.attackerID, defender->unitId());
				EXPECT_EQ(hit.sourceSide, BattleSide::DEFENDER);
				EXPECT_FALSE(hit.sourceHero);
			}
	EXPECT_TRUE(found);
}

class CultivationDomainBoundaryTest : public BattleTestFixture, public ::testing::WithParamInterface<std::tuple<int, int, int>> {};

TEST_P(CultivationDomainBoundaryTest, RadiusUsesNativeHexGeometry)
{
	startGame();
	startBattle();
	const auto [row, direction, steps] = GetParam();
	const BattleHex origin(row * GameConstants::BFIELD_WIDTH + 8);
	auto destination = origin;
	for(int i = 0; i < steps; ++i)
		destination.moveInDirection(static_cast<BattleHex::EDir>(direction));
	ASSERT_EQ(BattleHex::getDistance(origin, destination), steps);
	addStack(BattleSide::ATTACKER, creatureByName("cultivation:swordSoul"), origin, 1);
	auto * enemy = addStack(BattleSide::DEFENDER, creatureByName("cultivation:elder"), destination, 10);
	const auto health = enemy->getAvailableHealth();
	beginCombat();
	EXPECT_EQ(health - enemy->getAvailableHealth(), steps <= 3 ? 30 : 0);
}

INSTANTIATE_TEST_SUITE_P(HexDirections, CultivationDomainBoundaryTest,
	::testing::Combine(::testing::Values(4, 5), ::testing::Range(0, 6), ::testing::Values(3, 4)));

TEST_F(CultivationInteractionTest, RealmMultipliesExistingAttackModifiers)
{
	startGame();
	startBattle();
	auto * attacker = addStack(BattleSide::ATTACKER, CreatureID(0), BattleHex(leftHex), 10);
	auto * defender = addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(rightHex), 100);
	forceMaximumDamage(attacker);
	attacker->addNewBonus(std::make_shared<Bonus>(BonusDuration::PERMANENT, BonusType::PRIMARY_SKILL, BonusSource::OTHER, 20, BonusSourceID(), BonusSubtypeID(PrimarySkill::ATTACK)));
	const auto before = battle()->calculateDmgRange(BattleAttackInfo(attacker, defender, 0, false));
	realm(attacker, 2);
	realm(defender, 1);
	const auto after = battle()->calculateDmgRange(BattleAttackInfo(attacker, defender, 0, false));
	EXPECT_EQ(after.damage.max, before.damage.max * 3 / 2);
}

TEST_F(CultivationInteractionTest, CreatureConfigurationCarriesRealmAndTuning)
{
	startGame();
	startBattle();
	auto * unit = addStack(BattleSide::ATTACKER, creatureByName("cultivation:swordSoul"), BattleHex(leftHex), 1);
	auto value = [unit](const std::string & name)
	{
		return unit->valOfBonuses(BonusTypeID(BonusTypeID::decode("cultivation:" + name)).toEnum());
	};
	EXPECT_EQ(value("REALM"), 4);
	EXPECT_EQ(value("REALM_SUPPRESSION_STEP"), 50);
	EXPECT_EQ(value("REALM_SUPPRESSION_CAP"), 3);
}

TEST_F(CultivationInteractionTest, GuardsStackAndExcludeClonesAndDeadUnits)
{
	startGame();
	startBattle();
	auto * attacker = addStack(BattleSide::ATTACKER, creatureByName("vcmi-test:testStackExperience"), BattleHex(leftHex), 10);
	auto * defender = addStack(BattleSide::DEFENDER, creatureByName("vcmi-test:testStackExperience"), BattleHex(rightHex), 100);
	grant(defender, "GUARD_REDUCTION", 10);
	grant(defender, "GUARD_CAP", 3);
	auto damage = [&]() { return battle()->calculateDmgRange(BattleAttackInfo(attacker, defender, 0, false)).damage.max; };
	EXPECT_EQ(damage(), 100);
	auto * guard1 = addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(4 * GameConstants::BFIELD_WIDTH + 8), 1);
	EXPECT_EQ(damage(), 90);
	auto * guard2 = addStack(BattleSide::DEFENDER, CreatureID(0), BattleHex(6 * GameConstants::BFIELD_WIDTH + 8), 1);
	EXPECT_EQ(damage(), 80);
	makeClone(guard2);
	EXPECT_EQ(damage(), 90);
	injure(guard1, guard1->getAvailableHealth());
	EXPECT_EQ(damage(), 100);
}

TEST_F(CultivationInteractionTest, SwordDomainTicksAndStopsWhenSourceDies)
{
	startGame();
	startBattle();
	auto * body = addStack(BattleSide::ATTACKER, creatureByName("cultivation:swordSoul"), BattleHex(leftHex), 1);
	auto * enemy = addStack(BattleSide::DEFENDER, creatureByName("cultivation:elder"), BattleHex(rightHex), 10);
	const auto health = enemy->getAvailableHealth();
	beginCombat();
	EXPECT_EQ(health - enemy->getAvailableHealth(), 30);
	endRound();
	EXPECT_EQ(health - enemy->getAvailableHealth(), 60);
	injure(body, body->getAvailableHealth());
	gameHandler->battles->processBattleEventTriggers(*battle(), CombatEventType::ROUND_START, body, nullptr);
	EXPECT_EQ(health - enemy->getAvailableHealth(), 60);
}

TEST_F(CultivationInteractionTest, EqualRealmDomainsCancelUntilOneSourceDies)
{
	startGame();
	startBattle();
	addStack(BattleSide::ATTACKER, creatureByName("cultivation:swordSoul"), BattleHex(leftHex), 1);
	auto * body = addStack(BattleSide::DEFENDER, creatureByName("cultivation:bodySoul"), BattleHex(rightHex), 1);
	auto * enemy = addStack(BattleSide::DEFENDER, creatureByName("cultivation:elder"), BattleHex(rightHex + 1), 10);
	const auto health = enemy->getAvailableHealth();
	beginCombat();
	EXPECT_EQ(health, enemy->getAvailableHealth());
	injure(body, body->getAvailableHealth());
	endRound();
	EXPECT_EQ(health - enemy->getAvailableHealth(), 30);
}

TEST_F(CultivationInteractionTest, FormationSuppressesDomain)
{
	startGame();
	startBattle();
	addStack(BattleSide::ATTACKER, creatureByName("cultivation:swordSoul"), BattleHex(leftHex), 1);
	auto * enemy = addStack(BattleSide::DEFENDER, creatureByName("cultivation:elder"), BattleHex(rightHex), 10);
	grant(enemy, "DOMAIN_SUPPRESSOR", 3);
	const auto health = enemy->getAvailableHealth();
	beginCombat();
	endRound();
	EXPECT_EQ(health, enemy->getAvailableHealth());
}

TEST_F(CultivationInteractionTest, FinalBodyDeathPenalizesArmyMorale)
{
	startGame();
	startBattle();
	auto * killer = addStack(BattleSide::ATTACKER, creatureByName("vcmi-test:testStackExperience"), BattleHex(leftHex), 1000);
	auto * body = addStack(BattleSide::DEFENDER, creatureByName("cultivation:swordQi"), BattleHex(rightHex), 1);
	auto * army = addStack(BattleSide::DEFENDER, creatureByName("cultivation:disciple"), BattleHex(rightHex + 2), 10);
	const auto morale = army->valOfBonuses(BonusType::MORALE);
	beginCombat();
	ASSERT_TRUE(attack(killer, body->getPosition()));
	EXPECT_FALSE(body->alive());
	EXPECT_EQ(army->valOfBonuses(BonusType::MORALE), morale - 2);
	EXPECT_TRUE(server.battleResults.empty());
}

TEST_F(CultivationInteractionTest, ResurrectionBudgetDoesNotResetAfterUnitsDieAgain)
{
	startGame();
	teachSpell(attackerSideHero, SpellID::RESURRECTION);
	attackerSideHero->setPrimarySkill(PrimarySkill::SPELL_POWER, 30, ChangeValueMode::ABSOLUTE);
	attackerSideHero->setCreature(SlotID(0), creatureByName("cultivation:disciple"), 20);
	startBattle();
	auto * body = addStack(BattleSide::ATTACKER, creatureByName("cultivation:swordQi"), BattleHex(leftHex), 1);
	auto armies = battle()->getStacksIf([](const CStack * stack)
	{
		return stack->unitSide() == BattleSide::ATTACKER && stack->unitSlot() == SlotID(0);
	});
	ASSERT_EQ(armies.size(), 1);
	auto * army = armies.front();
	beginCombat();
	const auto budgetType = BonusTypeID(BonusTypeID::decode("cultivation:RESURRECTION_BUDGET")).toEnum();
	const auto usedType = BonusTypeID(BonusTypeID::decode("cultivation:RESURRECTION_USED")).toEnum();
	EXPECT_EQ(body->valOfBonuses(budgetType), 4);
	injure(army, 180);
	ASSERT_EQ(army->getCount(), 10);
	ASSERT_TRUE(castOn(attackerSideHero, SpellID::RESURRECTION, army));
	EXPECT_EQ(army->getCount(), 14);
	EXPECT_EQ(body->valOfBonuses(usedType), 4);
	injure(army, 72);
	endRound();
	EXPECT_FALSE(castOn(attackerSideHero, SpellID::RESURRECTION, army));
	EXPECT_EQ(army->getCount(), 10);
	EXPECT_EQ(body->valOfBonuses(usedType), 4);
}

class CultivationResurrectionBudgetTest : public BattleTestFixture, public ::testing::WithParamInterface<int> {};

TEST_P(CultivationResurrectionBudgetTest, OnlyPreBattleTroopsProvideQuota)
{
	startGame();
	const auto count = GetParam();
	attackerSideHero->setCreature(SlotID(0), creatureByName("cultivation:disciple"), count);
	giveArtifact(attackerSideHero, ArtifactID::BALLISTA, ArtifactPosition::MACH1);
	giveArtifact(attackerSideHero, ArtifactID::AMMO_CART, ArtifactPosition::MACH2);
	giveArtifact(attackerSideHero, ArtifactID::FIRST_AID_TENT, ArtifactPosition::MACH3);
	startBattle();
	const auto machines = battle()->getStacksIf([](const CStack * stack)
	{
		return stack->unitSide() == BattleSide::ATTACKER && stack->hasBonusOfType(BonusType::SIEGE_WEAPON);
	});
	ASSERT_EQ(machines.size(), 3);
	auto * body = addStack(BattleSide::ATTACKER, creatureByName("cultivation:swordQi"), BattleHex(leftHex), 1);
	addStack(BattleSide::ATTACKER, creatureByName("cultivation:disciple"), BattleHex(leftHex - 2), 100);
	auto * clone = addStack(BattleSide::ATTACKER, creatureByName("cultivation:disciple"), BattleHex(leftHex - 3), 100);
	makeClone(clone);
	beginCombat();
	const auto budgetType = BonusTypeID(BonusTypeID::decode("cultivation:RESURRECTION_BUDGET")).toEnum();
	EXPECT_EQ(body->valOfBonuses(budgetType), count / 5);
	addStack(BattleSide::ATTACKER, creatureByName("cultivation:disciple"), BattleHex(leftHex + 2), 100);
	gameHandler->battles->processBattleEventTriggers(*battle(), CombatEventType::BATTLE_SETUP, body, nullptr);
	EXPECT_EQ(body->valOfBonuses(budgetType), count / 5);
}

INSTANTIATE_TEST_SUITE_P(InitialTroopCounts, CultivationResurrectionBudgetTest, ::testing::Values(4, 9, 10));

TEST_F(CultivationInteractionTest, ExhaustedResurrectionQuotaStillAllowsHealingSurvivors)
{
	startGame();
	teachSpell(attackerSideHero, SpellID::RESURRECTION);
	attackerSideHero->setPrimarySkill(PrimarySkill::SPELL_POWER, 30, ChangeValueMode::ABSOLUTE);
	attackerSideHero->setCreature(SlotID(0), creatureByName("cultivation:disciple"), 4);
	startBattle();
	auto * body = addStack(BattleSide::ATTACKER, creatureByName("cultivation:swordQi"), BattleHex(leftHex), 1);
	auto armies = battle()->getStacksIf([](const CStack * stack)
	{
		return stack->unitSide() == BattleSide::ATTACKER && stack->unitSlot() == SlotID(0);
	});
	ASSERT_EQ(armies.size(), 1);
	auto * army = armies.front();
	beginCombat();
	const auto maximumHealth = army->getMaxHealth();
	injure(army, maximumHealth + 1);
	ASSERT_EQ(army->getCount(), 3);
	ASSERT_TRUE(castOn(attackerSideHero, SpellID::RESURRECTION, army));
	EXPECT_EQ(army->getCount(), 3);
	EXPECT_EQ(army->getAvailableHealth(), 3 * maximumHealth);
	const auto usedType = BonusTypeID(BonusTypeID::decode("cultivation:RESURRECTION_USED")).toEnum();
	EXPECT_EQ(body->valOfBonuses(usedType), 0);
}

TEST_F(CultivationInteractionTest, BodyRebirthPrecedesEscapeAndOnlyWorksOnce)
{
	startGame();
	startBattle();
	auto * killer = addStack(BattleSide::ATTACKER, creatureByName("vcmi-test:testStackExperience"), BattleHex(leftHex), 1000);
	auto * body = addStack(BattleSide::DEFENDER, creatureByName("cultivation:bodyCore"), BattleHex(rightHex), 1);
	auto * army = addStack(BattleSide::DEFENDER, creatureByName("cultivation:disciple"), BattleHex(rightHex + 2), 10);
	const auto morale = army->valOfBonuses(BonusType::MORALE);
	beginCombat();
	ASSERT_TRUE(attack(killer, body->getPosition()));
	EXPECT_TRUE(body->alive());
	EXPECT_EQ(army->valOfBonuses(BonusType::MORALE), morale);
	ASSERT_TRUE(attack(killer, body->getPosition()));
	EXPECT_FALSE(body->alive());
	EXPECT_EQ(army->valOfBonuses(BonusType::MORALE), morale - 2);
}
