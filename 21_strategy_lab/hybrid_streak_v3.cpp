#include "benchmark.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace twentyone;

class HybridStreakV3A final : public Strategy {
public:
    std::string name() const override {
        return "Hybrid Streak V3";
    }

    void reset(std::uint64_t gameSeed = 0) override {
        (void)gameSeed;
        chainMemo_.clear();
        exactMemo_.clear();
    }

    Action chooseAction(
        const Observation& obs,
        const GameRules& rules
    ) override {
        chainMemo_.clear();

        struct Candidate {
            Action action;
            Transition tr;
            double quick = -INF;
            double finalValue = -INF;
        };

        std::vector<Candidate> candidates;
        candidates.reserve(20);

        for (const Action& a : allLegalActions(obs.state)) {
            Transition tr = applyAction(obs.state, obs.currentCard, a, rules);
            if (!tr.valid) continue;

            // No fake score penalty for busting. Losing a life is represented
            // by the resulting state itself. Terminal states simply have no
            // future value.
            double quick = static_cast<double>(tr.reward);

            if (!tr.terminal) {
                quick += setupValue(tr.next, rules);
                quick += (tr.next.streak > 0 ? 0.50 : 0.35) * expectedNextClearReward(tr.next, rules);
            }

            candidates.push_back({a, tr, quick, quick});
        }

        if (candidates.empty()) {
            return {ActionType::PLACE, 0, -1};
        }

        // Exact Bellman solve in the small endgame.
        if (cardsLeft(obs.state) <= 4) {
            const Candidate* best = nullptr;
            double bestValue = -INF;
            for (Candidate& c : candidates) {
                double v = static_cast<double>(c.tr.reward);
                if (!c.tr.terminal) v += exactFutureEV(c.tr.next, rules);
                c.finalValue = v;
                if (!best || v > bestValue) {
                    best = &c;
                    bestValue = v;
                }
            }
            return best->action;
        }

        // Search the expensive streak-continuation model only for the most
        // promising afterstates. This keeps 20k-trial benchmarks practical.
        std::sort(
            candidates.begin(),
            candidates.end(),
            [](const Candidate& a, const Candidate& b) {
                return a.quick > b.quick;
            }
        );

        const int shortlist = std::min<int>(4, candidates.size());

        for (int i = 0; i < shortlist; ++i) {
            Candidate& c = candidates[i];

            if (!c.tr.terminal) {
                // Exact chance-weighted value of maintaining a consecutive
                // scoring chain for up to five future cards. At each future
                // card we choose the best action that SCORES on that turn.
                // If that card cannot score, the streak ends and this term is 0.
                c.finalValue =
                    static_cast<double>(c.tr.reward) +
                    setupValue(c.tr.next, rules) +
                    streakChainEV(c.tr.next, rules, c.tr.next.streak > 0 ? 6 : 5);
            }
        }

        // Non-shortlisted candidates retain their cheap estimate. This is
        // deliberate: a clearly poor STAY/bust action should not consume the
        // expensive chain search budget.
        const Candidate* best = &candidates.front();

        for (const Candidate& c : candidates) {
            if (c.finalValue > best->finalValue) {
                best = &c;
            }
        }

        return best->action;
    }

private:
    static constexpr double INF = 1e100;

    std::unordered_map<std::string, double> chainMemo_;
    std::unordered_map<std::string, double> exactMemo_;

    static std::string chainKey(const State& s, int depth) {
        std::array<std::array<int, 4>, NUM_COLS> cols{};

        for (int i = 0; i < NUM_COLS; ++i) {
            cols[i] = {
                s.col[i].hard,
                s.col[i].aces,
                s.col[i].cards,
                s.col[i].sevens
            };
        }

        // Physical column labels do not matter strategically.
        std::sort(cols.begin(), cols.end());

        std::string key;
        key.reserve(110);

        auto appendInt = [&](int x) {
            key += std::to_string(x);
            key.push_back(',');
        };

        appendInt(depth);
        appendInt(s.lives);
        appendInt(s.stays);
        appendInt(s.streak);

        key.push_back('|');

        for (int v = 1; v <= 10; ++v) {
            appendInt(s.remaining[v]);
        }

        key.push_back('|');

        for (const auto& c : cols) {
            for (int x : c) appendInt(x);
            key.push_back(';');
        }

        return key;
    }

