/*
 * CHeroBackpackWindow.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "CHeroBackpackWindow.h"

#include "../GameInstance.h"
#include "../gui/Shortcut.h"

#include "../widgets/Buttons.h"
#include "../widgets/Images.h"
#include "../widgets/TextControls.h"
#include "../widgets/WidgetLayout.h"
#include "../widgets/CArtifactsOfHeroBackpack.h"
#include "../widgets/Slider.h"
#include "CMessage.h"
#include "render/Canvas.h"
#include "CPlayerInterface.h"

#include "../../lib/GameLibrary.h"
#include "../../lib/IGameSettings.h"
#include "../../lib/json/JsonNode.h"
#include "../../lib/callback/CCallback.h"
#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/networkPacks/ArtifactLocation.h"
#include "../../lib/texts/CGeneralTextHandler.h"
#include "../../lib/texts/MetaString.h"

/// 储物袋窗口的模组布局（gameWindows.json 的 "backpack" 节），没有时为空节点、一律按原版
static const JsonNode & backpackLayout()
{
	return WidgetLayout::cached("config/widgets/gameWindows.json")["backpack"];
}

/// 储物袋“快取”小窗（长按法器格弹出）改用信息弹窗的宣纸卷轴样式：布局 "quickScroll": true 且模组给了弹窗样式时
static bool quickBackpackScroll()
{
	return backpackLayout()["quickScroll"].Bool() && CMessage::infoWindowStyle().custom;
}

CHeroBackpackWindow::CHeroBackpackWindow(const CGHeroInstance * hero, const std::vector<CArtifactsOfHeroPtr> & artsSets)
	: CWindowObject(WidgetLayout::windowOptions(backpackLayout()["window"], PLAYER_COLORED_BORDERED_STATUSBAR), ImagePath::builtin("heroBackpackDialog"))
	, CWindowWithArtifacts(&artsSets)
	, hero(hero)
{
	OBJECT_CONSTRUCTION;
	addUsedEvents(KEYBOARD);

	// 布局 "grid": {x, y} 是 8×8 储物格左上角（原版 22, 23），"slider": 滑条换皮，"sort": [3 个 {x, y, image}] 排序按钮，
	// "sortText": {font, color} 按钮字，"statusBar": {x,y,w,h}，"title": 标签（修士名与件数），"close": {x, y, image} 关闭按钮
	const JsonNode & layout = backpackLayout();
	const Point artifactsOffset(17, 18);
	arts = std::make_shared<CArtifactsOfHeroBackpack>();
	arts->moveBy(WidgetLayout::point(layout["grid"], Point(windowMargin, windowMargin) + artifactsOffset));
	arts->clickPressedCallback = [this](const CArtPlace & artPlace, const Point & cursorPosition)
	{
		clickPressedOnArtPlace(arts->getHero(), artPlace.slot, true, false, true, cursorPosition);
	};
	arts->showPopupCallback = [this](CArtPlace & artPlace, const Point & cursorPosition)
	{
		showArtifactPopup(*arts, artPlace, cursorPosition);
	};
	addSet(arts);
	arts->setHero(hero);

	WidgetLayout::applySliderSkin(arts->getSlider(), layout["slider"]);

	const EFonts sortFont = WidgetLayout::font(layout["sortText"]["font"], EFonts::FONT_SMALL);
	const ColorRGBA sortColor = WidgetLayout::color(layout["sortText"]["color"], Colors::YELLOW);
	std::string sortByValue = LIBRARY->generaltexth->translate("vcmi.heroWindow.sortBackpackByCost.hover");
	std::string sortBySlot = LIBRARY->generaltexth->translate("vcmi.heroWindow.sortBackpackBySlot.hover");
	std::string sortByClass = LIBRARY->generaltexth->translate("vcmi.heroWindow.sortBackpackByClass.hover");
	
	buttons.emplace_back(std::make_shared<CButton>(Point(), AnimationPath::builtin("heroBackpackSort"),
		CButton::tooltipLocalized("vcmi.heroWindow.sortBackpackByCost"),
		[hero]() { GAME->interface()->cb->sortBackpackArtifactsByCost(hero->id); }));
	buttons.back()->setTextOverlay(sortByValue, sortFont, sortColor);
	buttons.emplace_back(std::make_shared<CButton>(Point(), AnimationPath::builtin("heroBackpackSort"),
		CButton::tooltipLocalized("vcmi.heroWindow.sortBackpackBySlot"),
		[hero]() { GAME->interface()->cb->sortBackpackArtifactsBySlot(hero->id); }));
	buttons.back()->setTextOverlay(sortBySlot, sortFont, sortColor);
	buttons.emplace_back(std::make_shared<CButton>(Point(), AnimationPath::builtin("heroBackpackSort"),
		CButton::tooltipLocalized("vcmi.heroWindow.sortBackpackByClass"),
		[hero]() { GAME->interface()->cb->sortBackpackArtifactsByClass(hero->id); }));
	buttons.back()->setTextOverlay(sortByClass, sortFont, sortColor);

	const int buttonsY = arts->pos.y + arts->pos.h + windowMargin + 1;

	auto buttonPos = Point(arts->pos.x, buttonsY);
	for(size_t i = 0; i < buttons.size(); ++i)
	{
		buttons[i]->moveTo(buttonPos);
		buttonPos += Point(buttons[i]->pos.w + 10, 0);
		if(layout["sort"].isVector() && i < layout["sort"].Vector().size())
			WidgetLayout::applyButton(buttons[i], layout["sort"].Vector()[i]);
	}

	const Rect statusRect = WidgetLayout::rect(layout["statusBar"], Rect(8, pos.h - 26, pos.w - 16, 19));
	statusbar = CGStatusBar::create(std::make_shared<CPicture>(background->getSurface(), statusRect, statusRect.x, statusRect.y));
	if(!layout["statusBar"]["color"].isNull())
		statusbar->setTextColor(WidgetLayout::color(layout["statusBar"]["color"], Colors::WHITE));

	if(layout["title"].isStruct())
	{
		title = WidgetLayout::label(layout["title"], Point(pos.w / 2, 12), FONT_MEDIUM, ETextAlignment::CENTER, Colors::WHITE);
		updateTitle();
	}
	if(layout["close"].isStruct())
	{
		closeButton = std::make_shared<CButton>(Point(), WidgetLayout::animation(layout["close"]["image"], AnimationPath::builtin("hsbtns.def")),
			CButton::tooltip(), [this](){ close(); }, EShortcut::GLOBAL_CANCEL);
		WidgetLayout::applyButton(closeButton, layout["close"]);
	}

	addUsedEvents(LCLICK);
	center();
}

void CHeroBackpackWindow::updateTitle()
{
	if(!title)
		return;
	// 修士名 · 件数（有上限时写“件数 / 上限”）
	const auto cap = GAME->interface()->cb->getSettings().getInteger(EGameSettings::HEROES_BACKPACK_CAP);
	MetaString heroName;
	heroName.appendTextID(hero->getNameTextID());
	std::string text = heroName.toString(&GAME->translator()) + " · " + std::to_string(hero->artifactsInBackpack.size());
	if(cap >= 0)
		text += " / " + std::to_string(cap);
	title->setText(text);
}

void CHeroBackpackWindow::updateArtifacts()
{
	CWindowWithArtifacts::updateArtifacts();
	updateTitle();
}

void CHeroBackpackWindow::notFocusedClick()
{
	close();
}

void CHeroBackpackWindow::keyPressed(EShortcut key)
{
	if(key == EShortcut::GLOBAL_RETURN)
		close();
}

CHeroQuickBackpackWindow::CHeroQuickBackpackWindow(const CGHeroInstance * hero, ArtifactPosition targetSlot)
{
	OBJECT_CONSTRUCTION;

	stretchedBackground = std::make_shared<CFilledTexture>(quickBackpackScroll() ? CMessage::infoWindowStyle().background : ImagePath::builtin("DIBOXBCK"), Rect(0, 0, 0, 0));
	arts = std::make_shared<CArtifactsOfHeroQuickBackpack>(targetSlot);
	arts->moveBy(Point(windowMargin, windowMargin));
	arts->clickPressedCallback = [this](const CArtPlace & artPlace, const Point & cursorPosition)
	{
		if(const auto curHero = arts->getHero())
			swapArtifactAndClose(*arts, artPlace.slot, ArtifactLocation(curHero->id, arts->getFilterSlot()));
	};
	addSet(arts);
	arts->setHero(hero);
	addUsedEvents(GESTURE);
	addUsedEvents(LCLICK);
	pos.w = stretchedBackground->pos.w = arts->pos.w + 2 * windowMargin;
	pos.h = stretchedBackground->pos.h = arts->pos.h + windowMargin;
}

void CHeroQuickBackpackWindow::gesture(bool on, const Point & initialPosition, const Point & finalPosition)
{
	if(on)
		return;

	arts->swapSelected();
	close();
}

void CHeroQuickBackpackWindow::gesturePanning(const Point & initialPosition, const Point & currentPosition, const Point & lastUpdateDistance)
{
	arts->selectSlotAt(currentPosition);
	redraw();
}

void CHeroQuickBackpackWindow::notFocusedClick()
{
	close();
}

void CHeroQuickBackpackWindow::showAll(Canvas & to)
{
	if(arts->getSlotsNum() == 0)
	{
		// Dirty solution for closing that window
		close();
		return;
	}
	if(quickBackpackScroll())
		CMessage::drawInfoWindowBorder(GAME->interface()->playerID, to, pos.x, pos.y, pos.w, pos.h);
	else
		CMessage::drawBorder(PlayerColor(GAME->interface()->playerID), to, pos.w + 28, pos.h + 29, pos.x - 14, pos.y - 15);
	CIntObject::showAll(to);
}
