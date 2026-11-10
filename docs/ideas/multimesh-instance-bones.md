# MultiMesh instance bones: skinned instances for wind

Research note, 10 Nov 2026, written before the code. Gap 2 of [vegetation.md](vegetation.md).

## The problem

Trees, bushes and reeds should bend in the wind through bones, because the path tracer sees
skinned geometry and Unreal 5.7 moved foliage wind to bones for the same reason. Godot skins one
`Skeleton3D` per `MeshInstance3D`: a compute pass writes a skinned copy of the vertices per
instance. A forest of 10,000 trees would need 10,000 nodes and 10,000 vertex copies. A MultiMesh
has no bones at all.

## How others do it

- **GPU Gems 3, ch. 2** (Dudash, Animated Crowd Rendering, 2007): instanced skinning. Every
  instance's bone matrices sit in a buffer; the vertex shader skins from its own block, indexed
  by the instance id. One draw call for the crowd, no per-instance vertex copies.
- **Unreal Engine 5.7**: Instanced Skinned Mesh components (PCG can spawn them); bone
  transforms come from "transform providers" on the GPU, an anim bank for baked clips or the
  Dynamic Wind plugin's "Wind Transform Provider", which moves bones "per bone instead of per
  vert". Nanite Skinning then deforms the instances.
- **Unity Entities Graphics**: compute or vertex-shader skinning from a per-entity bone buffer.

## What the engine already has

Godot's particle trails are instanced skinning with another name. A trail particle owns
`trail_steps` consecutive records (3x4 transform, colour, custom data); a mesh whose bone indices
name those records, drawn with a `particle_trails` material, is skinned per instance in the
vertex shader (up to four weights), in both the Forward+ and Mobile renderers. The path tracer
already captures and traces it.

## What we build

`MultiMesh.bones_per_instance` (`RenderingServer.multimesh_set_bones_per_instance`): with K > 1,
every K consecutive records are the bones of one drawn instance, and a material with
`use_particle_trails` (or `render_mode particle_trails`) skins the mesh's `ARRAY_BONES` and
`ARRAY_WEIGHTS` against them. Each bone record is the full transform of that bone in the
MultiMesh's space (placement times bone pose times inverse bind pose), the same convention as
trails.

- Records use the trail layout, so the MultiMesh needs `TRANSFORM_3D` with colours and custom
  data (5 vec4 per record).
- `instance_count` counts records; `instance_count / K` instances are drawn.
- Records can be written by a compute shader (the wind solver) and announced with
  `multimesh_mark_buffer_rd_changed()` (gpu-multimesh-records.md), or set from the CPU.
- The vertex shader reads the records at the MultiMesh's motion-vector offset, so CPU writes with
  TAA or DLSS read the current half.
- Path tracing: the MultiMesh is captured like a trail particle system and its merged BLAS is
  refit when the records change.

The wind solver itself (bone response to `WeatherMap3D`, Diener 2009 and Habel 2009) is stage 4
and writes these records.

## Results

Bench: `docs/bench/instance_bones` (fifteen three-bone stems; `--grid=N --animate` for cost).

- Match against `Skeleton3D` skinning of the same pose, mean pixel difference (0-255): raster
  0.0001, raster with TAA 0.05, path traced 0.013, path traced with a pose change mid-run 0.021.
  `--mode=rest` against the bent pose is 1.69, so the bend is real.
- Cost, 900 stems swaying every frame, raster, CPU renderer (lavapipe), mean per frame:

  | mode | render CPU | render GPU | script |
  |---|---|---|---|
  | unskinned MultiMesh | 0.28 ms | 97 ms | 0.01 ms |
  | instance bones | 0.30 ms | 143 ms | 3.2 ms (GDScript buffer writes) |
  | 900 Skeleton3D | 5.97 ms | 152 ms | 1.4 ms |

  Path traced, 100 stems, 1 spp: instance bones 0.59 ms CPU / 109 ms GPU, skeletons 2.18 ms /
  166 ms. Instance bones are cheaper than skeletons everywhere; the script cost goes away when the
  wind solver (stage 4) writes the records on the GPU. MultiMeshes with `bones_per_instance = 1`
  run the same shaders as before (the trail variant is only used with `use_particle_trails`).
- Fix found on the way: under path tracing the capture pass read a stale `flags_cache`, so trail
  particle systems (and instance bones) were captured with a trail length of 0 and only their
  first instance reached the path tracer. The flags are now refreshed before the capture.
