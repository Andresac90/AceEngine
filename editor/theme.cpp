#include "theme.h"
#include "core/base.h"
#include "render/pixel_font.h"

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace ace::ui {

Fonts fonts;
float displayScale = 1.0f;

namespace {

struct PixelGlyph {
  int rect;              // custom rectangle in the font atlas
  const char* const* rows; // pixel_font rows
  int unit;              // atlas pixels per font pixel
};
std::vector<PixelGlyph> g_pixelGlyphs;

// The engine's 5x7 font as an ImGui font: each glyph is a custom atlas
// rectangle filled after the atlas is built. Lower case uses the upper-case shapes.
ImFont* addPixelFont(ImGuiIO& io, int unit) {
  static const ImWchar kSpaceOnly[] = {0x20, 0x20, 0};
  ImFontConfig cfg;
  cfg.GlyphRanges = kSpaceOnly; // the glyphs come from the custom rectangles below
  cfg.SizePixels = (float)(pixel_font::kLineHeight * unit);
  ImFont* font = io.Fonts->AddFontDefault(&cfg);
  for (const pixel_font::Glyph& g : pixel_font::kGlyphs) {
    auto add = [&](char c) {
      int rect = io.Fonts->AddCustomRectFontGlyph(font, (ImWchar)c, pixel_font::kGlyphW * unit, pixel_font::kGlyphH * unit,
                                                   (float)(pixel_font::kAdvance * unit), ImVec2(0.0f, (float)unit));
      g_pixelGlyphs.push_back({rect, g.rows, unit});
    };
    add(g.c);
    if (g.c >= 'A' && g.c <= 'Z') add((char)(g.c - 'A' + 'a'));
  }
  return font;
}

ImFont* addTtf(ImGuiIO& io, const char* file, float size) {
  std::string path = engineDataDir() + "fonts/" + file;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    ACE_WARN("editor font %s is missing; using ImGui's built-in font", path.c_str());
    ImFontConfig cfg;
    cfg.SizePixels = 13.0f * displayScale;
    return io.Fonts->AddFontDefault(&cfg);
  }
  ImFontConfig cfg;
  cfg.OversampleH = cfg.OversampleV = 1; // rasterized at the display's own resolution
  cfg.PixelSnapH = true;
  return io.Fonts->AddFontFromFileTTF(path.c_str(), size * displayScale, &cfg);
}

ImVec4 v4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

void applyStyle() {
  ImGuiStyle& s = ImGui::GetStyle();
  s = ImGuiStyle();
  s.WindowPadding = ImVec2(12, 10);
  s.FramePadding = ImVec2(7, 4);
  s.ItemSpacing = ImVec2(8, 6);
  s.ItemInnerSpacing = ImVec2(5, 4);
  s.IndentSpacing = 14;
  s.ScrollbarSize = 8;
  s.GrabMinSize = 8;
  s.WindowRounding = s.ChildRounding = s.FrameRounding = s.PopupRounding = 0;
  s.ScrollbarRounding = s.GrabRounding = s.TabRounding = 0;
  s.WindowBorderSize = s.ChildBorderSize = s.FrameBorderSize = 0;
  s.PopupBorderSize = 1;
  s.WindowMenuButtonPosition = ImGuiDir_None;
  s.SelectableTextAlign = ImVec2(0, 0.5f);

  ImVec4* c = s.Colors;
  c[ImGuiCol_Text] = v4(color::kText);
  c[ImGuiCol_TextDisabled] = v4(color::kFaint);
  c[ImGuiCol_WindowBg] = v4(color::kCard);
  c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_PopupBg] = v4(IM_COL32(38, 34, 29, 252));
  c[ImGuiCol_Border] = v4(color::kEdge);
  c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_FrameBg] = v4(color::kRaised);
  c[ImGuiCol_FrameBgHovered] = v4(color::kHover);
  c[ImGuiCol_FrameBgActive] = v4(color::kPressed);
  c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = v4(color::kBar);
  c[ImGuiCol_MenuBarBg] = v4(color::kBar);
  c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ScrollbarGrab] = v4(color::kEdge);
  c[ImGuiCol_ScrollbarGrabHovered] = v4(color::kFaint);
  c[ImGuiCol_ScrollbarGrabActive] = v4(color::kDim);
  c[ImGuiCol_CheckMark] = v4(color::kAccent);
  c[ImGuiCol_SliderGrab] = v4(color::kAccent);
  c[ImGuiCol_SliderGrabActive] = v4(IM_COL32(255, 190, 100, 255));
  c[ImGuiCol_Button] = v4(color::kRaised);
  c[ImGuiCol_ButtonHovered] = v4(color::kHover);
  c[ImGuiCol_ButtonActive] = v4(color::kPressed);
  c[ImGuiCol_Header] = v4(color::kAccentSoft); // selected rows
  c[ImGuiCol_HeaderHovered] = v4(IM_COL32(255, 240, 210, 18));
  c[ImGuiCol_HeaderActive] = v4(IM_COL32(242, 165, 65, 80));
  c[ImGuiCol_Separator] = c[ImGuiCol_SeparatorHovered] = v4(color::kEdge);
  c[ImGuiCol_SeparatorActive] = v4(color::kAccent);
  c[ImGuiCol_ResizeGrip] = c[ImGuiCol_ResizeGripHovered] = c[ImGuiCol_ResizeGripActive] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_TextSelectedBg] = v4(IM_COL32(242, 165, 65, 90));
  c[ImGuiCol_DragDropTarget] = v4(color::kAccent);
  c[ImGuiCol_NavHighlight] = v4(color::kAccent);
  c[ImGuiCol_ModalWindowDimBg] = v4(IM_COL32(10, 8, 6, 140));
  c[ImGuiCol_PlotHistogram] = v4(color::kAccent);
}

