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

## Round 4 (20 Nov 2026; Paulo: all three)

1. **Light bounced between clouds** (cloud-to-cloud interreflection). The sun light grid's bake also stores how
   much of each 256 m texel is sunlit cloud (its opacity across a texel times the two-stream sun reaching it).
   Building the texture blurs that at two reaches, 1.3 and 3.3 km (a cone's footprint widening with distance, as
   voxel cone tracing reads coarser mips further out; Crassin et al. 2011), leaving out the texel itself, and
   stores it as the grid's green channel. The march adds sun x 0.7 x that share as diffuse light, entering the
   shell as the sky's light does: the shaded side of a tower facing a sunlit neighbour warms. `cloud_bounce`
   (1.0). Frame cost unchanged within noise (one more read of a texture already bound); the bake adds one
   extinction read a texel and two box blurs. Raster and PT match.

2. **Cumulus that grow and dissolve.** Each cell keeps its own clock (a cycle per cloud lifetime, give or take a
   fifth, offset per cell); each cycle draws a new cloud from the cell (position, size, presence). A cloud is
   born thin, widens over the first fifth of its life and rises over the first third as its thermals climb,
   lives, then over its last quarter dissolves from the edges in, its billows turning to wisps as it mixes with
   dry air (fair-weather cumulus last some 10 to 40 minutes; Plank 1969). CPU (shadows) and GPU agree; frozen
   clouds sit in mid life as before. While evolving, more cells hold a cloud (2.9 times the cover, 2.2 frozen) so
   the cover's share of sky holds (shadow share by cover 0.2 to 1.0: 4/9/25/71/94%). Raster and PT match.
   Bench: weather `--cloud-time=S` strips.

3. **Sunshine Clouds 2 compared** (Bonkahe, MIT, v2.7, a Godot compositor compute effect; source read). What it
   does and what we take:
   - Shape: Perlin-Worley coverage remap with a painted height gradient, curl-noise domain advection iterated
     three times (stronger low in the cloud), and a wind sweep that drags the lowest part downwind. We already
     warp by the detail's own billows and curl; the base sweep runs against the shear (bases lag upwind in
     reality), so not taken.
   - Lighting: Beer's law with an exponentiated HG, a powder term pow(density, k) and a "sun up" weight that
     treats the air above a height as cloud when the sun is low. Stylisation; Nubis Cubed's multiple-scattering
     probability field (which we use) replaced powder for the same dark edges. Not taken.
   - Ambient: one density read above the sample (as ours since round 2, credited there).
   - Worth taking, both new capabilities, not look tweaks: **point lights in the clouds** (each point light marched
     three steps through the cloud toward it, HG phase, falloff to a radius) for fires, city glow and flares
     under a deck at night; and **density effectors** (spheres that add or remove density with a falloff) for
     smoke plumes feeding a cloud or a blast punching a hole. Both cross systems (fire, lightning, the weather
     map's contract), so they go to Paulo as a choice first.

4. **Point lights and density effectors in the clouds** (Paulo picked both; names agreed with the weather owner;
   both after Sunshine Clouds 2, Bonkahe 2025, MIT).
   - `WeatherMap3D.add_cloud_light(position, color, energy, reach)` (and `set_`/`remove_`, `get_cloud_lights()`),
     at most eight, -1 when full. The march adds, per light in reach, three extinction reads toward it over at
     most 600 m, inverse-square falloff times a smooth window to the reach (Karis 2013), HG 0.6 through the thin
     part plus the two-stream diffuse rest, as the sun. Energy is in the sun's units times square metres. Lights
     the clouds only: a scene adds its own OmniLight3D for the ground. The sky now marches clouds at night when
     any cloud light is present (it skipped the clouds with no sun before).
   - Fires light their clouds by themselves: each WorldFire plume registers a light 30 m above its base, colour
     of a flame (1, 0.62, 0.32), energy a third of its heat release as light spread over the sphere. Fires and a
     scene's lights share the eight slots by brightness.
   - Lightning stays on its own full-resolution glow: a flash lasts a few frames and the half-resolution,
     reprojected march would smear it. Flashes are listed in `get_cloud_lights()` (`lightning` true) so readers
     see every light in the clouds in one place.
   - `add_cloud_effector(position, radius, strength)` (and `set_`/`remove_`, `get_cloud_effector_at`, contract
     `WorldWeather.cloud_effector_at`), at most eight. Strength -1..1, linear falloff to the radius: positive adds
     to the cloud's envelope, negative scales the density down. Effectors ride the cumulus drift. CPU (shadow bake,
     `get_cloud_density_at`) and GPU agree. Fires do not add effectors: their clouds already grow from the plume
     (fire clouds, 19 Nov).
   - Bench: weather `--cloud-light=x,y,z,E,R[,r,g,b]`, `--cloud-effector=x,y,z,R,S`, `--sun-energy=0`; checks in
     `--test=clouds` (hole clears the thickest cloud to 0, an adding effector makes cloud in clear air, slots full
     at eight). Raster and PT match.

## Round 5: lenticulars, wave clouds (20 Nov 2026; Paulo: all extra kinds, every one driven by the weather)

Research. Lenticulars (altocumulus standing lenticularis, and their stratocumulus and cirrocumulus kin) are the
condensed crests of mountain waves: stable air crossing a ridge oscillates at the buoyancy frequency, the waves
stand still over the ground, and where a moist layer is lifted past saturation it condenses into a smooth lens
that air flows through. No published game technique models them physically (Vimont et al. 2020 simulate
orographic cloud on a coarse grid offline; flight simulators place them from weather data), so we build from the
meteorology:
- Linear mountain wave theory (Smith 1980, Tellus 32): the terrain's 2D FFT times exp(i m z), non-hydrostatic
  m^2 = (k^2 + l^2)(N^2 - s^2) / s^2 with s = U k + V l; m takes the sign of s (energy upward), imaginary where
  waves cannot propagate (they decay). Checked against Queney's analytic ridge (1948): lift +236 m upwind,
  -270 m downwind at a quarter vertical wavelength for 250 m expected.
- Flow blocking: only the top U / N of the terrain lifts the air (dividing streamline, Sheppard 1956; Smith 1989).
- Saturation: lifted air cools 9.8 K/km and its dew point 1.8 K/km, so it saturates after 125 m per kelvin of
  dew point depression; past that the moist adiabat condenses about 2 g/kg per km near 0 C (6% less per kelvin
  colder); extinction 1.5 LWC / (rho r_e), droplets 10 um, ice 30 um below -25 C (Stephens 1978).
