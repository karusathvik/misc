// Reuse the exact benchmarked strategy implementation, but rename its
// benchmark main() so this file can provide the live main().
#define main consistency_fortress_benchmark_main
#include "consistency_fortress_v10.cpp"
#undef main

#include "live_runner.hpp"

int main() {
    ConsistencyFortressV10 strategy;
    return twentyone::runLiveStrategy(strategy);
}
