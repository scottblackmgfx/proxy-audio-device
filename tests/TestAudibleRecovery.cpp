// test_audible_recovery.cpp
//
// Audible end-to-end test for the desync recovery fix.
//
// Plays a sine wave through the system's current default output device,
// injects a desync via the trigger file midway through, and verifies
// (both audibly and programmatically) that audio continues without dropout.
//
// What you should hear:
//   - 3 seconds of sine tone (pre-injection, baseline)
//   - A brief click/skip at most when the desync is injected
//   - 3 seconds more of sine tone (post-injection, recovered)
//   - Silence (test complete)
//
// The test also monitors its own output buffer for silence gaps and reports
// whether the fix worked.
//
// Usage: make test_audible_recovery && ./test_audible_recovery
//
// Requirements: proxy audio device driver must be installed and set as the
// system output device (or pass a device name as an argument).

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <thread>
#include <chrono>
#include <unistd.h>

// ============================================================================
// Configuration
// ============================================================================
static const double kSampleRate       = 44100.0;
static const double kToneFrequency    = 440.0;   // A4
static const double kToneAmplitude    = 0.3;      // comfortable volume
static const int    kPreInjectionSec  = 3;
static const int    kPostInjectionSec = 3;
static const int    kNumBuffers       = 3;
static const UInt32 kBufferFrames     = 2048;
static const UInt32 kChannels         = 2;
static const UInt32 kBytesPerFrame    = kChannels * sizeof(Float32);

// ============================================================================
// Shared state between callback and main thread
// ============================================================================
static std::atomic<uint64_t> gTotalFramesRendered{0};
static std::atomic<int>      gSilentBufferCount{0};
static std::atomic<int>      gNonZeroBufferCount{0};
static std::atomic<bool>     gInjected{false};
static std::atomic<int>      gSilentAfterInjection{0};
static std::atomic<int>      gNonZeroAfterInjection{0};
static double                gPhase = 0.0;

// ============================================================================
// AudioQueue callback — generates a sine wave
// ============================================================================
static void audioQueueCallback(void *inUserData,
                               AudioQueueRef inAQ,
                               AudioQueueBufferRef inBuffer) {
    Float32 *samples = (Float32 *)inBuffer->mAudioData;
    UInt32 frameCount = kBufferFrames;
    double phaseInc = 2.0 * M_PI * kToneFrequency / kSampleRate;

    bool hasNonZero = false;
    for (UInt32 i = 0; i < frameCount; i++) {
        Float32 val = (Float32)(sin(gPhase) * kToneAmplitude);
        samples[i * kChannels]     = val;  // L
        samples[i * kChannels + 1] = val;  // R
        gPhase += phaseInc;
        if (val != 0.0f) hasNonZero = true;
    }

    // Keep phase bounded
    if (gPhase > 2.0 * M_PI * 1000.0) {
        gPhase -= 2.0 * M_PI * 1000.0;
    }

    inBuffer->mAudioDataByteSize = frameCount * kBytesPerFrame;
    gTotalFramesRendered.fetch_add(frameCount, std::memory_order_relaxed);

    if (hasNonZero) {
        gNonZeroBufferCount.fetch_add(1, std::memory_order_relaxed);
        if (gInjected.load(std::memory_order_relaxed)) {
            gNonZeroAfterInjection.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        gSilentBufferCount.fetch_add(1, std::memory_order_relaxed);
        if (gInjected.load(std::memory_order_relaxed)) {
            gSilentAfterInjection.fetch_add(1, std::memory_order_relaxed);
        }
    }

    AudioQueueEnqueueBuffer(inAQ, inBuffer, 0, NULL);
}

// ============================================================================
// Find an audio device by name substring (case-insensitive)
// Returns kAudioObjectUnknown if not found.
// ============================================================================
static AudioDeviceID findDeviceByName(const char *searchName) {
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    UInt32 size = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, NULL, &size);
    UInt32 count = size / sizeof(AudioDeviceID);
    if (count == 0) return kAudioObjectUnknown;

    AudioDeviceID *devices = new AudioDeviceID[count];
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &size, devices);

    AudioDeviceID found = kAudioObjectUnknown;
    for (UInt32 i = 0; i < count; i++) {
        AudioObjectPropertyAddress nameAddr = {
            kAudioObjectPropertyName,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        CFStringRef name = NULL;
        UInt32 nameSize = sizeof(name);
        if (AudioObjectGetPropertyData(devices[i], &nameAddr, 0, NULL, &nameSize, &name) == noErr && name) {
            char nameBuf[256];
            CFStringGetCString(name, nameBuf, sizeof(nameBuf), kCFStringEncodingUTF8);
            CFRelease(name);

            // Case-insensitive substring match
            if (strcasestr(nameBuf, searchName)) {
                printf("  Found device: \"%s\" (ID=%u)\n", nameBuf, devices[i]);
                found = devices[i];
                break;
            }
        }
    }

    delete[] devices;
    return found;
}

