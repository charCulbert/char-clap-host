# Third-party software

clap-host is built with, and its binaries include, the following. The git submodules under `external/` and the vendored `external/wclap-bridge` carry their own licence files; keep their notices with anything you distribute.

- CLAP SDK (`external/clap`, a git submodule): MIT. https://github.com/free-audio/clap
- clap-helpers (`external/clap-helpers`, a git submodule): MIT. https://github.com/free-audio/clap-helpers
- CHOC (`external/choc`, a git submodule; windows and webviews off macOS): ISC. https://github.com/Tracktion/choc
- WebView2Loader, which CHOC embeds in Windows builds: BSD-style, © Microsoft Corporation (the full notice is near the end of `external/choc/choc/gui/choc_WebView.h`). https://www.nuget.org/packages/Microsoft.Web.WebView2
- RtAudio (`external/rtaudio`, a git submodule): MIT-style. https://github.com/thestk/rtaudio
- RtMidi (`external/rtmidi`, a git submodule): MIT-style. https://github.com/thestk/rtmidi
- compost (`external/compost`, a git submodule; the device selector): MIT. https://github.com/charCulbert/compost
- wclap-bridge (`external/wclap-bridge`, vendored with changes listed in its `VENDORED.md`): BSL-1.0. https://github.com/WebCLAP/wclap-bridge
- wclap-cpp (`external/wclap-bridge/modules/wclap-cpp`): MIT. https://github.com/WebCLAP/wclap-cpp
- Wasmtime C API v39.0.1 (downloaded when configuring with WCLAP support, and linked in): Apache-2.0 WITH LLVM-exception. https://github.com/bytecodealliance/wasmtime

Not included as code, but followed in design:

- clap-validator: MIT. https://github.com/free-audio/clap-validator. The validation suite uses its test IDs, mirrors its `NoteGenerator` and `ParamFuzzer`, and uses the same xoshiro128++ generator and default seed (though it fills the generator's state differently, so the numbers differ). Copyright © 2022 Robbert van der Helm.
