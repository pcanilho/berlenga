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
- Muller, Kim, Chentanez. Fast Simulation of Inextensible Hair and Fur. VRIPHYS 2012.
- AMD. TressFX 4.1, `TressFXSimulation.hlsl` and `TressFXSettings.h` (GPUOpen, MIT, 2019). Han, Harada. Real-time Hair Simulation with Efficient Hair Style Preservation. VRIPHYS 2012.
- Kugelstadt, Schomer. Position and Orientation Based Cosserat Rods. SCA 2016.
- Woop, Benthin, Wald, Johnson, Tabellion. Exploiting Local Orientation Similarity for Efficient Ray Traversal of Hair and Fur. HPG 2014.
- Reshetov, Luebke. Phantom Ray-Hair Intersector. HPG 2018.
- NVIDIA. Render Path-Traced Hair in Real Time with NVIDIA GeForce RTX 50 Series GPUs (blog, Feb 2025); RTXCR repository and licence.
- Blender manual, Principled Hair BSDF (Chiang and Huang models).
- Unreal Engine documentation, Groom Strands; AMD TressFX (GPUOpen).
- pbrt-v4, `HairBxDF` (Pharr, Jakob, Humphreys).
