# How It Works

This document describes the internal architecture of `lmmsaudiocapture`, its threading model, memory management, audio capture backends, real-time DSP playback, and GUI rendering.

---

## 1. High-Level Architecture & Threading Model

`lmmsaudiocapture` separates work across three threads so that recording, UI interaction, and real-time audio synthesis never block one another:

1. **Host Audio Thread** (`LMMSAudioCaptureVestige.cpp` -> `CapturePluginEngine::process`):
   - Real-time safe: zero heap allocations, zero blocking mutex locks (`try_lock` only), wait-free MIDI consumption, and off-thread buffer deallocation.
2. **Capture Worker Thread** (`AudioCaptureEngine::runCaptureThread`):
   - Runs at `THREAD_PRIORITY_HIGHEST` independently of LMMS's audio engine.
   - Pulls audio packets from Windows (`WASAPI IN`, `WASAPI LOOPBACK`, or `MME WaveIn`), routes channels, updates atomic peak meters and a throttled live preview, and commits the finished buffer to `AudioClip`.
3. **Windows UI Thread** (`CaptureUI.hpp`):
   - Hosts the Direct2D / DirectWrite child window (`640 x 360`), handles user input, updates `AudioClip`, and drives a dirty-checked 30 Hz `WM_TIMER` that skips repaints when idle.

---

## 2. Independent Windows Audio Capture (`src/capture/AudioCaptureEngine.hpp`)

- **Device Enumeration (`enumerateDevices`):**
  - `WASAPI_INPUT`: enumerates active `eCapture` endpoints via `IMMDeviceEnumerator`.
  - `WASAPI_LOOPBACK`: enumerates active `eRender` endpoints via `IMMDeviceEnumerator`.
  - `MME_WAVEIN`: enumerates devices via `waveInGetNumDevs()` and `waveInGetDevCapsW()`.
- **Zero-Drop `ChunkedStereoBuffer`:**
  - Live samples are appended into fixed `65,536`-frame stereo blocks instead of a single contiguous `std::vector`, avoiding vector-doubling `memcpy` stalls during long or unlimited (`INF`) recordings.
  - When recording stops, `flatten()` copies the chunks into contiguous `L`/`R` vectors once.
- **Backends & Latency:**
  - **WASAPI Input:** opens in Shared mode (`AUDCLNT_SHAREMODE_SHARED`) with `AUDCLNT_STREAMFLAGS_EVENTCALLBACK` and the device's default period (falling back to polling if a driver rejects event callbacks). Supports 32-bit IEEE Float and 16/24/32-bit PCM mix formats.
  - **WASAPI Loopback:** opens the target render endpoint with `AUDCLNT_STREAMFLAGS_LOOPBACK` and a 5 ms poll loop.
  - **MME WaveIn:** opens at 48 kHz 16-bit stereo with 4 queued `480`-frame (`10 ms`) `WAVEHDR` buffers and event wakeup.
- **Channel Modes & Duration Caps:**
  - Applies `STEREO`, `MONO_MIX`, `MONO_L`, or `MONO_R` per frame as audio arrives.
  - Enforces configurable caps (`1M`, `3M`, `5M` default, `10M`, or `INF` unlimited) and updates atomic peak meters + a 50 ms-throttled, O(1)-strided 256-bucket live waveform preview.

---

## 3. Immutable Audio Data Model (`src/dsp/AudioClip.hpp`)

- **Shared Immutable `AudioBuffer`:**
  - `ClipSnapshot` holds a `std::shared_ptr<const AudioBuffer>` containing `L`, `R`, and a precomputed `peaks` mipmap (`PEAK_BLOCK = 256` samples per block).
  - Non-destructive trim edits (`setTrim`) create a new `ClipSnapshot` that shares the existing `AudioBuffer` pointer with **zero bytes copied**.
  - Destructive edits (`cropToRange`, `deleteRange`, `silenceRange`, `fadeIn`, `fadeOut`, `normalize`, `reverse`, `removeDC`) build a new `AudioBuffer`, recompute its 256-sample peak mipmap, and atomically bump `versionAtomic`.
