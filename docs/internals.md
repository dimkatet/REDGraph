# REDGraph internals

How the host works inside the engine - for people changing REDGraph itself. Modders writing a CLIENT need only
[`sdk/README.md`](../sdk/README.md) and [`include/rg_host.h`](../include/rg_host.h).

Every address below is a Cyberpunk 2077 **2.31** RVA (the exe base is 0x140000000; "0x1401f7224" and "0x1f7224" name
the same function). They all live in [`src/core/engine_231.hpp`](../src/core/engine_231.hpp) (the constant-block
hooks keep theirs in `cb_hooks.cpp`). Section [Finding engine addresses](#finding-engine-addresses) says how they were
found and how to check a new one.

Each mechanism carries the milestone tag the code comments use (M1..M12): the order they were built in.

## Overview

```
include/rg_host.h        THE API: plain C, versioned, append-only. Clients include only this.
src/plugin.cpp           RED4ext entry: settings, crash log, core::open / arm, the CET functions
src/core/                the host CORE - knows no client
  host.cpp               the rg_host table, registration window, attach-on-demand, the w_* wrappers (clients tab)
  engine_231.hpp         every 2.31 RVA and struct offset
  pso_patch.cpp          M1  PSO clone + live swap; the bind funnel; patch scope (M11); shader skip (debug)
  bindings.cpp           M2  leases: our descriptors staged into the engine's own tables
  rt_lib.cpp             M3  RT libraries at the state-object creator; M12 raygen variants
  passes.cpp             M4  dispatch subscriptions, companions, after-dispatch passes, RestoreState
  cb_hooks.cpp               engine constant blocks (cb12, fog / cloud b6)
  render_flow.cpp        M5  node context per thread, RenderFlow by name, request record / replay, jobs carry node
  render_nodes.cpp       M6-M9 our nodes, declarations, wrap / remove of engine nodes; the debug slots
  graph_probe.cpp            graph walk, dump, census (node -> shaders / resources / DispatchRays)
  graph_debug.cpp            the CET graph debug module's state and report
  clients.cpp                who registered what (the Clients tab)
  prof.cpp                   the host's own CPU profile
  crash_log.cpp              vectored exception handler, device-removed watchdog, DRED
src/settings.cpp         REDGraph.ini
src/ui.cpp               RTTI global functions for the CET window (Game.RG_*)
cet/REDGraph/init.lua    the CET window
selftest/                REDGraphSelfTest.dll - the host's tests, a client like any other
```

Clients are other RED4ext plugins. They find the host with `rg_host_when_ready` (any load order: a client that loads
first waits in a process-wide rendezvous, a named file mapping `Local\REDGraph.rendezvous.<pid>`, and the host runs its
init in the host's own Load) or directly via the export `rg_host_get`.

## Lifetime: registration and attach

| step | when | what |
|---|---|---|
| `core::open` | REDGraph's RED4ext Load | the log works; the table is served |
| `drain_clients` | right after | clients that loaded earlier and wait in the rendezvous register now |
| clients' Load | RED4ext loads plugins alphabetically | a client loading after REDGraph registers in its own Load |
| `core::arm` | end of REDGraph's Load | hooks `GpuApi::InitDevice` (0x7e0bb4, called from `CRenderInterface::Init`) |
| `core::attach` | at `InitDevice`'s entry | registration CLOSES (`RG_E_LATE` after this); the engine hooks the registrations need are attached |

`InitDevice` creates the device and the global root signatures; no PSO or RT state object exists yet, and every
RED4ext plugin's Load has run before it. If that hook cannot attach, `attach` runs at the end of Load instead.

The flag file `no_pso_swap` next to the DLL makes `attach` attach nothing native and claim no shader (the user's A/B
baseline); `no_hooks` leaves out the constant-block hooks only. The build option `RG_NATIVE_HOST=OFF` compiles the
native part out (for a bisect).

## Core rules

* **Hooks are attached on demand.** No RT library registered -> no state-object hook; no lease -> no
  `UpdateRootParams` detour, and so on. Fewer collisions with other mods' hooks, zero cost for unused mechanisms.
* **Every hook checks its call site first.** `call_site_ok(base, ret, target)`: the `E8` call right before a known
  return address must land on the target. A different exe fails the check and that mechanism stays off.
* **Variants are built eagerly.** The full PSO desc exists only inside `PSOSubCache::Create`, so all of a patch's
  variants (up to `RG_MAX_VARIANTS`) are cloned there at once; the bind funnel then picks one per bind.
* **Patches of different stages compose on one engine PSO** (since 2026-10-10): a VS, a PS and an added GS patch -
  any clients' - each switch on their own; see [PSO patches (M1)](#pso-patches-m1). One stage of one shader is still
  ONE patch (below).
* **Two clients on one target: one wins** - higher `priority`, ties by registration order. The loser gets
  `RG_E_TAKEN` (or is refused later with the reason in the Clients tab).
* **A handle belongs to the module that made it.** `clients.cpp` records the owner of every handle (the module the
  registering call returned to) and each client's image range; every entry that takes a handle - the registering
  wrappers and the `[hot]` ones (`h_*` in `host.cpp`: a scan of the client ranges, no lock) - refuses another
  module's with `RG_E_FOREIGN`, logged. The `*_stats` calls are read-only and take any handle. A client's call in
  TAIL position returns straight into the host, so its return address names the host: while the host runs client
  code on a thread (a waiting init, and since 2026-10-10 every callback - subscription, pass, node / wrap fn,
  device_ready; `clients::Running`) such a call is that code's module's. Before that, a tail-called
  `pass_after_dispatch` from a subscription was refused as "an unknown module".
* **Clients share resources only through the engine's graph, by name.** A `node_alloc` transient is PUBLISHED under
  `"<mod>/<name>"` (one publisher per name); another client reads it with `flow_register(name, 0)` + `node_use`, and
  the engine owns its lifetime, barriers and frame order - the host brokers no raw D3D12 objects between clients. A
  node's name is unique among all clients' nodes (it is its class name in the graph).
* **The host never writes the engine's shared data.** Swaps and additions edit the CREATING thread's own lists (PSO
  desc copies, the state-object subobject list), so no engine object waits on a lock of ours.
* **Failure falls back to vanilla.** A clone the driver refuses, a bake that fails, a lease the vanilla shader would
  collide with: the engine's own object runs, the reason is logged.
* **Objects the engine consumes live on the engine's device** (`rg_host::device`, `kDeviceData + kDeviceInData`) -
  with ReShade loaded that is ReShade's proxy, and a heap made on the underlying device crashes it.

## Engine hooks

| hook (2.31) | mechanism | runs | attached when |
|---|---|---|---|
| `PSOSubCache<compute/graphics>::Create` 0x88dc58 / 0x88f364 | M1 identify + clone | every PSO creation | any patch, companion, subscription, lease, census |
| `ComputePSO/GraphicsPSO::CreatePSO` 0x1f7224 / 0x1f744c (from `SetupForDispatch/Draw`) | M1 bind funnel: engine PSO -> clone | every state change before a draw / dispatch | same |
| `SFrameResources::UpdateRootParams` 0x1f22e4 | M2 stage / restore leased descriptors | before every draw / dispatch with dirty tables | any lease |
| `GpuApi::DispatchCompute` 0x1f6ed4, `DispatchIndirectCompute` 0x6232ac | M4 post-hooks | every compute dispatch | any companion, pass, subscription |
| state-object creator 0x315f68 + its subobject append 0x316398 | M3 swap / ADD of RT libraries | every RT state object built | any RT library |
| global `DispatchRays` 0x2934058, `GetShaderGroupIdentifier` 0x3141f0 | M12 raygen variants; census rows | every RT dispatch | any variant, census |
| `CRenderNodeBase::Process` 0x1ec404 | M5 node context per thread | every node run | flow resources, scope, census, debug |
| `rend::NodeGraphFactory::Register` 0x98585c | M6 our nodes; M8 wraps (vtable slot patch) | every graph build | any node, wrap, removal, debug |
| `SRenderFlowInternalData::HandleRequest` 0x1f0f80 | M9 record / replay; census rows | every RenderFlow request | any removal, census |
| `job::Dispatcher::RunJob` 0x14299c | jobs carry their node | every job submission | node context (scope / census / debug) |
| `CRenderNodeGraph::ExecuteParallel` 0xa86438 | frame tick: census windows, graph walk, profile | once per frame | always (one detour per frame) |
| `GpuApi::UpdateBuffer`, `SetComputeShaderConstsRaw` | engine constant blocks | filtered on return address | always unless `no_hooks` |
| `GpuApi::InitDevice` 0x7e0bb4 | registration window | once | always |

Detours go through RED4ext's hooking API (`sdk->hooking->Attach`). Other plugins compare prologues in their own Load,
so REDGraph attaches late (at `InitDevice`), after every plugin's Load.

## PSO patches (M1)

**Identity.** `PSOSubCache<desc>::Create(self, ComPtr* out, base, u32, u32 key, const desc*)` is the only place the
engine's full PSO desc - every stage's bytecode - exists. The host reads the 16-byte digest of each stage's DXIL
`HASH` chunk (`common/dxil_hash.hpp`) and looks it up in the registrations. A match: one clone per variant from the
same desc with the client's bytecode in that stage (`CreateComputePipelineState` / `CreateGraphicsPipelineState` on the
engine's device), stored in an insert-only open-addressing table engine PSO -> `clone[variant]` (written under a lock
at creation, read lock-free).

**Composition.** A graphics PSO's VS, PS and (added) GS may each be patched - by different clients. The PSO's
mapping then holds those patches and a clone for EVERY combination of their states: with variant counts n_i, prod(n_i
+ 1) - 1 clones, `combo = sum (variant_i + 1) * stride_i` (`stride` the mixed radix; combo 0 = all vanilla = the engine
PSO). At the bind each patch decides alone - selected, in scope, its leases staged - and the funnel returns
`clone[combo]`; a patch that is off (or whose lease did not stage for this draw) leaves its stage vanilla without
taking the others with it. A combination that failed to build keeps the engine PSO for that combination only (logged
with every patch in it). Worst case 4 variants on 3 stages = 124 clones of one engine PSO, built on the engine's own
PSO-creation thread like the rest. Until 2026-10-10 a PSO took ONE patch (the VS won, the PS patch was logged and lost
there). `ComposeTest`.

**Pipeline state** (since 2026-10-10). `patch_state(patch, variant, rg_pso_state)`: a graphics variant also edits the
COPY of the engine's desc its clones are built from - by group (`RG_PSO_CULL`, `_FILL`, `_DEPTH_BIAS`, `_DEPTH`,
`_BLEND` / `_WRITE_MASK` per render target in `rt_mask` - `IndependentBlendEnable` is switched on with RT0's desc copied
to all when a target other than 0 is named - and `_FORMATS`); the groups not named stay the engine's. A variant blob
`{NULL, 0}` (VS / PS) keeps the engine's bytecode: a state-only variant. Composed patches apply theirs in stage order
VS, PS, GS; a group two of them set goes to the later (logged once). A separate call, not a field of
`rg_patch_desc`: that struct carries no size, so it cannot grow. The engine still binds its own targets - a format
edit that does not match them is the client's error. `StateTest`.

