/*
 * fdkls.h — FDK-Layer-Shell: the wlr-layer-shell add-on for FDK
 *
 * The official Faded Dream Project add-on package for desktop shell
 * surfaces: panels, docks, bars, notification overlays, wallpapers,
 * OSDs — anything that lives on a LAYER of the desktop rather than
 * in the window layer. On Wayland it speaks zwlr_layer_shell_v1 (the
 * wlr-layer-shell protocol wlroots-based compositors — sway, labwc,
 * river, Hyprland, wayfire, … — ship); on X11 there is nothing to
 * bind and the package refuses honestly (X11 dock semantics ride
 * EWMH directly; see the X11 section in docs/design.md).
 *
 * The package is an FDK ADD-ON in exactly the sense docs/EXTENDING.md
 * defines: it compiles against FDK's installed public headers and
 * library only (include/fdk/fdk_extend.h is the whole surface it
 * touches), registers ONE surface kind ("fdkls.layer-surface") on
 * the context, and builds every native surface itself — the
 * wl_surface through the wl_display* the extension API hands out,
 * the zwlr_layer_surface_v1 role through its own wl_registry on the
 * same display. FDK adopts the result: the layer surface renders
 * through the ordinary public fdk_surface API, receives the same
 * backend-neutral fdk_event_data stream applications get, can parent
 * popups (the role adopts each xdg_popup through
 * zwlr_layer_surface_v1.get_popup in the kind's popup_attached hook),
 * can host the whole widget catalog, subsurfaces, and input regions.
 * FDK never learns what the role IS; this package never touches an
 * FDK private struct.
 *
 * Versioning — the two numbers an application checks:
 *
 *     #include <fdkls/fdkls.h>
 *     #if !defined(FDKLS_API_VERSION) || FDKLS_API_VERSION < 1
 *     #error "this application needs FDK-Layer-Shell API v1"
 *     #endif
 *     ...
 *     if (fdkls_api_version() < 1) { ...refuse to run... }
 *
 * FDKLS_API_VERSION governs THIS package's ABI (append-only: nothing
 * is ever removed or renamed — the same discipline FDK's extension
 * API follows). The package version (FDKLS_VERSION_STRING) tracks
 * the FDK release the package was built against and bumps freely.
 * An application also pins the extension API it needs before
 * including anything (see docs/design.md §2 for the full ladder):
 *
 *     #if !defined(FDK_EXT_API_VERSION) || FDK_EXT_API_VERSION < 3
 *     #error "this application needs FDK's extension API v3 (FDK 1.0.0)"
 *     #endif
 *
 * Symbol namespace: every public symbol here is `fdkls_*` — the
 * `fdk_*` namespace belongs to FDK alone (docs/EXTENDING.md §11),
 * and the shared library exports exactly this list through a
 * version script generated from this header at build time.
 *
 * Threading: exactly like the rest of FDK — UI-thread-only. Every
 * call below must come from the context's UI thread; listener
 * callbacks run from inside FDK's dispatch/pump paths.
 *
 * Documentation: docs/design.md (the model, the protocol notes, the
 * honest limits); README.md (build + a five-minute tour).
 */

#ifndef FDKLS_H
#define FDKLS_H

#include <stddef.h>
#include <stdint.h>

#include <fdk/fdk_extend.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Version --------------------------------------------------------------
 *
 * FDKLS_API_VERSION is THIS add-on's ABI contract (append-only).
 * FDKLS_VERSION_STRING is the package version ("1.0.0" for the first
 * release), also carried by the pkg-config file and the shared
 * library's SONAME major (libfdk-layershell.so.1). */

#define FDKLS_API_VERSION 1
#define FDKLS_VERSION_MAJOR 1
#define FDKLS_VERSION_MINOR 0
#define FDKLS_VERSION_PATCH 0
#define FDKLS_VERSION_STRING "1.0.0"

/* The runtime check: returns the loaded library's FDKLS_API_VERSION
 * (>= 1). Compare once at startup against the compile-time macro —
 * a mismatch means the header and the .so disagree, the exact bug
 * this check exists to catch. */
uint32_t fdkls_api_version(void);

/* The package version string ("1.0.0"; never NULL). Diagnostics. */
const char *fdkls_version_string(void);

