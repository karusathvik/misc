#pragma once

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

namespace twentyone::advanced {

inline constexpr double NEG_INF = -1e100;

enum class Profile {
    SAFE,
    BALANCED,
    AGGRESSIVE
};

struct Features {
    double clearProb = 0.0;
    double forcedBustProb = 0.0;
    double expectedSafePlacements = 0.0;
    double coverage = 0.0;
    double bridge = 0.0;
    double reservoir = 0.0;
    double poison = 0.0;
    double nextClearEV = 0.0;
    int distinctOuts = 0;
    int readyLanes = 0;
};

inline std::string canonicalKey(const State& s, int tag, bool includeScore = false) {
    std::array<std::array<int, 4>, NUM_COLS> cols{};
    for (int i = 0; i < NUM_COLS; ++i) {
        cols[i] = {s.col[i].hard, s.col[i].aces, s.col[i].cards, s.col[i].sevens};
    }
    std::sort(cols.begin(), cols.end());

    std::string key;
    key.reserve(144);
    auto add = [&](int x) {
        key += std::to_string(x);
        key.push_back(',');
    };

    add(tag);
    add(s.lives);
    add(s.stays);
    add(s.streak);
    if (includeScore) add(s.score);
    key.push_back('|');
    for (int v = 1; v <= 10; ++v) add(s.remaining[v]);
    key.push_back('|');
    for (const auto& c : cols) {
        for (int x : c) add(x);
        key.push_back(';');
    }
    return key;
}

inline double poisonedness(const Column& c, const State& s) {
    if (c.cards == 0) return 0.0;
    const int t = total(c);
    const int n = cardsLeft(s);
    if (n <= 0) return 0.0;

    double rescue = 0.0;
    for (int card = 1; card <= 10; ++card) {
        if (s.remaining[card] <= 0) continue;
        Column nc = addCard(c, card);
        if (total(nc) <= 21) {
            rescue += static_cast<double>(s.remaining[card]) / n;
        }
    }

    double base = 0.0;
    if (t >= 20) base = 1.0;
    else if (t == 19) base = 0.82;
    else if (t == 18) base = 0.58;
    else if (t == 17) base = 0.34;
    else if (t == 16) base = 0.17;

    return base * (1.0 - 0.65 * rescue);
}

inline Features extractFeatures(const State& s, const GameRules& rules) {
    Features f;
    const int n = cardsLeft(s);
    if (n <= 0 || s.lives <= 0) return f;

    for (int card = 1; card <= 10; ++card) {
        const int count = s.remaining[card];
        if (count <= 0) continue;
        const double p = static_cast<double>(count) / n;

        int safe = 0;
        int ready = 0;
        int bestReward = 0;
        int baseTotal = 0;

        for (int col = 0; col < NUM_COLS; ++col) {
            Column nc = addCard(s.col[col], card);
            if (total(nc) > 21) continue;
            ++safe;
            const int base = clearScore(nc, rules);
            if (base > 0) {
                ++ready;
                baseTotal += base;
                bestReward = std::max(bestReward, base + streakBonus(s.streak, rules));
            }
        }

        if (ready > 0) {
            f.clearProb += p;
            ++f.distinctOuts;
            f.readyLanes += ready;
            f.coverage += p * std::max(400.0, static_cast<double>(baseTotal) / ready);

            if (ready >= 2) {
                const double abundance = std::min(1.0, static_cast<double>(count) / ready);
                f.coverage += p * 180.0 * (ready - 1) * abundance;
            }
        }

        if (safe == 0) f.forcedBustProb += p;
        f.expectedSafePlacements += p * safe;
        f.nextClearEV += p * bestReward;
    }

    int elevens = 0;
    for (const Column& c : s.col) {
        if (c.cards == 0) continue;
        const int t = total(c);

        f.poison += poisonedness(c, s);

        if (c.cards == 4) {
            double safe = 0.0;
            double ev = 0.0;
            for (int card = 1; card <= 10; ++card) {
                const int count = s.remaining[card];
                if (count <= 0) continue;
                Column nc = addCard(c, card);
                if (total(nc) > 21) continue;
                const double p = static_cast<double>(count) / n;
                safe += p;
                ev += p * clearScore(nc, rules);
            }
            f.bridge += 0.55 * ev + 260.0 * safe;
        } else if (c.cards == 3 && t <= 10) {
            f.bridge += 125.0;
        } else if (c.cards == 2 && t <= 7) {
            f.bridge += 40.0;
        }

        if (t == 11 && s.remaining[10] > 0) {
            ++elevens;
            f.reservoir += 150.0 * static_cast<double>(s.remaining[10]) / n;
        }

        if (c.cards == 2 && c.sevens == 2 && s.remaining[7] > 0) {
            f.reservoir += 145.0 * static_cast<double>(s.remaining[7]) / n;
        }
    }

    if (elevens >= 2) {
        const double p10 = static_cast<double>(s.remaining[10]) / n;
        f.reservoir += 220.0 * (elevens - 1) * p10;
    }

    return f;
}

inline double profileValue(const State& s, const GameRules& rules, Profile profile) {
    if (s.lives <= 0 || cardsLeft(s) <= 0) return 0.0;
    const Features f = extractFeatures(s, rules);

    const double diversity = 35.0 * f.distinctOuts;
    const double flexibility = 30.0 * f.expectedSafePlacements - 145.0 * f.forcedBustProb;
    const double resourceSlack = 20.0 * s.stays + 12.0 * std::max(0, s.lives - 1);

    if (profile == Profile::SAFE) {
        return
            0.58 * f.coverage +
            0.82 * f.bridge +
            0.62 * f.reservoir +
            1.10 * flexibility +
            0.30 * f.nextClearEV +
            0.80 * diversity -
            170.0 * f.poison +
            resourceSlack;
    }

    if (profile == Profile::AGGRESSIVE) {
        return
            0.78 * f.coverage +
            1.33 * f.bridge +
            1.18 * f.reservoir +
            0.18 * flexibility +
            (s.streak > 0 ? 0.88 : 0.50) * f.nextClearEV +
            0.42 * diversity -
            45.0 * f.poison +
            0.25 * resourceSlack;
    }

    return
        0.72 * f.coverage +
        1.02 * f.bridge +
        0.92 * f.reservoir +
        0.65 * flexibility +
        (s.streak > 0 ? 0.70 : 0.38) * f.nextClearEV +
        0.62 * diversity -
        95.0 * f.poison +
        0.55 * resourceSlack;
}

inline double superHeuristicValue(const State& s, const GameRules& rules) {
    const double a = profileValue(s, rules, Profile::SAFE);
    const double b = profileValue(s, rules, Profile::BALANCED);
    const double c = profileValue(s, rules, Profile::AGGRESSIVE);
    const double mx = std::max({a, b, c});
    const double avg = (a + b + c) / 3.0;
    return 0.72 * mx + 0.28 * avg;
}

inline Action quickPolicy(const State& state, int card, const GameRules& rules, Profile profile) {
    Action best{ActionType::PLACE, 0, -1};
    double bestV = NEG_INF;

    for (const Action& a : allLegalActions(state)) {
        Transition tr = applyAction(state, card, a, rules);
        if (!tr.valid) continue;

        double v = static_cast<double>(tr.reward);
        if (tr.terminal) {
            v -= (profile == Profile::SAFE ? 3500.0 : profile == Profile::BALANCED ? 1800.0 : 500.0);
        } else {
            State next = tr.next;
            next.score = state.score + tr.reward;
            v += profileValue(next, rules, profile);

            if (tr.bust) {
                // Bust can be strategically useful by discarding the card and
                // resetting a poisoned lane; only SAFE treats it as expensive.
                const double relief = poisonedness(state.col[a.place], state);
                if (profile == Profile::SAFE) v += 20.0 * relief;
                else if (profile == Profile::BALANCED) v += 70.0 * relief;
                else v += 120.0 * relief;
            }
        }

        if (v > bestV) {
            bestV = v;
            best = a;
        }
    }

    return best;
}

inline double weightedLowerTailCVaR(std::vector<std::pair<double, double>> outcomes, double alpha) {
    if (outcomes.empty()) return 0.0;
    alpha = std::clamp(alpha, 1e-6, 1.0);
    std::sort(outcomes.begin(), outcomes.end(), [](const auto& x, const auto& y) {
        return x.first < y.first;
    });

    double need = alpha;
    double acc = 0.0;
    for (const auto& [value, probability] : outcomes) {
        if (need <= 0.0) break;
        const double take = std::min(need, probability);
        acc += take * value;
        need -= take;
    }
    return acc / alpha;
}

inline double oneCardExpertRollout(
    const State& s,
    const GameRules& rules,
    Profile profile,
    double* cvarOut = nullptr,
    double cvarAlpha = 0.25
) {
    const int n = cardsLeft(s);
    if (n <= 0 || s.lives <= 0) {
        if (cvarOut) *cvarOut = 0.0;
        return 0.0;
    }

    double ev = 0.0;
    std::vector<std::pair<double, double>> outcomes;
    outcomes.reserve(10);

    for (int card = 1; card <= 10; ++card) {
        const int count = s.remaining[card];
        if (count <= 0) continue;
        const double p = static_cast<double>(count) / n;

        State draw = s;
        --draw.remaining[card];

        double best = NEG_INF;
        for (const Action& a : allLegalActions(draw)) {
            Transition tr = applyAction(draw, card, a, rules);
            if (!tr.valid) continue;

            double v = static_cast<double>(tr.reward);
            if (tr.terminal) {
                v -= (profile == Profile::SAFE ? 3000.0 : profile == Profile::BALANCED ? 1400.0 : 300.0);
            } else {
                State next = tr.next;
                next.score = s.score + tr.reward;
                v += 0.78 * profileValue(next, rules, profile);
            }
            best = std::max(best, v);
        }

        ev += p * best;
        outcomes.push_back({best, p});
    }

    if (cvarOut) *cvarOut = weightedLowerTailCVaR(outcomes, cvarAlpha);
    return ev;
}

class ExactSolver {
public:
    void clear() {
        evMemo_.clear();
        targetMemo_.clear();
    }

