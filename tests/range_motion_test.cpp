#include "../src/range_motion.hpp"

#include <cstdio>
#include <stdint.h>

namespace {

int gFailures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++gFailures;
    }
}

void expectNear(double actual, double expected, double tolerance,
                const char* message) {
    const double difference = actual < expected ? expected - actual
                                                : actual - expected;
    expect(difference <= tolerance, message);
}

bool sameState(const midibuffer::RangeMotionState& left,
               const midibuffer::RangeMotionState& right) {
    return left.logicalPosition == right.logicalPosition &&
           left.physicalPosition == right.physicalPosition &&
           left.pulseResidual == right.pulseResidual &&
           left.established == right.established;
}

bool sameSelection(const midibuffer::RangeMotionSelection& left,
                   const midibuffer::RangeMotionSelection& right) {
    return left.start == right.start && left.end == right.end;
}

void verifyNormativeVectors() {
    const midibuffer::RangeMotionBounds bounds = {0U, 110U};

    midibuffer::RangeMotionSelection positive = {20U, 30U};
    midibuffer::RangeMotionState positiveState;
    expect(midibuffer::establishRangeMotion(positiveState, bounds, positive,
                                             0.8),
           "positive-offset vector establishes from selection");
    midibuffer::moveRangeMotion(positiveState, bounds, positive, 0.81);
    expectNear(positiveState.logicalPosition, 0.2175, 1.0e-12,
               "u=.2,p=.8,d=.01 produces uNext=.2175");

    midibuffer::RangeMotionSelection negative = {80U, 90U};
    midibuffer::RangeMotionState negativeState;
    expect(midibuffer::establishRangeMotion(negativeState, bounds, negative,
                                             0.2),
           "negative-offset vector establishes from selection");
    midibuffer::moveRangeMotion(negativeState, bounds, negative, 0.19);
    expectNear(negativeState.logicalPosition, 0.7825, 1.0e-12,
               "u=.8,p=.2,d=-.01 produces uNext=.7825");

    midibuffer::RangeMotionSelection opposing = {20U, 30U};
    midibuffer::RangeMotionState opposingState;
    midibuffer::establishRangeMotion(opposingState, bounds, opposing, 0.8);
    midibuffer::moveRangeMotion(opposingState, bounds, opposing, 0.79);
    expectNear(opposingState.logicalPosition, 0.1975, 1.0e-12,
               "u=.2,p=.8,d=-.01 produces uNext=.1975");
}

void verifyGainBoundsAndConvergence() {
    const midibuffer::RangeMotionBounds bounds = {0U, 1010U};
    struct GainCase {
        double physical;
        double nextPhysical;
        double expectedGain;
    };
    const GainCase cases[] = {
        {0.7, 0.74, 1.75},
        {0.7, 0.66, 0.25},
        {0.3, 0.34, 0.25},
        {0.3, 0.26, 1.75},
    };
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        midibuffer::RangeMotionSelection selection = {500U, 510U};
        midibuffer::RangeMotionState state;
        midibuffer::establishRangeMotion(state, bounds, selection,
                                          cases[index].physical);
        const double before = state.logicalPosition;
        const double delta =
            cases[index].nextPhysical - cases[index].physical;
        midibuffer::moveRangeMotion(state, bounds, selection,
                                    cases[index].nextPhysical);
        const double gain =
            (state.logicalPosition - before) / delta;
        expect(gain >= 0.25 - 1.0e-12 && gain <= 1.75 + 1.0e-12,
               "unclamped catch-up gain remains within .25..1.75");
        expectNear(gain, cases[index].expectedGain, 1.0e-12,
                   "catch-up gain matches offset and movement direction");
    }

    const midibuffer::RangeMotionBounds convergenceBounds = {0U, 110U};
    midibuffer::RangeMotionSelection selection = {20U, 30U};
    midibuffer::RangeMotionState state;
    midibuffer::establishRangeMotion(state, convergenceBounds, selection,
                                      0.8);
    double previousMismatch = 0.6;
    double physical = 0.8;
    double travel = 0.0;
    for (int step = 0; step < 8; ++step) {
        physical = physical == 0.8 ? 0.7 : 0.8;
        travel += 0.1;
        midibuffer::moveRangeMotion(state, convergenceBounds, selection,
                                    physical);
        const double mismatch = state.physicalPosition -
                                state.logicalPosition;
        const double magnitude = mismatch < 0.0 ? -mismatch : mismatch;
        expect(magnitude <= previousMismatch + 1.0e-12,
               "continued travel monotonically contracts mismatch");
        expectNear(previousMismatch - magnitude, 0.075, 1.0e-12,
                   "each convergence step contracts by .75 times travel");
        previousMismatch = magnitude;
    }
    expectNear(travel, 0.6 / 0.75, 1.0e-12,
               "convergence fixture supplies mismatch/.75 travel");
    expectNear(state.physicalPosition - state.logicalPosition, 0.0, 1.0e-12,
               "back-and-forth travel converges without a pickup snap");
}