- Humidity aloft: Manabe and Wetherald's (1967) RH profile from the ground humidity, moistened within the mid and
  high layers: saturated in their clouds, 0.93 at their edges, drier further out, with thin sheets 350 m apart in
  the mid layer (the stepped humidity that stacks plates).

As built:
- `WeatherMountainWaves` (weather_mountain_waves.cpp): a 51 km square at 400 m round the camera, its heights
  sampled through the terrain-wind terrain source over two ticks, derived again when the wind aloft turns 5
  degrees or changes a tenth (a few ms, main thread). Displacement at 2, 3.5, 5 and 7 km, one RGBA float texture.
- The sky marches two thin slabs (round the mid layer and round the high layer) only where the field's largest
  lift can reach saturation (`get_wave_cloud_slab`, refreshed twice a second); none when the air is dry or calm,
  so it costs nothing then. 16 coarse steps over at most 8 km of path; where the lift comes within 150 m of
  saturation, four fine samples (the moisture read once a step). Lit as the mid layer. Not jittered: a jittered
  thin edge broke into dots.
- CPU: `get_wave_cloud_density_at`; the cloud shadow bake marches both slabs (8 steps each), so lenses shade
  the ground.
- Wind: `get_mountain_wave_velocity_at` (w = U . grad eta) joins `get_wind_at` above the boundary layer, blended
  over 300 m (Valence's rule); linear theory has no rotors, so no lee turbulence.
- Connected: terrain (heights), wind aloft (U), temperature (N, condensate, ice), humidity and the mid/high
  layers' moisture (driven by humidity, rain and storms), time of day through all of these.
- Contract: `WorldWeather.mountain_wave_at`, `mountain_wave_velocity_at`, `wave_cloud_density_at`;
  `WeatherMap3D.wave_clouds` (on).
- Cost (lavapipe raster, lenticular view): 566 to 595 ms against 529 ms with wave clouds off, about 10% while
  lenses are on screen; zero with empty slabs. Raster and PT match.
- Bench: weather `--view=lenticular` (a Witch of Agnesi ridge 2 km high; a test stand-in, not for docs frames),
  `--no-wave-clouds`. It prints the slabs, a top-down map, the wave wind aloft against the ground, and a dry run.
- Shading (later 20 Nov): the sun read through the whole lens (to 700 m) and the sky light shaded by the cloud
  above the point, so crowns are lit and bellies darker. Next: stacked plates showing more clearly, iridescence near the
  sun; then a docs frame over real terrain from the showcase scene.

## Round 6: stratus and stratocumulus from the weather (20 Nov 2026)

Research: the boundary layer's cloud is layered while the air is stable and convective once the ground heats
(Stull 1988, 11.2): morning stratus burns off into cumulus, evening cumulus spreads into stratocumulus. Wind or
drier air mixes a stratus layer into stratocumulus (Wood 2012, Mon. Wea. Rev. 140). Stratocumulus is closed cells
1.5 to 3 km across a 300 to 500 m deep layer, thick in the middle and thin at the sinking rim, lined into rolls
along a mixing wind (Atkinson and Zhang 1996). The base sits at the lifting condensation level, 125 m per kelvin
of dew point depression (Espy), so it rises through the day as the air dries.

- Type from the weather (`WeatherMap3D.cloud_regime_from_weather`, on): heating H = the day's warming curve times
  (1 - 0.7 cover), 1 with no clock. Stable type = 0.05 (stratus) in calm humid air, 0.22 (stratocumulus) once the
  wind passes about 6 m/s or the air dries below 0.9. Convection (H, cut by rain) blends to `cloud_type`, the
  afternoon's own type. While it rains the layer stays nimbostratus. `get_convective_heating`.
- Base: `cloud_base_height` plus the condensation level now minus that of the default air (15 C, 0.7), never under
  the ground below the camera plus 30 m (a base on the ground is fog, the mist's job). `get_cloud_base_now`;
  contract `WorldWeather.cloud_base_at` (default 1500).
- Shape: below type 0.36 the single clouds give way to one sheet (`layered_share`), whose noise is lifted so an
  overcast layer closes; it reaches down to a broken sky (cover 0.15 to 0.45). Stratocumulus modulates the sheet
  by Worley cells (F2 - F1) in the wind frame, 1800 m, stretched up to 3x into rolls by the wind. Cumulus
  (type above 0.36) is unchanged.
- Bench: weather `--test=clouds` prints the type, heating and base over a day for calm, windy, dry and rainy air;
  `--view=clouds --type=0.05|0.2 --wind=8`. Raster and PT match (640 px).
- Cells as heaps (later 20 Nov; docs declined straight extruded rolls): each cell's top highest in its middle and
  lower toward the rim (top at 0.25 to 1 of the layer by the cell value, roughened by the shape noise), so a roll is
  a row of rounded lumps with thin darker gaps; rolls stretched at most 2x.
- Air under a deck: the light the deck transmits diffusely (two-stream, minus the direct beam) now lights the air
  below it as a source (F / 2 pi, isotropic). Before, only the air's own multiple scattering lit it, so the
  horizon under an overcast was a dark band below both the deck and the ground.
- Cell shading (docs: even grey discs): cell middles dense (the remap tripled, liquid water 0.2 to 0.5 g/m3) with a
  soft thin rim, and the base sags under each middle (rims up to 15% of the layer higher), so cells show darker
  bases and centres, bright rims and lumps along the underside.
- Next: cumulus base detail (per cloud sag, scud under humid bases).

## Round 7: cirrocumulus and cirrostratus (20 Nov 2026)

Research: cirrocumulus is the cirrus layer broken into grains and ripples under one degree across, by shallow
instability or Kelvin-Helmholtz billows where the shear across the layer overcomes its stability (Ludlam 1967;
WMO International Cloud Atlas, Cc stratiformis/undulatus); the ripples lie across the shear. Cirrostratus is the
extensive, nearly featureless veil of a warm front's shield or an anvil, and the cloud of halos (Atlas, Cs
nebulosus/fibratus; Tape 1994).

- Cirrocumulus share (`WeatherMap3D.get_cirrocumulus_share`): gradient Richardson number Ri = N^2 / S^2 between
  the mid and high layers, N from the 6.5 K/km lapse at the layer's temperature, S from `get_wind_aloft` (so from
  `wind_shear_aloft`); ramp 1 - smoothstep(2, 20, Ri). Default shear 1.5 m/s/km gives none; 5 and up most.