- **Memory-Bounded Undo/Redo:**
  - Stores up to `20` snapshots or `256 MB` of audio history (always retaining at least 1 undo step), evicting the oldest entries when exceeded.
- **Off-Audio-Thread Deallocation:**
  - When `CapturePluginEngine::process()` picks up a newer `ClipSnapshot` via `tryGetSnapshot()`, it moves the old `rtSnap` into an 8-slot `retiredPool` (under `retiredMtx.try_lock()`). The UI timer calls `reclaimRetiredSnapshots()` to free old buffers off the real-time thread.

---

## 4. Real-Time Playback & MIDI Engine (`src/dsp/CapturePluginEngine.hpp`)

- **Wait-Free MIDI Queue:**
  - `queueMidiEvent()` pushes incoming MIDI events into a 256-entry single-producer/single-consumer ring buffer (`midiWrite` / `midiRead` atomics).
  - `process()` drains and sorts events by `deltaFrames` for sample-accurate triggering.
- **Note & Mode Logic:**
  - **A4 (MIDI 69):**
    - Note On sets `velocityGain = velocity / 127.0f` and starts playback with a `1.5 ms` anti-click attack ramp.
    - In `RESTART` mode, `playhead` resets to `trimStart` on every A4 Note On.
    - In `CONTINUE` mode, `playhead` resumes from its current position; Note Off in `GATE` mode triggers a `3.0 ms` release ramp and pauses while preserving `playhead`.
    - In `1-SHOT` mode, Note Off is ignored and the clip plays through `trimEnd`.
  - **A#4 (MIDI 70):**
    - Note On resets `playhead` back to `trimStart` (with a micro-ramp if currently playing).
  - All other notes are ignored; CC 120 / CC 123 stop playback cleanly.
- **Resampling & Gain:**
  - When `clipSampleRate == hostSampleRate` and `playhead` is integer-aligned, `process()` reads `L`/`R` samples directly (1:1 fast path).
  - Otherwise, it uses 4-point Catmull-Rom cubic Hermite interpolation (`sampleCubic`) advancing at `clipSampleRate / hostSampleRate`.
  - Linear output gain is cached on `PARAM_GAIN` updates so `process()` never calls `std::pow`.

---

## 5. Direct2D / DirectWrite UI (`src/gui/CaptureUI.hpp`)

- **0% Idle CPU:** `WM_TIMER` (33 ms) only calls `InvalidateRect` while recording, playing, transitioning out of recording/playback, or when the clip version changes.
- **Transient-Accurate Cached Waveform:**
  - Computes a 608-column min/max cache (`waveCache`) only when the clip version, zoom (`viewLen`), or pan (`viewStart`) changes.
  - When zoomed out (`>= 512` samples per pixel column), it reads from `AudioBuffer::peaks` so single-sample transients are never skipped; when zoomed in, it scans exact samples.

---

## 6. State Persistence & VeSTige Wrapper

- **WAV I/O (`src/dsp/WavIO.hpp`):**
  - Parses RIFF/WAVE chunks (`fmt `, `data`, skipping metadata chunks) for 16/24/32-bit PCM and 32-bit IEEE Float (mono, stereo, or multi-channel), and exports 16-bit PCM stereo WAVs.
- **Project Chunk (`serializeChunk` / `deserializeChunk`):**
  - Saves/restores parameters, capture settings, sample rate, trim points, and 16-bit PCM stereo audio (capped at `24 MB` raw PCM per chunk) with magic header `0x43414D4C` (`"LMAC"`).
- **VeSTige Interface (`src/vestige/LMMSAudioCaptureVestige.cpp`):**
  - Uses the open-source GPL `include/vestige/vestige.h` header, exporting `VSTPluginMain` and `main` as a 2-output synth (`effFlagsIsSynth | effFlagsHasEditor | effFlagsCanReplacing | effFlagsProgramChunks`).
