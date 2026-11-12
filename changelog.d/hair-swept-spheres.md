### Added
- Path tracer: strand hair is traced as linear swept spheres (round tubes, one primitive per segment) on GPUs with `VK_NV_ray_tracing_linear_swept_spheres`, and shaded from where the ray meets the round fibre; other GPUs keep the crossed strips (`rendering/pathtracing/linear_swept_spheres`)
- RenderingDevice: `SUPPORTS_RAYTRACING_LINEAR_SWEPT_SPHERES` and a linear swept spheres BLAS geometry type
