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
    // Signed sub-pulse displacement carried between transitions.
    double pulseResidual;
    bool established;

    RangeMotionState()
        : logicalPosition(0.0), physicalPosition(0.0), pulseResidual(0.0),
          established(false) {}
};

struct RangeMotionBounds {
    uint64_t historyStart;
    uint64_t historyEnd;
};

struct RangeMotionSelection {
    uint64_t start;
    uint64_t end;
};

// Fixed-size state for one zoom-aware Start or End boundary pot. Movement is
// relative to the last physical sample; pulseResidual retains sub-pulse travel
// at the current viewport scale. offscreenSide records whether the boundary
// was last observed before (-1), inside (0), or after (1) the viewport so a
// newly offscreen boundary gets exactly one deliberate retrieval.
struct BoundaryMotionState {
    double physicalPosition;
    double pulseResidual;
    int8_t offscreenSide;
    bool offscreenRetrievalConsumed;
    bool established;

    explicit BoundaryMotionState(double physicalPositionValue = 0.0)
        : physicalPosition(physicalPositionValue), pulseResidual(0.0),
          offscreenSide(0), offscreenRetrievalConsumed(false),
          established(true) {}
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

struct WideUnsigned {
    uint64_t high;
    uint64_t low;
};

inline WideUnsigned multiplyUnsigned64By53(uint64_t left, uint64_t right) {
    const uint64_t leftLow = static_cast<uint32_t>(left);
    const uint64_t leftHigh = left >> 32U;
    const uint64_t rightLow = static_cast<uint32_t>(right);
    const uint64_t rightHigh = right >> 32U;
    const uint64_t lowLow = leftLow * rightLow;
    const uint64_t lowHigh = leftLow * rightHigh;
    const uint64_t highLow = leftHigh * rightLow;

    uint64_t low = lowLow;
    const uint64_t beforeLowHigh = low;
    low += lowHigh << 32U;
    const uint64_t carryLowHigh = low < beforeLowHigh ? 1U : 0U;
    const uint64_t beforeHighLow = low;
    low += highLow << 32U;
    const uint64_t carryHighLow = low < beforeHighLow ? 1U : 0U;

    WideUnsigned result = {
        leftHigh * rightHigh + (lowHigh >> 32U) + (highLow >> 32U) +
            carryLowHigh + carryHighLow,
        low,
    };
    return result;
}

inline uint64_t lowBitsMask(unsigned bits) {
    return bits == 0U ? 0U : (~static_cast<uint64_t>(0) >> (64U - bits));
}

inline double unsigned64ToDouble(uint64_t value) {
    // Convert exact 32-bit limbs rather than a potentially lossy absolute pulse
    // coordinate. This helper is used only for a sub-pulse remainder.
    return static_cast<double>(static_cast<uint32_t>(value >> 32U)) *
               4294967296.0 +
           static_cast<double>(static_cast<uint32_t>(value));
}

inline double scaleDownByPowerOfTwo(double value, unsigned power) {
    while (power >= 64U) {
        value *= 0x1p-64;
        power -= 64U;
    }
    while (power >= 16U) {
        value *= 0x1p-16;
        power -= 16U;
    }
    while (power != 0U) {
        value *= 0.5;
        --power;
    }
    return value;
}

inline double wideFraction(const WideUnsigned& product, unsigned shift) {
    WideUnsigned remainder = product;
    if (shift < 64U) {
        remainder.high = 0U;
        remainder.low &= lowBitsMask(shift);
    } else if (shift < 128U) {
        remainder.high &= lowBitsMask(shift - 64U);
    }

    const double asDouble =
        unsigned64ToDouble(remainder.high) * 18446744073709551616.0 +
        unsigned64ToDouble(remainder.low);
    const double fraction = scaleDownByPowerOfTwo(asDouble, shift);
    // Rounding while converting the remainder's limbs must not promote a
    // proper fraction to a second whole pulse.
    return fraction < 1.0 ? fraction : 0x1.fffffffffffffp-1;
}

// Scale the represented binary64 movement by a uint64 travel domain. The
// integer product is formed explicitly in two uint64 limbs, so neither travel
// nor an absolute pulse coordinate is narrowed to floating point.
inline void scaleTravel(uint64_t travel, double magnitude,
                        uint64_t& wholePulses, double& fractionalPulse) {
    if (!(magnitude > 0.0)) {
        wholePulses = 0U;
        fractionalPulse = 0.0;
        return;
    }
    if (magnitude >= 1.0) {
        wholePulses = travel;
        fractionalPulse = 0.0;
        return;
    }

    uint64_t representedBits = 0U;
    __builtin_memcpy(&representedBits, &magnitude, sizeof(representedBits));
    const unsigned exponentBits =
        static_cast<unsigned>((representedBits >> 52U) & 0x7ffU);
    uint64_t significand = representedBits & 0x000fffffffffffffULL;
    unsigned shift = 1074U;
    if (exponentBits != 0U) {
        significand |= 0x0010000000000000ULL;
        shift = 1075U - exponentBits;
    }

    const WideUnsigned product =
        multiplyUnsigned64By53(travel, significand);
    if (shift >= 128U) {
        wholePulses = 0U;
    } else if (shift >= 64U) {
        wholePulses = product.high >> (shift - 64U);
    } else {
        wholePulses = (product.high << (64U - shift)) |
                      (product.low >> shift);
    }
    fractionalPulse = wideFraction(product, shift);
}

inline uint64_t roundedScaledMovement(uint64_t travel, double magnitude,
                                      bool towardNewer, uint64_t available,
                                      double previousResidual,
                                      double& nextResidual) {
    if (!(magnitude > 0.0)) {
        nextResidual = previousResidual;
        return 0U;
    }

    uint64_t wholePulses = 0;
    double fractionalPulse = 0.0;
    scaleTravel(travel, magnitude, wholePulses, fractionalPulse);
    double combinedFraction =
        previousResidual +
        (towardNewer ? fractionalPulse : -fractionalPulse);

    // If the whole component reaches the legal limit, do not increment or
    // convert beyond uint64 range. The caller is at a clamp and discards debt.
    if (wholePulses >= available) {
        nextResidual = 0.0;
        return available;
    }

    if (towardNewer && combinedFraction >= 0.5) {
        ++wholePulses;
        combinedFraction -= 1.0;
    } else if (!towardNewer && combinedFraction <= -0.5) {
        ++wholePulses;
        combinedFraction += 1.0;
    }
    nextResidual = combinedFraction;
    return wholePulses;
}

} // namespace range_motion_detail

