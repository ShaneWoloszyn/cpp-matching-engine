#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/operators.h>
#include "orderbook/engine.hpp"

namespace py = pybind11;
using namespace orderbook;

// Helpers to construct strongly-typed IDs from Python ints.
static inline OrderId   oid(std::uint64_t v) { return OrderId{v};   }
static inline Price     px(std::int64_t v)   { return Price{v};     }
static inline Quantity  qty(std::uint64_t v) { return Quantity{v};  }
static inline Timestamp ts(std::uint64_t v)  { return Timestamp{v}; }

PYBIND11_MODULE(orderbook, m) {
    m.doc() = "C++ limit order book matching engine";

    // ---- enums -------------------------------------------------------------
    py::enum_<Side>(m, "Side")
        .value("Buy",  Side::Buy)
        .value("Sell", Side::Sell);

    py::enum_<OrderType>(m, "OrderType")
        .value("Limit",  OrderType::Limit)
        .value("Market", OrderType::Market);

    py::enum_<TimeInForce>(m, "TimeInForce")
        .value("GTC", TimeInForce::GTC)
        .value("IOC", TimeInForce::IOC)
        .value("FOK", TimeInForce::FOK)
        .value("Day", TimeInForce::Day);

    py::enum_<EventType>(m, "EventType")
        .value("Ack",    EventType::Ack)
        .value("Fill",   EventType::Fill)
        .value("Cancel", EventType::Cancel)
        .value("Reject", EventType::Reject);

    py::enum_<RejectReason>(m, "RejectReason")
        .value("DuplicateOrderId",          RejectReason::DuplicateOrderId)
        .value("UnknownOrderId",            RejectReason::UnknownOrderId)
        .value("InvalidPrice",              RejectReason::InvalidPrice)
        .value("InvalidQuantity",           RejectReason::InvalidQuantity)
        .value("AlreadyTerminal",           RejectReason::AlreadyTerminal)
        .value("IocNoFill",                 RejectReason::IocNoFill)
        .value("FokInsufficientLiquidity",  RejectReason::FokInsufficientLiquidity);

    py::enum_<CancelReason>(m, "CancelReason")
        .value("UserRequested", CancelReason::UserRequested)
        .value("IocRemainder",  CancelReason::IocRemainder)
        .value("DayExpiry",     CancelReason::DayExpiry)
        .value("EngineShutdown",CancelReason::EngineShutdown);

    // ---- NewOrder ----------------------------------------------------------
    py::class_<NewOrder>(m, "NewOrder")
        .def(py::init([](std::uint64_t id, Side side, OrderType type, TimeInForce tif,
                         std::int64_t price, std::uint64_t quantity, std::uint64_t timestamp) {
            return NewOrder{oid(id), side, type, tif, px(price), qty(quantity), ts(timestamp)};
        }), py::arg("id"), py::arg("side"), py::arg("type") = OrderType::Limit,
            py::arg("tif") = TimeInForce::GTC, py::arg("price") = 0,
            py::arg("quantity") = 0, py::arg("timestamp") = 0)
        .def_property_readonly("id",        [](const NewOrder& o){ return static_cast<std::uint64_t>(o.id); })
        .def_property_readonly("side",      [](const NewOrder& o){ return o.side; })
        .def_property_readonly("type",      [](const NewOrder& o){ return o.type; })
        .def_property_readonly("tif",       [](const NewOrder& o){ return o.tif; })
        .def_property_readonly("price",     [](const NewOrder& o){ return static_cast<std::int64_t>(o.price); })
        .def_property_readonly("quantity",  [](const NewOrder& o){ return static_cast<std::uint64_t>(o.quantity); })
        .def_property_readonly("timestamp", [](const NewOrder& o){ return static_cast<std::uint64_t>(o.timestamp); })
        .def("__repr__", [](const NewOrder& o) {
            return "<NewOrder id=" + std::to_string(static_cast<std::uint64_t>(o.id)) +
                   " qty=" + std::to_string(static_cast<std::uint64_t>(o.quantity)) + ">";
        });

    // ---- Event -------------------------------------------------------------
    py::class_<Event>(m, "Event")
        .def_property_readonly("type",          [](const Event& e){ return e.type; })
        .def_property_readonly("order_id",      [](const Event& e){ return static_cast<std::uint64_t>(e.order_id); })
        .def_property_readonly("counterparty",  [](const Event& e){ return static_cast<std::uint64_t>(e.counterparty); })
        .def_property_readonly("price",         [](const Event& e){ return static_cast<std::int64_t>(e.price); })
        .def_property_readonly("quantity",      [](const Event& e){ return static_cast<std::uint64_t>(e.quantity); })
        .def_property_readonly("timestamp",     [](const Event& e){ return static_cast<std::uint64_t>(e.timestamp); })
        .def_property_readonly("reject_reason", [](const Event& e){ return e.reject_reason; })
        .def_property_readonly("cancel_reason", [](const Event& e){ return e.cancel_reason; })
        .def("__repr__", [](const Event& e) {
            auto et = [](EventType t) -> std::string {
                switch(t) {
                case EventType::Ack:    return "Ack";
                case EventType::Fill:   return "Fill";
                case EventType::Cancel: return "Cancel";
                case EventType::Reject: return "Reject";
                }
                return "?";
            };
            return "<Event " + et(e.type) +
                   " order=" + std::to_string(static_cast<std::uint64_t>(e.order_id)) +
                   " qty=" + std::to_string(static_cast<std::uint64_t>(e.quantity)) +
                   " px=" + std::to_string(static_cast<std::int64_t>(e.price)) + ">";
        });

    // ---- TopOfBook / Level -------------------------------------------------
    py::class_<Engine::TopOfBook>(m, "TopOfBook")
        .def_property_readonly("price",    [](const Engine::TopOfBook& t){ return static_cast<std::int64_t>(t.price); })
        .def_property_readonly("quantity", [](const Engine::TopOfBook& t){ return static_cast<std::uint64_t>(t.aggregate_quantity); });

    py::class_<Engine::Level>(m, "Level")
        .def_property_readonly("price",    [](const Engine::Level& l){ return static_cast<std::int64_t>(l.price); })
        .def_property_readonly("quantity", [](const Engine::Level& l){ return static_cast<std::uint64_t>(l.aggregate_quantity); });

    // ---- Engine ------------------------------------------------------------
    py::class_<Engine>(m, "Engine")
        .def(py::init<>())
        .def("submit", &Engine::submit, py::arg("order"),
             "Submit a new order. Returns list of events produced.")
        .def("cancel", [](Engine& e, std::uint64_t id, std::uint64_t t){
                 return e.cancel(OrderId{id}, Timestamp{t});
             }, py::arg("order_id"), py::arg("timestamp") = 0,
             "Cancel a resting order by ID. Returns list of events.")
        .def("best_bid",  &Engine::best_bid,  "Top bid level, or None if empty.")
        .def("best_ask",  &Engine::best_ask,  "Top ask level, or None if empty.")
        .def("bid_depth", &Engine::bid_depth, py::arg("n") = 10,
             "Top N bid levels, best-first.")
        .def("ask_depth", &Engine::ask_depth, py::arg("n") = 10,
             "Top N ask levels, best-first.")
        .def("assert_invariants", &Engine::assert_invariants,
             "O(N) invariant check. Raises on violation (uses assert).");
}
