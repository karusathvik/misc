#include "benchmark.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

using namespace twentyone;

// -----------------------------------------------------------------------------
// AdaptiveRolloutJackpot
//
// Design goals:
//   1. Maximize score, not survival rate.
//   2. Treat a life and a STAY as different resources whose value depends on
//      the resulting state. Intentional non-terminal busts are fully legal.
//   3. Combine three useful policy biases instead of committing to one:
//        - reservoir / out-coverage construction
//        - long progressive-streak continuation
//        - aggressive five-card / upper-tail construction
//   4. Improve the heuristic policy with an exact one-card chance rollout.
//   5. Use an exact Bellman solve in the very small endgame.
//
// It is intentionally more expensive than GreedyUpperTail, but still designed
// to be usable with the shared benchmark framework.
// -----------------------------------------------------------------------------
class AdaptiveRolloutJackpot final : public Strategy {
public:
    std::string name() const override {
        return "Adaptive Rollout Jackpot";
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
            double value = -INF;
        };

        std::vector<Candidate> candidates;
        candidates.reserve(20);

        // Every legal action remains available, including deliberate busts and
        // proactive STAY -> PLACE actions. There is no arbitrary "bust cost".
        for (const Action& a : allLegalActions(obs.state)) {
            Transition tr = applyAction(obs.state, obs.currentCard, a, rules);
            if (!tr.valid) continue;

            double q = static_cast<double>(tr.reward);
            if (!tr.terminal) {
                State next = tr.next;
                next.score = obs.state.score + tr.reward;
                q += expertBlend(next, rules);
                q += 0.30 * expectedImmediateClear(next, rules);

                // If a bust just cleared a genuinely poisoned lane, recognize
                // that structural gain. This is NOT a generic bust reward.
                if (tr.bust) {
                    q += 65.0 * poisonedRelief(obs.state, a.place, next);
                }
            }

            candidates.push_back({a, tr, q, q});
        }

        if (candidates.empty()) {
            return {ActionType::PLACE, 0, -1};
        }

        // Exact endgame. With repeated card values and canonicalized columns,
        // memoization makes this much smaller than enumerating every raw order.
        if (cardsLeft(obs.state) <= 5) {
            Candidate* best = nullptr;
            double bestValue = -INF;

            for (Candidate& c : candidates) {
                double v = static_cast<double>(c.tr.reward);
                if (!c.tr.terminal) {
                    State next = c.tr.next;
                    next.score = obs.state.score + c.tr.reward;
                    v += exactFutureEV(next, rules);
                }
                c.value = v;

                if (!best || v > bestValue) {
                    best = &c;
                    bestValue = v;
                }
            }

            return best->action;
        }

        std::sort(
            candidates.begin(),
            candidates.end(),
            [](const Candidate& a, const Candidate& b) {
                return a.quick > b.quick;
            }
        );

        // A wider root shortlist than V3/V4, but only one exact future chance
        // layer. The rollout layer is where life-vs-STAY tradeoffs are examined
        // explicitly instead of via fixed resource penalties.
        const int shortlist = std::min<int>(6, candidates.size());

        for (int i = 0; i < shortlist; ++i) {
            Candidate& c = candidates[i];

            if (c.tr.terminal) {
                c.value = static_cast<double>(c.tr.reward);
                continue;
            }

            State next = c.tr.next;
            next.score = obs.state.score + c.tr.reward;

            const bool active = next.streak > 0;
            const int chainDepth = active ? 6 : 4;

            // One-step rollout: reveal the next card exactly according to the
            // remaining deck, then choose the best legal action for that card.
            const double rollout = oneCardRolloutEV(next, rules);

            // Separate scoring-only continuation tree. Progressive streak
            // bonuses make this term genuinely nonlinear.
            const double chain = streakChainEV(next, rules, chainDepth);

            // Preparation geometry is more important before ignition; chain
            // continuation is more important after a streak has started.
            const double setup = expertBlend(next, rules);

            double v =
                1.10 * static_cast<double>(c.tr.reward) +
                (active ? 1.05 : 0.95) * rollout +
                (active ? 1.45 : 0.90) * chain +
                (active ? 0.55 : 0.85) * setup;

            // Upper-tail tilt without replacing actual score as the objective.
            // Once a run is already strong, preserving a long live streak has
            // extra marginal value because later streak bonuses are large.
            const int projectedScore = next.score;
            if (active && projectedScore >= 6000) {
                v += 75.0 * std::min(next.streak, 6);
            }

            c.value = v;
        }

