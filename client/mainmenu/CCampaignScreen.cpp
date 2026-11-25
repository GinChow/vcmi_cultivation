/*
 * CCampaignScreen.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */

#include "StdInc.h"
#include "CCampaignScreen.h"

#include "CMainMenu.h"

#include "../CPlayerInterface.h"
#include "../CServerHandler.h"
#include "../GameEngine.h"
#include "../gui/Shortcut.h"
#include "media/IMusicPlayer.h"
#include "render/Canvas.h"
#include "../widgets/CComponent.h"
#include "../widgets/Buttons.h"
#include "../widgets/MiscWidgets.h"
#include "../widgets/ObjectLists.h"
#include "../widgets/TextControls.h"
#include "../widgets/VideoWidget.h"
#include "../widgets/WidgetLayout.h"
#include "../windows/GUIClasses.h"
#include "../windows/InfoWindows.h"
#include "../windows/CWindowObject.h"

#include "../../lib/CConfigHandler.h"
#include "../../lib/CCreatureHandler.h"
#include "../../lib/CSkillHandler.h"
#include "../../lib/GameLibrary.h"
#include "../../lib/IGameSettings.h"
#include "../../lib/campaign/CampaignHandler.h"
#include "../../lib/filesystem/Filesystem.h"
#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/mapping/CMapService.h"
#include "../../lib/texts/CGeneralTextHandler.h"
#include "../../lib/texts/CompositeTranslator.h"
#include "../GameInstance.h"

/// 征途选择布局（模组换皮用，如 cultivation-ui 的 config/widgets/menuScreens.json 里的 "campaignScreen" 节）：
/// 战役按钮的可点范围、悬停时的光框图、是否显示原版黄色悬停名、悬停时底部的简介卷（名字 + 战役简介）。
/// 背景、各战役图与位置仍由 config/campaignSets.json 给（模组可整体覆盖）。没有布局文件时全按原版。
static const JsonNode & campaignLayout()
{
	return WidgetLayout::cached("config/widgets/menuScreens.json")["campaignScreen"];
}

CCampaignScreen::CCampaignScreen(const JsonNode & config, std::string name)
	: CWindowObject(WidgetLayout::windowOptions(campaignLayout()["window"], BORDERED)), campaignSet(name)
{
	OBJECT_CONSTRUCTION;
	
	const auto& campaigns = config[name]["items"].Vector();

	// Define mapping of background name -> campaigns per page
	const std::unordered_map<std::string, int> campaignsPerPageMap = {
		{"CampaignBackground4", 4},
		{"CampaignBackground5", 5},
		{"CampaignBackground6", 6},
		{"CampaignBackground7", 7},
		{"CAMPBACK", 7},
		{"CAMPBKX2", 7},
		{"CampaignBackground8", 8}
	};

	// Process images and check if name is in the map
	for (const JsonNode& node : config[name]["images"].Vector())
	{
		images.push_back(CMainMenu::createPicture(node));

		std::string imageName = node["name"].String();
		auto it = campaignsPerPageMap.find(imageName);
		if (it != campaignsPerPageMap.end())
		{
			campaignsPerPage = it->second;
		}
	}

	if (!images.empty())
	{
		images[0]->center(); // move background to center
		moveTo(images[0]->pos.topLeft()); // move everything else to center
		images[0]->moveTo(pos.topLeft()); // restore moved twice background
		pos = images[0]->pos; // fix height\width of this window
	}
	
	for (const auto& node : campaigns)
	{
		auto button = std::make_shared<CCampaignButton>(node, config, campaignSet,
			[this](const std::string & title, const std::string & description, bool on){ showCampaignInfo(title, description, on); });
		button->enable();
		campButtons.push_back(button);
	}

	// 悬停时底部的简介卷：底图、战役名、简介正文，平时隐藏
	const auto & info = campaignLayout()["info"];
	if(info.isStruct())
	{
		infoBackground = std::make_shared<CPicture>(WidgetLayout::image(info["image"], ImagePath()), WidgetLayout::point(info, Point()));
		infoTitle = WidgetLayout::label(info["title"], Point(), FONT_BIG, ETextAlignment::CENTER, Colors::YELLOW);
		const auto & textNode = info["text"];
		infoText = std::make_shared<CMultiLineLabel>(WidgetLayout::rect(textNode, Rect()), WidgetLayout::font(textNode["font"], FONT_SMALL),
			WidgetLayout::alignment(textNode["alignment"], ETextAlignment::TOPCENTER), WidgetLayout::color(textNode["color"], Colors::WHITE), "");
		showCampaignInfo("", "", false);
	}

	maxPages = (campaigns.size() + campaignsPerPage - 1) / campaignsPerPage;
	
	if (!config[name]["nextbutton"].isNull())
	{
		buttonNext = std::make_shared<CButton>(
			Point(config[name]["nextbutton"]["x"].Integer(), config[name]["nextbutton"]["y"].Integer()),
			AnimationPath::fromJson(config[name]["nextbutton"]["name"]),
			std::make_pair("", ""),
			[this, name]() { switchPage(1); }
		);
		buttonNext->setHoverable(true);
		buttonNext->disable();
	}

	if (!config[name]["backbutton"].isNull())
	{
		buttonPrev = std::make_shared<CButton>(
			Point(config[name]["backbutton"]["x"].Integer(), config[name]["backbutton"]["y"].Integer()),
			AnimationPath::fromJson(config[name]["backbutton"]["name"]),
			std::make_pair("", ""),
			[this, name]() { switchPage(-1); }
		);
		buttonPrev->setHoverable(true);
		buttonPrev->disable();
	}

	page = std::make_shared<CLabel>(10, 570, FONT_MEDIUM, ETextAlignment::BOTTOMLEFT, Colors::YELLOW, "");

	if (!config[name]["exitbutton"].isNull())
	{
		buttonBack = createExitButton(config[name]["exitbutton"]);
		buttonBack->setHoverable(true);
	}

	updateCampaignButtons(config);
}

