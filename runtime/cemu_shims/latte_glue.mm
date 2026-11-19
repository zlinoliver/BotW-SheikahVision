// BotWRecomp: glue between this runtime and the vendored Cemu Latte GPU (Cemu environment).
// Implements runtime/src/latte/latte.h on top of Cemu's command processor:
//  - GX2 submissions (latte::submit) become ring words: INDIRECT_BUFFER + EVENT_WRITE_EOP(timestamp),
//    the same pair Cemu's TCL queues; Cemu's CP drains them through TCL::TCLGPUReadRBWord.
//  - retirement, flips and vsync are reported back through TCL/GX2 notification hooks.
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE  // iOS port (ios/): no AppKit; CoreGraphics/ImageIO are all this file needs
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#else
#import <AppKit/AppKit.h>
#endif
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
// VR: MetalFX spatial upscaling of the eye pictures (BOTW_VR_METALFX=1, botw_vr_publish). Not in the simulator SDK
#if !TARGET_OS_SIMULATOR && __has_include(<MetalFX/MetalFX.h>)
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#define BOTW_VR_HAVE_METALFX 1
#endif

#include <condition_variable>
#include <deque>

#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h"
#include "Cafe/HW/Latte/Renderer/Metal/LatteTextureMtl.h"
#include "Cafe/OS/libs/TCL/TCL.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/OS/libs/gx2/GX2_Event.h"
#include "WindowSystem.h"
#include "config/CemuConfig.h"
#include "../src/latte/latte.h"

void Latte_Start();
namespace mem { uint32_t sys_alloc(uint32_t size, uint32_t align); }

namespace {
std::mutex g_rb_lock;
std::deque<uint32> g_rb;
std::condition_variable g_rb_cv;  // the idle GPU thread waits here (TCLGPUWaitForWork)
bool g_rb_waiting = false;        // under g_rb_lock
std::atomic<uint64> g_last_submitted{0}, g_retired{0};
std::mutex g_retire_lock;
std::condition_variable g_retire_cv;
uint32 g_retire_slot = 0;
std::function<void(latte::Event)> g_sink;
std::mutex g_sync_lock;
std::condition_variable g_flip_cv, g_vsync_cv;
uint64 g_vsync_count = 0;
std::atomic<uint64> g_last_flip_time{0}, g_last_vsync_time{0};
std::atomic<uint64> g_frames{0};
int g_capture_every = 0;
std::atomic<uint64> g_capture_pending{0};
std::atomic<uint64> g_capture_eye_pending[3];
std::atomic<uint64> g_capture_window_pending;  // VR: the composed window  // VR: the eye images (0 left, 1 right) and the HUD layer (2)
std::string g_capture_dir;

void write_png(const std::vector<uint8>& rgb, int w, int h, const std::string& path) {
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CFDataRef data = CFDataCreate(nullptr, rgb.data(), rgb.size());
    CGDataProviderRef prov = CGDataProviderCreateWithCFData(data);
    CGImageRef img = CGImageCreate(w, h, 8, 24, w * 3, cs, kCGImageAlphaNone, prov, nullptr, false, kCGRenderingIntentDefault);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8*)path.c_str(), path.size(), false);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, (__bridge CFStringRef)UTTypePNG.identifier, 1, nullptr);
    if (dst) {
        CGImageDestinationAddImage(dst, img, nullptr);
        CGImageDestinationFinalize(dst);
        CFRelease(dst);
    }
    CFRelease(url);
    CGImageRelease(img);
    CGDataProviderRelease(prov);
    CFRelease(data);
    CGColorSpaceRelease(cs);
}
}  // namespace
namespace window { void on_frame(uint64_t frame); }
void log_write(const char* fmt, ...);

