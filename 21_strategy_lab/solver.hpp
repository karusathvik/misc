#pragma once

#include "strategy.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace twentyone {

enum class SolverMode {
    FAST,
    LIVE
};

class TwentyOneSolver final : public Strategy {
public:
    explicit TwentyOneSolver(
        SolverMode mode = SolverMode::LIVE,
        int liveTimeBudgetMs = 140,
        int exactEndgameCards = 8,
        int rolloutsPerLeaf = 4
    )
        : mode_(mode),
          liveTimeBudgetMs_(liveTimeBudgetMs),
          exactEndgameCards_(exactEndgameCards),
          rolloutsPerLeaf_(rolloutsPerLeaf) {}

    std::string name() const override {
        return mode_ == SolverMode::FAST
            ? "TwentyOneSolver FAST"
            : "TwentyOneSolver LIVE";
    }

    void reset(std::uint64_t gameSeed = 0) override {
        (void)gameSeed;
        valueMemo_.clear();
        nodeCounter_ = 0;
    }

    Action chooseAction(
        const Observation& obs,
        const GameRules& rules
    ) override {
        if (mode_ == SolverMode::FAST) {
            return fastPolicy(obs.state, obs.currentCard, rules);
        }
        return solveCurrentCard(obs.state, obs.currentCard, rules);
    }

private:
    struct Candidate {
        Action action;
        Transition tr;
    };

    struct Timeout {};

    SolverMode mode_;
    int liveTimeBudgetMs_;
    int exactEndgameCards_;
    int rolloutsPerLeaf_;

    std::chrono::steady_clock::time_point deadline_{};
    std::uint64_t nodeCounter_ = 0;
    std::unordered_map<std::string, double> valueMemo_;

    void checkTime() {
        if ((++nodeCounter_ & 1023ULL) == 0) {
            if (std::chrono::steady_clock::now() >= deadline_) {
                throw Timeout{};
            }
        }
    }

    static std::string canonicalStateKey(
        const State& s,
        int depth,
        char mode
    ) {
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
        key.reserve(96);
        key.push_back(mode);
        key.append(std::to_string(depth));
        key.push_back('|');
        key.append(std::to_string(s.lives));
        key.push_back(',');
        key.append(std::to_string(s.stays));
        key.push_back(',');
        key.append(std::to_string(s.streak));
        key.push_back('|');

        for (int v = 1; v <= 10; ++v) {
            key.append(std::to_string(s.remaining[v]));
            key.push_back(',');
        }

        key.push_back('|');

        for (const auto& c : cols) {
            for (int x : c) {
                key.append(std::to_string(x));
                key.push_back(',');
            }
            key.push_back(';');
        }

        return key;
    }

    static std::string afterStateKey(
        const State& s,
        int reward
    ) {
        return canonicalStateKey(s, 0, 'D') + "#" + std::to_string(reward);
    }

    static std::vector<Candidate> generateCandidates(
        const State& s,
        int card,
        const GameRules& rules
    ) {
        const auto actions = allLegalActions(s);

        std::vector<Candidate> out;
        std::unordered_map<std::string, int> where;
        where.reserve(actions.size() * 2 + 1);

        for (const auto& a : actions) {
            Transition tr = applyAction(s, card, a, rules);
            if (!tr.valid) continue;

            std::string key = afterStateKey(tr.next, tr.reward);
            auto it = where.find(key);

            if (it == where.end()) {
                where.emplace(key, static_cast<int>(out.size()));
                out.push_back({a, tr});
            } else {
                Candidate& old = out[it->second];

                // Same strategic afterstate. Prefer the simpler non-STAY action.
                if (a.type == ActionType::PLACE &&
                    old.action.type == ActionType::STAY_PLACE) {
                    old = {a, tr};
                }
            }
        }

        return out;
    }

