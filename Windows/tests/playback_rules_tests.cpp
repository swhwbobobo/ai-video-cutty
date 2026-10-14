#include "PlaybackRules.h"
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
void check(bool value, const char* reason) {
    if (!value) { std::cerr << reason << '\n'; std::exit(1); }
}
}
int main() {
    using namespace cutty;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    Markers markers;
    check(!markers.range(10), "cleared markers have no explicit selection");
    check(markers.setB(6), "B first");
    check(markers.range(10)->start == 0 && markers.range(10)->end == 6, "implicit A");
    check(!markers.setA(6) && !markers.setA(8), "A cannot cross B");
    check(markers.setA(2), "valid A");
    check(!markers.setB(2) && !markers.setB(1), "B cannot cross A");
    check(markers.setA(1) && markers.setA(3), "A can expand and contract");
    check(markers.setB(5) && markers.setB(8), "B can contract and expand");
    check(!markers.setA(nan) && !markers.setB(nan), "reject nonfinite markers");
    check(!markers.range(0) && !markers.range(nan) && !markers.range(7), "invalid durations");
    check(loopTarget(8, 1, 10, markers, true).value() == 3, "loop at B");
    check(!loopTarget(7.9, 1, 10, markers, true), "no premature loop");
    check(!loopTarget(8, 0, 10, markers, true), "paused does not loop");
    check(!loopTarget(8, 1, 10, markers, false), "disabled does not loop");
    check(loopTarget(3, -1, 10, markers, true).value() < 8, "reverse stays inside B");
    markers.clear();
    check(markers.setA(4) && markers.range(10)->end == 10, "implicit B");
    markers.clear();
    check(loopTarget(10, 1, 10, markers, true).value() == 0, "whole-file loop");
    check(!loopTarget(nan, 1, 10, markers, true), "invalid playhead does not seek");
    check(boundedTime(-5, 10) == 0 && boundedTime(50, 10) == 10, "seek clamps");
    check(boundedTime(nan, 10) == 0 && boundedTime(3, nan) == 0, "seek rejects nonfinite");
    check(shuttleRate(4, false) == -1 && shuttleRate(-2, false) == -4,
          "shuttle changes direction and doubles");
    check(shuttleRate(8, true) == 8, "shuttle capped at 8x");
    std::cout << "Playback rules passed\n";
}
