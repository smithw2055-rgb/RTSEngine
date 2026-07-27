#pragma once

#include <RTSEngine/Navigation/GridNavigation.h>

namespace rts::gameplay {

using GridPoint = ::rts::navigation::GridPoint;
using NavigationGridState = ::rts::navigation::NavigationGridState;
using NavigationGrid = ::rts::navigation::NavigationGrid;
using PathResult = ::rts::navigation::PathResult;
using GridPathfinderScratch = ::rts::navigation::GridPathfinderScratch;
using GridPathfinder = ::rts::navigation::GridPathfinder;

using ::rts::navigation::ManhattanDistance;

} // namespace rts::gameplay
