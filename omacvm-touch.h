/*
 * OmacVM: touch on a Mac's touch display (the MacBook Pro with the touch
 * screen, macOS 27; a Sidecar iPad's touches come the same way), decided
 * without AppKit (#843).
 *
 * AppKit gives a view that overrides the mouse methods, as QemuCocoaView
 * does, its generic mouse emulation: a tap is a click made at the lift, a
 * touch that moves is a left drag (text selected, a browser tab torn off),
 * one held about 730 ms a right button; no scroll events ever. ui/cocoa.m
 * takes every direct touch with one gesture recognizer that begins at
 * touch-down (so AppKit's emulation never runs) and asks this file what the
 * guest gets:
 *
 *   lifted before HOLD, moved at most SLOP        a click where the touch began
 *     (a tap within DOUBLE_TIME and DOUBLE_DIST of the last one lands on it:
 *     the guest sees a double click; fingers land 5-26 pt apart)
 *   moved more than SLOP before HOLD              scrolling, content following
 *                                                 the finger; a coast after a flick
 *   still at HOLD, then moved (any time later)    a left-button drag
 *   still at LONG and lifted without moving       a right click, at the release
 *                                                 (as Windows does: a right click
 *                                                 at a fixed time races a slow
 *                                                 hold-drag, people start moving
 *                                                 470-850 ms in)
 *   two fingers                                   scrolling by the fingers' own
 *                                                 movement, ended (with its coast)
 *                                                 when the first finger lifts
 *
 * Times are the events' own (seconds), places the view's points (AppKit's:
 * y up). Everything is decided from them when the next event comes, so no
 * timer is needed but the coast's. Measured and tuned on the touch display
 * (#843).
 */
#ifndef OMACVM_TOUCH_H
#define OMACVM_TOUCH_H

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define OMACVM_TOUCH_SLOP 8.0          /* points a tap or a hold may move */
#define OMACVM_TOUCH_HOLD 0.35         /* s still: a move after this is a drag */
#define OMACVM_TOUCH_LONG 0.6          /* s still and lifted: a right click */
#define OMACVM_TOUCH_DOUBLE_TIME 0.4   /* s from a tap's lift to the next touch */
#define OMACVM_TOUCH_DOUBLE_DIST 40.0  /* points: the next tap lands on the last */
#define OMACVM_TOUCH_COAST_MIN 100.0   /* points/s: slower lifts do not coast */
#define OMACVM_TOUCH_COAST_STOP 20.0   /* points/s: a coast ends below this */
#define OMACVM_TOUCH_DECEL 0.998       /* a coast's speed kept per ms (iOS's) */
#define OMACVM_TOUCH_STOPPED 0.05      /* s still before the lift: no coast */
#define OMACVM_TOUCH_VEL_WINDOW 0.08   /* s of movement a lift's speed is from */
#define OMACVM_TOUCH_SAMPLES 32
#define OMACVM_TOUCH_GAIN 2.0          /* wheel units per point (OMACVM_TOUCH_SCROLL) */

enum {
    OMACVM_TOUCH_IDLE,
    OMACVM_TOUCH_PENDING,   /* one finger down, nothing decided yet */
    OMACVM_TOUCH_SCROLL,
    OMACVM_TOUCH_DRAG,
    OMACVM_TOUCH_MULTI,     /* two or more fingers: scrolling */
    OMACVM_TOUCH_DONE,      /* decided and sent; waiting for the last lift */
};

enum {
    OMACVM_TA_MOVE,         /* the guest's pointer to (x, y) */
    OMACVM_TA_LEFT_DOWN,
    OMACVM_TA_LEFT_UP,
    OMACVM_TA_RIGHT_DOWN,
    OMACVM_TA_RIGHT_UP,
    OMACVM_TA_SCROLL,       /* the content moves by (dx, dy) points */
    OMACVM_TA_COAST,        /* a coast starts at (dx, dy) points/s */
    OMACVM_TA_STOP_COAST,
};

typedef struct OmacVMTouchAction {
    int kind;
    double x, y, dx, dy;
} OmacVMTouchAction;

