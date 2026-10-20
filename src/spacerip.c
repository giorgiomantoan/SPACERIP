/*****************************************************************************
 * spacerip.c: SPACERIP - an interstellar journey visualisation for VLC
 *****************************************************************************
 * Copyright (C) 2026 SPACERIP authors
 *
 * Authors: SPACERIP project
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston MA 02110-1301 USA.
 *****************************************************************************/

/**
 * SPACERIP: the viewer is the pilot of a small spacecraft crossing a living
 * galaxy.  The ship flies through a star field leaving light trails; nebulae
 * breathe with the bass line, a flash and a warp burst happen on every beat
 * and hyperspace rings expand from the centre of the screen.
 *
 * The module is a "visualization" audio filter: VLC feeds it with float PCM
 * (VLC_CODEC_FL32) and it owns a video output through aout_filter_RequestVout().
 * Everything is rendered in software (RGB32), so no GPU/driver dependency.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_aout.h>
#include <vlc_vout.h>
#include <vlc_filter.h>

/* Standalone build (outside the VLC tree, hence without config.h which
 * normally drags in vlc_fixups.h): provide the translation markers. */
#ifndef N_
# define N_(str) (str)
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* x86-64 always has SSE2: used to render 4 pixels of background at a time */
#if defined(__SSE2__) && !defined(SPACERIP_NO_SSE2)
# include <emmintrin.h>
#endif

#if defined(_WIN32)
# include <windows.h>
#elif defined(__unix__) || defined(__APPLE__)
# include <dlfcn.h>
#endif

/*****************************************************************************
 * Local constants
 *****************************************************************************/
#define SPACERIP_FFT_N     2048            /* analysis window (>= 46 ms)     */
#define SPACERIP_RING_N    8192            /* mono sample ring buffer (min)  */
#define SPACERIP_RING_SEC   3              /* retained audio history (s)     */
#define SPACERIP_RING_CAP   786432         /* hard cap on ring samples       */
#define SPACERIP_MAX_RINGS 6               /* simultaneous warp rings        */
#define SPACERIP_DUMP_MAX  8               /* max debug frames dumped        */
#define SPACERIP_LUT_N     256             /* falloff look-up table size     */
#define SPACERIP_LUT_UMAX  8.f             /* argument at the end of the LUT */
#define SPACERIP_LUT_SCALE ((SPACERIP_LUT_N - 1) / SPACERIP_LUT_UMAX)

#define DELAY_DEFAULT_MS    80              /* spacerip-delay default (ms)    */
#define SPACERIP_MAX_QUEUE  (800 * 1000)    /* hard cap on picture queue (us) */
#define SPACERIP_AUTO_MAX_W 3840           /* auto cap: 4K                    */
#define SPACERIP_AUTO_MAX_H 2160
#define SPACERIP_FALLBACK_W 1920           /* when detection fails            */
#define SPACERIP_FALLBACK_H 1080

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int  Open (vlc_object_t *);
static void Close (vlc_object_t *);

#define WIDTH_TEXT  N_("SPACERIP display width")
#define WIDTH_LONG  N_("Horizontal resolution of the SPACERIP rendering. " \
                       "Use 0 to automatically match the native resolution " \
                       "of your monitor (maximum definition). The value is " \
                       "rounded to an even number.")
#define HEIGHT_TEXT N_("SPACERIP display height")
#define HEIGHT_LONG N_("Vertical resolution of the SPACERIP rendering. " \
                       "Use 0 to automatically match the native resolution " \
                       "of your monitor (maximum definition). The value is " \
                       "rounded to an even number.")
#define FS_TEXT     N_("Start SPACERIP in fullscreen")
#define FS_LONG     N_("Open the visualisation directly in fullscreen " \
                       "video mode. It can also be toggled at any time " \
                       "with the standard VLC fullscreen controls.")
#define STARS_TEXT  N_("Number of stars")
#define STARS_LONG  N_("Number of stars in the interstellar field. " \
                       "Use 0 for the automatic value, scaled to keep " \
                       "the same star density at any resolution (more " \
                       "stars need more CPU).")
#define FPS_TEXT    N_("Frames per second")
#define FPS_LONG    N_("Rendering frame rate of the visualisation.")
#define SENS_TEXT   N_("Audio sensitivity")
#define SENS_LONG   N_("How strongly the music drives the journey: bass " \
                       "boost, beat detection and flashes.")
#define DELAY_TEXT  N_("Presentation delay")
#define DELAY_LONG  N_("Extra milliseconds added to the audio presentation " \
                       "timestamp of every frame. Keep it small (the " \
                       "timestamp already tells when the sound is heard); " \
                       "raise it only if you see the visuals ahead of the " \
                       "beat on your sound card.")
#define SPREAD_TEXT N_("Star spread")
#define SPREAD_LONG N_("How wide the star field is around the centre of " \
                       "the screen.")

vlc_module_begin ()
    set_shortname (N_("SPACERIP"))
    set_description (N_("SPACERIP interstellar journey"))
    set_category (CAT_AUDIO)
    set_subcategory (SUBCAT_AUDIO_VISUAL)
    set_capability ("visualization", 0)

    add_integer_with_range ("spacerip-width", 0, 0, 7680,
                            WIDTH_TEXT, WIDTH_LONG, false)
    add_integer_with_range ("spacerip-height", 0, 0, 4320,
                            HEIGHT_TEXT, HEIGHT_LONG, false)
    add_integer_with_range ("spacerip-stars", 0, 0, 5000,
                            STARS_TEXT, STARS_LONG, false)
    add_bool ("spacerip-fullscreen", false, FS_TEXT, FS_LONG, false)
    add_integer_with_range ("spacerip-fps", 30, 10, 60,
                            FPS_TEXT, FPS_LONG, false)
    add_float_with_range ("spacerip-sensitivity", 1.0, 0.1, 5.0,
                          SENS_TEXT, SENS_LONG, false)
    add_float_with_range ("spacerip-spread", 1.0, 0.4, 2.5,
                          SPREAD_TEXT, SPREAD_LONG, false)
    add_integer_with_range ("spacerip-delay", DELAY_DEFAULT_MS, 0, 2000,
                            DELAY_TEXT, DELAY_LONG, false)

    set_callbacks (Open, Close)
    add_shortcut ("spacerip")
vlc_module_end ()

/* Le distribuzioni Debian/Ubuntu che hanno ricostruito VLC per l'ABI "t64"
 * cercano il simbolo d'ingresso con suffisso "__3_0_0ft64". Esportiamo anche
 * quell'alias, così SPACERIP si carica sia con il VLC upstream (es. snap 3.0.x)
 * sia con i pacchetti delle distribuzioni, senza doverli raccogliere a mano. */
extern int vlc_entry__3_0_0ft64 (vlc_set_cb, void *)
    __attribute__ ((alias ("vlc_entry__3_0_0f")));

/*****************************************************************************
 * Local types
 *****************************************************************************/
struct star
{
    float x, y;            /* normalised position (-1..1)                    */
    float z;               /* depth (0..1], smaller = closer                 */
    float sx, sy;          /* current projection (pixels)                    */
    float px, py;          /* previous projection (pixels)                   */
    float br;              /* base brightness (0..1)                         */
    float tint;            /* colour variance (0..1)                         */
    float phase;           /* twinkle phase                                  */
    bool  has_prev;        /* previous projection is valid                   */
};

struct warp_ring
{
    float radius;          /* pixels                                         */
    float strength;        /* 0..1, fades out                                */
    float speed;           /* pixels/second                                  */
    float hue;             /* ring colour offset                             */
};

struct filter_sys_t
{
    vlc_thread_t  thread;
    video_format_t fmt;
    vout_thread_t *p_vout;

    vlc_mutex_t   lock;
    vlc_cond_t    wait;
    bool          b_exit;

    /* geometry */
    int  w, h;             /* full resolution                               */
    int  bw, bh;           /* half resolution background buffer             */
    int  stars;            /* star count                                    */
    float spread;          /* star field spread                             */
    float sensitivity;     /* audio gain                                    */

