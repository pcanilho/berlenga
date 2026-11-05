# valence-engine-rtx: assessment of another NVIDIA path tracer fork

Status: assessment only. No integration work has started.
Date: 2026-11-05. Fork `main` at 7283cbf. NVIDIA `nvidia-pt-dlss-dev` at 1dd330f.
Assessed tip: `valence-main` at 2082787 (2026-10-18).

## Question

Does https://github.com/Rman887/valence-engine-rtx contain techniques or code
worth taking into this fork's path tracer?

## What the project is

- A fork of NVIDIA-RTX/godot `nvidia-pt-dlss`, Godot 4.8-dev. Same base commit
  as this fork, `135dff3887 NVIDIA: Pathtracer + DLSS`. Branches:
  `nvidia-pt-dlss` (the untouched base) and `valence-main`.
- It is the engine submodule of a larger, unpublished project. Its CLAUDE.md
  refers to `../docs/engine-fork.md`, `../AGENTS.md`, `../game/` and test rigs
  (`pt_smoke`, `materials_matrix`, `battle_plot`) that are not in this
  repository. The design notes the commits cite (D50 to D126, `docs/r6-...`)
  are not published either. Only the commit messages explain the work.
- Size: 272 commits, all prefixed `VAL:`, one author, 2026-09-11 to
  2026-10-18 (nothing since). 268 files, +84,500 / -2,300 lines including
  vendored libraries; `render_raytracing.cpp` alone grows by about 12,000
  lines. Written with an AI agent (Claude), with measured validation in most
  commit messages (PSNR against a reference build, GPU timings, white furnace
  tests).
- Licence: MIT (Godot's) for its own code. Additions: OpenSubdiv 3.7 (TOST-1.0,
  Apache-style), meshoptimizer 1.3 with `clusterlod.h` (MIT), the RTXCR hair
  BCSDF port (MIT header). RTX Texture Filtering (`thirdparty/rtxtf`) is under
  the NVIDIA RTX SDKs licence, which allows redistribution in object code
  form. Shipping its shader source in a public fork needs a closer read
  before we vendor it.
- It has not been rebased onto NVIDIA's 2026-11-02 rewrite. It carries the
  pre-rewrite tracer, as our `main` does.

## Can we take commits directly?

No. Trial `git cherry-pick -n` of four self-contained-looking features
(emissive lights, opacity micromaps, clearcoat, thin transmission) conflicts
in 12 to 21 files each, against both our `main` and NVIDIA `-dev`. Every
feature sits on earlier Valence refactors (its 128-byte material record, the
`shade_direct_light` split, the path-state struct). Anything we want is a
reimplementation that uses its commits as a worked reference, on top of the
NVIDIA rewrite branch once that port lands.

## Overlap with what we already have

| Area | Valence | This fork / NVIDIA rewrite |
|---|---|---|
| Emission without an emission texture | yes | ours #1, NVIDIA rewrite |
| Cast Shadow Off honoured in the TLAS | yes | ours |
| Volumetric fog with ray-query shadows | yes | ours (froxel light cache), NVIDIA rewrite |
| Decals in hit shaders, UV2, triplanar | yes | NVIDIA rewrite, ours (UV2, triplanar) |
| Ray-cone texture LOD, anisotropic footprint | yes | ours |
| Hashed alpha in any-hit | yes | ours, NVIDIA rewrite (kept theirs) |
| Many positional lights | light tree (BVH) | ours: world grid of light lists + RIS (4096 lights) |
| Omni/spot falloff without the 1 m cap | yes (D123) | NVIDIA rewrite (uncapped); our `main` still caps |
| Per-frame BLAS build budget | yes | NVIDIA rewrite |
| Transparent layers, particles in the trace | partial | ours + NVIDIA rewrite (path-traced transparency) |

## What is new to us, ranked for a night city under rain

### 1. Emissive surfaces as lights (next-event estimation)

Commits: "sample constant emissive surfaces as lights (D50)", "resample the
emissive list by proximity per hit (D51)", "an alias table for the emissive
candidates".

