# valence-engine-rtx: implementation takeaways

Status: reference notes for later work. Nothing here is implemented on `main`.
Date: 2026-11-05. Companion to `valence-engine-rtx.md` (what the project is,
licence, ranking). Source: https://github.com/Rman887/valence-engine-rtx,
branch `valence-main` at 2082787. Commit hashes below are from that branch;
`D` numbers are the design ids its commit messages use.

When this work starts it builds on the NVIDIA rewrite base (`nv-port`), not
on today's `main`. Valence's commits do not cherry-pick (they sit on its own
material record and path-state refactors), so each item below is a
reimplementation that reads the named commits for the algorithm, the data
layout and the traps they measured.

Columns used below: what it does, how, the commits to read, what they
measured, and the traps.

## Attribution

Anything this fork takes from valence-engine-rtx (Rman887, MIT) is credited:

- Each commit that reimplements or adapts one of its features names it in
  the message body: `Based on Rman887/valence-engine-rtx <commit> (<D id>)`.
- The code carries a one-line comment at the feature's entry point naming
  the same source, next to the papers it implements.
- Code copied rather than rewritten keeps its MIT copyright and permission
  notice, and COPYRIGHT.txt gains an entry for it. Third-party code it
  vendors keeps its own licence entry (RTXCR: MIT; OpenSubdiv: TOST-1.0;
  RTXTF: NVIDIA RTX SDKs licence, to be checked before vendoring).
- The CHANGELOG line for such a PR and the README's references section
  name the project.

## 1. Many-light sampling: light tree with resampling

What our base has: `nv-port` picks a positional light by resampled importance
sampling (RIS) over a world grid of per-cell light lists (`build_light_grid`,
binding 42, `lights_select_positional`). One grid over the whole world is too
coarse for a large map.

What Valence does (D55 to D63, c60e95a09e, 7b3b2a7ec3, 8ce7f0e1c3,
13a597e7e5, 592dc4352b, 293c55bc41, 986af36c66, 1dcbd98025):

- A light BVH rebuilt every frame on the CPU: one leaf per positional light
  and per emissive instance; per node a bounding sphere, an orientation cone
  (axis, theta_o, theta_e), summed power and largest range. Importance per
  Conty Estevez and Kulla, "Importance Sampling of Many Lights with Adaptive
  Tree Splitting", HPG 2018.
- The tree is used as the proposal of RIS, not as the final sampler: N
  candidates drawn through it, each weighted by its unshadowed contribution
  over its proposal probability, one kept by weighted reservoir sampling.
  Measured reason: a plain descent was noisier than the uniform pick, because
  a receiver inside a cluster's bounds reads every containing node alike and
  a node in range by its bound alone fails a level down.
- Cost fix (D56): walking the whole tree per candidate cost +9 % to +86 % of
  the frame. Replaced by a breadth-first cut of the shallowest 32 nodes,
  scored once per hit; each candidate picks a cut node by its share and a
  leaf under it (uniformly, or by power through a Vose alias table, D60/D61).
  Per-hit arrays in local memory were tried and rejected (2 to 3x slower).
- D57: the importance written without acos/asin, all angles as cosine and
  sine pairs through sum and difference identities.
- D59: a second, coarser cut for vertices past the first diffuse bounce.
- D63: node falloff uses the lights' own decay exponent, not inverse square.
  Godot's default omni/spot attenuation is 1, so an inverse-square proposal
  under-weights default lights by up to the distance.
- Cone union with exactly opposite axes must rotate about any perpendicular
  (1dcbd98025); a zero cross product silently dropped one cone.
- MIS: a bounce ray that hits an emitter re-walks its leaf to the root at the
  previous vertex to get the tree density, so emission is weighed by the
  power heuristic against the light sampler.

Measured: torches 40.2 dB vs 37.9 (raw 1 spp against 64 spp references), 300
lanterns with a hundredfold power spread 27.5 dB vs 21.5 with the power pick.

Trap list for our implementation:
- Keep the light cull mask union and the largest range in each node so a
  node with no reachable light reads exactly zero (unbiased).