// the command thread's time waiting for work, for the [gpu] line (overlay_menu.cpp): how much of it the frame leaves idle
std::atomic<uint64> g_cp_idle_ns{0};
extern "C" double botw_cp_idle_take_ms(void) { return (double)g_cp_idle_ns.exchange(0) / 1e6; }
void TCL::TCLGPUWaitForWork(int max_us) {
    std::unique_lock<std::mutex> l(g_rb_lock);
    if (!g_rb.empty()) return;
    const auto start = std::chrono::steady_clock::now();
    g_rb_waiting = true;
    g_rb_cv.wait_for(l, std::chrono::microseconds(max_us), [] { return !g_rb.empty(); });
    g_rb_waiting = false;
    g_cp_idle_ns += (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
}
bool TCL::TCLGPUReadRBWord(uint32& w) {
    std::lock_guard<std::mutex> l(g_rb_lock);
    if (g_rb.empty()) return false;
    w = g_rb.front();
    g_rb.pop_front();
    return true;
}
void TCL::TCLGPUNotifyNewRetirementTimestamp() {
    uint64 ts = memory_readU64(g_retire_slot);
    {
        std::lock_guard<std::mutex> l(g_retire_lock);
        if (ts > g_retired) g_retired = ts;
    }
    g_retire_cv.notify_all();
}

namespace GX2 {
void __GX2NotifyNewRetirementTimestamp(uint64) {}
extern "C" int botw_vr_caption_jpeg(uint8* out, uint32 cap);  // below
void __GX2NotifyEvent(GX2CallbackEventType e) {
    if (e == GX2CallbackEventType::VSYNC) {
        g_last_vsync_time = coreinit::OSGetSystemTime();
        std::lock_guard<std::mutex> l(g_sync_lock);
        g_vsync_count++;
        g_vsync_cv.notify_all();
    } else if (e == GX2CallbackEventType::FLIP) {
        g_last_flip_time = coreinit::OSGetSystemTime();
        uint64 f = ++g_frames;
        window::on_frame(f);
        if (g_capture_every && f % g_capture_every == 0) g_capture_pending = g_capture_eye_pending[0] = g_capture_eye_pending[1] = g_capture_eye_pending[2] = g_capture_window_pending = f;
        if (f % 300 == 0) log_write("frame %llu", (unsigned long long)f);
        // BOTW_VR_TEST_CAPTION=<frame> (Mac tests): the save thumbnail as a save would get it, as <capture dir>/caption.jpg
        static const uint64 test_caption = [] { const char* e = getenv("BOTW_VR_TEST_CAPTION"); return e ? strtoull(e, nullptr, 10) : 0ull; }();
        if (test_caption && f == test_caption) {
            std::vector<uint8> jpeg(10240);
            if (botw_vr_caption_jpeg(jpeg.data(), (uint32)jpeg.size())) {
                FILE* o = fopen(fmt::format("{}/caption.jpg", g_capture_dir.empty() ? "." : g_capture_dir).c_str(), "wb");
                if (o) fwrite(jpeg.data(), 1, jpeg.size(), o), fclose(o);
            } else {
                log_write("vr: test caption: no thumbnail yet");
            }
        }
        std::lock_guard<std::mutex> l(g_sync_lock);
        g_flip_cv.notify_all();
    }
    if (g_sink) g_sink((latte::Event)(uint32)e);
}
}  // namespace GX2

static void save_view_png(LatteTextureView* view, bool isPadView, const std::string& path);

// --capture: read the TV image back synchronously (GPU thread) and save it as PNG.
void botw_capture_hook(LatteTextureView* view, bool isPadView) {
    uint64 f = g_capture_pending.exchange(0);
    if (!f || isPadView || !g_renderer) {
        if (f && isPadView) g_capture_pending = f;
        return;
    }
    save_view_png(view, isPadView, fmt::format("{}/frame_{:06}.png", g_capture_dir, f));
}

// --capture with VR: each eye's 3D image as frame_<n>_L.png / _R.png, the HUD layer as _H.png (LatteRenderTarget.cpp)
extern "C" void botw_capture_eye_hook(LatteTextureView* view, int eye) {
    if (eye < 0 || eye > 2) return;
    uint64 f = g_capture_eye_pending[eye].exchange(0);
    if (!f || !g_renderer) return;
    save_view_png(view, false, fmt::format("{}/frame_{:06}_{}.png", g_capture_dir, f, "LRH"[eye]));
}

static void save_texture_png(MTL::Texture* tex, bool isPadView, const std::string& path);
static void save_view_png(LatteTextureView* view, bool isPadView, const std::string& path) {
    save_texture_png(static_cast<LatteTextureMtl*>(view->baseTexture)->GetTexture(), isPadView, path);
}

// --capture with VR: the window as composed (both eyes and the HUD) as frame_<n>_S.png, after the HUD is drawn
// (LatteRenderTarget.cpp). The drawable must be readable: the first request makes the layer's next drawables so.
// ---- VR on Apple Vision Pro: the pictures for the immersive compositor (ios/src/vr_bridge.h) ---------------------
// Three sets of textures in rotation, written on the GPU thread in command-stream order: the eye images and the HUD as
// LatteRenderTarget.cpp sees them, or the game's picture in both eyes for a frame without eye images. When the frame's
// TV copy arrives the set is closed: its serial is signalled on a shared event after the GPU work so far, and the
// compositor (another thread, its own queue) reads the newest set the event has reached. Only active with BOTW_VR.
#include "../../ios/src/vr_bridge.h"
extern "C" void botw_vr_depth_range(float out[4]);  // runtime/src/game/vr_render.cpp
namespace {
struct VRSet {
    MTL::Texture* eye[2] = {};
    MTL::Texture* hud = nullptr;
    MTL::Texture* depth[2] = {};
    bool hud_valid = false, stereo = false, depth_valid = false;
    uint8 depth_mask = 0, eye_mask = 0;  // bit per eye whose depth / picture reached this set (since it was last closed)
    float depth_range[4] = {};
    uint32 pose_tag = 0;
    uint32 pose_tags[2] = {0, 0};  // each eye's own (vr_render.cpp botw_vr_latch_right_pose)
};
// Each eye's depth drives that eye's per-pixel reprojection in the compositor (and the system's, through the drawable's
// depth). A set used to count as having depth as soon as one eye's arrived, so when the other eye's depth copy was
// missing (dropped right after a command-buffer error, item 116) that eye was warped with the depth left in its texture
// three sets earlier: only that eye jumped as the head turned or moved (device 2026-11-19: the right eye shook, the left
// was steady). Now a set has depth only when both eyes' depth came in for it; otherwise both eyes use the single
// distance for that frame. The counts go to runtime.log every 900 sets and to botw_vr_set_stats.
struct VRSetStats { uint64 sets = 0, stereo = 0, mono_fallback = 0, depth_partial[2] = {0, 0}, eye_partial[2] = {0, 0}, pose_split = 0; };
VRSetStats g_vr_stats;  // GPU thread writes; botw_vr_set_stats reads (a torn read of a counter is harmless)
VRSet g_vr_sets[3];
MTL::SharedEvent* g_vr_event = nullptr;
uint64 g_vr_serial = 0;              // GPU thread: the set being written is g_vr_serial % 3, published as g_vr_serial + 1
std::atomic<uint64> g_vr_closed{0};  // the last serial closed (its signal is encoded)
std::mutex g_vr_lock;
std::atomic<bool> g_vr_reader{false};  // a compositor asked for frames: only then are they made (not on a Mac)

// A replaced texture is released only some sets later: the compositor holds the set's textures unretained while it
// draws (BotWImmersive.swift takeUnretainedValue), and the first stereo sets with MetalFX on always replace them
// (review 2026-11-04). GPU thread only.
struct VRRetired { MTL::Texture* t; uint64 serial; };
std::vector<VRRetired> g_vr_retired;
void vr_retire(MTL::Texture* t) {
    if (t) g_vr_retired.push_back({t, g_vr_serial});
    for (size_t i = 0; i < g_vr_retired.size();)
        if (g_vr_serial >= g_vr_retired[i].serial + 6) g_vr_retired[i].t->release(), g_vr_retired.erase(g_vr_retired.begin() + i);
        else i++;
}

MTL::Texture* vr_texture(MTL::Texture*& t, uint32 w, uint32 h) {
    if (t && t->width() == w && t->height() == h) return t;
    vr_retire(t);  // a size change
    auto* desc = MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatBGRA8Unorm, w, h, false);
    desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead | MTL::TextureUsagePixelFormatView);
    desc->setStorageMode(MTL::StorageModePrivate);
    t = ((MetalRenderer*)g_renderer.get())->GetDevice()->newTexture(desc);
    return t;
}
VRSet& vr_writing() { return g_vr_sets[g_vr_serial % 3]; }
void vr_draw(LatteTextureView* view, MTL::Texture*& slot, float sharpen = 0) {
    auto* mr = (MetalRenderer*)g_renderer.get();
    MTL::Texture* src = static_cast<LatteTextureMtl*>(view->baseTexture)->GetTexture();
    std::lock_guard<std::mutex> l(g_vr_lock);
    mr->DrawTextureToTexture(view, vr_texture(slot, (uint32)src->width(), (uint32)src->height()), sharpen);
}
// The eye pictures are sharpened as they are copied, at the game's resolution (BOTW_VR_CAS, 0.5, 0 off; Fable 5.1's
// option 5): the compositor then magnifies a crisper picture 1.3-2.3x onto the display. Sharpening there instead, at
// the display's resolution 90 times a second, cost the headset its 90 Hz (2026-11-01); here it is two game-size
// passes 45 times a second. The HUD and the screen pictures (text, menus) are left as they are
float eye_sharpen() {
    static const float v = [] {
        const char* e = getenv("BOTW_VR_CAS");
        return e ? (float)atof(e) : 0.25f;  // 0.5 until 2026-11-03: it made the edges grainy at this size
    }();
    return v;
}
}  // namespace

