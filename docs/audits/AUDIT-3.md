# Audit 3 — the recording wire, the pool blobs, and the rack container

Adversarial read of the code that landed after `IPC-AUDIT.md` and `RT-AUDIT.md`:
the take path (daemon-written take files, `src/ipc/take.h`, the take worker
thread), the arrangement / signature / rack-state pool blobs, the seqlock
mirror, the lifecycle path, and Spectra.

Method as before: read it as an attacker, not as a reviewer. Every finding below
either has a test that failed before the fix and passes after it, or a sanitizer
report with the same shape. Nothing here is an opinion about style.

Scope of the fixes: `src/ipc/**`, `src/daemon/**`, `src/plugin/**`,
`src/ui/engine_handle.*`, `src/ui/engine_state.h` and their suites.
`src/audio/engine.cpp` belonged to another agent this wave, so engine-side
findings are FILED with the exact diff rather than applied.

---

## Summary

| # | Severity | What | State |
|---|---|---|---|
| 1 | **CRITICAL** | A cancelled take was erased while its finish event was still in flight — buffer freed under the engine, and an ABA away from a silent take loss plus a UAF on the audio thread | FIXED, red→green |
| 2 | **CRITICAL** | `Rack` rewrote the `Layout` the audio thread was reading; `setState()` wraps the four-slot ring six times in microseconds | FIXED, TSan red→green |
| 3 | MAJOR | A full client event ring silently **destroyed** engine events, one per pump tick, with no counter | FIXED, red→green |
| 4 | MAJOR | A take capacity was bounded *after* the cast to `i64`; `(u64)INT64_MIN * 8` wraps to 0, so the extent check passed | FIXED, red→green |
| 5 | MAJOR | A flood of take *refusals* could crowd out a take *announcement*, stranding the take and pinning the client's capture buffer forever; the documented retry did not exist | FIXED, guard test |
| 6 | MINOR | `countNxTaktShm()` counted every `nxtakt` region on the machine, so the leak check failed on a clean tree when anything else was running | FIXED |
| F1 | FILED | `engine.cpp`: the RecordSlot *retarget* and *second hand-over* paths strand a capture buffer with no event | filed, diff below |
| F2 | FILED | `PluginInstance::setParam` / `setBypassed` are plain non-atomic scalars read by the audio thread — the last data races TSan reports | filed, design |
| F3 | FILED | Spectra's `midi()` queue drops on overflow, and it drops *panics* too, so a flood can leave voices stuck on | filed, design |
| F4 | FILED | A second client attaching to a daemon whose first client died can never record | filed, design |

Verification of the tree after every fix is at the bottom.

---

## 1. CRITICAL — a cancel inferred while its finish event is in flight

**Where** `src/daemon/nxtaktd.cpp`, `pumpTakeCancels()`.

`pumpTakeCancels()` infers "the engine cancelled this take in silence" from three
facts. The first was written as:

> the take never STARTED — no `Ev::RecordStarted` for it, and
> `Ev::RecordFinished` is only ever emitted from a phase the start creates, **so
> there is no finish event in flight to race**

That sentence stopped being true at commit `4cbd4c9`, "cancelRec hands the buffer
back". `cancelRec()` now emits a zero-frame `Ev::RecordFinished` **carrying the
buffer pointer** (`src/audio/engine.cpp:512`). The daemon's own comment was never
updated, and the inference was never re-derived against it.

The other two facts — `drains` reaching the `+2` proof, and `recState` reading 0
— are published by the engine *after* that event is pushed. So all three can hold
with the finish still sitting unread in the engine's ring, because `pumpEvents()`
does not always reach it:

* it returns the instant the **client's** ring is full (`nxtaktd.cpp`, the
  `map_.evts->push(e)` failure path), and
* it stops at `kEvtBudget`.

The old code then did `takes_.erase(...)`, destroying the `Take` and with it
`std::unique_ptr<f32[]> buf`.

