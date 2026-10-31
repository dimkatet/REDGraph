# Plan: access to the engine's mesh render proxies (`CRenderProxy_Mesh`)

Status 2026-10-31: **BUILT and RUN** (`src/core/mesh_proxy.cpp`; `rg_host::mesh_proxy_subscribe / _chunks / _hide /
_frame`, appended to the table). First client: Cyberpunk Hair Overhaul (`docs/strand-hair.md` 14 "The vanilla hair
hidden per instance"): its hair cards hidden per instance in photo mode and in the mirror (a second view), one mask
write, no flicker; the proxy IS collected every frame (CollectElements per view per frame - an early doubt, "2470
collections in 70 s", was first person, where the player's hair is not in the main view). A mask write lands at the
next frame's collection. Not committed. Not done: SDK guide / CHANGELOG entries, the self-test, the TLAS exclusion.
The sections below are the RE log that led here.

## Why

A client that draws its own geometry IN PLACE of an engine mesh (CHO: strands instead of the hair cards) has to take
the engine's mesh out of the scene. Today it can only patch a SHADER (CHO: the hair VS with depth NEVER) - every
mesh drawn with that shader goes, not only the ones the client replaces (the eyelashes and the fur on clothes share
the hair material). Ruled out at the draw (user 2026-10-31: hacks): a per-thread patch variant (the lashes' batch has
the same PSO, GpuApi does not rebind an unchanged PSO - the clone leaks into the next batch); collapsing the instance
transform inside `DrawSkinnedMeshInstances`. The engine has its own switch for this - "render this chunk in the
scene / in the shadows" - and decides it per proxy at the collection; the host should expose that.

A second gain: the client learns its instances from the proxy (transform, skinning data, material) instead of
snooping draws (CHO marks hair instances from a hair VS draw subscription inside `Hair_Opaque`).

## What the engine has (2.5, names from `cloud-shadow-addon/_artifacts/game-new/symbols`, read with `tools/exe_re.py`)

* `enum EMeshChunkFlags : u16 { MCF_RenderInScene = 1, MCF_RenderInShadows = 2, MCF_IsTwoSided = 4,
  MCF_IsRayTracedEmissive = 8, MCF_IsPrefabProxy = 64, MCF_ConsoleLOD0 = 512, MCF_SkipDynamicDecalGeneration = 1024 }`
  - the mesh's `renderChunkInfos[].renderMask`.
* `CRenderProxy_Mesh::SetupRenderMask(const RenderProxyMeshComponentInitData&)` 0x1403dbfbc: proxy `+0xe8` (u16) = OR
  of every chunk's mask over the LOD groups (+ the init data's `+0x140`, `+0x149` bit 0 inverted x 0x80), more bits
  into `+0xea`, `+0x9c`, `+0xf1`. A per-PROXY aggregate, not per chunk.
* `GetLodGroups` 0x1404a085c: `*(proxy + 0xd8) + 0x38` - the LOD groups live in the SHARED `CRenderMesh` (proxy
  `+0xd8`), 24 B per group (`+0` chunk array, `+0xc` count), 24 B per chunk (`+0x10` renderMask u16, `+0x12` material
  vertex factory, `+8` flags). So the per-chunk masks are per MESH, shared by every instance - writing them would hide
  the chunk on every character wearing that mesh (one without strands: bald).
* `CollectContext` ctor 0x14032f078 `(RenderStageCollector&, CRenderCollector&, SRenderStageCluster, EMeshChunkFlags,
  u32, ERenderDissolveContext, float, const RenderProxyCustomData_Skinning*, const RenderProxyCustomData_MiscMeshParams*,
  bool)`: the requested mask at `ctx + 0x18` (u8), skinning custom data at `ctx + 0x38`.
* Collection: `CollectElements` 0x140330320 (virtual, the scene) / `CollectLocalShadowElements` 0x14032f2a0 /
  `CollectCascadeShadowElements` 0x14032f5ac / `CollectDistantShadowElements` 0x14049d330 ->
  `CollectAllLodModeInstances` 0x1403309bc -> (helper 0x140330e88) -> `CollectChunkWithMaterial` 0x1403313bc
  `(this, u32, const ChunkToCollect&, const CollectContext&, ...)` - the per-chunk step, 2 call sites.
* The proxy <-> component link: `GetSkinningData` 0x14049d180 = the proxy's custom data `Skinning` (`+0x7c` bit 3 has
  it) `+ 0x10`: a `TRenderPtr<CRenderSkinningData>` - most likely the same object as the component's
  `ent::SkinnedMeshComponent::CreateSkinningBuffer` output that CHO already tracks (to check).
