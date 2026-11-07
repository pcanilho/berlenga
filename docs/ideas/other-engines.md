# Other engines' ray tracing: what to take for this fork

Research note, 7 Nov 2026. Follows `witcher3-remaster.md`. Sources are public
talks and articles; items marked (unverified) come from summaries of slides I
could not read in full.

## Unreal Engine 5: MegaLights (SIGGRAPH Advances 2025)

Stochastic direct lighting for hundreds of shadowed lights at a fixed cost.

- About one shadow ray per pixel, at half resolution, upsampled.
- Epic tried ReSTIR and dropped it: reservoir resampling "adds a high constant
  cost" on consoles.
- Each 8x8 screen tile keeps a list of the lights that were *visible* (not
  shadowed) in last frame's samples. Lights are picked from that list by
  weighted reservoir sampling on estimated BRDF luminance. Lights missing from
  the list still get 20% of samples (50% after a history miss), drawn from the
  light grid, so newly visible lights are found.
- Shadows: screen trace first, then hardware RT on proxy meshes near the
  camera (~150 m), then a merged far-field TLAS (unverified).
- One denoiser for all lights, diffuse and specular separate, albedo
  demodulated.
- PS5, 1080p, 941 shadowed area lights on screen: 5.5 ms total (unverified).
- Limits they list: proxy self-shadowing, alpha-tested and animated geometry,
  lag when many lights matter at once.

## Assassin's Creed Shadows, Anvil (SIGGRAPH Advances 2025)

Ray traced diffuse and specular GI over a large open world.

- Screen march first; about half the pixels fall through to the BVH.
- Secondary hits are lit by sun and local lights plus a probe cache: five
  camera-following cascades of 16x16x8 probes, 2 to 32 m spacing, toroidal
  addressing, 1k probe updates per frame, 73 MB.
- The ray tracing scene is coarse: low LODs, small objects culled, no
  skinning. Instances are added and removed as they stream around the camera;
  the TLAS is rebuilt on the async queue with time-sliced culling by solid
  angle.
- Alpha-tested meshes get a per-mesh opacity, measured by rasterising their
  triangles into the alpha texture; that decides whether foliage can be traced
  as opaque.

## Doom: The Dark Ages, id Tech 8 (NVIDIA, June 2025)

Path tracing update over the shipped RT GI and reflections: multi-bounce sun,
moon and sky light, every emissive traced as a light ("even small lights"),
SHaRC radiance cache, shader execution reordering, DLSS RR replacing the
denoisers. No public talk yet on sampling or bounce count. The same recipe as
Cyberpunk and the Witcher remaster.

## Alan Wake 2, Northlight (Remedy 2023, Tom's Hardware 2026)

Path traced, with night and storm scenes close to ours. Foliage bends through
GPU skinning (~300k bones per frame in one level). Fog and particles use
moment-based OIT at three resolutions. Mega Geometry (patch 1.2.8) moved
existing meshes to cluster acceleration structures (up to 256 triangles per
cluster): on an RTX 4090, about 1 GB less VRAM and 13% faster at 4K. That is
our PC's GPU, and its driver exposes cluster AS (checked 6 Nov).

## Star Wars Outlaws, Snowdrop (NVIDIA, 2024)

RTXDI for shadowed direct light from many lights plus RTGI, in an open world.
No public numbers.

## Not researched further

- Star Engine: little public on ray tracing. Worth a look later only for
  planet-scale coordinates and object streaming.
- CryEngine: its software ray tracing (SVOGI, Neon Noir) predates what the
  fork already has.

## What this changes for the fork

| Item | Take |
|---|---|
| Many lights | Our grid picks lights by estimated power with no knowledge of walls, so lamps behind buildings waste samples. MegaLights' fix, a per-tile list of last frame's *visible* lights with a reserved share for the rest, is cheap and fits on top of the grid. Try it next to ReSTIR DI temporal reuse and keep whichever measures better on the street rig; both may stack. |
| Emissive surfaces | Doom traces every emissive as a light. Keeps the plan: emissives join the same sampling. |
| Indirect light | Doom (SHaRC) and AC Shadows (camera-following probe cascades) both end paths in a cache. A camera-following hash cache matches our camera grid. |
| Open-world BLAS/TLAS | AC Shadows: coarse RT LODs, instances streamed around the camera, async TLAS rebuild, solid-angle culling. Alan Wake 2: cluster AS saves ~1 GB and 13% on a 4090. Both back the planned cluster LOD and partitioned TLAS. |
| Foliage | AC Shadows' per-mesh opacity test (trace mostly-solid foliage as opaque) is a cheap step before opacity micromaps. |
| Denoiser | Everyone shipping PT on PC uses DLSS RR. No change. |

## Revised order after the pause

1. Light selection: visible-light tile list vs ReSTIR DI temporal reuse,
   measured on the street rig.
2. Emissive surfaces through the same selection.
3. Radiance cache that follows the camera.
4. DLSS 4.5 RR check; exposure for bounce-lifted dark scenes.

## Sources

- Narkowicz, Costa, MegaLights: Stochastic Direct Lighting, SIGGRAPH Advances 2025:
  https://www.advances.realtimerendering.com/s2025/content/MegaLights_Stochastic_Direct_Lighting_2025.pdf
- Raytracing the world of Assassin's Creed Shadows, SIGGRAPH Advances 2025:
  https://www.realtimerendering.com/advances/s2025/content/Advances%202025%20-%20Raytracing%20the%20world%20of%20Assassin's%20Creed%20Shadows.pdf
- NVIDIA, Doom: The Dark Ages path tracing update:
  https://www.nvidia.com/en-us/geforce/news/doom-the-dark-ages-path-tracing-dlss-ray-reconstruction-update/
- Remedy, How Northlight makes Alan Wake 2 shine:
  https://www.remedygames.com/article/how-northlight-makes-alan-wake-2-shine
- Tom's Hardware, Testing RTX Mega Geometry:
  https://www.tomshardware.com/pc-components/gpus/testing-nvidias-rtx-mega-geometry-tech-vram-reducing-tech-a-leap-forward-for-path-traced-rendering
- Wccftech, Star Wars Outlaws to support RTXDI and RTGI:
  https://wccftech.com/star-wars-outlaws-to-support-nvidia-rtx-direct-illumination-and-rtgi-on-pc/
