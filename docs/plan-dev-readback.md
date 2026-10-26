# Plan: GPU readback on request, from CET (dev)

Status 2026-10-24 (later): **sources 1, 2, 5 and 6 BUILT and hardware-checked** (`ReadbackTest` 10/10 PASS on 2.5;
`vsout:c25f7935 draws:64 blas:1` on CRT's palms: 64 draws + 521 BLAS copies in one frame) - how it works, the spec,
the two traps (the engine's graphics root signature had no ALLOW_STREAM_OUTPUT; the BLAS builds run on a COMPUTE
list): [GPU readback (dev)](internals.md#gpu-readback-dev). Built as `vsout:<hash> draws:<n> blas:1` (one request,
"pair" in this plan), not as separate `blas:vs=` / `pair:` specs. **Left:** a BLAS self-test, source 7 (`va:`). Sources 3-4 and the watch mode: not started (source 3 waits for
the user's go - RE risk). Companion to [Shader hot reload (dev)](internals.md#shader-hot-reload-dev):
edit a shader live, then look at what it wrote, without a restart and without a hand-written readback per feature.

## Why

Every readback today is bespoke code inside a client feature (CRT: fog stats, `PtLightsDump`, `SharcStats`), written
to the log, switched by an ini key. There is no "read THIS buffer now" from CET / REDBridge.

## Shape

Behind a developer switch, like hot reload (`[Dev] Readback`, name open; off = the host is the release one):

1. CET (or REDBridge `eval_lua`): `Game.RG_Readback(spec)` -> request id. `spec` is text, e.g.
   `block:<engine block name>`, `flow:<RenderFlow name> after:<node>` / `before:<node>`, later `bound:<hash> t35`.
2. The host copies the resource on the ENGINE's command list at that moment (barrier to COPY_SOURCE and back) into a
   READBACK ring; a few frames later it writes a file into a real folder (outside the MO2 VFS - the hot-reload lesson:
   reuse `[Dev] ShaderReloadDir` or a sibling key): buffer = `.bin` + `.json` (bytes, stride, format, frame, spec),
   texture = `.dds`.
3. `Game.RG_ReadbackResult(id)` -> a short summary (pending / done / failed + why, path, dims, min / max / NaN count).
4. Optional watch mode: the same spec every N frames.

## Sources, in build order

| # | source | host pieces that exist | missing | size |
|---|---|---|---|---|
| 1 | engine constant blocks (cb12, fog / cloud b6, ... by catalog name) | CPU copies, `engine_block_find` | only the CET call + file | < 1 h |
| 2 | a RenderFlow resource by name, before / after node X | `flow_register` / `flow_texture` give the `ID3D12Resource`; wraps; the graph debug module wraps every node class | a RUNTIME request: wraps register before `InitDevice` today - under the dev switch wrap every node up front (as `RenderGraphDebug` does) and resolve the name at request time; resource state for the barrier (the flow knows it) | ~1 session |
| 3 | what a shader (by hash) has bound at register tN / uN / bN at its dispatch / draw | post-dispatch / draw hooks; the staging CPU handles of the context (`kFrStaging`) | CPU descriptor -> resource: no D3D12 API. Hook the engine's bind layer above `SFrameResources::BindDescriptor` (2.5: 0x140336d3c, 11 call sites by `tools/exe_re.py callers` in CRT; names there are 2.31's, wrong on 2.5 - identify by code) to record which TextureRef / BufferRef went into which slot | 1-2 sessions, RE risk |
| 4 | RT raygen inputs | raygens get the CS tables | #3 at the DispatchRays hook | small after #3 |
| 5 | **BUILT** `vsout:<vs hash>` - the outputs of an ENGINE VS at its next draw, unmodified shader | draw companions (a PSO per engine PSO + the same draw, `pso_patch` / `passes`); every mapped PSO's desc kept under a dev switch (hot reload); the draw entries' argument maps (`engine_231.hpp` "The graphics wing") | the companion PSO with a `D3D12_STREAM_OUTPUT_DESC` built from the VS's output signature (DXIL `OSG1`), `RasterizedStream = D3D12_SO_NO_RASTERIZED_STREAM`; `SOSetTargets` (our buffer + filled-size counter) before the companion draw, `SOSetTargets(0, 0, nullptr)` after; the draw's IB / VB VAs and counts into the `.json`. SO writes per primitive vertex in index order (a triangle list: 3 per triangle) | ~1 session |
| 6 | **BUILT** (as `vsout:<hash> blas:1`) `blas:vs=<vs hash>` - the vertices / indices a BLAS is BUILT from, for the meshes drawn by that VS | the NvAPI BLAS build hook + draw <-> BLAS geometry match by index-buffer VA and count (`blas_census.cpp`) | a copy BY GPU VA: a host CS with a root SRV / UAV at the geometry's vertex / index VA (no `ID3D12Resource` needed) into our buffer, on the build's list right before the build; the geometry desc (format, stride, counts, transform VA) into the `.json` | ~1 session |
| 7 | `va:` raw GPU VA ranges a draw / dispatch has bound (root CBV / SRV / UAV) - the constants and offset buffers next to 5 / 6 | the same VA copy as 6 | the root arguments at the draw / dispatch (the context's root-parameter cache) | small after 6 |

## Next: sources 5-7 - what a VS computes vs what the BVH is built from (planned 2026-10-24)

The first consumer: CRT's palm crowns (VS `5fec49ce`) - black spots under them in RT / PT under wind; the residual
between the raster's wound vertices and the BLAS's vertices was never measured on the crown (CRT
`docs/tree-bvh-sync-plan.md` 4.15c, 4.23). The user's constraint: **no per-case debug shader patches** (the addon era's
`treewind_dump` was one instrumented VS per case and once measured the wrong shader). So the host captures the
ORIGINAL shaders' work generically, under `[Dev] Readback` (no new switch; restart to turn on, nothing per draw without
a request):

* `vsout:` = Stream Output of the unmodified engine VS (source 5) - the GPU computes, Python only compares;
* `blas:` = the BLAS build's own inputs by VA (source 6) - exactly what the BVH sees, not "the pool somewhere earlier";
* `pair:<spec> <spec>` - both in ONE frame (wind is time-dependent); the frame number in each `.json`.

Matching offline: the draw and the BLAS share the index buffer (already proven by the census), so SO vertex k of
triangle t is BLAS vertex `ib[3t + k]`; spaces: SV_Position (clip) -> world with cb12's matrices of the same frame
(`block:CRenderInterface::CalculateSharedConstants`), BLAS vertices (cell-local since CRT's `cslocal`) -> world with
the cell origin / instance transform. Out: median / p90 per mesh.

Checks first: a self-test on a known VS (the sky's - its vertex count and clip positions are known) before any
engine mesh; SO on an engine PSO has never been tried; the VA copy reads buffers in the state the BLAS build needs
(non-pixel shader resource) - log it. Limit: SO sees only what the VS OUTPUTS (no intermediate terms - those still need
a patch).

## Risks / checks

* Resource states: the engine's tracked state may not be what the copy assumes. CRT copies its own buffers on engine
  lists fine; engine resources are untested. Log the state used; the self-test must read back a KNOWN pattern
  (a client transient written by our node, as `WrapUseTest` / `LeaseTexTest` do) before trusting any engine resource.
* A readback needs its own self-test (CRT rule): a perfect copy that never executes looks like a broken shader.
* Transient RenderFlow resources live one frame: copy inside the request's node, never cache the resource.
