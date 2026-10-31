#pragma once
// ================================================================
//  Shared SD path / name helpers.
//  These were byte-identical copies in warhog_fork/SDUtils.cpp and
//  WarHog_Freenove_S3_CYD_HAM_UI4/src/SDUtils.cpp. Extracted here so a fix
//  lands once instead of drifting between the two lineages. Pure Arduino
//  `String` logic — no board/peripheral dependencies — so every firmware
//  shares one definition. Consumed via `-I../common` (added to each
//  platformio.ini, alongside `-I../scheduler`); SDUtils.h includes this, so
//  every caller of these helpers gets the definitions.
// ================================================================
#include <Arduino.h>

inline String pathBasename(const String& p) {
  int slash = p.lastIndexOf('/');
  if (slash < 0) return p;
  return p.substring(slash + 1);
}

inline String normalizeSdPath(const char* dir, const char* nameIn) {
  if (!dir || !nameIn) return "";

  String d(dir);
  String n(nameIn);

  d.trim();
  n.trim();

  if (d.length() == 0 || n.length() == 0) return "";

  // Ensure dir starts with "/"
  if (d[0] != '/') d = "/" + d;

  // Strip trailing "/" from dir
  while (d.endsWith("/")) d.remove(d.length() - 1);

  // Case A: name is already absolute: "/logs/foo.csv" or "/uploaded/foo.csv"
  if (n[0] == '/') {
    // If SD lib already gives full path, just return it
    return n;
  }

  // Case B: name is "logs/foo.csv" (no leading slash)
  // If it starts with the same directory name, convert to absolute.
  // Example: dir="/logs", name="logs/foo.csv" => "/logs/foo.csv"
  String dNoSlash = d;
  if (dNoSlash.startsWith("/")) dNoSlash = dNoSlash.substring(1); // "logs"

  if (n.startsWith(dNoSlash + "/")) {
    return "/" + n;  // make it absolute
  }

  // Case C: name is just "foo.csv"
  // Join dir + "/" + name
  return d + "/" + n;
}

inline bool isAllowedDataPath(const String& p) {
  return p.startsWith("/logs/") || p.startsWith("/uploaded/");
}

// Device-name field for the WiGLE header: spaces -> '_', strip anything that
// isn't alnum/_/-, cap at 20 chars.
inline String sanitiseDeviceName(const String& raw) {
  String s = raw;
  s.replace(" ", "_");
  for (int i = (int)s.length() - 1; i >= 0; i--) {
    char c = s[i];
    if (!isAlphaNumeric(c) && c != '_' && c != '-') s.remove(i, 1);
  }
  if (s.length() > 20) s = s.substring(0, 20);
  return s;
}
