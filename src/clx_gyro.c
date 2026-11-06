/* Gyro aim (gyro_aim=1; Switch): the pad's rotation turns the aim camera,
 * on top of the right stick.
 *
 * The camera routine 0x002F99A0 keeps the pitch at [cam+0x40] (lower = up,
 * limits in the settings) and leaves the frame's yaw step at [cam+8]
 * (degrees, positive = right). The aim's users (0x002FB960 slingshot, pan
 * and bat; 0x002BAEC0) take the horizontal from that step, not from the
 * yaw angle at [cam+4]. The wrapper (recomp_manual.c) lowers the pitch
 * before the call and adds to the step after it, so the stick's curve,
 * dead zone and ramp stay out of the way.
 *
 * Directions follow the right stick. Left/right: the side of the pad that
 * goes down, or the way the pad turns, is the way the aim goes. Up/down: the
 * far edge up does what stick up does, so it reverses with the stick
 * (camera_invert_y, the game's Invert Aim; the wrapper says which).
 *
 *   gyro_aim=0 | 1          off by default (menu: GYRO AIM)
 *   gyro_sens=100           % : 100 = the view turns as far as the pad (GYRO SENSITIVITY)
 *   gyro_invert_x=1, gyro_invert_y=1   reverse a direction on top of that (GYRO FLIP ...)
 *   gyro_roll=100           % of a flat pad's sideways tilt added to the turn; 0 = turns only (file only)
 *   CLX_GYRO_DEADZONE=1     degrees per second ignored (sensor drift)
 *
 * Self-check: gcc -DCLX_GYRO_TEST src/clx_gyro.c -lm && ./a.out
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* One frame. w = angular velocity (degrees per second) and a = acceleration
 * (g; only its direction matters) in the pad's frame: x right, y to the top
 * of the screen, z out of the screen, right-handed; a points down at rest.
 * g = the caller's smoothed gravity (zero at first). Out: degrees to turn
 * right and up; part[0] = the turn and part[1] = the roll, degrees per
 * second to the right (for the log).
 *
 * The turn is "player space" (Steam Input, JoyShockMapper): its direction
 * is the rotation about the vertical, its size the rotation about the
 * pad's y and z axes together (at most 1.41 x the vertical part). So with
 * the pad tilted back, turning it about the screen's own upright axis and
 * turning it like a wheel about the screen's normal both aim sideways,
 * and a pad held upside down still turns the right way. A pure roll about
 * the line of sight (a wheel with the screen upright, a sideways tilt with
 * the pad flat) is the vertical's blind spot.
 * A pad held flat (screen up) is aimed by tilting it sideways (console log,
 * 0.56.4: rates about y with the acceleration at (0, -0.1, -0.9), and the
 * view went the other way). So within 13 degrees of flat the turn is the
 * plain rotation about the vertical plus roll_k x the roll, and from 37
 * degrees it is the player-space turn alone; between them the two blend.
 * A rotation about the pad's y axis changes its meaning on the way (a tilt
 * when flat, a turn when upright), so near 26 degrees it does nothing.
 * ponytail: the pitch is the pad's own x axis; with the pad rolled far to
 * one side it mixes with the turn. Project x off the gravity if it matters. */
