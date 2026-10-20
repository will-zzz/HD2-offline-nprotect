// Interface of the render-camera hook DLL (hd2vr_cam*.dll), used by the VR module.
#pragma once
#include <windows.h>

// What the next game frame's view cameras render with. Stingray row-vector 4x4, camera-local:
// rows x (right), y (forward), z (up), translation (metres).
struct Hd2vrCamPose {
    float head[16];
    float vfov;        // vertical field of view in radians; 0 keeps the game's
    float aspect;      // width / height of the projection; 0 keeps the game's
    unsigned seq;      // echoed back by hd2vr_cam_used() once a frame renders with this pose
    int enabled;       // 0: render the game's own camera untouched
    // Yaw lock (body follows head): render as if the game camera's yaw were yawRef (radians, game
    // world, atan2(-fwd.x, fwd.y)) whatever yaw the game has this frame, so the view stays on the head
    // while the body (game camera) catches up. Rotates about the eye point.
    float yawRef;
    int yawLock;
};

// Info about a view-camera update: hd2vr_cam_used() reports the calling thread's own last one
// when that thread updates cameras (the render thread, which also presents), else the latest.
struct Hd2vrCamUsed {
    unsigned seq;      // the pose's seq (0 if the camera was untouched)
    DWORD tid;         // render thread
    LONG frames;       // count of view-camera updates so far
    float gameVfov;    // the game's own vertical fov (radians) and aspect, before any override
    float gameAspect;
    float gameYaw;     // the game camera's own yaw and pitch this update (radians), before our changes
    float gamePitch;
};

using Hd2vrCamSetPoseFn = void(__cdecl*)(const Hd2vrCamPose*);
using Hd2vrCamUsedFn = void(__cdecl*)(Hd2vrCamUsed*);
using Hd2vrCamActiveFn = int(__cdecl*)();
using Hd2vrCamFeaturesFn = int(__cdecl*)();   // hd2vr_cam_features(): bit 0 = yaw lock
