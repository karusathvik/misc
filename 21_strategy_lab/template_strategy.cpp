#include "benchmark.hpp"

#include <string>

using namespace twentyone;

class MyNewStrategy final : public Strategy {
public:
    std::string name() const override {
        return "My New Strategy";
    }

    Action chooseAction(
        const Observation& obs,
        const GameRules& rules
    ) override {
        (void)rules;

        // Replace this with your algorithm.
        // This placeholder always places in column 1.
        return {ActionType::PLACE, 0, -1};
    }
};

int main(int argc, char* argv[]) {
    MyNewStrategy strategy;
    return runBenchmarkFromArgs(strategy, argc, argv, 20000);
}
