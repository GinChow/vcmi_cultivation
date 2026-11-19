/*
 * MapParticles.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "MapParticles.h"

#include <SDL3/SDL.h>

namespace
{

/// 粒子片元着色器（MSL）。输入与 SDL gpu 渲染器内置的贴图顶点着色器对齐：颜色在 [[user(locn0)]]、纹理坐标在 [[user(locn1)]]。
///
/// 每个四边形是一个粒子。纹理坐标 x 是粒子局部坐标，y 是局部坐标加上 4 × 种类（种类见 ParticleKind），
/// 局部坐标都在 ±2 之内，所以 floor((y + 2) / 4) 就是种类。顶点颜色是给这个粒子的参数（四个顶点相同）：
///   落叶、飞花：rg 是出发点在缓存纹理里的坐标，b 是明暗的一半，a 是淡入淡出；
///   萤火虫：a 是这一刻的亮度；飞鸟：r 是扇翅的相位 0~1，a 是淡入淡出；鸟影：a 是浓淡；
///   雪花、瘴气、雨丝：a 是浓淡；余烬：a 是这一刻的亮度；
///   宝物闪光：rg 是落点在缓存纹理里的坐标（落点要在物件上），a 是这一刻的亮度；尘土、炊烟：rgb 是颜色，a 是浓淡。
/// 输出预乘 alpha，按 SDL_BLENDMODE_BLEND_PREMULTIPLIED 叠；萤火虫、余烬输出 alpha 为 0，于是成了加色的光。
/// uniform 的 grade 是时段色调（rgb）与天黑的程度（w），粒子的颜色跟画面一起调。
const char * const particleShaderSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct Params
{
	float4 grade; // rgb：时段色调；w：天黑的程度
};

struct main0_in
{
	float4 color [[user(locn0)]];
	float2 uv [[user(locn1)]];
};

struct main0_out
{
	float4 color [[color(0)]];
};

static inline float segmentDistance(float2 p, float2 a, float2 b)
{
	float2 pa = p - a;
	float2 ba = b - a;
	float h = saturate(dot(pa, ba) / max(dot(ba, ba), 1e-6));
	return length(pa - ba * h);
}

fragment main0_out main0(main0_in in [[stage_in]], constant Params & P [[buffer(0)]],
	texture2d<float> mainTex [[texture(0)]], sampler mainSmp [[sampler(0)]],
	texture2d<float> windTex [[texture(1)]], sampler windSmp [[sampler(1)]],
	texture2d<float> swayTex [[texture(2)]], sampler swaySmp [[sampler(2)]],
	texture2d<float> objectTex [[texture(3)]], sampler objectSmp [[sampler(3)]])
{
	float kind = floor((in.uv.y + 2.0) * 0.25);
	float2 local = float2(in.uv.x, in.uv.y - 4.0 * kind);
	float4 p = in.color;
	float4 result = float4(0.0);

	if(kind < 3.5)
	{
		// 落叶（0）、飞花（1~3 是取到绿叶时换用的花色）：四边形已按叶片的朝向、长短、翻面后的宽窄摆好，这里画椭圆
		float edge = length(local);
		float soft = max(fwidth(edge), 1e-4);
		float alpha = 1.0 - smoothstep(1.0 - soft, 1.0 + soft, edge);
		if(alpha <= 0.0)
			discard_fragment();

		// 出发点要在树冠（或花丛）上：有叶子，落叶还要离地够高。颜色取那一点的树冠颜色
		bool petal = kind > 0.5;
		float4 m = windTex.sample(windSmp, p.rg);
		if(m.b < 0.5 || m.g / m.b < 0.45 || (!petal && m.r / m.b < 0.12))
			discard_fragment();
		float4 canopy = swayTex.sample(swaySmp, p.rg);
		// 树冠颜色里有阴影，原样取会是一片暗斑；提亮一些，叶子飘到草地上才看得出颜色
		float3 tint = min(canopy.rgb / max(canopy.a, 0.001) * 1.25 + 0.04, float3(1.0));
		// 取到花丛的绿叶时换成花色：粉、白、淡黄
		if(petal && tint.g > max(tint.r, tint.b))
			tint = kind < 1.5 ? float3(0.96, 0.80, 0.84) : (kind < 2.5 ? float3(0.98, 0.96, 0.92) : float3(0.97, 0.86, 0.48));

		alpha *= p.a;
		result = float4(tint * P.grade.rgb * (p.b * 2.0) * alpha, alpha);
	}
	else if(kind < 4.5)
	{
		// 萤火虫：一个亮点外面罩一圈柔光，加色。局部坐标 1 对应 0.18 个图块。天黑了更亮
		float d2 = dot(local, local) * (0.18 * 0.18);
		float glow = exp(-d2 / 0.0005) + 0.3 * exp(-d2 / 0.007);
		result = float4(float3(0.88, 1.0, 0.55) * glow * p.a * (1.0 + 0.8 * P.grade.w), 0.0);
	}
	else if(kind < 5.5)
	{
		// 飞鸟：水墨“m”字。局部坐标 x 沿翼展、y 沿飞行方向，以半翼展为单位。每只翅膀两段：肩到肘略向前拱，
		// 肘到翼尖向后掠；扇下去（flap 大）时翼尖掠得更后
		float flap = p.r;
		float2 elbow = float2(0.5, 0.18 - 0.2 * flap);
		float2 tip = float2(1.0, -(0.1 + 0.55 * flap));
		float2 mirrored = float2(-1.0, 1.0);
		float dist = min(min(segmentDistance(local, float2(0.0), elbow), segmentDistance(local, elbow, tip)),
			min(segmentDistance(local, float2(0.0), elbow * mirrored), segmentDistance(local, elbow * mirrored, tip * mirrored)));
		float thick = 0.085 + 0.1 * saturate(1.0 - length(local)); // 身子粗、翼尖细
		float soft = max(fwidth(dist), 1e-4);
		float alpha = (1.0 - smoothstep(thick - soft, thick + soft, dist)) * p.a;
		result = float4(float3(0.10, 0.10, 0.12) * P.grade.rgb * alpha, alpha);
	}
	else if(kind < 6.5)
	{
		// 鸟影：一团柔和的影子；夜里没有日影
		float alpha = p.a * exp(-dot(local, local)) * (1.0 - P.grade.w);
		result = float4(0.0, 0.0, 0.0, alpha);
	}
	else if(kind < 7.5)
	{
		// 雪花：中间实、边上虚的白点，略带冷色
		float alpha = p.a * (1.0 - smoothstep(0.35, 1.0, length(local)));
		result = float4(float3(0.95, 0.97, 1.0) * P.grade.rgb * alpha, alpha);
	}
	else if(kind < 8.5)
	{
		// 余烬：橙红的火星，一个亮核加一圈小光晕，加色；夜里更显眼
		float d2 = dot(local, local);
		float glow = exp(-d2 * 6.0) + 0.25 * exp(-d2 * 1.2);
		result = float4(float3(1.0, 0.52, 0.16) * glow * p.a * (0.8 + 0.7 * P.grade.w), 0.0);
	}
	else if(kind < 9.5)
	{
		// 瘴气：一团边缘很虚的黄绿浊气
		float alpha = p.a * exp(-dot(local, local) * 2.0);
		result = float4(float3(0.58, 0.68, 0.40) * P.grade.rgb * alpha, alpha);
	}
	else if(kind < 10.5)
	{
		// 雨丝：细长的一道，沿下落方向两头渐细；局部 x 沿下落方向、y 横向
		float across = 1.0 - smoothstep(0.4, 1.0, abs(local.y));
		float along = smoothstep(-1.0, -0.2, local.x) * (1.0 - smoothstep(0.5, 1.0, local.x));
		float alpha = p.a * across * along;
		result = float4(float3(0.80, 0.86, 0.95) * P.grade.rgb * alpha, alpha);
	}
	else if(kind < 11.5)
	{
		// 宝物闪光：落点要在物件本体上（透明处、半透明的阴影上不闪）。一道细长的十字星光，斜角再两道短的，中间一个亮核，加色。
		// 夜里没有阳光，闪得弱些
		if(objectTex.sample(objectSmp, p.rg).a < 0.6)
			discard_fragment();
		float r = length(local);
		float2 d = float2(local.x + local.y, local.x - local.y) * 0.7071;
		float rays = exp(-abs(local.x) * 22.0) * exp(-abs(local.y) * 2.6) + exp(-abs(local.y) * 22.0) * exp(-abs(local.x) * 2.6);
		float diagonal = exp(-abs(d.x) * 26.0) * exp(-abs(d.y) * 5.5) + exp(-abs(d.y) * 26.0) * exp(-abs(d.x) * 5.5);
		float core = exp(-r * r * 45.0);
		float glow = (rays * 0.9 + diagonal * 0.35 + core * 1.2) * (1.0 - smoothstep(0.7, 1.0, r)) * p.a * (1.0 - 0.5 * P.grade.w);
		result = float4(float3(1.0, 0.95, 0.80) * glow, 0.0);
	}
	else
	{
		// 尘土、炊烟：一团边缘很虚的烟，颜色由顶点给；边上按角度略起伏，不是正圆。
		// 上面亮、下面暗一些，看得出是一团有体积的烟——与地面颜色相近时（浅色沙地上的沙尘）只靠颜色分不出来
		float angle = atan2(local.y, local.x);
		float wobble = 1.0 + 0.12 * sin(angle * 3.0 + p.r * 17.0) + 0.08 * sin(angle * 5.0 + p.g * 23.0);
		float d2 = dot(local, local) / (wobble * wobble);
		float alpha = p.a * exp(-d2 * 2.4);
		float shade = 1.08 - 0.22 * smoothstep(-1.0, 1.0, local.y);
		result = float4(p.rgb * shade * P.grade.rgb * alpha, alpha);
	}

	main0_out out;
	out.color = result;
	return out;
}
)MSL";

/// 粒子种类，编进纹理坐标 y（见着色器）
enum ParticleKind : int
{
	KIND_LEAF = 0,
	KIND_PETAL_PINK = 1,
	KIND_PETAL_WHITE = 2,
	KIND_PETAL_YELLOW = 3,
	KIND_FIREFLY = 4,
	KIND_BIRD = 5,
	KIND_SHADOW = 6,
	KIND_SNOW = 7,
	KIND_EMBER = 8,
	KIND_MIASMA = 9,
	KIND_RAIN = 10,
	KIND_SPARKLE = 11,
	KIND_PUFF = 12
};

/// 与粒子着色器的 Params 一一对应
struct ParticleUniforms
{
	float grade[4];
};

/// 局部坐标的上限：种类靠 floor((y + 2) / 4) 解出，局部坐标必须在 ±2 之内
constexpr float maxExtent = 1.9f;

struct Random4
{
	float x;
	float y;
	float z;
	float w;
};

float fract(float value)
{
	return value - std::floor(value);
}

/// 四个输入到四个 0~1 随机数的哈希（Dave Hoskins 的 hash44），粒子的一切随机性都出自它，同样的输入永远同样的结果
Random4 hash44(float a, float b, float c, float d)
{
	float x = fract(a * 0.1031f);
	float y = fract(b * 0.1030f);
	float z = fract(c * 0.0973f);
	float w = fract(d * 0.1099f);
	const float mix = x * (w + 33.33f) + y * (z + 33.33f) + z * (x + 33.33f) + w * (y + 33.33f);
	x += mix;
	y += mix;
	z += mix;
	w += mix;
	return {fract((x + y) * z), fract((x + z) * y), fract((y + z) * w), fract((z + w) * x)};
}

float smoothstep(float edge0, float edge1, float value)
{
	const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

/// 一群鸟起飞 age 秒后：离地高度和在地面上飞出的距离（图块）
float flockHeight(float age)
{
	return 0.35f * age + 0.10f * age * age;
}

float flockDistance(float age)
{
	return 0.75f * age + 0.22f * age * age;
}

constexpr float pi2 = 6.2831853f;

}

MapParticles::~MapParticles()
{
	if(shader || sampler || state || white)
		logGlobal->warn("Map particles destroyed without release()");
}

void MapParticles::setVegetationField(const std::vector<uint8_t> & rgba, const Point & size)
{
	vegetation = rgba;
	vegetationSize = size;

	// 换了地图或地图层，还在飞的鸟群属于原来的地方
	for(BirdFlock & flock : flocks)
		flock.birds = 0;
}

void MapParticles::setTerrainField(const std::vector<uint8_t> & rgba, const Point & size)
{
	terrain.resize(rgba.size() / 4);
	for(size_t i = 0; i < terrain.size(); ++i)
		terrain[i] = rgba[i * 4];
	terrainSize = size;
}

void MapParticles::setNoise(const std::vector<uint8_t> & pixels, int size)
{
	noise = pixels;
	noiseSize = size;
}

float MapParticles::sampleNoise(float u, float v, int channel) const
{
	if(noiseSize <= 0)
		return 0.5f;

	// 与 GPU 的线性过滤一样：像素中心在 (i + 0.5) / size，坐标环绕
	const float x = u * static_cast<float>(noiseSize) - 0.5f;
	const float y = v * static_cast<float>(noiseSize) - 0.5f;
	const float x0 = std::floor(x);
	const float y0 = std::floor(y);
	const float fx = x - x0;
	const float fy = y - y0;
	auto at = [&](float ix, float iy)
	{
		const int px = ((static_cast<int>(ix) % noiseSize) + noiseSize) % noiseSize;
		const int py = ((static_cast<int>(iy) % noiseSize) + noiseSize) % noiseSize;
		return static_cast<float>(noise[(static_cast<size_t>(py) * noiseSize + px) * 4 + channel]) / 255.0f;
	};
	const float top = at(x0, y0) + (at(x0 + 1.0f, y0) - at(x0, y0)) * fx;
	const float bottom = at(x0, y0 + 1.0f) + (at(x0 + 1.0f, y0 + 1.0f) - at(x0, y0 + 1.0f)) * fx;
	return top + (bottom - top) * fy;
}

float MapParticles::cloudShape(float x, float y, float t) const
{
	// 与合成着色器的 bigCloudShape 逐项相同
	const float px = x - 0.94f * t * 0.3f;
	const float py = y - 0.34f * t * 0.3f;
	const float warp = sampleNoise(px * 0.02f + 0.31f, py * 0.02f + 0.77f, 0);
	return sampleNoise(px * 0.0085f + warp * 0.06f, py * 0.0085f + 0.13f, 1) * 0.75f
		+ sampleNoise(px * 0.031f + 0.57f, py * 0.031f + 0.21f, 0) * 0.25f;
}

float MapParticles::showerAt(float x, float y, float t) const
{
	// 与合成着色器的 showerAt 逐项相同
	const float px = x - 0.94f * t * 0.3f;
	const float py = y - 0.34f * t * 0.3f;
	const float rainy = sampleNoise(px * 0.0045f + 0.71f, py * 0.0045f + 0.37f, 0);
	return smoothstep(0.56f, 0.66f, cloudShape(x, y, t)) * smoothstep(0.46f, 0.58f, rainy);
}

uint8_t MapParticles::terrainAt(int x, int y) const
{
	if(x < 0 || y < 0 || x >= terrainSize.x || y >= terrainSize.y || terrain.size() != static_cast<size_t>(terrainSize.x * terrainSize.y))
		return 255;
	return terrain[static_cast<size_t>(y) * terrainSize.x + x];
}

void MapParticles::startBirdFlock(float x, float y, float directionX, float directionY, float now)
{
	// 找空位或已经飞完的；都在飞就顶掉最早起飞的一群
	size_t slot = 0;
	for(size_t i = 0; i < flocks.size(); ++i)
	{
		const BirdFlock & flock = flocks[i];
		if(flock.birds <= 0 || now - flock.start > flockLifetime || now < flock.start)
		{
			slot = i;
			break;
		}
		if(flock.start < flocks[slot].start)
			slot = i;
	}

	const float length = std::hypot(directionX, directionY);
	std::uniform_real_distribution<float> unit(0.0f, 1.0f);

	BirdFlock & flock = flocks[slot];
	flock.x = x;
	flock.y = y;
	flock.directionX = length > 0.001f ? directionX / length : 1.0f;
	flock.directionY = length > 0.001f ? directionY / length : 0.0f;
	flock.start = now;
	flock.seed = unit(flockRandom) * 100.0f;
	flock.birds = 4 + static_cast<int>(unit(flockRandom) * 3.99f); // 4~7 只
}

void MapParticles::forgetInputs()
{
	inputsForgotten = true;
}

bool MapParticles::ensureResources(SDL_Renderer * renderer, const Frame & frame)
{
	if(owner != renderer)
	{
		// 换了渲染器：旧资源随旧渲染器一起没了（release() 已在销毁前调用），从头再来
		shader = nullptr;
		sampler = nullptr;
		state = nullptr;
		white = nullptr;
		boundTextures = {};
		unavailable = false;
		owner = renderer;
	}

	if(unavailable)
		return false;

	SDL_GPUDevice * device = static_cast<SDL_GPUDevice *>(SDL_GetPointerProperty(SDL_GetRendererProperties(renderer), SDL_PROP_RENDERER_GPU_DEVICE_POINTER, nullptr));
	if(!device)
	{
		unavailable = true;
		return false;
	}

	if(!shader)
	{
		SDL_GPUShaderCreateInfo shaderInfo;
		SDL_zero(shaderInfo);
		shaderInfo.code = reinterpret_cast<const Uint8 *>(particleShaderSource);
		shaderInfo.code_size = std::strlen(particleShaderSource);
		shaderInfo.entrypoint = "main0";
		shaderInfo.format = SDL_GPU_SHADERFORMAT_MSL;
		shaderInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
		shaderInfo.num_samplers = 4; // SDL 的主纹理（白图）、风场遮罩、摇摆层、物件层
		shaderInfo.num_uniform_buffers = 1;
		shader = SDL_CreateGPUShader(device, &shaderInfo);

		SDL_GPUSamplerCreateInfo samplerInfo;
		SDL_zero(samplerInfo);
		samplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
		samplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
		samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
		samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
		samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
		samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
		sampler = SDL_CreateGPUSampler(device, &samplerInfo);

		white = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, 1, 1);
		if(white)
		{
			const uint8_t pixel[4] = {255, 255, 255, 255};
			SDL_UpdateTexture(white, nullptr, pixel, 4);
			SDL_SetTextureBlendMode(white, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
		}

		if(!shader || !sampler || !white)
		{
			logGlobal->error("Failed to set up the map particle shader: %s - map drawn without particles", SDL_GetError());
			release();
			owner = renderer;
			unavailable = true;
			return false;
		}
	}

	const std::array<SDL_Texture *, 3> wanted = {frame.windMask, frame.sway, frame.objects};
	const bool forgotten = inputsForgotten.exchange(false);
	if(state && boundTextures == wanted && !forgotten)
		return true;

	if(state)
		SDL_DestroyGPURenderState(state);
	state = nullptr;
	boundTextures = {};

	std::array<SDL_GPUTextureSamplerBinding, 3> bindings = {};
	for(size_t i = 0; i < wanted.size(); ++i)
	{
		bindings[i].texture = wanted[i] ? static_cast<SDL_GPUTexture *>(SDL_GetPointerProperty(SDL_GetTextureProperties(wanted[i]), SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER, nullptr)) : nullptr;
		bindings[i].sampler = sampler;
		if(!bindings[i].texture)
			return false; // 这一帧缓存还没准备好，下一帧再试
	}

	SDL_GPURenderStateCreateInfo stateInfo;
	SDL_zero(stateInfo);
	stateInfo.fragment_shader = shader;
	stateInfo.num_sampler_bindings = static_cast<Sint32>(bindings.size());
	stateInfo.sampler_bindings = bindings.data();
	state = SDL_CreateGPURenderState(renderer, &stateInfo);
	if(!state)
	{
		logGlobal->error("Failed to create the map particle render state: %s - map drawn without particles", SDL_GetError());
		unavailable = true;
		return false;
	}

	boundTextures = wanted;
	return true;
}

void MapParticles::addQuad(std::vector<SDL_Vertex> & target, const Placement & placement, float centerX, float centerY,
	float axisXx, float axisXy, float axisYx, float axisYy, float extentX, float extentY, int kind, const SDL_FColor & color) const
{
	extentX = std::min(extentX, maxExtent);
	extentY = std::min(extentY, maxExtent);

	auto corner = [&](float sx, float sy)
	{
		const float worldX = centerX + axisXx * extentX * sx + axisYx * extentY * sy;
		const float worldY = centerY + axisXy * extentX * sx + axisYy * extentY * sy;
		SDL_Vertex vertex;
		vertex.position.x = placement.originX + (worldX - placement.worldX) * placement.tilePixels;
		vertex.position.y = placement.originY + (worldY - placement.worldY) * placement.tilePixels;
		vertex.color = color;
		vertex.tex_coord.x = extentX * sx;
		vertex.tex_coord.y = extentY * sy + 4.0f * static_cast<float>(kind);
		return vertex;
	};

	// 四个顶点按 左上、右上、右下、左下，绘制时每四个一组拆成两个三角形
	target.push_back(corner(-1.0f, -1.0f));
	target.push_back(corner(1.0f, -1.0f));
	target.push_back(corner(1.0f, 1.0f));
	target.push_back(corner(-1.0f, 1.0f));
}

void MapParticles::buildBirds(const Placement & placement, const Frame & frame)
{
	// 惊鸟：一群 4~7 只先后从树冠里飞起，越飞越高（屏幕上往上走、个头略变大）、越飞越快，几秒后飞远淡出；
	// 地上跟着淡淡的影子，飞得越高影子离得越远、越淡
	for(size_t i = 0; i < flocks.size(); ++i)
	{
		const BirdFlock & flock = flocks[i];
		const float age = frame.time - flock.start;
		if(flock.birds <= 0 || age < 0.0f || age > flockLifetime)
			continue;

		const float fadeOut = 1.0f - smoothstep(flockLifetime - 1.2f, flockLifetime, age);
		for(int b = 0; b < flock.birds; ++b)
		{
			const Random4 r = hash44(static_cast<float>(i), static_cast<float>(b), flock.seed, 5.0f);
			const float a = age - r.z * 0.45f; // 先后起飞
			if(a <= 0.0f)
				continue;

			const float height = flockHeight(a);
			const float distance = flockDistance(a);
			const float spread = 0.35f + 0.65f * std::clamp(a * 0.6f, 0.0f, 1.0f);
			const float groundX = flock.x + (r.w - 0.5f) * 0.4f + flock.directionX * distance + (r.x - 0.5f) * 0.8f * spread;
			const float groundY = flock.y + (r.z - 0.5f) * 0.4f + flock.directionY * distance + (r.y - 0.5f) * 0.5f * spread;
			const float bodyX = groundX;
			const float bodyY = groundY - height;

			// 屏幕上的飞行方向：地面上飞出去，加上往上升
			float forwardX = flock.directionX * (0.75f + 0.44f * a);
			float forwardY = flock.directionY * (0.75f + 0.44f * a) - (0.35f + 0.2f * a);
			const float forwardLength = std::max(std::hypot(forwardX, forwardY), 0.001f);
			forwardX /= forwardLength;
			forwardY /= forwardLength;
			const float sideX = -forwardY;
			const float sideY = forwardX;

			const float span = 0.085f * (1.0f + 0.25f * height); // 半翼展：飞高了离镜头近，略大
			const float flap = 0.5f + 0.5f * std::sin(a * (15.0f + 5.0f * r.w) + r.x * pi2);
			const float fade = smoothstep(0.0f, 0.15f, a) * fadeOut * frame.strength;

			addQuad(birdVertices, placement, bodyX, bodyY, sideX * span, sideY * span, forwardX * span, forwardY * span, 1.35f, 1.35f, KIND_BIRD, {flap, 0.0f, 0.0f, fade});

			// 影子落在鸟正下方的地面上，再顺着物件阴影的方向往左偏一点
			const float shadowSize = span * 0.8f + 0.02f * height;
			const float shadowAlpha = fade * 0.3f * (1.0f - std::clamp(height / 3.0f, 0.0f, 1.0f));
			addQuad(shadowVertices, placement, groundX - 0.25f * height, groundY + 0.1f * height, shadowSize, 0.0f, 0.0f, shadowSize, maxExtent, maxExtent, KIND_SHADOW, {0.0f, 0.0f, 0.0f, shadowAlpha});
		}
	}
}

void MapParticles::buildDriftingBits(const Placement & placement, const Frame & frame)
{
	// 落叶与飞花：可见窗口里每个图块有两个槽位，各按自己的周期（图块、槽位之间错开）至多掉一片，从图块里随机一点出发。
	// 叶片顺风（往右略往下）飘、一边下落一边左右荡、翻面，3.6~5 秒后落地淡出；花瓣更轻，先被风托高再飘远。
	// 出发点在不在树冠上、颜色如何，由片元着色器查缓存决定，所以出发点必须在可见窗口里（只遍历窗口内的图块）
	for(int y = 0; y < frame.slots.y; ++y)
	{
		for(int x = 0; x < frame.slots.x; ++x)
		{
			const int tileX = frame.worldOrigin.x + x;
			const int tileY = frame.worldOrigin.y + y;
			if(tileX < 0 || tileY < 0 || tileX >= vegetationSize.x || tileY >= vegetationSize.y)
				continue;

			const size_t index = (static_cast<size_t>(tileY) * vegetationSize.x + tileX) * 4;
			const float leafRate = vegetation[index] / 255.0f;
			const float petalRate = vegetation[index + 1] / 255.0f;
			const float rate = leafRate + petalRate;
			if(rate < 0.004f)
				continue;

			const float cellX = static_cast<float>(tileX);
			const float cellY = static_cast<float>(tileY);

			for(int slot = 0; slot < 2; ++slot)
			{
				// 周期：飘 3.6~5 秒，再歇 0.5~3 秒
				const Random4 r = hash44(cellX, cellY, 17.0f + static_cast<float>(slot) * 31.0f, 3.0f);
				const float life = 3.6f + 1.4f * r.z;
				const float period = life + 0.5f + 2.5f * r.w;
				const float clock = frame.time + r.x * period;
				const float cycle = std::floor(clock / period);
				const float age = clock - cycle * period;
				if(age > life)
					continue;

				// 这个周期掉不掉、掉的是叶还是花、从哪里掉
				const Random4 q = hash44(cellX, cellY, cycle, 29.0f + static_cast<float>(slot) * 13.0f);
				if(q.w >= rate)
					continue;
				const bool petal = q.w < petalRate;
				const float spawnX = cellX + q.x;
				const float spawnY = cellY + q.y;

				const float speed = petal ? 0.28f + 0.14f * q.z : 0.20f + 0.16f * q.z; // 顺风的速度（图块/秒）
				const float fall = petal ? 0.07f : 0.13f + 0.06f * r.y;                 // 下落的速度
				const float swing = age * (petal ? 1.7f : 2.4f) + q.z * pi2;
				const float lift = petal ? 0.30f * std::sin(std::clamp(age / life, 0.0f, 1.0f) * 3.1415927f) : 0.0f;
				const float positionX = spawnX + 0.94f * speed * age + (petal ? 0.14f : 0.10f) * std::sin(swing);
				const float positionY = spawnY + (0.17f * speed + fall) * age - lift - 0.03f * std::abs(std::cos(swing));

				// 形状：转动的椭圆，翻面时变窄、变暗；开头淡入，落地前淡出
				const float angle = r.y * pi2 + age * (petal ? 2.1f : 1.3f);
				const float flip = std::cos(age * (petal ? 4.3f : 3.1f) + r.x * pi2);
				const float size = petal ? 0.045f : 0.085f; // 半长（图块）
				const float width = size * (petal ? 0.75f : 0.45f) * (0.25f + 0.75f * std::abs(flip));
				const float fade = smoothstep(0.0f, 0.35f, age) * (1.0f - smoothstep(life - 0.8f, life, age)) * frame.strength;
				const float shade = (0.78f + 0.28f * std::abs(flip)) * (petal ? 1.05f : 1.0f);

				// 出发点在缓存纹理里的坐标（缓存按图块环形存放）
				const float spawnU = fract((spawnX - static_cast<float>(frame.worldOrigin.x) + static_cast<float>(frame.slotOrigin.x)) / static_cast<float>(frame.slots.x));
				const float spawnV = fract((spawnY - static_cast<float>(frame.worldOrigin.y) + static_cast<float>(frame.slotOrigin.y)) / static_cast<float>(frame.slots.y));

				int kind = KIND_LEAF;
				if(petal)
					kind = q.z < 0.4f ? KIND_PETAL_PINK : (q.z < 0.75f ? KIND_PETAL_WHITE : KIND_PETAL_YELLOW);

				// 四边形比椭圆略大，留出两个像素给边缘的抗锯齿
				const float axisX = std::cos(angle);
				const float axisY = std::sin(angle);
				const float marginX = 1.0f + 2.0f / std::max(size * placement.tilePixels, 0.5f);
				const float marginY = 1.0f + 2.0f / std::max(width * placement.tilePixels, 0.5f);
				addQuad(bitVertices, placement, positionX, positionY, axisX * size, axisY * size, -axisY * width, axisX * width, marginX, marginY, kind,
					{spawnU, spawnV, shade * 0.5f, fade});
			}
		}
	}
}

void MapParticles::buildFireflies(const Placement & placement, const Frame & frame)
{
	// 萤火虫：可见窗口里每个图块按密度至多一只，在图块里慢慢绕着走、时明时暗
	const float t = frame.time;
	for(int y = 0; y < frame.slots.y; ++y)
	{
		for(int x = 0; x < frame.slots.x; ++x)
		{
			const int tileX = frame.worldOrigin.x + x;
			const int tileY = frame.worldOrigin.y + y;
			if(tileX < 0 || tileY < 0 || tileX >= vegetationSize.x || tileY >= vegetationSize.y)
				continue;

			const float density = vegetation[(static_cast<size_t>(tileY) * vegetationSize.x + tileX) * 4 + 2] / 255.0f;
			if(density < 0.01f)
				continue;

			const Random4 r = hash44(static_cast<float>(tileX), static_cast<float>(tileY), 41.0f, 7.0f);
			if(r.w >= density)
				continue;

			const float blink = smoothstep(0.2f, 1.0f, std::sin(t * (0.8f + 0.6f * r.z) + r.y * 25.0f)) * frame.strength;
			if(blink < 0.01f)
				continue;

			const float positionX = static_cast<float>(tileX) + 0.2f + 0.6f * r.x
				+ 0.30f * std::sin(t * (0.31f + 0.2f * r.z) + r.x * pi2) + 0.07f * std::sin(t * 1.9f + r.y * 9.0f);
			const float positionY = static_cast<float>(tileY) + 0.2f + 0.6f * r.y
				+ 0.20f * std::sin(t * (0.23f + 0.15f * r.w) + r.y * pi2) + 0.06f * std::sin(t * 2.3f + r.x * 7.0f);

			// 局部坐标 1 对应 0.18 个图块（与着色器一致），柔光在这个范围内已淡到看不出
			addQuad(glowVertices, placement, positionX, positionY, 0.18f, 0.0f, 0.0f, 0.18f, 1.0f, 1.0f, KIND_FIREFLY, {0.0f, 0.0f, 0.0f, blink});
		}
	}
}

/// 地形编号（与 TerrainId 一致）
constexpr uint8_t terrainSnow = 3;
constexpr uint8_t terrainSwamp = 4;
constexpr uint8_t terrainLava = 7;
constexpr uint8_t terrainWater = 8;

void MapParticles::buildSnow(const Placement & placement, const Frame & frame)
{
	// 飘雪：雪地上每个图块四个槽位，各按自己的周期落一片，从 1.6~2.4 个图块高处飘下来，顺风斜着走、左右荡，
	// 落地化掉。一阵大一阵小：一层慢慢飘移的大尺度噪声决定这片地方此刻雪下得多密。
	// 雪花在空中，画在离地点上方，所以窗口下边外两三排图块的雪也要算
	const float t = frame.time;
	for(int y = 0; y < frame.slots.y + 3; ++y)
	{
		for(int x = 0; x < frame.slots.x; ++x)
		{
			const int tileX = frame.worldOrigin.x + x;
			const int tileY = frame.worldOrigin.y + y;
			if(terrainAt(tileX, tileY) != terrainSnow)
				continue;

			const float cellX = static_cast<float>(tileX);
			const float cellY = static_cast<float>(tileY);
			const float squall = 0.35f + 0.65f * smoothstep(0.3f, 0.7f, sampleNoise(cellX * 0.02f - t * 0.006f, cellY * 0.02f - t * 0.002f, 1));

			for(int slot = 0; slot < 4; ++slot)
			{
				const Random4 r = hash44(cellX, cellY, 53.0f + static_cast<float>(slot) * 7.0f, 11.0f);
				const float life = 5.0f + 2.0f * r.z;
				const float clock = t + r.x * life;
				const float cycle = std::floor(clock / life);
				const float age = clock - cycle * life;

				const Random4 q = hash44(cellX, cellY, cycle, 61.0f + static_cast<float>(slot) * 5.0f);
				if(q.w > squall)
					continue;

				const float start = 1.6f + 0.8f * q.z;
				const float height = start * (1.0f - age / life);
				const float drift = 0.22f * age;
				const float sway = 0.08f * std::sin(age * (1.4f + 0.6f * q.y) + q.z * pi2);
				const float groundX = cellX + q.x + 0.94f * drift + sway;
				const float groundY = cellY + q.y + 0.34f * drift;

				const float size = 0.03f + 0.03f * q.z; // 半径（图块）：近大远小，大片也更白。太小在白雪地上看不出在下雪
				const float fade = smoothstep(0.0f, 0.6f, age) * (1.0f - smoothstep(life - 0.5f, life, age)) * frame.weatherStrength * (0.6f + 0.4f * q.z);
				const float margin = 1.0f + 1.5f / std::max(size * placement.tilePixels, 0.5f);
				addQuad(snowVertices, placement, groundX, groundY - height, size, 0.0f, 0.0f, size, margin, margin, KIND_SNOW, {0.0f, 0.0f, 0.0f, fade});
			}
		}
	}
}

void MapParticles::buildEmbers(const Placement & placement, const Frame & frame)
{
	// 余烬：岩浆地上零星飞起、营火和火柱上一串串往上窜的火星，越飞越高、顺风飘、一闪一闪，一两秒后熄灭
	const float t = frame.time;
	auto ember = [&](float originX, float originY, float originHeight, const Random4 & r, const Random4 & q, float age, float life)
	{
		const float rise = originHeight + 0.45f * age + 0.12f * age * age;
		const float drift = 0.18f * age;
		const float wiggle = 0.05f * std::sin(age * (5.0f + 3.0f * r.y) + q.z * pi2);
		const float positionX = originX + 0.94f * drift + wiggle;
		const float positionY = originY + 0.34f * drift - rise;
		const float twinkle = 0.55f + 0.45f * std::sin(age * (11.0f + 6.0f * q.x) + r.w * 20.0f);
		const float fade = smoothstep(0.0f, 0.2f, age) * (1.0f - smoothstep(life * 0.6f, life, age)) * twinkle * frame.weatherStrength;
		addQuad(glowVertices, placement, positionX, positionY, 0.06f, 0.0f, 0.0f, 0.06f, 1.9f, 1.9f, KIND_EMBER, {0.0f, 0.0f, 0.0f, fade});
	};

	for(int y = 0; y < frame.slots.y + 2; ++y)
	{
		for(int x = 0; x < frame.slots.x; ++x)
		{
			const int tileX = frame.worldOrigin.x + x;
			const int tileY = frame.worldOrigin.y + y;
			if(terrainAt(tileX, tileY) != terrainLava)
				continue;

			const float cellX = static_cast<float>(tileX);
			const float cellY = static_cast<float>(tileY);
			for(int slot = 0; slot < 2; ++slot)
			{
				const Random4 r = hash44(cellX, cellY, 71.0f + static_cast<float>(slot) * 11.0f, 13.0f);
				const float life = 1.4f + 1.2f * r.z;
				const float period = life + 0.6f + 2.0f * r.w;
				const float clock = t + r.x * period;
				const float cycle = std::floor(clock / period);
				const float age = clock - cycle * period;
				if(age > life)
					continue;
				const Random4 q = hash44(cellX, cellY, cycle, 79.0f + static_cast<float>(slot) * 7.0f);
				if(q.w > 0.7f)
					continue;
				ember(cellX + q.x, cellY + q.y, 0.0f, r, q, age, life);
			}
		}
	}

	for(const Frame::Fire & fire : frame.fires)
	{
		for(int slot = 0; slot < 6; ++slot)
		{
			const Random4 r = hash44(fire.seed, static_cast<float>(slot), 83.0f, 17.0f);
			const float life = 1.0f + 0.9f * r.z;
			const float period = life + 0.2f + 0.8f * r.w;
			const float clock = t + r.x * period;
			const float cycle = std::floor(clock / period);
			const float age = clock - cycle * period;
			if(age > life)
				continue;
			const Random4 q = hash44(fire.seed, static_cast<float>(slot), cycle, 89.0f);
			ember(fire.x + (q.x - 0.5f) * 0.35f, fire.y + (q.y - 0.5f) * 0.2f, fire.height * 0.6f, r, q, age, life);
		}
	}
}

void MapParticles::buildMiasma(const Placement & placement, const Frame & frame)
{
	// 瘴气团：沼泽上零星冒起一团黄绿的浊气，慢慢升高、胀开、散掉，七八秒一团
	const float t = frame.time;
	for(int y = 0; y < frame.slots.y + 1; ++y)
	{
		for(int x = 0; x < frame.slots.x; ++x)
		{
			const int tileX = frame.worldOrigin.x + x;
			const int tileY = frame.worldOrigin.y + y;
			if(terrainAt(tileX, tileY) != terrainSwamp)
				continue;

			const float cellX = static_cast<float>(tileX);
			const float cellY = static_cast<float>(tileY);
			const Random4 r = hash44(cellX, cellY, 97.0f, 19.0f);
			const float life = 6.0f + 3.0f * r.z;
			const float period = life + 2.0f + 3.0f * r.w;
			const float clock = t + r.x * period;
			const float cycle = std::floor(clock / period);
			const float age = clock - cycle * period;
			if(age > life)
				continue;
			const Random4 q = hash44(cellX, cellY, cycle, 101.0f);
			if(q.w > 0.35f)
				continue;

			const float progress = age / life;
			const float radius = 0.18f + 0.3f * progress;
			const float positionX = cellX + q.x + 0.94f * 0.05f * age;
			const float positionY = cellY + q.y + 0.34f * 0.05f * age - 0.07f * age;
			const float fade = std::sin(3.1415927f * progress) * 0.16f * frame.weatherStrength;
			addQuad(miasmaVertices, placement, positionX, positionY, radius, 0.0f, 0.0f, radius * 0.7f, 1.9f, 1.9f, KIND_MIASMA, {0.0f, 0.0f, 0.0f, fade});
		}
	}
}

void MapParticles::buildRain(const Placement & placement, const Frame & frame)
{
	// 雨丝：阵雨的云下面、水面上，一道道细雨斜着落下（风从西边来，雨丝往右偏），落进水里打出涟漪（涟漪在合成着色器里画）
	const float t = frame.time;
	for(int y = 0; y < frame.slots.y + 2; ++y)
	{
		for(int x = 0; x < frame.slots.x; ++x)
		{
			const int tileX = frame.worldOrigin.x + x;
			const int tileY = frame.worldOrigin.y + y;
			if(terrainAt(tileX, tileY) != terrainWater)
				continue;

			const float cellX = static_cast<float>(tileX);
			const float cellY = static_cast<float>(tileY);
			const float shower = showerAt(cellX + 0.5f, cellY + 0.5f, t);
			if(shower < 0.02f)
				continue;

			for(int slot = 0; slot < 3; ++slot)
			{
				const Random4 r = hash44(cellX, cellY, 107.0f + static_cast<float>(slot) * 3.0f, 23.0f);
				const float life = 0.35f + 0.15f * r.z;
				const float period = life + 0.1f + 0.4f * r.w;
				const float clock = t + r.x * period;
				const float cycle = std::floor(clock / period);
				const float age = clock - cycle * period;
				if(age > life)
					continue;
				const Random4 q = hash44(cellX, cellY, cycle, 113.0f + static_cast<float>(slot));
				if(q.w > shower)
					continue;

				const float height = 1.4f * (1.0f - age / life);
				const float landX = cellX + q.x;
				const float landY = cellY + q.y;
				const float positionX = landX - 0.22f * height;
				const float positionY = landY - height;
				// 雨丝沿下落方向：屏幕上往下、略往右
				const float dirX = 0.22f / std::hypot(0.22f, 1.0f);
				const float dirY = 1.0f / std::hypot(0.22f, 1.0f);
				const float length = 0.16f;
				const float width = std::max(0.012f, 1.2f / std::max(placement.tilePixels, 1.0f)); // 至少一个多像素宽，否则细得看不见
				const float fade = 0.5f * smoothstep(0.0f, 0.08f, age) * frame.weatherStrength;
				const float marginY = 1.0f + 1.5f / std::max(width * placement.tilePixels, 0.5f);
				addQuad(rainVertices, placement, positionX, positionY, dirX * length, dirY * length, -dirY * width, dirX * width, 1.0f, std::min(marginY, maxExtent), KIND_RAIN, {0.0f, 0.0f, 0.0f, fade});
			}
		}
	}
}

namespace
{

/// 扬尘的颜色与多少：沙地扬沙最多，泥地次之，雪地扬起雪沫，岩浆地上是火山灰，草地只带起一点草屑
struct DustLook
{
	float r;
	float g;
	float b;
	float amount;
};

DustLook dustLook(MapDustSource::Kind kind)
{
	switch(kind)
	{
		// 尘土要比地面亮一些才看得出（与地面同色时扬起来像没有）
		case MapDustSource::Kind::SAND:
			return {0.93f, 0.87f, 0.72f, 1.0f};
		case MapDustSource::Kind::DIRT:
			return {0.80f, 0.72f, 0.60f, 0.8f};
		case MapDustSource::Kind::SNOW:
			return {0.95f, 0.97f, 1.0f, 0.9f};
		case MapDustSource::Kind::ASH:
			return {0.42f, 0.39f, 0.37f, 0.7f};
		case MapDustSource::Kind::GRASS:
			return {0.58f, 0.64f, 0.40f, 0.3f};
	}
	return {0.8f, 0.7f, 0.55f, 1.0f};
}

/// 每个走动的英雄每秒扬起几团尘土（乘上地面的 amount）；同时最多留多少团
constexpr float dustPerSecond = 16.0f;
constexpr size_t maxDustPuffs = 240;

}

void MapParticles::updateDust(const Frame & frame)
{
	const float now = frame.time;
	// 合成器的时钟约 2.8 小时绕回一次，或者很久没画（打开了别的窗口）：从头算
	if(dustClock < 0.0f || now < dustClock || now - dustClock > 1.0f)
	{
		dustClock = now;
		dustCarry.clear();
	}
	const float dt = std::min(now - dustClock, 0.1f);
	dustClock = now;

	std::erase_if(dustPuffs, [&](const DustPuff & puff) { return now < puff.born || now - puff.born > puff.life; });

	std::uniform_real_distribution<float> unit(0.0f, 1.0f);
	std::unordered_map<int, float> carry;
	for(const MapDustSource & source : frame.dust)
	{
		const float speed = std::hypot(source.vx, source.vy);
		if(speed < 0.3f)
			continue;
		const DustLook look = dustLook(source.kind);
		float pending = (dustCarry.count(source.id) ? dustCarry[source.id] : 0.0f) + dustPerSecond * look.amount * dt;
		const float backX = -source.vx / speed;
		const float backY = -source.vy / speed;
		while(pending >= 1.0f && dustPuffs.size() < maxDustPuffs)
		{
			pending -= 1.0f;
			// 从马蹄后面扬起：在脚下偏后一点、左右散开，往后、往上飘，慢慢停下
			const float lateral = (unit(dustRandom) - 0.5f) * 0.32f;
			DustPuff puff;
			puff.x = source.x + backX * (0.1f + 0.12f * unit(dustRandom)) - backY * lateral;
			puff.y = source.y + backY * (0.1f + 0.12f * unit(dustRandom)) + backX * lateral + 0.08f;
			const float kick = 0.25f + 0.35f * unit(dustRandom);
			puff.vx = backX * kick + (unit(dustRandom) - 0.5f) * 0.25f;
			puff.vy = backY * kick * 0.6f + (unit(dustRandom) - 0.5f) * 0.15f;
			puff.born = now - unit(dustRandom) * dt; // 一帧里扬起的几团错开一点
			puff.life = 0.7f + 0.6f * unit(dustRandom);
			puff.size = 0.06f + 0.04f * unit(dustRandom);
			puff.strength = look.amount;
			puff.kind = source.kind;
			dustPuffs.push_back(puff);
		}
		carry[source.id] = std::min(pending, 1.0f);
	}
	// 没在走的英雄不再攒
	dustCarry.swap(carry);
}

void MapParticles::buildDust(const Placement & placement, const Frame & frame)
{
	for(const DustPuff & puff : dustPuffs)
	{
		const float age = frame.time - puff.born;
		if(age < 0.0f || age > puff.life)
			continue;
		const float progress = age / puff.life;
		// 先冲出去、很快慢下来；一边往上飘、一边胀开淡掉
		const float travel = age * (1.0f - 0.5f * progress);
		const float positionX = puff.x + puff.vx * travel + 0.94f * 0.05f * age;
		const float positionY = puff.y + puff.vy * travel + 0.34f * 0.05f * age - 0.16f * age;
		const float radius = puff.size + 0.2f * progress;
		const float fade = smoothstep(0.0f, 0.08f, age) * (1.0f - progress) * (1.0f - progress) * 0.5f * puff.strength * frame.strength;
		if(fade < 0.004f)
			continue;
		const DustLook look = dustLook(puff.kind);
		addQuad(puffVertices, placement, positionX, positionY, radius, 0.0f, 0.0f, radius * 0.8f, 1.9f, 1.9f, KIND_PUFF, {look.r, look.g, look.b, fade});
	}
}

void MapParticles::buildSmoke(const Placement & placement, const Frame & frame)
{
	// 炊烟：每个屋顶十团烟轮流冒出，六七秒里升高两个多图块、顺风斜斜飘开、越来越大越淡，连成一缕。
	// 淡墨色：刚冒出来浓一些，散开后成一抹淡灰；夜里淡下去。太小太淡在缩放后的画面里看不出（2026-11-18 实机）
	const float t = frame.time;
	constexpr int puffs = 10;
	for(const MapChimney & chimney : frame.chimneys)
	{
		for(int slot = 0; slot < puffs; ++slot)
		{
			const Random4 r = hash44(chimney.seed, static_cast<float>(slot), 131.0f, 29.0f);
			const float life = 6.0f + 1.5f * r.z;
			const float clock = t + (static_cast<float>(slot) / puffs + r.x * 0.08f) * life + chimney.seed * 3.1f;
			const float cycle = std::floor(clock / life);
			const float age = clock - cycle * life;
			const Random4 q = hash44(chimney.seed, static_cast<float>(slot), cycle, 137.0f);
			const float progress = age / life;

			const float rise = 0.48f * age - 0.018f * age * age;
			const float drift = 0.04f * age + 0.02f * age * age;
			const float wobble = 0.08f * std::sin(age * (0.9f + 0.4f * q.y) + q.x * pi2) * progress;
			const float positionX = chimney.x + (q.x - 0.5f) * 0.08f + 0.94f * drift + wobble;
			const float positionY = chimney.y - chimney.height + 0.34f * drift - rise;
			const float radius = 0.13f + 0.32f * progress + 0.04f * q.z;
			const float fade = smoothstep(0.0f, 0.1f, progress) * (1.0f - smoothstep(0.3f, 1.0f, progress)) * 0.5f * (1.0f - 0.45f * frame.night) * frame.strength;
			if(fade < 0.004f)
				continue;
			const float shade = 0.80f - 0.10f * progress;
			addQuad(puffVertices, placement, positionX, positionY, radius, 0.0f, 0.0f, radius * 0.85f, 1.9f, 1.9f, KIND_PUFF,
				{shade * 1.02f, shade, shade * 0.98f, fade});
		}
	}
}

void MapParticles::buildSparkles(const Placement & placement, const Frame & frame)
{
	// 宝物闪光：每件宝物各按自己的周期（4~8 秒）闪一下，每次落在物件画面里随机的一点，0.5 秒里亮起又灭，星光略转一点
	const float t = frame.time;
	for(const MapSparkle & sparkle : frame.sparkles)
	{
		const Random4 r = hash44(sparkle.seed, 151.0f, 31.0f, 7.0f);
		const float period = 4.0f + 4.0f * r.z;
		const float clock = t + r.x * period;
		const float cycle = std::floor(clock / period);
		const float age = clock - cycle * period;
		constexpr float flash = 0.6f;
		if(age > flash)
			continue;
		const Random4 q = hash44(sparkle.seed, cycle, 157.0f, 11.0f);
		const float positionX = sparkle.x + (q.x * 2.0f - 1.0f) * sparkle.halfWidth;
		const float positionY = sparkle.y + (q.y * 2.0f - 1.0f) * sparkle.halfHeight;
		const float brightness = std::sin(3.1415927f * age / flash);
		const float glow = brightness * brightness * frame.strength;

		// 落点在缓存纹理里的坐标（缓存按图块环形存放），片元着色器据此查物件层
		const float spotU = fract((positionX - static_cast<float>(frame.worldOrigin.x) + static_cast<float>(frame.slotOrigin.x)) / static_cast<float>(frame.slots.x));
		const float spotV = fract((positionY - static_cast<float>(frame.worldOrigin.y) + static_cast<float>(frame.slotOrigin.y)) / static_cast<float>(frame.slots.y));

		const float angle = 0.25f * (q.z - 0.5f) + 0.5f * age;
		const float size = (0.24f + 0.10f * q.w) * (0.7f + 0.3f * brightness);
		addQuad(glowVertices, placement, positionX, positionY, std::cos(angle) * size, std::sin(angle) * size, -std::sin(angle) * size, std::cos(angle) * size,
			1.0f, 1.0f, KIND_SPARKLE, {spotU, spotV, 0.0f, glow});
	}
}

void MapParticles::draw(SDL_Renderer * renderer, const Frame & frame, const std::vector<PresentedRegion> & regions)
{
	if(frame.strength <= 0.001f || regions.empty() || frame.slots.x <= 0 || frame.slots.y <= 0 || frame.cacheWidth <= 0.0f)
		return;

	const bool hasVegetation = vegetation.size() == static_cast<size_t>(vegetationSize.x * vegetationSize.y) * 4 && !vegetation.empty();

	if(!ensureResources(renderer, frame))
		return;

	// 地图坐标到屏幕：第一块区域就是可见窗口左上角图块起的那一片（见 MapViewCache::forEachCachedBand），
	// 各区域在屏幕上连成一整块，所以一个平移加缩放就够了
	const PresentedRegion & first = regions.front();
	if(first.source.w <= 0)
		return;

	Placement placement;
	placement.originX = static_cast<float>(first.target.x);
	placement.originY = static_cast<float>(first.target.y);
	placement.worldX = static_cast<float>(frame.worldOrigin.x);
	placement.worldY = static_cast<float>(frame.worldOrigin.y);
	placement.tilePixels = static_cast<float>(first.target.w) / static_cast<float>(first.source.w) * frame.cacheWidth / static_cast<float>(frame.slots.x);

	updateDust(frame);

	shadowVertices.clear();
	miasmaVertices.clear();
	puffVertices.clear();
	bitVertices.clear();
	glowVertices.clear();
	birdVertices.clear();
	rainVertices.clear();
	snowVertices.clear();

	buildBirds(placement, frame);
	if(hasVegetation)
	{
		buildDriftingBits(placement, frame);
		buildFireflies(placement, frame);
	}
	// 天气粒子按图块数逐个算，缩到全图时几千个图块、每片雪不到一个像素，不算
	if(frame.weatherStrength > 0.01f)
	{
		if(frame.sky)
			buildSnow(placement, frame);
		buildEmbers(placement, frame);
		buildMiasma(placement, frame);
		if(frame.sky)
			buildRain(placement, frame);
	}
	// 尘土、炊烟、宝物闪光与物件个数有关（不按图块逐个算），缩小时照画，只随粒子强度淡出
	buildDust(placement, frame);
	buildSmoke(placement, frame);
	buildSparkles(placement, frame);

	// 先画的在下面：鸟影、瘴气、尘土与炊烟、落叶飞花、萤火虫与余烬与宝物闪光、飞鸟、雨丝、飘雪
	vertices.clear();
	for(const auto * part : {&shadowVertices, &miasmaVertices, &puffVertices, &bitVertices, &glowVertices, &birdVertices, &rainVertices, &snowVertices})
		vertices.insert(vertices.end(), part->begin(), part->end());
	if(vertices.empty())
		return;

	indices.clear();
	for(int base = 0; base + 3 < static_cast<int>(vertices.size()); base += 4)
		for(int offset : {0, 1, 2, 0, 2, 3})
			indices.push_back(base + offset);

	// 只画在地图区域里：各区域外接矩形之外是界面。可见窗口最左、最上一格常有一部分在屏幕外，外接矩形的左上角是负的；
	// gpu 渲染器遇到伸出视口的裁剪矩形会把整批都裁掉（2026-11-18 查出：落叶、飞花、萤火虫因此一直没画出来），所以先与视口求交
	Rect bounds = first.target;
	for(const PresentedRegion & region : regions)
		bounds = bounds.include(region.target);
	SDL_Rect viewport;
	SDL_GetRenderViewport(renderer, &viewport);
	bounds = bounds.intersect(Rect(0, 0, viewport.w, viewport.h));
	if(bounds.w <= 0 || bounds.h <= 0)
		return;

	SDL_Rect previousClip;
	const bool hadClip = SDL_RenderClipEnabled(renderer);
	if(hadClip)
		SDL_GetRenderClipRect(renderer, &previousClip);

	const SDL_Rect clip = {bounds.x, bounds.y, bounds.w, bounds.h};
	SDL_SetRenderClipRect(renderer, &clip);
	ParticleUniforms uniforms = {{frame.grade[0], frame.grade[1], frame.grade[2], frame.night}};
	SDL_SetGPURenderStateFragmentUniforms(state, 0, &uniforms, sizeof(uniforms));
	SDL_SetGPURenderState(renderer, state);
	SDL_SetTextureBlendMode(white, frame.blendMode);

	if(!SDL_RenderGeometry(renderer, white, vertices.data(), static_cast<int>(vertices.size()), indices.data(), static_cast<int>(indices.size())))
		logGlobal->error("Failed to draw map particles: %s", SDL_GetError());

	SDL_SetGPURenderState(renderer, nullptr);
	SDL_SetRenderClipRect(renderer, hadClip ? &previousClip : nullptr);
}

void MapParticles::release()
{
	if(state)
		SDL_DestroyGPURenderState(state);

	if(owner && (shader || sampler))
	{
		SDL_GPUDevice * device = static_cast<SDL_GPUDevice *>(SDL_GetPointerProperty(SDL_GetRendererProperties(owner), SDL_PROP_RENDERER_GPU_DEVICE_POINTER, nullptr));
		if(device && shader)
			SDL_ReleaseGPUShader(device, shader);
		if(device && sampler)
			SDL_ReleaseGPUSampler(device, sampler);
	}

	if(white)
		SDL_DestroyTexture(white);

	dustPuffs.clear();
	dustCarry.clear();
	dustClock = -1.0f;

	state = nullptr;
	shader = nullptr;
	sampler = nullptr;
	white = nullptr;
	boundTextures = {};
	owner = nullptr;
	unavailable = false;
}
