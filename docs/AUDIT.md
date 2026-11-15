# chost architecture audit and gap analysis

- **Date:** 2026-11-15
- **Version audited:** v0.21.0, commit `3c93654`
- **Scope:** every file in `src/core` and `src/render` read in full. The key parts of `src/gui` (`MainWindow`, `History`, `Autosave`, `Control`, `SampleEditor`, `PianoRoll`, `ArrangeView`, `ScriptJob`), the protocol and design notes of every script in `tools/`, the whole remaster pipeline, and the CI workflow. Not read line by line: all of `MainWindow.cpp` (3,938 lines) and the notation code (`Score`, `ScoreView`, `Chords`).
- **Method:** static reading only. The findings come from the code; they were not reproduced in a running build.

## The short version

chost is a strong **AI-assisted production tool** sitting on a **thin DAW core**. The AI workflow (stem separation → audio to MIDI → arrangement → tuning → balance → mastering, all controllable by Claude through the MCP server) is ahead of what Cubase, Logic and Studio One ship. Undo, autosave and crash-isolated export are also well done.

The core underneath has three structural limits that block it from being a professional DAW:

1. **There is no timeline model.** A track plays exactly one file from 0:00. "Clips" are really whole-file copies: every move, fade or cut writes a new 32-bit WAV of the entire track.
2. **The engine is Carla's patchbay driven from the GUI thread.** Every structural change disconnects and reconnects the whole graph. The loop, seeking and tempo are handled by GUI timers, not by the audio engine.
3. **Time is measured in seconds, not musical time.** There is one tempo, the bar grid is hard-coded to 4/4, and MIDI notes are stored in seconds. Tempo changes, automation and groove quantize all need a musical timebase first.

The roadmap therefore starts by fixing the foundation before adding features.

---

## 1. Architecture and code health

### Layering

```
GUI (Qt) ── MainWindow (3.9k lines, a "god object") ── Control (friend class, 1.4k lines, MCP tools)
   │
core ── Session (tracks/buses/sends → Carla plugin ids) ── HostEngine (wraps libcarla)
   │                                                        └─ OfflineEngine (chost-render)
tools/*.py ── run as subprocesses (ScriptJob), line-based stdout protocol
```

**Good choices:**

- Running the AI and export work in separate processes, so a crashing plugin or script can't take chost down.
- Plugin discovery runs each plugin in its own sandboxed process.
- Undo stores whole snapshots of the session, keyed by a stable `Uid` for each track and plugin.
- Carla callbacks are queued onto the GUI thread (`MainWindow.cpp:270`).
- The MCP socket is restricted to the current user (`Control.cpp:100`).

### Findings, most severe first

