#include <mln/renderer/dem_elevation_provider.hpp>

#include <mln/geometry/dem_data.hpp>
#include <mln/renderer/buckets/hillshade_bucket.hpp>
#include <mln/renderer/render_source.hpp>
#include <mln/renderer/render_tile.hpp>
#include <mln/tile/raster_dem_tile.hpp>

#include <algorithm>
#include <limits>

namespace mln {

DEMElevationProvider::DEMElevationProvider(const RenderSource* demSource, double exaggeration) {
    // Snapshot the elevation range of every loaded DEM tile (once, here, not per query).
    // The aggregate is the fallback for tiles with no loaded DEM, so the cover dilation can
    // re-test a not-yet-loaded frontier neighbour as if it were about as tall/deep as the
    // terrain already in view.
    //
    // A disabled source is not prepared, so its render tiles are left over from the last frame
    // it rendered and refer to tiles its pyramid has since released: reading them was a
    // use-after-free (EXC_BAD_ACCESS in DEMData::getMinElevation when terrain came back on after
    // a style reload in which the DEM source had been unused, e.g. hillshade hidden).
    if (!demSource || !demSource->isEnabled()) {
        return;
    }
    const auto renderTiles = demSource->getRawRenderTiles();
    double minEle = std::numeric_limits<double>::max();
    double maxEle = std::numeric_limits<double>::lowest();
    tileRanges.reserve(renderTiles->size());
    for (const auto& renderTile : *renderTiles) {
        const auto& tile = renderTile.getTile();
        if (tile.kind != Tile::Kind::RasterDEM) {
            continue;
        }
        const auto* demTile = static_cast<const RasterDEMTile*>(&tile);
        const auto* bucket = const_cast<RasterDEMTile*>(demTile)->getBucket();
        if (!bucket) {
            continue;
        }
        const auto& demData = bucket->getDEMData();
        const double lo = demData.getMinElevation();
        const double hi = demData.getMaxElevation();
        tileRanges.push_back({renderTile.id.canonical, Range<double>{lo * exaggeration, hi * exaggeration}});
        minEle = std::min(minEle, lo);
        maxEle = std::max(maxEle, hi);
    }
    if (minEle <= maxEle) {
        loadedRange = Range<double>{minEle * exaggeration, maxEle * exaggeration};
    }
}

std::optional<Range<double>> DEMElevationProvider::getTileElevationRange(const CanonicalTileID& id) const {
    if (tileRanges.empty()) {
        return std::nullopt;
    }

    // The tile's own DEM, or failing that the deepest loaded ancestor: an ancestor's
    // range covers this tile's area, so it stays conservative, just looser.
    const TileRange* best = nullptr;
    for (const auto& candidate : tileRanges) {
        const bool covers = candidate.id == id || id.isChildOf(candidate.id);
        if (!covers || (best && candidate.id.z <= best->id.z)) {
            continue;
        }
        best = &candidate;
        if (candidate.id == id) {
            break; // exact match; nothing looser can improve on it
        }
    }

    if (!best) {
        // No DEM covers this tile. Fall back to the range of terrain loaded in view
        // (nullopt only if nothing is loaded), so the cover dilation's frustumCull can
        // decide the tile on its (assumed) elevation instead of treating it as flat.
        return loadedRange;
    }

    // Exaggeration is applied to the mesh in the terrain vertex shader, so the bounds
    // have to carry it too, or an exaggerated peak would still be culled.
    return best->range;
}

} // namespace mln
