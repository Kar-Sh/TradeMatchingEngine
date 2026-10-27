#pragma once
#include <functional>
#include <list>
#include <map>
#include <string>
#include <vector>

enum class Side
{
    Buy,
    Sell
};

struct Order
{
    int qty;
    std::string trader;
    int order_id;
    std::string symbol;
    Side side;
    int price;
    int remaining_qty;
};


struct OrderKey
{
    std::string trader;
    int order_id;

    bool operator<(const OrderKey& other) const
    {
        if (trader != other.trader)
            return trader < other.trader;
        return order_id < other.order_id;
    }
};

using OrderList = std::list<Order>;

struct OrderBook
{
    // Best prices
    std::map<int, OrderList, std::greater<int>> buy_orders;
    std::map<int, OrderList> sell_orders;
};

struct OrderLocation
{
    std::string symbol;
    Side side;
    int price;
    OrderList::iterator it;
};

struct Trade
{
    std::string symbol;
    std::string buyer;
    std::string seller;
    int price;
    int qty;
};

struct LevelSnapshot
{
    int price;
    long long volume;
};

struct BookSnapshot
{
    std::string symbol;
    std::vector<LevelSnapshot> bids;
    std::vector<LevelSnapshot> asks;
};

