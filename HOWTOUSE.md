# How to Use lmmsaudiocapture

`lmmsaudiocapture` lets you record audio from any Windows input or desktop audio stream independently from LMMS, trim and edit the clip, and trigger it in your track with **A4**.

---

## 1. Loading the Plugin in LMMS

1. Build or copy `lmmsaudiocapture.dll` into your LMMS VST plugins folder (for example, `Documents\lmms\plugins\vst\lmmsaudiocapture.dll`).
2. In LMMS, drag **VeSTige** into the Song Editor or Beat+Bassline Editor.
3. Click the folder icon in VeSTige and select `lmmsaudiocapture.dll`.
4. You can also run `bin\lmmsaudiocapture_standalone.exe` outside LMMS to record, edit, and audition clips directly.

---

## 2. Capturing Audio (Top Bar)

From left to right across the top bar:

1. **`REC` / `STOP`:** Starts or stops recording. While recording, the timer shows elapsed time and the stereo meter displays live input levels.
2. **Backend Button (`WASAPI IN` / `LOOPBACK` / `WAVEIN`):** Click to cycle capture modes:
   - **`WASAPI IN`:** Microphones, line-in, headsets, USB/PCIe audio interfaces, or virtual cables.
   - **`LOOPBACK`:** Records whatever audio is currently playing through your speakers, headphones, or DAC (browsers, media players, other apps).
   - **`WAVEIN`:** Legacy Windows MME input fallback.
3. **Device Selector (`< Default >`):**
   - Click `<` or `>` to step through available devices for the selected backend.
   - Click the device name in the middle to refresh the device list (useful if you plug in a USB mic or sound card after opening the plugin).
4. **Channel Mode (`CH: ST` / `CH: MO` / `CH: L` / `R`):**
   - **`CH: ST`:** Stereo recording.
   - **`CH: MO`:** Mixes Left + Right into centered mono.
   - **`CH: L` / `CH: R`:** Records only Input 1 (Left) or Input 2 (Right) centered to both ears. *Tip: use `CH: L` or `CH: R` if you plug a single mic or guitar into a 2-channel audio interface.*
5. **Duration Cap (`CAP: 1M` / `3M` / `5M` / `10M` / `INF`):**
   - Sets the maximum recording length (default `5M`). Recording stops automatically when the cap is reached, or select `CAP: INF` for unlimited length.

---

## 3. Editing the Waveform (Center Area & Toolbar)

### Waveform Interactions
- **Trim Handles:** Drag the bright vertical handles at the start and end of the waveform to set the playback region (shaded areas outside the handles are skipped during playback).
- **Select a Region:** Click and drag inside the waveform to highlight a range. Double-click to select the entire clip; single-click to clear the selection.
- **Zoom & Pan:**
  - Scroll the mouse wheel over the waveform to zoom in/out around your cursor.
  - Hold `Shift` + mouse wheel, drag with the middle mouse button, or drag the bottom scrollbar to pan.
  - Use `-`, `+`, and `FIT` at the bottom-left of the waveform card for quick zoom control.
- **Drag & Drop:** Drop any `.wav` file onto the plugin window to load it immediately.

### Edit Toolbar
Most edits apply to your **highlighted selection** if one is active, or to the **trimmed region** if nothing is selected:

- **`TRIM`:** Permanently crops the audio to your selection (or to the trim handles if nothing is selected).
- **`DEL`:** Deletes the highlighted selection with a tiny splice crossfade.
- **`SIL`:** Silences the selection (or trimmed region) with smooth edges.
- **`FADE IN` / `FADE OUT`:** Applies a smooth fade-in or fade-out (if nothing is selected, fades the start/end of the trimmed region).
- **`NORM`:** Normalizes peak level to `-0.5 dB`.
- **`REV`:** Reverses the audio.
- **`DC`:** Removes DC offset.
- **`UNDO` / `REDO`:** Steps backward or forward through edits (up to 20 steps / 256 MB).
- **`CLR`:** Clears the current audio buffer.
- **`LOAD` / `SAVE`:** Opens a `.wav` file or exports the trimmed clip as a 16-bit stereo `.wav`.

**Keyboard Shortcuts** (when the plugin window is focused):
- `Ctrl+Z`: Undo
- `Ctrl+Y`: Redo
- `Delete`: Delete selection
- `Space`: Play / Stop audition

---

## 4. Playing with MIDI in LMMS (Bottom Bar)

- **Trigger Note (`A4`):** Play **A4** in LMMS's Piano Roll or on a MIDI keyboard to trigger the clip. Note velocity controls playback volume. Other notes are ignored so you can keep **A4** as your trigger note without pitch-shifting the recording.
- **Reset Note (`A#4`):** Play **A#4** at any time to jump the playback cursor back to the start trim handle.
- **`PLAY` / `STOP` & `RST`:** Audition the clip directly from the UI and reset the cursor without sending MIDI.
- **`RESTART` / `CONTINUE`:**
  - **`RESTART`:** Starts from the beginning (start trim handle) every time **A4** is pressed.
  - **`CONTINUE`:** Resumes from where playback last paused. Combine with **A#4** to reset the loop point whenever you want.
- **`GATE` / `1-SHOT`:**
  - **`GATE`:** Plays while **A4** is held down and stops on Note Off.
  - **`1-SHOT`:** Plays the full trimmed clip on **A4** Note On and ignores Note Off.
- **`LOOP`:** Loops continuously between the start and end trim handles.
- **`GAIN` Slider:** Adjusts output volume from `-24.0 dB` to `+24.0 dB` (double-click the slider to reset to `0.0 dB`).

### Saving Your Work
- When you save your LMMS project, the captured/edited audio and all plugin settings are automatically saved inside the project.

---

## 5. Tips for Low Latency & Sound Cards

- For lowest MIDI-to-audio latency in LMMS, go to **Edit → Settings → Audio Settings**, use **WASAPI** or **ASIO**, and lower the buffer size (e.g., `128`–`256` samples).
- If LMMS is using an audio interface's **native ASIO** driver, that driver may lock the card from Windows `WASAPI IN` at the same time—switch LMMS to WASAPI/SDL while recording from the same card, or record from a different input.
