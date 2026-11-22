// VR headsets through OpenXR: see xr.h. Phase 1 shows the window's image on a screen in front of the player (a quad
// layer, still in the room: the player can look around it); the game is drawn as on a monitor.
#include "xr.h"

#include <math.h>

#ifndef PRIME_VR
// (a build without the OpenXR headers - macOS, Linux, Android, or Windows without the SDK: no VR)
bool xr_on(void) { return false; }
int xr_display_hz(void) { return 0; }
int xr_eye_height(void) { return 0; }
float xr_shape(void) { return 0; }
bool xr_setup(void) { return false; }
VkResult xr_create_vk_instance(const VkInstanceCreateInfo *ci, VkInstance *out) { return vkCreateInstance(ci, NULL, out); }
VkPhysicalDevice xr_vk_physical_device(VkInstance inst) { (void)inst; return VK_NULL_HANDLE; }
VkResult xr_create_vk_device(VkPhysicalDevice phys, const VkDeviceCreateInfo *di, VkDevice *out) {
    return vkCreateDevice(phys, di, NULL, out);
}
void xr_start_session(VkInstance inst, VkPhysicalDevice phys, VkDevice dev, uint32_t qfam) { (void)inst, (void)phys, (void)dev, (void)qfam; }
void xr_off(const char *why) { (void)why; }
void xr_frame_begin(void) {}
void xr_capture(VkCommandBuffer cb, VkImage img, VkFormat fmt, int x, int y, int w, int h) {
    (void)cb, (void)img, (void)fmt, (void)x, (void)y, (void)w, (void)h;
}
void xr_frame_end(void) {}
void xr_recenter(void) {}
float xr_vfov(void) { return 0; }
int xr_cam_sign(void) { return 1; }
bool xr_head(float rot[9], float pos[3], uint32_t *id) { (void)rot, (void)pos, (void)id; return false; }
void xr_head_used(uint32_t id, const float m[12]) { (void)id, (void)m; }
void xr_note_view(const float vp[16], const float p[16]) { (void)vp, (void)p; }
float xr_half_ipd(void) { return 0; }
bool xr_stereo_wanted(void) { return false; }
void xr_capture_eyes(VkCommandBuffer cb, VkImage l, VkImage r, VkFormat fmt, uint32_t w, uint32_t h, bool flip) {
    (void)cb, (void)l, (void)r, (void)fmt, (void)w, (void)h, (void)flip;
}

#else
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XR_NO_PROTOTYPES  // (every function through xrGetInstanceProcAddr: the loader is opened at run time)
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "../../rt.h"
#include "../../settings.h"

bool xr_mat_inv4(const float m[16], float o[16]);

// kernel.c: this thread's time blocked (waiting for the headset is idle time, not work: exec.c vk_pace_frame)
extern __thread uint64_t g_blocked_ns;

static struct {
    SDL_SharedObject *lib;
    PFN_xrGetInstanceProcAddr gipa;
    XrInstance inst;
    XrSystemId sys;
    XrSession session;
    XrSpace space;          // LOCAL: the player's position when the session began (the runtime's recenter moves it)
    XrSpace view;           // VIEW: the head (between the eyes)
    XrSessionState state;
    bool on, running, in_frame, should_render, captured;
    XrTime display_time;
    XrDuration period;      // the headset's frame period as it last said (twice as long, or more, while it throttles)
    int hz;
    bool throttled;         // the runtime holds frames back (the headset is off the player's head): not its pace
    uint64_t last_wait_ns;  // (when xrWaitFrame last returned: throttled frames go by without one)
    XrEnvironmentBlendMode blend;
    float vfov_deg;         // the eyes' vertical field of view (the widest), degrees: the game's while VR is on
    float ipd_m;            // between the eyes, metres (the runtime's views)
    uint32_t rec_w, rec_h;  // the runtime's recommended eye image size
    float view_aspect;      // the eyes' view (both), width over height
    _Atomic float shape;    // the game image's shape over 16:9 in play (the eyes' view's), 0 elsewhere: xr_shape
    // stereo (exec.c, each frame drawn for both eyes): the left and right game images, a swapchain each
    struct { XrSwapchain sc; int64_t fmt; uint32_t w, h, n; XrSwapchainImageVulkan2KHR *img; } eye[2];
    bool eyes_captured;
    // the screen: a swapchain the size of the window's game image, in the window's channel order, sRGB
    XrSwapchain sc;
    int64_t sc_fmt;
    uint32_t sc_w, sc_h, sc_n;
    XrSwapchainImageVulkan2KHR *sc_img;
} X;
static SDL_Mutex *hmx;  // (head poses: the game's update thread writes them, the render thread matches its frames)

#define XR_FN(name) static PFN_##name p_##name
XR_FN(xrEnumerateInstanceExtensionProperties);
XR_FN(xrCreateInstance);
XR_FN(xrDestroyInstance);
XR_FN(xrGetInstanceProperties);
XR_FN(xrGetSystem);
XR_FN(xrGetSystemProperties);
XR_FN(xrResultToString);
XR_FN(xrPollEvent);
XR_FN(xrCreateSession);
XR_FN(xrDestroySession);
XR_FN(xrBeginSession);
XR_FN(xrEndSession);
XR_FN(xrCreateReferenceSpace);
XR_FN(xrEnumerateEnvironmentBlendModes);
XR_FN(xrEnumerateSwapchainFormats);
XR_FN(xrCreateSwapchain);
XR_FN(xrDestroySwapchain);
XR_FN(xrEnumerateSwapchainImages);
XR_FN(xrAcquireSwapchainImage);
XR_FN(xrWaitSwapchainImage);
XR_FN(xrReleaseSwapchainImage);
XR_FN(xrWaitFrame);
XR_FN(xrBeginFrame);
XR_FN(xrEndFrame);
XR_FN(xrLocateSpace);
XR_FN(xrLocateViews);
XR_FN(xrEnumerateViewConfigurationViews);
XR_FN(xrGetVulkanGraphicsRequirements2KHR);
XR_FN(xrCreateVulkanInstanceKHR);
XR_FN(xrGetVulkanGraphicsDevice2KHR);
XR_FN(xrCreateVulkanDeviceKHR);

