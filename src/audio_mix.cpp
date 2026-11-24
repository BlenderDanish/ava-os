// Shared output mixer for every backend (CoreAudio on macOS, miniaudio on
// Windows): routing, solo, second sends, thermal guard, channel-finder ping
// and per-channel meters. Backends only pull the tapped stereo, run the
// engine, and hand the interleaved device buffer here.
#include "audio_out.h"
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstring>

void mixOutputBlock(OutputUnit* self, Engine* eng, const float* L, const float* R,
                    float* const* vib, const float* const* vibR, float* dst, int n, int ch) {
    float vol = eng->params.masterVolume.load();
    bool monitor = eng->params.monitorVibOnStereo.load() != 0;
    const bool transducer = eng->params.transducerOnStereo.load() != 0;
    // AVA plays the music only when it has taken it over (muted tap); in
    // listen mode the music is already coming out of the speakers untouched
    // (an authored stem set brings its own music track: that always plays)
    const bool takeOver = (self->tapMutesSource && eng->params.takeOver.load() != 0)
                          || (self->player && self->player->active())
                          || (self->bed && self->bed->running());
    // transport: paused = the bed is still, the music keeps playing
    const float vibG = eng->params.vibOn.load() ? 1.0f : 0.0f;
    const float* monL = self->monL_;
    const float* monR = self->monR_;

    // Routing: music pair + surround pair + per-zone channels, all on the
    // chosen device. On a plain stereo device (≤2ch) fall back to a vibration
    // monitor mix instead of doubling the music that's usually audible there.
    int musicL = eng->params.musicChanL.load();
    float musicG = eng->params.musicGain.load();
    int surrL = eng->params.surrChanL.load();
    const int monoMask = eng->params.monoMask.load();
    float surrG = eng->params.surrGain.load();
    float surrW = eng->params.surrWidth.load();
    int zc[NZONES], zc2[NZONES];
    float zt[NZONES];
    const float* vR[NZONES];
    for (int z = 0; z < NZONES; z++) vR[z] = vibR ? vibR[z] : nullptr;
    int solo = eng->params.soloZone.load();
    for (int z = 0; z < NZONES; z++) {
        zc[z] = eng->params.zoneChan[z].load();
        zc2[z] = eng->params.zoneChan2[z].load();
        zt[z] = (solo >= 0 && z != solo) ? 0.0f : eng->params.zoneTrim[z].load();
    }

    // block peak per device channel, decayed into chanPeak for the monitor
    auto captureMeters = [&]() {
        for (int c = 0; c < ch && c < OutputUnit::kMaxCh; c++) {
            float pk = 0;
            for (int i = 0; i < n; i++) {
                float a = std::fabs(dst[(size_t)i * ch + c]);
                if (a > pk) pk = a;
            }
            float prev = self->chanPeak[c].load(std::memory_order_relaxed) * 0.92f;
            self->chanPeak[c].store(pk > prev ? pk : prev, std::memory_order_relaxed);
        }
    };

    std::memset(dst, 0, (size_t)n * ch * sizeof(float));
    // AVA Circle: 8 transducers = inner layer (FEET+ROOT+BELLY) and outer layer
    // (HEART+HEAD), each on four quadrants; the orbit leans the level round them
    const bool circle = eng->params.bedType.load() == 1 && ch > 2;
    int cc[8]; float cg[8];
    if (circle) {
        float depth = eng->params.circOrbitDepth.load();
        self->circPhase_ += 6.2831853f * eng->params.circOrbitHz.load() * (float)n / 48000.0f;
        if (self->circPhase_ > 6.2831853f) self->circPhase_ -= 6.2831853f;
        for (int k = 0; k < 8; k++) {
            cc[k] = eng->params.circChan[k].load();
            float lean = 0.5f + 0.5f * std::cos(self->circPhase_ - 1.5707963f * (float)(k & 3));
            cg[k] = eng->params.circTrim[k].load() * (1.0f - depth * (1.0f - lean));
        }
        for (int z = 0; z < NZONES; z++) { zc[z] = -1; zc2[z] = -1; }   // the octagon's rings are not wired on this bed
    }
    // a ring or mono speaker routed onto a stereo device's own channels (an
    // amp + transducer on the headphone jack) makes it a real bed output:
    // routing, trims, solo, TEST and PING apply as on an interface
    bool routedHere = false;
    for (int z = 0; z < NZONES; z++)
        routedHere |= (zc[z] >= 0 && zc[z] < ch) || (zc2[z] >= 0 && zc2[z] < ch);
    routedHere |= (monoMask & ((1 << std::min(ch, 31)) - 1)) != 0;
    if (ch <= 2 && !routedHere) {
        // Plain stereo device (laptop, headphones, a DJ's built-in output):
        // this is what every download runs on. When the tap has muted the
        // source apps we ARE the speakers, so the music passes through at
        // unity (already delayed by the sync look-ahead); the vibration bus
        // rides underneath as an audible monitor so the bed can be heard
        // pumping even without transducers.
        float mg = takeOver ? musicG : 0.0f;
        for (int i = 0; i < n; i++) {
            // audible bed on small speakers: the rings live at 10-80 Hz, which
            // a laptop cannot reproduce, so the monitor is gently saturated —
            // the harmonics (80-240 Hz) carry the same rhythm where you can hear it
            float m = 0.5f * (vib[HEART][i] + vib[BELLY][i] + vib[ROOT][i] + vib[FEET][i]);
            float v = monitor ? vibG * 0.8f * std::tanh(2.2f * m) : 0.0f;
            if (transducer) {   // an amp + transducer on this output: the whole bed, clean, with trims and solo
                v = 0.0f;
                for (int z = 0; z < NZONES; z++) v += zt[z] * vib[z][i];
                v *= vibG;
            }
            dst[i * ch] = vol * (mg * L[i] + v);
            if (ch > 1) dst[i * ch + 1] = vol * (mg * R[i] + v);
            // the mic as heard follows the master like everything else
            if (monL) {
                if (ch > 1) { dst[i * ch] += vol * monL[i]; dst[i * ch + 1] += vol * monR[i]; }
                else dst[i] += vol * 0.5f * (monL[i] + monR[i]);
            }
        }
        // same limiter as the multichannel path
        const float ceil = 0.98f, aRel = 1.0f / (0.15f * 48000.0f);
        for (int c = 0; c < ch; c++) {
            float g = self->limG_[c], minG = 1.0f;
            for (int i = 0; i < n; i++) {
                float& v = dst[(size_t)i * ch + c];
                float a = std::fabs(v), tg = a > ceil ? ceil / a : 1.0f;
                if (tg < g) g = tg; else g += (1.0f - g) * aRel;
                v *= g; if (g < minG) minG = g;
            }
            self->limG_[c] = g;
            self->chanLimRed[c].store(1.0f - minG, std::memory_order_relaxed);
        }
        captureMeters();
        return;
    }
    for (int i = 0; i < n; i++) {
        float* f = dst + (size_t)i * ch;
        if (monL && musicL >= 0 && musicL < ch) {
            f[musicL] += vol * monL[i];
            if (musicL + 1 < ch) f[musicL + 1] += vol * monR[i];
        }
        // listening: the Mac already plays on ch 1-2 itself; a music pair moved
        // anywhere else (speakers on another output) gets AVA's copy
        if ((takeOver || musicL > 0) && musicL >= 0 && musicL < ch) {
            f[musicL] += vol * musicG * L[i];
            if (musicL + 1 < ch) f[musicL + 1] += vol * musicG * R[i];
        }
        if (monoMask) {   // single speakers (a TS cable in a headphone jack): music summed to mono
            float mono = vol * musicG * 0.5f * (L[i] + R[i]);
            for (int c = 0; c < ch && c < 32; c++) if (monoMask & (1 << c)) f[c] += mono;
        }
        if (surrL >= 0 && surrL < ch) {   // AVA-only pair: plays while listening too
            // mid/side width control: 0 = mono fill, 1 = full stereo image
            float mid = 0.5f * (L[i] + R[i]);
            float side = 0.5f * (L[i] - R[i]) * surrW;
            f[surrL] += vol * surrG * (mid + side);
            if (surrL + 1 < ch) f[surrL + 1] += vol * surrG * (mid - side);
        }
        if (circle) {
            float in = vibG * (zt[FEET] * vib[FEET][i] + zt[ROOT] * vib[ROOT][i] + zt[BELLY] * vib[BELLY][i]);
            float out = vibG * (zt[HEART] * vib[HEART][i] + zt[HEAD] * vib[HEAD][i]);
            for (int k = 0; k < 8; k++)
                if (cc[k] >= 0 && cc[k] < ch) f[cc[k]] += vol * cg[k] * (k < 4 ? in : out);
        }
        for (int z = 0; z < NZONES; z++) {
            if (zc[z] >= 0 && zc[z] < ch) f[zc[z]] += vol * vibG * zt[z] * vib[z][i];
            // second send: the same signal, or in STEREO mode the right side
            if (zc2[z] >= 0 && zc2[z] < ch) f[zc2[z]] += vol * vibG * zt[z] * (vR[z] ? vR[z][i] : vib[z][i]);
        }
    }
    // thermal guard on zone channels: integrate output power over ~45 s and,
    // past the RMS budget, ease the channel down (2 s slew) until it cools
    {
        float lim = eng->params.thermalLimit.load();
        bool zoneCh[OutputUnit::kMaxCh] = {false};
        for (int z = 0; z < NZONES; z++) {
            if (zc[z] >= 0 && zc[z] < OutputUnit::kMaxCh) zoneCh[zc[z]] = true;
            if (zc2[z] >= 0 && zc2[z] < OutputUnit::kMaxCh) zoneCh[zc2[z]] = true;
        }
        if (circle) for (int k = 0; k < 8; k++) if (cc[k] >= 0 && cc[k] < OutputUnit::kMaxCh) zoneCh[cc[k]] = true;
        const float aHeat = 1.0f / (45.0f * 48000.0f);
        const float aG = 1.0f / (2.0f * 48000.0f);
        const float budget = lim * lim;
        for (int c = 0; c < ch && c < OutputUnit::kMaxCh; c++) {
            if (!zoneCh[c]) { self->chanHeat[c].store(0, std::memory_order_relaxed); continue; }
            float ms = self->heatMs_[c], g = self->thermG_[c];
            for (int i = 0; i < n; i++) {
                float& v = dst[(size_t)i * ch + c];
                ms += (v * v - ms) * aHeat; // demand (pre-gain) power, so the cap is exact
                float tg = 1.0f;
                if (lim > 0.0f && ms > budget) tg = std::max(0.2f, std::sqrt(budget / ms));
                g += (tg - g) * aG;
                v *= g;
            }
            self->heatMs_[c] = ms;
            self->thermG_[c] = g;
            self->chanHeat[c].store(budget > 0 ? ms / budget : 0, std::memory_order_relaxed);
            self->chanThermGain[c].store(g, std::memory_order_relaxed);
        }
    }
    // channel-finder burst: 55 Hz sine with 30 ms fades on one raw channel
    {
        int pc = self->pingChan.load(std::memory_order_relaxed);
        int left = self->pingLeft_.load(std::memory_order_relaxed);
        if (pc >= 0 && pc < ch && left > 0) {
            const int total = 43200, fade = 1440;
            for (int i = 0; i < n && left > 0; i++, left--) {
                int done = total - left;
                float env = std::min(1.0f, std::min(done, left) / (float)fade);
                dst[(size_t)i * ch + pc] += 0.7f * env * std::sin(self->pingPhase_);
                self->pingPhase_ += 2.0f * 3.14159265f * 55.0f / 48000.0f;
                if (self->pingPhase_ > 6.2831853f) self->pingPhase_ -= 6.2831853f;
            }
            self->pingLeft_.store(left, std::memory_order_relaxed);
            if (left <= 0) self->pingChan.store(-1, std::memory_order_relaxed);
        } else if (pc >= 0 && left <= 0) {
            self->pingChan.store(-1, std::memory_order_relaxed);
        }
    }
    // peak limiter on every device channel: whatever stacked up above (hot
    // music send, six zone normaliser gain, pads on top of the chain, a
    // feedback loop through the interface) leaves here below full scale
    {
        const float ceil = 0.98f;
        const float aRel = 1.0f / (0.15f * 48000.0f);
        for (int c = 0; c < ch && c < OutputUnit::kMaxCh; c++) {
            float g = self->limG_[c], minG = 1.0f;
            for (int i = 0; i < n; i++) {
                float& v = dst[(size_t)i * ch + c];
                float a = std::fabs(v);
                float tg = a > ceil ? ceil / a : 1.0f;
                if (tg < g) g = tg; else g += (1.0f - g) * aRel;
                v *= g;
                if (g < minG) minG = g;
            }
            self->limG_[c] = g;
            self->chanLimRed[c].store(1.0f - minG, std::memory_order_relaxed);
        }
    }
    captureMeters();
}

