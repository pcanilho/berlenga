---
title: "Trees"
weight: 70
group: "World"
summary: "Trees and woody shrubs grown by light competition, 31 species in ten stand types; bark, moss, lichen, climbers, seasons, pollards, old trees, deadwood and fungi; weather wind out to 1 km, colliders and touch."
---

## Growth

- **Tree generator** (`TreeGenerator`: `preset`, `years`, `seed`): trees and woody shrubs grow bud by bud, competing for light, so the same settings always give the same tree. 31 species from Europe and eastern North America, each with its climate, slope and soil habitat (`apply_habitat`).

  ![Seven tree species grown in the open, raster](media/trees-species.jpg)

  *Tree generator: seven species grown in the open.*

- **Stand and open forms** (`forest_shade`, `forest_spacing`): trees grown in a closed stand shed their lower limbs to a long clear bole under a narrow crown; open-grown trees keep leaves down to their lower limbs.
- **Conifer crowns** (`crown_cone`): spruce, fir, larch, pine and hemlock keep their cone, with branches to the top.
- **Stands** (`make_stand`, `make_woodland`): thirteen stand types, from beech, oak-lime, riparian alder and subalpine larch to North American northern hardwood and pine-hemlock, plus managed coppice with standards, wood pasture and pollard rows (`STAND_COPPICE_STANDARDS`, `STAND_WOOD_PASTURE`, `STAND_POLLARD_ROW`). Each places canopy trees, trees in the gaps, saplings, shrubs under their shade and deadwood, and overlapping stands blend at their edges.
- **Pollards and coppice** (`cut_height`, `cut_cycle`, `cut_since`): trees cut back on a cycle regrow straight poles from a swollen, knobbly head or a ground stool; lapsed pollards carry big limbs. Poles swell into collars and carry branch scars; coppice stools end in old rotted cuts under a callus lip, and their rods swell at the nodes.
- **Shrubs and hedges** (`stems`, `envelope`): many-stemmed shrubs, drawn up and thin under a canopy; hedges and topiary clipped to a box.
- **Wind, bank and rock trees** (`wind_exposure`, `krummholz`, `ground_slope`, `bank_erosion`, `root_stone`): at the tree line, windward limbs die back and the rest turn downwind, and krummholz grows as a low many-stemmed mat cut at the snow surface; trees on stream banks and gorge sides lean their roots downhill, with eroded roots arching off the face; roots can drape over a boulder beside the stem.
- **Clearings** (`mask_channel`): painted clearings and paths keep trees and shrubs out.

## Wood and leaves

