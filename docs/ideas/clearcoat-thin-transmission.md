# Clearcoat and thin transmission in the path tracer

## What the path tracer does today

- **Clearcoat.** `CLEARCOAT` and `CLEARCOAT_ROUGHNESS` are parsed in custom shaders
  (`scene_shader_raytracing.cpp`) and then dropped. The material record has no coat fields, and
  BaseMaterial3D's Clearcoat section is ignored. A coated car under path tracing looks like bare
  paint, while the rasterizer shows the lacquer.
- **Back lighting.** Custom shaders' `BACKLIGHT` reaches direct light only: `evalCombinedBRDFSeparate`
  adds `backlight * (1/pi - diffuse)` for lights behind the surface. Bounce rays never pass
  through, and BaseMaterial3D's Back Lighting is not read at all (`m.backlight = vec3(0.0)` with a
  TODO in the standard hit group).
- **See-through surfaces.** Glass is alpha blending. Transparency peeling traces the layers
  behind and blends them by alpha. That blend is the same at every angle: a window does not get
  more reflective at grazing angles, and transmitted light is not tinted or blurred by roughness.

## How others do it

### Clearcoat

- **Godot's rasterizer**, following Filament. The coat is a second GGX lobe with fixed IOR 1.5
  (F0 4%) on the vertex normal, so normal maps don't reach it. Its roughness is mapped to
  [0.001, 0.1] and it uses the Kelemen visibility term. The base layer is attenuated by
  `1 - Fc * clearcoat`, Fresnel only, and the base F0 is re-derived for a 1.5 to 1.5 interface
  (`f0_Clear_Coat_To_Surface`).
- **OpenPBR Surface** (ASWF, 2024): the coat is a dielectric layer (default IOR 1.6) over the
  base. Light through it is attenuated by the coat's directional albedo, and the coat can darken
  the base through internal reflection. Albedo scaling (Kulla and Conty 2017; Weidlich and
  Wilkie 2007) is the standard practical approximation of the layering.
- **Disney principled BRDF** (Burley 2012, 2015): the clearcoat is a fixed GTR1 lobe with F0 4%,
  added on top.
- **Falcor and RTXPT** (NVIDIA): StandardBSDF picks one lobe per bounce with probabilities from
  the lobes' estimated albedos, and weights the base by what the upper lobe lets through.

Plan, matching Godot's raster model so a material looks the same in both renderers:

1. **Material record.** It gains `clearcoat` and `clearcoat_roughness`, read from
   BaseMaterial3D and from custom shaders' `CLEARCOAT` and `CLEARCOAT_ROUGHNESS`.
2. **Direct light.** Coat specular comes from GGX on the geometry normal, with roughness mapped
   to [0.001, 0.1] as in raster, Kelemen visibility, and F0 0.04. Base diffuse and specular are
   multiplied by `1 - Fc`, and the base F0 is converted as in raster.
3. **Bounce rays.** A third lobe. The coat is picked with probability
   `clearcoat * F(NdotV)`, then the base lobes are picked as now. A coat sample uses VNDF
   sampling, and its weight is F times the Smith G2/G1 ratio: the exact estimator for GGX, where
   raster's Kelemen term is its approximation. A base sample's weight carries `1 - Fc(NdotV)`.
4. **Light-sample probabilities.** The emissive and sky light samples' MIS density includes the
   coat lobe's probability.
5. **Ray Reconstruction.** The specular albedo guide adds the coat's environment albedo, and the
   specular hit distance follows the coat when the coat lobe was picked.

### Thin transmission

- **glTF.** `KHR_materials_transmission` without `KHR_materials_volume` is a thin-walled
  dielectric: light passes straight through, tinted by base colour, Fresnel-weighted, and blurred
  by roughness. `KHR_materials_diffuse_transmission` (2024) is the diffuse-through lobe used for
  leaves and paper.
