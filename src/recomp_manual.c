/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

/* The generated register model (g_eax/g_esp are thread-local there) and the
 * XBOX_PTR/MEM32 accessors; the register names below are its macros. */
#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"
#include "platform/frame_stats.h"
#include "clx_settings.h"
#include "clx_mouse.h"

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

extern ptrdiff_t g_xbox_mem_offset;

/* ══ Library hooks (CRT / D3D / DSOUND) ═══════════════════════════════════
 *
 * Copied from FlatOut 2 (XDK 5849, the same library build as Conker). The
 * mechanism is the same in every XDK title; only the addresses move. No FO2
 * XBE was at hand for scripts/match_hooks.py, so each address was found by
 * behaviour and constants (library-hooks.md, "No match") and checked in the
 * disassembly: `ret N`, the register holding `this`, the constants. How each
 * was found: see git history. */

/* ── CRT memmove/memcpy ───────────────────────────────────────
 *
 * Conker links one copy of the MSVC CRT memmove, 0x0046E670 (FlatOut 2 had
 * two, 0x00218550 and 0x0021BFD0; memcpy is the same body). Found by its
 * prologue bytes (55 8B EC 57 56 8B 75 0C 8B 4D 10 8B 7D 08). Their tail and backward-copy paths dispatch
 * through UnwindDown-style vectors indexed with a *negative* register -- the
 * table address in the instruction is the end of the table -- which the
 * lifter's forward table reader cannot follow, so those jumps were lowered
 * to indirect tail calls into the middle of the function and failed to
 * resolve: every copy whose length was not a multiple of 4, and every
 * overlapping backward copy, silently lost its tail.
 *
 * Replaced by the host memmove. cdecl (dst, src, count), returns dst, and
 * esi/edi are preserved, as the original restores them. Generated bodies are
 * skipped by tools.recomp --exclude-manual reading this file. FlatOut 2 also
 * had a title-own SSE memcpy (0x001C8380, a computed jump into a movsb run)
 * replaced the same way. Conker's SSE/MMX copy, 0x0045DB10, has no computed
 * jumps (plain loops and a tail by bit tests), so it is lifted as is. */
static void crt_memmove(void)
{
    uint32_t dst = MEM32(esp + 4);
    uint32_t src = MEM32(esp + 8);
    uint32_t n   = MEM32(esp + 12);

    if (n)
        memmove((void *)XBOX_PTR(dst), (const void *)XBOX_PTR(src), n);
    eax = dst;
    esp += 4;   /* return address; cdecl, the caller pops the arguments */
}

void sub_0046E670(void) { crt_memmove(); }


/* ── D3D fence wait, D3D_BlockOnTime (0x0053C190) ────────────
 *
 * stdcall (fence, flags), ret 8. The XDK 5849 D3D device (NFSU2/Carbon's
 * code) is reached through the global at 0x005499E8 (FO2 0x0024C9D8):
 *   +0x2C  write sequence, bumped as fences are inserted
 *   +0x30  pointer to the semaphore the GPU releases as it completes work
 * and this spins until *(+0x30) catches up with the fence it was asked for.
 *
 * Without the pushbuffer executor nothing ever releases the semaphore, so the
 * GPU is reported caught up. With it (RECOMP_PB_EXEC) the real semaphore is
 * handed to the executor once and the original wait runs against it.
 *
 * Found: the ret-8 function in D3D that loads the device global (the D3D
 * section's most used global), compares +0x2C with *(+0x30) and spins on
 * *(+0x30), else waits on the event at +0x1DCC (FO2 0x00240240). */
#define CLX_D3D_DEVICE_PTR 0x005499E8u
extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);
extern void sub_0053C190_gen(void);
extern uint32_t (*recomp_lat_game_seq)(void);
extern int xbox_Nv2aExecSleep(void);
extern int xbox_Nv2aWaitWord(uint32_t va, uint32_t fence, unsigned ms);

/* RECOMP_LAT_TRACE: the fence the frame being built will carry. */
static uint32_t clx_lat_seq(void)
{
    uint32_t dev = MEM32(CLX_D3D_DEVICE_PTR);
    return dev ? MEM32(dev + 0x2C) : 0;
}

