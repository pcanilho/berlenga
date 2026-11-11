# Forest floor under a closed canopy

Goal: inside a wood the ground is hidden as well as in the open, by what really covers a forest
floor, and the floor changes with the trees above it. This module owns everything under about
1.5 m (herbs, ferns, mosses, dwarf shrubs, tree seedlings, fallen twigs, cones and litter); the
trees, the woody shrubs above that height, stumps and logs come from the trees module.

## What covers a temperate forest floor

Under a closed canopy only 1 to 5% of daylight reaches the ground, so the herb layer is thin and
the floor is mostly **litter**: a continuous layer of fallen leaves or needles, 2 to 10 cm deep,
over which herbs, mosses and seedlings stand where light allows (Ellenberg 1996; Gilliam 2007,
"The ecological significance of the herbaceous layer in temperate forest ecosystems"). What
hides the ground in a wood is therefore litter first, plants second; a floor of bare soil
between plants is the giveaway of a fake forest.

The understory follows the stand type (Ellenberg and Leuschner, *Vegetation Mitteleuropas mit
den Alpen*, 2010):

| Stand | Light at the floor | Floor |
|---|---|---|
| Beech (*Fagus*) | 1 to 3% in summer | deep brown leaf litter, sparse: woodruff, wood sorrel, wood anemone in spring, beech seedlings, few ferns |
| Oak and hornbeam | 3 to 10% | litter with a rich herb layer: bramble, ivy carpets, wood anemone, bluebell, wood sedge, male fern, honeysuckle |
| Scots pine | 10 to 30% (open crowns) | needle litter, bilberry and cowberry, heather in gaps, wavy hair-grass, feather mosses, bracken |
| Norway spruce | 1 to 5% | a carpet of feather mosses on needle litter, bilberry, wood sorrel, broad buckler fern, few herbs |
| Alder carr (wet) | 5 to 15% | sedges, ferns, nettle, marsh marigold, mud |

Across all of them: **gaps** (a fallen tree, the edge) carry a dense flush of bramble,
raspberry, foxglove, grasses and seedlings, because light there is 20 to 50%; **fallen twigs,
branches and cones** lie everywhere (0.5 to 2% of the floor's area in managed woods, much more
in old woods); **mosses** climb onto roots, stones and deadwood.

## How games fill it

- Kingdom Come: Deliverance II (CryEngine): a forest floor material of scanned litter, with
  dense instanced ferns, bilberry, seedlings, fallen branches and mossy stones; the floor
  material itself carries most of the cover, plants break up the silhouette.
- Horizon Forbidden West (Decima): placement by density maps per ecotope (van Muijden GDC 2017
  for Zero Dawn), with ground debris as its own layer of small meshes.
- Star Citizen planet tech: ground cover as a layer that follows the biome's material.

The common recipe: (1) a litter ground material that matches the canopy above, (2) a 3D litter
and debris layer near the camera so the floor has relief, (3) herbs, ferns and dwarf shrubs by
light, (4) more of everything in gaps.

## What we build

1. **Stand type in the canopy map.** `PlantSpecies` gets the litter its crown drops
   (broadleaf or needle) and `VegetationField3D` writes, next to the LAI it already publishes,
   the share of conifer crowns overhead. A new rule, `RULE_CONIFER` (0 broadleaf to 1 needle),
   lets each floor plant choose its stand. The trees thread sets the property on its species.