* Lifetime: ctor 0x1403dad50 `(const RenderProxyInitInfo&)`, `AttachToScene` 0x14049be10, `DetachFromScene`
  0x14049b97c, dtor 0x14049b3e8. Also `SetMeshProxyFlag(ERenderMeshProxyFlags, bool)` 0x1410baa30 (flags not mapped).
* RT, later: `CRenderAccelerationStructure::UpdateSkinnedInstances(ctx, ArraySpan<CRenderProxy_Mesh*>)` 0x142e92e9c -
  where a proxy's skinned BLAS instance enters the TLAS (CHO: the vanilla hair out of the BVH once the strands are in
  it, LSS).

## Shape (draft)

1. **Proxy events**: a client registers a filter (mesh path hash list); the host calls it at attach / detach with an
   opaque proxy handle, the mesh hash, the skinning data object, chunk count and per-chunk material. Any thread.
2. **Per-proxy chunk mask**: `proxy_mask(handle, chunk_bits, and_mask)` - the host ANDs `and_mask` into what the
   collection asks for those chunks of that proxy (CHO: `~MCF_RenderInScene` on the hair.mt chunks; the shadows,
   the cap chunk and every other proxy as they are). Hook point to pick: `CollectChunkWithMaterial` (per chunk; the
   context's mask at `+0x18` tells scene from shadow) - find where the chunk mask is tested against the context's
   (in the helper 0x140330e88 or inside `CollectChunkWithMaterial`), and whether returning early there is clean
   (instance counts, dissolve, dynamic decals).
3. **Per-frame data**: the proxy's transform (`RelinkTransformOnly`, `RenderProxyTransform`), skinning data (this
   frame's palette offset in the bone buffer), material constants per chunk.
4. Later: exclude a proxy from the TLAS (`UpdateSkinnedInstances`).

**The per-instance decision is per PROXY (user 2026-10-31): the hook point is `CollectElements`.** It is the
proxy's own SCENE collection (`this` = the proxy; virtual - no direct caller, the vtable `??_7CRenderProxy_Mesh@@6B@`
0x143059a88); the shadows come through the separate `Collect*ShadowElements`. Its calls: custom data / skinning data,
the dissolve value, a visibility bit set, then ONE `CollectAllLodModeInstances` at the end (0x14033070f). So
"this proxy not in the scene, still in the shadows" = not calling the original for it - or, per chunk, filtering
inside its `CollectAllLodModeInstances`. Open: (a) do the hair meshes have non-hair chunks (a scalp cap) - then the
whole-proxy skip is too much and the per-chunk filter is needed; (b) what skipping leaves stale (the dissolve state,
a per-frame bookkeeping bit); (c) a proxy not collected appends no instance (`AddChunkInstancesToBatch`) - CHO loses
its palette offset from the batcher: point 3 (the data from the proxy) becomes REQUIRED, not optional.

**(a) answered (WolvenKit `mesh_info`, 2026-10-31): the hair meshes DO have non-hair chunks - the per-chunk filter is
needed.** hh_063 messy_bob: 4 chunks (one per LOD), all `*_cards` (hair.mt). hh_051 judy: 3 submeshes per LOD -
`hair_cards`, `hair_cards2` (both `base\materials\hair.mt`) and `hair_cap` = `mesh_decal_gradientmap_recolor.mt` (the
scalp decal: hairline / stubble on the skin, 830 triangles). hh_083 ponytail_base, hh_140 short_afro: `*_cards` +
`*_cap` / `*_afro_cap`, 2 per LOD. The cap is drawn by another VS, so today's hair-VS hide keeps it - and it should
stay (the scalp under sparse strands). Also: the `hologram` / `special_hologram*` appearances put ONE non-hair
material on every chunk - a per-chunk "is it hair.mt" filter keeps those vanilla by itself. So: the proxy is chosen
in `CollectElements` (this = the proxy, the scene pass), the CHUNK in its `CollectAllLodModeInstances` /
`CollectChunkWithMaterial` (the material per chunk, the context's mask `+0x18` = scene, not shadow). The client
gives the chunks (or the host gives the client each chunk's material template and the client picks).

**FOUND 2026-10-31: the engine already has a PER-PROXY render mask per chunk - no hook on the collection needed.**
The chunk loop of `CollectAllLodModeInstances` (its helper 0x140330e88, loop at 0x1403311a1 and a second one at
0x1403312dc for the alternate LOD mode):
```
chunk = *(proxy + 0x118) + 48 * (byte[proxy + 0x122 + 2 * lod] + i)    // i < byte[proxy + 0x123 + 2 * lod]
if (!(u16[chunk + 0x28] & u16[ctx + 0x1a])) skip                       // the chunk's renderMask vs the requested
if (!bt(u64[proxy + 0xe0], byte[chunk + 0x22])) skip                   // the component's chunkMask
CollectChunkWithMaterial(proxy, ..., &chunk, ctx, ...)
```
* `proxy + 0x118` = the proxy's `MaterialLodGroups` (ctor: `MaterialLodGroups::Apply(this + 0x118, ...)` 0x1404a0520
  after `CreateLodGroups`). `Apply` CLEARS it and allocates a NEW array per proxy (`NewArrayHelper`), per chunk:
  `+0` RenderMaterialInfo (the proxy's own material for that chunk - the appearance's), `+0x22` u16 chunk index,
  `+0x24` u32, `+0x28` u16 renderMask (a COPY from the mesh). So `+0x28` is per INSTANCE.
* `ctx + 0x1a` = the ctor's `EMeshChunkFlags` argument (the ctor stores the stage cluster at `+0x18`, the mask at
  `+0x1a`) - `MCF_RenderInScene` in the scene collection, `MCF_RenderInShadows` in the shadow ones.
