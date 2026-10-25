#include "MixBalance.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace chost {

double balanceTarget(const std::string& role, VocalStyle style)
{
    static const std::map<std::string, double> targets{
        {"lead", 0.0},    {"vocals", 0.0},  {"backing", -6.0}, {"drums", -2.0},   {"kick", -5.0},   {"snare", -6.0},
        {"toms", -11.0},  {"hh", -13.0},    {"ride", -14.0},   {"crash", -14.0},  {"bass", -4.0},   {"guitar", -6.0},
        {"piano", -6.0},  {"keys", -6.0},   {"other", -7.0},   {"strings", -9.0}, {"pad", -10.0},   {"chords", -7.0},
    };
    const auto it = targets.find(role);
    const double target = it != targets.end() ? it->second : -6.0;
    const bool vocal = role == "lead" || role == "vocals" || role == "backing";
    if (vocal)
        return target;
    // the band moves rather than the vocals, so the vocal level stays where the reference puts it
    return target + (style == VocalStyle::Forward ? -2.0 : style == VocalStyle::Blend ? 2.0 : 0.0);
}

std::vector<float> balanceVolumes(const std::vector<BalancePart>& parts, VocalStyle style, double reference)
{
    constexpr double kTop = 1.27; // carla's +2 dB
    std::vector<double> wanted;
    for (const BalancePart& p : parts)
    {
        if (p.silent)
        {
            wanted.push_back(p.volume);
            continue;
        }
        const double change = reference + balanceTarget(p.role, style) - p.loudness;
        wanted.push_back(std::max(1e-4, double(p.volume)) * std::pow(10.0, change / 20.0));
    }
    double highest = 0.0;
    for (size_t i = 0; i < parts.size(); ++i)
        if (!parts[i].silent)
            highest = std::max(highest, wanted[i]);
    const double down = highest > kTop ? kTop / highest : 1.0;
    std::vector<float> volumes;
    for (size_t i = 0; i < parts.size(); ++i)
        volumes.push_back(parts[i].silent ? parts[i].volume : static_cast<float>(std::min(kTop, wanted[i] * down)));
    return volumes;
}

} // namespace chost
