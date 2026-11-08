// Output stage: enumerate CoreAudio output devices (your interface),
// run an AUHAL unit on the chosen device, pull tapped stereo from the ring,
// run the engine, and lay out channels:
//   ch1 L · ch2 R (passthrough of what you're playing)
//   ch3 HEAD · ch4 HEART · ch5 BELLY · ch6 ROOT · ch7 FEET
// Devices with fewer channels get a truncated map (stereo-only devices can
// optionally monitor the vibration bus mixed into L/R).
#pragma once
#include <atomic>
#include <string>
#include <vector>
#include "ringbuf.h"
#include "engine.h"
#include "stem_player.h"
#include "bed_input.h"
#include "dsp.h"
#include "mic_fx.h"

struct OutDevice {
    unsigned id;
    std::string name;
    int channels;
    double sampleRate;
    // inputs only: how likely this is a mic someone plugged in to talk into —
    // 4 USB, 3 other hardware / Bluetooth, 2 built-in, 1 iPhone (Continuity) /
    // unknown, 0 virtual / aggregate
    int micRank = 0;
};

std::vector<OutDevice> listOutputDevices();
// the OS's current default output (what the user's music is playing through);
// 0 if unknown. On macOS the tap mutes the source apps, so a fresh install
// must play through this device or the music simply disappears.
unsigned defaultOutputDevice();

// hardware output volume scalars on a device (the knob between us and the
// DACs): average across settable elements, or -1 if the device exposes none
float deviceHwVolume(unsigned deviceID);
// push every settable output volume scalar to 1.0 and unmute all elements;
// returns true if the device exposed anything settable
bool maxDeviceHwVolume(unsigned deviceID);

class OutputUnit {
public:
    bool start(unsigned deviceID, Engine* engine, StereoRing* ring);
    void stop();
    bool running() const { return running_.load(); }
    std::string lastError;
    int activeChannels = 0;
    // per-device-channel output peak, post master/trim — what actually leaves
    // the interface toward each amp. Written on the audio thread with a slow
    // per-block decay; the health monitor reads them.
    static constexpr int kMaxCh = 16;
    std::atomic<float> chanPeak[kMaxCh];
    // channel finder: ping(c) plays a 0.9 s 55 Hz burst on device channel c
    // alone (bypasses zone routing) so an unknown amp/transducer can be
    // located by feel. <0 = idle. Cleared by the audio thread when done.
    std::atomic<int> pingChan{-1};
    void ping(int c) { pingLeft_.store(43200); pingChan.store(c); }
    // optional second stereo ring (live input from the interface); summed
    // with the tap before the engine. Set by the app, may be null.
    StereoRing* inRing = nullptr;
    // optional stem player: while it is active its stems replace the engine's
    // vibration and the tapped music (the engine still analyses the music)
    StemPlayer* player = nullptr;
    // optional live bed input (a DAW over a multichannel device): while it
    // runs, its channels 1-2 are the music and 3-7 the zones it owns
    BedInput* bed = nullptr;
    // when authored zones (stems / DAW) and engine zones share the bed, the
    // authored ones are held back by the engine's look-ahead so all align
    dsp::DelayLine bedDelay_[NZONES];
    // live input goes to the ENGINE only by default (felt, not heard); on =
    // also heard on the stereo pair / music channels (the MIC button)
    std::atomic<bool> inToSpeakers{false};
    // effects on the live input (heard and felt) + body echo on the zones
    MicFx micFx;
    // MIC level ×0..2, on its own (the music has params.musicGain): what is
    // heard and what the engine turns into vibration
    std::atomic<float> micLevel{1.0f};
    float micG_ = 1.0f;   // audio thread: last block's gain (ramped)
    // the live input as heard (inToSpeakers): straight to the stereo pair /
    // music channels, never through the sync look-ahead — talking into a mic
    // has to come back instantly. Set per block by renderOutputBlock.
    const float* monL_ = nullptr;
    const float* monR_ = nullptr;
    // true when the capture path silences the source apps (macOS process
    // tap): on a plain stereo device we must then play the music ourselves.
    // WASAPI loopback (Windows) leaves the source audible, so false there.
    bool tapMutesSource = false;
    std::atomic<int> pingLeft_{0}; // samples remaining (audio thread owned)
    float pingPhase_ = 0;
    // thermal guard state per device channel (zone channels only): running
    // mean-square (~45 s) and the gain currently applied. chanHeat = fraction
    // of the RMS budget (1.0 = at the limit) for the health monitor.
    std::atomic<float> chanHeat[kMaxCh];
    std::atomic<float> chanThermGain[kMaxCh];
    float heatMs_[kMaxCh] = {0};
    float thermG_[kMaxCh] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    // output peak limiter, every device channel, last thing before the DAC:
    // instant attack to a 0.98 ceiling, 150 ms release. chanLimRed = gain
    // reduction currently applied (0 = none, 0.5 = -6 dB) for the health monitor.
    std::atomic<float> chanLimRed[kMaxCh] = {};
    float limG_[kMaxCh] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

private:
    std::atomic<bool> running_{false};
    void* unit_ = nullptr; // AudioUnit
    Engine* engine_ = nullptr;
    StereoRing* ring_ = nullptr;
    friend struct OutTrampoline;
public:
    Engine* engine() { return engine_; }
    StereoRing* ring() { return ring_; }
};

// the whole per-block render, shared by both backends (audio_mix.cpp): pop the
// tap (kept `tapTarget` frames deep), add the live input, let a live DAW feed
// or a stem set take over the zones it brings, run the engine, lay out the
// device channels into dst. ch = device channels.
void renderOutputBlock(OutputUnit* self, Engine* eng, float* dst, int n, int ch, int tapTarget);
// shared mixer (audio_mix.cpp) used by every platform backend
// vibR: per-zone right-side signal for send 2 (STEREO mode) or null entries
void mixOutputBlock(OutputUnit* self, Engine* eng, const float* L, const float* R,
                    float* const* vib, const float* const* vibR, float* dst, int n, int ch);
