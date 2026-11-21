#pragma once

// ==============================================================================
// GuiKit Geo Module - Cartography, Projections, S-52/ENC GIS & MapView
// ==============================================================================

#include "guikit/context.hpp"
#include "guikit/geo/chart_data_source.hpp"
#include "guikit/geo/geo_json_io.hpp"
#include "guikit/geo/geo_types.hpp"
#include "guikit/geo/gis_render_engine.hpp"
#include "guikit/geo/map_grid.hpp"
#include "guikit/geo/map_view.hpp"
#include "guikit/geo/polygon_triangulator.hpp"

namespace guikit::geo {

/// Ensures built-in embedded geo assets (e.g. `assets/geo/demo_bay.geojson`) are registered with `guikit::Assets`.
void RegisterEmbeddedAssets() noexcept;

/// Opens an interactive multi-projection geographic MapView container scoped to `ui`.
[[nodiscard]] inline MapViewScope MapView(
    Context& ui,
    MapViewOptions opts = {},
    keel::SourceLocation loc = keel::SourceLocation::current()
) {
    return ui.Open<guikit::MapView>(opts, loc);
}

} // namespace guikit::geo
