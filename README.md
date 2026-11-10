<div align="center">

# FDK-Layer-Shell

**The desktop shell add-on for FDK — panels, docks, bars, overlays and
wallpapers on wlr-layer-shell.**

Part of the [Faded Dream Project](https://github.com/Faded-Dream-Project)

[![License: FDK Proprietary v1.0](https://img.shields.io/badge/license-FDK%20Proprietary%20v1.0-blueviolet?style=flat-square)](LICENSE)
[![Built with C](https://img.shields.io/badge/built%20with%20C-orange?style=flat-square&logo=c)](https://www.open-std.org/jtc1/sc22/wg14/)
[![FDK 1.0.0+](https://img.shields.io/badge/FDK-1.0.0%2B%20extension%20API%20v3-blueviolet?style=flat-square)](https://github.com/Faded-Dream-Project/FDK)
[![Protocol: zwlr_layer_shell_v1](https://img.shields.io/badge/protocol-zwlr__layer__shell__v1%20v1%E2%80%934-blue?style=flat-square)](docs/design.md)

</div>

---

FDK-Layer-Shell is an [FDK](https://github.com/Faded-Dream-Project/FDK)
**add-on package** in exactly the sense `docs/EXTENDING.md` defines: it
compiles against FDK's installed public headers and library only,
registers one surface kind on the context, and builds every native
surface itself. On Wayland it speaks **zwlr_layer_shell_v1** (the
wlr-layer-shell protocol that sway, labwc, river, Hyprland, wayfire,
niri and every wlroots-based compositor ship) through its own
`wl_registry` on FDK's shared display connection; FDK then ADOPTS the
result. FDK never learns what the role IS; this package never touches
an FDK private struct.

What you get on a layer surface — the whole FDK extension-surface
contract:

- **Rendering** through the ordinary public `fdk_surface` API
  (damage-tracked, frame-paced, the same pixels everywhere);
- **Events** — the same backend-neutral `fdk_event_data` stream
  applications get (configure, expose, pointer, keyboard);
- **Widget hosting** (`fdkls_surface_host_widgets`) — the entire
  widget catalog on a bar or card;
- **Popups** — `fdk_ext_popup_create` dropdowns, adopted by the layer
  role through the kind's `popup_attached` hook (the waybar pattern);
- **Subsurfaces, input regions, hover cursors** — the rest of the
  extension API, unchanged;
- **Role-level events** FDK cannot carry (`closed`) through a small
  package listener.

On **X11** the package refuses honestly (one log line, NULL shell):
layer-shell is a Wayland protocol, and X11 panels ride EWMH dock
windows (`_NET_WM_WINDOW_TYPE_DOCK` + struts) directly through FDK's
own window API — documented, not faked.

## Build

You need FDK 1.0.0+ installed (its `fdk.pc` reachable through
`pkg-config`), `libwayland-dev` (wayland-client), and a C17 compiler.
For the integration test suite, `libwayland-server` as well.

```sh
make                # static + shared library (debug, ASan/UBSan)
make test           # headless contract tests (no display needed)
make test-wayland   # the mock-compositor integration suite — a real
                    # in-process Wayland server over a real socket;
                    # no desktop compositor required
make test-x11       # the X11-refusal leg (Xvfb started automatically)
make release        # optimized build
make verify-exports # the .so exports exactly the fdkls_* API
make examples       # the three example programs
make install        # headers + libs + pkg-config file to PREFIX
```

The `fdkls_*` namespace rule: FDK's `docs/EXTENDING.md` §11 reserves
the `fdk_*` namespace for FDK alone, so every public symbol here is
`fdkls_*`, and a generated version script (built the same way FDK
generates its own export map) keeps the shared library honest —
`make verify-exports` fails the build when anything leaks.

## Five-minute tour

The whole API is `include/fdkls/fdkls.h` (355 lines of documented
surface). A panel bar:

```c
#include <fdk/fdk.h>
#include <fdkls/fdkls.h>

int main(void) {
    fdk_context *ctx = NULL;
    fdk_init(&ctx, NULL);                    /* Wayland session */

    fdkls_shell *shell = fdkls_shell_open(ctx);
    if (shell == NULL || !fdkls_shell_available(shell)) return 1;

    fdkls_surface_options opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.layer = FDKLS_LAYER_TOP;            /* ALWAYS set: the zero
                                                value is BACKGROUND */
    opts.anchor = FDKLS_ANCHOR_TOP | FDKLS_ANCHOR_LEFT
                | FDKLS_ANCHOR_RIGHT;
    opts.height = 32;                        /* width: compositor-
                                                assigned (both edges) */
    opts.exclusive_zone = 32;                /* maximized windows stop
                                                above the bar */
    opts.namespace = "my-panel";

    fdkls_surface *bar = NULL;
    fdkls_surface_create(shell, &opts, &bar);

    fdk_widget *root = NULL;
    fdkls_surface_host_widgets(bar, &root);  /* the whole widget
                                                catalog on the bar */
    /* ... fdk_label_create / fdk_button_create into root ... */

    fdkls_surface_show(bar);                 /* the protocol's initial
                                                empty commit; the
                                                compositor answers
                                                configure, the package
                                                acks + reports, the
                                                first present maps */
    fdk_run(ctx);
    fdkls_surface_destroy(bar);
    fdkls_shell_close(shell);
    fdk_shutdown(ctx);
    return 0;
}
```

Compile against an installed FDK + FDK-Layer-Shell:

```sh
cc mypanel.c $(pkg-config --cflags --libs fdk-layershell) -o mypanel
```

Three worked examples live in `examples/`:

| Example | Shape |
|---|---|
| `01_panel_bar.c` | the classic TOP-layer bar: exclusive zone, widget-hosted clock, a popup dropdown |
| `02_overlay_toast.c` | an OVERLAY toast card: non-exclusive, margins, a timer that destroys it |
| `03_wallpaper.c` | a BACKGROUND surface: all edges anchored, zone -1, direct `fdk_surface` painting |

## The commit discipline (what the package gets right for you)

The protocol's hard rules — a violation is a **connection-killing**
compositor error — are all enforced before anything reaches the wire:

- **No buffer before the first configure.** FDK's present gate holds
  the first buffer until the role's configure is acked and reported
  (the rule that took down a demo bar on labwc and sway with
  `"layer_surface has never been configured"`); the mock-compositor
  suite proves the ordering on the wire.
- **Zero size needs both edges.** `width = 0` (compositor-assigned)
  is legal ONLY with left+right anchored (height: top+bottom); the
  package validates locally and refuses with
  `FDK_ERR_INVALID_ARGUMENT` instead of dying on the wire.
- **Enum ranges, anchor bits, on-demand keyboard (v4), set_layer
  (v2)** — all version-gated and range-checked locally. A compositor
  advertising v3 never sees an on-demand keyboard request.
- **The initial empty commit** — the handshake opener — is
  `fdkls_surface_show()`, exactly once, idempotent.

## Repository layout

```
include/fdkls/fdkls.h     the public API (the contract)
src/fdkls_shell.c         registry, bindings, the kind, option validation
src/fdkls_surface.c       the kind callbacks, the role listener, the API
generated/                vendored protocol XMLs + wayland-scanner output
tests/                    headless contract tests + the mock compositor
examples/                 panel bar / overlay toast / wallpaper
docs/design.md            the model, the protocol notes, the honest limits
```

## Status

In development, tracking FDK 1.0.0's extension API v3. The public
API carries its own version (`FDKLS_API_VERSION`, append-only —
nothing is ever removed or renamed), pinned compile-time AND run-time
by applications. See CHANGELOG.md.

## License

FDK Proprietary v1.0 — the Faded Dream Project's package license
(LICENSE). The vendored protocol XMLs carry their own permissive
licenses (see THIRD-PARTY-NOTICES.md).
