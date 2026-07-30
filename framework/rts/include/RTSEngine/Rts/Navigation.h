#pragma once

#include <RTSEngine/Navigation/GridNavigation.h>
#include <RTSEngine/Navigation/NavigationWorld.h>

namespace rts::gameplay {

using GridPoint = ::rts::navigation::GridPoint;
using NavigationGridState = ::rts::navigation::NavigationGridState;
using NavigationGrid = ::rts::navigation::NavigationGrid;
using PathResult = ::rts::navigation::PathResult;
using GridPathfinderScratch = ::rts::navigation::GridPathfinderScratch;
using GridPathfinder = ::rts::navigation::GridPathfinder;

using MovementDomain = ::rts::navigation::MovementDomain;
using MovementDomainMask = ::rts::navigation::MovementDomainMask;
using NavProfile = ::rts::navigation::NavProfile;
using NavigationCellLayers = ::rts::navigation::NavigationCellLayers;
using NavigationWorldState = ::rts::navigation::NavigationWorldState;
using NavigationWorld = ::rts::navigation::NavigationWorld;
using WeightedPathResult = ::rts::navigation::WeightedPathResult;
using WeightedPathfinderScratch =
    ::rts::navigation::WeightedPathfinderScratch;
using WeightedGridPathfinder = ::rts::navigation::WeightedGridPathfinder;
using NavPathRequest = ::rts::navigation::NavPathRequest;
using NavPathCompletion = ::rts::navigation::NavPathCompletion;
using NavigationRequestQueue = ::rts::navigation::NavigationRequestQueue;
using FixedPosition2D = ::rts::navigation::FixedPosition2D;
using FixedMover = ::rts::navigation::FixedMover;
using Facing16 = ::rts::navigation::Facing16;
using FormationKind = ::rts::navigation::FormationKind;
using FormationSlot = ::rts::navigation::FormationSlot;
using FormationPlanner = ::rts::navigation::FormationPlanner;

using ::rts::navigation::DomainBit;
using ::rts::navigation::ManhattanDistance;

} // namespace rts::gameplay
