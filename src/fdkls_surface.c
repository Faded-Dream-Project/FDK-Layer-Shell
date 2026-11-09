/*
 * fdkls_surface.c — the layer-surface kind and the public surface API.
 *
 * The kind's create callback is the whole Wayland side of the
 * package: build the wl_surface through the shell's own
 * wl_compositor binding, assign the zwlr_layer_surface_v1 role
 * through the shell's own layer-shell binding, apply the options,
 * and hand the wl_surface to FDK — which adopts it (the render
 * machinery, the event routing, popups, widget hosting) and enforces
 * the protocol's "no buffer before the first configure" rule through
 * its present gate (docs/EXTENDING.md §4's buffer-side note).
 *
 * The commit discipline (fdk_extend.h, verbatim): "do NOT commit in
 * create. Your role's initial configure handshake comes first ... For
 * a shell-surface role: create the role, set your parameters, and
 * commit an EMPTY surface at show-time to start the configure."
 * fdkls_surface_show() is that commit — FDK's fdk_ext_surface_show()
 * is a documented no-op on Wayland, so the package owns the one
 * handshake-opening empty commit.
 *
 * After the handshake every state change goes out with an applying
 * empty commit (the same shape FDK's subsurface operations use for
 * parent-managed state) — legal any time after the first configure,
 * invisible to the application's frame pacing.
 */

#define FDKLS_LOG_TAG "fdkls"

#include "fdkls_internal.h"

#include <fdkls/fdkls.h>

#include <stdlib.h>
#include <string.h>

/* ---- the role listener ----------------------------------------------------- */

static void role_configure(void *data,
                           struct zwlr_layer_surface_v1 *role,
                           uint32_t serial, uint32_t width,
                           uint32_t height) {
    fdkls_surface *s = (fdkls_surface *)data;
    if (s == NULL || s->closed) {
        /* A configure for a withdrawn surface: the compositor sent
         * it before our ack of `closed` landed — nothing to apply
         * (and no state of ours left to carry it). */
        return;
    }

    /* The ack comes FIRST — fdk_ext_surface_notify_configure may
     * commit the deferred first frame post-ack on the same dispatch,
     * and the protocol demands the ack before any commit answering
     * this configure. */
    zwlr_layer_surface_v1_ack_configure(role, serial);

    /* Assigned size: nonzero is the compositor's assignment (on a
     * both-edges-anchored axis it is authoritative); zero means
     * "client decides" on that axis — fall back to the desired size
     * (fdkls_surface_options.width/height), which validation
     * guarantees is a real size on a client-decided axis. */
    int32_t w = width > 0 && width <= (uint32_t)INT32_MAX
                    ? (int32_t)width
                    : s->opts.width;
    int32_t h = height > 0 && height <= (uint32_t)INT32_MAX
                    ? (int32_t)height
                    : s->opts.height;
    if (w < 1) {
        w = 1; /* degenerate last resort — never 0: the render
                * machinery's buffer math divides by it */
    }
    if (h < 1) {
        h = 1;
    }

    /* FDK records the size, resizes its render machinery, and
     * delivers FDK_EVENT_WINDOW_CONFIGURE (re-arranging the widget
     * tree, if hosted). The add-on's own EXPOSE discipline then
     * repaints. */
    (void)fdk_ext_surface_notify_configure(s->ext, w, h);
}

static void role_closed(void *data,
                        struct zwlr_layer_surface_v1 *role) {
    fdkls_surface *s = (fdkls_surface *)data;
    (void)role;
    if (s == NULL || s->closed) {
        return;
    }
    s->closed = 1;

    /* The role proxy STAYS ALIVE. The protocol: "Further changes to
     * the surface will be ignored" — committing a closed layer
     * surface is legal, while destroying the role would leave the
     * FDK-owned wl_surface role-less and one stray present from a
     * connection-killing protocol error. The kind's destroy tears
     * the role down in the end (the only correct order). */

    fdk_log(FDK_LOG_INFO, FDKLS_LOG_TAG,
            "surface closed by the compositor (output gone or "
            "user-removed) — the 'closed' listener owns cleanup");

    if (s->listener.closed != NULL) {
        s->listener.closed(s, s->listener_user);
    }
}

