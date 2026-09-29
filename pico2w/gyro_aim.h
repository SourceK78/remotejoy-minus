#ifndef RJM_GYRO_AIM_H
#define RJM_GYRO_AIM_H

#include <stdbool.h>
#include <stdint.h>

/* Sensor frame of the controller that produced a motion sample, in the
 * PlayStation convention: X = right, Y = up, Z = toward the player,
 * right-handed. */
enum RjmGyroKind {
    RJM_GYRO_KIND_NONE = 0,
    RJM_GYRO_KIND_PLAYSTATION,   /* DualShock 4, DualSense */
};

struct RjmGyroSample {
    enum RjmGyroKind kind;
    int16_t gyro[3];         /* raw driver values */
    int16_t accel[3];        /* raw driver values */
    uint16_t gyro_range_dps; /* raw +/-32768 corresponds to this rate */
    uint16_t accel_range_mg; /* raw +/-32768 corresponds to this acceleration */
    uint32_t time_us;        /* receive time of the report */
    bool recenter;           /* recenter button held */
    uint8_t span_deg;        /* rotation that sweeps the screen width */
};

/* Screen-width rotation range accepted from the configuration portal. */
#define RJM_GYRO_SPAN_MIN_DEG     10
#define RJM_GYRO_SPAN_MAX_DEG     90
#define RJM_GYRO_SPAN_DEFAULT_DEG 40

/* Forget position, gravity and bias for a player (disconnect, mode change). */
void rjm_gyro_aim_reset(int player);

/* Integrate one motion sample. Returns true and writes the aim position
 * (0..65535 on each axis, 0 = left/top, clamped to the screen) when the aim
 * is valid. *offscreen is set while the controller points past an edge. */
bool rjm_gyro_aim_update(int player, const struct RjmGyroSample *sample,
                         uint16_t *x, uint16_t *y, bool *offscreen);

#endif
