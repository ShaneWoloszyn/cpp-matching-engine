#pragma once

#include <cstdint>
#include <vector>
#include <list>
#include <map>
#include <unordered_map>
#include <memory>
#include <optional>
#include <string_view>

namespace orderbook {

    // ID and price types
    enum class OrderId   : std::uint64_t {};
    enum class Price     : std::int64_t  {}; 
    enum class Quantity  : std::uint64_t {};
    enum class Timestamp : std::uint64_t {};  

    // enums for orders
    enum class Side : std::uint8_t {
    Buy,
    Sell,
    };

    enum class OrderType : std::uint8_t {
        Limit,
        Market,
    };

    enum class TimeInForce : std::uint8_t {
        GTC,   // good til cancelled
        IOC,   // immediate or cancel
        FOK,   // fill or cill
        Day,   // cancel at end of session
    };

    // order spec and identifier
    struct NewOrder {
        OrderId      id;            // caller supplied, engine rejects duplicates
        Side         side;
        OrderType    type;
        TimeInForce  tif;
        Price        price;         
        Quantity     quantity;
        Timestamp    timestamp;     // caller supplied, used for priority
    };


    // engin events
    enum class EventType : std::uint8_t {
        Ack,           // order accepted and resting
        Fill,          // a full or partial execution
        Cancel,        // order removed 
        Reject,        // order or cancel rejected
    };

    enum class RejectReason : std::uint8_t {
        DuplicateOrderId,
        UnknownOrderId,
        InvalidPrice,
        InvalidQuantity,
        AlreadyTerminal,
        IocNoFill,        // IOC arrived with nothing to fill 
        FokInsufficientLiquidity,
    };

    enum class CancelReason : std::uint8_t {
        UserRequested,
        IocRemainder,
        DayExpiry,
        EngineShutdown,
    };

    struct Event {
        EventType    type;
        OrderId      order_id;       // the order this event pertains to
        OrderId      counterparty;   // valid only for Fill
        Price        price;          // fill price for Fill, original price for Ack/Cancel
        Quantity     quantity;       // fill qty for Fill, remaining qty for Ack/Cancel
        Timestamp    timestamp;
        RejectReason reject_reason;  // valid only for Reject
        CancelReason cancel_reason;  // valid only for Cancel
    };

    class Engine {
    public:
        Engine();
        ~Engine();

        // Non-copyable: the engine owns mutable state with internal pointers
        // (list iterators in the locator hashmap). Copying would invalidate them.
        // Move is also disabled in v1 for the same reason; we'll revisit in Step 3.
        Engine(const Engine&)            = delete;
        Engine& operator=(const Engine&) = delete;
        Engine(Engine&&)                 = delete;
        Engine& operator=(Engine&&)      = delete;

        // Submit a new order. Returns the sequence of events the order produced:
        //  - 1 Ack if the order rested
        //  - 0+ Fills if it matched
        //  - 1 Cancel if it was IOC and didn't fully fill (remainder)
        //  - 1 Reject if it failed validation
        // The returned vector is empty only in impossible circumstances; if you
        // see an empty vector, the engine has a bug.
        [[nodiscard]] std::vector<Event> submit(const NewOrder& order);

        // Cancel a resting order by ID. Returns:
        //  - 1 Cancel event (reason=UserRequested) on success
        //  - 1 Reject event (reason=UnknownOrderId or AlreadyTerminal) on failure
        [[nodiscard]] std::vector<Event> cancel(OrderId id, Timestamp ts);

        // Top-of-book accessors. Return std::nullopt if the side is empty.
        struct TopOfBook { Price price; Quantity aggregate_quantity; };
        [[nodiscard]] std::optional<TopOfBook> best_bid() const noexcept;
        [[nodiscard]] std::optional<TopOfBook> best_ask() const noexcept;

        // Depth: top N levels per side, best-first.
        struct Level { Price price; Quantity aggregate_quantity; };
        [[nodiscard]] std::vector<Level> bid_depth(std::size_t n) const;
        [[nodiscard]] std::vector<Level> ask_depth(std::size_t n) const;

        // Debug: O(N) walk, verifies all invariants. Used in tests, not in hot path.
        void assert_invariants() const;

    private:
        struct Impl;                       // pimpl idiom — see below
        std::unique_ptr<Impl> impl_;
    };

}  // namespace orderbook