    static double fastBoardPotential(
        const State& s,
        const GameRules& rules
    ) {
        if (s.lives <= 0) return 0.0;

        const int n = cardsLeft(s);
        if (n == 0) return 0.0;

        double nextBestReward = 0.0;
        double forcedOrdinaryBustProb = 0.0;
        double scoringProb = 0.0;

        for (int v = 1; v <= 10; ++v) {
            if (s.remaining[v] == 0) continue;

            const double p = static_cast<double>(s.remaining[v]) / n;
            int bestReward = 0;
            bool anySafe = false;
            bool anyScore = false;

            for (int j = 0; j < NUM_COLS; ++j) {
                Column nc = addCard(s.col[j], v);

                if (total(nc) <= 21) {
                    anySafe = true;
                    const int base = clearScore(nc, rules);

                    if (base > 0) {
                        anyScore = true;
                        bestReward = std::max(
                            bestReward,
                            base + streakBonus(s.streak, rules)
                        );
                    }
                }
            }

            nextBestReward += p * bestReward;
            if (!anySafe) forcedOrdinaryBustProb += p;
            if (anyScore) scoringProb += p;
        }

        double columnFlex = 0.0;

        for (const auto& c : s.col) {
            if (c.cards == 0) {
                columnFlex += 10.0;
                continue;
            }

            const int t = total(c);

            if (c.cards == 4 && t <= 11) columnFlex += 70.0;
            else if (t <= 11) columnFlex += 18.0;
            else if (t >= 19) columnFlex -= 18.0;
            else if (t >= 17) columnFlex -= 8.0;

            double outs = 0.0;

            for (int v = 1; v <= 10; ++v) {
                if (s.remaining[v] == 0) continue;

                Column nc = addCard(c, v);
                if (total(nc) <= 21 && clearScore(nc, rules) > 0) {
                    outs += static_cast<double>(s.remaining[v]) / n;
                }
            }

            columnFlex += 45.0 * outs;
        }

        double resourceOption = 0.0;
        resourceOption += 20.0 * s.stays * forcedOrdinaryBustProb;
        resourceOption += 12.0 * std::max(0, s.lives - 1) * forcedOrdinaryBustProb;

        return nextBestReward +
               100.0 * scoringProb -
               150.0 * forcedOrdinaryBustProb +
               columnFlex +
               resourceOption;
    }

    static Action fastPolicy(
        const State& s,
        int card,
        const GameRules& rules
    ) {
        auto candidates = generateCandidates(s, card, rules);

        if (candidates.empty()) {
            return {ActionType::PLACE, 0, -1};
        }

        double best = -1e100;
        Action answer = candidates.front().action;

        for (const auto& c : candidates) {
            double value = c.tr.reward;
            if (!c.tr.terminal) {
                value += fastBoardPotential(c.tr.next, rules);
            }

            if (value > best) {
                best = value;
                answer = c.action;
            }
        }

        return answer;
    }

    static std::uint64_t deckSeed(
        const State& s,
        int rolloutIndex
    ) {
        std::uint64_t h = 1469598103934665603ULL;

        for (int v = 1; v <= 10; ++v) {
            h ^= static_cast<std::uint64_t>(s.remaining[v] + 17 * v);
            h *= 1099511628211ULL;
        }

        h ^= static_cast<std::uint64_t>(rolloutIndex + 0x9e3779b9);
        h *= 1099511628211ULL;
        return h;
    }

    double rolloutValue(
        const State& start,
        const GameRules& rules
    ) {
        if (start.lives <= 0 || cardsLeft(start) == 0) return 0.0;

        double totalScore = 0.0;

        for (int r = 0; r < rolloutsPerLeaf_; ++r) {
            checkTime();
            State s = start;
            std::vector<int> deck;
            deck.reserve(cardsLeft(s));

            for (int v = 1; v <= 10; ++v) {
                for (int k = 0; k < s.remaining[v]; ++k) {
                    deck.push_back(v);
                }
            }

            std::mt19937_64 rng(deckSeed(s, r));
            std::shuffle(deck.begin(), deck.end(), rng);

            int score = 0;

            for (int card : deck) {
                checkTime();
                if (s.lives <= 0) break;

                --s.remaining[card];
                Action a = fastPolicy(s, card, rules);
                Transition tr = applyAction(s, card, a, rules);
                score += std::max(0, tr.reward);
                s = tr.next;

                if (tr.terminal) break;
            }

            totalScore += score;
        }

        return totalScore / rolloutsPerLeaf_;
    }

