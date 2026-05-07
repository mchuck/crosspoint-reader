#pragma once

#include "components/themes/lyra/LyraTheme.h"

namespace CascadeMetrics {
constexpr ThemeMetrics values = [] {
  ThemeMetrics v = LyraMetrics::values;
  v.topPadding = 0;
  v.homeTopPadding = 70;
  v.homeCoverHeight = 340;
  v.homeCoverTileHeight = 400;
  v.homeRecentBooksCount = 5;
  v.homeContinueReadingInMenu = false;
  v.homeCarouselMode = true;
  v.homeMenuTopOffset = 40;
  return v;
}();
}  // namespace CascadeMetrics

class CascadeTheme : public LyraTheme {
 public:
  const ThemeMetrics& getMetrics() const { return CascadeMetrics::values; }

  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer) const override;

  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;

  void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                       const char* btn4) const override;
};
