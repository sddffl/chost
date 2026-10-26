#include "Loudness.h"

#include <algorithm>
#include <cmath>

namespace chost {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;
    double run(double x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// the K-weighting curve's two stages for any rate, from their analogue prototypes (as in libebur128)
Biquad shelf(double rate)
{
    const double db = 3.999843853973347, f0 = 1681.974450955533, q = 0.7071752369554196;
    const double k = std::tan(kPi * f0 / rate);
    const double vh = std::pow(10.0, db / 20.0);
    const double vb = std::pow(vh, 0.4996667741545416);
    const double a0 = 1.0 + k / q + k * k;
    Biquad f;
    f.b0 = (vh + vb * k / q + k * k) / a0;
    f.b1 = 2.0 * (k * k - vh) / a0;
    f.b2 = (vh - vb * k / q + k * k) / a0;
    f.a1 = 2.0 * (k * k - 1.0) / a0;
    f.a2 = (1.0 - k / q + k * k) / a0;
    return f;
}

Biquad highPass(double rate)
{
    const double f0 = 38.13547087602444, q = 0.5003270373238773;
    const double k = std::tan(kPi * f0 / rate);
    const double a0 = 1.0 + k / q + k * k;
    Biquad f;
    f.b0 = 1.0;
    f.b1 = -2.0;
    f.b2 = 1.0;
    f.a1 = 2.0 * (k * k - 1.0) / a0;
    f.a2 = (1.0 - k / q + k * k) / a0;
    return f;
}

double toLufs(double energy)
{
    return energy > 0.0 ? -0.691 + 10.0 * std::log10(energy) : -200.0;
}

} // namespace

Loudness measureLoudness(const std::vector<float>& samples, uint32_t channels, uint32_t sampleRate)
{
    Loudness result;
    if (channels == 0 || sampleRate == 0 || samples.empty())
        return result;
    const size_t frames = samples.size() / channels;
    const uint32_t used = std::min<uint32_t>(channels, 2);

    // K-weighted squares, summed over the channels (left and right weigh 1), per 100 ms step
    const size_t step = sampleRate / 10;
    std::vector<double> steps(frames / step + 1, 0.0);
    float peak = 0.0f;
    for (uint32_t c = 0; c < used; ++c)
    {
        Biquad a = shelf(sampleRate), b = highPass(sampleRate);
        for (size_t i = 0; i < frames; ++i)
        {
            const float x = samples[i * channels + c];
            peak = std::max(peak, std::fabs(x));
            const double y = b.run(a.run(x));
            steps[i / step] += y * y;
        }
    }
    result.peak = peak > 0.0f ? 20.0 * std::log10(peak) : -120.0;

    // 400 ms blocks every 100 ms (75 % overlap)
    std::vector<double> blocks;
    const size_t whole = frames / step; // only complete steps
    for (size_t s = 0; s + 4 <= whole; ++s)
        blocks.push_back((steps[s] + steps[s + 1] + steps[s + 2] + steps[s + 3]) / (4.0 * step));
    if (blocks.empty() && whole > 0) // shorter than 400 ms: one block of what there is
    {
        double sum = 0.0;
        for (size_t s = 0; s < whole; ++s)
            sum += steps[s];
        blocks.push_back(sum / (whole * step));
    }

    double sum = 0.0;
    size_t count = 0;
    for (double z : blocks)
        if (toLufs(z) > -70.0)
        {
            sum += z;
            ++count;
        }
    if (count == 0)
        return result;
    const double relative = toLufs(sum / count) - 10.0;
    sum = 0.0;
    count = 0;
    for (double z : blocks)
        if (toLufs(z) > -70.0 && toLufs(z) > relative)
        {
            sum += z;
            ++count;
        }
    result.integrated = toLufs(sum / count);
    result.silent = false;
    return result;
}

} // namespace chost
