# Utawave

**English** | [日本語](README.ja.md)

A free, **recording-focused DAW** for vocal-cover artists, distributed at no cost for macOS and Windows.

Copyright (C) 2025-2026 Utawave
Licensed under the **GNU Affero General Public License v3.0 or later (AGPL-3.0-or-later)**.

- Source code: https://github.com/AsteroidApp-hub/Utawave
- Full license text: [LICENSE](LICENSE)
- Third-party components: [THIRD_PARTY_LICENSES.txt](THIRD_PARTY_LICENSES.txt)

---

## Features

- Multitrack recording / playback (simultaneous multi-track recording, punch-in, retrospective recording, loop recording)
- Waveform editing (move / resize / split / consolidate / fade / crossfade / strip silence)
- MIDI tracks, piano roll, built-in synth
- Mixer / metering / simple reverb / plugin (VST3) hosting
- WAV / AIFF / MP3 export (built-in encoder, high-quality dither)
- Japanese / English UI

See the bundled manual ([Docs/MANUAL.html](Docs/MANUAL.html)) and the specification ([Docs/SPEC.md](Docs/SPEC.md)) for details.

---

## Build

### Requirements

- CMake 3.22 or later
- A C++17 compiler (macOS: Xcode / Windows: MSVC)
- JUCE 8 (fetched automatically by CMake, or placed in a local `JUCE/`)

### Steps

```sh
git clone https://github.com/AsteroidApp-hub/Utawave.git
cd Utawave
cmake -S . -B build-mac -DCMAKE_BUILD_TYPE=Release
cmake --build build-mac --config Release
```

The first configure **automatically downloads JUCE 8 (8.0.12)** via CMake FetchContent; JUCE is not
vendored in the repository. To build offline or pin a specific version, place JUCE 8 in `JUCE/` at the
repository root and the local copy is used instead of downloading.

### ASIO support on Windows (optional)

The ASIO SDK is not included because its redistribution is restricted. To use it, obtain the SDK from
[Steinberg Developer](https://www.steinberg.net/developers/) and place it in `Source/ThirdParty/asiosdk/`
(CMake enables it automatically when present). Without it, the app runs with WASAPI / DirectSound only.

---

## Tests

```sh
cmake --build build-mac --target UtawaveTests --config Debug
./build-mac/UtawaveTests_artefacts/Debug/UtawaveTests
```

A console app based on `juce::UnitTest` (exit code 0 when all pass).

---

## License

The application itself is distributed under **AGPL-3.0-or-later**. Under the AGPL, anyone who receives
the binary must be able to obtain the corresponding source code, and this repository is that
corresponding source. See [THIRD_PARTY_LICENSES.txt](THIRD_PARTY_LICENSES.txt) for the licenses of the
open-source libraries and SDKs used.

## Disclaimer

This software is free and provided "AS IS". While every effort is made to ensure it works well, it comes
with no guarantee that it will fit any particular environment or purpose. The author cannot accept
liability for damages (including loss or corruption of recordings or project data, or damage to other
software or equipment) arising from the use of, or inability to use, this software. Please back up
important data. See Sections 15 and 16 of the AGPL-3.0-or-later for details.

## Support

Your support helps keep development going 🙇 Donations go toward code-signing certificate
costs and ongoing development.

- **Donate (Stripe)**: https://donate.stripe.com/fZubJ16rg5bW9Dw9uc38405

## Download

Prebuilt apps (macOS / Windows) are available from
**[Releases](https://github.com/AsteroidApp-hub/Utawave/releases)** (the same builds are also distributed
on the official site <https://utawave.com/download.html>). See [RELEASE.md](RELEASE.md) for the release
procedure.

## Contributing

Pull requests, bug reports, and suggestions are welcome. As this is a personal project, review and
integration may take time, and a merge cannot always be guaranteed. Before sending changes, please
review the terms in [CONTRIBUTING.md](CONTRIBUTING.md) (AGPL-3.0-or-later / DCO sign-off).
