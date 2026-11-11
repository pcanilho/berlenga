---
title: "Berlenga"
description: "A Godot 4 fork with Vulkan path tracing and DLSS Ray Reconstruction, built for large open worlds."
---

<section class="hero">
<h1><span>Berlenga</span></h1>
<p class="lead">A Godot 4 fork with Vulkan path tracing and DLSS Ray Reconstruction, built for large open worlds. Based on <a href="https://github.com/NVIDIA-RTX/godot">NVIDIA-RTX/godot</a> (2 November 2026).</p>
<div class="btns"><a class="btn primary" href="https://github.com/pcanilho/berlenga/releases/latest">Download</a><a class="btn" href="getting-started/">Getting started</a><a class="btn" href="https://github.com/pcanilho/berlenga">Source</a></div>
</section>

![1024 street lamps: NVIDIA-RTX/godot on the left keeps 64 of them, Berlenga on the right lights them all](media/many-lights.jpg)

*1024 lights, 16 samples per pixel. Left: NVIDIA-RTX/godot. Right: Berlenga. Images on this site are raw path tracer output, mostly CPU-rendered, without denoiser or Ray Reconstruction.*

<div class="stats"><div><b>4096</b><span>path-traced lights (upstream 64)</span></div><div><b>DLSS 4.5</b><span>Ray Reconstruction</span></div><div><b>PhysX 5</b><span>alongside Jolt</span></div><div><b>MIT</b><span>like Godot</span></div></div>

Each feature that changes the look or the cost can be turned off with the setting shown on its page; off is upstream behaviour.
