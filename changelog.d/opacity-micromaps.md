### Added
- Path tracer: opacity micromaps for alpha-scissor StandardMaterial3D surfaces (Vulkan `VK_EXT_opacity_micromap`), baked on the GPU on first trace so rays skip cut-out texels and accept solid ones without the any-hit shader; same image, opt-in with `rendering/pathtracing/opacity_micromaps` (off by default).
