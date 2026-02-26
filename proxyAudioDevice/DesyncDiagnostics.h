// DesyncDiagnostics.h
//
// Event-driven diagnostics for the audio cutout bug.
// Only logs when something meaningful CHANGES — no periodic noise.
//
// Designed for real-time audio: the hot path (outputDeviceIOProc) only touches
// atomic variables. File I/O happens on a separate dispatch queue, triggered
// by state transitions on the hot path.
//
// Log file: /tmp/ProxyAudioDiagnostics.log

#ifndef DesyncDiagnostics_h
#define DesyncDiagnostics_h

#include <atomic>
#include <cstdio>
#include <ctime>
#include <cmath>
#include <cstring>
#include <dispatch/dispatch.h>
#include <syslog.h>
#include <unistd.h>

// ============================================================================
// Compile-time toggle — set to 0 to strip all diagnostics from the build
// ============================================================================
#define DESYNC_DIAGNOSTICS_ENABLED 0

// The code path taken by outputDeviceIOProc on each cycle
enum class OutputProcPath : int {
    unknown = 0,
    earlyReturn_noInputData,     // lastInputFrameTime < 0 (input was reset)
    earlyReturn_sampleRateMismatch,
    earlyReturn_pastFinalFrame,  // inputFinalFrameTime check
    normal_fetchOK,              // Fetch succeeded, data written to output
    normal_fetchOverrun,         // Fetch returned overrun flag
};

static const char* pathName(OutputProcPath p) {
    switch (p) {
        case OutputProcPath::unknown:                      return "unknown";
        case OutputProcPath::earlyReturn_noInputData:      return "EARLY_RETURN:no_input_data";
        case OutputProcPath::earlyReturn_sampleRateMismatch: return "EARLY_RETURN:sample_rate_mismatch";
        case OutputProcPath::earlyReturn_pastFinalFrame:   return "EARLY_RETURN:past_final_frame";
        case OutputProcPath::normal_fetchOK:               return "normal";
        case OutputProcPath::normal_fetchOverrun:          return "OVERRUN";
    }
    return "?";
}

class DesyncDiagnostics {
public:

    // ========================================================================
    // Initialise logging — call once at driver startup
    // ========================================================================
    void start(double sampleRate, uint32_t bufferFrameSize, uint32_t ringBufferCapacity) {
        if (logFile) return;

        nominalSampleRate = sampleRate;
        nominalBufferSize = bufferFrameSize;
        ringCapacity = ringBufferCapacity;

        logFile = fopen("/tmp/ProxyAudioDiagnostics.log", "a");
        if (!logFile) {
            syslog(LOG_ERR, "ProxyAudio: failed to open diagnostics log");
            return;
        }

        logQueue = dispatch_queue_create("net.briankendall.ProxyAudioDiag",
                                          DISPATCH_QUEUE_SERIAL);

        writeLog("=== STARTED === sampleRate=%.0f bufferSize=%u ringCapacity=%u",
                 sampleRate, bufferFrameSize, ringBufferCapacity);
    }

    void stop() {
        if (logFile) {
            writeLog("=== STOPPED === totalOutputCycles=%llu totalInputCycles=%llu "
                     "totalOverruns=%llu",
                     outputCycles.load(std::memory_order_relaxed),
                     inputCycles.load(std::memory_order_relaxed),
                     overrunCount.load(std::memory_order_relaxed));
            fclose(logFile);
            logFile = nullptr;
        }
        logQueue = nullptr;
    }

    ~DesyncDiagnostics() { stopWatchdog(); stop(); }

