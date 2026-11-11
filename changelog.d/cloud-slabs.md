### Fixed
- WeatherSkyMaterial: clouds no longer look like flat-topped slabs with vertical walls (the shape noise was stretched so far in height that each cloud was one column through the layer), and distant clouds near the horizon are no longer boxes (the noise is read at the pixel's footprint, not the march step).
