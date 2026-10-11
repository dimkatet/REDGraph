# Plan: GPU timing and runtime HLSL compile (from spuddeh/cp2077-native-shaders)

Written 2026-10-11. Source: https://github.com/spuddeh/cp2077-native-shaders (private, shared with us; its author said
in chat 2026-10-11 "anything you want to use, feel free"). No LICENSE in that repo: keep a credit comment in every ported
file (`Ported from spuddeh/cp2077-native-shaders, with the author's permission`) and ask the author to add a licence.

What was read: `plugin/src/Timing.cpp`, `Effects.cpp` (compiler part), `Main.cpp` (debug events), `docs/re/*`.
Nothing here is run yet. The host already has a CPU profiler (`core/prof.cpp`, `RG_Profile`); it has NO GPU timing.

## 1. GPU time per host work item (port of `Timing.cpp`)

### What the source does
- One `D3D12_QUERY_HEAP_TYPE_TIMESTAMP` heap + one READBACK buffer that stays mapped. Device and queue from
  `RED4ext::GpuApi::GetDeviceData()` (`device`, `directCommandQueue`); ticks from `GetTimestampFrequency`.
- A ring of 4 frame slots; per slot an empty begin/end pair (= the timing's own cost) and a pair per effect.
  `EndQuery` before and after the dispatch, `ResolveQueryData` into the readback, read 4 frames later - never waits.
- A pair is trusted only if `begin != 0`, `end >= begin`, `< 50 ms`. Smoothing: new = old + (reading - old) * 0.05.
- `list->BeginEvent(0, wide string, size)` around each dispatch - metadata 0 = null-terminated wide string, what PIX /
  Nsight / RenderDoc read (`Main.cpp` ~229).

### What to measure in the host (`core/passes.cpp`)
1. **Companions**: around `cl->Dispatch` in `record_companion` (and the draw wing's equivalent).
2. **Client passes** (`pass_after_dispatch`, `pass_after_draw`): around `a.fn(...)` in `run_after`.
3. **Engine dispatch / draw of a subscribed or patched PSO** (optional, for `perf-audit.md`): bracket the engine's own
   call by PSO hash, so "what does the engine's pass cost vs what our companion adds" is one number each.
4. Raygens (`rt_lib` `DispatchRays`) later, with the RT wing (gap 5).

Each item gets a stable key (client + name), a smoothed ms, and a count of rejected readings. Expose it as a host
API call (`gpu_ms(handle)` or a stats array) and a column in the debug window; log it with the existing periodic stats.

### Differences from the source - decide before coding
- The source records on ONE list (its post-process pass). The host's work is spread over many command lists, recorded on
  several threads. So: query indices must come from an atomic counter per frame slot, not a single `aIndex`.
- **Resolve**: do NOT port `EndFrame` (one resolve at the end of a frame - the host has no single list that is last).
  Recommended: `ResolveQueryData` for the 2 queries RIGHT AFTER the end timestamp, on the same list. Ordering is then
  guaranteed, the cost is a tiny copy per item. Needs the readback buffer to stay in `COPY_DEST` (it does).
- Slot reuse: the source reads a slot 4 frames later and never clears it, so a slot the GPU has not finished yet returns
  the OLD frame's valid-looking numbers. Under frame generation / deep queues 4 may not be enough. Make the ring depth
  a constant, add a per-slot frame stamp (written by the CPU at record time, checked at read), and count mismatches.
- Frame counter: use the host's own (`prof::on_graph_execute` / `ExecuteParallel` is already the frame boundary).
- Timestamps include any barrier / wait the engine put before our work. Say so in the doc; it is still the right number
  for "what does this cost me".
- Our own earlier finding: `GPU timer per-dispatch unreliable across sessions` (61f9 +-30%, sign flips). Do not promise
  absolute truth; the ablation rule in `perf-audit.md` still applies. The new thing is a number that is always there.
- Cost of timing itself: keep the empty-pair measurement; it is the honest overhead line.
- Off by default (setting in `REDGraph.ini` + CET toggle): no query heap is created until it is switched on.

### Built 2026-10-11, checked in game the same day
`src/core/gpu_timing.{hpp,cpp}`; brackets in `passes.cpp` (companion dispatch, each companion draw, pass after a
dispatch / draw), the tick in `prof.cpp` next to `pso::frame_tick`; `[Diagnostics] GpuTiming` (live), `RG_GpuTiming()`,
a block in the CET Host tab and in `RG_Status`. Decisions taken against the list above:
- ring 8 slots x 2048 pairs (a draw companion is one pair PER DRAW); per item the frame's SUM, smoothed.
- the per-slot CPU frame stamp was NOT built: it cannot see a GPU that is late. Instead a slot is ZEROED after it is
  read, so an unresolved pair reads 0 and is counted `late`, never as an older frame. A stale write landing after the
  clear needs the GPU 8 frames behind.
- an item whose frame lost a pair (budget) or had a late / rejected pair gets no reading for that frame.
- the empty pair is recorded at the first bracket of each frame, on that list.
- debug events ride the same switch (zero cost when off).
- engine dispatch by PSO hash (item 3) and raygens (item 4): not yet. No public API call yet (`gpu_ms`) - the CET
  table / status first; an API when a client asks.
- the plan's self-test needs a client-visible number - not possible without that API. Check by eye: with
  `REDGraphSelfTest` deployed, its companions / passes must appear in the table with ms > 0.

### Debug events
Add the `BeginEvent` / `EndEvent` pair (names `REDGraph: <client>/<item>`) around the same bracket. Cheap, and makes our
passes findable in a GPU capture. PIX with the plugin loaded is untried (see CLAUDE.md) - do not claim it works; Nsight /
RenderDoc are the likelier consumers.

### Tests (`selftest/`)
- A client pass with a known-heavy dispatch: ms > 0 and stable (+-30%); with the pass off: no reading, no entry.
- Timing off: zero queries recorded (check the counter).

## 2. Runtime HLSL compile as a host service (port of `Effects.cpp`, compiler part)

### What the source does
- `LoadLibraryW(dxil.dll)` by FULL path first (dxcompiler loads `dxil.dll` by bare name to sign; this makes that work
  without touching the search path), then `dxcompiler.dll`, `DxcCreateInstance` for `IDxcUtils` + `IDxcCompiler3`.
  Both DLLs are shipped NEXT to the plugin (about 20 MB), not linked in - that is how it avoids the size problem that
  made us reject runtime compile (memory `runtime-shader-compile-rejected`).
- `Compile(... -T cs_6_0 -E cs_main -O3)`; result blob; **an all-zero hash at container bytes 4..20 = unsigned =
  D3D12 rejects it** (check this and log "dxil.dll was not loaded").
- Engine shader created from bare bytecode; the engine keeps the pointer, so every compiled blob is kept alive for good
  (leak per reload is accepted).
- Hot reload by file mtime; on a compile error the old shader keeps running and the error is logged/shown.
- Line numbers: the client-level preprocessing blanks lines instead of deleting them and emits `#line 1 "<file>"`.
- The engine's root signature is global and never read from the DXIL, so the HLSL registers must match the engine's
  layout (their `docs/re/re-shader-dispatch.md`, "What our HLSL must match"; ours: the free space0 registers).

### What goes into the host
`compile_hlsl(source or path, entry, profile, defines[], include_dir) -> {bytecode, size, error text}`, plus an optional
file watch that calls the client back on change. The host owns the DLL loading and the keep-alive; the client keeps its
own prelude, `uniform` annotation parsing, settings and UI (that is the post-process layer - it stays in spuddeh's mod,
or whoever builds it).

- **v1: `cs_6_0`**, then `vs_6_0` / `ps_6_0` for the draw wing. RT libs (`lib_6_x`) last: reassembly drops RDAT
  subobjects (memory `reasm-drops-rdat-subobjects`), so check what dxc emits before promising it.
- Optional: absent DLLs = the service reports "unavailable" and nothing else changes. Never a hard dependency.
- The two DLLs are not committed; fetch them in `scripts/` from a DXC release (or the Windows SDK) and put them in the
  package. **Check redistribution terms before shipping**: DXC is open source, `dxil.dll` ships in the DXC release zip;
  confirm against that zip's licence file, not from memory.
- Hot reload: **no joined thread at unload** (memory `unload-unreliable-exit-crash`: the modded game crashes on exit).
  Prefer polling mtimes from the frame hook every ~0.5 s and compiling on a worker that is detached / never waited on.
- The result feeds the existing client entry points (a pass / companion / patch takes bytecode today); no new engine
  hook is needed. Check how a client hands bytecode in now before designing the call.

### Tests
- Compile a trivial `cs_6_0`: signed, runs in a client pass.
- A deliberately broken edit: error text returned, previous version keeps running.
- DLLs missing: `unavailable`, no crash, no log spam.

## 3. Cross-checks against our render-graph notes (cheap, do first)
Their reading vs ours; each is a grep in `docs/render-graph.md` / `render_nodes.cpp` / `render_flow.cpp`:
- `RTTempAlloc` at `0x1401ed93c` is `<3>`, not `<2>`; the template argument is the `RTUseBegin` flags word; the name
  argument is ignored (tag comes from slot + request counter). Do we rely on a name anywhere in `node_alloc`?
- `GameplayPostFX::Execute` runs twice (request + GPU pass); the request pass's view index (`ctx+0x34`) is unreliable.
- Under DLSS the view's `+0x34/+0x38` is the RENDER size; the output size is at `+0x54/+0x58`. Does any host/client
  code size a post-upscale target from the wrong pair?
- A node object is recreated on each graph rebuild: no per-node state keyed on the node pointer (`node_register`,
  `node_wrap`).
- Their gate: `Test` / `TestFilter` against the view's gameplay post-process mask keeps the HUD and menu 3D previews
  clean. Do `pass_after_*` / `node_wrap` clients need a helper for the same gate?

### Results (2026-10-11, static, `tools/exe_re.py` in CRT)
| claim | verdict | consequence for us |
|---|---|---|
| `RTTempAlloc` 0x1401ed93c is `<3>`, the argument = `RTUseBegin` flags | CONFIRMED: `??$RTTempAlloc@$02` (MSVC `$02` = 3); it calls the plain `RTTempAlloc` 0x1401ee470, then `HandleRequest` type 4 with access 3 (`[rsp+0x28]`). Siblings `$01` = `<2>` 0x140157abc, `$06` = `<7>` 0x1403785d8 | none: the host never calls `RTTempAlloc`; nothing in our docs said `<2>` |
| its name argument is ignored | CONFIRMED: plain `RTTempAlloc` never reads r8 (the name hash); the tag pointer is the global `0x142ff5b64`. `RTAlloc` 0x1401f01c4 does use it: `name ^ view << 24`, view 0 when `ctx+0x30` bit 0 | none: `node_alloc` issues an `RTAlloc`-shaped request (`render_flow::request`, `tag_of` = the same formula), so the published name is real. Never switch it to a temp alloc - the name would stop working |
| `GameplayPostFX::Execute` runs twice, the request run's view index unreliable | CONSISTENT with ours (the graph runs twice: declaration + real, `kFlowPhase`) | none: `tag_of` reads `ctx+0x34` in both runs exactly as the engine's `RTAlloc` does; `flow_texture` resolves only in the real run |
| under DLSS the view's `+0x34/+0x38` = render size, `+0x54/+0x58` = output | not checked (a VIEW object, not the node ctx - our `kNodeCtxView` 0x34 is a different struct) | none today: no host or CRT code sizes a target from a view; `node_alloc` takes absolute sizes from the client. If a host helper ever sizes post-upscale targets, read `+0x54/+0x58` and verify it first |
| a node object is recreated per graph rebuild | handled | removal instances (`render_nodes.cpp` `claim_instance`) recycle the oldest by declaration stamp and re-latch every declaration run; graph-debug removals are wanted by a rebuild-proof KEY and re-mapped to the new object (`graph_debug.cpp` ~171); our own nodes carry their index inside the object |
| `Test` / `TestFilter` keeps HUD / menu 3D previews clean | `Test(DebugFilter)` 0x14021be28: `ctx+0x30` bit 0 -> open; else the view at `ctx+0x18`, a 32 x u64 mask at `view+0x18a0` ANDed with the filter | OPEN GAP: `pass_after_*` / companions are keyed by PSO and fire in every view (inventory / photo-mode previews too); `node_wrap` gets the ctx but no helper. Candidate host call: `rg_view_test(ctx, filter)` or a `gameplay_only` flag on a pass - only when a client asks |

### Built 2026-10-11, checked in game the same day
`compile_hlsl(rg_hlsl_desc*, rg_blob*, error, size)` at the end of `rg_host` (+ `RG_E_UNAVAILABLE` / `RG_E_COMPILE`),
`src/core/hlsl.cpp`: synchronous, CPU only, so a client calls it in `init` and registers the blob as any other.
Profiles cs/vs/ps_6_x; lib refused. Self-test `[Tests] HlslTest` (good CS, broken PS with the error text, lib refused)
and, with `LeaseTexTest` on, that test's companion compiled at runtime from `lease_tex.hlsl` - its content checks are
the "it runs" proof. `build_and_deploy.ps1 -Dxc <bin/x64>` copies the two DLLs for LOCAL testing.
- NOT shipped: the local DXC is a PREVIEW (1.10.2605) under a TIME-LIMITED Microsoft licence (`LICENSE-MS.txt`:
  ends 30 days after the commercial release). Check a STABLE release zip's licence files before any package.
- hot reload NOT built: a registered patch / companion cannot take a new blob (no such host mechanism - a gap of its own).
- check: RED4ext scans `red4ext/plugins/<dir>/` for DLLs - it must not choke on `dxcompiler.dll` / `dxil.dll` there.

## 4. Not taking
- `uniform` annotations, `settings.ini` layout, the CET window, the script API: the client layer.
- Their addresses: we have cp2077-symbols for 2.31.

## The 2026-10-11 run (one batch: sections 1, 2, 6a, 6b)
`HlslTest` PASS (cs_6_0 2068 B signed; broken ps_6_0 -> RG_E_COMPILE with the text; lib_6_3 refused); `LeaseTexTest`
with its companion compiled AT RUNTIME from `lease_tex.hlsl` - 21 x PASS (the "it runs" proof); `DrawTest` 5 x PASS;
GpuTiming over 117 502 frames: late 0, over budget 0, empty pair 0.0000 ms, lease-tex companion 0.0001 ms, its copies
pass 0.0126, sky wireframe draw companion 0.0030, pass after the sky draw 0.0002; switched off / on live, no crash.
RED4ext tries `dxcompiler.dll` / `dxil.dll` in the plugin folder as plugins and goes on silently - fine there. CRT's
items were absent: its features were off in that run. The temporary switches (test ini entries, `GpuTiming = true`,
the copied DXC DLLs) were removed from the mod afterwards.

## 5. Order
1. Section 3 (reading only).
2. Section 1 for companions + client passes + debug events, then the stats/window column.
3. Section 2 for `cs_6_0`, then vs/ps.
4. Reply to the author with what landed; ask for a LICENSE.

## 6. Requests from a client author (0.1.1, 2026-10-11) - budgets, node_remove, engine data

Source: a user running a live city blackout (patch variants of material shaders reading one power cbuffer through
leases; raster works). Nothing here is built yet.

### 6a. Budgets - DO
Current limits are static array sizes picked with headroom, not a measured budget (no comment or measurement behind them):
`kMaxPatches` 96 (`pso_patch.cpp`) = `kMaxPatchHandles` 96 (`bindings.cpp`, change together); `kMaxLeases` 16 (also sizes
`Group::leases[]`, so memory grows quadratically); `kMaxLeaseOwners` 8 (`Lease::patches[]/comps[]`); `kMaxLeasesPerOwner` 4
(the user will hit it next); `g_lease_of_patch` is `int8_t` (cap 127); `Lease::rt_mask` is a `uint64_t` (RT owners cap 64).
- Before raising: is the patch list scanned linearly per `CreatePSO` / per draw? Measure. Check every narrow type above.
- Raise patches + handles together (proposal 256), leases 32, owners per lease and leases per owner with them.
- NO `REDGraph.ini` knob: the arrays are static, a setting would change nothing.
- Ask the user how many distinct hashes he patches and how many patches per lease he needs.

### 6b. node_remove: clear the output - DO, with caveats
- Opt-in flag (e.g. `RG_REMOVE_CLEAR`): a removed node leaves stale output, the screen flickers (his
  `CRenderNode_ScreenSpacePlanarReflections`).
- "Black" is wrong for some resources (depth, motion vectors, masks): clear each output with its own neutral value.
  First read the node's real outputs (render-graph dump); ask which resource goes stale.
- A client's `node_remove_set` does not show as removed in the graph debug window. `node_remove_stats` is already
  collected (`clients.cpp`); find why the window ignores it (likely it only reflects its own debug removals).

### 6c. Emissive in path tracing - NOT a host feature; the dump below is NOT ours either (user, 2026-10-11) - the client's to do
Patching RT libraries is avoided on purpose: they are a pressure point (two mods patching one library conflict; a swap
takes effect only at state-object creation; SHARC UPDATE `06ae1631` and the RTXDI emissive-triangle path may need it too;
no way to tell neon from other emissive at that point). The host does not offer "scale emission in PT".

Alternative to investigate: change DATA, not shaders. RT reads `emissiveParamBuffer[idx].x + bias` (library `d6336346`) -
a per-material table. A companion compute dispatch AFTER the node that fills it could scale `.x` for chosen `idx`:
no RT shader touched, every reader of the table is covered, composes with other mods' shader patches, and it may replace
most of his raster patches if raster emission comes from the same or a similar table. ALL UNVERIFIED
(`emissiveParamBuffer` appears nowhere in our shader sources or docs). Open:
1. Who writes the buffer (CPU upload or a GPU pass), in which node. Decides where the pass goes / whether an upload hook is needed.
2. How to get `idx` for a material (`device_diode`): CPU-side material instance -> idx map, or by value.
3. Does the RTXDI emissive-triangle path read the same table or build its own list.
4. Does the host have "an engine BUFFER by name" (it has `flow_texture` for textures)? If not, that is a framework gap.
First step: a dump - find the writing node/shader (neighbours of `d6336346`) and the buffer contents. No code before that.

### 6a / 6b built 2026-10-11, checked in game the same day (6b's window not looked at)
- 6a: the patch list is scanned linearly only at `CreatePSO` (`find_patch`); binds go through the mapped table and
  `UpdateRootParams` walks only REGISTERED groups / leases - a bigger array costs memory, not time. Raised: patches =
  patch handles 256 (one constant, `internal.hpp` `kMaxPatchCount`), leases 32, groups 32 (`FrSave::mask` is 32 bits -
  static_assert), owners per lease 16, leases per owner 8, **ShaderRefs per lease 8 -> 64** (the likely real wall for a
  blackout: every engine shader an owner is bound with is one ref, the 9th was silently never staged - now an ERROR
  once), the engine-PSO map 4096 -> 16384 slots.
- 6b: the graph debug window shows a client's `node_remove` (report field `removed` = 2, owner = that client), its
  checkbox disabled.

### 6d. Order
1. 6a (cheap, safe) after measuring the per-draw cost of a longer patch list.
2. 6b debug-window visibility, then the clear flag.
3. ~~6c dump~~ - dropped (the client's).
4. Reply to the user (text agreed 2026-10-11).
