/* 01_panel_bar.c — the classic panel bar, on FDK-Layer-Shell.
 *
 * The surface every shell add-on exists to build: a TOP-layer bar
 * anchored across the top edge, an exclusive zone so maximized
 * windows stop above it, and real widgets on it — a clock (a
 * repeating fdk_timer drives the label) and a Menu button (an
 * fdk_ext_popup dropdown, adopted by the layer role through the
 * popup_attached hook). The whole widget catalog lands on the bar
 * through fdkls_surface_host_widgets (extension API v3): configure
 * events re-arrange the tree automatically, pointer events dispatch
 * with the standard widget semantics.
 *
 *   run: ./01_panel_bar   (a Wayland compositor with layer-shell:
 *                          sway, labwc, river, Hyprland, wayfire)
 *
 * The bar closes when the compositor withdraws it (closed listener)
 * — quit, clean teardown, shell_close, shutdown.
 */

#include "fdk/fdk.h"
#include <fdkls/fdkls.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

static fdk_context *g_ctx;
static fdkls_shell *g_shell;
static fdkls_surface *g_bar;
static fdk_widget *g_clock;
static fdk_font *g_font;
static fdk_ext_surface *g_menu; /* the open dropdown, or NULL */

static void clock_tick(fdk_timer *timer, void *user) {
    (void)timer;
    (void)user;
    if (g_clock == NULL) {
        return;
    }
    char buf[32];
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(buf, sizeof buf, "%H:%M:%S", &tm_now);
    fdk_label_set_text(g_clock, buf);
}

static void menu_close(fdk_ext_surface *popup,
                       const fdk_event_data *event, void *user) {
    (void)user;
    /* The popup contract: outside click or Escape arrives as
     * FDK_EVENT_WINDOW_CLOSE_REQUEST on the POPUP's callback — the
     * application responds by destroying it. */
    if (event->type == FDK_EVENT_WINDOW_CLOSE_REQUEST) {
        fdk_ext_surface_destroy(popup);
        if (popup == g_menu) {
            g_menu = NULL;
        }
    }
}

static void on_menu_button(fdk_widget *button, void *user) {
    (void)button;
    (void)user;
    if (g_menu != NULL) {
        return; /* one dropdown at a time */
    }
    fdk_ext_surface *pop = NULL;
    if (!fdk_ok(fdk_ext_popup_create(fdkls_surface_get_ext(g_bar),
                                     8, 40, 220, 120, &pop))) {
        return;
    }
    g_menu = pop;
    fdk_ext_surface_set_event_callback(pop, menu_close, NULL);
    fdk_surface *s = NULL;
    if (fdk_ok(fdk_ext_surface_get_surface(pop, &s)) && s != NULL) {
        fdk_surface_fill(s, (fdk_color){0.10f, 0.09f, 0.14f, 1});
        fdk_surface_fill_rect(s, (fdk_rect){8, 8, 204, 104},
                              (fdk_color){0.16f, 0.15f, 0.22f, 1});
        fdk_surface_present(s);
    }
    fdk_ext_surface_show(pop);
}

