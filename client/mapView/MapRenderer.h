/*
 * MapRenderer.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "../../lib/int3.h"
#include "../../lib/Point.h"
#include "../../lib/filesystem/ResourcePath.h"

class ObjectInstanceID;
class CGObjectInstance;

class CAnimation;
class IImage;
class Canvas;
class IMapRendererContext;
struct CGPath;
struct NeighborTilesInfo;
enum class EImageBlitMode : uint8_t;
enum class MapMaskBlend : uint8_t;

class MapTileStorage
{
	using TerrainAnimation = std::array<std::shared_ptr<CAnimation>, 4>;
	std::vector<TerrainAnimation> animations;

	/// Memo for groupCount(), which walks CAnimation::size() - a map lookup each. Depends only on
	/// the loaded animations, so it is computed once per key. -1 means "not yet known".
	using GroupCounts = std::array<std::vector<int>, 4>;
	std::vector<GroupCounts> groupCounts;

	int computeGroupCount(size_t fileIndex, size_t rotationIndex, size_t imageIndex) const;

public:
	explicit MapTileStorage(size_t capacity);
	void load(size_t index, const AnimationPath & filename, EImageBlitMode blitMode);
	std::shared_ptr<IImage> find(size_t fileIndex, size_t rotationIndex, size_t imageIndex, size_t groupIndex = 0);
	int groupCount(size_t fileIndex, size_t rotationIndex, size_t imageIndex);
};

class MapRendererTerrain
{
	MapTileStorage storage;

public:
	MapRendererTerrain();

	uint8_t checksum(IMapRendererContext & context, const int3 & coordinates);
	void renderTile(IMapRendererContext & context, Canvas & target, const int3 & coordinates);
};

class MapRendererRiver
{
	MapTileStorage storage;

public:
	MapRendererRiver();

	uint8_t checksum(IMapRendererContext & context, const int3 & coordinates);
	void renderTile(IMapRendererContext & context, Canvas & target, const int3 & coordinates);
};

class MapRendererRoad
{
	MapTileStorage storage;

public:
	MapRendererRoad();

	uint8_t checksum(IMapRendererContext & context, const int3 & coordinates);
	void renderTile(IMapRendererContext & context, Canvas & target, const int3 & coordinates);
};

/// 水面遮罩（2026-11-17，大地图水面特效）：给水面类地形和河流画出遮罩帧。
///
/// 遮罩动画名为图块动画名加 "_MASK"（如 WATRTL_MASK、CLRRVR_MASK），由地形模组提供，与图块逐帧对应；
/// 帧号和翻转取自图块，所以遮罩与画面上的岸线严丝合缝。没有提供遮罩的地形、河流不画（保持黑色）。
/// 海面遮罩是灰白（红绿相等），河流遮罩只有绿色：着色器用 绿减红 区分河面，河面只做扰动、波光和倒影。
class MapRendererWaterMask
{
	MapTileStorage storage;
	MapTileStorage riverStorage;

public:
	MapRendererWaterMask();

	void renderTile(IMapRendererContext & context, Canvas & target, const int3 & coordinates);
};

/// 分层渲染时图块的各层（2026-11-17，大地图水面特效、风场摇摆、光影）。按 地面 → 物件（静止与摇摆两层按风场遮罩排好前后）
/// → 覆盖 叠起来，没有风时结果与整块渲染一致。
enum class MapTileLayer : uint8_t
{
	GROUND,       ///< 地形、贴地物件、河流、道路、地图边缘
	WATER_MASK,   ///< 水面遮罩（不参与叠加，只给着色器用）
	OBJECTS,      ///< 不摇摆的地图物件，以及所有物件的阴影
	SWAY_OBJECTS, ///< 随风摇摆的物件本体（树、灌木、花草），由着色器按风场扭动
	WIND_MASK,    ///< 风场遮罩（不参与叠加）：RGB 是摇摆物件的风场参数，A 是“上面盖着不摇摆物件”的程度
	RELIEF,       ///< 起伏层（不参与叠加）：RG 是物件表面的法线，B 是离地高度，A 是本体覆盖度（预乘），给长投影、打光、雾和云用
	OVERLAY,      ///< 行军路径、覆盖图标、战争迷雾
	ALL           ///< 不分层：整块图块，与 renderTile 相同
};

/// 植被粒子（2026-11-17）：物件会掉多少落叶、飘多少花瓣、树上栖多少鸟，都是 0~1。
/// 来自模组为摇摆物件提供的 <动画名>_WIND.json 的 "vegetation" 字段（地形模组 tools/wind_masks.py 生成）
struct MapVegetation
{
	float leaves = 0.0f;
	float petals = 0.0f;
	float birds = 0.0f;
};

/// 雾与云海（2026-11-17）：物件脚下生多少雾、山腰绕多少云，都是 0~1。
/// 来自模组为物件提供的 <动画名>_RELIEF.json 的 "atmosphere" 字段（地形模组 tools/relief.py 生成）
struct MapAtmosphere
{
	float mist = 0.0f;
	float cloud = 0.0f;
};

/// 画一个图块上的哪些物件（2026-11-17）。
/// “水面装饰物”是模组为其提供了 <动画名>_MASK 水面遮罩的物件，如湖、清水河口：分层时它们画在地面层，水面着色器才能扭动湖面。
/// “摇摆物件”是模组为其提供了 <动画名>_WIND 风场参数图的物件，如树、灌木：开启风场时画在摇摆层，阴影仍画在静止的物件层。
enum class MapObjectSelection : uint8_t
{
	ALL,
	WATER_SURFACES,
	SWAYING,
	OTHERS ///< 既不是水面装饰物、也不摇摆的物件，外加摇摆物件的阴影
};

class MapRendererObjects
{
	std::map<std::pair<AnimationPath, EImageBlitMode>, std::shared_ptr<CAnimation>> animations;
	mutable std::map<ImagePath, std::shared_ptr<IImage>> images;

	/// Everything the checksum needs to know about one object in one animation group:
	/// whether it animates at all, and how large its sprite is.
	struct ObjectChecksumInfo
	{
		bool baseAnimated = false;
		bool flagAnimated = false;
		Point baseDimensions = Point(0, 0);
		Point flagDimensions = Point(0, 0);
	};

	/// Memo for the above, valid for a single update pass only: the answer costs a string-keyed
	/// animation lookup plus an image, and is asked once per tile the object covers, every frame.
	/// Cleared by prepareFrame(), and nothing it depends on can change within a pass.
	/// Keyed by object and animation group, packed into one integer.
	std::unordered_map<uint64_t, ObjectChecksumInfo> checksumInfoCache;

	/// The images an object contributes this pass, in draw order. Resolving them costs a lookup
	/// each and is asked once per tile it covers. Cleared by prepareFrame().
	/// Like in H3 the shadow is drawn on its own, before the bodies of everything on the tile, so
	/// it never darkens another object. Heroes and boats keep the shadow in their body image.
	enum ObjectImageLayer
	{
		LAYER_SHADOW,
		LAYER_BODY,
		LAYER_COLOR_OVERLAY, // player color of flaggable objects
		LAYER_FLAG,
		LAYER_OVERLAY,
		LAYER_COUNT
	};
	using ObjectImages = std::array<std::shared_ptr<IImage>, LAYER_COUNT>;
	std::unordered_map<int32_t, ObjectImages> renderImageCache;

	const ObjectImages & getObjectImages(IMapRendererContext & context, const CGObjectInstance * object);

	const ObjectChecksumInfo & getChecksumInfo(IMapRendererContext & context, const CGObjectInstance * object, size_t groupIndex);

	std::shared_ptr<CAnimation> getBodyAnimation(const CGObjectInstance * obj);
	std::shared_ptr<CAnimation> getShadowAnimation(const CGObjectInstance * obj);
	std::shared_ptr<CAnimation> getColorOverlayAnimation(const CGObjectInstance * obj);
	std::shared_ptr<CAnimation> getFlagAnimation(const CGObjectInstance * obj);
	std::shared_ptr<CAnimation> getOverlayAnimation(const CGObjectInstance * obj);

	std::shared_ptr<CAnimation> getAnimation(const AnimationPath & filename, bool generateMovementGroups, EImageBlitMode mode);
	std::shared_ptr<IImage> getImage(const ImagePath & filename) const;

	/// firstFrame：不论动画时钟，总取第 0 帧（开启风场时的摇摆物件，摇摆由着色器做）
	std::shared_ptr<IImage> getImageToRender(const IMapRendererContext & context, const CGObjectInstance * obj, const std::shared_ptr<CAnimation> & animation, bool firstFrame = false) const;

	/// mask：画进遮罩画布（风场遮罩、起伏层），方式见 MapMaskBlend；为空时照常画
	void renderImage(IMapRendererContext & context, Canvas & target, const int3 & coordinates, const CGObjectInstance * object, const std::shared_ptr<IImage> & image, double transparencyFactor = 1.0, bool applyOwnerColor = true, std::optional<MapMaskBlend> mask = std::nullopt);
	void renderShadow(IMapRendererContext & context, Canvas & target, const int3 & coordinates, const CGObjectInstance * object);
	void renderObject(IMapRendererContext & context, Canvas & target, const int3 & coordinates, const CGObjectInstance * obj, double transparencyFactor = 1.0, std::optional<MapMaskBlend> mask = std::nullopt);

	/// transparency of the second, always-on-top copy of the active hero
	static constexpr double activeHeroTransparency = 0.5;

	/// 模组随物件动画附带的同名辅助动画（<动画名><suffix>，如水面遮罩 _MASK、风场参数图 _WIND），
	/// 按动画名记在 cache 里，没有的记为空，免得每次都查资源
	std::shared_ptr<CAnimation> findCompanionAnimation(std::map<std::string, std::shared_ptr<CAnimation>> & cache, const CGObjectInstance * obj, const std::string & suffix);

	std::map<std::string, std::shared_ptr<CAnimation>> waterMaskAnimations;
	std::map<std::string, std::shared_ptr<CAnimation>> windMaskAnimations;
	std::map<std::string, std::shared_ptr<CAnimation>> reliefAnimations;

	/// 各物件动画的雾、云参数，按动画名记下
	std::map<std::string, MapAtmosphere> atmosphereConfigs;

	/// 各物件动画的植被粒子参数，按动画名记下
	std::map<std::string, MapVegetation> vegetationConfigs;

	/// 物件动画名（去掉 SPRITES/ 前缀）；没有动画时为空
	static std::string spriteName(const CGObjectInstance * obj);

	/// 物件的植被粒子参数；不是摇摆物件时全为 0
	const MapVegetation & getVegetation(const CGObjectInstance * obj);

	/// 物件的水面遮罩动画；不是水面装饰物时返回空
	std::shared_ptr<CAnimation> getWaterMaskAnimation(const CGObjectInstance * obj);

	/// 物件的风场参数图（只有一帧，与本体第 0 帧逐像素对齐，见地形模组 tools/wind_masks.py）；不摇摆的物件返回空
	std::shared_ptr<CAnimation> getWindMaskAnimation(const CGObjectInstance * obj);

	/// 物件的起伏图（地形模组 tools/relief.py 生成，与本体第 0 帧逐像素对齐）：第 0 组是法线和高度，
	/// 第 1 组是只有接触阴影的阴影图（分层时代替原来的阴影，长投影改由着色器按起伏层算）。没有起伏图的物件返回空
	std::shared_ptr<CAnimation> getReliefAnimation(const CGObjectInstance * obj);

	/// 是否开启分层特效：开启时摇摆物件停在第 0 帧、单独成层，有起伏图的物件改画接触阴影（见 setLayeredEffects）
	bool layeredEffects = false;

	/// 物件属于哪类特效物件（水面装饰物、有风场参数图），本次更新内按物件记下。
	/// 分层时每个物件在每个图块的三个物件层里都要问好几遍，每次按动画名查表，缩到最小滚屏时会占掉可观的帧时间。
	/// 由 prepareFrame() 清空
	struct ObjectEffectKind
	{
		bool waterSurface = false;
		bool windParameters = false;
		bool relief = false;
	};
	std::unordered_map<int32_t, ObjectEffectKind> effectKindCache;
	const ObjectEffectKind & getEffectKind(const CGObjectInstance * obj);

	/// 开启风场时，物件是否随风摇摆（有风场参数图、且不是水面装饰物）
	bool isSwaying(const CGObjectInstance * obj);

	bool isSelected(const CGObjectInstance * obj, MapObjectSelection selection);

	/// 物件的阴影画在哪一层：摇摆物件的阴影留在静止的物件层（阴影贴地，不随风动）
	bool isShadowSelected(const CGObjectInstance * obj, MapObjectSelection selection);
public:
	/// Special ground objects, which are drawn before the rivers and roads
	void renderGround(IMapRendererContext & context, Canvas & target, const int3 & coordinates);

	/// Must be called once per update pass before any checksum() call
	void prepareFrame();

	uint8_t checksum(IMapRendererContext & context, const int3 & coordinates);
	void renderTile(IMapRendererContext & context, Canvas & target, const int3 & coordinates, MapObjectSelection selection = MapObjectSelection::ALL);

	/// 把图块上水面装饰物的遮罩画到 target（水面遮罩层）
	void renderWaterMasks(IMapRendererContext & context, Canvas & target, const int3 & coordinates);

	/// 图块上有没有水面装饰物（它们画在地面层，动画换帧时地面层也要重画）
	bool hasWaterSurface(IMapRendererContext & context, const int3 & coordinates);

	/// 开关分层特效（大地图分层、着色器可用时开启）：风场摇摆、起伏图的接触阴影。
	/// 关闭时摇摆物件照原来的逐帧动画画在物件层，阴影照原来的阴影图
	void setLayeredEffects(bool enabled);

	/// 图块上有没有随风摇摆的物件
	bool hasSwayingObject(IMapRendererContext & context, const int3 & coordinates);

	/// 图块上各物件植被粒子参数的最大值（与是否开启风场无关）
	MapVegetation vegetationAt(IMapRendererContext & context, const int3 & coordinates);

	/// 图块上各物件雾、云参数的最大值
	MapAtmosphere atmosphereAt(IMapRendererContext & context, const int3 & coordinates);

	/// 把图块画进起伏层（target 须先清成全透明）：按与物件层相同的前后顺序，有起伏图的物件画起伏图，
	/// 其余物件画清零剪影（见 MapMaskBlend::CUTOUT），阴影、水面装饰物不画。
	/// 着色器据此算长投影、给物件重新打光，并让雾贴着山脚、峰顶伸出云海
	void renderRelief(IMapRendererContext & context, Canvas & target, const int3 & coordinates);

	/// 把图块画进风场遮罩（target 须先清成全透明）：按与物件层完全相同的前后顺序，摇摆物件画风场参数图，
	/// 其余物件画遮挡剪影，阴影不画。着色器据此知道树冠各处能摆多远，以及树从别的物件背后摆出来时谁在前面。
	/// 图块上没有摇摆物件时什么都不画
	void renderWindMask(IMapRendererContext & context, Canvas & target, const int3 & coordinates);
};

class MapRendererBorder
{
	std::shared_ptr<CAnimation> animation;

	size_t getIndexForTile(IMapRendererContext & context, const int3 & coordinates);

public:
	MapRendererBorder();

	uint8_t checksum(IMapRendererContext & context, const int3 & coordinates);
	void renderTile(IMapRendererContext & context, Canvas & target, const int3 & coordinates);
};

class MapRendererFow
{
	std::shared_ptr<CAnimation> fogOfWarFullHide;
	std::shared_ptr<CAnimation> fogOfWarPartialHide;

public:
	MapRendererFow();

	/// Both take the neighbour visibility that the caller has already computed - it costs
	/// eight visibility queries and the caller needs it anyway to reach this point.
	uint8_t checksum(IMapRendererContext & context, const int3 & coordinates, const NeighborTilesInfo & neighborInfo);
	void renderTile(IMapRendererContext & context, Canvas & target, const int3 & coordinates, const NeighborTilesInfo & neighborInfo);
};

class MapRendererPath
{
	std::shared_ptr<CAnimation> pathNodes;

	/// Frame-scoped snapshot of the hero path, refreshed by prepareFrame(). Resolving it costs
	/// three lookups plus a linear scan, otherwise repeated for every visible tile every frame.
	const CGPath * cachedPath = nullptr;
	/// Sorted coordinates of cachedPath, for rejecting the vast majority of tiles in
	/// logarithmic time instead of scanning the whole node list
	std::vector<int3> cachedPathTiles;

	size_t selectImageReachability(bool reachableToday, size_t imageIndex);
	size_t selectImageCross(bool reachableToday, const int3 & curr);
	size_t selectImageArrow(bool reachableToday, const int3 & curr, const int3 & prev, const int3 & next);
	size_t selectImage(IMapRendererContext & context, const int3 & coordinates);

public:
	MapRendererPath();

	/// Must be called once per frame before any checksum() or renderTile() call
	void prepareFrame(IMapRendererContext & context);

	uint8_t checksum(IMapRendererContext & context, const int3 & coordinates);
	void renderTile(IMapRendererContext & context, Canvas & target, const int3 & coordinates);
};

class MapRendererOverlay
{
	std::shared_ptr<IImage> imageGrid;
	std::shared_ptr<IImage> imageVisitable;
	std::shared_ptr<IImage> imageBlocked;
	std::shared_ptr<IImage> imageSpellRange;
	std::shared_ptr<IImage> imageEvent;
	std::shared_ptr<IImage> imageGrail;

	int3 grailPos;
public:
	MapRendererOverlay();

	uint8_t checksum(IMapRendererContext & context, const int3 & coordinates);
	void renderTile(IMapRendererContext & context, Canvas & target, const int3 & coordinates);
};

class MapRenderer
{
	MapRendererTerrain rendererTerrain;
	MapRendererRiver rendererRiver;
	MapRendererRoad rendererRoad;
	MapRendererBorder rendererBorder;
	MapRendererFow rendererFow;
	MapRendererObjects rendererObjects;
	MapRendererPath rendererPath;
	MapRendererOverlay rendererOverlay;
	MapRendererWaterMask rendererWaterMask;

	/// 地面部分：地形、贴地物件、河流、道路（顺序与原版一致）
	void renderGroundParts(IMapRendererContext & context, Canvas & target, const int3 & coordinates);

public:
	using TileChecksum = std::array<uint8_t, 8>;

	/// Refreshes state that is constant for the whole frame. Must be called before the
	/// per-tile getTileChecksum() / renderTile() calls of that frame.
	void prepareFrame(IMapRendererContext & context);

	TileChecksum getTileChecksum(IMapRendererContext & context, const int3 & coordinates);

	void renderTile(IMapRendererContext & context, Canvas & target, const int3 & coordinates, bool withOverlays);

	/// 图块上有没有水面装饰物（湖、清水河口），见 MapRendererObjects::hasWaterSurface
	bool hasWaterSurface(IMapRendererContext & context, const int3 & coordinates);

	/// 开关分层特效，见 MapRendererObjects::setLayeredEffects
	void setLayeredEffects(bool enabled);

	/// 图块上的雾、云参数，见 MapRendererObjects::atmosphereAt；地图外为 0
	MapAtmosphere atmosphereAt(IMapRendererContext & context, const int3 & coordinates);

	/// 图块上植物会掉多少落叶、飘多少花瓣、栖多少鸟，见 MapRendererObjects::vegetationAt；地图外为 0
	MapVegetation vegetationAt(IMapRendererContext & context, const int3 & coordinates);

	/// 只画图块的某一层（target 须先清空：遮罩为黑，其余为透明）。
	/// 每层单独一个调用，调用方就能把许多图块的同一层连着画，不必在几张画布间来回切换渲染目标。
	void renderTileLayer(IMapRendererContext & context, Canvas & target, const int3 & coordinates, MapTileLayer layer, bool withOverlays);
};
