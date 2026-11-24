#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace naomi::web {

inline constexpr const char *kGoogleAuthUserAgent =
    "Mozilla/5.0 (X11; Linux x86_64; rv:136.0) Gecko/20100101 Firefox/136.0";

inline constexpr std::array<const char *, 9> kClientHintHeaders = {
    "Sec-CH-UA",
    "Sec-CH-UA-Mobile",
    "Sec-CH-UA-Platform",
    "Sec-CH-UA-Full-Version",
    "Sec-CH-UA-Full-Version-List",
    "Sec-CH-UA-Arch",
    "Sec-CH-UA-Bitness",
    "Sec-CH-UA-Model",
    "Sec-CH-UA-Platform-Version",
};

inline std::string normalizeHost(std::string_view host)
{
    while (!host.empty() && (host.front() == ' ' || host.front() == '\t'))
        host.remove_prefix(1);
    while (!host.empty() && (host.back() == ' ' || host.back() == '\t' || host.back() == '.'))
        host.remove_suffix(1);
    std::string out(host.size(), '\0');
    std::transform(host.begin(), host.end(), out.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return out;
}

inline bool hostMatches(std::string_view host, std::string_view domain)
{
    if (host.empty() || domain.empty())
        return false;
    if (host == domain)
        return true;
    return host.size() > domain.size()
           && host.compare(host.size() - domain.size(), domain.size(), domain) == 0
           && host[host.size() - domain.size() - 1] == '.';
}

inline bool isGoogleAuthHost(std::string_view rawHost)
{
    const std::string host = normalizeHost(rawHost);
    return hostMatches(host, "accounts.google.com")
           || hostMatches(host, "accounts.youtube.com")
           || hostMatches(host, "gds.google.com");
}

inline bool isGoogleAuthSupportHost(std::string_view rawHost)
{
    const std::string host = normalizeHost(rawHost);
    return isGoogleAuthHost(host)
           || hostMatches(host, "gstatic.com")
           || hostMatches(host, "googleusercontent.com")
           || hostMatches(host, "apis.google.com")
           || hostMatches(host, "ogs.google.com")
           || hostMatches(host, "myaccount.google.com");
}

inline bool shouldBypassBlockForGoogleAuth(std::string_view requestHost, std::string_view firstPartyHost)
{
    const std::string req = normalizeHost(requestHost);
    const std::string page = normalizeHost(firstPartyHost);
    if (isGoogleAuthHost(req) || isGoogleAuthHost(page))
        return true;
    if (hostMatches(page, "myaccount.google.com"))
        return true;
    return false;
}

inline bool shouldOverrideAuthHeaders(std::string_view requestHost, std::string_view firstPartyHost)
{
    const std::string req = normalizeHost(requestHost);
    const std::string page = normalizeHost(firstPartyHost);
    if (isGoogleAuthHost(req))
        return true;
    if (isGoogleAuthHost(page) && isGoogleAuthSupportHost(req))
        return true;
    return false;
}

inline std::string cleanChromeUserAgent(std::string_view rawUa)
{
    std::string out(rawUa);
    for (;;) {
        const std::size_t pos = out.find("QtWebEngine/");
        if (pos == std::string::npos)
            break;
        std::size_t end = pos + 12;
        while (end < out.size() && out[end] != ' ')
            ++end;
        std::size_t start = pos;
        if (start > 0 && out[start - 1] == ' ')
            --start;
        else if (end < out.size() && out[end] == ' ')
            ++end;
        out.erase(start, end - start);
    }
    const std::string aarch = "Linux aarch64";
    for (std::size_t pos = 0; (pos = out.find(aarch, pos)) != std::string::npos;) {
        out.replace(pos, aarch.size(), "Linux x86_64");
        pos += 12;
    }
    for (std::size_t pos = 0; (pos = out.find("  ", pos)) != std::string::npos;)
        out.erase(pos, 1);
    while (!out.empty() && out.front() == ' ')
        out.erase(out.begin());
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

} // namespace naomi::web
