/*
 * IScreenHandler.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */

#pragma once

#include "lib/constants/Enumerations.h"
#include "lib/Rect.h"

class Point;
class Rect;

class Canvas;

/// GPU layers composited under the software screen, in draw order. Each is screen sized;
/// a layer is transparent wherever its owner has not drawn.
enum class GpuRenderLayer : uint8_t
{
	MAP,
	BATTLE,

	COUNT
};

/// State of the OS taskbar/dock progress indicator (Windows taskbar, macOS dock, Linux launchers)
enum class TaskbarProgress : uint8_t
{
	/// No progress indicator shown
	HIDDEN,
	/// Busy indicator with no known completion fraction
	INDETERMINATE,
	/// Indicator filled to a known completion fraction, in range [0, 1]
	NORMAL
};

/// One region of an offscreen canvas, drawn onto the screen while the frame is composed
struct PresentedRegion
{
	Rect source; ///< within the canvas
	Rect target; ///< in screen pixels
};

/// 大地图分层合成时，地面层以外交给屏幕的几张缓存画布与对位参数（2026-11-17，大地图水面特效）。
///
/// 这几张画布与地面层布局完全相同：按图块环形存放（地图 x 列的图块放在第 x mod 槽位宽 个槽位，行同理），
/// 所以合成着色器把纹理坐标取模就能越过环形接缝，取到屏幕上相邻的图块。
struct MapLayerSet
{
	const Canvas * objects = nullptr;   ///< 不摇摆的地图物件（含所有阴影、英雄、旗帜），透明底，预乘 alpha
	const Canvas * sway = nullptr;      ///< 随风摇摆的物件本体（树、灌木、花草），透明底，预乘 alpha
	const Canvas * overlay = nullptr;   ///< 行军路径、覆盖图标、战争迷雾，透明底，预乘 alpha
	const Canvas * waterMask = nullptr; ///< 水面遮罩，R 通道 1 为水、0 为岸
	const Canvas * windMask = nullptr;  ///< 风场遮罩：RGB 为摇摆物件的弯曲权重、叶片抖动、覆盖度（预乘），A 为上面盖着不摇摆物件的程度
	const Canvas * relief = nullptr;    ///< 起伏层：RG 为物件表面法线、B 为离地高度（预乘），A 为物件本体覆盖度；RGB 为 0 处是没有起伏图的物件
	const Canvas * splat = nullptr;     ///< 地形分布层：RGB 为本地形、泥地、沙地的比重（见 MapRendererTerrainSplat），之和不足 1 处保留图块原画
	Point slots;       ///< 缓存的图块槽位数（宽、高）
	Point worldOrigin; ///< 可见窗口左上角图块的地图坐标
	Point slotOrigin;  ///< 该图块在缓存中的槽位
	int tileSize = 32; ///< 屏幕上一个图块的大小（界面像素，不含缩放倍数）。不到 16 时摇摆层、风场遮罩、起伏层、地形分布层是空的
	Rect viewArea;     ///< 地图视图在屏幕上的矩形（屏幕像素）。各区域会伸出它半个图块（被界面盖住），暗角、边缘墨晕按它画
	/// 盖在合成好的地图上的标记（察看世界的城镇、英雄、矿等图标，2026-11-20）：与地图视图同大，透明底，预乘 alpha，
	/// 不经着色器（不吃迷雾、色调、辉光），合成完地图后原样贴到 viewArea。为空表示没有
	const Canvas * decorations = nullptr;
};

/// 大地图上的一盏局部光（2026-11-18，大地图光照）：城镇的灯笼、营火、灯塔、传送门和神殿的法光、熔岩、英雄手里的火把。
/// 由地图缓存每帧从可见的物件收集，合成着色器按它照亮周围的地面和物件；天越暗越明显，正午几乎看不出
struct MapLight
{
	/// 光的种类，决定怎么闪：火光跳动、法光缓慢明灭、熔岩慢慢呼吸、灯笼几乎不动
	enum class Flicker : uint8_t
	{
		STEADY,
		FIRE,
		MAGIC,
		LAVA
	};

	float x = 0.0f;      ///< 光源在地面上的位置（地图坐标，以图块为单位，可带小数）
	float y = 0.0f;
	float height = 0.5f; ///< 离地多高（图块），越高照得越开、物件顶上也亮
	float radius = 2.0f; ///< 照多远（图块），之外为 0
	float r = 1.0f;      ///< 颜色乘强度
	float g = 0.7f;
	float b = 0.4f;
	float halo = 0.0f;   ///< 夜里光源周围空气里的一团辉光，0~1
	Flicker flicker = Flicker::STEADY;
	float seed = 0.0f;   ///< 闪动的相位，每盏灯不同
};