**Why that is a use-after-free and not just a stray log line.** The next take
asks the worker for `new f32[n]()` — same size, same allocator, next request —
and gets that address back. `finishTake()` matches takes to events **by buffer
pointer** (`takeByBuf`). So the stale zero-frame finish is applied to the *new*
take: announced empty (`TakeWasEmpty`, `x = 0`), dropped from `takes_`, and its
buffer freed — while the audio thread is in `recPhase 2`, appending into it every
block. A live recording is silently destroyed *and* the engine keeps writing into
freed memory. The trigger is a client that merely stopped reading its events.

**Reproduction** — `tests/daemon_test.cpp` §17j. It fills the client's 4096-slot
event ring with refusals and stops reading, puts three ordinary transport events
in front of the finish (without them the finish is at the head of the ring and
`finishTake()` consumes it before the failing push — the bug is invisible), then
arms a take and stops it before the quantized start.

Against the daemon with only fact (4) reverted:

```
FAIL  the take is NOT committed while its finish event is still in flight (1, was 0)
[nxtakt err ] a finished take names buffer 0x7f3796488010, which belongs to no take this daemon started
note  drained 4100 events; 3 forwarded, 1 orphaned
FAIL  and NO finish event named a buffer no take owned (1)
```

The second failure is the precondition of the ABA, stated from the daemon's own
side. After the fix both pass, and `takesOrphanedFinish` stays 0.

**The fix.** A fourth fact: *the event ring has been drained to empty since (2)
and (3) first held.*

It is a proof, not a delay. Let `D` be the drain that consumed the stop.
`cancelRec()` ran inside `D` and pushed the event inside `D`. `drainProven()`
becoming true means `D` has completed, so the push has already happened. Any
drain of the engine's event ring **to empty** that begins after that moment must
therefore observe the event. So the take arms on the tick facts (2) and (3) first
hold and is only erased after one full drain-to-empty afterwards. If the finish
never arrives across that, it was never emitted, and the inference is sound.

Implemented as `Daemon::evtRingDrains_`, bumped by `pumpEvents()` **only** when
it reached the end of the ring — not on a budget exhaustion and not on an early
return. A counter that also moved on a partial drain would prove nothing.

`ControlHeader::takesOrphanedFinish` (taken out of `reserved1`, so no offset
moves and no protocol bump) counts finish events naming a buffer no take owns. It
must read 0 forever, like `arrOrderViolations`.

**Note the interaction with finding 3.** Before finding 3 was fixed, `pumpEvents`
*destroyed* one stuck event per tick, so the ring drained at 1 kHz on its own and
the window was usually only a few milliseconds wide. Fixing the event loss —
correct on its own terms — makes this window unbounded. Shipping finding 3
without finding 1 would have turned a rare race into a reliable one. They belong
in the same change.

---

## 2. CRITICAL — a rack edit rewrites the layout the audio thread is reading

**Where** `src/plugin/internal_devices.cpp`, `Rack::republish()` / `Rack::process()`.

`Rack` publishes its chain as an immutable `Layout` in a slot of a four-deep
ring. The stated argument:

> The ring is four deep so that four edits would have to land inside a single
> audio block before the layout being read could be rewritten — that is a user's
> hand against a 5.3 ms block at 256 frames.

`setState()` is not a user's hand. It republishes once to unlink, once per
device, once per mapping, and once to close. Restoring a rack of two devices and
two mappings is **six republishes back to back, in microseconds**, on the pump
thread — so the ring wraps *inside a single restore*, while `process()` holds its
`const Layout*` for the whole block.

ThreadSanitizer, on the daemon under `daemon_test`:

```
WARNING: ThreadSanitizer: data race
  Write of size 8 at 0x7298000022e0 by main thread:
    #0 republish  src/plugin/internal_devices.cpp:2189   (L.dev[L.n] = d)
    #1 setStateDepth                        :2377
    #2 doSetRackState  src/daemon/nxtaktd.cpp:2902
  Previous read of size 8 at 0x7298000022e0 by thread T1:
    #0 process    src/plugin/internal_devices.cpp:1931   (L->dev[i]->setTransport(...))
    #1 lat::Engine::process(...)  src/audio/engine.cpp
```

and the same for `L.n` (line 2186) against `process()`'s `L->n` read. `dev[]` is
an array of **pointers to sub-devices**. A torn `n` indexes past what the current
generation wrote, into entries left by generations old enough that `reclaim()`
has since destroyed them — a virtual call through a freed `PluginInstance` on the
audio thread.

