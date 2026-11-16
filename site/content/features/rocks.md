---
title: "Rocks"
weight: 55
group: "World"
summary: "Rocks and stones as geometry, broken along each rock type's joints and weathered; placed by rockfall, scree, glacial till, rivers, shores and cliffs."
---

## Shapes

- **Rock generator** (`RockGenerator`: `rock_type`, `family`, `transport_distance`, `weathering`): rocks and stones as real geometry, broken along each rock type's joints, rounded by transport and weathered by rock type; joint sets at varying angles, stepped fracture faces and broken corners on fallen blocks; granite, limestone, sandstone, basalt and slate.

  ![Thirty generated rocks in five rows of rock type and six columns of family, raster](media/rocks-gallery.jpg)

  *Rock generator: one rock type per row and one family per column, from sharp fresh fragments to rounded cobbles and pebbles.*

- **Close-up detail** (`RockField3D`: `close_up_enabled`, `close_up_radius`): stones near the camera get real geometry for their grain: crystals whose cleavage faces glint in low sun, weathering pits, hairline cracks, spalled flakes and stepped fresh breaks, so the outline breaks too.

  ![A granite block's face at about 1 m in low sun, base shape and close-up shape, raster](media/rocks-closeup.jpg)

  *Close-up detail: the same granite face gains crystal relief, pits, hairline cracks and a broken top edge.*

- **Mineral glints** (`RockField3D.mineral_glints`, off by default): glossy crystals in granite and other quartz-bearing rock keep sparkling at a distance, counted per pixel footprint, and lichen, moss, water, snow and dust cover them.
- **Ground contact and aspect** (rock material: `contact_height`, `aspect_strength`; RockField3D `ground_blend` (off by default), `ground_blend_height`, `ground_material`): stones blend into the ground with a soil band at their foot, or take the ground's own layer over a band, and take wetness and snow from the weather; moss grows on shaded faces and lichen on sunlit ones.

## Placement

- **Rock fields** (`RockField3D`: `rockfall_`, `scree_`, `glacial_`, `river_`, `shore_`, `cliff_` layers; `layer_enabled`): rocks placed by the processes that put them there: rockfall, scree, glacial till, river beds, shores and cliff courses, streamed round the camera with colliders near it.
- **Breakable stones** (`RockField3D.break_stone`): stones split into pieces along rough rock breaks, and layered rock into slabs.

  ![A granite block about 3 m away, whole and broken where it lay, raster](media/rocks-break.jpg)

  *Breakable stones: the block breaks into a pile of angular pieces where it stood.*

- **Live rockfall** (`RockField3D.live_rockfall`, off by default; `live_rockfall_rate`, `live_rockfall_radius`, `live_rockfall_max`): cliff blocks fall in frost, thaw and rain, and break on impact.

- **Tors and coasts** (`layer_enabled`: `LAYER_TOR`, `LAYER_COAST`): tors and outcrops on convex crests, with granite woolsack blocks and weathering pans; sea stacks, arches and wave-cut platforms on rocky coasts, with a wet barnacle and weed band below high water and a dark splash band above.
- **Landforms** (`layer_enabled`: `LAYER_LANDFORM`): hoodoo rows on bare soft slopes, limestone pavement on flat limestone benches, rows of sandstone fins on gentle bare sandstone, about half pierced by an arch, clusters of rounded sandstone knobs (goblins) on flat bare sandstone, and bedrock slabs drilled with potholes, grinder stones inside, on steep stream reaches.
- **Cave mouths** (`RockField3D.cave_mouths`, off by default; `terrain3d`): limestone cliff feet get cave openings cut into the terrain, also on Terrain3D.
