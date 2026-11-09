# Makefile — FDK-Layer-Shell
#
# The build mirrors FDK's own (the package is an FDK add-on and keeps
# the same quality gates: the same warning set, a debug configuration
# under ASan/UBSan by default, a generated export map, and an export
# verification that fails the build when anything outside the
# package's namespace leaks).
#
# Targets:
#   make            build static + shared library (debug)
#   make release    build with optimizations, NDEBUG defined
#   make static     static library only (libfdk-layershell.a)
#   make shared     shared library only (libfdk-layershell.so)
#   make test       build and run the headless test suite (no display
#                   needed — safe for plain CI)
#   make test-wayland  build and run the mock-compositor integration
#                   suite (libwayland-server drives a real in-process
#                   Wayland server; no desktop compositor needed)
#   make test-x11   the X11-refusal leg (needs a reachable X display;
#                   Xvfb is started automatically when $DISPLAY is unset)
#   make examples   build the example programs (against the static lib)
#   make install    install headers + libraries to PREFIX (default /usr/local)
#   make uninstall  remove what `install` installed
#   make clean      remove build output
#   make verify-exports  check the .so exports exactly the fdkls_* API
#
# Override on the command line, e.g.:
#   make release PREFIX=/usr
#
# Dependencies (pkg-config):
#   fdk             FDK 1.0.0+ — the extension API this package builds
#                   on (make install of the FDK tree puts fdk.pc in
#                   place; point PKG_CONFIG_PATH at it if FDK went to
#                   a custom prefix)
#   wayland-client  libwayland-client (the wire the package speaks)
#   wayland-server  libwayland-server — ONLY for the mock-compositor
#                   test suite (make test-wayland); the library
#                   itself never touches it

CC       ?= gcc
AR       ?= ar
PREFIX   ?= /usr/local
LIBDIR   ?= $(PREFIX)/lib
INCDIR   ?= $(PREFIX)/include
# Version for the pkg-config file and the shared-library SONAME;
# kept in one place, mirroring include/fdkls/fdkls.h (the header is
# the source of truth — a mismatch is a release-blocker).
FDKLS_PC_VERSION := $(shell sed -n 's/^#define FDKLS_VERSION_STRING "\(.*\)"/\1/p' include/fdkls/fdkls.h)
FDKLS_VERSION_MAJOR := $(shell sed -n 's/^#define FDKLS_VERSION_MAJOR \([0-9]*\).*/\1/p' include/fdkls/fdkls.h)
FDKLS_VERSION_MINOR := $(shell sed -n 's/^#define FDKLS_VERSION_MINOR \([0-9]*\).*/\1/p' include/fdkls/fdkls.h)
FDKLS_VERSION_PATCH := $(shell sed -n 's/^#define FDKLS_VERSION_PATCH \([0-9]*\).*/\1/p' include/fdkls/fdkls.h)
FDKLS_SO_VERSION := $(FDKLS_VERSION_MAJOR).$(FDKLS_VERSION_MINOR).$(FDKLS_VERSION_PATCH)
FDKLS_SONAME := libfdk-layershell.so.$(FDKLS_VERSION_MAJOR)

# The extension API version this package pins (compile-time guard in
# include/fdkls/fdkls.h; the run-time half lives in fdkls__shell_open).
FDK_EXT_API_REQUIRED := $(shell sed -n 's/^#define FDK_EXT_API_VERSION \([0-9]*\).*/\1/p' include/fdkls/fdkls.h)

# ---- Dependencies ---------------------------------------------------------

FDK_PKGS      := fdk
HAVE_FDK      := $(shell pkg-config --exists $(FDK_PKGS) && echo yes || echo no)
HAVE_WAYLAND  := $(shell pkg-config --exists wayland-client && echo yes || echo no)
HAVE_WLSERVER := $(shell pkg-config --exists wayland-server && echo yes || echo no)

ifeq ($(HAVE_FDK),no)
  $(error pkg-config did not find fdk — build and install FDK 1.0.0+ first (make install in the FDK tree), then point PKG_CONFIG_PATH at its lib/pkgconfig)
endif
ifeq ($(HAVE_WAYLAND),no)
  $(error pkg-config did not find wayland-client — install libwayland-dev)
endif

FDK_CFLAGS  := $(shell pkg-config --cflags $(FDK_PKGS))
FDK_LIBS    := $(shell pkg-config --libs $(FDK_PKGS))
WAYLAND_CFLAGS := $(shell pkg-config --cflags wayland-client)
WAYLAND_LIBS   := $(shell pkg-config --libs wayland-client)
ifeq ($(HAVE_WLSERVER),yes)
  WLSERVER_CFLAGS := $(shell pkg-config --cflags wayland-server)
  WLSERVER_LIBS   := $(shell pkg-config --libs wayland-server)
