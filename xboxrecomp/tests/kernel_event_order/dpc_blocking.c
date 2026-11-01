/* A contended guest critical section must remain a scheduling point in a DPC. */
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
int xbox_cs_waiting_long(unsigned ms);
static uint8_t *memory;
static uint32_t import;
static HANDLE entered, done;
static DWORD WINAPI watchdog(LPVOID unused)
{
    (void)unused;
    if (WaitForSingleObject(done, 5000) != WAIT_OBJECT_0)
    {
        fputs("FAIL: contended critical section retained the guest CPU\n", stderr);
        ExitProcess(90);
    }
    return 0;
}
static DWORD WINAPI dpc(LPVOID unused)
{
    (void)unused;
    xbox_KfRaiseIrql(DISPATCH_LEVEL);
    xbox_gil_isr_begin();
    g_esp = 0x40000;
    uint32_t *stack = (uint32_t *)(memory + g_esp);
    stack[0] = 0xBEEF0001;
    stack[1] = 0x32000;
    guest_fn enter = recomp_lookup_kernel(import);
    SetEvent(entered);
    enter();
    if (g_esp != 0x40008)
        ExitProcess(20);
    xbox_RtlLeaveCriticalSection((PRTL_CRITICAL_SECTION)(memory + 0x32000));
    xbox_gil_isr_end();
    xbox_KfLowerIrql(PASSIVE_LEVEL);
    return 0;
}
int main(void)
{
    memory = VirtualAlloc(NULL, 16 * 1024 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!memory)
        return 10;
    g_xbox_mem_offset = (ptrdiff_t)memory;
    uint32_t *thunk = (uint32_t *)(memory + 0x10000);
    *thunk = 0x80000000u | 277;
    xbox_kernel_set_thunk_address(0x10000, 1);
    xbox_kernel_bridge_init();
    import = *thunk;
    PRTL_CRITICAL_SECTION lock = (PRTL_CRITICAL_SECTION)(memory + 0x32000);
    xbox_RtlInitializeCriticalSection(lock);
    xbox_RtlEnterCriticalSection(lock);
    entered = CreateEventA(NULL, TRUE, FALSE, NULL);
    done = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE guard = CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    HANDLE worker = CreateThread(NULL, 0, dpc, NULL, 0, NULL);
    if (!entered || !done || !guard || !worker)
        return 11;
    if (WaitForSingleObject(entered, 4000) != WAIT_OBJECT_0)
        return 12;
    DWORD started = GetTickCount();
    while (!xbox_cs_waiting_long(0))
    {
        if (GetTickCount() - started > 3000)
            return 15;
        Sleep(1);
    }
    xbox_gil_enter();
    xbox_RtlLeaveCriticalSection(lock);
    xbox_gil_leave();
    if (WaitForSingleObject(worker, 4000) != WAIT_OBJECT_0)
        return 13;
    DWORD code = 1;
    GetExitCodeThread(worker, &code);
    if (code)
        return 14;
    SetEvent(done);
    WaitForSingleObject(guard, 1000);
    CloseHandle(worker);
    CloseHandle(guard);
    CloseHandle(entered);
    CloseHandle(done);
    VirtualFree(memory, 0, MEM_RELEASE);
    puts("PASS: contended DPC critical section releases guest CPU and dispatch lock");
    return 0;
}
