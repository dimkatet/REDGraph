/* REDGraph - the render-host API (docs/internals.md). What a CLIENT of the host may do - patch the engine's
 * shaders, hand them data through the engine's own descriptor tables, add RT raygen variants, wrap / remove / add nodes
 * of the engine's render graph, record work on the engine's command lists. The host is its own RED4ext plugin
 * (REDGraph.dll). The SDK's README.md is the guide; `docs/...` below are the internals notes in REDGraph's source
 * repository.
 *
 * Kept as if it were public: plain C, fixed-width fields, function pointers, no STL / RED4ext / D3D12 types.
 * Versioning: rg_host_get(RG_HOST_API_VERSION) returns the table, or NULL when the host is OLDER than the header the
 * client was built with (an older client gets the table: fields are only ever APPENDED, and struct_size tells a client
 * which ones exist).
 *
 * A client plugin (another RED4ext plugin) calls rg_host_when_ready(init, user) in its own Load - in ANY load order:
 * if the host has loaded, init runs at once; if not, init waits in a process-wide rendezvous and the host runs it in
 * ITS Load. A client may also find the host directly: module RG_HOST_MODULE, export RG_HOST_EXPORT_NAME
 * (rg_host_find). rg_host_get returns NULL until the host's own Load has run.
 *
 * Lifetime: every *_register call is accepted only while registration is open - from the host's RED4ext Load until
 * the engine's GpuApi::InitDevice (every plugin's Load runs before it; no PSO or RT state object exists yet), so
 * register inside init. At the window's end the core attaches only the engine hooks the registrations need. Handles
 * are > 0; a negative return is a RG_E_* code. Every call is thread-safe; the ones marked [hot] are cheap enough per
 * draw.
 *
 * Ownership: a handle works only in the module (DLL) that made it - passed by another client it gets RG_E_FOREIGN
 * (the *_stats calls are read-only and take any handle). Clients share a RESOURCE only through the engine's render
 * graph, by name: a transient one client publishes (node_alloc, "<mod>/<name>") another reads with flow_register(name,
 * 0) + node_use - the engine then owns its lifetime, barriers and order. Patches, variants and nodes are not shared:
 * one target, one winner (priority). */
#pragma once

#include <stddef.h>
#include <stdint.h>
#if defined(_WIN32)
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define RG_HOST_API_VERSION 1u
#define RG_HOST_MODULE L"REDGraph.dll"
#define RG_HOST_EXPORT_NAME "rg_host_get"

#if defined(RG_HOST_BUILD)
#define RG_HOST_EXPORT __declspec(dllexport)
#else
#define RG_HOST_EXPORT
#endif

typedef int32_t rg_handle;

enum rg_error {
  RG_E_ARG = -1,          /* bad descriptor */
  RG_E_TAKEN = -2,        /* another client owns this shader / register with a higher or equal priority */
  RG_E_FULL = -3,         /* a host table is full */
  RG_E_UNSUPPORTED = -4,  /* not implemented by this host version */
  RG_E_LATE = -5,         /* registration is closed (after Load) */
  RG_E_BLOB = -6,         /* a blob is not a DXBC container, or does not read what it is registered for */
  RG_E_FOREIGN = -7,      /* a handle another client made (handles work only in the module that made them) */
  RG_E_UNAVAILABLE = -8,  /* a host service is not there on this install (compile_hlsl: no dxcompiler.dll) */
  RG_E_COMPILE = -9       /* compile_hlsl: the compiler said no - the error text says why */
};

/* = the engine's own stage numbering in its descriptor tables (0 VS, 1 PS, 2 CS; RT uses CS).
 * RG_STAGE_GS (patches only, no leases): the blob is a geometry shader ADDED to every engine PSO whose PS has the
 * patch's hash - nothing is replaced. Its input must match the PSO's VS output and its output the PS input, or the
 * clone fails to build and the engine PSO stays (logged). */
enum rg_stage { RG_STAGE_VS = 0, RG_STAGE_PS = 1, RG_STAGE_CS = 2, RG_STAGE_GS = 3 };
enum rg_kind { RG_KIND_CBV = 0, RG_KIND_SRV = 1, RG_KIND_UAV = 2 };
/* RG_LOG_DEBUG: written only with [Diagnostics] VerboseLog (per-object detail: inventories, every clone / bake). */
enum rg_log_level { RG_LOG_INFO = 0, RG_LOG_WARN = 1, RG_LOG_ERROR = 2, RG_LOG_DEBUG = 3 };
/* The engine's own constant blocks the host captures (GpuApi::UpdateBuffer / SetComputeShaderConstsRaw at known call
 * sites) - a cbuffer can be fed from one (rg_host::cbuffer_feed). */
/* The first four entries of the host's catalog of engine constant blocks (engine_block_find, by the engine's name of
 * the writing function) - kept for clients of API v1, always captured. Prefer engine_block_find. */
enum rg_engine_block {
  RG_ENGINE_FOG_B6 = 0,          /* CRenderVolumetricFogManager::Update - the fog CS b6 (SVolFogConstants) */
  RG_ENGINE_CLOUD_BAKE_B6 = 1,   /* the cloud self-shadow bake 53c455b6's b6 */
  RG_ENGINE_CLOUD_WORLD_B6 = 2,  /* the world clouds 3dd31a42's b6 (VolumetricCludsState, 192 B) */
  RG_ENGINE_CB12 = 3             /* cb12, CRenderInterface::CalculateSharedConstants (1584 B) */
};
/* A state of a patch / lease, as stats report it. */
enum rg_state { RG_STATE_PENDING = 0, RG_STATE_OK = 1, RG_STATE_REFUSED = 2 };

typedef struct rg_blob {
  const void* bytes;
  size_t size;
} rg_blob;

/* compile_hlsl's input. `source` = the HLSL text (`path` then names it in errors and gives the include folder), or
 * NULL = read the file at `path` (UTF-8). defines: "NAME" or "NAME=VALUE". include_dir NULL = the file's folder. */
typedef struct rg_hlsl_desc {
  uint32_t struct_size; /* sizeof(rg_hlsl_desc) - fields are only ever appended */
  const char* source;
  const char* path;
  const char* entry;   /* e.g. "main" */
  const char* profile; /* e.g. "cs_6_0" */
  const char* const* defines;
  uint32_t define_count;
  const char* include_dir;
} rg_hlsl_desc;