- Keep the receiver term floored (backlit and transmissive surfaces are lit
  from behind). In the transparent light cache (points in the air) skip the
  receiver term entirely.
- Shadow groups stay per light at evaluation; the tree does not need them.

Our draft: branch `claude/emissive-light-tree` (3a62efa1c2, off `nv-port`
d5b0948f42, not built or tested) replaces the grid with a binned-SAOH light
BVH and RIS over full walks. Before it ships it should take D56's cut and
D63's decay-aware falloff, both measured above.

## 2. Emissive surfaces as lights

What our base has: emission is found only when a bounce ray hits it.

What Valence does (D50 fe640d25b9, D51 cff8345002, alias ab052925b2):

- Each frame, list the TLAS instances whose material emits a constant,
  untextured colour through Add on an opaque surface (80-byte entries:
  transform, radiance, triangle count, two-sided and facing flags corrected
  for mirrored instances, probability, CDF by luminance times area), capped
  at 1024 by camera distance.
- Each hit takes one light sample from either the analytic lights or the
  emissive list (share by summed scores, clamped 0.1 to 0.9), then a
  triangle and a point on it.
- MIS by the power heuristic against BSDF sampling, using the BSDF density
  carried in the payload (a delta sentinel for mirrors and refraction).
  `brdf_pdf` must mirror the scatter's lobe split and VNDF sampler exactly.
- D51: resample 8 emissive candidates by proximity per hit; about a quarter
  of the samples for the same noise. Later the emissive entries became light
  tree leaves.

Measured cost: +9.5 % p50 with 17 emitters, +5 % with 74.

Trap: textured emission (sign lettering, window masks) is left to chance hits
in Valence. For signs we need per-triangle emitted power from the texture,
the established route in RTXDI and Falcor (integrate the emissive texture
over each triangle's UV footprint at load, store per-triangle flux, sample
triangles by flux, evaluate the real texel at the sampled point).

## 3. Material model parity with the raster

What our base has: GGX with Godot's DFG energy compensation, a raster-style
backlight term, no clearcoat, no sheen, anisotropy only in texture filtering.

What Valence does (D118 MM0 to MM4, 64c1646a7f, 517a25c221, 7d8982900e,
11293fd532, clearcoat commit "clearcoat from Godot's clearcoat"):

- Multiple-scattering compensation (Turquin 2019, 1 + F0 (1 - E) / E) with E
  from a 32 x 32 GGX albedo table over sqrt(alpha) and sqrt(mu), applied on
  both the BSDF sample and the light sample so both MIS strategies estimate
  the same integrand. NVIDIA's fitted approximation was up to 13 to 16 % off
  in a white furnace.
- Clearcoat: a GGX coat (F0 0.04, alpha mix(0.001, 0.1, r) as the raster),
  base F0 remapped by the raster's clear-coat-to-surface function, base
  attenuated by (1 - c F_c) both ways, the coat lobe picked first by its
  reflectance.
- Anisotropic GGX with Godot's stretch and flow map, one (T, B, N) frame
  shared by light sample, pdf and BSDF sample; a derived tangent where the
  mesh has none.
- Sheen from Godot's rim: Estevez and Kulla's Charlie sheen in Falcor's form.
- Thin diffuse transmission from backlight (6aac992109): a sampled Lambertian
  lobe through the surface with RTXPT's energy split, NEE accepting below-N
  samples for flagged hits, the next origin offset along -N.

Trap: each lobe has to be added to evaluation, sampling and pdf together, or
MIS weights go wrong without any visible error at first.

## 4. Opacity micromaps

What our base has: alpha-tested geometry runs the any-hit shader.

What Valence does (D96, 1dc03dcbd6, acb9b340d9, 9c6e86a9c9, 4e8ed01b40):

- `VK_EXT_opacity_micromap` end to end in the driver and RenderingDevice
  (micromap create, build, sizes; the micromap chained into triangle
  geometry; the pipeline flag).
