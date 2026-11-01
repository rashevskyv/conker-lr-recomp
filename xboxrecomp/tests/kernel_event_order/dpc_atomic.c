/* Synthetic imports and two host threads; no title code or assets. */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef void (*guest_fn)(void);
extern guest_fn recomp_lookup_kernel(uint32_t);
extern ptrdiff_t g_xbox_mem_offset;
extern volatile int g_gil_contended;
void *recomp_lookup(ULONG address)
{
    (void)address;
    abort();
}
void *recomp_lookup_manual(ULONG address)
{
    (void)address;
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
static volatile LONG phase, violation;

static DWORD WINAPI contender(LPVOID unused)
{
    (void)unused;
    xbox_gil_enter();
    if (!InterlockedCompareExchange(&phase, 0, 0))
        InterlockedExchange(&violation, 1);
    xbox_gil_leave();
    return 0;
}

int main(void)
{
    const unsigned ordinals[] = {175, 145, 119, 137};
    uint8_t *memory =
        VirtualAlloc(NULL, 16 * 1024 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!memory)
        return 10;
    g_xbox_mem_offset = (ptrdiff_t)memory;
    uint32_t *imports = (uint32_t *)(memory + 0x10000);
    for (unsigned i = 0; i < 4; ++i)
        imports[i] = 0x80000000u | ordinals[i];
    xbox_kernel_set_thunk_address(0x10000, 4);
    xbox_kernel_bridge_init();
    /* Inline notification event and KDPC, as initialized by the XDK. */
    uint32_t *event = (uint32_t *)(memory + 0x30000);
    event[0] = 0x00040000;
    event[2] = event[3] = 0x30008;
    memory[0x31000] = 0x13;

    for (unsigned i = 0; i < 4; ++i)
    {
        phase = violation = 0;
        xbox_gil_isr_begin();
        g_gil_contended = 0;
        HANDLE thread = CreateThread(NULL, 0, contender, NULL, 0, NULL);
        if (!thread)
            return 11;
        DWORD start = GetTickCount();
        while (!g_gil_contended)
        {
            if (GetTickCount() - start > 5000)
                return 12;
            Sleep(1);
        }
        g_esp = 0x20000;
        uint32_t *stack = (uint32_t *)(memory + g_esp);
        stack[0] = 0xBEEF0001;
        stack[1] = ordinals[i] == 145 ? 0x30000 : ordinals[i] == 175 ? 0x32000 : 0x31000;
        stack[2] = ordinals[i] == 175 ? 16 : 0;
        stack[3] = ordinals[i] == 175 ? 1 : 0;
        guest_fn call = recomp_lookup_kernel(imports[i]);
        if (!call)
            return 13;
        call();
        unsigned expected_stack = ordinals[i] == 137 ? 0x20008 : 0x20010;
        if (g_esp != expected_stack)
            return 14;
        InterlockedExchange(&phase, 1);
        xbox_gil_isr_end();
        if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0)
            return 15;
        CloseHandle(thread);
        if (violation)
        {
            fprintf(stderr, "FAIL: ordinal %u let passive code run inside ISR/DPC\n", ordinals[i]);
            return 1;
        }
    }
    if (event[1] != 1 || memory[0x31002] != 0)
        return 16;
    VirtualFree(memory, 0, MEM_RELEASE);
    puts("PASS: buffer cleanup, event signalling and DPC queue changes preserve ISR/DPC ordering");
    return 0;
}
