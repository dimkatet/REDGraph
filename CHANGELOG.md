# Changelog

## 0.4.1

- The CET windows use Window Utils when it is installed (right-click on a switch resets it); without it they look as before.
- Fixed: `[Diagnostics] FrameTimer` logged nothing unless `RenderGraphDump` was also on.
- Fixed: a crash when an RT raygen variant was refused because the variant table was full - its library replaced the
  game's own. Any refused variant now takes its library with it; the table holds 128 variants (was 64).

## 0.4.0

- New API call `engine_block_find(name)`: the engine constant blocks are now a catalog keyed by the engine's name of the
  function that writes them (`CRenderInterface::CalculateSharedConstants` = cb12, ...); a block's writer is hooked only
  when a client asks for it. The four `rg_engine_block` values keep working as before (ids 0-3, always captured).
- New API call `census_watch(stage, hash, name)` and a new diagnostic `[Diagnostics] BlasCensus`: is a mesh in the
  ray-traced BVH (static / updatable / absent), and what the engine's BVH pool writers did - every 10 s in the log.
  The client names the shaders to watch; with the census off the call does nothing.
- New diagnostic `[Diagnostics] FrameTimer` (live): the frame time every 10 s in the log, for an on / off comparison
  of any feature. Both new diagnostics are in the REDGraph window.
- Subscription limits raised: 32 dispatch and 32 draw subscriptions (were 8 / 16).
- Existing API calls unchanged; API version stays 1 (the two calls are appended to the table).

## 0.3.4

- Fixed ray-traced client features (e.g. Cyberpunk Render Tweaks' grass shadows and reflection fog) switching off for
  the rest of the session after a long play: a table of 64 render-context entries leaked one entry per context the
  engine retired, and once full every new context's leases were LOST (`LOST (64 contexts)` in the log). An entry
  unused for 600 frames is now taken over.
- Log: the peak of context entries in use, each take-over (the first 16, then every 2^n), and the totals on the
  `RT lease delivery this session` line.

## 0.3.3

- Fixed a rare GPU crash (page fault at VA 0, `DEVICE_HUNG`) inside a client's raygen variant with ray tracing or
  path tracing on - seen with Cyberpunk Render Tweaks' grass shadows, about once in 30-45 min of play. At a rare
  `DispatchRays` an RT lease did not reach the descriptor table the dispatch used, and the variant read a descriptor
  that was not ours.
- RT leases are now checked at every `DispatchRays`: what each one got, and whether ours is still in the slot after
  the engine's `UpdateRootParams` copy (catches a slot overwritten by the engine, and a STATIC table of any kind - before,
  only an SRV's static table was caught). A variant is handed out only if every RT lease of its library landed;
  otherwise that one dispatch runs the engine's raygen.
- Log: `rt variant: ... the ENGINE's raygen ran this dispatch instead` (the first 16, then every 2^n) and
  `rt variant: lease delivery checked at N hand-outs` (at the first, then every 2^n from 1024). With `Dred = true`
  every DispatchRays marker carries its lease state (`leases: b3=new#12 b4=ours#20 ...`), and the crash log prints the
  session's totals and where each RT lease lives in our heap.

## 0.3.2

- With `Dred = true`, ray-tracing structure builds are tracked too: a GPU crash while the game builds them names
  which one, and a build given a missing buffer is written to the log when it happens, crash or not.
- `node_remove_clear` now works on a node that is a member of a group (e.g.
  `CRenderNode_ScreenSpacePlanarReflections`): name the texture it writes, and it is cleared in the node's place.
  Before, nothing was cleared and the log said nothing.

## 0.3.1

- With `Dred = true`, a GPU crash in a ray-traced pass now names the ray dispatch the GPU was in, and whether a
  mod's shader was running there.

## 0.3.0

- GPU timing (Host tab, off by default): the GPU time of every mod companion and pass (each one marked as a debug
  event for GPU capture tools), of the ray dispatches mods change, and - when on at the game's start - of every pass of
  the game's render graph: switch a mod's change off and on to see what it costs.
- Mods can compile HLSL through REDGraph while the game starts. The shader compiler is not included: it works
  only with `dxcompiler.dll` and `dxil.dll` put next to REDGraph.dll.
- Thanks to Spuddeh, whose work both of the above grew out of.
- Higher limits for mods that patch many shaders: 256 patches, 32 leases, and a lease now reaches up to 64 different
  game shaders (it stopped at 8 without a word; now the log says when it is full).
- The render graph window shows the nodes a mod removed.
- A mod that removes a game node can have that node's outputs cleared to a value it chooses, instead of leaving them
  stale.

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
