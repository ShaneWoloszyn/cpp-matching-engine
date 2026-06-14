#include "orderbook/engine.hpp"
#include <algorithm>
#include <cassert>
#include <functional>
#include <limits>

namespace orderbook {

// ---------------------------------------------------------------------------
// Raw-value helpers: unwrap the strong typedefs without casts scattered
// through every comparison.
// ---------------------------------------------------------------------------

static inline std::int64_t  rv(Price p)     { return static_cast<std::int64_t>(p);  }
static inline std::uint64_t rv(Quantity q)  { return static_cast<std::uint64_t>(q); }
static inline std::uint64_t rv(OrderId id)  { return static_cast<std::uint64_t>(id);}
static inline std::uint64_t rv(Timestamp t) { return static_cast<std::uint64_t>(t); }

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

struct ROrder {
    OrderId   id;
    Price     price;
    Quantity  remaining;
    Timestamp timestamp;
};

struct PriceLevel {
    std::list<ROrder> queue;
    Quantity          total{Quantity{0}};
};

struct Locator {
    Side                        side;
    Price                       price;
    std::list<ROrder>::iterator it;
};

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct Engine::Impl {
    // bids: descending order → best bid = begin()
    std::map<Price, PriceLevel, std::greater<Price>> bids;
    // asks: ascending order  → best ask = begin()
    std::map<Price, PriceLevel, std::less<Price>>    asks;

    std::unordered_map<std::uint64_t, Locator> loc;

    // ---- book mutations ---------------------------------------------------

    void rest(OrderId id, Side side, Price price, Quantity qty, Timestamp ts) {
        ROrder o{id, price, qty, ts};
        if (side == Side::Buy) {
            auto& lv = bids[price];
            lv.queue.push_back(o);
            auto it = std::prev(lv.queue.end());
            lv.total = Quantity{rv(lv.total) + rv(qty)};
            loc[rv(id)] = Locator{Side::Buy, price, it};
        } else {
            auto& lv = asks[price];
            lv.queue.push_back(o);
            auto it = std::prev(lv.queue.end());
            lv.total = Quantity{rv(lv.total) + rv(qty)};
            loc[rv(id)] = Locator{Side::Sell, price, it};
        }
    }

    // Remove a resting order entirely (used by cancel).
    // Caller must erase from `loc` separately.
    void remove(const Locator& lc) {
        if (lc.side == Side::Buy) {
            auto map_it = bids.find(lc.price);
            auto& lv = map_it->second;
            lv.total = Quantity{rv(lv.total) - rv(lc.it->remaining)};
            lv.queue.erase(lc.it);
            if (lv.queue.empty()) bids.erase(map_it);
        } else {
            auto map_it = asks.find(lc.price);
            auto& lv = map_it->second;
            lv.total = Quantity{rv(lv.total) - rv(lc.it->remaining)};
            lv.queue.erase(lc.it);
            if (lv.queue.empty()) asks.erase(map_it);
        }
    }

    // ---- FOK pre-flight ---------------------------------------------------

    // Returns true if `needed` quantity can be filled at or better than `limit`.
    bool has_liquidity(Side aggressor, Price limit, Quantity needed) const {
        std::uint64_t avail = 0;
        if (aggressor == Side::Buy) {
            for (auto& [p, lv] : asks) {          // ascending: stops when price > limit
                if (rv(p) > rv(limit)) break;
                avail += rv(lv.total);
                if (avail >= rv(needed)) return true;
            }
        } else {
            for (auto& [p, lv] : bids) {          // descending: stops when price < limit
                if (rv(p) < rv(limit)) break;
                avail += rv(lv.total);
                if (avail >= rv(needed)) return true;
            }
        }
        return false;
    }

    // ---- matching ---------------------------------------------------------

    // Walk the opposite side, consuming FIFO until `remaining` is zero or
    // there are no more crossable levels.  Emits Fill events for both the
    // aggressive order and each passive order touched.
    std::vector<Event> match(OrderId agg_id, Side agg_side,
                              Price limit, Quantity& remaining, Timestamp ts)
    {
        std::vector<Event> events;

        auto emit_fill = [&](OrderId oid, OrderId cp, Price fp, Quantity qty) {
            Event e{};
            e.type        = EventType::Fill;
            e.order_id    = oid;
            e.counterparty = cp;
            e.price       = fp;
            e.quantity    = qty;
            e.timestamp   = ts;
            events.push_back(e);
        };

        if (agg_side == Side::Buy) {
            auto it = asks.begin();
            while (it != asks.end() && rv(remaining) > 0) {
                if (rv(it->first) > rv(limit)) break;  // price no longer crosses

                auto& lv  = it->second;
                auto  q_it = lv.queue.begin();
                while (q_it != lv.queue.end() && rv(remaining) > 0) {
                    Quantity fill{std::min(rv(remaining), rv(q_it->remaining))};

                    emit_fill(agg_id,    q_it->id, it->first, fill);
                    emit_fill(q_it->id,  agg_id,   it->first, fill);

                    remaining = Quantity{rv(remaining) - rv(fill)};
                    lv.total  = Quantity{rv(lv.total)  - rv(fill)};

                    if (rv(q_it->remaining) == rv(fill)) {
                        loc.erase(rv(q_it->id));
                        q_it = lv.queue.erase(q_it);
                    } else {
                        q_it->remaining = Quantity{rv(q_it->remaining) - rv(fill)};
                        ++q_it;
                    }
                }
                it = lv.queue.empty() ? asks.erase(it) : std::next(it);
            }
        } else {
            auto it = bids.begin();
            while (it != bids.end() && rv(remaining) > 0) {
                if (rv(it->first) < rv(limit)) break;

                auto& lv  = it->second;
                auto  q_it = lv.queue.begin();
                while (q_it != lv.queue.end() && rv(remaining) > 0) {
                    Quantity fill{std::min(rv(remaining), rv(q_it->remaining))};

                    emit_fill(agg_id,    q_it->id, it->first, fill);
                    emit_fill(q_it->id,  agg_id,   it->first, fill);

                    remaining = Quantity{rv(remaining) - rv(fill)};
                    lv.total  = Quantity{rv(lv.total)  - rv(fill)};

                    if (rv(q_it->remaining) == rv(fill)) {
                        loc.erase(rv(q_it->id));
                        q_it = lv.queue.erase(q_it);
                    } else {
                        q_it->remaining = Quantity{rv(q_it->remaining) - rv(fill)};
                        ++q_it;
                    }
                }
                it = lv.queue.empty() ? bids.erase(it) : std::next(it);
            }
        }

        return events;
    }

    // ---- invariant checker (O(N), tests only) -----------------------------

    void assert_invariants() const {
        // 1. No price level is empty.
        for (auto& [p, lv] : bids) assert(!lv.queue.empty());
        for (auto& [p, lv] : asks) assert(!lv.queue.empty());

        // 2. PriceLevel.total == sum of order remainders.
        for (auto& [p, lv] : bids) {
            std::uint64_t s = 0;
            for (auto& o : lv.queue) s += rv(o.remaining);
            assert(s == rv(lv.total));
        }
        for (auto& [p, lv] : asks) {
            std::uint64_t s = 0;
            for (auto& o : lv.queue) s += rv(o.remaining);
            assert(s == rv(lv.total));
        }

        // 3. Best bid strictly below best ask (no crossed book).
        if (!bids.empty() && !asks.empty())
            assert(rv(bids.begin()->first) < rv(asks.begin()->first));

        // 4. Locator count == resting order count; each locator is valid.
        std::size_t resting = 0;
        for (auto& [p, lv] : bids) resting += lv.queue.size();
        for (auto& [p, lv] : asks) resting += lv.queue.size();
        assert(resting == loc.size());

        for (auto& [id, lc] : loc) {
            assert(rv(lc.it->id) == id);
            if (lc.side == Side::Buy)
                assert(bids.count(lc.price));
            else
                assert(asks.count(lc.price));
        }
    }
};

// ---------------------------------------------------------------------------
// Engine public interface
// ---------------------------------------------------------------------------

Engine::Engine()  : impl_(std::make_unique<Impl>()) {}
Engine::~Engine() = default;

std::vector<Event> Engine::submit(const NewOrder& o) {
    std::vector<Event> events;

    // ---- validation --------------------------------------------------------
    if (rv(o.quantity) == 0) {
        Event e{}; e.type = EventType::Reject; e.order_id = o.id;
        e.reject_reason = RejectReason::InvalidQuantity; e.timestamp = o.timestamp;
        return {e};
    }
    if (o.type == OrderType::Limit && rv(o.price) <= 0) {
        Event e{}; e.type = EventType::Reject; e.order_id = o.id;
        e.reject_reason = RejectReason::InvalidPrice; e.timestamp = o.timestamp;
        return {e};
    }
    if (impl_->loc.count(rv(o.id))) {
        Event e{}; e.type = EventType::Reject; e.order_id = o.id;
        e.reject_reason = RejectReason::DuplicateOrderId; e.timestamp = o.timestamp;
        return {e};
    }

    // ---- effective matching price ------------------------------------------
    // Market orders cross at any price on the opposite side.
    constexpr std::int64_t kMaxP = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t kMinP = std::numeric_limits<std::int64_t>::min();
    Price match_price = (o.type == OrderType::Market)
                        ? (o.side == Side::Buy ? Price{kMaxP} : Price{kMinP})
                        : o.price;

    // ---- FOK pre-flight ---------------------------------------------------
    if (o.tif == TimeInForce::FOK) {
        if (!impl_->has_liquidity(o.side, match_price, o.quantity)) {
            Event e{}; e.type = EventType::Reject; e.order_id = o.id;
            e.reject_reason = RejectReason::FokInsufficientLiquidity;
            e.timestamp = o.timestamp;
            return {e};
        }
    }

    // ---- match -------------------------------------------------------------
    Quantity remaining = o.quantity;
    auto fills = impl_->match(o.id, o.side, match_price, remaining, o.timestamp);
    events.insert(events.end(), fills.begin(), fills.end());

    if (rv(remaining) == 0) return events;  // fully filled

    // ---- remainder handling ------------------------------------------------
    bool is_immediate = (o.tif == TimeInForce::IOC ||
                         o.tif == TimeInForce::FOK ||
                         o.type == OrderType::Market);

    if (is_immediate) {
        if (fills.empty()) {
            // Nothing traded at all.
            Event e{}; e.type = EventType::Reject; e.order_id = o.id;
            e.reject_reason = RejectReason::IocNoFill; e.timestamp = o.timestamp;
            events.push_back(e);
        } else {
            Event e{}; e.type = EventType::Cancel; e.order_id = o.id;
            e.quantity = remaining; e.cancel_reason = CancelReason::IocRemainder;
            e.timestamp = o.timestamp;
            events.push_back(e);
        }
        return events;
    }

    // GTC / Day: rest remainder on the book.
    impl_->rest(o.id, o.side, o.price, remaining, o.timestamp);
    {
        Event e{}; e.type = EventType::Ack; e.order_id = o.id;
        e.price = o.price; e.quantity = remaining; e.timestamp = o.timestamp;
        events.push_back(e);
    }
    return events;
}

std::vector<Event> Engine::cancel(OrderId id, Timestamp ts) {
    auto it = impl_->loc.find(rv(id));
    if (it == impl_->loc.end()) {
        Event e{}; e.type = EventType::Reject; e.order_id = id;
        e.reject_reason = RejectReason::UnknownOrderId; e.timestamp = ts;
        return {e};
    }

    const Locator& lc = it->second;
    Quantity remaining = lc.it->remaining;
    Price    price     = lc.it->price;

    impl_->remove(lc);     // erases list entry + potentially the map level
    impl_->loc.erase(it);  // removes locator (invalidates lc ref — done using it)

    Event e{}; e.type = EventType::Cancel; e.order_id = id;
    e.price = price; e.quantity = remaining;
    e.cancel_reason = CancelReason::UserRequested; e.timestamp = ts;
    return {e};
}

std::optional<Engine::TopOfBook> Engine::best_bid() const noexcept {
    if (impl_->bids.empty()) return std::nullopt;
    auto& [p, lv] = *impl_->bids.begin();
    return TopOfBook{p, lv.total};
}

std::optional<Engine::TopOfBook> Engine::best_ask() const noexcept {
    if (impl_->asks.empty()) return std::nullopt;
    auto& [p, lv] = *impl_->asks.begin();
    return TopOfBook{p, lv.total};
}

std::vector<Engine::Level> Engine::bid_depth(std::size_t n) const {
    std::vector<Level> out;
    out.reserve(n);
    for (auto& [p, lv] : impl_->bids) {
        if (out.size() >= n) break;
        out.push_back({p, lv.total});
    }
    return out;
}

std::vector<Engine::Level> Engine::ask_depth(std::size_t n) const {
    std::vector<Level> out;
    out.reserve(n);
    for (auto& [p, lv] : impl_->asks) {
        if (out.size() >= n) break;
        out.push_back({p, lv.total});
    }
    return out;
}

void Engine::assert_invariants() const { impl_->assert_invariants(); }

}  // namespace orderbook
