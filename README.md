# <img src="misc/dist/linux/berlenga.svg" width="32" height="32" alt=""> Berlenga

[![Release](https://github.com/pcanilho/berlenga/actions/workflows/release.yml/badge.svg)](https://github.com/pcanilho/berlenga/actions/workflows/release.yml) [![CodeQL](https://github.com/pcanilho/berlenga/actions/workflows/codeql.yml/badge.svg)](https://github.com/pcanilho/berlenga/actions/workflows/codeql.yml) [![Latest release](https://img.shields.io/github/v/release/pcanilho/berlenga?label=latest)](https://github.com/pcanilho/berlenga/releases/latest)

Berlenga is a Godot 4 fork with Vulkan path tracing and DLSS Ray Reconstruction, built for large open worlds. Based on [NVIDIA-RTX/godot](https://github.com/NVIDIA-RTX/godot) (2 November 2026).

**[Download](https://github.com/pcanilho/berlenga/releases/latest):** Windows (Ray Reconstruction: add NVIDIA's DLSS, Streamline and Reflex DLLs next to the exe), Linux x86_64/arm64 (no Ray Reconstruction), macOS (raster only). **Changes:** [CHANGELOG.md](CHANGELOG.md).

![1024 street lamps: NVIDIA-RTX/godot on the left keeps 64 of them, Berlenga on the right lights them all](docs/media/many-lights.jpg)

*1024 lights, 16 samples per pixel. Left: NVIDIA-RTX/godot. Right: Berlenga. Images are raw path tracer output, mostly CPU-rendered, without denoiser or Ray Reconstruction.*

## At a glance

- **Lights**: 4096 path-traced lights (upstream 64); emissive surfaces count as lights.
- **Faster convergence**: light visibility memory and a radiance cache.
- **Materials**: clearcoat, back lighting, thin-glass transmission, subsurface scattering; textures filter like raster.
- **Hair**: fibre shading, strand grooms, TressFX strand simulation with body collision.
- **Terrain**: mountain ranges raised and eroded per seed, with geology, river networks, coasts and lakes; non-destructive height brushes; terrain data maps.
- **Snow**: a snowpack from the weather map that falls, settles, melts, slides and drifts, covering materials raster and path traced; deep snow with tracks; falling and blowing snow; baked winters and cornices.
- **Vegetation**: biome plant placement from terrain maps; grass in the weather's wind; GPU-placed instances seen by path tracing; per-instance bones for plants in the wind.
- **Weather**: one weather map for wind, storms, rain, wet surfaces and puddles, driving water, cloth and hair.
- **Clouds**: a cloud layer from the weather map, reprojected, seen by path tracing, with shadows on the ground.
- **Water**: refraction, murky water, caustics, ocean, currents, white water, rivers and waterfalls, shore waves, camera under water.
- **Exposure**: histogram metering, dark-scene key, local exposure.
- **Physics**: PhysX 5 alongside Jolt: cloth, tearing, destruction, path-traced smoke.
- **Open worlds**: camera-relative tracing, persistent instances, culling, mesh LOD.

## What Berlenga adds

Each feature that changes the look or the cost can be turned off with the setting shown.

### Lighting
- **4096 lights** (upstream 64): camera-following light grid with resampled importance sampling.
- **Emissive surfaces as lights** (`rendering/pathtracing/emissive_lights`).
- **Light visibility memory** (`rendering/pathtracing/light_visibility_memory`): hidden lights stop taking samples.
- **Radiance cache** (`rendering/pathtracing/radiance_cache`): bounces end on cached light.
- **Light reuse** (`rendering/pathtracing/light_reuse`, off by default): ReSTIR DI reuses light samples across pixels and frames.
- **Indirect light reuse** (`rendering/pathtracing/indirect_reuse`, off by default): ReSTIR GI; rooms and shade lit by bounced light are far less noisy at the same samples.
- Shadow-only casters bounce light.
- Lit raster transparents and rain (`rendering/pathtracing/max_transparency_layers` 0), still traced in reflections.

<details><summary>Image: indirect light reuse</summary>

![A window-lit room at 4 samples per pixel, indirect light reuse off and on](docs/media/indirect-reuse.jpg)

*Indirect light reuse: the same 4 samples per pixel, off then on; both brightened 3x to show the noise.*

</details>

### Materials
- **Back lighting** (`rendering/pathtracing/back_lighting`): light through leaves, paper and curtains, with coloured shadows; back light texture.
- **Transmission** (BaseMaterial3D section, `rendering/pathtracing/transmission`): thin glass, tinted and blurred by roughness, with tinted shadows; transmission texture; `TRANSMISSION` in shaders; glTF `KHR_materials_transmission`.
- **Clearcoat** (`rendering/pathtracing/clearcoat`): matches raster, with traced coat reflections; coat texture.
- **Subsurface scattering** (`rendering/pathtracing/subsurface_scattering`, radius `subsurface_scattering_scale`): random walk inside closed meshes; skin, wax and marble soften and thin parts glow when backlit.
- **Wet surfaces** (BaseMaterial3D Wetness: `wetness_enabled`, `wetness_porosity`): porous surfaces darken and turn glossier as the weather map gets wet, raster and path traced; `get_wetness_shader_code()` for ShaderMaterials.
- Shaders writing `BACKLIGHT` or `TRANSMISSION` tint their shadows.
- Textures filter like raster (ray cones, anisotropic).
- Even, tinted glass shadows.
- Back-face normals in `vertex()`; `IN_SHADOW_PASS` in custom shaders.

<details><summary>Images: transmission, back lighting, clearcoat, layer textures, subsurface scattering, wet surfaces</summary>

![Glass panes before coloured spheres: raster, path traced with transmission on, and off](docs/media/transmission.jpg)

*Transmission: the spheres show through and the floor gets green-tinted light.*

![Back-lit leaf cards and a red curtain, back lighting on and off](docs/media/back-lighting.jpg)

*Back lighting: green and red light on the floor.*

![Red coated spheres: raster, path traced with clearcoat on, and off](docs/media/clearcoat.jpg)

*Clearcoat: sharp coat highlights, as in raster.*

![Checkered back light and transmission textures, raster vs path traced](docs/media/layer-textures.jpg)

*Layer textures: checkered coloured shadows.*

![Backlit skin slabs 2 to 20 cm thick, subsurface scattering off and on](docs/media/subsurface.jpg)

*Subsurface scattering: thin slabs glow, thick ones stay dark.*

![Spheres of rising porosity, dry and wet, raster and path traced](docs/media/wet-materials.jpg)

*Wet surfaces: the more porous, the darker and glossier when wet; raster and path traced match.*

</details>

### Water
![A pool with a checkered floor and coloured posts, refraction off and on](docs/media/water-refraction.jpg)

*Refraction off, then on: the posts shift at the surface and the floor darkens with depth.*

- **Refraction** (`rendering/pathtracing/refraction`): water and glass blocks bend light by their IOR, with total internal reflection and depth tint; glTF `KHR_materials_volume`, `KHR_materials_ior`.
- **Murky water** (same setting): scattering media; `IOR`, `ABSORPTION` and `SCATTERING` in shaders; glTF `KHR_materials_scatter` draft.
- **Caustics** (`rendering/pathtracing/caustics`): sunlight lines under rippled water.
- **Caustic cascades** (`rendering/pathtracing/caustics_cascades`): sun caustics out to 256 m, near lines unchanged.
- **Path-traced ocean** (`PhysXWaterSurface3D`: `pathtraced_absorption`, `pathtraced_scattering`, `pathtraced_roughness`): waves refract, tint and scatter, with caustics on the seabed.
- **Ocean swell** (`wave_cascades`): up to four wave layers give long swell under the chop without visible tiling, whitecaps where they add up, on a shallow-water (TMA) spectrum; set `depth` to the real depth for open sea (the 3 m default damps long waves).
- **Finer water mesh up close** (`mesh_detail`).
- **Shore waves** (`shore_waves`): swell turns with the seabed, grows in the shallows, breaks and runs up the sand (Stockdon run-up).
- **Wave groups** (`shore_wave_groups`): shore waves arrive in sets with their own Rayleigh heights, so each breaks at its own place.
- **Plunging breakers** (`shore_wave_curl`, `shore_spray`, `shore_spray_amount`): the crest throws a sheet of water that lands ahead and sinks into the bore, with spray where it is thin and where it lands.
- **Currents** (`current`, `flow_period`): flow round rocks and along shores carries waves, foam and floating objects.
- **Live flow** (`live_flow`, `wall_friction`): near the player, rocks and banks shed wakes and eddies.
- **White water** (`flow_foam`): foam in fast water, trailing rocks and drifting downstream.
- **Rivers** (`PhysXRiver3D`: `discharge`, `roughness`, `max_slope`): drawn as a curve; finds its depth and never runs uphill.
  - Cascades into white water; `carve` emits channel brushes for a terrain deformer.
  - Rocks part the current and trail foam (`obstacle_collision_mask`); near the camera they shed eddies and a swinging wake (`live_flow`).
  - Joins lakes, the sea and other rivers (`mouth_target`), ponding to the water level; tributaries add their flow.
  - Waterfalls (`fall_min_height`): a falling sheet that thins, whitens and breaks up, with a boiling pool and spray.
  - Living waterline (`edge_lap_height`, `edge_relief`, `edge_foam`): edges lap with the flow, look ragged and carry a thin foam band.
- **Camera under water** (path traced automatically; raster with `underwater_enabled`): fog, scattering, Snell's window, image split at the water line.

<details><summary>Images and clips: murky water, caustics, ocean, currents, live flow, white water, rivers, under water</summary>

![The pool with clear and with murky water](docs/media/murky-water.jpg)

*Murky water: a green glow fills the deep end.*

![A rippled pool, caustics off and on](docs/media/caustics.jpg)

*Caustics: bright lines on the pool floor.*

![Ocean shallows with floating crates, raster vs path traced](docs/media/ocean.jpg)

*Ocean: sand and caustic light through the water.*

![Open sea with one wave layer and with three](docs/media/ocean-swell.jpg)

*Ocean swell: one layer repeats the same foam patches; three layers add long swell.*

![Waves breaking on a beach, raster](docs/media/shore-waves.gif)

*Shore waves: swell breaks into foam on the beach.*

![Waves run up a sloping beach and draw back](docs/media/shore-runup.gif)

*Run-up: the wash climbs the sand and drains back.*

![A plunging breaker throwing a sheet of water with spray, raster](docs/media/shore-curl.jpg)

*Plunging breakers: the sheet thrown off the crest, with spray along its thin edge.*

![Floating boxes drift on a current past a rock](docs/media/currents.gif)

*Currents: the boxes drift past the rock.*

![Flecks carried past a rock, baked flow vs live flow](docs/media/live-flow.gif)

*Live flow: the streaks roll up into a wake behind the rock.*

![Foam lines trailing a rock in a current](docs/media/white-water.gif)

*White water: foam peels off both sides of the rock.*

![A river with a cascade, raster vs path traced](docs/media/river.jpg)

*Rivers: the cascade and the white water below it.*

![A river parting round a rock, raster vs path traced](docs/media/river-rocks.jpg)

*Rivers round rocks: foam trails behind the rock.*

![A river with live flow: a rock sheds a swinging wake and a floating box swirls in it](docs/media/river-live.gif)

*River live flow: the wake swings behind the rock.*

![Two rivers join and flow into a lake, raster vs path traced](docs/media/river-joins.jpg)

*River joins: two streams merge and spread into the lake.*

![A waterfall from a river lip into a pool, raster vs path traced](docs/media/waterfall.jpg)

*Waterfall: the sheet breaks up into white water above the pool.*

![Under water near crates, raster vs path traced](docs/media/underwater.jpg)

![The camera dives through the water surface](docs/media/underwater-dive.gif)

*Under water: light scattering and the surface seen from below.*

</details>

### Hair
- **Fibre shading** (BaseMaterial3D Hair section, `rendering/pathtracing/hair`): Chiang hair model under path tracing, melanin colour, on shells, cards and strands; mask for single-material characters.
- **Strand grooms** (`HairGroom`, `HairMesh`): grown on a scalp mesh or loaded from a HAIR file.
- **Moving strand hair** (`PhysXHair3D`): AMD TressFX's simulation on GPU compute, guide and follow strands, velocity shock propagation, wind with gusts and turbulence, collision spheres.
- **Raster strands** (TressFX coverage): blended and sorted far to near on the GPU each frame; smooth without TAA or MSAA, and the hair shadows itself.
- **Raster hair shading** (`PhysXHair3D.hair_shading`): Marschner model as Unreal evaluates it, with multiple scattering and light through the strands in shadow maps, so backlit hair glows.
- **Distance LOD** (`PhysXHair3D`: `lod_enabled`, `lod_start_distance`, `lod_end_distance`, `lod_strand_ratio`): far away, fewer and wider strands with the same look; simulation and path tracing keep every strand.
- **Body collision** (`PhysXHair3D`: `collision_body`, `collision_cell_size`): strands collide with a body mesh, skinned or not, through a distance field built on the GPU every frame.
- **Hair volume** (`volume_enabled`, `volume_friction`, `volume_stiffness`): strands push and drag on each other through a density grid, so hair keeps its volume.
- **Motion vectors for strands**: TAA, FSR 2 and DLSS follow moving hair instead of smearing it; shaders get `PREVIOUS_FRAME` and `render_mode alpha_motion_vectors`.
- **Custom hair shaders**: `HAIR`, `HAIR_ABSORPTION`, `HAIR_ROUGHNESS` outputs; `hair_strands` and `hair_cards` render modes; `IN_PATH_TRACER` built-in.

<details><summary>Images and clip: hair shading, strand hair, moving hair, raster strands, raster shading, distance LOD, body collision, hair volume, motion vectors</summary>

![Hair-shaded spheres and cards: raster, path traced, sun behind, cards backlit](docs/media/hair.jpg)

*Fibre shading: fibre highlights and a bright rim against the sun.*

![Strand hair grooms: raster, path traced, and path traced with the sun behind](docs/media/hair-strands.jpg)

*Strand hair: per-fibre colour and a backlit rim.*

![Simulated strand hair blown by gusting wind, raster](docs/media/hair-sim.gif)

*Moving strand hair: the groom streams and flutters in gusting wind.*

![Raster strand hair: older soft strands, then sharp sorted strands](docs/media/hair-raster.jpg)

*Raster strands: sorted and blended with no AA; each strand is drawn at its real width, so the groom stays sharp.*

![Backlit strand hair in raster with Marschner shading](docs/media/hair-raster-shading.jpg)

*Raster hair shading: backlit, the strands glow warm.*

![Distant strand hair: all strands, then distance LOD](docs/media/hair-lod.jpg)

*Distance LOD: fewer, wider strands far away, with the same look.*

![Strand hair over rounded shoulders: spheres only, shoulders as the collision body, skinned to a tilted bone](docs/media/hair-body-collision.jpg)

*Body collision: with spheres only the hair sinks into the shoulders; with the shoulders as the body it drapes over them, also when skinned and tilted.*

![Strand hair shaking and in wind, hair volume off and on](docs/media/hair-volume.jpg)

*Hair volume: with it on, the hair stays full instead of collapsing into a flat sheet.*

![Moving strand hair: motion vectors before and after, then TAA before and after](docs/media/hair-motion-vectors.jpg)

*Motion vectors: before, only part of the hair reports its motion; after, every strand does.*

</details>

### Terrain
- **Mountain generation** (`TerrainGenerator3D`: `uplift`, `ranges`, `erodibility`, `critical_slope`, `detail_wavelength`, `detail_octaves`, `detail_crest`): raises ranges from an uplift map, Path3D ranges and noise, then erodes them to equilibrium with stream power erosion; branching gullies come from an erosion filter computed per point, so tiles join exactly; on crests gullies keep part of their depth and a ridged noise adds irregular pinnacles; the same for a seed; writes into Terrain3D or a HeightMapShape3D.
- **Tile erosion** (`tile_erosion_enabled`): droplet hydraulic and thermal erosion in seamless overlapping tiles.
- **Material detail** (`get_detail_shader_code()`): erosion detail below the vertex spacing in the terrain material; path tracing keeps it.
- **Geology maps** (`terrain_geology()` in shaders): soil, bare rock and scree from the erosion run.
- **River network** (`build_river_paths()`): rivers with widths and discharges, made into joined `PhysXRiver3D` paths.
- **Coast and lakes** (`ocean_edges` or `sea_map`; `lake_count`, `lakes`; `build_water()`): sea with a Dean-profile seabed and beaches; lakes behind natural dams; builds the sea and lake surfaces, and rivers end in them.
- **Climate maps** (`TerrainDataMap3D.bake_climate()`, `ClimateRecipe`): temperature, orographic rain and rain shadows, snow share, aridity, winter and annual sunlight with terrain shadows, and wind exposure.
- **Ground material** (`TerrainMaterial3D`, `use_generator()`): rock, scree, soil, alluvium and sand from the generator's geology, height-blended, biplanar cliffs, erosion detail, wetness and snow.
- **Footprints in ground** (`GroundSurface3D`, `BaseMaterial3D.ground_deform_enabled`): prints in sand, soil, mud and leaf litter; moisture from the weather map, walls slump to the angle of repose.
- **Terrain deformation** (`TerrainDeformer3D`, `TerrainBrush3D`): non-destructive height brushes (rivers, craters, plots) over Terrain3D or a height map; moving a brush restores the ground.
- **Terrain data maps** (`TerrainDataMap3D`): bakes slope, curvature, water flow, wetness, water depth and distance to water around a point, for placing plants and for materials; a 512 map bakes once in about 0.2 s.

<details><summary>Images: mountain generation, crests, tile erosion, material detail, geology, coast, climate, ground material, footprints, terrain data maps</summary>

![Generated mountain range, shaded relief with rivers](docs/media/terrain-generator-map.jpg)

*Mountain generation: ridges, valleys and the rivers that carved them, from one seed.*

![The generated range with erosion detail, raster](docs/media/terrain-detail-wide.jpg)

*Mountain generation: branching gullies down every slope.*

![Close view of the generated peaks, raster and path traced](docs/media/terrain-generator.jpg)

*Mountain generation up close, raster and path traced.*

![Summit crests: a regular comb before, irregular crests now](docs/media/terrain-crest.jpg)

*Crests: before, a thin regular comb along the summits; now, irregular crests and pinnacles.*

![The same slopes without and with tile erosion](docs/media/terrain-tile-erosion.jpg)

*Tile erosion: fine branching rills cut into every slope.*

![A 4 m terrain mesh alone, then with erosion detail in its material](docs/media/terrain-material-detail.jpg)

*Material detail: rills finer than the 4 m mesh, drawn by the material.*

![Geology map with soil, rock, scree and rivers, and the range path traced and coloured from it](docs/media/terrain-geology.jpg)

*Geology: soil green, bare rock grey, scree ochre, rivers blue; the shader colours the range from the map.*

![A generated island map: sea, beaches, rivers and two dammed lakes](docs/media/terrain-coast-map.jpg)

![The island, raster and path traced](docs/media/terrain-coast.jpg)

*Coast and lakes: beaches, a shelving seabed and lakes behind dams, from one seed.*

![Climate maps of a range: temperature, precipitation, winter sun, snow share](docs/media/terrain-climate.jpg)

*Climate maps: cold high ridges, rain over the range, shaded faces in winter, snow on the high crest.*

![A generated range with its ground material: rock, scree, soil and a sand beach](docs/media/terrain-ground.jpg)

*Ground material: bare rock on the ridges, soil and green in the valleys, a sand beach at the coast, all from the generator's geology.*

![Footprints and tracks in dry sand, damp sand, soil, mud and leaf litter, raster and path traced](docs/media/ground-footprints.jpg)

*Footprints: deepest in mud, shallow in dry sand where the walls slump.*

![Slope, curvature, flow, wetness and distance-to-water maps of a valley, and the valley coloured from them](docs/media/terrain-data-maps.jpg)

*Terrain data maps: the stream bed, wet ground and banks come from the maps alone.*

</details>

### Vegetation
- **Plant placement** (`VegetationField3D`, `Biome`, `PlantSpecies`): places species by spacing, footprint and rules on slope, wetness and distance to water over a `TerrainDataMap3D`; evenly spread, the same for a seed. 12,899 plants over 256 m place once in about 45 ms, then draw as ordinary MultiMeshes.
- **Placement by geology** (`TerrainDataMap3D.geology_source`; `PlantSpecies` `soil_depth_range`, `rock_range`, `talus_range`): plants follow the generated soil, rock and scree; trees stay off bare rock and thin soil, grass thins on rock.
- **Placement by climate** (`temperature_range`, `january_temperature_range`, `precipitation_range`, `aridity_range`, `sunlight_range`): species by temperature, winter frost, rain, aridity and sunlight.
- **Canopy shade** (`canopy_radius`, `shade_range`; `weather_map`): understorey placed by the shade of placed crowns; crowns feed the weather's canopy map.
- **Grass** (`GrassField3D`): blades grow round the camera in clumps, follow `TerrainDataMap3D` rules, thin with distance, bend in the `WeatherMap3D` wind and darken in rain; path traced.
- **Trampling** (`grass_trample` group, `GrassField3D.trample_recovery`): bodies lay grass down and it springs back.
- **Ground cover** (`GrassField3D` undercoat fields, `view_facing`; `PlantSpecies` patches): edge-on blades widened, ground tinted under grass, clover and flower drifts.
- **GPU-placed instances** (`RenderingServer.multimesh_mark_buffer_rd_changed()`): MultiMesh records written by a compute shader reach culling, motion vectors and path tracing.
- **Instance bones** (`MultiMesh.bones_per_instance`): each instance is skinned from its own records, so thousands of plants bend in the wind without a Skeleton3D each; raster and path traced. 900 swaying stems: 0.30 ms render CPU, against 5.97 ms as 900 Skeleton3D.

<details><summary>Images and clip: plant placement, grass, ground cover, grass in wind, trampling, GPU-placed instances, instance bones</summary>

![A valley with trees, bushes and reeds placed by biome rules, raster and path traced](docs/media/vegetation-placement.jpg)

*Plant placement: trees on dry ground, bushes between them, reeds along the banks (placeholder meshes).*

![A grass carpet with undercoat, clover and flowers, raster and path traced](docs/media/grass-field.jpg)

*Grass: a full carpet, raster and path traced.*

![Sparse grass before, then with ground cover](docs/media/grass-carpet.jpg)

*Ground cover: bare ground between blades is filled with undercoat, clover and flowers.*

![Grass bending as gusts travel downwind, raster](docs/media/grass-wind.gif)

*Grass in wind: gusts sweep across the field downwind.*

![A body walking through grass that lies down and springs back, raster](docs/media/grass-trample.gif)

*Trampling: the grass lies down under the body and springs back behind it.*

![GPU-written MultiMesh records, path traced: before and after the call](docs/media/gpu-multimesh-records.jpg)

*GPU-placed instances: without the call the GPU-written instances never appear.*

![Stems in rest pose, then bent by instance bones in one MultiMesh, path traced](docs/media/instance-bones.jpg)

*Instance bones: one MultiMesh, every stem bent by its own bones.*

</details>

### Weather
- **Weather map** (`WeatherMap3D`): one weather state round the camera (wind, cloud cover, rain, wetness, puddles), read by every shader through global uniforms, raster and path traced.
- **Wind**: slows near the ground (`roughness_length`); gusts follow the measured spectrum and travel with the wind.
- **Wind near the ground** (`add_canopy`, `add_shelter_box`, `gust_patch_size`): shelter inside plant canopies and behind buildings; gust patches sweep across fields.
- **Storms** (`add_storm`): drift with the wind, with a rain core and a downdraft.
- **Lightning and thunder** (`WeatherLightning3D`): strikes from storm cells at a rate set by cloud-top height, lit by their own channel under path tracing, with sharp shadows in raster too (`raster_shadow_distance`) and thunder made from the channel.
- **Precipitation haze** (`WeatherMap3D.set_precipitation_extinction()`): snow and rain haze the air in raster fog and the path traced medium; blowing snow is a lit layer near the ground.
- **Wet surfaces and puddles** (`surface_storage`, `puddle_drain_rate`, `temperature`, `humidity`): fill with rain and dry by evaporation; materials with Wetness on darken and gloss up.
- **Followers**: water waves grow with the weather's wind (`follow_weather`); cloth and hair move in it (`weather_wind`).

<details><summary>Images: weather map, wet ground, lightning, blizzard</summary>

![Weather map fields round a storm: cloud cover, rain, wind at 10 m, wetness](docs/media/weather-map.jpg)

*Weather map: a storm's cloud, rain core, wind and the wet ground it leaves.*

![A beach dry and after rain: darker sand, puddles and a sheen](docs/media/weather-wet.jpg)

*Wet surfaces: the sand darkens, puddles form and catch the sky.*

![A lightning strike among blocks, raster and path traced](docs/media/weather-lightning.jpg)

*Lightning: the channel lights the ground and casts sharp shadows from the blocks, raster and path traced.*

![A shelter in calm weather, then in a blizzard, raster and path traced](docs/media/weather-blizzard.jpg)

*Precipitation haze: in a blizzard the blowing snow hides the distance.*

</details>

### Clouds
- **Cloud layer** (`cloud_base_height`, `cloud_thickness`, `cloud_density`): shaped from the weather map's cover, cloud type and storms; drawn by `WeatherSkyMaterial` and seen by path traced rays (`pathtracing_sky_shader_enabled`).
- **Cloud reprojection** (`WeatherSkyMaterial.cloud_reprojection`, on by default): clouds draw a sixteenth of their pixels each frame and reproject the rest (Nubis), still full resolution.
- **Cloud shadows** (`sun_light`): the clouds' shadow is baked into the sun's projector, matching in raster and path tracing.
- **Sun projector**: `light_projector` works on DirectionalLight3D (`projector_size`, `projector_offset`), raster and path traced.

<details><summary>Images: clouds, cloud shadows</summary>

![A cloud layer over a field of blocks, path traced](docs/media/weather-clouds.jpg)

*Clouds: the layer seen from the ground, with its shadows on the field.*

![Cloud shadows moving across a field, seen from above, path traced](docs/media/weather-cloud-shadows.jpg)

*Cloud shadows: dark patches on the ground under the clouds.*

</details>

### Snow
- **Snow cover** (`SnowCover3D`): a snowpack from the weather map, with snowfall, settling, melt, sliding off steep slopes and wind drift.
- **Deep snow** (`SnowSurface3D`, `snow_deform` group): deep snow as geometry with prints and tracks; bodies sink by weight (Bekker) and rest in their own tracks; tracks refill with new snow and wind.
- **Falling snow** (`SnowFall3D`): falling and blowing snow from the weather; flake counts, sizes and fall speeds from snowfall measurements, drawn as motion streaks; roofs shelter, snow blows above the wind threshold, `get_extinction()` feeds fog.
- **Snow climate** (`SnowClimate`): bakes a winter over the whole terrain, with snow lines, shaded faces that keep their snow and loaded lee slopes; the cover carries it on with the weather and draws it far away.
- **Cornices** (`SnowCornice3D`): overhangs on lee crests, with collision; milder ranges grow them too (crest wind speed-up).
- **Snowstorms** (`SnowFall3D.haze_enabled`, `turbulence`): falling and blowing snow haze the air through the weather map (Pomeroy-Male suspension, 118 m visibility at 20 m/s); flakes ride turbulent eddies.
- **Shelter** (`SnowCover3D.canopy_enabled`, `shelter_collision_mask`): ground under roofs and overhangs stays bare; tree crowns hold snow and unload it later.
- **Melt-freeze crusts** (`SnowCover3D.crust_enabled`): a thawed surface refreezes into a crust that hardens each cycle; boots break thin ones into tipped pieces and stand on hard ones.
- **Sparkle**: dry snow glints from ice grains (its own mirror lobe, raster and path traced), lost when wet.
- **Snow on materials** (BaseMaterial3D `snow_enabled`): surfaces are covered from the snowpack, flattest facets first, raster and path traced; `get_snow_shader_code()` for ShaderMaterials.

<details><summary>Images and clips: snow on a mountain, snow on props, snowfall and melt, tracks, falling snow, snow climate, cornices, sparkle, shelter, crust</summary>

![Snow on a mountain, raster and path traced](docs/media/snow-mountain.jpg)

*Snow cover: deep on the cold summit, thinning to patches lower down.*

![Thin snow over the ground and props, raster and path traced](docs/media/snow-props.jpg)

*Thin snow: ground and props partly covered, raster and path traced match.*

![Time-lapse of snow building up and melting on a mountain](docs/media/snow-mountain.gif)

*Snowfall and melt: snow builds up down the slopes, then melts back to the summit.*

![Time-lapse of snow building up on props](docs/media/snow-props.gif)

*Snowfall on props: the ground covers first, then the flatter tops of the props.*

![Prints and tracks in deep snow, raster and path traced](docs/media/snow-tracks.jpg)

*Deep snow: prints and tracks behind the bodies, raster and path traced.*

![Bodies leaving prints and tracks in deep snow](docs/media/snow-tracks.gif)

*Tracks: bodies sink in by weight as they move.*

![Falling snow round a shelter at full size, raster and path traced](docs/media/snowfall.jpg)

*Falling snow: streaked flakes, none under the roof.*

![Snow falling and blowing round a shelter](docs/media/snowfall.gif)

*Falling snow in wind.*

![A peak after a baked winter, raster and path traced](docs/media/snow-climate.jpg)

*Snow climate: snow holds on the shaded and lee faces below the snow line.*

![A snow cornice overhanging a lee crest, raster and path traced](docs/media/snow-cornice.jpg)

*Cornice: a lip of snow over the lee side of a crest.*

![Snow surface close up: dry in raster, dry path traced, and wet](docs/media/snow-sparkle.jpg)

*Sparkle: dry snow glints in raster and path tracing; wet snow loses it.*

![A shelter and snow-laden trees, raster and path traced](docs/media/snow-shelter.jpg)

*Shelter: bare ground under the roof; tree crowns hold their snow.*

![Footprints in snow without a crust, then through a broken crust, raster and path traced](docs/media/snow-crust.jpg)

*Melt-freeze crust: boots break it into tipped plates instead of soft prints.*

</details>

### Exposure
- **Histogram metering** (`rendering/camera/auto_exposure/metering`): lamps in view no longer darken the scene.
- **Dark-scene key** (`dark_scene_luminance`): night stays night.
- **Local exposure** (`local_highlight_contrast`, `local_shadow_contrast`).

### DLSS Ray Reconstruction
- DLSS 4.5 (`rendering/streamline/dlss_ray_reconstruction_preset`).
- Continuous thin rain and correct blending over its output; frame generation fixes.

### Physics (PhysX 5)
![PhysX: cloth flag in wind, a Blast wall broken by a ball, rigid crates, Flow smoke](docs/media/physx.gif)

Windows x64 and Linux x86_64. Choose `PhysX` or `Jolt Physics` in **Project Settings > Physics > 3D > Physics Engine**.
- Either engine: tearable cloth, skinned cloth, Blast destruction, Flow smoke and fire, ocean buoyancy, compute-shader fluids.
- PhysX only: vehicles, CUDA fluids and deformables.
- **Path-traced smoke and fire** (`rendering/pathtracing/simulated_gas`).
- **Cloth tearing** (`PhysXCloth3D.tear_factor`, `physics/physx_3d/cloth/tearing`): overstretched edges split and the rip runs.

  <details><summary>Clip: a ribbon rips off its post and a flag frays</summary>

  ![Cloth tearing: a ribbon rips off its post and a flag frays in rising wind](docs/media/cloth-tearing.gif)

  </details>

- Builds without the SDKs skip the module (`modules/godot_physx/README.md`).

### Other
- Cached shadows, time-sliced reflection probes; `is_pathtracing_ready()`, video memory queries; leak, crash and device-fault fixes.

## Comparisons

Scenes: [docs/media/showcase](docs/media/showcase) and [docs/media/light-selection](docs/media/light-selection).

<details>
<summary>Glass shadows: NVIDIA-RTX/godot vs Berlenga</summary>

![Glass pane shadow: speckled on NVIDIA-RTX/godot, even and tinted on Berlenga](docs/media/glass-shadow.jpg)
</details>

<details>
<summary>Texture filtering: NVIDIA-RTX/godot, Berlenga, raster</summary>

![Checker floor: moire on NVIDIA-RTX/godot, filtered on Berlenga, raster for reference](docs/media/texture-filtering.jpg)
</details>

<details>
<summary>Emissive lights off vs on</summary>

![A pavement lit by a glowing window: dark and speckled with the setting off, evenly lit with it on](docs/media/emissive-lights.jpg)

Look at the pavement under the window.
</details>

<details>
<summary>Light visibility memory off vs on</summary>

![A street among buildings with lamps hidden inside them: noisy with the setting off, much cleaner with it on](docs/media/light-visibility-memory.jpg)
</details>

<details>
<summary>Radiance cache off vs on, equal time</summary>

![The same street at 4 bounces and equal render time: 4 samples per pixel with the cache off, 6 with it on](docs/media/radiance-cache.jpg)
</details>

<details>
<summary>PhysX scene path traced</summary>

![The PhysX scene path traced at 64 samples per pixel](docs/media/physx-pathtraced.webp)
</details>

<details>
<summary>65 km from the origin: absolute vs camera-relative</summary>

![A cube, a mirror ball and a lamp 65 km from the origin: speckled cube and a broken reflection with absolute positions, correct with camera-relative positions](docs/media/far-origin.jpg)
</details>

<details>
<summary>Exposure: average, histogram, dark-scene key</summary>

![A night street with lamp posts: dark with average metering, readable with histogram metering, darker again with the dark-scene key](docs/media/night-exposure.jpg)
</details>

<details>
<summary>Local exposure off vs on</summary>

![The night street: hot spots on lit walls and black facades without local exposure, detail kept with it](docs/media/local-exposure.jpg)
</details>

## Open worlds

Target: tens of thousands of objects over kilometres, mostly static.

- **Camera-relative tracing** (`rendering/pathtracing/relative_origin_grid`): exact shadows and reflections far from the origin.
- **Persistent instances** (`rendering/pathtracing/persistent_instances`): unchanged objects and MultiMeshes cost no per-frame work.
- **Off-screen culling** (`rendering/pathtracing/cull_max_distance`, `cull_min_size`): small or distant unseen objects leave the trace.
- **Opacity micromaps** (`rendering/pathtracing/opacity_micromaps`): cut-out foliage traced without any-hit shaders.
- **Mesh LOD in the path tracer** (`rendering/pathtracing/mesh_lod`).

Settings for a large world, in `project.godot`:

```ini
[rendering]
pathtracing/relative_origin_grid=1024.0
pathtracing/persistent_instances=true
pathtracing/cull_max_distance=500.0
pathtracing/cull_min_size=0.1
pathtracing/opacity_micromaps=true
```

Test scene (2 km, prints timing and memory):

```sh
godot --path docs/bench/open_world --gpu-profile -- --count=100000 --set=rendering/pathtracing/persistent_instances:1
```

Options: `--movers=N`, `--churn=N`, `--recolor`, `--fly=S`, `--multimesh=M` (MultiMesh tiles of M meters), `--mmmove=N`, `--raster`, `--out=frame.png`. Design and measurements: [open-world geometry](docs/ideas/open-world-geometry.md), [persistent instances](docs/ideas/persistent-instances.md), [instanced forests](docs/ideas/instanced-forests.md), [opacity micromaps](docs/ideas/opacity-micromaps.md).

## References

Code: [NVIDIA-RTX/godot](https://github.com/NVIDIA-RTX/godot) (path tracer, DLSS); [uno1982/godot](https://github.com/uno1982/godot) `feature/physx5-module` (Wild Ox Studios, MIT); [NVIDIA-Omniverse/PhysX](https://github.com/NVIDIA-Omniverse/PhysX) (BSD-3-Clause); [GPUOpen-Effects/TressFX](https://github.com/GPUOpen-Effects/TressFX) (AMD, MIT; strand hair simulation and rendering); [valence-engine-rtx](https://github.com/Rman887/valence-engine-rtx) (camera-relative tracing, ray offset, persistent instance slots, opacity micromap bake). Papers: [docs/REFERENCES.md](docs/REFERENCES.md). Research notes: [docs/ideas](docs/ideas).

## License

MIT, like Godot. Third-party licenses in `thirdparty/` and `COPYRIGHT.txt`; NVIDIA binaries under NVIDIA's terms. Build and docs: [godotengine/godot](https://github.com/godotengine/godot), [docs.godotengine.org](https://docs.godotengine.org).

Berlenga is an independent fork and is not affiliated with or endorsed by the Godot Foundation.
