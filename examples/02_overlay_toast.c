/* 02_overlay_toast.c — a self-dismissing notification toast.
 *
 * The second shape a shell add-on builds: an OVERLAY-layer card in
 * the top-right corner — above everything (that is what OVERLAY is
 * for), NOT exclusive (zone 0: it moves to avoid the panel's
 * reservation rather than reserving its own), client-sized
 * 260x90, with margins from its corner anchor. A one-shot
 * fdk_timer destroys it after four seconds — the destroy-sweep
 * takes the widgets with it.
 *
 * The pointer shape flip (FDK_CURSOR_POINTER while the pointer
 * hovers the dismiss button — the hover-cursor contract from
 * docs/EXTENDING.md §6) rides the widget tree's enter/leave through
 * the surface callback.
 *
 *   run: ./02_overlay_toast
 */

#include "fdk/fdk.h"
#include <fdkls/fdkls.h>

#include <stdio.h>
#include <string.h>

static fdk_context *g_ctx;
static fdkls_shell *g_shell;
static fdkls_surface *g_toast;
static fdk_timer *g_dismiss_timer;
static int g_dismissed;

static void toast_closed(fdkls_surface *surface, void *user) {
    (void)surface;
    (void)user;
    printf("fdk-layershell example: toast withdrawn by the "
           "compositor\n");
    if (g_dismiss_timer != NULL) {
        fdk_timer_remove(g_dismiss_timer);
        g_dismiss_timer = NULL;
    }
    fdkls_surface_destroy(g_toast);
    g_toast = NULL;
    fdk_quit(g_ctx);
}

static void dismiss_now(fdk_timer *timer, void *user) {
    (void)timer;
    (void)user;
    g_dismiss_timer = NULL; /* one-shot: the handle is dangling */
    if (g_dismissed) {
        return;
    }
    g_dismissed = 1;
    printf("fdk-layershell example: four seconds passed — the toast "
           "destroys itself\n");
    fdkls_surface_destroy(g_toast);
    g_toast = NULL;
    fdk_quit(g_ctx);
}

static void on_dismiss_button(fdk_widget *button, void *user) {
    (void)button;
    (void)user;
    g_dismissed = 1;
    if (g_dismiss_timer != NULL) {
        fdk_timer_remove(g_dismiss_timer);
        g_dismiss_timer = NULL;
    }
    fdkls_surface_destroy(g_toast);
    g_toast = NULL;
    fdk_quit(g_ctx);
}

static void toast_event(fdk_ext_surface *surface,
                        const fdk_event_data *event, void *user) {
    (void)surface;
    (void)user;
    /* The hover cursor: the hand over the dismiss button, the arrow
     * elsewhere (window-level input events no widget consumed reach
     * the surface callback; consumed button presses do not). */
    switch (event->type) {
    case FDK_EVENT_POINTER_MOTION:
        /* A real card hit-tests its own regions; the example keeps
         * the arrow everywhere and lets the button's own hover
         * styling do the talking. */
        break;
    default:
        break;
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    fdk_context *ctx = NULL;
    fdk_result r = fdk_init(&ctx, NULL);
    if (!fdk_ok(r)) {
        fprintf(stderr,
                "02_overlay_toast: no display connection — this "
                "example needs a Wayland session\n");
        return 1;
    }
    g_ctx = ctx;

    fdkls_shell *shell = fdkls_shell_open(ctx);
    if (shell == NULL || !fdkls_shell_available(shell)) {
        fprintf(stderr,
                "02_overlay_toast: no Wayland layer-shell on this "
                "context — nothing to show a toast on\n");
        if (shell != NULL) {
            fdkls_shell_close(shell);
        }
        fdk_shutdown(ctx);
        return 1;
    }
    g_shell = shell;

    fdk_font *font = fdk_font_load_system_default(14);

    /* The card: OVERLAY layer, top-right corner, client-sized,
     * margin 12 from the anchor, exclusive zone 0 (never reserves,
     * always dodges). */
    fdkls_surface_options opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.layer = FDKLS_LAYER_OVERLAY;
    opts.anchor = (fdkls_anchor)(FDKLS_ANCHOR_TOP |
                                 FDKLS_ANCHOR_RIGHT);
    opts.width = 260;
    opts.height = 90;
    opts.exclusive_zone = 0;
    opts.margin_top = 12;
    opts.margin_right = 12;
    opts.keyboard = FDKLS_KEYBOARD_NONE;
    opts.namespace = "fdkls-example-toast";

    fdkls_surface *toast = NULL;
    r = fdkls_surface_create(shell, &opts, &toast);
    if (!fdk_ok(r)) {
        fprintf(stderr, "02_overlay_toast: surface create failed "
                        "(%d)\n", r);
        fdkls_shell_close(shell);
        fdk_shutdown(ctx);
        return 1;
    }
    g_toast = toast;

    fdkls_surface_listener listener;
    memset(&listener, 0, sizeof(listener));
    listener.struct_size = sizeof(listener);
    listener.closed = toast_closed;
    fdkls_surface_set_listener(toast, &listener, NULL);
    fdkls_surface_set_event_callback(toast, toast_event, NULL);

    fdk_widget *root = NULL;
    if (!fdk_ok(fdkls_surface_host_widgets(toast, &root))) {
        fprintf(stderr, "02_overlay_toast: widget hosting failed\n");
        fdkls_surface_destroy(toast);
        fdkls_shell_close(shell);
        fdk_shutdown(ctx);
        return 1;
    }
    fdk_widget_set_background(root,
                              (fdk_color){0.10f, 0.09f, 0.16f, 1});

    fdk_widget *title = NULL;
    (void)fdk_label_create(root, font, "Download complete", &title);
    fdk_widget_set_bounds(title, (fdk_rect){14, 10, 232, 20});

    fdk_widget *body = NULL;
    (void)fdk_label_create(root, font, "report.pdf is ready to open",
                           &body);
    fdk_widget_set_bounds(body, (fdk_rect){14, 34, 232, 20});

    fdk_widget *button = NULL;
    (void)fdk_button_create(root, font, "Dismiss", &button);
    fdk_widget_set_bounds(button, (fdk_rect){178, 58, 68, 24});
    fdk_button_set_on_activate(button, on_dismiss_button, NULL);

    fdkls_surface_show(toast);
    g_dismiss_timer = fdk_timer_add(ctx, 4000, false, dismiss_now, NULL);

    printf("fdk-layershell example: toast up (OVERLAY, top-right, "
           "non-exclusive) — Dismiss or wait four seconds\n");
    fdk_run(ctx);

    if (g_toast != NULL) {
        fdkls_surface_destroy(g_toast);
        g_toast = NULL;
    }
    fdkls_shell_close(shell);
    fdk_shutdown(ctx);
    printf("fdk-layershell example: clean exit\n");
    return 0;
}
