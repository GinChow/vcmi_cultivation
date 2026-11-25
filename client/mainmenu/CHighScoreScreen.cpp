/*
 * CHighScoreScreen.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */

#include "StdInc.h"

#include "CHighScoreScreen.h"
#include "CStatisticScreen.h"
#include "CMainMenu.h"
#include "../GameEngine.h"
#include "../GameInstance.h"
#include "../gui/WindowHandler.h"
#include "../gui/Shortcut.h"
#include "media/IMusicPlayer.h"
#include "media/ISoundPlayer.h"
#include "../widgets/Buttons.h"
#include "../widgets/CTextInput.h"
#include "../widgets/Images.h"
#include "../widgets/GraphicalPrimitiveCanvas.h"
#include "../widgets/VideoWidget.h"
#include "../widgets/WidgetLayout.h"
#include "../windows/InfoWindows.h"
#include "../widgets/TextControls.h"
#include "render/Canvas.h"
#include "render/IRenderHandler.h"

#include "../../lib/texts/CGeneralTextHandler.h"
#include "../../lib/texts/TextOperations.h"
#include "../../lib/CConfigHandler.h"
#include "../../lib/CCreatureHandler.h"
#include "../../lib/constants/EntityIdentifiers.h"
#include "../../lib/gameState/HighScore.h"
#include "../../lib/gameState/GameStatistics.h"
#include "../../lib/GameLibrary.h"
#include "../../lib/serializer/JsonSerializer.h"
#include "../../lib/serializer/JsonDeserializer.h"

/// 英名录布局（模组换皮用，如 cultivation-ui 的 config/widgets/menuScreens.json 里的 "highScores" 节）：
/// 表头与各行的纵向位置、行距、各列横坐标、字色、名次标签隐藏、兵种小像位置、四个按钮的位置与图。
/// 没有这份文件时返回空节点，下面每一项都退回原版写死的值。
static const JsonNode & highScoreLayout()
{
	return WidgetLayout::cached("config/widgets/menuScreens.json")["highScores"];
}

/// 布局里的整数项，没写时用原版值
static int layoutInt(const JsonNode & node, int fallback)
{
	return node.isNumber() ? static_cast<int>(node.Integer()) : fallback;
}

CHighScoreScreen::CHighScoreScreen(HighScorePage highscorepage, int highlighted)
	: CWindowObject(WidgetLayout::windowOptions(highScoreLayout()["window"], BORDERED)), highscorepage(highscorepage), highlighted(highlighted)
{
	addUsedEvents(LCLICK);
	addUsedEvents(SHOW_POPUP);

	OBJECT_CONSTRUCTION;
	pos = center(Rect(0, 0, 800, 600));

	addHighScores();
	addButtons();
}

void CHighScoreScreen::rowEvent(std::function<void(int row, bool currentGameNotInListEntry)> func, const Point & cursorPosition)
{
	// 每行的可点范围：原版 x 80、宽 635，行高 50、从第一行中线往上 26 像素起
	const auto & layout = highScoreLayout();
	const int rowY = layoutInt(layout["rows"]["y"], 66);
	const int rowStep = layoutInt(layout["rows"]["step"], 50);
	const int clickX = layoutInt(layout["clickArea"]["x"], 80);
	const int clickW = layoutInt(layout["clickArea"]["w"], 635);
	for (int i = 0; i < screenRows; i++)
	{
		bool currentGameNotInListEntry = i == (screenRows - 1) && highlighted > (screenRows - 1);

		Rect r = Rect(clickX, rowY + i * rowStep - rowStep / 2 - 1, clickW, rowStep);
		if(r.isInside(cursorPosition - pos))
			func(i, currentGameNotInListEntry);
	}
}

void CHighScoreScreen::clickPressed(const Point & cursorPosition)
{
	rowEvent([this](int row, bool currentGameNotInListEntry){
		auto node = persistentStorage["highscore"][highscorepage == HighScorePage::SCENARIO ? "scenario" : "campaign"][currentGameNotInListEntry ? highlighted : row];
		if(node["statistic"].isNull())
			return;
		
		JsonDeserializer ser(nullptr, node);
		StatisticDataSet stat;
		ser.serializeStruct("statistic", stat);
		ENGINE->windows().createAndPushWindow<CStatisticScreen>(stat);
	}, cursorPosition);
}

