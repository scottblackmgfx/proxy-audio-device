// test_desync_recovery.cpp
//
// Unit tests for DesyncRecovery.h — the fix for the audio cutout bug.
//
// Tests checkAndRecoverDesync() with:
//   1. Real values from production logs (the actual failure observed)
//   2. Edge cases (zero capacity, boundary conditions, negative fill)
//   3. Normal operation (no recovery needed)
//
// Usage: make test_desync_recovery && ./test_desync_recovery

#include "DesyncRecovery.h"
#include <cstdio>
#include <cmath>
#include <cstdlib>

static int testsPassed = 0;
static int testsFailed = 0;

#define ASSERT_TRUE(expr, msg) do { \
    if (!(expr)) { \
        printf("  FAIL: %s\n    %s (line %d)\n", msg, #expr, __LINE__); \
        testsFailed++; \
    } else { \
        printf("  PASS: %s\n", msg); \
        testsPassed++; \
    } \
} while(0)

#define ASSERT_NEAR(a, b, tol, msg) do { \
    if (fabs((double)(a) - (double)(b)) > (tol)) { \
        printf("  FAIL: %s\n    expected %.1f, got %.1f (line %d)\n", msg, (double)(b), (double)(a), __LINE__); \
        testsFailed++; \
    } else { \
        printf("  PASS: %s\n", msg); \
        testsPassed++; \
    } \
} while(0)

// ============================================================================
// Test 1: Production failure — exact values from 2026-02-26 09:15 logs
//
// The real failure: fill jumped from 1609 to 47924021 with ring capacity 16384.
// The fix should detect this and recalculate the delta.
// ============================================================================
void test_production_failure() {
    printf("\n--- Production failure (from real logs) ---\n");

    DesyncRecoveryParams params;
    params.ringCapacity       = 16384;
    params.outputDeviceBufferSize = 512;
    params.outputDeviceSafetyOffset = 0;

    // Simulate the state just before SILENCE_STARTED:
    // fill=47924021, delta=-47602717, outputCycles=158
    // The write position (mEndFrame) is way ahead of the read position.
    params.lastInputFrameTime     = 770956;   // realistic input frame position
    params.lastInputBufferFrameSize = 512;
    params.outputSampleTime       = 47686129; // Scarlett's clock position
    params.inputOutputSampleDelta = -47602717;

    // mEndFrame such that fill = 47924021
    // fill = mEndFrame - (startFrame + bufferSize)
    // startFrame = 47686129 + (-47602717) = 83412
    // mEndFrame = 47924021 + 83412 + 512 = 48007945
    params.ringEndFrame = 48007945;

    DesyncRecoveryResult result = checkAndRecoverDesync(params);

    ASSERT_TRUE(result.wasRecovered, "should trigger recovery on fill=47924021");

    // After recovery, the corrected delta should place startFrame near the
    // latest input position (not the stale one). Verify the recalculated delta
    // matches the expected formula: (lastInput - inputBuf - outputBuf - safety) - outputTime
    Float64 expectedDelta = (params.lastInputFrameTime - params.lastInputBufferFrameSize
                             - static_cast<Float64>(params.outputDeviceBufferSize)
                             - params.outputDeviceSafetyOffset) - params.outputSampleTime;
    ASSERT_NEAR(result.correctedDelta, expectedDelta, 0.01,
                "recovered delta matches fresh recalculation");
}

// ============================================================================
// Test 2: Synthetic injection — matches our trigger (subtract 50M from delta)
// ============================================================================
void test_synthetic_injection() {
    printf("\n--- Synthetic injection (touch /tmp/ProxyAudioTriggerDesync) ---\n");

    DesyncRecoveryParams params;
    params.ringCapacity       = 16384;
    params.outputDeviceBufferSize = 512;
    params.outputDeviceSafetyOffset = 0;
    params.lastInputFrameTime     = 202612;
    params.lastInputBufferFrameSize = 512;
    params.outputSampleTime       = 405968;
    params.inputOutputSampleDelta = -405869;  // healthy delta

    // Healthy state first: startFrame = 405968 + (-405869) = 99
    // mEndFrame ≈ 99 + 512 + 1097 = 1708 (fill=1097, healthy)
    params.ringEndFrame = 1708;

    DesyncRecoveryResult healthyResult = checkAndRecoverDesync(params);
    ASSERT_TRUE(!healthyResult.wasRecovered, "should NOT trigger recovery when healthy (fill=1097)");

    // Now inject: subtract 50M from delta (same as our trigger)
    params.inputOutputSampleDelta -= 50000000;
    // startFrame = 405968 + (-50405869) = -49999901
    // fill = 1708 - (-49999901 + 512) = 50001097

    DesyncRecoveryResult injectedResult = checkAndRecoverDesync(params);
    ASSERT_TRUE(injectedResult.wasRecovered, "should trigger recovery after 50M injection");

    // Verify the corrected delta is sane (should be close to the original healthy delta)
    Float64 expectedDelta = (params.lastInputFrameTime - params.lastInputBufferFrameSize
                             - static_cast<Float64>(params.outputDeviceBufferSize)
                             - params.outputDeviceSafetyOffset) - params.outputSampleTime;
    ASSERT_NEAR(injectedResult.correctedDelta, expectedDelta, 0.01,
                "recovered delta matches fresh recalculation");
}

