// Live input, the platform-neutral half: one captured block in, the picked
// L/R pair (with gain, the clip/loop guard and the meter) into the ring.
#include "audio_in.h"
#include <algorithm>
#include <cmath>

void InputUnit::processBlock(const float* in, int ch, int nFrames) {
    if (ch <= 0 || nFrames <= 0 || !ring_) return;
    int cl = std::min(std::max(chanL.load(), 0), ch - 1);
    int cr = std::min(std::max(chanR.load(), 0), ch - 1);
    float g = gain.load();
    static thread_local std::vector<float> L, R;
    L.resize(nFrames); R.resize(nFrames);
    // the interface's own peak (before GAIN) decides the guard; the guard's
    // trim is then applied with a ~50 ms slew so steps are inaudible
    float pkRaw = 0, pkGain = 0;
    for (int i = 0; i < nFrames; i++) {
        float l = in[(size_t)i * ch + cl], r = in[(size_t)i * ch + cr];
        pkRaw = std::max(pkRaw, std::max(std::fabs(l), std::fabs(r)));
        L[i] = l * g; R[i] = r * g;
    }
    pkGain = pkRaw * g;
    const float dt = nFrames / 48000.0f;
    // FIT: remember the loudest post-GAIN peak, letting it fall over ~4 s
    hotPk_ = std::max(pkGain, hotPk_ * std::exp(-dt / 4.0f));
    hotPeak.store(hotPk_);
    float clip = clipHold.load();
    if (pkRaw > 0.97f) {
        clip = 1.5f;
        hotSec_ += dt;
        coolSec_ = 0;
        if (hotSec_ > 0.25f) {            // sustained full scale: clip or loop
            hotSec_ = 0;
            trimTarget_ = std::max(1.0f / 64.0f, trimTarget_ * 0.5f);
        }
    } else {
        clip = std::max(0.0f, clip - dt);
        hotSec_ = std::max(0.0f, hotSec_ - dt);
        if (pkRaw < 0.5f) {
            coolSec_ += dt;
            if (coolSec_ > 4.0f && trimTarget_ < 1.0f) {   // quiet: ease back 1 dB
                coolSec_ = 0;
                trimTarget_ = std::min(1.0f, trimTarget_ * 1.122f);
            }
        } else coolSec_ = 0;
    }
    clipHold.store(clip);
    const float aT = 1.0f / (0.05f * 48000.0f);
    const float ceil = std::min(1.0f, std::max(0.25f, limitCeil.load()));
    const float rel = 1.0f - std::exp(-1.0f / (0.1f * 48000.0f));
    float t = trim_, pk = 0, lg = limG_, minG = 1.0f;
    for (int i = 0; i < nFrames; i++) {
        t += (trimTarget_ - t) * aT;
        float l = L[i] * t, r = R[i] * t;
        float a = std::max(std::fabs(l), std::fabs(r));
        lg += (1.0f - lg) * rel;
        if (a * lg > ceil) lg = ceil / a;            // never over the ceiling
        minG = std::min(minG, lg);
        L[i] = l * lg; R[i] = r * lg;
        pk = std::max(pk, std::max(std::fabs(L[i]), std::fabs(R[i])));
    }
    trim_ = t; limG_ = lg;
    limitRed.store(-20.0f * std::log10(std::max(minG, 1e-4f)));
    autoTrim.store(t);
    float prev = peak.load() * 0.9f;
    peak.store(pk > prev ? pk : prev);
    ring_->pushPlanar(L.data(), R.data(), nFrames);
}
