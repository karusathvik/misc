#include "advanced_policy_common.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

using namespace twentyone;
using namespace twentyone::advanced;

// -----------------------------------------------------------------------------
// Consistency Fortress V10
//
// Objective: make low-score catastrophes rare, with 5500 as the target floor.
// It intentionally differs from the score-maximizers:
//   * never takes an avoidable bust;
//   * if all ordinary placements bust, uses a safe STAY before a life;
//   * preserves at least one flexible lane;
//   * still builds 11 reservoirs / 5-card bridges and exploits streaks;
//   * uses exact target-utility DP in the final six unknown cards.
//
// No stochastic policy can guarantee 5500 on every possible deck, but this is
// explicitly optimized for lower-tail consistency rather than maximum score.
// -----------------------------------------------------------------------------
class ConsistencyFortressV10 final : public Strategy {
public:
    std::string name() const override { return "Consistency Fortress V10"; }

    void reset(std::uint64_t gameSeed = 0) override {
        (void)gameSeed;
        exact_.clear();
        chainMemo_.clear();
    }

    Action chooseAction(const Observation& obs, const GameRules& rules) override {
        constexpr int TARGET = 5500;
        chainMemo_.clear();

        struct Candidate {
            Action a;
            Transition tr;
            double quick = NEG_INF;
            double value = NEG_INF;
        };

        std::vector<Candidate> all;
        all.reserve(20);
        bool hasSafeNormal = false;
        bool hasSafeAny = false;

        for (const Action& a : allLegalActions(obs.state)) {
            Transition tr = applyAction(obs.state, obs.currentCard, a, rules);
            if (!tr.valid) continue;
            if (!tr.bust) {
                hasSafeAny = true;
                if (a.type == ActionType::PLACE) hasSafeNormal = true;
            }
            all.push_back({a, tr, NEG_INF, NEG_INF});
        }

        if (all.empty()) return {ActionType::PLACE, 0, -1};

        std::vector<Candidate> candidates;
        candidates.reserve(all.size());

        for (Candidate c : all) {
            // Hard lower-tail rule: a life is never spent when the same card
            // can be resolved without busting. If a normal safe placement
            // exists we may still consider proactive STAYs, but never a bust.
            if (hasSafeAny && c.tr.bust) continue;

            // Reserve STAYs as emergency resources. If an ordinary placement
            // is safe, do not spend a STAY merely to make the board prettier.
            // The only exception is an immediate scoring STAY while a streak
            // is already live, because progressive streak rewards can justify it.
            if (hasSafeNormal && c.a.type == ActionType::STAY_PLACE) {
                const bool liveScoringException =
                    obs.state.streak > 0 && c.tr.reward > 0;
                if (!liveScoringException) continue;
            }

            State next = c.tr.next;
            next.score = obs.state.score + c.tr.reward;
            const Features f = extractFeatures(next, rules);

            double q = static_cast<double>(c.tr.reward);
            if (!c.tr.terminal) {
                q += profileValue(next, rules, Profile::SAFE);
                q += 0.36 * f.nextClearEV;

                // A four-card bridge and a live streak are still excellent
                // because they generate points while simultaneously clearing
                // pressure from the board.
                if (next.streak > 0) q += 120.0 * std::min(next.streak, 5);
                q += 0.18 * f.bridge;

                // With one life left, explicitly prefer afterstates with low
                // next-card forced-bust probability.
                if (next.lives == 1) q -= 850.0 * f.forcedBustProb;
            }
            c.quick = c.value = q;
            candidates.push_back(c);
        }

        // Should only happen if every legal action busts.
        if (candidates.empty()) candidates = all;

        if (cardsLeft(obs.state) <= 6) {
            Candidate* best = nullptr;
            double bestV = NEG_INF;
            for (Candidate& c : candidates) {
                State next = c.tr.next;
                next.score = obs.state.score + c.tr.reward;
                const double v = (c.tr.terminal || cardsLeft(next) == 0)
                    ? terminalUtility(next.score, TARGET)
                    : exact_.floorUtility(next, rules, TARGET);
                c.value = v;
                if (!best || v > bestV) {
                    best = &c;
                    bestV = v;
                }
            }
            return best->a;
        }

        std::sort(candidates.begin(), candidates.end(), [](const Candidate& x, const Candidate& y) {
            return x.quick > y.quick;
        });

        const int shortlist = std::min<int>(5, candidates.size());
        for (int i = 0; i < shortlist; ++i) {
            Candidate& c = candidates[i];
            if (c.tr.terminal) continue;

            State next = c.tr.next;
            next.score = obs.state.score + c.tr.reward;
            const Features f = extractFeatures(next, rules);

            double cvar = 0.0;
            const double roll = oneCardExpertRollout(next, rules, Profile::SAFE, &cvar, 0.30);
            const double chain = exactStreakChainEV(next, rules, next.streak > 0 ? 4 : 2, chainMemo_);

            const int projectedPlayed = obs.state.cardsPlayed + 1;
            const double lead = progressLead(next, projectedPlayed, TARGET);
            const bool ahead = lead >= 150.0 || next.score >= TARGET;

            c.value =
                1.08 * c.tr.reward +
                0.90 * profileValue(next, rules, ahead ? Profile::SAFE : Profile::BALANCED) +
                0.62 * roll +
                (ahead ? 0.62 : 0.35) * cvar +
                (next.streak > 0 ? 0.78 : 0.28) * chain;

            if (next.lives == 1 && next.score < TARGET) {
                c.value -= 700.0 * f.forcedBustProb;
            }
        }

        const Candidate* best = &candidates.front();
        for (const Candidate& c : candidates) {
            if (c.value > best->value) best = &c;
        }
        return best->a;
    }

private:
    ExactSolver exact_;
    std::unordered_map<std::string, double> chainMemo_;

    static double terminalUtility(int score, int target) {
        if (score >= target) return 100000.0 + 0.30 * (score - target);
        const double d = target - score;
        return -d - 0.0020 * d * d;
    }
};

int main(int argc, char* argv[]) {
    ConsistencyFortressV10 strategy;
    return runBenchmarkFromArgs(strategy, argc, argv, 20000);
}
