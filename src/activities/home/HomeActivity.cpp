#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Utf8.h>
#include <Xtc.h>

#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

int HomeActivity::getMenuItemCount() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  if (metrics.homeCarouselMode) {
    int count = 5;  // Continue Reading + File Browser + Recents + File Transfer + Settings
    if (hasOpdsServers) count++;
    return count;
  }
  int count = 4;  // File Browser, Recents, File transfer, Settings
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
  if (hasOpdsServers) {
    count++;
  }
  return count;
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    // Skip if file no longer exists
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }

    recentBooks.push_back(book);
  }
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;

  int progress = 0;
  for (RecentBook& book : recentBooks) {
    if (!book.coverBmpPath.empty()) {
      std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
      if (!Storage.exists(coverPath.c_str())) {
        // If epub, try to load the metadata for title/author and cover
        if (FsHelpers::hasEpubExtension(book.path)) {
          Epub epub(book.path, "/.crosspoint");
          // Skip loading css since we only need metadata here
          epub.load(false, true);

          // Try to generate thumbnail image for Continue Reading card
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
          bool success = epub.generateThumbBmp(coverHeight);
          if (!success) {
            RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
            book.coverBmpPath = "";
          }
          coverRendered = false;
          requestUpdate();
        } else if (FsHelpers::hasXtcExtension(book.path)) {
          // Handle XTC file
          Xtc xtc(book.path, "/.crosspoint");
          if (xtc.load()) {
            // Try to generate thumbnail image for Continue Reading card
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
            bool success = xtc.generateThumbBmp(coverHeight);
            if (!success) {
              RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
              book.coverBmpPath = "";
            }
            coverRendered = false;
            requestUpdate();
          }
        }
      }
    }
    progress++;
  }

  recentsLoaded = true;
  recentsLoading = false;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  hasOpdsServers = OPDS_STORE.hasServers();

  carouselIndex = 0;
  menuSelectorIndex = 0;

  const auto& metrics = UITheme::getInstance().getMetrics();
  loadRecentBooks(metrics.homeRecentBooksCount);

  const auto base = static_cast<int>(recentBooks.size());
  selectorIndex = initialMenuItem == HomeMenuItem::NONE ? 0 : base + menuItemToIndex(initialMenuItem, hasOpdsServers);

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Free the stored cover buffer if any
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  // render() must have already set the cover rect; without it we'd be back to
  // cloning the whole framebuffer.
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  freeCoverBuffer();
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;
  coverBuffer = static_cast<uint8_t*>(malloc(needed));
  if (!coverBuffer) {
    LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", (unsigned)needed);
    return false;
  }
  coverBufferSize = needed;
  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize)) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
    return false;
  }
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferSize = 0;
  coverBufferStored = false;
}