static const struct zwlr_layer_surface_v1_listener g_role_listener = {
    .configure = role_configure,
    .closed = role_closed,
};

/* ---- the kind's three callbacks --------------------------------------------- */

fdkls_surface *fdkls__surface_find(const fdkls_shell *shell,
                                   const fdk_ext_surface *ext) {
    if (shell == NULL) {
        return NULL;
    }
    for (fdkls_surface *s = shell->surfaces; s != NULL; s = s->next) {
        if ((const fdk_ext_surface *)s->ext == ext) {
            return s;
        }
    }
    return NULL;
}

fdk_result fdkls__kind_create(fdk_ext_surface *surface,
                              fdk_ext_native_handle *out_native,
                              void *user) {
    (void)surface; /* the wrapper arrives through the pending slot —
                    * the ext surface is FDK's, adopted after this
                    * returns */
    fdkls_shell *shell = (fdkls_shell *)user;
    fdkls_surface *s = shell->pending; /* stashed by
                                        * fdkls_surface_create */
    shell->pending = NULL;
    if (s == NULL) {
        fdk_log(FDK_LOG_ERROR, FDKLS_LOG_TAG,
                "kind create: no pending surface (internal error)");
        return FDK_ERR_WINDOW_CREATE;
    }

    if (shell->compositor == NULL || shell->layer_shell == NULL) {
        fdk_log(FDK_LOG_ERROR, FDKLS_LOG_TAG,
                "layer surface create: the compositor offers no "
                "layer-shell (fdkls_shell_available() reports this)");
        return FDK_ERR_UNSUPPORTED;
    }

    struct wl_surface *wls = wl_compositor_create_surface(
        shell->compositor);
    if (wls == NULL) {
        fdk_log(FDK_LOG_ERROR, FDKLS_LOG_TAG,
                "wl_compositor_create_surface failed");
        return FDK_ERR_WINDOW_CREATE;
    }

    /* The role FIRST state: get_layer_surface assigns it before any
     * commit could exist (a committed wl_surface is a client error
     * here — nothing has committed yet, by construction). The output
     * proxy rides from the by-name match (NULL = compositor picks).
     * The namespace string is copied at create; the protocol copies
     * it on the wire. */
    struct zwlr_layer_surface_v1 *role =
        zwlr_layer_shell_v1_get_layer_surface(
            shell->layer_shell, wls, s->output,
            (uint32_t)s->opts.layer, s->opts.namespace);
    if (role == NULL) {
        fdk_log(FDK_LOG_ERROR, FDKLS_LOG_TAG,
                "get_layer_surface failed");
        wl_surface_destroy(wls);
        return FDK_ERR_WINDOW_CREATE;
    }
    zwlr_layer_surface_v1_add_listener(role, &g_role_listener, s);

    /* The parameters (double-buffered state — all of it applies with
     * the initial commit show() sends). set_size only when a size is
     * desired: 0x0 is the protocol default and the both-edges rule
     * makes the compositor assign the axis itself. */
    zwlr_layer_surface_v1_set_anchor(role,
                                     (uint32_t)s->opts.anchor);
    zwlr_layer_surface_v1_set_exclusive_zone(
        role, s->opts.exclusive_zone);
    zwlr_layer_surface_v1_set_margin(role, s->opts.margin_top,
                                     s->opts.margin_right,
                                     s->opts.margin_bottom,
                                     s->opts.margin_left);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        role, (uint32_t)s->opts.keyboard);
    if (s->opts.width > 0 || s->opts.height > 0) {
        zwlr_layer_surface_v1_set_size(role,
                                       (uint32_t)s->opts.width,
                                       (uint32_t)s->opts.height);
    }

    /* NO COMMIT here — the handshake opener is fdkls_surface_show().
     * FDK's present gate holds any eager buffer until the role's
     * first configure has been acked and reported. */

    s->wl_surface = wls;
    s->role = role;
    s->next = shell->surfaces;
    shell->surfaces = s;

    out_native->backend = FDK_EXT_BACKEND_WAYLAND;
    out_native->pointer = (void *)wls;

    fdk_log(FDK_LOG_INFO, FDKLS_LOG_TAG,
            "layer surface created: layer %d, anchor 0x%x, %dx%d, "
            "namespace '%s', output %s",
            (int)s->opts.layer, (unsigned)s->opts.anchor,
            s->opts.width, s->opts.height, s->opts.namespace,
            s->output_name[0] != '\0' ? s->output_name
                                      : "(compositor picks)");
    return FDK_OK;
}

