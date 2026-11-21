#include "StudyPackStore.h"

#include <Arduino.h>
#include <HalClock.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>

namespace study {

namespace {

constexpr size_t kMaxManifestBytes = 64 * 1024;
constexpr size_t kScanChunk = 512;
constexpr size_t kPeekBytes = 40;           // "c" is the second key of every line, well inside this window
constexpr uint32_t kSrsMagic = 0x31535253;  // "SRS1"
constexpr uint16_t kMaxSrsRecords = 4096;
constexpr const char* kProgressPath = "/study/progress.jsonl";

// Fixed on-disk record: id[24], due u32, interval u16, ease100 u16, reps u8, last u8, 2 reserved bytes.
constexpr size_t kSrsRecordBytes = 36;

}  // namespace

bool loadManifest(std::vector<TrackInfo>& out) {
  out.clear();
  HalFile file;
  if (!Storage.openFileForRead("STUDY", "/study/manifest.json", file)) {
    LOG_INF("STUDY", "No study pack manifest on the SD card");
    return false;
  }
  const size_t size = file.fileSize();
  if (size == 0 || size > kMaxManifestBytes) {
    LOG_ERR("STUDY", "Manifest size %u is out of range", static_cast<unsigned>(size));
    file.close();
    return false;
  }
  // The manifest is read once per screen visit and parsed into compact TrackInfo records, then freed.
  auto json = makeUniqueNoThrow<char[]>(size);
  if (!json) {
    LOG_ERR("STUDY", "Cannot allocate %u bytes for the manifest", static_cast<unsigned>(size));
    file.close();
    return false;
  }
  const int read = file.read(json.get(), size);
  file.close();
  if (read != static_cast<int>(size)) {
    LOG_ERR("STUDY", "Short manifest read (%d of %u)", read, static_cast<unsigned>(size));
    return false;
  }
  if (!parseManifest(json.get(), size, out)) {
    LOG_ERR("STUDY", "Manifest is not a Cert Study Hub study pack");
    return false;
  }
  return true;
}

uint32_t nowUnixSeconds() {
  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  uint8_t hour = 0;
  uint8_t minute = 0;
  if (!halClock.getDateTime(year, month, day, hour, minute)) return 0;
  return unixSecondsUtc(year, month, day, hour, minute);
}

// ---- JsonlFile ----

bool JsonlFile::open(const std::string& path, const bool keepIds) {
  close();
  if (!Storage.openFileForRead("STUDY", path, file)) {
    LOG_ERR("STUDY", "Cannot open %s", path.c_str());
    return false;
  }
  const size_t size = file.fileSize();
  // Pack lines average ~350 bytes; reserving from the size avoids repeated vector growth while scanning.
  offsets.reserve(size / 256 + 1);
  categories.reserve(size / 256 + 1);
  if (keepIds) ids.reserve(size / 256 + 1);
  const auto finishLine = [&](const char* peekBuf, const size_t peekBytes) {
    categories.push_back(peekCategory(peekBuf, peekBytes));
    if (keepIds) {
      ShortId id{};
      peekId(peekBuf, peekBytes, id.text, sizeof(id.text));
      ids.push_back(id);
    }
  };

  char chunk[kScanChunk];
  char peek[kPeekBytes];
  size_t peekLen = 0;
  bool inLine = false;
  uint32_t pos = 0;
  int n = 0;
  while ((n = file.read(chunk, sizeof(chunk))) > 0) {
    for (int i = 0; i < n; ++i, ++pos) {
      const char c = chunk[i];
      if (c == '\n' || c == '\r') {
        if (inLine) {
          finishLine(peek, peekLen);
          inLine = false;
        }
        continue;
      }
      if (!inLine) {
        inLine = true;
        offsets.push_back(pos);
        peekLen = 0;
      }
      if (peekLen < sizeof(peek)) peek[peekLen++] = c;
    }
  }
  if (inLine) finishLine(peek, peekLen);
  LOG_DBG("STUDY", "%s: %u lines", path.c_str(), static_cast<unsigned>(offsets.size()));
  return true;
}

void JsonlFile::close() {
  if (file) file.close();
  offsets.clear();
  offsets.shrink_to_fit();
  categories.clear();
  categories.shrink_to_fit();
  ids.clear();
  ids.shrink_to_fit();
}

size_t JsonlFile::readLine(const size_t index, char* buf, const size_t cap) {
  if (index >= offsets.size() || buf == nullptr || cap < 2 || !file) return 0;
  if (!file.seekSet(offsets[index])) {
    LOG_ERR("STUDY", "Seek to line %u failed", static_cast<unsigned>(index));
    return 0;
  }
  const int n = file.read(buf, cap - 1);
  if (n <= 0) {
    LOG_ERR("STUDY", "Read of line %u failed", static_cast<unsigned>(index));
    return 0;
  }
  size_t len = 0;
  while (len < static_cast<size_t>(n) && buf[len] != '\n' && buf[len] != '\r') ++len;
  if (len == static_cast<size_t>(n) && len == cap - 1) {
    LOG_ERR("STUDY", "Line %u is longer than %u bytes", static_cast<unsigned>(index), static_cast<unsigned>(cap - 1));
    return 0;
  }
  buf[len] = '\0';
  return len;
}

// ---- SrsStore ----

bool SrsStore::load(const std::string& trackKey) {
  track = trackKey;
  records.clear();
  isDirty = false;
  if (!isSafeTrackKey(trackKey)) return false;
  const std::string path = std::string(kStudyRoot) + "/.srs/" + trackKey + ".bin";
  if (!Storage.exists(path.c_str())) return true;  // nothing rated yet on this reader
  HalFile file;
  if (!Storage.openFileForRead("STUDY", path, file)) return false;
  uint8_t header[6];
  if (file.read(header, sizeof(header)) != static_cast<int>(sizeof(header))) {
    LOG_ERR("STUDY", "Schedule %s is truncated", path.c_str());
    file.close();
    return false;
  }
  uint32_t magic = 0;
  uint16_t count = 0;
  memcpy(&magic, header, 4);
  memcpy(&count, header + 4, 2);
  if (magic != kSrsMagic || count > kMaxSrsRecords) {
    LOG_ERR("STUDY", "Schedule %s has an unknown format; starting fresh", path.c_str());
    file.close();
    return true;
  }
  records.reserve(count);
  uint8_t raw[kSrsRecordBytes];
  for (uint16_t i = 0; i < count; ++i) {
    if (file.read(raw, sizeof(raw)) != static_cast<int>(sizeof(raw))) break;
    Record r{};
    memcpy(r.id, raw, kMaxItemIdLen);
    r.id[kMaxItemIdLen] = '\0';
    memcpy(&r.entry.due, raw + 24, 4);
    memcpy(&r.entry.intervalDays, raw + 28, 2);
    memcpy(&r.entry.ease100, raw + 30, 2);
    r.entry.reps = raw[32];
    r.entry.last = raw[33];
    records.push_back(r);
  }
  file.close();
  return true;
}

bool SrsStore::save() {
  if (!isDirty) return true;
  if (!isSafeTrackKey(track)) return false;
  const std::string dir = std::string(kStudyRoot) + "/.srs";
  if (!Storage.ensureDirectoryExists(dir.c_str())) {
    LOG_ERR("STUDY", "Cannot create %s", dir.c_str());
    return false;
  }
  const std::string path = dir + "/" + track + ".bin";
  const std::string tmp = path + ".tmp";
  HalFile file;
  if (!Storage.openFileForWrite("STUDY", tmp, file)) return false;
  uint8_t header[6];
  const uint16_t count = static_cast<uint16_t>(records.size());
  memcpy(header, &kSrsMagic, 4);
  memcpy(header + 4, &count, 2);
  bool ok = file.write(header, sizeof(header)) == sizeof(header);
  uint8_t raw[kSrsRecordBytes];
  for (const Record& r : records) {
    if (!ok) break;
    memset(raw, 0, sizeof(raw));
    memcpy(raw, r.id, strnlen(r.id, kMaxItemIdLen));
    memcpy(raw + 24, &r.entry.due, 4);
    memcpy(raw + 28, &r.entry.intervalDays, 2);
    memcpy(raw + 30, &r.entry.ease100, 2);
    raw[32] = r.entry.reps;
    raw[33] = r.entry.last;
    ok = file.write(raw, sizeof(raw)) == sizeof(raw);
  }
  ok = file.sync() && ok;
  file.close();
  if (!ok) {
    LOG_ERR("STUDY", "Writing %s failed", tmp.c_str());
    Storage.remove(tmp.c_str());
    return false;
  }
  if (Storage.exists(path.c_str())) Storage.remove(path.c_str());
  if (!Storage.rename(tmp.c_str(), path.c_str())) {
    LOG_ERR("STUDY", "Cannot replace %s", path.c_str());
    return false;
  }
  isDirty = false;
  return true;
}

const SrsEntry* SrsStore::find(const std::string& cardId) const {
  for (const Record& r : records) {
    if (cardId == r.id) return &r.entry;
  }
  return nullptr;
}

void SrsStore::set(const std::string& cardId, const SrsEntry& entry) {
  if (cardId.empty() || cardId.size() > kMaxItemIdLen) return;
  isDirty = true;
  for (Record& r : records) {
    if (cardId == r.id) {
      r.entry = entry;
      return;
    }
  }
  if (records.size() >= kMaxSrsRecords) return;
  Record r{};
  memcpy(r.id, cardId.c_str(), cardId.size());
  r.entry = entry;
  records.push_back(r);
}

// ---- ProgressLog ----

bool ProgressLog::append(const char* line, const size_t len) {
  if (len == 0) return false;
  HalFile file = Storage.open(kProgressPath, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    LOG_ERR("STUDY", "Cannot open %s for append", kProgressPath);
    return false;
  }
  bool ok = true;
  if (file.size() == 0) {
    // A new log needs a random id so the app can tell a continued log from a fresh one.
    char logId[16];
    snprintf(logId, sizeof(logId), "st%08lx", static_cast<unsigned long>(random(0x7fffffff)));
    char header[96];
    const size_t headerLen = formatLogHeader(header, sizeof(header), logId);
    ok = headerLen > 0 && file.write(header, headerLen) == headerLen;
  }
  ok = ok && file.write(line, len) == len;
  ok = file.sync() && ok;
  file.close();
  if (!ok) LOG_ERR("STUDY", "Appending to %s failed", kProgressPath);
  return ok;
}

bool ProgressLog::logCard(const std::string& track, const std::string& cardId, const int quality) {
  char line[128];
  return append(line, formatCardEvent(line, sizeof(line), track.c_str(), cardId.c_str(), quality, nowUnixSeconds()));
}

bool ProgressLog::logQuestion(const std::string& track, const std::string& questionId, const bool correct) {
  char line[128];
  return append(line,
                formatQuestionEvent(line, sizeof(line), track.c_str(), questionId.c_str(), correct, nowUnixSeconds()));
}

}  // namespace study