        const Candidate* best = &candidates.front();
        for (const Candidate& c : candidates) {
            if (c.value > best->value) best = &c;
        }

        return best->action;
    }

private:
    static constexpr double INF = 1e100;

    std::unordered_map<std::string, double> chainMemo_;
    std::unordered_map<std::string, double> exactMemo_;

    static std::string stateKey(const State& s, int tag) {
        std::array<std::array<int, 4>, NUM_COLS> cols{};

        for (int i = 0; i < NUM_COLS; ++i) {
            cols[i] = {
                s.col[i].hard,
                s.col[i].aces,
                s.col[i].cards,
                s.col[i].sevens
            };
        }

        std::sort(cols.begin(), cols.end());

        std::string key;
        key.reserve(128);

        auto add = [&](int x) {
            key += std::to_string(x);
            key.push_back(',');
        };

        add(tag);
        add(s.lives);
        add(s.stays);
        add(s.streak);
        key.push_back('|');

        for (int v = 1; v <= 10; ++v) add(s.remaining[v]);
        key.push_back('|');

        for (const auto& c : cols) {
            for (int x : c) add(x);
            key.push_back(';');
        }

        return key;
    }

    // Expected best immediate scoring reward on the next unknown card.
    static double expectedImmediateClear(
        const State& s,
        const GameRules& rules
    ) {
        const int n = cardsLeft(s);
        if (n <= 0 || s.lives <= 0) return 0.0;

        double ev = 0.0;

        for (int card = 1; card <= 10; ++card) {
            const int count = s.remaining[card];
            if (count <= 0) continue;

            int best = 0;
            State draw = s;
            --draw.remaining[card];

            // Include STAY actions here: an active streak can justify spending
            // a STAY to clear one lane while scoring with the current card in
            // another lane.
            for (const Action& a : allLegalActions(draw)) {
                Transition tr = applyAction(draw, card, a, rules);
                if (!tr.valid || tr.reward <= 0) continue;
                best = std::max(best, tr.reward);
            }

            ev += static_cast<double>(count) / n * best;
        }

        return ev;
    }

    // Probability-aware out coverage. Duplicate scarce outs are bad, whereas
    // duplicated abundant outs (especially 10s feeding several 11s) can be a
    // powerful streak reservoir.
    static double coveragePotential(
        const State& s,
        const GameRules& rules
    ) {
        const int n = cardsLeft(s);
        if (n <= 0) return 0.0;

        double coverage = 0.0;
        double abundanceReservoir = 0.0;
        double diversityMass = 0.0;
        int distinctOutValues = 0;

        for (int card = 1; card <= 10; ++card) {
            const int count = s.remaining[card];
            if (count <= 0) continue;

            int ready = 0;
            int totalBase = 0;

            for (int col = 0; col < NUM_COLS; ++col) {
                Column nc = addCard(s.col[col], card);
                if (total(nc) > 21) continue;

                const int base = clearScore(nc, rules);
                if (base > 0) {
                    ++ready;
                    totalBase += base;
                }
            }

            if (ready == 0) continue;

            ++distinctOutValues;
            const double p = static_cast<double>(count) / n;
            coverage += p * std::max(400.0, static_cast<double>(totalBase) / ready);
            diversityMass += p;

            if (ready >= 2) {
                // If there are enough copies of this out to serve multiple
                // prepared lanes, duplication is useful rather than redundant.
                const double abundance =
                    std::min(1.0, static_cast<double>(count) / ready);
                abundanceReservoir +=
                    p * 210.0 * (ready - 1) * abundance;
            }
        }

        // Mild entropy-like reward for covering several distinct next-card
        // values instead of making the board depend on one scarce rank.
        const double diversityBonus =
            45.0 * distinctOutValues * std::min(1.0, diversityMass);

        return coverage + abundanceReservoir + diversityBonus;
    }

    static double bridgePotential(
        const State& s,
        const GameRules& rules
    ) {
        const int n = cardsLeft(s);
        if (n <= 0) return 0.0;

        double value = 0.0;

        for (const Column& c : s.col) {
            if (c.cards == 0) continue;
            const int t = total(c);

            if (c.cards == 4) {
                double ev = 0.0;
                double safe = 0.0;

                for (int card = 1; card <= 10; ++card) {
                    const int count = s.remaining[card];
                    if (count <= 0) continue;

                    Column nc = addCard(c, card);
                    if (total(nc) > 21) continue;

                    const double p = static_cast<double>(count) / n;
                    safe += p;
                    ev += p * clearScore(nc, rules);
                }

                // A low 4-card stack is close to a wildcard streak bridge.
                value += 0.52 * ev + 250.0 * safe;
            } else if (c.cards == 3 && t <= 10) {
                value += 125.0;
            } else if (c.cards == 2 && t <= 7) {
                value += 45.0;
            }
        }

        return value;
    }

    static double elevenAndSevenPotential(const State& s) {
        const int n = cardsLeft(s);
        if (n <= 0) return 0.0;

        int elevens = 0;
        double value = 0.0;

        for (const Column& c : s.col) {
            if (c.cards == 0) continue;

            if (total(c) == 11) {
                ++elevens;
                value += 130.0 * static_cast<double>(s.remaining[10]) / n;
            }

            if (c.cards == 2 && c.sevens == 2 && s.remaining[7] > 0) {
                value += 150.0 * static_cast<double>(s.remaining[7]) / n;
            }
        }

        if (elevens >= 2) {
            const double p10 = static_cast<double>(s.remaining[10]) / n;
            value += 220.0 * (elevens - 1) * p10;
        }

        return value;
    }

    // Measures how constrained the board is by the actual remaining deck.
    static double flexibilityPotential(const State& s) {
        const int n = cardsLeft(s);
        if (n <= 0) return 0.0;

        double expectedSafe = 0.0;
        double forcedBust = 0.0;

        for (int card = 1; card <= 10; ++card) {
            const int count = s.remaining[card];
            if (count <= 0) continue;

            int safe = 0;
            for (int col = 0; col < NUM_COLS; ++col) {
                if (total(addCard(s.col[col], card)) <= 21) ++safe;
            }

            const double p = static_cast<double>(count) / n;
            expectedSafe += p * safe;
            if (safe == 0) forcedBust += p;
        }

        // Do not over-reward safety. It matters only because it preserves
        // future point-producing options.
        return 28.0 * expectedSafe - 150.0 * forcedBust;
    }

    // Three "experts" capture the strongest behaviors seen in prior versions.
    // We use a soft maximum rather than a fixed weighted average so a state can
    // be recognized as excellent for different reasons.
    static double expertBlend(
        const State& s,
        const GameRules& rules
    ) {
        if (s.lives <= 0 || cardsLeft(s) <= 0) return 0.0;

        const double coverage = coveragePotential(s, rules);
        const double bridges = bridgePotential(s, rules);
        const double reservoirs = elevenAndSevenPotential(s);
        const double flexibility = flexibilityPotential(s);
        const double nextClear = expectedImmediateClear(s, rules);

        // Reservoir expert: broad card coverage + stable bridge construction.
        const double reservoirExpert =
            0.70 * coverage +
            0.95 * bridges +
            0.80 * reservoirs +
            0.75 * flexibility +
            0.30 * nextClear;

        // Streak expert: ready scoring inventory and bridge insurance.
        const double streakExpert =
            0.85 * coverage +
            1.20 * bridges +
            1.00 * reservoirs +
            0.30 * flexibility +
            (s.streak > 0 ? 0.80 : 0.40) * nextClear;

        // Upper-tail expert: willingly tolerates somewhat ugly boards if they
        // contain high-value 5-card or duplicated abundant-out reservoirs.
        const double tailExpert =
            0.75 * coverage +
            1.35 * bridges +
            1.15 * reservoirs +
            0.15 * flexibility +
            0.55 * nextClear;

        const double mx = std::max({reservoirExpert, streakExpert, tailExpert});
        const double avg = (reservoirExpert + streakExpert + tailExpert) / 3.0;

        // Soft max: benefits from specialization without accepting the full
        // optimism bias of a hard max over noisy heuristic values.
        return 0.68 * mx + 0.32 * avg;
    }

    // Exact one-future-card rollout. At the next card, every legal action is
    // considered, including intentional bust and STAY. This is the core policy
    // improvement layer over the static experts.
    double oneCardRolloutEV(
        const State& s,
        const GameRules& rules
    ) {
        const int n = cardsLeft(s);
        if (n <= 0 || s.lives <= 0) return 0.0;

        double ev = 0.0;

        for (int card = 1; card <= 10; ++card) {
            const int count = s.remaining[card];
            if (count <= 0) continue;

            State draw = s;
            --draw.remaining[card];

            double best = -INF;

            for (const Action& a : allLegalActions(draw)) {
                Transition tr = applyAction(draw, card, a, rules);
                if (!tr.valid) continue;

                double v = static_cast<double>(tr.reward);

                if (!tr.terminal) {
                    State next = tr.next;
                    next.score = s.score + tr.reward;
                    v += 0.78 * expertBlend(next, rules);
                    v += (next.streak > 0 ? 0.42 : 0.20) *
                         expectedImmediateClear(next, rules);

                    if (tr.bust) {
                        v += 45.0 * poisonedRelief(draw, a.place, next);
                    }
                }

                best = std::max(best, v);
            }

            ev += static_cast<double>(count) / n * best;
        }

        return ev;
    }

    double streakChainEV(
        const State& s,
        const GameRules& rules,
        int depth
    ) {
        if (depth <= 0 || s.lives <= 0 || cardsLeft(s) <= 0) return 0.0;

        const std::string key = stateKey(s, depth);
        auto it = chainMemo_.find(key);
        if (it != chainMemo_.end()) return it->second;

        const int n = cardsLeft(s);
        double ev = 0.0;

        for (int card = 1; card <= 10; ++card) {
            const int count = s.remaining[card];
            if (count <= 0) continue;

            State draw = s;
            --draw.remaining[card];

            double bestForCard = 0.0;

            for (const Action& a : allLegalActions(draw)) {
                Transition tr = applyAction(draw, card, a, rules);

                // A scoring chain survives only if this turn actually scores.
                if (!tr.valid || tr.terminal || tr.reward <= 0) continue;

                State next = tr.next;
                next.score = s.score + tr.reward;

                double v = static_cast<double>(tr.reward);
                v += streakChainEV(next, rules, depth - 1);

                // Tie-break toward a board that can continue after the exact
                // scoring path being measured.
                v += 0.12 * expertBlend(next, rules);

                bestForCard = std::max(bestForCard, v);
            }

            ev += static_cast<double>(count) / n * bestForCard;
        }

        chainMemo_[key] = ev;
        return ev;
    }

    static double poisonedRelief(
        const State& before,
        int col,
        const State& after
    ) {
        (void)after;
        if (col < 0 || col >= NUM_COLS || before.col[col].cards == 0) return 0.0;

        const int t = total(before.col[col]);
        if (t >= 20) return 1.0;
        if (t == 19) return 0.70;
        if (t == 18) return 0.40;
        if (t == 17) return 0.18;
        return 0.0;
    }

    double exactFutureEV(
        const State& s,
        const GameRules& rules
    ) {
        if (s.lives <= 0 || cardsLeft(s) <= 0) return 0.0;

        const std::string key = stateKey(s, 10000);
        auto it = exactMemo_.find(key);
        if (it != exactMemo_.end()) return it->second;

        const int n = cardsLeft(s);
        double ev = 0.0;

        for (int card = 1; card <= 10; ++card) {
            const int count = s.remaining[card];
            if (count <= 0) continue;

            State draw = s;
            --draw.remaining[card];

            double best = -INF;

            for (const Action& a : allLegalActions(draw)) {
                Transition tr = applyAction(draw, card, a, rules);
                if (!tr.valid) continue;

                double v = static_cast<double>(tr.reward);
                if (!tr.terminal) {
                    State next = tr.next;
                    next.score = s.score + tr.reward;
                    v += exactFutureEV(next, rules);
                }

                best = std::max(best, v);
            }

            ev += static_cast<double>(count) / n * best;
        }

        exactMemo_[key] = ev;
        return ev;
    }
};

int main(int argc, char* argv[]) {
    AdaptiveRolloutJackpot strategy;
    return runBenchmarkFromArgs(strategy, argc, argv, 20000);
}
