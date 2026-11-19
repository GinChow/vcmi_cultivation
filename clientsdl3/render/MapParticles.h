/*
 * MapParticles.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "IScreenHandler.h"

#include <SDL3/SDL_render.h>

#include <span>

struct SDL_GPUShader;
struct SDL_GPUSampler;
struct SDL_GPURenderState;

/// 大地图植被粒子（2026-11-17，Claude Code）：落叶、飞花、萤火虫、惊鸟。
///
/// 每帧在 CPU 上按植被场（每个地图图块落叶、飞花、萤火虫的多少，见 IScreenHandler::setMapVegetationField）和时间，
/// 用哈希算出可见窗口里每个粒子此刻的位置，画成一批小四边形；片元着色器只在这些四边形里画叶片、花瓣、光点、飞鸟和鸟影。
/// 成本只随粒子个数增长，与屏幕大小无关。最初的版本在全屏合成着色器里逐像素找附近的粒子，满屏树林、窗口又大时
/// 掉到 60 多帧（2026-11-17 用户反馈），因此改成现在这样。
///
/// 粒子不存状态：位置只由图块、槽位、周期和时间决定，所以锚在地图上，滚动画面时不会漂。
/// 落叶、飞花的出发点必须落在树冠或花丛上，颜色取那一点的树冠颜色：这一步要看风场遮罩和摇摆层，在片元着色器里做
/// （出发点在缓存里的位置随顶点颜色传过去，同一片叶子的所有像素取的是同一点，结论一致）。
/// 惊鸟的鸟群由地图缓存登记（startBirdFlock），这里按起飞时间算每只鸟的位置。
///
/// 天气粒子（2026-11-18）：雪地上飘雪（一阵大一阵小）、岩浆和营火上飞起的余烬、沼泽上一团团升起的瘴气、
/// 阵雨落在水面上的雨丝。按地形场（每个图块的地形）放；阵雨落在哪由大块云影的形状决定，这里在 CPU 上
/// 用与合成着色器同一张噪声、同一个公式算（cloudShape、showerAt），雨丝才会落在水面涟漪的那片云下。
/// 粒子颜色跟着时段调色走（夜里偏蓝变暗），萤火虫、余烬入夜更亮。
///
/// 交互反馈（2026-11-18）：英雄走在沙地、泥地、雪地上时脚下扬起尘土雪沫（这一种要记状态：每帧按英雄的速度新扬起几团，
/// 各自减速、升高、胀开、散掉）；宝物、资源隔几秒在物件画面上随机一点闪一道十字星光（落点查物件层，透明处不画）；
/// 城镇屋顶冒出淡墨色的炊烟，顺风斜斜飘散。
///
/// 由 MapLayerCompositor 在合成完地面、物件之后，叠覆盖层（迷雾、路径）之前调用，所以粒子都在迷雾之下。
/// 只有 MSL 版着色器（macOS），着色器不可用时什么都不画。
class MapParticles
{
public:
	/// 一帧的对位参数和要取样的缓存纹理
	struct Frame
	{
		SDL_Texture * windMask = nullptr; ///< 风场遮罩：判断出发点在不在树冠、花丛上
		SDL_Texture * sway = nullptr;     ///< 摇摆层：树冠、花丛的颜色
		Point slots;                       ///< 缓存的图块槽位数
		Point worldOrigin;                 ///< 可见窗口左上角图块的地图坐标
		Point slotOrigin;                  ///< 该图块在缓存中的槽位
		float cacheWidth = 0.0f;           ///< 缓存纹理的像素宽度
		float time = 0.0f;                 ///< 合成器的时钟（秒）
		float strength = 0.0f;             ///< 粒子强度 0~1：图块在屏幕上太小时为 0，整批不画
		float weatherStrength = 0.0f;      ///< 天气粒子（雪、余烬、瘴气、雨丝）的强度：要放大到图块够大才出现，缩到全图时不算
		std::array<float, 3> grade = {1.0f, 1.0f, 1.0f}; ///< 整幅画面的时段色调，粒子颜色也乘上它
		float night = 0.0f;                ///< 天黑的程度 0~1
		bool sky = true;                   ///< 头上有天：地下为假，不下雨、不飘雪（余烬、瘴气照旧）

		/// 会飞出余烬的火（营火、火柱）：地面位置、离地高度、相位
		struct Fire
		{
			float x;
			float y;
			float height;
			float seed;
		};
		std::vector<Fire> fires;

		SDL_Texture * objects = nullptr;   ///< 物件层：宝物闪光的落点要在物件上
		/// 粒子叠上去的方式：直接画到窗口上时按预乘 alpha；画进合成器的离屏图时不能抬高 alpha（那里是自发光）
		SDL_BlendMode blendMode = SDL_BLENDMODE_BLEND_PREMULTIPLIED;
		std::span<const MapSparkle> sparkles; ///< 会闪光的宝物、资源
		std::span<const MapChimney> chimneys; ///< 冒炊烟的屋顶
		std::span<const MapDustSource> dust;  ///< 正在走的英雄脚下
	};

	MapParticles() = default;
	~MapParticles();

	MapParticles(const MapParticles &) = delete;
	MapParticles & operator=(const MapParticles &) = delete;

	/// 植被场：rgba 按行存放 size.x × size.y 个地图图块（R 落叶、G 飞花、B 萤火虫、A 不用）。
	/// 换了地图或地图层，同时清掉还在飞的鸟群。须在渲染线程上调用
	void setVegetationField(const std::vector<uint8_t> & rgba, const Point & size);

	/// 地形场（含义见 IScreenHandler::setMapTerrainField），只用 R（每个图块的地形）放飘雪、余烬、瘴气、雨丝。须在渲染线程上调用
	void setTerrainField(const std::vector<uint8_t> & rgba, const Point & size);

	/// 合成着色器的噪声纹理（RGBA，size × size）：阵雨在哪要与着色器算得一样
	void setNoise(const std::vector<uint8_t> & pixels, int size);

	/// 大块云影在地图坐标 (x, y)、时刻 t 的形状，与合成着色器的 bigCloudShape 同一公式
	float cloudShape(float x, float y, float t) const;
	/// 这里此刻下多大的雨 0~1，与合成着色器的 showerAt 同一公式
	float showerAt(float x, float y, float t) const;

	/// 惊起一群鸟：从地图坐标 (x, y) 起飞，朝 (directionX, directionY) 飞走；now 是合成器的时钟。
	/// 同时最多 maxFlocks 群，再多就顶掉最早起飞的一群。须在渲染线程上调用
	void startBirdFlock(float x, float y, float directionX, float directionY, float now);

	/// 把粒子画到渲染器当前的渲染目标上。regions 与合成地图时相同（source 在缓存纹理内、target 在屏幕上），
	/// 粒子只画在这些区域里
	void draw(SDL_Renderer * renderer, const Frame & frame, const std::vector<PresentedRegion> & regions);

	/// 缓存画布即将销毁或替换：之后不能再用绑着它们的渲染状态。可在任何线程调用，下次绘制前在渲染线程上重建
	void forgetInputs();

	/// 释放全部 GPU 资源，必须在渲染器销毁之前调用
	void release();

	/// 一群鸟从起飞到飞远淡出的时间（秒）
	static constexpr float flockLifetime = 5.0f;

private:
	/// 正在飞的鸟群。birds 为 0 的是空位
	struct BirdFlock
	{
		float x = 0.0f;
		float y = 0.0f;
		float directionX = 1.0f;
		float directionY = 0.0f;
		float start = 0.0f;
		float seed = 0.0f;
		int birds = 0;
	};
	static constexpr size_t maxFlocks = 4;
	std::array<BirdFlock, maxFlocks> flocks = {};
	std::mt19937 flockRandom{20261117};

	std::vector<uint8_t> vegetation;
	Point vegetationSize;

	/// 每个图块的地形（TerrainId，255 为模组新增地形），按行存放
	std::vector<uint8_t> terrain;
	Point terrainSize;

	std::vector<uint8_t> noise;
	int noiseSize = 0;
	/// 噪声纹理线性取样、环绕，与 GPU 的取样器一致；channel 0~3 为 RGBA
	float sampleNoise(float u, float v, int channel) const;
	/// 图块 (x, y) 的地形；地图外为 255
	uint8_t terrainAt(int x, int y) const;

	SDL_Renderer * owner = nullptr;
	SDL_GPUShader * shader = nullptr;
	SDL_GPUSampler * sampler = nullptr;
	SDL_GPURenderState * state = nullptr;
	/// 1×1 白图：画四边形时传给 SDL，使它用带纹理坐标的顶点着色器；片元着色器不取它
	SDL_Texture * white = nullptr;
	/// state 绑定的风场遮罩、摇摆层、物件层；换了就重建 state
	std::array<SDL_Texture *, 3> boundTextures = {};
	std::atomic<bool> inputsForgotten{false};
	/// 着色器路径已确认不可用，本渲染器上不再重试
	bool unavailable = false;

	bool ensureResources(SDL_Renderer * renderer, const Frame & frame);

	/// 地图坐标（图块）到屏幕像素的换算：屏幕位置 = origin + (地图坐标 − world) × tilePixels
	struct Placement
	{
		float originX = 0.0f;    ///< 可见窗口左上角图块的左上角在屏幕上的位置
		float originY = 0.0f;
		float worldX = 0.0f;     ///< 该图块的地图坐标
		float worldY = 0.0f;
		float tilePixels = 0.0f; ///< 一个图块在屏幕上多少像素
	};

	/// 扬起的一团尘土（雪沫）：在地图坐标里，按合成器的时钟算年龄
	struct DustPuff
	{
		float x = 0.0f;
		float y = 0.0f;
		float vx = 0.0f;
		float vy = 0.0f;
		float born = 0.0f;
		float life = 1.0f;
		float size = 0.05f;
		float strength = 1.0f;
		MapDustSource::Kind kind = MapDustSource::Kind::SAND;
	};
	std::vector<DustPuff> dustPuffs;
	/// 每个英雄攒着的“还差多少团没扬起”（物件编号 → 小数），扬尘的多少因此与帧率无关
	std::unordered_map<int, float> dustCarry;
	float dustClock = -1.0f; ///< 上一次更新扬尘的时刻；小于 0 表示还没更新过
	std::mt19937 dustRandom{20261118};
	/// 按这一帧的英雄扬起新的尘土、去掉散完的
	void updateDust(const Frame & frame);

	/// 这一帧的四边形，按画的先后：鸟影、瘴气、尘土与炊烟、落叶飞花、萤火虫与余烬与宝物闪光、飞鸟、雨丝、飘雪
	std::vector<SDL_Vertex> shadowVertices;
	std::vector<SDL_Vertex> miasmaVertices;
	std::vector<SDL_Vertex> puffVertices;
	std::vector<SDL_Vertex> bitVertices;
	std::vector<SDL_Vertex> glowVertices;
	std::vector<SDL_Vertex> birdVertices;
	std::vector<SDL_Vertex> rainVertices;
	std::vector<SDL_Vertex> snowVertices;
	std::vector<SDL_Vertex> vertices;
	std::vector<int> indices;

	/// 加一个四边形：中心 center、两个半轴 axisX、axisY（都以图块为单位），纹理坐标在 ±extentX、±extentY 之间，
	/// kind 是粒子种类（片元着色器据此决定画什么，编进纹理坐标 y），color 是交给片元着色器的参数
	void addQuad(std::vector<SDL_Vertex> & target, const Placement & placement, float centerX, float centerY,
		float axisXx, float axisXy, float axisYx, float axisYy, float extentX, float extentY, int kind, const SDL_FColor & color) const;

	void buildBirds(const Placement & placement, const Frame & frame);
	void buildDriftingBits(const Placement & placement, const Frame & frame);
	void buildFireflies(const Placement & placement, const Frame & frame);
	void buildSnow(const Placement & placement, const Frame & frame);
	void buildEmbers(const Placement & placement, const Frame & frame);
	void buildMiasma(const Placement & placement, const Frame & frame);
	void buildRain(const Placement & placement, const Frame & frame);
	void buildDust(const Placement & placement, const Frame & frame);
	void buildSmoke(const Placement & placement, const Frame & frame);
	void buildSparkles(const Placement & placement, const Frame & frame);
};
