#include "doctest.h"
#include "logic/civil.h"
#include "logic/runlog.h"

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
