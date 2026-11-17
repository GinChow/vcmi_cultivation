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
	};

	MapParticles() = default;
	~MapParticles();

	MapParticles(const MapParticles &) = delete;
	MapParticles & operator=(const MapParticles &) = delete;

	/// 植被场：rgba 按行存放 size.x × size.y 个地图图块（R 落叶、G 飞花、B 萤火虫、A 不用）。
	/// 换了地图或地图层，同时清掉还在飞的鸟群。须在渲染线程上调用
	void setVegetationField(const std::vector<uint8_t> & rgba, const Point & size);

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

	SDL_Renderer * owner = nullptr;
	SDL_GPUShader * shader = nullptr;
	SDL_GPUSampler * sampler = nullptr;
	SDL_GPURenderState * state = nullptr;
	/// 1×1 白图：画四边形时传给 SDL，使它用带纹理坐标的顶点着色器；片元着色器不取它
	SDL_Texture * white = nullptr;
	/// state 绑定的风场遮罩、摇摆层；换了就重建 state
	std::array<SDL_Texture *, 2> boundTextures = {};
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

	/// 这一帧的四边形，按画的先后：鸟影、落叶飞花、萤火虫、飞鸟
	std::vector<SDL_Vertex> shadowVertices;
	std::vector<SDL_Vertex> bitVertices;
	std::vector<SDL_Vertex> glowVertices;
	std::vector<SDL_Vertex> birdVertices;
	std::vector<SDL_Vertex> vertices;
	std::vector<int> indices;

	/// 加一个四边形：中心 center、两个半轴 axisX、axisY（都以图块为单位），纹理坐标在 ±extentX、±extentY 之间，
	/// kind 是粒子种类（片元着色器据此决定画什么，编进纹理坐标 y），color 是交给片元着色器的参数
	void addQuad(std::vector<SDL_Vertex> & target, const Placement & placement, float centerX, float centerY,
		float axisXx, float axisXy, float axisYx, float axisYy, float extentX, float extentY, int kind, const SDL_FColor & color) const;

	void buildBirds(const Placement & placement, const Frame & frame);
	void buildDriftingBits(const Placement & placement, const Frame & frame);
	void buildFireflies(const Placement & placement, const Frame & frame);
};
