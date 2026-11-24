"""Hand-written replacements for single guest instructions, keyed by guest address (gen.py applies them while lifting).

Metroid Prime Remastered v1.0.0 (main at 0x7000000000).
"""

# The simulation step. The game builds its 1/60 s tick as `movz wN, #0x8889; movk wN, #0x3c88, lsl #16` (0x3c888889)
# wherever it needs it; at each site that means one simulation tick or one frame, the movk instead takes the step for
# the chosen frame rate (runtime/hle/game.c rt_sim_step_bits: 1/60, or 1/120 at 120 fps). Left alone: the motion
# sensor's 60 Hz sample period (CNXSixAxisAnalysis::UpdateMotion, 0x7000359e50).
STEP_SITES = {
    0x70000fe264: (8, False),   # CGraphicsServerDevice::BeginTargetFrame
    0x70000fe4a0: (8, False),   # CGraphicsServerDevice::GetRenderFrameIntervalTime
    0x70000ff2f8: (9, False),   # CGraphicsServerDevice::TickMainThreadMaintenance
    0x70003c81a0: (8, False),   # CGameArchitectureSupport::GetSimulationTimeStep
    0x70003c81b0: (8, False),   # CGameArchitectureSupport::GetTotalSimulationTimeStep
    # 0x70003c8234 (CGameArchitectureSupport::Update: the frame's elapsed time, clamped to one tick): below
    0x70003c8308: (27, False),  # CGameArchitectureSupport::Update: the tick
    # 0x70003c8350 (CGameArchitectureSupport::Update: minus one tick, 0xbc888889, each pass of the tick loop): below
    0x70006ce284: (8, False),   # NPlayerMovementHelpers::ComputeFramesPerBeatForBPM
    0x7000b30424: (8, False),   # CGameRenderJob::RenderGamePass
    0x7000b7c470: (8, False),   # CProjectileWeapon::GetTickTime
    0x7000c4e7d8: (8, False),   # CAnimDataMP1::OnUserEvent
    0x7000c4e7f8: (22, False),  # CAnimDataMP1::OnUserEvent
    0x7000c5abd4: (9, False),   # CStateManagerGameLogicMP1::CStateManagerGameLogicMP1
    0x7000cb69e0: (8, False),   # CAimingCursorMP1::Update
    0x7000cb6a24: (8, False),   # CAimingCursorMP1::Update
    0x7000fa1474: (8, False),   # CGameRenderJobMP1::RenderPauseScreenElements
    0x7000fa18bc: (20, False),  # CGameRenderJobMP1::RenderPauseScreenElements
    0x700101e560: (9, False),   # CNewFlameThrowerMP1::AcceptScriptMsg
}


def _step(gen, a):
    rd, neg = STEP_SITES[a]
    return f'{{ extern unsigned rt_sim_step_bits(int); c->x[{rd}] = rt_sim_step_bits({int(neg)}); }}'


PATCHES = {a: _step for a in STEP_SITES}

# CPlayerMP1::GetDampedClampedVelocityWR(mgr, dt), once per tick: in the air after a jump it takes 3.5 * |v| / mass
# off Samus' horizontal speed each call - per tick, where its other cases scale their friction by dt * 60. At 120 ticks
# a second she slowed twice as fast, and a running jump fell 20% short (issue #12). Its `fmul s8, s0, s1` (|v| / mass
# times 3.5) gets the same dt * 60 (runtime/hle/game.c rt_air_drag): unchanged at 60.
PATCHES[0x7000cee3ec] = lambda gen, a: '{ extern void rt_air_drag(Cpu *); rt_air_drag(c); }'

# CStateManager::BeginFrame (every gameplay frame; it also steps a running autosave): `mov x19, x0`, then the
# runtime's debug autosave (runtime/hle/game.c rt_debug_autosave, LARECOMP_AUTOSAVE_AT) - saving tested without
# reaching a save station.
PATCHES[0x7000b02dcc] = lambda gen, a: '{ c->x[19] = c->x[0]; extern void rt_debug_autosave(Cpu *); rt_debug_autosave(c); }'