    static double expectedNextClearReward(
        const State& s,
        const GameRules& rules
    ) {
        const int n = cardsLeft(s);
        if (n <= 0 || s.lives <= 0) return 0.0;

        double ev = 0.0;

        for (int card = 1; card <= 10; ++card) {
            const int count = s.remaining[card];
            if (count <= 0) continue;

            int bestReward = 0;

            // Normal placements are enough for this cheap feature. STAY is
            // evaluated explicitly at the real decision node.
            for (int col = 0; col < NUM_COLS; ++col) {
                Column nc = addCard(s.col[col], card);
                if (total(nc) > 21) continue;

                const int base = clearScore(nc, rules);
                if (base <= 0) continue;

                bestReward = std::max(
                    bestReward,
                    base + streakBonus(s.streak, rules)
                );
            }

            ev +=
                static_cast<double>(count) / n *
                static_cast<double>(bestReward);
        }

        return ev;
    }

    // Fast, deck-aware value of a board BEFORE the next card is known.
    // This is intentionally based on measurable quantities (outs, safe
    // placements, five-card bridges, resource pressure), rather than just
    // fixed labels such as "11 is always worth +150".
    static double setupValue(
        const State& s,
        const GameRules& rules
    ) {
        if (s.lives <= 0) return 0.0;

        const int n = cardsLeft(s);
        if (n <= 0) return 0.0;

        double clearProbability = 0.0;
        double forcedBustProbability = 0.0;
        double expectedSafePlacements = 0.0;
        double stackedOutValue = 0.0;

        // Measure the next-card geometry of the whole board.
        for (int card = 1; card <= 10; ++card) {
            const int count = s.remaining[card];
            if (count <= 0) continue;

            const double p = static_cast<double>(count) / n;

            int safePlacements = 0;
            int readyScoringColumns = 0;

            for (int col = 0; col < NUM_COLS; ++col) {
                Column nc = addCard(s.col[col], card);

                if (total(nc) <= 21) {
                    ++safePlacements;

                    if (clearScore(nc, rules) > 0) {
                        ++readyScoringColumns;
                    }
                }
            }

            if (readyScoringColumns > 0) {
                clearProbability += p;
            }

            if (safePlacements == 0) {
                forcedBustProbability += p;
            }

            expectedSafePlacements += p * safePlacements;

            // Multiple prepared columns sharing an ABUNDANT out can be very
            // useful for streaks. Three 11s with many tens remaining are not
            // redundant in the same way that three stacks waiting for the last
            // remaining 7 would be.
            if (readyScoringColumns >= 2) {
                const double abundance =
                    std::min(1.0, static_cast<double>(count) /
                                      readyScoringColumns);

                stackedOutValue +=
                    p * (readyScoringColumns - 1) * abundance;
            }
        }

        double fiveCardBridge = 0.0;
        double lowBuilder = 0.0;
        double elevenReservoir = 0.0;
        double sevenReservoir = 0.0;

        for (const Column& c : s.col) {
            if (c.cards == 0) continue;

            const int t = total(c);

            // A four-card low stack acts as a streak bridge because a large
            // fraction (sometimes all) of the remaining deck gives an 800/1000
            // point fifth-card clear.
            if (c.cards == 4) {
                double safeFifth = 0.0;

                for (int card = 1; card <= 10; ++card) {
                    if (s.remaining[card] <= 0) continue;

                    Column nc = addCard(c, card);
                    if (total(nc) <= 21) {
                        safeFifth +=
                            static_cast<double>(s.remaining[card]) / n;
                    }
                }

                fiveCardBridge += 240.0 * safeFifth;
            }

            // Reward a low multi-card stack because it is approaching a robust
            // five-card clear without poisoning the board.
            if (c.cards == 3 && t <= 10) {
                lowBuilder += 55.0;
            } else if (c.cards == 2 && t <= 8) {
                lowBuilder += 22.0;
            }

            // 11 is valuable only in proportion to how many 10-value cards
            // actually remain.
            if (t == 11 && s.remaining[10] > 0) {
                const double pTen =
                    static_cast<double>(s.remaining[10]) / n;
                elevenReservoir += 115.0 * pTen;
            }

            // Likewise, preserve 7+7 only when 7s actually remain.
            if (c.cards == 2 && c.sevens == 2 && s.remaining[7] > 0) {
                const double pSeven =
                    static_cast<double>(s.remaining[7]) / n;
                sevenReservoir += 100.0 * pSeven;
            }
        }

        const double remainingFraction =
            static_cast<double>(n) / 52.0;

        // Scarce-resource option value. These are deliberately modest; the
        // actual transition already records whether a life/STAY was consumed.
        // Their value is larger when much of the deck remains and when the board
        // has a significant forced-bust probability.
        const double stayOption =
            s.stays * remainingFraction *
            (35.0 + 180.0 * forcedBustProbability);

        const double lifeOption =
            std::max(0, s.lives - 1) * remainingFraction *
            (22.0 + 120.0 * forcedBustProbability);

        const double lastLifeProtection =
            (s.lives == 1)
                ? 180.0 * remainingFraction * forcedBustProbability
                : 0.0;

        return
            175.0 * clearProbability +
            27.0 * expectedSafePlacements -
            230.0 * forcedBustProbability +
            165.0 * stackedOutValue +
            fiveCardBridge +
            lowBuilder +
            elevenReservoir +
            sevenReservoir +
            stayOption +
            lifeOption -
            lastLifeProtection;
    }

