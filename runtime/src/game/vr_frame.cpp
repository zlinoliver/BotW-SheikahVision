// VR frame loop. With VR on, sead::GameFramework::procFrame (0x031fa880) runs the draw and calc steps twice, once per
// eye; the second (right-eye) pass skips everything that would advance the game, so the world is simulated once and
// drawn twice. A port of BetterVR's patch_RND_StereoRendering.asm (custom_sead_GameFramework_procFrame and the
// filters after it), written as host code that calls the game's own functions. With VR off every patch here runs
// the original code.
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <thread>

#include "hooks.h"
#include "vr.h"

extern "C" void hook_0x031fa97c(PPCContext* c);  // game/fps.cpp: frame time from the frame-delta store


namespace vr {
std::atomic<int> g_jobs_running{0};  // actor jobs inside their routed body (vr_actors.cpp)
namespace {
std::atomic<int> g_eye{0};
std::atomic<int> g_calc_pass{0};  // 1 / 2 while procFrame runs the left / right pass's calc
std::atomic<uint64_t> g_stereo_frames{0};
}  // namespace

bool enabled() {
    static const bool on = [] {
        const char* e = getenv("BOTW_VR");
        bool v = e && strcmp(e, "0") != 0;
        if (v) LOG("vr: stereo rendering on (BOTW_VR)");
        return v;
    }();
    return on;
}
int eye() { return g_eye.load(std::memory_order_relaxed); }
uint64_t stereo_frames() { return g_stereo_frames.load(std::memory_order_relaxed); }
int calc_pass() { return g_calc_pass.load(std::memory_order_relaxed); }
}  // namespace vr

extern "C" int botw_vr_enabled(void) { return vr::enabled(); }  // ios/src/vr_bridge.h

namespace {
using mem::r32;
using mem::w32;

// guest functions (names as in BetterVR)
constexpr uint32_t kGetTimeForGameUpdate = 0x031fb1b4;  // (framework + 0x388)
constexpr uint32_t kLockFrameDrawContext = 0x0309f72c;
constexpr uint32_t kUnlockFrameDrawContext = 0x0309f744;
constexpr uint32_t kTaskMgrCalcDestruction = 0x030a27cc;
constexpr uint32_t kCalculateUI = 0x031faf00;
// sead::GameFrameworkCafe virtual functions (byte offsets in its vtable)
constexpr uint32_t kProcDraw = 0xf4, kProcCalc = 0xfc, kProcReset = 0x7c, kPresentAndDrawDone = 0x10c,
                   kWaitForVsync = 0x104;

uint32_t call(PPCContext* c, uint32_t fn, std::initializer_list<uint32_t> args = {}) { return guest_call(c, fn, args); }
uint32_t vcall(PPCContext* c, uint32_t obj, uint32_t slot) { return guest_call(c, r32(r32(obj) + slot), {obj}); }
uint32_t gx2(PPCContext* c, const char* name, std::initializer_list<uint32_t> args = {}) {
    return guest_call(c, dispatch::export_thunk("gx2", name), args);
}
uint64_t system_time(PPCContext* c) {
    uint32_t hi = guest_call(c, dispatch::export_thunk("coreinit", "OSGetSystemTime"));
    return ((uint64_t)hi << 32) | c->r[4];
}

// sead::GameFrameworkCafe::presentAndDrawDone without the present: runs the draw-done callback so the frame's GPU
// work is submitted, but shows nothing (the first eye; BetterVR simplified_sead_GameFrameworkCafe_presentAndDrawDone)
void submit_without_present(PPCContext* c, uint32_t fw) {
    uint32_t cb = r32(fw + 0x370);
    if (!cb) return;
    gx2(c, "GX2Flush");
    call(c, cb, {1});  // gsys::SystemTask::waitDrawDoneCallback(1)
}

// The actor jobs read the eye when they run, on the game's worker threads, which here run in parallel with this one
// (in Cemu, where BetterVR's routing comes from, every guest thread shares one host thread, so a job never sees the
// eye change under it). A job still running from one pass when the eye flips would finish its work in the other one
// and the next pass's jobs would be routed against a half-done frame (seen 2026-11-01 while tracing why arrows did not
// fly, vr_actors.cpp is_arrow: one call of the arrow's AI began in one pass and ended in the other). So the eye only
// changes once no routed job is running.
void set_eye(int e) {
    const auto start = std::chrono::steady_clock::now();
    while (vr::g_jobs_running.load(std::memory_order_acquire) > 0) {
        if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(50)) {
            static int warned;
            if (warned++ < 5) LOG("vr: actor jobs still running after 50 ms at the eye switch");
            break;
        }
        std::this_thread::yield();
    }
    vr::g_eye.store(e, std::memory_order_release);
}

