# <img src="misc/dist/linux/berlenga.svg" width="32" height="32" alt=""> Berlenga

[![Release](https://github.com/pcanilho/berlenga/actions/workflows/release.yml/badge.svg)](https://github.com/pcanilho/berlenga/actions/workflows/release.yml) [![CodeQL](https://github.com/pcanilho/berlenga/actions/workflows/codeql.yml/badge.svg)](https://github.com/pcanilho/berlenga/actions/workflows/codeql.yml) [![Latest release](https://img.shields.io/github/v/release/pcanilho/berlenga?label=latest)](https://github.com/pcanilho/berlenga/releases/latest)

Berlenga is a Godot 4 fork with Vulkan path tracing and DLSS Ray Reconstruction, built for large open worlds. Based on [NVIDIA-RTX/godot](https://github.com/NVIDIA-RTX/godot) (2 November 2026).

**[Download](https://github.com/pcanilho/berlenga/releases/latest):** Windows (Ray Reconstruction: add NVIDIA's DLSS, Streamline and Reflex DLLs next to the exe), Linux x86_64/arm64 (no Ray Reconstruction), macOS (raster only, no PhysX); export templates for Windows and Linux with PhysX (install the `.tpz` from Manage Export Templates). **Docs:** [berlenga.canilho.net](https://berlenga.canilho.net/). **Changes:** [CHANGELOG.md](CHANGELOG.md).

![1024 street lamps: NVIDIA-RTX/godot on the left keeps 64 of them, Berlenga on the right lights them all](docs/media/many-lights.jpg)

*1024 lights, 16 samples per pixel. Left: NVIDIA-RTX/godot. Right: Berlenga. Images are raw path tracer output, mostly CPU-rendered, without denoiser or Ray Reconstruction.*

## At a glance

- **[Lights](https://berlenga.canilho.net/features/lighting/)**: 4096 path-traced lights (upstream 64); emissive surfaces count as lights.
- **[Faster convergence](https://berlenga.canilho.net/features/lighting/)**: light visibility memory and a radiance cache.
- **[Materials](https://berlenga.canilho.net/features/materials/)**: clearcoat, back lighting, thin-glass transmission, subsurface scattering; textures filter like raster.
- **[Hair](https://berlenga.canilho.net/features/hair/)**: fibre shading, strand grooms, TressFX strand simulation with body collision.
- **[Terrain](https://berlenga.canilho.net/features/terrain/)**: mountain ranges raised and eroded per seed, with geology, river networks, coasts and lakes; non-destructive height brushes; terrain data maps.
- **[Snow](https://berlenga.canilho.net/features/snow/)**: a snowpack from the weather map that falls, settles, melts, slides and drifts, covering materials raster and path traced; deep snow with tracks; falling and blowing snow; baked winters and cornices.
- **[Vegetation](https://berlenga.canilho.net/features/vegetation/)**: biome plant placement from terrain maps; grass and generated meadow, forest floor and wetland plants in the weather's wind; GPU-placed instances seen by path tracing; per-instance bones for plants in the wind.
- **[Trees](https://berlenga.canilho.net/features/trees/)**: trees and woody shrubs grown by light competition, 31 species in ten stand types, with bark, moss, climbers, seasons, old trees and deadwood; weather wind out to 1 km, colliders and touch.
- **[Weather](https://berlenga.canilho.net/features/weather/)**: one weather map for wind, storms, rain, wet surfaces and puddles, driving water, cloth and hair.
- **[Sky and clouds](https://berlenga.canilho.net/features/sky-and-clouds/)**: sun and moon set by date, time and place; a scattering atmosphere with twilight; a cloud layer from the weather map, reprojected, seen by path tracing, with shadows on the ground.
- **[Water](https://berlenga.canilho.net/features/water/)**: refraction, murky water, caustics, ocean, currents, boat wakes, white water, rivers and waterfalls, shore waves, camera under water.
- **[Exposure](https://berlenga.canilho.net/features/exposure/)**: histogram metering, dark-scene key, local exposure.
- **[Physics](https://berlenga.canilho.net/features/physics/)**: PhysX 5 alongside Jolt: cloth, tearing, destruction, path-traced smoke.
- **[Open worlds](https://berlenga.canilho.net/features/open-worlds/)**: camera-relative tracing, persistent instances, culling, mesh LOD.

Every feature with its setting and images: [features](https://berlenga.canilho.net/features/).

## References

Code: [NVIDIA-RTX/godot](https://github.com/NVIDIA-RTX/godot) (path tracer, DLSS); [uno1982/godot](https://github.com/uno1982/godot) `feature/physx5-module` (Wild Ox Studios, MIT); [NVIDIA-Omniverse/PhysX](https://github.com/NVIDIA-Omniverse/PhysX) (BSD-3-Clause); [GPUOpen-Effects/TressFX](https://github.com/GPUOpen-Effects/TressFX) (AMD, MIT; strand hair simulation and rendering); [valence-engine-rtx](https://github.com/Rman887/valence-engine-rtx) (camera-relative tracing, ray offset, persistent instance slots, opacity micromap bake). Papers: [docs/REFERENCES.md](docs/REFERENCES.md). Research notes: [docs/ideas](docs/ideas).

## License

MIT, like Godot. Third-party licenses in `thirdparty/` and `COPYRIGHT.txt`; NVIDIA binaries under NVIDIA's terms. Build and docs: [godotengine/godot](https://github.com/godotengine/godot), [docs.godotengine.org](https://docs.godotengine.org).

Berlenga is an independent fork and is not affiliated with or endorsed by the Godot Foundation.
