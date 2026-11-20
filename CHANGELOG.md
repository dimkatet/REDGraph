# Changelog

## 0.7.1

- `device_ready` callbacks (shared by every client) were capped at 8 in host.cpp, outside limits.hpp: with several heavy
  mods installed later clients got `RG_E_FULL` and silently drew nothing. Now `RG_MAX_DEVICE_FNS` (32, overridable,
  in the `limits:` log line), and a refused `device_ready` states its reason in the client summary. The SDK guide
  notes that `device()` needs no slot.

## 0.7.0

- For mod developers: `lease_rt_scope(lease, scoped)` - scopes an RT lease (`lease_register` with `rtlibs`) to its
  owners' raygen variants (`rt_variant`): it is staged only at a `DispatchRays` that runs a variant of one of its
  rtlibs, never at the engine's own dispatches, and an engine RT library that declares the register no longer refuses
  it (logged once per library, the first 8). Until now an RT lease was staged at every `DispatchRays` and so needed a
  register no engine RT library declares - in 2.5 there is no free UAV register in space0 (u3 is a reflection hit
  collection's, u5 an RTXDI shadow library's). The client asserts that no shader that can run inside its variants'
  dispatches reads that register. Register-time, right after `lease_register`; `RG_E_ARG` for a non-RT lease or one
  that is not the caller's, `RG_E_LATE` once registration is closed. One entry appended (the last); API version stays 1.
- A client RT library that uses the NvAPI shader extensions (SER: `NvTraceRayHitObject` / `NvInvokeHitObject`, through
  the `g_NvidiaExt` UAV) now gets its extension slot: the host hooks the driver's
  `NvAPI_D3D12_SetNvShaderExtnSlotSpaceLocalThread` (it is per thread; the engine calls it only around its own SER
  libraries), keeps the slot it registers (u14 space0 until then), and sets it on the creating thread before a state
  object that adds such a library. Without it the driver took `g_NvidiaExt` for a real UAV.
- For mod developers: RenderFlow resources by name are no longer capped (was 16, shared by every client:
  `flow_register`, and the names `node_alloc` publishes). A client that shows engine buffers as debug views reads one
  name per buffer and ran into the cap at 17 (`RG_E_FULL`). A handle is an index into the table and nothing walks it
  per frame, so its length has no cost while rendering; entries are added only while registration is open. No ABI
  change; API version stays 1 (an older REDGraph still answers `RG_E_FULL` at 16, so a mod can check).
- Raised: views (`view_create` / `view_create_texture`) 16 -> 128 (requests for 64-128 buffers) and cbuffers
  (`cbuffer_create`) 8 -> 32. No API change.
- RenderFlow resources by name stop at the handle space (`RG_MAX_HANDLES`, 1024) with `RG_E_FULL`: past it a handle
  would have no owner in the per-handle table.
- The client-visible capacity limits (views, cbuffers, clients, patches, leases, draw companions, RT libraries / hit
  groups / variants, mesh-proxy subscriptions, graph nodes, wraps, passes, handles) now live in one file,
  `src/core/limits.hpp`, as `RG_MAX_*` macros; override one at build time with `-DRG_MAX_VIEWS=256` (a CMake cache
  variable of the same name). Limits that come from a data type are `static_assert`ed. The values in force are logged
  once at startup (`limits: ...`).

## 0.6.1

