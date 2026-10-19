// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// stake_claim.hpp -- the claim procedure for one word that several hosts may write at once.
//
// With no cross-host CAS, a candidate stakes its own value, settles and re-reads, and wins only if
// the value still stands. A stake flushed past its window is held, then withdrawn while still its own.
// The admission lease and the recovery claim both run it, each over one field of a line of its own.

#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

#include "cme/shared.hpp"
#include "common/timing.hpp"
#include "config.hpp"
#include "observe/failpoint.hpp"
#include "util/coherency.hpp"

namespace cme::stake
{

// The claim field @field of one 64-byte line. The read, the own-value check and the retraction
// live here once, so a claim word adds only its own value, its free test and its publish.
template <typename T_Line, typename T_Value, auto field>
class FieldWord
{
public:
    // @empty is what a retraction leaves, and what a line format() never wrote reads as.
    FieldWord(T_Line* line, CoherencyMode mode, T_Value empty) noexcept
        : line_{line},
          mode_{mode},
          empty_{empty}
    {
    }

    [[nodiscard]] T_Value read() const
    {
        const T_Line copy = coherency::get(line_, mode_);
        return copy.isValidMagic() ? static_cast<T_Value>(copy.*field) : empty_;
    }

    [[nodiscard]] bool isMine(T_Value mine) const
    {
        return read() == mine;
    }

    // Writes @mine only while the field holds a value @isFree accepts, judged on the copy the write
    // is made from, and reaches @beforeWrite between that judgement and the write.
    template <typename T_IsFree>
    [[nodiscard]] bool publishIfFree(T_Value mine, T_IsFree isFree, failpoint::Boundary beforeWrite)
    {
        return coherency::rmwIfTrue(
            line_, mode_,
            [&](T_Line* copy)
            {
                if (!copy->isValidMagic() ||
                    !isFree(static_cast<T_Value>(copy->*field)))
                {
                    return false;
                }
                CME_FAILPOINT_REACH(beforeWrite);
                copy->*field = mine;
                return true;
            });
    }

    // Clears the field only while it still holds @mine, so another candidate's value is never
    // erased. @afterCheck, when given, is reached between that check and the write.
    void retract(T_Value mine, std::optional<failpoint::Boundary> afterCheck = std::nullopt) const noexcept
    {
        coherency::rmwIfTrue(
            line_, mode_,
            [&](T_Line* copy)
            {
                if (!copy->isValidMagic() || static_cast<T_Value>(copy->*field) != mine)
                {
                    return false;
                }
                if (afterCheck)
                {
                    CME_FAILPOINT_REACH(*afterCheck);
                }
                copy->*field = empty_;
                return true;
            });
    }

    [[nodiscard]] T_Line* getLine() const noexcept
    {
        return line_;
    }

private:
    T_Line* line_;
    CoherencyMode mode_;
    T_Value empty_;
};

enum class Outcome : std::uint8_t
{
    Pending,  // nothing won: watching, settling, holding a late stake, or yielding to another value
    Won,      // settled, and the word still carries this candidate's value
};

// One candidate's attempt at one word. The word type supplies Value, LateHold, an optional
// StallTimeout, and read, isFree, makeOwn, publish and retract.
template <typename T_Value>
class Claim
{
public:
    // One step, never blocking. Won is answered again on each tick for as long as the value stands,
    // so a caller that keeps ticking learns when a later writer has taken the word.
    template <typename T_Word>
    [[nodiscard]] Outcome tick(T_Word& word)
    {
        switch (phase_)
        {
            case Phase::Idle:
            case Phase::Watching:
                return tryStake(word);
            case Phase::Settling:
                return endSettle(word);
            case Phase::Holding:
                return endHold(word);
        }
        return Outcome::Pending;
    }

    // Withdraws a stake that is settling, won or held late, and starts over.
    template <typename T_Word>
    void reset(T_Word& word)
    {
        if (isStaked())
        {
            word.retract(mine_);
        }
        phase_ = Phase::Idle;
        stalled_.reset();
    }

