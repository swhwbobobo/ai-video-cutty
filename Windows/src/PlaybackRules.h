#pragma once
#include <algorithm>
#include <cmath>
#include <optional>

namespace cutty {
struct Range { double start, end; };

inline bool validDuration(double duration) {
    return std::isfinite(duration) && duration > 0;
}
inline double boundedTime(double time, double duration) {
    return validDuration(duration) && std::isfinite(time)
        ? std::clamp(time, 0.0, duration) : 0.0;
}

// Port the existing macOS marker rules, including implicit endpoints.
struct Markers {
    std::optional<double> a, b;
    bool setA(double time) {
        if (!std::isfinite(time) || time < 0) return false;
        if (b && time >= *b) return false;
        if (a && b && time == *a) return false;
        a = time;
        return true;
    }
    bool setB(double time) {
        if (!std::isfinite(time) || time < 0) return false;
        if (a && time <= *a) return false;
        if (a && b && time == *b) return false;
        b = time;
        return true;
    }
    std::optional<Range> range(double duration) const {
        if (!validDuration(duration) || (!a && !b)) return std::nullopt;
        const double start = a.value_or(0.0), end = b.value_or(duration);
        if (!std::isfinite(start) || !std::isfinite(end) ||
            start < 0 || end > duration || start >= end) return std::nullopt;
        return Range{start, end};
    }
    void clear() { a.reset(); b.reset(); }
};

inline std::optional<double> loopTarget(double time, double rate, double duration,
                                       const Markers& markers, bool enabled) {
    if (!enabled || !std::isfinite(time) || !std::isfinite(rate) || rate == 0 ||
        !validDuration(duration)) return std::nullopt;
    const auto selected = markers.range(duration);
    const auto span = selected.value_or(Range{0.0, duration});
    if (rate > 0 && time >= span.end) return span.start;
    // Stay inside the half-open selection when reversing.
    if (rate < 0 && time <= span.start) return std::nextafter(span.end, span.start);
    return std::nullopt;
}

inline double shuttleRate(double current, bool forward) {
    const double direction = forward ? 1.0 : -1.0;
    if (!std::isfinite(current) || current * direction <= 0) return direction;
    return direction * std::min(8.0, std::abs(current) * 2.0);
}
} // namespace cutty