// ============================================================================
// Test 3: Normal operation — fill within bounds, no recovery
// ============================================================================
void test_normal_operation() {
    printf("\n--- Normal operation (various healthy fill levels) ---\n");

    DesyncRecoveryParams params;
    params.ringCapacity       = 16384;
    params.outputDeviceBufferSize = 512;
    params.outputDeviceSafetyOffset = 0;
    params.lastInputFrameTime     = 100000;
    params.lastInputBufferFrameSize = 512;

    // Test at various healthy fill levels
    int64_t fillLevels[] = {0, 1, 1097, 8192, 16383, 16384};
    for (int64_t targetFill : fillLevels) {
        params.outputSampleTime = 50000;
        params.inputOutputSampleDelta = -49000;
        // startFrame = 50000 + (-49000) = 1000
        // fill = mEndFrame - (1000 + 512) = mEndFrame - 1512
        params.ringEndFrame = 1512 + targetFill;

        DesyncRecoveryResult result = checkAndRecoverDesync(params);
        char msg[128];
        snprintf(msg, sizeof(msg), "no recovery at fill=%lld (within capacity)", targetFill);
        ASSERT_TRUE(!result.wasRecovered, msg);
    }
}

// ============================================================================
// Test 4: Just above capacity — should trigger
// ============================================================================
void test_boundary() {
    printf("\n--- Boundary: fill just above capacity ---\n");

    DesyncRecoveryParams params;
    params.ringCapacity       = 16384;
    params.outputDeviceBufferSize = 512;
    params.outputDeviceSafetyOffset = 0;
    params.lastInputFrameTime     = 100000;
    params.lastInputBufferFrameSize = 512;
    params.outputSampleTime       = 50000;
    params.inputOutputSampleDelta = -49000;
    // startFrame = 1000, fill = mEndFrame - 1512

    // fill = 16384 (exactly at capacity) — should NOT trigger
    params.ringEndFrame = 1512 + 16384;
    DesyncRecoveryResult atCapacity = checkAndRecoverDesync(params);
    ASSERT_TRUE(!atCapacity.wasRecovered, "no recovery at fill == capacity (boundary)");

    // fill = 16385 (one above capacity) — SHOULD trigger
    params.ringEndFrame = 1512 + 16385;
    DesyncRecoveryResult aboveCapacity = checkAndRecoverDesync(params);
    ASSERT_TRUE(aboveCapacity.wasRecovered, "recovery at fill == capacity + 1");
}

// ============================================================================
// Test 5: Negative fill — buffer draining, should NOT trigger
// ============================================================================
void test_negative_fill() {
    printf("\n--- Negative fill (buffer draining after input stops) ---\n");

    DesyncRecoveryParams params;
    params.ringCapacity       = 16384;
    params.outputDeviceBufferSize = 512;
    params.outputDeviceSafetyOffset = 0;
    params.lastInputFrameTime     = 100000;
    params.lastInputBufferFrameSize = 512;
    params.outputSampleTime       = 50000;
    params.inputOutputSampleDelta = -49000;
    // startFrame = 1000

    // mEndFrame behind startFrame — negative fill (draining)
    params.ringEndFrame = 500;  // fill = 500 - 1512 = -1012

    DesyncRecoveryResult result = checkAndRecoverDesync(params);
    ASSERT_TRUE(!result.wasRecovered, "no recovery on negative fill (normal drain)");
}

// ============================================================================
// Test 6: Zero capacity guard
// ============================================================================
void test_zero_capacity() {
    printf("\n--- Zero capacity guard ---\n");

    DesyncRecoveryParams params;
    params.ringCapacity       = 0;  // pathological
    params.outputDeviceBufferSize = 512;
    params.outputDeviceSafetyOffset = 0;
    params.lastInputFrameTime     = 100000;
    params.lastInputBufferFrameSize = 512;
    params.outputSampleTime       = 50000;
    params.inputOutputSampleDelta = -49000;
    params.ringEndFrame           = 100000;

    DesyncRecoveryResult result = checkAndRecoverDesync(params);
    ASSERT_TRUE(!result.wasRecovered, "no recovery when ringCapacity == 0 (guard)");
}

// ============================================================================
// Test 7: Recovery produces correct delta value
// ============================================================================
void test_recovery_delta_correctness() {
    printf("\n--- Recovery delta correctness ---\n");

    DesyncRecoveryParams params;
    params.ringCapacity       = 16384;
    params.outputDeviceBufferSize = 512;
    params.outputDeviceSafetyOffset = 0;
    params.lastInputFrameTime     = 100000;
    params.lastInputBufferFrameSize = 512;
    params.outputSampleTime       = 50000;
    params.inputOutputSampleDelta = -49000;  // will be corrupted
    params.ringEndFrame           = 999999999;  // massive overshoot

    DesyncRecoveryResult result = checkAndRecoverDesync(params);
    ASSERT_TRUE(result.wasRecovered, "recovery triggered");

    // Expected delta = (lastInputFrameTime - lastInputBufferFrameSize
    //                   - outputDeviceBufferSize - safetyOffset) - outputSampleTime
    //                = (100000 - 512 - 512 - 0) - 50000 = 48976
    Float64 expectedDelta = (100000.0 - 512.0 - 512.0 - 0.0) - 50000.0;
    ASSERT_NEAR(result.correctedDelta, expectedDelta, 0.01,
                "corrected delta matches expected recalculation");

    Float64 expectedStartFrame = 50000.0 + expectedDelta;
    ASSERT_NEAR(result.correctedStartFrame, expectedStartFrame, 0.01,
                "corrected startFrame matches expected value");
}

int main() {
    printf("=== DesyncRecovery Unit Tests ===\n");

    test_production_failure();
    test_synthetic_injection();
    test_normal_operation();
    test_boundary();
    test_negative_fill();
    test_zero_capacity();
    test_recovery_delta_correctness();

    printf("\n=== Results: %d passed, %d failed ===\n", testsPassed, testsFailed);
    return testsFailed > 0 ? 1 : 0;
}
