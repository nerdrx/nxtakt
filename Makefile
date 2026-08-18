# NxTakt — native Linux DAW
#
#   make            release build   -> build/nxtakt
#   make debug      -O0 -g3 + asserts
#   make run        build and launch
#   make clean
#
# Wayland is the primary window backend; X11 is kept as a runtime fallback.
# Force one with NXTAKT_BACKEND=wayland|x11 at run time.

CXX      ?= g++
CC       ?= gcc
BIN      := build/nxtakt
GEN      := build/gen

# The version the binary reports and `make dist` stamps into the artifact name.
# CI passes the tag (v0.1.0 -> 0.1.0); a working tree falls back to
# git-describe so a dev build is identifiable, and to "dev" outside a checkout.
VERSION  ?= $(shell git describe --tags --always 2>/dev/null || echo dev)

PKGS     := jack alsa sndfile samplerate gl x11 xcursor freetype2 fontconfig lilv-0
WL_PKGS  := wayland-client wayland-egl wayland-cursor egl xkbcommon

# ---- Wayland protocol discovery -------------------------------------------
# wayland-protocols proper if installed, otherwise Qt6 ships the same upstream
# XML, which is enough for wayland-scanner.
PROTO_ROOTS := $(shell pkg-config --variable=pkgdatadir wayland-protocols 2>/dev/null) \
               /usr/share/wayland-protocols /usr/share/qt6/wayland/protocols
findproto = $(firstword $(foreach r,$(PROTO_ROOTS),$(shell test -d $(r) && find $(r) -name '$(1)' 2>/dev/null | head -1)))

XDG_SHELL_XML  := $(call findproto,xdg-shell.xml)
XDG_DECO_XML   := $(call findproto,xdg-decoration-unstable-v1.xml)
FRAC_SCALE_XML := $(call findproto,fractional-scale-v1.xml)
VIEWPORTER_XML := $(call findproto,viewporter.xml)
SCANNER        := $(shell command -v wayland-scanner 2>/dev/null)

HAVE_WAYLAND := 0
ifneq ($(SCANNER),)
ifneq ($(XDG_SHELL_XML),)
ifeq ($(shell pkg-config --exists $(WL_PKGS) && echo yes),yes)
HAVE_WAYLAND := 1
endif
endif
endif

PROTO_NAMES := xdg-shell
ifneq ($(XDG_DECO_XML),)
PROTO_NAMES += xdg-decoration-unstable-v1
endif
ifneq ($(FRAC_SCALE_XML),)
PROTO_NAMES += fractional-scale-v1
endif
ifneq ($(VIEWPORTER_XML),)
PROTO_NAMES += viewporter
endif

# ---- flags ----------------------------------------------------------------
ALL_PKGS := $(PKGS)
ifeq ($(HAVE_WAYLAND),1)
ALL_PKGS += $(WL_PKGS)
endif

PKG_CF   := $(shell pkg-config --cflags $(ALL_PKGS))
PKG_LD   := $(shell pkg-config --libs $(ALL_PKGS))

WARN     := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
CXXFLAGS := -std=c++20 -fno-math-errno $(WARN) $(PKG_CF) -I$(GEN) -Ivendor/clap/include \
            -DNXTAKT_VERSION='"$(VERSION)"' -MMD -MP
CFLAGS   := -std=c11 -w $(PKG_CF) -I$(GEN) -MMD -MP
LDLIBS   := $(PKG_LD) -lpthread -lm -ldl

ifeq ($(HAVE_WAYLAND),1)
CXXFLAGS += -DLAT_HAVE_WAYLAND=1
ifneq ($(XDG_DECO_XML),)
CXXFLAGS += -DLAT_HAVE_XDG_DECORATION=1
endif
ifneq ($(FRAC_SCALE_XML),)
CXXFLAGS += -DLAT_HAVE_FRACTIONAL_SCALE=1
endif
ifneq ($(VIEWPORTER_XML),)
CXXFLAGS += -DLAT_HAVE_VIEWPORTER=1
endif
endif

ifeq ($(MAKECMDGOALS),debug)
  CXXFLAGS += -O0 -g3 -fno-omit-frame-pointer -DLAT_DEBUG=1
  CFLAGS   += -O0 -g3
else
  CXXFLAGS += -O2 -g -DNDEBUG
  CFLAGS   += -O2 -g
endif

