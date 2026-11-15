#include "RemasterProtocol.h"

#include <cctype>
#include <cstdlib>
#include <vector>

namespace chost {

namespace {

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        --b;
    return s.substr(a, b - a);
}

std::vector<std::string> fields(const std::string& s)
{
    std::vector<std::string> out;
    size_t start = 0;
    for (;;)
    {
        const size_t tab = s.find('\t', start);
        out.push_back(trim(s.substr(start, tab == std::string::npos ? std::string::npos : tab - start)));
        if (tab == std::string::npos)
            return out;
        start = tab + 1;
    }
}

bool startsWith(const std::string& s, const char* prefix, std::string& rest)
{
    const std::string p(prefix);
    if (s.compare(0, p.size(), p) != 0)
        return false;
    rest = s.substr(p.size());
    return true;
}

// "lead", "kick", "other_2": what roles and stem ids look like; never a path or a number
bool isRole(const std::string& s)
{
    if (s.empty() || std::isdigit(static_cast<unsigned char>(s[0])))
        return false;
    for (const char c : s)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-')
            return false;
    return true;
}

bool isNumber(const std::string& s)
{
    if (s.empty())
        return false;
    char* end = nullptr;
    std::strtod(s.c_str(), &end);
    return end != nullptr && *end == '\0';
}

bool isPath(const std::string& s)
{
    return !s.empty() && !isNumber(s);
}

} // namespace

RemasterEvent parseRemasterLine(const std::string& raw)
{
    RemasterEvent e;
    // the workflow's own lines start in the first column; a tool's lines it passes on are indented
    if (raw.empty() || std::isspace(static_cast<unsigned char>(raw[0])))
        return e;
    std::string line = raw;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();
    std::string rest;

    if (startsWith(line, "PROGRESS ", rest))
    {
        rest = trim(rest);
        const size_t space = rest.find(' ');
        const std::string pct = rest.substr(0, space);
        if (!isNumber(pct))
            return e;
        e.kind = RemasterEvent::Kind::Progress;
        e.percent = std::atoi(pct.c_str());
        e.label = space == std::string::npos ? std::string() : trim(rest.substr(space + 1));
    }
    else if (startsWith(line, "ANALYZE ", rest))
    {
        const std::vector<std::string> f = fields(rest);
        if (!isNumber(f[0]))
            return e;
        e.kind = RemasterEvent::Kind::Analyze;
        e.number = std::strtod(f[0].c_str(), nullptr);
        e.label = f.size() > 1 ? f[1] : std::string();
    }
    else if (startsWith(line, "STEM ", rest))
    {
        const std::vector<std::string> f = fields(rest);
        if (f.size() < 3 || !isRole(f[0]) || !isPath(f[2]))
            return e;
        e.kind = RemasterEvent::Kind::Stem;
        e.role = f[0];
        e.label = f[1];
        e.path = f[2];
    }
    else if (startsWith(line, "TUNED ", rest) || startsWith(line, "MIDI ", rest))
    {
        const bool tuned = line[0] == 'T';
        const std::vector<std::string> f = fields(rest);
        if (f.size() < 2 || !isRole(f[0]) || !isPath(f[1]))
            return e;
        e.kind = tuned ? RemasterEvent::Kind::Tuned : RemasterEvent::Kind::Midi;
        e.role = f[0];
        e.path = f[1];
    }
    else if (startsWith(line, "BALANCE ", rest))
    {
        const std::vector<std::string> f = fields(rest);
        if (f.size() < 3 || !isRole(f[0]) || !isNumber(f[1]) || !isNumber(f[2]))
            return e;
        e.kind = RemasterEvent::Kind::Balance;
        e.role = f[0];
        e.number = std::strtod(f[1].c_str(), nullptr);
        e.pan = std::strtod(f[2].c_str(), nullptr);
    }
    else if (startsWith(line, "MASTER ", rest))
    {
        rest = trim(rest);
        if (!isPath(rest))
            return e;
        e.kind = RemasterEvent::Kind::Master;
        e.path = rest;
    }
    return e;
}

} // namespace chost
