# Clouds: crisp crests and richer light (research, 19 Nov 2026)

The goal: cumulus with crisp, sunlit top crests, dark creases between billows, a bright silver lining
toward the sun and a soft glow inside, at the level the best current open-world skies show. Our clouds
already follow Nubis 2015/2017 and Frostbite (see [weather.md](weather.md), steps 3 to 3f). This note
names what the newer published work adds, compares it with our code, and lists build options.

## What is published

### The open-world sky that started the question
- **No technical talk or paper on its clouds exists yet.** What it shows (trailer observations): far
  more micro detail on the billows than the studio's previous game, stronger occlusion between billows,
  towering storm cells with lightning lit from inside, time-of-day lighting with long shadows and shafts.
- **The same studio's previous talk is the best evidence of the pipeline it grew from**: Bauer, "Creating
  the Atmospheric World of Red Dead Redemption 2", Advances SIGGRAPH 2019 (slides read):
  - 32 x 32 km cloud map (512^2, two layers) plus a 1x128 height LUT per cloud type; detail carved from
    the low frequency shape with one 3D Perlin-Worley read, displaced by two 2D noise reads (xy and xz).
  - Shading: single scattering plus two Wrenninge octaves; artist-weighted HG lobes, cloud g = 0.8, and
    a **back-scatter floor**: `phase = max(phase, 1/pi * rescale(min, max, extinction))`, because HG alone
    leaves the side away from the sun dead.
  - Ambient: the clear sky's average from a low resolution paraboloid, visibility reusing the sun ray's
    sample. Cloud shadow map 768^2 ESM with mips. A 2D **cirrus layer at 8 km** with the same shading.
  - Half resolution march, blue-noise start, reconstruction over 4 frames, dithered upscale with TAA.
- Observers of its second trailer describe the step from that pipeline as "more micro-detail, better
  occlusion, more complex patterns"; the studio has confirmed neither the technique nor ray tracing.

### Star Engine (Star Citizen)
- No talk on its planetary clouds is public. Its own monthly report (June 2022) only says the Gen12
  renderer ported "atmospheric and volumetric cloud rendering"; press on the CitizenCon 2953 demo cites
  multiple scattering and RT GI. Gas clouds in space are VDB volumes (CitizenCon 2948). Nothing to copy.

