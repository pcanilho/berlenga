---
title: "Comparisons"
weight: 30
---

Scenes: [docs/media/showcase](https://github.com/pcanilho/berlenga/tree/main/docs/media/showcase) and [docs/media/light-selection](https://github.com/pcanilho/berlenga/tree/main/docs/media/light-selection).

## Glass shadows: NVIDIA-RTX/godot vs Berlenga

![Glass pane shadow: speckled on NVIDIA-RTX/godot, even and tinted on Berlenga](media/glass-shadow.jpg)

## Texture filtering: NVIDIA-RTX/godot, Berlenga, raster

![Checker floor: moire on NVIDIA-RTX/godot, filtered on Berlenga, raster for reference](media/texture-filtering.jpg)

## Emissive lights off vs on

![A pavement lit by a glowing window: dark and speckled with the setting off, evenly lit with it on](media/emissive-lights.jpg)

Look at the pavement under the window.

## Light visibility memory off vs on

![A street among buildings with lamps hidden inside them: noisy with the setting off, much cleaner with it on](media/light-visibility-memory.jpg)

## Radiance cache off vs on, equal time

![The same street at 4 bounces and equal render time: 4 samples per pixel with the cache off, 6 with it on](media/radiance-cache.jpg)

## PhysX scene path traced

![The PhysX scene path traced at 64 samples per pixel](media/physx-pathtraced.webp)

## 65 km from the origin: absolute vs camera-relative

![A cube, a mirror ball and a lamp 65 km from the origin: speckled cube and a broken reflection with absolute positions, correct with camera-relative positions](media/far-origin.jpg)

## Exposure: average, histogram, dark-scene key

![A night street with lamp posts: dark with average metering, readable with histogram metering, darker again with the dark-scene key](media/night-exposure.jpg)

## Local exposure off vs on

![The night street: hot spots on lit walls and black facades without local exposure, detail kept with it](media/local-exposure.jpg)
