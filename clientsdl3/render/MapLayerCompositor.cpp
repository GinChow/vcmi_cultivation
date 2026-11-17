/*
 * MapLayerCompositor.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "MapLayerCompositor.h"

#include "SDL_Extensions.h"

#include "lib/CConfigHandler.h"

#include <SDL3/SDL.h>

namespace
{

/// 片元着色器（MSL）。输入结构必须与 SDL gpu 渲染器内置的贴图顶点着色器对齐：
/// 颜色在 [[user(locn0)]]、纹理坐标在 [[user(locn1)]]。texture(0) 是 SDL 自己绑定的主纹理
/// （即地面层，这里不用它，因为它的采样器不能环绕取样），本类额外绑定的纹理从 texture(1) 起。
///
/// 纹理坐标 uv 指向地图缓存。缓存按图块环形存放，所以任何偏移后的坐标都先取模（wrapUV）再取样，
/// 就能越过环形接缝取到屏幕上相邻的图块。
///
/// 取样预算（每个水面像素）：全分辨率纹理约 15 次，其余在 1/8、1/16 遮罩小图、离岸距离图和 256×256 噪声图上，
/// 这些小纹理几乎都落在缓存里。不在水面、附近也没有摇摆物件的像素只取 6 次（地面、水面遮罩、物件、覆盖、风场遮罩两次）；
/// 树冠附近的像素为倒推摇摆来源再多取约 6 次风场遮罩。
/// 光影（2026-11-17）每个像素再取 3 次：起伏层、1/4 光照图（长投影由预处理着色器算好）、雾云场；
/// 有雾或云的地方另取噪声 2~8 次。
const char * const waterShaderSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct Params
{
	float4 cache;     // xy：缓存纹理像素尺寸；zw：槽位数（图块）
	float4 world;     // xy：可见窗口左上角图块的地图坐标；zw：该图块所在槽位
	float4 misc;      // x：时间（秒）；y：特效强度，0 为关闭；zw：离岸距离图尺寸（图块）
	float4 sun;       // xyz：指向太阳的方向（屏幕坐标：x 右、y 上、z 朝镜头）；w：按法线重新打光的强度
	float4 sunColor;  // rgb：阳光颜色；w：边缘光强度
	float4 shade;     // rgb：背阴处的色调；w：长投影的浓度
	float4 camera;    // xy：屏幕中心的地图坐标；z：云层的视差系数；w：云层的高度（图块）
	float4 atmos;     // x：雾的浓度；y：云海的浓度
	float4 shadowDir; // xy：离地 1 个图块的点，影子在屏幕上落出多远（图块）
	float4 grade;     // rgb：整幅画面的时段色调
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

// 缓存按图块环形存放，只装着可见窗口那一片：窗口最上一行的上方紧挨着的是窗口最下一行，不是地图上真正的上一行。
// 所以往旁边取样不能简单地环绕（那样屏幕顶部的倒影会取到屏幕底部的物件，2026-11-17 用户发现），
// 而要先换算成“窗口内的位置”（以图块为单位，0 为窗口左上角），越界的夹在窗口边上，再换算回缓存坐标
static inline float2 windowPosition(float2 uv, float2 slots, float2 slotOrigin)
{
	return fmod(uv * slots - slotOrigin + slots, slots);
}

static inline float2 windowToUV(float2 position, float2 slots, float2 slotOrigin)
{
	float2 clamped = clamp(position, float2(0.001), slots - 0.001);
	float2 uv = (clamped + slotOrigin) / slots;
	return uv - floor(uv);
}

// ---- 风场摇摆（2026-11-17） ----
// 树、灌木、花草画在单独的摇摆层，定格在第 0 帧；风场遮罩与它逐像素对齐：
//   R：弯曲权重（树根 0、树梢最大），G：叶片抖动，B：摇摆物件的覆盖度，三者都已按覆盖度预乘；
//   A：这一点最上面盖着不摇摆物件（英雄、山、建筑）的程度，用来在合成时排前后。
// 每个摇摆像素被吹离原位 D × 权重，D 是这一点的风（权重为 1 处的位移）。合成时倒推“哪个像素被吹到了这里”。

// 树梢在满权重、阵风最强时被吹开的距离（图块）：约 4 个原版像素。来回摆的幅度约为它的一半，
// 原版逐帧晃动只有 1 个像素，看上去生硬（2026-11-17 用户反馈）
constant float windAmplitude = 0.13;

struct WindSample
{
	float2 bend;    // 权重为 1 处的位移（图块，窗口坐标方向）
	float strength; // 这一点的风力 0~1
};

// 风从西偏北吹来（屏幕上往右、略往下）。三层叠加：
//   1. 阵风：一张低频噪声沿风向推进，阵风扫过时风力大，林梢一片接一片地被压弯；
//   2. 摆动：每棵树按固有频率来回摆，相位随地图位置缓慢变化，相邻的树大致同步又不完全齐整；
//   3. 叶片抖动：在调用处按遮罩的 G 加上，只抖树叶不抖树干。
// 噪声都按地图坐标取样，滚动画面时风不会跟着屏幕走
static inline WindSample windAt(float2 world, float t, texture2d<float> noiseTex, sampler noiseSmp)
{
	const float2 windDir = float2(0.94, 0.34);
	float gust = noiseTex.sample(noiseSmp, (world - windDir * (t * 1.3)) * 0.045).r;
	gust = smoothstep(0.25, 0.85, gust);
	float strength = 0.3 + 0.7 * gust;

	// 相位约每两三个图块转一圈：同一树冠里几乎同相（否则树冠像果冻一样扭），隔几棵树就明显错开
	float phase = noiseTex.sample(noiseSmp, world * 0.05 + float2(0.37, 0.61)).g * 6.283;
	float swing = sin(t * 2.5 + phase) * 0.7 + sin(t * 4.1 + phase * 1.7) * 0.3;

	// 平均偏向下风、再来回摆（略过竖直位置）：风越大压得越弯、摆得也越开。
	// 俯视斜角下前后方向的摆动在屏幕上压扁成一小段竖直位移
	float bend = strength * (0.45 + 0.55 * swing);
	WindSample result;
	result.bend = float2(windDir.x, windDir.y * 0.4) * bend * windAmplitude;
	result.strength = strength;
	return result;
}

// 风场遮罩一点的弯曲权重（还原预乘）
static inline float windWeight(float4 m)
{
	return m.r / max(m.b, 0.02);
}

// ---- 光影（2026-11-17）：长投影、2D 打光、流动的雾、山腰云海 ----
// 起伏层与物件层逐像素对齐，记着最上面那件物件表面的法线（RG）和离地高度（B，预乘 alpha），
// 由地形模组 tools/relief.py 从立绘推出（法线来自单目法线估计模型，高度来自“地面线”）。
// 没有起伏图的物件（原版的城镇、英雄……）在起伏层上 RGB 为 0：当成平地，不重新打光、不投长影。

constant float reliefHeightRange = 4.0;                    // 起伏层 B = 1 对应离地 4 个图块，与 tools/relief.py 一致
constant float3 groundNormal = float3(0.0, 0.399, 0.917);   // 平地的法线：大地图约是 66° 俯视，平地朝上偏向屏幕上方（与法线模型在山脚平地处的估计一致）

struct Relief
{
	float3 normal; // 屏幕坐标：x 右、y 上、z 朝镜头
	float height;  // 离地高度（图块），已按覆盖度淡出
	float known;   // 1 为有起伏图的物件，0 为平地或没有起伏图的物件
};

static inline Relief decodeRelief(float4 r)
{
	Relief result;
	float3 enc = r.rgb / max(r.a, 0.001);
	result.known = r.a > 0.02 ? smoothstep(0.08, 0.25, enc.r + enc.g) : 0.0;
	float2 nxy = enc.rg * 2.0 - 1.0;
	float3 n = float3(nxy, sqrt(saturate(1.0 - dot(nxy, nxy))));
	result.normal = normalize(mix(groundNormal, n, result.known));
	result.height = enc.b * reliefHeightRange * result.known * saturate(r.a * 1.5);
	return result;
}

// 物件按法线重新打光（颜色为预乘 alpha）。原画本身就画了明暗，所以按“相对平地”的比例调：
// 朝太阳的一面提亮、偏暖，背阴的一面压暗、偏冷，平地和没有起伏图的物件不变；
// 轮廓上朝着太阳的一圈再加一道边缘光（低太阳时更明显，像《八方旅人》的逆光描边）
static inline float3 relight(float3 rgb, float alpha, Relief r, constant Params & P)
{
	float3 L = P.sun.xyz;
	float ndl = dot(r.normal, L);
	float gdl = dot(groundNormal, L);
	const float ambient = 0.5;
	float k = (ambient + (1.0 - ambient) * saturate(ndl)) / max(ambient + (1.0 - ambient) * saturate(gdl), 0.05);
	k = clamp(k, 0.6, 1.4);
	float3 litTint = mix(P.shade.rgb, P.sunColor.rgb, saturate(ndl * 1.6 + 0.25));
	float3 refTint = mix(P.shade.rgb, P.sunColor.rgb, saturate(gdl * 1.6 + 0.25));
	// 色调只按比例轻推：清晨、傍晚阳光很暖，背阴处直接除下来会整片发蓝发紫（2026-11-17 离线预览）
	float3 tint = clamp(mix(float3(1.0), litTint / max(refTint, float3(0.05)), 0.5), float3(0.85), float3(1.15));
	float strength = P.sun.w * r.known;
	rgb *= mix(float3(1.0), k * tint, strength);

	float2 facing = r.normal.xy / max(length(r.normal.xy), 1e-4);
	float2 toward = L.xy / max(length(L.xy), 1e-4);
	float rim = pow(saturate(1.0 - r.normal.z), 2.0) * saturate(dot(facing, toward));
	return rgb + P.sunColor.rgb * (rim * P.sunColor.w * strength * alpha);
}

// 流动的雾：雾云场的 G 说这里生雾多少（山脚、林地、水边、沼泽浓），两层顺风飘移的低频噪声把它撕成一缕一缕；
// 只贴着地面：可见表面离地越高雾越淡，树根、山脚埋在雾里，树冠、山峰露出来
static inline float fogAt(float2 world, float t, float mist, float height, texture2d<float> noiseTex, sampler noiseSmp)
{
	const float2 windDir = float2(0.94, 0.34);
	float f1 = noiseTex.sample(noiseSmp, (world - windDir * (t * 0.10)) * 0.085).r;
	float f2 = noiseTex.sample(noiseSmp, (world - windDir * (t * 0.17)) * 0.21 + float2(0.31, 0.67)).g;
	float wisps = smoothstep(0.3, 0.85, f1 * 0.62 + f2 * 0.38);
	return mist * (0.12 + 0.88 * wisps) * exp(-height / 0.4);
}

// 云的形状：三层噪声，大团的云块带着碎边
static inline float cloudShape(float2 p, texture2d<float> noiseTex, sampler noiseSmp)
{
	float a = noiseTex.sample(noiseSmp, p * 0.055).g;
	float b = noiseTex.sample(noiseSmp, p * 0.13 + float2(0.41, 0.17)).r;
	float c = noiseTex.sample(noiseSmp, p * 0.37 + float2(0.73, 0.29)).a;
	return a * 0.55 + b * 0.3 + c * 0.15;
}

// 只取最粗的一层：云影、迎光面这类只要大致形状的地方用，一次取样
static inline float cloudShapeCoarse(float2 p, texture2d<float> noiseTex, sampler noiseSmp)
{
	return noiseTex.sample(noiseSmp, p * 0.055).g * 0.55 + 0.225;
}

// 云量 cover 越大，云越厚、越连成片。天上处处是 0.35（稀疏的几朵），山周围到 1。
// 只在噪声高处成团、团与团之间留出大片空隙（2026-11-17 用户先嫌云太密挡东西，调稀后又说天上没有云了）
static inline float cloudDensity(float shape, float cover)
{
	return smoothstep(0.66 - 0.16 * cover, 0.84 - 0.1 * cover, shape) * smoothstep(0.0, 0.2, cover);
}

// 静止物件 s 与摇摆物件 tree（都是预乘 alpha）按前后关系叠起来：front 是静止物件盖在上面的程度
static inline float4 stackObjects(float4 s, float4 tree, float front)
{
	float4 treeOver = tree + s * (1.0 - tree.a);
	float4 staticOver = s + tree * (1.0 - s.a);
	return mix(treeOver, staticOver, front);
}

fragment main0_out main0(main0_in in [[stage_in]], constant Params & P [[buffer(0)]],
	texture2d<float> mainTex [[texture(0)]], sampler mainSmp [[sampler(0)]],
	texture2d<float> groundTex [[texture(1)]], sampler groundSmp [[sampler(1)]],
	texture2d<float> objectTex [[texture(2)]], sampler objectSmp [[sampler(2)]],
	texture2d<float> maskTex [[texture(3)]], sampler maskSmp [[sampler(3)]],
	texture2d<float> noiseTex [[texture(4)]], sampler noiseSmp [[sampler(4)]],
	texture2d<float> maskMid [[texture(5)]], sampler maskMidSmp [[sampler(5)]],
	texture2d<float> maskLow [[texture(6)]], sampler maskLowSmp [[sampler(6)]],
	texture2d<float> depthTex [[texture(7)]], sampler depthSmp [[sampler(7)]],
	texture2d<float> swayTex [[texture(8)]], sampler swaySmp [[sampler(8)]],
	texture2d<float> windTex [[texture(9)]], sampler windSmp [[sampler(9)]],
	texture2d<float> reliefTex [[texture(10)]], sampler reliefSmp [[sampler(10)]],
	texture2d<float> lightTex [[texture(11)]], sampler lightSmp [[sampler(11)]])
{
	float2 uv = in.uv;
	float3 color = groundTex.sample(groundSmp, uv).rgb;

	float2 slots = P.cache.zw;
	float t = P.misc.x;

	// 窗口内的位置与地图坐标（都以图块为单位）：噪声跟着地图走，滚动画面时水纹、风都不会滑动
	float2 slotOrigin = P.world.zw;
	float2 rel = windowPosition(uv, slots, slotOrigin);
	float2 world = P.world.xy + rel;

	// 这一点的风；关闭特效时不动
	WindSample wind = windAt(world, t, noiseTex, noiseSmp);
	float2 windShift = wind.bend * P.misc.y;

	float pixel = slots.x / max(P.cache.x, 1.0); // 一个缓存像素是多少图块

	// 水面遮罩用颜色区分水的种类：海面遮罩灰白（红绿蓝相等），湖黄色（红绿），河流和河口绿色。
	// 于是 蓝 = 海面，红 − 蓝 = 湖面，绿 − 红 = 河面，绿 = 任何水面。
	// 河很窄，只做扰动、波光和倒影；湖面平静，没有涌浪浪花、白边很淡、倒影更清楚；海面全套
	float3 maskRGB = maskTex.sample(maskSmp, uv).rgb;
	float water = maskRGB.r * P.misc.y;             // 海面和湖面
	float wet = maskRGB.g * P.misc.y;               // 任何水面
	float river = saturate(wet - water);            // 河面
	float riverShare = river / max(wet, 0.001);     // 河面在这一点水面里的比重
	float lakeShare = saturate(maskRGB.r - maskRGB.b) / max(maskRGB.g, 0.001); // 湖面的比重

	if(wet > 0.003)
	{
		// 开阔度：1/16 遮罩上取中心和半个图块外的四点。小图每个像素已是 16×16 的平均，
		// 所以这五次取样就相当于一个约一个图块宽的模糊。岸边约 0.5，离岸一个图块以上接近 1
		float open = maskLow.sample(maskLowSmp, uv).r * 0.4;
		open += maskLow.sample(maskLowSmp, windowToUV(rel + float2(0.55, 0.0), slots, slotOrigin)).r * 0.15;
		open += maskLow.sample(maskLowSmp, windowToUV(rel - float2(0.55, 0.0), slots, slotOrigin)).r * 0.15;
		open += maskLow.sample(maskLowSmp, windowToUV(rel + float2(0.0, 0.55), slots, slotOrigin)).r * 0.15;
		open += maskLow.sample(maskLowSmp, windowToUV(rel - float2(0.0, 0.55), slots, slotOrigin)).r * 0.15;
		float deep = smoothstep(0.62, 0.98, open);

		// 噪声：两层朝不同方向流动的低频噪声做涟漪，再加一层较细的做波光。
		// 流速约 3 秒挪过一个波纹：再慢的话开阔海面看上去是静止的（2026-11-17 用户反馈）
		float4 nA = noiseTex.sample(noiseSmp, world * 0.13 + float2(t * 0.040, t * 0.026));
		float4 nB = noiseTex.sample(noiseSmp, world * 0.31 + float2(-t * 0.034, t * 0.047));
		float4 nC = noiseTex.sample(noiseSmp, world * 0.83 + float2(t * 0.060, -t * 0.041));
		float2 ripple = (nA.rg - 0.5) * 0.6 + (nB.rg - 0.5) * 0.4;

		// 1. 折射扰动：只从水里取样，免得把岸上的沙石拖进水里
		float3 fx = color;
		//    河面窄、流得急，扰动略强一些
		float2 duv = windowToUV(rel + ripple * (0.035 + 0.035 * deep + 0.02 * riverShare), slots, slotOrigin);
		if(maskTex.sample(maskSmp, duv).g > 0.6)
			fx = groundTex.sample(groundSmp, duv).rgb;
		float texLum = dot(fx, float3(0.3, 0.59, 0.11)); // 水面贴图本身的明暗：笔触与白浪

		// 用两种尺度估计离岸远近：near 是 1/8 遮罩（岸线外约 0.15 个图块内从 0.5 升到 1），
		// far 由开阔度换算（约一个图块内从 0 升到 1）
		float near = maskMid.sample(maskMidSmp, uv).r;
		float far = saturate((open - 0.5) * 2.0);

		// 2. 水色。湖、河保留原画色调，只做原来的深浅（近岸略提亮，深处压暗偏青）；
		//    海面按离岸远近重新上色（2026-11-17 用户要求：更湛蓝、更通透，近海与深海分明）
		float seaShare = saturate(maskRGB.b) / max(maskRGB.g, 0.001);
		float3 calm = float3(0.34, 0.43, 0.50); // 倒影处压平白浪时用的平静水色
		float3 tinted = fx * mix(float3(1.05, 1.05, 1.02), float3(0.78, 0.86, 0.92), deep);
		if(seaShare > 0.001)
		{
			// 离岸深度：查离岸距离图（每个图块离最近陆地几格，按地图坐标取样，线性插值成平滑的渐变），
			// 离岸约一格是浅滩，四五格以外是深海。不看缓存，所以屏幕边缘、缩放都不影响
			float tileDistance = depthTex.sample(depthSmp, world / max(P.misc.zw, float2(1.0))).r * 8.0;
			// 用低频噪声把深浅分界扭一扭：海底有起伏，浅滩边缘不会顺着岸线排成一条整齐的带子
			tileDistance += (nA.g - 0.5) * 1.2;
			float depth = smoothstep(0.8, 4.8, tileDistance);

			float3 shallowColor = float3(0.42, 0.70, 0.68); // 浅滩：透出沙底的青绿
			float3 nearColor = float3(0.17, 0.53, 0.77);    // 近海：湛蓝
			float3 deepColor = float3(0.08, 0.30, 0.56);    // 深海
			float3 base = mix(shallowColor, nearColor, smoothstep(0.0, 0.45, depth));
			base = mix(base, deepColor, smoothstep(0.4, 1.0, depth));
			// 深海大片明暗缓慢起伏，不是一整块平涂的蓝
			base *= 1.0 + (nB.r - 0.5) * 0.14 * depth;
			// 浅滩隐约透出沙底；紧贴岸线的一窄条沙色更明显
			base = mix(base, float3(0.70, 0.68, 0.55), 0.15 * (1.0 - smoothstep(0.0, 0.2, depth)));
			base = mix(base, float3(0.74, 0.72, 0.56), 0.3 * (1.0 - smoothstep(0.55, 0.85, near)));

			// 原画的笔触用亮度起伏保留下来：浅水清澈，起伏收一些；白浪仍是白的，但比原画淡，水色更纯
			float3 sea = base * (1.0 + (texLum - 0.49) * mix(0.9, 1.2, depth));
			float whitecap = smoothstep(0.62, 0.84, texLum);
			sea = mix(sea, float3(0.92, 0.96, 0.97), whitecap * 0.4);

			// 浅水焦散：两层流向不同的低频噪声取值相近处连成一张流动的亮纹网，像阳光透过水面照在水底。
			// 网眼约半个图块；用细噪声会碎成颗粒
			float4 nP = noiseTex.sample(noiseSmp, world * 0.33 + float2(t * 0.045, -t * 0.03));
			float4 nQ = noiseTex.sample(noiseSmp, world * 0.41 + float2(-t * 0.035, t * 0.05));
			float caustic = 1.0 - smoothstep(0.012, 0.06, abs(nP.r - nQ.g));
			sea += caustic * (1.0 - smoothstep(0.1, 0.6, depth)) * 0.14 * float3(0.85, 1.0, 0.95);

			tinted = mix(tinted, sea, seaShare);
			calm = mix(calm, base * 0.97, seaShare);
		}
		fx = tinted;

		// 3. 倒影：在 1/8 遮罩上向上找岸线（任何水面的边，河岸也算），再在全分辨率遮罩上二分细化；
		//    以岸线所在图块的上沿（即岸上物件的底边）为镜面，取镜像位置的物件。
		//    只在可见窗口里找：碰到窗口上边就停，找不到岸就不画倒影（窗口外的东西缓存里没有）
		float marchStep = 1.0 / 6.0; // 图块
		int hit = 0;
		for(int i = 1; i <= 14; ++i)
		{
			float2 probe = rel - float2(0.0, marchStep * float(i));
			if(probe.y < 0.0)
				break;
			if(maskMid.sample(maskMidSmp, windowToUV(probe, slots, slotOrigin)).g < 0.5)
			{
				hit = i;
				break;
			}
		}
		if(hit > 0)
		{
			float lo = float(hit - 1);
			float hi = float(hit);
			for(int k = 0; k < 4; ++k)
			{
				float mid = 0.5 * (lo + hi);
				if(maskTex.sample(maskSmp, windowToUV(rel - float2(0.0, marchStep * mid), slots, slotOrigin)).g < 0.5)
					hi = mid;
				else
					lo = mid;
			}
			// 以下位置、距离都以图块为单位、在窗口坐标里
			float shore = rel.y - marchStep * hi;
			float axis = floor(shore + 0.02);
			float dist = rel.y - axis; // 离镜面几个图块

			// 岸影：北岸下方一窄条水面压暗，像岸的影子，把岸和水分开。河面窄，整条都在岸影里，所以减半
			float bank = 1.0 - smoothstep(0.0, 0.45, rel.y - shore);
			fx *= 1.0 - mix(0.16, 0.08, riverShare) * bank;

			// 离镜面越远，倒影被水波晃得越厉害：大波纹横向错开，加一道细密的横纹抖动（相位按地图坐标，滚动时不跳）
			//    湖面平静，晃得轻一半
			float mirrorY = axis - dist;
			float sway = (ripple.x * 0.09 + sin((P.world.y + mirrorY) * 15.0 + t * 2.3) * 0.012) * (0.35 + dist) * mix(1.0, 0.5, lakeShare);
			float2 q = float2(rel.x + sway, mirrorY + ripple.y * 0.03);
			// 竖向取两点平均，倒影略软，不像贴上去的硬图。树木的倒影也随风摆（只按镜像点自己的权重挪一步，够用）
			float4 r = float4(0.0);
			for(int k = 0; k < 2; ++k)
			{
				float2 qk = q - float2(0.0, 0.04 * float(k));
				float2 quv = windowToUV(qk, slots, slotOrigin);
				float4 qm = windTex.sample(windSmp, quv);
				float4 qTree = qm.b > 0.002 ? swayTex.sample(swaySmp, windowToUV(qk - windShift * windWeight(qm), slots, slotOrigin)) : float4(0.0);
				r += 0.5 * stackObjects(objectTex.sample(objectSmp, quv), qTree, qm.a);
			}
			float body = smoothstep(0.45, 0.9, r.a); // 半透明的阴影不倒映，只要物件本体
			float fade = pow(1.0 - smoothstep(0.0, 2.6, dist), 1.3);
			// 镜像位置落到窗口上边之外时，缓存里没有那里的物件：在边上渐隐、出界不画
			fade *= smoothstep(0.0, 0.5, q.y);
			float strength = body * fade;
			// 倒影所在处先把水面的白浪压平一些，否则倒影淹没在碎白浪里看不出来
			fx = mix(fx, calm, strength * 0.35);
			float3 reflected = r.rgb / max(r.a, 0.001);
			// 倒影带一点水色、略暗；近岸最清楚
			reflected = mix(reflected * float3(0.82, 0.86, 0.90), fx, 0.15);
			fx = mix(fx, reflected, strength * mix(0.75, 0.85, lakeShare));
		}

		// 4. 浪花（near、far 见第 2 步）
		float3 foamColor = float3(0.94, 0.95, 0.93);

		// 4a. 拍岸白边：贴着岸线一道白边，宽度随浪一涨一退，沿岸各处错开。
		//     阈值用噪声扭一扭、再用粗噪声断开，免得顺着直的岸线画出一条规整的直线
		float lap = 0.68 + 0.12 * sin(t * 1.3 + world.x * 1.7 - world.y * 0.9) + (nB.r - 0.5) * 0.22;
		float edge = (1.0 - smoothstep(lap - 0.12, lap + 0.05, near)) * smoothstep(0.3, 0.55, water);
		edge *= smoothstep(0.3, 0.62, nA.b) * mix(1.0, 0.4, lakeShare);
		fx = mix(fx, foamColor, edge * 0.55);

		// 4b. 涌浪：一阵阵往岸上推的涌浪经过时，把水面贴图里原有的白浪提亮成浪花，
		//     浪花的形状就是画上去的笔触，不是程序画的线。离岸距离也用噪声扭过，涌浪前沿是弯的。
		//     湖没有涌浪；湖水颜色浅，若按亮度提亮会把整片湖刷白
		float surgeDistance = far + (nA.g - 0.5) * 0.45;
		float surge = smoothstep(0.15, 0.95, sin(surgeDistance * 9.0 + t * 2.0));
		float envelope = (1.0 - smoothstep(0.6, 1.0, far)) * smoothstep(0.03, 0.2, far);
		float caps = smoothstep(0.42, 0.65, texLum); // 按贴图本身的白浪判断，不受海面重新上色影响
		fx = mix(fx, foamColor, caps * surge * envelope * 0.8 * (1.0 - lakeShare));

		// 5. 波光：两层流向不同的噪声叠加后只取高值，稀疏地闪动；海、湖只在开阔处，河面整条都有
		float g = nC.b * 0.6 + nB.a * 0.4;
		float glint = smoothstep(0.66, 0.82, g) * max(deep, riverShare * 0.8);
		fx += glint * 0.38 * float3(1.0, 0.98, 0.92);

		color = mix(color, fx, saturate(wet));
	}

	// 物件层、摇摆层、覆盖层都是画在透明底上的，颜色已是预乘 alpha
	float4 objects = objectTex.sample(objectSmp, uv);
	// 起伏层：最上面那件物件表面的法线和高度。树被吹离原位时改取它原位处的值
	float4 relief = reliefTex.sample(reliefSmp, uv);

	// 风场摇摆。附近（本点，或上风方向一个满摆幅处）有摇摆物件才算
	float4 m0 = windTex.sample(windSmp, uv);
	float4 lead = windTex.sample(windSmp, windowToUV(rel - windShift, slots, slotOrigin));
	if(m0.b + lead.b > 0.002)
	{
		// 倒推这一点显示的是哪个摇摆像素：解 s + windShift × 权重(s) = rel。权重在物件内平滑变化，迭代两步就收敛。
		// 从两处出发：甲从本点出发，管树冠内部和背风一侧；乙从上风满摆幅处出发，管树冠伸进原本空着的地方。
		// 乙解出来的是树（覆盖度够）就用乙，否则用甲；两处都解不出来（树冠背风边缘让出的空处，或权重陡变处），
		// 退而用甲的第一步——那里落在树外就是透明，露出后面的东西，落在树内就近似拉伸一下，不会开洞
		float tolerance = 0.75 * pixel;

		float2 firstA = rel - windShift * windWeight(m0);
		float4 mA = windTex.sample(windSmp, windowToUV(firstA, slots, slotOrigin));
		float2 sA = rel - windShift * windWeight(mA);
		mA = windTex.sample(windSmp, windowToUV(sA, slots, slotOrigin));
		bool solvedA = length(sA + windShift * windWeight(mA) - rel) < tolerance;

		float2 sB = rel - windShift * windWeight(lead);
		float4 mB = windTex.sample(windSmp, windowToUV(sB, slots, slotOrigin));
		sB = rel - windShift * windWeight(mB);
		mB = windTex.sample(windSmp, windowToUV(sB, slots, slotOrigin));
		bool solvedB = mB.b > 0.02 && length(sB + windShift * windWeight(mB) - rel) < tolerance;

		float2 source = solvedB ? sB : (solvedA ? sA : firstA);
		float4 sourceMask = solvedB ? mB : mA;

		// 叶片抖动：树叶一簇一簇各自以不同相位细颤（约半个原版像素），阵风里抖得更厉害；
		// 同时随颤动微微闪亮，像叶子翻过来露出浅色的叶背
		float leaf = sourceMask.g / max(sourceMask.b, 0.02);
		float4 nLeaf = noiseTex.sample(noiseSmp, world * 0.3);
		float flick = t * 12.0 + nLeaf.b * 18.0;
		float2 flutter = float2(sin(flick), cos(t * 10.0 + nLeaf.a * 18.0)) * (0.014 * leaf * (0.35 + 0.65 * wind.strength) * P.misc.y);
		float4 tree = swayTex.sample(swaySmp, windowToUV(source + flutter, slots, slotOrigin));
		tree.rgb *= 1.0 + leaf * wind.strength * 0.07 * (0.5 + 0.5 * sin(flick)) * P.misc.y;

		objects = stackObjects(objects, tree, m0.a);

		// 看得见的树像素来自 source，它的法线、高度也取那里（静止物件盖在上面的地方仍取本点）
		float treeShare = tree.a * (1.0 - m0.a);
		if(treeShare > 0.002)
			relief = mix(relief, reliefTex.sample(reliefSmp, windowToUV(source, slots, slotOrigin)), treeShare);
	}

	// 2D 打光：物件按法线重新打光、加边缘光（平地不变）
	Relief surface = decodeRelief(relief);
	if(P.sun.w * P.misc.y > 0.001 && surface.known > 0.001)
		objects.rgb = relight(objects.rgb, objects.a, surface, P);

	color = objects.rgb + color * (1.0 - objects.a);

	// 长投影：预处理着色器按起伏层逐点朝太阳方向找遮挡，算在 1/4 大小的光照图上（线性取样，影子边缘自然柔和）。
	// 落在地面、湖面和矮处的物件上，偏冷色。取样位置按地图坐标上的噪声错开约 0.15 个图块，影子边缘随地面起伏弯弯曲曲；
	// 再用一层细噪声把影子打散成深浅不一的斑驳（树冠透光），不再是一整块平涂的直边长条（2026-11-17 用户反馈雪地树影假）
	float4 nShadow = noiseTex.sample(noiseSmp, world * 0.21 + float2(0.17, 0.53));
	float2 shadowWarp = (nShadow.rg - 0.5) * 0.3;
	float sunShadow = lightTex.sample(lightSmp, windowToUV(rel + shadowWarp, slots, slotOrigin)).r * P.shade.w * P.misc.y;
	sunShadow *= 0.72 + 0.28 * smoothstep(0.15, 0.85, noiseTex.sample(noiseSmp, world * 0.7).b);
	color *= mix(float3(1.0), P.shade.rgb * 0.6, sunShadow);

	// 雾云场：G 生雾多少，B 山腰绕云多少（与离岸距离图同一张，按地图坐标取样）。
	// 没有起伏图的物件（英雄、城镇、宝物、资源、船……都是可交互的）上不起雾、不盖云：雾和云只是气氛，不能挡住要点的东西
	float2 fieldSize = max(P.misc.zw, float2(1.0));
	float interactive = saturate(relief.a * 1.5) * (1.0 - surface.known);
	float4 field = depthTex.sample(depthSmp, world / fieldSize) * P.misc.y * (1.0 - interactive);
	const float2 cloudWind = float2(0.94, 0.34);
	float cloudAltitude = P.camera.w;

	// 云量：天上处处有稀疏的云，山周围（雾云场 B）更浓；地图外、可交互物件上没有
	float inside = smoothstep(0.0, 1.0, world.x) * smoothstep(0.0, 1.0, fieldSize.x - world.x)
		* smoothstep(0.0, 1.0, world.y) * smoothstep(0.0, 1.0, fieldSize.y - world.y);
	float cloudCover = (0.35 * P.misc.y * (1.0 - interactive) + 0.65 * field.b) * inside;

	// 云影：天上的云在太阳下投下的影子，一团团随云飘过地面和山坡——雪原上白云看不出来，靠云影看出天上有云。
	// 云在高处，所以影子顺着长投影的方向错开
	if(cloudCover * P.atmos.y > 0.003)
	{
		float2 shadowSource = world - P.shadowDir.xy * cloudAltitude - float2(0.0, cloudAltitude) + P.camera.xy * P.camera.z + cloudWind * (t * 0.12);
		float shading = cloudDensity(cloudShapeCoarse(shadowSource, noiseTex, noiseSmp), cloudCover);
		color *= 1.0 - 0.25 * shading * P.atmos.y * P.sun.w * (1.0 - smoothstep(cloudAltitude - 0.3, cloudAltitude + 0.3, surface.height));
	}

	// 流动的雾，贴着地面，在物件与粒子之间
	if(field.g * P.atmos.x > 0.003)
	{
		float fog = fogAt(world, t, field.g, surface.height, noiseTex, noiseSmp) * P.atmos.x;
		float3 fogColor = mix(float3(0.86, 0.89, 0.92), float3(0.86, 0.89, 0.92) * P.sunColor.rgb, 0.35);
		color = mix(color, fogColor, saturate(fog));
	}

	// 云层（山腰云海）：在离地 cloudAltitude 处。滚动地图时云层比地面移得快（视差），像离镜头更近；
	// 比云层高的山峰从云里伸出来，矮处、地面被半透明的云盖住。迎着太阳的一侧亮、背面偏灰
	if(cloudCover * P.atmos.y > 0.003)
	{
		float2 cloudPos = world + P.camera.xy * P.camera.z + cloudWind * (t * 0.12);
		float shape = cloudShape(cloudPos, noiseTex, noiseSmp);
		float density = cloudDensity(shape, cloudCover) * (1.0 - smoothstep(cloudAltitude - 0.2, cloudAltitude + 0.4, surface.height));
		if(density > 0.001)
		{
			float2 toSun = -P.shadowDir.xy / max(length(P.shadowDir.xy), 1e-3);
			float lit = saturate(0.55 + (cloudShapeCoarse(cloudPos, noiseTex, noiseSmp) - cloudShapeCoarse(cloudPos + toSun * 0.35, noiseTex, noiseSmp)) * 4.0);
			float3 cloudColor = mix(float3(0.63, 0.67, 0.75), float3(0.98, 0.97, 0.95), lit) * mix(float3(1.0), P.sunColor.rgb, 0.4);
			color = mix(color, cloudColor, density * 0.55 * P.atmos.y);
		}
	}

	// 时段色调：清晨、傍晚整体偏暖
	color *= mix(float3(1.0), P.grade.rgb, P.misc.y);

	// 植被粒子（MapParticles）和覆盖层（迷雾、路径）在这一遍之后另外画上去，粒子才能夹在物件和迷雾之间

	main0_out out;
	out.color = float4(color, 1.0) * in.color;
	return out;
}
)MSL";

/// 长投影的预处理着色器（MSL，2026-11-17）：画进 1/4 大小的光照图，R 是这一点在太阳下的遮挡程度 0~1。
/// texture(0) 是 SDL 绑定的主纹理（1/4 起伏层，不环绕，这里不用），texture(1) 是同一张起伏层、环绕取样。
///
/// 起伏层里每个物件像素有离地高度 H（由立绘推出：同一列里离物件最下沿多高），把它看成立在“地面线”上的一块竖板。
/// 一个离地 h 的点 x 朝太阳看：沿太阳方向每升高 s，它在地面上的投影朝太阳走 −d·s（d 是影子的方向与长度），
/// 在屏幕上就走到 x − (d.x, d.y + 1)·s。那里的物件表面若比射线高（且不至于高出太多、在它背后很远），这一点就在影子里。
/// 太阳总在南边（屏幕下方）一侧，影子总朝屏幕上方偏，竖板不会自己挡住自己。
/// 可见窗口外缓存里没有东西，射线走出窗口就停。影子边缘随距离变软（远处的遮挡物给出半影），远处也稍淡
const char * const shadowShaderSource = R"SHADOWMSL(
#include <metal_stdlib>
using namespace metal;

struct Params
{
	float4 cache;  // xy：光照图（与 1/4 起伏层同大）的像素尺寸；zw：槽位数（图块）
	float4 window; // xy：可见窗口左上角图块所在槽位；z：起伏层 B = 1 对应的高度（图块）
	float4 sun;    // xy：离地 1 个图块的点，影子在屏幕上落出多远（图块）；z：射线最高找到多高（图块）
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

static inline float2 windowPosition(float2 uv, float2 slots, float2 slotOrigin)
{
	return fmod(uv * slots - slotOrigin + slots, slots);
}

static inline float2 windowToUV(float2 position, float2 slots, float2 slotOrigin)
{
	float2 clamped = clamp(position, float2(0.001), slots - 0.001);
	float2 uv = (clamped + slotOrigin) / slots;
	return uv - floor(uv);
}

fragment main0_out main0(main0_in in [[stage_in]], constant Params & P [[buffer(0)]],
	texture2d<float> mainTex [[texture(0)]], sampler mainSmp [[sampler(0)]],
	texture2d<float> reliefTex [[texture(1)]], sampler reliefSmp [[sampler(1)]])
{
	float2 slots = P.cache.zw;
	float2 slotOrigin = P.window.xy;
	float range = P.window.z;
	float2 rel = windowPosition(in.uv, slots, slotOrigin);

	// 接收者自己的高度：物件边缘与地面混在一起，按覆盖度取平均
	float4 here = reliefTex.sample(reliefSmp, in.uv);
	float receiver = here.b * range;
	// 物件表面只接受明显更高的遮挡物投下的影子（大山的影子落在林梢上）。立绘量出的高度对连绵的山脉不准——
	// 后排峰顶从前排山脚量起，偏高——不留余量的话山脉会被自己的后排峰顶罩住
	float margin = 0.45 * smoothstep(0.02, 0.2, receiver);

	float2 march = -float2(P.sun.x, P.sun.y + 1.0);
	float reach = P.sun.z;
	float shadow = 0.0;
	const int steps = 40;
	for(int i = 1; i <= steps; ++i)
	{
		// 近处步子小（细树干、矮石头的影子不漏），远处步子大
		float s = reach * pow(float(i) / float(steps), 1.4);
		float2 p = rel + march * s;
		if(any(p < float2(0.0)) || any(p > slots))
			break;

		float4 c = reliefTex.sample(reliefSmp, windowToUV(p, slots, slotOrigin));
		if(c.a < 0.2)
			continue;
		float caster = c.b / c.a * range;
		float diff = caster - (receiver + s) - margin;
		// 半影随距离变宽：离遮挡物越远（射线升得越高）影子边越虚，影子梢也就不是一刀切的直边
		float soft = 0.06 + 0.22 * s;
		float thickness = 0.5 + 0.8 * caster;
		float occlusion = smoothstep(-soft, soft, diff) * (1.0 - smoothstep(thickness, thickness + 0.4, diff)) * smoothstep(0.2, 0.6, c.a);
		// 影子靠近根部浓，外半段渐淡（天光照进来），梢部剩一半。整片一样深的长条看上去像贴上去的（2026-11-17 用户反馈雪地树影假）
		shadow = max(shadow, occlusion * (1.0 - 0.5 * smoothstep(0.3, 1.0, s / max(caster, 0.3))));
	}

	main0_out out;
	out.color = float4(shadow, 0.0, 0.0, 1.0);
	return out;
}
)SHADOWMSL";

/// 与着色器里 Params 一一对应，每项 16 字节对齐
struct MapUniforms
{
	float cache[4];
	float world[4];
	float misc[4];
	float sun[4];
	float sunColor[4];
	float shade[4];
	float camera[4];
	float atmos[4];
	float shadowDir[4];
	float grade[4];
};

/// 与长投影预处理着色器的 Params 一一对应
struct ShadowUniforms
{
	float cache[4];
	float window[4];
	float sun[4];
};

/// 着色器声明的取样器数：SDL 的主纹理加本类绑定的十一张（含离岸距离图、摇摆层、风场遮罩、起伏层、光照图）。
/// Metal 每个阶段最多 16 个取样器，离岸距离图因此兼存雾云场，没有另占一个
constexpr Uint32 shaderSamplerCount = 12;
/// 长投影预处理着色器：主纹理加一张环绕取样的 1/4 起伏层
constexpr Uint32 shadowSamplerCount = 2;

/// 起伏层 B = 1 对应的离地高度（图块），与着色器的 reliefHeightRange、地形模组 tools/relief.py 一致
constexpr float reliefHeightRange = 4.0f;

/// 云层离地多高（图块）：山高 2~4 个图块，云在山腰
constexpr float cloudAltitude = 1.3f;
/// 云层的视差：地图滚过 1 个图块，云层多移这么多
constexpr float cloudParallax = 0.35f;

constexpr int noiseSize = 256;

/// 可平铺的值噪声：cells×cells 的随机格点，平滑插值；坐标取模，所以纹理首尾相接无缝
float periodicValueNoise(const std::vector<float> & lattice, int cells, float u, float v)
{
	const float x = u * static_cast<float>(cells);
	const float y = v * static_cast<float>(cells);
	const int x0 = static_cast<int>(std::floor(x));
	const int y0 = static_cast<int>(std::floor(y));
	float fx = x - static_cast<float>(x0);
	float fy = y - static_cast<float>(y0);
	fx = fx * fx * (3.0f - 2.0f * fx);
	fy = fy * fy * (3.0f - 2.0f * fy);

	auto at = [&](int ix, int iy)
	{
		return lattice[((iy % cells + cells) % cells) * cells + ((ix % cells + cells) % cells)];
	};

	const float top = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * fx;
	const float bottom = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * fx;
	return top + (bottom - top) * fy;
}

/// 生成噪声纹理：R、G 是两路独立的低频噪声（涟漪），B、A 是两路较细的噪声（波光）。
/// 每路叠两个八度，再拉伸到 0~1 全范围（否则插值后的值挤在 0.5 附近，波光阈值很难调）。种子固定。
std::vector<uint8_t> generateNoisePixels()
{
	std::mt19937 random(20261117);
	std::uniform_real_distribution<float> unit(0.0f, 1.0f);

	// 每个通道两个八度的格点数
	const std::array<std::array<int, 2>, 4> octaves = {{ {{8, 16}}, {{8, 16}}, {{24, 48}}, {{24, 48}} }};
	std::array<std::array<std::vector<float>, 2>, 4> lattices;

	for(size_t channel = 0; channel < 4; ++channel)
	{
		for(size_t octave = 0; octave < 2; ++octave)
		{
			const int cells = octaves[channel][octave];
			lattices[channel][octave].resize(cells * cells);
			for(float & value : lattices[channel][octave])
				value = unit(random);
		}
	}

	std::array<std::vector<float>, 4> values;
	for(size_t channel = 0; channel < 4; ++channel)
	{
		values[channel].resize(noiseSize * noiseSize);
		for(int y = 0; y < noiseSize; ++y)
		{
			for(int x = 0; x < noiseSize; ++x)
			{
				const float u = static_cast<float>(x) / noiseSize;
				const float v = static_cast<float>(y) / noiseSize;
				const float coarse = periodicValueNoise(lattices[channel][0], octaves[channel][0], u, v);
				const float fine = periodicValueNoise(lattices[channel][1], octaves[channel][1], u, v);
				values[channel][y * noiseSize + x] = coarse + 0.5f * fine;
			}
		}
	}

	std::vector<uint8_t> pixels(noiseSize * noiseSize * 4);
	for(size_t channel = 0; channel < 4; ++channel)
	{
		const auto [low, high] = std::minmax_element(values[channel].begin(), values[channel].end());
		const float range = std::max(*high - *low, 0.0001f);
		for(size_t i = 0; i < values[channel].size(); ++i)
			pixels[i * 4 + channel] = static_cast<uint8_t>(std::clamp((values[channel][i] - *low) / range * 255.0f, 0.0f, 255.0f));
	}
	return pixels;
}

SDL_GPUDevice * deviceOf(SDL_Renderer * renderer)
{
	return static_cast<SDL_GPUDevice *>(SDL_GetPointerProperty(SDL_GetRendererProperties(renderer), SDL_PROP_RENDERER_GPU_DEVICE_POINTER, nullptr));
}

SDL_GPUTexture * gpuTextureOf(SDL_Texture * texture)
{
	return static_cast<SDL_GPUTexture *>(SDL_GetPointerProperty(SDL_GetTextureProperties(texture), SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER, nullptr));
}

}

MapLayerCompositor::~MapLayerCompositor()
{
	// 渲染器应已先调用 release()；到这里还有资源说明渲染器已不在，只能放弃这些指针
	if(shader || sampler || noise || state || shadowShader)
		logGlobal->warn("Map layer compositor destroyed without release()");
}

bool MapLayerCompositor::isShaderPathAvailable(SDL_Renderer * renderer)
{
	if(!renderer)
		return false;

	const char * name = SDL_GetRendererName(renderer);
	if(!name || std::string(name) != "gpu")
		return false;

	SDL_GPUDevice * device = deviceOf(renderer);
	return device && (SDL_GetGPUShaderFormats(device) & SDL_GPU_SHADERFORMAT_MSL);
}

bool MapLayerCompositor::ensureShaderResources(SDL_Renderer * renderer)
{
	if(owner != renderer)
	{
		// 换了渲染器：旧资源随旧渲染器一起没了（release() 已在销毁前调用），从头再来
		shader = nullptr;
		sampler = nullptr;
		noise = nullptr;
		state = nullptr;
		clampSampler = nullptr;
		depthField.texture = nullptr;
		depthField.dirty = true;
		maskLevels = {};
		maskLevelsSourceSize = Point();
		shadowShader = nullptr;
		shadowState = nullptr;
		reliefLevels = {};
		lightMap = nullptr;
		reliefLevelsSourceSize = Point();
		boundTextures = {};
		shaderUnavailable = false;
		owner = renderer;
	}

	if(shaderUnavailable)
		return false;

	if(shader && shadowShader && sampler && clampSampler && noise)
		return true;

	if(!isShaderPathAvailable(renderer))
	{
		shaderUnavailable = true;
		logGlobal->info("Map water effects need the 'gpu' render driver with MSL shader support - drawing map layers without them");
		return false;
	}

	SDL_GPUDevice * device = deviceOf(renderer);

	SDL_GPUShaderCreateInfo shaderInfo;
	SDL_zero(shaderInfo);
	shaderInfo.code = reinterpret_cast<const Uint8 *>(waterShaderSource);
	shaderInfo.code_size = std::strlen(waterShaderSource);
	shaderInfo.entrypoint = "main0";
	shaderInfo.format = SDL_GPU_SHADERFORMAT_MSL;
	shaderInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
	shaderInfo.num_samplers = shaderSamplerCount;
	shaderInfo.num_uniform_buffers = 1;
	shader = SDL_CreateGPUShader(device, &shaderInfo);

	shaderInfo.code = reinterpret_cast<const Uint8 *>(shadowShaderSource);
	shaderInfo.code_size = std::strlen(shadowShaderSource);
	shaderInfo.num_samplers = shadowSamplerCount;
	shadowShader = SDL_CreateGPUShader(device, &shaderInfo);

	SDL_GPUSamplerCreateInfo samplerInfo;
	SDL_zero(samplerInfo);
	samplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
	samplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
	samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
	// 缓存是环形存放的，噪声纹理可平铺，所以一律环绕取样
	samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
	samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
	samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
	sampler = SDL_CreateGPUSampler(device, &samplerInfo);

	// 离岸距离图按地图坐标取样，地图外夹到边缘的值
	samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	clampSampler = SDL_CreateGPUSampler(device, &samplerInfo);

	noise = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, noiseSize, noiseSize);
	if(noise)
	{
		const std::vector<uint8_t> pixels = generateNoisePixels();
		SDL_UpdateTexture(noise, nullptr, pixels.data(), noiseSize * 4);
	}

	if(!shader || !shadowShader || !sampler || !clampSampler || !noise || !gpuTextureOf(noise))
	{
		logGlobal->error("Failed to set up the map water shader: %s - drawing map layers without it", SDL_GetError());
		release();
		owner = renderer;
		shaderUnavailable = true;
		return false;
	}

	logGlobal->info("Map water shader ready");
	return true;
}

void MapLayerCompositor::destroyMaskLevels()
{
	for(SDL_Texture *& level : maskLevels)
	{
		if(level)
			SDL_DestroyTexture(level);
		level = nullptr;
	}
	maskLevelsSourceSize = Point();
}

bool MapLayerCompositor::ensureMaskLevels(SDL_Renderer * renderer, SDL_Texture * waterMask)
{
	float width = 0;
	float height = 0;
	SDL_GetTextureSize(waterMask, &width, &height);
	const Point sourceSize(static_cast<int>(width), static_cast<int>(height));

	if(maskLevels.back() && sourceSize == maskLevelsSourceSize)
		return true;

	destroyMaskLevels();

	// 缓存尺寸是 槽位数 × 图块像素（32 的倍数），所以每级正好减半，环形排布在各级里保持对齐
	Point size = sourceSize;
	for(SDL_Texture *& level : maskLevels)
	{
		size = Point(std::max(1, size.x / 2), std::max(1, size.y / 2));
		level = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, size.x, size.y);
		if(!level)
		{
			logGlobal->error("Failed to create a %dx%d water mask level: %s", size.x, size.y, SDL_GetError());
			destroyMaskLevels();
			shaderUnavailable = true;
			return false;
		}
		SDL_SetTextureBlendMode(level, SDL_BLENDMODE_NONE);
		SDL_SetTextureScaleMode(level, SDL_SCALEMODE_LINEAR);
	}

	maskLevelsSourceSize = sourceSize;
	return true;
}

void MapLayerCompositor::destroyReliefLevels()
{
	if(shadowState)
		SDL_DestroyGPURenderState(shadowState);
	shadowState = nullptr;

	for(SDL_Texture *& level : reliefLevels)
	{
		if(level)
			SDL_DestroyTexture(level);
		level = nullptr;
	}
	if(lightMap)
		SDL_DestroyTexture(lightMap);
	lightMap = nullptr;
	reliefLevelsSourceSize = Point();
}

bool MapLayerCompositor::ensureReliefLevels(SDL_Renderer * renderer, SDL_Texture * relief)
{
	float width = 0;
	float height = 0;
	SDL_GetTextureSize(relief, &width, &height);
	const Point sourceSize(static_cast<int>(width), static_cast<int>(height));

	if(shadowState && sourceSize == reliefLevelsSourceSize)
		return true;

	destroyReliefLevels();

	auto create = [&](const Point & size)
	{
		SDL_Texture * texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, size.x, size.y);
		if(texture)
		{
			SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE);
			SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
		}
		else
			logGlobal->error("Failed to create a %dx%d map lighting texture: %s", size.x, size.y, SDL_GetError());
		return texture;
	};

	// 与水面遮罩一样每级减半，环形排布在各级里保持对齐
	Point size = sourceSize;
	for(SDL_Texture *& level : reliefLevels)
	{
		size = Point(std::max(1, size.x / 2), std::max(1, size.y / 2));
		level = create(size);
	}
	lightMap = create(size);

	SDL_GPUTextureSamplerBinding binding;
	SDL_zero(binding);
	binding.texture = reliefLevels.back() ? gpuTextureOf(reliefLevels.back()) : nullptr;
	binding.sampler = sampler;

	if(!reliefLevels[0] || !reliefLevels[1] || !lightMap || !binding.texture)
	{
		destroyReliefLevels();
		shaderUnavailable = true;
		return false;
	}

	SDL_GPURenderStateCreateInfo stateInfo;
	SDL_zero(stateInfo);
	stateInfo.fragment_shader = shadowShader;
	stateInfo.num_sampler_bindings = 1;
	stateInfo.sampler_bindings = &binding;
	shadowState = SDL_CreateGPURenderState(renderer, &stateInfo);
	if(!shadowState)
	{
		logGlobal->error("Failed to create the map shadow render state: %s", SDL_GetError());
		destroyReliefLevels();
		shaderUnavailable = true;
		return false;
	}

	reliefLevelsSourceSize = sourceSize;
	// 光照图换了，绑着它的合成状态也要重建
	inputsForgotten = true;
	return true;
}

MapLayerCompositor::Sun MapLayerCompositor::currentSun()
{
	constexpr float pi = 3.14159265f;

	// 时段 0 为清晨、0.5 为正午、1 为傍晚。开局从早上开始；到傍晚后影子淡出，再从清晨淡入
	const double cycle = settings["video"]["mapSunCycle"].Float();
	float time = static_cast<float>(settings["video"]["mapSunTime"].Float());
	if(cycle > 0.0)
		time = static_cast<float>(std::fmod(0.2 + static_cast<double>(SDL_GetTicks()) / 1000.0 / cycle, 1.0));
	time = std::clamp(time, 0.0f, 1.0f);

	const float day = std::sin(pi * time); // 清晨、傍晚为 0，正午为 1
	const float warm = std::pow(1.0f - day, 1.5f);
	const float fade = std::clamp(time / 0.04f, 0.0f, 1.0f) * std::clamp((1.0f - time) / 0.04f, 0.0f, 1.0f);

	// 太阳从东走到西（地图上方是北）。方位角从正东量起、朝南为正：上午大半时间停在东边（影子朝左，落在物件旁边看得见，
	// 与原版阴影同向），正午前后很快转过南边（这时影子短、朝上，大多藏在物件背后），下午停在西边。
	// 日出、日落略偏北，影子稍朝下——物件高度按立绘打过折，不会因此自己挡住自己
	const float swing = 2.0f * time - 1.0f;
	const float azimuth = pi * (0.5f + 0.55f * (swing < 0.0f ? -1.0f : 1.0f) * std::pow(std::abs(swing), 0.45f));
	const float elevation = (14.0f + 44.0f * std::pow(day, 1.3f)) * pi / 180.0f;
	const float sunEast = std::cos(elevation) * std::cos(azimuth);
	const float sunNorth = -std::cos(elevation) * std::sin(azimuth);
	const float sunUp = std::sin(elevation);

	// 换到屏幕坐标：大地图约是 66° 俯视，屏幕“上”是北偏上、“朝镜头”是南偏上（与着色器的 groundNormal 一致）
	const float tiltSin = 0.917f;
	const float tiltCos = 0.399f;
	std::array<float, 3> direction = {sunEast, sunNorth * tiltSin + sunUp * tiltCos, -sunNorth * tiltCos + sunUp * tiltSin};
	const float length = std::hypot(direction[0], direction[1], direction[2]);
	for(float & component : direction)
		component /= length;

	// 影子朝太阳的反方向落，长度是高度除以太阳高度角的正切；太阳太低时影子会拖到屏幕外，封顶 2.4 倍
	const float shadowLength = std::min(std::cos(elevation) / std::sin(elevation), 2.4f) * 0.85f;

	auto mixColor = [](std::array<float, 3> a, const std::array<float, 3> & b, float k)
	{
		for(size_t i = 0; i < 3; ++i)
			a[i] += (b[i] - a[i]) * k;
		return a;
	};

	Sun result;
	result.direction = direction;
	result.shadow = {-std::cos(azimuth) * shadowLength, -std::sin(azimuth) * shadowLength};
	result.color = mixColor({1.0f, 0.98f, 0.95f}, {1.0f, 0.80f, 0.58f}, warm);
	result.shade = mixColor({0.80f, 0.86f, 1.0f}, {0.70f, 0.74f, 0.98f}, warm);
	result.grade = mixColor({1.0f, 1.0f, 1.0f}, {1.04f, 0.96f, 0.88f}, warm * 0.55f);
	result.relight = 0.85f * fade;
	result.rim = 0.25f + 0.45f * warm;
	result.strength = 0.8f * fade;
	result.reach = reliefHeightRange;
	return result;
}

void MapLayerCompositor::renderLightMap(SDL_Renderer * renderer, const Inputs & inputs)
{
	if(!inputs.relief || !ensureReliefLevels(renderer, inputs.relief))
		return;

	SDL_Texture * source = inputs.relief;
	SDL_SetTextureBlendMode(source, SDL_BLENDMODE_NONE);
	SDL_SetTextureScaleMode(source, SDL_SCALEMODE_LINEAR);
	for(SDL_Texture * level : reliefLevels)
	{
		SDL_SetRenderTarget(renderer, level);
		SDL_RenderTexture(renderer, source, nullptr, nullptr);
		source = level;
	}

	float width = 0;
	float height = 0;
	SDL_GetTextureSize(lightMap, &width, &height);

	ShadowUniforms uniforms = {};
	uniforms.cache[0] = width;
	uniforms.cache[1] = height;
	uniforms.cache[2] = static_cast<float>(inputs.slots.x);
	uniforms.cache[3] = static_cast<float>(inputs.slots.y);
	uniforms.window[0] = static_cast<float>(inputs.slotOrigin.x);
	uniforms.window[1] = static_cast<float>(inputs.slotOrigin.y);
	uniforms.window[2] = reliefHeightRange;
	uniforms.sun[0] = sun.shadow[0];
	uniforms.sun[1] = sun.shadow[1];
	uniforms.sun[2] = sun.reach;

	SDL_SetRenderTarget(renderer, lightMap);
	SDL_SetGPURenderStateFragmentUniforms(shadowState, 0, &uniforms, sizeof(uniforms));
	SDL_SetGPURenderState(renderer, shadowState);
	SDL_RenderTexture(renderer, reliefLevels.back(), nullptr, nullptr);
	SDL_SetGPURenderState(renderer, nullptr);
}

void MapLayerCompositor::prepare(SDL_Renderer * renderer, const Inputs & inputs)
{
	if(!inputs.waterMask || !ensureShaderResources(renderer) || !ensureMaskLevels(renderer, inputs.waterMask))
		return;

	sun = currentSun();

	// 每级把上一级缩小一半：线性过滤在 2×2 像素正中取样，正好是 2×2 的平均
	SDL_Texture * previousTarget = SDL_GetRenderTarget(renderer);
	SDL_Texture * source = inputs.waterMask;
	SDL_SetTextureBlendMode(source, SDL_BLENDMODE_NONE);
	SDL_SetTextureScaleMode(source, SDL_SCALEMODE_LINEAR);

	for(SDL_Texture * level : maskLevels)
	{
		SDL_SetRenderTarget(renderer, level);
		SDL_RenderTexture(renderer, source, nullptr, nullptr);
		source = level;
	}

	renderLightMap(renderer, inputs);

	SDL_SetRenderTarget(renderer, previousTarget);
}

void MapLayerCompositor::setDepthField(const std::vector<uint8_t> & distances, const Point & size)
{
	// 离岸距离在 R 通道；G、B 是雾云场（setAtmosphereField），换了尺寸就一并清掉
	if(depthField.size != size || depthField.rgba.size() != distances.size() * 4)
		depthField.rgba.assign(distances.size() * 4, 0);
	for(size_t i = 0; i < distances.size(); ++i)
		depthField.rgba[i * 4] = distances[i];
	depthField.size = size;
	depthField.dirty = true;
}

void MapLayerCompositor::setAtmosphereField(const std::vector<uint8_t> & rgba, const Point & size)
{
	if(depthField.size != size || depthField.rgba.size() != rgba.size())
	{
		depthField.rgba.assign(rgba.size(), 0);
		depthField.size = size;
	}
	for(size_t i = 0; i + 3 < rgba.size(); i += 4)
	{
		depthField.rgba[i + 1] = rgba[i + 1];
		depthField.rgba[i + 2] = rgba[i + 2];
	}
	depthField.dirty = true;
}

void MapLayerCompositor::setVegetationField(const std::vector<uint8_t> & rgba, const Point & size)
{
	particles.setVegetationField(rgba, size);
}

float MapLayerCompositor::shaderTime()
{
	return static_cast<float>(SDL_GetTicks() % 10000000) / 1000.0f;
}

void MapLayerCompositor::startBirdFlock(float x, float y, float directionX, float directionY)
{
	particles.startBirdFlock(x, y, directionX, directionY, shaderTime());
}

bool MapLayerCompositor::ensureFieldTexture(SDL_Renderer * renderer, TileField & field, const std::array<uint8_t, 4> & fallback, const char * name)
{
	if(field.texture && !field.dirty)
		return true;

	const bool hasField = field.size.x > 0 && field.size.y > 0 && field.rgba.size() == static_cast<size_t>(field.size.x * field.size.y) * 4;
	const Point size = hasField ? field.size : Point(1, 1);

	float width = 0;
	float height = 0;
	if(field.texture)
		SDL_GetTextureSize(field.texture, &width, &height);

	if(!field.texture || static_cast<int>(width) != size.x || static_cast<int>(height) != size.y)
	{
		if(field.texture)
			SDL_DestroyTexture(field.texture);
		field.texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, size.x, size.y);
		if(!field.texture)
		{
			logGlobal->error("Failed to create the %dx%d %s texture: %s", size.x, size.y, name, SDL_GetError());
			shaderUnavailable = true;
			return false;
		}
		// 新纹理可能分到旧纹理的地址，绑着旧纹理的渲染状态必须重建
		inputsForgotten = true;
	}

	if(hasField)
		SDL_UpdateTexture(field.texture, nullptr, field.rgba.data(), size.x * 4);
	else
		SDL_UpdateTexture(field.texture, nullptr, fallback.data(), 4);

	field.dirty = false;
	return true;
}

void MapLayerCompositor::forgetInputs()
{
	inputsForgotten = true;
	particles.forgetInputs();
}

bool MapLayerCompositor::ensureRenderState(SDL_Renderer * renderer, const Inputs & inputs)
{
	const std::array<SDL_Texture *, 10> wanted = {inputs.ground, inputs.objects, inputs.sway, inputs.waterMask, inputs.windMask, maskLevels[2], maskLevels[3], depthField.texture, inputs.relief, lightMap};

	// 缓存画布重建后，新纹理可能恰好分到旧纹理的地址，单比指针不可靠，所以还要看 forgetInputs() 的标记
	const bool forgotten = inputsForgotten.exchange(false);

	if(state && boundTextures == wanted && !forgotten)
		return true;

	if(state)
	{
		SDL_DestroyGPURenderState(state);
		state = nullptr;
	}
	boundTextures = {};

	// 顺序与着色器的 texture(1)~texture(11) 一一对应；离岸距离图按地图坐标取样、不环绕，用夹边采样器。
	// 覆盖层不在这里：粒子要夹在物件和迷雾之间，覆盖层在粒子之后另画（见 composeWithShader）
	const std::array<SDL_Texture *, 11> textures = {inputs.ground, inputs.objects, inputs.waterMask, noise, maskLevels[2], maskLevels[3], depthField.texture, inputs.sway, inputs.windMask, inputs.relief, lightMap};
	std::array<SDL_GPUTextureSamplerBinding, 11> bindings = {};

	for(size_t i = 0; i < textures.size(); ++i)
	{
		bindings[i].texture = textures[i] ? gpuTextureOf(textures[i]) : nullptr;
		bindings[i].sampler = textures[i] == depthField.texture ? clampSampler : sampler;
		if(!bindings[i].texture)
		{
			logGlobal->error("Map compositor input %d has no GPU texture - drawing map layers without the water shader", static_cast<int>(i));
			shaderUnavailable = true;
			return false;
		}
	}

	SDL_GPURenderStateCreateInfo stateInfo;
	SDL_zero(stateInfo);
	stateInfo.fragment_shader = shader;
	stateInfo.num_sampler_bindings = static_cast<Sint32>(bindings.size());
	stateInfo.sampler_bindings = bindings.data();
	state = SDL_CreateGPURenderState(renderer, &stateInfo);

	if(!state)
	{
		// 失败一次就不再每帧重试，免得刷屏
		logGlobal->error("Failed to create the map water render state: %s - drawing map layers without it", SDL_GetError());
		shaderUnavailable = true;
		return false;
	}

	boundTextures = wanted;
	logGlobal->debug("Map water render state rebuilt");
	return true;
}

void MapLayerCompositor::compose(SDL_Renderer * renderer, const Inputs & inputs, const std::vector<PresentedRegion> & regions, int scaleMode)
{
	const bool shaderReady = ensureShaderResources(renderer) && maskLevels.back() && maskLevelsSourceSize.x > 0 && lightMap
		&& ensureFieldTexture(renderer, depthField, {128, 0, 0, 255}, "sea depth")
		&& ensureRenderState(renderer, inputs);

	if(shaderReady)
		composeWithShader(renderer, inputs, regions);
	else
		composeLayerByLayer(renderer, inputs, regions, scaleMode);
}

void MapLayerCompositor::composeWithShader(SDL_Renderer * renderer, const Inputs & inputs, const std::vector<PresentedRegion> & regions)
{
	float width = 0;
	float height = 0;
	SDL_GetTextureSize(inputs.ground, &width, &height);

	MapUniforms uniforms = {};
	uniforms.cache[0] = width;
	uniforms.cache[1] = height;
	uniforms.cache[2] = static_cast<float>(inputs.slots.x);
	uniforms.cache[3] = static_cast<float>(inputs.slots.y);
	uniforms.world[0] = static_cast<float>(inputs.worldOrigin.x);
	uniforms.world[1] = static_cast<float>(inputs.worldOrigin.y);
	uniforms.world[2] = static_cast<float>(inputs.slotOrigin.x);
	uniforms.world[3] = static_cast<float>(inputs.slotOrigin.y);
	const float now = shaderTime();
	uniforms.misc[0] = now;
	uniforms.misc[1] = 1.0f;
	uniforms.misc[2] = static_cast<float>(std::max(1, depthField.size.x));
	uniforms.misc[3] = static_cast<float>(std::max(1, depthField.size.y));

	// 太阳（prepare() 里算好）
	for(size_t i = 0; i < 3; ++i)
	{
		uniforms.sun[i] = sun.direction[i];
		uniforms.sunColor[i] = sun.color[i];
		uniforms.shade[i] = sun.shade[i];
		uniforms.grade[i] = sun.grade[i];
	}
	uniforms.sun[3] = sun.relight;
	uniforms.sunColor[3] = sun.rim;
	uniforms.shade[3] = sun.strength;
	uniforms.shadowDir[0] = sun.shadow[0];
	uniforms.shadowDir[1] = sun.shadow[1];

	// 屏幕中心的地图坐标（云层视差用）：第一块区域正是可见窗口左上角图块所在的那一块，从它的屏幕位置推算
	if(!regions.empty() && regions.front().target.w > 0 && inputs.slots.x > 0)
	{
		Rect bounds = regions.front().target;
		for(const PresentedRegion & region : regions)
			bounds = bounds.include(region.target);
		const float cachePerScreen = static_cast<float>(regions.front().source.w) / static_cast<float>(regions.front().target.w);
		const float tileSize = width / static_cast<float>(inputs.slots.x);
		const Point center = bounds.center() - regions.front().target.topLeft();
		uniforms.camera[0] = static_cast<float>(inputs.worldOrigin.x) + static_cast<float>(center.x) * cachePerScreen / tileSize;
		uniforms.camera[1] = static_cast<float>(inputs.worldOrigin.y) + static_cast<float>(center.y) * cachePerScreen / tileSize;
	}
	uniforms.camera[2] = cloudParallax;
	uniforms.camera[3] = cloudAltitude;
	uniforms.atmos[0] = 0.36f; // 雾
	uniforms.atmos[1] = 0.85f; // 云

	SDL_SetGPURenderStateFragmentUniforms(state, 0, &uniforms, sizeof(uniforms));
	SDL_SetGPURenderState(renderer, state);

	// 着色器自己输出不透明的最终颜色，所以这次绘制直接覆盖
	SDL_SetTextureBlendMode(inputs.ground, SDL_BLENDMODE_NONE);

	for(const PresentedRegion & region : regions)
	{
		const SDL_FRect from = CSDL_Ext::toSDLFloat(region.source);
		const SDL_FRect to = CSDL_Ext::toSDLFloat(region.target);

		if(!SDL_RenderTexture(renderer, inputs.ground, &from, &to))
			logGlobal->error("Failed to compose a map region with the water shader: %s", SDL_GetError());
	}

	SDL_SetGPURenderState(renderer, nullptr);

	// 植被粒子：按屏幕上一个图块多大淡入。缩到图块不足十几个像素时叶片、鸟都小于一个像素，物件动画此时也已停下
	MapParticles::Frame frame;
	frame.windMask = inputs.windMask;
	frame.sway = inputs.sway;
	frame.slots = inputs.slots;
	frame.worldOrigin = inputs.worldOrigin;
	frame.slotOrigin = inputs.slotOrigin;
	frame.cacheWidth = width;
	frame.time = now;
	if(!regions.empty() && regions.front().source.w > 0 && inputs.slots.x > 0)
	{
		const float tilePixels = static_cast<float>(regions.front().target.w) / static_cast<float>(regions.front().source.w) * width / static_cast<float>(inputs.slots.x);
		const float fade = std::clamp((tilePixels - 12.0f) / 8.0f, 0.0f, 1.0f);
		frame.strength = fade * fade * (3.0f - 2.0f * fade);
	}
	particles.draw(renderer, frame, regions);

	// 覆盖层（迷雾、路径）最后叠上，盖住粒子
	SDL_SetTextureBlendMode(inputs.overlay, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
	SDL_SetTextureScaleMode(inputs.overlay, SDL_SCALEMODE_LINEAR);
	for(const PresentedRegion & region : regions)
	{
		const SDL_FRect from = CSDL_Ext::toSDLFloat(region.source);
		const SDL_FRect to = CSDL_Ext::toSDLFloat(region.target);

		if(!SDL_RenderTexture(renderer, inputs.overlay, &from, &to))
			logGlobal->error("Failed to compose the map overlay: %s", SDL_GetError());
	}
}

void MapLayerCompositor::composeLayerByLayer(SDL_Renderer * renderer, const Inputs & inputs, const std::vector<PresentedRegion> & regions, int scaleMode) const
{
	// 没有着色器排前后，摇摆物件一律叠在静止物件之上
	const std::array<std::pair<SDL_Texture *, SDL_BlendMode>, 4> layers = {{
		{inputs.ground, SDL_BLENDMODE_NONE},
		{inputs.objects, SDL_BLENDMODE_BLEND_PREMULTIPLIED},
		{inputs.sway, SDL_BLENDMODE_BLEND_PREMULTIPLIED},
		{inputs.overlay, SDL_BLENDMODE_BLEND_PREMULTIPLIED},
	}};

	for(const auto & [texture, blendMode] : layers)
	{
		SDL_SetTextureBlendMode(texture, blendMode);
		SDL_SetTextureScaleMode(texture, static_cast<SDL_ScaleMode>(scaleMode));

		for(const PresentedRegion & region : regions)
		{
			const SDL_FRect from = CSDL_Ext::toSDLFloat(region.source);
			const SDL_FRect to = CSDL_Ext::toSDLFloat(region.target);

			if(!SDL_RenderTexture(renderer, texture, &from, &to))
				logGlobal->error("Failed to compose a map layer region: %s", SDL_GetError());
		}
	}
}

void MapLayerCompositor::release()
{
	if(state)
		SDL_DestroyGPURenderState(state);

	if(owner && (shader || sampler || clampSampler))
	{
		SDL_GPUDevice * device = deviceOf(owner);
		if(device && shader)
			SDL_ReleaseGPUShader(device, shader);
		if(device && sampler)
			SDL_ReleaseGPUSampler(device, sampler);
		if(device && clampSampler)
			SDL_ReleaseGPUSampler(device, clampSampler);
	}

	if(noise)
		SDL_DestroyTexture(noise);

	destroyReliefLevels();
	if(owner && shadowShader)
	{
		SDL_GPUDevice * device = deviceOf(owner);
		if(device)
			SDL_ReleaseGPUShader(device, shadowShader);
	}
	shadowShader = nullptr;

	if(depthField.texture)
		SDL_DestroyTexture(depthField.texture);
	depthField.texture = nullptr;
	depthField.dirty = true; // 数据留着，下一个渲染器上重建纹理
	clampSampler = nullptr;

	particles.release();

	destroyMaskLevels();

	state = nullptr;
	shader = nullptr;
	sampler = nullptr;
	noise = nullptr;
	boundTextures = {};
	owner = nullptr;
	shaderUnavailable = false;
}
