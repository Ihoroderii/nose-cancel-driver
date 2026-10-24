# Noise Cancel Driver Starter

This repository is a no-JUCE starter for a voice-enhancement project.

What is included:

- `cleanmic_core`: shared C++ processing code
- `cleanmic_cli`: offline WAV-to-WAV demo
- three denoising backends for comparison:
  - a built-in DSP pipeline
  - Hush integrated as a runtime-loaded standalone backend
  - RNNoise integrated from vendored upstream source
- a macOS shared-memory virtual microphone feed
- a buildable macOS `CleanMic.driver` AudioServerPlugIn bundle
- a macOS host process that captures the physical mic, runs Hush, and publishes `CleanMic`

What is not included yet:

- signed macOS installer / launch agent packaging
- Windows virtual audio driver backend
- Linux PipeWire virtual source backend
- DeepFilterNet integration

## Architecture

The project is split so the shared DSP and the OS integration stay separate:

```text
physical mic / wav file
        |
        v
  cleanmic_core
  - high-pass filter
  - simple VAD
  - STFT denoiser
        |
        v
processed PCM frames
        |
        +--> file output / tests
        +--> macOS shared-memory feed
                |
                v
          CleanMic.driver
                |
                v
             Teams / Zoom / Discord
```

That structure is the practical path for multi-platform development:

- keep the audio engine in portable C++
- add platform adapters later
- keep driver code out of the core library

## Build

```bash
cmake -S . -B build
cmake --build build
```

## Run

```bash
./build/cleanmic_cli --input noisy.wav --output clean.wav
./build/cleanmic_cli --input noisy.wav --output clean.wav --mode hush
./build/cleanmic_cli --input noisy.wav --output clean.wav --mode rnnoise
./build/cleanmic_cli --input noisy.wav --output clean.wav --mode both
./build/cleanmic_cli --input noisy.wav --output clean.wav --mode all
```

`cleanmic_cli` now defaults to `hush` when `--mode` is omitted.

## Real-Time Mic

On macOS, a separate live app is built:

```bash
./build/cleanmic_realtime
./build/cleanmic_realtime --mode hush
./build/cleanmic_realtime --mode rnnoise
./build/cleanmic_realtime --mode bypass
./build/cleanmic_realtime --save-prefix session
./build/cleanmic_realtime --monitor
```

What it does:

- captures microphone audio in real time
- runs the selected backend live
- saves the captured raw mic signal and the processed signal so you can compare them later
- optionally plays the processed signal back to your speakers when `--monitor` is enabled

Default behavior:

- live mode defaults to `hush`
- speaker monitoring is off by default to avoid delayed self-echo
- files are saved as `capture_raw.wav` and `capture_hush.wav`
- press `Ctrl+C` to stop and flush the files

Notes:

- use `--monitor` only when you actually want live speaker playback
- use headphones with `--monitor` to avoid feedback
- allow Terminal microphone access in System Settings if macOS blocks startup
- `rnnoise` live mode currently requires a 48 kHz device
- `dsp` remains available in the offline CLI for comparison

## CleanMic Virtual Mic (macOS)

This repo now builds the macOS virtual-mic pieces:

- `./build/cleanmic_virtual_host`
- `./build/CleanMic.driver`

The host process:

- captures the physical microphone
- runs `hush`, `rnnoise`, or `bypass`
- publishes processed mono PCM into a shared ring buffer named `CleanMic`

The driver bundle:

- exposes an input-only Core Audio device named `CleanMic`
- reads PCM from the shared ring buffer
- presents that stream to apps like Teams as a normal microphone

Build artifacts:

```bash
cmake -S . -B build
cmake --build build
find build/CleanMic.driver -maxdepth 3 -type f
```

Install the driver on macOS:

```bash
chmod +x scripts/install_cleanmic_driver_macos.sh
./scripts/install_cleanmic_driver_macos.sh
```

Remove it:

```bash
chmod +x scripts/uninstall_cleanmic_driver_macos.sh
./scripts/uninstall_cleanmic_driver_macos.sh
```

Run the host:

```bash
./build/cleanmic_virtual_host
./build/cleanmic_virtual_host --mode hush
./build/cleanmic_virtual_host --mode rnnoise
./build/cleanmic_virtual_host --save-prefix cleanmic_session
```

Teams flow:

1. Install `CleanMic.driver`.
2. Start `./build/cleanmic_virtual_host`.
3. Open Microsoft Teams.
4. Go to `Settings -> Devices -> Microphone`.
5. Select `CleanMic`.

Current macOS virtual-mic notes:

- the virtual device is fixed at `48 kHz`, mono, float32
- the host process must be running or the virtual mic outputs silence
- the driver bundle is not code signed yet, so this is a developer build path
- if `CleanMic` does not appear, restart Teams and `coreaudiod`

The current CLI:

- reads a WAV file
- converts multichannel input to mono
- runs the DSP pipeline, RNNoise, Hush, or comparison modes
- writes cleaned mono WAV files

When `--mode both` is used, the CLI writes:

- `*_dsp.wav`
- `*_rnnoise.wav`

When `--mode all` is used, the CLI writes:

- `*_dsp.wav`
- `*_hush.wav`
- `*_rnnoise.wav`

## Current DSP Pipeline

The built-in DSP pipeline is intentionally lightweight and dependency-free:

1. High-pass filter to remove low-frequency rumble
2. Voice activity detector based on frame energy
3. STFT noise estimator + Wiener-style suppression
4. Peak normalization before writing output

RNNoise backend notes:

- integrated from vendored official upstream source
- uses the default pretrained model from the official 0.2 release
- currently expects 48 kHz input audio

Hush backend notes:

- integrated via the vendor-provided standalone shared library
- uses the included ONNX bundle from the vendored Hush deployment tree
- accepts arbitrary input sample rates and resamples internally

## Next Steps

For a real clean-mic product, the usual order is:

1. Add code signing and a proper macOS installer / launch agent
2. Harden the Core Audio plug-in property surface against more host queries
3. Add echo cancellation and device selection to the host process
4. Port the virtual-mic layer to Windows and Linux

Suggested backend split:

- macOS: host app + Core Audio AudioServerPlugIn virtual device
- Windows: host app + virtual audio driver / WASAPI integration
- Linux: PipeWire virtual source