/* ---- M1: a shader patch - our bytecode in place of one engine shader, live-switchable between variants ---- */
#define RG_MAX_VARIANTS 4u
typedef struct rg_patch_desc {
  const char* name;           /* for the log */
  uint32_t stage;             /* rg_stage: the stage whose bytecode is replaced */
  uint8_t hash[16];           /* the engine shader's DXIL HASH chunk */
  const rg_blob* variants;   /* 1..RG_MAX_VARIANTS blobs; a clone per variant is built when the engine makes a PSO.
                                 VS / PS: {NULL, 0} = the ENGINE's bytecode stays - a variant that only changes the
                                 pipeline state (rg_host::patch_state); such a variant can hold no lease */
  uint32_t variant_count;
  int32_t initial_variant;    /* -1 = vanilla until rg_host::patch_select */
  int32_t priority;           /* two clients on one shader: the higher one wins, ties go to the first registered */
} rg_patch_desc;

/* ---- M1 (2026-10-10): a variant's PIPELINE STATE beyond the bytecode (rg_host::patch_state), graphics only ----
 * A copy of the engine's own desc is edited at clone time, group by group: only the groups named in `set` change,
 * the rest stays the engine's. Composed patches (one VS + one PS + one GS on a PSO) apply theirs in stage order VS,
 * PS, GS - a later one that sets a group an earlier one set wins (logged). The engine still binds ITS render targets
 * and depth buffer: a format edit must match what it binds, or the draw is invalid - yours to know. */
#define RG_PSO_CULL 1u        /* cull_mode */
#define RG_PSO_FILL 2u        /* fill_mode */
#define RG_PSO_DEPTH_BIAS 4u  /* depth_bias, depth_bias_clamp, slope_scaled_depth_bias */
#define RG_PSO_DEPTH 8u       /* depth_enable, depth_write, depth_func */
#define RG_PSO_BLEND 16u      /* blend, for each render target in rt_mask (write masks untouched) */
#define RG_PSO_WRITE_MASK 32u /* write_mask[i], for each render target i in rt_mask */
#define RG_PSO_FORMATS 64u    /* num_render_targets, rtv_formats, dsv_format */
typedef struct rg_pso_blend {
  uint32_t enable;                    /* 0 / 1 */
  uint32_t src, dst, op;              /* D3D12_BLEND, D3D12_BLEND, D3D12_BLEND_OP */
  uint32_t src_alpha, dst_alpha, op_alpha;
} rg_pso_blend;
typedef struct rg_pso_state {
  uint32_t struct_size;               /* sizeof(rg_pso_state) - fields are only ever appended */
  uint32_t set;                       /* RG_PSO_* */
  uint32_t cull_mode;                 /* D3D12_CULL_MODE: 1 none, 2 front, 3 back */
  uint32_t fill_mode;                 /* D3D12_FILL_MODE: 2 wireframe, 3 solid */
  int32_t depth_bias;
  float depth_bias_clamp, slope_scaled_depth_bias;
  uint32_t depth_enable, depth_write; /* 0 / 1 */
  uint32_t depth_func;                /* D3D12_COMPARISON_FUNC (1..8) */
  uint32_t rt_mask;                   /* bit i = render target i, for RG_PSO_BLEND / RG_PSO_WRITE_MASK */
  rg_pso_blend blend;
  uint8_t write_mask[8];              /* D3D12_COLOR_WRITE_ENABLE bits per render target */
  uint32_t num_render_targets;        /* 0..8 */
  uint32_t rtv_formats[8];            /* DXGI_FORMAT */
  uint32_t dsv_format;
} rg_pso_state;

typedef struct rg_patch_stats {
  uint32_t state;         /* rg_state */
  int32_t variant;        /* the selected one, -1 = vanilla */
  uint32_t engine_psos;   /* engine PSOs created with this shader */
  uint32_t clones;        /* clones built (variants x engine PSOs) */
  uint32_t clone_failed;
  uint32_t last_hr;
  uint64_t engine_hits;   /* binds of an engine PSO carrying this shader */
  uint64_t swaps;         /* ... where a clone went in instead (a companion: its dispatches) */
  uint64_t out_of_scope;  /* M11: binds OUTSIDE the patch's node (patch_scope) - the engine's shader stayed */
} rg_patch_stats;

/* ---- M2: a buffer or texture of ours bound into a register the ENGINE's descriptor tables cover (0 root DWORDs) ----
 * A lease = (stage, kind, register) in space0, scoped to a set of patches and / or companions. Granted only if every
 * variant of every patch (and every companion's blob) reads exactly that register (checked at register time), the
 * vanilla shader does NOT read it (checked when the engine creates its first PSO - a failure REFUSES the patch /
 * companion: it stays vanilla / is never dispatched), and the engine's root-signature definition covers it (checked
 * at the first bind - a failure leaves the patch vanilla and the companion undispatched).
 * What is bound: a CBV lease takes a cbuffer (rg_host::cbuffer_create) or a CBV view; an SRV / UAV lease takes a view -
 * of a buffer the client owns (rg_host::view_create), or of a texture the client owns or the engine's render graph
 * holds by name (rg_host::view_create_texture). The view must be what every owner DECLARES at that register (checked
 * at register time, from the blob's PSV0 / RDAT): a raw view for a ByteAddressBuffer, a structured one for a
 * StructuredBuffer, a texture view of the declared dimension.
 * For a COMPANION the descriptor is staged at every dispatch of the engine's shader it accompanies (the vanilla shader
 * does not read the register, so it is inert there), and the companion is dispatched only if the descriptor sat in
 * the engine's table for that dispatch.
 * For RT LIBRARIES (stage CS - DispatchRays binds the CS tables): staged at EVERY DispatchRays, whichever state object
 * it uses - an engine RT dispatch carries no shader identity the host could match, and a baked library reads the
 * register at every dispatch. So: every library the ENGINE creates is checked (its RDAT) and must not declare the
 * register - one that does refuses the lease (and the libraries not baked yet stay vanilla); a source that is not ready
 * stages a NULL descriptor (reads 0) instead of leaving whatever the engine's last compute dispatch put there.
 * Hence ONE RT lease per register in the process: a second one (any client) gets RG_E_TAKEN. Leases of shaders share a
 * register freely (staged per draw / dispatch of their own shaders). */
typedef struct rg_lease_desc {
  const char* name;
  uint32_t stage;               /* rg_stage */
  uint32_t kind;                /* rg_kind */
  uint32_t reg;                 /* the register, space0, as the engine's tables cover it (2.31, logged at the first
                                   bind as "native table: XS covers"): b0-b13, u0-u18; SRV t0-t126 for VS / PS,
                                   t0-t92 for CS (and RT) - outside it the owner never runs */
  const rg_handle* patches;   /* a patch / companion may hold several leases (different registers) */
  uint32_t patch_count;
  rg_handle cbuffer;           /* CBV: what is bound there */
  const rg_handle* companions; /* rg_host::companion_register handles (stage CS) */
  uint32_t companion_count;
  rg_handle view;              /* SRV / UAV (or a CBV view): what is bound there */
  const rg_handle* rtlibs;     /* rg_host::rtlib_register handles (stage CS) - see above */
  uint32_t rtlib_count;
} rg_lease_desc;

