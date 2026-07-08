# IME Windows Frontend

Windows TSF frontend DLL for Llavon IME. This repository owns the in-process TSF DLL and talks to the backend service over the named pipe `\\.\pipe\llavon-ime`.

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

The DLL keeps TSF UI/composition behavior local and sends prediction/input-mode requests to `llavon-ime-service.exe` through the named pipe protocol implemented in `src/engine/impl/pipeEngine.hpp`.
