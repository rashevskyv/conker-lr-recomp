/* In-game settings menu: BACK (Switch: -, PC: Esc) opens and closes
 * it. While it is open the title gets no input (the game keeps running:
 * cutscenes go on); up/down pick a line, left/right or A change it, B or
 * BACK close it, and closing saves clx_options.txt. Drawn through the
 * renderers' overlay (xboxrecomp platform/overlay.h). The title never sees
 * BACK: it only opened the Xbox Live sign-up (main.c, RECOMP_PAD_MASK).
 * On a PC it also picks pad 1 and remaps the pad and the keyboard; on every
 * platform its last line quits the game. */
#include "clx_settings.h"
#include "platform/overlay.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>

#if CLX_PC_INPUT
#include "input/sdl_keys.h"
#include <SDL_keyboard.h>
#endif

#if defined(NFSU2_VULKAN)
extern volatile int nv2a_vk_fxaa, nv2a_vk_aniso;
#endif
extern volatile int xbox_widescreen;                 /* kernel_xbox.c */
extern volatile float xbox_wide_ratio;
unsigned xbox_movie_state(unsigned *reads);          /* kernel_file.c */
void xa2_set_master_volume(float v);                 /* apu_xaudio2.c */
void clx_scale_apply(void);                          /* switch_nx.c / here */

enum { RESTART = 1, VK_ONLY = 2, SWITCH_ONLY = 4, GL_RESTART = 8 };
enum { VALUE, PAD, REMAP_PAD, REMAP_KEYS, RESET, EXIT };

typedef struct {
    const char *label;
    volatile int *val;
    int n;
    int v[8];
    const char *name[8];         /* NULL: the number itself (with % for scales) */
    int flags;
    int kind;
} Item;

static const Item k_items[] = {
#if CLX_PC_INPUT
    { "CONTROLLER", NULL, 0, { 0 }, { 0 }, 0, PAD },
#endif
    { "ASPECT", &g_clx.aspect, 3, { 0, 1, 2 }, { "4:3", "16:9", "21:9" }, 0 },
    { "FRAME RATE", &g_clx.fps60, 2, { 0, 1 }, { "30", "60" }, RESTART },
    { "CAMERA", &g_clx.cam_free, 2, { 0, 1 }, { "CLASSIC", "FREE" }, 0 },
    { "INVERT CAMERA UP/DOWN", &g_clx.cam_invert, 2, { 0, 1 }, { "OFF", "ON" }, 0 },
    { "AIM SPEED", &g_clx.aim_speed, 7, { 50, 75, 100, 125, 150, 200, 300 }, { 0 }, 0 },
#if defined(__SWITCH__)
    { "GYRO AIM", &g_clx.gyro_aim, 2, { 0, 1 }, { "OFF", "ON" }, 0 },
    { "GYRO SENSITIVITY", &g_clx.gyro_sens, 6, { 50, 75, 100, 150, 200, 300 }, { 0 }, 0 },
#endif
    { "SUBTITLE BOX", &g_clx.bubble_wide, 2, { 1, 0 }, { "WIDE", "CLASSIC" }, 0 },
    { "VIGNETTE", &g_clx.vignette, 2, { 1, 0 }, { "ON", "OFF" }, 0 },
    { "DISTANT GRASS WIND", &g_clx.grass_wind, 2, { 1, 0 }, { "ON", "OFF" }, 0 },
    { "LANGUAGE", &g_clx.language, 6, { 1, 4, 3, 5, 6, 2 },
      { "ENGLISH", "FRENCH", "GERMAN", "SPANISH", "ITALIAN", "JAPANESE" }, RESTART },
    { "FPS COUNTER", &g_clx.fps_osd, 2, { 0, 1 }, { "OFF", "ON" }, 0 },
    { "BUTTON ICONS", &g_clx.glyphs, 3, { 1, 0, 2 }, { "SWITCH", "XBOX", "PLAYSTATION" }, RESTART },
    { "VOLUME", &g_clx.volume, 6, { 0, 20, 40, 60, 80, 100 }, { 0 }, 0 },
#if defined(__SWITCH__)
    { "SCALE HANDHELD", &g_clx.scale_handheld, 5, { 100, 125, 150, 175, 200 }, { 0 }, GL_RESTART },
    { "SCALE DOCKED", &g_clx.scale_docked, 6, { 100, 150, 200, 225, 250, 300 }, { 0 }, GL_RESTART },
#else
    { "RENDER SCALE", &g_clx.scale_handheld, 6, { 100, 150, 200, 250, 300, 400 }, { 0 }, GL_RESTART },
#endif
#if defined(NFSU2_VULKAN)
    { "ANISOTROPIC FILTER", &g_clx.aniso, 5, { 1, 2, 4, 8, 16 }, { 0 }, 0 },
    { "FXAA", &g_clx.fxaa, 2, { 0, 1 }, { "OFF", "ON" }, 0 },
#endif
#if CLX_PC_INPUT
    { "REMAP CONTROLLER", NULL, 0, { 0 }, { 0 }, 0, REMAP_PAD },
    { "REMAP KEYBOARD", NULL, 0, { 0 }, { 0 }, 0, REMAP_KEYS },
    { "RESET CONTROLS", NULL, 0, { 0 }, { 0 }, 0, RESET },
#endif
    { "EXIT GAME", NULL, 0, { 0 }, { 0 }, 0, EXIT },
};
#define NITEMS ((int)(sizeof k_items / sizeof k_items[0]))
#define VIS 11                   /* lines shown at once; the list scrolls */