    /* audio */
    unsigned channels;
    float   *ring;         /* mono ring buffer                              */
    size_t   ring_w;       /* write position                                */
    size_t   ring_n;       /* ring capacity in samples                      */
    date_t   date;         /* presentation clock of the audio               */
    float    fft_in[SPACERIP_FFT_N];
    float    fft_re[SPACERIP_FFT_N];
    float    fft_im[SPACERIP_FFT_N];
    float    window[SPACERIP_FFT_N];
    float    tw_re[SPACERIP_FFT_N / 2];
    float    tw_im[SPACERIP_FFT_N / 2];
    unsigned bin_lo[3], bin_hi[3];
    float    rate;

    /* falloff LUT: fal[i] = 1 / (1 + SPACERIP_LUT_UMAX * i / (N-1)) */
    float    fal[SPACERIP_LUT_N];

    /* analysis results (render thread only) */
    float raw[3];          /* bass / mid / treble rms                        */
    float env[3];          /* smoothed envelopes (attack/release)            */
    float bass_avg;        /* adaptive average for beat detection            */
    vlc_tick_t last_beat;
    vlc_tick_t beat_prev;
    float    beat_bpm;
    unsigned beats;
    float    flash;        /* screen flash, decays                           */
    float    warp;         /* hyperwarp boost, decays                        */

    /* scene */
    struct star     *stars_tab;
    struct warp_ring rings[SPACERIP_MAX_RINGS];
    unsigned rng;
    float t;               /* scene time (s)                                */
    float speed;           /* current flight speed (depth units / s)        */
    float hue;             /* global colour hue (0..1)                       */
    float hue_speed;

    /* background buffers */
    uint32_t *bg;          /* bw * bh, RGB32                                 */
    uint8_t  *vig;         /* bw * bh, vignette factors 0..255               */
    uint8_t  *grad;        /* bh * 3, vertical gradient                      */
    float     glow;        /* centre glow cache                              */

    /* timing */
    vlc_tick_t interval;   /* frame interval (us)                            */
    vlc_tick_t delay;      /* extra presentation delay (us)                   */
    vlc_tick_t last_frame;
    unsigned   frames;
    char      *dump;       /* SPACERIP_DUMP prefix or NULL                   */
    unsigned   dump_left;
    float      ana_ms;     /* EMA: analyze+update (ms)                       */
    float      gp_ms;      /* EMA: vout_GetPicture wait (ms)                 */
    float      ren_ms;     /* EMA: render_frame (ms)                         */
    float      put_ms;     /* EMA: vout_PutPicture (ms)                      */
    float      bg_ms;      /* EMA: render_background (ms)                    */
    float      up_ms;      /* EMA: upscale_background (ms)                   */
    float      st_ms;      /* EMA: draw_stars (ms)                           */
    float      ri_ms;      /* EMA: draw_rings (ms)                           */
    unsigned   bg_count;   /* frames since the background was last drawn     */
    float      bg_flash;   /* flash level baked into the background          */
    float      gp_raw;     /* last raw GetPicture wait (ms)                  */
    float      pd;         /* audio pts - now (ms), for A/V diagnostics      */
    vlc_tick_t start;      /* thread start, for the final average fps        */
};

/*****************************************************************************
 * Small helpers
 *****************************************************************************/
static inline uint32_t sr_rand (struct filter_sys_t *sys)
{
    uint32_t x = sys->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    sys->rng = x;
    return x;
}

static inline float sr_frand (struct filter_sys_t *sys)
{
    return (sr_rand (sys) & 0xFFFFFF) / 16777216.f;
}

/* average of two packed 0x00RRGGBB pixels */
static inline uint32_t avg2 (uint32_t a, uint32_t b)
{
    return (a & b) + (((a ^ b) >> 1) & 0x7F7F7F7FL);
}

/* saturating addition of two packed pixels */
static inline uint32_t addpix (uint32_t a, uint32_t b)
{
    unsigned ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    unsigned br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    ar += br; ag += bg; ab += bb;
    if (ar > 255) ar = 255;
    if (ag > 255) ag = 255;
    if (ab > 255) ab = 255;
    return (ar << 16) | (ag << 8) | ab;
}

static inline uint32_t scalepix (uint32_t a, unsigned num, unsigned den)
{
    unsigned r = (((a >> 16) & 0xFF) * num) / den;
    unsigned g = (((a >> 8) & 0xFF) * num) / den;
    unsigned b = ((a & 0xFF) * num) / den;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (r << 16) | (g << 8) | b;
}

static void hsv2rgb (float h, float s, float v,
                     unsigned *r, unsigned *g, unsigned *b)
{
    h -= floorf (h);
    if (h < 0.f) h += 1.f;
    float i = h * 6.f;
    int sector = (int)i;
    float f = i - sector;
    float p = v * (1.f - s);
    float q = v * (1.f - s * f);
    float tt = v * (1.f - s * (1.f - f));
    float rr, gg, bb;
    switch (sector % 6)
    {
        case 0: rr = v;  gg = tt; bb = p;  break;
        case 1: rr = q;  gg = v;  bb = p;  break;
        case 2: rr = p;  gg = v;  bb = tt; break;
        case 3: rr = p;  gg = q;  bb = v;  break;
        case 4: rr = tt; gg = p;  bb = v;  break;
        default: rr = v; gg = p;  bb = q;  break;
    }
    *r = (unsigned)(rr * 255.f + .5f);
    *g = (unsigned)(gg * 255.f + .5f);
    *b = (unsigned)(bb * 255.f + .5f);
}

/*****************************************************************************
 * Native resolution of the monitor (best effort, no hard dependency)
 *****************************************************************************/
static bool detect_monitor_size (int *width, int *height)
{
#if defined(_WIN32)
    HMODULE user32 = GetModuleHandleW (L"user32.dll");
    if (user32 != NULL)
    {
        int (__stdcall *get_sys_metrics)(int) =
            (int (__stdcall *)(int))GetProcAddress (user32, "GetSystemMetrics");
        if (get_sys_metrics != NULL)
        {
            int w = get_sys_metrics (0);   /* SM_CXSCREEN */
            int h = get_sys_metrics (1);   /* SM_CYSCREEN */
            if (w > 0 && h > 0)
            {
                *width = w;
                *height = h;
                return true;
            }
        }
    }
    return false;
#elif defined(__unix__) || defined(__APPLE__)
    /* Load libX11 dynamically: works on X11 and on XWayland, and simply
     * fails (returning false) where it is not available. */
    void *x11 = dlopen ("libX11.so.6", RTLD_LAZY | RTLD_NOLOAD);
    if (x11 == NULL)
        x11 = dlopen ("libX11.so.6", RTLD_LAZY);
    if (x11 == NULL)
        x11 = dlopen ("libX11.dylib", RTLD_LAZY);
    if (x11 == NULL)
        return false;

    void *(*x_open)(const char *) =
        (void *(*)(const char *))dlsym (x11, "XOpenDisplay");
    int (*x_width)(void *, int) =
        (int (*)(void *, int))dlsym (x11, "XDisplayWidth");
    int (*x_height)(void *, int) =
        (int (*)(void *, int))dlsym (x11, "XDisplayHeight");
    int (*x_screen)(void *) =
        (int (*)(void *))dlsym (x11, "XDefaultScreen");
    int (*x_close)(void *) =
        (int (*)(void *))dlsym (x11, "XCloseDisplay");

    bool ok = false;
    if (x_open && x_width && x_height && x_screen && x_close)
    {
        void *dpy = x_open (NULL);
        if (dpy != NULL)
        {
            int screen = x_screen (dpy);
            int w = x_width (dpy, screen);
            int h = x_height (dpy, screen);
            x_close (dpy);
            if (w > 0 && h > 0)
            {
                *width = w;
                *height = h;
                ok = true;
            }
        }
    }
    dlclose (x11);
    return ok;
#else
    return false;
#endif
}

/*****************************************************************************
 * Audio analysis: windowed FFT and band RMS
 *****************************************************************************/