inline bool seedBoundaryMotionPhysical(BoundaryMotionState& state,
                                       double physicalPosition) {
    state.physicalPosition =
        range_motion_detail::clampNormalized(physicalPosition);
    state.pulseResidual = 0.0;
    state.established = true;
    return true;
}

// Apply one genuine physical change at viewport scale. An actual boundary
// outside the viewport is retrieved on the first changed sample only. The
// retrieval clamps the nearest viewport edge through the legal boundary domain
// so ordering and the caller's one-pulse minimum remain authoritative.
inline bool moveBoundaryMotion(BoundaryMotionState& state,
                               uint64_t legalMinimum,
                               uint64_t legalMaximum,
                               uint64_t viewStart,
                               uint64_t viewEnd,
                               uint64_t& boundary,
                               double newPhysicalPosition) {
    if (!state.established || legalMinimum > legalMaximum ||
        boundary < legalMinimum || boundary > legalMaximum ||
        viewEnd < viewStart) {
        return false;
    }

    const int8_t side = boundary < viewStart ? -1 :
                        (boundary > viewEnd ? 1 : 0);
    if (side != state.offscreenSide) {
        state.offscreenRetrievalConsumed = false;
        state.offscreenSide = side;
    }
    if (side == 0) {
        state.offscreenRetrievalConsumed = false;
    }

    const double physical =
        range_motion_detail::clampNormalized(newPhysicalPosition);
    const double delta = physical - state.physicalPosition;
    if (delta == 0.0) {
        return false;
    }
    state.physicalPosition = physical;

    if (side != 0 && !state.offscreenRetrievalConsumed) {
        uint64_t retrieved = side < 0 ? viewStart : viewEnd;
        if (retrieved < legalMinimum) {
            retrieved = legalMinimum;
        } else if (retrieved > legalMaximum) {
            retrieved = legalMaximum;
        }
        state.pulseResidual = 0.0;
        state.offscreenRetrievalConsumed = true;
        boundary = retrieved;
        state.offscreenSide = boundary < viewStart ? -1 :
                              (boundary > viewEnd ? 1 : 0);
        return true;
    }

    const uint64_t viewportSpan = viewEnd - viewStart;
    if (viewportSpan == 0U) {
        return false;
    }
    const bool towardNewer = delta > 0.0;
    const uint64_t available = towardNewer
                                   ? legalMaximum - boundary
                                   : boundary - legalMinimum;
    double nextResidual = state.pulseResidual;
    const uint64_t amount = range_motion_detail::roundedScaledMovement(
        viewportSpan, range_motion_detail::absolute(delta), towardNewer,
        available, state.pulseResidual, nextResidual);
    state.pulseResidual = nextResidual;
    if (amount == 0U) {
        return false;
    }
    boundary = towardNewer ? boundary + amount : boundary - amount;
    return true;
}

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
    state.pulseResidual = 0.0;
    state.established = true;
    return true;
}

// Report the selection-derived logical position without changing motion state.
// setupUi uses this to synchronize the host's normal (unpressed) pot function.
inline bool rangeMotionLogicalPosition(
    const RangeMotionBounds& bounds,
    const RangeMotionSelection& selection, double& logicalPosition) {
    uint64_t length = 0;
    uint64_t travel = 0;
    if (!range_motion_detail::movableDomain(bounds, selection, length,
                                             travel)) {
        return false;
    }
    (void)length;
    logicalPosition = range_motion_detail::normalizedSelectionPosition(
        bounds, selection, travel);
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
    state.pulseResidual = 0.0;
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
    double nextResidual = state.pulseResidual;
    const uint64_t amount = range_motion_detail::roundedScaledMovement(
        travel, range_motion_detail::absolute(normalizedMovement), towardNewer,
        available, state.pulseResidual, nextResidual);

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
    state.pulseResidual = reachedOutwardLimit ? 0.0 : nextResidual;
    return amount != 0U;
}

} // namespace midibuffer

#endif
