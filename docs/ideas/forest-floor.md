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

## References

- Ellenberg, *Vegetation Mitteleuropas mit den Alpen*, 5th ed., 1996; Ellenberg and Leuschner,
  6th ed., 2010.
- Gilliam, "The ecological significance of the herbaceous layer in temperate forest
  ecosystems", BioScience 57, 2007.
- van Muijden, "GPU-Based Run-Time Procedural Placement in Horizon: Zero Dawn", GDC 2017.