void verifyResidualStationaryAndReversal() {
    const midibuffer::RangeMotionBounds bounds = {100U, 200U};
    midibuffer::RangeMotionSelection selection = {120U, 140U};
    midibuffer::RangeMotionState state;
    expect(midibuffer::establishRangeMotion(state, bounds, selection, 0.25),
           "range establishes at selection-derived logical position");
    expectNear(state.logicalPosition, 0.25, 1.0e-12,
               "establishment ignores absolute physical position for logical u");

    for (int step = 1; step <= 6; ++step) {
        expect(!midibuffer::moveRangeMotion(
                   state, bounds, selection, 0.25 + step * 0.001),
               "sub-pulse input is retained before rounding threshold");
    }
    expect(selection.start == 120U && selection.end == 140U,
           "tiny deltas do not fabricate early whole-pulse motion");
    expect(state.pulseResidual != 0.0,
           "sub-pulse displacement remains in fixed-size state");
    expect(midibuffer::moveRangeMotion(state, bounds, selection, 0.257),
           "accumulated tiny deltas eventually move one pulse");
    expect(selection.start == 121U && selection.end == 141U,
           "rounded accumulated displacement preserves exact length");

    const midibuffer::RangeMotionState stationaryState = state;
    const midibuffer::RangeMotionSelection stationarySelection = selection;
    expect(!midibuffer::moveRangeMotion(state, bounds, selection, 0.257),
           "no physical delta produces no movement");
    expect(sameState(state, stationaryState) &&
               sameSelection(selection, stationarySelection),
           "stationary input changes neither motion state nor selection");

    midibuffer::RangeMotionSelection reverseSelection = {140U, 150U};
    const midibuffer::RangeMotionBounds reverseBounds = {0U, 1010U};
    midibuffer::RangeMotionState reverseState;
    midibuffer::establishRangeMotion(reverseState, reverseBounds,
                                      reverseSelection, 0.7);
    const uint64_t beforeReverse = reverseSelection.start;
    expect(midibuffer::moveRangeMotion(reverseState, reverseBounds,
                                        reverseSelection, 0.69) &&
               reverseSelection.start < beforeReverse,
           "physical reversal immediately reverses integer output");
    expect(reverseState.physicalPosition > reverseState.logicalPosition,
           "reversal retains unfinished positive mismatch");
}

void verifyClampsAndEndpointMismatch() {
    const midibuffer::RangeMotionBounds bounds = {100U, 200U};

    midibuffer::RangeMotionSelection upper = {180U, 200U};
    midibuffer::RangeMotionState upperState;
    midibuffer::establishRangeMotion(upperState, bounds, upper, 0.8);
    expect(!midibuffer::moveRangeMotion(upperState, bounds, upper, 0.9) &&
               upper.start == 180U && upper.end == 200U,
           "outward motion at upper clamp preserves the legal pair");
    expectNear(upperState.logicalPosition, 1.0, 1.0e-12,
               "upper clamp keeps normalized logical limit");
    expectNear(upperState.physicalPosition - upperState.logicalPosition,
               -0.1, 1.0e-12,
               "upper clamp preserves physical/logical mismatch");
    expect(upperState.pulseResidual == 0.0,
           "upper clamp discards outward displacement debt");
    expect(midibuffer::moveRangeMotion(upperState, bounds, upper, 0.8) &&
               upper.start < 180U && upper.end - upper.start == 20U,
           "upper-clamp inward reversal responds immediately");

    midibuffer::RangeMotionSelection lower = {100U, 120U};
    midibuffer::RangeMotionState lowerState;
    midibuffer::establishRangeMotion(lowerState, bounds, lower, 0.2);
    expect(!midibuffer::moveRangeMotion(lowerState, bounds, lower, 0.1) &&
               lower.start == 100U && lower.end == 120U,
           "outward motion at lower clamp preserves the legal pair");
    expectNear(lowerState.physicalPosition - lowerState.logicalPosition,
               0.1, 1.0e-12,
               "lower clamp preserves physical/logical mismatch");
    expect(lowerState.pulseResidual == 0.0 &&
               midibuffer::moveRangeMotion(lowerState, bounds, lower, 0.2) &&
               lower.start > 100U && lower.end - lower.start == 20U,
           "lower-clamp inward reversal has no displacement debt delay");

    midibuffer::RangeMotionSelection endpoint = {120U, 140U};
    midibuffer::RangeMotionState endpointState;
    midibuffer::establishRangeMotion(endpointState, bounds, endpoint, 0.8);
    midibuffer::moveRangeMotion(endpointState, bounds, endpoint, 1.0);
    expectNear(endpointState.logicalPosition, 0.6, 1.0e-12,
               "arrival at a physical endpoint does not force synchronization");
    const midibuffer::RangeMotionState atEndpoint = endpointState;
    midibuffer::moveRangeMotion(endpointState, bounds, endpoint, 1.0);
    expect(sameState(endpointState, atEndpoint),
           "stationary physical endpoint performs no catch-up");
}

