### Fixed
- Path tracing: a surface whose custom shader is still compiling (`rendering/pathtracing/async_shader_compilation`) is traced with the default material until its shader is ready, if it is opaque and never discards; it was left out, so a character drawn by `PhysXSkinnedCloth3D` vanished for the first minute on a cold shader cache. Stills still wait for the real shader.
