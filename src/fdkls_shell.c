/*
 * fdkls_shell.c — the package's registry, bindings, and kind.
 *
 * fdkls_shell_open walks the documented add-on recipe (docs/EXTENDING.md
 * §10): FDK owns the display connection; this package binds its OWN
 * wl_registry on it (a second registry is ordinary Wayland — events
 * fan out per listener, FDK's registry is untouched), binds what the
 * layer role needs (wl_compositor, zwlr_layer_shell_v1, every
 * wl_output for by-name targeting), and registers the ONE surface
 * kind the whole package creates. fdkls_shell_close mirrors it:
 * every proxy back, the kind unregistered, the struct freed.
 *
 * The degrade-is-the-contract rule (the D-Bus client's posture,
 * docs/EXTENDING.md §13): every absence here is one log line and a
 * working shell — surfaces created on a shell whose compositor
 * offers no layer-shell global fail with FDK_ERR_UNSUPPORTED, and
 * nothing busy-loops, nothing crashes.
 */

#define FDKLS_LOG_TAG "fdkls"

#include "fdkls_internal.h"

#include <fdkls/fdkls.h>

#include <stdlib.h>
#include <string.h>

/* ---- output names ---------------------------------------------------------
 * wl_output.name (v4) carries the connector name ("DP-1"). It arrives
 * as an ordinary event; one more roundtrip after the binds collects
 * every name. */

static void output_geometry(void *data, struct wl_output *output,
                            int32_t x, int32_t y, int32_t physical_width,
                            int32_t physical_height, int32_t subpixel,
                            const char *make, const char *model,
                            int32_t transform) {
    (void)data; (void)output; (void)x; (void)y; (void)physical_width;
    (void)physical_height; (void)subpixel; (void)make; (void)model;
    (void)transform;
}

static void output_mode(void *data, struct wl_output *output,
                        uint32_t flags, int32_t width, int32_t height,
                        int32_t refresh) {
    (void)data; (void)output; (void)flags; (void)width; (void)height;
    (void)refresh;
}

static void output_done(void *data, struct wl_output *output) {
    (void)data; (void)output;
}

static void output_scale(void *data, struct wl_output *output,
                         int32_t factor) {
    (void)data; (void)output; (void)factor;
}

/* The description event rides the same v4 sequence as name (the
 * compositor sends name THEN description before done) — a listener
 * that handles name but not description aborts the CLIENT on every
 * real compositor ("listener function for opcode 5 of wl_output is
 * NULL"; the mock-compositor suite sends both since the fix). */
static void output_description(void *data, struct wl_output *output,
                               const char *description) {
    (void)data;
    (void)output;
    (void)description; /* diagnostics-only; nothing to record */
}

static void output_name(void *data, struct wl_output *output,
                        const char *name) {
    (void)data;
    /* The proxy arrives here per bound output; find its record. The
     * event only fires on v4+ bindings — matching the version the
     * registry bound, so name stays NULL (never matched) on older
     * compositors. */
    /* (The shell is reachable through the listener's user data; the
     * name event's data pointer is the shell — set below.) */
    fdkls_shell *shell = (fdkls_shell *)data;
    for (fdkls_output *out = shell->outputs; out != NULL;
         out = out->next) {
        if (out->output != output) {
            continue;
        }
        free(out->name);
        out->name = strdup(name);
        if (out->name == NULL) {
            fdk_log(FDK_LOG_WARN, FDKLS_LOG_TAG,
                    "out of memory recording output name '%s' — "
                    "targeting by that name is unavailable", name);
        }
        return;
    }
}

static const struct wl_output_listener g_output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};


