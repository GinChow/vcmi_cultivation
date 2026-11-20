/*
 * CAdventureOptions.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */

#include "StdInc.h"
#include "AdventureOptions.h"

#include "../CPlayerInterface.h"
#include "../lobby/CCampaignInfoScreen.h"
#include "../lobby/CScenarioInfoScreen.h"
#include "render/AssetGenerator.h"
#include "render/IRenderHandler.h"
#include "../GameEngine.h"
#include "../GameInstance.h"
#include "../gui/WindowHandler.h"
#include "../gui/Shortcut.h"
#include "../gui/ShortcutHandler.h"
#include "../widgets/Buttons.h"
#include "../widgets/Slider.h"
#include "AdventureMapInterface.h"

#include "../../lib/CConfigHandler.h"
#include "../../lib/GameLibrary.h"
#include "../../lib/StartInfo.h"
#include "../../lib/callback/CCallback.h"
#include "../../lib/texts/CGeneralTextHandler.h"
#include "../../lib/json/JsonNode.h"
#include "../../lib/filesystem/ResourcePath.h"
#include "../widgets/TextControls.h"

namespace
{
/// 布局 JSON 的 "layout" 段（模组用来换整窗外观）：background 窗口底图，不画玩家色、不带矩形阴影
int windowOptions(const JsonNode & layout)
{
	if(!layout["background"].isNull())
		return CWindowObject::SHADOW_DISABLED | CWindowObject::BACKGROUND_ALPHA;
	return CWindowObject::PLAYER_COLORED;
}

ImagePath windowBackground(const JsonNode & layout)
{
	if(!layout["background"].isNull())
		return ImagePath::fromJson(layout["background"]);
	return settings["general"]["enableUiEnhancements"].Bool() ? ImagePath::builtin("AdventureOptionsBackground") : ImagePath::builtin("ADVOPTS");
}
}

AdventureOptions::AdventureOptions()
	: AdventureOptions(JsonNode(JsonPath::builtin("config/widgets/adventureOptions")))
{
}