* `proxy + 0xe0` = `RenderProxyInitInfo + 0x138` = the component's `chunkMask` (u64, all ones by default): per
  proxy too, but it drops the chunk from EVERY pass (shadows as well) - not what CHO wants.

So "this instance's hair chunks not in the scene, still in the shadows" = clear bit 0 of `u16[chunk + 0x28]` in
that proxy's MaterialLodGroups for the hair.mt chunks - the engine's own data, read by the engine's own test. The
host's part shrinks to: find the proxy (events / by skinning object), give the client each chunk's material, write
the mask bits, and RE-APPLY after every `MaterialLodGroups::Apply` on that proxy (an appearance / material change
rebuilds the array - a hook on Apply's return, not hot). Open: the proxy aggregate `+0xe8` (SetupRenderMask, the OR
of the chunk masks) stays with the scene bit - check what reads it (an early accept only, or a list the proxy is put
in); the threads that write `+0x118` (Apply) vs. read it (collection) - our write is a u16 store into a live entry,
done at the same point Apply finishes; the ctor compares each last-LOD chunk material's `+0x130` with the CName hash
0xf8199c5f69260fc9 (sets `proxy + 0x156`) - not "hair" / "hair.mt" / "base\materials\hair.mt", unknown.

**Who writes the chunk table, who reads the aggregate (2026-10-31):**
* `MaterialLodGroups::Apply` has ONE caller: the proxy ctor (itself called by `CRenderInterface::CreateProxy`
  0x1403d9740 and `CRenderProxy_MorphTargetMesh`'s ctor). An appearance change makes a NEW proxy - set once at
  creation, no rebuild to chase.
* The second writer: an unnamed function 0x1422819d0 (symbol "InvalidateShadowCache+0xa00" is a wrong neighbour
  name) - on the first material parameter override it copies `proxy + 0x118` (`MaterialLodGroups::operator=`
  0x140aef5e0) into a custom data backup (custom data id at 0x1422819ec, `+0x18` = taken), then
  `ApplyMaterialParameterOverride` 0x140578e44 swaps the chunks' MATERIALS (writes `RenderMaterialInfo`, not `+0x28`).
  A restore from that backup would bring the old masks back - the host re-asserts its masks (cheap: a few proxies).
* `+0xe8` (the aggregate): `CollectElements` returns at once when bit 0 (scene) is clear (0x140330359); the distant
  shadows test bit 7. The host leaves it alone: a proxy whose every scene chunk is masked still runs the chunk loop
  and collects nothing - correct, and nothing else depends on it.
* Who creates the proxy for a component: `ent::SkinnedMeshComponent::SetupRenderProxy(RenderProxyMeshComponentInitData&)`
  0x1404b2d80, `ent::MeshComponent::SetupRenderProxyInitInfo` 0x140715400 / `SetupRenderProxy` 0x1407156d4,
  `GarmentSkinnedMeshComponent::SetupRenderProxy` 0x1405602f0 (and the world node instances for static meshes).

## RUN 2026-10-31 (the proxy census, `[Diagnostics] MeshProxyCensus`, 3 runs) - the chain HOLDS

Module `src/core/mesh_proxy_census.cpp` (read only): hooks both `SetupRenderProxy` and the proxy ctor; logs every
proxy a SKINNED component set up, once per mesh, with its chunk table. Run 3 (a save in the city, 400 meshes logged,
7 of them library hairstyles), e.g. hh_054_wa__michiko:
```
[proxy] mesh b887c30e3012880b (via init) proxy ... CRenderMesh ... skinning 0 (custom data bits 00) | chunkMask
ffffffffffffffff agg 0105/0e01 lods 4 || lod 0 [0+2]: #0 mask 5 m0 hair; #1 mask 1 m0 mesh_decal_gradientmap_recolor;
|| lod 1 [2+2]: #2 mask 5 m0 hair; #3 mask 1 m0 mesh_decal_gradientmap_recolor; || lod 2 ... || lod 3 ...
```
What it settles:
* **mesh -> proxy**: by the INIT DATA pointer (`RenderProxyInitInfo + 0` == the `initData` the component's
  `SetupRenderProxy` filled; synchronous, the init data is on the caller's stack). Matching by `CRenderMesh` (init
  `+0xa0`) is NOT enough: run 2 matched 82 by init data, 0 by CRenderMesh. The unmatched skinned proxies (43) come in
  bursts of 20 from one stack init data - `DistantCrowdController::SpawnCrowdMesh` (the distant crowd), not components.
  Every constructor of the init data (`RenderProxyMeshComponentInitData` ctor 0x14020a2d8, 25 callers): the
  components' `CreateRenderingProxy` (skinned, mesh, garment, morph target skinned, physical destruction, skinned
  cloth), the world node instances, foliage, the distant crowd, decals, the world map preview.