void sub_0053C190(void)
{
    static int exec = -1, low = -1;
    uint32_t dev = MEM32(CLX_D3D_DEVICE_PTR);
    uint32_t sem = dev ? MEM32(dev + 0x30) : 0;

    if (exec < 0) {
        exec = getenv("RECOMP_PB_EXEC") != NULL;
        recomp_lat_game_seq = clx_lat_seq;
    }
    if (exec) {
        static uint32_t registered;
        if (sem && sem != registered) {
            nv2a_pb_set_semaphore_target(sem);
            fprintf(stderr, "[D3D] GPU semaphore at 0x%08X\n", sem);
            registered = sem;
        }
        /* Low latency (default; CLX_LOW_LATENCY=0 for the title's own):
         * Present (0x0053D700) waits on fence[(count - 1) & 1] at +0x1974,
         * which 0x0053D6C0 stored two Presents ago -- two frames in flight
         * between reading the pad and the executor. Wait on fence[count & 1]
         * instead (the last Present's): one frame less of input lag. Same
         * change as flatout2-sw 7c8478f (MIT toolkit; idea, Conker's own
         * addresses). Call sites: returns 0x0053D75E and 0x0053D7A0. */
        if (low < 0) {
            const char *e = getenv("CLX_LOW_LATENCY");
            low = !(e && *e == '0');
            fprintf(stderr, "[D3D] low latency %s\n", low
                    ? "on: Present waits on the last frame (CLX_LOW_LATENCY=0 off)"
                    : "off (the title's two frames in flight)");
        }
        if (low && dev && (MEM32(esp) == 0x0053D75Eu || MEM32(esp) == 0x0053D7A0u)) {
            uint32_t last = MEM32(dev + 0x1974 + (MEM32(dev + 0x2478) & 1) * 4);
            if (last && (int32_t)(last - MEM32(esp + 4)) > 0)
                MEM32(esp + 4) = last;
        }
        {
            /* CLX_DT_LOG=1: once a second, the game's frame step (0x002A7B60:
             * [0x007FA218] seconds, [0x007FA20C] vblank ticks, frame counter
             * [0x007F9F60]). At 60 Hz (CLX_FPS=60) the step should halve. */
            static int dtlog = -1;
            static uint32_t frames;
            if (dtlog < 0) {
                const char *e = getenv("CLX_DT_LOG");
                dtlog = e && *e == '1';
            }
            if (dtlog && (MEM32(esp) == 0x0053D75Eu || MEM32(esp) == 0x0053D7A0u)
                    && ++frames % 30 == 0)
                fprintf(stderr, "[dt] step %.4f s, ticks %u, frame %u\n",
                        (double)MEMF(0x007FA218u), MEM32(0x007FA20Cu), MEM32(0x007F9F60u));
        }
        {   /* RECOMP_FRAME_STATS: the game thread's wait for GPU progress */
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            /* The lifted function polls *sem until it reaches the fence
             * (0x0053C2C0: while (seq - fence) < (seq - *sem)): a core at
             * 100% on a PC, and on the Switch a sleep of at most 250 us a
             * turn. Sleep until the executor's semaphore release instead
             * (xbox_Nv2aWaitWord), then run the function, which finds the
             * fence passed. Only for a fence already submitted (not the
             * current sequence, which the function would insert first) and
             * no block callback at +0x19FC (the title's own wait work);
             * 100 ms at most, then the original wait takes over. Idea from
             * cdetv's conker-live-recompiled 0.1.4 (MIT); own code. */
            if (dev && sem && xbox_Nv2aExecSleep()) {
                static int said;
                uint32_t fence = MEM32(esp + 4);
                if (!said++)
                    fprintf(stderr, "[D3D] GPU-time wait sleeps (CLX_EXEC_SLEEP=0 spins)\n");
                if ((int32_t)(MEM32(dev + 0x2C) - fence) > 0 && !MEM32(dev + 0x19FC))
                    xbox_Nv2aWaitWord(sem, fence, 100);
            }
            sub_0053C190_gen();
            clock_gettime(CLOCK_MONOTONIC, &t1);
            fs_add(FS_GAME_WAIT, (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull
                                 + (uint64_t)t1.tv_nsec - (uint64_t)t0.tv_nsec);
        }
        return;
    }
    if (sem)
        MEM32(sem) = MEM32(dev + 0x2C);     /* "GPU" is always caught up */
    eax = 0;
    esp += 4 + 8;                           /* return address, 2 stdcall args */
}


/* ── D3D interrupt handlers at DISPATCH_LEVEL ────────────────
 *
 * D3D services the GPU from its DPC (0x(DPC)) and, with the interrupt
 * masked, from its own busy-waits (BlockUntilIdle and friends) on the game
 * thread: both call the vblank handler (0x00540F70) and the PGRAPH handler
 * (0x005411D0), which queues flips. On the Xbox the two never interleave --
 * one is a DPC, the other runs with the interrupt off. Here the DPC runs on
 * the kernel timer thread, and lifted code hands over the guest lock at any
 * function entry, so the vblank count could move between D3D computing a
 * flip's target vblank and checking it. D3D retires a flip only when the
 * count *equals* its target, so that flip stayed pending forever and
 * PersistDisplay, which waits for pending flips, never returned: the hang
 * starting a Quick Race or Career. Run at DISPATCH_LEVEL (the dispatch lock,
 * kernel_hal.c), neither can be interrupted by the other.
 *
 * Raising blocks on the lock that the timer thread holds while it waits for
 * the guest lock, so the guest lock is let go for the raise, as a kernel
 * call would; already at DISPATCH (inside the DPC) there is nothing to take.
 *
 * Conker: vblank handler 0x00540F70 (FO2 0x00242AF0), PGRAPH handler
 * 0x005411D0 (FO2 0x00243060). Found by their register offsets: the vblank
 * handler writes PCRTC_INTR (+0x600100) and spins on PMC_INTR bit 24 (+0x100);
 * PGRAPH reads PGRAPH_INTR/NSOURCE (+0x400100/+0x400108). The six callers
 * call both, as in FO2. Unlike FO2, Conker's LTCG D3D passes the block to
 * the vblank handler in eax, not ecx (every caller: mov eax, esi/edi). */
extern uint8_t xbox_KfRaiseIrql(uint8_t irql);
extern void xbox_KfLowerIrql(uint8_t irql);
extern uint8_t xbox_CurrentIrql(void);
extern int xbox_gil_suspend(void);
extern void xbox_gil_resume(int depth);
extern void xbox_Nv2aVblankTaken(void);

static uint8_t d3d_isr_enter(void)
{
    uint8_t old = xbox_CurrentIrql();

    if (old < 2) {
        int d = xbox_gil_suspend();
        old = xbox_KfRaiseIrql(2);
        xbox_gil_resume(d);
    }
    return old;
}

static void d3d_isr_leave(uint8_t old)
{
    if (old < 2)
        xbox_KfLowerIrql(old);
}

/* Flips retired by a handler call: D3D's retire index, at +0x1BC of the
 * block both handlers take (Conker: 0x00540EA0 bumps +0x1BC on a retire,
 * +0x1C0 is the vblank count) (their own reads index the pending-flip
 * slots from it). The pushbuffer executor's FLIP_STALL waits on these, as
 * PGRAPH waits on the retire's PGRAPH_INCREMENT write. */
extern void nv2a_pb_flip_retired(unsigned n);

/* Vblank handler, block in eax (LTCG). It ends by writing PCRTC_INTR and spinning until
 * PMC_INTR bit 24 drops. Its callers (the DPC, D3D's busy-waits) have read
 * the bits before calling it, so this is the vblank being taken: clear them
 * now, and the spin ends at once instead of holding the guest lock until the
 * runtime's ack thread comes round. */
extern void sub_00540F70_gen(void);

void sub_00540F70(void)
{
    uint32_t blk = eax;
    /* Read only once at DISPATCH: read before, a retire by the other
     * handler (DPC thread vs a D3D busy-wait) in between was counted by
     * both, the executor's flip index ran one off for good, and every
     * FLIP_STALL then waited out its 250 ms (Carbon, console, 3.7 fps). */
    uint8_t old = d3d_isr_enter();
    uint32_t head = MEM32(blk + 0x1BC);

    xbox_Nv2aVblankTaken();
    sub_00540F70_gen();
    nv2a_pb_flip_retired(MEM32(blk + 0x1BC) - head);
    d3d_isr_leave(old);
}

/* PGRAPH handler, thiscall, ecx = the device's hardware block (+0 =
 * 0xFD000000). Reads a software-method trap from PGRAPH_INTR / NSOURCE /
 * TRAPPED_ADDR / TRAPPED_DATA, acknowledges it by writing PGRAPH_INTR back,
 * and acts on it: NOP(5) sets the event BlockOnTime sleeps on, a flip trap
 * queues the flip. The acknowledge is write-1-to-clear on
 * hardware and a no-op on RAM. Left pending, every turn of a busy-wait took
 * the same trap again (tens of thousands of flips a second, until the flip
 * targets were millions of vblanks ahead); guessed from the handler's
 * PGRAPH_FIFO writes, a trap posted during a call was sometimes dropped (the
 * fence event never set). So the executor posts traps under a lock held here
 * around the call, and a call that found one reports it taken, which clears
 * it (nv2a_pb_exec.c). */
extern void sub_005411D0_gen(void);
extern void nv2a_pb_trap_lock(void);
extern void nv2a_pb_trap_unlock(void);
extern void nv2a_pb_trap_taken(void);
extern void nv2a_pb_trap_mark(int out);

void sub_005411D0(void)
{
    volatile uint32_t *nv = (volatile uint32_t *)XBOX_PTR(0xFD000000u);
    uint32_t blk = ecx;
    uint8_t old = d3d_isr_enter();
    uint32_t head = MEM32(blk + 0x1BC);     /* at DISPATCH, as above */
    uint32_t nsource;

    nv2a_pb_trap_lock();
    nsource = nv[0x400108 / 4];
    /* The flip trap is NOP((interval << 5) | 1) (0x00541410 case 1: bits 0-2
     * of param >> 5 = vblanks per flip, bit 3 = immediate). Conker presents
     * with interval 2, a 30 fps cap; CLX_FPS_CAP=60 rewrites it to 1. */
    if (nsource && (nv[0x400704 / 4] & 0x1FFC) == 0x100) {
        static int uncap = -1;
        uint32_t d = nv[0x400708 / 4];
        if (uncap < 0) {
            const char *e = getenv("CLX_FPS_CAP");
            uncap = e && atoi(e) >= 60;
        }
        if (uncap && (d & 0x1F) == 1 && ((d >> 5) & 7) == 2) {
            static int said;
            nv[0x400708 / 4] = (d & ~(7u << 5)) | (1u << 5);
            if (!said++)
                fprintf(stderr, "[fps] flip interval 2 -> 1 vblank (CLX_FPS_CAP=60)\n");
        }
    }
    if (nsource) nv2a_pb_trap_mark(0);
    sub_005411D0_gen();
    if (nsource) nv2a_pb_trap_mark(1);
    nv2a_pb_flip_retired(MEM32(blk + 0x1BC) - head);   /* a flip queued on its vblank retires at once */
    if (nsource)
        nv2a_pb_trap_taken();
    nv2a_pb_trap_unlock();
    d3d_isr_leave(old);
}


/* ── Float to integer, _ftol2 (0x00471288) ──────────────────
 *
 * MSVC's CRT _ftol2: st(0) truncated to a 64-bit integer in edx:eax, popped.
 * Every (int)float cast in the title calls it; same body as Carbon's
 * 0x0032C47C (NFSU2's hottest main-thread function in a race, 4.4%).
 * It rounds with fistp
 * (the control word's mode), then corrects towards zero by the sign of the
 * float (x - r): x >= 0 and x - r < 0 -> r - 1; x < 0 (sign of (float)x)
 * and x - r > 0 -> r + 1. r == 0 or the integer indefinite is returned as
 * is. Exact, including ecx, the frame it publishes and the x87 top.
 * RECOMP_NATIVE=0 lifted, RECOMP_NATIVE_CHECK=1 both and compare.
 *
 * Conker 0x00471288 (FO2 0x00218FD4), found by its body bytes
 * (fst [esp+18h] / fistp qword [esp+10h] / fild qword [esp+10h]). */
extern void sub_00471288_gen(void);

__attribute__((noinline, cold)) static int64_t ftol_fist_slow(double x, uint16_t cw)
{
    return recomp_fist(x, cw, 64);
}

static inline int64_t ftol_fist(double x, uint16_t cw)
{
    if (__builtin_expect(((cw >> 10) & 3u) == 0 && fabs(x) < 0x1p63, 1))
        return (int64_t)rint(x);        /* host rounding is never changed: nearest */
    return ftol_fist_slow(x, cw);       /* other modes, NaN, out of range */
}

static inline void ftol2_native(double x, uint32_t *peax, uint32_t *pedx, uint32_t *pecx)
{
    int64_t r = ftol_fist(x, g_fp_control_word);
    uint32_t lo = (uint32_t)r, hi = (uint32_t)((uint64_t)r >> 32), d;
    float fx = (float)x, fd;
    uint32_t sx;

    if (lo == 0 && (hi & 0x7FFFFFFFu) == 0) {   /* 0 or the indefinite */
        *peax = lo;
        *pedx = hi;
        return;
    }
    fd = (float)(x - (double)r);
    memcpy(&d, &fd, 4);
    memcpy(&sx, &fx, 4);
    if (sx & 0x80000000u) {
        d ^= 0x80000000u;
        r += d >= 0x80000001u;          /* carry of d + 0x7FFFFFFF */
    } else {
        r -= d >= 0x80000001u;          /* borrow */
    }
    *peax = (uint32_t)r;
    *pedx = (uint32_t)((uint64_t)r >> 32);
    *pecx = d + 0x7FFFFFFFu;
}

/* Modes other than plain native, out of line so that the hot path below is
 * small enough for LTO to inline into its lifted callers. */
static int s_ftol_mode = -1;            /* 0 lifted, 1 native, 2 native + check */

__attribute__((noinline, cold)) static void ftol2_slow(void)
{
    double x = g_fp_stack[g_fp_top & 7];
    uint32_t a, d, c = ecx, frame = esp - 4u;

    if (s_ftol_mode < 0) {
        const char *e = getenv("RECOMP_NATIVE"), *k = getenv("RECOMP_NATIVE_CHECK");
        s_ftol_mode = (e && *e == '0') ? 0 : (k && *k == '1') ? 2 : 1;
    }
    if (s_ftol_mode == 0) {
        sub_00471288_gen();
        return;
    }
    ftol2_native(x, &a, &d, &c);
    if (s_ftol_mode == 2) {
        static unsigned long calls, bad;
        int top = (g_fp_top + 1) & 7;
        uint32_t sp = esp + 4u;
        sub_00471288_gen();
        calls++;
        if ((eax != a || edx != d || ecx != c || g_fp_top != top || esp != sp
             || g_ebp != frame) && bad++ < 20)
            fprintf(stderr, "[native] sub_00471288 mismatch for %.17g: lifted %08X:%08X "
                    "ecx %08X, native %08X:%08X ecx %08X\n", x, edx, eax, ecx, d, a, c);
        if ((calls & 0xFFFFFF) == 0)
            fprintf(stderr, "[native] sub_00471288: %lu calls, %lu mismatches\n", calls, bad);
        return;
    }
    eax = a;
    edx = d;
    ecx = c;
    g_fp_top = (g_fp_top + 1) & 7;
    g_ebp = g_seh_ebp = frame;
    esp += 4;
}

inline void sub_00471288(void)
{
    uint32_t a, d, c, frame;
    int top;

    if (__builtin_expect(s_ftol_mode != 1, 0)) {
        ftol2_slow();                   /* first call, lifted or check mode */
        return;
    }
    top = g_fp_top;
    c = ecx;
    frame = esp - 4u;
    ftol2_native(g_fp_stack[top & 7], &a, &d, &c);
    eax = a;
    edx = d;
    ecx = c;
    g_fp_top = (top + 1) & 7;
    g_ebp = g_seh_ebp = frame;          /* as its push ebp / mov ebp, esp left them */
    esp += 4;                           /* return address */
}


/* ── DirectSound DSP command post (0x004C763D) ───────────────
 *
 * thiscall, ecx = the DSP-side object. Copies a command block into the GP
 * DSP's scratch area, stores the command code at scratch + 0x810, and spins
 * until the DSP zeroes it (this loop does re-read memory). The scratch block
 * is ***(this + 8 + 0x10), the same chain the runtime's RECOMP_DSP_ACK notes
 * describe. There is no DSP, so register the word with the runtime's
 * completion list before running the original body.
 *
 * Conker 0x004C763D (FO2 0x00252E89): the only `add reg, 0x810` in DSOUND,
 * followed by the store and the `cmp [reg], 0 / jne` spin; ret 4. */
extern int xbox_ApuDspAckWord(uint32_t va);
extern void sub_004C763D_gen(void);

void sub_004C763D(void)
{
    uint32_t obj = MEM32(ecx + 8);
    uint32_t blk = obj ? MEM32(obj + 0x10) : 0;
    uint32_t scratch = blk ? MEM32(blk) : 0;

    if (scratch)
        xbox_ApuDspAckWord(scratch + 0x810);
    sub_004C763D_gen();
}


/* ── DirectSound EP watchdog reset (0x004C7DBF) ──────────────
 *
 * thiscall, no arguments. FlatOut 2's DSOUND (the 5849 build Carbon has, flags 0x400B;
 * NFSU1/NFSU2 have no such check) ends its per-frame work (FO2 0x0025384B) by
 * reading a word in the EP DSP's memory through the pointer at 0x0026CF50
 * (Conker 0x004E1C60)
 * (0xFE85A018); the EP firmware keeps 0xCCCCCC there. Anything else means
 * the DSP died, and it calls this: reset the whole APU (both AC'97
 * channels, a 10 ms stall). The runtime's DSPs are stubbed and the APU model
 * reads their memory as 0, so the title reset the APU every frame (~60/s)
 * and the intro movie, paced by the audio clock, froze on one frame.
 *
 * The DSP memory cannot just start holding writes: DSOUND's mailboxes there
 * "complete" today because they read back 0. So only the watchdog's call
 * (return address 0x004C80A3) is skipped; DirectSound's own resets run.
 *
 * Conker 0x004C7DBF (FO2 0x0025360B): the function called when
 * `cmp dword [[0x004E1C60]], 0xCCCCCC` fails; it resets both AC'97 channels
 * (0x004CE34C). The watchdog's call returns to 0x004C80A3 (FO2 0x002538EF;
 * the same distance from the reset function, 0x2E4). */
extern void sub_004C7DBF_gen(void);

void sub_004C7DBF(void)
{
    if (MEM32(esp) == 0x004C80A3u) {
        esp += 4;                       /* return address; no arguments */
        return;
    }
    sub_004C7DBF_gen();
}


/* ── DirectSound AC'97 channel reset (0x004CE34C) ────────────
 *
 * thiscall, ecx = channel object. Sets RR (bit 1) in the channel's NABM
 * control byte, then waits for the controller to clear it -- except MSVC
 * hoisted the load out of the loop, so the original reads the byte once and
 * spins on the register copy forever unless the bit is already clear at that
 * one read:
 *
 *     mov  byte [eax+0xFEC0010B], 2
 *     mov  cl, [eax+0xFEC0010B]
 *     and  cl, 2
 *   L: test cl, cl
 *     jne  L
 *
 * On hardware the reset has completed by then. The Windows runtime answers it
 * with a write trap on the NABM page; there is no fault handling on POSIX or
 * Switch, so this is the same function with the reset completing at once.
 * Everything else -- the lock taken through 0x004C36B0/0x004C36D2, the
 * buffer-descriptor base store, the 0xFEC0017C write for channel 1 and the
 * 0x004CE156 re-arm -- is as the original does it.
 *
 * Conker 0x004CE34C (FO2 0x002596A8), found by `mov byte [eax+0FEC0010Bh], 2`;
 * the same instructions as FO2's. Lock 0x004C36B0/0x004C36D2 (FO2
 * 0x0024EF10/0x0024EF32), re-arm 0x004CE156 (FO2 0x002594B2), NABM table
 * 0x004E1608 (FO2 0x0026C8F8), return addresses 0x004CE360/0x004CE3B3/
 * 0x004CE3BB. */
void sub_004CE34C(void)
{
    uint32_t ebp_saved = g_ebp, frame, chan, nabm;

    PUSH32(esp, ebp_saved);
    frame = esp;
    g_ebp = frame;
    g_seh_ebp = frame;
    esp -= 8;
    MEM32(frame - 4) = 0;
    PUSH32(esp, esi);
    esi = ecx;
    chan = esi;

    ecx = frame - 8;
    PUSH32(esp, 0x004CE360u); RECOMP_ABI_CALL(0x004C36B0u, sub_004C36B0);

    nabm = MEM32(MEM32(chan) * 4 + 0x004E1608u);
    MEM8(nabm + 0xFEC0010Bu) = 2;
    MEM8(nabm + 0xFEC0010Bu) &= (uint8_t)~2u;   /* reset complete */
    eax = nabm;
    SET_LO8(ecx, 0);

    MEM32(nabm + 0xFEC00100u) = MEM32(chan + 0x1C);
    if (MEM32(chan) == 1)
        MEM32(0xFEC0017Cu) = MEM32(chan + 0x28);

    PUSH32(esp, 1);
    PUSH32(esp, 1);
    ecx = chan;
    PUSH32(esp, MEM8(chan + 0x24));
    PUSH32(esp, MEM8(chan + 0x25));
    eax = MEM8(chan + 0x25);
    g_ebp = frame;
    g_seh_ebp = frame;
    PUSH32(esp, 0x004CE3B3u); RECOMP_ABI_CALL(0x004CE156u, sub_004CE156);

    ecx = frame - 8;
    g_ebp = frame;
    g_seh_ebp = frame;
    PUSH32(esp, 0x004CE3BBu); RECOMP_ABI_CALL(0x004C36D2u, sub_004C36D2);

    POP32(esp, esi);
    esp = frame;
    POP32(esp, ebp_saved);
    g_ebp = ebp_saved;
    esp += 4;
}


/* ── Debug probe: the title's clock (CLX_TIME_PROBE=1) ─────────
 *
 * 0x00416770 returns milliseconds since boot in edx:eax (rdtsc * 3 / 2200,
 * minus the base at [0x00828B48], / 1000); 0x004167B0 the same as a float in
 * st(0). Logged every 2000th call, to check the game clock. */
extern void sub_00416770_gen(void);
extern void sub_004167B0_gen(void);

static int time_probe(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("CLX_TIME_PROBE"); on = e && *e == '1'; }
    return on;
}

