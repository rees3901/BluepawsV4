#pragma once
#include <cmath>
#include <stdint.h>
namespace personal {
// Heuristic only: HDOP is dimensionless, not a receiver error bound.
inline uint16_t estimatedAccuracyMetres(double hdop) {
    if (!std::isfinite(hdop) || hdop <= 0) return 0;
    const double metres = std::ceil(hdop * 5.0);
    return metres >= 65534 ? 65534 : uint16_t(metres);
}
}