- **OpenPBR** `geometry_thin_walled`, **Autodesk Standard Surface** `thin_walled`, and
  **Disney 2015** (`specTrans`, `diffTrans`, thin flag) all give the same pair: specular
  transmission without refraction offset, plus diffuse transmission.
- **Falcor and RTXPT** StandardBSDF: `specularTransmission`, `diffuseTransmission` and
  `isThinSurface`. A thin specular transmission sample reflects the sampled direction through
  the surface plane, so rough glass blurs without bending the view.

Plan:

1. **Diffuse transmission from Back Lighting.** BaseMaterial3D's Back Lighting and custom
   `BACKLIGHT` become a diffuse lobe on the far side. They already shade direct light that way;
   this adds bounce rays sampled through the surface and the standard hit group's missing read.
   Leaves, curtains, paper and lampshades pass light both ways.
2. **Specular thin transmission for glass.** What drives it is an open choice, because Godot 4
   has no transmission parameter. Options:
   - **(a) Path-tracing reading of alpha glass.** An alpha-blended surface that is not metal
     and not unshaded traces as a thin dielectric: transmittance `(1 - alpha)`, scaled by
     `1 - F` at the hit angle, with roughness blurring it. No new properties, and existing glass
     changes when the setting is on.
   - **(b) New BaseMaterial3D Transmission section**, like glTF (`transmission`, colour from
     albedo). The rasterizer keeps showing alpha. The glTF importer could fill it in. It adds
     engine API.
   - Recommendation: (a) first, behind a setting. It works for every existing scene and needs
     no new properties; (b) can follow if materials need glass that is opaque in raster.
3. **Shadow rays through thin surfaces** carry the same transmittance, so leaves and windows
   cast tinted, partial shadows. Opacity micromaps and any-hit stay as they are for cutouts.

## Clearcoat as built

Steps 1 to 5 above, with one change to step 3: the coat lobe's probability is its Fresnel against
the base's albedo under it (Falcor's albedo-based lobe choice), not its Fresnel alone. With Fresnel
alone, a few percent of rays reflected bright sky off dark paint and showed as speckle; a dark base
now sends most rays to the coat. The coat is stored in the material record next to the proximity
fade distance, which moved to fp16. BaseMaterial3D's clearcoat texture is not read.

`docs/bench/materials` renders the coat rows; at 512 samples the path tracer matches the
rasterizer, and coats off gives the old look.

## Back lighting as built

Plan step 1. BaseMaterial3D's back light colour is stored sRGB in three spare bytes of the material
record (the texture is not read). Bounce rays pass through the surface as Lambertian transmission
with reflectance albedo times back light, picked by its share of the base's light, and the emissive
light sample's density includes it. Direct light keeps raster's term. Shadow rays for lights behind
the surface now start on its back side: from the front they hit the surface itself, so `BACKLIGHT`
from lights behind never reached opaque cards before. Glass follows as option (b), a Transmission
section (Paulo's choice, 9 Nov).

## Transmission section (option b) design

Godot already has the raster half: BaseMaterial3D's Refraction reads the screen behind the
surface, adds it as emission (`ref_amount = 1 - alpha`, blurred by `ROUGHNESS * 8` mip levels),
scales albedo by `1 - ref_amount` and draws the surface with alpha 1. The path tracer ignores it,
so refractive glass traces opaque today. The Transmission section reuses that path:

- **Properties.** `FEATURE_TRANSMISSION` (appended, so saved feature values keep their meaning),
  `transmission` (0 to 1) and `transmission_texture` (red channel), as glTF
  `KHR_materials_transmission`. Thin-walled: no refraction offset unless Refraction is also on.
- **Raster.** The screen behind, at the refraction UV when Refraction is on, tinted by albedo and
  scaled by `transmission * (1 - metallic) * (1 - F(NdotV))` with the material's dielectric F0,
  is added to emission; albedo is scaled by `1 - transmission`. Depth draw always, like Refraction.
- **Path tracer.** A thin dielectric lobe (Falcor / RTXPT `isThinSurface`): a GGX VNDF half vector,
  the reflection direction mirrored through the surface plane, weight `(1 - F) * G2 / G1 * albedo *
  transmission`. Diffuse is scaled by `1 - transmission`. The lobe is picked by its share of the
  surface's light. Direct light does not use the lobe (a thin smooth pane passes a light as a
  delta), and a ray through it weighs emission it finds in full. Refraction-enabled materials take
  the same lobe with `1 - alpha` as their transmission, so existing refractive glass stops tracing
  opaque.
