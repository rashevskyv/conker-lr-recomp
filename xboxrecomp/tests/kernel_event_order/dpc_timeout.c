/* No title code/assets. Simulate a slow translated DPC overlapping a guest wait. */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef void (*guest_fn)(void);
extern guest_fn recomp_lookup_kernel(uint32_t);
extern ptrdiff_t g_xbox_mem_offset;
void *recomp_lookup(ULONG a)
{
    (void)a;
    abort();
}
void *recomp_lookup_manual(ULONG a)
{
    (void)a;
    abort();
}
void xbox_guest_pin(int raised)
{
    (void)raised;
}
void xbox_gil_enter(void);
void xbox_gil_leave(void);
void xbox_gil_isr_begin(void);
void xbox_gil_isr_end(void);
extern volatile int g_gil_contended;
static uint8_t *memory;
static uint32_t imports[3];
static DWORD WINAPI slow_dpc(LPVOID unused)
{
    (void)unused;
    xbox_gil_isr_begin();
    /* The host clock expires while this guest DPC owns the virtual CPU. */
    Sleep(150);
    /* USB completes its buffer cleanup before signalling the waiter. Neither
     * this non-blocking import nor KeSetEvent may let passive code run yet. */
    g_esp = 0x40000;
    uint32_t *cleanup = (uint32_t *)(memory + g_esp);
    cleanup[0] = 0xBEEF0003;
    cleanup[1] = 0x32000;
    cleanup[2] = 16;
    cleanup[3] = 1;
    guest_fn unlock = recomp_lookup_kernel(imports[2]);
    unlock();
    g_esp = 0x40000;
    uint32_t *sp = (uint32_t *)(memory + g_esp);
    sp[0] = 0xBEEF0002;
    sp[1] = 0x30000;
    sp[2] = 0;
    sp[3] = 0;
    guest_fn fn = recomp_lookup_kernel(imports[0]);
    fn();
    xbox_gil_isr_end();
    return 0;
}
static uint32_t wait_event(int64_t timeout)
{
    *(int64_t *)(memory + 0x30100) = timeout;
    g_esp = 0x20000;
    uint32_t *sp = (uint32_t *)(memory + g_esp);
    sp[0] = 0xBEEF0001;
    sp[1] = 0x30000;
    sp[2] = 0;
    sp[3] = 0;
    sp[4] = 0;
    sp[5] = 0x30100;
    guest_fn fn = recomp_lookup_kernel(imports[1]);
    fn();
    if (g_esp != 0x20018)
    {
        fprintf(stderr, "stack cleanup failed\n");
        exit(20);
    }
    return g_eax;
}
int main(void)
{
    memory = VirtualAlloc(NULL, 16 * 1024 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!memory)
        return 10;
    g_xbox_mem_offset = (ptrdiff_t)memory;
    uint32_t *thunks = (uint32_t *)(memory + 0x10000);
    thunks[0] = 0x80000000u | 145;
    thunks[1] = 0x80000000u | 159;
    thunks[2] = 0x80000000u | 175;
    xbox_kernel_set_thunk_address(0x10000, 3);
    xbox_kernel_bridge_init();
    imports[0] = thunks[0];
    imports[1] = thunks[1];
    imports[2] = thunks[2];
    uint32_t *event = (uint32_t *)(memory + 0x30000);
    event[0] = 0x00040001;
    event[1] = 0;
    event[2] = event[3] = 0x30008;
    xbox_gil_enter();
    HANDLE thread = CreateThread(NULL, 0, slow_dpc, NULL, 0, NULL);
    if (!thread)
        return 11;
    DWORD start = GetTickCount();
    while (!g_gil_contended)
    {
        if (GetTickCount() - start > 5000)
            return 12;
        Sleep(1);
    }
    uint32_t result = wait_event(-500000); /* 50 ms */
    xbox_gil_leave();
    if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0)
        return 13;
    CloseHandle(thread);
    if (result != 0)
    {
        fprintf(stderr, "FAIL: host timeout pre-empted DPC completion: %08X\n", result);
        return 1;
    }
    if (event[1])
    {
        fprintf(stderr, "FAIL: synchronization event not consumed\n");
        return 2;
    }
    xbox_gil_enter();
    uint32_t poll = wait_event(0), expired = wait_event(-100000);
    event[1] = 1;
    uint32_t ready = wait_event(-100000);
    xbox_gil_leave();
    if (poll != 0x102 || expired != 0x102 || ready != 0 || event[1])
        return 3;
    VirtualFree(memory, 0, MEM_RELEASE);
    puts("PASS: DPC signal precedes guest timeout, unsignalled timeout and polling preserved");
    return 0;
}
