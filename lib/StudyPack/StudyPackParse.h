#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Pure parsing and formatting for Cert Study Hub's e-ink "study pack" (EINK_STUDY_PACK.md in the Study-App repo).
// Nothing here touches storage, so the native test suite can cover it. File access lives in StudyPackStore.
namespace study {

constexpr const char* kPackName = "cert-study-eink";
constexpr uint8_t kMaxOptions = 6;
constexpr size_t kMaxTrackKeyLen = 24;
constexpr size_t kMaxItemIdLen = 23;

enum class QuestionType : uint8_t { Single, Multi, TrueFalse };

struct ExamSettings {
  uint16_t length = 0;   // questions per practice test (0 = no settings in the pack)
  uint16_t minutes = 0;  // time limit (0 = untimed)
  uint8_t passPct = 0;   // pass mark in percent (0 = none)
};

struct TrackInfo {
  std::string key;
  std::string label;
  std::string subtitle;
  std::vector<std::string> categories;
  uint16_t cards = 0;
  uint16_t questions = 0;
  ExamSettings exam;
};

struct Card {
  std::string id;
  std::string term;
  std::string prompt;  // definition with the term blanked; empty when the pack has none (show def instead)
  std::string def;
  std::string example;
  int16_t category = -1;

  const std::string& front() const { return prompt.empty() ? def : prompt; }
};

struct Question {
  std::string id;
  std::string text;
  std::string why;
  std::string options[kMaxOptions];
  uint8_t optionCount = 0;
  uint8_t answerMask = 0;  // bit i set when options[i] is a right answer
  QuestionType type = QuestionType::Single;
  int16_t category = -1;
};

// Track keys become folder names, so only [a-z0-9_-] is accepted.
bool isSafeTrackKey(const std::string& key);

bool parseManifest(const char* json, size_t len, std::vector<TrackInfo>& out);
bool parseCardLine(const char* line, size_t len, Card& out);
bool parseQuestionLine(const char* line, size_t len, Question& out);

// Formats one progress.jsonl line, newline included. Returns its length, or 0 when it does not fit or an argument is
// invalid. `t` is Unix seconds; 0 leaves it out (the app then uses the import time).
size_t formatLogHeader(char* buf, size_t cap, const char* logId);
size_t formatCardEvent(char* buf, size_t cap, const char* track, const char* cardId, int quality, uint32_t t);
size_t formatQuestionEvent(char* buf, size_t cap, const char* track, const char* questionId, bool correct, uint32_t t);

// Category index from the first bytes of a pack line ("c":N), without parsing the JSON. Returns -1 when absent.
int8_t peekCategory(const char* lineStart, size_t len);

// Copies the line's "id" value from its first bytes into out (NUL-terminated). Returns its length, or 0 when the id is
// absent, not a safe id, or does not fit in cap - 1 bytes.
size_t peekId(const char* lineStart, size_t len, char* out, size_t cap);

// True when the chosen options are exactly the right ones.
inline bool isAnswerCorrect(const Question& q, const uint8_t chosenMask) { return chosenMask == q.answerMask; }

}  // namespace study