void sub_00416770(void)
{
    static unsigned n;
    sub_00416770_gen();
    if (time_probe() && n++ % 2000 == 0)
        fprintf(stderr, "[time] 0x416770 -> %u:%u ms\n", edx, eax);
}

void sub_004167B0(void)
{
    static unsigned n;
    sub_004167B0_gen();
    if (time_probe() && n++ % 2000 == 0)
        fprintf(stderr, "[time] 0x4167B0 -> %g (top %d)\n", g_fp_stack[g_fp_top & 7], g_fp_top);
}

/* ── D3D write-combine flush (0x0053C630) ───────────────────
 *
 * Sets PFB +0x100410 bit 16 and spins until the hardware clears it, after
 * an sfence. Guest memory here is coherent: there is nothing to flush, and
 * the spin only ended when the runtime's spin waits timed out. The PGRAPH
 * handler calls it for every NOP(12)/(13) trap (Eden: ~1000 traps/s, 410 us
 * each, 52% of the executor's time spent waiting for the handler), and the
 * main thread spent ~12% here. Same registers on return, no write, no spin. */
void sub_0053C630(void)
{
    eax = MEM32(MEM32(0x005499E8u) + 0x1C28);   /* the PFB base, as it left it */
    ecx = 0x10000;
    esp += 4;                                   /* ret */
}

