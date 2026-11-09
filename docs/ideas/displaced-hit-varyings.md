# Varyings of vertex-displaced surfaces at a path traced hit

Research note, 9 Nov 2026, for issue #169. Written before the fix.

## The problem

A shader whose `vertex()` moves `VERTEX` is traced from a capture: the raster pass runs
`vertex()` once per point and writes the moved positions and TBN, and the BLAS is built over
them. At a hit, the material still needs what `vertex()` hands the fragment stage (its
varyings, and `UV`, `UV2`, `COLOR` when it writes them), so the hit shader re-runs `vertex()`.
Until now it ran it on the hit point. That point is already moved, so `vertex()` moved it a
second time and every varying it derived from its input came out wrong: a rest position, a
world position before the offset, a triplanar coordinate, a wind phase read from the
position. The water surface hid its own case by inverting its displacement inside its
`#if defined(RT)` branch (four fixed-point steps), but every other displacing shader with a
varying was still off, by as much as it displaces.

## How other engines do it

- **Unreal Engine.** World Position Offset under hardware ray tracing is applied by a
  separate dynamic geometry pass that writes the moved vertices the BLAS is built from
  ("Evaluate World Position Offset" in the Ray Tracing settings of a primitive; Epic's ray
  tracing performance guide covers its cost). The material's hit shader does not see that
  buffer as its input: it loads the vertex factory's own streams for the three corners of the
  hit triangle (`LoadVertexFactoryInputForHWRT` in `RayTracingMaterialHitShaders.usf`) and
  builds the interpolants from them with the hit's barycentrics. So the deformed positions
  give the intersection, and the undeformed inputs give the shading.
- **Unity HDRP.** Hit shaders never run the vertex stage. They fetch the triangle's mesh
  attributes and interpolate them (`FetchIntersectionVertex`); vertex animation reaches the
  ray tracer only through skinned or dynamic geometry, and the path tracing limitations page
  lists vertex displacement among what it doesn't reproduce.
- **Vulkan.** `VK_KHR_ray_tracing_position_fetch` returns the positions the BLAS was built
  from, which is the displaced point again: it helps normals, not inputs.

The common rule: build the acceleration structure from the deformed result, shade from the
inputs the vertex stage started with. Raster gives `vertex()` its inputs and interpolates
its outputs; the hit should do the same thing.

## What we build

The capture keeps a second block, in the same layout as the displaced one (float3
positions, then packed TBN): `vertex()`'s inputs, `VERTEX`, `NORMAL`, `TANGENT`,
`BINORMAL`, taken to capture space by the transform raster applies to its outputs (so they
sit in the same space as the hit's object space, MultiMesh instance transform included).
The hit interpolates them with its barycentrics and runs `vertex()` on them; afterwards it
still restores the captured position and TBN, as before, so geometry and shading normals
come from the capture.

Cost and scope:

- The block is only stored when `vertex()` leaves something the hit keeps: a varying that
  reaches the fragment stage, or a write to `UV`, `UV2` or `COLOR`. A wind shader that only
  moves grass doubles nothing. Otherwise the capture doubles in size for that surface.
- Both trace paths keep it: single meshes (copied with the capture into the deformed
  buffer) and merged MultiMesh / particle surfaces (copied by the merge dispatch).
- `GeometryData` grows by 8 bytes (`rest_byte_offset` and the `FLAG_REST` bit).
- The water surface drops its inversion: its `vertex()` is the same under path tracing as in
  raster.

Left as it was:

- The hit evaluates `vertex()` once, at the interpolated inputs, where raster interpolates
  `vertex()`'s results from the three corners. The two agree for anything linear in the
  inputs (positions, offsets, most varyings) and differ only inside a triangle for strongly
  curved functions of the position. Unreal's three-corner evaluation would remove that at
  three times the cost per hit; not worth it until a shader shows the difference.
- For a MultiMesh, `vertex()` at a hit sees its input in the node's space (instance
  transform applied) and `MODEL_MATRIX` without the instance transform, so world positions
  agree with raster but a displacement along a mesh axis of a rotated instance doesn't. This
  predates the fix and is separate from it.

## Check

Bench `docs/bench/water_surface --view=beach`, path traced against raster at 100% crop: the
shore depth, swash and foam line sit where raster draws them, as they did with the inversion,
now without it.
