/*
 * CHeroWindow.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "CHeroWindow.h"
#include "wiki/WikiWindow.h"

#include "CCreatureWindow.h"
#include "CHeroBackpackWindow.h"
#include "CKingdomInterface.h"
#include "CExchangeWindow.h"

#include "../CPlayerInterface.h"

#include "../GameEngine.h"
#include "../GameInstance.h"
#include "../gui/TextAlignment.h"
#include "../gui/Shortcut.h"
#include "../gui/WindowHandler.h"
#include "../widgets/Images.h"
#include "../widgets/MiscWidgets.h"
#include "../widgets/CComponent.h"
#include "../widgets/CGarrisonInt.h"
#include "../widgets/TextControls.h"
#include "../widgets/Buttons.h"
#include "../widgets/Slider.h"
#include "../widgets/WidgetLayout.h"
#include "render/IRenderHandler.h"

#include "../../lib/CConfigHandler.h"
#include "../../lib/CSkillHandler.h"
#include "../../lib/GameLibrary.h"
#include "../../lib/callback/CCallback.h"
#include "../../lib/entities/artifact/ArtifactUtils.h"
#include "../../lib/entities/hero/CHeroHandler.h"
#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/networkPacks/ArtifactLocation.h"
#include "../../lib/texts/CGeneralTextHandler.h"
#include "../../lib/json/JsonNode.h"

void CHeroSwitcher::clickPressed(const Point & cursorPosition)
{
	//TODO: do not recreate window
	if (false)
	{
		owner->updateArtifacts();
	}
	else
	{
		const CGHeroInstance * buf = hero;
		ENGINE->windows().popWindows(1);
		ENGINE->windows().createAndPushWindow<CHeroWindow>(buf);
	}
}

CHeroSwitcher::CHeroSwitcher(CHeroWindow * owner_, Point pos_, const CGHeroInstance * hero_)
	: CIntObject(LCLICK),
	owner(owner_),
	hero(hero_)
{
	OBJECT_CONSTRUCTION;
	pos += pos_;

	image = std::make_shared<CAnimImage>(AnimationPath::builtin("PortraitsSmall"), hero->getIconIndex());
	pos.w = image->pos.w;
	pos.h = image->pos.h;
}

namespace
{
/// 修士窗口布局：模组的 config/widgets/heroWindow.json；没有这份文件时是空节点，下面每一项都取原版值
const JsonNode & heroLayout()
{
	static const JsonNode layout = WidgetLayout::load(JsonPath::builtin("config/widgets/heroWindow.json"));
	return layout;
}

int heroWindowOptions()
{
	const JsonNode & layout = heroLayout();
	if(layout["background"].isNull())
		return CWindowObject::PLAYER_COLORED;
	// 模组底图：不按玩家色染色、不带矩形阴影；"backgroundAlpha": true 时按透明通道贴（毛边窗口）
	return CWindowObject::SHADOW_DISABLED | (layout["backgroundAlpha"].Bool() ? CWindowObject::BACKGROUND_ALPHA : 0);
}

ImagePath heroWindowBackground()
{
	return WidgetLayout::image(heroLayout()["background"], ImagePath::builtin(ENGINE->isRoeData() ? "HeroScr3" : "HeroScr4"));
}

/// 布局里按钮项 {"x", "y", "image"}：位置和动画，缺省用原版
Point buttonPos(const char * key, const Point & fallback)
{
	return WidgetLayout::point(heroLayout()["buttons"][key], fallback);
}

AnimationPath buttonImage(const char * key, const AnimationPath & fallback)
{
	return WidgetLayout::animation(heroLayout()["buttons"][key]["image"], fallback);
}

/// 模组换了按钮图时，原版叠在按钮上的小图标（储物袋、统领）不再叠
bool keepButtonOverlay(const char * key)
{
	return heroLayout()["buttons"][key]["image"].isNull();
}
}

CHeroWindow::CHeroWindow(const CGHeroInstance * hero)
	: CWindowObject(heroWindowOptions(), heroWindowBackground())
{

	OBJECT_CONSTRUCTION;
	curHero = hero;
	const JsonNode & layout = heroLayout();

	if(!(layout["banner"].isBool() && !layout["banner"].Bool()))
	{
		const Point bannerPos = WidgetLayout::point(layout["banner"], Point(606, 8));
		banner = std::make_shared<CAnimImage>(AnimationPath::builtin("CREST58"), GAME->interface()->playerID.getNum(), 0, bannerPos.x, bannerPos.y);
	}
	name = WidgetLayout::label(layout["name"], Point(190, 38), EFonts::FONT_BIG, ETextAlignment::CENTER, Colors::YELLOW);
	title = WidgetLayout::label(layout["title"], Point(190, 65), EFonts::FONT_MEDIUM, ETextAlignment::CENTER, Colors::WHITE);

	const Rect statusRect = WidgetLayout::rect(layout["statusBar"], Rect(7, 559, 660, 19));
	statusbar = CGStatusBar::create(std::make_shared<CPicture>(background->getSurface(), statusRect, statusRect.x, statusRect.y));

	quitButton = std::make_shared<CButton>(buttonPos("quit", Point(609, 516)), buttonImage("quit", AnimationPath::builtin("hsbtns.def")), CButton::tooltip(LIBRARY->generaltexth->translate("core.heroscrn.17")), [this](){ close(); }, EShortcut::GLOBAL_RETURN);

	if(settings["general"]["enableUiEnhancements"].Bool())
	{
		questlogButton = std::make_shared<CButton>(buttonPos("questLog", Point(314, 429)), buttonImage("questLog", AnimationPath::builtin("hsbtns4.def")), CButton::tooltip(LIBRARY->generaltexth->translate("core.heroscrn.0")), [](){ GAME->interface()->showQuestLog(); }, EShortcut::ADVENTURE_QUEST_LOG);
		backpackButton = std::make_shared<CButton>(buttonPos("backpack", Point(424, 429)), buttonImage("backpack", AnimationPath::builtin("heroBackpack")), CButton::tooltipLocalized("vcmi.heroWindow.openBackpack"), [this](){ createBackpackWindow(); }, EShortcut::HERO_BACKPACK);
		if(keepButtonOverlay("backpack"))
			backpackButton->setOverlay(std::make_shared<CPicture>(ImagePath::builtin("heroWindow/backpackButtonIcon")));
		dismissButton = std::make_shared<CButton>(buttonPos("dismiss", Point(534, 429)), buttonImage("dismiss", AnimationPath::builtin("hsbtns2.def")), CButton::tooltip(LIBRARY->generaltexth->translate("core.heroscrn.28")), [this](){ dismissCurrent(); }, EShortcut::HERO_DISMISS);
	}
	else
	{
		dismissLabel = std::make_shared<CTextBox>(LIBRARY->generaltexth->translate("core.jktext.8"), Rect(370, 430, 65, 35), 0, FONT_SMALL, ETextAlignment::TOPLEFT, Colors::WHITE);
		questlogLabel = std::make_shared<CTextBox>(LIBRARY->generaltexth->translate("core.jktext.9"), Rect(510, 430, 65, 35), 0, FONT_SMALL, ETextAlignment::TOPLEFT, Colors::WHITE);
		dismissButton = std::make_shared<CButton>(Point(454, 429), AnimationPath::builtin("hsbtns2.def"), CButton::tooltip(LIBRARY->generaltexth->translate("core.heroscrn.28")), [this](){ dismissCurrent(); }, EShortcut::HERO_DISMISS);
		questlogButton = std::make_shared<CButton>(Point(314, 429), AnimationPath::builtin("hsbtns4.def"), CButton::tooltip(LIBRARY->generaltexth->translate("core.heroscrn.0")), [](){ GAME->interface()->showQuestLog(); }, EShortcut::ADVENTURE_QUEST_LOG);
	}
	questlogButton->block(!GAME->interface()->hasJournalEntries());

	formations = std::make_shared<CToggleGroup>(0);
	formations->addToggle(0, std::make_shared<CToggleButton>(buttonPos("formation0", Point(481, 483)), buttonImage("formation0", AnimationPath::builtin("hsbtns6.def")), std::make_pair(LIBRARY->generaltexth->translate("core.heroscrn.23"), LIBRARY->generaltexth->translate("core.heroscrn.29")), 0, EShortcut::HERO_TIGHT_FORMATION));
	formations->addToggle(1, std::make_shared<CToggleButton>(buttonPos("formation1", Point(481, 519)), buttonImage("formation1", AnimationPath::builtin("hsbtns7.def")), std::make_pair(LIBRARY->generaltexth->translate("core.heroscrn.24"), LIBRARY->generaltexth->translate("core.heroscrn.30")), 0, EShortcut::HERO_LOOSE_FORMATION));

	if(hero->getCommander())
	{
		commanderButton = std::make_shared<CButton>(buttonPos("commander", Point(317, 18)), buttonImage("commander", AnimationPath::builtin("heroCommander")), CButton::tooltipLocalized("vcmi.heroWindow.openCommander"), [&](){ commanderWindow(); }, EShortcut::HERO_COMMANDER);
		if(keepButtonOverlay("commander"))
			commanderButton->setOverlay(std::make_shared<CPicture>(ImagePath::builtin("heroWindow/commanderButtonIcon")));
	}

	//right list of heroes（布局 "heroList"：起点、步长、最多几个）
	const JsonNode & listLayout = layout["heroList"];
	const Point listOrigin = WidgetLayout::point(listLayout, Point(612, 87));
	const Point listStep = WidgetLayout::point(listLayout["step"], Point(0, 54));
	const int listMax = listLayout["max"].isNull() ? 8 : listLayout["max"].Integer();
	for(int i=0; i < std::min<int>(GAME->interface()->cb->howManyHeroes(false), listMax); i++)
		heroList.push_back(std::make_shared<CHeroSwitcher>(this, listOrigin + listStep * i, GAME->interface()->cb->getHeroBySerial(i, false)));

	//areas
	const Point portraitPos = WidgetLayout::point(layout["portrait"], Point(18, 18));
	portraitArea = std::make_shared<LRClickableAreaWText>(Rect(portraitPos, Point(58, 64)));
	portraitImage = std::make_shared<CAnimImage>(AnimationPath::builtin("PortraitsLarge"), 0, 0, portraitPos.x + 1, portraitPos.y + 1);

	portraitWikiArea = std::make_shared<LRClickableArea>(Rect(portraitPos, Point(58, 64)), [this]()
	{
		ENGINE->windows().createAndPushWindow<WikiWindow>(
			WikiWindow::Style::BROWN,
			WikiEntryKey{WikiCategory::HERO, curHero->getHeroType()->getJsonKey()});
	});

	// 主属性：布局 "primarySkills" 是 4 项 {"area": {x,y,w,h}, "value": 标签}
	const JsonNode & primLayout = layout["primarySkills"];
	for(int v = 0; v < GameConstants::PRIMARY_SKILLS; ++v)
	{
		const JsonNode & primNode = primLayout.isVector() && v < primLayout.Vector().size() ? primLayout.Vector()[v] : JsonNode();
		auto area = std::make_shared<LRClickableAreaWTextComp>(WidgetLayout::rect(primNode["area"], Rect(30 + 70 * v, 109, 42, 64)), ComponentType::PRIM_SKILL);
		area->text = GAME->translator().translate("core.arraytxt", 2+v);
		area->component.subType = PrimarySkill(v);
		MetaString hoverText;
		hoverText.appendTextID("core.heroscrn.1");
		hoverText.replaceTextID("core.priskill", v);
		area->hoverText = hoverText.toString(&GAME->translator());
		primSkillAreas.push_back(area);

		auto value = WidgetLayout::label(primNode["value"], Point(53 + 70 * v, 166), FONT_SMALL, ETextAlignment::CENTER, Colors::WHITE);
		primSkillValues.push_back(value);
	}

	// 主属性、修为、灵力的图标：布局 "skillImages": {"image": 动画, "positions": [6 个 {x,y}]}（按 PSKIL42 的帧序）
	const JsonNode & skillImages = layout["skillImages"];
	const AnimationPath skillAnim = WidgetLayout::animation(skillImages["image"], AnimationPath::builtin("PSKIL42"));
	const std::array<Point, 6> skillImagePos = {Point(32, 111), Point(102, 111), Point(172, 111), Point(162, 230), Point(20, 230), Point(242, 111)};
	for(int frame = 0; frame < skillImagePos.size(); ++frame)
	{
		const JsonNode & posNode = skillImages["positions"].isVector() && frame < skillImages["positions"].Vector().size() ? skillImages["positions"].Vector()[frame] : JsonNode();
		const Point at = WidgetLayout::point(posNode, skillImagePos[frame]);
		primSkillImages.push_back(std::make_shared<CAnimImage>(skillAnim, frame, 0, at.x, at.y));
	}

	const JsonNode & specLayout = layout["specialty"];
	const Point specPos = WidgetLayout::point(specLayout["image"], Point(18, 180));
	specImage = std::make_shared<CAnimImage>(AnimationPath::builtin("UN44"), 0, 0, specPos.x, specPos.y);
	specArea = std::make_shared<LRClickableAreaWText>(WidgetLayout::rect(specLayout["area"], Rect(18, 180, 136, 42)), LIBRARY->generaltexth->translate("core.heroscrn.27"));
	specName = WidgetLayout::label(specLayout["name"], Point(69, 205), FONT_SMALL, ETextAlignment::TOPLEFT, Colors::WHITE);

	expArea = std::make_shared<LRClickableAreaWText>(WidgetLayout::rect(layout["experience"]["area"], Rect(18, 228, 136, 42)), LIBRARY->generaltexth->translate("core.heroscrn.9"));
	morale = std::make_shared<MoraleLuckBox>(true, WidgetLayout::rect(layout["morale"], Rect(175, 179, 53, 45)));
	luck = std::make_shared<MoraleLuckBox>(false, WidgetLayout::rect(layout["luck"], Rect(233, 179, 53, 45)));
	spellPointsArea = std::make_shared<LRClickableAreaWText>(WidgetLayout::rect(layout["mana"]["area"], Rect(162,228, 136, 42)), LIBRARY->generaltexth->translate("core.heroscrn.22"));

	expValue = WidgetLayout::label(layout["experience"]["value"], Point(68, 252), FONT_SMALL, ETextAlignment::TOPLEFT, Colors::WHITE);
	manaValue = WidgetLayout::label(layout["mana"]["value"], Point(211, 252), FONT_SMALL, ETextAlignment::TOPLEFT, Colors::WHITE);

	// 功法（二级技能）：布局 "secondarySkills": {"slots": [8 个 {x,y}], "value": {dx,dy}, "name": {dx,dy}, "width", "color", "slider": {x,y,length}}
	const JsonNode & secLayout = layout["secondarySkills"];
	if(hero->secSkills.size() > 8)
	{
		auto divisionRoundUp = [](int x, int y){ return (x + (y - 1)) / y; };
		int lines = divisionRoundUp(hero->secSkills.size(), 2);
		const Point sliderPos = WidgetLayout::point(secLayout["slider"], Point(284, 276));
		const int sliderLength = secLayout["slider"]["length"].isNull() ? 189 : secLayout["slider"]["length"].Integer();
		const int sliderReach = secLayout["slider"]["reach"].isNull() ? 266 : secLayout["slider"]["reach"].Integer();
		secSkillSlider = std::make_shared<CSlider>(sliderPos, sliderLength, [this](int val){ CHeroWindow::updateArtifacts(); }, 4, lines, 0, Orientation::VERTICAL, CSlider::BROWN);
		secSkillSlider->setPanningStep(48);
		secSkillSlider->setScrollBounds(Rect(-sliderReach, 0, secSkillSlider->pos.x - pos.x + secSkillSlider->pos.w, secSkillSlider->pos.h));
	}

	const Point secValueOffset = WidgetLayout::point(secLayout["value"], Point(50, 4));
	const Point secNameOffset = WidgetLayout::point(secLayout["name"], Point(50, 24));
	const ColorRGBA secColor = WidgetLayout::color(secLayout["color"], Colors::WHITE);
	for(int i = 0; i < std::min<size_t>(hero->secSkills.size(), 8u); ++i)
	{
		bool isSmallBox = (secSkillSlider && i%2 == 1);
		Point slot(i%2 == 0  ?  18  :  162,  276 + 48 * (i/2));
		if(secLayout["slots"].isVector() && i < secLayout["slots"].Vector().size())
			slot = WidgetLayout::point(secLayout["slots"].Vector()[i], slot);
		secSkills.emplace_back(std::make_shared<CSecSkillPlace>(slot, CSecSkillPlace::ImageSize::MEDIUM));

		int width = secLayout["width"].isNull() ? (isSmallBox ? 71 : 87) : secLayout["width"].Integer();
		const Point valuePos = slot + secValueOffset;
		const Point namePos = slot + secNameOffset;

		secSkillValues.push_back(std::make_shared<CLabel>(valuePos.x, valuePos.y, FONT_SMALL, ETextAlignment::TOPLEFT, secColor, "", width));
		secSkillNames.push_back(std::make_shared<CLabel>(namePos.x, namePos.y, FONT_SMALL, ETextAlignment::TOPLEFT, secColor, "", width));
	}

	// various texts：布局 "labels" 是数组时整组替换（空数组 = 不要这些字，比如字已经画在底图上）
	if(layout["labels"].isVector())
	{
		for(const JsonNode & entry : layout["labels"].Vector())
			labels.push_back(WidgetLayout::label(entry, Point(), FONT_SMALL, ETextAlignment::CENTER, Colors::YELLOW, LIBRARY->generaltexth->translate(entry["text"].String())));
	}
	else
	{
	labels.push_back(std::make_shared<CLabel>(52, 99, FONT_SMALL, ETextAlignment::CENTER, Colors::YELLOW, LIBRARY->generaltexth->translate("core.jktext.1")));
	labels.push_back(std::make_shared<CLabel>(123, 99, FONT_SMALL, ETextAlignment::CENTER, Colors::YELLOW, LIBRARY->generaltexth->translate("core.jktext.2")));
	labels.push_back(std::make_shared<CLabel>(193, 99, FONT_SMALL, ETextAlignment::CENTER, Colors::YELLOW, LIBRARY->generaltexth->translate("core.jktext.3")));
	labels.push_back(std::make_shared<CLabel>(262, 99, FONT_SMALL, ETextAlignment::CENTER, Colors::YELLOW, LIBRARY->generaltexth->translate("core.jktext.4")));

	labels.push_back(std::make_shared<CLabel>(69, 183, FONT_SMALL, ETextAlignment::TOPLEFT, Colors::YELLOW, LIBRARY->generaltexth->translate("core.jktext.5")));
	labels.push_back(std::make_shared<CLabel>(69, 232, FONT_SMALL, ETextAlignment::TOPLEFT, Colors::YELLOW, LIBRARY->generaltexth->translate("core.jktext.6")));
	labels.push_back(std::make_shared<CLabel>(213, 232, FONT_SMALL, ETextAlignment::TOPLEFT, Colors::YELLOW, LIBRARY->generaltexth->translate("core.jktext.7")));
	}

	addUsedEvents(KEYBOARD);
	CHeroWindow::updateArtifacts();
}

void CHeroWindow::keyPressed(EShortcut key)
{
	if(key == EShortcut::ADVENTURE_OPEN_WIKI)
		ENGINE->windows().createAndPushWindow<WikiWindow>(
			WikiWindow::Style::BROWN,
			WikiEntryKey{WikiCategory::HERO, curHero->getHeroType()->getJsonKey()});
}

void CHeroWindow::updateArtifacts()
{
	OBJECT_CONSTRUCTION;

	CWindowWithArtifacts::updateArtifacts();
	assert(curHero);

	name->setText(GAME->translator().translate(curHero->getNameTextID()));
	MetaString titleText;
	titleText.appendTextID("core.genrltxt.342");
	titleText.replaceNumber(curHero->level);
	titleText.replaceTextID(curHero->getClassNameTextID());
	title->setText(titleText.toString(&GAME->translator()));

	specArea->text = curHero->getHeroType()->getSpecialtyDescriptionTranslated();
	specImage->setFrame(curHero->getHeroType()->imageIndex);
	specName->setText(curHero->getHeroType()->getSpecialtyNameTranslated());

	tacticsButton = std::make_shared<CToggleButton>(buttonPos("tactics", Point(539, 483)), buttonImage("tactics", AnimationPath::builtin("hsbtns8.def")), std::make_pair(LIBRARY->generaltexth->translate("core.heroscrn.26"), LIBRARY->generaltexth->translate("core.heroscrn.31")), 0, EShortcut::HERO_TOGGLE_TACTICS);
	tacticsButton->addHoverText(EButtonState::HIGHLIGHTED, LIBRARY->generaltexth->translate("core.heroscrn.25"));
	tacticsButton->setSelectedSilent(curHero->tacticFormationEnabled);

	MetaString dismissText;
	dismissText.appendTextID("core.heroscrn.16");
	dismissText.replaceTextID(curHero->getNameTextID());
	dismissText.replaceTextID(curHero->getClassNameTextID());

	dismissButton->addHoverText(EButtonState::NORMAL, dismissText.toString(&GAME->translator()));
	portraitArea->hoverText = curHero->getObjectName().toString(&GAME->translator());
	portraitArea->text = GAME->translator().translate(curHero->getBiographyTextID());
	portraitImage->setFrame(curHero->getIconIndex());

	{
		if(!garr)
		{
			bool removableTroops = curHero->getOwner() == GAME->interface()->playerID;
			MetaString helpBoxText = MetaString::createFromTextID("core.heroscrn.32");
			helpBoxText.replaceTextID("core.genrltxt.43");
			std::string helpBox = helpBoxText.toString(&GAME->translator());

			const JsonNode & garrLayout = heroLayout()["garrison"];
			const int garrInterval = garrLayout["interval"].isNull() ? 8 : garrLayout["interval"].Integer();
			garr = std::make_shared<CGarrisonInt>(WidgetLayout::point(garrLayout, Point(15, 485)), garrInterval, Point(), curHero, nullptr, removableTroops);
			auto split = std::make_shared<CButton>(buttonPos("split", Point(539, 519)), buttonImage("split", AnimationPath::builtin("hsbtns9.def")), CButton::tooltip(LIBRARY->generaltexth->allTexts[256], helpBox), [this](){ garr->splitClick(); }, EShortcut::HERO_ARMY_SPLIT);
			garr->addSplitBtn(split);
		}
		if(!arts)
		{
			const JsonNode & artsLayout = heroLayout()["artifacts"];
			arts = std::make_shared<CArtifactsOfHeroMain>(WidgetLayout::point(artsLayout, Point(-65, -8)), artsLayout);
			arts->clickPressedCallback = [this](const CArtPlace & artPlace, const Point & cursorPosition){clickPressedOnArtPlace(curHero, artPlace.slot, true, false, false, cursorPosition);};
			arts->showPopupCallback = [this](CArtPlace & artPlace, const Point & cursorPosition){showArtifactPopup(*arts, artPlace, cursorPosition);};
			arts->gestureCallback = [this](const CArtPlace & artPlace, const Point & cursorPosition){showQuickBackpackWindow(curHero, artPlace.slot, cursorPosition);};
			arts->setHero(curHero);
			addSet(arts);
			enableKeyboardShortcuts();
		}

		int serial = GAME->interface()->cb->getHeroSerial(curHero, false);

		listSelection.reset();
		// getHeroSerial 从 1 开始数（未驻守的修士里第几个）
		if(serial >= 1 && serial <= heroList.size())
		{
			// 选中框：布局 "heroList" 的 "selection" 图和相对头像的 "selectionOffset"
			const JsonNode & listLayout = heroLayout()["heroList"];
			const Point origin = WidgetLayout::point(listLayout, Point(612, 87));
			const Point step = WidgetLayout::point(listLayout["step"], Point(0, 54));
			const Point offset = WidgetLayout::point(listLayout["selectionOffset"], Point(0, 0));
			const Point at = origin + step * (serial - 1) + offset;
			listSelection = std::make_shared<CPicture>(WidgetLayout::image(listLayout["selection"], ImagePath::builtin("HPSYYY")), at.x, at.y);
		}
	}

	//primary skills support
	for(size_t g=0; g<primSkillAreas.size(); ++g)
	{
		int value = curHero->getPrimSkillLevel(static_cast<PrimarySkill>(g));
		primSkillAreas[g]->component.value = value;
		primSkillValues[g]->setText(std::to_string(value));
	}

	//secondary skills support
	for(size_t g=0; g < secSkills.size(); ++g)
	{
		int offset = secSkillSlider ? secSkillSlider->getValue() * 2 : 0;
		if(curHero->secSkills.size() < g + offset + 1)
		{
			secSkillNames[g]->setText("");
			secSkillValues[g]->setText("");
			secSkills[g]->setSkill(SecondarySkill::NONE);
			break;
		}
		SecondarySkill skill = curHero->secSkills[g + offset].first;
		int	level = curHero->getSecSkillLevel(skill);
		std::string skillName = skill.toEntity(LIBRARY)->getNameTranslated();
		std::string skillValue = GAME->translator().translate("core.skilllev", level-1);

		secSkillNames[g]->setText(skillName);
		secSkillValues[g]->setText(skillValue);
		secSkills[g]->setSkill(skill, level);
	}

	std::ostringstream expstr;
	expstr << curHero->exp;
	expValue->setText(expstr.str());

	std::ostringstream manastr;
	manastr << curHero->mana << '/' << curHero->manaLimit();
	manaValue->setText(manastr.str());

	expArea->text = curHero->getExperienceDescription().toString(&GAME->translator());

	MetaString spellPointsText;
	spellPointsText.appendTextID("core.genrltxt.205");
	spellPointsText.replaceTextID(curHero->getNameTextID());
	spellPointsText.replaceNumber(curHero->mana);
	spellPointsText.replaceNumber(curHero->manaLimit());
	spellPointsArea->text = spellPointsText.toString(&GAME->translator());

	//if we have exchange window with this curHero open
	bool noDismiss=false;

	for(auto cew : ENGINE->windows().findWindows<CExchangeWindow>())
	{
		if (cew->holdsGarrison(curHero))
			noDismiss = true;
	}

	//if player only have one hero and no towns
	if(!GAME->interface()->cb->howManyTowns() && GAME->interface()->cb->howManyHeroes() == 1)
		noDismiss = true;

	if(curHero->isMissionCritical())
		noDismiss = true;

	dismissButton->block(noDismiss);

	if(curHero->valOfBonuses(BonusType::BEFORE_BATTLE_REPOSITION) == 0)
	{
		tacticsButton->block(true);
	}
	else
	{
		tacticsButton->block(false);
		tacticsButton->addCallback([this](bool on){ GAME->interface()->cb->setTactics(curHero, on); });
	}

	formations->resetCallback();
	//setting formations
	formations->setSelected(curHero->formation == EArmyFormation::TIGHT ? 1 : 0);
	formations->addCallback([this](int value){ GAME->interface()->cb->setFormation(curHero, static_cast<EArmyFormation>(value));});

	morale->set(curHero);
	luck->set(curHero);

	redraw();
}

void CHeroWindow::dismissCurrent()
{
	GAME->interface()->showYesNoDialog(LIBRARY->generaltexth->allTexts[22], [this]()
		{
			arts->putBackPickedArtifact();
			close();
			GAME->interface()->cb->dismissHero(curHero);
			arts->setHero(nullptr);
		}, nullptr);
}

void CHeroWindow::createBackpackWindow()
{
	ENGINE->windows().createAndPushWindow<CHeroBackpackWindow>(curHero, artSets);
}

void CHeroWindow::commanderWindow()
{
	const auto pickedArtInst = getPickedArtifact();
	const auto hero = getHeroPickedArtifact();

	if(pickedArtInst)
	{
		const auto freeSlot = ArtifactUtils::getArtAnyPosition(curHero->getCommander(), pickedArtInst->getTypeId());
		if(vstd::contains(ArtifactUtils::commanderSlots(), freeSlot)) // We don't want to put it in commander's backpack!
		{
			ArtifactLocation dst(curHero->id, freeSlot);
			dst.creature = SlotID::COMMANDER_SLOT_PLACEHOLDER;
			GAME->interface()->cb->swapArtifacts(ArtifactLocation(hero->id, ArtifactPosition::TRANSITION_POS), dst);
		}
	}
	else
	{
		ENGINE->windows().createAndPushWindow<CStackWindow>(curHero->getCommander(), false);
	}
}

void CHeroWindow::updateGarrisons()
{
	garr->recreateSlots();
	morale->set(curHero);
}

bool CHeroWindow::holdsGarrison(const CArmedInstance * army)
{
	return army == curHero;
}
