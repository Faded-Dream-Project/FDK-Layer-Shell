/* test_fdkls_wayland.c — the mock-compositor integration suite.
 *
 * The REAL wire behavior, proven against a REAL (in-process) Wayland
 * server: libwayland-server runs a minimal compositor on a real unix
 * socket advertising exactly the globals FDK's backend needs
 * (wl_compositor, wl_shm, xdg_wm_base, wl_seat, wl_output) plus
 * zwlr_layer_shell_v1 — and the test drives the whole lifecycle over
 * the wire.
 *
 * Threads: the mock compositor pumps its event loop on its OWN
 * thread — the client side (FDK) blocks inside wl_display_roundtrip
 * at connect and at shell_open, so a single-threaded server could
 * never dispatch those requests. The mutex guards the mock's state:
 * the server thread holds it while dispatching (its handlers run
 * under it), the main thread holds it while asserting, and neither
 * side waits on the other's protocol progress while holding it.
 * Wire-state assertions go through run_until (pump the client,
 * check under the lock) — client requests sit in the connection's
 * output buffer until a flush, exactly as with any real compositor.
 *
 * Legs (two servers, sequentially):
 *   v4 — the full lifecycle: shell_open's registry dance, the
 *        validation matrix refusing before anything reaches the
 *        wire, create (role state on the wire as sent), show (the
 *        protocol's initial empty commit) -> configure -> ack ->
 *        notify, the first REAL buffer strictly after the ack,
 *        live state updates with applying commits, popup adoption
 *        through get_popup, the closed event firing the listener
 *        and refusing further updates, clean teardown.
 *   v3 — the version gate: on-demand keyboard refuses LOCALLY (the
 *        value does not exist on a v3 wire — sending it would be a
 *        connection-killing protocol error).
 *
 * Every assertion reads COMPOSITOR-SIDE state (requests received,
 * event order, buffer geometry), not just client-side returns — the
 * point is the wire.
 */

#include "fdk/fdk.h"
#include <fdkls/fdkls.h>

#include "generated/wlr-layer-shell-unstable-v1-server-protocol.h"
#include "generated/xdg-shell-server-protocol.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server.h>

/* ---- mock compositor state ------------------------------------------------ */

#define MOCK_OUTPUT_W 1280
#define MOCK_OUTPUT_H 800
#define MOCK_SOCKET_HINT 64
#define RUN_TIMEOUT_MS 5000

typedef struct mock_surface mock_surface;
typedef struct mock_layer mock_layer;

/* One wl_surface the client created. Commit-state tracking lives
 * here — the attach-before-configure assertion (the protocol error
 * FDK's present gate exists to prevent) is wire-side proof. */
struct mock_surface {
    struct wl_resource *resource;
    mock_layer *layer;         /* the layer role on it, or NULL   */
    mock_surface *next;

    int committed;             /* any commit seen                 */
    int first_commit_had_buffer; /* recorded at the FIRST commit —
                                  * the handshake opener must be an
                                  * EMPTY commit (protocol rule)    */
    int acked;                 /* ack_configure seen              */
    struct wl_resource *pending_buffer;
    struct wl_resource *attached_buffer;
    int attach_w, attach_h;    /* the last buffer's geometry      */
};

/* One zwlr_layer_surface_v1. */
struct mock_layer {
    struct wl_resource *resource;
    mock_surface *surface;
    uint32_t layer;
    uint32_t anchor;
    int32_t exclusive_zone;
    uint32_t keyboard;
    int has_size;
    uint32_t size_w, size_h;
    char *namespace;
    uint32_t configure_serial;
    int configured;
    int get_popup_seen;        /* the popup-attached hook, on the wire */
    mock_layer *next;
};

typedef struct {
    struct wl_display *display;
    struct wl_event_loop *loop;
    char socket_name[MOCK_SOCKET_HINT];
    uint32_t layer_shell_version; /* advertised version            */

    /* Compositor-side recordings (the assertions read these). */
    mock_surface *surfaces;
    mock_layer *layers;
    int configure_count;
    int commit_count;
    int buffer_commit_count;   /* commits carrying a buffer        */
    int get_layer_surface_count;
    int last_exclusive_zone;
} mock_server;

static mock_server g_mock;

/* The mutex + server thread (see the file comment). */
static pthread_mutex_t g_mock_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_server_thread;
static int g_server_running;

/* The application-side counters (the run_until conditions and the
 * timeout debug dump read them). */
static int g_app_configured;
static uint32_t g_app_configured_w, g_app_configured_h;
static int g_app_closed_count;

/* ---- xdg_wm_base (the popup leg; v1 stubs — complete request
 * tables, no behavior beyond what the popup-assertion needs) -------- */

