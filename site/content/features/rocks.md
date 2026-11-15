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

- **Close-up detail** (`RockField3D`: `close_up_enabled`, `close_up_radius`): stones near the camera get real geometry for their grain: crystals, weathering pits, hairline cracks, spalled flakes and stepped fresh breaks, so the outline breaks too.

  ![A granite block's face at about 1 m in low sun, base shape and close-up shape, raster](media/rocks-closeup.jpg)

  *Close-up detail: the same granite face gains pits, hairline cracks and a broken top edge.*

- **Ground contact and aspect** (rock material: `contact_height`, `aspect_strength`): stones blend into the ground with a soil band at their foot and take wetness and snow from the weather; moss grows on shaded faces and lichen on sunlit ones.

## Placement

- **Rock fields** (`RockField3D`: `rockfall_`, `scree_`, `glacial_`, `river_`, `shore_`, `cliff_` layers; `layer_enabled`): rocks placed by the processes that put them there: rockfall, scree, glacial till, river beds, shores and cliff courses, streamed round the camera with colliders near it.
- **Tors and coasts** (`layer_enabled`: `LAYER_TOR`, `LAYER_COAST`): tors and outcrops on convex crests, with granite woolsack blocks and weathering pans; sea stacks, arches and wave-cut platforms on rocky coasts.
