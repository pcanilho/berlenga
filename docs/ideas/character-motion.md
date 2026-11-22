# Character motion: procedural layers on top of the clip

Goal: a small set of native `SkeletonModifier3D` nodes for the procedural layer every character game
writes by hand: a gaze that turns the eyes, head, neck and upper back together; hips that shift and
settle while the feet stay planted on the real ground; hands that land on what they hold, oriented; and
spring-like (second-order) follow for all of it, so nothing moves on a straight line or a tween. The
first use case is a project that wrote this layer in script (about 900 lines over seven scripts); this
note lists what it does, what Godot already ships, what the established techniques are, and the gaps.

Status: research, 22 Nov. Nothing built yet; a card to Paulo picks the scope.

## What the first project does in script

| Script | What it does | Technique it names |
| --- | --- | --- |
| two-joint IK helper | Analytic two-bone IK with an optional pole and an optional end-bone basis; a frame-alignment helper | Holden, "Simple Two Joint IK" (2017) |
| hand placement modifier | After the arm IK places the wrist, turns the hand about it so the fingers (wrist to middle knuckle) and the across-hand line (little to index knuckle) match a target's axes; independent of bone roll | - |
| skinned contact tool | Re-skins meshes on the CPU (linear blend skinning) from a pose snapshot, grids the triangles, measures the gap and penetration between hand vertices and another surface | LBS; Ericson, closest point on triangle (RTCD 5.1.5) |
| idle weight shift modifier | Moves the hips over one leg, drops the free hip, counter-tilts the spine (contrapposto) on minimum-jerk curves; keeps both feet where the clip put them with two-bone IK; fades out when not standing | Egges, Molet, Magnenat-Thalmann 2004 (idle balance shifts); Winter 1995 (sway); Flash and Hogan 1985 (minimum jerk) |
| second-order helper | A scalar spring-damper with frequency, damping and initial response, stable at any frame time | t3ssel8r, "Giving Personality to Procedural Animations using Math" (2022) |
| gaze + head modifier | Yaw and pitch toward a world point, split neck 40% / head 60%, the upper back taking the part of the turn past 50 degrees (up to 22); limits; Perlin drift; no target behind | Andrist, Pejsa, Mutlu, Gleicher 2012; Pejsa et al. 2015 (head-eye coordination, torso alignment) |
| conversation gaze | Who looks at whom per line, look-away timing | Argyle and Cook 1976; Lee, Badler, Badler 2002; Kendon 1967; Pan et al. S3 2024 |

The conversation gaze and the idle timing styles are game logic and stay in the game. The rest is
generic.

## What Godot (and the fork, 4.8 dev) already ships

Godot 4.6 brought IK back as `SkeletonModifier3D`s, and later releases added constraints:

- `TwoBoneIK3D` (analytic, pole node or direction), `FABRIK3D`, `CCDIK3D`, `JacobianIK3D`, `SplineIK3D`
  (all `IKModifier3D`, multi-setting).
- `LookAtModifier3D`: aims **one** bone, with angle limits and damp thresholds and a time-based tween
  (`duration`, `transition_type`). `AimModifier3D`: aims a bone, constraint style.
- `CopyTransformModifier3D`, `ConvertTransformModifier3D`, `BoneTwistDisperser3D` (twist spread over a
  chain), `LimitAngularVelocityModifier3D` (a hard cap on angular speed).
- `SpringBoneSimulator3D` with sphere, capsule and plane colliders: verlet secondary motion for chains
  (hair, tails, coat hems). The first project already uses it for coats and `TwoBoneIK3D` for a driver's
  hands.
- `MeshInstance3D.bake_mesh_from_current_skeleton_pose()`: a CPU-skinned copy of the mesh.

Verified in the fork source (22 Nov):

| The project's piece | Native today? | Verdict |
| --- | --- | --- |
| Two-joint IK with pole | Yes, `TwoBoneIK3D` (pole node or direction; root and middle bones only, the end bone keeps its pose) | Port work |
| Hand orientation on a target | Partly: `CopyTransformModifier3D` can copy a target node's rotation onto the hand after `TwoBoneIK3D`; the target needs a per-rig offset for bone roll | Port work (the roll-free knuckle frame stays a small script, or a later option) |
| Skinned contact measuring (tools, tests) | Partly: `bake_mesh_from_current_skeleton_pose()` gives the skinned mesh; the nearest-surface query is script | Port work (tool only, not in play) |
| Idle weight shift timing and styles | No, and it should not be: it is behaviour | Stays game script, driving a pelvis offset (gap 3) |
| Gaze over several bones | Partly: `AimModifier3D` (a `BoneConstraint3D`) takes several settings with an `amount` each, so a spine, neck and head can each aim a share. No angle limits, no eye lead, no springs. `LookAtModifier3D` has limits but aims one bone and only tweens | **Gap** (1) |
| Second-order springs on procedural targets | No: `LookAtModifier3D` tweens over a fixed duration, `LimitAngularVelocityModifier3D` caps speed (constant-speed, the linear motion to avoid), `SpringBoneSimulator3D` is verlet chain physics, not a target follower | **Gap** (2) |
| Feet kept planted while the hips move; feet on uneven, deforming ground | No: nothing locks feet, offsets the pelvis or reads the ground | **Gap** (3) |