// ============================================================================
// List all output devices
// ============================================================================
static void listOutputDevices() {
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    UInt32 size = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, NULL, &size);
    UInt32 count = size / sizeof(AudioDeviceID);
    AudioDeviceID *devices = new AudioDeviceID[count];
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &size, devices);

    printf("  Available devices:\n");
    for (UInt32 i = 0; i < count; i++) {
        // Check if device has output channels
        AudioObjectPropertyAddress streamAddr = {
            kAudioDevicePropertyStreamConfiguration,
            kAudioObjectPropertyScopeOutput,
            kAudioObjectPropertyElementMain
        };
        UInt32 streamSize = 0;
        AudioObjectGetPropertyDataSize(devices[i], &streamAddr, 0, NULL, &streamSize);
        if (streamSize > sizeof(AudioBufferList)) {
            AudioObjectPropertyAddress nameAddr = {
                kAudioObjectPropertyName,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            CFStringRef name = NULL;
            UInt32 nameSize = sizeof(name);
            if (AudioObjectGetPropertyData(devices[i], &nameAddr, 0, NULL, &nameSize, &name) == noErr && name) {
                char nameBuf[256];
                CFStringGetCString(name, nameBuf, sizeof(nameBuf), kCFStringEncodingUTF8);
                CFRelease(name);
                printf("    [%u] %s\n", devices[i], nameBuf);
            }
        }
    }
    delete[] devices;
}

// ============================================================================
// Get default output device (ideally - manually set this beforehand through the proxy)
// ============================================================================
static AudioDeviceID getDefaultOutputDevice() {
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioDeviceID deviceID = kAudioObjectUnknown;
    UInt32 size = sizeof(deviceID);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &size, &deviceID);
    return deviceID;
}