// The frame's present step as the original procFrame does it (0x031fa8f0..0x031fa95c), for the second eye.
void present(PPCContext* c, uint32_t fw) {
    uint32_t flags = r32(fw + 0x74);
    bool show = !((flags & 1) && (flags & 2));
    uint32_t lock = r32(fw + 0x4c);
    if (lock) call(c, lock, {1});
    if (show) vcall(c, fw, kPresentAndDrawDone);
    if (lock) call(c, lock, {0});
}
}  // namespace

extern "C" void botw_vr_new_frame(PPCContext* c);  // vr_render.cpp: the frame's headset pose and event (view mode)
extern "C" void botw_gx2_vr_set_eye(unsigned eye);  // gx2/GX2_RenderTarget.cpp: marks the eye in the GPU command stream
extern "C" void botw_vr_latch_right_pose(void);     // vr_render.cpp

BOTW_REPLACE(0x031fa880) {
    if (!vr::enabled()) return orig_0x031fa880(c);
    botw_vr_new_frame(c);
    const uint32_t fw = c->r[3];
    call(c, kGetTimeForGameUpdate, {fw + 0x388});
    call(c, kLockFrameDrawContext, {fw});
    call(c, kTaskMgrCalcDestruction, {r32(fw + 0x1c)});
    call(c, kCalculateUI);

    // left eye: draw, calc (advances the game), reset; its GPU work is submitted but not shown
    // (each draw pass is marked with its eye in the GPU command stream for the renderer's stereo paths; the first
    // draw of a frame still belongs to the previous frame's right eye, see the end of the frame)
    botw_gx2_vr_set_eye((unsigned)vr::eye());
    vcall(c, fw, kProcDraw);
    set_eye(0);
    vr::g_calc_pass.store(1, std::memory_order_relaxed);
    vcall(c, fw, kProcCalc);
    vr::g_calc_pass.store(0, std::memory_order_relaxed);
    vcall(c, fw, kProcReset);
    submit_without_present(c, fw);
    // BOTW_VR_WAIT_PREV_DRAW=1 (an experiment, off by default): the commands submitted so far, the frame's first draw. The
    // right pass's calc rewrites that buffer, and the left eye's own commands (the draw below) stay unwaited: if the
    // game's double buffering keeps the eyes apart, the right calc can overlap the command thread
    static const bool wait_prev_draw = [] { const char* e = getenv("BOTW_VR_WAIT_PREV_DRAW"); return e && strcmp(e, "0") != 0; }();
    uint32_t prev_hi = 0, prev_lo = 0;
    if (wait_prev_draw) {
        prev_hi = gx2(c, "GX2GetLastSubmittedTimeStamp"), prev_lo = c->r[4];
        static bool logged;
        if (!logged) logged = true, LOG("vr: the right pass waits only for the frame's first draw (BOTW_VR_WAIT_PREV_DRAW)");
    }

    // right eye: draw, calc with the game-advancing parts skipped (the filters below), reset, present
    botw_gx2_vr_set_eye((unsigned)vr::eye());
    vcall(c, fw, kProcDraw);
    // keep the left eye's command buffers alive until the GPU is past them (BetterVR's GX2DrawDone replacement)
    // (needed: without it the right pass rewrites display lists the GPU has not read yet, and Cemu's command processor
    // reports "Detected error in GPU command buffer" within half a minute; tested 2026-10-31)
    uint32_t ts_hi = gx2(c, "GX2GetLastSubmittedTimeStamp"), ts_lo = c->r[4];
    if (wait_prev_draw) ts_hi = prev_hi, ts_lo = prev_lo;
    gx2(c, "GX2WaitTimeStamp", {ts_hi, ts_lo});
    set_eye(1);
    botw_vr_latch_right_pose();  // vr_render.cpp: the right eye's pose through its calc and the next frame's draw
    vr::g_calc_pass.store(2, std::memory_order_relaxed);
    vcall(c, fw, kProcCalc);
    vr::g_calc_pass.store(0, std::memory_order_relaxed);
    vcall(c, fw, kProcReset);
    present(c, fw);
    // the eye stays 1 until the next frame's left calc: the next frame's first draw belongs to this right eye, as in
    // BetterVR (currentEyeSide is set before each calc and never reset)

    // the rest of the original procFrame (0x031fa95c..), frame time through game/fps.cpp's hook as it would run there
    call(c, kUnlockFrameDrawContext, {fw});
    uint64_t now = system_time(c);
    uint64_t last = ((uint64_t)r32(fw + 0x80) << 32) | r32(fw + 0x84);
    uint64_t dt = now - last;
    w32(fw + 0x7c, (uint32_t)dt);
    w32(fw + 0x78, (uint32_t)(dt >> 32));
    {
        uint32_t r0 = c->r[0], r12 = c->r[12], r30 = c->r[30];
        c->r[0] = (uint32_t)(dt >> 32), c->r[12] = (uint32_t)dt, c->r[30] = fw;
        hook_0x031fa97c(c);
        c->r[0] = r0, c->r[12] = r12, c->r[30] = r30;
    }
    now = system_time(c);
    w32(fw + 0x84, (uint32_t)now);
    w32(fw + 0x80, (uint32_t)(now >> 32));
    if (r32(fw + 0x28) == 1) w32(fw + 0x28, 2);
    uint32_t flags = r32(fw + 0x74);
    if (flags & 1) w32(fw + 0x74, flags ^ 2);
    vcall(c, fw, kWaitForVsync);
    vr::g_stereo_frames.fetch_add(1, std::memory_order_relaxed);
}