**Swap.** `ComputePSO::CreatePSO` / `GraphicsPSO::CreatePSO(state)` is the bind funnel: `GpuApi::SetupForDispatch` /
`SetupForDraw` call it for the PSO to set. The detour maps the engine PSO to the selected clone (`patch_select`, one
atomic per patch; -1 = the engine PSO). A clone the driver refused stays NULL and the engine PSO binds.

**Scope (M11).** `patch_scope(patch, node)`: the funnel swaps only when the innermost node recording on this thread
(`render_flow::current_node`) has `GetName == node` - a class (`CRenderNode_RenderSkyScattering`) or a group
(`Lighting`). Elsewhere the engine's shader stays and `out_of_scope` counts it. Cost: one virtual `GetName` per bind of
a scoped patch's PSO. The log names the first bind inside and the first outside.

**Shader skip (debug).** If the funnel returns NULL, `SetupForDraw/Dispatch` returns false and the engine records no
draw / dispatch (`test rax,rax; je` at 0x1401f6f7d / 0x1401f711f). The graph debug module uses that.

## Leases (M2)

A client's descriptor reaches a patched engine shader through the ENGINE'S OWN descriptor tables - no root-signature
extension, zero root DWORDs.

GpuApi never binds a resource into the root signature directly: `SFrameResources::BindDescriptor` writes a CPU handle
into the context's staging array (`fr+0x28`) and marks that table dirty; `UpdateRootParams` (right before every draw /
dispatch) copies every dirty table into the engine's shader-visible ring and sets it. So before the engine's
`UpdateRootParams` the host stages ITS handle (a CBV / SRV / UAV in the host's CPU-only heap) into the leased register
when the bound shader is one of the lease's owners, and gives the engine's handle back at the next other draw.

The register inventory comes from the engine's root-signature DEFINITION (`fr+0x68`): per stage, CBV b0-b13, SRV
t0-t127, UAV u0-u18 in space0; a byte map gives each register's bucket, `0xff` = not covered. What it COVERS (logged
once, `native table: XS covers`; 2.31, 2026-10-10): VS / PS b0-b13 t0-t126 u0-u18, CS b0-b13 **t0-t92** u0-u18. A CS
register past t92 is not even in the root signature: a companion reading it fails to build (`E_INVALIDARG`) - the first
LeaseTexTest run leased t120 and lost its companion that way. A lease is granted only if:

| check | when | failure |
|---|---|---|
| every owner blob (patch variant / companion / RT library) reads exactly that register | registration | `RG_E_*` |
| the VANILLA shader does not read it (its `PSV0` bindings) | the engine's first PSO of that shader | the owner is refused (vanilla) |
| the definition covers it | the first `UpdateRootParams` | never staged; patch never swaps |
| (SRV) its table is not STATIC at that moment (`fr+0x78`, a resource pack at t0) | per draw | not staged (`static_skips`) |

Leases of different shaders share a register freely: staging is per draw.

**RT leases.** `UpdateRootParams` with `rays = 1` (from `GpuApi::DispatchRays`) forces the CS tables, so those are what
raygens read. A `DispatchRays` carries no shader identity, so an RT lease stages at EVERY RT dispatch. That is safe
because every library the engine creates is checked (its `RDAT`) not to declare the register; a source not ready
stages a NULL descriptor. Hence **one RT lease per register in the process** - a second gets `RG_E_TAKEN`.

**RT lease delivery** (0.3.3, after a client's raygen variant faulted reading VA 0 at a rare `DispatchRays` - ~once in
30 min, two users, the Aftermath shader hash = the client's library; its only input the engine's raygen lacks is an RT
lease). `GpuApi::DispatchRays` calls `UpdateRootParams(rays = 1)` BEFORE `GetShaderGroupIdentifier` (0x142a40756 vs
0x142a407e1), on one thread. So per dispatch the recording thread keeps what every RT slot group got (staged / already
ours / NULL / static-skip / lost / no staging array) and, AFTER the engine's copy, re-checks the staging slot: not ours
any more = GONE, its table's STATIC bit still set = STATIC (a static table is bound as it is; the staging array is not
copied - before 0.3.3 only an SRV's static table was caught). `d_gsgi` hands a variant out only if every RT lease of
its library landed; otherwise THAT dispatch runs the engine's raygen (`rt variant: ... the ENGINE's raygen ran this
dispatch instead`, the first 16 and every 2^n). Totals: at hand-out 1 and every 2^n from 1024, and in the crash log;
a rays marker carries the dispatch's lease state (`leases: b3=new#12 b4=ours#20 t26=null#33`, # = our heap index,
decoded by the crash log's `RT leases in our heap` line).

**Context entries** (0.3.4). Staging over an ENGINE handle remembers it per `SFrameResources` in one of 64 entries, freed
when the last group is given back. A context the engine retires with a group still pending never gives it back - before
0.3.4 its entry leaked, and a user session filled all 64 after 38 min: from then on every lease of every NEW context
was LOST (RT: the engine's raygen ran; raster / compute: `lease_bind_ok` kept the engine's PSO) until a restart. Now an
entry unused for 600 frames is taken over. Log: a new peak of entries in use (from 16), `took over the entry of a
context unused for N frames` and `all 64 context entries are in use` (the first 16 and every 2^n); the RT totals line
ends with `context entries: in use, peak, taken over, all in use`.

**Companions' leases** stage at every dispatch of the engine shader they accompany (inert for the vanilla shader);
`companion_request` is accepted only when the descriptors sit in that dispatch's table.

What a lease binds: a host cbuffer (`cbuffer_create` / `_write` / `_feed`, a ring on the engine's device; UPLOAD, or
GPU_UPLOAD when available), a view of a client-owned **buffer** (`view_create` / `view_set_resource`), or - since
2026-10-10 - a **texture** view (`view_create_texture`): of a client-owned texture, or of an ENGINE RenderFlow texture
by name (`desc.flow`). Every owner must declare the register as the view's kind - the `PSV0` entry's `ResKind` (16 ->
24-byte entries, PSV version 2+; the game's blobs and ours carry it) or the `RDAT` resource record's `kind`, both the
DXIL `ResourceKind`; `rg_tex_dim` uses the same numbers. Checked at registration; a blob without the field is accepted.
A texture view keeps a NULL descriptor of its own dimension after its ring - what an RT lease stages while the source
is not there (the shared NULLs are raw buffers).

**A flow source** has no resource to set: at the stage, inside `UpdateRootParams` on the recording thread, the host
resolves the name exactly as `flow_texture` does (the node context of that thread) and writes a view of what it finds
into the next ring slot - once per (resource, engine id, frame): a transient is a new resource every frame and its
address can come back for another. A slot is overwritten only when the ring comes round, long after the engine copied
it into its shader-visible table (the copy happens in the same `UpdateRootParams`). Not resolved (outside a node, not
alive at that point) = not staged: the patch keeps the engine's shader for that draw, the companion is not dispatched,
an RT lease stages the view's NULL. The engine does not know the shader reads it - no lifetime extension, no barrier:
the client scopes the owners to nodes where it is alive and puts it in a readable state (`flow_transition` in a
`node_wrap` before) unless the engine's own use already leaves it so. A resolved texture that does not fit the view
(dimension, samples, a TYPELESS format with none named) is logged once and the view never stages again.

## RT libraries (M3)

The creator `0x315f68` (`bool(StateObjectData* out, const u32* blob id)`; only caller the StateObject resource
container's Create, from `CRenderInterface::InitDevice` and lazily from
`CRenderAccelerationStructure::PrepareAccelerationStructures`) takes an id into the GpuApi blob container
(`kBlobContainer`, entries `{ptr, u32 size}`). The blob is a `CPOS` wrapper as stored in `staticshader_final.cache`:
header 0x1c, UTF-16 export names, then the DXIL library.

The creator reads the DXIL bytes in ONE place - `BlobView::Range(entry + 0x14 ..)` at 0x140315fd9 - into the
DXIL-library desc it appends at 0x316143 with `NumExports 0` (= export all); the export names at `+0x10` feed the
hit groups / raygen names. It builds its subobject list through a 0x24-byte append (0x316398: a fixed 64 x 16 B array,
count at +0x400, no bounds check). One detour on that append, armed per creation on the creating thread, does both:

* **Swap** (a library without variants; the highest priority on that engine library): the engine's DXIL-library
  subobject is replaced by one pointing at the client's DXIL (`NumExports 0`). The export names stay the engine's.
  Bake-time only, no live switch.
* **Add** (a library WITH variants): the client's library goes in right after the engine's as a second DXIL-library
  subobject exporting ONLY the variants (`pExports`); the engine's stays byte-identical. Several clients can add to
  one engine library (up to 8; an export name already taken in that state object is left out with a warning, and
  `rt_variant` refuses a duplicate name up front). Such blobs carry only their variants (`rt_raygen_variant.py --only`).

A failed creation is retried with nothing armed (vanilla). The shared container entry is never written, so creations
run fully in parallel. (Until 2026-10-10 the swap rewrote the entry for the length of the call under an exclusive
lock, which serialized every state-object build in the game behind the driver's DXR compile - tens of seconds on a
cold driver cache.) `rtlib_stats`: swapped / added / failed state objects per library.

### RT per node

M12, `rt_variant(lib, raygen, variant, node)`: "in node X, instead of raygen Y run Z", switchable live.

Why not a duplicate state object: `GpuApi::DispatchRays` (0x2a406e4) builds the RAYGEN record per dispatch, but the
MISS and HIT tables are engine buffers filled with the ORIGINAL state object's identifiers - a duplicate would run
against foreign identifiers.

So: every RT node's Execute calls ONE global `DispatchRays(eRayTracingStateObject so, u32 a2..a8, BufferRef)`
(0x2934058). `so` -> a 24 B entry at `[kRtSoTable] + 0x4648` (+0 the state-object id) -> `SetPipelineState1` on the
`ID3D12StateObject` at device data `+0x13b2c48 + (id-1) * 0x38` - exactly the creator's `out` (+0 the ComPtr, +0x10
the export names). `a4` = the raygen group, `a2 x a3` the grid. The hook reads the dispatch's state object, the
libraries baked into it and the raygen's name; for a variant of that raygen that is on and in scope (highest library
priority wins, an off one falls through) it arms a thread-local override, and the hook on `GetShaderGroupIdentifier`
(0x3141f0 - which `GpuApi::DispatchRays` calls for exactly that raygen record) returns
`GetShaderIdentifier(variant)`. Root signature, miss / hit tables and every other export stay the engine's.