So the true gaps are a multi-bone gaze with limits and springs, a shared second-order follower, and
foot placement with pelvis offset linked to the world ground. The rest is the project moving onto
`TwoBoneIK3D`, `CopyTransformModifier3D` and `AimModifier3D`.

## Established techniques elsewhere

- **Unreal.** `AnimNode_FootPlacement` (Animation Warping plugin): pelvis adjustment toward the planted
  feet, foot lock (planted feet do not slide), ground alignment and trace per foot, plus Stride and
  Orientation Warping. Control Rig Full Body IK (position-based IK, many effectors with pelvis and spine
  participating). `LookAt` and Control Rig aim with chain distribution.
- **Unity Animation Rigging.** `MultiAimConstraint` applied down a spine chain with per-bone weights (the
  standard way to share a look over spine, neck and head), `DampedTransform` (a bone follows its parent
  with a damped lag), `TwoBoneIKConstraint` with target rotation weight (the hand takes the target's
  orientation), `MultiParentConstraint` for holding props.
- **Springs.** Holden, "Spring-It-On: The Game Developer's Spring-Roll-Call" (2021): exact critically
  damped and under-damped springs in closed form, frame-rate independent, and quaternion springs;
  t3ssel8r's second-order system (above) adds the initial-response term. Inertialization (Bollo, GDC 2018,
  Gears of War) uses the same idea for blends.
- **Gaze.** The head-eye coordination model (Andrist et al. 2012): eyes lead and saturate at their
  oculomotor range, the head follows with latency and covers the rest up to its comfortable range, the
  torso joins only for the remainder. Saccade "main sequence": peak velocity rises with amplitude
  (Bahill et al. 1975). Lee, Badler, Badler 2002 for saccade timing statistics. Shipped games (The Last of
  Us Part II, Red Dead Redemption 2) layer look-at on a distributed chain with eye leads and limits; their
  talks describe behaviour, not code, so the papers above are the spec.
- **Foot placement on terrain.** Per-foot ray or surface query, pelvis lowered to the lower foot, two-bone
  IK per leg, foot rotated to the ground normal within a limit, all spring-smoothed (UE Foot Placement,
  Unity foot IK samples, Johansen, "Automated Semi-Procedural Animation for Character Locomotion" 2009).
- **Hand contact.** Contact-aware IK resolves penetration against the skinned body or prop with a signed
  distance or nearest-surface query; for props, a grip frame on the prop that the hand frame matches.

## Gaps and the proposed nodes

### 1. `GazeModifier3D` (largest gap)

`LookAtModifier3D` turns one bone and tweens. A look needs a chain: eyes (if the rig has them), head,
neck, upper spine, each with a share and a comfortable range, eyes leading, head following later, the
torso taking only what is left, and spring dynamics instead of a tween.

- Settings: a target node or point; a bone list (spine to head, optional left/right eye bones) with
  share, yaw/pitch limits and frequency/damping per bone; a "too far behind, do not look" angle; an eye
  lead time; an optional low-amplitude noise drift.
- Motion: each bone's yaw and pitch through a second-order system (item 2) so shifts start quickly,
  overshoot slightly and settle; bigger shifts move faster (main sequence).
- Forward axis from the rest pose and the skeleton, so rigs whose armature is turned under a node work
  without a per-game "frame" fix.
- Replaces the project's gaze + head modifier pair; the conversation logic only sets targets.

### 2. Second-order dynamics, shared

