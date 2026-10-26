/* REDGraph - DEVELOPER exports, outside the versioned client API (rg_host.h). They answer only while the matching
 * [Dev] switch is on in REDGraph.ini; a mod built for players never calls them. Found with
 * GetProcAddress(GetModuleHandleW(L"REDGraph.dll"), "rg_dev_readback"); the same calls are in CET as
 * Game.RG_Readback(spec) / Game.RG_ReadbackResult(id) (docs/internals.md "GPU readback (dev)").
 *
 * [Dev] Readback: a copy of an engine constant block or of a RenderFlow resource at a node, into a file.
 *   spec (text, tokens separated by blanks):
 *     block:<catalog name | id>                     the next write of that engine constant block -> .bin + .json
 *     flow:<8 hex | mod/name> after:<node> | before:<node>   a RenderFlow texture at that node -> .dds + .json
 *       optional: mip:<n> (default 0), frames:<n> (give up after n frames, default 600)
 *     vsout:<VS hash, 8..32 hex digits>             what that engine VS outputs at its next draw(s), by Stream Output
 *       -> .bin (32-bit components per vertex, in the order of the .json's "elements"; draw after draw) + .json
 *       optional: draws:<n> (1..64 draws of one frame, default 1), frames:<n>
 *   <node> = a node's class name (CRenderNode_VolumetricFog, a client's node_register name); the first instance in a
 *   frame where the resource is live is taken. Groups are not nodes here - name a member.
 */
#ifndef RG_DEV_H
#define RG_DEV_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(RG_HOST_BUILD)
#define RG_DEV_EXPORT __declspec(dllexport)
#else
#define RG_DEV_EXPORT
#endif

enum rg_readback_state { RG_RB_PENDING = 0, RG_RB_DONE = 1, RG_RB_FAILED = -1, RG_RB_UNKNOWN = -2 };

typedef struct rg_readback_info {
  int32_t state;        /* rg_readback_state */
  uint32_t format;      /* DXGI_FORMAT of the data in the file (a texture), 0 = a constant block */
  uint32_t width, height, depth, slices;  /* a texture: mip `mip`'s size, depth (3D) and array slices */
  uint32_t mip;
  uint32_t row_bytes;   /* bytes of one row in the file (tightly packed) */
  uint64_t bytes;       /* data bytes in the file (after the DDS header) */
  uint64_t frame;       /* the host's frame counter at the copy */
  char path[520];       /* the data file (.dds / .bin), UTF-8; "" until done */
  char text[1024];      /* the one-line summary RG_ReadbackResult returns */
} rg_readback_info;

/* A request; > 0 = its id (a bad spec too: its result says why), RG_E_UNAVAILABLE (-8) = [Dev] Readback is off. */
RG_DEV_EXPORT int32_t rg_dev_readback(const char* spec);
/* The request's state; out may be NULL. Thread-safe. */
RG_DEV_EXPORT int32_t rg_dev_readback_result(int32_t id, rg_readback_info* out);

typedef int32_t (*rg_dev_readback_fn)(const char* spec);
typedef int32_t (*rg_dev_readback_result_fn)(int32_t id, rg_readback_info* out);

#ifdef __cplusplus
}
#endif

#endif /* RG_DEV_H */
