// This suite is an integration test. It does not check for a specific feature,
// but makes sure the system behaves correctly in different use cases. The
// system always consists of at least three nodes. Messages are not checked
// individually. Rather, the system runs to a predetermined point before
// checking for an expected outcome.
#define SUITE integration

#include "test.hpp"
#include <caf/test/io_dsl.hpp>

#include "broker/broker.hh"

using namespace broker;

using caf::unit_t;

using caf::io::accept_handle;
using caf::io::connection_handle;

// Useful type aliases.
using data_vector = std::vector<endpoint::value_type>;

namespace {

configuration make_config() {
  configuration cfg(true);
  cfg.parse(caf::test::engine::argc(), caf::test::engine::argv());
  cfg.middleman_network_backend = caf::atom("testing");
  cfg.scheduler_policy = caf::atom("testing");
  cfg.logger_inline_output = true;
  return cfg;
}

struct peer_fixture;

// Holds state shared by all peers. There exists exactly one global fixture.
struct global_fixture {
  // Maps host names to peers.
  using peers_map = std::map<std::string, peer_fixture*>;
  peers_map peers;

  // Makes sure all handles are distinct.
  uint64_t next_handle_id = 1;

  // Tries progressesing actors messages or network traffic.
  bool try_exec();

  // Progresses actors messages and network traffic as much as possible.
  void exec_loop() {
    while (try_exec())
      ; // rinse and repeat
  }
};

// Holds state for individual peers. We use one fixture per simulated peer.
struct peer_fixture {
  // Pointer to the global state.
  global_fixture* parent;

  // Identifies this fixture in the parent's `peers` map.
  std::string name;

  // Each peer is an endpoint.
  endpoint ep;

  // Convenient access to `ep.system()`.
  caf::actor_system& sys;

  // Convenient access to `sys.scheduler()` with proper type.
  caf::scheduler::test_coordinator& sched;

  // Convenienct access to `sys.middleman()`.
  caf::io::middleman& mm;

  // Convenient access to `mm.backend()` with proper type.
  caf::io::network::test_multiplexer& mpx;

  // Lists all open connections on this peer.
  std::vector<connection_handle> connections;

  // Lists all open "ports" on this peer.
  std::vector<accept_handle> acceptors;

  // Stores all received items for subscribed topics.
  data_vector data; 

  // Stores all actors we spawn for subscribing or publishing.
  std::vector<caf::actor> workers;

  // Initializes this peer and registers it at parent.
  peer_fixture(global_fixture* parent_ptr, std::string peer_name)
    : parent(parent_ptr),
      name(std::move(peer_name)),
      ep(make_config()),
      sys(ep.system()),
      sched(dynamic_cast<caf::scheduler::test_coordinator&>(sys.scheduler())),
      mm(sys.middleman()),
      mpx(dynamic_cast<caf::io::network::test_multiplexer&>(mm.backend())) {
    // Register at parent.
    parent->peers.emplace(name, this);
    // Run initialization code
    exec_loop();
  }

  ~peer_fixture() {
    MESSAGE("shut down " << name);
    for (auto& w : workers)
      caf::anon_send_exit(w, caf::exit_reason::user_shutdown);
    exec_loop();
    sched.inline_all_enqueues();
  }

  // Returns the next unused connection handle.
  connection_handle make_connection_handle() {
    auto result = connection_handle::from_int(parent->next_handle_id++);
    connections.emplace_back(result);
    return result;
  }

  // Returns the next unused accept handle.
  accept_handle make_accept_handle() {
    auto result = accept_handle::from_int(parent->next_handle_id++);
    acceptors.emplace_back(result);
    return result;
  }

  // Subscribes to a topic, storing all incoming tuples in `data`.
  void subscribe_to(topic t) {
    workers.emplace_back(ep.subscribe_nosync(
      {t},
      [](unit_t&) {
        // nop
      },
      [=](unit_t&, endpoint::value_type x) {
        data.emplace_back(std::move(x));
      },
      [](unit_t&) {
        // nop
      }
    ));
    parent->exec_loop();
  }