typedef struct OmacVMTouchActions {
    OmacVMTouchAction a[8];
    int n;
} OmacVMTouchActions;

typedef struct OmacVMTouch {
    int phase;
    double x0, y0, t0;      /* where and when the touch began */
    double x, y, t;         /* the last place, and the last movement's time */
    double st[OMACVM_TOUCH_SAMPLES], sx[OMACVM_TOUCH_SAMPLES], sy[OMACVM_TOUCH_SAMPLES];
    int ns;                 /* samples kept (the newest last) */
    bool tap;               /* the last tap, for a double tap */
    double tap_t, tap_x, tap_y;
} OmacVMTouch;

static inline void omacvm_touch_add(OmacVMTouchActions *o, int kind, double x, double y,
                                    double dx, double dy)
{
    if (o->n < (int)(sizeof(o->a) / sizeof(o->a[0]))) {
        o->a[o->n++] = (OmacVMTouchAction){ kind, x, y, dx, dy };
    }
}

static inline bool omacvm_touch_ok(double v)
{
    return isfinite(v);
}

/* Movement for the lift's speed: the newest OMACVM_TOUCH_SAMPLES kept. */
static inline void omacvm_touch_sample(OmacVMTouch *t, double now, double dx, double dy)
{
    if (t->ns == OMACVM_TOUCH_SAMPLES) {
        memmove(t->st, t->st + 1, sizeof(t->st[0]) * (OMACVM_TOUCH_SAMPLES - 1));
        memmove(t->sx, t->sx + 1, sizeof(t->sx[0]) * (OMACVM_TOUCH_SAMPLES - 1));
        memmove(t->sy, t->sy + 1, sizeof(t->sy[0]) * (OMACVM_TOUCH_SAMPLES - 1));
        t->ns--;
    }
    t->st[t->ns] = now;
    t->sx[t->ns] = dx;
    t->sy[t->ns] = dy;
    t->ns++;
}

/*
 * The speed at a lift (points/s), over the last OMACVM_TOUCH_VEL_WINDOW of
 * movement; none if the finger stood still OMACVM_TOUCH_STOPPED before it.
 */
static inline bool omacvm_touch_velocity(const OmacVMTouch *t, double now, double *vx, double *vy)
{
    double x = 0, y = 0, first = -1;

    *vx = *vy = 0;
    if (!t->ns || now - t->st[t->ns - 1] > OMACVM_TOUCH_STOPPED) {
        return false;
    }
    for (int i = 0; i < t->ns; i++) {
        if (now - t->st[i] <= OMACVM_TOUCH_VEL_WINDOW) {
            if (first < 0) {
                first = t->st[i];
            }
            x += t->sx[i];
            y += t->sy[i];
        }
    }
    if (first < 0) {
        return false;
    }
    /* One frame (8 ms at 120 Hz) more than the samples span, at least two. */
    double span = fmax(now - first + 0.008, 0.016);
    *vx = x / span;
    *vy = y / span;
    return hypot(*vx, *vy) >= OMACVM_TOUCH_COAST_MIN;
}

static inline void omacvm_touch_end_scroll(OmacVMTouch *t, double now, OmacVMTouchActions *o)
{
    double vx, vy;

    if (omacvm_touch_velocity(t, now, &vx, &vy)) {
        omacvm_touch_add(o, OMACVM_TA_COAST, t->x, t->y, vx, vy);
    }
    t->phase = OMACVM_TOUCH_DONE;
}

/*
 * A finger landed: n fingers touch now, (x, y) their centre. A new touch
 * stops a coast. A second finger makes a scroll of what was not decided yet.
 */
