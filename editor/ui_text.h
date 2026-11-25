// Small text helpers for the editor UI: readable labels from field names, and
// the fuzzy match behind the command palette and the search boxes.
#pragma once
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>

namespace ace::ui {

// "probeResolution" -> "Probe resolution", "MeshRenderer" -> "Mesh renderer",
// "ptSamples" -> "PT samples".
inline std::string humanize(std::string_view name) {
  std::string out;
  for (size_t i = 0; i < name.size(); i++) {
    char c = name[i];
    bool wordStart = i > 0 && isupper((unsigned char)c) && islower((unsigned char)name[i - 1]);
    if (wordStart) out += ' ';
    out += out.empty() ? (char)toupper((unsigned char)c) : (char)tolower((unsigned char)c);
  }
  if (out.rfind("Pt ", 0) == 0) out.replace(0, 2, "PT"); // path-tracing settings
  return out;
}

// -1 when the letters of `pattern` (spaces ignored, any case) do not all appear
// in order in `text`; otherwise a score where higher is better: letters at
// word starts and runs of consecutive letters count most, shorter texts win ties.
inline int fuzzyScore(std::string_view pattern, std::string_view text) {
  int score = 0;
  size_t t = 0, last = std::string_view::npos;
  for (char pc : pattern) {
    if (pc == ' ') continue;
    char p = (char)tolower((unsigned char)pc);
    while (t < text.size() && tolower((unsigned char)text[t]) != p) t++;
    if (t == text.size()) return -1;
    bool wordStart = t == 0 || strchr(" _-./", text[t - 1]) ||
                     (isupper((unsigned char)text[t]) && islower((unsigned char)text[t - 1]));
    score += 1 + (wordStart ? 8 : 0) + (last != std::string_view::npos && t == last + 1 ? 5 : 0);
    last = t++;
  }
  return std::max(0, score * 4 - (int)text.size() / 4);
}

} // namespace ace::ui
