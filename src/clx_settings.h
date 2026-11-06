/* Conker port settings: clx_options.txt, read at boot and changed at run
 * time by the in-game menu (clx_menu.c). Consumers read the fields each
 * time, so a menu change takes effect at once where the game allows it. */
#ifndef CLX_SETTINGS_H
#define CLX_SETTINGS_H

/* SDL keyboard and pads: Linux and the Windows GL build. */
#if !defined(__SWITCH__) && (!defined(_WIN32) || defined(CLX_PC_GL))
#define CLX_PC_INPUT 1
#else
#define CLX_PC_INPUT 0
#endif

typedef struct {
    volatile int aspect;         /* 0 = 4:3, 1 = Hor+ 16:9, 2 = Hor+ 21:9 */
    volatile int fps60;          /* 1 = 60 Hz (applied at the next start) */
    volatile int cam_free;       /* 1 = no swing back behind Conker */
    volatile int cam_invert;     /* 1 = right stick's vertical axis flipped */
    volatile int aim_speed;      /* 25..400: % of the title's aim speed (slingshot, weapon aim camera) */
    volatile int bubble_wide;    /* 1 = 40 x 6 subtitle box, 0 = the game's */
    volatile int fps_osd;        /* FPS counter */
    volatile int volume;         /* 0..100 */
    volatile int scale_handheld; /* render scale, % (Switch handheld; PC: the scale) */
    volatile int scale_docked;   /* render scale, % (Switch docked) */
    volatile int aniso;          /* 1..16 (Vulkan) */
    volatile int fxaa;           /* (Vulkan) */
    volatile int glyphs;         /* button icons: 0 Xbox, 1 Switch, 2 PlayStation */
    volatile int rumble_damage;  /* 1 = the pad rumbles when Conker is hurt (clx_rumble.c) */
    volatile int rumble_hit;     /* 1 = a short rumble when Conker hits something (clx_rumble.c) */
    volatile int vignette;       /* 1 = the game's dark screen edges in the levels; 0 = off */
    volatile int grass_wind;     /* 1 = distant (fading) grass sways like the near grass */
    volatile int language;       /* XC_LANGUAGE: 1 English, 2 Japanese, 3 German, 4 French, 5 Spanish, 6 Italian (next start) */
    volatile int gyro_aim;       /* 1 = the pad's motion sensor turns the aim camera (clx_gyro.c; Switch) */
    volatile int gyro_sens;      /* 25..400: % of the pad's own rotation */
    volatile int gyro_invert_x;  /* 1 = gyro left/right reversed (file only) */
    volatile int gyro_invert_y;  /* 1 = gyro up/down reversed (file only) */
    volatile int gyro_roll;      /* -400..400: % of a sideways roll added to the gyro's turn (file only) */
} ClxSettings;

extern ClxSettings g_clx;

/* clx_options.txt, then the CLX_* / RECOMP_* environment (clx_env.txt) over
 * it, into g_clx; also sets the environment the boot code reads. */
void clx_options_load(void);
/* Writes the menu's keys back to clx_options.txt, keeping every other line. */
int clx_options_save(void);
const char *clx_options_path(void);
/* The aspect setting: "4:3" / "16:9" / "21:9", and its width / height. */
const char *clx_aspect_name(int aspect);
float clx_aspect_ratio(int aspect);
/* The language setting's name ("english", "french", ...; RECOMP_LANGUAGE). */
const char *clx_language_name(int language);
/* PC: bind "key_<control>" or "pad_<control>" (sdl_keys.h names) and keep
 * it for the next save; reset: default keys and pad buttons, none saved. */
int clx_bind(const char *name, const char *value);
void clx_bind_reset(void);

/* Button icons in the chosen style (clx_buttons.c): nv2a_tex_hook. */
void clx_buttons_hook(unsigned int va, unsigned int color, unsigned int w, unsigned int h,
                      unsigned int *argb);

/* A one-line prompt at the bottom of the screen for ms milliseconds
 * (clx_menu.c; NULL hides it; nothing while the menu is open). */
void clx_prompt(const char *text, unsigned ms);

/* Gyro aim (clx_gyro.c): degrees to turn the aim right and up in a frame of
 * dt seconds; 0 = nothing to add, 2 = it logged this frame. */
int clx_gyro_aim(float dt, float *right, float *up);

/* The in-game menu (clx_menu.c), called from clx_pad_hook. */
void clx_menu_hook(unsigned char *report, unsigned host_buttons);

/* usb_gamepad_hook (clx_rumble.c): the damage rumble, then the menu where
 * there is one. */
void clx_pad_hook(unsigned char *report, unsigned host_buttons);

#endif
