# Changelog

## Unreleased

- GPU timing (Host tab, off by default): the GPU time of every mod companion and pass, and each one marked as a
  debug event for GPU capture tools.
- Mods can compile HLSL through REDGraph while the game starts (needs the shader compiler DLLs next to REDGraph).
- Higher limits for mods that patch many shaders: 256 patches, 32 leases, and a lease now reaches up to 64 different
  game shaders (it stopped at 8 without a word; now the log says when it is full).
- The render graph window shows the nodes a mod removed.

## 0.2.1

- A GPU crash is now written to the log (the reason and, with `Dred = true`, where on the GPU) before the game
  closes - before, the game closed first and the log said nothing.

## 0.2.0

- Leases can bind textures: your own, or one of the game's by name.
- A lease whose shader declares the register as a different kind of resource than the one you bind is refused, with
  the reason in the log.
- A mod's call made from inside one of its callbacks is no longer mistaken for another module's (and refused).
- Several mods can patch the same draw at once when they patch different shader stages (vertex / pixel / geometry);
  each one switches on and off on its own.
- A patch can also change how the game draws with a shader (culling, wireframe, depth test, blending, write masks,
  target formats), with or without replacing the shader itself.
- The log lists which registers of the game's tables a lease can use, per shader stage.
- A wrap of a game node can declare a texture it needs (`node_wrap_use`), so the game keeps it alive through that node.
- Mods can react to the game's draws, not only its compute dispatches: run code right after a draw of a given shader,
  draw the same thing again with another shader or pipeline state, or record their own work after it.

## 0.1.1

- Ray-tracing shader variants cost less CPU: about 2 us less per ray-tracing dispatch that runs one.

## 0.1.0

First release as a standalone project (it started inside Cyberpunk Render Tweaks, its first client).

- Engine shader patches with live variants, scoped to render-graph nodes.
- Client data in the engine's own descriptor tables (leases).
- RT raygen variants, switched live per node; whole RT libraries swapped without making the game's other RT pipelines
  wait (`rtlib_stats` says where a library went).
- The render graph: engine resources by name, nodes of your own, wrapping, removing and replacing engine nodes,
  descriptors from the engine's ring.
- Several mods at once: a handle works only in the mod that made it; mods share a texture only by its published name.
- Patched shaders stay correct when the game streams materials out and back in.
- CET window: render graph debug view, clients, host CPU profile.
- SDK: the guide, shader tools, node reference, an example client.
