#include "functions.hpp"

#include <algorithm>
#include <climits>
#include <iterator>
#include <optional>
#include <utility>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

using json = nlohmann::json;

namespace
{
using Fields = std::vector<std::pair<std::string, json>>;

std::string format_line(const Fields& fields)
{
    std::string out = "{";
    for (std::size_t i = 0; i < fields.size(); ++i)
    {
        if (i > 0)
            out += ", ";
        out += json(fields[i].first).dump() + ": " + fields[i].second.dump();
    }
    return out + "}";
}


std::string ack(const std::string& action, const std::string& trader, int order_id)
{
    return format_line({{"event",  "ack"},
                        {"action", action},
                        {"trader", trader},
                        {"order_id", order_id},
                        {"status", "success"}});
}

std::string reject(const std::string& action, const std::string& trader, int order_id,
                   const std::string& reason)
{
    return format_line({{"event", "reject"},
                        {"action", action},
                        {"trader", trader},
                        {"order_id", order_id},
                        {"reason", reason}});
}


std::optional<std::string> get_string(const json& j, const char* key)
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_string())
        return std::nullopt;
    return it->get<std::string>();
}

std::optional<int> get_int(const json& j, const char* key)
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer())
        return std::nullopt;
    if (it->is_number_unsigned())
    {
        auto v = it->get<unsigned long long>();
        if (v > static_cast<unsigned long long>(INT_MAX))
            return std::nullopt;
        return static_cast<int>(v);
    }
    auto v = it->get<long long>();
    if (v < INT_MIN || v > INT_MAX)
        return std::nullopt;
    return static_cast<int>(v);
}
} // namespace

std::string format_trade(const Trade& t)
{
    return format_line({{"event", "trade"},
                        {"symbol", t.symbol},
                        {"buyer", t.buyer},
                        {"seller", t.seller},
                        {"price", t.price},
                        {"qty", t.qty}});
}

std::string format_snapshot(const BookSnapshot& s, const std::string& time)
{
    auto levels = [](const std::vector<LevelSnapshot>& v) {
        json arr = json::array();
        for (const auto& l : v)
            arr.push_back(json::object({{"price", l.price}, {"volume", l.volume}}));
        return arr;
    };
    return format_line({{"time", time}, {"symbol", s.symbol}, {"bids", levels(s.bids)},
                        {"asks", levels(s.asks)}});
}

MatchingEngine::MatchingEngine()
{
    for (const auto& symbol : kSymbols)
        books_[symbol] = OrderBook{};
}

// --------------------------PARSING-------------------------------- //

std::vector<std::string> MatchingEngine::handle_line(const std::string& line)
{
    json j;
    try
    {
        j = json::parse(line);
    }
    catch (const json::parse_error& e)
    {
        spdlog::error("Malformed JSON: {} | input: {}", e.what(), line);
        return {format_line({{"event", "reject"}, {"reason", "Malformed JSON"}})};
    }
    if (!j.is_object())
    {
        spdlog::error("JSON is not an object | input: {}", line);
        return {format_line({{"event", "reject"}, {"reason", "Malformed JSON"}})};
    }

    // Include the request fields that were parsed successfully.
    Fields known = {{"event", "reject"}};
    auto fail = [&](const std::string& reason) {
        spdlog::warn("Rejected request: {} | input: {}", reason, line);
        Fields f = known;
        f.emplace_back("reason", reason);
        return std::vector<std::string>{format_line(f)};
    };
   
    auto action = get_string(j, "action");
    if (!action)
        return fail("Missing or invalid field: action");
    known.emplace_back("action", *action);
    if (*action != "new" && *action != "cancel" && *action != "modify")
        return fail("Unknown action");

    auto trader = get_string(j, "trader");
    if (!trader)
        return fail("Missing or invalid field: trader");
    known.emplace_back("trader", *trader);

    auto order_id = get_int(j, "order_id");
    if (!order_id)
        return fail("Missing or invalid field: order_id");
    known.emplace_back("order_id", *order_id);

    if (*action == "cancel")
        return process_cancel(*trader, *order_id);

    if (*action == "modify")
    {
        auto new_qty = get_int(j, "new_qty");
        if (!new_qty)
            return fail("Missing or invalid field: new_qty");
        auto new_price = get_int(j, "new_price");
        if (!new_price)
            return fail("Missing or invalid field: new_price");
        return process_modify(*trader, *order_id, *new_qty, *new_price);
    }

    // action == "new"
    auto symbol = get_string(j, "symbol");
    if (!symbol)
        return fail("Missing or invalid field: symbol");
    auto side = get_string(j, "side");
    if (!side)
        return fail("Missing or invalid field: side");
    if (*side != "B" && *side != "S")
        return fail("Invalid side");
    auto qty = get_int(j, "qty");
    if (!qty)
        return fail("Missing or invalid field: qty");
    auto price = get_int(j, "price");
    if (!price)
        return fail("Missing or invalid field: price");

    Order order;
    order.trader = *trader;
    order.order_id = *order_id;
    order.symbol = *symbol;
    order.side = (*side == "B") ? Side::Buy : Side::Sell;
    order.qty = *qty;
    order.price = *price;
    order.remaining_qty = *qty;
    return process_new_order(order);
}

