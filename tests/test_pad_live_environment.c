/* A deterministic environment fixture: setenv may invalidate getenv storage. */
#include "../xboxrecomp/src/platform/mingw_env.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static FILE *live_file;
static char script_value[256];
static int script_present;
static char *fixture_getenv(const char *name)
{
    if (!strcmp(name, "RECOMP_PAD_SCRIPT"))
        return script_present ? script_value : NULL;
    if (!strcmp(name, "RECOMP_PAD_LIVE"))
        return "live-fixture";
    return NULL;
}
static int fixture_setenv(const char *name, const char *value, int overwrite)
{
    assert(!strcmp(name, "RECOMP_PAD_SCRIPT") && overwrite);
    assert(strlen(value) < sizeof script_value);
    /* Reuse the entry's storage, invalidating any saved getenv pointer. */
    memmove(script_value, value, strlen(value) + 1);
    script_present = 1;
    return 0;
}
static int fixture_unsetenv(const char *name)
{
    assert(!strcmp(name, "RECOMP_PAD_SCRIPT"));
    script_present = 0;
    script_value[0] = 0;
    return 0;
}
static FILE *fixture_fopen(const char *path, const char *mode)
{
    assert(!strcmp(path, "live-fixture") && !strcmp(mode, "r"));
    return live_file;
}
#define getenv fixture_getenv
#define setenv fixture_setenv
#define unsetenv fixture_unsetenv
#define fopen fixture_fopen
#include "../xboxrecomp/src/usb/usb_gamepad.c"
#undef getenv
#undef setenv
#undef unsetenv
#undef fopen
DWORD xbox_InputGetState(DWORD dev, XBOX_INPUT_STATE *state)
{
    (void)dev;
    memset(state, 0, sizeof *state);
    return 0;
}
DWORD xbox_InputSetState(DWORD dev, const XBOX_VIBRATION *value)
{
    (void)dev;
    (void)value;
    return 0;
}
int xbox_FramebufferKeyDown(int key)
{
    (void)key;
    return 0;
}
void recomp_lat_input(void)
{
}
static void append_step(const char *step)
{
    long position = ftell(live_file);
    assert(position >= 0 && fseek(live_file, 0, SEEK_END) == 0);
    assert(fputs(step, live_file) >= 0 && fflush(live_file) == 0);
    assert(fseek(live_file, position, SEEK_SET) == 0);
}
int main(void)
{
    const char *original = "0:x:5000";
    uint8_t report[21];
    live_file = tmpfile();
    assert(live_file);
    strcpy(script_value, original);
    script_present = 1;
    append_step("a:220\n");
    report[20] = 0x5a;
    assert(usb_gamepad_report(0, report, 20) == 20 && report[20] == 0x5a);
    if (!script_present || strcmp(script_value, original))
    {
        fputs("FAIL: live input changed the saved script environment\n", stderr);
        return 1;
    }
    script_present = 0;
    append_step("b:220\n");
    assert(usb_gamepad_report(0, report, 20) == 20);
    assert(!script_present && report[20] == 0x5a);
    fclose(live_file);
    puts("PASS: live input restores the original script value and its absent state");
    return 0;
}
