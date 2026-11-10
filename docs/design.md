# FDK-Layer-Shell — the design document

This is the narrative contract: the model, the ownership rules, the
protocol notes, and the honest limits. `include/fdkls/fdkls.h` carries
the per-function reference; README.md the build and a tour.

---

## 1. The model in one paragraph

FDK-Layer-Shell is an FDK add-on in the sense FDK's
`docs/EXTENDING.md` defines. `fdkls_shell_open(ctx)` binds the
package's OWN `wl_registry` on FDK's shared display connection (a
second registry is ordinary Wayland — events fan out per listener;
FDK's registry is untouched), collects the globals it needs
(`wl_compositor` for surface creation, `zwlr_layer_shell_v1` for the
role, every `wl_output` for by-name targeting) through two
roundtrips, and registers ONE surface kind (`fdkls.layer-surface`)
on the context. `fdkls_surface_create` runs the kind's create
callback, which builds the `wl_surface` and assigns the
`zwlr_layer_surface_v1` role itself, and hands the wl_surface to FDK —
which ADOPTS it (event routing, the render target, popups, widget
hosting, subsurfaces, cursors, input regions). FDK never learns what
the role IS; the package never touches an FDK private struct.

## 2. The version ladder

Two independent numbers, both checked:

- **`FDK_EXT_API_VERSION`** — FDK's extension API. This package
  needs **v3** (kinds + popups + widget hosting). Compile-time: the
  application carries the `#if` guard before including anything.
  Run-time: `fdkls__shell_open` refuses an older library (one log
  line, NULL). An FDK older than 1.0.0 is refused as well.
- **`FDKLS_API_VERSION`** — this package's own ABI (append-only).
  Applications pin it the same way: `#if` at compile time,
  `fdkls_api_version()` at run time.

The protocol version is a THIRD axis, handled inside the package:
`fdkls_shell_protocol_version()` reports what the compositor
advertised (bounded by what the package speaks, 4); every
version-gated request (`set_layer` since v2, on-demand keyboard since
v4) is checked locally and refuses with `FDK_ERR_UNSUPPORTED` rather
than reaching the wire.

## 3. Ownership and lifetimes

- The PACKAGE owns the **role objects**: the
  `zwlr_layer_surface_v1` proxy (and the layer_shell/compositor/
  output bindings on the shell). The kind's destroy callback tears
  the role down — FDK calls it BEFORE destroying the native surface,
  so the teardown runs against a live wl_surface.
- FDK owns the **wl_surface** from the moment the create callback
  returns it. The package never destroys it. Every failure path in
  the kind's create callback cleans up what IT made (both proxies)
  before returning a negative result — on adopt failure FDK still
  runs the kind's destroy, so the paths stay symmetric.
- The namespace and output-name strings are copied at create; the
  application may free its copies immediately.
- An `fdkls_surface*` is valid from a successful create until
  `fdkls_surface_destroy()` (or the shutdown sweep). `closed` does
  NOT free anything — the surface stays a valid (inert) handle until
  destroyed.

## 4. The commit discipline

The layer-shell protocol's hard rule: a role's configure handshake
comes first — "any attempts by a client to attach or manipulate a
buffer prior to the first layer_surface.configure call must also be
treated as errors" (and FDK's own history: that exact error took down
a demo bar on labwc and sway). The package's shape:

1. **create** — surface + role + parameters, NO commit. FDK's
   present gate holds any eager buffer from here.
2. **show** — the protocol's initial commit without a buffer ("after
   creating a layer_surface object and setting it up, the client
   must perform an initial commit without any buffer attached").
   Idempotent; the one handshake opener.
3. **configure** — the package acks FIRST (the protocol demands the
   ack before any commit answering the configure), then reports the
   assigned size through `fdk_ext_surface_notify_configure`, which
   commits FDK's deferred first frame post-ack and delivers
   `FDK_EVENT_WINDOW_CONFIGURE`.
