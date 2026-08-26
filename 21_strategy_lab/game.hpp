#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace twentyone {

inline constexpr int NUM_COLS = 4;
inline constexpr int START_LIVES = 3;
inline constexpr int START_STAYS = 2;

struct GameRules {
    int score21 = 400;
    int score777 = 600;
    int scoreFiveCards = 800;
    int scoreFiveCard21 = 1000;
    int perfectNoBustBonus = 1000;

    // Confirmed experimentally through the third clear: +250, then +500.
    // The continuation +750/+1000/+1250 is the working progressive model.
    std::vector<int> streakBonuses{0, 250, 500, 750, 1000, 1250};
};

struct Column {
    int hard = 0;      // Aces counted as 1 here.
    int aces = 0;
    int cards = 0;
    int sevens = 0;

    bool operator==(const Column& other) const {
        return hard == other.hard &&
               aces == other.aces &&
               cards == other.cards &&
               sevens == other.sevens;
    }
};

struct State {
    std::array<Column, NUM_COLS> col{};

    // remaining[1] = Aces
    // remaining[2..9] = normal values
    // remaining[10] = 10/J/Q/K combined
    std::array<int, 11> remaining{};

    int lives = START_LIVES;
    int stays = START_STAYS;
    int streak = 0;

    // Benchmark/live bookkeeping. The solver does not need these in its DP key.
    int score = 0;
    int cardsPlayed = 0;
};

enum class ActionType : std::uint8_t {
    PLACE,
    STAY_PLACE
};

struct Action {
    ActionType type = ActionType::PLACE;
    int place = 0;  // 0..3
    int stay = -1;  // 0..3 only for STAY_PLACE
};

struct Observation {
    // IMPORTANT: state.remaining has already had currentCard removed.
    State state;
    int currentCard = 0;
};

struct Transition {
    State next;
    int reward = 0;
    bool bust = false;
    bool terminal = false;
    bool valid = true;
};

inline State freshGame() {
    State s;
    for (int v = 1; v <= 9; ++v) s.remaining[v] = 4;
    s.remaining[10] = 16;
    s.lives = START_LIVES;
    s.stays = START_STAYS;
    s.streak = 0;
    s.score = 0;
    s.cardsPlayed = 0;
    return s;
}

inline std::vector<int> makeDeck() {
    std::vector<int> deck;
    deck.reserve(52);

    for (int v = 1; v <= 9; ++v) {
        for (int k = 0; k < 4; ++k) deck.push_back(v);
    }
    for (int k = 0; k < 16; ++k) deck.push_back(10);

    return deck;
}

inline int cardsLeft(const State& s) {
    int n = 0;
    for (int v = 1; v <= 10; ++v) n += s.remaining[v];
    return n;
}

inline int total(const Column& c) {
    int t = c.hard;
    if (c.aces > 0 && t + 10 <= 21) t += 10;
    return t;
}

inline Column addCard(Column c, int card) {
    c.hard += card;
    ++c.cards;
    if (card == 1) ++c.aces;
    if (card == 7) ++c.sevens;
    return c;
}

inline int clearScore(const Column& c, const GameRules& rules) {
    const int t = total(c);
    if (t > 21) return 0;

    if (c.cards == 5 && t == 21) return rules.scoreFiveCard21;
    if (c.cards == 5) return rules.scoreFiveCards;
    if (c.cards == 3 && c.sevens == 3) return rules.score777;
    if (t == 21) return rules.score21;
    return 0;
}

inline int streakBonus(int previousStreak, const GameRules& rules) {
    if (previousStreak < 0 || rules.streakBonuses.empty()) return 0;

    if (previousStreak >= static_cast<int>(rules.streakBonuses.size())) {
        return rules.streakBonuses.back();
    }
    return rules.streakBonuses[previousStreak];
}

inline bool validAction(const State& s, const Action& a) {
    if (a.place < 0 || a.place >= NUM_COLS) return false;

    if (a.type == ActionType::PLACE) return true;

    if (a.type == ActionType::STAY_PLACE) {
        return s.stays > 0 &&
               a.stay >= 0 && a.stay < NUM_COLS &&
               s.col[a.stay].cards > 0;
    }

    return false;
}

inline Transition applyPlacement(
    const State& s,
    int card,
    int placeCol,
    const GameRules& rules
) {
    Transition tr;
    tr.next = s;

    if (placeCol < 0 || placeCol >= NUM_COLS) {
        tr.valid = false;
        tr.terminal = true;
        tr.reward = std::numeric_limits<int>::min() / 4;
        return tr;
    }

    Column nc = addCard(tr.next.col[placeCol], card);

    if (total(nc) > 21) {
        tr.bust = true;
        --tr.next.lives;
        tr.next.streak = 0;
        tr.next.col[placeCol] = Column{};
        tr.terminal = (tr.next.lives <= 0);
        return tr;
    }

    const int base = clearScore(nc, rules);
    if (base > 0) {
        tr.reward = base + streakBonus(tr.next.streak, rules);
        ++tr.next.streak;
        tr.next.col[placeCol] = Column{};
    } else {
        tr.next.streak = 0;
        tr.next.col[placeCol] = nc;
    }

    // Perfect bonus: all 52 cards completed with zero busts.
    if (cardsLeft(tr.next) == 0 &&
        tr.next.lives == START_LIVES) {
        tr.reward += rules.perfectNoBustBonus;
    }

    return tr;
}

inline Transition applyAction(
    const State& s,
    int card,
    const Action& a,
    const GameRules& rules
) {
    if (!validAction(s, a)) {
        Transition bad;
        bad.next = s;
        bad.valid = false;
        bad.terminal = true;
        bad.reward = std::numeric_limits<int>::min() / 4;
        return bad;
    }

    State temp = s;

    if (a.type == ActionType::STAY_PLACE) {
        --temp.stays;
        temp.col[a.stay] = Column{};
    }

    return applyPlacement(temp, card, a.place, rules);
}

inline std::vector<Action> allLegalActions(const State& s) {
    std::vector<Action> actions;
    actions.reserve(20);

    for (int p = 0; p < NUM_COLS; ++p) {
        actions.push_back({ActionType::PLACE, p, -1});
    }

    if (s.stays > 0) {
        for (int st = 0; st < NUM_COLS; ++st) {
            if (s.col[st].cards == 0) continue;
            for (int p = 0; p < NUM_COLS; ++p) {
                actions.push_back({ActionType::STAY_PLACE, p, st});
            }
        }
    }

    return actions;
}

} // namespace twentyone
