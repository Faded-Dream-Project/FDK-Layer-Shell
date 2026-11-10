/* test_fdkls_x11.c — the X11-refusal leg.
 *
 * The documented posture (docs/design.md §6): layer-shell is a
 * Wayland protocol; on an X11 context fdkls_shell_open returns NULL
 * with one log line — never a crash, never a half-open shell. The
 * X11 route to panels/docks (EWMH _NET_WM_WINDOW_TYPE_DOCK + struts)
 * rides FDK's own window API and needs nothing from this package.
 *
 * Needs a reachable X display (Xvfb started automatically by
 * `make test-x11` when $DISPLAY is unset).
 */

#include "fdk/fdk.h"
#include <fdkls/fdkls.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    unsetenv("WAYLAND_DISPLAY");

    fdk_context *ctx = NULL;
    fdk_result r = fdk_init(&ctx, NULL);
    if (!fdk_ok(r)) {
        printf("[skip] no X11 display reachable — nothing to refuse\n");
        return 0;
    }
    assert(fdk_ext_active_backend(ctx) == FDK_EXT_BACKEND_X11);

    fdkls_shell *shell = fdkls_shell_open(ctx);
    assert(shell == NULL && "shell_open on X11 refuses (NULL, one log line)");

    fdk_shutdown(ctx);
    printf("[ok] X11 context: shell_open refuses with NULL (EWMH dock "
           "windows are the X11 route, no package involved)\n");
    return 0;
}
