#include "StudyListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "StudyUi.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;
namespace {
constexpr fui::ActionId ACTION_ROW = 1;
}

StudyListActivity::StudyListActivity(std::string name, GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity(std::move(name), renderer, mappedInput),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void StudyListActivity::onEnter() {
  Activity::onEnter();
  uiReady = false;
  topIndex = 0;
  visibleRows = 1;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &StudyListActivity::onRowEvent, this);
  app.setScreen(&StudyListActivity::listScreen, this);
  requestUpdate();
}

void StudyListActivity::refreshRows() {
  for (size_t i = 0; i < rows.size(); ++i) rows[i].actionValue = static_cast<int16_t>(i);
  if (selectedIndex >= static_cast<int>(rows.size()))
    selectedIndex = rows.empty() ? 0 : static_cast<int>(rows.size()) - 1;
  app.setScreen(&StudyListActivity::listScreen, this);
  requestUpdate();
}

void StudyListActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<StudyListActivity*>(user);
  if (event.value < 0 || event.value >= static_cast<int>(self->rows.size())) return;
  self->selectedIndex = event.value;
  self->app.clearTapFlash();
  self->onRowChosen(event.value);
}

void StudyListActivity::loop() {
  if (StudyUi::backRequested(mappedInput, renderer)) {
    finish();
    return;
  }
  const int count = static_cast<int>(rows.size());
  if (count == 0) return;
  if (uiReady) {
    const auto snapshot = touchSnapshotFrom(mappedInput);
    if (snapshot.touchPressed || snapshot.touchReleased) {
      const auto event = app.route(snapshot);
      if (app.invalidated()) requestUpdate();
      if (event) return;
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    onRowChosen(selectedIndex);
    return;
  }
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const int next = scrollListBy(topIndex, swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows,
                                  visibleRows, count);
    if (next != topIndex) {
      topIndex = next;
      requestUpdate();
    }
    return;
  }
  const auto move = [this, count](const int index) {
    selectedIndex = index;
    topIndex = followListSelection(selectedIndex, topIndex, visibleRows, count);
    requestUpdate();
  };
  buttonNavigator.onNextRelease([this, &move, count] { move(ButtonNavigator::nextIndex(selectedIndex, count)); });
  buttonNavigator.onPreviousRelease(
      [this, &move, count] { move(ButtonNavigator::previousIndex(selectedIndex, count)); });
}

void StudyListActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<StudyListActivity*>(user)->buildListScreen(screen);
}

void StudyListActivity::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput)), 0,
                  static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  if (rows.empty()) return;

  fui::ListProps props;
  props.items = rows.data();
  props.count = static_cast<uint16_t>(rows.size());
  props.selectedIndex = static_cast<int16_t>(selectedIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.labelText = screen.theme().bodyText;
  const auto visible = configureUiList(props, screen.theme(), screen.body());
  visibleRows = visible > 0 ? visible : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, static_cast<int>(rows.size()));
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void StudyListActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, title(), false, 0, subtitle());
  } else {
    GUI.drawHeader(renderer, header, title(), subtitle());
  }
  uiReady = false;
  if (rows.empty()) {
    const Rect area{metrics.contentSidePadding, 0, renderer.getScreenWidth() - metrics.contentSidePadding * 2, 0};
    int y = renderer.getScreenHeight() / 3;
    if (emptyMessage() != nullptr) {
      y += UITheme::drawCenteredWrappedText(renderer, area, StudyUi::titleFont(), y, emptyMessage(), 3, true,
                                            EpdFontFamily::BOLD);
      y += metrics.verticalSpacing;
    }
    if (emptyHint() != nullptr) {
      UITheme::drawCenteredWrappedText(renderer, area, StudyUi::bodyFont(), y, emptyHint(), 6);
    }
  } else {
    app.render();
    uiReady = true;
  }
  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
