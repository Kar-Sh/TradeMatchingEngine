#include <gtest/gtest.h>

#include <atomic>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "functions.hpp"

namespace
{
using Lines = std::vector<std::string>;

Lines run(MatchingEngine& engine, const Lines& input)
{
    Lines out;
    for (const auto& line : input)
        for (auto& o : engine.handle_line(line))
            out.push_back(o);
    return out;
}

std::string new_order(const std::string& trader, int id, const std::string& side, int qty,
                      int price, const std::string& symbol = "BTCUSDT")
{
    return "{\"action\": \"new\", \"trader\": \"" + trader + "\", \"order_id\": " +
           std::to_string(id) + ", \"symbol\": \"" + symbol + "\", \"side\": \"" + side +
           "\", \"qty\": " + std::to_string(qty) + ", \"price\": " + std::to_string(price) + "}";
}

std::string cancel(const std::string& trader, int id)
{
    return "{\"action\": \"cancel\", \"trader\": \"" + trader + "\", \"order_id\": " +
           std::to_string(id) + "}";
}

std::string modify(const std::string& trader, int id, int qty, int price)
{
    return "{\"action\": \"modify\", \"trader\": \"" + trader + "\", \"order_id\": " +
           std::to_string(id) + ", \"new_qty\": " + std::to_string(qty) +
           ", \"new_price\": " + std::to_string(price) + "}";
}

std::string ack(const std::string& action, const std::string& trader, int id)
{
    return "{\"event\": \"ack\", \"action\": \"" + action + "\", \"trader\": \"" + trader +
           "\", \"order_id\": " + std::to_string(id) + ", \"status\": \"success\"}";
}

std::string rej(const std::string& action, const std::string& trader, int id,
                const std::string& reason)
{
    return "{\"event\": \"reject\", \"action\": \"" + action + "\", \"trader\": \"" + trader +
           "\", \"order_id\": " + std::to_string(id) + ", \"reason\": \"" + reason + "\"}";
}

std::string trade(const std::string& buyer, const std::string& seller, int price, int qty,
                  const std::string& symbol = "BTCUSDT")
{
    return "{\"event\": \"trade\", \"symbol\": \"" + symbol + "\", \"buyer\": \"" + buyer +
           "\", \"seller\": \"" + seller + "\", \"price\": " + std::to_string(price) +
           ", \"qty\": " + std::to_string(qty) + "}";
}

const BookSnapshot& book(const std::vector<BookSnapshot>& s, const std::string& symbol)
{
    for (const auto& b : s)
        if (b.symbol == symbol)
            return b;
    throw std::runtime_error("symbol not found");
}
} // namespace

// ---------------------------SPECIFICATION EXAMPLE---------------------------------//

TEST(Specification, ExampleFromTaskProducesExactOutput)
{
    MatchingEngine e;
    Lines out = run(e, {
        new_order("T1", 1, "B", 4, 31),
        new_order("T2", 2, "B", 6, 32),
        new_order("T1", 3, "B", 3, 32),
        new_order("T2", 4, "B", 5, 32),
        cancel("T2", 4),
        cancel("T2", 4),
        new_order("T1", 5, "B", 2, 32),
        modify("T2", 2, 8, 32),
        modify("T1", 3, 2, 32),
        new_order("T4", 6, "S", 14, 30),
    });

    Lines expected = {
        ack("new", "T1", 1),
        ack("new", "T2", 2),
        ack("new", "T1", 3),
        ack("new", "T2", 4),
        ack("cancel", "T2", 4),
        rej("cancel", "T2", 4, "Order not found"),
        ack("new", "T1", 5),
        ack("modify", "T2", 2),
        ack("modify", "T1", 3),
        ack("new", "T4", 6),
        trade("T1", "T4", 32, 4),
        trade("T2", "T4", 32, 8),
        trade("T1", "T4", 31, 2),
    };
    EXPECT_EQ(out, expected);
}

// ---------------------------MATCHING-------------------------------//