void fdkls__kind_destroy(fdk_ext_surface *surface, void *user) {
    fdkls_shell *shell = (fdkls_shell *)user;
    fdkls_surface *s = fdkls__surface_find(shell, surface);
    if (s == NULL) {
        /* Not ours (or torn down twice) — the ext surface's sweep
         * path can reach here after a direct destroy already freed
         * the wrapper; nothing left to do. */
        return;
    }

    /* Role teardown, against the still-live native surface (FDK
     * destroys the wl_surface next — fdk_extend.h's lifetime
     * block). The role proxy dies here even on a closed surface:
     * after the kind's destroy, FDK's teardown is the only writer
     * left, and the surface is going away entirely. */
    if (s->role != NULL) {
        zwlr_layer_surface_v1_destroy(s->role);
        s->role = NULL;
    }
    /* wl_surface: FDK's (never destroyed here). */

    /* Unlink from the shell's list. */
    fdkls_surface **link = &shell->surfaces;
    while (*link != NULL && *link != s) {
        link = &(*link)->next;
    }
    if (*link == s) {
        *link = s->next;
    }

    free(s->output_name);
    free((char *)s->opts.namespace);
    free(s);

    fdk_log(FDK_LOG_DEBUG, FDKLS_LOG_TAG,
            "layer surface destroyed (role torn down, wrapper freed)");
}

void fdkls__kind_popup_attached(fdk_ext_surface *surface,
                                void *native_popup, void *user) {
    fdkls_shell *shell = (fdkls_shell *)user;
    fdkls_surface *s = fdkls__surface_find(shell, surface);
    if (s == NULL || s->role == NULL || s->closed) {
        /* A popup on a withdrawn surface: FDK's grab still applies
         * (the popup stands on it alone, the documented fallback). */
        return;
    }
    /* The shell-surface pattern (docs/EXTENDING.md §4): the role
     * adopts the xdg_popup through its own get_popup request.
     * native_popup is the struct xdg_popup* — the one place
     * wayland-client types appear in a caller's hands. */
    zwlr_layer_surface_v1_get_popup(
        s->role, (struct xdg_popup *)native_popup);
    if (s->shown) {
        /* The adoption is double-buffered parent state; an applying
         * commit puts it on the wire now. */
        wl_surface_commit(s->wl_surface);
    }
}

/* ---- the public surface API --------------------------------------------------- */

static const char *output_lookup(const fdkls_shell *shell,
                                 const char *name,
                                 struct wl_output **out_proxy) {
    for (const fdkls_output *o = shell->outputs; o != NULL;
         o = o->next) {
        if (o->name != NULL && strcmp(o->name, name) == 0) {
            *out_proxy = o->output;
            return o->name;
        }
    }
    return NULL;
}