- Sky (march_cirrus): in patches (14 km noise) of the sheet where it is not a veil, the fibres give way by that
  share to grains, the detail noise's billows broken by its finer octave, 1.8 by 2.7 km per 128 texels (cells
  about 200 m, ripples across the wind), soft edges.
- Cirrostratus: where the sheet's cover passes 0.55 to 0.9 the fibres merge into a veil (0.7 + 0.3 of the fibre),
  and halos there run 1.4x (0.7x on bare cirrus).
- Connected: humidity and coming rain (the high cover's driver) make the veil; the jet's shear makes the grains.
- Raster and PT match (640). Raster shows the half-res reconstruction's stepped edge on fine grains (the known
  dotted rim issue, on the improvements card); PT is smooth.
- Bench: weather `--view=clouds_up` (50 degrees up), `--shear-aloft=S`, `--high-cover=C`.
- No lattice (docs: one regular diamond lattice, the texture tiling every 1.8 km): two reads 1.37x apart in scale
  and 23 degrees apart, blended by a 6 km noise; both domain-warped by the texture's coarse octave (rows bend, part,
  merge); the grain threshold wanders on 2.5 km, so ripples break into gaps and run together.
- Halo check: the 22 and 46 degree rings show through the veil toward the sun (raster and PT). Sun-facing frame:
  weather `--view=shafts --cover=0.05 --high-cover=1.0` (sun 20 degrees up, 13 left of the view).

## Round 8: storm details, rain shafts and shelf clouds (20 Nov 2026)

Research: rain's visible extinction from its rate, sigma = 0.31 R^0.67 per km (Atlas 1953, visibility in rain);
heavy rain falls in shafts a few km across that slant downwind as the drops fall at about 7 m/s. A shelf cloud
(arcus) forms on a storm's gust front where its cold outflow lifts the warm moist inflow ahead of it to
saturation below the storm's base: a smooth, low wedge along the leading edge (Doswell 1985; WMO International
Cloud Atlas, arcus). Mammatus (next): pouches under an anvil from sublimation and evaporation into drier air
below it (Schultz et al. 2006, Mon. Wea. Rev. 134).

- Rain shafts (sky, `march_rain_shafts`): under each storm's base, the storm's rain (its peak rate times growth,
  `State.storm_rain`) over a core with an edge, exp(-(r/R)^4), broken into curtains by two octaves of noise (900
  and 260 m), slanted by the low wind over 7 m/s (`rain_slant`). Lit by the sky's light under the clouds (a third
  under the storm's shield) and the sun where the cover toward it is thin (HG 0.6). 14 steps over the chord,
  composited with the cloud layers by distance. Cost on lavapipe within noise (558 to 601 ms a frame).
- Shelf cloud (`shelf_drop`, CPU and GLSL): the base drops up to 450 m (`SHELF_DROP`, scaled by outflow / 15 m/s and
  growth) along the gust front, which runs ahead of the core by 1.1 radii plus 0.04 per m/s of outflow on the side
  the storm moves (`State.storm_dir`, the wind that steers storms); sharp at the front, rising back over 1.2 x 0.6
  radii; there the cover is raised to one continuous sheet and the type taken to stratus, so it reads as a smooth
  wedge. `WeatherMap3D.get_cloud_base_at` (and `WorldWeather.cloud_base_at`) include it.
- Connected: storm rain rate, outflow, the steering wind, the low wind, the sun.
- Raster and PT match (640). Near the camera the rain is the haze's and RainFall3D's; distant shafts draw against
  the sky only (terrain in front of a shaft hides it, as it should; a shaft in front of far terrain is not drawn yet).