/* ── Title menu: start on Game 1 (CLX_START_SINGLE, Switch default) ──
 *
 * The title screen is a camera between six items; the cursor is the byte at
 * 0x0077625D (with two copies at +1, +2): 0 = Xbox Live & Co (the multiplayer
 * front end), 3/4/5 = Game 1/2/3, wrapping left/right (written by 0x001FD0F0
 * from this handler). It starts on 0, so A opened multiplayer. 0x00045730 is
 * the title menu's per-frame handler (first called when the menu appears);
 * after its first two calls the cursor is set to Game 1, and A then offers
 * New Game / Play. CLX_START_SINGLE=0 keeps the title's own start. */
extern void sub_00045730_gen(void);

void sub_00045730(void)
{
    static int on = -1, calls;
    if (on < 0) {
        const char *e = getenv("CLX_START_SINGLE");
#if defined(__SWITCH__)
        on = !(e && *e == '0');
#else
        on = e && *e == '1';
#endif
    }
    sub_00045730_gen();
    if (on && calls < 2) {
        calls++;
        MEM8(0x0077625Du) = 3;
        MEM8(0x0077625Eu) = 3;
        MEM8(0x0077625Fu) = 3;
    }
}

/* ── Cutscene skip on the first viewing (CLX_SKIP_CUTSCENES, Switch default) ──
 *
 * 0x0004FF00 (edi = cutscene slot; eax = 1: skip it) is N64 CBFD's
 * func_1501E05C, called each frame by 0x00050020, which fades out and ends
 * the cutscene. The title's rules: L (the pad block 0x00763260 + 0x14*0x1B0
 * or + 0, word +0xA0) skips only a cutscene watched before in this slot
 * (0x0004EBE0: a bit per scene/cutscene at 0x00765608, or byte 0x007655E9
 * set = all watched), never one the script marks unskippable (0x007655F7),
 * in scene 0x1D only cutscene 5, in scene 0x18 never cutscene 1 (the
 * opening's throne room), and in scene 0x21 only START after 5 s. The
 * wrapper lifts the rest for the call: a cutscene skips with L, START or A.
 * The unskippable mark stays unless asked (as the N64 mod's default): the
 * script sets it for cutscenes that set up the game, and skipping one in the
 * opening left a node with a garbage link (crash in 0x00355430, 0.62).
 *   CLX_SKIP_CUTSCENES unset or =press: skip on a press, not the marked ones
 *     (Switch default; unset is off elsewhere)
 *   CLX_SKIP_CUTSCENES=all: skip on a press, the marked ones too
 *   CLX_SKIP_CUTSCENES=1: every cutscene ends as soon as it starts (marked too)
 *   CLX_SKIP_CUTSCENES=0: the title's own rules */
extern void sub_0004FF00_gen(void);

void sub_0004FF00(void)
{
    static int on = -1;                         /* 0 off, 1 on a press, 2 always */
    static int lift;                            /* also the unskippable ones */
    static uint32_t told = ~0u;
    static unsigned armed_at;
    static int armed;
    unsigned t;
    int go;
    uint32_t pad, l, scene;
    uint8_t seen, blocked;

    if (on < 0) {
        const char *e = getenv("CLX_SKIP_CUTSCENES");
        if (e && *e == '1')
            on = 2, lift = 1;
        else if (e && *e == '0')
            on = 0;
        else if (e && strcmp(e, "all") == 0)
            on = 1, lift = 1;
        else if (e && strcmp(e, "press") == 0)
            on = 1;
        else
#if defined(__SWITCH__)
            on = 1;
#else
            on = 0;
#endif
        fprintf(stderr, "[skip] cutscenes: %s%s\n", on == 2 ? "skipped at once (CLX_SKIP_CUTSCENES=1)"
                : on ? "L, START or A skips" : "the title's own rules",
                on && !lift ? ", not the ones the script marks unskippable" : "");
    }
    if (!on) {
        sub_0004FF00_gen();
        return;
    }
    pad = 0x00763260u + ((int8_t)MEM8(0x006BCFEBu) > 0 ? 0 : 0x14u * 0x1B0u);
    l = MEM32(pad + 0xA0);
    scene = MEM32(0x007FA1F8u);
    seen = MEM8(0x007655E9u);
    blocked = MEM8(0x007655F7u);
    /* The player's raw pressed bits (START 0x10, A 0x100, L 0x4000):
     * 0x0004D320 copies L to pad+0xA0 only while 0x008471A0 is clear. */
    /* A press asks first ("SKIP THIS CUTSCENE? PRESS AGAIN", 3 s); the
     * next press within that time skips (a stray A no longer ends one). */
    go = on == 2;
    if (on == 1 && (MEM32(MEM32(MEM32(0x008470FCu) + 8) + 8) & 0x4110)) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        t = (unsigned)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
        if (blocked && !lift) {
            if (told != (scene << 8 | MEM8(0x0076F0DCu + edi))) {
                told = scene << 8 | MEM8(0x0076F0DCu + edi);
                fprintf(stderr, "[skip] cutscene %u in scene 0x%X is marked unskippable: it plays on\n",
                        MEM8(0x0076F0DCu + edi), scene);
            }
        } else if (armed && (int)(t - armed_at) < 3000) {
            go = 1;
            armed = 0;
            clx_prompt(NULL, 0);
        } else {
            armed = 1;
            armed_at = t;
            clx_prompt("SKIP THIS CUTSCENE?  PRESS AGAIN", 3000);
            fprintf(stderr, "[skip] cutscene %u in scene 0x%X: asked (press again to skip)\n",
                    MEM8(0x0076F0DCu + edi), scene);
        }
    }
    if (go)
        MEM32(pad + 0xA0) = l | 0x4000;
    MEM8(0x007655E9u) = 1;
    if (lift)
        MEM8(0x007655F7u) = 0;
    if ((scene == 0x1D && !MEM8(0x008497F9u)) || scene == 0x21 || scene == 0x18)
        MEM32(0x007FA1F8u) = 0;
    sub_0004FF00_gen();
    if (eax)
        fprintf(stderr, "[skip] cutscene %u (slot %u) in scene 0x%X skipped%s\n",
                MEM8(0x0076F0DCu + edi), edi, scene, blocked ? " (marked unskippable)" : "");
    MEM32(0x007FA1F8u) = scene;
    MEM8(0x007655F7u) = blocked;
    MEM8(0x007655E9u) = seen;
    MEM32(pad + 0xA0) = l;
}

