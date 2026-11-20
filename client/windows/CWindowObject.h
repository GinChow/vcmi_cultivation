/*
 * CWindowObject.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "../gui/CIntObject.h"
#include "../../lib/filesystem/ResourcePath.h"

class CGStatusBar;

class CWindowObject : public WindowBase
{
	std::vector<std::shared_ptr<CPicture>> shadowParts;

	void setShadow(bool on);

	int options;

protected:
	std::shared_ptr<CPicture> background;

	//Used only if RCLICK_POPUP was set
	bool isPopupWindow() const override;
	//To display border
	void updateShadow();
	void setBackground(const ImagePath & filename);
	std::shared_ptr<CPicture> createBg(const ImagePath & imageName, int windowOptions);
	std::shared_ptr<CPicture> createPlayerColoredBorderedStatusbar(const std::shared_ptr<CPicture> & image, PlayerColor playerColor);
public:
	enum EOptions
	{
		PLAYER_COLORED=1, //background will be player-colored
		RCLICK_POPUP=2, // window will behave as right-click popup
		BORDERED=4, // window will have border if current resolution is bigger than size of window
		SHADOW_DISABLED=8, //this window won't display any shadow
		NEEDS_ANIMATED_BACKGROUND=16, //there are videos in the background that have to be played
		PLAYER_COLORED_BORDERED_STATUSBAR=32, //composite player-colored bordered frame for statusbar dialogs
		BACKGROUND_ALPHA=64 // 底图按透明通道叠在下层画面上（模组的毛边墨带、卷轴等不规则外形）；默认底图当不透明图贴
	};

	/*
	 * options - EOptions enum
	 * imageName - name for background image, can be empty
	 * centerAt - position of window center. Default - center of the screen
	*/
	CWindowObject(int options, const ImagePath & imageName, Point centerAt);
	CWindowObject(int options = 0, const ImagePath & imageName = {});
	~CWindowObject();

	void showAll(Canvas & to) override;
	/// 底图带透明通道（BACKGROUND_ALPHA）的窗口：只重画自己时半透明毛边会叠在上一次的图上越来越深，改为请求整屏重画
	void redraw() override;
};

class CStatusbarWindow : virtual public CWindowObject
{
protected:
	std::shared_ptr<CGStatusBar> statusbar;
};
