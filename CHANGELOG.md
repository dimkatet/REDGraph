# Changelog

## Unreleased

- A whole RT library swapped in no longer makes the game's other RT pipelines wait for it: faster first start with a
  cold shader cache.
- A mod can no longer use another mod's handles by mistake; mods share a texture only by its published name.
- `rtlib_stats`: where an RT library went.
- Fixed: after the game streamed materials out and back in, a patched shader could stay unpatched or be drawn with
  the wrong pipeline state.

## 0.1.0

First release as a standalone project (it started inside Cyberpunk Render Tweaks, its first client).

- Engine shader patches with live variants, scoped to render-graph nodes.
- Client data in the engine's own descriptor tables (leases).
- RT raygen variants, switched live per node.
- The render graph: engine resources by name, nodes of your own, wrapping, removing and replacing engine nodes,
  descriptors from the engine's ring.
- CET window: render graph debug view, clients, host CPU profile.
- SDK: the guide, shader tools, node reference, an example client.
