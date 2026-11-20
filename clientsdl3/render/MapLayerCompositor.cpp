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
#include "Canvas.h"

#include "lib/CConfigHandler.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

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
/// 昼夜与天气（2026-11-18）每个像素再取 5 次：地形场的天气通道 1 次、大块云影 3 次、阵雨 1 次（都是小纹理）；
/// 热浪、瘴气、熔岩处另取噪声 1~3 次。局部光不取样，是 uniform 里最多 32 盏灯的循环（照不到的灯第一步就跳过）。
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
	float4 atmos;     // x：雾的浓度；y：云海的浓度；z：这一帧之后后处理统一色调的强度（不经后处理时为 0）；w：修仙建筑保色的程度
	float4 shadowDir; // xy：离地 1 个图块的点，影子在屏幕上落出多远（图块）
	float4 grade;     // rgb：整幅画面的时段色调
	float4 terrain;   // x：地表材质强度（缩得太小时为 0）；y：一格材质铺多少图块；zw：地形场尺寸（图块）
	float4 atlas;     // xy：材质图集像素尺寸；z：一格多少像素；w：每格四周环绕边的宽度（像素）
	float4 cellMean[44]; // 材质图集每格的平均颜色（rgb）与平均高度（a），按 组 × 4 + 子材质 排（组 8、9 是特殊地面，组 10 是海面）
	float4 night;     // x：天黑的程度；y：局部光显出来多少；z：夜里颜色褪掉多少；w：沙地热浪的强度
	float4 weather;   // x：大块云影的浓度；y：阵雨开关（0 或 1）
	float4 lightInfo; // x：这一帧有几盏局部光；y：有几对蹄印
	float4 lightPos[32];   // 局部光：xy 地面位置（地图坐标），z 照多远（图块），w 离地多高（图块）
	float4 lightColor[32]; // rgb 颜色乘强度（已含闪动），w 夜里空气中辉光的多少
	float4 lightGrid;      // 局部光分格：xy 第一格左上角的地图坐标，zw 一格多大（图块）
	uint4 lightCells[64];  // 16 × 16 格，每格一个 32 位掩码：第 i 位为 1 表示第 i 盏灯照得到这一格
	float4 printBox;       // 蹄印的外接矩形（地图坐标，已留出蹄印的大小）：左、上、右、下
	float4 prints[64];     // 蹄印：xy 一对蹄印的中点（地图坐标），z 走的方向（弧度），w 浓淡 + 2 × 地面（0 沙地、1 雪地）
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

// 修仙建筑记号（2026-11-18）：模组给按修仙设定重绘的建筑配一张起伏图，R、G 存 (10, 0)/255、A 为覆盖度——
// R+G 不到 0.08，按“没有法线”处理（不按法线重新打光）；雾、云、明暗按下面的 building* 另算（2026-11-19）；
// B 是离地高度（按地面线量，建筑另加 buildingHeightFloor 底数；资源与宝物也带记号，B 为 0）：只给雾、云海、云影和墙脚压暗用，
// 长投影着色器跳过带记号的像素（建筑的影子是烘好的固定投影），记号本身不看 B。
// 真实法线不会落在 R≈0.04、G≈0 这一角（那是长度超过 1 的法线），所以这个组合只当记号用。
// 压在建筑前面、没有起伏图的物件（英雄、宝物）按清零剪影画进起伏层，会把这里的 RGB 乘掉，记号也就跟着消失
static inline float vividMark(float4 r)
{
	if(r.a < 0.02)
		return 0.0;
	float3 enc = r.rgb / r.a;
	return smoothstep(0.015, 0.028, enc.r) * (1.0 - smoothstep(0.06, 0.09, enc.r))
		* (1.0 - smoothstep(0.015, 0.03, enc.g)) * saturate(r.a * 1.5);
}

// 后处理“统一色调”（postShaderSource 第 2 步）的反变换：先在这里把建筑的颜色往反方向推，经过后处理正好回到原色，
// 工笔重彩不被收艳、压墨青、抬黑位罩灰（2026-11-18 用户反馈）。三小步倒着来：纸墨范围 → 冷暖 → 收艳。
// 常量必须与后处理着色器一致；收艳的力度取决于“收之前”的饱和度，用推回去的结果再算一遍
static inline float3 undoPostGrade(float3 y, float g, float night)
{
	const float3 lumW = float3(0.299, 0.587, 0.114);
	float3 inkFloor = float3(0.05, 0.05, 0.06) * (1.0 - 0.6 * night);
	const float3 paperTop = float3(0.97, 0.95, 0.905);
	float3 x = (y - g * inkFloor) / ((1.0 - g) + g * (paperTop - inkFloor));
	float lum = dot(x, lumW);
	float3 tint = mix(float3(0.95, 0.975, 1.02), float3(1.025, 1.0, 0.95), smoothstep(0.12, 0.7, lum));
	x /= mix(float3(1.0), tint, g);
	lum = dot(x, lumW);
	float3 z = x;
	for(int i = 0; i < 2; ++i)
	{
		float hi = max(z.r, max(z.g, z.b));
		float lo = min(z.r, min(z.g, z.b));
		float sat = (hi - lo) / max(hi, 1e-3);
		float keep = 1.0 - g * (0.08 + 0.30 * smoothstep(0.35, 0.9, sat));
		z = lum + (x - lum) / keep;
	}
	return saturate(z);
}

// ---- 修仙建筑融进场景（2026-11-19）----
// 用户嫌建筑像贴上去的：原来建筑整个跳过打光、雾、云，雾和云影飘到建筑轮廓就断，像在场景里挖了个洞；
// 地上的影子朝左（光从右来），朝左的墙却一样亮；墙脚直接切在草上。建筑没有逐像素法线，这里按剪影和地面线估。
// 第一版（只加雾云、侧光 0.16、墙脚 0.16）用户嫌不明显：最大的差别是建筑本身比周围亮一倍（白寺庙台基 0.69，周围 0.34）、
// 树冠饱和度是周围的 2～5 倍、没有深色，第一版没动这些。第二版加“明度匹配”（matchBuildingTone）并加大各项；
// 第二版雾 0.85、云海 0.75、饱和度 0.75，再加上后处理收艳全量作用，神像褪成旧照片、白亭灰成一片，第三版往回收
constant float buildingFog = 0.75;       // 贴地的雾落到建筑上的比例：雾云只是气氛，可交互的建筑不能被埋掉
constant float buildingCloud = 0.65;     // 山腰云海盖到建筑上的比例
constant float buildingSideLight = 0.3;  // 朝太阳一侧提亮、背阴一侧压暗的幅度（太阳在正东、正西时最大）
constant float buildingFootShade = 0.3;  // 墙脚压暗：离地 0 处压这么多，往上 buildingFootHeight 个图块淡出
constant float buildingBounce = 0.35;    // 墙脚映上地面的颜色（草地偏绿、雪地偏白）
constant float buildingFootHeight = 0.6;
// 明度匹配：大地图是淡墨底子，亮部到 0.5～0.7 为止、暗部到 0.15；工笔建筑白墙近 0.9、最暗处 0.25～0.4，
// 整张图里只有它像在棚里打了光。亮部从 buildingKnee 起按 buildingHighlight 压缓，暗部（亮度 0.1 处最多）再压深，
// 饱和度留 buildingSaturation。只改明度结构和艳度，不改色相
constant float buildingKnee = 0.5;
constant float buildingHighlight = 0.45;    // 白墙 0.9 → 0.68
constant float buildingShadowDepth = 0.3;   // 亮度 0.1 处压 30%，0.45 以上不压
constant float buildingSaturation = 0.85; // 后处理的统一收艳也照常作用（vividStrength 为 0），两者相乘
// 建筑起伏图 B 的底数（图块）：导出时（export_mod.BUILDING_HEIGHT_FLOOR）整张加上，资源与宝物的 B 为 0，靠它区分两者。
// 资源与宝物也带修仙记号（保色），但它们是可交互的小物件，照旧不起雾、不加明暗
constant float buildingHeightFloor = 0.1;

// 修仙建筑的程度：有记号、且 B 不为 0（资源与宝物为 0）
static inline float buildingMark(float4 r)
{
	float encB = r.b / max(r.a, 0.001);
	return vividMark(r) * smoothstep(0.008, 0.018, encB);
}

// 建筑离地高度（图块），去掉底数
static inline float buildingHeightOf(float4 r)
{
	return max(r.b / max(r.a, 0.001) * reliefHeightRange - buildingHeightFloor, 0.0) * saturate(r.a * 1.5);
}

// 起伏层上一个取样：x 是修仙建筑的程度，y 是这个取样能不能用。
// 英雄、原版物件、道路在起伏层上是清零剪影（RGB 为 0、A 有值），资源与宝物带记号但 B 为 0，都看不出后面是不是建筑，不能用：
// 当成“建筑到此为止”的话，英雄走过时建筑上会跟着一圈明暗
static inline float2 buildingProbe(float4 r)
{
	float3 enc = r.rgb / max(r.a, 0.001);
	float cutout = smoothstep(0.1, 0.3, r.a) * (1.0 - smoothstep(0.004, 0.012, enc.r + enc.g));
	float building = buildingMark(r);
	float pickup = vividMark(r) - building;
	return float2(building, 1.0 - saturate(cutout + pickup));
}

// 这一点在建筑的哪一侧：左右各取三处看还在不在建筑上。左边是建筑、右边空着为正（靠右边），反过来为负，
// 离两边都超过 2.2 个图块为 0。三处由近到远权重递减，明暗从边上往里渐渐淡，不是一道硬边
static inline float buildingSide(float2 rel, float2 slots, float2 slotOrigin, texture2d<float> reliefTex, sampler reliefSmp)
{
	const float reach[3] = {0.5, 1.2, 2.2};
	const float weight[3] = {0.45, 0.35, 0.2};
	float side = 0.0;
	for(int i = 0; i < 3; ++i)
	{
		float2 left = buildingProbe(reliefTex.sample(reliefSmp, windowToUV(rel - float2(reach[i], 0.0), slots, slotOrigin)));
		float2 right = buildingProbe(reliefTex.sample(reliefSmp, windowToUV(rel + float2(reach[i], 0.0), slots, slotOrigin)));
		side += weight[i] * (left.x - right.x) * left.y * right.y;
	}
	return side;
}

// 明度匹配（见 buildingKnee 等）。c 是不预乘的颜色
static inline float3 matchBuildingTone(float3 c)
{
	const float3 lumW = float3(0.299, 0.587, 0.114);
	float lum = dot(c, lumW);
	float target = lum <= buildingKnee ? lum : buildingKnee + (lum - buildingKnee) * buildingHighlight;
	target *= 1.0 - buildingShadowDepth * (1.0 - smoothstep(0.1, 0.45, lum));
	float3 scaled = c * (target / max(lum, 1e-3));
	return max(mix(float3(target), scaled, buildingSaturation), float3(0.0));
}

// 建筑表面的明暗：朝太阳一侧亮、背阴一侧暗（随时段换边，正午太阳在南、左右差不多），墙脚压暗并映上地面颜色。
// rgb 是预乘 alpha 的建筑颜色，ground 是这一点下面的地面颜色，height 是离地高度（图块）
static inline float3 shadeBuilding(float3 rgb, float side, float height, float3 ground, float sunlit, constant Params & P)
{
	float sideLight = 1.0 + buildingSideLight * side * P.sun.x * P.sun.w * sunlit;
	float foot = 1.0 - smoothstep(0.0, buildingFootHeight, height);
	float groundLum = max(dot(ground, float3(0.299, 0.587, 0.114)), 0.05);
	float3 bounce = clamp(ground / groundLum, float3(0.75), float3(1.25));
	float3 footTint = mix(float3(1.0), bounce, buildingBounce * foot) * (1.0 - buildingFootShade * foot);
	return rgb * sideLight * footTint;
}

// 物件按法线重新打光（颜色为预乘 alpha）。原画本身就画了明暗，所以按“相对平地”的比例调：
// 朝太阳的一面提亮、偏暖，背阴的一面压暗、偏冷，平地和没有起伏图的物件不变；
// 轮廓上朝着太阳的一圈再加一道边缘光（低太阳时更明显，像《八方旅人》的逆光描边）
// sunlit：这一点直射阳光还剩多少（大块云影里只剩散射的天光，朝向明暗就淡了）
static inline float3 relight(float3 rgb, float alpha, Relief r, float sunlit, constant Params & P)
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
	float strength = P.sun.w * r.known * sunlit;
	rgb *= mix(float3(1.0), k * tint, strength);

	float2 facing = r.normal.xy / max(length(r.normal.xy), 1e-4);
	float2 toward = L.xy / max(length(L.xy), 1e-4);
	float rim = pow(saturate(1.0 - r.normal.z), 2.0) * saturate(dot(facing, toward));
	return rgb + P.sunColor.rgb * (rim * P.sunColor.w * strength * alpha);
}

// ---- 地表（2026-11-17）：打破地形贴图的重复 ----
// 地形图块要能随机拼接，每块的大块明暗都压平了，满屏是同一种细纹理，像墙纸一样重复（用户反馈视觉疲劳）。
// 这里在合成时按地图坐标加上连续的变化，跨图块不断开；并抹开图块接缝。只用于陆地，水面由水面着色器管。

// 图块接缝：每块的笔触在边上切断、深浅略有出入，铺开后能看出一张方格。贴着图块边约 9% 图块宽的一条，
// 与边对面镜像位置的颜色混合，越靠边混得越多、边上正好各半，两侧在接缝处连成一片。边对面是水的不混
static inline float3 blendTileSeams(float3 c, float2 rel, float2 slots, float2 slotOrigin,
	texture2d<float> groundTex, sampler groundSmp, texture2d<float> maskTex, sampler maskSmp)
{
	const float seam = 0.09;
	float2 cell = floor(rel);
	float2 inTile = rel - cell;
	float2 toEdge = min(inTile, 1.0 - inTile);
	float3 result = c;
	if(toEdge.x < seam)
	{
		float edge = inTile.x < 0.5 ? cell.x : cell.x + 1.0;
		float2 mirrorUV = windowToUV(float2(2.0 * edge - rel.x, rel.y), slots, slotOrigin);
		float w = 0.5 * smoothstep(seam, 0.0, toEdge.x) * (1.0 - maskTex.sample(maskSmp, mirrorUV).g);
		result = mix(result, groundTex.sample(groundSmp, mirrorUV).rgb, w);
	}
	if(toEdge.y < seam)
	{
		float edge = inTile.y < 0.5 ? cell.y : cell.y + 1.0;
		float2 mirrorUV = windowToUV(float2(rel.x, 2.0 * edge - rel.y), slots, slotOrigin);
		float w = 0.5 * smoothstep(seam, 0.0, toEdge.y) * (1.0 - maskTex.sample(maskSmp, mirrorUV).g);
		result = mix(result, groundTex.sample(groundSmp, mirrorUV).rgb, w);
	}
	return result;
}

// 大尺度变化：三层噪声给出十来个图块的大片明暗、三四个图块的斑块、一个多图块的细碎起伏，
// 另一层噪声分出干湿冷暖（干处偏暖偏浅、略褪色，湿处偏冷偏深）。amount 按地形取自雾云场的 A（岩石少，水面为 0）。
// 雪地另加顺风方向拉长的雪垄阴影（偏蓝）；岩浆地上的余烬裂缝成片地明暗起伏、慢慢呼吸
static inline float3 groundVariation(float3 c, float2 world, float t, float amount, texture2d<float> noiseTex, sampler noiseSmp)
{
	float big = noiseTex.sample(noiseSmp, world * 0.011 + float2(0.23, 0.71)).r;
	float mid = noiseTex.sample(noiseSmp, world * 0.037 + float2(0.61, 0.13)).g;
	float fine = noiseTex.sample(noiseSmp, world * 0.12 + float2(0.37, 0.89)).r;
	float light = (big - 0.5) * 0.24 + (mid - 0.5) * 0.18 + (fine - 0.5) * 0.1;
	float dry = smoothstep(0.25, 0.75, noiseTex.sample(noiseSmp, world * 0.019 + float2(0.83, 0.41)).g) * 2.0 - 1.0;

	// 雪：亮、不鲜艳的地面。雪上不加冷暖（偏黄像脏雪），只加雪垄
	float sat = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
	float snow = smoothstep(0.68, 0.82, dot(c, float3(0.3, 0.59, 0.11))) * (1.0 - smoothstep(0.06, 0.16, sat));

	// 干湿：干处略亮、偏暖、褪色一点；湿处略暗、颜色更浓。不改色相——偏蓝偏紫在草地、碎石上都假（2026-11-17 离线对比）
	float3 v = c * (1.0 + light * amount);
	float lum = dot(v, float3(0.3, 0.59, 0.11));
	float wetness = saturate(-dry) * amount;
	float dryness = saturate(dry) * amount * (1.0 - snow);
	v = lum + (v - lum) * (1.0 + 0.3 * wetness - 0.2 * dryness);
	v *= (1.0 - 0.06 * wetness) * float3(1.0 + 0.1 * dryness, 1.0 + 0.06 * dryness, 1.0 - 0.05 * dryness);

	if(snow > 0.01)
	{
		const float2 windDir = float2(0.94, 0.34);
		float2 driftCoord = float2(dot(world, windDir) * 0.32, dot(world, float2(-windDir.y, windDir.x)) * 0.09);
		float drift = smoothstep(0.5, 0.85, noiseTex.sample(noiseSmp, driftCoord + float2(0.11, 0.57)).b);
		v = mix(v, v * float3(0.87, 0.91, 0.99), drift * 0.6 * snow * amount);
	}

	// 岩浆：发红的裂缝。热的一片裂缝亮起来、往橙黄走，冷的一片暗成深红，十几秒一呼吸
	float ember = saturate((c.r - c.b) * 3.0 - 0.6) * smoothstep(0.3, 0.55, c.r);
	if(ember > 0.01)
	{
		float heat = noiseTex.sample(noiseSmp, world * 0.05 + float2(t * 0.011, t * 0.007)).r;
		heat = smoothstep(0.25, 0.8, heat) * (0.8 + 0.2 * sin(t * 0.45 + big * 6.283));
		float3 glowing = v * (0.45 + 1.25 * heat) + float3(0.22, 0.08, 0.0) * heat * heat;
		v = mix(v, glowing, ember * amount);
	}
	return v;
}

// ---- 地表材质（2026-11-17）：按世界坐标铺的真实地表 ----
// 地形图块只告诉着色器“哪里是什么地形”（地形分布层：本地形、泥地、沙地的比重），颜色改从材质图集按地图坐标取，
// 跨图块连续、不重复。图集 8 行 × 4 列：行是材质组（与 TerrainId 0~7 一致：泥地、沙地、草地、雪地、沼泽、碎石、地下、岩浆），
// 列是该组的 4 种子材质（地形模组 tools/gen_v4.sh 生成，tools/materials.py 做成无缝并推出高度），RGB 颜色、A 高度。
// 一种地形 = 底子材质 + 成片的大斑 + 中等斑块 + 零星点缀，子材质交界按高度参差（石子、草尖先从土里露出来）；
// 再加大尺度的缓坡起伏（雪地加风吹出的雪垄），按游戏里的太阳打光，地表就有了真实的层次和光影。

