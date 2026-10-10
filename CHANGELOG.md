# Changelog

## Unreleased

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
