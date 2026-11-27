#pragma once
// Shared drawing for the recovery tool and the recovery review: a banner that
// makes the recovery mode unmistakable, a faint repeated warning behind the
// display areas, and a small flow diagram of what the work needs. GDI only;
// callers own every HDC and font they pass in.
#include <windows.h>
#include <algorithm>
#include <string>
#include <vector>

namespace recovery_visual {

enum class Mark { Ok, Pending, Warn, Bad };

struct Node {
  std::wstring title;  // what this step is
  std::wstring detail; // its current state, in words
  Mark mark = Mark::Pending;
};

// Line design: areas keep the caller's plain background; rules, outlines and
// colored text carry the recovery state instead of filled panels.
inline COLORREF Accent() { return RGB(140, 72, 0); }
inline COLORREF TextStrong() { return RGB(33, 33, 33); }
inline COLORREF TextBody() { return RGB(66, 66, 66); }
inline COLORREF WatermarkText(COLORREF background) {
  // A few steps darker than the background: readable, never competing.
  const auto shade = [](int v) { return std::max(0, v - 34); };
  return RGB(shade(GetRValue(background)), shade(GetGValue(background)),
             shade(GetBValue(background)));
}

inline COLORREF MarkEdge(Mark mark) {
  switch (mark) {
  case Mark::Ok: return RGB(46, 125, 50);
  case Mark::Warn: return RGB(176, 110, 0);
  case Mark::Bad: return RGB(183, 28, 28);
  default: return RGB(96, 96, 96);
  }
}
// Shape and color both carry the state, so color is never the only cue.
inline const wchar_t *MarkGlyph(Mark mark) {
  switch (mark) {
  case Mark::Ok: return L"✓";   // check
  case Mark::Warn: return L"!";
  case Mark::Bad: return L"✕";  // cross
  default: return L"…";         // ellipsis: not decided yet
  }
}

inline HFONT DeriveFont(HFONT base, int heightDelta, int weight, int escapement = 0) {
  LOGFONTW font{};
  if (!base || !GetObjectW(base, sizeof(font), &font))
    GetObjectW(GetStockObject(DEFAULT_GUI_FONT), sizeof(font), &font);
  if (font.lfHeight < 0)
    font.lfHeight -= heightDelta;
  else
    font.lfHeight += heightDelta;
  font.lfWeight = weight;
  font.lfEscapement = escapement;
  font.lfOrientation = escapement;
  font.lfQuality = CLEARTYPE_QUALITY;
  return CreateFontIndirectW(&font);
}

// Repeats the warning diagonally across `area`. Text only; nothing is filled.
inline void PaintWatermark(HDC dc, const RECT &area, HFONT base,
                           COLORREF background, const std::wstring &text) {
  if (text.empty() || area.right <= area.left || area.bottom <= area.top)
    return;
  HFONT font = DeriveFont(base, 2, FW_SEMIBOLD, 200); // 20 degrees
  if (!font)
    return;
  const int saved = SaveDC(dc);
  IntersectClipRect(dc, area.left, area.top, area.right, area.bottom);
  SelectObject(dc, font);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, WatermarkText(background));
  SIZE size{};
  GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
  const int stepX = std::max(200, static_cast<int>(size.cx) + 80);
  const int stepY = std::max(90, static_cast<int>(size.cy) * 5);
  int row = 0;
  for (int y = area.top - stepY; y < area.bottom + size.cx; y += stepY, ++row) {
    const int offset = (row % 2) * (stepX / 2);
    for (int x = area.left - stepX + offset; x < area.right + stepX; x += stepX)
      TextOutW(dc, x, y, text.c_str(), static_cast<int>(text.size()));
  }
  RestoreDC(dc, saved);
  DeleteObject(font);
}

inline void Rule(HDC dc, int x0, int y, int x1, int width, COLORREF color) {
  HPEN pen = CreatePen(PS_SOLID, width, color);
  HGDIOBJ old = SelectObject(dc, pen);
  MoveToEx(dc, x0, y, nullptr);
  LineTo(dc, x1, y);
  SelectObject(dc, old);
  DeleteObject(pen);
}

