// VR: the world is simulated once per frame although each frame runs the game's calc twice (vr_frame.cpp). Actor
// jobs, the player's update, and the event / world / awareness jobs each run in one of the two passes only. A port of
// BetterVR's patch_RND_StereoRendering_ActorJobs.asm and hook_RouteActorJob (src/hooking/settings.cpp); the sites
// and their original instructions are listed in docs/bettervr-patch-map.md section 2.2 (A1..A22). With VR off every
// patch here leaves the game's code as it is.
#include <atomic>
#include <cstring>
#include <cmath>
#include <mutex>
#include <string>
#include <unordered_map>

#include "hooks.h"
#include "vr.h"

using mem::r32;
bool vr_actor_in_view(uint32_t actor, float out[3]);  // vr_render.cpp

namespace vr {
extern std::atomic<int> g_jobs_running;  // vr_frame.cpp: the eye switch waits for these
}

namespace {
constexpr uint32_t kPlayerClimbState = 0x16e8;         // byte in the player actor (BetterVR: "climbing state")
constexpr uint32_t kUpdatePlayerNormalState = 0x02d149a0;  // updatePlayerNormalStateForClimbing(playerNormal)
uint32_t g_player_normal;                              // the player's state object (A2)

// The actor jobs the scheduler calls through delegates the actor registration fills in (A5..A18). Each delegate
// gets a host wrapper instead of the job; the wrapper runs the job in one pass (hook_RouteActorJob):
//   player: job0_1 in the right pass, and in the left pass only its climbing part; the others in the left pass
//   other actors: job0_1 in the right pass, the others in the left pass
enum Job { kJob0_1, kJob0_2, kJob1_1, kJob1_2, kJob2_1, kJob2_2, kJob4, kJobs };
constexpr uint32_t kJobFn[kJobs] = {0x037a5db0, 0x037a7d3c, 0x037a6ec4, 0x037a7cb8, 0x037a7438, 0x037a7e30, 0x037a7c00};
constexpr const char* kJobName[kJobs] = {"job0_1", "job0_2", "job1_1", "job1_2", "job2_1", "job2_2", "job4"};

std::atomic<uint32_t> g_player_actor{0};

// Which pass an actor's job0_1 belongs to. The "calc" jobs (job0_1, BaseProcMgr job type 5) are not run inside the
// calc like the other job types: BaseProcMgr::calc (0x0378ec20) runs them in a batch the main thread kicks off during
// procCalc and that keeps running on the worker threads after procCalc returned, through the pass's reset / submit, the
// next draw and past the eye switch (Mac traces 2026-11-04: the arrow's job0_1 of the right pass's batch ran after
// set_eye(0) about half of the time; the left pass's batch is waited for before the left calc returns). Routing job0_1
// by vr::eye() at run time therefore split the right pass's batch between the passes by thread timing: the player's
// job0_1 wrote the bow's loose flag (Weapon +0x768 = 6/7) and the arrow's job0_1, later in the same batch, read it only
// when it still ran before the switch; otherwise it was skipped as "left" and the bow's job1_1 / job2_1 of the next
// left calc consumed and reset the flag (-1) before anything read it, so the arrow put itself away (the 2026-11-01
// workaround of running arrows' job0_1 in the left pass only moved which side of the race shot). So job0_1 has its own
// eye, g_job_eye, which changes only when a batch starts (0x0378ec20 below, with the pass of the calc that kicked it):
// every job of a batch is routed alike, in the game's own order (priority groups: the player, then the actors after
// it, the arrow among them), as without VR, whichever thread runs it and however late. The other job types finish
// inside their calc (job1_1 at +4 ms, job2_1 at +5 ms of a 5 ms left calc) and keep the eye routing.
constexpr uint32_t kCalcJobType = 5;  // BaseProcMgr job type of job0_1 (its delegate holds job0_1: Mac 2026-11-04)
std::atomic<int> g_job_eye{0};        // 0 left, 1 right: the pass whose calc kicked the job0_1 batch running now
thread_local int t_job_pass;          // 1 / 2 while this thread runs a routed job0_1, else 0

bool is_player(uint32_t actor) {
    const char* name = mem::str(r32(actor + 4));  // BaseProc::name (sead::FixedSafeString40 at +4): its c_str
    bool player = name && strcmp(name, "GameROMPlayer") == 0;
    if (player) g_player_actor.store(actor, std::memory_order_relaxed);
    static bool logged;
    if (player && !logged) logged = true, LOG("vr: actor jobs routed per eye (player actor %08x)", actor);
    return player;
}

// Arrows (log only). Their AI shoots in the frame it reads the bow's "loose" flags (Weapon +0x768 = 6/7, written by the
// player's job0_1), from its own job0_1 in the same queue; 2026-11-01 moved their job0_1 to the left pass to dodge the
// race described above, which made the arrow read the flag after the bow's job1_1 / job2_1 instead (-1: put away)
// unless the right batch's job happened to run after the eye switch. With the batch eye they run with the player.
bool is_arrow(uint32_t actor) {
    const char* name = mem::str(r32(actor + 4));
    return name && strstr(name, "Arrow") != nullptr;
}

// BOTW_VR_ACTOR_LOG=1 (2026-11-16, far Bokoblins and horses standing still on the headset): for every enemy, horse and
// animal, each 300 stereo frames: how often each of its jobs was pushed and how often it was run, how far it is from
// the player, how far it moved, its BaseProc flags (+92: 0x17 jobs skipped by the LOD walk), + 94, the keep-active timer
// (+1068), the LOD state's flags and level (+944: +12, +20, +28). An actor that is pushed nothing shows only once more
// (its next line never comes): the "silent" list every 300 frames names those
struct ActorLog {
    int pushed[kJobs] = {}, ran[kJobs] = {};
    uint64_t since = 0, last = 0;
    float at[3] = {};
    std::string name;
};
bool actor_log_on() {
    static const bool on = [] {
        const char* e = getenv("BOTW_VR_ACTOR_LOG");
        return e && strcmp(e, "0") != 0;
    }();
    return on;
}
void actor_log(uint32_t actor, int job, bool ran) {
    const char* name = mem::str(r32(actor + 4));
    if (!name || !(strstr(name, "Enemy_") || strstr(name, "Horse") || strstr(name, "Animal_"))) return;
    static std::mutex lock;
    static std::unordered_map<uint32_t, ActorLog> recs;
    static uint64_t swept = 0;
    std::lock_guard<std::mutex> l(lock);
    const uint64_t f = vr::stereo_frames();
    const uint32_t pl = g_player_actor.load(std::memory_order_relaxed);
    auto dist = [pl](uint32_t a) {
        return pl ? hypotf(mem::rf32(a + 516) - mem::rf32(pl + 516), mem::rf32(a + 548) - mem::rf32(pl + 548)) : -1.0f;
    };
    ActorLog& r = recs[actor];
    if (!r.since) {
        r.since = f, r.name = name;
        for (int i = 0; i < 3; i++) r.at[i] = mem::rf32(actor + 516 + 16 * i);
    }
    r.last = f, r.pushed[job]++;
    if (ran) r.ran[job]++;
    if (f - r.since >= 300) {
        const uint32_t ls = r32(actor + 944);
        const float moved = hypotf(hypotf(mem::rf32(actor + 516) - r.at[0], mem::rf32(actor + 532) - r.at[1]), mem::rf32(actor + 548) - r.at[2]);
        float v[3] = {-1, -1, 0};
        vr_actor_in_view(actor, v);
        LOG("vr: actor %s %08x at %.3f %.3f of the left eye's picture, %.0f degrees left of the game camera's heading", name, actor, v[0], v[1], v[2]);
        LOG("vr: actor %s %08x %.0f m, moved %.2f m in %d frames: job0_1 %d/%d job0_2 %d/%d job1_1 %d/%d job1_2 %d/%d job2_1 %d/%d job2_2 %d/%d job4 %d/%d "
            "(run/pushed); flags %04x %04x timer %u lod %08x %08x level %u",
            name, actor, dist(actor), moved, (int)(f - r.since), r.ran[0], r.pushed[0], r.ran[1], r.pushed[1], r.ran[2], r.pushed[2], r.ran[3],
            r.pushed[3], r.ran[4], r.pushed[4], r.ran[5], r.pushed[5], r.ran[6], r.pushed[6], mem::r16(actor + 92), mem::r16(actor + 94),
            mem::r8(actor + 1068), ls ? r32(ls + 12) : 0, ls ? r32(ls + 20) : 0, ls ? r32(ls + 28) : 0);
        r = ActorLog{};
    }
    if (f - swept >= 300) {  // the actors no job was pushed for since
        swept = f;
        for (auto it = recs.begin(); it != recs.end();) {
            if (f - it->second.last >= 300 && it->second.last) {
                LOG("vr: actor %s %08x silent: no job pushed for %d frames", it->second.name.c_str(), it->first, (int)(f - it->second.last));
                it = recs.erase(it);
            } else {
                ++it;
            }
        }
    }
}

template <Job J>
void job_wrapper(PPCContext* c) {
    const uint32_t actor = c->r[3];
    const int eye = vr::eye();
    if (actor_log_on()) {
        const bool runs = J == kJob0_1 ? g_job_eye.load(std::memory_order_acquire) == 1 : eye == 0;
        actor_log(actor, J, runs);
    }
    if (J == kJob0_1) {
        const int pass = g_job_eye.load(std::memory_order_acquire) + 1;
        // the race g_job_eye settles: a job of the right pass's batch run after the eye switch (or the reverse)
        if (pass != eye + 1) {
            static std::atomic<int> n{0};
            const int k = n.fetch_add(1, std::memory_order_relaxed) + 1;
            if (k <= 20 || k % 500 == 0) {
                const char* name = mem::str(r32(actor + 4));
                LOG("vr: job0_1 of %s of the %s pass's batch ran with eye %d (%d so far)", name ? name : "?",
                    pass == 2 ? "right" : "left", eye, k);
            }
        }
        if (pass != 2) {
            if (!is_player(actor)) return;
            // the player's climbing state for the jobs that follow in this pass
            mem::w8(actor + kPlayerClimbState, 0);
            t_job_pass = 1;
            if (g_player_normal) guest_call(c, kUpdatePlayerNormalState, {g_player_normal});
            t_job_pass = 0;
            return;
        }
    } else if (eye == 1) {
        return;
    }
    vr::g_jobs_running.fetch_add(1, std::memory_order_acq_rel);
    if (J == kJob0_1) t_job_pass = 2;
    rt_call(c, kJobFn[J]);
    t_job_pass = 0;
    vr::g_jobs_running.fetch_sub(1, std::memory_order_acq_rel);
}

uint32_t job_thunk(Job j) {
    static uint32_t thunks[kJobs];
    static constexpr GuestFn kWrappers[kJobs] = {job_wrapper<kJob0_1>, job_wrapper<kJob0_2>, job_wrapper<kJob1_1>,
                                                 job_wrapper<kJob1_2>, job_wrapper<kJob2_1>, job_wrapper<kJob2_2>,
                                                 job_wrapper<kJob4>};
    if (!thunks[j]) thunks[j] = dispatch::thunk(kWrappers[j], kJobName[j]);
    return thunks[j];
}
}  // namespace