/* ---- Layers, anchors, keyboard -------------------------------------------
 *
 * The protocol's own vocabulary, typed. The numeric values match the
 * wire (zwlr_layer_shell_v1.layer, .anchor, .keyboard_interactivity)
 * — casts in the transport layer are literal, never translated. */

/* Which layer of the desktop the surface renders in — z-ordered
 * bottom-most first. Traditional shell surfaces (toplevels) render
 * between BOTTOM and TOP; fullscreen toplevels at TOP. The values
 * are the wire's, so a zero-initialized options struct means
 * BACKGROUND — ALWAYS set `layer` explicitly (the zero value is a
 * real layer, not a default). */
typedef enum fdkls_layer {
    FDKLS_LAYER_BACKGROUND = 0, /* wallpaper, conky-style displays   */
    FDKLS_LAYER_BOTTOM     = 1, /* beneath regular windows           */
    FDKLS_LAYER_TOP        = 2, /* above regular windows: bars       */
    FDKLS_LAYER_OVERLAY    = 3, /* above everything: OSDs, lock      */
} fdkls_layer;

/* Anchor edges — a BITMASK (OR the values; a corner is two bits).
 * A positive exclusive zone additionally requires the surface to be
 * anchored to one edge (or one edge plus its two perpendicular
 * edges); the compositor treats a positive zone on a corner-, edge-
 * pair-, or all-anchored surface as zero (protocol text, verbatim). */
typedef enum fdkls_anchor {
    FDKLS_ANCHOR_NONE   = 0,
    FDKLS_ANCHOR_TOP    = 1, /* bit 0 — the wire's anchor.top     */
    FDKLS_ANCHOR_BOTTOM = 2, /* bit 1                             */
    FDKLS_ANCHOR_LEFT   = 4, /* bit 2                             */
    FDKLS_ANCHOR_RIGHT  = 8, /* bit 3                             */
} fdkls_anchor;

/* How the surface participates in keyboard focus.
 *   NONE      — never focused (a clock bar; the default).
 *   EXCLUSIVE — exclusive focus while shown (lock screens, prompts).
 *   ON_DEMAND — normal click-to-focus semantics (protocol v4; a
 *               compositor advertising an older layer-shell refuses
 *               the value — fdkls_surface_create reports
 *               FDK_ERR_UNSUPPORTED rather than risking the wire). */
typedef enum fdkls_keyboard {
    FDKLS_KEYBOARD_NONE      = 0,
    FDKLS_KEYBOARD_EXCLUSIVE = 1,
    FDKLS_KEYBOARD_ON_DEMAND = 2,
} fdkls_keyboard;

/* ---- The shell ------------------------------------------------------------
 *
 * One fdkls_shell per fdk_context: the package's own wl_registry on
 * FDK's shared display connection (a second registry is ordinary
 * Wayland — events fan out per listener; FDK's registry is
 * untouched), the bindings it needs (wl_compositor to create
 * surfaces, zwlr_layer_shell_v1 for the role, wl_output for
 * by-name output targeting), and the one surface kind everything
 * below creates. */

typedef struct fdkls_shell fdkls_shell;

/* Opens the shell on `ctx`. Performs the registry dance (bind, one
 * roundtrip to collect globals, one more to collect the outputs'
 * names) and registers the package's surface kind.
 *
 * Returns NULL when:
 *   - ctx is NULL, or the context has no live connection (one log
 *     line, the D-Bus-client posture: a refusal never blocks);
 *   - the context is NOT on the Wayland backend — on X11 the
 *     package is out of its depth by construction (log line, NULL);
 *   - the FDK library at RUNTIME is older than this package was
 *     built against (the extension-API version ladder);
 *   - the context already has this package's kind registered (a
 *     second fdkls_shell_open on one context — close the first
 *     shell's surfaces and shell first);
 *   - allocation failed.
 *
 * A NULL return is ONE log line, never a crash, never a busy-loop.
 * A shell on a compositor that offers no layer-shell global still
 * OPENS (the degrade-is-the-contract rule): surfaces created on it
 * fail with FDK_ERR_UNSUPPORTED — check fdkls_shell_available(). */
fdkls_shell *fdkls_shell_open(fdk_context *ctx);

