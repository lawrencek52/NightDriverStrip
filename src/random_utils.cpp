//+--------------------------------------------------------------------------
//
// File:        random_utils.cpp
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
//    True-random / pseudo-random mode switch for every random source the
//    effects draw on (see random_utils.h).
//
//---------------------------------------------------------------------------

#include "random_utils.h"

#include <esp_random.h>

// The fixed seed pseudo-random mode restarts every generator from. Any constant would do; it
// only has to be the same every time.
static constexpr uint32_t kPseudoRandomSeed = 0x4E445253;     // "NDRS"

static bool s_trueRandom = true;

void ApplyRandomMode(bool trueRandom)
{
    s_trueRandom = trueRandom;

    // Arduino's random() uses esp_random() until randomSeed() is called; useRealRandomGenerator()
    // flips it back and forth explicitly.
    useRealRandomGenerator(trueRandom);

    const uint32_t seed = trueRandom ? esp_random() : kPseudoRandomSeed;
    srand(seed);
    random16_set_seed(static_cast<uint16_t>(seed));
}

bool IsTrueRandom()
{
    return s_trueRandom;
}

// FastLED's random8()/random16() are a 16-bit linear congruential generator, so on their own
// they'd follow one fixed cycle from whatever seed they were given. Mixing 16 fresh hardware
// bits in every frame keeps the sequence unpredictable without touching the hot path of the
// effects that call them hundreds of times a frame.
void StirRandomEntropy()
{
    if (s_trueRandom)
        random16_add_entropy(static_cast<uint16_t>(esp_random()));
}

uint32_t RandomU32()
{
    if (s_trueRandom)
        return esp_random();

    // rand() only promises 15 bits (RAND_MAX may be as small as 0x7FFF), so build 32 from three.
    return (static_cast<uint32_t>(rand()) << 30) ^ (static_cast<uint32_t>(rand()) << 15) ^ static_cast<uint32_t>(rand());
}
