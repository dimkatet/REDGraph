#pragma once

// Every 2.31 address and struct offset the host uses, in one place (docs/internals.md "Finding engine addresses";
// scripts/exe_re.py). RVAs relative to the exe base. The game version is fixed at 2.31 by design; the call-site checks
// below cost nothing and stay.

#include <cstdint>
#include <cstring>

namespace core::e231 {

// ---- PSO creation and bind (M1) ----
// PSOSubCache<desc>::Create(self, ComPtr* out, base, u32, u32 key, const desc*) - every PSO of that kind the engine
// makes, with the FULL desc (all stages' bytecode). Only caller: PSOSubCache::Compile.
constexpr uintptr_t kComputeSubCacheCreate = 0x88dc58;
constexpr uintptr_t kGraphicsSubCacheCreate = 0x88f364;
constexpr uintptr_t kComputeCompileRet = 0x88e036;   // PSOSubCache<compute>::Compile -> Create
constexpr uintptr_t kGraphicsCompileRet = 0x88ee27;  // PSOSubCache<graphics>::Compile -> Create
// ComputePSO::CreatePSO / GraphicsPSO::CreatePSO(state) -> ID3D12PipelineState*; the bind funnel. The desc in `state`
// carries NO bytecode at this point.
constexpr uintptr_t kComputeCreatePSO = 0x1f7224;
constexpr uintptr_t kGraphicsCreatePSO = 0x1f744c;
constexpr uintptr_t kSetupForDispatchRet = 0x1f6f7a;  // GpuApi::SetupForDispatch -> ComputePSO::CreatePSO
constexpr uintptr_t kSetupForDrawRet = 0x1f711c;      // GpuApi::SetupForDraw -> GraphicsPSO::CreatePSO

// ---- descriptor tables (M2, feasibility 0.11) ----
constexpr uintptr_t kUpdateRootParams = 0x1f22e4;  // SFrameResources::UpdateRootParams(fr, ctx, bool rays)
constexpr uintptr_t kBindDescriptor = 0x1f3978;    // SFrameResources::BindDescriptor(fr, type, range, slot, h*, force)
constexpr uintptr_t kSetupForDrawUrpRet = 0x1f7030;  // GpuApi::SetupForDraw -> UpdateRootParams
constexpr uintptr_t kBindCbDescRet = 0x1f389f;       // GpuApi::BindConstantBuffer -> BindDescriptor
// The GpuApi device data and, inside it, the ID3D12Device the engine calls CopyDescriptors on - ReShade's proxy when
// ReShade is loaded. Anything the ENGINE consumes (heaps, descriptors) must live on THAT device (docs/internals.md
// "Core rules").
constexpr uintptr_t kDeviceData = 0x3438a28;
constexpr uintptr_t kDeviceInData = 0x13bc4a8;
constexpr uintptr_t kDirectQueueInData = 0x13bc4d0;  // ComPtr<ID3D12CommandQueue> (RED4ext.SDK GpuApi/DeviceData.hpp)
// GpuApi::InitDevice(const DeviceParams&) - creates the device and the global root signatures; every RED4ext plugin's
// Load has run before it (feasibility 0.1), no PSO / state object exists yet. The host's registration window closes
// and its hooks attach at its entry (core::arm).
constexpr uintptr_t kGpuInitDevice = 0x7e0bb4;
constexpr uintptr_t kGpuInitDeviceRet = 0x7e26b6;  // CRenderInterface::Init -> it

// GpuApi::GpuCrashReport(bool) - the engine's own GPU-crash path (breadcrumbs, Aftermath, then its fatal error
// "Gpu Crash for unknown reasons!", gpuApiDX12Error.cpp:42), which ends the process before the crash log's 500 ms
// watchdog sees the removed device. Two callers: Present on DXGI_ERROR_DEVICE_REMOVED / _RESET, and CheckHR_Internal.
constexpr uintptr_t kGpuCrashReport = 0x2a43d98;
constexpr uintptr_t kPresentCrashReportRet = 0x21c7ef;  // GpuApi::Present -> it
constexpr uintptr_t kCheckHrCrashReportRet = 0x18a9e8f;  // GpuApi::CheckHR_Internal -> it

// ---- RT state objects (M3, feasibility 0.7 / 0.7b) ----
constexpr uintptr_t kCreateStateObject = 0x315f68;   // bool(StateObjectData* out, const u32* blob id)
constexpr uintptr_t kContainerCreateRet = 0x31510a;  // ResourceContainer<StateObject>::Create -> it (the only caller)
constexpr uintptr_t kBlobContainer = 0xbdcb00;       // in the device data: entries {ptr, u32 size} at +0x18 + (id-1)*32
constexpr uint32_t kCposMagic = 0x534f5043;          // "CPOS"
// The creator's subobject append: (u8* arr, const D3D12_STATE_SUBOBJECT*) - arr = 64 x 16 B, count u32 at +0x400, no
// bounds check. Called only from the creator; the DXIL library goes in at 0x140316143 (NumExports 0 = export all).
constexpr uintptr_t kSubobjectAdd = 0x316398;
constexpr uintptr_t kSubobjectAddLibRet = 0x316148;  // the creator -> it, for the DXIL library
constexpr uint32_t kSubobjectMax = 64, kSubobjectCount = 0x400;

// ---- command lists (M4, phase 4) ----
constexpr uintptr_t kDispatchCompute = 0x1f6ed4;         // GpuApi::DispatchCompute(x, y, z, ComputeCuUsage)
constexpr uintptr_t kRenderShaderComputeRet = 0x1f6ec3;  // CRenderShaderCompute::Dispatch -> DispatchCompute
// GpuApi::DispatchIndirectCompute(), no args: the args buffer is ctx+0x6c/+0x70 (BindIndirectArgs); it calls
// SetupForDispatch itself, then ExecuteIndirect (list vslot 0x1d8). Its only caller: CRenderShaderCompute::DispatchIndirect.
constexpr uintptr_t kDispatchIndirectCompute = 0x6232ac;
constexpr uintptr_t kRenderShaderComputeIndirectRet = 0x6232a5;
constexpr uintptr_t kGetCommandListContext = 0x1f405c;   // thread-current CommandListContext*, no args
// The graphics wing (2026-10-10): EVERY draw goes through GraphicsPSO::CreatePSO from one of two places - GpuApi::
// SetupForDraw (its 7 callers below record the draw after it) or DrawIndexedPrimitive (SetupForDraw inlined). Each
// records ONE D3D12 draw at its end, skipped when the PSO came back NULL (still compiling): the graphics-dirty byte
// (kCtxGraphicsDirty) is then still 1. The bind is skipped too while that byte is 0 - a batch of draws resolves its PSO
// once - so "which PSO drew" is ctx+0x3d0, not the last bind.
//   DrawIndexedPrimitive(type, base_vertex, start_index, index_count, instances, start_instance) -> DrawIndexedInstanced
//   DrawPrimitive(type, ?, count, ?) -> DrawInstanced(count, 1, 0, 0)       (its 3rd arg; SetupForDraw gets type 3)
//   DrawPrimitiveNoBuffers(type, count, cu) -> DrawInstanced(count, 1, 0, 0) (DrawTriangleNoBuffers tail-jumps here)
//   Draw[Indexed]Primitive[Indirect][NoBuffers](type, ?) -> ExecuteIndirect(device data + kCmdSig*, 1,
//     GetD3DBuffer(ctx + kCtxIndirectArgs), [ctx + kCtxIndirectOffset], NULL, 0)
//   DrawSystemPrimitive / DrawSystemIndexedPrimitive_Impl - vertices uploaded for the draw, then DrawInstanced /
//     DrawIndexedInstanced (UI / debug)
constexpr uintptr_t kDrawIndexedPrimitive = 0x1f2098;
constexpr uintptr_t kDrawIndexedPrimitiveRet = 0x1f2013;  // CRenderGeometryBatcher::DrawMeshInstances -> it
constexpr uintptr_t kDrawPrimitive = 0xbe4abc;
constexpr uintptr_t kDrawPrimitiveRet = 0x1f6ba4;  // DrawSingle -> it
constexpr uintptr_t kDrawPrimitiveNoBuffers = 0x20cc64;
constexpr uintptr_t kDrawPrimitiveNoBuffersRet = 0x20cbc8;  // CMipmapGenerator::GenerateMipmaps_PS -> it
constexpr uintptr_t kDrawIndexedIndirect = 0x623c58;  // DrawIndexedPrimitiveIndirect
constexpr uintptr_t kDrawIndexedIndirectRet = 0x6238b0;  // CRenderTerrainCellCollector::RenderAutoSpawnMeshes -> it
constexpr uintptr_t kDrawIndirectNoBuffers = 0x6245c0;  // DrawPrimitiveIndirectNoBuffers
constexpr uintptr_t kDrawIndirectNoBuffersRet = 0x61bd6b;  // CRenderNode_RenderVolumetricClouds::Execute -> it
constexpr uintptr_t kDrawIndexedIndirectNoBuffers = 0x6247c4;  // DrawIndexedPrimitiveIndirectNoBuffers
constexpr uintptr_t kDrawIndexedIndirectNoBuffersRet = 0x77da22;  // CRenderDistantLightBatcher::RenderLights -> it
constexpr uintptr_t kDrawSystemPrimitive = 0x1f677c;
constexpr uintptr_t kDrawSystemPrimitiveRet = 0x783ae3;
constexpr uintptr_t kDrawSystemIndexed = 0x9dc2ec;  // DrawSystemIndexedPrimitive_Impl
constexpr uintptr_t kDrawSystemIndexedRet = 0x9dc2af;
// GpuApi::GetD3DBuffer(const BufferRef*) -> ID3D12Resource* (device data + (id-1) * 0xb0 + 0x5c0b08)
constexpr uintptr_t kGetD3DBuffer = 0x623ccc;
constexpr uintptr_t kCtxIndirectArgs = 0x6c;    // BufferRef of the indirect arguments (BindIndirectArgs)
constexpr uintptr_t kCtxIndirectOffset = 0x70;  // u32 byte offset into it
constexpr uintptr_t kCmdSigDrawIndexed = 0x13bc678;  // in the device data: ID3D12CommandSignature*, one indexed draw
constexpr uintptr_t kCmdSigDraw = 0x13bc670;         // ... one draw
constexpr uintptr_t kCtxTopology = 0x628;  // u32 D3D_PRIMITIVE_TOPOLOGY set on the list (re-set when it differs)
constexpr uintptr_t kRestoreState = 0x2a3e9d4;           // GpuApi::Hacks::RestoreState(cmdlist)
// GpuApi::TransitionTexture(CommandListContext&, TextureRef, ResourceState, u32 subresource) - ResourceState IS a
// D3D12_RESOURCE_STATES (CopyTextureData passes 0x400 = COPY_DEST), subresource -1 = all; the engine's state tracker
// records it and queues the barrier. FlushPendingBarriers(CommandListContext&) records the queued barriers.
constexpr uintptr_t kTransitionTexture = 0x1f40dc;
// DispatchRays(eRayTracingStateObject so, u32 a2 .. a8, BufferRef) - the ONE path of every RT node's Execute to
// GpuApi::DispatchRays (its only caller): so -> a 24 B entry in the table at [kRtSoTable] + 0x4648 (+0 u32 the
// state-object id) -> SetPipelineState1(the ID3D12StateObject at device data + kRtStateObjects + (id-1) * 0x38);
// a5 * 64 = the raygen record's offset in the shader table, a2 / a3 the grid (DispatchRaysDesc +0x38 / +0x3c).
// The render-mode config vars (ConfigVar<bool>, the value at +0x30 - CRenderInterface::LoadConfig writes it):
// Config::cvPathTracing 0x1432f7e30, Config::cvRayTracingEnable 0x1432f7490.
constexpr uintptr_t kCvPathTracingValue = 0x32f7e60;
constexpr uintptr_t kCvRayTracingEnableValue = 0x32f74c0;
constexpr uintptr_t kDispatchRays = 0x2934058;
constexpr uintptr_t kDispatchRaysRet = 0xdf1d62;  // CRenderNode_RenderRayTracedGlobalShadow::Execute -> it
// GpuApi::GetShaderGroupIdentifier(u32 state-object id, u32 group) -> const void* (32 B): the entry's
// ID3D12StateObjectProperties (+0x8) ->GetShaderIdentifier(names[group]) - names = the entry's DynArray at +0x10,
// 16 B elements, the wchar_t* name at +8. GpuApi::DispatchRays calls it for the RAYGEN record of every dispatch
// (group = the global DispatchRays' a4); its other two callers fill the miss / hit tables.
constexpr uintptr_t kGetShaderGroupIdentifier = 0x3141f0;
constexpr uintptr_t kGpuDispatchRaysGsgiRet = 0x2a407e6;  // GpuApi::DispatchRays -> it
constexpr uintptr_t kRtEntryProps = 0x08;
constexpr uintptr_t kRtEntryNames = 0x10;
constexpr uintptr_t kRtEntryNameCount = 0x1c;  // DynArray size
constexpr uintptr_t kRtSoTable = 0x3427c00;       // a pointer; the entry array at +0x4648
constexpr uintptr_t kRtSoTableArray = 0x4648;
constexpr size_t kRtSoEntry = 24;
constexpr uintptr_t kRtStateObjects = 0x13b2c48;  // in the device data: the state-object container's entries
constexpr size_t kRtStateObjectStride = 0x38;     // = the creator's `out`: +0 ComPtr<ID3D12StateObject>, +0x10 names
constexpr uintptr_t kFlushPendingBarriers = 0x1f7164;
// Acceleration-structure builds (markers, 0.3.2). The BLAS batch builder (the symbols call it
// BindAccelerationStructure+0x104 - a folded name) records per BLAS either the statically linked
// NvAPI_D3D12_BuildRaytracingAccelerationStructureEx(cl, params) or ID3D12GraphicsCommandList4 vtable +0x240; args
// (range {begin, end} of 32 B BLAS entries, geometry table, BufferRef). GpuApi::BuildTopLevelAccelerationStructure(
// u32 dest AS, u32 instance count, BufferRef instances, u32, BufferRef scratch, u32 scratch offset): InstanceDescs =
// the instance buffer's VA with NO offset, and 0 when the ref is invalid ((id - 1) > 0x7ffffffe) - the count is kept.
constexpr uintptr_t kBuildBlasBatch = 0x2a40df4;
constexpr uintptr_t kBuildBlasBatchRet = 0x2a40dec;  // its thunk -> it
constexpr uintptr_t kNvBuildAsEx = 0x9430;
constexpr uintptr_t kNvBuildAsExRet = 0x2a410dc;     // the BLAS batch builder -> it
// CRenderMesh::EvaluatePositions(this, const SRenderNodeImplContext&, const CRenderProxy_Mesh*, char lod, u32 chunk,
// BufferRef pool, u32 pool offset, UpdateContext&) - writes one chunk's deform pool slot with the m_computeSkinning_*
// CS its vertex factory / material picks (the BLAS census). Callers: AddProxies (x2, a BLAS's birth) and the update
// path of UpdateSkinnedInstances (ret below). The chunk's material = *(proxy[+0x118] + (proxy[+0x122 + 2*lod] + chunk)
// * 48 + 8), its [+0x58] = the RT material handler's index. UpdateSkinnedInstances reuses a built BLAS while
// proxy[+0x90] < blas[+0x1c] (a change-driven refit).
constexpr uintptr_t kEvaluatePositions = 0x2930410;
constexpr uintptr_t kEvaluatePositionsRet = 0x291fa0d;  // UpdateSkinnedInstances' per-instance helper -> it
constexpr uintptr_t kAddProxiesEvalRet1 = 0x291daef;    // CRenderAccelerationStructure::AddProxies -> it (x2)
constexpr uintptr_t kAddProxiesEvalRet2 = 0x291dc22;
constexpr uintptr_t kBuildTlas = 0x2a413f4;
constexpr uintptr_t kBuildTlasRet = 0x290a91e;       // CRenderLightVolumeCollector::UpdateAccelerationStructures -> it
constexpr uintptr_t kBufferVa = 0x5c0b00;            // in the device data: + (id - 1) * 0xb0 = a buffer's GPU VA
constexpr size_t kBufferStride = 0xb0;
// AS lifetime (light-volume check, 0.3.2): an AS id is a 16-bit slot (free list of u16) of a 0x58 B entry at device
// data + 0xd1b6a0 (+0 refcount, +0x20 the pool resource, +0x40 its offset). Create(desc*) -> id in eax; Release drops
// the refcount and at 0 QUEUES the id; EvictQueuedDeletions later calls Destroy(const AccelerationStructureRef*), which
// frees the memory and returns the id to the free list. GetAccelerationStructureHandle(id) = resource VA + offset -
// never 0. The light-volume collector (CRenderLightVolumeCollector::UpdateAccelerationStructures) caches its BLAS ids
// by volume data hash and calls GetAccelerationStructureHandle once per instance it puts in its TLAS.
constexpr uintptr_t kCreateAs = 0x87b948;
constexpr uintptr_t kCreateAsRet = 0x290a743;        // the light-volume collector -> it
constexpr uintptr_t kDestroyAs = 0x87c4d4;
constexpr uintptr_t kDestroyAsRet = 0x647c2d;        // GpuApi::EvictQueuedDeletions -> it
constexpr uintptr_t kAsHandle = 0x2a41cc0;
constexpr uintptr_t kAsHandleLightVolumeRet = 0x290a85e;  // the light-volume collector -> it

// ---- CommandListContext layout ----
constexpr uintptr_t kCtxCmdList = 0x30;       // ID3D12GraphicsCommandList*
constexpr uintptr_t kCtxFrameRes = 0x60;      // SFrameResources*
constexpr uintptr_t kFrameResDescIncrement = 0x10;  // u32 in SFrameResources: the CBV/SRV/UAV descriptor size
// GpuDescriptorRangeAllocator: the engine's per-frame RING of shader-visible CBV/SRV/UAV descriptors, in the heap the
// engine keeps bound (every table of UpdateRootParams, GpuApi::ClearBufferUAV_Zero 0x140774384 - which uses a range
// with no SetDescriptorHeaps). Lock-free (RingBufferAllocator::Allocate: lock cmpxchg), released by frame fence.
// AllocateRange(alloc, Range* out, u32 n): Range = {ID3D12DescriptorHeap*, u32 offset, u32 count}.
constexpr uintptr_t kDescRangeAllocator = 0x149de40;  // in the device data
constexpr uintptr_t kAllocateRange = 0x7744b0;
constexpr uintptr_t kDescRingHeap = 0x38;     // in the allocator: the ring's ID3D12DescriptorHeap* (1M, shader-visible)
constexpr uintptr_t kDescSamplerHeap = 0x60;  // in the allocator: the sampler heap BindDescriptorHeaps binds with it
constexpr uintptr_t kBindDescriptorHeaps = 0xa1ffa8;  // SFrameResources::BindDescriptorHeaps(frame, cl, direct): [frame+0]
                                                      // NULL (no heap yet on the context) = returns, binds nothing
constexpr uintptr_t kCtxComputeState = 0xb8;  // ComputePSO state (the arg of ComputePSO::CreatePSO)
constexpr uintptr_t kCtxComputeDirty = 0xc8;  // byte
constexpr uintptr_t kCtxCsRef = 0x10c;        // the bound CS ShaderRef id
constexpr uintptr_t kCtxGraphicsState = 0x110;  // GraphicsPSO state
constexpr uintptr_t kCtxGraphicsDirty = 0x120;  // byte
constexpr uintptr_t kCtxVsRef = 0x3c0;        // the bound VS ShaderRef id
constexpr uintptr_t kCtxPsRef = 0x3c4;        // the bound PS ShaderRef id
constexpr uintptr_t kCtxPso = 0x3d0;          // the PSO set on the list

// ---- SFrameResources / RootSignatureDefinition (0.11) ----
constexpr uintptr_t kFrStaging = 0x28;  // D3D12_CPU_DESCRIPTOR_HANDLE[] (one per covered register)
constexpr uintptr_t kFrDef = 0x68;      // RootSignatureDefinition* (buckets at +0x38, 16 B each)
constexpr uintptr_t kFrStatic = 0x78;   // u64: bit p = table p is a STATIC one (BindStaticTable sets it, BindDescriptor
                                        // clears it - and the table is then copied whole from the staging array)
// Per range kind: the byte map base and its stride per stage (type); byte at base + type*stride + slot*2 = bucket
// index, 0xff = not covered. CBV b0-b13, SRV t0-t127, UAV u0-u18.
// Checked against BindDescriptor's disassembly 2026-10-06 (range 0 -> 0x4c3 + type<<8, 1 -> 0x8c3 + type*0x26,
// 2 -> 0x453 + type*0x1c).
constexpr uintptr_t kDefCbvMap = 0x453;
constexpr uint32_t kDefCbvStride = 0x1c;
constexpr uintptr_t kDefSrvMap = 0x4c3;
constexpr uint32_t kDefSrvStride = 0x100;
constexpr uintptr_t kDefUavMap = 0x8c3;
constexpr uint32_t kDefUavStride = 0x26;
// D3D12_DESCRIPTOR_RANGE_TYPE_*, BindDescriptor's `range`
constexpr uint32_t kRangeSrv = 0;
constexpr uint32_t kRangeUav = 1;
constexpr uint32_t kRangeCbv = 2;

// ---- the render graph, read only (graph_probe.cpp; docs/internals.md) ----
constexpr uintptr_t kGraphExecuteParallel = 0xa86438;  // CRenderNodeGraph::ExecuteParallel(graph, ctx, job::Builder&)
constexpr uintptr_t kRenderFrameExecuteRet = 0x21a068;  // CRenderInterface::RenderFrame -> it (the only caller)
constexpr uintptr_t kNodeProcess = 0x1ec404;     // CRenderNodeBase::Process(node, ctx, job::Builder*) -> Execute
constexpr uintptr_t kGroupProcessRet = 0x1ec281;  // CRenderNodeCommandListGroup::Execute -> it (a member)
// RenderFlow: SRenderFlowInternalData::HandleRequest(data, const SRequest*, u8 thread) - every resource request
constexpr uintptr_t kFlowHandleRequest = 0x1f0f80;
constexpr uintptr_t kFlowAccessRet = 0x1f0da9;  // the access-request helper (type 4) -> it
constexpr uintptr_t kFlowPhase = 2;  // u8 in SRenderFlowInternalData; the graph runs TWICE a frame:
constexpr uint8_t kFlowPhaseDeclare = 1;  // 1 = the declaration run (requests recorded, no command list), 2 = the real one
// SRequest, 0x30 B: +0 u8 type, +1 u8 the flow thread (= ctx+0x38), +4 u32 tag, +8 u32 param or (type 1) a 40 B
// SRenderFlowTargetDesc. Phase 2 matches a request to its declaration by its INDEX within the thread (docs/internals.md
// "Requests"): a node must issue the same sequence in both runs.
constexpr size_t kFlowRequestSize = 0x30;
constexpr uintptr_t kFlowRequestPayload = 0x08;
constexpr uint8_t kFlowReqAlloc = 1;     // RTAlloc: payload = the desc
constexpr uint8_t kFlowReqUseBegin = 4;  // RTUseBegin(tag, access): 1 read, 2 write, 4 keep the contents (no discard)
constexpr uint8_t kFlowReqUseEnd = 5;    // RTUseEnd(tag)
// SRenderFlowTargetDesc, 40 B (the ctor 0x1401edcd0 + the census' descs): +0x10/+0x12 u16 width/height, +0x14/+0x16
// the same again for an absolute size (t64), +0x18 u16 1, +0x1a u16 1, +0x1e u8 0x11, +0x20 u8 flags (ctor 0x09, t64
// 0x39), +0x25 u8 the engine's texture format (ctor 5 = t64's R8G8B8A8_UNORM).
constexpr size_t kFlowDescSize = 40;
constexpr uintptr_t kFlowDescWidth = 0x10;
constexpr uintptr_t kFlowDescHeight = 0x12;
constexpr uintptr_t kFlowDescWidth2 = 0x14;
constexpr uintptr_t kFlowDescHeight2 = 0x16;
constexpr uintptr_t kFlowDescOnes = 0x18;   // u32 0x10001
constexpr uintptr_t kFlowDescKind = 0x1c;   // u32 0x110000 (+0x1e = 0x11)
constexpr uintptr_t kFlowDescFlags = 0x20;
constexpr uintptr_t kFlowDescFormat = 0x25;
// SRenderFlowInternalData::GetResource<GpuApi::TextureRef>(data, TextureRef* out, const u32* tag, u8 thread, bool)
constexpr uintptr_t kFlowGetTexture = 0x1f3d20;
// SRenderNodeImplContext (the ctx of Execute / Process): +0x08 -> SRenderFlowInternalData* (one more deref), +0x30 u8
// flags (bit 0: Test() answers true and a tag takes view 0 - meaning open; NOT the declaration run, that is
// kFlowPhase), +0x34 u32 view (a tag is name_hash ^ view << 24, 32-bit), +0x38 u8 the flow thread
constexpr uintptr_t kNodeCtxFlow = 0x08;
constexpr uintptr_t kNodeCtxFlags = 0x30;
constexpr uintptr_t kNodeCtxView = 0x34;
constexpr uintptr_t kNodeCtxFlowThread = 0x38;
// GpuApi texture container (docs/internals.md "Resources by name (M5)"): SDeviceData + 0x2F1C0, a 0x10 header, then 32768 handles of
// 0xB0 {i32 refCount (< 0 = unused), ..., +8 ID3D12Resource*}; handle = id - 1
constexpr uintptr_t kTexContainer = 0x2f1c0;
constexpr uintptr_t kTexHandles = 0x10;
constexpr size_t kTexStride = 0xb0;
constexpr uintptr_t kTexResource = 0x08;
constexpr uint32_t kTexSlots = 32768;

// ---- our own render-graph node (M6, render_nodes.cpp) ----
// rend::NodeGraphFactory::Register(factory, NodeGroupID, const SRenderNodeParameters&) - every Create<T> -> it
constexpr uintptr_t kFactoryRegister = 0x98585c;
constexpr uintptr_t kFactoryRegisterRet = 0x98398e;  // Create<CRenderNode_VolumetricFog> -> it
constexpr uintptr_t kFactoryOpenGroup = 0xf0;        // the group being filled (CRenderNodeCommandListGroup*), 0 = none
constexpr uintptr_t kParamsNode = 0x08;              // SRenderNodeParameters {u32 type, u32 subtype, CRenderNodeBase*}
constexpr size_t kParamsSize = 0x10;
constexpr uintptr_t kGraphPoolAlloc = 0xa1eff0;      // VPoolRenderGraph Allocate(u32 size) -> zeroed memory
constexpr uintptr_t kNodeTemplateVtable = 0x2b39e20;  // ??_7CRenderNode_VolumetricFog@@6B@ - slots 3..5 are copied
constexpr uint32_t kNodeVtSlots = 6;                 // every CRenderNode_* vtable (168 checked offline)
constexpr uint32_t kOurNodeSize = 0x20;              // CRenderNodeBase is 0x18; +0x18 = our node index
constexpr uintptr_t kOurNodeIndex = 0x18;
// CRenderNodeGraph: nodes = TRenderNodeGraphArray at +0x08, deps = one at +0x38 (count, data, size, live list)
constexpr uintptr_t kGraphNodeLive = 0x10;
constexpr uintptr_t kGraphNodeData = 0x18;
constexpr uintptr_t kGraphNodeSize = 0x24;
constexpr uintptr_t kGraphNodeList = 0x28;
constexpr uintptr_t kGraphDepLive = 0x40;
constexpr uintptr_t kGraphDepData = 0x48;
constexpr uintptr_t kGraphDepSize = 0x54;
constexpr uintptr_t kGraphDepList = 0x58;
constexpr size_t kNodeStride = 48;
constexpr uintptr_t kNodeType = 0x08;
constexpr uintptr_t kNodeSubtype = 0x0c;
constexpr uintptr_t kNodeObject = 0x10;  // CRenderNodeBase*
constexpr uintptr_t kNodeF18 = 0x18;
constexpr uintptr_t kNodeFlowGroup = 0x1c;  // u16
constexpr size_t kDepStride = 24;
constexpr uintptr_t kDepType = 0x04;
constexpr uintptr_t kDepFrom = 0x08;
constexpr uintptr_t kDepTo = 0x0c;
constexpr uint32_t kHandleIndexMask = 0x1fffffff;  // node handles carry 1<<29, dep handles 1<<30
constexpr uint32_t kNodeVtExecute = 1;
constexpr uint32_t kNodeVtGetName = 2;  // CName* GetName(CName* out)
// bool GetJobBuilderUsage(): true = CRenderNodeBase::Process runs a job with the node's job::Builder and Syncs it AFTER
// Execute returns (a group: any member does - its Execute Syncs the members' jobs itself)
constexpr uint32_t kNodeVtJobBuilderUsage = 4;
// GpuApi::BindCommandList(u32 id): a TLS binding only (id at tls+0x1b8, its context at tls+0x188); 0 = none.
// CRenderNodeBase::Process binds a member's list as [[kRtSoTable] + 0x4c50] + flow_thread * 4 + 4 (0x1401ec526) - the
// list its group created for that flow thread. A group's Execute starts with BindCommandList(0) in ctx+0x30 bit-1 mode
// and every member unbinds after itself: nothing is bound when a group's Execute returns.
constexpr uintptr_t kBindCommandList = 0x1ec68c;
// job::Dispatcher::RunJob(dispatcher, const JobDesc*, u8 priority, waiting list, counter) - EVERY job submission
// (job::RunJob and ~10 direct callers). JobDesc = 32 B {fn(data, run ctx), data, name/profile, ?}, copied by value
// into the queue entry; both run paths (DispatcherThread::DoWorkLoop, Dispatcher::DoRunJobQueueEntry) call
// fn(data, &their context) once.
constexpr uintptr_t kDispatcherRunJob = 0x14299c;
constexpr uintptr_t kJobRunJobRet = 0x95eca9;  // job::RunJob -> Dispatcher::RunJob
constexpr uintptr_t kFlowCmdLists = 0x4c50;  // in *kRtSoTable: a pointer to u32 ids, [thread + 1]
// rend::CRenderNodeCommandListGroup: members {CRenderNodeBase*, u64} at +0x28, count +0x34, run in order by its Execute
// (one vtable for every group, ??_7CRenderNodeCommandListGroup@rend@@6B@ 0x142b3a008; GetName = its name at +0x18)
constexpr uintptr_t kGroupExecute = 0x1ec1d0;
constexpr uintptr_t kGroupItems = 0x28;
constexpr uintptr_t kGroupItemCount = 0x34;
constexpr size_t kGroupItemStride = 16;

inline uintptr_t stage_ref_offset(uint32_t stage) { return stage == 0 ? kCtxVsRef : stage == 1 ? kCtxPsRef : kCtxCsRef; }

// The exe is 2.31 iff the call right before `ret_rva` is an E8 landing exactly on `target_rva`.
inline bool call_site_ok(uintptr_t base, uintptr_t ret_rva, uintptr_t target_rva) {
  const auto* p = reinterpret_cast<const uint8_t*>(base + ret_rva - 5);
  if (p[0] != 0xE8) return false;
  int32_t rel;
  std::memcpy(&rel, p + 1, 4);
  return base + ret_rva + static_cast<intptr_t>(rel) == base + target_rva;
}

}  // namespace core::e231
