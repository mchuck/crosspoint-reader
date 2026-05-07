#include "CascadeTheme.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/themes/lyra/LyraTheme.h"
#include "components/icons/book.h"
#include "components/icons/cover.h"
#include "components/icons/folder.h"
#include "components/icons/library.h"
#include "components/icons/recent.h"
#include "components/icons/settings2.h"
#include "components/icons/transfer.h"
#include "fontIds.h"

namespace {

constexpr int kIconSize = 32;
constexpr int kIconPaddingX = 20;
constexpr int kTextOffsetX = kIconPaddingX + kIconSize + 8;
constexpr int kSelectionBarWidth = 8;

struct SlotGeom {
  int x;
  int y;
  int w;
  int h;
};

// bmpW/bmpH: this slot's bitmap dimensions for aspect-correct sizing. Pass 0 for 14/10 fallback.
// actualCenterW/actualCenterH: the center cover's actual rendered pixel dimensions, used to size
// and position side slots. These come from the center bitmap headers (pre-scanned before the loop).
SlotGeom slotGeom(int offset, int centerX, int bmpW, int bmpH, int actualCenterW, int actualCenterH,
                  int rectY) {
  SlotGeom g;
  const int absOffset = offset < 0 ? -offset : offset;
  const int halfCenter = actualCenterW / 2;

  if (absOffset == 0) {
    g.w = actualCenterW;
    g.h = actualCenterH;
    g.x = centerX - g.w / 2;
    g.y = rectY;
  } else {
    // Side covers: height as % of center height; width from each slot's own bitmap AR.
    const int heightPct = absOffset == 1 ? 90 : 75;
    g.h = actualCenterH * heightPct / 100;
    g.w = (bmpW > 0 && bmpH > 0) ? g.h * bmpW / bmpH : g.h * 10 / 14;
    g.y = rectY + (actualCenterH - g.h) / 2 + (absOffset == 1 ? 14 : 30);

    // Pixels the side cover overlaps behind the cover in front of it.
    constexpr int kOverlap = 150;

    if (absOffset == 1) {
      g.x = centerX + (offset > 0 ? halfCenter - kOverlap : -(halfCenter - kOverlap + g.w));
    } else {
      // ±2: approximate ±1 width using the ±2 book's own AR at 90% height.
      const int pm1H = actualCenterH * 90 / 100;
      const int pm1W = (bmpW > 0 && bmpH > 0) ? pm1H * bmpW / bmpH : pm1H * 10 / 14;
      g.x = centerX + (offset > 0 ? halfCenter - kOverlap + pm1W - kOverlap
                                   : -(halfCenter - kOverlap + pm1W - kOverlap + g.w));
    }
  }
  return g;
}

// Draws a bitmap into a slot as a perspective quadrilateral.
//
// The quad has two vertical edges of different heights:
//   Inner edge (near center): full height g.h, at innerX.
//   Outer edge (far from center): height g.h - 2*verticalTaper, centred, at outerX.
// Top and bottom edges are diagonal, connecting inner corners to outer corners.
//
// anchorRight=true: innerX = g.x+g.w (left-side covers).
// anchorRight=false: innerX = g.x    (right-side covers).
//
// The full bitmap is loaded into RAM to allow random Y access during scanline rendering.
void drawSlotBitmap(GfxRenderer& renderer, Bitmap& bitmap, const SlotGeom& g, int verticalTaper,
                    bool anchorRight) {
  const int bitmapW = bitmap.getWidth();
  const int bitmapH = bitmap.getHeight();
  if (bitmapW <= 0 || bitmapH <= 0) return;

  const int outputRowBytes = (bitmapW + 3) / 4;
  const int rawRowBytes = bitmap.getRowBytes();

  auto* bitmapData = static_cast<uint8_t*>(malloc(bitmapH * outputRowBytes));
  auto* rowBuf = static_cast<uint8_t*>(malloc(rawRowBytes));
  if (!bitmapData || !rowBuf) {
    free(bitmapData);
    free(rowBuf);
    return;
  }

  for (int srcY = 0; srcY < bitmapH; srcY++) {
    if (bitmap.readNextRow(bitmapData + srcY * outputRowBytes, rowBuf) != BmpReaderError::Ok) break;
  }
  free(rowBuf);

  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int innerX = anchorRight ? (g.x + g.w) : g.x;
  const int outerX = anchorRight ? g.x : (g.x + g.w);
  const int outerTop = g.y + verticalTaper;
  const int outerBot = g.y + g.h - verticalTaper;
  const int outerH = outerBot - outerTop;

  for (int screenY = g.y; screenY < g.y + g.h; screenY++) {
    if (screenY < 0 || screenY >= screenH) continue;

    // Source V at the inner edge: linear over the full slot height. (Q16)
    const int v_inner_q16 = (screenY - g.y) * 65536 / g.h;

    // Outer edge X and source V at the outer edge. (Q16)
    int scanOuterX;
    int v_outer_q16;
    if (screenY < outerTop) {
      // Top diagonal: from inner top-corner to outer top-corner.
      scanOuterX = innerX + (outerX - innerX) * (screenY - g.y) / verticalTaper;
      v_outer_q16 = 0;
    } else if (screenY > outerBot) {
      // Bottom diagonal: from outer bottom-corner to inner bottom-corner.
      scanOuterX = outerX + (innerX - outerX) * (screenY - outerBot) / verticalTaper;
      v_outer_q16 = 65536;
    } else {
      // Middle: outer edge is vertical at outerX.
      scanOuterX = outerX;
      v_outer_q16 = outerH > 0 ? (screenY - outerTop) * 65536 / outerH : 32768;
    }

    const int scanLeft = anchorRight ? scanOuterX : innerX;
    const int scanRight = anchorRight ? innerX : scanOuterX;
    const int scanW = scanRight - scanLeft;
    if (scanW <= 0) continue;

    // V at the left and right boundaries of the scanline. (Q16)
    const int v_left_q16 = anchorRight ? v_outer_q16 : v_inner_q16;
    const int v_right_q16 = anchorRight ? v_inner_q16 : v_outer_q16;
    const int dv_q16 = (v_right_q16 - v_left_q16) / scanW;

    for (int screenX = scanLeft; screenX < scanRight; screenX++) {
      if (screenX < 0 || screenX >= screenW) continue;
      const int xi = screenX - scanLeft;
      const int bmpX = xi * bitmapW / scanW;
      const int bmpY = (v_left_q16 + dv_q16 * xi) * bitmapH / 65536;
      const int srcRow = bitmap.isTopDown() ? bmpY : (bitmapH - 1 - bmpY);
      if (srcRow < 0 || srcRow >= bitmapH || bmpX < 0 || bmpX >= bitmapW) continue;
      const uint8_t val = bitmapData[srcRow * outputRowBytes + bmpX / 4] >> (6 - (bmpX * 2) % 8) & 0x3;
      if (val < 3) renderer.drawPixel(screenX, screenY);
    }
  }

  free(bitmapData);
}

const uint8_t* iconForUIIcon(UIIcon icon) {
  switch (icon) {
    case UIIcon::Book:
      return BookIcon;
    case UIIcon::Folder:
      return FolderIcon;
    case UIIcon::Recent:
      return RecentIcon;
    case UIIcon::Library:
      return LibraryIcon;
    case UIIcon::Transfer:
      return TransferIcon;
    case UIIcon::Settings:
      return Settings2Icon;
    default:
      return nullptr;
  }
}

}  // namespace

void CascadeTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect,
                                       const std::vector<RecentBook>& recentBooks, const int selectorIndex,
                                       bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                                       std::function<bool()> storeCoverBuffer) const {
  const int screenW = renderer.getScreenWidth();
  const int centerW = std::min(screenW / 2, 230);
  const int centerX = rect.x + rect.width / 2;

  // Pre-scan center cover to determine actual rendered dimensions for side slot positioning.
  // Thumbnails are generated at homeCoverHeight; width varies with AR. drawBitmap only downscales,
  // so actual rendered size = min(bitmapDim, slotDim).
  const int coverHeight = CascadeMetrics::values.homeCoverHeight;
  int actualCenterW = coverHeight * 10 / 14;  // fallback ~200px for 10:14 AR at 280 height
  int actualCenterH = coverHeight;
  if (!recentBooks.empty()) {
    const RecentBook& centerBook = recentBooks[selectorIndex];
    if (!centerBook.coverBmpPath.empty()) {
      const std::string centerPath = UITheme::getCoverThumbPath(centerBook.coverBmpPath, coverHeight);
      FsFile cf;
      if (Storage.openFileForRead("HOME", centerPath, cf)) {
        Bitmap cb(cf);
        if (cb.parseHeaders() == BmpReaderError::Ok && cb.getWidth() > 0) {
          if (cb.getWidth() <= centerW) {
            actualCenterW = cb.getWidth();
            actualCenterH = cb.getHeight();
          } else {
            actualCenterW = centerW;
            actualCenterH = centerW * cb.getHeight() / cb.getWidth();
          }
        }
        cf.close();
      }
    }
  }

  if (recentBooks.empty()) {
    const SlotGeom g = slotGeom(0, centerX, 0, 0, actualCenterW, actualCenterH, rect.y);
    renderer.fillRect(g.x, g.y, g.w, g.h, false);
    return;
  }

  if (!coverRendered) {
    // Render order: back to front — offsets ±2, then ±1, then 0
    constexpr int renderOrder[] = {-2, 2, -1, 1, 0};
    const int bookCount = static_cast<int>(recentBooks.size());

    for (int offset : renderOrder) {
      const int bookIdx = ((selectorIndex + offset) % bookCount + bookCount) % bookCount;
      const RecentBook& book = recentBooks[bookIdx];

      if (book.coverBmpPath.empty()) {
        const SlotGeom g = slotGeom(offset, centerX, 0, 0, actualCenterW, actualCenterH, rect.y);
        renderer.fillRect(g.x, g.y, g.w, g.h, false);
        continue;
      }

      const std::string coverBmpPath =
          UITheme::getCoverThumbPath(book.coverBmpPath, CascadeMetrics::values.homeCoverHeight);
      FsFile file;
      if (!Storage.openFileForRead("HOME", coverBmpPath, file)) {
        const SlotGeom g = slotGeom(offset, centerX, 0, 0, actualCenterW, actualCenterH, rect.y);
        renderer.fillRect(g.x, g.y, g.w, g.h, false);
        continue;
      }

      Bitmap bitmap(file);
      if (bitmap.parseHeaders() != BmpReaderError::Ok) {
        file.close();
        const SlotGeom g = slotGeom(offset, centerX, 0, 0, actualCenterW, actualCenterH, rect.y);
        renderer.fillRect(g.x, g.y, g.w, g.h, false);
        continue;
      }

      // Slot geometry sized from actual bitmap aspect ratio, centred on center cover dimensions
      const SlotGeom g =
          slotGeom(offset, centerX, bitmap.getWidth(), bitmap.getHeight(), actualCenterW, actualCenterH, rect.y);
      // Clear slot to white first so adjacent cover pixels don't bleed through
      renderer.fillRect(g.x, g.y, g.w, g.h, false);

      if (offset == 0) {
        // Render center at native resolution — drawBitmap downscales to fit g.w×g.h when needed,
        // and renders at native size when bitmap is smaller (typical portrait cover).
        renderer.drawBitmap(bitmap, g.x, g.y, g.w, g.h);
      } else {
        const int absOffset = offset < 0 ? -offset : offset;
        const int verticalTaper = absOffset == 1 ? g.h / 5 : g.h / 3;
        const bool anchorRight = (offset > 0);
        drawSlotBitmap(renderer, bitmap, g, verticalTaper, anchorRight);
      }
      file.close();
    }

    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;
  }

  // Title below carousel (up to 2 lines, centred)
  const int titleTopY = rect.y + rect.height + 8;
  const int titleMaxWidth = rect.width - 2 * CascadeMetrics::values.contentSidePadding;
  const RecentBook& centered = recentBooks[selectorIndex];
  const auto titleLines = renderer.wrappedText(UI_12_FONT_ID, centered.title.c_str(), titleMaxWidth, 2,
                                               EpdFontFamily::BOLD);
  const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  int titleY = titleTopY;
  for (const auto& line : titleLines) {
    renderer.drawCenteredText(UI_12_FONT_ID, titleY, line.c_str(), true, EpdFontFamily::BOLD);
    titleY += titleLineHeight;
  }
}

void CascadeTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                                  const std::function<std::string(int index)>& buttonLabel,
                                  const std::function<UIIcon(int index)>& rowIcon) const {
  const int rowHeight = CascadeMetrics::values.listRowHeight;

  for (int i = 0; i < buttonCount; ++i) {
    const int rowY = rect.y + i * rowHeight;
    const bool selected = (selectedIndex == i);

    if (selected) {
      renderer.fillRect(rect.x, rowY, kSelectionBarWidth, rowHeight);
    }

    if (rowIcon != nullptr) {
      const UIIcon icon = rowIcon(i);
      const uint8_t* iconBitmap = iconForUIIcon(icon);
      if (iconBitmap != nullptr) {
        const int iconY = rowY + (rowHeight - kIconSize) / 2;
        renderer.drawIcon(iconBitmap, rect.x + kIconPaddingX, iconY, kIconSize, kIconSize);
      }
    }

    const std::string labelStr = buttonLabel(i);
    const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int textY = rowY + (rowHeight - lineHeight) / 2;
    renderer.drawText(UI_12_FONT_ID, rect.x + kTextOffsetX, textY, labelStr.c_str(), true);
  }
}

void CascadeTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                   const char* btn4) const {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int hintHeight = CascadeMetrics::values.buttonHintsHeight;
  const int hintY = pageHeight - hintHeight;

  renderer.drawLine(0, hintY, pageWidth, hintY, true);

  (void)btn1;
  (void)btn2;
  (void)btn3;
  (void)btn4;

  // Fixed glyphs per logical role: Back=«  Confirm=o  Left=<  Right=>
  // Reorder to physical screen positions based on frontButtonLayout.
  const char* back = "\xc2\xab";  // «
  const char* confirm = "o";
  const char* left = "<";
  const char* right = ">";
  const char* ordered[4];
  switch (static_cast<CrossPointSettings::FRONT_BUTTON_LAYOUT>(SETTINGS.frontButtonLayout)) {
    case CrossPointSettings::LEFT_RIGHT_BACK_CONFIRM:
      ordered[0] = left; ordered[1] = right; ordered[2] = back; ordered[3] = confirm;
      break;
    case CrossPointSettings::LEFT_BACK_CONFIRM_RIGHT:
      ordered[0] = left; ordered[1] = back; ordered[2] = confirm; ordered[3] = right;
      break;
    case CrossPointSettings::BACK_CONFIRM_RIGHT_LEFT:
      ordered[0] = back; ordered[1] = confirm; ordered[2] = right; ordered[3] = left;
      break;
    case CrossPointSettings::BACK_CONFIRM_LEFT_RIGHT:
    default:
      ordered[0] = back; ordered[1] = confirm; ordered[2] = left; ordered[3] = right;
      break;
  }

  const int slotWidth = pageWidth / 4;
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int textY = hintY + (hintHeight - lineHeight) / 2;

  for (int i = 0; i < 4; ++i) {
    if (ordered[i] == nullptr || ordered[i][0] == '\0') continue;
    const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, ordered[i]);
    const int textX = i * slotWidth + (slotWidth - textWidth) / 2;
    renderer.drawText(SMALL_FONT_ID, textX, textY, ordered[i], true);
  }
}