A `SecondOrderDynamics` helper in core (float, Vector3, Quaternion; t3ssel8r's f, zeta, r with the
stable step, and Holden's closed-form critically damped case) and a `follow` group on the new nodes
(frequency, damping, response). Optionally a `DampedBoneModifier3D` (Unity's DampedTransform: a bone or
chain lags its animated pose with a spring), useful for bellies, bags, holsters and heavy coats where
the verlet springs are too loose.

### 3. `FootPlacementModifier3D` (the world link)

The first project shifts the hips at idle and keeps the feet planted with its own IK; an open world
needs the same node to put feet on uneven, deformable ground.

- Per leg: two-bone IK (reusing `TwoBoneIK3D`'s solver), foot lock while planted (the clip's foot speed
  below a threshold), foot rotated to the ground normal within a limit, toe bone optional.
- Pelvis: lowered toward the lower foot; plus an `offset` the game drives (the idle weight shift becomes a
  few lines of script setting it on minimum-jerk curves; the node keeps the feet put).
- Ground from the world contracts, not physics rays alone: `WorldGround.height_at` / the deformed surface
  height (ground-deform.md, `get_surface_height_at`), snow surface and sinkage (SnowCover3D), mud and sand
  sinkage. Falls back to a physics ray when no world provider exists, so it stays plain Godot.
- The other direction: with this node a rigged character's real foot bones press the prints (meta
  `world_contact_feet`, which the contracts already define) instead of `WorldWalkerSteps` guessing the
  stride, so prints, wet tracks, snow and mud follow the animation. New world-links row.

### 4. Not proposed now: end orientation and skinned contact

`TwoBoneIK3D` end orientation (Unity's TwoBoneIKConstraint has a target rotation weight) is covered well
enough by `CopyTransformModifier3D` after the IK; revisit only if per-rig roll offsets prove painful.

Skinned contact query (later, tools):

A native `Skeleton3D`/`MeshInstance3D` query for the nearest skinned surface point to a set of bones'
vertices (gap and penetration), for tools and tests, and later for resolving hand-on-body penetration in
play. `bake_mesh_from_current_skeleton_pose` gives the skinned positions; the native part is the grid and
the query. Lower priority.

## Path tracing and quality

- The modifiers run on the CPU before skinning; the path tracer builds its skinned BLAS from the same
  skeleton output (render_raytracing.cpp reads the mesh instance's skeleton output), so raster and PT
  always see the same pose. Nothing here changes materials or geometry detail.
- Accumulation: when time stops (stills, paused captures) every spring and noise term must hold still, or
  a PT still smears. All state advances by the modifier's delta only, noise is a function of accumulated
  time, and a `reset()` snaps the springs to the target for cuts.
- Motion vectors: skinned instances already carry previous-frame bones; springs make the motion
  continuous, which helps the denoiser.
- No visual degradation: the native nodes must reproduce the script poses within a small tolerance on the
  first project's existing captures before the scripts are swapped out (raster and PT pairs, 100% crops of
  face, shoulders, hands and feet).

## Performance

Script modifiers run every frame per character; the native versions remove that script cost and are
needed for crowds in an open world. Each node reports a before/after CPU time per character from the
profiler, measured, not assumed.

## Order on a skeleton

Clip, then `FootPlacementModifier3D` (pelvis and feet), arm IK and stances (`TwoBoneIK3D`), then
`GazeModifier3D`, then `SpringBoneSimulator3D` and damped bones last, so secondary motion reacts to the
final pose.

## Plan, once a scope is picked

1. `SecondOrderDynamics` in core, with unit tests (step response, overshoot, stability at large dt).
2. `GazeModifier3D`, with class docs and a test skeleton; compare against the script gaze on the
   project's captures.
3. `FootPlacementModifier3D` with the world-contract ground; links bench row; walker feet link.
4. The first project moves onto the nodes (its two-joint IK, hand placement, gaze and weight shift
   become settings and a little script), behind an in-engine side-by-side close-up check.

Each C++ step gets the clang syntax pass (also with `-DREAL_T_IS_DOUBLE`) and a project setting only
where it changes existing behaviour (new nodes are opt-in by being added).

## References

- Holden, "Simple Two Joint IK" (2017); "Spring-It-On: The Game Developer's Spring-Roll-Call" (2021).
- t3ssel8r, "Giving Personality to Procedural Animations using Math" (2022).
- Andrist, Pejsa, Mutlu, Gleicher, "A Head-Eye Coordination Model for Animating Gaze Shifts of Virtual
  Characters" (2012); Pejsa et al., "Gaze and Attention Management for Embodied Conversational Agents"
  (ACM TiiS 2015); Bahill, Clark, Stark, "The main sequence" (1975); Lee, Badler, Badler, "Eyes Alive"
  (SIGGRAPH 2002).
- Egges, Molet, Magnenat-Thalmann, "Personalised Real-Time Idle Motion Synthesis" (PG 2004); Winter,
  "Human balance and posture control during standing and walking" (1995); Flash and Hogan (1985).
- Johansen, "Automated Semi-Procedural Animation for Character Locomotion" (2009); Bollo, "Inertialization"
  (GDC 2018).
- Unreal Engine docs: AnimNode_FootPlacement, Pose Warping, Control Rig Full Body IK.
- Unity Animation Rigging docs: Multi-Aim, Damped Transform, Two Bone IK constraints.
- Godot: "Inverse kinematics returns to Godot 4.6" (godotengine.org, 2026).
