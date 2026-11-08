// AVA OS — live input effects (the mic). Two places in the signal:
//   process():  the input itself, before it is heard and before the engine
//               turns it into vibration — LOW CUT · COMP · ECHO · REVERB
//   bodyEcho(): the zone outputs — each zone's vibration repeats on the next
//               zone along the body (head → feet or feet → head), fading
// Everything defaults off; parameters are atomics the UI writes freely.
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>
#include "dsp.h"
#include "engine.h"

struct MicFx {
    static constexpr float kSR = 48000.0f;
    // 0..1 controls (0 = off where that makes sense)
    std::atomic<float> lowCut{0.0f};     // 0 off, else 30..400 Hz high-pass
    std::atomic<float> comp{0.0f};       // amount: threshold down + ratio up + makeup
    std::atomic<float> echoMix{0.0f}, echoTime{0.35f}, echoFb{0.35f};
    std::atomic<float> revMix{0.0f}, revSize{0.6f};
    std::atomic<float> wetCut{0.45f};    // HIGH CUT on the reverb + echo (not the dry voice)
    std::atomic<float> bodyAmt{0.0f}, bodySpeed{0.4f};
    std::atomic<int>   bodyDir{0};       // 0 head → feet, 1 feet → head

    static float wetCutHz(float v) { return 1000.0f * std::pow(12.0f, v); }   // 1 → 12 kHz
    static float lowCutHz(float v) { return 30.0f * std::pow(400.0f / 30.0f, v); }
    static float echoMs(float v) { return 60.0f + 1140.0f * v * v; }
    static float bodyMs(float v) { return 60.0f + 440.0f * v; }
    static float revSec(float v) { return 0.6f + 5.4f * v * v; }   // rough RT60 shown in the UI

