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

## Round 2 (20 Nov 2026; Paulo picked everything listed)

1. **Adaptive refinement** (Schneider 2015; Hillaire 2016): long steps through clear air; on reaching cloud,
   one step back and steps of `cloud_refine` (0.33) of a long one while inside, long again after four empty
   short steps (iteration cap three times the step count). The grain on far faces went with it, so
   `cloud_surface_occlusion` is back up to 1.0.
2. **Sky light shaded from above**: one coarse read 100 m over the sample, ambient times
   `mix(0.4, 1, exp(-tau_up))` (Nubis Cubed scales ambient by the summed density toward the sky;
   SunshineClouds2 reads the density one step up). Undersides and the lee of towers darken.

SunshineClouds2 (MIT, code read 20 Nov): its punch comes from art terms on top of the same Schneider model:
transmittance raised to a "lighting sharpness" power, HG raised to a power, a powder term `pow(density, k)`,
a density-weighted adaptive step `mix(max, min, pow(density, 0.1))`, light steps spaced quadratically, a
painted coverage and tint mask. We take the ambient-from-above read; the sharpening powers are not
energy-conserving and would split raster from path traced bounces, so they stay out.

Cost (lavapipe, crest view, 40 reprojected frames): 800 ms a frame before round 1, 1084 after refinement: the
short steps inside cloud are the cost the next two items (sun shadow grid, empty-space skipping) pay back.

3. **Sun shadow grid** (Nubis Cubed's light voxel grid, Schneider 2023): `WeatherMap3D` bakes the optical
   depth toward the sun out of the layer for a 128 x 128 x 32 box (32 km across, cloud base to top) round the
   camera, from the top level down, one level a frame on worker threads: each voxel marches only to the next
   level's height (steps of at most 128 m) and adds that level's value where its path meets it (bilinear); off
   the box's side it marches the rest. Rebaked every four shadow intervals, sliding with the drift between.
   The sky's light march keeps its first 512 m (detail to 64 m, then 64/128/256 m shape steps) and reads the
   grid once 512 m along the path for the rest: a small cloud keeps its own shading (a first try that read the
   grid at the sample itself blurred it, 256 m texels), and shadows now reach right out of the layer, so
   at a low sun towers shade the clouds behind them over kilometres. `cloud_light_grid_enabled` switches it
   for A/B checks; outside the box the march is as before.
   Cost (lavapipe, `--view=clouds`, raster, frames 30 to 60): 836 ms a frame without, 750 to 773 with (the
   bake shares the same CPU there). Bake about 0.37 s of CPU in all, spread over 32 frames. PT parity: sky
   mean difference 4/255 on `clouds_wide`.

4. **Empty-space skipping: tried, not kept.** A 2D map over the grid's box of the distance from each 256 m
   column to the nearest column holding any cloud (16 points x 8 heights per column, grown by one, chamfer
   distance), leaped by the march in clear sky. No visible change, but at fair-weather cover (0.3) 54% of
   columns hold some cloud and only 7% stay clear after the safety margin (2% at the crest view's cover), so
   it saved 3 to 4%, within the bench's noise. The clear-sky steps were already cheap (the cover read and one
   shape read, then out); the frame's cloud cost is inside the clouds (short steps, each with its light
   samples). A 3D occupancy grid fares no better at these sizes. Not worth its code; the light grid stays the
   saving.

