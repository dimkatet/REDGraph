# REDGraph SDK

REDGraph is a RED4ext plugin for **Cyberpunk 2077 2.31** that opens the game's renderer to other plugins. With it, a
plugin can:

* replace an engine shader with its own bytecode and switch between versions live;
* feed its own data into that shader through the engine's own descriptor tables;
* add a ray-tracing raygen variant and switch it per render-graph node;
* wrap, remove or add nodes of the engine's render graph and record D3D12 work on the engine's command lists.

REDGraph never touches files on disk and needs no ReShade.

Your plugin is a **client**. It includes one header, `include/rg_host.h` (plain C), registers what it wants while the
game starts, and REDGraph does the rest. Several clients can run at once. REDGraph arbitrates between them and shows
who registered what.

This SDK contains:

| path | what |
|---|---|
| `include/rg_host.h` | the whole API, with each call documented where it is declared |
| `examples/rg_client_example/` | the smallest working client (wraps one node and logs), with its own `CMakeLists.txt` |
| `tools/rg_shader_cache.py` | names of every game shader, search, and **the vanilla bytecode of any shader by hash**, offline from the game's caches |
| `tools/rg_dxil.py` | disassemble / assemble + validate DXIL, the hash of a container, a blob as a C++ header |
| `tools/space0_rebind.py` | move a shader's resource bindings to other registers (for leases) |
| `tools/rt_raygen_variant.py` | build an RT library that carries a raygen variant (for `rt_variant`) |
| `reference/nodes.md`, `reference/node_shaders.csv` | every render-graph node we have seen, with its shaders, RT libraries and render modes |

