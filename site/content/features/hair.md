---
title: "Hair"
weight: 40
summary: "Fibre shading, strand grooms, TressFX strand simulation with body collision."
---

## Shading

- **Fibre shading** (BaseMaterial3D Hair section, `rendering/pathtracing/hair`): Chiang hair model under path tracing, melanin colour, on shells, cards and strands; mask for single-material characters.

  ![Hair-shaded spheres and cards: raster, path traced, sun behind, cards backlit](media/hair.jpg)

  *Fibre shading: fibre highlights and a bright rim against the sun.*

- **Raster hair shading** (`PhysXHair3D.hair_shading`): Marschner model as Unreal evaluates it, with multiple scattering and light through the strands in shadow maps, so backlit hair glows.

  ![Backlit strand hair in raster with Marschner shading](media/hair-raster-shading.jpg)

  *Raster hair shading: backlit, the strands glow warm.*

- **Deep opacity shadows** (`PhysXHair3D.deep_shadows`, on with `hair_shading`): raster strands shadow themselves in depth, calibrated against the path tracer.

  ![Strand hair with deep opacity shadows in raster, and path traced](media/hair-deep-shadows.jpg)

  *Deep opacity shadows: raster darkens the hair with depth like the path tracer; path tracing shows more fuzz at the edges.*

- **Custom hair shaders**: `HAIR`, `HAIR_ABSORPTION`, `HAIR_ROUGHNESS` outputs; `hair_strands` and `hair_cards` render modes; `IN_PATH_TRACER` built-in.

## Strands

- **Strand grooms** (`HairGroom`, `HairMesh`): grown on a scalp mesh or loaded from a HAIR file.
- **Moving strand hair** (`PhysXHair3D`): AMD TressFX's simulation on GPU compute, guide and follow strands, velocity shock propagation, wind with gusts and turbulence, collision spheres.

  ![Strand hair grooms: raster, path traced, and path traced with the sun behind](media/hair-strands.jpg)

  *Strand hair: per-fibre colour and a backlit rim.*

  ![Simulated strand hair blown by gusting wind, raster](media/hair-sim.gif)

  *Moving strand hair: the groom streams and flutters in gusting wind.*

- **Raster strands** (TressFX coverage): blended and sorted far to near on the GPU each frame; smooth without TAA or MSAA, and the hair shadows itself.

  ![Raster strand hair: older soft strands, then sharp sorted strands](media/hair-raster.jpg)

  *Raster strands: sorted and blended with no AA; each strand is drawn at its real width, so the groom stays sharp.*

- **Distance LOD** (`PhysXHair3D`: `lod_enabled`, `lod_start_distance`, `lod_end_distance`, `lod_strand_ratio`): far away, fewer and wider strands with the same look; simulation and path tracing keep every strand.

  ![Distant strand hair: all strands, then distance LOD](media/hair-lod.jpg)

  *Distance LOD: fewer, wider strands far away, with the same look.*

- **Motion vectors for strands**: TAA, FSR 2 and DLSS follow moving hair instead of smearing it; shaders get `PREVIOUS_FRAME` and `render_mode alpha_motion_vectors`.

  ![Moving strand hair: motion vectors before and after, then TAA before and after](media/hair-motion-vectors.jpg)

  *Motion vectors: before, only part of the hair reports its motion; after, every strand does.*

## Collision

- **Body collision** (`PhysXHair3D`: `collision_body`, `collision_cell_size`): strands collide with a body mesh, skinned or not, through a distance field built on the GPU every frame.

  ![Strand hair over rounded shoulders: spheres only, shoulders as the collision body, skinned to a tilted bone](media/hair-body-collision.jpg)

  *Body collision: with spheres only the hair sinks into the shoulders; with the shoulders as the body it drapes over them, also when skinned and tilted.*

- **Hair volume** (`volume_enabled`, `volume_friction`, `volume_stiffness`): strands push and drag on each other through a density grid, so hair keeps its volume.

  ![Strand hair shaking and in wind, hair volume off and on](media/hair-volume.jpg)

  *Hair volume: with it on, the hair stays full instead of collapsing into a flat sheet.*