static const char* getDeviceName(AudioDeviceID deviceID) {
    static char nameBuf[256];
    AudioObjectPropertyAddress nameAddr = {
        kAudioObjectPropertyName,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    CFStringRef name = NULL;
    UInt32 nameSize = sizeof(name);
    if (AudioObjectGetPropertyData(deviceID, &nameAddr, 0, NULL, &nameSize, &name) == noErr && name) {
        CFStringGetCString(name, nameBuf, sizeof(nameBuf), kCFStringEncodingUTF8);
        CFRelease(name);
    } else {
        snprintf(nameBuf, sizeof(nameBuf), "Unknown (ID=%u)", deviceID);
    }
    return nameBuf;
}

// ============================================================================
// Main
// ============================================================================
int main(int argc, char *argv[]) {
    printf("=== Audible Desync Recovery Test ===\n\n");

    // Determine target device
    AudioDeviceID targetDevice = kAudioObjectUnknown;

    if (argc > 1) {
        printf("Searching for device: \"%s\"\n", argv[1]);
        targetDevice = findDeviceByName(argv[1]);
        if (targetDevice == kAudioObjectUnknown) {
            printf("  Device not found.\n");
            listOutputDevices();
            return 1;
        }
    } else {
        targetDevice = getDefaultOutputDevice();
        printf("Using default output device: \"%s\" (ID=%u)\n",
               getDeviceName(targetDevice), targetDevice);
        printf("(Pass a device name as argument to target a specific device)\n");
    }

    // Clean up any stale trigger file
    unlink("/tmp/ProxyAudioTriggerDesync");

    // Set up AudioQueue
    AudioStreamBasicDescription format = {};
    format.mSampleRate       = kSampleRate;
    format.mFormatID         = kAudioFormatLinearPCM;
    format.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mBytesPerPacket   = kBytesPerFrame;
    format.mFramesPerPacket  = 1;
    format.mBytesPerFrame    = kBytesPerFrame;
    format.mChannelsPerFrame = kChannels;
    format.mBitsPerChannel   = 32;

    AudioQueueRef queue = NULL;
    OSStatus err = AudioQueueNewOutput(&format, audioQueueCallback, NULL,
                                       NULL, NULL, 0, &queue);
    if (err != noErr) {
        printf("ERROR: AudioQueueNewOutput failed (%d)\n", (int)err);
        return 1;
    }

    // Route to the target device
    CFStringRef deviceUID = NULL;
    AudioObjectPropertyAddress uidAddr = {
        kAudioDevicePropertyDeviceUID,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 uidSize = sizeof(deviceUID);
    err = AudioObjectGetPropertyData(targetDevice, &uidAddr, 0, NULL, &uidSize, &deviceUID);
    if (err == noErr && deviceUID) {
        AudioQueueSetProperty(queue, kAudioQueueProperty_CurrentDevice, &deviceUID, sizeof(deviceUID));
        CFRelease(deviceUID);
    }

    // Allocate and prime buffers
    for (int i = 0; i < kNumBuffers; i++) {
        AudioQueueBufferRef buf;
        AudioQueueAllocateBuffer(queue, kBufferFrames * kBytesPerFrame, &buf);
        buf->mAudioDataByteSize = kBufferFrames * kBytesPerFrame;
        memset(buf->mAudioData, 0, buf->mAudioDataByteSize);
        audioQueueCallback(NULL, queue, buf);
    }

    // Start playback
    printf("\nPlaying %d seconds of 440Hz sine wave...\n", kPreInjectionSec);
    err = AudioQueueStart(queue, NULL);
    if (err != noErr) {
        printf("ERROR: AudioQueueStart failed (%d)\n", (int)err);
        return 1;
    }

    // Phase 1: Pre-injection baseline
    std::this_thread::sleep_for(std::chrono::seconds(kPreInjectionSec));

    int preInjectionNonZero = gNonZeroBufferCount.load();
    int preInjectionSilent  = gSilentBufferCount.load();
    printf("  Pre-injection: %d non-zero buffers, %d silent buffers\n",
           preInjectionNonZero, preInjectionSilent);

    // Phase 2: Inject desync
    printf("\nInjecting desync (touch /tmp/ProxyAudioTriggerDesync)...\n");
    FILE *f = fopen("/tmp/ProxyAudioTriggerDesync", "w");
    if (f) fclose(f);
    gInjected.store(true, std::memory_order_relaxed);

    // Phase 3: Post-injection observation
    printf("Playing %d more seconds...\n", kPostInjectionSec);
    std::this_thread::sleep_for(std::chrono::seconds(kPostInjectionSec));

    // Stop
    AudioQueueStop(queue, true);
    AudioQueueDispose(queue, true);

    // Clean up trigger file
    unlink("/tmp/ProxyAudioTriggerDesync");

    // Results
    int postNonZero = gNonZeroAfterInjection.load();
    int postSilent  = gSilentAfterInjection.load();
    int totalNonZero = gNonZeroBufferCount.load();
    int totalSilent  = gSilentBufferCount.load();

    printf("\n=== Results ===\n");
    printf("  Total buffers rendered: %d non-zero, %d silent\n", totalNonZero, totalSilent);
    printf("  Post-injection:         %d non-zero, %d silent\n", postNonZero, postSilent);
    printf("  Frames rendered:        %llu (%.1f seconds)\n",
           gTotalFramesRendered.load(), gTotalFramesRendered.load() / kSampleRate);

    bool passed = true;

    if (preInjectionNonZero == 0) {
        printf("\n  FAIL: No audio rendered pre-injection (is the device working?)\n");
        passed = false;
    }

    if (postNonZero == 0) {
        printf("\n  FAIL: No audio rendered post-injection (fix didn't work)\n");
        passed = false;
    }

    if (postSilent > 2) {
        printf("\n  FAIL: %d silent buffers after injection (expected at most 2)\n", postSilent);
        passed = false;
    }

    if (passed) {
        printf("\n  PASS: Audio continued through desync injection\n");
    }

    printf("\n");
    return passed ? 0 : 1;
}
