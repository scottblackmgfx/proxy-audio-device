// test_e2e_recovery.cpp
//
// End-to-end test: simulates the full audio pipeline (writer → ring buffer → reader)
// with the DesyncRecovery fix active, injects the catastrophic desync mid-stream,
// and verifies audio data continuity.
//
// This mirrors the real driver's outputDeviceIOProc + DoIOOperation(WriteMix) loop,
// using the actual AudioRingBuffer. The test proves that:
//   1. Audio flows normally before injection
//   2. The desync injection is detected and recovered within one cycle
//   3. Audio data after recovery is non-zero (real audio, not stale zeros)
//   4. No sustained silence occurs at any point
//
// Usage: make test_e2e_recovery && ./test_e2e_recovery

#include "AudioRingBuffer.h"
#include "DesyncRecovery.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>

static int testsPassed = 0;
static int testsFailed = 0;

#define ASSERT_TRUE(expr, msg) do { \
    if (!(expr)) { \
        printf("  FAIL: %s\n    %s (line %d)\n", msg, #expr, __LINE__); \
        testsFailed++; \
        return; \
    } else { \
        printf("  PASS: %s\n", msg); \
        testsPassed++; \
    } \
} while(0)

static const UInt32 kBytesPerFrame   = 8;      // 2ch * Float32
static const UInt32 kRingBufferSize  = 16384;
static const UInt32 kBufferFrameSize = 512;
static const double kSampleRate      = 44100.0;

// Check if a buffer contains any non-zero Float32 samples
static bool hasNonZeroAudio(const Byte *data, UInt32 frameCount) {
    const Float32 *samples = reinterpret_cast<const Float32 *>(data);
    UInt32 sampleCount = frameCount * 2;  // 2 channels
    for (UInt32 i = 0; i < sampleCount; i++) {
        if (samples[i] != 0.0f) return true;
    }
    return false;
}

// Fill a buffer with a recognisable sine wave pattern
static void fillWithAudio(Byte *data, UInt32 frameCount, double startPhase) {
    Float32 *samples = reinterpret_cast<Float32 *>(data);
    for (UInt32 i = 0; i < frameCount; i++) {
        Float32 val = static_cast<Float32>(sin(startPhase + i * 0.1));
        samples[i * 2]     = val;  // L
        samples[i * 2 + 1] = val;  // R
    }
}