// 每组的子材质覆盖率：x 大斑（第 1 列）、y 中斑（第 2 列）、z 点缀（第 3 列），w 是材质高度换成的起伏幅度（图块）
constant float4 setCoverage[10] = {
	float4(0.32, 0.28, 0.18, 0.10), // 泥地：黄土底，龟裂黏土、深色腐殖土、碎石浮土
	float4(0.28, 0.18, 0.14, 0.06), // 沙地：风纹细沙，粗砂贝壳、湿沙、沙丘草
	float4(0.34, 0.22, 0.20, 0.08), // 草地：茂草，干草三叶草、踩秃的土、苔藓蕨类
	float4(0.14, 0.24, 0.07, 0.08), // 雪地：粉雪，风壳雪脊（只零星几片，太多像揉皱的布）、薄雪露地、踩过的雪（灰脚印多了像脏水坑）
	float4(0.38, 0.30, 0.24, 0.08), // 沼泽：烂泥水洼，苔草墩、浅水芦苇、泥炭
	float4(0.30, 0.30, 0.16, 0.14), // 碎石：碎石坡，石板地衣、石土枯草、大石块
	float4(0.30, 0.24, 0.16, 0.12), // 地下：湿石地，碎石蘑菇、乱石、矿晶
	float4(0.34, 0.26, 0.16, 0.12), // 岩浆：玄武岩红裂缝，火山灰、绳状熔岩、硫磺焦土
	// 特殊地面（2026-11-19，tools/gen_v5.sh）：第一版 (0.32, 0.14, 0.16) 实机里几乎只剩灰烬底子，白骨残剑看不到，调高到零星可见
	float4(0.38, 0.22, 0.22, 0.10), // 阴煞地（被诅咒的大地）：灰烬死土，焦土枯草、白骨残剑、碎石纸钱
	float4(0.30, 0.20, 0.24, 0.06)  // 灵气草甸（魔法平原）：玉青灵草，零星灵花、灵脉微光的苔地、灵草露珠
};
// 靠近山脚时加多的子材质（1~3 列）：碎石、乱石这类
constant int mountainSub[10] = {3, 1, 3, 2, 3, 3, 2, 1, 3, 1};

// 四周图块的特殊地面种类（地形场 R 的高四位）按地图坐标双线性插值（2026-11-19）：x 阴煞地、y 灵气草甸，
// 在其中为 1，出了边界约半个图块淡到 0。阴气、灵气按它的浓淡画，不按图块一刀切
static inline float2 specialGroundField(float2 world, constant Params & P, texture2d<float> terrainTex)
{
	float2 p = world - 0.5;
	float2 base = floor(p);
	float2 f = p - base;
	float2 result = float2(0.0);
	for(int k = 0; k < 4; ++k)
	{
		float2 offset = float2(float(k & 1), float(k >> 1));
		uint2 tile = uint2(clamp(base + offset, float2(0.0), P.terrain.zw - 1.0));
		int kind = int(terrainTex.read(tile).r * 255.0 + 0.5) >> 4;
		float w = (offset.x > 0.5 ? f.x : 1.0 - f.x) * (offset.y > 0.5 ? f.y : 1.0 - f.y);
		result += w * float2(kind == 1 ? 1.0 : 0.0, kind == 2 ? 1.0 : 0.0);
	}
	return result;
}

static inline float2 hash22(float2 p)
{
	float3 p3 = fract(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.xx + p3.yz) * p3.zy);
}

// 图集一格里的一点：local 以格为单位，可以是任意实数（取小数部分环绕）。四周的环绕边保证线性取样跨过格边也连续
static inline float4 atlasTexel(texture2d<float> atlasTex, sampler atlasSmp, constant Params & P, int cellIndex, float2 local)
{
	float cell = P.atlas.z;
	float gutter = P.atlas.w;
	float2 origin = float2(float(cellIndex % 4), float(cellIndex / 4)) * cell + gutter;
	return atlasTex.sample(atlasSmp, (origin + fract(local) * (cell - 2.0 * gutter)) / P.atlas.xy, level(0.0));
}

// 六边形随机铺贴（Heitz & Neyret 2018 的思路）：在三角网格的三个顶点上各取一个随机错位的样本，按到顶点的距离混合，
// 再按权重还原方差（不然混合处发灰发糊）。同一张材质铺满地图也看不出周期；不旋转，免得画上的光影朝向乱掉
static inline float4 hexMaterial(texture2d<float> atlasTex, sampler atlasSmp, constant Params & P, int cellIndex, float2 uv)
{
	float2 g = uv * 1.7;
	float2 sk = float2(g.x - g.y * 0.57735, g.y * 1.1547);
	float2 b = floor(sk);
	float2 f = sk - b;
	bool up = f.x + f.y > 1.0;
	float2 v0 = up ? b + 1.0 : b;
	float3 w = up ? float3(f.x + f.y - 1.0, 1.0 - f.y, 1.0 - f.x) : float3(1.0 - f.x - f.y, f.x, f.y);
	w = pow(max(w, 0.0), 4.0);
	float salt = float(cellIndex) * 7.13;
	float4 a0 = atlasTexel(atlasTex, atlasSmp, P, cellIndex, uv + hash22(v0 + salt));
	float4 a1 = atlasTexel(atlasTex, atlasSmp, P, cellIndex, uv + hash22(b + float2(1.0, 0.0) + salt));
	float4 a2 = atlasTexel(atlasTex, atlasSmp, P, cellIndex, uv + hash22(b + float2(0.0, 1.0) + salt));
	float wsum = w.x + w.y + w.z;
	float4 blended = (a0 * w.x + a1 * w.y + a2 * w.z) / wsum;
	float4 mean = P.cellMean[cellIndex];
	return mean + (blended - mean) * (wsum / sqrt(dot(w, w)));
}

// 按高度混合：子材质的覆盖度 cover 越高越能盖过底子；交界处底子里高起的颗粒先露出来，交界参差，不是一圈柔边
static inline float heightBlend(float baseHeight, float layerHeight, float cover)
{
	return smoothstep(-0.32, 0.32, (layerHeight + cover * 1.6 - 0.8) - baseHeight);
}

