#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "StudyPackParse.h"
#include "StudySrs.h"

// SD card access for the study pack:
//   /study/manifest.json           tracks (written by the Study-App's tools/eink_pack.py)
//   /study/<track>/cards.jsonl     one flashcard per line
//   /study/<track>/questions.jsonl one question per line
//   /study/progress.jsonl          appended here; the app imports it ("Import e-reader progress")
//   /study/.srs/<track>.bin        this reader's own flashcard schedule
namespace study {

constexpr const char* kStudyRoot = "/study";
constexpr size_t kLineBufferSize = 4096;  // longest line in the full pack is ~1.6 KB

bool loadManifest(std::vector<TrackInfo>& out);

// Current UTC time in Unix seconds from the RTC, or 0 when the clock is not set.
uint32_t nowUnixSeconds();

// Random-access reader for a .jsonl file. open() scans the file once and keeps the byte offset (4 bytes) and
// category (1 byte) of every line, so items can be read one at a time with a single small buffer.
class JsonlFile {
 public:
  // keepIds also records each line's id (up to kShortIdLen chars; longer ids read back as empty), 12 bytes a line.
  bool open(const std::string& path, bool keepIds = false);
  void close();
  size_t count() const { return offsets.size(); }
  // Category index the line declares ("c"), or -1. Read during open() without parsing the JSON.
  int8_t categoryAt(size_t index) const { return index < categories.size() ? categories[index] : -1; }
  // Reads line `index` (without the newline) into buf, NUL-terminated. Returns its length, or 0 on failure.
  size_t readLine(size_t index, char* buf, size_t cap);
  // The id recorded by open(keepIds), or "" when not kept or too long.
  const char* idAt(size_t index) const { return index < ids.size() ? ids[index].text : ""; }

  static constexpr size_t kShortIdLen = 11;

 private:
  HalFile file;
  std::vector<uint32_t> offsets;
  std::vector<int8_t> categories;
  struct ShortId {
    char text[kShortIdLen + 1];
  };
  std::vector<ShortId> ids;
};

// The reader's flashcard schedule for one track, kept in RAM while the track is open (about 30 bytes a card).
class SrsStore {
 public:
  bool load(const std::string& trackKey);
  bool save();
  // Returns nullptr when the card has never been rated on this reader.
  const SrsEntry* find(const std::string& cardId) const;
  void set(const std::string& cardId, const SrsEntry& entry);
  bool dirty() const { return isDirty; }

 private:
  struct Record {
    char id[kMaxItemIdLen + 1];
    SrsEntry entry;
  };
  std::string track;
  std::vector<Record> records;
  bool isDirty = false;
};

// Appends events to /study/progress.jsonl, one flushed line at a time, writing the header when the file is new.
class ProgressLog {
 public:
  bool logCard(const std::string& track, const std::string& cardId, int quality);
  bool logQuestion(const std::string& track, const std::string& questionId, bool correct);

 private:
  bool append(const char* line, size_t len);
};

}  // namespace study