- **Shadows.** Shadow rays pass transmissive surfaces with transmittance `albedo * transmission *
  (1 - F0)`, through the any-hit path tinted glass shadows already use.
- **Back-lit shadows** (Paulo, 9 Nov: "I want our engine being capable of that"). Opaque
  back-lit BaseMaterial3D surfaces let shadow rays keep albedo times back light: all the light
  their diffuse transmission sends on, taken straight through. A sunlit curtain or leaf canopy
  then lets coloured light into its shadow. Under `rendering/pathtracing/back_lighting`.
- **glTF.** The importer sets the section from `KHR_materials_transmission` and the exporter
  writes it.
- **Setting.** `rendering/pathtracing/transmission` (on; off traces these surfaces opaque, as before).

## Layer textures and shader TRANSMISSION as built

- **Textures.** The material record grew from 128 to 144 bytes to hold three more bindless indices:
  `texture_clearcoat` (red scales the coat, green its roughness), `texture_backlight` (added to the
  back light colour) and `texture_transmission` (red scales transmission), each read as raster
  reads it. Camera, bounce and shadow rays all use them, so a checker back light or transmission
  texture gives a checkered coloured shadow.
- **Shaders.** `TRANSMISSION` is a new fragment output (float, 0 to 1): the path tracer traces it
  as BaseMaterial3D's Transmission. The rasterizer ignores it; a shader wanting raster glass reads
  the screen behind itself, as BaseMaterial3D does. Shaders writing `TRANSMISSION` or `BACKLIGHT`
  get their own any-hit, which runs the fragment for shadow rays, so their shadows pass the light
  too. Ray-query shadows (volumetric fog) still treat them as opaque, as they cannot run the
  shader.
- **Fix.** `render_mode rt_force_opaque` was only read from a shader's RT variant, so a
  BaseMaterial3D with Transmission or Refraction (no RT variant) still traced alpha-blended:
  camera rays saw through it, but shadow rays were stopped by its alpha of 1. It is now read from
  the shader itself, and glass shadows pass the tinted light.

## Settings

- `rendering/pathtracing/clearcoat` (on; off = today's look).
- `rendering/pathtracing/thin_transmission` (off at first; off = today's alpha blend and
  direct-only back lighting).

Both are checked in cloud renders: a coated sphere row (roughness by clearcoat) and a backlit
leaf card plus a window pane at several angles. Rasterizer and path tracer are compared side by
side, and RR is checked on the PC later.

## Sources

- Godot `scene_forward_lights_inc.glsl` and `scene_forward_clustered.glsl` (clearcoat); Filament
  documentation, "Clear coat model".
- OpenPBR Surface specification v1.1, Academy Software Foundation, 2024.
- B. Burley, "Physically Based Shading at Disney", 2012; "Extending the Disney BRDF to a BSDF
  with Integrated Subsurface Scattering", 2015.
- C. Kulla, A. Conty, "Revisiting Physically Based Shading at Imageworks", 2017.
- A. Weidlich, A. Wilkie, "Arbitrarily Layered Micro-Facet Surfaces", 2007.
- E. Heitz, "Sampling the GGX Distribution of Visible Normals", 2018.
- Khronos glTF `KHR_materials_transmission`, `KHR_materials_volume`,
  `KHR_materials_diffuse_transmission`.
- NVIDIA Falcor / RTXPT StandardBSDF.
- Autodesk Standard Surface specification.
