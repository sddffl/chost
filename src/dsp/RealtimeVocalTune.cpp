#include "RealtimeVocalTune.h"

namespace chost::dsp {

// ------------------------------------------------------------------ Scales & Keys
std::vector<std::string> keyNames() {
    return {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
}

std::vector<std::string> scaleNames() {
    return {
        "Chromatic", "Major", "Minor", "Pentatonic major",
        "Pentatonic minor", "Blues", "Dorian", "Mixolydian", "Harmonic minor"
    };
}

unsigned scaleMask(int scale, int key) {
    static const unsigned kSets[NumScales] = {
        0xFFF,                                                             // chromatic
        1u | 1u << 2 | 1u << 4 | 1u << 5 | 1u << 7 | 1u << 9 | 1u << 11,   // major
        1u | 1u << 2 | 1u << 3 | 1u << 5 | 1u << 7 | 1u << 8 | 1u << 10,   // minor
        1u | 1u << 2 | 1u << 4 | 1u << 7 | 1u << 9,                        // pentatonic major
        1u | 1u << 3 | 1u << 5 | 1u << 7 | 1u << 10,                       // pentatonic minor
        1u | 1u << 3 | 1u << 5 | 1u << 6 | 1u << 7 | 1u << 10,             // blues
        1u | 1u << 2 | 1u << 3 | 1u << 5 | 1u << 7 | 1u << 9 | 1u << 10,   // dorian
        1u | 1u << 2 | 1u << 4 | 1u << 5 | 1u << 7 | 1u << 9 | 1u << 10,   // mixolydian
        1u | 1u << 2 | 1u << 3 | 1u << 5 | 1u << 7 | 1u << 8 | 1u << 11,   // harmonic minor
    };
    const unsigned set = kSets[clamp(scale, 0, static_cast<int>(NumScales) - 1)];
    key = ((key % 12) + 12) % 12;
    unsigned out = 0;
    for (int i = 0; i < 12; ++i) {
        if (set & (1u << i)) out |= 1u << ((i + key) % 12);
    }
    return out;
}

static inline bool inMask(int note, unsigned mask) {
    return (mask >> (((note % 12) + 12) % 12)) & 1u;
}

double nearestNote(double key, unsigned mask) {
    if ((mask & 0xFFFu) == 0) return key;
    const int base = static_cast<int>(std::floor(key));
    double best = key, bestDist = 1.0e9;
    for (int n = base - 6; n <= base + 7; ++n) {
        if (!inMask(n, mask)) continue;
        const double d = std::fabs(key - static_cast<double>(n));
        if (d < bestDist) {
            bestDist = d;
            best = static_cast<double>(n);
        }
    }
    return best;
}

// ------------------------------------------------------------------ Pitch Detector
void PitchDetector::prepare(double sr, int monoRingSize) {
    sr_ = sr;
    dec_ = sr >= 64000.0 ? 8 : (sr >= 32000.0 ? 4 : 2);
    defaultT_ = sr / 200.0;
    centre_ = static_cast<int>(std::lround(kCentreSeconds * sr));

    for (int r = 0; r < 2; ++r) {
        const double fmin = r == 0 ? 60.0 : 110.0;
        const double fmax = r == 0 ? 600.0 : 1200.0;
        fineMin_[r] = std::max(2, static_cast<int>(std::floor(sr / fmax)));
        fineMax_[r] = std::min(static_cast<int>(sr * 0.03), static_cast<int>(std::ceil(sr / fmin)));
        tmin_[r] = std::max(2, fineMin_[r] / dec_);
        tmax_[r] = fineMax_[r] / dec_;
        win_[r] = std::max(tmax_[r] + 16, (2 * fineMax_[r]) / dec_ + 8);
    }

    const int maxWin = std::max(win_[0], win_[1]);
    const int maxLag = std::max(tmax_[0], tmax_[1]);
    decBuf_.assign(static_cast<size_t>(maxWin + maxLag + 16), 0.0f);
    w_.assign(static_cast<size_t>(maxWin), 0.0f);
    lin_.assign(static_cast<size_t>(maxWin), 0.0f);
    sq_.assign(static_cast<size_t>(maxWin + maxLag + 16), 0.0);
    psq_.assign(static_cast<size_t>(maxLag + 1), 0.0);
    nsdf_.assign(static_cast<size_t>(maxLag + 1), 0.0f);
    vals_.assign(static_cast<size_t>(std::max(fineMax_[0], fineMax_[1]) + 16), 0.0);

    for (int i = 0; i < maxWin; ++i) {
        w_[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(kTwoPi * i / (maxWin - 1)));
        lin_[static_cast<size_t>(i)] = 1.0f - static_cast<float>(i) / static_cast<float>(maxWin);
    }

    const double cutoff = 1300.0;
    for (auto& f : lp_) {
        f.lowpass(cutoff, 0.7071, sr);
    }
    mono_.prepare(monoRingSize);
    reset();
}

void PitchDetector::reset() {
    mono_.clear();
    std::fill(decBuf_.begin(), decBuf_.end(), 0.0f);
    std::fill(sq_.begin(), sq_.end(), 0.0);
    decW_ = decPhase_ = hopCount_ = 0;
    n_ = count_ = 0;
    lastVoiced_ = false;
    lastPeriod_ = defaultT_;
    for (auto& f : lp_) f.reset();
}

void PitchDetector::push(float x) {
    mono_.at(n_) = x;
    const float y = lp_[1].process(lp_[0].process(x));
    if (++decPhase_ >= dec_) {
        decPhase_ = 0;
        decBuf_[static_cast<size_t>(decW_)] = y;
        decW_ = (decW_ + 1) % static_cast<int>(decBuf_.size());
        if (++hopCount_ >= kHopDec) {
            hopCount_ = 0;
            analyse();
        }
    }
    ++n_;
}

void PitchDetector::analyse() {
    const int R = range_;
    const int W = win_[R], Tmin = tmin_[R], Tmax = tmax_[R];
    const int bufN = static_cast<int>(decBuf_.size());
    const int start = (decW_ - W - Tmax + bufN * 4) % bufN;

    sq_[0] = 0.0;
    for (int i = 0; i < W + Tmax; ++i) {
        const double v = decBuf_[static_cast<size_t>((start + i) % bufN)];
        sq_[static_cast<size_t>(i + 1)] = sq_[static_cast<size_t>(i)] + v * v;
    }

    const double e0 = sq_[static_cast<size_t>(Tmax + W)] - sq_[static_cast<size_t>(Tmax)];
    if (e0 < 1.0e-7 * W) {
        Estimate e;
        e.time = static_cast<double>(n_ - centre_);
        e.period = lastPeriod_;
        e.clarity = 0.0f;
        e.voiced = false;
        est_[(count_++) % kMaxEst] = e;
        lastVoiced_ = false;
        return;
    }

    for (int t = Tmin; t <= Tmax; ++t) {
        const double et = sq_[static_cast<size_t>(Tmax - t + W)] - sq_[static_cast<size_t>(Tmax - t)];
        psq_[static_cast<size_t>(t)] = e0 + et;
    }

    double bestVal = -1.0;
    int bestLag = -1;
    for (int t = Tmin; t <= Tmax; ++t) {
        double r = 0.0;
        const int s0 = (start + Tmax) % bufN;
        const int st = (start + Tmax - t + bufN) % bufN;
        for (int i = 0; i < W; ++i) {
            r += decBuf_[static_cast<size_t>((s0 + i) % bufN)] * decBuf_[static_cast<size_t>((st + i) % bufN)];
        }
        const double d = psq_[static_cast<size_t>(t)];
        const double nsdf = d > 1.0e-9 ? (2.0 * r) / d : 0.0;
        nsdf_[static_cast<size_t>(t)] = static_cast<float>(nsdf);
        if (nsdf > bestVal) {
            bestVal = nsdf;
            bestLag = t;
        }
    }

    int lag = bestLag;
    const double thresh = 0.82 * bestVal;
    for (int t = Tmin + 1; t < Tmax; ++t) {
        if (nsdf_[static_cast<size_t>(t)] > nsdf_[static_cast<size_t>(t - 1)] &&
            nsdf_[static_cast<size_t>(t)] >= nsdf_[static_cast<size_t>(t + 1)] &&
            nsdf_[static_cast<size_t>(t)] >= thresh) {
            lag = t;
            break;
        }
    }

    // Refinement pass at full rate
    const int fineMin = std::max(fineMin_[R], (lag - 1) * dec_);
    const int fineMax = std::min(fineMax_[R], (lag + 1) * dec_ + dec_);
    const int fineWin = std::min(static_cast<int>(sr_ * 0.025), fineMax * 2);
    const int64 baseT = n_ - centre_;

    double fbest = -1.0;
    double period = static_cast<double>(lag * dec_);
    for (int t = fineMin; t <= fineMax; ++t) {
        double r = 0.0, e1 = 0.0, e2 = 0.0;
        for (int i = 0; i < fineWin; ++i) {
            const double s1 = mono_.at(baseT + i);
            const double s2 = mono_.at(baseT + i - t);
            r += s1 * s2;
            e1 += s1 * s1;
            e2 += s2 * s2;
        }
        const double denom = e1 + e2;
        const double score = denom > 1.0e-9 ? (2.0 * r) / denom : 0.0;
        if (score > fbest) {
            fbest = score;
            period = static_cast<double>(t);
        }
    }

    const bool voiced = fbest >= 0.45 && e0 > 1.0e-5 * W;
    if (voiced) lastPeriod_ = period;
    lastVoiced_ = voiced;

    Estimate e;
    e.time = static_cast<double>(baseT);
    e.period = voiced ? period : lastPeriod_;
    e.clarity = static_cast<float>(fbest);
    e.voiced = voiced;
    est_[(count_++) % kMaxEst] = e;
}

bool PitchDetector::periodAt(double time, double& period) const {
    if (count_ <= 0) {
        period = defaultT_;
        return false;
    }
    // Binary search timestamp
    int lo = 0, hi = std::min(static_cast<int>(count_), kMaxEst) - 1;
    const int64 startIdx = count_ > kMaxEst ? count_ - kMaxEst : 0;
    while (hi - lo > 1) {
        const int mid = (lo + hi) / 2;
        if (est_[(startIdx + mid) % kMaxEst].time <= time) lo = mid;
        else hi = mid;
    }
    const auto& e = est_[(startIdx + lo) % kMaxEst];
    period = e.period;
    return e.voiced;
}

// ------------------------------------------------------------------ GrainShifter (PSOLA)
GrainShifter::Tables::Tables() {
    for (int i = 0; i < kWin; ++i) {
        win[i] = static_cast<float>(0.5 - 0.5 * std::cos(kTwoPi * i / (kWin - 1)));
    }
    for (int p = 0; p < kPhases; ++p) {
        const double frac = static_cast<double>(p) / static_cast<double>(kPhases);
        for (int k = 0; k < kTaps; ++k) {
            const double x = static_cast<double>(k - kTaps / 2 + 1) - frac;
            const double s = std::fabs(x) < 1.0e-6 ? 1.0 : std::sin(kPi * x) / (kPi * x);
            const double w = 0.54 - 0.46 * std::cos(kTwoPi * (k + 0.5) / kTaps);
            sinc[p][k] = static_cast<float>(s * w);
        }
    }
}

const GrainShifter::Tables& GrainShifter::tables() {
    static const Tables t;
    return t;
}

void GrainShifter::prepare(double sr, int lookahead, float* const* hist, int64 mask, int nch) {
    tab_ = &tables();
    lookahead_ = static_cast<double>(lookahead);
    hist_[0] = hist[0];
    hist_[1] = nch > 1 ? hist[1] : hist[0];
    mask_ = mask;
    nch_ = nch;
    minT_ = sr / 1200.0;
    maxT_ = sr / 60.0;
    scratch_.assign(2048, 0.0f);
    scratchSq_.assign(2048, 0.0);
}

void GrainShifter::reset(double startTime) {
    n_ = 0;
    hasPrev_ = false;
    nextMark_ = lastMark_ = spawnAt_ = startTime;
    prevSpacing_ = prevT_ = 240.0;
    delta_ = 0.0;
}

double GrainShifter::localPeriod(double estimate, int64) {
    return clamp(estimate, minT_, maxT_);
}

void GrainShifter::spawn(const GrainCtl& c, int64 newest) {
    const double outPeriod = clamp(c.period / std::max(0.25, c.pitch), minT_, maxT_);
    const double inPeriod = clamp(c.period, minT_, maxT_);

    if (n_ >= kMaxGrains) return;
    Grain& g = g_[n_++];
    g.centre = nextMark_;
    const double span = inPeriod * 1.5;
    g.start = g.centre - span;
    g.end = g.centre + span;
    g.invL = span > 0.0 ? 1.0 / span : 1.0;
    g.invR = g.invL;
    g.src = (static_cast<double>(newest) - lookahead_) - inPeriod;
    g.rate = clamp(c.formant, 0.25, 4.0);
    g.gain = 1.0f;
    g.ident = std::fabs(c.pitch - 1.0) < 1.0e-3 && std::fabs(c.formant - 1.0) < 1.0e-3;

    lastMark_ = nextMark_;
    nextMark_ += outPeriod;
    spawnAt_ = nextMark_ - outPeriod * 0.5;
}

void GrainShifter::render(int64 t, float* out) {
    out[0] = out[1] = 0.0f;
    int alive = 0;
    const double now = static_cast<double>(t);

    for (int i = 0; i < n_; ++i) {
        Grain& g = g_[i];
        if (now < g.start || now >= g.end) {
            if (now < g.start) g_[alive++] = g;
            continue;
        }
        const double x = now < g.centre ? (now - g.start) * g.invL : (g.end - now) * g.invR;
        const double winIdx = clamp(x, 0.0, 1.0) * (kWin - 1);
        const float w = tab_->win[static_cast<int>(winIdx)];

        const double srcPos = g.src + (now - g.centre) * g.rate;
        const int64 isrc = static_cast<int64>(std::floor(srcPos));
        const double frac = srcPos - static_cast<double>(isrc);
        const int phase = clamp(static_cast<int>(frac * kPhases), 0, kPhases - 1);
        const float* sinc = tab_->sinc[phase];

        float sl = 0.0f, sr = 0.0f;
        for (int k = 0; k < kTaps; ++k) {
            const int64 idx = isrc + (k - kTaps / 2 + 1);
            sl += hist_[0][idx & mask_] * sinc[k];
            sr += hist_[1][idx & mask_] * sinc[k];
        }

        out[0] += sl * w * g.gain;
        out[1] += sr * w * g.gain;
        g_[alive++] = g;
    }
    n_ = alive;
}

// ------------------------------------------------------------------ RealtimeVocalTune
RealtimeVocalTune::RealtimeVocalTune() {
    setSpeed(35.0);
}

void RealtimeVocalTune::prepare(double sampleRate) {
    sr_ = sampleRate;
    latency_ = static_cast<int>(std::lround(0.018 * sr_)); // 18 ms standard latency
    const int ringSize = latency_ + static_cast<int>(sr_ / 50.0) * 8 + 128;

    for (auto& h : hist_) h.prepare(ringSize);
    det_.prepare(sr_, ringSize);

    float* hp[2] = {hist_[0].d, hist_[1].d};
    sh_.prepare(sr_, latency_ - 6, hp, hist_[0].mask, 2);
    reset();
}

void RealtimeVocalTune::reset() {
    for (auto& h : hist_) h.clear();
    det_.reset();
    n_ = 0;
    sh_.reset(-static_cast<double>(latency_));
    wasVoiced_ = false;
    corr_ = 0.0;
    lastMark_ = -static_cast<double>(latency_);
}

void RealtimeVocalTune::setKeyAndScale(int key, int scale) {
    mask_ = scaleMask(scale, key);
}

void RealtimeVocalTune::setSpeed(double retuneTimeMs) {
    speedMs_ = retuneTimeMs;
    snap_ = speedMs_ < 0.5;
    rateK_ = snap_ ? 0.0 : std::log(10.0) / (0.001 * speedMs_ * sr_);
}

void RealtimeVocalTune::setAmount(float amount) {
    amount_ = clamp(static_cast<double>(amount), 0.0, 1.0);
}

void RealtimeVocalTune::setTranspose(int semitones) {
    transpose_ = static_cast<double>(clamp(semitones, -12, 12));
}

void RealtimeVocalTune::setFormantPreserve(bool preserve) {
    preserve_ = preserve;
}

void RealtimeVocalTune::setFormantShift(float semitones) {
    fshift_ = std::pow(2.0, static_cast<double>(clamp(semitones, -12.0f, 12.0f)) / 12.0);
}

void RealtimeVocalTune::setMix(float mix) {
    mix_ = clamp(mix, 0.0f, 1.0f);
}

void RealtimeVocalTune::setVoiceRange(int range) {
    det_.setRange(range);
}

void RealtimeVocalTune::spawnGrain() {
    const double s = sh_.nextMark();
    GrainCtl c;
    double T = 0.0;
    c.voiced = det_.periodAt(s, T);
    c.period = T;

    if (!c.voiced) {
        wasVoiced_ = false;
        corr_ = 0.0;
        lastMark_ = s;
        sh_.spawn(c, n_);
        return;
    }

    T = sh_.localPeriod(T, n_);
    c.period = T;
    const double keyIn = 69.0 + 12.0 * std::log2(sr_ / (T * 440.0));
    double target = keyIn;

    if (mask_ != 0) {
        const double cand = nearestNote(keyIn, mask_);
        const bool held = wasVoiced_ && ((mask_ >> ((((static_cast<int>(note_) % 12) + 12) % 12))) & 1u);
        // Hysteresis prevents note flipping on delicate vibratos
        if (!held || (cand != note_ && std::fabs(keyIn - note_) - std::fabs(keyIn - cand) > 0.3)) {
            note_ = cand;
        }
        target = note_;
    }

    const double want = (target - keyIn) * amount_;
    if (snap_) corr_ = want;
    else if (!wasVoiced_) corr_ = 0.0;
    else corr_ += (want - corr_) * (1.0 - std::exp(-std::max(0.0, s - lastMark_) * rateK_));

    wasVoiced_ = true;
    lastMark_ = s;
    c.centre = (amount_ > 0.0 && mask_ != 0) || transpose_ != 0.0 || fshift_ != 1.0;
    c.pitch = std::pow(2.0, (corr_ + transpose_) / 12.0);
    c.formant = preserve_ ? fshift_ : c.pitch * fshift_;
    sh_.spawn(c, n_);
}

void RealtimeVocalTune::process(float* left, float* right, int numSamples) {
    float* hp[2] = {hist_[0].d, hist_[1].d};
    const int64 mask = hist_[0].mask;

    for (int i = 0; i < numSamples; ++i) {
        float l = left[i];
        float r = right ? right[i] : l;
        if (!std::isfinite(l)) l = 0.0f;
        if (!std::isfinite(r)) r = 0.0f;

        hp[0][n_ & mask] = l;
        hp[1][n_ & mask] = r;
        det_.push(0.5f * (l + r));

        const int64 t = n_ - latency_;
        int guard = 0;
        while (sh_.due(t) && guard++ < 32) {
            spawnGrain();
        }

        float w[2];
        sh_.render(t, w);

        const float dl = hp[0][t & mask];
        const float dr = hp[1][t & mask];

        left[i] = dl + mix_ * (w[0] - dl);
        if (right) {
            right[i] = dr + mix_ * (w[1] - dr);
        }
        ++n_;
    }

    const PitchDetector::Estimate* e = det_.latest();
    const bool voiced = e && e->voiced;
    mPitch_.store(voiced ? static_cast<float>(69.0 + 12.0 * std::log2(sr_ / (e->period * 440.0))) : 0.0f, std::memory_order_relaxed);
    mCents_.store(static_cast<float>(corr_ * 100.0), std::memory_order_relaxed);
    mVoiced_.store(voiced ? 1.0f : 0.0f, std::memory_order_relaxed);
}

} // namespace chost::dsp