// The band at the top: mode title, the standing warning, and a summary line,
// framed by an accent rule above and a thin rule below. Nothing is filled
// except the caller's own background.
inline void PaintBanner(HDC dc, const RECT &band, HFONT base, COLORREF background,
                        const std::wstring &title, const std::wstring &warning,
                        const std::wstring &summary) {
  HBRUSH brush = CreateSolidBrush(background);
  FillRect(dc, &band, brush);
  DeleteObject(brush);
  Rule(dc, band.left, band.top + 2, band.right, 3, Accent());
  Rule(dc, band.left + 12, band.bottom - 2, band.right - 12, 1, Accent());
  HFONT titleFont = DeriveFont(base, 4, FW_BOLD);
  HFONT bodyFont = DeriveFont(base, 0, FW_NORMAL);
  const int saved = SaveDC(dc);
  SetBkMode(dc, TRANSPARENT);
  RECT line = band;
  InflateRect(&line, -12, -8);
  SelectObject(dc, titleFont);
  SetTextColor(dc, Accent());
  RECT measured = line;
  DrawTextW(dc, title.c_str(), -1, &measured, DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
  DrawTextW(dc, title.c_str(), -1, &line, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
  line.top = measured.bottom + 2;
  SelectObject(dc, bodyFont);
  SetTextColor(dc, TextBody());
  measured = line;
  DrawTextW(dc, warning.c_str(), -1, &measured, DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
  DrawTextW(dc, warning.c_str(), -1, &line, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
  if (!summary.empty()) {
    line.top = measured.bottom + 2;
    SetTextColor(dc, TextStrong());
    DrawTextW(dc, summary.c_str(), -1, &line, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
  }
  RestoreDC(dc, saved);
  DeleteObject(titleFont);
  DeleteObject(bodyFont);
}

// Outlined boxes left to right with arrows. Each box carries a circled mark,
// its title and state; problems get a heavier outline than finished steps.
inline void PaintFlow(HDC dc, const RECT &area, HFONT base, COLORREF background,
                      const std::vector<Node> &nodes) {
  HBRUSH back = CreateSolidBrush(background);
  FillRect(dc, &area, back);
  DeleteObject(back);
  if (nodes.empty())
    return;
  const int count = static_cast<int>(nodes.size());
  const int arrow = 36, pad = 8;
  const int width = area.right - area.left - pad * 2;
  const int boxW = std::max(80, (width - arrow * (count - 1)) / count);
  const int top = area.top + pad, bottom = area.bottom - pad;
  HFONT titleFont = DeriveFont(base, 0, FW_BOLD);
  HFONT bodyFont = DeriveFont(base, 0, FW_NORMAL);
  HFONT markFont = DeriveFont(base, 2, FW_BOLD);
  const int saved = SaveDC(dc);
  SetBkMode(dc, TRANSPARENT);
  SelectObject(dc, GetStockObject(NULL_BRUSH));
  for (int i = 0; i < count; ++i) {
    const auto &node = nodes[static_cast<size_t>(i)];
    const int left = area.left + pad + i * (boxW + arrow);
    RECT box{left, top, left + boxW, bottom};
    const int weight = node.mark == Mark::Ok ? 1 : 2;
    HPEN pen = CreatePen(PS_SOLID, weight, MarkEdge(node.mark));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, box.left, box.top, box.right, box.bottom, 10, 10);
    const int cy = (box.top + box.bottom) / 2, r = 11, cx = box.left + 8 + r;
    Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    RECT markRect{cx - r, cy - r, cx + r + 1, cy + r + 1};
    SelectObject(dc, markFont);
    SetTextColor(dc, MarkEdge(node.mark));
    DrawTextW(dc, MarkGlyph(node.mark), -1, &markRect,
              DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
    RECT text{cx + r + 10, box.top + 6, box.right - 8, box.bottom - 6};
    SelectObject(dc, titleFont);
    SetTextColor(dc, node.mark == Mark::Ok ? TextStrong() : MarkEdge(node.mark));
    RECT titleRect = text;
    DrawTextW(dc, node.title.c_str(), -1, &titleRect,
              DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    RECT measured = text;
    DrawTextW(dc, node.title.c_str(), -1, &measured,
              DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
    text.top = measured.bottom + 4;
    SelectObject(dc, bodyFont);
    SetTextColor(dc, TextBody());
    DrawTextW(dc, node.detail.c_str(), -1, &text,
              DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS | DT_EDITCONTROL);
    if (i + 1 < count) {
      const int y = (top + bottom) / 2, x0 = box.right + 6, x1 = box.right + arrow - 6;
      HPEN line = CreatePen(PS_SOLID, 1, RGB(117, 117, 117));
      HGDIOBJ old = SelectObject(dc, line);
      MoveToEx(dc, x0, y, nullptr);
      LineTo(dc, x1, y);
      MoveToEx(dc, x1, y, nullptr);
      LineTo(dc, x1 - 7, y - 6);
      MoveToEx(dc, x1, y, nullptr);
      LineTo(dc, x1 - 7, y + 6);
      SelectObject(dc, old);
      DeleteObject(line);
    }
  }
  RestoreDC(dc, saved);
  DeleteObject(titleFont);
  DeleteObject(bodyFont);
  DeleteObject(markFont);
}

} // namespace recovery_visual