TEST(Matching, NoCrossRestsInBook)
{
    MatchingEngine e;
    Lines out = run(e, {new_order("A", 1, "B", 5, 10), new_order("B", 1, "S", 5, 11)});
    EXPECT_EQ(out, (Lines{ack("new", "A", 1), ack("new", "B", 1)}));

    auto s = book(e.snapshot(), "BTCUSDT");
    ASSERT_EQ(s.bids.size(), 1u);
    EXPECT_EQ(s.bids[0].price, 10);
    ASSERT_EQ(s.asks.size(), 1u);
    EXPECT_EQ(s.asks[0].price, 11);
}

TEST(Matching, TradeExecutesAtRestingPrice)
{
    MatchingEngine e;
    Lines out = run(e, {new_order("S1", 1, "S", 5, 100), new_order("B1", 1, "B", 5, 120)});
    EXPECT_EQ(out.back(), trade("B1", "S1", 100, 5));

    out = run(e, {new_order("B2", 2, "B", 3, 90), new_order("S2", 2, "S", 3, 80)});
    EXPECT_EQ(out.back(), trade("B2", "S2", 90, 3));
}

TEST(Matching, BestPriceFirstForBuyAggressor)
{
    MatchingEngine e;
    Lines out = run(e, {new_order("S1", 1, "S", 1, 105), new_order("S2", 1, "S", 1, 101),
                        new_order("S3", 1, "S", 1, 103), new_order("B", 1, "B", 2, 110)});
    EXPECT_EQ(out[4], trade("B", "S2", 101, 1));
    EXPECT_EQ(out[5], trade("B", "S3", 103, 1));
    EXPECT_EQ(out.size(), 6u);
}

TEST(Matching, BestPriceFirstForSellAggressor)
{
    MatchingEngine e;
    Lines out = run(e, {new_order("B1", 1, "B", 1, 95), new_order("B2", 1, "B", 1, 99),
                        new_order("B3", 1, "B", 1, 97), new_order("S", 1, "S", 2, 90)});
    EXPECT_EQ(out[4], trade("B2", "S", 99, 1));
    EXPECT_EQ(out[5], trade("B3", "S", 97, 1));
}

TEST(Matching, FifoWithinPriceLevel)
{
    MatchingEngine e;
    Lines out = run(e, {new_order("S1", 1, "S", 2, 100), new_order("S2", 1, "S", 2, 100),
                        new_order("B", 1, "B", 3, 100)});
    EXPECT_EQ(out[3], trade("B", "S1", 100, 2));
    EXPECT_EQ(out[4], trade("B", "S2", 100, 1));
}

TEST(Matching, PartialFillOfRestingOrderKeepsRemainder)
{
    MatchingEngine e;
    run(e, {new_order("S1", 1, "S", 10, 100), new_order("B", 1, "B", 4, 100)});
    auto s = book(e.snapshot(), "BTCUSDT");
    ASSERT_EQ(s.asks.size(), 1u);
    EXPECT_EQ(s.asks[0].volume, 6);
    EXPECT_TRUE(s.bids.empty());
}

TEST(Matching, PartialFillOfAggressorRestsRemainder)
{
    MatchingEngine e;
    run(e, {new_order("S1", 1, "S", 3, 100), new_order("B", 1, "B", 10, 101)});
    auto s = book(e.snapshot(), "BTCUSDT");
    EXPECT_TRUE(s.asks.empty());
    ASSERT_EQ(s.bids.size(), 1u);
    EXPECT_EQ(s.bids[0].price, 101);
    EXPECT_EQ(s.bids[0].volume, 7);
}

TEST(Matching, SweepsSeveralLevelsAndStopsAtLimit)
{
    MatchingEngine e;
    Lines out = run(e, {new_order("S1", 1, "S", 2, 100), new_order("S2", 1, "S", 2, 101),
                        new_order("S3", 1, "S", 2, 102), new_order("B", 1, "B", 10, 101)});
    EXPECT_EQ(out.size(), 6u);
    auto s = book(e.snapshot(), "BTCUSDT");
    ASSERT_EQ(s.asks.size(), 1u);
    EXPECT_EQ(s.asks[0].price, 102);
    ASSERT_EQ(s.bids.size(), 1u);
    EXPECT_EQ(s.bids[0].volume, 6);
}

