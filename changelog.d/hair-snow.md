### Added
- Snow settles on strand hair: PhysXHair3D's `snow` lies on the top layer, where no hair above shields it and on the side the wind drives it at, read from an extra deep opacity map; it is caught from the weather's snowfall, melts above freezing and from the head's warmth, and the melt wets the hair (`snow`, `snow_capacity`)

### Fixed
- Snowfall no longer soaks strand hair as if it were rain: the weather's precipitation is split into rain and snow by the air's temperature and humidity
