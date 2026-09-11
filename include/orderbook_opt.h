#pragma once

#include "order.h"
#include <vector>
#include <array>
#include <unordered_map>
#include <cstdint>
#include <limits>

// ============================================================
// OPTIMIZED ORDER BOOK
//
// Key differences from the naive version:
//
// 1. Price levels are stored in a flat array indexed by price,
//    not a tree. For a price range of 1000 ticks, that's a
//    1000-element array. Accessing the best bid/ask is O(1)
//    — just maintain a pointer to the current best.
//
// 2. Orders within a price level are stored in a std::vector
//    (contiguous memory) instead of std::list (scattered heap
//    nodes). Scanning orders is cache-friendly.
//
// 3. An unordered_map from order_id to (price, index) enables
//    O(1) cancel instead of linear scan.
//
// 4. The Order struct is cache-line aligned so that accessing
//    one order doesn't pull in parts of adjacent orders that
//    we don't need.
// ============================================================

// ------------------------------------------------------------
// Cache-line aligned Order struct.
//
// A cache line on x86 is 64 bytes. We want each Order to start
// at a 64-byte boundary so that reading one Order's fields
// doesn't cause a cache miss on a neighboring Order's cache line.
//
// alignas(64) tells the compiler: "when you allocate this struct,
// make sure its starting address is a multiple of 64."
//
// We also reorder fields so that the most frequently accessed
// fields (id, price, quantity, filled) are in the first 32 bytes
// — they'll always be in the same cache line together.
// ------------------------------------------------------------
struct AlignedOrder {
    // === HOT FIELDS (accessed during matching) ===
    // These are checked on every match attempt, so they must
    // be close together in memory.
    uint64_t id;          // 8 bytes  — offset 0
    double price;         // 8 bytes  — offset 8
    uint32_t quantity;    // 4 bytes  — offset 16
    uint32_t filled;      // 4 bytes  — offset 20
    Side side;            // 1 byte   — offset 24
    OrderType type;       // 1 byte   — offset 25
    bool active;          // 1 byte   — offset 26
    // 5 bytes padding    —            offset 27-31

    // === COLD FIELDS (rarely accessed during hot path) ===
    uint64_t timestamp;   // 8 bytes  — offset 32
    // 24 bytes padding to fill the 64-byte cache line

    // Remaining shares waiting to be filled
    uint32_t remaining() const {
        return quantity - filled;
    }

    // Is this order completely filled or cancelled?
    bool is_done() const {
        return !active || filled >= quantity;
    }
};

// ------------------------------------------------------------
// A price level: all orders resting at one specific price.
//
// Orders are stored in a vector for cache-friendly iteration.
// New orders are pushed to the back (time priority).
// Filled/cancelled orders are marked inactive but not removed
// immediately — they're cleaned up lazily to avoid shifting
// the entire vector on every fill.
// ------------------------------------------------------------
struct PriceLevel {
    double price;
    std::vector<AlignedOrder> orders;
    uint32_t active_count;  // Number of non-done orders
    size_t front_idx;       // Index of the first potentially active order
                            // Orders before this index are guaranteed inactive.
                            // This avoids scanning thousands of dead orders
                            // at the front of the vector during matching.

    PriceLevel() : price(0.0), active_count(0), front_idx(0) {}
    explicit PriceLevel(double p) : price(p), active_count(0), front_idx(0) {}
};

// ------------------------------------------------------------
// The optimized order book.
// ------------------------------------------------------------
class OrderBookOpt {
public:
    // Constructor takes a price range: the minimum and maximum
    // prices the book will handle, and the tick size (minimum
    // price increment). This determines the size of the flat
    // price-level array.
    //
    // For example: min=900, max=1100, tick=1.0 gives 200 price
    // levels stored in a 200-element array. Accessing any price
    // is O(1) — just compute the array index.
    OrderBookOpt(double min_price, double max_price, double tick_size);

    // Add a new order. Returns any trades that resulted.
    std::vector<Trade> add_order(AlignedOrder order);

    // Cancel an existing order by ID. Returns true if found.
    bool cancel_order(uint64_t order_id);

    // Best bid (highest buy price with active orders). 0 if none.
    double best_bid() const;

    // Best ask (lowest sell price with active orders). 0 if none.
    double best_ask() const;

    // Total active orders in the book
    size_t order_count() const;

    // Spread between best ask and best bid
    double spread() const;

private:
    // Convert a price to an index in our flat array.
    // price=900, min=900, tick=1.0 → index 0
    // price=901, min=900, tick=1.0 → index 1
    // price=1050, min=900, tick=1.0 → index 150
    size_t price_to_index(double price) const;

    // Convert an array index back to a price
    double index_to_price(size_t index) const;

    // Match an incoming order against the opposite side
    std::vector<Trade> match_order(AlignedOrder& order);

    // Update best_bid_index_ after a change to the bid side
    void update_best_bid();

    // Update best_ask_index_ after a change to the ask side
    void update_best_ask();

    // === Level occupancy summary ===
    // Bit i is set when level i holds at least one active order.
    // The flat array gives O(1) access to a KNOWN price, but finding
    // the next best occupied level meant walking every level in
    // between — which is why deep-book matching lost to the naive
    // std::map, where the best price is just begin(). Scanning 64
    // levels per word instead makes that step O(levels/64).
    std::vector<uint64_t> bid_occupied_;
    std::vector<uint64_t> ask_occupied_;

    static void set_occupied(std::vector<uint64_t>& bm, size_t idx);
    static void clear_occupied(std::vector<uint64_t>& bm, size_t idx);

    // Both return SIZE_MAX when no occupied level exists that way.
    static size_t highest_occupied_at_or_below(
        const std::vector<uint64_t>& bm, size_t start);
    static size_t lowest_occupied_at_or_above(
        const std::vector<uint64_t>& bm, size_t start, size_t num_levels);

    // === The flat price-level arrays ===
    // One vector for bids, one for asks.
    // Each element is a PriceLevel at that price index.
    // Most elements will be empty (no orders at that price)
    // — that's fine, an empty PriceLevel is tiny.
    std::vector<PriceLevel> bid_levels_;
    std::vector<PriceLevel> ask_levels_;

    // Track the current best prices by their array indices.
    // This gives O(1) best_bid() and best_ask() — no tree
    // traversal, no scanning.
    //
    // SIZE_MAX means "no valid best" (empty side).
    size_t best_bid_idx_;
    size_t best_ask_idx_;

    // O(1) cancel lookup: order_id → (side, price_index, position in vector)
    struct OrderLocation {
        Side side;
        size_t level_index;     // Index in bid_levels_ or ask_levels_
        size_t order_position;  // Index within the PriceLevel's orders vector
    };
    std::unordered_map<uint64_t, OrderLocation> order_locations_;

    // Price range configuration
    double min_price_;
    double tick_size_;
    size_t num_levels_;

    // Total active order count
    size_t order_count_;
};