## Work on the engine's command list (M4)

Post-hooks on `GpuApi::DispatchCompute` and `DispatchIndirectCompute` (attached only for a companion, a pass or a
subscription):

* **subscription** (`dispatch_subscribe`): `fn` runs right after every dispatch of an engine PSO carrying the shader,
  on the recording thread, the engine's Dispatch already recorded. From inside `fn` a client may request:
* **companion** (`companion_register` / `_request`): `SetPipelineState(companion) -> Dispatch ->
  SetPipelineState(engine PSO)`. Same root signature, every root argument still bound - no restore. Scoped by
  `companion_scope` like a patch.
* **pass** (`pass_after_dispatch`): `fn(user, list)`; returning `RG_PASS_TOUCHED_STATE` makes the host run
  `GpuApi::Hacks::RestoreState(list)` (0x2a3e9d4: re-ensures both root signatures - all params dirty - and re-binds
  the heaps) and clear the PSO with both PSO states dirty.

A request whose context (or, for a companion, bound PSO) changed before the engine's Dispatch returned is dropped.

### The graphics wing (draws)

2026-10-10, hardware-confirmed 21:48 (`DrawTest` 5 / 5 phases PASS: companion draws exactly per request, 0 refused;
600 / 600 passes after a draw; the visible sky 5f079e10 is a SYSTEM draw, 4 vertices x 1). Every draw resolves its PSO through `GraphicsPSO::CreatePSO`
from exactly two places: `GpuApi::SetupForDraw` (callers: `DrawPrimitive`, `DrawPrimitiveNoBuffers` - the full-screen
passes, `DrawTriangleNoBuffers` tail-jumps there -, the three indirect draws, the two system-primitive draws) and
`DrawIndexedPrimitive` (SetupForDraw inlined; the world's meshes, decals, skinned meshes, light volumes). Each records
one D3D12 draw at its end; the host post-hooks all eight (attached only with a draw subscription;
`engine_231.hpp` "The graphics wing" has the RVAs, call sites and argument maps).

The bind is NOT per draw: `CreatePSO` runs only while the graphics-dirty byte (`ctx+0x120`) is set - `SetShader` sets
it, a batch of instances binds once (compute is per dispatch: `CRenderShaderCompute::Dispatch` calls `SetShader`
every time). So the funnel leaves `{context, the PSO it returned, the engine PSO, its draw-subscription mask}` in a
thread_local at EVERY graphics bind, and a draw hook reports a draw iff that PSO is still the one on the list
(`ctx+0x3d0`) and the dirty byte is clear (a NULL PSO - still compiling, a debug skip - leaves it set: nothing drawn).
Every other draw costs one thread-local load.

* **draw subscription** (`draw_subscribe(name, stage VS / PS, hash, fn)`): `rg_draw_info` = the D3D12 call made
  (instanced / indexed with its counts; indirect with counts 0; `RG_DRAW_SYSTEM` for uploaded primitives - the visible sky's bottom layer 5f079e10 is one, UI, debug; such an
  entry returns without drawing on a zero count, which is therefore not reported).
* **draw companion** (`draw_companion_register(name, stage, hash, shader, state)` / `_request`): a PSO per engine PSO
  carrying the hash, from the engine's desc with that stage's bytecode and / or `rg_pso_state` applied; recorded as
  `SetPipelineState(companion) -> the same draw -> SetPipelineState(what the engine drew with)` (indirect: the
  engine's command signature at device data `+0x13bc678` / `+0x13bc670`, `GetD3DBuffer(ctx+0x6c)`, offset
  `ctx+0x70` - still bound). No leases (v1). `draw_companion_scope` as `companion_scope`.
* **pass after a draw** (`pass_after_draw`): as after a dispatch, plus the topology cache (`ctx+0x628`) cleared on a
  restore. NOT restored: render targets, viewports, scissors, VB / IB, stencil ref, blend factor - the next draw of the
  batch re-sets none of them.

## Engine constant blocks

`rg_engine_block` (`cbuffer_feed`, `engine_block`): copies of constant data the engine writes, caught at the write -
`GpuApi::UpdateBuffer(BufferRef, size, data)` from `CRenderInterface::CalculateSharedConstants` (cb12, size 0x630) and
`GpuApi::SetComputeShaderConstsRaw(size, data)` (the fog CS b6, the cloud bake's b6, the world clouds' b6). Both detours
filter on their return address and size before taking a lock.

## The render graph

Not a call chain: a DATA graph, built by code and cached.

| piece | what | where |
|---|---|---|
| nodes | ~250 classes `CRenderNode_*` : `CRenderNodeBase`; every vtable has 6 slots: [0] dtor, [1] `Execute(SRenderNodeImplContext&, job::Builder*)`, [2] `GetName(CName*)`, [3] `CreateCommandList`, [4] `GetJobBuilderUsage`, [5] `GetCommandListUsage` | e.g. `??_7CRenderNode_VolumetricFog@@6B@` 0x142b39e20 |
| creation | `rend::NodeGraphFactory::Create<T>(group, type, subtype)` -> pool alloc -> `Register` -> `CRenderNodeGraph::AddNode` | 0x98585c |
| groups | `rend::CRenderNodeCommandListGroup` (one vtable 0x142b3a008, name at +0x18): members `{CRenderNodeBase*, u64}` at +0x28, count +0x34, run in order by its Execute (0x1ec1d0) | `BeginCommandListGroup` 0x9853b4 |
| edges | `AddDependency(from, to, type)` - type 0 = CPU job dependencies (a DAG through `Synchronize`), type 1 = the submission chain (one linear chain). The union is NOT acyclic - never order by both | 0x986104 |
| caching | `CRenderGraphCache` keyed by `SCameraHash`; rebuilt on a setup change (render mode, load), not per frame | |
| execution | `CRenderNodeGraph::ExecuteParallel` (0xa86438, one call site in `RenderFrame`) -> jobs -> group `Execute` -> `CRenderNodeBase::Process` (0x1ec404) -> `Execute` | |
| resources | "RenderFlow": nodes request resources by NAME HASH through `SRenderFlowInternalData::HandleRequest` (0x1f0f80, 524 call sites, mostly inlined) | |

**The graph is a superset.** RT, PT and raster graphs are identical by name except for the shadow cascades (raster) and
the acceleration-structure nodes (RT / PT); the render mode is decided inside `Execute`. The top level is ~90 nodes:
~45 command-list GROUPS (`DepthPrepass`, `GBuffer_*`, `Lighting`, `Transparents`, `PostFX`, ...), plain nodes, and
dummy group inputs / outputs with no object. Groups hold ~300 members. [`sdk/reference/nodes.md`](../sdk/reference/nodes.md)
lists them with their shaders.

**Process.** `Process` binds the node's command list per `GetCommandListUsage` (2 = the group's own list, 1 = the flow
thread's current list), calls `Execute` VIRTUALLY, unbinds, then - if `GetJobBuilderUsage` - runs a job with the node's
builder and waits for it (such work records AFTER `Execute` returns). A group's `Execute` starts with
`BindCommandList(0)` and every member unbinds after itself: nothing is bound when a group's `Execute` returns.
`GpuApi::BindCommandList` (0x1ec68c) is a TLS binding only.

### Layout

`CRenderNodeGraph`: two sparse sets (`TRenderNodeGraphArray`: element data + a dense list of live indices).

| field | nodes | deps |
|---|---|---|
| live count | +0x10 | +0x40 |
| element data | +0x18 | +0x48 |
| data size | +0x24 | +0x54 |
| live list (u32*) | +0x28 | +0x58 |

`SNodeData`, 48 B: +0 dense position (-1 = free), +8 type, +0xc subtype, +0x10 `CRenderNodeBase*`, +0x1c u16, +0x20 /
+0x28 in / out edge list heads (one per dependency type). `SDepData`, 24 B: +4 type, +8 from, +0xc to, +0x10 next
out-edge, +0x14 next in-edge. Handles: node index | 1<<29, dep index | 1<<30; 0x80000000 = end of list.

### Requests

`SRequest`, 0x30 B: +0 u8 type, +1 u8 the flow thread (`ctx+0x38`, one per group / command list), +4 u32 the tag =
name hash ^ (view << 24), +8 u32 param or a 40 B `SRenderFlowTargetDesc`.

| type | wrapper | payload |
|---|---|---|
| 1 | `RTAlloc` / `RTTempAlloc` / `RTSharedAlloc` | the desc |
| 3 | `RTIsAlloc` | - |
| 4 | `RTUseBegin(tag, access)`: 1 read, 2 write, 4 keep the contents | access |
| 5 | `RTUseEnd(tag)` | - |
| 2, 7, 8, 0xb, 0xd | rare; meaning open | |

**The graph runs TWICE a frame.** Phase (`[data+2]`, `SetPhase` 0x1eda84): 1 = DECLARE (every node's `Execute` runs
with no command list and only issues requests; each is recorded per flow thread), 2 = `Resolve` then the real run.
`Resolve` (0x24cac8) sorts the requests by (flow thread, index): a resource lives from its alloc to its last request in
that order - the flow thread number IS the frame order of the groups (StartRender 0 ... Lighting 68 ... Transparents 74
... PostFX 80 ... EndRender 84). **Phase 2 matches a request to its declaration by (thread, index), not by hash**: a
request issued in one run only shifts every later request of that thread onto another resource's events. At a
lifetime's first request the engine places an aliasing barrier (+ `DiscardTexture` unless access bit 4), at its last
`MakeStateSafeToRetire`. State transitions are not RenderFlow's - GpuApi tracks them at bind time.

`SRenderFlowTargetDesc` (40 B, ctor 0x1edcd0): +0x10 / +0x12 u16 width / height, +0x14 / +0x16 the same for an absolute
size, +0x1a u16 array size, +0x20 u8 flags (0x39 = render target + UAV), +0x25 u8 the ENGINE's format (5 =
R8G8B8A8_UNORM).

