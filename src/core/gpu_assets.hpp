#pragma once

// Client buffers in DEFAULT heaps and the host's frame fence (gpu_assets.cpp; the API entries are in internal.hpp).

#include <string>

namespace core::gpu_assets {

// One line for the status report: buffers alive, copies pending, retired waiting, frame / fence, lists submitted.
std::string summary();

}  // namespace core::gpu_assets