/* Closes the shell: destroys the package's registry and bindings and
 * unregisters the surface kind. Refuses while surfaces are still
 * alive (FDK_ERR_INVALID_STATE — destroy them first; the same rule
 * fdk_ext_kind_destroy applies). NULL is a safe no-op (FDK_OK). */
fdk_result fdkls_shell_close(fdkls_shell *shell);

/* Nonzero when the compositor offers zwlr_layer_shell_v1 and the
 * shell bound it. 0 also for a NULL shell. Surfaces created while 0
 * fail with FDK_ERR_UNSUPPORTED. */
int fdkls_shell_available(const fdkls_shell *shell);

/* The zwlr_layer_shell_v1 interface version the compositor
 * advertised (bounded by what this package speaks — 4), 0 when
 * unavailable. ON_DEMAND keyboard needs >= 4; set_layer needs >= 2;
 * everything else works from 1. */
uint32_t fdkls_shell_protocol_version(const fdkls_shell *shell);

/* ---- Layer surfaces -------------------------------------------------------- */

typedef struct fdkls_surface fdkls_surface;

/* The creation parameters. Zero-initialize, set struct_size, then
 * fill what you need — FDK-Layer-Shell copies exactly struct_size
 * bytes, so an application compiled against an older package keeps
 * working when fields are appended (never reordered), the same
 * contract fdk_ext_surface_ops carries. */
typedef struct fdkls_surface_options {
    size_t struct_size;      /* MUST be sizeof(fdkls_surface_options) */

    fdkls_layer layer;       /* ALWAYS set: the zero value is the
                              * real BACKGROUND layer (wire values)  */

    fdkls_anchor anchor;     /* bitmask — see fdkls_anchor           */

    /* Desired size in surface-local (logical) coordinates. 0 on an
     * axis means "compositor assigns" and REQUIRES the surface to be
     * anchored to both opposite edges of that axis (top+bottom for
     * height, left+right for width) — the protocol's own rule, and a
     * violation is a CONNECTION-KILLING protocol error, so create()
     * validates it locally and refuses with FDK_ERR_INVALID_ARGUMENT
     * before anything reaches the wire. A full-screen surface is
     * anchor = all four bits, width = height = 0. */
    int32_t width, height;

    /* The exclusive zone (protocol semantics, verbatim):
     *   > 0  reserve this many px from the anchored edge — maximized
     *        windows and other reserved-aware surfaces avoid it;
     *   = 0  reserve nothing, but DO move to avoid other surfaces'
     *        reservations (a toast above the panel);
     *   = -1 ignore reservations, extend to the anchored edges
     *        (wallpaper, lock screen).
     * Default 0. A positive zone includes the margin. */
    int32_t exclusive_zone;

    /* Margin from the anchor point, per edge (the exclusive zone
     * includes the margin). Default all zero. */
    int32_t margin_top, margin_right, margin_bottom, margin_left;

    fdkls_keyboard keyboard; /* default NONE                         */

    /* Target output by connector name ("DP-1", "eDP-1", "HDMI-A-1").
     * NULL = compositor picks (usually the recently-focused output).
     * An unknown name fails FDK_ERR_NOT_FOUND (the names the shell
     * knows come from the compositor's own wl_output.name events —
     * a compositor advertising wl_output < 4 carries no names and
     * only NULL targeting works; fdkls_surface_output_name()
     * reports what was actually resolved). */
    const char *output;

    /* The compositor's namespace for the surface — how compositors
     * and user config tell one client's surfaces from another's
     * (sway's `swaymsg` output, labwc rules, CSS selectors in GTK
     * compositors). NULL = "fdkls". */
    const char *namespace;
} fdkls_surface_options;

/* The role-level events FDK's backend-neutral stream cannot carry
 * (FDK never learns what the role is). Zero-initialize, set
 * struct_size, fill what you need; unset callbacks simply do not
 * fire. */
typedef struct fdkls_surface_listener {
    size_t struct_size; /* MUST be sizeof(fdkls_surface_listener) */

    /* The compositor withdrew the surface: the output was destroyed
     * or the user asked it gone (protocol `closed`). The role is
     * dead — further state changes are refused with
     * FDK_ERR_INVALID_STATE, and the compositor ignores any commits
     * that slip through ("further changes to the surface will be
     * ignored", protocol text). DESTROY THE SURFACE from here (the
     * popup sweep and shutdown sweep cover it if you don't). The
     * callback runs inside FDK's dispatch — destroying the surface
     * from it is safe, the same contract a window's
     * FDK_EVENT_WINDOW_CLOSE_REQUEST carries. */
    void (*closed)(fdkls_surface *surface, void *user);
} fdkls_surface_listener;

