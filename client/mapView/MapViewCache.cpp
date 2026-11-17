/*
 * MapViewCache.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */

#include "StdInc.h"
#include "MapViewCache.h"

#include "IMapRendererContext.h"
#include "MapRenderer.h"
#include "MapViewModel.h"

#include "render/CAnimation.h"
#include "render/CanvasImage.h"
#include "render/Canvas.h"
#include "render/IImage.h"
#include "render/IFont.h"
#include "render/IRenderHandler.h"
#include "render/Graphics.h"

#include "../GameEngine.h"
#include "render/IScreenHandler.h"
#include "../widgets/TextControls.h"

#include "../../lib/TerrainHandler.h"
#include "../../lib/filesystem/Filesystem.h"
#include "../../lib/filesystem/PngStripWriter.h"
#include "../../lib/int3.h"
#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/mapObjects/CGObjectInstance.h"
#include "../../lib/mapping/TerrainTile.h"

MapViewCache::~MapViewCache()
{
	// the canvases go with this object, so nothing may be left pointing at them
	ENGINE->screenHandler().clearPresentedCanvas(GpuRenderLayer::MAP);
}

MapViewCache::MapViewCache(const std::shared_ptr<MapViewModel> & model, bool useGpuLayer)
	: model(model)
	, useGpuLayer(useGpuLayer)
	, cachedLevel(0)
	, overlayWasVisible(false)
	, mapRenderer(new MapRenderer())
	, birdRandom(std::random_device{}())
	, iconsStorage(ENGINE->renderHandler().loadAnimation(AnimationPath::builtin("VwSymbol"), EImageBlitMode::COLORKEY))
{
	Point visibleSize = model->getTilesVisibleDimensions();
	terrainChecksum.resize(boost::extents[visibleSize.x][visibleSize.y]);
	tilesUpToDate.resize(boost::extents[visibleSize.x][visibleSize.y]);
}

std::unique_ptr<Canvas> MapViewCache::createCanvas(const Point & size) const
{
	// a view drawn into the software screen cannot read from a GPU-backed cache
	if(canvasesOnGpu)
		return std::make_unique<Canvas>(ENGINE->screenHandler().createOffscreenCanvas(size));

	return std::make_unique<Canvas>(size, CanvasScalingPolicy::AUTO);
}

void MapViewCache::ensureCanvases()
{
	const bool useGpu = useGpuLayer && ENGINE->screenHandler().isGpuRenderingEnabled();

	// On the GPU the cache normally keeps its tiles unscaled and scales them while blitting,
	// which spares the per-tile render target the scaling used to go through. But that texture
	// follows the number of visible tiles, which on a fully zoomed out Giant map can ask for
	// more pixels than the driver allows - so it falls back to the bounded, ready-scaled size,
	// same as the non-GPU path, rather than risk an oversized render target.
	const Point nativeSizeDimensions = model->getTilesVisibleDimensions() * model->getCacheTileSize();
	const int maxCanvasSize = ENGINE->screenHandler().maxOffscreenCanvasSize();
	const bool nativeSizeFits = nativeSizeDimensions.x <= maxCanvasSize && nativeSizeDimensions.y <= maxCanvasSize;
	const bool cacheAtNativeSize = useGpu && nativeSizeFits;

	model->setCacheAtNativeSize(cacheAtNativeSize);

	const Point cacheDimensions = model->getCacheDimensionsPixels();

	// only the native cache changes size during play - it follows the number of visible tiles,
	// while the ready-scaled one is measured from the window and stays as it was
	const bool dimensionsStale = cacheAtNativeSize && cachedCanvasDimensions != cacheDimensions;

	// 分层合成的着色器靠纹理坐标取模越过环形接缝，所以缓存必须正好是 槽位数 × 图块尺寸。
	// GPU 缓存都满足这一点（缩得很小时存的是缩小的图块，但仍按槽位排列）；退回窗口大小的缓存则不满足
	const bool useLayers = wantLayers && cacheAtNativeSize;

	if(terrain && canvasesOnGpu == useGpu && cachedAtNativeSize == cacheAtNativeSize && !dimensionsStale && layered == useLayers)
		return;

	// Must run on the rendering thread: this object is constructed while handling a
	// netpack, and creating a texture there would move the GL context off the GUI thread
	// what the screen handler was told to draw refers to the canvas about to be replaced
	ENGINE->screenHandler().clearPresentedCanvas(GpuRenderLayer::MAP);

	canvasesOnGpu = useGpu;
	cachedAtNativeSize = cacheAtNativeSize;
	cachedCanvasDimensions = cacheDimensions;

	atlasCapacity = canvasesOnGpu ? atlasTilesPerSide * atlasTilesPerSide : 1;
	atlas = createCanvas(MapViewModel::getNativeTileSize() * (canvasesOnGpu ? atlasTilesPerSide : 1));
	terrain = createCanvas(cacheDimensions);
	terrainTransition = createCanvas(model->getPixelsVisibleDimensions());

	layered = useLayers;
	liteLayersCleared = false;
	if(layered)
	{
		objectLayer = createCanvas(cacheDimensions);
		swayLayer = createCanvas(cacheDimensions);
		overlayLayer = createCanvas(cacheDimensions);
		waterMaskLayer = createCanvas(cacheDimensions);
		windMaskLayer = createCanvas(cacheDimensions);
		reliefLayer = createCanvas(cacheDimensions);
		splatLayer = createCanvas(cacheDimensions);
		logGlobal->info("Map cache uses separate layers for water, wind and lighting effects (%dx%d)", cacheDimensions.x, cacheDimensions.y);
	}
	else
	{
		objectLayer.reset();
		swayLayer.reset();
		overlayLayer.reset();
		waterMaskLayer.reset();
		windMaskLayer.reset();
		reliefLayer.reset();
		splatLayer.reset();
	}

	// the new canvases are empty, so nothing cached about the old ones still holds
	std::fill_n(terrainChecksum.data(), terrainChecksum.num_elements(), TileChecksum{});
	std::fill_n(tilesUpToDate.data(), tilesUpToDate.num_elements(), false);
}

std::shared_ptr<IImage> MapViewCache::getOverlayImageForTile(const std::shared_ptr<IMapRendererContext> & context, const int3 & coordinates)
{
	size_t imageIndex = context->overlayImageIndex(coordinates);

	if(imageIndex < iconsStorage->size())
		return iconsStorage->getImage(imageIndex);
	return nullptr;
}

void MapViewCache::invalidate(const std::shared_ptr<IMapRendererContext> & context, const ObjectInstanceID & object)
{
	for(size_t cacheY = 0; cacheY < terrainChecksum.shape()[1]; ++cacheY)
	{
		for(size_t cacheX = 0; cacheX < terrainChecksum.shape()[0]; ++cacheX)
		{
			auto & entry = terrainChecksum[cacheX][cacheY];

			int3 tile(entry.tileX, entry.tileY, cachedLevel);

			if(context->isInMap(tile) && vstd::contains(context->getObjects(tile), object))
				entry = TileChecksum{};
		}
	}
}

/// Terrain animation is unstaggered, so every animated tile goes dirty on the same frame. Drawing
/// them all at once overruns the frame, so the burst is spread over this many frames.
static constexpr int animationCatchUpFrames = 4;

/// Smallest share a frame redraws. The share follows the number of dirty tiles, so it only grows
/// past this when a step dirties more than the old fixed cap could serve before the next one.
static constexpr int animationRedrawShareMinimum = 64;

/// 分层模式下各层在 PendingTile::layers 里的位
static constexpr uint8_t tileLayerBit(MapTileLayer layer)
{
	return static_cast<uint8_t>(1u << static_cast<uint8_t>(layer));
}
/// 物件变了要重画的层：静止物件、摇摆物件、记着它们前后关系的风场遮罩和起伏层总是一起变
static constexpr uint8_t objectTileLayers = tileLayerBit(MapTileLayer::OBJECTS) | tileLayerBit(MapTileLayer::SWAY_OBJECTS) | tileLayerBit(MapTileLayer::WIND_MASK) | tileLayerBit(MapTileLayer::RELIEF);
/// 地面变了（不只是动画换帧）要重画的遮罩层：水面遮罩、地形分布
static constexpr uint8_t groundMaskLayers = tileLayerBit(MapTileLayer::WATER_MASK) | tileLayerBit(MapTileLayer::TERRAIN_SPLAT);
static constexpr uint8_t allTileLayers = tileLayerBit(MapTileLayer::GROUND) | groundMaskLayers | objectTileLayers | tileLayerBit(MapTileLayer::OVERLAY);

