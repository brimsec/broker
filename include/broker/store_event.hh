#pragma once

#include <cstdint>
#include <string>

#include "broker/data.hh"

namespace broker {

/// Wraps view types for convenient handling of store events.
class store_event {
public:
  enum class type : uint8_t {
    add,
    put,
    erase,
  };

  /// A view into a ::data object representing an `add` event.
  /// Broker encodes `add` events as
  /// `["add", key: data, value: data, expiry: optional<timespan>]`.
  class add {
  public:
    add(const add&) noexcept = default;

    add& operator=(const add&) noexcept = default;

    static add make(const data& src) noexcept {
      if (auto xs = get_if<vector>(src))
        return make(*xs);
      return add{nullptr};
    }

    static add make(const vector& xs) noexcept;

    explicit operator bool() const noexcept {
      return xs_ != nullptr;
    }

    const data& key() const noexcept {
      return (*xs_)[1];
    }

    const data& value() const noexcept {
      return (*xs_)[2];
    }

    caf::optional<timespan> expiry() const noexcept {
      if (auto value = get_if<timespan>((*xs_)[3]))
        return *value;
      return nil;
    }

  private:
    explicit add(const vector* xs) noexcept : xs_(xs) {
      // nop
    }

    const vector* xs_;
  };

  /// A view into a ::data object representing a `put` event.
  /// Broker encodes `put` events as
  /// `["put", key: data, value: data, expiry: optional<timespan>]`.
  class put {
  public:
    put(const put&) noexcept = default;

    put& operator=(const put&) noexcept = default;

    static put make(const data& src) noexcept {
      if (auto xs = get_if<vector>(src))
        return make(*xs);
      return put{nullptr};
    }

    static put make(const vector& xs) noexcept;

    explicit operator bool() const noexcept {
      return xs_ != nullptr;
    }

    const data& key() const noexcept {
      return (*xs_)[1];
    }

    const data& value() const noexcept {
      return (*xs_)[2];
    }

    caf::optional<timespan> expiry() const noexcept {
      if (auto value = get_if<timespan>((*xs_)[3]))
        return *value;
      return nil;
    }

  private:
    explicit put(const vector* xs) noexcept : xs_(xs) {
      // nop
    }

    const vector* xs_;
  };

  /// A view into a ::data object representing an `erase` event.
  /// Broker encodes `erase` events as `["erase", key: data]`.
  class erase {
  public:
    erase(const erase&) noexcept = default;

    erase& operator=(const erase&) noexcept = default;

    static erase make(const data& src) noexcept {
      if (auto xs = get_if<vector>(src))
        return make(*xs);
      return erase{nullptr};
    }

    static erase make(const vector& xs) noexcept;

    explicit operator bool() const noexcept {
      return xs_ != nullptr;
    }

    const data& key() const noexcept {
      return (*xs_)[1];
    }

  private:
    explicit erase(const vector* xs) noexcept : xs_(xs) {
      // nop
    }

    const vector* xs_;
  };
};

/// @relates store_event::type
const char* to_string(store_event::type code) noexcept;

/// @relates store_event::type
bool convert(const std::string& src, store_event::type& dst) noexcept;

/// @relates store_event::type
bool convert(const data& src, store_event::type& dst) noexcept;

/// @relates store_event::type
bool convertible_to_store_event_type(const data& src) noexcept;

template <>
struct can_convert_predicate<store_event::type> {
  static bool check(const data& src) noexcept {
    return convertible_to_store_event_type(src);
  }
};

} // namespace broker