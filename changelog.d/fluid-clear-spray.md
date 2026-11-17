### Changed

- MPM fluid spray is drawn as clear water drops; foam and bubbles stay white.

### Fixed

- A smooth MPM pour no longer fills with white specks: trapped air is measured against each particle's own velocity field, so only liquid striking liquid makes foam and bubbles.
- `foam_size` set on an MPM fluid no longer rescales its foam spheres twice.