    double expectedFutureReward(const State& s, const GameRules& rules) {
        if (s.lives <= 0 || cardsLeft(s) <= 0) return 0.0;
        const std::string key = canonicalKey(s, 9001, false);
        auto it = evMemo_.find(key);
        if (it != evMemo_.end()) return it->second;

        const int n = cardsLeft(s);
        double ev = 0.0;

        for (int card = 1; card <= 10; ++card) {
            const int count = s.remaining[card];
            if (count <= 0) continue;
            State draw = s;
            --draw.remaining[card];

            double best = NEG_INF;
            for (const Action& a : allLegalActions(draw)) {
                Transition tr = applyAction(draw, card, a, rules);
                if (!tr.valid) continue;
                double v = static_cast<double>(tr.reward);
                if (!tr.terminal) {
                    State next = tr.next;
                    next.score = s.score + tr.reward;
                    v += expectedFutureReward(next, rules);
                }
                best = std::max(best, v);
            }
            ev += static_cast<double>(count) / n * best;
        }

        evMemo_[key] = ev;
        return ev;
    }

    // Exact risk-sensitive terminal utility for a fixed score floor.
    double floorUtility(const State& s, const GameRules& rules, int target) {
        if (s.lives <= 0 || cardsLeft(s) <= 0) return terminalFloorUtility(s.score, target);

        const std::string key = canonicalKey(s, 10000 + target, true);
        auto it = targetMemo_.find(key);
        if (it != targetMemo_.end()) return it->second;

        const int n = cardsLeft(s);
        double ev = 0.0;
        for (int card = 1; card <= 10; ++card) {
            const int count = s.remaining[card];
            if (count <= 0) continue;
            State draw = s;
            --draw.remaining[card];

            double best = NEG_INF;
            for (const Action& a : allLegalActions(draw)) {
                Transition tr = applyAction(draw, card, a, rules);
                if (!tr.valid) continue;
                State next = tr.next;
                next.score = s.score + tr.reward;
                const double v = tr.terminal
                    ? terminalFloorUtility(next.score, target)
                    : floorUtility(next, rules, target);
                best = std::max(best, v);
            }
            ev += static_cast<double>(count) / n * best;
        }

        targetMemo_[key] = ev;
        return ev;
    }

private:
    std::unordered_map<std::string, double> evMemo_;
    std::unordered_map<std::string, double> targetMemo_;

