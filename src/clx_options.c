/* clx_options.txt: the user's settings, one KEY=VALUE per line.
 *
 *   aspect=4:3 | 16:9 | 21:9   Hor+ 16:9 or 21:9 (wider view; HUD stretched)
 *   fps=30 | 60                60 Hz (experimental; from the next start)
 *   camera=classic | free      free: no automatic swing behind Conker
 *   camera_invert_y=0 | 1      flip the right stick's vertical axis (camera_invert=1 too)
 *   aim_speed=100              % of the game's aim speed: slingshot and weapon aim camera (CLX_AIM_SPEED)
 *   bubble=wide | classic      subtitle box 40 x 6 lines (default) or the game's 20 x 4
 *   bubble_wrap= bubble_lines= bubble_height=   override the box (not in the menu)
 *   fps_counter=0 | 1          FPS digits top-left (RECOMP_FPS_OSD)
 *   volume=0..100              (RECOMP_AUDIO_VOLUME)
 *   scale_handheld=150         render scale % (Switch handheld; on a PC the render scale)
 *   scale_docked=225           render scale % (Switch docked)
 *   aniso=16                   anisotropic filtering 1/2/4/8/16 (Vulkan)
 *   fxaa=1                     FXAA on the presented frame (Vulkan)
 *   button_glyphs=switch|xbox|ps   button icons (default switch on the Switch, xbox on a PC)
 *   rumble_damage=1            the pad rumbles when Conker is hurt (the game only
 *                              rumbles for a few scripted events); 0 = off
 *   rumble_hit=1               a short rumble when Conker's weapon hits an enemy; 0 = off
 *   gyro_aim=0 | 1             Switch: the pad's motion sensor turns the aim (slingshot, weapons)
 *                              on top of the right stick; off by default
 *   gyro_sens=100              % of the pad's own rotation (25-400)
 *   gyro_invert_x=1, gyro_invert_y=1   reverse a gyro direction (up/down goes by the aim inversion by itself)
 *   gyro_roll=0                % of a pure sideways roll added to the gyro's turn (-400..400; not in the menu)
 *   vignette=1                 the game's darkened screen edges in the levels; 0 = off
 *   grass_wind=1               distant grass sways in the wind like the near grass; 0 = the game's
 *   language=english           menus and subtitles: english, french, german, spanish,
 *                              italian, japanese (the disc's text; the voices stay English)
 *   fullscreen=borderless|window   borderless fullscreen on a PC (Alt+Enter/F11 toggles)
 *   mouse=0|1                  native mouse camera (F8 toggles at run time)
 *   key_<control>=<key>        PC keyboard: the key for a pad control, SDL key names
 *                              (key_a=Space, key_rs_up=Up; controls in input/sdl_keys.h)
 *
 * Where: sdmc:/switch/clx/clx_options.txt on the Switch; elsewhere
 * CLX_OPTIONS or ./clx_options.txt. The same variable set in clx_env.txt or
 * the environment wins over the file at boot (CLX_ASPECT, CLX_FPS, ...), except
 * fps_counter and volume: the file wins (clx_env.txt ships RECOMP_FPS_OSD=1). */
#include "clx_settings.h"
#include "kernel/xbox_eeprom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CLX_PC_INPUT
#include "input/sdl_keys.h"

/* Key and pad bindings set from the file or the menu, as file lines
 * ("key_a=Return", "pad_a=b"): written back on save, cleared on reset. */
static char s_bind[64][48];
static int s_nbind;

int clx_bind(const char *name, const char *v)
{
    int i, ok = !strncmp(name, "key_", 4) ? xbox_keys_bind(name + 4, v)
              : !strncmp(name, "pad_", 4) ? xbox_pad_bind(name + 4, v) : 0;
    size_t n = strlen(name);
    if (!ok)
        return 0;
    for (i = 0; i < s_nbind; i++)
        if (!strncmp(s_bind[i], name, n) && s_bind[i][n] == '=')
            break;
    if (i < 64) {
        snprintf(s_bind[i], sizeof s_bind[i], "%s=%s", name, v);
        if (i == s_nbind)
            s_nbind++;
    }
    return 1;
}

void clx_bind_reset(void)
{
    s_nbind = 0;
    xbox_keys_reset();
    xbox_pad_reset();
}
#endif