# ---- sources --------------------------------------------------------------
SRC := $(shell find src -name '*.cpp' | sort)
ifneq ($(HAVE_WAYLAND),1)
SRC := $(filter-out src/ui/window_wayland.cpp,$(SRC))
endif
# src/daemon is a separate program with its own main(); it is built by the
# build/nxtaktd rule below and must never be swept into the GUI's link.
SRC := $(filter-out src/daemon/%,$(SRC))

OBJ := $(patsubst src/%.cpp,build/obj/%.o,$(SRC))

ifeq ($(HAVE_WAYLAND),1)
PROTO_H := $(foreach n,$(PROTO_NAMES),$(GEN)/$(n)-client-protocol.h)
PROTO_C := $(foreach n,$(PROTO_NAMES),$(GEN)/$(n)-protocol.c)
OBJ     += $(patsubst $(GEN)/%.c,build/obj/gen/%.o,$(PROTO_C))
endif

DEP := $(OBJ:.o=.d)

.PHONY: all debug run clean audio-only config

# The daemon is part of `all` since the default engine became nxtaktd
# (GUI-ON-DAEMON.md §16): a fresh `make && ./build/nxtakt` must be able to
# spawn the engine it defaults to, or every first run opens in degraded mode.
# `debug` gets the same treatment — a debug GUI still wants an engine to talk
# to, and the daemon keeps its own flags (it is a separate program).
all: $(BIN) build/nxtaktd
debug: $(BIN) build/nxtaktd

config:
	@echo "wayland backend : $(HAVE_WAYLAND)"
	@echo "  scanner       : $(SCANNER)"
	@echo "  xdg-shell     : $(XDG_SHELL_XML)"
	@echo "  xdg-decoration: $(XDG_DECO_XML)"
	@echo "  fractional    : $(FRAC_SCALE_XML)"
	@echo "  viewporter    : $(VIEWPORTER_XML)"
	@echo "  protocols     : $(PROTO_NAMES)"

# ---- protocol codegen -----------------------------------------------------
$(GEN)/xdg-shell-client-protocol.h: $(XDG_SHELL_XML)
	@mkdir -p $(GEN)
	$(SCANNER) client-header $< $@
$(GEN)/xdg-shell-protocol.c: $(XDG_SHELL_XML)
	@mkdir -p $(GEN)
	$(SCANNER) private-code $< $@

$(GEN)/xdg-decoration-unstable-v1-client-protocol.h: $(XDG_DECO_XML)
	@mkdir -p $(GEN)
	$(SCANNER) client-header $< $@
$(GEN)/xdg-decoration-unstable-v1-protocol.c: $(XDG_DECO_XML)
	@mkdir -p $(GEN)
	$(SCANNER) private-code $< $@

$(GEN)/fractional-scale-v1-client-protocol.h: $(FRAC_SCALE_XML)
	@mkdir -p $(GEN)
	$(SCANNER) client-header $< $@
$(GEN)/fractional-scale-v1-protocol.c: $(FRAC_SCALE_XML)
	@mkdir -p $(GEN)
	$(SCANNER) private-code $< $@

$(GEN)/viewporter-client-protocol.h: $(VIEWPORTER_XML)
	@mkdir -p $(GEN)
	$(SCANNER) client-header $< $@
$(GEN)/viewporter-protocol.c: $(VIEWPORTER_XML)
	@mkdir -p $(GEN)
	$(SCANNER) private-code $< $@

