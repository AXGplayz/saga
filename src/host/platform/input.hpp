#pragma once

#include "nu2api/nucore/common.h"

// Selected host input backend used by the harness.
void HostInputReset();
void HostInputTap(i32 port, u32 buttons);
void HostInputSetHeld(i32 port, u32 buttons);
void HostInputSetAnalog(i32 port, f32 left_x, f32 left_y);
void HostInputSetKeyboardHeld(i32 port, u32 buttons);
void HostInputTouch(i32 x, i32 y, i32 width, i32 height);

// Physical gamepad state for a port (0 = player 1, 1 = player 2).
void HostInputSetGamepad(i32 port, u32 buttons, f32 left_x, f32 left_y);

// Two-player mode exposes a second gamepad device for player 2 and pins each
// player to their own pad. Call before the engine initialises input.
void HostInputEnableTwoPlayer(bool enabled);

// Lockstep support (netplay, input recording and replay).
// `frame` runs on the game thread once per engine frame, before pads are read.
void HostInputSetLockstepFrame(void (*frame)(void));
// Reads and consumes this frame's live input for a port.
void HostInputSampleLocal(i32 port, u32 *buttons, f32 *left_x, f32 *left_y);
// Overrides both ports with agreed lockstep samples until cleared.
void HostInputSetNetFrame(i32 port, u32 buttons, f32 left_x, f32 left_y);
void HostInputClearNetFrame();

// Weak extension points used by platform input backends.
void HostInputResetPlatform();
u32 HostInputConsumePlatform(i32 port);