// ---- VR: MetalFX spatial upscaling of the eye pictures (BOTW_VR_METALFX=1, launcher "MetalFX upscaling", off by
// default). Each eye's picture is copied (sRGB-encoded, sharpened as above) into a texture of its own at the game's
// resolution, then MTLFXSpatialScaler writes it into the set's eye texture at the size an equivalent render scale
// BOTW_VR_FX_SCALE (2.0 by default) would have given, on Latte's command buffer before the set's signal, so the
// compositor's wait covers it. Flat pictures (menus, loading), the HUD and the depth are left as they are; the save
// thumbnail reads the game-size copy. Off, or where MetalFX is missing: the pictures are made exactly as before
extern float g_render_scale;  // runtime: --scale / BOTW_SCALE
namespace {
bool vr_metalfx_wanted() {
    static const bool v = [] {
        const char* e = getenv("BOTW_VR_METALFX");
        return e && !strcmp(e, "1");
    }();
    return v;
}
#if BOTW_VR_HAVE_METALFX
static_assert(!__has_feature(objc_arc), "the MetalFX objects below are retained and released by hand");
struct VRFx {
    id<MTLFXSpatialScaler> scaler = nil;  // one for both eyes (+1, released on a size change; this file is not ARC)
    uint32 in_w = 0, in_h = 0, out_w = 0, out_h = 0;
    bool failed = false;  // logged once; today's path from then on
    MTL::Texture* lo[2] = {};  // the eyes' game-size pictures (not rotated: written and read in command-stream order)
};
VRFx g_vr_fx;

void vr_fx_fail(const char* why) {
    g_vr_fx.failed = true;
    log_write("vr metalfx: unavailable (%s)", why);
}
// a private BGRA8 texture with at least these usages (vr_draw keeps a texture of the right size as it is)
MTL::Texture* vr_fx_texture(MTL::Texture*& t, uint32 w, uint32 h, MTL::TextureUsage usage) {
    usage |= MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead | MTL::TextureUsagePixelFormatView;
    if (t && t->width() == w && t->height() == h && (t->usage() & usage) == usage) return t;
    vr_retire(t);
    auto* desc = MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatBGRA8Unorm, w, h, false);
    desc->setUsage(usage);
    desc->setStorageMode(MTL::StorageModePrivate);
    t = ((MetalRenderer*)g_renderer.get())->GetDevice()->newTexture(desc);
    return t;
}
// once after the scaler is made: its GPU time on textures of its own, on a queue of its own
void vr_fx_bench(MTL::Device* device) {
    id<MTLDevice> dev = (__bridge id<MTLDevice>)device;
    MTL::Texture *in = nullptr, *out = nullptr;
    vr_fx_texture(in, g_vr_fx.in_w, g_vr_fx.in_h, (MTL::TextureUsage)g_vr_fx.scaler.colorTextureUsage);
    vr_fx_texture(out, g_vr_fx.out_w, g_vr_fx.out_h, (MTL::TextureUsage)g_vr_fx.scaler.outputTextureUsage);
    @autoreleasepool {  // the GPU thread has no pool of its own
    id<MTLCommandQueue> queue = [dev newCommandQueue];
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    g_vr_fx.scaler.colorTexture = (__bridge id<MTLTexture>)in;
    g_vr_fx.scaler.outputTexture = (__bridge id<MTLTexture>)out;
    g_vr_fx.scaler.inputContentWidth = g_vr_fx.in_w;
    g_vr_fx.scaler.inputContentHeight = g_vr_fx.in_h;
    for (int i = 0; i < 10; i++) [g_vr_fx.scaler encodeToCommandBuffer:cb];
    [cb commit];
    [cb waitUntilCompleted];
    const double ms = (cb.GPUEndTime - cb.GPUStartTime) * 1000.0 / 10.0;
    if (cb.status == MTLCommandBufferStatusCompleted)
        log_write("vr metalfx: ~%.2f ms per eye (10 runs, %ux%u -> %ux%u)", ms, g_vr_fx.in_w, g_vr_fx.in_h,
                  g_vr_fx.out_w, g_vr_fx.out_h);
    else
        log_write("vr metalfx: benchmark failed (command buffer status %d)", (int)cb.status);
    g_vr_fx.scaler.colorTexture = nil;
    g_vr_fx.scaler.outputTexture = nil;
    [queue release];
    }
    in->release();
    out->release();
}
// an eye's picture through MetalFX into set.eye[e]; false: not done (the caller makes it as before)
bool vr_fx_eye(LatteTextureView* view, int e, MTL::Texture*& slot) {
    if (!vr_metalfx_wanted() || g_vr_fx.failed) return false;
    auto* mr = (MetalRenderer*)g_renderer.get();
    MTL::Texture* src = static_cast<LatteTextureMtl*>(view->baseTexture)->GetTexture();
    const uint32 iw = (uint32)src->width(), ih = (uint32)src->height();
    if (g_vr_fx.scaler && (iw != g_vr_fx.in_w || ih != g_vr_fx.in_h)) {
        [g_vr_fx.scaler release];
        g_vr_fx.scaler = nil;
    }
    if (!g_vr_fx.scaler) {
        static const float fx = [] {
            const char* v = getenv("BOTW_VR_FX_SCALE");
            return v ? (float)atof(v) : 2.0f;
        }();
        const float scale = g_render_scale > 0.01f ? g_render_scale : 1.0f;
        const float ratio = std::clamp(fx / scale, 1.0f, 2.0f);
        if (ratio <= 1.001f) {
            char why[96];
            snprintf(why, sizeof why, "render scale %.2f is already at or above the fx scale %.2f", scale, fx);
            vr_fx_fail(why);
            return false;
        }
        const uint32 ow = std::clamp<uint32>((uint32)lroundf(iw * ratio), iw, iw * 2);
        const uint32 oh = std::clamp<uint32>((uint32)lroundf(ih * ratio), ih, ih * 2);
        id<MTLDevice> dev = (__bridge id<MTLDevice>)mr->GetDevice();
        if (![MTLFXSpatialScalerDescriptor supportsDevice:dev]) {
            vr_fx_fail("the spatial scaler does not support this device");
            return false;
        }
        MTLFXSpatialScalerDescriptor* desc = [MTLFXSpatialScalerDescriptor new];
        desc.colorTextureFormat = MTLPixelFormatBGRA8Unorm;  // sRGB-encoded values (DrawTextureToTexture's sRGB view)
        desc.outputTextureFormat = MTLPixelFormatBGRA8Unorm;
        desc.inputWidth = iw;
        desc.inputHeight = ih;
        desc.outputWidth = ow;
        desc.outputHeight = oh;
        desc.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
        g_vr_fx.scaler = [desc newSpatialScalerWithDevice:dev];
        [desc release];
        if (!g_vr_fx.scaler) {
            vr_fx_fail("newSpatialScalerWithDevice returned nil");
            return false;
        }
        const bool first = !g_vr_fx.in_w;
        g_vr_fx.in_w = iw, g_vr_fx.in_h = ih, g_vr_fx.out_w = ow, g_vr_fx.out_h = oh;
        log_write("vr metalfx: spatial %ux%u -> %ux%u (fx scale %.2f, ratio %.3f), bgra8 perceptual", iw, ih, ow, oh, fx,
                  ratio);
        if (first) vr_fx_bench(mr->GetDevice());
    }
    // the game-size picture, as today's path makes it (sRGB-encoded, sharpened)
    {
        std::lock_guard<std::mutex> l(g_vr_lock);
        vr_fx_texture(g_vr_fx.lo[e], iw, ih, (MTL::TextureUsage)g_vr_fx.scaler.colorTextureUsage);
    }
    vr_draw(view, g_vr_fx.lo[e], eye_sharpen());
    std::lock_guard<std::mutex> l(g_vr_lock);
    MTL::Texture* out = vr_fx_texture(slot, g_vr_fx.out_w, g_vr_fx.out_h,
                                      (MTL::TextureUsage)g_vr_fx.scaler.outputTextureUsage);
    mr->EndEncoding();  // MetalFX encodes on the command buffer itself
    id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)mr->GetCommandBuffer();
    @autoreleasepool {
        g_vr_fx.scaler.colorTexture = (__bridge id<MTLTexture>)g_vr_fx.lo[e];
        g_vr_fx.scaler.outputTexture = (__bridge id<MTLTexture>)out;
        g_vr_fx.scaler.inputContentWidth = iw;
        g_vr_fx.scaler.inputContentHeight = ih;
        [g_vr_fx.scaler encodeToCommandBuffer:cb];
    }
    return true;
}
MTL::Texture* vr_fx_thumbnail_source(MTL::Texture* eye) {
    return vr_metalfx_wanted() && !g_vr_fx.failed && g_vr_fx.lo[0] ? g_vr_fx.lo[0] : eye;
}
#else
bool vr_fx_eye(LatteTextureView*, int, MTL::Texture*&) {
    static bool logged = false;
    if (vr_metalfx_wanted() && !logged) logged = true, log_write("vr metalfx: unavailable (not in this build)");
    return false;
}
MTL::Texture* vr_fx_thumbnail_source(MTL::Texture* eye) { return eye; }
#endif
}  // namespace