void CCampaignScreen::activate()
{
	ENGINE->music().playMusic(AudioPath::builtin("Music/MainMenu"), true, false);

	CWindowObject::activate();
}

std::shared_ptr<CButton> CCampaignScreen::createExitButton(const JsonNode & button)
{
	std::pair<std::string, std::string> help;
	if(!button["help"].isNull() && button["help"].Float() > 0)
		help = LIBRARY->generaltexth->zelp[(size_t)button["help"].Float()];

	return std::make_shared<CButton>(Point((int)button["x"].Float(), (int)button["y"].Float()), AnimationPath::fromJson(button["name"]), help, [this](){ close();}, EShortcut::GLOBAL_CANCEL);
}

void CCampaignScreen::showCampaignInfo(const std::string & title, const std::string & description, bool on)
{
	if(!infoBackground)
		return;

	for(CIntObject * widget : std::initializer_list<CIntObject *>{infoBackground.get(), infoTitle.get(), infoText.get()})
	{
		if(on)
			widget->enable();
		else
			widget->disable();
	}
	if(on)
	{
		infoTitle->setText(title);
		infoText->setText(description);
	}
	redraw();
}

CCampaignScreen::CCampaignButton::CCampaignButton(const JsonNode & config, const JsonNode & parentConfig, std::string campaignSet, HoverCallback onHover)
	: campaignSet(campaignSet), onHover(std::move(onHover))
{
	OBJECT_CONSTRUCTION;

	const auto & layout = campaignLayout();
	pos.x += static_cast<int>(config["x"].Float());
	pos.y += static_cast<int>(config["y"].Float());
	// 原版战役图 200×116；换皮后（如竖幅挂轴）按布局给的可点范围
	pos.w = layout["buttonSize"]["w"].isNumber() ? static_cast<int>(layout["buttonSize"]["w"].Integer()) : 200;
	pos.h = layout["buttonSize"]["h"].isNumber() ? static_cast<int>(layout["buttonSize"]["h"].Integer()) : 116;

	campFile = config["file"].String();
	videoPath = VideoPath::fromJson(config["video"]);

	status = CCampaignScreen::ENABLED;

	if(CResourceHandler::get()->existsResource(ResourcePath(campFile, EResType::CAMPAIGN)))
	{
		auto header = CampaignHandler::getHeader(campFile);
		// the header is local to this scope, so its own texts are the only place its name lives
		CompositeTranslator translator;
		translator.install(header->getTexts());
		hoverText = header->getNameTranslated(&translator);
		descriptionText = header->getDescriptionTranslated(&translator);

		if (persistentStorage["completedCampaigns"][header->getFilename()].Bool())
			status = CCampaignScreen::COMPLETED;
	}
	else
	{
		status = CCampaignScreen::DISABLED;
	}

	for(const JsonNode & node : parentConfig[campaignSet]["items"].Vector())
	{
		for(const JsonNode & requirement : config["requires"].Vector())
		{
			if(node["id"].Integer() == requirement.Integer())
				if(!persistentStorage["completedCampaigns"][node["file"].String()].Bool())
					status = CCampaignScreen::DISABLED;
		}
	}

	if(LIBRARY->engineSettings()->getBoolean(EGameSettings::CAMPAIGN_UNLOCK_ALL))
		status = CCampaignScreen::ENABLED;

	if(status != CCampaignScreen::DISABLED)
	{
		addUsedEvents(LCLICK | HOVER);
		graphicsImage = std::make_shared<CPicture>(ImagePath::fromJson(config["image"]));
		// 布局 "hoverLabel": false 时不要原版图下方的黄色战役名（换皮后名字画在图上，悬停简介另有简介卷）
		if(!layout["hoverLabel"].isBool() || layout["hoverLabel"].Bool())
		{
			hoverLabel = std::make_shared<CLabel>(pos.w / 2, pos.h + 20, FONT_MEDIUM, ETextAlignment::CENTER, Colors::YELLOW, "");
			parent->addChild(hoverLabel.get());
		}
		// 悬停光框：画在按钮外圈，换下时要整窗重画才擦得掉，所以按钮重画一律交给窗口
		const auto & glow = layout["hoverImage"];
		if(glow["image"].isString())
		{
			hoverImage = std::make_shared<CPicture>(WidgetLayout::image(glow["image"], ImagePath()), WidgetLayout::point(glow, Point()));
			hoverImage->disable();
			setRedrawParent(true);
		}
	}

	if(status == CCampaignScreen::COMPLETED)
		graphicsCompleted = std::make_shared<CPicture>(ImagePath::builtin("CAMPCHK"));
}