#if defined(_WIN32) && !defined(setenv)
/* MinGW has no setenv. */
#define setenv(k, v, overwrite) ((overwrite) || !getenv(k) ? _putenv_s((k), (v)) : 0)
#endif

ClxSettings g_clx = {
#if defined(__SWITCH__)
    0, 0, 0, 0, 100, 1, 0, 100, 150, 225, 16, 1, 1, 1, 1, 1, 1, 1, 0, 100 /* Switch button icons */
#else
    0, 0, 0, 0, 100, 1, 0, 100, 100, 225, 16, 1, 0, 1, 1, 1, 1, 1, 0, 100 /* PC: render scale 100%, Xbox icons */
#endif
};

const char *clx_options_path(void)
{
#if defined(__SWITCH__)
    return "sdmc:/switch/clx/clx_options.txt";
#else
    const char *e = getenv("CLX_OPTIONS");
    return e && *e ? e : "clx_options.txt";
#endif
}

static int on(const char *v)
{
    return v && (*v == '1' || !strncmp(v, "on", 2));
}

static const char *const k_aspects[] = { "4:3", "16:9", "21:9" };

/* XC_LANGUAGE values the disc has text for (aid/text/<Name>). */
static const char *const k_languages[] = {
    "", "english", "japanese", "german", "french", "spanish", "italian",
};

const char *clx_language_name(int language)
{
    return k_languages[language >= 1 && language <= 6 ? language : 1];
}

static int aspect_parse(const char *v)
{
    return !strcmp(v, "21:9") ? 2 : !strcmp(v, "16:9") ? 1 : 0;
}

const char *clx_aspect_name(int aspect)
{
    return k_aspects[aspect >= 0 && aspect <= 2 ? aspect : 0];
}

float clx_aspect_ratio(int aspect)
{
    return aspect == 2 ? 21.0f / 9.0f : aspect == 1 ? 16.0f / 9.0f : 4.0f / 3.0f;
}