// 9x9 icons, '#' = ink. Drawn like the pixel font, so they share its look.
const char* const kIcons[(int)Icon::Count][9] = {
    {"..#......", "..##.....", "..###....", "..####...", "..#####..", "..####...", "..###....", "..##.....", "..#......"}, // Play
    {".........", ".##...##.", ".##...##.", ".##...##.", ".##...##.", ".##...##.", ".##...##.", ".##...##.", "........."}, // Pause
    {".........", ".#######.", ".#######.", ".#######.", ".#######.", ".#######.", ".#######.", ".#######.", "........."}, // Stop
    {".#.......", ".##......", ".###.....", ".####....", ".#####...", ".######..", ".###.....", ".#..#....", ".....#..."}, // Select
    {"....#....", "...###...", "....#....", ".#..#..#.", "#########", ".#..#..#.", "....#....", "...###...", "....#...."}, // Move
    {"..####...", ".#....#.#", "#......##", "#.....###", "#........", "#.......#", ".#.....#.", "..#####..", "........."}, // Rotate
    {"....#####", "......###", ".....#.##", "....#...#", "#####....", "#...#....", "#...#....", "#...#....", "#####...."}, // Scale
    {".........", "#########", "#...#...#", "#########", "#.#...#.#", "#########", "#...#...#", "#########", "........."}, // Build
    {"###...###", "###...###", "#.#...#.#", "#.#...#.#", "#.#...#.#", "#..###..#", "#.......#", ".#.....#.", "..#####.."}, // Snap
    {"###...###", "#.......#", "#.......#", "....#....", "...###...", "....#....", "#.......#", "#.......#", "###...###"}, // Frame
    {".........", ".........", "#####...#", "#...#..##", "#...#.###", "#...#..##", "#####...#", ".........", "........."}, // Camera
    {"....#....", ".#.....#.", "...###...", "..#####..", "#.#####.#", "..#####..", "...###...", ".#.....#.", "....#...."}, // Sun
    {"...###...", "..#####..", ".#######.", ".#######.", ".#######.", "..#####..", "...###...", "...###...", "....#...."}, // Bulb
    {"..######.", ".#.....##", "#######.#", "#.....#.#", "#.....#.#", "#.....#.#", "#.....##.", "#######..", "........."}, // Mesh
    {"#########", "#.......#", "#.##....#", "#.##....#", "#....#..#", "#...###.#", "#..######", "#.#######", "#########"}, // Sprite
    {".........", "...###...", "..#...#..", ".#.....#.", ".#.....#.", ".#.....#.", "..#...#..", "...###...", "........."}, // Empty
    {".........", "..#...#..", ".#.....#.", ".#.....#.", "#.......#", ".#.....#.", ".#.....#.", "..#...#..", "........."}, // Script
    {"#.#.#.#.#", ".........", "#.......#", ".........", "#.......#", ".........", "#.......#", ".........", "#.#.#.#.#"}, // Collider
    {".........", "####.....", "#..######", "#.......#", "#########", "#.......#", "#.......#", "#########", "........."}, // Folder
    {"#####....", "#...##...", "#...#.#..", "#...####.", "#......#.", "#......#.", "#......#.", "########.", "........."}, // File
    {".........", "#########", "#...#...#", "#...#...#", "#########", "#...#...#", "#...#...#", "#########", "........."}, // Scene
    {".........", "..#####..", ".#..#..#.", "#########", "#...#...#", "#...#...#", "#...#...#", "#########", "........."}, // Prefab
    {"#########", "#.......#", "#.##....#", "#.##..#.#", "#....###.", "#...#####", "#..######", "#########", "........."}, // Image
    {"..###....", ".#...#...", "#.....#..", "#.....#..", "#.....#..", ".#...#...", "..####...", "......##.", ".......##"}, // Search
    {"...###...", "..#...#..", "..#...#..", ".#######.", ".#######.", ".###.###.", ".###.###.", ".#######.", "........."}, // Lock
    {".........", "....#....", "....#....", "....#....", ".#######.", "....#....", "....#....", "....#....", "........."}, // Plus
    {"....###..", "...#...#.", "#.#.....#", "##......#", "###.....#", "........#", "..#....#.", "...####..", "........."}, // Reset
    {".........", ".........", "#.......#", ".#.....#.", "..#...#..", "...#.#...", "....#....", ".........", "........."}, // ChevronDown
    {"..#......", "...#.....", "....#....", ".....#...", "......#..", ".....#...", "....#....", "...#.....", "..#......"}, // ChevronRight
    {"####.####", "####.####", "####.####", ".........", "####.####", "####.####", "####.####", ".........", "........."}, // Grid
    {"##.######", ".........", "##.######", ".........", "##.######", ".........", "##.######", ".........", "........."}, // List
    {"#########", "#.......#", "#.#.....#", "#..#....#", "#.#..##.#", "#.......#", "#########", ".........", "........."}, // Console
    {"#.......#", ".#.....#.", "..#...#..", "...#.#...", "....#....", "...#.#...", "..#...#..", ".#.....#.", "#.......#"}, // Close
};

} // namespace