- Bench: weather `--storm=x,z,R,mm,outflow` (grown 15 min, the storm moving about 9 km), `--wind-dir=x,z`.
- Review fixes (later 20 Nov; docs: a doughnut shelf, no storm above it, even shafts):
  - Cumulonimbus body (`storm_body_at` / `wc_storm_body`): one tower about 1.5 core radii across from the base to
    the storm's height (top_height at type 1 times growth, 7.5 km over the base by default), leaning downwind,
    spreading from 78% of its height into an anvil about 3 core radii across centred 1.6 radii downwind, flat under
    the tropopause (Houze 1993, 8). Solid through (the lifted noise times the envelope), outline the noise's. Single
    cells fade out within 1.8 body radii (the updraft's subsidence keeps other towers away). Under the anvil's
    footprint the column's type is the storm's, so its height is not cut.
  - Shelf as a solid wedge: from the lowered base to 400 m above the storm's base, smooth underneath, its upper face
    the noise's; single cells give way there. Arc narrowed to about +-55 degrees of the storm's motion.
  - Shafts: three curtain octaves (1.2 km, 400 m, 130 m) thresholded so lighter gaps open between heavier sheets;
    24 steps.
  - Mammatus (`march_mammatus`): lobes hang up to 500 m under the body's anvil (from 78% of the storm's height) on a
    jittered 1.6 km grid, where the anvil overhangs (away from the tower) and the more the drier the air (the
    ground humidity, as the sub-anvil air's proxy); 16 steps over a 700 m slab; lit by the light under the anvil and
    the sun through their skin. Composited with the low march by distance. Subtle under a high sun; they show best
    with a low sun (a sunset frame).
  - Cost unchanged on lavapipe (538 ms a frame with a storm in view). Raster and PT match. The raster speckle on a
    distant sunlit tower face is the half-res reconstruction (improvements card).
- Second review (docs, 20 Nov evening: a heap of loose puffs, a jellyfish anvil, no mammatus, cirrus like motion
  blur). Research: Schneider, "Nubis: Authoring Real-Time Volumetric Cloudscapes" (Advances 2015) and "Real-Time
  Volumetric Superstorms" (GDC 2022) model a storm as one envelope with its own density and only the edge eroded;
  photos and the Atlas: an anvil is a thin ice sheet with a flat ceiling, a sharp upwind edge, spreading far
  downwind; ice anvils are optically about 10 deep near the core and 1 or 2 downstream (Lilly 1988; Garrett 2005).
  - Tower: narrower at the base (0.75 core radii) widening to 1.1 by 60% of its height, a hard outline (the last
    tenth of the radius), shaped by the shape noise for its bulges plus a read 1.6 times finer for kilometre
    cauliflower lobes; the cumulus erosion at a fifth of its strength there. A soft rim let the erosion bore through
    the whole tower into foam.
  - Anvil: a fan from a sharp upwind edge, widening 0.35 core radii per radius downwind to about 7.5 radii, a flat
    top under the tropopause and a flat ceiling below, at a tenth of the tower's density and thinning downwind
    (`r_thin`). Mammatus sag into its underside (a 1.6 km jittered grid, up to 500 m, where it overhangs and the
    drier the air); the separate mammatus march is gone.
  - The crowd: raising the type under the anvil made every single cell there a congestus as tall as the tower. Now
    the storm's column height is a separate bound (`storm_column_type`, also in `get_cloud_top_at`) and the cells
    keep their own type's height; cells fade out within 1.8 to 3 core radii of the tower.
  - The storm body has its own density (0.1 per metre in the tower), not the surrounding cover's and type's.
  - Cirrus: fibres warped by a 20 km noise (they hook and bend instead of all converging) and a fibril read 3.3 times
    finer inside each streak.
  - Raster and PT match (640). 1.1 s a frame on lavapipe with the storm in view at 960x540 (the stills used to read
    as foam because 3 frames are not enough for the reprojection: review stills use 24).

## Light grid contrast (20 Nov 2026; scene: grid on dulled sunlit rims at dawn)

- Reproduced in the weather bench (`--view=shafts --sun-up=8 --cover=0.45 --type=0.9`, A/B with
  `--sky-params=cloud_light_grid_enabled:0`): with the grid the brightest 1% of the sky fell from 136 to 92.
- Cause 1, a leak at the read: the sky reads the grid's depth 512 m along the sun path, trilinearly. Between levels
  that mixed paths a level apart, which against a low sun start hundreds of metres apart along it, and texels whose
  centres lie inside a cloud, so clear paths took on their neighbours' clouds. Fixes: the read lands on a level's
  centre (the march runs on to where the path crosses one), the stored depth is the least over each texel's
  3x3x3 neighbours, and the bake chains levels by the nearest voxel instead of bilinearly (the bilinear chain blurred
  again at every level). Rims back to 106.
- The rest is not a fault of the grid: the bake agrees with a 32 m CPU march to 24 km (mean depth 50.27 against
  50.33 over 1,176 sampled voxels, and of the 489 whose fine march is clear only 3 read over 3 in the grid). Without
  the grid the shader's doubling steps (several km long far out, read at a coarse mip that the coverage remap turns
  to zero) miss the clouds 8 to 24 km toward a low sun, so clouds stood sunlit that the clouds between them and the
  sun shade. The bounce term adds about 3 levels to the darkest tenth (interreflection, as designed).
- Raster and PT match (640).

## Storm review 3 (20 Nov 2026; docs: mottled cylinder, decal mammatus, no shelf or shaft at 30 km)

- Tower (`storm_tower` / `wc_storm_tower`): a core column (0.5 to 0.7 core radii) and eight rising bubbles, each
  a thermal 0.7 to 1.4 radii across, at its own height (14% to 88%) and bearing round the axis, larger higher up,
  plus a crown over the axis; outlines hard over their last fifth, jittered a little by the shape noise. A Cb reads
  as a few huge rounded towers, each lit on its sun side and grey in shadow, which the bubbles' own shape gives the
  light march; the finer lobe noise (an even mottle over a cylinder) is gone, and with it one texture read.
- Mammatus: packed (cells 0.6 to 0.8 of the 1.6 km grid, neighbours overlap), up to 700 m deep, not at the anvil's
  thin side edges; the sheet over them is denser (0.45 of the tower's) so the underside is one surface, and the
  pouches denser still (0.7), so each shades its lower half (Schultz et al. 2006: they carry the anvil's falling,
  evaporating ice).
- Rain shaft: the heavy core 1.5 radii across under the tower (it filled the whole base as a grey wall), curtains
  thresholded more sharply, lit at two thirds of the open sky's light under the shield (rain is lighter than the
  base above it).
- Shelf: its top a smooth slope rippled by the noise at large scale, its face lightly eroded (laminar), not a
  frilled grainy rim.
- Raster and PT match (640). Review stills: storm 15 km (`--storm=0,-24000,...`) and 20 km (`0,-30000`), 24 frames.

## Storm review 4 (20 Nov 2026; docs: figurine tower, UFO shelf, repeating mammatus, faint shaft)

- Proportions: the tower 1.5 core radii across at its base (about 7.5 km for R 2.5 km), widening to 2.4 by 85% of
  its height into an anvil that starts 1.6 radii wide and widens 0.4 per radius downwind; twelve bubbles (0.6 to 1.9
  radii across) set 0.45 to 1.0 radii off the axis, and the cumulus erosion back at 45% for a cauliflower skin.
  Cells fade 2.6 to 3.8 radii out.
- The "brim" round the base was not the shelf: the storm's raised cover made a flat-topped deck there. The deck now
  gives way round the tower as the single cells do.
- Shelf: only along the leading front (cos 0.65 to 0.92 of the storm's motion, about 40 degrees each side), just
  outside the tower (1.5 radii plus 0.04 per m/s of outflow), a wedge whose top is lowest at the leading edge and
  rises about 1.5 km back toward the storm.
- Mammatus: a fifth of the grid cells hold none, sizes 0.45 to 0.85 of the cell and depths 0.25 to 1 vary, in
  patches about 6 km across.
- Rain shaft: lit at two fifths of the open sky's light under the shield, so it stands as a dark curtain.
- Raster and PT match (640).

## Storm review 5 and snow shafts (20 Nov 2026)

- Shelf: tapers in height toward both ends of its arc (the leading front, about 30 degrees each side) and rises
  from a low leading edge back to the tower's base; its face in tiers (bands a quarter of its height, lighter and
  denser) over a smooth underside, lightly eroded (laminar). The flat slab across the whole base is gone.
- Mammatus: bell-shaped pouches ((1 - r^2)^1.5) that curve out of the sheet, at the sheet's density, instead of
  hemispheres with a hard rim at a higher density (read as dark craters from the side).
