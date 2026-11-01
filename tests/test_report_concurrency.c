/* Exercise real USB report code concurrently as OHCI/native readers do. */
#include "../xboxrecomp/src/usb/usb_gamepad.c"
#include <assert.h>
static atomic_uint in_hook, samples;
static unsigned hooks;
DWORD xbox_InputGetState(DWORD dev, XBOX_INPUT_STATE *state)
{
    memset(state, 0, sizeof *state);
    state->Gamepad.wButtons = 0x10;
    state->Gamepad.bAnalogButtons[0] = 255;
    state->Gamepad.sThumbRX = -1234;
    atomic_fetch_add(&samples, 1);
    return 0;
}
DWORD xbox_InputSetState(DWORD dev, const XBOX_VIBRATION *v)
{
    return 0;
}
int xbox_FramebufferKeyDown(int vk)
{
    return 0;
}
void recomp_lat_input(void)
{
}
static void hook(uint8_t *report, unsigned host)
{
    assert(atomic_fetch_add(&in_hook, 1) == 0);
    hooks++;
    SwitchToThread();
    assert(atomic_fetch_sub(&in_hook, 1) == 1);
}
static DWORD WINAPI reader(LPVOID arg)
{
    for (unsigned i = 0; i < 5000; i++)
    {
        uint8_t report[21];
        report[20] = 0x5a;
        assert(usb_gamepad_report(0, report, 20) == 20);
        assert(report[1] == 20 && report[2] == 0x10 && report[4] == 255);
        assert((int16_t)(report[16] | report[17] << 8) == -1234 && report[20] == 0x5a);
    }
    return 0;
}
int main(void)
{
    HANDLE threads[4];
    _putenv_s("RECOMP_PAD_SCRIPT", "");
    _putenv_s("RECOMP_PAD_LIVE", "");
    usb_gamepad_hook = hook;
    for (int i = 0; i < 4; i++)
        threads[i] = CreateThread(NULL, 0, reader, NULL, 0, NULL);
    WaitForMultipleObjects(4, threads, TRUE, INFINITE);
    for (int i = 0; i < 4; i++)
        CloseHandle(threads[i]);
    assert(hooks == 20000 && atomic_load(&samples) == 20000);
    puts("PASS: 20000 real USB reports across four concurrent readers; hook serialization and "
         "report layout");
}