static inline void omacvm_touch_begin(OmacVMTouch *t, double now, double x, double y, int n,
                                      OmacVMTouchActions *o)
{
    o->n = 0;
    if (!omacvm_touch_ok(now) || !omacvm_touch_ok(x) || !omacvm_touch_ok(y) || n < 1) {
        return;
    }
    if (t->phase == OMACVM_TOUCH_IDLE) {
        omacvm_touch_add(o, OMACVM_TA_STOP_COAST, x, y, 0, 0);
        t->x0 = t->x = x;
        t->y0 = t->y = y;
        t->t0 = t->t = now;
        t->ns = 0;
        t->phase = n >= 2 ? OMACVM_TOUCH_MULTI : OMACVM_TOUCH_PENDING;
        if (t->phase == OMACVM_TOUCH_MULTI) {
            /* Wayland scrolls the surface under the pointer. */
            omacvm_touch_add(o, OMACVM_TA_MOVE, x, y, 0, 0);
        }
        return;
    }
    if (n >= 2 && t->phase == OMACVM_TOUCH_PENDING) {
        t->phase = OMACVM_TOUCH_MULTI;
        t->x = x;
        t->y = y;
        t->t = now;
        omacvm_touch_add(o, OMACVM_TA_MOVE, x, y, 0, 0);
    } else if (n >= 2 && t->phase == OMACVM_TOUCH_SCROLL) {
        t->phase = OMACVM_TOUCH_MULTI;
    }
}

/*
 * The fingers moved: (x, y) their centre now, (dx, dy) their own movement
 * (the mean over the fingers in both frames: a finger landing or lifting
 * moves the centre, not the content).
 */
static inline void omacvm_touch_move(OmacVMTouch *t, double now, double x, double y,
                                     double dx, double dy, OmacVMTouchActions *o)
{
    o->n = 0;
    if (!omacvm_touch_ok(now) || !omacvm_touch_ok(x) || !omacvm_touch_ok(y) ||
        !omacvm_touch_ok(dx) || !omacvm_touch_ok(dy)) {
        return;
    }
    switch (t->phase) {
    case OMACVM_TOUCH_PENDING:
        if (hypot(x - t->x0, y - t->y0) <= OMACVM_TOUCH_SLOP) {
            return;
        }
        if (now - t->t0 < OMACVM_TOUCH_HOLD) {
            t->phase = OMACVM_TOUCH_SCROLL;
            omacvm_touch_add(o, OMACVM_TA_MOVE, t->x0, t->y0, 0, 0);
            omacvm_touch_add(o, OMACVM_TA_SCROLL, x, y, x - t->x0, y - t->y0);
            omacvm_touch_sample(t, now, x - t->x0, y - t->y0);
        } else {
            t->phase = OMACVM_TOUCH_DRAG;
            omacvm_touch_add(o, OMACVM_TA_MOVE, t->x0, t->y0, 0, 0);
            omacvm_touch_add(o, OMACVM_TA_LEFT_DOWN, t->x0, t->y0, 0, 0);
            omacvm_touch_add(o, OMACVM_TA_MOVE, x, y, 0, 0);
        }
        break;
    case OMACVM_TOUCH_SCROLL:
    case OMACVM_TOUCH_MULTI:
        omacvm_touch_add(o, OMACVM_TA_SCROLL, x, y, dx, dy);
        omacvm_touch_sample(t, now, dx, dy);
        break;
    case OMACVM_TOUCH_DRAG:
        omacvm_touch_add(o, OMACVM_TA_MOVE, x, y, 0, 0);
        break;
    default:
        return;
    }
    t->x = x;
    t->y = y;
    t->t = now;
}

/*
 * A finger lifted, n still touch. Two fingers' scroll ends at the first
 * lift; everything else at the last.
 */