### Resources by name (M5)

`render_flow.cpp` hooks `Process` (node + ctx per recording thread, nested for group members) and resolves a name the
way the engine's nodes do: `GetResource<TextureRef>(**(ctx+8), &id, hash ^ view << 24, ctx[0x38])` (0x1f3d20) -> the
GpuApi texture container (`SDeviceData + 0x2F1C0`, a 0x10 header, then 32768 handles of 0xB0 B: refcount, ..., +8 the
`ID3D12Resource*`; handle = id - 1). A transient is aliased memory: take it between its write and its last consumer,
never across frames. `flow_transition` = the engine's `TransitionTexture` (0x1f40dc, takes a `D3D12_RESOURCE_STATES`)
+ `FlushPendingBarriers` (0x1f7164) on the thread's context.

### Our own node

M6 / M7. `NodeGraphFactory::Register(factory, group, SRenderNodeParameters{type, subtype, CRenderNodeBase*})` is where
every node enters the graph; with a group open (`factory+0xf0`) it is appended to that group's members. The hook waits
for the ANCHOR (its `GetName` hash == FNV-1a 64 of the client's `after`), then registers ours the same way: a 0x20 B
object from the engine's pool allocator (0xa1eff0), our index at +0x18, a vtable whose slots 3..5 are copied from
`CRenderNode_VolumetricFog`'s. The engine frees our nodes itself on every rebuild.

`Execute` runs in both phases; the client's `fn` only in the real run. Because phase 2 matches by index, the node's
RenderFlow declarations (`node_use`, `node_alloc` - a transient of ours, tag = FNV-1a 32 of its published name
`"<mod>/<name>"`; `flow_register(name, 0)` resolves to the same tag, and `render_flow::attach` warns about a name looked
up that nobody publishes) are issued BY
THE HOST in both runs, identically: the allocs, an `RTUseBegin` per resource, [fn], an `RTUseEnd` per resource.

### Wrapping an engine node

M8, `node_wrap(target, before, after)`. The first time the engine registers a node whose `GetName` == target, slot [1]
of its class vtable (.rdata: `VirtualProtect` + one `InterlockedExchangePointer`) becomes the host's `wrap_execute`:
befores, the original, afters - the fns in the real run only. A vtable shared by several names (every group) is
matched by `GetName` per call. After a group nothing is bound, so for `after` the host binds the list the group's
members recorded into and unbinds after (`rebinds`). Several wraps on one node run in registration order.

**Declarations** (`node_wrap_use(wrap, flow, access)`, 2026-10-10, hardware-confirmed 21:48 - `WrapUseTest` PASS: the
declared PostFX node read our Lighting transient 1796 / 1796 intact, the group declaration logged NOT issued): M7's `node_use` for an engine node. The host issues
an `RTUseBegin` per declared resource before the befores (and before the node's own requests), an `RTUseEnd` after the
afters, in BOTH runs, unconditionally - on the removal paths too (outside the recorded window, so a replay is
bracketed the same way). The engine then extends the resource's lifetime to the node: a transient whose last engine
user ran earlier is not aliased before it, and `flow_texture` / a `view_create_texture` lease scoped there resolve it.
Class vtables only: on a group's (shared) vtable nothing is issued - its `Execute` leaves no list bound and its
members' requests come from other threads - logged once when the target turns out to be a group. Not tested: a
declaration of a resource the wrapped node ITSELF begins / ends (nested uses on one tag in one node).

### Removing an engine node

M9, `node_remove(target, on)`. Skipping `Register` would leave the engine a node handle that does not exist (top-level
`Link`) and need a rebuild to toggle. Instead the node STAYS and only its work goes: in the declaration run its
original runs (no list, it only declares) while the host records its requests (48 B each, up to 512 per instance); in
the real run the host REPLAYS exactly those requests through the engine's entry instead of running it. Every alloc,
lifetime, barrier and later node's request index stays as declared; readers of what the node writes get old /
undefined contents. The switch is latched per node instance at its declaration run. Refused: groups and job-builder
nodes. One removal per node class in the process.

### Replacing an engine node

No API of its own: `node_remove` + `node_wrap(after)` on the same class = the client's work in the removed node's
slot, on its command list, inside the resource contract its replayed declarations keep. `RestoreState` restores root
signatures and heaps only; the engine re-sets render targets (`SetupRenderTargets` always calls `OMSetRenderTargets`)
and viewports itself when it needs them.

### Descriptors

M10, `gpu_descriptors(count)`. The engine hands out shader-visible descriptors itself:
`GpuDescriptorRangeAllocator` at device data `+0x149de40`, `AllocateRange(alloc, Range* out, n)` (0x7744b0; Range =
{heap, offset, count}) over a lock-free ring released by the frame's fence - in the heap the engine keeps bound
(`GpuApi::ClearBufferUAV_Zero` 0x774384 uses it with no `SetDescriptorHeaps`). The ring heap is 1 000 000 descriptors.

Before handing out a range the host makes sure the list HAS the engine's heaps bound: a context the engine has not
drawn on yet has `SFrameResources[0]` NULL and `BindDescriptorHeaps` (0xa1ffa8) then binds nothing - so the host binds
{ring heap, sampler heap} itself and leaves `[0]` NULL (the engine still does its full bind at its first draw);
otherwise it runs `RestoreState`. A table set with no heap bound crashes the driver.

### A patch scoped to a node

See [PSO patches (M1)](#pso-patches-m1), "Scope". `companion_scope` is the same for companions, `rt_variant`'s `node`
for raygens.

### Jobs carry their node

Many nodes submit jobs (`job::Dispatcher::RunJob`, 0x14299c - every submission) and wait for them; the work they
record ran outside the node's `Process`. The hook wraps a job submitted while a node (or a job carrying one) runs:
`{thunk, record {fn, data, node}}`; the thunk runs the job with the node set. Jobs a job submits inherit it. Left
outside any node: `CRenderInterface::FrameTick` work (surface cache, BC compression - its own command list) and some UI
draw-buffer batches.

## Graph debug module

`[Diagnostics] RenderGraphDebug = true` (restart). `graph_probe::walk` builds the node tree at every change of the
executing graph (a KEY per node, `<parent>/<name>#<n-th among siblings>`, survives rebuilds); the census pairs every
PSO bind with its node; under the module every node class is wrapped at `Register`. The CET window then offers:

| control | how | refused |
|---|---|---|
| node off | M9 per INSTANCE (32 debug slots keyed by the node object), re-mapped by key at every graph change | groups as such, job-builder nodes, clients' nodes (`node_register`: recognised by the host's own node vtable, shown with the client that registered them), structural nodes (`Set*`, `End*`, `Bind*`, `AccelerationStructure*`, `Init*`, `Synchronize`, `Present`, `DoCulling`, ...) unless "allow structural" |
| shader skip | a rule = the row's stage hashes (+ optional node key); the bind funnel returns NULL | RT rows |

## Census and graph dump

`[Diagnostics] RenderGraphDump = true` writes `render-graph/graph-NNN.txt` next to the DLL, one file per distinct
graph (at most 64 per session), and for 300 frames after each new graph a census: `graph-NNN-shaders.txt` (node ->
stage hashes, binds, PSOs; RT rows with library, raygen record, call site, grid) and `graph-NNN-resources.txt` (node ->
RenderFlow requests by type and phase). The headers record the render mode both by config vars
(`Config::cvPathTracing` / `cvRayTracingEnable`, value at +0x30) and by what ran. [`scripts/sdk_node_reference.py`](../scripts/sdk_node_reference.py)
turns census files into `sdk/reference/`.

## Clients tab

The API carries no client identity. Every registering entry of the table is a host-side wrapper (`host.cpp`, `w_*`)
that records the call under the MODULE its return address lies in (`clients.cpp`); a waiting client's init runs with
its module as the fallback. Refusals found after registration (a lease's vanilla / RT check, a patch taken by a higher
priority, a library a higher-priority swap replaces) are attached with their reason. Shown in the window's Clients tab
with live counters, plus "Shared targets" (one shader / raygen / node / register registered by several clients) and
"Registers". Hot calls (`*_stats`, `patch_select`, `rt_variant_set`, ...) are not wrapped.

## Host CPU profile

`prof.cpp`. Window tab Host -> "Host CPU profile" (`RG_Profile(delay, seconds)`). Every hot detour opens a
`prof::Scope`; the engine's original and every client callback inside it run under a `prof::Pause`, so a point counts
only the host's own code (rdtsc, the TSC rate measured against QPC). Not measuring = one relaxed load per Scope. A
measured reference point (raster and PT, one spot): ~570 us/frame summed over ~19 threads, spread evenly - about 40 us
on any one thread, below run-to-run fps noise.

## Crash log

`crash_log.cpp`. A vectored exception handler (first in the chain; AV, illegal instruction, stack overflow, buffer
overrun, in-page error - not C++ exceptions; at most 12 reports; observe only, never reads memory it was not handed)
and a watchdog thread (`GetDeviceRemovedReason` every 500 ms). Once the device is removed: DRED - the faulting GPU VA,
the allocations around it, the breadcrumbs of unfinished command lists. DRED must be enabled before the engine creates
its device: `[Diagnostics] Dred = true`.

The engine has its own GPU-crash path, `GpuApi::GpuCrashReport` (from `Present` on DEVICE_REMOVED / _RESET and from
`CheckHR_Internal`): breadcrumbs, Aftermath, then the fatal error "Gpu Crash for unknown reasons!"
(`gpuApiDX12Error.cpp:42`) that ends the process - inside the watchdog's 500 ms, so a GPU crash used to leave nothing
in our log. It is hooked at the host's attach (after every plugin's Load): `[gpu] the ENGINE reports a GPU crash`,
then the removal reason and DRED, then the engine's path unchanged. A DRED query that fails is logged with its
HRESULT (0.3.1: on a user's machine `GetAutoBreadcrumbsOutput` returned nothing - the engine's Aftermath likely holds
the breadcrumbs).

DispatchRays markers (0.3.1, `rt_lib.cpp`, only with `Dred = true`): every engine DispatchRays (the one hook) takes a
slot in an 8192-slot ring - its description on the CPU (state object, raygen group + name, grid, node, thread, our
variant ran or the engine's raygen) and two `WriteBufferImmediate` of its seq on the engine's list into a mapped
READBACK buffer (MARKER_IN before, MARKER_OUT after). At removal `crash_log` calls rtlib's dump (`set_extra_dump`):
every slot begun and not ended = `INSIDE`, else the newest done and oldest queued. Answers "was the GPU inside a
client's raygen" for any client. Foreign DispatchRays (another mod at D3D12 level) are not seen. MARKER_IN may be
written before the dispatch really starts: two or three INSIDE at once read together.

AS build markers (0.3.2, same ring, same switch): after own repros faulted in the DRIVER's acceleration-structure
build (`AS Build or Refit`, VA 0) between two DispatchRays, three engine functions are hooked by call site
(`engine_231.hpp` "Acceleration-structure builds"): the BLAS batch builder (one bracket for the batch - its D3D12 path
is a vtable call), `NvAPI_D3D12_BuildRaytracingAccelerationStructureEx` (one bracket per BLAS) and
`GpuApi::BuildTopLevelAccelerationStructure`. Each build is also checked on the CPU before it is recorded: dest,
scratch, update source, every geometry's vertices / indices / AABBs (NvAPI path), the TLAS instance buffer - an input
of 0 where the build reads one logs `[gpu] AS build #n with a ZERO input address` at once (first 32). The engine's
TLAS builder passes InstanceDescs = 0 for an invalid instance BufferRef and keeps the count. Not covered: a BLAS
through the plain D3D12 path is bracketed only as its batch, unchecked; foreign builds (another mod) not seen.

AS lifetime + the light-volume TLAS (0.3.2, same switch): a crash named the light-volume TLAS (built inside
`CRenderNode_RenderRayTracedReflections` by `CRenderLightVolumeCollector::UpdateAccelerationStructures`, BLAS ids
cached by volume data hash) as the build the GPU was in, with a garbage fault VA. `CreateAccelerationStructure`,
`GpuApi::Destroy` (AS) and the batch's entries stamp per 16-bit AS id when it was last created / destroyed / built
(marker seq); `GetAccelerationStructureHandle` called from the collector collects the ids of its instances. At that
TLAS's build each id is judged: never built since the hooks, destroyed after its last build, or re-created after it
= STALE -> `[gpu] light-volume TLAS #n has k STALE BLAS` at once (first 32 bad AS events), and a count in the crash
dump. The first such TLAS logs a self-test line (ids seen, an example id's stamps).

A first-chance access violation is logged even when an SEH handler elsewhere will catch it. Never read engine memory
under `__try`: the engine's crash handler freezes the game on any first-chance AV.

## Self-tests

`REDGraphSelfTest.dll` is a client like any other; `REDGraphSelfTest.ini` next to it, section `[Tests]`:

| key | covers | expected (log) |
|---|---|---|
| `FlowProbe = true` (+ REDGraph's `RenderGraphDump`) | M5-M10: t64 by name, `RgNode_SelfTest` after VolumetricFog with a transient read across groups, wrap of VolumetricFog and Lighting, removal of the world clouds in 600-frame windows (they vanish for 10 s), replacement by a striped sky that samples t64 through engine-ring descriptors, a second removal refused | `node self-test`, `node content test ... bad 0`, `node wrap test`, `node remove test`, `node replace test`; on screen: clouds gone / striped sky every other 10 s |
| `RtSwapTest = 1` | M3 swap: 318cee44 WHOLE, `rgs_shadow_main` "lit" in place (no variant: swapped in the subobject list) | `rt swap test ... swapped N ... PASS` every 600 GlobalShadow runs; on screen: RT sun shadows gone for the session (switch off a client's variant of that raygen, e.g. CRT's grass shadow) |
| `RtVariantTest = 1` / `2` | M12: 318cee44 + `rgtest_rgs_shadow_lit` (RT sun shadows "lit"), live in GlobalShadow / scoped to a foreign node; a duplicate variant name refused | `rt variant test ... swapped` grows (1: shadows vanish every other 10 s) / `out_of_scope` grows (2) |
| `PatchScopeTest = 1` / `2` | M11: the game's own PS 0c359133 swapped for itself, scoped inside / outside its node | `scope test ... swaps` / `out_of_scope` grow |
| `ComposeTest = true` | M1 composition: VS 69090ccd (2 variants) and PS 0c359133 (1), the game's own bytecode swapped for itself, on the sky pass's PSO; four phases of 600 sky runs: VS only, both, PS only, none | `engine PSO ... carries 2 patches ... 5 of 5 combination(s) built`, then per phase `compose test A/B/C/D ... -> PASS` (swaps grow exactly for the patches switched on). Nothing on screen; takes CRT's sky patch for the session |
| `StateTest = true` | M1 pipeline state: the visible sky's PS 5f079e10 with two state-only variants (no bytecode): wireframe, RT0 write mask 0; phases of 600 sky runs; bad fill mode and a variant out of range refused at registration | `state test: registered ... (expected 0 0 -1 -1)`, then `state test phase 'wireframe' / 'write mask 0': swaps +N -> PASS`, `'vanilla': +0 -> PASS`. ON SCREEN only where nothing covers the sky's bottom layer (5f079e10 lies under the volumetric clouds, fog and aerial composite): far off, near the horizon |
| `DrawTest = true` | M4 graphics wing: draw subscriptions on the visible sky's PS 5f079e10 and the sky-scattering VS 69090ccd, a draw companion of 5f079e10 that only sets FILL_MODE_WIREFRAME, a pass after the sky draw that returns `RG_PASS_TOUCHED_STATE` touching nothing; phases of 600 sky draws: none / companion / pass; refusals (stage CS, a companion with nothing to change) | `draw test: registered ... PASS`, host `first subscribed draw ... kind N`, `first companion draw`, `first pass after a draw`, then per phase `draw test phase '...': companion draws +N (requests +N+1 ...) ... -> PASS`; the line names the draw kinds the sky goes through. On screen only far off: wireframe sky edges in the companion phase |
| `WrapUseTest = true` | M8 declarations: our transient `rgwrapuse/tmp` written in Lighting (`RgNode_WrapUseWriter` after ClassifyMaterials), declared by a wrap of `CRenderNode_ApplyBloomAndTonemapping` (PostFX) with `node_wrap_use` and copied in its `before`; a wrap of `DrawHUD` without the declaration as an informational control; registration refusals (access 0, no such flow, a 9th declaration) and a group target | `wrap use test: registered ... PASS`, host `wrap 'PostFX' - a GROUP: its 1 node_wrap_use declaration(s) are NOT issued`, then every 1800 frames `wrap use test #N ... DECLARED: ok N (exact M) stale 0 bad 0 ... miss 0 ... -> PASS` (ok = the pattern with a counter within 2 of the writer's: PostFX may record on the CPU before this frame's Lighting). Nothing on screen |
| `LeaseTexTest = true` | M2 texture sources: a companion of the tonemapping-LUT CS 88c57ef1 (scoped to `CRenderNode_GenerateTonemappingLUT`, after the frame's third dispatch) reads our 4x4x2 R32_UINT texture through a 2D_ARRAY view of slice 1 (t64) and the engine's LUT `2ae9f45c` by name through a 3D UAV view (u16) into our raw buffer (u17); the registration checks (a raw view at a Texture2DArray refused, a cube UAV refused, a buffer into a texture view / a resource into a flow view refused) | `lease tex test: registered`, `views set`, then every 300 frames `lease tex test #N ... client texture 16/16 = slice 1 ... LUT texels 2/2 equal to the copy ... PASS`. Nothing on screen |

RtVariantTest, RtSwapTest, PatchScopeTest and ComposeTest need game bytecode, which this repository does not carry: CMake builds it from the
installed game (`scripts/make_selftest_blobs.py`, `RG_GAME_DIR` + `RG_DXC_DIR`). Without them those tests are
compiled out and log that they are.

## Finding engine addresses

Names come from [cp2077-symbols](https://codeberg.org/bartmoss/cp2077-symbols) (names by address for 2.31);
[`scripts/exe_re.py`](../scripts/exe_re.py) disassembles the exe with them: `func`, `callers`, `up` (caller tree),
`sym`. Symbol names can be wrong where the linker folded identical functions - trust the code. A new hook needs:
the target RVA, a known call site's return address for `call_site_ok`, and a line in `engine_231.hpp`. Verify on
hardware with a log line before building on it.

**The game version is fixed at 2.31.** The game has not been updated in over a year; hard-coded RVAs are the design.

## Known gaps

The order the host grows in: first what keeps the existing mechanisms whole and consistent, then the cells of the
mechanism x kind-of-work matrix that are empty. A consumer (a glass / refraction overhaul, a strand-hair mod, ...)
validates an item; it does not set the order.

| mechanism | compute dispatch | draw | DispatchRays |
|---|---|---|---|
| replace the shader, live variants | yes | yes (VS / PS, GS added) | raygen only (`rt_variant`) |
| scope to a node | yes | yes | yes |
| lease a register | yes | yes | yes (one per register) |
| subscription (callback after it) | yes | yes (`draw_subscribe`) | **no** |
| companion (again, with every root argument bound) | yes | yes (`draw_companion_*`) | **no** |
| raw pass after it | yes | yes (`pass_after_draw`) | **no** |
| pipeline state beyond bytecode | - | **no** | - |

0. ~~The engine-PSO map is keyed by pointer and insert-only~~ - FIXED 2026-10-10, hardware-confirmed 13:12 (`48 engine PSO(s) released by the engine` two minutes into a
   drive, 32 left mapped; no `duplicate`): the engine
   frees PSOs (`GpuApi::Destroy(ShaderRef)` -> `PSOSubCache::ShaderDestroyed`, material streaming); the host now holds
   a reference on every engine PSO it maps and drops entries the engine released every 64 frames (`pso_patch.cpp`
   `sweep_locked`; clones released one sweep later).
1. ~~**Lease sources: textures.**~~ DONE 2026-10-10, hardware-confirmed 20:25 (`LeaseTexTest` 19/19 PASS: client
   texture 16/16 = slice 1, LUT texels 2/2 = the copy incl. across a scene change, flow hits 16569 misses 0): `view_create_texture`
   - a client texture or an engine one by name (`desc.flow`, resolved per stage), a NULL of the view's dimension, and
   the owners' declared ResourceKind checked against the view at registration (buffers too: raw vs structured). A flow
   source declares nothing by itself: its lifetime comes from `node_wrap_use` on the scoped node (gap 6), its state
   stays the client's.
2. ~~**Several clients' patches on one PSO.**~~ DONE 2026-10-10, hardware-confirmed 20:38 (`ComposeTest` 5/5 PASS;
   both on: VS +909, PS +185 swaps; `5 of 5 combination(s) built, 0 failed`): VS, PS
   and an added GS patch compose on one engine PSO, a clone per combination of their variants, each switched alone
   (PSO patches (M1), "Composition").
3. ~~**PSO desc edits beyond bytecode.**~~ DONE 2026-10-10, hardware-confirmed 21:04 (`StateTest` 3/3 PASS, swaps
   +539 in both edited phases, +0 vanilla; seen on screen only far off - 5f079e10 is the BOTTOM layer of the visible
   sky, the volumetric clouds, fog and aerial composite cover most of it): `patch_state` -
   rasterizer (cull, fill, depth bias), depth, blend / write mask per target, formats, per variant; state-only
   variants (no bytecode). Not covered: stencil, sample desc, input layout, topology.
4. ~~**The graphics wing of M4.**~~ DONE 2026-10-10, hardware-confirmed 21:48 (`DrawTest` 5/5 PASS): draw subscriptions, draw companions,
   passes after a draw over the eight `GpuApi` draw entries ("The graphics wing (draws)"; `DrawTest`). Left: leases
   for draw companions.
5. **The RT wing of M4.** An RT companion: the engine's `DispatchRays` issued again (same state object and tables,
   another raygen, a leased output). The raygen record is built per dispatch in the upload ring
   (`GpuApi::DispatchRays` 0x2a406e4), so the second dispatch needs its own record.
6. Smaller: ~~`node_wrap` cannot declare resources~~ DONE 2026-10-10, hardware-confirmed 21:48: `node_wrap_use`
   (Wrapping an engine node, "Declarations"; `WrapUseTest`); subscriptions have no stats;
   resources that outlive a frame cannot be published (only `node_alloc` transients) - whether the engine's RenderFlow
   has persistent resources to hang them on is not checked; a client loading BEFORE REDGraph cannot log through the
   host until the host's Load.
7. **GPU time per host work item + runtime HLSL compile service** - BUILT and hardware-checked 2026-10-11 (ideas from
   spuddeh/cp2077-native-shaders, author's permission): [`plan-gpu-timing-and-hlsl.md`](plan-gpu-timing-and-hlsl.md).
   `[Diagnostics] GpuTiming` (companions, draw companions, passes; engine dispatches by hash and raygens not yet);
   `compile_hlsl` (cs/vs/ps_6_x; the DXC DLLs are NOT shipped - licence of a stable release unchecked; no hot reload).