/// 会隔一会儿闪一道光的宝物、资源（2026-11-18，大地图交互反馈）：宝箱、神器、资源堆、潘多拉魔盒、卷轴、漂流物。
/// 光点落在物件画面里随机的一点（粒子着色器查物件层，落在透明处的不画）
struct MapSparkle
{
	float x = 0.0f;          ///< 物件画面中心（地图坐标，图块）
	float y = 0.0f;
	float halfWidth = 0.35f; ///< 光点可能落的范围（图块）
	float halfHeight = 0.35f;
	float seed = 0.0f;       ///< 相位与落点的随机种子，每件物件不同
};

/// 冒炊烟的地方（2026-11-18）：城镇屋顶。烟从 (x, y) 往上 height 个图块处冒出，顺风飘散
struct MapChimney
{
	float x = 0.0f;
	float y = 0.0f;
	float height = 1.0f;
	float seed = 0.0f;
};

/// 英雄在沙地、雪地上留下的一对蹄印（2026-11-18）。地图缓存记下英雄走过的位置，按时间算好浓淡
struct MapFootprint
{
	/// 印在什么地上：颜色、深浅不同
	enum class Ground : uint8_t
	{
		SAND,
		SNOW
	};

	float x = 0.0f;      ///< 这一对蹄印的中点（地图坐标，图块）
	float y = 0.0f;
	float dirX = 1.0f;   ///< 走的方向（单位向量，地图坐标）
	float dirY = 0.0f;
	float alpha = 1.0f;  ///< 浓淡：刚踩下为 1，慢慢淡到 0
	Ground ground = Ground::SAND;
};

/// 正在走的英雄脚下（2026-11-18）：扬起尘土、雪沫。速度为 0 时不起尘
struct MapDustSource
{
	/// 扬起什么：沙尘、泥土、雪沫、火山灰、草屑
	enum class Kind : uint8_t
	{
		SAND,
		DIRT,
		SNOW,
		ASH,
		GRASS
	};

	float x = 0.0f;  ///< 脚下的地面位置（地图坐标，图块）
	float y = 0.0f;
	float vx = 0.0f; ///< 这一帧的速度（图块/秒）
	float vy = 0.0f;
	Kind kind = Kind::SAND;
	int id = 0;      ///< 英雄的物件编号：同一个英雄每帧的尘土接着上一帧算
};

/// 鼠标悬停的物件（2026-11-18）：地图缓存把它单独画进一张小画布（透明底），合成器按画布的 alpha 描一圈发光的边
struct MapHighlight
{
	/// 描边的颜色：自己的（青）、中立的宝物与建筑（金）、敌人（朱砂）
	enum class Tone : uint8_t
	{
		NEUTRAL,
		OWN,
		ENEMY
	};

	const Canvas * canvas = nullptr; ///< 物件单独画成的画布；为空表示没有悬停的物件
	float x = 0.0f;                  ///< 画布左上角的地图坐标（图块）
	float y = 0.0f;
	float width = 0.0f;              ///< 画布在地图上的大小（图块）
	float height = 0.0f;
	Tone tone = Tone::NEUTRAL;
	float strength = 1.0f;           ///< 刚移上去时淡入
};

/// 地图缓存每帧从看得见的物件收集、交给合成器的东西（2026-11-18）：局部光、宝物闪光、炊烟、脚印、扬尘、悬停描边
struct MapObjectEffects
{
	std::vector<MapLight> lights;
	std::vector<MapSparkle> sparkles;
	std::vector<MapChimney> chimneys;
	std::vector<MapFootprint> footprints;
	std::vector<MapDustSource> dust;
	MapHighlight highlight;
};

class IScreenHandler
{
public:
	virtual ~IScreenHandler() = default;

	/// Updates window state after fullscreen state has been changed in settings
	virtual bool onScreenResize(bool keepWindowResolution) = 0;

	/// Fills screen with black color, erasing any existing content
	virtual void clearScreen() = 0;

	/// Returns canvas that can be used to display objects on screen
	virtual Canvas getScreenCanvas() const = 0;

	/// Synchronizes internal screen texture. Screen canvas may not be modified during this call
	virtual void updateScreenTexture() = 0;

	/// Presents screen texture on the screen
	virtual void presentScreenTexture() = 0;

	/// Returns list of resolutions supported by current screen
	virtual std::vector<Point> getSupportedResolutions() const = 0;

	/// Returns <min, max> range of possible values for screen scaling percentage
	virtual std::tuple<int, int> getSupportedScalingRange() const = 0;

