/*
 * MapViewCache.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "../../lib/Point.h"
#include "../../lib/Rect.h"
#include "../../lib/int3.h"

#include "render/IScreenHandler.h"

class ObjectInstanceID;
class CGHeroInstance;

class IImage;
class CAnimation;
class Canvas;
class MapRenderer;
class IMapRendererContext;
class MapViewModel;

/// Class responsible for rendering of entire map view
/// uses rendering parameters provided by owner class
class MapViewCache
{
	struct TileChecksum
	{
		int tileX = std::numeric_limits<int>::min();
		int tileY = std::numeric_limits<int>::min();
		std::array<uint8_t, 8> checksum{};

		bool operator==(const TileChecksum & other) const
		{
			return tileX == other.tileX && tileY == other.tileY && checksum == other.checksum;
		}
	};

	boost::multi_array<TileChecksum, 2> terrainChecksum;
	boost::multi_array<bool, 2> tilesUpToDate;

	/// Animation-step bookkeeping of update(): tiles dirtied this frame and the previous one,
	/// tiles redrawn so far, this frame's share, and whether the previous frame left some waiting
	int animatedTilesDirty = 0;
	int animatedTilesDirtyBefore = 0;
	int animatedTilesRedrawn = 0;
	int animatedTileRedrawBudget = 0;
	bool drainingAnimationBurst = false;

	Point cachedSize;
	Point cachedPosition;
	int cachedLevel;
	bool overlayWasVisible;

	std::shared_ptr<MapViewModel> model;

	/// Whether this cache may allocate GPU render targets
	bool useGpuLayer;

	/// Whether the canvases below are GPU backed. A colour scheme moves the whole client to the
	/// surface path, and a cache built for one path cannot be drawn onto the other.
	bool canvasesOnGpu = false;

	/// Whether the terrain cache was last built at native tile size, rather than the bounded,
	/// ready-scaled size used when that would exceed the driver's maximum texture size
	bool cachedAtNativeSize = false;

	/// Size the cache canvas was created for - at native tile size it follows the zoom level
	Point cachedCanvasDimensions;

	/// Set by update(), cleared by render(). Lets a caller that already filled the cache
	/// this frame skip doing it again, which would re-checksum every visible tile.
	bool updatedThisFrame = false;

	/// Tiles the cache was filled for, to detect a scroll between the update and the draw
	Rect updatedTilesRect;

	std::unique_ptr<Canvas> terrain;
	std::unique_ptr<Canvas> terrainTransition;
	std::unique_ptr<MapRenderer> mapRenderer;

	/// 图集：缓存存的是缩小过的图块时（缩放到图块小于 16 像素），图块先按原尺寸画进这里，再缩小拷进缓存。
	/// GPU 上一次放 atlasTilesPerSide² 块，一批只切两次渲染目标；逐块画进单格再拷，缩小视野时会掉到 10 FPS。
	/// 软件表面的缩放拷贝只能整张拷，所以那时图集只有一格。
	std::unique_ptr<Canvas> atlas;
	/// 32 × 32 格：缩到全图时一层几千个图块，每批多装些，切换渲染目标（每次都要新开渲染通道）的次数就少（2026-11-18 由 16 加大）
	static constexpr int atlasTilesPerSide = 32;
	int atlasCapacity = 1;

	/// 分层模式（2026-11-17，大地图水面特效、风场摇摆）。开启时 terrain 只存地面层，静止物件、摇摆物件、覆盖物、
	/// 水面遮罩、风场遮罩各存一张与它布局相同的画布，合成屏幕时由屏幕处理器的着色器一次叠好（见 presentMapLayers）。
	/// 只在着色器可用、缓存正好是 槽位数 × 图块尺寸、且不是灰度视图（拼图地图）时开启。
	bool wantLayers = false;
	bool layered = false;

	/// 已交给屏幕处理器的离岸距离图对应的地图层（地上、地下）；-1 表示还没给过
	int depthFieldLevel = -1;

	/// 分层时给屏幕处理器一张当前地图层的离岸距离图（海面的浅滩、近海、深海靠它分出来）
	void sendDepthField(const std::shared_ptr<IMapRendererContext> & context);

	/// 植被粒子（2026-11-17）：分层时给屏幕处理器一张当前地图层的植被场——每个图块落叶、飞花多少（按树种乘地形系数）、
	/// 萤火虫多少（按地形，靠近植物或水的地方多），着色器据此放粒子；同时记下每个图块栖鸟的多少，惊鸟时用
	void sendVegetationField(const std::shared_ptr<IMapRendererContext> & context);

	/// 雾与云海（2026-11-17）：分层时给屏幕处理器一张当前地图层的雾云场——每个图块生雾多少（山脚、林地、水边、沼泽浓）、
	/// 山腰绕云多少（山和火山周围），都向四周晕开几个图块，着色器据此让流动的雾和云海只在这些地方浓起来
	void sendAtmosphereField(const std::shared_ptr<IMapRendererContext> & context);

	/// 当前地图层每个图块栖鸟的多少（0~1，已乘地形系数），按行存放
	std::vector<float> birdRoosts;
	Point roostMapSize;

	/// 惊鸟：英雄每走一步由 MapViewController 记下（可能在网络线程上），渲染线程在 update() 里处理
	std::mutex heroStepsMutex;
	std::vector<int3> pendingHeroSteps;

	/// 最近飞起过鸟群的地方和时间：同一片林子一阵子内不再飞起
	std::vector<std::pair<int3, std::chrono::steady_clock::time_point>> recentFlocks;

	/// 下一次“没人惊动、林子里自己飞起一群鸟”的时间
	std::chrono::steady_clock::time_point nextAmbientFlock;
	std::mt19937 birdRandom;

	/// 处理英雄走过的格子和偶尔自己飞起的鸟群（只在分层时）
	void updateBirds(const std::shared_ptr<IMapRendererContext> & context);
	float roostAt(const int3 & tile) const;
	bool flockedRecently(const int3 & tile, std::chrono::steady_clock::time_point now) const;
	/// 让 roost 图块的树上飞起一群鸟，朝 (directionX, directionY) 飞走
	void launchFlock(const int3 & roost, float directionX, float directionY, std::chrono::steady_clock::time_point now);
	std::unique_ptr<Canvas> objectLayer;
	std::unique_ptr<Canvas> swayLayer;
	std::unique_ptr<Canvas> overlayLayer;
	std::unique_ptr<Canvas> waterMaskLayer;
	std::unique_ptr<Canvas> windMaskLayer;
	std::unique_ptr<Canvas> reliefLayer;
	std::unique_ptr<Canvas> splatLayer;
	/// 缩小图块时不画的四层（摇摆、风场遮罩、起伏、地形分布）是否已在这批画布上清过：画布新建后内容不定，清一次之后就一直是空的
	bool liteLayersCleared = false;

	/// 地表材质（2026-11-17）：分层时给屏幕处理器一张当前地图层的地形场（每个图块属于哪套地表材质），
	/// 以及模组提供的材质图集（只给一次）。合成着色器据此按世界坐标铺地表材质
	void sendTerrainField(const std::shared_ptr<IMapRendererContext> & context);
	bool terrainMaterialsSent = false;

	/// 昼夜（2026-11-18）：把本方这一天的进度（见 IMapRendererContext::dayProgress）交给屏幕处理器推动太阳。
	/// 要查每个英雄的行动力上限，所以每 0.25 秒才算一次，太阳本身在合成器里平滑地走
	void sendDayProgress(const std::shared_ptr<IMapRendererContext> & context);
	std::chrono::steady_clock::time_point nextDayProgress;

	/// 物件带出的逐帧效果（2026-11-18 由局部光扩充）：每帧从可见窗口外加一圈（光能照进来的距离）里看得见的物件收集
	///   局部光：城镇灯笼、营火、灯塔、传送门和神殿的法光、熔岩装饰、英雄的火把；
	///   宝物闪光：宝箱、神器、资源、潘多拉魔盒、卷轴、漂流物；炊烟：城镇屋顶；
	///   英雄：走在沙地、雪地上留下蹄印（trackHero），走在沙、泥、雪、灰、草地上扬尘；
	///   鼠标悬停的物件：单独画成一张小画布（updateHighlight）。
	/// 都交给屏幕处理器（setMapObjectEffects）。走地图缓存每个图块的物件表，物件拿走、英雄走动都跟得上
	void sendObjectEffects(const std::shared_ptr<IMapRendererContext> & context);
	MapObjectEffects objectEffects;
	/// 上一次收集的时刻（算英雄的速度用）
	std::chrono::steady_clock::time_point lastEffectsTime;

	/// 英雄走过的路：上一帧在哪、上一对蹄印踩在哪，按物件编号记
	struct HeroTrail
	{
		float x = 0.0f;
		float y = 0.0f;
		float printX = 0.0f;
		float printY = 0.0f;
		std::chrono::steady_clock::time_point seen;
	};
	std::unordered_map<int, HeroTrail> heroTrails;
	/// 踩下的蹄印：在哪一层、什么时候踩的（浓淡按时间算）
	struct Footprint
	{
		MapFootprint print;
		int level = 0;
		std::chrono::steady_clock::time_point made;
	};
	std::deque<Footprint> footprints;
	/// 英雄这一帧在 (x, y)（站的图块中心加上走路动画的偏移，地图坐标）：记下走过的路，踩蹄印、扬尘
	void trackHero(const std::shared_ptr<IMapRendererContext> & context, const CGHeroInstance * hero, float x, float y, float dt, std::chrono::steady_clock::time_point now);
	/// 去掉淡完的蹄印，把当前地图层看得见的交给 objectEffects
	void collectFootprints(const Rect & tiles, std::chrono::steady_clock::time_point now);

	/// 鼠标悬停的物件（2026-11-18）：编号（-1 为没有）与描边颜色，由冒险地图界面在 GUI 线程上设置
	std::atomic<int> hoveredObject{-1};
	std::atomic<uint8_t> hoveredTone{0};
	/// 悬停物件单独画成的画布（透明底，四周留 highlightMargin 像素给光晕），上一次画的是哪件、什么时候画的、什么时候移上去的
	std::unique_ptr<Canvas> highlightCanvas;
	Point highlightCanvasSize;
	int highlightDrawnObject = -1;
	std::chrono::steady_clock::time_point highlightDrawnAt;
	std::chrono::steady_clock::time_point highlightSince;
	/// 把悬停的物件画进 highlightCanvas，填好 objectEffects.highlight
	void updateHighlight(const std::shared_ptr<IMapRendererContext> & context, std::chrono::steady_clock::time_point now);
	/// 去重：大物件占好几个图块，每个图块的物件表里都有它。litStamp[物件编号] 等于 litFrame 就是这一帧已经看过。
	/// 缩到全图时一帧要看几千个图块，线性查找去重要两毫秒（2026-11-18 实测），所以用按编号的标记
	std::vector<uint32_t> litStamp;
	uint32_t litFrame = 0;

	/// 水墨云海迷雾（2026-11-18；2026-11-20 起察看世界也算）：分层并且由合成器直接画到屏幕上时（不需要自己的图层），覆盖层不画原版的黑色迷雾，
	/// 改由合成器按云雾场画流动的云（见 MapRenderer::setCloudFog）。云雾场每个图块一个像素：
	///   R 云的浓度（在迷雾里为 1，揭开后慢慢降到 0，已按缓入缓出取好），G 正在散开的程度（散到一半最大，平时 0，着色器据此让云翻卷、往外推），
	///   B 在冒险法术射程外（原版画成纯黑，云也压暗）。
	/// 可见窗口里的图块每帧从校验值读迷雾状态（updateTile），窗口外的每帧轮着查几行（看不见那里，变了直接定格，不放动画）。
	/// 揭开的图块按离原先看得见的地方远近依次散开，云就像被从已探索的地方往外推开；重新被迷雾盖住时立即为 1，
	/// 否则那一瞬间完全盖住的图块地面是空的（只画覆盖层），会露出黑块
	std::vector<uint8_t> fogHidden;      ///< 0 看得见、1 在迷雾里、2 在迷雾里且在法术射程外
	std::vector<float> fogAmount;        ///< 云的浓度 0~1（未缓动）
	std::vector<float> fogDelay;         ///< 揭开后还要等多少秒才开始散
	std::vector<int> fogAnimating;       ///< 正在散开（或等着散开）的图块下标
	std::vector<int> fogRevealed;        ///< 这一帧刚揭开的图块下标，advanceFog() 里排好先后再放进 fogAnimating
	std::vector<uint8_t> fogRgba;        ///< 编码好的云雾场
	Point fogSize;
	int fogLevel = -1;
	int fogScanRow = 0;                  ///< 窗口外轮查到第几行
	bool fogDirty = false;               ///< 云雾场这一帧变了，要交给屏幕处理器
	std::chrono::steady_clock::time_point fogClock;
	/// 拍传送过渡的快照时用原版迷雾：快照是画进普通画布的，没有合成器画云
	bool forceOriginalFog = false;
	/// 这一帧的覆盖层是按云雾模式画的
	bool cloudFogActive = false;

	/// 换了地图层（或第一次）：整层查一遍迷雾，直接定格，不放动画
	void resetFogField(const std::shared_ptr<IMapRendererContext> & context);
	/// 可见窗口里的图块这一帧的迷雾状态（updateTile 从校验值读出）
	void noteFogState(const int3 & tile, uint8_t hidden);
	/// 窗口外每帧轮着查几行：别的英雄、盟友在别处揭开的地方，滚过去时不该再看到云散开
	void scanFogOffscreen(const std::shared_ptr<IMapRendererContext> & context);
	/// 推进散开的动画，云雾场变了就交给屏幕处理器
	void advanceFog();
	/// 按 fogAmount、fogHidden 重新编码一个图块
	void encodeFogTile(size_t index);

	/// Canvas of the given logical size, drawing onto a GPU render target when this view
	/// uses the GPU layer and onto a plain surface otherwise
	std::unique_ptr<Canvas> createCanvas(const Point & size) const;

	/// Allocates the canvases on first use. Deferred out of the constructor because that
	/// runs on the network thread, where creating a texture would steal the GL context
	void ensureCanvases();

	std::shared_ptr<CAnimation> iconsStorage;

	/// 察看世界（含侦察类法术）的地图标记层（2026-11-20）：地图照常分层交给合成器（才有水墨迷雾），
	/// 城镇、英雄、矿等图标画进这张与地图视图同大的透明画布，合成完地图后盖上去。不在察看世界时释放
	std::unique_ptr<Canvas> decorationLayer;
	/// 把当前视野里图块的标记画到 target（target 与地图视图同大、左上角对齐）
	void drawImageOverlay(const std::shared_ptr<IMapRendererContext> & context, Canvas & target);
	/// 察看世界时重画标记层并返回它；不需要标记时释放它、返回空
	const Canvas * updateDecorationLayer(const std::shared_ptr<IMapRendererContext> & context);

	void updateTile(const std::shared_ptr<IMapRendererContext> & context, const int3 & coordinates);

	/// 本帧要重画的图块。先在 updateTile() 里收集，等所有图块判断完再批量画：
	/// 每换一张目标画布，gpu 渲染器就要结束并新开一个渲染通道，逐块轮流换画布会慢到个位数帧率。
	struct PendingTile
	{
		int3 coordinates;
		TileChecksum checksum;
		/// 分层模式下要重画的层，位掩码，第 n 位对应 MapTileLayer 值为 n 的层。
		/// 只有物件动画换帧时只重画物件层，缩到最小时满屏的树、旗子才不会每次都把四层全重画一遍
		uint8_t layers;
		/// 完全被迷雾盖住（算校验值时顺带得出）：分层时除了覆盖层（云雾模式下连覆盖层）都是空的
		bool fullyHidden;
	};
	std::vector<PendingTile> pendingTiles;

	/// 把 pendingTiles 画完再记下各图块的校验值：分层时按 地面、水面遮罩、物件、摇摆物件、风场遮罩、起伏层、地形分布、覆盖 一层一层画，
	/// 不分层时整块画进 terrain；缓存存缩小图块时经图集分批中转
	void drawPendingLayers(const std::shared_ptr<IMapRendererContext> & context);

	/// 把缓存中的一块区域画到 target 上；分层模式下依次叠上地面、物件、摇摆物件、覆盖物（没有水面、风场特效，
	/// 摇摆物件一律叠在静止物件之上，站在树前的英雄这时会被树盖住；只在文字覆盖、传送过渡这类短暂画面里用到）
	void drawCachedArea(Canvas & target, const Rect & cacheArea, const Point & position, const Point & size) const;

	/// Walks the visible window of the cache. Because tiles are stored wrapped around on both
	/// axes it falls into at most four bands, each contiguous in the cache and on screen.
	void forEachCachedBand(const std::function<void(const Rect & cacheArea, const Point & targetPosition, const Point & targetSize)> & visit) const;

	/// Copies the entire cached tile window onto the target in as few blits as possible. Used
	/// when every tile has to be repainted anyway, above all while the view is scrolling.
	void renderCachedTiles(Canvas & target);

	std::shared_ptr<IImage> getOverlayImageForTile(const std::shared_ptr<IMapRendererContext> & context, const int3 & coordinates);

public:
	/// useGpuLayer must match how the owning view presents itself: a view drawn into the
	/// software screen cannot read from a GPU-backed cache
	MapViewCache(const std::shared_ptr<MapViewModel> & model, bool useGpuLayer);
	~MapViewCache();

	/// invalidates cache of specified object
	void invalidate(const std::shared_ptr<IMapRendererContext> & context, const ObjectInstanceID & object);

	/// 英雄走到了 tile（看得见的一步）：只记下，下一帧在渲染线程上决定要不要惊起附近树林里的鸟。可在任何线程调用
	void noteHeroStep(const int3 & tile);

	/// 鼠标悬停在哪件物件上（-1 为没有），描边用什么颜色（2026-11-18）。下一帧画描边
	void setHoveredObject(int objectID, MapHighlight::Tone tone);

	/// updates internal terrain cache according to provided time delta
	/// True when update() already ran this frame and the view still shows the same tiles. A scroll
	/// in between brings tiles into view whose cache slots were never filled for them.
	bool isUpdatedThisFrame() const;

	void update(const std::shared_ptr<IMapRendererContext> & context);

	/// renders updated terrain cache onto provided canvas
	void render(const std::shared_ptr<IMapRendererContext> & context, Canvas & target, bool fullRedraw);

	/// Hands the visible window of the cache to the screen handler, to be drawn onto the screen
	/// while the frame is composed rather than copied into a layer now. Only valid when nothing
	/// has to be drawn on top of the terrain - see needsOwnLayer(). targetArea is in screen pixels.
	/// 分层时察看世界的标记也在这里画好、随地图一起交出去（见 decorationLayer）
	void present(const std::shared_ptr<IMapRendererContext> & context, const Rect & targetArea);

	/// Whether this frame draws anything over the terrain, which needs a layer of its own
	bool needsOwnLayer(const std::shared_ptr<IMapRendererContext> & context) const;

	/// renders an entire map level at native tile size, without the hero path and debug overlays, into a PNG
	void exportMapLevel(const std::shared_ptr<IMapRendererContext> & context, int level, const boost::filesystem::path & path);

	/// creates snapshot of current view and stores it into internal canvas
	/// used for view transition, e.g. Dimension Door spell or teleporters (Subterra gates / Monolith)
	void createTransitionSnapshot(const std::shared_ptr<IMapRendererContext> & context);
};