/// Indices of the terrain and river components of MapRenderer::getTileChecksum()
static constexpr size_t terrainChecksumIndex = 1;
static constexpr size_t riverChecksumIndex = 2;

/// Whether the two checksums differ in nothing but the terrain animation
static bool differsOnlyInTerrainAnimation(const std::array<uint8_t, 8> & before, const std::array<uint8_t, 8> & after)
{
	for(size_t i = 0; i < before.size(); ++i)
		if(before[i] != after[i] && i != terrainChecksumIndex && i != riverChecksumIndex)
			return false;

	return true;
}

void MapViewCache::updateTile(const std::shared_ptr<IMapRendererContext> & context, const int3 & coordinates)
{
	int cacheX = (terrainChecksum.shape()[0] + coordinates.x) % terrainChecksum.shape()[0];
	int cacheY = (terrainChecksum.shape()[1] + coordinates.y) % terrainChecksum.shape()[1];

	auto & oldCacheEntry = terrainChecksum[cacheX][cacheY];
	TileChecksum newCacheEntry;

	newCacheEntry.tileX = coordinates.x;
	newCacheEntry.tileY = coordinates.y;
	newCacheEntry.checksum = mapRenderer->getTileChecksum(*context, coordinates);

	if(cachedLevel == coordinates.z && oldCacheEntry == newCacheEntry && !context->tileAnimated(coordinates))
		return;

	// only an animation step may wait - a scrolled-in or genuinely changed tile is drawn now
	const bool holdsSameTile = oldCacheEntry.tileX == coordinates.x && oldCacheEntry.tileY == coordinates.y;
	const bool animationOnly = cachedLevel == coordinates.z && holdsSameTile
		&& !context->tileAnimated(coordinates)
		&& differsOnlyInTerrainAnimation(oldCacheEntry.checksum, newCacheEntry.checksum);

	if(animationOnly)
	{
		++animatedTilesDirty;

		if(animatedTilesRedrawn >= animatedTileRedrawBudget)
			return;

		++animatedTilesRedrawn;
	}

	// 分层时算出哪些层真的变了：校验值第 0~3 项是地图边缘、地形、河流、道路（地面层），第 4 项是物件（三个物件层），
	// 第 5~7 项是路径、覆盖图标、战争迷雾（覆盖层）。换了图块、换了地图层或图块被标记为动画中时整块重画
	uint8_t layers = allTileLayers;
	if(holdsSameTile && cachedLevel == coordinates.z && !context->tileAnimated(coordinates))
	{
		const auto & before = oldCacheEntry.checksum;
		const auto & after = newCacheEntry.checksum;
		auto changed = [&](size_t index) { return before[index] != after[index]; };

		layers = 0;
		// 只是地形、河流动画换帧时只重画地面层；其他地面变化（如迷雾揭开、地形改变）还要重画水面遮罩和地形分布
		if(changed(0) || changed(1) || changed(2) || changed(3))
			layers |= animationOnly ? tileLayerBit(MapTileLayer::GROUND) : tileLayerBit(MapTileLayer::GROUND) | groundMaskLayers;
		if(changed(4))
		{
			layers |= objectTileLayers;
			// 湖这类水面装饰物画在地面层（地形分布层上挖空），它们换帧时这两层也要重画
			if(layered && mapRenderer->hasWaterSurface(*context, coordinates))
				layers |= tileLayerBit(MapTileLayer::GROUND) | tileLayerBit(MapTileLayer::TERRAIN_SPLAT);
		}
		if(changed(5) || changed(6) || changed(7))
			layers |= tileLayerBit(MapTileLayer::OVERLAY);
		if(layers == 0)
			layers = allTileLayers;
	}

	// 先记下，等这一帧的图块都判断完，再在 drawPendingLayers() 里批量画
	pendingTiles.push_back({coordinates, newCacheEntry, layers});
}

