#pragma once

//+--------------------------------------------------------------------------
//
// File:        random_utils.h
//
// NightDriverStrip - (c) 2026 Plummer's Software LLC.  All Rights Reserved.
//
// This file is part of the NightDriver software project.
//
//    NightDriver is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 3 of the License, or
//    (at your option) any later version.
//
//    NightDriver is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with Nightdriver.  It is normally found in copying.txt
//    If not, see <https://www.gnu.org/licenses/>.
//
//
// Description:
//
//    Random number helpers used across effects.
//
//---------------------------------------------------------------------------

#include "globals.h"

#include <cstdlib>
#include <ctime>
#include <random>
#include <type_traits>

// Randomness mode - the "trueRandom" device setting (DeviceConfig::SetTrueRandom).
//
//   true:  Arduino random() reads the ESP32's hardware RNG (esp_random(), truly random while
//          the radio is on), and the draw loop stirs fresh hardware entropy into FastLED's
//          random8()/random16() every frame (StirRandomEntropy), so they never repeat either.
//   false: Arduino random(), FastLED and std::rand() all restart from one fixed seed, so
//          effects replay the same random sequence from that point on - for testing and
//          regression comparisons.
void ApplyRandomMode(bool trueRandom);
bool IsTrueRandom();
// Called once per frame from the draw loop; a no-op in pseudo-random mode.
void StirRandomEntropy();
// 32 random bits from whichever source the current mode uses.
uint32_t RandomU32();

template <typename T>
inline static T random_range(T lower, T upper)
{
#if USE_STRONG_RAND
    static_assert(std::is_arithmetic<T>::value, "Template argument must be numeric type");

    static std::random_device rd;
    static std::mt19937 gen(rd());

    if constexpr (std::is_integral<T>::value) {
        std::uniform_int_distribution<T> distrib(lower, upper);
        return distrib(gen);
    } else if constexpr (std::is_floating_point<T>::value) {
        std::uniform_real_distribution<T> distrib(lower, upper);
        return distrib(gen);
    }
#else
    // Seeded by ApplyRandomMode() - hardware bits in true-random mode, a fixed seed otherwise.
    if constexpr (std::is_integral<T>::value) {
        return RandomU32() % (upper - lower + 1) + lower;
    } else if constexpr (std::is_floating_point<T>::value) {
        return RandomU32() / (4294967295.0 / (upper - lower)) + lower;
    } else {
        static_assert(std::is_integral<T>::value || std::is_floating_point<T>::value, "Template argument must be numeric type");
    }
#endif
}
