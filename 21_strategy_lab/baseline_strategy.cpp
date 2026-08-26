#include "benchmark.hpp"
#include "solver.hpp"

using namespace twentyone;

int main(int argc, char* argv[]) {
    // This wraps the previously supplied solver's FAST policy so it can be
    // benchmarked with exactly the same generic benchmark.hpp interface.
    TwentyOneSolver strategy(SolverMode::FAST);
    return runBenchmarkFromArgs(strategy, argc, argv, 20000);
}
