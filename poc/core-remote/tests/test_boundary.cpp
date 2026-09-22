// POC_ONLY: proves the harness builds and its test wiring runs end to end on this toolchain. The
// substantive protocol tests live beside this file and replace nothing about it.
#include "poc_boundary.hpp"

#include <iostream>

int main()
{
    if (poc::core_remote::kBoundary != "POC_ONLY") {
        std::cerr << "boundary marker missing\n";
        return 1;
    }
    if (poc::core_remote::kUpstreamIssue != "skawu/HyRemoteView#9") {
        std::cerr << "unexpected upstream issue reference\n";
        return 1;
    }

    std::cout << poc::core_remote::boundaryStatement() << "\n";
    std::cout << "PASS: POC boundary marker and test wiring\n";
    return 0;
}