void MapViewCache::drawPendingLayers(const std::shared_ptr<IMapRendererContext> & context)
{
	if(pendingTiles.empty())
		return;

	// 每个图块用到的替身图次数（还没放大好的图）。A tile drawn from a stretched stand-in is not final:
	// leaving its checksum empty keeps it out of the cache, so it is drawn again until the upscale it waits for has finished.
	std::vector<uint32_t> placeholders(pendingTiles.size(), 0);

	std::vector<std::pair<MapTileLayer, Canvas *>> passes;
	if(layered)
		passes = {
			{MapTileLayer::GROUND, terrain.get()},
			{MapTileLayer::WATER_MASK, waterMaskLayer.get()},
			{MapTileLayer::OBJECTS, objectLayer.get()},
			{MapTileLayer::SWAY_OBJECTS, swayLayer.get()},
			{MapTileLayer::WIND_MASK, windMaskLayer.get()},
			{MapTileLayer::RELIEF, reliefLayer.get()},
			{MapTileLayer::TERRAIN_SPLAT, splatLayer.get()},
			{MapTileLayer::OVERLAY, overlayLayer.get()}
		};
	else
		passes = {{MapTileLayer::ALL, terrain.get()}};

	const Point nativeTileSize = MapViewModel::getNativeTileSize();
	const Point cacheTileSize = model->getCacheTileSize();

	// 缓存按原尺寸存图块时直接画进去；存缩小图块时先成批画进图集，再成批缩小拷进缓存
	const bool direct = cacheTileSize == nativeTileSize;
	auto atlasCell = [&](size_t slot)
	{
		return Rect(Point(static_cast<int>(slot) % atlasTilesPerSide, static_cast<int>(slot) / atlasTilesPerSide) * nativeTileSize, nativeTileSize);
	};

	// 缩放时每一步都要把可见的图块全部重画，各层累计起来每个图块十几次绘制，缩到全图时几千个图块要五六十毫秒
	// （2026-11-18 用户反馈缩放掉帧）。省掉其中的固定开销：
	//  1. 整张缓存都要重画时整层清一次，不再每个图块单独清（每次清色都是一次绘制）；图集每批也只清一次；
	//  2. 完全在迷雾里的图块只有覆盖层有东西，每个图块只判断一次，其余各层不再逐层去算四周的可见性；
	//  3. 缓存存缩小图块时（图块不到 16 像素），摇摆层、风场遮罩、起伏层、地形分布层不画、保持清空——
	//     这么小时树摆不到一个像素，长投影、地表材质也已淡出（见 MapLayerCompositor），树改画回物件层（update() 里关了分层特效）
	const Rect tilesRect = model->getTilesTotalRect();
	const bool fullRedraw = pendingTiles.size() >= static_cast<size_t>(tilesRect.w) * static_cast<size_t>(tilesRect.h);
	const bool lite = layered && !direct;
	auto skippedWhenLite = [](MapTileLayer layer)
	{
		return layer == MapTileLayer::SWAY_OBJECTS || layer == MapTileLayer::WIND_MASK || layer == MapTileLayer::RELIEF || layer == MapTileLayer::TERRAIN_SPLAT;
	};

	std::vector<uint8_t> hidden(pendingTiles.size(), 0);
	if(layered)
		for(size_t i = 0; i < pendingTiles.size(); ++i)
			hidden[i] = mapRenderer->isFullyHidden(*context, pendingTiles[i].coordinates);

	for(const auto & [layer, canvas] : passes)
	{
		// 槽位里还留着上一个图块的内容，先清空：水面遮罩、地形分布清成不透明的黑（不是水、不铺材质），其余（含风场遮罩、起伏层）清成透明
		const bool opaqueMask = layer == MapTileLayer::WATER_MASK || layer == MapTileLayer::TERRAIN_SPLAT;
		const ColorRGBA clearColor = opaqueMask ? ColorRGBA(0, 0, 0, 255) : ColorRGBA(0, 0, 0, 0);

		std::vector<size_t> tiles;
		for(size_t i = 0; i < pendingTiles.size(); ++i)
			if(layer == MapTileLayer::ALL || (pendingTiles[i].layers & tileLayerBit(layer)))
				tiles.push_back(i);

		// 整层都要重画：整张清一次，各图块就不必再清（图集模式下拷进来的图块是覆盖写入，本来就不用清缓存）。
		// 缩小图块时不画的层清过一次就不再清
		const bool wholeLayer = fullRedraw && tiles.size() == pendingTiles.size();
		const bool alreadyEmpty = lite && skippedWhenLite(layer) && liteLayersCleared;
		if(wholeLayer && !alreadyEmpty)
			canvas->drawColor(Rect(Point(0, 0), canvas->getRenderArea().dimensions() / canvas->getScalingFactor()), clearColor);

		// 缩小图块时不画的层：新建的画布整张清一次，之后什么都不往里画，一直是空的（滚动时槽位换了图块也不用清）
		if(lite && skippedWhenLite(layer))
		{
			if(!liteLayersCleared && !wholeLayer)
				canvas->drawColor(Rect(Point(0, 0), canvas->getRenderArea().dimensions() / canvas->getScalingFactor()), clearColor);
			continue;
		}

		const size_t batch = direct ? tiles.size() : static_cast<size_t>(atlasCapacity);
		for(size_t begin = 0; begin < tiles.size(); begin += batch)
		{
			const size_t end = std::min(tiles.size(), begin + batch);

			// 图集这一批用到的格子整片清一次
			if(!direct)
			{
				const int rows = static_cast<int>((end - begin + atlasTilesPerSide - 1) / atlasTilesPerSide);
				atlas->drawColor(Rect(0, 0, nativeTileSize.x * atlasTilesPerSide, nativeTileSize.y * rows), clearColor);
			}

			for(size_t k = begin; k < end; ++k)
			{
				const PendingTile & tile = pendingTiles[tiles[k]];
				Canvas target = direct ? Canvas(*canvas, model->getCacheTileArea(tile.coordinates)) : Canvas(*atlas, atlasCell(k - begin));
				if(direct && !wholeLayer)
					target.drawColor(Rect(Point(0, 0), nativeTileSize), clearColor);

				// 完全在迷雾里的图块，除了覆盖层都是空的
				if(hidden[tiles[k]] && layer != MapTileLayer::OVERLAY)
					continue;

				const uint32_t placeholdersBefore = ENGINE->renderHandler().getPlaceholderDrawCount();
				mapRenderer->renderTileLayer(*context, target, tile.coordinates, layer, true);
				placeholders[tiles[k]] += ENGINE->renderHandler().getPlaceholderDrawCount() - placeholdersBefore;
			}

			if(direct)
				continue;

			// 整批一次缩小拷进缓存
			std::vector<std::pair<Rect, Rect>> copies;
			copies.reserve(end - begin);
			for(size_t k = begin; k < end; ++k)
				copies.emplace_back(atlasCell(k - begin), model->getCacheTileArea(pendingTiles[tiles[k]].coordinates));
			canvas->drawScaledBatch(*atlas, copies);
		}

		// 拼图地图整体变灰（只在不分层时出现）
		if(context->filterGrayscale())
			for(size_t i : tiles)
				Canvas(*canvas, model->getCacheTileArea(pendingTiles[i].coordinates)).applyGrayscale();
	}
	if(lite)
		liteLayersCleared = true;
	else
		liteLayersCleared = false; // 回到原尺寸后这几层又要画了，下次缩小时重新清

	for(size_t i = 0; i < pendingTiles.size(); ++i)
	{
		const PendingTile & tile = pendingTiles[i];
		const int cacheX = (terrainChecksum.shape()[0] + tile.coordinates.x) % terrainChecksum.shape()[0];
		const int cacheY = (terrainChecksum.shape()[1] + tile.coordinates.y) % terrainChecksum.shape()[1];

		terrainChecksum[cacheX][cacheY] = placeholders[i] ? TileChecksum{} : tile.checksum;
		tilesUpToDate[cacheX][cacheY] = false;
	}

	pendingTiles.clear();
}

void MapViewCache::sendDepthField(const std::shared_ptr<IMapRendererContext> & context)
{
	const int3 mapSize = context->getMapSize();
	const int level = model->getLevel();
	const int width = mapSize.x;
	const int height = mapSize.y;

	// 倒角距离变换：陆地为 0，水面取到最近陆地图块的距离（直走 1、斜走 √2），两遍扫描。
	// 地图外不算陆地，所以贴着地图边的大海也是深海
	constexpr float unreachable = 1e6f;
	constexpr float diagonal = 1.41421356f;
	std::vector<float> distance(static_cast<size_t>(width * height), unreachable);
	auto at = [&](int x, int y) -> float & { return distance[static_cast<size_t>(y * width + x)]; };

	for(int y = 0; y < height; ++y)
		for(int x = 0; x < width; ++x)
			if(!context->getMapTile(int3(x, y, level)).getTerrain()->isWater())
				at(x, y) = 0;

	for(int y = 0; y < height; ++y)
	{
		for(int x = 0; x < width; ++x)
		{
			float & d = at(x, y);
			if(x > 0)
				d = std::min(d, at(x - 1, y) + 1);
			if(y > 0)
				d = std::min(d, at(x, y - 1) + 1);
			if(x > 0 && y > 0)
				d = std::min(d, at(x - 1, y - 1) + diagonal);
			if(x + 1 < width && y > 0)
				d = std::min(d, at(x + 1, y - 1) + diagonal);
		}
	}

	for(int y = height - 1; y >= 0; --y)
	{
		for(int x = width - 1; x >= 0; --x)
		{
			float & d = at(x, y);
			if(x + 1 < width)
				d = std::min(d, at(x + 1, y) + 1);
			if(y + 1 < height)
				d = std::min(d, at(x, y + 1) + 1);
			if(x + 1 < width && y + 1 < height)
				d = std::min(d, at(x + 1, y + 1) + diagonal);
			if(x > 0 && y + 1 < height)
				d = std::min(d, at(x - 1, y + 1) + diagonal);
		}
	}

	// 编码：0~255 对应 0~8 个图块，再远都算 8
	std::vector<uint8_t> encoded(distance.size());
	for(size_t i = 0; i < distance.size(); ++i)
		encoded[i] = static_cast<uint8_t>(std::round(std::min(distance[i], 8.0f) / 8.0f * 255.0f));

	ENGINE->screenHandler().setMapDepthField(encoded, Point(width, height));
	depthFieldLevel = level;
}

/// 各地形上植被粒子的系数（2026-11-17）：落叶、飞花、萤火虫、栖鸟。按地形类型放粒子就靠这张表
struct TerrainParticles
{
	float leaves;
	float petals;
	float fireflies;
	float birds;
};

static TerrainParticles terrainParticles(const TerrainId & terrain)
{
	switch(terrain.getNum())
	{
		case ETerrainId::GRASS:
			return {1.0f, 1.0f, 0.12f, 1.0f};
		case ETerrainId::DIRT:
			return {1.0f, 0.8f, 0.06f, 1.0f};
		case ETerrainId::SWAMP: // 沼泽萤火虫最多
			return {0.8f, 0.5f, 1.0f, 0.8f};
		case ETerrainId::ROUGH:
			return {0.8f, 0.6f, 0.04f, 0.8f};
		case ETerrainId::SAND:
			return {0.6f, 0.5f, 0.0f, 0.4f};
		case ETerrainId::SNOW: // 雪地没有花，鸟也少
			return {0.4f, 0.0f, 0.0f, 0.6f};
		case ETerrainId::SUBTERRANEAN: // 地下没有鸟，洞里飘些萤光
			return {0.3f, 0.2f, 0.5f, 0.0f};
		case ETerrainId::LAVA:
			return {0.2f, 0.0f, 0.0f, 0.0f};
		case ETerrainId::WATER:
		case ETerrainId::ROCK:
			return {0.0f, 0.0f, 0.0f, 0.0f};
		default: // 模组新增的地形
			return {0.6f, 0.5f, 0.05f, 0.6f};
	}
}