- Fixed: a GPU crash on cards without linear swept spheres (an alpha tester, an older card: `DEVICE REMOVED
  0x887a0020 DRIVER_INTERNAL_ERROR`). The LSS pipeline option (`RG_HIT_GROUP_LSS`) was added to every RT pipeline
  whenever a client had registered an LSS hit group, with no look at what the GPU can do - and a driver that does not
  report LSS must never get the bit ("behavior is undefined"). The host now asks the device once, through the driver's
  own `NvAPI_D3D12_GetRaytracingCaps` (`CAPS_TYPE_LINEAR_SWEPT_SPHERES`, `CAP_STANDARD`), at the first call that would
  set the option (the engine's own, or the host's `lss_apply_once`); on a "no" - or no `nvapi64.dll` at all - the option
  is never set and the log says so (`this GPU does NOT report linear swept spheres`). The hit groups stay registered
  as plain triangle groups, so a client may put ordinary triangle geometry through them there (Cyberpunk Hair
  Overhaul's ribbon BLAS does). `rg_rt_hit_group_stats.lss` stays "the driver took the option" (0 on such a card).
  No API change.

## 0.6.0

- For mod developers: `mesh_proxy_subscribe_flags(name, meshes, count, fn, user, flags)` - a mesh-proxy subscription
  that says what it NEEDS. `RG_MPF_NEEDS_BVH` is what attaches the instance-desc hook, so `RG_MCF_OUT_OF_BVH`,
  `mesh_proxy_bvh_swap`, `mesh_proxy_bvh_desc` and `rg_hit_records.engine_high` exist only for a subscription that asks
  (~78k calls a frame otherwise, for nothing). `mesh_proxy_subscribe` is this call with flags 0 - a mod that used it for
  the BVH must pass the flag. One entry appended (the last); API version stays 1.

- For mod developers: `rg_bvh_desc.applied_mask` - the InstanceMask the host actually writes into the running desc
  (0 = the proxy is out of the ray-traced scene, a swap's `RG_BVH_MASK` when one set it), where `mask` stays the
  game's own write. That makes `RG_MCF_OUT_OF_BVH` and a swap's mask observable without a GPU readback. Field appended;
  API version stays 1.

- `mesh_proxy_bvh_desc` and `mesh_proxy_frame` accept an OLDER `struct_size` (`RG_BVH_DESC_V1` = 80,
  `RG_MESH_PROXY_FRAME_V1` = 16) and write only the fields the caller's size holds, so appending a field no longer
  breaks a mod built before it; `rg_vram_stats.retire_pending` now counts the resources a `mesh_proxy_bvh_retire` holds
  for a live swap.

- `RG_HIT_GROUP_LSS` is the host's own capability now: with OMM off the game never calls
  `NvAPI_D3D12_SetCreatePipelineStateOptions`, so the host makes that call itself, once, before the first state object.
  Before, a mod asking for LSS got nothing at all on such a setup (the collection log said `LSS OFF`).

- Fixed: a collection rebuilt after a pipeline failed to link it (`unlink_ours`) leaked the old `ID3D12StateObject`
  (now released once the rebuild took its place), held the exclusive collection lock across the driver's compile (now
  no lock is held across it), and could be handed a blob that is no longer a collection (checked first).

- Fixed: a hit group's collection is known again by its whole 16-byte DXIL hash, not the 4-byte tag its export names
  carry - two collections whose tags collided merged into one entry (their groups together, one slot rebuilt for the
  other).

- For mod developers: `rg_bvh_desc.applied_flags` - the D3D12_RAYTRACING_INSTANCE_FLAGS the host actually writes into
  the running desc (the engine's `flags` with a live swap's `flags_set` / `flags_clear` applied; the engine's while no
  swap is on, since out of the BVH only the mask is zeroed). `flags` stays the game's own write, as `mask` does. The
  field took the struct's tail `pad`, so the layout is unchanged and API version stays 1.

- Two self-tests: `BvhSwapTest` (the instance-desc mechanisms from a client: OUT_OF_BVH, a swap and its desc,
  `RG_E_TAKEN`, `mesh_proxy_bvh_retire` waiting and holding) and `HitRecordsTest` (`rt_hit_records`'s ranges and
  refusals) - `docs/internals.md` "Self-tests".

- For mod developers: `rt_hit_records(count, capacity, out)` - a mod's own RECORDS in the game's hit tables (the
  64-byte entries its hit shaders read, where a swapped instance's contribution points) at indices no other mod can
  take: the host hands ranges out from the top of the tables downwards, one per calling module, and
  `mesh_proxy_bvh_swap` refuses a contribution inside ANOTHER mod's range (`RG_E_TAKEN`) - two mods that shade their own
  ray-traced geometry can no longer overwrite each other's records. `capacity` is the mod's own reading of the tables
  (GetDimensions of the bound table buffers, 200001 on 2.5); `rg_hit_records.engine_high` reports the highest
  contribution the game's own instance descs carried. One entry appended (check `struct_size`); API version stays 1.

- For mod developers: `rt_hit_group` - a mod's own closest-hit / any-hit shaders as a hit group in the game's
  ray-tracing hit-group collections (every collection with the mod's payload size), so its own ray-traced geometry can
  be shaded; `rt_hit_group_identifier` gives the group's shader identifier for a record the game filled with one of its
  own, `rt_hit_group_pairs` every such pair at once (for a mod that recognises the game's records on the GPU),
  `rt_hit_group_stats` where it went - including `uncovered`, the pipelines that link a collection of the group's
  payload without it (logged too). With `RG_HIT_GROUP_LSS` the game's ray-tracing pipelines are created with NVIDIA's
  linear swept spheres support (when the driver accepts it). A collection or pipeline that fails with a mod's group is
  rebuilt without it. Four entries appended (check `struct_size`); API version stays 1.

- For mod developers: a mesh proxy's ray-tracing instance with a mod's own geometry - `mesh_proxy_bvh_swap` puts a
  mod's BLAS (with its hit-group records, instance flags and mask) into the proxy's TLAS instance every frame, the
  game's transform kept; `mesh_proxy_bvh_desc` reads the instance as the game wrote it; `mesh_proxy_bvh_retire`
  releases a BLAS (or what its records read) only once no TLAS can still reference it - a swap that ended is still in
  the TLAS of the frames already recorded, so freeing it at once faults the GPU. One mod owns a proxy's swap (another
  gets `RG_E_TAKEN`). Three entries appended (check `struct_size`); API version stays 1.

- For mod developers: `RG_MCF_OUT_OF_BVH` for `mesh_proxy_hide` - a mesh proxy out of the ray-traced scene (its TLAS
  instance mask 0 from the next frame: no RT / PT shadow, reflection or GI ray hits it), per proxy, switchable every
  frame. No new entry; API version stays 1 (an older REDGraph ignores the flag).

- `[Diagnostics] BvhProbe` (off by default): one `[bvh]` line a frame while it changes - the instance-desc writes of
  the proxies a mod swapped or watches, every TLAS build (instances, buffer, caller) and every DispatchRays (state
  object, call site).

- For mod developers: `engine_buffer(gpuapi_id)` / `engine_buffer_va(gpuapi_id)` - a game buffer by its GpuApi id (one
  a mod read off a game object of its own knowledge) as its `ID3D12Resource` / its GPU address (a buffer may be a
  piece of a larger resource). Two entries appended (check `struct_size`); API version stays 1.
- For mod developers: a mod's own data in video memory - `buffer_create` (a GPU-only buffer, filled by a copy REDGraph
  makes before the next frame), `buffer_ready`, `retire` (a mod's buffer released once the GPU is done with it, by a
  fence REDGraph signals every frame) and `vram_stats` (the video memory budget and use). Four entries appended (check
  `struct_size`); API version stays 1.
- For mod developers: `mesh_proxy_material(proxy, chunk, lod, ...)` - the material and its parameters a mesh proxy's
  chunk has now (an appearance's material change swaps them in place; hidden chunks included). One entry appended
  (check `struct_size`); API version stays 1.

- For mod developers: more draw companions - 32 in all (was 8), shared by every client, and 8 on one engine PSO (was
  4). One companion per shader hash means a vehicle paint alone takes 8 and its glass another 8, so two mods using the
  feature together ran into the cap (`RG_E_FULL`). Leases, their owners and learned shader refs were raised with it
  (32 -> 64 leases, 16 -> 32 owners per lease, 64 -> 128 ShaderRefs each) - a companion holds one lease per register it
  reads, so 32 companions alone can want 32 of them. No ABI change; API version stays 1 (an older REDGraph still answers
  `RG_E_FULL` at 8, so a mod can check).

- Log: a draw companion that does not fit now says why. A registration past the host's 32 (`RG_E_FULL`) names the table
  shared by every client, a companion that does not fit on one engine PSO (8) is reported, and one that does not fit on
  one draw (8 requested) is reported too. All three were silent - the only sign was a return code a mod could ignore.
  `kMaxDrawRequests` was 4 and is 8 with the per-PSO cap, so the 8 companions one PSO carries can all be drawn.

## 0.5.2

- Fixed: the game could crash when a mod asked for a render-graph texture by name at a point of the frame where that
  texture does not exist (yet or any more).
- For mod developers: a draw companion can now read a mod's own data - `lease_register` takes draw companions (in
  `companions`, with stage VS / PS). The companion is drawn only when that data is in place for the draw. API version
  stays 1; an older REDGraph refuses such a lease (`RG_E_ARG`).
- For mod developers: two mods leasing the same register of the same game shader no longer break each other silently -
  the second one is refused (`RG_E_TAKEN`) and the log names the first.

## 0.5.1

- Fixed (0.5.0 only): a mod's code that runs at the end of a render node (for example Cyberpunk Render Tweaks'
  reflection fog) could be skipped for good, so its option did nothing. The 0.5.0 crash fix on game 2.5 was too strict.

## 0.5.0

- For mod developers: the A/B profile (`[Diagnostics] ABProfile`) - a mod registers a switch per feature
  (`rg_host::ab_switch`, new), and `Game.RG_AB(...)` from CET turns them off and on in alternating blocks and logs
  what the feature costs: the frame, the GPU time of its ray-tracing dispatches and extra passes, the CPU per engine
  node, per callback and in the mod's own hooks (`prof_active` / `prof_add`, new). API: three entries appended (check
  `struct_size`); API version stays 1. Off by default; players are not affected.