/* ── NaN points kept out of the collision tree (Windmill freeze) ──
 *
 * 0x00355B40 (stdcall, arg 0 = tree) drains a queue of 32-byte entries
 * (tree+0x30 base, tree+0x34 count; a point xyzw at +0) into a tree, each
 * one descending from the root to the child nearest its point (0x00354610).
 * A point with a NaN is nearest to nothing: 0x00354610 hits the title's
 * *(int*)0 = 1 assert -- a crash on the Xbox, a no-op here -- and returns
 * the entry before the child array, and the descent loops on it forever,
 * the picture frozen at full CPU. On Android a point in Windmill turns NaN
 * ~5 s after the worm cutscene starts (xyz NaN, w 212.0) whether or not it
 * is skipped; where the NaN comes from is not found yet. Such a point is
 * put at the origin instead, and logged. */
extern void sub_00355B40_gen(void);

volatile unsigned g_clx_nan_fixes;              /* android.c's [mem] line */

void sub_00355B40(void)
{
    static unsigned reported;
    uint32_t tree = MEM32(esp + 4);
    uint32_t base = MEM32(tree + 0x30), n = MEM32(tree + 0x34), i, k;

    for (i = 0; i < n && i < 4096; i++) {
        uint32_t e = base + i * 0x20;
        float x = MEMF(e), y = MEMF(e + 4), z = MEMF(e + 8), w = MEMF(e + 12);
        if (x == x && y == y && z == z && w == w)
            continue;
        g_clx_nan_fixes++;
        if (reported < 8) {
            reported++;
            fprintf(stderr, "[nan] collision tree %08X entry %u/%u: point %08X %08X %08X %08X,"
                            " +10 %08X +14 %08X, called from %08X; put at the origin\n",
                    tree, i, n, MEM32(e), MEM32(e + 4), MEM32(e + 8), MEM32(e + 12),
                    MEM32(e + 0x10), MEM32(e + 0x14), MEM32(esp));
            if (reported == 1) {
                /* The tree and the object the entry names, to find the field
                 * that went NaN and from it the code that writes it. */
                uint32_t objs[2] = { tree, MEM32(e + 0x10) }, o, j;
                for (o = 0; o < 2; o++) {
                    if (objs[o] < 0x10000 || objs[o] >= 0x04000000)
                        continue;
                    for (j = 0; j < 0x100; j += 0x20)
                        fprintf(stderr, "[nan]   %08X+%02X: %08X %08X %08X %08X %08X %08X %08X %08X\n",
                                objs[o], j, MEM32(objs[o] + j), MEM32(objs[o] + j + 4),
                                MEM32(objs[o] + j + 8), MEM32(objs[o] + j + 12),
                                MEM32(objs[o] + j + 16), MEM32(objs[o] + j + 20),
                                MEM32(objs[o] + j + 24), MEM32(objs[o] + j + 28));
                }
                for (j = 0; j < 0x400; j += 4) {
                    uint32_t v = MEM32(esp + j);
                    if (v >= 0x00011000 && v < 0x0048CF40)
                        fprintf(stderr, "[nan]   stack +%03X: %08X\n", j, v);
                }
            }
        }
        for (k = 0; k < 4; k++)
            MEMF(e + k * 4) = 0.0f;
    }
    sub_00355B40_gen();
}

/* ── Diagnostics for 0.62's crash: a tree node with a garbage link ───────
 *
 * 0x00355430 (cdecl: node, value) reads [node+0x10] and then [that+0x100];
 * its caller (0x00355C40..0x00355C9D) walks a tree with 0x00354610. On the
 * console a link of 0x0B7D1D84 faulted (on the Xbox and Linux that address is
 * a RAM mirror and reads garbage). The wrapper keeps the last 64 calls
 * (caller, node, link) and, when a link is outside RAM, logs the node's
 * words, the calls and the stack before the call goes on (and faults). */
extern void sub_00355430_gen(void);

static int guest_ram(uint32_t a)
{
    return (a >= 0x00010000u && a < 0x04000000u) || (a >= 0x80000000u && a < 0x84000000u);
}

void sub_00355430(void)
{
    static uint32_t ring[64][3];
    static unsigned n, told;
    uint32_t ret = MEM32(esp), node = MEM32(esp + 4), link = 0, i, k;

    if (guest_ram(node) && guest_ram(node + 0x3C))
        link = MEM32(node + 0x10);
    ring[n & 63][0] = ret;
    ring[n & 63][1] = node;
    ring[n & 63][2] = link;
    n++;
    /* The descent in 0x00355B40 (return 0x00355C91) takes the child 0x00354610
     * picked: array base + index * 0x20 (it returns ecx = base). Index -1 means
     * "no nearest child": every squared distance was NaN or >= FLT_MAX, or the
     * parent has no children. The title then writes [0] (its own crash) and
     * every walker that follows reads garbage: the worm crash (issues #2, #4).
     * Layout (0x20 bytes): +0 x y z r, +0x10 child array, +0x14 count, +0x15
     * level. Frame: parent at esp+0x44, the item's sphere at esp+0x94. */
    if (ret == 0x00355C91u) {
        uint32_t par = MEM32(esp + 0x44), base = 0, cnt = 0, lvl = 0;
        if (guest_ram(par) && guest_ram(par + 0x20)) {
            base = MEM32(par + 0x10);
            cnt = MEM8(par + 0x14);
            lvl = MEM8(par + 0x15);
        }
        if (node == base - 0x20u) {
            float it[4] = { MEMF(esp + 0x94), MEMF(esp + 0x98), MEMF(esp + 0x9C), MEMF(esp + 0xA0) };
            uint32_t c, finite = 0;
            static int fix = -1;
            /* The first 6 are reported in full; the fix below applies every time
             * (the console's 0.52 crashed on the first one; later ones must not). */
            int say = told < 6;
            told++;
            if (say)
            fprintf(stderr, "[node] no nearest child: parent 0x%08X level %u count %u base 0x%08X, item %g %g %g %g (%s)\n",
                    par, lvl, cnt, base, (double)it[0], (double)it[1], (double)it[2], (double)it[3],
                    (it[0] == it[0] && it[1] == it[1] && it[2] == it[2]) ? "finite or inf" : "NaN");
            for (c = 0; say && c < cnt && c < 8 && guest_ram(base) && guest_ram(base + 0x120); c++) {
                float x = MEMF(base + c * 0x20) - it[0], y = MEMF(base + c * 0x20 + 4) - it[1],
                      z = MEMF(base + c * 0x20 + 8) - it[2], d = x * x + y * y + z * z;
                fprintf(stderr, "[node]   child %u: %g %g %g %g level %u count %u d2 %g\n", c,
                        (double)MEMF(base + c * 0x20), (double)MEMF(base + c * 0x20 + 4),
                        (double)MEMF(base + c * 0x20 + 8), (double)MEMF(base + c * 0x20 + 12),
                        MEM8(base + c * 0x20 + 0x15), MEM8(base + c * 0x20 + 0x14), (double)d);
                finite += d < 3.4028235e38f;
            }
            if (say)
                fprintf(stderr, "[node]   children with a finite squared distance: %u of %u\n", finite, cnt);
            if (fix < 0) {
                const char *e = getenv("CLX_TREE_FIX");
                fix = !(e && *e == '0');
            }
            /* With children and a valid array, take the first child instead of
             * running into the title's own crash (the tree stays usable). */
            if (fix && cnt > 0 && cnt <= 8 && guest_ram(base) && guest_ram(base + 0x120)) {
                if (say)
                    fprintf(stderr, "[node]   taking child 0 (CLX_TREE_FIX=0 to stop)\n");
                node = base;
                MEM32(esp + 4) = base;
                edi = base;
                link = MEM32(base + 0x10);
            }
            fflush(stderr);
        }
    }
    if ((!guest_ram(link) || !guest_ram(link + 0x100)) && told < 6) {
        told++;
        fprintf(stderr, "[node] 0x00355430 from 0x%08X: node 0x%08X link 0x%08X (not RAM), call %u\n",
                ret, node, link, n);
        if (guest_ram(node - 0x20) && guest_ram(node + 0x40)) {
            for (i = 0; i < 24; i += 8) {
                fprintf(stderr, "[node]   0x%08X:", node - 0x20 + i * 4);
                for (k = 0; k < 8; k++)
                    fprintf(stderr, " %08X", MEM32(node - 0x20 + (i + k) * 4));
                fprintf(stderr, "\n");
            }
        }
        fprintf(stderr, "[node]   last calls (caller node link), oldest first:\n");
        for (i = n > 64 ? n - 64 : 0; i < n; i++)
            fprintf(stderr, "[node]     %08X %08X %08X\n", ring[i & 63][0], ring[i & 63][1],
                    ring[i & 63][2]);
        fprintf(stderr, "[node]   stack (code addresses from esp):");
        for (i = 0, k = 0; i < 256 && k < 24; i++) {
            uint32_t w = MEM32(esp + i * 4);
            if (w >= 0x00011000u && w < 0x0048D000u) {
                fprintf(stderr, " %08X", w);
                k++;
            }
        }
        fprintf(stderr, "\n");
        fflush(stderr);
    }
    sub_00355430_gen();
}

