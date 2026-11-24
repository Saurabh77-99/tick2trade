#pragma once
// Minimal "--key value" / "--flag" argument parsing for the CLI.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace hft::cli {

class Args {
public:
    Args(int argc, char** argv, int first) {
        for (int i = first; i < argc; ++i) {
            std::string a = argv[i];
            if (a.rfind("--", 0) == 0) {
                std::string key = a.substr(2);
                if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) opts_[key] = argv[++i];
                else opts_[key] = "";
            } else {
                positional_.push_back(a);
            }
        }
    }

    bool has(const std::string& k) const { return opts_.count(k) > 0; }

    std::string str(const std::string& k, const std::string& def = "") const {
        auto it = opts_.find(k);
        return it == opts_.end() ? def : it->second;
    }

    std::uint64_t num(const std::string& k, std::uint64_t def) const {
        auto it = opts_.find(k);
        return it == opts_.end() || it->second.empty() ? def : std::strtoull(it->second.c_str(), nullptr, 10);
    }

    const std::vector<std::string>& positional() const { return positional_; }

private:
    std::map<std::string, std::string> opts_;
    std::vector<std::string> positional_;
};

// "09:30" or "09:30:15" -> nanoseconds since midnight.
inline std::uint64_t parse_clock(const std::string& s) {
    unsigned h = 0, m = 0, sec = 0;
    std::sscanf(s.c_str(), "%u:%u:%u", &h, &m, &sec);
    return (std::uint64_t{h} * 3600 + m * 60 + sec) * 1'000'000'000ull;
}

inline std::string format_clock(std::uint64_t ns) {
    const std::uint64_t s = ns / 1'000'000'000ull;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02llu:%02llu:%02llu.%03llu", (unsigned long long)(s / 3600),
                  (unsigned long long)(s / 60 % 60), (unsigned long long)(s % 60),
                  (unsigned long long)(ns / 1'000'000 % 1000));
    return buf;
}

}  // namespace hft::cli