typedef struct rg_lease_stats {
  uint32_t state;           /* rg_state */
  uint32_t ready;           /* the host's descriptor heap exists (and, for a view, a resource was set) */
  uint32_t heap_kind;       /* 1 UPLOAD, 2 GPU_UPLOAD */
  uint32_t shader_refs;     /* engine ShaderRef ids learned for the lease's patches / companions */
  int32_t staging_index;    /* the register's index in the engine's staging array, -1 = not resolved yet */
  uint32_t pad;
  uint64_t stages;          /* our descriptor written into a context's staging slot */
  uint64_t restores;        /* the engine's descriptor given back */
  uint64_t lost;            /* too many contexts at once - not staged */
  uint64_t not_staged;      /* companion dispatches dropped: our descriptor was not in the engine's table */
  uint64_t null_stages;     /* RT: a NULL descriptor staged (the source was not ready) */
  uint64_t static_skips;    /* SRV: not staged - the engine's table for that register was a STATIC one at the time */
  uint32_t rt_checked;      /* RT: engine libraries checked against the register (none may declare it) */
  uint32_t rt_baked;        /* RT: owner libraries baked so far */
} rg_lease_stats;

/* M3: where a library went. A library WITH raygen variants is ADDED next to the engine's (exports: its variants); one
 * without is SWAPPED in place of the engine's (the highest priority on that engine library) - at bake time, no live
 * switch. Both edit only the creating thread's subobject list; the engine's own blob is never written. */
typedef struct rg_rtlib_stats {
  uint32_t state;    /* rg_state: OK once in a state object, REFUSED (a lease, a name, a higher-priority swap) */
  uint32_t swapped;  /* state objects built with it in place of the engine's library */
  uint32_t added;    /* ... with it next to the engine's library */
  uint32_t failed;   /* creations that failed with it - rebuilt vanilla */
} rg_rtlib_stats;

/* ---- M4: work on the engine's command list ----
 * fn runs on the recording thread with the engine's ID3D12GraphicsCommandList*. Return RG_PASS_TOUCHED_STATE when it
 * changed the root signature / PSO / heaps - the host then restores the engine's state (GpuApi::Hacks::RestoreState). */
#define RG_PASS_TOUCHED_STATE 1u
typedef uint32_t (*rg_pass_fn)(void* user, void* cmdlist);

/* ---- M4c: dispatch subscriptions, the device ---- */
#define RG_DISPATCH_INDIRECT 1u /* rg_dispatch_info.flags: an ExecuteIndirect - the group counts are on the GPU (0 here) */
typedef struct rg_dispatch_info {
  uint32_t gx, gy, gz; /* the engine's group counts; 0 for an indirect dispatch */
  uint32_t flags;      /* RG_DISPATCH_* */
} rg_dispatch_info;
typedef void (*rg_dispatch_fn)(void* user, const rg_dispatch_info* info);

/* ---- M4, the graphics wing (2026-10-10): draw subscriptions, draw companions, passes after a draw ----
 * rg_draw_info.kind: which GpuApi draw recorded it - the D3D12 call it made. */
enum rg_draw_kind {
  RG_DRAW_INSTANCED = 0,         /* DrawInstanced(count, instances, start, start_instance) */
  RG_DRAW_INDEXED = 1,           /* DrawIndexedInstanced(count, instances, start, base_vertex, start_instance) */
  RG_DRAW_INDIRECT = 2,          /* ExecuteIndirect of one draw - the counts are on the GPU (0 here) */
  RG_DRAW_INDEXED_INDIRECT = 3   /* ... of one indexed draw */
};
#define RG_DRAW_SYSTEM 1u /* rg_draw_info.flags: vertices the engine uploaded for this draw (GpuApi::DrawSystem*: the
                             visible sky's bottom layer, UI, debug primitives); start / base_vertex are 0 */
typedef struct rg_draw_info {
  uint32_t kind;  /* rg_draw_kind */
  uint32_t flags; /* RG_DRAW_* */
  uint32_t count, instances, start;  /* vertices / indices per instance, instances, the first vertex / index */
  int32_t base_vertex;               /* indexed */
  uint32_t start_instance;
} rg_draw_info;
typedef void (*rg_draw_fn)(void* user, const rg_draw_info* info);
typedef void (*rg_device_fn)(void* user, void* d3d12_device);

typedef struct rg_host_stats {
  uint32_t pso_hooks;          /* 1 = the PSO hooks are attached */
  uint32_t mapped;             /* engine PSOs mapped to clones / companions */
  uint64_t compute_creates, graphics_creates, creates_unhashed;
  uint64_t compute_binds, compute_bind_cycles;    /* our code inside ComputePSO::CreatePSO (rdtsc, per 256 calls) */
  uint64_t graphics_binds, graphics_bind_cycles;
} rg_host_stats;

/* ---- M5: engine RenderFlow resources by name (docs/internals.md) ---- */
typedef struct rg_flow_stats {
  uint32_t attached;     /* 1 = the node-context hook is live */
  uint32_t last_id;      /* the GpuApi texture id of the last hit */
  uint64_t hits;         /* resolved to a live texture */
  uint64_t misses;       /* asked inside a node, but the name did not resolve (not alive at this point) */
  uint64_t outside;      /* asked outside a node's Process, or in the declaration pass */
  uint64_t id_changes;   /* the resolved id differed from the previous hit (a transient: once per frame) */
} rg_flow_stats;

/* ---- M6: a node of ours in the engine's render graph (docs/internals.md) ---- */
typedef struct rg_node_info {
  uint32_t declare;  /* always 0: fn runs only in the real pass; the host declares the node's resources itself */
  uint32_t view;     /* the node context's view index */
  void* cmdlist;     /* ID3D12GraphicsCommandList* of the group the node records into */
} rg_node_info;
/* Return RG_PASS_TOUCHED_STATE when fn changed root signature / PSO / heaps - the host restores the engine's state. */
typedef uint32_t (*rg_node_fn)(void* user, const rg_node_info* info);
typedef struct rg_node_stats {
  uint32_t attached;          /* 1 = the graph-builder hook is live */
  uint32_t inserted;          /* times the node went into a built graph (once per graph build) */
  uint32_t skipped_toplevel;  /* the anchor was a top-level node - not inserted */
  uint32_t pad;
  uint64_t executes, declares, no_cmdlist, restores;
  uint64_t requests;          /* RenderFlow requests the host issued for the node, both passes */
} rg_node_stats;
/* node_use access bits (the engine's RTUseBegin mask) */
#define RG_FLOW_READ 1u
#define RG_FLOW_WRITE 2u
#define RG_FLOW_KEEP 4u /* the contents are kept: without it a transient's first use is discarded by the engine */
/* ---- M8: an ENGINE node's Execute, wrapped (docs/internals.md "Wrapping an engine node") ---- */
typedef struct rg_wrap_stats {
  uint32_t attached;  /* 1 = the graph-builder hook is live */
  uint32_t wrapped;   /* 1 = the target's class vtable carries the host's Execute */
  uint32_t shared;    /* 1 = that vtable is shared by several names (a command-list GROUP): matched by name per call */
  uint32_t pad;
  uint64_t executes;  /* real runs of the target (before / after called) */
  uint64_t declares;  /* declaration runs (the original only) */
  uint64_t no_cmdlist, restores;
  uint64_t jobs;      /* real runs where the node uses the job builder: its jobs record AFTER `after` */
  uint64_t rebinds;   /* `after` found no list bound (a group unbinds it) - the host bound the node's flow-thread list */
} rg_wrap_stats;