- Snow shafts: each storm's snow share is the weather's own rain/snow split (`get_rain_share_at` under it,
  `State.storm_snow`, `cloud_storm_snow`). Snow's extinction follows SnowFall3D.snowfall_extinction (about 2 km of
  visibility at 1 mm/h of water, 200 m at 3 mm/h; fitted 0.00196 r^2.1, capped at 5 mm/h); flakes fall at about
  1 m/s, so the shaft trails seven times further downwind than rain; its veils are softer. Nimbostratus while it
  snows was already the case (the regime reads the total precipitation). Bench `--temp=C`.
- Raster and PT match (640).

## Storm review 6 and winter convection (20 Nov 2026; docs: dome shelf, slab snow shaft, smudged winter tower)

- Shelf: a long, low arc wider than the tower (centred 1.9 radii out plus 0.04 per m/s of outflow, along the
  leading front over a wider angle, smoothstep 0.15 to 0.6 of the alignment), its top low at the leading edge and
  tapered toward the ends of the arc, sharp underside, tiered lighter face. The anvil's downwind overhang keeps a
  thin sheet (density 0.25) so it reads as one surface.
- Snow shafts: flakes fall about 1 m/s against 6 to 9 m/s for rain, so the shaft slants 3.5 times further downwind;
  a soft core (exp(-r) in place of exp(-r^2)), wider curtain thresholds and a 500 m fade below the base, so it is a
  veil trailing downwind and not a slab with a hard edge.
- Winter convection is shallow: snow thunderstorms and lake-effect cells top out at 3 to 6 km against 10 to 15 km
  in summer (Market et al. 2002, thundersnow climatology; Niziol et al. 1995, lake-effect snow). The storm's height
  scales by a cold factor from 0.45 at -10 C to 1 at 20 C (the air temperature at the ground from the weather), and
  the anvil's downwind length scales with it, so a winter storm is a low, lit tower with a short anvil.
- Grain (the half-resolution reconstruction): each pixel is drawn every 16th frame, and its exponential mean over
  4 samples stopped reducing noise there, so thin hard-edged cloud (the shelf, about 500 m thick seen at a few
  degrees through 600 m march steps) kept a grain however long a still ran (256 frames as grainy as 96). Two fixes in
  the engine: the sky shader reads REPROJECTION_FRAME (the frame number whose low four bits pick the pixel each 4x4
  block draws; the Bayer order now follows it), so each pixel's march starts form a golden-ratio (R1) sequence over
  its revisits instead of unrelated noise; and the mean runs over 12 revisits, the min/max history clamp still
  following moving cloud. Tripling the march steps was tried and only thinned the holes, at three times the cost.
  Review stills are 192 frames (about 1.1 s at 174 Hz).
- Raster and PT match (640).

## Storm review 7 (21 Nov 2026; docs: mushroom ring, grain, contour bands)

- Grain on the shelf: a render with the march start fixed showed step banding and dots, and one with the shelf's
  detail erosion off came out smooth: even a light erosion (0.15) over a dense, thin, hard-edged wedge seen at a few
  degrees read as grain. The shelf is laminar (Doswell 1985: the lifted inflow condenses smoothly), so it now has no
  detail erosion, its face shaped by its tiers alone. Inside cloud the short step also stays under 0.4% of the
  distance (100 m at 25 km), with the iteration cap raised to four times the steps.
- Shelf: hung from the storm's base instead of a ring further out. From under the tower (0.85 core radii) it slopes
  down to a sharp leading edge just ahead of the tower's base (1.45 radii plus 0.02 per m/s of outflow), its top
  flush with the base, along the leading side (about 70 degrees each way), the edge bulging 0.25 radii in and out
  along the front (outflow surges), so it is one cloud with the base and not a saucer.
- Contour bands under the anvil (and swirls on towers): the light grid's read stepped up a level wherever the read
  height crossed one, a jump in shading along height contours of a sloping underside. The read now also takes the
  next level up a level's climb further along the path (one march sample across the gap) and blends by where the
  height falls between the two, so the shading is continuous at each crossing.
- Snow veils stay lighter than rain's curtains under the shield (three quarters of the sky's light against two
  fifths): flakes scatter nearly everything they intercept. Their curtains keep more contrast.
- Storm towers billow all the way down (no fair-weather wisps on their sides).
- Bench `--fov=D` frames a whole storm.

## Side view and the anvil's upwind edge (21 Nov 2026)

- The side view's tower reads dark and streaky because the sun sits on the anvil's downwind side there: the anvil
  shades the tower's sunlit face and only crests poking out of its shadow catch the sun (the light grid was ruled
  out: the same with it off). Lit from the other side (`--wind-dir=-1,0 --storm=9600,-22000,...`) it is solid.
- The anvil's upwind edge was a cut wall. It now bulges upwind (the overshooting outflow spreads a little against
  the wind), deepest at mid-depth (0.35 core radii), roughened by the noise.

## Cumulus base detail: sag and scud (21 Nov 2026; Paulo's pick)

- Flat bases are right: a field of cumulus shares one condensation level, level to tens of metres (Stull,
  Practical Meteorology 2017, ch. 6). What breaks them up is below.
- Sag: each cloud's base dips under its middle, where the updraft core brings moister air that condenses a little
  lower: up to 5% of the cloud's height and at most 150 m (BASE_SAG), a parabola across the cloud, a random
  0.4 to 1 per cloud. The base's softness is now in the cloud's own height (sagged), not the column's.
- Scud (cumulus fractus, pannus): ragged fragments 20 to 450 m under the bases of cumulus and decks
  (SCUD_DEPTH) where the air under them is near saturation, from eddies and rain-cooled air (WMO International
  Cloud Atlas; Houze, Cloud Dynamics 2014). The share is State::scud, from the weather: the relative humidity at the
  ground (none under 82%, all by 97%) or rain falling (all by 3 mm/h). The rags are the shape noise at an eighth
  of its scale, thresholded, raised under a cloud or a deck; their top and bottom wander with the same noise, so
  no straight walls; the cumulus wisps erode them. Bench `--humidity=H`.
- Raster and PT match (640).

## Lake-effect snow bands

