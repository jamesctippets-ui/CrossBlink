#include "OnlineDictionary.h"

#if CROSSINK_APP_ONLINE_DICTIONARY

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "WifiCredentialStore.h"
#include "network/HttpDownloader.h"
#include "network/WifiUtils.h"

namespace OnlineDictionary {
namespace {

constexpr const char* kLogTag = "ODICT";
// The definition view shows the parent folder name as the dictionary name, so
// this folder doubles as the "Online Dictionary" label in the modal footer.
constexpr const char* kCacheDir = "/.crosspoint/Online Dictionary";
constexpr const char* kResponsePath = "/.crosspoint/Online Dictionary/response.json";
// Free Dictionary API: free, keyless, English, Wiktionary-sourced (CC BY-SA).
constexpr const char* kApiBase = "https://api.dictionaryapi.dev/api/v2/entries/en/";

// esp_http_client + mbedTLS run the TLS handshake on the calling task. The
// shared dictionary worker only has 4 KB, so the network work runs on its own
// short-lived task. 8 KB matches what TLS needs with headroom; it is allocated
// only for the duration of an online lookup and freed when the task exits.
constexpr uint32_t kTaskStackBytes = 8192;
// Wi-Fi start-up plus a TLS session need tens of KB of internal RAM. Refuse
// early (and log) rather than risk an allocation abort mid-handshake.
constexpr uint32_t kMinFreeInternalHeap = 48 * 1024;
constexpr uint32_t kConnectTimeoutMs = 10000;
constexpr uint32_t kPollMs = 50;
constexpr int kMaxNetworksToTry = 2;
// Common words ("set", "run") produce large responses. They stream to SD, but
// cap what we are willing to parse so a pathological body cannot exhaust RAM.
constexpr size_t kMaxResponseBytes = 512 * 1024;
constexpr int kMaxEntries = 3;
constexpr int kMaxMeaningsPerEntry = 6;
constexpr int kMaxDefinitionsPerMeaning = 5;
// Each cached word is a .ifo + .dict pair. Past this many files the folder is
// cleared so the cache stays small and directory scans stay cheap.
constexpr int kMaxCacheFiles = 128;

struct Job {
  const std::string* word = nullptr;
  DictLocation* out = nullptr;
  std::atomic<bool> cancel{false};
  std::atomic<bool> done{false};
  Result result = Result::Failed;
};

uint32_t fnv1a(const std::string& s) {
  uint32_t h = 2166136261u;
  for (const char c : s) {
    h ^= static_cast<uint8_t>(c);
    h *= 16777619u;
  }
  return h;
}

std::string normalizeWord(const std::string& word) {
  std::string out;
  out.reserve(word.size());
  for (const char c : word) {
    out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
  }
  return out;
}

std::string cacheBasePath(const std::string& normalizedWord) {
  char name[16];
  snprintf(name, sizeof(name), "/w%08lx", static_cast<unsigned long>(fnv1a(normalizedWord)));
  return std::string(kCacheDir) + name;
}

std::string urlEncode(const std::string& s) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 3);
  for (const char ch : s) {
    const auto c = static_cast<uint8_t>(ch);
    const bool unreserved = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                            c == '.' || c == '_' || c == '~';
    if (unreserved) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

bool isCached(const std::string& base, DictLocation& out, const std::string& word) {
  HalFile dict;
  if (!Storage.openFileForRead(kLogTag, base + ".dict", dict)) return false;
  const size_t size = dict.fileSize();
  dict.close();
  if (size == 0 || !Storage.exists((base + ".ifo").c_str())) return false;
  out = DictLocation{};
  out.folderPath = base;
  out.headword = word;
  out.offset = 0;
  out.size = static_cast<uint32_t>(size);
  out.found = true;
  return true;
}

void pruneCacheIfLarge() {
  HalFile dir = Storage.open(kCacheDir);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }
  int count = 0;
  for (HalFile f = dir.openNextFile(); f; f = dir.openNextFile()) {
    count++;
    f.close();
  }
  dir.close();
  if (count <= kMaxCacheFiles) return;
  LOG_INF(kLogTag, "Clearing online dictionary cache (%d files)", count);
  Storage.removeDir(kCacheDir);
  Storage.mkdir(kCacheDir);
}

// ---------------------------------------------------------------------------
// Wi-Fi
// ---------------------------------------------------------------------------

void wifiOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

bool waitForConnection(const Job& job) {
  const unsigned long start = millis();
  while (millis() - start < kConnectTimeoutMs) {
    if (job.cancel.load()) return false;
    const wl_status_t status = WiFi.status();
    if (status == WL_CONNECTED) return true;
    if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) return false;
    delay(kPollMs);
  }
  return false;
}