extern "C" void* botw_vr_shared_event(void) { return g_vr_event; }

extern "C" int botw_vr_latest_frame(BotWVRFrame* out) {
    g_vr_reader = true;
    if (!g_vr_event) return 0;
    uint64 done = g_vr_event->signaledValue();
    if (!done) return 0;
    std::lock_guard<std::mutex> l(g_vr_lock);
    VRSet& set = g_vr_sets[(done - 1) % 3];
    if (!set.eye[0]) return 0;
    out->eye[0] = set.eye[0];
    out->eye[1] = set.stereo && set.eye[1] ? set.eye[1] : set.eye[0];
    out->hud = set.stereo && set.hud_valid ? set.hud : nullptr;
    out->stereo = set.stereo;
    out->pose_tag = set.pose_tag;
    out->pose_tag_eye[0] = set.pose_tags[0];
    out->pose_tag_eye[1] = set.stereo ? set.pose_tags[1] : set.pose_tags[0];
    out->serial = done;
    const bool depth = set.stereo && set.depth_valid && set.depth[0] && set.depth[1];
    out->depth[0] = depth ? set.depth[0] : nullptr;
    out->depth[1] = depth ? set.depth[1] : nullptr;
    for (int i = 0; i < 4; i++) out->depth_range[i] = set.depth_range[i];
    return 1;
}

