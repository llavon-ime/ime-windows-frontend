# IME Windows Frontend

Windows TSF frontend DLL for Llavon IME. This repository owns the in-process TSF DLL and talks to the backend service over dedicated named pipes.

The backend service is built separately from `C:\code_prog\llavon\ime-service`; do not add it as a CMake subdirectory here.

## Build

This repository owns only `vcpkg.json`. It does not vendor vcpkg and should not point at a local absolute vcpkg path. Pass the vcpkg toolchain from the build environment, CI, or a local user preset:

```powershell
cmake --preset windows -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build --preset windows
cmake --install build/windows --config Release
```

The install output is written to:

```text
dist/bin/llavon-ime.dll
```

## Runtime Boundary

The DLL keeps TSF composition, candidate state, keyboard behavior, and
`ITfCandidateListUIElementBehavior` local. Prediction/input-mode requests use
`\\.\pipe\llavon-ime`; render-ready candidate snapshots use the independent
`\\.\pipe\llavon-ime-candidate-ui`. The candidate HWND and XAML island are
owned by `llavon-ime-candidate-ui.dll` in the service process.

## Automatic feedback

When a user replaces the IME prediction with a different candidate, a successful
composition commit appends one local sample to
`%LOCALAPPDATA%\Llavon IME\feedback.jsonl`. Each line uses validation-set schema
version 1:

```json
{"schemaVersion":1,"license":"CC-BY-4.0","context":"我今天想吃","answer":"早餐","padding":[{"syllable":"ㄗㄠ","tone":3},{"syllable":"ㄘㄢ","tone":1}],"difficulty":1}
```

`context` is the text before the pending composition; `answer` and `padding`
contain the finalized pending composition. Automatic samples use difficulty 1.
Feedback is discarded after backspace or insertion before the composition tail.
Pending text that cannot provide one valid Zhuyin syllable per answer character
is not logged.
