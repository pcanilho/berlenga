# MultiMesh records written on the GPU, seen by the path tracer

Research note, 10 Nov 2026, written before the code. Gap 1 of [vegetation.md](vegetation.md).

## The problem

`RenderingServer.multimesh_get_buffer_rd_rid()` lets a compute shader write MultiMesh records
directly, which GPU placement and procedural grass need. Raster draws them. Everything else
reads the CPU side, which a GPU write never touches:

- **Expanded MultiMeshes** (above `multimesh_merged_blas_max_triangles`):
  `RTTlasBuilder::_add_expanded_mm_surface` builds one TLAS record per instance from the CPU
  mirror (`multimesh_get_local_data_ptr`). The mirror is filled once, by a stalling readback, and
  then never again, so rays keep seeing the records from that first readback (often all zeros).
- **Merged MultiMeshes** read the GPU buffer in their merge dispatch, but only rebake when
  `motion_vectors_last_change` moves, which only CPU writes do.
- **Persistent slots** (instanced-forests.md) are rewritten when `DEPENDENCY_CHANGED_MULTIMESH_DATA`
  fires, which only CPU writes send.
- **The AABB** is rebuilt from CPU data, so culling uses stale bounds unless a custom AABB is set.

## How others do it

- **Unreal Engine.** Instance data lives in GPU Scene; the ray tracing instance descriptors are
  written by a compute pass (`RayTracingInstanceBufferUtil`) from that data, so GPU-driven
  instances reach the TLAS without a CPU copy.
- **Unity.** `RayTracingAccelerationStructure.AddInstancesIndirect` takes a GPU buffer of instance
  matrices plus an indirect count; the CPU never sees the transforms.
- **Vulkan** reads TLAS instances from a device address, so they may be written by a shader;
  an instance with a null BLAS reference is inactive.

The general answer is a GPU pass that writes TLAS instances. In this fork the TLAS instance
buffer is assembled inside RenderingDevice (staging, keyed writes for persistent slots), so a GPU
write into a range of it needs RenderingDevice changes and belongs with partitioned TLAS (stage B).

## What we build now

The case vegetation has today is records that change when a tile streams in or is edited, not
every frame (moving plants move through bones, not records). For that, one explicit call is
enough:

`RenderingServer.multimesh_mark_buffer_rd_changed(multimesh)`, called after the compute writes:

1. Copies the written block over the second motion-vector half, so raster reads it whichever
   half is current, with no motion.
2. Moves the change stamp and sends `DEPENDENCY_CHANGED_MULTIMESH_DATA` at once: a merged
   MultiMesh rebakes from the GPU buffer the same frame.
3. Starts one asynchronous readback (`buffer_get_data_async`, no stall) into the CPU mirror. When
   it lands: the AABB is rebuilt (unless custom), the stamp and dependency move again, and
   expanded TLAS records and persistent slots follow, one or two frames late. Calls made while a
   readback is in flight coalesce into one more. While the first readback is pending the path
   tracer skips the MultiMesh instead of stalling.

## Found on the way

Merged MultiMeshes keyed their rebake on `motion_vectors_last_change`, which only moves while
motion vectors are on. Without TAA, DLSS or another motion-vector user, a small MultiMesh moved
from script (`multimesh_set_buffer`, `multimesh_instance_set_*`) kept its first records under path
tracing while raster moved. A separate `records_version`, bumped with every
`DEPENDENCY_CHANGED_MULTIMESH_DATA`, now drives it.

## Results (bench, CPU Vulkan, 640x360, 8 spp)

- `--mode=gpu`: records, AABBs and both MultiMeshes in the path traced frame match `--mode=cpu`
  (mean pixel difference 0.02 of 255, sampling noise), with and without `--taa`.
- `--mode=nomark`: AABBs stay empty, so both MultiMeshes are culled in raster and missing in the
  path tracer: the gap this closes.
- Before the `records_version` fix, `--mode=cpu` traced the boxes at their first position 40
  frames after they moved.

## Left for later

- Records that move every frame on the GPU in an expanded MultiMesh: the readback lags by a
  frame or two and costs bandwidth. The real fix is the GPU instance pass above, with
  partitioned TLAS.
- Indirect draw counts: rays see `visible_instances` records, not the command buffer count, so
  unused records must be collapsed to zero scale.

Bench: `docs/bench/gpu_multimesh` (`--mode=gpu|nomark|cpu`, `--test`).