// The save's thumbnail (caption.jpg): in VR the game's own capture comes out black (the 3D picture is drawn into the
// eyes, not where the game takes it from; the saves made with BetterVR on a PC are black too). Every 2 s of the
// headset's view the left eye's picture is copied into a CPU-readable buffer, in the command stream as it goes (no
// waiting); a save then gets a JPEG of it (coreinit/fs.cpp, botw_vr_caption_jpeg)
extern "C" int botw_vr_view_now(void);  // vr_render.cpp
static std::mutex g_thumb_lock;
static MTL::Buffer* g_thumb_buf = nullptr;
static uint32 g_thumb_w = 0, g_thumb_h = 0;
static double g_thumb_at = 0;  // when the copy was put in the stream (seconds), 0 none
static double now_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
extern "C" int botw_vr_pause_menu(void);  // vr_render.cpp
static void keep_thumbnail(MTL::Texture* eye) {
    // not while the pause menu is open: the world behind it is dimmed and blurred, and a save made from the menu got
    // that picture (2026-11-17); the copy from before the menu opened is kept instead
    if (!eye || !botw_vr_view_now() || botw_vr_pause_menu()) return;
    const double t = now_seconds();
    std::lock_guard<std::mutex> l(g_thumb_lock);
    if (t - g_thumb_at < 2.0) return;
    auto* mr = (MetalRenderer*)g_renderer.get();
    const uint32 w = (uint32)eye->width(), h = (uint32)eye->height();
    if (!g_thumb_buf || g_thumb_w != w || g_thumb_h != h) {
        if (g_thumb_buf) g_thumb_buf->release();
        g_thumb_buf = mr->GetDevice()->newBuffer(w * h * 4, MTL::ResourceStorageModeShared);
        g_thumb_w = w, g_thumb_h = h;
    }
    auto* enc = mr->GetBlitCommandEncoder();
    enc->copyFromTexture(eye, 0, 0, MTL::Origin(0, 0, 0), MTL::Size(w, h, 1), g_thumb_buf, 0, w * 4, 0);
    g_thumb_at = t;
}
// coreinit/fs.cpp: the thumbnail as a JPEG of at most cap bytes (256x144, the game's), the rest zeros; 0 without one
extern "C" int botw_vr_caption_jpeg(uint8* out, uint32 cap) {
    std::lock_guard<std::mutex> l(g_thumb_lock);
    if (!g_thumb_buf || now_seconds() - g_thumb_at < 0.3) return 0;  // the copy is done well within that
    const uint32 W = 256, H = 144, sw = g_thumb_w, sh = g_thumb_h;
    // the middle 16:9 of the eye picture (taller, 1.25:1), box-averaged down to 256x144
    const uint32 cw = sw, ch = std::min(sh, sw * 9 / 16), y0 = (sh - ch) / 2;
    const uint8* src = (const uint8*)g_thumb_buf->contents();  // BGRA8, already sRGB-encoded (vr_draw)
    std::vector<uint8> rgba(W * H * 4);
    for (uint32 y = 0; y < H; y++)
        for (uint32 x = 0; x < W; x++) {
            const uint32 xa = x * cw / W, xb = std::max(xa + 1, (x + 1) * cw / W);
            const uint32 ya = y0 + y * ch / H, yb = std::max(ya + 1, y0 + (y + 1) * ch / H);
            uint32 b = 0, g = 0, r = 0, n = 0;
            for (uint32 yy = ya; yy < yb; yy += 2)
                for (uint32 xx = xa; xx < xb; xx += 2) {
                    const uint8* p = src + ((size_t)yy * sw + xx) * 4;
                    b += p[0], g += p[1], r += p[2], n++;
                }
            uint8* o = &rgba[(y * W + x) * 4];
            o[0] = r / n, o[1] = g / n, o[2] = b / n, o[3] = 255;
        }
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(rgba.data(), W, H, 8, W * 4, cs, kCGImageAlphaNoneSkipLast);
    CGImageRef img = ctx ? CGBitmapContextCreateImage(ctx) : nullptr;
    int ok = 0;
    for (float q = 0.85f; img && !ok && q > 0.2f; q -= 0.15f) {
        CFMutableDataRef data = CFDataCreateMutable(nullptr, 0);
        CGImageDestinationRef dst = CGImageDestinationCreateWithData(data, (CFStringRef)UTTypeJPEG.identifier, 1, nullptr);
        NSDictionary* props = @{(id)kCGImageDestinationLossyCompressionQuality: @(q)};
        CGImageDestinationAddImage(dst, img, (CFDictionaryRef)props);
        if (CGImageDestinationFinalize(dst) && (uint32)CFDataGetLength(data) <= cap) {
            const uint32 n = (uint32)CFDataGetLength(data);
            memcpy(out, CFDataGetBytePtr(data), n);
            memset(out + n, 0, cap - n);
            log_write("vr: save thumbnail from the left eye's picture (%u bytes, quality %.2f)", n, q);
            ok = 1;
        }
        CFRelease(dst);
        CFRelease(data);
    }
    if (img) CGImageRelease(img);
    if (ctx) CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    return ok;
}

