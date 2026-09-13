#ifndef MIDIBUFFER_RANGE_MOTION_HPP
#define MIDIBUFFER_RANGE_MOTION_HPP

#include <stdint.h>

namespace midibuffer {

// Fixed-size state for relative whole-range motion. logicalPosition and
// physicalPosition are normalized to [0, 1]. pulseResidual retains movement
// that has not yet rounded to a whole pulse.
struct RangeMotionState {
    double logicalPosition;
    double physicalPosition;
    // Signed numerator over 2^32-1, always less than half a pulse after a
    // completed transition.
    int64_t pulseResidualNumerator;
    bool established;

    RangeMotionState()
        : logicalPosition(0.0), physicalPosition(0.0),
          pulseResidualNumerator(0), established(false) {}
};

struct RangeMotionBounds {
    uint64_t historyStart;
    uint64_t historyEnd;
};

struct RangeMotionSelection {
    uint64_t start;
    uint64_t end;
};

namespace range_motion_detail {

inline double clampNormalized(double value) {
    if (!(value >= 0.0)) {
        return 0.0;
    }
    return value > 1.0 ? 1.0 : value;
}

inline double absolute(double value) {
    return value < 0.0 ? -value : value;
}

inline bool movableDomain(const RangeMotionBounds& bounds,
                          const RangeMotionSelection& selection,
                          uint64_t& length, uint64_t& travel) {
    if (bounds.historyEnd <= bounds.historyStart ||
        selection.start < bounds.historyStart ||
        selection.end > bounds.historyEnd ||
        selection.end <= selection.start) {
        return false;
    }
    const uint64_t span = bounds.historyEnd - bounds.historyStart;
    length = selection.end - selection.start;
    if (length >= span) {
        return false;
    }
    travel = span - length;
    return travel != 0U;
}

const uint32_t kFractionDenominator = 0xffffffffU;

inline double normalizedSelectionPosition(
    const RangeMotionBounds& bounds,
    const RangeMotionSelection& selection, uint64_t travel) {
    // Subtract before normalizing, then generate the binary fraction directly.
    // This avoids narrowing either absolute or relative uint64 coordinates to
    // floating point (and avoids target-runtime uint64 conversion helpers).
    uint64_t remainder = selection.start - bounds.historyStart;
    if (remainder == travel) {
        return 1.0;
    }
    double result = 0.0;
    double place = 0.5;
    for (int bit = 0; bit < 53 && remainder != 0U; ++bit) {
        if (remainder >= travel - remainder) {
            remainder -= travel - remainder;
            result += place;
        } else {
            remainder += remainder;
        }
        place *= 0.5;
    }
    return result;
}

inline uint64_t divideUnsigned64By32(uint64_t value, uint32_t divisor,
                                     uint32_t& remainder) {
    uint64_t quotient = 0;
    uint64_t workingRemainder = 0;
    for (int bit = 63; bit >= 0; --bit) {
        workingRemainder =
            (workingRemainder << 1U) | ((value >> bit) & 1U);
        if (workingRemainder >= divisor) {
            workingRemainder -= divisor;
            quotient |= static_cast<uint64_t>(1) << bit;
        }
    }
    remainder = static_cast<uint32_t>(workingRemainder);
    return quotient;
}

inline void scaleTravel(uint64_t travel, uint32_t numerator,
                        uint64_t& wholePulses,
                        uint32_t& fractionalNumerator) {
    uint32_t travelRemainder = 0;
    const uint64_t travelQuotient = divideUnsigned64By32(
        travel, kFractionDenominator, travelRemainder);
    wholePulses = travelQuotient * numerator;

    const uint64_t remainderProduct =
        static_cast<uint64_t>(travelRemainder) * numerator;
    uint32_t productRemainder = 0;
    wholePulses += divideUnsigned64By32(
        remainderProduct, kFractionDenominator, productRemainder);
    fractionalNumerator = productRemainder;
}

inline uint32_t normalizedMovementNumerator(double magnitude) {
    if (magnitude >= 1.0) {
        return kFractionDenominator;
    }
    return static_cast<uint32_t>(
        magnitude * static_cast<double>(kFractionDenominator) + 0.5);
}

inline uint64_t roundedScaledMovement(uint64_t travel, uint32_t numerator,
                                      bool towardNewer, uint64_t available,
                                      int64_t previousResidual,
                                      int64_t& nextResidual) {
    if (numerator == 0U) {
        nextResidual = previousResidual;
        return 0U;
    }

    uint64_t wholePulses = 0;
    uint32_t fractionalNumerator = 0;
    scaleTravel(travel, numerator, wholePulses, fractionalNumerator);
    int64_t combinedFraction =
        previousResidual +
        (towardNewer ? static_cast<int64_t>(fractionalNumerator)
                     : -static_cast<int64_t>(fractionalNumerator));

    // If the whole component reaches the legal limit, do not increment or
    // convert beyond uint64 range. The caller is at a clamp and discards debt.
    if (wholePulses >= available) {
        nextResidual = 0;
        return available;
    }

    if (towardNewer &&
        combinedFraction * 2 >=
            static_cast<int64_t>(kFractionDenominator)) {
        ++wholePulses;
        combinedFraction -= kFractionDenominator;
    } else if (!towardNewer &&
               -combinedFraction * 2 >=
                   static_cast<int64_t>(kFractionDenominator)) {
        ++wholePulses;
        combinedFraction += kFractionDenominator;
    }
    nextResidual = combinedFraction;
    return wholePulses;
}

} // namespace range_motion_detail

// Establish from the selected integer pair, never from the pot's absolute
// position. Invalid, empty, and full-history ranges leave state untouched.
inline bool establishRangeMotion(RangeMotionState& state,
                                 const RangeMotionBounds& bounds,
                                 const RangeMotionSelection& selection,
                                 double physicalPosition) {
    uint64_t length = 0;
    uint64_t travel = 0;
    if (!range_motion_detail::movableDomain(bounds, selection, length,
                                             travel)) {
        return false;
    }
    (void)length;
    state.logicalPosition =
        range_motion_detail::normalizedSelectionPosition(bounds, selection,
                                                          travel);
    state.physicalPosition =
        range_motion_detail::clampNormalized(physicalPosition);
    state.pulseResidualNumerator = 0;
    state.established = true;
    return true;
}

// Rebase after a boundary edit or retained-domain change. The last physical
// sample is deliberately retained so the next genuine delta remains relative.
inline bool rebaseRangeMotion(RangeMotionState& state,
                              const RangeMotionBounds& bounds,
                              const RangeMotionSelection& selection) {
    if (!state.established) {
        return false;
    }
    uint64_t length = 0;
    uint64_t travel = 0;
    if (!range_motion_detail::movableDomain(bounds, selection, length,
                                             travel)) {
        return false;
    }
    (void)length;
    state.logicalPosition =
        range_motion_detail::normalizedSelectionPosition(bounds, selection,
                                                          travel);
    state.pulseResidualNumerator = 0;
    return true;
}

// Seed a new physical baseline (for example on UI entry or modifier release)
// without treating the sample difference as motion or changing catch-up state.
inline bool seedRangeMotionPhysical(RangeMotionState& state,
                                    double physicalPosition) {
    if (!state.established) {
        return false;
    }
    state.physicalPosition =
        range_motion_detail::clampNormalized(physicalPosition);
    return true;
}

// Apply one genuine physical sample change. Returns true only when the integer
// selected pair moved; state still advances when sub-pulse movement is retained.
inline bool moveRangeMotion(RangeMotionState& state,
                            const RangeMotionBounds& bounds,
                            RangeMotionSelection& selection,
                            double newPhysicalPosition) {
    uint64_t length = 0;
    uint64_t travel = 0;
    if (!state.established ||
        !range_motion_detail::movableDomain(bounds, selection, length,
                                             travel)) {
        return false;
    }

    const double physical =
        range_motion_detail::clampNormalized(newPhysicalPosition);
    const double delta = physical - state.physicalPosition;
    if (delta == 0.0) {
        return false;
    }

    const double mismatch =
        state.physicalPosition - state.logicalPosition;
    const double correctionLimit =
        0.75 * range_motion_detail::absolute(delta);
    const double mismatchMagnitude =
        range_motion_detail::absolute(mismatch);
    const double correctionMagnitude =
        mismatchMagnitude < correctionLimit ? mismatchMagnitude
                                             : correctionLimit;
    const double correction = mismatch < 0.0 ? -correctionMagnitude
                                              : correctionMagnitude;
    const double nextLogical = range_motion_detail::clampNormalized(
        state.logicalPosition + delta + correction);
    const double normalizedMovement =
        nextLogical - state.logicalPosition;

    state.logicalPosition = nextLogical;
    state.physicalPosition = physical;

    if (normalizedMovement == 0.0) {
        return false;
    }

    const bool towardNewer = normalizedMovement > 0.0;
    const uint64_t available =
        towardNewer ? bounds.historyEnd - selection.end
                    : selection.start - bounds.historyStart;
    int64_t nextResidual = state.pulseResidualNumerator;
    const uint64_t amount = range_motion_detail::roundedScaledMovement(
        travel,
        range_motion_detail::normalizedMovementNumerator(
            range_motion_detail::absolute(normalizedMovement)),
        towardNewer, available, state.pulseResidualNumerator, nextResidual);

    if (amount != 0U) {
        if (towardNewer) {
            selection.start += amount;
        } else {
            selection.start -= amount;
        }
        // The validated length and clamped legal start make this addition
        // overflow-safe, even for domains adjacent to UINT64_MAX.
        selection.end = selection.start + length;
    }

    const bool reachedOutwardLimit =
        (towardNewer && selection.end == bounds.historyEnd) ||
        (!towardNewer && selection.start == bounds.historyStart);
    // Keep logical/physical mismatch, but never accumulate displacement debt
    // that would delay an inward reversal.
    state.pulseResidualNumerator = reachedOutwardLimit ? 0 : nextResidual;
    return amount != 0U;
}

} // namespace midibuffer

#endif