void verifyEstablishRebaseAndNoOpDomains() {
    const midibuffer::RangeMotionBounds bounds = {100U, 200U};
    midibuffer::RangeMotionSelection selection = {120U, 140U};
    midibuffer::RangeMotionState state;
    midibuffer::establishRangeMotion(state, bounds, selection, 0.8);
    midibuffer::moveRangeMotion(state, bounds, selection, 0.81);
    const double retainedLogical = state.logicalPosition;
    const double retainedResidual = state.pulseResidual;
    expect(midibuffer::seedRangeMotionPhysical(state, 0.4) &&
               state.physicalPosition == 0.4 &&
               state.logicalPosition == retainedLogical &&
               state.pulseResidual == retainedResidual,
           "physical baseline seeding does not become range motion or restart catch-up");
    const double retainedPhysical = state.physicalPosition;

    selection.start = 150U;
    selection.end = 170U;
    expect(midibuffer::rebaseRangeMotion(state, bounds, selection),
           "effective external edit rebases range state");
    expectNear(state.logicalPosition, 0.625, 1.0e-12,
               "rebase derives logical position from edited selection");
    expect(state.physicalPosition == retainedPhysical &&
               state.pulseResidual == 0.0,
           "rebase retains physical sample and clears stale residual");

    midibuffer::RangeMotionState unestablished;
    expect(!midibuffer::seedRangeMotionPhysical(unestablished, 0.5),
           "physical baseline cannot seed absent range state");

    const midibuffer::RangeMotionState established = state;
    const midibuffer::RangeMotionSelection establishedSelection = selection;
    const midibuffer::RangeMotionBounds empty = {100U, 100U};
    const midibuffer::RangeMotionSelection invalid = {130U, 120U};
    const midibuffer::RangeMotionSelection full = {100U, 200U};
    expect(!midibuffer::establishRangeMotion(state, empty, selection, 0.0) &&
               sameState(state, established),
           "empty retained history is an establishment no-op");
    expect(!midibuffer::rebaseRangeMotion(state, bounds, invalid) &&
               sameState(state, established),
           "invalid selection is a rebase no-op");

    midibuffer::RangeMotionSelection movingInvalid = invalid;
    expect(!midibuffer::moveRangeMotion(state, bounds, movingInvalid, 0.2) &&
               sameState(state, established) &&
               sameSelection(movingInvalid, invalid),
           "invalid range movement changes no state or coordinates");
    midibuffer::RangeMotionSelection movingFull = full;
    expect(!midibuffer::moveRangeMotion(state, bounds, movingFull, 0.2) &&
               sameState(state, established) &&
               sameSelection(movingFull, full),
           "full-history range movement is a no-op");
    expect(sameSelection(selection, establishedSelection),
           "domain no-op checks do not alter the valid fixture");
}