| # | Severity | Finding | Where |
|---|---|---|---|
| 1 | **High: wrong audio** | The audio cache names files by their **file name only** when the path is plain ASCII. Two different files called `vocals.wav`, or a file rewritten in place (for example by re-running separation), hit the old cache entry and **the wrong or stale audio plays**. The engine sample rate isn't part of the cache name either. | `AudioConvert.cpp` `prepareAudioFile` |
| 2 | **High: glitches** | Every seek calls `relocate()`, which **reloads the file of every player**, including MIDI players and removed tracks kept for undo. Scrubbing in the sample editor does this on every mouse move, and the loop does it on every pass. | `HostEngine.cpp` `relocate`, `SampleEditor.cpp` mouse move |
| 3 | **High: timing** | Loop playback is **polled by a 30 ms GUI timer** (`seconds >= end - 0.02`). The loop point drifts by 20–50 ms plus the buffer size, then triggers the reload in #2. That isn't usable for recording or editing in a loop. | `MainWindow.cpp` `tick` |
| 4 | **High: dropouts** | `Session::reroute()` does `disconnectAll()` and then reconnects **every** track for any change (adding an insert, adding a send, bypassing, an undo). This is likely to cause audible dropouts during playback. | `Session.cpp` `reroute` |
| 5 | **High: musical** | **The tempo never reaches Carla's transport**: there are no calls to set the BPM or bar/beat position anywhere, and offline renders send no bar/beat info. Tempo-synced delays, LFOs and arpeggiators run at Carla's default tempo, during playback and in exports. | no transport BPM calls; `OfflineEngine.cpp` `NativeTimeInfo` |
| 6 | **High: functional bug** | The **Scissors tool sends a Move edit** with `seconds = clickTime`, which shifts the whole clip later instead of splitting it. | `SampleEditor.cpp` `mousePressEvent` |
| 7 | **High: pro readiness** | There is **no plugin delay compensation**. Carla's patchbay doesn't align parallel paths, so a linear-phase EQ or lookahead limiter on one track puts it out of time with the others. | architecture |
| 8 | Medium: audio quality | The resampler is **linear interpolation with no filtering**, so 96k→48k aliases and 44.1k→48k dulls the top end. Every imported file that isn't at the engine rate goes through it. | `AudioConvert.cpp` `resample` |
| 9 | Medium: GUI freezes | Waveform peaks (`computePeaks` decodes the **whole file** into memory), clip edits (full read, process, write) and autosave (Carla project save) all run **on the GUI thread**. | `MainWindow::clip`, `MainWindow::editClip`, `Autosave` |
| 10 | Medium: memory | Removed plugins are **kept loaded, only switched off, for 100 undo steps**. Deleting a Kontakt instance holding gigabytes of samples doesn't free its RAM. The `fClips` waveform cache is never cleared either, and every edit adds a new file to it. | `Session::park`, `History` |
| 11 | Medium: fragile export | `chost-render` waits a fixed **500 ms** for Carla to apply its connections. On a slow machine or a big session, tracks can be missing from the export without any error. | `OfflineEngine.cpp` `stopPump` |
| 12 | Medium: crash | The remaster pipeline **crashes on mono stems**: `processed[:, 1]` on an array that has one channel. It also mixes stems at different sample rates without resampling (`sr = file_sr` keeps whichever rate came last). | `chost_workflow_remaster.py` `mix_and_master` |
| 13 | Medium: hygiene | The remaster script hard-codes a developer path, `D:\AI_Models\...`, and **quietly sets an HTTP proxy for every subprocess** if something is listening on 127.0.0.1:7890/10808/10809. The final master is written straight to the Desktop. | `chost_workflow_remaster.py` top of file |
| 14 | Medium: smoothing | Faders, mute and pan set Carla's per-plugin volume in one step, and mute switches between 1 and 0 instantly. Whether Carla ramps these changes was not confirmed; it's a likely source of zipper noise and clicks that should be tested. | `Session::applyGains` |
| 15 | Low | Plugin parameter changes **aren't in undo**: only adding, removing and bypassing inserts are. The piano roll keeps its own separate undo stack. | `SessionState` |
| 16 | Low | The `.chost` save is **not atomic** (the Carla project is written first, then the session file) and depends on Carla's plugin numbering. Names containing a newline would corrupt the file. | `Session::save` |
| 17 | Low | `setEventCallback(nullptr)` is called while Carla's threads may still be calling the callback (a race on shutdown). | `MainWindow::~MainWindow` |
| 18 | Low: maintainability | `MainWindow` (3.9k lines) and `Control` (a friend class reaching into its internals) mix view, model and commands. The UI text is hard-coded in Chinese with no translation system, so there's no English UI. | `src/gui` |
| 20 | **High: crash** (found while testing Phase 0) | **Building a large project quickly crashes Carla 2.5's graph.** Pre-existing: the original code crashes 5 of 8 times making a 40-track project with inserts and sends through `chost-cli`, and 9 of 15 times doing the same in the offline engine. Carla asserts (`channelNumber < numChannels`, `dataToReferTo[i] != nullptr`) and segfaults. Every `addTrack`/`addInsert`/`setSend` calls `reroute()` (#4), which tears down and rebuilds all connections while audio is processing; 40 tracks means about 160 full rewires. Loading a saved project (one rewire) is reliable. Stem separation and auto-arrangement add many tracks in a row, so they are exposed. Likely interim mitigation (untested): batch structural changes into one `reroute()`. The real fix is Phase 1's own graph. | `Session::reroute` |
| 21 | Medium: crash under load (found while testing Phase 0) | Under extreme CPU oversubscription (72 busy processes on 24 cores), `chost-render` crashes in about 1 run in 5, mostly while loading the project, in the original and the new code alike (21/124 vs 14/60, not a significant difference). It is the same Carla race as #20. Mitigation: the GUI could retry an export whose renderer crashed, since rendering runs in its own process. | `chost-render` |
| 22 | Low (found while testing Phase 0) | A project's file paths are stored as given and resolved against the current directory, not the project's folder. A project made with `chost-cli --track a.wav` renders silent (−120 dB) when `chost-render` runs from another directory. The GUI stores full paths, so it mostly affects CLI-made projects and moved project folders. Fix: store paths relative to the `.chost` file and resolve them against it. | `Session::save`, `Session::load` |
| 19 | Low: tests | CI has good end-to-end smoke tests (undo, autosave, MCP, separation self-test), but **no unit tests** for the core maths (clip edits, loudness, the MIDI parser, resampling) and no sanitizer builds. | `.github/workflows/build.yml` |

---

## 2. Feature completeness

### Well implemented

- AI stem separation into 4, 6 or 14 stems, with groups and roles assigned automatically
- Audio to MIDI with Basic Pitch, including drum-part note mapping
- Note-based pitch correction (pYIN + TD-PSOLA, Melodyne-style, keeps vibrato) with an A/B switch
- Loudness measured to the BS.1770 standard, AI mix balance and auto-pan, and AI mastering with true-peak limiting
- Chord pad with next-chord suggestions, chord detection, and style-based auto-arrangement
- Sheet music: PDF and MusicXML, triplets, compound meters, drum notation
- Groups (folders), sends and effect buses, with safeguards against feedback loops; solo behaves the way it does in Cubase
- Snapshot undo with a history list, autosave with crash recovery, kept backup versions
- Crash-isolated offline export with TPDF dither; separate stem export
- An MCP server so Claude can drive the whole application

### Half-finished or rough

| Area | Current state |
|---|---|
| **Waveform rendering** | Only the peak height of the left and right channels combined, drawn symmetrically. The resolution is fixed (40k buckets in the arrangement, 8k in the editor), so it turns blocky when zoomed in. There's no per-channel, minimum/maximum or RMS display, no peak files on disk, and everything is computed on the GUI thread. |
| **Clip editing** | Every edit writes a whole new file, so the disk fills with full-length 32-bit copies. Nothing can be adjusted afterwards: no fade handles, no trimming. Scissors is broken (#6). |
| **Undo** | Covers the project structure only, not plugin parameters. The piano roll's undo is separate from the main one. |
| **Loop / cycle** | Driven by the GUI timer (#3). |
| **Quantize** | Hard grid only, no strength, swing or groove. Notes are stored in seconds, so changing the tempo knocks MIDI out of the grid. |
| **Metronome** | Works by pre-rendering a MIDI file of clicks; no count-in and no time signature beyond 4/4. |
| **Time signature** | 4/4 is hard-coded in the arrangement, the sample editor and the MCP tools. The notation code supports other meters, but the timeline doesn't. |
| **Remaster pipeline** | Mixes in Python, outside the engine, so it ignores inserts and the user's mix. Stems are summed with fixed gains (and #12). |
| **Bypassed inserts** | Implemented by rewiring the graph (#4) rather than a click-free bypass. |

### Missing professional features (by importance)

1. **A clip/region timeline**: several clips per track, each with an offset and trims, non-destructive fades and gain, and crossfades
2. **Automation**: envelopes for volume, pan, sends and any plugin parameter, with read/write/touch/latch modes
3. **Recording**: audio and MIDI input, input monitoring, punch in/out, takes and comping, latency compensation for recording
4. **Musical timebase**: a tempo track, time-signature track, bars/beats/ticks, and warping audio to tempo
5. **Plugin delay compensation**
6. **Sample-accurate loop and transport** driven by the engine, with a count-in
7. **Editing tools**: split, glue, slip, ripple, snap to events, a nudge grid
8. **Markers and arranger**: markers already exist; still missing are an arranger/section track (rearrange the song by sections), cycle markers and a marker list
9. **Groove quantize**, swing, quantize strength, and extracting a groove from audio
10. **A chord track** that MIDI and arrangement parts follow (the chord pad has the data but there's no timeline lane)
11. **Mixer depth**: pre/post-fader sends, sidechain routing, VCA faders, a phase invert/trim/HPF channel strip, a proper constant-power pan law
12. **Metering**: LUFS, true peak, correlation and a spectrum analyser on the master
13. **MIDI**: a controller-lane editor (CC, pitch bend, velocity), a step editor, a drum editor, MIDI effects
14. **Audio**: time-stretching (élastique/Rubber Band), varispeed, a high-quality resampler, a project sample-rate setting
15. **Export**: MP3/FLAC/AAC, export a range, normalise to loudness, real-time export for hardware
16. **Platform**: macOS/CoreAudio and WASAPI (Carla 2.5's Windows build lacks it), CLAP plugins, an English UI

---

## 3. Prioritized roadmap

### Phase 0: stop the bleeding (1–2 weeks) — done 2026-11-15, except #13's proxy/Desktop items

New findings #20 and #21 (Carla graph crashes) move the Phase 1 graph rewrite up in priority.


1. **Cache key (#1):** name cache files by a hash of the full path, modification time, size and engine rate. Keep a readable name prefix for the Carla rack display.
2. **Scissors (#6):** make it actually split (until the Phase 1 timeline exists: into two tracks).
3. **Seek (#2):** reload only audio players, only when needed, and never while scrubbing; use a lightweight preview instead.
4. **Remaster script (#12, #13):** handle mono and mixed sample rates; remove the `D:\` path; make the proxy opt-in; let the user choose the export folder.
5. **Render (#11):** replace the 500 ms sleep with a check that waits until every expected connection is present.
6. **Tempo to the transport (#5):** pass the session BPM and time signature to Carla, and fill in the bar/beat info in `OfflineEngine`.
7. Add **unit tests** (Catch2) for `ClipEdit`, `Loudness`, `MidiFile` and the resampler, plus an AddressSanitizer/UBSan build in CI.

### Phase 1: foundation rewrite (2–3 months), the critical path

Everything later depends on this phase.

1. **Own the audio graph.** Keep Carla for *hosting plugins*, but have chost process the mix itself: one audio callback, a mixer graph compiled ahead of time and swapped in without locks (no more `reroute()` dropouts), with plugin delay compensation from the plugins' reported latencies.
2. **Timeline model:** `Track → Clip[] { source, sourceOffset, start, length, fadeIn/Out curves, gain }`. Playback reads from disk in a streaming thread into lock-free ring buffers (replacing Carla's file player). All edits become non-destructive. Existing projects migrate as one clip per track.
3. **Musical timebase:** positions stored in ticks, a tempo map and a signature map, conversion between ticks and seconds, MIDI kept in ticks. The 4/4 assumption goes everywhere.
4. **Engine-side transport:** sample-accurate loop and punch points, count-in, and a metronome generated by the engine itself.
5. **Peak files:** multi-resolution minimum/maximum per channel, built in the background and cached on disk next to the media. Drawing reads only what is visible.
6. **Command layer:** pull a `ProjectModel` and a set of `Commands` out of `MainWindow`. Undo, the GUI and MCP all go through it, and plugin-parameter changes become undoable as merged steps. Unload removed plugins after N steps or above a memory limit, keeping saved states so they can still come back.
7. **Smoothing:** ramp every gain, pan and mute change over 5–20 ms inside the engine, and switch bypass by crossfading.
8. **Project format v2:** a JSON or XML document with atomic save (write to a temporary file, then rename) and plugin state stored by `Uid`, not by Carla's numbering.

### Phase 2: professional parity (3–4 months)

1. **Automation:** a lane for any parameter, read/write/touch/latch modes, applied sample-accurately in the engine; automation follows clips when they move.
2. **Recording:** audio and MIDI input, monitoring, punch, loop recording into takes, a comping lane.
3. **Editing:** split, glue, trim handles, crossfades (adjustable shape, automatic crossfade on overlap), slip and ripple, snap to grid and events, nudge.
4. **Arranger and chord track:** promote the existing markers into an arranger track; turn the chord pad's progression into a chord track lane that MIDI parts and the auto-arranger follow live.
5. **Groove quantize:** strength, swing, groove templates, and extracting a groove from audio (onsets from the existing transcription code).
6. **MIDI editing:** CC, velocity and pitch-bend lanes; a drum editor (the drum-name mapping already exists); a step editor.
7. **Mixer:** pre/post-fader sends, sidechain, VCAs, channel strip, metering (LUFS, true peak, correlation, spectrum), constant-power pan law.
8. **Audio quality:** a high-quality resampler (libsamplerate or r8brain), Rubber Band for time-stretch and warp, a project sample-rate setting.
9. **Export:** FLAC, MP3 and Opus; export a range; loudness-normalised export.

### Phase 3: commercial readiness (parallel and ongoing)

- Platforms: macOS/CoreAudio and WASAPI (needs Carla 2.6+ or chost's own device layer), CLAP plugins
- Translations (Qt Linguist): English UI, needed for a global market
- **Licensing:** libcarla is GPL-2.0+, which rules out closed-source distribution. To sell chost proprietary, the Phase 1 graph needs a hosting layer that can be licensed (chost's own code, JUCE commercial, or a dual-licensed framework). **Decide this before Phase 1, because it decides what Phase 1 is built on.**
- Crash reporting, telemetry with consent, a signed installer and auto-update, a plugin blacklist and a sandbox for flaky plugins.

### Phase 4: AI features traditional DAWs lack

These build on code chost already has; most only become possible once Phase 1 and 2 are in place.

1. **Live stem tracks:** separated stems stay linked to the source as one "stem container" clip. Moving or cutting the container moves all its stems; a slider remixes the stems inside one clip without exploding it into 14 tracks.
2. **Re-separate a region:** select a bad bar and re-run separation on just that range with a better model.
3. **Note-level tuning in the timeline:** the pitch curves and notes `chost_tune.py` already produces, shown as editable blobs in the sample editor. Drag a note, straighten its drift, adjust formants. Rendering happens in the background and can be undone like any edit.
4. **Audio↔MIDI link:** transcribed MIDI stays bound to its audio. Correcting a note in the piano roll re-tunes the vocal; quantizing the MIDI time-stretches the audio.
5. **Groove and chords from a reference:** drop in a reference song and extract its tempo map, groove template, chord track and sections, then apply them to the project.
6. **AI arranger on the chord track:** regenerate one part for one section ("busier drums in chorus 2"), and generate variations to choose between.
7. **Mix assistant using automation:** turn the loudness-based balance into automation suggestions (ride the vocal, duck the bass under the kick) instead of static fader settings, accepted or rejected per lane. Add a frequency-masking detector between stems.
8. **Claude as a session musician through MCP:** the Phase 1 command layer makes every action available to Claude and undoable as one step. Add tools that let it *listen* to a range (loudness and spectrum summaries) so it can carry out instructions like "make the chorus hit harder" and check the result.
9. **Smart comping:** score each take for pitch accuracy, timing against the grid, and noise, and propose a comp.
10. **Fully local and private:** keep every model on-device. It's a real selling point; add GPU and DirectML acceleration and lazy model downloads.

---

### Suggested order of work

| Order | Item |
|---|---|
| Now | Phase 0, items 1–6 |
| Next | Decide the licensing question, then build the Phase 1 engine, timeline and timebase together, since they depend on each other |
| Then | Peak files, the command layer and smoothing |
| After that | Phase 2 automation and recording, then editing and the chord/arranger tracks |
| Ongoing | Turn the AI features into timeline-native tools (Phase 4, items 1, 3 and 4 first, because they reuse existing code) |

---

## Change log

| Date | Item | Status |
|---|---|---|
| 2026-11-15 | Audit written | done |
| 2026-11-15 | #1 audio cache key: full path + modification time + size + engine rate; atomic write (`.part` file renamed into place) | done, tested |
| 2026-11-15 | #2 seek reloads: only active audio file players are refreshed; repeated seeks (scrubbing) throttled to one refresh per 100 ms plus a final one | done, tested |
| 2026-11-15 | #6 Scissors: cuts the clip in two (track keeps the part before the cut, a new track mixing identically gets the part after); one undo step | done, tested |
| 2026-11-15 | #5 tempo to the transport: the session tempo goes to carla's transport live (`carla_transport_bpm`) and offline (`NativeTimeInfo` with bar, beat, tick and time signature for every block) | done, tested; see limits below |
| 2026-11-15 | #12 remaster mixdown: every stem loaded as stereo (mono spread, extra channels dropped) at one sample rate (polyphase resampling) | done, tested |
| 2026-11-15 | #13 (part) developer paths: `D:\AI_Models\audio_separator` removed from the remaster script, `RemasterDialog` and `StemDialog`; the remaster script takes `--model-dir` and chost passes the folder stem separation already uses; "RTX 3090" removed from UI text | done, tested |
| — | #13 (rest): proxy port sniffing and the default export to the Desktop in `chost_workflow_remaster.py` | open |
| 2026-11-15 | #11 render settling: the fixed 500 ms wait is replaced by watching Carla apply the connections (a probe wired last, see `HostEngine::settleOffline`), then 100 ms of silence so Carla's gain ramps finish. The old wait also **faded in the first ~50 ms of every export**; that is fixed too | done, tested |
| 2026-11-15 | Phase 0 item 7: the Phase 0 checks run in CI (`CHOST_BUILD_TESTS`, step "Phase 0 regression tests"), each one confirmed to fail with its bug put back | done |
| — | #20 and #21 (Carla graph crashes while building projects or under extreme load) | open; Phase 1, interim mitigations suggested above |

### Phase 0 test results (2026-11-15)

Run on Linux with Carla 2.5.8 and Qt 6.4.2. The live tests use a headless JACK server (dummy backend, 48 kHz, 256 frames).

- **Cache (#1):** regression test of 12 checks. HEAD fails 5 of them: a second song's `vocals.wav` played the first song's audio, a re-separated stem played the stale copy, and a new engine rate reused the old file. The fix passes all 12.
- **Seek (#2):** on a live engine with 2 active audio tracks, 1 deleted track kept for undo and 1 MIDI track, file reloads went from 4 to 2 for one seek, and from 240 to 22 for 60 seeks in one second (scrubbing). Sound after a backward seek is kept: 0 silent runs in 28 for each of HEAD and the fix.
- **Scissors (#6):**
  - `splitAudioFile`: the two parts add back to the original (largest error 3e-8), each part is silent on the other side of the cut, and the crossfade adds no jump.
  - `Session::duplicateTrack`: copies inserts as new plugins, keeping their settings and bypass state, along with fader, pan, colour, role, output and sends. Undo restores the exact earlier state; a failed copy leaves no plugins behind.
  - Null test through `chost-render` (project before vs after the cut, with a gain-0.37 insert, a bypassed gain-0 insert and a send): largest difference 3e-8 (−139 dB).
- **Regression:** CI's multitrack suite (15 checks, run with `--driver JACK` instead of PulseAudio) and the GUI `--test-undo` self-test pass.

- **Tempo (#5):** Carla's built-in tempo-synced LFO (sawtooth, 1 beat) was measured at session tempos of 90, 120 and 150 BPM, timed by the transport.
  - Old code: the plugin hears about 120 BPM whatever the session says, both live and in `chost-render`.
  - New code: it hears 90.0 / 120.1 / 150.0 BPM live and 90.0 / 119.9 / 150.0 offline.
  - `barBeatAt` passes 9 unit checks (4/4, 3/4, 6/8, beat and bar boundaries, no drift after 10 hours).
  - Switching carla to its internal transport mode turned out not to be needed for this and was left out, so chost still follows JACK transport like before.
- **Tempo limits:**
  - Live, Carla 2.5's transport takes only the tempo; its bars are always 4/4.
  - The session has no time signature yet (4/4 is hard-coded in the GUI), so 4/4 is what gets passed. `HostEngine::setTempo(bpm, beatsPerBar, beatType)` already carries the time signature through to offline rendering, ready for the Phase 1 timebase.
- **Remaster (#12):**
  - HEAD crashes on a mono stem (`IndexError`).
  - The fix mixes a mono 44.1 kHz vocal, a stereo 48 kHz drum stem and a 3-channel 48 kHz bass through the real mastering step; all three tones are present in the master and nothing smears between them (−106 dB).
  - The model folder is taken in this order: `--model-dir`, then `AUDIO_SEPARATOR_MODEL_DIR`, then `chost_separate.py`'s default (6 checks).

- **Render settling (#11):** in a 40-track project (each track its own tone, two inserts each, sends to a bus), a wait of 0–20 ms rendered silence or a damaged start, while 50 ms or more was enough. All connections were already confirmed by Carla *before* the wait, so they were not what it waited for: Carla applies graph changes on a timer of its own, which the probe now watches.
  - Typical waits: 25–80 ms; under heavy load 60–320 ms, taken only when needed.
  - Every render is bit-identical, and matches the ideal mix from the first sample to the last (error 3e-5, float rounding; with the overall gain fitted, 2e-7).
  - The old renderer was 0.74 off in the first 5 ms (a 50 ms fade-in). Under load its output also varied from run to run (0.04–0.55); the new one's does not.
  - A version that kept processing during the wait crashed more under load. The one shipped stops processing while Carla rebuilds, as the old wait did, and checks one block every 15 ms.
- **CI:** five checks were confirmed to fail with their bugs put back: cache key, seek reloads, tempo (live and offline), settle, insert cloning, and the old remaster script. Four simulated runs of the CI step on a fresh Release build passed.

**Not covered:**
- The Scissors mouse click in the GUI was not exercised by a test; only the code it calls was.
- Carla 2.5's file player has a rare start-up race: a freshly loaded player can stay silent at the first `play()` when no `relocate()` comes before it. It occurs in both the original and the fixed builds (seen in about 1 run in 40 of each) and the fixes don't touch that path. Phase 1's own disk streaming removes it.
- The remaster pipeline's separation, tuning and transcription steps need the AI models and weren't run; the mixdown and mastering steps were.