static void gyro_map(const float w[3], const float a[3], float dt, float gain,
                     float roll_k, float dead, float g[3], float *right,
                     float *up, float part[2])
{
    float uy, uz, n, world, roll, local, yaw, lean, pitch, mag;
    int i, first = g[0] == 0.0f && g[1] == 0.0f && g[2] == 0.0f;

    for (i = 0; i < 3 && isfinite(a[0] + a[1] + a[2]); i++)
        g[i] = first ? a[i] : g[i] + (a[i] - g[i]) * 0.1f;
    n = sqrtf(g[1] * g[1] + g[2] * g[2]);
    if (n < 0.3f)                       /* on its side or in free fall: the usual hold */
        uy = uz = 0.70710678f;
    else
        uy = -g[1] / n, uz = -g[2] / n; /* up, in the pad's y-z plane */
    world = w[1] * uy + w[2] * uz;      /* about the vertical, positive = to the left */
    roll = w[2] * uy - w[1] * uz;       /* about the line of sight, positive = left side down */
    local = sqrtf(w[1] * w[1] + w[2] * w[2]);
    yaw = copysignf(fminf(fabsf(world) * 1.41421356f, local), world);
    lean = (37.0f - fabsf(atan2f(uy, uz)) * 57.29578f) / 24.0f;   /* 1 = flat, screen up */
    lean = lean < 0.0f ? 0.0f : lean > 1.0f ? 1.0f : lean;
    yaw += lean * (world + roll * roll_k - yaw);
    pitch = w[0];                       /* positive = up */
    part[0] = -world;
    part[1] = -roll;
    mag = sqrtf(yaw * yaw + pitch * pitch);
    if (!(mag > dead)) {                /* also catches a NaN */
        *right = *up = 0.0f;
        return;
    }
    gain *= (mag - dead) / mag * dt;
    *right = -yaw * gain;
    *up = pitch * gain;
}

#ifndef CLX_GYRO_TEST
#include "clx_settings.h"

int clx_menu_is_open(void);
#if defined(__SWITCH__)
int xbox_nx_pad_motion(unsigned port, float rate[3], float accel[3]);
#endif

/* Degrees to turn the aim right and up in this frame of dt seconds; 0 when
 * the gyro has nothing to add. stick_down = stick up looks down now: the
 * gyro's up/down reverses with it. */
int clx_gyro_aim(float dt, int stick_down, float *right, float *up)
{
#if defined(__SWITCH__)
    static float g[3], dead = -1.0f;
    static int lines;
    static float since;
    float w[3], a[3], part[2];

    if (!g_clx.gyro_aim || clx_menu_is_open() || !(dt > 0.0f))
        return 0;
    if (dt > 0.1f)
        dt = 0.1f;
    if (dead < 0.0f) {
        const char *e = getenv("CLX_GYRO_DEADZONE");
        dead = e ? (float)atof(e) : 1.0f;
        if (!(dead >= 0.0f && dead <= 20.0f))
            dead = 1.0f;
        fprintf(stderr, "[gyro] aim on: sensitivity %d%%, roll %d%%, dead zone %g deg/s%s%s\n",
                g_clx.gyro_sens, g_clx.gyro_roll, (double)dead,
                g_clx.gyro_invert_x ? ", x reversed" : "",
                g_clx.gyro_invert_y ? ", y reversed" : "");
    }
    if (!xbox_nx_pad_motion(0, w, a))
        return 0;
    gyro_map(w, a, dt, (float)g_clx.gyro_sens / 100.0f, (float)g_clx.gyro_roll / 100.0f,
             dead, g, right, up, part);
    if (g_clx.gyro_invert_x) *right = -*right;
    if (!g_clx.gyro_invert_y != !stick_down) *up = -*up;
    if (*right == 0.0f && *up == 0.0f)
        return 0;
    /* What the sensor says, for the log: 60 lines, one a second while the
     * pad turns faster than 20 degrees per second. 2 = the caller adds its
     * own line (what the camera took). */
    since += dt;
    if (lines < 60 && since >= 1.0f && fabsf(w[0]) + fabsf(w[1]) + fabsf(w[2]) > 20.0f) {
        lines++;
        since = 0.0f;
        fprintf(stderr, "[gyro] rate %.0f %.0f %.0f deg/s, accel %.2f %.2f %.2f g:"
                " turn %.0f roll %.0f deg/s, right %.2f up %.2f deg\n",
                (double)w[0], (double)w[1], (double)w[2],
                (double)a[0], (double)a[1], (double)a[2], (double)part[0], (double)part[1],
                (double)*right, (double)*up);
        return 2;
    }
    return 1;
#else
    (void)dt; (void)stick_down; (void)right; (void)up;
    (void)gyro_map;
    return 0;
#endif
}

#else
#include <assert.h>

static int close_to(float x, float y) { return fabsf(x - y) < 0.01f; }