TEST(Matching, FillsOfSameTraderAndPriceAreAggregated)
{
    MatchingEngine e;
    Lines out = run(e, {new_order("S1", 1, "S", 1, 100), new_order("S2", 2, "S", 1, 100),
                        new_order("S1", 3, "S", 1, 100), new_order("S1", 4, "S", 1, 101),
                        new_order("B", 1, "B", 4, 101)});
    Lines trades(out.begin() + 5, out.end());
    EXPECT_EQ(trades, (Lines{trade("B", "S1", 100, 2), trade("B", "S2", 100, 1),
                             trade("B", "S1", 101, 1)}));
}

TEST(Matching, AckIsPrintedBeforeTrades)
{
    MatchingEngine e;
    Lines out = run(e, {new_order("S1", 1, "S", 1, 100), new_order("B", 7, "B", 1, 100)});
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[1], ack("new", "B", 7));
    EXPECT_EQ(out[2], trade("B", "S1", 100, 1));
}

TEST(Matching, SymbolsAreIndependent)
{
    MatchingEngine e;
    Lines out = run(e, {new_order("S1", 1, "S", 1, 100, "ETHUSDT"),
                        new_order("B", 1, "B", 1, 100, "BTCUSDT")});
    EXPECT_EQ(out.size(), 2u);  // no trade across symbols
}

// ------------------------------------------------------------ identifiers

TEST(Identifiers, DuplicateActiveOrderIsRejected)
{
    MatchingEngine e;
    Lines out = run(e, {new_order("T1", 1, "B", 1, 10), new_order("T1", 1, "B", 1, 11)});
    EXPECT_EQ(out[1], rej("new", "T1", 1, "Duplicate order ID"));
}

TEST(Identifiers, DuplicateCheckIsAcrossSymbols)
{
    MatchingEngine e;
    Lines out = run(e, {new_order("T1", 1, "B", 1, 10, "BTCUSDT"),
                        new_order("T1", 1, "B", 1, 10, "ETHUSDT")});
    EXPECT_EQ(out[1], rej("new", "T1", 1, "Duplicate order ID"));
}

TEST(Identifiers, DifferentTradersMayUseSameId)
{
    MatchingEngine e;
    Lines out = run(e, {new_order("T1", 1, "B", 1, 10), new_order("T2", 1, "B", 1, 10),
                        cancel("T1", 1)});
    EXPECT_EQ(out, (Lines{ack("new", "T1", 1), ack("new", "T2", 1), ack("cancel", "T1", 1)}));
    EXPECT_EQ(book(e.snapshot(), "BTCUSDT").bids[0].volume, 1);
}

TEST(Identifiers, IdCanBeReusedAfterFullExecution)
{
    MatchingEngine e;
    Lines out = run(e, {new_order("T1", 1, "B", 1, 10), new_order("T2", 1, "S", 1, 10),
                        new_order("T1", 1, "B", 1, 10)});
    EXPECT_EQ(out.back(), ack("new", "T1", 1));
}

// -----------------------------CANCEL------------------------------//

TEST(Cancel, RemovesOrderAndEmptyLevel)
{
    MatchingEngine e;
    run(e, {new_order("T1", 1, "S", 5, 100), cancel("T1", 1)});
    EXPECT_TRUE(book(e.snapshot(), "BTCUSDT").asks.empty());

    Lines out = run(e, {new_order("B", 1, "B", 5, 100)});
    EXPECT_EQ(out.size(), 1u);  // nothing left to match
}

TEST(Cancel, UnknownOrderIsRejected)
{
    MatchingEngine e;
    EXPECT_EQ(run(e, {cancel("T1", 999)}), (Lines{rej("cancel", "T1", 999, "Order not found")}));
}

TEST(Cancel, FullyExecutedOrderIsRejected)
{
    MatchingEngine e;
    Lines out = run(e, {new_order("T1", 1, "S", 1, 10), new_order("T2", 1, "B", 1, 10),
                        cancel("T1", 1)});
    EXPECT_EQ(out.back(), rej("cancel", "T1", 1, "Order not found"));
}

