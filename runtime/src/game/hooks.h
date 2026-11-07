// Game patches as host hooks. BOTW_HOOK(0xADDR) { ... } runs right after the (non-branch) guest
// instruction at ADDR; tools/recompile.py finds these by text, so write each at the start of a line
// with a literal 0x%08x address and re-run the recompiler after adding or removing one.
// Addresses are for U-King.rpx 1.5.0 (v208).
#pragma once
#include "../hle.h"
#include "../os.h"

#define BOTW_HOOK(addr) extern "C" void hook_##addr(PPCContext* c)

// BOTW_REPLACE(0xADDR) { ... } runs instead of the guest function that starts at ADDR (every caller: direct calls,
// tail calls, function pointers); orig_0xADDR(c) runs the original. The replacement has the guest ABI: arguments in
// r3.., result in r3, and r1 / r13..r31 / f14..f31 as the caller left them.
#define BOTW_REPLACE(addr)                           \
    extern "C" void orig_##addr(PPCContext* c);      \
    extern "C" void replace_##addr(PPCContext* c)

// BOTW_SITE(0xADDR) { ... } runs in place of the guest instruction at ADDR, then execution continues at ADDR + 4
// (the form of a Cemu patch "0xADDR = bla handler"). If that instruction was a call, LR is ADDR + 4 as the call would
// have left it; the original call is not made unless the site makes it.
#define BOTW_SITE(addr) extern "C" void site_##addr(PPCContext* c)

// The calling thread's next GX2WaitForVsync returns immediately.
extern thread_local bool g_skip_vsync_wait;
