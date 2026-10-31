# Contributing to Utawave

**English** | [日本語](CONTRIBUTING.ja.md)

Pull requests, bug reports, and suggestions are welcome. As this is a personal project, however, review
and integration may take some time, and a merge cannot always be guaranteed — thank you for your
understanding. Opening a pull request is taken as agreement to the terms below.

## License (important / inbound = outbound)

- Contributions to this project are provided under the same license as the application,
  **AGPL-3.0-or-later** (inbound = outbound).
- Opening a pull request means you agree to license your changes under AGPL-3.0-or-later.
- Utawave links **JUCE** (dual-licensed AGPLv3 / commercial). This project uses the AGPL option and
  accepts contributions under AGPL.

## DCO (Developer Certificate of Origin)

Please **sign off** every commit. This certifies your agreement to the
[Developer Certificate of Origin](https://developercertificate.org/) and that you have the right to
contribute the change.

```sh
git commit -s -m "Fix: ..."
```

Adding `-s` appends the following line to the commit message:

```
Signed-off-by: Your Name <your.email@example.com>
```

## Development environment

See [README.md](README.md) for build instructions (CMake 3.22+ / C++17 / JUCE 9 fetched automatically by
CMake). Run the unit tests after making changes:

```sh
cmake --build build --target UtawaveTests --config Debug
./build/UtawaveTests_artefacts/Debug/UtawaveTests
```

## Coding guidelines (excerpt)

- C++17. Use JUCE classes and types (`juce::String` / `juce::File`, etc.).
- **Never allocate memory, take locks, or perform I/O on the audio thread.**
- **Wrap end-user-visible text in `tr(u8"...")`** for localization and add the key to the English
  translation table (the key must match exactly, including trailing spaces and newlines).
- **Keep third-party product / library names out of end-user-visible strings**
  (license / attribution files excepted).