void MapViewCache::sendVegetationField(const std::shared_ptr<IMapRendererContext> & context)
{
	const int3 mapSize = context->getMapSize();
	const int level = model->getLevel();
	const int width = mapSize.x;
	const int height = mapSize.y;
	const size_t count = static_cast<size_t>(width * height);
	auto index = [&](int x, int y) { return static_cast<size_t>(y * width + x); };

	std::vector<float> leaves(count, 0.0f);
	std::vector<float> petals(count, 0.0f);
	std::vector<float> fireflies(count, 0.0f);
	std::vector<uint8_t> lively(count, 0); // 有植物或水
	birdRoosts.assign(count, 0.0f);

	for(int y = 0; y < height; ++y)
	{
		for(int x = 0; x < width; ++x)
		{
			const int3 tile(x, y, level);
			const TerrainTile & mapTile = context->getMapTile(tile);
			const TerrainParticles terrain = terrainParticles(mapTile.getTerrainID());
			const MapVegetation vegetation = mapRenderer->vegetationAt(*context, tile);
			const size_t i = index(x, y);

			leaves[i] = vegetation.leaves * terrain.leaves;
			petals[i] = vegetation.petals * terrain.petals;
			fireflies[i] = terrain.fireflies;
			birdRoosts[i] = vegetation.birds * terrain.birds;
			lively[i] = mapTile.getTerrain()->isWater() || vegetation.leaves + vegetation.petals + vegetation.birds > 0.0f;
		}
	}

	auto encode = [](float value) { return static_cast<uint8_t>(std::round(std::clamp(value, 0.0f, 1.0f) * 255.0f)); };

	std::vector<uint8_t> rgba(count * 4, 0);
	for(int y = 0; y < height; ++y)
	{
		for(int x = 0; x < width; ++x)
		{
			// 萤火虫贴着植物和水：周围一圈都空旷的地方只剩十分之一
			bool nearLife = false;
			// 风往右下吹，落叶、飞花从左上方两个图块之内飘来
			float reach = 0.0f;
			for(int dy = -2; dy <= 1; ++dy)
			{
				for(int dx = -2; dx <= 1; ++dx)
				{
					const int nx = x + dx;
					const int ny = y + dy;
					if(nx < 0 || ny < 0 || nx >= width || ny >= height)
						continue;
					if(std::abs(dx) <= 1 && std::abs(dy) <= 1)
						nearLife = nearLife || lively[index(nx, ny)];
					if(dx <= 0 && dy <= 0)
						reach = std::max(reach, leaves[index(nx, ny)] + petals[index(nx, ny)]);
				}
			}

			const size_t i = index(x, y);
			rgba[i * 4 + 0] = encode(leaves[i]);
			rgba[i * 4 + 1] = encode(petals[i]);
			rgba[i * 4 + 2] = encode(fireflies[i] * (nearLife ? 1.0f : 0.1f));
			rgba[i * 4 + 3] = encode(reach);
		}
	}

	ENGINE->screenHandler().setMapVegetationField(rgba, Point(width, height));
	roostMapSize = Point(width, height);
	recentFlocks.clear();
}

/// 各地形自己生雾的多少（2026-11-17）：沼泽水汽最重，雪原、地下有些薄雾；水面只在岸边起雾（见 sendAtmosphereField），
/// 开阔海面上一团团白雾像脏斑（2026-11-17 实机）；山脚、林地的雾来自物件的 atmosphere 参数
static float terrainMist(const TerrainId & terrain)
{
	switch(terrain.getNum())
	{
		case ETerrainId::SWAMP:
			return 0.75f;
		case ETerrainId::WATER:
			return 0.0f;
		case ETerrainId::SNOW:
		case ETerrainId::SUBTERRANEAN:
			return 0.25f;
		case ETerrainId::GRASS:
		case ETerrainId::DIRT:
			return 0.08f;
		default:
			return 0.0f;
	}
}

/// 各地形地表大尺度变化（明暗、干湿冷暖）的幅度（2026-11-17）：地形图块为了无缝拼接把每块的大块明暗压平了，
/// 由合成着色器按地图坐标连续地加回来。水面另有水面着色器，岩石（虚空、崖壁）少加
static float terrainVariation(const TerrainId & terrain)
{
	switch(terrain.getNum())
	{
		case ETerrainId::WATER:
			return 0.0f;
		case ETerrainId::ROCK:
			return 0.4f;
		case ETerrainId::SNOW:
			return 0.85f;
		default:
			return 1.0f;
	}
}

/// 按图块的场做盒式模糊：每遍沿 x、y 各模糊一次，半径 radius 个图块，地图外按边上的值算。做两遍近似高斯
static void blurTileField(std::vector<float> & field, int width, int height, int radius, int passes)
{
	auto index = [&](int x, int y) { return static_cast<size_t>(y * width + x); };
	std::vector<float> line;
	for(int pass = 0; pass < passes; ++pass)
	{
		for(int axis = 0; axis < 2; ++axis)
		{
			const int length = axis == 0 ? width : height;
			const int lines = axis == 0 ? height : width;
			line.resize(static_cast<size_t>(length));
			for(int l = 0; l < lines; ++l)
			{
				for(int k = 0; k < length; ++k)
					line[static_cast<size_t>(k)] = field[axis == 0 ? index(k, l) : index(l, k)];
				for(int k = 0; k < length; ++k)
				{
					float sum = 0.0f;
					for(int d = -radius; d <= radius; ++d)
						sum += line[static_cast<size_t>(std::clamp(k + d, 0, length - 1))];
					field[axis == 0 ? index(k, l) : index(l, k)] = sum / static_cast<float>(2 * radius + 1);
				}
			}
		}
	}
}

void MapViewCache::sendAtmosphereField(const std::shared_ptr<IMapRendererContext> & context)
{
	const int3 mapSize = context->getMapSize();
	const int level = model->getLevel();
	const int width = mapSize.x;
	const int height = mapSize.y;
	const size_t count = static_cast<size_t>(width * height);
	auto index = [&](int x, int y) { return static_cast<size_t>(y * width + x); };

	std::vector<float> mist(count, 0.0f);
	std::vector<float> cloud(count, 0.0f);
	std::vector<float> variation(count, 0.0f);
	for(int y = 0; y < height; ++y)
	{
		for(int x = 0; x < width; ++x)
		{
			const int3 tile(x, y, level);
			const MapAtmosphere atmosphere = mapRenderer->atmosphereAt(*context, tile);
			const size_t i = index(x, y);
			mist[i] = std::max(atmosphere.mist, terrainMist(context->getMapTile(tile).getTerrainID()));
			cloud[i] = atmosphere.cloud;
			variation[i] = terrainVariation(context->getMapTile(tile).getTerrainID());

			// 水边：水陆相邻的图块起一层薄雾（两边都算），开阔水面不起
			const bool water = context->getMapTile(tile).getTerrain()->isWater();
			bool shore = false;
			for(int dy = -1; dy <= 1 && !shore; ++dy)
				for(int dx = -1; dx <= 1 && !shore; ++dx)
				{
					const int3 neighbour(x + dx, y + dy, level);
					shore = context->isInMap(neighbour) && context->getMapTile(neighbour).getTerrain()->isWater() != water;
				}
			if(shore)
				mist[i] = std::max(mist[i], 0.3f);
		}
	}

	// 向四周晕开：雾漫出两三个图块，云绕着山铺得更开。可分离的盒式模糊各做两遍，近似高斯；
	// 取模糊值与原值的较大者再略降，山脚、林中仍最浓，边缘渐淡
	auto spread = [&](std::vector<float> & field, int radius, float keep)
	{
		std::vector<float> blurred = field;
		blurTileField(blurred, width, height, radius, 2);
		for(size_t i = 0; i < count; ++i)
			field[i] = std::max(blurred[i] * 1.6f, field[i] * keep);
	};
	spread(mist, 2, 0.85f);
	spread(cloud, 2, 0.9f); // 云只贴着山，不往外铺太远


	// 最外一圈图块清零：着色器按地图坐标取样、地图外夹到边缘的值，不清零的话雾和云会铺满地图外的黑边（2026-11-17 实机）
	for(int y = 0; y < height; ++y)
	{
		for(int x = 0; x < width; ++x)
		{
			if(x == 0 || y == 0 || x == width - 1 || y == height - 1)
			{
				mist[index(x, y)] = 0.0f;
				cloud[index(x, y)] = 0.0f;
			}
		}
	}

	auto encode = [](float value) { return static_cast<uint8_t>(std::round(std::clamp(value, 0.0f, 1.0f) * 255.0f)); };
	std::vector<uint8_t> rgba(count * 4, 0);
	for(size_t i = 0; i < count; ++i)
	{
		rgba[i * 4 + 1] = encode(mist[i]);
		rgba[i * 4 + 2] = encode(cloud[i]);
		rgba[i * 4 + 3] = encode(variation[i]);
	}
	ENGINE->screenHandler().setMapAtmosphereField(rgba, Point(width, height));
}

