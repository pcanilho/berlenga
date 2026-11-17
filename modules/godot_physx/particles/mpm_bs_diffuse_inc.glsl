// Diffuse particles (spray, foam, air bubbles) for the block-sparse MLS-MPM fluid.
// #include'd after mpm_block_inc.glsl by mpm_bs_diffuse_spawn.glsl and
// mpm_bs_diffuse_advect.glsl only.
//
// Technique: Ihmsen, Akinci, Akinci and Teschner 2012, "Unified spray, foam and
// air bubbles for particle-based fluids" (The Visual Computer 28): fluid
// particles spawn secondary particles from trapped-air, wave-crest and
// kinetic-energy potentials; each secondary particle is spray, foam or a bubble
// by how much liquid surrounds it, and moves by its own rule. The particles do
// not act back on the liquid.
//
// The bubble and foam rules follow Wretborn, Flynn and Stomakhin 2022, "Guided
// bubbles and wet foam for realistic whitewater simulation" (ACM TOG 41(4)):
// bubbles drift with the water at the slip speed where buoyancy balances the
// drag law of their Eq. 5, with radii drawn from the inverse-cubic size
// distribution (Deike et al. 2016); foam is held on the liquid surface by a
// manifold projection, each step's move capped; foam lifetimes are normally
// distributed. Their two-way bubble coupling and SPH wet-foam forces are not
// done (cost).
//
// MPM adaptation: the "neighbours" of Ihmsen's sums are the 27 grid nodes of
// the quadratic B-spline stencil, and the neighbour count that sorts a particle
// is the grid's liquid fraction there (node mass over its mass at rest).

layout(set = 0, binding = 14, std430) restrict buffer Diffuse {
	vec4 diffuse[];
}; // 2 vec4 / slot: xyz position, w life left (<= 0: free) | xyz velocity, w bubble radius
layout(set = 0, binding = 15, std430) restrict buffer DiffuseHead {
	uint dhead[];
}; // [0] ring write head
layout(set = 0, binding = 16, std430) restrict buffer DiffuseOut {
	vec4 dout[];
}; // 1 vec4 / slot for drawing: xyz position, w drawn radius (0: none; negative: spray)

#define FDT (diffuse_a.x)
#define DRATE (diffuse_a.y)
#define DSEED (uint(diffuse_a.z))
#define DCAP (uint(diffuse_a.w))
#define DLIFE (diffuse_b.x)
#define DBUOY (diffuse_b.y)
#define DRMAX (diffuse_b.z)
#define DRMIN (diffuse_b.w)
#define TA_MIN (diffuse_c.x)
#define TA_MAX (diffuse_c.y)
#define KE_MIN (diffuse_c.z)
#define KE_MAX (diffuse_c.w)

// Liquid fraction bands (Ihmsen sorts by neighbour count: < 6 spray, > 20 bubble).
#define SPRAY_BELOW 0.12
#define BUBBLE_ABOVE 0.7
// The level foam is held on: the liquid surface in grid liquid fraction.
#define FOAM_LEVEL 0.4

// Ihmsen Eq. 1: the clamped, normalised potential.
float potential(float I, float lo, float hi) {
	return (min(I, hi) - min(I, lo)) / max(hi - lo, 1e-6);
}

uint pcg(uint v) {
	uint state = v * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

float rand01(inout uint s) {
	s = pcg(s);
	return float(s >> 8) * (1.0 / 16777216.0);
}
