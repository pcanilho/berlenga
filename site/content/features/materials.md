---
title: "Materials"
weight: 20
summary: "Clearcoat, back lighting, thin-glass transmission, subsurface scattering; textures filter like raster."
---

- **Back lighting** (`rendering/pathtracing/back_lighting`): light through leaves, paper and curtains, with coloured shadows; back light texture.

  ![Back-lit leaf cards and a red curtain, back lighting on and off](media/back-lighting.jpg)

  *Back lighting: green and red light on the floor.*

  ![Checkered back light and transmission textures, raster vs path traced](media/layer-textures.jpg)

  *Layer textures: checkered coloured shadows.*

- **Transmission** (BaseMaterial3D section, `rendering/pathtracing/transmission`): thin glass, tinted and blurred by roughness, with tinted shadows; transmission texture; `TRANSMISSION` in shaders; glTF `KHR_materials_transmission`.

  ![Glass panes before coloured spheres: raster, path traced with transmission on, and off](media/transmission.jpg)

  *Transmission: the spheres show through and the floor gets green-tinted light.*

- **Raster refraction through solids** (`transmission_thickness`, glTF `KHR_materials_volume` thicknessFactor): raster refraction leaves a solid through its far side, not just the front face.

- **Clearcoat** (`rendering/pathtracing/clearcoat`): matches raster, with traced coat reflections; coat texture.

  ![Red coated spheres: raster, path traced with clearcoat on, and off](media/clearcoat.jpg)

  *Clearcoat: sharp coat highlights, as in raster.*

- **Subsurface scattering** (`rendering/pathtracing/subsurface_scattering`, radius `subsurface_scattering_scale`): random walk inside closed meshes; skin, wax and marble soften and thin parts glow when backlit.

  ![Backlit skin slabs 2 to 20 cm thick, subsurface scattering off and on](media/subsurface.jpg)

  *Subsurface scattering: thin slabs glow, thick ones stay dark.*

- **Wet surfaces** (BaseMaterial3D Wetness: `wetness_enabled`, `wetness_porosity`): porous surfaces darken and turn glossier as the weather map gets wet, raster and path traced; `get_wetness_shader_code()` for ShaderMaterials.

  ![Spheres of rising porosity, dry and wet, raster and path traced](media/wet-materials.jpg)

  *Wet surfaces: the more porous, the darker and glossier when wet; raster and path traced match.*

- Shaders writing `BACKLIGHT` or `TRANSMISSION` tint their shadows.
- Textures filter like raster (ray cones, anisotropic).
- Even, tinted glass shadows.
- Back-face normals in `vertex()`; `IN_SHADOW_PASS` in custom shaders; `SPECULAR_NORMAL` output, a separate normal for the specular lobe under path tracing.
