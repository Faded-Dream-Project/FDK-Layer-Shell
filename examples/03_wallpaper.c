/* 03_wallpaper.c — the background layer, drawn directly.
 *
 * The third shape: a BACKGROUND-layer surface anchored to all four
 * edges with the exclusive zone at -1 ("extend below/over the
 * panel" — the wallpaper/lock-screen posture from the protocol's
 * own set_exclusive_zone text). No widgets: this example is the
 * DIRECT-rendering shape — an add-on that owns its pixels draws
 * through the public fdk_surface path and obeys the EXPOSE
 * discipline (docs/EXTENDING.md §5): every configure and every
 * expose answers with a redraw + present. A repeating timer drives
 * a slow animated gradient so the surface visibly stays alive.
 *
 *   run: ./03_wallpaper
 *
 * (A real wallpaper would blit an image — fdk_surface_blit — at the
 * configured size; the gradient keeps the example dependency-free.
 * Ctrl+C quits.)
 */

#include "fdk/fdk.h"
#include <fdkls/fdkls.h>

#include <stdio.h>
#include <string.h>

static fdk_context *g_ctx;
static fdkls_shell *g_shell;
static fdkls_surface *g_wall;
static long g_frame;

static void paint_wall(fdkls_surface *wall) {
    fdk_surface *s = NULL;
    if (!fdk_ok(fdkls_surface_get_surface(wall, &s)) || s == NULL) {
        return;
    }
    fdk_surface_info info;
    (void)fdk_surface_get_info(s, &info);

    /* A vertical gradient drifting in hue: row bands, cheap and
     * honest — the point is the lifecycle, not the art. */
    for (int y = 0; y < info.height; y++) {
        float t = (float)y /
                   (float)(info.height > 1 ? info.height - 1 : 1);
        float drift = (float)(g_frame % 240) / 240.0f;
        float r = 0.10f + 0.16f * t + 0.05f * drift;
        float g = 0.07f + 0.10f * (1.0f - t);
        float b = 0.16f + 0.22f * t;
        fdk_surface_fill_rect(s, (fdk_rect){0, y, info.width, 1},
                              (fdk_color){r, g, b, 1.0f});
    }
    /* One moving highlight band so animation is obvious. */
    int band_y = (int)((float)(g_frame % 180) / 180.0f *
                       (float)info.height);
    fdk_surface_fill_rect(s,
                          (fdk_rect){0, band_y, info.width, 2},
                          (fdk_color){0.35f, 0.30f, 0.55f, 0.6f});
    fdk_surface_present(s);
    g_frame++;
}

static void wall_event(fdk_ext_surface *surface,
                       const fdk_event_data *event, void *user) {
    (void)surface;
    (void)user;
    switch (event->type) {
    case FDK_EVENT_WINDOW_CONFIGURE:
        /* The compositor assigned the full output size (all four
         * edges anchored): repaint at the new size now. */
        paint_wall(g_wall);
        break;
    case FDK_EVENT_WINDOW_EXPOSE:
        /* EXPOSE is a repaint OBLIGATION on a directly-driven
         * surface (the retained-damage model concludes nothing
         * changed otherwise) — the same discipline example 19
         * demonstrates. */
        paint_wall(g_wall);
        break;
    default:
        break;
    }
}

static void animate(fdk_timer *timer, void *user) {
    (void)timer;
    (void)user;
    if (g_wall != NULL) {
        paint_wall(g_wall);
    }
}

static void wall_closed(fdkls_surface *surface, void *user) {
    (void)surface;
    (void)user;
    printf("fdk-layershell example: wallpaper withdrawn\n");
    fdkls_surface_destroy(g_wall);
    g_wall = NULL;
    fdk_quit(g_ctx);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    fdk_context *ctx = NULL;
    fdk_result r = fdk_init(&ctx, NULL);
    if (!fdk_ok(r)) {
        fprintf(stderr,
                "03_wallpaper: no display connection — this example "
                "needs a Wayland session\n");
        return 1;
    }
    g_ctx = ctx;

    fdkls_shell *shell = fdkls_shell_open(ctx);
    if (shell == NULL || !fdkls_shell_available(shell)) {
        fprintf(stderr,
                "03_wallpaper: no Wayland layer-shell on this "
                "context — nothing to put a wallpaper on\n");
        if (shell != NULL) {
            fdkls_shell_close(shell);
        }
        fdk_shutdown(ctx);
        return 1;
    }
    g_shell = shell;

    /* BACKGROUND layer, all four edges, 0x0 (compositor-assigned —
     * legal ONLY with both edges anchored per axis), zone -1 (the
     * wallpaper posture: stretch below/over every reservation). */
    fdkls_surface_options opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.layer = FDKLS_LAYER_BACKGROUND;
    opts.anchor = (fdkls_anchor)(FDKLS_ANCHOR_TOP |
                                 FDKLS_ANCHOR_BOTTOM |
                                 FDKLS_ANCHOR_LEFT |
                                 FDKLS_ANCHOR_RIGHT);
    opts.width = 0;
    opts.height = 0;
    opts.exclusive_zone = -1;
    opts.keyboard = FDKLS_KEYBOARD_NONE;
    opts.namespace = "fdkls-example-wallpaper";

    fdkls_surface *wall = NULL;
    r = fdkls_surface_create(shell, &opts, &wall);
    if (!fdk_ok(r)) {
        fprintf(stderr, "03_wallpaper: surface create failed (%d)\n",
                r);
        fdkls_shell_close(shell);
        fdk_shutdown(ctx);
        return 1;
    }
    g_wall = wall;

    fdkls_surface_listener listener;
    memset(&listener, 0, sizeof(listener));
    listener.struct_size = sizeof(listener);
    listener.closed = wall_closed;
    fdkls_surface_set_listener(wall, &listener, NULL);
    fdkls_surface_set_event_callback(wall, wall_event, NULL);

    fdkls_surface_show(wall);

    fdk_timer_add(ctx, 50, true, animate, NULL);

    printf("fdk-layershell example: wallpaper up (BACKGROUND, all "
           "edges, zone -1) — Ctrl+C quits\n");
    fdk_run(ctx);

    if (g_wall != NULL) {
        fdkls_surface_destroy(g_wall);
        g_wall = NULL;
    }
    fdkls_shell_close(shell);
    fdk_shutdown(ctx);
    printf("fdk-layershell example: clean exit\n");
    return 0;
}
