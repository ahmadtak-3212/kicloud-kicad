/*
 * KiRouter - a push-and-(sometimes-)shove PCB router
 *
 * Copyright (C) 2013-2014 CERN
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 * Author: Tomasz Wlostowski <tomasz.wlostowski@cern.ch>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __PNS_WALKAROUND_H
#define __PNS_WALKAROUND_H

#include <functional>   // KICLOUD: R3.1b (WINDING_PREFERENCE)
#include <optional>     // KICLOUD: R3.1b
#include <set>

#include "pns_line.h"
#include "pns_node.h"
#include "pns_router.h"
#include "pns_logger.h"
#include "pns_algo_base.h"
#include "pns_topology.h"

namespace PNS {

class WALKAROUND : public ALGO_BASE
{
    static constexpr int MaxWalkPolicies = 3;

public:
    WALKAROUND( NODE* aWorld, ROUTER* aRouter ) :
        ALGO_BASE ( aRouter ),
        m_world( aWorld )
    {
        m_forceWinding = false;
        m_itemMask = ITEM::ANY_T;

        // Initialize other members, to avoid uninitialized variables.
        m_iteration = 0;
        m_initialLength = 0.0;
        m_forceCw = false;
        m_forceLongerPath = false;
        m_lengthLimitOn = true;
        m_useShortestPath = false;
        m_lengthExpansionFactor = 10.0;
        m_iterationLimit = Settings().WalkaroundIterationLimit();
    }

    ~WALKAROUND() {};

    enum STATUS
    {
        ST_IN_PROGRESS = 0,
        ST_ALMOST_DONE,
        ST_DONE,
        ST_STUCK,
        ST_NONE
    };

    enum WALK_POLICY
    {
        WP_CW = 0,
        WP_CCW = 1,
        WP_SHORTEST = 2
    };

    struct RESULT
    {
        RESULT()
        {
            for( int i = 0; i < MaxWalkPolicies; i++ )
                status [i] = ST_NONE;
        }

        STATUS status[ MaxWalkPolicies ];
        LINE lines[ MaxWalkPolicies ];

        /// KICLOUD: R3.1b per-obstacle side memory: per policy, how many obstacle clusters were
        /// walked with the other winding than the policy's own because the winding preference
        /// asked for it (0: the line is the one the policy gives without a preference), and how
        /// many were walked with the policy's own winding because the preference had no side for
        /// them (0: every cluster's winding came from the preference, so the other policy, walked
        /// with the same preference, gives the same line). Reset by start(), counted by
        /// singleStep(); both stay 0 without a preference.
        int  overrides[ MaxWalkPolicies ] = {};
        int  freeChoices[ MaxWalkPolicies ] = {};

        /// KICLOUD: R3.1b, for the measurements (LINE_PLACER::LastWalkStats): the walk's
        /// iterations (singleStep() calls) and the clusters walked a second time with the other
        /// winding because the preferred one failed or gave the wrong side.
        int  iterations = 0;
        int  retries = 0;
    };

    /**
     * KICLOUD: R3.1b per-obstacle side memory (docs/patches.md). A function that says on which
     * side of the line an obstacle item should stay: +1 or -1 as ObstacleSide() measures it, or
     * no value when it has no preference for that item. Set by the caller (LINE_PLACER) from the
     * sides the route head showed on its earlier steps; WALKAROUND only reads it.
     */
    using WINDING_PREFERENCE = std::function<std::optional<int>( const ITEM* aObstacle )>;

    void SetWorld( NODE* aNode )
    {
        m_world = aNode;
    }

    void SetIterationLimit( const int aIterLimit )
    {
        m_iterationLimit = aIterLimit;
    }

    void SetSolidsOnly( bool aSolidsOnly )
    {
        if( aSolidsOnly )
            m_itemMask = ITEM::SOLID_T;
        else
            m_itemMask = ITEM::ANY_T;
    }

    void SetItemMask( int aMask )
    {
        m_itemMask = aMask;
    }

    void SetForceWinding ( bool aEnabled, bool aCw )
    {
        m_forceCw = aCw;
        m_forceWinding = aEnabled;
    }

    void SetPickShortestPath( bool aEnabled )
    {
        m_useShortestPath = aEnabled;
    }

    void RestrictToCluster( bool aEnabled, const TOPOLOGY::CLUSTER& aCluster );

    STATUS Route( const LINE& aInitialPath, LINE& aWalkPath,
            bool aOptimize = true );

    const RESULT Route( const LINE& aInitialPath );

    void SetLengthLimit( bool aEnable, double aLengthExpansionFactor )
    {
        m_lengthLimitOn = aEnable;
        m_lengthExpansionFactor = aLengthExpansionFactor;
    }

    void SetAllowedPolicies( std::vector<WALK_POLICY> aPolicies);

    /**
     * KICLOUD: R3.1b per-obstacle side memory: with \a aPreference set, the WP_CW and WP_CCW
     * policies walk each obstacle cluster they meet so that it stays on the side the preference
     * names for the item they hit (or for the first cluster member it names), and with the
     * policy's own winding only when it names none (RESULT::overrides and freeChoices count
     * both). The other winding is walked only when the preferred one fails or clearly puts the
     * obstacle on the other side (measured on the part of the line that cluster's walk
     * changed). An empty function (the default) gives KiCad 10's walk. WP_SHORTEST ignores it.
     * Caller: LINE_PLACER::walkInitialTrack(); valid until the next call.
     */
    void SetWindingPreference( WINDING_PREFERENCE aPreference )
    {
        m_windingPreference = std::move( aPreference );
    }

    /**
     * KICLOUD: R3.1b: on which side of \a aLine the point \a aPoint lies, seen along the line's
     * direction at the line's point nearest to it: +1 when the cross product of that direction
     * and (aPoint - nearest point) is positive (KiCad's y axis points down, so +1 is to the right
     * of the direction of travel), -1 when it is negative; at a corner the direction is the mean of the two
     * segments' directions. 0 when the line has no segment, the nearest point is the line's
     * first or last point (the point lies behind the start or ahead of the end: the line does
     * not pass it), the point lies on the line (within 1 nm) or the two directions at a corner
     * cancel out (a turn-back). The same rule as the routing plan's R-J1 measurement. Pure.
     * Callers: singleStep() (checking a preferred winding) and LINE_PLACER::acceptWalk().
     */
    static int ObstacleSide( const SHAPE_LINE_CHAIN& aLine, const VECTOR2I& aPoint );

    /// KICLOUD: R3.1b: the point ObstacleSide() measures an obstacle item by: the centre of the
    /// bounding box on \a aLayer of its shape, or for a hole of its pad's or via's shape (a hole
    /// counts as its pad or via, also with an offset drill), or no value when there is no shape
    /// there. Pure.
    static std::optional<VECTOR2I> ObstacleAnchor( const ITEM* aItem, int aLayer );