4. **live updates** — every setter sends its request and, on a shown
   surface, applies it with an immediate empty commit (the same
   "applying commit" shape FDK's subsurface operations use). On a
   not-yet-shown surface the state joins the initial commit.

The zero-size rule ("you must set your anchor to opposite edges in
the dimensions you omit; not doing so is a protocol error") is
validated LOCALLY — in create and in `fdkls_surface_set_size` —
because the wire's alternative is a dead connection. The same policy
covers the enum ranges, the anchor bitfield, the keyboard range, and
the version-gated values: **local refusal beats a protocol error,
always** (the mock-compositor suite pins this: seven refusals, zero
`get_layer_surface` requests on the wire).

## 5. Closed surfaces — the protocol's ignore-rule

The compositor's `closed` event (output gone, user removal) makes the
surface inert. The package: fires the role listener, marks the
surface closed, refuses all state changes with
`FDK_ERR_INVALID_STATE` — and deliberately KEEPS the role proxy
alive. The protocol says "further changes to the surface will be
ignored", so committing a closed layer surface is legal; destroying
the role would leave the FDK-owned wl_surface role-less, and the
next stray present would be a connection-killing protocol error. The
role dies in the kind's destroy callback — the only correct order.
The application's move is to destroy the surface from the listener
(the popup sweep and the shutdown sweep cover it if it doesn't).

## 6. X11 — the honest refusal

Layer-shell is a Wayland protocol. On an X11 context
`fdkls_shell_open` returns NULL with ONE log line. The X11 route to
panels and docks is EWMH: `_NET_WM_WINDOW_TYPE_DOCK` plus
`_NET_WM_STRUT_PARTIAL` set directly on FDK windows (FDK's
extension-API capability `FDK_EXT_CAP_EWMH_DOCK` reports it). The
honest limits of that route (screen-global struts, WM-dependent
stacking) are documented in FDK's `docs/EXTENDING.md` §12 — this
package does not fake Wayland fidelity on X11 and adds nothing to
that path.

## 7. Outputs

`fdkls_surface_options.output` targets an output by connector name
("DP-1", "eDP-1", ...). The names come from the compositor's
`wl_output.name` events (protocol v4 — universal on modern
compositors); a compositor advertising wl_output < 4 carries no
names, and only compositor-picked targeting (NULL) works there. An
unknown name refuses with `FDK_ERR_NOT_FOUND` before anything
reaches the wire. `fdkls_surface_output_name()` reports what a
surface actually resolved to ("" = compositor picked). An output the
compositor unplugs drops out of the shell's records; surfaces on it
receive the ROLE's `closed` event, which is the documented cleanup
path.

## 8. Popups

`fdk_ext_popup_create` on a layer surface's ext handle creates the
xdg_popup; FDK routes it through the kind's `popup_attached` hook and
the package adopts it with `zwlr_layer_surface_v1.get_popup` (the
waybar pattern; the xdg_popup's parent stays NULL — the protocol's
own requirement for layer-parented popups). A live applying commit
puts the adoption on the wire immediately. The package never touches
the popup's grab, configure, or buffer discipline — that is FDK's
xdg-popup machinery, unchanged.

## 9. Testing

Three tiers, mirroring FDK's own split (headless by default,
display-dependent suites explicitly):

1. **Headless** (`make test`) — the version surface, the argument /
   NULL ladder of every entry point, the struct_size contracts, the
   defaults. No display, plain CI.
2. **Mock compositor** (`make test-wayland`) — libwayland-server runs
   a real in-process compositor on a real unix socket advertising
   exactly the globals FDK's backend needs plus zwlr_layer_shell_v1;
   the suite drives the FULL lifecycle over the wire and asserts
   compositor-side state: the registry dance, the validation
   matrix's zero-request refusals, the role state as sent, the empty
   handshake commit, the configure/ack/notify order, the first REAL
   buffer strictly after the ack (the wire-side proof of the present
   gate), applying commits, by-name targeting, popup adoption on the
   wire, the closed event firing the listener and refusing updates.
   Two servers: v4 (everything) and v3 (the version-gated refusals).
   The server thread pumps its own loop (the client blocks in
   registry roundtrips); a mutex makes the mock's state atomic to
   the assertions.
3. **X11 refusal** (`make test-x11`) — a real X11 context (Xvfb when
   no $DISPLAY), asserting the NULL-shell refusal.

All tiers run under ASan/UBSan in the default configuration, the
same posture as FDK's own suites.

## 10. What the package deliberately does NOT provide

- **No hide/unmap.** A layer surface unmaps by committing a null
  buffer, which would fight FDK's damage-tracked present path.
  Destroy-to-hide is the honest move; recreating is one commit and
  one configure.
- **No raw wl_seat access.** Input flows through FDK's translated
  events (fdk_extend.h §12's rule); a layer surface's keyboard
  interactivity is the package's protocol negotiation, and FDK's
  keyboard events follow the surface's focus from there.
- **No GPU lane.** Extension surfaces render through the software
  `fdk_surface` path (identical pixels everywhere) — FDK's own
  documented extension-render contract.
- **No per-output enumeration API.** The names exist for targeting;
  a shell that wants an output LIST reads `fdkls_surface_output_name`
  results or registers its own registry (the extension API's
  documented native access).
- **No layer-shell v5+ surface features yet.** The package speaks
  v1–4 (v4 = on-demand keyboard); newer protocol versions bump
  FDKLS_LAYER_SHELL_MAX_VERSION together with FDKLS_API_VERSION.