    MicFx() {
        const float s = kSR / 44100.0f;
        static const int kComb[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        static const int kAp[4] = {556, 441, 341, 225};
        for (int c = 0; c < 2; c++) {
            for (int i = 0; i < 8; i++) comb_[c][i].buf.assign((size_t)((kComb[i] + 23 * c) * s), 0.0f);
            for (int i = 0; i < 4; i++) ap_[c][i].buf.assign((size_t)((kAp[i] + 23 * c) * s), 0.0f);
            echo_[c].assign(kEcho, 0.0f);
        }
        for (int z = 0; z < NZONES; z++) body_[z].init(kBody);
        echoD_ = echoMs(echoTime.load()) * 0.001f * kSR;
    }

    // ── input chain, in place on planar L/R ─────────────────────────────
    void process(float* L, float* R, int n) {
        const float lc = lowCut.load(), cp = comp.load();
        const float em = echoMix.load(), rm = revMix.load();
        if (lc <= 0.001f && cp <= 0.001f && em <= 0.001f && rm <= 0.001f && !tail_) return;
        // low cut: re-design only when the knob moved
        if (lc > 0.001f && std::fabs(lc - lcSet_) > 1e-4f) {
            float hz = lowCutHz(lc);
            for (auto& b : hp_) b.highpass(hz, kSR);
            lcSet_ = lc;
        }
        // comp: −6 → −30 dB threshold, 1:1 → 6:1, most of the reduction made back up
        const float thr = -6.0f - 24.0f * cp, ratio = 1.0f + 5.0f * cp;
        const float makeup = -thr * (1.0f - 1.0f / ratio) * 0.7f;
        const float cA = std::exp(-1.0f / (0.003f * kSR)), cR = std::exp(-1.0f / (0.15f * kSR));
        const float echoTarget = echoMs(echoTime.load()) * 0.001f * kSR;
        const float fb = 0.75f * echoFb.load();
        const float room = 0.7f + 0.28f * revSize.load();
        // high cut: a 12 dB/oct low-pass on the wet signals, and the reverb's
        // own damping follows it so the tail darkens as it decays too
        const float wc = wetCut.load();
        const float damp = 0.2f + 0.55f * (1.0f - wc);
        if (std::fabs(wc - wcSet_) > 1e-4f) {
            float hz = wetCutHz(wc);
            for (auto& b : hcRev_) b.lowpass(hz, kSR);
            for (auto& b : hcEcho_) b.lowpass(hz, kSR);
            wcSet_ = wc;
        }
        bool loud = false;
        for (int i = 0; i < n; i++) {
            float l = L[i], r = R[i];
            if (lc > 0.001f) { l = hp_[0].process(l); r = hp_[1].process(r); }
            if (cp > 0.001f) {
                float a = std::max(std::fabs(l), std::fabs(r));
                env_ = (a > env_ ? cA : cR) * (env_ - a) + a;
                float db = 20.0f * std::log10(std::max(env_, 1e-6f));
                float gr = db > thr ? (db - thr) * (1.0f - 1.0f / ratio) : 0.0f;
                float g = std::pow(10.0f, (makeup - gr) / 20.0f);
                l *= g; r *= g;
            }
            float outL = l, outR = r;
            // echo: ping-pong, darkened a little on every repeat, time glides
            float eL = 0, eR = 0;
            if (em > 0.001f || tail_) {
                echoD_ += (echoTarget - echoD_) * 0.0004f;
                eL = tapEcho(0, echoD_); eR = tapEcho(1, echoD_);
                lpL_ += 0.35f * (eL - lpL_); lpR_ += 0.35f * (eR - lpR_);
                echo_[0][ew_] = 0.5f * (l + r) + fb * lpR_;
                echo_[1][ew_] = fb * lpL_;
                ew_ = (ew_ + 1) & (kEcho - 1);
                outL += em * hcEcho_[0].process(eL); outR += em * hcEcho_[1].process(eR);
            }
            // reverb (Freeverb topology): fed the voice plus its echoes
            if (rm > 0.001f || tail_) {
                float in = 0.015f * (l + r + em * (eL + eR));
                float w[2];
                for (int c = 0; c < 2; c++) {
                    float acc = 0;
                    for (auto& cb : comb_[c]) acc += cb.tick(in, room, damp);
                    for (auto& a : ap_[c]) acc = a.tick(acc);
                    w[c] = acc;
                }
                outL += rm * 1.5f * hcRev_[0].process(w[0]); outR += rm * 1.5f * hcRev_[1].process(w[1]);
                if (std::fabs(w[0]) + std::fabs(eL) > 1e-5f) loud = true;
            }
            L[i] = soft(outL); R[i] = soft(outR);
        }
        // keep running after the knobs go to zero until the tails have died
        tail_ = (em > 0.001f || rm > 0.001f) || (tail_ && loud);
    }

    // ── body echo on the zone outputs (in place; R pointers swapped to
    // owned copies when a zone is stereo) ────────────────────────────────
    void bodyEcho(float** zl, const float** zr, int n) {
        const float amt = bodyAmt.load();
        if (amt <= 0.001f) { bodyOn_ = false; return; }
        if (!bodyOn_) { for (auto& d : body_) std::fill(d.buf.begin(), d.buf.end(), 0.0f); bodyOn_ = true; }
        const int dir = bodyDir.load();
        const float target = bodyMs(bodySpeed.load()) * 0.001f * kSR;
        const float g = 0.8f;   // each hop keeps 80% (feet hears ~40% of a head hit)
        static thread_local std::vector<float> rc[NZONES];
        for (int z = 0; z < NZONES; z++)
            if (zr[z]) { rc[z].assign(zr[z], zr[z] + n); zr[z] = rc[z].data(); }
        int ord[NZONES];
        for (int k = 0; k < NZONES; k++) ord[k] = dir ? NZONES - 1 - k : k;
        for (int i = 0; i < n; i++) {
            bodyD_ += (target - bodyD_) * 0.0004f;
            const int d = std::min((int)bodyD_, kBody - 2);
            float e = 0;   // echo arriving at the current zone in the chain
            for (int k = 0; k < NZONES; k++) {
                const int z = ord[k];
                float src = zl[z][i];
                if (zr[z]) src = 0.5f * (src + zr[z][i]);
                if (k > 0) {
                    float o = amt * e;
                    zl[z][i] = std::clamp(zl[z][i] + o, -1.0f, 1.0f);
                    if (zr[z]) rc[z][i] = std::clamp(rc[z][i] + o, -1.0f, 1.0f);
                }
                // what this zone hands on: itself plus what reached it
                body_[z].push(src + e);
                e = g * body_[z].tap(d);
            }
        }
    }

private:
    static constexpr int kEcho = 65536;   // 1.36 s
    static constexpr int kBody = 32768;   // 0.68 s per hop
    struct Comb {
        std::vector<float> buf; int i = 0; float store = 0;
        inline float tick(float in, float fb, float damp) {
            float o = buf[i];
            store = o * (1.0f - damp) + store * damp;
            buf[i] = in + store * fb;
            if (++i >= (int)buf.size()) i = 0;
            return o;
        }
    };
    struct Allpass {
        std::vector<float> buf; int i = 0;
        inline float tick(float in) {
            float b = buf[i], o = -in + b;
            buf[i] = in + b * 0.5f;
            if (++i >= (int)buf.size()) i = 0;
            return o;
        }
    };
    // transparent below 0.8, then bends toward 1 so stacked tails can't slam the limiter
    static inline float soft(float x) {
        float a = std::fabs(x);
        return a <= 0.8f ? x : std::copysign(0.8f + 0.2f * std::tanh((a - 0.8f) * 5.0f), x);
    }
    inline float tapEcho(int c, float d) const {
        int di = (int)d; float f = d - di;
        float a = echo_[c][(ew_ - 1 - di) & (kEcho - 1)], b = echo_[c][(ew_ - 2 - di) & (kEcho - 1)];
        return a + f * (b - a);
    }
    dsp::Biquad hp_[2], hcRev_[2], hcEcho_[2];
    float wcSet_ = -1;
    float lcSet_ = -1, env_ = 0;
    std::vector<float> echo_[2];
    int ew_ = 0;
    float echoD_ = 0, lpL_ = 0, lpR_ = 0;
    Comb comb_[2][8];
    Allpass ap_[2][4];
    bool tail_ = false;
    dsp::DelayLine body_[NZONES];
    float bodyD_ = 0.4f * kSR;
    bool bodyOn_ = false;
};
