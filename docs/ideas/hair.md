# Hair under path tracing

Research note, 9 Nov 2026. Nothing built yet. Asked: is strand hair under path tracing a good
idea for this fork, and at what scope.

## Short answer

Yes, in two stages, and the first stage is not strands.

1. **A hair BCSDF for the hair we already have.** Characters come in as meshes whose hair is
   a sculpted shell or a set of cards. Under path tracing that hair is shaded with the same
   GGX lobe as skin and cloth, so it reads as plastic: no coloured second highlight, no light
   through the hair, no glints along the strands. A proper hair scattering function (Chiang
   et al. 2016) on mesh and card hair fixes the look of every character at once, costs one
   material feature, and needs no new geometry or assets.
2. **Strand geometry and strand simulation** later, when a project has close-range
   characters and grooms to feed it. Real strands need a groom per character (a
   curve asset, not something Tripo or glTF gives us), a curve primitive in the BLAS (which
   the RTX 40 cards do not have in hardware), and a strand solver. It pays off at close range
   (cutscenes, a third-person open world camera), much less behind a fixed isometric camera
   where a head is 20 to 40 pixels tall.

## What the fork has today

- Path tracer materials: GGX with multiple-scattering compensation, clearcoat, back lighting,
  Transmission with IOR, Beer-Lambert media and scattering (`brdf_inc.glsl`,
  `raytracing_material_eval_inc.glsl`). Anisotropy and an anisotropy flow vector already reach
  the hit shader (`raytracing_custom_globals_inc.glsl`), which gives a strand direction for
  free on any material that sets a flow map.
- BLAS: triangles, plus `TYPE_AABBS` procedural geometry and intersection shaders in the
  Vulkan driver (`rendering_device_driver_vulkan.cpp`, procedural hit groups). So a
  curve/capsule intersection shader can be added without driver work.
- Vulkan headers carry `VK_NV_ray_tracing_linear_swept_spheres`; nothing uses it.
- PhysX module (`modules/godot_physx`): PhysX 5 PBD particles, deformable surfaces and
  volumes, plus the module's own position-based cloth: `XPBDClothSolver` (CPU, Macklin et al.
  2019 small steps) and `SkinnedClothSolverGPU` (`skinned_cloth.glsl`, RenderingDevice compute,
  any GPU): skinned targets with a max distance, distance and bending constraints in colour
  batches, tethers, body capsules interpolated across substeps, optional self collision.
- Earlier notes: `valence-engine-rtx-takeaways.md` section 9 lists Valence's strand hair (LSS
  BLAS, RTXCR BCSDFs, hair feature on BaseMaterial3D) and found LSS absent on an RTX 4090;
  `witcher3-remaster.md` records CDPR shipping LSS on RTX 50 with a DOTS mod on RTX 40.

## Research

### Scattering models

- **Marschner et al. 2003**, "Light Scattering from Human Hair Fibers". Hair as a rough
  dielectric cylinder with a tilted cuticle; three lobes: R (surface reflection, white,
  shifted towards the root), TT (through the fibre, coloured, the backlit glow) and TRT (one
  internal bounce, coloured second highlight with glints). Every model since keeps these.
- **d'Eon et al. 2011**, "An Energy-Conserving Hair Reflectance Model". Fixes energy loss in
  Marschner's longitudinal term; adds the exact azimuthal integral over the fibre offset `h`.
- **Chiang, Bitterli, Tappan, Burley 2016** (Disney), "A Practical and Controllable Hair and Fur
  Model for Production Path Tracing". The production standard: lobes R, TT, TRT plus a
  residual lobe for all higher bounces, longitudinal roughness and azimuthal roughness as
  artist controls, melanin and pheomelanin concentrations mapped to absorption, exact
  importance sampling of every lobe. Used by pbrt-v3/v4 (`HairBxDF`, Apache-2.0), Cycles'
  Principled Hair "Chiang" model, Arnold's standard hair, RenderMan PxrMarschnerHair, and
  RTXCR's near-field model.
- **Huang, Hullin, Hanika 2022**, "A Microfacet-based Hair Scattering Model". Elliptical fibres
  and microfacet roughness; Cycles 4.0 added it as the Principled Hair "Huang" model, with the
  older model kept as "Chiang". Better for very rough or curly hair; costlier and newer.
- **Zinke et al. 2008**, "Dual Scattering Approximation for Fast Multiple Scattering in Hair".
  Raster engines use it (or a cheap version of it) for light that bounces between many
  strands. A path tracer traces those bounces, except where the hair is a single shell.
- **Karis 2016**, "Physically Based Hair Shading in Unreal" (SIGGRAPH course). Unreal's raster
  hair: Marschner lobes with cheap fits, and a scatter approximation in place of dual
  scattering. Hair cards in Unreal use the same shading with the offset `h` integrated.
- **RTXCR** (NVIDIA RTX Character Rendering SDK): Chiang's near-field model and a far-field
  analytic model (the `h` integral done in closed form, so one evaluation per hit).

### Strand geometry for ray tracing

- **Linear swept spheres (LSS)**: a hardware curve primitive on RTX 50 (Blackwell), exposed in
  Vulkan as `VK_NV_ray_tracing_linear_swept_spheres`. NVIDIA's numbers for animated human
  hair: about 2x faster than DOTS and about 5x less VRAM; LSS needs about one vertex per
  segment, triangle tubes and DOTS more than two. Not available on RTX 40 (checked on a 4090).
- **DOTS** (disjoint orthogonal triangle strips): NVIDIA's fallback for earlier RTX cards; each
  segment becomes two crossed strips, so a ray from any side hits a strip whose width matches
  the strand. Plain triangles, so our BLAS, refit and opacity paths all work unchanged.
- **Camera-facing ribbons / tubes**: what Unreal's ray tracing does with grooms (strands are
  expanded to triangle geometry for the ray tracing scene, radius scaled by
  `r.HairStrands.RaytracingRadiusScale`). Ribbons facing the camera break for secondary rays;
  DOTS exists to fix that.
- **Intersection shaders on AABBs**: round cones or capsules per segment (as LSS does in
  hardware), or Bezier curves (Woop et al. 2014 oriented bounds; Reshetov and Luebke 2018
  "Phantom Ray-Hair Intersector"). Exact and compact, but software intersection is slow on
  RT cores; Embree and OptiX use it on CPU or older hardware.

### Simulation

PhysX 5 has no hair or rod solver; the closest is PBD particles with springs. The methods
games and engines use:

- **Muller, Kim, Chentanez 2012**, "Fast Simulation of Inextensible Hair and Fur" (dynamic
  follow-the-leader, NVIDIA PhysX research). Each strand solved root to tip in one pass with a
  velocity correction; very cheap, no stretching, stable at game step sizes.
- **AMD TressFX** (MIT): guide strands on compute, length constraints, local and global shape
  constraints pulling each vertex towards the skinned rest pose, capsule collision, wind;
  a few percent of strands simulated, the rest interpolated from guides.
- **Kugelstadt and Schomer 2016**, "Position and Orientation Based Cosserat Rods". Adds twist
  and bend with orientations; Unreal's groom physics (Niagara strand solver) is rod-based.
- **Frostbite** strand hair (EA, SIGGRAPH 2019 course, shipped in Dragon Age: The Veilguard
  2024): compute-simulated guides, interpolated render strands, strand rasteriser.

The module's `skinned_cloth.glsl` is already the TressFX scheme in all but name: skinned
targets with a max distance are the global shape constraint, tethers are follow-the-leader's
length bound, and the capsules and wind are shared. A strand is a cloth with only edge and
bend constraints along a chain. So hair simulation reuses the PhysX module's GPU cloth solver
with a strand topology, rather than PhysX particles (which would put another CUDA step on an
already busy GPU, see `physx.md` finding 1).

### Level of detail

Unreal switches strands to cards to meshes by screen size; Frostbite and TressFX cull and thin
strands with distance. An open world can only afford strands within a few metres of the camera;
everything further is card or shell hair, which is why stage 1 matters even when stage 2 exists.

### Licences