void MapViewCache::sendTerrainField(const std::shared_ptr<IMapRendererContext> & context)
{
	// 材质图集只读一次：模组 Content/Data/MapMaterials.png（地形模组 tools/materials.py 生成），没有就不铺地表材质
	if(!terrainMaterialsSent)
	{
		terrainMaterialsSent = true;
		const ResourcePath atlasPath("DATA/MAPMATERIALS", EResType::IMAGE);
		if(CResourceHandler::get()->existsResource(atlasPath))
		{
			auto [data, size] = CResourceHandler::get()->load(atlasPath)->readAll();
			ENGINE->screenHandler().setMapTerrainMaterials(std::vector<uint8_t>(data.get(), data.get() + size));
		}
		else
			logGlobal->info("No adventure map terrain materials (DATA/MAPMATERIALS) - terrain keeps its tile art");
	}

	// R：每个图块的地形。0~7 依次是泥地、沙地、草地、雪地、沼泽、碎石、地下、岩浆（与 TerrainId 的编号、材质图集的行一致，
	// 着色器只给这几种铺地表材质），8 水面、9 岩石，255 为模组新增的地形，照旧显示图块原画。粒子按它放飘雪、余烬、瘴气、雨丝。
	// G、B、A（2026-11-18，按地形的天气）：热浪（沙地、岩浆）、瘴气（沼泽）、熔岩照到这里的红光，都向四周晕开，
	// 着色器按地图坐标线性取样，所以地形交界处是渐变，不是按图块一刀切
	const int3 mapSize = context->getMapSize();
	const int level = model->getLevel();
	const int width = mapSize.x;
	const int height = mapSize.y;
	const size_t count = static_cast<size_t>(width * height);

	std::vector<uint8_t> rgba(count * 4, 0);
	std::vector<float> heat(count, 0.0f);
	std::vector<float> miasma(count, 0.0f);
	std::vector<float> lava(count, 0.0f);
	for(int y = 0; y < height; ++y)
	{
		for(int x = 0; x < width; ++x)
		{
			const int id = context->getMapTile(int3(x, y, level)).getTerrainID().getNum();
			const size_t i = static_cast<size_t>(y * width + x);
			rgba[i * 4] = id >= ETerrainId::DIRT && id <= ETerrainId::ROCK ? static_cast<uint8_t>(id) : 255;
			heat[i] = id == ETerrainId::SAND ? 0.7f : (id == ETerrainId::LAVA ? 1.0f : 0.0f);
			miasma[i] = id == ETerrainId::SWAMP ? 1.0f : 0.0f;
			lava[i] = id == ETerrainId::LAVA ? 1.0f : 0.0f;
		}
	}

	// 热浪、瘴气只漫出约一个图块；熔岩的红光照得远一些，约一个半图块。取模糊值与原值的较大者，地形内部保持满值
	std::vector<float> heatSpread = heat;
	std::vector<float> miasmaSpread = miasma;
	std::vector<float> lavaSpread = lava;
	blurTileField(heatSpread, width, height, 1, 1);
	blurTileField(miasmaSpread, width, height, 1, 2);
	blurTileField(lavaSpread, width, height, 1, 2);

	auto encode = [](float value) { return static_cast<uint8_t>(std::round(std::clamp(value, 0.0f, 1.0f) * 255.0f)); };
	for(size_t i = 0; i < count; ++i)
	{
		rgba[i * 4 + 1] = encode(std::max(heatSpread[i] * 1.3f, heat[i]));
		rgba[i * 4 + 2] = encode(std::max(miasmaSpread[i] * 1.4f, miasma[i]));
		rgba[i * 4 + 3] = encode(std::max(lavaSpread[i] * 1.5f, lava[i]));
	}
	ENGINE->screenHandler().setMapTerrainField(rgba, Point(width, height));
}

void MapViewCache::sendDayProgress(const std::shared_ptr<IMapRendererContext> & context)
{
	const auto now = std::chrono::steady_clock::now();
	if(now < nextDayProgress)
		return;
	nextDayProgress = now + std::chrono::milliseconds(250);
	const auto [day, progress] = context->dayProgress();
	ENGINE->screenHandler().setMapDayProgress(day, progress);
}

/// 一类物件的光（2026-11-18）：颜色已乘强度。灯笼、火把偏橙，熔岩偏红，法光偏青紫
struct LightProfile
{
	float height;
	float radius;
	float r;
	float g;
	float b;
	float halo;
	MapLight::Flicker flicker;
};

static std::optional<LightProfile> lightProfile(const CGObjectInstance * object)
{
	using Flicker = MapLight::Flicker;
	switch(object->ID.getNum())
	{
		case Obj::TOWN: // 满城灯火：光源在城中央、挂得高，罩住整座城
			return LightProfile{1.4f, 4.6f, 0.98f, 0.68f, 0.36f, 0.35f, Flicker::STEADY};
		case Obj::CAMPFIRE:
			return LightProfile{0.4f, 2.6f, 1.25f, 0.70f, 0.28f, 0.65f, Flicker::FIRE};
		case Obj::PILLAR_OF_FIRE:
			return LightProfile{1.2f, 3.0f, 1.30f, 0.72f, 0.26f, 0.7f, Flicker::FIRE};
		case Obj::FIERY_FIELDS:
			return LightProfile{0.3f, 2.4f, 0.95f, 0.48f, 0.17f, 0.35f, Flicker::FIRE};
		case Obj::LIGHTHOUSE:
			return LightProfile{2.5f, 5.0f, 1.10f, 0.95f, 0.64f, 0.6f, Flicker::STEADY};
		case Obj::LAVA_FLOW:
		case Obj::LAVA_LAKE:
			return LightProfile{0.2f, 2.4f, 1.00f, 0.36f, 0.10f, 0.4f, Flicker::LAVA};
		case Obj::VOLCANO:
			return LightProfile{1.5f, 3.2f, 1.10f, 0.40f, 0.11f, 0.5f, Flicker::LAVA};
		case Obj::MONOLITH_ONE_WAY_ENTRANCE:
		case Obj::MONOLITH_ONE_WAY_EXIT:
		case Obj::MONOLITH_TWO_WAY:
			return LightProfile{0.8f, 2.3f, 0.42f, 0.60f, 0.95f, 0.55f, Flicker::MAGIC};
		case Obj::SUBTERRANEAN_GATE:
			return LightProfile{0.6f, 2.2f, 0.56f, 0.42f, 0.88f, 0.45f, Flicker::MAGIC};
		case Obj::MAGIC_WELL:
		case Obj::MAGIC_SPRING:
			return LightProfile{0.4f, 2.0f, 0.30f, 0.55f, 0.78f, 0.45f, Flicker::MAGIC};
		case Obj::SHRINE_OF_MAGIC_INCANTATION:
		case Obj::SHRINE_OF_MAGIC_GESTURE:
		case Obj::SHRINE_OF_MAGIC_THOUGHT:
			return LightProfile{0.8f, 2.0f, 0.54f, 0.39f, 0.75f, 0.4f, Flicker::MAGIC};
		case Obj::STAR_AXIS:
		case Obj::SCHOOL_OF_MAGIC:
		case Obj::LIBRARY_OF_ENLIGHTENMENT:
			return LightProfile{1.0f, 2.2f, 0.40f, 0.40f, 0.62f, 0.3f, Flicker::MAGIC};
		case Obj::TAVERN:
		case Obj::TEMPLE:
		case Obj::SANCTUARY:
			return LightProfile{0.8f, 2.2f, 0.75f, 0.52f, 0.30f, 0.25f, Flicker::STEADY};
		case Obj::MINE:
		case Obj::GARRISON:
		case Obj::BORDER_GATE:
		case Obj::BORDERGUARD:
			return LightProfile{0.8f, 1.8f, 0.62f, 0.40f, 0.19f, 0.25f, Flicker::FIRE};
		case Obj::HERO: // 英雄举着火把：夜里能看清自己的英雄在哪
			return LightProfile{0.7f, 2.4f, 0.72f, 0.46f, 0.23f, 0.35f, Flicker::FIRE};
		default:
			return std::nullopt;
	}
}