uint32_t vr::player_actor() { return g_player_actor.load(std::memory_order_relaxed); }
int vr::job_eye() { return t_job_pass ? t_job_pass - 1 : vr::eye(); }

// BaseProcMgr::calc(mgr r3, job type r4, priority r5, r6): a batch of actor jobs of one type. For job0_1's type the
// pass of the calc that kicks it (the main thread's; 0 when this instance starts on a worker after procCalc returned,
// which keeps the eye of the kick) becomes the job eye: the left calc's batch cannot start before the right pass's
// batch is over (one double-buffered job list, and the game waits for it), so from here the right pass's late jobs are
// done and the left pass's own ones are not run; the right calc's batch flips it back.
BOTW_REPLACE(0x0378ec20) {
    if (!vr::enabled() || c->r[4] != kCalcJobType) return orig_0x0378ec20(c);
    const int pass = vr::calc_pass(), eye = vr::eye();
    if (pass) g_job_eye.store(pass - 1, std::memory_order_release);
    static std::atomic<int> n{0};
    const int k = n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (k <= 12 || k % 2000 == 0)
        LOG("vr: job0_1 batch %d: kicked in calc pass %d (eye %d, job eye %d, priority %d)", k, pass, eye,
            g_job_eye.load(std::memory_order_relaxed), (int)c->r[5]);
    orig_0x0378ec20(c);
}