// 一组地表材质在地图坐标 world 处的颜色（rgb）与高度（a，0~1）。mountain：离山多近，shore：离水多近（都 0~1）
static inline float4 terrainMaterial(int set, float2 world, float mountain, float shore, constant Params & P,
	texture2d<float> atlasTex, sampler atlasSmp, texture2d<float> noiseTex, sampler noiseSmp)
{
	float2 uv = world / P.terrain.y;
	int base = set * 4;
	float4 m = hexMaterial(atlasTex, atlasSmp, P, base, uv);

	// 子材质分布：大斑约 6 个图块（带扭曲，边缘不圆）、中斑约 2.5 个图块、点缀不到一个图块
	float2 salt = float2(float(set) * 0.173, float(set) * 0.291);
	float2 warp = (noiseTex.sample(noiseSmp, world * 0.05 + salt + float2(0.5, 0.2)).rg - 0.5) * 1.2;
	float n1 = noiseTex.sample(noiseSmp, (world + warp) * 0.021 + salt).r;
	float n2 = noiseTex.sample(noiseSmp, (world + warp * 0.5) * 0.05 + salt + float2(0.41, 0.07)).g;
	float n3 = noiseTex.sample(noiseSmp, world * 0.07 + salt + float2(0.19, 0.63)).b;

	float3 cover = setCoverage[set].xyz;
	int bias = mountainSub[set] - 1;
	cover[bias] += mountain * 0.35;
	if(set == 1)
		cover.y += shore * 0.6; // 沙地靠水处多湿沙

	// 斑块边缘留得宽一些：覆盖度从 0 到 1 有一大段过渡，交界由高度参差决定，不是一刀切
	float3 masks = float3(smoothstep(0.84 - cover.x * 1.2, 1.12 - cover.x * 1.2, n1),
		smoothstep(0.84 - cover.y * 1.2, 1.12 - cover.y * 1.2, n2),
		smoothstep(0.88 - cover.z * 1.1, 1.08 - cover.z * 1.1, n3));

	for(int k = 0; k < 3; ++k)
	{
		if(masks[k] < 0.01)
			continue;
		float4 layer = hexMaterial(atlasTex, atlasSmp, P, base + k + 1, uv * (1.0 + 0.11 * float(k)) + float2(0.37, 0.61) * float(k + 1));
		m = mix(m, layer, heightBlend(m.a, layer.a, masks[k]));
	}
	return m;
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

// ---- 天气（2026-11-18）：大块云影、阵雨、热浪、瘴气 ----

// 大块云影的形状：一片片十几个图块大的云影顺风缓缓飘过全图（约一分钟飘过一屏），边缘被扭得不规则。
// 云本身在镜头之上看不见，只看见影子扫过地面。MapParticles::cloudShape 在 CPU 上按同一公式算阵雨落在哪，两边必须一致
static inline float bigCloudShape(float2 world, float t, texture2d<float> noiseTex, sampler noiseSmp)
{
	const float2 cloudWind = float2(0.94, 0.34);
	float2 p = world - cloudWind * (t * 0.3);
	float warp = noiseTex.sample(noiseSmp, p * 0.02 + float2(0.31, 0.77)).r;
	return noiseTex.sample(noiseSmp, p * 0.0085 + float2(warp * 0.06, 0.13)).g * 0.75
		+ noiseTex.sample(noiseSmp, p * 0.031 + float2(0.57, 0.21)).r * 0.25;
}

// 阵雨：只有一部分云下雨（另一层更慢、更大的噪声挑出来），雨下在云影最浓的那一块。与 MapParticles::showerAt 一致
static inline float showerAt(float2 world, float t, float shape, texture2d<float> noiseTex, sampler noiseSmp)
{
	const float2 cloudWind = float2(0.94, 0.34);
	float2 p = world - cloudWind * (t * 0.3);
	float rainy = noiseTex.sample(noiseSmp, p * 0.0045 + float2(0.71, 0.37)).r;
	return smoothstep(0.56, 0.66, shape) * smoothstep(0.46, 0.58, rainy);
}

// 雨点打在水面上：两层错开的网格，每格按自己的周期落一滴，打出一圈向外扩散、渐淡的涟漪，落点先溅起一个亮点。
// 落点留在格子中间，涟漪扩到最大也不出格，所以每层只看本格。rain 越大，每个周期落雨的格子越多
// pixel：一个缓存像素是多少图块，涟漪的线宽至少一个多像素
static inline float rainRipples(float2 world, float t, float rain, float pixel)
{
	const float cell = 0.5; // 图块
	float result = 0.0;
	for(int layer = 0; layer < 2; ++layer)
	{
		float2 g = world / cell + float2(0.5, 0.37) * float(layer);
		float2 id = floor(g);
		float2 f = g - id;
		float2 h = hash22(id + float(layer) * 17.0);
		float period = 0.8 + 0.7 * h.x;
		float clock = t / period + h.y;
		float cycle = floor(clock);
		float phase = clock - cycle;
		float2 drop = hash22(id * 1.7 + float2(cycle * 0.37, float(layer) * 3.1));
		if(drop.x > rain)
			continue;
		float2 center = 0.3 + 0.4 * hash22(id * 2.3 + cycle * 0.71);
		float d = length(f - center);
		float radius = 0.28 * sqrt(phase);
		float width = max(0.045, 1.6 * pixel / cell);
		float ring = (1.0 - smoothstep(0.0, width, abs(d - radius))) * (1.0 - phase) * (1.0 - phase);
		float splash = (1.0 - smoothstep(0.0, 0.07, d)) * (1.0 - smoothstep(0.0, 0.15, phase));
		result += ring + splash * 0.8;
	}
	return result;
}

// ---- 蹄印（2026-11-18）：英雄在沙地、雪地上走过，留下一对对蹄印，几十秒里慢慢淡去 ----
// 每对蹄印左右各一个，前后错开半步。蹄印是浅浅的凹坑：朝着太阳的那面坑壁背光、暗，对面的坑壁迎光、略亮，坑沿有一圈被踩出来的土（雪）；
// 整对蹄印底下还有一片淡淡压暗的“踩乱的地”，一路连成一条看得出来的路（单靠几像素大的蹄印，在有纹理的地面上看不出来，2026-11-18 实机）。
// 沙地上是偏暖的深色，雪地上是偏蓝的影子。只在蹄印外接矩形里的像素才找，每对蹄印先按距离跳过
static inline float3 footprints(float3 color, float2 world, constant Params & P)
{
	if(any(world < P.printBox.xy) || any(world > P.printBox.zw))
		return color;

	// 太阳在地面上的方向（地图坐标 y 朝南，屏幕 y 朝上）
	float2 toSun = float2(P.sun.x, -P.sun.y);
	toSun = length(toSun) > 1e-3 ? normalize(toSun) : float2(0.0, -1.0);

	float sandDent = 0.0;
	float snowDent = 0.0;
	float sandChurn = 0.0;
	float snowChurn = 0.0;
	float rim = 0.0;
	float lit = 0.0;
	int count = int(P.lightInfo.y);
	for(int i = 0; i < count; ++i)
	{
		float4 fp = P.prints[i];
		float2 d = world - fp.xy;
		if(abs(d.x) > 0.32 || abs(d.y) > 0.32)
			continue;
		float ground = floor(fp.w * 0.5);
		float alpha = fp.w - 2.0 * ground;
		float2 dir = float2(cos(fp.z), sin(fp.z));
		float2 side = float2(-dir.y, dir.x);

		// 踩乱的地：顺着走向拉长的一片，相邻几对连起来
		float churn = (1.0 - smoothstep(0.35, 1.0, length(float2(dot(d, dir) / 0.30, dot(d, side) / 0.22)))) * alpha;
		if(ground < 0.5)
			sandChurn = max(sandChurn, churn);
		else
			snowChurn = max(snowChurn, churn);

		for(int k = 0; k < 2; ++k)
		{
			float s = k == 0 ? -1.0 : 1.0;
			float2 c = d - dir * (0.08 * s) - side * (0.12 * s);
			// 局部坐标：沿走向半长 0.09、横向半宽 0.065 个图块（比真实的马蹄夸张，原版一个图块才 32 像素）
			float2 q = float2(dot(c, dir) / 0.09, dot(c, side) / 0.065);
			float e = length(q);
			if(e > 1.4)
				continue;
			float dent = (1.0 - smoothstep(0.7, 1.0, e)) * alpha;
			float2 wall = e > 1e-3 ? -(dir * q.x + side * q.y) / e : float2(0.0);
			// 坑壁朝着太阳的程度：坑里离太阳近的一侧坑壁背光
			float facing = dot(wall, toSun) * smoothstep(0.3, 0.9, e);
			if(ground < 0.5)
				sandDent = max(sandDent, dent * (0.8 - 0.2 * facing));
			else
				snowDent = max(snowDent, dent * (0.8 - 0.2 * facing));
			lit = max(lit, dent * saturate(facing) * 0.5);
			rim = max(rim, smoothstep(0.9, 1.05, e) * (1.0 - smoothstep(1.05, 1.35, e)) * alpha);
		}
	}
	color *= 1.0 - 0.10 * sandChurn;
	color = mix(color, color * float3(0.86, 0.90, 0.98), snowChurn * 0.6);
	color = mix(color, color * float3(0.56, 0.48, 0.40), sandDent * 0.85);
	color = mix(color, color * float3(0.62, 0.72, 0.92), snowDent * 0.85);
	color += (rim * 0.06 + lit * 0.05) * float3(1.0, 0.98, 0.94);
	return color;
}

// ---- 局部光（2026-11-18）：城镇灯笼、营火、法光、熔岩、英雄火把 ----
// ground：这一点在地面上的位置（物件表面已按离地高度挪回它脚下），h：离地高度，N：屏幕坐标里的法线。
// 光按三维距离衰减，按朝向加一点包裹光：朝着光的一面亮，背面也不至于全黑。halo 累加光源周围空气里的辉光。
// 只算照得到这一点的灯：CPU 把可见窗口分成 16 × 16 格，按每盏灯照到的范围记下每格有哪些灯（缩到全图时
// 每个像素都把 32 盏灯挨个看一遍，GPU 吃紧，2026-11-18 用户反馈缩放掉帧）
constant int lightGridSize = 16;

static inline float3 localLights(float2 ground, float2 screen, float h, float3 N, constant Params & P, thread float3 & halo)
{
	float3 sum = float3(0.0);
	int2 cell = clamp(int2(floor((screen - P.lightGrid.xy) / P.lightGrid.zw)), int2(0), int2(lightGridSize - 1));
	int index = cell.y * lightGridSize + cell.x;
	uint mask = P.lightCells[index >> 2][index & 3];
	while(mask != 0u)
	{
		int i = int(ctz(mask));
		mask &= mask - 1u;
		float4 lp = P.lightPos[i];
		float2 d = lp.xy - ground;
		float r = lp.z;
		if(abs(d.x) > r || abs(d.y) > r)
			continue;
		float3 L = float3(d.x, d.y, lp.w - h); // 地面坐标：x 东、y 南（屏幕往下）、z 上
		float dist = length(L);
		float fall = saturate(1.0 - (dist * dist) / (r * r));
		fall *= fall;
		float3 Ls = L.x * float3(1.0, 0.0, 0.0) - L.y * float3(0.0, 0.917, -0.399) + L.z * groundNormal;
		float wrap = saturate((dot(N, Ls / max(dist, 1e-3)) + 0.45) / 1.45);
		sum += P.lightColor[i].rgb * (fall * wrap);

		// 辉光：光源在屏幕上的位置是地面位置往上挪离地高度
		float2 g = screen - float2(lp.x, lp.y - lp.w * 0.8);
		halo += P.lightColor[i].rgb * (P.lightColor[i].w * exp(-dot(g, g) / (0.12 * r * r)));
	}
	return sum;
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
	texture2d<float> lightTex [[texture(11)]], sampler lightSmp [[sampler(11)]],
	texture2d<float> splatTex [[texture(12)]], sampler splatSmp [[sampler(12)]],
	texture2d<float> atlasTex [[texture(13)]], sampler atlasSmp [[sampler(13)]],
	texture2d<float> terrainTex [[texture(14)]], sampler terrainSmp [[sampler(14)]])
{
	float2 uv = in.uv;

	float2 slots = P.cache.zw;
	float t = P.misc.x;

	// 窗口内的位置与地图坐标（都以图块为单位）：噪声跟着地图走，滚动画面时水纹、风都不会滑动
	float2 slotOrigin = P.world.zw;
	float2 rel = windowPosition(uv, slots, slotOrigin);
	float2 world = P.world.xy + rel;

	// 世界之外是装裱的绫和隔水边框（MapRendererBorder），不带天气和光照特效，在世界边界上齐齐收住（2026-11-20）。
	// 各种场按地图坐标取样、地图外夹到边缘的值：边上是岩浆、沼泽时，熔岩红光、热浪、瘴气会铺满整片绫
	float2 beyondWorld = max(-world, world - max(P.misc.zw, float2(1.0)));
	float inWorld = 1.0 - step(0.0, max(beyondWorld.x, beyondWorld.y));

	// 地形场的天气通道（按地图坐标线性取样）：G 热浪、B 瘴气、A 熔岩红光
	float4 weatherField = terrainTex.sample(terrainSmp, world / max(P.terrain.zw, float2(1.0))) * inWorld;

	// 大块云影与阵雨（2026-11-18）。云影里直射阳光只剩一小半：地面、物件压暗偏冷，长投影、朝向明暗、水面波光都淡下去
	float cloudShade = 0.0;
	float rain = 0.0;
	if(P.weather.x * P.misc.y > 0.001)
	{
		float shape = bigCloudShape(world, t, noiseTex, noiseSmp);
		// 约三成地面在云影里；边缘收得比较利落，一眼看得出是云的影子，不是一片模糊的暗斑
		cloudShade = smoothstep(0.47, 0.55, shape) * P.weather.x * P.misc.y;
		rain = showerAt(world, t, shape, noiseTex, noiseSmp) * P.weather.y * P.misc.y;
		cloudShade *= inWorld;
		rain *= inWorld;
	}
	float sunlit = 1.0 - 0.7 * cloudShade;

	// 自发光（2026-11-18）：熔岩裂缝、水面波光、灯火的辉光有多亮，写进输出的 alpha，后处理据此做辉光（见 bloomShaderSource）
	float emission = 0.0;

	// 热浪（2026-11-18）：沙地正午、岩浆上不论早晚，地面像隔着一层晃动的热空气。只扭地面层（地表、水），
	// 物件不扭：站在沙漠里的英雄、建筑跟着晃会像画面坏了。地面这一段用扭过的坐标，算完再换回来
	float2 uvTrue = uv;
	float2 relTrue = rel;
	float2 worldTrue = world;
	float haze = weatherField.g * mix(P.night.w, 1.0, weatherField.a) * P.misc.y;
	if(haze > 0.01)
	{
		// 细碎的热纹往上飘（屏幕往上约每秒一个图块），两层不同快慢叠起来不成规则的波
		float2 h1 = noiseTex.sample(noiseSmp, world * float2(0.07, 0.05) + float2(0.0, t * 0.08)).ba;
		float2 h2 = noiseTex.sample(noiseSmp, world * float2(0.11, 0.08) + float2(0.43, t * 0.13)).ab;
		float2 shimmer = (h1 - 0.5) * 0.6 + (h2 - 0.5) * 0.4;
		rel = rel + shimmer * float2(0.035, 0.05) * haze;
		uv = windowToUV(rel, slots, slotOrigin);
		world = P.world.xy + rel;
	}
	float3 color = groundTex.sample(groundSmp, uv).rgb;

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

	// 地表：有地形分布的地方按世界坐标铺材质、按起伏打光；其余（岩石、道路、河流、湖、没有材质的地形）抹开图块接缝，
	// 再加按地图坐标连续变化的明暗、干湿。地形的变化幅度在雾云场的 A 通道（与离岸距离图同一张，按地图坐标取样）
	// 世界之外的绫不是陆地：不然“大尺度明暗干湿”（groundVariation）会按夹到地图边缘的场在绫上铺一片片云影似的斑（2026-11-20 用户反馈）
	float land = (1.0 - saturate(maskRGB.g)) * P.misc.y * inWorld;
	float4 fieldHere = depthTex.sample(depthSmp, world / max(P.misc.zw, float2(1.0)));
	float4 splatFull = splatTex.sample(splatSmp, uv);
	float3 splat = splatFull.rgb;

	// 特殊地面（2026-11-19）：分布层的 1 − A 是它盖住这里的程度（MapMaskBlend::SPECIAL_GROUND，那里 RGB 也已按它压低），
	// 是哪一种看地形场 R 的高四位。种类按四周图块双线性插值（specialGroundField），阴煞地、灵气草甸紧挨着时
	// 两种材质在交界处过渡约一个图块，不按图块切出直边。图集里有那一组材质（阴煞地组 8、灵气草甸组 9）才铺，
	// 否则那里照旧显示它的贴图
	uint2 tile = uint2(clamp(floor(world), float2(0.0), P.terrain.zw - 1.0));
	int terrainCode = int(terrainTex.read(tile).r * 255.0 + 0.5);
	int atlasSets = int(P.atlas.y / max(P.atlas.z, 1.0) + 0.5);
	float2 specialKinds = specialGroundField(world, P, terrainTex);
	float2 specialSplit = float2(atlasSets > 8 ? specialKinds.x : 0.0, atlasSets > 9 ? specialKinds.y : 0.0);
	specialSplit /= max(specialKinds.x + specialKinds.y, 1e-3);
	float2 specialWeights = saturate(1.0 - splatFull.a) * specialSplit; // x 阴煞地、y 灵气草甸
	float specialCover = specialWeights.x + specialWeights.y;
	float materialShare = saturate(splat.r + splat.g + splat.b + specialCover) * P.terrain.x * land;

	// 大尺度缓坡：两层低频噪声，起伏约一个图块（与材质无关，所有像素都算，导数才连续）
	float macro = noiseTex.sample(noiseSmp, world * 0.03 + float2(0.71, 0.29)).r * 0.75
		+ noiseTex.sample(noiseSmp, world * 0.085 + float2(0.13, 0.83)).g * 0.25;
	float terrainHeight = macro * 0.9 * materialShare;
	float3 terrainColor = color;
	float snowShare = 0.0;

	if(materialShare > 0.003)
	{
		int own = terrainCode & 15;

		// 离山多近（雾云场的云量）、离水多近（1/16 水面遮罩在半个图块外的四点）
		float mountain = smoothstep(0.15, 0.7, fieldHere.b);
		float near = maskLow.sample(maskLowSmp, windowToUV(rel + float2(0.5, 0.0), slots, slotOrigin)).g
			+ maskLow.sample(maskLowSmp, windowToUV(rel - float2(0.5, 0.0), slots, slotOrigin)).g
			+ maskLow.sample(maskLowSmp, windowToUV(rel + float2(0.0, 0.5), slots, slotOrigin)).g
			+ maskLow.sample(maskLowSmp, windowToUV(rel - float2(0.0, 0.5), slots, slotOrigin)).g;
		float shore = smoothstep(0.05, 0.6, near * 0.25);

		// 本地形、泥地、沙地、两种特殊地面按分布图的比重混合；比重很小的组不算。水面、岩石、模组新增的地形没有材质
		float weights[5] = {splat.r, splat.g, splat.b, specialWeights.x, specialWeights.y};
		int sets[5] = {own <= 7 ? own : -1, 0, 1, 8, 9};
		float4 sum = float4(0.0);
		float total = 0.0;
		float detail = 0.0;
		for(int k = 0; k < 5; ++k)
		{
			if(weights[k] < 0.02 || sets[k] < 0)
				continue;
			float4 m = terrainMaterial(sets[k], world, mountain, shore, P, atlasTex, atlasSmp, noiseTex, noiseSmp);
			sum += m * weights[k];
			total += weights[k];
			detail += (m.a - 0.5) * setCoverage[sets[k]].w * weights[k];
			if(sets[k] == 3)
				snowShare += weights[k];
		}
		if(total > 0.001)
		{
			terrainColor = sum.rgb / total;
			terrainHeight += detail / total * materialShare;
			snowShare /= total;
			// 水边湿：颜色略深、更浓
			float wetLum = dot(terrainColor, float3(0.3, 0.59, 0.11));
			terrainColor = mix(terrainColor, (wetLum + (terrainColor - wetLum) * 1.25) * 0.8, shore * 0.55);
		}
		else
			materialShare = 0.0;

		// 雪垄：风从西偏北吹来，雪垄横在风向上，按噪声断续、弯曲
		if(snowShare > 0.01)
		{
			const float2 windDir = float2(0.94, 0.34);
			float across = dot(world, float2(-windDir.y, windDir.x));
			float along = dot(world, windDir);
			float bend = noiseTex.sample(noiseSmp, world * 0.06 + float2(0.3, 0.9)).r;
			float drift = sin(across * 3.6 + bend * 7.0 + 0.6 * sin(along * 0.45)) * 0.5 + 0.5;
			drift = drift * drift * smoothstep(0.25, 0.75, noiseTex.sample(noiseSmp, world * 0.025 + float2(0.6, 0.1)).g);
			terrainHeight += drift * 0.09 * snowShare * materialShare;
		}
	}

	// 起伏打光：屏幕空间的高度导数（每 2×2 像素一组）换成地面坡度，再换到屏幕坐标里算朝向太阳的程度。导数要在分支外取
	float2 dWorld = float2(length(dfdx(world)), length(dfdy(world)));
	float2 slope = float2(dfdx(terrainHeight), dfdy(terrainHeight)) / max(dWorld, float2(1e-5));
	float splatEdgeWidth = fwidth(splat.r);
	float sandEdgeWidth = fwidth(splat.g - splat.b);
	float specialEdgeWidth = fwidth(specialCover);
	float3 groundN = groundNormal; // 地面的法线（屏幕坐标），局部光也按它照

	if(materialShare > 0.003)
	{
		// 地面坐标（x 右、y 北、z 上）里的法线 → 屏幕坐标（与 groundNormal 同一套俯视角）
		float3 n = normalize(float3(-slope.x, slope.y, 1.0));
		float3 nScreen = n.x * float3(1.0, 0.0, 0.0) + n.y * float3(0.0, 0.917, -0.399) + n.z * groundNormal;
		groundN = normalize(mix(groundNormal, nScreen, materialShare));
		const float ambient = 0.55;
		float flat = ambient + (1.0 - ambient) * max(dot(groundNormal, P.sun.xyz), 0.05);
		float k = clamp((ambient + (1.0 - ambient) * max(dot(nScreen, P.sun.xyz), 0.0)) / flat, 0.5, 1.6);
		k = mix(1.0, k, saturate(P.sun.w * 1.2) * sunlit);
		float3 lit = terrainColor * k;
		// 背阴处偏冷，雪地更明显
		lit = mix(lit, lit * mix(P.shade.rgb, P.shade.rgb * float3(0.92, 0.96, 1.04), snowShare), saturate((1.0 - k) * 1.8));

		// 地形交界的淡墨线：本地形与泥地、沙地之间，泥地与沙地之间；按噪声断续，像干笔
		float breakup = smoothstep(0.25, 0.65, noiseTex.sample(noiseSmp, world * 0.9 + float2(0.7, 0.4)).b);
		float inkOwn = 1.0 - smoothstep(0.0, max(splatEdgeWidth * 1.4, 1e-4), abs(splat.r - 0.5));
		float inkSand = (1.0 - smoothstep(0.0, max(sandEdgeWidth * 1.4, 1e-4), abs(splat.g - splat.b))) * smoothstep(0.3, 0.6, splat.g + splat.b);
		// 特殊地面的边界也勾一道（阴煞地重些，灵气草甸淡些）
		float inkSpecial = (1.0 - smoothstep(0.0, max(specialEdgeWidth * 1.4, 1e-4), abs(specialCover - 0.5))) * mix(0.5, 1.0, specialSplit.x);
		float ink = max(max(inkOwn * step(0.05, splat.g + splat.b), inkSand * 0.6), inkSpecial) * (0.35 + 0.65 * breakup) * 0.32;
		lit = mix(lit, float3(0.15, 0.17, 0.18), ink);

		color = mix(color, lit, materialShare);
	}

	if(land > 0.003)
	{
		// 剩下的图块原画：抹开接缝；所有陆地再加大尺度的明暗干湿（有材质处减半，材质和起伏光影已经有层次）
		float3 varied = materialShare < 0.99 ? blendTileSeams(color, rel, slots, slotOrigin, groundTex, groundSmp, maskTex, maskSmp) : color;
		varied = mix(varied, color, materialShare);
		varied = groundVariation(varied, world, t, fieldHere.a * (1.0 - 0.5 * materialShare), noiseTex, noiseSmp);
		color = mix(color, varied, land);

		// 蹄印：画在地面上，后面叠的物件（英雄自己、树）会盖住它
		if(P.lightInfo.y > 0.5)
			color = mix(color, footprints(color, world, P), land);
	}

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

		// 海面的笔触与白浪按地图坐标取（2026-11-21）：图集第 10 行是用水面图块同一张材质做成的无缝海面（地形模组
		// tools/materials.py 的 water_row），六边形随机铺贴、随时间平移流动，代替图块原画的明暗。原画每个变体从材质不同位置
		// 切一块，图块边上笔触对不上，海上一格一道竖缝（用户 2026-11-20 截图）。湖、河照旧用原画；靠岸一窄条（遮罩 0.5~0.8）
		// 渐变回原画（岸线的淡墨勾边画在原画里）；缩得太小、材质淡出时也退回原画。图集没有这一行时保持原样
		const int seaSet = 10;             // 图集里海面那一行
		const float seaCellTiles = 5.3125; // 一格海面铺多少图块，与 materials.py 的 WATER_CELL_TILES 一致
		float seaHere = saturate(maskRGB.b) / max(maskRGB.g, 0.001);
		float seaWorld = (atlasSets > seaSet ? 1.0 : 0.0) * seaHere * smoothstep(0.5, 0.8, maskRGB.r) * P.terrain.x;
		if(seaWorld > 0.003)
		{
			// 折射扰动与上面取原画时相同；流向、流速与图块动画相同：每帧（0.18 秒）挪 (3.2, 0.8) 个 3× 像素，即每秒约 (0.185, 0.046) 个图块
			float2 seaPos = world + ripple * (0.035 + 0.035 * deep);
			float2 seaUV = (seaPos + float2(0.185, 0.046) * t) / seaCellTiles;
			const int seaCell = seaSet * 4;
			float seaLum = dot(hexMaterial(atlasTex, atlasSmp, P, seaCell, seaUV).rgb, float3(0.3, 0.59, 0.11));
			float seaMean = dot(P.cellMean[seaCell].rgb, float3(0.3, 0.59, 0.11));
			// 亮部此起彼伏（水光粼粼），同图块动画的 shimmer：12 帧一个周期，相位按一小片一小片的噪声错开。
			// 原画是正的细节乘 (0.6 + shimmer)，图集里存的是 1 倍
			float phase = noiseTex.sample(noiseSmp, seaPos * 2.1 + float2(0.53, 0.17)).g;
			float shimmer = 0.45 + 0.55 * sin(6.2832 * (t / 2.16 + 2.0 * phase));
			seaLum += max(seaLum - seaMean, 0.0) * (shimmer - 0.4);
			// 岸边浅水带提亮，同原画 build_water 的 foam：遮罩 0.95→0.55 处向 240 提亮 45%
			float foam = smoothstep(0.95, 0.55, maskRGB.r) * step(0.5, maskRGB.r);
			seaLum = mix(seaLum, 0.94, 0.45 * foam);
			texLum = mix(texLum, seaLum, seaWorld);
		}

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

		// 5. 波光：两层流向不同的噪声叠加后只取高值，稀疏地闪动；海、湖只在开阔处，河面整条都有。
		//    要有直射阳光：云影里、夜里都暗下去
		float g = nC.b * 0.6 + nB.a * 0.4;
		float glint = smoothstep(0.66, 0.82, g) * max(deep, riverShare * 0.8) * (1.0 - 0.8 * cloudShade) * (1.0 - 0.75 * P.night.x);
		fx += glint * 0.38 * float3(1.0, 0.98, 0.92);
		emission += glint * 0.9 * saturate(wet);

		// 6. 阵雨（2026-11-18）：雨点打出一圈圈涟漪；下雨处水面略灰暗
		if(rain > 0.01)
		{
			// 水面先压暗、褪色（雨云下、水面被打毛），涟漪才显得出来；原画的白浪笔触很碎，涟漪太淡会淹没在里面
			fx = mix(fx, float3(dot(fx, float3(0.3, 0.59, 0.11))) * float3(0.86, 0.91, 0.98), 0.3 * rain);
			fx += rainRipples(world, t, rain, pixel) * 0.42 * float3(0.88, 0.94, 1.0) * saturate(wet * 1.5);
		}

		color = mix(color, fx, saturate(wet));
	}

	// 地面画完，换回没扭过的坐标：物件不受热浪扭曲
	uv = uvTrue;
	rel = relTrue;
	world = worldTrue;

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
		objects.rgb = relight(objects.rgb, objects.a, surface, sunlit, P);

	// 修仙建筑：离地高度取起伏层 B，后面的雾、云海、云影、局部光都按它算；再按左右剪影和墙脚加明暗（见 shadeBuilding）。
	// 这时 color 还是建筑下面的地面，墙脚映的就是它的颜色
	float mark = vividMark(relief);         // 带修仙记号：建筑、资源与宝物（保色用）
	float building = buildingMark(relief);  // 其中的建筑
	if(building > 0.001)
	{
		float buildingHeight = buildingHeightOf(relief);
		surface.height = mix(surface.height, buildingHeight, building);
		if(P.misc.y > 0.001)
		{
			float side = buildingSide(rel, slots, slotOrigin, reliefTex, reliefSmp);
			float3 toned = matchBuildingTone(objects.rgb / max(objects.a, 0.001)) * objects.a;
			objects.rgb = mix(objects.rgb, shadeBuilding(toned, side, buildingHeight, color, sunlit, P), building * P.misc.y);
		}
	}

	color = objects.rgb + color * (1.0 - objects.a);

	// 长投影：预处理着色器按起伏层逐点朝太阳方向找遮挡，算在 1/4 大小的光照图上（线性取样，影子边缘自然柔和）。
	// 落在地面、湖面和矮处的物件上，偏冷色。取样位置按地图坐标上的噪声错开约 0.15 个图块，影子边缘随地面起伏弯弯曲曲；
	// 再用一层细噪声把影子打散成深浅不一的斑驳（树冠透光），不再是一整块平涂的直边长条（2026-11-17 用户反馈雪地树影假）
	float4 nShadow = noiseTex.sample(noiseSmp, world * 0.21 + float2(0.17, 0.53));
	float2 shadowWarp = (nShadow.rg - 0.5) * 0.3;
	float sunShadow = lightTex.sample(lightSmp, windowToUV(rel + shadowWarp, slots, slotOrigin)).r * P.shade.w * P.misc.y;
	sunShadow *= 0.72 + 0.28 * smoothstep(0.15, 0.85, noiseTex.sample(noiseSmp, world * 0.7).b);
	// 大块云影里没有直射阳光，长投影也就淡了
	sunShadow *= sunlit;
	color *= mix(float3(1.0), P.shade.rgb * 0.6, sunShadow);

	// 大块云影：整片压暗、略偏冷（天光），边缘是柔的
	color *= mix(float3(1.0), float3(0.74, 0.77, 0.86), cloudShade);

	// 雾云场：G 生雾多少，B 山腰绕云多少（与离岸距离图同一张，按地图坐标取样）。
	// 没有起伏图的物件（英雄、城镇、宝物、资源、船……都是可交互的）上不起雾、不盖云：雾和云只是气氛，不能挡住要点的东西。
	// 修仙建筑也可交互，但整个跳过的话雾和云影飘到轮廓就断，像贴上去的（2026-11-19）：云影照落，雾和云海打折（buildingFog、buildingCloud）
	float2 fieldSize = max(P.misc.zw, float2(1.0));
	float interactive = saturate(relief.a * 1.5) * (1.0 - surface.known) * (1.0 - building);
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

	// 流动的雾，贴着地面，在物件与粒子之间。沼泽上的雾带着黄绿的瘴气色
	float miasma = weatherField.b * P.misc.y * (1.0 - interactive);
	const float3 miasmaColor = float3(0.60, 0.70, 0.42);
	if(field.g * P.atmos.x > 0.003)
	{
		float fog = fogAt(world, t, field.g, surface.height, noiseTex, noiseSmp) * P.atmos.x * mix(1.0, buildingFog, building);
		float3 fogColor = mix(float3(0.86, 0.89, 0.92), float3(0.86, 0.89, 0.92) * P.sunColor.rgb, 0.35);
		fogColor = mix(fogColor, miasmaColor, miasma * 0.55);
		color = mix(color, fogColor, saturate(fog));
	}

	// 瘴气（2026-11-18）：沼泽上贴地的黄绿浊气，一团团缓慢打旋、一胀一缩地呼吸，比白雾慢、比白雾稠
	if(miasma > 0.003)
	{
		float m1 = noiseTex.sample(noiseSmp, (world - cloudWind * (t * 0.05)) * 0.12 + float2(0.21, 0.66)).g;
		float m2 = noiseTex.sample(noiseSmp, world * 0.3 + float2(t * 0.012, -t * 0.019)).r;
		float swirl = smoothstep(0.35, 0.85, m1 * 0.6 + m2 * 0.4);
		float breath = 0.7 + 0.3 * sin(t * 0.35 + m1 * 6.283);
		float density = miasma * (0.2 + 0.6 * swirl) * breath * exp(-surface.height / 0.5) * 0.42;
		color = mix(color, miasmaColor * mix(float3(1.0), P.sunColor.rgb, 0.3), saturate(density));
	}

	// 阴气、灵气（2026-11-19，特殊地面）：阴煞地（被诅咒的大地）上贴地一层灰紫的淡墨，比瘴气更低、更暗、更慢，
	// 一缕缕游走，入夜更浓；灵气草甸（魔法平原）贴地泛一层很淡的玉青微光，一处处缓缓明灭。可交互物件上不画
	float2 specialField = specialKinds * P.misc.y * (1.0 - interactive);
	if(specialField.x > 0.003)
	{
		float y1 = noiseTex.sample(noiseSmp, (world - cloudWind * (t * 0.03)) * 0.16 + float2(0.83, 0.12)).b;
		float y2 = noiseTex.sample(noiseSmp, world * 0.42 + float2(-t * 0.015, t * 0.01)).g;
		float wisps = smoothstep(0.3, 0.8, y1 * 0.65 + y2 * 0.35);
		float density = specialField.x * (0.3 + 0.7 * wisps) * exp(-surface.height / 0.35) * (0.42 + 0.15 * P.night.x);
		float3 yinColor = float3(0.27, 0.26, 0.31) * mix(float3(1.0), P.sunColor.rgb, 0.2);
		color = mix(color, yinColor, saturate(density));
	}
	if(specialField.y > 0.003)
	{
		float s1 = noiseTex.sample(noiseSmp, (world + cloudWind * (t * 0.02)) * 0.22 + float2(0.37, 0.58)).r;
		float pulse = 0.7 + 0.3 * sin(t * 0.8 + s1 * 6.283);
		float glow = specialField.y * smoothstep(0.4, 0.8, s1) * pulse * exp(-surface.height / 0.3) * (0.07 + 0.08 * P.night.x);
		color += float3(0.45, 0.85, 0.72) * glow;
		emission = max(emission, glow * 2.0);
	}

	// 云层（山腰云海）：在离地 cloudAltitude 处。滚动地图时云层比地面移得快（视差），像离镜头更近；
	// 比云层高的山峰从云里伸出来，矮处、地面被半透明的云盖住。迎着太阳的一侧亮、背面偏灰
	if(cloudCover * P.atmos.y > 0.003)
	{
		float2 cloudPos = world + P.camera.xy * P.camera.z + cloudWind * (t * 0.12);
		float shape = cloudShape(cloudPos, noiseTex, noiseSmp);
		float density = cloudDensity(shape, cloudCover) * (1.0 - smoothstep(cloudAltitude - 0.2, cloudAltitude + 0.4, surface.height))
			* mix(1.0, buildingCloud, building);
		if(density > 0.001)
		{
			float2 toSun = -P.shadowDir.xy / max(length(P.shadowDir.xy), 1e-3);
			float lit = saturate(0.55 + (cloudShapeCoarse(cloudPos, noiseTex, noiseSmp) - cloudShapeCoarse(cloudPos + toSun * 0.35, noiseTex, noiseSmp)) * 4.0);
			float3 cloudColor = mix(float3(0.63, 0.67, 0.75), float3(0.98, 0.97, 0.95), lit) * mix(float3(1.0), P.sunColor.rgb, 0.4);
			color = mix(color, cloudColor, density * 0.55 * P.atmos.y);
		}
	}

	// 时段色调（2026-11-18 起含夜晚）：清晨偏暖、黄昏偏橙、夜里偏蓝变暗。夜里颜色也褪一些——人眼在暗处辨不清颜色，
	// 只把亮度压暗、颜色照旧，会像一张调暗了的白天照片
	float3 albedo = color;
	float lum = dot(color, float3(0.3, 0.59, 0.11));
	color = mix(color, lum * float3(0.92, 0.97, 1.08), P.night.z * P.misc.y);
	color *= mix(float3(1.0), P.grade.rgb, P.misc.y);

	// 局部光（2026-11-18）：灯笼、营火、法光、英雄火把照亮周围的地面和物件，熔岩的红光照亮岩浆地和它旁边的地面。
	// 按上面算好的颜色（调色之前）乘光，天越暗越显眼；物件表面按它的离地高度挪回脚下再算距离，朝着光的一面更亮
	// 正午局部光显出来不到一成、几乎看不出，整段跳过（平滑地淡出，不会一下子灭掉）
	float lamps = P.night.y * smoothstep(0.08, 0.2, P.night.y) * P.misc.y * inWorld;
	float cover = saturate(objects.a);

	// 岩浆地上发红的裂缝：白天也算自发光（辉光不论早晚都有），夜里还照亮自己（见下）
	float crack = 0.0;
	float breathe = 1.0;
	if(weatherField.a > 0.003)
	{
		breathe = 0.8 + 0.2 * sin(t * 0.45 + noiseTex.sample(noiseSmp, world * 0.05).r * 6.283);
		crack = saturate((albedo.r - albedo.b) * 3.0 - 0.6) * smoothstep(0.3, 0.55, albedo.r) * (1.0 - cover) * weatherField.a * P.misc.y;
		emission += crack * breathe * (0.45 + 0.55 * P.night.x);
	}

	if(lamps > 0.001)
	{
		float h = surface.height * cover;
		float3 N = normalize(mix(groundN, surface.normal, cover));
		float3 halo = float3(0.0);
		float3 light = localLights(world + float2(0.0, h), world, h, N, P, halo) * 1.7;
		float3 glow = float3(0.0);
		if(weatherField.a > 0.003)
		{
			// 熔岩：暗红的光漫到旁边的地面和物件上；岩浆地上发红的裂缝自己发光，天黑也不暗下去（一整片都照亮会成一片粉红，2026-11-18 实机）
			light += float3(1.0, 0.32, 0.08) * (weatherField.a * breathe * 0.4);
			glow = albedo * float3(1.0, 0.7, 0.5) * (crack * breathe * 1.3);
		}
		// 亮色（雪、白墙）加光后很快冲白：按底色亮度收一些
		color += albedo * light * lamps / (1.0 + 0.6 * lum * dot(light, float3(0.33)));
		color += glow * lamps;
		float3 haloLight = halo * (P.night.x * 0.45 * P.misc.y);
		color += haloLight;
		// 灯火的辉光：光源近处才亮，后处理再把它晕开
		emission += saturate(dot(haloLight, float3(0.4)) * 1.6);
	}


	// 植被粒子（MapParticles）和覆盖层（迷雾、路径）在这一遍之后另外画上去，粒子才能夹在物件和迷雾之间。
	// alpha 是自发光的程度：这一遍直接覆盖（不混合），画进离屏图后由后处理取出来做辉光；直接画到窗口上时 alpha 用不到

	// 修仙保色：后处理会统一调色时，资源与宝物的像素先做反变换（见 undoPostGrade），建筑按 vividStrength（atmos.w）。
	// 时段色调、夜色、灯光照旧——那是光，不是调色
	if(P.atmos.z > 0.001)
	{
		float keep = mark * mix(1.0, P.atmos.w, building);
		if(keep > 0.001)
			color = mix(color, undoPostGrade(color, P.atmos.z, P.night.x), keep);
	}

	main0_out out;
	out.color = float4(color, saturate(emission)) * in.color;
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
		// 修仙建筑（R、G 记号，与主着色器 vividMark 一致）不当遮挡物：它的 B 只给雾、云海用，影子是导出时烘好的固定投影
		float3 enc = c.rgb / c.a;
		float building = smoothstep(0.015, 0.028, enc.r) * (1.0 - smoothstep(0.06, 0.09, enc.r)) * (1.0 - smoothstep(0.015, 0.03, enc.g));
		float caster = c.b / c.a * range * (1.0 - building);
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

/// 水墨云海迷雾（2026-11-18）：第三个片元着色器，在粒子之后画，代替原来直接叠覆盖层的那一步。
/// 主纹理就是覆盖层（路径、图标；云雾模式下没有原版迷雾），先按云雾场画云，再把覆盖层按预乘 alpha 叠在云上，输出预乘颜色。
///
/// 云雾场每个图块一个像素（R 浓度、G 正在散开的程度、B 在法术射程外），按地图坐标线性取样，四点平均抹圆，同时得出梯度（迷雾深处在哪边）。
/// 云的边缘在“浓度 + 噪声起伏”过阈值处：浓度 0.5 正好在看得见与看不见的图块交界，噪声让它不规则地鼓出、凹进，并随风缓缓流动；
/// 边缘外还有一圈半透明的薄雾慢慢淡开。完全在迷雾里的图块（地图缓存那里只有空地面）浓度为 1，不论噪声多少都是不透明的。
/// 揭开时浓度由地图缓存按离已探索处的远近依次降到 0，边缘一路退向迷雾深处；散开的程度越大，噪声扭得越厉害、打着旋、
/// 往迷雾深处推开，纸面变薄、墨边变浓往外洇——像墨被水晕开散去。
///
/// 画法是东方水墨（2026-11-18 用户嫌白色体积云没有水墨韵味，重做）：暖米色宣纸底带纤维，横向大片湿笔淡墨烟云随风流动、
/// 墨干处有水渍边；云靠淡墨烘托留白；云海里有装饰性的淡墨远山（固定在地图上）；边缘是宣纸洇墨的毛边、淡墨勾边，外面一层淡墨烟。
/// 昼夜色调与合成着色器一致，夜里纸色压暗偏青；在法术射程外的云压暗（原版画成纯黑）。
///
/// 取样：覆盖层 1 次、云雾场 5 次（小纹理）；有云的像素再取噪声十几次（256 × 256 的小纹理），远山每组 2 次。看得见的地方取完云雾场就返回。
const char * const fogShaderSource = R"FOGMSL(
#include <metal_stdlib>
using namespace metal;

struct Params
{
	float4 cache;  // xy：缓存纹理像素尺寸；zw：槽位数（图块）
	float4 world;  // xy：可见窗口左上角图块的地图坐标；zw：该图块所在槽位
	float4 misc;   // x：时间（秒）；y：屏幕上一个图块多少像素；zw：云雾场尺寸（图块）
	float4 sun;    // xyz：指向太阳的方向（屏幕坐标：x 右、y 上、z 朝镜头）；w：天黑的程度
	float4 grade;  // rgb：整幅画面的时段色调；w：夜里颜色褪掉多少
	float4 style;  // x：地下（1）还是地上（0）
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

// 云雾场按地图坐标取样：图块 (x, y) 的像素中心在地图坐标 (x + 0.5, y + 0.5)，线性插值，地图外夹到边缘的值
static inline float4 fogAt(texture2d<float> fogTex, sampler fogSmp, float2 world, float2 size)
{
	return fogTex.sample(fogSmp, world / size);
}

// 云团：大团约 3.5 个图块（噪声 R），上面鼓着约 1.7 个图块的云头（G）。
// x 是云头的形状——取“鼓包”（|2n−1|，两团之间凹进去，谷底磨圆），边缘就是一个个圆鼓鼓的云头；y 是平滑的高低
static inline float2 cloudForm(texture2d<float> noiseTex, sampler noiseSmp, float2 p)
{
	float big = noiseTex.sample(noiseSmp, p * (1.0 / 28.0)).r;
	float mid = noiseTex.sample(noiseSmp, p * (1.0 / 14.0) + float2(0.71, 0.23)).g;
	float d = mid * 2.0 - 1.0;
	float puff = sqrt(d * d + 0.03) - 0.17;
	return float2(big * 0.58 + puff * 0.40, big * 0.62 + mid * 0.38);
}

static inline float2 hash22(float2 p)
{
	float3 q = fract(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
	q += dot(q, q.yzx + 33.33);
	return fract((q.xx + q.yz) * q.zy);
}

// 一组山在 q 处的墨（0~1）。id 是这组山所在的格子，cell 是格子大小，shift 是这一行横向错开多少
static inline float mountainGroup(float2 q, float2 id, float2 cell, float shift, texture2d<float> noiseTex, sampler noiseSmp)
{
	float2 a = hash22(id);
	float2 b = hash22(id + float2(17.3, 5.1));
	float2 c = hash22(id + float2(3.7, 29.9));
	if(a.x > 0.5)
		return 0.0;
	// 位置、大小都放开：有的是一座孤峰，有的是一溜矮山，山脚高低错开，不排成一行
	float x0 = (id.x + 0.2 + 0.6 * a.y) * cell.x - shift;
	float baseY = (id.y + 0.4 + 0.5 * b.x) * cell.y;
	float halfWidth = 2.2 + 4.0 * b.y;
	float height = (1.2 + 2.6 * c.x) * (0.6 + 0.4 * halfWidth / 6.2);
	float dx = (q.x - x0) / halfWidth;
	if(abs(dx) >= 1.3)
		return 0.0;
	// 主峰位置、尖锐程度随机，两侧的次峰有的有、有的没有
	float sharp = 1.1 + 0.7 * c.y;
	float main = pow(max(1.0 - abs(dx - (c.x - 0.5) * 0.5) / 0.75, 0.0), sharp);
	float left = pow(max(1.0 - abs(dx + 0.5 + 0.2 * b.x) / 0.5, 0.0), 1.3) * step(0.35, c.y) * (0.35 + 0.35 * a.y);
	float right = pow(max(1.0 - abs(dx - 0.55 - 0.2 * a.y) / 0.55, 0.0), 1.3) * step(c.y, 0.75) * (0.3 + 0.4 * b.x);
	float ridge = noiseTex.sample(noiseSmp, float2(q.x * (1.0 / 2.7), id.y * 0.37 + a.x)).g;
	float profile = max(main, max(left, right)) * (0.88 + 0.24 * ridge);
	if(profile <= 0.0)
		return 0.0;
	float top = baseY - height * profile;
	float below = q.y - top;
	if(below < -0.1)
		return 0.0;
	// 山脊一笔：轮廓处最浓，往下很快淡成一层薄墨，再淡进云里；山脚整片淡出
	float crest = smoothstep(-0.05, 0.04, below);
	float stroke = 1.0 - smoothstep(0.0, 0.18, below);
	float body = exp(-below / (0.45 + 0.35 * height * profile));
	float foot = 1.0 - smoothstep(baseY - 1.2, baseY + 0.2, q.y);
	float rub = noiseTex.sample(noiseSmp, (q - float2(x0, 0.0)) * float2(1.0 / 0.6, 1.0 / 2.4)).b;
	float m = crest * (stroke * 0.55 + body * 0.6 * (0.7 + 0.6 * rub)) * foot;
	// 远近：有的淡、有的浓一些
	return m * (0.3 + 0.45 * b.x * b.x);
}

// 远山：云海里若隐若现的淡墨山。固定在地图上（不随镜头移动），按 13 × 9 个图块分格，约一半的格子里有一组山；
// 每一行横向错开、山脚高低不一，免得排成整齐的一排。山顶最高可伸进上一行，所以每个像素看本行和下一行、左中右三格。
// 纯装饰，与迷雾下真实的地形无关；只画在完全看不见的地方，不会和看得见的地形混淆
static inline float inkMountains(float2 q, texture2d<float> noiseTex, sampler noiseSmp)
{
	const float2 cell = float2(13.0, 9.0);
	float row = floor(q.y / cell.y);
	float ink = 0.0;
	for(int j = 0; j <= 1; ++j)
	{
		float ry = row + float(j);
		float shift = fract(ry * 0.618) * cell.x;
		float home = floor((q.x + shift) / cell.x);
		for(int i = -1; i <= 1; ++i)
			ink = max(ink, mountainGroup(q, float2(home + float(i), ry), cell, shift, noiseTex, noiseSmp));
	}
	return ink;
}

fragment main0_out main0(main0_in in [[stage_in]], constant Params & P [[buffer(0)]],
	texture2d<float> overlayTex [[texture(0)]], sampler overlaySmp [[sampler(0)]],
	texture2d<float> fogTex [[texture(1)]], sampler fogSmp [[sampler(1)]],
	texture2d<float> noiseTex [[texture(2)]], sampler noiseSmp [[sampler(2)]])
{
	main0_out out;
	float4 over = overlayTex.sample(overlaySmp, in.uv);
	float2 world = P.world.xy + windowPosition(in.uv, P.cache.zw, P.world.zw);
	float2 size = P.misc.zw;
	float t = P.misc.x;

	// 四点平均把按图块的场抹圆（半径不到半个图块：完全在迷雾里的图块，四个点仍只落在它四周的迷雾图块上，浓度保持 1）
	const float r = 0.45;
	float4 fe = fogAt(fogTex, fogSmp, world + float2(r, 0.0), size);
	float4 fw = fogAt(fogTex, fogSmp, world - float2(r, 0.0), size);
	float4 fs = fogAt(fogTex, fogSmp, world + float2(0.0, r), size);
	float4 fn = fogAt(fogTex, fogSmp, world - float2(0.0, r), size);
	float4 field = (fe + fw + fs + fn) * 0.25;

	// 地图外夹着边缘的值。云整片铺到世界边界、在边界上齐齐收住（抗锯齿一个屏幕像素）：世界外紧贴的一圈是边框
	// （MapRendererBorder，模组可换成装裱的隔水），原来让云在边框外一个多图块内淡掉，边上没探索时会把边框盖住（2026-11-20）
	float2 beyond = max(-world, world - size);
	float edgePixel = 1.0 / max(P.misc.y, 1.0);
	float fade = 1.0 - smoothstep(-edgePixel, 0.0, max(beyond.x, beyond.y));
	float density = field.r * fade;
	if(density <= 0.003)
	{
		out.color = over;
		return out;
	}

	float stir = field.g;  // 正在散开的程度
	float dark = field.b;  // 在法术射程外
	float2 deeper = float2(fe.r - fw.r, fs.r - fn.r) / (2.0 * r); // 指向迷雾深处
	float detail = smoothstep(7.0, 20.0, P.misc.y); // 缩得很小时细纹淡出，免得闪

	// 烟云随风缓缓流动（与树木风场同向：往右、略往下）；低频噪声扭曲取样位置，形状慢慢变化。
	// 散开时扭曲加大、打着旋，往迷雾深处推开
	float2 drift = float2(0.16, 0.05) * t;
	float2 wq = world * (1.0 / 31.0) + float2(0.37, 0.11) + t * float2(0.0021, -0.0013);
	float2 warp = float2(noiseTex.sample(noiseSmp, wq).a, noiseTex.sample(noiseSmp, wq.yx + float2(0.53, 0.29)).r) - 0.5;
	float turn = stir * 1.3;
	warp = float2x2(cos(turn), sin(turn), -sin(turn), cos(turn)) * warp * (1.0 + stir * 0.7);
	float2 p = world - drift - deeper * stir * 0.9 + warp * 2.2;

	float2 form = cloudForm(noiseTex, noiseSmp, p);
	float fine = noiseTex.sample(noiseSmp, p * (1.0 / 10.0) + float2(0.13, 0.57)).b;
	// 宣纸的纤维：洇墨的边缘顺着纤维走，是毛的，不是光滑的弧
	float fiber = noiseTex.sample(noiseSmp, world * (1.0 / 2.1) + float2(0.41, 0.83)).a * 0.6
		+ noiseTex.sample(noiseSmp, world * (1.0 / 0.8) + float2(0.17, 0.29)).b * 0.4;

	// 边缘：浓度加起伏过阈值。起伏有大团、云头和纸纤维三层，边缘是一个个云头，细看是洇开的毛边；散开时毛边更乱
	float small = noiseTex.sample(noiseSmp, p * (1.0 / 7.2) + float2(0.29, 0.83)).g * 2.0 - 1.0;
	float shape = form.x * 0.85 + (sqrt(small * small + 0.02) - 0.14) * 0.32 + (fine - 0.5) * 0.08 * detail;
	float wobble = 1.0 + stir * 0.3;
	float level = density + (shape - 0.46) * wobble + (fiber - 0.5) * (0.09 + 0.12 * stir) * detail;
	float body = smoothstep(0.42 - stir * 0.12, 0.52, level);
	// 完全在迷雾里（浓度 1，地图缓存那里只有空地面）必须不透明
	body = max(body, smoothstep(0.96, 0.995, density));
	// 边缘外一层淡墨烟，慢慢淡开
	float haze = smoothstep(0.24, 0.48, level) * (0.5 + 0.5 * fine) * 0.32;

	// ---- 画面：宣纸为底，淡墨烘出留白的云 ----
	const float3 paper = float3(0.86, 0.835, 0.78);
	const float3 inkColor = float3(0.13, 0.13, 0.14);
	float edgeZone = 1.0 - smoothstep(0.86, 0.985, density);
	float deep = smoothstep(0.9, 1.0, density); // 只在完全看不见的地方画远山、烟云的重墨

	// 烟云：横向拉长的淡墨带，随风流动；墨干处边上略聚（水渍边）
	float2 bq = float2(p.x * (1.0 / 40.0), p.y * (1.0 / 12.0));
	float bandN = noiseTex.sample(noiseSmp, bq + float2(0.13, 0.71)).r * 0.78 + noiseTex.sample(noiseSmp, bq * float2(2.3, 1.9) + float2(0.57, 0.09)).g * 0.22;
	float wash = smoothstep(0.52, 0.90, bandN);
	float pool = smoothstep(0.50, 0.56, bandN) * (1.0 - smoothstep(0.56, 0.64, bandN));
	float ink = wash * 0.2 + pool * 0.05 * detail;

	// 远山：固定在地图上。原来带视差（比地面移得慢），镜头跟着英雄走时山也跟着滑，看上去像山在动（2026-11-18 用户反馈）
	// 缩得很小时（屏幕上一个图块不到二十几个像素）远山只剩满屏的小三角，像撒了一地帐篷，淡出
	// 地下没有远山（2026-11-19 用户：地下不该有云）
	bool underground = P.style.x > 0.5;
	float hills = underground ? 0.0 : inkMountains(world, noiseTex, noiseSmp) * deep * smoothstep(16.0, 28.0, P.misc.y);

	// 云：留白。云头盖住烟云和山腰（云遮山），云头下沿勾一笔淡墨、染一点淡墨，笔触时断时续
	float cloudWhite = smoothstep(0.50, 0.72, form.x);
	float formBelow = cloudForm(noiseTex, noiseSmp, p + float2(0.0, 0.35)).x;
	float underside = smoothstep(0.0, 0.06, form.x - formBelow);
	float cloudShade = smoothstep(0.28, 0.50, form.x) * (1.0 - smoothstep(0.50, 0.62, form.x)) * underside;

	ink = ink * (1.0 - cloudWhite * 0.85) + hills * (1.0 - cloudWhite * 0.9);
	ink += cloudShade * 0.10;

	// 边缘：里侧一道淡墨（墨洇到边上聚起来的水渍），最外一笔细一些的墨边；散开时墨被水晕开，这一道变浓、往外洇
	float inner = smoothstep(0.46, 0.52, level) * (1.0 - smoothstep(0.52, 0.70, level)) * edgeZone;
	float contour = smoothstep(0.42, 0.46, level) * (1.0 - smoothstep(0.46, 0.51, level)) * edgeZone;
	ink += inner * 0.07 + contour * (0.13 + 0.25 * stir) * (0.6 + 0.4 * fiber);

	float3 color = mix(paper * (0.96 + (fiber - 0.5) * 0.08 * detail), inkColor, clamp(ink, 0.0, 0.9));

	// 地下：不是云海，是一片泼墨的幽暗——浓墨底子带宣纸纤维，上面只有横向缓缓流动、颜色略浅的墨烟，
	// 云面的大起伏只留成浓淡变化（墨分五色），没有留白的云头；边上里侧略浅，像墨洇开，最外一笔更浓的墨边
	const float3 deepInk = float3(0.15, 0.145, 0.14);
	const float3 smokeInk = float3(0.34, 0.33, 0.31);
	if(underground)
	{
		float smoke = wash * 0.55 + pool * 0.12 * detail + smoothstep(0.35, 0.75, form.y) * 0.12;
		color = mix(deepInk, smokeInk, smoke) * (0.95 + (fiber - 0.5) * 0.12 * detail);
		color = mix(color, smokeInk * 0.85, inner * 0.35);
		color = mix(color, deepInk * 0.5, contour * 0.5 * detail);
	}

	// 法术射程外压暗
	color *= 1.0 - 0.6 * dark;

	// 时段色调；夜里纸色暗下来、偏青
	color *= P.grade.rgb;
	float night = P.sun.w;
	float luma = dot(color, float3(0.299, 0.587, 0.114));
	color = mix(color, float3(luma), P.grade.w * 0.6);
	color = mix(color, color * float3(0.32, 0.35, 0.44), night);

	// 外面一圈淡墨烟是半透明的灰，正在散开的云变薄
	float3 hazeColor = (underground ? mix(deepInk, smokeInk, 0.35) : mix(paper, inkColor, 0.18)) * P.grade.rgb;
	hazeColor = mix(hazeColor, hazeColor * float3(0.32, 0.35, 0.44), night);
	float alpha = clamp(max(body, haze) * (1.0 - stir * 0.45), 0.0, 1.0);
	color = mix(hazeColor, color, body / max(max(body, haze), 1e-4));

	// 云在地上投一层淡影（光从上方来，影子落在云的下方）
	float above = fogAt(fogTex, fogSmp, world + float2(0.2, -0.42), size).r * fade;
	float shadow = smoothstep(0.42, 0.62, above + (shape - 0.46) * 0.6) * 0.22 * (1.0 - night * 0.6);

	// 预乘颜色：影子在云下，覆盖层（路径、图标）在最上
	float4 cloud = float4(color * alpha, alpha);
	cloud += float4(float3(0.03, 0.03, 0.04) * shadow, shadow) * (1.0 - alpha);
	out.color = over + cloud * (1.0 - over.a);
	return out;
}
)FOGMSL";

/// 辉光（2026-11-18）：第四个片元着色器，两种用法（Params.texel.z）。
///   0：从地图离屏图里取自发光缩成 1/4。主着色器把熔岩裂缝、水面波光、灯火辉光的自发光程度写在 alpha 里，粒子、云雾画进离屏图时
///      只会把它压低（云遮住熔岩就不再发光），所以这里取 颜色 × alpha。四个双线性取样各平均 2 × 2，合起来是 4 × 4 的平均。
///   1：把 1/4 图模糊缩成 1/16：4 × 4 个双线性取样、间隔 2 个像素、按 1:3:3:1 加权，约是 8 × 8 像素的高斯模糊。
/// 输出不透明，按覆盖画进辉光图。
const char * const bloomShaderSource = R"BLOOMMSL(
#include <metal_stdlib>
using namespace metal;

struct Params
{
	float4 texel; // xy：源纹理一个像素在纹理坐标里多大；z：0 取自发光、1 模糊
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

fragment main0_out main0(main0_in in [[stage_in]], constant Params & P [[buffer(0)]],
	texture2d<float> mainTex [[texture(0)]], sampler mainSmp [[sampler(0)]])
{
	float2 px = P.texel.xy;
	float3 sum = float3(0.0);
	if(P.texel.z < 0.5)
	{
		for(int k = 0; k < 4; ++k)
		{
			float2 o = float2((k & 1) != 0 ? 1.0 : -1.0, (k & 2) != 0 ? 1.0 : -1.0);
			float4 c = mainTex.sample(mainSmp, in.uv + o * px);
			sum += c.rgb * c.a;
		}
		sum *= 0.25;
	}
	else
	{
		const float weight[4] = {1.0, 3.0, 3.0, 1.0};
		for(int j = 0; j < 4; ++j)
			for(int i = 0; i < 4; ++i)
				sum += mainTex.sample(mainSmp, in.uv + float2(float(i) * 2.0 - 3.0, float(j) * 2.0 - 3.0) * px).rgb * (weight[i] * weight[j]);
		sum *= 1.0 / 64.0;
	}
	main0_out out;
	out.color = float4(sum, 1.0);
	return out;
}
)BLOOMMSL";

/// 后处理（2026-11-18）：第五个片元着色器，把地图离屏图画到窗口上，只画地图所在的矩形。
///   1. 辉光：1/4 图（四点帐篷，紧贴光源）加 1/16 图（晕得很开），亮处加得少，免得冲成一片白。
///   2. 统一色调：往水墨的设色拉——越艳的颜色收得越多（原版像素风的宝箱、资源金黄大红最艳，只能缓解），暗处偏墨青、亮处偏纸暖，
///      黑位抬成墨色、高光收成纸白（墨不会黑到发死，纸也不是纯白）。
///   3. 宣纸：纸浆厚薄不匀的云状斑、斜着的细长纤维，跟着地图走（与云雾的纸纹同一套地图坐标，滚动时纸不滑），亮处看得出纸、浓墨处盖住；
///      缩得很小时纤维小于一个像素，淡出免得闪。
///   4. 暗角：四角轻轻压暗、偏一点墨褐。
///   5. 边缘墨晕：地图边上一圈淡墨，像画到纸边时墨洇开——宽窄不匀（固定在屏幕上），里侧有一道墨聚起来的水渍边。
/// 取样：离屏图 1 次、辉光图 8 次（小纹理）、噪声 6 次。
const char * const postShaderSource = R"POSTMSL(
#include <metal_stdlib>
using namespace metal;

struct Params
{
	float4 frame;  // xy：离屏图尺寸（像素）；zw：1/4 辉光图尺寸
	float4 bounds; // 地图视图露出来的矩形（离屏图像素）：左、上、宽、高
	float4 map;    // xy：可见窗口左上角图块的左上角在离屏图上的位置（像素）；z：一个图块多少像素；w：1/16 辉光图宽
	float4 world;  // xy：该图块的地图坐标；z：1/16 辉光图高；w：天黑的程度
	float4 tune;   // x：辉光强度；y：调色强度；z：宣纸纹理强度；w：暗角与墨晕强度
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

static inline float3 tent(texture2d<float> tex, sampler smp, float2 uv, float2 step)
{
	return 0.25 * (tex.sample(smp, uv + step * float2(-1.0, -1.0)).rgb
		+ tex.sample(smp, uv + step * float2(1.0, -1.0)).rgb
		+ tex.sample(smp, uv + step * float2(-1.0, 1.0)).rgb
		+ tex.sample(smp, uv + step * float2(1.0, 1.0)).rgb);
}

fragment main0_out main0(main0_in in [[stage_in]], constant Params & P [[buffer(0)]],
	texture2d<float> frameTex [[texture(0)]], sampler frameSmp [[sampler(0)]],
	texture2d<float> nearTex [[texture(1)]], sampler nearSmp [[sampler(1)]],
	texture2d<float> farTex [[texture(2)]], sampler farSmp [[sampler(2)]],
	texture2d<float> noiseTex [[texture(3)]], sampler noiseSmp [[sampler(3)]])
{
	float2 uv = in.uv;
	float2 pixel = uv * P.frame.xy;
	float3 color = frameTex.sample(frameSmp, uv).rgb;
	float night = P.world.w;

	// 1. 辉光
	float3 nearGlow = tent(nearTex, nearSmp, uv, 1.0 / P.frame.zw);
	float3 farGlow = tent(farTex, farSmp, uv, 0.75 / float2(P.map.w, P.world.z));
	float3 bloom = (nearGlow * 0.8 + farGlow * 1.4) * P.tune.x;
	color += bloom * (1.0 - 0.5 * saturate(color));

	// 2. 统一色调
	float g = P.tune.y;
	float lum = dot(color, float3(0.299, 0.587, 0.114));
	float hi = max(color.r, max(color.g, color.b));
	float lo = min(color.r, min(color.g, color.b));
	float sat = (hi - lo) / max(hi, 1e-3);
	color = mix(float3(lum), color, 1.0 - g * (0.08 + 0.30 * smoothstep(0.35, 0.9, sat)));
	float3 tint = mix(float3(0.95, 0.975, 1.02), float3(1.025, 1.0, 0.95), smoothstep(0.12, 0.7, lum));
	color *= mix(float3(1.0), tint, g);
	float3 inkFloor = float3(0.05, 0.05, 0.06) * (1.0 - 0.6 * night);
	const float3 paperTop = float3(0.97, 0.95, 0.905);
	color = mix(color, inkFloor + color * (paperTop - inkFloor), g);

	// 3. 宣纸（噪声的 R、G 通道一团约 1/12 纹理宽，B、A 约 1/36）
	float2 world = P.world.xy + (pixel - P.map.xy) / max(P.map.z, 1e-3);
	float detail = smoothstep(12.0, 26.0, P.map.z);
	float pulp = noiseTex.sample(noiseSmp, world * 0.13 + float2(0.31, 0.77)).r * 0.65
		+ noiseTex.sample(noiseSmp, world * 0.37 + float2(0.63, 0.12)).g * 0.35;
	float2 r1 = float2(dot(world, float2(0.94, 0.34)), dot(world, float2(-0.34, 0.94)));
	float2 r2 = float2(dot(world, float2(0.50, -0.87)), dot(world, float2(0.87, 0.50)));
	float f1 = noiseTex.sample(noiseSmp, r1 * float2(0.024, 0.42) + float2(0.17, 0.41)).b;
	float f2 = noiseTex.sample(noiseSmp, r2 * float2(0.030, 0.50) + float2(0.52, 0.86)).a;
	float fibers = (smoothstep(0.64, 0.80, f1) * 0.6 + smoothstep(0.66, 0.82, f2) * 0.4) * detail;
	float paperShow = mix(0.35, 1.0, smoothstep(0.15, 0.7, lum)) * P.tune.z;
	color *= 1.0 + (pulp - 0.5) * 0.08 * paperShow;
	color = mix(color, color * 0.94 + float3(0.04, 0.038, 0.034) * (1.0 - 0.7 * night), fibers * 0.7 * paperShow);

	// 4. 暗角
	float2 q = (pixel - P.bounds.xy) / max(P.bounds.zw, float2(1.0));
	float vignette = smoothstep(0.7, 1.5, length((q - 0.5) * float2(2.0, 1.85))) * P.tune.w;
	color = mix(color, color * float3(0.78, 0.74, 0.70), vignette * 0.55);

	// 5. 边缘墨晕：宽度按地图区域大小缩放（800 像素高时约 28 像素），起伏固定在屏幕上
	float edge = min(min(pixel.x - P.bounds.x, P.bounds.x + P.bounds.z - pixel.x), min(pixel.y - P.bounds.y, P.bounds.y + P.bounds.w - pixel.y));
	float scale = max(min(P.bounds.z, P.bounds.w) / 800.0, 0.25);
	float2 sp = pixel / scale;
	float ragged = (noiseTex.sample(noiseSmp, sp * (1.0 / 1400.0)).r - 0.5) * 26.0
		+ (noiseTex.sample(noiseSmp, sp * (1.0 / 420.0) + float2(0.37, 0.61)).b - 0.5) * 9.0;
	float reach = max((28.0 + ragged) * scale, 2.0);
	float wash = 1.0 - smoothstep(0.0, reach, edge);
	float ring = (edge - reach) / (2.0 * scale);
	float pool = exp(-ring * ring) * 0.6;
	float bleed = saturate((wash * wash * 0.55 + pool * 0.16) * P.tune.w);
	color = mix(color, float3(0.17, 0.16, 0.15) * (1.0 - 0.5 * night) + color * 0.25, bleed);

	main0_out out;
	out.color = float4(saturate(color), 1.0);
	return out;
}
)POSTMSL";

/// 悬停描边（2026-11-18）：第六个片元着色器。主纹理是地图缓存单独画出的悬停物件（透明底），四边形比物件大出一圈。
/// 在两个半径上各取一圈 12 个点：内圈取最大值，减去本点的覆盖度就是一道贴着轮廓的细边；外圈取平均，成一层往外淡开的光晕。
/// 输出预乘颜色，alpha 只给一部分，光晕就带点加色的亮。
const char * const highlightShaderSource = R"HIGHLIGHTMSL(
#include <metal_stdlib>
using namespace metal;

struct Params
{
	float4 texel; // xy：画布一个像素在纹理坐标里多大；z：细边的半径（像素）；w：光晕的半径（像素）
	float4 tone;  // rgb：描边颜色；a：强度（含淡入和一明一暗）
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

fragment main0_out main0(main0_in in [[stage_in]], constant Params & P [[buffer(0)]],
	texture2d<float> mainTex [[texture(0)]], sampler mainSmp [[sampler(0)]])
{
	float a0 = mainTex.sample(mainSmp, in.uv).a;
	float inner = 0.0;
	float outer = 0.0;
	for(int k = 0; k < 12; ++k)
	{
		float angle = float(k) * (6.2831853 / 12.0);
		float2 dir = float2(cos(angle), sin(angle)) * P.texel.xy;
		inner = max(inner, mainTex.sample(mainSmp, in.uv + dir * P.texel.z).a);
		outer += mainTex.sample(mainSmp, in.uv + dir * P.texel.w).a;
	}
	outer *= 1.0 / 12.0;
	float line = saturate((inner - a0) * 1.4);
	float glow = saturate(outer * 1.6 - a0) * (1.0 - line);
	float alpha = saturate(line * 0.9 + glow * 0.35) * P.tone.a;

	main0_out out;
	out.color = float4(P.tone.rgb * alpha, alpha * 0.65);
	return out;
}
)HIGHLIGHTMSL";

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
	float terrain[4];
	float atlas[4];
	float cellMean[MapLayerCompositor::materialSets * MapLayerCompositor::materialsPerSet][4];
	float night[4];
	float weather[4];
	float lightInfo[4];
	float lightPos[MapLayerCompositor::maxLights][4];
	float lightColor[MapLayerCompositor::maxLights][4];
	float lightGrid[4];
	uint32_t lightCells[MapLayerCompositor::lightGridSize * MapLayerCompositor::lightGridSize];
	float printBox[4];
	float prints[MapLayerCompositor::maxFootprints][4];
};

/// 与长投影预处理着色器的 Params 一一对应
struct ShadowUniforms
{
	float cache[4];
	float window[4];
	float sun[4];
};

/// 与云雾着色器的 Params 一一对应
struct FogUniforms
{
	float cache[4];
	float world[4];
	float misc[4];
	float sun[4];
	float grade[4];
	float style[4];
};

/// 云雾着色器：主纹理（覆盖层）加云雾场、噪声
constexpr Uint32 fogSamplerCount = 3;

/// 与辉光着色器的 Params 一一对应
struct BloomUniforms
{
	float texel[4];
};

/// 与后处理着色器的 Params 一一对应
struct PostUniforms
{
	float frame[4];
	float bounds[4];
	float map[4];
	float world[4];
	float tune[4];
};

/// 与悬停描边着色器的 Params 一一对应
struct HighlightUniforms
{
	float texel[4];
	float tone[4];
};

/// 后处理着色器：主纹理（离屏图）加两级辉光图、噪声
constexpr Uint32 postSamplerCount = 4;

/// 后处理的强度（2026-11-18 初版，实机调）：辉光、统一色调、宣纸纹理、暗角与边缘墨晕
constexpr float bloomStrength = 1.0f;
constexpr float gradeStrength = 0.85f;
/// 修仙建筑（起伏层记号，见着色器 vividMark、buildingMark）不受统一色调影响的程度：1 为完全保住原色（2026-11-18 用户要求建筑不被调色罩灰）。
/// 2026-11-19 用户又嫌建筑像贴上去的：整张图都收艳、压墨青，只有建筑不变，就和周围不在一个色调里。第一版降到一半，
/// 第二版建筑另做明度匹配（着色器 matchBuildingTone），统一色调也照常吃，降到 0。资源与宝物照旧全保
constexpr float vividStrength = 0.0f;
constexpr float paperStrength = 1.0f;
constexpr float frameStrength = 1.0f;

/// 悬停描边在画布上的半径（原版像素）：细边、光晕。地图缓存给画布四周留的边要比光晕宽
constexpr float highlightLineRadius = 1.5f;
constexpr float highlightGlowRadius = 4.0f;

/// 着色器声明的取样器数：SDL 的主纹理加本类绑定的十四张（含离岸距离图、摇摆层、风场遮罩、起伏层、光照图、
/// 地形分布层、材质图集、地形场）。Metal 每个阶段最多 16 个取样器，离岸距离图因此兼存雾云场，没有另占一个
constexpr Uint32 shaderSamplerCount = 15;

/// 一格材质在地图上铺多大（图块）：图集一格 768 像素、内容 752 像素，铺 4.8 个图块（约 157 像素一块，比缓存的 96 像素细，
/// 草丛、石子与树、英雄的比例才对）
constexpr float materialCellTiles = 4.8f;
/// 屏幕上一个图块小于这么多像素时，地表材质淡出、退回图块原画（没有多级纹理，缩得太小会闪烁）
constexpr float materialFadeStart = 22.0f;
constexpr float materialFadeEnd = 40.0f;
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

/// 屏幕中心的地图坐标（云层视差用）：第一块区域正是可见窗口左上角图块所在的那一块，从它的屏幕位置推算
std::array<float, 2> screenCenterTile(const MapLayerCompositor::Inputs & inputs, const std::vector<PresentedRegion> & regions, float cacheWidth)
{
	std::array<float, 2> center = {static_cast<float>(inputs.worldOrigin.x), static_cast<float>(inputs.worldOrigin.y)};
	if(regions.empty() || regions.front().target.w <= 0 || inputs.slots.x <= 0)
		return center;

	Rect bounds = regions.front().target;
	for(const PresentedRegion & region : regions)
		bounds = bounds.include(region.target);
	const float cachePerScreen = static_cast<float>(regions.front().source.w) / static_cast<float>(regions.front().target.w);
	const float tileSize = cacheWidth / static_cast<float>(inputs.slots.x);
	const Point offset = bounds.center() - regions.front().target.topLeft();
	center[0] += static_cast<float>(offset.x) * cachePerScreen / tileSize;
	center[1] += static_cast<float>(offset.y) * cachePerScreen / tileSize;
	return center;
}

/// 屏幕上一个图块多少像素（屏幕像素，含缩放倍数）
float tilePixelsOnScreen(const MapLayerCompositor::Inputs & inputs, const std::vector<PresentedRegion> & regions, float cacheWidth)
{
	if(regions.empty() || regions.front().source.w <= 0 || inputs.slots.x <= 0)
		return 32.0f;
	return static_cast<float>(regions.front().target.w) / static_cast<float>(regions.front().source.w) * cacheWidth / static_cast<float>(inputs.slots.x);
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
		terrainField.texture = nullptr;
		terrainField.dirty = true;
		materialAtlas = nullptr;
		materialsDirty = true;
		maskLevels = {};
		maskLevelsSourceSize = Point();
		shadowShader = nullptr;
		shadowState = nullptr;
		reliefLevels = {};
		lightMap = nullptr;
		reliefLevelsSourceSize = Point();
		boundTextures = {};
		fogShader = nullptr;
		fogState = nullptr;
		fogField.texture = nullptr;
		fogField.dirty = true;
		fogBoundTextures = {};
		frameTexture = nullptr;
		frameSize = Point();
		frameLogicalSize = Point();
		bloomLevels = {};
		bloomShader = nullptr;
		postShader = nullptr;
		highlightShader = nullptr;
		extractState = nullptr;
		blurState = nullptr;
		postState = nullptr;
		highlightState = nullptr;
		postBoundTextures = {};
		framePrepared = false;
		postUnavailable = false;
		shaderUnavailable = false;
		owner = renderer;
	}

	if(shaderUnavailable)
		return false;

	if(shader && shadowShader && fogShader && sampler && clampSampler && noise)
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

	shaderInfo.code = reinterpret_cast<const Uint8 *>(fogShaderSource);
	shaderInfo.code_size = std::strlen(fogShaderSource);
	shaderInfo.num_samplers = fogSamplerCount;
	fogShader = SDL_CreateGPUShader(device, &shaderInfo);

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
		// 粒子要在 CPU 上算阵雨落在哪，与着色器用同一张噪声
		particles.setNoise(pixels, noiseSize);
	}

	if(!shader || !shadowShader || !fogShader || !sampler || !clampSampler || !noise || !gpuTextureOf(noise))
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