The tools need Python 3.9+ (standard library only). `rg_dxil.py` also needs a
[DirectX Shader Compiler release](https://github.com/microsoft/DirectXShaderCompiler/releases); point it at the
release's `bin/x64` with `--dxc` or `RG_DXC_DIR`.

## 1. Quick start

1. Install RED4ext and REDGraph. Users of your mod install REDGraph too; list it as a requirement.
2. Build the example: `cmake -B build -A x64 examples/rg_client_example && cmake --build build --config Release`.
   CMake fetches RED4ext.SDK; pass `-DRED4EXT_SDK_DIR=<checkout>` to use your own.
3. Copy `build/Release/REDGraphExampleClient.dll` to `<game>/red4ext/plugins/REDGraphExampleClient/`.
4. Start the game and load a save. `<game>/red4ext/logs/redgraph-*.log` shows:

```
example client: host API 1 (table N B) - node_wrap(CRenderNode_VolumetricFog) = 1
host: registration CLOSED - clients: ... REDGraphExampleClient (API 1): 1 registration(s), 0 refused
example client: VolumetricFog before #600 (host wrap stats: executes 600)
```

The CET overlay has a **REDGraph** window, and its **Clients** tab lists your plugin and its registration.

## 2. The rules every client follows

**Register in your RED4ext `Load`, through `rg_host_when_ready(init, user)`.** RED4ext loads plugins alphabetically,
so the host may load before or after you:

* If the host is already loaded, `init(host, user)` runs immediately.
* If not, `init` waits in a process-wide queue and the host runs it in its own `Load`.
* It returns -1 only when REDGraph is missing or older than your header. Then `init` never runs, and your plugin
  should do nothing.

**Registration is open until the engine creates its D3D12 device** (`GpuApi::InitDevice`). Every plugin's `Load` runs
before that. After it, every `*_register` / `node_*` / `rt_variant` / `*_scope` call returns `RG_E_LATE`. REDGraph
attaches only the engine hooks that the registrations need.

**Handles and errors.**

* A handle is > 0. A negative value is an `RG_E_*` code: `ARG`, `TAKEN`, `FULL`, `UNSUPPORTED`, `LATE` or `BLOB`.
* REDGraph also logs every refusal, with the reason.
* Some refusals come later, when the engine creates the pipeline (for example, a lease whose register the vanilla
  shader reads). Check `*_stats(...).state` or the Clients tab.

**Threads.**

* Every call is thread-safe.
* Calls marked `[hot]` in the header are cheap enough to make per draw.
* Your callbacks (node / wrap functions, dispatch subscriptions, after-dispatch passes) run on the engine's
  **recording threads**, often several at once. Keep them short and lock-free.
* If a callback changes the root signature, PSO or descriptor heaps on the command list, return
  `RG_PASS_TOUCHED_STATE`, and REDGraph restores the engine's state.

**Versioning.** `rg_host` only ever grows at the end. A client built against an older header still gets the table;
`struct_size` tells you which fields exist. Check `host->struct_size` before calling a field newer than the oldest
REDGraph you support.

**Do not rely on RED4ext `Unload`.** A heavily modded game often crashes on exit, so `Unload` may never run.

**The game version is 2.5** (REDGraph 0.4.2 on; 0.4.1 is the last for 2.31; 0.5.0 adds the A/B profile API entries). REDGraph uses fixed engine addresses of
that build.

## 3. What a client can do

The header documents each call in detail. This table is the map: which call you need, and what to read next.

| you want | call | notes |
|---|---|---|
| your bytecode instead of an engine shader (VS / PS / CS) | `patch_register` (M1) | up to 4 variants, `patch_select` switches live (-1 = vanilla). Two clients on one shader: the higher `priority` wins |
| ... only inside one node | `patch_scope` (M11) | up to 4 nodes; elsewhere the engine's shader stays |
| change the engine PSO's pipeline state (cull, fill, depth, blend, write masks, formats) per variant, with or without new bytecode | `patch_state` (M1) | a variant blob `{NULL, 0}` keeps the engine's shader |
| data into your patched shader | `cbuffer_create` / `cbuffer_write`, `view_create` / `view_create_texture` / `view_set_resource`, `lease_register` (M2) | binds into a **free space0 register of the engine's own tables**, so it costs 0 root-signature DWORDs. See section 5 |
| the engine's own constants | `engine_block_find` (by the writer's name), `cbuffer_feed`, `engine_block` | the host's catalog: fog, clouds, `cb12` (the shared per-frame constants) - `docs/internals.md` "Engine constant blocks" lists it |
| an extra compute pass right after an engine dispatch, with all its bindings | `companion_register` + `companion_request` (M4a), `companion_scope` | the companion reads exactly what the engine's dispatch had bound |
| to know when an engine CS dispatches | `dispatch_subscribe` (M4c) | direct and indirect dispatches |
| raw D3D12 work after a dispatch | `pass_register` + `pass_after_dispatch` (M4b) | |
| to know when an engine VS / PS draws, and what it drew | `draw_subscribe` (M4, graphics) | every GpuApi draw entry; a batch of instances reports each draw |
| the same draw again with another VS / PS and / or pipeline state | `draw_companion_register` + `draw_companion_request` (from a draw subscription), `draw_companion_scope` | every root argument, render target and vertex buffer stays the engine's; no leases yet |
| raw D3D12 work after a draw | `pass_register` + `pass_after_draw` | leave render targets, viewports, vertex / index buffers as you found them: the next draw of the batch does not re-set them |
| a whole RT library replaced | `rtlib_register` (M3), `rtlib_stats` | baked into the engine's state object at creation, not switchable |
| an RT raygen variant, switchable live, per node | `rtlib_register` + `rt_variant` + `rt_variant_set` (M12) | the engine's library stays byte-identical. See section 6 |
| an engine texture by name | `flow_register` + `flow_texture` (M5), `flow_transition` | valid only inside a node's recording |
| your own node in the render graph | `node_register` (M6), `node_use` / `node_alloc` (M7) | the engine schedules it; transients are aliased by the engine; a transient's name is `"<yourmod>/<name>"` |
| another mod's transient | `flow_register("<theirmod>/<name>", 0)` + `node_use` | the only way clients share a resource - see section 8 |
| code before / after an engine node | `node_wrap` (M8), `node_wrap_use` | any number of wraps per node; `node_wrap_use` declares a resource on the wrapped node (not on a group) |
| an engine node's work switched off (or replaced) | `node_remove` + `node_remove_set` (M9), plus a wrap for a replacement | **one** removal per node class across all clients |
| what your feature costs, measured | `ab_switch` (one per feature), `prof_active` + `prof_add` (your own engine hooks' CPU) | `[Diagnostics] ABProfile`, `Game.RG_AB` - section 9; check `struct_size` first (new in 0.5.0) |
| ... and its outputs cleared instead of left stale | `node_remove_clear` | the node's own textures, or a named one; the value is yours. See section 7 |
| HLSL compiled into a blob at load | `compile_hlsl` | `cs/vs/ps_6_x`; needs `dxcompiler.dll` + `dxil.dll` next to REDGraph.dll - **not shipped** with REDGraph, else `RG_E_UNAVAILABLE` |
| shader-visible descriptors for your own pass | `gpu_descriptors` (M10) | from the engine's per-frame ring: allocate per use, never keep |
| the engine's `ID3D12Device*` | `device`, `device_ready` | create your objects on THIS device (it is ReShade's proxy when ReShade is loaded) |