# ---- compile --------------------------------------------------------------
$(BIN): $(OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(OBJ) -o $@ $(LDLIBS)
	@echo "  ->  $@"

build/obj/%.o: src/%.cpp $(PROTO_H)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/obj/gen/%.o: $(GEN)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

audio-only: $(patsubst src/%.cpp,build/obj/%.o,$(wildcard src/audio/*.cpp src/core/*.cpp))
	@echo "audio layer OK"

# ---- tools and tests ------------------------------------------------------
# Deliberately standalone: none of these link the GUI, so they run headless in
# CI and stay usable when the UI is mid-refactor.
CORE_SRC  := src/core/common.cpp src/core/project.cpp src/audio/sample.cpp src/audio/engine.cpp

# The instruments internal_devices.cpp INCLUDES textually rather than links.
# They must appear as prerequisites of every target that compiles
# internal_devices.cpp, or editing an instrument leaves every tool and test
# binary stale -- the same class as the $(IPC_H) lesson below, and it bit
# live: a red-proof run once reported PASSes against a binary that had never
# seen the revert. They ride in $^ as sources too, which is harmless BY
# CONSTRUCTION: standalone, their include guards compile them to empty
# translation units.
INTERNAL_INSTR := src/plugin/spectra.cpp src/plugin/sampler.cpp \
                  src/plugin/fx_shimmer.cpp src/plugin/fx_bloom.cpp src/plugin/fx_tape.cpp
# Data an instrument includes textually (spectra_presets.inc, the Spectra v2
# factory bank). Same staleness class as INTERNAL_INSTR itself: it must be a
# prerequisite of every target that compiles internal_devices.cpp, or editing
# a preset leaves every tool and test binary stale. It is NOT a source, so
# recipes that used $^ raw now filter %.cpp — the idiom internal_device_test
# and handle_test already use, for the same reason.
INTERNAL_DATA := src/plugin/spectra_presets.inc
TOOL_LIBS := $(shell pkg-config --libs sndfile samplerate lilv-0) -ldl -lpthread -lm
TOOL_CF   := -std=c++20 -O2 -w $(shell pkg-config --cflags sndfile samplerate lilv-0) -Ivendor/clap/include

.PHONY: tools test
tools: build/gen_demo build/render build/pitch_check build/plugin_scan

build/gen_demo: tools/gen_demo.cpp $(CORE_SRC)
	@mkdir -p build
	$(CXX) $(TOOL_CF) $^ -o $@ $(TOOL_LIBS)
# render materialises a project's device chains, so unlike gen_demo it needs the
# plugin backends linked in.
build/render: tools/render.cpp $(CORE_SRC) src/plugin/host.cpp src/plugin/lv2_host.cpp \
              src/plugin/clap_host.cpp src/plugin/internal_devices.cpp $(INTERNAL_INSTR) $(INTERNAL_DATA)
	@mkdir -p build
	$(CXX) $(TOOL_CF) $(filter %.cpp,$^) -o $@ $(TOOL_LIBS)
build/pitch_check: tools/pitch_check.cpp
	@mkdir -p build
	$(CXX) $(TOOL_CF) $^ -o $@ $(TOOL_LIBS)
build/plugin_scan: tools/plugin_scan.cpp src/plugin/host.cpp src/plugin/lv2_host.cpp src/plugin/clap_host.cpp src/plugin/internal_devices.cpp src/core/common.cpp $(INTERNAL_INSTR) $(INTERNAL_DATA)
	@mkdir -p build
	$(CXX) $(TOOL_CF) $(filter %.cpp,$^) -o $@ $(TOOL_LIBS)
build/engine_test: tests/engine_test.cpp src/audio/engine.cpp src/core/common.cpp
	@mkdir -p build
	$(CXX) $(TOOL_CF) $^ -o $@ $(TOOL_LIBS)

# src/ipc is header-only and depends on libc alone, so this one deliberately
# does not use TOOL_CF/TOOL_LIBS: no sndfile, no lilv, and warnings left on.
# -lrt is only needed for shm_open on glibc < 2.34; harmless after.
IPC_CF := -std=c++20 -O2 $(WARN)
IPC_H  := src/ipc/shm.h src/ipc/pool.h src/ipc/control.h src/ipc/client.h \
          src/ipc/take.h
# ALL FIVE headers, not just shm.h. The short list let ipc_test go stale: a
# protocol bump in control.h never triggered a rebuild, every local run tested
# yesterday's binary against today's headers, and the mismatch only surfaced on
# a fresh CI build -- where it cost three red runs to locate from outside.
build/ipc_test: tests/ipc_test.cpp $(IPC_H)
	@mkdir -p build
	$(CXX) $(IPC_CF) $< -o $@ -lrt -lpthread

# The engine daemon: Engine + a backend + the control region + the plugin
# layer, and no GUI. Phase 3 is where src/plugin joins the link: the daemon
# owns every PluginInstance now, so it needs the backends (lilv for LV2, dl for
# CLAP) that phases 1 and 2 could do without. Still no GUI, no window system and
# no sndfile — nxtaktd renders, it does not decode or draw.
DAEMON_SRC := src/daemon/nxtaktd.cpp src/audio/engine.cpp src/audio/backend.cpp \
              src/core/common.cpp \
              src/plugin/host.cpp src/plugin/lv2_host.cpp src/plugin/clap_host.cpp \
              src/plugin/internal_devices.cpp $(INTERNAL_INSTR)
DAEMON_CF  := -std=c++20 -O2 $(WARN) -Ivendor/clap/include \
              $(shell pkg-config --cflags jack alsa lilv-0)
DAEMON_LD  := $(shell pkg-config --libs jack alsa lilv-0) -ldl -lrt -lpthread -lm

# src/audio/sample.h joined the list with generic device state (v10): the daemon
# builds a SampleBuffer from bytes the GUI decoded, so it now depends on that
# struct's LAYOUT while still linking none of sample.cpp. A header it compiles
# against and does not list is the $(IPC_H) lesson with a different filename.
build/nxtaktd: $(DAEMON_SRC) $(INTERNAL_DATA) $(IPC_H) src/audio/engine.h src/audio/backend.h \
               src/plugin/host.h src/audio/sample.h
	@mkdir -p build
	$(CXX) $(DAEMON_CF) $(DAEMON_SRC) -o $@ $(DAEMON_LD)

# daemon_test spawns ./build/nxtaktd, so the binary is a build dependency of
# the test rather than something the test is trusted to find.
build/daemon_test: tests/daemon_test.cpp $(IPC_H) src/audio/engine.h build/nxtaktd
	@mkdir -p build
	$(CXX) $(IPC_CF) $< -o $@ -lrt -lpthread

# The internal devices, exercised through the same PluginInstance contract every
# third-party plugin goes through. This had no target for a long time and its
# header said "built by hand" -- which meant several hundred assertions that CI
# had never once run, and a suite nobody runs is a suite that rots. It needs the
# plugin backends linked because host.cpp reaches into both of them.
#
# The header list is the $(IPC_H) lesson again: this recipe had none, so a change
# to the PluginInstance/SamplerControl contract in host.h left the binary stale
# and every local run tested yesterday's devices against today's interface. The
# recipe filters %.cpp out of $^ for the same reason handle_test's does — a .h
# handed to g++ is compiled as a precompiled header, not ignored.
build/internal_device_test: tests/internal_device_test.cpp src/plugin/host.cpp \
                            src/plugin/lv2_host.cpp src/plugin/clap_host.cpp \
                            src/plugin/internal_devices.cpp src/core/common.cpp $(INTERNAL_INSTR) $(INTERNAL_DATA) \
                            src/plugin/host.h src/audio/sample.h
	@mkdir -p build
	$(CXX) $(TOOL_CF) $(filter %.cpp,$^) -o $@ $(shell pkg-config --libs lilv-0) -ldl

# The view's bar grid against the engine's own bar arithmetic. Worth its own
# binary rather than folding into engine_test: the property under test is that
# two INDEPENDENT pieces of code agree -- every bar line the arrangement ruler
# draws is a downbeat the engine would play -- so it has to link the view's
# time axis and the engine together and assert they cannot diverge. Bars stopped
# being uniform when signature changes landed, which is exactly when a drawn
# grid and a played one become able to disagree silently.
build/timesig_view_test: tests/timesig_view_test.cpp src/audio/engine.cpp \
                         src/audio/sample.cpp src/core/common.cpp \
                         src/plugin/host.cpp src/plugin/lv2_host.cpp \
                         src/plugin/clap_host.cpp src/plugin/internal_devices.cpp $(INTERNAL_INSTR) $(INTERNAL_DATA)
	@mkdir -p build
	$(CXX) $(TOOL_CF) $(filter %.cpp,$^) -o $@ $(TOOL_LIBS) -ldl

# EngineHandle across both of its backings: the in-process engine and a real
# nxtaktd. Warnings stay on (it is our code, not a tool wrapper) and it links no
# sndfile, because the handle is the seam and the seam has no business knowing
# how a wav is decoded.
#
# It DOES link src/plugin, as of the rack-contents wave, and that is a change
# worth a sentence. The suite's own note says the FakeDevice trick works because
# "host.h is a header and the vtable is ours" — true for a plugin, and not true
# for a RACK: `rackStateToString` is defined in internal_devices.cpp, and a fake
# RackControl would only prove that the handle can serialise a fake. What has to
# be tested is a real `nxtakt:rack` holding a real `nxtakt:saturator`, whose
# state string the real serialiser produced and the daemon's real
# rackStateFromString read back. Nothing here starts a scan, so lilv is a link
# dependency and not a runtime cost.
#
# It spawns its own daemon, so it depends on one existing.
# The header list is not decoration and it is not complete by luck: this recipe
# had NONE, so a protocol bump in src/ipc left the binary stale and every local
# run tested yesterday's seam against today's headers. That is exactly what cost
# build/ipc_test three red CI runs to locate; the note above $(IPC_H) is the
# other half of the same lesson. Recording added src/ipc/take.h to that list and
# engine_handle.h/engine_state.h to this one.
build/handle_test: tests/handle_test.cpp src/ui/engine_handle.cpp src/audio/engine.cpp \
                   src/audio/backend.cpp src/audio/midi_in.cpp src/core/common.cpp \
                   src/plugin/host.cpp src/plugin/lv2_host.cpp src/plugin/clap_host.cpp \
                   src/plugin/internal_devices.cpp $(INTERNAL_INSTR) $(INTERNAL_DATA) \
                   $(IPC_H) src/ui/engine_handle.h src/ui/engine_state.h \
                   src/audio/engine.h src/plugin/host.h src/audio/sample.h \
                   build/nxtaktd
	@mkdir -p build
	$(CXX) -std=c++20 -O2 $(WARN) -I. -Ivendor/clap/include \
	  $(shell pkg-config --cflags lilv-0 alsa) $(filter %.cpp,$^) -o $@ \
	  $(shell pkg-config --libs jack alsa lilv-0) -ldl -lrt -lpthread -lm

# Full headless check: engine unit tests, then a real render that must not be
# silent, then a plugin scan.
test: build/engine_test build/ipc_test build/daemon_test build/internal_device_test \
      build/timesig_view_test build/handle_test build/render build/gen_demo build/plugin_scan
	./build/engine_test
	./build/ipc_test
	./build/daemon_test
	./build/internal_device_test
	./build/timesig_view_test
	./build/handle_test
	./build/gen_demo /tmp/nxtakt-selftest >/dev/null
	./build/render /tmp/nxtakt-selftest/demo.lattice /tmp/nxtakt-selftest/render.wav --scene 2 --bars 2
	./build/plugin_scan | tail -3
	@echo "ALL CHECKS PASSED"

run: $(BIN)
	./$(BIN)

# ---- release artifact ------------------------------------------------------
# One Linux tarball, shaped for NX Hub's discovery (SPEC.md): the asset name
# contains "linux" and ends .tar.gz, so the hub classifies it archive-dir; the
# binary heuristic matches the file named like the repo, so `nxtakt` is what a
# desktop entry will point at; nxtaktd rides in the SAME directory because
# daemon mode finds it beside /proc/self/exe; icon.svg is what the hub extracts
# for the entry; and the sibling .sha256 is verified by the hub on download.
#
# gen_demo and render are included on purpose: a fresh install with no set to
# open is a blank screen, and `./gen_demo ~/Music/Demo` is the two-second fix.
#
# CI passes VERSION from the tag. The tarball unpacks into a versioned
# directory rather than loose files, because loose files in ~/Downloads is what
# cheap software does.
DISTDIR := build/dist/nxtakt-$(VERSION)-linux-x86_64
dist: all build/nxtaktd build/gen_demo build/render
	rm -rf build/dist
	mkdir -p $(DISTDIR)
	cp build/nxtakt build/nxtaktd build/gen_demo build/render $(DISTDIR)/
	cp README.md LICENSE $(DISTDIR)/
	cp assets/logo.svg $(DISTDIR)/icon.svg
	printf '%s\n' \
	  "NxTakt $(VERSION) — Linux x86_64" \
	  "" \
	  "  ./nxtakt [set.lattice]        the DAW" \
	  "  ./gen_demo ~/Music/Demo       write a demo set to open" \
	  "  ./nxtaktd                     the engine as its own process (optional)" \
	  "  ./render set.lattice out.wav  offline render" \
	  "" \
	  "Runtime libraries (from your distro, all common): jack or alsa, sndfile," \
	  "samplerate, freetype, fontconfig, GL, lilv; wayland or X11." \
	  > $(DISTDIR)/START-HERE.txt
	tar -C build/dist -czf build/dist/nxtakt-$(VERSION)-linux-x86_64.tar.gz \
	  nxtakt-$(VERSION)-linux-x86_64
	cd build/dist && sha256sum nxtakt-$(VERSION)-linux-x86_64.tar.gz \
	  > nxtakt-$(VERSION)-linux-x86_64.tar.gz.sha256
	@ls -la build/dist/*.tar.gz*
	@echo "  ->  build/dist/nxtakt-$(VERSION)-linux-x86_64.tar.gz"

.PHONY: dist

clean:
	rm -rf build/obj build/gen $(BIN)

-include $(DEP)
