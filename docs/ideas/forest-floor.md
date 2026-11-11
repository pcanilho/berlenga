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

## Fallen leaves, twigs and small leafy plants (research, step 6)

Asked for: fallen leaves that read as the trees above them, small sticks between the plants, and
small leafy plants breaking up grass and litter, with the forest floor's real amounts. The trees
module owns wood from 1 cm thick up (sticks, branches, logs, with `tree_deadwood_axis`); this
module owns leaf litter, twigs under 1 cm and the small plants.

### How much falls, and how much stays

- **Leaf fall.** A closed broadleaf stand drops about its own leaf area every year: leaf area index
  4 to 7 in beech and oak, leaf mass per area 40 to 100 g/m2 (Poorter et al. 2009), so 2.5 to 4.5
  t/ha of leaves. Young or open stands drop far less: 0.6 to 1.6 t/ha in a 10-year common garden
  (Carnol and Bazgir 2013, Table 4: beech 0.65, oak 0.54, spruce 1.2, birch 1.6 t/ha/yr).
  In leaf layers: one year's fall covers the ground about LAI times over.
- **What stays depends on the species.** Decay rates differ by about a factor five (Hobbie et al.
  2006, 14 species in one garden): ash, alder, lime and maple litter is mostly gone by summer
  (decay constant k about 1 per year or more; soil and herbs show through), beech, oak and spruce
  litter lasts two to four years (k 0.2 to 0.4), so the floor stays brown under them all year.
  With fall F (layers per year) and k, the steady layers on the ground are F/k, and the share of
  ground they hide, for leaves dropped at random, is 1 - exp(-layers) (the Poisson cover we
  already use for crowns). Beech: 5/0.3 = 16 layers, fully covered; ash in July: about one layer,
  63% covered, with soil and herbs in the gaps.
- **Where it lands.** Leaves fall mostly under their own crowns, with a tail that reaches tens of
  metres downwind (Ferrari and Sugita 1996: a dispersal kernel fitted per species, most within
  one or two crown radii). So edges and clearings get a drift of leaves that thins outward and
  leans downwind, and leaves pile against logs, trunks, stones and in hollows.
- **Twigs under 1 cm.** Foresters count down wood by diameter class with line-intersect sampling
  (Brown 1974; the US Forest Inventory, Woodall and Monleon 2008): the class under 0.64 cm is
  typically 0.5 to 2 t/ha on the floor of a closed wood. A twig 0.5 cm thick and 20 cm long weighs
  about 2 g, so that is 25 to 100 twigs per square metre, mostly short broken pieces half hidden
  in the litter. Twigs and small branches are 5 to 20% of the yearly litterfall (Carnol and
  Bazgir: birch and alder about 20%, beech and spruce under 5% in young stands).