  // Publishes all `(t, xs)...` tuples.
  template <class... Ts>
  void publish(topic t, Ts... xs) {
    using buf_t = std::deque<endpoint::value_type>;
    auto buf = std::make_shared<buf_t>(buf_t{std::make_pair(t, std::move(xs))...});
    workers.emplace_back(ep.publish_all_nosync(
      [](unit_t&) {
        // nop
      },
      [=](unit_t&, caf::downstream<endpoint::value_type>& out, size_t num) {
        auto n = std::min(num, buf->size());
        CAF_MESSAGE("push" << n << "values downstream");
        for (size_t i = 0u; i < n; ++i)
          out.push(buf->at(i));
        buf->erase(buf->begin(), buf->begin() + static_cast<ptrdiff_t>(n));
      },
      [=](const unit_t&) {
        return buf->empty();
      },
      [](expected<void>) {
        // nop
      }
    ));
    parent->exec_loop();
  }

  // Tries to advance actor messages or network data on this peer.
  bool try_exec() {
    return sched.try_run_once() || mpx.try_read_data()
           || mpx.try_exec_runnable() || mpx.try_accept_connection();
  }

  // Advances actor messages and network data on this peer as much as possible.
  void exec_loop() {
    while (try_exec())
      ; // rinse and repeat
  }

  void loop_after_next_enqueue() {
    sched.after_next_enqueue([=] { parent->exec_loop();  });
  }
};

bool global_fixture::try_exec() {
  return std::any_of(peers.begin(), peers.end(),
                     [](const peers_map::value_type& kvp) {
                       return kvp.second->try_exec();
                     });
}

// A fixture for simple setups consisting of three nodes.
struct triangle_fixture : global_fixture {
  peer_fixture mercury;
  peer_fixture venus;
  peer_fixture earth;

  triangle_fixture()
    : mercury(this, "mercury"),
      venus(this, "venus"),
      earth(this, "earth") {
    // nop
  }

  void connect_peers() {
    MESSAGE("prepare connections");
    auto server_handle = mercury.make_accept_handle();
    mercury.mpx.prepare_connection(server_handle,
                                   mercury.make_connection_handle(), venus.mpx,
                                   "mercury", 4040,
                                   venus.make_connection_handle());
    mercury.mpx.prepare_connection(server_handle,
                                   mercury.make_connection_handle(), earth.mpx,
                                   "mercury", 4040,
                                   earth.make_connection_handle());
    MESSAGE("start listening on mercury:4040");
    // We need to connect venus and earth while mercury is blocked on ep.listen()
    // in order to avoid a "deadlock" in `ep.listen()`.
    mercury.sched.after_next_enqueue([&] {
      exec_loop();
      MESSAGE("peer venus to mercury:4040");
      venus.loop_after_next_enqueue();
      venus.ep.peer("mercury", 4040);
      MESSAGE("peer earth to mercury:4040");
      earth.loop_after_next_enqueue();
      earth.ep.peer("mercury", 4040);
    });
    //mercury.sched.inline_next_enqueue();
    mercury.ep.listen("", 4040);
  }
};

} // namespace <anonymous>

CAF_TEST_FIXTURE_SCOPE(triangle_use_cases, triangle_fixture)

// -- prefix-based data forwarding in Broker -----------------------------------

