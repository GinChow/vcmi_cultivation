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
	Point slots;       ///< 缓存的图块槽位数（宽、高）
	Point worldOrigin; ///< 可见窗口左上角图块的地图坐标
	Point slotOrigin;  ///< 该图块在缓存中的槽位
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

	/// 雾云场（2026-11-17）：rgba 按行存放 size.x × size.y 个地图图块，G 生雾多少、B 山腰绕云多少（R、A 不用）。
	/// 着色器据此让流动的雾只在山脚、林地、水边浓起来，云海只绕着山。换地图或切换地上地下时由地图缓存调用
	virtual void setMapAtmosphereField(const std::vector<uint8_t> & rgba, const Point & size) = 0;
};
