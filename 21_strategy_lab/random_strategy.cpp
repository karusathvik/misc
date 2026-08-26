#include "benchmark.hpp"

#include <cstdint>
#include <random>
#include <string>

using namespace twentyone;

class RandomStrategy final : public Strategy {
private:
    std::mt19937_64 rng_;

public:
    std::string name() const override {
        return "Random Strategy";
    }

    void reset(std::uint64_t gameSeed = 0) override {
        rng_.seed(gameSeed);
    }

    Action chooseAction(
        const Observation& obs,
        const GameRules& rules
    ) override {
        (void)obs;
        (void)rules;

        std::uniform_int_distribution<int> columnDist(0, NUM_COLS - 1);
        return {ActionType::PLACE, columnDist(rng_), -1};
    }
};

int main(int argc, char* argv[]) {
    RandomStrategy strategy;
    return runBenchmarkFromArgs(strategy, argc, argv, 20000);
}
