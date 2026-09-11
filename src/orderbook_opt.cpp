#include "orderbook_opt.h"
#include <algorithm>

// Price levels start with zero capacity, so the first pushes walk a
// 1->2->4->8 reallocation chain, copying 40-byte orders each time. That
// shows up as a p99 tail on add_order. Reserve once, on first use, so
// only levels that actually hold orders pay for it.
namespace {
constexpr size_t kInitialLevelCapacity = 128;
}

// ============================================================
// Constructor: set up the flat price-level arrays
// ============================================================
OrderBookOpt::OrderBookOpt(double min_price, double max_price, double tick_size)
    : min_price_(min_price)
    , tick_size_(tick_size)
    , order_count_(0)
{
    // Calculate how many price levels we need.
    // For min=900, max=1100, tick=1.0: (1100-900)/1 + 1 = 201 levels
    num_levels_ = static_cast<size_t>((max_price - min_price) / tick_size) + 1;

    // Pre-allocate the price level arrays.
    // Each level starts empty (no orders).
    bid_levels_.resize(num_levels_);
    ask_levels_.resize(num_levels_);

    // Initialize each level with its price
    for (size_t i = 0; i < num_levels_; i++) {
        double price = index_to_price(i);
        bid_levels_[i] = PriceLevel(price);
        ask_levels_[i] = PriceLevel(price);
    }

    // One bit per level, rounded up to whole 64-bit words.
    bid_occupied_.assign((num_levels_ + 63) / 64, 0ULL);
    ask_occupied_.assign((num_levels_ + 63) / 64, 0ULL);

    // No valid best bid or ask yet
    // SIZE_MAX is a sentinel meaning "no valid index"
    best_bid_idx_ = SIZE_MAX;
    best_ask_idx_ = SIZE_MAX;
}

// ============================================================
// Occupancy bitmap helpers
// ============================================================
void OrderBookOpt::set_occupied(std::vector<uint64_t>& bm, size_t idx) {
    bm[idx >> 6] |= (1ULL << (idx & 63));
}

void OrderBookOpt::clear_occupied(std::vector<uint64_t>& bm, size_t idx) {
    bm[idx >> 6] &= ~(1ULL << (idx & 63));
}

size_t OrderBookOpt::highest_occupied_at_or_below(
    const std::vector<uint64_t>& bm, size_t start) {
    size_t word = start >> 6;
    size_t bit = start & 63;

    // Mask away bits above `start` in the starting word.
    uint64_t w = bm[word] & ((bit == 63) ? ~0ULL : ((1ULL << (bit + 1)) - 1));
    while (true) {
        if (w) {
            // 63 - clz gives the index of the highest set bit.
            return (word << 6) + (63 - static_cast<size_t>(__builtin_clzll(w)));
        }
        if (word == 0) return SIZE_MAX;
        word--;
        w = bm[word];
    }
}

size_t OrderBookOpt::lowest_occupied_at_or_above(
    const std::vector<uint64_t>& bm, size_t start, size_t num_levels) {
    size_t word = start >> 6;
    if (word >= bm.size()) return SIZE_MAX;
    size_t bit = start & 63;

    // Mask away bits below `start` in the starting word.
    uint64_t w = bm[word] & (~0ULL << bit);
    while (true) {
        if (w) {
            size_t idx = (word << 6) + static_cast<size_t>(__builtin_ctzll(w));
            return (idx < num_levels) ? idx : SIZE_MAX;
        }
        word++;
        if (word >= bm.size()) return SIZE_MAX;
        w = bm[word];
    }
}

// ============================================================
// Convert price to array index
//
// Simple arithmetic: subtract the minimum price, divide by
// tick size. price=950, min=900, tick=1 → index 50.
//
// This is O(1) — no comparison, no tree traversal.
// ============================================================
size_t OrderBookOpt::price_to_index(double price) const {
    // Calculate the index, clamping to valid range
    int idx = static_cast<int>((price - min_price_) / tick_size_);
    if (idx < 0) return 0;
    if (static_cast<size_t>(idx) >= num_levels_) return num_levels_ - 1;
    return static_cast<size_t>(idx);
}

// ============================================================
// Convert array index back to price
// ============================================================
double OrderBookOpt::index_to_price(size_t index) const {
    return min_price_ + (static_cast<double>(index) * tick_size_);
}

