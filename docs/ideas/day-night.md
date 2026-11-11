# Day and night cycle

Research note, 10 Nov 2026. This note covers what a day and night cycle needs, how papers and other engines
handle it, and what fits this fork. It extends weather step 2 (sky and haze, [weather.md](weather.md)) and
uses the exposure note ([exposure.md](exposure.md)).

## What the fork has today

There is no day and night cycle, in the fork or in Godot itself.

- The sun is a `DirectionalLight3D` that a scene or script points by hand. Nothing ties it to a clock, a date or
  a place on Earth.
- `PhysicalSkyMaterial` and our `WeatherSkyMaterial` follow whichever light the sky shader sees. Both use the
  Preetham analytic daylight model with single scattering. That model is fitted to daylight, so it gives no real
  twilight, no Earth shadow and no night sky.
- There is no moon, no stars, no Milky Way and no airglow.
- The clouds and the cloud shadow projector (#207) follow the sun light. They would follow a moving sun with no
  changes.
- Godot already has what the rest needs: physical light units (lux on lights, EV100 on cameras), auto
  exposure, a sky radiance map that can update over several frames, and real-time sun shadows. The path
  tracer gets the sky shader for camera rays (`pathtracing_sky_shader_enabled`) and the radiance map for
  bounce rays.

## The parts of a day and night cycle

1. **A clock and an ephemeris.** These take the date, the time, the latitude and the longitude and give the
   direction of the sun and the moon, the moon's phase, and the rotation of the stars (sidereal time).
2. **An atmosphere that works at any sun angle.** That means day, golden hour, twilight with the Earth's shadow,
   and night. The same tables colour the sunlight, the moonlight and the haze.
3. **A night sky.** The moon with its phases and earthshine, the stars, the Milky Way, and faint sources such
   as airglow and zodiacal light.
4. **Lighting in physical units.** Sunlight and moonlight in lux, and sky light from the atmosphere. Under path
   tracing the sun and the moon are discs of their real angular size.
5. **Exposure and perception.** Light levels span eight orders of magnitude, from about 100,000 lux at noon to
   about 0.001 lux under starlight. Night must still look like night, with the colour loss and blue shift of
   rod vision.
6. **Ties to the weather.** A daily temperature cycle controls drying, dew and fog at dawn, and the clouds and
   cloud shadows follow the sun and the moon.

## Papers

### Jensen, Durand, Dorsey, Stark, Shirley and Premoze, A Physically-Based Night Sky Model (SIGGRAPH 2001)

This is the reference for the night half.
- **Moon.** The moon is real geometry, built from elevation and albedo maps. It is lit by the sun and by
  earthshine, with full earthshine at 0.19 W/m2. Its reflectance follows the Hapke-Lommel-Seeliger model, with
  g = 0.6, t = 0.1 and a mean albedo of 0.072. Albedo rises from 70% at 340 nm to 135% at 740 nm, which is why
  moonlight is slightly red, not blue.
- **Stars.** The stars come from the Yale Bright Star Catalogue: about 9,000 stars, about 6,000 of them visible
  to the eye.
  - Irradiance: `E = 10^(0.4 (-m - 19))` W/m2, with 0.4 magnitudes of extinction.
  - Colour: a black body at `T = 7000 K / (B-V + 0.56)`.
- **Faint light.** Zodiacal light comes from a measured table in ecliptic coordinates. Airglow is a layer at
  80 km with emission lines at 557.7, 583.0 and 630.0 nm. Galactic and cosmic light are constants.
- **Irradiance at the ground (W/m2).**

  | Source | Irradiance |
  | --- | --- |
  | Sunlight | 1.3e3 |
  | Full moon | 2.1e-3 |
  | Bright planets | 2.0e-6 |
  | Zodiacal light | 1.2e-7 |
  | Airglow | 5.1e-8 |
  | Integrated starlight | 3.0e-8 |
  | Diffuse galactic light | 9.1e-9 |
  | Cosmic light | 9.1e-10 |

  The full moon is about 600,000 times dimmer than the sun.
- **Night vision.** Rendering uses 40 spectral samples, converted to XYZ plus a scotopic channel. A histogram
  tone mapper models rod and cone vision, the blue shift and the loss of fine detail at night.
- The renders were offline: 30 s to 2 min per frame.

### Hillaire, A Scalable and Production Ready Sky and Atmosphere Rendering Technique (EGSR 2020)

This is already the plan for weather step 2. It uses a few small tables: transmittance, multiple scattering, a
sky view and an aerial perspective volume. The tables are cheap to rebuild every frame, so the sun can move
freely and the atmosphere can change. The method works from the ground to space and, according to the paper,
also speeds up path tracing. It is what Unreal's `SkyAtmosphere` uses.

For a day and night cycle it brings three things:
- The transmittance table gives the colour and strength of the sunlight and the moonlight at any elevation.
  The light turns red at sunset because the light really is red there, not because a curve was authored.
- Twilight and the blue hour come out of multiple scattering.
- The moon can be a second atmosphere light, so moonlit haze has the right colour.

### Night vision: Cao, Pokorny, Smith and Zele (2008); Kirk and O'Brien (2011)

Cao et al. measured how rods feed the cone pathways. Kirk and O'Brien turned this into a tone mapping step for
low light, called "Perceptually Based Tone Mapping for Low-Light Conditions". Under about 3 lux, colours fade
and shift toward blue, the Purkinje shift.

Ghost of Tsushima (Patry, SIGGRAPH Advances 2021) ships this. RGB is converted to cone and rod responses (LMSR),
each cone gets a gain from the rod signal, and the result is mapped back to RGB. The constants in the talk are
K = 45, S = 10, k3 = 0.6, r_w = 0.139 and p = 0.6189. These come from a summary of the slides; check them against
the slides before use. The step runs after lighting and before tone mapping, so raster and path tracing get the
same treatment.

## Ephemeris: how accurate does it need to be

| Method | Accuracy | Cost | Used by |
| --- | --- | --- | --- |
| NREL Solar Position Algorithm (Reda and Andreas 2004) | sun 0.0003 deg | hundreds of terms | solar energy |
| Meeus, Astronomical Algorithms (ch. 25 sun, ch. 47 moon) | sun about 0.01 deg, moon about 10 arcsec with the full table | about 100 terms for the moon | planetarium software, NOAA calculator |
| Schlyter, "How to compute planetary positions" | about 1-2 arcmin | a few lines | Sky3D (Godot), many games |

The moon's disc is 30 arcmin wide, so Schlyter's 1-2 arcmin is already invisible on screen. Meeus with a
truncated table costs microseconds once a frame and removes all doubt. Recommendation: Meeus. It gives the sun,
the moon, the moon's phase angle and its position angle (which way the lit side faces).

Moon brightness with phase: the moon's magnitude is `m = -12.73 + 0.026 |a| + 4e-9 a^4` (a = phase angle in
degrees; Allen's Astrophysical Quantities). At a half moon (a = 90) it is about 1/10 of a full moon, not 1/2,
because of the opposition surge. In the Hapke model the same spike near full moon falls out of the shadow
hiding term.

## Other engines and addons

| Engine | Clock and positions | Sky | Night | Units and exposure |
| --- | --- | --- | --- | --- |
| Unreal 5 | Sun Position Calculator plugin: latitude, longitude, time zone, north offset, date, time; moves the sun light (no algorithm named; no moon) | `SkyAtmosphere` (Hillaire 2020), up to two atmosphere lights, so a moon is a second directional light | No built-in moon or stars; marketplace packs add them | Physical units, EV100, auto exposure |
| Unity HDRP | Scripts set the light | Physically Based Sky (Bruneton-style tables), directional lights drawn as discs | Directional lights can be drawn as textured celestial bodies; stars are a space emission texture | Physical units, exposure in EV |
| Frostbite (Hillaire 2016) | Game side | Physically based sky with tables | Moon as a second light | Physical units, EV100 and auto exposure built around them |
| Ghost of Tsushima (Patry 2021) | Game side | Precomputed 3D tables indexed by altitude, sun or moon angle and view angle (Bruneton 2008, Elek 2009, Yusov 2013), cut to 2D per frame | Moon lights the same atmosphere; Purkinje shift | Illuminance-based exposure, local tone mapping |
| Sky3D (Godot 4 addon, MIT, port of J. Cuellar's TimeOfDay) | Schlyter's orbital elements in GDScript: sun, moon, sidereal time for the stars | Preetham and Hoffman scattering in a sky shader, plus fog and cloud layers | Moon disc and phases; ESO Milky Way photo (CC BY 4.0); moon light scaled by a dot product, not by phase physics | Artist light energies (not lux); its own exposure value |

What we take from Sky3D after reading its code (`TimeOfDay.gd`, `SkyDome.gd`): its clock and date handling,
sidereal rotation and its set of controls are a good checklist. We don't bundle it. Its atmosphere is the same
Preetham model we want to replace, its light levels are not physical, and it knows nothing of path tracing.
That matches the terrain decision of 9 Nov: no bundled addons.

## What fits the fork

All of this lives in the weather module, next to the clouds, so one node owns the sky.

1. **`SkyClock` on `WeatherMap3D`.**
   - Properties: date, time of day, latitude, longitude, time zone, north offset and time scale. Pausing and
     scrubbing work.
   - A Meeus ephemeris gives the sun, the moon, the moon's phase and position angle, and the sidereal time.
   - It drives `sun_light` and a new `moon_light`, both `DirectionalLight3D`.
2. **Hillaire atmosphere in `WeatherSkyMaterial`** (weather step 2, already planned).
   - The sun and the moon are both atmosphere lights.
   - The transmittance table is also read on the CPU, so each light's colour and lux follow its elevation.
   - The aerial perspective volume gives distance haze.
3. **Night sky in the same sky shader.**
   - Moon disc: Hapke-Lommel-Seeliger shading of an albedo and normal map from the NASA LRO CGI Moon Kit
     (public domain). Earthshine on the dark side. The disc is oriented by the position angle.
   - Stars: about 9,000 points from the Yale Bright Star Catalogue (public domain), with magnitude to
     irradiance, B-V to colour, twinkle from scintillation, and extinction near the horizon.
   - Milky Way: the ESO panorama (CC BY 4.0; one line in References), or the NASA SVS Deep Star Maps.
   - Airglow and zodiacal light as Jensen's constants and table.
4. **Physical light levels.**
   - Sun: about 100,000-120,000 lux at the top of the atmosphere, times the transmittance.
   - Moon: from the magnitude formula above (about 0.25 lux at full moon), times the transmittance.
   - Starlight and airglow: about 0.001 lux of sky light.
   - Under path tracing, the sun is 0.53 deg wide and the moon is 0.52 deg wide.
   - The cloud shadow projector is baked for whichever light is up.
5. **Exposure across the day.**
   - Meter on illuminance, as Ghost of Tsushima does. A compression curve keeps night darker than day instead of
     metering it back to mid-grey. Exposure.md already describes the night half.
   - The Purkinje shift (Kirk and O'Brien, with Ghost of Tsushima's constants) runs as a post step, so raster
     and path tracing match.
6. **Lighting that keeps up.**
   - Raster: the sky radiance map updates incrementally. Directional shadows already update every frame.
   - Path tracing: the radiance cache has to let go of old light fast enough during time-lapse. This needs a
     check at fast time scales.
   - Baked lightmaps can't follow a moving sun; scenes with a cycle use SDFGI or path tracing.
7. **Weather ties.**
   - A daily temperature curve, lagging the sun by about 2-3 hours, sets drying rates.
   - Dew and radiation fog at dawn on clear, calm nights.
   - The clouds are already lit by whichever light is up.

Order inside the sky step: the clock and ephemeris first (small, and everything hangs off it), then the Hillaire
atmosphere with two lights, then the night sky, then exposure and the Purkinje shift. Paulo chose on 10 Nov to
build the moon with the sky step.

### As built: step 1, the clock (11 Nov)

`SkyClock` (modules/weather/sky_clock.*), a Node: `year`, `month`, `day`, `time_of_day`, `utc_offset`,
`time_scale`, `latitude`, `longitude`, `north_offset`, `refraction`, `sun_light`, `moon_light`,
`hide_below_horizon`. It points both lights and reports the sun's and the moon's direction, altitude and
azimuth, the moon's phase angle, lit fraction, bright limb angle (from the zenith, for the disc's shading) and
magnitude, the light above the air in lux, sidereal time and `get_celestial_basis()` for the stars. The moon
uses the 50 largest longitude and distance terms and 30 largest latitude terms of Meeus 47.A and 47.B, plus
parallax (the moon sits up to 1 deg lower from the ground) and Saemundsson refraction.

`docs/bench/sky --test` against Meeus' worked examples: the sun within 0.0004 deg (25.a), the moon within 0.001
deg in longitude and 10 km in distance (47.a), the phase angle within 0.001 deg (48.a), sidereal time within
0.005 s (12.a); Lisbon on 21 June 2026 rises at 06:11.7 and sets at 21:05.3 (almanac 06:12, 21:05); full moon
0.27 lux above the air. One update costs about 2 microseconds. The lights' colour and strength come with the
atmosphere (step 2), which owns the transmittance.

### Checks

- Positions against published almanac values. The JPL Horizons ephemeris gives the sun and the moon for a
  given place and time; aim for under 0.05 deg.
- Light levels at noon, at sunset, at the end of civil twilight (about 3 lux) and at full and quarter moon.
- Raster and path traced frames at 100% crop: dusk, full moon and new moon.
- Time-lapse at 1 hour per second: no popping in sky light, shadows or exposure.

## Sources

- Jensen, Durand, Dorsey, Stark, Shirley and Premoze, A Physically-Based Night Sky Model, SIGGRAPH 2001:
  https://graphics.cs.yale.edu/sites/default/files/nightsky.pdf
- Hillaire, A Scalable and Production Ready Sky and Atmosphere Rendering Technique, EGSR 2020:
  https://diglib.eg.org/handle/10.1111/cgf14050
- Hillaire, Physically Based Sky, Atmosphere and Cloud Rendering in Frostbite, SIGGRAPH 2016 course.
- Patry, Lighting, Atmosphere, and Tonemapping in Ghost of Tsushima, SIGGRAPH Advances 2021:
  https://www.advances.realtimerendering.com/s2021/jpatry_advances2021/index.html
- Kirk and O'Brien, Perceptually Based Tone Mapping for Low-Light Conditions, SIGGRAPH 2011:
  https://www2.eecs.berkeley.edu/Pubs/TechRpts/2011/EECS-2011-91.html
- Cao, Pokorny, Smith and Zele, Rod contributions to color perception, Vision Research 48 (2008).
- Meeus, Astronomical Algorithms, 2nd ed., Willmann-Bell 1998.
- Reda and Andreas, Solar Position Algorithm for Solar Radiation Applications, NREL 2004.
- Schlyter, How to compute planetary positions: https://stjarnhimlen.se/comp/ppcomp.html
- Unreal Engine, Sun Position Calculator:
  https://dev.epicgames.com/documentation/en-us/unreal-engine/geographically-accurate-sun-positioning-tool-in-unreal-engine
- Sky3D (TokisanGames, MIT): https://github.com/TokisanGames/Sky3D
- Hoffleit and Warren, The Bright Star Catalogue, 5th revised ed. (Yale), 1991.
- NASA SVS, CGI Moon Kit (LRO): https://svs.gsfc.nasa.gov/4720
