"""
Test suite for the C++ matching engine.

Covers:
  - Order resting (Ack)
  - Full and partial fills
  - Price-time priority (FIFO within a level; better-priced levels first)
  - IOC: immediate fill / partial fill + cancel remainder / zero-fill reject
  - FOK: sufficient liquidity (fills) / insufficient (reject)
  - Market orders
  - Cancellation: success and error cases
  - Duplicate order ID rejection
  - Invariant preservation across all operations
"""

import pytest
import orderbook as ob


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def limit(id_, side, price, qty, tif=ob.TimeInForce.GTC, ts=0):
    return ob.NewOrder(id=id_, side=side, type=ob.OrderType.Limit,
                       tif=tif, price=price, quantity=qty, timestamp=ts)

def market(id_, side, qty, ts=0):
    return ob.NewOrder(id=id_, side=side, type=ob.OrderType.Market,
                       tif=ob.TimeInForce.IOC, price=0, quantity=qty, timestamp=ts)

def ioc(id_, side, price, qty, ts=0):
    return limit(id_, side, price, qty, tif=ob.TimeInForce.IOC, ts=ts)

def fok(id_, side, price, qty, ts=0):
    return limit(id_, side, price, qty, tif=ob.TimeInForce.FOK, ts=ts)

def events_of(evts, kind):
    return [e for e in evts if e.type == kind]

def fills(evts, order_id):
    return [e for e in evts if e.type == ob.EventType.Fill
            and e.order_id == order_id]

def acks(evts): return events_of(evts, ob.EventType.Ack)
def cancels(evts): return events_of(evts, ob.EventType.Cancel)
def rejects(evts): return events_of(evts, ob.EventType.Reject)


# ---------------------------------------------------------------------------
# 1. Resting orders
# ---------------------------------------------------------------------------

def test_limit_rests_when_no_opposite_side():
    e = ob.Engine()
    evts = e.submit(limit(1, ob.Side.Buy, 100, 10))
    assert len(acks(evts)) == 1
    assert len(rejects(evts)) == 0
    tob = e.best_bid()
    assert tob is not None
    assert tob.price == 100
    assert tob.quantity == 10
    assert e.best_ask() is None
    e.assert_invariants()


def test_two_resting_orders_same_side():
    e = ob.Engine()
    e.submit(limit(1, ob.Side.Sell, 200, 5))
    e.submit(limit(2, ob.Side.Sell, 210, 3))
    tob = e.best_ask()
    assert tob.price == 200          # best ask is lowest
    levels = e.ask_depth(5)
    assert levels[0].price == 200
    assert levels[1].price == 210
    e.assert_invariants()


# ---------------------------------------------------------------------------
# 2. Full match
# ---------------------------------------------------------------------------

def test_full_match_buy_aggressor():
    e = ob.Engine()
    e.submit(limit(1, ob.Side.Sell, 100, 10))

    evts = e.submit(limit(2, ob.Side.Buy, 100, 10))
    assert len(fills(evts, 2)) == 1
    assert fills(evts, 2)[0].quantity == 10
    assert fills(evts, 2)[0].price == 100
    assert fills(evts, 2)[0].counterparty == 1
    # Passive order also gets a fill event.
    assert len(fills(evts, 1)) == 1
    # Book should be empty.
    assert e.best_bid() is None
    assert e.best_ask() is None
    e.assert_invariants()


def test_full_match_sell_aggressor():
    e = ob.Engine()
    e.submit(limit(1, ob.Side.Buy, 100, 10))

    evts = e.submit(limit(2, ob.Side.Sell, 100, 10))
    assert fills(evts, 2)[0].quantity == 10
    assert fills(evts, 2)[0].price == 100
    e.assert_invariants()


# ---------------------------------------------------------------------------
# 3. Partial fills
# ---------------------------------------------------------------------------