// Checks whether topic subscriptions are prefix-based using the asynchronous
// `endpoint::subscribe_nosync` API to subscribe to topics.
CAF_TEST(topic_prefix_matching_async_subscribe) {
  connect_peers();
  MESSAGE("assume two peers for mercury");
  mercury.loop_after_next_enqueue();
  auto mercury_peers = mercury.ep.peers();
  CAF_REQUIRE_EQUAL(mercury_peers.size(), 2);
  CAF_CHECK_EQUAL(mercury_peers.front().status, peer_status::peered);
  CAF_CHECK_EQUAL(mercury_peers.back().status, peer_status::peered);
  MESSAGE("assume one peer for venus");
  venus.loop_after_next_enqueue();
  auto venus_peers = venus.ep.peers();
  CAF_REQUIRE_EQUAL(venus_peers.size(), 1);
  CAF_CHECK_EQUAL(venus_peers.front().status, peer_status::peered);
  MESSAGE("assume one peer for earth");
  earth.loop_after_next_enqueue();
  auto earth_peers = earth.ep.peers();
  CAF_REQUIRE_EQUAL(earth_peers.size(), 1);
  CAF_CHECK_EQUAL(earth_peers.front().status, peer_status::peered);
  MESSAGE("subscribe to 'bro/events' on venus");
  venus.subscribe_to("bro/events");
  MESSAGE("subscribe to 'bro/events/failures' on earth");
  earth.subscribe_to("bro/events/failures");
  MESSAGE("verify subscriptions");
  auto filter = [](std::initializer_list<topic> xs) -> std::vector<topic> {
    return xs;
  };
  mercury.loop_after_next_enqueue();
  CAF_CHECK_EQUAL(mercury.ep.peer_subscriptions(),
                  filter({"bro/events", "bro/events/failures"}));
  venus.loop_after_next_enqueue();
  CAF_CHECK_EQUAL(venus.ep.peer_subscriptions(), filter({}));
  earth.loop_after_next_enqueue();
  CAF_CHECK_EQUAL(earth.ep.peer_subscriptions(), filter({}));
  MESSAGE("publish to 'bro/events/(logging|failures)' on mercury");
  mercury.publish("bro/events/failures", "oops", "sorry!");
  mercury.publish("bro/events/logging", 123, 456);
  MESSAGE("verify published data");
  auto data = [](std::initializer_list<endpoint::value_type> xs) -> data_vector {
    return xs;
  };
  CAF_CHECK_EQUAL(mercury.data, data({}));
  CAF_CHECK_EQUAL(venus.data, data({{"bro/events/failures", "oops"},
                                    {"bro/events/failures", "sorry!"},
                                    {"bro/events/logging", 123},
                                    {"bro/events/logging", 456}}));
  CAF_CHECK_EQUAL(earth.data, data({{"bro/events/failures", "oops"},
                                    {"bro/events/failures", "sorry!"}}));
}

