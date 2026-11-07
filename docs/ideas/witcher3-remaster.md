# The Witcher 3 Remastered (2026): what its path tracer suggests for this fork

Research note, 7 Nov 2026. Sources are public articles only; CD Projekt Red has
not published a technical talk on the remaster yet, so the renderer internals
below come from NVIDIA's announcement and press coverage, and from the
studio's documented Cyberpunk 2077 path tracer, which the remaster most likely
shares.

## What the remaster ships (PC)

- Path tracing replaces the 2022 mix of traced GI, AO, shadows and reflections
  with one system: indirect light, reflections, water refraction, and indoor
  light from candles, torches and effects (NVIDIA).
- DLSS 4.5: second-generation transformer Ray Reconstruction (on with path
  tracing), transformer Super Resolution, frame generation (new model on RTX
  40 and 50), dynamic multi frame generation up to 6x on RTX 50 (NVIDIA).
- Shader execution reordering on RTX 40 and 50 (NVIDIA).
- Hair: linear swept spheres on RTX 50 for the main character and NPC hair
  that used HairWorks. No fallback in the game; a mod enables NVIDIA's DOTS
  fallback (disjoint orthogonal triangle strips) on RTX 40 (PCGH).
- Skin subsurface scattering, a new tone mapper, updated HDR, a reworked
  shading model, denser foliage with reworked foliage shading (NVIDIA,
  GamingBolt).
- DirectX 12 only. Requirement for 4K path tracing at 60 FPS: RTX 5080 with
  upscaling and frame generation (CDPR table via Notebookcheck).

Not published: light sampling method, bounce count, foliage alpha handling
(opacity micromaps or not), Mega Geometry use, sky and volumetrics internals.

Reception worth noting: lighting reads warmer with softer shadows, and storm
scenes are less dark than before (GamingBolt). Path traced bounce light lifts
night and storm scenes unless exposure and the artists' light levels account
for it. Occasional pop-in at high travel speed.

## The studio's documented path tracer (Cyberpunk 2077 Overdrive)

NVIDIA's descriptions of it: ReSTIR DI (RTXDI) for direct light from every
light and emissive surface, reservoirs reused across neighbouring pixels and
frames, a spatial hash radiance cache (SHaRC) to end indirect paths early, then
the denoiser (NRD, later Ray Reconstruction). Exact settings in the remaster
are unconfirmed. The remaster's candles and torches lighting rooms points to the
same design.

## What this means for the fork

| Item | Where we are | Take |
|---|---|---|
| Many lights | Camera light grid + per-hit RIS, no reuse (#98) | Add ReSTIR DI reuse (temporal, then spatial) on top of the grid's candidates. Biggest noise win available for lamp-heavy night scenes and for emissive lights. |
| Emissive surfaces | Draft branch `claude/emissive-nee`, one sample per hit | Feed emissive triangles into the same ReSTIR reservoirs once reuse exists; until then keep the MIS design. |
| Indirect light | One path per pixel | A radiance cache (SHaRC) to end bounces early, or ReSTIR GI, after DI reuse. |
| Denoiser | DLSS RR via Streamline | Run the DLSS 4.5 RR model (driver override or updated Streamline); check guide buffers against its integration guide. |
| SER | `rendering/pathtracing/use_shader_execution_reordering`, default true | Nothing to do; keep it on for RTX 40. |
| Hair | Planned | DOTS strips on RTX 40 (plain triangles in our BLAS), LSS on RTX 50, one hair BCSDF. |
| Skin | Planned (SSS) | Confirms the pick. |
| Foliage | Opacity micromaps planned | Unconfirmed in the remaster; keep the pick, measure on a forest rig. |
| Tone mapping and exposure | Raster tonemapper over PT output | Path traced bounce lifts dark scenes: add an auto exposure that ignores bounce-only lift, or per-scene exposure targets, before night scenes are tuned. |
| Open world | Camera grid; cluster LOD and streaming planned | Pop-in at speed is the remaster's weak spot: stream geometry and BLAS ahead of camera velocity, not distance alone. |

## Suggested order after the pause

1. ReSTIR DI temporal and spatial reuse on the grid's candidates.
2. Finish emissive surfaces as lights, sampled through the same reservoirs.
3. DLSS 4.5 RR check.
4. Exposure handling for bounce-lifted dark scenes.

## Sources

- NVIDIA, The Witcher 3: Wild Hunt Remastered path tracing and DLSS 4.5:
  https://www.nvidia.com/en-us/geforce/news/witcher-3-wild-hunt-remastered-path-tracing-dlss-4-5-ray-reconstruction/
- GamingBolt, Remastered vs Next-Gen Update on PC:
  https://gamingbolt.com/the-witcher-3-wild-hunt-remastered-vs-next-gen-update-on-pc-raising-the-bar-on-a-classic
- PCGH, path traced hair on RTX 4000 via mod (DOTS):
  https://www.pcgameshardware.de/The-Witcher-3-Spiel-38488/News/Mod-ermoeglicht-Pathtraced-Hair-auf-RTX-4000-1555634/
- MIXED, linear swept spheres on RTX 50:
  https://mixed-news.com/en/witcher-3-remastered-linear-swept-spheres-rtx-50-nvidia-430-fps-6x/
- Notebookcheck, system requirements:
  https://www.notebookcheck.com/The-Witcher-3-Remastered-benoetigt-RTX-5080-fuer-4K-Pathtracing-mit-60-FPS.1410621.0.html
- Bitterli et al., Spatiotemporal Reservoir Resampling (ReSTIR DI), SIGGRAPH 2020;
  Ouyang et al., ReSTIR GI, HPG 2021; NVIDIA RTXDI and SHaRC (RTXGI) SDKs.
