# Instanced forests in the path tracer

Follows [persistent-instances.md](persistent-instances.md). Persistent instance slots made a
single MeshInstance3D cost almost nothing per frame while it stays still: at 140k instances the
render CPU is 2.25 ms with slots on against 25.3 ms off, and the GPU takes 1.99 ms. Forests,
grass and rocks will mostly come as MultiMesh, though, and a large MultiMesh still takes the
per-frame path.

## What happens to a MultiMesh today

`RTTlasBuilder::_gather_mm_surfaces` classifies each MultiMesh surface:

- **Merged**: the whole MultiMesh fits under
  `rendering/pathtracing/multimesh_merged_blas_max_triangles` (65,536 triangles by default). One
  BLAS is baked over all records and there is one TLAS entry per surface. That is cheap, but the
  BLAS is rebaked when any record changes, and its bounds cover the whole MultiMesh.
- **Expanded** (`_add_expanded_mm_surface`): above the cap, every record becomes its own TLAS
  instance pointing at the surface's BLAS. Every frame the builder loops over all records. Each
  one rebuilds its world transform from the CPU mirror, compares it against the previous frame's
  record for motion, and pushes one TLAS instance and one geometry row.

Both kinds are `INSTANCE_MERGED` in `gather_instances`, so persistent slots never cover them. A
100k-tree MultiMesh therefore costs about what 100k MeshInstance3D nodes cost with slots off.
There is also no culling per record: `cull_max_distance` and `cull_min_size` act on whole
instances in `RendererSceneCull`, and a forest MultiMesh's bounds cover the whole forest.

## How others do it

- **Valence** (D94b, commit 1d8854cf02) gives an expanded MultiMesh surface one slot range, with
  one slot per record. Next to the range it keeps a key: the MultiMesh buffer generation, node
  transform, BLAS, geometry and material generations, count, SBT offset, flags, mask and
  `rt_origin`. When the key matches and nothing was rebuilt or moved, the walk skips the whole
  range: zero work per record. Any change rewrites the range. A new
  `MultiMesh::buffer_generation` counter, bumped by every buffer write, makes the key cheap.
- **Unreal Engine.** Hardware RT keeps one TLAS instance per instanced-mesh instance and leans
  on culling to bound the count. Ray tracing culling mode 3 combines distance and frustum. Ray
  tracing group ids cull an assembly, such as a tree's parts, as one unit.
  `r.RayTracing.Scene.CompactInstances` builds the TLAS from the active instances only. One
  open-world forest report (PCG-spawned assemblies) went from about 150k active instances and
  2.7 ms of instance gathering to about 18k instances and 0.03 ms through culling alone. The
  same report rejects merging instances into one BLAS: memory, streaming, culling and traversal
  all suffer.
- **RTX Mega Geometry foliage** (previewed GDC 2026, planned for an Unreal 5 title) uses
  partitioned TLAS to "instance and update large portions of the scene every frame" for millions
  of animated foliage elements. Public detail stops there. The extensions are NVIDIA's
  (`VK_NV_partitioned_acceleration_structure`). DXR 1.3 previewed cross-vendor partitioned TLAS
  on 28 Oct 2026.
- **Interactive Moana island** (Kim, 2025): about 36 million instances in a partitioned TLAS
  (4,194,304 slots, about 900 MB). Partitions of up to 1024 instances are built by a binned SAH
  sweep, and a GPU pass enables or disables a partition by its screen size. Freed slots go back
  through a GPU free list. Disabled instances still count toward a partition's maximum, so stale
  indices must be reused. Multi-level instancing helped little because most instances are unique
  pebbles and plants.

The common shape is that instances stay separate (no merging past a small cap), unchanged ones
cost nothing, and culling acts on groups rather than on single instances.

## Proposed design

### Step 1: MultiMesh records in persistent slots

When `persistent_instances` is on, an expanded MultiMesh surface keeps its TLAS entries in the
instance's slot, the same way a mesh instance's surfaces do (`_persistent_commit`). The slot is
kept while a key matches. Following Valence, the key holds:

- a new `MultiMesh::buffer_generation`, bumped by `multimesh_set_buffer`,
  `multimesh_instance_set_*`, a visible-count change and a mesh change,
- the node transform, `rt_origin`, BLAS, material, mask and flags (already in the instance's
  `rt_generation` and the global reset key).

An unchanged forest MultiMesh then costs one key check a frame instead of one walk per record.
Merged MultiMeshes get the same treatment for their one entry per surface.

Not kept, and staying on the per-frame path: particles, captured (vertex-animated)
MultiMeshes, camera-facing billboards and records with per-frame custom data. Wind in a vertex
shader is a capture, so animated grass stays per frame until clusters or partitioned TLAS take it
over.

A changed MultiMesh, for example one record moved by gameplay, rewrites its whole range, as in
Valence. Rewriting only the changed records needs per-record dirty bits in mesh storage, which
the MultiMesh API has no place for yet. It is left for later.

### Step 2: chunked forests and group culling

A forest should be many MultiMeshes, one per tile, not one per world. Then:

- `cull_max_distance` and `cull_min_size` already act per tile, which matches Unreal's group
  culling;
- a change touches one tile's range;
- the forest generator's tiling (mortal-plane `docs/ideas/forest-generator.md`) already produces
  this layout.

The engine side is documentation plus a test scene option (`--multimesh=N` tiles in the
open-world bench). It needs no new culling code.

### Step 3: partitioned TLAS (stage B)

Once records sit in slots, a partitioned TLAS can put each tile's range in a partition and
rebuild only touched partitions (Valence D94b uses 128 m columns of up to 8192 slots, plus a
global partition for movers). That is the existing stage B, and it gains more with forests than
it did with the 140k-instance bench, where it would save at most 0.3 ms of GPU.

## Measuring

Add `--multimesh=N` to `docs/bench/open_world`: the same rocks and trees placed as N MultiMeshes
(tiles) instead of nodes. Compare cpu_ms and gpu_ms against the node version at 100k records,
with slots on and off, still and `--fly`. Expectation: with step 1, still and fly fall to the
node version's slot cost. Without it, they stay near the slots-off numbers.

## Sources

- Valence D94b slot-resident instances, commit 1d8854cf02 (expanded MultiMesh range key).
- "Managing HWRT in Instance-Heavy Scenes", real-mrbeam.github.io, 20 Jan 2026.
- NVIDIA, "NVIDIA RTX innovations are powering the next era of game development", 10 Mar 2026.
- NVIDIA, "RTX Mega Geometry now available with new Vulkan samples", 6 Feb 2025
  (vk_partitioned_tlas).
- Microsoft DirectX blog, clustered geometry and partitioned TLAS preview, 28 Oct 2026.
- M. Kim, "Interactive Moana, part 2", 9 Nov 2025.
- Wald, "On fast construction of SAH-based bounding volume hierarchies", 2007.
