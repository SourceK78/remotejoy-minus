#include "gyro_aim.h"

#include <math.h>
#include <string.h>

/* Gyro aim for a light gun: the aim position is the integral of the angular
 * rate, so the aim follows where the controller points. There is no dead
 * zone, rate curve or smoothing: earlier variants with those made the aim
 * stutter. Only the sensor zero-rate offset (bias) is corrected, and never
 * in a step. */

/* The vertical sweep follows the 4:3 picture: turning by span_deg moves the
 * aim across the screen width, and by 3/4 of it across the height. */
#define V_SPAN_RATIO             0.75f

/* Bias handling:
 * - Start-up: once the controller has been still for STARTUP_CAL_US, the
 *   mean rate over that stretch becomes the bias.
 * - Running: while still for more than STILL_HOLD_US, the bias follows the
 *   rate with a time constant of roughly 1 / (BIAS_ALPHA * report rate).
 * "Still" = gravity reads 1 g (+/- STILL_G_TOL), the gravity vector barely
 * moves between reports, and the bias-corrected rate is below STILL_DPS. */
#define STARTUP_CAL_US           1500000u
#define STILL_HOLD_US            1000000u
/* Spread allowed around the running mean while waiting for the start-up
 * calibration, and the largest corrected rate the running bias follows.
 * The latter is kept small so a deliberate slow turn is not absorbed. */
#define STARTUP_SPREAD_DPS       2.0f
#define STILL_DPS                1.0f
#define STILL_G_TOL              0.08f
#define STILL_G_DELTA            0.03f
#define BIAS_ALPHA               0.002f

/* Set to 1 to invert an axis if the aim moves the wrong way on hardware. */
#ifndef RJM_GYRO_AIM_INVERT_X
#define RJM_GYRO_AIM_INVERT_X 0
#endif
#ifndef RJM_GYRO_AIM_INVERT_Y
#define RJM_GYRO_AIM_INVERT_Y 0
#endif

/* Gaps longer than this (missed reports, reconnect) are not integrated. */
#define MAX_DT_US                50000u

#define AIM_MAX                  65535.0f
#define AIM_CENTER               32767.5f
/* The internal aim may travel three screens beyond each edge. Turning past the
 * edge and back therefore lands where the controller points again, while
 * the reported aim stays on screen. */
#define AIM_VIRTUAL_MIN          (-3.0f * AIM_MAX)
#define AIM_VIRTUAL_MAX          (4.0f * AIM_MAX)

struct AimState {
    bool active;
    bool recenter_prev;
    bool have_up;
    uint32_t last_us;
    float x, y;
    float up[3];
    float pitch_axis[3];
    float bias[3];
    /* bias tracking */
    bool startup_done;
    bool still;
    uint32_t still_since_us;
    float still_sum[3];
    uint32_t still_count;
    float prev_accel_dir[3];
    bool have_prev_accel;
};

static struct AimState g_aim[2];

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Axis whose rotation raises or lowers the aim, in the controller frame.
 * In the PlayStation frame used here, Y is the face normal (buttons side)
 * and -Z is the pointing direction, away from the player.
 * The pointing direction is levelled against gravity and the pitch axis is
 * forward x up, so the vertical aim follows elevation even when the
 * controller is tilted or rolled. */
static void compute_pitch_axis(const float up[3], float out[3])
{
    float f[3] = {0.0f, 0.0f, -1.0f};
    float d = f[0] * up[0] + f[1] * up[1] + f[2] * up[2];
    for (int i = 0; i < 3; ++i) f[i] -= d * up[i];
    float p[3] = {
        f[1] * up[2] - f[2] * up[1],
        f[2] * up[0] - f[0] * up[2],
        f[0] * up[1] - f[1] * up[0],
    };
    float n = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    if (n < 1e-3f) {
        out[0] = 1.0f; out[1] = 0.0f; out[2] = 0.0f;
        return;
    }
    for (int i = 0; i < 3; ++i) out[i] = p[i] / n;
}

static void bias_track(struct AimState *s, const float gyro[3], const float accel[3],
                       float g_ratio, uint32_t now)
{
    float dir[3] = {0.0f, 0.0f, 0.0f};
    float an = sqrtf(accel[0] * accel[0] + accel[1] * accel[1] + accel[2] * accel[2]);
    if (an > 1.0f)
        for (int i = 0; i < 3; ++i) dir[i] = accel[i] / an;
    float moved = 0.0f;
    if (s->have_prev_accel)
        for (int i = 0; i < 3; ++i) moved += fabsf(dir[i] - s->prev_accel_dir[i]);
    for (int i = 0; i < 3; ++i) s->prev_accel_dir[i] = dir[i];
    bool first = !s->have_prev_accel;
    s->have_prev_accel = true;

    bool still = !first && fabsf(g_ratio - 1.0f) <= STILL_G_TOL && moved <= STILL_G_DELTA;
    for (int i = 0; i < 3 && still; ++i)
        if (fabsf(gyro[i] - s->bias[i]) > STILL_DPS && s->startup_done) still = false;
    /* Before the first calibration the bias is unknown, so judge stillness by
     * the spread around the running mean instead of by the absolute rate. */
    if (still && !s->startup_done && s->still && s->still_count > 0) {
        for (int i = 0; i < 3; ++i)
            if (fabsf(gyro[i] - s->still_sum[i] / (float)s->still_count) > STARTUP_SPREAD_DPS) still = false;
    }

    if (!still) {
        s->still = false;
        s->still_count = 0;
        return;
    }
    if (!s->still) {
        s->still = true;
        s->still_since_us = now;
        s->still_count = 0;
        for (int i = 0; i < 3; ++i) s->still_sum[i] = 0.0f;
    }
    for (int i = 0; i < 3; ++i) s->still_sum[i] += gyro[i];
    s->still_count++;

    uint32_t held = now - s->still_since_us;
    if (!s->startup_done) {
        if (held >= STARTUP_CAL_US) {
            for (int i = 0; i < 3; ++i) s->bias[i] = s->still_sum[i] / (float)s->still_count;
            s->startup_done = true;
        }
    } else if (held >= STILL_HOLD_US) {
        for (int i = 0; i < 3; ++i) s->bias[i] += BIAS_ALPHA * (gyro[i] - s->bias[i]);
    }
}

