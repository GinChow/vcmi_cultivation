/*
 * BattleTheme.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "../widgets/WidgetLayout.h"
#include "../../lib/json/JsonNode.h"

/// 战斗界面换皮：读 config/widgets/battleWindow2.json 里的 "theme"（模组可整份覆盖这份文件）。
/// "theme": {"images": {键: 图或动画}, "colors": {键: [r, g, b(, a)]}}，没写的键一律用调用处给的原值
/// （jadeTheme 的或原版的），所以没有这一节时界面完全不变。用到的键见各调用处。
namespace BattleTheme
{
inline const JsonNode & config()
{
	return WidgetLayout::cached("config/widgets/BattleWindow2.json");
}

inline ImagePath image(const std::string & key, const std::string & fallback)
{
	return WidgetLayout::image(config()["theme"]["images"][key], ImagePath::builtin(fallback));
}

inline AnimationPath animation(const std::string & key, const std::string & fallback)
{
	return WidgetLayout::animation(config()["theme"]["images"][key], AnimationPath::builtin(fallback));
}

inline ColorRGBA color(const std::string & key, const ColorRGBA & fallback)
{
	return WidgetLayout::color(config()["theme"]["colors"][key], fallback);
}
}