void CHighScoreScreen::showPopupWindow(const Point & cursorPosition)
{
	rowEvent([this](int row, bool currentGameNotInListEntry){
		std::string tmp = persistentStorage["highscore"][highscorepage == HighScorePage::SCENARIO ? "scenario" : "campaign"][currentGameNotInListEntry ? highlighted : row]["datetime"].String();
		if(!tmp.empty())
			CRClickPopup::createAndPush("{" + LIBRARY->generaltexth->translate("core.help.316.hover") + "}\n\n" + tmp);
	}, cursorPosition);
}

void CHighScoreScreen::addButtons()
{
	OBJECT_CONSTRUCTION;
	
	buttons.clear();

	buttons.push_back(std::make_shared<CButton>(Point(31, 113), AnimationPath::builtin("HISCCAM.DEF"), CButton::tooltip(),  [this](){ buttonCampaignClick(); }, EShortcut::HIGH_SCORES_CAMPAIGNS));
	buttons.push_back(std::make_shared<CButton>(Point(31, 345), AnimationPath::builtin("HISCSTA.DEF"), CButton::tooltip(),  [this](){ buttonScenarioClick(); }, EShortcut::HIGH_SCORES_SCENARIOS));
	buttons.push_back(std::make_shared<CButton>(Point(726, 113), AnimationPath::builtin("HISCRES.DEF"), CButton::tooltip(), [this](){ buttonResetClick(); }, EShortcut::HIGH_SCORES_RESET));
	buttons.push_back(std::make_shared<CButton>(Point(726, 345), AnimationPath::builtin("HISCEXT.DEF"), CButton::tooltip(), [this](){ buttonExitClick(); }, EShortcut::GLOBAL_RETURN));

	// 布局可换四个按钮的位置与图（顺序同上：征途、单局、重置、返回）
	const auto & layout = highScoreLayout()["buttons"];
	const std::array<std::string, 4> keys = {"campaign", "scenario", "reset", "exit"};
	for(size_t i = 0; i < keys.size(); i++)
		WidgetLayout::applyButton(buttons[i], layout[keys[i]]);
}