/* ---- registry listeners --------------------------------------------------- */

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version) {
    fdkls_shell *shell = (fdkls_shell *)data;
    (void)registry;

    if (strcmp(interface, "wl_compositor") == 0) {
        /* v4, NOT the advertised maximum: every surface this proxy
         * creates inherits its version, and FDK 1.0.0's wl_surface
         * listener speaks only enter/leave — a v5+/v6 surface
         * delivers preferred_buffer_scale/transform (opcodes 2/3)
         * onto NULL handler entries and libwayland aborts the CLIENT
         * at map time ("listener function for opcode 2 of wl_surface
         * is NULL"; every modern compositor sends the pair). FDK caps
         * its own compositor bind at v4 — this package's binding MUST
         * match (the version-alignment rule; the mock advertises v6
         * and sends the pair to pin it). */
        uint32_t v = version < 4 ? version : 4;
        shell->compositor = wl_registry_bind(
            registry, name, &wl_compositor_interface, v);
        if (shell->compositor == NULL) {
            fdk_log(FDK_LOG_ERROR, FDKLS_LOG_TAG,
                    "wl_compositor bind failed (global %u)", name);
        }
        return;
    }

    if (strcmp(interface, "zwlr_layer_shell_v1") == 0) {
        uint32_t v = version < FDKLS_LAYER_SHELL_MAX_VERSION
                         ? version
                         : FDKLS_LAYER_SHELL_MAX_VERSION;
        shell->layer_shell = wl_registry_bind(
            registry, name, &zwlr_layer_shell_v1_interface, v);
        if (shell->layer_shell != NULL) {
            shell->layer_shell_version = v;
            fdk_log(FDK_LOG_INFO, FDKLS_LOG_TAG,
                    "bound zwlr_layer_shell_v1 v%u", v);
        } else {
            fdk_log(FDK_LOG_ERROR, FDKLS_LOG_TAG,
                    "zwlr_layer_shell_v1 bind failed (global %u)", name);
        }
        return;
    }

    if (strcmp(interface, "wl_output") == 0) {
        uint32_t v = version < FDKLS_OUTPUT_MAX_VERSION
                         ? version
                         : FDKLS_OUTPUT_MAX_VERSION;
        struct wl_output *output =
            wl_registry_bind(registry, name, &wl_output_interface, v);
        if (output == NULL) {
            fdk_log(FDK_LOG_WARN, FDKLS_LOG_TAG,
                    "wl_output bind failed (global %u) — by-name "
                    "targeting skips this output", name);
            return;
        }
        fdkls_output *out = (fdkls_output *)malloc(sizeof(*out));
        if (out == NULL) {
            wl_output_destroy(output);
            fdk_log(FDK_LOG_WARN, FDKLS_LOG_TAG,
                    "out of memory tracking wl_output %u — by-name "
                    "targeting skips it", name);
            return;
        }
        out->global_name = name;
        out->output = output;
        out->name = NULL;
        out->next = shell->outputs;
        shell->outputs = out;
        /* The name event (v4+) carries the connector name the
         * by-name targeting matches; the listener's user pointer is
         * the shell (the records live on its list). */
        wl_output_add_listener(output, &g_output_listener, shell);
        return;
    }
}

static void registry_global_remove(void *data,
                                   struct wl_registry *registry,
                                   uint32_t name) {
    (void)registry;
    fdkls_shell *shell = (fdkls_shell *)data;

    /* An output the compositor unplugged: drop its record (the
     * proxy is inert — wl_output carries no destructor request, the
     * local wl_output_destroy in close() is a plain proxy destroy).
     * Surfaces created against it keep their strdup'd names; the
     * compositor sends the ROLE's closed event for surfaces on the
     * removed output, which the surface layer handles. Compositor /
     * layer-shell removal is compositor death — FDK's connection
     * teardown owns that path. */
    fdkls_output **link = &shell->outputs;
    while (*link != NULL && (*link)->global_name != name) {
        link = &(*link)->next;
    }
    if (*link != NULL) {
        fdkls_output *dead = *link;
        *link = dead->next;
        free(dead->name);
        free(dead);
    }
}

static const struct wl_registry_listener g_registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* ---- the shell ------------------------------------------------------------- */

static int count_outputs(const fdkls_shell *shell) {
    int n = 0;
    for (const fdkls_output *o = shell->outputs; o != NULL; o = o->next) {
        n++;
    }
    return n;
}

static int count_surfaces(const fdkls_shell *shell) {
    int n = 0;
    for (const fdkls_surface *s = shell->surfaces; s != NULL; s = s->next) {
        n++;
    }
    return n;
}