	/// Converts provided rect from logical coordinates into coordinates within window, accounting for scaling and viewport
	virtual Rect convertLogicalPointsToWindow(const Rect & input) const = 0;

	/// Dimensions of render output
	virtual Point getRenderResolution() const = 0;

	/// Dimensions of logical output. Can be different if scaling is used
	virtual Point getLogicalResolution() const = 0;

	virtual int getInterfaceScalingPercentage() const = 0;

	virtual int getScalingFactor() const = 0;

	/// True when the selected upscaling filter also applies an FSR RCAS sharpen pass
	virtual bool isSharpeningEnabled() const = 0;

	/// Strength of the FSR RCAS sharpen pass, in AMD's own convention (0 = strongest)
	virtual float getSharpeningStrength() const = 0;

	virtual void screenShot() const = 0;

	/// Window has focus
	virtual bool hasFocus() = 0;

	virtual void setColorScheme(ColorScheme scheme) = 0;

	/// True when GPU layers are available, i.e. the driver granted us render targets
	virtual bool isGpuRenderingEnabled() const = 0;

	/// Canvas drawing into one of the GPU layers. Only valid while GPU rendering is active.
	/// Requesting it marks the layer as having content to composite.
	virtual Canvas getLayerCanvas(GpuRenderLayer layer) = 0;

	/// Gives a layer back once its owner is gone, so that its last frame stops showing
	/// through. Safe to call from any thread; acted on at the start of the next frame.
	virtual void releaseLayer(GpuRenderLayer layer) = 0;

	/// Clears any layer released since the last frame. Must run before windows redraw, so
	/// that an owner that is merely covered rather than gone can reclaim its layer.
	virtual void clearReleasedLayers() = 0;

	/// Offscreen canvas of the given logical size. Drawn by the GPU where this backend
	/// supports it, into a plain surface otherwise. The canvas owns whatever backs it.
	virtual Canvas createOffscreenCanvas(const Point & size) const = 0;

	/// Largest render target the GPU driver will create, in pixels along one dimension.
	/// A caller sizing an offscreen canvas from map or window state should stay under this,
	/// rather than rely on the surface fallback createOffscreenCanvas takes on failure.
	virtual int maxOffscreenCanvasSize() const = 0;

	/// Hands everything drawn so far to the GPU instead of leaving it queued. Lets drawing
	/// that a later pass reads back start early, rather than stalling on the first read.
	virtual void flushRenderCommands() = 0;

	/// Sets the OS taskbar/dock progress indicator, mirroring the adventure map's own
	/// hourglass while another player (AI or hotseat) is taking their turn. value is only
	/// used when state is NORMAL, and must be in range [0, 1].
	virtual void setTaskbarProgress(TaskbarProgress state, float value) = 0;

	/// Flashes the window's taskbar/dock entry if it does not currently have focus, so that
	/// a notification is not missed while the game is in the background. No-op if focused.
	virtual void flashWindowIfUnfocused() = 0;

	/// Draws parts of an offscreen canvas onto the screen while the frame is composed, in place of
	/// the given layer, instead of copying them into that layer during the frame.
	///
	/// Reading a render target in the middle of a frame makes a tiling GPU resolve it right then.
	/// On some Android drivers that reorganises memory and stalls every process on the device for
	/// a fifth of a second; the same read costs nothing once the frame is being handed over.
	///
	/// What is registered here replaces the previous registration and stays until it is replaced,
	/// or clearPresentedCanvas() is called - a layer keeps its content across frames, and a window
	/// opening over the map must not blank it. The layer it stands in for stops being composited.
	virtual void presentFromCanvas(GpuRenderLayer layer, const Canvas & source, const std::vector<PresentedRegion> & regions) = 0;

	/// Forgets what presentFromCanvas() registered. Must be called before the canvas it refers to
	/// is destroyed, since only the caller knows when that happens.
	virtual void clearPresentedCanvas(GpuRenderLayer layer) = 0;

	/// 能否用着色器分层合成大地图：要求 GPU 层可用、渲染驱动为 gpu 且接受 MSL 着色器，
	/// 并且设置 video.mapEffects 打开。不满足时大地图照旧单层缓存，没有水面特效。
	virtual bool isMapLayeringSupported() const = 0;

	/// 与 presentFromCanvas(GpuRenderLayer::MAP, ...) 相同，但 ground 只含地面层：合成这一帧时，
	/// 由水面着色器处理地面里的水，再把物件层、覆盖层按预乘 alpha 叠上去。各画布的生命周期要求同上。
	virtual void presentMapLayers(const Canvas & ground, const MapLayerSet & layers, const std::vector<PresentedRegion> & regions) = 0;

