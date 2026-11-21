#include "StudyPackParse.h"

#include <ArduinoJson.h>

#include <cstdio>
#include <cstring>

namespace study {

namespace {

bool isSafeId(const char* id) {
  if (id == nullptr || id[0] == '\0') return false;
  const size_t len = strlen(id);
  if (len > kMaxItemIdLen) return false;
  // Ids are written back into JSON unescaped, so keep them to characters that never need escaping.
  for (size_t i = 0; i < len; ++i) {
    const char c = id[i];
    const bool ok =
        (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    if (!ok) return false;
  }
  return true;
}

void copyString(JsonVariantConst value, std::string& out) {
  const char* s = value.as<const char*>();
  out.assign(s != nullptr ? s : "");
}

uint16_t clampU16(JsonVariantConst value) {
  if (!value.is<int>()) return 0;
  const int v = value.as<int>();
  if (v < 0) return 0;
  return static_cast<uint16_t>(v > 65535 ? 65535 : v);
}

}  // namespace

bool isSafeTrackKey(const std::string& key) {
  if (key.empty() || key.size() > kMaxTrackKeyLen) return false;
  for (const char c : key) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!ok) return false;
  }
  return true;
}

bool parseManifest(const char* json, const size_t len, std::vector<TrackInfo>& out) {
  out.clear();
  JsonDocument doc;
  if (deserializeJson(doc, json, len)) return false;
  const char* pack = doc["pack"].as<const char*>();
  if (pack == nullptr || strcmp(pack, kPackName) != 0) return false;
  JsonArrayConst tracks = doc["tracks"].as<JsonArrayConst>();
  if (tracks.isNull()) return false;
  out.reserve(tracks.size());
  for (JsonObjectConst t : tracks) {
    TrackInfo info;
    copyString(t["key"], info.key);
    if (!isSafeTrackKey(info.key)) continue;
    copyString(t["label"], info.label);
    if (info.label.empty()) info.label = info.key;
    copyString(t["subtitle"], info.subtitle);
    JsonArrayConst cats = t["categories"].as<JsonArrayConst>();
    info.categories.reserve(cats.size());
    for (JsonVariantConst c : cats) {
      const char* name = c.as<const char*>();
      info.categories.emplace_back(name != nullptr ? name : "");
    }
    info.cards = clampU16(t["cards"]);
    info.questions = clampU16(t["questions"]);
    JsonObjectConst exam = t["exam"].as<JsonObjectConst>();
    if (!exam.isNull()) {
      info.exam.length = clampU16(exam["length"]);
      info.exam.minutes = clampU16(exam["minutes"]);
      const uint16_t pass = clampU16(exam["passPct"]);
      info.exam.passPct = static_cast<uint8_t>(pass > 100 ? 100 : pass);
    }
    out.push_back(std::move(info));
  }
  return true;
}

bool parseCardLine(const char* line, const size_t len, Card& out) {
  JsonDocument doc;
  if (deserializeJson(doc, line, len)) return false;
  const char* id = doc["id"].as<const char*>();
  const char* term = doc["term"].as<const char*>();
  const char* def = doc["def"].as<const char*>();
  if (!isSafeId(id) || term == nullptr || def == nullptr) return false;
  out.id.assign(id);
  out.term.assign(term);
  out.def.assign(def);
  copyString(doc["prompt"], out.prompt);
  copyString(doc["eg"], out.example);
  out.category = doc["c"].is<int>() ? static_cast<int16_t>(doc["c"].as<int>()) : -1;
  return true;
}

bool parseQuestionLine(const char* line, const size_t len, Question& out) {
  JsonDocument doc;
  if (deserializeJson(doc, line, len)) return false;
  const char* id = doc["id"].as<const char*>();
  const char* text = doc["q"].as<const char*>();
  const char* type = doc["type"].as<const char*>();
  if (!isSafeId(id) || text == nullptr || type == nullptr) return false;
  if (strcmp(type, "mc") == 0) {
    out.type = QuestionType::Single;
  } else if (strcmp(type, "ms") == 0) {
    out.type = QuestionType::Multi;
  } else if (strcmp(type, "tf") == 0) {
    out.type = QuestionType::TrueFalse;
  } else {
    return false;
  }
  JsonArrayConst opts = doc["opts"].as<JsonArrayConst>();
  if (opts.isNull() || opts.size() < 2 || opts.size() > kMaxOptions) return false;
  out.optionCount = 0;
  for (JsonVariantConst o : opts) {
    copyString(o, out.options[out.optionCount]);
    ++out.optionCount;
  }
  out.answerMask = 0;
  for (JsonVariantConst a : doc["ans"].as<JsonArrayConst>()) {
    if (!a.is<int>()) return false;
    const int index = a.as<int>();
    if (index < 0 || index >= out.optionCount) return false;
    out.answerMask = static_cast<uint8_t>(out.answerMask | (1u << index));
  }
  if (out.answerMask == 0) return false;
  out.id.assign(id);
  out.text.assign(text);
  copyString(doc["why"], out.why);
  out.category = doc["c"].is<int>() ? static_cast<int16_t>(doc["c"].as<int>()) : -1;
  return true;
}

