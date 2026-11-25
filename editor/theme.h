// The editor's look ("pixel workshop"): a warm charcoal and parchment palette
// with one amber accent, Karla for text, Cousine for logs, the engine's own 5x7
// pixel font for headings, and 9x9 pixel icons drawn as rectangles on the
// display's pixel grid, so everything stays crisp at any display scale.
#pragma once
#include <imgui.h>

namespace ace::ui {

namespace color {
constexpr ImU32 kBackdrop = IM_COL32(20, 18, 16, 255);
constexpr ImU32 kCard = IM_COL32(33, 29, 25, 255); // opaque: blending is linear, so even 4% of a bright sky shows
constexpr ImU32 kBar = IM_COL32(26, 23, 20, 255); // top bar and status bar
constexpr ImU32 kRaised = IM_COL32(44, 39, 33, 255);
constexpr ImU32 kHover = IM_COL32(56, 49, 41, 255);
constexpr ImU32 kPressed = IM_COL32(66, 58, 48, 255);
constexpr ImU32 kEdge = IM_COL32(66, 58, 48, 255);
constexpr ImU32 kText = IM_COL32(236, 225, 204, 255);
constexpr ImU32 kDim = IM_COL32(160, 147, 126, 255);
constexpr ImU32 kFaint = IM_COL32(110, 99, 85, 255);
constexpr ImU32 kAccent = IM_COL32(242, 165, 65, 255);
constexpr ImU32 kAccentSoft = IM_COL32(242, 165, 65, 40);
constexpr ImU32 kAxis[3] = {IM_COL32(226, 88, 78, 255), IM_COL32(140, 194, 82, 255), IM_COL32(82, 146, 228, 255)};
constexpr ImU32 kPlay = IM_COL32(124, 198, 116, 255);
constexpr ImU32 kWarn = IM_COL32(242, 196, 82, 255);
constexpr ImU32 kError = IM_COL32(236, 104, 92, 255);
} // namespace color

struct Fonts {
  ImFont* text = nullptr;  // Karla (the default font)
  ImFont* mono = nullptr;  // Cousine: console
  ImFont* pixel = nullptr; // engine pixel font, 1 font pixel = 1 point: labels and headings
  ImFont* pixelLarge = nullptr; // 2 points per font pixel: the logo
};
extern Fonts fonts;
extern float displayScale; // framebuffer pixels per UI point

// Fonts and style. Karla covers ASCII only: keep UI strings ASCII.
// Call once, after ImGui::CreateContext and before the font
// texture is created; `scale` is the window's pixel density.
void setup(float scale);

enum class Icon {
  Play, Pause, Stop, Select, Move, Rotate, Scale, Build, Snap, Frame,
  Camera, Sun, Bulb, Mesh, Sprite, Empty, Script, Collider,
  Folder, File, Scene, Prefab, Image, Search, Lock, Plus, Reset,
  ChevronDown, ChevronRight, Grid, List, Console, Close, Count
};

// Draws a 9x9 pixel icon centered on `center`; `px` is the size of one icon
// pixel in points (snapped so icon pixels land on whole display pixels).
void icon(ImDrawList* dl, ImVec2 center, Icon which, ImU32 color, float px = 1.0f);
// A square button showing an icon; `active` draws it in the accent color.
bool iconButton(const char* id, Icon which, bool active, const char* tooltip, float size = 28.0f, float px = 2.0f);
// Text in the pixel font (upper case reads best).
void pixelText(const char* text, ImU32 color, bool large = false);
// Card background: a notched pixel-art rectangle with a hard drop shadow.
void card(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 fill = color::kCard);

} // namespace ace::ui