// -----------------------------LIFECYCLE-------------------------------- //

std::vector<std::string> MatchingEngine::process_new_order(const Order& order)
{
    if (books_.find(order.symbol) == books_.end())
        return {reject("new", order.trader, order.order_id, "Invalid symbol")};
    if (order.qty <= 0)
        return {reject("new", order.trader, order.order_id, "Invalid quantity")};
    if (order.price <= 0)
        return {reject("new", order.trader, order.order_id, "Invalid price")};

    std::lock_guard<std::mutex> lock(mutex_);

    if (index_.count(OrderKey{order.trader, order.order_id}))
        return {reject("new", order.trader, order.order_id, "Duplicate order ID")};

    std::vector<std::string> out;
    out.push_back(ack("new", order.trader, order.order_id));  // ack before trades (rule 5)

    Order aggressor = order;
    aggressor.remaining_qty = order.qty;
    auto trades = match(aggressor);
    if (aggressor.remaining_qty > 0)
        rest(aggressor);

    for (const auto& t : trades)
        out.push_back(format_trade(t));
    return out;
}

std::vector<std::string> MatchingEngine::process_cancel(const std::string& trader, int order_id)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto pos = index_.find(OrderKey{trader, order_id});
    if (pos == index_.end())
        return {reject("cancel", trader, order_id, "Order not found")};

    remove(pos);
    return {ack("cancel", trader, order_id)};
}

std::vector<std::string> MatchingEngine::process_modify(const std::string& trader, int order_id,
                                                        int new_qty, int new_price)
{
    if (new_qty <= 0)
        return {reject("modify", trader, order_id, "Invalid quantity")};
    if (new_price <= 0)
        return {reject("modify", trader, order_id, "Invalid price")};

    std::lock_guard<std::mutex> lock(mutex_);

    auto pos = index_.find(OrderKey{trader, order_id});
    if (pos == index_.end())
        return {reject("modify", trader, order_id, "Order not found")};

    Order& resting = *pos->second.it;

    // A quantity decrease at the same price keeps the order priority.
    if (new_price == resting.price && new_qty <= resting.remaining_qty)
    {
        resting.remaining_qty = new_qty;
        return {ack("modify", trader, order_id)};
    }

    // A price change or quantity increase sends the order to the back of the queue.
    Order updated = resting;
    remove(pos);
    updated.price = new_price;
    updated.qty = new_qty;
    updated.remaining_qty = new_qty;

    std::vector<std::string> out;
    out.push_back(ack("modify", trader, order_id));
    auto trades = match(updated);
    if (updated.remaining_qty > 0)
        rest(updated);
    for (const auto& t : trades)
        out.push_back(format_trade(t));
    return out;
}

// -------------------------MATCHING-------------------------------------//

