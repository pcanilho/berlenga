# Raster lighting for Flow smoke

Status: plan (15 Nov 2026). Under path tracing, Flow smoke is already lit by every light, shadows itself, and
casts shadows (raygen delta tracking plus ratio-tracked shadow rays). This note covers raster only.

## What raster does today

- **Sun.** Flow's own ray march colours the smoke from a colormap. The module multiplies that colormap by the
  first DirectionalLight3D's colour and energy, and Flow's shadow pass gives the smoke its self-shadowing from that
  light. The smoke does not receive the scene's sun shadows: a wall between the sun and the smoke does not darken
  it.
- **Ambient.** A grey floor, `shadow_min_intensity = luminance(ambient) / luminance(sun)`. It carries no colour, no
  sky, and no occlusion, so the middle of a dense plume is as bright as its edge.
- **Omni and spot lights.** These go through the module's scattering pass (`FlowScatterRD`), which marches the
  smoke with a Henyey-Greenstein phase, a short self-shadow march, and Godot's shadow atlas. It only runs when
  `light_scattering` is on, and that is off by default.
- **Shadows cast by the smoke.** None. The ground under a plume stays fully sunlit.

## How other engines do it

- **Frostbite** (Hillaire 2015, "Physically based and unified volumetric rendering"): participating media are voxelised into
  an extinction volume. Lights see the media through "volumetric shadow maps", which store transmittance from each
  light. Opaque surfaces and particles sample those maps, so media both receive and cast shadows. Ambient comes
  from the sky's SH, scaled by a cheap occlusion term.
- **Unreal** (Heterogeneous Volumes, 5.3+): a sparse voxel volume is ray marched per light. Shadows cast onto
  the scene use a "Beer shadow map" (front depth plus optical depth per texel), or adaptive volumetric shadow maps
  (Salvi et al. 2010) for several layers. Sky light enters as ambient from the sky's SH, with an optional
  occlusion march toward the sky.
- **HDRP** (Local Volumetric Fog, volumetric clouds): media are lit through the froxel fog with the
  directional, punctual and area lights and their shadow maps. Ambient is the sky's ambient probe (SH). Clouds use
  a shadow texture projected from the sun onto the ground.
- **Opacity shadow maps** (Kim and Neumann 2001) and **Fourier opacity mapping** (Jansen and Bavoil 2010):
  transmittance from a light as a function of depth, stored in a few 2D layers. They are the cheap version of
  the above when one light dominates.
- **Godot's own volumetric fog** (`volumetric_fog_process.glsl`) mixes `ambient_color` with the sky's radiance
  map (an isotropic tap up, plus an anisotropic tap along the scatter direction) by `sky_contribution`.

## Plan

Each step is a PhysXFlow3D property, off by default. Off keeps today's look, and defaults change only after a GPU
bench.

1. **Sky and ambient colour (`ambient_scattering`).** The scattering pass adds ambient light the way Godot's fog
   does: `ambient_color`, mixed with the sky's radiance map by `sky_contribution` and scaled by
   `ambient_light_energy`. A short march upward (sky visibility, as in Frostbite's and Unreal's ambient occlusion
   for volumes) darkens the inside of dense smoke. This replaces the grey floor.
2. **Sun in the scattering pass with scene shadows (`sun_scattering`).** The first directional light is marched
   like the others and samples Godot's directional shadow cascades, so walls shade the smoke. Flow's colormap then
   carries only the glow.
3. **Smoke shadows onto the scene (`cast_shadows`).** A Beer shadow map from the sun: one 2D texture over the
   grid's footprint, seen from the sun, holding front depth and optical depth. It is built by marching the sparse
   grid once per frame at a quarter of the grid's resolution. The forward shader multiplies the directional
   light by `exp(-optical_depth)` behind the front depth. Built: a 128² RGBA16F map (transmittance, front
   and back depth) fit to the grid's bounds along the sun, 48 steps per texel, read by the Forward+ opaque pass
   through the directional light data a frame later (`LightStorage::directional_volume_shadow_set`); the
   receiver interpolates linearly between front and back depth. Omni and spot lights come later through the same map
   type per light if it pays off.
4. **Local lights by default (`light_scattering`).** This is already built. It stays off until it has been
   benched on the PC.

Costs are measured in the cloud as compute pass counts and steps per pixel, and on the PC as GPU time once the
desktop is back. Every step is checked with raster and path-traced 100% crops of the showcase smoke shot.

## Next: edge banding and shadow map cost (16 Nov 2026)

- **Banding at the gas's edges (raster).** The scattering pass marches at a quarter of the resolution with a
  per-pixel interleaved gradient noise offset (Jimenez 2014) that is the same every frame, then upsamples with
  depth weights. The step pattern stays put and shows as stairs. The established fix is to vary the offset each
  frame (IGN with a per-frame offset, or blue noise; Jimenez 2014, Wronski 2014 for AC4's volumetric fog) and
  blend with a reprojected history (Hillaire 2015 for Frostbite's froxels; Godot's own volumetric fog keeps a
  temporal history too). The history is rejected where the reprojected scene depth or the gas's depth moves too
  far, and clamped to the current neighbourhood as TAA does (Karis 2014), so moving smoke does not smear.
- **Shadow map cost.** The Beer shadow map marches every texel's full column through the grid's box, in
  fixed steps, also through blocks that hold no gas. The view march already jumps over empty blocks through
  Flow's sparse table (as the path tracer does, Hofmann et al. 2024); the shadow pass should do the same, which
  keeps the result and cuts steps to the gas that is there. Measured in the cloud as steps per texel.