endif

STD      := -std=c17
WARN     := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
            -Wmissing-prototypes -Wconversion -Wsign-conversion \
            -Wcast-qual -Wpointer-arith -Wundef -Wwrite-strings
FEATURE  := -D_POSIX_C_SOURCE=200809L

# Sanitizers are the debug default on glibc toolchains (the same
# posture as FDK's Makefile); FDK_SANITIZE=0 builds the same debug
# configuration without -fsanitize for musl/minimal environments.
FDK_SANITIZE ?= 1
ifeq ($(FDK_SANITIZE),0)
  DEBUG_FLAGS := -g -O0
else
  DEBUG_FLAGS := -g -O0 -fsanitize=address,undefined
endif
REL_FLAGS   := -O2 -DNDEBUG

FDK_CONFIG  ?= debug
CONFIG_FLAGS := $(if $(filter release,$(FDK_CONFIG)),$(REL_FLAGS),$(DEBUG_FLAGS))

ENV_CFLAGS  := $(CFLAGS)
ENV_LDFLAGS := $(LDFLAGS)
CFLAGS  := $(STD) $(WARN) $(FEATURE) $(CONFIG_FLAGS) $(ENV_CFLAGS)
CPPFLAGS:= -I. -Iinclude -Isrc $(FDK_CFLAGS)
# wayland-client for the library; the generated scanner code and the
# listener-registration inlines both trigger -Wcast-qual upstream —
# the same per-TU suppression FDK's own Wayland backend uses (nothing
# else is suppressed anywhere).
LDFLAGS := $(FDK_LIBS) $(WAYLAND_LIBS) -lm $(ENV_LDFLAGS)

BUILD_DIR   := build

LIB_SRCS     := src/fdkls_shell.c src/fdkls_surface.c \
                generated/wlr-layer-shell-unstable-v1-protocol.c \
                generated/xdg-shell-protocol.c
LIB_OBJS     := $(patsubst %.c,$(BUILD_DIR)/obj/%.o,$(LIB_SRCS))
LIB_OBJS_PIC := $(patsubst %.c,$(BUILD_DIR)/obj-pic/%.o,$(LIB_SRCS))

STATIC_LIB := $(BUILD_DIR)/libfdk-layershell.a
SHARED_LIB := $(BUILD_DIR)/libfdk-layershell.so

# Per-source extra flags: everything that touches the wayland headers
# (all of src/, the generated scanner code, the mock-compositor test)
# needs the pkg-config include path and the -Wno-cast-qual upstream
# suppression. Plain libc sources (none today — every TU speaks the
# protocol) would take neither.
extra_flags = $(if $(findstring generated/,$(1)),$(WAYLAND_CFLAGS) -Wno-cast-qual) \
              $(if $(findstring src/,$(1)),$(WAYLAND_CFLAGS) -Wno-cast-qual)

# The export map, generated exactly like FDK's own (docs/EXTENDING.md
# §11: "Your package's shared library should export only its own
# namespace"): the intersection of (fdkls_* names declared in
# include/fdkls/*.h) and (globals the static archive actually
# defines). Everything else — the scanner interface structs
# (zwlr_*, wl_*), internal fdkls__* seams — lands in `local:` and is
# hidden from the dynamic table.
EXPORT_MAP := $(BUILD_DIR)/libfdk-layershell.exports.map

# Header style constraint the generator depends on (FDK's own): a
# public function's name and its opening paren sit on ONE line.
WL_TEST_SRC := tests/test_fdkls_wayland.c
WL_TEST_BIN := $(BUILD_DIR)/tests/test_fdkls_wayland

X11_TEST_SRC := tests/test_fdkls_x11.c
X11_TEST_BIN := $(BUILD_DIR)/tests/test_fdkls_x11

DEPS := $(LIB_OBJS:.o=.d) $(LIB_OBJS_PIC:.o=.d)

.PHONY: all release static shared test test-wayland test-x11 examples \
        install uninstall clean verify-exports

.DEFAULT_GOAL := all

all: static shared

release:
	$(MAKE) FDK_CONFIG=release all

static: $(STATIC_LIB)
shared: $(SHARED_LIB)

$(BUILD_DIR)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(call extra_flags,$<) -MMD -MP -c $< -o $@

$(BUILD_DIR)/obj-pic/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(call extra_flags,$<) -MMD -MP -fPIC -c $< -o $@

