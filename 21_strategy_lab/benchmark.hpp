#pragma once

#include "strategy.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace twentyone {

struct BenchmarkOptions {
    int trials = 20000;
    std::uint64_t seed = 123456789ULL;
    bool showProgress = true;
};

struct BenchmarkStats {
    int trials = 0;

    double mean = 0.0;
    double median = 0.0;
    int mode = 0;
    double stddev = 0.0;

    int minimum = 0;
    int maximum = 0;

    double p05 = 0.0;
    double p25 = 0.0;
    double p75 = 0.0;
    double p95 = 0.0;

    double mean95Low = 0.0;
    double mean95High = 0.0;

    double bustOutRate = 0.0;
    double avgCardsPlayed = 0.0;
    double avgLivesLeft = 0.0;
    double avgStaysLeft = 0.0;
};

struct GameResult {
    int score = 0;
    int cardsPlayed = 0;
    int livesLeft = 0;
    int staysLeft = 0;
    bool bustedOut = false;
};

inline GameResult playBenchmarkGame(
    Strategy& strategy,
    std::uint64_t gameSeed,
    const GameRules& rules
) {
    State state = freshGame();
    std::vector<int> deck = makeDeck();

    std::mt19937_64 rng(gameSeed);
    std::shuffle(deck.begin(), deck.end(), rng);

    strategy.reset(gameSeed);

    for (int card : deck) {
        if (state.lives <= 0) break;

        // The current card is visible, so it is removed from the
        // unknown-card counts before the strategy receives the state.
        --state.remaining[card];

        Observation obs{state, card};
        Action action = strategy.chooseAction(obs, rules);

        if (!validAction(state, action)) {
            throw std::runtime_error(
                "Strategy returned an invalid action during benchmark."
            );
        }

        Transition tr = applyAction(state, card, action, rules);
        if (!tr.valid) {
            throw std::runtime_error(
                "Strategy produced an invalid state transition."
            );
        }

        state = tr.next;
        state.score += tr.reward;
        ++state.cardsPlayed;
    }

    return {
        state.score,
        state.cardsPlayed,
        state.lives,
        state.stays,
        state.lives <= 0
    };
}

