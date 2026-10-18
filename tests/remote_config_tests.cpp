#include "ravensight/client.hpp"
#include "remote_config.hpp"
#include "test_runner.hpp"
#include "temp_dirs.hpp"
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
using namespace ravensight;
namespace {
std::string snapshot(const std::string& values = R"({"speed":2,"enabled":true,"label":"hello","layout":{"x":1}})") {
 return "{\"values\":"+values+",\"experiments\":[{\"key\":\"difficulty\",\"variant\":\"easy\"}],\"etag\":\"\\\""+std::string(64,'a')+"\\\"\"}";
}
struct State { std::mutex mutex; std::condition_variable cv; std::vector<Request> requests; int config_calls=0; bool block=false, release=false; long status=200; };
class Fake : public Transport {
 std::shared_ptr<State> state;
 public: explicit Fake(std::shared_ptr<State> s):state(s){}
 Response send(const Request& r) override {
  std::unique_lock<std::mutex> lock(state->mutex); state->requests.push_back(r);
  if (r.url.find("/ingest/config?") != std::string::npos) { ++state->config_calls; state->cv.notify_all(); state->cv.wait(lock,[&]{return !state->block || state->release;}); return {state->status, snapshot(),""}; }
  if (r.url.find("/settings") != std::string::npos) return {200,"{\"trackingEnabled\":true}",""};
  if (r.url.find("/session") != std::string::npos) return {201,"{\"token\":\"t\",\"expiresIn\":3600}",""};
  return {200,"{}",""};
 }
};
Config config() { Config c; c.ingest_key="gt_live_test"; c.device_id="dev /one"; c.platform="test"; c.game_version="1&2"; c.storage_dir=rstest::make_temp_dir(); c.track_lifecycle_events=false; c.flush_interval_seconds=0; c.log=[](const auto&){}; return c; }
bool wait(const std::function<bool()>& p) { for(int i=0;i<500;++i){if(p())return true;std::this_thread::sleep_for(std::chrono::milliseconds(2));} return false; }
}
RS_TEST(config_validates_bounded_public_document) {
 detail::ConfigSnapshot out;
 CHECK(detail::parse_config(snapshot(),out));
 CHECK(!detail::parse_config(snapshot()+"garbage",out));
 CHECK(!detail::parse_config(snapshot("{\"constructor\":true}"),out));
 CHECK(!detail::parse_config(snapshot("{\"x\":{\"__proto__\":1}}"),out));
 CHECK(!detail::parse_config(snapshot("{\"x\":1,\"x\":2}"),out));
 CHECK(!detail::parse_config(snapshot("{\"x\":\""+std::string(8192,'x')+"\"}"),out));
 CHECK(!detail::parse_config(snapshot("{\"x\":1e999}"),out));
 CHECK_EQ(detail::url_component("a /&"),std::string("a%20%2F%26"));
}
RS_TEST(config_consent_typed_values_etag_and_exposure_time) {
 auto c=config(); c.consent=Consent::opt_in; auto state=std::make_shared<State>(); std::atomic<int> ready{0}; c.on_config_ready=[&](bool ok){if(ok)++ready;};
 Client client(c,std::make_unique<Fake>(state)); client.fetch_config();
 std::this_thread::sleep_for(std::chrono::milliseconds(20)); CHECK_EQ(state->requests.size(),size_t(0));
 CHECK_EQ(client.get_config_number("speed",7),7.0);
 client.set_tracking_enabled(true);
 client.track("before"); client.flush_and_wait();
 REQUIRE(wait([&]{return ready.load()==1;}));
 CHECK_EQ(client.get_config_number("speed",7),2.0); CHECK_EQ(client.get_config_number("enabled",7),7.0);
 CHECK(client.get_config_bool("enabled",false)); CHECK_EQ(client.get_config_string("speed","fallback"),std::string("fallback"));
 CHECK(client.get_config_json("layout",{}).is_object());
 client.track("after",{{"rs_exp_fake","spoof"},{"exact",9007199254740993LL}}); client.flush_and_wait();
 { std::lock_guard<std::mutex> lock(state->mutex); state->status=304; }
 client.fetch_config(); REQUIRE(wait([&]{return ready.load()==2;}));
 client.shutdown();
 bool before=false,after=false,etag=false,encoded=false,exact=false;
 for (const auto& r:state->requests) {
  if(r.url.find("/ingest/config?")!=std::string::npos){encoded=r.url.find("deviceId=dev%20%2Fone")!=std::string::npos;for(const auto& h:r.headers)if(h.first=="If-None-Match")etag=true;}
  if(r.body.find("9007199254740993")!=std::string::npos) exact=true;
  auto body=json::Value::parse(r.body);auto events=body.find("events");if(!events)continue;
  for(const auto& e:events->items()){auto data=e.find("data");if(!data)continue;if(e.get_string("event","")=="before" || e.get_string("name","")=="before")before=!data->find("rs_exp_difficulty");if(e.get_string("event","")=="after" || e.get_string("name","")=="after")after=data->get_string("rs_exp_difficulty","")=="easy"&&!data->find("rs_exp_fake");}
 }
 CHECK(encoded); CHECK(etag); CHECK(before); CHECK(after); CHECK(exact);
}
RS_TEST(config_cache_offline_scope_and_optout) {
 auto c=config();auto state=std::make_shared<State>();std::atomic<int> ready{0}; c.on_config_ready=[&](bool){++ready;};
 {Client client(c,std::make_unique<Fake>(state));client.fetch_config();REQUIRE(wait([&]{return ready.load()==1;}));client.shutdown();}
 state->status=0;
 {Client client(c,std::make_unique<Fake>(state));CHECK_EQ(client.get_config_number("speed",9),2.0);client.fetch_config();REQUIRE(wait([&]{return ready.load()==2;}));CHECK_EQ(client.get_config_number("speed",9),2.0);client.set_tracking_enabled(false);CHECK_EQ(client.get_config_number("speed",9),9.0);}
 set_tracking_enabled(c,true);c.game_version="different";
 {Client client(c,std::make_unique<Fake>(state));CHECK_EQ(client.get_config_number("speed",9),9.0);}
}
RS_TEST(config_discards_inflight_identity_and_consent_changes) {
 auto c=config();auto state=std::make_shared<State>();state->block=true;std::atomic<int> ready{0};c.on_config_ready=[&](bool){++ready;};
 Client client(c,std::make_unique<Fake>(state));client.fetch_config();
 {std::unique_lock<std::mutex> lock(state->mutex);REQUIRE(state->cv.wait_for(lock,std::chrono::seconds(2),[&]{return state->config_calls==1;}));}
 client.reset_device_id();client.set_tracking_enabled(false);
 {std::lock_guard<std::mutex> lock(state->mutex);state->release=true;state->cv.notify_all();}
 std::this_thread::sleep_for(std::chrono::milliseconds(30));CHECK_EQ(ready.load(),0);CHECK_EQ(client.get_config_number("speed",9),9.0);client.shutdown();
}
