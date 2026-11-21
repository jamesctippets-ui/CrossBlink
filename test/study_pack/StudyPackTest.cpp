#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "StudyPackParse.h"
#include "StudySrs.h"

using namespace study;

namespace {
constexpr uint32_t kNow = 1762825000;

size_t len(const char* s) { return strlen(s); }
}  // namespace

TEST(StudySrs, GotItFollowsTheAppsIntervals) {
  SrsEntry e;
  e = nextSrs(e, kQualityGotIt, kNow);
  EXPECT_EQ(e.reps, 1);
  EXPECT_EQ(e.intervalDays, 1);
  EXPECT_EQ(e.ease100, 250);  // 2.5 + (0.1 - 1 * (0.08 + 0.02)) = 2.5
  EXPECT_EQ(e.due, kNow + kSecondsPerDay);
  EXPECT_EQ(e.last, 4);
  e = nextSrs(e, kQualityGotIt, kNow);
  EXPECT_EQ(e.intervalDays, 6);
  e = nextSrs(e, kQualityGotIt, kNow);
  EXPECT_EQ(e.intervalDays, 15);  // round(6 * 2.5)
  EXPECT_EQ(e.due, kNow + 15 * kSecondsPerDay);
}

TEST(StudySrs, MissResetsAndLowersEase) {
  SrsEntry e;
  e = nextSrs(e, kQualityGotIt, kNow);
  e = nextSrs(e, kQualityGotIt, kNow);
  e = nextSrs(e, kQualityMissed, kNow);
  EXPECT_EQ(e.reps, 0);
  EXPECT_EQ(e.intervalDays, 1);
  EXPECT_EQ(e.ease100, 230);
  EXPECT_EQ(e.due, kNow);
  EXPECT_EQ(e.last, 1);
  for (int i = 0; i < 10; ++i) e = nextSrs(e, kQualityMissed, kNow);
  EXPECT_EQ(e.ease100, 130);  // floor at 1.3
}

TEST(StudySrs, UnixTimeFromDate) {
  EXPECT_EQ(daysFromCivil(1970, 1, 1), 0);
  EXPECT_EQ(unixSecondsUtc(2025, 11, 11, 1, 36), 1762824960u);
  EXPECT_EQ(unixSecondsUtc(2000, 1, 1, 0, 0), 0u);  // an unset clock
  EXPECT_EQ(unixSecondsUtc(2026, 13, 1, 0, 0), 0u);
}

TEST(StudyPackParse, Manifest) {
  const char* json =
      R"({"pack":"cert-study-eink","v":1,"built":"2026-11-21","tracks":[
        {"key":"ccna","label":"CCNA","subtitle":"Cisco","categories":["Fundamentals","Access"],"cards":233,
         "questions":201,"exam":{"length":60,"minutes":75,"passPct":82}},
        {"key":"../evil","label":"Bad"},
        {"key":"itil","label":"ITIL","categories":[],"cards":100,"questions":123}]})";
  std::vector<TrackInfo> tracks;
  ASSERT_TRUE(parseManifest(json, len(json), tracks));
  ASSERT_EQ(tracks.size(), 2u);  // the unsafe key is dropped
  EXPECT_EQ(tracks[0].key, "ccna");
  EXPECT_EQ(tracks[0].categories.size(), 2u);
  EXPECT_EQ(tracks[0].cards, 233);
  EXPECT_EQ(tracks[0].exam.length, 60);
  EXPECT_EQ(tracks[0].exam.minutes, 75);
  EXPECT_EQ(tracks[0].exam.passPct, 82);
  EXPECT_EQ(tracks[1].exam.length, 0);  // older packs have no exam settings
}

TEST(StudyPackParse, RejectsOtherJson) {
  const char* json = R"({"pack":"something-else","tracks":[]})";
  std::vector<TrackInfo> tracks;
  EXPECT_FALSE(parseManifest(json, len(json), tracks));
  EXPECT_FALSE(parseManifest("{nope", 5, tracks));
}

TEST(StudyPackParse, Card) {
  const char* line =
      R"({"id":"f7","c":1,"term":"Trunk port","prompt":"A _____ carries VLANs","def":"A trunk port carries VLANs","eg":"Uplinks"})";
  Card card;
  ASSERT_TRUE(parseCardLine(line, len(line), card));
  EXPECT_EQ(card.id, "f7");
  EXPECT_EQ(card.category, 1);
  EXPECT_EQ(card.front(), "A _____ carries VLANs");
  EXPECT_EQ(card.example, "Uplinks");

  const char* noPrompt = R"({"id":"f8","c":0,"term":"VLAN","def":"A virtual LAN"})";
  ASSERT_TRUE(parseCardLine(noPrompt, len(noPrompt), card));
  EXPECT_EQ(card.front(), "A virtual LAN");
  EXPECT_TRUE(card.example.empty());

  const char* badId = R"({"id":"f\"8","term":"x","def":"y"})";
  EXPECT_FALSE(parseCardLine(badId, len(badId), card));
}