/* ── Nearest-node search: keep a failed result inside its array ────────────
 *
 * 0x00354610 (base, query; count in ebx) picks the node nearest a query
 * transform, returning base + best*32. When no entry is closer than FLT_MAX
 * (best stays -1) it returns base - 32 and executes `mov dword ptr [0], 1`
 * (the title's own debug break).
 *
 * The earth worm's bone palette holds a transform whose x/y/z are NaN
 * (0xFFC00000, w = 212.0). Every distance is then NaN, best stays -1, and the
 * caller at 0x00355C40 walks out of the array: node 0x01D7EA30 ->
 * 0x01D7EA10 -> 0x3F00 -> 0x3EE0 -> 0xFFFFFFE0, which faults (or, with a
 * RAM mirror, loops forever). The same crash reproduces with the official
 * Windows and Linux builds.
 *
 * Treat the failed search as "use the first entry": it is inside the array,
 * its loop counters are valid, and the walk terminates normally. Restore
 * address 0 as well -- the generated assert overwrites the fake TIB/SEH head
 * that lives there.
 */
extern void sub_00354610_gen(void);

void sub_00354610(void)
{
    uint32_t base = MEM32(esp + 4);
    uint32_t count = ebx;
    uint32_t zero = MEM32(0);

    sub_00354610_gen();
    if (eax != base - 0x20)
        return;                         /* an entry was found */
    MEM32(0) = zero;
    if (count && base >= 0x00010000u && base < 0x04000000u)
        eax = base;
}

/* ── Debug probe: failed async reads (CLX_IO_PROBE=1) ─────────
 *
 * 0x002AA990 (stdcall, 1 arg) walks the title's list of pending reads:
 * node+4 next, node+8 request, request+4 object, object+4 status. A status
 * outside 996..997 (IO incomplete/pending) sends the title to its dirty-disc screen
 * (0x001F04F0). Logs every such request before the walk. */
extern void sub_002AA990_gen(void);

void sub_002AA990(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("CLX_IO_PROBE"); on = e && *e == '1'; }
    if (on) {
        uint32_t node = MEM32(MEM32(esp + 4) + 4);
        int guard = 0;
        for (; node && guard < 64; node = MEM32(node + 4), guard++) {
            uint32_t req = MEM32(node + 8), obj = req ? MEM32(req + 4) : 0;
            uint32_t st = obj ? MEM32(obj + 4) : 0;
            if (MEM32(node + 0xC) || !obj || !st || (st > 0x3E3 && st <= 0x3E5))
                continue;
            fprintf(stderr, "[io] failed read: node %08X req %08X obj %08X status %u"
                            " obj: %08X %08X %08X %08X %08X %08X %08X %08X\n",
                    node, req, obj, st, MEM32(obj), MEM32(obj + 4), MEM32(obj + 8),
                    MEM32(obj + 12), MEM32(obj + 16), MEM32(obj + 20), MEM32(obj + 24),
                    MEM32(obj + 28));
        }
    }
    sub_002AA990_gen();
}

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    (void)xbox_va;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}

/* The camera's right-stick response curve (0x002F9E50 reads this table at
 * 0x00747798: six (input, output) pairs). Its first segment is a deadzone:
 * input below 0.4 gives 0, so a small mouse pulse did nothing. While the
 * mouse is captured, replace it with a straight line; restore the game's
 * table when the mouse is released, so a real pad keeps its own feel.
 * (PC mouse camera, clx_mouse.c; from blackn0x's PR 3.) */
void clx_camera_curve_mouse(int on)
{
    static float original[6][2];
    static int saved;
    static const float linear[6][2] = {
        { 0.0f, 0.0f }, { 0.2f, 0.2f }, { 0.4f, 0.4f },
        { 0.6f, 0.6f }, { 0.8f, 0.8f }, { 1.0f, 1.0f },
    };
    int i;
    if (!!on == saved)
        return;
    if (on) {
        if (!saved) {
            for (i = 0; i < 6; i++) {
                original[i][0] = MEMF(0x00747798u + (unsigned)i * 8);
                original[i][1] = MEMF(0x0074779Cu + (unsigned)i * 8);
            }
            saved = 1;
        }
        for (i = 0; i < 6; i++) {
            MEMF(0x00747798u + (unsigned)i * 8) = linear[i][0];
            MEMF(0x0074779Cu + (unsigned)i * 8) = linear[i][1];
        }
    } else if (saved) {
        for (i = 0; i < 6; i++) {
            MEMF(0x00747798u + (unsigned)i * 8) = original[i][0];
            MEMF(0x0074779Cu + (unsigned)i * 8) = original[i][1];
        }
        saved = 0;
    }
}

/* ── Camera pitch speed (CLX_CAM_PITCH) ───────────────────────
 *
 * 0x002F99A0 (ecx = camera, eax = the player's input block 0x00763260+..)
 * turns the right stick into camera rates. Its settings are at [camera]:
 * +0 pitch speed (50.3 in the first field, x1.25 in the code), +4 yaw speed
 * (120), +0x18/+0x1C pitch limits (-45/55). Pitch input is stick/63, yaw
 * input stick*1.25/63 clamped to 0.9, so up/down turned at about half the
 * left/right speed -- the title's tuning, slow on the Switch's short stick.
 * The wrapper scales +0 for the call only (the settings are shared and the
 * title may read them elsewhere). CLX_CAM_PITCH=<factor>: Switch default 2
 * (about the yaw speed), 1 = the title's own. Found: memory snapshots while
 * the right stick was held (pitch at camera+0x8C ran to the -45 limit),
 * a gdb write watchpoint on it, then the disassembly. */
extern void sub_002F99A0_gen(void);