void HomeActivity::loop() {
  const auto& metrics = UITheme::getInstance().getMetrics();

  if (metrics.homeCarouselMode) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      if (!recentBooks.empty()) {
        carouselIndex = (carouselIndex + 1) % static_cast<int>(recentBooks.size());
        coverRendered = false;
        freeCoverBuffer();
      }
      requestUpdate();
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      if (!recentBooks.empty()) {
        carouselIndex = (carouselIndex - 1 + static_cast<int>(recentBooks.size())) %
                        static_cast<int>(recentBooks.size());
        coverRendered = false;
        freeCoverBuffer();
      }
      requestUpdate();
    }

    const int menuCount = getMenuItemCount();
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      menuSelectorIndex = ButtonNavigator::nextIndex(menuSelectorIndex, menuCount);
      requestUpdate();
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      menuSelectorIndex = ButtonNavigator::previousIndex(menuSelectorIndex, menuCount);
      requestUpdate();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      int idx = 0;
      const int continueReadingIdx = idx++;
      const int fileBrowserIdx = idx++;
      const int recentsIdx = idx++;
      const int opdsLibraryIdx = hasOpdsServers ? idx++ : -1;
      const int fileTransferIdx = idx++;
      const int settingsIdx = idx;

      if (menuSelectorIndex == continueReadingIdx && !recentBooks.empty()) {
        onSelectBook(recentBooks[carouselIndex].path);
      } else if (menuSelectorIndex == fileBrowserIdx) {
        onFileBrowserOpen();
      } else if (menuSelectorIndex == recentsIdx) {
        onRecentsOpen();
      } else if (menuSelectorIndex == opdsLibraryIdx) {
        onOpdsBrowserOpen();
      } else if (menuSelectorIndex == fileTransferIdx) {
        onFileTransferOpen();
      } else if (menuSelectorIndex == settingsIdx) {
        onSettingsOpen();
      }
    }
  } else {
    const int menuCount = getMenuItemCount();

    buttonNavigator.onNext([this, menuCount] {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
      requestUpdate();
    });

    buttonNavigator.onPrevious([this, menuCount] {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
      requestUpdate();
    });

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (selectorIndex < static_cast<int>(recentBooks.size())) {
        onSelectBook(recentBooks[selectorIndex].path);
      } else {
        const int menuIndex = selectorIndex - static_cast<int>(recentBooks.size());
        switch (indexToMenuItem(menuIndex, hasOpdsServers)) {
          case HomeMenuItem::FILE_BROWSER:
            onFileBrowserOpen();
            break;
          case HomeMenuItem::RECENTS:
            onRecentsOpen();
            break;
          case HomeMenuItem::OPDS_BROWSER:
            onOpdsBrowserOpen();
            break;
          case HomeMenuItem::FILE_TRANSFER:
            onFileTransferOpen();
            break;
          case HomeMenuItem::SETTINGS_MENU:
            onSettingsOpen();
            break;
          default:
            break;
        }
      }
    }
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  const int coverSelector = metrics.homeCarouselMode ? carouselIndex : selectorIndex;

  // Guard against a race where the render task writes coverRendered=true after the button
  // handler reset it to false (carousel changed during slow SD cover reads). If the stored
  // cover is for a different index than the one we're about to display, force a re-render.
  if (coverRenderedForIndex != coverSelector) {
    coverRendered = false;
    freeCoverBuffer();
  }

  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding},
                 metrics.homeContinueReadingInMenu && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr);

  // Record the tile rect so storeCoverBuffer (called from the theme) knows
  // which sub-region of the framebuffer to snapshot. ~16 KB in Portrait
  // instead of the 48 KB full framebuffer the previous bind captured.
  coverRectX = 0;
  coverRectY = metrics.homeTopPadding;
  coverRectW = pageWidth;
  coverRectH = metrics.homeCoverTileHeight;

  GUI.drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                          recentBooks, coverSelector, coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this));

  if (coverRendered) {
    coverRenderedForIndex = coverSelector;
  }

  // Build menu items dynamically
  std::vector<const char*> menuItems = {tr(STR_BROWSE_FILES), tr(STR_MENU_RECENT_BOOKS), tr(STR_FILE_TRANSFER),
                                        tr(STR_SETTINGS_TITLE)};
  std::vector<UIIcon> menuIcons = {Folder, Recent, Transfer, Settings};

  if (hasOpdsServers) {
    menuItems.insert(menuItems.begin() + 2, tr(STR_OPDS_BROWSER));
    menuIcons.insert(menuIcons.begin() + 2, Library);
  }

  if ((metrics.homeCarouselMode || metrics.homeContinueReadingInMenu) && !recentBooks.empty()) {
    // Insert Continue Reading at the top if enabled in theme
    menuItems.insert(menuItems.begin(), tr(STR_CONTINUE_READING));
    menuIcons.insert(menuIcons.begin(), Book);
  }

  int menuSelector;
  if (metrics.homeCarouselMode) {
    menuSelector = menuSelectorIndex;
  } else if (metrics.homeContinueReadingInMenu) {
    menuSelector = selectorIndex;
  } else {
    menuSelector = selectorIndex - static_cast<int>(recentBooks.size());
  }

  GUI.drawButtonMenu(
      renderer,
      Rect{0, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset, pageWidth,
           pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing +
                         metrics.homeMenuTopOffset + metrics.buttonHintsHeight)},
      static_cast<int>(menuItems.size()), menuSelector,
      [&menuItems](int index) { return std::string(menuItems[index]); },
      [&menuIcons](int index) { return menuIcons[index]; });

  const auto labels = mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  } else if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    loadRecentCovers(metrics.homeCoverHeight);
  }
}

void HomeActivity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onRecentsOpen() { activityManager.goToRecentBooks(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }
