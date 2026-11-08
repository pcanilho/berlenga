# Exposure for dark scenes under path tracing

Research note, 8 Nov 2026. How games keep night and storm scenes dark but
readable under automatic exposure, and what fits this fork.

## What the fork does today

Checked on the cloud renderer with a grey plane:
- A directional or omni light gives the same pixel value under the path tracer
  as under raster. Direct light is calibrated the same in both.
- Godot's auto exposure (CameraAttributes) works under the path tracer exactly
  as under raster.

So a path traced scene that looks darker than raster is not a scaling bug. The
raster image is lifted by ambient light and probes that the path tracer
replaces with real bounces, and by light that never reaches places in a traced
scene.

Godot's auto exposure meters the arithmetic mean of each pixel's brightest
channel (`luminance_reduce.glsl`), then scales the image so that mean lands on
a fixed key (`auto_exposure_scale`). Two consequences matter for dark scenes:
- An arithmetic mean is pulled up by a few very bright pixels: lamps, signs and
  specular highlights the camera sees directly. A night street with visible
  lamps is then exposed for the lamps and the street goes black.
- A fixed key drives every scene toward the same mid-grey. A night scene that
  is metered on its dark pixels comes out looking like dusk.

Path tracing makes both stronger: emissive surfaces and lamps are now visible
at full brightness, and bounce light lifts some dark areas while true shadow
stays black.

## Published methods

### Log average and histogram metering
- Reinhard et al., *Photographic Tone Reproduction for Digital Images*,
  SIGGRAPH 2002: meter the log-average luminance and map it to a key value
  (0.18 for a normal scene). The log average is far less sensitive to a few
  bright pixels than the arithmetic mean.
- Unreal Engine, *Auto Exposure Histogram*: a luminance histogram in EV, and
  the exposure is set from the mean of the bins between a low and a high
  percentile (defaults 80% and 98.3%). Lamps above the high percentile and
  black sky below the low one are ignored.
- Lagarde and de Rousiers, *Moving Frostbite to Physically Based Rendering*,
  SIGGRAPH 2014: exposure in EV100 with a physical camera, auto exposure from a
  histogram, and an exposure compensation the artists set.

### Keeping dark scenes dark
- Krawczyk, Myszkowski and Seidel, *Lightness Perception in Tone Reproduction
  for High Dynamic Range Images*, Eurographics 2005: the key value follows the
  scene's average luminance, key = 1.03 - 2 / (2 + log10(L + 1)). Dark scenes
  get a low key and stay dark, bright scenes a higher one.
- Unreal Engine, *Exposure Compensation Curve*: an artist curve from the
  metered scene EV to an exposure compensation, so a night (low EV) can be
  pushed one to two stops darker than mid-grey on purpose.

### Local exposure
- Mertens, Kautz and Van Reeth, *Exposure Fusion*, Pacific Graphics 2007, and
  Wronski, *Exposure Fusion: Local Tonemapping for Real-time Rendering*, 2022:
  blend a few exposures of the same frame by local weights. Lit windows keep
  detail while the street around them stays readable.
- Unreal Engine 5.1 *Local Exposure*: a bilateral grid of local luminance that
  compresses highlight and shadow contrast around the global exposure.

## What to take

| Item | Take |
|---|---|
| Metering | Histogram in EV (64 bins) with low and high percentiles, as Unreal. Replaces the arithmetic mean, so visible lamps no longer black out a night street. |
| Key | Krawczyk's luminance-dependent key, or a compensation curve over metered EV. Keeps night darker than day without per-scene numbers. |
| Settings | Project or CameraAttributes options: metering mode (average as now, or histogram), percentiles, dark-scene compensation. The default is Godot's current behaviour. |
| Local exposure | Later, after the above: exposure fusion as a post pass, for lit windows and signs at night. |

Order: histogram metering first (largest change for night scenes with visible
lamps), then the key curve, then local exposure.
