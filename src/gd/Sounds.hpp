// Plays the user-supplied ding-1..6 sounds through GD's FMOD engine.
#pragma once

#include "../core/Playback.hpp"

namespace fwl::gd::sounds {

// Plays the ding mapped to `ev` in settings (0 = off).
void play(SoundEvent ev);
// Plays ding `index` (1..6) directly, honouring enable/volume settings.
void ding(int index);
// Rising count ding for the n-th passing tick of a sweep (0-based).
void count(int passIndex);

} // namespace fwl::gd::sounds
