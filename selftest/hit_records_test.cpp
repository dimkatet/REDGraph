// HitRecordsTest ([Tests] HitRecordsTest = true, docs/internals.md "Known gaps" 10): rt_hit_records' contract as a client
// can see it, all at registration - no GPU, nothing on screen.
//   * a range of `count` records from the TOP of the tables down (base + count <= capacity), never the same indices
//     twice: a second call gets the run of records just below the first;
//   * a capacity other than the first caller's -> RG_E_ARG (one set of tables, one size: the host never guesses);
//   * count 0 -> RG_E_ARG; a count no room is left for -> RG_E_FULL;
//   * `engine_high` (the highest contribution the ENGINE's own descs carried; 0 = the instance desc hook has not looked).
// What it CANNOT cover from one module: a contribution inside ANOTHER module's range (RG_E_TAKEN at
// mesh_proxy_bvh_swap) - a range belongs to the module that asked, and a plugin is one module. That half is covered by
// the second real RT client in a session (the hair mod + this test both hold ranges: their swaps stay in their own).

#include <cstddef>
#include <cstdio>

#include "selftest.hpp"

namespace selftest::hit_records_test {
namespace {

constexpr uint32_t kCapacity = 200001;  // the engine's hit tables on 2.5 (three buffers of 200001 64-byte records)

// The whole test: called at registration, nothing has to run in the frame.
void run(const rg_host* host) {
  if (host->struct_size < offsetof(rg_host, rt_hit_records) + sizeof(void*)) {
    host->log(RG_LOG_ERROR, "hit records test: this REDGraph has no rt_hit_records (older host) - not run");
    return;
  }
  rg_hit_records a{}, b{}, bad{};
  a.struct_size = b.struct_size = bad.struct_size = sizeof(rg_hit_records);
  const int32_t r1 = host->rt_hit_records(8, kCapacity, &a);
  const int32_t r2 = host->rt_hit_records(8, kCapacity, &b);
  const int32_t r3 = host->rt_hit_records(100, kCapacity + 1, &bad);  // a capacity the first caller did not name
  const int32_t r4 = host->rt_hit_records(0, kCapacity, &bad);        // count 0
  rg_hit_records full{};
  full.struct_size = sizeof full;
  const int32_t r5 = host->rt_hit_records(kCapacity, kCapacity, &full);  // more than the tables have left

  const bool ok_a = r1 == 0 && a.count == 8 && a.base <= kCapacity - 8 && a.base > 0;
  const bool ok_b = r2 == 0 && b.count == 8 && b.base + 8 == a.base;  // never twice: right below the first
  char line[520];
  std::snprintf(line, sizeof line,
                "hit records test: range %u+%u -> %d, a second %u+%u -> %d (want %u, right below), capacity %u -> %d, "
                "count 0 -> %d, %u records -> %d (want %d %d %d %d); engine_high %u (frame %u: 0 = the instance desc "
                "hook has not looked yet) - %s", a.base, a.count, r1, b.base, b.count, r2, a.base ? a.base - 8 : 0,
                kCapacity + 1, r3, r4, kCapacity, r5, static_cast<int32_t>(RG_E_ARG), static_cast<int32_t>(RG_E_ARG),
                static_cast<int32_t>(RG_E_ARG), static_cast<int32_t>(RG_E_FULL), a.engine_high, a.frame,
                ok_a && ok_b && r3 == RG_E_ARG && r4 == RG_E_ARG && r5 == RG_E_FULL ? "PASS" : "FAIL");
  const bool pass = ok_a && ok_b && r3 == RG_E_ARG && r4 == RG_E_ARG && r5 == RG_E_FULL;
  host->log(pass ? RG_LOG_INFO : RG_LOG_ERROR, line);
}

}  // namespace

void register_with(const rg_host* host, bool on) {
  if (on) run(host);
}

}  // namespace selftest::hit_records_test