void rjm_gyro_aim_reset(int player)
{
    if (player < 0 || player >= 2) return;
    memset(&g_aim[player], 0, sizeof(g_aim[player]));
}

bool rjm_gyro_aim_update(int player, const struct RjmGyroSample *sample,
                         uint16_t *x, uint16_t *y, bool *offscreen)
{
    if (player < 0 || player >= 2 || !sample || sample->kind == RJM_GYRO_KIND_NONE)
        return false;

    struct AimState *s = &g_aim[player];
    float gyro[3], accel[3];
    float scale = (float)(sample->gyro_range_dps ? sample->gyro_range_dps : 2000) / 32768.0f;
    for (int i = 0; i < 3; ++i) {
        gyro[i] = (float)sample->gyro[i] * scale;
        accel[i] = (float)sample->accel[i];
    }

    if (!s->active) {
        s->active = true;
        s->x = AIM_CENTER;
        s->y = AIM_CENTER;
        s->last_us = sample->time_us;
    }

    /* Turning is measured about gravity ("up") and aiming up/down about the
     * level axis perpendicular to the pointing direction. Both are sampled
     * from the accelerometer, not tracked continuously: at the first report,
     * when the start-up calibration completes (the controller is still, so
     * gravity is clean) and on each recenter press. Held flat, buttons up,
     * this is simply turn = Y, pitch = X. */
    bool recenter_edge = sample->recenter && !s->recenter_prev;
    s->recenter_prev = sample->recenter;
    bool was_calibrated = s->startup_done;

    float anorm = sqrtf(accel[0] * accel[0] + accel[1] * accel[1] + accel[2] * accel[2]);
    float one_g = 32768.0f * 1000.0f /
                  (float)(sample->accel_range_mg ? sample->accel_range_mg : 4000);
    bias_track(s, gyro, accel, anorm / one_g, sample->time_us);

    bool calibrated_now = s->startup_done && !was_calibrated;
    if ((!s->have_up || recenter_edge || calibrated_now) && anorm > 1.0f) {
        for (int i = 0; i < 3; ++i) s->up[i] = accel[i] / anorm;
        s->have_up = true;
        compute_pitch_axis(s->up, s->pitch_axis);
    }
    if (!s->have_up) {
        s->up[0] = 0.0f; s->up[1] = 1.0f; s->up[2] = 0.0f;
        compute_pitch_axis(s->up, s->pitch_axis);
    }

    uint32_t dt_us = sample->time_us - s->last_us;
    if (dt_us > MAX_DT_US) dt_us = 0;
    s->last_us = sample->time_us;
    float dt = (float)dt_us * 1e-6f;

    float g[3];
    for (int i = 0; i < 3; ++i) g[i] = gyro[i] - s->bias[i];
    float yaw_rate = g[0] * s->up[0] + g[1] * s->up[1] + g[2] * s->up[2];
    float pitch_rate = g[0] * s->pitch_axis[0] + g[1] * s->pitch_axis[1] +
                       g[2] * s->pitch_axis[2];
    float h_span = (float)sample->span_deg;
    if (h_span < RJM_GYRO_SPAN_MIN_DEG || h_span > RJM_GYRO_SPAN_MAX_DEG)
        h_span = RJM_GYRO_SPAN_DEFAULT_DEG;
    float dx = -yaw_rate * dt * (AIM_MAX / h_span);
    float dy = -pitch_rate * dt * (AIM_MAX / (h_span * V_SPAN_RATIO));
#if RJM_GYRO_AIM_INVERT_X
    dx = -dx;
#endif
#if RJM_GYRO_AIM_INVERT_Y
    dy = -dy;
#endif
    if (sample->recenter) {
        s->x = AIM_CENTER;
        s->y = AIM_CENTER;
    } else {
        s->x = clampf(s->x + dx, AIM_VIRTUAL_MIN, AIM_VIRTUAL_MAX);
        s->y = clampf(s->y + dy, AIM_VIRTUAL_MIN, AIM_VIRTUAL_MAX);
    }
    *x = (uint16_t)(clampf(s->x, 0.0f, AIM_MAX) + 0.5f);
    *y = (uint16_t)(clampf(s->y, 0.0f, AIM_MAX) + 0.5f);
    *offscreen = s->x < 0.0f || s->x > AIM_MAX || s->y < 0.0f || s->y > AIM_MAX;
    return true;
}