# CPlayerMP1::UpdateCameraBob (each gameplay frame): `sub sp, sp, #0x90` after the runtime notes the player and the
# state manager (runtime/hle/game.c rt_player_frame: LARECOMP_POS_LOG, LARECOMP_TELEPORT).
PATCHES[0x7000cf315c] = lambda gen, a: '{ extern void rt_player_frame(Cpu *); rt_player_frame(c); c->sp -= 0x90; }'

# Areas (runtime/hle/game.c rt_area_load): CGameAreaManager::BeginAreaLoad's `mov x29, sp` (the area's CObjectId at
# [x1], the load type in w2) logs each area the game loads.
PATCHES[0x70003e4bec] = lambda gen, a: ('{ c->x[29] = c->sp; extern void rt_area_load(Cpu *); rt_area_load(c); }')

# CCameraManager::CheckForCinematicSkip: `tbz w0, #0` on the skip button's state; with LARECOMP_SKIP_CINEMATICS the
# button counts as pressed, so each cutscene (all skippable then: runtime/hle/game.c) ends on its first frame.
PATCHES[0x70004bd840] = lambda gen, a: ('{ extern int rt_skip_cinematics(void); if (rt_skip_cinematics()) c->x[0] |= 1; '
                                        'if ((c->x[0] & 1) == 0) goto L_70004bd788; }')

# CWorldStateMP1::SetCurrentArea (each gameplay frame, from GameMainLoop): `stp x1, x2, [x0, #0x10]` after the runtime
# notes the world state (runtime/hle/game.c rt_world_state: LARECOMP_SAVE_AREA sets its desired area for a save).
PATCHES[0x7000c82748] = lambda gen, a: ('{ extern void rt_world_state(Cpu *); rt_world_state(c); '
                                        'ST64(c->x[0] + 0x10, c->x[1]); ST64(c->x[0] + 0x18, c->x[2]); }')


# CGameArchitectureSupport::Update's step sites with the frame's timing for the runtime (runtime/hle/game.c
# rt_frame_step: LARECOMP_TICK_LOG): the elapsed time it measured (s8) and each pass of its tick loop.
PATCHES[0x70003c8234] = lambda gen, a: '{ extern unsigned rt_frame_step(Cpu *); c->x[8] = rt_frame_step(c); }'
PATCHES[0x70003c8350] = lambda gen, a: '{ extern unsigned rt_tick_pass(void); c->x[8] = rt_tick_pass(); }'
# ... and the tick body itself (`fmov s0, w27`: one simulation tick ran)
PATCHES[0x70003c8328] = lambda gen, a: ('{ extern void rt_tick_body(void); rt_tick_body(); '
                                        'c->v[0] = (__uint128_t)(uint32_t)c->x[27]; }')

# CMovieList::Update(float dt) (the movies' clock): `mov x29, sp` after the runtime logs dt (s0; LARECOMP_TICK_LOG)
PATCHES[0x70003669f4] = lambda gen, a: '{ c->x[29] = c->sp; extern void rt_movie_dt(Cpu *); rt_movie_dt(c); }'

# VR: the arm cannon in the right hand (runtime/hle/game.c rt_gun_transform, rt_cursor_point; Video tab "VR: aim with
# your hand"). CPlayerGunMP1::SetTransform(xf) - called directly from 18 places, so not through an override - first
# lets the runtime point x1 at the hand's transform, then its `ldp x8, x9, [x1]`; CAimingCursorMP1::GetCursorInWorld
# and ...ForGun (`add x0, x0, #0x94` / `#0xa0`: the point shots are aimed at) the hand's point instead, when it has one.
PATCHES[0x7000c9d374] = lambda gen, a: ('{ extern void rt_gun_transform(Cpu *); rt_gun_transform(c); '
                                        'c->x[8] = LD64(c->x[1]); c->x[9] = LD64(c->x[1] + 8); }')
PATCHES[0x7000cb6d10] = lambda gen, a: ('{ extern uint64_t rt_cursor_point(uint64_t); '
                                        'c->x[0] = rt_cursor_point(c->x[0] + 0x94); }')
PATCHES[0x7000cb6d18] = lambda gen, a: ('{ extern uint64_t rt_cursor_point(uint64_t); '
                                        'c->x[0] = rt_cursor_point(c->x[0] + 0xa0); }')
