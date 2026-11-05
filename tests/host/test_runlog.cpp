#include "doctest.h"
#include "civil.h"
#include "runlog.h"

using namespace irrigation;

TEST_CASE("runlog: record_json exact") {
  RunRecord r{1, 1, 0, 2, 1793520000, 600, 598, 61.2f, Result::COMPLETED};
  CHECK(record_json(r) ==
        "{\"seq\":1,\"inst\":1,\"p\":0,\"z\":2,\"ts\":1793520000,\"plan\":600,\"act\":598,\"l\":61.2,\"r\":\"completed\"}");
}

TEST_CASE("runlog: plan_json exact") {
  PlannedRun p{0, days_from_civil(2026, 11, 3), 360, 480, 80, 1};
  CHECK(plan_json({p}) == "[{\"p\":0,\"date\":\"2026-11-03\",\"start\":\"06:00\",\"s\":480,\"pct\":80,\"zones\":1}]");
}

TEST_CASE("runlog: plan_json caps entries and flags truncation") {
  PlannedRun p{0, days_from_civil(2026, 11, 3), 360, 480, 80, 1};
  std::vector<PlannedRun> v(300, p);
  bool trunc = false;
  std::string s = plan_json(v, 256, &trunc);
  CHECK(trunc);
  size_t n = 0;
  for (size_t pos = 0; (pos = s.find("{\"p\"", pos)) != std::string::npos; pos++) n++;
  CHECK(n == 256);
  trunc = true;
  plan_json(std::vector<PlannedRun>(256, p), 256, &trunc);
  CHECK_FALSE(trunc);
}

TEST_CASE("runlog: truncate_utf8 never splits a code point") {
  CHECK(truncate_utf8("abcdef", 3) == "abc");
  CHECK(truncate_utf8("abc", 24) == "abc");
  // U+00E9 is 2 bytes (C3 A9); cutting after the lead byte must drop the whole character.
  CHECK(truncate_utf8("ab\xC3\xA9", 3) == "ab");
  CHECK(truncate_utf8("ab\xC3\xA9", 4) == "ab\xC3\xA9");
  // U+20AC is 3 bytes (E2 82 AC).
  CHECK(truncate_utf8("a\xE2\x82\xAC", 3) == "a");
  CHECK(truncate_utf8("a\xE2\x82\xAC", 2) == "a");
}

TEST_CASE("runlog: ring keeps 48") {
  RunLog log;
  for (int i = 0; i < 50; i++) log.add(RunRecord{});
  auto v = log.since(0);
  CHECK(v.size() == 48);
  CHECK(v.front().seq == 3);
  CHECK(log.last_seq() == 50);
  auto w = log.since(49);
  REQUIRE(w.size() == 1);
  CHECK(w[0].seq == 50);
}

TEST_CASE("runlog: empty array") { CHECK(records_json({}) == "[]"); }

TEST_CASE("runlog: result names") {
  CHECK(std::string(result_name(Result::SKIPPED_WEATHER)) == "skipped_weather");
  CHECK(std::string(result_name(Result::FLOW_LOW)) == "flow_low");
}
