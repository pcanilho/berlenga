# Quality levels and memory budgets

Research note, 22 Nov 2026. One engine-wide quality ladder (Low, Medium, High, Ultra) that every
quality knob hangs from, and the two systems under it that need new engine work: texture streaming
by a video memory budget, and mesh detail by level. First user: a game that today makes its own
2K and 1K texture copies and its own detail levels in script. The same ladder has to carry an
open world, so everything here is sized for that.

## The gap

What a game has to do on its own today:

- **Smaller textures for lower settings.** Godot keeps every texture whole in video memory, so a
  mipmap bias saves no memory. The game ships 2K and 1K copies of every 4K prop and character map,
  and a `ResourceFormatLoader` in front of Godot's loaders swaps in a copy by setting, applied at the
  next start. Measured on the game's own scenes: every model at 4K used 8.1 GB raster and 9.5 GB
  path traced, too much for 8 GB cards. The copies cost disk, import time and a restart, and they
  drop detail everywhere, even on the one object the camera is next to.
- **Detail levels.** The game sets `Viewport.mesh_lod_threshold` (4, 1, 0 px) and
  `texture_mipmap_bias` per level, and reads video memory through the fork's
  `RenderingDevice.get_video_memory_usage()` / `get_video_memory_budget()` to warn near the limit.
- **Every other knob** (shadows, volumetrics, vegetation density, path tracer samples and bounces)
  is a separate project setting or node property. There is no single place where "Medium" is
  defined, so each game rebuilds the table, and a new engine feature has no way to say how it
  scales.

What the fork already has, which the design builds on:

- **Path tracer mesh LOD** (`rendering/pathtracing/mesh_lod`, default off): the TLAS builder picks
  the same LOD per surface as raster does (`RTTlasBuilder::_surface_lod`, same distance, model
  scale, bias and `screen_mesh_lod_threshold`), with one shared BLAS per (surface, LOD) and eviction
  of unused levels. So raster and path tracing already agree on geometry when it is on.
- **Ray cone texture LOD** in the path tracer (Akenine-Moller et al., Ray Tracing Gems 2019 ch. 20;
  JCGT 2021): closest hits sample a mip from the cone's footprint, with the render buffers' own
  samplers and bias, so raster and path tracing read matching mips.
- **Bindless block** for the path tracer: when a texture is destroyed or recreated the uniform set
  goes invalid, `BindlessBlock::begin_frame()` scrubs dead entries and bumps a version, and
  materials re-resolve their indices. A texture swapped for one with fewer mips is picked up the
  next frame without new plumbing.