static inline void omacvm_touch_lift(OmacVMTouch *t, double now, int n, OmacVMTouchActions *o)
{
    o->n = 0;
    if (!omacvm_touch_ok(now)) {
        now = t->t;
    }
    if (t->phase == OMACVM_TOUCH_MULTI) {
        omacvm_touch_end_scroll(t, now, o);
    }
    if (n > 0) {
        return;
    }
    switch (t->phase) {
    case OMACVM_TOUCH_PENDING:
        if (now - t->t0 >= OMACVM_TOUCH_LONG) {
            omacvm_touch_add(o, OMACVM_TA_MOVE, t->x0, t->y0, 0, 0);
            omacvm_touch_add(o, OMACVM_TA_RIGHT_DOWN, t->x0, t->y0, 0, 0);
            omacvm_touch_add(o, OMACVM_TA_RIGHT_UP, t->x0, t->y0, 0, 0);
            t->tap = false;
        } else {
            double x = t->x0, y = t->y0;

            if (t->tap && t->t0 - t->tap_t <= OMACVM_TOUCH_DOUBLE_TIME &&
                hypot(x - t->tap_x, y - t->tap_y) <= OMACVM_TOUCH_DOUBLE_DIST) {
                x = t->tap_x;
                y = t->tap_y;
            }
            omacvm_touch_add(o, OMACVM_TA_MOVE, x, y, 0, 0);
            omacvm_touch_add(o, OMACVM_TA_LEFT_DOWN, x, y, 0, 0);
            omacvm_touch_add(o, OMACVM_TA_LEFT_UP, x, y, 0, 0);
            t->tap = true;
            t->tap_t = now;
            t->tap_x = x;
            t->tap_y = y;
        }
        break;
    case OMACVM_TOUCH_SCROLL:
        omacvm_touch_end_scroll(t, now, o);
        break;
    case OMACVM_TOUCH_DRAG:
        omacvm_touch_add(o, OMACVM_TA_MOVE, t->x, t->y, 0, 0);
        omacvm_touch_add(o, OMACVM_TA_LEFT_UP, t->x, t->y, 0, 0);
        break;
    default:
        break;
    }
    if (t->phase != OMACVM_TOUCH_PENDING) {
        t->tap = false;
    }
    t->phase = OMACVM_TOUCH_IDLE;
}

/* macOS took the touches (a system gesture): a drag's button goes up. */
static inline void omacvm_touch_cancel(OmacVMTouch *t, OmacVMTouchActions *o)
{
    o->n = 0;
    if (t->phase == OMACVM_TOUCH_DRAG) {
        omacvm_touch_add(o, OMACVM_TA_LEFT_UP, t->x, t->y, 0, 0);
    }
    t->phase = OMACVM_TOUCH_IDLE;
    t->tap = false;
}

/*
 * One step of a coast, dt seconds on: the content moves (*dx, *dy) points,
 * the speed (*vx, *vy) slows as iOS's scroll views do. False when it ended.
 */
static inline bool omacvm_touch_coast_step(double *vx, double *vy, double dt,
                                           double *dx, double *dy)
{
    *dx = *dy = 0;
    if (!omacvm_touch_ok(*vx) || !omacvm_touch_ok(*vy) || !omacvm_touch_ok(dt) || dt <= 0) {
        return false;
    }
    dt = fmin(dt, 0.1);   /* a late timer: no jump */
    double k = pow(OMACVM_TOUCH_DECEL, dt * 1000.0);
    *vx *= k;
    *vy *= k;
    *dx = *vx * dt;
    *dy = *vy * dt;
    return hypot(*vx, *vy) >= OMACVM_TOUCH_COAST_STOP;
}

/*
 * The content's movement (points, y up) as AppKit's precise scrolling deltas
 * (ui/cocoa.m's precise-scroll path takes them): the content follows the
 * finger whatever the Mac's scrolling direction, as everywhere on a touch
 * screen. scrollingDeltaY > 0 shows what is above (the content moves down:
 * a finger moving down, dy < 0); scrollingDeltaX > 0 shows what is left (the
 * content moves right: dx > 0).
 */
static inline void omacvm_touch_scroll_deltas(double dx, double dy, double gain,
                                              double *sx, double *sy)
{
    *sx = dx * gain;
    *sy = -dy * gain;
}

/* OMACVM_TOUCH: unset or anything but "0" is on. */
static inline bool omacvm_touch_off_env(const char *v)
{
    return v && !strcmp(v, "0");
}

/* OMACVM_TOUCH_SCROLL: wheel units per point, 0.25 to 8; else the default. */
static inline double omacvm_touch_gain_env(const char *v)
{
    char *end;
    double g;

    if (!v || !*v) {
        return OMACVM_TOUCH_GAIN;
    }
    g = strtod(v, &end);
    if (*end || !isfinite(g) || g < 0.25 || g > 8) {
        return OMACVM_TOUCH_GAIN;
    }
    return g;
}

#endif
