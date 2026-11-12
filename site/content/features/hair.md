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

- **Deep opacity shadows** (`PhysXHair3D.deep_shadows`, on with `hair_shading`): raster strands shadow themselves in depth, calibrated against the path tracer; omni and spot lights too (`deep_shadow_local_lights`).

  ![Strand hair with deep opacity shadows in raster, and path traced](media/hair-deep-shadows.jpg)

  *Deep opacity shadows: raster darkens the hair with depth like the path tracer; path tracing shows more fuzz at the edges.*

- **Hair shadows** (`cast_hair_shadows`, on by default): raster strand hair casts shadows on the face, body and ground.

  ![Strand hair over the ground: shadow from the head only, then from the strands, raster](media/hair-shadows.jpg)

  *Hair shadows: the shadow on the ground now takes the shape of the strands.*

- **Custom hair shaders**: `HAIR`, `HAIR_ABSORPTION`, `HAIR_ROUGHNESS` and `POSITIONAL_SHADOW_OFFSET` outputs; `hair_strands` and `hair_cards` render modes; `IN_PATH_TRACER` built-in.

## Strands

- **Strand grooms** (`HairGroom`, `HairMesh`): grown on a scalp mesh or loaded from a HAIR file.
- **Moving strand hair** (`PhysXHair3D`): AMD TressFX's simulation on GPU compute, guide and follow strands, velocity shock propagation, wind with gusts and turbulence, collision spheres.

  ![Strand hair grooms: raster, path traced, and path traced with the sun behind](media/hair-strands.jpg)

  *Strand hair: per-fibre colour and a backlit rim.*

  ![Simulated strand hair blown by gusting wind, raster](media/hair-sim.gif)

  *Moving strand hair: the groom streams and flutters in gusting wind.*

- **Swept sphere strands** (`rendering/pathtracing/linear_swept_spheres`, off by default): where the GPU traces swept spheres in hardware, strands are traced as round tubes, one primitive per segment, and shaded where the ray meets the fibre; other GPUs keep crossed strips. Not yet checked on supporting hardware.
- **Raster strands** (TressFX coverage): blended and sorted far to near on the GPU each frame; smooth without TAA or MSAA, and the hair shadows itself.

  ![Raster strand hair: older soft strands, then sharp sorted strands](media/hair-raster.jpg)

  *Raster strands: sorted and blended with no AA; each strand is drawn at its real width, so the groom stays sharp.*

- **Distance LOD** (`PhysXHair3D`: `lod_enabled`, `lod_start_distance`, `lod_end_distance`, `lod_strand_ratio`): far away, fewer and wider strands with the same look; simulation and path tracing keep every strand. Straight hair is drawn through fewer points with the same look (`simplify_tolerance`).

  ![Distant strand hair: all strands, then distance LOD](media/hair-lod.jpg)

  *Distance LOD: fewer, wider strands far away, with the same look.*

- **Hair cards for crowds** (`cards_enabled`, off by default; `card_screen_size`, `card_transition`): strand hair that gets small on screen cross-fades to textured cards baked from its own groom, simulating only its guides, so close-ups and zoomed cameras keep the strands; raster only, path tracing keeps the strands.
- **Motion vectors for strands**: TAA, FSR 2 and DLSS follow moving hair instead of smearing it; shaders get `PREVIOUS_FRAME` and `render_mode alpha_motion_vectors`.

  ![Moving strand hair: motion vectors before and after, then TAA before and after](media/hair-motion-vectors.jpg)

  *Motion vectors: before, only part of the hair reports its motion; after, every strand does.*

- **Wet hair** (`wetness`, `weather_wetting`, `wet_clumping`): strands soak in rain and dry over time, turning darker, glossier, heavier, flatter and clumped.

  ![The same hair in the same wind, dry and soaked by rain, raster](media/hair-wet.jpg)

  *Wet hair: soaked strands darken, clump and lie flatter in the same wind.*

- **Snow on hair** (`snow`, `snow_capacity`): snow from the weather settles on the top layer and windward side as small clumps, on strands and hair cards alike, and melts into wetness.

  ![Strand hair before and after snowfall, raster](media/hair-snow.jpg)

  *Snow on hair: flakes settle through the outer strands.*

- **Grey and sun-bleached hair** (`grey_ratio`, `sun_bleaching`, off by default): single grey strands mixed through the hair, and lengths lighter and warmer toward the tips.

  ![The same strand hair plain and with grey strands, raster](media/hair-grey.jpg)

  *Grey strands: single grey hairs through the brown.*

  ![The same strand hair plain and sun-bleached, raster](media/hair-sun-bleached.jpg)

  *Sun-bleached: the lengths lighten toward the tips.*

## Collision

- **Body collision** (`PhysXHair3D`: `collision_body`, `collision_cell_size`): strands collide with a body mesh, skinned or not, through a distance field built on the GPU every frame.

  ![Strand hair over rounded shoulders: spheres only, shoulders as the collision body, skinned to a tilted bone](media/hair-body-collision.jpg)

  *Body collision: with spheres only the hair sinks into the shoulders; with the shoulders as the body it drapes over them, also when skinned and tilted.*

- **Hair on cloth** (`collision_cloth`, `cloth_thickness`): strands rest on simulated cloth such as a collar or hood, lying over it as it moves.

  ![Strand hair over a cloth capelet: sinking through it, then resting on it, raster](media/hair-cloth.jpg)

  *Hair on cloth: before, the hair sinks through the capelet; after, it lies over it.*

- **Strand-strand collision** (`strand_collision`, `strand_collision_radius`, `strand_collision_friction`): guide strands no longer pass through each other, with friction.
- **Hair volume** (`volume_enabled`, `volume_friction`, `volume_stiffness`): strands push and drag on each other through a density grid, so hair keeps its volume.

  ![Strand hair shaking and in wind, hair volume off and on](media/hair-volume.jpg)

  *Hair volume: with it on, the hair stays full instead of collapsing into a flat sheet.*