/* ---- M12: RT raygen variants ---- */
typedef struct rg_rt_variant_stats {
  uint64_t swapped;      /* DispatchRays that ran the variant */
  uint64_t out_of_scope; /* ... of the raygen, in a state object carrying the library, outside the node (vanilla ran) */
  uint64_t off;          /* ... while switched off (vanilla ran) */
  uint64_t missing;      /* the state object has no such export (not baked with the library?) - vanilla ran */
  uint32_t on;
  uint32_t pad;
} rg_rt_variant_stats;
/* ---- M9: an ENGINE node's work, removed (docs/internals.md "Removing an engine node") ---- */
typedef struct rg_remove_stats {
  uint32_t attached;  /* 1 = the graph-builder hook is live */
  uint32_t wrapped;   /* 1 = the target's class vtable carries the host's Execute */
  uint32_t on;        /* the switch as last set */
  uint32_t pad;
  uint64_t removed;   /* real runs replaced by the replay of the node's declared requests */
  uint64_t ran;       /* real runs of the original (switch off when the frame was declared) */
  uint64_t replayed;  /* RenderFlow requests replayed */
  uint64_t overflows; /* the declaration run issued more requests than the host records - the original ran */
  uint64_t refused;   /* calls on a group or a job-builder node - never removed, the original ran */
} rg_remove_stats;
/* node_alloc: our own transient. The engine's desc, as far as it is decoded (docs/internals.md "Requests"). */
typedef struct rg_flow_desc {
  uint32_t width, height;
  uint8_t engine_format; /* the engine's texture format, NOT DXGI: 5 = R8G8B8A8_UNORM (t64's) */
  uint8_t engine_flags;  /* t64's = 0x39 (render target + UAV) */
  uint8_t pad[2];
} rg_flow_desc;

/* ---- M2 (2026-10-10): a TEXTURE view as a lease's source (rg_host::view_create_texture) ----
 * The dimension = the DXIL ResourceKind the shader declares (Texture2D -> RG_TEX_2D, RWTexture3D -> RG_TEX_3D ...):
 * lease_register refuses an owner whose declaration of the register is another kind. UAV: 1D, 1D_ARRAY, 2D, 2D_ARRAY,
 * 3D only. */
enum rg_tex_dim {
  RG_TEX_1D = 1,
  RG_TEX_2D = 2,
  RG_TEX_2DMS = 3,
  RG_TEX_3D = 4,
  RG_TEX_CUBE = 5,
  RG_TEX_1D_ARRAY = 6,
  RG_TEX_2D_ARRAY = 7,
  RG_TEX_2DMS_ARRAY = 8,
  RG_TEX_CUBE_ARRAY = 9
};
typedef struct rg_texture_view_desc {
  uint32_t dimension;   /* rg_tex_dim */
  uint32_t format;      /* DXGI_FORMAT of the view; 0 = the resource's own (which then must not be TYPELESS) */
  uint32_t mip;         /* SRV: the most detailed mip; UAV: the mip slice */
  uint32_t mip_count;   /* SRV: 0 = every mip from `mip` on; UAV: unused */
  uint32_t first_slice; /* arrays: the first slice (a cube array: the first 2D face); 3D UAV: the first W slice */
  uint32_t slice_count; /* 0 = every slice from first_slice on (a cube array: whole cubes) */
  uint32_t plane;       /* 2D / 2D_ARRAY: the plane slice */
  rg_handle flow;       /* 0 = a texture the client owns (view_set_resource); > 0 = an ENGINE RenderFlow texture
                           (flow_register handle), resolved at every stage - see view_create_texture */
} rg_texture_view_desc;

/* ---- M10: shader-visible descriptors from the ENGINE's per-frame ring (docs/internals.md "Descriptors") ---- */
typedef struct rg_descriptors {
  uint64_t cpu;       /* D3D12_CPU_DESCRIPTOR_HANDLE.ptr of the first: write views here (CreateShaderResourceView ...) */
  uint64_t gpu;       /* D3D12_GPU_DESCRIPTOR_HANDLE.ptr of the first: SetGraphics/ComputeRootDescriptorTable */
  uint32_t increment; /* bytes between two descriptors */
  uint32_t count;
} rg_descriptors;