fdkls_shell *fdkls__shell_open(fdk_context *ctx) {
    if (ctx == NULL) {
        fdk_log(FDK_LOG_ERROR, FDKLS_LOG_TAG,
                "shell_open: NULL context");
        return NULL;
    }

    /* The extension-API ladder: compile-time (the header's #if, the
     * application's job to carry) AND run-time (here). v3 is what
     * this package's API uses: kinds, popups, widget hosting. */
    if (fdk_ext_api_version() < 3) {
        fdk_log(FDK_LOG_ERROR, FDKLS_LOG_TAG,
                "shell_open: FDK library carries extension API v%d, "
                "this package needs v3+ — upgrade FDK",
                fdk_ext_api_version());
        return NULL;
    }
    if (fdk_get_version() < FDK_VERSION_ENCODE(1, 0, 0)) {
        fdk_log(FDK_LOG_ERROR, FDKLS_LOG_TAG,
                "shell_open: FDK %s is older than the 1.0.0 this "
                "package needs",
                fdk_get_version_string());
        return NULL;
    }

    fdk_ext_backend backend = fdk_ext_active_backend(ctx);
    if (backend != FDK_EXT_BACKEND_WAYLAND) {
        /* The honest X11 posture (docs/design.md §6): layer-shell is
         * a Wayland protocol; on X11 a dock/panel rides EWMH
         * (_NET_WM_WINDOW_TYPE_DOCK + struts) directly through FDK's
         * window API — this package has nothing to add there. */
        fdk_log(FDK_LOG_ERROR, FDKLS_LOG_TAG,
                "shell_open: context is on the %s backend — "
                "layer-shell is Wayland-only (on X11 use EWMH dock "
                "windows directly)",
                backend == FDK_EXT_BACKEND_X11 ? "X11" : "not-connected");
        return NULL;
    }

    fdkls_shell *shell = (fdkls_shell *)calloc(1, sizeof(*shell));
    if (shell == NULL) {
        fdk_log(FDK_LOG_ERROR, FDKLS_LOG_TAG,
                "shell_open: out of memory");
        return NULL;
    }
    shell->ctx = ctx;

    fdk_ext_native_display nd;
    fdk_result r = fdk_ext_get_native_display(ctx, &nd);
    if (!fdk_ok(r) || nd.backend != FDK_EXT_BACKEND_WAYLAND ||
        nd.display == NULL) {
        fdk_log(FDK_LOG_ERROR, FDKLS_LOG_TAG,
                "shell_open: no native Wayland display (%d)", r);
        free(shell);
        return NULL;
    }
    shell->display = (struct wl_display *)nd.display;

    /* The kind FIRST: it is what every surface below registers
     * through. A duplicate registration means shell_open ran twice
     * on this context — the honest refusal (destroy the first
     * shell's surfaces and shell first). */
    fdk_ext_surface_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.struct_size = sizeof(ops);
    ops.create = fdkls__kind_create;
    ops.destroy = fdkls__kind_destroy;
    ops.popup_attached = fdkls__kind_popup_attached;
    r = fdk_ext_register_kind(ctx, FDKLS_KIND_NAME, &ops, shell,
                              &shell->kind);
    if (!fdk_ok(r)) {
        fdk_log(FDK_LOG_ERROR, FDKLS_LOG_TAG,
                "shell_open: registering the surface kind failed (%d) "
                "— a shell is already open on this context?", r);
        free(shell);
        return NULL;
    }

    shell->registry = wl_display_get_registry(shell->display);
    if (shell->registry == NULL) {
        fdk_log(FDK_LOG_ERROR, FDKLS_LOG_TAG,
                "shell_open: wl_display_get_registry failed");
        (void)fdk_ext_kind_destroy(shell->kind);
        free(shell);
        return NULL;
    }
    wl_registry_add_listener(shell->registry, &g_registry_listener,
                             shell);

    /* Roundtrip 1: globals collected, compositor/shell/outputs bound.
     * Roundtrip 2: the outputs' name events collected. FDK's own
     * listeners are dispatch-safe (docs/EXTENDING.md §10) — the trips
     * are harmless at any time. */
    if (wl_display_roundtrip(shell->display) < 0 ||
        wl_display_roundtrip(shell->display) < 0) {
        fdk_log(FDK_LOG_ERROR, FDKLS_LOG_TAG,
                "shell_open: display roundtrip failed — the Wayland "
                "connection is broken");
        (void)fdkls__shell_close(shell);
        return NULL;
    }

    if (shell->compositor == NULL) {
        fdk_log(FDK_LOG_ERROR, FDKLS_LOG_TAG,
                "shell_open: the compositor offers no wl_compositor — "
                "no surfaces can exist");
        (void)fdkls__shell_close(shell);
        return NULL;
    }

    if (shell->layer_shell == NULL) {
        /* Open anyway (degrade-is-the-contract): surfaces created on
         * this shell fail with FDK_ERR_UNSUPPORTED, one log line per
         * attempt. */
        fdk_log(FDK_LOG_WARN, FDKLS_LOG_TAG,
                "shell_open: the compositor does not offer "
                "zwlr_layer_shell_v1 — layer surfaces will refuse "
                "(fdkls_shell_available() reports this)");
    }

    fdk_log(FDK_LOG_INFO, FDKLS_LOG_TAG,
            "shell open: layer-shell %s (v%u), %d output(s) tracked",
            shell->layer_shell != NULL ? "bound" : "absent",
            shell->layer_shell_version,
            count_outputs(shell));

    return shell;
}

