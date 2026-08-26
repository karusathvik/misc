#pragma once

#include "strategy.hpp"

#include <cstdint>
#include <iostream>
#include <string>

namespace twentyone {

// Generic live terminal driver for any Strategy implementation.
//
// Input:
//   1      = Ace
//   2..9   = face value
//   10     = 10/J/Q/K
//   0      = reset the internal game state
//   -1     = quit
//
// Output:
//   c3       -> place current card in column 3
//   s2 c4    -> STAY column 2, then place current card in column 4
//
// The runner tracks the exact remaining deck, score, streak, lives, STAYs,
// and columns. If you make a move different from the printed command, reset
// with 0 before continuing because the internal state will otherwise diverge.
template <typename StrategyType>
int runLiveStrategy(StrategyType& strategy) {
    GameRules rules;
    State state = freshGame();
    strategy.reset(0);

    while (true) {
        std::cerr << "current card: " << std::flush;

        int card = 0;
        if (!(std::cin >> card)) {
            return 0;
        }

        if (card == -1) {
            return 0;
        }

        if (card == 0) {
            state = freshGame();
            strategy.reset(0);
            std::cerr << "reset\n";
            continue;
        }

        if (card < 1 || card > 10) {
            std::cerr << "invalid card; use 1..10, 0 reset, -1 quit\n";
            continue;
        }

        if (state.lives <= 0) {
            std::cerr << "game already ended; enter 0 to reset\n";
            continue;
        }

        if (state.remaining[card] <= 0) {
            std::cerr << "invalid: no remaining cards of value " << card << "\n";
            continue;
        }

        // The current card is visible, so it is removed from the unknown deck
        // before the strategy evaluates the position.
        --state.remaining[card];

        Observation obs{state, card};
        Action action = strategy.chooseAction(obs, rules);

        if (!validAction(state, action)) {
            // Restore the deck count because the current turn was not applied.
            ++state.remaining[card];
            std::cerr << "strategy returned an invalid action\n";
            return 1;
        }

        Transition tr = applyAction(state, card, action, rules);
        if (!tr.valid) {
            ++state.remaining[card];
            std::cerr << "invalid state transition\n";
            return 1;
        }

        if (action.type == ActionType::PLACE) {
            std::cout << "c" << (action.place + 1) << '\n';
        } else {
            std::cout << "s" << (action.stay + 1)
                      << " c" << (action.place + 1) << '\n';
        }
        std::cout.flush();

        state = tr.next;
        state.score += tr.reward;
        ++state.cardsPlayed;

        if (state.lives <= 0) {
            std::cerr << "game over; internal score " << state.score
                      << ". enter 0 to reset or -1 to quit\n";
        } else if (state.cardsPlayed >= 52) {
            std::cerr << "52 cards completed; internal score " << state.score
                      << ". enter 0 to reset or -1 to quit\n";
        }
    }
}

} // namespace twentyone