- When: cold air crossing open water at least 13 K warmer than the air at 850 hPa (Niziol et al. 1995, Wea.
  Forecasting 10), here the water against the air 1.5 km up on the 6.5 K/km lapse (from 10 to 16 K), ice-free
  water (above about 1 C) and a wind of 5 m/s or more. Water temperature is the weather's own
  (`get_water_temperature`, the climate a month late; `water_temperature` overrides it).
- Fetch field (WeatherLakeEffect): 64 x 64 cells of 2 km round the camera; each is open water where WorldWater's
  surface stands a metre or more over the terrain (puddles do not count), sampled over a few ticks. For the wind's
  direction each cell holds the water crossed upwind (km, each km weighted by exp(-s / 60 km) for its distance s
  back), so the cloud and snow reach 50 to 100 km inland and fade; water past the square's edge continues as the
  edge holds (a sea). A second channel holds how near the cell lies to the largest fetch across the wind within
  16 km: the axis a single band forms on. Rebuilt when the wind turns by 5 degrees.
- Form, by U / L (wind over the longest fetch, m/s per km; Laird, Kristovich and Walsh 2003, MWR 131, as cited
  by Laird and Kristovich 2004): vortices up to 0.02 (left out), a shoreline band 0.02 to 0.06, band and
  widespread cover together to 0.11, wind-parallel rolls above. The rolls' spacing is 2.75 times the convective
  layer's depth (Kristovich 1993), the depth 1.5 to 3.5 km with the instability. U is the 10 m wind times 1.4
  (above the surface layer's drag).
- Effect: cover rises toward 0.95 and the type toward cumulus congestus along the bands (cover_type_at and
  wc_cover_type, so shadows, the cells and the sky agree); the weather's cells add 4 mm/h of water at full
  strength (2 to 8 cm of snow an hour), so get_snowfall_at, wet ground and snow cover follow. The sky draws the
  falling snow from afar as veils under the band (march_lake_snow: 16 steps from 1.5 to 30 km, the storms'
  snow visibility, Rasmussen et al. 1999), trailing downwind as the flakes fall; near the camera the falling
  snow's particles draw it.
- Bench: `--view=lake_effect` (a 30 km by 200 km lake, 6 C water, -8 C air, wind down its length, camera 14 km
  aside of the band); `--test=clouds` checks the band on the axis and none aside, snowfall under it, the cover,
  no band with the water under 10 K over the air aloft, and rolls with the wind across the lake.

## Lenticular plates and iridescence

- Technique: Altocumulus lenticularis duplicatus (pile of plates). Each plate is a moist layer lifted
  over the lee wave crest; dry layers between them stay clear. Implemented as vertical gaps every
  400 m (offset by 4 km noise) inside the wave layer, faded in by a 9 km stack mask so some lenses
  stay single. Applied in `wave_plate_gaps` (CPU) and `wc_wave_sigma` (GLSL), so shadows and the
  view march agree.
- Iridescence: lens edges hold young, small, uniform droplets, which diffract sunlight into pastel
  bands near the sun (Gedzelman and Lock 2003, "Simulating coronas in color"). Airy pattern
  [2 J1(v)/v]^2 per RGB wavelength (0.65/0.55/0.45 um), luminance-normalised, droplet radius 1 um
  at the edge to 6 um inside. Only within about 35 degrees of the sun, only on thin parts of the
  plate (`cloud_iridescence`, default 0.6). Raster and PT share the sky shader.
- Docs review (21 Nov): plate ends stepped and plates dotted. The lens march took four fixed samples per
  coarse step (125 m) and lit each coarse step once, so a plate's end and the 100 m gaps fell between
  samples. Now eight samples per step, jittered per revisit (the 12-revisit reprojection averages them), and
  the sun's light read at each sample. The tint covers both forward lobes and its difference from white is
  doubled (the lens's depth along a ray mixes droplet sizes and greys it); droplets 1.5 to 5 um; default
  0.8. PT matches raster at 192 frames; at 30 frames each pixel has had two revisits and shows the dots.
- A hard round cut in the lens near its end (in the 1920 shots, older than the plates): the lens march stopped
  8 km into the slab, so a lens running on beyond that ended on the sphere where the cap met it (and lenses
  further along a ray were never drawn). It now runs up to 16 km in a fixed 32 coarse steps (a count that varied
  between pixels stepped the plates' gaps), each step tested on the largest lift at its middle and both ends.

## Review 11 (21 Nov 2026; docs: plates gone, rainbow fringe, boxy cloudlet, lake band a flat wall)

- **Plates from humidity sheets.** The pile of plates is now moist and dry sheets 220 m apart in the dew depression
  (wave_plate_depression(), wc_plate_dep()), stacked where a 9 km noise says so and fading 1.5 thicknesses from the
  layer's middle: a lens condenses as one plate where the air is uniform and as a pile where it is layered, as
  observed ("pile d'assiettes"; Ludlam 1980, Clouds and Storms). The old cut-out gaps went: they left a puffy blob
  once the march reached 16 km. The wave march is fixed at 32 steps over at most 16 km (a step count that varied with
  the range stepped the plates).
- **Smooth crests.** mountain_wave_eta() reads the displacement grid with a cubic B-spline (16 taps on the CPU, 4
  bilinear taps in GLSL; Sigg and Hadwiger 2005): linear reads put kinks at the grid lines into the lens outlines.
- **Iridescence** is a softer tint (1 + 1.6 (tint - 1), clamped), only on the thin edge (profile 0 to 0.08 and
  out by 0.9), with the droplet radius wandering 2.5 um over 700 m so the bands are not drawn by a ruler.
- **Mid-layer cloudlets.** A thin cover thinned the cloudlets by raising the billows' threshold until only their
  peaks were left, the fine octaves' cells of a few texels, drawn as blocks (one read as letters). The threshold now
  stops at a 0.3 cover and a thinner cover fades the cloudlets instead; the cover is read through a 300 m warp and
  near texels are smoothed (Quilez's improved texture interpolation). The mid cover itself and the stratocumulus
  cells are domain-warped (Quilez) so no edge runs straight for kilometres.
- **Lake band.** Snow curtains in three octaves (2.5 km, 800 m, 250 m stretched across the view) wander 1.5 km,
  fade in over the 100 to 700 m under the base, and the cover share is 0.8 so the band breaks into cells; the bench's
  shadow grid spans 60 km so the band's shadow has no straight edge.

## Distant shafts in front of terrain (21 Nov 2026; Paulo's pick)

The storms' rain and snow shafts and the lake-effect veils are marched in the sky shader, so they show only where
the sky shows: a shaft standing between the camera and a far ridge vanishes in front of the ridge and reappears above
it. Engines that draw far precipitation composite it against the depth buffer: Nubis and Red Dead Redemption 2's
volumetrics (Schneider 2015, 2017; Bauer 2019) march the view ray only up to the scene depth and blend the result
over the opaque scene; Hillaire's aerial perspective (2020) does the same for the air.

- **Technique.** A depth-limited ray march over the scene: one screen-covering quad in the transparent pass reads
  the depth texture, marches the same shaft functions as the sky (shared GLSL) from 1.5 km to the scene's depth, and
  blends premultiplied over the terrain. Where there is no geometry the sky already draws the shaft, so the pass
  skips sky pixels.
- **Light.** The same terms as the sky: the sky's light under the base (ambient_bottom) and the sun through the cover
  with the drops' Henyey-Greenstein phase. The march runs with unit lights (ambient (1, 0, 0), sun (0, 1, 0)), so one
  march returns both weights; the material's own lighting supplies the lights: the sky's irradiance on a sideways
  normal (half sky, half ground, as ambient_bottom) through ALBEDO, the sun's colour through light(). The cloud
  uniforms are copied from the sky material each tick (WeatherMap3D distant_shafts, weather_shafts.cpp).
- **Bench.** --no-distant-shafts for the A/B; a storm before the lenticular view's ridge:
  `--view=lenticular --storm=3000,16000,2500,20,8 --cam=14000,700,24000,11100,600,12360 --fov=35`.
- **Wave clouds, same day.** Scene's scout on a 1636 m crest got lenses of optical depth 0.09 with an 838 m wave:
  the moist layer's clear air fell to RH 0.5 to 0.68 away from its altocumulus patches, so a lee wave over a gap
  needed 600 m and more of lift. The layer's clear air now keeps RH 0.8 once its cover is 0.3 (wave_layer_rh()):
  350 m of lift condenses a lens anywhere along a moist layer, as on wave days.
- **Path tracing.** Hidden while the environment path traces: the tracer has its own media, and a screen quad would be
  hit by camera rays.
- **Cost.** Early-outs per storm on the ray's chord through the shaft's column and per pixel on sky depth; 24 steps
  a storm, 16 for lake snow, as in the sky.

## Review 12 (22 Nov 2026; docs: stair-stepped edges everywhere, melted-clay lens, no iridescence, no band)

- **The common cause of the stair steps and the smooth cotton balls.** With the reprojection the cloud pass draws one
  pixel of each 4x4 block a frame, so dFdx(EYEDIR) there spans four screen pixels: the noise was read at four
  pixels' footprint (detail gone), and every revisit sent its ray through the same pixel centre, so a cloud's hard
  edge (a lens, a turret) came out as a staircase. The derivative is now quartered, and each revisit moves the ray
  inside its pixel on an R2 sequence (Roberts 2018): the resolve's running mean becomes the pixel's coverage, the
  sub-pixel jitter TAA and Nubis use for edges.
- **Lens as a stack.** The moist layer is flat-topped (exp(-d^4), about 800 m deep with sharp edges, as under an
  inversion) where a Gaussian's tails let a strong wave condense a 1.5 km blob; the dry sheets between plates are
  3 K drier (375 m more lift), so they stay clear as gaps instead of embossing grooves.
- **Iridescence** 2.2 times the Airy tint's difference from white, over the edge's first 15 % of depth and out to
  40 degrees from the sun.
- **Lake band.** The bench view is clear apart from the band (a cold outbreak's sky), seen from 16 km aside looking up
  it; the dark plane drawn for the lake (the water is the LakeStub contract) is hidden. The veils march to 60 km.
- **Distant shafts' light.** The irradiance on a sideways normal is about twice ambient_bottom (the bright horizon and
  sunlit ground in full): halved, so a veil keeps one brightness across the horizon.

## Review 12 leftovers (22 Nov 2026; Paulo: fix the three visible flaws)

- **Plates inside the lens.** The dry sheets' gaps now close where the lift is large (wave_plate_x(): gone by 400 m of lift over saturation), so plates
  show at a lens's edges and merge inside it, as the Atlas describes the pile of plates.
- **Seam at the horizon.** The far veils drawn over the ground lacked the air in front of them: each veil now keeps
  the environment fog's transmittance to its depth (the ground behind carries its own fog), its ambient normal tilts
  45 degrees up (the ground's half was warmer than ambient_bottom) and its ambient is scaled 0.38, measured on one
  lake-effect veil just above and just below the horizon (80 vs 83 grey).
- **Lens outline and the oval.** The lenses read the mid layer's moisture at the altocumulus patches' scale, with their
  saturated cores, so lift condensed only inside patches: each lens's outline was a patch's, and one small patch over a
  bump of lift made a perfectly elliptical saucer (the "ghost oval"). The lenses now read the layer's moisture at three
  times that scale and without the cores: broad and smooth, so the outline follows the wave's crest, a long lens
  tapering at its ends, as observed. (A moist floor of 0.88 was tried first: it flooded the cirrus wave slab with
  streaks; the floor stays 0.8.)

## Review 13 (22 Nov 2026; docs: stepped cumulus, streaky plate edge, lens as a contrail, lake wall)

- **One root cause for the steps and streaks.** The reprojection's history clamp (Karis 2014 min/max box over the
  drawn pixels) took its 3x3 box round the nearest drawn pixel, which changes every 4 pixels. Wherever one sample's
  noise pulled the box in, the converged history was cut along that 4-pixel grid: edges came out stair-stepped (the
  same at --no-reprojection, since that flag's frames differed by 0.1 grey) and plate edges as short vertical streaks.
  Measured on an 8x zoom of a cumulus edge: column-to-column jumps every 4th column of 1.9 against 0.3 to 0.7; now
  0.7 to 0.95, flat. The box is now bilinear in the pixel's position (the four drawn pixels about it, each with its own
  3x3 box; 16 fetches instead of 9), so it moves smoothly and the clamp still follows motion. Quilez's smoothing of the
  shape texels was tried first, changed nothing, and was dropped (its cost bought nothing).
- **Iridescent plate tip.** The plates' streak noise was stretched 7:1 along the wind, which drew the thin tip as
  brush strokes; now about 2.5:1, with a 25 m wander instead of 40.
- **Lake band wall.** Still open: the far veil drawn over the ground reads as a flat grey strip with a toothed foot.
- **Lenses that end.** The test ridge was 80 km of near-uniform crest, so its lee wave was the same along it and every
  lens one bar the ridge's length, seen along it as a contrail. Moisture stretched along the wind (streamlines) was
  tried and changed nothing, since the layer is near saturation everywhere and the lift alone sets the outline. The
  ridge now has summits some 16 km apart with saddles under half their height, as a range has; the lenses form over
  the summits, a few km long, and the bench view looks up the wind from downwind.

## Review 14 (22 Nov 2026; docs: zebra iridescence, soft plate, serrated edge)

- **Iridescence as zebra contours.** A single droplet size per point drew the Airy pattern's higher orders, whose
  rings crowd as the size grows, so each small change of size was a new hard ring; and the size wandered 2.5 um over
  700 m, so the rings ran across the edge. The tint now averages three sizes (8 % spread; Gedzelman and Lock 2003 need a
  narrow but not single size), and the size follows the depth in from the edge, wandering 0.6 um over 2.5 km: pastel
  pink and green bands along the outline.
- **Soft plate.** The plate's water was uniform inside the lens. It now varies in fibres, the detail noise stretched
  6:1 along the wind and flattened 3:1 in height (sigma x 0.3 to 1.7), read at the pixel footprint so far lenses keep
  their mean. One more texture read per fine sample.
- **Lake band veil.** The bench view moved 4 km further back and looks higher, so the veil meets the ground near the
  horizon instead of standing over the near ground as a strip. The veil drawn over near ground is unchanged.

## Review 15 (22 Nov 2026; docs: lake veil jagged and striped, smear columns in the band's base)

- **Smear columns.** The band's field (wc_lake_effect) was read bilinearly over its coarse cells, so the band's edge
  was a polygon of straight runs and the cumulus over it stood those up as flat vertical facets. Now a cubic B-spline
  read (four bilinear reads, Sigg and Hadwiger 2005, as the wave field), on the CPU too.
- **Stripes in the veil.** The curtains' finest octave was stretched along the view so its streaks would stand up;
  that made the pattern follow the camera and each pixel column a ray down one long streak. Isotropic now: the
  density is the same all the way down, so the sheets stand upright by themselves.
- **Grain over the ground.** The pass over the scene has no temporal resolve, so its interleaved-gradient jitter
  stood as a fixed diagonal grain. It now steps at mid-step with 32 steps (16 in the sky): each step's ends follow
  the pixel's depth, which changes smoothly over the ground, so the veil is smooth. Path tracing has no
  over-ground veil (the pass is raster only), as before.

## Haboob dust wall in the cloud march (design, 22 Nov 2026; Paulo's haboob pick, sand's contract)

Technique: a participating medium of mineral dust marched with the low cloud, its shape a density current's head
(Simpson 1997, "Gravity Currents": the raised head about twice the following flow's depth, a nose lifted off the
ground by friction, lobes and clefts along the front from the lobe-and-cleft instability, Kelvin-Helmholtz billows
rolling back along the top behind the head; Benjamin 1968 for the head-to-depth ratio). Optics from the contract
(sand, 22 Nov): mass extinction 0.6 m2/g at 550 nm, per-channel 0.97/1.0/1.04, single-scattering albedo 0.92 at 550
falling to 0.85 in blue, Henyey-Greenstein g 0.75 (dust's forward peak; Dubovik et al. 2002 AERONET dust).

- Reads (GPU, contract): weather_dust_map over weather_map_rect: R column g/m2, G top above ground (m, already with the
  nose), B head share 0..1. Concentration c = R / (0.9 G) uniform to 0.8 G, smoothstep to 0 at G (matches
  dust_concentration_at). Read with the cubic B-spline (the band field's lesson: bilinear cells stand up as facets).
- Shape at the head (B > 0): lobes and clefts along the front as a 1D noise along the front tangent (gradient of R)
  at 300 to 800 m (Simpson's lobe spacing scales with the head height, about 1 to 2 heights), moving the top by
  +-25 % and cutting clefts down to the nose; the nose: density lifted off the ground in the first 50 to 150 m ahead
  of the foot (the foot trails the nose); billows: a 3D detail noise advected with the outflow on the top surface,
  the cumulus detail texture at the dust's scale, so the top reads as rolling cauliflower like the photos (Phoenix
  2011, Khartoum). Body behind the head: smooth, a slight erosion of the top only.
- March: its own slab from the ground to max top (<= 3 km), entered only where the map's R > 0 along the ray (a coarse
  pre-pass over the map at the cumulus' coarse steps, as the wave slab). Composited with cloud_over by depth, under
  the cumulus. 24 coarse steps, refined at the edge; jitter per revisit as the other layers.
- Light: as the low cloud, Wrenninge's octaves for multiple scattering with the dust's albedo per channel (that is
  what turns it brown-orange: blue is absorbed more at each scattering), sun transmittance through the dust toward the
  sun (two reads), ambient from above and the ground. Sun dims inside by the column (the sky and haze readers get it
  from sand's layer).
- PT: the same density function in the PT medium (as the clouds' PT path), so raster and PT agree.
- Cost: zero when no dust (R == 0 everywhere: a uniform flag from WeatherMap3D, like cloud_lake.x). Bench: sand's
  haboob bench; my checks: 100% crops of the head, raster against PT 640.

## PT parity for the veils over the ground (22 Nov 2026)

Path tracing lost the snow and rain veils over distant ground, for two reasons. First, its camera rays stopped at a
fixed 10 km, so ground beyond that was sky. They now go to the camera's far plane, as raster draws. Second, the
veil pass (a full-screen quad that marches the medium to the scene's depth) was switched off under path tracing,
since blend surfaces are traced there and the pass has no place in the ray-traced scene. Now any material whose
shader writes POSITION (a screen-space overlay, no world geometry) stays out of the TLAS, and is drawn over the
traced frame against the traced depth even under path-traced transparency, lit from the trace's light cache. The
lake view's veil row now matches raster (84 grey at the horizon in both). Under Ray Reconstruction the overlay is
drawn into the colour before the denoiser; a layer of its own is left for later if it smears.