typedef struct rg_host {
  uint32_t version;
  uint32_t struct_size;

  void (*log)(uint32_t level, const char* message);

  /* M1 */
  rg_handle (*patch_register)(const rg_patch_desc* desc);
  void (*patch_select)(rg_handle patch, int32_t variant); /* [hot] -1 = vanilla; lands at the next state change */
  void (*patch_stats)(rg_handle patch, rg_patch_stats* out);

  /* M2 */
  /* bytes: multiple of 256. initial NULL = NOT READY until the first write / feed: a patch leasing it stays vanilla
   * until then (a shader never reads a cbuffer nobody filled). */
  rg_handle (*cbuffer_create)(uint32_t bytes, uint32_t ring, const void* initial);
  void (*cbuffer_write)(rg_handle cbuffer, uint32_t offset, const void* data, uint32_t size); /* new ring version */
  rg_handle (*lease_register)(const rg_lease_desc* desc);
  void (*lease_stats)(rg_handle lease, rg_lease_stats* out);

  /* M3: an RT library baked into the engine's state object at creation - swapped in, or added with its variants
   * (rg_rtlib_stats) */
  rg_handle (*rtlib_register)(const char* name, const uint8_t* hash16, rg_blob library, int32_t priority);

  /* M4a: a COMPANION - a second PSO built from the engine's desc with our CS, dispatched right after the engine's own
   * dispatch with every root argument still bound. companion_request: from inside that dispatch, on its recording
   * thread; 1 = accepted. */
  rg_handle (*companion_register)(const char* name, const uint8_t* hash16, rg_blob cs);
  uint32_t (*companion_request)(rg_handle companion, uint32_t gx, uint32_t gy, uint32_t gz);
  void (*companion_stats)(rg_handle companion, rg_patch_stats* out);

  /* M4b: after the compute dispatch now being recorded on this thread (after any companion), call fn. 1 = accepted. */
  rg_handle (*pass_register)(const char* name);
  uint32_t (*pass_after_dispatch)(rg_handle pass, rg_pass_fn fn, void* user);

  /* 1 = some client holds this shader / RT library and the host's hooks for it are live */
  uint32_t (*shader_claimed)(const uint8_t* hash16);
  void (*host_stats)(rg_host_stats* out);

  /* M2: every time the engine writes block `which` (rg_engine_block), its first `size` bytes are copied into the
   * cbuffer at `offset` as a new ring version - fresh per engine write, no polling. Register-time only. */
  int32_t (*cbuffer_feed)(rg_handle cbuffer, uint32_t which, uint32_t offset, uint32_t size);

  /* M2, SRV / UAV / CBV: a view of a client-owned D3D12 BUFFER (ID3D12Resource*, passed as void*) - structured when
   * stride > 0 (NumElements = bytes / stride), raw (R32_TYPELESS) when stride = 0; a CBV view (stride 0) covers the
   * first `bytes` rounded up to 256, at most 65536 and within the buffer. view_create at registration (kind
   * RG_KIND_SRV / UAV / CBV; ring = descriptor slots for resource changes, 1..8). NOT READY until view_set_resource - its
   * leases stage nothing until then. view_set_resource may be called at any time, from any thread; the host AddRefs
   * the resource and releases the previous one; NULL = not ready again. Its state transitions stay the client's.
   * A view_create_texture view of a client texture takes the texture here, bytes 0. */
  rg_handle (*view_create)(uint32_t kind, uint32_t stride, uint32_t ring);
  int32_t (*view_set_resource)(rg_handle view, void* d3d12_resource, uint64_t bytes);

  /* M4c (2026-10-06): a SUBSCRIPTION to an engine compute shader - the client decides per dispatch, no outside event.
   * fn runs right after every GpuApi::DispatchCompute - and, since 2026-10-08, every DispatchIndirectCompute
   * (info->flags RG_DISPATCH_INDIRECT, counts 0: a companion requested there names its own counts) - whose engine PSO
   * carries the shader (DXIL HASH), on the recording thread, inside the host's hook, the engine's Dispatch already
   * recorded. From inside fn the client may
   * call companion_request, pass_after_dispatch and cbuffer_write; a companion / pass requested there is recorded
   * right after fn returns (companion first). A cbuffer written there reaches the NEXT dispatch (this one's tables
   * were copied before it). Several subscriptions on one shader run in registration order. Register-time only. */
  rg_handle (*dispatch_subscribe)(const char* name, const uint8_t* hash16, rg_dispatch_fn fn, void* user);
  /* The engine's ID3D12Device* (as void*; ReShade's proxy when ReShade is loaded - what the engine itself calls), NULL
   * before it exists. Anything a client creates for the engine's command lists lives on THIS device. */
  void* (*device)(void);
  /* fn runs ONCE, on the thread of the engine's first PSO creation (startup, seconds before the first frame), with
   * device() valid - where a client creates its resources and pipelines. Register-time only. */
  int32_t (*device_ready)(rg_device_fn fn, void* user);
  /* The latest copy of an engine block (rg_engine_block): returns its write counter (0 = never written); copies
   * min(size, max_bytes) bytes into out and the engine's byte count into *size when they are non-NULL. [hot] */
  uint64_t (*engine_block)(uint32_t which, void* out, uint32_t max_bytes, uint32_t* size);
  /* 1 = RG_LOG_DEBUG lines are written from now on (most of them come at startup: set it before the first PSO). */
  void (*log_set_verbose)(uint32_t on);

  /* M5 (2026-10-07): an engine RenderFlow resource BY NAME - `name_hash` is the constant the engine's node code
   * declares it under (view 0; t64, the cloud shadow map, = 0x583c0f7f; a [Diagnostics] RenderGraphDump
   * census lists them: render-graph/graph-NNN-resources.txt). Register-time only. */
  rg_handle (*flow_register)(const char* name, uint32_t name_hash);
  /* name_hash 0: `name` is a transient another client PUBLISHES with node_alloc ("<mod>/<name>"); the host hashes it.
   * Load order does not matter; a name nobody publishes is logged at attach and never resolves. */
  /* The texture's ID3D12Resource* (no AddRef) as the node recording on THIS thread sees it now - call it from a
   * dispatch subscription or an after-dispatch pass; NULL outside a render-graph node, in the declaration pass, or
   * when the resource is not alive at this point of the frame. A transient is a different resource every frame:
   * never keep the pointer past this recording. *gpuapi_id (optional) = the engine's texture id, 0 on NULL. [hot] */
  void* (*flow_texture)(rg_handle res, uint32_t* gpuapi_id);
  void (*flow_stats)(rg_handle res, rg_flow_stats* out);

  /* M6 (2026-10-07): OUR NODE in the engine's render graph - inserted when the engine builds a graph, right after the
   * node whose class name is `after` (e.g. "CRenderNode_VolumetricFog"), in the same group, and run by the engine in
   * its turn: fn gets the group's command list on the recording thread; flow_texture works inside it. Only after a
   * group MEMBER (the SDK's reference/nodes.md lists the groups and their members). `name` becomes the node's class
   * name in the graph (GetName): unique among every client's nodes (RG_E_TAKEN). Register-time only. */
  rg_handle (*node_register)(const char* name, const char* after, rg_node_fn fn, void* user);
  void (*node_stats)(rg_handle node, rg_node_stats* out);

  /* M7 (2026-10-07): the node's RenderFlow DECLARATIONS - what the engine's own nodes ask in the graph's declaration
   * run, so the engine keeps a resource alive across the node and places the aliasing barrier / discard / retire of a
   * transient on it. Declared at registration, issued BY THE HOST, identically in both runs around fn (the engine
   * matches the real run's requests to the declaration's by their index on the thread - a request issued in one run
   * only would shift every later node of the group). Register-time only.
   * node_use: an engine resource (flow_register) with RG_FLOW_* access. 0 = ok. */
  int32_t (*node_use)(rg_handle node, rg_handle flow, uint32_t access);
  /* node_alloc: OUR transient, alive (and aliased by the engine) only inside the node; a flow handle for
   * flow_texture from inside fn. Written first by the node: its contents are undefined at each fn. `name` =
   * "<mod>/<name>" (your mod's tag, printable ASCII, no blanks) - the transient is PUBLISHED under it: other clients'
   * nodes read it with flow_register(name, 0) + node_use. One publisher per name (RG_E_TAKEN). */
  rg_handle (*node_alloc)(rg_handle node, const char* name, const rg_flow_desc* desc);
  /* A transient lives from its node_alloc node to the LAST node that node_use's it, in frame order (the engine's
   * group order: Lighting before Transparents before PostFX ...). A user that runs BEFORE the alloc node is ignored by
   * the engine (flow_texture there: NULL).
   * flow_transition: put a RenderFlow texture into a D3D12_RESOURCE_STATES through the ENGINE's state tracker (its
   * TransitionTexture + FlushPendingBarriers on this thread's command list), so raw D3D12 work on it stays coherent
   * with the engine's own later binds. Same context rules as flow_texture. 1 = recorded. [hot] */
  uint32_t (*flow_transition)(rg_handle res, uint32_t d3d12_state);

  /* M8 (2026-10-07): WRAP an engine node - `before` / `after` (either may be NULL, not both) run around the Execute of
   * every node named `target`: a class name ("CRenderNode_VolumetricFog") or a top-level command-list group's name
   * ("Lighting"), on the command list the engine bound for that node, real run only (never in the declaration run),
   * so flow_texture / flow_transition work inside them as in a node of ours. The host patches slot [1] of the class
   * vtable the first time the engine registers such a node. A group's Execute leaves no list bound - for `after` the
   * host binds the list its members record into (rg_wrap_stats.rebinds); it ends after its members' jobs, a plain
   * node's job-builder work records after `after` (rg_wrap_stats.jobs). A resource the wrapped node does not keep
   * alive: declare it with node_wrap_use. Register-time only. */
  rg_handle (*node_wrap)(const char* target, rg_node_fn before, rg_node_fn after, void* user);
  void (*node_wrap_stats)(rg_handle wrap, rg_wrap_stats* out);

  /* M9 (2026-10-07): REMOVE the work of every node of class `target` while the switch is on. The node stays in the
   * graph: in the declaration run its original runs (it records nothing, it declares) and the host records its
   * RenderFlow requests; in the real run the host replays exactly those requests instead of running it - every
   * resource keeps its allocation, lifetime, barriers and the engine's per-thread request indices; only the node's GPU
   * work is gone (what it would have written keeps old / undefined contents - whoever reads it gets that). Wraps on
   * the same class still run their before / after around it (= a replacement). ONE removal per node: a second
   * node_remove of the same class (any client) gets RG_E_TAKEN. The switch is latched per frame at
   * the declaration run. Not for a group or a job-builder node (rg_remove_stats.refused). Register-time;
   * node_remove_set any time [hot]. */
  rg_handle (*node_remove)(const char* target, uint32_t on);
  void (*node_remove_set)(rg_handle remove, uint32_t on);
  void (*node_remove_stats)(rg_handle remove, rg_remove_stats* out);

  /* M10 (2026-10-07): `count` CBV/SRV/UAV descriptors from the engine's own per-frame ring, in the shader-visible heap
   * the engine keeps bound on its command lists - the way the engine builds its own tables. Valid for what is
   * recorded THIS frame (the ring is reclaimed by the frame's fence): allocate per use, never keep. Call it on a
   * recording thread (a node / wrap fn, a dispatch subscription, an after-dispatch pass), BEFORE setting your own
   * root signature: it first binds the engine's heaps on the list (the engine's RestoreState - which also re-sets the
   * engine's root signatures and static tables); a list the engine has not drawn on yet has no heap bound. Return
   * RG_PASS_TOUCHED_STATE from your fn as usual. 1 = *out filled. [hot] */
  uint32_t (*gpu_descriptors)(uint32_t count, rg_descriptors* out);

  /* M11 (2026-10-08): "in node X, instead of shader Y run Z" - the patch swaps only where the engine binds its shader
   * INSIDE a node named `node` (GetName: a class, e.g. "CRenderNode_RenderSkyScattering", or a group, e.g. "Lighting";
   * the innermost node recording on the binding thread). A bind elsewhere keeps the engine's shader and counts in
   * rg_patch_stats.out_of_scope. Work a node hands to a JOB is the node's (every job it submits carries it,
   * docs/internals.md "Jobs carry their node"). Register-time; called again, it adds a node (up to 4: the patch
   * swaps inside ANY of them). */
  int32_t (*patch_scope)(rg_handle patch, const char* node);

  /* M12 (2026-10-08): an RT RAYGEN VARIANT - "in node X, instead of raygen Y run Z", switchable LIVE. `lib`
   * (rtlib_register) must carry the engine library's own code PLUS an extra raygen export `variant`
   * (sdk/tools/rt_raygen_variant.py): the engine adds a library with no export list, so the extra raygen is in the very
   * state object the library bakes into. Every DispatchRays whose state object carries `lib` and whose raygen group is
   * named `raygen` (unmangled export names, e.g. "rgs_shadow_main"), recorded inside node `node` (GetName, as for
   * patch_scope; NULL = everywhere), gets the variant's shader identifier in its raygen record - the miss / hit tables,
   * the root signature and every other export stay the engine's. Off = the engine's raygen. Register-time; returns
   * the variant handle. rt_variant_set at any time, from any thread.
   * Several clients may add variants to one engine library, but an export name is ONE function per state object: a
   * variant name another client's library already uses on the same engine library gets RG_E_TAKEN, and its library is
   * refused with it. Prefix your variant exports with your mod's tag ("mymod_rgs_shadow"). */
  rg_handle (*rt_variant)(rg_handle lib, const char* raygen, const char* variant, const char* node);
  int32_t (*rt_variant_set)(rg_handle variant, uint32_t on);
  void (*rt_variant_stats)(rg_handle variant, rg_rt_variant_stats* out);

  /* M11 for a COMPANION (2026-10-08): "in node X, after shader Y run Z" - companion_request is accepted only inside a
   * node named `node` (GetName, as for patch_scope); elsewhere it returns 0 and counts in rg_patch_stats.out_of_scope.
   * Register-time; up to 4 nodes, as patch_scope. */
  int32_t (*companion_scope)(rg_handle companion, const char* node);

  /* M3 (2026-10-10): where an RT library went (rg_rtlib_stats). [hot] */
  void (*rtlib_stats)(rg_handle lib, rg_rtlib_stats* out);

  /* M2 (2026-10-10): a TEXTURE view for a lease (kind RG_KIND_SRV / UAV; ring = descriptor slots, 1..8). Register-time.
   * desc->flow = 0: a texture the client owns, set with view_set_resource(view, resource, 0) as for a buffer view - its
   * D3D12 dimension must fit desc->dimension (a 2DMS / 2DMS_ARRAY one multisampled, the others not; a cube's array a
   * multiple of 6), its state transitions stay the client's.
   * desc->flow > 0: an ENGINE texture by name (flow_register; your handle). Nothing to set: at every draw / dispatch a
   * lease of it stages for, the host resolves it on the recording thread exactly as flow_texture would and writes a view
   * of what it finds (once per resource per frame). Where it does not resolve - outside a node, or not alive at that
   * point of the frame - the lease does not stage: the patch stays vanilla for that draw, the companion is not
   * dispatched, an RT lease stages a NULL descriptor of the view's dimension (reads 0). The engine knows nothing of the
   * read: scope the lease's owners (patch_scope / companion_scope) to nodes where the texture is alive - or make it
   * alive there: node_wrap + node_wrap_use on that node - and put it in a state your shader may read it in
   * (flow_transition in a node_wrap `before`) unless the engine already left it so.
   * view_set_resource on such a view is RG_E_ARG. */
  rg_handle (*view_create_texture)(uint32_t kind, const rg_texture_view_desc* desc, uint32_t ring);

  /* M1 (2026-10-10): variant `variant` of a VS / PS / GS patch also changes the engine PSO's pipeline state
   * (rg_pso_state). Register-time; once per variant (a second call replaces the first). 0 = ok. */
  int32_t (*patch_state)(rg_handle patch, uint32_t variant, const rg_pso_state* state);

  /* M8 (2026-10-10): node_use for a WRAP - the wrapped node also declares `flow` (flow_register; an engine name or a
   * name another client publishes) with RG_FLOW_* access, as a node of ours does: the host issues an RTUseBegin per
   * resource before the wraps' `before` and the node's own work, and an RTUseEnd after the `after`s, in BOTH runs of
   * the graph, identically. The engine then keeps the resource alive (and its memory un-aliased) through the node - a
   * transient reaches the node even when its last engine user ran earlier - so flow_texture / flow_transition in
   * before / after and a lease of it (view_create_texture) scoped to the node (patch_scope / companion_scope) see it
   * with no lifetime of their own to manage. A transient's ALLOC must run before the node in frame order (a use before
   * it is ignored by the engine). Up to 8 per wrap; declarations of several wraps on one node add up, in registration
   * order. Not for a GROUP target ("Lighting"): its Execute leaves no list bound - the declarations are not issued
   * there (logged when the target turns out to be a group). Register-time. 0 = ok. */
  int32_t (*node_wrap_use)(rg_handle wrap, rg_handle flow, uint32_t access);

  /* M4, the graphics wing (2026-10-10). A DRAW SUBSCRIPTION: fn runs right after every draw of an engine PSO whose
   * `stage` shader (RG_STAGE_VS or RG_STAGE_PS) has the DXIL HASH - every GpuApi draw entry: DrawIndexedPrimitive (the
   * world's meshes, decals, skinned), DrawPrimitive, DrawPrimitiveNoBuffers (full-screen passes), the three indirect
   * draws (terrain auto-spawn grass, the clouds, distant lights) and the system-primitive draws - on the recording thread,
   * the engine's draw already recorded, with what it drew (rg_draw_info). A draw the engine skipped (its PSO still
   * compiling, a debug shader skip) is not reported. Also when a patch swapped that PSO for a clone: the hash is the
   * ENGINE's shader. From inside fn: draw_companion_request, pass_after_draw, cbuffer_write (reaches the NEXT draw).
   * Several subscriptions on one shader run in registration order. Unsubscribed draws cost one thread-local load.
   * Register-time only. */
  rg_handle (*draw_subscribe)(const char* name, uint32_t stage, const uint8_t* hash16, rg_draw_fn fn, void* user);
  /* A DRAW COMPANION: a second graphics PSO built from the engine's desc of every PSO whose `stage` (VS / PS) shader
   * has the hash - `shader` in that stage ({NULL, 0} = the engine's bytecode stays) and `state` (rg_pso_state, NULL =
   * none) applied, as a patch variant would. Issued by draw_companion_request from inside a draw subscription of the same
   * engine PSO: the host records the SAME draw again (the same counts / indirect arguments) with the companion PSO, every
   * root argument, render target, viewport and vertex / index buffer still the engine's, then sets back the PSO the
   * engine drew with. Leases cannot name a draw companion (v1): it reads what the
   * engine's draw has bound. Register-time only. */
  rg_handle (*draw_companion_register)(const char* name, uint32_t stage, const uint8_t* hash16, rg_blob shader,
                                       const rg_pso_state* state);
  /* From inside a draw subscription: 1 = the companion is recorded right after the subscriptions return (before any
   * pass_after_draw). 0 = not this draw's PSO, out of scope, or its build failed. [hot] */
  uint32_t (*draw_companion_request)(rg_handle companion);
  /* M11 for a draw companion: requested only inside a node named `node` (as patch_scope; up to 4). Register-time. */
  int32_t (*draw_companion_scope)(rg_handle companion, const char* node);
  /* engine_psos, clones (built), clone_failed, swaps = companion draws recorded, out_of_scope. [hot] */
  void (*draw_companion_stats)(rg_handle companion, rg_patch_stats* out);
  /* From inside a draw subscription: fn(user, cmdlist) runs after the draw (and after a companion). Returning
   * RG_PASS_TOUCHED_STATE makes the host restore the engine's root signatures, heaps and PSO and re-set the topology
   * at the next draw. Render targets, viewports, scissors, vertex / index buffers, stencil ref and blend factor are NOT
   * restored - the engine's NEXT draw may be in the same batch and re-sets none of them: leave them as you found them.
   * 1 = accepted. [hot] */
  uint32_t (*pass_after_draw)(rg_handle pass, rg_pass_fn fn, void* user);

  /* HLSL compiled by the host (2026-10-11; dxcompiler.dll + dxil.dll from next to REDGraph.dll - an optional part of
   * the install). Synchronous, on the CPU, no device needed: call it in init and register the blob as any other. The
   * blob is the host's and lives for the process (the engine keeps pointers into bytecode). Profiles cs_6_x, vs_6_x,
   * ps_6_x (RG_E_UNSUPPORTED otherwise); -O3. The registers must be the engine's layout (free space0 registers - see
   * the SDK guide): its root signatures are global, the DXIL's own one is never read. 0 = ok; RG_E_UNAVAILABLE = the
   * compiler is not installed; RG_E_COMPILE = errors in `error` (NUL-terminated, cut to error_size; may be NULL).
   * Warnings of a successful compile are logged. Thread-safe (one compile at a time). */
  int32_t (*compile_hlsl)(const rg_hlsl_desc* desc, rg_blob* out, char* error, uint32_t error_size);

  /* M9: what a REMOVED node leaves in its outputs (2026-10-11). Without this they keep old / undefined contents (a
   * transient: aliased memory - flicker). With it, in the node's place in the frame, each texture the node DECLARED it
   * writes (RTUseBegin with write access - its own declarations, nothing guessed) that matches `name_hash` (the
   * engine's RenderFlow name hash, as flow_register) is cleared, mip 0 and every slice. name_hash 0 = only the textures
   * the node ALLOCATES itself (its own outputs, nobody else fills them); a SHARED target it only writes into (scene
   * colour, which e.g. the clouds composite onto) is cleared only when named - clearing it wipes the frame:
   * a render target to value[0..3] (RGBA), a depth target to depth value[0] and stencil value[1]. Which value is
   * neutral is the CLIENT's call (motion vectors 0, a mask 0 or 1, depth per the engine's Z convention). A member of
   * a command-list group (graph dump: an `m` line) declares nothing itself - its group does: name its texture, and a
   * named rule that met no write in the node's own declarations is cleared at the node's place if that texture is live
   * there (0.3.2); name_hash 0 finds nothing on a member (logged once). Not cleared
   * (logged once, counted): UAV-only textures, buffers, 1D. The first matching rule wins, in registration order; up to
   * 4 per removal. Register-time. 0 = ok. */
  int32_t (*node_remove_clear)(rg_handle remove, uint32_t name_hash, const float value[4]);

  /* 2026-10-17: the BLAS census's watch list ([Diagnostics] BlasCensus - is a mesh in the ray-traced BVH, does it
   * move there). stage RG_STAGE_VS: every indexed draw of that vertex shader is joined to the BLAS geometries the
   * engine built (STATIC / DYNAMIC / NONE in the host's log); RG_STAGE_CS: its dispatches are counted, and inside which
   * deform-pool writes (CRenderMesh::EvaluatePositions) - for the engine's BVH pool writers, m_computeSkinning_*. A
   * watch is a host subscription (it shares their tables with every client). Up to 32 per stage, all clients together.
   * 0 = watched; RG_E_UNAVAILABLE = the census is off this session (nothing done - call it unconditionally); RG_E_LATE
   * after registration. NVIDIA only (the census reads the NvAPI BLAS builds). Register-time. */
  int32_t (*census_watch)(uint32_t stage, const uint8_t* hash16, const char* name);

  /* 2026-10-17: an engine constant block from the host's CATALOG, by the engine's name of the function that writes it
   * (e.g. "CRenderInterface::CalculateSharedConstants" = cb12, "CRenderVolumetricFogManager::Update" = the fog CS b6;
   * the list: the SDK guide / docs/internals.md "Engine constant blocks"). Returns its id - the `which` of
   * engine_block / cbuffer_feed - and from then on the host captures it (a writer's hook goes on only when one of its
   * blocks is asked for). RG_E_ARG = no such entry (the catalog grows by appending; an id never changes); RG_E_LATE
   * after registration. Register-time. */
  int32_t (*engine_block_find)(const char* name);
} rg_host;

