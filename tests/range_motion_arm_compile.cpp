#include "../src/range_motion.hpp"

// This translation unit deliberately exercises the public transition seam so
// the ARM build verifies code generation, not just header parsing.
extern "C" bool rangeMotionArmCompileProbe(
    midibuffer::RangeMotionState& state,
    const midibuffer::RangeMotionBounds& bounds,
    midibuffer::RangeMotionSelection& selection, double physicalPosition) {
    return midibuffer::moveRangeMotion(state, bounds, selection,
                                        physicalPosition);
}
