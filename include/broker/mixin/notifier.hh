#pragma once

#include <caf/behavior.hpp>
#include <caf/group.hpp>

#include "broker/atoms.hh"
#include "broker/detail/assert.hh"
#include "broker/detail/lift.hh"
#include "broker/endpoint_info.hh"
#include "broker/error.hh"
#include "broker/logger.hh"
#include "broker/status.hh"

namespace broker::mixin {

template <class Base, class Subtype>
class notifier : public Base {
public:
  using super = Base;

  using extended_base = notifier;

  using peer_id_type = typename super::peer_id_type;

  using communication_handle_type = typename Base::communication_handle_type;

  // The notifier embeds `endpoint_info` objects into status and error updates.
  // While we keep the implementation as generic as possible, the current
  // implementation `endpoint_info` prohibits any other peer ID type at the
  // moment.
  static_assert(std::is_same<peer_id_type, caf::node_id>::value);

  template <class... Ts>
  explicit notifier(Ts&&... xs) : super(std::forward<Ts>(xs)...) {
    auto& groups = super::self()->system().groups();
    errors_ = groups.get_local("broker/errors");
    statuses_ = groups.get_local("broker/statuses");
  }

  void peer_connected(const peer_id_type& remote_id,
                      const communication_handle_type& hdl) {
    emit(remote_id, sc::peer_added, "handshake successful");
    super::peer_connected(remote_id, hdl);
  }

  void peer_disconnected(const peer_id_type& remote_id,
                         const communication_handle_type& hdl,
                         const error& reason) {
    emit(remote_id, sc::peer_lost, "lost connection to remote peer");
    super::peer_disconnected(remote_id, hdl, reason);
  }

  void peer_removed(const peer_id_type& remote_id,
                    const communication_handle_type& hdl) {
    emit(remote_id, sc::peer_removed, "removed connection to remote peer");
    super::peer_removed(remote_id, hdl);
  }

  void peer_unavailable(const network_info& addr) {
    auto self = super::self();
    emit({}, addr, ec::peer_unavailable, "unable to connect to remote peer");
  }

  template <class T>
  void cannot_remove_peer(const T& x) {
    emit(x, ec::peer_invalid, "cannot unpeer from unknown peer");
    super::cannot_remove_peer(x);
  }

  void disable_notifications() {
    errors_ = caf::group{};
    statuses_ = caf::group{};
  }

  template <class... Fs>
  caf::behavior make_behavior(Fs... fs) {
    using detail::lift;
    auto& d = dref();
    return super::make_behavior(
      fs..., lift<atom::no_events>(d, &Subtype::disable_notifications),
      [=](atom::publish, endpoint_info& receiver, data_message& msg) {
        dref().ship(msg, receiver.node);
      },
      [](atom::add, atom::status, const caf::actor&) {
        // TODO: this handler exists only for backwards-compatibility. It used
        //       to register status subscribers for synchronization. Eventually,
        //       we should either re-implement the synchronization if necessary
        //       or remove this handler.
      });
  }

private:
  auto& dref() {
    return *static_cast<Subtype*>(this);
  }

  template <class Enum>
  void emit(const peer_id_type& remote_id, const network_info& x, Enum code,
            const char* msg) {
    BROKER_INFO("emit:" << code << x);
    auto self = super::self();
    if constexpr (std::is_same<Enum, sc>::value)
      self->send(statuses_, atom::local::value,
                 status::make(code, endpoint_info{remote_id, x}, msg));
    else
      self->send(errors_, atom::local::value,
                 make_error(code, endpoint_info{remote_id, x}, msg));
  }

  template <class Enum>
  void emit(const network_info& x, Enum code, const char* msg) {
    BROKER_INFO("emit:" << code << x);
    auto self = super::self();
    if constexpr (std::is_same<Enum, sc>::value)
      self->send(statuses_, atom::local::value,
                 status::make(code, endpoint_info{{}, x}, msg));
    else
      self->send(errors_, atom::local::value,
                 make_error(code, endpoint_info{{}, x}, msg));
  }

  /// Reports an error to all status subscribers.
  template <class Enum>
  void emit(const communication_handle_type& hdl, Enum code, const char* msg) {
    if (auto peer_id_opt = get_peer_id(dref().tbl(), hdl)) {
      auto peer_id = std::move(*peer_id_opt);
      auto on_cache_hit = [=](network_info x) { emit(peer_id, x, code, msg); };
      auto on_cache_miss = [=](caf::error) { emit(peer_id, {}, code, msg); };
      dref().cache().fetch(hdl, on_cache_hit, on_cache_miss);
    } else {
      auto on_cache_hit = [=](network_info x) { emit({}, x, code, msg); };
      auto on_cache_miss = [=](caf::error) {
        if constexpr (std::is_same<caf::node_id, peer_id_type>::value) {
          emit(hdl.node(), {}, code, msg);
        } else {
          BROKER_DEBUG(
            "cannot resolve actor handle to network info or ID:" << hdl);
          emit({}, {}, code, msg);
        }
      };
      dref().cache().fetch(hdl, on_cache_hit, on_cache_miss);
    }
  }

  template <class Enum>
  void emit(const peer_id_type& remote_id, Enum code, const char* msg) {
    auto on_cache_hit = [=](network_info x) { emit(remote_id, x, code, msg); };
    auto on_cache_miss = [=](caf::error) { emit(remote_id, {}, code, msg); };
    auto& tbl = dref().tbl();
    if (auto i = tbl.find(remote_id); i != tbl.end()) {
      dref().cache().fetch(i->second.hdl, on_cache_hit, on_cache_miss);
    } else {
      on_cache_miss({});
    }
  }

  /// Caches the CAF group for error messages.
  caf::group errors_;

  /// Caches the CAF group for status messages.
  caf::group statuses_;
};

} // namespace broker::mixin