Today an emissive surface (sign tubes, lit windows, lamp lenses) only lights
the scene when a bounce ray happens to hit it, so its light on wet ground is
noise that Ray Reconstruction has to smooth, and small bright emitters are
fireflies. Valence lists emissive instances per frame (up to 1024, ranked by
camera distance), samples them like lights with a shadow ray, and weighs
emission found by bounce rays with the power heuristic so nothing counts
twice. This is standard many-light NEE with multiple importance sampling
(Veach 1997; RTXDI treats emissive triangles the same way).

Limit: Valence only takes constant, untextured emission. Sign lettering and
window masks are textured, so we would need per-triangle (or per-instance
average) emitted power, as RTXDI and Cycles do. Neither our `main` nor the
NVIDIA rewrite has emissive NEE.

### 2. Opacity micromaps for alpha-tested surfaces

Commits: "opacity micromaps for alpha-tested standard materials", "custom
shaders opt in to opacity micromaps by render mode", "2-state opacity
micromaps on indirect rays", "the micromap bake's alpha bound over 3 x 3
cells".

`VK_EXT_opacity_micromap` lets the RT cores resolve most alpha-tested texels
without invoking the any-hit shader. Ada (RTX 40) has hardware support. Tree
canopies, chain-link fences, grates and leaf litter are dominated by any-hit
cost. Valence bakes a 4-state micromap from the alpha texture at import and
falls back to 2-state on indirect rays (cheaper, the error is invisible
after a bounce). Pure performance; no visual change when correct.

### 3. Clearcoat, anisotropy, sheen and multiple-scattering compensation

Commits: D118 MM0 to MM4.

Neither our `main` nor the NVIDIA rewrite traces BaseMaterial3D clearcoat: the
value reaches custom shaders and is dropped. Valence adds a GGX coat over the
base with the raster's F0 remap, anisotropic GGX from Godot's anisotropy, a
sheen lobe from rim, and Kulla-Conty style energy compensation from an albedo
table (rough metals stop losing energy). Car paint and wet surfaces (a thin
water film is a clearcoat) are where raster and PT currently disagree most;
this closes that gap. Lowest-risk item in this list.

### 4. Thin diffuse transmission

Commit: "thin diffuse transmission from BaseMaterial3D backlight".

Traces Godot's `backlight` as a diffuse lobe through the surface. Leaves lit
from below by a street lamp, umbrellas, curtains, paper lamp shades.

### 5. One-sided rectangle area lights

Commit: "area lights as one-sided rectangles (D124)", sampled by solid angle
(Urena et al. 2013, as Cycles). NVIDIA traces only omni, spot and
directional. Useful for shop windows and light boxes if we use AreaLight3D.

### 6. Exposure chain and physical light units

Commits on 2026-10-05/06: histogram auto exposure, pre-exposed path-traced
frame, sky and fog carrying the camera exposure once, light power under
physical light units matching the raster.

Our PT frame needs a hand-set exposure multiplier to match raster. Before
porting anything, check whether part of that gap is the 1 m falloff cap (fixed
by the NVIDIA rewrite) or a double-applied exposure term that these commits
describe.

### Lower priority for us

- Light tree (D55 to D63, Estevez and Kulla 2018 / Conty and Kulla 2018 style
  BVH over lights with an adaptive cut). An alternative to our light grid.
  Worth a comparison only if the grid shows noise with many dim lamps.
- Owen-scrambled Sobol sampling, path contribution clamp, Russian roulette as
  settings. Small wins; Ray Reconstruction already dominates noise.
- Sky light importance table. Our night skies are dim; little to gain.
- Random-walk subsurface scattering (D120). Faces and hands at close range.
- Cluster acceleration structures, cluster LOD, partitioned TLAS, geometry
  streaming, async AS builds (D79 to D98). This is the RTX Mega Geometry path.
  Large and invasive; our scenes are small enough not to need it.
- Strand hair (linear swept spheres, RTXCR BCSDF). LSS hardware is Blackwell
  (RTX 50); our characters are single meshes without strands.
- OpenSubdiv subdivision with displacement. Not needed for our assets.

## Recommendation

Do not merge or track this fork. Use it as a reference implementation, with
its commit messages as design notes, for a few features we lack, each done
on the NVIDIA rewrite base after the port lands:

1. Emissive NEE, extended to textured emitters.
2. Clearcoat and multiple-scattering compensation in the PT BRDF.
3. Opacity micromaps for alpha-tested foliage and grates.
4. Thin diffuse transmission.

Each is its own branch with raster and PT comparison shots before it merges.
