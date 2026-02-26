// desync_reproducer.cpp
//
// Standalone test that reproduces the ring buffer desync bug in proxy-audio-device
// WITHOUT requiring any audio hardware. Simulates the two independent clocks
// (proxy device clock vs hardware device clock) and detects when the ring buffer
// underruns — the exact failure mode that causes audio to cut out.
//
// Usage: make && ./desync_reproducer
//
// The test runs in accelerated time, simulating hours of playback in seconds.

#include "AudioRingBuffer.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

// Match the real driver's constants
static const UInt32 kBytesPerFrame = 8;       // 2 channels * 4 bytes (Float32)
static const UInt32 kRingBufferSize = 16384;  // kDevice_RingBufferSize from ProxyAudioDevice.h

struct SimulationConfig {
    double sampleRate;         // e.g. 48000.0
    UInt32 bufferFrameSize;    // IO buffer size in frames (e.g. 512)
    double driftPPM;           // clock drift in parts per million (positive = reader faster)
    double durationSeconds;    // how long to simulate
    UInt32 safetyOffset;       // output device safety offset (usually 0)
};

struct SimulationResult {
    bool desyncOccurred;
    double timeToDesyncSeconds;    // -1 if no desync
    SInt64 minFillLevel;           // lowest buffer fill observed
    SInt64 maxFillLevel;           // highest buffer fill observed
    UInt64 totalOverrunCycles;     // total IO cycles where Fetch returned overrun
    UInt64 totalCycles;            // total IO cycles simulated
};

// Simulates the exact logic from outputDeviceIOProc and DoIOOperation(WriteMix)
// with two clocks running at slightly different rates.
SimulationResult simulateDesync(const SimulationConfig &config) {
    AudioRingBuffer ringBuffer(kBytesPerFrame, kRingBufferSize);

    // Work buffers (filled with a recognisable pattern so we can detect silence)
    std::vector<Byte> writeData(config.bufferFrameSize * kBytesPerFrame, 0);
    std::vector<Byte> readData(config.bufferFrameSize * kBytesPerFrame, 0);

    // Fill write data with non-zero pattern (simulates actual audio)
    for (size_t i = 0; i < writeData.size(); i += sizeof(Float32)) {
        Float32 val = 0.5f;
        memcpy(&writeData[i], &val, sizeof(Float32));
    }

    // State variables mirroring the real driver
    double inputOutputSampleDelta = -1;
    double lastInputFrameTime = -1;
    double lastInputBufferFrameSize = -1;

    SimulationResult result = {};
    result.desyncOccurred = false;
    result.timeToDesyncSeconds = -1;
    result.minFillLevel = kRingBufferSize;
    result.maxFillLevel = 0;

    // Calculate total IO cycles to simulate
    // Each cycle processes bufferFrameSize frames
    double framesPerSecond = config.sampleRate;
    double cyclesPerSecond = framesPerSecond / config.bufferFrameSize;
    UInt64 totalCycles = (UInt64)(cyclesPerSecond * config.durationSeconds);
    result.totalCycles = totalCycles;

    // The two clocks:
    // - writerFrame: proxy device clock (nominal rate)
    // - readerSampleTime: hardware device clock (drifted rate)
    // Both start at 0 and advance by bufferFrameSize each cycle,
    // but the reader advances slightly faster/slower due to drift.
    double writerFrame = 0;
    double readerSampleTime = 0;

    // The drift factor: reader's clock runs at (1 + driftPPM/1e6) * nominal rate
    double readerAdvancePerCycle = config.bufferFrameSize * (1.0 + config.driftPPM / 1e6);

    int consecutiveOverruns = 0;
    const int kDesyncThreshold = 3;  // 3 consecutive overruns = desync event

    for (UInt64 cycle = 0; cycle < totalCycles; cycle++) {
        // === WRITE SIDE (DoIOOperation WriteMix) ===
        // This mirrors line 5289: inputBuffer->Store(data, frameSize, mOutputTime.mSampleTime)
        // The writer uses the proxy device's clock (nominal, no drift)
        ringBuffer.Store(writeData.data(), config.bufferFrameSize, (SInt64)writerFrame);
        lastInputFrameTime = writerFrame;
        lastInputBufferFrameSize = config.bufferFrameSize;
        writerFrame += config.bufferFrameSize;

        // === READ SIDE (outputDeviceIOProc) ===
        // This mirrors lines 5369-5383

        // Calculate inputOutputSampleDelta exactly once (line 5369-5375)
        if (inputOutputSampleDelta == -1) {
            double targetFrameTime = (lastInputFrameTime - lastInputBufferFrameSize
                                      - config.bufferFrameSize - config.safetyOffset);
            inputOutputSampleDelta = targetFrameTime - readerSampleTime;
        }

        // Calculate read position (line 5377)
        double startFrame = readerSampleTime + inputOutputSampleDelta;

        // Fetch from ring buffer (line 5383)
        bool overrun = ringBuffer.Fetch(readData.data(), config.bufferFrameSize, (SInt64)startFrame);

        // Track buffer fill level
        SInt64 fillLevel = ringBuffer.mEndFrame - ((SInt64)startFrame + config.bufferFrameSize);
        if (fillLevel < result.minFillLevel) result.minFillLevel = fillLevel;
        if (fillLevel > result.maxFillLevel) result.maxFillLevel = fillLevel;

        // Detect desync: sustained overrun where we're reading past the written data
        if (overrun && startFrame >= ringBuffer.mStartFrame) {
            result.totalOverrunCycles++;
            consecutiveOverruns++;

            if (consecutiveOverruns >= kDesyncThreshold && !result.desyncOccurred) {
                result.desyncOccurred = true;
                result.timeToDesyncSeconds = (double)cycle / cyclesPerSecond;
            }
        } else {
            consecutiveOverruns = 0;
        }

        // Reader advances at the drifted rate
        readerSampleTime += readerAdvancePerCycle;
    }

    return result;
}

