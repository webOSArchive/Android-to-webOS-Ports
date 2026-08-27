/**
 * apkenv - accelerometer via the PDK's own sensor API (webOS)
 *
 * The SDL implementation next door is a dead end on the TouchPad: webOS 3.0.5
 * has no joydev in the kernel, so SDL_INIT_JOYSTICK finds nothing and
 * SDL_JoystickOpen(0) returns NULL - see webos://knowledge/game-controllers.
 * PDL exposes the sensors directly instead (PDL_Sensors.h):
 *
 *     PDL_SensorExists(PDL_SENSOR_ACCELEROMETER)
 *     PDL_EnableSensor(PDL_SENSOR_ACCELEROMETER, PDL_TRUE)
 *     PDL_PollSensor(PDL_SENSOR_ACCELEROMETER, &event)
 *
 * PDL reports acceleration in **g** in the device's natural (landscape) frame.
 * apkenv's contract with the modules is Android's: **m/s2**, natural frame,
 * +Z out of the screen, flat face-up ~ (0, 0, +9.81). So values are scaled by
 * g here and the axis convention can be corrected at runtime without a rebuild:
 *
 *     APKENV_ACCEL_MAP="x,y,z"   pick/permute/negate axes, e.g. "y,-x,z"
 *     APKENV_ACCEL_DEBUG=1       log the raw PDL vector and what we hand on
 *
 * The mapping knob exists because the sign/axis convention of a 2011 tablet's
 * sensor is not documented anywhere we can check - it is settled by tilting the
 * device, which takes one launch with APKENV_ACCEL_DEBUG=1.
 */
#ifndef APKENV_PDL_ACCELEROMETER_IMPL_H
#define APKENV_PDL_ACCELEROMETER_IMPL_H

#include <PDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../../accelerometer/accelerometer.h"

#define PDL_ACCEL_G 9.80665f

struct PDLAccelerometer {
    /* must match struct Accelerometer */
    int (*init)(struct PDLAccelerometer *accelerometer);
    int (*get)(struct PDLAccelerometer *accelerometer, float *x, float *y, float *z);

    int ok;
    int debug;
    float last[3];
    /* axis permutation + sign, from APKENV_ACCEL_MAP (default identity) */
    int src[3];
    float sign[3];
};

static void
pdl_accelerometer_parse_map(struct PDLAccelerometer *a)
{
    const char *spec = getenv("APKENV_ACCEL_MAP");
    int i = 0;
    const char *p;

    a->src[0] = 0; a->src[1] = 1; a->src[2] = 2;
    a->sign[0] = a->sign[1] = a->sign[2] = 1.0f;
    if (spec == NULL || spec[0] == 0)
        return;

    for (p = spec; *p && i < 3; p++) {
        float s = 1.0f;
        if (*p == ',' || *p == ' ') continue;
        if (*p == '-') { s = -1.0f; p++; }
        else if (*p == '+') { p++; }
        if (*p == 'x' || *p == 'X') a->src[i] = 0;
        else if (*p == 'y' || *p == 'Y') a->src[i] = 1;
        else if (*p == 'z' || *p == 'Z') a->src[i] = 2;
        else continue;
        a->sign[i] = s;
        i++;
    }
    fprintf(stderr, "[ACCEL] map=%s -> out = (%c%c, %c%c, %c%c)\n", spec,
            a->sign[0] < 0 ? '-' : '+', 'x' + a->src[0],
            a->sign[1] < 0 ? '-' : '+', 'x' + a->src[1],
            a->sign[2] < 0 ? '-' : '+', 'x' + a->src[2]);
}

static int
pdl_accelerometer_init(struct PDLAccelerometer *a)
{
    a->debug = (getenv("APKENV_ACCEL_DEBUG") != NULL);
    pdl_accelerometer_parse_map(a);

    if (!PDL_SensorExists(PDL_SENSOR_ACCELEROMETER)) {
        fprintf(stderr, "[ACCEL] PDL reports no accelerometer on this device\n");
        a->ok = 0;
        return 0;
    }
    if (PDL_EnableSensor(PDL_SENSOR_ACCELEROMETER, PDL_TRUE) != PDL_NOERROR) {
        fprintf(stderr, "[ACCEL] PDL_EnableSensor(ACCELEROMETER) failed: %s\n",
                PDL_GetError());
        a->ok = 0;
        return 0;
    }
    a->ok = 1;
    fprintf(stderr, "[ACCEL] PDL accelerometer enabled\n");
    return 1;
}

static int
pdl_accelerometer_get(struct PDLAccelerometer *a, float *x, float *y, float *z)
{
    PDL_SensorEvent ev;
    float raw[3];
    int got = 0;
    int i;

    if (!a->ok)
        return 0;

    /* Drain to the newest sample: PDL queues events and a stale one would make
     * the tilt lag behind the device. */
    while (PDL_PollSensor(PDL_SENSOR_ACCELEROMETER, &ev) == PDL_NOERROR &&
           ev.type == PDL_SENSOR_ACCELEROMETER) {
        a->last[0] = ev.accelerometer.x;
        a->last[1] = ev.accelerometer.y;
        a->last[2] = ev.accelerometer.z;
        got = 1;
    }

    for (i = 0; i < 3; i++)
        raw[i] = a->last[i];

    if (a->debug) {
        static int n;
        if (n++ % 60 == 0)
            fprintf(stderr, "[ACCEL] raw g=(% .3f,% .3f,% .3f) |g|=%.3f%s\n",
                    raw[0], raw[1], raw[2],
                    (double)sqrtf(raw[0]*raw[0] + raw[1]*raw[1] + raw[2]*raw[2]),
                    got ? "" : " (no new sample)");
    }

    /* PDL g -> Android m/s2, with the configured axis permutation. */
    if (x) *x = a->sign[0] * raw[a->src[0]] * PDL_ACCEL_G;
    if (y) *y = a->sign[1] * raw[a->src[1]] * PDL_ACCEL_G;
    if (z) *z = a->sign[2] * raw[a->src[2]] * PDL_ACCEL_G;

    return 1;
}

static struct PDLAccelerometer
g_pdl_accelerometer = {
    pdl_accelerometer_init,
    pdl_accelerometer_get,
    0, 0, { 0.0f, 0.0f, 0.0f },
    { 0, 1, 2 },
    { 1.0f, 1.0f, 1.0f },
};

static struct Accelerometer *
pdl_accelerometer = (struct Accelerometer *)(&g_pdl_accelerometer);

#endif /* APKENV_PDL_ACCELEROMETER_IMPL_H */