RG_HOST_EXPORT const rg_host* rg_host_get(uint32_t version);
typedef const rg_host* (*rg_host_get_fn)(uint32_t version);

/* The rendezvous: a process-wide queue (a named file mapping + a named mutex, both per process id) where clients that
 * load BEFORE the host leave their init; the host drains it in its Load. Laid out as plain C - the host and every
 * client must agree on it, so RG_RENDEZVOUS_VERSION changes with any layout change. */
typedef void (*rg_client_init_fn)(const rg_host* host, void* user);
#define RG_RENDEZVOUS_VERSION 1u
#define RG_RENDEZVOUS_MAX 32u
typedef struct rg_rendezvous {
  uint32_t version;  /* 0 = fresh (zero-filled by the system), then RG_RENDEZVOUS_VERSION */
  uint32_t count;    /* entries waiting in pending[] */
  uint32_t drained;  /* 1 = the host ran its Load - nothing waits after that */
  uint32_t pad;
  struct {
    rg_client_init_fn fn;
    void* user;
    uint32_t api_version;  /* the client's RG_HOST_API_VERSION */
    uint32_t pad;
  } pending[RG_RENDEZVOUS_MAX];
} rg_rendezvous;

#if defined(_WIN32)
/* The view and the handles live for the process (never closed). */
static inline rg_rendezvous* rg_rendezvous_open(HANDLE* lock) {
  wchar_t name[96];
  const DWORD pid = GetCurrentProcessId();
  HANDLE m;
  rg_rendezvous* r;
  swprintf(name, 96, L"Local\\REDGraph.rendezvous.%lu", (unsigned long)pid);
  m = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, (DWORD)sizeof(rg_rendezvous), name);
  if (!m) return NULL;
  r = (rg_rendezvous*)MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(rg_rendezvous));
  swprintf(name, 96, L"Local\\REDGraph.rendezvous.lock.%lu", (unsigned long)pid);
  *lock = CreateMutexW(NULL, FALSE, name);
  if (!r || !*lock) return NULL;
  WaitForSingleObject(*lock, INFINITE);
  if (r->version == 0) r->version = RG_RENDEZVOUS_VERSION;
  if (r->version != RG_RENDEZVOUS_VERSION) {
    ReleaseMutex(*lock);
    return NULL;
  }
  return r; /* LOCKED - the caller releases *lock */
}
#endif