private:
    /// KICLOUD: R3.1b: the part of \a aAfter (a line after one cluster's walk) that differs from
    /// \a aBefore (the line before it), with one unchanged point on each side, or the whole of
    /// \a aAfter when nothing in it can be told apart. singleStep() measures the walked obstacle's
    /// side on it. Pure.
    static SHAPE_LINE_CHAIN changedPart( const SHAPE_LINE_CHAIN& aBefore,
                                         const SHAPE_LINE_CHAIN& aAfter );

    void start( const LINE& aInitialPath );
    bool singleStep();

    NODE::OPT_OBSTACLE nearestObstacle( const LINE& aPath );
    NODE* m_world;

    int m_iteration;
    int m_iterationLimit;
    int m_itemMask;
    bool m_forceWinding;
    bool m_forceCw;
    VECTOR2I m_cursorPos;
    VECTOR2I m_lastP;
    std::set<const ITEM*> m_restrictedSet;
    std::vector<VECTOR2I> m_restrictedVertices;
    bool m_forceLongerPath;
    bool m_lengthLimitOn;
    bool m_useShortestPath;
    double m_lengthExpansionFactor;
    bool m_enabledPolicies[ MaxWalkPolicies ];
    NODE::OPT_OBSTACLE m_currentObstacle[ MaxWalkPolicies ];
    TOPOLOGY::CLUSTER m_currentCluster[ MaxWalkPolicies ];
    std::set<ITEM*> m_processedItems;
    RESULT m_currentResult;
    double m_initialLength;
    WINDING_PREFERENCE m_windingPreference;   ///< KICLOUD: R3.1b (SetWindingPreference)
};

}

#endif    // __PNS_WALKAROUND_H
