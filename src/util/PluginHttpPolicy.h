#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pluginhttp {
inline uint32_t phaseTimeout(uint32_t now, uint32_t deadline, uint32_t ordinary = 60000) {
  if (!deadline) return ordinary;
  const int32_t remaining = static_cast<int32_t>(deadline - now);
  return remaining <= 0 ? 0 : std::max<uint32_t>(1, std::min<uint32_t>(2500, uint32_t(remaining) / 4));
}
inline bool responseHeadersWithinBudget(const std::vector<std::pair<std::string, std::string>>& headers) {
  if (headers.size() > 32) return false;
  size_t bytes = 0;
  for (const auto& h : headers) {
    if (h.first.size() > 4096 - bytes) return false;
    bytes += h.first.size();
    if (h.second.size() > 4096 - bytes) return false;
    bytes += h.second.size();
  }
  return true;
}
inline bool validUrl(const std::string& url) {
  if (url.empty() || url.size() > 2048 || url.find_first_of("\r\n\\# ") != std::string::npos) return false;
  const size_t sep = url.find("://");
  if (sep == std::string::npos) return false;
  std::string scheme = url.substr(0, sep);
  std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return std::tolower(c); });
  if (scheme != "http" && scheme != "https") return false;
  const size_t end = url.find_first_of("/?", sep + 3);
  const auto authority = url.substr(sep + 3, end == std::string::npos ? end : end - sep - 3);
  if (authority.empty() || authority.find_first_of("@%\t") != std::string::npos) return false;
  for (unsigned char c : url)
    if (c < 32 || c == 127) return false;
  const size_t colon = authority.find(':');
  if (colon != std::string::npos) {
    if (colon == 0 || colon + 1 == authority.size()) return false;
    uint32_t port = 0;
    for (size_t i = colon + 1; i < authority.size(); ++i) {
      if (authority[i] < '0' || authority[i] > '9') return false;
      port = port * 10 + authority[i] - '0';
      if (port > 65535) return false;
    }
    if (port == 0) return false;
  }
  return true;
}
inline std::string origin(const std::string& url) {
  if (!validUrl(url)) return {};
  std::string result = url.substr(0, url.find_first_of("/?", url.find("://") + 3));
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return std::tolower(c); });
  const auto sep = result.find("://");
  const size_t port = result.find(':', sep + 3);
  if (port != std::string::npos && ((result.substr(0, sep) == "https" && result.substr(port) == ":443") ||
                                    (result.substr(0, sep) == "http" && result.substr(port) == ":80")))
    result.erase(port);
  return result;
}
inline bool sameOrigin(const std::string& a, const std::string& b) {
  const auto o = origin(a);
  return !o.empty() && o == origin(b);
}
inline bool redirectAllowed(const std::string& from, const std::string& to) {
  return validUrl(to) && !(origin(from).find("https://") == 0 && origin(to).find("http://") == 0);
}
inline bool resolveRedirect(const std::string& base, const std::string& target, std::string& out) {
  if (!validUrl(base) || target.empty()) return false;
  // Callers may update the URL in place. Check against the original base
  // before assigning, otherwise HTTPS -> HTTP would compare HTTP to itself.
  std::string resolved;
  if (target.find("://") != std::string::npos)
    resolved = target;
  else if (target.compare(0, 2, "//") == 0)
    resolved = base.substr(0, base.find(':') + 1) + target;
  else if (target.front() == '/')
    resolved = origin(base) + target;
  else {
    std::string parent = base.substr(0, base.find('?'));
    const auto slash = parent.rfind('/');
    resolved = slash < base.find("://") + 3 ? origin(base) + "/" + target : parent.substr(0, slash + 1) + target;
  }
  if (!redirectAllowed(base, resolved)) return false;
  out = std::move(resolved);
  return true;
}
inline bool validHeader(const std::string& name, const std::string& value) {
  if (name.empty() || name.size() > 64 || value.size() > 1024) return false;
  for (unsigned char c : name)
    if (!std::isalnum(c) && c != '-') return false;
  for (unsigned char c : value)
    if (c < 32 || c == 127) return false;
  std::string lower = name;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
  return lower != "host" && lower != "connection" && lower != "content-length" && lower != "transfer-encoding" &&
         lower != "proxy-authorization";
}
}  // namespace pluginhttp