static atomic_int s_open;
static int s_sel, s_top, s_restart_note, s_armed, s_done;

int clx_menu_is_open(void) { return s_open; }

#if CLX_PC_INPUT
/* The remap wizard: one control after the other. The pad walks the first
 * 15 (BACK stays the menu button), the keyboard all 23 (Esc stays). */
static const char *const k_ctl[][2] = {
    { "a", "A" }, { "b", "B" }, { "x", "X" }, { "y", "Y" },
    { "white", "WHITE" }, { "black", "BLACK" },
    { "lt", "LEFT TRIGGER" }, { "rt", "RIGHT TRIGGER" }, { "start", "START" },
    { "lthumb", "LEFT STICK CLICK" }, { "rthumb", "RIGHT STICK CLICK" },
    { "up", "D-PAD UP" }, { "down", "D-PAD DOWN" }, { "left", "D-PAD LEFT" },
    { "right", "D-PAD RIGHT" },
    { "ls_up", "MOVE UP" }, { "ls_down", "MOVE DOWN" }, { "ls_left", "MOVE LEFT" },
    { "ls_right", "MOVE RIGHT" }, { "rs_up", "CAMERA UP" }, { "rs_down", "CAMERA DOWN" },
    { "rs_left", "CAMERA LEFT" }, { "rs_right", "CAMERA RIGHT" },
};
static int s_wiz, s_wiz_i;       /* REMAP_PAD / REMAP_KEYS, control index */

static int wiz_count(void)
{
    return s_wiz == REMAP_PAD ? 15 : 23;
}

/* A newly pressed pad input or key, by SDL name. */
static int wiz_poll(char *out, int n)
{
    int nk;
    const uint8_t *ks;
    if (s_wiz == REMAP_PAD)
        return xbox_pad_poll_new(out, n);
    ks = SDL_GetKeyboardState(&nk);
    return ks && xbox_keys_poll_new(ks, nk, out, n);
}
#endif

static unsigned now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static int restart_needed(const Item *it)
{
#if defined(NFSU2_VULKAN)
    return (it->flags & RESTART) != 0;
#else
    return (it->flags & (RESTART | GL_RESTART)) != 0;
#endif
}

/* Make a changed value take effect where the game and renderer allow it. */
static void apply(const Item *it)
{
    if (it->val == &g_clx.aspect) {
        if (g_clx.aspect)
            xbox_wide_ratio = clx_aspect_ratio(g_clx.aspect);
        xbox_widescreen = g_clx.aspect != 0;
    }
    else if (it->val == &g_clx.fps_osd)
        ovl_fps_osd = g_clx.fps_osd;
    else if (it->val == &g_clx.volume)
        xa2_set_master_volume(g_clx.volume / 100.0f);
    else if (it->val == &g_clx.scale_handheld || it->val == &g_clx.scale_docked)
        clx_scale_apply();
#if defined(NFSU2_VULKAN)
    else if (it->val == &g_clx.aniso)
        nv2a_vk_aniso = g_clx.aniso;
    else if (it->val == &g_clx.fxaa)
        nv2a_vk_fxaa = g_clx.fxaa;
#endif
    if (restart_needed(it))
        s_restart_note = 1;
}