**The fix.** A layout is now **retired, never rewritten**. `republish()`
allocates a fresh `Layout`, publishes it with the same single release store, and
moves the displaced one onto `retired_`. `reclaim()` frees them — the same
discipline the file already applies to unlinked sub-devices, and for the same
reason: no code inside a `PluginInstance` can know when the audio thread last
dereferenced a pointer, but the caller can. Both callers already have that proof
(the daemon rides its chain-retirement drain proof at
`nxtaktd.cpp:pumpChainRetirements`; the GUI calls `reclaim()` from
`app_devices.cpp:267`).

Cost: one ~1.4 KB allocation per structural edit, on the GUI thread, beside the
`reg_->instantiate()` an edit already pays for. Growth between reclaims is a
*leak* if a caller never reclaims, named by `kLayoutWarn` in the log rather than
capped — refusing an edit would be a rack that silently does not change, and this
is the same direction the arrangement retirement already chose: a block nobody
frees costs memory, a block freed under a voice costs the process.

**Reproduction** — `tests/internal_device_test.cpp`, "Rack: a setState() may not
rewrite the layout being rendered". One thread renders, one restores two states
of *different device counts* (so a torn `n` indexes past the other state rather
than landing on a same-shaped chain). `reclaim()` is deliberately kept **out** of
the concurrent loop: calling it there would violate its own contract and bury the
real race under races the test caused.

Built with `-fsanitize=thread`:

| | races | `republish` races |
|---|---|---|
| before | 10 | `:2186` (`Layout::n`), `:2189` (`Layout::dev[]`) |
| after | 6 | none |

The six that remain in both are third-party (`lsp-plugins-lv2.so` racing itself
during the scan) and finding F2's documented plain-scalar parameter contract.

---

## 3. MAJOR — a full client ring silently destroyed engine events

**Where** `src/daemon/nxtaktd.cpp`, `pumpEvents()`.

```c
if (!map_.evts->push(e)) return;     // client asleep; retry next tick
```

There was nothing to retry with. `e` had already been **popped off the engine's
ring** and is a local; the `return` discarded it. So while a client was not
reading, the daemon ate one engine event per pump tick — `Ev::ClipStarted`,
`Ev::TransportStopped`, `Ev::RecordStarted` — with no counter moving, and
`eventsDropped` untouched.

`pumpCommands()` one screen up has had the right rule since phase 1
("dropping would lose user intent silently"), and the journal's own note states
the asymmetry plainly: *an event must not be lost; a journal entry must be KNOWN
to be lost.*

**Fix** — park it, exactly as `pumpCommands` parks a command: `pendingEvt_` /
`havePendingEvt_`, retried at the top of the next tick, in order.

**Reproduction** — the tail of §17j. Three transport events are emitted while
nothing can be forwarded; three must come out when the client reads again.
Before: `0 forwarded`. After: `3 forwarded`.

---

## 4. MAJOR — a take capacity bounded after the cast, and it wrapped

**Where** `src/daemon/nxtaktd.cpp`, `doTakeCommand()`.

```c
if (!std::isfinite(w.x) || w.x < 1.0) { ...refuse... }
const i64 cap   = (i64)w.x;                        // UB for w.x > INT64_MAX
const u64 bytes = midi ? cap * sizeof(WireNote) : (u64)cap * 8ull;
if (bytes > ipc::kMaxTakeBytes) { ...refuse... }
```

`isfinite` and `>= 1.0` admit `1e30`. `(i64)1e30` is undefined behaviour, and on
x86 it yields `INT64_MIN`:

```
$ g++ -fsanitize=undefined,float-cast-overflow ub.cpp && ./ub
ub.cpp:2:64: runtime error: 1e+30 is outside the range of representable values
             of type 'long long int'
-9223372036854775808
```

`(u64)INT64_MIN * 8` is `0x8000000000000000 * 8`, which **wraps to 0** — so
`bytes > kMaxTakeBytes` is false, the extent check passes, and the daemon accepts
a take with a negative capacity, handing `takeFloats(INT64_MIN, …)` to the
worker's `new f32[n]()`.