void MapLayerCompositor::advanceDayClock()
{
	const uint64_t ticks = SDL_GetTicks();
	const float elapsed = dayClockTicks == 0 ? 0.0f : std::clamp(static_cast<float>(ticks - dayClockTicks) / 1000.0f, 0.0f, 0.5f);
	dayClockTicks = ticks;

	const std::string clock = settings["video"]["mapSunClock"].String();
	if(clock == "fixed")
	{
		dayPhase = std::fmod(std::clamp(static_cast<float>(settings["video"]["mapSunTime"].Float()), 0.0f, dayCycle), dayCycle);
		return;
	}
	if(clock == "cycle")
	{
		// 开局在上午
		const double seconds = std::max(1.0, settings["video"]["mapSunCycle"].Float());
		dayPhase = static_cast<float>(std::fmod(0.2 + static_cast<double>(ticks) / 1000.0 / seconds * dayCycle, static_cast<double>(dayCycle)));
		return;
	}

	// 跟着回合走：本方进度 0~1 对应刚日出（0.06）到太阳贴着地平线（0.96，最橙的时候）；不在本方回合是半夜
	float target = 1.25f;
	if(dayProgress)
		target = 0.06f + 0.9f * std::clamp(dayProgressMax, 0.0f, 1.0f);

	if(dayPhase < 0.0f)
	{
		// 刚进地图（开局、读档）：直接是此刻的天色，不从别的时段转过来
		dayPhase = target;
		return;
	}

	// 只往前走：差得多时走得快（入夜、天亮约一两秒），英雄每走一步太阳挪一点，平滑地追上
	const float ahead = std::fmod(target - dayPhase + dayCycle, dayCycle);
	if(ahead > dayCycle - 0.02f)
		return; // 目标在身后一点点（取整误差），不必绕一整天
	const float step = std::min(ahead, std::max(ahead * 2.0f, 0.06f) * elapsed);
	dayPhase = std::fmod(dayPhase + step, dayCycle);
}

