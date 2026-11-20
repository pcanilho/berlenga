### Changed

- The contact wetness map dries and re-encodes only the area that is wet, and skips the GPU upload when nothing visible changed; the picture is unchanged.
- Wet tracks check each walking body's own-mud parts only when its children change, not every frame.
