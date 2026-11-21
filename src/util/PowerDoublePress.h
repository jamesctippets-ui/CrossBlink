#pragma once

#include <cstdint>

// Tells one short Power click apart from two quick clicks.
//
// It only runs while a double-press action is configured. A short release is
// then held back for WINDOW_MS. A second press inside that window becomes a
// double press. Otherwise the held click is handed back as an ordinary short
// press, so the configured short-press action still runs, just slightly later.
// Long presses never enter here: hold-to-sleep and other long-press actions
// keep their existing timing.
//
// Times are button-sample timestamps, so a busy UI that delivers queued input
// late does not stretch or shrink the gap between the two clicks.
class PowerDoublePress {
 public:
  // Matches the X4 Pro Home key's double-tap window.
  static constexpr uint32_t WINDOW_MS = 300;
  // A held click older than this is dropped instead of replayed. The main loop
  // can skip this detector for a while (for example while Quick Lock or a
  // chord owns input), and a click from seconds ago must not fire later.
  static constexpr uint32_t STALE_MS = WINDOW_MS + 1000;

  enum class Event : uint8_t {
    None,
    // A short release was captured. Hide it from every other consumer.
    HoldClick,
    // The window passed with no second press. Deliver the held click now.
    ReplayClick,
    // A second press arrived in time. Run the double-press action and hide
    // this press and its release.
    DoublePress,
  };

  struct Input {
    uint32_t now = 0;
    // Power press edge in this input frame.
    bool pressed = false;
    // Power release edge in this frame that ended a short press and has not
    // already been claimed by another handler.
    bool shortRelease = false;
    // How long the released click was held, replayed with the click.
    uint32_t heldMs = 0;
    // Whether a new gesture may start on this screen. A pending click still
    // completes after this turns false.
    bool canStart = false;
  };

  Event update(const Input& input) {
    if (pending_) {
      const uint32_t age = input.now - releasedAt_;
      if (age > STALE_MS) {
        pending_ = false;
        return Event::None;
      }
      if (age > WINDOW_MS) {
        pending_ = false;
        return Event::ReplayClick;
      }
      // Shared Confirm/Power hardware can report a whole click (press and
      // release) in one frame. The press edge alone decides a double press.
      if (input.pressed) {
        pending_ = false;
        return Event::DoublePress;
      }
      return Event::None;
    }

    if (!input.shortRelease || !input.canStart) return Event::None;
    pending_ = true;
    releasedAt_ = input.now;
    heldMs_ = input.heldMs;
    return Event::HoldClick;
  }

  void reset() { pending_ = false; }
  bool pending() const { return pending_; }
  uint32_t heldMs() const { return heldMs_; }

 private:
  bool pending_ = false;
  uint32_t releasedAt_ = 0;
  uint32_t heldMs_ = 0;
};
