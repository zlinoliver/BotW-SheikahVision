// Serialise the sound priority sort (0x03c1942c). It sorts voices by priority (0..127) through scratch lists in
// static memory (0x10498050, and 0x10599b70 in its callee 0x03c191f0). Of its three callers only one takes the
// game's lock (0x030bb668), and only when a flag is set; the one reached from the frame job (0x03be6920, on
// WorkerMgr threads) takes none. On the Wii U the callers apparently never overlap (threads on one core do not run
// at the same time); here every guest thread is its own host thread, two sorts can interleave, and the corrupted
// lists end in a null read (seen: SIGBUS at guest 0 in WorkerMgr/Worker2 at 0x03c192b4 after ~7 min in the open
// world). Each call site takes a host mutex just before the call and releases it right after; a call site's
// release instruction is also reached when the call is skipped, so a per-thread flag says whether to release.
//
// 2026-11-03: the lock alone did not stop the crash (Mac, three crashes at 53-161 s, all with identical registers,
// no contention logged). The registers say what broke: the list being sorted (r26, a heap object) had its first node
// equal to bucket 127's own head node in the static array, i.e. its count (r26+12) said "not empty" while its links
// were already empty, so the sort took the list's own head for a voice. A count that runs ahead of the links is what
// two unsynchronised inserts or removes on the same list leave (lwz/addi/stw of the count). So the list helpers
// 0x03c0e660 (insert) and 0x03c0e68c (remove) also take the lock when the list is one the sort has ever been given
// (registered at the sort's entry, by its count address r3+12), and BOTW_SOUND_LIST_LOG=1 logs each thread and
// return address that touches such a list the first time.
#include <pthread.h>

#include <atomic>
#include <mutex>
#include <set>
#include <utility>

#include "hooks.h"

namespace {
std::recursive_mutex g_sort_lock;
thread_local bool t_held = false;
std::atomic<uint64_t> g_contended{0}, g_calls{0};

void acquire() {
    if (!g_sort_lock.try_lock()) {
        uint64_t n = ++g_contended;
        if ((n & (n - 1)) == 0) LOG("sound sort lock: contended %llu times in %llu calls", (unsigned long long)n,
                                    (unsigned long long)g_calls.load());
        g_sort_lock.lock();
    }
    ++g_calls;
    t_held = true;
}
void release() {
    if (!t_held) return;
    t_held = false;
    g_sort_lock.unlock();
}
// count addresses (list + 12) of the lists the sort has been given; a handful, written once each
constexpr int kMaxLists = 16;
std::atomic<uint32_t> g_lists[kMaxLists];
std::atomic<int> g_nlists{0};
std::mutex g_register_lock;

bool sorted_list(uint32_t count_addr) {
    int n = g_nlists.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++)
        if (g_lists[i].load(std::memory_order_relaxed) == count_addr) return true;
    return false;
}

void register_list(uint32_t count_addr) {
    if (sorted_list(count_addr)) return;
    std::lock_guard<std::mutex> g(g_register_lock);
    if (sorted_list(count_addr)) return;
    int n = g_nlists.load(std::memory_order_relaxed);
    if (n == kMaxLists) return;
    g_lists[n].store(count_addr, std::memory_order_relaxed);
    g_nlists.store(n + 1, std::memory_order_release);
    LOG("sound sort lock: list %08x registered", count_addr - 12);
}

const bool g_log_users = [] { const char* e = getenv("BOTW_SOUND_LIST_LOG"); return e && *e == '1'; }();

void note_user(PPCContext* c, const char* op) {
    static std::mutex lock;
    static std::set<std::pair<std::string, uint32_t>> seen;
    char name[64] = "?";
    pthread_getname_np(pthread_self(), name, sizeof name);
    std::lock_guard<std::mutex> g(lock);
    if (seen.insert({name, c->lr}).second)
        LOG("sound sort lock: %s on list %08x from %08x (thread %s)", op, c->r[3] - 12, c->lr, name);
}

// a list helper on a sorted list runs under the sort's lock (recursive: the sort itself calls the helpers)
template <void (*Orig)(PPCContext*)>
void locked_helper(PPCContext* c, const char* op) {
    if (!sorted_list(c->r[3])) return Orig(c);
    if (g_log_users) note_user(c, op);
    std::lock_guard<std::recursive_mutex> g(g_sort_lock);
    Orig(c);
}
}  // namespace

// the sort's entry: r3 is the list, its count at +12
BOTW_HOOK(0x03c191f0) { register_list(c->r[3] + 12); }

// list insert (r3 = the list's count, r4 = the position, r5 = the node) and remove (r3 = count, r4 = the node)
BOTW_REPLACE(0x03c0e660) { locked_helper<orig_0x03c0e660>(c, "insert"); }
BOTW_REPLACE(0x03c0e68c) { locked_helper<orig_0x03c0e68c>(c, "remove"); }

// frame job path (no game lock): addi r3, r31, 2188 before bl 0x03c1942c; lwz r0, 20(r1) after (also the skip target)
BOTW_HOOK(0x03be695c) { acquire(); }
BOTW_HOOK(0x03be6964) { release(); }

// locked path: addi r3, r30, 4 before bl 0x03c1942c; or r3, r31, r31 after
BOTW_HOOK(0x030c6cd0) { acquire(); }
BOTW_HOOK(0x030c6cd8) { release(); }

// unlocked path: addi r3, r30, 4 before bl 0x03c1942c; lwz r3, 380(r30) after
BOTW_HOOK(0x030c6cf0) { acquire(); }
BOTW_HOOK(0x030c6cf8) { release(); }
