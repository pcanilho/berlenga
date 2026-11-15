---
title: "Rocks"
weight: 55
summary: "Rocks and stones as geometry, broken along each rock type's joints and weathered; placed by rockfall, scree, glacial till, rivers, shores and cliffs."
---

## Shapes

- **Rock generator** (`RockGenerator`: `rock_type`, `family`, `transport_distance`, `weathering`): rocks and stones as real geometry, broken along each rock type's joints, rounded by transport and weathered by rock type; granite, limestone, sandstone, basalt and slate.

  ![Thirty generated rocks in five rows of rock type and six columns of family, raster](media/rocks-gallery.jpg)

  *Rock generator: one rock type per row and one family per column, from sharp fresh fragments to rounded cobbles and pebbles.*

## Placement

- **Rock fields** (`RockField3D`: `rockfall_`, `scree_`, `glacial_`, `river_`, `shore_`, `cliff_` layers): rocks placed by the processes that put them there: rockfall, scree, glacial till, river beds, shores and cliff courses, streamed round the camera with colliders near it.