MapLayerCompositor::Sun MapLayerCompositor::sunAt(float phase)
{
	constexpr float pi = 3.14159265f;

	// 白天的太阳只看 0~1 这一段：夜里太阳停在日落的位置，反正影子、打光都已淡出
	const float time = std::clamp(phase, 0.0f, 1.0f);
	const float day = std::sin(pi * time); // 清晨、傍晚为 0，正午为 1
	const float warm = std::pow(1.0f - day, 1.5f);
	const float fade = std::clamp(time / 0.04f, 0.0f, 1.0f) * std::clamp((1.0f - time) / 0.04f, 0.0f, 1.0f);
	const bool evening = time > 0.5f;

	// 太阳的高度（正为地平线上，负为地平线下）：天黑的程度、暮色都看它
	const float elevationSign = phase <= 1.0f ? day : -std::sin(pi * (phase - 1.0f) / (dayCycle - 1.0f));

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
	auto smooth = [](float edge0, float edge1, float value)
	{
		const float k = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
		return k * k * (3.0f - 2.0f * k);
	};

	// 天黑的程度：太阳落到地平线下一点就开始暗，再往下是夜里
	const float night = smooth(0.08f, -0.45f, elevationSign);
	// 暮色：太阳在地平线附近那一小段（日落后、日出前），天光偏紫蓝
	const float twilight = std::exp(-std::pow((elevationSign + 0.12f) / 0.13f, 2.0f));

	Sun result;
	result.direction = direction;
	result.shadow = {-std::cos(azimuth) * shadowLength, -std::sin(azimuth) * shadowLength};
	// 清晨的光淡金，黄昏的光橙红（2026-11-18：清晨偏暖、黄昏偏橙）
	result.color = mixColor({1.0f, 0.98f, 0.95f}, evening ? std::array<float, 3>{1.0f, 0.74f, 0.48f} : std::array<float, 3>{1.0f, 0.85f, 0.66f}, warm);
	result.shade = mixColor({0.80f, 0.86f, 1.0f}, {0.70f, 0.74f, 0.98f}, warm);
	std::array<float, 3> grade = mixColor({1.0f, 1.0f, 1.0f}, evening ? std::array<float, 3>{1.10f, 0.91f, 0.74f} : std::array<float, 3>{1.05f, 0.98f, 0.90f}, warm * (evening ? 0.75f : 0.55f));
	grade = mixColor(grade, {0.80f, 0.74f, 0.94f}, twilight * 0.55f);
	// 夜里：偏蓝、压暗，但地图上的东西还要看得清（等别家行动时也要看对手的英雄走到哪）
	result.grade = mixColor(grade, {0.40f, 0.48f, 0.70f}, night);
	result.relight = 0.85f * fade;
	result.rim = 0.25f + 0.45f * warm;
	result.strength = 0.8f * fade;
	result.reach = reliefHeightRange;
	result.night = night;
	result.lamps = std::clamp(0.08f + 0.92f * night + 0.3f * warm * fade, 0.0f, 1.0f);
	result.desaturate = 0.5f * night;
	result.heat = std::pow(day, 1.2f) * (1.0f - night);
	result.cloudShadow = fade * (0.45f + 0.55f * day);
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

void MapLayerCompositor::prepare(SDL_Renderer * renderer, const Inputs & inputs, const std::vector<PresentedRegion> & regions)
{
	framePrepared = false;
	if(!inputs.waterMask || !ensureShaderResources(renderer) || !ensureMaskLevels(renderer, inputs.waterMask))
		return;

	advanceDayClock();
	sun = sunAt(dayPhase);

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

	// 离屏图的像素数与窗口上逻辑画面实际占的像素相同，坐标仍用窗口的逻辑坐标（离屏图设同样的逻辑分辨率）。
	// 不能按逻辑尺寸建：逻辑尺寸含界面放大倍数（如 xbrz3 时 3081 × 1803），比窗口实际的像素多好几倍，
	// 合成着色器跟着多算好几倍，帧率从 119 掉到 64（2026-11-18 实测，M1 Pro）。要在切走渲染目标之前问
	SDL_SetRenderTarget(renderer, previousTarget);
	int logicalWidth = 0;
	int logicalHeight = 0;
	SDL_RendererLogicalPresentation presentation = SDL_LOGICAL_PRESENTATION_DISABLED;
	SDL_GetRenderLogicalPresentation(renderer, &logicalWidth, &logicalHeight, &presentation);
	SDL_FRect presented = {0.0f, 0.0f, 0.0f, 0.0f};
	SDL_GetRenderLogicalPresentationRect(renderer, &presented);
	if(logicalWidth <= 0 || logicalHeight <= 0)
	{
		SDL_GetCurrentRenderOutputSize(renderer, &logicalWidth, &logicalHeight);
		presented.w = static_cast<float>(logicalWidth);
		presented.h = static_cast<float>(logicalHeight);
	}
	// Retina 窗口（2026-11-20）：离屏图按窗口的点数建，不按物理像素——按像素建要多算 4 倍，
	// 同一场景实测 57 → 35 FPS（当时整机 GPU 已被 WindowServer 占满）。后处理那一遍仍画到全分辨率窗口、
	// 线性放大离屏图，地图清晰度与开高像素密度之前相同，界面与文字则按物理像素画
	float density = SDL_GetWindowPixelDensity(SDL_GetRenderWindow(renderer));
	if(density < 1.0f)
		density = 1.0f;
	const Point pixels(std::max(1, static_cast<int>(std::lround(presented.w / density))), std::max(1, static_cast<int>(std::lround(presented.h / density))));

	if(!regions.empty() && ensurePostResources(renderer, pixels, Point(logicalWidth, logicalHeight)))
		renderFrame(renderer, inputs, regions);

	SDL_SetRenderTarget(renderer, previousTarget);
}

bool MapLayerCompositor::shaderReady(SDL_Renderer * renderer, const Inputs & inputs)
{
	return ensureShaderResources(renderer) && maskLevels.back() && maskLevelsSourceSize.x > 0 && lightMap
		&& ensureFieldTexture(renderer, depthField, {128, 0, 0, 255}, "sea depth")
		&& ensureFieldTexture(renderer, terrainField, {255, 0, 0, 0}, "terrain")
		&& ensureMaterialAtlas(renderer)
		&& inputs.splat
		&& ensureRenderState(renderer, inputs);
}

void MapLayerCompositor::destroyPostTargets()
{
	if(frameTexture)
		SDL_DestroyTexture(frameTexture);
	frameTexture = nullptr;
	for(SDL_Texture *& level : bloomLevels)
	{
		if(level)
			SDL_DestroyTexture(level);
		level = nullptr;
	}
	frameSize = Point();
	frameLogicalSize = Point();
	framePrepared = false;
}

bool MapLayerCompositor::ensurePostResources(SDL_Renderer * renderer, const Point & size, const Point & logicalSize)
{
	if(postUnavailable || shaderUnavailable || size.x <= 0 || size.y <= 0 || logicalSize.x <= 0 || logicalSize.y <= 0)
		return false;

	if(!bloomShader || !postShader || !highlightShader || !extractState || !blurState || !highlightState)
	{
		SDL_GPUDevice * device = deviceOf(renderer);

		SDL_GPUShaderCreateInfo shaderInfo;
		SDL_zero(shaderInfo);
		shaderInfo.entrypoint = "main0";
		shaderInfo.format = SDL_GPU_SHADERFORMAT_MSL;
		shaderInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
		shaderInfo.num_uniform_buffers = 1;

		shaderInfo.code = reinterpret_cast<const Uint8 *>(bloomShaderSource);
		shaderInfo.code_size = std::strlen(bloomShaderSource);
		shaderInfo.num_samplers = 1;
		bloomShader = SDL_CreateGPUShader(device, &shaderInfo);

		shaderInfo.code = reinterpret_cast<const Uint8 *>(postShaderSource);
		shaderInfo.code_size = std::strlen(postShaderSource);
		shaderInfo.num_samplers = postSamplerCount;
		postShader = SDL_CreateGPUShader(device, &shaderInfo);

		shaderInfo.code = reinterpret_cast<const Uint8 *>(highlightShaderSource);
		shaderInfo.code_size = std::strlen(highlightShaderSource);
		shaderInfo.num_samplers = 1;
		highlightShader = SDL_CreateGPUShader(device, &shaderInfo);

		// 取自发光、模糊、描边只用 SDL 自己绑的主纹理，渲染状态建一次就够
		SDL_GPURenderStateCreateInfo stateInfo;
		SDL_zero(stateInfo);
		if(bloomShader)
		{
			stateInfo.fragment_shader = bloomShader;
			extractState = SDL_CreateGPURenderState(renderer, &stateInfo);
			blurState = SDL_CreateGPURenderState(renderer, &stateInfo);
		}
		if(highlightShader)
		{
			stateInfo.fragment_shader = highlightShader;
			highlightState = SDL_CreateGPURenderState(renderer, &stateInfo);
		}

		// 颜色照常按预乘 alpha 叠；alpha（自发光）只乘上 1 − 源 alpha：被粒子、云遮住多少就暗多少，不会因此发光
		emissionKeepingBlend = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD,
			SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD);

		if(!bloomShader || !postShader || !highlightShader || !extractState || !blurState || !highlightState)
		{
			logGlobal->error("Failed to set up the map post-processing shaders: %s - drawing the map without bloom and paper", SDL_GetError());
			postUnavailable = true;
			return false;
		}
	}

	if(!frameTexture || frameSize != size || frameLogicalSize != logicalSize)
	{
		destroyPostTargets();
		const Point nearSize((size.x + 3) / 4, (size.y + 3) / 4);
		const Point farSize((nearSize.x + 3) / 4, (nearSize.y + 3) / 4);
		frameTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, size.x, size.y);
		bloomLevels[0] = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, nearSize.x, nearSize.y);
		bloomLevels[1] = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, farSize.x, farSize.y);
		if(!frameTexture || !bloomLevels[0] || !bloomLevels[1])
		{
			logGlobal->error("Failed to create the %dx%d map frame for post-processing: %s - drawing the map without it", size.x, size.y, SDL_GetError());
			destroyPostTargets();
			postUnavailable = true;
			return false;
		}
		for(SDL_Texture * texture : {frameTexture, bloomLevels[0], bloomLevels[1]})
		{
			SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
			SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE);
		}
		frameSize = size;
		frameLogicalSize = logicalSize;
		logGlobal->info("Map post-processing frame ready (%dx%d pixels for %dx%d logical)", size.x, size.y, logicalSize.x, logicalSize.y);
	}

	const std::array<SDL_Texture *, 3> wanted = {bloomLevels[0], bloomLevels[1], noise};
	if(postState && postBoundTextures == wanted)
		return true;

	if(postState)
		SDL_DestroyGPURenderState(postState);
	postState = nullptr;
	postBoundTextures = {};

	// 顺序与后处理着色器的 texture(1)~texture(3) 对应：两级辉光图不环绕，噪声可平铺
	std::array<SDL_GPUTextureSamplerBinding, postSamplerCount - 1> bindings = {};
	for(size_t i = 0; i < wanted.size(); ++i)
	{
		bindings[i].texture = gpuTextureOf(wanted[i]);
		bindings[i].sampler = wanted[i] == noise ? sampler : clampSampler;
		if(!bindings[i].texture || !bindings[i].sampler)
			return false;
	}

	SDL_GPURenderStateCreateInfo stateInfo;
	SDL_zero(stateInfo);
	stateInfo.fragment_shader = postShader;
	stateInfo.num_sampler_bindings = static_cast<Sint32>(bindings.size());
	stateInfo.sampler_bindings = bindings.data();
	postState = SDL_CreateGPURenderState(renderer, &stateInfo);
	if(!postState)
	{
		logGlobal->error("Failed to create the map post-processing render state: %s - drawing the map without it", SDL_GetError());
		postUnavailable = true;
		return false;
	}
	postBoundTextures = wanted;
	return true;
}

