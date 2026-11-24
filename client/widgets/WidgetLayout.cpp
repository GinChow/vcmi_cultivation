/*
 * WidgetLayout.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "WidgetLayout.h"

#include "TextControls.h"
#include "Slider.h"
#include "Buttons.h"

#include "../../lib/filesystem/Filesystem.h"
#include "../../lib/json/JsonNode.h"
#include "../windows/CWindowObject.h"

namespace WidgetLayout
{
JsonNode load(const JsonPath & path)
{
	if(!CResourceHandler::get()->existsResource(path))
		return JsonNode();
	return JsonNode(path);
}

const JsonNode & cached(const std::string & path)
{
	static std::map<std::string, JsonNode> cache;
	auto it = cache.find(path);
	if(it == cache.end())
		it = cache.emplace(path, load(JsonPath::builtin(path))).first;
	return it->second;
}

ColorRGBA remapColor(const JsonNode & colors, const ColorRGBA & original)
{
	if(!colors.isStruct())
		return original;
	static const std::vector<std::pair<std::string, ColorRGBA>> names = {
		{"white", Colors::WHITE}, {"yellow", Colors::YELLOW}, {"orange", Colors::ORANGE}, {"red", Colors::RED},
		{"metallicGold", Colors::METALLIC_GOLD}, {"brightYellow", Colors::BRIGHT_YELLOW}};
	for(const auto & [name, value] : names)
		if(original == value && !colors[name].isNull())
			return color(colors[name], original);
	return original;
}

int windowOptions(const JsonNode & node, int options)
{
	if(!node.isStruct())
		return options;
	if(!node["drawBorder"].isNull() && !node["drawBorder"].Bool())
		options &= ~CWindowObject::BORDERED;
	if(!node["playerColored"].isNull() && !node["playerColored"].Bool())
		options &= ~CWindowObject::PLAYER_COLORED;
	if(!node["statusbarFrame"].isNull() && !node["statusbarFrame"].Bool())
		options &= ~CWindowObject::PLAYER_COLORED_BORDERED_STATUSBAR;
	if(node["alpha"].Bool())
		options |= CWindowObject::BACKGROUND_ALPHA;
	if(!node["shadow"].isNull() && !node["shadow"].Bool())
		options |= CWindowObject::SHADOW_DISABLED;
	return options;
}

Point point(const JsonNode & node, const Point & fallback)
{
	if(!node.isStruct())
		return fallback;
	return Point(node["x"].isNull() ? fallback.x : node["x"].Integer(), node["y"].isNull() ? fallback.y : node["y"].Integer());
}

Rect rect(const JsonNode & node, const Rect & fallback)
{
	if(!node.isStruct())
		return fallback;
	return Rect(node["x"].isNull() ? fallback.x : node["x"].Integer(), node["y"].isNull() ? fallback.y : node["y"].Integer(),
		node["w"].isNull() ? fallback.w : node["w"].Integer(), node["h"].isNull() ? fallback.h : node["h"].Integer());
}

EFonts font(const JsonNode & node, EFonts fallback)
{
	if(!node.isString())
		return fallback;
	static const std::map<std::string, EFonts> names = {
		{"big", FONT_BIG}, {"medium", FONT_MEDIUM}, {"small", FONT_SMALL}, {"tiny", FONT_TINY}, {"calisto", FONT_CALLI}};
	auto it = names.find(node.String());
	return it == names.end() ? fallback : it->second;
}

ETextAlignment alignment(const JsonNode & node, ETextAlignment fallback)
{
	if(!node.isString())
		return fallback;
	if(node.String() == "left")
		return ETextAlignment::TOPLEFT;
	if(node.String() == "center")
		return ETextAlignment::CENTER;
	if(node.String() == "right")
		return ETextAlignment::TOPRIGHT;
	if(node.String() == "centerLeft")
		return ETextAlignment::CENTERLEFT;
	if(node.String() == "centerRight")
		return ETextAlignment::CENTERRIGHT;
	return fallback;
}

ColorRGBA color(const JsonNode & node, const ColorRGBA & fallback)
{
	if(node.isString())
		return Colors::parseColor(node.String()).value_or(fallback);
	if(node.isVector() && node.Vector().size() >= 3)
	{
		const auto & v = node.Vector();
		return ColorRGBA(v[0].Integer(), v[1].Integer(), v[2].Integer(), v.size() > 3 ? v[3].Integer() : 255);
	}
	return fallback;
}

AnimationPath animation(const JsonNode & node, const AnimationPath & fallback)
{
	return node.isString() ? AnimationPath::fromJson(node) : fallback;
}

ImagePath image(const JsonNode & node, const ImagePath & fallback)
{
	return node.isString() ? ImagePath::fromJson(node) : fallback;
}

std::shared_ptr<CLabel> label(const JsonNode & node, const Point & position, EFonts fontValue, ETextAlignment alignmentValue,
	const ColorRGBA & colorValue, const std::string & text, int maxWidth)
{
	const Point at = point(node, position);
	const int width = node["maxWidth"].isNull() ? maxWidth : node["maxWidth"].Integer();
	auto result = std::make_shared<CLabel>(at.x, at.y, font(node["font"], fontValue), alignment(node["alignment"], alignmentValue),
		color(node["color"], colorValue), text, width);
	// "{}" 括起来的字的颜色（原版写死黄色）
	if(!node["highlightColor"].isNull())
		result->highlightColor = color(node["highlightColor"], result->highlightColor);
	return result;
}

void applySliderSkin(const std::shared_ptr<CSlider> & slider, const JsonNode & node)
{
	if(!slider || !node.isStruct())
		return;
	CSlider::Skin result;
	result.track = image(node["track"], ImagePath());
	result.fill = image(node["fill"], ImagePath());
	result.knob = animation(node["knob"], AnimationPath());
	result.left = animation(node["left"], AnimationPath());
	result.right = animation(node["right"], AnimationPath());
	slider->setSkin(result);
}

void applyButton(const std::shared_ptr<CButton> & button, const JsonNode & node)
{
	if(!button || !node.isStruct())
		return;
	if(node["image"].isString())
		button->setImage(AnimationPath::fromJson(node["image"]));
	if(!node["x"].isNull() || !node["y"].isNull())
	{
		const Point origin = button->parent ? button->parent->pos.topLeft() : Point();
		button->moveTo(origin + point(node, button->pos.topLeft() - origin));
	}
	if(node["hidden"].Bool())
		button->disable();
}
}