void MapViewCache::sendLights(const std::shared_ptr<IMapRendererContext> & context)
{
	// 光最远照 5 个图块：可见窗口外这一圈里的光也能照进来
	constexpr int reach = 5;
	const int level = model->getLevel();
	const Rect tiles = model->getTilesTotalRect();

	lights.clear();
	++litFrame;
	for(int y = tiles.top() - reach; y < tiles.bottom() + reach; ++y)
	{
		for(int x = tiles.left() - reach; x < tiles.right() + reach; ++x)
		{
			const int3 tile(x, y, level);
			if(!context->isInMap(tile))
				continue;

			for(const ObjectInstanceID & objectID : context->getObjects(tile))
			{
				if(objectID.getNum() < 0)
					continue;
				const auto id = static_cast<size_t>(objectID.getNum());
				if(id >= litStamp.size())
					litStamp.resize(id + 64, 0);
				if(litStamp[id] == litFrame)
					continue;
				litStamp[id] = litFrame;

				const CGObjectInstance * object = context->getObject(objectID);
				if(!object)
					continue;
				const std::optional<LightProfile> profile = lightProfile(object);
				if(!profile)
					continue;

				// 光源放在物件占地的中央；英雄放在他站的格子上，走路时跟着动画平滑移动
				float centerX = 0.0f;
				float centerY = 0.0f;
				int3 lookout = object->visitablePos();
				if(object->ID == Obj::HERO)
				{
					const auto * hero = dynamic_cast<const CGHeroInstance *>(object);
					if(!hero || hero->isGarrisoned())
						continue;
					const int3 anchor = object->anchorPos();
					const Point moving = context->objectImageOffset(objectID, anchor);
					const int3 standing = object->visitablePos() - anchor;
					centerX = static_cast<float>(anchor.x + standing.x) + 0.5f + static_cast<float>(moving.x) / 32.0f;
					centerY = static_cast<float>(anchor.y + standing.y) + 0.5f + static_cast<float>(moving.y) / 32.0f;
				}
				else
				{
					const std::set<int3> blocked = object->getBlockedPos();
					if(blocked.empty())
					{
						centerX = static_cast<float>(lookout.x) + 0.5f;
						centerY = static_cast<float>(lookout.y) + 0.5f;
					}
					else
					{
						for(const int3 & position : blocked)
						{
							centerX += static_cast<float>(position.x);
							centerY += static_cast<float>(position.y);
						}
						centerX = centerX / static_cast<float>(blocked.size()) + 0.5f;
						centerY = centerY / static_cast<float>(blocked.size()) + 0.5f;
					}
					if(!object->isVisitable())
						lookout = int3(static_cast<int>(centerX), static_cast<int>(centerY), level);
				}

				// 迷雾里的光不画：光照到看得见的地方会暴露藏着的城镇、营火
				if(!context->isInMap(lookout) || !context->isVisible(lookout))
					continue;

				MapLight light;
				light.x = centerX;
				light.y = centerY;
				light.height = profile->height;
				light.radius = profile->radius;
				light.r = profile->r;
				light.g = profile->g;
				light.b = profile->b;
				light.halo = profile->halo;
				light.flicker = profile->flicker;
				light.seed = static_cast<float>(objectID.getNum() % 97) * 0.61f;
				lights.push_back(light);
			}
		}
	}

	ENGINE->screenHandler().setMapLights(lights);
}

void MapViewCache::noteHeroStep(const int3 & tile)
{
	std::lock_guard lock(heroStepsMutex);
	// 只留最近几步：渲染停了很久（如打开了别的窗口）时不必一下子惊起一串
	if(pendingHeroSteps.size() >= 8)
		pendingHeroSteps.erase(pendingHeroSteps.begin());
	pendingHeroSteps.push_back(tile);
}

float MapViewCache::roostAt(const int3 & tile) const
{
	if(tile.x < 0 || tile.y < 0 || tile.x >= roostMapSize.x || tile.y >= roostMapSize.y)
		return 0.0f;
	return birdRoosts[static_cast<size_t>(tile.y * roostMapSize.x + tile.x)];
}

/// 一片林子飞起过鸟之后，这么久之内、这么近的树上不再飞起
static constexpr auto flockCooldown = std::chrono::seconds(25);
static constexpr int flockCooldownRadius = 3;

bool MapViewCache::flockedRecently(const int3 & tile, std::chrono::steady_clock::time_point now) const
{
	for(const auto & [place, when] : recentFlocks)
		if(now - when < flockCooldown && std::abs(place.x - tile.x) <= flockCooldownRadius && std::abs(place.y - tile.y) <= flockCooldownRadius)
			return true;
	return false;
}

void MapViewCache::launchFlock(const int3 & roost, float directionX, float directionY, std::chrono::steady_clock::time_point now)
{
	// 从图块中间偏上起飞：栖鸟的图块大多是树冠
	ENGINE->screenHandler().startMapBirdFlock(static_cast<float>(roost.x) + 0.5f, static_cast<float>(roost.y) + 0.3f, directionX, directionY);
	recentFlocks.emplace_back(roost, now);
}