/* Creates a layer surface: this package's create callback builds
 * the wl_surface (through the shell's own wl_compositor binding),
 * assigns the zwlr_layer_surface_v1 role (through the shell's own
 * layer-shell binding), applies every parameter from `options`, and
 * hands the surface to FDK — which adopts it (event routing, the
 * render target, popups, widget hosting). NO COMMIT happens here:
 * the role's initial configure handshake comes first, and the wire
 * rule "no buffer before the first configure" is enforced by FDK's
 * present gate on top.
 *
 * The surface starts INERT. fdkls_surface_show() opens the
 * handshake (the protocol's initial empty commit); the compositor
 * answers with a configure, the package acks it and reports the
 * assigned size through fdk_ext_surface_notify_configure(), and the
 * surface's first present maps it.
 *
 * `options` must carry struct_size = sizeof(fdkls_surface_options).
 * The namespace string and the output name are copied at create —
 * an application may free its own copies immediately after.
 *
 * Can fail with:
 *   FDK_ERR_INVALID_ARGUMENT - shell/out NULL, options NULL or
 *                              struct_size too small, anchor bits
 *                              unknown, layer out of range, the
 *                              zero-size-without-both-edges rule,
 *                              width/height negative, an empty
 *                              output name
 *   FDK_ERR_NOT_FOUND        - `output` names an output the shell
 *                              does not know
 *   FDK_ERR_UNSUPPORTED      - the shell opened but the compositor
 *                              offers no layer-shell global, or
 *                              ON_DEMAND keyboard against a < v4
 *                              compositor
 *   FDK_ERR_INVALID_STATE    - the shell is closed
 *   FDK_ERR_OUT_OF_MEMORY
 *   FDK_ERR_WINDOW_CREATE    - the Wayland layer refused (role
 *                              collision, allocation), or FDK's
 *                              adoption refused the surface
 */
fdk_result fdkls_surface_create(fdkls_shell *shell,
                                const fdkls_surface_options *options,
                                fdkls_surface **out_surface);

/* Destroys the surface: the kind's role teardown runs first (the
 * zwlr_layer_surface_v1 dies against the still-live wl_surface),
 * then FDK destroys the native surface and frees the wrapper. NULL
 * is a safe no-op. Live popups on the surface are swept first (the
 * popup-family contract). A closed surface destroys cleanly — the
 * role teardown recognizes an already-dead role. */
void fdkls_surface_destroy(fdkls_surface *surface);

/* Opens the protocol handshake: the protocol-mandated initial commit
 * without a buffer ("after creating a layer_surface object and
 * setting it up, the client must perform an initial commit without
 * any buffer attached"). The compositor answers with a configure;
 * the package acks it and reports the assigned size, and the
 * surface's first present maps it. Idempotent: a second show is a
 * no-op. Refuses (FDK_ERR_INVALID_STATE) on a closed surface.
 *
 * There is no hide: a layer surface unmaps by committing a null
 * buffer, which would fight FDK's damage-tracked present path.
 * Hide-by-destroy is the honest move (recreate to re-show — the
 * handshake is one commit and one configure). */
fdk_result fdkls_surface_show(fdkls_surface *surface);

/* The FDK extension surface behind this layer surface. Every
 * fdk_ext_surface_* operation is meaningful on it: rendering
 * (get_surface), events (set_event_callback), the hover cursor,
 * input regions, widget hosting, popups (fdk_ext_popup_create —
 * the role adopts the xdg_popup through the kind's popup_attached
 * hook), subsurfaces, the native handle. NULL for a NULL surface. */
fdk_ext_surface *fdkls_surface_get_ext(fdkls_surface *surface);

/* Sets the role-level listener (replaces any previous one; NULL
 * listener removes it). `user` rides along to every callback.
 *
 * Can fail with:
 *   FDK_ERR_INVALID_ARGUMENT - surface NULL, or listener->struct_size
 *                              too small
 */