// ---- right-eye filters --------------------------------------------------------------------------------------------

// uking::frm::System: preCalc, calcAndRunStateMachine, postCalc (the game's update, called back to back here).
// Right eye: none of them; only the background camera's postCalc, which the draw needs (BetterVR custom_PostCalc).
BOTW_SITE(0x02c4bfe0) {
    if (!vr::enabled() || vr::eye() == 0) return rt_call(c, 0x02c57e4c);
    uint32_t game_scene = r32(c->r[3] + 8);
    uint32_t binder = r32(game_scene + 0x200);
    if (!binder || r32(0x10463e64) == 2) return;  // GameScene::sBinderStatus
    uint32_t stage = call(c, 0x02c4fad4, {binder});  // StageBinder::getStage
    if (!stage || r32(r32(stage) + 0x4c) != 0x02c44944) return;  // only MainFieldDungeonStage::postCalc
    uint32_t sp = c->r[1];
    c->r[1] = sp - 0x20;  // a guest stack slot for the callback object
    w32(c->r[1] + 8, 0x101c1474);
    call(c, 0x02c42ff0, {stage, c->r[1] + 8});  // BackgroundCamera::postCalc
    c->r[1] = sp;
}
BOTW_SITE(0x02c4bfec) {
    if (!vr::enabled() || vr::eye() == 0) rt_call(c, 0x02c57e7c);
}
BOTW_SITE(0x02c4bff8) {
    if (!vr::enabled() || vr::eye() == 0) rt_call(c, 0x02c57ed8);
}

// sead::MethodTreeNode::callRec's call of a node's delegate (bctrl, r10 = the delegate's invoke function). Right eye:
// only the gsys::SystemTask and RootTask / ControllerMgr delegates (drawing and input), not the game's calc tree.
BOTW_SITE(0x0309fa60) {
    if (vr::enabled() && vr::eye() == 1 && c->r[10] != 0x03a14d74 && c->r[10] != 0x030a0594) return;
    rt_call(c, c->ctr);
}