# ... and CPlayerMP1::UpdateGunTransform's last two calls, after it places the gun (SetTransform, above):
# UpdateAimTargetPrediction and UpdateAssistedAiming, which turn the gun toward the aim point with a fixed up
# (CTransform4f::LookAt - no roll). Skipped while the gun is in the hand (runtime/hle/game.c rt_gun_in_hand), as the
# Ryujinx VR mod skipped them: its UpdateGunTransform only placed the gun.
PATCHES[0x7000c69438] = lambda gen, a: ('{ extern int rt_gun_in_hand(void); extern void f_7000c68ae0(Cpu *); '
                                        'if (!rt_gun_in_hand()) { c->x[30] = 0x7000c6943cull; f_7000c68ae0(c); } }')
PATCHES[0x7000c69448] = lambda gen, a: ('{ extern int rt_gun_in_hand(void); extern void f_7000c68e74(Cpu *); '
                                        'if (!rt_gun_in_hand()) { c->x[30] = 0x7000c6944cull; f_7000c68e74(c); } }')
# ... and CAimingCursorMP1::GetCursorOnPlane (`add x0, x0, #0x10`: where the combat reticle is drawn) along the
# hand's pointing (runtime/hle/game.c rt_cursor_plane)
PATCHES[0x7000cb6d00] = lambda gen, a: ('{ extern uint64_t rt_cursor_plane(uint64_t); '
                                        'c->x[0] = rt_cursor_plane(c->x[0] + 0x10); }')
# ... and CPlayerGunMP1::Update, which in the pointer aiming mode (the player's +0x69c == 2) turns the drawn gun (gun +
# 0x1e4) toward GetCursorInWorldForGun - the hand's aim point - with the world's up (CTransform4f::LookAt: no roll).
# Where its two ways of doing that meet (`ldr x8, [x19, #0x520]`; x19 the gun, x23 = gun + 0x1e4), the runtime turns
# it about where it points by the hand's roll (runtime/hle/game.c rt_gun_roll).
PATCHES[0x7000c942e0] = lambda gen, a: ('{ extern void rt_gun_roll(Cpu *); rt_gun_roll(c); '
                                        'c->x[8] = LD64(c->x[19] + 0x520); }')
# ... and CPlayerGunMP1::FirePrimary, which with an aim target picked (the player's +0x5e0) and not locked on fires the
# beam along the gun's assisted-aiming transforms (gun + 0x184 / + 0x1b4: UpdateAssistedAiming's, skipped above, so
# stale) instead of at GetCursorInWorld: its `mov w22, #1` (that case) is 0 while the gun is in the hand, so the shot
# goes where the hand points. Locked on (its `and w22, w8, w9`) it fires at the player's +0x5e4, the target's place
# as UpdateAimTargetPrediction (skipped) predicts it - 0,0,0 then: 0 too, so the cursor's point, which locked on is the
# target (runtime/hle/game.c vr_aim). (FireSecondary's missiles always aim at the cursor - the aim target only homes
# them.)
PATCHES[0x7000c997a0] = lambda gen, a: '{ extern int rt_gun_in_hand(void); c->x[22] = rt_gun_in_hand() ? 0 : 1; }'
PATCHES[0x7000c99788] = lambda gen, a: ('{ extern int rt_gun_in_hand(void); '
                                        'c->x[22] = rt_gun_in_hand() ? 0 : (uint32_t)(c->x[8] & c->x[9]); }')
# ... and CAimingCursorMP1::Update, at its `stur w8, [x29, #-0xb8]` (the hit's id cleared), after it made its ray
# (from the camera through the cursor, normalized) and before it casts it (BuildNearList, RayWorldIntersectionFixed):
# with the gun in the hand the ray is the gun's, along the hand's pointing (runtime/hle/game.c rt_cursor_ray), so the
# cursor's point in the world - which shots aim at and the reticle shows - is what the hand points at.
PATCHES[0x7000cb6788] = lambda gen, a: ('{ ST32(c->x[29] - 0xb8, (uint32_t)c->x[8]); '
                                        'extern void rt_cursor_ray(Cpu *); rt_cursor_ray(c); }')
