### Changed

- Ground mist sets its emitters only when a cell's place, density or wind changes, instead of seven property writes per cell every frame; the mist is unchanged.
- Ground mist reads wet ground, water and steam at each cell twice a second instead of every frame, and takes the ground height from the world's ground before casting physics rays.
