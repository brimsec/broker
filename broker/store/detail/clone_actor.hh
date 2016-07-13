#ifndef BROKER_STORE_DETAIL_CLONE_ACTOR_HH
#define BROKER_STORE_DETAIL_CLONE_ACTOR_HH

#include <unordered_map>

#include <caf/stateful_actor.hpp>

namespace broker {
namespace store {
namespace detail {

struct clone_state {
  std::unordered_map<data, data> store;
};

caf::behavior clone_actor(caf::stateful_actor<clone_state>* self,
                           caf::actor core, caf::actor master,
                           std::string name);

} // namespace detail
} // namespace store
} // namespace broker

#endif // BROKER_STORE_DETAIL_CLONE_ACTOR_HH