static void step(int dir)
{
    const Item *it = &k_items[s_sel];
    int i, k = 0;
    for (i = 0; i < it->n; i++)
        if (it->v[i] == *it->val)
            k = i;
    k = (k + dir + it->n) % it->n;
    *it->val = it->v[k];
    apply(it);
}

/* clx_settings.h: a prompt line, hidden by clx_menu_hook when its time is
 * up (the hook runs with every pad report). */
static volatile unsigned s_prompt_until;
static volatile int s_prompt_on;

void clx_prompt(const char *text, unsigned ms)
{
    const uint32_t gold = OVL_RGBA(255, 210, 60, 255);
    int w;
    if (s_open)
        return;
    if (!text) {
        if (s_prompt_on)
            ovl_publish(0);
        s_prompt_on = 0;
        return;
    }
    w = ovl_text_width(2, text) + 32;
    ovl_begin();
    ovl_rect((OVL_W - w) / 2, OVL_H - 70, w, 30, OVL_RGBA(14, 16, 24, 225));
    ovl_rect((OVL_W - w) / 2, OVL_H - 70, w, 2, gold);
    ovl_text((OVL_W - w) / 2 + 16, OVL_H - 60, 2, gold, text);
    ovl_publish(1);
    s_prompt_until = now_ms() + ms;
    s_prompt_on = 1;
}

static void close_menu(void)
{
    s_open = 0;
    ovl_publish(0);
    clx_options_save();
    printf("[menu] closed\n");
}

/* The window's close button and Alt+F4 (recomp_quit_hook). */
static void quit_hook(void)
{
    if (s_open)
        clx_options_save();
}

static const char *pad_name(void)
{
#if CLX_PC_INPUT
    return xbox_pad_name();
#else
    return NULL;
#endif
}

/* The text right of a line's label. */
static void value_text(const Item *it, char *val, int len)
{
    const char *p;
    int k;
    val[0] = 0;
    switch (it->kind) {
    case VALUE:
        snprintf(val, (size_t)len, "%d%s", *it->val,
                 it->val == &g_clx.scale_handheld || it->val == &g_clx.scale_docked ||
                 it->val == &g_clx.aim_speed || it->val == &g_clx.gyro_sens ? "%" : "");
        for (k = 0; k < it->n; k++)
            if (it->v[k] == *it->val && it->name[k])
                snprintf(val, (size_t)len, "%s", it->name[k]);
        break;
    case PAD:
        p = pad_name();
        snprintf(val, (size_t)len, "%.18s", p ? p : "NONE");
        break;
    case REMAP_PAD:
        if (!pad_name())
            snprintf(val, (size_t)len, "NO PAD");
        break;
    case RESET:
        if (s_done)
            snprintf(val, (size_t)len, "DONE");
        break;
    case EXIT:
        if (s_armed)
            snprintf(val, (size_t)len, "PRESS A AGAIN");
        break;
    }
}