// Checks whether topic subscriptions are prefix-based using the synchronous
// `endpoint::make_subscriber` API to subscribe to topics.
CAF_TEST(topic_prefix_matching_make_subscriber) {
  connect_peers();
  MESSAGE("assume two peers for mercury");
  mercury.loop_after_next_enqueue();
  auto mercury_peers = mercury.ep.peers();
  CAF_REQUIRE_EQUAL(mercury_peers.size(), 2);
  CAF_CHECK_EQUAL(mercury_peers.front().status, peer_status::peered);
  CAF_CHECK_EQUAL(mercury_peers.back().status, peer_status::peered);
  MESSAGE("assume one peer for venus");
  venus.loop_after_next_enqueue();
  auto venus_peers = venus.ep.peers();
  CAF_REQUIRE_EQUAL(venus_peers.size(), 1);
  CAF_CHECK_EQUAL(venus_peers.front().status, peer_status::peered);
  MESSAGE("assume one peer for earth");
  earth.loop_after_next_enqueue();
  auto earth_peers = earth.ep.peers();
  CAF_REQUIRE_EQUAL(earth_peers.size(), 1);
  CAF_CHECK_EQUAL(earth_peers.front().status, peer_status::peered);
  MESSAGE("subscribe to 'bro/events' on venus");
  auto venus_s1 = venus.ep.make_subscriber({"bro/events"});
  auto venus_s2 = venus.ep.make_subscriber({"bro/events"});
  exec_loop();
  MESSAGE("subscribe to 'bro/events/failures' on earth");
  auto earth_s1 = earth.ep.make_subscriber({"bro/events/failures"});
  auto earth_s2 = earth.ep.make_subscriber({"bro/events/failures"});
  exec_loop();
  MESSAGE("verify subscriptions");
  auto filter = [](std::initializer_list<topic> xs) -> std::vector<topic> {
    return xs;
  };
  mercury.loop_after_next_enqueue();
  CAF_CHECK_EQUAL(mercury.ep.peer_subscriptions(),
                  filter({"bro/events", "bro/events/failures"}));
  venus.loop_after_next_enqueue();
  CAF_CHECK_EQUAL(venus.ep.peer_subscriptions(), filter({}));
  earth.loop_after_next_enqueue();
  CAF_CHECK_EQUAL(earth.ep.peer_subscriptions(), filter({}));
  MESSAGE("publish to 'bro/events/(logging|failures)' on mercury");
  mercury.publish("bro/events/failures", "oops", "sorry!");
  mercury.publish("bro/events/logging", 123, 456);
  MESSAGE("verify published data");
  auto data = [](std::initializer_list<endpoint::value_type> xs) -> data_vector {
    return xs;
  };
  CAF_CHECK_EQUAL(venus_s1.poll(), data({{"bro/events/failures", "oops"},
                                         {"bro/events/failures", "sorry!"},
                                         {"bro/events/logging", 123},
                                         {"bro/events/logging", 456}}));
  CAF_CHECK_EQUAL(venus_s2.poll(), data({{"bro/events/failures", "oops"},
                                         {"bro/events/failures", "sorry!"},
                                         {"bro/events/logging", 123},
                                         {"bro/events/logging", 456}}));
  CAF_CHECK_EQUAL(earth_s1.poll(), data({{"bro/events/failures", "oops"},
                                         {"bro/events/failures", "sorry!"}}));
  CAF_CHECK_EQUAL(earth_s2.poll(), data({{"bro/events/failures", "oops"},
                                         {"bro/events/failures", "sorry!"}}));
  exec_loop();
}

// -- unpeering of nodes and emitted status/error messages ---------------------

using event_value = event_subscriber::value_type;

struct code {
  code(ec x) : value(x) {
    // nop
  }

  code(sc x) : value(x) {
    // nop
  }

  code(const event_value& x) {
    if (broker::detail::holds_alternative<error>(x))
      value = static_cast<ec>(broker::detail::get<error>(x).code());
    else
      value = broker::detail::get<status>(x).code();
  }

  detail::variant<sc, ec> value;
};

std::string to_string(const code& x) {
  return broker::detail::holds_alternative<sc>(x.value)
         ? to_string(broker::detail::get<sc>(x.value))
         : to_string(broker::detail::get<ec>(x.value));
}

bool operator==(const code& x, const code& y) {
  return x.value == y.value;
}

std::vector<code> event_log(std::initializer_list<code> xs) {
  return {xs};
}

std::vector<code> event_log(const std::vector<event_value>& xs) {
  std::vector<code> ys;
  ys.reserve(xs.size());
  for (auto& x : xs)
    ys.emplace_back(x);
  return ys;
}