// LatteRenderTarget.cpp: an eye's image (0 / 1, with the pose tag it was rendered with), the HUD layer (2)
extern "C" void botw_vr_publish(LatteTextureView* view, int kind, uint32 pose_tag) {
    static const bool probe = getenv("BOTW_VR_DEPTH_PROBE") != nullptr;  // a Mac test: the sets made without a reader
    if (probe) g_vr_reader = true;
    if (!g_vr_reader || !g_renderer) return;
    VRSet& set = vr_writing();
    if (kind == 2) {
        vr_draw(view, set.hud);
        set.hud_valid = true;
    } else if (kind >= 3) {  // an eye's depth
        auto* mr = (MetalRenderer*)g_renderer.get();
        MTL::Texture* src = static_cast<LatteTextureMtl*>(view->baseTexture)->GetTexture();
        MTL::Texture*& t = set.depth[kind - 3];
        if (!t || t->width() != src->width() || t->height() != src->height()) {
            if (t) t->release();
            auto* desc = MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatR32Float, src->width(), src->height(), false);
            desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
            desc->setStorageMode(MTL::StorageModePrivate);
            t = mr->GetDevice()->newTexture(desc);
        }
        std::lock_guard<std::mutex> l(g_vr_lock);
        mr->DrawDepthToTexture(view, t);
        set.depth_mask |= 1 << (kind - 3);
    } else {
        if (!vr_fx_eye(view, kind & 1, set.eye[kind & 1])) vr_draw(view, set.eye[kind & 1], eye_sharpen());
        set.eye_mask |= 1 << (kind & 1);
        set.pose_tag = pose_tag;
        set.pose_tags[kind & 1] = pose_tag;
        if (kind == 0) keep_thumbnail(vr_fx_thumbnail_source(set.eye[0]));
    }
}

// LatteRenderTarget.cpp: the frame's TV copy closes the set; without both eye images the TV picture is the set
extern "C" void botw_vr_close_frame(LatteTextureView* tv, bool eyes_shown) {
    if (!g_vr_reader || !g_renderer) return;
    auto* mr = (MetalRenderer*)g_renderer.get();
    if (!g_vr_event) g_vr_event = mr->GetDevice()->newSharedEvent();
    VRSet& set = vr_writing();
    set.stereo = eyes_shown;
    set.depth_valid = eyes_shown && set.depth_mask == 3;  // both eyes' depth from this frame, or none (above)
    g_vr_stats.sets++;
    if (eyes_shown) {
        g_vr_stats.stereo++;
        if (set.pose_tags[0] != set.pose_tags[1]) g_vr_stats.pose_split++;  // the eyes drawn from different poses
        for (int e = 0; e < 2; e++)
            if (set.depth_mask != 0 && !(set.depth_mask & (1 << e))) g_vr_stats.depth_partial[e]++;
    } else if (set.eye_mask == 1 || set.eye_mask == 2) {  // one eye's picture came, the other's did not: the TV picture instead
        g_vr_stats.mono_fallback++;
        for (int e = 0; e < 2; e++)
            if (!(set.eye_mask & (1 << e))) g_vr_stats.eye_partial[e]++;
    }
    if (g_vr_stats.sets % 900 == 0)
        cemuLog_log(LogType::Force, "vr sets: {} ({} stereo, {} with the eyes on different poses), missing depth L {} R {}, missing picture L {} R {} (mono fallback {})",
            g_vr_stats.sets, g_vr_stats.stereo, g_vr_stats.pose_split, g_vr_stats.depth_partial[0], g_vr_stats.depth_partial[1],
            g_vr_stats.eye_partial[0], g_vr_stats.eye_partial[1], g_vr_stats.mono_fallback);
    botw_vr_depth_range(set.depth_range);
    if (!eyes_shown) {
        vr_draw(tv, set.eye[0]);
        set.hud_valid = false;
    }
    // BOTW_VR_DEPTH_PROBE=1 (tests, a Mac): every 120 sets the left eye's depth down its middle column, 9 samples
    static const bool probe = getenv("BOTW_VR_DEPTH_PROBE") != nullptr;
    // and once, the left eye's picture as the compositor gets it (after the sRGB encoding and the sharpening)
    if (probe && set.stereo && set.eye[0] && g_vr_serial == 900 && !g_capture_dir.empty())
        save_texture_png(set.eye[0], true, fmt::format("{}/vr_set_eye_L.png", g_capture_dir));
    if (probe && set.stereo && set.depth_valid && set.depth[0] && g_vr_serial % 120 == 0) {
        MTL::Texture* t = set.depth[0];
        MTL::Buffer* buf = mr->GetDevice()->newBuffer(9 * 4, MTL::ResourceStorageModeShared);
        auto* enc = mr->GetBlitCommandEncoder();
        for (int i = 0; i < 9; i++)
            enc->copyFromTexture(t, 0, 0, MTL::Origin(t->width() / 2, t->height() * (i + 1) / 10, 0), MTL::Size(1, 1, 1),
                                 buf, i * 4, 4, 4);
        mr->Flush(true);
        const float* v = (const float*)buf->contents();
        cemuLog_log(LogType::Force, "vr depth probe: near {:.3f} far {:.1f} zscale {:.3f} zoffset {:.3f}, column {:.5f} {:.5f} {:.5f} {:.5f} {:.5f} {:.5f} {:.5f} {:.5f} {:.5f}",
            set.depth_range[0], set.depth_range[1], set.depth_range[2], set.depth_range[3], v[0], v[1], v[2], v[3], v[4],
            v[5], v[6], v[7], v[8]);
        buf->release();
    }
    mr->SignalSharedEvent(g_vr_event, ++g_vr_serial);
    g_vr_closed = g_vr_serial;
    VRSet& next = vr_writing();
    next.hud_valid = false;
    next.depth_valid = false;
    next.depth_mask = 0;
    next.eye_mask = 0;
}

// The counts above since the game started (for diagnostics): sets closed, stereo sets, sets
// with one eye's depth missing (left, right), frames that fell back to the TV picture with one eye's picture missing,
// stereo sets whose eyes were drawn from different poses
extern "C" void botw_vr_set_stats(uint64_t out[8]) {
    out[0] = g_vr_stats.sets; out[1] = g_vr_stats.stereo;
    out[2] = g_vr_stats.depth_partial[0]; out[3] = g_vr_stats.depth_partial[1];
    out[4] = g_vr_stats.eye_partial[0]; out[5] = g_vr_stats.eye_partial[1];
    out[6] = g_vr_stats.mono_fallback;
    out[7] = g_vr_stats.pose_split;
}