    // ========================================================================
    // Hot-path: record the code path taken by outputDeviceIOProc each cycle.
    // Logs ONLY when the path changes (transition from normal → silent, etc.)
    //
    // Only atomic operations — no file I/O, no locks, no allocations.
    // ========================================================================
    void recordOutputPath(OutputProcPath path, int64_t fillLevel, double delta,
                          bool outputDataNonZero) {
        outputCycles.fetch_add(1, std::memory_order_relaxed);

        OutputProcPath prev = currentPath.load(std::memory_order_relaxed);

        if (path != prev) {
            // ── PATH CHANGED — this is the critical event ──
            currentPath.store(path, std::memory_order_relaxed);
            lastOutputNonZero.store(outputDataNonZero, std::memory_order_relaxed);

            uint64_t outC = outputCycles.load(std::memory_order_relaxed);
            uint64_t inC = inputCycles.load(std::memory_order_relaxed);
            uint64_t cyclesOnPrev = outC - pathStartCycle;
            pathStartCycle = outC;

            writeLog("PATH_CHANGED %s -> %s | fill=%lld delta=%.1f "
                     "outputNonZero=%d | prevPathCycles=%llu | "
                     "outputCycles=%llu inputCycles=%llu overruns=%llu",
                     pathName(prev), pathName(path),
                     fillLevel, delta, (int)outputDataNonZero,
                     cyclesOnPrev, outC, inC,
                     overrunCount.load(std::memory_order_relaxed));
        }

        // ── SILENCE TRANSITION — detect when fetched data goes from audio to zeros ──
        if (path == OutputProcPath::normal_fetchOK) {
            bool prevNonZero = lastOutputNonZero.load(std::memory_order_relaxed);
            if (prevNonZero != outputDataNonZero) {
                lastOutputNonZero.store(outputDataNonZero, std::memory_order_relaxed);
                uint64_t outC = outputCycles.load(std::memory_order_relaxed);
                uint64_t inC = inputCycles.load(std::memory_order_relaxed);
                writeLog("** %s ** fill=%lld delta=%.1f | "
                         "outputCycles=%llu inputCycles=%llu overruns=%llu",
                         outputDataNonZero ? "SILENCE_ENDED" : "SILENCE_STARTED",
                         fillLevel, delta, outC, inC,
                         overrunCount.load(std::memory_order_relaxed));
            }
        }

        // Track first cycle baseline
        if (path == OutputProcPath::normal_fetchOK &&
            initialFillLevel.load(std::memory_order_relaxed) == INT64_MIN) {
            int64_t expected = INT64_MIN;
            if (initialFillLevel.compare_exchange_strong(expected, fillLevel,
                                                          std::memory_order_relaxed)) {
                writeLog("FIRST_CYCLE fill=%lld delta=%.1f outputCycles=%llu inputCycles=%llu",
                         fillLevel, delta,
                         outputCycles.load(std::memory_order_relaxed),
                         inputCycles.load(std::memory_order_relaxed));
            }
        }

        if (path == OutputProcPath::normal_fetchOverrun) {
            overrunCount.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void recordInputCycle() {
        inputCycles.fetch_add(1, std::memory_order_relaxed);
    }

    // ========================================================================
    // Called when inputOutputSampleDelta is recalculated
    // ========================================================================
    void recordDeltaRecalculated(double newDelta, double lastInputFrame, double lastBufferSize) {
        writeLog("DELTA_RECALC delta=%.1f lastInputFrame=%.0f lastBufferSize=%.0f",
                 newDelta, lastInputFrame, lastBufferSize);
        initialFillLevel.store(INT64_MIN, std::memory_order_relaxed);
    }

    // ========================================================================
    // Lifecycle events
    // ========================================================================
    void recordIOStarted() {
        outputCycles.store(0, std::memory_order_relaxed);
        inputCycles.store(0, std::memory_order_relaxed);
        overrunCount.store(0, std::memory_order_relaxed);
        initialFillLevel.store(INT64_MIN, std::memory_order_relaxed);
        currentPath.store(OutputProcPath::unknown, std::memory_order_relaxed);
        lastOutputNonZero.store(false, std::memory_order_relaxed);
        pathStartCycle = 0;
        writeLog("IO_STARTED");
        startWatchdog();
    }

    void recordIOStopped() {
        stopWatchdog();
        writeLog("IO_STOPPED outputCycles=%llu inputCycles=%llu overruns=%llu lastPath=%s",
                 outputCycles.load(std::memory_order_relaxed),
                 inputCycles.load(std::memory_order_relaxed),
                 overrunCount.load(std::memory_order_relaxed),
                 pathName(currentPath.load(std::memory_order_relaxed)));
    }

    void recordSampleRateMismatch(double inputRate, double outputRate) {
        writeLog("SAMPLE_RATE_MISMATCH input=%.0f output=%.0f", inputRate, outputRate);
    }

    // ========================================================================
    // Output device state changes (e.g audio interface start/stop/reset)
    // ========================================================================
    void recordOutputDeviceStarted() {
        writeLog("OUTPUT_DEVICE_STARTED");
    }

    void recordOutputDeviceStopped() {
        writeLog("OUTPUT_DEVICE_STOPPED");
    }

    void recordInputDataReset() {
        writeLog("INPUT_DATA_RESET");
    }

    void recordOutputDeviceInvalid() {
        writeLog("OUTPUT_DEVICE_INVALID");
    }

    // ========================================================================
    // Desync injection: returns true once after trigger file is touched.
    // Called from the hot path — just an atomic load + exchange.
    // ========================================================================
    bool shouldInjectDesync() {
        if (desyncTriggerPending.load(std::memory_order_relaxed)) {
            return desyncTriggerPending.exchange(false, std::memory_order_relaxed);
        }
        return false;
    }

    // ========================================================================
    // Watchdog: detects when outputDeviceIOProc stops being called entirely
    // (e.g. audio interface USB glitch, macOS suspends device IO).
    // Checks every 2 seconds — if output cycle count hasn't advanced but
    // input cycles have, the output device went silent without our proc
    // knowing about it.
    // ========================================================================
    void startWatchdog() {
        if (watchdogTimer || !logQueue) return;

        watchdogTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, logQueue);
        uint64_t interval = 2 * NSEC_PER_SEC;
        dispatch_source_set_timer(watchdogTimer,
                                  dispatch_time(DISPATCH_TIME_NOW, interval),
                                  interval, interval / 10);

        dispatch_source_set_event_handler(watchdogTimer, ^{
            uint64_t outNow = outputCycles.load(std::memory_order_relaxed);
            uint64_t inNow = inputCycles.load(std::memory_order_relaxed);

            if (watchdogLastOutputCycles > 0) {
                bool outputStalled = (outNow == watchdogLastOutputCycles);
                bool inputActive = (inNow > watchdogLastInputCycles);
                bool wasStalled = watchdogOutputStalled;

                // Output proc stopped but input is still running = silent
                if (outputStalled && inputActive && !wasStalled) {
                    watchdogOutputStalled = true;
                    writeLog("** OUTPUT_PROC_STALLED ** outputCycles=%llu (stuck) "
                             "inputCycles=%llu (advancing) lastPath=%s",
                             outNow, inNow,
                             pathName(currentPath.load(std::memory_order_relaxed)));
                }
                // Output proc stopped and input also stopped = everything idle (normal)
                else if (outputStalled && !inputActive && !wasStalled) {
                    // Not interesting — both sides idle
                }
                // Output proc resumed after stall
                else if (!outputStalled && wasStalled) {
                    watchdogOutputStalled = false;
                    writeLog("OUTPUT_PROC_RESUMED outputCycles=%llu inputCycles=%llu", outNow, inNow);
                }
            }

            watchdogLastOutputCycles = outNow;
            watchdogLastInputCycles = inNow;

            // Check for desync trigger file (single-shot: once consumed, ignored
            // until coreaudiod restarts. Delete the file manually when done.)
            if (!desyncTriggerConsumed &&
                access("/tmp/ProxyAudioTriggerDesync", F_OK) == 0) {
                desyncTriggerConsumed = true;
                desyncTriggerPending.store(true, std::memory_order_relaxed);
                writeLog("** DESYNC_TRIGGER_INJECTED ** (from /tmp/ProxyAudioTriggerDesync)");
            }
        });

        dispatch_resume(watchdogTimer);
    }

    void stopWatchdog() {
        if (watchdogTimer) {
            dispatch_source_cancel(watchdogTimer);
            watchdogTimer = nullptr;
        }
        watchdogOutputStalled = false;
        watchdogLastOutputCycles = 0;
        watchdogLastInputCycles = 0;
    }

private:
    FILE *logFile = nullptr;
    dispatch_queue_t logQueue = nullptr;

    double nominalSampleRate = 0;
    uint32_t nominalBufferSize = 0;
    uint32_t ringCapacity = 0;

    // Hot-path atomics
    std::atomic<uint64_t> outputCycles{0};
    std::atomic<uint64_t> inputCycles{0};
    std::atomic<uint64_t> overrunCount{0};
    std::atomic<int64_t> initialFillLevel{INT64_MIN};
    std::atomic<OutputProcPath> currentPath{OutputProcPath::unknown};
    std::atomic<bool> lastOutputNonZero{false};
    std::atomic<bool> desyncTriggerPending{false};
    std::atomic<uint64_t> pathStartCycle{0};

    // Watchdog state (accessed only from logQueue)
    dispatch_source_t watchdogTimer = nullptr;
    uint64_t watchdogLastOutputCycles = 0;
    uint64_t watchdogLastInputCycles = 0;
    bool watchdogOutputStalled = false;
    bool desyncTriggerConsumed = false;  // persists across IO sessions, resets on coreaudiod restart

    // Write a log line — no-args overload (avoids -Wformat-security on literal strings)
    void writeLog(const char* msg_str) {
        writeLogImpl(msg_str);
    }

    // Write a log line — variadic overload for formatted messages
    template<typename... Args>
    void writeLog(const char* fmt, Args... args) {
        char msg[512];
        snprintf(msg, sizeof(msg), fmt, args...);
        writeLogImpl(msg);
    }

    void writeLogImpl(const char* msg) {

        time_t now;
        time(&now);
        struct tm tm_buf;
        localtime_r(&now, &tm_buf);
        char timeBuf[32];
        strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &tm_buf);

        char line[600];
        snprintf(line, sizeof(line), "[%s] %s\n", timeBuf, msg);

        char *heapLine = strdup(line);
        if (!heapLine) return;

        dispatch_async(logQueue, ^{
            if (logFile) {
                fprintf(logFile, "%s", heapLine);
                fflush(logFile);

                long pos = ftell(logFile);
                if (pos > 10 * 1024 * 1024) {
                    fclose(logFile);
                    rename("/tmp/ProxyAudioDiagnostics.log",
                           "/tmp/ProxyAudioDiagnostics.log.old");
                    logFile = fopen("/tmp/ProxyAudioDiagnostics.log", "a");
                }
            }
            free(heapLine);
        });
    }

};