void CHighScoreScreen::addHighScores()
{
	OBJECT_CONSTRUCTION;

	background = std::make_shared<CPicture>(ImagePath::builtin(highscorepage == HighScorePage::SCENARIO ? "HISCORE" : "HISCORE2"));

	texts.clear();
	images.clear();

	// 布局：表头纵坐标、各行中线与行距、各列横坐标、字色；缺省全是原版值
	const auto & layout = highScoreLayout();
	const bool scenario = highscorepage == HighScorePage::SCENARIO;
	const auto & columns = layout["columns"][scenario ? "scenario" : "campaign"];
	const int rankX = layoutInt(columns["rank"], 115);
	const int playerX = layoutInt(columns["player"], 225);
	const int nameX = layoutInt(columns["name"], 405);
	const int daysX = layoutInt(columns["days"], 557);
	const int scoreX = layoutInt(columns["score"], scenario ? 627 : 592);
	const int playerW = layoutInt(layout["widths"]["player"], 120);
	const int nameW = layoutInt(layout["widths"]["name"], 200);
	const int headerY = layoutInt(layout["header"]["y"], 20);
	const ColorRGBA headerColor = WidgetLayout::color(layout["header"]["color"], Colors::WHITE);
	const int rowY = layoutInt(layout["rows"]["y"], 66);
	const int rowStep = layoutInt(layout["rows"]["step"], 50);
	const ColorRGBA rowColor = WidgetLayout::color(layout["rows"]["color"], Colors::WHITE);
	const ColorRGBA highlightColor = WidgetLayout::color(layout["rows"]["highlightColor"], Colors::YELLOW);
	// 底图上已经画好前几名的名次印时，这几行不再写数字
	const int hiddenRanks = layoutInt(layout["rows"]["hideRanks"], 0);
	const int creatureX = layoutInt(layout["creature"]["x"], 670);
	const int creatureDy = layoutInt(layout["creature"]["dy"], -15);

	// Header
	texts.push_back(std::make_shared<CLabel>(rankX, headerY, FONT_MEDIUM, ETextAlignment::CENTER, headerColor, LIBRARY->generaltexth->translate("core.genrltxt.433"))); // rank
	texts.push_back(std::make_shared<CLabel>(playerX, headerY, FONT_MEDIUM, ETextAlignment::CENTER, headerColor, LIBRARY->generaltexth->translate("core.genrltxt.434"))); // player

	if(scenario)
	{
		texts.push_back(std::make_shared<CLabel>(nameX, headerY, FONT_MEDIUM, ETextAlignment::CENTER, headerColor, LIBRARY->generaltexth->translate("core.genrltxt.435"))); // land
		texts.push_back(std::make_shared<CLabel>(daysX, headerY, FONT_MEDIUM, ETextAlignment::CENTER, headerColor, LIBRARY->generaltexth->translate("core.genrltxt.436"))); // days
		texts.push_back(std::make_shared<CLabel>(scoreX, headerY, FONT_MEDIUM, ETextAlignment::CENTER, headerColor, LIBRARY->generaltexth->translate("core.genrltxt.75"))); // score
	}
	else
	{
		texts.push_back(std::make_shared<CLabel>(nameX, headerY, FONT_MEDIUM, ETextAlignment::CENTER, headerColor, LIBRARY->generaltexth->translate("core.genrltxt.672"))); // campaign
		texts.push_back(std::make_shared<CLabel>(scoreX, headerY, FONT_MEDIUM, ETextAlignment::CENTER, headerColor, LIBRARY->generaltexth->translate("core.genrltxt.75"))); // score
	}

	// Content
	auto & data = persistentStorage["highscore"][scenario ? "scenario" : "campaign"];
	for (int i = 0; i < screenRows; i++)
	{
		bool currentGameNotInListEntry = (i == (screenRows - 1) && highlighted > (screenRows - 1));
		auto & curData = data[currentGameNotInListEntry ? highlighted : i];
		const int y = rowY + i * rowStep;

		ColorRGBA color = (i == highlighted || currentGameNotInListEntry) ? highlightColor : rowColor;

		if(i >= hiddenRanks || currentGameNotInListEntry)
			texts.push_back(std::make_shared<CLabel>(rankX, y, FONT_MEDIUM, ETextAlignment::CENTER, color, std::to_string((currentGameNotInListEntry ? highlighted : i) + 1)));
		texts.push_back(std::make_shared<CLabel>(playerX, y, FONT_MEDIUM, ETextAlignment::CENTER, color, curData["player"].String(), playerW));

		if(scenario)
		{
			texts.push_back(std::make_shared<CLabel>(nameX, y, FONT_MEDIUM, ETextAlignment::CENTER, color, curData["scenarioName"].String(), nameW));
			texts.push_back(std::make_shared<CLabel>(daysX, y, FONT_MEDIUM, ETextAlignment::CENTER, color, std::to_string(curData["days"].Integer())));
			texts.push_back(std::make_shared<CLabel>(scoreX, y, FONT_MEDIUM, ETextAlignment::CENTER, color, std::to_string(curData["points"].Integer())));
		}
		else
		{
			texts.push_back(std::make_shared<CLabel>(nameX, y, FONT_MEDIUM, ETextAlignment::CENTER, color, curData["campaignName"].String(), nameW));
			texts.push_back(std::make_shared<CLabel>(scoreX, y, FONT_MEDIUM, ETextAlignment::CENTER, color, std::to_string(curData["points"].Integer())));
		}

		if(curData["points"].Integer() > 0 && curData["points"].Integer() <= ((highscorepage == HighScorePage::CAMPAIGN) ? 2500 : 500))
			images.push_back(std::make_shared<CAnimImage>(AnimationPath::builtin("CPRSMALL"), (*LIBRARY->creh)[HighScoreCalculation::getCreatureForPoints(curData["points"].Integer(), highscorepage == HighScorePage::CAMPAIGN)]->getIconIndex(), 0, creatureX, y + creatureDy));
	}
}

void CHighScoreScreen::buttonCampaignClick()
{
	highscorepage = HighScorePage::CAMPAIGN;
	addHighScores();
	addButtons();
	redraw();
}

void CHighScoreScreen::buttonScenarioClick()
{
	OBJECT_CONSTRUCTION;
	highscorepage = HighScorePage::SCENARIO;
	addHighScores();
	addButtons();
	redraw();
}

void CHighScoreScreen::buttonResetClick()
{
	CInfoWindow::showYesNoDialog(
		LIBRARY->generaltexth->allTexts[666],
		{},
		[this]()
		{
			Settings entry = persistentStorage.write["highscore"];
			entry->clear();
			addHighScores();
			addButtons();
			redraw();
		},
		0
	);
}

void CHighScoreScreen::buttonExitClick()
{
	close();
	GAME->mainmenu()->playMusic();
}

void CHighScoreScreen::showAll(Canvas & to)
{
	const auto & bgConfig = CMainMenuConfig::get().getConfig()["backgroundAround"];
	to.fillTexture(ENGINE->renderHandler().loadImage(bgConfig.isString() ? ImagePath::fromJson(bgConfig) : ImagePath::builtin("DIBOXBCK"), EImageBlitMode::OPAQUE));
	CWindowObject::showAll(to);
}