static bool load(XrInstance inst, const char *name, PFN_xrVoidFunction *f) {
    *f = NULL;
    return X.gipa(inst, name, f) == XR_SUCCESS && *f;
}
#define LOAD(inst, name) load(inst, #name, (PFN_xrVoidFunction *)&p_##name)

static const char *res(XrResult r) {
    static char s[XR_MAX_RESULT_STRING_SIZE];
    if (!p_xrResultToString || !X.inst || p_xrResultToString(X.inst, r, s) != XR_SUCCESS) snprintf(s, sizeof s, "%d", (int)r);
    return s;
}

bool xr_on(void) { return X.on; }
int xr_display_hz(void) { return X.on && X.running && !X.throttled ? X.hz : 0; }
// The game's render height while VR is on (device.c): the runtime's recommended eye image's, times the Video tab's
// "VR resolution" - the eye's view is about the game image's height, so that's about a pixel per display pixel.
// 0: not in VR (the window's size, as before).
int xr_eye_height(void) {
    if (!X.on || !X.rec_h) return 0;
    int pct = g_settings.vr_res_pct >= VR_RES_MIN && g_settings.vr_res_pct <= VR_RES_MAX ? g_settings.vr_res_pct : 80;
    return (int)(X.rec_h * pct / 100);
}

void xr_off(const char *why) {
    if (!X.inst) return;
    LOG("[xr] VR off: %s\n", why);
    if (X.sc) p_xrDestroySwapchain(X.sc);
    if (X.session) p_xrDestroySession(X.session);
    if (p_xrDestroyInstance) p_xrDestroyInstance(X.inst);  // (missing: xr_setup found the runtime lacking)
    free(X.sc_img);
    SDL_SharedObject *lib = X.lib;
    PFN_xrGetInstanceProcAddr gipa = X.gipa;
    memset(&X, 0, sizeof X);
    X.lib = lib, X.gipa = gipa;  // (the loader stays open: nothing else of it is used)
}