// ============================================================================
// Macros
// ============================================================================
#if DESYNC_DIAGNOSTICS_ENABLED
    #define DIAG_START(diag, sr, bs, rc)                    (diag).start(sr, bs, rc)
    #define DIAG_STOP(diag)                                 (diag).stop()
    #define DIAG_RECORD_OUTPUT_PATH(diag, p, f, d, nz)     (diag).recordOutputPath(p, f, d, nz)
    #define DIAG_RECORD_INPUT(diag)                         (diag).recordInputCycle()
    #define DIAG_IO_STARTED(diag)                           (diag).recordIOStarted()
    #define DIAG_IO_STOPPED(diag)                           (diag).recordIOStopped()
    #define DIAG_DELTA_RECALC(diag, d, f, b)               (diag).recordDeltaRecalculated(d, f, b)
    #define DIAG_SAMPLE_RATE_MISMATCH(diag, i, o)          (diag).recordSampleRateMismatch(i, o)
    #define DIAG_OUTPUT_DEVICE_STARTED(diag)                (diag).recordOutputDeviceStarted()
    #define DIAG_OUTPUT_DEVICE_STOPPED(diag)                (diag).recordOutputDeviceStopped()
    #define DIAG_INPUT_DATA_RESET(diag)                     (diag).recordInputDataReset()
    #define DIAG_OUTPUT_DEVICE_INVALID(diag)                (diag).recordOutputDeviceInvalid()
#else
    #define DIAG_START(diag, sr, bs, rc)                    ((void)0)
    #define DIAG_STOP(diag)                                 ((void)0)
    #define DIAG_RECORD_OUTPUT_PATH(diag, p, f, d, nz)     ((void)0)
    #define DIAG_RECORD_INPUT(diag)                         ((void)0)
    #define DIAG_IO_STARTED(diag)                           ((void)0)
    #define DIAG_IO_STOPPED(diag)                           ((void)0)
    #define DIAG_DELTA_RECALC(diag, d, f, b)               ((void)0)
    #define DIAG_SAMPLE_RATE_MISMATCH(diag, i, o)          ((void)0)
    #define DIAG_OUTPUT_DEVICE_STARTED(diag)                ((void)0)
    #define DIAG_OUTPUT_DEVICE_STOPPED(diag)                ((void)0)
    #define DIAG_INPUT_DATA_RESET(diag)                     ((void)0)
    #define DIAG_OUTPUT_DEVICE_INVALID(diag)                ((void)0)
#endif

#endif /* DesyncDiagnostics_h */
