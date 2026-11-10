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

Next: wind on the generated plants (the UV2 weights, stage 4), billboard or impostor LODs for
the far range, and shrubs and trees from the same parts (stage 3b).