2. **Litter as ground.** The ground under crowns blends to leaf or needle litter by that share
   (the turf ground code's counterpart, `grass_ground()` for woods), so no soil shows.
3. **Litter and debris layer.** Leaf litter (exists), needle litter, fallen twigs and branches,
   cones: low, cheap meshes drawn near only, on the near turf layer's scheme.
4. **Floor plants by stand and light.** New presets: wood sorrel, woodruff, wood anemone, ivy
   carpet, feather moss carpet, wavy hair-grass, heather, broad buckler fern (fern variant), beech
   and oak seedlings, spruce seedlings. Each placed by shade (light), conifer share and wetness.
5. **Gaps.** The shade rule already sees gaps: bramble, foxglove, bracken and grasses claim them.

Checks: the grass bench `--forest=in` in a beech, a spruce and a pine stand (stand-in trees with
their litter type); no bare soil visible at 100% crop in raster and path traced; cost per stand.

## Built: litter as the ground (step 2)

`VegetationField3D` publishes, after each populate that places crowns, `forest_floor_map` (RG8 on
half-metre cells: canopy closure from `get_shade_at()`, conifer share from `get_conifer_at()`) and
`forest_floor_rect`. `get_floor_shader_code()` gives `forest_floor(world_pos, albedo)`, which
`grass_ground()` calls first, so TerrainMaterial3D and any material on the grass ground code get
the floor with no change of their own.

- **Cover:** `smoothstep(0.12, 0.5, closure)` with noise on the edge, so litter blows a little past
  the crowns and thins where light reaches the ground; the sward's shade rule hands over there.
- **Leaves (broadleaf):** a stochastic sprite layer in the shader: jittered cells, one ovate,
  pointed leaf per cell at a random turn and size, overlapping its 3x3 neighbours (topmost by hash),
  two layers of 6 and 4.5 cm cells (leaves 7 to 11 cm, beech and oak size). Palette from beech
  litter: orange-brown fresh, dark brown decaying, a few pale and grey-brown. Midrib catches light,
  curled rims shade the leaf under them. Over dark humus.
- **Needles (conifer):** two strokes per 2.5 and 1.9 cm cell, rusty, brown and grey, over
  half-decayed duff; feather moss drifts (yellow-green, two noise scales) where spruce and pine
  floors carry their moss carpets.
- **Mixed stands** are a patchwork of the two litters (conifer share against a noise threshold), not
  their average.
- **Filtering:** each layer fades to its mean colour once a pixel's footprint (`fwidth` of the
  world position) reaches a third of its cell, so it does not alias and costs nothing far off
  (the branch skips the 3x3 search).
- **3D layer near the camera:** leaf litter and needle litter presets give relief; needle litter
  lost its mat (the ground now carries it) and keeps loose needles and cones. A 3D feather moss
  carpet had a visible outline and read as dark holes on the floor, so in the bench the moss is the
  ground's; the preset stays for cushions on logs and stones.

Bench: `docs/bench/grass --forest=in --stand=beech|spruce|pine|mixed` (stand-in trees; spruce
crowns start 4 m up, pine crowns open, opacity 0.55). Floor species by rules: leaf litter at
conifer 0 to 0.5, needle litter 0.5 to 1, bilberry 0.4 to 1, wood sorrel in deep shade, seedlings
at shade 0.4 to 0.97, twigs everywhere in shade.

## Built: floor sets per stand (steps 4 and 5)

`PlantGenerator.make_floor(stand, variants)` returns the floor species for the tree generator's
seven stand types, to append to its stand Biome after the trees and woody shrubs. New presets:
woodruff, wood anemone, ivy carpet, heather (plus needle litter, twigs, wood sorrel, seedling and
feather moss from step 3).

| Stand | Floor |
|---|---|
| Beech | bramble and foxglove in gaps, few ferns, many seedlings, woodruff, wood anemone, wood sorrel, moss, twigs, leaf litter |
| Oak, lime, ash | bramble under the crowns too, bracken and foxglove in gaps, ferns, ivy carpets, wood anemone, woodruff |
| Montane spruce-fir-beech | ferns, bilberry, wood sorrel, seedlings, both litters by the conifer rule |
| Subalpine larch-spruce | bilberry carpets (also under larch), heather in the light, few ferns |
| Pine heath | bilberry, heather, hair-grass, bracken in gaps |
| Riparian alder-willow | nettle, sedge within 15 m of water, rush within 8 m, ferns |
| Hedgerow | bramble, nettle, ivy, foxglove, fern |

Gaps need no special case: the shade rule already hands half-light to bramble, bracken and
foxglove (step 5). Bench: `docs/bench/grass --forest=in --stand=beech|oak|spruce|pine|larch|mixed`.
Triangles per plant: heather 14k and woodruff 6.4k are the heaviest; they wait for the far-distance
LOD work (stage 6) before dense use.

## References

- Ellenberg, *Vegetation Mitteleuropas mit den Alpen*, 5th ed., 1996; Ellenberg and Leuschner,
  6th ed., 2010.
- Gilliam, "The ecological significance of the herbaceous layer in temperate forest
  ecosystems", BioScience 57, 2007.
- van Muijden, "GPU-Based Run-Time Procedural Placement in Horizon: Zero Dawn", GDC 2017.
