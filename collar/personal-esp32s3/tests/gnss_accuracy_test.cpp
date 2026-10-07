#include <assert.h>
#include <limits>
#include "gnss_accuracy.h"
int main() {
    assert(personal::estimatedAccuracyMetres(1.0) == 5);
    assert(personal::estimatedAccuracyMetres(1.01) == 6);
    assert(personal::estimatedAccuracyMetres(20.0) == 100);
    assert(personal::estimatedAccuracyMetres(0) == 0);
    assert(personal::estimatedAccuracyMetres(-1) == 0);
    assert(personal::estimatedAccuracyMetres(std::numeric_limits<double>::quiet_NaN()) == 0);
    assert(personal::estimatedAccuracyMetres(std::numeric_limits<double>::infinity()) == 0);
    assert(personal::estimatedAccuracyMetres(20000) == 65534);
}
