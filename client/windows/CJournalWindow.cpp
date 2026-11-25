/*
 * CJournalWindow.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "CJournalWindow.h"

#include "../widgets/WidgetLayout.h"
#include "../CPlayerInterface.h"
#include "../GameEngine.h"
#include "../GameInstance.h"
#include "../gui/Shortcut.h"
#include "render/Canvas.h"
#include "../render/Colors.h"
#include "../widgets/Buttons.h"
#include "../widgets/CComponent.h"
#include "../widgets/Slider.h"

#include "../../lib/GameLibrary.h"
#include "../../lib/callback/CCallback.h"
#include "../../lib/gameState/QuestInfo.h"
#include "../../lib/gameState/ScenarioEventJournalEntry.h"
#include "../../lib/texts/CGeneralTextHandler.h"

JournalLabel::JournalLabel(const Rect & position, const std::string & text)
	: CMultiLineLabel(position, FONT_SMALL, ETextAlignment::TOPLEFT, Colors::WHITE, text)
{
}

void JournalLabel::clickPressed(const Point & cursorPosition)
{
	callback();
}

void JournalLabel::showAll(Canvas & to)
{
	CMultiLineLabel::showAll(to);
}

/// 行旅札记（任务日志与事件日志共用）布局：gameWindows.json 的 "journal" 节——窗口选项、正文与条目字色、
/// 当前条目框色、两个页签与确定按钮的图和字色、条目滑条皮肤。底图 questDialog 按原名替换。没有布局文件时按原版。
static const JsonNode & journalLayout()
{
	return WidgetLayout::cached("config/widgets/gameWindows.json")["journal"];
}

JournalWindow::JournalWindow(EJournalMode mode)
	: CWindowObject(WidgetLayout::windowOptions(journalLayout()["window"], PLAYER_COLORED | BORDERED), ImagePath::builtin("questDialog"))
	, mode(mode)
{
	OBJECT_CONSTRUCTION;

	const auto & layout = journalLayout();
	description = std::make_shared<CTextBox>("", Rect(205, DESCRIPTION_TOP, 385, DESCRIPTION_HEIGHT), CSlider::BROWN, FONT_MEDIUM, ETextAlignment::TOPLEFT, WidgetLayout::color(layout["textColor"], Colors::WHITE));
	ok = std::make_shared<CButton>(Point(539, 398), AnimationPath::builtin("IOKAY.DEF"), LIBRARY->generaltexth->zelp[445], std::bind(&JournalWindow::close, this), EShortcut::GLOBAL_RETURN);
	auto questsTab = std::make_shared<CToggleButton>(Point(193, 18), AnimationPath::builtin("settingsWindow/button190"), CButton::tooltip(), nullptr);
	auto eventsTab = std::make_shared<CToggleButton>(Point(411, 18), AnimationPath::builtin("settingsWindow/button190"), CButton::tooltip(), nullptr);
	const ColorRGBA tabTextColor = WidgetLayout::color(layout["tabs"]["textColor"], Colors::YELLOW);
	questsTab->setTextOverlay(LIBRARY->generaltexth->translate("vcmi.adventureMap.journal.quests"), FONT_SMALL, tabTextColor);
	eventsTab->setTextOverlay(LIBRARY->generaltexth->translate("vcmi.adventureMap.journal.events"), FONT_SMALL, tabTextColor);
	WidgetLayout::applyButton(questsTab, layout["tabs"]["quests"]);
	WidgetLayout::applyButton(eventsTab, layout["tabs"]["events"]);
	WidgetLayout::applyButton(ok, layout["ok"]);

	questsTab->block(!GAME->interface()->hasDisplayableQuests());
	eventsTab->block(!GAME->interface()->hasScenarioEventJournalEntries());

	journalTabs = std::make_shared<CToggleGroup>([this](int selectedMode)
	{
		if(selectedMode == static_cast<int>(this->mode))
			return;

		close();
		if(selectedMode == static_cast<int>(EJournalMode::QUESTS))
			GAME->interface()->showQuestLog();
		else
			GAME->interface()->showScenarioEventJournal();
	});
	journalTabs->addToggle(static_cast<int>(EJournalMode::QUESTS), questsTab);
	journalTabs->addToggle(static_cast<int>(EJournalMode::EVENTS), eventsTab);
	journalTabs->setSelected(static_cast<int>(mode));

	slider = std::make_shared<CSlider>(Point(166, 195), 191, std::bind(&JournalWindow::sliderMoved, this, _1), VISIBLE_ITEM_COUNT, 0, 0, Orientation::VERTICAL, CSlider::BROWN);
	slider->setPanningStep(32);
	WidgetLayout::applySliderSkin(slider, layout["sliderSkin"]);
}

void JournalWindow::initializeItems()
{
	OBJECT_CONSTRUCTION;

	for(size_t i = 0; i < getItemCount(); ++i)
	{
		auto label = std::make_shared<JournalLabel>(Rect(13, 195, 149, 31), getItemText(i));
		if(!journalLayout()["listColor"].isNull())
			label->setColor(WidgetLayout::color(journalLayout()["listColor"], Colors::WHITE));
		label->callback = [this, i]()
		{
			selectItem(i);
			redraw();
		};
		labels.push_back(label);
	}

	const int firstVisible = std::max(0, static_cast<int>(getItemCount()) - VISIBLE_ITEM_COUNT);
	recreateItemList(firstVisible);
	selectItem(getItemCount() - 1);

	slider->setAmount(static_cast<int>(getItemCount()));
	if(getItemCount() > VISIBLE_ITEM_COUNT)
	{
		slider->block(false);
		slider->scrollTo(firstVisible);
	}
	else
	{
		slider->block(true);
		slider->scrollToMin();
	}
}

void JournalWindow::selectItem(size_t itemIndex)
{
	selectedLabel = static_cast<int>(itemIndex);
	onItemSelected(itemIndex);
}

void JournalWindow::setContent(const std::string & text, std::vector<std::shared_ptr<CComponent>> components, int componentAreaHeight)
{
	if(description->slider)
		description->slider->scrollToMin();
	description->setText(text);
	componentsBox.reset();

	int descriptionHeight = DESCRIPTION_HEIGHT;
	if(!components.empty())
	{
		descriptionHeight -= componentAreaHeight;
		OBJECT_CONSTRUCTION;
		componentsBox = std::make_shared<CComponentBox>(std::move(components), Rect(205, DESCRIPTION_TOP + descriptionHeight + 15, 385, 115));
		if(!journalLayout()["textColor"].isNull())
			componentsBox->setTextColor(WidgetLayout::color(journalLayout()["textColor"], Colors::WHITE));
	}
	description->resize(Point(385, descriptionHeight));
}

void JournalWindow::recreateItemList(int firstVisible)
{
	for(size_t i = 0; i < labels.size(); ++i)
	{
		labels[i]->pos = Rect(pos.x + 14, pos.y + 195 + (static_cast<int>(i) - firstVisible) * 32, 151, 31);
		if(static_cast<int>(i) >= firstVisible && static_cast<int>(i) < firstVisible + VISIBLE_ITEM_COUNT)
			labels[i]->enable();
		else
			labels[i]->disable();
	}
	updateMinimap();
}

void JournalWindow::sliderMoved(int newPosition)
{
	recreateItemList(newPosition);
	redraw();
}

void JournalWindow::showAll(Canvas & to)
{
	CWindowObject::showAll(to);
	if(selectedLabel < 0 || selectedLabel >= labels.size())
		return;

	// Keep the established quest-log rendering unchanged; event rows are clipped
	// when scrolled out because their previous positions may overlap the map view.
	if(mode == EJournalMode::EVENTS && labels[selectedLabel]->isDisabled())
		return;

	Rect selection = Rect::createAround(labels[selectedLabel]->pos, 1);
	selection.x -= 2;
	selection.w += 2;
	to.drawBorder(selection, WidgetLayout::color(journalLayout()["selectionColor"], Colors::METALLIC_GOLD));
}