void MapViewCache::updateBirds(const std::shared_ptr<IMapRendererContext> & context)
{
	const auto now = std::chrono::steady_clock::now();
	std::erase_if(recentFlocks, [&](const auto & entry) { return now - entry.second >= flockCooldown; });

	std::vector<int3> steps;
	{
		std::lock_guard lock(heroStepsMutex);
		steps.swap(pendingHeroSteps);
	}

	std::uniform_real_distribution<float> unit(0.0f, 1.0f);
	const int level = model->getLevel();

	// 1. 英雄走到树林边：两格内有鸟栖的树，七成机会惊起一群，背着英雄飞走
	for(const int3 & step : steps)
	{
		if(step.z != level || unit(birdRandom) > 0.7f)
			continue;

		std::optional<int3> roost;
		float best = 0.0f;
		for(int dy = -2; dy <= 2; ++dy)
		{
			for(int dx = -2; dx <= 2; ++dx)
			{
				const int3 tile = step + int3(dx, dy, 0);
				const float strength = roostAt(tile);
				if(strength < 0.2f || !context->isInMap(tile) || !context->isVisible(tile) || flockedRecently(tile, now))
					continue;

				const float score = strength * (0.5f + 0.5f * unit(birdRandom));
				if(score > best)
				{
					best = score;
					roost = tile;
				}
			}
		}
		if(!roost)
			continue;

		// 背着英雄飞，方向随机偏一点；正好落在英雄脚下时随便挑个方向
		float directionX = static_cast<float>(roost->x - step.x);
		float directionY = static_cast<float>(roost->y - step.y);
		float angle = std::hypot(directionX, directionY) > 0.1f ? std::atan2(directionY, directionX) : unit(birdRandom) * 6.2832f;
		angle += (unit(birdRandom) - 0.5f) * 1.0f;
		launchFlock(*roost, std::cos(angle), std::sin(angle), now);
	}

	// 2. 没人惊动时，林子里也偶尔自己飞起一群（顺着风飞），地图看上去才是活的
	if(nextAmbientFlock == std::chrono::steady_clock::time_point{})
		nextAmbientFlock = now + std::chrono::seconds(8 + static_cast<int>(unit(birdRandom) * 12.0f));

	if(now < nextAmbientFlock)
		return;

	nextAmbientFlock = now + std::chrono::seconds(18 + static_cast<int>(unit(birdRandom) * 22.0f));

	std::vector<int3> candidates;
	const Rect tiles = model->getTilesTotalRect();
	for(int y = tiles.top(); y < tiles.bottom(); ++y)
	{
		for(int x = tiles.left(); x < tiles.right(); ++x)
		{
			const int3 tile(x, y, level);
			if(roostAt(tile) >= 0.5f && context->isInMap(tile) && context->isVisible(tile) && !flockedRecently(tile, now))
				candidates.push_back(tile);
		}
	}
	if(candidates.empty())
		return;

	const int3 roost = candidates[std::min(candidates.size() - 1, static_cast<size_t>(unit(birdRandom) * static_cast<float>(candidates.size())))];
	const float angle = std::atan2(0.34f, 0.94f) + (unit(birdRandom) - 0.5f) * 2.4f;
	launchFlock(roost, std::cos(angle), std::sin(angle), now);
}

void MapViewCache::drawCachedArea(Canvas & target, const Rect & cacheArea, const Point & position, const Point & size) const
{
	const Canvas ground(*terrain, cacheArea);

	if(model->getCacheTileSize() == model->getSingleTileSize())
		target.draw(ground, position);
	else
		target.drawScaled(ground, position, size);

	if(!layered)
		return;

	target.drawLayerPremultiplied(Canvas(*objectLayer, cacheArea), position, size);
	target.drawLayerPremultiplied(Canvas(*swayLayer, cacheArea), position, size);
	target.drawLayerPremultiplied(Canvas(*overlayLayer, cacheArea), position, size);
}

void MapViewCache::update(const std::shared_ptr<IMapRendererContext> & context)
{
	// 拼图地图要整体变灰，水面特效会在灰图上加回颜色，所以灰度视图不分层
	wantLayers = useGpuLayer && !context->filterGrayscale() && ENGINE->screenHandler().isMapLayeringSupported();

	ensureCanvases();

	// 分层才有着色器做风场和光影：这时树木定格在第 0 帧、画进摇摆层，有起伏图的物件只画接触阴影；否则照原来逐帧晃动、画原来的阴影。
	// 缓存存缩小图块（图块不到 16 像素）时也按不分层画物件：摇摆层、风场、起伏层这时不画（见 drawPendingLayers）
	mapRenderer->setLayeredEffects(layered && model->getCacheTileSize() == MapViewModel::getNativeTileSize());

	if(layered && depthFieldLevel != model->getLevel())
	{
		sendDepthField(context);
		sendVegetationField(context);
		sendAtmosphereField(context);
		sendTerrainField(context);
	}

	if(layered)
	{
		updateBirds(context);
		sendDayProgress(context);
		sendLights(context);
	}

	Rect dimensions = model->getTilesTotalRect();
	bool mapResized = cachedSize != model->getSingleTileSize();

	if(mapResized || dimensions.w != terrainChecksum.shape()[0] || dimensions.h != terrainChecksum.shape()[1])
	{
		boost::multi_array<TileChecksum, 2> newCache;
		newCache.resize(boost::extents[dimensions.w][dimensions.h]);
		terrainChecksum.resize(boost::extents[dimensions.w][dimensions.h]);
		terrainChecksum = newCache;
	}

	if(mapResized || dimensions.w != tilesUpToDate.shape()[0] || dimensions.h != tilesUpToDate.shape()[1])
	{
		boost::multi_array<bool, 2> newCache;
		newCache.resize(boost::extents[dimensions.w][dimensions.h]);
		tilesUpToDate.resize(boost::extents[dimensions.w][dimensions.h]);
		tilesUpToDate = newCache;
	}

	// Refresh whatever the renderer can resolve once instead of per tile
	mapRenderer->prepareFrame(*context);

	const int share = std::max(animationRedrawShareMinimum, (animatedTilesDirtyBefore + animationCatchUpFrames - 1) / animationCatchUpFrames);

	// within a burst the share may only grow - the dirty count falls as tiles are drawn
	animatedTileRedrawBudget = drainingAnimationBurst ? std::max(animatedTileRedrawBudget, share) : share;
	animatedTilesRedrawn = 0;
	animatedTilesDirty = 0;

	for(int y = dimensions.top(); y < dimensions.bottom(); ++y)
		for(int x = dimensions.left(); x < dimensions.right(); ++x)
			updateTile(context, {x, y, model->getLevel()});

	drawPendingLayers(context);

	drainingAnimationBurst = animatedTilesDirty > animatedTilesRedrawn;
	animatedTilesDirtyBefore = animatedTilesDirty;

	cachedSize = model->getSingleTileSize();
	cachedLevel = model->getLevel();
	updatedTilesRect = dimensions;
	updatedThisFrame = true;
}

bool MapViewCache::isUpdatedThisFrame() const
{
	return updatedThisFrame
		&& updatedTilesRect == model->getTilesTotalRect()
		&& cachedSize == model->getSingleTileSize()
		&& cachedLevel == model->getLevel();
}

void MapViewCache::forEachCachedBand(const std::function<void(const Rect &, const Point &, const Point &)> & visit) const
{
	const Rect tilesRect = model->getTilesTotalRect();
	const Point tileSize = model->getSingleTileSize();
	const Point cacheTileSize = model->getCacheTileSize();
	const int width = tilesRect.w;
	const int height = tilesRect.h;

	if(width <= 0 || height <= 0)
		return;

	// Screen position of the tile in the top left corner of the visible window, always within
	// one tile of the origin since the view may be scrolled by a fraction of a tile.
	const Point origin = model->getTargetTileArea(int3(tilesRect.x, tilesRect.y, model->getLevel())).topLeft();

	// Position of that same tile inside the cache, which stores tiles wrapped around
	// on both axes (tile x lives at slot x modulo width).
	const int firstSlotX = ((tilesRect.x % width) + width) % width;
	const int firstSlotY = ((tilesRect.y % height) + height) % height;

	// Because of the wrapping the visible window is split into at most two bands per axis - so
	// at most four rectangles, each contiguous both in the cache and on screen.
	const std::array<std::pair<int, int>, 2> columns = {{ // {first slot, slot count}
		{firstSlotX, width - firstSlotX},
		{0, firstSlotX}
	}};
	const std::array<std::pair<int, int>, 2> rows = {{
		{firstSlotY, height - firstSlotY},
		{0, firstSlotY}
	}};

	int offsetX = 0;
	for(const auto & column : columns)
	{
		int offsetY = 0;
		for(const auto & row : rows)
		{
			if(column.second > 0 && row.second > 0)
			{
				Rect cacheArea(
					column.first * cacheTileSize.x,
					row.first * cacheTileSize.y,
					column.second * cacheTileSize.x,
					row.second * cacheTileSize.y);

				Point targetPosition = origin + Point(offsetX * tileSize.x, offsetY * tileSize.y);
				Point targetSize(column.second * tileSize.x, row.second * tileSize.y);

				visit(cacheArea, targetPosition, targetSize);
			}
			offsetY += row.second;
		}
		offsetX += column.second;
	}
}

void MapViewCache::renderCachedTiles(Canvas & target)
{
	forEachCachedBand([&](const Rect & cacheArea, const Point & targetPosition, const Point & targetSize)
	{
		drawCachedArea(target, cacheArea, targetPosition, targetSize);
	});
}