static void paint(void)
{
    const int sc = 2, lh = 22, w = 470, x0 = (OVL_W - w) / 2;
    const int rows = NITEMS < VIS ? NITEMS : VIS;
    const int h = 46 + rows * lh + 40, y0 = (OVL_H - h) / 2;
    const uint32_t white = OVL_RGBA(235, 235, 235, 255), grey = OVL_RGBA(150, 150, 160, 255);
    const uint32_t gold = OVL_RGBA(255, 210, 60, 255);
    char val[32], shown[40];
    int i;

    ovl_begin();
    ovl_rect(x0, y0, w, h, OVL_RGBA(14, 16, 24, 225));
    ovl_rect(x0, y0, w, 2, gold);
#if CLX_PC_INPUT
    if (s_wiz) {
        const char *t = s_wiz == REMAP_PAD ? "REMAP CONTROLLER" : "REMAP KEYBOARD";
        const char *c = k_ctl[s_wiz_i][1];
        snprintf(shown, sizeof shown, "%d / %d", s_wiz_i + 1, wiz_count());
        ovl_text(x0 + (w - ovl_text_width(3, t)) / 2, y0 + 10, 3, gold, t);
        ovl_text(x0 + (w - ovl_text_width(2, "PRESS THE INPUT FOR")) / 2, y0 + 90, 2, white,
                 "PRESS THE INPUT FOR");
        ovl_text(x0 + (w - ovl_text_width(3, c)) / 2, y0 + 130, 3, gold, c);
        ovl_text(x0 + (w - ovl_text_width(2, shown)) / 2, y0 + 180, 2, grey, shown);
        t = s_wiz == REMAP_PAD ? "BACK: CANCEL (SAVED SO FAR STAYS)" : "ESC: CANCEL (SAVED SO FAR STAYS)";
        ovl_text(x0 + 16, y0 + h - 20, 1, grey, t);
        ovl_publish(1);
        return;
    }
#endif
    if (s_sel < s_top)
        s_top = s_sel;
    if (s_sel >= s_top + rows)
        s_top = s_sel - rows + 1;
    ovl_text(x0 + (w - ovl_text_width(3, "SETTINGS")) / 2, y0 + 10, 3, gold, "SETTINGS");
    if (s_top > 0)
        ovl_text(x0 + w - 28, y0 + 22, 2, grey, "^");
    if (s_top + rows < NITEMS)
        ovl_text(x0 + w - 28, y0 + 46 + rows * lh - 6, 2, grey, "v");
    for (i = s_top; i < s_top + rows; i++) {
        const Item *it = &k_items[i];
        int y = y0 + 46 + (i - s_top) * lh;
        uint32_t c = i == s_sel ? gold : white;
        if (i == s_sel)
            ovl_rect(x0 + 6, y - 4, w - 12, lh, OVL_RGBA(60, 64, 96, 255));
        ovl_text(x0 + 16, y, sc, c, it->label);
        value_text(it, val, sizeof val);
        snprintf(shown, sizeof shown, i == s_sel && (it->kind == VALUE || it->kind == PAD)
                 ? "< %s%s >" : "%s%s", val, it->kind == VALUE && restart_needed(it) ? "*" : "");
        ovl_text(x0 + w - 40 - ovl_text_width(sc, shown), y, sc, c, shown);
    }
    ovl_text(x0 + 16, y0 + h - 34, 1, grey, "UP/DOWN CHOOSE   LEFT/RIGHT/A CHANGE   B CLOSE AND SAVE");
    if ((k_items[s_sel].kind == PAD || k_items[s_sel].kind == REMAP_PAD) && !pad_name())
        ovl_text(x0 + 16, y0 + h - 20, 1, gold, "PLUG IN OR PAIR A PAD");
    else
        ovl_text(x0 + 16, y0 + h - 20, 1, s_restart_note ? gold : grey,
                 "* TAKES EFFECT AFTER A RESTART");
    ovl_publish(1);
}

/* A on a line that is an action, not a value. */
static void act(const Item *it)
{
    switch (it->kind) {
#if CLX_PC_INPUT
    case PAD:
        xbox_pad_cycle();
        break;
    case REMAP_PAD:
    case REMAP_KEYS: {
        char tmp[48];
        if (it->kind == REMAP_PAD && !pad_name())
            break;
        s_wiz = it->kind;
        s_wiz_i = 0;
        wiz_poll(tmp, sizeof tmp);          /* forget what is held now (A) */
        printf("[menu] %s\n", it->label);
        break;
    }
    case RESET:
        clx_bind_reset();
        clx_options_save();
        s_done = 1;
        printf("[menu] controls reset\n");
        break;
#endif
    case EXIT:
        if (!s_armed) {
            s_armed = 1;
            break;
        }
        s_open = 0;
        clx_options_save();
        recomp_host_quit("exit from the menu");
        break;
    }
}

