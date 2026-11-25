/*
 * CExchangeWindow.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "CExchangeWindow.h"

#include "CHeroBackpackWindow.h"

#include "../CPlayerInterface.h"

#include "../GameEngine.h"
#include "../GameInstance.h"
#include "../gui/CursorHandler.h"
#include "../gui/Shortcut.h"
#include "../gui/WindowHandler.h"

#include "../widgets/CGarrisonInt.h"
#include "../widgets/Images.h"
#include "../widgets/Buttons.h"
#include "../widgets/TextControls.h"
#include "../widgets/WidgetLayout.h"

#include "render/IRenderHandler.h"

#include "../../lib/CSkillHandler.h"
#include "../../lib/GameLibrary.h"
#include "../../lib/callback/CCallback.h"
#include "../../lib/entities/hero/CHeroHandler.h"
#include "../../lib/filesystem/Filesystem.h"
#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/texts/CGeneralTextHandler.h"
#include "../../lib/texts/TextOperations.h"
#include "../../lib/json/JsonNode.h"

static const std::string QUICK_EXCHANGE_BG = "quick-exchange/TRADEQE";

/// 修士交换窗口的模组布局：gameWindows.json 的 "exchange" 节。给了 "background" 时整窗按布局排（底图、各控件位置与图、字色），
/// 并且一律带快速交换的那一套按钮（图由布局给，不再依赖 vcmi-extras 的 quick-exchange）；没有时按原版。
static const JsonNode & exchangeLayout()
{
	return WidgetLayout::cached("config/widgets/gameWindows.json")["exchange"];
}

static bool hasCustomExchangeLayout()
{
	return exchangeLayout()["background"].isString();
}

static bool isQuickExchangeLayoutAvailable()
{
	return hasCustomExchangeLayout() || CResourceHandler::get()->existsResource(ImagePath::builtin("SPRITES/" + QUICK_EXCHANGE_BG));
}

static ImagePath exchangeBackground()
{
	if(hasCustomExchangeLayout())
		return ImagePath::fromJson(exchangeLayout()["background"]);
	if(isQuickExchangeLayoutAvailable())
		return ImagePath::builtin(QUICK_EXCHANGE_BG);
	return ImagePath::builtin(ENGINE->isRoeData() ? "TRADE" : "TRADE2");
}

/// 布局里左右成对的项（[左, 右]）取一边，没有时返回空节点
static const JsonNode & exchangeSide(const JsonNode & node, int side)
{
	static const JsonNode empty;
	if(node.isVector() && side < node.Vector().size())
		return node.Vector()[side];
	return empty;
}

/// 第 side 位修士第 index 个功法格左上角：布局 "secondarySkills": [{x, y, step}, …]
static Point exchangeSecSkillPos(int side, int index, bool qeLayout)
{
	const JsonNode & node = exchangeSide(exchangeLayout()["secondarySkills"], side);
	const Point origin = WidgetLayout::point(node, Point(32 + 454 * side, qeLayout ? 80 : 88));
	const int step = node["step"].isNull() ? 36 : node["step"].Integer();
	return origin + Point(step * index, 0);
}

CExchangeWindow::CExchangeWindow(ObjectInstanceID hero1, ObjectInstanceID hero2, QuestionID questionID)
	: CWindowObject(WidgetLayout::windowOptions(exchangeLayout()["window"], PLAYER_COLORED | BORDERED), exchangeBackground()),
	controller(hero1, hero2),
	questionID(questionID)
{
	const bool qeLayout = isQuickExchangeLayoutAvailable();
	const bool custom = hasCustomExchangeLayout();
	const JsonNode & layout = exchangeLayout();
	// 布局里 [左, 右] 成对的点
	auto sidePoint = [&layout](const char * key, int side, const Point & fallback)
	{
		return WidgetLayout::point(exchangeSide(layout[key], side), fallback);
	};
	// 主属性、修为、灵力图标：布局 "skillImage" 换动画（帧序同 PSKIL32：0～3 四维，4 修为，5 灵力）
	const AnimationPath skillAnim = WidgetLayout::animation(layout["skillImage"], AnimationPath::builtin("PSKIL32"));
	const ColorRGBA textColor = WidgetLayout::color(layout["textColor"], Colors::WHITE);

	OBJECT_CONSTRUCTION;
	addUsedEvents(KEYBOARD);

	heroInst[0] = GAME->interface()->cb->getHero(hero1);
	heroInst[1] = GAME->interface()->cb->getHero(hero2);

	auto genTitle = [](const CGHeroInstance * h)
	{
		MetaString title = MetaString::createFromTextID("core.genrltxt.138"); // %s, Level %d %s
		title.replaceTextID(h->getNameTextID());
		title.replaceNumber(h->level);
		title.replaceTextID(h->getClassNameTextID());
		return title.toString(&GAME->translator());
	};

	// 修士名与等级：布局 "titles": [左, 右] 两个标签
	titles[0] = WidgetLayout::label(exchangeSide(layout["titles"], 0), Point(147, qeLayout ? 21 : 25), FONT_SMALL, ETextAlignment::CENTER, Colors::WHITE, genTitle(heroInst[0]));
	titles[1] = WidgetLayout::label(exchangeSide(layout["titles"], 1), Point(653, qeLayout ? 21 : 25), FONT_SMALL, ETextAlignment::CENTER, Colors::WHITE, genTitle(heroInst[1]));

	// 四维：布局 "primarySkills": {"icons": [4 个 {x,y,w,h}], "values": [[左 4 个 {x,y}], [右 4 个]], "areas": [4 个 {x,y,w,h}], "font", "color"}
	const JsonNode & primLayout = layout["primarySkills"];
	for(int g = 0; g < 4; ++g)
	{
		const JsonNode & icon = exchangeSide(primLayout["icons"], g);
		if (!icon.isNull())
			primSkillImages.push_back(std::make_shared<CAnimImage>(skillAnim, g, WidgetLayout::rect(icon, Rect(389, 12 + 26 * g, 22, 22))));
		else if (qeLayout)
			primSkillImages.push_back(std::make_shared<CAnimImage>(skillAnim, g, Rect(389, 12 + 26 * g, 22, 22)));
		else
			primSkillImages.push_back(std::make_shared<CAnimImage>(skillAnim, g, 0, 385, 19 + 36 * g));
	}
	const EFonts primFont = WidgetLayout::font(primLayout["font"], FONT_SMALL);
	const ColorRGBA primColor = WidgetLayout::color(primLayout["color"], Colors::WHITE);

	for(int leftRight : {0, 1})
	{
		const CGHeroInstance * hero = heroInst.at(leftRight);

		for(int m=0; m<GameConstants::PRIMARY_SKILLS; ++m)
		{
			const Point valuePos = WidgetLayout::point(exchangeSide(exchangeSide(primLayout["values"], leftRight), m),
				Point(352 + (qeLayout ? 96 : 93) * leftRight, (qeLayout ? 22 : 35) + (qeLayout ? 26 : 36) * m));
			primSkillValues[leftRight].push_back(std::make_shared<CLabel>(valuePos.x, valuePos.y, primFont, ETextAlignment::CENTER, primColor));
		}


		for(int m=0; m < std::min(static_cast<int>(hero->secSkills.size()), 8); ++m)
			secSkills[leftRight].push_back(std::make_shared<CSecSkillPlace>(exchangeSecSkillPos(leftRight, m, qeLayout), CSecSkillPlace::ImageSize::SMALL,
				hero->secSkills[m].first, hero->secSkills[m].second));

		const Point specPos = sidePoint("specialty", leftRight, Point(67 + 490 * leftRight, qeLayout ? 41 : 45));
		specImages[leftRight] = std::make_shared<CAnimImage>(AnimationPath::builtin("UN32"), hero->getHeroType()->imageIndex, 0, specPos.x, specPos.y);

		const Point expPos = sidePoint("experience", leftRight, Point(103 + 490 * leftRight, qeLayout ? 41 : 45));
		const Point expValuePos = sidePoint("experienceValue", leftRight, Point(119 + 490 * leftRight, qeLayout ? 66 : 71));
		if(custom)
			expImages[leftRight] = std::make_shared<CAnimImage>(skillAnim, 4, Rect(expPos, Point(32, 32)));
		else
			expImages[leftRight] = std::make_shared<CAnimImage>(skillAnim, 4, 0, expPos.x, expPos.y);
		expValues[leftRight] = std::make_shared<CLabel>(expValuePos.x, expValuePos.y, FONT_SMALL, ETextAlignment::CENTER, textColor);

		const Point manaPos = sidePoint("mana", leftRight, Point(139 + 490 * leftRight, qeLayout ? 41 : 45));
		const Point manaValuePos = sidePoint("manaValue", leftRight, Point(155 + 490 * leftRight, qeLayout ? 66 : 71));
		if(custom)
			manaImages[leftRight] = std::make_shared<CAnimImage>(skillAnim, 5, Rect(manaPos, Point(32, 32)));
		else
			manaImages[leftRight] = std::make_shared<CAnimImage>(skillAnim, 5, 0, manaPos.x, manaPos.y);
		manaValues[leftRight] = std::make_shared<CLabel>(manaValuePos.x, manaValuePos.y, FONT_SMALL, ETextAlignment::CENTER, textColor);
	}

	// 法器：布局 "artifacts": [左, 右]，各是 CArtifactsOfHeroMain 的布局（x, y 与 slots、backpack、backpackLeft/Right、backpackScroll）
	if(custom)
	{
		artifs[0] = std::make_shared<CArtifactsOfHeroMain>(WidgetLayout::point(exchangeSide(layout["artifacts"], 0), Point()), exchangeSide(layout["artifacts"], 0));
		artifs[1] = std::make_shared<CArtifactsOfHeroMain>(WidgetLayout::point(exchangeSide(layout["artifacts"], 1), Point()), exchangeSide(layout["artifacts"], 1));
	}
	else
	{
		artifs[0] = std::make_shared<CArtifactsOfHeroMain>(Point(-334, 151));
		artifs[1] = std::make_shared<CArtifactsOfHeroMain>(Point(98, 151));
	}
	artifs[0]->clickPressedCallback = [this, hero = heroInst[0]](const CArtPlace & artPlace, const Point & cursorPosition){clickPressedOnArtPlace(hero, artPlace.slot, true, false, false, cursorPosition);};
	artifs[0]->showPopupCallback = [this, heroArts = artifs[0]](CArtPlace & artPlace, const Point & cursorPosition){showArtifactPopup(*heroArts, artPlace, cursorPosition);};
	artifs[0]->gestureCallback = [this, hero = heroInst[0]](const CArtPlace & artPlace, const Point & cursorPosition){showQuickBackpackWindow(hero, artPlace.slot, cursorPosition);};
	artifs[0]->setHero(heroInst[0]);
	artifs[1]->clickPressedCallback = [this, hero = heroInst[1]](const CArtPlace & artPlace, const Point & cursorPosition){clickPressedOnArtPlace(hero, artPlace.slot, true, false, false, cursorPosition);};
	artifs[1]->showPopupCallback = [this, heroArts = artifs[1]](CArtPlace & artPlace, const Point & cursorPosition){showArtifactPopup(*heroArts, artPlace, cursorPosition);};
	artifs[1]->gestureCallback = [this, hero = heroInst[1]](const CArtPlace & artPlace, const Point & cursorPosition){showQuickBackpackWindow(hero, artPlace.slot, cursorPosition);};
	artifs[1]->setHero(heroInst[1]);


	addSet(artifs[0]);
	addSet(artifs[1]);

	for(int g=0; g<4; ++g)
	{
		primSkillAreas.push_back(std::make_shared<LRClickableAreaWTextComp>());
		const Rect area = qeLayout ? Rect(Point(324, 12 + 26 * g), Point(152, 22)) : Rect(Point(329, 19 + 36 * g), Point(140, 32));
		primSkillAreas[g]->pos = WidgetLayout::rect(exchangeSide(primLayout["areas"], g), area) + pos.topLeft();
		MetaString hoverText;
		hoverText.appendTextID("core.heroscrn.1");
		hoverText.replaceTextID("core.priskill", g);

		primSkillAreas[g]->text = GAME->translator().translate("core.arraytxt", 2 + g);
		primSkillAreas[g]->component = Component( ComponentType::PRIM_SKILL, PrimarySkill(g));
		primSkillAreas[g]->hoverText = hoverText.toString(&GAME->translator());
	}

	//heroes related thing
	for(int b=0; b < heroInst.size(); b++)
	{
		const CGHeroInstance * hero = heroInst.at(b);

		const Point portraitPos = sidePoint("portraits", b, Point(257 + 228 * b + (qeLayout ? 1 : 0), qeLayout ? 10 : 13));
		heroAreas[b] = std::make_shared<CHeroArea>(portraitPos.x, portraitPos.y, hero);
		heroAreas[b]->addClickCallback([this, hero]() -> void
									   {
										   if(getPickedArtifact() == nullptr)
											   GAME->interface()->openHeroWindow(hero);
									   });

		specialtyAreas[b] = std::make_shared<LRClickableAreaWText>();
		specialtyAreas[b]->pos = Rect(specImages[b]->pos.topLeft(), Point(32, 32));
		specialtyAreas[b]->hoverText = LIBRARY->generaltexth->translate("core.heroscrn.27");
		specialtyAreas[b]->text = hero->getHeroType()->getSpecialtyDescriptionTranslated();

		experienceAreas[b] = std::make_shared<LRClickableAreaWText>();
		experienceAreas[b]->pos = Rect(expImages[b]->pos.topLeft(), Point(32, 32));
		experienceAreas[b]->hoverText = LIBRARY->generaltexth->translate("core.heroscrn.9");
		experienceAreas[b]->text = hero->getExperienceDescription().toString(&GAME->translator());

		spellPointsAreas[b] = std::make_shared<LRClickableAreaWText>();
		spellPointsAreas[b]->pos = Rect(manaImages[b]->pos.topLeft(), Point(32, 32));
		spellPointsAreas[b]->hoverText = LIBRARY->generaltexth->translate("core.heroscrn.22");
		MetaString spellPointsText = MetaString::createFromTextID("core.genrltxt.205");
		spellPointsText.replaceTextID(hero->getNameTextID());
		spellPointsText.replaceNumber(hero->mana);
		spellPointsText.replaceNumber(hero->manaLimit());
		spellPointsAreas[b]->text = spellPointsText.toString(&GAME->translator());

		morale[b] = std::make_shared<MoraleLuckBox>(true, Rect(sidePoint("morale", b, Point(176 + 490 * b, 39)), Point(32, 32)), true);
		luck[b] = std::make_shared<MoraleLuckBox>(false,  Rect(sidePoint("luck", b, Point(212 + 490 * b, 39)), Point(32, 32)), true);
	}

	quit = std::make_shared<CButton>(Point(732, 567), AnimationPath::builtin("IOKAY.DEF"), LIBRARY->generaltexth->zelp[600], std::bind(&CExchangeWindow::close, this), EShortcut::GLOBAL_ACCEPT);
	WidgetLayout::applyButton(quit, layout["quit"]);
	if(questionID.getNum() > 0)
		quit->addCallback([=](){ GAME->interface()->cb->selectionMade(0, questionID); });

	// An ally who started the exchange with us decides when it ends, the server then closes this window
	if(questionID.getNum() > 0 && heroInst[0]->tempOwner != GAME->interface()->playerID)
		quit->block(true);

	questlogButton[0] = std::make_shared<CButton>(Point( qeLayout ? 8 : 10, qeLayout ? 39 : 44), AnimationPath::builtin("hsbtns4.def"), CButton::tooltip(LIBRARY->generaltexth->translate("core.heroscrn.0")), std::bind(&CExchangeWindow::questLogShortcut, this));
	questlogButton[1] = std::make_shared<CButton>(Point(740, qeLayout ? 39 : 44), AnimationPath::builtin("hsbtns4.def"), CButton::tooltip(LIBRARY->generaltexth->translate("core.heroscrn.0")), std::bind(&CExchangeWindow::questLogShortcut, this));
	questlogButton[0]->block(!GAME->interface()->hasJournalEntries());
	questlogButton[1]->block(!GAME->interface()->hasJournalEntries());
	// 布局 "questLog": [左, 右] 换位换图，"hidden": true 时不显示
	WidgetLayout::applyButton(questlogButton[0], exchangeSide(layout["questLog"], 0));
	WidgetLayout::applyButton(questlogButton[1], exchangeSide(layout["questLog"], 1));

	const Rect barRect = WidgetLayout::rect(layout["statusBar"], Rect(5, 578, 725, 18));
	statusbar = CGStatusBar::create(std::make_shared<CPicture>(background->getSurface(), barRect, barRect.x, barRect.y));
	if(!layout["statusBar"]["color"].isNull())
		statusbar->setTextColor(WidgetLayout::color(layout["statusBar"]["color"], Colors::WHITE));

	//garrison interface
	// 布局 "garrison": {x, y, "interval": 格间距, "offset": {x, y} 右边修士的麾下相对左边的位移}；"split": [左, 右] 拆分按钮 {x, y, image}
	const JsonNode & garrisonLayout = layout["garrison"];
	garr = std::make_shared<CGarrisonInt>(WidgetLayout::point(garrisonLayout, Point(69, qeLayout ? 120 : 131)),
		garrisonLayout["interval"].isNull() ? 4 : garrisonLayout["interval"].Integer(),
		WidgetLayout::point(garrisonLayout["offset"], Point(418, 0)), heroInst[0], heroInst[1], true, true);
	auto splitButtonCallback = [&](){ garr->splitClick(); };
	const std::array<Point, 2> splitPos = {Point(10, qeLayout ? 122 : 132), Point(744, qeLayout ? 122 : 132)};
	for(int side : {0, 1})
	{
		const JsonNode & splitLayout = exchangeSide(layout["split"], side);
		garr->addSplitBtn(std::make_shared<CButton>(WidgetLayout::point(splitLayout, splitPos[side]),
			WidgetLayout::animation(splitLayout["image"], AnimationPath::builtin("TSBTNS.DEF")),
			CButton::tooltip(LIBRARY->generaltexth->translate("core.tcommand.3")), splitButtonCallback, EShortcut::HERO_ARMY_SPLIT));
	}

	if(qeLayout)
	{
		// 布局里的按钮：{x, y, image}，缺省用快速交换模组原来的位置与图
		auto qeButton = [&layout](const char * key, const Point & fallbackPos, const char * fallbackImage, const std::pair<std::string, std::string> & help, const CFunctionList<void()> & callback)
		{
			return std::make_shared<CButton>(WidgetLayout::point(layout[key], fallbackPos),
				WidgetLayout::animation(layout[key]["image"], AnimationPath::builtin(fallbackImage)), help, callback);
		};

		buttonMoveUnitsFromLeftToRight = qeButton("moveUnitsRight", Point(325, 118), "quick-exchange/armRight.DEF",
			CButton::tooltip(LIBRARY->generaltexth->translate("vcmi.quickExchange.moveAllUnits")),
			[this](){ this->moveUnitsShortcut(true); });

		buttonMoveUnitsFromRightToLeft = qeButton("moveUnitsLeft", Point(425, 118), "quick-exchange/armLeft.DEF",
			CButton::tooltip(LIBRARY->generaltexth->translate("vcmi.quickExchange.moveAllUnits")),
			[this](){ this->moveUnitsShortcut(false); });

		buttonMoveArtifactsFromLeftToRight = qeButton("moveArtifactsRight", Point(325, 154), "quick-exchange/artRight.DEF",
			CButton::tooltip(LIBRARY->generaltexth->translate("vcmi.quickExchange.moveAllArtifacts")),
			[this](){ this->moveArtifactsCallback(true);});

		buttonMoveArtifactsFromRightToLeft = qeButton("moveArtifactsLeft", Point(425, 154), "quick-exchange/artLeft.DEF",
			CButton::tooltip(LIBRARY->generaltexth->translate("vcmi.quickExchange.moveAllArtifacts")),
			[this](){ this->moveArtifactsCallback(false);});

		exchangeUnitsButton = qeButton("swapUnits", Point(377, 118), "quick-exchange/swapAll.DEF",
			CButton::tooltip(LIBRARY->generaltexth->translate("vcmi.quickExchange.swapAllUnits")),
			[this](){ controller.swapArmy(); });

		exchangeArtifactsButton = qeButton("swapArtifacts", Point(377, 154), "quick-exchange/swapAll.DEF",
			CButton::tooltip(LIBRARY->generaltexth->translate("vcmi.quickExchange.swapAllArtifacts")),
			[this](){ this->swapArtifactsCallback(); });

		backpackButtonLeft = qeButton("backpackLeft", Point(325, 518), "heroBackpack",
			CButton::tooltipLocalized("vcmi.heroWindow.openBackpack"),
			[this](){ this->backpackShortcut(true); });

		backpackButtonRight = qeButton("backpackRight", Point(419, 518), "heroBackpack",
			CButton::tooltipLocalized("vcmi.heroWindow.openBackpack"),
			[this](){ this->backpackShortcut(false); });

		// 布局按钮图自带字时不叠原版储物袋小图标（"backpackIcon": false）
		if(layout["backpackIcon"].isNull() || layout["backpackIcon"].Bool())
		{
			backpackButtonLeft->setOverlay(std::make_shared<CPicture>(ImagePath::builtin("heroWindow/backpackButtonIcon")));
			backpackButtonRight->setOverlay(std::make_shared<CPicture>(ImagePath::builtin("heroWindow/backpackButtonIcon")));
		}

		auto leftHeroBlock = heroInst[0]->tempOwner != GAME->interface()->cb->getPlayerID();
		auto rightHeroBlock = heroInst[1]->tempOwner != GAME->interface()->cb->getPlayerID();

		buttonMoveUnitsFromLeftToRight->block(leftHeroBlock);
		buttonMoveUnitsFromRightToLeft->block(rightHeroBlock);
		buttonMoveArtifactsFromLeftToRight->block(leftHeroBlock);
		buttonMoveArtifactsFromRightToLeft->block(rightHeroBlock);

		exchangeUnitsButton->block(leftHeroBlock || rightHeroBlock);
		exchangeArtifactsButton->block(leftHeroBlock || rightHeroBlock);

		backpackButtonLeft->block(leftHeroBlock);
		backpackButtonRight->block(rightHeroBlock);

		// 每格兵下面的单格移动箭头：布局 "unitArrows": {"left": {x, y, step, image}, "right": {…}}（left 在右边修士麾下、朝左移）
		const JsonNode & arrowsLeft = layout["unitArrows"]["left"];
		const JsonNode & arrowsRight = layout["unitArrows"]["right"];
		const Point arrowLeftOrigin = WidgetLayout::point(arrowsLeft, Point(483, 155));
		const Point arrowRightOrigin = WidgetLayout::point(arrowsRight, Point(65, 155));
		const int arrowLeftStep = arrowsLeft["step"].isNull() ? 36 : arrowsLeft["step"].Integer();
		const int arrowRightStep = arrowsRight["step"].isNull() ? 36 : arrowsRight["step"].Integer();
		for(int i = 0; i < GameConstants::ARMY_SIZE; i++)
		{
			moveUnitFromRightToLeftButtons.push_back(
				std::make_shared<CButton>(
					arrowLeftOrigin + Point(arrowLeftStep * i, 0),
					WidgetLayout::animation(arrowsLeft["image"], AnimationPath::builtin("quick-exchange/unitLeft.DEF")),
					CButton::tooltip(LIBRARY->generaltexth->translate("vcmi.quickExchange.moveUnit")),
					[this, i]() { creatureArrowButtonCallback(false, SlotID(i)); }));
			moveUnitFromRightToLeftButtons.back()->block(rightHeroBlock);

			moveUnitFromLeftToRightButtons.push_back(
				std::make_shared<CButton>(
					arrowRightOrigin + Point(arrowRightStep * i, 0),
					WidgetLayout::animation(arrowsRight["image"], AnimationPath::builtin("quick-exchange/unitRight.DEF")),
					CButton::tooltip(LIBRARY->generaltexth->translate("vcmi.quickExchange.moveUnit")),
					[this, i]() { creatureArrowButtonCallback(true, SlotID(i)); }));
			moveUnitFromLeftToRightButtons.back()->block(leftHeroBlock);
		}
	}

	CExchangeWindow::updateArtifacts();
}

void CExchangeWindow::creatureArrowButtonCallback(bool leftToRight, SlotID slotId)
{
	if (ENGINE->isKeyboardAltDown())
		controller.moveArmy(leftToRight, slotId);
	else if (ENGINE->isKeyboardCtrlDown())
		controller.moveSingleStackCreature(leftToRight, slotId, true);
	else if (ENGINE->isKeyboardShiftDown())
		controller.moveSingleStackCreature(leftToRight, slotId, false);
	else
		controller.moveStack(leftToRight, slotId);
}

void CExchangeWindow::moveArtifactsCallback(bool leftToRight)
{
	bool moveEquipped = !ENGINE->isKeyboardShiftDown();
	bool moveBackpack = !ENGINE->isKeyboardCmdDown();
	controller.moveArtifacts(leftToRight, moveEquipped, moveBackpack);
};

void CExchangeWindow::swapArtifactsCallback()
{
	bool moveEquipped = !ENGINE->isKeyboardShiftDown();
	bool moveBackpack = !ENGINE->isKeyboardCmdDown();
	controller.swapArtifacts(moveEquipped, moveBackpack);
}

void CExchangeWindow::moveUnitsShortcut(bool leftToRight)
{
	std::optional<SlotID> slotId = std::nullopt;
	if(const auto * slot = getSelectedSlotID())
		slotId = slot->getSlot();
	controller.moveArmy(leftToRight, slotId);
};

void CExchangeWindow::backpackShortcut(bool leftHero)
{
	ENGINE->windows().createAndPushWindow<CHeroBackpackWindow>(heroInst[leftHero ? 0 : 1], artSets);
};

void CExchangeWindow::keyPressed(EShortcut key)
{
	switch (key)
	{
		case EShortcut::EXCHANGE_ARMY_TO_LEFT:
			moveUnitsShortcut(false);
		break;
		case EShortcut::EXCHANGE_ARMY_TO_RIGHT:
			moveUnitsShortcut(true);
		break;
		case EShortcut::EXCHANGE_ARMY_SWAP:
			controller.swapArmy();
		break;
		case EShortcut::EXCHANGE_ARTIFACTS_TO_LEFT:
			controller.moveArtifacts(false, true, true);
		break;
		case EShortcut::EXCHANGE_ARTIFACTS_TO_RIGHT:
			controller.moveArtifacts(true, true, true);
		break;
		case EShortcut::EXCHANGE_ARTIFACTS_SWAP:
			controller.swapArtifacts(true, true);
		break;
		case EShortcut::EXCHANGE_EQUIPPED_TO_LEFT:
			controller.moveArtifacts(false, true, false);
		break;
		case EShortcut::EXCHANGE_EQUIPPED_TO_RIGHT:
			controller.moveArtifacts(true, true, false);
		break;
		case EShortcut::EXCHANGE_EQUIPPED_SWAP:
			controller.swapArtifacts(true, false);
		break;
		case EShortcut::EXCHANGE_BACKPACK_TO_LEFT:
			controller.moveArtifacts(false, false, true);
		break;
		case EShortcut::EXCHANGE_BACKPACK_TO_RIGHT:
			controller.moveArtifacts(true, false, true);
		break;
		case EShortcut::EXCHANGE_BACKPACK_SWAP:
			controller.swapArtifacts(false, true);
		break;
		case EShortcut::EXCHANGE_BACKPACK_LEFT:
			backpackShortcut(true);
		break;
		case EShortcut::EXCHANGE_BACKPACK_RIGHT:
			backpackShortcut(false);
		break;
	}
}

const CGarrisonSlot * CExchangeWindow::getSelectedSlotID() const
{
	return garr->getSelection();
}

void CExchangeWindow::updateGarrisons()
{
	garr->recreateSlots();

	updateArtifacts();
}

bool CExchangeWindow::holdsGarrison(const CArmedInstance * army)
{
	return garr->upperArmy() == army || garr->lowerArmy() == army;
}

void CExchangeWindow::questLogShortcut()
{
	ENGINE->cursor().dragAndDropCursor(nullptr);
	GAME->interface()->showQuestLog();
}

void CExchangeWindow::updateArtifacts()
{
	const bool qeLayout = isQuickExchangeLayoutAvailable();

	OBJECT_CONSTRUCTION;

	CWindowWithArtifacts::updateArtifacts();

	for(size_t leftRight : {0, 1})
	{
		const CGHeroInstance * hero = heroInst.at(leftRight);

		for(int m=0; m<GameConstants::PRIMARY_SKILLS; ++m)
		{
			auto value = heroInst[leftRight]->getPrimSkillLevel(static_cast<PrimarySkill>(m));
			primSkillValues[leftRight][m]->setText(std::to_string(value));
		}

		int slots = 8;
		bool isMoreSkillsThanSlots = hero->secSkills.size() > slots;
		for(int m=0; m < std::min(static_cast<int>(hero->secSkills.size()), 8); ++m)
		{
			if(m == slots - 1)
			{
				if(isMoreSkillsThanSlots)
				{
					Rect r(exchangeSecSkillPos(leftRight, m, qeLayout) + Point(0, qeLayout ? 3 : 0), Point(34, 28));
					secSkillsFull[leftRight] = std::make_shared<CMultiLineLabel>(r, EFonts::FONT_MEDIUM, ETextAlignment::CENTER,
						WidgetLayout::color(exchangeLayout()["textColor"], Colors::WHITE), "...");
					secSkillsFullArea[leftRight] = std::make_shared<LRClickableAreaWText>(r, LIBRARY->generaltexth->translate("vcmi.kingdomOverview.secSkillOverflow.hover"), LIBRARY->generaltexth->translate("vcmi.kingdomOverview.secSkillOverflow.help"));
					secSkills[leftRight][m]->setSkill(SecondarySkill::NONE);
					continue;
				}
				else
				{
					secSkillsFull[leftRight].reset();
					secSkillsFullArea[leftRight].reset();
				}
			}

			int id = hero->secSkills[m].first;
			int level = hero->secSkills[m].second;

			secSkills[leftRight][m]->setSkill(id, level);
		}

		expValues[leftRight]->setText(TextOperations::formatMetric(hero->exp, 3));
		manaValues[leftRight]->setText(TextOperations::formatMetric(hero->mana, 3));

		morale[leftRight]->set(hero);
		luck[leftRight]->set(hero);
	}
}