void sub_002F99A0(void)
{
    static float k = -1.0f;
    int free_cam = g_clx.cam_free, invert_y = g_clx.cam_invert;   /* the menu changes them */
    uint32_t cam = ecx, cfg, inp = eax, ret = MEM32(esp);
    int32_t ry = 0;
    float saved = 0.0f, follow = 0.0f, kk;
    int mouse_on = 0;

    if (k < 0.0f) {
        const char *e = getenv("CLX_CAM_PITCH");
#if defined(__SWITCH__)
        k = e ? (float)atof(e) : 2.0f;
#else
        k = e ? (float)atof(e) : 1.0f;
#endif
        if (!(k > 0.0f && k <= 8.0f))
            k = 1.0f;
        /* camera / camera_invert_y (clx_options.c; CLX_CAM_FREE /
         * CLX_CAM_INVERT_Y win at boot). The inversion is off by default, with
         * camera=free too (the user's choice, 2026-10-22). */
        if (invert_y)
            fprintf(stderr, "[cam] vertical axis inverted (CLX_CAM_INVERT_Y)\n");
        fprintf(stderr, "[cam] pitch speed x%g (CLX_CAM_PITCH), auto-follow %s (CLX_CAM_FREE)\n",
                (double)k, free_cam ? "off" : "on");
    }
    kk = k;
    cfg = cam ? MEM32(cam) : 0;
    /* Speed, free camera and invert only for the player camera (0x0003CB17): the
     * slingshot's aim (0x002BBB4F) and the vehicles share this function and
     * keep the title's tuning (0.62: the slingshot aimed wrong). */
    {
        /* Which camera user runs: a line when another one (the slingshot's
         * aim returns to 0x002BBB54, vehicles elsewhere) takes over and when
         * the player camera comes back; 400 lines at most. */
        static uint32_t other;
        static int lines;
        if (ret != 0x0003CB1Cu && ret != other && lines < 400) {
            lines++;
            fprintf(stderr, "[cam] camera user 0x%08X%s starts\n", ret - 5,
                    ret == 0x002BBB54u ? " (aim)" :
                    ret == 0x002FBC10u ? " (pan)" : "");
        } else if (ret == 0x0003CB1Cu && other && lines < 400) {
            lines++;
            fprintf(stderr, "[cam] player camera again (after 0x%08X)\n", other - 5);
        }
        other = ret == 0x0003CB1Cu ? 0 : ret;
    }
    if (ret != 0x0003CB1Cu && ret != 0x002FBC10u) {
        /* Invert too: the slingshot's aim has the game's own "Invert Aim"
         * (pause Options). Flipping it here sent shots the other way
         * (user report, 0.45 with camera_invert_y=1). Vehicles keep the
         * title's tuning as well. */
        free_cam = 0, kk = 1.0f, invert_y = 0;
        clx_mouse_set_mode(0);
    } else {
        int pan = ret == 0x002FBC10u;   /* frying pan / first-person aim */
        clx_mouse_set_mode(pan ? 2 : 1);
        if (!pan) {
            static int seen;
            if (!seen++)
                fprintf(stderr, "[cam] player camera (0x0003CB17): pitch x%g, auto-follow %s\n",
                        (double)k, free_cam ? "off" : "on");
        } else {
            /* The pan's aim camera keeps the title's tuning; only the mouse
             * stick is fed into it. */
            free_cam = 0, kk = 1.0f, invert_y = 0;
        }
        /* clx_mouse.c adds the mouse deflection to the pad report itself, so
         * every camera/aim user sees it exactly like the right stick. This
         * wrapper only tracks that the mouse is driving the camera (for the
         * free camera and the capture timeout). */
        if (cam >= 0x10000u && cam < 0x03FFF000u &&
            cfg >= 0x10000u && cfg < 0x03FFF000u &&
            (pan || !MEM32(cam + 0x80))) {
            mouse_on = clx_mouse_camera_active();
            if (mouse_on && !pan)
                free_cam = 1;                /* no swing-back while the mouse drives it */
        }
    }
    if (cfg < 0x00010000u || cfg >= 0x04000000u || (kk == 1.0f && !free_cam && !invert_y)) {
        sub_002F99A0_gen();
        return;
    }
    /* CLX_CAM_FREE=1: with the right stick idle the function adds
     * [cam+0xC0] * [cam+0xBC] to the yaw rate (0x002F9CC2): the camera
     * swinging back behind Conker. 0 at +0xBC for the call: the camera
     * stays where the player left it. */
    if (free_cam) {
        follow = MEMF(cam + 0xBC);
        MEMF(cam + 0xBC) = 0.0f;
    }
    /* CLX_CAM_INVERT_Y=1: the input block (eax) holds the right stick's
     * vertical axis negated at +0x18 (0x0004D470); stick down raised the
     * view. Flipped for the call: stick up looks up, as most modern games. */
    if (invert_y && inp >= 0x00010000u && inp < 0x04000000u) {
        ry = (int32_t)MEM32(inp + 0x18);
        MEM32(inp + 0x18) = (uint32_t)(-ry);
    }
    /* camera=free: no ramp-up (settings +0x0C/+0x10 lerp the rate from 1 to
     * 2 over +0x14 seconds of holding: both 1.5, a constant speed) and a
     * wider pitch range (+0x18/+0x1C, -45/55 in the first field: -55/65).
     * Restored after the call (the settings are shared). */
    {
        float r0 = 0.0f, r1 = 0.0f, lo = 0.0f, hi = 0.0f;
        if (free_cam) {
            r0 = MEMF(cfg + 0x0C); r1 = MEMF(cfg + 0x10);
            lo = MEMF(cfg + 0x18); hi = MEMF(cfg + 0x1C);
            MEMF(cfg + 0x0C) = 1.5f; MEMF(cfg + 0x10) = 1.5f;
            if (lo < 0.0f && lo > -60.0f) MEMF(cfg + 0x18) = lo - 10.0f;
            if (hi > 0.0f && hi < 70.0f) MEMF(cfg + 0x1C) = hi + 10.0f;
        }
        saved = MEMF(cfg);
        MEMF(cfg) = saved * kk;
        sub_002F99A0_gen();
        MEMF(cfg) = saved;
        if (free_cam) {
            MEMF(cfg + 0x0C) = r0; MEMF(cfg + 0x10) = r1;
            MEMF(cfg + 0x18) = lo; MEMF(cfg + 0x1C) = hi;
        }
    }
    if (invert_y && inp >= 0x00010000u && inp < 0x04000000u)
        MEM32(inp + 0x18) = (uint32_t)ry;
    if (free_cam)
        MEMF(cam + 0xBC) = follow;
}

/* -- 60 fps (CLX_FPS=60, fps=60 in clx_options.txt; default 30) --
 *
 * 0x002A78D0 (dl = presentation interval) stores it in byte 0x006C0268 and
 * creates/resets the D3D device with it; the callers pass 2 (30 Hz) and 3
 * in one mode (0x002A9DD9, [0x0084A144] != 0). D3D's Present then waits two
 * vblanks, which is why rewriting only the flip trap's interval changed
 * nothing. The game step is time-based: 0x002A7B60 measures the frame time
 * (rdtsc) into [0x007FA218] (or vblank ticks x 1/60 when [0x007819D5] is
 * set; ticks = [0x006C0268] + the vblank callback's count, 0x002A8020), so
 * interval 1 should halve the step, not double the game speed. Untested on
 * the console. */
extern void sub_002A78D0_gen(void);

void sub_002A78D0(void)
{
    static int fps60 = -1;
    if (fps60 < 0) {
        fps60 = g_clx.fps60;          /* fps= in clx_options.txt, CLX_FPS */
        fprintf(stderr, "[fps] %s\n", fps60 ? "60 Hz requested (fps=60)" : "the title's 30 Hz");
    }
    if (fps60 && (edx & 0xFF) == 2) {
        fprintf(stderr, "[fps] presentation interval 2 -> 1\n");
        edx = (edx & ~0xFFu) | 1u;
    }
    sub_002A78D0_gen();
}

/* ── Hor+ 16:9 / 21:9 (CLX_ASPECT, aspect= in clx_options.txt) ──
 *
 * Conker renders 4:3 only (see main.c). 0x003FB030 (ecx = a camera context;
 * [0x0084B868] is the current one) rebuilds the context's projection every
 * frame from its parameters: flags at +8 (bit 0 off-centre, bit 1 ortho,
 * neither = perspective), viewport +0xD4..+0xE0 (x, y, w, h floats),
 * perspective fov +0x104 (degrees), aspect +0x108 (1.212 in the first field,
 * 4/3 x 10/11), near +0x10C, far +0x110. The projection goes to +0x90 and
 * the frustum planes (0x003FAE90, the culling) come from the same
 * parameters. For a full-screen 640x480 perspective context the wrapper
 * multiplies the aspect by the wide ratio over 4:3 (4/3 for 16:9, 7/4 for
 * 21:9) for the call only: the projection and the culling both widen (Hor+:
 * same vertical view, more to the sides), and the game's own logic keeps
 * reading the 4:3 aspect. The renderer presents the 640x480 surfaces at the
 * wide aspect with square pixels (RECOMP_WIDESCREEN=1 and xbox_wide_ratio,
 * set by main.c from CLX_ASPECT). 2D (HUD, menus, movies) stays full width,
 * so it is stretched. Same idea as antoxa2584x's NFSU1 0.2 "Hor+ 16:9"
 * (their game layer: ideas only); Conker's addresses found with a RAM search
 * for projection matrices, gdb access watchpoints and the disassembly. */
