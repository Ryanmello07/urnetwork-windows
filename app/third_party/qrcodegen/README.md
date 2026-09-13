# qrcodegen (Nayuki) - vendored

The share screen renders its code with Project Nayuki's QR Code generator
library (C++), vendored here rather than fetched: it is two small files with no
dependencies beyond the standard library, it is the encoder half only (this app
never decodes with it), and pinning the bytes is what makes the rendered code
reproducible across build machines.

- upstream: https://github.com/nayuki/QR-Code-generator
  https://www.nayuki.io/page/qr-code-generator-library
- version:  v1.8.0
- files:    `cpp/qrcodegen.hpp`, `cpp/qrcodegen.cpp`, unmodified
- sha256:   qrcodegen.hpp  b779c3b156cf7a57ce789d6fee4fc991ccc2913774d26c909d22bb8f26b2a793
            qrcodegen.cpp  1f3b3fcdac6954c32cf583ccd02ec9b5901f756a38c461acedc70be4a77d3757
- license:  MIT (the notice heads both files, and is reproduced in
            ../../THIRD-PARTY-NOTICES.txt)

To re-vendor:

    v=v1.8.0
    for f in qrcodegen.hpp qrcodegen.cpp; do
      curl -L -o "$f" \
        "https://raw.githubusercontent.com/nayuki/QR-Code-generator/$v/cpp/$f"
    done

Used by `src/App/ExtenderSheets.cpp` (EXTENDER.md K7) and compiled into the
host test tool `tools/extender-tests.cpp`, which is what verifies on a
non-Windows machine that the vendored copy still builds and encodes.

DECODING is not this library's job and it cannot do it: the import sheet reads
a QR out of an image file with zxing-cpp, which `tools/fetch-deps.ps1` fetches
(it is far larger, and only the Windows build needs it).