Worth noting why no earlier sanitizer run caught this: GCC's `-fsanitize=undefined`
does **not** include `float-cast-overflow` (unlike Clang's). Verified above — the
plain `undefined` build prints `INT64_MIN` silently. Any `f64 → integer` cast on
a peer-supplied value in this tree is therefore unguarded by the sanitizer runs
we do, and has to be closed by a bound in the code.

It is a fail-open, not merely a theoretical UB. Against the daemon with the bound
reverted, `tests/daemon_test.cpp` §17l:

```
FAIL  every impossible capacity is refused with a reason (3 of 7)
```

Only `0.5`, `0.0` and `-1.0` were caught — by the `< 1.0` test. `1e30`, `1e300`,
`9.3e18` and `kMaxTakeBytes + 1` all got through.

**Fix** — the bound comes *before* the cast, against `kMaxTakeBytes`, which is
the ceiling the extent check applies anyway and is far below the `f64 → i64`
cliff, so the cast that follows is total.

---

## 5. MAJOR — refusals crowded out announcements, and the retry did not exist

**Where** `src/daemon/nxtaktd.cpp`, `queueTakeEvent()` / `pumpTakes()`.

`takeEvts_` is bounded at 64 and was first-come-first-served. Two things ride it
with very different consequences:

* `EvTakeFailed` — a refusal. Losing one costs a log line. It is also the only
  **unbounded** producer: a client can ask for a take with a bad index as fast as
  it can push commands.
* `EvTakeReady` — the hand-back. `App` frees its capture buffer on this event and
  on nothing else, so one lost is a buffer pinned and a slot stuck in "recording"
  for the rest of the session — and, for a take with a file, one of the eight
  `kMaxPendingTakes` gone for good.

So a client that stopped draining and spammed bad take starts could fill the
queue with refusals and cost a real take its announcement.

The file claims this cannot happen:

> A take whose `EvTakeReady` will not fit the client's ring stays in `takes_`
> with its file written and `announced` false, and the announcement is retried
> every tick for as long as it takes.

`announced` was set **unconditionally** at the announce site and **read
nowhere**. The retry did not exist; the flag was write-only state.

**Fix**, two halves:

* `queueTakeEvent(e, reserve)` — a refusal may only use the queue down to a floor
  of `kMaxPendingTakes` slots. At most that many takes exist and each owes at
  most one announcement over its whole life, so an `EvTakeReady` is unrefusable
  *by construction*, which is what lets the announce paths drop their take the
  moment they queue it.
* `announceTake()` reports whether it went, `announced` records it, and
  `pumpTakes` step 3 retries every tick for any `Ready` take that is not
  announced. One `readyEvent(const Take&)` now spells the event, so the
  announcement is a function of the take rather than of which of four call sites
  reached it.

`tests/daemon_test.cpp` §17k covers it. **Honest note:** I did not get this one
to go red against the reverted daemon in the time available — filling both the
client ring *and* the 64-slot queue past the drop point turned out to need more
pressure than the section applies. The drop path is plainly there in the old code
(`queueTakeEvent` returns without queueing, `announced` is read nowhere), but §17k
currently stands as a guard, not as a demonstration. It should be strengthened.

---

## 6. MINOR — the leak check counted the whole machine

**Where** `tests/daemon_test.cpp` and `tests/ipc_test.cpp`, `countNxTaktShm()`.

Both counted *every* `/dev/shm` entry containing `nxtakt`. A developer's own
daemon, a second suite in CI, or another agent's test process on the same machine
all read as "this run leaked". It was observed failing once during this audit on
a clean tree, for exactly that reason — and a leak check that cries wolf is one
that gets ignored the day it is right.

**Fix** — scope to this run. Every region `daemon_test` creates is `gSession` or a
suffix of it; every region `ipc_test` creates carries the pid. Both now filter on
their own tag, which is necessary and sufficient: nothing of ours can escape it
and nothing of anybody else's can match it.

---

# Filed

## F1 — `engine.cpp`: two RecordSlot paths still strand a capture buffer

Commit `4cbd4c9` fixed `cancelRec` to hand the buffer back. The same bug survives
one branch over, in `Cmd::RecordSlot` (`src/audio/engine.cpp` ~2256):

```c
} else {                                   // t.recSlot != c.b
    if (!buf || cap <= 0) break;
    if (t.recPhase == 1) {
        // Nothing captured yet, so this is just a retarget.
        t.recBuf = buf; t.recCap = cap; t.recSlot = c.b; t.recMidi = midi;
        ...
```

`t.recBuf` is overwritten with **no event for the displaced buffer**. It is the
identical shape to the bug `cancelRec`'s comment now describes ("nulling them
without an event stranded each buffer in `App::pendingRecs_` until shutdown").
The same applies to the hand-over path below it: reaching it a second time while
`recPhase == 3` overwrites a non-null `t.pendBuf` without emitting.

Not reachable from the daemon today — `doTakeCommand` answers `RejectTakeBusy`
for a second take on a live track, and `App::startRecording` refuses a track whose
`recState` is not idle — so this is latent, not live. It is exactly the kind of
latent that becomes live when a caller is added.

Suggested diff (engine.cpp is another agent's file this wave):

```diff
                 if (t.recPhase == 1) {
-                    // Nothing captured yet, so this is just a retarget.
+                    // Nothing captured yet, so this is just a retarget — but the
+                    // buffer being displaced is the caller's and only an event
+                    // gives it back. Same rule as cancelRec.
+                    if (t.recBuf && t.recBuf != buf)
+                        emitCritical(this, evts_,
+                                     {t.recMidi ? Ev::MidiRecordFinished : Ev::RecordFinished,
+                                      c.a, t.recSlot, 0.0, t.recBuf});
                     t.recBuf = buf; t.recCap = cap; t.recSlot = c.b; t.recMidi = midi;
                     t.recFireBeat = nextQuantum(beat_, -1);
                 } else {
+                    if (t.pendBuf && t.pendBuf != buf && t.pendBuf != t.recBuf)
+                        emitCritical(this, evts_,
+                                     {t.pendMidi ? Ev::MidiRecordFinished : Ev::RecordFinished,
+                                      c.a, t.pendSlot, 0.0, t.pendBuf});
                     t.recPhase = 3;
```

## F2 — the plain-scalar parameter contract is the last data race

After finding 2, every remaining ThreadSanitizer report on our own code is one
class: `InternalInstance::setParam` (`internal_base.h:49`) and `setBypassed`
(`:91`) are plain non-atomic stores, read by `process()` on the audio thread
without synchronisation. Five reports in the daemon suite, two in the device
suite.

It is deliberate and documented — `host.h` calls `setParam` "GUI thread,
concurrent with `process()`", and `nxtaktd.cpp` spends thirty lines arguing the
guarantee has three parts and none of them is about which thread it is. On every
target we build for, a 4-byte aligned store cannot tear and a stale read costs
one block. It is still formally UB, and it is now the *only* thing standing
between this tree and a clean TSan run — which has real value, because a clean
run is a run where the next race is visible.

**Design options.**

1. `std::atomic<f32> pv_[]`, relaxed on both sides. Free on every target we
   build for (a relaxed 4-byte load/store compiles to the same instruction), and
   it makes the whole class go away including `bypassed_`. Changes `host.h`'s
   documented contract from "a plain scalar" to "a relaxed atomic" — which is
   what the contract has always *meant*.
2. Leave it and suppress. A TSan suppression file naming these two lines. Cheap,
   honest, and keeps the noise out of future runs — but it also suppresses the
   day one of them stops being a scalar.
3. Leave it and document the expected count. Weakest: an expected count is a
   number nobody updates.

Recommend (1). It is a small mechanical change to `internal_base.h` and it is not
a behaviour change on any target in the matrix.

## F3 — Spectra's MIDI queue drops panics

**Depth cut here on the orchestrator's instruction**; Spectra was born under the
determinism gates and is the lowest-risk target. What I did read:

`Spectra::queue()` (`src/plugin/spectra.cpp`) drops silently when `nPend_`
reaches `kPend = 128`:

```c
void queue(int frame, u8 type, u8 a, u8 b) {
    if (nPend_ >= kPend) return;
```

128 note events in one block "is not music", and dropping a note-on or a note-off
under that pressure is a defensible answer. Dropping `kEvNotesOff` /
`kEvSoundOff` is not: those are the two messages whose entire job is to fix a
stuck note, and they go through the same gate. A flood that fills the queue with
note-ons and then drops the All Notes Off behind them leaves up to 16 voices
sounding until the next panic that happens to fit.

Fix is contained but touches the realtime path, so it is filed rather than
applied under time pressure: let a panic always land, either by overwriting the
last queued slot or by setting a `pendPanic_` flag that `process()` applies at
frame 0 and which clears the queue. The second is simpler and cannot reorder.

Two smaller things noticed and not chased:

* `process()` applies pending events in **queue order**, not frame order
  (`while (ev < nPend_ && pend_[ev].frame <= n)`). If a host ever delivered
  out-of-order offsets within a block, the loop stalls at the first late event
  and then applies several at once, out of order. Every caller in this tree
  delivers in order, so this is a robustness note, not a bug.
* The shared table build is fine: `spTables()` is a function-local `static`, so
  C++11 magic-static initialisation makes two instances preparing concurrently on
  different threads safe by construction. The header's "once per process" claim
  holds. The SVF flushes denormals on both integrator states
  (`internal_dsp.h:323`), so the "denormals at extreme settings" concern does not
  apply.

## F4 — a second client can never record from an adopted daemon

`reclaimTakesIfClientGone()` keys liveness on the **pool's** creator pid, and a
pool is mapped once per daemon lifetime (`pumpPool` refuses a second epoch by
design, "restart the engine to change pools"). So once the original client dies,
`pool_.creatorAlive()` is false forever, and every take a *new* client starts is
marked `discard` within 250 ms.

In practice a second client cannot do much with such a daemon anyway — it has no
pool of its own, so every `SetClip` carrying a ref answers `RejectNoPool` — so
this is consistent with an already-unsupported shape rather than a new hole. It
is filed because "attach to a running daemon" is a natural thing to want next,
and this is one of the things that has to be answered first. The design question
is whether liveness should move off the pool (a client-written heartbeat word in
`ControlHeader`, which the protocol currently has no field for) or whether
daemon adoption should be refused explicitly rather than half-working.

---

# What was checked and found sound

Recorded so the next audit does not re-derive it.

* **Take capacity/ceiling exactness.** `takes_.size() >= kMaxPendingTakes` counts
  takes in every state, so the ceiling is exact. `len` is clamped to `cap` before
  `TakeHitCeiling` is derived from `len >= cap`.
* **`.part` rename window.** `takeWriteAtomic` writes to `path.part`, `fflush`,
  `fsync`, `fclose`, then `rename` — a reader that can see the final name is
  looking at a complete file. `tmp[768]` is wider than any path this header
  composes plus `.part`, so the compose cannot truncate.
* **`CmdTakeRelease` vs reclaim.** A release for a take that is not `Ready` is
  refused with `RejectNoTake`, and the client only ever releases after
  `EvTakeReady`, so the "release a take the worker is still writing" case is
  closed by the state gate. A written take's buffer is returned at the *write*,
  not at the release, so an unclaimed take costs a file and not a buffer.
* **Lock discipline between pump and worker.** The buffer travels *inside*
  `TakeJob` in both directions, so ownership is never shared; `takeMx_` is held
  only around the two deques and never across an allocation or a write. TSan
  agrees: zero races among the daemon's four threads.
* **Pool blob validation.** Every field of `WireArrHeader`, `WireArrItem`,
  `WireAutoSetHeader`, `WireAutoLane` and `WireAutoPoint` is bounded before any
  arithmetic uses it, the declared extent is checked against the extent
  `poolValidate` *proved* (not a re-read of the peer-writable `bytes`), and the
  blob is snapshotted before validation so there is no TOCTOU. `RtAutoLane::index`
  is the one field bounded loosely (`kMaxDevParams`) for a target that uses it as
  a send index — and the engine bounds it again at use
  (`l.index >= kMaxReturns` breaks), so the class is closed on the far side.
  Generation/ack pairing: exactly one `EvArrangementAck` / `EvSignaturesAck` /
  `EvClipAck` answers every command on every path, accepted or refused.
* **The seqlock.** Every field the daemon mirrors is inside the
  `publishBegin()` / `publishEnd()` bracket, including the `pos*` five added for
  the musician's playhead. `shutdown()`'s `engineState` store is correctly
  *outside* it — the mirror thread is still running there and a seqlock has one
  writer. `sampleRate` / `blockSize` / `enginePid` are set once in `init()` after
  the driver is up, which is why they are not in the loop.
* **The seqlock's "removal-tested" claim, spot-checked by removing two fields.**
  Not taken on trust: I deleted `posBar`'s store and `recState`'s store from
  `mirrorLoop()` in a scratch daemon and ran the suite. `posBar` fails three
  assertions (one of which names the experiment: *"delete its store from
  mirrorLoop() and this poison survives"*), `recState` fails two, across three
  separate sections — 708 passed, 5 failed. Both fields are genuinely covered,
  and the coverage is by behaviour rather than by a field list somebody has to
  remember to update.
* **`EngineState`.** No take counters were added to it; the take counters live in
  `ControlHeader`, read directly. `link` / `linkSilentMs` / `devicesPending` are
  client-derived, not mirrored, so they are not seqlock-relevant.
* **`restartEngine()` vs in-flight takes.** `restart()` clears `synth` and *then*
  calls `cancelTakes()` — the order is load-bearing and correct, since
  `cancelTakes` answers each outstanding take through `synth`. The old daemon
  removes `Preparing`/`Armed` take files at shutdown and the new one sweeps the
  directory at startup, and `stopSpawned()` waits for the old daemon to exit
  before the new one is spawned, so the sweep cannot race a live writer.

**Not covered, and worth a later pass:** `takeReady()` in `engine_handle.cpp`
matches a take by **track alone**, ignoring both the slot and the uid. It is
correct today because the daemon's one-live-take-per-track invariant and the
client's own `takes` table make a second announcement for a track impossible —
but it is the client-side half of finding 1's ABA, and it would be cheap to make
it match on `uid` instead. Filed as a follow-up rather than changed under time
pressure, because the client's `PendingTake` does not currently carry the uid.

---

# Verification

Everything below is from **clean binaries** (`build/` emptied of every test and
tool before `make -j`).

* `make -j` — zero warnings, zero errors, GUI and daemon.
* `make test` — **ALL CHECKS PASSED**.

| suite | baseline | now |
|---|---|---|
| `engine_test` | 708 | 708 |
| `ipc_test` | 134 | 134 |
| `daemon_test` | 695 | **713** (+18) |
| `internal_device_test` | 590 | **597** (+7) |
| `timesig_view_test` | 1020 | 1020 |
| `handle_test` | 183 | 183 |

* **Renders** — four demo scenes, `cmp`-identical to `scratchpad/BASELINE-bdbaebb/`.
  Nothing in this audit touched a sample.
* **ASan + UBSan** — `ipc_test` 134/134 clean; `internal_device_test` 597/597
  clean; `daemon_test` 713/713 against an ASan+UBSan `nxtaktd`, clean. No
  `runtime error`, no `AddressSanitizer` report. (These runs use GCC's
  `-fsanitize=undefined`, which excludes `float-cast-overflow` — see finding 4.
  Adding it to the suite's flags is a cheap follow-up and would have caught that
  finding on its own.)
* **TSan, the daemon's four threads** — 713/713 pass, **5 races, down from 7**.
  The two `Rack::republish` races are gone. The five that remain are all finding
  F2's documented plain-scalar contract (`setParam` ×3, `setBypassed`,
  `InternalInstance::p`). **Zero races among the pump, audio, take-worker and
  mirror threads themselves** — the take machine, the job queues and the seqlock
  mirror are clean.
* **TSan, the device suite** — 597/597 pass, 10 races → 6, the two `republish`
  races gone; the six remaining are F2 plus `lsp-plugins-lv2.so` racing itself
  during the scan.
