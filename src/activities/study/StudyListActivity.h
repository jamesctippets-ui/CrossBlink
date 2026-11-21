#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <atomic>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// A FreeInkUI list screen with a touch Back header, shared by the study track list and track menu. Subclasses fill
// `rows` (and keep the strings they point at alive) and react to a chosen row.
class StudyListActivity : public Activity {
 protected:
  using UiApp = freeink::ui::FreeInkApp<24, 4>;

  StudyListActivity(std::string name, GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

  virtual const char* title() const = 0;
  virtual const char* subtitle() const { return nullptr; }
  virtual void onRowChosen(int index) = 0;
  // Drawn instead of the list when there are no rows.
  virtual const char* emptyMessage() const { return nullptr; }
  virtual const char* emptyHint() const { return nullptr; }

  // Rebuilds the list after rows or their labels change.
  void refreshRows();

  std::vector<freeink::ui::ListItem> rows;
  int selectedIndex = 0;

 private:
  ButtonNavigator buttonNavigator;
  freeink::ui::GfxRendererTarget uiTarget;
  UiApp app;
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;
  int topIndex = 0;

  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiApp::ScreenType& screen);
};