void MapViewCache::present(const Rect & targetArea)
{
	ensureCanvases();

	// The cache is handed over rather than copied here: reading this render target in the middle
	// of a frame makes a tiling GPU resolve it right then, which on some Android drivers stalls
	// every process on the device. Drawn while the frame is composed, the same read is free.
	const int scaling = ENGINE->screenHandler().getScalingFactor();
	std::vector<PresentedRegion> regions;

	forEachCachedBand([&](const Rect & cacheArea, const Point & targetPosition, const Point & targetSize)
	{
		// The cache canvas holds its pixels at the scaling factor, so the region inside its
		// texture is not the logical rectangle. Letting the canvas work it out keeps this
		// identical to what the blit into a layer would have read.
		const Canvas band(*terrain, cacheArea);

		regions.push_back({band.getRenderArea(), Rect(targetArea.topLeft() + targetPosition * scaling, targetSize * scaling)});
	});

	if(layered)
	{
		// 着色器要知道缓存的环形排布：槽位数，以及可见窗口左上角图块是哪个、放在哪个槽位
		const Rect tilesRect = model->getTilesTotalRect();
		const Point slots = tilesRect.dimensions();

		MapLayerSet layers;
		layers.objects = objectLayer.get();
		layers.sway = swayLayer.get();
		layers.overlay = overlayLayer.get();
		layers.waterMask = waterMaskLayer.get();
		layers.windMask = windMaskLayer.get();
		layers.relief = reliefLayer.get();
		layers.splat = splatLayer.get();
		layers.slots = slots;
		layers.worldOrigin = tilesRect.topLeft();
		layers.slotOrigin = Point(((tilesRect.x % slots.x) + slots.x) % slots.x, ((tilesRect.y % slots.y) + slots.y) % slots.y);
		layers.tileSize = model->getSingleTileSize().x;

		ENGINE->screenHandler().presentMapLayers(*terrain, layers, regions);
	}
	else
	{
		ENGINE->screenHandler().presentFromCanvas(GpuRenderLayer::MAP, *terrain, regions);
	}

	// the whole window was handed over, so nothing is waiting to be drawn from the cache
	std::fill_n(tilesUpToDate.data(), tilesUpToDate.num_elements(), true);

	cachedPosition = model->getMapViewCenter();
	overlayWasVisible = false;
	updatedThisFrame = false;
}

bool MapViewCache::needsOwnLayer(const std::shared_ptr<IMapRendererContext> & context) const
{
	return context->showImageOverlay() || context->showTextOverlay() || !vstd::isAlmostZero(context->viewTransitionProgress());
}

void MapViewCache::render(const std::shared_ptr<IMapRendererContext> & context, Canvas & target, bool fullRedraw)
{
	ensureCanvases();

	bool mapMoved = (cachedPosition != model->getMapViewCenter());
	bool textOverlayVisible = context->showTextOverlay();
	bool overlayVisible = context->showImageOverlay() || textOverlayVisible;
	bool overlayVisibilityChanged = overlayVisible != overlayWasVisible;
	// redraw text overlay backgrounds; track dirty overlay tiles if this becomes expensive.
	bool lazyUpdate = !textOverlayVisible && !overlayVisibilityChanged && !mapMoved && !fullRedraw && vstd::isAlmostZero(context->viewTransitionProgress());

	Rect dimensions = model->getTilesTotalRect();

	if(lazyUpdate)
	{
		// Only the handful of tiles that actually changed need repainting
		for(int y = dimensions.top(); y < dimensions.bottom(); ++y)
		{
			for(int x = dimensions.left(); x < dimensions.right(); ++x)
			{
				int cacheX = (terrainChecksum.shape()[0] + x) % terrainChecksum.shape()[0];
				int cacheY = (terrainChecksum.shape()[1] + y) % terrainChecksum.shape()[1];
				int3 tile(x, y, model->getLevel());

				if(tilesUpToDate[cacheX][cacheY])
					continue;

				Rect targetRect = model->getTargetTileArea(tile);
				drawCachedArea(target, model->getCacheTileArea(tile), targetRect.topLeft(), targetRect.dimensions());

				if (!fullRedraw)
					tilesUpToDate[cacheX][cacheY] = true;
			}
		}
	}
	else
	{
		// Every tile has to be repainted - copy the whole cached window at once
		// instead of issuing one blit per tile.
		renderCachedTiles(target);

		if (!fullRedraw)
			std::fill_n(tilesUpToDate.data(), tilesUpToDate.num_elements(), true);
	}

	if(context->showImageOverlay())
	{
		for(int y = dimensions.top(); y < dimensions.bottom(); ++y)
		{
			for(int x = dimensions.left(); x < dimensions.right(); ++x)
			{
				int3 tile(x, y, model->getLevel());
				auto overlay = getOverlayImageForTile(context, tile);

				if(overlay)
				{
					Rect targetRect = model->getTargetTileArea(tile);
					Point position = targetRect.center() - overlay->dimensions() / 2;
					target.draw(overlay, position);
				}
			}
		}
	}

	if(textOverlayVisible)
	{
		const auto & font = ENGINE->renderHandler().loadFont(FONT_TINY);

		for(int y = dimensions.top(); y < dimensions.bottom(); ++y)
		{
			for(int x = dimensions.left(); x < dimensions.right(); ++x)
			{
				int3 tile(x, y, model->getLevel());
				auto overlay = context->overlayText(tile);

				if(!overlay.text.empty())
				{
					Rect targetRect = model->getTargetTileArea(tile);
					Point position = targetRect.center();
					if (x % 2 == 0)
						position.y += targetRect.h / 4;
					else
						position.y -= targetRect.h / 4;

					Point dimensions(font->getStringWidth(overlay.text), font->getLineHeight());
					Rect textRect = Rect(position - dimensions / 2, dimensions).resize(2);

					target.drawColor(textRect, overlay.color);
					target.drawBorder(textRect, Colors::BRIGHT_YELLOW);
					target.drawText(position, EFonts::FONT_TINY, Colors::BLACK, ETextAlignment::CENTER, overlay.text);
				}
			}
		}
	}

	if(!vstd::isAlmostZero(context->viewTransitionProgress()))
		target.drawTransparent(*terrainTransition, Point(0, 0), 1.0 - context->viewTransitionProgress());

	cachedPosition = model->getMapViewCenter();
	overlayWasVisible = overlayVisible;
	updatedThisFrame = false;
}

void MapViewCache::exportMapLevel(const std::shared_ptr<IMapRendererContext> & context, int level, const boost::filesystem::path & path)
{
	const int3 mapSize = context->getMapSize();
	const Point tileSize = MapViewModel::getNativeTileSize();

	// rendered and written a few rows of tiles at a time, the whole map is too big for memory
	constexpr int stripHeight = 8;

	PngStripWriter writer(path, mapSize.x * tileSize.x, mapSize.y * tileSize.y);
	std::vector<uint8_t> pixels;

	mapRenderer->prepareFrame(*context);

	for(int stripY = 0; stripY < mapSize.y; stripY += stripHeight)
	{
		const int rows = std::min(stripHeight, mapSize.y - stripY);

		// ignores the screen scaling, which would multiply the size of an already huge image
		CanvasImage image(Point(mapSize.x, rows) * tileSize, CanvasScalingPolicy::IGNORE);
		Canvas target = image.getCanvas();

		for(int y = 0; y < rows; ++y)
		{
			for(int x = 0; x < mapSize.x; ++x)
			{
				Canvas tile(target, Rect(Point(x, y) * tileSize, tileSize));
				mapRenderer->renderTile(*context, tile, int3(x, stripY + y, level), false);
			}
		}

		if(context->filterGrayscale())
			target.applyGrayscale();

		image.readPixelsRGBA(pixels);
		writer.writeRows(pixels.data(), rows * tileSize.y);
	}
	writer.finish();
}

void MapViewCache::createTransitionSnapshot(const std::shared_ptr<IMapRendererContext> & context)
{
	update(context);
	render(context, *terrainTransition, true);
}