    // Expected value of a PURE consecutive scoring run from this state.
    // This is not a full-game expectimax; it is a targeted exact calculation
    // for the feature we specifically want to exploit: escalating streaks.
    double streakChainEV(
        const State& s,
        const GameRules& rules,
        int depth
    ) {
        if (depth <= 0 || s.lives <= 0) return 0.0;

        const int n = cardsLeft(s);
        if (n <= 0) return 0.0;

        const std::string key = chainKey(s, depth);
        auto memoIt = chainMemo_.find(key);
        if (memoIt != chainMemo_.end()) {
            return memoIt->second;
        }

        double ev = 0.0;

        for (int card = 1; card <= 10; ++card) {
            const int count = s.remaining[card];
            if (count <= 0) continue;

            State afterDraw = s;
            --afterDraw.remaining[card];

            double best = 0.0;

            // To continue the streak, this turn MUST score. Enumerate all legal
            // actions, including STAY -> PLACE, because a STAY can sometimes
            // clean one column while the current card scores in another.
            for (const Action& a : allLegalActions(afterDraw)) {
                Transition tr = applyAction(afterDraw, card, a, rules);

                if (!tr.valid || tr.terminal || tr.reward <= 0) {
                    continue;
                }

                const double value =
                    static_cast<double>(tr.reward) +
                    streakChainEV(tr.next, rules, depth - 1);

                if (value > best) {
                    best = value;
                }
            }

            ev +=
                static_cast<double>(count) / n * best;
        }

        chainMemo_.emplace(key, ev);
        return ev;
    }

    double exactFutureEV(
        const State& s,
        const GameRules& rules
    ) {
        if (s.lives <= 0) return 0.0;
        const int n = cardsLeft(s);
        if (n <= 0) return 0.0;

        const std::string key = chainKey(s, 1000);
        auto it = exactMemo_.find(key);
        if (it != exactMemo_.end()) return it->second;

        double ev = 0.0;
        for (int card = 1; card <= 10; ++card) {
            const int count = s.remaining[card];
            if (count <= 0) continue;

            State afterDraw = s;
            --afterDraw.remaining[card];

            double best = -INF;
            for (const Action& a : allLegalActions(afterDraw)) {
                Transition tr = applyAction(afterDraw, card, a, rules);
                if (!tr.valid) continue;

                double v = static_cast<double>(tr.reward);
                if (!tr.terminal) v += exactFutureEV(tr.next, rules);
                best = std::max(best, v);
            }

            ev += static_cast<double>(count) / n * best;
        }

        exactMemo_.emplace(key, ev);
        return ev;
    }

};

int main(int argc, char* argv[]) {
    HybridStreakV3A strategy;
    return runBenchmarkFromArgs(strategy, argc, argv, 20000);
}
