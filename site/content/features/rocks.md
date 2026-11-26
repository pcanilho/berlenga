---
title: "Rocks"
weight: 55
group: "World"
summary: "Rocks and stones as geometry, broken along each rock type's joints and weathered; placed by rockfall, scree, glacial till, rivers, shores and cliffs."
---

## Shapes

- **Rock generator** (`RockGenerator`: `rock_type`, `family`, `transport_distance`, `weathering`): rocks and stones as real geometry, broken along each rock type's joints, rounded by transport and weathered by rock type; joint sets at varying angles, stepped fracture faces and broken corners on fallen blocks; granite, limestone, sandstone, basalt and slate.
- **Tafoni** (RockGenerator `salt`): salt weathering carves honeycomb hollows and tafoni into coastal and dry inland stone.

  ![Thirty generated rocks in five rows of rock type and six columns of family, raster](media/rocks-gallery.jpg)

  *Rock generator: one rock type per row and one family per column, from sharp fresh fragments to rounded cobbles and pebbles.*

- **Close-up detail** (`RockField3D`: `close_up_enabled`, `close_up_radius`): stones near the camera get real geometry for their grain: crystals whose cleavage faces glint in low sun, weathering pits, hairline cracks, spalled flakes and stepped fresh breaks, so the outline breaks too.

  ![A granite block's face at about 1 m in low sun, base shape and close-up shape, raster](media/rocks-closeup.jpg)

  *Close-up detail: the same granite face gains crystal relief, pits, hairline cracks and a broken top edge.*

- **Mineral glints** (`RockField3D.mineral_glints`): glossy crystals in granite and other quartz-bearing rock keep sparkling at a distance, counted per pixel footprint, and lichen, moss, water, snow and dust cover them.
- **Ground contact and aspect** (rock material: `contact_height`, `aspect_strength`; RockField3D `ground_contact`, `ground_blend`, `ground_blend_height`, `ground_material`): stones sit in the soil, with damp, gritty ground and fines banked uphill round each foot; the foot blends into the ground with a soil band, or takes the ground's own grain with a damp, darker foot (the landscape's TerrainMaterial3D when `ground_material` is unset), and stones take wetness and snow from the weather; moss grows on shaded faces and lichen on sunlit ones.
- **Moss cushions** (`moss_cushions`): on near stones, moss grows as real cushions 1 to 3.5 cm thick, with tufts that hold up under path tracing.

## Placement

- **Rock fields** (`RockField3D`: `rockfall_`, `scree_`, `glacial_`, `river_`, `shore_`, `cliff_` layers; `layer_enabled`; `cliff_block_max_slope`; `far_thinning`, `far_radius`, `far_min_size`, `far_widen`): rocks placed by the processes that put them there: rockfall, scree, glacial till, river beds, shores and cliff courses, the coursed wall blocks covering even sheer faces, streamed round the camera with colliders near it; past the near field, only the larger rocks carry on out to the horizon, grown so the slope keeps its stone cover.
- **Cliff pillars** (`RockField3D`: `cliff_kit`, `cliff_recess`): steep fluted faces are built of joint-bounded rock pillars in columns, with the drawn face stepping back behind them.

  ![A fluted cliff foot, left the bare heightfield face, right built of rock pillars, raster](media/rocks-cliff-kit.jpg)

  *Cliff pillars: the same cliff foot, the heightfield face on the left, columns of jointed rock pillars on the right.*

- **Ground detail** (`RockField3D`: `ground_detail`, `ground_detail_pixels`, `ground_detail_cover`, `ground_detail_rivers`, `ground_detail_tiles`, `stones_react`): pebbles and gravel from 1 to 8 cm as real stones near the camera, sized and sorted like the beach, scree or river bed around them (coarse on riffles, silted in pools, each stone tilted against the flow), each drawn only while it stays a few pixels tall; the path tracer traces every one, grouped into tiles of piles shared across cells so it handles far fewer instances, and small stones off screen leave its scene with no change to the image (`rendering/pathtracing/offscreen_record_cull`). Beyond that the ground takes on their cover, colour and shading toward the sun, so no edge shows where the stones stop. Feet and wheels press the stones into the bed, push some aside and flick a few loose, which settle again. Stones buried by the snowpack are left out, so only coarse ones show through (`snow_burial`).

  ![A shingle shore at low eye height, ground detail off and on, raster](media/rocks-ground-detail.jpg)

  *Shingle shore: before, scattered pebbles on bare sand; after, a band of gravel and small pebbles runs along the shore below the cobbles.*

- **Frost-sorted ground** (`RockField3D.frost_sorting`): on cold, wet, gentle soil, frost sorts the stones into circles round fine-soil centres, drawn out into stripes on slopes.
- **Runoff gravel** (`RockField3D.runoff_gravel`): coarse lag gathers on gully floors and fine grains spread on the fans below.
- **Rain splash pedestals** (`RockField3D.rain_pedestals`): on bare, rained-on soil, stones stand on little columns of the soil they shielded from the splash, and some topple off.
- **Breakable stones** (`RockField3D.break_stone`): stones split into pieces along rough rock breaks, and layered rock into slabs.

  ![A granite block about 3 m away, whole and broken where it lay, raster](media/rocks-break.jpg)

  *Breakable stones: the block breaks into a pile of angular pieces where it stood.*

- **Live rockfall** (`RockField3D.live_rockfall`; `live_rockfall_rate`, `live_rockfall_radius`, `live_rockfall_max`): cliff blocks fall in frost, thaw and rain, and break on impact.
- **Rockfall dust** (`rockfall_dust`; needs PhysX Flow): falling and breaking stones throw up dust where they hit.

- **Tors and coasts** (`layer_enabled`: `LAYER_TOR`, `LAYER_COAST`; rock material `salt_bloom`): tors and outcrops on convex crests, with granite woolsack blocks and weathering pans; sea stacks cut into sharp-edged joint blocks with spray pitting only on upward faces, arches and wave-cut platforms on rocky coasts, with a wet barnacle and weed band below high water and a dark splash band above; on sheltered rock, sea spray salt dries to a white crust that is gone while the rock is wet.
- **Landforms** (`layer_enabled`: `LAYER_LANDFORM`): hoodoo rows on bare soft slopes, limestone pavement on flat limestone benches, rows of sandstone fins on gentle bare sandstone, about half pierced by an arch, clusters of rounded sandstone knobs (goblins) on flat bare sandstone, and bedrock slabs drilled with potholes, grinder stones inside, on steep stream reaches.
- **Ledge slabs** (`RockField3D.ledge_slabs`): caprock slabs overhang the lips of hard-bed benches, broken along their joints.
- **Caves** (`CaveNetwork3D.build_meshes()` or `follow_target`, `speleothems`, `scallops`, `cave_streams`, `add_sea_cave()`, `sea_surge`; `terrain3d`): cave networks grow behind limestone cliff-foot entrances (Paris 2021) and are meshed into the hillside around the opening, also on Terrain3D; stalactites, stalagmites and columns grow where water drips through the roof, and flow scallops the walls; streams run over washed, wet gravel beds. Sea caves follow cracks into sea cliffs, their walls stepped by beds of uneven thickness, flooded at the mouth by the sea, which surges in with each swell so bodies wade and swim and the walls get wet, some ending in a chamber with a blowhole to the cliff top.

