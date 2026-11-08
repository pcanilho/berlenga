# Opacity micromaps for alpha-tested surfaces

Research note, 8 Nov 2026, written before any code. Item 3 of
[valence-engine-rtx-takeaways.md](valence-engine-rtx-takeaways.md#4-opacity-micromaps): leaves, grass,
weeds, fences, grates and posters are alpha-tested cards, and every ray that touches one runs the any-hit
shader to sample its alpha texture. Foliage makes that the most expensive part of a trace.

## What it is

An opacity micromap (OMM) splits each triangle into 4^N micro-triangles (subdivision level N, up to 12)
and stores a state per micro-triangle: transparent, opaque, unknown-transparent or unknown-opaque. The
ray-tracing hardware reads the state at the hit's barycentrics during traversal: transparent hits are
skipped and opaque hits accepted without the any-hit shader. Only unknown micro-triangles (the ones a
cutout edge crosses) still call it. A BLAS triangle geometry points at a micromap through one index per
triangle; an index can also be a special value (whole triangle transparent, opaque or unknown) that needs
no storage.

- Vulkan: `VK_EXT_opacity_micromap` (`VkMicromapEXT`, `vkCmdBuildMicromapsEXT`,
  `VkAccelerationStructureTrianglesOpacityMicromapEXT` chained into the triangle geometry,
  `VK_PIPELINE_CREATE_RAY_TRACING_OPACITY_MICROMAP_BIT_EXT` on the ray-tracing pipeline). Ray queries use
  the micromaps with no opt-in. Formats: 2-state (1 bit) and 4-state (2 bits). Micro-triangles are stored
  in the bird-curve order (a Morton-like space-filling curve over the subdivided triangle).
- D3D12: DXR 1.2 has the same model (and NVAPI before it). The fork's D3D12 driver is not built for
  releases, so only Vulkan matters here.
- Hardware: traversal reads micromaps natively on RTX 40 and 50 series; older RTX emulates them in the
  driver, which can still help.

## What others measured

- **Indiana Jones and the Great Circle** (NVIDIA blog, path-tracing mode, RTX 5080, 4K with DLSS RR
  Performance, Peru jungle): main trace 7.90 ms to 3.58 ms (-55 %), SHaRC update 1.99 to 0.89 ms, sun
  shadows 0.83 to 0.71 ms. The share of samples that ran an any-hit fell from 17 % to 3 %. They baked
  with NVIDIA's OMM SDK offline, skipped micromaps above 0.5 MB, used 4-state on shadow rays and forced
  2-state on indirect rays.
- **Valence** (Godot path tracer, D96, commits 1dc03dcbd6, acb9b340d9, 1dba763c79, 9c6e86a9c9,
  4e8ed01b40, b06616801b): 240,000 foliage cards, GPU -11 % with RR and -17 % without; with the 3 x 3
  bound fix the any-hit invocations dropped another 27 %. 2-state on indirect rays gave a further 5 to 8 %.
- **3DMark** added an Opacity Micromap feature test (Android, Vulkan), and Arm's learning path covers
  the same flow on Mali; the extension is cross-vendor, not NVIDIA-only.

## How to bake

Two established ways:

1. **NVIDIA OMM SDK** (github.com/NVIDIA-RTX/OMM): a CPU baker (blocking, OpenMP) and a GPU baker that
   returns a dispatch chain for the engine to run. Inputs: index buffer, UVs, the alpha texture, cutoff,
   sampler, subdivision level or a texel-based scale. Outputs: the micromap array, its descriptors and the
   per-triangle index buffer. Its rule for an exact 4-state result is the "standard form" any-hit: one
   alpha texture, a constant threshold and a constant mip. It warns that the CPU bake must see the same
   (compressed) texels the GPU samples, and that bake time and memory grow fast with level (one Indiana
   Jones micromap took 43 MB, some bakes over 30 minutes).
2. **Runtime GPU bake** (Valence D96): bake on first trace from the texture already on the GPU, so the
   texels are exactly the ones sampled and no import step or stored data is needed:
   - a min/max pyramid of the alpha texture (each level halves the one below);
   - a setup pass, one thread per triangle, that fetches the UVs exactly as the hit shader does and
     picks a level from the triangle's area in texels;
   - a CPU step after an async readback: triangles that are fully transparent or opaque take a special
     index, the rest are deduplicated by their exact UVs (tiled cards share one entry);
   - a bake pass, one thread per 16 micro-triangles: each micro-triangle's state from the smallest and
     largest alpha over every texel a bilinear sample inside it can read (3 x 3 texels read directly,
     larger boxes from the pyramid);
   - the micromap build, and a BLAS that carries it.

   Conservative by construction: a micro-triangle is opaque only if every possible sample passes and
   transparent only if every sample fails, so the image does not change; unknowns run the any-hit as
   before.

## Choice for the fork

The runtime GPU bake. Reasons: the fork already
has the textures on the GPU and the hit shader's own UV fetch; nothing changes in import or in saved
scenes; it works for any texture a material gets at runtime; and it scales with instancing, which is what
an open-world forest is (one micromap per mesh surface and texture, shared by every instance and every
MultiMesh copy). The SDK would add a dependency, an import-time bake and stored data per mesh.

What the bake has to match in this fork (read from `scene_raytracing_raygen.glsl` any-hit and
`raytracing_shadow_query_inc.glsl`):

- StandardMaterial3D (hit group 0) alpha: `albedo_texture.a * albedo_color.a`, UV1 with `uv1_scale` and
  `uv1_offset`, sampled linear-repeat (or nearest for point filtering) at the base level: ray cones are
  not set in the any-hit, so `rt_tex_lod` stays at the base level.
- The threshold is 0.5 for hit group 0 (the any-hit and the ray-query shadows both test `>= 0.5`; the
  material's `alpha_scissor_threshold` is not read by the path tracer today).
- Left out, so the any-hit alone decides: custom shaders, triplanar UV1, materials whose blend class is
  not opaque, instances with transparency or visibility fade, deformed (skinned, blend shape, captured)
  surfaces, particles, camera-facing billboards, and mesh LOD levels above 0 (their triangles are not the
  baked ones).
- Shadow rays and ray queries use `NoOpaque`: micromaps still remove the transparent micro-triangles
  there, and the opaque ones still run the alpha test, so they gain less than camera and bounce rays.
- Ray-query shadows sample at the ray cone's level (`lod`) where they have one, not at the base level.
  Their transparent micro-triangles are now skipped as the base level decides: a blurrier level could
  have kept a thin sliver; this is the one place the image can change, by sub-texel amounts.

## Design

- **Setting** `rendering/pathtracing/opacity_micromaps` (off, restart). Off asks for no extension and
  changes nothing: stock behaviour. Two tuning settings: `opacity_micromap_scale` (texels a
  micro-triangle covers, default 2) and `opacity_micromap_max_level` (default 10). The device's limits
  apply too.
- **Driver and RenderingDevice**: micromap create, build sizes, build, free; the triangle geometry's
  micromap fields; the pipeline flag; a graph command for the build. The build waits on and is waited on
  by all commands with a full memory barrier on each side (the documented fallback without
  synchronization2's micromap stage). Inputs at 256-byte addresses (VUID 07515).
- **Bake**: Valence's three passes, re-implemented on this fork's geometry rows and hit-group 0 alpha
  test (threshold 0.5, point filter). At most 16 setup passes a frame; a 64 MB state budget per micromap
  (every level drops by one past it).
- **Who carries it**:
  - a static surface traced on its own: a second BLAS of the surface with the micromap;
  - a grouped multi-surface BLAS: the group's key includes its members' micromaps, so a group with
    micromaps is its own variant;
  - MultiMesh, one TLAS instance per element: the surface's micromap BLAS;
  - MultiMesh, merged (small MultiMeshes under `multimesh_merged_blas_max_triangles`): not yet. Its BLAS is
    built inside a compute list, where the repeated index buffer cannot be uploaded; a follow-up.
- **Persistent instances**: an instance whose slot is clean is not walked, so it would never pick up a
  micromap that lands later. A landed or released micromap changes a version in the slots' global key, so
  every slot is written again once (bakes arrive in bursts of at most 16 a frame, so a few full writes while
  a scene's meshes bake, then none). Finer, per material slot, is a follow-up if that shows up in a profile.
- **Mipmap bias**: the any-hit reads the base level through the render buffers' sampler, whose bias also
  applies to an explicit level. A positive `texture_mipmap_bias` would move it to a coarser level, so the
  micromaps turn off with a warning then.
- **Lifetime**: an entry lives while its surface, texture and inputs are the same; it goes with its
  surface, when its inputs change, or after the MultiMesh cache's time to live unused (never while a
  persistent slot still traces a BLAS that carries it). A mesh group key carries its members' micromaps,
  checked member by member, so a group never reuses a BLAS built with other micromaps.

## Open-world fit

- One micromap per (mesh surface, alpha texture, UV transform): a forest of 100,000 trees from 20 tree
  meshes bakes 20 micromaps.
- Memory: 2 bits per micro-triangle; level 10 is 256 KB per distinct triangle at worst, so the UV
  deduplication and special indices matter. The 64 MB cap stops a pathological mesh.
- Bake cost is a one-time cost per new mesh-texture pair, spread at 16 a frame.
- Follow-ups worth doing later: merged MultiMeshes (grass under the merge cap), micromaps for mesh LOD levels (bake per LOD index buffer), deformed
  BLASes (skinned trees near the camera), 2-state on indirect rays as Indiana Jones and Valence do, and
  `alpha_scissor_threshold` support in the path tracer (then the bake reads it too).

## Validation plan

- Cloud: build, the setting off is an identity (no extension asked for), and lavapipe without the
  extension prints the warning and keeps the any-hit path.
- 4090: a generic foliage-card scene (no game content), raster and path-traced shots at 100 % crop with
  the setting on and off (they must match), GPU time on and off, and the micromap statistics in the
  verbose log (coverage, bytes).

## References

- Khronos, `VK_EXT_opacity_micromap` specification and the Vulkan tutorial course "Opacity Micromaps".
- NVIDIA, Opacity Micro-Map SDK (github.com/NVIDIA-RTX/OMM) and its integration guide.
- NVIDIA developer blog, *Path Tracing Optimizations in Indiana Jones: Opacity Micromaps and Compaction of
  Dynamic BLASs*.
- NVIDIA OptiX SDK, `optixMicromapIndexToBaseBarycentrics` (the bird curve's index-to-barycentrics).
- Arm, *Evaluate Opacity Micromaps for ray-traced game assets* (learning path).
- Microsoft, DirectX Raytracing 1.2 (opacity micromaps).
- [valence-engine-rtx](https://github.com/Rman887/valence-engine-rtx) D96, commits listed above.
