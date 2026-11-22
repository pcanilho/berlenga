---
title: "Other"
weight: 110
group: "Engine"
summary: "Cached shadows, time-sliced reflection probes, video memory queries, fixes."
---

- Cached shadows, time-sliced reflection probes; `is_pathtracing_ready()`, video memory queries; leak, crash and device-fault fixes.
- **Shaders compile in the background** (`rendering/pathtracing/async_shader_compilation`): path tracing draws a surface with the default material while its own shader compiles, instead of leaving it out.
- **Long stills in bands** (`rendering/pathtracing/max_samples_per_dispatch`): high-sample path traced stills trace in short bands, so the GPU never times out.
