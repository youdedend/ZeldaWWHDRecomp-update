// GamePad motion (accelerometer, gyroscope, orientation) from the host's sensors: the device's
// own, or a controller's. Samples go through a Mahony sensor fusion and come out in the units and
// axes of the VPAD library (VPADRead). Conventions follow Cemu's input/motion (MPL-2.0).
#pragma once

namespace motion {

// One sensor sample in SDL's controller axes (held flat in front of you: +x right, +y up out of
// the face, +z toward you): gyro in rad/s, acceleration in m/s² (gravity included). dt in seconds.
void sample(float dt, float gx, float gy, float gz, float ax, float ay, float az);
// forget the state (sensors switched off or another source): VPAD reports the resting values again
void reset();

// what VPADStatus carries
struct Vpad {
    float acc[3];       // in g
    float accMagnitude;
    float accAcceleration;
    float accXY[2];
    float gyro[3];      // revolutions per second
    float angle[3];     // revolutions (accumulated)
    float dir[9];       // attitude: x, y, z axis vectors
};
// false if there are no recent samples (the caller writes the resting values)
bool vpad(Vpad& out);

// mods/camera.cpp, once per logic step: the game aims now (first person or an item aim)
// VPADRead / KPADReadEx, each fresh read: the right stick the game aims with (GamePad or Pro
// Controller). The game ignores the gyro while it is outside its 0.1 dead zone; a diagnostic
// says so after 2 s (stick drift)
void set_aiming(bool aiming);
void right_stick(float x, float y);

}  // namespace motion
