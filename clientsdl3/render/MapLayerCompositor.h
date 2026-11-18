/*
 * MapLayerCompositor.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "IScreenHandler.h"
#include "MapParticles.h"

struct SDL_Renderer;
struct SDL_Texture;
struct SDL_GPUShader;
struct SDL_GPUSampler;
struct SDL_GPURenderState;

/// 大地图分层合成（2026-11-17，Claude Code，大地图水面特效、风场摇摆）。
///
/// 地图缓存在分层模式下拆成地面、静止物件、摇摆物件、覆盖物四层，外加水面遮罩和风场遮罩（见 MapViewCache）。
/// 每帧合成屏幕时，这里用一个 MSL 片元着色器一次画完：先在地面层的水面上做折射扰动、深浅色调、
/// 岸边物件的倒影、近岸浪花和波光；再按风场把摇摆层的树木吹弯（阵风沿风向扫过林梢、每棵树相位错开地摆、
/// 叶片细颤），按风场遮罩记下的前后关系与静止物件叠好。这一遍之后再画植被粒子（落叶、飞花、萤火虫、惊鸟，
/// 见 MapParticles，画成一批小四边形），最后按预乘 alpha 叠上覆盖层，所以粒子都在迷雾之下。
/// 物件、迷雾、路径因此不会被水波扭曲，站在树前的英雄也不会被摆过来的树冠盖住。
///
/// 光影（2026-11-17）：地图缓存另给一张起伏层（物件表面的法线和离地高度）。每帧先把它缩成 1/4，用第二个着色器
/// 逐点朝太阳方向找遮挡，算出整屏的长投影（不受图块裁切，山影能落到旁边的林子上）；合成时物件按法线重新打光、
/// 加边缘光，再叠流动的雾（贴着山脚、林地、水边）和山腰云海（有视差，峰顶伸出云层）。
///
/// 昼夜与天气（2026-11-18）：时段默认跟着回合走（设置 video.mapSunClock）——本方回合开始是清晨，英雄用掉行动力太阳就西斜，
/// 行动力用完是黄昏，等别家行动时是夜里。影子方向、长短、光色、整屏色调（清晨偏暖、黄昏偏橙、夜里偏蓝）跟着变；
/// 天越暗，城镇灯笼、营火、法光、熔岩、英雄火把这些局部光（MapLight，最多 maxLights 盏）越显眼，照亮周围的地面和物件。
/// 大块云影顺风缓缓飘过全图，有的云下着阵雨，雨点在水面上打出涟漪；沙地、岩浆上热浪扭曲，沼泽上飘着黄绿的瘴气，
/// 飘雪、余烬、瘴气团和雨丝由 MapParticles 画。
///
/// 水墨云海迷雾（2026-11-18）：分层合成时地图缓存的覆盖层不画原版的黑色迷雾，改给一张每图块一个像素的云雾场（setFogField）。
/// 粒子画完后，第三个着色器按它画流动的云（宣纸留白、淡墨勾边，边缘不规则地慢慢淡开），再把覆盖层（路径、图标）叠在云上。
/// 探索时地图缓存让揭开处的云按离已探索处的远近依次散去，着色器让它们翻卷着往迷雾深处推开。
///
/// 交互反馈与全屏效果（2026-11-18）：整幅地图（地面与物件、粒子、悬停描边、云雾与覆盖层）先画进一张离屏图，主着色器把
/// 每个像素自发光的程度（熔岩裂缝、水面波光、灯火的辉光）写在 alpha 里；从它缩出 1/4、1/16 两级辉光图，最后由后处理着色器
/// 画到屏幕上：叠辉光、统一成水墨的色调（收掉过艳的颜色、暗处偏墨青、亮处偏纸暖、黑位抬成墨色），盖一层跟着地图走的宣纸纤维，
/// 四角轻微压暗，地图边缘一圈洇开的淡墨。英雄在沙地、雪地上留下的蹄印画在地面上（物件之下）；扬尘、宝物闪光、城镇炊烟由
/// MapParticles 画；鼠标悬停的物件由地图缓存单独画成一张小画布，这里按它的 alpha 描一圈发光的边。
///
/// 性能：着色器对每个屏幕像素运行，满屏是水时取样次数决定帧率。离岸远近、向上找岸线这类要看一大片
/// 遮罩的计算，都改在每帧逐级缩小出的 1/8、1/16 遮罩小图上做，全分辨率纹理每个水面像素只取约十次。
/// 海面的近海、深海按每个图块离最近陆地的距离上色，这张距离图按地图坐标取样，与屏幕边缘、缩放无关。
///
/// 着色器只能挂在 SDL3 的 gpu 渲染驱动上（SDL_GPURenderState），并且目前只提供 MSL（macOS）。
/// 着色器不可用或创建失败时退回逐层混合，画面与单层缓存一致，只是没有水面特效。
class MapLayerCompositor
{
public:
	/// 一帧要合成的六张缓存纹理及对位参数，布局说明见 MapLayerSet
	struct Inputs
	{
		SDL_Texture * ground = nullptr;
		SDL_Texture * objects = nullptr;
		SDL_Texture * sway = nullptr;
		SDL_Texture * overlay = nullptr;
		SDL_Texture * waterMask = nullptr;
		SDL_Texture * windMask = nullptr;
		SDL_Texture * relief = nullptr;
		SDL_Texture * splat = nullptr;
		Point slots;
		Point worldOrigin;
		Point slotOrigin;
		int tileSize = 32; ///< 屏幕上一个图块的大小（界面像素），不到 16 时起伏层等效果层是空的（见 MapLayerSet）
		Rect viewArea;     ///< 地图视图在屏幕上的矩形（见 MapLayerSet）
	};

	MapLayerCompositor() = default;
	~MapLayerCompositor();

	MapLayerCompositor(const MapLayerCompositor &) = delete;
	MapLayerCompositor & operator=(const MapLayerCompositor &) = delete;

	/// 该渲染器能否挂本类的着色器：渲染驱动为 gpu，且其 GPU 设备接受 MSL
	static bool isShaderPathAvailable(SDL_Renderer * renderer);

	/// 每帧在画到窗口之前调用：把水面遮罩逐级缩小成着色器要用的小图，算长投影，再把整幅地图画进离屏图、缩出辉光图。
	/// 会切换渲染目标，所以必须在本帧往窗口上画任何东西之前完成。regions 与 compose() 的相同
	void prepare(SDL_Renderer * renderer, const Inputs & inputs, const std::vector<PresentedRegion> & regions);

	/// 把地图画到渲染器当前的渲染目标（窗口）上：prepare() 画好了离屏图时用后处理着色器画，否则退回直接合成。
	/// regions 的 source 在缓存纹理内、target 在屏幕上，各张缓存纹理共用同一组 source。scaleMode 是这次绘制本该用的缩放过滤方式（退回路径使用）。
	void compose(SDL_Renderer * renderer, const Inputs & inputs, const std::vector<PresentedRegion> & regions, int scaleMode);

	/// 设置离岸距离图：distances 按行存放 size.x × size.y 个图块，0 为陆地，255 为离岸 8 个图块及以上。
	/// 换地图或切换地上地下时由地图缓存调用，须在渲染线程上调用
	void setDepthField(const std::vector<uint8_t> & distances, const Point & size);

	/// 设置植被场（含义见 IScreenHandler::setMapVegetationField），转给 MapParticles，同时清掉还在飞的鸟群。须在渲染线程上调用
	void setVegetationField(const std::vector<uint8_t> & rgba, const Point & size);

	/// 设置雾云场（含义见 IScreenHandler::setMapAtmosphereField）：与离岸距离图共用一张纹理，存在它的 G、B、A 通道。须在渲染线程上调用
	void setAtmosphereField(const std::vector<uint8_t> & rgba, const Point & size);

	/// 设置地形场（含义见 IScreenHandler::setMapTerrainField）。须在渲染线程上调用
	void setTerrainField(const std::vector<uint8_t> & rgba, const Point & size);

	/// 设置地表材质图集（PNG 文件内容，含义见 IScreenHandler::setMapTerrainMaterials），在渲染线程上按需解码。须在渲染线程上调用
	void setTerrainMaterials(const std::vector<uint8_t> & png);

	/// 本方这一天的进度（含义见 IScreenHandler::setMapDayProgress），设置 video.mapSunClock 为 turn 时推动太阳。须在渲染线程上调用
	void setDayProgress(int day, std::optional<float> dayProgress);

	/// 这一帧物件带出的效果（局部光、宝物闪光、炊烟、脚印、扬尘、悬停描边，见 MapObjectEffects）。须在渲染线程上调用
	void setObjectEffects(const MapObjectEffects & effects);

	/// 合成着色器一帧最多照几盏局部光：多出来的取离屏幕中心最近的。着色器按 32 位掩码记每格有哪些灯，所以不能超过 32
	static constexpr size_t maxLights = 32;
	/// 合成着色器一帧最多画几对蹄印：多出来的取最新的（地图缓存按先后给）
	static constexpr size_t maxFootprints = 64;
	/// 局部光分格：可见窗口均分成这么多 × 这么多格，与着色器的 lightGridSize 一致
	static constexpr int lightGridSize = 16;

	/// 地表材质图集的布局：8 行（材质组，与 TerrainId 0~7 一致）× 4 列（子材质），每格是正方形，四周 8 像素是环绕的内容
	static constexpr int materialSets = 8;
	static constexpr int materialsPerSet = 4;

	/// 设置云雾场（含义见 IScreenHandler::setMapFogField）。须在渲染线程上调用
	void setFogField(const std::vector<uint8_t> & rgba, const Point & size);

	/// 着色器在这个渲染器上建不起来（或缺纹理）、已退回逐层叠加：这时分层的地图缓存画不出云，屏幕处理器据此让它退回原版单层缓存
	bool isShaderBroken() const { return shaderUnavailable; }

	/// 惊起一群鸟（见 IScreenHandler::startMapBirdFlock），转给 MapParticles。须在渲染线程上调用
	void startBirdFlock(float x, float y, float directionX, float directionY);

	/// 地图缓存的画布即将销毁或替换：之后不能再用绑着它们的渲染状态。可在任何线程调用，
	/// 实际的重建留到下一次 compose()，在渲染线程上进行。
	void forgetInputs();

	/// 释放全部 GPU 资源。必须在渲染器销毁之前调用：着色器、采样器属于渲染器的 GPU 设备。
	void release();

private:
	/// 水面遮罩缩小的级数：第 i 级是原尺寸的 1/2^(i+1)，着色器用最后两级（1/8、1/16）
	static constexpr size_t maskLevelCount = 4;

	SDL_Renderer * owner = nullptr;
	SDL_GPUShader * shader = nullptr;
	SDL_GPUSampler * sampler = nullptr;
	SDL_Texture * noise = nullptr;
	SDL_GPURenderState * state = nullptr;

	std::array<SDL_Texture *, maskLevelCount> maskLevels = {};
	Point maskLevelsSourceSize;

	/// 长投影：起伏层逐级缩小成 1/2、1/4，预处理着色器在 1/4 起伏层上朝太阳找遮挡，结果画进同样大小的光照图
	SDL_GPUShader * shadowShader = nullptr;
	SDL_GPURenderState * shadowState = nullptr;
	std::array<SDL_Texture *, 2> reliefLevels = {};
	SDL_Texture * lightMap = nullptr;
	Point reliefLevelsSourceSize;

	/// state 创建时绑定的纹理（七张缓存、两级遮罩小图、离岸距离图、光照图、材质图集、地形场）；任一张换了就要重建 state
	std::array<SDL_Texture *, 13> boundTextures = {};

	/// 地表材质图集：PNG 内容由地图缓存给，渲染线程上解码成纹理；同时算出每格的平均颜色（六边形随机铺贴保持方差要用）
	std::vector<uint8_t> materialsPng;
	bool materialsDirty = false;
	SDL_Texture * materialAtlas = nullptr;
	bool materialsLoaded = false; ///< 图集有效（模组提供了、解码成功、尺寸对）
	Point materialAtlasSize;
	int materialCell = 0;
	std::array<std::array<float, 4>, 32> materialMeans = {};

	/// 解码图集、建纹理；没有图集时建一张 1×1 的占位，materialsLoaded 为假
	bool ensureMaterialAtlas(SDL_Renderer * renderer);

	/// 这一帧的太阳：在 prepare() 里按设置和时间算好，合成时用
	struct Sun
	{
		std::array<float, 3> direction = {0.0f, 0.0f, 1.0f}; ///< 指向太阳（屏幕坐标：x 右、y 上、z 朝镜头）
		std::array<float, 2> shadow = {-1.0f, 0.0f};        ///< 离地 1 个图块的点，影子在屏幕上落出多远（图块）
		std::array<float, 3> color = {1.0f, 1.0f, 1.0f};    ///< 阳光颜色
		std::array<float, 3> shade = {0.8f, 0.86f, 1.0f};   ///< 背阴处的色调
		std::array<float, 3> grade = {1.0f, 1.0f, 1.0f};    ///< 整幅画面的时段色调
		float relight = 0.0f;  ///< 按法线重新打光的强度
		float rim = 0.0f;      ///< 边缘光强度
		float strength = 0.0f; ///< 长投影的浓度（日出日落时淡出）
		float reach = 0.0f;    ///< 预处理着色器的射线最高找到多高（图块）
		float night = 0.0f;       ///< 天黑的程度：白天 0，入夜 1
		float lamps = 0.0f;       ///< 局部光显出来多少：正午几乎看不出，黄昏亮起来，夜里最亮
		float desaturate = 0.0f;  ///< 夜里颜色褪掉多少（人眼在暗处辨色差）
		float heat = 0.0f;        ///< 沙地热浪的强度：正午最强，夜里没有（岩浆上不论早晚都有）
		float cloudShadow = 0.0f; ///< 大块云影的浓度：太阳越高越清楚，夜里没有
	};
	Sun sun;

	/// 时段：0 日出、0.5 正午、1 日落，1~1.5 是夜里，1.5 又是日出（取模）
	static constexpr float dayCycle = 1.5f;
	/// 某个时段的太阳、天色
	static Sun sunAt(float phase);

	/// 按设置 video.mapSunClock 推进时段：turn 跟着本方回合（setDayProgress），cycle 按 video.mapSunCycle 秒转一整天，
	/// fixed 定在 video.mapSunTime。跟回合走时只往前走、平滑地追上目标，跨回合时经过夜里到第二天清晨
	void advanceDayClock();
	float dayPhase = -1.0f;              ///< 此刻的时段；小于 0 表示还没开始，第一帧直接跳到目标
	std::optional<float> dayProgress = 0.0f; ///< 最近一次收到的本方进度，空为不在本方回合
	int dayNumber = 0;                   ///< 最近一次收到的是哪一天
	float dayProgressMax = 0.0f;         ///< 本方这一天走到过的最远进度：英雄途中补了行动力，天也不会倒回早上
	uint64_t dayClockTicks = 0;          ///< 上次推进时段的时间（毫秒）

	/// 这一帧物件带出的效果，由地图缓存给
	MapObjectEffects effects;

	/// 每个地图图块一个像素的场（离岸距离图兼雾云场）：RGBA 数据由地图缓存给，纹理在渲染线程上按需建。
	/// 着色器按地图坐标线性取样，地图外夹到边缘的值
	struct TileField
	{
		std::vector<uint8_t> rgba;
		Point size;
		bool dirty = true;
		SDL_Texture * texture = nullptr;
	};
	TileField depthField;
	TileField terrainField;
	SDL_GPUSampler * clampSampler = nullptr;

	/// 建好或更新场的纹理；还没收到数据时用 1×1 的 fallback
	bool ensureFieldTexture(SDL_Renderer * renderer, TileField & field, const std::array<uint8_t, 4> & fallback, const char * name);

	/// 水墨云海迷雾：云雾着色器、它的渲染状态（绑云雾场与噪声；主纹理是覆盖层，每次绘制由 SDL 绑定）
	SDL_GPUShader * fogShader = nullptr;
	SDL_GPURenderState * fogState = nullptr;
	TileField fogField;
	std::array<SDL_Texture *, 2> fogBoundTextures = {};
	/// 与 inputsForgotten 同时置位：纹理可能换过（新纹理可能分到旧地址），云雾的渲染状态也要重建
	std::atomic<bool> fogInputsForgotten{false};
	bool ensureFogState(SDL_Renderer * renderer);
	/// 画覆盖层：云雾着色器可用时先画云、再把覆盖层叠在云上；不可用时直接叠覆盖层。blendMode 是覆盖层叠上去的方式
	void drawOverlay(SDL_Renderer * renderer, const Inputs & inputs, const std::vector<PresentedRegion> & regions, SDL_BlendMode blendMode);

	/// ---- 离屏图、辉光与后处理（2026-11-18） ----
	/// 离屏图：像素数与窗口上逻辑画面实际占的像素相同，逻辑分辨率与窗口相同，地图按与窗口上相同的坐标画进去；
	/// alpha 是自发光的程度（见 composeWithShader）
	SDL_Texture * frameTexture = nullptr;
	Point frameSize;        ///< 像素
	Point frameLogicalSize; ///< 逻辑尺寸（窗口的逻辑分辨率）
	/// 辉光：离屏图取自发光缩成 1/4（bloomLevels[0]），再模糊缩成 1/16（bloomLevels[1]）
	std::array<SDL_Texture *, 2> bloomLevels = {};
	SDL_GPUShader * bloomShader = nullptr;
	SDL_GPUShader * postShader = nullptr;
	SDL_GPUShader * highlightShader = nullptr;
	/// 取自发光、模糊各用一个渲染状态（同一个着色器，uniform 不同）；后处理绑两级辉光图和噪声
	SDL_GPURenderState * extractState = nullptr;
	SDL_GPURenderState * blurState = nullptr;
	SDL_GPURenderState * postState = nullptr;
	SDL_GPURenderState * highlightState = nullptr;
	std::array<SDL_Texture *, 3> postBoundTextures = {};
	/// 这一帧 prepare() 已把地图画进离屏图，compose() 只需画后处理
	bool framePrepared = false;
	/// 后处理在这个渲染器上建不起来：照旧把地图直接合成到窗口上（没有辉光、宣纸、暗角，其余特效照常）
	bool postUnavailable = false;
	/// 离屏图里要画的矩形（各区域外接矩形与离屏图求交，逻辑坐标）
	Rect frameBounds;
	/// 地图视图露在外面的矩形（视图矩形与 frameBounds 求交），暗角、边缘墨晕按它画
	Rect frameView;

	/// 粒子、悬停描边、云雾画进离屏图时用的混合方式：颜色照常按预乘 alpha 叠，alpha（自发光）被盖住多少就减多少，不会增加
	SDL_BlendMode emissionKeepingBlend = SDL_BLENDMODE_INVALID;

	/// 合成着色器能用（着色器资源、各张场纹理、材质图集、渲染状态都就绪）
	bool shaderReady(SDL_Renderer * renderer, const Inputs & inputs);
	/// 建好或按窗口尺寸重建离屏图（size 像素、logicalSize 逻辑尺寸）、辉光图，以及后处理用的着色器和渲染状态
	bool ensurePostResources(SDL_Renderer * renderer, const Point & size, const Point & logicalSize);
	void destroyPostTargets();
	/// 把地图画进离屏图并缩出辉光图（prepare() 调用），成功时 framePrepared 为真
	void renderFrame(SDL_Renderer * renderer, const Inputs & inputs, const std::vector<PresentedRegion> & regions);
	/// 把离屏图经后处理着色器画到当前渲染目标上
	void drawPost(SDL_Renderer * renderer, const Inputs & inputs, const std::vector<PresentedRegion> & regions);
	/// 悬停物件的发光描边，画在粒子之后、云雾之前
	void drawHighlight(SDL_Renderer * renderer, const Inputs & inputs, const std::vector<PresentedRegion> & regions);

	/// 植被与天气粒子：落叶、飞花、萤火虫、惊鸟、飘雪、余烬、瘴气团、雨丝，在合成之后、覆盖层之前画
	MapParticles particles;

	/// 着色器用的时钟（秒）。约 2.8 小时后从头开始，保证 float 精度足够让动画平滑
	static float shaderTime();

	/// 由 forgetInputs() 置位：缓存画布换过了，下次合成前必须重建 state
	std::atomic<bool> inputsForgotten{false};

	/// 着色器路径已确认不可用（驱动不对或创建失败），本渲染器上不再重试
	bool shaderUnavailable = false;

	bool ensureShaderResources(SDL_Renderer * renderer);
	bool ensureMaskLevels(SDL_Renderer * renderer, SDL_Texture * waterMask);
	bool ensureReliefLevels(SDL_Renderer * renderer, SDL_Texture * relief);
	bool ensureRenderState(SDL_Renderer * renderer, const Inputs & inputs);
	void destroyMaskLevels();
	void destroyReliefLevels();
	/// 把起伏层缩成 1/4，再用预处理着色器算出这一帧的长投影光照图
	void renderLightMap(SDL_Renderer * renderer, const Inputs & inputs);
	/// overlayBlend：粒子叠上去的方式（画进离屏图时不能抬高 alpha 里的自发光）
	void composeWithShader(SDL_Renderer * renderer, const Inputs & inputs, const std::vector<PresentedRegion> & regions, SDL_BlendMode overlayBlend);
	/// 退回路径：逐层叠地面、物件、摇摆物件（覆盖层由 drawOverlay() 画）
	void composeLayerByLayer(SDL_Renderer * renderer, const Inputs & inputs, const std::vector<PresentedRegion> & regions, int scaleMode) const;
};
