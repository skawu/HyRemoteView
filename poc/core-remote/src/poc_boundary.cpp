#include "poc_boundary.hpp"

namespace poc::core_remote {

std::string_view boundaryStatement() noexcept
{
    return "POC_ONLY (G3-POC-1A, skawu/HyRemoteView#9): verification harness, "
           "not a product architecture baseline";
}

}  // namespace poc::core_remote