    double expectedValue(
        const State& s,
        int depth,
        const GameRules& rules
    ) {
        checkTime();

        if (s.lives <= 0) return 0.0;

        const int n = cardsLeft(s);
        if (n == 0) return 0.0;

        if (n <= exactEndgameCards_ && depth < n) {
            depth = n;
        }

        if (depth <= 0) {
            return rolloutValue(s, rules);
        }

        const std::string key = canonicalStateKey(s, depth, 'V');
        auto it = valueMemo_.find(key);
        if (it != valueMemo_.end()) return it->second;

        double ev = 0.0;

        for (int card = 1; card <= 10; ++card) {
            const int count = s.remaining[card];
            if (count == 0) continue;

            State afterDraw = s;
            --afterDraw.remaining[card];

            const double probability = static_cast<double>(count) / n;
            ev += probability * bestKnownCardValue(
                afterDraw,
                card,
                depth - 1,
                rules
            );
        }

        if (valueMemo_.size() > 700000) {
            valueMemo_.clear();
        }

        valueMemo_.emplace(key, ev);
        return ev;
    }

    double bestKnownCardValue(
        const State& afterDraw,
        int card,
        int depth,
        const GameRules& rules
    ) {
        checkTime();

        auto candidates = generateCandidates(afterDraw, card, rules);

        std::sort(
            candidates.begin(),
            candidates.end(),
            [&](const Candidate& a, const Candidate& b) {
                const double va = a.tr.reward +
                    (a.tr.terminal ? 0.0 : fastBoardPotential(a.tr.next, rules));
                const double vb = b.tr.reward +
                    (b.tr.terminal ? 0.0 : fastBoardPotential(b.tr.next, rules));
                return va > vb;
            }
        );

        double best = -1e100;

        for (const auto& c : candidates) {
            double value = c.tr.reward;

            if (!c.tr.terminal) {
                value += expectedValue(c.tr.next, depth, rules);
            }

            best = std::max(best, value);
        }

        return best;
    }

    Action solveCurrentCard(
        const State& afterDraw,
        int card,
        const GameRules& rules
    ) {
        auto root = generateCandidates(afterDraw, card, rules);

        if (root.empty()) {
            return {ActionType::PLACE, 0, -1};
        }

        Action bestAction = root.front().action;
        double bestFallback = -1e100;

        for (const auto& c : root) {
            const double value = c.tr.reward +
                (c.tr.terminal ? 0.0 : fastBoardPotential(c.tr.next, rules));

            if (value > bestFallback) {
                bestFallback = value;
                bestAction = c.action;
            }
        }

        nodeCounter_ = 0;
        deadline_ = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(liveTimeBudgetMs_);

        const int remaining = cardsLeft(afterDraw);
        const int startDepth =
            (remaining <= exactEndgameCards_) ? remaining : 1;
        const int maxDepth =
            (remaining <= exactEndgameCards_)
                ? remaining
                : std::min(7, remaining);

        for (int depth = startDepth; depth <= maxDepth; ++depth) {
            try {
                double iterationBest = -1e100;
                Action iterationAction = bestAction;

                std::vector<Candidate> ordered = root;
                std::sort(
                    ordered.begin(),
                    ordered.end(),
                    [&](const Candidate& a, const Candidate& b) {
                        const double va = a.tr.reward +
                            (a.tr.terminal ? 0.0 : fastBoardPotential(a.tr.next, rules));
                        const double vb = b.tr.reward +
                            (b.tr.terminal ? 0.0 : fastBoardPotential(b.tr.next, rules));
                        return va > vb;
                    }
                );

                for (const auto& c : ordered) {
                    checkTime();

                    double value = c.tr.reward;
                    if (!c.tr.terminal) {
                        value += expectedValue(c.tr.next, depth, rules);
                    }

                    if (value > iterationBest) {
                        iterationBest = value;
                        iterationAction = c.action;
                    }
                }

                // Publish only a fully completed depth.
                bestAction = iterationAction;

            } catch (const Timeout&) {
                break;
            }
        }

        return bestAction;
    }
};

} // namespace twentyone