    static double terminalFloorUtility(int score, int target) {
        if (score >= target) {
            // Once the floor is secured, extra score still matters, but much
            // less than crossing the threshold.
            return 100000.0 + 0.35 * static_cast<double>(score - target);
        }
        const double deficit = static_cast<double>(target - score);
        return -deficit - 0.0016 * deficit * deficit;
    }
};

inline double exactStreakChainEV(
    const State& s,
    const GameRules& rules,
    int depth,
    std::unordered_map<std::string, double>& memo
) {
    if (depth <= 0 || s.lives <= 0 || cardsLeft(s) <= 0) return 0.0;

    const std::string key = canonicalKey(s, 20000 + depth, false);
    auto it = memo.find(key);
    if (it != memo.end()) return it->second;

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
            if (!tr.valid || tr.terminal || tr.reward <= 0) continue;
            State next = tr.next;
            next.score = s.score + tr.reward;
            double v = static_cast<double>(tr.reward);
            v += exactStreakChainEV(next, rules, depth - 1, memo);
            v += 0.10 * superHeuristicValue(next, rules);
            bestForCard = std::max(bestForCard, v);
        }

        ev += static_cast<double>(count) / n * bestForCard;
    }

    memo[key] = ev;
    return ev;
}

inline double progressLead(const State& s, int projectedCardsPlayed, int target) {
    const double expectedPace = static_cast<double>(target) * projectedCardsPlayed / 52.0;
    return static_cast<double>(s.score) - expectedPace;
}

} // namespace twentyone::advanced