- RTXCR is three repositories with different terms (checked 9 Nov 2026 by cloning them):
  - `RTXCR-Material-Library`: the hair BCSDF files (`HairChiangBSDF.hlsli`,
    `HairFarFieldBCSDF.hlsli`, `HairMaterial.hlsli`) each carry an MIT header, although the
    repository's `License.txt` is the NVIDIA RTX SDKs licence. Valence ported these files with
    their MIT header; we can do the same (one line in References, header kept).
  - `RTXCR-Geometry-Library` (`CurveTessellation.h`, which builds DOTS and tubes): NVIDIA
    proprietary header ("any use, reproduction ... without an express license agreement ...
    is strictly prohibited"). Not vendored. DOTS is simple enough to write from the
    description (two orthogonal strips per segment, widths scaled by `(pi/4) / sin(pi/4)` so a
    strip pair covers the same projected area as the round strand).
  - The RTXCR sample (`hair.hlsli` and the rest): proprietary header. Reference only.
- pbrt-v4 `HairBxDF` (Apache-2.0) and Cycles' hair BSDF (Apache-2.0) are the cross-checks for
  the port (white furnace, lobe values).
- TressFX is MIT.

## Design

### Stage 1: hair BCSDF on mesh and card hair

- **BaseMaterial3D Hair section** (feature flag, off by default): `hair_melanin`,
  `hair_melanin_redness` (pheomelanin share), or `hair_color` as absorption by colour (Chiang
  section 4.2 inversion); `hair_roughness` (longitudinal), `hair_radial_roughness`
  (azimuthal), cuticle tilt `hair_tilt` 2 degrees; IOR 1.55. Shader
  outputs `HAIR_MELANIN`, `HAIR_TANGENT` for custom shaders. Same properties drive raster.
- **Strand direction**: the anisotropy flow map if set, else the tangent along UV v (how card
  UVs are laid out), else a direction from a crown point projected onto the surface (for
  sculpted shells with arbitrary UVs; one `hair_crown` vector per material).
- **Path tracer**: port RTXCR's MIT hair files to GLSL. Chiang R/TT/TRT/residual lobes with
  its importance sampling, NEE and MIS through the existing `bsdf_pdf` path; the far-field
  model (one evaluation per hit, `h` integrated in closed form) as the default for cards and
  shells, where there is no true `h`. Apply the fix Valence found in RTXCR's Chiang pdf (a
  density error that made a white furnace return +7.6 %).
- **Thin cards**: alpha-tested cards pass TT straight through, as Transmission does.
- **Solid shells** (Tripo heads): TT and higher lobes would enter the head, which has no
  hair inside. Fold them back to the reflection side through Zinke's dual-scattering forward
  and backward terms, so a shell keeps the soft backlit colour without fake geometry.
- **Raster**: Karis 2016 fit of the same lobes in the scene shader, so raster and path traced
  hair agree in colour (the "path tracing must not remove detail" check compares them).
