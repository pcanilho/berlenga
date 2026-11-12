### Added
- `outwash_age`: gravel reaches below the last ice stand or on fans become braided outwash plains (Murray-Paola cellular braiding on a flat graded plain, bare gravel); `get_braidplains()` gives each plain's centre line, half widths, water stage per point, head discharge and source (`get_last_braid_count()`).
### Fixed
- Rock detail no longer draws moire hairlines at a distance or at grazing angles, and no longer darkens the ground: cracks are box filtered over each pixel's footprint, every other feature fades to its mean, it filters the same under path tracing, and it stays off rock gentler than 45 degrees.
- Falls over 25 m keep the face they fall over, with a notch at the lip, instead of a straight-sided slot and a flat pad at the foot.