def test_aggressor_larger_than_resting():
    """Aggressor 10 vs resting 6 → partial fill, aggressor rests 4."""
    e = ob.Engine()
    e.submit(limit(1, ob.Side.Sell, 100, 6))

    evts = e.submit(limit(2, ob.Side.Buy, 100, 10))
    agg_fills = fills(evts, 2)
    assert len(agg_fills) == 1
    assert agg_fills[0].quantity == 6

    # Aggressor rests the remaining 4 as a bid.
    assert len(acks(evts)) == 1
    assert acks(evts)[0].quantity == 4
    assert e.best_bid().price == 100
    assert e.best_bid().quantity == 4
    assert e.best_ask() is None
    e.assert_invariants()


def test_aggressor_smaller_than_resting():
    """Aggressor 4 vs resting 10 → partial fill, resting has 6 remaining."""
    e = ob.Engine()
    e.submit(limit(1, ob.Side.Sell, 100, 10))

    evts = e.submit(limit(2, ob.Side.Buy, 100, 4))
    assert fills(evts, 2)[0].quantity == 4
    assert fills(evts, 1)[0].quantity == 4
    assert e.best_ask().quantity == 6
    e.assert_invariants()


def test_aggressor_sweeps_multiple_levels():
    """Aggressor buy at 110 sweeps asks at 100 (qty 5) and 105 (qty 5)."""
    e = ob.Engine()
    e.submit(limit(1, ob.Side.Sell, 100, 5))
    e.submit(limit(2, ob.Side.Sell, 105, 5))

    evts = e.submit(limit(3, ob.Side.Buy, 110, 10))
    agg_fills = fills(evts, 3)
    # Two passive orders → two fill events for aggressor.
    assert len(agg_fills) == 2
    assert sum(f.quantity for f in agg_fills) == 10
    assert agg_fills[0].price == 100  # better price matched first
    assert agg_fills[1].price == 105
    assert e.best_ask() is None
    e.assert_invariants()


# ---------------------------------------------------------------------------
# 4. Price-time priority
# ---------------------------------------------------------------------------

def test_fifo_within_price_level():
    """Two resting sells at the same price; earlier one fills first."""
    e = ob.Engine()
    e.submit(limit(1, ob.Side.Sell, 100, 5, ts=1))
    e.submit(limit(2, ob.Side.Sell, 100, 5, ts=2))

    evts = e.submit(limit(3, ob.Side.Buy, 100, 5))
    # Order 1 should be the counterparty.
    agg_f = fills(evts, 3)
    assert agg_f[0].counterparty == 1
    # Order 2 still rests.
    assert e.best_ask().quantity == 5
    e.assert_invariants()


def test_better_price_level_first():
    """Resting sells at 100 and 105; buy at 110 hits 100 first."""
    e = ob.Engine()
    e.submit(limit(1, ob.Side.Sell, 105, 5))
    e.submit(limit(2, ob.Side.Sell, 100, 5))

    evts = e.submit(limit(3, ob.Side.Buy, 110, 5))
    agg_f = fills(evts, 3)
    assert len(agg_f) == 1
    assert agg_f[0].price == 100
    assert agg_f[0].counterparty == 2
    e.assert_invariants()


# ---------------------------------------------------------------------------
# 5. IOC orders
# ---------------------------------------------------------------------------

def test_ioc_full_fill():
    e = ob.Engine()
    e.submit(limit(1, ob.Side.Sell, 100, 10))

    evts = e.submit(ioc(2, ob.Side.Buy, 100, 5))
    assert len(fills(evts, 2)) == 1
    assert fills(evts, 2)[0].quantity == 5
    assert len(cancels(evts)) == 0
    e.assert_invariants()


def test_ioc_partial_fill_cancels_remainder():
    e = ob.Engine()
    e.submit(limit(1, ob.Side.Sell, 100, 3))

    evts = e.submit(ioc(2, ob.Side.Buy, 100, 10))
    assert fills(evts, 2)[0].quantity == 3
    # Remainder 7 is cancelled, not rested.
    c = cancels(evts)
    assert len(c) == 1
    assert c[0].quantity == 7
    assert c[0].cancel_reason == ob.CancelReason.IocRemainder
    assert e.best_bid() is None
    e.assert_invariants()


