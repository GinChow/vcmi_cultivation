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
	/// mask：画进遮罩画布（如地形分布层的挖空，见 MapMaskBlend）；为空时照常画
	void renderTile(IMapRendererContext & context, Canvas & target, const int3 & coordinates, std::optional<MapMaskBlend> mask = std::nullopt);
};

class MapRendererRoad
{
	MapTileStorage storage;

public:
	MapRendererRoad();

	uint8_t checksum(IMapRendererContext & context, const int3 & coordinates);
	/// mask：同 MapRendererRiver::renderTile
	void renderTile(IMapRendererContext & context, Canvas & target, const int3 & coordinates, std::optional<MapMaskBlend> mask = std::nullopt);
};

/// 地形分布图（2026-11-17，大地图地表材质）：给陆地图块画出“这一点本地形、泥地、沙地各占多少”。
///
/// 分布图动画名为图块动画名加 "_SPLAT"（如 GRASTL_SPLAT），由地形模组提供，与图块逐帧对应，帧号和翻转取自图块，
/// 所以交界形状与原版一致。R 为本地形、G 为泥地、B 为沙地的比重，三者之和不足 1 处（岩石、道路、河流、湖……）保留图块原画。
/// 合成着色器据此按世界坐标铺各地形的材质（见 MapLayerCompositor），图块里只剩“哪里是什么地形”的信息。
/// 水面图块的分布图只有沙地（岸上的沙滩），水由水面着色器管。没有提供分布图的地形（岩石、模组新增地形）不画（保持黑色），照旧显示图块原画。
class MapRendererTerrainSplat
{
	MapTileStorage storage;

public:
	MapRendererTerrainSplat();

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
	TERRAIN_SPLAT,///< 地形分布层（不参与叠加）：RGB 是本地形、泥地、沙地的比重，道路、河流、湖等贴地物件处挖空，见 MapRendererTerrainSplat；
	              ///< 1 − A 是特殊地面的覆盖度（2026-11-19，见 MapRendererObjects::renderGroundCutouts）
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

	/// 把图块上画在地面层的物件画进地形分布层：湖这类水面装饰物按清零剪影，那里保留原画、不铺地表材质；
	/// 特殊地面（被诅咒的大地、魔法平原……）把 RGB、A 都按 alpha 压低（MapMaskBlend::SPECIAL_GROUND），
	/// 着色器按 1 − A 在那里铺特殊地面自己的材质（2026-11-19）。specialGround 选画哪一类：两类在道路、河流前后画的时机不同
	void renderGroundCutouts(IMapRendererContext & context, Canvas & target, const int3 & coordinates, bool specialGround);

	/// 把图块画进起伏层（target 须先清成全透明）：按与物件层相同的前后顺序，有起伏图的物件画起伏图，
	/// 其余物件画清零剪影（见 MapMaskBlend::CUTOUT），阴影、水面装饰物不画。
	/// 着色器据此算长投影、给物件重新打光，并让雾贴着山脚、峰顶伸出云海
	void renderRelief(IMapRendererContext & context, Canvas & target, const int3 & coordinates);

	/// 只画一件物件落在图块 coordinates 上的那一块（本体、玩家颜色、旗子，不画阴影）：鼠标悬停时单独画成一张画布描边（2026-11-18）
	void renderSingleObject(IMapRendererContext & context, Canvas & target, const int3 & coordinates, const CGObjectInstance * object);

	/// 把图块画进风场遮罩（target 须先清成全透明）：按与物件层完全相同的前后顺序，摇摆物件画风场参数图，
	/// 其余物件画遮挡剪影，阴影不画。着色器据此知道树冠各处能摆多远，以及树从别的物件背后摆出来时谁在前面。
	/// 图块上没有摇摆物件时什么都不画
	void renderWindMask(IMapRendererContext & context, Canvas & target, const int3 & coordinates);
};

class MapRendererBorder
{
	std::shared_ptr<CAnimation> animation;
	/// 世界之外的底纹（模组提供 Data/mapBorderBackground 时才有，2026-11-20 修仙装裱绫绢）：
	/// 有它时世界之外的图块都按世界像素坐标从这张图里取，EDG 只用四角四边（16–35 帧）半透明叠在上面；
	/// 没有时保持原版，世界之外画 EDG 0–15 帧
	std::shared_ptr<IImage> background;