// ── the per-block render shared by the macOS (AUHAL) and Windows (WASAPI) backends ──
void renderOutputBlock(OutputUnit* self, Engine* eng, float* dst, int n, int ch, int tapTarget) {
    static thread_local std::vector<float> L, R, vib[NZONES];
    L.resize(n); R.resize(n);
    float* vibPtr[NZONES];
    for (int z = 0; z < NZONES; z++) { vib[z].resize(n); vibPtr[z] = vib[z].data(); }

    // Latency control: keep the tap→output queue tight. If more than
    // target+2 blocks are waiting (output started late, clock drift),
    // skip ahead to ~target so felt vibration stays in sync with heard sound.
    {
        StereoRing* ring = self->ring();
        int target = tapTarget; // mac ≈ block + 2.7 ms (as tight as the tap allows); Windows + 512
        int avail = ring->available();
        if (avail > target + 2 * n) ring->discard(avail - target);
    }
    self->ring()->pop(L.data(), R.data(), n);
    self->monL_ = self->monR_ = nullptr;
    // what the speakers get: the tap alone (a heard input is added in the
    // mixer, undelayed, via monL_/monR_)
    static thread_local std::vector<float> mL, mR;
    mL.assign(L.begin(), L.begin() + n); mR.assign(R.begin(), R.begin() + n);
    if (StereoRing* ir = self->inRing) {
        // keep the live input tight too, then add it on top of the tap
        int avail = ir->available();
        if (avail > n + 128 + 2 * n) ir->discard(avail - (n + 128));
        static thread_local std::vector<float> iL, iR;
        iL.assign(n, 0.0f); iR.assign(n, 0.0f);
        if (avail >= n) ir->pop(iL.data(), iR.data(), n);
        // effects run on silence too, so echo and reverb tails ring out
        self->micFx.process(iL.data(), iR.data(), n);
        // MIC level, ramped across the block so moving it never clicks
        const float mic1 = std::min(2.0f, std::max(0.0f, self->micLevel.load())), mic0 = self->micG_;
        for (int i = 0; i < n; i++) {
            float mic = mic0 + (mic1 - mic0) * (i + 1) / (float)n;
            iL[i] *= mic; iR[i] *= mic;
            L[i] += iL[i]; R[i] += iR[i];
        }
        self->micG_ = mic1;
        if (self->inToSpeakers.load()) { self->monL_ = iL.data(); self->monR_ = iR.data(); }
    }
    // authored content owns the bed: a live DAW feed first, else a stem set.
    // Its music replaces the tap; zones it brings replace the engine's; zones
    // it leaves out are filled by the live engine (hybrid sets).
    {
        const float* authored[NZONES] = {nullptr, nullptr, nullptr, nullptr, nullptr};
        bool have = false;
        static thread_local std::vector<float> bch[BedInput::kCh];
        static thread_local std::vector<float> stem[NZONES];
        if (BedInput* bi = self->bed; bi && bi->running()) {
            float* bp[BedInput::kCh];
            for (int c = 0; c < BedInput::kCh; c++) { bch[c].resize(n); bp[c] = bch[c].data(); }
            int avail = bi->ring.available();
            if (avail > n + 128 + 2 * n) bi->ring.discard(avail - (n + 128));
            bi->ring.pop(bp, n);
            for (int i = 0; i < n; i++) { mL[i] = bp[0][i]; mR[i] = bp[1][i]; }
            for (int z = 0; z < NZONES; z++) if (bi->zoneFromDaw[z].load()) authored[z] = bp[2 + z];
            have = true;
        } else if (self->player && self->player->active()) {
            float* stemPtr[NZONES];
            for (int z = 0; z < NZONES; z++) { stem[z].resize(n); stemPtr[z] = stem[z].data(); }
            if (self->player->render(mL.data(), mR.data(), stemPtr, n)) {
                for (int z = 0; z < NZONES; z++) if (self->player->has[1 + z]) authored[z] = stemPtr[z];
                have = true;
            }
        }
        if (have) {
            eng->process(mL.data(), mR.data(), vibPtr, n);   // analysis + visuals, and the engine zones
            bool anyLive = false;
            for (int z = 0; z < NZONES; z++) if (!authored[z]) anyLive = true;
            const int look = eng->lookaheadSamples();
            const bool align = anyLive && look > 0;
            // the engine's zones run look-ahead late against the music it was
            // given; with engine zones in play, hold the music and the authored
            // zones back by the same amount so everything lands together
            if (align) eng->delayMusic(mL.data(), mR.data(), mL.data(), mR.data(), n);
            static thread_local std::vector<float> ab[NZONES];
            float* outZ[NZONES];
            const float* vibR[NZONES];
            for (int z = 0; z < NZONES; z++) {
                if (authored[z]) {
                    ab[z].resize(n);
                    if (align) for (int i = 0; i < n; i++) { self->bedDelay_[z].push(authored[z][i]); ab[z][i] = self->bedDelay_[z].tap(look); }
                    else std::memcpy(ab[z].data(), authored[z], (size_t)n * sizeof(float));
                    outZ[z] = ab[z].data(); vibR[z] = nullptr;
                } else {
                    outZ[z] = vibPtr[z]; vibR[z] = eng->rightOut(z);
                }
            }
            self->micFx.bodyEcho(outZ, vibR, n);
            mixOutputBlock(self, eng, mL.data(), mR.data(), outZ, vibR, dst, n, ch);
            return;
        }
    }
    eng->process(L.data(), R.data(), vibPtr, n);
    // speakers get the music held back by the sync look-ahead, in step with the felt output
    eng->delayMusic(mL.data(), mR.data(), mL.data(), mR.data(), n);
    const float* vibR[NZONES];
    for (int z = 0; z < NZONES; z++) vibR[z] = eng->rightOut(z);
    self->micFx.bodyEcho(vibPtr, vibR, n);
    mixOutputBlock(self, eng, mL.data(), mR.data(), vibPtr, vibR, dst, n, ch);
}