// ============================================================
// Add a new order to the book
//
// First tries to match against the opposite side.
// If unmatched quantity remains and it's a limit order,
// rests it in the book at the appropriate price level.
// ============================================================
std::vector<Trade> OrderBookOpt::add_order(AlignedOrder order) {
    order.active = true;
    std::vector<Trade> trades = match_order(order);

    // If the order still has remaining quantity and it's a limit
    // order, add it to the book at its price level
    if (!order.is_done() && order.type == OrderType::Limit) {
        size_t level_idx = price_to_index(order.price);
        uint64_t id = order.id;
        Side side = order.side;

        if (side == Side::Buy) {
            // Get the price level for this bid
            PriceLevel& level = bid_levels_[level_idx];

            // Record where this order lives for O(1) cancel
            size_t pos = level.orders.size();
            order_locations_[id] = OrderLocation{
                .side = Side::Buy,
                .level_index = level_idx,
                .order_position = pos
            };

            // Push to the back of the vector (time priority)
            if (level.orders.capacity() == 0) {
                level.orders.reserve(kInitialLevelCapacity);
            }
            level.orders.push_back(order);
            level.active_count++;
            set_occupied(bid_occupied_, level_idx);

            // Update best bid if this price is better (higher)
            if (best_bid_idx_ == SIZE_MAX || level_idx > best_bid_idx_) {
                best_bid_idx_ = level_idx;
            }
        } else {
            PriceLevel& level = ask_levels_[level_idx];

            size_t pos = level.orders.size();
            order_locations_[id] = OrderLocation{
                .side = Side::Sell,
                .level_index = level_idx,
                .order_position = pos
            };

            if (level.orders.capacity() == 0) {
                level.orders.reserve(kInitialLevelCapacity);
            }
            level.orders.push_back(order);
            level.active_count++;
            set_occupied(ask_occupied_, level_idx);

            // Update best ask if this price is better (lower)
            if (best_ask_idx_ == SIZE_MAX || level_idx < best_ask_idx_) {
                best_ask_idx_ = level_idx;
            }
        }

        order_count_++;
    }

    return trades;
}

// ============================================================
// Match an incoming order against resting orders
//
// Buy orders match against asks (sell side), starting from
// the lowest ask price. Sell orders match against bids (buy
// side), starting from the highest bid price.
//
// The key optimization: instead of traversing a tree to find
// the best price, we use best_bid_idx_ / best_ask_idx_ —
// O(1) access to the best price level.
// ============================================================
std::vector<Trade> OrderBookOpt::match_order(AlignedOrder& incoming) {
    std::vector<Trade> trades;

    if (incoming.side == Side::Buy) {
        // Buy matches against asks, starting from best (lowest) ask
        while (best_ask_idx_ != SIZE_MAX && incoming.remaining() > 0) {
            PriceLevel& level = ask_levels_[best_ask_idx_];

            // For limit orders, check price compatibility
            if (incoming.type == OrderType::Limit &&
                incoming.price < level.price) {
                break;
            }

            // Scan from front_idx, skipping known-inactive orders.
            // This is the critical optimization: instead of scanning
            // from index 0 through thousands of dead entries, we
            // start from where active orders begin.
            while (level.front_idx < level.orders.size() &&
                   incoming.remaining() > 0) {

                AlignedOrder& resting = level.orders[level.front_idx];

                // Skip inactive orders and advance front_idx past them.
                // Once we advance past an inactive order, we never
                // look at it again — permanent skip, not per-match skip.
                if (resting.is_done()) {
                    level.front_idx++;
                    continue;
                }

                // Calculate fill quantity
                uint32_t fill_qty = std::min(
                    incoming.remaining(), resting.remaining()
                );

                // Record the trade
                trades.push_back(Trade{
                    .buy_order_id = incoming.id,
                    .sell_order_id = resting.id,
                    .price = level.price,
                    .quantity = fill_qty
                });

                // Update filled quantities
                incoming.filled += fill_qty;
                resting.filled += fill_qty;

                // If fully filled, mark inactive and advance front_idx
                if (resting.is_done()) {
                    resting.active = false;
                    level.active_count--;
                    order_locations_.erase(resting.id);
                    order_count_--;
                    level.front_idx++;
                }
            }

            // If this price level has no active orders, move to next
            if (level.active_count == 0) {
                clear_occupied(ask_occupied_, best_ask_idx_);
                update_best_ask();
            } else {
                break;
            }
        }
    } else {
        // Sell matches against bids, starting from best (highest) bid
        while (best_bid_idx_ != SIZE_MAX && incoming.remaining() > 0) {
            PriceLevel& level = bid_levels_[best_bid_idx_];

            if (incoming.type == OrderType::Limit &&
                incoming.price > level.price) {
                break;
            }

            while (level.front_idx < level.orders.size() &&
                   incoming.remaining() > 0) {

                AlignedOrder& resting = level.orders[level.front_idx];

                if (resting.is_done()) {
                    level.front_idx++;
                    continue;
                }

                uint32_t fill_qty = std::min(
                    incoming.remaining(), resting.remaining()
                );

                trades.push_back(Trade{
                    .buy_order_id = resting.id,
                    .sell_order_id = incoming.id,
                    .price = level.price,
                    .quantity = fill_qty
                });

                incoming.filled += fill_qty;
                resting.filled += fill_qty;

                if (resting.is_done()) {
                    resting.active = false;
                    level.active_count--;
                    order_locations_.erase(resting.id);
                    order_count_--;
                    level.front_idx++;
                }
            }

            if (level.active_count == 0) {
                clear_occupied(bid_occupied_, best_bid_idx_);
                update_best_bid();
            } else {
                break;
            }
        }
    }

    return trades;
}

