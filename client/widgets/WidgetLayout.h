/*
 * WidgetLayout.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "../render/EFont.h"
#include "../render/Colors.h"
#include "../../lib/Point.h"
#include "../../lib/Rect.h"
#include "../../lib/filesystem/ResourcePath.h"

VCMI_LIB_NAMESPACE_BEGIN
class JsonNode;
VCMI_LIB_NAMESPACE_END

class CLabel;
class CSlider;
class CButton;
enum class ETextAlignment;

/// 读窗口布局 JSON（模组换皮用，如 config/widgets/heroWindow.json）的小工具。
/// 每个读取函数都带默认值：JSON 里没写这一项时返回默认值，所以引擎在没有布局文件时保持原版排布。
namespace WidgetLayout
{
/// 模组（或引擎）提供这份布局文件时读出来，否则返回空节点
JsonNode load(const JsonPath & path);

/// 同 load，但每个路径只读一次（窗口反复打开时不重复解析）
const JsonNode & cached(const std::string & path);

/// 原版写死的字色按布局换色：colors 形如 {"white": [r,g,b], "yellow": [...], "orange": ..., "red": ...}，
/// 给的是这几种原版色之一且布局里有对应项时返回布局色，否则原样返回
ColorRGBA remapColor(const JsonNode & colors, const ColorRGBA & original);

/// 窗口选项按布局去掉：{"drawBorder": false} 去掉 BORDERED，{"playerColored": false} 去掉 PLAYER_COLORED，
/// {"statusbarFrame": false} 去掉 PLAYER_COLORED_BORDERED_STATUSBAR，{"alpha": true} 加 BACKGROUND_ALPHA，
/// {"shadow": false} 加 SHADOW_DISABLED（卷轴这类不规则外形不要矩形投影）
int windowOptions(const JsonNode & node, int options);

/// {"x", "y"}
Point point(const JsonNode & node, const Point & fallback);
/// {"x", "y", "w", "h"}
Rect rect(const JsonNode & node, const Rect & fallback);
/// "big" / "medium" / "small" / "tiny" / "calisto"
EFonts font(const JsonNode & node, EFonts fallback);
/// "left"（左上角定位）/ "center"（中心点定位）/ "right"（右上角定位）
ETextAlignment alignment(const JsonNode & node, ETextAlignment fallback);
/// [r, g, b(, a)] 或颜色名
ColorRGBA color(const JsonNode & node, const ColorRGBA & fallback);
AnimationPath animation(const JsonNode & node, const AnimationPath & fallback);
ImagePath image(const JsonNode & node, const ImagePath & fallback);

/// 滑条换皮：{"track","fill","knob","left","right"}，见 CSlider::Skin；node 不是对象时什么都不做
void applySliderSkin(const std::shared_ptr<CSlider> & slider, const JsonNode & node);

/// 按钮换位置、换图：{"x", "y", "image"}，各项缺省时保持按钮原样；"hidden": true 时隐藏按钮
void applyButton(const std::shared_ptr<CButton> & button, const JsonNode & node);

/// 按 {"x", "y", "font", "alignment", "color", "highlightColor", "maxWidth"} 建标签，各项缺省时用给定的原版值
std::shared_ptr<CLabel> label(const JsonNode & node, const Point & position, EFonts font, ETextAlignment alignment,
	const ColorRGBA & color, const std::string & text = "", int maxWidth = 0);
}