## 4. Finding your target

**Which node?** Look it up in [`reference/nodes.md`](reference/nodes.md) (render modes, engine shaders, RT libraries),
or watch it live:

* Set `[Diagnostics] RenderGraphDebug = true` in `red4ext/plugins/REDGraph/REDGraph.ini` and restart.
* The **Render Graph (debug)** window lists every node of the running graph with its shaders.
* From that window you can switch any node off, or skip any shader, live. It is the fastest way to find which node
  draws what.

Node names:

* A node name is its class name (`CRenderNode_VolumetricFog`). A **group** name (`Lighting`) also works for wraps
  and scopes.
* One class can appear in many groups. `CRenderNode_RenderElements` draws geometry in a dozen of them, and a wrap
  by class runs around every instance.

**Which shader?** Every shader is named by the `HASH` chunk digest of its DXBC container: 32 hex digits, also shown in
REDGraph's log and windows.

```
python tools/rg_shader_cache.py names                     # once: shader_names/*.csv (~20 s)
python tools/rg_shader_cache.py find volumetricClouds     # by engine name, or by hash prefix
python tools/rg_shader_cache.py extract 53c455b6          # 53c455b6....dxil = the shipped bytecode
python tools/rg_dxil.py dis 53c455b6....dxil 53c455b6.ll  # text DXIL to edit
```

Notes:

* `extract` writes exactly the bytes the engine creates its pipeline from. For an RT library it also prints the
  export names (raygens, miss and hit shaders).
* Material shaders (world geometry) are named by their top material and pass.

**Which render mode?** Ray-traced nodes run only in RT or path tracing (PT), and some raster nodes do not run in PT.
`nodes.md` lists the modes each node was seen in.

## 5. Workflow: patch an engine shader

1. **Extract and disassemble** the shader (section 4).
2. **Edit the `.ll`.** Edit the vanilla shader's own DXIL rather than recompiling HLSL: the result keeps the engine's
   resource layout and signature exactly. (Your OWN shaders - a companion, a pass - can be HLSL compiled at load with
   `compile_hlsl` and registered like any blob. The engine's root signatures are global and the DXIL's own one is never
   read: declare the engine's registers / free space0 registers, nothing else. The compiler DLLs are the user's to
   install for now - check `RG_E_UNAVAILABLE` and fall back to a precompiled blob.)