void CCampaignScreen::CCampaignButton::clickReleased(const Point & cursorPosition)
{
	CMainMenu::openCampaignLobby(campFile, campaignSet);
}

void CCampaignScreen::CCampaignButton::hover(bool on)
{
	OBJECT_CONSTRUCTION;

	if (on && !videoPath.empty())
		videoPlayer = std::make_shared<VideoWidget>(Point(), videoPath, false);
	else
		videoPlayer.reset();

	if(hoverImage)
	{
		if(on)
			hoverImage->enable();
		else
			hoverImage->disable();
	}
	if(onHover)
		onHover(hoverText, descriptionText, on);
	if(hoverImage)
		redraw();

	if(hoverLabel)
	{
		if(on)
			hoverLabel->setText(hoverText); // Shows the name of the campaign when you get into the bounds of the button
		else
			hoverLabel->setText(" ");
	}
}

void CCampaignScreen::switchPage(int delta)
{
	currentPage += delta;
	currentPage = std::clamp(currentPage, 0, maxPages - 1);

	const auto& campaignConfig = CMainMenuConfig::get().getCampaigns();

	updateCampaignButtons(campaignConfig);
}

void CCampaignScreen::updateCampaignButtons(const JsonNode & parentConfig)
{
	const auto& campaigns = parentConfig[campaignSet]["items"].Vector();

	int minId = (currentPage * campaignsPerPage) + 1;
	int maxId = minId + campaignsPerPage - 1;

	for(size_t i = 0; i < campButtons.size(); ++i)
	{
		int campaignId = campaigns[i]["id"].Integer();

		if(campaignId >= minId && campaignId <= maxId)
			campButtons[i]->enable();
		else
			campButtons[i]->disable();

		if(!CResourceHandler::get()->existsResource(ResourcePath(campaigns[i]["file"].String(), EResType::CAMPAIGN)))
		{
			campButtons[i]->disable();
			logGlobal->warn("Campaign %s doesn't exist", campaigns[i]["file"].String());
		}
	}

	if(buttonNext && buttonPrev)
	{
		page->setText(std::to_string(currentPage + 1) + "/" + std::to_string(maxPages));

		if (maxId < campaigns.size())
			buttonNext->enable();
		else
			buttonNext->disable();

		if (currentPage > 0)
			buttonPrev->enable();
		else
			buttonPrev->disable();
	}

	redraw();
}
