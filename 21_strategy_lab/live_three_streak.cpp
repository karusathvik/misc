#include "game.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

using namespace twentyone;

namespace {

struct KeyHash {
    std::size_t operator()(const std::string& s) const noexcept {
        return std::hash<std::string>{}(s);
    }
};

static std::unordered_map<std::string, double, KeyHash> chainMemo;

static std::string chainKey(const State& s, int need) {
    std::array<std::array<int,4>, NUM_COLS> cols{};
    for (int i = 0; i < NUM_COLS; ++i) {
        cols[i] = {s.col[i].hard, s.col[i].aces, s.col[i].cards, s.col[i].sevens};
    }
    std::sort(cols.begin(), cols.end());

    std::string k;
    k.reserve(80);
    auto add = [&](int x) {
        k += std::to_string(x);
        k.push_back(',');
    };

    add(need);
    add(s.lives);
    add(s.stays);
    add(s.streak);
    for (int v = 1; v <= 10; ++v) add(s.remaining[v]);
    for (const auto& c : cols) for (int x : c) add(x);
    return k;
}

static bool scoresNow(const Transition& tr) {
    return tr.valid && !tr.bust && tr.reward > 0;
}

// Exact probability that the next `need` turns can ALL be scoring turns,
// assuming we choose optimally after each revealed card.
// This is exactly the quantity we care about once a streak is running.
static double chainSuccessProbability(const State& s, const GameRules& rules, int need) {
    if (need <= 0) return 1.0;
    if (s.lives <= 0) return 0.0;

    const int n = cardsLeft(s);
    if (n < need || n <= 0) return 0.0;

    const std::string key = chainKey(s, need);
    auto it = chainMemo.find(key);
    if (it != chainMemo.end()) return it->second;

    double ans = 0.0;

    for (int card = 1; card <= 10; ++card) {
        const int cnt = s.remaining[card];
        if (cnt <= 0) continue;

        State revealed = s;
        --revealed.remaining[card];

        double best = 0.0;

        for (const Action& a : allLegalActions(revealed)) {
            Transition tr = applyAction(revealed, card, a, rules);
            if (!scoresNow(tr)) continue;

            const double p = chainSuccessProbability(tr.next, rules, need - 1);
            if (p > best) best = p;
        }

        ans += (static_cast<double>(cnt) / n) * best;
    }

    chainMemo.emplace(key, ans);
    return ans;
}

// Probability that the very next unknown card can be scored immediately.
static double nextClearProbability(const State& s, const GameRules& rules) {
    const int n = cardsLeft(s);
    if (n <= 0) return 0.0;

    double p = 0.0;

    for (int card = 1; card <= 10; ++card) {
        const int cnt = s.remaining[card];
        if (cnt <= 0) continue;

        State revealed = s;
        --revealed.remaining[card];

        bool canScore = false;
        for (const Action& a : allLegalActions(revealed)) {
            Transition tr = applyAction(revealed, card, a, rules);
            if (scoresNow(tr)) {
                canScore = true;
                break;
            }
        }

        if (canScore) p += static_cast<double>(cnt) / n;
    }

    return p;
}

// If the next card starts a streak, how likely is that newly started streak
// to reach length 3?  This is a one-card ignition lookahead.
static double ignitionProbabilityNextCard(const State& s, const GameRules& rules) {
    const int n = cardsLeft(s);
    if (n <= 0) return 0.0;

    double p = 0.0;

    for (int card = 1; card <= 10; ++card) {
        const int cnt = s.remaining[card];
        if (cnt <= 0) continue;

        State revealed = s;
        --revealed.remaining[card];

        double best = 0.0;
        for (const Action& a : allLegalActions(revealed)) {
            Transition tr = applyAction(revealed, card, a, rules);
            if (!scoresNow(tr)) continue;

            // That clear is streak-clear #1. Need two more consecutive clears.
            best = std::max(best, chainSuccessProbability(tr.next, rules, 2));
        }

        p += (static_cast<double>(cnt) / n) * best;
    }

    return p;
}

static int immediateScoringOutCount(const State& s, const GameRules& rules) {
    int outs = 0;
    for (int card = 1; card <= 10; ++card) {
        if (s.remaining[card] <= 0) continue;

        State revealed = s;
        --revealed.remaining[card];
        bool canScore = false;
        for (const Action& a : allLegalActions(revealed)) {
            Transition tr = applyAction(revealed, card, a, rules);
            if (scoresNow(tr)) {
                canScore = true;
                break;
            }
        }
        if (canScore) outs += s.remaining[card];
    }
    return outs;
}

static double preparationScore(const State& s, const GameRules& rules) {
    // The main term: probability that NEXT card can ignite a full 3-clear chain.
    double score = 12000.0 * ignitionProbabilityNextCard(s, rules);

    // Secondary: broad immediate scoring coverage means more ignition choices.
    score += 1200.0 * nextClearProbability(s, rules);

    // Prefer several independent prepared mechanisms rather than one pretty pile.
    int prepared = 0;
    int flexible = 0;

    for (const Column& c : s.col) {
        if (c.cards == 0) {
            ++flexible;
            continue;
        }

        const int t = total(c);

        // 11 is the classic abundant-out reservoir.
        if (t == 11) {
            ++prepared;
            score += 260.0;
        }

        // Four-card <=11 is essentially a universal fifth-card bridge.
        if (c.cards == 4 && t <= 11) {
            ++prepared;
            score += 650.0;
        }

        // 7+7 can be another distinct clear mechanism if 7s remain.
        if (c.cards == 2 && c.sevens == 2 && s.remaining[7] > 0) {
            ++prepared;
            score += 120.0 * s.remaining[7];
        }

        // Three/four low cards are useful bridge construction.
        if (c.cards == 3 && t <= 10) score += 170.0;
        if (c.cards == 4 && t <= 14) score += 120.0;

        // Keep at least one landing lane for ugly cards while preparing.
        if (t <= 10) ++flexible;

        // Poisoned columns hurt our ability to assemble multiple clears.
        if (t >= 18) score -= 110.0 * (t - 17);
    }

    score += 170.0 * prepared;
    score += 45.0 * std::min(flexible, 2);
    score += 6.0 * immediateScoringOutCount(s, rules);

    // We only care about surviving long enough to get ONE streak.
    // Lives/STAYs are therefore useful, but not sacred.
    score += 90.0 * s.lives;
    score += 130.0 * s.stays;

    return score;
}

static Action chooseForThreeStreak(const State& s, int card, const GameRules& rules) {
    chainMemo.clear();

    const auto actions = allLegalActions(s);

    Action best = actions.front();
    double bestValue = -std::numeric_limits<double>::infinity();

    // ------------------------------------------------------------
    // ACTIVE STREAK: score THIS TURN or the chain dies.
    // ------------------------------------------------------------
    if (s.streak > 0 && s.streak < 3) {
        const int needAfterThis = 3 - (s.streak + 1);

        for (const Action& a : actions) {
            Transition tr = applyAction(s, card, a, rules);
            if (!tr.valid) continue;

            double value;

            if (scoresNow(tr)) {
                // If this is the 3rd clear, success is absolute.
                if (s.streak + 1 >= 3) {
                    value = 1e12;
                } else {
                    // Otherwise maximize exact probability of completing the chain.
                    value = 1e9 * chainSuccessProbability(tr.next, rules, needAfterThis);

                    // Tie-break toward a state with more immediate continuation coverage.
                    value += 1e5 * nextClearProbability(tr.next, rules);
                }
            } else {
                // Any non-scoring move destroys the active streak. Still choose
                // the best preparation state if continuation is impossible.
                value = -1e8 + preparationScore(tr.next, rules);
                if (tr.bust) value -= (tr.next.lives <= 0 ? 1e9 : 5000.0);
            }

            if (value > bestValue) {
                bestValue = value;
                best = a;
            }
        }

        return best;
    }

    // ------------------------------------------------------------
    // PREPARATION MODE: do NOT cash isolated clears automatically.
    // We want a clear only when it has a credible path to 3 in a row.
    // ------------------------------------------------------------
    for (const Action& a : actions) {
        Transition tr = applyAction(s, card, a, rules);
        if (!tr.valid) continue;

        double value = 0.0;

        if (scoresNow(tr)) {
            // Current clear would be streak #1. The only thing that matters is
            // the exact probability of scoring on BOTH of the next two turns.
            const double pFinish = chainSuccessProbability(tr.next, rules, 2);

            value = 1e7 * pFinish;

            // Small bonus for retaining a strong board if probabilities tie.
            value += 0.15 * preparationScore(tr.next, rules);
        } else {
            // Deliberately prefer preparation over a weak isolated clear.
            value = preparationScore(tr.next, rules);

            // Busts are allowed, but consuming a life should buy a meaningfully
            // better setup. The last life is especially valuable because death
            // eliminates all future chances to complete the experiment.
            if (tr.bust) {
                if (tr.terminal) value -= 1e9;
                else value -= 350.0 + 250.0 * (START_LIVES - tr.next.lives);
            }

            // A STAY is also allowed in preparation, but don't burn one for a
            // microscopic improvement.
            if (a.type == ActionType::STAY_PLACE) value -= 180.0;
        }

        if (value > bestValue) {
            bestValue = value;
            best = a;
        }
    }

    return best;
}

static void printAction(const Action& a) {
    if (a.type == ActionType::PLACE) {
        std::cout << "c" << (a.place + 1) << '\n';
    } else {
        // One line: clear column X, then place current card in column Y.
        std::cout << "s" << (a.stay + 1)
                  << " c" << (a.place + 1) << '\n';
    }
}

} // namespace

int main() {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    // Streak payout amounts do not matter to this strategy. We only use
    // whether a move clears and the current streak length.
    GameRules rules;
    State state = freshGame();

    int card;

    while (true) {
        std::cout << "current card: " << std::flush;
        if (!(std::cin >> card)) break;

        if (card == -1) break;

        if (card == 0) {
            state = freshGame();
            chainMemo.clear();
            std::cout << "reset\n";
            continue;
        }

        if (card < 1 || card > 10 || state.remaining[card] <= 0) {
            std::cout << "INVALID\n";
            continue;
        }

        // The card is visible now, so remove it from the unknown deck BEFORE
        // evaluating future probabilities.
        --state.remaining[card];

        const Action action = chooseForThreeStreak(state, card, rules);
        const Transition tr = applyAction(state, card, action, rules);

        printAction(action);

        state = tr.next;
        ++state.cardsPlayed;
        state.score += std::max(0, tr.reward);

        if (state.streak >= 3) {
            std::cout << "TARGET: 3-clear streak achieved\n";
            break;
        }

        if (tr.terminal || state.lives <= 0) {
            std::cout << "GAME OVER before target\n";
            break;
        }
    }

    return 0;
}
