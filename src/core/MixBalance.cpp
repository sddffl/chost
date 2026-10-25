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

std::vector<float> autoPan(const std::vector<std::string>& roles)
{
    struct Place {
        float width; // how far out the part goes, 0: the centre
        float side;  // where a lone one goes, -1 left, 1 right
    };
    static const std::map<std::string, Place> places{
        {"lead", {0.0f, 0.0f}},     {"vocals", {0.0f, 0.0f}},  {"kick", {0.0f, 0.0f}},    {"snare", {0.0f, 0.0f}},
        {"bass", {0.0f, 0.0f}},     {"drums", {0.0f, 0.0f}},   {"pad", {0.0f, 0.0f}},     {"backing", {0.5f, -1.0f}},
        {"toms", {0.35f, -1.0f}},   {"hh", {0.3f, 1.0f}},      {"ride", {0.4f, -1.0f}},   {"crash", {0.5f, 1.0f}},
        {"guitar", {0.6f, -1.0f}},  {"piano", {0.35f, 1.0f}},  {"keys", {0.35f, 1.0f}},   {"strings", {0.5f, -1.0f}},
        {"chords", {0.25f, 1.0f}},  {"other", {0.3f, -1.0f}},
    };
    std::map<std::string, int> count, seen;
    for (const std::string& role : roles)
        ++count[role];
    std::vector<float> pans;
    for (const std::string& role : roles)
    {
        const auto it = places.find(role);
        const Place place = it != places.end() ? it->second : places.at("other");
        const int n = count[role];
        const int k = seen[role]++;
        if (place.width == 0.0f)
            pans.push_back(0.0f);
        else if (n == 1)
            pans.push_back(place.side * place.width * 0.7f); // on its own a part needn't go as far out
        else
        {
            // in pairs, one each side, each pair a little further in than the one before
            const int pairs = (n + 1) / 2;
            const float amount = place.width * (1.0f - 0.5f * static_cast<float>(k / 2) / static_cast<float>(pairs));
            pans.push_back((k % 2 == 0 ? place.side : -place.side) * amount);
        }
    }
    return pans;
}

} // namespace chost
