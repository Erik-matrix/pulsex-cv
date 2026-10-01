# Third-party notices

The PulseX code is MIT (see LICENSE). It uses the following, each under its own license.

## Included in this repository

- **Dear ImGui** (`third_party/imgui/`) — MIT License, Omar Cornut and contributors. The license text is in
  `third_party/imgui/LICENSE.txt`.
- **nlohmann/json** (`third_party/json/nlohmann/json.hpp`) — MIT License, Niels Lohmann. The license is stated in
  the header of the file.

## Not included

- **Qualcomm QAIRT / Genie** — the built-in "local" engine is compiled against the Genie headers of Qualcomm's
  QAIRT SDK and loads `Genie.dll` at run time from a model bundle. Neither the headers nor the runtime are in this
  repository; both are under Qualcomm's own license.
- **Models** — no model is included. The example model, a Q4_0 quantisation of AI Sweden's Llama 3 8B, is under the
  Meta Llama 3 Community License.
- **llama.cpp** (`llama-server.exe`, used for a model file on the PC) — MIT License. Not included.
- **poppler / pdftotext** — GPL. Used only if it is already on your PATH, as a separate program, to read PDFs you
  add under *Merits*. Not included.
- **Claude Code** — used through its command line if you have it installed and logged in. Not included.

No warranty. With a model on the PC nothing the user enters leaves it; a cloud model, when the user picks one,
receives the merits and the ad.