CHighScoreInputScreen::CHighScoreInputScreen(bool won, HighScoreCalculation calc, const StatisticDataSet & statistic)
	: CWindowObject(BORDERED), won(won), calc(calc), stat(statistic)
{
	addUsedEvents(LCLICK | KEYBOARD);

	OBJECT_CONSTRUCTION;
	pos = center(Rect(0, 0, 800, 600));

	backgroundRect = std::make_shared<TransparentFilledRectangle>(Rect(0, 0, pos.w, pos.h), Colors::BLACK);

	if(won)
	{

		videoPlayer = std::make_shared<VideoWidget>(Point(0, 0), VideoPath::builtin("HSANIM.SMK"), VideoPath::builtin("HSLOOP.SMK"), true);

		int border = 100;
		int textareaW = ((pos.w - 2 * border) / 4);
		std::vector<std::string> t = { "438", "439", "440", "441", "676" }; // time, score, difficulty, final score, rank
		for (int i = 0; i < 5; i++)
			texts.push_back(std::make_shared<CMultiLineLabel>(Rect(textareaW * i + border - (textareaW / 2), 450, textareaW, 100), FONT_HIGH_SCORE, ETextAlignment::TOPCENTER, Colors::WHITE, LIBRARY->generaltexth->translate("core.genrltxt." + t[i])));

		std::string creatureName = (calc.calculate().cheater) ? LIBRARY->generaltexth->translate("core.genrltxt.260") : (*LIBRARY->creh)[HighScoreCalculation::getCreatureForPoints(calc.calculate().total, calc.isCampaign)]->getNameSingularTranslated();
		t = { std::to_string(calc.calculate().sumDays), std::to_string(calc.calculate().basic), LIBRARY->generaltexth->translate("core.arraytxt." + std::to_string((142 + calc.parameters[0].difficulty))), std::to_string(calc.calculate().total), creatureName };
		for (int i = 0; i < 5; i++)
			texts.push_back(std::make_shared<CMultiLineLabel>(Rect(textareaW * i + border - (textareaW / 2), 530, textareaW, 100), FONT_HIGH_SCORE, ETextAlignment::TOPCENTER, Colors::WHITE, t[i]));
 
		ENGINE->music().playMusic(AudioPath::builtin("music/Win Scenario"), true, true);
	}
	else
	{
		videoPlayer = std::make_shared<VideoWidgetOnce>(Point(0, 0), VideoPath::builtin("LOSEGAME.SMK"), true, this);
		ENGINE->music().playMusic(AudioPath::builtin("music/UltimateLose"), false, true);
	}

	if (settings["general"]["enableUiEnhancements"].Bool())
	{
		statisticButton = std::make_shared<CButton>(Point(726, 10), AnimationPath::builtin("TPTAV02.DEF"), CButton::tooltip(LIBRARY->generaltexth->translate("vcmi.statisticWindow.statistics")), [this](){ ENGINE->windows().createAndPushWindow<CStatisticScreen>(stat); }, EShortcut::HIGH_SCORES_STATISTICS);
		texts.push_back(std::make_shared<CLabel>(716, 25, EFonts::FONT_HIGH_SCORE, ETextAlignment::CENTERRIGHT, Colors::WHITE, LIBRARY->generaltexth->translate("vcmi.statisticWindow.statistics") + ":"));
	}
}

void CHighScoreInputScreen::stopMusicAndClose()
{
	GAME->mainmenu()->playMusic();
	close();
}

void CHighScoreInputScreen::onVideoPlaybackFinished()
{
	stopMusicAndClose();
}

