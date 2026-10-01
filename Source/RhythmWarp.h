#pragma once

#include "DawFeatureTypes.h"

#include <vector>

namespace RhythmWarp
{
std::vector<RhythmMarker> sanitise(std::vector<RhythmMarker> markers,
                                   double clipDuration);

double mapTargetToSource(const std::vector<RhythmMarker>& markers,
                         double clipDuration,
                         double targetTime) noexcept;
}
