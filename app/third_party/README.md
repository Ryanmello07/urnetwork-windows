# Vendored dependencies

These are fetched by `tools/fetch-deps.ps1`, not committed (except this README).
The `.gitignore` excludes the binary payloads.

## `urnetwork-sdk/{amd64,arm64}/`

The C ABI + C++ wrapper produced by `sdk/cgo`. Each arch dir holds:

- `URnetworkSdk.dll` — the SDK (embeds the Go runtime, wintun is separate)
- `URnetworkSdk.lib` — import library, generated from `urnetwork_sdk.def` via
  `lib /def:urnetwork_sdk.def /machine:{x64|arm64} /out:URnetworkSdk.lib`
- `urnetwork_sdk.h` — the raw C ABI
- `urnetwork_sdk.hpp` — the header-only C++17 wrapper (the API the app uses)

Built on the macOS build server: `make -C ../../sdk/cgo build_windows` produces
`sdk/cgo/build/URnetworkSdkWindows.zip` with `windows/{amd64,arm64}/` inside.
`fetch-deps.ps1` unzips it here and generates the import libs.

## `wintun/`

The upstream-signed Wintun DLLs (wintun.net). Pinned by SHA256 and Authenticode
signer thumbprint, exactly as Mullvad does (plan §2). Layout after fetch:

- `wintun.h`
- `bin/amd64/wintun.dll`, `bin/arm64/wintun.dll`

We redistribute these unmodified per the Wintun Prebuilt Binaries License; we do
not build or sign the driver ourselves.

## `qrcodegen/`

Project Nayuki's QR Code generator library (C++) v1.8.0, COMMITTED (not
fetched): two dependency-free files, and pinning the bytes is what makes the
extender share code reproducible across build machines. See
`qrcodegen/README.md` for the exact provenance, hashes and re-vendor command.
It is also compiled into `tools/extender-tests.cpp`, which is what proves on a
non-Windows host that the vendored copy still builds.

## `zxing-cpp/`

The QR DECODER for the extender import sheet (a chosen image file; there is no
camera path on windows). Fetched by `tools/fetch-deps.ps1` from a pinned
upstream release and built headless into a static lib, because it is far larger
than the encoder and only the windows build needs it. Layout after fetch:

- `zxing-cpp/include/ZXing/*.h`
- `zxing-cpp/lib/{x64,ARM64}/ZXing.lib`
- `zxing-cpp/zxing-cpp-license.txt`

## nlohmann-json, wil

Come from vcpkg (manifest mode, see `../vcpkg.json`) — not vendored here.