fdk_result fdkls_surface_create(fdkls_shell *shell,
                                const fdkls_surface_options *options,
                                fdkls_surface **out_surface) {
    if (out_surface != NULL) {
        *out_surface = NULL;
    }
    if (shell == NULL || out_surface == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (shell->closed) {
        fdk_log(FDK_LOG_ERROR, FDKLS_LOG_TAG,
                "surface create: the shell is closed");
        return FDK_ERR_INVALID_STATE;
    }
    if (options == NULL || options->struct_size < sizeof(*options)) {
        fdk_log(FDK_LOG_ERROR, FDKLS_LOG_TAG,
                "surface create: options NULL or struct_size %zu < "
                "%zu (the struct_size contract)",
                options == NULL ? (size_t)0 : options->struct_size,
                sizeof(fdkls_surface_options));
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (shell->layer_shell == NULL) {
        fdk_log(FDK_LOG_ERROR, FDKLS_LOG_TAG,
                "surface create: the compositor does not offer "
                "zwlr_layer_shell_v1 — nothing to create");
        return FDK_ERR_UNSUPPORTED;
    }

    fdk_result err = FDK_OK;
    if (!fdk_ok(fdkls__validate_options(shell, options, &err))) {
        return err;
    }

    /* Resolve the output by name BEFORE anything exists (a refusal
     * here costs nothing; a bad name on the wire is impossible —
     * get_layer_surface takes a proxy). */
    struct wl_output *output_proxy = NULL;
    const char *resolved = "";
    if (options->output != NULL && options->output[0] != '\0') {
        resolved = output_lookup(shell, options->output, &output_proxy);
        if (resolved == NULL) {
            fdk_log(FDK_LOG_ERROR, FDKLS_LOG_TAG,
                    "output '%s' is not one the compositor advertised "
                    "(wl_output v4 names: DP-1, eDP-1, ...)",
                    options->output);
            return FDK_ERR_NOT_FOUND;
        }
    }

    fdkls_surface *s = (fdkls_surface *)calloc(1, sizeof(*s));
    if (s == NULL) {
        fdk_log(FDK_LOG_ERROR, FDKLS_LOG_TAG,
                "surface create: out of memory");
        return FDK_ERR_OUT_OF_MEMORY;
    }
    s->shell = shell;
    s->output = output_proxy;

    /* The options, with the two strings owned (the caller may free
     * its copies immediately). */
    memcpy(&s->opts, options, sizeof(*options));
    const char *ns = options->namespace != NULL &&
                             options->namespace[0] != '\0'
                         ? options->namespace
                         : FDKLS_DEFAULT_NAMESPACE;
    s->opts.namespace = strdup(ns);
    s->output_name = strdup(resolved);
    if (s->opts.namespace == NULL || s->output_name == NULL) {
        free((char *)s->opts.namespace);
        free(s->output_name);
        free(s);
        fdk_log(FDK_LOG_ERROR, FDKLS_LOG_TAG,
                "surface create: out of memory (string copy)");
        return FDK_ERR_OUT_OF_MEMORY;
    }

    /* The kind's create callback picks the wrapper out of the
     * pending slot and builds the native pair; FDK adopts the
     * result. Any failure inside frees what it made. */
    shell->pending = s;
    fdk_result r = fdk_ext_surface_create(shell->kind, &s->ext);
    if (!fdk_ok(r)) {
        shell->pending = NULL;
        free((char *)s->opts.namespace);
        free(s->output_name);
        free(s);
        fdk_log(FDK_LOG_ERROR, FDKLS_LOG_TAG,
                "surface create failed (%d)", r);
        return r;
    }

    *out_surface = s;
    return FDK_OK;
}

void fdkls_surface_destroy(fdkls_surface *surface) {
    if (surface == NULL) {
        return;
    }
    /* fdk_ext_surface_destroy fires the kind's destroy callback,
     * which tears the role down against the live surface, unlinks
     * the wrapper, and frees it — this handle is dead on return
     * (the fdk_ext_surface contract, verbatim). */
    fdk_ext_surface_destroy(surface->ext);
}

fdk_result fdkls_surface_show(fdkls_surface *surface) {
    if (surface == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (surface->closed) {
        fdk_log(FDK_LOG_WARN, FDKLS_LOG_TAG,
                "show: the surface was closed by the compositor — "
                "destroy it (a closed role cannot re-map)");
        return FDK_ERR_INVALID_STATE;
    }
    if (surface->shown) {
        return FDK_OK; /* idempotent, like fdk_ext_surface_show */
    }

    /* FDK's show is a documented no-op on Wayland (the role's own
     * handshake owns mapping) — the package's contribution is the
     * protocol-mandated initial commit without a buffer: "After
     * creating a layer_surface object and setting it up, the client
     * must perform an initial commit without any buffer attached."
     * The compositor answers with configure(ack cycle); the first
     * present maps the surface. */
    fdk_ext_surface_show(surface->ext);
    wl_surface_commit(surface->wl_surface);
    surface->shown = 1;

    fdk_log(FDK_LOG_DEBUG, FDKLS_LOG_TAG,
            "handshake opened (initial empty commit)");
    return FDK_OK;
}

fdk_ext_surface *fdkls_surface_get_ext(fdkls_surface *surface) {
    return surface != NULL ? surface->ext : NULL;
}

fdk_result fdkls_surface_set_listener(fdkls_surface *surface,
                                      const fdkls_surface_listener *listener,
                                      void *user) {
    if (surface == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (listener == NULL) {
        surface->listener.struct_size = 0;
        surface->listener.closed = NULL;
        surface->listener_user = NULL;
        return FDK_OK;
    }
    if (listener->struct_size < sizeof(*listener)) {
        fdk_log(FDK_LOG_ERROR, FDKLS_LOG_TAG,
                "set_listener: struct_size %zu < %zu (the "
                "struct_size contract)",
                listener->struct_size, sizeof(fdkls_surface_listener));
        return FDK_ERR_INVALID_ARGUMENT;
    }
    surface->listener = *listener;
    surface->listener_user = user;
    return FDK_OK;
}

/* ---- passthroughs ---------------------------------------------------------- */

fdk_result fdkls_surface_get_surface(fdkls_surface *surface,
                                     fdk_surface **out) {
    if (surface == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    return fdk_ext_surface_get_surface(surface->ext, out);
}

void fdkls_surface_set_event_callback(fdkls_surface *surface,
                                      fdk_ext_event_fn callback,
                                      void *user) {
    if (surface == NULL) {
        return;
    }
    fdk_ext_surface_set_event_callback(surface->ext, callback, user);
}

void fdkls_surface_set_cursor(fdkls_surface *surface, int shape) {
    if (surface == NULL) {
        return;
    }
    fdk_ext_surface_set_cursor(surface->ext, shape);
}

fdk_result fdkls_surface_host_widgets(fdkls_surface *surface,
                                      fdk_widget **out_root) {
    if (surface == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    return fdk_ext_surface_host_widgets(surface->ext, out_root);
}

/* ---- live role-state updates --------------------------------------------------- */

/* Sends `apply` (the request sender), then an applying commit when
 * the surface is shown (the state is double-buffered; the initial
 * commit carries it otherwise). The common shape of every setter. */
static void role_apply(fdkls_surface *s, void (*send)(fdkls_surface *s)) {
    send(s);
    if (s->shown && !s->closed) {
        wl_surface_commit(s->wl_surface);
    }
}

static void send_anchor(fdkls_surface *s) {
    zwlr_layer_surface_v1_set_anchor(s->role, (uint32_t)s->opts.anchor);
}

static void send_exclusive(fdkls_surface *s) {
    zwlr_layer_surface_v1_set_exclusive_zone(s->role,
                                             s->opts.exclusive_zone);
}

static void send_margin(fdkls_surface *s) {
    zwlr_layer_surface_v1_set_margin(s->role, s->opts.margin_top,
                                     s->opts.margin_right,
                                     s->opts.margin_bottom,
                                     s->opts.margin_left);
}

static void send_keyboard(fdkls_surface *s) {
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        s->role, (uint32_t)s->opts.keyboard);
}

static void send_layer(fdkls_surface *s) {
    zwlr_layer_surface_v1_set_layer(s->role, (uint32_t)s->opts.layer);
}

static void send_size(fdkls_surface *s) {
    if (s->opts.width > 0 || s->opts.height > 0) {
        zwlr_layer_surface_v1_set_size(s->role,
                                       (uint32_t)s->opts.width,
                                       (uint32_t)s->opts.height);
    }
    /* Both zero: the protocol default — and the both-edges rule
     * (validated before we got here) makes the compositor assign
     * both axes; sending 0x0 explicitly is harmless but the default
     * needs no request. */
}

/* The setters' shared guard: refuses closed roles and validates the
 * NEW state before anything reaches the wire. */
static fdk_result surface_update_check(fdkls_surface *s) {
    if (s->closed) {
        fdk_log(FDK_LOG_WARN, FDKLS_LOG_TAG,
                "state update on a closed surface — the compositor "
                "ignores it; destroy the surface instead");
        return FDK_ERR_INVALID_STATE;
    }
    return FDK_OK;
}

fdk_result fdkls_surface_set_anchor(fdkls_surface *surface,
                                    fdkls_anchor anchor) {
    if (surface == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_result r = surface_update_check(surface);
    if (!fdk_ok(r)) {
        return r;
    }
    fdkls_surface_options check = surface->opts;
    check.anchor = anchor;
    fdk_result err = FDK_OK;
    if (!fdk_ok(fdkls__validate_options(surface->shell, &check, &err))) {
        return err;
    }
    surface->opts.anchor = anchor;
    role_apply(surface, send_anchor);
    return FDK_OK;
}

fdk_result fdkls_surface_set_exclusive_zone(fdkls_surface *surface,
                                            int32_t zone) {
    if (surface == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_result r = surface_update_check(surface);
    if (!fdk_ok(r)) {
        return r;
    }
    surface->opts.exclusive_zone = zone;
    role_apply(surface, send_exclusive);
    return FDK_OK;
}

fdk_result fdkls_surface_set_margin(fdkls_surface *surface,
                                    int32_t top, int32_t right,
                                    int32_t bottom, int32_t left) {
    if (surface == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_result r = surface_update_check(surface);
    if (!fdk_ok(r)) {
        return r;
    }
    surface->opts.margin_top = top;
    surface->opts.margin_right = right;
    surface->opts.margin_bottom = bottom;
    surface->opts.margin_left = left;
    role_apply(surface, send_margin);
    return FDK_OK;
}

fdk_result fdkls_surface_set_keyboard_interactivity(
    fdkls_surface *surface, fdkls_keyboard keyboard) {
    if (surface == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (keyboard < FDKLS_KEYBOARD_NONE ||
        keyboard > FDKLS_KEYBOARD_ON_DEMAND) {
        fdk_log(FDK_LOG_ERROR, FDKLS_LOG_TAG,
                "keyboard interactivity %d out of range — the "
                "compositor would raise "
                "invalid_keyboard_interactivity",
                (int)keyboard);
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_result r = surface_update_check(surface);
    if (!fdk_ok(r)) {
        return r;
    }
    /* The only v4-gated value: validate on a copy so the surface's
     * state never carries a value the wire could not. */
    fdkls_surface_options check = surface->opts;
    check.keyboard = keyboard;
    fdk_result err = FDK_OK;
    if (!fdk_ok(fdkls__validate_options(surface->shell, &check, &err))) {
        return err;
    }
    surface->opts.keyboard = keyboard;
    role_apply(surface, send_keyboard);
    return FDK_OK;
}

fdk_result fdkls_surface_set_layer(fdkls_surface *surface,
                                   fdkls_layer layer) {
    if (surface == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (layer < FDKLS_LAYER_BACKGROUND || layer > FDKLS_LAYER_OVERLAY) {
        fdk_log(FDK_LOG_ERROR, FDKLS_LOG_TAG,
                "layer %d out of range", (int)layer);
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_result r = surface_update_check(surface);
    if (!fdk_ok(r)) {
        return r;
    }
    if (surface->shell->layer_shell_version < 2) {
        fdk_log(FDK_LOG_ERROR, FDKLS_LOG_TAG,
                "set_layer needs zwlr_layer_shell_v1 v2, the "
                "compositor offers v%u",
                surface->shell->layer_shell_version);
        return FDK_ERR_UNSUPPORTED;
    }
    surface->opts.layer = layer;
    role_apply(surface, send_layer);
    return FDK_OK;
}

fdk_result fdkls_surface_set_size(fdkls_surface *surface,
                                  int32_t width, int32_t height) {
    if (surface == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_result r = surface_update_check(surface);
    if (!fdk_ok(r)) {
        return r;
    }
    fdkls_surface_options check = surface->opts;
    check.width = width;
    check.height = height;
    fdk_result err = FDK_OK;
    if (!fdk_ok(fdkls__validate_options(surface->shell, &check, &err))) {
        return err;
    }
    surface->opts.width = width;
    surface->opts.height = height;
    role_apply(surface, send_size);
    return FDK_OK;
}

const char *fdkls_surface_output_name(const fdkls_surface *surface) {
    if (surface == NULL) {
        return NULL;
    }
    return surface->output_name;
}

/* ---- version -------------------------------------------------------------- */

uint32_t fdkls_api_version(void) {
    return FDKLS_API_VERSION;
}

const char *fdkls_version_string(void) {
    return FDKLS_VERSION_STRING;
}