TEST(Cancel, PartiallyExecutedOrderCanBeCancelled)
{
    MatchingEngine e;
    Lines out = run(e, {new_order("T1", 1, "S", 5, 10), new_order("T2", 1, "B", 2, 10),
                        cancel("T1", 1)});
    EXPECT_EQ(out.back(), ack("cancel", "T1", 1));
    EXPECT_TRUE(book(e.snapshot(), "BTCUSDT").asks.empty());
}

TEST(Cancel, MiddleOfQueueKeepsOthersInOrder)
{
    MatchingEngine e;
    Lines out = run(e, {new_order("S1", 1, "S", 1, 10), new_order("S2", 1, "S", 1, 10),
                        new_order("S3", 1, "S", 1, 10), cancel("S2", 1),
                        new_order("B", 1, "B", 2, 10)});
    EXPECT_EQ(out[5], trade("B", "S1", 10, 1));
    EXPECT_EQ(out[6], trade("B", "S3", 10, 1));
}

// ---------------------------MODIFY---------------------------------//

TEST(Modify, QuantityDecreaseKeepsPriority)
{
    MatchingEngine e;
    Lines out = run(e, {new_order("S1", 1, "S", 5, 10), new_order("S2", 1, "S", 5, 10),
                        modify("S1", 1, 2, 10), new_order("B", 1, "B", 3, 10)});
    EXPECT_EQ(out[2], ack("modify", "S1", 1));
    EXPECT_EQ(out[4], trade("B", "S1", 10, 2));
    EXPECT_EQ(out[5], trade("B", "S2", 10, 1));
}

TEST(Modify, QuantityIncreaseLosesPriority)
{
    MatchingEngine e;
    Lines out = run(e, {new_order("S1", 1, "S", 1, 10), new_order("S2", 1, "S", 1, 10),
                        modify("S1", 1, 3, 10), new_order("B", 1, "B", 2, 10)});
    EXPECT_EQ(out[4], trade("B", "S2", 10, 1));
    EXPECT_EQ(out[5], trade("B", "S1", 10, 1));
}

TEST(Modify, PriceChangeLosesPriorityAndMovesLevel)
{
    MatchingEngine e;
    run(e, {new_order("B1", 1, "B", 4, 10), new_order("B2", 1, "B", 1, 11),
            modify("B1", 1, 4, 11)});
    auto s = book(e.snapshot(), "BTCUSDT");
    ASSERT_EQ(s.bids.size(), 1u);
    EXPECT_EQ(s.bids[0].price, 11);
    EXPECT_EQ(s.bids[0].volume, 5);

    Lines out = run(e, {new_order("S", 1, "S", 2, 11)});
    EXPECT_EQ(out[1], trade("B2", "S", 11, 1));
    EXPECT_EQ(out[2], trade("B1", "S", 11, 1));
}


TEST(Modify, PriceChangeThatCrossesTradesAfterAck)
{
    MatchingEngine e;
    Lines out = run(e, {new_order("S1", 1, "S", 2, 100), new_order("B1", 1, "B", 2, 90),
                        modify("B1", 1, 2, 100)});
    EXPECT_EQ(out[2], ack("modify", "B1", 1));
    EXPECT_EQ(out[3], trade("B1", "S1", 100, 2));
    auto s = book(e.snapshot(), "BTCUSDT");
    EXPECT_TRUE(s.bids.empty());
    EXPECT_TRUE(s.asks.empty());
}

TEST(Modify, UnknownOrderIsRejected)
{
    MatchingEngine e;
    EXPECT_EQ(run(e, {modify("T1", 5, 1, 10)}), (Lines{rej("modify", "T1", 5, "Order not found")}));
}

TEST(Modify, NonPositiveQuantityIsRejected)
{
    MatchingEngine e;
    Lines out = run(e, {new_order("T1", 1, "B", 5, 10), modify("T1", 1, 0, 10)});
    EXPECT_EQ(out[1], rej("modify", "T1", 1, "Invalid quantity"));
    EXPECT_EQ(book(e.snapshot(), "BTCUSDT").bids[0].volume, 5);
}

TEST(Modify, DecreaseAppliesToRemainingQuantity)
{
    MatchingEngine e;
    run(e, {new_order("S1", 1, "S", 10, 10), new_order("B", 1, "B", 6, 10),  // 4 left
            modify("S1", 1, 3, 10)});
    EXPECT_EQ(book(e.snapshot(), "BTCUSDT").asks[0].volume, 3);
}