std::vector<Trade> MatchingEngine::match(Order& aggressor)
{
    std::vector<Trade> trades;
    // Combine fills against the same trader at the same price.
    std::map<std::pair<std::string, int>, std::size_t> merged;

    OrderBook& book = books_[aggressor.symbol];
    if (aggressor.side == Side::Buy)
        match_against(book.sell_orders, aggressor, trades, merged);
    else
        match_against(book.buy_orders, aggressor, trades, merged);
    return trades;
}

template <typename Levels>
void MatchingEngine::match_against(Levels& levels, Order& aggressor, std::vector<Trade>& trades,
                                   std::map<std::pair<std::string, int>, std::size_t>& merged)
{
    auto crosses = [&](int resting_price) {
        return aggressor.side == Side::Buy ? aggressor.price >= resting_price
                                           : aggressor.price <= resting_price;
    };

    while (aggressor.remaining_qty > 0 && !levels.empty())
    {
        auto level = levels.begin();  // The first level is the best available price.
        if (!crosses(level->first))
            break;

        OrderList& queue = level->second;
        while (aggressor.remaining_qty > 0 && !queue.empty())
        {
            Order& resting = queue.front();  // Orders at one price are matched in arrival order.
            int fill = std::min(aggressor.remaining_qty, resting.remaining_qty);
            aggressor.remaining_qty -= fill;
            resting.remaining_qty -= fill;

            auto key = std::make_pair(resting.trader, level->first);
            auto found = merged.find(key);
            if (found != merged.end())
            {
                trades[found->second].qty += fill;
            }
            else
            {
                Trade t;
                t.symbol = aggressor.symbol;
                t.buyer = aggressor.side == Side::Buy ? aggressor.trader : resting.trader;
                t.seller = aggressor.side == Side::Buy ? resting.trader : aggressor.trader;
                t.price = level->first;  // Trades use the resting order price.
                t.qty = fill;
                merged.emplace(key, trades.size());
                trades.push_back(t);
            }

            if (resting.remaining_qty == 0)
            {
                index_.erase(OrderKey{resting.trader, resting.order_id});
                queue.pop_front();
            }
        }
        if (queue.empty())
            levels.erase(level);
    }
}

void MatchingEngine::rest(const Order& order)
{
    OrderBook& book = books_[order.symbol];
    OrderList& queue = (order.side == Side::Buy) ? book.buy_orders[order.price]
                                                 : book.sell_orders[order.price];
    queue.push_back(order);
    index_[OrderKey{order.trader, order.order_id}] =
        OrderLocation{order.symbol, order.side, order.price, std::prev(queue.end())};
}

void MatchingEngine::remove(Index::iterator pos)
{
    const OrderLocation& loc = pos->second;
    OrderBook& book = books_[loc.symbol];

    // Find the existing level so we do not create an empty one by accident.
    if (loc.side == Side::Buy)
    {
        auto level = book.buy_orders.find(loc.price);
        level->second.erase(loc.it);
        if (level->second.empty())
            book.buy_orders.erase(level);
    }
    else
    {
        auto level = book.sell_orders.find(loc.price);
        level->second.erase(loc.it);
        if (level->second.empty())
            book.sell_orders.erase(level);
    }
    index_.erase(pos);
}

// --------------------------TELEMENTRY----------------------------- //

std::vector<BookSnapshot> MatchingEngine::snapshot(std::size_t depth) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto top = [depth](const auto& levels) {
        std::vector<LevelSnapshot> result;
        for (auto it = levels.begin(); it != levels.end() && result.size() < depth; ++it)
        {
            long long volume = 0;
            for (const Order& o : it->second)
                volume += o.remaining_qty;
            result.push_back({it->first, volume});
        }
        return result;
    };

    std::vector<BookSnapshot> result;
    for (const auto& symbol : kSymbols)
    {
        const OrderBook& book = books_.at(symbol);
        result.push_back({symbol, top(book.buy_orders), top(book.sell_orders)});
    }
    return result;
}