	size_t getIndexForTile(IMapRendererContext & context, const int3 & coordinates);
	/// 图块是否在紧贴世界的那一圈（四角四边）
	bool isFrameTile(IMapRendererContext & context, const int3 & coordinates) const;
	/// 按世界像素坐标从底纹图里取一个图块大小（正取模，世界左边、上边与右边、下边同向平铺）
	void renderBackground(Canvas & target, const int3 & coordinates);

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
	MapRendererTerrainSplat rendererTerrainSplat;

	/// 地面部分：地形、贴地物件、河流、道路（顺序与原版一致）
	void renderGroundParts(IMapRendererContext & context, Canvas & target, const int3 & coordinates);

	/// 水墨云海迷雾（见 setCloudFog）
	bool cloudFog = false;

public:
	using TileChecksum = std::array<uint8_t, 8>;

	/// 图块的迷雾状态，从校验值读出（见 fogStateOf）
	enum class FogState : uint8_t
	{
		VISIBLE,         ///< 看得见，或在地图外
		HIDDEN,          ///< 在迷雾里
		HIDDEN_OUT_OF_RANGE ///< 在迷雾里，且正在选冒险法术的目标、这里在射程外（原版画成纯黑）
	};

	/// 校验值第 7 项是迷雾：看得见时保持 0xff，在迷雾里时是迷雾的校验值。地图缓存每帧本来就要算可见范围内每个图块的校验值，
	/// 从这里读迷雾状态不必再查一遍可见性（缩到全图时几万个图块，每次查询都要经游戏状态找本方的迷雾图）
	static FogState fogStateOf(const TileChecksum & checksum);

	/// 水墨云海迷雾（2026-11-18）：开启时分层画的覆盖层不再画原版的黑色迷雾图块，迷雾改由合成器按地图缓存给的云雾场画成流动的云。
	/// 只影响 renderTileLayer() 的覆盖层，renderTile()（不分层的缓存、导出整张地图）照旧画原版迷雾。
	/// 切换时迷雾的校验值随之改变，地图缓存会把迷雾里图块的覆盖层重画一遍
	void setCloudFog(bool enabled);

	/// Refreshes state that is constant for the whole frame. Must be called before the
	/// per-tile getTileChecksum() / renderTile() calls of that frame.
	void prepareFrame(IMapRendererContext & context);

	/// fullyHidden 不为空时顺带给出图块是否完全被迷雾盖住（自己和四周都看不见：这时只有覆盖层画迷雾，其余各层都是空的）。
	/// 判断要查四周 8 格的可见性，算校验值时本来就要查，地图缓存据此按层批量重画，不必再算一遍（2026-11-18，缩放掉帧）
	TileChecksum getTileChecksum(IMapRendererContext & context, const int3 & coordinates, bool * fullyHidden = nullptr);

	void renderTile(IMapRendererContext & context, Canvas & target, const int3 & coordinates, bool withOverlays);

	/// 图块上有没有水面装饰物（湖、清水河口），见 MapRendererObjects::hasWaterSurface
	bool hasWaterSurface(IMapRendererContext & context, const int3 & coordinates);

	/// 开关分层特效，见 MapRendererObjects::setLayeredEffects
	void setLayeredEffects(bool enabled);

	/// 只画一件物件落在图块上的那一块，见 MapRendererObjects::renderSingleObject
	void renderSingleObject(IMapRendererContext & context, Canvas & target, const int3 & coordinates, const CGObjectInstance * object);

	/// 图块上的雾、云参数，见 MapRendererObjects::atmosphereAt；地图外为 0
	MapAtmosphere atmosphereAt(IMapRendererContext & context, const int3 & coordinates);

	/// 图块上植物会掉多少落叶、飘多少花瓣、栖多少鸟，见 MapRendererObjects::vegetationAt；地图外为 0
	MapVegetation vegetationAt(IMapRendererContext & context, const int3 & coordinates);

	/// 只画图块的某一层（target 须先清空：遮罩为黑，其余为透明）。
	/// 每层单独一个调用，调用方就能把许多图块的同一层连着画，不必在几张画布间来回切换渲染目标。
	void renderTileLayer(IMapRendererContext & context, Canvas & target, const int3 & coordinates, MapTileLayer layer, bool withOverlays);
};
