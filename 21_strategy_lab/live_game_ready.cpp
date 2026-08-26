// Reuse the exact benchmarked strategy implementation, but rename its
// benchmark main() so this file can provide the live main().
#define main game_ready_benchmark_main
#include "game_ready_v14.cpp"
#undef main

#include "live_runner.hpp"

int main() {
    GameReadyV14 strategy;
    return twentyone::runLiveStrategy(strategy);
}