void clx_menu_hook(unsigned char *r, unsigned host)
{
    static unsigned prev, rep_at, painted_at;
    int swallow = s_open;
    unsigned b = host | r[2] | (unsigned)r[3] << 8, now = now_ms(), edge;
    int16_t ly = (int16_t)(r[14] | r[15] << 8), lx = (int16_t)(r[12] | r[13] << 8);
    int16_t ry = (int16_t)(r[18] | r[19] << 8), rx = (int16_t)(r[16] | r[17] << 8);
    int dir = 0, sel = s_sel;

    if (!recomp_quit_hook)
        recomp_quit_hook = quit_hook;
    if (s_prompt_on && !s_open && (int)(now - s_prompt_until) >= 0) {
        s_prompt_on = 0;
        ovl_publish(0);
    }
    /* Pseudo-buttons from both sticks (a PC's arrow keys are the right
     * stick), A (analog 0) and B (analog 1). */
    if (ly > 16000 || ry > 16000) b |= 0x01;
    if (ly < -16000 || ry < -16000) b |= 0x02;
    if (lx < -16000 || rx < -16000) b |= 0x04;
    if (lx > 16000 || rx > 16000) b |= 0x08;
    if (r[4] > 0x80) b |= 0x10000;
    if (r[5] > 0x80) b |= 0x20000;
    edge = b & ~prev;
    prev = b;
    r[2] &= (unsigned char)~0x20;           /* BACK belongs to the menu */

    /* Movies (XMV) end on A or START, the title's own skip; a stray press
     * ended the boot movie. A movie plays while its file was read in the
     * last 6 s (it streams about 1 MB every 2-3 s; Conker never closes it)
     * and it was not skipped. The first press only asks (held back until
     * released); the next one within 3 s goes through. */
    if (!s_open) {
        static unsigned asked_at, id_seen, reads_seen, read_at, skipped_id;
        static int asked, holding;
        unsigned reads, id = xbox_movie_state(&reads);
        if (id != id_seen || reads != reads_seen) {
            id_seen = id;
            reads_seen = reads;
            read_at = now;
        }
        if (id && id != skipped_id && (int)(now - read_at) < 6000 && (edge & (0x10 | 0x10000))) {
            if (asked && (int)(now - asked_at) < 3000) {
                asked = 0;
                skipped_id = id;
                clx_prompt(NULL, 0);
            } else {
                asked = 1;
                asked_at = now;
                holding = 1;
                clx_prompt("SKIP THIS MOVIE?  PRESS AGAIN", 3000);
                printf("[skip] movie: asked (press again to skip)\n");
            }
        }
        if (holding && !(b & (0x10 | 0x10000)))
            holding = 0;
        if (holding) {
            r[2] &= (unsigned char)~0x10;
            r[4] = 0;
        }
    }

#if CLX_PC_INPUT
    if (s_wiz) {
        char in[48], name[24];
        if (edge & 0x20) {                  /* BACK / Esc: stop here */
            s_wiz = 0;
            clx_options_save();
            paint();
        } else if (wiz_poll(in, sizeof in) && strcmp(in, "back") && strcmp(in, "Escape")) {
            snprintf(name, sizeof name, "%s_%s", s_wiz == REMAP_PAD ? "pad" : "key", k_ctl[s_wiz_i][0]);
            if (clx_bind(name, in))
                printf("[menu] %s=%s\n", name, in);
            if (++s_wiz_i == wiz_count()) {
                s_wiz = 0;
                clx_options_save();
            }
            paint();
        }
        memset(r + 2, 0, 18);
        return;
    }
#endif
    if (edge & 0x20) {
        s_open = !s_open;
        if (s_open) {
            s_restart_note = s_armed = s_done = 0;
            s_prompt_on = 0;
            paint();
            painted_at = now;
            printf("[menu] open\n");
        } else {
            close_menu();
        }
    } else if (s_open) {
        const Item *it;
        /* Directions repeat while held: first after 350 ms, then every 120 ms. */
        if (edge & 0x0F) {
            rep_at = now + 350;
        } else if ((b & 0x0F) && (int)(now - rep_at) >= 0) {
            edge |= b & 0x0F;
            rep_at = now + 120;
        }
        if (edge & 0x01) s_sel = (s_sel + NITEMS - 1) % NITEMS;
        if (edge & 0x02) s_sel = (s_sel + 1) % NITEMS;
        if (s_sel != sel)
            s_armed = s_done = 0;
        it = &k_items[s_sel];
        if (edge & 0x04) dir = -1;
        if (edge & (0x08 | 0x10000 | 0x10)) dir = 1;
        if (dir && it->kind == VALUE)
            step(dir);
        else if (dir && (it->kind == PAD || (edge & (0x10000 | 0x10))))
            act(it);
        if (edge & 0x20000) {
            close_menu();
        } else if ((edge & (0x0F | 0x10 | 0x10000)) || (int)(now - painted_at) >= 500) {
            paint();                        /* also twice a second: pads come and go */
            painted_at = now;
        }
    }
    if (swallow || s_open)
        memset(r + 2, 0, 18);               /* the title sees nothing pressed */
}