- Bake per alpha-tested surface: min/max alpha pyramid, setup pass through
  the hit shader's own UV fetch, async readback, dedup on the CPU, bake pass,
  build. Each micro-triangle's alpha bound covers every texel a bilinear
  sample inside it can read (3 x 3 direct reads, else the pyramid level that
  covers it in 3 x 3). 4-state, so unknown triangles still run the any-hit:
  every ray decision stays bit-exact.
- Custom shaders opt in with a render mode and a `hint_alpha` texture.
- Optional 2-state on rays past the first diffuse bounce.

Measured: 240,000 foliage cards, -11 % GPU with RR, -17 % raw.

Trap: without synchronization2 the micromap build needs a full memory
dependency both sides.

## 5. Open-world geometry: cluster acceleration structures and LOD

What our base has: one BLAS per mesh, NVIDIA's per-frame BLAS build budgets.

What Valence does (D79 to D92, 81f3f1d064, 3ad85df97a, 2f84ae5276,
02c38b9ed6, 3a8186b1d1, c10b5495a1, D93a to D93d):

- `VK_NV_cluster_acceleration_structure` behind a project setting; the
  extension requested only when on.
- Clusterize every eligible surface at creation with meshoptimizer (128
  triangles and vertices per cluster), saved with the mesh.
- Cluster LOD hierarchy with meshoptimizer's `clusterlod.h` (groups of 32,
  error merged at 1.5, seams protected), saved with the mesh.
- Per-instance LOD selection in a compute pass by projected error (1 output
  pixel default); BLAS per instance over the selected clusters; instances of
  one surface at similar depth share BLAS builds (after NVIDIA's
  vk_lod_clusters sample).
- Residency and streaming: only the coarsest groups resident by default,
  others loaded and unloaded as units; selection clamped by residency.
- Async builds on a compute queue ordered by timeline semaphores (D97, D98).

Read alongside NVIDIA's own samples `vk_lod_clusters` and
`vk_partitioned_tlas` before choosing a design.

## 6. Open-world instances: partitioned TLAS

What Valence does (D94a to D94c, ffb98e75e3): `VK_NV_partitioned_acceleration_structure`
behind a restart setting; acceleration-structure descriptors switched to the
partitioned type; slot-resident instances so only changed partitions are
rebuilt; a world-space partition translation. Falls back to a plain TLAS
with one warning when the extension is missing.

## 7. Precision far from the origin

- Relative origin (63b4e200de): TLAS transforms, previous transforms, light
  positions, the camera and the VPs uploaded relative to a camera anchor
  snapped to a grid (Valence later set 16 m), subtracted in double. Needed in
  double-precision builds too, where the RT stages read the scene UBO with a
  mismatched layout.
- Ray-origin offset floor (ea59420c9b): Wachter-Binder offsets scaled by the
  largest coordinate and the ray length, so a surface near y = 0 far from
  the origin does not self-intersect.

## 8. Smaller items worth taking

- Owen-scrambled Sobol sampler instead of per-draw PCG hashing (D52,
  6d64575301), as a switch; stratification across a path's dimensions.
- Per-path luminance clamp, direct and indirect separately, dropping
  non-finite contributions (D53).
- Sky importance sampling as a pyramid over the sky octmap's texels with a
  hierarchical warp (D54, D69); matters for daytime worlds.
- Histogram auto exposure (D115, 46b3db3a5f): 64-bin log2 histogram on the
  displayed image, mean of the 50th to 95th percentile, slower adaptation to
  dark.
- Specular hit-distance guide for Ray Reconstruction traced to the closest
  hit, not the first confirmed hit (wet-street reflections read better).
- Area lights as one-sided rectangles sampled by solid angle (D124,
  Urena et al. 2013).
- Stochastic texture filtering (D117) with RTXTF; check the NVIDIA RTX SDK
  licence before vendoring its shader source.
