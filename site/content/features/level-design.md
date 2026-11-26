---
title: "Level design"
weight: 48
group: "World"
summary: "How the world systems are set up in the editor: nodes, curves, brush nodes and painted maps."
---

Each world system is a node set up in the inspector; systems find each other through node paths and the world contracts.

## Editor tools

- **Editor preview** (`generate_on_ready`, `water_on_ready`, `auto_rebuild`, `editor_preview`): ground, water, material, data maps, plants, grass and stones appear in the editor as in the game; nothing generated is saved.
- **Mask painting** (Paint Mask in the 3D toolbar): paint any `*_map` or `*_mask` slot on the ground with Add, Subtract, Replace and Smooth; each stroke is one undo step.
- **Keep-out areas** (`WorldExclusion3D`): a closed curve, open track or box keeps plants, grass or stones out, with a soft edge.
- **Hand-placed plants** (`PlacedPlant3D`): one plant of a species, with its footprint kept clear of procedural plants and stones.
- **Biome map** (`biome_map`, `biome_channel`): fields share one painted map, a channel each; overlapping paint splits the ground.
- **Sculpt layers** (`TerrainSculptLayer3D`): painted height offsets over the generated ground; lakes and rivers settle round them, and turning a layer off restores the ground.

## Terrain

- **Generate** (`TerrainGenerator3D`: `generate_on_ready`, Generate button): generates the ground when the scene opens and writes it into the ground node on Generate.
- **Ranges and guides** (`ranges`, `TerrainGuide3D`: Raise, Lower, Bar, Coast, Beach): curves drawn with the Path3D editor shape the relief before erosion.
- **Touch-ups** (`TerrainBrush3D` under the generator): brush nodes moved with the gizmo raise, lower or flatten ground; lakes and rivers re-solve around them.
- **Painted maps** (`uplift_map`, `sea_map`): images over the generator's rectangle steer uplift and sea.
- **Ground material** (`TerrainMaterial3D`): rock, scree, soil, alluvium and sand follow the generator's geology; no splat painting.
- **Data maps** (`TerrainDataMap3D`): slope, curvature, wetness and water distance, baked once for placement rules.

## Plants, rocks and ground

- **Placement rules** (`PlantSpecies`: `slope_range`, `height_range`, `wetness_range` and others): each species is kept where its rules allow.
- **Density masks** (`VegetationField3D.density_mask`, `PlantSpecies.mask_channel`): a painted image thins or clears species by channel.
- **Rocks** (`RockField3D`): scree, river cobbles, shore stones, erratics and cliff blocks placed from the terrain.
- **Ground type** (`GroundSurface3D`: `type_map`, `mud_map`): sand, soil and mud follow painted maps and the weather.

## Weather and snow

- **Weather** (`WeatherMap3D`: `painted_map`): painted cloud and rain over a region; the rest follows the base settings.
- **Snow** (`SnowCover3D`, `SnowSurface3D`): the snowpack builds from the weather and the terrain.