- **Video memory budget query** (`VK_EXT_memory_budget`, fork #61/#62).

## How others do it

### Quality ladders

- **Unreal Engine scalability groups.** `sg.ViewDistanceQuality`, `sg.AntiAliasingQuality`,
  `sg.ShadowQuality`, `sg.GlobalIlluminationQuality`, `sg.ReflectionQuality`,
  `sg.PostProcessQuality`, `sg.TextureQuality`, `sg.EffectsQuality`, `sg.FoliageQuality`,
  `sg.ShadingQuality`, each 0 (Low) to 3 (Epic), 4 (Cinematic). A group is a named section per level
  in `Scalability.ini` that sets a list of console variables; a project overrides the sections, and
  device profiles pick defaults per hardware. Example, `sg.TextureQuality`: 0 sets
  `r.Streaming.MipBias=2.5`, `r.MaxAnisotropy=0`, `r.Streaming.PoolSize=200`; 3 sets 0, 8, 1000.
  An overall level sets every group; the user may then move one group alone. Resolution is its
  own setting (`r.ScreenPercentage`), not a group.
- **Unity** keeps Quality Levels (assets holding shadow, LOD bias, texture mip limit, streaming
  budget, anisotropy) plus per-pipeline assets in HDRP/URP. Same shape: a named level is a bundle
  of settings, and features read from the bundle.
- **Godot** has no ladder. Quality is a set of independent project settings
  (`rendering/lights_and_shadows/...`, `rendering/textures/default_filters/...`, etc.) and
  per-feature `.mobile` overrides.

### Texture streaming

- **Unreal texture streamer.** Each frame an async task works out a wanted resolution per texture
  from texel-to-pixel ratio: per-primitive bounds and per-material UV density, built offline
  ("Build Texture Streaming"), against the view. A fixed pool (`r.Streaming.PoolSize`, or the card's
  VRAM with `r.Streaming.LimitPoolSizeToVRAM`) caps residency. When the wanted set is over budget it
  drops one mip at a time, in this order: off-screen mips first, least recently seen first; keeps
  landscape, forced, character and small textures; keeps on-screen mips last. Loads go visible
  first. `r.Streaming.MipBias` lowers everything for low tiers; `sg.TextureQuality` drives it.
- **id Tech (Rage, Doom Eternal) and Far Cry / Frostbite terrain: virtual texturing.** Pages
  (typically 128x128 texels) in a physical cache, a page table per texture, a GPU feedback pass
  that writes wanted pages, CPU loads, the shader indirects through the page table. Fine-grained
  (only the visible part of a mip), but every sampler goes through an indirection and the
  feedback pass. UE's Runtime Virtual Texture and Streaming Virtual Texture are the same idea.
- **Sampler feedback streaming** (D3D12 Ultimate, Intel's SFS sample, GDC 2021). Reserved (tiled)
  textures; the hardware writes a MinMip feedback map per texture with `WriteSamplerFeedback`, the
  app maps tiles and writes a MinMip clamp map the shader reads. Needs tiled resources tier 2. No
  Vulkan equivalent; Vulkan has sparse residency and `VK_EXT_image_view_min_lod`.
- **Godot 4.8 mip-level texture streaming** (upstream PR #113429, T. Davenport, in 4.8 dev 5,
  Sep 2026; proposal godot-proposals #3177). A new importer ("Texture2D Streamed",
  `StreamedTexture2D`) stores mips so each can be loaded alone. Materials with a streamed texture
  write the mip they used into a feedback buffer from the forward shaders; the CPU reads it back
  asynchronously, compares with what is loaded and reloads the texture with more or fewer mips
  through ordinary `texture_2d_create` (no sparse textures; the uniform set is rebuilt).
  Project settings `rendering/textures/streaming/enabled`, `memory_budget_mb`, `min_lod`,
  `max_lod`; per-texture overrides; a `TextureStreaming` singleton with `memory_budget_mb_override`,
  `min_lod_override`, `max_lod_override`, `get_memory_budget_bytes_used()`, `flush_texture_streaming()`.
  Eviction by recency and screen coverage. Forward+ and Mobile only. Known gaps from review: no
  3D textures or lightmaps, uploads bypass the RD upload queues, no "keep loaded near the player"
  radius, the first load can show a blurry mip, all spatial shaders carry the feedback uniform while
  a `sampler2DStreamed` type is pending. Not in the fork's base yet.

### What none of them cover: the path tracer

Feedback from raster shaders only sees what the camera sees directly. A path tracer also reads
textures in reflections, refractions, bounce lighting and shadows of alpha-tested leaves, often on
objects behind the camera. With raster-only feedback those textures fall to their smallest mips
and the path-traced picture loses detail the raster one keeps (a mirror shows a blurred poster,
a sunlit wall bounces the wrong colour from a far mip). That breaks this fork's rule that path
tracing must not remove detail. The fix is to let the path tracer's closest hits write the same
feedback: the ray cone already gives the exact mip each hit samples, so the feedback write is one
`atomicMin` per textured hit into the same buffer.

## Design

### 1. The ladder (`RenderingServer` quality levels)

- `rendering/quality/level`: Low, Medium, High, Ultra (Ultra default, so a project that ignores the
  ladder behaves as today: full assets everywhere). Runtime: `RenderingServer.quality_set_level()`,
  and per group `quality_set_group_level(group, level)`, as UE's overall level then per-group moves.
- **Groups**, each a level of its own: textures, geometry, shadows, global illumination and path
  tracing, reflections, volumetrics and weather, vegetation, effects, post. View distance stays in
  geometry; render resolution stays outside (the upscaler's own setting), as in UE.
- **Each feature registers how it scales.** A group level is a table of existing project settings
  and their values, kept in one engine file and overridable per project under
  `rendering/quality/groups/<group>/<level>/...`. A new fork feature adds its rows there instead of
  inventing its own tier. Ultra rows are always the shipped defaults, so Ultra changes nothing.
- The ladder only writes settings that already exist, so every system stays testable alone, and
  the shared link has one written contract: provider `RenderingServer`, readers each feature, units
  those of the setting, default Ultra. It goes into world-links' matrix.
- A game's Options menu maps onto it directly: one overall level, then a row per group.

Under path tracing the PT group decides samples and bounces; raster-only groups (SSAO, SSR) are
ignored, as today.

### 2. Textures: budget streaming with path tracer feedback

1. **Bring in upstream mip-level streaming** (#113429 and its follow-ups) when the base moves past
   4.8 dev 5, or cherry-pick it now. It is the established Godot design and keeps the fork close to
   upstream for both games.
2. **Path tracer feedback.** Closest hits that sample a streamed texture write the ray cone's mip
   into the same feedback buffer (one buffer, raster and path tracer both write; atomicMin keeps the
   sharpest). Any-hit (alpha test) writes the mip it reads too. Under path tracing the streamer
   therefore keeps every mip any path needs, not only the camera's.
3. **Budget only drops invisible mips.** The budget is a target, not a hard cut: a mip that any
   feedback asked for in the last few frames is never dropped to meet it. Over budget, drop in UE's
   order: textures nobody sampled, then least recently seen, then off-screen. If the visible set
   alone is over the card's real limit (`VK_EXT_memory_budget`), the engine warns once, as UE's
   "over budget" stat, instead of blurring what is on screen.
4. **Pin and preload.** A near radius around the camera keeps full mips (the follow-up reviewers
   asked for), and a scene can preload (no first-frame blur for cut scenes and stills; still
   renders wait for streaming, as they wait for `RENDER_INFO_RT_PENDING_SURFACES`).
5. **Uploads through the RD upload queue** and swaps between frames, so neither raster nor the
   bindless block ever sees a half-written texture.
6. **The textures group** sets `max_lod` (the largest mip allowed) and the budget: Ultra no cap
   (full 4K, and 8K maps where a project ships them, such as character close-ups); High drops 8K to
   4K; Medium caps at 2K; Low at 1K. One shipped texture serves every level, so the game's 2K and
   1K copies and its custom loader go away, and a change of level applies without a restart.

Cost: the feedback write is one atomic per textured sample in raster (upstream measured it as
cheap; spec-constant off when streaming is off) and per textured closest hit in path tracing. The
readback is asynchronous. Memory saved: everything below the wanted mip, which on a 4K map seen
from a few metres is three quarters of it or more.

### 3. Geometry

- **Turn `rendering/pathtracing/mesh_lod` on by default.** With it off, path tracing always traces
  the full mesh while raster may draw a LOD, so the two pictures differ on purpose. On, both use
  the same LOD from the same threshold, and at Ultra (threshold 0) both draw the full mesh at every
  distance. Before flipping it: raster/PT 100% crops at the default 1 px threshold on the rocks and
  trees benches, since reflections can see an object closer than the camera does. If they show
  any loss, the PT LOD uses the ray cone's footprint at the hit instead of camera distance (the
  same rule as textures).
- **The geometry group** sets `mesh_lod_threshold` (Ultra 0, High 1, Medium 2, Low 4), the
  LOD distance multiplier, vegetation impostor and dissolve distances, and view distance.
- **Mesh streaming** (resident LODs by budget, cluster streaming) stays on the open-world geometry
  plan (open-world-geometry.md steps 4 to 6). The ladder gives it its budget when it lands.

## Order

1. Ladder skeleton: groups, levels, table, `quality_set_level`, Options mapping in a generic test
   scene; Ultra = today's defaults, proven byte-identical raster and PT frames.
2. Geometry group, and path tracer mesh LOD on by default after the crop check.
3. Upstream texture streaming into the fork (Forward+, Vulkan first; D3D12 later).
4. Path tracer feedback and the visibility-safe budget; bench `docs/bench/texture_budget`: a room
   with 4K and 8K maps, mirror and bounce views of objects behind the camera, reports wanted mips,
   resident bytes, raster and PT frames at each level.
5. Remaining groups (shadows, volumetrics, vegetation, PT samples and bounces, effects), one at a
   time, each with its rows and a frame pair per level.

## Sources

- Unreal Engine docs: Texture Streaming Overview, Texture Streaming Configuration, Scalability
  Reference (dev.epicgames.com, UE 5.x).
- Godot PR #113429 "Implement texture mip-level streaming", godot-proposals #3177, `TextureStreaming`
  class reference, Godot 4.8 dev 5 announcement (10 Sep 2026).
- Microsoft DirectX spec: Sampler Feedback; Intel GDC 2021 "Sampler Feedback Streaming".
- J. M. P. van Waveren, "id Tech 5 Challenges: From Texture Virtualization to Massive Parallelization",
  SIGGRAPH 2009; Barrett, "Sparse Virtual Textures", GDC 2008.
- Akenine-Moller et al., "Texture Level of Detail Strategies for Real-Time Ray Tracing", Ray Tracing
  Gems 2019, ch. 20; "Improved Shader and Texture Level of Detail Using Ray Cones", JCGT 2021.
