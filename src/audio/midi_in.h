// ALSA sequencer MIDI input.
//
// Threading contract:
//   * start()/stop() are GUI-thread only. They open/close the sequencer client
//     and own the reader thread.
//   * the reader thread does nothing but block in snd_seq_event_input() and
//     hand translated messages to the SINK. It never touches engine state
//     directly and it never allocates.
//
// THE SINK, AND WHY IT IS NOT AN Engine& ANY MORE
// -----------------------------------------------
// This class used to take an `Engine&` and call `Engine::pushMidi()`. That is
// exactly one destination, and it is the one destination that does not exist in
// daemon mode: there is no in-process Engine there, so hardware MIDI was simply
// not connected (docs/GUI-ON-DAEMON.md §1.3, "not connected"). §1.3 lists three
// ways out — move the reader into nxtaktd, hand off through a GUI-owned queue,
// or put a lock around the two producers — and rejects the middle one because it
// costs a frame of latency on an instrument.
//
// What is here is the first and third combined at the seam rather than in the
// daemon: the reader hands each message to a callable the CALLER supplies, so
// `EngineHandle` routes it to `Engine::pushMidi` locally and to the shared-memory
// MIDI ring remotely. That keeps the ALSA client in the process the user's
// aconnect wiring already names, and it makes the destination a decision of the
// one object that knows where the engine is.
//
// Two rules the sink must satisfy, and both are the caller's to keep:
//
//   1. IT IS CALLED FROM THE READER THREAD, never from the GUI thread. On the
//      daemon path that ring has a second producer (the computer keyboard and
//      the note previews, pushed from the GUI thread) and lat::Ring is
//      single-producer, so the sink there takes a lock. Neither producer is
//      realtime, so a lock is free of consequence — this is §1.3's option 3.
//   2. IT MUST OUTLIVE THE READER. stop() joins the thread BEFORE it drops the
//      sink, so anything the sink captures need only be alive until stop()
//      returns.
//
// Nothing is auto-connected: a DAW that grabs every keyboard on the system is a
// nuisance in a JACK/PipeWire graph. The client:port id is logged so the user
// can wire it up with aconnect or qpwgraph.
//
// Constructing a MidiInput does nothing at all; until start() is called there
// is no client, no thread and no cost.
#pragma once
#include "../core/common.h"
#include "engine.h"          // MidiMsg, and Engine for the convenience overload
#include <atomic>
#include <functional>
#include <thread>

namespace lat {

class MidiInput {
public:
    ~MidiInput() { stop(); }

    // Returns true if the message was accepted. A false is a full ring and is
    // counted nowhere: received() is the count that actually reached an engine,
    // which is the number a status bar means.
    using Sink = std::function<bool(const MidiMsg&)>;

    // Opens the sequencer client and spins up the reader thread. Returns false
    // (and stays fully inert) if ALSA sequencer support is unavailable, which
    // is the normal case in containers and on kernels without snd-seq, or if
    // `sink` is empty — a reader with nowhere to put its messages is a thread
    // that costs a wakeup ten times a second to throw notes away.
    bool start(Sink sink);

    // The in-process destination, spelled out. Kept because it is what every
    // local caller means and because a lambda at each call site would be three
    // copies of the same one-liner.
    bool start(Engine& e);

    void stop();

    bool running() const { return running_.load(std::memory_order_relaxed); }
    // ALSA client id of our sequencer client, -1 when not running. The input
    // port is always port 0, so the wiring target is "<clientId()>:0".
    int  clientId() const { return client_; }
    // Messages accepted since start(); the sink drops on overflow, so this is
    // the count that actually reached the engine.
    u64  received() const { return received_.load(std::memory_order_relaxed); }

private:
    void run();

    void*             seq_     = nullptr;   // snd_seq_t*, opaque here so this
                                            // header pulls in no ALSA headers.
    Sink              sink_;                // written before the thread starts,
                                            // cleared after it is joined
    std::thread       thread_;
    std::atomic<bool> running_{false};
    std::atomic<u64>  received_{0};
    int               client_  = -1;
    int               port_    = -1;
};

} // namespace lat