extern "C" void botw_capture_window_hook() {
    uint64 f = g_capture_window_pending.load();
    if (!f || !g_renderer) return;
    auto* mr = (MetalRenderer*)g_renderer.get();
    MTL::Texture* tex = mr->GetMainDrawableTexture();
    if (!tex) return;
    if (tex->framebufferOnly()) {
        mr->SetMainLayerReadable();
        return;  // the next capture can read it
    }
    g_capture_window_pending = 0;
    save_texture_png(tex, true, fmt::format("{}/frame_{:06}_S.png", g_capture_dir, f));
}

static void save_texture_png(MTL::Texture* tex, bool isPadView, const std::string& path) {
    auto* mr = (MetalRenderer*)g_renderer.get();
    uint32 w = (uint32)tex->width(), h = (uint32)tex->height();
    MTL::PixelFormat fmt = tex->pixelFormat();
    uint32 bpp = (fmt == MTL::PixelFormatRGBA16Float || fmt == MTL::PixelFormatRGBA16Unorm) ? 8 : 4;
    MTL::Buffer* buf = mr->GetDevice()->newBuffer(w * h * bpp, MTL::ResourceStorageModeShared);
    auto* enc = mr->GetBlitCommandEncoder();
    enc->copyFromTexture(tex, 0, 0, MTL::Origin(0, 0, 0), MTL::Size(w, h, 1), buf, 0, w * bpp, 0);
    mr->Flush(true);
    std::vector<uint8> rgb(w * h * 3);
    const uint8* src = (const uint8*)buf->contents();
    // the screen output gamma-encodes when the scan-out buffer is sRGB; do the same so captures match
    const bool encode = !isPadView && LatteGPUState.tvBufferUsesSRGB && fmt != MTL::PixelFormatBGRA8Unorm_sRGB;
    static uint8 s_srgb[1024];
    static bool s_srgb_init = [] {
        for (int k = 0; k < 1024; k++) {
            float v = k / 1023.0f;
            v = v <= 0.0031308f ? 12.92f * v : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
            s_srgb[k] = (uint8)std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f);
        }
        return true;
    }();
    (void)s_srgb_init;
    for (uint32 i = 0; i < w * h; i++) {
        uint8 r, g, b;
        if (fmt == MTL::PixelFormatBGRA8Unorm || fmt == MTL::PixelFormatBGRA8Unorm_sRGB) {
            b = src[i * 4]; g = src[i * 4 + 1]; r = src[i * 4 + 2];
        } else if (fmt == MTL::PixelFormatRGB10A2Unorm) {
            uint32 v = ((const uint32*)src)[i];
            r = (uint8)(((v >> 0) & 1023) >> 2); g = (uint8)(((v >> 10) & 1023) >> 2); b = (uint8)(((v >> 20) & 1023) >> 2);
        } else if (fmt == MTL::PixelFormatRGBA16Unorm) {  // the VR frame buffer with the HUD (graphic_pack_shim.cpp)
            const uint16* up = (const uint16*)(src + i * 8);
            r = (uint8)(up[0] >> 8); g = (uint8)(up[1] >> 8); b = (uint8)(up[2] >> 8);
        } else if (bpp == 8) {
            const __fp16* hp = (const __fp16*)(src + i * 8);
            auto c = [](float x) { return (uint8)std::clamp(x * 255.0f + 0.5f, 0.0f, 255.0f); };
            r = c(hp[0]); g = c(hp[1]); b = c(hp[2]);
        } else {
            r = src[i * 4]; g = src[i * 4 + 1]; b = src[i * 4 + 2];
        }
        if (encode) {
            if (fmt == MTL::PixelFormatRGB10A2Unorm) {  // keep 10-bit precision
                uint32 v = ((const uint32*)src)[i];
                r = s_srgb[v & 1023]; g = s_srgb[(v >> 10) & 1023]; b = s_srgb[(v >> 20) & 1023];
            } else {
                r = s_srgb[r * 1023 / 255]; g = s_srgb[g * 1023 / 255]; b = s_srgb[b * 1023 / 255];
            }
        }
        rgb[i * 3] = r; rgb[i * 3 + 1] = g; rgb[i * 3 + 2] = b;
    }
    // the HUD layer (_H): its alpha as a grey picture (_A) and, in the log, how many semi-transparent pixels have a colour
    // above their alpha, which a premultiplied layer cannot have (the UI's blending over a transparent clear may square
    // alpha: grey haze under translucent HUD backdrops, device 2026-11-12)
    if (path.size() > 6 && path.compare(path.size() - 6, 6, "_H.png") == 0) {
        std::vector<uint8> alpha(w * h * 3);
        uint64 semi = 0, above = 0, above_sqrt = 0;
        for (uint32 i = 0; i < w * h; i++) {
            float r, g, b, a;
            if (fmt == MTL::PixelFormatRGBA16Unorm) {
                const uint16* up = (const uint16*)(src + i * 8);
                r = up[0] / 65535.0f, g = up[1] / 65535.0f, b = up[2] / 65535.0f, a = up[3] / 65535.0f;
            } else if (bpp == 8) {
                const __fp16* hp = (const __fp16*)(src + i * 8);
                r = hp[0], g = hp[1], b = hp[2], a = hp[3];
            } else if (fmt == MTL::PixelFormatRGB10A2Unorm) {  // 2-bit alpha: 0, 1/3, 2/3, 1
                const uint32 v = ((const uint32*)src)[i];
                r = (v & 1023) / 1023.0f, g = ((v >> 10) & 1023) / 1023.0f, b = ((v >> 20) & 1023) / 1023.0f, a = (v >> 30) / 3.0f;
            } else if (fmt == MTL::PixelFormatBGRA8Unorm || fmt == MTL::PixelFormatBGRA8Unorm_sRGB) {
                b = src[i * 4] / 255.0f, g = src[i * 4 + 1] / 255.0f, r = src[i * 4 + 2] / 255.0f, a = src[i * 4 + 3] / 255.0f;
            } else {
                r = src[i * 4] / 255.0f, g = src[i * 4 + 1] / 255.0f, b = src[i * 4 + 2] / 255.0f, a = src[i * 4 + 3] / 255.0f;
            }
            const uint8 a8 = (uint8)std::clamp(a * 255.0f + 0.5f, 0.0f, 255.0f);
            alpha[i * 3] = alpha[i * 3 + 1] = alpha[i * 3 + 2] = a8;
            if (a > 0.04f && a < 0.96f) {
                semi++;
                if (std::max(r, std::max(g, b)) > a + 0.05f) above++;
                if (std::max(r, std::max(g, b)) > sqrtf(a) + 0.05f) above_sqrt++;
            }
        }
        write_png(alpha, (int)w, (int)h, path.substr(0, path.size() - 6) + "_A.png");
        log_write("capture: HUD alpha check: %llu semi-transparent pixels, %llu with colour above alpha, %llu above its square root (format %u)",
                  semi, above, above_sqrt, (uint32)fmt);
    }
    buf->release();
    write_png(rgb, (int)w, (int)h, path);
    log_write("capture %s (%ux%u, format %u)", path.c_str(), w, h, (uint32)fmt);
}

