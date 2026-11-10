# Plant generator (stage 3a: ground plants)

The grass carpet (grass.md) needs real plants in and between the blades: rosettes, clover,
ferns, sedges and wildflowers. Hand-built placeholder meshes do not sell a meadow. Stage 3a
generates small plants from a few parameters, with random variations per seed, as geometry the
path tracer sees, ready for `VegetationField3D`. Trees and shrubs (stage 3b) reuse the same
parts.

## How others do it

- **Prusinkiewicz and Lindenmayer, *The Algorithmic Beauty of Plants* (1990)**: L-systems. A
  turtle reads a string of modules; branching, phyllotaxis and flower heads come from rewriting
  rules. Powerful but hard to direct: a small rule change moves the whole plant.
- **Prusinkiewicz, Mündermann, Karwowski and Lane, "The use of positional information in the
  modeling of plants" (SIGGRAPH 2001)**: the method used by most production tools since. Every
  attribute of an organ (length, width, insertion angle, bend) is a *function of its position
  along the parent axis*, drawn as a curve. Organs are spaced by integrating a spacing
  function λ(s) (a node each time ∫ds/λ crosses an integer). Phyllotaxis is the divergence
  angle between successive organs: 0 (distichous), 90° (decussate) or 137.5° (the golden angle,
  spiral). Packed organs on a disc (a daisy's florets, a sunflower) follow Vogel (1979):
  θ = n·137.5°, r = c·√n. Stems, leaves and petals are generalized cylinders swept along a
  turtle path with bend rates, twist and a width function; the frame is carried by parallel
  transport, not Frenet frames, so it does not flip.
- **Lintermann and Deussen, "Interactive modeling of plants" (IEEE CG&A 1999)** (Xfrog): the
  same idea as a graph of components (stem, leaf, whorl, phyllotaxis ball, hydra) with
  positional curves. Artists build plants without rules.
- **Weber and Penn, "Creation and rendering of realistic trees" (SIGGRAPH 1995)**: parametric
  recursive branching; stage 3b.
- **Unreal 5.7 Procedural Vegetation Editor**: a node graph (trunk, branch generator, leaf
  placer, leaf shape, fronds, gravity and phototropism bends) with seed variations, output as a
  Nanite mesh with leaves as geometry, or a skeletal assembly (4 to 30+ bones) for wind. Aimed
  at trees and large shrubs; ground cover is out of scope so far.
- **SpeedTree**: the same component graph (generators for trunks, branches, leaves, fronds)
  with per-generator curves; leaves as cutout cards or modelled geometry.
- **Godot**: cosmo_tree_gen (MIT, GDScript, read 8 Nov) is a Weber-Penn-style tree tool with
  leaf cards; nothing for ground plants. Blender's Sapling is Weber-Penn too.

For the path tracer, geometry leaves beat alpha-tested cards: an alpha card needs an any-hit
shader on every ray it touches (opacity micromaps help but are off by default), and a cut-out
outline aliases under ray reconstruction. Small plants have few, small leaves, so modelled
outlines cost little: a 12-segment leaf is about 40 triangles.

## What we build (`modules/vegetation`)

`PlantGenerator` (Resource), Prusinkiewicz-style positional functions over a small set of
components, built in code (no node graph yet):

- **Leaf**: a blade swept along its midrib: outline from a width curve over its length
  (lanceolate, ovate, round, lobed by a sine on the width), fold along the midrib, bend and
  droop along its length, twist, a petiole. Shared by leaves, leaflets, petals and sedge blades.
- **Stem**: a tapered generalized cylinder along a turtle path with bend toward gravity or
  light, parallel-transport frame.
- **Arrangements**: a *rosette* (leaves at the golden angle around the base, size and angle as
  functions of position: dandelion, plantain); a *whorl* (n organs at one node: clover's three
  leaflets, petals); a *phyllotaxis axis* (organs along a stem by λ(s) and a divergence angle:
  fern pinnae, stem leaves); a *Vogel disc* (florets of a daisy head); a *tuft* (blades from
  one point, leaning out: sedges).
- **Presets** for seven meadow plants: dandelion, ribwort plantain, white clover, a fern, a sedge
  tuft, oxeye daisy and buttercup, each with seeds for variations.
- **Output**: an `ArrayMesh` with a surface per material (leaf, stem, petal), vertex colours
  carrying the wind weight (0 at the root, 1 at the tips) and an ambient occlusion guess (darker
  in the rosette's centre), UVs along each organ. Leaves are two-sided geometry with back
  lighting, so light through leaves reads the same in raster and under path tracing.
- `VegetationField3D` places the variants (a `PlantSpecies` with several meshes picks one per
  plant).

Wind: the vertex colour weight drives a sway in the material now; bone wind (stage 4) uses the
same weights.

## Checks

Bench `docs/bench/plants`: every preset at three seeds side by side, raster and path traced at
100% crop; triangle counts per plant; generation time per plant; the meadow bench with the new
plants in place of the placeholders.

## Results

Bench `docs/bench/plants --test`: every preset builds, stands on the ground at a plausible
size, gives the same plant for the same seed and another for another seed. Triangles per plant
(full detail; distance LODs are generated by meshoptimizer as the importer does) and build
time on the cloud CPU:

| plant | triangles | height | build |
|---|---|---|---|
| dandelion | 876 | 0.19 m | 1.6 ms |
| plantain | 680 | 0.28 m | 1.4 ms |
| clover | 448 | 0.08 m | 0.5 ms |
| fern | 4,936 | 0.16 m (arching, 0.64 m across) | 4.0 ms |
| sedge | 1,400 | 0.35 m | 1.8 ms |
| oxeye daisy | 1,504 | 0.45 m | 1.3 ms |
| buttercup | 1,040 | 0.48 m | 1.0 ms |

Raster and path traced frames of all presets match at 100% crop (mean difference 4.4 of 255,
mostly the path tracer's sharper shadows); every leaf, petal and floret is kept
(plant-generator.jpg, plant-presets.jpg).

In the meadow (`docs/bench/grass`, three variants per plant): daisy, buttercup and dandelion
drifts, plantain patches, clover between the tall grass, sedge within 3 m of the pond; about
33,000 plants placed in 110 to 190 ms once. Frame time on the cloud CPU rasteriser, close view:
337 ms against 238 ms for the grass carpet alone, after LODs and visibility ranges (flowers
15 m, plantain 12 m, clover 10 m, sedge 30 m); without LODs it was 723 ms. The CPU rasteriser
is bound by triangles, so this overstates a GPU's cost; GPU timings wait for the PC. The cost
is only paid where plants are placed: the generator adds nothing to scenes that do not use it.

## Leaf textures (stage 3a, detail pass)

Flat-coloured blades read as plastic at close range. Ground vegetation that holds up close in
current games (a medieval open world on CryEngine is the reference most cited for meadows,
along with the earlier Crysis titles, Red Dead Redemption 2, Ghost of Tsushima and Horizon
Forbidden West) shares the same ingredients: a leaf albedo with veins and colour drift, a
normal map that sinks the veins and puckers the blade between them, a translucency map so
light through the leaf shows the veins dark against a glowing blade, and per-plant variation
in colour and wear. Those games paint or scan these; here they are generated per plant from
the same seed, so nothing is authored and every variant differs.

How the generator builds them (`textures`, `texture_size`, default 256):

- **Veins** grow by Runions et al. 2005 open venation: auxin sources scattered over the blade
  pull their nearest vein node, nodes step towards them, sources die when a vein reaches them
  (kill distance 3% of the blade length). Growth leans towards the tip, standing in for the
  marginal growth of the paper, so secondaries leave the midrib at a forward angle. Palmate
  leaves (buttercup) start from five primaries; monocots and petals (plantain, sedge, petals)
  use parallel veins converging at the tip.
- **Vein widths** follow Murray's law (r³ of a vein is the sum of its branches' r³), so the
  midrib tapers and every branch thins towards its tip.
- **Albedo**: the blade colour with drift noise, areoles a touch darker than the veins, paler
  veins (reddish for dandelion), dry yellowed margins that vary along the edge, a few spots,
  white clover's pale chevron, and a warmer base on petals.
- **Normal map** from a height field: veins sunk, the blade raised between them.
- **Translucency** into the material's back light texture: the blade glows and the veins block
  it (Sousa, "Vegetation procedural animation and shading in Crysis", GPU Gems 3 ch. 16). The
  path tracer reads the same texture.
- **UVs** are planar across the blade (0.5 at the midrib, 0 and 1 at the widest point), so the
  veins keep their shape when the outline is toothed or lobed; veins fill the outline without
  its teeth and run on into every lobe.

Textures are baked once per preset, surface and seed variant (three per preset) and shared by
every mesh that uses them.

Results: veins, colour drift, margins and clover chevrons are visible at 100% crop in raster and
under path tracing (plant-textures.jpg; mean difference 5 to 9 of 255, the path tracer's
shadows). Cost: about 10 to 30 ms per preset on the first build on the cloud CPU (the bake),
then nothing; memory 0.8 MB per texture set (three 256² RGB maps with mipmaps), so at most
about 5 MB for every preset with petals. Frame time in the meadow (`docs/bench/grass --close`,
`--notex` for the untextured plants) on the cloud CPU rasteriser: 404 ms against 378 ms, the
cost of texture sampling in a software rasteriser; GPU timings wait for the PC. At meadow
distance the plants are small, so the gain shows up close.

Still to come in this pass: irregular outlines (wavy edges, curl, holes, asymmetry, ageing),
then a litter layer and more species.

Next: wind on the generated plants (the UV2 weights, stage 4), billboard or impostor LODs for
the far range, and shrubs and trees from the same parts (stage 3b).

## Woodland edge species

Real woods do not stop at a line: a mantle of shrubs and brambles and a fringe of tall herbs and
bracken run along the edge where the light falls off (phytosociology's Saum and Mantel; Dierschke
1974). Three new presets for that band:

- `PRESET_BRACKEN` (Pteridium aquilinum): a few fronds from the rhizome, a bare stipe with a knee,
  then a flat, triangular blade of pinnae longest at the base, each with crowded lobed pinnules
  (Page 1976). 6 to 10k triangles.
- `PRESET_BRAMBLE` (Rubus fruticosus agg.): arching canes that end where the tip comes back to the
  ground (where it roots), compound leaves of three or five toothed leaflets, flowering laterals
  and berry clusters. Up to 27k triangles: the heaviest preset, kept to near views.
- `PRESET_FOXGLOVE` (Digitalis purpurea): a rosette of soft toothed leaves and a secund spike of
  hanging purple bells, buds at the top.

Placement (grass bench `--forest`): canopy shade bands, bramble 0.05 to 0.7, bracken 0.1 to 0.8,
foxglove 0.1 to 0.7, placed right after the trees so the large plants claim their space first.

## Bending under bodies and wind (stage 3c)

Bodies walking through ferns, bracken, brambles and nettles push them aside, and the plants spring
back. No colliders: a game has hundreds of these plants around the player and a physics body for
each is the wrong cost.

How others do it:

- **CryEngine (Crysis, Kingdom Come: Deliverance)**: per-vertex "main bending" of the whole plant
  about its root plus "detail bending" of leaf edges, weighted by painted vertex colours (Sousa,
  GPU Gems 3, ch. 16). Touch bending on bushes adds a few physical branch ropes per instance.
- **Unreal Engine**: Pivot Painter stores each branch's pivot and direction in UV channels, so
  a vertex shader can turn every branch about its own base; interactive foliage is a spring per
  instance.
- **Ghost of Tsushima, Jahrmann and Wimmer 2017**: grass reads a trample texture around the
  player: a bend vector per texel that relaxes back over time.

What we build (one ShaderMaterial shared by all generated plants, so the path tracer sees the
same bend through its vertex capture):

- **One trample field for the whole ground cover.** GrassField3D already keeps the field for its
  blades (bodies in the `grass_trample` group, half-life recovery). It now publishes it as the
  global uniforms `grass_trample_map` and `grass_trample_rect`, with its fallback wind in
  `grass_wind`; every plant reads them. Nothing new runs on the CPU.
- **Organs turn about their own base (the Pivot Painter idea, done by the generator).** Each
  part that starts on the ground (a frond from the rhizome, a bramble cane from the crown, a
  nettle stem, a rosette leaf) starts an organ; everything grown on it belongs to it. UV2.y packs
  the organ's base (heading and distance from the plant's origin); UV2.x is the bend weight,
  0 at the base, 1 at the tips.
- **Push direction from the organ's base and the part.** The field is read at the organ's base
  and at the vertex (two fetches); the strength is the larger, so a frond lying across a boot goes
  down with it.
- **Upright parts lean, spreading parts lie down and swing.** Leaning a horizontal frond
  sideways about a horizontal axis rolls it over. So the push splits: upright parts lean about
  their base along it; parts that reach out from the base are laid down by its outward share and
  swung about the vertical by its sideways share; a body never folds a part back over its base.
  Parts laid past flat stay above the ground.
- **Stiffness per plant**: bramble 4, bracken 2.5, moss and litter 50 (they do not bend), the
  rest 1.5 (relative to grass). Maximum lean 1.2 / sqrt(stiffness) rad.
- **Wind**: the same rotation, from WeatherMap3D's wind (gusts, canopy) or the grass's fallback
  wind, with a few millimetres of leaf flutter along the normal (Sousa's detail bending).
- **Shading kept**: the fragment code is BaseMaterial3D's (leaf textures, back light, wetness,
  snow on faces that look up), so nothing is lost from the old StandardMaterial3D.

Checks (plants bench, CPU renderer, 1280x720):

- No push, no wind: raster frames match the old StandardMaterial3D to a mean difference below
  0.013 of 255 (bracken, bramble, foxglove).
- Frame time of all 60 plants: 111 to 125 ms before and after (the vertex work is lost in the
  noise). GPU timing waits for the desktop (`--push`, `--wind`).
- `--push --still` against `--push`: the stood-on nettle opens around the boot, bramble canes
  are laid aside, neighbours outside the field do not move.

Open: per-vertex sampling of the field costs two texture reads per vertex for every plant inside
the field's rectangle; a far LOD can skip both.

## References

- Prusinkiewicz, Lindenmayer, *The Algorithmic Beauty of Plants*, 1990.
- Prusinkiewicz, Mündermann, Karwowski, Lane, "The use of positional information in the
  modeling of plants", SIGGRAPH 2001.
- Runions, Fuhrer, Lane, Federl, Rolland-Lagan, Prusinkiewicz, "Modeling and visualization of
  leaf venation patterns", SIGGRAPH 2005.
- Murray, "The physiological principle of minimum work", PNAS 1926 (vein radii).
- Vogel, "A better way to construct the sunflower head", Mathematical Biosciences 1979.
- Sousa, "Vegetation procedural animation and shading in Crysis", GPU Gems 3, ch. 16, 2007.
- Lindquist, "Pivot Painter 2", Unreal Engine documentation, 2017.
- Jahrmann, Wimmer, "Responsive real-time grass rendering for general 3D scenes", I3D 2017.
- Lintermann, Deussen, "Interactive modeling of plants", IEEE CG&A 1999.
- Weber, Penn, "Creation and rendering of realistic trees", SIGGRAPH 1995.
