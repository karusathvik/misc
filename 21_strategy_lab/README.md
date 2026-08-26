# 21 Strategy Lab

A modular framework for testing many strategies against the exact same game engine and benchmark harness.

## Files

- `game.hpp` — game state, deck, scoring, lives, STAYs, streaks, and transitions.
- `strategy.hpp` — the common `Strategy` interface. Every algorithm implements `chooseAction()`.
- `benchmark.hpp` — generic simulation and statistics. Each strategy includes this directly.
- `solver.hpp` — the previously supplied TwentyOneSolver implementation.
- `baseline_strategy.cpp` — runs the old solver's FAST policy through the generic benchmark.
- `random_strategy.cpp` — deliberately simple example showing how to add a new strategy.
- `template_strategy.cpp` — copy this file whenever you want to start a new strategy.

There is intentionally no `bench.cpp`. Each strategy `.cpp` is its own benchmark executable.

## Compile and run baseline

```bash
g++ -O3 -std=c++17 baseline_strategy.cpp -o baseline
./baseline
```

Default is 20,000 trials with seed `123456789`.

Choose a trial count:

```bash
./baseline 1000
./baseline 20000
```

Choose both trial count and seed:

```bash
./baseline 20000 12345
```

## Compile and run random example

```bash
g++ -O3 -std=c++17 random_strategy.cpp -o random_strategy
./random_strategy 20000 12345
```

Using the same seed for two strategies gives them the same sequence of shuffled decks.

## Add a new strategy

Copy the template:

```bash
cp template_strategy.cpp strategy_v2.cpp
```

Implement:

```cpp
Action chooseAction(const Observation& obs, const GameRules& rules)
```

Then compile it:

```bash
g++ -O3 -std=c++17 strategy_v2.cpp -o strategy_v2
./strategy_v2 20000 12345
```

No benchmark-specific source file is needed.

## Card representation

- Ace = `1`
- 2 through 9 = their normal value
- 10/J/Q/K = `10`

## Current streak rule

`game.hpp` currently preserves the earlier solver assumption:

```cpp
{0, 250, 250, 250, 250, 250}
```

If the actual game uses a progressive chain, change only `GameRules::streakBonuses` in `game.hpp`. Every strategy and benchmark then uses the same updated rule.
