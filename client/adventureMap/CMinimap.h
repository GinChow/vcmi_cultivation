/*
 * CMinimap.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "../gui/CIntObject.h"
#include "../../lib/Color.h"

VCMI_LIB_NAMESPACE_BEGIN
class JsonNode;
VCMI_LIB_NAMESPACE_END

using FowTilesType = std::set<int3>;

class Canvas;
class CMinimap;
class IImage;

/// 小地图样式：大地图布局 JSON 里 adventureMinimap 控件的 "style" 字段（纯客户端显示，不影响存档）。
/// 不写 style 时各项为空，保持原版画法：地形原色、未探索纯黑、紫色虚线视野框、最近邻放大。
struct MinimapStyle
{
	/// 按 TerrainId 下标的（未遮挡色, 遮挡色）；没配的地形用地形配置里的原色
	std::vector<std::optional<std::pair<ColorRGBA, ColorRGBA>>> terrainColors;
	std::optional<ColorRGBA> fogColor;        ///< 未探索格
	std::optional<ColorRGBA> backgroundColor; ///< 地图不是正方形时小地图两侧的空白
	std::optional<ColorRGBA> radarColor;      ///< 视野框
	std::optional<ColorRGBA> fogEdgeColor;    ///< 已探索区贴着未探索格那一圈的勾边色
	float fogEdgeStrength = 0;                ///< 勾边色混入比例，0 不勾边
	bool smooth = false;                      ///< 放大时线性插值（格子边晕开），否则最近邻
	std::shared_ptr<IImage> overlay;          ///< 盖在小地图上的纸纹（与小地图同尺寸，半透明）

	static std::shared_ptr<const MinimapStyle> fromJson(const JsonNode & config);
};

class CMinimapInstance : public CIntObject
{
	std::unique_ptr<Canvas> minimap;
	std::shared_ptr<const MinimapStyle> style;
	int level;

	/// 已探索格的上下左右有没有未探索格（用于迷雾边勾墨）
	bool touchesFog(const int3 & pos) const;

	//get color of selected tile on minimap
	//minimapShowHeroes is passed in rather than read here: it is a settings lookup,
	//and redrawMinimap() calls this once per tile of the entire map
	ColorRGBA getTileColor(const int3 & pos, bool minimapShowHeroes) const;

	void redrawMinimap();
public:
	/// style 为空时按原版画法（如 InfoWindows 里的小地图）
	CMinimapInstance(const Point & position, const Point & dimensions, int level, std::shared_ptr<const MinimapStyle> style = nullptr);
	~CMinimapInstance();

	void showAll(Canvas & to) override;
	void refreshTile(const int3 & pos, bool minimapShowHeroes);
};

/// Minimap which is displayed at the right upper corner of adventure map
class CMinimap : public CIntObject
{
	std::shared_ptr<CPicture> aiShield; //the graphic displayed during AI turn
	std::shared_ptr<CMinimapInstance> minimap;
	std::shared_ptr<IImage> heroIcon;
	std::shared_ptr<const MinimapStyle> style;

	Rect screenArea;
	int level;

	void gesturePanning(const Point & initialPosition, const Point & currentPosition, const Point & lastUpdateDistance) override;
	void clickPressed(const Point & cursorPosition) override;
	void showPopupWindow(const Point & cursorPosition) override;
	void hover(bool on) override;
	void mouseDragged(const Point & cursorPosition, const Point & lastUpdateDistance) override;
	std::vector<ObjectInstanceID> getVisibleHeroes();

	/// relocates center of adventure map screen to currently hovered tile
	void moveAdvMapSelection(const Point & positionGlobal);

protected:
	/// computes coordinates of tile below cursor pos
	int3 pixelToTile(const Point & cursorPos) const;

	/// computes position of tile within minimap instance
	Point tileToPixels(const int3 & position) const;

public:
	/// style 为空时按原版画法（任务日志等窗口里的小地图）
	explicit CMinimap(const Rect & position, std::shared_ptr<const MinimapStyle> style = nullptr);

	void onMapViewMoved(const Rect & visibleArea, int mapLevel);
	void update();
	void setAIRadar(bool on);

	void showAll(Canvas & to) override;

	void updateTiles(const FowTilesType & positions);
};