// ============================================================
// Cancel an order by ID — O(1) lookup
//
// The order_locations_ map tells us exactly where the order
// lives: which side, which price level, which position in
// the vector. No scanning required.
// ============================================================
bool OrderBookOpt::cancel_order(uint64_t order_id) {
    auto it = order_locations_.find(order_id);
    if (it == order_locations_.end()) {
        return false;  // Order not found
    }

    OrderLocation loc = it->second;

    // Go directly to the order and mark it inactive
    if (loc.side == Side::Buy) {
        PriceLevel& level = bid_levels_[loc.level_index];

        // Bounds check — the position must be valid
        if (loc.order_position < level.orders.size()) {
            AlignedOrder& order = level.orders[loc.order_position];
            order.active = false;
            level.active_count--;
        }

        // The level must leave the occupancy map whenever it empties,
        // not only when it happened to be the best bid.
        if (level.active_count == 0) {
            clear_occupied(bid_occupied_, loc.level_index);
            if (loc.level_index == best_bid_idx_) {
                update_best_bid();
            }
        }
    } else {
        PriceLevel& level = ask_levels_[loc.level_index];

        if (loc.order_position < level.orders.size()) {
            AlignedOrder& order = level.orders[loc.order_position];
            order.active = false;
            level.active_count--;
        }

        if (level.active_count == 0) {
            clear_occupied(ask_occupied_, loc.level_index);
            if (loc.level_index == best_ask_idx_) {
                update_best_ask();
            }
        }
    }

    order_locations_.erase(it);
    order_count_--;
    return true;
}

// ============================================================
// Update best bid index — scan downward from current best
//
// Called when the best bid price level becomes empty.
// Scans downward (lower prices) to find the next non-empty level.
//
// This is O(k) where k is the gap between price levels.
// In practice, k is small (prices cluster tightly) so this
// is nearly O(1) amortized.
// ============================================================
void OrderBookOpt::update_best_bid() {
    // If current best is invalid, start from the top
    size_t start = (best_bid_idx_ == SIZE_MAX) ? num_levels_ - 1 : best_bid_idx_;

#ifdef USE_LINEAR_BEST_SCAN
    // Original linear scan, kept so the A/B benchmark can rebuild it.
    for (int i = static_cast<int>(start); i >= 0; i--) {
        if (bid_levels_[i].active_count > 0) {
            best_bid_idx_ = static_cast<size_t>(i);
            return;
        }
    }
    best_bid_idx_ = SIZE_MAX;
#else
    // Walk down the occupancy bits, 64 levels per word.
    best_bid_idx_ = highest_occupied_at_or_below(bid_occupied_, start);
#endif
}

// ============================================================
// Update best ask index — scan upward from current best
//
// Mirror of update_best_bid but scanning upward (higher prices).
// ============================================================
void OrderBookOpt::update_best_ask() {
    size_t start = (best_ask_idx_ == SIZE_MAX) ? 0 : best_ask_idx_;

#ifdef USE_LINEAR_BEST_SCAN
    for (size_t i = start; i < num_levels_; i++) {
        if (ask_levels_[i].active_count > 0) {
            best_ask_idx_ = i;
            return;
        }
    }
    best_ask_idx_ = SIZE_MAX;
#else
    // Walk up the occupancy bits, 64 levels per word.
    best_ask_idx_ = lowest_occupied_at_or_above(ask_occupied_, start, num_levels_);
#endif
}

// ============================================================
// Accessors
// ============================================================
double OrderBookOpt::best_bid() const {
    if (best_bid_idx_ == SIZE_MAX) return 0.0;
    return index_to_price(best_bid_idx_);
}

double OrderBookOpt::best_ask() const {
    if (best_ask_idx_ == SIZE_MAX) return 0.0;
    return index_to_price(best_ask_idx_);
}

size_t OrderBookOpt::order_count() const {
    return order_count_;
}

double OrderBookOpt::spread() const {
    double bid = best_bid();
    double ask = best_ask();
    if (bid == 0.0 || ask == 0.0) return 0.0;
    return ask - bid;
}