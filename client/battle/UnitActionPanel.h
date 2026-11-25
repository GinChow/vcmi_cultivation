/*
 * UnitActionPanel.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "../gui/CIntObject.h"
#include "../../lib/battle/PossiblePlayerBattleAction.h"
#include "../../lib/filesystem/ResourcePath.h"

class CFilledTexture;
class TransparentFilledRectangle;
class CToggleButton;
class CLabel;
class BattleInterface;

class UnitActionPanel : public CIntObject
{
private:
	std::shared_ptr<CFilledTexture> background;
	std::shared_ptr<TransparentFilledRectangle> rect;
	std::vector<std::shared_ptr<CToggleButton>> buttons;

	BattleInterface & owner;

	void testAndAddAction(const std::vector<PossiblePlayerBattleAction> & allActions, const std::vector<PossiblePlayerBattleAction::Actions> & actionFilter, const ImagePath & iconPath, const std::string & descriptionTextID );
	void testAndAddSpell(const std::vector<PossiblePlayerBattleAction> & allActions, const SpellID & spellFilter );

	void restoreAllActions();
	void setActions(int buttonIndex, const std::vector<PossiblePlayerBattleAction> & newActions);
public:
	static constexpr int ACTION_SLOTS = 12;

	/// height：面板高度，原版 600 占满战斗窗一侧；战斗窗两侧上下叠放时只占修士浮窗下面那一段
	UnitActionPanel(BattleInterface & owner, int height = 600);

	void setPossibleActions(const std::vector<PossiblePlayerBattleAction> & actions);

	std::vector<std::tuple<SpellID, bool>> getSpells() const;

	void show(Canvas & to) override;
};
