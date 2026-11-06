/* Gyro aim (gyro_aim=1; Switch): the pad's rotation turns the aim camera,
 * on top of the right stick.
 *
 * The camera routine 0x002F99A0 keeps the yaw at [cam+4] (degrees, 0..360,
 * lower = right) and the pitch at [cam+0x40] (lower = up, limits in the
 * settings). Its wrapper (recomp_manual.c) adds one frame of rotation to
 * them before the call, so the routine's own wrap, limits and smoothing
 * apply and the stick's curve, dead zone and ramp stay out of the way.
 *
 *   gyro_aim=0 | 1          off by default (menu: GYRO AIM)
 *   gyro_sens=100           % : 100 = the view turns as far as the pad (GYRO SENSITIVITY)
 *   gyro_invert_x=1, gyro_invert_y=1   reverse a direction (file only)
 *   CLX_GYRO_DEADZONE=1     degrees per second ignored (sensor drift)
 *
 * Self-check: gcc -DCLX_GYRO_TEST src/clx_gyro.c -lm && ./a.out
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* One frame. w = angular velocity (degrees per second) and a = acceleration
 * (g; only its direction matters) in the pad's frame: x right, y away from
 * the player, z out of the buttons or the screen, right-handed. g = the
 * caller's smoothed gravity (zero at first). Out: degrees to turn right and
 * up. Turning about the vertical is the yaw however the pad is tilted, and
 * rolling it like a steering wheel turns nothing.
 * ponytail: the vertical is taken as (|gy|, |gz|), so a pad held upside
 * down or on its side yaws wrong; use the signed gravity if that matters. */
static void gyro_map(const float w[3], const float a[3], float dt, float gain,
                     float dead, float g[3], float *right, float *up)
{
    float wy, wz, n, yaw, pitch, mag;
    int i, first = g[0] == 0.0f && g[1] == 0.0f && g[2] == 0.0f;

    for (i = 0; i < 3 && isfinite(a[0] + a[1] + a[2]); i++)
        g[i] = first ? a[i] : g[i] + (a[i] - g[i]) * 0.1f;
    wy = fabsf(g[1]);
    wz = fabsf(g[2]);
    n = sqrtf(wy * wy + wz * wz);
    if (n < 0.3f)
        wy = wz = 0.70710678f;
    else
        wy /= n, wz /= n;
    yaw = w[1] * wy + w[2] * wz;        /* positive = to the left */
    pitch = w[0];                       /* positive = up */
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
 * the gyro has nothing to add. */
int clx_gyro_aim(float dt, float *right, float *up)
{
#if defined(__SWITCH__)
    static float g[3], dead = -1.0f;
    static int lines;
    static float since;
    float w[3], a[3];

    if (!g_clx.gyro_aim || clx_menu_is_open() || !(dt > 0.0f))
        return 0;
    if (dt > 0.1f)
        dt = 0.1f;
    if (dead < 0.0f) {
        const char *e = getenv("CLX_GYRO_DEADZONE");
        dead = e ? (float)atof(e) : 1.0f;
        if (!(dead >= 0.0f && dead <= 20.0f))
            dead = 1.0f;
        fprintf(stderr, "[gyro] aim on: sensitivity %d%%, dead zone %g deg/s%s%s\n",
                g_clx.gyro_sens, (double)dead, g_clx.gyro_invert_x ? ", x reversed" : "",
                g_clx.gyro_invert_y ? ", y reversed" : "");
    }
    if (!xbox_nx_pad_motion(0, w, a))
        return 0;
    gyro_map(w, a, dt, (float)g_clx.gyro_sens / 100.0f, dead, g, right, up);
    if (g_clx.gyro_invert_x) *right = -*right;
    if (g_clx.gyro_invert_y) *up = -*up;
    /* What the sensor says, for the log: 40 lines, one a second while the
     * pad turns faster than 20 degrees per second. */
    since += dt;
    if (lines < 40 && since >= 1.0f && fabsf(w[0]) + fabsf(w[1]) + fabsf(w[2]) > 20.0f) {
        lines++;
        since = 0.0f;
        fprintf(stderr, "[gyro] rate %.0f %.0f %.0f deg/s, accel %.2f %.2f %.2f g:"
                " right %.2f up %.2f deg\n", (double)w[0], (double)w[1], (double)w[2],
                (double)a[0], (double)a[1], (double)a[2], (double)*right, (double)*up);
    }
    return *right != 0.0f || *up != 0.0f;
#else
    (void)dt; (void)right; (void)up;
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
    float g[3], r, u;
    float w[3];
    const float dt = 1.0f / 30.0f;

#define RUN(ACC, X, Y, Z) (g[0] = g[1] = g[2] = 0, w[0] = X, w[1] = Y, w[2] = Z, \
                           gyro_map(w, ACC, dt, 1.0f, 0.0f, g, &r, &u))
    RUN(flat, 0, 0, -90);                       /* on a table, turned clockwise */
    assert(close_to(r, 3) && close_to(u, 0));
    RUN(upright, 0, -90, 0);                    /* screen upright, turned right */
    assert(close_to(r, 3) && close_to(u, 0));
    RUN(tilted, 0, -63.64f, -63.64f);           /* tilted 45 degrees, turned right */
    assert(close_to(r, 3) && close_to(u, 0));
    RUN(tilted, 0, 63.64f, -63.64f);            /* rolled like a steering wheel */
    assert(close_to(r, 0) && close_to(u, 0));
    RUN(tilted, 60, 0, 0);                      /* back of the pad turned up: up */
    assert(close_to(r, 0) && close_to(u, 2));
    g[0] = g[1] = g[2] = 0; w[0] = 0.5f; w[1] = w[2] = 0;
    gyro_map(w, flat, dt, 1.0f, 1.0f, g, &r, &u);   /* inside the dead zone */
    assert(r == 0 && u == 0);
    w[0] = 31;                                  /* 30 over it, twice the gain */
    gyro_map(w, flat, dt, 2.0f, 1.0f, g, &r, &u);
    assert(close_to(u, 2));
    w[0] = NAN;
    gyro_map(w, flat, dt, 1.0f, 1.0f, g, &r, &u);
    assert(r == 0 && u == 0);
    puts("clx_gyro ok");
    return 0;
}
#endif
