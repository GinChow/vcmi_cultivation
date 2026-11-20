/*
 * CMessage.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "render/EFont.h"
#include "render/Colors.h"
#include "../../lib/GameConstants.h"
#include "../../lib/filesystem/ResourcePath.h"

struct SDL_Surface;
class CInfoWindow;
class CComponent;
class Canvas;

/// 通用信息弹窗（CInfoWindow、CSelWindow、右键说明文字）的外观。
/// 模组提供 config/widgets/infoWindow.json 时启用（custom = true），否则各项保持原版：
/// 棕色底纹 DIBOXBCK、玩家色边框 DIALGBOX、白字黄色高亮、原版按钮。
struct InfoWindowStyle
{
	bool custom = false;
	/// 平铺在弹窗内容区的底纹
	ImagePath background = ImagePath::builtin("DiBoxBck");
	/// 边框 8 块（与 DIALGBOX 同序：左上、右上、左下、右下、左、右、上、下），边框画在内容区外面这几个像素里
	int marginLeft = 14;
	int marginTop = 15;
	int marginRight = 14;
	int marginBottom = 14;
	/// 四边平铺段伸到四角底下的像素：四角内侧做渐隐，盖在平铺段上交叉过渡，
	/// 平铺段在右、下两端被截在任意位置时也接得上（原版 0：平铺段止于角块边）
	int borderOverlap = 0;
	ColorRGBA textColor = Colors::WHITE;
	/// 正文里 {} 括起来的字
	ColorRGBA highlightColor = Colors::YELLOW;
	/// 按钮动画替换表：原版名（大写、不带扩展名）→ 模组动画
	std::map<std::string, AnimationPath> buttons;

	AnimationPath replaceButton(const AnimationPath & original) const;
};

/// Class which draws formatted text messages and generates chat windows
class CMessage
{
	/// Draw simple dialog box (borders and background only)
	static SDL_Surface * drawDialogBox(int w, int h, PlayerColor playerColor = PlayerColor(1));

public:
	/// Draw border on exiting surface
	static void drawBorder(PlayerColor playerColor, Canvas & to, int w, int h, int x, int y);

	/// 信息弹窗外观（第一次用到时读布局 JSON，此后不变）
	static const InfoWindowStyle & infoWindowStyle();
	/// 画信息弹窗边框：x、y、w、h 是内容区（窗口 pos），边框按外观里的边距画在外面
	static void drawInfoWindowBorder(PlayerColor playerColor, Canvas & to, int x, int y, int w, int h);

	static void drawIWindow(CInfoWindow * ret, std::string text, PlayerColor player);

	/// split text in lines
	static std::vector<std::string> breakText(std::string text, size_t maxLineWidth, EFonts font);

	/// Try to guess a header of a message
	static std::string guessHeader(const std::string & msg);

	/// For convenience
	static int guessHeight(const std::string & string, int width, EFonts fnt);

	static int getEstimatedComponentHeight(int numComps);
	/// constructor
	static void init();
	/// destructor
	static void dispose();
};
