# REDGraph

A render host for **Cyberpunk 2077 2.31**: a RED4ext plugin that opens the game's renderer to other plugins. A client
plugin can:

* replace an engine shader with its own bytecode and switch between versions live, optionally only inside one
  render-graph node;
* feed its own data into that shader through the engine's own descriptor tables, with no root-signature changes;
* add ray-tracing raygen variants next to the engine's and switch them live, per node;
* wrap, remove, replace or add nodes of the engine's render graph and record D3D12 work on the engine's command lists,
  with the engine's own resources (by name), transients and descriptors.

Several clients can run at once. REDGraph arbitrates between them, and its CET window shows who registered what. It
needs no ReShade and touches no game files.

## For modders

The SDK - a guide, the header, Python tools that extract and patch the game's shaders, and a reference of every
render-graph node with its shaders - is in [`sdk/`](sdk/README.md) and in every release as `REDGraph-SDK-<version>.zip`.
A client includes one plain-C header, [`include/rg_host.h`](include/rg_host.h). With CMake:

```cmake
add_subdirectory(REDGraph)            # or FetchContent; only the header is used
target_link_libraries(my_mod PRIVATE REDGraph::api)
```

## Building

Windows, Visual Studio 2022 or later with the C++ tools, CMake 3.20+.

```
cmake -B build -A x64
cmake --build build --config Release
```

CMake fetches [RED4ext.SDK](https://github.com/WopsS/RED4ext.SDK) at a pinned commit; `-DRG_RED4EXT_SDK_DIR=<checkout>`
uses yours. If the clone fails with "Filename too long", run `git config --global core.longpaths true` or use a
shorter build folder.

The output is `REDGraph.dll`, `REDGraphSelfTest.dll` (the host's tests) and `REDGraphExampleClient.dll` (the SDK's
example). Two self-tests need bytecode of the game, which this repository does not contain: give CMake the installed
game and a [DirectX Shader Compiler release](https://github.com/microsoft/DirectXShaderCompiler/releases) and it builds
them from the game's own shader caches -

```
cmake -B build -A x64 -DRG_GAME_DIR="C:/.../Cyberpunk 2077" -DRG_DXC_DIR="C:/.../dxc/bin/x64"
```

(or set `RG_GAME_DIR` / `RG_DXC_DIR` in the environment). Without them those two tests are left out.

`scripts/build_and_deploy.ps1 -Target <game or mod-manager mod folder> [-SelfTest]` builds and installs.

## How it works

[`docs/internals.md`](docs/internals.md): the engine hooks, every mechanism, the render graph's structure as far as it
is decoded, and what is still missing. Contributions: [`CONTRIBUTING.md`](CONTRIBUTING.md).

## License

[MIT](LICENSE). Cyberpunk 2077 and REDengine are trademarks of CD PROJEKT S.A.; REDGraph is not affiliated with or
endorsed by CD PROJEKT RED.
