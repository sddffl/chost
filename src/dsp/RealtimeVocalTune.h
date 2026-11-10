#pragma once
// Native Real-Time Vocal Pitch Correction & Formant Tuning Engine
// Ported & adapted from PULP DAW (TrueNote & PSOLA Pitch/Formant Architecture)
// Bit-accurate, ultra-low latency (~18ms), no external Python required.

#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <cstdint>

namespace chost::dsp {

using int64 = std::int64_t;
constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 6.28318530717958647692;

template <typename T>
inline T clamp(T v, T lo, T hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Biquad filter for coarse voice decimation and anti-aliasing
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;
    void reset() { z1 = z2 = 0; }
    inline float process(float x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return static_cast<float>(y);
    }
    void set(double B0, double B1, double B2, double A0, double A1, double A2) {
        b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0;
    }
    void lowpass(double hz, double q, double sr) {
        const double w = kTwoPi * clamp(hz, 1.0, sr * 0.49) / sr;
        const double c = std::cos(w);
        const double al = std::sin(w) / (2.0 * q);
        set((1.0 - c) / 2.0, 1.0 - c, (1.0 - c) / 2.0, 1.0 + al, -2.0 * c, 1.0 - al);
    }
};

// Power-of-two ring buffer addressed by sample index
struct Ring {
    std::vector<float> buf;
    float* d = nullptr;
    int64 mask = 0;
    void prepare(int minSize) {
        int n = 64;
        while (n < minSize) n <<= 1;
        buf.assign(static_cast<size_t>(n), 0.0f);
        d = buf.data();
        mask = n - 1;
    }
    void clear() { std::fill(buf.begin(), buf.end(), 0.0f); }
    inline float& at(int64 i) { return d[i & mask]; }
    inline float at(int64 i) const { return d[i & mask]; }
};

enum Scale {
    Chromatic,
    Major,
    Minor,
    PentaMajor,
    PentaMinor,
    Blues,
    Dorian,
    Mixolydian,
    HarmonicMinor,
    NumScales
};

std::vector<std::string> keyNames();
std::vector<std::string> scaleNames();
unsigned scaleMask(int scale, int key);
double nearestNote(double key, unsigned mask);

// Monophonic pitch detector using coarse autocorrelation on decimated audio + refined pass
class PitchDetector {
public:
    struct Estimate {
        double time = 0.0;
        double period = 0.0;
        float clarity = 0.0f;
        bool voiced = false;
    };
    static constexpr int kHopDec = 64, kMaxEst = 128;
    static constexpr double kCentreSeconds = 0.0095;