- Fixed: with `[Diagnostics] GpuTiming` on, game 2.5 crashed at the first rendered frame.
- For mod developers: `[Dev] ShaderHotReload` in `REDGraph.ini` - drop a `.dxbc` or an edited `.ll` into a folder and
  a registered patch / companion runs it at once, without restarting the game (`docs/internals.md`, "Shader hot reload
  (dev)"). Off by default; players are not affected.
- For mod developers: `[Dev] Readback` - `Game.RG_Readback("flow:<name> after:<node>")` (or `block:<engine block>`)
  from CET copies a render-graph texture at that node, or the next write of an engine constant block, into a file
  (`.dds` / `.bin` + `.json`), and `Game.RG_ReadbackResult(id)` says where, with min / max / NaN (`docs/internals.md`,
  "GPU readback (dev)"). Off by default; players are not affected.
- For mod developers: `[Dev] Readback` also takes `vsout:<VS hash>` - what a game vertex shader outputs at its next
  draws, by Stream Output, with cb12 of the same frame - and `vsout:<hash> blas:1` - the same plus the ray-tracing
  copies (BLAS build inputs) of the meshes it draws, in the same frame. With the switch on, the game's graphics root
  signature allows Stream Output.
- Fixed: with `RenderGraphDump` or `RenderGraphDebug` on, `[Dev] ShaderHotReload` never looked at its folder.

## 0.4.2

- **Game 2.5.** This version runs on Cyberpunk 2077 2.5 only (needs RED4ext for 2.5); for 2.31 keep 0.4.1.
- `RG_ENGINE_CB12` is 2512 bytes on 2.5 (was 1584): the game added fields to cb12.
- API unchanged; API version stays 1.

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
