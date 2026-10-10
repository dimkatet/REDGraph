# Contributing

Start with [`docs/internals.md`](docs/internals.md) - every mechanism, hook and engine structure the host uses.

## Ground rules

* **No game content in the repository.** No game binaries, no shader bytecode or disassembly taken from the game
  (edited or not), no game shader sources. What needs game bytecode builds it from the user's own install at build
  time (see `scripts/make_selftest_blobs.py`). Facts about the game - addresses, struct offsets, node names, shader
  hashes - are fine.
* **The game version is 2.31.** Addresses are hard-coded RVAs in `src/core/engine_231.hpp`, each hook guarded by a
  call-site check. No signature scanning.
* **The API is append-only.** `include/rg_host.h` is an ABI: never reorder, remove or change a field or a function
  pointer; append, and document the new entry where it is declared. Bump `RG_HOST_API_VERSION` only for what an older
  client cannot ignore.
* **The core knows no client.** Nothing in `src/core/` may depend on a particular mod's shaders or settings.
* **Hooks attach on demand** - only when some registration needs them - and every failure falls back to the engine's
  own behaviour, with a log line saying why.
* **Proof on hardware.** A change to the engine side is done when the game ran with it and the log (or the screen)
  shows it working. Say in the PR what you ran - render mode (raster / RT / PT), what you looked at, the log lines -
  and which self-test covers it. Add a self-test (`selftest/`) for a new mechanism when it can be observed from a
  client.

## Building and testing

See the README's "Building". `scripts/build_and_deploy.ps1 -Target <game or mod folder> -SelfTest` installs the host
and the self-test client; `REDGraphSelfTest.ini` next to it picks the tests (`docs/internals.md` "Self-tests").
The game must be closed while deploying.

CI builds every push without the game (the two game-blob tests compiled out), packages the mod and the SDK, and
builds the SDK's example on its own. A `v*` tag publishes a release with both zips - take release zips from CI: a local
Windows PowerShell 5.1 writes backslashes into zip paths.

## Tools

| script | what |
|---|---|
| `scripts/exe_re.py` | disassemble the game's exe with names from cp2077-symbols (find / check an address) |
| `scripts/make_selftest_blobs.py` | the self-tests' game-derived blobs (run by CMake) |
| `scripts/sdk_node_reference.py` | `sdk/reference/` from census runs (`[Diagnostics] RenderGraphDump`) |
| `scripts/compile_sky_paint.ps1` | regenerates `selftest/sky_paint_shader.hpp` from its HLSL |
| `scripts/package_sdk.ps1`, `scripts/package_mod.ps1` | the release zips |
| `sdk/tools/*.py` | the modders' tools (shipped in the SDK) |

## Style

C++20, MSVC `/W4` clean. Follow the surrounding code: comments say what the engine does and why the code does what it
does, with the address when it is about the engine.
