/*
 * fdkls_internal.h — FDK-Layer-Shell internal structures.
 *
 * Not installed, not public, never included by anything outside
 * src/. The one shared header between fdkls_shell.c (registry +
 * bindings + kind registration) and fdkls_surface.c (the surface
 * kind, the role listener, the public surface API).
 */

#ifndef FDKLS_INTERNAL_H
#define FDKLS_INTERNAL_H

#include <fdk/fdk_extend.h>
#include <fdk/fdk_log.h>

#include <fdkls/fdkls.h>

#include "generated/wlr-layer-shell-unstable-v1-client-protocol.h"

#include <stdint.h>

/* The package's log tag (fdk_log.h's public sink receives it — the
 * package logs through FDK's own logger, never a private stream). */
#define FDKLS_LOG_TAG "fdkls"

/* The one surface kind this package registers per context
 * (fdk_ext_register_kind's name — diagnostics; unique per ctx). */
#define FDKLS_KIND_NAME "fdkls.layer-surface"

/* The compositor namespace applied when options.namespace is NULL
 * (how compositors tell this package's surfaces from other clients'). */
#define FDKLS_DEFAULT_NAMESPACE "fdkls"

/* The newest zwlr_layer_shell_v1 interface version this package
 * speaks. v2 added set_layer; v4 added on-demand keyboard. Bumping
 * this is an FDKLS_API_VERSION bump (new surface of the protocol
 * reachable). */
#define FDKLS_LAYER_SHELL_MAX_VERSION 4

/* The newest wl_output version bound for by-name targeting. v4
 * carries the name event; older outputs are bound (the role needs a
 * wl_output proxy) but have no name to match against. */
#define FDKLS_OUTPUT_MAX_VERSION 4

typedef struct fdkls_surface fdkls_surface;

/* One bound wl_output global: the proxy the role request needs plus
 * the connector name the by-name targeting matches. `name` stays
 * NULL until the compositor's wl_output.name event arrives (v4+);
 * a NULL name is simply never matched. `global_name` is the
 * registry's numeric id, for global_remove. */
typedef struct fdkls_output {
    uint32_t global_name;
    struct wl_output *output;
    char *name;
    struct fdkls_output *next;
} fdkls_output;

struct fdkls_shell {
    fdk_context *ctx;
    fdk_ext_surface_kind *kind;

    /* FDK's shared display connection (borrowed for the context's
     * lifetime — fdk_ext_get_native_display's contract). */
    struct wl_display *display;

    /* This package's OWN registry on that display (a second registry
     * is ordinary Wayland; FDK's registry and listeners are
     * untouched). */
    struct wl_registry *registry;

    /* The bindings the role work needs. compositor + layer_shell are
     * destroyed with the shell; a global_remove for either marks the
     * shell unavailable (surfaces already created keep rendering —
     * their role proxies are independent objects). */
    struct wl_compositor *compositor;
    struct zwlr_layer_shell_v1 *layer_shell;
    uint32_t layer_shell_version; /* 0 = not bound */

    /* Every wl_output global, for by-name targeting (fdkls_output). */
    fdkls_output *outputs;

    /* Live surfaces of this kind (kind_destroy's lookup goes through
     * here; the shutdown sweep and the popup sweep both land in the
     * kind's destroy callback, always with the ext surface pointer
     * in hand and no wrapper handle). */
    fdkls_surface *surfaces;

    /* The create-in-progress slot: fdkls_surface_create stashes the
     * fresh wrapper here, fdk_ext_surface_create fires the kind's
     * create callback, which picks it up (the ops table is per-kind;
     * the per-surface state rides this slot — UI-thread-only, the
     * same single-thread discipline every FDK callback carries). */
    fdkls_surface *pending;

    /* Set by shell_close: the registry and bindings are gone; every
     * create attempt refuses. */
    int closed;
};

struct fdkls_surface {
    struct fdkls_surface *next; /* the shell's live-surface list    */

    fdkls_shell *shell;
    fdk_ext_surface *ext;       /* the FDK extension surface (NULL
                                 * only mid-teardown)                */

    /* The native pair. wl_surface is FDK-OWNED from the successful
     * adopt (fdk_ext.c's lifetime block — never destroyed here);
     * role is THIS package's (torn down in the kind's destroy).
     * output is the wl_output proxy the role request targeted (NULL
     * = compositor picks). */
    struct wl_surface *wl_surface;
    struct zwlr_layer_surface_v1 *role;
    struct wl_output *output;

    /* Desired state — a copy of the create options, with the two
     * strings owned (namespace + the resolved output name; "" for a
     * compositor-picked output). The setters mutate the fields they
     * change, so the role's configure callback can fall back to the
     * desired size on a compositor-assigned-zero axis. */
    fdkls_surface_options opts;
    char *output_name; /* owned; "" = compositor picked */

    /* Lifecycle flags. shown: the handshake-opening commit went out.
     * closed: the compositor withdrew the surface (role events stop,
     * setters refuse, the role proxy STAYS ALIVE — committing a
     * closed layer surface is protocol-legal ("further changes to
     * the surface will be ignored"), while destroying the role would
     * leave the FDK-owned wl_surface role-less and one stray commit
     * from a connection-killing protocol error). */
    int shown;
    int closed;

    /* The role-level listener (struct_size-validated at set time). */
    fdkls_surface_listener listener;
    void *listener_user;
};

/* fdkls_shell.c */
fdkls_shell *fdkls__shell_open(fdk_context *ctx);
fdk_result fdkls__shell_close(fdkls_shell *shell);
fdk_result fdkls__validate_options(const fdkls_shell *shell,
                                   const fdkls_surface_options *options,
                                   fdk_result *out_error);

/* fdkls_surface.c: the kind ops (registered through
 * fdk_ext_register_kind with the shell as the user pointer). */
fdk_result fdkls__kind_create(fdk_ext_surface *surface,
                              fdk_ext_native_handle *out_native,
                              void *user);
void fdkls__kind_destroy(fdk_ext_surface *surface, void *user);
void fdkls__kind_popup_attached(fdk_ext_surface *surface,
                                void *native_popup, void *user);

/* Finds the wrapper for an ext surface (kind callbacks receive the
 * ext pointer and the KIND's user — the shell — only). */
fdkls_surface *fdkls__surface_find(const fdkls_shell *shell,
                                   const fdk_ext_surface *ext);

#endif /* FDKLS_INTERNAL_H */