bool xr_setup(void) {
    if (!g_settings.vr || getenv("LARECOMP_NO_VR")) return false;
    // the Khronos loader as the OpenXR SDK ships it, or as MSYS2 builds it
    static const char *const names[] = {"openxr_loader.dll", "libopenxr_loader.dll"};
    for (int i = 0; i < 2 && !X.lib; i++) X.lib = SDL_LoadObject(names[i]);
    if (!X.lib) { LOG("[xr] no OpenXR loader (openxr_loader.dll next to the program): VR off\n"); return false; }
    X.gipa = (PFN_xrGetInstanceProcAddr)SDL_LoadFunction(X.lib, "xrGetInstanceProcAddr");
    if (!X.gipa || !LOAD(XR_NULL_HANDLE, xrEnumerateInstanceExtensionProperties) || !LOAD(XR_NULL_HANDLE, xrCreateInstance)) {
        LOG("[xr] the OpenXR loader has no xrGetInstanceProcAddr: VR off\n");
        return false;
    }
    uint32_t n = 0;
    XrResult r = p_xrEnumerateInstanceExtensionProperties(NULL, 0, &n, NULL);
    if (XR_FAILED(r)) { LOG("[xr] no OpenXR runtime (%d): VR off - is SteamVR, Meta Quest Link or Virtual Desktop set as the "
                            "OpenXR runtime?\n", (int)r); return false; }
    XrExtensionProperties *ep = calloc(n ? n : 1, sizeof *ep);
    for (uint32_t i = 0; i < n; i++) ep[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    p_xrEnumerateInstanceExtensionProperties(NULL, n, &n, ep);
    bool vk2 = false;
    for (uint32_t i = 0; i < n; i++) vk2 |= !strcmp(ep[i].extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
    free(ep);
    if (!vk2) { LOG("[xr] the OpenXR runtime has no %s: VR off\n", XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME); return false; }

    const char *ext[] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    XrInstanceCreateInfo ci = {.type = XR_TYPE_INSTANCE_CREATE_INFO, .enabledExtensionCount = 1, .enabledExtensionNames = ext};
    snprintf(ci.applicationInfo.applicationName, sizeof ci.applicationInfo.applicationName, "PrimeRecomp");
    snprintf(ci.applicationInfo.engineName, sizeof ci.applicationInfo.engineName, "PrimeRecomp");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);  // (1.0: what every runtime still takes)
    r = p_xrCreateInstance(&ci, &X.inst);
    if (XR_FAILED(r)) { LOG("[xr] xrCreateInstance: %d: VR off\n", (int)r); X.inst = XR_NULL_HANDLE; return false; }
    bool ok = LOAD(X.inst, xrDestroyInstance) && LOAD(X.inst, xrGetInstanceProperties) && LOAD(X.inst, xrGetSystem) &&
              LOAD(X.inst, xrGetSystemProperties) && LOAD(X.inst, xrResultToString) && LOAD(X.inst, xrPollEvent) &&
              LOAD(X.inst, xrCreateSession) && LOAD(X.inst, xrDestroySession) && LOAD(X.inst, xrBeginSession) &&
              LOAD(X.inst, xrEndSession) && LOAD(X.inst, xrCreateReferenceSpace) &&
              LOAD(X.inst, xrEnumerateEnvironmentBlendModes) && LOAD(X.inst, xrEnumerateSwapchainFormats) &&
              LOAD(X.inst, xrCreateSwapchain) && LOAD(X.inst, xrDestroySwapchain) &&
              LOAD(X.inst, xrEnumerateSwapchainImages) && LOAD(X.inst, xrAcquireSwapchainImage) &&
              LOAD(X.inst, xrWaitSwapchainImage) && LOAD(X.inst, xrReleaseSwapchainImage) && LOAD(X.inst, xrWaitFrame) &&
              LOAD(X.inst, xrBeginFrame) && LOAD(X.inst, xrEndFrame) && LOAD(X.inst, xrLocateSpace) &&
              LOAD(X.inst, xrLocateViews) && LOAD(X.inst, xrEnumerateViewConfigurationViews) &&
              LOAD(X.inst, xrGetVulkanGraphicsRequirements2KHR) && LOAD(X.inst, xrCreateVulkanInstanceKHR) &&
              LOAD(X.inst, xrGetVulkanGraphicsDevice2KHR) && LOAD(X.inst, xrCreateVulkanDeviceKHR);
    if (!ok) { X.on = true; xr_off("the runtime lacks core functions"); return false; }
    XrInstanceProperties ip = {.type = XR_TYPE_INSTANCE_PROPERTIES};
    p_xrGetInstanceProperties(X.inst, &ip);

    XrSystemGetInfo gi = {.type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    r = p_xrGetSystem(X.inst, &gi, &X.sys);
    if (XR_FAILED(r)) {  // (XR_ERROR_FORM_FACTOR_UNAVAILABLE: the headset isn't connected / its software isn't running)
        char why[160];
        snprintf(why, sizeof why, "%s has no headset (%s)", ip.runtimeName, res(r));
        X.on = true;
        xr_off(why);
        return false;
    }
    XrSystemProperties sp = {.type = XR_TYPE_SYSTEM_PROPERTIES};
    p_xrGetSystemProperties(X.inst, X.sys, &sp);
    // (required before a session: the Vulkan versions the runtime works with)
    XrGraphicsRequirementsVulkan2KHR req = {.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    p_xrGetVulkanGraphicsRequirements2KHR(X.inst, X.sys, &req);
    LOG("[xr] %s %u.%u.%u, headset %s (Vulkan %u.%u..%u.%u)\n", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
        XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion), sp.systemName,
        XR_VERSION_MAJOR(req.minApiVersionSupported), XR_VERSION_MINOR(req.minApiVersionSupported),
        XR_VERSION_MAJOR(req.maxApiVersionSupported), XR_VERSION_MINOR(req.maxApiVersionSupported));
    X.on = true;
    return true;
}

// The runtime makes the instance / device: it adds the Vulkan extensions it needs to the ones device.c asks for.
VkResult xr_create_vk_instance(const VkInstanceCreateInfo *ci, VkInstance *out) {
    if (!X.on) return vkCreateInstance(ci, NULL, out);
    XrVulkanInstanceCreateInfoKHR xi = {.type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR, .systemId = X.sys,
                                        .pfnGetInstanceProcAddr = vkGetInstanceProcAddr, .vulkanCreateInfo = ci};
    VkResult vr = VK_ERROR_INITIALIZATION_FAILED;
    XrResult r = p_xrCreateVulkanInstanceKHR(X.inst, &xi, out, &vr);
    if (XR_FAILED(r) || vr != VK_SUCCESS) {
        LOG("[xr] xrCreateVulkanInstanceKHR: %s, Vulkan %d\n", res(r), vr);
        xr_off("the runtime couldn't make the Vulkan instance");
        return vkCreateInstance(ci, NULL, out);
    }
    return VK_SUCCESS;
}

VkPhysicalDevice xr_vk_physical_device(VkInstance inst) {
    if (!X.on) return VK_NULL_HANDLE;
    XrVulkanGraphicsDeviceGetInfoKHR gi = {.type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR, .systemId = X.sys,
                                           .vulkanInstance = inst};
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    XrResult r = p_xrGetVulkanGraphicsDevice2KHR(X.inst, &gi, &pd);
    if (XR_FAILED(r)) LOG("[xr] xrGetVulkanGraphicsDevice2KHR: %s\n", res(r));
    return pd;
}

VkResult xr_create_vk_device(VkPhysicalDevice phys, const VkDeviceCreateInfo *di, VkDevice *out) {
    if (!X.on) return vkCreateDevice(phys, di, NULL, out);
    XrVulkanDeviceCreateInfoKHR xi = {.type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR, .systemId = X.sys,
                                      .pfnGetInstanceProcAddr = vkGetInstanceProcAddr, .vulkanPhysicalDevice = phys,
                                      .vulkanCreateInfo = di};
    VkResult vr = VK_ERROR_INITIALIZATION_FAILED;
    XrResult r = p_xrCreateVulkanDeviceKHR(X.inst, &xi, out, &vr);
    if (XR_FAILED(r) || vr != VK_SUCCESS) {
        LOG("[xr] xrCreateVulkanDeviceKHR: %s, Vulkan %d\n", res(r), vr);
        xr_off("the runtime couldn't make the Vulkan device");
        return vkCreateDevice(phys, di, NULL, out);
    }
    return VK_SUCCESS;
}

void xr_start_session(VkInstance inst, VkPhysicalDevice phys, VkDevice dev, uint32_t qfam) {
    if (!X.on) return;
    XrGraphicsBindingVulkan2KHR gb = {.type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR, .instance = inst,
                                      .physicalDevice = phys, .device = dev, .queueFamilyIndex = qfam, .queueIndex = 0};
    XrSessionCreateInfo si = {.type = XR_TYPE_SESSION_CREATE_INFO, .next = &gb, .systemId = X.sys};
    XrResult r = p_xrCreateSession(X.inst, &si, &X.session);
    if (XR_FAILED(r)) { LOG("[xr] xrCreateSession: %s\n", res(r)); X.session = XR_NULL_HANDLE; xr_off("no session"); return; }
    XrReferenceSpaceCreateInfo rs = {.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO, .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL,
                                     .poseInReferenceSpace = {.orientation = {0, 0, 0, 1}}};
    r = p_xrCreateReferenceSpace(X.session, &rs, &X.space);
    if (XR_FAILED(r)) { LOG("[xr] xrCreateReferenceSpace: %s\n", res(r)); xr_off("no reference space"); return; }
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    r = p_xrCreateReferenceSpace(X.session, &rs, &X.view);
    if (XR_FAILED(r)) { LOG("[xr] xrCreateReferenceSpace (view): %s\n", res(r)); xr_off("no view space"); return; }
    uint32_t n = 0;
    XrEnvironmentBlendMode bm[8];
    p_xrEnumerateEnvironmentBlendModes(X.inst, X.sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 8, &n, bm);
    X.blend = n ? bm[0] : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;  // (the runtime's preferred one first)
    if (!hmx) hmx = SDL_CreateMutex();  // (head tracking: before any game thread asks for a pose)
    {   // the runtime's recommended size of an eye's image (xr_eye_height: the game's render size in VR)
        XrViewConfigurationView vv[2] = {{.type = XR_TYPE_VIEW_CONFIGURATION_VIEW}, {.type = XR_TYPE_VIEW_CONFIGURATION_VIEW}};
        uint32_t nv = 0;
        if (XR_SUCCEEDED(p_xrEnumerateViewConfigurationViews(X.inst, X.sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
                                                              &nv, vv)) && nv >= 1) {
            X.rec_w = vv[0].recommendedImageRectWidth, X.rec_h = vv[0].recommendedImageRectHeight;
            LOG("[xr] recommended eye image %ux%u\n", X.rec_w, X.rec_h);
        }
    }
    LOG("[xr] session made; waiting for the headset\n");
}

static void poll_events(void) {
    XrEventDataBuffer ev = {.type = XR_TYPE_EVENT_DATA_BUFFER};
    while (X.on && p_xrPollEvent(X.inst, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const XrEventDataSessionStateChanged *e = (const XrEventDataSessionStateChanged *)&ev;
            X.state = e->state;
            LOG("[xr] session state %d\n", (int)e->state);
            if (e->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = {.type = XR_TYPE_SESSION_BEGIN_INFO,
                                         .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
                XrResult r = p_xrBeginSession(X.session, &bi);
                X.running = XR_SUCCEEDED(r);
                if (!X.running) LOG("[xr] xrBeginSession: %s\n", res(r));
            } else if (e->state == XR_SESSION_STATE_STOPPING) {
                p_xrEndSession(X.session);
                X.running = false;
            } else if (e->state == XR_SESSION_STATE_EXITING || e->state == XR_SESSION_STATE_LOSS_PENDING) {
                // (the headset's software quit or the headset went away: the window carries on alone)
                X.running = false;
                xr_off(e->state == XR_SESSION_STATE_EXITING ? "the runtime ended the session" : "the headset was lost");
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            X.running = false;
            xr_off("the OpenXR runtime is going away");
        } else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            xr_recenter();  // (the runtime's own recenter - holding the Oculus button - moved LOCAL: ours follows)
        }
        ev = (XrEventDataBuffer){.type = XR_TYPE_EVENT_DATA_BUFFER};
    }
}

void xr_frame_begin(void) {
    X.in_frame = X.captured = X.eyes_captured = false;
    if (!X.on) return;
    poll_events();
    if (!X.running) return;
    // A throttling runtime (the headset off the player's head: Virtual Desktop gives a frame every 100 ms) would hold
    // the game to its pace - 0.2x speed. Then only the frames due go to it; the others are the game's own (game.c
    // paces them: xr_display_hz is 0 meanwhile).
    uint64_t now = SDL_GetTicksNS();
    if (X.throttled && X.last_wait_ns && now - X.last_wait_ns + 2000000 < (uint64_t)X.period) return;
    XrFrameState fs = {.type = XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo wi = {.type = XR_TYPE_FRAME_WAIT_INFO};
    uint64_t t0 = now;
    XrResult r = p_xrWaitFrame(X.session, &wi, &fs);
    X.last_wait_ns = SDL_GetTicksNS();
    g_blocked_ns += X.last_wait_ns - t0;
    if (XR_FAILED(r)) { LOG("[xr] xrWaitFrame: %s\n", res(r)); return; }
    X.display_time = fs.predictedDisplayTime;
    X.should_render = fs.shouldRender;
    if (fs.predictedDisplayPeriod > 0) {
        // the refresh rate: the shortest period seen. A runtime that finds the frames late shows each for two
        // refreshes and says so in the period (Virtual Desktop: 72 <-> 36 Hz) - that's no new refresh rate, and
        // following it would halve the game's frame rate (runtime/hle/game.c) and keep it late
        X.period = fs.predictedDisplayPeriod;
        int hz = (int)lround(1e9 / (double)fs.predictedDisplayPeriod);
        if (hz > X.hz) LOG("[xr] headset at %d Hz\n", hz), X.hz = hz;
        bool thr = X.hz && hz * 3 < X.hz;  // (a third of the refresh rate or less: held back, not just late)
        if (thr != X.throttled) LOG("[xr] %s\n", thr ? "the headset holds frames back (off the head?): the game keeps its own pace"
                                                     : "the headset takes every frame again");
        X.throttled = thr;
    }
    {   // the eyes' field of view (the game's, while VR is on: runtime/hle/game.c rt_fov)
        XrViewLocateInfo li = {.type = XR_TYPE_VIEW_LOCATE_INFO, .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                               .displayTime = X.display_time, .space = X.space};
        XrViewState vs = {.type = XR_TYPE_VIEW_STATE};
        XrView v[2] = {{.type = XR_TYPE_VIEW}, {.type = XR_TYPE_VIEW}};
        uint32_t n = 0;
        if (XR_SUCCEEDED(p_xrLocateViews(X.session, &li, &vs, 2, &n, v)) && n == 2) {
            // the game's view covers both eyes' with its symmetric projection: half-angles the largest of the eyes'
            // up / down and left / right (the eyes' views lean outwards: each eye's outer side is the wider)
            float tu = 0, th = 0;
            for (int i = 0; i < 2; i++) {
                tu = fmaxf(tu, fmaxf(tanf(v[i].fov.angleUp), tanf(-v[i].fov.angleDown)));
                th = fmaxf(th, fmaxf(tanf(v[i].fov.angleRight), tanf(-v[i].fov.angleLeft)));
            }
            float deg = 2 * atanf(tu) * 57.29578f;
            if (deg > 30 && th > 0.3f) {
                if (fabsf(deg - X.vfov_deg) > 0.5f)
                    LOG("[xr] eyes' view %.1f x %.1f degrees (the game's: %.1f tall, %.3f wide for 1 tall)\n",
                        2 * atanf(th) * 57.29578f, deg, deg, th / tu);
                X.vfov_deg = deg, X.view_aspect = th / tu;
            }
            float dx = v[1].pose.position.x - v[0].pose.position.x, dy = v[1].pose.position.y - v[0].pose.position.y,
                  dz = v[1].pose.position.z - v[0].pose.position.z, ipd = sqrtf(dx * dx + dy * dy + dz * dz);
            if (ipd > 0.03f && ipd < 0.09f) {
                if (fabsf(ipd - X.ipd_m) > 0.001f) LOG("[xr] eyes %.1f mm apart\n", ipd * 1000);
                X.ipd_m = ipd;
            }
        }
    }
    XrFrameBeginInfo bi = {.type = XR_TYPE_FRAME_BEGIN_INFO};
    t0 = SDL_GetTicksNS();
    r = p_xrBeginFrame(X.session, &bi);
    g_blocked_ns += SDL_GetTicksNS() - t0;
    if (XR_FAILED(r)) { LOG("[xr] xrBeginFrame: %s\n", res(r)); return; }
    X.in_frame = true;
}

// The window's 8-bit format's sRGB twin (same channel order): the window's bytes are already sRGB-encoded, so they're
// copied as they are and the runtime reads them as sRGB
static VkFormat srgb_of(VkFormat f) {
    switch (f) {
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32: case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_SRGB_PACK32;
    default: return VK_FORMAT_UNDEFINED;
    }
}

static bool make_screen(VkFormat win_fmt, uint32_t w, uint32_t h) {
    VkFormat want = srgb_of(win_fmt);
    if (X.sc && X.sc_fmt == want && X.sc_w == w && X.sc_h == h) return true;
    if (X.sc) p_xrDestroySwapchain(X.sc), X.sc = XR_NULL_HANDLE;
    free(X.sc_img), X.sc_img = NULL;
    static int warned;
    if (want == VK_FORMAT_UNDEFINED) {  // (an HDR window: VR turns HDR off, so only for a frame)
        if (!warned++) LOG("[xr] window format %d isn't 8-bit: no screen in the headset\n", win_fmt);
        return false;
    }
    uint32_t n = 0;
    p_xrEnumerateSwapchainFormats(X.session, 0, &n, NULL);
    int64_t *fm = calloc(n ? n : 1, sizeof *fm);
    p_xrEnumerateSwapchainFormats(X.session, n, &n, fm);
    bool have = false;
    for (uint32_t i = 0; i < n; i++) have |= fm[i] == want;
    free(fm);
    if (!have) {
        if (!warned++) LOG("[xr] the runtime has no swapchain format %d (the window's sRGB twin): no screen\n", want);
        return false;
    }
    XrSwapchainCreateInfo ci = {.type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
                                .usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT,
                                .format = want, .sampleCount = 1, .width = w, .height = h, .faceCount = 1, .arraySize = 1,
                                .mipCount = 1};
    XrResult r = p_xrCreateSwapchain(X.session, &ci, &X.sc);
    if (XR_FAILED(r)) { if (!warned++) LOG("[xr] xrCreateSwapchain %ux%u: %s\n", w, h, res(r)); X.sc = XR_NULL_HANDLE; return false; }
    p_xrEnumerateSwapchainImages(X.sc, 0, &X.sc_n, NULL);
    X.sc_img = calloc(X.sc_n ? X.sc_n : 1, sizeof *X.sc_img);
    for (uint32_t i = 0; i < X.sc_n; i++) X.sc_img[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
    p_xrEnumerateSwapchainImages(X.sc, X.sc_n, &X.sc_n, (XrSwapchainImageBaseHeader *)X.sc_img);
    X.sc_fmt = want, X.sc_w = w, X.sc_h = h;
    LOG("[xr] screen %ux%u (%u images)\n", w, h, X.sc_n);
    return true;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags2 src_access,
                    VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = src_access,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstAccessMask = dst_access,
                               .oldLayout = from, .newLayout = to, .image = img,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
}

// ---- stereo: the game's left and right images (exec.c draws each frame for both eyes), a swapchain each, as big as
// they are; copied byte for byte into the sRGB twin of their format (as the window's: srgb_of)
static bool make_eye(int e, VkFormat fmt, uint32_t w, uint32_t h) {
    VkFormat want = srgb_of(fmt);
    if (X.eye[e].sc && X.eye[e].fmt == want && X.eye[e].w == w && X.eye[e].h == h) return true;
    if (X.eye[e].sc) p_xrDestroySwapchain(X.eye[e].sc), X.eye[e].sc = XR_NULL_HANDLE;
    free(X.eye[e].img), X.eye[e].img = NULL;
    static int warned;
    if (want == VK_FORMAT_UNDEFINED) {
        if (!warned++) LOG("[xr] the game image's format %d isn't 8-bit RGBA: no stereo (both eyes the left's)\n", fmt);
        return false;
    }
    XrSwapchainCreateInfo ci = {.type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
                                .usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT,
                                .format = want, .sampleCount = 1, .width = w, .height = h, .faceCount = 1, .arraySize = 1,
                                .mipCount = 1};
    XrResult r = p_xrCreateSwapchain(X.session, &ci, &X.eye[e].sc);
    if (XR_FAILED(r)) { if (!warned++) LOG("[xr] eye swapchain %ux%u: %s\n", w, h, res(r)); X.eye[e].sc = XR_NULL_HANDLE; return false; }
    p_xrEnumerateSwapchainImages(X.eye[e].sc, 0, &X.eye[e].n, NULL);
    X.eye[e].img = calloc(X.eye[e].n ? X.eye[e].n : 1, sizeof *X.eye[e].img);
    for (uint32_t i = 0; i < X.eye[e].n; i++) X.eye[e].img[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
    p_xrEnumerateSwapchainImages(X.eye[e].sc, X.eye[e].n, &X.eye[e].n, (XrSwapchainImageBaseHeader *)X.eye[e].img);
    X.eye[e].fmt = want, X.eye[e].w = w, X.eye[e].h = h;
    if (e) LOG("[xr] eye images %ux%u\n", w, h);
    return true;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags2 src_access,
                    VkAccessFlags2 dst_access);

// The left and right game images (GENERAL layout, left so) into the eyes' swapchains: recorded into cb, which is
// submitted before xr_frame_end
void xr_capture_eyes(VkCommandBuffer cb, VkImage l, VkImage r, VkFormat fmt, uint32_t w, uint32_t h, bool flip) {
    if (!X.in_frame || !X.should_render || !l || !r || !w || !h) return;
    if (!make_eye(0, fmt, w, h) || !make_eye(1, fmt, w, h)) return;
    uint64_t t0 = SDL_GetTicksNS();  // (the runtime's waits: idle - xr_capture)
    for (int e = 0; e < 2; e++) {
        uint32_t i = 0;
        XrSwapchainImageAcquireInfo ai = {.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo wi = {.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION};
        if (XR_FAILED(p_xrAcquireSwapchainImage(X.eye[e].sc, &ai, &i)) || XR_FAILED(p_xrWaitSwapchainImage(X.eye[e].sc, &wi))) {
            // (the first one's back: released with nothing new in it - the layer then uses the left image's last)
            if (e) {
                XrSwapchainImageReleaseInfo ri = {.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                p_xrReleaseSwapchainImage(X.eye[0].sc, &ri);
            }
            g_blocked_ns += SDL_GetTicksNS() - t0;
            return;
        }
        VkImage src = e ? r : l, dst = X.eye[e].img[i].image;
        barrier(cb, src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        barrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        if (flip && (int64_t)fmt == X.eye[e].fmt) {
            // (the game's images upside down - its origin at the lower left: device.c's present flips them too - so
            // turned over on the way; a blit between the same format, texel for texel, is the copy, turned)
            VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}},
                              .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .dstOffsets = {{0, (int32_t)h, 0}, {(int32_t)w, 0, 1}}};
            vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
        } else {
            static int warned;
            if (flip && !warned++) LOG("[xr] the game image (format %d) can't be turned over on the way: upside down\n", fmt);
            VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .extent = {w, h, 1}};
            vkCmdCopyImage(cb, src, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
        }
        barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
    }
    g_blocked_ns += SDL_GetTicksNS() - t0;
    X.eyes_captured = true;
}

float xr_half_ipd(void) {  // (game units: the world scale's)
    float s = g_settings.vr_world_scale > 0.1f ? g_settings.vr_world_scale : 1.0f;
    return (X.ipd_m > 0 ? X.ipd_m : 0.063f) * 0.5f * s;
}

void xr_capture(VkCommandBuffer cb, VkImage img, VkFormat fmt, int x, int y, int w, int h) {
    if (!X.in_frame || !X.should_render || w <= 0 || h <= 0) return;
    if (!make_screen(fmt, (uint32_t)w, (uint32_t)h)) return;
    uint32_t i = 0;
    // (waits for the runtime - its compositor, Virtual Desktop's encoder - are idle time, as xrWaitFrame's: counted
    // as such, else exec.c vk_pace_frame took them for slow frames and put the game in its 30 fps mode)
    uint64_t t0 = SDL_GetTicksNS();
    XrSwapchainImageAcquireInfo ai = {.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(p_xrAcquireSwapchainImage(X.sc, &ai, &i))) return;
    XrSwapchainImageWaitInfo wi = {.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION};
    XrResult wr = p_xrWaitSwapchainImage(X.sc, &wi);
    g_blocked_ns += SDL_GetTicksNS() - t0;
    if (XR_FAILED(wr)) {
        XrSwapchainImageReleaseInfo ri = {.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        p_xrReleaseSwapchainImage(X.sc, &ri);
        return;
    }
    VkImage dst = X.sc_img[i].image;
    // the window image -> the screen's, byte for byte (UNORM and sRGB 8-bit formats are size-compatible); the runtime
    // gets its image back in COLOR_ATTACHMENT_OPTIMAL, as OpenXR asks
    barrier(cb, img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffset = {x, y, 0},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {(uint32_t)w, (uint32_t)h, 1}};
    vkCmdCopyImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
    barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_2_TRANSFER_READ_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    X.captured = true;
}

// ================================================================== head tracking (runtime/hle/game.c vr_head_apply)
// The game's update thread turns the first-person camera by the head's pose (xr_head, then xr_head_used with the
// camera it made); the render thread draws and shows that frame a little later, with the pose the latest update used
// (no frame-by-frame matching: the runtime's reprojection makes up the head's movement since). While the head was
// applied in the last few updates the image goes all around the player; otherwise - menus, the morph ball,
// cutscenes: no first-person camera - on the screen in front.
// The game's world is y-up, as OpenXR's; its camera's own axes are x, y up and z along the view (CTransform4f columns
// 0..2). Whether it looks down -z, as OpenXR (and OpenGL) do, or +z is read off the frames themselves: the view they
// were drawn with against the camera (xr_note_view); cam_sign 1 for -z, -1 for +z (then x is left).
static struct { XrPosef raw; float z[3]; uint64_t ns; bool cam; } last;  // (the latest update's pose: under hmx)
static XrPosef pending;  // (xr_head's pose, until xr_head_used: the update thread's own)
static _Atomic int cam_sign;  // (0: not known yet - taken as 1)
static _Atomic bool recenter_req;
static struct { bool seen; float tan[4]; } fv;  // (render thread: this frame's projection)

static XrQuaternionf qmul(XrQuaternionf a, XrQuaternionf b) {
    return (XrQuaternionf){a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                           a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
static XrQuaternionf qconj(XrQuaternionf q) { return (XrQuaternionf){-q.x, -q.y, -q.z, q.w}; }
static XrVector3f qrot(XrQuaternionf q, XrVector3f v) {
    XrQuaternionf r = qmul(qmul(q, (XrQuaternionf){v.x, v.y, v.z, 0}), qconj(q));
    return (XrVector3f){r.x, r.y, r.z};
}
void xr_recenter(void) { recenter_req = true; }
int xr_cam_sign(void) { return cam_sign ? cam_sign : 1; }
// Draw the next frame for both eyes (exec.c vk_set_stereo): VR on, the Video tab's "VR stereo", and the head applied
// lately (in play: not menus, the morph ball, cutscenes - on the screen, one image does)
// In play for a while: the head applied for a third of a second running (in and out of play - a pause, the morph
// ball - this turned stereo on and off a frame at a time, each time making the right eye's images anew). Worked out
// once a frame (xr_frame_end); stereo and the game image's shape (xr_shape) go by it.
static bool settled;
static void update_settled(void) {
    bool head = false;
    uint64_t now = SDL_GetTicksNS();
    if (X.on && X.running && !X.throttled && hmx) {
        SDL_LockMutex(hmx);
        head = last.cam && now - last.ns < 150000000ull;
        SDL_UnlockMutex(hmx);
    }
    static uint64_t since;
    if (!head) since = 0;
    else if (!since) since = now;
    settled = head && now - since > 300000000ull;
    // the game image in the eyes' view's shape while in play (game.c wide_frame: its projection; device.c: its
    // render targets) - all of it is seen, and the 2D on it (HUD, visor) is boxed into a 16:9 strip across it
    // (exec.c ui_boxed), so the HUD is in view; elsewhere the game's 16:9, on the screen
    float s = settled && X.view_aspect > 0.3f ? X.view_aspect / (16.0f / 9.0f) : 0;
    if (s != X.shape) LOG("[xr] game image %s\n", s ? "in the eyes' view's shape" : "16:9");
    X.shape = s;
}
float xr_shape(void) { return X.on ? X.shape : 0; }

bool xr_stereo_wanted(void) { return settled && g_settings.vr_stereo; }
float xr_vfov(void) { return X.on && X.running && X.vfov_deg > 0 ? X.vfov_deg : 0; }

bool xr_head(float rot[9], float pos[3], uint32_t *id) {
    if (!X.on || !X.running || X.throttled || !X.display_time || !hmx) return false;
    XrSpaceLocation loc = {.type = XR_TYPE_SPACE_LOCATION};
    // (where the head will be when this frame is shown: it's drawn after this update, shown a frame or so later)
    if (XR_FAILED(p_xrLocateSpace(X.view, X.space, X.display_time + X.period, &loc))) return false;
    if (!(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return false;
    if (!(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) loc.pose.position = (XrVector3f){0, 0, 0};
    static bool have_ref;
    static XrPosef ref;  // straight ahead and the head's place (Recenter): its yaw only, so up stays up
    if (recenter_req || !have_ref) {
        recenter_req = false, have_ref = true;
        XrVector3f f = qrot(loc.pose.orientation, (XrVector3f){0, 0, -1});
        float yaw = atan2f(-f.x, -f.z);
        ref.orientation = (XrQuaternionf){0, sinf(yaw / 2), 0, cosf(yaw / 2)};
        ref.position = loc.pose.position;
        LOG("[xr] recentered: ahead is %.0f degrees from the room's\n", yaw * 57.29578f);
    }
    XrQuaternionf iq = qconj(ref.orientation), q = qmul(iq, loc.pose.orientation);
    XrVector3f d = {loc.pose.position.x - ref.position.x, loc.pose.position.y - ref.position.y,
                    loc.pose.position.z - ref.position.z};
    // the head's rotation in the camera's own axes: OpenXR's as they are (cam_sign 1), or turned half round about up
    // (-1: x and z the other way) - column j = where the camera's axis j turns to
    float sg = (float)xr_cam_sign();
    for (int j = 0; j < 3; j++) {
        XrVector3f e = {j == 0 ? sg : 0, j == 1, j == 2 ? sg : 0}, c = qrot(q, e);
        rot[0 * 3 + j] = sg * c.x, rot[1 * 3 + j] = c.y, rot[2 * 3 + j] = sg * c.z;
    }
    // its movement in OpenXR's axes (x right, y up, z back), in game units (game.c lays it along the view's facing)
    float s = g_settings.vr_world_scale > 0.1f ? g_settings.vr_world_scale : 1.0f;
    XrVector3f dp = qrot(iq, d);
    pos[0] = dp.x * s, pos[1] = dp.y * s, pos[2] = dp.z * s;
    pending = loc.pose;
    *id = 1;
    return true;
}

// The camera the game made with xr_head's pose (CTransform4f: 3 rows of 4, the translation in column 3): the pose the
// next frames shown go with
void xr_head_used(uint32_t id, const float m[12]) {
    (void)id;
    if (!hmx) return;
    SDL_LockMutex(hmx);
    last.raw = pending;
    last.z[0] = m[2], last.z[1] = m[6], last.z[2] = m[10];
    last.ns = SDL_GetTicksNS();
    last.cam = true;
    SDL_UnlockMutex(hmx);
}

// Render thread: the camera of this frame's first world draw - view-projection and projection, column-major, OpenGL
// clip space (runtime/nvn/vulkan/exec.c): the frame's extent for the headset, and which way the game's camera looks
void xr_note_view(const float vp[16], const float p[16]) {
    if (!X.on || fv.seen || !hmx) return;
    fv.seen = true;
    // the frame's extent from its projection: x_ndc = p0 * (x / -z) - p8, the same in y
    fv.tan[0] = (p[8] - 1) / p[0], fv.tan[1] = (p[8] + 1) / p[0];
    fv.tan[2] = (p[9] + 1) / p[5], fv.tan[3] = (p[9] - 1) / p[5];
    if (cam_sign) return;  // (known: the rest is only to find it)
    float pi[16], v[16];
    if (!xr_mat_inv4(p, pi)) return;
    for (int c = 0; c < 4; c++)  // V = P^-1 VP: world -> view
        for (int r = 0; r < 4; r++)
            v[c * 4 + r] = pi[r] * vp[c * 4] + pi[4 + r] * vp[c * 4 + 1] + pi[8 + r] * vp[c * 4 + 2] + pi[12 + r] * vp[c * 4 + 3];
    float fwd[3], len = 0, dot = 0;
    for (int i = 0; i < 3; i++) fwd[i] = -v[i * 4 + 2], len += fwd[i] * fwd[i];  // (view -z, in the world)
    SDL_LockMutex(hmx);
    bool have = last.cam;
    for (int i = 0; have && i < 3; i++) dot += last.z[i] * fwd[i];
    SDL_UnlockMutex(hmx);
    len = sqrtf(len);
    if (!have || !(len > 1e-6f) || fabsf(dot / len) < 0.99f) return;  // (z along the view one way or the other)
    int sg = dot < 0 ? 1 : -1;  // (looking down -z: the view is against z)
    LOG("[xr] the game's camera looks down %sz\n", sg > 0 ? "-" : "+");
    cam_sign = sg;
}

void xr_frame_end(void) {
    update_settled();
    // the pose of the latest update that applied the head; the image all around while that was in the last 150 ms
    static float last_tan[4] = {-1, 1, 1, -1};
    if (fv.seen) memcpy(last_tan, fv.tan, sizeof last_tan);
    XrPosef pose = {.orientation = {0, 0, 0, 1}};
    bool head = false;
    if (hmx) {
        SDL_LockMutex(hmx);
        head = last.cam && SDL_GetTicksNS() - last.ns < 150000000ull;
        pose = last.raw;
        SDL_UnlockMutex(hmx);
    }
    float tan[4] = {last_tan[0], last_tan[1], last_tan[2], last_tan[3]};
    fv.seen = false;
    if (!X.in_frame) return;
    X.in_frame = false;
    uint64_t t0 = SDL_GetTicksNS();  // (the runtime's time, idle for the frame pacing: xr_capture)
    if (X.captured) {  // (the copy was submitted with the window's frame: vk_frame_end)
        XrSwapchainImageReleaseInfo ri = {.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        p_xrReleaseSwapchainImage(X.sc, &ri);
    }
    bool eyes = X.eyes_captured;  // (stereo: the left and right game images, xr_capture_eyes)
    for (int e = 0; eyes && e < 2; e++) {
        XrSwapchainImageReleaseInfo ri = {.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        p_xrReleaseSwapchainImage(X.eye[e].sc, &ri);
    }
    X.eyes_captured = false;
    // the screen: Video tab "VR screen size" wide (metres), "VR screen distance" ahead of where the player's head was
    // when the session began, at eye height
    float dist = g_settings.vr_screen_distance > 0.5f ? g_settings.vr_screen_distance : 2.5f;
    float width = g_settings.vr_screen_width > 0.5f ? g_settings.vr_screen_width : 3.0f;
    XrCompositionLayerQuad q = {.type = XR_TYPE_COMPOSITION_LAYER_QUAD, .space = X.space,
                                .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
                                .subImage = {.swapchain = X.sc, .imageRect = {{0, 0}, {(int32_t)X.sc_w, (int32_t)X.sc_h}}},
                                .pose = {.orientation = {0, 0, 0, 1}, .position = {0, 0, -dist}},
                                .size = {width, X.sc_w ? width * (float)X.sc_h / (float)X.sc_w : width * 9 / 16}};
    // In play with the head applied (and not the F1 menu over it): the image all around, as seen from the pose it was
    // drawn with, in the extent of its projection - the runtime turns it by the head's movement since. Stereo: each
    // eye its own image, from its own place (half the eyes' distance to the head's side); else both the window's.
    extern bool menu_input_captured(void);  // runtime/ui/menu.mm
    XrCompositionLayerProjectionView pv[2];
    for (int i = 0; i < 2; i++) {
        XrPosef ep = pose;
        if (eyes) {
            XrVector3f off = qrot(pose.orientation, (XrVector3f){(i ? 0.5f : -0.5f) * (X.ipd_m > 0 ? X.ipd_m : 0.063f), 0, 0});
            ep.position.x += off.x, ep.position.y += off.y, ep.position.z += off.z;
        }
        pv[i] = (XrCompositionLayerProjectionView){
            .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW, .pose = ep,
            .fov = {atanf(tan[0]), atanf(tan[1]), atanf(tan[2]), atanf(tan[3])},
            .subImage = eyes ? (XrSwapchainSubImage){.swapchain = X.eye[i].sc, .imageRect = {{0, 0}, {(int32_t)X.eye[i].w, (int32_t)X.eye[i].h}}}
                             : (XrSwapchainSubImage){.swapchain = X.sc, .imageRect = {{0, 0}, {(int32_t)X.sc_w, (int32_t)X.sc_h}}}};
    }
    XrCompositionLayerProjection pl = {.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION, .space = X.space, .viewCount = 2,
                                       .views = pv};
    bool around = head && !menu_input_captured();
    static int was = -1;
    int now = around ? (eyes ? 2 : 1) : 0;
    if (X.captured && now != was)
        LOG("[xr] %s\n", now == 2 ? "the game all around, in stereo" : now ? "the game all around (head tracked)" : "the game on the screen"),
            was = now;
    const XrCompositionLayerBaseHeader *layers[] = {around ? (const XrCompositionLayerBaseHeader *)&pl
                                                           : (const XrCompositionLayerBaseHeader *)&q};
    XrFrameEndInfo ei = {.type = XR_TYPE_FRAME_END_INFO, .displayTime = X.display_time, .environmentBlendMode = X.blend,
                         .layerCount = X.captured ? 1 : 0, .layers = layers};
    XrResult r = p_xrEndFrame(X.session, &ei);
    g_blocked_ns += SDL_GetTicksNS() - t0;
    if (XR_FAILED(r)) {
        static int n;
        if (n++ < 10) LOG("[xr] xrEndFrame: %s\n", res(r));
    }
    X.captured = false;
}
#endif

// A 4x4 inverse, by cofactors (column-major or not alike); false: singular. Here and exec.c (VR stereo)
bool xr_mat_inv4(const float m[16], float o[16]) {
    float a[16];
    a[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    a[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    a[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    a[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    a[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    a[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    a[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    a[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    a[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    a[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    a[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    a[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    a[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    a[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    a[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    a[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * a[0] + m[1] * a[4] + m[2] * a[8] + m[3] * a[12];
    if (!(fabsf(det) > 1e-20f)) return false;
    for (int i = 0; i < 16; i++) o[i] = a[i] / det;
    return true;
}
