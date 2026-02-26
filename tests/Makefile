CXX = clang++
CXXFLAGS = -std=c++17 -O2 -Wall -I../proxyAudioDevice
LDFLAGS = -framework CoreServices

all: TestDesyncRecovery TestE2ERecovery TestAudibleRecovery DesyncReproducer

TestDesyncRecovery: TestDesyncRecovery.cpp ../proxyAudioDevice/DesyncRecovery.h
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $<

TestE2ERecovery: TestE2ERecovery.cpp ../proxyAudioDevice/DesyncRecovery.h ../proxyAudioDevice/AudioRingBuffer.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ TestE2ERecovery.cpp ../proxyAudioDevice/AudioRingBuffer.cpp

TestAudibleRecovery: TestAudibleRecovery.cpp
	$(CXX) $(CXXFLAGS) -framework CoreAudio -framework AudioToolbox -framework CoreFoundation -o $@ $<

DesyncReproducer: DesyncReproducer.cpp ../proxyAudioDevice/AudioRingBuffer.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^

test: TestDesyncRecovery TestE2ERecovery
	@echo "=== Running unit tests ==="
	./TestDesyncRecovery
	@echo ""
	@echo "=== Running e2e tests ==="
	./TestE2ERecovery

test-audible: TestAudibleRecovery
	@echo "=== Running audible recovery test ==="
	@echo "(You should hear a 440Hz tone for 6 seconds)"
	./TestAudibleRecovery

clean:
	rm -f TestDesyncRecovery TestE2ERecovery TestAudibleRecovery DesyncReproducer

.PHONY: all test test-audible clean