void clx_options_load(void)
{
    FILE *f = fopen(clx_options_path(), "r");
    char line[128];
    const char *e;

    if (f) {
        while (fgets(line, sizeof line, f)) {
            char *v = strchr(line, '='), *nl;
            if (!v || line[0] == '#')
                continue;
            *v++ = 0;
            if ((nl = strpbrk(v, "\r\n")))
                *nl = 0;
            if (!strcmp(line, "aspect")) g_clx.aspect = aspect_parse(v);
            else if (!strcmp(line, "fps")) g_clx.fps60 = atoi(v) == 60;
            else if (!strcmp(line, "camera")) g_clx.cam_free = !strcmp(v, "free");
            else if (!strcmp(line, "camera_invert_y") || !strcmp(line, "camera_invert"))
                g_clx.cam_invert = on(v);
            else if (!strcmp(line, "aim_speed")) g_clx.aim_speed = atoi(v);
            else if (!strcmp(line, "bubble")) g_clx.bubble_wide = strncmp(v, "classic", 7) != 0;
            else if (!strcmp(line, "bubble_wrap")) setenv("CLX_BUBBLE_WRAP", v, 0);
            else if (!strcmp(line, "bubble_lines")) setenv("CLX_BUBBLE_LINES", v, 0);
            else if (!strcmp(line, "bubble_height")) setenv("CLX_BUBBLE_HEIGHT", v, 0);
            else if (!strcmp(line, "fps_counter")) setenv("RECOMP_FPS_OSD", on(v) ? "1" : "0", 1);
            else if (!strcmp(line, "volume")) setenv("RECOMP_AUDIO_VOLUME", v, 1);
            else if (!strcmp(line, "scale_handheld")) g_clx.scale_handheld = atoi(v);
            else if (!strcmp(line, "scale_docked")) g_clx.scale_docked = atoi(v);
            else if (!strcmp(line, "aniso")) g_clx.aniso = atoi(v);
            else if (!strcmp(line, "fxaa")) g_clx.fxaa = atoi(v) != 0;
            else if (!strcmp(line, "button_glyphs"))
                g_clx.glyphs = !strcmp(v, "xbox") ? 0 : !strcmp(v, "ps") ? 2 : 1;
            else if (!strcmp(line, "rumble_damage")) g_clx.rumble_damage = on(v);
            else if (!strcmp(line, "rumble_hit")) g_clx.rumble_hit = on(v);
            else if (!strcmp(line, "gyro_aim")) g_clx.gyro_aim = on(v);
            else if (!strcmp(line, "gyro_sens")) g_clx.gyro_sens = atoi(v);
            else if (!strcmp(line, "gyro_invert_x")) g_clx.gyro_invert_x = on(v);
            else if (!strcmp(line, "gyro_invert_y")) g_clx.gyro_invert_y = on(v);
            else if (!strcmp(line, "gyro_roll")) g_clx.gyro_roll = atoi(v);
            else if (!strcmp(line, "vignette")) g_clx.vignette = on(v);
            else if (!strcmp(line, "grass_wind")) g_clx.grass_wind = on(v);
            else if (!strcmp(line, "language")) g_clx.language = (int)xbox_eeprom_language(v);
            else if (!strcmp(line, "fullscreen"))
                setenv("CLX_BORDERLESS",
                       (!strcmp(v, "borderless") || !strcmp(v, "fullscreen") || on(v)) ? "1" : "0", 0);
            else if (!strcmp(line, "mouse"))
                setenv("CLX_NATIVE_MOUSE", on(v) ? "1" : "0", 0);
#if CLX_PC_INPUT
            else if ((!strncmp(line, "key_", 4) || !strncmp(line, "pad_", 4)) && !clx_bind(line, v))
                fprintf(stderr, "[options] unknown binding %s=%s\n", line, v);
#endif
        }
        fclose(f);
    }
    /* The environment (clx_env.txt) wins, as before the menu existed. */
    if ((e = getenv("CLX_ASPECT"))) g_clx.aspect = aspect_parse(e);
    if ((e = getenv("CLX_FPS"))) g_clx.fps60 = atoi(e) == 60;
    if ((e = getenv("CLX_CAM_FREE"))) g_clx.cam_free = *e == '1';
    if ((e = getenv("CLX_CAM_INVERT_Y"))) g_clx.cam_invert = *e == '1';
    if ((e = getenv("CLX_AIM_SPEED"))) g_clx.aim_speed = atoi(e);
    if (g_clx.aim_speed < 25 || g_clx.aim_speed > 400) g_clx.aim_speed = 100;
    if ((e = getenv("CLX_GYRO_AIM"))) g_clx.gyro_aim = *e == '1';
    if (g_clx.gyro_sens < 25 || g_clx.gyro_sens > 400) g_clx.gyro_sens = 100;
    if (g_clx.gyro_roll < -400 || g_clx.gyro_roll > 400) g_clx.gyro_roll = 0;
    if ((e = getenv("CLX_BUBBLE"))) g_clx.bubble_wide = strncmp(e, "classic", 7) != 0;
    if ((e = getenv("CLX_RUMBLE_DAMAGE"))) g_clx.rumble_damage = *e == '1';
    if ((e = getenv("CLX_RUMBLE_HIT"))) g_clx.rumble_hit = *e == '1';
    if ((e = getenv("CLX_VIGNETTE"))) g_clx.vignette = *e == '1';
    if ((e = getenv("CLX_GRASS_WIND"))) g_clx.grass_wind = *e == '1';
    if ((e = getenv("RECOMP_LANGUAGE"))) g_clx.language = (int)xbox_eeprom_language(e);
    if (g_clx.language < 1 || g_clx.language > 6) g_clx.language = 1;
    g_clx.fps_osd = on(getenv("RECOMP_FPS_OSD"));
    if ((e = getenv("RECOMP_AUDIO_VOLUME"))) g_clx.volume = atoi(e);
    if (g_clx.volume < 0 || g_clx.volume > 100) g_clx.volume = 100;
    if (g_clx.scale_docked < 50 || g_clx.scale_docked > 400) g_clx.scale_docked = 225;
    if (g_clx.scale_handheld < 50 || g_clx.scale_handheld > 400) g_clx.scale_handheld = 150;
    if (g_clx.aniso < 1) g_clx.aniso = 1;
    if (g_clx.aniso > 16) g_clx.aniso = 16;
    /* What the boot code reads (main.c, the renderers). */
    setenv("CLX_ASPECT", clx_aspect_name(g_clx.aspect), 0);
    setenv("RECOMP_LANGUAGE", clx_language_name(g_clx.language), 1);
    printf("[options] %s: aspect %s, fps %d, camera %s%s, aim speed %d%%, subtitles %s, FPS counter %s,"
           " volume %d, scale handheld %d%% docked %d%%, aniso %dx, FXAA %s, vignette %s, grass wind %s,"
           " language %s\n", clx_options_path(),
           clx_aspect_name(g_clx.aspect), g_clx.fps60 ? 60 : 30,
           g_clx.cam_free ? "free" : "classic", g_clx.cam_invert ? " (inverted)" : "", g_clx.aim_speed,
           g_clx.bubble_wide ? "wide" : "classic", g_clx.fps_osd ? "on" : "off", g_clx.volume,
           g_clx.scale_handheld, g_clx.scale_docked, g_clx.aniso, g_clx.fxaa ? "on" : "off",
           g_clx.vignette ? "on" : "off", g_clx.grass_wind ? "on" : "off",
           clx_language_name(g_clx.language));
}