static void fft_init_tables (struct filter_sys_t *sys)
{
    for (int i = 0; i < SPACERIP_FFT_N; i++)
        sys->window[i] = 0.5f * (1.f - cosf (2.f * (float)M_PI * i
                                              / (SPACERIP_FFT_N - 1)));
    for (int i = 0; i < SPACERIP_FFT_N / 2; i++)
    {
        float a = -2.f * (float)M_PI * i / SPACERIP_FFT_N;
        sys->tw_re[i] = cosf (a);
        sys->tw_im[i] = sinf (a);
    }

    /* falloff curve shared by the nebula blobs and the centre glow:
     * f(u) = 1 / (1 + u), sampled uniformly on [0, SPACERIP_LUT_UMAX] */
    for (int i = 0; i < SPACERIP_LUT_N; i++)
    {
        float u = SPACERIP_LUT_UMAX * i / (SPACERIP_LUT_N - 1);
        sys->fal[i] = 1.f / (1.f + u);
    }
}

static void fft_run (struct filter_sys_t *sys)
{
    float *restrict re = sys->fft_re;
    float *restrict im = sys->fft_im;
    enum { N = SPACERIP_FFT_N };

    /* bit reversal permutation */
    for (int i = 1, j = 0; i < N; i++)
    {
        int bit = N >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
        {
            float tr = re[i]; re[i] = re[j]; re[j] = tr;
            float ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }

    for (int len = 2; len <= N; len <<= 1)
    {
        int half = len >> 1;
        int step = N / len;
        for (int i = 0; i < N; i += len)
        {
            for (int k = 0; k < half; k++)
            {
                int t = k * step;
                float wr = sys->tw_re[t], wi = sys->tw_im[t];
                int ia = i + k, ib = ia + half;
                float vr = re[ib] * wr - im[ib] * wi;
                float vi = re[ib] * wi + im[ib] * wr;
                re[ib] = re[ia] - vr;
                im[ib] = im[ia] - vi;
                re[ia] += vr;
                im[ia] += vi;
            }
        }
    }
}

/* Copy SPACERIP_FFT_N mono samples out of the ring buffer, ending `lag`
 * samples before the newest one.  The caller holds sys->lock. */
static void ring_snapshot (struct filter_sys_t *sys, size_t lag)
{
    if (lag + SPACERIP_FFT_N > sys->ring_n)
        lag = sys->ring_n - SPACERIP_FFT_N;
    size_t start = (sys->ring_w + sys->ring_n - SPACERIP_FFT_N - lag)
                   % sys->ring_n;
    for (int i = 0; i < SPACERIP_FFT_N; i++)
    {
        size_t pos = (start + i) % sys->ring_n;
        sys->fft_in[i] = sys->ring[pos];
    }
}

static void analyze (struct filter_sys_t *sys, vlc_tick_t now)
{
    for (int i = 0; i < SPACERIP_FFT_N; i++)
    {
        sys->fft_re[i] = sys->fft_in[i] * sys->window[i];
        sys->fft_im[i] = 0.f;
    }
    fft_run (sys);

    float norm = 2.f / ((float)SPACERIP_FFT_N * SPACERIP_FFT_N);
    float band[3];
    for (int b = 0; b < 3; b++)
    {
        float sum = 0.f;
        for (unsigned k = sys->bin_lo[b]; k < sys->bin_hi[b]; k++)
        {
            float re = sys->fft_re[k], im = sys->fft_im[k];
            sum += re * re + im * im;
        }
        band[b] = sqrtf (sum * norm);
    }

    /* store raw values, envelopes are smoothed in update_scene() */
    sys->raw[0] = band[0];
    sys->raw[1] = band[1];
    sys->raw[2] = band[2];

    /* --- beat detection on the low band ------------------------------- */
    float bass = band[0];
    if (sys->bass_avg <= 0.f)
        sys->bass_avg = bass;
    else if (bass > sys->bass_avg)
        sys->bass_avg += (bass - sys->bass_avg) * 0.20f;
    else
        sys->bass_avg += (bass - sys->bass_avg) * 0.025f;

    float sens = sys->sensitivity;
    float threshold = sys->bass_avg * (1.55f - 0.10f * sens);
    vlc_tick_t since = now - sys->last_beat;

    if (bass > threshold && bass > 0.012f && since > 165000
     && sys->bass_avg > 0.004f)
    {
        float strength = (bass / sys->bass_avg - 1.4f) / 1.6f + 0.45f;
        if (strength < 0.4f) strength = 0.4f;
        if (strength > 1.4f) strength = 1.4f;

        sys->beats++;
        if (sys->last_beat > 0 && since < 2000000)
        {
            float bpm = 60000000.f / (float)since;
            if (sys->beat_bpm <= 0.f)
                sys->beat_bpm = bpm;
            else
                sys->beat_bpm += (bpm - sys->beat_bpm) * 0.25f;
        }
        sys->beat_prev = sys->last_beat;
        sys->last_beat = now;

        /* react! */
        float kick = strength * sens;
        if (kick > 1.5f) kick = 1.5f;
        sys->warp += 0.85f * kick;
        if (sys->warp > 2.f) sys->warp = 2.f;
        sys->flash += 0.40f * kick;
        if (sys->flash > 1.f) sys->flash = 1.f;

        /* spawn an expanding hyperspace ring */
        for (int i = 0; i < SPACERIP_MAX_RINGS; i++)
            if (sys->rings[i].strength <= 0.02f)
            {
                sys->rings[i].radius = 12.f;
                sys->rings[i].strength = 0.30f + 0.45f * kick;
                sys->rings[i].speed = 620.f + 900.f * kick;
                sys->rings[i].hue = sys->hue + (sr_frand (sys) - .5f) * .1f;
                break;
            }
    }
}

/*****************************************************************************
 * Scene update
 *****************************************************************************/
static void respawn_star (struct filter_sys_t *sys, struct star *s)
{
    s->x = (sr_frand (sys) * 2.f - 1.f) * 1.5f * sys->spread;
    s->y = (sr_frand (sys) * 2.f - 1.f) * 0.92f * sys->spread;
    s->z = 1.f + sr_frand (sys) * 0.05f;
    s->br = 0.45f + sr_frand (sys) * 0.55f;
    s->tint = sr_frand (sys);
    s->phase = sr_frand (sys);
    s->has_prev = false;
}

static void update_scene (struct filter_sys_t *sys, float dt)
{
    float sens = sys->sensitivity;

    /* --- audio envelopes (fast attack, slow release) ----------------- */
    for (int b = 0; b < 3; b++)
    {
        float target = sys->raw[b] * 3.2f * sens;
        if (target > 1.7f) target = 1.7f;
        float k = (target > sys->env[b]) ? dt * 28.f : dt * 5.f;
        if (k > 1.f) k = 1.f;
        sys->env[b] += (target - sys->env[b]) * k;
    }

    /* --- decay transient effects ------------------------------------- */
    float d = 1.f / (1.f + dt * 2.6f);
    sys->warp *= d;
    d = 1.f / (1.f + dt * 5.5f);
    sys->flash *= d;

    /* --- flight speed: cruise + bass + hyperwarp --------------------- */
    float target = 0.26f + sys->env[0] * 0.55f + sys->warp * 2.3f;
    float ks = dt * 7.f;
    if (ks > 1.f) ks = 1.f;
    sys->speed += (target - sys->speed) * ks;

    /* --- colour hue slowly rotates, faster with the mids ------------- */
    sys->hue += dt * (0.016f + sys->env[1] * 0.035f * sens);
    if (sys->hue >= 1.f) sys->hue -= 1.f;

    sys->t += dt;

    /* --- warp rings --------------------------------------------------- */
    for (int i = 0; i < SPACERIP_MAX_RINGS; i++)
    {
        struct warp_ring *r = &sys->rings[i];
        if (r->strength <= 0.02f) { r->strength = 0.f; continue; }
        r->radius += r->speed * (1.f + sys->warp * 0.9f) * dt;
        r->strength *= 1.f / (1.f + dt * 1.35f);
        if (r->radius > (float)sys->w * 1.4f)
            r->strength = 0.f;
    }

    /* --- stars --------------------------------------------------------- */
    float focal = 0.46f * (float)sys->h;
    float cx = 0.5f * (float)sys->w;
    float cy = 0.5f * (float)sys->h;

    for (int i = 0; i < sys->stars; i++)
    {
        struct star *s = &sys->stars_tab[i];

        s->px = s->sx;
        s->py = s->sy;

        s->z -= sys->speed * dt;
        if (s->z < 0.055f)
        {
            respawn_star (sys, s);
            s->sx = cx + s->x * focal / s->z;
            s->sy = cy + s->y * focal / s->z;
            continue;
        }

        float k = focal / s->z;
        s->sx = cx + s->x * k;
        s->sy = cy + s->y * k;
    }
}

/*****************************************************************************
 * Rendering
 *****************************************************************************/
static void render_background (struct filter_sys_t *sys)
{
    const int bw = sys->bw, bh = sys->bh;
    uint32_t *bg = sys->bg;
    const uint8_t *vig = sys->vig;
    const uint8_t *grad = sys->grad;
    const float *fal = sys->fal;

    /* --- moving nebula blobs ------------------------------------------ */
    float bx[3], by[3], kb[3];
    unsigned br_[3], bg_[3], bb_[3];
    float tint_gain[3];
    tint_gain[0] = 0.55f + sys->env[0] * 1.15f;
    tint_gain[1] = 0.45f + sys->env[1] * 0.95f;
    tint_gain[2] = 0.35f + sys->env[2] * 0.85f;

    for (int i = 0; i < 3; i++)
    {
        bx[i] = bw * (0.5f + 0.34f * sinf (sys->t * (0.11f + 0.05f * i)
                                           + i * 2.1f));
        by[i] = bh * (0.5f + 0.30f * cosf (sys->t * (0.09f + 0.04f * i)
                                           + i * 1.3f));
        float rad = (0.20f + 0.07f * sinf (sys->t * 0.07f + i * 1.7f))
                    * (float)bw * (1.f + 0.35f * sys->env[0]);
        if (rad < 8.f) rad = 8.f;
        kb[i] = 6.f / (rad * rad);
        hsv2rgb (sys->hue + i * 0.31f, 0.78f, 1.f, &br_[i], &bg_[i], &bb_[i]);
    }

    /* centre glow (the destination star) */
    float glow_gain = (26.f + 130.f * sys->env[0]) * sys->sensitivity;
    if (glow_gain > 210.f) glow_gain = 210.f;
    float kg = 9.f / (0.13f * (float)bw * 0.13f * (float)bw);
    float gcx = 0.5f * bw, gcy = 0.5f * bh;

    float flash = sys->flash * 115.f;
    float flash_r = flash * 0.92f, flash_g = flash * 0.97f;

#if defined(__SSE2__) && !defined(SPACERIP_NO_SSE2)
    /* 4 pixels per iteration: everything stays in float and is converted to
     * 0x00RRGGBB once at the end (exact: the value is below 2^24). */
    for (int y = 0; y < bh; y++)
    {
        uint32_t *row = bg + y * bw;
        const uint8_t *gr = grad + y * 3;
        float gdy = y + 0.5f - gcy;
        float dyb[3];
        for (int i = 0; i < 3; i++)
            dyb[i] = y + 0.5f - by[i];

        const __m128 GY = _mm_set1_ps (gdy);
        const __m128 R0 = _mm_set1_ps ((float)gr[0]);
        const __m128 G0 = _mm_set1_ps ((float)gr[1]);
        const __m128 B0 = _mm_set1_ps ((float)gr[2]);
        const __m128 GCX = _mm_set1_ps (gcx);
        const __m128 KG = _mm_set1_ps (kg * SPACERIP_LUT_SCALE);
        const __m128 GGAIN = _mm_set1_ps (glow_gain);
        const __m128 C112 = _mm_set1_ps (1.12f);
        const __m128 C145 = _mm_set1_ps (1.45f);
        const __m128 LIM = _mm_set1_ps ((float)(SPACERIP_LUT_N - 1));
        const __m128 GMAX = _mm_set1_ps (1.4f);
        const __m128 K62 = _mm_set1_ps (0.62f);
        const __m128 INV256 = _mm_set1_ps (1.f / 256.f);
        const __m128 MAXV = _mm_set1_ps (255.f);
        const __m128 FR = _mm_set1_ps (flash_r);
        const __m128 FG = _mm_set1_ps (flash_g);
        const __m128 FB = _mm_set1_ps (flash);
        const __m128 C256 = _mm_set1_ps (256.f);
        const __m128 C64K = _mm_set1_ps (65536.f);
        __m128 BXR[3], DYR[3], KBR[3], TIR[3], BR[3], BG[3], BB[3];
        for (int i = 0; i < 3; i++)
        {
            BXR[i] = _mm_set1_ps (bx[i]);
            DYR[i] = _mm_set1_ps (dyb[i]);
            KBR[i] = _mm_set1_ps (kb[i] * SPACERIP_LUT_SCALE);
            TIR[i] = _mm_set1_ps (tint_gain[i]);
            BR[i] = _mm_set1_ps ((float)br_[i]);
            BG[i] = _mm_set1_ps ((float)bg_[i]);
            BB[i] = _mm_set1_ps ((float)bb_[i]);
        }
        const uint8_t *vrow = vig + y * bw;

        for (int x = 0; x < bw; x += 4)
        {
            const __m128 X = _mm_set_ps ((float)x + 3.5f,
                                         (float)x + 2.5f,
                                         (float)x + 1.5f,
                                         (float)x + 0.5f);
            int ii[4];

            /* centre glow */
            __m128 d = _mm_sub_ps (X, GCX);
            __m128 u = _mm_min_ps (_mm_mul_ps (
                                       _mm_add_ps (_mm_mul_ps (d, d),
                                                   _mm_mul_ps (GY, GY)),
                                       KG), LIM);
            /* memcpy: a raw __m128i store to an int[] would violate
             * strict aliasing and can be optimised away */
            __m128i iv = _mm_cvttps_epi32 (u);
            memcpy (ii, &iv, sizeof (ii));
            __m128 f = _mm_set_ps (fal[ii[3]], fal[ii[2]],
                                   fal[ii[1]], fal[ii[0]]);
            __m128 gl = _mm_mul_ps (GGAIN, f);
            __m128 vR = _mm_add_ps (R0, gl);
            __m128 vG = _mm_add_ps (G0, _mm_mul_ps (gl, C112));
            __m128 vB = _mm_add_ps (B0, _mm_mul_ps (gl, C145));

            /* nebula blobs */
            for (int i = 0; i < 3; i++)
            {
                __m128 dx = _mm_sub_ps (X, BXR[i]);
                __m128 dy = DYR[i];    /* already y + 0.5 - by[i] */
                u = _mm_min_ps (_mm_mul_ps (
                                    _mm_add_ps (_mm_mul_ps (dx, dx),
                                                _mm_mul_ps (dy, dy)),
                                    KBR[i]), LIM);
                iv = _mm_cvttps_epi32 (u);
                memcpy (ii, &iv, sizeof (ii));
                f = _mm_set_ps (fal[ii[3]], fal[ii[2]],
                                fal[ii[1]], fal[ii[0]]);
                __m128 add = _mm_mul_ps (
                    _mm_min_ps (_mm_mul_ps (TIR[i], f), GMAX), K62);
                vR = _mm_add_ps (vR, _mm_mul_ps (BR[i], add));
                vG = _mm_add_ps (vG, _mm_mul_ps (BG[i], add));
                vB = _mm_add_ps (vB, _mm_mul_ps (BB[i], add));
            }

            /* vignette (bw is a multiple of 4, so this read is aligned) */
            uint32_t v32;
            memcpy (&v32, vrow + x, sizeof (v32));
            __m128 vf = _mm_mul_ps (_mm_cvtepi32_ps (
                          _mm_unpacklo_epi16 (
                            _mm_unpacklo_epi8 (_mm_cvtsi32_si128 ((int)v32),
                                               _mm_setzero_si128 ()),
                            _mm_setzero_si128 ())), INV256);
            vR = _mm_mul_ps (vR, vf);
            vG = _mm_mul_ps (vG, vf);
            vB = _mm_mul_ps (vB, vf);

            /* beat flash, clamp, then pack (r << 16) | (g << 8) | b */
            vR = _mm_min_ps (_mm_add_ps (vR, FR), MAXV);
            vG = _mm_min_ps (_mm_add_ps (vG, FG), MAXV);
            vB = _mm_min_ps (_mm_add_ps (vB, FB), MAXV);
            /* truncation is mandatory here: the fractional part of vR
             * (multiplied by 65536) would bleed into the low bytes */
            vR = _mm_cvtepi32_ps (_mm_cvttps_epi32 (vR));
            vG = _mm_cvtepi32_ps (_mm_cvttps_epi32 (vG));
            vB = _mm_cvtepi32_ps (_mm_cvttps_epi32 (vB));
            __m128 pix = _mm_add_ps (_mm_mul_ps (vR, C64K),
                                     _mm_add_ps (_mm_mul_ps (vG, C256), vB));
            _mm_storeu_si128 ((__m128i *)(row + x), _mm_cvttps_epi32 (pix));
        }
    }
#else
    for (int y = 0; y < bh; y++)
    {
        uint32_t *row = bg + y * bw;
        const uint8_t *gr = grad + y * 3;
        float dyb[3];
        for (int i = 0; i < 3; i++)
            dyb[i] = y + 0.5f - by[i];
        float gdy = y + 0.5f - gcy;
        unsigned gr0 = gr[0], gg0 = gr[1], gb0 = gr[2];

        for (int x = 0; x < bw; x++)
        {
            unsigned r = gr0, g = gg0, b = gb0;

            /* centre glow */
            float dx0 = x + 0.5f - gcx;
            unsigned idx = (unsigned)((dx0 * dx0 + gdy * gdy) * kg
                                      * SPACERIP_LUT_SCALE);
            if (idx >= SPACERIP_LUT_N) idx = SPACERIP_LUT_N - 1;
            float gg = glow_gain * fal[idx];
            r += (unsigned)gg;
            g += (unsigned)(gg * 1.12f);
            b += (unsigned)(gg * 1.45f);

            /* nebula blobs */
            for (int i = 0; i < 3; i++)
            {
                float dx = x + 0.5f - bx[i];
                idx = (unsigned)((dx * dx + dyb[i] * dyb[i]) * kb[i]
                                 * SPACERIP_LUT_SCALE);
                if (idx >= SPACERIP_LUT_N) idx = SPACERIP_LUT_N - 1;
                float gain = tint_gain[i] * fal[idx];
                if (gain > 1.4f) gain = 1.4f;
                float add = gain * 0.62f;
                r += (unsigned)(br_[i] * add);
                g += (unsigned)(bg_[i] * add);
                b += (unsigned)(bb_[i] * add);
            }

            /* vignette */
            unsigned v = vig[y * bw + x];
            r = (r * v) >> 8;
            g = (g * v) >> 8;
            b = (b * v) >> 8;

            /* beat flash */
            r += (unsigned)flash_r;
            g += (unsigned)flash_g;
            b += (unsigned)flash;
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;

            row[x] = (r << 16) | (g << 8) | b;
        }
    }
#endif
}

/* Bilinear-ish (0.5 / 0.5 taps) upscale of the half resolution background */
static void upscale_background (struct filter_sys_t *sys,
                                uint32_t *dst, int stride)
{
    const int bw = sys->bw, bh = sys->bh, w = sys->w, h = sys->h;
    const int dw = w >> 1;
    const uint32_t *bg = sys->bg;

    for (int y = 0; y < h; y++)
    {
        int ry = y >> 1;
        int ry1 = ry + (y & 1);
        if (ry1 >= bh) ry1 = bh - 1;
        const uint32_t *ra = bg + ry * bw;
        const uint32_t *rb = bg + ry1 * bw;
        uint32_t *out = dst + y * stride;

        /* two destination pixels per iteration, no branches inside */
        int s = 0;
        if (!(y & 1))
        {
            for (; s < dw - 1; s++)
            {
                uint32_t a = ra[s], b = ra[s + 1];
                out[2 * s] = a;
                out[2 * s + 1] = avg2 (a, b);
            }
        }
        else
        {
            for (; s < dw - 1; s++)
            {
                uint32_t a0 = ra[s], a1 = ra[s + 1];
                uint32_t b0 = rb[s], b1 = rb[s + 1];
                out[2 * s] = avg2 (a0, b0);
                out[2 * s + 1] = avg2 (avg2 (a0, a1), avg2 (b0, b1));
            }
        }

        /* last pair of the row: the right neighbour exists only if the
         * background width was rounded up past w / 2 */
        int sx = dw - 1;
        int sx1 = (sx + 1 < bw) ? sx + 1 : sx;
        if (!(y & 1))
        {
            out[2 * sx] = ra[sx];
            out[2 * sx + 1] = avg2 (ra[sx], ra[sx1]);
        }
        else
        {
            out[2 * sx] = avg2 (ra[sx], rb[sx]);
            out[2 * sx + 1] = avg2 (avg2 (ra[sx], ra[sx1]),
                                    avg2 (rb[sx], rb[sx1]));
        }
    }
}

static inline void add_star_pixel (struct filter_sys_t *sys,
                                   uint32_t *p, int x, int y,
                                   uint32_t col)
{
    if (x < 0 || y < 0 || x >= sys->w || y >= sys->h)
        return;
    int bx = x >> 1, by = y >> 1;
    if (bx >= sys->bw) bx = sys->bw - 1;
    if (by >= sys->bh) by = sys->bh - 1;
    unsigned v = sys->vig[by * sys->bw + bx];
    col = scalepix (col, v, 255);
    *p = addpix (*p, col);
}

static void draw_stars (struct filter_sys_t *sys,
                        uint32_t *dst, int stride)
{
    const float w = (float)sys->w, h = (float)sys->h;
    const float max_len = 420.f;

    /* star size follows the output resolution, so a star drawn at 4K is
     * as visible as the same star drawn at 480p */
    int arm = sys->w / 1280;
    if (arm < 1) arm = 1;
    if (arm > 4) arm = 4;
    const bool wide_trail = sys->w >= 2560;

    for (int i = 0; i < sys->stars; i++)
    {
        struct star *s = &sys->stars_tab[i];

        /* brightness: closer = brighter + twinkle */
        float tw = 0.78f + 0.22f * sinf (sys->t * (2.2f + s->phase * 5.5f)
                                         + s->phase * 37.f);
        float near = 1.f - (s->z - 0.055f) / 0.95f;
        if (near < 0.f) near = 0.f;
        float b = s->br * (0.30f + 0.70f * near) * tw;

        unsigned rr, gg, bb;
        hsv2rgb (0.55f + s->tint * 0.13f, 0.10f + s->tint * 0.30f, 1.f,
                 &rr, &gg, &bb);
        uint32_t head = ((unsigned)(rr * b) << 16) | ((unsigned)(gg * b) << 8)
                        | (unsigned)(bb * b);
        if (((head >> 16) & 0xFF) < 6 && (head & 0xFF) < 6)
            head = (unsigned)(90.f * b) << 16 | (unsigned)(100.f * b) << 8
                   | (unsigned)(130.f * b);

        int x1 = (int)s->sx, y1 = (int)s->sy;
        int x0 = (int)s->px, y0 = (int)s->py;

        bool inside = s->sx > -8 && s->sx < w + 8 && s->sy > -8
                      && s->sy < h + 8;
        if (!inside)
            continue;

        if (!s->has_prev || sys->speed < 0.42f)
        {
            /* plain star: cross scaled to the output resolution */
            add_star_pixel (sys, dst + y1 * stride + x1, x1, y1, head);
            uint32_t side1 = scalepix (head, 55, 100);
            uint32_t side2 = scalepix (head, 26, 100);
            for (int a = 1; a <= arm; a++)
            {
                uint32_t side = (a == 1) ? side1 : side2;
                add_star_pixel (sys, dst + y1 * stride + (x1 - a),
                                x1 - a, y1, side);
                add_star_pixel (sys, dst + y1 * stride + (x1 + a),
                                x1 + a, y1, side);
                add_star_pixel (sys, dst + (y1 - a) * stride + x1,
                                x1, y1 - a, side);
                add_star_pixel (sys, dst + (y1 + a) * stride + x1,
                                x1, y1 + a, side);
            }
            continue;
        }

        /* light trail from the previous position to the current one */
        float dx = s->sx - s->px, dy = s->sy - s->py;
        float len = sqrtf (dx * dx + dy * dy);
        if (len > max_len)
        {
            float cut = max_len / len;
            s->px = s->sx - dx * cut;
            s->py = s->sy - dy * cut;
            x0 = (int)s->px; y0 = (int)s->py;
            len = max_len;
        }
        int steps = (int)len;
        if (steps < 1) steps = 1;
        if (steps > 400) steps = 400;

        uint32_t tail = scalepix (head, 22, 100);
        float ix = s->px, iy = s->py;
        float sx_step = (s->sx - s->px) / steps;
        float sy_step = (s->sy - s->py) / steps;

        for (int k = 0; k <= steps; k++)
        {
            unsigned num = 22 + 78 * (unsigned)k;
            uint32_t col = (k == steps) ? head
                           : (unsigned)(((((head >> 16) & 0xFF) * num) / 100)) << 16
                           | (unsigned)((((head >> 8) & 0xFF) * num) / 100) << 8
                           | (unsigned)(((head & 0xFF) * num) / 100);
            int x = (int)ix, y = (int)iy;
            add_star_pixel (sys, dst + y * stride + x, x, y, col);
            /* at high resolution also thicken the streak a little */
            if (wide_trail)
                add_star_pixel (sys, dst + (y + 1) * stride + x, x, y + 1,
                                scalepix (col, 60, 100));
            ix += sx_step;
            iy += sy_step;
        }
        (void)tail;
        (void)x0; (void)y0;
    }
}

static void draw_rings (struct filter_sys_t *sys,
                        uint32_t *dst, int stride)
{
    float cx = 0.5f * (float)sys->w;
    float cy = 0.5f * (float)sys->h;

    for (int i = 0; i < SPACERIP_MAX_RINGS; i++)
    {
        struct warp_ring *r = &sys->rings[i];
        if (r->strength <= 0.02f)
            continue;

        float rad = r->radius;
        int steps = (int)(6.2832f * rad / 1.6f);
        if (steps < 12) steps = 12;
        if (steps > 4000) steps = 4000;

        unsigned cr, cg, cb;
        hsv2rgb (r->hue, 0.55f, 1.f, &cr, &cg, &cb);
        unsigned alpha = (unsigned)(r->strength * 210.f);
        if (alpha > 255) alpha = 255;
        uint32_t col = (cr * alpha / 255 << 16) | (cg * alpha / 255 << 8)
                       | (cb * alpha / 255);

        float fade_out = 1.f - rad / ((float)sys->w * 1.4f);
        if (fade_out < 0.15f) fade_out = 0.15f;
        col = scalepix (col, (unsigned)(fade_out * 100.f), 100);

        /* line thickness scales with the resolution */
        int lw = sys->w / 1600;
        if (lw < 1) lw = 1;
        if (lw > 3) lw = 3;
        int yoff = (lw - 1) / 2;

        for (int k = 0; k < steps; k++)
        {
            float a = 6.2832f * k / steps;
            int x = (int)(cx + cosf (a) * rad);
            int y = (int)(cy + sinf (a) * rad);
            for (int t = 0; t < lw; t++)
            {
                int yy = y + t - yoff;
                if (x < 0 || yy < 0 || x >= sys->w || yy >= sys->h)
                    continue;
                add_star_pixel (sys, dst + yy * stride + x, x, yy, col);
            }
        }
    }
}

/*****************************************************************************
 * Debug frame dump (SPACERIP_DUMP=/path/prefix)
 *****************************************************************************/
static void dump_bmp (struct filter_sys_t *sys, const uint32_t *src,
                      int stride)
{
    if (sys->dump == NULL || sys->dump_left == 0)
        return;

    char path[1024];
    snprintf (path, sizeof (path), "%s_%03u.bmp", sys->dump,
              sys->frames);
    FILE *f = fopen (path, "wb");
    if (f == NULL)
    {
        sys->dump = NULL;         /* stop trying */
        return;
    }

    int w = sys->w, h = sys->h;
    int row = w * 3, pad = (4 - row % 4) % 4;
    int data = (row + pad) * h;
    uint8_t hdr[54];
    memset (hdr, 0, sizeof (hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    int fs = 54 + data;
    hdr[2] = fs & 0xFF; hdr[3] = (fs >> 8) & 0xFF;
    hdr[4] = (fs >> 16) & 0xFF; hdr[5] = (fs >> 24) & 0xFF;
    hdr[10] = 54; hdr[14] = 40;
    hdr[18] = w & 0xFF; hdr[19] = (w >> 8) & 0xFF;
    hdr[22] = h & 0xFF; hdr[23] = (h >> 8) & 0xFF;
    hdr[26] = 1; hdr[28] = 24;
    hdr[34] = data & 0xFF; hdr[35] = (data >> 8) & 0xFF;
    hdr[36] = (data >> 16) & 0xFF; hdr[37] = (data >> 24) & 0xFF;
    fwrite (hdr, 1, 54, f);

    uint8_t line[3 * 7680 + 4];
    for (int y = h - 1; y >= 0; y--)
    {
        const uint32_t *s = src + y * stride;
        for (int x = 0; x < w; x++)
        {
            uint32_t p = s[x];
            line[x * 3 + 0] = p & 0xFF;            /* B */
            line[x * 3 + 1] = (p >> 8) & 0xFF;     /* G */
            line[x * 3 + 2] = (p >> 16) & 0xFF;    /* R */
        }
        fwrite (line, 1, row, f);
        for (int i = 0; i < pad; i++)
            fputc (0, f);
    }
    fclose (f);

    sys->dump_left--;
    msg_Dbg (sys->p_vout, "SPACERIP frame dumped to %s", path);
}

/*****************************************************************************
 * Render thread
 *****************************************************************************/
static inline void ema (float *p, float v)
{
    *p = (*p <= 0.f) ? v : *p + (v - *p) * 0.15f;
}

static void render_frame (struct filter_sys_t *sys, picture_t *pic)
{
    uint32_t *dst = (uint32_t *)pic->p[0].p_pixels;
    int stride = pic->p[0].i_pitch / 4;

    vlc_tick_t s0 = mdate ();
    /* The background (nebulae, glow, vignette) changes slowly: drawing it
     * on alternate frames is enough, except when a beat makes the flash
     * jump, so that the attack stays immediate. */
    if ((sys->bg_count++ & 1) == 0 || sys->flash > sys->bg_flash + 0.03f)
    {
        sys->bg_flash = sys->flash;
        render_background (sys);
    }
    vlc_tick_t s1 = mdate ();
    upscale_background (sys, dst, stride);
    vlc_tick_t s2 = mdate ();
    draw_stars (sys, dst, stride);
    vlc_tick_t s3 = mdate ();
    draw_rings (sys, dst, stride);
    vlc_tick_t s4 = mdate ();

    ema (&sys->bg_ms, (s1 - s0) / 1000.f);
    ema (&sys->up_ms, (s2 - s1) / 1000.f);
    ema (&sys->st_ms, (s3 - s2) / 1000.f);
    ema (&sys->ri_ms, (s4 - s3) / 1000.f);

    if (sys->dump != NULL && sys->dump_left > 0
     && sys->frames >= 12 && sys->frames <= 66
     && (sys->frames % 6 == 0))
        dump_bmp (sys, dst, stride);
}

static void *Thread (void *data)
{
    struct filter_sys_t *sys = data;
    int canc = vlc_savecancel ();

    vlc_tick_t last = mdate ();
    vlc_tick_t next = last + sys->interval;
    sys->start = last;

    vlc_mutex_lock (&sys->lock);

    while (!sys->b_exit)
    {
        while (!sys->b_exit && mdate () < next)
            vlc_cond_timedwait (&sys->wait, &sys->lock, next);
        if (sys->b_exit)
            break;

        vlc_tick_t now = mdate ();
        float dt = (now - last) / 1000000.f;
        last = now;
        if (dt > 0.1f) dt = 0.1f;
        if (dt < 0.0005f) dt = 0.0005f;
        next = now + sys->interval;

        date_t audio_date = sys->date;
        vlc_tick_t pts_head = date_Get (&audio_date);
        /* How early the audio reaches us compared to the moment the sound
         * card will play it.  The analysis is delayed by the same amount so
         * that pts_an below always refers to the sound heard right now,
         * whatever the output buffering is. */
        vlc_tick_t lead = pts_head - now + sys->interval;
        if (lead < sys->interval) lead = sys->interval;
        size_t lag = (size_t)((uint64_t)lead * (uint64_t)sys->rate / 1000000);
        if (lag + SPACERIP_FFT_N > sys->ring_n)
            lag = sys->ring_n - SPACERIP_FFT_N;
        ring_snapshot (sys, lag);
        vlc_mutex_unlock (&sys->lock);

        /* presentation time of the analysed sound (≈ now) */
        vlc_tick_t pts_an = pts_head
                          - (vlc_tick_t)((double)lag * 1000000. / sys->rate);
        if (pts_an < 0) pts_an = 0;

        vlc_tick_t t0 = mdate ();
        analyze (sys, pts_an);
        update_scene (sys, dt);
        vlc_tick_t t1 = mdate ();

        picture_t *pic = vout_GetPicture (sys->p_vout);
        vlc_tick_t t2 = mdate ();
        if (pic != NULL)
        {
            vlc_tick_t target = pts_an + sys->delay;
            /* Safety: never queue deeper than the picture pool can hold. */
            if (target > now + SPACERIP_MAX_QUEUE)
                target = now + SPACERIP_MAX_QUEUE;
            if (target < now)
                target = now;
            pic->date = target;
            sys->gp_raw = (t2 - t1) / 1000.f;
            sys->pd = (float)((pts_head - now) / 1000);
            render_frame (sys, pic);
            vlc_tick_t t3 = mdate ();
            vout_PutPicture (sys->p_vout, pic);
            vlc_tick_t t4 = mdate ();

            ema (&sys->ana_ms, (t1 - t0) / 1000.f);
            ema (&sys->gp_ms, (t2 - t1) / 1000.f);
            ema (&sys->ren_ms, (t3 - t2) / 1000.f);
            ema (&sys->put_ms, (t4 - t3) / 1000.f);
        }

        sys->frames++;
        if (sys->frames % 15 == 0)
            msg_Dbg (sys->p_vout,
                     "SPACERIP frame %u: speed=%.2f bass=%.3f "
                     "mid=%.3f treble=%.3f warp=%.2f beats=%u bpm=%.0f "
                     "ana=%.1f gp=%.1f (raw %.0f) ren=%.1f put=%.1f "
                     "[bg=%.1f up=%.1f st=%.1f ri=%.1f] pd=%.0f ms",
                     sys->frames, sys->speed, sys->env[0], sys->env[1],
                     sys->env[2], sys->warp, sys->beats, sys->beat_bpm,
                     sys->ana_ms, sys->gp_ms, sys->gp_raw, sys->ren_ms,
                     sys->put_ms, sys->bg_ms, sys->up_ms, sys->st_ms,
                     sys->ri_ms, sys->pd);

        vlc_mutex_lock (&sys->lock);
    }

    vlc_mutex_unlock (&sys->lock);
    vlc_restorecancel (canc);
    return NULL;
}

/*****************************************************************************
 * Audio pipeline (called by the aout from its own thread)
 *****************************************************************************/
static block_t *DoWork (filter_t *p_filter, block_t *p_in_buf)
{
    struct filter_sys_t *sys = p_filter->p_sys;
    const float *in = (const float *)p_in_buf->p_buffer;
    unsigned ch = sys->channels;
    size_t frames = p_in_buf->i_buffer / (sizeof (float) * ch);

    if (ch == 0 || frames == 0)
        return p_in_buf;

    vlc_mutex_lock (&sys->lock);
    size_t w = sys->ring_w;
    for (size_t i = 0; i < frames; i++)
    {
        float acc = 0.f;
        const float *f = in + i * ch;
        for (unsigned c = 0; c < ch; c++)
            acc += f[c];
        sys->ring[w] = acc / (float)ch;
        if (++w >= sys->ring_n)
            w = 0;
    }
    sys->ring_w = w;

    /* presentation clock of the audio we just consumed */
    if (p_in_buf->i_pts > VLC_TICK_INVALID)
        date_Set (&sys->date, p_in_buf->i_pts);
    date_Increment (&sys->date, frames);

    vlc_cond_signal (&sys->wait);
    vlc_mutex_unlock (&sys->lock);

    return p_in_buf;
}

/*****************************************************************************
 * Open / Close
 *****************************************************************************/
static void sys_free (struct filter_sys_t *sys)
{
    free (sys->stars_tab);
    free (sys->grad);
    free (sys->vig);
    free (sys->bg);
    free (sys->ring);
    free (sys->dump);
}

static int Open (vlc_object_t *p_this)
{
    filter_t *p_filter = (filter_t *)p_this;
    struct filter_sys_t *sys = calloc (1, sizeof (*sys));
    if (sys == NULL)
        return VLC_ENOMEM;
    p_filter->p_sys = sys;

    int w = (int)var_InheritInteger (p_filter, "spacerip-width");
    int h = (int)var_InheritInteger (p_filter, "spacerip-height");
    bool auto_w = w <= 0;
    bool auto_h = h <= 0;

    /* 0 = automatic: match the native (maximum) definition of the monitor */
    if (auto_w || auto_h)
    {
        int mw = 0, mh = 0;
        if (detect_monitor_size (&mw, &mh))
            msg_Info (p_filter, "SPACERIP: monitor native resolution "
                      "detected: %dx%d", mw, mh);
        else
        {
            mw = SPACERIP_FALLBACK_W;
            mh = SPACERIP_FALLBACK_H;
            msg_Info (p_filter, "SPACERIP: monitor detection unavailable, "
                      "using %dx%d", mw, mh);
        }

        if (auto_w && auto_h)
        {
            w = mw;
            h = mh;
        }
        else if (auto_w)
            w = (mh > 0) ? (int)((int64_t)h * mw / mh) : mw;
        else
            h = (mw > 0) ? (int)((int64_t)w * mh / mw) : mh;

        /* keep the automatic mode within a fluid 4K budget */
        if (w > SPACERIP_AUTO_MAX_W || h > SPACERIP_AUTO_MAX_H)
        {
            double sx = (double)SPACERIP_AUTO_MAX_W / w;
            double sy = (double)SPACERIP_AUTO_MAX_H / h;
            double s = sx < sy ? sx : sy;
            w = (int)(w * s);
            h = (int)(h * s);
            msg_Info (p_filter, "SPACERIP: auto resolution capped to %dx%d "
                      "to keep the frame rate fluid", w, h);
        }
    }

    w &= ~1;
    h &= ~1;
    if (w < 320) w = 320;
    if (h < 200) h = 200;
    if (w > 7680) w = 7680;
    if (h > 4320) h = 4320;
    msg_Info (p_filter, "SPACERIP: rendering at %dx%d%s", w, h,
              (auto_w || auto_h) ? " (automatic)" : "");
    sys->w = w;
    sys->h = h;
    /* background width is rounded up to a multiple of 4 so the SSE2 path
     * can process full vector rows without a ragged tail */
    sys->bw = (w / 2 + 3) & ~3;
    sys->bh = h / 2;
    sys->stars = (int)var_InheritInteger (p_filter, "spacerip-stars");
    if (sys->stars <= 0)        /* automatic: keep the density of 960x540  */
        sys->stars = (int)((int64_t)w * h / 735);
    if (sys->stars < 50) sys->stars = 50;
    if (sys->stars > 5000) sys->stars = 5000;
    sys->sensitivity = (float)var_InheritFloat (p_filter,
                                                "spacerip-sensitivity");
    sys->spread = (float)var_InheritFloat (p_filter, "spacerip-spread");
    if (sys->sensitivity < 0.1f) sys->sensitivity = 0.1f;
    if (sys->spread < 0.4f) sys->spread = 0.4f;

    int fps = (int)var_InheritInteger (p_filter, "spacerip-fps");
    if (fps < 10) fps = 10;
    if (fps > 60) fps = 60;
    sys->interval = 1000000 / fps;

    int delay_ms = (int)var_InheritInteger (p_filter, "spacerip-delay");
    if (delay_ms < 0) delay_ms = 0;
    if (delay_ms > 2000) delay_ms = 2000;
    sys->delay = (vlc_tick_t)delay_ms * 1000;

    sys->rng = 0x9E3779B9u ^ (unsigned)mdate ();
    if (sys->rng == 0) sys->rng = 1;
    sys->speed = 0.26f;
    sys->hue = 0.55f;
    sys->dump_left = SPACERIP_DUMP_MAX;
    const char *env = getenv ("SPACERIP_DUMP");
    if (env != NULL && *env != '\0')
        sys->dump = strdup (env);

    /* --- video output ------------------------------------------------- */
    video_format_Init (&sys->fmt, VLC_CODEC_RGB32);
    sys->fmt.i_width = sys->fmt.i_visible_width = w;
    sys->fmt.i_height = sys->fmt.i_visible_height = h;
    sys->fmt.i_sar_num = sys->fmt.i_sar_den = 1;
    sys->fmt.b_color_range_full = true;

    sys->p_vout = aout_filter_RequestVout (p_filter, NULL, &sys->fmt);
    if (sys->p_vout == NULL)
    {
        msg_Err (p_filter, "no suitable vout module");
        video_format_Clean (&sys->fmt);
        free (sys->dump);
        free (sys);
        p_filter->p_sys = NULL;
        return VLC_EGENERIC;
    }

    /* optional fullscreen video mode */
    if (var_InheritBool (p_filter, "spacerip-fullscreen"))
    {
        var_SetBool (sys->p_vout, "fullscreen", true);
        msg_Info (p_filter, "SPACERIP: fullscreen video mode");
    }

    /* --- audio --------------------------------------------------------- */
    sys->channels = aout_FormatNbChannels (&p_filter->fmt_in.audio);
    if (sys->channels == 0)
        sys->channels = p_filter->fmt_in.audio.i_channels;
    if (sys->channels == 0)
        sys->channels = 2;
    sys->rate = p_filter->fmt_in.audio.i_rate;
    if (sys->rate <= 0.f)
        sys->rate = 44100.f;

    date_Init (&sys->date, (unsigned)sys->rate, 1);
    date_Set (&sys->date, 0);

    /* band edges: bass < 140 Hz < mid < 2 kHz < treble */
    float bin_hz = sys->rate / SPACERIP_FFT_N;
    unsigned nyq = SPACERIP_FFT_N / 2 - 1;
    sys->bin_lo[0] = (unsigned)(25.f / bin_hz);      if (sys->bin_lo[0] < 1) sys->bin_lo[0] = 1;
    sys->bin_hi[0] = (unsigned)(140.f / bin_hz) + 1;
    sys->bin_lo[1] = sys->bin_hi[0];
    sys->bin_hi[1] = (unsigned)(2000.f / bin_hz) + 1;
    sys->bin_lo[2] = sys->bin_hi[1];
    unsigned top = (unsigned)(14000.f / bin_hz);
    unsigned cap = (unsigned)(0.45f * sys->rate / bin_hz);
    if (top > cap) top = cap;
    if (top > nyq) top = nyq;
    sys->bin_hi[2] = top;
    for (int b = 0; b < 3; b++)
        if (sys->bin_hi[b] <= sys->bin_lo[b])
            sys->bin_hi[b] = sys->bin_lo[b] + 1;
    fft_init_tables (sys);

    /* --- buffers ------------------------------------------------------- */
    /* audio history: long enough to delay the analysis by the time the
     * sound card keeps the audio before playing it (see the Thread) */
    size_t rn = (size_t)(sys->rate * SPACERIP_RING_SEC) + SPACERIP_FFT_N;
    if (rn < SPACERIP_RING_N) rn = SPACERIP_RING_N;
    if (rn > SPACERIP_RING_CAP) rn = SPACERIP_RING_CAP;
    sys->ring_n = rn;
    sys->ring = calloc (rn, sizeof (float));
    sys->bg = calloc ((size_t)sys->bw * sys->bh, sizeof (uint32_t));
    sys->vig = malloc ((size_t)sys->bw * sys->bh);
    sys->grad = malloc ((size_t)sys->bh * 3);
    sys->stars_tab = calloc (sys->stars, sizeof (struct star));

    if (sys->ring == NULL || sys->bg == NULL || sys->vig == NULL
     || sys->grad == NULL || sys->stars_tab == NULL)
    {
        sys_free (sys);
        aout_filter_RequestVout (p_filter, sys->p_vout, NULL);
        video_format_Clean (&sys->fmt);
        p_filter->p_sys = NULL;
        return VLC_ENOMEM;
    }

    /* vertical gradient of deep space */
    for (int y = 0; y < sys->bh; y++)
    {
        float v = (float)y / (float)(sys->bh - 1);
        float r = 3.f + 4.f * v - 3.f * v * v;
        float g = 4.f + 1.f * v - 3.f * v * v;
        float b = 13.f + 7.f * v - 6.f * v * v;
        sys->grad[y * 3 + 0] = (uint8_t)r;
        sys->grad[y * 3 + 1] = (uint8_t)g;
        sys->grad[y * 3 + 2] = (uint8_t)b;
    }

    /* vignette */
    for (int y = 0; y < sys->bh; y++)
        for (int x = 0; x < sys->bw; x++)
        {
            float nx = (x + 0.5f) / sys->bw * 2.f - 1.f;
            float ny = (y + 0.5f) / sys->bh * 2.f - 1.f;
            float d2 = nx * nx + ny * ny;
            float v = 1.f - 0.55f * d2;
            if (v < 0.25f) v = 0.25f;
            sys->vig[y * sys->bw + x] = (uint8_t)(v * 255.f);
        }

    /* stars */
    for (int i = 0; i < sys->stars; i++)
    {
        respawn_star (sys, &sys->stars_tab[i]);
        sys->stars_tab[i].sx = 0.f;
        sys->stars_tab[i].sy = 0.f;
        sys->stars_tab[i].has_prev = false;
    }

    vlc_mutex_init (&sys->lock);
    vlc_cond_init (&sys->wait);
    sys->b_exit = false;

    if (vlc_clone (&sys->thread, Thread, sys, VLC_THREAD_PRIORITY_LOW))
    {
        msg_Err (p_filter, "cannot launch SPACERIP thread");
        vlc_mutex_destroy (&sys->lock);
        vlc_cond_destroy (&sys->wait);
        sys_free (sys);
        aout_filter_RequestVout (p_filter, sys->p_vout, NULL);
        video_format_Clean (&sys->fmt);
        p_filter->p_sys = NULL;
        return VLC_EGENERIC;
    }

    p_filter->fmt_in.audio.i_format = VLC_CODEC_FL32;
    p_filter->fmt_out.audio = p_filter->fmt_in.audio;
    p_filter->pf_audio_filter = DoWork;

    msg_Info (p_filter, "SPACERIP engaged: %dx%d, %d stars, %d fps, "
              "sensitivity %.2f", w, h, sys->stars, fps, sys->sensitivity);
    return VLC_SUCCESS;
}

static void Close (vlc_object_t *p_this)
{
    filter_t *p_filter = (filter_t *)p_this;
    struct filter_sys_t *sys = p_filter->p_sys;

    vlc_mutex_lock (&sys->lock);
    sys->b_exit = true;
    vlc_cond_signal (&sys->wait);
    vlc_mutex_unlock (&sys->lock);

    vlc_join (sys->thread, NULL);

    if (sys->start > 0 && sys->frames > 0)
    {
        float secs = (mdate () - sys->start) / 1000000.f;
        if (secs > 0.f)
            msg_Info (p_filter, "SPACERIP: %u frames in %.1f s "
                      "(%.1f fps average)", sys->frames, secs,
                      sys->frames / secs);
    }

    aout_filter_RequestVout (p_filter, sys->p_vout, NULL);

    vlc_mutex_destroy (&sys->lock);
    vlc_cond_destroy (&sys->wait);
    video_format_Clean (&sys->fmt);
    sys_free (sys);
}