- Random-walk subsurface (D120, Cycles' method) for faces at close range.
- Water medium in the payload (ABSORPTION built-in, Beer-Lambert along the
  real path) for puddles and submerged geometry.

## 9. Open world additions (wanted for the open-world game)

Strand hair (D126, e016add0e5, ed8a806f0c, 7d87b757a1, plus the RenderingDevice
and Vulkan commits of 2026-10-14):
- `PRIMITIVE_LINES` surfaces with a per-vertex radius in CUSTOM0 build a
  linear swept sphere (LSS) BLAS through `VK_NV_ray_tracing_linear_swept_spheres`;
  skinned strands refit; MultiMesh copies supported.
- BaseMaterial3D gains a hair feature (absorption by melanin or colour,
  redness, IOR 1.55, cuticle tilt 2 degrees).
- Shading: the RTXCR material library (MIT) hair BCSDFs ported to GLSL, the
  far-field model by default and Chiang 2016 as an option, with NEE, MIS and
  the BCSDF's own sample. Fixed a density bug in RTXCR's Chiang pdf (white
  furnace +7.6 % before).
- Trap: Valence has no fallback where the LSS extension is missing. Checked
  2026-11-06 on an RTX 4090 (driver 621.01, Vulkan 1.4.357):
  `VK_NV_ray_tracing_linear_swept_spheres` is absent, so RTX 40 needs the
  fallback: strand cards as triangle ribbons, or a capsule intersection
  shader, shaded with the same BCSDF. LSS stays an RTX 50 fast path.

Extension availability on the same RTX 4090: `VK_NV_cluster_acceleration_structure`
rev 4, `VK_NV_partitioned_acceleration_structure` rev 1, `VK_EXT_opacity_micromap`
rev 2 (and `VK_KHR_opacity_micromap` rev 1) present; `VK_NV_displacement_micromap`
absent. Cluster LOD, partitioned TLAS and opacity micromaps can all run there.

Subdivision with displacement (D122, OpenSubdiv 3.7, 13ee3fa1, 68a3c4a9):
Catmull-Clark refinement on the GPU per frame for deformed meshes, a height
map baked per refined vertex as Cycles displaces. Geometric detail on
terrain, rocks and hero assets at close range.

Sky light sampling and exposure (D54, D62 to D72, D115): needed for daylight
and day/night changes in a large world. See section 8.

Water medium (2dc857368e): ABSORPTION and medium built-ins carried in the
payload, Beer-Lambert along the actual path, so lakes, sea and puddles fade
light correctly and the camera can go under water without fog workarounds.

Random-walk subsurface (D120): skin, foliage and wax at close range.

## 10. General engine fixes (small, independent of the path tracer's design)

- Index variants on mesh surfaces, drawn per instance (196834c, ff0ce16,
  5aa0bad): one mesh, triangles hidden per instance, in raster and trace.
  Damage, destruction, cut-aways.
- Pipeline precompile makes each vertex and framebuffer format once
  (1a8c611); the Vulkan pipeline cache capped and safe past 2 GiB.
- Skeleton loops over the active blend shapes only; an unchanged blend
  shape weight does not re-deform its instance; a mesh swapped for one of
  the same layout keeps its surface caches (4fcb408, 9bc0a2a).
- MultiMesh: set_buffer on a mirrored MultiMesh uploads once.
- SurfaceTool hashes Vertex field by field, not over padded bytes.
- Jolt: cast_motion streams broadphase candidates through its filters; a
  zero-length motion has a defined step count.
- RenderingDeviceGraph barrier fixes for acceleration-structure reads
  (MEMORY_READ, TLAS lists reading their BLASes, in-place writes waiting
  for device-address reads).

## Order when work resumes

1. Light tree (finish the draft with D56 cut and D63 falloff), then emissive
   triangles as tree leaves with textured flux.
2. Multiple-scattering table and clearcoat, then thin transmission.
3. Opacity micromaps.
4. Open world: relative origin and offset floor first (small), then a
   measured design note on cluster LOD, partitioned TLAS and streaming
   against NVIDIA's samples.
5. Strand hair with a fallback for hardware without LSS, then sky sampling
   and exposure, water medium, subdivision, subsurface.