void MapLayerCompositor::renderFrame(SDL_Renderer * renderer, const Inputs & inputs, const std::vector<PresentedRegion> & regions)
{
	if(!shaderReady(renderer, inputs))
		return;

	Rect bounds = regions.front().target;
	for(const PresentedRegion & region : regions)
		bounds = bounds.include(region.target);
	bounds = bounds.intersect(Rect(Point(0, 0), frameLogicalSize));
	if(bounds.w <= 0 || bounds.h <= 0)
		return;
	frameBounds = bounds;
	frameView = inputs.viewArea.w > 0 && inputs.viewArea.h > 0 ? inputs.viewArea.intersect(bounds) : bounds;
	if(frameView.w <= 0 || frameView.h <= 0)
		frameView = bounds;

	// 1. 整幅地图画进离屏图：地面与物件（alpha 是自发光）、粒子、悬停描边、云雾与覆盖层
	SDL_SetRenderTarget(renderer, frameTexture);
	// 每个渲染目标有自己的逻辑分辨率：与窗口相同，地图照旧按窗口的逻辑坐标画（含粒子的裁剪、视口）
	SDL_SetRenderLogicalPresentation(renderer, frameLogicalSize.x, frameLogicalSize.y, SDL_LOGICAL_PRESENTATION_STRETCH);
	SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
	SDL_RenderClear(renderer);

	composeWithShader(renderer, inputs, regions, emissionKeepingBlend);
	drawHighlight(renderer, inputs, regions);
	drawOverlay(renderer, inputs, regions, emissionKeepingBlend);

	// 2. 取自发光缩成 1/4，再模糊缩成 1/16。虽然整张都会被覆盖，仍要先清：清过的目标 SDL 可以换一块新纹理来画；
	//    不清就只能接着上一帧那块画，得等上一帧画到窗口时读完它才能开画，每帧都和显示同步卡一下，帧率减半（2026-11-18 实测 120 → 64）
	float nearWidth = 0;
	float nearHeight = 0;
	SDL_GetTextureSize(bloomLevels[0], &nearWidth, &nearHeight);

	SDL_SetTextureBlendMode(frameTexture, SDL_BLENDMODE_NONE);
	const BloomUniforms extract = {{1.0f / static_cast<float>(frameSize.x), 1.0f / static_cast<float>(frameSize.y), 0.0f, 0.0f}};
	SDL_SetRenderTarget(renderer, bloomLevels[0]);
	SDL_RenderClear(renderer);
	SDL_SetGPURenderStateFragmentUniforms(extractState, 0, &extract, sizeof(extract));
	SDL_SetGPURenderState(renderer, extractState);
	SDL_RenderTexture(renderer, frameTexture, nullptr, nullptr);
	SDL_SetGPURenderState(renderer, nullptr);

	const BloomUniforms blur = {{1.0f / nearWidth, 1.0f / nearHeight, 1.0f, 0.0f}};
	SDL_SetRenderTarget(renderer, bloomLevels[1]);
	SDL_RenderClear(renderer);
	SDL_SetGPURenderStateFragmentUniforms(blurState, 0, &blur, sizeof(blur));
	SDL_SetGPURenderState(renderer, blurState);
	SDL_RenderTexture(renderer, bloomLevels[0], nullptr, nullptr);
	SDL_SetGPURenderState(renderer, nullptr);

	framePrepared = true;
}