int main(void)
{
    static const float flat[3] = { 0, 0, -1 }, upright[3] = { 0, -1, 0 },
                       tilted[3] = { 0, -0.70710678f, -0.70710678f };
    static const float down[3] = { 0, 0, 1 };   /* flat, screen to the floor */
    float g[3], r, u, part[2];
    float w[3];
    const float dt = 1.0f / 30.0f;

#define RUNK(ACC, K, X, Y, Z) (g[0] = g[1] = g[2] = 0, w[0] = X, w[1] = Y, w[2] = Z, \
                               gyro_map(w, ACC, dt, 1.0f, K, 0.0f, g, &r, &u, part))
#define RUN(ACC, X, Y, Z) RUNK(ACC, 0.0f, X, Y, Z)
    RUN(flat, 0, 0, -90);                       /* on a table, turned clockwise */
    assert(close_to(r, 3) && close_to(u, 0));
    RUN(upright, 0, -90, 0);                    /* screen upright, turned right */
    assert(close_to(r, 3) && close_to(u, 0));
    RUN(tilted, 0, -63.64f, -63.64f);           /* tilted 45 degrees, turned right */
    assert(close_to(r, 3) && close_to(u, 0));
    RUN(tilted, 0, -90, 0);                     /* tilted, turned about the screen's upright axis */
    assert(close_to(r, 3) && close_to(u, 0));
    RUN(tilted, 0, 0, -90);                     /* tilted, turned like a wheel about the screen's normal */
    assert(close_to(r, 3) && close_to(u, 0));
    RUN(down, 0, 0, -90);                       /* upside down: the same turn aims the other way */
    assert(close_to(r, -3) && close_to(u, 0));
    RUNK(tilted, 1.0f, 0, 63.64f, -63.64f);     /* a pure roll, right side down: held up, nothing */
    assert(close_to(r, 0) && close_to(u, 0) && close_to(part[1], 90));
    RUNK(flat, 1.0f, 0, 90, 0);                 /* flat, right side down: right */
    assert(close_to(r, 3) && close_to(u, 0));
    RUNK(flat, 1.0f, 0, 0, 90);                 /* flat, turned to the left: left */
    assert(close_to(r, -3) && close_to(u, 0));
    RUNK(flat, 0.0f, 0, 90, 0);                 /* gyro_roll=0: turns only */
    assert(close_to(r, 0) && close_to(u, 0));
    RUNK(flat, -1.0f, 0, 90, 0);                /* gyro_roll=-100: the tilt the other way */
    assert(close_to(r, -3) && close_to(u, 0));
    {   /* the console's 0.56.4 log: flat, left side down went right, right side down went left */
        static const float a1[3] = { -0.09f, -0.10f, -0.90f }, a2[3] = { 0.31f, -0.17f, -0.81f },
                           a3[3] = { 0.05f, -0.02f, -0.96f }, a4[3] = { 0.03f, -0.70f, -0.47f };
        RUNK(a1, 1.0f, -10, -74, -19);
        assert(r < -1.0f);
        RUNK(a2, 1.0f, 16, 57, 15);
        assert(r > 0.8f);
        RUNK(a3, 1.0f, 68, 10, 1);              /* far edge up: up, as before */
        assert(u > 2.0f);
        RUNK(a4, 1.0f, 3, -42, -4);             /* its 0.56.3 log: held up 56 degrees, turned right */
        assert(r > 1.3f);
    }
    RUN(tilted, 60, 0, 0);                      /* back of the pad turned up: up */
    assert(close_to(r, 0) && close_to(u, 2));
    g[0] = g[1] = g[2] = 0; w[0] = 0.5f; w[1] = w[2] = 0;
    gyro_map(w, flat, dt, 1.0f, 0.0f, 1.0f, g, &r, &u, part);   /* inside the dead zone */
    assert(r == 0 && u == 0);
    w[0] = 31;                                  /* 30 over it, twice the gain */
    gyro_map(w, flat, dt, 2.0f, 0.0f, 1.0f, g, &r, &u, part);
    assert(close_to(u, 2));
    w[0] = NAN;
    gyro_map(w, flat, dt, 1.0f, 0.0f, 1.0f, g, &r, &u, part);
    assert(r == 0 && u == 0);
    puts("clx_gyro ok");
    return 0;
}
#endif
