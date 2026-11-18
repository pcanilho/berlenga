---
title: "Berlenga"
description: "A Godot 4 fork with Vulkan path tracing and DLSS Ray Reconstruction, built for large open worlds."
---

<section class="hero">
<h1><img src="media/berlenga-512.png" alt="" width="72" height="72"><span>Berlenga</span></h1>
<p class="lead">A Godot 4 fork with Vulkan path tracing and DLSS Ray Reconstruction, and a simulated world where weather, water, fire, snow and ground act on each other. Built for large open worlds. Based on <a href="https://github.com/NVIDIA-RTX/godot">NVIDIA-RTX/godot</a>.</p>
<div class="btns"><a class="btn primary" href="getting-started/#download" data-dl-hero>Download</a><a class="btn" href="getting-started/">Getting started</a><a class="btn" href="https://github.com/pcanilho/berlenga">Source</a></div>
</section>

![Breaking waves open into torn lace foam over sand ripples in the shallows](media/shore-surf.jpg)

*Surf: breakers open into torn lace foam over sand ripples in the shallows. Images on this site are raw renderer output, without denoiser or Ray Reconstruction.*

<div class="stats"><div><b>4096</b><span>path-traced lights (upstream 64)</span></div><div><b>DLSS 4.5</b><span>Ray Reconstruction</span></div><div><b>PhysX 5</b><span>cloth, Flow smoke and fire, Blast, fluids</span></div><div><b>One world</b><span>wind, rain, snow and fire reach every system</span></div></div>

## Highlights

<ul class="hl"><li><a href="features/lighting/"><img src="media/many-lights.jpg" alt="1024 street lamps, upstream left and Berlenga right" loading="lazy"><span class="hl-area">Lighting</span><b>Thousands of path-traced lights</b><span class="hl-text">1024 lamps at 16 samples per pixel: upstream keeps 64 of them (left), Berlenga lights them all (right).</span></a></li><li><a href="features/physics/"><img src="media/physx-campfire.jpg" alt="Fire that spreads" loading="lazy"><span class="hl-area">Physics</span><b>Fire that spreads</b><span class="hl-text">Flames light the scene, throw embers, spread through grass and crowns, char wood and leave a burn scar.</span></a></li><li><a href="features/terrain/"><img src="media/sand-shore.jpg" alt="Ground that remembers" loading="lazy"><span class="hl-area">Terrain</span><b>Ground that remembers</b><span class="hl-text">Sand, mud and snow take prints, ruts and tracks that wet, freeze, slump and regrow.</span></a></li><li><a href="features/sky-and-clouds/"><img src="media/sky-clouds.jpg" alt="A sky that follows the weather" loading="lazy"><span class="hl-area">Sky and clouds</span><b>A sky that follows the weather</b><span class="hl-text">Cumulus, cloud shadows and haze from one weather map, raster and path traced.</span></a></li><li><a href="features/rocks/"><img src="media/rocks-sea-cave.jpg" alt="Rocks and caves" loading="lazy"><span class="hl-area">Rocks</span><b>Rocks and caves</b><span class="hl-text">Rocks placed by rockfall, rivers and shores; caves grown behind cliff feet.</span></a></li><li><a href="features/physics/"><img src="media/physx-fluid-pour.jpg" alt="Clear water as particles" loading="lazy"><span class="hl-area">Physics</span><b>Clear water as particles</b><span class="hl-text">PhysX fluid with a real surface; foam and bubbles only where water strikes water.</span></a></li></ul>

## One connected world

<p>One weather map drives every system, and each system acts on the others.</p>

<ul class="hl hl-links"><li><a href="features/vegetation/"><img src="media/grass-drought.jpg" alt="Weather → grass: In a summer drought whole blades die back to straw." loading="lazy"><span class="hl-area">Weather → grass</span><span class="hl-text">In a summer drought whole blades die back to straw.</span></a></li><li><a href="features/trees/"><img src="media/trees-seasons.jpg" alt="Season → trees: By November the beech turns, maple and birch drop their leaves, the larch goes gold." loading="lazy"><span class="hl-area">Season → trees</span><span class="hl-text">By November the beech turns, maple and birch drop their leaves, the larch goes gold.</span></a></li><li><a href="features/weather/"><img src="media/weather-wet.jpg" alt="Rain → ground: Sand darkens and puddles form and catch the sky." loading="lazy"><span class="hl-area">Rain → ground</span><span class="hl-text">Sand darkens and puddles form and catch the sky.</span></a></li><li><a href="features/hair/"><img src="media/hair-snow.jpg" alt="Snow → hair: Flakes settle through the outer strands." loading="lazy"><span class="hl-area">Snow → hair</span><span class="hl-text">Flakes settle through the outer strands.</span></a></li><li><a href="features/vegetation/"><img src="media/plants-bend.jpg" alt="Bodies → plants: Nettles part round a passing body." loading="lazy"><span class="hl-area">Bodies → plants</span><span class="hl-text">Nettles part round a passing body.</span></a></li><li><a href="features/water/"><img src="media/lake-ripples.jpg" alt="Wind → water: Short wind waves and sun glitter on a lake." loading="lazy"><span class="hl-area">Wind → water</span><span class="hl-text">Short wind waves and sun glitter on a lake.</span></a></li></ul>

Each feature that changes the look or the cost can be turned off with the setting shown on its page; off is upstream behaviour.