// AI__Arrow isBowLoosing (0x026ff700): lwz r10, 0x768(r31) is the read that decides the shot. Log each loose the
// arrow sees, with the pass of the job reading it (one headset test: "bow released" / this / "vr: shot" should count
// the same).
BOTW_HOOK(0x026ff7e0) {
    if (!vr::enabled() || (c->r[10] != 6 && c->r[10] != 7)) return;
    static int logged;
    if (logged < 60) logged++, LOG("vr: arrow read the bow's loose flag %u (job0_1 pass %d, eye %d)", c->r[10], t_job_pass, vr::eye());
}

// A1: an actor's skip-job-push timer starts at 4 instead of 2 (li r12, 2): with two calcs a frame it would run out
// twice as fast, and link / event tags (dungeon doors) would miss updates
BOTW_HOOK(0x037a1b3c) {
    if (vr::enabled()) c->r[12] = 4;
}

// A2: mr r3, r31 in the player state constructor: remember the object (r31)
BOTW_HOOK(0x02d0a168) { g_player_normal = c->r[31]; }

// A3: in updatePlayerNormalStateForClimbing, the right pass sees no physics (r3 = 0) and skips the state update
// (the pass of the job0_1 running it, which may outlive its calc; vr::job_eye)
// Not on a ladder (moveBitFlags +0x8dc: 0x10 without the wall 0x80 or swimming 0x400): the ladder's own update runs in
// the right pass (vr_ladder.cpp's log: "on a ladder ... pass 2"), and with no physics there it never moved Link up
// (device 2026-11-05 / 06, about ten tries; BetterVR, with the same patch, has its players jump up ladders instead)
bool on_ladder() {
    const uint32_t p = g_player_actor.load(std::memory_order_relaxed);
    if (!p) return false;
    const uint32_t f = r32(p + 0x8dc);
    return (f & 0x10) && !(f & (0x80 | 0x400));
}
BOTW_SITE(0x02d149dc) {
    // on a ladder the other way round, still once a frame: only the right pass, where the ladder's update runs (both
    // passes at once, 2026-11-06, Link climbed by jumping but went through the tower's wall)
    if (vr::enabled() && vr::job_eye() == (on_ladder() ? 0 : 1)) c->r[3] = 0;
    else rt_call(c, 0x037966a8);  // getPhysicsField60
}