- **Branch collars** (`junction_weld`): branches flare into their parent in a short collar with a bark ridge in the crotch. Limbs that would grow into thicker wood bend away or die back to it.

  ![A sycamore's limbs meeting its trunk, without and with the weld, raster](media/trees-junction.jpg)

  *Branch collars: the hard seam where each limb enters the trunk becomes a soft flare with the bark running through.*

- **Surface roots** (`root_flare`): buttress roots run out of the trunk flare, tapering as they sink into the soil, longer on shallow-rooted species.

  ![The foot of an oak with surface roots running into leaf litter, raster](media/trees-roots.jpg)

  *Surface roots: buttress roots spread from the trunk into the litter, with moss in the crotches between them.*

- **Bark** (`bark_relief`): a procedural bark per species from six families (smooth, furrowed, plated, scaly, flaking, papery), with displaced relief on near trunks for raster and path tracing. Smooth beech bark carries stemflow tracks, low algae and crust lichen.
- **Bark stories** (`bark_wounds`, on by default): frost ribs, lightning scars, deer stripping and resin runs on trunks; drying checks and knots on lying logs.
- **Bark moss** (`moss_cover`): moss gathers at the foot of the stem and on the upper faces of limbs, thicker on rough bark and decayed wood, as real cushions near the camera.
- **Crown parts** (`fruit_cover`, `mistletoe`, `witches_brooms`): cones, catkins with bract scales, ash keys in bunches, and rowan and elder berries in dense corymbs on branching stalks in season; winter buds on shoot tips; mistletoe on its hosts and witches' brooms as twig nests on birches.

  ![A rowan berry cluster among its leaves at about 0.6 m, raster](media/trees-rowan.jpg)

  *Crown parts: a rowan corymb hangs on branching red stalks among toothed, veined leaflets.*

- **Bark lichen** (`lichen_cover`): lichen on bark, placed by height, crown light, stem age and moss; beard lichens and oakmoss hang from and stand off crown twigs on near trees. 0 turns it off.

  ![An oak's bole without and with lichen, raster](media/trees-lichen.jpg)

  *Bark lichen: pale colonies gather on the upper bole and thin out toward the mossy foot.*

- **Climbers** (`climber`, `climber_cover`): ivy up the stem, clematis hanging from the limbs or honeysuckle twining round them, swaying with the tree. Old ivy has woody stems that thicken with the host's age and lie on the bark ridges, with unlobed adult leaves high on old stems.
- **Leaf shapes** (`TreeGenerator.get_leaf_form()`): simple, lobed, palmate and compound leaves per species, with the true outline as geometry; compound leaflets take a toothed, veined texture, and leaf undersides are paler.
- **Needles** (`needle_sprays`, `near_needle_range`): real needles near the camera, crossed needle sprays beyond.
- **Near leaf detail** (`near_leaf_range`): lobed and compound leaves are built only for trees near the camera; further out each leaf is a simple leaf of the same area.
- **Seasons** (`season`): leaves come out, turn and fall by species from the shared season state; larch turns gold and goes bare, evergreens stay.

  ![Birch, beech, red maple and larch in July and November, raster](media/trees-seasons.jpg)

  *Seasons: by November the beech has turned, the maple and birch are bare and the larch is gold.*

- **Tree LODs**: far levels drop thin wood and thin out leaves, enlarging the rest so leaf area holds; leaves stay geometry at every level.

## Old trees and deadwood

- **Old trees** (`veteran`): basal hollows, rot holes, woodpecker holes, burls, epicormic shoots and snapped crowns on the clear bole, listed by `get_veteran_features()`.

  ![An oak's lower trunk at veteran 0 and 1, raster](media/trees-veteran.jpg)

  *Old trees: a basal hollow with a rounded lip, a rot hole, a brush of epicormic shoots and the edge of a burl.*

- **Deadwood** (`decay_class`, `fallen`, `bark_peel`, `uprooted`): standing dead trees, snapped tops, stumps and fallen logs in the five forestry decay classes. Bark comes away in plates by decay class, from the top of snags and the upper side of logs, baring grey or brown checked wood. Logs settle along the ground at every decay class, and fallen sticks lie as snapped, kinked pieces. Windthrown trees lie with their root plate on edge, its torn underside holding soil clods, stones and root stubs, the butt held off the ground.

  ![An oak snag without and with bark peel, raster](media/trees-bark-peel.jpg)

  *Bark peel: on a class 2 snag the bark has come away in plates from the upper stem, baring grey wood.*

- **Broken limbs** (`limb_breakage`): snapped ends get a splintered fracture face and torn bark; storms break limbs on living trees, leaving some hanging.

  ![A winter oak limb, whole and snapped, raster](media/trees-broken-limb.jpg)

  *Broken limbs: the limb ends in a splintered break of pale wood with torn bark.*

- **Fungi** (`fungi_cover`): bracket fungi and mushroom tufts with stalks, caps and gills grow as geometry on old and dead wood, by host and decay class.
- **Fallen branches** (`fallen_branch`): shed limbs and sticks on the forest floor, from the stand's own trees.

## Wind and touch

- **Tree wind** (`TreeWind3D`, `TreeGenerator.rigged`): trees sway in the weather map's wind and gusts, each limb at its own frequency.
- **Leaf flutter** (`leaf_flutter`): leaves flutter and twist on their stalks in the same wind as grass and plants.
- **Far wind** (`wind_range`, `lod_enabled`, `lod_pixel_error`, `budget_msec`): trees move in the wind out to 1 km, with far ones posed less often and with fewer limbs.
- **Colliders and touch** (`collision_enabled`, `touch_enabled`, `break_speed`): trunks and limbs collide near the camera; characters and vehicles push branches aside, and fast bodies snap thin ones off.
