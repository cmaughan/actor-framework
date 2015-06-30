/******************************************************************************
 *                       ____    _    _____                                   *
 *                      / ___|  / \  |  ___|    C++                           *
 *                     | |     / _ \ | |_       Actor                         *
 *                     | |___ / ___ \|  _|      Framework                     *
 *                      \____/_/   \_|_|                                      *
 *                                                                            *
 * Copyright (C) 2011 - 2015                                                  *
 * Dominik Charousset <dominik.charousset (at) haw-hamburg.de>                *
 *                                                                            *
 * Distributed under the terms and conditions of the BSD 3-Clause License or  *
 * (at your option) under the terms and conditions of the Boost Software      *
 * License 1.0. See accompanying files LICENSE and LICENSE_ALTERNATIVE.       *
 *                                                                            *
 * If you did not receive a copy of the license files, see                    *
 * http://opensource.org/licenses/BSD-3-Clause and                            *
 * http://www.boost.org/LICENSE_1_0.txt.                                      *
 ******************************************************************************/

#include "caf/config.hpp"

// exclude this suite; seems to be too much to swallow for MSVC
#ifndef CAF_WINDOWS

#define CAF_SUITE typed_spawn
#include "caf/test/unit_test.hpp"

#include "caf/string_algorithms.hpp"

#include "caf/all.hpp"

using namespace std;
using namespace caf;

using passed_atom = caf::atom_constant<caf::atom("passed")>;

namespace {

// check invariants of type system
using dummy1 = typed_actor<reacts_to<int, int>,
                           replies_to<double>::with<double>>;

using dummy2 = dummy1::extend<reacts_to<ok_atom>>;

static_assert(std::is_convertible<dummy2, dummy1>::value,
              "handle not assignable to narrower definition");

static_assert(! std::is_convertible<dummy1, dummy2>::value,
              "handle is assignable to broader definition");

/******************************************************************************
 *            simple request/response test            *
 ******************************************************************************/

struct my_request {
  int a;
  int b;
};

using server_type = typed_actor<replies_to<my_request>::with<bool>>;

bool operator==(const my_request& lhs, const my_request& rhs) {
  return lhs.a == rhs.a && lhs.b == rhs.b;
}

server_type::behavior_type typed_server1() {
  return {
    [](const my_request& req) {
      return req.a == req.b;
    }
  };
}

server_type::behavior_type typed_server2(server_type::pointer) {
  return typed_server1();
}

class typed_server3 : public server_type::base {

public:

  typed_server3(const string& line, actor buddy) { send(buddy, line); }

  behavior_type make_behavior() override { return typed_server2(this); }

};

void client(event_based_actor* self, actor parent, server_type serv) {
  self->sync_send(serv, my_request{0, 0}).then(
    [=](bool val1) {
      CAF_CHECK_EQUAL(val1, true);
      self->sync_send(serv, my_request{10, 20}).then(
        [=](bool val2) {
          CAF_CHECK_EQUAL(val2, false);
          self->send(parent, passed_atom::value);
        }
      );
    }
  );
}

void test_typed_spawn(server_type ts) {
  scoped_actor self;
  self->send(ts, my_request{1, 2});
  self->receive(
    [](bool value) {
      CAF_CHECK_EQUAL(value, false);
    }
  );
  self->send(ts, my_request{42, 42});
  self->receive(
    [](bool value) {
      CAF_CHECK_EQUAL(value, true);
    }
  );
  self->sync_send(ts, my_request{10, 20}).await(
    [](bool value) {
      CAF_CHECK_EQUAL(value, false);
    }
  );
  self->sync_send(ts, my_request{0, 0}).await(
    [](bool value) {
      CAF_CHECK_EQUAL(value, true);
    }
  );
  self->spawn<monitored>(client, self, ts);
  self->receive(
    [](passed_atom) {
      CAF_MESSAGE("received `passed_atom`");
    }
  );
  self->receive(
    [](const down_msg& dmsg) {
      CAF_CHECK_EQUAL(dmsg.reason, exit_reason::normal);
    }
  );
  self->send_exit(ts, exit_reason::user_shutdown);
}

/******************************************************************************
 *      test skipping of messages intentionally + using become()      *
 ******************************************************************************/

struct get_state_msg {};

using event_testee_type = typed_actor<replies_to<get_state_msg>::with<string>,
                                      replies_to<string>::with<void>,
                                      replies_to<float>::with<void>,
                                      replies_to<int>::with<int>>;

class event_testee : public event_testee_type::base {

public:

  behavior_type wait4string() {
    return {on<get_state_msg>() >> [] { return "wait4string"; },
        on<string>() >> [=] { become(wait4int()); },
        (on<float>() || on<int>()) >> skip_message};
  }

  behavior_type wait4int() {
    return {
      on<get_state_msg>() >> [] { return "wait4int"; },
      on<int>() >> [=]()->int {become(wait4float());
        return 42;
      },
      (on<float>() || on<string>()) >> skip_message
    };
  }

  behavior_type wait4float() {
    return {
      on<get_state_msg>() >> [] {
        return "wait4float";
      },
      on<float>() >> [=] { become(wait4string()); },
      (on<string>() || on<int>()) >> skip_message};
  }