$(STATIC_LIB): $(LIB_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(AR) rcs $@ $^

$(SHARED_LIB): $(LIB_OBJS_PIC) $(EXPORT_MAP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -shared -Wl,-soname,$(FDKLS_SONAME) -Wl,--version-script=$(EXPORT_MAP) -o $@ $(LIB_OBJS_PIC) $(LDFLAGS)
	ln -sf libfdk-layershell.so $(BUILD_DIR)/$(FDKLS_SONAME)

$(EXPORT_MAP): $(STATIC_LIB) $(wildcard include/fdkls/*.h) Makefile
	@echo "== generating $(EXPORT_MAP) (the fdkls_* API only) =="
	@printf '{\n  global:\n' > $@
	@{ nm --defined-only $(STATIC_LIB) \
	           | awk '$$2 ~ /^[TDBRW]$$/ { print $$3 }' | sort -u; \
	   grep -hoE '\bfdkls_[a-z0-9_]+[[:space:]]*\(' include/fdkls/*.h \
	           | tr -d ' (' | sort -u; } \
	  | sort | uniq -d | sed 's/^/    /; s/$$/;/' >> $@
	@printf '  local:\n    *;\n};\n' >> $@

verify-exports: $(SHARED_LIB)
	@echo "== verifying the shared library's exported surface =="
	@exports=$$(nm -D --defined-only $(SHARED_LIB) \
	           | awk '$$2 ~ /^[TDBRW]$$/ { print $$3 }' | sort -u); \
	echo "exported symbols: $$(printf '%s\n' "$$exports" | wc -l)"; \
	bad=$$(printf '%s\n' "$$exports" | grep -v '^fdkls_' || true); \
	if [ -n "$$bad" ]; then \
	        echo "FAIL: non-public exports leaked into the .so (the fdkls_ namespace rule):"; \
	        printf '%s\n' "$$bad"; exit 1; \
	fi; \
	echo "EXPORT VERIFICATION OK: the .so exports exactly the fdkls_* API"

# --- Tests -----------------------------------------------------------------
#
# Headless first (plain CI, no display): the version surface, the
# argument/NULL ladder of every entry point, and the option
# validation matrix (every rule that keeps a protocol error off the
# wire). The mock-compositor suite (make test-wayland) drives a REAL
# in-process Wayland server over a real unix socket — the full
# handshake, configure/ack/notify, the first buffer, live state
# updates, the closed path — with libwayland-server answering on the
# other end. See docs/testing.md (docs/design.md's testing section).

TEST_SRCS := tests/test_fdkls_api.c
TEST_BINS := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%,$(TEST_SRCS))

test: $(TEST_BINS)
	@echo "== running tests =="
	@for t in $(TEST_BINS); do \
		echo "-- $$t --"; \
		"$$t" || exit 1; \
	done
	@echo "== all tests passed =="

$(BUILD_DIR)/tests/%: tests/%.c $(STATIC_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FDK_CFLAGS) $(WAYLAND_CFLAGS) -Wno-cast-qual $< $(STATIC_LIB) -o $@ $(LDFLAGS)

# The mock-compositor integration suite. Requires libwayland-server
# (dev) — skips honestly when absent, so packaging containers without
# it stay green; CI installs it and the skip is not taken there.
.PHONY: test-wayland
test-wayland:
ifeq ($(HAVE_WLSERVER),yes)
	@mkdir -p $(BUILD_DIR)/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FDK_CFLAGS) $(WAYLAND_CFLAGS) $(WLSERVER_CFLAGS) -Wno-cast-qual $(WL_TEST_SRC) $(STATIC_LIB) -o $(WL_TEST_BIN) $(LDFLAGS) $(WLSERVER_LIBS)
	@echo "== running the mock-compositor integration suite =="
	XDG_RUNTIME_DIR=$${XDG_RUNTIME_DIR:-/tmp/fdkls-runtime-$$$$} \
	    sh -c 'mkdir -p "$$XDG_RUNTIME_DIR" && chmod 700 "$$XDG_RUNTIME_DIR" && exec $(WL_TEST_BIN)'
else
	@echo "[skip] wayland-server dev files not available — the mock-compositor suite needs libwayland-server"
endif

# The X11-refusal leg: fdkls_shell_open on an X11 context must return
# NULL with one log line (the documented posture). Uses $DISPLAY when
# set; otherwise starts and tears down a throwaway Xvfb (like FDK's
# own test-x11).
.PHONY: test-x11
test-x11: $(X11_TEST_BIN)
	@if ! command -v Xvfb >/dev/null 2>&1 && [ -z "$$DISPLAY" ]; then \
		echo "[skip] no DISPLAY and no Xvfb — the X11-refusal leg needs one"; exit 0; fi
	@if [ -n "$$DISPLAY" ]; then \
		echo "== running X11-refusal test against DISPLAY=$$DISPLAY =="; \
		"$(X11_TEST_BIN)" || exit 1; \
	else \
		XVFB_NUM=$$((90 + ($$$$ % 400))); \
		XVFB_DISP=":$$XVFB_NUM"; \
		rm -f "/tmp/.X11-unix/X$$XVFB_NUM"; \
		Xvfb "$$XVFB_DISP" -screen 0 1024x768x24 >/tmp/fdkls-xvfb.log 2>&1 & \
		XVFB_PID=$$!; \
		sleep 2; \
		echo "== running X11-refusal test against Xvfb :$$XVFB_NUM =="; \
		DISPLAY="$$XVFB_DISP" "$(X11_TEST_BIN)"; \
		TEST_RESULT=$$?; \
		kill "$$XVFB_PID" 2>/dev/null; \
		wait "$$XVFB_PID" 2>/dev/null; \
		rm -f "/tmp/.X11-unix/X$$XVFB_NUM"; \
		exit $$TEST_RESULT; \
	fi

$(X11_TEST_BIN): $(X11_TEST_SRC) $(STATIC_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FDK_CFLAGS) $(WAYLAND_CFLAGS) -Wno-cast-qual $< $(STATIC_LIB) -o $@ $(LDFLAGS)

# --- Examples ---------------------------------------------------------
#
# Built against the static lib like FDK's (real applications link
# against the installed shared one; examples prove the API).

EXAMPLE_SRCS := $(wildcard examples/*.c)
EXAMPLE_BINS := $(patsubst examples/%.c,$(BUILD_DIR)/examples/%,$(EXAMPLE_SRCS))

examples: $(EXAMPLE_BINS)

$(BUILD_DIR)/examples/%: examples/%.c $(STATIC_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FDK_CFLAGS) $(WAYLAND_CFLAGS) -Wno-cast-qual $< $(STATIC_LIB) -o $@ $(LDFLAGS)

# --- pkg-config ------------------------------------------------------------

$(BUILD_DIR)/fdk-layershell.pc: fdk-layershell.pc.in include/fdkls/fdkls.h Makefile \
                               $(BUILD_DIR)/fdk-layershell.pc.prefix
	@mkdir -p $(BUILD_DIR)
	@sed -e 's|@PREFIX@|$(PREFIX)|g' \
	     -e 's|@LIBDIR@|$(LIBDIR)|g' \
	     -e 's|@INCDIR@|$(INCDIR)|g' \
	     -e 's|@VERSION@|$(FDKLS_PC_VERSION)|g' fdk-layershell.pc.in > $@

$(BUILD_DIR)/fdk-layershell.pc.prefix: FORCE
	@mkdir -p $(BUILD_DIR)
	@cur=`cat $@ 2>/dev/null || echo none`; \
	if [ x"$$cur" != x"prefix=$(PREFIX)|libdir=$(LIBDIR)|incdir=$(INCDIR)" ]; then \
		printf '%s\n' 'prefix=$(PREFIX)|libdir=$(LIBDIR)|incdir=$(INCDIR)' > $@; \
	fi

# --- Install ---------------------------------------------------------------

install: all $(BUILD_DIR)/fdk-layershell.pc
	install -d $(DESTDIR)$(INCDIR)/fdkls
	install -m 644 include/fdkls/fdkls.h $(DESTDIR)$(INCDIR)/fdkls/
	install -d $(DESTDIR)$(LIBDIR)
	install -m 644 $(STATIC_LIB) $(DESTDIR)$(LIBDIR)/
	install -m 755 $(SHARED_LIB) $(DESTDIR)$(LIBDIR)/libfdk-layershell.so.$(FDKLS_SO_VERSION)
	ln -sf libfdk-layershell.so.$(FDKLS_SO_VERSION) $(DESTDIR)$(LIBDIR)/$(FDKLS_SONAME)
	ln -sf $(FDKLS_SONAME) $(DESTDIR)$(LIBDIR)/libfdk-layershell.so
	install -d $(DESTDIR)$(LIBDIR)/pkgconfig
	install -m 644 $(BUILD_DIR)/fdk-layershell.pc \
	               $(DESTDIR)$(LIBDIR)/pkgconfig/fdk-layershell.pc

uninstall:
	rm -rf $(DESTDIR)$(INCDIR)/fdkls
	rm -f $(DESTDIR)$(LIBDIR)/libfdk-layershell.a
	rm -f $(DESTDIR)$(LIBDIR)/libfdk-layershell.so
	rm -f $(DESTDIR)$(LIBDIR)/$(FDKLS_SONAME)
	rm -f $(DESTDIR)$(LIBDIR)/libfdk-layershell.so.$(FDKLS_SO_VERSION)
	rm -f $(DESTDIR)$(LIBDIR)/pkgconfig/fdk-layershell.pc

clean:
	rm -rf $(BUILD_DIR)

.PHONY: FORCE
FORCE:

-include $(DEPS)