void verifyExactWideDisplacementAndLengthInvariant() {
    const uint64_t halfTravel = static_cast<uint64_t>(1) << 39U;
    const uint64_t travel = static_cast<uint64_t>(1) << 40U;
    const midibuffer::RangeMotionBounds wide = {0U, travel + 1U};

    midibuffer::RangeMotionSelection positive = {0U, 1U};
    midibuffer::RangeMotionState positiveState;
    midibuffer::establishRangeMotion(positiveState, wide, positive, 0.0);
    expect(midibuffer::moveRangeMotion(positiveState, wide, positive, 0.5) &&
               positive.start == halfTravel &&
               positive.end == halfTravel + 1U,
           "D=2^40 times positive 0.5 moves exactly 2^39 pulses");

    midibuffer::RangeMotionSelection negative = {travel, travel + 1U};
    midibuffer::RangeMotionState negativeState;
    midibuffer::establishRangeMotion(negativeState, wide, negative, 1.0);
    expect(midibuffer::moveRangeMotion(negativeState, wide, negative, 0.5) &&
               negative.start == halfTravel &&
               negative.end == halfTravel + 1U,
           "D=2^40 times negative 0.5 moves exactly 2^39 pulses");

    const double quarterPulse = 0x1p-42;
    midibuffer::RangeMotionSelection tiny = {0U, 1U};
    midibuffer::RangeMotionState tinyState;
    midibuffer::establishRangeMotion(tinyState, wide, tiny, 0.0);
    expect(!midibuffer::moveRangeMotion(tinyState, wide, tiny,
                                        quarterPulse) &&
               tiny.start == 0U && tinyState.pulseResidual == 0.25,
           "quarter-pulse movement is retained without early motion");
    midibuffer::moveRangeMotion(tinyState, wide, tiny, 2.0 * quarterPulse);
    midibuffer::moveRangeMotion(tinyState, wide, tiny, 3.0 * quarterPulse);
    midibuffer::moveRangeMotion(tinyState, wide, tiny, 4.0 * quarterPulse);
    expect(tiny.start == 1U && tiny.end == 2U &&
               tinyState.pulseResidual == 0.0,
           "four tiny represented movements accumulate to one exact pulse");

    midibuffer::RangeMotionSelection tinyNegative = {travel, travel + 1U};
    midibuffer::RangeMotionState tinyNegativeState;
    midibuffer::establishRangeMotion(tinyNegativeState, wide, tinyNegative,
                                      1.0);
    expect(!midibuffer::moveRangeMotion(tinyNegativeState, wide,
                                         tinyNegative,
                                         1.0 - quarterPulse) &&
               tinyNegative.start == travel &&
               tinyNegativeState.pulseResidual == -0.25,
           "negative quarter-pulse movement is retained without early motion");
    midibuffer::moveRangeMotion(tinyNegativeState, wide, tinyNegative,
                                1.0 - 2.0 * quarterPulse);
    midibuffer::moveRangeMotion(tinyNegativeState, wide, tinyNegative,
                                1.0 - 3.0 * quarterPulse);
    midibuffer::moveRangeMotion(tinyNegativeState, wide, tinyNegative,
                                1.0 - 4.0 * quarterPulse);
    expect(tinyNegative.start == travel - 1U &&
               tinyNegative.end == travel &&
               tinyNegativeState.pulseResidual == 0.0,
           "four tiny negative movements accumulate to one exact pulse");

    const uint64_t maximum = ~static_cast<uint64_t>(0);
    const midibuffer::RangeMotionBounds nearMaximum = {
        maximum - (travel + 1U), maximum,
    };
    midibuffer::RangeMotionSelection nearMaximumSelection = {
        nearMaximum.historyStart, nearMaximum.historyStart + 1U,
    };
    midibuffer::RangeMotionState nearMaximumState;
    midibuffer::establishRangeMotion(nearMaximumState, nearMaximum,
                                      nearMaximumSelection, 0.0);
    expect(midibuffer::moveRangeMotion(nearMaximumState, nearMaximum,
                                        nearMaximumSelection, 0.5) &&
               nearMaximumSelection.start ==
                   nearMaximum.historyStart + halfTravel &&
               nearMaximumSelection.end ==
                   nearMaximum.historyStart + halfTravel + 1U,
           "near-UINT64 coordinates retain exact relative displacement");

    const midibuffer::RangeMotionBounds widest = {0U, maximum};
    midibuffer::RangeMotionSelection onePulse = {0U, 1U};
    midibuffer::RangeMotionState widestState;
    midibuffer::establishRangeMotion(widestState, widest, onePulse, 0.0);
    midibuffer::moveRangeMotion(widestState, widest, onePulse, 0.5);
    const uint64_t expectedWidestHalf =
        (maximum - 1U) / 2U;
    expect(onePulse.start == expectedWidestHalf &&
               onePulse.end == expectedWidestHalf + 1U,
           "nearly full uint64 travel has exact half-range displacement");

    const double samples[] = {1.0, 0.0, 0.73, 0.21, 0.99, 0.01, 0.5};
    for (size_t index = 0; index < sizeof(samples) / sizeof(samples[0]);
         ++index) {
        midibuffer::moveRangeMotion(widestState, widest, onePulse,
                                    samples[index]);
        expect(onePulse.start >= widest.historyStart &&
                   onePulse.end <= widest.historyEnd &&
                   onePulse.start < onePulse.end &&
                   onePulse.end - onePulse.start == 1U,
               "wide-coordinate motion always preserves a legal fixed-length pair");
    }
}

} // namespace

int main() {
    static_assert(sizeof(midibuffer::RangeMotionState) <= 32U,
                  "range motion bookkeeping must remain fixed and compact");
    verifyNormativeVectors();
    verifyGainBoundsAndConvergence();
    verifyResidualStationaryAndReversal();
    verifyClampsAndEndpointMismatch();
    verifyEstablishRebaseAndNoOpDomains();
    verifyExactWideDisplacementAndLengthInvariant();

    if (gFailures != 0) {
        std::fprintf(stderr, "%d range motion checks failed\n", gFailures);
        return 1;
    }
    std::printf("PASS: fixed-size range motion arithmetic and state transitions\n");
    return 0;
}