static void bar_closed(fdkls_surface *surface, void *user) {
    (void)surface;
    (void)user;
    /* The compositor withdrew the bar (output gone, user removal):
     * the role is dead — destroy and leave. */
    printf("fdk-layershell example: bar closed by the compositor\n");
    fdkls_surface_destroy(g_bar);
    g_bar = NULL;
    fdk_quit(g_ctx);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    fdk_context *ctx = NULL;
    fdk_result r = fdk_init(&ctx, NULL);
    if (!fdk_ok(r)) {
        fprintf(stderr,
                "01_panel_bar: no display connection — this example "
                "needs a Wayland session\n");
        return 1;
    }
    g_ctx = ctx;

    fdkls_shell *shell = fdkls_shell_open(ctx);
    if (shell == NULL) {
        fprintf(stderr,
                "01_panel_bar: FDK-Layer-Shell refused this context "
                "(X11 session, or no Wayland connection) — layer "
                "surfaces need a Wayland compositor\n");
        fdk_shutdown(ctx);
        return 1;
    }
    if (!fdkls_shell_available(shell)) {
        fprintf(stderr,
                "01_panel_bar: the compositor does not offer "
                "zwlr_layer_shell_v1 — no panels here\n");
        fdkls_shell_close(shell);
        fdk_shutdown(ctx);
        return 1;
    }
    g_shell = shell;

    fdk_font *font = fdk_font_load_system_default(14);
    g_font = font;

    /* The bar: TOP layer, top edge, width compositor-assigned (both
     * left+right anchored), height 32, exclusive zone 32. */
    fdkls_surface_options opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.layer = FDKLS_LAYER_TOP;
    opts.anchor = (fdkls_anchor)(FDKLS_ANCHOR_TOP |
                                 FDKLS_ANCHOR_LEFT |
                                 FDKLS_ANCHOR_RIGHT);
    opts.height = 32;
    opts.exclusive_zone = 32;
    opts.keyboard = FDKLS_KEYBOARD_NONE;
    opts.namespace = "fdkls-example-bar";

    fdkls_surface *bar = NULL;
    r = fdkls_surface_create(shell, &opts, &bar);
    if (!fdk_ok(r)) {
        fprintf(stderr, "01_panel_bar: surface create failed (%d)\n",
                r);
        fdkls_shell_close(shell);
        fdk_shutdown(ctx);
        return 1;
    }
    g_bar = bar;

    fdkls_surface_listener listener;
    memset(&listener, 0, sizeof(listener));
    listener.struct_size = sizeof(listener);
    listener.closed = bar_closed;
    fdkls_surface_set_listener(bar, &listener, NULL);

    /* The widgets: a dark root, the clock on the left, the menu
     * button on the right. Configure events re-arrange the tree on
     * every compositor-assigned width. */
    fdk_widget *root = NULL;
    if (!fdk_ok(fdkls_surface_host_widgets(bar, &root))) {
        fprintf(stderr, "01_panel_bar: widget hosting failed\n");
        fdkls_surface_destroy(bar);
        fdkls_shell_close(shell);
        fdk_shutdown(ctx);
        return 1;
    }
    fdk_widget_set_background(root,
                              (fdk_color){0.08f, 0.07f, 0.12f, 1});

    fdk_widget *clock_label = NULL;
    (void)fdk_label_create(root, font, "--:--:--", &clock_label);
    fdk_widget_set_bounds(clock_label, (fdk_rect){12, 6, 110, 20});
    g_clock = clock_label;

    fdk_widget *menu_button = NULL;
    (void)fdk_button_create(root, font, "Menu", &menu_button);
    /* The button's x follows the compositor-assigned width only
     * after the first configure; 640 is a sane first-frame spot and
     * the re-arrange corrects it (hosted trees follow configures). */
    fdk_widget_set_bounds(menu_button, (fdk_rect){620, 4, 64, 24});
    fdk_button_set_on_activate(menu_button, on_menu_button, NULL);

    /* The handshake: the initial empty commit; the compositor
     * assigns the width, the package acks + reports, the tree
     * re-arranges, the first frame maps the bar. */
    fdkls_surface_show(bar);

    fdk_timer_add(ctx, 1000, true, clock_tick, NULL);
    clock_tick(NULL, NULL);

    printf("fdk-layershell example: bar up on namespace "
           "'fdkls-example-bar' — Menu opens a popup, the clock "
           "ticks, Ctrl+C quits\n");
    fdk_run(ctx);

    if (g_menu != NULL) {
        fdk_ext_surface_destroy(g_menu);
        g_menu = NULL;
    }
    if (g_bar != NULL) {
        fdkls_surface_destroy(g_bar);
        g_bar = NULL;
    }
    fdkls_shell_close(shell);
    fdk_shutdown(ctx);
    printf("fdk-layershell example: clean exit\n");
    return 0;
}