// -------------------------FAULT TOLERANCE-------------------------------//

TEST(Validation, MalformedJsonIsRejectedAndEngineContinues)
{
    MatchingEngine e;
    Lines out = run(e, {"{not json", new_order("T1", 1, "B", 1, 10)});
    EXPECT_EQ(out[0], "{\"event\": \"reject\", \"reason\": \"Malformed JSON\"}");
    EXPECT_EQ(out[1], ack("new", "T1", 1));
}

TEST(Validation, InvalidFieldsAreRejected)
{
    MatchingEngine e;
    EXPECT_EQ(run(e, {new_order("T1", 1, "B", 1, 10, "DOGEUSDT")})[0],
              rej("new", "T1", 1, "Invalid symbol"));
    EXPECT_EQ(run(e, {new_order("T1", 1, "X", 1, 10)})[0], rej("new", "T1", 1, "Invalid side"));
    EXPECT_EQ(run(e, {new_order("T1", 1, "B", 0, 10)})[0],
              rej("new", "T1", 1, "Invalid quantity"));
    EXPECT_EQ(run(e, {new_order("T1", 1, "B", 1, -5)})[0], rej("new", "T1", 1, "Invalid price"));
}

TEST(Validation, MissingOrWrongTypeFieldsAreRejected)
{
    MatchingEngine e;
    EXPECT_EQ(run(e, {"{\"action\": \"cancel\", \"trader\": \"T1\"}"})[0],
              "{\"event\": \"reject\", \"action\": \"cancel\", \"trader\": \"T1\", "
              "\"reason\": \"Missing or invalid field: order_id\"}");
    EXPECT_EQ(run(e, {"{\"action\": \"new\", \"trader\": \"T1\", \"order_id\": 1, "
                      "\"symbol\": \"BTCUSDT\", \"side\": \"B\", \"qty\": \"5\", \"price\": 1}"})[0],
              "{\"event\": \"reject\", \"action\": \"new\", \"trader\": \"T1\", \"order_id\": 1, "
              "\"reason\": \"Missing or invalid field: qty\"}");
    EXPECT_EQ(run(e, {"{\"action\": \"delete\"}"})[0],
              "{\"event\": \"reject\", \"action\": \"delete\", \"reason\": \"Unknown action\"}");
    EXPECT_EQ(run(e, {"[1, 2]"})[0], "{\"event\": \"reject\", \"reason\": \"Malformed JSON\"}");
}

// ----------------------------TELEMENTRY----------------------------//

TEST(Snapshot, BestThreeLevelsWithTotalVolume)
{
    MatchingEngine e;
    run(e, {new_order("A", 1, "B", 1, 10), new_order("A", 2, "B", 2, 10),
            new_order("A", 3, "B", 3, 9), new_order("A", 4, "B", 4, 8), new_order("A", 5, "B", 5, 7),
            new_order("C", 1, "S", 1, 20), new_order("C", 2, "S", 1, 21)});
    auto s = book(e.snapshot(), "BTCUSDT");
    ASSERT_EQ(s.bids.size(), 3u);
    EXPECT_EQ(s.bids[0].price, 10);
    EXPECT_EQ(s.bids[0].volume, 3);
    EXPECT_EQ(s.bids[2].price, 8);
    ASSERT_EQ(s.asks.size(), 2u);
    EXPECT_EQ(s.asks[0].price, 20);
    EXPECT_TRUE(book(e.snapshot(), "ETHUSDT").bids.empty());
}

TEST(Snapshot, ConcurrentReadsDuringMatchingAreSafe)
{
    MatchingEngine e;
    std::atomic<bool> done{false};
    std::thread reader([&] {
        while (!done)
            (void)e.snapshot();
    });
    for (int i = 1; i <= 2000; ++i)
    {
        e.handle_line(new_order("S", i, "S", 1, 100 + i % 5));
        e.handle_line(new_order("B", i, "B", 1, 104));
    }
    done = true;
    reader.join();
    SUCCEED();
}
