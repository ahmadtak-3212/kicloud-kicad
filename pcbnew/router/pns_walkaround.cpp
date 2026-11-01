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

#include <chrono>
#include <cmath>      // KICLOUD: R3.1b (std::hypot)
#include <optional>

#include <advanced_config.h>
#include <core/typeinfo.h>
#include <geometry/shape_line_chain.h>

#include "pns_walkaround.h"
#include "pns_optimizer.h"
#include "pns_router.h"
#include "pns_debug_decorator.h"
#include "pns_solid.h"


namespace PNS {

void WALKAROUND::start( const LINE& aInitialPath )
{
    m_iteration = 0;
    for( int pol = 0 ; pol < MaxWalkPolicies; pol++)
    {
        m_currentResult.status[ pol ] = ST_IN_PROGRESS;
        m_currentResult.lines[ pol ] = aInitialPath;
        m_currentResult.lines[ pol ].ClearLinks();
        m_currentResult.overrides[ pol ] = 0;     // KICLOUD: R3.1b
        m_currentResult.freeChoices[ pol ] = 0;
    }

    m_currentResult.iterations = 0;               // KICLOUD: R3.1b
    m_currentResult.retries = 0;
    m_currentResult.hitLimit = false;
}


// KICLOUD: R3.1b per-obstacle side memory (docs/patches.md); see the comment in the header.
int WALKAROUND::ObstacleSide( const SHAPE_LINE_CHAIN& aLine, const VECTOR2I& aPoint )
{
    const int segCount = aLine.SegmentCount();

    if( segCount < 1 )
        return 0;

    // The segment nearest to the point (the first one on a tie) and the nearest point on it.
    int        best = -1;
    SEG::ecoord bestDist = 0;
    VECTOR2I   nearest;

    for( int i = 0; i < segCount; i++ )
    {
        const SEG      seg = aLine.CSegment( i );
        const VECTOR2I p = seg.NearestPoint( aPoint );
        const SEG::ecoord d = ( aPoint - p ).SquaredEuclideanNorm();

        if( best < 0 || d < bestDist )
        {
            best = i;
            bestDist = d;
            nearest = p;
        }
    }

    const SEG seg = aLine.CSegment( best );

    // Behind the start or ahead of the end: the line does not pass the point.
    if( ( best == 0 && nearest == seg.A ) || ( best == segCount - 1 && nearest == seg.B ) )
        return 0;

    auto unit = []( const VECTOR2I& aV )
            {
                const double l = aV.EuclideanNorm();
                return l > 0 ? VECTOR2D( aV.x / l, aV.y / l ) : VECTOR2D( 0, 0 );
            };

    VECTOR2D dir = unit( seg.B - seg.A );

    // At a corner the direction is the mean of the two segments' directions.
    if( nearest == seg.B && best + 1 < segCount )
    {
        const SEG next = aLine.CSegment( best + 1 );
        dir = dir + unit( next.B - next.A );
    }
    else if( nearest == seg.A && best > 0 )
    {
        const SEG prev = aLine.CSegment( best - 1 );
        dir = dir + unit( prev.B - prev.A );
    }

    const double dirLength = std::hypot( dir.x, dir.y );

    // Opposite directions at a turn-back corner: no direction, no side.
    if( dirLength < 1e-9 )
        return 0;

    const double cross = dir.x * (double) ( aPoint.y - nearest.y )
                         - dir.y * (double) ( aPoint.x - nearest.x );

    // The point on the line itself (within 1 nm): no side.
    if( std::abs( cross ) <= dirLength )
        return 0;

    return cross > 0.0 ? 1 : -1;
}


// KICLOUD: R3.1b per-obstacle side memory (docs/patches.md); see the comment in the header.
SHAPE_LINE_CHAIN WALKAROUND::changedPart( const SHAPE_LINE_CHAIN& aBefore,
                                          const SHAPE_LINE_CHAIN& aAfter )
{
    const int n = aAfter.PointCount();
    const int m = aBefore.PointCount();
    int       prefix = 0;
    int       suffix = 0;

    while( prefix < n && prefix < m && aAfter.CPoint( prefix ) == aBefore.CPoint( prefix ) )
        prefix++;

    while( suffix < n - prefix && suffix < m - prefix
           && aAfter.CPoint( n - 1 - suffix ) == aBefore.CPoint( m - 1 - suffix ) )
    {
        suffix++;
    }

    // One point on each side of the changed points, so the changed segments are whole.
    const int first = std::max( 0, prefix - 1 );
    const int last = std::min( n - 1, n - suffix );

    if( last - first < 1 )
        return aAfter;

    return aAfter.Slice( first, last );
}


std::optional<VECTOR2I> WALKAROUND::ObstacleAnchor( const ITEM* aItem, int aLayer )
{
    // A hole is measured as its pad or via, so both get the same anchor (and key).
    const ITEM*  owner = aItem && aItem->ParentPadVia() ? aItem->ParentPadVia() : aItem;
    const SHAPE* shape = owner ? owner->Shape( aLayer ) : nullptr;

    if( !shape )
        return std::nullopt;

    return shape->BBox().Centre();
}


NODE::OPT_OBSTACLE WALKAROUND::nearestObstacle( const LINE& aPath )
{
    COLLISION_SEARCH_OPTIONS opts;

    opts.m_kindMask = m_itemMask;

    if( ! m_restrictedSet.empty() )
    {
        opts.m_filter = [ this ] ( const ITEM* item ) -> bool
        {
            if( m_restrictedSet.find( item ) != m_restrictedSet.end() )
                return true;
            return false;
        };
    }

    opts.m_useClearanceEpsilon = true;
    return m_world->NearestObstacle( &aPath, opts );
}


void WALKAROUND::RestrictToCluster( bool aEnabled, const TOPOLOGY::CLUSTER& aCluster )
{
    m_restrictedVertices.clear();
    m_restrictedSet.clear();

    if( aEnabled )
    {
        for( ITEM* item : aCluster.m_items )
        {
            m_restrictedSet.insert( item );

            if ( item->HasHole() )
                m_restrictedSet.insert( item->Hole() );
        }
    }

    for( ITEM* item : aCluster.m_items )
    {
        if( SOLID* solid = dyn_cast<SOLID*>( item ) )
            m_restrictedVertices.push_back( solid->Anchor( 0 ) );
    }
}

bool WALKAROUND::singleStep()
{
    TOPOLOGY topo( m_world );
    TOPOLOGY::CLUSTER pendingClusters[MaxWalkPolicies];
    const ITEM*       hitItems[MaxWalkPolicies] = {};   // KICLOUD: R3.1b: the obstacle each policy hit

    for( int i = 0; i < MaxWalkPolicies; i++ )
    {
        if( !m_enabledPolicies[i] )
            continue;

        auto& line = m_currentResult.lines[ i ];
        auto& status = m_currentResult.status[ i ];

        PNS_DBG( Dbg(), AddItem, &line, WHITE, 10000, wxString::Format( "current (policy %d, stat %d)", i, status ) );

        if( status != ST_IN_PROGRESS )
            continue;

        auto obstacle = nearestObstacle( line );

        if( !obstacle )
        {

            m_currentResult.status[ i ] = ST_DONE;
            PNS_DBG( Dbg(), Message,  wxString::Format( "no-more-colls pol %d st %d", i, status ) );

            continue;
        }


        pendingClusters[ i ] = topo.AssembleCluster( obstacle->m_item, line.Layer(), 0.0, line.Net() );
        hitItems[ i ] = obstacle->m_item;
        PNS_DBG( Dbg(), AddItem, obstacle->m_item, BLUE, 10000, wxString::Format( "col-item owner-depth %d cl-items=%d", static_cast<const NODE*>( obstacle->m_item->Owner() )->Depth(), (int) pendingClusters[i].m_items.size() ) );

    }

    DIRECTION_45::CORNER_MODE cornerMode = Settings().GetCornerMode();

    auto processCluster = [ & ] ( TOPOLOGY::CLUSTER& aCluster, LINE& aLine, bool aCw ) -> bool
    {
        using namespace std::chrono;
        auto start_time = steady_clock::now();

        int timeout_ms = ADVANCED_CFG::GetCfg().m_PNSProcessClusterTimeout;

        PNS_DBG( Dbg(), BeginGroup, wxString::Format( "cluster-details [cw %d]", aCw?1:0 ), 1 );

        for( auto& clItem : aCluster.m_items )
        {
            // Check for wallclock timeout
            // Emprically, 100ms seems to be about where you're not going to find a valid path
            // if you haven't found it by then.  This allows the user to adjust their mouse position
            // to get a better path without waiting too long.
            auto now = steady_clock::now();
            auto elapsed = duration_cast<milliseconds>( now - start_time ).count();

            if( elapsed > timeout_ms )
            {
                PNS_DBG( Dbg(), Message, wxString::Format( "processCluster timeout after %d ms", timeout_ms ) );
                PNS_DBGN( Dbg(), EndGroup );
                return false;
            }

            int clearance = m_world->GetClearance( clItem, &aLine, false );
            const SHAPE_LINE_CHAIN& cachedHull = m_world->GetRuleResolver()->HullCache(
                    clItem, clearance, aLine.Width(), aLine.Layer() );

            SHAPE_LINE_CHAIN hull;

            if( cornerMode == DIRECTION_45::MITERED_90 || cornerMode == DIRECTION_45::ROUNDED_90 )
            {
                BOX2I bbox = cachedHull.BBox();
                hull.Append( bbox.GetLeft(),  bbox.GetTop()    );
                hull.Append( bbox.GetRight(), bbox.GetTop()    );
                hull.Append( bbox.GetRight(), bbox.GetBottom() );
                hull.Append( bbox.GetLeft(),  bbox.GetBottom() );
            }
            else
            {
                hull = cachedHull;
            }

            LINE tmp( aLine );

            aLine.Line().Simplify2();

            bool stat = aLine.Walkaround( hull, tmp.Line(), aCw );

            PNS_DBG( Dbg(), AddShape, &hull, YELLOW, 10000, wxString::Format( "hull stat %d", stat?1:0 ) );
            PNS_DBG( Dbg(), AddItem, &tmp, RED, 10000, wxString::Format( "walk stat %d", stat?1:0 ) );
            PNS_DBG( Dbg(), AddItem, clItem, WHITE, 10000, wxString::Format( "item stat %d", stat?1:0 ) );

            if( !stat )
            {
                PNS_DBGN( Dbg(), EndGroup );
                return false;
            }

            aLine.SetShape( tmp.CLine() );
        }

        PNS_DBGN( Dbg(), EndGroup );

        return true;
    };

    // KICLOUD: R3.1b per-obstacle side memory (docs/patches.md): walks policy aPol's pending
    // cluster. Without a winding preference, or when it names no side for the cluster, with the
    // policy's own winding aCw, as KiCad 10. Otherwise with the winding that keeps the obstacle on
    // the preferred side: the clockwise walk puts the obstacle to the right of the direction of
    // travel (KiCad's hulls are clockwise in its y-down coordinates), and the result is checked
    // with ObstacleSide(); when that winding fails or ends on the wrong side, the other winding is
    // tried, and when neither keeps the side the policy's own winding is used. Counts the cluster
    // in m_currentResult.overrides (walked against aCw) or freeChoices (no preferred side).
    // Result: processCluster()'s (false: the walk is stuck).
    auto walkPolicy = [&]( int aPol, bool aCw ) -> bool
    {
        TOPOLOGY::CLUSTER& cluster = pendingClusters[aPol];
        LINE&              line = m_currentResult.lines[aPol];

        if( !m_windingPreference || cluster.m_items.empty() )
            return processCluster( cluster, line, aCw );

        // The preferred side of the item hit first, else of the first cluster member that has one.
        std::optional<int>      side;
        std::optional<VECTOR2I> anchor;

        auto tryItem = [&]( const ITEM* aItem )
                {
                    if( side || !aItem )
                        return;

                    std::optional<int> s = m_windingPreference( aItem );

                    if( !s || *s == 0 )
                        return;

                    anchor = ObstacleAnchor( aItem, line.Layer() );

                    if( anchor )
                        side = s;
                };

        tryItem( hitItems[aPol] );

        for( const ITEM* item : cluster.m_items )
            tryItem( item );

        if( !side )
        {
            m_currentResult.freeChoices[aPol]++;
            return processCluster( cluster, line, aCw );
        }

        const LINE before( line );
        bool       cw = *side > 0;
        bool       ok = processCluster( cluster, line, cw );

        // The side is measured on the part of the line this cluster's walk changed (a line that
        // passes the same obstacle twice, as in a U-turn, is not judged by its other pass). The
        // other winding is walked only when this one failed or puts the obstacle on the other
        // side for sure (0, no clear side, keeps this result): with KiCad's clockwise hulls the
        // preferred winding normally gives the side at once.
        const int measured = ok ? ObstacleSide( changedPart( before.CLine(), line.CLine() ), *anchor )
                                : 0;

        if( !ok || measured == -*side )
        {
            LINE retry( before );
            bool okRetry = processCluster( cluster, retry, !cw );

            m_currentResult.retries++;   // KICLOUD: R3.1b (measurement)

            if( okRetry
                    && ObstacleSide( changedPart( before.CLine(), retry.CLine() ), *anchor ) != -*side )
            {
                line = retry;
                ok = true;
                cw = !cw;
            }
            else if( cw != aCw )
            {
                // Neither winding keeps the side: the policy's own (the retry's), as without one.
                line = retry;
                ok = okRetry;
                cw = aCw;
            }
        }

        if( cw != aCw )
            m_currentResult.overrides[aPol]++;

        return ok;
    };

    if ( m_enabledPolicies[WP_CW] )
    {
        bool stat = walkPolicy( WP_CW, true );
        if( !stat )
            m_currentResult.status[ WP_CW ] = ST_STUCK;
    }

    if ( m_enabledPolicies[WP_CCW] )
    {
        bool stat = walkPolicy( WP_CCW, false );
        if( !stat )
            m_currentResult.status[ WP_CCW ] = ST_STUCK;
    }

    if( m_enabledPolicies[WP_SHORTEST] )
    {
        LINE& line = m_currentResult.lines[WP_SHORTEST];
        LINE  path_cw( line ), path_ccw( line );

        auto st_cw = processCluster( pendingClusters[WP_SHORTEST], path_cw, true );
        auto st_ccw = processCluster( pendingClusters[WP_SHORTEST], path_ccw, false );

        bool cw_coll = st_cw ? m_world->CheckColliding( &path_cw ).has_value() : false;
        bool ccw_coll = st_ccw ? m_world->CheckColliding( &path_ccw ).has_value() : false;

        double lengthFactorCw = (double) path_cw.CLine().Length() / (double) m_initialLength;
        double lengthFactorCcw = (double) path_ccw.CLine().Length() / (double) m_initialLength;

        PNS_DBG( Dbg(), AddItem, &path_cw, RED, 10000, wxString::Format( "shortest-cw stat %d lf %.1f", st_cw?1:0, lengthFactorCw ) );
        PNS_DBG( Dbg(), AddItem, &path_ccw, BLUE, 10000, wxString::Format( "shortest-ccw stat %d lf %.1f", st_ccw?1:0, lengthFactorCcw ) );


        std::optional<LINE> shortest;
        std::optional<LINE> shortest_alt;


        if( st_cw && st_ccw )
        {
            if( ( !cw_coll && !ccw_coll ) || ( cw_coll && ccw_coll ) )
            {
                if( path_cw.CLine().Length() > path_ccw.CLine().Length() )
                {
                    shortest = path_ccw;
                    shortest_alt = path_cw;
                }
                else
                {
                    shortest = path_cw;
                    shortest_alt = path_ccw;
                }
            }
            else if( !cw_coll )
                shortest = path_cw;
            else if( !ccw_coll )
                shortest = path_ccw;

        }
        else if( st_ccw )
            shortest = path_ccw;
        else if( st_cw )
            shortest = path_cw;

        bool anyColliding = false;

        if( shortest.has_value() )
        {
            PNS_DBG( Dbg(), AddItem, &shortest.value(), RED, 10000, wxString::Format( "shortest-l" ) );

            for( auto& item : m_processedItems )
            {
                std::set<PNS::OBSTACLE> obstacles;
                PNS::COLLISION_SEARCH_CONTEXT ctx( obstacles );
                if( shortest->Collide( item, m_world, shortest->Layer(), &ctx ) )
                {
                    anyColliding = true;
                    break;
                }
            }

            PNS_DBG( Dbg(), Message, wxString::Format("check-back cc %d items %d coll %d", (int) pendingClusters[ WP_SHORTEST ].m_items.size(), (int) m_processedItems.size(), anyColliding ? 1: 0 ) );
        }

        if ( anyColliding )
        {
            shortest = std::move( shortest_alt );
        }

        if( !shortest )
        {
            m_currentResult.status[WP_SHORTEST] = ST_STUCK;
        }
        else
        {
            m_currentResult.lines[WP_SHORTEST] = *shortest;
        }

        for( auto item : pendingClusters[ WP_SHORTEST ].m_items )
            m_processedItems.insert( item );
    }

    return ST_IN_PROGRESS;
}


const WALKAROUND::RESULT WALKAROUND::Route( const LINE& aInitialPath )
{
    RESULT result;

    m_initialLength = aInitialPath.CLine().Length();

    // special case for via-in-the-middle-of-track placement

#if 0
    if( aInitialPath.PointCount() <= 1 )
    {
        if( aInitialPath.EndsWithVia() && m_world->CheckColliding( &aInitialPath.Via(),
                                                                   m_itemMask ) )
            {
                // fixme restult
            }
            //return RESULT( STUCK, STUCK );

        return RESULT(); //( DONE, DONE, aInitialPath, aInitialPath );
    }
#endif

    start( aInitialPath );

    m_processedItems.clear();

    PNS_DBG( Dbg(), AddItem, &aInitialPath, WHITE, 10000, wxT( "initial-path" ) );

    while( m_iteration < m_iterationLimit )
    {
        singleStep();
        m_currentResult.iterations++;   // KICLOUD: R3.1b (measurement: singleStep() calls)

        bool stillInProgress = false;

        for( int pol = 0; pol < MaxWalkPolicies; pol++ )
        {
            if (!m_enabledPolicies[pol])
                continue;

            auto& st = m_currentResult.status[pol];
            auto& ln = m_currentResult.lines[pol];
            double lengthFactor = (double) ln.CLine().Length() / (double) aInitialPath.CLine().Length();
            // In some situations, there isn't a trivial path (or even a path at all).  Hitting the
            // iteration limit causes lag, so we can exit out early if the walkaround path gets very long
            // compared with the initial path.  If the length exceeds the initial length times this factor,
            // fail out.
            if( m_lengthLimitOn )
            {
                if( st != ST_DONE && lengthFactor > m_lengthExpansionFactor )
                    st = ST_ALMOST_DONE;
            }

            PNS_DBG( Dbg(), Message, wxString::Format( "check-wp iter %d st %d i %d lf %.1f", m_iteration, st, pol, lengthFactor ) );

            if ( st == ST_IN_PROGRESS )
                stillInProgress = true;
        }


        if( !stillInProgress )
            break;

        m_iteration++;
    }

    // KICLOUD: R3.1b: the walk stopped at the iteration limit, still in progress (its lines may be
    // cut short); LINE_PLACER::walkSides() then does not take it for the walk without the memory.
    m_currentResult.hitLimit = m_iteration >= m_iterationLimit;


    for( int pol = 0; pol < MaxWalkPolicies; pol++ )
    {
        auto&       st = m_currentResult.status[pol];
        const auto& ln = m_currentResult.lines[pol].CLine();

        m_currentResult.lines[pol].ClearLinks();
        if( st == ST_IN_PROGRESS )
            st = ST_ALMOST_DONE;

        if( ln.SegmentCount() < 1 || ln.CPoint( 0 ) != aInitialPath.CPoint( 0 ) )
        {
            st = ST_STUCK;
        }

        if( ln.PointCount() > 0 && ln.CLastPoint() != aInitialPath.CLastPoint() )
        {
            st = ST_ALMOST_DONE;

        }
        PNS_DBG( Dbg(), Message, wxString::Format( "stat=%d", st ) );

    }


    return m_currentResult;
}

void WALKAROUND::SetAllowedPolicies( std::vector<WALK_POLICY> aPolicies)
{
    for( int i = 0; i < MaxWalkPolicies; i++ )
        m_enabledPolicies[i] =  false;

    for ( auto p : aPolicies )
        m_enabledPolicies[p] = true;
}

}
