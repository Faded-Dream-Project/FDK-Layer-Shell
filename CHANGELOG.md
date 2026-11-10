# Changelog

All notable changes to FDK-Layer-Shell. The package version tracks the
FDK release it builds against; the API version (FDKLS_API_VERSION)
bumps on additions and never removes or renames (the same append-only
discipline as FDK's extension API and ABI policy).

## 1.0.0 — 2026-11-09

The first release, built on FDK 1.0.0's public extension API v3
(`fdk/fdk_extend.h`).

Added:

- The package's surface kind (`fdkls.layer-surface`) and the whole
  public API (`include/fdkls/fdkls.h`, `FDKLS_API_VERSION 1`):
  shell open/close + availability/protocol-version probes, surface
  create/destroy/show, the role listener (`closed`), the FDK-side
  passthroughs (render target, event callback, hover cursor, widget
  hosting), the live role-state setters (anchor, exclusive zone,
  margin, keyboard interactivity, layer, size), output targeting by
  connector name, and the diagnostics accessors.
- zwlr_layer_shell_v1 v1–4 support: layers, anchors, exclusive zones,
  margins, keyboard interactivity (on-demand gated to v4), set_layer
  (gated to v2), popups adopted through the kind's popup_attached
  hook, per-output targeting via the compositor's wl_output.name
  events.
- The option-validation matrix: every rule whose violation would be a
  connection-killing protocol error (layer range, anchor bits,
  zero-size-without-both-edges, negative sizes, keyboard range,
  version-gated values, unknown output names) refuses LOCALLY before
  anything reaches the wire.
- The commit discipline: no commit in create, the protocol's initial
  empty commit at show, applying commits for live state updates, and
  FDK's present gate holding the first buffer until the role's
  configure is acked and reported.
- Tests: the headless contract suite (version surface, argument
  ladders, struct_size contracts), the X11-refusal leg, and the
  mock-compositor integration suite — a real in-process Wayland
  server (libwayland-server) driving the full lifecycle over the
  wire, two servers (v4 + v3), ASan/UBSan-clean.
- Examples: 01_panel_bar (widget-hosted bar with a popup dropdown),
  02_overlay_toast (timer-dismissed overlay card), 03_wallpaper
  (direct-rendered background layer).
- Build mirroring FDK's own: debug/release configs, the same warning
  set, a generated export map with `make verify-exports`, a pkg-config
  file (fdk-layershell.pc), install/uninstall targets.
- CI: build + headless tests + the mock-compositor suite (both
  sanitized and release) + examples + the X11-refusal leg under
  Xvfb, against FDK built from the pinned main.
