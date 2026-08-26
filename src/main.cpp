// NxTakt — a native, session-first DAW for Linux.
#include "ui/app.h"
#include "ui/keymap.h"
#include "audio/backend.h"
#include <clocale>
#include <csignal>
#include <cstdio>
#include <cstdlib>

#if defined(__x86_64__) || defined(__i386__)
#include <pmmintrin.h>
#include <xmmintrin.h>
#endif

#ifndef NXTAKT_VERSION
#define NXTAKT_VERSION "dev"
#endif

static void usage() {
    std::printf(
        "NxTakt " NXTAKT_VERSION " — session-first DAW\n"
        "\n"
        "  nxtakt [project.lattice]\n"
        "\n"
        "Environment:\n"
        "  NXTAKT_BACKEND=wayland|x11    force a window backend\n"
        "  NXTAKT_AUDIO=jack|alsa        force an audio backend\n"
#ifdef _WIN32
        "  NXTAKT_ENGINE=local|daemon    local (the default on the Windows port)\n"
        "                                keeps the engine in-process; a daemon\n"
        "                                does not exist here yet (docs/PORTING.md)\n"
#else
        "  NXTAKT_ENGINE=daemon          the engine runs as nxtaktd — the only\n"
        "                                mode; =local was retired and now warns,\n"
        "                                then opens the daemon (GUI-ON-DAEMON.md\n"
        "                                §18)\n"
#endif
        "  NXTAKT_SCALE=1.5              override UI scale\n"
        "  (the pre-rename LATTICE_* spellings are still read; NXTAKT_* wins)\n"
        "\n");

    // THE KEYS COME OUT OF src/ui/keymap.h, which is also what F1 draws inside
    // the running program. They used to be a second copy in this string, and a
    // second copy of a keymap is a keymap that is wrong: this help text still
    // described a piano roll whose shortcuts had moved and said nothing at all
    // about the widget vocabulary every knob in the program answers to. One
    // table, two renderers, and the drift is not possible any more.
    for (int i = 0; i < lat::keys::count; ++i) {
        const lat::keys::Row& r = lat::keys::table[i];
        if (!r.keys && !r.what) { std::printf("\n"); continue; }
        if (!r.keys)            { std::printf("%s\n", r.what); continue; }
        std::printf("  %-14s %s\n", r.keys, r.what ? r.what : "");
    }
    std::printf("\n  (F1 shows this list inside the program.)\n");
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") { usage(); return 0; }
    }

    std::setlocale(LC_ALL, "");
    // LC_NUMERIC must stay in the C locale. The project format is written and
    // read with printf/strtod, so under a comma-decimal locale (de_DE, fr_FR,
    // ...) every "0.85" in a saved set would parse as 0 and every number we
    // wrote would be unreadable anywhere else.
    std::setlocale(LC_NUMERIC, "C");

    // A dying JACK/ALSA peer must not take the process with it.
    std::signal(SIGPIPE, SIG_IGN);

    // Before anything can touch ALSA -- and plugins do, whether or not ALSA is
    // our audio backend. libasound prints its own diagnostics straight to
    // stderr, in red, saying "error", and on any machine with a partial
    // /usr/share/alsa config an instrument that opens a device of its own emits
    // dozens of them. They arrive with no attribution, so they read as this
    // program crashing. Routed through our logger they are labelled, printed
    // once each, and obviously not ours.
    lat::alsaInstallLogHandler();

#if defined(__x86_64__) || defined(__i386__)
    // Denormals in feedback tails cost orders of magnitude on the audio thread.
    // These are per-thread, but the audio thread is created after this point.
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif

    // Heap-allocated: Engine carries per-track fx scratch (~2 MB), which has
    // no business on main's stack.
    auto app = std::make_unique<lat::App>();
    if (!app->init(argc, argv)) {
        std::fprintf(stderr, "nxtakt: failed to start\n");
        return 1;
    }
    app->run();
    app->shutdown();
    return 0;
}
