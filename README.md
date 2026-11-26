# myaudiocaptureforlmms

a Windows x64 VeSTige instrument plugin for LMMS that captures audio independently from LMMS, lets you edit the waveform in a clutter-free Direct2D/DirectWrite interface, and plays the audio when MIDI note A4 is triggered.

## features

- **independent Windows audio capture:**
  - `WASAPI IN` (event-driven shared mode for microphones, line-in, and external sound cards)
  - `LOOPBACK` (WASAPI desktop and system audio capture)
  - `WAVEIN` (MME WaveIn fallback)
  - selectable input devices, channel routing (`ST`, `MO`, `L`, `R`), and dynamic recording caps (`1M`, `3M`, `5M` default, `10M`, `INF`)
- **waveform editor:**
  - draggable start and end trim handles, region selection, zoom/pan, and drag-and-drop WAV loading
  - non-destructive trim plus destructive edits: `TRIM`, `DEL`, `SIL`, `FADE IN`, `FADE OUT`, `NORM`, `REV`, `DC`, `UNDO`/`REDO`, `CLR`, `LOAD`, `SAVE`
- **MIDI playback & persistence:**
  - A4 (MIDI 69) plays the clip and respects MIDI velocity
  - A#4 (MIDI 70) resets the playback cursor to trim start
  - `RESTART` / `CONTINUE` modes, `GATE` / `1-SHOT` modes, `LOOP` toggle, and `-24 dB` to `+24 dB` output gain
  - automatic project state saving via VeSTige chunks
- **lightweight native C++17 build:**
  - static Windows binary with SSE4.2, LTO, zero-copy trim, wait-free MIDI queue, and ~0% idle GUI CPU

## documentation

- user guide: [HOWTOUSE.md](HOWTOUSE.md)
- technical architecture: [HOWITWORKS.md](HOWITWORKS.md)

## building

run the PowerShell build script (requires `g++` / MinGW-w64):

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

outputs:
- `dist/myaudiocaptureforlmms.dll` (64-bit VeSTige plugin for LMMS)
- `bin/myaudiocaptureforlmms_standalone.exe` (standalone Windows app)

## license

licensed under GPL-3.0 — see [LICENSE](LICENSE).