    // Settling, won, or holding a late stake.
    [[nodiscard]] bool isStaked() const noexcept
    {
        return phase_ == Phase::Settling || phase_ == Phase::Holding;
    }

    [[nodiscard]] T_Value getMine() const noexcept
    {
        return mine_;
    }

    // For a caller that may block between ticks: until the open window closes, at most @poll.
    [[nodiscard]] timing::Nanos getNextWait(timing::Nanos poll) const noexcept
    {
        if (!isStaked())
        {
            return poll;
        }
        const auto left = window_.remaining();
        return left ? std::min(*left, poll) : timing::Nanos::zero();
    }

private:
    enum class Phase : std::uint8_t
    {
        Idle,
        Watching,  // the word carries another value, timed against the word's stall timeout
        Settling,  // staked in time; won once the settle has passed and the value still stands
        Holding,   // staked late; withdrawn once the late hold has passed
    };

    template <typename T_Word>
    [[nodiscard]] Outcome tryStake(T_Word& word)
    {
        // Opened before the read: a stake flushed after it closes may land behind another
        // candidate's confirmation, so it never settles.
        const timing::Deadline stakeWindow{config::stake::Window};
        const T_Value seen = word.read();
        const bool reclaimable = stalled_.has_value() && *stalled_ == seen;
        if (!word.isFree(seen) && !reclaimable)
        {
            // A value that has just stood its whole timeout ends the watch, and a new attempt re-reads it.
            return hasStoodTimeout<T_Word>(seen) ? tryStake(word) : Outcome::Pending;
        }
        stalled_.reset();
        const T_Value mine = word.makeOwn();
        if (!word.publish(mine))
        {
            phase_ = Phase::Idle;
            return Outcome::Pending;
        }
        mine_ = mine;
        const bool late = stakeWindow.expired();
        phase_ = late ? Phase::Holding : Phase::Settling;
        window_ = late ? timing::Deadline{T_Word::LateHold} : timing::Deadline{config::stake::Settle};
        return Outcome::Pending;
    }

    // Whether @seen has stood unchanged through the word's stall timeout, which ends the watch and
    // keeps @seen as reclaimable. Any other value restarts the watch and drops an earlier result.
    template <typename T_Word>
    [[nodiscard]] bool hasStoodTimeout(T_Value seen)
    {
        if constexpr (!T_Word::StallTimeout.has_value())
        {
            static_cast<void>(seen);
            return false;
        }
        else
        {
            if (phase_ != Phase::Watching || seen != watched_)
            {
                phase_ = Phase::Watching;
                watched_ = seen;
                watchedSince_.restart();
                stalled_.reset();
                return false;
            }
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access) checked by the if constexpr above
            if (watchedSince_.elapsed() < *T_Word::StallTimeout)
            {
                return false;
            }
            stalled_ = seen;
            phase_ = Phase::Idle;
            return true;
        }
    }

    template <typename T_Word>
    [[nodiscard]] Outcome endSettle(T_Word& word)
    {
        if (!window_.expired())
        {
            return Outcome::Pending;
        }
        // Re-read rather than trust the stake: a later cross-host write may have overwritten it.
        if (word.read() == mine_)
        {
            return Outcome::Won;
        }
        // Another value stands, so a new attempt starts at once.
        phase_ = Phase::Idle;
        return tryStake(word);
    }

    template <typename T_Word>
    [[nodiscard]] Outcome endHold(T_Word& word)
    {
        if (!window_.expired())
        {
            return Outcome::Pending;
        }
        // Whoever a late stake overwrote has finished its write by now, so the word opens again.
        word.retract(mine_);
        phase_ = Phase::Idle;
        return tryStake(word);
    }

    Phase phase_{Phase::Idle};
    T_Value mine_{};
    T_Value watched_{};
    timing::Stopwatch watchedSince_;
    std::optional<T_Value> stalled_;  // a value that stood its whole timeout, staked over if read again
    // Settle, or the late hold. A zero budget, so it reads as expired until a stake opens a real one.
    timing::Deadline window_{timing::Nanos::zero()};
};

}  // namespace cme::stake
