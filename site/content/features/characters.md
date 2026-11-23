---
title: "Characters"
weight: 96
group: "Simulation"
summary: "Procedural gaze and foot placement on deforming ground, spring followers."
---

- **Gaze** (`GazeModifier3D`): eyes, head, neck and upper back share one look, eyes leading, with spring motion.
- **Foot placement** (`FootPlacementModifier3D`): feet plant on deformed ground and snow, with pelvis drop and slope tilt, and prints land where the feet land.
- **Resting off screen** (`Skeleton3D.modifier_update_mode`): skeleton modifiers can rest while the skeleton is hidden or off screen.
- **Spring followers** (`SecondOrderDynamics`): a spring-damper follower for procedural motion.