void printResult(const SimulationConfig &config, const SimulationResult &result) {
    printf("  Drift: %+7.1f PPM | ", config.driftPPM);

    if (result.desyncOccurred) {
        double minutes = result.timeToDesyncSeconds / 60.0;
        if (minutes < 60.0) {
            printf("DESYNC at %6.1f min", minutes);
        } else {
            printf("DESYNC at %5.1f hrs", minutes / 60.0);
        }
    } else {
        printf("OK (no desync)      ");
    }

    printf(" | fill [%6lld, %6lld] | overruns: %llu/%llu cycles\n",
           result.minFillLevel, result.maxFillLevel,
           result.totalOverrunCycles, result.totalCycles);
}

int main() {
    printf("=== Proxy Audio Device: Ring Buffer Desync Reproducer ===\n\n");
    printf("Simulates two independent clocks (proxy vs hardware) with the real\n");
    printf("AudioRingBuffer implementation to reproduce the desync that causes\n");
    printf("audio dropout. Ring buffer: %u frames. No audio hardware required.\n\n", kRingBufferSize);

    // Test matrix: various drift rates at common sample rates and buffer sizes
    struct TestCase {
        double sampleRate;
        UInt32 bufferFrameSize;
        const char *label;
    };

    TestCase cases[] = {
        {48000.0, 512, "48kHz / 512 frames"},
        {44100.0, 512, "44.1kHz / 512 frames"},
        {48000.0, 128, "48kHz / 128 frames"},
        {48000.0, 1024, "48kHz / 1024 frames"},
    };

    double driftRates[] = {1.0, 5.0, 10.0, 25.0, 50.0, 100.0, -10.0, -50.0};
    double simDuration = 8.0 * 3600.0;  // Simulate 8 hours

    for (auto &tc : cases) {
        printf("--- %s (simulating %.0f hours) ---\n", tc.label, simDuration / 3600.0);

        for (double drift : driftRates) {
            SimulationConfig config = {};
            config.sampleRate = tc.sampleRate;
            config.bufferFrameSize = tc.bufferFrameSize;
            config.driftPPM = drift;
            config.durationSeconds = simDuration;
            config.safetyOffset = 0;

            SimulationResult result = simulateDesync(config);
            printResult(config, result);
        }
        printf("\n");
    }

    // Summary
    printf("=== Interpretation ===\n");
    printf("DESYNC = the moment audio would cut out (ring buffer underrun).\n");
    printf("fill [min, max] = range of buffer fill levels observed (frames).\n");
    printf("  - Healthy: fill stays near %u (half buffer).\n", kRingBufferSize / 2);
    printf("  - Failing: min fill reaches 0 or negative = underrun.\n");
    printf("overruns = IO cycles where Fetch read past available data.\n");
    printf("\nUSB clock drift is typically 10-50 PPM. Any drift > 0 will eventually\n");
    printf("cause desync with the current code — it's a question of when, not if.\n");

    return 0;
}
