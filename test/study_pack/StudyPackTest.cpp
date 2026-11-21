#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "StudyPackParse.h"
#include "StudySrs.h"
#include "StudyStats.h"

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

TEST(StudyStats, ParsesTheReadersOwnLogLines) {
  char buf[128];
  LogEvent e;
  const size_t cardLen = formatCardEvent(buf, sizeof(buf), "ccna", "f12", 4, kNow);
  ASSERT_TRUE(parseLogEvent(buf, cardLen - 1, e));  // without the newline, as the line reader passes it
  EXPECT_EQ(e.kind, LogEvent::Kind::Card);
  EXPECT_STREQ(e.track, "ccna");
  EXPECT_TRUE(e.ok);
  EXPECT_EQ(e.t, kNow);

  const size_t missLen = formatCardEvent(buf, sizeof(buf), "ccna", "f12", kQualityMissed, 0);
  ASSERT_TRUE(parseLogEvent(buf, missLen, e));
  EXPECT_FALSE(e.ok);
  EXPECT_EQ(e.t, 0u);

  const size_t qLen = formatQuestionEvent(buf, sizeof(buf), "sec-plus", "q3", true, kNow);
  ASSERT_TRUE(parseLogEvent(buf, qLen, e));
  EXPECT_EQ(e.kind, LogEvent::Kind::Question);
  EXPECT_STREQ(e.track, "sec-plus");
  EXPECT_TRUE(e.ok);
}

TEST(StudyStats, SkipsHeaderAndUnknownLines) {
  char buf[128];
  LogEvent e;
  const size_t headerLen = formatLogHeader(buf, sizeof(buf), "st0001abcd");
  EXPECT_FALSE(parseLogEvent(buf, headerLen, e));
  const char* game = "{\"k\":\"g\",\"tr\":\"ccna\",\"game\":\"seq\",\"id\":\"s1\",\"ok\":1}";
  EXPECT_FALSE(parseLogEvent(game, len(game), e));
  const char* noTrack = "{\"k\":\"q\",\"id\":\"q1\",\"ok\":1}";
  EXPECT_FALSE(parseLogEvent(noTrack, len(noTrack), e));
  const char* badQuality = "{\"k\":\"c\",\"tr\":\"ccna\",\"id\":\"f1\",\"q\":9}";
  EXPECT_FALSE(parseLogEvent(badQuality, len(badQuality), e));
  const char* cut = "{\"k\":\"q\",\"tr\":\"cc";
  EXPECT_FALSE(parseLogEvent(cut, len(cut), e));
}

namespace {
LogEvent event(const LogEvent::Kind kind, const bool ok, const uint32_t t) {
  LogEvent e;
  e.kind = kind;
  e.ok = ok;
  e.t = t;
  strcpy(e.track, "ccna");
  return e;
}
}  // namespace

TEST(StudyStats, CountsTodayAndStreakInLocalTime) {
  // kNow is 2025-11-11 01:36 UTC. At UTC-5 that is still Nov 10 locally.
  const int32_t offset = -5 * 3600;
  StudyTally tally(kNow, offset);
  tally.add(event(LogEvent::Kind::Card, true, kNow - 60));                   // local today
  tally.add(event(LogEvent::Kind::Question, false, kNow - 3 * 3600));        // Nov 10 17:36 local, today
  tally.add(event(LogEvent::Kind::Question, true, kNow - kSecondsPerDay));   // yesterday
  tally.add(event(LogEvent::Kind::Card, false, kNow - 2 * kSecondsPerDay));  // two days ago
  tally.add(event(LogEvent::Kind::Card, true, kNow - 5 * kSecondsPerDay));   // gap before this one
  tally.add(event(LogEvent::Kind::Question, true, 0));                       // no clock: all-time only
  const StudyTotals t = tally.totals();
  EXPECT_EQ(t.cardReviews, 3u);
  EXPECT_EQ(t.cardsGotIt, 2u);
  EXPECT_EQ(t.questions, 3u);
  EXPECT_EQ(t.questionsCorrect, 2u);
  EXPECT_EQ(t.todayCards, 1u);
  EXPECT_EQ(t.todayQuestions, 1u);
  EXPECT_EQ(t.streakDays, 3u);
}

TEST(StudyStats, StreakSurvivesUntilTheEndOfTheNextDay) {
  StudyTally tally(kNow, 0);
  tally.add(event(LogEvent::Kind::Card, true, kNow - kSecondsPerDay));
  tally.add(event(LogEvent::Kind::Card, true, kNow - 2 * kSecondsPerDay));
  EXPECT_EQ(tally.totals().streakDays, 2u);
  EXPECT_EQ(tally.totals().todayCards, 0u);

  StudyTally lapsed(kNow, 0);
  lapsed.add(event(LogEvent::Kind::Card, true, kNow - 2 * kSecondsPerDay));
  EXPECT_EQ(lapsed.totals().streakDays, 0u);
}

TEST(StudyStats, NoClockKeepsAllTimeCountsOnly) {
  StudyTally tally(0, 0);
  EXPECT_FALSE(tally.clockSet());
  tally.add(event(LogEvent::Kind::Card, true, kNow));
  const StudyTotals t = tally.totals();
  EXPECT_EQ(t.cardReviews, 1u);
  EXPECT_EQ(t.todayCards, 0u);
  EXPECT_EQ(t.streakDays, 0u);
}

TEST(StudyStats, ScheduleCountsSeenAndDueCards) {
  StudyTally tally(kNow, 0);
  TrackStudyStats track;
  tally.addSchedule(SrsEntry{}, track);                                                     // never rated
  tally.addSchedule(nextSrs(SrsEntry{}, kQualityMissed, kNow), track);                      // due now
  tally.addSchedule(nextSrs(SrsEntry{}, kQualityGotIt, kNow), track);                       // due tomorrow
  tally.addSchedule(nextSrs(SrsEntry{}, kQualityGotIt, kNow - 2 * kSecondsPerDay), track);  // overdue
  EXPECT_EQ(track.cardsSeen, 3u);
  EXPECT_EQ(track.cardsDue, 2u);
  EXPECT_EQ(percentOf(2, 3), 66u);
  EXPECT_EQ(percentOf(1, 0), 0u);
}