#if defined(_WIN32) && !defined(RG_HOST_BUILD)
/* The host's table, or NULL (not loaded, its Load not run yet, or older than this header). */
static inline const rg_host* rg_host_find(void) {
  HMODULE m = GetModuleHandleW(RG_HOST_MODULE);
  rg_host_get_fn get = m ? (rg_host_get_fn)(void*)GetProcAddress(m, RG_HOST_EXPORT_NAME) : NULL;
  return get ? get(RG_HOST_API_VERSION) : NULL;
}

/* For a client plugin, in its RED4ext Load: `init(host, user)` runs once the host is there - now (returns 1) or in
 * the host's Load (returns 0). Returns -1 when it cannot wait (no rendezvous, the queue full, or the host already
 * drained it and is not reachable - absent or older than this header): init never runs. */
static inline int rg_host_when_ready(rg_client_init_fn init, void* user) {
  HANDLE lock = NULL;
  rg_rendezvous* r = rg_rendezvous_open(&lock);
  const rg_host* h;
  int ret = -1;
  if (!r) {
    h = rg_host_find();
    if (!h) return -1;
    init(h, user);
    return 1;
  }
  h = rg_host_find();
  if (!h && !r->drained && r->count < RG_RENDEZVOUS_MAX) {
    r->pending[r->count].fn = init;
    r->pending[r->count].user = user;
    r->pending[r->count].api_version = RG_HOST_API_VERSION;
    r->count++;
    ret = 0;
  }
  ReleaseMutex(lock);
  if (h) {
    init(h, user);
    ret = 1;
  }
  return ret;
}
#endif

#ifdef __cplusplus
}
#endif
