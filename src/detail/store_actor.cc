#include "broker/detail/store_actor.hh"

#include "broker/detail/assert.hh"

#include <caf/scheduled_actor/flow.hpp>

using namespace std::string_literals;

namespace broker::detail {

namespace {

template <class T>
constexpr size_t vec_slots() {
  if constexpr (std::is_same<T, entity_id>::value)
    return 2;
  else
    return 1;
}

template <class T>
void append(vector& xs, const T& x) {
  xs.emplace_back(x);
}

template <class T>
void append(vector& xs, const std::optional<T>& x) {
  if (x)
    xs.emplace_back(*x);
  else
    xs.emplace_back(nil);
}

void append(vector& xs, const entity_id& x) {
  if (x) {
    if (auto ep = to<data>(x.endpoint)) {
      xs.emplace_back(std::move(*ep));
      xs.emplace_back(x.object);
      return;
    }
  }
  xs.emplace_back(nil);
  xs.emplace_back(nil);
}

template <class... Ts>
void fill_vector(vector& vec, const Ts&... xs) {
  vec.reserve((vec_slots<Ts>() + ...));
  (append(vec, xs), ...);
}

} // namespace

store_actor_state::~store_actor_state() {
  // nop
}

using caf::async::consumer_resource;
using caf::async::producer_resource;

void store_actor_state::init(caf::event_based_actor* selfptr,
                             endpoint_id this_endpoint, endpoint::clock* clock,
                             std::string&& store_name, caf::actor&& core,
                             consumer_resource<command_message> in_res,
                             producer_resource<command_message> out_res) {
  BROKER_ASSERT(self != nullptr);
  BROKER_ASSERT(clock != nullptr);
  this->self = selfptr;
  this->clock = clock;
  this->store_name = std::move(store_name);
  this->id.endpoint = this_endpoint;
  this->id.object = self->id();
  this->core = std::move(core);
  this->dst = topic::store_events() / this->store_name;
  auto& cfg = self->system().config();
  tick_interval = caf::get_or(cfg, "broker.store.tick-interval",
                              defaults::store::tick_interval);
  self //
    ->make_observable()
    .from_resource(in_res)
    .for_each([this](const command_message& msg) { dispatch(msg); },
              [this](const caf::error& what) { self->quit(what); },
              [this] { self->quit(); });
  out.emplace(self);
  out->as_observable().subscribe(out_res);
}

// -- event signaling ----------------------------------------------------------

void store_actor_state::emit_insert_event(const data& key, const data& value,
                                          const std::optional<timespan>& expiry,
                                          const entity_id& publisher) {
  vector xs;
  fill_vector(xs, "insert"s, store_name, key, value, expiry, publisher);
  self->send(core, atom::publish_v, atom::local_v,
             make_data_message(dst, data{std::move(xs)}));
}

void store_actor_state::emit_update_event(const data& key,
                                          const data& old_value,
                                          const data& new_value,
                                          const std::optional<timespan>& expiry,
                                          const entity_id& publisher) {
  vector xs;
  fill_vector(xs, "update"s, store_name, key, old_value, new_value, expiry,
              publisher);
  self->send(core, atom::publish_v, atom::local_v,
             make_data_message(dst, data{std::move(xs)}));
}

void store_actor_state::emit_erase_event(const data& key,
                                         const entity_id& publisher) {
  vector xs;
  fill_vector(xs, "erase"s, store_name, key, publisher);
  self->send(core, atom::publish_v, atom::local_v,
             make_data_message(dst, data{std::move(xs)}));
}

void store_actor_state::emit_expire_event(const data& key,
                                          const entity_id& publisher) {
  vector xs;
  fill_vector(xs, "expire"s, store_name, key, publisher);
  self->send(core, atom::publish_v, atom::local_v,
             make_data_message(dst, data{std::move(xs)}));
}

// -- callbacks for the behavior -----------------------------------------------

void store_actor_state::on_down_msg(const caf::actor_addr& source,
                                    const error& reason) {
  if (source == core) {
    BROKER_INFO("core is down, quit");
    self->quit(reason);
    return;
  }
  auto i = local_requests.begin();
  while (i != local_requests.end()) {
    if (source == i->second.next())
      i = local_requests.erase(i);
    else
      ++i;
  }
}

} // namespace broker::detail