	/// 海面的离岸距离图：distances 按行存放 size.x × size.y 个地图图块，0 为陆地，255 为离岸 8 个图块及以上。
	/// 水面着色器据此给海面分出浅滩、近海、深海。换地图或切换地上地下时由地图缓存调用
	virtual void setMapDepthField(const std::vector<uint8_t> & distances, const Point & size) = 0;

	/// 植被粒子场（2026-11-17）：rgba 按行存放 size.x × size.y 个地图图块，R 落叶、G 飞花、B 萤火虫、
	/// A 落叶飞花能飘到这里的程度。着色器据此放粒子；换地图或切换地上地下时由地图缓存调用，同时清掉还在飞的鸟群
	virtual void setMapVegetationField(const std::vector<uint8_t> & rgba, const Point & size) = 0;

	/// 惊起一群鸟：从地图坐标 (x, y)（以图块为单位，可带小数）的树上飞起，朝 (directionX, directionY) 飞走。
	/// 同时最多几群，再多就顶掉最早的一群
	virtual void startMapBirdFlock(float x, float y, float directionX, float directionY) = 0;

	/// 雾云场（2026-11-17）：rgba 按行存放 size.x × size.y 个地图图块，G 生雾多少、B 山腰绕云多少，
	/// A 地表大尺度明暗、干湿变化的幅度（按地形，水面为 0）；R 不用。
	/// 着色器据此让流动的雾只在山脚、林地、水边浓起来，云海只绕着山。换地图或切换地上地下时由地图缓存调用
	virtual void setMapAtmosphereField(const std::vector<uint8_t> & rgba, const Point & size) = 0;

	/// 地形场（2026-11-17，2026-11-18 加天气）：rgba 按行存放 size.x × size.y 个地图图块。
	/// R 的低四位是该图块的地形（TerrainId 0~9：泥地、沙地、草地、雪地、沼泽、碎石、地下、岩浆、水、岩石；0~7 也是地表材质组，15 为模组新增的地形），
	/// 高四位是盖在这里的特殊地面（2026-11-19：0 没有或没有配材质，1 被诅咒的大地，2 魔法平原；材质组为 7 + 种类）；
	/// G 热浪多少（沙地、岩浆），B 瘴气多少（沼泽），A 熔岩的红光照到这里多少，三者都向四周晕开一两个图块。
	/// 换地图或切换地上地下时由地图缓存调用
	virtual void setMapTerrainField(const std::vector<uint8_t> & rgba, const Point & size) = 0;

	/// 大地图的时段（2026-11-18）：day 标出是哪一天（换了就是新的一天，从早上重新算）；dayProgress 是本方这一天过了多少，
	/// 0 为刚开始行动、1 为本方英雄的行动力都已用完，为空表示不在本方回合（等其他玩家、AI 行动），大地图入夜。
	/// 设置 video.mapSunClock 为 turn 时由它推动太阳，地图缓存每 0.25 秒调用一次
	virtual void setMapDayProgress(int day, std::optional<float> dayProgress) = 0;

	/// 这一帧可见范围里物件带出的效果（见 MapObjectEffects），地图缓存每帧调用。
	/// 局部光多于合成器能处理的盏数时只用离屏幕中心最近的；悬停描边的画布要活到下一次调用
	virtual void setMapObjectEffects(const MapObjectEffects & effects) = 0;

	/// 地表材质图集（2026-11-17）：模组提供的 PNG 文件内容，8 行（材质组）× 4 列（子材质），RGB 颜色、A 高度。只给一次
	virtual void setMapTerrainMaterials(const std::vector<uint8_t> & png) = 0;

	/// 云雾场（2026-11-18，水墨云海迷雾）：rgba 按行存放 size.x × size.y 个地图图块，R 云的浓度（在迷雾里为 255，揭开后慢慢降到 0），
	/// G 正在散开的程度（散到一半最大），B 为 255 表示在冒险法术射程外（云压暗）。分层合成时覆盖层不画原版迷雾，由合成器据此画云。
	/// 换地图层时整张给一次，之后迷雾变化、云在散开时由地图缓存每帧调用
	virtual void setMapFogField(const std::vector<uint8_t> & rgba, const Point & size) = 0;

	/// 当前显示的地图层是不是地下（2026-11-19）：地下没有天，合成器不画云层、云影、阵雨和雨雪，水墨迷雾改成泼墨的幽暗。
	/// 换地图层时由地图缓存调用
	virtual void setMapUnderground(bool underground) = 0;
};
