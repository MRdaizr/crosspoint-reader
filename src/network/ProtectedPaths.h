#pragma once

#include <string.h>

#include <string>

// Credential stores no web request (file manager, plugin endpoints) or plugin
// manifest may read or write: their passwords are obfuscated with a device
// key, so the files must stay on the device. Mirrors the getFilePath() of
// WifiCredentialStore, OpdsServerStore, KOReaderCredentialStore, Nutstore and
// WeRead session storage. Prefix
// match, so the stores' temp/backup siblings are covered too.
namespace protectedpaths {

inline bool prefixEqualsIgnoreCase(const char* text, const char* prefix) {
  for (; *prefix; ++text, ++prefix) {
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; };
    if (!*text || lower(*text) != lower(*prefix)) return false;
  }
  return true;
}

inline constexpr const char* SENSITIVE_FILES[] = {"/.crosspoint/wifi.json",     "/.crosspoint/opds.json",
                                                  "/.crosspoint/koreader.json", "/.crosspoint/nutstore.json",
                                                  "/.crosspoint/weread",        "/.crosspoint/plugin-permissions"};

// SdFat also opens a name by its generated 8.3 alias (".crosspoint" is
// CROSSP~1), which no spelling check can map back to the long name.
inline bool isShortAlias(const char* first, const char* last) {
  const char* tilde = static_cast<const char*>(memchr(first, '~', static_cast<size_t>(last - first)));
  if (!tilde || tilde + 1 >= last || tilde[1] < '0' || tilde[1] > '9') return false;
  const char* dot = static_cast<const char*>(memchr(first, '.', static_cast<size_t>(last - first)));
  const size_t base = static_cast<size_t>((dot ? dot : last) - first);
  const size_t ext = dot ? static_cast<size_t>(last - dot - 1) : 0;
  return base <= 8 && ext <= 3;
}

inline bool isSensitivePath(const char* path, bool includePluginRoots = true) {
  if (!path || strlen(path) > 240) return true;
  // Canonicalise first ("//", "/./" and ".." would otherwise dodge the prefix
  // match while still opening the same file).
  std::string canon;
  canon.reserve(strlen(path) + 1);
  int depth = 0;
  for (const char* seg = path; *seg;) {
    while (*seg == '/' || *seg == '\\') seg++;
    const char* end = strpbrk(seg, "/\\");
    const size_t len = end ? static_cast<size_t>(end - seg) : strlen(seg);
    if (len == 2 && seg[0] == '.' && seg[1] == '.') {
      const size_t slash = canon.rfind('/');
      canon.erase(slash == std::string::npos ? 0 : slash);
      if (depth > 0) depth--;
    } else {
      // SdFat skips leading spaces and drops trailing dots and spaces when it
      // opens a name, so compare the name it would actually open.
      const char* first = seg;
      const char* last = seg + len;
      while (first < last && *first == ' ') first++;
      const char* spaceEnd = last;
      while (spaceEnd > first && spaceEnd[-1] == ' ') --spaceEnd;
      // Ambiguous FAT dot-directory spellings must not bypass the native
      // route guard (e.g. /books/.. /.crosspoint/plugin-permissions).
      if (spaceEnd - first == 2 && first[0] == '.' && first[1] == '.') return true;
      while (last > first && (last[-1] == '.' || last[-1] == ' ')) last--;
      // Reject encoded/control/alternate-stream spellings before FAT sees them.
      for (const char* c = first; c < last; ++c) {
        if (static_cast<unsigned char>(*c) < 32 || *c == 127 || *c == '%' || *c == ':') return true;
      }
      // Root stores sit two levels deep; WeRead also has a nested session file.
      const bool inWeRead =
          canon.size() == strlen("/.crosspoint/weread") && prefixEqualsIgnoreCase(canon.c_str(), "/.crosspoint/weread");
      if ((depth < 2 || inWeRead) && isShortAlias(first, last)) return true;
      if (first < last) {
        canon += '/';
        canon.append(first, static_cast<size_t>(last - first));
        depth++;
      }
    }
    seg += len;
  }
  for (const char* file : SENSITIVE_FILES) {
    // FAT names are case-insensitive.
    if (prefixEqualsIgnoreCase(canon.c_str(), file)) return true;
  }
  if (includePluginRoots) {
    for (const char* root : {"/.crosspoint/plugins", "/plugins", "/.plugins"}) {
      const size_t n = strlen(root);
      if (prefixEqualsIgnoreCase(canon.c_str(), root) && (canon[n] == '/' || canon.size() == n)) return true;
    }
  }
  return false;
}

// Ordinary HTTP/WebDAV file routes must never serve executable plugin files,
// configs, or outboxes. The dedicated /plugin route applies approval first.
inline bool isPluginRootPath(const char* path) {
  if (!path) return false;
  for (const char* root : {"/.crosspoint/plugins", "/plugins", "/.plugins"}) {
    const size_t n = strlen(root);
    if (prefixEqualsIgnoreCase(path, root) && (path[n] == '/' || path[n] == '\0')) return true;
  }
  return false;
}

// A plugin-supplied path (web request or device.json): absolute, no parent
// refs, not a credential store.
inline bool isPluginPath(const std::string& p) {
  for (unsigned char c : p)
    if (c < 32 || c == 127) return false;
  if (p.size() <= 1 || p.size() > 220 || p[0] != '/' || p.find("..") != std::string::npos ||
      p.find('\\') != std::string::npos || isSensitivePath(p.c_str(), false))
    return false;
  // Disallow every FAT alias and trimmed segment in plugin-controlled paths:
  // aliases at arbitrary depth could reach another plugin's approval store.
  for (size_t start = 1; start < p.size();) {
    size_t end = p.find('/', start);
    if (end == std::string::npos) end = p.size();
    if (end == start || p[start] == ' ' || p[end - 1] == ' ' || p[end - 1] == '.' ||
        isShortAlias(p.data() + start, p.data() + end))
      return false;
    start = end + 1;
  }
  return true;
}

}  // namespace protectedpaths
