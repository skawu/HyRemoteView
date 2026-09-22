#pragma once

#include <string_view>

// Every artifact produced by this harness carries this marker so that no PoC result can be
// mistaken for a product release or a frozen architecture decision.
namespace poc::core_remote {

inline constexpr std::string_view kBoundary = "POC_ONLY";
inline constexpr std::string_view kTask = "G3-POC-1A";
inline constexpr std::string_view kUpstreamIssue = "skawu/HyRemoteView#9";

// A single line suitable for the head of a report or a log, stating what this code is and is not.
[[nodiscard]] std::string_view boundaryStatement() noexcept;

}  // namespace poc::core_remote