namespace latte {
// Main thread, before the game starts: Cemu's renderer adds a Metal child view to `nsview`.
void create_renderer(void* nsview, int w, int h) {
    WindowSystem::GetWindowInfo().window_main.surface = nsview;
    WindowSystem::GetWindowInfo().window_main.backend = WindowSystem::WindowHandleInfo::Backend::Cocoa;
    g_renderer = std::make_unique<MetalRenderer>();
    ((MetalRenderer*)g_renderer.get())->InitializeLayer({w, h}, true);
}

void resize_window(int w, int h) {
    if (g_renderer) ((MetalRenderer*)g_renderer.get())->ResizeLayer({w, h}, true);
}

void set_display_mode(int mode) { GetConfig().fullscreen_scaling = mode; }
void set_output_filter(int f) {  // upscaling uses the choice; downscaling stays bilinear unless nearest
    GetConfig().upscale_filter = f;
    GetConfig().downscale_filter = f == kNearestNeighborFilter ? kNearestNeighborFilter : kLinearFilter;
}

void set_scanout_srgb(bool tv, bool srgb) {
    if (tv) LatteGPUState.tvBufferUsesSRGB = srgb;
    else LatteGPUState.drcBufferUsesSRGB = srgb;
}

void set_capture(int every, const char* dir) {
    g_capture_every = every;
    g_capture_dir = dir;
    fs::create_directories(dir);
}

void init() {
    static bool done = false;
    if (done) return;
    done = true;
    LatteGPUState.sharedAreaAddr = mem::sys_alloc(sizeof(gx2GPUSharedArea_t), 0x20);
    LatteGPUState.sharedArea = (gx2GPUSharedArea_t*)memory_getPointerFromVirtualOffset(LatteGPUState.sharedAreaAddr);
    LatteGPUState.sharedArea->flipRequestCountBE = 0;
    LatteGPUState.sharedArea->flipExecuteCountBE = 0;
    LatteGPUState.sharedArea->swapInterval = 1;
    g_retire_slot = mem::sys_alloc(16, 16);
    Latte_Start();  // returns once the GPU thread finished its init
    LatteGPUState.gx2InitCalled++;
}

uint64_t submit(uint32_t addr, uint32_t ndw) {
    uint64 ts = ++g_last_submitted;
    uint32 words[] = {pm4HeaderType3(IT_INDIRECT_BUFFER_PRIV, 3), addr, 0, ndw,
                      pm4HeaderType3(IT_EVENT_WRITE_EOP, 5), 0x504, g_retire_slot, 0x40000000, (uint32)ts, (uint32)(ts >> 32)};
    bool wake;
    {
        std::lock_guard<std::mutex> l(g_rb_lock);
        g_rb.insert(g_rb.end(), std::begin(words), std::end(words));
        wake = g_rb_waiting;
    }
    if (wake) g_rb_cv.notify_one();
    return ts;
}
uint64_t last_submitted() { return g_last_submitted; }
uint64_t retired() { return g_retired; }
void wait_retired(uint64_t ts) {
    std::unique_lock<std::mutex> l(g_retire_lock);
    g_retire_cv.wait(l, [&] { return g_retired >= ts; });
}

uint32_t flip_requests() { return _swapEndianU32(LatteGPUState.sharedArea->flipRequestCountBE); }
uint32_t flips_executed() { return _swapEndianU32(LatteGPUState.sharedArea->flipExecuteCountBE); }
void add_flip_request() {
    LatteGPUState.sharedArea->flipRequestCountBE = _swapEndianU32(_swapEndianU32(LatteGPUState.sharedArea->flipRequestCountBE) + 1);
}
void wait_for_flip() {
    std::unique_lock<std::mutex> l(g_sync_lock);
    if (flip_requests() == flips_executed()) return;
    g_flip_cv.wait_for(l, std::chrono::milliseconds(100));
}
void wait_for_vsync() {
    std::unique_lock<std::mutex> l(g_sync_lock);
    uint64 v = g_vsync_count;
    g_vsync_cv.wait_for(l, std::chrono::milliseconds(100), [&] { return g_vsync_count != v; });
}
uint64_t last_flip_time() { return g_last_flip_time; }
uint64_t last_vsync_time() { return g_last_vsync_time; }
uint32_t swap_interval() { return LatteGPUState.sharedArea ? LatteGPUState.sharedArea->swapInterval : 1; }
void set_swap_interval(uint32_t v) {
    if (LatteGPUState.sharedArea) LatteGPUState.sharedArea->swapInterval = v;
}
void set_event_sink(std::function<void(Event)> sink) { g_sink = std::move(sink); }
uint32_t g_regs[kRegCount];  // unused with the Cemu CP (kept for the interface)
}  // namespace latte
