/*
 * CCastleInterface.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "CWindowObject.h"
#include "../widgets/Images.h"

class CBuilding;
class CGTownInstance;
class CSpell;
struct CStructure;
class CGHeroInstance;
class CCreature;

class CButton;
class CCastleBuildings;
class CCreaturePic;
class CGStatusBar;
class CLabel;
class CMinorResDataBar;
class CPicture;
class CResDataBar;
class CTextBox;
class CTownList;
class CGarrisonInt;
class CComponent;
class CComponentBox;
class LRClickableArea;
class LRClickableAreaWText;
class CTextInputWithConfirm;

/// Building "button"
class CBuildingRect : public CShowableAnim
{
	std::string getSubtitle();
public:
	enum EBuildingCreationAnimationPhases : uint32_t
	{
		BUILDING_APPEAR_TIMEPOINT = 500, //500 msec building appears: 0->100% transparency
		BUILDING_WHITE_BORDER_TIMEPOINT = 900, //400 msec border glows from white to yellow
		BUILDING_YELLOW_BORDER_TIMEPOINT = 1100, //200 msec border glows from yellow to normal (dark orange)
		BUILD_ANIMATION_FINISHED_TIMEPOINT = 2100, // 1000msec once border is back to yellow nothing happens (this stage is basically removed by HD Mod)

		BUILDING_FRAME_TIME = 150 // confirmed H3 timing: 150 ms for each building animation frame
	};

	/// returns building associated with this structure
	const CBuilding * getBuilding();

	CCastleBuildings * owner;
	const CGTownInstance * town;
	const CStructure* str;
	std::shared_ptr<IImage> border;
	std::shared_ptr<IImage> area;
	std::shared_ptr<IImage> highlight; // 选中光晕（structure 的 highlight 字段），有它时悬停不再画原版描边

	ui32 stateTimeCounter;//For building construction - current stage in animation

	CBuildingRect(CCastleBuildings * Par, const CGTownInstance *Town, const CStructure *Str);
	bool operator<(const CBuildingRect & p2) const;
	void hover(bool on) override;
	void clickPressed(const Point & cursorPosition) override;
	void showPopupWindow(const Point & cursorPosition) override;
	void mouseMoved (const Point & cursorPosition, const Point & lastUpdateDistance) override;
	bool receiveEvent(const Point & position, int eventType) const override;
	void tick(uint32_t msPassed) override;
	void show(Canvas & to) override;
	void showAll(Canvas & to) override;
};

/// Dwelling info box - right-click screen for dwellings
class CDwellingInfoBox : public CWindowObject
{
	std::shared_ptr<CLabel> title;
	std::shared_ptr<CCreaturePic> animation;
	std::shared_ptr<CLabel> available;
	std::shared_ptr<CLabel> costPerTroop;

	std::vector<std::shared_ptr<CAnimImage>> resPicture;
	std::vector<std::shared_ptr<CLabel>> resAmount;
public:
	CDwellingInfoBox(int centerX, int centerY, const CGTownInstance * Town, int level);
	~CDwellingInfoBox();
};

class HeroSlots;
/// Hero icon slot
class CHeroGSlot : public CIntObject
{
	std::shared_ptr<CAnimImage> portrait;
	std::shared_ptr<CAnimImage> flag;
	std::shared_ptr<CAnimImage> selection; //selection border. nullptr if not selected

	HeroSlots * owner;
	const CGHeroInstance * hero;
	int upg; //0 - up garrison, 1 - down garrison

public:
	CHeroGSlot(int x, int y, int updown, const CGHeroInstance *h, HeroSlots * Owner);
	~CHeroGSlot();

	bool isSelected() const;

	void setHighlight(bool on);
	void set(const CGHeroInstance * newHero);

	void hover (bool on) override;
	void gesture(bool on, const Point & initialPosition, const Point & finalPosition) override;
	void clickPressed(const Point & cursorPosition) override;
	void showPopupWindow(const Point & cursorPosition) override;
	void deactivate() override;
};

/// Two hero slots that can interact with each other
class HeroSlots : public CIntObject
{
public:
	bool showEmpty;
	const CGTownInstance * town;

	std::shared_ptr<CGarrisonInt> garr;
	std::shared_ptr<CHeroGSlot> garrisonedHero;
	std::shared_ptr<CHeroGSlot> visitingHero;

	HeroSlots(const CGTownInstance * town, Point garrPos, Point visitPos, std::shared_ptr<CGarrisonInt> Garrison, bool ShowEmpty);
	~HeroSlots();

	void update();
	void swapArmies(); //exchange garrisoned and visiting heroes or move hero to\from garrison
};

/// Class for town screen management (town background and structures)
class CCastleBuildings : public CIntObject
{
	std::shared_ptr<CPicture> background;
	//List of buildings and structures that can represent them
	std::map<BuildingID, std::vector<const CStructure *> > groups;
	// actual IntObject's visible on screen
	std::vector<std::shared_ptr<CBuildingRect>> buildings;

	const CGTownInstance * town;

	const CGHeroInstance* getHero();//Select hero for buildings usage

	void enterBlacksmith(BuildingID building, ArtifactID artifactID);//support for blacksmith + ballista yard
	void enterBuilding(BuildingID building);//for buildings with simple description + pic left-click messages
	void enterCastleGate(BuildingID building);
	void enterFountain(const BuildingID & building, BuildingSubID::EBuildingSubID subID, BuildingID upgrades);//Rampart's fountains
	
	void openMagesGuild();
	void openTownHall();

	void recreate();

	// 建筑列表选中的建筑：画面其余部分压暗，选中建筑加呼吸光晕；未建建筑在原址显示半透明虚影
	const CStructure * pinnedStructure;
	std::shared_ptr<IImage> pinnedGlow; // 光晕图；没有 highlight 时退回原版描边
	std::shared_ptr<CShowableAnim> pinnedGhost; // 未建建筑的虚影，不作为子控件，由 drawHighlights 手动绘制
	uint32_t highlightTime; // 呼吸动画计时（毫秒）
	Rect blockedArea; // 被浮层列表遮住的区域，鼠标在这里时不响应建筑

	std::shared_ptr<CBuildingRect> findRect(const CStructure * structure) const;
	void drawGlow(Canvas & to, const Rect & buildingPos, const std::shared_ptr<IImage> & image, uint8_t alpha) const;
	void drawHighlights(Canvas & to);
	void drawOverlays(Canvas & to, std::vector<std::shared_ptr<CBuildingRect>> buildingRects);
	void show(Canvas & to) override;
	void showAll(Canvas & to) override;
	void tick(uint32_t msPassed) override;
	void clickPressed(const Point & cursorPosition) override;
public:
	CBuildingRect * selectedBuilding;

	/// 列表点击后固定选中某个结构；nullptr 表示取消
	void pinStructure(const CStructure * structure);
	const CStructure * getPinnedStructure() const;
	/// 城镇画面中代表该建筑（或同一升级链）的结构；没有显示时返回 nullptr
	const CStructure * displayedStructure(const CBuilding * building) const;
	/// 在城镇画面里点击时清除固定选中，通知列表同步
	std::function<void()> onPinCleared;
	void setBlockedArea(const Rect & area);
	bool isBlocked(const Point & cursorPosition) const;

	CCastleBuildings(const CGTownInstance * town);
	~CCastleBuildings();

	void enterDwelling(int level);
	void enterTownHall();
	void enterRewardable(BuildingID building);
	void enterMagesGuild();
	void enterAnyMarket();
	void enterAnyThievesGuild();
	void enterBank(BuildingID building);
	void enterToTheQuickRecruitmentWindow();

	bool buildingTryActivateCustomUI(BuildingID buildingToTest, BuildingID buildingTarget);
	void buildingClicked(BuildingID building);
	void addBuilding(BuildingID building);
	void removeBuilding(BuildingID building);//FIXME: not tested!!!
};

/// 城镇建筑列表：按大殿建造槽位列出建筑，分已建、可建造、未解锁三组。
/// 点一下在城镇画面中高亮对应建筑（未建的显示虚影）；再点一下，已建的进入建筑，未建的打开建造确认框。
/// 屏幕够宽时放在城镇窗口左外侧；不够宽时收成画面左上角的标签，点开后浮在画面上。
class CTownBuildingList : public CIntObject
{
public:
	struct Entry
	{
		std::vector<BuildingID> slot; // 大殿的一个槽位（同一升级链）
		const CBuilding * building = nullptr; // 已建：槽位中最高的已建建筑；未建：下一个可建建筑
		const CBuilding * next = nullptr; // 已建时下一档升级（可能为空）
		EBuildingState state = EBuildingState::FORBIDDEN; // 未建：building 的建造状态；已建：next 的建造状态
		bool built = false;
		const CStructure * structure = nullptr; // 城镇画面中对应的结构，可能为空
	};

private:
	class CItem : public CIntObject
	{
		CTownBuildingList * owner;
		size_t index;
		std::shared_ptr<CLabel> name;
		std::shared_ptr<CLabel> badge;
	public:
		CItem(CTownBuildingList * owner, size_t index, const Rect & position);
		void hover(bool on) override;
		void clickPressed(const Point & cursorPosition) override;
		void showPopupWindow(const Point & cursorPosition) override;
		void showAll(Canvas & to) override;
	};

	class CToggle : public CIntObject
	{
		CTownBuildingList * owner;
		std::shared_ptr<CLabel> label;
	public:
		CToggle(CTownBuildingList * owner, const Rect & position);
		void clickPressed(const Point & cursorPosition) override;
		void hover(bool on) override;
		void showAll(Canvas & to) override;
	};

	const CGTownInstance * town;
	CCastleBuildings * scene;
	bool overlayMode; // true：浮层模式（屏幕不够宽）
	bool expanded;
	int selected; // 当前选中的条目序号，-1 表示没有
	Rect panelRect; // 列表底板（绝对坐标）

	std::vector<Entry> entries;
	std::shared_ptr<CLabel> title;
	std::vector<std::shared_ptr<CLabel>> headers;
	std::vector<std::shared_ptr<CItem>> items;
	std::shared_ptr<CToggle> toggle;

	void collectEntries();
	void rebuildItems();
	void select(int index);
	void activateEntry(int index);
	std::string statusText(const Entry & entry) const;

public:
	static constexpr int PANEL_WIDTH = 150;
	static constexpr int PANEL_GAP = 8;

	/// panel：列表底板位置（绝对坐标）；overlay：是否浮层模式
	CTownBuildingList(const CGTownInstance * town, CCastleBuildings * scene, const Rect & panel, bool overlay);

	/// 建筑或资源变化后重新计算状态
	void update();
	void setExpanded(bool on);
	void onPinCleared();
	void activate() override;
	void showAll(Canvas & to) override;
	void show(Canvas & to) override;
};

/// Creature info window
class CCreaInfo : public CIntObject
{
	const CGTownInstance * town;
	CreatureID creature;
	int level;
	bool showAvailable;

	std::shared_ptr<CAnimImage> picture;
	std::shared_ptr<CLabel> label;

	std::string genGrowthText();

public:
	CCreaInfo(Point position, const CGTownInstance * Town, int Level, bool compact=false, bool _showAvailable=false);

	void update();
	void hover(bool on) override;
	void clickPressed(const Point & cursorPosition) override;
	void showPopupWindow(const Point & cursorPosition) override;
	bool getShowAvailable();
};

/// Town hall and fort icons for town screen
class CTownInfo : public CIntObject
{
	const CGTownInstance * town;
	const CBuilding * building;
public:
	std::shared_ptr<CAnimImage> picture;
	//if (townHall) hall-capital else fort - castle
	CTownInfo(int posX, int posY, const CGTownInstance * town, bool townHall);

	void hover(bool on) override;
	void showPopupWindow(const Point & cursorPosition) override;
};

/// Class which manages the castle window
class CCastleInterface final : public CStatusbarWindow, public IGarrisonHolder, public IArtifactsHolder
{
	std::shared_ptr<CTextInputWithConfirm> title;
	std::shared_ptr<CLabel> income;
	std::shared_ptr<CAnimImage> icon;

	std::shared_ptr<CPicture> panel;
	std::shared_ptr<CResDataBar> resdatabar;

	std::shared_ptr<CTownInfo> hall;
	std::shared_ptr<CTownInfo> fort;

	std::shared_ptr<CButton> exit;
	std::shared_ptr<CButton> fastTownHall;
	std::shared_ptr<CButton> fastArmyPurchase;
	std::shared_ptr<LRClickableArea> fastMarket;
	std::shared_ptr<LRClickableArea> fastWiki;
	std::shared_ptr<CIntObject> townRadialArea;

	std::vector<std::shared_ptr<CCreaInfo>> creainfo;//small icons of creatures (bottom-left corner);
	std::shared_ptr<CTownBuildingList> buildingList;

public:
	std::shared_ptr<CTownList> townlist;

	//TODO: move to private
	const CGTownInstance * town;
	std::shared_ptr<HeroSlots> heroes;
	std::shared_ptr<CCastleBuildings> builds;

	std::shared_ptr<CGarrisonInt> garr;

	//from - previously selected castle (if any)
	CCastleInterface(const CGTownInstance * Town, const CGTownInstance * from = nullptr);
	~CCastleInterface();

	void updateArtifacts() override;
	void updateGarrisons() override;
	bool holdsGarrison(const CArmedInstance * army) override;

	void castleTeleport(int where);
	void townChange();
	void keyPressed(EShortcut key) override;

	void close() override;
	void addBuilding(BuildingID bid);
	void removeBuilding(BuildingID bid);
	void recreateIcons();
	void creaturesChangedEventHandler();
};

/// Hall window where you can build things
class CHallInterface : public CStatusbarWindow
{
	class CBuildingBox : public CIntObject
	{
		const CGTownInstance * town;
		const CBuilding * building;

		EBuildingState state;

		std::shared_ptr<CAnimImage> header;
		std::shared_ptr<CAnimImage> icon;
		std::shared_ptr<CAnimImage> mark;
		std::shared_ptr<CLabel> name;
	public:
		CBuildingBox(int x, int y, const CGTownInstance * Town, const CBuilding * Building);
		void hover(bool on) override;
		void clickPressed(const Point & cursorPosition) override;
		void showPopupWindow(const Point & cursorPosition) override;
	};
	const CGTownInstance * town;

	std::vector<std::vector<std::shared_ptr<CBuildingBox>>> boxes;
	std::shared_ptr<CLabel> title;
	std::shared_ptr<CMinorResDataBar> resdatabar;
	std::shared_ptr<CButton> exit;

public:
	CHallInterface(const CGTownInstance * Town);
};

///  Window where you can decide to buy a building or not
class CBuildWindow: public CStatusbarWindow
{
	const CGTownInstance * town;
	const CBuilding * building;

	std::shared_ptr<CAnimImage> icon;
	std::shared_ptr<CLabel> name;
	std::shared_ptr<CTextBox> description;
	std::shared_ptr<CTextBox> stateText;
	std::shared_ptr<CComponentBox> cost;

	std::shared_ptr<CButton> buy;
	std::shared_ptr<CButton> cancel;

	std::string getTextForState(EBuildingState state);
	void buyFunc();
public:
	CBuildWindow(const CGTownInstance *Town, const CBuilding * building, EBuildingState State, bool rightClick);
};

//Small class to display
class LabeledValue : public CIntObject
{
	std::string hoverText;
	std::shared_ptr<CLabel> name;
	std::shared_ptr<CLabel> value;
	void init(std::string name, std::string descr, int min, int max);

public:
	LabeledValue(Rect size, std::string name, std::string descr, int min, int max);
	LabeledValue(Rect size, std::string name, std::string descr, int val);
	void hover(bool on) override;
};

/// The fort screen where you can afford units
class CFortScreen : public CStatusbarWindow
{
	class RecruitArea : public CIntObject
	{
		const CGTownInstance * town;
		int level;

		std::string hoverText;
		std::shared_ptr<CLabel> availableCount;

		std::vector<std::shared_ptr<LabeledValue>> values;
		std::shared_ptr<CPicture> icons;
		std::shared_ptr<CAnimImage> buildingIcon;
		std::shared_ptr<CLabel> buildingName;

		const CCreature * getMyCreature();
		const CBuilding * getMyBuilding();
	public:
		RecruitArea(int posX, int posY, const CGTownInstance *town, int level);

		void creaturesChangedEventHandler();
		void hover(bool on) override;
		void clickPressed(const Point & cursorPosition) override;
		void showPopupWindow(const Point & cursorPosition) override;

	};
	std::shared_ptr<CLabel> title;
	std::vector<std::shared_ptr<RecruitArea>> recAreas;
	std::shared_ptr<CMinorResDataBar> resdatabar;
	std::shared_ptr<CButton> exit;

	ImagePath getBgName(const CGTownInstance * town);

public:
	CFortScreen(const CGTownInstance * town);

	void creaturesChangedEventHandler();
};

class CSpellResearchDialog : public CStatusbarWindow
{
	std::shared_ptr<CLabel> title;
	std::shared_ptr<CTextBox> description;
	std::shared_ptr<CComponentBox> components;
	std::shared_ptr<CButton> acceptButton;
	std::shared_ptr<CButton> rerollButton;
	std::shared_ptr<CButton> closeButton;

public:
	CSpellResearchDialog(
		const std::string & textToShow,
		const std::vector<std::shared_ptr<CComponent>> & components,
		const CGTownInstance * town,
		SpellID oldSpell,
		bool canAfford);
};

/// The mage guild screen where you can see which spells you have
class CMageGuildScreen : public CStatusbarWindow
{
	class Scroll : public CIntObject
	{
		const CSpell * spell;
		std::shared_ptr<CAnimImage> image;
		ObjectInstanceID townId;

	public:
		Scroll(Point position, const CSpell *Spell, ObjectInstanceID townId);
		void clickPressed(const Point & cursorPosition) override;
		void showPopupWindow(const Point & cursorPosition) override;
		void hover(bool on) override;
	};

	class ScrollAllSpells : public CIntObject
	{
		std::shared_ptr<CAnimImage> image;
		std::shared_ptr<LRClickableAreaWText> text;

	public:
		ScrollAllSpells(Point position, const std::string & buildingName);
	};

	std::shared_ptr<CPicture> window;
	std::shared_ptr<CButton> exit;
	std::vector<std::shared_ptr<Scroll>> spells;
	std::vector<std::shared_ptr<CAnimImage>> emptyScrolls;
	std::vector<std::shared_ptr<ScrollAllSpells>> auroraBorealisScrolls;

	std::shared_ptr<CMinorResDataBar> resdatabar;

	ObjectInstanceID townId;

public:
	CMageGuildScreen(CCastleInterface * owner, const ImagePath & image);
	void updateSpells(ObjectInstanceID tID);
};

/// The blacksmith window where you can buy available in town war machine
class CBlacksmithDialog : public CStatusbarWindow
{
	std::shared_ptr<CButton> buy;
	std::shared_ptr<CButton> cancel;
	std::shared_ptr<CPicture> animBG;
	std::shared_ptr<CCreatureAnim> anim;
	std::shared_ptr<CLabel> title;
	std::shared_ptr<CAnimImage> costIcon;
	std::shared_ptr<CLabel> costText;
	std::shared_ptr<CLabel> costValue;

public:
	CBlacksmithDialog(bool possible, ArtifactID newArtifact, ArtifactID existingArtifact, ObjectInstanceID hid);
};