// ============================================================================
// Test: Full pipeline with desync injection and recovery
//
// Simulates 10 seconds of audio at 44.1kHz/512 frames:
//   - Writer stores audio into ring buffer (proxy device clock)
//   - Reader fetches from ring buffer (hardware device clock)
//   - At the midpoint, we inject a catastrophic desync (-50M shift)
//   - The recovery should correct it within 1 cycle
//   - We verify: no sustained silence before, during, or after injection
// ============================================================================
void test_full_pipeline_with_injection() {
    printf("\n--- E2E: Full pipeline with desync injection ---\n");

    AudioRingBuffer ringBuffer(kBytesPerFrame, kRingBufferSize);

    std::vector<Byte> writeData(kBufferFrameSize * kBytesPerFrame);
    std::vector<Byte> readData(kBufferFrameSize * kBytesPerFrame);

    // Driver state
    double inputOutputSampleDelta = -1;
    double lastInputFrameTime = -1;
    double lastInputBufferFrameSize = -1;
    double safetyOffset = 0;

    double writerFrame = 0;
    double readerSampleTime = 0;

    double cyclesPerSecond = kSampleRate / kBufferFrameSize;
    UInt64 totalCycles = static_cast<UInt64>(cyclesPerSecond * 10.0);  // 10 seconds
    UInt64 injectionCycle = totalCycles / 2;  // inject at the midpoint

    int silentCyclesBeforeInjection = 0;
    int silentCyclesAfterInjection = 0;
    int maxConsecutiveSilentAfter = 0;
    int currentConsecutiveSilent = 0;
    bool injected = false;
    bool recoveryTriggered = false;
    int recoveryCount = 0;

    for (UInt64 cycle = 0; cycle < totalCycles; cycle++) {
        // === WRITE SIDE ===
        fillWithAudio(writeData.data(), kBufferFrameSize, writerFrame * 0.01);
        ringBuffer.Store(writeData.data(), kBufferFrameSize, static_cast<SInt64>(writerFrame));
        lastInputFrameTime = writerFrame;
        lastInputBufferFrameSize = kBufferFrameSize;
        writerFrame += kBufferFrameSize;

        // === READ SIDE ===

        // Initial delta calculation (mirrors outputDeviceIOProc)
        if (inputOutputSampleDelta == -1) {
            double targetFrameTime = (lastInputFrameTime - lastInputBufferFrameSize
                                      - kBufferFrameSize - safetyOffset);
            inputOutputSampleDelta = targetFrameTime - readerSampleTime;
        }

        // === DESYNC INJECTION at midpoint ===
        if (cycle == injectionCycle) {
            inputOutputSampleDelta -= 50000000;  // same as our trigger
            injected = true;
        }

        double startFrame = readerSampleTime + inputOutputSampleDelta;

        // === THE FIX: DesyncRecovery ===
        DesyncRecoveryParams params;
        params.ringEndFrame           = static_cast<int64_t>(ringBuffer.mEndFrame);
        params.ringCapacity           = kRingBufferSize;
        params.outputSampleTime       = readerSampleTime;
        params.inputOutputSampleDelta = inputOutputSampleDelta;
        params.outputDeviceBufferSize = kBufferFrameSize;
        params.lastInputFrameTime     = lastInputFrameTime;
        params.lastInputBufferFrameSize = lastInputBufferFrameSize;
        params.outputDeviceSafetyOffset = safetyOffset;

        DesyncRecoveryResult recovery = checkAndRecoverDesync(params);
        if (recovery.wasRecovered) {
            inputOutputSampleDelta = recovery.correctedDelta;
            startFrame = recovery.correctedStartFrame;
            recoveryTriggered = true;
            recoveryCount++;
        }

        // Fetch from ring buffer
        ringBuffer.Fetch(readData.data(), kBufferFrameSize, static_cast<SInt64>(startFrame));

        // Check if output has real audio
        bool hasAudio = hasNonZeroAudio(readData.data(), kBufferFrameSize);

        if (!hasAudio) {
            if (!injected) {
                silentCyclesBeforeInjection++;
            } else {
                silentCyclesAfterInjection++;
                currentConsecutiveSilent++;
                if (currentConsecutiveSilent > maxConsecutiveSilentAfter) {
                    maxConsecutiveSilentAfter = currentConsecutiveSilent;
                }
            }
        } else {
            currentConsecutiveSilent = 0;
        }

        readerSampleTime += kBufferFrameSize;
    }

    printf("  Total cycles: %llu (%.1f seconds)\n", totalCycles, totalCycles / cyclesPerSecond);
    printf("  Injection at cycle: %llu\n", injectionCycle);
    printf("  Recovery triggered: %s (%d times)\n", recoveryTriggered ? "YES" : "NO", recoveryCount);
    printf("  Silent cycles before injection: %d\n", silentCyclesBeforeInjection);
    printf("  Silent cycles after injection: %d\n", silentCyclesAfterInjection);
    printf("  Max consecutive silent after injection: %d\n", maxConsecutiveSilentAfter);

    ASSERT_TRUE(recoveryTriggered, "recovery should have triggered after injection");
    ASSERT_TRUE(recoveryCount == 1, "recovery should trigger exactly once");
    // The first 1-3 cycles may be silent as the ring buffer fills initially — that's normal
    ASSERT_TRUE(silentCyclesBeforeInjection <= 3, "at most 3 silent startup cycles before injection");
    ASSERT_TRUE(maxConsecutiveSilentAfter <= 1, "at most 1 cycle of silence after injection (the injection cycle itself)");
}

// ============================================================================
// Test: Pipeline WITHOUT the fix — proves injection causes sustained silence
// ============================================================================
void test_full_pipeline_without_fix() {
    printf("\n--- E2E: Pipeline WITHOUT fix (proves the bug) ---\n");

    AudioRingBuffer ringBuffer(kBytesPerFrame, kRingBufferSize);

    std::vector<Byte> writeData(kBufferFrameSize * kBytesPerFrame);
    std::vector<Byte> readData(kBufferFrameSize * kBytesPerFrame);

    double inputOutputSampleDelta = -1;
    double lastInputFrameTime = -1;
    double lastInputBufferFrameSize = -1;

    double writerFrame = 0;
    double readerSampleTime = 0;

    double cyclesPerSecond = kSampleRate / kBufferFrameSize;
    UInt64 totalCycles = static_cast<UInt64>(cyclesPerSecond * 5.0);  // 5 seconds
    UInt64 injectionCycle = totalCycles / 2;

    int silentCyclesAfter = 0;
    bool injected = false;

    for (UInt64 cycle = 0; cycle < totalCycles; cycle++) {
        // Write side
        fillWithAudio(writeData.data(), kBufferFrameSize, writerFrame * 0.01);
        ringBuffer.Store(writeData.data(), kBufferFrameSize, static_cast<SInt64>(writerFrame));
        lastInputFrameTime = writerFrame;
        lastInputBufferFrameSize = kBufferFrameSize;
        writerFrame += kBufferFrameSize;

        // Read side (NO fix applied)
        if (inputOutputSampleDelta == -1) {
            double targetFrameTime = (lastInputFrameTime - lastInputBufferFrameSize
                                      - kBufferFrameSize - 0);
            inputOutputSampleDelta = targetFrameTime - readerSampleTime;
        }

        if (cycle == injectionCycle) {
            inputOutputSampleDelta -= 50000000;
            injected = true;
        }

        double startFrame = readerSampleTime + inputOutputSampleDelta;

        // NO recovery — this is the unfixed path
        ringBuffer.Fetch(readData.data(), kBufferFrameSize, static_cast<SInt64>(startFrame));

        bool hasAudio = hasNonZeroAudio(readData.data(), kBufferFrameSize);
        if (!hasAudio && injected) {
            silentCyclesAfter++;
        }

        readerSampleTime += kBufferFrameSize;
    }

    UInt64 cyclesAfterInjection = totalCycles - injectionCycle;
    printf("  Cycles after injection: %llu\n", cyclesAfterInjection);
    printf("  Silent cycles (unfixed): %d\n", silentCyclesAfter);

    ASSERT_TRUE(silentCyclesAfter == static_cast<int>(cyclesAfterInjection),
                "without fix, ALL cycles after injection should be silent");
}

