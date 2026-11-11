---
title: "DLSS Ray Reconstruction"
weight: 110
summary: "DLSS 4.5 Ray Reconstruction; pixel antialiasing when it is off."
---

- DLSS 4.5 (`rendering/streamline/dlss_ray_reconstruction_preset`).
- Continuous thin rain and correct blending over its output; frame generation fixes.
- **Pixel antialiasing** when Ray Reconstruction is off: samples after the first spread over the pixel (R2 sequence), so sub-pixel detail antialiases instead of sparkling.

## Images

![Path traced strand hair: sparkling at 256 spp before, smooth at 64 spp with pixel antialiasing](media/pixel-aa.jpg)

*Pixel antialiasing: sub-pixel strands sparkled even at 256 samples; now 64 resolve smoothly.*
