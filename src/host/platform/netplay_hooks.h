#pragma once

// Host-only switches read by engine code for two-player and lockstep play
// (online co-op, input recording and replay). Every use in engine code sits
// behind HOST_BUILD, so the Android matching target is unaffected.
//
// Defined in host/platform/input.cpp.

#ifdef HOST_BUILD

#include "nu2api/nucore/common.h"

#ifdef __cplusplus
extern "C" {
#endif

// Called once per engine frame at the start of NuPadUpdatePads, on the game
// thread, before input devices are read.
extern void (*g_host_pad_frame_hook)(void);

// When > 0, every engine frame advances game time by exactly this many seconds
// (or 1 / nuapi.max_fps while the game lowers its frame cap), instead of the
// measured wall-clock time. Required for lockstep determinism.
extern f32 g_host_forced_frametime;

// Nonzero when a second gamepad drives player 2 (local or online co-op).
extern i32 g_host_two_player;

// Nonzero during lockstep play: engine timers that normally read the wall
// clock count frames instead.
extern i32 g_host_lockstep_active;

#ifdef __cplusplus
}
#endif

#endif // HOST_BUILD