5. **Banding under the far cloud bases: fixed.** It was not the clouds (a prefiltered far-cloud density, tried
   first, left it unchanged and smeared the mid-distance clouds, so it was dropped). It was the sky's own far
   ground, past the scene, lit through the cloud shadow map from one texel per pixel: there neighbouring
   pixel rows meet the ground kilometres apart, so single 160 m texels of cloud shadow showed as dark
   horizontal streaks. The sky shadow map now has mipmaps and the ground read takes the mip of the pixel's
   footprint (t times the pixel's angle over the sine the ray meets the ground at). The air's shafts read as
   before. PT parity: sky mean difference 5/255 on `clouds_wide`.

6. **Altocumulus mid layer** (names and driver agreed with the weather owner): `WeatherMap3D.mid_cloud_cover`
   (0.15), `mid_cloud_height` (4500 m, slab middle), `mid_cloud_thickness` (400 m), `mid_cloud_optical_depth`
   (4, vertical at full cover), `mid_cloud_drift` (wind aloft at its height), `get_mid_cloud_cover_at`,
   `get_mid_cloud_optical_depth_at`; contract `WorldWeather.mid_cloud_cover_at`. Out of `cloud_cover`. Humid air
   thickens it, dry air thins it to 0.2, rain dialled in thickens it into an altostratus sheet (eases 2x faster
   than the rain, after the cirrus at 4x); it never starts rain. Storms add a band round their anvils. Shades the
   ground by its own optical depth in the shadow bake. Drawn as its own thin volume (as layered volumetric
   clouds in Flight Simulator and Unreal): 8 steps across the slab, cloudlets from the detail texture's billows
   (300 by 450 m cells, rows across the wind: Ludlam's cellular convection), rounded top and bottom, lit like
   the cumulus (octaves, two-stream diffuse, sky light) with two sun samples through the slab. Not yet: the mid
   sheet dimming the cumulus' sun beneath it.

## Round 3: single clouds, groups and cauliflower crests (20 Nov 2026)

Review of round 2's frames: puffs with flat, sheared sides; far rows of the same flat-bottomed pancake; little
grouping; crests read as soft lumps. Causes found, each fixed:

1. **One noise threshold over the whole sky** made every cloud the same kind of blob, packed evenly. Cumulus are
   placed as single clouds instead (`cells_at()` on the CPU, `wc_cells` on the GPU): a grid of
   `WeatherMap3D` cells (1200 m), at most one cloud a cell, jittered centre, kept where the local cover beats the
   cell's draw (the cover and its cluster noise vary, so some stretches hold many clouds and others few). Sizes
   skew small (Plank 1969's exponential spread); height follows width by type (humilis about half as tall as
   wide, congestus taller). Each cloud is an envelope: straight-sided body, domed top, plus three thermal bubbles
   off its axis (Neyret 1997's rising thermals). The envelope remaps the shape noise as Nubis's coverage does, so
   the noise still carves the edge. Decks (cover above about 0.7) keep the continuous noise.
2. **Presence tested against the cover at the sample** cut a cloud by a vertical wall wherever it crossed a change
   in cover (the far "blocks"). The test now fades over a margin and shrinks the cloud toward thinner cover.
3. **Crests as an erosion after the remap** only roughened the skin of one round dome. Turrets now cut into the
   envelope itself, from billows in the cloud's own frame (about four across it, finer octaves inside), fully over
   its top half and less down the sides, so the outline becomes a cluster of rounded crests and the walls bulge.
   `cloud_turret_strength` (0.9). The world-scale billows add smaller lobes on top.
4. **The fine billow erosion bored pits**: it eroded by the inverted Worley (1 at the feature points), which
   removes each billow's centre and leaves a cracked cotton surface. It now erodes by one minus that, so the
   feature points stay as bulges (as Schneider 2015 flips the noise with height).
5. **Shear** default 0.4 (was 1): towers lean a little instead of smearing sideways.

Cost on lavapipe the same or lower (clouds view 1078 to 1058 ms, towers 1170 to 1006, fair weather 976 to
785): fewer, more compact clouds. Turrets are detail level, left out of the CPU shadow bake like the other
erosion (bake against a 10 m march still 0.002). Shadow share of the ground by cover 0.2/0.4/0.6/0.8/1.0:
4/9/26/72/94% (was 0/10/29/71/94).

PhysX Flow was considered for the shapes: it simulates a grid tens of metres across, so a sky of clouds
kilometres wide would need either a coarse grid with no crests or far more memory than a frame has; the
procedural model above is what every shipped sky uses. Not yet: the mid sheet dimming the cumulus' sun.

Round 3 follow-ups (docs review of 118c13bc):

6. **Flat bases**: the lowest 120 m of a cumulus read the shape noise (and its turrets) at that height, so the
   outline drops straight to the condensation level instead of rounding up underneath; no side turrets in the
   lowest quarter. Clouds high in a frame from the ground are near ones, seen from below: their bases are on
   the same level, higher in the frame by perspective.
7. **Sky light: the hemisphere, not the zenith.** The cloud's sky light was the zenith radiance, the darkest
   part of the sky; a cumulus in another's shadow at a low sun came out near black, darker than the sky behind
   it. Now the zenith (0.4) plus two reads 30 degrees up (0.3 each), one 30 degrees round from the sun's
   azimuth (its glow, not its aureole) and one opposite, in both atmosphere models. Shadowed clouds read grey,
   about the sky's brightness.
8. **Bench**: the weather bench's ground was 12 km across, so the wide view from 520 m saw past its edge (a grey
   band under the horizon). Now 60 km.

## Round 2 leftovers (20 Nov 2026)

1. **22 and 46 degree ice halos** (Tape 1994, "Atmospheric Halos"; Greenler): randomly oriented hexagonal
   columns refract through 60 degree faces with a minimum deviation near 22 degrees (21.7 red to 22.4 blue) and
   through 90 degree faces near 46. The cirrus phase gets one lobe a ring, per channel: nothing inside the
   minimum (a sharp red inner rim), an exponential tail outward, normalised over the sphere. The colours overlap
   through the tail, so the ring is pale, not a rainbow. `cloud_halo_strength` (0.08) is the 22 degree ring's
   share of the single scattering, the 46 degree ring a fifth of it. Shown only where a slow noise (25 km
   patches) says the sheet's ice is columns, so rings show as arcs over some of the sky. Raster and PT match.
   Bench: weather `--view=shafts --cover=0.05 --high-cover=0.7 --sun-yaw=0`.
2. **The mid sheet dims the cumulus' sun**: read once a pixel where the view ray crosses the middle of the
   cumulus layer, followed up the sun to the sheet, two-stream transmission as in the ground's shadow bake. Under
   a full altocumulus sheet far cumulus lose about 6% of their brightness (the sheet is thin, optical depth 4 at
   full cover, and most light comes through scattered).
