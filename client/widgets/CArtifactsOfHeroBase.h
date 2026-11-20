/*
 * CArtifactsOfHeroBase.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "CComponentHolder.h"
#include "Scrollable.h"

#include "../gui/Shortcut.h"
#include "../../lib/filesystem/ResourcePath.h"

VCMI_LIB_NAMESPACE_BEGIN
class JsonNode;
VCMI_LIB_NAMESPACE_END

class CButton;
class BackpackScroller;

class CArtifactsOfHeroBase : virtual public CIntObject, public CKeyShortcut
{
protected:
	using ArtPlacePtr = std::shared_ptr<CArtPlace>;
	using BpackScrollFunctor = std::function<void(int)>;

public:
	using ArtPlaceMap = std::map<ArtifactPosition, ArtPlacePtr>;
	using ClickFunctor = std::function<void(CArtPlace&, const Point&)>;

	ClickFunctor clickPressedCallback;
	ClickFunctor showPopupCallback;
	ClickFunctor gestureCallback;
	
	CArtifactsOfHeroBase();
	virtual void putBackPickedArtifact();
	virtual void clickPressedArtPlace(CComponentHolder & artPlace, const Point & cursorPosition);
	virtual void showPopupArtPlace(CComponentHolder & artPlace, const Point & cursorPosition);
	virtual void gestureArtPlace(CComponentHolder & artPlace, const Point & cursorPosition);
	virtual void setHero(const CGHeroInstance * hero);
	virtual const CGHeroInstance * getHero() const;
	virtual void scrollBackpack(bool left);
	virtual void markPossibleSlots(const CArtifact * art, bool assumeDestRemoved = true);
	virtual void unmarkSlots();
	virtual ArtPlacePtr getArtPlace(const ArtifactPosition & slot);
	virtual ArtPlacePtr getArtPlace(const Point & cursorPosition);
	virtual ArtPlacePtr getArtPlace(const CComponentHolder & artPlace);
	virtual void updateWornSlots();
	virtual void updateBackpackSlots();
	virtual void updateSlot(const ArtifactPosition & slot);
	virtual const CArtifactInstance * getPickedArtifact();
	void enableGesture();
	const CArtifactInstance * getArt(const ArtifactPosition & slot) const;
	void enableKeyboardShortcuts();
	void setClickPressedArtPlacesCallback(const CArtPlace::ClickFunctor & callback) const;
	void setShowPopupArtPlacesCallback(const CArtPlace::ClickFunctor & callback) const;

	const CGHeroInstance * curHero;
	ArtPlaceMap artWorn;
	std::vector<ArtPlacePtr> backpack;
	std::shared_ptr<CButton> leftBackpackRoll;
	std::shared_ptr<CButton> rightBackpackRoll;
	std::shared_ptr<BackpackScroller> backpackScroller;

	/// 穿戴槽位置（按 ArtifactPosition 0～18）；修士窗口的布局 JSON 可整组换掉（applyLayout）
	std::vector<Point> slotPos =
	{
		Point(509,30),  Point(568,242), Point(509,80),  //0-2
		Point(383,69),  Point(562,184), Point(509,131), //3-5
		Point(431,69),  Point(610,184), Point(515,295), //6-8
		Point(383,143), Point(399,193), Point(415,244), //9-11
		Point(431,295), Point(564,30),  Point(610,30), //12-14
		Point(610,76),  Point(610,122), Point(610,310), //15-17
		Point(381,295) //18
	};
	/// 储物袋格位置（原版一排 5 格），两个翻页按钮、滚轮区域
	std::vector<Point> backpackPos = {Point(403, 365), Point(449, 365), Point(495, 365), Point(541, 365), Point(587, 365)};
	Point backpackLeftPos = Point(379, 364);
	Point backpackRightPos = Point(632, 364);
	AnimationPath backpackLeftImage = AnimationPath::builtin("hsbtns3.def");
	AnimationPath backpackRightImage = AnimationPath::builtin("hsbtns5.def");
	Rect backpackScrollArea = Rect(380, 30, 278, 382);

public:
	/// 在 init 之前调用：按布局 JSON 换穿戴槽、储物袋格、翻页按钮的位置与图
	/// {"slots": [{x,y} ×19], "backpack": [{x,y}…], "backpackLeft": {x,y,image}, "backpackRight": {…}, "backpackScroll": {x,y,w,h}}
	void applyLayout(const JsonNode & layout);

protected:
	virtual void init(const Point & position, const BpackScrollFunctor & scrollCallback);
	// Assigns an artifacts to an artifact place depending on it's new slot ID
	virtual void setSlotData(ArtPlacePtr artPlace, const ArtifactPosition & slot);
};

class BackpackScroller : public Scrollable
{
	CArtifactsOfHeroBase * owner;

	void scrollBy(int distance) override;

public:
	BackpackScroller(CArtifactsOfHeroBase * owner, const Rect & dimensions);
};