CAF_TEST(unpeering) {
  MESSAGE("get events from all peers");
  auto mercury_es = mercury.ep.make_event_subscriber(true);
  auto venus_es = venus.ep.make_event_subscriber(true);
  auto earth_es = earth.ep.make_event_subscriber(true);
  connect_peers();
  CAF_CHECK_EQUAL(event_log(mercury_es.poll()),
                  event_log({sc::peer_added, sc::peer_added}));
  CAF_CHECK_EQUAL(event_log(venus_es.poll()), event_log({sc::peer_added}));
  CAF_CHECK_EQUAL(event_log(earth_es.poll()), event_log({sc::peer_added}));
  MESSAGE("disconnect venus from mercury");
  venus.loop_after_next_enqueue();
  venus.ep.unpeer("mercury", 4040);
  CAF_CHECK_EQUAL(event_log(mercury_es.poll()), event_log({sc::peer_lost}));
  CAF_CHECK_EQUAL(event_log(venus_es.poll()), event_log({sc::peer_removed}));
  CAF_CHECK_EQUAL(event_log(earth_es.poll()), event_log({}));
  MESSAGE("disconnect venus again (raises ec::peer_invalid)");
  venus.loop_after_next_enqueue();
  venus.ep.unpeer("mercury", 4040);
  CAF_CHECK_EQUAL(event_log(mercury_es.poll()), event_log({}));
  CAF_CHECK_EQUAL(event_log(venus_es.poll()), event_log({ec::peer_invalid}));
  CAF_CHECK_EQUAL(event_log(earth_es.poll()), event_log({}));
  MESSAGE("disconnect venus from sun (invalid peer)");
  venus.loop_after_next_enqueue();
  venus.ep.unpeer("sun", 123);
  CAF_CHECK_EQUAL(event_log(mercury_es.poll()), event_log({}));
  CAF_CHECK_EQUAL(event_log(venus_es.poll()), event_log({ec::peer_invalid}));
  CAF_CHECK_EQUAL(event_log(earth_es.poll()), event_log({}));
  MESSAGE("disconnect earth from mercury");
  earth.loop_after_next_enqueue();
  earth.ep.unpeer("mercury", 4040);
  CAF_CHECK_EQUAL(event_log(mercury_es.poll()), event_log({sc::peer_lost}));
  CAF_CHECK_EQUAL(event_log(venus_es.poll()), event_log({}));
  CAF_CHECK_EQUAL(event_log(earth_es.poll()), event_log({sc::peer_removed}));
}

CAF_TEST(unpeering_without_connections) {
  MESSAGE("get events from all peers");
  auto venus_es = venus.ep.make_event_subscriber(true);
  MESSAGE("disconnect venus from non-existing peer");
  venus.loop_after_next_enqueue();
  venus.ep.unpeer("mercury", 4040);
  CAF_CHECK_EQUAL(event_log(venus_es.poll()), event_log({sc::peer_lost}));
}

CAF_TEST(connection_retry) {
  MESSAGE("get events from mercury and venus");
  auto mercury_es = mercury.ep.make_event_subscriber(true);
  auto venus_es = venus.ep.make_event_subscriber(true);
  MESSAGE("initiate peering from venus to mercury (will fail)");
  venus.ep.peer_nosync("mercury", 4040, std::chrono::seconds(1));
  exec_loop();
  MESSAGE("start listening on mercury:4040");
  auto server_handle = mercury.make_accept_handle();
  mercury.mpx.prepare_connection(server_handle,
                                 mercury.make_connection_handle(), venus.mpx,
                                 "mercury", 4040,
                                 venus.make_connection_handle());
  // We need to connect venus while mercury is blocked on ep.listen() in order
  // to avoid a "deadlock" in `ep.listen()`.
  mercury.sched.after_next_enqueue([&] {
    exec_loop();
    MESSAGE("peer venus to mercury:4040 by triggering the retry timeout");
    CAF_CHECK_EQUAL(venus.sched.dispatch(), 1);
    exec_loop();
  });
  mercury.ep.listen("", 4040);
  MESSAGE("check event logs");
  CAF_CHECK_EQUAL(event_log(mercury_es.poll()), event_log({sc::peer_added}));
  CAF_CHECK_EQUAL(event_log(venus_es.poll()), event_log({sc::peer_added}));
  MESSAGE("disconnect venus from mercury");
  venus.loop_after_next_enqueue();
  venus.ep.unpeer("mercury", 4040);
  CAF_CHECK_EQUAL(event_log(mercury_es.poll()), event_log({sc::peer_lost}));
  CAF_CHECK_EQUAL(event_log(venus_es.poll()), event_log({sc::peer_removed}));
}

CAF_TEST_FIXTURE_SCOPE_END()