  behavior_type make_behavior() override {
    return wait4int();
  }

};

/******************************************************************************
 *             simple 'forwarding' chain              *
 ******************************************************************************/

using string_actor = typed_actor<replies_to<string>::with<string>>;

void simple_relay(string_actor::pointer self, string_actor master, bool leaf) {
  string_actor next =
    leaf ? spawn_typed(simple_relay, master, false) : master;
  self->link_to(next);
  self->become(
    [=](const string& str) {
      return self->sync_send(next, str).then(
        [](const string & answer)->string {
          return answer;
        }
      );
  });
}

string_actor::behavior_type simple_string_reverter() {
  return {
    [](const string& str) {
      return string{str.rbegin(), str.rend()};
    }
  };
}

/******************************************************************************
 *            sending typed actor handles             *
 ******************************************************************************/

using int_actor = typed_actor<replies_to<int>::with<int>>;

int_actor::behavior_type int_fun() {
  return {
    [](int i) { return i * i; }
  };
}

behavior foo(event_based_actor* self) {
  return {
    [=](int i, int_actor server) {
      return self->sync_send(server, i).then([=](int result) -> int {
        self->quit(exit_reason::normal);
        return result;
      });
    }
  };
}

int_actor::behavior_type int_fun2(int_actor::pointer self) {
  self->trap_exit(true);
  return {
    [=](int i) {
      self->monitor(self->current_sender());
      return i * i;
    },
    [=](const down_msg& dm) {
      CAF_CHECK_EQUAL(dm.reason, exit_reason::normal);
      self->quit();
    },
    [=](const exit_msg&) {
      CAF_TEST_ERROR("Unexpected message: "
                     << to_string(self->current_message()));
    }
  };
}

behavior foo2(event_based_actor* self) {
  return {
    [=](int i, int_actor server) {
      return self->sync_send(server, i).then([=](int result) -> int {
        self->quit(exit_reason::normal);
        return result;
      });
    }
  };
}

struct fixture {
  fixture() {
    announce<get_state_msg>("get_state_msg");
    announce<int_actor>("int_actor");
    announce<my_request>("my_request", &my_request::a, &my_request::b);
  }

  ~fixture() {
    await_all_actors_done();
    shutdown();
  }
};

} // namespace <anonymous>

CAF_TEST_FIXTURE_SCOPE(typed_spawn_tests, fixture)

/******************************************************************************
 *                             put it all together                            *
 ******************************************************************************/

CAF_TEST(typed_spawns) {
  // run test series with typed_server(1|2)
  test_typed_spawn(spawn_typed(typed_server1));
  await_all_actors_done();
  CAF_MESSAGE("finished test series with `typed_server1`");

  test_typed_spawn(spawn_typed(typed_server2));
  await_all_actors_done();
  CAF_MESSAGE("finished test series with `typed_server2`");
  {
    scoped_actor self;
    test_typed_spawn(spawn_typed<typed_server3>("hi there", self));
    self->receive(on("hi there") >> [] {
      CAF_MESSAGE("received \"hi there\"");
    });
  }
}

CAF_TEST(test_event_testee) {
  // run test series with event_testee
  scoped_actor self;
  auto et = self->spawn_typed<event_testee>();
  string result;
  self->send(et, 1);
  self->send(et, 2);
  self->send(et, 3);
  self->send(et, .1f);
  self->send(et, "hello event testee!");
  self->send(et, .2f);
  self->send(et, .3f);
  self->send(et, "hello again event testee!");
  self->send(et, "goodbye event testee!");
  typed_actor<replies_to<get_state_msg>::with<string>> sub_et = et;
  // $:: is the anonymous namespace
  set<string> iface{"caf::replies_to<get_state_msg>::with<@str>",
                    "caf::replies_to<@str>::with<void>",
                    "caf::replies_to<float>::with<void>",
                    "caf::replies_to<@i32>::with<@i32>"};
  CAF_CHECK_EQUAL(join(sub_et->message_types(), ","), join(iface, ","));
  self->send(sub_et, get_state_msg{});
  // we expect three 42s
  int i = 0;
  self->receive_for(i, 3)([](int value) { CAF_CHECK_EQUAL(value, 42); });
  self->receive(
    [&](const string& str) {
      result = str;
    },
    after(chrono::minutes(1)) >> [&] {
      CAF_TEST_ERROR("event_testee does not reply");
      throw runtime_error("event_testee does not reply");
    }
  );
  self->send_exit(et, exit_reason::user_shutdown);
  self->await_all_other_actors_done();
  CAF_CHECK_EQUAL(result, "wait4int");
}

CAF_TEST(test_simple_string_reverter) {
  // run test series with string reverter
  scoped_actor self;
  // actor-under-test
  auto aut = self->spawn_typed<monitored>(simple_relay,
                                          spawn_typed(simple_string_reverter),
                                          true);
  set<string> iface{"caf::replies_to<@str>::with<@str>"};
  CAF_CHECK(aut->message_types() == iface);
  self->sync_send(aut, "Hello World!").await([](const string& answer) {
    CAF_CHECK_EQUAL(answer, "!dlroW olleH");
  });
  anon_send_exit(aut, exit_reason::user_shutdown);
}

CAF_TEST(test_sending_typed_actors) {
  scoped_actor self;
  auto aut = spawn_typed(int_fun);
  self->send(spawn(foo), 10, aut);
  self->receive(
    [](int i) { CAF_CHECK_EQUAL(i, 100); }
  );
  self->send_exit(aut, exit_reason::user_shutdown);
}

CAF_TEST(test_sending_typed_actors_and_down_msg) {
  scoped_actor self;
  auto aut = spawn_typed(int_fun2);
  self->send(spawn(foo2), 10, aut);
  self->receive([](int i) { CAF_CHECK_EQUAL(i, 100); });
}

CAF_TEST_FIXTURE_SCOPE_END()

#endif // CAF_WINDOWS