- **Setting**: `rendering/pathtracing/hair` (off = today's GGX on hair materials).
- **glTF**: no hair extension exists; the Hair section is set after import (or by a material
  override in the import settings).
- **Tests**: white furnace on lavapipe (Chiang is energy conserving: an absorption-free fibre
  must return 1 within noise), the Disney paper's colour chart for melanin values, a
  `docs/bench/materials --scene=hair` shot pair; RR check on the PC (hair highlights are a
  known DLSS RR weak spot, compare with `--no-rr`).

### Stage 2: strands (later)

- **Asset**: a `HairGroom` resource of guide and render curves bound to a skinned mesh,
  imported from Blender hair curves (Alembic or a glTF extras block); scalp binding by
  nearest triangle and barycentrics, as Unreal and TressFX do.
- **Geometry**: DOTS triangles by default (works on every RTX card and on lavapipe), LSS when
  the extension is present (RTX 50), same BCSDF with the true `h` from the hit. Refit per frame
  like skinned meshes; strand LOD by screen size, falling back to the stage 1 card or shell.
- **Raster**: strands drawn as camera-facing ribbons with the stage 1 raster shading;
  a strand rasteriser (Frostbite, Unreal visibility buffer) only if measured necessary.
- **Simulation**: guide strands on `SkinnedClothSolverGPU` with a chain topology, Muller 2012
  follow-the-leader for inextensibility, skinned rest pose as global shape, the character's
  existing cloth capsules for collision, world wind. Render strands interpolated from guides.
- **Cost at scale**: strands for the player and characters within a few metres only; crowds
  and anything past the cutoff use stage 1 hair.

## Stage 1 as built

Paulo picked B (stage 1 now, strands after). Stage 1 differs from the design above where the
code was simpler or measured better:

- **Properties**: `hair_enabled`, `hair_color_mode` (albedo or melanin), `hair_melanin`,
  `hair_melanin_redness`, `hair_roughness`, `hair_radial_roughness`, `hair_cuticle_tilt`,
  `hair_direction` (crown, tangent, bitangent), `hair_crown`, `hair_cards`, `hair_mask_texture`
  (for single-material characters such as Tripo's). No custom shader outputs yet.
- **Path tracer** (`raytracing_hair_inc.glsl`): written from pbrt-v3/v4's `HairBxDF`
  (Chiang 2016, residual lobe included in the pdf and sampled), not ported from RTXCR. The
  fibre offset `h` is drawn per path vertex, which averages over the fibre the way a far-field
  model does. Checked in a Python mirror: white furnace 1.000, pdf integrates to 1, sampling
  density matches the pdf.
- **Shells** fold the scattering across the geometry plane (`f(L) + f(mirror L)`), cards pass
  it through. Instead of Zinke's dual scattering, both use the colour Chiang's section 4.2
  gives a mass of hair: the fibre's absorption is raised to `-ln(c) / (pi / 2)`, which makes one
  fibre's albedo match `c` within 0.05 for `c` from 0.3 to 0.9, so the path tracer, the raster
  and `rendering/pathtracing/hair` off all show the same hair colour. The cost: one scatter, so
  no soft multiple-scattering glow deep in light hair.
- **Raster**: anisotropic GGX across the strands (ANISOTROPY 0.8, Kajiya-Kay-like band) and
  the melanin colour, not the Karis fit. A Karis/Marschner raster lobe is a follow-up.
- **Bench**: `docs/bench/materials --scene=hair [--hair=back|mask|cards]`.
- **Open**: RR check on the PC (hair speckle is a known RR weak spot).

## Stage 2a as built: strand geometry

- **HairGroom** resource: strands as polylines (`points`, `strand_point_counts`), root triangle and
  barycentrics when grown. `grow_on_mesh()` spreads roots by area under a mask, leaves the
  surface combed away from a crown, droops, curls (helix) and frizzes, kept outside the ellipsoid
  of the scalp's bounds. `load_hair_file()` reads Cem Yuksel's HAIR format.
- **HairMesh** (PrimitiveMesh): two crossed strips per segment (the DOTS layout, written from its
  description, not NVIDIA's proprietary geometry library), tapered root to tip, strip frames
  carried by parallel transport. UV.x across the strip (its tangent), UV.y root to tip.
- **Hair direction Strands** on BaseMaterial3D: strands along the bitangent, light passes through,
  and the path tracer takes the true fibre offset `h` from the hit instead of drawing it: the
  offset across the strip, projected on the axis across the view (pbrt's sign convention).
- From Blender (hair curves object selected), a HAIR file for `load_hair_file()`:

```python
import bpy, struct
obj = bpy.context.active_object
curves = obj.data
sizes = [c.points_length for c in curves.curves]
with open(bpy.path.abspath("//hair.hair"), "wb") as f:
    f.write(b"HAIR" + struct.pack("<III", len(sizes), len(curves.points), 3))
    f.write(struct.pack("<Ifffff", 0, 1.0, 0.0, 1.0, 1.0, 1.0) + bytes(88))  # 128-byte header
    f.write(struct.pack(f"<{len(sizes)}H", *[n - 1 for n in sizes]))
    for p in curves.points:
        v = obj.matrix_world @ p.position
        f.write(struct.pack("<fff", v.x, v.y, v.z))
```

  (Blender is Z-up in metres: load with `load_hair_file(path, 1.0, true)`.)
- **Next (2b)**: guide strands simulated on the cloth compute, render strands following them.

## Stage 2b as built: strand simulation

- **PhysXHair3D** (PhysX module, `cloth/`): a node that is the head. It simulates every strand of
  a HairGroom on compute (`hair_strands.glsl`, one thread per strand, any GPU, no CUDA) and draws
  it as crossed strips rebuilt in `vertex()` from a positions texture (no CPU readback). Its own
  pass next to `skinned_cloth.glsl` rather than the cloth solver itself: a strand is a chain, so
  one thread walks it root to tip and the cloth's graph colouring and tethers are not needed.
- Per substep: the root moves with the head (transform blended across substeps, so a fast head
  sweeps its roots); the other points take a Verlet step under gravity, wind with per-strand
  gusts and drag, and are pulled toward the groomed pose carried by the head (TressFX global
  shape, `root_follow` fading to `tip_follow` over `follow_range`, 0.3 of the strand: pulled along the
  whole strand, even a gale barely moved the hair). Then root to tip: local shape (each rest segment turned
  by the parent's change of direction, `shape_stiffness`), collision spheres, and
  follow-the-leader length with the DFTL velocity correction (`length_damping`, Muller 2012).
- Every strand simulates; no guide interpolation yet. 5,000 strands of 20 points is 100k points,
  a small dispatch next to the path tracer. Guides plus interpolation (TressFX, Frostbite) are the
  step for 100k-strand grooms.
- Custom shaders shade as hair under PT: new outputs `HAIR`, `HAIR_ABSORPTION`, `HAIR_ROUGHNESS`
  and render modes `hair_strands` / `hair_cards`, read by the custom hit-group path the same way
  BaseMaterial3D's Hair section is. The node's built-in shader uses them, with melanin colour.
- No strand-strand collision (TressFX and Frostbite skip it too; shape constraints keep volume).
- Revision after the first clips read as heavy rope (HairWorks' Physical tab as the reference:
  mass scale, damping, inertia scale and limit, stiffness strength / damping / root / tip / bend,
  hair interaction; guide curves with interpolated strands):
  - **Air**: velocity relaxes toward the air's at `air_drag` (5/s; one loose fibre is ~20/s,
    falling at ~0.5 m/s), implicit. `wind` is the air's velocity in m/s, with `turbulence`:
    eddies of `turbulence_size` carried along by the wind (Taylor's frozen turbulence).
  - **Guides**: `guide_ratio` (0.1) of the strands simulate; the rest follow their three nearest
    guides by root (inverse square distance), each guide's point at the same share of its length
    plus the groomed offset turned with the guide's segment (TressFX follow hairs). Strands move
    in clumps and keep their own curl.
  - **Inertia scale / limit** (0.6, 0.5 m/s): above the limit, part of the head's motion carries
    the strands instead of being left to inertia.
  - **Shape damping** (0.5): part of the global shape pull moves the point without becoming
    velocity, so it settles instead of bouncing.
- Bench: `docs/bench/materials --scene=strands_sim --fixed-fps 30 --write-movie` (`--hair=wind`: head
  still in a gusty gale).

## Stage 2c: the TressFX solver

The approximation above (DFTL, my own shape and inertia terms) was replaced by a port of AMD
TressFX 4.1's simulation (MIT, GPUOpen; `TressFXSimulation.hlsl`), after Paulo asked for the
weight of HairWorks hair (HairWorks itself is DX11/12, GameWorks-licensed and discontinued).
TressFX runs one kernel per stage over all strands; here one thread walks a guide strand through
the same stages, which is the same maths (length constraints run Gauss-Seidel root to tip
instead of TressFX's red-black Jacobi, which converges faster). Per substep:

1. **Pinned roots**: the first two points follow the head (TressFX's immovable vertices), so the
   root segment keeps the groomed direction.
2. **Integrate**: Verlet with TressFX's `decay = exp(-damping * 60 dt)` (damping 0.08), except
   the velocity decays toward the air's rather than zero: in still air it is TressFX's, in
   `wind` (m/s, with gusts and carried turbulence, kept from 2b) the hair takes up the air at
   4.8/s, the drag rate that read right in 2b.
3. **Global shape**: `p += global_stiffness * (groomed - p)` for points within `global_range`
   (0.1 per 1/60 s over 0.3 of the strand; TressFX ships 0 and 0, so this is a groom choice).
4. **Velocity shock propagation**: the rotation from the old root segment to the new, plus the
   root's move, applied to the rest of the strand and its previous positions with weight
   `vsp_coeff` (0.8), or 1 when the root's acceleration exceeds `vsp_accel_threshold`
   (TressFX's 1.4 cm/frame² at 60 Hz, about 50 m/s²). This is what makes the hair weighty but
   attached: it keeps up with a turning head instead of trailing like rope.
5. **Local shape**, `local_iterations` (2) times: for each point, the next segment's groomed
   vector turned by the rotation from the groomed parent segment to the current one; both ends
   moved toward it, stiffness `0.5 * min(local_stiffness, 0.95)` (0.9 by default). Keeps waves
   and curls.
6. **Length**, `length_iterations` (2) times: distance constraints, half each, all on the free
   end next to a pinned point.
7. **Collisions**: points inside a sphere are pushed out and their velocity zeroed (prev = pos).
8. **Speed clamp**: `max_speed` (12 m/s; TressFX's 20 cm per frame).

Stiffnesses given per 1/60 s are converted per substep (`1 - (1 - s)^(60 dt)`), so `substeps`
changes accuracy, not the look. Follow strands are unchanged from 2b (three nearest guides, the
groomed offset turned with the guide segment; TressFX adds its offset unturned).

Test head: 30,000 strands (3,000 guides) of 20 points, 0.3 mm wide at the root.

## Stage 2d: raster strands (TressFX coverage)

Raster strands 0.3 mm wide are far below a pixel, so each pixel either hits one or misses: grain
that no amount of strands fixes. TressFX (`TressFXStrands.hlsl`, `ComputeCoverage` in
`TressFXRendering.hlsl`), HairWorks (MSAA coverage) and Unreal (minimum pixel width plus
coverage, resolved by TAA) all draw strands at least a pixel wide and fade them to the share of
the pixel the real fibre covers. Ours, in the PhysXHair3D render shader, raster only:

- One ribbon facing the camera (the second crossed strip collapsed), half width = real half
  width + 0.71 px, the pixel measured from the projection and `VIEWPORT_SIZE` at the point's depth.
- Coverage: wider than a pixel, solid with a one-pixel soft edge; thinner, a tent across the
  ribbon whose area equals the strand's real width, so the hair's total opacity is physical.
- Blended, as TressFX blends: `ALPHA` = coverage. First tried hashed alpha (`ALPHA_HASH_SCALE`,
  #203): order-free, but dither that only TAA cleans up, so stills stayed grainy. Blending needs
  order: unsorted, the far side of the head painted over the near side and the skull showed as a
  dark disc. So the solver sorts the strands every frame (MODE_SORT_KEYS: each strand's middle
  point's view depth; MODE_SORT_STEP: bitonic sort on the GPU, 120 passes for 32k strands) and
  MODE_OUTPUT writes the positions in that order, fixed-size slots per strand (padded, w = -1),
  so the static index buffer draws far to near. Per-strand order, not per-fragment (TressFX's
  PPLL/ShortCut): a strand that wraps from back to front can still be slightly off, which thin
  strands hide. Smooth with no AA; TAA or MSAA still help on edges.
- Shadow maps: the same ribbons; coverage as the share of texels kept (interleaved gradient noise
  discard, no blending there), so the hair's shadow is partial and accumulates through the volume, a stochastic stand-in for TressFX's and Unreal's deep opacity maps. The
  widening is capped at 2 mm there (a far cascade's texel would make every strand centimetres
  wide; that stalled lavapipe) and at 1 cm elsewhere.
- Path tracing keeps the real-width crossed strips (`IN_PATH_TRACER`, a new spatial built-in:
  true in the RT capture variant and in hit shaders), so PT detail is unchanged.

Bench: `--scene=strands_sim --raster --aa=taa|msaa|both|none`.

## Stage 2e: raster hair shading and light through the hair

With Godot's standard anisotropic GGX on the strands, the side away from the light fell black: a
lambert-style N.L on a ribbon facing the camera, and shadow maps where every strand blocked
light fully. Real hair is lit there by light travelling through the fibres (TT) and by multiple
scattering between them, which is most of what blonde hair looks like (Zinke et al. 2008: the
multiply scattered part dominates for light hair). What engines do:

- Unreal (Karis 2016, `HairShading` in `ShadingModels.ush`): Marschner's R, TT and TRT lobes with
  Gaussian longitudinal terms and cheap fits for the azimuthal terms (`Np` R `0.25 cos(phi/2)`,
  TT `exp(-3.65 cos phi - 3.98)`, TRT `exp(17 cos phi - 16.78)`), absorption as powers of the base
  colour, and a multiple scattering approximation: light wrapped around a normal facing the
  viewer, mixed with Kajiya-Kay diffuse, tinted `pow(base / luma, 1 - shadow)` so deeper hair
  turns a richer colour. Strand groom adds deep opacity maps and a dual scattering LUT.
- TressFX: Kajiya-Kay with two shifted highlights plus deep shadow / opacity fragments.
- Frostbite and others: Marschner-like lobes in one form or another, with deep opacity shadows.
- Zinke et al. 2008, dual scattering: global multiple scattering along the shadow path is the
  forward transmittance of one fibre to the power of the strands crossed (from a deep opacity map),
  plus a local constant term.

Ours (PhysXHair3D `hair_shading`, default on; off = the previous standard lighting):

- A `light()` in the render shader evaluates Unreal's model per light, with the strand tangent
  from the vertex stage. The longitudinal widths use Chiang's mapping of the fibre roughness and
  the same 2 degree cuticle tilt, so the raster highlights sit where the path tracer puts them.
  All of it goes to `SPECULAR_LIGHT` (colour is already in the lobes).
- The shadow term for the tint is the light's attenuation for directional lights (shadow only);
  omni and spot lights mix falloff into it, so their tint uses 1 (no tint).
- Shadow maps: each strand keeps `coverage * (1 - 0.91 * luma(sqrt(base)))` of its texels: one
  fibre's forward transmittance (two Fresnel crossings at n = 1.55, absorption across it as
  Unreal's `Tp` at normal incidence). Through n strands, light falls as that to the n-th power,
  the dual scattering global term, with no deep opacity map: our stochastic shadow map already
  counts strands. Blonde lets about 60% through a fibre, dark brown about 20%.
- The path tracer ignores `light()` and keeps its own Chiang BSDF.

Bench: `--scene=strands_sim --raster --hair=wind_wavy[_back][_blonde][_plain]`.

## Stage 2f: distance LOD

Hair far away is all fill: tens of thousands of blended ribbons, each under a pixel. What
engines do:

- TressFX 4.1: `LODStartDistance`, `LODEndDistance`, `LODPercent` and `LODWidthMultiplier`;
  the strands are shuffled at load, so drawing the first share of them is an even random subset,
  and the drawn ones are widened.
- Unreal groom: LOD levels with a curve decimation share and a thickness scale per level, then
  cards and meshes further out; strand count follows screen size.
- HairWorks: density and width scaled with distance (`lodDistanceFactor`, `lodWidthFactor`).

Ours (PhysXHair3D `lod_enabled`, `lod_start_distance` 2 m, `lod_end_distance` 12 m,
`lod_strand_ratio` 0.25): the share drawn falls linearly with camera distance to the ratio. Each
strand has a fixed random rank (PCG hash of its index), and MODE_OUTPUT flags a strand ranked
above the share by adding 2 to its texels' w. The render shader collapses flagged strands in
raster (zero width, no fragments) and widens the rest by 1 / share, so with TressFX coverage the
hair's total opacity stays the same. The padding past a strand's end now also draws nothing
(before, a zero-coverage ribbon). The simulation (guides and followers) and the path tracer keep
every strand at real width: path tracing must not lose detail, and a dropped strand that came
back would need its follow positions anyway.

Found while measuring it: since #203 the pixel size was computed from `PROJECTION_MATRIX[1][1]`,
which is negative (the projection flips y), so it came out huge and every raster strand was
widened by the 1 cm cap (2 mm in shadow maps) instead of 0.71 px: hair was a soft smudge of 2 cm
ribbons at any distance, and that much wider ribbon hid the LOD's drop in strands. With `abs()`
the strands are sharp, a near frame renders about 3x faster on lavapipe, and LOD at 8 m matches
drawing every strand (mean colour within 0.3/255) while the frame takes about a third less.

Not done: dropping the vertex work too (the index buffer still covers every slot; a draw range
over the sorted slots could do it), screen-size rather than distance, and cards at range.

Bench: `--scene=strands_sim --raster --hair=wind_wavy_far[_nolod]`.

## Stage 2g: body collision (signed distance field)

Spheres fit a skull well and shoulders, collars and backs badly. What engines do:

- TressFX 4.1 (`TressFXSDFCollision.hlsl`): every frame the skinned collision mesh writes a
  signed distance field on the GPU (per triangle, the cells in its padded box take the smaller
  distance with an atomic min, signed by the face), then each hair vertex inside the margin moves
  out along the field's gradient (central differences of trilinear samples).
- Unreal groom: physics asset capsules and spheres (Niagara), optionally a skinned mesh SDF.
- HairWorks: collision spheres and capsules.

Ours (PhysXHair3D `collision_body`, `collision_cell_size` 1 cm): the TressFX scheme, with the
field filled everywhere. The body's triangles are skinned on the CPU (skeleton x bone pose x bind
pose, weighted, as the GPU skins; rigid meshes take their transform) and uploaded. In a grid of at
most 64^3 reaching 5 cm past the body: MODE_SDF_BUILD gives each cell a triangle crosses its
smallest distance (`atomicMin` on the float's bits), MODE_SDF_SEED marks which triangle that is,
jump flooding (Rong and Tan 2006, steps 32..1 plus one extra step 1) carries the nearest triangle
to every cell, and MODE_SDF_FINAL stores the distance to it, signed by its face. TressFX keeps only
a narrow band, so a strand more than a few cells inside stayed there; tried first, and a groom
whose rest pose crossed the shoulders hung straight through them. MODE_SDF_COLLIDE runs after the
followers are placed, per strand past the two pinned points, and like the spheres it stops a
pushed point (prev = pos). Limits: a closed mesh is assumed; the sign comes from the nearest face,
right past convex edges and approximate in concave ones.

Bench: `--scene=strands_sim --raster --hair=wavy_box[_body][_skinned]`.

## Stage 2h: hair volume (strand interaction)

Without it, strands pass through each other: hair falling on the shoulders or blown flat packs
into a thin sheet, and every strand swings on its own. Simulating strand-strand contacts directly
costs O(n^2) and is noisy; production and engines use a grid instead:

- Petrovic, Henne, Anderson 2005 (Pixar, "Volumetric Methods for Simulation and Rendering of
  Hair"): splat the hair's density and velocity into a coarse grid; filter each point's velocity
  toward the grid's (friction), and push hair out of cells denser than a target (pressure).
- McAdams, Selle, Ward, Sifakis, Teran 2009 ("Detail Preserving Continuum Simulation of Straight
  Hair"): the same continuum view with an incompressible-style pressure solve, plus point
  contacts where detail matters (film, offline).
- Unreal's groom solver (Niagara): a velocity and density grid built from the strands each step,
  used for friction and a pressure push between strands, as Petrovic.
- TressFX 4.1 and HairWorks: none (strands pass through each other).

Ours (PhysXHair3D `volume_enabled`, on; `volume_friction` 0.2, `volume_stiffness` 1.0):
Petrovic's scheme. The grid is 32 cells a side in groom space (it moves with the head), covering
the groom grown by half its longest strand. At build the CPU splats the groom's own guide points
into it (the rest density); each frame MODE_VOL_SPLAT splats the guide points' density and
velocity (trilinear, fixed point atomics) and in MODE_STEP every point takes `volume_friction` of
the gap to its neighbours' mean velocity and is pushed down the gradient of the density above
the rest one (never below half the groom's mean, so blown hair can gather a little before it
pushes). Only guides take part (follow strands ride them). Off: as before.

Bench: `--scene=strands_sim --raster --hair=wavy` against `--hair=wavy_novolume`.

## Stage 2i: motion vectors for raster strands

TAA, FSR 2 and DLSS reproject each pixel by its motion vector. Godot writes motion vectors only
for opaque surfaces, so the blended strands took the vectors of what lay behind them (the head,
or the still background): moving hair smeared and ghosted. Two things were missing: the vertex
function, which Godot runs a second time with last frame's transforms for the motion vector, read
this frame's strand positions both times; and the alpha pass did not write motion vectors at all.

What engines do: Unreal writes hair strand velocity from its own previous-position buffer (the
groom keeps last frame's positions), and translucent materials can opt in to writing velocity
("Output Velocity"). TressFX keeps no previous positions.

Ours: MODE_OUTPUT keeps where each point was put last frame (per point, so the far-to-near draw
order can change between frames) and writes it beside this frame's positions in a second image;
the shader built-in PREVIOUS_FRAME (true while the vertex function runs for last frame) makes the
hair shader rebuild the ribbon there. The render mode `alpha_motion_vectors` lets a blended
surface write motion vectors in the alpha pass, blended by its alpha (the shader passes alpha in
the vector's w; other blended surfaces leave the target untouched), so dense hair takes over a
pixel's motion as it takes over its colour. A paused simulation copies the current positions to
the previous ones (no stale motion). Raster only; the path tracer's own motion is unchanged.

Bench: `--scene=strands_sim --raster --hair=wavy_taa`.

## Stage 2j: deep opacity shadows

The raster strands shadowed themselves through Godot's shadow maps: each strand kept a share of
the texels it covered (its coverage against interleaved gradient noise). Two faults: every strand
at a texel compared against the same noise, so a texel behind several strands kept the largest
coverage rather than 1 - (1 - c1)(1 - c2)...; and a depth map holds one depth, so the hair's
self-shadow was grain that PCF only partly smoothed.

What engines do:

- Lokovic and Veach 2000, deep shadow maps: per texel, a transmittance function of depth (film).
- Kim and Neumann 2001, opacity shadow maps: opacity in slices at fixed depths.
- Yuksel and Keyser 2008, deep opacity maps: the slices start at each texel's first hair, so a
  few layers cover the hair closely; one depth pass and one accumulation pass.
- Unreal's hair: deep opacity maps (first depth plus four layers) for hair receivers, per light.
- TressFX 4.1: the distance past the first hair in the shadow map times a fibre density.
- Enderton et al. 2010, stochastic transparency: each layer draws its own random threshold, so a
  depth shadow map's coverage averages to the product of the layers' transmittance.

Ours (PhysXHair3D `deep_shadows`, on, with `hair_shading`; `deep_shadow_light`, or the brightest
shadowed DirectionalLight3D): Yuksel's map from the solver's final strand positions, 256 x 256
cells across the hair's bounding sphere, in compute: MODE_DOM_FRONT keeps each cell's first hair
(atomicMin), MODE_DOM_ACCUM adds every strand sample's coverage (width times length over the
cell's area, bilinear) to one of three layers ending 1, 4 and 15 cm past it, and the collision
spheres and collision body give the first opaque depth (the head shadows the hair behind it).
The hair shader sums the coverage up to its own depth (linear within the layers) over 4 cells and
takes `exp(-a * coverage)`, with `a` the fibre's share of light it stops (as the shadow-map pass,
light through blonde hair). So the hair is not shadowed twice, it moves its directional shadow
lookup to just in front of the first hair with a new shader output, `DIRECTIONAL_SHADOW_OFFSET`
(metres toward the light, an extra shadow bias for that fragment), which keeps everything beyond
the groom. The hair still casts into the shadow maps for every other surface, now with each
strand's own noise threshold (stochastic transparency), so the shadow behind several strands is
their product. Limits: one light; opaque things inside the hair count only through the collision
spheres and body; other directional lights lose the hair's self-shadow (their lookup moves too).

The map is read per strand point in the vertex function (4 cells, bilinear) and interpolated: a
strand is far thinner than a cell and its segments are about one cell long, so per fragment it
cost twice as much for the same picture. The fibre density in the exponent, `dom_density` 0.3,
is calibrated against a path traced frame of the same groom: mean brightness over five regions
(lit side, centre, far side, crown, lower left) is off by 130 levels in total without the map
and 43 with it; the hard terminator the head drew across the hair is gone. The far side stays
darker than the path tracer (light scattered several times inside the hair is not carried).

Cost (cloud, software Vulkan, 30k strands, 60 frames): 139 s without, 158 s with (+14%), most
of it the four compute passes; the GPU cost is on the PC queue.

Bench: `--scene=strands_sim --raster --hair=wavy` against `--hair=wavy_nodeep`.

Tried and not kept: Zinke's dual scattering (as Unreal) driven by the map's hair count, with
the forward transmittance af^n, the lobes spread by n fibre widths and the back-scattering A_b
in place of the wrapped multiple-scattering term. Against the same path traced frame the error
rose from 43 to 60-82 (best fibre density): for brown hair a fibre passes little light forward,
so A_b is small and the lit side lost the light the wrapped term gave it. The far side's
remaining gap does not move with any of it: there the head blocks the sun, and the path tracer
lights it with the sky and the light the floor bounces, which raster gets only from global
illumination (SDFGI, VoxelGI), not from the hair shading.

Also tried and not kept: sky occlusion through the hair, as Unreal's hair voxels. The solver's
32^3 density grid went to the material (head and body opaque), and each strand point marched 14
directions through it for an AO term and a bent normal for the ambient light. Against the path
traced frame the error rose from 43 to 48: the hair's raster colour is already the colour of a mass
of fibres (its own shadowing included), so occlusion only darkened hair the path tracer shows
brighter. Turning the reflections to the bent normal did lift the far side, but with a grey sky
sheen from grazing Fresnel, not the hair's colour. The experiment is kept as a patch outside the
tree. On the way it found two path tracer bugs, fixed: a material writing BENT_NORMAL_MAP failed
to compile its hit shader (the object vanished), and the dynamic BLAS scheduler never refit an item
larger than the whole refit budget (1.5 M triangles), so a 30k-strand groom stayed where it was
first built while the raster hair moved.

## Stage 2k: deep shadows from omni and spot lights

Stage 2j's deep opacity map follows one directional light; under lamps (night streets, rooms)
the hair fell back to the shadow maps' grain. Unreal's hair builds a deep shadow map per light and
groom, for local lights a perspective view from the light that just holds the groom's bounds;
TressFX's shadow likewise follows whichever light casts it.

Ours (`deep_shadow_local_lights`, default 3): up to three shadowed OmniLight3D / SpotLight3D
nodes, the ones that light the hair most (energy times Godot's omni falloff at the hair's centre,
spot lights outside their cone skipped), each get a map beside the directional light's, built by
the same passes with the map index in the push constant. A local map is a perspective view from
the light: cells across by angle (the half angle's tangent just holds the hair's bounding
sphere), depth the distance from the light less that to the sphere's near side, so the layers
(1, 4, 15 cm past the first hair) mean the same as in the directional map. Strand coverage uses the
cell's width at the segment's depth and the segment's length across the light's ray; the head's
spheres are hit by rays from the light through each cell. A light inside the bounding sphere gets
no map. The hair shader reads every map per strand point (4 cells each) and light() finds a
light's map by direction (toward an omni or spot light's position, or along a directional one).

So the hair does not shadow itself twice, a new fragment output, `POSITIONAL_SHADOW_OFFSET`, moves
the omni and spot shadow lookups toward each light by that many metres (as
`DIRECTIONAL_SHADOW_OFFSET`); the hair sets it to the largest of its local maps' distances past
the first hair. Lights without a map lose the hair's self-shadow through it (their lookups move
too), the reason the strongest lights take the maps. The raster's second (collapsed) strip now
skips the map lookups, which halves their per-vertex cost.

Measured (bench `--hair=wavy_lamp`, an omni light up and to the side, sun off, sky dimmed; mean
brightness over the five regions against a 16 spp path traced frame): error 321 with the shadow
maps alone (`wavy_lamp_nolocal`, the hair washed out pale), 105 with the lamp's map (the warm,
darker mass the path tracer shows). The sun scene is unchanged (error 42.9 before and after).
`wavy_spotlamp` renders the spot light's map. Cost on the cloud's software Vulkan, 60 frames: 184 s
without the lamp's map, 197 s with it (+7%); the GPU cost is on the PC queue.

## Stage 2l: cheaper deep shadows

The maps' splat passes (MODE_DOM_FRONT and MODE_DOM_ACCUM) cost most of the maps: every strand
segment sampled twice per cell it crosses, each sample splatted bilinearly into 4 cells with an
atomic each. Unreal rasterizes each strand into its deep shadow map as a line (one texel per
step); the hair shader already filters across 4 cells when it reads. So each segment is now
sampled once per cell it crosses, each sample into its nearest cell: an eighth of the atomics.
Measured on the cloud with the map passes run 20 times a frame to isolate them: 5.6 s per 20
frames before, 1.8 s after (-68%). Picture: sun scene mean difference 0.04 levels (error against
the path tracer 42.9 -> 41.4), lamp scene 0.11 levels (104.8 -> 109.4, about one level a region).

## Stage 2m: strand-strand collision

The hair volume (Petrovic et al. 2005, stage 2f) keeps the hair's density but its cells are about
2 cm, so single guide strands still pass through each other. Huang et al. 2023 handle hair-hair
contact on a 1 cm Eulerian grid with Drucker-Prager friction, which still cannot stop two strands
crossing inside a cell; the established real time answer is position based contact between strand
points (Müller et al. 2007) found through a spatial hash (Teschner et al. 2003), with each point's
corrections averaged (Jacobi, Macklin et al. 2014) so the result does not depend on the order.

Built on the guides only (the followers take their places from them), once a frame after the
substeps: MODE_COLL_CLEAR empties a hash of buckets (the next power of two at least twice the guide
points, 8 points a bucket), MODE_COLL_INSERT puts each guide point into its cell's bucket,
MODE_COLL_SOLVE looks through the 27 neighbouring cells for points of other strands closer than
`strand_collision_radius` (default 2 mm, a lock's thickness) and averages the push apart, less
`strand_collision_friction` of the sliding between them, and MODE_COLL_APPLY moves the points. The
two root points stay pinned. On the wavy bench head shake the hair is a little fuller where it
falls over itself (frame 75 against no collision: mean 0.25 levels, 99th percentile 6.3); with an
8 mm radius 0.45 / 12. Cloud time with and without is within its noise (four small passes over
the guide points).

## Stage 2n: swept spheres on GPUs that have them (plan)

Linear swept spheres (LSS, `VK_NV_ray_tracing_linear_swept_spheres`, NVIDIA RTX Hair; shipped in
Indiana Jones and The Witcher 3 remaster, 50-series only): the ray tracing hardware intersects a
round tube between two points with a radius at each end, so a strand segment is one primitive with
one point and one radius per strand point, against our four triangles over eight vertices. NVIDIA
measures about 2x faster tracing and 5x less memory than its own crossed-strip geometry (DOTS), and
the hit is on the real round fibre, so the fibre offset h of the Chiang model is exact instead of
read across a flat strip.

The path tracer already knows which surfaces are strands (`render_mode hair_strands`, HairMesh
layout: four vertices per point, two strips, four triangles per segment). On a device with the
extension, such a surface whose index list has that pattern is built as LSS instead of triangles:

- Driver: the extension and its `linearSweptSpheres` feature are requested; the BLAS takes a new
  geometry type (`VkAccelerationStructureGeometryLinearSweptSpheresDataNV` chained on the geometry,
  positions and radii from one vec4 buffer, successive indexing, chained end caps); ray tracing
  pipelines get `VK_PIPELINE_CREATE_2_RAY_TRACING_ALLOW_SPHERES_AND_LINEAR_SWEPT_SPHERES_BIT_NV`.
- Every frame, after the vertex capture, a small pass writes each point's centre (between the two
  sides of its first strip) and radius (half the strip's width); the segment list is built once.
- Shaders get `RT_LSS` (and `GL_NV_linear_swept_spheres`) only on such devices. A swept sphere hit
  is mapped to the first strip's triangle at the same place along the segment, with barycentrics on
  the strip's centre line, so every attribute (UV, colour, previous position for motion vectors)
  comes from the same captured vertices as today; the hair shading then takes the exact h from the
  hit and the round fibre's normal.
- Ray queries (shadows, transmitted shadows) treat a swept sphere candidate as an opaque hit
  (the strands are `rt_force_opaque`).

Everywhere else the shaders and the BLAS are built exactly as before, which is all the cloud and a
40-series card can check; the LSS path needs a 50-series GPU to test.

Built (#258). The setting is off by default until the swept sphere path has run on a 50-series GPU.

## Stage 2o: wet hair from the rain

What changes when hair gets wet, from the literature:

- Ward, Galoppo and Lin, "Modeling Hair Influenced by Water and Styling Products" (CASA 2004): wet hair
  is darker and shinier, heavier, softer and gathers into clumps. A thin water film smooths the
  cuticle scales into a near mirror surface; light kept inside the wet mass is absorbed more.
- Gupta and Magnenat-Thalmann, "Interactive Rendering of Optical Effects in Wet Hair" (VRST 2007):
  hair takes up about 45% of its dry weight in water; the fibre roughness falls as
  alpha (1 - c w); bending stiffness x (1 - 0.45 w); child strands are pulled onto their guides
  as w rises.
- Lekner and Dorf, "Why some things are darker when wet" (Applied Optics 1988): water between
  the scatterers nearly matches their index (1.33 against keratin's 1.55, relative 1.165, so a
  fibre's reflection at normal incidence drops from 4.6% to 0.6%), light goes deeper and is
  absorbed over more fibres.
- Bico, Roman, Moulin and Boudaoud, "Adhesion: Elastocapillary coalescence in wet hair" (Nature 2004):
  water pulls neighbouring fibres together where their free length beats their bending stiffness,
  so clumps close from a little past the root out to the tips.
- None of them gives a drying rate.

Built on PhysXHair3D (`wetness`, `weather_wetting`, `water_capacity`, `drying_time`,
`wet_clumping`), off when nothing wets the hair:

- Wetting: rain from the WeatherMap3D (mm/h) less what the canopy catches (free throughfall,
  Gash 1979: the share of sky the canopy leaves open), none under a roof (a ray up from the top of
  the hair every 0.5 s), fills `water_capacity` (1 mm: about 27 g of water, 45% of a head of hair,
  over a 0.03 m2 crown). Drying is an even rate, soaked to dry in `drying_time` (1 h by default, a
  guess at air drying long hair; no measured rate found).
- Look (raster and path tracer, the same parameters): the longitudinal roughness halves when soaked
  and the fibre absorption rises by half (the darkening, standing in for index matching; tuned, not
  measured). The raster mass colour follows the absorption.
- Motion: mass x (1 + 0.45 w), so the air's drag moves the strands less; global and local shape
  stiffness x (1 - 0.45 w), so the hair hangs flatter; the volume's push apart x (1 - w);
  followers' groomed offsets from their guides shrink by `wet_clumping` x w, ramping in over the
  first 30% of the strand.
- The fibre index in the path tracer stays 1.55: a wet index would need a new HAIR output through
  every renderer, for a highlight change the roughness already gives.

## Stage 2p: hair cards for crowds

Far away a head of strands costs the same simulation, sort and deep shadow work as up close, and
the path tracer still refits every strand. Games draw distant (and most) hair as cards: textured
strips laid along clumps of strands. How cards are made from a strand groom:

- Unreal's groom cards (Groom asset Cards, Hair Card Generator, Dataflow): clumps of strands each
  become a few cards; per card textures of coverage, depth, tangent, root UV, along-the-strand
  coordinate and a seed; "Guide-Based" cards are skinned to the nearest simulated guide; the
  groom's LOD switches strands to cards by screen size.
- K. Wu et al., DiffHairCard (2025, arXiv 2505.18805): k-means on strands with Wang et al.'s 2009
  shape distance; the card follows the cluster's mean strand with rotation-minimising (Bishop)
  frames; its half width at each sample is the largest offset of the cluster's strands along the
  card's binormal; the root orientation is the one (of a set of candidates) with the least
  out-of-plane error; the strands are projected into card space as 2D curves (not rasterised
  from a render) for coverage, tangent, depth and occlusion; optional crossed second card.
  Their differentiable-rendering refinement takes ~46 min a groom, too slow at load.
- Tojo, Hu, Umetani, Li, Strands2Cards (SIGGRAPH Asia 2025): wisps clustered, each skinned to a
  normalised pose in UV space for its texture, textures shared between similar wisps, strips fitted
  by differentiable rendering.

Built (`cards_enabled`, off by default; `card_count`, `card_distance`), DiffHairCard's fitting
without the optimisation pass, baked once when the hair is built:

- k-means (k-means++ seeds) of the strands resampled to 8 points by length, Wang's shape distance;
  each card is the cluster's mean strand at the drawn point count (up to 16), with Bishop frames
  and the angle about it with the least spread of the strands out of its plane (DiffHairCard
  searches candidates; the least-squares answer is the principal axis of the offsets, in closed
  form); half width from the strands' offsets along the axis (a 95th percentile, so one flyaway
  does not widen it). Baked once per groom and settings and shared by every node using them.
- Two crossed strips a card, each with its own projection baked into a shared atlas tile:
  coverage (each strand's width over the texel, summed as 1 - prod(1 - c)), the strands' tilt in
  the card plane (the raster highlight follows the real strands, not the card) and a per-strand
  seed. Each strip counts by how squarely it faces the view, the two weights summing to one, as
  optical depth (opacity 1 - (1 - c)^w), so crossed cards neither double nor thin the hair.
- The card's mean strand is simulated as one more follower strand (its three nearest guides), so
  cards move with the same guides, wind, collisions and wet clumping as the strands. In card mode
  only the guides and the card strands run: the other followers and the draw-order sort are
  skipped.
- Self-shadowing: the deep opacity maps are made from the cards' strands, each splatted as wide as
  its clump with the clump's coverage (a strand heaped into one cell lets light through beside it
  and left the cards too bright), and read per card point as the strands read them.
- The cards take the strands' raster hair shading (the strand direction from the card, tilted by
  the baked tilt). Opacity is blended as the strands' is; the hair is one colour, so the unsorted
  cards' order barely shows. Alpha hash (Unreal's masked cards) was tried first and is noise
  without TAA. Shadow maps keep a share of texels (stochastic transparency).
- Raster only. Under the path tracer the hair stays strands at every distance: traced as cards
  (opaque at random by coverage) the hair came out near black and lost its strands, which the
  path tracer must not do. Crowds under the path tracer keep the strands' cost.

### Checked: cards against strands at the switch distance

Compared at 8, 15 and 25 m (`--hair=farN_cards` against `farN`), straight hair shaken and wavy hair
in wind. The cards read lighter, bluish and see-through, the head showing through them, at every
distance. Three causes, three fixes:

- Lighting: the cards took Godot's ambient and reflected light by their own face, so cards tipped
  up caught the sky. They now light with the normal facing the viewer about the strands, as the
  strands' ribbons do (and as Unreal's hair lights its cards, Karis 2016). The blue cast is gone.
- Fanning: the cards keep the clump's width from the groom, but a draped clump fans out with its
  guides (its strands sit between the same guides), so the strands spread over the head while
  the cards stayed narrow ribbons with gaps between. Each card point now takes how far its guides
  have moved apart since the groom (the solver writes it with the card's strand) and the card
  widens by that much, thinning by the same factor (optical depth over the fan), so the strands'
  total width is kept. The gaps close.
- Points: the cards took the drawn point count, which simplify_tolerance cuts to the two ends of
  a straight strand; a card through only those cuts through the head as the strand drapes. Cards
  now take up to 16 groomed points (the solver's slots grow to hold them; the strands' mesh keeps
  its own points).

Tried and not kept: more cards (512, 1024), full width instead of the 95th percentile, cards faced
out from the groom's centre, and a coverage gain of 2 to 4 all left the gaps; the gaps were the
fanning. Left then: the cards still read lighter than the strands, put down to weaker
self-shadowing.

Measured again, one light at a time (bench `--hair=far8_cards` against `far8`, the hair's mean
luminance; and close up): with the sun off the cards were 2.5 times as bright as the strands, with
the sky's light off only 1.5 times. Three causes, none of them the deep opacity maps:

- Sky reflection: the cards faced their normal toward the viewer, but the strands' ribbon frame
  (across = strand x eye, normal = across x strand) faces away from it, and Godot's horizon term
  then leaves out the reflected sky on the strands. The cards took a full sky reflection (a grey
  sheen; setting SPECULAR to 0 took 20% off the cards and 1% off the strands). The cards now build
  the strands' own frame about their strand direction.
- Scalp through the cards at the crown: a card is a slab of strands centred on its simulated strand,
  which the collision keeps only just off the body, so the slab's inner half and a flat strip's
  edges (the sagitta over the round scalp) sank into the head, and the scalp showed between the
  cards. The card strand now keeps half the clump's thickness more off the body (spheres and the
  distance field), and near the scalp (a sphere fitted to the roots, least squares) each strip is
  raised until its slab clears it.
- Deep opacity lookups at the strip's edges: the edges reach sideways out of the hair the map was
  built from, in front of it toward the light, and read as lit. The cards now read the maps on
  their centre line.

Result: close up the cards are within 7% of the strands (mean 47.1 against 44.1), at 8 m within 38%
(28.8 against 20.9; before, 40.7: 95% lighter). What is left at 8 m is mostly the body showing
between the hanging cards at the shoulders, where the strands cover it (fixed below). Also found, in the
renderer's code (not yet measured): Godot leaves alpha-blended materials out of its shadow
passes, so the strands' and cards' shadow-pass code only runs for the path tracer's shadow rays,
and the deep opacity maps carry the hair's self-shadowing in raster. Cards stay off by default.

### Fixed: cards see-through over the shoulders and head

Turning the cards' opacity to 1 wherever the atlas holds any hair showed no gaps: the body showed
through low opacity, not between cards. Four causes:

- Draw order: the strands are sorted far to near every frame (bitonic sort on the GPU) because
  they are blended, but the cards kept a fixed order (card c in slot c, sorted once). A card
  behind drew over one in front, and lit hair from the back of the head showed through the dark
  front layer everywhere but in front of the head (where the head hides it): the head's outline
  showed as a dark disc. The cards' strands are now sorted every frame too (only theirs, a few
  hundred), and the card in each slot is named in the previous positions' w (-2 - card), so the
  card mesh reads its card's axes, widths and tiles from a small texture (card_data).
- Mipmaps: the atlas held coverage, and its mipmaps averaged it. A pixel spanning several texels
  sees all their strands, each by its share of the pixel, so it lets through exp(-mean depth); the
  mean of the texels' transmittances is more (exp is convex), so the cards grew see-through with
  distance. The atlas now holds the optical depth (16 bits: R high, B low byte; filtering is linear
  in each) and makes its own mipmaps of it.
- Sub-pixel cards: a card under a pixel wide falls between pixel centres. As the strands
  (TressFX), it is now drawn at least 0.71 px a side (capped at 1 cm, 2 mm in shadow maps) and
  its depth thinned by as much.
- Oblique strips: a strip seen at an angle packs its strands into less of the screen, so their
  depth per pixel grows by sin a / |f| (a the view's angle to the strands, f the strip's facing).
  The strips' weights (f^2 / (f^2 + o^2)) left this out, and an oblique card lost up to a third of
  its opacity.

Result (bench mean luminance): close up 44.8 against the strands' 44.1 (47.1 before), at 8 m 23.6
against 21.0 (28.8 before), at 15 m 24.8 against 22.9. The sort adds about 36 small dispatches a
frame for 256 cards (the strands' own sort runs over all 30,000).

The rest at 8 m, and a faint outline of the head through the crown, was not opacity: drawn as
black hair, cards and strands let through the same light (52.1 against 52.3). With the sun alone
and no deep shadows the cards read 25.3 against 20.7, with no shadow maps 44.0 against 42.8, and
with the head casting no shadow 37.2 against 36.5: the head's shadow, on the hair hanging beside
it. Both hang in the same place (a map of each fragment's distance from the head's shadow volume
matches), and drawn opaque, the front layers of both lie in the shadow alike. The difference is
the blend: a card runs the whole length of the hair, and the cards are sorted whole by their
middle, so a lit card from beside the head still drew over the shadowed front of another.

Fix, the two-pass hair of Scheuermann (GDC 2004): the cards' dense cores (80% or more) write depth
in the pre-pass (depth_prepass_alpha), then all of the cards blend far to near over them. Godot's
pre-pass keeps only fragments of 99% or more, so a spatial shader can now set its own threshold
(OPAQUE_PREPASS_THRESHOLD, fragment output; the shadow passes keep theirs). The pre-pass would let
the cards cast into the shadow maps, so they cast none, as the strands. Result: close up 43.8
against 44.1, at 8 m 22.1 against 21.0, at 15 m 23.9 against 22.9, and no outline. Wavy hair in
wind (whose cards were 22% lighter, 52.4 against 43.0) now reads 13% darker (37.6), with a few
dark blotches where the frizz's cores overlap; cards stay off by default.

### Fixed: black specks on strands with deep shadows

Rare black 2x2 pixel blocks on strands, only with deep shadows on. Turned off one step at a time:
not the shadow-map offset into the hair (DIRECTIONAL_SHADOW_OFFSET), not the multiple-scattering
term; gone when light() does not multiply by the map's transmittance, and gone as well with that
transmittance clamped to 0..1 (min alone and max alone each clear it, so the value reaching light()
was not a number). The per-vertex values check finite and in range, so the bad value appears
between vertex and light(); not pinned further on llvmpipe. light() now clamps it, which changes
nothing for any valid value. Bench `--hair=far8`: 12 to 22 black pixels on the hair before, 0
after in every run (far8, near, wind).


## Stage 2q: hair resting on cloth

Long hair lies on a collar, a hood or a coat's shoulders, and the cloth there moves. The collision
body (stage 2h) is a skinned proxy of the body; a simulated garment is not in it, so hair sank into
the cloth down to the body under it. How others do it:

- AMD TressFX 4.1: hair collides with a signed distance field built on the GPU each frame from a
  skinned collision mesh's triangles (seeded, then jump flooded); that is what the collision body
  already is.
- Unreal's groom: hair collides with the physics asset's capsules (Niagara), not with Chaos cloth;
  hair on cloth is faked with the body's capsules swollen.
- Bridson, Fedkiw, Anderson, Robust Treatment of Collisions, Contact and Friction for Cloth (2002):
  a cloth is a sheet with a thickness, kept apart from what touches it by its distance less that
  thickness.

Built (`collision_cloth` names a PhysXSkinnedCloth3D, `cloth_thickness`): TressFX's distance field
fed the cloth's simulated triangles as well as the body's. The node gives the solver the cloth's
triangles once (its render copy's indices mapped to particles); each frame a pass reads their
corners straight from the cloth's position texture on the GPU (no readback), into the triangle
buffer after the body's, and the field is seeded and flooded over both. Cloth triangles are marked
and their distance has its thickness taken off. The distance is signed by the cloth's face, as the
body's is (its outside is where the mesh normals face), not unsigned as a free sheet's: tried
unsigned first, hair that the groom's rest pose already put under the capelet stayed under it
(the push only ever moved it further from the sheet, on the side it was). Signed, hair under the
cloth near its surface goes back over it, which is where hair lies on clothes. The cloth's triangles also shadow the hair in the
deep opacity maps, as the body's do. One frame of lag at most (the cloth's step order), under a
centimetre at walking speed. Cost: one pass over the cloth's triangles plus their share of the
seeding (the field's flood is the same size); measured below.

Bench: `--hair=cloth` puts a capelet 4 cm off the shoulders, its hem free to move 3 cm;
`--hair=nocloth` is the same without the collision.

## Stage 2r: snow settling on hair

Snow on a head of hair lies on the top layer only, as small white clumps of crystals caught
between strands, thicker on the side the wind drives it at; above freezing, or warmed by the
head, it melts into the hair as water. Before this, the hair's weather wetting (stage 2o) took the
weather's whole precipitation as rain, so a snowfall soaked the hair. How others do it:

- Hedstrom and Pomeroy (1998), snow interception by a canopy: the catch is the snowfall times how
  much room is left, `dL = P (1 - L / L_max)`; the load fills fast at first, then slows.
- Hock (2003), degree-day melt: about 0.2 to 0.6 mm of water an hour per degree above freezing for
  snow; the low end for snow lying in the open on a cold surface.
- Jennings et al. (2018), rain/snow split from air temperature and humidity: the weather map
  already uses it for its own surfaces (`WeatherMap3D.rain_fraction`).
- Games (Red Dead Redemption 2, The Last of Us Part II): snow on characters is a texture mask
  painted from above; it does not follow the hair's layers or move with the strands.

Built (`snow`, `snow_capacity`): the precipitation splits into rain and snow; rain wets as before,
snow fills a load by Hedstrom and Pomeroy, melts by a degree-day rate (0.2 mm/h/°C) plus 0.05 mm/h
from the head's warmth, and the melt adds to the wetness. Where it lies: one more deep opacity map
(stage 2j) seen from where the snow comes from, the fall (1 m/s, Locatelli and Hobbs 1974's
aggregates) against the wind, so the map gives each strand point the hair above it, in the snow's
direction. Snow is drawn as its own mesh: six small blobs per strand (octahedra, smooth shaded,
irregularly scaled), each at a random point along its strand, keyed by the strand's root so a
strand keeps its clumps when the blend sort moves it between slots. A blob shows with a weight
`snow · exp(-2 · hair above)` against its own random number, so only the top layer holds snow, and
sits on the strand's side the snow came from; clumps are 1.25 to 3.75 mm across at full load
(white, albedo 0.85, matte). The others shrink to nothing. Tried first: widening the strand itself
where it held snow; the width ran along the whole segment between two strand points, so snow
showed as white streaks a couple of centimetres long, not clumps. The blobs are opaque geometry,
so the path tracer traces the same clumps. The map costs one more slot only while snow lies, and
needs hair_shading's deep opacity maps. Not on the cards yet (hidden in card mode).

Bench: `--hair=snow` holds snow at 1.

## Checked: lighter path traced strands

The path tracer sees each strand as two crossed strips, four triangles a segment (a 30k-strand,
24-point groom is 2.8 M triangles, refit every frame). Checked before building anything lighter:

- NVIDIA's disjoint orthogonal triangle strips (DOTS, RTXCR sample, `CurveTessellation.cpp`) are
  also two orthogonal quads a segment, four triangles: the same count as ours, so no saving.
- Linear swept spheres (LSS, `VK_NV_linear_swept_spheres`): one vertex a segment, NVIDIA measures
  about 2x faster and 5x less memory than DOTS, but only 50-series GPUs have it; it cannot be
  tested on the cloud's software Vulkan or a 40-series card.
- Fewer segments where a strand runs straight (Douglas-Peucker on the groomed strand, as Unreal's
  groom LOD vertex decimation): with a tolerance of 0.2 mm (below a root width) the straight bench
  groom keeps 14 of 20 points on average, 17 for the 95th percentile and 18 at most, so the fixed
  slots shrink by about 10%; the wavy groom keeps all 24 (waves and frizz need every point).

None was built: no option gives a large saving without losing detail on a 40-series GPU.

Built afterwards at Paulo's pick (`simplify_tolerance`, default 0.5 of the root width): each
strand is drawn through its Douglas-Peucker points only, chosen once on the groomed strand; the
solver's output pass maps a slot's k-th texel to the strand's k-th drawn point (a per-strand list
in a new buffer), so every point still simulates, collides and casts into the deep shadows. The
render mesh's slots are as long as the strand that keeps the most points (the strands are sorted
into the slots each frame for blending), so the saving follows the worst strand, not the mean:
the 20-point straight bench groom draws at most 19 (5% fewer triangles), a 64-point straight
groom at most 37 (42% fewer); wavy and frizzy grooms keep every point. Raster and path traced
frames at 100% crop are the same with and without (raster mean difference 0.19 levels, the path
traced difference is its noise). Capping the slots at the 95th percentile would save 16% on the
bench groom but moves 2.6% of strands by up to 0.28 mm, so it was not done.

## Options for scope

- **A. Stage 1 only, now.** Hair BCSDF on mesh and card hair, PT and raster, one setting.
  One PR; every existing character benefits; fully testable in the cloud except the RR check.
- **B. Stage 1, then stage 2.** As A, then the groom resource, DOTS/LSS geometry and the strand
  solver on the module's cloth compute. Several PRs; useful only once there are grooms and a
  close camera; GPU cost measured on the PC.
- **C. Defer.** Keep hair as GGX until a project needs close-range characters.

Recommendation: A now, B's strand work once the open-world game has a close camera and a
groom to test with.

## References

- Marschner, Jensen, Cammarano, Worley, Hanrahan. Light Scattering from Human Hair Fibers. SIGGRAPH 2003.
- d'Eon, Francois, Hill, Letteri, Aubry. An Energy-Conserving Hair Reflectance Model. EGSR 2011.
- Chiang, Bitterli, Tappan, Burley. A Practical and Controllable Hair and Fur Model for Production Path Tracing. CGF 2016.
- Huang, Hullin, Hanika. A Microfacet-based Hair Scattering Model. CGF (EGSR) 2022.
- Zinke, Yuksel, Weber, Keyser. Dual Scattering Approximation for Fast Multiple Scattering in Hair. SIGGRAPH 2008.
- Karis. Physically Based Hair Shading in Unreal. SIGGRAPH 2016 course.
- Petrovic, Henne, Anderson. Volumetric Methods for Simulation and Rendering of Hair. Pixar Technical Memo 06-08, 2005.
- McAdams, Selle, Ward, Sifakis, Teran. Detail Preserving Continuum Simulation of Straight Hair. SIGGRAPH 2009.
- Muller, Kim, Chentanez. Fast Simulation of Inextensible Hair and Fur. VRIPHYS 2012.
- AMD. TressFX 4.1, `TressFXSimulation.hlsl` and `TressFXSettings.h` (GPUOpen, MIT, 2019). Han, Harada. Real-time Hair Simulation with Efficient Hair Style Preservation. VRIPHYS 2012.
- Kugelstadt, Schomer. Position and Orientation Based Cosserat Rods. SCA 2016.
- Woop, Benthin, Wald, Johnson, Tabellion. Exploiting Local Orientation Similarity for Efficient Ray Traversal of Hair and Fur. HPG 2014.
- Reshetov, Luebke. Phantom Ray-Hair Intersector. HPG 2018.
- NVIDIA. Render Path-Traced Hair in Real Time with NVIDIA GeForce RTX 50 Series GPUs (blog, Feb 2025); RTXCR repository and licence.
- Blender manual, Principled Hair BSDF (Chiang and Huang models).
- Unreal Engine documentation, Groom Strands; AMD TressFX (GPUOpen).
- pbrt-v4, `HairBxDF` (Pharr, Jakob, Humphreys).
- Ward, Galoppo, Lin. Modeling Hair Influenced by Water and Styling Products. CASA 2004.
- Gupta, Magnenat-Thalmann. Interactive Rendering of Optical Effects in Wet Hair. VRST 2007.
- Lekner, Dorf. Why Some Things Are Darker When Wet. Applied Optics 27(7), 1988.
- Bico, Roman, Moulin, Boudaoud. Elastocapillary Coalescence in Wet Hair. Nature 432, 2004.
- Gash. An Analytical Model of Rainfall Interception by Forests. QJRMS 105, 1979.
- K. Wu et al. DiffHairCard: Auto Hair Card Extraction with Differentiable Rendering. arXiv 2505.18805, 2025.
- Tojo, Hu, Umetani, Li. Strands2Cards: Automatic Generation of Hair Cards from Strands. SIGGRAPH Asia 2025.
- Wang, Yu, Zhou, Guo, Shum. Example-based Hair Geometry Synthesis. SIGGRAPH 2009.
- Epic Games. Unreal Engine documentation: Setting up Cards and Meshes for Grooms; Hair Card Generator.
- Bridson, Fedkiw, Anderson. Robust Treatment of Collisions, Contact and Friction for Cloth Animation. SIGGRAPH 2002.
- Hedstrom, Pomeroy. Measurements and Modelling of Snow Interception in the Boreal Forest. Hydrological Processes 12, 1998.
- Hock. Temperature Index Melt Modelling in Mountain Areas. Journal of Hydrology 282, 2003.
- Jennings, Winchell, Livneh, Molotch. Spatial Variation of the Rain-Snow Temperature Threshold across the Northern Hemisphere. Nature Communications 9, 2018.
- Locatelli, Hobbs. Fall Speeds and Masses of Solid Precipitation Particles. JGR 79, 1974.
