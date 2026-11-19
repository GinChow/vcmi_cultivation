/*
 * Canvas.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "gui/TextAlignment.h"
#include "lib/Rect.h"
#include "lib/Color.h"

struct SDL_Surface;
struct SDL_Texture;
class IImage;
class IVideoInstance;
enum EFonts : int8_t;

enum class CanvasScalingPolicy
{
	AUTO,  // automatically scale canvas operations by global scaling factor
	IGNORE // disable any scaling processing. Scaling factor will be set to 1

};

/// 往大地图的遮罩画布上画图的方式（2026-11-17，大地图风场摇摆、光影）。
/// 风场遮罩（见 MapRendererObjects::renderWindMask）的 RGB 存摇摆物件的风场参数（预乘 alpha），
/// A 存“此处最上面盖着的是不摇摆的物件”的程度；起伏层（见 MapRendererObjects::renderRelief）
/// 的 RGB 存法线和高度（预乘 alpha），A 存物件本体的覆盖度：
enum class MapMaskBlend : uint8_t
{
	/// 摇摆物件的参数图：RGB 按 alpha 叠上去；A 乘以 (1 − alpha)，即这里改由摇摆物件在最上面
	SWAY_PARAMETERS,
	/// 不摇摆物件的剪影：RGB 不动（下面摇摆物件的参数保留，树从它背后摆出来时还要用）；
	/// A 叠上它的 alpha，即这里最上面盖着它
	OCCLUDER,
	/// 没有起伏图的物件（原版的城镇、英雄、矿……）在起伏层上的剪影：RGB 按 alpha 清零、A 按 alpha 叠上，
	/// 即这里最上面是一件“不知道起伏”的物件。着色器把 RGB 为 0 的地方当成平地，不重新打光、也不投长影
	CUTOUT,
	/// 特殊地面（被诅咒的大地、魔法平原……）在地形分布层上的覆盖度（2026-11-19）：RGB 与 A 都乘以 (1 − alpha)。
	/// 分布层清成不透明的黑、图块和其余剪影都不降低 A，所以 1 − A 就是特殊地面盖住这里的程度，
	/// 合成着色器按它铺特殊地面的材质（见 MapRendererObjects::renderGroundCutouts）
	SPECIAL_GROUND
};

/// Class that represents surface for drawing on
class Canvas
{
	friend class CanvasClipRectGuard;

	/// Upscaler awareness. Must be first member for initialization
	CanvasScalingPolicy scalingPolicy;

	/// Target surface. Null when this canvas draws onto a GPU render target instead.
	SDL_Surface * surface;

	/// GPU render target this canvas draws onto, or null for the software path.
	/// The caller owns it; the canvas only references it.
	SDL_Texture * renderTarget = nullptr;

	/// Whether this canvas has to destroy renderTarget. Only the canvas handed out by
	/// the screen handler owns it; copies and clipped views never do.
	bool ownsRenderTarget = false;

	/// GpuResources generation renderTarget was created under; deferred destruction only
	/// queues it when this still matches, same as SDLImage/CanvasImage do for their textures
	uint32_t renderTargetGeneration = 0;

	/// Current rendering area, all rendering operations will be moved into selected area
	Rect renderArea;

	/// constructs canvas using existing surface. Caller maintains ownership on the surface
	explicit Canvas(SDL_Surface * surface, CanvasScalingPolicy scalingPolicy);

	/// constructs canvas drawing onto a GPU render target. Caller maintains ownership
	Canvas(SDL_Texture * renderTarget, const Point & size, CanvasScalingPolicy scalingPolicy, bool ownsRenderTarget = false);

	/// Makes the renderer draw onto this canvas' target, if it is not already doing so
	void bindRenderTarget() const;

	/// GPU counterpart of the canvas-to-canvas blits. blendMode is an SDL_BlendMode,
	/// kept untyped so that this header does not have to pull in SDL
	void copyFromCanvas(const Canvas & image, const Rect & targetArea, uint32_t blendMode, uint8_t alpha, bool linear = false);

	/// Fallback for content that has no texture of its own: renders it into a scratch
	/// surface through the software path, then uploads that onto this canvas' target
	void drawViaScratchSurface(const Point & pos, const Point & size, const std::function<void(SDL_Surface *)> & render);

	/// copy constructor
	Canvas(const Canvas & other);

	Point transformPos(const Point & input);
	Point transformSize(const Point & input);

public:
	Canvas & operator = (const Canvas & other) = delete;
	Canvas & operator = (Canvas && other) = delete;

	/// move constructor
	Canvas(Canvas && other);

	/// creates canvas that only covers specified subsection of a surface
	Canvas(const Canvas & other, const Rect & clipRect);

	/// constructs canvas of specified size
	explicit Canvas(const Point & size, CanvasScalingPolicy scalingPolicy);

	/// constructs canvas using existing surface. Caller maintains ownership on the surface
	/// Compatibility method. AVOID USAGE. To be removed once SDL abstraction layer is finished.
	static Canvas createFromSurface(SDL_Surface * surface, CanvasScalingPolicy scalingPolicy);

	/// constructs canvas drawing onto a GPU render target of the given size in logical units
	static Canvas createFromRenderTarget(SDL_Texture * renderTarget, const Point & size, CanvasScalingPolicy scalingPolicy);

	/// Same, but the returned canvas destroys the render target when it dies
	static Canvas createOwningRenderTarget(SDL_Texture * renderTarget, const Point & size, CanvasScalingPolicy scalingPolicy);

	/// True when this canvas draws with the GPU rather than into a surface
	bool isRenderTarget() const { return renderTarget != nullptr; }

	/// The texture this canvas draws onto, for the screen handler to read from while it composes
	/// the frame. Null for a surface-backed canvas.
	SDL_Texture * getRenderTargetTexture() const { return renderTarget; }

	~Canvas();

	/// if set to true, drawing this canvas onto another canvas will use alpha channel information
	void applyTransparency(bool on);

	/// applies grayscale filter onto current image
	void applyGrayscale();

	/// renders image onto this canvas at specified position
	void draw(const std::shared_ptr<IImage>& image, const Point & pos);
	void draw(const IImage& image, const Point & pos);

	void draw(IVideoInstance & video, const Point & pos);

	/// renders section of image bounded by sourceRect at specified position
	void draw(const std::shared_ptr<IImage>& image, const Point & pos, const Rect & sourceRect);

	/// renders another canvas onto this canvas
	void draw(const Canvas &image, const Point & pos);

	/// renders another canvas onto this canvas with transparency
	void drawTransparent(const Canvas & image, const Point & pos, double transparency);

	/// renders another canvas onto this canvas with scaling
	/// smooth：放大时线性插值（小地图水墨样式用，格子边晕开），默认最近邻
	void drawScaled(const Canvas &image, const Point & pos, const Point & targetSize, bool smooth = false);

	/// 把一张画在透明底上的图层（颜色已是预乘 alpha，例如大地图分层缓存的物件层）缩放叠到本画布上
	void drawLayerPremultiplied(const Canvas & image, const Point & pos, const Point & targetSize);

	/// 与 draw(image, pos, sourceRect) 相同，但按 blend 画进大地图的遮罩画布（风场遮罩、起伏层，见 MapMaskBlend）。
	/// 只用于 GPU 画布（这些画布只在大地图分层时存在）；软件表面上按普通方式画
	void drawMapMask(const std::shared_ptr<IImage> & image, const Point & pos, const Rect & sourceRect, MapMaskBlend blend);

	/// 一次把 image 的多块区域缩放拷到本画布（覆盖，不混合）：每项是 {image 内的区域, 本画布内的目标区域}，逻辑坐标。
	/// GPU 上合成一次几何绘制，免得逐块拷贝时每块都要重设一遍纹理状态、单独提交（大地图缩到最小时每帧要拷上千块）
	void drawScaledBatch(const Canvas & image, const std::vector<std::pair<Rect, Rect>> & regions);

	/// renders single pixels with specified color
	void drawPoint(const Point & dest, const ColorRGBA & color);

	/// renders continuous, 1-pixel wide line with color gradient
	void drawLine(const Point & from, const Point & dest, const ColorRGBA & colorFrom, const ColorRGBA & colorDest);

	/// renders rectangular, solid-color border in specified location
	void drawBorder(const Rect & target, const ColorRGBA & color, int width = 1);

	/// renders rectangular, dashed border in specified location
	void drawBorderDashed(const Rect & target, const ColorRGBA & color);

	/// renders single line of text with specified parameters
	void drawText(const Point & position, const EFonts & font, const ColorRGBA & colorDest, ETextAlignment alignment, const std::string & text );

	/// renders multiple lines of text with specified parameters
	void drawText(const Point & position, const EFonts & font, const ColorRGBA & colorDest, ETextAlignment alignment, const std::vector<std::string> & text );

	/// fills selected area with solid color
	void drawColor(const Rect & target, const ColorRGBA & color);

	/// fills selected area with blended color
	void drawColorBlended(const Rect & target, const ColorRGBA & color);

	/// fills canvas with texture
	void fillTexture(const std::shared_ptr<IImage>& image);

	int getScalingFactor() const;

	/// get the render area
	Rect getRenderArea() const;

	/// get pixel color
	ColorRGBA getPixel(const Point & position) const;
};

class CanvasClipRectGuard : boost::noncopyable
{
	SDL_Surface * surf;
	Rect oldRect;

	/// Set when the guarded canvas draws on the GPU, where clipping is renderer state
	/// rather than a property of the target
	bool onRenderTarget = false;
	bool hadClipRect = false;

	/// The guarded canvas, so its target can be rebound before the clip is restored
	const Canvas * guarded = nullptr;

public:
	CanvasClipRectGuard(Canvas & canvas, const Rect & rect);
	~CanvasClipRectGuard();
};