3. **Your data, if any: lease a free register.** Your edit reads a buffer at a register the vanilla shader does not
   use: space0, CBV `b0-b13`, UAV `u0-u18`, SRV `t0-t126` for VS / PS but only **`t0-t92` for CS and RT** - what
   the engine's tables cover (the log's `native table: CS covers ...` line). Outside it the patch stays vanilla and a
   companion's PSO does not even build.
   * If you wrote the edit against other registers (another space, say), `space0_rebind.py in.ll out.ll b0/space6=b3`
     moves the binding.
   * REDGraph grants the lease only when all of these hold:
     * every variant reads that register, and declares it as what the view is (a raw view for a
       `ByteAddressBuffer`, a structured one for a `StructuredBuffer`, a `RG_TEX_2D_ARRAY` view for a
       `Texture2DArray` ...);
     * the vanilla shader does **not** read it (checked at the first PSO);
     * the engine's root signature covers it (checked at the first bind).
   * Otherwise the patch stays vanilla, and the log says why.
   * A texture: `view_create_texture(kind, &desc, ring)`. With `desc.flow = 0` it is yours (`view_set_resource(view,
     texture, 0)`; its states are yours). With `desc.flow` = a `flow_register` handle it is the ENGINE's texture by
     name, resolved at every draw / dispatch: where it is not alive the patch stays vanilla for that draw. Scope the
     patch to nodes where it lives - or make it live there: `node_wrap` that node and declare the texture on the wrap
     with `node_wrap_use` - and make sure it is in a readable state there (`flow_transition` in the wrap's `before`).
4. **Assemble.** `python tools/rg_dxil.py asm edited.ll patch.dxil` assembles and validates the container, and writes
   its checksum. Without the checksum, D3D12 silently ignores the shader.
5. **Embed.** `python tools/rg_dxil.py header patch.dxil patch.hpp --symbol MyPatch` creates the header (or load the
   bytes from a file next to your DLL).
6. **Register** in `init`:

```c
static const rg_blob blob = {blobs::kMyPatchBytes, blobs::kMyPatchSize};
rg_patch_desc d = {0};
d.name = "mymod: clouds";
d.stage = RG_STAGE_CS;
memcpy(d.hash, ENGINE_HASH_53c455b6, 16);   /* the ENGINE shader's hash, not your blob's */
d.variants = &blob; d.variant_count = 1;
d.initial_variant = 0;                       /* -1 = vanilla until patch_select */
rg_handle p = host->patch_register(&d);
host->patch_scope(p, "CRenderNode_RenderVolumetricCloudsLighting");   /* optional */

rg_handle cb = host->cbuffer_create(256, 3, NULL);   /* NOT READY until the first write: the patch waits */
rg_lease_desc l = {0};
l.name = "mymod: params"; l.stage = RG_STAGE_CS; l.kind = RG_KIND_CBV; l.reg = 9;
l.patches = &p; l.patch_count = 1; l.cbuffer = cb;
host->lease_register(&l);
/* any time later: host->cbuffer_write(cb, 0, &params, sizeof params); host->patch_select(p, -1 / 0); */
```

7. **Check the log.** Look for the clone built, the lease granted, `swaps` growing and no `out_of_scope` (when you
   scoped the patch). `patch_stats` gives the same numbers in code.

## 6. Workflow: an RT raygen variant

REDGraph adds your library to the engine's ray-tracing state object as a second library. That library exports only
your variant raygens; the engine's library stays byte-identical. While your variant is on, the dispatches you scope it
to get its shader identifier. The miss and hit tables, the root signature and every other export stay the engine's.

```
python tools/rg_shader_cache.py extract 318cee44            # prints: rgs_shadow_main rgs_shadow_transparent_main ...
python tools/rg_dxil.py dis 318cee44....dxil clean.ll
# edit rgs_shadow_main in a copy -> edited.ll, then:
python tools/rt_raygen_variant.py edited.ll variant.ll --clone rgs_shadow_main --as mymod_rgs_shadow \
       --vanilla clean.ll --only --prefix mymod_
python tools/rg_dxil.py asm variant.ll variant.dxil
```

```c
rg_handle lib = host->rtlib_register("mymod: sun shadow", HASH_318cee44, (rg_blob){bytes, size}, 0);
rg_handle v = host->rt_variant(lib, "rgs_shadow_main", "mymod_rgs_shadow", "CRenderNode_RenderRayTracedGlobalShadow");
host->rt_variant_set(v, 1);   /* live, any thread; 0 = the engine's raygen */
```

* **Prefix your export names with your mod's tag.** Two clients may add variants to the same engine library, but an
  export name is one function per state object: a name another client already uses gets `RG_E_TAKEN`.
* When several clients' variants of one raygen are on in the same dispatch, the higher priority runs.
* An RT lease (data for your raygen) stages at every `DispatchRays`, so there is **one RT lease per register** in the
  process. A second one gets `RG_E_TAKEN`.
* **First start after a new blob:** the driver compiles the enlarged state objects once, so RT can start tens of
  seconds late on a cold shader cache (about 1 s when warm).

## 7. Workflow: the render graph

* **Wrap** (`node_wrap`): `before` / `after` run on the command list the engine bound for that node, in the real run
  only. `flow_texture` and `flow_transition` work inside them. A resource the node itself does not use - a transient
  whose last engine reader ran earlier, another mod's published one: `node_wrap_use(wrap, flow, access)`, and the
  engine keeps it alive through the node as for a node of yours. Not on a group target (declare on a member).
* **Remove** (`node_remove`): the node stays in the graph and its resource declarations are replayed, so lifetimes and
  barriers stay intact. Only its GPU work is gone. Add a wrap on the same node to put your own pass in its place.
  Groups and job-builder nodes cannot be removed (`rg_remove_stats.refused`).
  What the node would have written keeps old / undefined contents - a transient may flicker. `node_remove_clear(remove,
  name_hash, value)` clears it in the node's place: `name_hash` 0 = only the textures the node ALLOCATES itself; a
  SHARED target it only writes into (scene colour, which e.g. the clouds composite onto) only by its name - clearing
  that wipes the frame. The neutral value is yours to choose (motion vectors 0, a mask 0 or 1, depth per the engine's
  Z). Render / depth targets only; a UAV-only texture is skipped (logged).
* **Your own node** (`node_register(name, after, fn)`): it is inserted after a group member, in the same group. Declare
  what it reads and writes with `node_use` and allocate transients with `node_alloc`, before the window closes.
* **Your own pipeline:**
  1. Create it on `device()` in `device_ready`.
  2. In your fn, call `gpu_descriptors(n, &d)` **first** (it binds the engine's heaps).
  3. Write your views at `d.cpu`, set your root signature and `d.gpu`, and draw or dispatch.
  4. Return `RG_PASS_TOUCHED_STATE`.

## 8. Several mods at once

| shared thing | rule |
|---|---|
| one engine shader patched by two clients | the higher `priority` wins (ties: the first registered); the other is refused, with the reason |
| one draw whose VS one client patches and PS (or added GS) another | allowed: both apply, each switches on its own (a clone per combination of their variants) |
| one register leased by shaders of two clients | allowed: each lease stages only for its own shaders |
| one register leased for RT by two clients | the second gets `RG_E_TAKEN` |
| one node removed by two clients | the second gets `RG_E_TAKEN` |
| one node wrapped by several clients | all run: every `before`, the node, every `after`, in registration order |
| one RT library with variants from several clients | all added; an export name taken by another client gets `RG_E_TAKEN` |
| a handle another client made | refused with `RG_E_FOREIGN` (the `*_stats` calls read any) - handles work only in the DLL that made them |
| a resource two clients use | only through the render graph, by name: one publishes a transient with `node_alloc(node, "<mod>/<name>", ...)`, the other reads it with `flow_register("<mod>/<name>", 0)` + `node_use` in its own node. The engine places the barriers and the order (the reader's node must run after the publisher's, in frame order). One publisher per name (`RG_E_TAKEN`) |
| one node name registered by two clients | the second gets `RG_E_TAKEN` (a node's name is its class name in the graph) |

The REDGraph window's **Clients** tab shows, per plugin:

* each registration with its state and live counters;
* refusals with the reason;
* **Shared targets** (one shader / node / register / raygen used by several clients);
* **Registers** (every leased register and who holds it).

The log has the same information in one line at startup: `registration CLOSED - clients: ...`.

## 9. Debugging

* **Logs:** `<game>/red4ext/logs/redgraph-*.log`. It covers every clone, swap, lease, RT bake and variant, refusals
  with their reasons, and your `host->log` lines. Only the latest log is kept.
* **`REDGraph.ini`** (`red4ext/plugins/REDGraph/`), section `[Diagnostics]`. Every key takes effect at the next start.
  * `VerboseLog`: per-object detail (off by default). You can also set it from code with `log_set_verbose`.
  * `RenderGraphDump`: writes every distinct render graph, plus a census (node → shaders, RT dispatches, resources), to
    `red4ext/plugins/REDGraph/render-graph/`.
  * `RenderGraphDebug`: the live graph window (section 4).
  * `Dred`: GPU page-fault address and breadcrumbs in the crash log, for a GPU hang or device removal.
  * `GpuTiming` (live): GPU time of your companions, draw companions and passes (with a debug event each, for Nsight /
    RenderDoc), of DispatchRays with your RT libraries, and - when on at the START - of every node of the engine's render
    graph (groups and job-builder nodes not yet). Switch your patch off and on and read its node's time: that is what
    it costs. Shown under **Host -> GPU time** and in the status report.
  * `BlasCensus`: is your mesh in the ray-traced BVH (static / updatable / absent), and what the engine's BVH pool
    writers did - every 10 s as `[blas]`. Name what to watch from your init with `host->census_watch(RG_STAGE_VS, hash,
    name)` (its draws joined to the BLAS) or `RG_STAGE_CS` (dispatches; for the `m_computeSkinning_*` pool writers);
    it returns `RG_E_UNAVAILABLE` and costs nothing while the census is off. NVIDIA only.
  * `FrameTimer` (live): the frame time every 10 s as `[frame]` - switch your feature off and on and compare.
  * `ABProfile`: the A/B profile - register a switch per feature (`host->ab_switch("<Mod>.<Feature>", set, get,
    user)`: `set` is called from the profiler's own thread - apply live, do not save), then from the CET console
    `Game.RG_AB("<Mod>.<Feature>", 4, 1.5, 10)` (block s, settle s, pairs; `*` = every switch; `"stop"` aborts) and
    `Game.RG_ABState()`. The report (`[ab]` in the log): the frame, the GPU span, your DispatchRays, companions and
    passes, the CPU per engine node, the host's and every client callback's - B (on) minus A (off), with a 95 %
    interval. Time your own engine hooks with `prof_add` while `prof_active()`. `docs/internals.md` "A/B profile".
* **`[Dev]` (developers):** `ShaderHotReload` (a folder of new shader bytes for your registered patches, live) and
  `Readback` (`Game.RG_Readback(...)`: an engine constant block, a render-graph texture at a node, a VS's outputs + the
  BVH copies of its meshes, into files) - `docs/internals.md` "Shader hot reload (dev)", "GPU readback (dev)".
* **REDGraph window → Host:**
  * **Status into the log** writes REDGraph's counters and settings.
  * **Profile** measures REDGraph's own CPU time per hook point. About 0.3 % of a frame is normal.
* **A patch that never swaps:** check, in this order:
  * the hash (the engine's shader, all 32 digits);
  * the render mode (is the node running at all?);
  * the scope (`out_of_scope` growing);
  * a refused lease;
  * a higher-priority client on the same shader.

## 10. Limits

* Game **2.31** only.
* An RT patch takes effect when the engine creates its state objects (startup, mode switches). A variant switches live;
  a whole-library swap (`rtlib_register` without variants) does not.
* Leases are space0 registers of the engine's tables: the count is limited, and each depends on the stage.
* Up to 4 variants per patch and 4 scope nodes per patch.
* Up to 256 patches, 32 leases (8 per patch / companion, 16 owners per lease), and a lease reaches up to 64 different
  engine shaders (one ShaderRef each - the log says when a lease is full).
* The engine's texture formats in `rg_flow_desc` are its own enum, not DXGI. Only what the header documents is
  decoded.