def test_ioc_no_fill_is_rejected():
    e = ob.Engine()
    evts = e.submit(ioc(1, ob.Side.Buy, 100, 5))
    r = rejects(evts)
    assert len(r) == 1
    assert r[0].reject_reason == ob.RejectReason.IocNoFill
    e.assert_invariants()


# ---------------------------------------------------------------------------
# 6. FOK orders
# ---------------------------------------------------------------------------

def test_fok_sufficient_liquidity_fills():
    e = ob.Engine()
    e.submit(limit(1, ob.Side.Sell, 100, 10))

    evts = e.submit(fok(2, ob.Side.Buy, 100, 10))
    assert len(fills(evts, 2)) == 1
    assert fills(evts, 2)[0].quantity == 10
    e.assert_invariants()


def test_fok_insufficient_liquidity_rejects():
    e = ob.Engine()
    e.submit(limit(1, ob.Side.Sell, 100, 5))

    evts = e.submit(fok(2, ob.Side.Buy, 100, 10))
    r = rejects(evts)
    assert len(r) == 1
    assert r[0].reject_reason == ob.RejectReason.FokInsufficientLiquidity
    # Resting order untouched.
    assert e.best_ask().quantity == 5
    e.assert_invariants()


def test_fok_across_multiple_levels():
    e = ob.Engine()
    e.submit(limit(1, ob.Side.Sell, 100, 5))
    e.submit(limit(2, ob.Side.Sell, 101, 5))

    evts = e.submit(fok(3, ob.Side.Buy, 101, 10))
    agg_f = fills(evts, 3)
    assert len(agg_f) == 2
    assert sum(f.quantity for f in agg_f) == 10
    e.assert_invariants()


# ---------------------------------------------------------------------------
# 7. Market orders
# ---------------------------------------------------------------------------

def test_market_buy_matches_best_ask():
    e = ob.Engine()
    e.submit(limit(1, ob.Side.Sell, 100, 10))
    e.submit(limit(2, ob.Side.Sell, 110, 10))

    evts = e.submit(market(3, ob.Side.Buy, 5))
    assert fills(evts, 3)[0].price == 100   # matched at best ask
    e.assert_invariants()


def test_market_no_liquidity_rejected():
    e = ob.Engine()
    evts = e.submit(market(1, ob.Side.Buy, 5))
    assert len(rejects(evts)) == 1
    assert rejects(evts)[0].reject_reason == ob.RejectReason.IocNoFill


def test_market_order_does_not_rest():
    e = ob.Engine()
    e.submit(limit(1, ob.Side.Sell, 100, 5))
    e.submit(market(2, ob.Side.Buy, 10))    # fills 5, remainder 5 cancelled
    assert e.best_bid() is None
    e.assert_invariants()


# ---------------------------------------------------------------------------
# 8. Cancellation
# ---------------------------------------------------------------------------

def test_cancel_resting_order():
    e = ob.Engine()
    e.submit(limit(1, ob.Side.Buy, 100, 10))

    evts = e.cancel(1)
    c = cancels(evts)
    assert len(c) == 1
    assert c[0].order_id == 1
    assert c[0].cancel_reason == ob.CancelReason.UserRequested
    assert c[0].quantity == 10
    assert e.best_bid() is None
    e.assert_invariants()


def test_cancel_unknown_id_rejects():
    e = ob.Engine()
    evts = e.cancel(999)
    r = rejects(evts)
    assert len(r) == 1
    assert r[0].reject_reason == ob.RejectReason.UnknownOrderId


