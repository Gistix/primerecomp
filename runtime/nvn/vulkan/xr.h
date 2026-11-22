// VR headsets through OpenXR (xr.c). Windows builds with the OpenXR SDK's headers (Makefile VR=1: -DPRIME_VR); the
// loader (openxr_loader.dll) is opened at run time, so a computer without it - or without a headset - runs as before.
//
// The window's image (the game and the menu, as the window shows them) goes to the headset: in first-person play all
// around the player, drawn from where the head looks (runtime/hle/game.c turns the camera by the head; the aim stays
// the stick's), else on a screen floating in front. One image for both eyes so far. device.c makes the Vulkan
// instance and device through the OpenXR runtime (it has to pick the GPU the headset is on, and the extensions it
// needs), and calls the frame functions around each present.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

// Video tab "VR headset" on and the runtime has a headset: OpenXR made the instance / device (device.c), so VR is on
// for this run. Off: everything below does nothing.
bool xr_on(void);
// The headset's refresh rate rounded (Hz), once it is known (its first frame); 0 before, or without VR.
// runtime/hle/game.c: the game's frame rate follows it when it is one the game runs at.
int xr_display_hz(void);

// ---- device.c, at start-up (each returns false / leaves VR off when the runtime can't)
bool xr_setup(void);  // the loader, an instance with XR_KHR_vulkan_enable2, the headset (g_settings.vr)
VkResult xr_create_vk_instance(const VkInstanceCreateInfo *ci, VkInstance *out);
VkPhysicalDevice xr_vk_physical_device(VkInstance inst);  // the GPU the headset is on (VK_NULL_HANDLE: unknown)
VkResult xr_create_vk_device(VkPhysicalDevice phys, const VkDeviceCreateInfo *di, VkDevice *out);
void xr_start_session(VkInstance inst, VkPhysicalDevice phys, VkDevice dev, uint32_t qfam);
void xr_off(const char *why);  // something on the Vulkan side failed: no VR this run

// ---- each presented frame (device.c backend_present / vk_frame_end), on the presenting thread
void xr_frame_begin(void);  // events, xrWaitFrame (the headset's pacing), xrBeginFrame
// The finished window image (COLOR_ATTACHMENT_OPTIMAL, left so) into the headset's screen: the part of it at x, y,
// w x h (the game's image; the menu over it). Recorded into cb, which is submitted before xr_frame_end.
void xr_capture(VkCommandBuffer cb, VkImage img, VkFormat fmt, int x, int y, int w, int h);
void xr_frame_end(void);  // xrEndFrame with the screen (or nothing, a frame without a capture)

// ---- head tracking (Phase 2): the head turns the first-person camera, not the aim
// The game's update thread (runtime/hle/game.c vr_head_apply): the head's pose for the frame being made, relative to
// straight ahead (xr_recenter) - its rotation in the first-person camera's own axes (row-major 3x3) and its
// movement in OpenXR's axes (x right, y up, z back), game units. false: no pose (no VR, the headset off the head, not
// tracking).
bool xr_head(float rot[9], float pos[3], uint32_t *id);
int xr_cam_sign(void);  // the camera looks down -z (1) or +z (-1): read off the drawn frames
void xr_head_used(uint32_t id, const float cam[12]);  // the camera made with it (CTransform4f)
// Render thread (exec.c): the view-projection and projection of the frame's first world draw (column-major)
void xr_note_view(const float vp[16], const float p[16]);
void xr_recenter(void);  // straight ahead becomes where the head faces now (F7, Video tab)

// ---- stereo (Phase 3): exec.c draws each frame for both eyes (vk_set_stereo)
bool xr_stereo_wanted(void);  // the next frame for both eyes (in play, the Video tab's "VR stereo")
float xr_half_ipd(void);      // half the eyes' distance, game units
// the left and right game images (GENERAL layout), into the eyes' views; cb is submitted before xr_frame_end
void xr_capture_eyes(VkCommandBuffer cb, VkImage l, VkImage r, VkFormat fmt, uint32_t w, uint32_t h, bool flip);
bool xr_mat_inv4(const float m[16], float o[16]);  // 4x4 inverse (false: singular)
float xr_vfov(void);     // the eyes' vertical field of view, degrees (0: not known): the game's while VR is on

#ifdef __cplusplus
}
#endif