fdk_result fdkls_surface_set_listener(fdkls_surface *surface,
                                      const fdkls_surface_listener *listener,
                                      void *user);

/* ---- The FDK-side passthroughs --------------------------------------------
 *
 * Convenience wrappers over the ext-surface API (each documents the
 * underlying call it forwards to). Use fdkls_surface_get_ext() and
 * the fdk_ext_surface_* functions directly when you need the rest
 * (input regions, subsurfaces, native handles, size queries). */

/* fdk_ext_surface_get_surface: the render target. */
fdk_result fdkls_surface_get_surface(fdkls_surface *surface,
                                     fdk_surface **out);

/* fdk_ext_surface_set_event_callback: the backend-neutral event
 * stream — configure (the compositor's assigned size, after the
 * package acked), expose (a repaint OBLIGATION on a directly-driven
 * surface: redraw + present), pointer/keyboard/touch, focus. */
void fdkls_surface_set_event_callback(fdkls_surface *surface,
                                      fdk_ext_event_fn callback,
                                      void *user);

/* fdk_ext_surface_set_cursor: FDK_CURSOR_DEFAULT / FDK_CURSOR_POINTER
 * while hovering (the two shapes a panel needs; no-op on unknown
 * values, re-applied on every pointer entry). */
void fdkls_surface_set_cursor(fdkls_surface *surface, int shape);

/* fdk_ext_surface_host_widgets (extension API v3): the whole widget
 * catalog on the layer surface — the bar-with-a-clock pattern. The
 * tree owns the surface's pixels while hosted; configure events
 * resize and re-arrange it automatically. */
fdk_result fdkls_surface_host_widgets(fdkls_surface *surface,
                                      fdk_widget **out_root);

/* ---- Live role-state updates ----------------------------------------------
 *
 * The double-buffered layer-surface state, past create. Each setter
 * sends its request and — on a SHOWN surface — applies it with an
 * immediate empty commit (the same "applying commit" FDK's
 * subsurface operations issue; legal any time after the first
 * configure, invisible to the app's frame pacing). On a not-yet-
 * shown surface the state simply joins the initial commit show()
 * sends. Closed surfaces refuse (FDK_ERR_INVALID_STATE): the
 * compositor ignores further changes to a closed role.
 *
 * The compositor answers a size change with a fresh configure; the
 * ack + notify_configure + repaint cycle is the package's, and the
 * FDK_EVENT_WINDOW_CONFIGURE (plus the widget tree's re-arrange, if
 * hosted) is the application's. */

fdk_result fdkls_surface_set_anchor(fdkls_surface *surface,
                                    fdkls_anchor anchor);

fdk_result fdkls_surface_set_exclusive_zone(fdkls_surface *surface,
                                            int32_t zone);

fdk_result fdkls_surface_set_margin(fdkls_surface *surface,
                                    int32_t top, int32_t right,
                                    int32_t bottom, int32_t left);

/* ON_DEMAND against a compositor advertising < v4 refuses with
 * FDK_ERR_UNSUPPORTED (the value does not exist on the wire there —
 * sending it would be a protocol error). */
fdk_result fdkls_surface_set_keyboard_interactivity(
    fdkls_surface *surface, fdkls_keyboard keyboard);

/* Moves the surface between layers (protocol set_layer, v2+). A
 * compositor advertising v1 refuses with FDK_ERR_UNSUPPORTED. */
fdk_result fdkls_surface_set_layer(fdkls_surface *surface,
                                   fdkls_layer layer);

/* Zero-size follows the same both-edges rule as create (an axis set
 * to 0 requires both opposite edges anchored — validated locally,
 * FDK_ERR_INVALID_ARGUMENT). */
fdk_result fdkls_surface_set_size(fdkls_surface *surface,
                                  int32_t width, int32_t height);

/* ---- Diagnostics ----------------------------------------------------------- */

/* The connector name of the output the surface was created against
 * ("" — never NULL — when the compositor picked: the protocol's
 * NULL-output default). The name comes from the compositor's
 * wl_output.name event; a compositor advertising wl_output < 4
 * carries no names and unknown names were refused at create, so a
 * named create always resolves here. NULL for a NULL surface. */
const char *fdkls_surface_output_name(const fdkls_surface *surface);

#ifdef __cplusplus
}
#endif

#endif /* FDKLS_H */
