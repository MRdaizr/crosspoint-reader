#pragma once
#include <cstddef>
#include <cstring>
#include <string>

namespace PluginPermissions {
inline constexpr const char* STORE_DIR = "/.crosspoint/plugin-permissions";
struct Status {
  bool enabled = false;
  bool approved = false;
  bool available = false;
  char digest[65] = {};
};
inline bool approvalMatches(bool enabled, const char* saved, const char* current) {
  return enabled && saved && current && strlen(saved) == 64 && strlen(current) == 64 && strcmp(saved, current) == 0;
}
// Gate all device HTTP, events, raw JS, and job APIs. Installs default disabled.
void setSystemEnabled(bool enabled);
bool systemEnabled();
Status inspect(const char* plugin);
bool allowed(const char* plugin);
// Enable requires the digest shown to the user. Changed code cannot be approved
// by a stale confirmation. Config/token files are deliberately outside the hash.
bool setEnabled(const char* plugin, bool enabled, const char* expectedDigest = nullptr);
}  // namespace PluginPermissions