void MapLayerCompositor::drawPost(SDL_Renderer * renderer, const Inputs & inputs, const std::vector<PresentedRegion> & regions)
{
	float cacheWidth = 0;
	float cacheHeight = 0;
	SDL_GetTextureSize(inputs.ground, &cacheWidth, &cacheHeight);
	float nearWidth = 0;
	float nearHeight = 0;
	SDL_GetTextureSize(bloomLevels[0], &nearWidth, &nearHeight);
	float farWidth = 0;
	float farHeight = 0;
	SDL_GetTextureSize(bloomLevels[1], &farWidth, &farHeight);

	PostUniforms uniforms = {};
	uniforms.frame[0] = static_cast<float>(frameSize.x);
	uniforms.frame[1] = static_cast<float>(frameSize.y);
	uniforms.frame[2] = nearWidth;
	uniforms.frame[3] = nearHeight;
	// 区域、视图矩形都在窗口的逻辑坐标里，着色器按离屏图的像素算
	const float scaleX = static_cast<float>(frameSize.x) / static_cast<float>(frameLogicalSize.x);
	const float scaleY = static_cast<float>(frameSize.y) / static_cast<float>(frameLogicalSize.y);
	uniforms.bounds[0] = static_cast<float>(frameView.x) * scaleX;
	uniforms.bounds[1] = static_cast<float>(frameView.y) * scaleY;
	uniforms.bounds[2] = static_cast<float>(frameView.w) * scaleX;
	uniforms.bounds[3] = static_cast<float>(frameView.h) * scaleY;
	// 地图坐标到离屏图：第一块区域就是可见窗口左上角图块起的那一片（与 MapParticles 的换算相同）
	uniforms.map[0] = static_cast<float>(regions.front().target.x) * scaleX;
	uniforms.map[1] = static_cast<float>(regions.front().target.y) * scaleY;
	uniforms.map[2] = tilePixelsOnScreen(inputs, regions, cacheWidth) * scaleX;
	uniforms.map[3] = farWidth;
	uniforms.world[0] = static_cast<float>(inputs.worldOrigin.x);
	uniforms.world[1] = static_cast<float>(inputs.worldOrigin.y);
	uniforms.world[2] = farHeight;
	uniforms.world[3] = sun.night;
	uniforms.tune[0] = bloomStrength;
	uniforms.tune[1] = gradeStrength;
	uniforms.tune[2] = paperStrength;
	uniforms.tune[3] = frameStrength;

	SDL_SetTextureBlendMode(frameTexture, SDL_BLENDMODE_NONE);
	SDL_SetGPURenderStateFragmentUniforms(postState, 0, &uniforms, sizeof(uniforms));
	SDL_SetGPURenderState(renderer, postState);
	const SDL_FRect to = CSDL_Ext::toSDLFloat(frameBounds);
	const SDL_FRect from = {to.x * scaleX, to.y * scaleY, to.w * scaleX, to.h * scaleY};
	if(!SDL_RenderTexture(renderer, frameTexture, &from, &to))
		logGlobal->error("Failed to draw the post-processed map: %s", SDL_GetError());
	SDL_SetGPURenderState(renderer, nullptr);
}

void MapLayerCompositor::drawHighlight(SDL_Renderer * renderer, const Inputs & inputs, const std::vector<PresentedRegion> & regions)
{
	const MapHighlight & highlight = effects.highlight;
	if(!highlight.canvas || highlight.strength <= 0.001f || highlight.width <= 0.0f || highlight.height <= 0.0f || regions.empty())
		return;
	SDL_Texture * texture = highlight.canvas->getRenderTargetTexture();
	if(!texture)
		return;

	float cacheWidth = 0;
	float cacheHeight = 0;
	SDL_GetTextureSize(inputs.ground, &cacheWidth, &cacheHeight);
	const float tilePixels = tilePixelsOnScreen(inputs, regions, cacheWidth);
	const PresentedRegion & first = regions.front();

	SDL_FRect to;
	to.x = static_cast<float>(first.target.x) + (highlight.x - static_cast<float>(inputs.worldOrigin.x)) * tilePixels;
	to.y = static_cast<float>(first.target.y) + (highlight.y - static_cast<float>(inputs.worldOrigin.y)) * tilePixels;
	to.w = highlight.width * tilePixels;
	to.h = highlight.height * tilePixels;

	float textureWidth = 0;
	float textureHeight = 0;
	SDL_GetTextureSize(texture, &textureWidth, &textureHeight);
	// 画布按原版像素画（一个图块 32 像素），纹理可能按界面缩放倍数放大过
	const float texelsPerPixel = textureWidth / (highlight.width * 32.0f);

	// 自己的偏青（玉色）、中立的宝物建筑偏金、敌人朱砂，慢慢一明一暗
	std::array<float, 3> tone = {1.0f, 0.84f, 0.50f};
	if(highlight.tone == MapHighlight::Tone::OWN)
		tone = {0.56f, 0.92f, 0.80f};
	else if(highlight.tone == MapHighlight::Tone::ENEMY)
		tone = {0.96f, 0.38f, 0.26f};
	const float pulse = 0.82f + 0.18f * std::sin(shaderTime() * 3.2f);

	HighlightUniforms uniforms = {};
	uniforms.texel[0] = 1.0f / textureWidth;
	uniforms.texel[1] = 1.0f / textureHeight;
	uniforms.texel[2] = highlightLineRadius * texelsPerPixel;
	uniforms.texel[3] = highlightGlowRadius * texelsPerPixel;
	uniforms.tone[0] = tone[0];
	uniforms.tone[1] = tone[1];
	uniforms.tone[2] = tone[2];
	uniforms.tone[3] = std::clamp(highlight.strength, 0.0f, 1.0f) * pulse;

	SDL_SetTextureBlendMode(texture, emissionKeepingBlend);
	SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
	SDL_SetGPURenderStateFragmentUniforms(highlightState, 0, &uniforms, sizeof(uniforms));
	SDL_SetGPURenderState(renderer, highlightState);
	if(!SDL_RenderTexture(renderer, texture, nullptr, &to))
		logGlobal->error("Failed to draw the map highlight: %s", SDL_GetError());
	SDL_SetGPURenderState(renderer, nullptr);
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

void MapLayerCompositor::setTerrainField(const std::vector<uint8_t> & rgba, const Point & size)
{
	terrainField.rgba = rgba;
	terrainField.size = size;
	terrainField.dirty = true;
	particles.setTerrainField(rgba, size);
}

void MapLayerCompositor::setDayProgress(int day, std::optional<float> progress)
{
	// 新的一天从早上重新算；同一天里只往前走（英雄途中补了行动力，天也不会倒回早上）
	if(day != dayNumber)
		dayProgressMax = 0.0f;
	dayNumber = day;
	dayProgress = progress;
	if(progress)
		dayProgressMax = std::max(dayProgressMax, *progress);
}

void MapLayerCompositor::setObjectEffects(const MapObjectEffects & newEffects)
{
	effects = newEffects;
}

void MapLayerCompositor::setTerrainMaterials(const std::vector<uint8_t> & png)
{
	materialsPng = png;
	materialsDirty = true;
}

bool MapLayerCompositor::ensureMaterialAtlas(SDL_Renderer * renderer)
{
	if(materialAtlas && !materialsDirty)
		return true;

	if(materialAtlas)
		SDL_DestroyTexture(materialAtlas);
	materialAtlas = nullptr;
	materialsLoaded = false;
	materialsDirty = false;
	inputsForgotten = true; // 新纹理可能分到旧纹理的地址

	SDL_Surface * decoded = nullptr;
	if(!materialsPng.empty())
	{
		SDL_Surface * raw = IMG_Load_IO(SDL_IOFromConstMem(materialsPng.data(), materialsPng.size()), true);
		if(raw)
		{
			decoded = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
			SDL_DestroySurface(raw);
		}
		if(!decoded)
			logGlobal->error("Failed to decode the adventure map terrain materials: %s", SDL_GetError());
	}

	// 图集必须是 4 列 × 8~11 行的正方形格子（第 9、10 行是特殊地面的材质，第 11 行是海面，都可以没有）
	const int atlasRows = decoded && decoded->w >= materialsPerSet ? decoded->h / (decoded->w / materialsPerSet) : 0;
	if(decoded && (decoded->w % materialsPerSet != 0 || decoded->w / materialsPerSet * atlasRows != decoded->h
		|| atlasRows < baseMaterialSets || atlasRows > materialSets))
	{
		logGlobal->error("Adventure map terrain materials are %dx%d, expected 4 columns by 8 to 11 rows of square cells - not used", decoded->w, decoded->h);
		SDL_DestroySurface(decoded);
		decoded = nullptr;
	}

	if(decoded)
	{
		materialAtlas = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, decoded->w, decoded->h);
		if(materialAtlas)
		{
			SDL_UpdateTexture(materialAtlas, nullptr, decoded->pixels, decoded->pitch);
			materialAtlasSize = Point(decoded->w, decoded->h);
			materialCell = decoded->w / materialsPerSet;

			// 每格的平均颜色与高度（隔几个像素取一个就够）
			for(int set = 0; set < atlasRows; ++set)
			{
				for(int sub = 0; sub < materialsPerSet; ++sub)
				{
					std::array<double, 4> sum = {};
					int count = 0;
					for(int y = set * materialCell; y < (set + 1) * materialCell; y += 4)
					{
						const auto * row = static_cast<const uint8_t *>(decoded->pixels) + static_cast<size_t>(y) * decoded->pitch;
						for(int x = sub * materialCell; x < (sub + 1) * materialCell; x += 4)
						{
							for(size_t c = 0; c < 4; ++c)
								sum[c] += row[x * 4 + c];
							++count;
						}
					}
					for(size_t c = 0; c < 4; ++c)
						materialMeans[set * materialsPerSet + sub][c] = static_cast<float>(sum[c] / std::max(count, 1) / 255.0);
				}
			}
			materialsLoaded = true;
			logGlobal->info("Adventure map terrain materials ready (%dx%d)", decoded->w, decoded->h);
		}
		SDL_DestroySurface(decoded);
	}

	if(!materialAtlas)
	{
		// 占位：着色器照样绑一张纹理，地表材质强度为 0
		materialAtlas = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, 1, 1);
		const uint8_t pixel[4] = {0, 0, 0, 0};
		if(materialAtlas)
			SDL_UpdateTexture(materialAtlas, nullptr, pixel, 4);
	}
	if(materialAtlas)
		SDL_SetTextureScaleMode(materialAtlas, SDL_SCALEMODE_LINEAR);
	return materialAtlas != nullptr;
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
		depthField.rgba[i + 3] = rgba[i + 3];
	}
	depthField.dirty = true;
}

void MapLayerCompositor::setUnderground(bool value)
{
	underground = value;
}

void MapLayerCompositor::setFogField(const std::vector<uint8_t> & rgba, const Point & size)
{
	fogField.rgba = rgba;
	fogField.size = size;
	fogField.dirty = true;
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
		fogInputsForgotten = true;
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
	fogInputsForgotten = true;
	particles.forgetInputs();
}

