---
title: "Trees"
weight: 65
summary: "Trees and woody shrubs grown by light competition, 31 species in ten stand types; bark, moss, climbers, seasons, old trees and deadwood; weather wind out to 1 km, colliders and touch."
---

## Growth

- **Tree generator** (`TreeGenerator`: `preset`, `years`, `seed`): trees and woody shrubs grow bud by bud, competing for light, so the same settings always give the same tree. 31 species from Europe and eastern North America, each with its climate, slope and soil habitat (`apply_habitat`).

  ![Seven tree species grown in the open, raster](media/trees-species.jpg)

  *Tree generator: seven species grown in the open.*

- **Stand and open forms** (`forest_shade`, `forest_spacing`): trees grown in a closed stand shed their lower limbs to a long clear bole under a narrow crown; open-grown trees keep leaves down to their lower limbs.
- **Conifer crowns** (`crown_cone`): spruce, fir, larch, pine and hemlock keep their cone, with branches to the top.
- **Stands** (`make_stand`, `make_woodland`): ten stand types, from beech, oak-lime, riparian alder and subalpine larch to North American northern hardwood and pine-hemlock. Each places canopy trees, trees in the gaps, saplings, shrubs under their shade and deadwood, and overlapping stands blend at their edges.
- **Shrubs and hedges** (`stems`, `envelope`): many-stemmed shrubs, drawn up and thin under a canopy; hedges and topiary clipped to a box.
- **Clearings** (`mask_channel`): painted clearings and paths keep trees and shrubs out.

## Wood and leaves

- **Branch collars** (`junction_weld`): branches flare into their parent in a short collar with a bark ridge in the crotch. Limbs that would grow into thicker wood bend away or die back to it.

  ![A sycamore's limbs meeting its trunk, without and with the weld, raster](media/trees-junction.jpg)

  *Branch collars: the hard seam where each limb enters the trunk becomes a soft flare with the bark running through.*

- **Surface roots** (`root_flare`): buttress roots run out of the trunk flare, tapering as they sink into the soil, longer on shallow-rooted species.
- **Bark** (`bark_relief`): a procedural bark per species from six families (smooth, furrowed, plated, scaly, flaking, papery), with displaced relief on near trunks for raster and path tracing.
- **Bark moss** (`moss_cover`): moss gathers at the foot of the stem and on the upper faces of limbs, thicker on rough bark and decayed wood, as real cushions near the camera.
- **Climbers** (`climber`, `climber_cover`): ivy up the stem, clematis hanging from the limbs or honeysuckle twining round them, swaying with the tree.
- **Leaf shapes** (`TreeGenerator.get_leaf_form()`): simple, lobed, palmate and compound leaves per species, with the true outline as geometry.
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

- **Deadwood** (`decay_class`, `fallen`): standing dead trees, snapped tops, stumps and fallen logs in the five forestry decay classes.
- **Fallen branches** (`fallen_branch`): shed limbs and sticks on the forest floor, from the stand's own trees.

## Wind and touch

- **Tree wind** (`TreeWind3D`, `TreeGenerator.rigged`): trees sway in the weather map's wind and gusts, each limb at its own frequency.
- **Leaf flutter** (`leaf_flutter`): leaves flutter and twist on their stalks in the same wind as grass and plants.
- **Far wind** (`wind_range`, `lod_enabled`, `lod_pixel_error`, `budget_msec`): trees move in the wind out to 1 km, with far ones posed less often and with fewer limbs.
- **Colliders and touch** (`collision_enabled`, `touch_enabled`, `break_speed`): trunks and limbs collide near the camera; characters and vehicles push branches aside, and fast bodies snap thin ones off.
