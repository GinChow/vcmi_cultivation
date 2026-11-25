/*
 * CSpellWindow.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "CWindowObject.h"
#include "../widgets/IVideoHolder.h"

class CGHeroInstance;
class CSpell;

class IImage;
class CAnimImage;
class CPicture;
class CLabel;
class CGStatusBar;
class CPlayerInterface;
class CSpellWindow;
class CTextInput;
class TransparentFilledRectangle;
class CToggleButton;
class VideoWidgetOnce;

/// The spell window
class CSpellWindow : public CWindowObject, public IVideoHolder
{
	class SpellArea : public CIntObject
	{
		const CSpell * mySpell;
		int schoolLevel; //range: 0 none, 3 - expert
		CSpellWindow * owner;
		std::shared_ptr<CAnimImage> image;
		std::shared_ptr<CAnimImage> schoolBorder;
		std::shared_ptr<CLabel> name;
		std::shared_ptr<CLabel> level;
		std::shared_ptr<CLabel> cost;
	public:
		SpellArea(Rect pos, CSpellWindow * owner);
		~SpellArea();
		void setSpell(const CSpell * spell);

		void clickPressed(const Point & cursorPosition) override;
		void showPopupWindow(const Point & cursorPosition) override;
		void hover(bool on) override;
	};

	class InteractiveArea : public CIntObject
	{
		std::function<void()> onLeft;
		CSpellWindow * owner;

		std::string hoverText;
		std::string helpText;
	public:
		void clickPressed(const Point & cursorPosition) override;
		void showPopupWindow(const Point & cursorPosition) override;
		void hover(bool on) override;

		InteractiveArea(const Rect &myRect, const std::function<void()> & funcL, int helpTextId, CSpellWindow * _owner);
		InteractiveArea(const Rect &myRect, const std::function<void()> & funcL, std::string textId, CSpellWindow * _owner);
	};

	std::shared_ptr<CPicture> leftCorner;
	std::shared_ptr<CPicture> rightCorner;

	// clickable page-turn areas
	std::shared_ptr<InteractiveArea> leftCornerArea;
	std::shared_ptr<InteractiveArea> rightCornerArea;

	std::shared_ptr<CAnimImage> schoolTab;
	std::vector<std::shared_ptr<CAnimImage>> schoolTabCustom;
	std::shared_ptr<CPicture> schoolTabAnyDisabled;
	std::shared_ptr<CAnimImage> schoolPicture;
	std::shared_ptr<CPicture> schoolPictureCustom;

	std::array<std::shared_ptr<SpellArea>, 24> spellAreas;
	std::shared_ptr<CLabel> mana;
	std::shared_ptr<CGStatusBar> statusBar;

	std::vector<std::shared_ptr<InteractiveArea>> interactiveAreas;

	std::shared_ptr<CTextInput> searchBox;
	std::shared_ptr<TransparentFilledRectangle> searchBoxRectangle;
	std::shared_ptr<CLabel> searchBoxDescription;

	std::shared_ptr<CToggleButton> showAllSpells;
	std::shared_ptr<CLabel> showAllSpellsDescription;

	std::shared_ptr<VideoWidgetOnce> video;

	/// 换皮用：当前是斗法还是行旅功法的标记图（布局 "modeMarker" 给两张图与位置），没有布局时为空
	std::shared_ptr<CPicture> modeMarker;

	bool isBigSpellbook;
	bool jadeTheme;
	/// 法术格两行字的颜色（可施放 / 灵力不足），原版黄白、玉色主题浅色，模组布局可改
	ColorRGBA spellNameColor;
	ColorRGBA spellDetailColor;
	ColorRGBA spellNameColorUnavailable;
	ColorRGBA spellDetailColorUnavailable;
	int spellsPerPage;
	int offL;
	int offR;
	int offRM;
	int offT;
	int offB;

	std::map<SpellSchool, int> sitesPerTabAdv;
	std::map<SpellSchool, int> sitesPerTabBattle;

	const int MAX_CUSTOM_SPELL_SCHOOLS = 5;
	const int MAX_CUSTOM_SPELL_SCHOOLS_BIG = 6;
	std::vector<SpellSchool> customSpellSchools;

	bool battleSpellsOnly; //if true, only battle spells are displayed; if false, only adventure map spells are displayed
	SpellSchool selectedTab;
	int currentPage; //changes when corners are clicked
	std::vector<const CSpell *> mySpells; //all spels in this spellbook

	const CGHeroInstance * myHero; //hero whose spells are presented
	CPlayerInterface * myInt;

	void processSpells();
	void searchInput();
	void computeSpellsPerArea(); //recalculates spellAreas::mySpell

	void setSchoolImages(SpellSchool school);

	void setCurrentPage(int value);
	void turnPageLeft();
	void turnPageRight();

	void onVideoPlaybackFinished() override;

	bool openOnBattleSpells;
	std::function<void(SpellID)> onSpellSelect; //external processing of selected spell

public:
	CSpellWindow(const CGHeroInstance * _myHero, CPlayerInterface * _myInt, bool openOnBattleSpells = true, const std::function<void(SpellID)> & onSpellSelect = nullptr);
	~CSpellWindow();

	void fexitb();
	/// 把斗法/行旅标记图挪到当前模式上（布局没给时什么都不做）
	void updateModeMarker();
	void fadvSpellsb();
	void fbattleSpellsb();
	void toggleSearchBoxFocus();
	void fmanaPtsb();

	void fLcornerb();
	void fRcornerb();

	void selectSchool(SpellSchool school);
	int pagesWithinCurrentTab();

	void keyPressed(EShortcut key) override;

	void show(Canvas & to) override;
};
