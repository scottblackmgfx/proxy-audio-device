// DesyncRecovery.h
//
// Fix for the audio cutout bug (GitHub issues #14, #19, #43, #62).
//
// Problem: inputOutputSampleDelta is computed once per IO session and never
// adjusted. If the proxy device's sample time epoch shifts (IO restart,
// GetZeroTimeStamp recalculation), the ring buffer's write position (mEndFrame)
// jumps relative to the read position (startFrame), causing fill levels millions
// of frames beyond the 16384-frame ring buffer. The output proc reads stale
// zeros — audio cuts out silently.
//
// Fix: Before each Fetch, check the ring buffer fill level. If it's outside
// valid bounds, recalculate inputOutputSampleDelta on the spot and recompute
// startFrame. The Fetch then reads from the correct position.
//
// See: https://github.com/briankendall/proxy-audio-device/issues/19

#ifndef DesyncRecovery_h
#define DesyncRecovery_h

#include <cstdint>
#include <CoreAudio/CoreAudio.h>

struct DesyncRecoveryParams {
    // Ring buffer state (read-only)
    int64_t  ringEndFrame;
    int64_t  ringCapacity;       // must be > 0

    // Current read position inputs
    Float64  outputSampleTime;         // inOutputTime->mSampleTime
    Float64  inputOutputSampleDelta;   // current delta (will be corrected if stale)
    int64_t  outputDeviceBufferSize;   // currentOutputDeviceBufferFrameSize

    // Input position (for recalculation)
    Float64  lastInputFrameTime;
    Float64  lastInputBufferFrameSize;
    Float64  outputDeviceSafetyOffset;
};

struct DesyncRecoveryResult {
    Float64  correctedDelta;     // possibly updated inputOutputSampleDelta
    Float64  correctedStartFrame;
    bool     wasRecovered;       // true if delta was stale and got corrected
};

// Check whether the current read position is within the ring buffer's valid
// range. If not, recalculate the delta immediately so Fetch reads real data.
//
// Called from outputDeviceIOProc on every cycle. The check is a single integer
// comparison — effectively free on the hot path.
//
// Only triggers on fillLevel > ringCapacity (write position jumped way ahead
// of read position). Negative fill (buffer draining after input stops) is
// normal and handled by the existing overrun/early-return paths.
//
// Preconditions:
//   - ringCapacity > 0
//   - All Float64 fields are finite (not NaN/Inf)
//   - Audio sample times are well within int64_t range (~9.2e18)
static inline DesyncRecoveryResult checkAndRecoverDesync(const DesyncRecoveryParams &p) {
    DesyncRecoveryResult result;
    result.correctedDelta = p.inputOutputSampleDelta;
    result.correctedStartFrame = p.outputSampleTime + p.inputOutputSampleDelta;
    result.wasRecovered = false;

    // Guard: ringCapacity must be positive to avoid false triggers
    if (p.ringCapacity <= 0) return result;

    // Safe to cast: audio sample times are bounded well within int64_t range
    int64_t fillLevel = p.ringEndFrame
                      - (static_cast<int64_t>(result.correctedStartFrame) + p.outputDeviceBufferSize);

    if (fillLevel > p.ringCapacity) {
        // Delta is stale — recalculate from current input position
        Float64 targetFrameTime = (p.lastInputFrameTime
                                   - p.lastInputBufferFrameSize
                                   - static_cast<Float64>(p.outputDeviceBufferSize)
                                   - p.outputDeviceSafetyOffset);
        result.correctedDelta = targetFrameTime - p.outputSampleTime;
        result.correctedStartFrame = p.outputSampleTime + result.correctedDelta;
        result.wasRecovered = true;
    }

    return result;
}

#endif /* DesyncRecovery_h */