### Nubis Cubed (Schneider, Advances SIGGRAPH 2023; full deck read)
The most detailed public account of crisp, close-up cumulus. Relevant parts:
- **Dimensional profile**: a 0..1 field from the outside to the core of each cloud (an SDF of the
  cloud's envelope). It is eroded by detail noise to make the density, and it drives the lighting.
- **Up-rez detail noise**: one 128^3 RGBA texture: low and high frequency **"Curly-Alligator" wisps**
  (Alligator noise distorted by curl noise) and low and high frequency **billows** (inverted Worley).
  - `wispy = lerp(n.r, n.g, profile)`: high frequency at the thin edges, low frequency inside.
  - `billowy = lerp(n.b * 0.3, n.a * 0.3, pow(profile, 0.25))`: rounded at the edge, finer at the core.
  - `composite = lerp(wispy, billowy, type)`; density = `ValueErosion(profile, composite)`.
  - **Sharpening**: `density = pow(density, lerp(0.3, 0.6, density_scale))`, more in thin areas.
  - **"Twice folded" noise** for the last 150 m: `1 - pow(abs(abs(n.g*2-1)*2-1), 4)` (wisps) and
    `pow(abs(abs(n.a*2-1)*2-1), 2)` (billows): higher frequency without another texture read.
  - Mip by distance: `log2(1 + distance * scale)`, 15% faster with no visible loss. 0.5 m precision from
    an 8 m voxel profile.
- **Lighting = direct + ambient + secondary**:
  - Multiple scattering as a probability field: `ms = profile * exp(-tau_sun * k)`, with `k` relaxed
    from 0.25 to 0.05 toward the sun and deep inside: **dark edges and the inner glow**.
  - Ambient: `pow(1 - profile, 0.5) * exp(-summed_density_up)`: light only enters the outer shell.
  - Light march: the **first two samples** in the view march as before (fine detail at the surface),
    the rest from a 256x256x32 grid of summed density toward the sun, rebuilt over 8 frames (0.1 to
    0.2 ms): 40% faster and long shadows between clouds.
  - Lightning: `pow(1 - d/r, 12) * (1 - saturate(density * 5))`.
- Ray march: sphere tracing on a compressed SDF; 4.2 ms to 1.3 ms. Cost 10 ms at 960x540 on PS5 when
  flying through the voxel clouds (worst case), which is why the voxel volume itself is not for us.

### Phase function: Jendersie and d'Eon, "An Approximate Mie Scattering Function for Fog and Cloud
Rendering" (SIGGRAPH 2023 Talks, NVIDIA)
- `phase = (1 - w_D) HG(g_HG) + w_D Draine(alpha, g_D)`, all four fitted to the droplet diameter d
  (5 to 50 um; cumulus about 10 to 20 um). Matches 95% of Mie's forward energy, so the bright, narrow
  **silver lining** comes from the measured droplet size instead of a hand-set 0.8 lobe. Analytic
  sampling (useful for the path tracer). Does not model the glory or fogbow backscatter.
- Fits (Eq. 4 to 7, checked against the PDF):
  `g_HG = exp(-0.0990567 / (d - 1.67154))`, `g_D = exp(-2.20679 / (d + 3.91029) - 0.428934)`,
  `alpha = exp(3.62489 - 8.29288 / (d + 5.52825))`, `w_D = exp(-0.599085 / (d - 0.641583) - 0.665888)`.
  At d = 15 um: g_HG 0.993, g_D 0.579, alpha 25.1, w_D 0.493: half the light in a lobe under a degree
  wide (the lining right at the sun's edge), the other half in a broad Draine bulk.

### Also checked
- Hillaire, "Physically Based and Scalable Atmospheres in Unreal Engine" / PBS 2020: same octave
  multiple scattering as ours. UE5 VolumetricCloud leaves shape and light to the material.
- Recent papers (path graphs for media, arXiv 2404.11894; neural multiple scattering, arXiv 2401.14051):
  offline or trained per volume; not for a sky shader.

## Our clouds today (weather_clouds.cpp, weather_sky_material.cpp)

| Area | Now | What the crests need | Gap |
|---|---|---|---|
| Detail noise | 32^3, one channel, Worley fbm, tile = shape_scale / 8 (375 m), so about 12 m a texel | 128^3 RGBA wisps + billows, a few metres a texel at 2 to 5 km | **Large**: billows blur into a soft edge |
| Detail blend | wispy below, billowy above, by height; domain warp by itself | wisp/billow frequency by depth into the cloud (profile), plus by type | Medium |
| Sharpening | none | pow sharpening of thin density; folded noise near | Medium |
| Light march | 6 steps from 25 m doubling, shape only (no detail) | first samples a few metres, with detail, so each billow shades its neighbour | **Large**: crests and creases lit the same, so they read flat |
| Multiple scattering | three Wrenninge octaves + uniform two-stream diffuse on tau | add the profile-based field: dark edges, inner glow | **Large** |
| Ambient | sky colour by height in the column | outer shell only (`pow(1 - profile, 0.5)`), dimmed under cloud above | Medium |
| Phase | 0.7 HG(0.8) + 0.3 HG(-0.3) | HG + Draine at a droplet size; back-scatter floor | Medium: silver lining too broad and dim |
| High cloud | none | cirrus / altocumulus layer at 6 to 10 km | Missing (new weather field: contract needed) |
| Resolution | half res, 16-frame reprojection | same or 4-frame; check crest softness at 100% crop | Check |
| Long shadows | light march reaches 1.6 km (6.4 km at low sun) | voxel summed density or the shadow bake | Small (low sun only) |
| Path tracing | camera and bounce rays read the same sky shader | changes stay in the shared GLSL, so parity holds | Check per change |

The dimensional profile is the key missing piece, and we nearly have it: `shape` in `wc_extinction`
(the cover-remapped noise before detail erosion, 0 at the edge, 1 at the core) plays the same role
and costs nothing extra to hand to the lighting.

## Build options

1. **Crisp crests (shape)**: a 128^3 RGBA detail texture made once on the CPU like today's (Curly-
   Alligator wisps, inverted Worley billows, low and high frequency each); Nubis Cubed's composite over
   our `shape` as the profile, sharpening, folded noise near the camera, distance mips. Light march's
   first two samples short (about 4 m and 12 m) and with detail.
2. **Light that models depth (lighting)**: Nubis's multiple-scattering probability field and shell
   ambient from `shape`, HG + Draine phase at `cloud_droplet_size` (default 15 um), RDR2's back-scatter
   floor. Replaces the uniform two-stream diffuse, keeping an overcast deck's brightness.
3. **High cloud layer**: a 2D cirrus / altocumulus layer at 6 to 10 km, shaded by the same model, its
   cover from a new WeatherMap3D field (written contract; water + weather owner informed).

Each change is in the shared GLSL (sky march, radiance pass, path traced misses), on by default, and
checked with `docs/bench/sky` and `docs/bench/weather --view=clouds` as raster and path traced 100%
crops before and after, with the cost per frame.

## Sources
- Bauer, Creating the Atmospheric World of Red Dead Redemption 2: A Complete and Integrated Solution,
  Advances in Real-Time Rendering, SIGGRAPH 2019 (slides_public_release.pptx).
- Schneider, Nubis Cubed: Methods (and Madness) to Model and Render Immersive Real-Time Voxel-Based
  Clouds, Advances in Real-Time Rendering, SIGGRAPH 2023.
- Schneider, Nubis, Evolved, Advances 2022; Schneider 2015 and 2017 (see weather.md).
- Jendersie and d'Eon, An Approximate Mie Scattering Function for Fog and Cloud Rendering, SIGGRAPH 2023
  Talks: https://research.nvidia.com/labs/rtr/approximate-mie
- Wrenninge et al., Oz: The Great and Volumetric, SIGGRAPH 2013 Talks (octave multiple scattering).
- Hillaire, Physically Based Sky, Atmosphere and Cloud Rendering in Frostbite, PBS 2016; Physically Based
  and Scalable Atmospheres in Unreal Engine, EGSR 2020.
- Cloud Imperium, Star Citizen Monthly Report June 2022 (Gen12 cloud rendering port).

## As built (19 Nov 2026; Paulo picked all three)

- **Detail** (`weather_clouds.cpp`): `DETAIL_SIZE` 128, RGBA. r, g: Curly-Alligator wisps (Alligator noise as
  Side Effects' anoise, strongest feature minus the second; domain moved by curl noise, Bridson et al. 2007),
  low and high frequency; b, a: inverted Worley billows, low and high. Each channel equalised. Made once on the
  CPU (about 2 s on 4 cores). In `wc_extinction_p`: wisps blend low to high frequency toward the thin edge,
  billows from rounded at the edge to finer inside, wisps to billows with height and type; erosion times
  `cloud_detail_strength` (1.5); Nubis's sharpening `pow(shape, mix(cloud_edge_sharpness, 1, profile))`
  (0.45). The profile (shape before erosion) goes out to the lighting.
- **Light march** (`weather_sky_material.cpp`): two detailed samples toward the sun (0 to 16 m, 16 to 64 m),
  read at the billows' scale (64 m or four pixels' footprint: finer, the noise shading itself showed as grain
  on far cloud faces), then coarse steps doubling from 64 m. Behind 85% opacity the coarse march's first
  75 m stand in for them.
- **Lighting**: HG + Draine phase at `cloud_droplet_size` (15 um), hoisted out of the march (fixed per ray);
  floor `cloud_back_scatter` (0.5) / 4 pi away from the sun (Bauer 2019). Nubis's multiple scattering field:
  the two-stream diffuse times `mix(0.25, 1, smoothstep(0, 0.6, profile))` (dark edges), times
  `exp(-cloud_surface_occlusion * tau_near)` (0.6; billows shade the scattered light, crests lit, creases dim),
  absorbed more slowly toward the sun deep inside (inner glow). Ambient through the outer shell,
  `mix(0.35, 1, sqrt(1 - profile)) * 1.25`.
- **High layer**: `WeatherMap3D.high_cloud_cover` (0.25 base), `high_cloud_height` (8 km),
  `high_cloud_optical_depth` (1), `get_high_cloud_cover_at`, contract `WorldWeather.high_cloud_cover_at`
  (world-contracts.md, world-links.md). Driver: dry air thins it to 0.3 of the base, rain set on the map
  thickens it and eases four times faster than the rain (the sheet arrives first), each storm spreads an anvil
  shield 2.5 radii downwind along the wind aloft. Drifts with `get_wind_aloft` at its height; drawn as one
  read where the ray crosses the sheet (fibres: the wisp channels stretched 8:1 along the wind), ice phase
  HG g = 0.75 (Baum et al. 2005). Shades the ground only in the cloud shadow bake, by its own optical depth.

Checks (lavapipe, 960x540):
- Crests (`docs/bench/sky --sky --time=16 --pitch=14 --fov=22 --yaw=160 --reproject`, cover 0.35, type 0.75):
  billows now have defined rims and darker creases where before they were one soft tone.
- `docs/bench/weather --view=clouds`: raster and path traced differ by the same amount as before (mean 8/255
  at 24 frames each, before and after): the clouds are in the shared sky shader.
- Cost: frame times with the march on every pixel (sky bench, 12 frames, twice each) 601/567 ms before,
  587/588 after; clear sky 207 and 227. No measurable change on lavapipe; the GPU cost goes on the PC queue.

Open: the march's uniform steps (tens of metres through a cloud) leave the near shading noisy where the
occlusion is strong; Schneider 2015's adaptive refinement (step back and take short steps once inside cloud)
is the next step for crisper edges and would let `cloud_surface_occlusion` go back up to about 1.2, which
looked best in close views. The horizontal banding under the far cloud bases predates this work.
