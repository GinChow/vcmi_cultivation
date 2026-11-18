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
	CUTOUT
};

/// Class that represents surface for drawing on
class Canvas
{
	friend class CanvasClipRectGuard;

	/// Upscaler awareness. Must be first member for initialization
	CanvasScalingPolicy scalingPolicy;

	/// Target surface
	SDL_Surface * surface;

	/// Current rendering area, all rendering operations will be moved into selected area
	Rect renderArea;

	/// constructs canvas using existing surface. Caller maintains ownership on the surface
	explicit Canvas(SDL_Surface * surface, CanvasScalingPolicy scalingPolicy);

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
	/// smooth 只在 SDL3 后端生效（线性插值），本后端始终最近邻
	void drawScaled(const Canvas &image, const Point & pos, const Point & targetSize, bool smooth = false);

	/// 叠加大地图分层缓存的图层。本后端不做分层（见 IScreenHandler::isMapLayeringSupported），按普通 alpha 混合缩放绘制
	void drawLayerPremultiplied(const Canvas & image, const Point & pos, const Point & targetSize);

	/// 画进大地图的遮罩画布（风场遮罩、起伏层）。本后端不做分层，不会有这些画布，按普通方式绘制
	void drawMapMask(const std::shared_ptr<IImage> & image, const Point & pos, const Rect & sourceRect, MapMaskBlend blend);

	/// 一次把 image 的多块区域缩放拷到本画布（覆盖）：每项是 {image 内的区域, 本画布内的目标区域}。本后端逐块拷
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

public:
	CanvasClipRectGuard(Canvas & canvas, const Rect & rect);
	~CanvasClipRectGuard();
};