- **Small leafy plants.** Tree seedlings carpet the floor after a mast year (hundreds per square
  metre under beech) and thin to a few per square metre within two years; in light, herbs with
  broad leaves (wood sorrel, violets, herb Robert, enchanter's nightshade, ground ivy) and bramble
  shoots. In meadows the grass is broken by rosettes and low broad-leaved herbs (plantain,
  dandelion, clover, yarrow, ground ivy).
- **Around fallen wood.** Moss cover on a log grows with its decay class; old logs carry ferns,
  seedlings (nurse logs) and wood sorrel on top; leaves collect on the uphill side and in the
  angle with the ground (Stokland, Siitonen and Jonsson 2012).

### How games and engines do it

- Horizon Zero Dawn (van Muijden, GDC 2017): every asset placed from its own density map built
  from rules over the terrain data; small debris is just another placed layer with footprints.
- Far Cry 5 (Carrier, GDC 2018): debris and dead leaves scattered by rules around the trees that
  were placed first (distance to trunk, slope, flow), so the floor reads as caused by the trees.
- Unreal PCG: scatter with filters on distance to other points (around trunks and logs), self
  pruning, and density from attributes; the same graph places trees then their litter.
- Kingdom Come: Deliverance II and Star Citizen's planets: a scanned litter ground material does
  most of the cover; near the camera instanced leaves, sticks and small plants give it relief.
- Ground detail far below a pixel: texture bombing (Glanville 2004) or stochastic tiling (Heitz
  and Neyret 2018) to scatter leaf shapes in the ground material without repeating tiles.

### Plan

1. **Leaves from the trees above.** Per stand species, read `TreeGenerator.get_leaf_form(preset)`
   (shape) and `get_leaf_season(preset, season)` (autumn colour, fallen share). Build litter leaf
   cards from those forms (curled, a few fresh and many older, darker and skeletonised ones) and
   the litter ground texture from the same shapes, so beech floors show beech leaves and oak
   floors oak leaves.
2. **Amount from the numbers above.** Layers on the ground = fall x leaf area / decay per
   species, mixed by the canopy map's species shares; ground cover = 1 - exp(-layers) drives both
   the litter ground blend and the 3D leaf density. Fast-decaying stands show soil and herbs by
   summer, slow ones stay brown.
3. **Drift.** The canopy layer blurred by a dispersal kernel leaning downwind (from the weather
   map's prevailing wind) gives leaves past the crowns into edges and clearings, on grass too;
   more in hollows (terrain curvature) and on the uphill side of logs and trunks.
4. **Twigs under 1 cm.** Count from the class load per stand (beech, oak 1 to 2 t/ha; pine,
   spruce less), bark colour from the trees' bark family, broken and forked short pieces, mostly
   under crowns with a tail into the edge.
5. **Small leafy plants.** Seedlings of the stand's species after the season (dense then thinning),
   broad-leaved herbs in the light patches, and low rosettes mixed through meadow grass.
6. **Around trees' fallen wood.** Read `tree_deadwood_axis` on deadwood species: moss and wood
   sorrel on top of old logs, ferns and seedlings along them, a band of trapped leaves on the
   uphill side; nothing under the log's footprint. A no-op until those species are present.

Checks: raster and path traced before/after at `--forest=in` (beech, oak, spruce) and the
forest edge; counts per square metre against the figures above; triangles and placement cost.

## References

- Ellenberg, *Vegetation Mitteleuropas mit den Alpen*, 5th ed., 1996; Ellenberg and Leuschner,
  6th ed., 2010.
- Gilliam, "The ecological significance of the herbaceous layer in temperate forest
  ecosystems", BioScience 57, 2007.
- van Muijden, "GPU-Based Run-Time Procedural Placement in Horizon: Zero Dawn", GDC 2017.
- Poorter et al., "Causes and consequences of variation in leaf mass per area (LMA): a
  meta-analysis", New Phytologist 182, 2009.
- Carnol and Bazgir, "Nutrient return to the forest floor through litter and throughfall under 7
  forest species after conversion from Norway spruce", Forest Ecology and Management 309, 2013.
- Hobbie et al., "Tree species effects on decomposition and forest floor dynamics in a common
  garden", Ecology 87, 2006.
- Ferrari and Sugita, "A spatially explicit model of leaf litter fall in hemlock-hardwood
  forests", Canadian Journal of Forest Research 26, 1996.
- Brown, "Handbook for inventorying downed woody material", USDA Forest Service GTR INT-16, 1974.
- Woodall and Monleon, "Sampling protocol, estimation, and analysis procedures for the down woody
  materials indicator of the FIA program", USDA Forest Service GTR NRS-22, 2008.
- Stokland, Siitonen and Jonsson, *Biodiversity in Dead Wood*, Cambridge, 2012.
- Carrier, "Procedural World Generation of Far Cry 5", GDC 2018.
- Glanville, "Texture Bombing", GPU Gems, 2004; Heitz and Neyret, "High-Performance By-Example
  Noise using a Histogram-Preserving Blending Operator", HPG 2018.