    void prepare(double sr, int monoRingSize);
    void reset();
    void setRange(int r) { range_ = r <= 0 ? 0 : 1; } // 0 = low voice (60..600 Hz), 1 = high voice (110..1200 Hz)
    void push(float x);
    int64 now() const { return n_; }
    bool periodAt(double time, double& period) const;
    const Estimate* latest() const { return count_ > 0 ? &est_[(count_ - 1) % kMaxEst] : nullptr; }
    int64 estimates() const { return count_; }
    Ring& mono() { return mono_; }
    double defaultPeriod() const { return defaultT_; }

private:
    void analyse();
    double sr_ = 48000.0, defaultT_ = 240.0;
    int dec_ = 4, centre_ = 456, range_ = 0;
    int tmin_[2] = {20, 10}, tmax_[2] = {201, 110}, win_[2] = {442, 242};
    int fineMin_[2] = {72, 36}, fineMax_[2] = {860, 470};
    Biquad lp_[2];
    Ring mono_;
    std::vector<float> decBuf_, w_, lin_, nsdf_;
    std::vector<double> sq_, psq_, vals_;
    int decW_ = 0, decPhase_ = 0, hopCount_ = 0;
    int64 n_ = 0, count_ = 0;
    Estimate est_[kMaxEst];
    bool lastVoiced_ = false;
    double lastPeriod_ = 0.0;
};

// PSOLA Grain Control
struct GrainCtl {
    double period = 240.0;
    double pitch = 1.0;
    double formant = 1.0;
    bool voiced = false;
    bool centre = false;
};

// Pitch-Synchronous Overlap-Add (PSOLA) grain resampler with formant preservation
class GrainShifter {
public:
    static constexpr int kMaxGrains = 24, kTaps = 8, kPhases = 256, kWin = 1024;
    void prepare(double sr, int lookahead, float* const* hist, int64 mask, int nch);
    void reset(double startTime);
    inline bool due(int64 t) const { return static_cast<double>(t) >= spawnAt_; }
    double nextMark() const { return nextMark_; }
    double localPeriod(double estimate, int64 newest);
    void spawn(const GrainCtl& c, int64 newest);
    void render(int64 t, float* out);
    int active() const { return n_; }

private:
    struct Grain {
        double centre = 0, start = 0, end = 0, invL = 0, invR = 0, src = 0, rate = 1;
        int64 ioff = 0;
        float gain = 1.0f;
        float coef[kTaps] = {};
        bool ident = false, fixed = true, intTap = true, first = false;
    };
    struct Tables {
        float win[kWin] = {};
        float sinc[kPhases][kTaps] = {};
        Tables();
    };
    static const Tables& tables();
    const Tables* tab_ = nullptr;
    Grain g_[kMaxGrains];
    int n_ = 0;
    bool hasPrev_ = false, prevIdent_ = true, prevUnity_ = true, prevVoiced_ = false;
    double nextMark_ = 0.0, lastMark_ = 0.0, spawnAt_ = 0.0, prevSpacing_ = 240.0, prevT_ = 240.0, delta_ = 0.0;
    double minT_ = 32.0, maxT_ = 880.0, lookahead_ = 0.0;
    std::vector<float> scratch_;
    std::vector<double> scratchSq_;
    float* hist_[2] = {nullptr, nullptr};
    int64 mask_ = 0;
    int nch_ = 1;
    int64 findPulse(int64 end, int period, int half, double at, double& peakEnergy, double& energyAt);
    void centreOnPulse(double s, double T, int64 newest, bool jump);
};

// Complete High-Performance Real-Time Vocal Pitch Corrector (TrueNote)
class RealtimeVocalTune {
public:
    RealtimeVocalTune();
    void prepare(double sampleRate);
    void reset();

    // Configuration
    void setKeyAndScale(int key, int scale); // key: 0=C, 1=C#... scale: Scale enum
    void setSpeed(double retuneTimeMs);     // 0 = Hard snap (Auto-Tune robot), 20-50 = Natural, 200 = Gentle
    void setAmount(float amount);           // 0.0 .. 1.0
    void setTranspose(int semitones);       // -12 .. +12
    void setFormantPreserve(bool preserve); // keep vocal tract formants natural
    void setFormantShift(float semitones);  // -12 .. +12 semitones
    void setMix(float mix);                 // 0.0 (dry) .. 1.0 (wet)
    void setVoiceRange(int range);          // 0 = low (60..600 Hz), 1 = high (110..1200 Hz)

    // In-place processing of stereo audio buffer
    void process(float* left, float* right, int numSamples);

    // Telemetry for GUI meters & waveform pitch overlay
    float currentPitch() const { return mPitch_.load(std::memory_order_relaxed); }
    float currentCorrectionCents() const { return mCents_.load(std::memory_order_relaxed); }
    bool isVoiced() const { return mVoiced_.load(std::memory_order_relaxed) > 0.5f; }

    int latencySamples() const { return latency_; }

private:
    void spawnGrain();
    PitchDetector det_;
    GrainShifter sh_;
    Ring hist_[2];

    double sr_ = 48000.0;
    int latency_ = 864;
    int64 n_ = 0;

    unsigned mask_ = 0xFFF;
    double amount_ = 1.0;
    double speedMs_ = 25.0;
    double rateK_ = 0.0;
    double transpose_ = 0.0;
    double fshift_ = 1.0;
    bool preserve_ = true;
    bool snap_ = false;
    float mix_ = 1.0f;

    double note_ = 69.0;
    double corr_ = 0.0;
    double lastMark_ = 0.0;
    bool wasVoiced_ = false;

    std::atomic<float> mPitch_{0.0f};
    std::atomic<float> mCents_{0.0f};
    std::atomic<float> mVoiced_{0.0f};
};

} // namespace chost::dsp