int CHighScoreInputScreen::addEntry(std::string text) {
	std::vector<JsonNode> baseNode = persistentStorage["highscore"][calc.isCampaign ? "campaign" : "scenario"].Vector();
	
	auto sortFunctor = [](const JsonNode & left, const JsonNode & right)
	{
		if(left["points"].Integer() == right["points"].Integer())
			return left["posFlag"].Bool() > right["posFlag"].Bool();
		return left["points"].Integer() > right["points"].Integer();
	};

	JsonNode newNode = JsonNode();
	newNode["player"].String() = text;
	if(calc.isCampaign)
		newNode["campaignName"].String() = calc.calculate().cheater ? LIBRARY->generaltexth->translate("core.genrltxt.260") : calc.parameters[0].campaignName;
	else
		newNode["scenarioName"].String() = calc.calculate().cheater ? LIBRARY->generaltexth->translate("core.genrltxt.260") : calc.parameters[0].scenarioName.toString(&GAME->translator());
	newNode["days"].Integer() = calc.calculate().sumDays;
	newNode["points"].Integer() = calc.calculate().cheater ? 0 : calc.calculate().total;
	newNode["datetime"].String() = TextOperations::getFormattedDateTimeLocal(std::time(nullptr));
	newNode["posFlag"].Bool() = true;
	if(!calc.isCampaign)
	{
		JsonSerializer ser(nullptr, newNode);
		ser.serializeStruct("statistic", stat);
	}

	int highscoreEntriesCap = settings["general"]["highscoreEntriesCap"].Integer();
	if (baseNode.size() > highscoreEntriesCap - 1)
		baseNode.resize(highscoreEntriesCap - 1);

	baseNode.push_back(newNode);
	std::ranges::sort(baseNode, sortFunctor);

	int pos = -1;
	for (int i = 0; i < baseNode.size(); i++)
	{
		if(!baseNode[i]["statistic"].isNull() && i >= settings["general"]["highscoreStatisticEntriesCap"].Integer() && baseNode[i]["posFlag"].isNull())
			baseNode[i]["statistic"].clear();

		if(!baseNode[i]["posFlag"].isNull())
		{
			baseNode[i]["posFlag"].clear();
			pos = i;
		}
	}

	Settings s = persistentStorage.write["highscore"][calc.isCampaign ? "campaign" : "scenario"];
	s->Vector() = baseNode;

	return pos;
}

void CHighScoreInputScreen::show(Canvas & to)
{
	CWindowObject::showAll(to);
}

void CHighScoreInputScreen::showAll(Canvas & to)
{
	to.fillTexture(ENGINE->renderHandler().loadImage(ImagePath::builtin("DiBoxBck"), EImageBlitMode::OPAQUE));
	CWindowObject::showAll(to);
}

void CHighScoreInputScreen::clickPressed(const Point & cursorPosition)
{
	if(statisticButton && statisticButton->pos.isInside(cursorPosition))
		return;

	OBJECT_CONSTRUCTION;

	if(!won)
	{
		stopMusicAndClose();
		return;
	}

	if(!input)
	{
		input = std::make_shared<CHighScoreInput>(calc.parameters[0].playerName,
		[this] (std::string text)
		{
			if(!text.empty())
			{
				int pos = addEntry(text);
				close();
				ENGINE->windows().createAndPushWindow<CHighScoreScreen>(calc.isCampaign ? CHighScoreScreen::HighScorePage::CAMPAIGN : CHighScoreScreen::HighScorePage::SCENARIO, pos);
			}
			else
				stopMusicAndClose();
		});
	}
}

void CHighScoreInputScreen::keyPressed(EShortcut key)
{
	if(key == EShortcut::HIGH_SCORES_STATISTICS) // ignore shortcut for skipping video with key
		return;
	clickPressed(Point());
}

CHighScoreInput::CHighScoreInput(std::string playerName, std::function<void(std::string text)> readyCB)
	: CWindowObject(NEEDS_ANIMATED_BACKGROUND, ImagePath::builtin("HIGHNAME")), ready(readyCB)
{
	OBJECT_CONSTRUCTION;

	pos = center(Rect(0, 0, 232, 212));
	updateShadow();

	text = std::make_shared<CMultiLineLabel>(Rect(15, 15, 202, 202), FONT_SMALL, ETextAlignment::TOPCENTER, Colors::WHITE, LIBRARY->generaltexth->translate("core.genrltxt.96"));

	buttonOk = std::make_shared<CButton>(Point(26, 142), AnimationPath::builtin("MUBCHCK.DEF"), LIBRARY->generaltexth->zelp[560], std::bind(&CHighScoreInput::okay, this), EShortcut::GLOBAL_ACCEPT);
	buttonCancel = std::make_shared<CButton>(Point(142, 142), AnimationPath::builtin("MUBCANC.DEF"), LIBRARY->generaltexth->zelp[561], std::bind(&CHighScoreInput::abort, this), EShortcut::GLOBAL_CANCEL);
// FIXME: broken. Never activates?
//	statusBar = CGStatusBar::create(std::make_shared<CPicture>(background->getSurface(), Rect(7, 186, 218, 18), 7, 186));
	textInput = std::make_shared<CTextInput>(Rect(18, 104, 200, 25), FONT_SMALL, ETextAlignment::CENTER, true);
	textInput->setText(playerName);
}

void CHighScoreInput::okay()
{
	ready(textInput->getText());
}

void CHighScoreInput::abort()
{
	ready("");
}
