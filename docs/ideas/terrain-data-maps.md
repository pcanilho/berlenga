# World data maps for placement (vegetation gap 4)

Placement rules for grass, trees, reeds and swamps need to know more about the ground than
its height: how steep it is, whether it is a ridge or a hollow, where water collects, how wet
the soil is, and how far it is to open water. No node provides these as maps today.
`TerrainDeformer3D` knows heights, rivers and water surfaces know their own extent, and
nothing combines them.

## How others do it

- **Horizon Zero Dawn** (van Muijden, GDC 2017, "GPU-based run-time procedural placement"):
  placement density is a graph of *world data* textures per tile: height, slope, distance to
  roads and rivers, water depth, painted biome masks. Every plant species reads them.
- **Far Cry 5** (Carrier, GDC 2018, "Procedural world generation of Far Cry 5"): Houdini bakes
  terrain analysis per tile: slope, curvature ("occlusion" in hollows), flow maps from an
  erosion pass, and distance to water. Biome rules read them, for example reeds where flow is
  high and the ground is flat.
- **Unreal PCG** (5.7): "Landscape data" gives height, normal and layer weights; slope and
  curvature filters are built in; distance to water comes from the Water plugin's water body
  queries.
- **Houdini HeightField tools**: slope, curvature, flow (accumulation), and a "wetness" from
  flow and slope. This is what most studios bake from.
- **GIS**, where all of these come from:
  - slope and aspect: Horn (1981), "Hill shading and the reflectance map", the 3x3 gradient
    that every GIS uses;
  - curvature: Zevenbergen and Thorne (1987), profile and plan curvature from a quadratic patch;
  - depression filling before flow routing: Priority-Flood (Barnes, Lehman and Mulla 2014,
    building on Planchon and Darboux 2002), so water leaves pits instead of stopping;
  - flow accumulation: multiple flow direction (Freeman 1991; Quinn et al. 1991; Holmgren
    1994), which spreads flow over all lower neighbours and gives the smooth wet valleys that
    plants follow. D8 (O'Callaghan and Mark 1984) gives single-cell lines, too thin for
    vegetation;
  - topographic wetness index TWI = ln(a / tan b) (Beven and Kirkby 1979), a = upslope area
    per unit contour width, b = slope. The standard predictor of soil moisture and of where
    wetland plants grow, so it is the swamp map;
  - exact Euclidean distance transform: Felzenszwalb and Huttenlocher (2012), linear time.

Godot addons read: ProtonScatter has slope and height filters computed per point by rays,
no maps. Terrain3D's instancer places by painted masks. Neither has flow, wetness or water
distance.

## What we build

`TerrainDataMap3D` (terrain_deform module, next to `TerrainDeformer3D`): a square of `size`
metres centred on the node, `resolution` cells a side, baked on demand.

- **Height source**, in order: `height_source` node with `get_height(Vector3)`
  (`TerrainDeformer3D`), or with `get_data().get_height(Vector3)` (Terrain3D, by name, never
  bundled); else rays down against `ground_collision_mask` (any collider, HeightMapShape3D
  included).
- **Water sources**: `water_sources` node paths. A node with `sample_height(Vector3)`
  (rivers, which return -inf outside) gives its surface height per cell; a node with only
  `get_water_level()` (water surfaces, oceans, lakes) is a level plane. The water solvers are
  not touched.
- **Maps**, two RGBA float textures:
  - `terrain_map`: height, slope (radians, Horn), aspect (radians from +Z, clockwise towards
    +X), curvature (Zevenbergen-Thorne general curvature, 1/m, positive in hollows where
    water collects);
  - `water_map`: flow (log10 of upslope area in m2, MFD with Holmgren exponent 1.1 on the
    Priority-Flood filled surface), wetness (TWI), water depth (m, 0 on land), signed
    distance to water (m, negative under water).
- **Queries** for CPU placement: `get_slope_at`, `get_curvature_at`, `get_flow_at`,
  `get_wetness_at`, `get_water_depth_at`, `get_water_distance_at`, `get_height_at`
  (bilinear).
- **Shaders**: global uniforms `terrain_data_map`, `terrain_water_map`, `terrain_data_rect`,
  like `WeatherMap3D`; `TerrainDataMap3D.get_shader_code()` gives `terrain_data(world_pos)` and
  `terrain_water(world_pos)` for placement compute and materials (wet hollows, snow on flat
  ground). The UV is clamped to the outer texel centres, so map edges never wrap.

CPU, not a GPU pass as vegetation.md first said: the maps are baked once per tile (and again
after terrain edits), Priority-Flood and MFD are sequential by nature, and a 512 x 512 bake
is expected to take tens of milliseconds. The bench measures it; if a tile bake is too slow
for streaming it moves to a worker thread first.

## Results

Bench `docs/bench/terrain_data` (`--test`): a V-shaped valley 256 m across with bumps, a 0.8 m
pit on the floor, a river down the floor (a stand-in with `sample_height`) and a lake at the
low end (a stand-in with `get_water_level`). The ground is a HeightMapShape3D, so heights come
from rays; a tilted plane checks the `get_height` path. All checks pass at 256 and 512 cells:

- heights match the analytic ground within 5 cm;
- the valley floor carries 30x to 100x the flow of the hillside, and flow grows downstream past
  the pit (Priority-Flood fills it);
- the wetness index is 12.6 on the valley floor and 6.7 on the hillside;
- beside the river the land drains sideways into it (no valley-sized flow on the bank);
- lake depth within 1 cm of the analytic value; dry land depth 0;
- signed distance: exactly 10.0 m and -5.0 m from a straight shore, within one cell of 0 at
  the river bank;
- curvature positive on the valley floor; plane slope 11.310 degrees (analytic 11.310) and
  aspect 270.0 degrees.

The shader path renders the valley coloured from the maps (green where wet, blue water).

Cost (one bake, cloud CPU, including 1 ray per cell): 256 x 256 37 to 47 ms, 512 x 512 150 to
210 ms, 1024 x 1024 760 ms. No per-frame cost; the textures are 2 x 16 bytes a cell (8 MB at
512). Streaming tiles (stage 1) will run the bake on a worker thread so it never costs a frame.

Limits: a water surface is a level plane over the whole map (lakes in basins are right; a
lake with ground below its level outside its outline would be flooded there). Flow is local to
the map, so a tile's edges do not see water flowing in from outside it.
