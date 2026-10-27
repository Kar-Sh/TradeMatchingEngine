# Trade Matching Engine

A C++17 console program that reads JSON requests, matches buy and sell orders, and prints the results. It supports BTCUSDT and ETHUSDT.

## Build

This project uses Conan 2 and CMake.

```bash
conan profile detect --force
conan install . --build=missing
cmake --preset conan-release
cmake --build --preset conan-release
```

## Run

```bash
./build/Release/matching_engine < input.jsonl
```

The program reads one JSON request per line. Responses and trades are printed to standard output.

- `snapshots.log` stores the top three price levels for each symbol every five seconds.
- `engine.log` stores parsing errors, rejected requests, and critical errors.

## Tests

```bash
ctest --test-dir build/Release --output-on-failure
```

The tests cover matching priority, partial fills, cancellation, modification, invalid input, snapshots, and concurrent snapshot reads.

## Notes

- Orders are matched by price first, then by arrival time.
- An active order is identified by the pair `(trader, order_id)`.
- Reducing the remaining quantity without changing the price keeps the order's place in the queue. Increasing quantity or changing price moves it to the back and may match it immediately.
- Fills against the same trader at the same price are combined into one trade.
- A mutex protects the order books while requests are being processed. The snapshot thread copies the current state before writing it to disk.

## Appendix

1. Each symbol has an `OrderBook` with two `std::map`s (price → `std::list<Order>`): bids sorted descending, asks ascending, so `begin()` is the best price; each list is FIFO.
   A global `std::map<(trader, order_id), OrderLocation>` stores symbol, side, price and the list iterator, so cancel/modify find and erase an order without scanning the book.
2. New order: O(log P + log N) if it does not cross; O(F·log N + L·log P) worst case,
   where F = fills, L = price levels consumed, P = price levels, N = resting orders.
   Cancel: O(log N + log P). Modify (quantity decrease): O(log N), in place.
   Modify (price change / quantity increase): cancel + new order.
3. One `std::mutex` guards the books and the index. The matching thread locks it per request;
   the telemetry thread locks it only to copy the top 3 levels, then writes `snapshots.log` without the lock.
