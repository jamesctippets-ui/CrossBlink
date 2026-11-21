#pragma once

#include <cstdint>
#include <string>

#include "AppCapabilities.h"
#include "Dictionary.h"

// Online fallback for dictionary lookups (Sticky only).
//
// When the active offline dictionary has no entry for a word, the lookup
// worker calls OnlineDictionary::lookup(). It briefly joins a saved Wi-Fi
// network (reusing an existing connection when there is one), queries the free
// Free Dictionary API (https://dictionaryapi.dev, English, no API key), and
// writes the result to a tiny single-entry StarDict pseudo-dictionary under
// /.crosspoint/Online Dictionary/. The returned DictLocation points at that
// entry, so DictionaryDefinitionActivity renders it exactly like an offline
// definition (paging, word-select, history) with no new UI code.
//
// Wi-Fi is only switched on when it was off, and is switched back off before
// returning, so the radio is never left running in the background.
namespace OnlineDictionary {

enum class Result : uint8_t {
  Found,      // `out` points at a renderable definition
  NotFound,   // the service answered but has no entry (or returned an error)
  NoNetwork,  // no saved network in range / connection failed
  Failed,     // low memory, SD write failure, or similar local problem
  Cancelled,  // user pressed Back while the lookup was running
  Skipped,    // feature disabled, or active dictionary is not English
};

#if CROSSINK_APP_ONLINE_DICTIONARY
// True when the user has the "Online Dictionary Fallback" setting on.
bool isEnabled();

// Blocking lookup; call from the dictionary lookup worker, never the main loop.
// `activeDictPath` is the offline dictionary base path (used to skip non-English
// dictionaries). Runs the network work on a short-lived task with a stack large
// enough for TLS, polling `shouldCancel(ctx)` while it waits.
Result lookup(const std::string& word, const std::string& activeDictPath, DictLocation& out,
              bool (*shouldCancel)(void* ctx), void* ctx);
#else
inline bool isEnabled() { return false; }
inline Result lookup(const std::string&, const std::string&, DictLocation&, bool (*)(void*), void*) {
  return Result::Skipped;
}
#endif

}  // namespace OnlineDictionary