fdk_result fdkls__shell_close(fdkls_shell *shell) {
    if (shell == NULL) {
        return FDK_OK;
    }

    /* Live surfaces hold wrappers reachable through the kind's
     * teardown paths — closing under them would dangle every
     * fdkls_surface* the application holds. Refuse, same rule as
     * fdk_ext_kind_destroy. */
    if (shell->surfaces != NULL) {
        fdk_log(FDK_LOG_WARN, FDKLS_LOG_TAG,
                "shell_close: %d live surface(s) — destroy them first",
                count_surfaces(shell));
        return FDK_ERR_INVALID_STATE;
    }

    for (fdkls_output *out = shell->outputs; out != NULL;) {
        fdkls_output *next = out->next;
        wl_output_destroy(out->output);
        free(out->name);
        free(out);
        out = next;
    }
    shell->outputs = NULL;

    if (shell->layer_shell != NULL) {
        if (shell->layer_shell_version >= 3) {
            zwlr_layer_shell_v1_destroy(shell->layer_shell);
        } else {
            wl_proxy_destroy((struct wl_proxy *)shell->layer_shell);
        }
        shell->layer_shell = NULL;
        shell->layer_shell_version = 0;
    }
    if (shell->compositor != NULL) {
        wl_compositor_destroy(shell->compositor);
        shell->compositor = NULL;
    }
    if (shell->registry != NULL) {
        wl_registry_destroy(shell->registry);
        shell->registry = NULL;
    }

    if (shell->kind != NULL) {
        (void)fdk_ext_kind_destroy(shell->kind);
        shell->kind = NULL;
    }
    shell->closed = 1;
    free(shell);
    return FDK_OK;
}

/* ---- option validation -----------------------------------------------------
 *
 * Shared by create and the setters. Every rule here exists because
 * its violation is a CONNECTION-KILLING protocol error on the wire —
 * the compositor's only recourse for a bad layer-surface state is
 * raising the protocol error, which takes down every surface of the
 * connection. Local refusal beats a dead connection, always.
 *
 * `options` may be partially pre-validated (a setter passes a
 * single-purpose copy); checks that depend on unset fields skip
 * through their defaults. Returns FDK_OK with *out_error untouched
 * when clean. */
fdk_result fdkls__validate_options(const fdkls_shell *shell,
                                   const fdkls_surface_options *options,
                                   fdk_result *out_error) {
    if (options->layer < FDKLS_LAYER_BACKGROUND ||
        options->layer > FDKLS_LAYER_OVERLAY) {
        fdk_log(FDK_LOG_ERROR, FDKLS_LOG_TAG,
                "layer %d out of range — the compositor would raise "
                "invalid_layer and kill the connection",
                (int)options->layer);
        *out_error = FDK_ERR_INVALID_ARGUMENT;
        return FDK_ERR_INVALID_ARGUMENT;
    }

    if ((options->anchor & (fdkls_anchor)~0xFu) != 0) {
        fdk_log(FDK_LOG_ERROR, FDKLS_LOG_TAG,
                "anchor 0x%x carries unknown bits — the compositor "
                "would raise invalid_anchor and kill the connection",
                (unsigned)options->anchor);
        *out_error = FDK_ERR_INVALID_ARGUMENT;
        return FDK_ERR_INVALID_ARGUMENT;
    }

    if (options->width < 0 || options->height < 0) {
        fdk_log(FDK_LOG_ERROR, FDKLS_LOG_TAG,
                "negative desired size %dx%d",
                options->width, options->height);
        *out_error = FDK_ERR_INVALID_ARGUMENT;
        return FDK_ERR_INVALID_ARGUMENT;
    }

    /* The zero-size rule (protocol set_size text, verbatim): "You
     * must set your anchor to opposite edges in the dimensions you
     * omit; not doing so is a protocol error." */
    if (options->width == 0 &&
        (options->anchor & (FDKLS_ANCHOR_LEFT | FDKLS_ANCHOR_RIGHT)) !=
            (FDKLS_ANCHOR_LEFT | FDKLS_ANCHOR_RIGHT)) {
        fdk_log(FDK_LOG_ERROR, FDKLS_LOG_TAG,
                "width 0 (compositor-assigned) without left+right "
                "anchored — a protocol error on the wire; anchor both "
                "edges or pick a width");
        *out_error = FDK_ERR_INVALID_ARGUMENT;
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (options->height == 0 &&
        (options->anchor & (FDKLS_ANCHOR_TOP | FDKLS_ANCHOR_BOTTOM)) !=
            (FDKLS_ANCHOR_TOP | FDKLS_ANCHOR_BOTTOM)) {
        fdk_log(FDK_LOG_ERROR, FDKLS_LOG_TAG,
                "height 0 (compositor-assigned) without top+bottom "
                "anchored — a protocol error on the wire; anchor both "
                "edges or pick a height");
        *out_error = FDK_ERR_INVALID_ARGUMENT;
        return FDK_ERR_INVALID_ARGUMENT;
    }

    /* The keyboard mode's range: an out-of-range value reaches the
     * wire as set_keyboard_interactivity(7) — the compositor raises
     * invalid_keyboard_interactivity and kills the connection. */
    if (options->keyboard < FDKLS_KEYBOARD_NONE ||
        options->keyboard > FDKLS_KEYBOARD_ON_DEMAND) {
        fdk_log(FDK_LOG_ERROR, FDKLS_LOG_TAG,
                "keyboard interactivity %d out of range — the "
                "compositor would raise "
                "invalid_keyboard_interactivity and kill the "
                "connection",
                (int)options->keyboard);
        *out_error = FDK_ERR_INVALID_ARGUMENT;
        return FDK_ERR_INVALID_ARGUMENT;
    }

    /* ON_DEMAND is protocol v4 (bounded at bind time — an older
     * compositor never advertises it and the value does not exist on
     * the wire there). */
    if (options->keyboard == FDKLS_KEYBOARD_ON_DEMAND &&
        shell->layer_shell_version < 4) {
        fdk_log(FDK_LOG_ERROR, FDKLS_LOG_TAG,
                "on-demand keyboard needs zwlr_layer_shell_v1 v4, the "
                "compositor offers v%u",
                shell->layer_shell_version);
        *out_error = FDK_ERR_UNSUPPORTED;
        return FDK_ERR_UNSUPPORTED;
    }

    return FDK_OK;
}

/* ---- the public shell API ---------------------------------------------------
 * Thin wrappers: the internal fdkls__* pair does the work (fdkls_surface.c's
 * create path and the kind teardown share it); these four are the documented
 * surface (fdkls.h carries the reference comments). */

fdkls_shell *fdkls_shell_open(fdk_context *ctx) {
    return fdkls__shell_open(ctx);
}

fdk_result fdkls_shell_close(fdkls_shell *shell) {
    return fdkls__shell_close(shell);
}

int fdkls_shell_available(const fdkls_shell *shell) {
    return shell != NULL && shell->layer_shell != NULL;
}

uint32_t fdkls_shell_protocol_version(const fdkls_shell *shell) {
    return shell != NULL ? shell->layer_shell_version : 0;
}
