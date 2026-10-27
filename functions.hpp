#pragma once
#include "types.hpp"

#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Symbols included in snapshots.
inline const std::vector<std::string> kSymbols = {"BTCUSDT", "ETHUSDT"};

class MatchingEngine
{
public:
    MatchingEngine();

    // Parse one JSON request and return its response lines.
    std::vector<std::string> handle_line(const std::string& line);

    // Order operations
    std::vector<std::string> process_new_order(const Order& order);
    std::vector<std::string> process_cancel(const std::string& trader, int order_id);
    std::vector<std::string> process_modify(const std::string& trader, int order_id,
                                            int new_qty, int new_price);

    // Return a snapshot of the top price levels. 
    std::vector<BookSnapshot> snapshot(std::size_t depth = 3) const;

private:
    using Index = std::map<OrderKey, OrderLocation>;

    std::vector<Trade> match(Order& aggressor);
    template <typename Levels>
    void match_against(Levels& levels, Order& aggressor, std::vector<Trade>& trades,
                       std::map<std::pair<std::string, int>, std::size_t>& merged);
    void rest(const Order& order);
    void remove(Index::iterator pos);

    std::unordered_map<std::string, OrderBook> books_;
    Index index_;               // one index for all symbols: cancel/modify do not know the symbol
    mutable std::mutex mutex_;  // Protects the order books and index.
};

// Output formatting (exact format from the specification)
std::string format_trade(const Trade& t);
std::string format_snapshot(const BookSnapshot& s, const std::string& time);