/* The keys the menu owns; any other line of the file is kept as it was. */
static const char *const k_keys[] = {
    "aspect", "fps", "camera", "camera_invert_y", "camera_invert", "aim_speed", "gyro_aim", "gyro_sens",
    "gyro_invert_x", "gyro_invert_y",
    "bubble", "fps_counter",
    "volume", "scale_handheld", "scale_docked", "aniso", "fxaa", "button_glyphs",
    "vignette", "grass_wind", "language",
};

static int owned(const char *line)
{
    size_t i, n;
    for (i = 0; i < sizeof k_keys / sizeof k_keys[0]; i++) {
        n = strlen(k_keys[i]);
        if (!strncmp(line, k_keys[i], n) && line[n] == '=')
            return 1;
    }
#if CLX_PC_INPUT
    if (!strncmp(line, "key_", 4) || !strncmp(line, "pad_", 4))
        return 1;
#endif
    return 0;
}

int clx_options_save(void)
{
    const char *path = clx_options_path();
    char tmp[512], line[256];
    FILE *in = fopen(path, "r"), *out;

    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (!(out = fopen(tmp, "w"))) {
        if (in) fclose(in);
        fprintf(stderr, "[options] cannot write %s\n", tmp);
        return 0;
    }
    if (in) {
        while (fgets(line, sizeof line, in))
            if (!owned(line))
                fputs(line, out);
        fclose(in);
    }
    fprintf(out, "aspect=%s\nfps=%d\ncamera=%s\ncamera_invert_y=%d\naim_speed=%d\ngyro_aim=%d\n"
            "gyro_sens=%d\ngyro_invert_x=%d\ngyro_invert_y=%d\nbubble=%s\n"
            "fps_counter=%d\nvolume=%d\nscale_handheld=%d\nscale_docked=%d\naniso=%d\nfxaa=%d\n"
            "button_glyphs=%s\nvignette=%d\ngrass_wind=%d\nlanguage=%s\n",
            clx_aspect_name(g_clx.aspect), g_clx.fps60 ? 60 : 30,
            g_clx.cam_free ? "free" : "classic", g_clx.cam_invert ? 1 : 0, g_clx.aim_speed,
            g_clx.gyro_aim ? 1 : 0, g_clx.gyro_sens,
            g_clx.gyro_invert_x ? 1 : 0, g_clx.gyro_invert_y ? 1 : 0,
            g_clx.bubble_wide ? "wide" : "classic", g_clx.fps_osd ? 1 : 0, g_clx.volume,
            g_clx.scale_handheld, g_clx.scale_docked, g_clx.aniso, g_clx.fxaa ? 1 : 0,
            g_clx.glyphs == 0 ? "xbox" : g_clx.glyphs == 2 ? "ps" : "switch",
            g_clx.vignette ? 1 : 0, g_clx.grass_wind ? 1 : 0, clx_language_name(g_clx.language));
#if CLX_PC_INPUT
    {
        int i;
        for (i = 0; i < s_nbind; i++)
            fprintf(out, "%s\n", s_bind[i]);
    }
#endif
    if (fclose(out) != 0) {
        remove(tmp);
        return 0;
    }
    remove(path);                       /* FAT on the Switch: rename does not replace */
    if (rename(tmp, path) != 0) {
        fprintf(stderr, "[options] cannot replace %s\n", path);
        return 0;
    }
    printf("[options] saved %s\n", path);
    return 1;
}
