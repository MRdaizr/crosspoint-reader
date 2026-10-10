#include "PluginLocations.h"

#include <HalStorage.h>

#include <algorithm>
#include <cctype>

namespace PluginLocations {

std::vector<Entry> scanPlugins() {
  std::vector<Entry> plugins;
  plugins.reserve(MAX_PLUGINS);
  std::vector<std::string> seen;
  seen.reserve(128);
  for (size_t r = 0; r < ROOT_COUNT; r++) {
    HalFile root = Storage.open(ROOTS[r]);
    if (!root || !root.isDirectory()) continue;
    for (HalFile entry = root.openNextFile(); entry; entry = root.openNextFile()) {
      if (!entry.isDirectory()) continue;
      char name[128];
      if (entry.getName(name, sizeof(name)) == 0 || !validName(name)) continue;
      if (seen.size() >= 128) return plugins;
      std::string folded = name;
      std::transform(folded.begin(), folded.end(), folded.begin(), [](unsigned char c) { return std::tolower(c); });
      if (std::find(seen.begin(), seen.end(), folded) != seen.end()) continue;
      // Claimed even without markers: findPluginDir resolves this name here,
      // so a same-named folder in a later root must not be reported instead.
      seen.emplace_back(std::move(folded));

      Entry e;
      e.name = name;
      e.dir = std::string(ROOTS[r]) + "/" + name;
      e.hasPluginJs = Storage.exists((e.dir + "/plugin.js").c_str());
      e.hasMainJs = Storage.exists((e.dir + "/main.js").c_str());
      e.hasDevice = Storage.exists((e.dir + "/device.json").c_str());
      e.hasManifest = Storage.exists((e.dir + "/manifest.json").c_str());
      e.hasReadme = Storage.exists((e.dir + "/README.md").c_str());
      if (e.hasPluginJs || e.hasMainJs || e.hasDevice || e.hasManifest || e.hasReadme) plugins.push_back(std::move(e));
      if (plugins.size() >= MAX_PLUGINS) return plugins;
    }
  }
  return plugins;
}

std::string findPluginDir(const char* name) {
  if (!validName(name)) return {};
  for (size_t i = 0; i < ROOT_COUNT; i++) {
    std::string dir = std::string(ROOTS[i]) + "/" + name;
    HalFile folder = Storage.open(dir.c_str());
    if (folder && folder.isDirectory()) return dir;
  }
  return {};
}

}  // namespace PluginLocations