bool MapLayerCompositor::ensureRenderState(SDL_Renderer * renderer, const Inputs & inputs)
{
	const std::array<SDL_Texture *, 13> wanted = {inputs.ground, inputs.objects, inputs.sway, inputs.waterMask, inputs.windMask, maskLevels[2], maskLevels[3], depthField.texture, inputs.relief, lightMap, inputs.splat, materialAtlas, terrainField.texture};

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
	const std::array<SDL_Texture *, 14> textures = {inputs.ground, inputs.objects, inputs.waterMask, noise, maskLevels[2], maskLevels[3], depthField.texture, inputs.sway, inputs.windMask, inputs.relief, lightMap, inputs.splat, materialAtlas, terrainField.texture};
	// 绑定数与纹理数一致（2026-11-17 曾只改了纹理表、绑定数还是 11：后三张没绑上，循环还写出了数组）
	std::array<SDL_GPUTextureSamplerBinding, std::tuple_size_v<decltype(textures)>> bindings = {};
	static_assert(std::tuple_size_v<decltype(textures)> + 1 == shaderSamplerCount, "纹理数要与着色器声明的取样器数（含 SDL 的主纹理）一致");

	for(size_t i = 0; i < textures.size(); ++i)
	{
		bindings[i].texture = textures[i] ? gpuTextureOf(textures[i]) : nullptr;
		// 离岸距离图、地形场按地图坐标取样、不环绕；材质图集在格子里自己环绕（四周有环绕的边），也用夹边
		const bool clamped = textures[i] == depthField.texture || textures[i] == terrainField.texture || textures[i] == materialAtlas;
		bindings[i].sampler = clamped ? clampSampler : sampler;
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
	// prepare() 已把整幅地图画进离屏图：经后处理画到窗口上
	if(framePrepared && frameTexture && postState && !regions.empty())
	{
		drawPost(renderer, inputs, regions);
		framePrepared = false;
		return;
	}
	framePrepared = false;

	if(shaderReady(renderer, inputs))
		composeWithShader(renderer, inputs, regions, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
	else
		composeLayerByLayer(renderer, inputs, regions, scaleMode);

	// 覆盖层最后叠上，盖住粒子；云雾着色器只要着色器资源在就能画，合成着色器暂时没就绪（遮罩小图还没建好之类）时也画云
	drawOverlay(renderer, inputs, regions, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
}

bool MapLayerCompositor::ensureFogState(SDL_Renderer * renderer)
{
	const std::array<SDL_Texture *, 2> wanted = {fogField.texture, noise};
	const bool forgotten = fogInputsForgotten.exchange(false);
	if(fogState && fogBoundTextures == wanted && !forgotten)
		return true;

	if(fogState)
	{
		SDL_DestroyGPURenderState(fogState);
		fogState = nullptr;
	}
	fogBoundTextures = {};

	// 顺序与着色器的 texture(1)、texture(2) 对应：云雾场按地图坐标取样、不环绕；噪声可平铺、环绕
	std::array<SDL_GPUTextureSamplerBinding, fogSamplerCount - 1> bindings = {};
	bindings[0].texture = fogField.texture ? gpuTextureOf(fogField.texture) : nullptr;
	bindings[0].sampler = clampSampler;
	bindings[1].texture = noise ? gpuTextureOf(noise) : nullptr;
	bindings[1].sampler = sampler;
	if(!bindings[0].texture || !bindings[1].texture || !bindings[0].sampler || !bindings[1].sampler)
		return false;

	SDL_GPURenderStateCreateInfo stateInfo;
	SDL_zero(stateInfo);
	stateInfo.fragment_shader = fogShader;
	stateInfo.num_sampler_bindings = static_cast<Sint32>(bindings.size());
	stateInfo.sampler_bindings = bindings.data();
	fogState = SDL_CreateGPURenderState(renderer, &stateInfo);
	if(!fogState)
	{
		// 画不出云，迷雾下的地图会露出来：让屏幕处理器退回原版单层缓存（isShaderBroken）
		logGlobal->error("Failed to create the map fog render state: %s - falling back to the classic map cache", SDL_GetError());
		shaderUnavailable = true;
		return false;
	}

	fogBoundTextures = wanted;
	return true;
}

void MapLayerCompositor::drawOverlay(SDL_Renderer * renderer, const Inputs & inputs, const std::vector<PresentedRegion> & regions, SDL_BlendMode blendMode)
{
	const bool fogReady = !shaderUnavailable && fogShader && noise && clampSampler && sampler
		&& ensureFieldTexture(renderer, fogField, {0, 0, 0, 255}, "fog")
		&& ensureFogState(renderer);

	SDL_SetTextureBlendMode(inputs.overlay, blendMode);
	SDL_SetTextureScaleMode(inputs.overlay, SDL_SCALEMODE_LINEAR);

	if(fogReady)
	{
		float width = 0;
		float height = 0;
		SDL_GetTextureSize(inputs.overlay, &width, &height);

		FogUniforms uniforms = {};
		uniforms.cache[0] = width;
		uniforms.cache[1] = height;
		uniforms.cache[2] = static_cast<float>(inputs.slots.x);
		uniforms.cache[3] = static_cast<float>(inputs.slots.y);
		uniforms.world[0] = static_cast<float>(inputs.worldOrigin.x);
		uniforms.world[1] = static_cast<float>(inputs.worldOrigin.y);
		uniforms.world[2] = static_cast<float>(inputs.slotOrigin.x);
		uniforms.world[3] = static_cast<float>(inputs.slotOrigin.y);
		uniforms.misc[0] = shaderTime();
		uniforms.misc[1] = tilePixelsOnScreen(inputs, regions, width);
		uniforms.misc[2] = static_cast<float>(std::max(1, fogField.size.x));
		uniforms.misc[3] = static_cast<float>(std::max(1, fogField.size.y));
		for(size_t i = 0; i < 3; ++i)
		{
			uniforms.sun[i] = sun.direction[i];
			uniforms.grade[i] = sun.grade[i];
		}
		uniforms.sun[3] = sun.night;
		uniforms.grade[3] = sun.desaturate;
		uniforms.style[0] = underground ? 1.0f : 0.0f;

		SDL_SetGPURenderStateFragmentUniforms(fogState, 0, &uniforms, sizeof(uniforms));
		SDL_SetGPURenderState(renderer, fogState);
	}

	for(const PresentedRegion & region : regions)
	{
		const SDL_FRect from = CSDL_Ext::toSDLFloat(region.source);
		const SDL_FRect to = CSDL_Ext::toSDLFloat(region.target);

		if(!SDL_RenderTexture(renderer, inputs.overlay, &from, &to))
			logGlobal->error("Failed to compose the map overlay: %s", SDL_GetError());
	}

	if(fogReady)
		SDL_SetGPURenderState(renderer, nullptr);
}

void MapLayerCompositor::composeWithShader(SDL_Renderer * renderer, const Inputs & inputs, const std::vector<PresentedRegion> & regions, SDL_BlendMode overlayBlend)
{
	std::vector<MapLight> & lights = effects.lights;
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
	// 图块缩到 16 像素以下时地图缓存不再画起伏层（见 MapViewCache::drawPendingLayers），按法线打光、长投影都没有了：
	// 在 22 到 16 像素之间先淡出，不在那一步突然消失
	const float reliefFade = std::clamp((static_cast<float>(inputs.tileSize) - 16.0f) / 6.0f, 0.0f, 1.0f);
	uniforms.sun[3] = sun.relight * reliefFade;
	uniforms.sunColor[3] = sun.rim;
	uniforms.shade[3] = sun.strength * reliefFade;
	uniforms.shadowDir[0] = sun.shadow[0];
	uniforms.shadowDir[1] = sun.shadow[1];

	// 屏幕中心的地图坐标（云层视差用）
	const auto center = screenCenterTile(inputs, regions, width);
	uniforms.camera[0] = center[0];
	uniforms.camera[1] = center[1];
	uniforms.camera[2] = cloudParallax;
	uniforms.camera[3] = cloudAltitude;
	uniforms.atmos[0] = 0.36f; // 雾（贴地的雾气，地下也有）
	uniforms.atmos[1] = underground ? 0.0f : 0.85f; // 云（云层、山腰云海与云影都是天上的，地下没有）
	// 画进离屏图、之后经后处理统一调色时（renderFrame 传 emissionKeepingBlend），建筑先做反调色；直接画到窗口上时不调色，也就不反
	uniforms.atmos[2] = overlayBlend == emissionKeepingBlend ? gradeStrength : 0.0f;
	uniforms.atmos[3] = vividStrength;

	// 地表材质：按屏幕上一个图块多大淡入（缩得太小时退回图块原画）
	float materialStrength = 0.0f;
	if(materialsLoaded && !regions.empty() && regions.front().source.w > 0 && inputs.slots.x > 0)
	{
		const float tilePixels = static_cast<float>(regions.front().target.w) / static_cast<float>(regions.front().source.w) * width / static_cast<float>(inputs.slots.x);
		const float fade = std::clamp((tilePixels - materialFadeStart) / (materialFadeEnd - materialFadeStart), 0.0f, 1.0f);
		materialStrength = fade * fade * (3.0f - 2.0f * fade);
	}
	uniforms.terrain[0] = materialStrength;
	uniforms.terrain[1] = materialCellTiles;
	uniforms.terrain[2] = static_cast<float>(std::max(1, terrainField.size.x));
	uniforms.terrain[3] = static_cast<float>(std::max(1, terrainField.size.y));
	uniforms.atlas[0] = static_cast<float>(std::max(1, materialAtlasSize.x));
	uniforms.atlas[1] = static_cast<float>(std::max(1, materialAtlasSize.y));
	uniforms.atlas[2] = static_cast<float>(std::max(1, materialCell));
	uniforms.atlas[3] = 8.0f; // 每格四周环绕边的宽度（像素），与 tools/materials.py 一致
	for(size_t i = 0; i < materialMeans.size(); ++i)
		for(size_t c = 0; c < 4; ++c)
			uniforms.cellMean[i][c] = materialMeans[i][c];

	// 昼夜与天气（prepare() 里算好的太阳）
	uniforms.night[0] = sun.night;
	uniforms.night[1] = sun.lamps;
	uniforms.night[2] = sun.desaturate;
	uniforms.night[3] = sun.heat;
	uniforms.weather[0] = underground ? 0.0f : sun.cloudShadow;
	uniforms.weather[1] = underground ? 0.0f : 1.0f; // 阵雨

	// 局部光：离屏幕中心近的优先，闪动在这里算好（每盏灯一个相位）
	const float centerX = uniforms.camera[0];
	const float centerY = uniforms.camera[1];
	const size_t lightCount = std::min(lights.size(), maxLights);
	std::partial_sort(lights.begin(), lights.begin() + lightCount, lights.end(), [&](const MapLight & a, const MapLight & b)
	{
		return std::hypot(a.x - centerX, a.y - centerY) < std::hypot(b.x - centerX, b.y - centerY);
	});
	for(size_t i = 0; i < lightCount; ++i)
	{
		const MapLight & light = lights[i];
		float flicker = 1.0f;
		switch(light.flicker)
		{
			case MapLight::Flicker::FIRE: // 火苗乱跳：几路快慢不同的正弦叠起来，听不出节拍
				flicker = 0.82f + 0.09f * std::sin(now * 9.1f + light.seed * 3.0f) + 0.06f * std::sin(now * 14.7f + light.seed * 5.3f) + 0.04f * std::sin(now * 2.3f + light.seed);
				break;
			case MapLight::Flicker::MAGIC: // 法光缓慢明灭
				flicker = 0.78f + 0.22f * std::sin(now * 1.4f + light.seed);
				break;
			case MapLight::Flicker::LAVA: // 熔岩慢慢呼吸
				flicker = 0.85f + 0.15f * std::sin(now * 0.5f + light.seed);
				break;
			case MapLight::Flicker::STEADY:
				flicker = 0.97f + 0.03f * std::sin(now * 3.1f + light.seed);
				break;
		}
		uniforms.lightPos[i][0] = light.x;
		uniforms.lightPos[i][1] = light.y;
		uniforms.lightPos[i][2] = light.radius;
		uniforms.lightPos[i][3] = light.height;
		uniforms.lightColor[i][0] = light.r * flicker;
		uniforms.lightColor[i][1] = light.g * flicker;
		uniforms.lightColor[i][2] = light.b * flicker;
		uniforms.lightColor[i][3] = light.halo * flicker;
	}
	uniforms.lightInfo[0] = static_cast<float>(lightCount);

	// 分格：可见窗口均分成 lightGridSize × lightGridSize 格，每盏灯在它可能照到的格里记一位。
	// 物件表面在它脚下的地面往上最多 reliefHeightRange 个图块处，所以往上多留这么多；辉光比照亮的范围小
	const float cellWidth = static_cast<float>(std::max(1, inputs.slots.x)) / static_cast<float>(lightGridSize);
	const float cellHeight = static_cast<float>(std::max(1, inputs.slots.y)) / static_cast<float>(lightGridSize);
	uniforms.lightGrid[0] = static_cast<float>(inputs.worldOrigin.x);
	uniforms.lightGrid[1] = static_cast<float>(inputs.worldOrigin.y);
	uniforms.lightGrid[2] = cellWidth;
	uniforms.lightGrid[3] = cellHeight;
	auto cellOf = [&](float value, float origin, float size)
	{
		return std::clamp(static_cast<int>(std::floor((value - origin) / size)), 0, lightGridSize - 1);
	};
	for(size_t i = 0; i < lightCount; ++i)
	{
		const MapLight & light = lights[i];
		const int left = cellOf(light.x - light.radius, uniforms.lightGrid[0], cellWidth);
		const int right = cellOf(light.x + light.radius, uniforms.lightGrid[0], cellWidth);
		const int top = cellOf(light.y - light.radius - reliefHeightRange, uniforms.lightGrid[1], cellHeight);
		const int bottom = cellOf(light.y + light.radius, uniforms.lightGrid[1], cellHeight);
		for(int y = top; y <= bottom; ++y)
			for(int x = left; x <= right; ++x)
				uniforms.lightCells[y * lightGridSize + x] |= 1u << i;
	}

	// 蹄印：取最新的 maxFootprints 对（地图缓存按先后给），外接矩形外的像素着色器整段跳过
	const auto & prints = effects.footprints;
	const size_t printCount = std::min(prints.size(), maxFootprints);
	const size_t printStart = prints.size() - printCount;
	float printLeft = std::numeric_limits<float>::max();
	float printTop = std::numeric_limits<float>::max();
	float printRight = std::numeric_limits<float>::lowest();
	float printBottom = std::numeric_limits<float>::lowest();
	for(size_t i = 0; i < printCount; ++i)
	{
		const MapFootprint & print = prints[printStart + i];
		uniforms.prints[i][0] = print.x;
		uniforms.prints[i][1] = print.y;
		uniforms.prints[i][2] = std::atan2(print.dirY, print.dirX);
		uniforms.prints[i][3] = std::clamp(print.alpha, 0.0f, 1.0f) + (print.ground == MapFootprint::Ground::SNOW ? 2.0f : 0.0f);
		printLeft = std::min(printLeft, print.x);
		printTop = std::min(printTop, print.y);
		printRight = std::max(printRight, print.x);
		printBottom = std::max(printBottom, print.y);
	}
	constexpr float printReach = 0.36f; // 一对蹄印连同踩乱的地离中点最远约 0.3 个图块，再留一点
	uniforms.lightInfo[1] = static_cast<float>(printCount);
	uniforms.printBox[0] = printCount ? printLeft - printReach : 1.0f;
	uniforms.printBox[1] = printCount ? printTop - printReach : 1.0f;
	uniforms.printBox[2] = printCount ? printRight + printReach : 0.0f;
	uniforms.printBox[3] = printCount ? printBottom + printReach : 0.0f;

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
	frame.grade = sun.grade;
	frame.night = sun.night;
	frame.sky = !underground;
	frame.objects = inputs.objects;
	frame.blendMode = overlayBlend;
	frame.sparkles = effects.sparkles;
	frame.chimneys = effects.chimneys;
	frame.dust = effects.dust;
	for(size_t i = 0; i < lightCount; ++i)
		if(lights[i].flicker == MapLight::Flicker::FIRE && lights[i].halo >= 0.5f)
			frame.fires.push_back({lights[i].x, lights[i].y, lights[i].height, lights[i].seed});
	if(!regions.empty() && regions.front().source.w > 0 && inputs.slots.x > 0)
	{
		const float tilePixels = static_cast<float>(regions.front().target.w) / static_cast<float>(regions.front().source.w) * width / static_cast<float>(inputs.slots.x);
		const float fade = std::clamp((tilePixels - 12.0f) / 8.0f, 0.0f, 1.0f);
		frame.strength = fade * fade * (3.0f - 2.0f * fade);
		// 天气粒子按可见图块数淡出（开销正比于它，与屏幕像素密度无关）：默认缩放约八百块，一千六百块以内全有，
		// 超过两千六百块（缩到全图）不算。缩到全图时每片雪不到一个像素，却要逐个图块算几千片（2026-11-18 用户反馈缩放掉帧）
		const float tiles = static_cast<float>(inputs.slots.x * inputs.slots.y);
		const float weatherFade = std::clamp((2600.0f - tiles) / 1000.0f, 0.0f, 1.0f);
		frame.weatherStrength = weatherFade * weatherFade * (3.0f - 2.0f * weatherFade) * frame.strength;
	}
	particles.draw(renderer, frame, regions);
	// 覆盖层（云雾、路径）随后由 compose() 叠上，盖住粒子
}

void MapLayerCompositor::composeLayerByLayer(SDL_Renderer * renderer, const Inputs & inputs, const std::vector<PresentedRegion> & regions, int scaleMode) const
{
	// 没有着色器排前后，摇摆物件一律叠在静止物件之上；覆盖层由 drawOverlay() 画
	const std::array<std::pair<SDL_Texture *, SDL_BlendMode>, 3> layers = {{
		{inputs.ground, SDL_BLENDMODE_NONE},
		{inputs.objects, SDL_BLENDMODE_BLEND_PREMULTIPLIED},
		{inputs.sway, SDL_BLENDMODE_BLEND_PREMULTIPLIED},
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

	for(SDL_GPURenderState * postRenderState : {extractState, blurState, postState, highlightState})
		if(postRenderState)
			SDL_DestroyGPURenderState(postRenderState);
	extractState = nullptr;
	blurState = nullptr;
	postState = nullptr;
	highlightState = nullptr;
	postBoundTextures = {};
	destroyPostTargets();
	if(owner)
	{
		SDL_GPUDevice * device = deviceOf(owner);
		for(SDL_GPUShader * postShaderObject : {bloomShader, postShader, highlightShader})
			if(device && postShaderObject)
				SDL_ReleaseGPUShader(device, postShaderObject);
	}
	bloomShader = nullptr;
	postShader = nullptr;
	highlightShader = nullptr;
	postUnavailable = false;
	effects.highlight = MapHighlight{};

	if(fogState)
		SDL_DestroyGPURenderState(fogState);
	fogState = nullptr;
	if(owner && fogShader)
	{
		SDL_GPUDevice * device = deviceOf(owner);
		if(device)
			SDL_ReleaseGPUShader(device, fogShader);
	}
	fogShader = nullptr;
	fogBoundTextures = {};

	for(TileField * field : {&depthField, &terrainField, &fogField})
	{
		if(field->texture)
			SDL_DestroyTexture(field->texture);
		field->texture = nullptr;
		field->dirty = true; // 数据留着，下一个渲染器上重建纹理
	}
	if(materialAtlas)
		SDL_DestroyTexture(materialAtlas);
	materialAtlas = nullptr;
	materialsDirty = true;
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