TEST(StudyPackParse, Questions) {
  const char* mc = R"({"id":"q1","c":0,"type":"mc","q":"Which?","opts":["A","B","C"],"ans":[2],"why":"C."})";
  Question q;
  ASSERT_TRUE(parseQuestionLine(mc, len(mc), q));
  EXPECT_EQ(q.type, QuestionType::Single);
  EXPECT_EQ(q.optionCount, 3);
  EXPECT_EQ(q.answerMask, 0b100);
  EXPECT_TRUE(isAnswerCorrect(q, 0b100));
  EXPECT_FALSE(isAnswerCorrect(q, 0b110));

  const char* ms = R"({"id":"q2","c":1,"type":"ms","q":"Pick two","opts":["A","B","C","D"],"ans":[0,3],"why":""})";
  ASSERT_TRUE(parseQuestionLine(ms, len(ms), q));
  EXPECT_EQ(q.type, QuestionType::Multi);
  EXPECT_EQ(q.answerMask, 0b1001);

  const char* tf = R"({"id":"q3","c":2,"type":"tf","q":"True?","opts":["True","False"],"ans":[1],"why":"No."})";
  ASSERT_TRUE(parseQuestionLine(tf, len(tf), q));
  EXPECT_EQ(q.type, QuestionType::TrueFalse);

  const char* outOfRange = R"({"id":"q4","type":"mc","q":"?","opts":["A","B"],"ans":[5]})";
  EXPECT_FALSE(parseQuestionLine(outOfRange, len(outOfRange), q));
  const char* unknownType = R"({"id":"q5","type":"essay","q":"?","opts":["A","B"],"ans":[0]})";
  EXPECT_FALSE(parseQuestionLine(unknownType, len(unknownType), q));
}

TEST(StudyPackParse, PeekCategory) {
  const char* line = R"({"id":"f12","c":14,"term":"x"})";
  EXPECT_EQ(peekCategory(line, len(line)), 14);
  EXPECT_EQ(peekCategory(line, 8), -1);  // cut before the key
  const char* none = R"({"id":"f12","term":"c"})";
  EXPECT_EQ(peekCategory(none, len(none)), -1);
}

TEST(StudyPackParse, PeekId) {
  char id[12];
  const char* line = R"({"id":"f12","c":14,"term":"x"})";
  EXPECT_EQ(peekId(line, len(line), id, sizeof(id)), 3u);
  EXPECT_STREQ(id, "f12");
  EXPECT_EQ(peekId(line, 9, id, sizeof(id)), 0u);  // closing quote cut off
  EXPECT_EQ(peekId(line, len(line), id, 3), 0u);   // does not fit
  const char* none = R"({"c":1,"term":"id"})";
  EXPECT_EQ(peekId(none, len(none), id, sizeof(id)), 0u);
}

TEST(StudyPackParse, LogLinesMatchTheContract) {
  char buf[128];
  ASSERT_GT(formatLogHeader(buf, sizeof(buf), "st0001abcd"), 0u);
  EXPECT_STREQ(buf, "{\"log\":\"cert-study-eink\",\"v\":1,\"id\":\"st0001abcd\"}\n");
  ASSERT_GT(formatCardEvent(buf, sizeof(buf), "ccna", "f12", 4, kNow), 0u);
  EXPECT_STREQ(buf, "{\"k\":\"c\",\"tr\":\"ccna\",\"id\":\"f12\",\"q\":4,\"t\":1762825000}\n");
  ASSERT_GT(formatQuestionEvent(buf, sizeof(buf), "ccna", "q3", false, 0), 0u);
  EXPECT_STREQ(buf, "{\"k\":\"q\",\"tr\":\"ccna\",\"id\":\"q3\",\"ok\":0}\n");
  EXPECT_EQ(formatCardEvent(buf, sizeof(buf), "ccna", "f12", 9, kNow), 0u);
  EXPECT_EQ(formatCardEvent(buf, 10, "ccna", "f12", 4, kNow), 0u);
  EXPECT_EQ(formatQuestionEvent(buf, sizeof(buf), "Bad Key", "q3", true, 0), 0u);
}