// A4: stb r25, 0x16e8(r30): BetterVR stores 0 there (and leaves r25 = 0)
BOTW_HOOK(0x02d5c67c) {
    if (!vr::enabled() || on_ladder()) return;  // a ladder keeps its climbing state (above)
    mem::w8(c->r[30] + kPlayerClimbState, 0);
    c->r[25] = 0;
}

// A5..A18: the job addresses loaded into the delegates (the addi of each lis / addi pair)
BOTW_HOOK(0x03795760) { if (vr::enabled()) c->r[27] = job_thunk(kJob0_1); }
BOTW_HOOK(0x03795768) { if (vr::enabled()) c->r[25] = job_thunk(kJob0_2); }
BOTW_HOOK(0x03795840) { if (vr::enabled()) c->r[25] = job_thunk(kJob1_1); }
BOTW_HOOK(0x03795850) { if (vr::enabled()) c->r[20] = job_thunk(kJob1_2); }
BOTW_HOOK(0x03795904) { if (vr::enabled()) c->r[20] = job_thunk(kJob2_1); }
BOTW_HOOK(0x03795914) { if (vr::enabled()) c->r[19] = job_thunk(kJob2_2); }
BOTW_HOOK(0x037959d0) { if (vr::enabled()) c->r[21] = job_thunk(kJob4); }