inline double benchmarkPercentile(
    const std::vector<int>& sorted,
    double p
) {
    if (sorted.empty()) return 0.0;

    const double position = p * (sorted.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(std::floor(position));
    const std::size_t hi = static_cast<std::size_t>(std::ceil(position));

    if (lo == hi) return sorted[lo];

    const double fraction = position - static_cast<double>(lo);
    return sorted[lo] + fraction * (sorted[hi] - sorted[lo]);
}

inline BenchmarkStats calculateBenchmarkStats(
    const std::vector<GameResult>& results
) {
    BenchmarkStats stats;
    if (results.empty()) return stats;

    stats.trials = static_cast<int>(results.size());

    std::vector<int> scores;
    scores.reserve(results.size());

    double scoreSum = 0.0;
    double cardsSum = 0.0;
    double livesSum = 0.0;
    double staysSum = 0.0;
    int bustOuts = 0;

    for (const auto& r : results) {
        scores.push_back(r.score);
        scoreSum += r.score;
        cardsSum += r.cardsPlayed;
        livesSum += r.livesLeft;
        staysSum += r.staysLeft;
        if (r.bustedOut) ++bustOuts;
    }

    std::sort(scores.begin(), scores.end());

    stats.mean = scoreSum / results.size();
    stats.median = benchmarkPercentile(scores, 0.50);
    stats.minimum = scores.front();
    stats.maximum = scores.back();

    stats.p05 = benchmarkPercentile(scores, 0.05);
    stats.p25 = benchmarkPercentile(scores, 0.25);
    stats.p75 = benchmarkPercentile(scores, 0.75);
    stats.p95 = benchmarkPercentile(scores, 0.95);

    std::map<int, int> frequency;
    for (int score : scores) ++frequency[score];

    int bestCount = -1;
    for (const auto& [score, count] : frequency) {
        if (count > bestCount) {
            bestCount = count;
            stats.mode = score;
        }
    }

    double squared = 0.0;
    for (int score : scores) {
        const double d = score - stats.mean;
        squared += d * d;
    }

    stats.stddev = std::sqrt(squared / results.size());

    const double standardError =
        stats.stddev / std::sqrt(static_cast<double>(results.size()));
    const double margin = 1.96 * standardError;

    stats.mean95Low = stats.mean - margin;
    stats.mean95High = stats.mean + margin;

    stats.bustOutRate = static_cast<double>(bustOuts) / results.size();
    stats.avgCardsPlayed = cardsSum / results.size();
    stats.avgLivesLeft = livesSum / results.size();
    stats.avgStaysLeft = staysSum / results.size();

    return stats;
}

inline void printBenchmarkStats(
    const std::string& strategyName,
    const BenchmarkStats& s
) {
    std::cout << std::fixed << std::setprecision(2);

    std::cout << "\n========================================\n";
    std::cout << strategyName << '\n';
    std::cout << "========================================\n";
    std::cout << "Trials:           " << s.trials << '\n';
    std::cout << "Mean:             " << s.mean << '\n';
    std::cout << "Median:           " << s.median << '\n';
    std::cout << "Mode:             " << s.mode << '\n';
    std::cout << "Std dev:          " << s.stddev << '\n';
    std::cout << "Lower / min:      " << s.minimum << '\n';
    std::cout << "Upper / max:      " << s.maximum << '\n';
    std::cout << "5th percentile:   " << s.p05 << '\n';
    std::cout << "25th percentile:  " << s.p25 << '\n';
    std::cout << "75th percentile:  " << s.p75 << '\n';
    std::cout << "95th percentile:  " << s.p95 << '\n';
    std::cout << "Mean 95% CI:      ["
              << s.mean95Low << ", " << s.mean95High << "]\n";
    std::cout << "Bust-out rate:    " << 100.0 * s.bustOutRate << "%\n";
    std::cout << "Avg cards played: " << s.avgCardsPlayed << " / 52\n";
    std::cout << "Avg lives left:   " << s.avgLivesLeft << '\n';
    std::cout << "Avg stays left:   " << s.avgStaysLeft << '\n';
    std::cout << "========================================\n";
}

inline BenchmarkStats runBenchmark(
    Strategy& strategy,
    const GameRules& rules = GameRules{},
    const BenchmarkOptions& options = BenchmarkOptions{}
) {
    if (options.trials <= 0) {
        throw std::invalid_argument("Benchmark trials must be > 0.");
    }

    std::vector<GameResult> results;
    results.reserve(options.trials);

    // Same master seed => same exact sequence of shuffled decks for all
    // strategies, which makes comparisons reproducible and paired.
    std::mt19937_64 seedGenerator(options.seed);

    std::cout << "\nStrategy: " << strategy.name()
              << "\nTrials:   " << options.trials
              << "\nSeed:     " << options.seed << "\n";

    const int interval = std::max(1, options.trials / 20);

    for (int i = 0; i < options.trials; ++i) {
        const std::uint64_t gameSeed = seedGenerator();
        results.push_back(playBenchmarkGame(strategy, gameSeed, rules));

        if (options.showProgress &&
            ((i + 1) % interval == 0 || i + 1 == options.trials)) {
            const double pct = 100.0 * (i + 1) / options.trials;
            std::cout << "\rBenchmark progress: "
                      << std::fixed << std::setprecision(1)
                      << pct << "%" << std::flush;
        }
    }

    if (options.showProgress) std::cout << "\n";

    BenchmarkStats stats = calculateBenchmarkStats(results);
    printBenchmarkStats(strategy.name(), stats);
    return stats;
}

// Put this in each strategy's main(). Then that strategy becomes its own
// benchmark executable with no separate bench.cpp required.
//
//   int main(int argc, char* argv[]) {
//       MyStrategy strategy;
//       return twentyone::runBenchmarkFromArgs(strategy, argc, argv);
//   }
//
// Usage:
//   ./my_strategy                 # default 20,000 trials
//   ./my_strategy 1000            # 1,000 trials
//   ./my_strategy 20000 12345     # trials + reproducible seed
inline int runBenchmarkFromArgs(
    Strategy& strategy,
    int argc,
    char* argv[],
    int defaultTrials = 20000,
    std::uint64_t defaultSeed = 123456789ULL
) {
    int trials = defaultTrials;
    std::uint64_t seed = defaultSeed;

    try {
        if (argc >= 2) trials = std::stoi(argv[1]);
        if (argc >= 3) seed = std::stoull(argv[2]);
    } catch (const std::exception&) {
        std::cerr << "usage: " << argv[0] << " [trials] [seed]\n";
        return 1;
    }

    if (trials <= 0) {
        std::cerr << "Trials must be positive.\n";
        return 1;
    }

    GameRules rules;
    BenchmarkOptions options;
    options.trials = trials;
    options.seed = seed;
    options.showProgress = true;

    runBenchmark(strategy, rules, options);
    return 0;
}

} // namespace twentyone
