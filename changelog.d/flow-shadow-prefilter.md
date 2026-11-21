### Fixed

- Raster Flow gas's shadow on the ground no longer stair-steps into a hard dark outline at the gas's edge: the shadow map averages four columns per texel and has a texel per two gas cells.
- The top of a deep, bright Flow cloud no longer draws dark under the sky: its sky light's scattering orders count the gas below the point as well as above, and in bright gas reach deeper by diffusion theory (bluer inside under a blue sky).