def test_cancel_then_submit_same_id_allowed():
    """After cancellation the ID slot is freed; the caller may reuse it."""
    e = ob.Engine()
    e.submit(limit(1, ob.Side.Buy, 100, 10))
    e.cancel(1)
    evts = e.submit(limit(1, ob.Side.Buy, 100, 5))
    assert len(acks(evts)) == 1
    e.assert_invariants()


def test_cancel_partially_filled_order():
    e = ob.Engine()
    e.submit(limit(1, ob.Side.Sell, 100, 10))
    e.submit(limit(2, ob.Side.Buy, 100, 5))   # partial fill on order 1 (5 left)

    evts = e.cancel(1)
    c = cancels(evts)
    assert len(c) == 1
    assert c[0].quantity == 5     # only the remaining qty is cancelled
    e.assert_invariants()


# ---------------------------------------------------------------------------
# 9. Duplicate order ID
# ---------------------------------------------------------------------------

def test_duplicate_id_rejected():
    e = ob.Engine()
    e.submit(limit(1, ob.Side.Buy, 100, 10))
    evts = e.submit(limit(1, ob.Side.Buy, 101, 5))
    r = rejects(evts)
    assert len(r) == 1
    assert r[0].reject_reason == ob.RejectReason.DuplicateOrderId
    # Original order untouched.
    assert e.best_bid().quantity == 10
    e.assert_invariants()


# ---------------------------------------------------------------------------
# 10. Validation
# ---------------------------------------------------------------------------

def test_zero_quantity_rejected():
    e = ob.Engine()
    evts = e.submit(limit(1, ob.Side.Buy, 100, 0))
    assert rejects(evts)[0].reject_reason == ob.RejectReason.InvalidQuantity


def test_non_positive_limit_price_rejected():
    e = ob.Engine()
    evts = e.submit(limit(1, ob.Side.Buy, 0, 10))
    assert rejects(evts)[0].reject_reason == ob.RejectReason.InvalidPrice

    evts = e.submit(limit(2, ob.Side.Buy, -5, 10))
    assert rejects(evts)[0].reject_reason == ob.RejectReason.InvalidPrice


# ---------------------------------------------------------------------------
# 11. Depth
# ---------------------------------------------------------------------------

def test_bid_depth_ordering():
    e = ob.Engine()
    for px in [98, 100, 99]:
        e.submit(limit(px, ob.Side.Buy, px, 1))
    depth = e.bid_depth(3)
    assert [l.price for l in depth] == [100, 99, 98]


def test_ask_depth_ordering():
    e = ob.Engine()
    for px in [103, 101, 102]:
        e.submit(limit(px, ob.Side.Sell, px, 1))
    depth = e.ask_depth(3)
    assert [l.price for l in depth] == [101, 102, 103]


def test_depth_limited_to_n():
    e = ob.Engine()
    for px in range(100, 110):
        e.submit(limit(px, ob.Side.Sell, px, 1))
    assert len(e.ask_depth(3)) == 3


# ---------------------------------------------------------------------------
# 12. Invariant stress test
# ---------------------------------------------------------------------------

def test_stress_invariants():
    """Random mix of submits and cancels; assert invariants every 100 ops."""
    import random
    rng = random.Random(42)

    e = ob.Engine()
    live = []    # (id, side, price)
    next_id = 1

    for _ in range(5_000):
        op = rng.randint(0, 9)

        if op < 7 or not live:
            # Submit limit GTC
            side = ob.Side.Buy if rng.randint(0, 1) else ob.Side.Sell
            px_  = rng.randint(95, 105)
            qty_ = rng.randint(1, 20)
            evts = e.submit(limit(next_id, side, px_, qty_))
            if any(ev.type == ob.EventType.Ack for ev in evts):
                live.append(next_id)
            next_id += 1
        else:
            # Cancel a random live order
            idx = rng.randrange(len(live))
            cid = live[idx]
            live[idx] = live[-1]
            live.pop()
            e.cancel(cid)

        if next_id % 100 == 0:
            e.assert_invariants()

    e.assert_invariants()