// Joins the strongest saved network that is in range. `startedWifi` reports
// whether this call turned the radio on, so the caller knows to turn it off.
Result connectToSavedNetwork(const Job& job, bool& startedWifi) {
  startedWifi = false;
  if (hasActiveStationWifiConnection()) {
    LOG_DBG(kLogTag, "Reusing existing Wi-Fi connection");
    return Result::Found;
  }
  if (WIFI_STORE.getCredentialCount() == 0) {
    LOG_INF(kLogTag, "No saved Wi-Fi networks");
    return Result::NoNetwork;
  }

  WiFi.persistent(false);  // Credentials live in WifiCredentialStore, not NVS.
  if (!WiFi.mode(WIFI_STA)) {
    LOG_ERR(kLogTag, "Failed to enter station mode");
    wifiOff();
    return Result::Failed;
  }
  startedWifi = true;

  // A blocking scan (~2 s) avoids burning a 10 s connect timeout on every
  // saved network that is out of range.
  const int found = WiFi.scanNetworks(false, false);
  if (job.cancel.load()) {
    WiFi.scanDelete();
    return Result::Cancelled;
  }
  struct Candidate {
    char ssid[33];
    int32_t rssi;
  };
  Candidate candidates[kMaxNetworksToTry] = {};
  int candidateCount = 0;
  for (int i = 0; i < found; i++) {
    char ssid[33];
    strlcpy(ssid, WiFi.SSID(i).c_str(), sizeof(ssid));
    if (ssid[0] == '\0' || !WIFI_STORE.hasSavedCredential(ssid)) continue;
    const int32_t rssi = WiFi.RSSI(i);
    bool duplicate = false;
    for (int c = 0; c < candidateCount; c++) {
      if (strcmp(candidates[c].ssid, ssid) == 0) {
        // Scan results are sorted strongest-first, so the first sighting wins.
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;
    // Keep the strongest kMaxNetworksToTry networks, strongest first.
    int insertAt = candidateCount;
    while (insertAt > 0 && candidates[insertAt - 1].rssi < rssi) insertAt--;
    if (insertAt >= kMaxNetworksToTry) continue;
    for (int c = std::min(candidateCount, kMaxNetworksToTry - 1); c > insertAt; c--) candidates[c] = candidates[c - 1];
    strlcpy(candidates[insertAt].ssid, ssid, sizeof(candidates[insertAt].ssid));
    candidates[insertAt].rssi = rssi;
    if (candidateCount < kMaxNetworksToTry) candidateCount++;
  }
  WiFi.scanDelete();

  if (candidateCount == 0) {
    LOG_INF(kLogTag, "No saved Wi-Fi network in range (%d visible)", found);
    return Result::NoNetwork;
  }

  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  for (int c = 0; c < candidateCount; c++) {
    const auto cred = WIFI_STORE.findCredential(candidates[c].ssid);
    if (!cred) continue;
    LOG_INF(kLogTag, "Connecting to %s (rssi=%ld)", cred->ssid.c_str(), static_cast<long>(candidates[c].rssi));
    if (cred->password.empty()) {
      WiFi.begin(cred->ssid.c_str());
    } else {
      WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
    }
    if (waitForConnection(job)) {
      LOG_INF(kLogTag, "Connected to %s", cred->ssid.c_str());
      return Result::Found;
    }
    if (job.cancel.load()) return Result::Cancelled;
    WiFi.disconnect(false, false, 1000);
  }
  LOG_INF(kLogTag, "Could not join any saved Wi-Fi network");
  return Result::NoNetwork;
}

// ---------------------------------------------------------------------------
// Response -> StarDict pseudo-entry
// ---------------------------------------------------------------------------

// ArduinoJson pulls bytes through read()/readBytes(); adapt HalFile so the
// response is parsed straight from SD instead of being buffered in RAM.
struct HalFileJsonReader {
  HalFile& file;
  int read() { return file.read(); }
  size_t readBytes(char* buffer, size_t length) {
    const int n = file.read(buffer, length);
    return n > 0 ? static_cast<size_t>(n) : 0;
  }
};

void writeEscaped(HalFile& out, const char* text) {
  if (!text) return;
  for (const char* p = text; *p; ++p) {
    switch (*p) {
      case '&':
        out.print("&amp;");
        break;
      case '<':
        out.print("&lt;");
        break;
      case '>':
        out.print("&gt;");
        break;
      default:
        out.write(static_cast<uint8_t>(*p));
        break;
    }
  }
}

bool writeIfo(const std::string& base) {
  HalFile ifo;
  if (!Storage.openFileForWrite(kLogTag, base + ".ifo", ifo)) return false;
  ifo.print(
      "StarDict's dict ifo file\n"
      "version=2.4.2\n"
      "bookname=Online Dictionary\n"
      "wordcount=1\n"
      "sametypesequence=h\n"
      "lang=en-en\n"
      "website=https://dictionaryapi.dev\n"
      "description=Free Dictionary API (Wiktionary, CC BY-SA)\n");
  ifo.close();
  return true;
}

Result writeEntry(const std::string& base, const std::string& word, DictLocation& out) {
  HalFile response;
  if (!Storage.openFileForRead(kLogTag, kResponsePath, response)) return Result::Failed;
  if (response.fileSize() > kMaxResponseBytes) {
    LOG_ERR(kLogTag, "Response too large to parse (%u bytes)", static_cast<unsigned>(response.fileSize()));
    response.close();
    return Result::Failed;
  }

  // The filter keeps only the fields we render, so the parsed document stays a
  // fraction of the raw response (phonetics audio URLs, synonyms, licenses and
  // source URLs are dropped while parsing).
  JsonDocument filter;
  filter[0]["word"] = true;
  filter[0]["phonetic"] = true;
  filter[0]["meanings"][0]["partOfSpeech"] = true;
  filter[0]["meanings"][0]["definitions"][0]["definition"] = true;
  filter[0]["meanings"][0]["definitions"][0]["example"] = true;

  JsonDocument doc;
  HalFileJsonReader reader{response};
  const DeserializationError err = deserializeJson(doc, reader, DeserializationOption::Filter(filter));
  response.close();
  if (err) {
    LOG_ERR(kLogTag, "Response parse failed: %s", err.c_str());
    return err == DeserializationError::NoMemory ? Result::Failed : Result::NotFound;
  }

  JsonArrayConst entries = doc.as<JsonArrayConst>();
  if (entries.isNull() || entries.size() == 0) return Result::NotFound;

  HalFile dict;
  if (!Storage.openFileForWrite(kLogTag, base + ".dict", dict)) return Result::Failed;

  const char* headword = entries[0]["word"] | word.c_str();
  dict.print("<b>");
  writeEscaped(dict, headword);
  dict.print("</b>");
  const char* phonetic = entries[0]["phonetic"].as<const char*>();
  if (phonetic && phonetic[0]) {
    dict.print(" ");
    writeEscaped(dict, phonetic);
  }

  int definitionsWritten = 0;
  int entryIndex = 0;
  for (JsonObjectConst entry : entries) {
    if (entryIndex++ >= kMaxEntries) break;
    int meaningIndex = 0;
    for (JsonObjectConst meaning : entry["meanings"].as<JsonArrayConst>()) {
      if (meaningIndex++ >= kMaxMeaningsPerEntry) break;
      const char* pos = meaning["partOfSpeech"] | "";
      dict.print("<p><i>");
      writeEscaped(dict, pos);
      dict.print("</i></p><ol>");
      int defIndex = 0;
      for (JsonObjectConst def : meaning["definitions"].as<JsonArrayConst>()) {
        if (defIndex++ >= kMaxDefinitionsPerMeaning) break;
        const char* text = def["definition"] | "";
        if (!text[0]) continue;
        dict.print("<li>");
        writeEscaped(dict, text);
        const char* example = def["example"] | "";
        if (example[0]) {
          dict.print("<br><i>\"");
          writeEscaped(dict, example);
          dict.print("\"</i>");
        }
        dict.print("</li>");
        definitionsWritten++;
      }
      dict.print("</ol>");
    }
  }
  dict.print("<p><small>Source: Free Dictionary API (Wiktionary)</small></p>");
  const size_t size = dict.fileSize();
  dict.close();

  if (definitionsWritten == 0) {
    Storage.remove((base + ".dict").c_str());
    return Result::NotFound;
  }
  if (!writeIfo(base)) {
    Storage.remove((base + ".dict").c_str());
    return Result::Failed;
  }

  out = DictLocation{};
  out.folderPath = base;
  out.headword = headword;
  out.offset = 0;
  out.size = static_cast<uint32_t>(size);
  out.found = true;
  return Result::Found;
}

// ---------------------------------------------------------------------------
// Network task
// ---------------------------------------------------------------------------

Result runOnline(Job& job) {
  const std::string word = normalizeWord(*job.word);
  const std::string base = cacheBasePath(word);

  const uint32_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < kMinFreeInternalHeap) {
    LOG_ERR(kLogTag, "Not enough free heap for Wi-Fi lookup (free=%u maxAlloc=%u)", static_cast<unsigned>(freeHeap),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
    return Result::Failed;
  }

  bool startedWifi = false;
  const Result net = connectToSavedNetwork(job, startedWifi);
  if (net != Result::Found) {
    if (startedWifi) wifiOff();
    return net;
  }

  if (!Storage.ensureDirectoryExists(kCacheDir)) {
    LOG_ERR(kLogTag, "Could not create %s", kCacheDir);
    if (startedWifi) wifiOff();
    return Result::Failed;
  }
  pruneCacheIfLarge();

  const std::string url = std::string(kApiBase) + urlEncode(word);
  LOG_INF(kLogTag, "GET %s", url.c_str());
  HttpDownloader::DownloadOptions options;
  options.shouldCancel = [&job]() { return job.cancel.load(); };
  const auto dl = HttpDownloader::downloadToFile(url, kResponsePath, nullptr, nullptr, "", "", std::move(options));

  // Radio off as soon as the bytes are on SD; parsing does not need it.
  if (startedWifi) wifiOff();

  Result result;
  switch (dl) {
    case HttpDownloader::OK:
      result = writeEntry(base, *job.word, *job.out);
      break;
    case HttpDownloader::ABORTED:
      result = Result::Cancelled;
      break;
    case HttpDownloader::HTTP_ERROR:
      // The API answers 404 for unknown words; HttpDownloader reports any
      // non-200 the same way, so treat it as "no online entry".
      result = Result::NotFound;
      break;
    default:
      result = Result::Failed;
      break;
  }
  Storage.remove(kResponsePath);
  return result;
}

void onlineTask(void* param) {
  auto* job = static_cast<Job*>(param);
  job->result = runOnline(*job);
  job->done.store(true, std::memory_order_release);
  vTaskDelete(nullptr);
}

}  // namespace

bool isEnabled() { return SETTINGS.onlineDictionaryFallback != 0; }

Result lookup(const std::string& word, const std::string& activeDictPath, DictLocation& out,
              bool (*shouldCancel)(void* ctx), void* ctx) {
  if (!isEnabled() || word.empty()) return Result::Skipped;

  // The Free Dictionary API serves English only. Do not spend a Wi-Fi session
  // on a miss from, say, a Greek dictionary.
  if (!activeDictPath.empty()) {
    const DictInfo info = Dictionary::readInfo(activeDictPath.c_str());
    if (info.lang[0] != '\0' && strncmp(info.lang, "en", 2) != 0) {
      LOG_DBG(kLogTag, "Skipping online lookup for non-English dictionary (%s)", info.lang);
      return Result::Skipped;
    }
  }

  // Previously fetched words are served from SD without touching the radio.
  const std::string base = cacheBasePath(normalizeWord(word));
  if (isCached(base, out, word)) {
    LOG_DBG(kLogTag, "Cache hit for %s", word.c_str());
    return Result::Found;
  }

  Job job;
  job.word = &word;
  job.out = &out;
  TaskHandle_t handle = nullptr;
  if (xTaskCreate(onlineTask, "DictOnline", kTaskStackBytes, &job, 1, &handle) != pdPASS) {
    LOG_ERR(kLogTag, "Could not start online lookup task");
    return Result::Failed;
  }

  // `job` lives on this stack frame, so always wait for the task to finish,
  // forwarding a Back press as a cancellation request.
  while (!job.done.load(std::memory_order_acquire)) {
    if (!job.cancel.load() && shouldCancel && shouldCancel(ctx)) job.cancel.store(true);
    delay(kPollMs);
  }
  return job.result;
}

}  // namespace OnlineDictionary

#endif  // CROSSINK_APP_ONLINE_DICTIONARY