// A19: the player's velocity update (0x0344cecc) from this site once a frame (BetterVR: in the right pass only, else
// gravity doubles). The site is the controller loop of the physics system's "phase 2" job (0x037ffde0; on the Mac the
// only caller of the update for the player's controller, 2026-11-04), which the worker threads run on their own time,
// twice a stereo frame: mostly inside the left calc, and for the right pass after procCalc returned, as late as the
// next frame's left draw and past set_eye(0); now and then the left pass's run came after set_eye(1). Routed by
// vr::eye() at run time, as BetterVR routes it (Cemu runs the job in its pass), the update ran in only a fifth of the
// stereo frames on the Mac (both runs of a frame seen with eye 0) and twice in some: with no update the physics body
// keeps its velocity and gravity is not applied, so a jump rose at a steady 0.18-0.23 m a frame for seconds ("Link
// floats up": Mac "jump 24.85 m: up 5.14 s", headset 2026-11-04 "jump 15.11 m: up 2.96 s", "9.95 m: up 2.48 s, down
// 2.51 s" at half speed), and walking was sluggish ("jumps rose and fell at about 40 % speed", vr_roomscale.cpp).
// So each controller's update runs on the first of its calls in a stereo frame and the later ones are skipped: once
// a frame, whichever pass's run comes first (normally the left calc's, right after the right pass's player job0_1 of
// the frame before has set the velocity it is to move with), however late the other runs.
BOTW_SITE(0x037ffe34) {
    bool run = true;
    if (vr::enabled()) {
        static std::mutex lock;
        std::lock_guard<std::mutex> l(lock);
        static std::unordered_map<uint32_t, uint64_t> last;  // controller -> the stereo frame of its last update
        const uint64_t f = vr::stereo_frames();
        if (last.size() > 64) last.clear();
        auto it = last.find(c->r[3]);
        run = it == last.end() || it->second != f;
        if (run) last[c->r[3]] = f;
        // the player's controller: every 900 stereo frames, how many frames ran it once / never / twice (should be
        // 900 / 0 / 0) and how often the eye at run time would have routed it the other way (the late runs above)
        const uint32_t pl = vr::player_actor();
        const uint32_t pc = pl && r32(pl + 0x3a0) ? r32(r32(pl + 0x3a0) + 0x50) : 0;
        if (pc && c->r[3] == pc) {
            static uint64_t frame = ~0ull, report = 0;
            static int n, hist[3], frames, spills, logged;
            if (frame == ~0ull) frame = f, report = f;
            if (f != frame) {
                hist[n > 2 ? 2 : n]++, frames++;
                for (uint64_t k = frame + 1; k < f; k++) hist[0]++, frames++;
                frame = f, n = 0;
            }
            if (run) n++;
            if (run != (vr::eye() == 1)) spills++;
            if (f - report >= 900 && logged < 100) {
                logged++;
                LOG("vr: player velocity update: of %d frames %d ran it once, %d never, %d twice; %d calls the eye would have routed the other way",
                    frames, hist[1], hist[0], hist[2], spills);
                report = f, frames = spills = 0, memset(hist, 0, sizeof hist);
            }
        }
    }
    if (run) rt_call(c, 0x0344cecc);
}

// A20: the event update job (r4 = 0x1054c430) is queued in the right pass only (else events run at double speed)
BOTW_SITE(0x031fbc78) {
    if (!vr::enabled() || vr::eye() == 1) rt_call(c, 0x030df7e8);  // FixedSizeJQ::enque
}
// A21: the world update job (weather, WorldMgr::calc; r4 = 0x1054c43c) in the left pass only
BOTW_SITE(0x031fbc84) {
    if (!vr::enabled() || vr::eye() == 0) rt_call(c, 0x030df7e8);
}
// A22: ksys::act::Awareness::calc (0x0317a128) in the left pass only. Its only caller is the three-instruction
// function at 0x031fde34 (r3 = the Awareness instance, tail jump at 0x031fde3c), which the recompiler merges with it
BOTW_REPLACE(0x031fde34) {
    if (vr::enabled() && vr::eye() == 1) return;
    orig_0x031fde34(c);
}
