# Gyro aiming

On the Wii U, Wind Waker HD lets you aim in first person by moving the GamePad: the bow, hookshot,
boomerang, telescope, Picto Box, grappling hook and the plain first-person look (R3) all use it.
This port feeds the GamePad's motion from the host's sensors: on Android, the controller's motion
sensors if it has them, else this device's gyroscope and accelerometer
(`android/.../MotionInput.java`, the Motion option).

It works with both controller choices (Wii U GamePad or Wii U Pro Controller). In Pro Controller
mode the game's own **Options → Gyroscope** switch stays in the options menu and can be changed as
usual; its help text still says it "will have no effect when using the Wii U Pro Controller", which
is the Wii U's behaviour: in the port it does apply while motion data comes in. The game's own
**Options → Gyro** switch (on by default in the game) still decides whether the game uses the
motion, and the game ignores the motion while the right stick is pushed (outside a 0.1 dead zone;
a drifting stick does that too, see Troubleshooting).

## How the game reads the GamePad

From the decompilation (`wwhd_src`):

- The game imports `VPADRead` and no VPAD gyro setup function, so it runs with the library defaults.
- `ControllerMgr::calc` (02617AF4) copies the **direction matrix** of the newest `VPADStatus` sample
  (+0x6C, +0x78, +0x84: the GamePad's X, Y and Z axes) every frame (026173B0). The gyro rate (+0x38),
  angle (+0x44) and accelerometer (+0x1C) are not read.
- Its only reader is `dCamera_c::CalcSubjectAngle` (02506964), the first-person ("subject") camera:
  `R = Cᵀ·M` with `C` the matrix of the previous frame (`calibrate`, 026185BC, runs every frame and
  when first person starts), yaw input `(R[2][0] − R[1][0])·30` (rotation about the GamePad's Y or Z,
  so flat and upright holding both work, and rolling a flat GamePad turns too), pitch input
  `R[2][1]·30`. Both are used like a right-stick value. It runs only when the options byte `+5` (the
  in-game Gyro switch, default on) is set, the controller mode is not 0, and the right stick is within
  its 0.1 dead zone.
- The controller mode (ControllerMgr +0x1D0) is 0 when the game plays with a **Pro Controller**,
  1 or 2 with the GamePad and 3 with both. `02618604`, which hands the camera the calibrated
  orientation, returns the identity (no motion) in mode 0: a real Pro Controller has no gyro and the
  GamePad lies on the table. The matrix is still copied every frame in that mode.
- Every item aim goes through that camera; no item code reads the gyro itself.

So what matters is how the direction matrix turns from one frame to the next, in the GamePad's own
frame. Measured in R3 look on the upstream port: a GamePad turn of 60° right turned the view 113°
right, a tilt of 20° up 29° up. There is no sensitivity setting: the view follows the device about
as a real GamePad would (the game turns its first-person view about 1.9 times the GamePad's
left/right turn and 1.5 times its up/down tilt).

## How the port does it

- `runtime/src/motion.cpp`: the sensor fusion (Mahony, with a gyro bias estimate learnt while the
  device rests, and a noise floor) and the conversion to VPAD's axes and units, following Cemu
  (MPL-2.0, as this port). Samples with a non-positive dt are dropped at the source, and samples
  with a non-finite component are dropped here, so a glitching sensor cannot freeze or poison the
  motion. `reset()` forgets the state when the sensors switch off or the source changes.
- `runtime/src/mods/gyro_pro.cpp`: in Pro Controller mode, while motion data comes in, the game's
  "is the Pro Controller active?" check (02617AE4) is answered "no" for calls from the gyro code
  (the aiming block 0270D000-02718000 and the motion update / attitude sites), so the game's own
  motion update, attitude copy and aiming path run exactly as with the GamePad. Everything else
  still sees the Pro Controller. `WWHD_NO_PRO_GYRO=1` turns it off.
- Diagnostics: `motion::set_aiming` (from `mods/camera.cpp`, once per logic step) and
  `motion::right_stick` (from `VPADRead` / `KPADReadEx`, each fresh read) feed the `[gyro]` log:
  when the right stick has rested outside the game's 0.1 dead zone for 2 s while aiming with motion
  flowing, the log says the game ignores the gyro until the stick returns to the centre (stick
  drift?). `WWHD_GYRO_LOG=1` logs the raw sample, dt and bias twice a second;
  `WWHD_LOG_PRO_GYRO=1` logs who asks for the Pro Controller state.

Upstream differences (kept out on purpose): the desktop port turns a virtual GamePad by an aim
worked out per source, with axis modes (player space / yaw / roll), a sensitivity setting and a
direct hook into `02618604` (`docs/gyro.md` there, issues #45 and #71). This tree feeds the fused,
hardware-like orientation straight to `VPADRead`, as Cemu does, so the game turns exactly as on the
console however the device is held; the axis-mode and sensitivity machinery does not apply.

## Troubleshooting

- **The gyro stops working after a while.** The log (`[gyro]` lines) says whether the game ignores
  it because of the right stick ("the right stick has rested at x, y for 2 s while aiming": a
  drifting stick does this). Otherwise the samples stopped: the device's sensors may have been
  switched off (battery saver, another app), or the controller went to sleep; pausing and resuming
  the app re-reads them.
- **The view drifts while the device rests.** Put it down for a second: the gyro's offset (bias) is
  learnt anew while it rests. (The view itself never needs recentering: the game only follows the
  motion.)
- **More detail:** `WWHD_GYRO_LOG=1` logs the raw gyro and accelerometer samples, dt and the bias
  twice a second.
