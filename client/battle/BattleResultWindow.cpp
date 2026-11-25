/*
 * BattleResultWindow.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "BattleResultWindow.h"

#include "BattleWindow.h"
#include "BattleTheme.h"

#include "../GameInstance.h"
#include "../Client.h"
#include "../CServerHandler.h"
#include "../CPlayerInterface.h"
#include "../GameEngine.h"
#include "../gui/Shortcut.h"
#include "../gui/WindowHandler.h"
#include "media/IMusicPlayer.h"
#include "../widgets/Buttons.h"
#include "../widgets/Images.h"
#include "../widgets/TextControls.h"
#include "../widgets/VideoWidget.h"

#include "../../lib/CStack.h"
#include "../../lib/CPlayerState.h"
#include "../../lib/ConditionalWait.h"
#include "../../lib/GameLibrary.h"
#include "../../lib/StartInfo.h"
#include "../../lib/battle/CPlayerBattleCallback.h"
#include "../../lib/callback/CCallback.h"
#include "../../lib/gameState/InfoAboutArmy.h"
#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/networkPacks/PacksForClientBattle.h"
#include "../../lib/texts/CGeneralTextHandler.h"

/// 斗法结算窗的模组布局：battleWindow2.json 的 "result" 节，没有时为空节点、一律按原版。
/// "background" 底图（给了就不再按玩家色染）；"exit"/"repeat" 按钮 {x, y, image}，"buttonBorder": false 不描边；
/// "applyLabel"、"sideResult": [左, 右]、"names": [左, 右]、"casualtiesTitle"、"attackerLabel"、"defenderLabel" 标签（"hidden": true 不显示）；
/// "winColor"/"loseColor" 胜负字色；"portraits": [左, 右]；"casualties": [{x 中心, y, w 最宽}, …]，"casualtyFrame" 每个兵头像下的框图与
/// "casualtyFrameOffset"，"countColor"、"emptyColor"；"description": {x, y, w, h, color, "compact": true 合并空行, "slider": 滑条换皮}；
/// "video": false 不放原版胜负动画；
/// "artwork": {"victory", "defeat", x, y} 按本方胜负换题图
static const JsonNode & resultLayout()
{
	return BattleTheme::config()["result"];
}

/// 按布局建标签：节点 "hidden": true 时不建（返回空）
static std::shared_ptr<CLabel> resultLabel(const JsonNode & node, const Point & position, EFonts font, ETextAlignment alignment, const ColorRGBA & color, const std::string & text)
{
	if(node["hidden"].Bool())
		return nullptr;
	return WidgetLayout::label(node, position, font, alignment, WidgetLayout::remapColor(resultLayout()["colors"], color), text);
}

static const JsonNode & resultSide(const JsonNode & node, int side)
{
	static const JsonNode empty;
	if(node.isVector() && side < node.Vector().size())
		return node.Vector()[side];
	return empty;
}

BattleResultWindow::BattleResultWindow(const BattleResult & br, CPlayerInterface & _owner, bool allowReplay)
	: owner(_owner)
{
	OBJECT_CONSTRUCTION;
	const bool jadeTheme = JsonNode(JsonPath::builtin("config/widgets/BattleWindow2.json"))["jadeTheme"].Bool();
	const JsonNode & layout = resultLayout();
	auto addLabel = [this](const std::shared_ptr<CLabel> & label)
	{
		if(label)
			labels.push_back(label);
	};

	background = std::make_shared<CPicture>(WidgetLayout::image(layout["background"], ImagePath::builtin("CPRESULT")));
	if(layout["background"].isNull())
		background->setPlayerColor(owner.playerID);
	pos = center(background->pos);

	const bool buttonBorder = layout["buttonBorder"].isNull() || layout["buttonBorder"].Bool();
	const std::optional<ColorRGBA> borderColor = buttonBorder ? std::optional<ColorRGBA>(jadeTheme ? ColorRGBA(154, 176, 161) : Colors::METALLIC_GOLD) : std::nullopt;

	exit = std::make_shared<CButton>(Point(384, 505), AnimationPath::builtin("iok6432.def"), std::make_pair("", ""), [this](){ bExitf();}, EShortcut::GLOBAL_ACCEPT);
	exit->setBorderColor(borderColor);
	WidgetLayout::applyButton(exit, layout["exit"]);

	auto battle = owner.cb->getBattle(br.battleID);
	const auto * attackerPlayer = GAME->server().client->gameInfo().getPlayerState(battle->sideToPlayer(BattleSide::ATTACKER));
	const auto * defenderPlayer = GAME->server().client->gameInfo().getPlayerState(battle->sideToPlayer(BattleSide::DEFENDER));
	bool isAttackerHuman = attackerPlayer && attackerPlayer->isHuman();
	bool isDefenderHuman = defenderPlayer && defenderPlayer->isHuman();
	bool onlyOnePlayerHuman = isAttackerHuman != isDefenderHuman;

	if((allowReplay || owner.cb->getStartInfo()->extraOptionsInfo.unlimitedReplay) && onlyOnePlayerHuman)
	{
		repeat = std::make_shared<CButton>(Point(24, 505), AnimationPath::builtin("icn6432.def"), std::make_pair("", ""), [this](){ bRepeatf();}, EShortcut::GLOBAL_CANCEL);
		repeat->setBorderColor(borderColor);
		WidgetLayout::applyButton(repeat, layout["repeat"]);
		addLabel(resultLabel(layout["applyLabel"], Point(232, 520), FONT_MEDIUM, ETextAlignment::CENTER, Colors::WHITE, LIBRARY->generaltexth->translate("vcmi.battleResultsWindow.applyResultsLabel")));
	}

	// 两边“胜利/失败”：布局给了 winColor/loseColor 时按胜负着色
	for(auto side : {BattleSide::ATTACKER, BattleSide::DEFENDER})
	{
		const bool sideWon = br.winner == side;
		const int s = static_cast<int>(side);
		const Point fallback = side == BattleSide::ATTACKER ? Point(59, 124) : Point(sideWon ? 412 : 408, 124);
		const ColorRGBA color = WidgetLayout::color(layout[sideWon ? "winColor" : "loseColor"], Colors::WHITE);
		addLabel(resultLabel(resultSide(layout["sideResult"], s), fallback, FONT_SMALL, ETextAlignment::CENTER, color, LIBRARY->generaltexth->allTexts[sideWon ? 410 : 411]));
	}

	addLabel(resultLabel(layout["casualtiesTitle"], Point(232, 302), FONT_BIG, ETextAlignment::CENTER, Colors::YELLOW,  LIBRARY->generaltexth->allTexts[407]));
	addLabel(resultLabel(layout["attackerLabel"], Point(232, 332), FONT_SMALL, ETextAlignment::CENTER, Colors::WHITE, LIBRARY->generaltexth->allTexts[408]));
	addLabel(resultLabel(layout["defenderLabel"], Point(232, 428), FONT_SMALL, ETextAlignment::CENTER, Colors::WHITE, LIBRARY->generaltexth->allTexts[409]));

	std::array<std::string, 2> sideNames = {"N/A", "N/A"};

	for(auto i : {BattleSide::ATTACKER, BattleSide::DEFENDER})
	{
		auto heroInfo = owner.cb->getBattle(br.battleID)->battleGetHeroInfo(i);
		const std::array xs = {21, 392};
		const Point portrait = WidgetLayout::point(resultSide(layout["portraits"], static_cast<int>(i)), Point(xs[static_cast<int>(i)], 38));

		if(heroInfo.portraitSource.isValid()) //attacking hero
		{
			icons.push_back(std::make_shared<CAnimImage>(AnimationPath::builtin("PortraitsLarge"), heroInfo.getIconIndex(), 0, portrait.x, portrait.y));
			sideNames[static_cast<int>(i)] = heroInfo.name.toString(&GAME->translator());
		}
		else
		{
			auto stacks = owner.cb->getBattle(br.battleID)->battleGetAllStacks();
			vstd::erase_if(stacks, [i](const CStack * stack) //erase stack of other side and not coming from garrison
						   {
							   return stack->unitSide() != i || !stack->base;
						   });

			auto best = vstd::maxElementByFun(stacks, [](const CStack * stack)
											  {
												  return stack->unitType()->getAIValue();
											  });

			if(best != stacks.end()) //should be always but to be safe...
			{
				icons.push_back(std::make_shared<CAnimImage>(AnimationPath::builtin("TWCRPORT"), (*best)->unitType()->getIconIndex(), 0, portrait.x, portrait.y));
				sideNames[static_cast<int>(i)] = (*best)->unitType()->getNamePluralTranslated();
			}
		}
	}

	//printing attacker and defender's names
	addLabel(resultLabel(resultSide(layout["names"], 0), Point(89, 37), FONT_SMALL, ETextAlignment::TOPLEFT, Colors::WHITE, sideNames[0]));
	addLabel(resultLabel(resultSide(layout["names"], 1), Point(381, 53), FONT_SMALL, ETextAlignment::BOTTOMRIGHT, Colors::WHITE, sideNames[1]));

	//printing casualties
	static constexpr int iconWidth = 32;
	static constexpr int maxIconGap = 10;
	static constexpr int maxCasualtiesWidth = 380; // width of casualties frame on background image
	const ImagePath casualtyFrame = WidgetLayout::image(layout["casualtyFrame"], ImagePath());
	const Point casualtyFrameOffset = WidgetLayout::point(layout["casualtyFrameOffset"], Point());
	const ColorRGBA countColor = WidgetLayout::color(layout["countColor"], Colors::WHITE);
	const ColorRGBA emptyColor = WidgetLayout::color(layout["emptyColor"], Colors::WHITE);

	for(auto step : {BattleSide::ATTACKER, BattleSide::DEFENDER})
	{
		std::vector<std::pair<CreatureID, si32>> shownCasualties;
		for(const auto & elem : br.casualties[step])
		{
			if (elem.first != CreatureID::ARROW_TOWERS) // do not show destroyed towers in battle results
				shownCasualties.push_back(elem);
		}

		// 布局 "casualties": [{x: 中心, y: 头像顶, w: 最宽, gap: 最大间距}, …]
		const JsonNode & row = resultSide(layout["casualties"], static_cast<int>(step));
		const int centerX = row["x"].isNull() ? 235 : row["x"].Integer();
		const int rowY = row["y"].isNull() ? 344 + static_cast<int>(step) * 97 : row["y"].Integer();
		const int rowWidth = row["w"].isNull() ? maxCasualtiesWidth : row["w"].Integer();
		const int rowGap = row["gap"].isNull() ? maxIconGap : row["gap"].Integer();

		if(shownCasualties.empty())
		{
			labels.push_back(std::make_shared<CLabel>(centerX, rowY + 16, FONT_SMALL, ETextAlignment::CENTER, emptyColor, LIBRARY->generaltexth->allTexts[523]));
		}
		else
		{
			int casualties = shownCasualties.size();
			// negative gap if icons can't fit into frame - overlap icons instead
			int iconGap = casualties > 1 ? std::min((rowWidth - casualties * iconWidth) / (casualties - 1), rowGap) : 0;
			int xPos = centerX - (casualties * iconWidth + (casualties - 1) * iconGap) / 2;
			int yPos = rowY;
			for(int i = 0; i < casualties; ++i)
			{
				const auto * creature = shownCasualties[i].first.toEntity(LIBRARY);
				if(!casualtyFrame.empty())
					casualtyFrames.push_back(std::make_shared<CPicture>(casualtyFrame, xPos + casualtyFrameOffset.x, yPos + casualtyFrameOffset.y));
				icons.push_back(std::make_shared<CAnimImage>(AnimationPath::builtin("CPRSMALL"), creature->getIconIndex(), 0, xPos, yPos));
				labels.push_back(std::make_shared<CLabel>(xPos + iconWidth / 2, yPos + 42, FONT_SMALL, ETextAlignment::CENTER, countColor, std::to_string(shownCasualties[i].second)));
				xPos += iconWidth + iconGap;
			}
		}
	}

	auto resources = getResources(br);

	const JsonNode & artwork = layout["artwork"];
	if(artwork.isStruct())
	{
		const bool won = br.winner == battle->battleGetMySide();
		resultArtwork = std::make_shared<CPicture>(WidgetLayout::image(artwork[won ? "victory" : "defeat"], ImagePath()), WidgetLayout::point(artwork, Point()));
	}
	else if(jadeTheme)
	{
		const bool won = br.winner == battle->battleGetMySide();
		resultArtwork = std::make_shared<CPicture>(ImagePath::builtin(won ? "cultivation/ui/y700-windows/victory" : "cultivation/ui/y700-windows/defeat"), 195, 75);
		labels.push_back(std::make_shared<CLabel>(235, 177, FONT_BIG, ETextAlignment::CENTER, ColorRGBA(243, 240, 231), LIBRARY->generaltexth->translate(won ? "cultivation.windows.victory" : "cultivation.windows.defeat")));
	}
	else if(layout["video"].isNull() || layout["video"].Bool())
		videoPlayer = std::make_shared<VideoWidget>(Point(107, 70), resources.prologueVideo, resources.loopedVideo, false);

	// 说明文字建在题图之后，题图里留了放说明的空当时字压在题图上面
	const JsonNode & descLayout = layout["description"];
	std::string resultText = resources.resultText.toString(&GAME->translator());
	// 原版说明在“辉煌的胜利！”和经验值之间空一行；模组题图里放说明的纸带窄，"compact" 时合并空行，免得两行字就要滑条
	if(descLayout["compact"].Bool())
	{
		while(boost::algorithm::contains(resultText, "\n\n"))
			boost::algorithm::replace_all(resultText, "\n\n", "\n");
		boost::algorithm::trim_if(resultText, boost::algorithm::is_any_of("\n"));
	}
	description = std::make_shared<CTextBox>(resultText, WidgetLayout::rect(descLayout, Rect(69, 203, 330, 68)), 0, FONT_SMALL, ETextAlignment::CENTER, WidgetLayout::color(descLayout["color"], Colors::WHITE));
	// 字太多放不下时出的滑条换皮（没有滑条时不起作用）
	WidgetLayout::applySliderSkin(description->slider, descLayout["slider"]);


	if(!resources.musicSet.empty())
		ENGINE->music().playMusicFromSet(resources.musicSet, false, true);
	else
		ENGINE->music().playMusic(resources.musicName, false, true);
}

BattleResultResources BattleResultWindow::getResources(const BattleResult & br)
{
	//printing result description
	bool weAreAttacker = owner.cb->getBattle(br.battleID)->battleGetMySide() == BattleSide::ATTACKER;
	bool weAreDefender = !weAreAttacker;
	bool weWon = (br.winner == BattleSide::ATTACKER && weAreAttacker) || (br.winner == BattleSide::DEFENDER && !weAreAttacker);
	bool isSiege = owner.cb->getBattle(br.battleID)->battleGetDefendedTown() != nullptr;

	BattleResultResources resources;

	if(weWon)
	{
		if(isSiege && weAreDefender)
		{
			resources.musicName = AudioPath::builtin("Music/Defend Castle");
			resources.prologueVideo = VideoPath::builtin("DEFENDALL.BIK");
			resources.loopedVideo = VideoPath::builtin("defendloop.bik");
		}
		else
		{
			// 胜利结算从 win-battle 集合（所有 MUSIC/Win Battle* 资源）中随机选一首
			resources.musicSet = "win-battle";
			resources.prologueVideo = VideoPath::builtin("WIN3.BIK");
			resources.loopedVideo = VideoPath::builtin("WIN3.BIK");
		}

		switch(br.result)
		{
			case EBattleResult::NORMAL:
				resources.resultText.appendTextID("core.genrltxt.304");
				break;
			case EBattleResult::ESCAPE:
				resources.resultText.appendTextID("core.genrltxt.303");
				break;
			case EBattleResult::SURRENDER:
				resources.resultText.appendTextID("core.genrltxt.302");
				break;
			default:
				throw std::runtime_error("Invalid battle result!");
		}

		const CGHeroInstance * ourHero = owner.cb->getBattle(br.battleID)->battleGetMyHero();
		if(ourHero)
		{
			resources.resultText.appendTextID("core.genrltxt.305");
			resources.resultText.replaceTextID(ourHero->getNameTextID());
			resources.resultText.replaceNumber(br.exp[weAreAttacker ? BattleSide::ATTACKER : BattleSide::DEFENDER]);
		}
	}
	else // we lose
	{
		switch(br.result)
		{
			case EBattleResult::NORMAL:
				resources.resultText.appendTextID("core.genrltxt.311");
				resources.musicName = AudioPath::builtin("Music/LoseCombat");
				resources.prologueVideo = VideoPath::builtin("LBSTART.BIK");
				resources.loopedVideo = VideoPath::builtin("LBLOOP.BIK");
				break;
			case EBattleResult::ESCAPE:
				resources.resultText.appendTextID("core.genrltxt.310");
				resources.musicName = AudioPath::builtin("Music/Retreat Battle");
				resources.prologueVideo = VideoPath::builtin("RTSTART.BIK");
				resources.loopedVideo = VideoPath::builtin("RTLOOP.BIK");
				break;
			case EBattleResult::SURRENDER:
				resources.resultText.appendTextID("core.genrltxt.309");
				resources.musicName = AudioPath::builtin("Music/Surrender Battle");
				resources.prologueVideo = VideoPath::builtin("SURRENDER.BIK");
				resources.loopedVideo = VideoPath::builtin("SURRENDER.BIK");
				break;
			default:
				throw std::runtime_error("Invalid battle result!");
		}

		if(isSiege && weAreDefender)
		{
			resources.musicName = AudioPath::builtin("Music/LoseCastle");
			resources.prologueVideo = VideoPath::builtin("LOSECSTL.BIK");
			resources.loopedVideo = VideoPath::builtin("LOSECSLP.BIK");
		}
	}

	return resources;
}

void BattleResultWindow::activate()
{
	owner.showingDialog->setBusy();
	CIntObject::activate();
}

void BattleResultWindow::buttonPressed(int button)
{
	if(resultCallback)
		resultCallback(button);

	CPlayerInterface & intTmp = owner; //copy reference because "this" will be destructed soon

	close();

	if(ENGINE->windows().topWindow<BattleWindow>())
		ENGINE->windows().popWindows(1); //pop battle interface if present

	//Result window and battle interface are gone. We requested all dialogs to be closed before opening the battle,
	//so we can be sure that there is no dialogs left on GUI stack.
	intTmp.showingDialog->setFree();
}

void BattleResultWindow::bExitf()
{
	buttonPressed(0);
}

void BattleResultWindow::bRepeatf()
{
	buttonPressed(1);
}