// A model's draw request flags (stb r11, 125(r31)): BetterVR sets bit 2 outside the right eye, so models stay
// requested for the second draw although Actor::update (which requests them) does not run then.
BOTW_SITE(0x03987ca4) {
    if (vr::enabled() && vr::eye() != 1) c->r[11] |= 2;
    mem::w8(c->r[31] + 125, (uint8_t)c->r[11]);
}

// gsys::ModelJobQueue::clear before the right eye's draw would drop the models queued by the left eye's calc.
BOTW_SITE(0x039a8d80) {
    if (!vr::enabled() || vr::eye() != 1) rt_call(c, 0x03a2406c);
}

// ---- input ------------------------------------------------------------------------------------------------------

// sead::Controller::calc (0x030bccb8; the four controller vtables 0x1027ae60 / 0x1027c79c / 0x1027c83c / 0x1027c930
// point to it): calc_ reads the controller's device (the GamePad's: CafeVPadDevice::calc 0x030d9f24 -> VPADRead) into
// the hold (+0x10c), then updateDerivativeParams_ (0x030bd694) sets the press (+0x00) and release (+0x04) as the
// change of the hold since the controller's previous calc, and counts the held frames for key repeat. ControllerMgr's
// calc runs in both passes (its delegate's invoke, 0x030a0594, is one the right-pass filter above keeps), so the right
// pass's calc finds the hold unchanged and clears the press the left pass made: whatever looks for a press in the
// right pass (the event update job, A20 in vr_actors.cpp, and the player's job0_1) or before the next left calc
// (kCalculateUI at the frame's start) never sees it. Likely why at a campfire "pass time" ignored four A presses the
// game read while B (cancel) worked (device 2026-11-04, the logs below settle it). Right pass: each controller keeps
// the left pass's state, so every reader in the frame sees the frame's input as without VR, and held buttons repeat
// at the game's rate (not twice as fast).
BOTW_REPLACE(0x030bccb8) {
    if (!vr::enabled()) return orig_0x030bccb8(c);
    // Off by default: on the device, in play, the controllers' calc ran only in the right pass, so skipping it there
    // left every button dead (device 2026-11-04). BOTW_VR_PAD_ONCE=1 skips it, for experiments only.
    static const bool twice = getenv("BOTW_VR_PAD_ONCE") == nullptr;
    const uint32_t ctl = c->r[3];
    const int pass = vr::calc_pass();
    const unsigned long long frame = (unsigned long long)vr::stereo_frames();
    const uint32_t press_before = r32(ctl);
    static int budget = 300;  // press lines (the presses of a few minutes of menus and talks)
    if (pass == 2 && !twice) {
        static bool once;
        if (!once) once = true, LOG("vr: pad: the controllers' calc in the right pass skipped (the left pass's input kept)");
        if (press_before && budget-- > 0)
            LOG("vr: pad: press %08x kept for the right pass (controller %08x, frame %llu)", press_before, ctl, frame);
        return;
    }
    orig_0x030bccb8(c);
    const uint32_t press = r32(ctl), hold = r32(ctl + 0x10c);
    static const char* const kPass[] = {"outside the calc", "left", "right"};
    if (press && budget-- > 0)
        LOG("vr: pad: press %08x (hold %08x) made in the %s pass (eye %d), controller %08x, frame %llu", press, hold,
            kPass[pass], vr::eye(), ctl, frame);
    if (press_before && !press && pass == 2 && budget-- > 0)
        LOG("vr: pad: the right pass's calc cleared the press %08x (controller %08x, frame %llu)", press_before, ctl,
            frame);
    // where the calc runs, once: the skip above only works if it runs in the frame's calc
    static bool seen[3];
    if (!seen[pass]) seen[pass] = true, LOG("vr: pad: controller calc seen in the %s pass (eye %d)", kPass[pass], vr::eye());
}
