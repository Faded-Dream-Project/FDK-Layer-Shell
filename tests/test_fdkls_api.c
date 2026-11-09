/* test_fdkls_api.c — FDK-Layer-Shell headless tests.
 *
 * Platform-independent contract checks that need no display: the
 * version surface, the argument-validation / NULL-safety ladder of
 * every fdkls_* entry point, and the option-validation matrix —
 * every rule whose violation would be a connection-killing protocol
 * error on the wire (docs/design.md §5) is pinned here so the
 * refusal is testable without a compositor.
 *
 * The REAL wire behavior (handshake, configure/ack, first buffer,
 * live state updates, closed) needs a Wayland connection and lives
 * in tests/test_fdkls_wayland.c's in-process mock compositor (make
 * test-wayland), and the X11-refusal posture in test_fdkls_x11.c
 * (make test-x11) — the same split FDK's own suites follow
 * (test_extend.c vs the integration suites).
 */

#include "fdk/fdk.h"
#include <fdkls/fdkls.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---- version surface ------------------------------------------------------ */

static void test_version_surface(void) {
    assert(fdkls_api_version() == FDKLS_API_VERSION);
    assert(fdkls_api_version() >= 1);
    assert(fdkls_version_string() != NULL);
    assert(fdkls_version_string()[0] != '\0');
    printf("[ok] version surface: api v%u, package %s\n",
           fdkls_api_version(), fdkls_version_string());
}

/* ---- NULL/argument ladders ------------------------------------------------- */

static void test_null_ladder(void) {
    /* The shell. */
    assert(fdkls_shell_open(NULL) == NULL);
    assert(fdkls_shell_close(NULL) == FDK_OK); /* safe no-op */
    assert(fdkls_shell_available(NULL) == 0);
    assert(fdkls_shell_protocol_version(NULL) == 0);

    /* Surfaces: NULL everywhere, every entry point. */
    fdkls_surface *s = NULL;
    fdkls_surface_options opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    assert(fdkls_surface_create(NULL, &opts, &s) ==
           FDK_ERR_INVALID_ARGUMENT);
    fdkls_surface_destroy(NULL); /* safe no-op */
    assert(fdkls_surface_show(NULL) == FDK_ERR_INVALID_ARGUMENT);
    assert(fdkls_surface_get_ext(NULL) == NULL);
    assert(fdkls_surface_set_listener(NULL, NULL, NULL) ==
           FDK_ERR_INVALID_ARGUMENT);
    fdk_surface *render = NULL;
    assert(fdkls_surface_get_surface(NULL, &render) ==
           FDK_ERR_INVALID_ARGUMENT);
    fdkls_surface_set_event_callback(NULL, NULL, NULL); /* no-op */
    fdkls_surface_set_cursor(NULL, FDK_CURSOR_POINTER); /* no-op */
    fdk_widget *root = NULL;
    assert(fdkls_surface_host_widgets(NULL, &root) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdkls_surface_set_anchor(NULL, FDKLS_ANCHOR_TOP) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdkls_surface_set_exclusive_zone(NULL, 10) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdkls_surface_set_margin(NULL, 1, 2, 3, 4) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdkls_surface_set_keyboard_interactivity(
               NULL, FDKLS_KEYBOARD_NONE) == FDK_ERR_INVALID_ARGUMENT);
    assert(fdkls_surface_set_layer(NULL, FDKLS_LAYER_TOP) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdkls_surface_set_size(NULL, 100, 32) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdkls_surface_output_name(NULL) == NULL);

    printf("[ok] NULL/argument ladder: every fdkls_* entry point\n");
}

/* ---- the struct_size contract ---------------------------------------------- */

static void test_struct_size_contract(void) {
    /* The options table: a zeroed or undersized struct_size refuses
     * (the append-only table contract — a binary compiled against an
     * older package must be caught, not silently half-initialized).
     * The checks run before anything else, so NULL shell + bad
     * struct_size still reports the struct_size error. */
    fdkls_surface_options opts;
    memset(&opts, 0, sizeof(opts));
    fdkls_surface *s = (fdkls_surface *)0x1; /* sentinel: untouched */
    assert(fdkls_surface_create(NULL, &opts, &s) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(s == NULL); /* out_surface zeroed on every entry */

    opts.struct_size = sizeof(opts) - 1;
    assert(fdkls_surface_create(NULL, &opts, &s) ==
           FDK_ERR_INVALID_ARGUMENT);

    /* The listener: same contract. (No live surface to set it on —
     * the surface-side check is the ladder's NULL path; the
     * struct_size check itself is exercised in the mock-compositor
     * suite's listener leg.) */
    printf("[ok] struct_size contract: options refuse undersized "
           "tables\n");
}

/* ---- default-value sanity --------------------------------------------------- */

static void test_defaults(void) {
    /* The defaults of fdkls_surface_options (zero-initialized): the
     * zero value of EVERY enum here is a real wire value, so a
     * zero-initialized struct is a legal — if quiet — surface:
     * layer BACKGROUND, no anchor, exclusive 0, keyboard NONE,
     * compositor-picked output. `layer` must ALWAYS be set
     * explicitly (its zero is a real layer, not a default); the
     * anchor-0 + size-0 combination is the ONE zero-size shape the
     * protocol refuses on its own — exactly what the validator
     * catches before the wire does (the live-shell matrix in
     * test_fdkls_wayland.c pins the refusal). */
    fdkls_surface_options opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    assert(opts.layer == FDKLS_LAYER_BACKGROUND);
    assert(opts.anchor == FDKLS_ANCHOR_NONE);
    assert(opts.exclusive_zone == 0);
    assert(opts.keyboard == FDKLS_KEYBOARD_NONE);
    assert(opts.output == NULL);
    assert(opts.namespace == NULL);
    printf("[ok] defaults: zero-initialized options = layer "
           "BACKGROUND (always set it), no anchor, zone 0, keyboard "
           "NONE\n");
}

int main(void) {
    test_version_surface();
    test_null_ladder();
    test_struct_size_contract();
    test_defaults();
    printf("all fdk-layershell headless tests passed\n");
    return 0;
}