int8_t peekCategory(const char* lineStart, const size_t len) {
  static constexpr char kKey[] = "\"c\":";
  constexpr size_t keyLen = sizeof(kKey) - 1;
  if (lineStart == nullptr) return -1;
  for (size_t i = 0; i + keyLen < len; ++i) {
    if (memcmp(lineStart + i, kKey, keyLen) != 0) continue;
    size_t j = i + keyLen;
    int value = 0;
    bool digits = false;
    while (j < len && lineStart[j] >= '0' && lineStart[j] <= '9' && value < 128) {
      value = value * 10 + (lineStart[j] - '0');
      digits = true;
      ++j;
    }
    return digits && value < 128 ? static_cast<int8_t>(value) : -1;
  }
  return -1;
}

size_t peekId(const char* lineStart, const size_t len, char* out, const size_t cap) {
  static constexpr char kKey[] = "\"id\":\"";
  constexpr size_t keyLen = sizeof(kKey) - 1;
  if (lineStart == nullptr || out == nullptr || cap < 2) return 0;
  out[0] = '\0';
  for (size_t i = 0; i + keyLen < len; ++i) {
    if (memcmp(lineStart + i, kKey, keyLen) != 0) continue;
    size_t n = 0;
    for (size_t j = i + keyLen; j < len && lineStart[j] != '"'; ++j) {
      if (n + 1 >= cap) {
        out[0] = '\0';
        return 0;
      }
      out[n++] = lineStart[j];
    }
    if (i + keyLen + n >= len) {  // no closing quote inside the window
      out[0] = '\0';
      return 0;
    }
    out[n] = '\0';
    if (!isSafeId(out)) {
      out[0] = '\0';
      return 0;
    }
    return n;
  }
  return 0;
}

size_t formatLogHeader(char* buf, const size_t cap, const char* logId) {
  if (buf == nullptr || cap == 0 || !isSafeId(logId)) return 0;
  const int n = snprintf(buf, cap, "{\"log\":\"%s\",\"v\":1,\"id\":\"%s\"}\n", kPackName, logId);
  return n > 0 && static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : 0;
}

size_t formatCardEvent(char* buf, const size_t cap, const char* track, const char* cardId, const int quality,
                       const uint32_t t) {
  if (buf == nullptr || cap == 0 || track == nullptr || !isSafeTrackKey(track) || !isSafeId(cardId) || quality < 1 ||
      quality > 5) {
    return 0;
  }
  const int n =
      t > 0 ? snprintf(buf, cap, "{\"k\":\"c\",\"tr\":\"%s\",\"id\":\"%s\",\"q\":%d,\"t\":%lu}\n", track, cardId,
                       quality, static_cast<unsigned long>(t))
            : snprintf(buf, cap, "{\"k\":\"c\",\"tr\":\"%s\",\"id\":\"%s\",\"q\":%d}\n", track, cardId, quality);
  return n > 0 && static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : 0;
}

size_t formatQuestionEvent(char* buf, const size_t cap, const char* track, const char* questionId, const bool correct,
                           const uint32_t t) {
  if (buf == nullptr || cap == 0 || track == nullptr || !isSafeTrackKey(track) || !isSafeId(questionId)) return 0;
  const int n = t > 0 ? snprintf(buf, cap, "{\"k\":\"q\",\"tr\":\"%s\",\"id\":\"%s\",\"ok\":%d,\"t\":%lu}\n", track,
                                 questionId, correct ? 1 : 0, static_cast<unsigned long>(t))
                      : snprintf(buf, cap, "{\"k\":\"q\",\"tr\":\"%s\",\"id\":\"%s\",\"ok\":%d}\n", track, questionId,
                                 correct ? 1 : 0);
  return n > 0 && static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : 0;
}

}  // namespace study
