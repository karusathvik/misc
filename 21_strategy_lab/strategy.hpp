#pragma once

#include "game.hpp"

#include <cstdint>
#include <string>

namespace twentyone {

class Strategy {
public:
    virtual ~Strategy() = default;

    virtual std::string name() const = 0;

    // Called once before each benchmark game or once at the start of live play.
    virtual void reset(std::uint64_t gameSeed = 0) {
        (void)gameSeed;
    }

    // Every algorithm implements this one decision function.
    virtual Action chooseAction(
        const Observation& obs,
        const GameRules& rules
    ) = 0;
};

} // namespace twentyone