extern void sub_003FB030_gen(void);

void sub_003FB030(void)
{
    static int shown = -1, persist = -1;
    /* Per widened context: the game's own aspect and the value we wrote. */
    static struct { uint32_t ctx; float game, mine; } tab[16];
    int mode = g_clx.aspect;            /* aspect= / CLX_ASPECT; the menu changes it */
    float k = clx_aspect_ratio(mode) * 0.75f;
    uint32_t c = ecx;
    float a;

    if (persist < 0) {
        const char *e = getenv("CLX_HORPLUS_PERSIST");
        persist = !(e && *e == '0');
    }
    if (shown != mode) {
        shown = mode;
        fprintf(stderr, "[aspect] %s%s\n", clx_aspect_name(mode), mode ? " Hor+" : "");
    }
    if (!mode && c && persist) {
        /* Back to 4:3: a context this wrapper widened gets the game's aspect
         * again (the value it holds is still ours), and leaves the table. */
        int i;
        for (i = 0; i < 16; i++)
            if (tab[i].ctx == c) {
                if (MEMF(c + 0x108) == tab[i].mine)
                    MEMF(c + 0x108) = tab[i].game;
                tab[i].ctx = 0;
                break;
            }
    }
    if (!mode || !c || (MEM8(c + 8) & 3)
            || MEMF(c + 0xDC) != 640.0f || MEMF(c + 0xE0) != 480.0f) {
        sub_003FB030_gen();
        return;
    }
    if (persist) {
        /* Keep the widened aspect in the context, so code that reads it
         * outside this call (culling) sees it too. A small table of the
         * values we wrote: widen only an aspect the game set itself, and
         * rewiden from the game's value when the menu changes 16:9 / 21:9. */
        int i, slot = -1;
        a = MEMF(c + 0x108);
        for (i = 0; i < 16; i++)
            if (tab[i].ctx == c) { slot = i; break; }
        if (slot < 0) {
            for (i = 0; i < 16 && tab[i].ctx; i++) { }
            slot = i < 16 ? i : 0;
            tab[slot].ctx = c;
            tab[slot].mine = 0.0f;
        }
        if (a != tab[slot].mine)
            tab[slot].game = a;
        if (a != tab[slot].game * k) {
            tab[slot].mine = tab[slot].game * k;
            MEMF(c + 0x108) = tab[slot].mine;
        }
        sub_003FB030_gen();
        return;
    }
    a = MEMF(c + 0x108);
    MEMF(c + 0x108) = a * k;
    sub_003FB030_gen();
    MEMF(c + 0x108) = a;
}

/* ── Subtitle box size (CLX_BUBBLE_WRAP / _LINES / _HEIGHT) ──────
 *
 * The speech bubble wraps at 20 characters, shows up to 4 lines, and above
 * 4 lines has a fixed height of 160.0 (docs/conker-text-limits-picoripi.md).
 * The lifted layout code reads those three numbers through the getters
 * below: tools/bubble_patch.py rewrites its literals in gen/recomp_0012.c
 * (sub_000DFD90 wrap, sub_000E0850 / sub_000E0CD0 / sub_000E0E00 layouts)
 * after every regen. Default (the user's choice for the release): the wide
 * box, 40 characters, 6 lines, height 240. CLX_BUBBLE=classic (clx_options
 * bubble=classic) gives the title's 20 / 4 / 160. CLX_BUBBLE_WRAP / _LINES /
 * _HEIGHT (bubble_wrap=, bubble_lines=, bubble_height=) override either.
 * Limits: wrap 1..255 characters, lines 1..127 (a signed 8-bit compare),
 * height > 0 pixels. */
static int s_bubble_ready = -1;        /* the g_clx.bubble_wide it was read for */
static int32_t s_bubble_wrap = 40, s_bubble_lines = 6;
static float s_bubble_height = 240.0f;

static void bubble_load(void)
{
    const char *w = getenv("CLX_BUBBLE_WRAP"), *l = getenv("CLX_BUBBLE_LINES"),
               *h = getenv("CLX_BUBBLE_HEIGHT");
    int wv = w ? atoi(w) : 0, lv = l ? atoi(l) : 0, wide = g_clx.bubble_wide;
    float hv = h ? (float)atof(h) : 0.0f;

    s_bubble_wrap = wide ? 40 : 20;
    s_bubble_lines = wide ? 6 : 4;
    s_bubble_height = wide ? 240.0f : MEMF(0x00649428u);   /* 160.0, the title's own */
    if (wv >= 1 && wv <= 255) s_bubble_wrap = wv;
    if (lv >= 1 && lv <= 127) s_bubble_lines = lv;
    if (hv > 0.0f && hv <= 2000.0f) s_bubble_height = hv;
    fprintf(stderr, "[bubble] subtitle box: wrap %d, lines %d, height %g (bubble=%s)\n",
            (int)s_bubble_wrap, (int)s_bubble_lines, (double)s_bubble_height,
            wide ? "wide" : "classic");
    s_bubble_ready = wide;
}

int32_t clx_bubble_wrap(void)
{
    if (s_bubble_ready != g_clx.bubble_wide) bubble_load();
    return s_bubble_wrap;
}

int32_t clx_bubble_line_limit(void)
{
    if (s_bubble_ready != g_clx.bubble_wide) bubble_load();
    return s_bubble_lines;
}

float clx_bubble_height(void)
{
    if (s_bubble_ready != g_clx.bubble_wide) bubble_load();
    return s_bubble_height;
}

/* ── Vignette off (vignette=0 / CLX_VIGNETTE=0) ──
 *
 * In the levels and the pub the game darkens the screen's edges: an overlay
 * object drawn by 0x0015CAF0 (cdecl, the object at [esp+4]; called through
 * the overlay types' tables from 0x00587880 on) as a 320x240 quad at the
 * origin with two layers, a copy of the screen (texture id 0xFFFFFFFE) and
 * the edge mask (0xFE), in a neutral dark grey. Object: +0x2C x, y, w, h
 * (floats), +0x4D..+0x4F colour B G R, +0x68 layer count, +0x6C the layers'
 * texture ids. With the setting off that one overlay is not drawn; a tinted
 * one (the red flash when Conker is hurt) is drawn as ever. The object
 * layout is from giacomozama's live-and-recompiled (GPL-3; facts only, no
 * code taken). Same XBE, same addresses. */
extern void sub_0015CAF0_gen(void);

void sub_0015CAF0(void)
{
    uint32_t o = MEM32(esp + 4);
    uint32_t bgr;

    if (!g_clx.vignette && o && MEM32(o + 0x68) == 2 && MEM32(o + 0x6C) == 0xFFFFFFFEu
        && MEM32(o + 0x70) == 0xFEu && MEMF(o + 0x2C) == 0.0f && MEMF(o + 0x30) == 0.0f
        && MEMF(o + 0x34) == 320.0f && MEMF(o + 0x38) == 240.0f
        && ((bgr = MEM32(o + 0x4C) >> 8) & 0xFF) == ((bgr >> 8) & 0xFF)
        && ((bgr >> 8) & 0xFF) == ((bgr >> 16) & 0xFF)) {
        esp += 4;                               /* ret; the caller pops the argument */
        return;
    }
    sub_0015CAF0_gen();
}

/* ── Distant grass sways too (grass_wind=1 / CLX_GRASS_WIND, default on) ──
 *
 * 0x001A9540 draws one grass patch. A near patch binds the wind vertex
 * program (handle at [0x006B89A4]: the blades bend with time and away from
 * the player); a patch fading out with distance binds a plain transform
 * ([0x006B89DC]) and so stands still. Both read the same vertices and
 * constants, so the fading patches get the wind program for the call: only
 * the handle the fading path reads changes; the fade itself (texture factor,
 * alpha reference) stays. Found by Mabec Games in their live-and-recompiled
 * branch (GPL-3; the handle addresses only, no code taken). */
extern void sub_001A9540_gen(void);
static int s_grass_logged;

void sub_001A9540(void)
{
    uint32_t wind = MEM32(0x006B89A4u), fade = MEM32(0x006B89DCu);

    if (!g_clx.grass_wind || !wind) {
        sub_001A9540_gen();
        return;
    }
    if (!s_grass_logged) {
        s_grass_logged = 1;
        fprintf(stderr, "[grass] distant grass drawn with the wind program (grass_wind=1)\n");
    }
    MEM32(0x006B89DCu) = wind;
    sub_001A9540_gen();
    MEM32(0x006B89DCu) = fade;
}