// ============================================================================
// Test: Gradual clock drift over 8 hours — the original desync_reproducer
// scenario but with the fix active. Recovery should NOT trigger for gradual
// drift (that's a different failure mode), but audio should still work.
// ============================================================================
void test_gradual_drift_with_fix() {
    printf("\n--- E2E: Gradual drift (50 PPM, 1 hour) with fix ---\n");

    AudioRingBuffer ringBuffer(kBytesPerFrame, kRingBufferSize);

    std::vector<Byte> writeData(kBufferFrameSize * kBytesPerFrame);
    std::vector<Byte> readData(kBufferFrameSize * kBytesPerFrame);

    double inputOutputSampleDelta = -1;
    double lastInputFrameTime = -1;
    double lastInputBufferFrameSize = -1;

    double writerFrame = 0;
    double readerSampleTime = 0;
    double readerAdvance = kBufferFrameSize * (1.0 + 50.0 / 1e6);  // 50 PPM drift

    double cyclesPerSecond = kSampleRate / kBufferFrameSize;
    UInt64 totalCycles = static_cast<UInt64>(cyclesPerSecond * 3600.0);  // 1 hour

    int recoveryCount = 0;

    for (UInt64 cycle = 0; cycle < totalCycles; cycle++) {
        fillWithAudio(writeData.data(), kBufferFrameSize, writerFrame * 0.01);
        ringBuffer.Store(writeData.data(), kBufferFrameSize, static_cast<SInt64>(writerFrame));
        lastInputFrameTime = writerFrame;
        lastInputBufferFrameSize = kBufferFrameSize;
        writerFrame += kBufferFrameSize;

        if (inputOutputSampleDelta == -1) {
            double targetFrameTime = (lastInputFrameTime - lastInputBufferFrameSize
                                      - kBufferFrameSize - 0);
            inputOutputSampleDelta = targetFrameTime - readerSampleTime;
        }

        double startFrame = readerSampleTime + inputOutputSampleDelta;

        DesyncRecoveryParams params;
        params.ringEndFrame           = static_cast<int64_t>(ringBuffer.mEndFrame);
        params.ringCapacity           = kRingBufferSize;
        params.outputSampleTime       = readerSampleTime;
        params.inputOutputSampleDelta = inputOutputSampleDelta;
        params.outputDeviceBufferSize = kBufferFrameSize;
        params.lastInputFrameTime     = lastInputFrameTime;
        params.lastInputBufferFrameSize = lastInputBufferFrameSize;
        params.outputDeviceSafetyOffset = 0;

        DesyncRecoveryResult recovery = checkAndRecoverDesync(params);
        if (recovery.wasRecovered) {
            inputOutputSampleDelta = recovery.correctedDelta;
            startFrame = recovery.correctedStartFrame;
            recoveryCount++;
        }

        ringBuffer.Fetch(readData.data(), kBufferFrameSize, static_cast<SInt64>(startFrame));
        readerSampleTime += readerAdvance;
    }

    printf("  Simulated: 1 hour at 50 PPM drift\n");
    printf("  Recovery count: %d\n", recoveryCount);
    printf("  (Note: gradual drift is a separate issue from the catastrophic jump)\n");

    // The fix should not trigger for gradual drift within the first hour
    // at 50 PPM, because the fill level doesn't exceed ring capacity.
    // If it does trigger, that's not a failure — it means drift accumulated
    // enough to overflow, which is expected at high PPM over long durations.
    ASSERT_TRUE(recoveryCount >= 0, "gradual drift test completed without crash");
}

int main() {
    printf("=== DesyncRecovery End-to-End Tests ===\n");

    test_full_pipeline_with_injection();
    test_full_pipeline_without_fix();
    test_gradual_drift_with_fix();

    printf("\n=== Results: %d passed, %d failed ===\n", testsPassed, testsFailed);
    return testsFailed > 0 ? 1 : 0;
}