static void xdg_popup_destroy_req(struct wl_client *client,
                                  struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void xdg_popup_grab(struct wl_client *client,
                           struct wl_resource *resource,
                           struct wl_resource *seat, uint32_t serial) {
    (void)client; (void)resource; (void)seat; (void)serial;
    /* No grab bookkeeping — the popup leg asserts only the role
     * adoption (the zwlr get_popup request), not the grab dance. */
}

static void xdg_popup_reposition(struct wl_client *client,
                                 struct wl_resource *resource,
                                 struct wl_resource *positioner,
                                 uint32_t token) {
    (void)client; (void)resource; (void)positioner; (void)token;
}

static const struct xdg_popup_interface g_xdg_popup_impl = {
    .destroy = xdg_popup_destroy_req,
    .grab = xdg_popup_grab,
    .reposition = xdg_popup_reposition,
};

static void xdg_toplevel_destroy(struct wl_client *client,
                                 struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void xdg_toplevel_set_parent(struct wl_client *c,
                                    struct wl_resource *r,
                                    struct wl_resource *parent) {
    (void)c; (void)r; (void)parent;
}
static void xdg_toplevel_set_title(struct wl_client *c,
                                   struct wl_resource *r,
                                   const char *title) {
    (void)c; (void)r; (void)title;
}
static void xdg_toplevel_set_app_id(struct wl_client *c,
                                    struct wl_resource *r,
                                    const char *app_id) {
    (void)c; (void)r; (void)app_id;
}
static void xdg_toplevel_show_window_menu(struct wl_client *c,
                                          struct wl_resource *r,
                                          struct wl_resource *seat,
                                          uint32_t serial, int32_t x,
                                          int32_t y) {
    (void)c; (void)r; (void)seat; (void)serial; (void)x; (void)y;
}
static void xdg_toplevel_move(struct wl_client *c,
                              struct wl_resource *r,
                              struct wl_resource *seat,
                              uint32_t serial) {
    (void)c; (void)r; (void)seat; (void)serial;
}
static void xdg_toplevel_resize(struct wl_client *c,
                                struct wl_resource *r,
                                struct wl_resource *seat,
                                uint32_t serial, uint32_t edges) {
    (void)c; (void)r; (void)seat; (void)serial; (void)edges;
}
static void xdg_toplevel_set_max_size(struct wl_client *c,
                                      struct wl_resource *r, int32_t w,
                                      int32_t h) {
    (void)c; (void)r; (void)w; (void)h;
}
static void xdg_toplevel_set_min_size(struct wl_client *c,
                                      struct wl_resource *r, int32_t w,
                                      int32_t h) {
    (void)c; (void)r; (void)w; (void)h;
}
static void xdg_toplevel_set_maximized(struct wl_client *c,
                                       struct wl_resource *r) {
    (void)c; (void)r;
}
static void xdg_toplevel_unset_maximized(struct wl_client *c,
                                         struct wl_resource *r) {
    (void)c; (void)r;
}
static void xdg_toplevel_set_fullscreen(struct wl_client *c,
                                        struct wl_resource *r,
                                        struct wl_resource *output) {
    (void)c; (void)r; (void)output;
}
static void xdg_toplevel_unset_fullscreen(struct wl_client *c,
                                          struct wl_resource *r) {
    (void)c; (void)r;
}
static void xdg_toplevel_set_minimized(struct wl_client *c,
                                       struct wl_resource *r) {
    (void)c; (void)r;
}

static const struct xdg_toplevel_interface g_xdg_toplevel_impl = {
    .destroy = xdg_toplevel_destroy,
    .set_parent = xdg_toplevel_set_parent,
    .set_title = xdg_toplevel_set_title,
    .set_app_id = xdg_toplevel_set_app_id,
    .show_window_menu = xdg_toplevel_show_window_menu,
    .move = xdg_toplevel_move,
    .resize = xdg_toplevel_resize,
    .set_max_size = xdg_toplevel_set_max_size,
    .set_min_size = xdg_toplevel_set_min_size,
    .set_maximized = xdg_toplevel_set_maximized,
    .unset_maximized = xdg_toplevel_unset_maximized,
    .set_fullscreen = xdg_toplevel_set_fullscreen,
    .unset_fullscreen = xdg_toplevel_unset_fullscreen,
    .set_minimized = xdg_toplevel_set_minimized,
};

static void xdg_surface_destroy_req(struct wl_client *client,
                                    struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void xdg_surface_get_toplevel(struct wl_client *client,
                                     struct wl_resource *resource,
                                     uint32_t id) {
    (void)resource;
    struct wl_resource *tl = wl_resource_create(
        client, &xdg_toplevel_interface, 1, id);
    assert(tl != NULL);
    wl_resource_set_implementation(tl, &g_xdg_toplevel_impl, NULL,
                                   NULL);
}

static void xdg_surface_get_popup(struct wl_client *client,
                                  struct wl_resource *resource,
                                  uint32_t id,
                                  struct wl_resource *parent,
                                  struct wl_resource *positioner) {
    (void)resource; (void)positioner;
    /* parent == NULL is the layer-surface-parented shape (the
     * protocol's own requirement: "created via xdg_surface::get_popup
     * with the parent set to NULL"). No bookkeeping — the ADOPTION
     * on the layer surface is the assertion. */
    (void)parent;
    struct wl_resource *pop = wl_resource_create(
        client, &xdg_popup_interface, 1, id);
    assert(pop != NULL);
    wl_resource_set_implementation(pop, &g_xdg_popup_impl, NULL, NULL);
}

static void xdg_surface_set_window_geometry(struct wl_client *client,
                                            struct wl_resource *resource,
                                            int32_t x, int32_t y,
                                            int32_t width,
                                            int32_t height) {
    (void)client; (void)resource; (void)x; (void)y; (void)width;
    (void)height;
}

static void xdg_surface_ack_configure(struct wl_client *client,
                                      struct wl_resource *resource,
                                      uint32_t serial) {
    (void)client; (void)resource; (void)serial;
}

static const struct xdg_surface_interface g_xdg_surface_impl = {
    .destroy = xdg_surface_destroy_req,
    .get_toplevel = xdg_surface_get_toplevel,
    .get_popup = xdg_surface_get_popup,
    .set_window_geometry = xdg_surface_set_window_geometry,
    .ack_configure = xdg_surface_ack_configure,
};

static void wm_base_destroy_req(struct wl_client *client,
                                struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void wm_base_get_xdg_surface(struct wl_client *client,
                                    struct wl_resource *resource,
                                    uint32_t id,
                                    struct wl_resource *surface) {
    (void)resource; (void)surface;
    struct wl_resource *xs = wl_resource_create(
        client, &xdg_surface_interface, 1, id);
    assert(xs != NULL);
    wl_resource_set_implementation(xs, &g_xdg_surface_impl, NULL, NULL);
}

static const struct xdg_positioner_interface g_positioner_impl; /* below */

static void wm_base_create_positioner(struct wl_client *client,
                                      struct wl_resource *resource,
                                      uint32_t id) {
    (void)resource;
    struct wl_resource *pos = wl_resource_create(
        client, &xdg_positioner_interface, 3, id);
    assert(pos != NULL);
    wl_resource_set_implementation(pos, &g_positioner_impl, NULL, NULL);
}

static void wm_base_pong(struct wl_client *client,
                         struct wl_resource *resource, uint32_t serial) {
    (void)client; (void)resource; (void)serial;
}

static const struct xdg_wm_base_interface g_wm_base_impl = {
    .destroy = wm_base_destroy_req,
    .get_xdg_surface = wm_base_get_xdg_surface,
    .pong = wm_base_pong,
    .create_positioner = wm_base_create_positioner,
};

static void wm_base_bind(struct wl_client *client, void *data,
                         uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *res = wl_resource_create(
        client, &xdg_wm_base_interface, (int)version, id);
    assert(res != NULL);
    wl_resource_set_implementation(res, &g_wm_base_impl, NULL, NULL);
}

/* ---- xdg_positioner (the popup leg's geometry carrier; v3 stubs) ---- */

static void positioner_destroy_req(struct wl_client *client,
                                   struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void positioner_set_size(struct wl_client *c,
                                struct wl_resource *r, int32_t w,
                                int32_t h) {
    (void)c; (void)r; (void)w; (void)h;
}
static void positioner_set_anchor_rect(struct wl_client *c,
                                       struct wl_resource *r, int32_t x,
                                       int32_t y, int32_t w, int32_t h) {
    (void)c; (void)r; (void)x; (void)y; (void)w; (void)h;
}
static void positioner_set_anchor(struct wl_client *c,
                                  struct wl_resource *r, uint32_t a) {
    (void)c; (void)r; (void)a;
}
static void positioner_set_gravity(struct wl_client *c,
                                   struct wl_resource *r, uint32_t g) {
    (void)c; (void)r; (void)g;
}
static void positioner_set_constraint_adjustment(struct wl_client *c,
                                                 struct wl_resource *r,
                                                 uint32_t adj) {
    (void)c; (void)r; (void)adj;
}
static void positioner_set_offset(struct wl_client *c,
                                  struct wl_resource *r, int32_t x,
                                  int32_t y) {
    (void)c; (void)r; (void)x; (void)y;
}
static void positioner_set_reactive(struct wl_client *c,
                                    struct wl_resource *r) {
    (void)c; (void)r;
}
static void positioner_set_parent_size(struct wl_client *c,
                                       struct wl_resource *r, int32_t w,
                                       int32_t h) {
    (void)c; (void)r; (void)w; (void)h;
}
static void positioner_set_parent_configure(struct wl_client *c,
                                            struct wl_resource *r,
                                            uint32_t serial) {
    (void)c; (void)r; (void)serial;
}

static const struct xdg_positioner_interface g_positioner_impl = {
    .destroy = positioner_destroy_req,
    .set_size = positioner_set_size,
    .set_anchor_rect = positioner_set_anchor_rect,
    .set_anchor = positioner_set_anchor,
    .set_gravity = positioner_set_gravity,
    .set_constraint_adjustment = positioner_set_constraint_adjustment,
    .set_offset = positioner_set_offset,
    .set_reactive = positioner_set_reactive,
    .set_parent_size = positioner_set_parent_size,
    .set_parent_configure = positioner_set_parent_configure,
};

/* ---- wl_seat (capabilities 0: the handshake needs a seat to exist,
 * not to carry input — the mock tests no input legs) -------------------- */

static void seat_get_object(struct wl_client *client,
                            struct wl_resource *resource, uint32_t id) {
    (void)client; (void)resource; (void)id;
}

static const struct wl_seat_interface g_seat_impl = {
    .get_pointer = seat_get_object,
    .get_keyboard = seat_get_object,
    .get_touch = seat_get_object,
};

static void seat_bind(struct wl_client *client, void *data,
                      uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *res = wl_resource_create(
        client, &wl_seat_interface, (int)version, id);
    assert(res != NULL);
    wl_resource_set_implementation(res, &g_seat_impl, NULL, NULL);
    wl_seat_send_capabilities(res, 0);
}

/* ---- wl_output ------------------------------------------------------------- */

static void output_bind(struct wl_client *client, void *data,
                        uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *res = wl_resource_create(
        client, &wl_output_interface, (int)version, id);
    assert(res != NULL);
    /* wl_output carries no requests — the initial state sequence,
     * done-terminated, exactly what FDK's scale tracking and this
     * package's name matching read. The name event is what the
     * package's by-name targeting reads; NO description event (FDK's
     * own wl_output listener predates it, and an unhandled opcode
     * aborts the CLIENT). */
    wl_output_send_geometry(res, 0, 0, 640, 380,
                            WL_OUTPUT_SUBPIXEL_UNKNOWN, "FadedDream",
                            "MockDisplay", WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(
        res,
        (uint32_t)(WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED),
        MOCK_OUTPUT_W, MOCK_OUTPUT_H, 60000);
    if (version >= 2) {
        wl_output_send_scale(res, 1);
    }
    if (version >= 4) {
        wl_output_send_name(res, "MOCK-1");
    }
    wl_output_send_done(res);
}

/* ---- wl_surface / wl_compositor ----------------------------------------------- */

static void surface_resource_dtor(struct wl_resource *resource) {
    mock_surface *s = (mock_surface *)wl_resource_get_user_data(resource);
    if (s != NULL) {
        mock_surface **link = &g_mock.surfaces;
        while (*link != NULL && *link != s) {
            link = &(*link)->next;
        }
        if (*link == s) {
            *link = s->next;
        }
        if (s->layer != NULL) {
            s->layer->surface = NULL; /* the layer dtor checks */
        }
        free(s);
    }
}

static void surface_destroy_req(struct wl_client *client,
                                struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource); /* the dtor unlinks + frees */
}

static void surface_attach(struct wl_client *client,
                           struct wl_resource *resource,
                           struct wl_resource *buffer, int32_t x,
                           int32_t y) {
    (void)client; (void)x; (void)y;
    mock_surface *s = (mock_surface *)wl_resource_get_user_data(resource);
    s->pending_buffer = buffer;
}

static void surface_damage(struct wl_client *client,
                           struct wl_resource *resource, int32_t x,
                           int32_t y, int32_t width, int32_t height) {
    (void)client; (void)resource; (void)x; (void)y; (void)width;
    (void)height;
}

static void surface_frame(struct wl_client *client,
                          struct wl_resource *resource, uint32_t id) {
    (void)resource;
    /* Frame callbacks drive FDK's Wayland pacing (the present path's
     * frame_ack gate). A compositor answers them per frame; the mock
     * answers immediately so presents never stall. */
    struct wl_resource *cb = wl_resource_create(
        client, &wl_callback_interface, 1, id);
    assert(cb != NULL);
    wl_callback_send_done(cb, 0);
    wl_resource_destroy(cb);
}

static void surface_set_opaque_region(struct wl_client *client,
                                      struct wl_resource *resource,
                                      struct wl_resource *region) {
    (void)client; (void)resource; (void)region;
}

static void surface_set_input_region(struct wl_client *client,
                                     struct wl_resource *resource,
                                     struct wl_resource *region) {
    (void)client; (void)resource; (void)region;
}

/* THE COMMIT. Every protocol assertion the suite makes about the
 * handshake flows through here: the initial empty commit opens the
 * configure; the first buffered commit must follow an ack (the
 * never-commit-before-configure rule the present gate enforces); a
 * buffer's geometry must match the configured size. */
static void surface_commit(struct wl_client *client,
                           struct wl_resource *resource) {
    (void)client;
    mock_surface *s = (mock_surface *)wl_resource_get_user_data(resource);
    mock_layer *l = s->layer;
    if (!s->committed) {
        s->first_commit_had_buffer = s->pending_buffer != NULL;
    }
    s->committed = 1;
    /* Count only LAYER commits: FDK's own surfaces (the cursor
     * surface it drives for the seat) commit too, and the suite's
     * applying-commit arithmetic is per layer surface. */
    if (l != NULL) {
        g_mock.commit_count++;
    }

    if (s->pending_buffer != NULL) {
        /* A buffered commit. The protocol (get_layer_surface text,
         * verbatim): "any attempts by a client to attach or
         * manipulate a buffer prior to the first
         * layer_surface.configure call must also be treated as
         * errors." The present gate (FDK) enforces it client-side;
         * this assert is the wire-side proof. */
        assert(s->acked &&
               "PROTOCOL VIOLATION: buffer attached before the first "
               "configure was acked");

        struct wl_shm_buffer *shm =
            wl_shm_buffer_get(s->pending_buffer);
        assert(shm != NULL && "the mock serves only wl_shm buffers");
        s->attach_w = (int)wl_shm_buffer_get_width(shm);
        s->attach_h = (int)wl_shm_buffer_get_height(shm);
        s->attached_buffer = s->pending_buffer;
        s->pending_buffer = NULL;
        g_mock.buffer_commit_count++;
        /* Released immediately — the compositor has read what it
         * needs; FDK's render slots cycle on the release. */
        wl_buffer_send_release(s->attached_buffer);
        return;
    }

    /* An empty commit. The FIRST one on a layer surface with no
     * configure yet is the handshake opener — answer it with the
     * protocol's configure, computed the way a compositor computes:
     * a both-edges-anchored axis is compositor-assigned, a
     * client-sized axis comes back as requested (0 = client
     * decides). */
    if (l != NULL && !l->configured) {
        uint32_t w = l->size_w;
        uint32_t h = l->size_h;
        if ((l->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) &&
            (l->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)) {
            w = MOCK_OUTPUT_W;
        }
        if ((l->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) &&
            (l->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)) {
            h = MOCK_OUTPUT_H;
        }
        l->configure_serial++;
        l->configured = 1;
        g_mock.configure_count++;
        zwlr_layer_surface_v1_send_configure(l->resource,
                                             l->configure_serial, w, h);
    }
}

static const struct wl_surface_interface g_surface_impl = {
    .destroy = surface_destroy_req,
    .attach = surface_attach,
    .damage = surface_damage,
    .frame = surface_frame,
    .set_opaque_region = surface_set_opaque_region,
    .set_input_region = surface_set_input_region,
    .commit = surface_commit,
    .set_buffer_transform = NULL, /* v2: FDK never sends it at v1 */
    .set_buffer_scale = NULL,     /* v3: ditto                    */
    .damage_buffer = NULL,        /* v4: ditto                    */
    .offset = NULL,               /* v5: ditto                    */
};

static void compositor_create_surface(struct wl_client *client,
                                      struct wl_resource *resource,
                                      uint32_t id) {
    (void)resource;
    struct wl_resource *res = wl_resource_create(
        client, &wl_surface_interface, 1, id);
    assert(res != NULL);
    mock_surface *s = (mock_surface *)calloc(1, sizeof(*s));
    assert(s != NULL);
    s->resource = res;
    s->next = g_mock.surfaces;
    g_mock.surfaces = s;
    wl_resource_set_implementation(res, &g_surface_impl, s,
                                   surface_resource_dtor);
}

static void region_add(struct wl_client *client,
                       struct wl_resource *resource, int32_t x,
                       int32_t y, int32_t width, int32_t height) {
    (void)client; (void)resource; (void)x; (void)y; (void)width;
    (void)height;
}

static void region_subtract(struct wl_client *client,
                            struct wl_resource *resource, int32_t x,
                            int32_t y, int32_t width, int32_t height) {
    (void)client; (void)resource; (void)x; (void)y; (void)width;
    (void)height;
}

static void region_destroy(struct wl_client *client,
                           struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct wl_region_interface g_region_impl = {
    .add = region_add,
    .subtract = region_subtract,
    .destroy = region_destroy,
};

static void compositor_create_region(struct wl_client *client,
                                     struct wl_resource *resource,
                                     uint32_t id) {
    (void)resource;
    struct wl_resource *res = wl_resource_create(
        client, &wl_region_interface, 1, id);
    assert(res != NULL);
    wl_resource_set_implementation(res, &g_region_impl, NULL, NULL);
}

static const struct wl_compositor_interface g_compositor_impl = {
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
};

static void compositor_bind(struct wl_client *client, void *data,
                            uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *res = wl_resource_create(
        client, &wl_compositor_interface, (int)version, id);
    assert(res != NULL);
    wl_resource_set_implementation(res, &g_compositor_impl, NULL, NULL);
}

/* ---- zwlr_layer_shell_v1 ------------------------------------------------------ */

static void layer_resource_dtor(struct wl_resource *resource) {
    mock_layer *l = (mock_layer *)wl_resource_get_user_data(resource);
    if (l != NULL) {
        mock_layer **link = &g_mock.layers;
        while (*link != NULL && *link != l) {
            link = &(*link)->next;
        }
        if (*link == l) {
            *link = l->next;
        }
        /* The paired surface may already be destroyed (teardown
         * order is the client's) — clear the back pointer only when
         * the record is still on the live list. */
        if (l->surface != NULL) {
            int live = 0;
            for (mock_surface *m = g_mock.surfaces; m != NULL;
                 m = m->next) {
                if (m == l->surface) {
                    live = 1;
                    break;
                }
            }
            if (live) {
                l->surface->layer = NULL;
            }
        }
        free(l->namespace);
        free(l);
    }
}

static void layer_surface_destroy_req(struct wl_client *client,
                                      struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource); /* the dtor unlinks + frees */
}

static void layer_set_size(struct wl_client *client,
                           struct wl_resource *resource, uint32_t width,
                           uint32_t height) {
    (void)client;
    mock_layer *l = (mock_layer *)wl_resource_get_user_data(resource);
    l->has_size = 1;
    l->size_w = width;
    l->size_h = height;
}

static void layer_set_anchor(struct wl_client *client,
                             struct wl_resource *resource,
                             uint32_t anchor) {
    (void)client;
    mock_layer *l = (mock_layer *)wl_resource_get_user_data(resource);
    l->anchor = anchor;
}

static void layer_set_exclusive_zone(struct wl_client *client,
                                     struct wl_resource *resource,
                                     int32_t zone) {
    (void)client;
    mock_layer *l = (mock_layer *)wl_resource_get_user_data(resource);
    l->exclusive_zone = zone;
    g_mock.last_exclusive_zone = zone;
}

static void layer_set_margin(struct wl_client *client,
                             struct wl_resource *resource, int32_t top,
                             int32_t right, int32_t bottom,
                             int32_t left) {
    (void)client; (void)resource; (void)top; (void)right; (void)bottom;
    (void)left;
}

static void layer_set_keyboard_interactivity(struct wl_client *client,
                                             struct wl_resource *resource,
                                             uint32_t mode) {
    (void)client;
    mock_layer *l = (mock_layer *)wl_resource_get_user_data(resource);
    l->keyboard = mode;
}

static void layer_get_popup(struct wl_client *client,
                            struct wl_resource *resource,
                            struct wl_resource *popup) {
    (void)client;
    mock_layer *l = (mock_layer *)wl_resource_get_user_data(resource);
    (void)popup;
    assert(popup != NULL);
    l->get_popup_seen = 1; /* the popup_attached hook, on the wire */
}

static void layer_ack_configure(struct wl_client *client,
                                struct wl_resource *resource,
                                uint32_t serial) {
    (void)client; (void)serial;
    mock_layer *l = (mock_layer *)wl_resource_get_user_data(resource);
    l->surface->acked = 1;
}

static void layer_set_layer(struct wl_client *client,
                            struct wl_resource *resource,
                            uint32_t layer) {
    (void)client;
    mock_layer *l = (mock_layer *)wl_resource_get_user_data(resource);
    l->layer = layer;
}

static const struct zwlr_layer_surface_v1_interface
    g_layer_surface_impl = {
        .set_size = layer_set_size,
        .set_anchor = layer_set_anchor,
        .set_exclusive_zone = layer_set_exclusive_zone,
        .set_margin = layer_set_margin,
        .set_keyboard_interactivity = layer_set_keyboard_interactivity,
        .get_popup = layer_get_popup,
        .ack_configure = layer_ack_configure,
        .destroy = layer_surface_destroy_req,
        .set_layer = layer_set_layer,
    };

static void layer_shell_get_layer_surface(
    struct wl_client *client, struct wl_resource *resource, uint32_t id,
    struct wl_resource *surface, struct wl_resource *output,
    uint32_t layer, const char *namespace_) {
    (void)resource;
    (void)output; /* one output in the mock; named targeting is
                   * asserted client-side (the name match) */

    /* "Creating a layer surface from a wl_surface which has a buffer
     * attached or committed is a client error" — the mock asserts
     * the CLEAN shape the package sends: role first, zero commits. */
    mock_surface *ms =
        (mock_surface *)wl_resource_get_user_data(surface);
    assert(ms != NULL);
    assert(!ms->committed && "surface committed before its role");

    struct wl_resource *res = wl_resource_create(
        client, &zwlr_layer_surface_v1_interface,
        wl_resource_get_version(resource), id);
    assert(res != NULL);
    mock_layer *l = (mock_layer *)calloc(1, sizeof(*l));
    assert(l != NULL);
    l->resource = res;
    l->surface = ms;
    l->layer = layer;
    l->namespace = strdup(namespace_);
    assert(l->namespace != NULL);
    l->next = g_mock.layers;
    g_mock.layers = l;
    ms->layer = l;
    wl_resource_set_implementation(res, &g_layer_surface_impl, l,
                                   layer_resource_dtor);
    g_mock.get_layer_surface_count++;
}

static void layer_shell_destroy_req(struct wl_client *client,
                                    struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct zwlr_layer_shell_v1_interface g_layer_shell_impl = {
    .get_layer_surface = layer_shell_get_layer_surface,
    .destroy = layer_shell_destroy_req,
};

static void layer_shell_bind(struct wl_client *client, void *data,
                             uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *res = wl_resource_create(
        client, &zwlr_layer_shell_v1_interface, (int)version, id);
    assert(res != NULL);
    wl_resource_set_implementation(res, &g_layer_shell_impl, NULL, NULL);
}

/* ---- the mock server lifecycle + thread harness ------------------------------- */

static void mock_start(uint32_t layer_shell_version) {
    memset(&g_mock, 0, sizeof(g_mock));
    g_mock.layer_shell_version = layer_shell_version;

    g_mock.display = wl_display_create();
    assert(g_mock.display != NULL);
    g_mock.loop = wl_display_get_event_loop(g_mock.display);

    const char *name = wl_display_add_socket_auto(g_mock.display);
    assert(name != NULL);
    snprintf(g_mock.socket_name, sizeof(g_mock.socket_name), "%s",
             name);

    assert(wl_global_create(g_mock.display, &wl_compositor_interface,
                            1, NULL, compositor_bind) != NULL);
    /* libwayland's BUILT-IN shm: init_shm creates the wl_shm global
     * and handles every pool/buffer request internally — that is
     * what makes wl_shm_buffer_get() work on the client's buffers. */
    assert(wl_display_init_shm(g_mock.display) == 0);
    /* v6: FDK's popup path creates a positioner (since v3) — a v1
     * advertisement would make the version-gated request invalid. */
    assert(wl_global_create(g_mock.display, &xdg_wm_base_interface, 6,
                            NULL, wm_base_bind) != NULL);
    assert(wl_global_create(g_mock.display, &wl_seat_interface, 1, NULL,
                            seat_bind) != NULL);
    assert(wl_global_create(g_mock.display, &wl_output_interface, 4,
                            NULL, output_bind) != NULL);
    assert(wl_global_create(g_mock.display,
                            &zwlr_layer_shell_v1_interface,
                            (int)layer_shell_version, NULL,
                            layer_shell_bind) != NULL);
}

/* The server thread: dispatch + flush until told to stop. Each
 * iteration holds the mutex for the dispatch itself — handlers run
 * under it, so their state mutations are atomic to the assertions. */
static void *server_thread_main(void *arg) {
    (void)arg;
    while (g_server_running) {
        pthread_mutex_lock(&g_mock_mutex);
        if (g_mock.display == NULL || g_mock.loop == NULL) {
            pthread_mutex_unlock(&g_mock_mutex);
            break;
        }
        wl_event_loop_dispatch(g_mock.loop, 1);
        wl_display_flush_clients(g_mock.display);
        pthread_mutex_unlock(&g_mock_mutex);
        struct timespec tick = {0, 1000 * 1000}; /* 1 ms */
        nanosleep(&tick, NULL);
    }
    return NULL;
}

static void mock_thread_start(void) {
    g_server_running = 1;
    assert(pthread_create(&g_server_thread, NULL, server_thread_main,
                          NULL) == 0);
}

static void mock_thread_stop(void) {
    g_server_running = 0;
    pthread_join(g_server_thread, NULL);
}

static void mock_stop(void) {
    mock_thread_stop();
    wl_display_destroy_clients(g_mock.display);
    wl_display_destroy(g_mock.display);
    g_mock.display = NULL;
    g_mock.loop = NULL;
}

/* Pumps the CLIENT side only (the server thread pumps itself): FDK
 * reads and dispatches the compositor's events, and flushes its own
 * requests out for the next server dispatch. */
static void pump_client(fdk_context *ctx) {
    (void)fdk_pump_events(ctx, 0);
}

/* run_until: pump the client, check `condition` under the lock, give
 * the server thread room, until it holds or the deadline passes. */
static int run_until(fdk_context *ctx, int (*condition)(void),
                     int timeout_ms) {
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        pump_client(ctx);
        pthread_mutex_lock(&g_mock_mutex);
        int done = condition();
        pthread_mutex_unlock(&g_mock_mutex);
        if (done) {
            return 1;
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000 +
                       (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed > timeout_ms) {
            return 0;
        }
        struct timespec tick = {0, 300 * 1000};
        nanosleep(&tick, NULL);
    }
}

/* Looks one mock layer record up by namespace. MUST be called under
 * the lock (run_until and the assertion blocks hold it). */
static mock_layer *find_layer_locked(const char *ns) {
    for (mock_layer *l = g_mock.layers; l != NULL; l = l->next) {
        if (l->namespace != NULL && strcmp(l->namespace, ns) == 0) {
            return l;
        }
    }
    return NULL;
}

/* Snapshots the mock counters under the lock (assertion reads). */
typedef struct {
    int configure_count;
    int commit_count;
    int buffer_commit_count;
    int get_layer_surface_count;
    int last_exclusive_zone;
} mock_snapshot;

static mock_snapshot mock_counters(void) {
    pthread_mutex_lock(&g_mock_mutex);
    mock_snapshot snap = {
        .configure_count = g_mock.configure_count,
        .commit_count = g_mock.commit_count,
        .buffer_commit_count = g_mock.buffer_commit_count,
        .get_layer_surface_count = g_mock.get_layer_surface_count,
        .last_exclusive_zone = g_mock.last_exclusive_zone,
    };
    pthread_mutex_unlock(&g_mock_mutex);
    return snap;
}

/* ---- the application side -------------------------------------------------------- */

static void app_present(fdk_ext_surface *surface) {
    fdk_surface *s = NULL;
    if (fdk_ok(fdk_ext_surface_get_surface(surface, &s)) && s != NULL) {
        fdk_surface_fill(s, (fdk_color){0.08f, 0.07f, 0.12f, 1});
        fdk_surface_present(s);
    }
}

static void app_event(fdk_ext_surface *surface,
                      const fdk_event_data *event, void *user) {
    (void)user;
    switch (event->type) {
    case FDK_EVENT_WINDOW_CONFIGURE:
        g_app_configured++;
        g_app_configured_w = (uint32_t)event->configure.size.width;
        g_app_configured_h = (uint32_t)event->configure.size.height;
        /* The add-on's EXPOSE discipline (docs/EXTENDING.md §5): a
         * configure answers with a repaint + present. */
        app_present(surface);
        break;
    default:
        break;
    }
}

static void app_closed(fdkls_surface *surface, void *user) {
    (void)surface;
    (void)user;
    g_app_closed_count++;
}

/* run_until conditions (plain functions over the mock state; called
 * under the lock — they look their records up by namespace, so no
 * stale captures). `g_snapshot_commit_before` is the commit-count
 * snapshot the applying-commit condition compares against; `g_zone_ns`
 * names the surface that condition watches (the bar first, the toast
 * later in the same leg). */
static int g_snapshot_commit_before;
static const char *g_zone_ns = "fdkls-test-bar";

static int cond_app_closed(void) {
    return g_app_closed_count > 0;
}

static int cond_bar_role(void) {
    mock_layer *l = find_layer_locked("fdkls-test-bar");
    return l != NULL && l->surface != NULL &&
           l->surface->committed == 0 &&
           l->layer == ZWLR_LAYER_SHELL_V1_LAYER_TOP &&
           l->anchor ==
               (ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) &&
           l->exclusive_zone == 32 &&
           l->keyboard ==
               ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE &&
           l->has_size && l->size_w == 0 && l->size_h == 32;
}

static int cond_handshake_opened(void) {
    mock_layer *l = find_layer_locked("fdkls-test-bar");
    return l != NULL && l->surface->committed &&
           /* the handshake opener was an EMPTY commit (the record
            * was captured at the first commit — polling can no
            * longer miss it, the first buffer follows within the
            * same pump) */
           l->surface->first_commit_had_buffer == 0 &&
           l->configured && l->configure_serial == 1;
}

static int cond_acked_and_notified(void) {
    mock_layer *l = find_layer_locked("fdkls-test-bar");
    return l != NULL && l->surface->acked && g_app_configured > 0 &&
           g_app_configured_w == MOCK_OUTPUT_W &&
           g_app_configured_h == 32;
}

static int cond_v3_acked_and_notified(void) {
    mock_layer *l = find_layer_locked("fdkls");
    return l != NULL && l->surface->acked && g_app_configured > 0 &&
           g_app_configured_w == MOCK_OUTPUT_W &&
           g_app_configured_h == 32;
}

static int cond_first_buffer(void) {
    mock_layer *l = find_layer_locked("fdkls-test-bar");
    return l != NULL && l->surface->attach_w == MOCK_OUTPUT_W &&
           l->surface->attach_h == 32;
}

static int cond_zone_applied(void) {
    mock_layer *l = find_layer_locked(g_zone_ns);
    return l != NULL && l->exclusive_zone == 40 &&
           g_mock.commit_count == g_snapshot_commit_before + 1;
}

static int cond_bar_size_updated(void) {
    mock_layer *l = find_layer_locked("fdkls-test-bar");
    return l != NULL && l->size_h == 40;
}

static int cond_toast_role(void) {
    mock_layer *l = find_layer_locked("fdkls-test-toast");
    return l != NULL && l->layer == ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY &&
           l->keyboard ==
               ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND &&
           l->size_w == 260 && l->size_h == 90;
}

static int cond_popup_adopted(void) {
    mock_layer *l = find_layer_locked("fdkls-test-bar");
    return l != NULL && l->get_popup_seen;
}

static int cond_bar_gone(void) {
    return find_layer_locked("fdkls-test-bar") == NULL;
}

static int cond_toast_gone(void) {
    return find_layer_locked("fdkls-test-toast") == NULL;
}

static int cond_v3_role(void) {
    mock_layer *l = find_layer_locked("fdkls");
    return l != NULL && l->layer == ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM &&
           l->keyboard ==
               ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;
}

/* ---- the legs --------------------------------------------------------------------- */

/* The option-validation matrix, re-proven on a LIVE shell (the
 * headless suite pins the same refusals against NULL): every rule
 * whose violation would be a connection-killing protocol error must
 * refuse LOCALLY — zero get_layer_surface requests on the wire. */
static void leg_validation_matrix(fdkls_shell *shell) {
    fdkls_surface_options opts;
    fdkls_surface *bad = NULL;

    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.layer = (fdkls_layer)4; /* invalid_layer on the wire */
    assert(fdkls_surface_create(shell, &opts, &bad) ==
           FDK_ERR_INVALID_ARGUMENT);
    opts.layer = FDKLS_LAYER_TOP;
    opts.anchor = (fdkls_anchor)0x10; /* invalid_anchor */
    assert(fdkls_surface_create(shell, &opts, &bad) ==
           FDK_ERR_INVALID_ARGUMENT);
    opts.anchor = FDKLS_ANCHOR_TOP;
    assert(fdkls_surface_create(shell, &opts, &bad) ==
           FDK_ERR_INVALID_ARGUMENT); /* 0x0, one edge */
    opts.anchor = FDKLS_ANCHOR_TOP | FDKLS_ANCHOR_LEFT;
    assert(fdkls_surface_create(shell, &opts, &bad) ==
           FDK_ERR_INVALID_ARGUMENT); /* width 0 without L+R */
    opts.anchor = FDKLS_ANCHOR_LEFT | FDKLS_ANCHOR_RIGHT;
    assert(fdkls_surface_create(shell, &opts, &bad) ==
           FDK_ERR_INVALID_ARGUMENT); /* height 0 without T+B */
    opts.anchor = FDKLS_ANCHOR_TOP | FDKLS_ANCHOR_LEFT |
                  FDKLS_ANCHOR_RIGHT;
    opts.height = -1;
    assert(fdkls_surface_create(shell, &opts, &bad) ==
           FDK_ERR_INVALID_ARGUMENT); /* negative size */
    opts.height = 32;
    opts.keyboard = (fdkls_keyboard)7; /* invalid_keyboard_... */
    assert(fdkls_surface_create(shell, &opts, &bad) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(bad == NULL);
    assert(mock_counters().get_layer_surface_count == 0 &&
           "validation refusals must never reach the wire");
    printf("[ok] validation matrix on a live shell: 7 refusals, 0 "
           "get_layer_surface requests\n");
}

static void leg_shell_and_bar(fdk_context *ctx) {
    fdkls_shell *shell = fdkls_shell_open(ctx);
    assert(shell != NULL && "shell_open on a live Wayland context");
    assert(fdkls_shell_available(shell));
    assert(fdkls_shell_protocol_version(shell) ==
           g_mock.layer_shell_version);
    printf("[ok] shell open: available, protocol v%u\n",
           fdkls_shell_protocol_version(shell));

    /* By-name targeting: the mock advertises MOCK-1; an unknown name
     * refuses (NOT_FOUND) before anything reaches the wire. */
    fdkls_surface_options opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    /* A valid shape (the option validator runs first — the zero-size
     * rule is the earlier refusal); the unknown NAME is what must
     * trip here. */
    opts.layer = FDKLS_LAYER_TOP;
    opts.anchor = (fdkls_anchor)(FDKLS_ANCHOR_TOP |
                                 FDKLS_ANCHOR_LEFT |
                                 FDKLS_ANCHOR_RIGHT);
    opts.height = 32;
    opts.output = "NOPE-9";
    fdkls_surface *nope = NULL;
    assert(fdkls_surface_create(shell, &opts, &nope) ==
           FDK_ERR_NOT_FOUND);
    assert(nope == NULL);
    assert(mock_counters().get_layer_surface_count == 0);
    printf("[ok] unknown output refused: FDK_ERR_NOT_FOUND, nothing "
           "on the wire\n");

    leg_validation_matrix(shell);

    /* THE BAR: layer TOP, anchored top+left+right, height 32 (width
     * compositor-assigned), exclusive zone 32. */
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.layer = FDKLS_LAYER_TOP;
    opts.anchor = (fdkls_anchor)(FDKLS_ANCHOR_TOP |
                                 FDKLS_ANCHOR_LEFT |
                                 FDKLS_ANCHOR_RIGHT);
    opts.height = 32;
    opts.exclusive_zone = 32;
    opts.namespace = "fdkls-test-bar";
    fdkls_surface *bar = NULL;
    assert(fdkls_surface_create(shell, &opts, &bar) == FDK_OK);
    assert(bar != NULL);
    assert(fdkls_surface_get_ext(bar) != NULL);
    assert(strcmp(fdkls_surface_output_name(bar), "") == 0);

    fdkls_surface_listener listener;
    memset(&listener, 0, sizeof(listener));
    listener.struct_size = sizeof(listener);
    listener.closed = app_closed;
    assert(fdkls_surface_set_listener(bar, &listener, NULL) == FDK_OK);
    fdkls_surface_set_event_callback(bar, app_event, NULL);

    /* Compositor-side state of the created role (before any
     * commit): every option landed on the wire as sent. */
    assert(run_until(ctx, cond_bar_role, RUN_TIMEOUT_MS));
    printf("[ok] bar created: role state on the wire matches the "
           "options\n");

    /* Reset BEFORE the handshake: the configure fires inside the
     * wait below (the delivery races the mock-side condition), one
     * configure, dispatched once. */
    g_app_configured = 0;

    /* show() = the initial empty commit; a second show is a no-op
     * (idempotent). */
    assert(fdkls_surface_show(bar) == FDK_OK);
    assert(fdkls_surface_show(bar) == FDK_OK);
    assert(run_until(ctx, cond_handshake_opened, RUN_TIMEOUT_MS));
    printf("[ok] show: the protocol's initial EMPTY commit reached "
           "the wire, configure sent, acked, serial 1\n");

    /* The compositor assigned (1280, 32) (width assigned: both
     * horizontal edges); the package acked FIRST and reported; the
     * app saw FDK_EVENT_WINDOW_CONFIGURE (fired inside the wait
     * above — the counter carries it). */
    assert(run_until(ctx, cond_acked_and_notified, RUN_TIMEOUT_MS));
    printf("[ok] handshake: configure(%u,%u) -> ack -> notify; the "
           "app saw CONFIGURE\n",
           g_app_configured_w, g_app_configured_h);

    /* The first present: a REAL wl_shm buffer of the configured
     * size, strictly after the ack (the present gate, proven on the
     * wire — surface_commit's assert would have fired otherwise). */
    assert(run_until(ctx, cond_first_buffer, RUN_TIMEOUT_MS));
    assert(mock_counters().buffer_commit_count >= 1);
    printf("[ok] first present: %dx%d buffer attached after the ack\n",
           MOCK_OUTPUT_W, 32);

    /* Live state: the exclusive zone changes and applies with an
     * empty commit (the applying-commit shape). */
    pthread_mutex_lock(&g_mock_mutex);
    g_snapshot_commit_before = g_mock.commit_count;
    pthread_mutex_unlock(&g_mock_mutex);
    assert(fdkls_surface_set_exclusive_zone(bar, 40) == FDK_OK);
    assert(run_until(ctx, cond_zone_applied, RUN_TIMEOUT_MS));
    printf("[ok] set_exclusive_zone(40): request + applying commit\n");

    /* set_size on a compositor-assigned axis: the request updates
     * the compositor's desired state. */
    assert(fdkls_surface_set_size(bar, 0, 40) == FDK_OK);
    assert(run_until(ctx, cond_bar_size_updated, RUN_TIMEOUT_MS));
    printf("[ok] set_size(0, 40): compositor state updated\n");

    /* A named toast on the same shell (second surface, independent
     * state), by output name this time. */
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.layer = FDKLS_LAYER_OVERLAY;
    opts.anchor = (fdkls_anchor)(FDKLS_ANCHOR_TOP |
                                 FDKLS_ANCHOR_RIGHT);
    opts.width = 260;
    opts.height = 90;
    opts.keyboard = FDKLS_KEYBOARD_ON_DEMAND; /* the mock is v4 */
    opts.output = "MOCK-1";
    opts.namespace = "fdkls-test-toast";
    fdkls_surface *toast = NULL;
    assert(fdkls_surface_create(shell, &opts, &toast) == FDK_OK);
    assert(strcmp(fdkls_surface_output_name(toast), "MOCK-1") == 0);
    assert(fdkls_surface_show(toast) == FDK_OK);
    assert(run_until(ctx, cond_toast_role, RUN_TIMEOUT_MS));
    printf("[ok] toast created by name (MOCK-1): overlay, on-demand "
           "keyboard, client-sized 260x90\n");

    /* Popups: the xdg_popup lands on the layer surface through the
     * role's get_popup adoption (the popup_attached hook). */
    fdk_ext_surface *pop = NULL;
    assert(fdk_ext_popup_create(fdkls_surface_get_ext(bar), 10, 10,
                                200, 60, &pop) == FDK_OK);
    assert(pop != NULL);
    assert(run_until(ctx, cond_popup_adopted, RUN_TIMEOUT_MS));
    printf("[ok] popup adopted by the layer role\n");
    fdk_ext_surface_destroy(pop);

    /* The compositor withdraws the bar (output gone / user
     * removal): the listener fires; the closed role refuses state
     * changes; teardown stays protocol-clean. */
    pthread_mutex_lock(&g_mock_mutex);
    mock_layer *ml = find_layer_locked("fdkls-test-bar");
    assert(ml != NULL);
    zwlr_layer_surface_v1_send_closed(ml->resource);
    pthread_mutex_unlock(&g_mock_mutex);
    assert(run_until(ctx, cond_app_closed, RUN_TIMEOUT_MS));
    assert(g_app_closed_count == 1);
    assert(fdkls_surface_set_exclusive_zone(bar, 10) ==
           FDK_ERR_INVALID_STATE);
    assert(fdkls_surface_show(bar) == FDK_ERR_INVALID_STATE);
    assert(fdkls_surface_set_anchor(bar, FDKLS_ANCHOR_TOP) ==
           FDK_ERR_INVALID_STATE);
    printf("[ok] closed: listener fired, the dead role refuses "
           "updates\n");

    /* The toast keeps working (the closed surface is independent). */
    pthread_mutex_lock(&g_mock_mutex);
    g_snapshot_commit_before = g_mock.commit_count;
    pthread_mutex_unlock(&g_mock_mutex);
    assert(fdkls_surface_set_exclusive_zone(toast, 0) == FDK_OK);
    assert(run_until(ctx, cond_zone_applied, RUN_TIMEOUT_MS));
    printf("[ok] the closed surface is independent: the toast still "
           "updates\n");

    fdkls_surface_destroy(toast);
    fdkls_surface_destroy(bar);
    fdkls_surface_destroy(NULL); /* safe no-op */

    assert(run_until(ctx, cond_bar_gone, RUN_TIMEOUT_MS));
    assert(run_until(ctx, cond_toast_gone, RUN_TIMEOUT_MS));

    assert(fdkls_shell_close(shell) == FDK_OK);
    printf("[ok] teardown clean: surfaces, role proxies, shell\n");
}

/* The v3 server: everything v1-shaped works; the v4-only value must
 * refuse LOCALLY (sending on_demand on a v3 wire would be a protocol
 * error that kills the connection). */
static void leg_v3(void) {
    mock_start(3);
    mock_thread_start();
    setenv("WAYLAND_DISPLAY", g_mock.socket_name, 1);
    unsetenv("DISPLAY");

    fdk_context *ctx = NULL;
    fdk_result r = fdk_init(&ctx, NULL);
    assert(fdk_ok(r));
    assert(fdk_ext_active_backend(ctx) == FDK_EXT_BACKEND_WAYLAND);

    fdkls_shell *shell = fdkls_shell_open(ctx);
    assert(shell != NULL);
    assert(fdkls_shell_protocol_version(shell) == 3);

    /* ON_DEMAND needs v4: refused at create AND at the setter. */
    fdkls_surface_options opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.anchor = (fdkls_anchor)(FDKLS_ANCHOR_TOP |
                                 FDKLS_ANCHOR_LEFT |
                                 FDKLS_ANCHOR_RIGHT);
    opts.height = 32;
    opts.keyboard = FDKLS_KEYBOARD_ON_DEMAND;
    fdkls_surface *s = NULL;
    assert(fdkls_surface_create(shell, &opts, &s) ==
           FDK_ERR_UNSUPPORTED);
    opts.keyboard = FDKLS_KEYBOARD_NONE;
    assert(fdkls_surface_create(shell, &opts, &s) == FDK_OK);
    /* The app's event callback: the handshake's CONFIGURE dispatch
     * needs somewhere to land (the v4 leg sets it via the listener
     * block; same thing here). */
    fdkls_surface_set_event_callback(s, app_event, NULL);
    assert(fdkls_surface_set_keyboard_interactivity(
               s, FDKLS_KEYBOARD_ON_DEMAND) == FDK_ERR_UNSUPPORTED);
    assert(fdkls_surface_set_keyboard_interactivity(
               s, FDKLS_KEYBOARD_EXCLUSIVE) == FDK_OK);
    /* set_layer is v2: present here (3 >= 2), must reach the wire. */
    assert(fdkls_surface_set_layer(s, FDKLS_LAYER_BOTTOM) == FDK_OK);

    assert(run_until(ctx, cond_v3_role, RUN_TIMEOUT_MS));
    printf("[ok] v3 compositor: on-demand refused locally, "
           "set_layer + exclusive delivered\n");

    /* The handshake works identically. */
    assert(fdkls_surface_show(s) == FDK_OK);
    g_app_configured = 0;
    assert(run_until(ctx, cond_v3_acked_and_notified, RUN_TIMEOUT_MS));
    printf("[ok] v3 compositor: full handshake works\n");

    fdkls_surface_destroy(s);
    assert(fdkls_shell_close(shell) == FDK_OK);
    fdk_shutdown(ctx);
    mock_stop();
    printf("[ok] v3 leg complete\n");
}

int main(void) {
    /* ---- the v4 server: the full lifecycle ---- */
    mock_start(4);
    mock_thread_start();
    setenv("WAYLAND_DISPLAY", g_mock.socket_name, 1);
    unsetenv("DISPLAY");

    fdk_context *ctx = NULL;
    fdk_result r = fdk_init(&ctx, NULL);
    assert(fdk_ok(r) && "fdk_init against the mock compositor");
    assert(fdk_ext_active_backend(ctx) == FDK_EXT_BACKEND_WAYLAND);

    leg_shell_and_bar(ctx);

    fdk_shutdown(ctx);
    mock_stop();
    printf("[ok] v4 leg complete\n");

    /* ---- the v3 server: version-gated refusals ---- */
    leg_v3();

    printf("all fdk-layershell mock-compositor tests passed\n");
    return 0;
}