* **the per-proxy chunk table is real and readable**: per LOD `[start + count]` at `proxy + 0x122 / 0x123`, LOD count
  `proxy + 0xf4`; chunk `+0x22` = the mesh chunk index (runs 0..n over all LODs), `+0x28` = the render mask.
* **the chunk material's name (`CRenderMaterial + 0x130`) is the TEMPLATE's short name**: `hair` for the cards,
  `mesh_decal_gradientmap_recolor` for the cap - not a path. A client picks hair chunks by the name `hair` (CName
  FNV-1a 64 of "hair" = 0x2e29a4cc7409d939). `[chunk + 8]` is not a material (its +0x130 is no name).
* **the masks**: hair chunks `5` (RenderInScene | IsTwoSided) at LOD 0 (some LODs `1`), NO RenderInShadows - the
  hair cards cast no shadow of their own; caps `1`, some `3` (scene + shadows: hh_062). So clearing bit 0 on the
  hair chunks takes them out of EVERY pass they are in. Agg `+0xe8` = 0105 / 0107.
* **the skinning data is NOT there at the ctor** (`+0x7c` bits 00, init `+0xc0` = 0 for every hairstyle): it is
  attached later. So a client joins its own data by the COMPONENT (the setup's `this`), not by the skinning object -
  CHO tracks components already (`CreateSkinningBuffer(comp)`). The API's CREATED event carries the component.
* Cost: 7.4k setups and 58k proxies in a save load, the census on - no visible hitch.

## API (draft, for rg_host.h)

```c
#define RG_MCF_RENDER_IN_SCENE   1u  /* EMeshChunkFlags */
#define RG_MCF_RENDER_IN_SHADOWS 2u
enum rg_mesh_proxy_event { RG_MESH_PROXY_CREATED = 0, RG_MESH_PROXY_DESTROYED = 1 };
typedef struct rg_mesh_proxy_info {
  uint32_t struct_size;
  rg_handle proxy;          /* valid from CREATED until DESTROYED (the host's handle, never the raw pointer) */
  uint64_t mesh_hash;       /* FNV-1a 64 of the .mesh path */
  const void* component;    /* the entity component that set the proxy up (its SetupRenderProxy `this`) - joins a
                               client's own component data (the skinning object comes later, not at creation) */
  const void* entity;       /* the component's owner entity (NULL if none) - WHOSE proxy it is: the client reads
                               its EntityID / TweakDB record / appearance / is-player itself */
  uint32_t chunk_count;     /* entries of the proxy's own chunk table, all LODs */
} rg_mesh_proxy_info;
typedef struct rg_mesh_chunk_info {
  uint16_t chunk;           /* the mesh chunk index (< 64) */
  uint8_t lod;
  uint16_t render_mask;     /* EMeshChunkFlags as the engine has it for THIS proxy now */
  uint64_t material;        /* the chunk material's CName (its template's short name: "hair",
                               "mesh_decal_gradientmap_recolor") */
} rg_mesh_chunk_info;
typedef void (*rg_mesh_proxy_fn)(void* user, uint32_t event, const rg_mesh_proxy_info* info);

/* Proxies of these meshes (path hashes) as they are created / destroyed; any engine thread. */
rg_handle mesh_proxy_subscribe(const char* name, const uint64_t* mesh_hashes, uint32_t count, rg_mesh_proxy_fn fn,
                               void* user);
int32_t mesh_proxy_chunks(rg_handle proxy, rg_mesh_chunk_info* out, uint32_t max);   /* count, or RG_E_* */
/* Remove `flags` from the chunks in `chunk_bits` (bit = mesh chunk index) of this proxy, every LOD; 0 bits = back to
   the engine's own. The host keeps it applied for the proxy's life. [hot] */
void mesh_proxy_hide(rg_handle proxy, uint64_t chunk_bits, uint32_t flags);
```
Use case (user 2026-10-31): strands only on CHOSEN characters (V, given NPCs), each with their own hairstyle's set -
the hide is per proxy, so the client picks the proxies by the entity; the set stays chosen by the mesh. To RE: the
component -> owner entity link (RED4ext `ent::IComponent`).

CHO: subscribe with the library's mesh hashes; per CREATED proxy remember (skinning -> proxy); when its strands are
drawn, `mesh_proxy_hide(proxy, <the hair.mt chunks>, RG_MCF_RENDER_IN_SCENE)`, when not (out of the 8 nearest, no
set loaded, HideVanilla off) `mesh_proxy_hide(proxy, 0, 0)`. The hair-VS patch goes. Later: the instance data
(transform, palette offset, material constants) from the proxy too, replacing the hair VS marks.

## Next RE steps

* ~~The chunk-vs-context mask test~~ - found (above).
* The mesh path hash at proxy creation - ANSWERED in part (2026-10-31): `RenderProxyInitInfo + 0` =
  `RenderProxyMeshComponentInitData*` (`+0x60` u8 kind: 1 / 0xc = a mesh proxy; `+0xa0` TRenderPtr<CRenderMesh> ->
  `proxy + 0xd8`; `+0xa8` the appearance's materials -> `MaterialLodGroups::Apply`; `+0xc0` the skinning data ->
  the proxy's `Skinning` custom data, `GetSkinningData`; `+0x138` chunkMask -> `proxy + 0xe0`; flag bytes
  `+0x145..0x149`). `CRenderMesh` has no path (no name / hash getter in the symbols). Plan: the host hooks the
  components' `SetupRenderProxy(this, RenderProxyMeshComponentInitData&)` (skinned 0x1404b2d80, plain 0x1407156d4,
  garment 0x1405602f0) and maps `initData + 0xa0` (the CRenderMesh, one per resource) -> the component's `mesh` path
  hash (RTTI, as CHO reads it); the proxy ctor hook looks up `proxy + 0xd8`. To check: that `SetupRenderProxy` fills
  `+0xa0` before the proxy is made, and whether the world node instances (static meshes) need the same.
* The chunk's material - ANSWERED in part: chunk `+0` = `CRenderMaterial*` (`+8` its parameters; the collect loop
  reads `[chunk]` / `[chunk + 8]`); `CRenderMaterial + 0x130` = its NAME, a CName (`GetDisplayableName` 0x1403dbca8 ->
  `red::Debug_GetNameString`). Most likely the template path (a CRenderMaterial is per template, the instance
  values are in the parameters) - CHECK in game: log the name of a hair chunk; hair.mt would be CName
  0xf2bd94731c40d09d (FNV-1a 64 of `base\materials\hair.mt`). The ctor's constant 0xf8199c5f69260fc9 (-> `proxy +
  0x156`) matched none of ~30 template paths tried.
* `CRenderSkinningData` from the proxy == CHO's tracked skinning object? (one log line in CHO).
* Lifetime: the destroy event - dtor 0x14049b3e8 (`??_G` 0x14049b3bc) or `DetachFromScene`; the pool reuses the memory.
* Threads: who runs the ctor (the render thread / a job) and the collection (jobs, in parallel); the u16 write
  into a live chunk entry vs. the collection reading it (a torn read of a u16 cannot happen; a stale one costs one
  frame).
* The restore path of the material override backup (if any) - else drop the re-assert.