AdventureOptions::AdventureOptions(const JsonNode & config)
	: CWindowObject(windowOptions(config["layout"]), windowBackground(config["layout"]))
{
	OBJECT_CONSTRUCTION;

	const bool enhanced = settings["general"]["enableUiEnhancements"].Bool();
	const JsonNode & layout = config["layout"];
	if(!layout.isNull())
	{
		// 自定义布局：按钮起点、步长（横排时 y 为 0）、一屏可见个数、是否显示说明文字
		if(!layout["buttonOrigin"].isNull())
			buttonOrigin = Point(layout["buttonOrigin"]["x"].Integer(), layout["buttonOrigin"]["y"].Integer());
		if(!layout["buttonStep"].isNull())
			buttonStep = Point(layout["buttonStep"]["x"].Integer(), layout["buttonStep"]["y"].Integer());
		if(!layout["maxVisible"].isNull())
			maxVisible = std::max<int>(1, layout["maxVisible"].Integer());
		if(!layout["showLabels"].isNull())
			showLabels = layout["showLabels"].Bool();
	}

	// Build shortcut state lookup from the adventure map
	std::map<EShortcut, AdventureMapShortcutState> shortcutStates;
	if(adventureInt)
		for(const auto & s : adventureInt->getAdventureShortcuts().getShortcuts())
			shortcutStates[s.shortcut] = s;

	// Pre-parse all entries from JSON into ButtonEntry records
	for(const JsonNode & entry : config["buttons"].Vector())
	{
		const std::string shortcutId = entry["shortcut"].String();
		const EShortcut   shortcut   = ENGINE->shortcuts().findShortcut(shortcutId);

		AnimationPath animPath;
		if(!entry["image"].isNull())
		{
			// 模组直接给整套按钮动画（不走原版 DEF 和增强模式的生成图）
			animPath = AnimationPath::fromJson(entry["image"]);
		}
		else if(enhanced && !entry["overlay"].isNull())
		{
			const std::string overlayName = entry["overlay"].String();
			animPath = AnimationPath::builtin("SPRITES/" + overlayName);

			auto assetGenerator = ENGINE->renderHandler().getAssetGenerator();
			auto layout = assetGenerator->createAdventureOptionsButton(ImagePath::builtin(overlayName));
			assetGenerator->addAnimationFile(animPath, layout);
			ENGINE->renderHandler().updateGeneratedAssets();
		}
		else
		{
			if(entry["legacyAnimation"].isNull())
				continue;
			animPath = AnimationPath::builtin(entry["legacyAnimation"].String());
		}

		bool isBlocked = false;
		std::function<void()> callback;
		auto it = shortcutStates.find(shortcut);
		if(it != shortcutStates.end())
		{
			isBlocked = !it->second.isEnabled;
			callback  = it->second.callback;
		}
		else
		{
			logGlobal->warn("AdventureOptions: no shortcut state found for '%s'", shortcutId);
		}

		ButtonEntry be;
		be.animPath  = animPath;
		be.shortcut  = shortcut;
		be.isBlocked = isBlocked;
		be.callback  = callback;
		be.labelKey  = (enhanced && showLabels && !entry["label"].isNull()) ? entry["label"].String() : "";
		allEntries.push_back(std::move(be));
	}

	// Restore persisted scroll position
	const int savedScroll = settings["adventure"]["optionsScrollPos"].Integer();
	const int initScroll  = std::clamp(savedScroll, 0, std::max(0, static_cast<int>(allEntries.size()) - maxVisible));

	rebuildVisibleButtons(initScroll, enhanced);

	// Scrollbar – only when there are more buttons than fit
	if(static_cast<int>(allEntries.size()) > maxVisible && !layout["slider"].isNull())
	{
		// 自定义布局的滚动条：位置、长度来自 JSON，横排按钮时横向滚动
		const JsonNode & sl = layout["slider"];
		const bool horizontal = buttonStep.x != 0;
		const int step = horizontal ? buttonStep.x : buttonStep.y;
		scrollBar = std::make_shared<CSlider>(
			Point(sl["position"]["x"].Integer(), sl["position"]["y"].Integer()),
			sl["length"].Integer(),
			[this, enhanced](int val)
			{
				Settings adventure = settings.write["adventure"];
				adventure["optionsScrollPos"].Integer() = val;
				rebuildVisibleButtons(val, enhanced);
			},
			maxVisible,
			static_cast<int>(allEntries.size()),
			initScroll,
			horizontal ? Orientation::HORIZONTAL : Orientation::VERTICAL,
			CSlider::BROWN);
		scrollBar->setPanningStep(step);
		scrollBar->setInertiaEnabled(true);
	}
	else if(static_cast<int>(allEntries.size()) > maxVisible)
	{
		const int sliderY      = 24;
		const int sliderLength = 282;

		scrollBar = std::make_shared<CSlider>(
			Point(252, sliderY),
			sliderLength,
			[this, enhanced](int val)
			{
				Settings adventure = settings.write["adventure"];
				adventure["optionsScrollPos"].Integer() = val;
				rebuildVisibleButtons(val, enhanced);
			},
			MAX_VISIBLE,
			static_cast<int>(allEntries.size()),
			initScroll,
			Orientation::VERTICAL,
			CSlider::BROWN);

		scrollBar->setScrollBounds(Rect(-228, 0, 228 + 16, MAX_VISIBLE * BUTTON_STEP));
		scrollBar->setPanningStep(BUTTON_STEP);
		scrollBar->setInertiaEnabled(true);
	}

	Point exitPos(203, 313);
	AnimationPath exitImage = AnimationPath::builtin("IOK6432.DEF");
	if(!layout["exit"].isNull())
	{
		exitPos = Point(layout["exit"]["position"]["x"].Integer(), layout["exit"]["position"]["y"].Integer());
		exitImage = AnimationPath::fromJson(layout["exit"]["image"]);
	}
	exit = std::make_shared<CButton>(exitPos, exitImage, CButton::tooltip(), std::bind(&AdventureOptions::close, this), EShortcut::GLOBAL_RETURN);
}

void AdventureOptions::rebuildVisibleButtons(int scrollPos, bool enhanced)
{
	buttons.clear();
	buttonLabels.clear();

	OBJECT_CONSTRUCTION;

	const int from = scrollPos;
	const int to   = std::min(from + maxVisible, static_cast<int>(allEntries.size()));

	for(int i = from; i < to; ++i)
	{
		const ButtonEntry & be  = allEntries[i];
		const int slot          = i - from;
		const Point btnPos = buttonOrigin + buttonStep * slot;

		auto btn = std::make_shared<CButton>(btnPos, be.animPath, CButton::tooltip(), [this](){ close(); }, be.shortcut);
		if(be.callback)
			btn->addCallback(be.callback);
		btn->block(be.isBlocked);

		if(!be.labelKey.empty())
		{
			const int margin = 5;
			const int bgX = 78;
			const int bgW = 189 - (enhanced ? 16 : 0);
			const int bgH = 48;
			buttonLabels.push_back(std::make_shared<CMultiLineLabel>(
				Rect(bgX + margin, BUTTON_START_Y + 1 + slot * BUTTON_STEP + margin, bgW - 2 * margin, bgH - 2 * margin),
				FONT_MEDIUM, ETextAlignment::CENTERLEFT, Colors::YELLOW,
				LIBRARY->generaltexth->translate(be.labelKey)));
		}

		buttons.push_back(btn);
	}

	redraw();
}

void AdventureOptions::showScenarioInfo()
{
	if(GAME->interface()->cb->getStartInfo()->campState)
	{
		ENGINE->windows().createAndPushWindow<CCampaignInfoScreen>();
	}
	else
	{
		ENGINE->windows().createAndPushWindow<CScenarioInfoScreen>();
	}
}