void setup(float scale) {
  displayScale = std::max(1.0f, scale);
  ImGuiIO& io = ImGui::GetIO();
  io.Fonts->Clear();
  g_pixelGlyphs.clear();
  // Fonts are rasterized at the display's resolution and drawn at 1/scale, so
  // text is crisp on HiDPI screens instead of an upscaled 1x atlas.
  fonts.text = addTtf(io, "Karla-Regular.ttf", 15.0f);
  fonts.mono = addTtf(io, "Cousine-Regular.ttf", 13.0f);
  int unit = std::max(1, (int)lroundf(displayScale));
  fonts.pixel = addPixelFont(io, unit);
  fonts.pixelLarge = addPixelFont(io, unit * 2);
  io.FontDefault = fonts.text;
  io.FontGlobalScale = 1.0f / displayScale;

  unsigned char* pixels = nullptr;
  int width = 0, height = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height); // builds the atlas
  for (const PixelGlyph& g : g_pixelGlyphs) {
    const ImFontAtlasCustomRect* r = io.Fonts->GetCustomRectByIndex(g.rect);
    for (int y = 0; y < r->Height; y++)
      for (int x = 0; x < r->Width; x++) {
        bool ink = g.rows[y / g.unit][x / g.unit] == '#';
        pixels[((size_t)(r->Y + y) * width + r->X + x) * 4 + 3] = ink ? 255 : 0;
      }
  }
  applyStyle();
}

void icon(ImDrawList* dl, ImVec2 center, Icon which, ImU32 col, float px) {
  // Snap the icon pixel and the origin to whole display pixels.
  float s = std::max(1.0f, roundf(px * displayScale)) / displayScale;
  float x0 = roundf((center.x - 4.5f * s) * displayScale) / displayScale;
  float y0 = roundf((center.y - 4.5f * s) * displayScale) / displayScale;
  const char* const* rows = kIcons[(int)which];
  for (int y = 0; y < 9; y++)
    for (int x = 0; x < 9;) {
      if (rows[y][x] != '#') {
        x++;
        continue;
      }
      int run = x;
      while (run < 9 && rows[y][run] == '#') run++;
      dl->AddRectFilled(ImVec2(x0 + x * s, y0 + y * s), ImVec2(x0 + run * s, y0 + (y + 1) * s), col);
      x = run;
    }
}

bool iconButton(const char* id, Icon which, bool active, const char* tooltip, float size, float px) {
  ImVec2 p = ImGui::GetCursorScreenPos();
  bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
  bool hovered = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (active) dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), color::kAccentSoft);
  else if (ImGui::IsItemActive()) dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), color::kPressed);
  else if (hovered) dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), color::kHover);
  icon(dl, ImVec2(p.x + size * 0.5f, p.y + size * 0.5f), which, active ? color::kAccent : hovered ? color::kText : color::kDim, px);
  if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tooltip);
  return pressed;
}

void pixelText(const char* text, ImU32 col, bool large) {
  ImGui::PushFont(large ? fonts.pixelLarge : fonts.pixel);
  ImGui::PushStyleColor(ImGuiCol_Text, col);
  ImGui::TextUnformatted(text);
  ImGui::PopStyleColor();
  ImGui::PopFont();
}

void card(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 fill) {
  const float n = 3.0f; // the notch: a pixel-art "rounded" corner
  auto outline = [&](float dx, float dy, ImVec2* pts) {
    ImVec2 p[8] = {{a.x + n, a.y}, {b.x - n, a.y}, {b.x, a.y + n}, {b.x, b.y - n},
                   {b.x - n, b.y}, {a.x + n, b.y}, {a.x, b.y - n}, {a.x, a.y + n}};
    for (int i = 0; i < 8; i++) pts[i] = ImVec2(p[i].x + dx, p[i].y + dy);
  };
  ImVec2 pts[8];
  dl->PushClipRectFullScreen();
  outline(3.0f, 3.0f, pts);
  dl->AddConvexPolyFilled(pts, 8, IM_COL32(0, 0, 0, 90)); // hard shadow, down and right
  outline(0.0f, 0.0f, pts);
  dl->AddConvexPolyFilled(pts, 8, fill);
  dl->AddPolyline(pts, 8, color::kEdge, ImDrawFlags_Closed, 1.0f);
  dl->PopClipRect();
}

} // namespace ace::ui
