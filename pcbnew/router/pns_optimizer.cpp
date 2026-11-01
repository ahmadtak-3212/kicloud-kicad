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

#include <core/typeinfo.h>
#include <geometry/shape_line_chain.h>
#include <geometry/shape_rect.h>
#include <geometry/shape_simple.h>

#include <cmath>
#include <set>     // KICLOUD: R3.2 (glossing)
#include <tuple>   // KICLOUD: R3.2 (GLOSS_SCORE)

#include "pns_arc.h"
#include "pns_joint.h"     // KICLOUD: R3.2 (glossSegmentIsNew)
#include "pns_segment.h"   // KICLOUD: R3.2 (glossContacts)
#include "pns_line.h"
#include "pns_diff_pair.h"
#include "pns_node.h"
#include "pns_solid.h"
#include "pns_optimizer.h"

#include "pns_utils.h"
#include "pns_router.h"
#include "pns_debug_decorator.h"


namespace PNS {


int COST_ESTIMATOR::CornerCost( const SEG& aA, const SEG& aB )
{
    DIRECTION_45 dir_a( aA ), dir_b( aB );

    switch( dir_a.Angle( dir_b ) )
    {
    case DIRECTION_45::ANG_OBTUSE:    return 10;
    case DIRECTION_45::ANG_STRAIGHT:  return 5;
    case DIRECTION_45::ANG_ACUTE:     return 50;
    case DIRECTION_45::ANG_RIGHT:     return 30;
    case DIRECTION_45::ANG_HALF_FULL: return 60;
    default:                          return 100;
    }
}


int COST_ESTIMATOR::CornerCost( const SHAPE_LINE_CHAIN& aLine )
{
    int total = 0;

    for( int i = 0; i < aLine.SegmentCount() - 1; ++i )
        total += CornerCost( aLine.CSegment( i ), aLine.CSegment( i + 1 ) );

    return total;
}


int COST_ESTIMATOR::CornerCost( const LINE& aLine )
{
    return CornerCost( aLine.CLine() );
}


void COST_ESTIMATOR::Add( const LINE& aLine )
{
    m_lengthCost += aLine.CLine().Length();
    m_cornerCost += CornerCost( aLine );
}


void COST_ESTIMATOR::Remove( const LINE& aLine )
{
    m_lengthCost -= aLine.CLine().Length();
    m_cornerCost -= CornerCost( aLine );
}


void COST_ESTIMATOR::Replace( const LINE& aOldLine, const LINE& aNewLine )
{
    m_lengthCost -= aOldLine.CLine().Length();
    m_cornerCost -= CornerCost( aOldLine );
    m_lengthCost += aNewLine.CLine().Length();
    m_cornerCost += CornerCost( aNewLine );
}


bool COST_ESTIMATOR::IsBetter( const COST_ESTIMATOR& aOther, double aLengthTolerance,
                               double aCornerTolerance ) const
{
    if( aOther.m_cornerCost < m_cornerCost && aOther.m_lengthCost < m_lengthCost )
        return true;
    else if( aOther.m_cornerCost < m_cornerCost * aCornerTolerance &&
             aOther.m_lengthCost < m_lengthCost * aLengthTolerance )
        return true;

    return false;
}


OPTIMIZER::OPTIMIZER( NODE* aWorld ) :
    m_world( aWorld ),
    m_collisionKindMask( ITEM::ANY_T ),
    m_effortLevel( MERGE_SEGMENTS ),
    m_restrictAreaIsStrict( false ),
    m_glossReference( nullptr ),   // KICLOUD: R3.2 (the four gloss members)
    m_glossTailSegments( 0 ),
    m_glossBudget( GlossDefaultBudget ),
    m_glossBudgetHit( false )
{
}


OPTIMIZER::~OPTIMIZER()
{
    for( OPT_CONSTRAINT* c : m_constraints )
        delete c;

    m_constraints.clear();
}


struct OPTIMIZER::CACHE_VISITOR
{
    CACHE_VISITOR( const ITEM* aOurItem, NODE* aNode, int aMask ) :
        m_ourItem( aOurItem ),
        m_collidingItem( nullptr ),
        m_node( aNode ),
        m_mask( aMask )
    {}

    bool operator()( ITEM* aOtherItem )
    {
        if( !( m_mask & aOtherItem->Kind() ) )
            return true;

        // TODO(JE) viastacks
        if( !aOtherItem->Collide( m_ourItem, m_node, m_ourItem->Layer() ) )
            return true;

        m_collidingItem = aOtherItem;
        return false;
    }

    const ITEM* m_ourItem;
    ITEM* m_collidingItem;
    NODE* m_node;
    int m_mask;
};


void OPTIMIZER::cacheAdd( ITEM* aItem, bool aIsStatic = false )
{
    if( m_cacheTags.find( aItem ) != m_cacheTags.end() )
        return;

    m_cache.Add( aItem );
    m_cacheTags[aItem].m_hits = 1;
    m_cacheTags[aItem].m_isStatic = aIsStatic;
}


void OPTIMIZER::removeCachedSegments( LINE* aLine, int aStartVertex, int aEndVertex )
{
    if( !aLine->IsLinked() )
        return;

    auto links = aLine->Links();

    if( aEndVertex < 0 )
        aEndVertex += aLine->PointCount();

    for( int i = aStartVertex; i < aEndVertex - 1; i++ )
    {
        LINKED_ITEM* s = links[i];
        m_cacheTags.erase( s );
        m_cache.Remove( s );
    }
}


void OPTIMIZER::CacheRemove( ITEM* aItem )
{
    if( aItem->Kind() == ITEM::LINE_T )
        removeCachedSegments( static_cast<LINE*>( aItem ) );
}


void OPTIMIZER::ClearCache( bool aStaticOnly )
{
    if( !aStaticOnly )
    {
        m_cacheTags.clear();
        m_cache.Clear();
        return;
    }

    for( auto i = m_cacheTags.begin(); i!= m_cacheTags.end(); ++i )
    {
        if( i->second.m_isStatic )
        {
            m_cache.Remove( i->first );
            m_cacheTags.erase( i->first );
        }
    }
}


bool AREA_CONSTRAINT::Check( int aVertex1, int aVertex2, const LINE* aOriginLine,
                             const SHAPE_LINE_CHAIN& aCurrentPath,
                             const SHAPE_LINE_CHAIN& aReplacement )
{
    const VECTOR2I& p1 = aCurrentPath.CPoint( aVertex1 );
    const VECTOR2I& p2 = aCurrentPath.CPoint( aVertex2 );

    bool p1_in = m_allowedArea.Contains( p1 );
    bool p2_in = m_allowedArea.Contains( p2 );

    if( p1_in && p2_in )
        return true;

    if( aVertex1 < aCurrentPath.PointCount() - 1 && !p1_in && p2_in
        && m_allowedArea.Contains( aCurrentPath.CPoint( aVertex1 + 1 ) ) )
        return aReplacement.CSegment( 0 ).Angle( aCurrentPath.CSegment( aVertex1 ) ).IsHorizontal();

    if( p1_in && !p2_in && m_allowedArea.Contains( aCurrentPath.CPoint( aVertex2 - 1 ) ) )
        return aReplacement.CSegment( -1 )
                .Angle( aCurrentPath.CSegment( aVertex2 - 1 ) )
                .IsHorizontal();

    //PNS_DBG( dbg, AddShape, m_allowedArea, YELLOW, 10000, wxT( "drag-affected-area" ) );
    //PNS_DBG( dbg, AddPoint, p1, YELLOW, 1000000, wxT( "drag-p1" ) );
    //PNS_DBG( dbg, AddPoint, p2, YELLOW, 1000000, wxT( "drag-p2" ) );

    return false;
}


bool PRESERVE_VERTEX_CONSTRAINT::Check( int aVertex1, int aVertex2, const LINE* aOriginLine,
                                        const SHAPE_LINE_CHAIN& aCurrentPath,
                                        const SHAPE_LINE_CHAIN& aReplacement )
{
    bool cv = false;

    for( int i = aVertex1; i < aVertex2; i++ )
    {
        SEG::ecoord dist = aCurrentPath.CSegment(i).SquaredDistance( m_v );

        if ( dist <= 1 )
        {
            cv = true;
            break;
        }
    }

    if( !cv )
        return true;

    for( int i = 0; i < aReplacement.SegmentCount(); i++ )
    {
        SEG::ecoord dist = aReplacement.CSegment(i).SquaredDistance( m_v );

        if ( dist <= 1 )
            return true;
    }

    return false;
}


bool RESTRICT_VERTEX_RANGE_CONSTRAINT::Check( int aVertex1, int aVertex2, const LINE* aOriginLine,
                                              const SHAPE_LINE_CHAIN& aCurrentPath,
                                              const SHAPE_LINE_CHAIN& aReplacement )
{
    return true;
}


bool CORNER_COUNT_LIMIT_CONSTRAINT::Check( int aVertex1, int aVertex2, const LINE* aOriginLine,
                                           const SHAPE_LINE_CHAIN& aCurrentPath,
                                           const SHAPE_LINE_CHAIN& aReplacement )
{
    LINE newPath( *aOriginLine, aCurrentPath );
    newPath.Line().Replace( aVertex1, aVertex2, aReplacement );
    newPath.Line().Simplify2();
    int cc = newPath.CountCorners( m_angleMask );

    if( cc >= m_minCorners )
        return true;

    // fixme: something fishy with the max corneriness limit
    // (cc <= m_maxCorners)

    return true;
}


/**
 * Determine if a point is located within a given polygon
 *
 * @todo fixme: integrate into SHAPE_LINE_CHAIN, check corner cases against current PointInside
 *       implementation
 *
 * @param aL Polygon
 * @param aP Point to check for location within the polygon
 *
 * @return false if point is not polygon boundary aL, true if within or on the polygon boundary
 */
static bool pointInside2( const SHAPE_LINE_CHAIN& aL, const VECTOR2I& aP )
{
    if( !aL.IsClosed() || aL.SegmentCount() < 3 )
        return false;

    int result  = 0;
    size_t cnt  = aL.PointCount();

    VECTOR2I ip = aL.CPoint( 0 );

    for( size_t i = 1; i <= cnt; ++i )
    {
        VECTOR2I ipNext = ( i == cnt ? aL.CPoint( 0 ) : aL.CPoint( i ) );

        if( ipNext.y == aP.y )
        {
            if( ( ipNext.x == aP.x )
                || ( ip.y == aP.y && ( ( ipNext.x > aP.x ) == ( ip.x < aP.x ) ) ) )
                return true; // pt on polyground boundary
        }

        if( ( ip.y < aP.y ) != ( ipNext.y < aP.y ) )
        {
            if( ip.x >=aP.x )
            {
                if( ipNext.x >aP.x )
                {
                    result = 1 - result;
                }
                else
                {
                    double d = static_cast<double>( ip.x - aP.x ) *
                               static_cast<double>( ipNext.y - aP.y ) -
                               static_cast<double>( ipNext.x - aP.x ) *
                               static_cast<double>( ip.y - aP.y );

                    if( !d )
                        return true;  // pt on polyground boundary

                    if( ( d > 0 ) == ( ipNext.y > ip.y ) )
                        result = 1 - result;
                }
            }
            else
            {
                if( ipNext.x >aP.x )
                {
                    double d = ( (double) ip.x - aP.x ) * ( (double) ipNext.y - aP.y )
                               - ( (double) ipNext.x - aP.x ) * ( (double) ip.y - aP.y );

                    if( !d )
                        return true;  // pt on polyground boundary

                    if( ( d > 0 ) == ( ipNext.y > ip.y ) )
                        result = 1 - result;
                }
            }
        }

        ip = ipNext;
    }

    return result > 0;
}


bool KEEP_TOPOLOGY_CONSTRAINT::Check( int aVertex1, int aVertex2, const LINE* aOriginLine,
                                      const SHAPE_LINE_CHAIN& aCurrentPath,
                                      const SHAPE_LINE_CHAIN& aReplacement )
{
    SHAPE_LINE_CHAIN encPoly = aOriginLine->CLine().Slice( aVertex1, aVertex2 );

    // fixme: this is a remarkably shitty implementation...
    encPoly.Append( aReplacement.Reverse() );
    encPoly.SetClosed( true );

    BOX2I bb = encPoly.BBox();
    std::vector<JOINT*> joints;

    int cnt = m_world->QueryJoints( bb, joints, aOriginLine->Layers(), ITEM::SOLID_T );

    if( !cnt )
        return true;

    for( JOINT* j : joints )
    {
        if( j->Net() == aOriginLine->Net() )
            continue;

        if( pointInside2( encPoly, j->Pos() ) )
        {
            bool falsePositive = false;

            for( int k = 0; k < encPoly.PointCount(); k++ )
            {
                if( encPoly.CPoint(k) == j->Pos() )
                {
                    falsePositive = true;
                    break;
                }
            }

            if( !falsePositive )
            {
                //dbg->AddPoint(j->Pos(), 5);
                return false;
            }
        }
    }

    return true;
}


bool OPTIMIZER::checkColliding( ITEM* aItem, bool aUpdateCache )
{
    CACHE_VISITOR v( aItem, m_world, m_collisionKindMask );

    return static_cast<bool>( m_world->CheckColliding( aItem ) );
}


void OPTIMIZER::addConstraint ( OPT_CONSTRAINT *aConstraint )
{
    m_constraints.push_back( aConstraint );
}


bool OPTIMIZER::checkConstraints( int aVertex1, int aVertex2, LINE* aOriginLine,
                                  const SHAPE_LINE_CHAIN& aCurrentPath,
                                  const SHAPE_LINE_CHAIN& aReplacement )
{
    for( OPT_CONSTRAINT* c : m_constraints )
    {
        if( !c->Check( aVertex1, aVertex2, aOriginLine, aCurrentPath, aReplacement ) )
            return false;
    }

    return true;
}


bool OPTIMIZER::checkColliding( LINE* aLine, const SHAPE_LINE_CHAIN& aOptPath )
{
    LINE tmp( *aLine, aOptPath );

    return checkColliding( &tmp );
}


bool OPTIMIZER::mergeObtuse( LINE* aLine )
{
    SHAPE_LINE_CHAIN& line = aLine->Line();

    int step = line.PointCount() - 3;
    int segs_pre = line.SegmentCount();

    if( step < 0 )
        return false;

    SHAPE_LINE_CHAIN current_path( line );

    while( true )
    {
        int n_segs = current_path.SegmentCount();
        int max_step = n_segs - 2;

        if( step > max_step )
            step = max_step;

        if( step < 2 )
        {
            line = std::move( current_path );
            return line.SegmentCount() < segs_pre;
        }

        bool found_anything = false;

        for( int n = 0; n < n_segs - step; n++ )
        {
            const SEG s1 = current_path.CSegment( n );
            const SEG s2 = current_path.CSegment( n + step );
            SEG s1opt, s2opt;

            if( DIRECTION_45( s1 ).IsObtuse( DIRECTION_45( s2 ) ) )
            {
                VECTOR2I ip = *s1.IntersectLines( s2 );

                s1opt = SEG( s1.A, ip );
                s2opt = SEG( ip, s2.B );

                if( DIRECTION_45( s1opt ).IsObtuse( DIRECTION_45( s2opt ) ) )
                {
                    SHAPE_LINE_CHAIN opt_path;
                    opt_path.Append( s1opt.A );
                    opt_path.Append( s1opt.B );
                    opt_path.Append( s2opt.B );

                    LINE opt_track( *aLine, opt_path );

                    if( !checkColliding( &opt_track ) )
                    {
                        current_path.Replace( s1.Index() + 1, s2.Index(), ip );

                        // removeCachedSegments(aLine, s1.Index(), s2.Index());
                        n_segs = current_path.SegmentCount();
                        found_anything = true;
                        break;
                    }
                }
            }
         }

        if( !found_anything )
        {
            if( step <= 2 )
            {
                line = std::move( current_path );
                return line.SegmentCount() < segs_pre;
            }

            step--;
        }
    }
}


bool OPTIMIZER::mergeFull( LINE* aLine )
{
    SHAPE_LINE_CHAIN& line = aLine->Line();
    int step = line.SegmentCount() - 1;

    int segs_pre = line.SegmentCount();

    line.Simplify2();

    if( step < 0 )
        return false;

    SHAPE_LINE_CHAIN current_path( line );

    while( true )
    {
        int n_segs = current_path.SegmentCount();
        int max_step = n_segs - 2;

        if( step > max_step )
            step = max_step;

        if( step < 1 )
            break;

        bool found_anything = mergeStep( aLine, current_path, step );

        if( !found_anything )
            step--;

        if( !step )
            break;
    }

    aLine->SetShape( current_path );

    return current_path.SegmentCount() < segs_pre;
}


bool OPTIMIZER::mergeColinear( LINE* aLine )
{
    SHAPE_LINE_CHAIN&          line   = aLine->Line();

    int nSegs = line.SegmentCount();

    for( int segIdx = 0; segIdx < line.SegmentCount() - 1; ++segIdx )
    {
        SEG s1 = line.CSegment( segIdx );
        SEG s2 = line.CSegment( segIdx + 1 );

        // Skip zero-length segs caused by abutting arcs
        if( s1.SquaredLength() == 0 || s2.SquaredLength() == 0 )
            continue;

        if( s1.Collinear( s2 ) && !line.IsPtOnArc( segIdx + 1 ) )
        {
            line.Remove( segIdx + 1 );
        }
    }

    return line.SegmentCount() < nSegs;
}


bool OPTIMIZER::Optimize( const LINE* aLine, LINE* aResult, LINE* aRoot )
{
    if( !aResult )
        return false;

    *aResult = *aLine;
    aResult->ClearLinks();

    bool hasArcs = aLine->ArcCount();
    bool rv = false;

    if( (m_effortLevel & LIMIT_CORNER_COUNT) && aRoot )
    {
        const int angleMask = DIRECTION_45::ANG_OBTUSE;
        int rootObtuseCorners = aRoot->CountCorners( angleMask );
        auto c = new CORNER_COUNT_LIMIT_CONSTRAINT( m_world, rootObtuseCorners,
                                                    aLine->SegmentCount(), angleMask );
        //PNS_DBG( dbg, Message,
         //        wxString::Format( "opt limit-corner-count root %d maxc %d mask %x",
           //                        rootObtuseCorners, aLine->SegmentCount(), angleMask ) );

        addConstraint( c );
    }

    if( m_effortLevel & PRESERVE_VERTEX )
    {
        auto c = new PRESERVE_VERTEX_CONSTRAINT( m_world, m_preservedVertex );
        addConstraint( c );
    }

    if( m_effortLevel & RESTRICT_VERTEX_RANGE )
    {
        auto c = new RESTRICT_VERTEX_RANGE_CONSTRAINT( m_world, m_restrictedVertexRange.first,
                                                       m_restrictedVertexRange.second );
        addConstraint( c );
    }

    if( m_effortLevel & RESTRICT_AREA )
    {
        auto c = new AREA_CONSTRAINT( m_world, m_restrictArea, m_restrictAreaIsStrict );
        SHAPE_RECT r( m_restrictArea );
        //PNS_DBG( dbg, AddShape, &r, YELLOW, 0, wxT( "area-constraint" ) );
        addConstraint( c );
    }

    if( m_effortLevel & KEEP_TOPOLOGY )
    {
        auto c = new KEEP_TOPOLOGY_CONSTRAINT( m_world );
        addConstraint( c );
    }

    // TODO: Fix for arcs
    if( !hasArcs && m_effortLevel & MERGE_SEGMENTS )
        rv |= mergeFull( aResult );

    // TODO: Fix for arcs
    if( !hasArcs && m_effortLevel & MERGE_OBTUSE )
        rv |= mergeObtuse( aResult );

    if( m_effortLevel & MERGE_COLINEAR )
        rv |= mergeColinear( aResult );

    // TODO: Fix for arcs
    if( !hasArcs && m_effortLevel & SMART_PADS )
        rv |= runSmartPads( aResult );

    // TODO: Fix for arcs
    if( !hasArcs && m_effortLevel & FANOUT_CLEANUP )
        rv |= fanoutCleanup( aResult );

    // KICLOUD: R3.2 glossing, last: it cleans what the passes above leave. aLine (the line as
    // given, still linked when it came from a node) tells it which same-net items it touches.
    if( !hasArcs && m_effortLevel & GLOSS )
        rv |= gloss( aResult, aLine );

    return rv;
}


bool OPTIMIZER::mergeStep( LINE* aLine, SHAPE_LINE_CHAIN& aCurrentPath, int step )
{
    int n_segs = aCurrentPath.SegmentCount();

    int cost_orig = COST_ESTIMATOR::CornerCost( aCurrentPath );

    if( aLine->SegmentCount() < 2 )
        return false;

    DIRECTION_45::CORNER_MODE cornerMode = ROUTER::GetInstance()->Settings().GetCornerMode();
    bool is90mode = cornerMode == DIRECTION_45::MITERED_90 || cornerMode == DIRECTION_45::ROUNDED_90;

    DIRECTION_45 orig_start( aLine->CSegment( 0 ), is90mode );
    DIRECTION_45 orig_end( aLine->CSegment( -1 ), is90mode );


    for( int n = 0; n < n_segs - step; n++ )
    {
        // Do not attempt to merge false segments that are part of an arc
        if( aCurrentPath.IsArcSegment( n )
            || aCurrentPath.IsArcSegment( static_cast<std::size_t>( n ) + step ) )
        {
            continue;
        }

        const SEG s1    = aCurrentPath.CSegment( n );
        const SEG s2    = aCurrentPath.CSegment( n + step );

        SHAPE_LINE_CHAIN path[2];
        SHAPE_LINE_CHAIN* picked = nullptr;
        int cost[2];

        for( int i = 0; i < 2; i++ )
        {
            SHAPE_LINE_CHAIN bypass = DIRECTION_45().BuildInitialTrace( s1.A, s2.B, i, cornerMode );
            cost[i] = INT_MAX;

            bool ok = false;

            if( !checkColliding( aLine, bypass ) )
            {
                ok = checkConstraints ( n, n + step + 1, aLine, aCurrentPath, bypass );
            }

            if( ok )
            {
                path[i] = aCurrentPath;
                path[i].Replace( s1.Index(), s2.Index(), bypass );
                path[i].Simplify2();
                cost[i] = COST_ESTIMATOR::CornerCost( path[i] );
            }
        }

        if( cost[0] < cost_orig && cost[0] < cost[1] )
            picked = &path[0];
        else if( cost[1] < cost_orig )
            picked = &path[1];

        if( picked )
        {
            n_segs = aCurrentPath.SegmentCount();
            aCurrentPath = *picked;
            return true;
        }
    }

    return false;
}


OPTIMIZER::BREAKOUT_LIST OPTIMIZER::circleBreakouts( int aWidth, const SHAPE* aShape,
                                                     bool aPermitDiagonal ) const
{
    BREAKOUT_LIST breakouts;

    for( EDA_ANGLE angle = ANGLE_0; angle < ANGLE_360; angle += ANGLE_45 )
    {
        const SHAPE_CIRCLE* cir = static_cast<const SHAPE_CIRCLE*>( aShape );
        SHAPE_LINE_CHAIN    l;
        VECTOR2I            p0 = cir->GetCenter();
        VECTOR2I            v0( cir->GetRadius() * M_SQRT2, 0 );

        RotatePoint( v0, -angle );

        l.Append( p0 );
        l.Append( p0 + v0 );
        breakouts.push_back( l );
    }

    return breakouts;
}


OPTIMIZER::BREAKOUT_LIST OPTIMIZER::customBreakouts( int aWidth, const ITEM* aItem,
                                                     bool aPermitDiagonal ) const
{
    BREAKOUT_LIST breakouts;
    const SHAPE_SIMPLE* convex = static_cast<const SHAPE_SIMPLE*>( aItem->Shape( -1 ) );

    BOX2I bbox = convex->BBox( 0 );
    VECTOR2I p0 = static_cast<const SOLID*>( aItem )->Pos();
    // must be large enough to guarantee intersecting the convex polygon
    int length = std::max( bbox.GetWidth(), bbox.GetHeight() ) / 2 + 5;
    EDA_ANGLE increment = ( aPermitDiagonal ? ANGLE_45 : ANGLE_90 );

    for( EDA_ANGLE angle = ANGLE_0; angle < ANGLE_360; angle += increment )
    {
        SHAPE_LINE_CHAIN l;
        VECTOR2I v0( p0 + VECTOR2I( length, 0 ) );
        RotatePoint( v0, p0, -angle );

        SHAPE_LINE_CHAIN::INTERSECTIONS intersections;
        int n = convex->Vertices().Intersect( SEG( p0, v0 ), intersections );

        // if n == 1 intersected a segment
        // if n == 2 intersected the common point of 2 segments
        // n == 0 can not happen I think, but...
        if( n > 0 )
        {
            l.Append( p0 );

            // for a breakout distance relative to the distance between
            // center and polygon edge
            //l.Append( intersections[0].p + (v0 - p0).Resize( (intersections[0].p - p0).EuclideanNorm() * 0.4 ) );

            // for an absolute breakout distance, e.g. 0.1 mm
            //l.Append( intersections[0].p + (v0 - p0).Resize( 100000 ) );

            // for the breakout right on the polygon edge
            l.Append( intersections[0].p );

            breakouts.push_back( l );
        }
    }

    return breakouts;
}


OPTIMIZER::BREAKOUT_LIST OPTIMIZER::rectBreakouts( int aWidth, const SHAPE* aShape,
                                                   bool aPermitDiagonal ) const
{
    const SHAPE_RECT* rect = static_cast<const SHAPE_RECT*>(aShape);
    VECTOR2I s = rect->GetSize();
    VECTOR2I c = rect->GetPosition() + VECTOR2I( s.x / 2, s.y / 2 );

    BREAKOUT_LIST breakouts;
    breakouts.reserve( 12 );

    VECTOR2I d_offset;

    d_offset.x = ( s.x > s.y ) ? ( s.x - s.y ) / 2 : 0;
    d_offset.y = ( s.x < s.y ) ? ( s.y - s.x ) / 2 : 0;

    VECTOR2I d_vert  = VECTOR2I( 0, s.y / 2 + aWidth );
    VECTOR2I d_horiz = VECTOR2I( s.x / 2 + aWidth, 0 );

    breakouts.emplace_back( SHAPE_LINE_CHAIN( { c, c + d_horiz } ) );
    breakouts.emplace_back( SHAPE_LINE_CHAIN( { c, c - d_horiz } ) );
    breakouts.emplace_back( SHAPE_LINE_CHAIN( { c, c + d_vert } ) );
    breakouts.emplace_back( SHAPE_LINE_CHAIN( { c, c - d_vert } ) );

    if( aPermitDiagonal )
    {
        int l = aWidth + std::min( s.x, s.y ) / 2;

        if( s.x >= s.y )
        {
            breakouts.emplace_back(
                    SHAPE_LINE_CHAIN( { c, c + d_offset, c + d_offset + VECTOR2I( l, l ) } ) );
            breakouts.emplace_back(
                    SHAPE_LINE_CHAIN( { c, c + d_offset, c + d_offset - VECTOR2I( -l, l ) } ) );
            breakouts.emplace_back(
                    SHAPE_LINE_CHAIN( { c, c - d_offset, c - d_offset + VECTOR2I( -l, l ) } ) );
            breakouts.emplace_back(
                    SHAPE_LINE_CHAIN( { c, c - d_offset, c - d_offset - VECTOR2I( l, l ) } ) );
        }
        else
        {
            // fixme: this could be done more efficiently
            breakouts.emplace_back(
                    SHAPE_LINE_CHAIN( { c, c + d_offset, c + d_offset + VECTOR2I( l, l ) } ) );
            breakouts.emplace_back(
                    SHAPE_LINE_CHAIN( { c, c - d_offset, c - d_offset - VECTOR2I( -l, l ) } ) );
            breakouts.emplace_back(
                    SHAPE_LINE_CHAIN( { c, c + d_offset, c + d_offset + VECTOR2I( -l, l ) } ) );
            breakouts.emplace_back(
                    SHAPE_LINE_CHAIN( { c, c - d_offset, c - d_offset - VECTOR2I( l, l ) } ) );
        }
    }

    return breakouts;
}


OPTIMIZER::BREAKOUT_LIST OPTIMIZER::computeBreakouts( int aWidth, const ITEM* aItem,
                                                      bool aPermitDiagonal ) const
{
    switch( aItem->Kind() )
    {
    case ITEM::VIA_T:
    {
        const VIA* via = static_cast<const VIA*>( aItem );
        // TODO(JE) padstacks -- computeBreakouts needs to have a layer argument
        return circleBreakouts( aWidth, via->Shape( 0 ), aPermitDiagonal );
    }

    case ITEM::SOLID_T:
    {
        const SHAPE* shape = aItem->Shape( -1 );

        switch( shape->Type() )
        {
        case SH_RECT:
            return rectBreakouts( aWidth, shape, aPermitDiagonal );

        case SH_SEGMENT:
        {
            const SHAPE_SEGMENT* seg = static_cast<const SHAPE_SEGMENT*> (shape);
            const SHAPE_RECT rect = ApproximateSegmentAsRect ( *seg );
            return rectBreakouts( aWidth, &rect, aPermitDiagonal );
        }

        case SH_CIRCLE:
            return circleBreakouts( aWidth, shape, aPermitDiagonal );

        case SH_SIMPLE:
            return customBreakouts( aWidth, aItem, aPermitDiagonal );

        default:
            break;
        }

        break;
    }

    default:
        break;
    }

    return BREAKOUT_LIST();
}


ITEM* OPTIMIZER::findPadOrVia( int aLayer, NET_HANDLE aNet, const VECTOR2I& aP ) const
{
    const JOINT* jt = m_world->FindJoint( aP, aLayer, aNet );

    if( !jt )
        return nullptr;

    for( ITEM* item : jt->LinkList() )
    {
        if( item->OfKind( ITEM::VIA_T | ITEM::SOLID_T ) )
            return item;
    }

    return nullptr;
}


int OPTIMIZER::smartPadsSingle( LINE* aLine, ITEM* aPad, bool aEnd, int aEndVertex )
{
    DIRECTION_45 dir;

    const int ForbiddenAngles = DIRECTION_45::ANG_ACUTE | DIRECTION_45::ANG_RIGHT |
                                DIRECTION_45::ANG_HALF_FULL | DIRECTION_45::ANG_UNDEFINED;

    typedef std::tuple<int, long long int, SHAPE_LINE_CHAIN> RtVariant;
    std::vector<RtVariant> variants;

    SOLID* solid = dyn_cast<SOLID*>( aPad );

    // don't do optimized connections for offset pads
    if( solid && solid->Offset() != VECTOR2I( 0, 0 ) )
        return -1;

    // don't do optimization on vias, they are always round at the moment and the optimizer
    // will possibly mess up an intended via exit posture
    if( aPad->Kind() == ITEM::VIA_T )
        return -1;

    BREAKOUT_LIST    breakouts = computeBreakouts( aLine->Width(), aPad, true );
    SHAPE_LINE_CHAIN line = ( aEnd ? aLine->CLine().Reverse() : aLine->CLine() );
    int              p_end = std::min( aEndVertex, std::min( 3, line.PointCount() - 1 ) );

    // Start at 1 to find a potentially better breakout (0 is the pad connection)
    for( int p = 1; p <= p_end; p++ )
    {
        // If the line is contained inside the pad, don't optimize
        if( solid && solid->Shape( -1 ) && !solid->Shape( -1 )->Collide(
                SEG( line.CPoint( 0 ), line.CPoint( p ) ), aLine->Width() / 2 ) )
        {
            continue;
        }

        for( SHAPE_LINE_CHAIN & breakout : breakouts )
        {
            for( int diag = 0; diag < 2; diag++ )
            {
                SHAPE_LINE_CHAIN v;
                SHAPE_LINE_CHAIN connect = dir.BuildInitialTrace(
                        breakout.CLastPoint(), line.CPoint( p ), diag == 0 );

                DIRECTION_45 dir_bkout( breakout.CSegment( -1 ) );

                if( !connect.SegmentCount() )
                    continue;

                int ang1 = dir_bkout.Angle( DIRECTION_45( connect.CSegment( 0 ) ) );

                if( ang1 & ForbiddenAngles )
                    continue;

                if( breakout.Length() > line.Length() )
                    continue;

                v = breakout;
                v.Append( connect );

                for( int i = p + 1; i < line.PointCount(); i++ )
                    v.Append( line.CPoint( i ) );

                LINE tmp( *aLine, v );
                int cc = tmp.CountCorners( ForbiddenAngles );

                if( cc == 0 )
                {
                    RtVariant vp;
                    std::get<0>( vp ) = p;
                    std::get<1>( vp ) = breakout.Length();
                    std::get<2>( vp ) = aEnd ? v.Reverse() : v;
                    std::get<2>( vp ).Simplify2();
                    variants.push_back( std::move( vp ) );
                }
            }
        }
    }

    // We attempt to minimize the corner cost (minimizes the segments and types of corners)
    // but given two, equally valid costs, we want to pick the longer pad exit.  The logic
    // here is that if the pad is oblong, the track should not exit the shorter side and parallel
    // the pad but should follow the pad's preferential direction before exiting.
    // The baseline guess is to start with the existing line the user has drawn.
    int              min_cost   = COST_ESTIMATOR::CornerCost( *aLine );
    long long int    max_length = 0;
    bool             found      = false;
    int              p_best     = -1;
    SHAPE_LINE_CHAIN l_best;

    for( RtVariant& vp : variants )
    {
        LINE tmp( *aLine, std::get<2>( vp ) );
        int cost = COST_ESTIMATOR::CornerCost( std::get<2>( vp ) );
        long long int len = std::get<1>( vp );

        if( !checkColliding( &tmp ) )
        {
            if( cost < min_cost || ( cost == min_cost && len > max_length ) )
            {
                l_best = std::get<2>( vp );
                p_best = std::get<0>( vp );
                found  = true;

                if( cost <= min_cost )
                    max_length = std::max<int>( len, max_length );

                min_cost = std::min( cost, min_cost );
            }
        }
    }

    if( found )
    {
        aLine->SetShape( l_best );
        return p_best;
    }

    return -1;
}


bool OPTIMIZER::runSmartPads( LINE* aLine )
{
    SHAPE_LINE_CHAIN& line = aLine->Line();

    if( line.PointCount() < 3 )
        return false;

    VECTOR2I p_start = line.CPoint( 0 ), p_end = line.CLastPoint();

    ITEM* startPad = findPadOrVia( aLine->Layer(), aLine->Net(), p_start );
    ITEM* endPad = findPadOrVia( aLine->Layer(), aLine->Net(), p_end );

    int vtx = -1;

    if( startPad )
        vtx = smartPadsSingle( aLine, startPad, false, 3 );

    if( endPad )
        smartPadsSingle( aLine, endPad, true,
                         vtx < 0 ? line.PointCount() - 1 : line.PointCount() - 1 - vtx );

    aLine->Line().Simplify2();

    return true;
}


bool OPTIMIZER::Optimize( LINE* aLine, int aEffortLevel, NODE* aWorld, const VECTOR2I& aV )
{
    OPTIMIZER opt( aWorld );

    opt.SetEffortLevel( aEffortLevel );
    opt.SetCollisionMask( -1 );

    if( aEffortLevel & OPTIMIZER::PRESERVE_VERTEX )
        opt.SetPreserveVertex( aV );

    LINE tmp( *aLine );
    return opt.Optimize( &tmp, aLine );
}


bool OPTIMIZER::fanoutCleanup( LINE* aLine )
{
    if( aLine->PointCount() < 3 )
        return false;

    DIRECTION_45::CORNER_MODE cornerMode = ROUTER::GetInstance()->Settings().GetCornerMode();

    VECTOR2I p_start = aLine->CPoint( 0 ), p_end = aLine->CLastPoint();

    ITEM* startPad = findPadOrVia( aLine->Layer(), aLine->Net(), p_start );
    ITEM* endPad = findPadOrVia( aLine->Layer(), aLine->Net(), p_end );

    int thr = aLine->Width() * 10;
    int len = aLine->CLine().Length();

    if( !startPad )
        return false;

    bool startMatch = startPad->OfKind( ITEM::VIA_T | ITEM::SOLID_T );
    bool endMatch = false;

    if(endPad)
    {
        endMatch = endPad->OfKind( ITEM::VIA_T | ITEM::SOLID_T );
    }
    else
    {
        endMatch = aLine->EndsWithVia();
    }

    if( startMatch && endMatch && len < thr )
    {
        for( int i = 0; i < 2; i++ )
        {
            SHAPE_LINE_CHAIN l2 = DIRECTION_45().BuildInitialTrace( p_start, p_end, i, cornerMode );
            LINE repl;
            repl = LINE( *aLine, l2 );

            if( !m_world->CheckColliding( &repl ) )
            {
                aLine->SetShape( repl.CLine() );
                return true;
            }
        }
    }

    return false;
}


int findCoupledVertices( const VECTOR2I& aVertex, const SEG& aOrigSeg,
                         const SHAPE_LINE_CHAIN& aCoupled, DIFF_PAIR* aPair, int* aIndices )
{
    int count = 0;

    for ( int i = 0; i < aCoupled.SegmentCount(); i++ )
    {
        SEG s = aCoupled.CSegment( i );
        VECTOR2I projOverCoupled = s.LineProject ( aVertex );

        if( s.ApproxParallel( aOrigSeg ) )
        {
            int64_t dist =
                    int64_t{ ( ( projOverCoupled - aVertex ).EuclideanNorm() ) } - aPair->Width();

            if( aPair->GapConstraint().Matches( dist ) )
            {
               *aIndices++ = i;
               count++;
            }
        }
    }

    return count;
}


bool verifyDpBypass( NODE* aNode, DIFF_PAIR* aPair, bool aRefIsP, const SHAPE_LINE_CHAIN& aNewRef,
                     const SHAPE_LINE_CHAIN& aNewCoupled )
{
    LINE refLine ( aRefIsP ? aPair->PLine() : aPair->NLine(), aNewRef );
    LINE coupledLine ( aRefIsP ? aPair->NLine() : aPair->PLine(), aNewCoupled );

    if( refLine.Collide( &coupledLine, aNode, refLine.Layer() ) )
        return false;

    if( aNode->CheckColliding ( &refLine ) )
        return false;

    if( aNode->CheckColliding ( &coupledLine ) )
        return false;

    return true;
}


bool coupledBypass( NODE* aNode, DIFF_PAIR* aPair, bool aRefIsP, const SHAPE_LINE_CHAIN& aRef,
                    const SHAPE_LINE_CHAIN& aRefBypass, const SHAPE_LINE_CHAIN& aCoupled,
                    SHAPE_LINE_CHAIN& aNewCoupled )
{
    int              vStartIdx[1024]; // fixme: possible overflow
    int              nStarts = findCoupledVertices( aRefBypass.CPoint( 0 ),
                                                    aRefBypass.CSegment( 0 ),
                                                    aCoupled, aPair, vStartIdx );
    DIRECTION_45     dir( aRefBypass.CSegment( 0 ) );

    int64_t          bestLength = -1;
    bool             found = false;
    SHAPE_LINE_CHAIN bestBypass;
    int              si, ei;

    for( int i=0; i< nStarts; i++ )
    {
        for( int j = 1; j < aCoupled.PointCount() - 1; j++ )
        {
            int delta = std::abs ( vStartIdx[i] - j );

            if( delta > 1 )
            {
                const VECTOR2I& vs = aCoupled.CPoint( vStartIdx[i] );
                SHAPE_LINE_CHAIN bypass = dir.BuildInitialTrace( vs, aCoupled.CPoint(j),
                                                                 dir.IsDiagonal() );

                int64_t coupledLength = aPair->CoupledLength( aRef, bypass );

                SHAPE_LINE_CHAIN newCoupled = aCoupled;

                si = vStartIdx[i];
                ei = j;

                if(si < ei)
                    newCoupled.Replace( si, ei, bypass );
                else
                    newCoupled.Replace( ei, si, bypass.Reverse() );

                if( coupledLength > bestLength && verifyDpBypass( aNode, aPair, aRefIsP, aRef,
                                                                  newCoupled) )
                {
                    bestBypass = std::move( newCoupled );
                    bestLength = coupledLength;
                    found = true;
                }
            }
        }
    }

    if( found )
        aNewCoupled = std::move( bestBypass );

    return found;
}


bool checkDpColliding( NODE* aNode, DIFF_PAIR* aPair, bool aIsP, const SHAPE_LINE_CHAIN& aPath )
{
    LINE tmp ( aIsP ? aPair->PLine() : aPair->NLine(), aPath );

    return static_cast<bool>( aNode->CheckColliding( &tmp ) );
}


bool OPTIMIZER::mergeDpStep( DIFF_PAIR* aPair, bool aTryP, int step )
{
    int n = 1;

    SHAPE_LINE_CHAIN currentPath = aTryP ? aPair->CP() : aPair->CN();
    SHAPE_LINE_CHAIN coupledPath = aTryP ? aPair->CN() : aPair->CP();

    int n_segs = currentPath.SegmentCount() - 1;

    int64_t clenPre = aPair->CoupledLength( currentPath, coupledPath );
    int64_t budget = clenPre / 10; // fixme: come up with something more intelligent here...

    while( n < n_segs - step )
    {
        const SEG s1    = currentPath.CSegment( n );
        const SEG s2    = currentPath.CSegment( n + step );

        DIRECTION_45 dir1( s1 );
        DIRECTION_45 dir2( s2 );

        if( dir1.IsObtuse( dir2 ) )
        {
            SHAPE_LINE_CHAIN bypass = DIRECTION_45().BuildInitialTrace( s1.A, s2.B,
                                                                        dir1.IsDiagonal() );
            SHAPE_LINE_CHAIN newRef;
            SHAPE_LINE_CHAIN newCoup;
            int64_t deltaCoupled = -1, deltaUni = -1;

            newRef = currentPath;
            newRef.Replace( s1.Index(), s2.Index(), bypass );

            deltaUni = aPair->CoupledLength ( newRef, coupledPath ) - clenPre + budget;

            if( coupledBypass( m_world, aPair, aTryP, newRef, bypass, coupledPath, newCoup ) )
            {
                deltaCoupled = aPair->CoupledLength( newRef, newCoup ) - clenPre + budget;

                if( deltaCoupled >= 0 )
                {
                    newRef.Simplify2();
                    newCoup.Simplify2();

                    aPair->SetShape( newRef, newCoup, !aTryP );
                    return true;
                }
            }
            else if( deltaUni >= 0 && verifyDpBypass( m_world, aPair, aTryP, newRef, coupledPath ) )
            {
                newRef.Simplify2();
                coupledPath.Simplify2();

                aPair->SetShape( newRef, coupledPath, !aTryP );
                return true;
            }
        }

        n++;
    }

    return false;
}


bool OPTIMIZER::mergeDpSegments( DIFF_PAIR* aPair )
{
    int step_p = aPair->CP().SegmentCount() - 2;
    int step_n = aPair->CN().SegmentCount() - 2;

    while( 1 )
    {
        int n_segs_p = aPair->CP().SegmentCount();
        int n_segs_n = aPair->CN().SegmentCount();

        int max_step_p = n_segs_p - 2;
        int max_step_n = n_segs_n - 2;

        if( step_p > max_step_p )
            step_p = max_step_p;

        if( step_n > max_step_n )
            step_n = max_step_n;

        if( step_p < 1 && step_n < 1 )
            break;

        bool found_anything_p = false;
        bool found_anything_n = false;

        if( step_p > 1 )
            found_anything_p = mergeDpStep( aPair, true, step_p );

        if( step_n > 1 )
            found_anything_n = mergeDpStep( aPair, false, step_n );

        if( !found_anything_n && !found_anything_p )
        {
            step_n--;
            step_p--;
        }
    }
    return true;
}


bool OPTIMIZER::Optimize( DIFF_PAIR* aPair )
{
    return mergeDpSegments( aPair );
}


static int64_t shovedArea( const SHAPE_LINE_CHAIN& aOld, const SHAPE_LINE_CHAIN& aNew )
{
    int64_t area = 0;
    const int oc = aOld.PointCount();
    const int nc = aNew.PointCount();
    const int total = oc + nc;

    for(int i = 0; i < total; i++)
    {
        int i_next = (i + 1 == total ? 0 : i + 1);

        const VECTOR2I &v0 = i < oc ? aOld.CPoint(i)
                                    : aNew.CPoint( nc - 1 - (i - oc) );
        const VECTOR2I &v1 = i_next < oc ? aOld.CPoint ( i_next )
                                         : aNew.CPoint( nc - 1 - (i_next - oc) );
        area += -(int64_t) v0.y * v1.x + (int64_t) v0.x * v1.y;
    }

    return std::abs( area / 2 );
}


bool tightenSegment( bool dir, NODE *aNode, const LINE& cur, const SHAPE_LINE_CHAIN& in,
                     SHAPE_LINE_CHAIN& out )
{
    SEG a = in.CSegment(0);
    SEG center = in.CSegment(1);
    SEG b = in.CSegment(2);

    DIRECTION_45 dirA ( a );
    DIRECTION_45 dirCenter ( center );
    DIRECTION_45 dirB ( b );

    if (!dirA.IsObtuse( dirCenter) || !dirCenter.IsObtuse(dirB))
        return false;

    SEG guide;
    int initial;

    //auto dbg = ROUTER::GetInstance()->GetInterface()->GetDebugDecorator();
    if ( dirA.Angle ( dirB ) != DIRECTION_45::ANG_RIGHT )
        return false;

    {
        /*
        auto rC = *a.IntersectLines( b );
        dbg->AddSegment ( SEG( center.A, rC ), 1 );
        dbg->AddSegment ( SEG( center.B, rC ), 2 );
        auto perp = dirCenter.Left().Left();

        SEG sperp ( center.A, center.A + perp.ToVector() );

        auto vpc = sperp.LineProject( rC );
        auto vpa = sperp.LineProject( a.A );
        auto vpb = sperp.LineProject( b.B );

        auto da = (vpc - vpa).EuclideanNorm();
        auto db = (vpc - vpb).EuclideanNorm();

        auto vp = (da < db) ? vpa : vpb;
        dbg->AddSegment ( SEG( vpc, vp ), 5 );


        guide = SEG ( vpc, vp );
         */
    }

    int da = a.Length();
    int db = b.Length();

    if( da < db )
        guide = a;
    else
        guide = b;

    initial = guide.Length();

    int step = initial;
    int current = step;
    SHAPE_LINE_CHAIN snew;

    while( step > 1 )
    {
        LINE l( cur );

        snew.Clear();
        snew.Append( a.A );
        snew.Append( a.B + ( a.A - a.B ).Resize( current ) );
        snew.Append( b.A + ( b.B - b.A ).Resize( current ) );
        snew.Append( b.B );

        step /= 2;

        l.SetShape(snew);

        if( aNode->CheckColliding(&l) )
            current -= step;
        else if ( current + step >= initial )
            current = initial;
        else
            current += step;

        //dbg->AddSegment ( SEG( center.A ,  a.LineProject( center.A + gr ) ), 3 );
        //dbg->AddSegment ( SEG( center.A ,  center.A + guideA  ), 3 );
        //dbg->AddSegment ( SEG( center.B , center.B + guideB ), 4 );

        if ( current == initial )
            break;
    }

    //dbg->AddLine ( snew, 3, 100000 );

    out = std::move( snew );
    return true;
}


void Tighten( NODE *aNode, const SHAPE_LINE_CHAIN& aOldLine, const LINE& aNewLine,
              LINE& aOptimized )
{
    LINE tmp;

    if( aNewLine.SegmentCount() < 3 )
        return;

    SHAPE_LINE_CHAIN current ( aNewLine.CLine() );

    for( int step = 0; step < 3; step++ )
    {
        current.Simplify2();

        for( int i = 0; i <= current.SegmentCount() - 3; i++ )
        {
            SHAPE_LINE_CHAIN l_in, l_out;

            l_in = current.Slice( i, i + 3 );

            for( int dir = 0; dir <= 1; dir++ )
            {
                if( tightenSegment( dir ? true : false, aNode, aNewLine, l_in, l_out ) )
                {
                    SHAPE_LINE_CHAIN opt = current;
                    opt.Replace( i, i + 3, l_out );
                    long long int optArea = std::abs( shovedArea( aOldLine, opt ) );
                    long long int prevArea = std::abs( shovedArea( aOldLine, current ) );

                    if( optArea < prevArea )
                        current = std::move( opt );

                    break;
                }
            }
        }
    }

    aOptimized = LINE( aNewLine, current );

    //auto dbg = ROUTER::GetInstance()->GetInterface()->GetDebugDecorator();
    //dbg->AddLine ( current, 4, 100000 );
}



// ---------------------------------------------------------------------------------------------
// KICLOUD: R3.2 glossing (docs/patches.md)
//
// gloss() cleans one line the way Altium's glossing does, without changing where it connects.
// First zero-length pieces and collinear splits are merged, but only where a vertex lies between
// its two neighbours on one straight piece (glossMergeExact: the copper stays the same; a spike
// that turns back is not touched here). Then, one change at a time and only when the change makes
// the line strictly cleaner (GLOSS_SCORE below), a loop of the line over itself is cut out at the
// crossing, and a jog shorter than the track width or a corner sharper than 90 degrees is replaced
// by a straight or a 45-degree (90-degree in the 90-degree corner modes) piece between two of the
// line's vertices. Every replacement keeps the line's two ends; is checked against the copper of
// other nets with the current clearance rules (strictly: without the router's clearance epsilon);
// keeps touching every same-net item the original line touched (a pad or via it runs over, a
// track that ends on it), so connectivity cannot change; satisfies the optimizer's other
// constraints (vertex to keep, area, topology); and leaves alone the first or last segment where
// the line ends on a pad or via, so the pad entry SMART_PADS made stays centred. Lines with arcs
// and closed lines (a ring from a via back to the same via, or with both ends inside the same pad
// or via) are not glossed, and no loop cut takes the whole line.
//
// Cost: each candidate is scored from the few segments it changes (glossRegionScore) against the
// line's running total, so a candidate costs O(window x segments) for the crossing test and
// O(window) for the rest; a budget bounds the candidates one optimizer evaluates (SetGlossBudget),
// at most GLOSS_MAX_CHECKS candidates per step get the world collision check, and a route head is
// glossed only in its last segments (SetGlossTail).
// ---------------------------------------------------------------------------------------------

/// How clean a line is. Compared field by field in this order; smaller is cleaner. gloss() only
/// accepts replacements that make it strictly smaller, so it always ends. (Declared in the header
/// so glossStep() can take it.)
struct GLOSS_SCORE
{
    int crossings = 0;    ///< pairs of segments that cross or touch (the line loops over itself)
    int acute = 0;        ///< corners whose inside angle is under 90 degrees
    int shortSegs = 0;    ///< segments shorter than the track width (tiny jogs)
    int segs = 0;         ///< segments
    int cornerCost = 0;   ///< COST_ESTIMATOR::CornerCost (sharper corners cost more)

    bool operator<( const GLOSS_SCORE& aOther ) const
    {
        return std::tie( crossings, acute, shortSegs, segs, cornerCost )
               < std::tie( aOther.crossings, aOther.acute, aOther.shortSegs, aOther.segs,
                           aOther.cornerCost );
    }
};


namespace
{

/// The widest piece of a line gloss() replaces at once, in segments: a jog is the short segment
/// with its two neighbours (3); one more lets a jog right next to a corner go as well (4).
constexpr int GLOSS_MAX_WINDOW = 4;

/// How many of one step's candidates, best first, get the (expensive) world collision check
/// before the step gives up.
constexpr int GLOSS_MAX_CHECKS = 8;




/// Whether the corner at vertex aV of aPath (not its first or last point) is sharper than 90
/// degrees: the directions to its two neighbours point into the same half-plane. Exact (integer
/// arithmetic); a right angle or a straight continuation is not acute.
bool glossIsAcute( const SHAPE_LINE_CHAIN& aPath, int aV )
{
    const VECTOR2I a = aPath.CPoint( aV - 1 ) - aPath.CPoint( aV );
    const VECTOR2I b = aPath.CPoint( aV + 1 ) - aPath.CPoint( aV );

    return a.Dot( b ) > 0;
}


/// The part of aPath's GLOSS_SCORE that depends on segments aLo..aHi (clamped to the line):
/// those segments shorter than aWidth; the crossing pairs with at least one segment in the range;
/// the acute corners at vertices aLo..aHi+1; the corner cost of the corners at those vertices.
/// segs is left 0. Two paths that differ only in such a range (and are equal before and after
/// it) differ in score exactly by the difference of their region scores, which is how gloss()
/// scores a candidate without rescanning the whole line.
GLOSS_SCORE glossRegionScore( const SHAPE_LINE_CHAIN& aPath, int aWidth, int aLo, int aHi )
{
    GLOSS_SCORE       score;
    const int         nSegs = aPath.SegmentCount();
    const SEG::ecoord minLength2 = SEG::Square( aWidth );

    aLo = std::max( aLo, 0 );
    aHi = std::min( aHi, nSegs - 1 );

    for( int i = aLo; i <= aHi; i++ )
    {
        const SEG s = aPath.CSegment( i );

        if( s.SquaredLength() < minLength2 )
            score.shortSegs++;

        // pairs (j, i) with j < i - 1 and j outside the range (inside it they are counted from j)
        for( int j = 0; j < std::min( i - 1, aLo ); j++ )
        {
            if( s.Intersects( aPath.CSegment( j ) ) )
                score.crossings++;
        }

        // pairs (i, j) with j > i + 1
        for( int j = i + 2; j < nSegs; j++ )
        {
            if( s.Intersects( aPath.CSegment( j ) ) )
                score.crossings++;
        }
    }

    for( int v = std::max( aLo, 1 ); v <= std::min( aHi + 1, aPath.PointCount() - 2 ); v++ )
    {
        if( glossIsAcute( aPath, v ) )
            score.acute++;

        score.cornerCost += COST_ESTIMATOR::CornerCost( aPath.CSegment( v - 1 ),
                                                        aPath.CSegment( v ) );
    }

    return score;
}


/// The whole GLOSS_SCORE of aPath (O(segments^2) for the crossings; once per line).
GLOSS_SCORE glossScore( const SHAPE_LINE_CHAIN& aPath, int aWidth )
{
    GLOSS_SCORE score = glossRegionScore( aPath, aWidth, 0, aPath.SegmentCount() - 1 );

    score.segs = aPath.SegmentCount();
    return score;
}


/// Removes duplicate points and every vertex that lies on the straight piece between its two
/// neighbours (it turns by nothing), so the copper is exactly the same in fewer pieces. Unlike
/// SHAPE_LINE_CHAIN::Simplify2() it never drops a vertex where the line turns back on itself (a
/// spike A-B-C with B beyond C), which would remove copper unchecked.
void glossMergeExact( SHAPE_LINE_CHAIN& aPath )
{
    std::vector<VECTOR2I> pts;

    pts.reserve( aPath.PointCount() );

    for( const VECTOR2I& p : aPath.CPoints() )
    {
        if( !pts.empty() && pts.back() == p )
            continue;

        while( pts.size() >= 2 )
        {
            const VECTOR2I& a = pts[pts.size() - 2];
            const VECTOR2I& b = pts.back();
            const VECTOR2I  ab = b - a;
            const VECTOR2I  bc = p - b;

            // b lies between a and p on one straight piece: collinear and pointing the same way
            if( ab.Cross( bc ) == 0 && ab.Dot( bc ) > 0 )
                pts.pop_back();
            else
                break;
        }

        pts.push_back( p );
    }

    aPath = SHAPE_LINE_CHAIN( pts );
}


/// Whether aSeg runs in a direction the corner mode draws: horizontal or vertical, and in the
/// 45-degree modes also at exactly 45 degrees.
bool glossDirectionAllowed( const SEG& aSeg, bool a90 )
{
    const VECTOR2I d = aSeg.B - aSeg.A;

    return d.x == 0 || d.y == 0 || ( !a90 && std::abs( d.x ) == std::abs( d.y ) );
}

} // namespace


// gloss(): see the block comment above. Caller: Optimize() when the effort includes GLOSS, with
// aLine the copy being optimized (aResult there) and aOrigin the line as given (still linked to
// its segments when it came from a node, which is how the same-net items it touches are found).
// Result: whether the line changed. State: aLine's shape, the budget (m_glossBudget).
bool OPTIMIZER::gloss( LINE* aLine, const LINE* aOrigin )
{
    SHAPE_LINE_CHAIN& line = aLine->Line();

    // A closed line (a ring from a via or pad back to itself) has no loop to cut: its "loop" is
    // the ring. KiCad's own loop removal (LINE_PLACER::removeLoops) leaves such lines alone too.
    if( line.ArcCount() || line.PointCount() < 2 || line.CPoint( 0 ) == line.CLastPoint() )
        return false;

    const std::vector<VECTOR2I> before = line.CPoints();

    glossMergeExact( line );

    const std::vector<ITEM*> contacts = glossContacts( aOrigin );
    ITEM*      startItem = findPadOrVia( aLine->Layer(), aLine->Net(), line.CPoint( 0 ) );
    ITEM*      endItem = findPadOrVia( aLine->Layer(), aLine->Net(), line.CLastPoint() );
    const bool startAnchored = startItem != nullptr;
    const bool endAnchored = endItem != nullptr;

    // A ring whose two ends lie in the same pad or via (on its anchor or anywhere inside it) is
    // closed as far as the copper goes; it is left alone like a ring closing at one point.
    if( startItem && startItem == endItem )
    {
        line = SHAPE_LINE_CHAIN( before );
        return false;
    }

    for( ITEM* contact : contacts )
    {
        if( !contact->OfKind( ITEM::SOLID_T | ITEM::VIA_T ) )
            continue;

        const SHAPE* shape = contact->Shape( aLine->Layer() );

        if( shape && shape->Collide( line.CPoint( 0 ) ) && shape->Collide( line.CLastPoint() ) )
        {
            line = SHAPE_LINE_CHAIN( before );
            return false;
        }
    }

    GLOSS_SCORE score = glossScore( line, aLine->Width() );

    // Every accepted step makes the score strictly smaller; the step limit and the candidate
    // budget only bound the work on a pathological line.
    for( int steps = 2 * line.SegmentCount() + 8; steps > 0 && m_glossBudget > 0; steps-- )
    {
        if( !glossStep( aLine, contacts, startAnchored, endAnchored, score ) )
            break;
    }

    return line.CPoints() != before;
}


// glossStep(): one improvement of aLine, if there is one. Caller: gloss(). Finds the candidate
// replacements (loops first, then windows of 2 to GLOSS_MAX_WINDOW segments around each sharp
// corner and each tiny jog), keeps those that score better than the line (aScore), and
// applies the best-scoring one that passes the constraints, the clearance check and the contact
// check (at most GLOSS_MAX_CHECKS of them are checked). With SetGlossTail() only the line's last
// segments are touched. Inputs: aContacts (glossContacts() of the original line);
// aStartAnchored / aEndAnchored (the line ends on a pad or via there, so its first / last segment
// is kept, by loop cuts too). Result: whether aLine changed. State: aLine, aScore (the line's
// score, kept up to date), m_glossBudget (one per candidate scored; m_glossBudgetHit when it runs out).
bool OPTIMIZER::glossStep( LINE* aLine, const std::vector<ITEM*>& aContacts, bool aStartAnchored,
                           bool aEndAnchored, GLOSS_SCORE& aScore )
{
    SHAPE_LINE_CHAIN&      line = aLine->Line();
    const SHAPE_LINE_CHAIN current( line );
    const int              nPts = current.PointCount();
    const int              nSegs = current.SegmentCount();

    if( nSegs < 2 )
        return false;

    const int width = aLine->Width();

    const DIRECTION_45::CORNER_MODE cornerMode = ROUTER::GetInstance()->Settings().GetCornerMode();
    const bool is90 = cornerMode == DIRECTION_45::MITERED_90
                      || cornerMode == DIRECTION_45::ROUNDED_90;
    const DIRECTION_45::CORNER_MODE pieceMode = is90 ? DIRECTION_45::MITERED_90
                                                     : DIRECTION_45::MITERED_45;

    // The vertices a replacement may span: not the first / last segment of an anchored end, and
    // with a tail limit only the line's last m_glossTailSegments segments.
    const int firstFrom = std::max( aStartAnchored ? 1 : 0,
                                    m_glossTailSegments > 0 ? nSegs - m_glossTailSegments : 0 );
    const int lastTo = aEndAnchored ? nPts - 2 : nPts - 1;

    struct GLOSS_CANDIDATE
    {
        GLOSS_SCORE      score;
        SHAPE_LINE_CHAIN path;    ///< the whole line after the replacement (merged)
        SHAPE_LINE_CHAIN piece;   ///< what replaces vertices from..to
        int              from;
        int              to;
    };

    std::vector<GLOSS_CANDIDATE> candidates;

    // touchesNewCopper(): whether vertices aFrom..aTo span a segment that is not already in the
    // reference node (always true without one).
    auto touchesNewCopper =
            [&]( int aFrom, int aTo )
            {
                for( int i = aFrom; i < aTo; i++ )
                {
                    if( glossSegmentIsNew( current.CSegment( i ), aLine ) )
                        return true;
                }

                return false;
            };

    // addCandidate(): the line with vertices aFrom..aTo replaced by aPiece, if that is cleaner.
    // Its score is the line's total minus what the changed segments (aFrom-1 .. aTo) contributed
    // plus what their replacement contributes; segments before and after are the same in both.
    auto addCandidate =
            [&]( int aFrom, int aTo, const SHAPE_LINE_CHAIN& aPiece )
            {
                if( m_glossBudget <= 0 )
                {
                    m_glossBudgetHit = true;
                    return;
                }

                m_glossBudget--;

                SHAPE_LINE_CHAIN path( current );

                path.Replace( aFrom, aTo, aPiece );
                glossMergeExact( path );

                const int   lo = aFrom - 1;
                const int   hiCur = std::min( aTo, nSegs - 1 );
                const int   hiNew = path.SegmentCount() - nSegs + hiCur;
                GLOSS_SCORE old = glossRegionScore( current, width, lo, hiCur );
                GLOSS_SCORE neu = glossRegionScore( path, width, lo, hiNew );
                GLOSS_SCORE score = aScore;

                score.crossings += neu.crossings - old.crossings;
                score.acute += neu.acute - old.acute;
                score.shortSegs += neu.shortSegs - old.shortSegs;
                score.cornerCost += neu.cornerCost - old.cornerCost;
                score.segs = path.SegmentCount();

                if( score < aScore )
                    candidates.push_back( { score, path, aPiece, aFrom, aTo } );
            };

    // applyBest(): applies the best candidate that passes every check; empties the list.
    auto applyBest =
            [&]() -> bool
            {
                std::stable_sort( candidates.begin(), candidates.end(),
                                  []( const GLOSS_CANDIDATE& aA, const GLOSS_CANDIDATE& aB )
                                  {
                                      return aA.score < aB.score;
                                  } );

                int checks = 0;

                for( const GLOSS_CANDIDATE& c : candidates )
                {
                    if( !checkConstraints( c.from, c.to, aLine, current, c.piece ) )
                        continue;

                    if( ++checks > GLOSS_MAX_CHECKS )
                        break;

                    if( glossCollides( aLine, c.piece ) )
                        continue;

                    if( !glossKeepsContacts( aLine, c.path, aContacts ) )
                        continue;

                    line = c.path;
                    aScore = c.score;
                    candidates.clear();
                    return true;
                }

                candidates.clear();
                return false;
            };

    // 1. Loops: where the line crosses itself, cut the loop out at the crossing (this only removes
    //    copper; the piece lies on the two crossing segments). The anchored ends are kept.
    for( int i = firstFrom; i < nSegs; i++ )
    {
        for( int j = i + 2; j < nSegs && j + 1 <= lastTo; j++ )
        {
            // never the whole line: a cut from its first to its last segment would leave a V of
            // its two ends (a ring that crosses itself inside the pad it starts and ends in)
            if( i == 0 && j == nSegs - 1 )
                continue;

            OPT_VECTOR2I ip = current.CSegment( i ).Intersect( current.CSegment( j ) );

            if( !ip || !touchesNewCopper( i, j + 1 ) )
                continue;

            SHAPE_LINE_CHAIN piece;

            piece.Append( current.CPoint( i ) );
            piece.Append( *ip );
            piece.Append( current.CPoint( j + 1 ) );
            addCandidate( i, j + 1, piece );
            break;
        }
    }

    if( !candidates.empty() && applyBest() )
        return true;

    // 2. Sharp corners and tiny jogs. Each defect is a range of segments a replacement must cover:
    //    the two segments meeting at a corner under 90 degrees, or one segment shorter than the
    //    track width. Windows that hold it are replaced by a straight piece (only in an allowed
    //    direction when the window's own segments all were) or either two-segment piece.
    const SEG::ecoord minLength2 = SEG::Square( width );

    std::vector<std::pair<int, int>> defects;

    for( int v = std::max( 1, firstFrom ); v + 1 < nPts; v++ )
    {
        if( glossIsAcute( current, v ) )
            defects.emplace_back( v - 1, v );
    }

    for( int k = firstFrom; k < nSegs; k++ )
    {
        if( current.CSegment( k ).SquaredLength() < minLength2 )
            defects.emplace_back( k, k );
    }

    for( const auto& [first, last] : defects )
    {
        for( int size = 2; size <= GLOSS_MAX_WINDOW; size++ )
        {
            for( int from = last - size + 1; from <= first; from++ )
            {
                const int to = from + size;   // the window is segments from..to-1

                if( from < firstFrom || to > lastTo || from < 0 || to > nSegs )
                    continue;

                if( !touchesNewCopper( from, to ) )
                    continue;

                bool regular = true;

                for( int i = from; i < to; i++ )
                    regular = regular && glossDirectionAllowed( current.CSegment( i ), is90 );

                const VECTOR2I a = current.CPoint( from );
                const VECTOR2I b = current.CPoint( to );

                if( !regular || glossDirectionAllowed( SEG( a, b ), is90 ) )
                    addCandidate( from, to, SHAPE_LINE_CHAIN( { a, b } ) );

                for( int diagonalFirst = 0; diagonalFirst < 2; diagonalFirst++ )
                {
                    addCandidate( from, to,
                                  DIRECTION_45().BuildInitialTrace( a, b, diagonalFirst != 0,
                                                                    pieceMode ) );
                }
            }
        }
    }

    return !candidates.empty() && applyBest();
}


// glossCollides(): whether aPiece, drawn as part of aLine (its width, net and layer), comes too
// close to an item of another net under the current clearance rules. Strict: without the
// clearance epsilon the router allows itself while walking, since glossing is a clean-up and must
// never add a DRC violation. Caller: glossStep(). Changes nothing.
bool OPTIMIZER::glossCollides( const LINE* aLine, const SHAPE_LINE_CHAIN& aPiece ) const
{
    LINE                     piece( *aLine, aPiece );
    COLLISION_SEARCH_OPTIONS opts;

    opts.m_kindMask = m_collisionKindMask;
    opts.m_limitCount = 1;
    opts.m_useClearanceEpsilon = false;

    return m_world->CheckColliding( &piece, opts ).has_value();
}


// glossContacts(): the same-net items the copper of aOrigin touches, other than its own segments
// (pads and vias at its ends or under it, tracks that end on it). Empty when aOrigin is not
// linked to segments in the world (a route head being drawn: it touches only what it ends on,
// and its ends never move). Caller: gloss(), once per line. Changes nothing.
std::vector<ITEM*> OPTIMIZER::glossContacts( const LINE* aOrigin ) const
{
    std::vector<ITEM*> contacts;

    if( !aOrigin || !aOrigin->IsLinked() )
        return contacts;

    COLLISION_SEARCH_OPTIONS opts;

    opts.m_differentNetsOnly = false;
    opts.m_overrideClearance = 0;
    opts.m_useClearanceEpsilon = false;

    std::set<ITEM*>         seen;
    const SHAPE_LINE_CHAIN& chain = aOrigin->CLine();

    for( int i = 0; i < chain.SegmentCount(); i++ )
    {
        const SEGMENT   probe( *aOrigin, chain.CSegment( i ) );
        NODE::OBSTACLES obstacles;

        m_world->QueryColliding( &probe, obstacles, opts );

        for( const OBSTACLE& obstacle : obstacles )
        {
            ITEM* item = obstacle.m_item;

            if( item->Net() != aOrigin->Net() || item->OfKind( ITEM::HOLE_T ) )
                continue;

            if( item->OfKind( ITEM::SEGMENT_T | ITEM::ARC_T | ITEM::VIA_T )
                    && aOrigin->ContainsLink( static_cast<LINKED_ITEM*>( item ) ) )
            {
                continue;
            }

            if( seen.insert( item ).second )
                contacts.push_back( item );
        }
    }

    return contacts;
}


// glossKeepsContacts(): whether aPath, drawn as aLine, still touches every item of aContacts.
// Caller: glossStep(), for each candidate. Changes nothing.
bool OPTIMIZER::glossKeepsContacts( const LINE* aLine, const SHAPE_LINE_CHAIN& aPath,
                                    const std::vector<ITEM*>& aContacts ) const
{
    if( aContacts.empty() )
        return true;

    const LINE               candidate( *aLine, aPath );
    COLLISION_SEARCH_OPTIONS opts;

    opts.m_differentNetsOnly = false;
    opts.m_overrideClearance = 0;
    opts.m_useClearanceEpsilon = false;

    for( ITEM* contact : aContacts )
    {
        std::set<OBSTACLE>       found;
        COLLISION_SEARCH_CONTEXT ctx( found, opts );

        if( !contact->Collide( &candidate, m_world, candidate.Layer(), &ctx ) )
            return false;
    }

    return true;
}


// glossSegmentIsNew(): whether aSeg (a piece of aLine) is missing from the reference node, i.e.
// the route, drag or shove being committed made it. A segment counts as old when the reference has
// it, or when at each of its ends the reference has a segment of the same net and layer running
// along it (the merge of an old collinear split is still old copper). With no reference every
// segment counts as new. Caller: glossStep(). Changes nothing.
bool OPTIMIZER::glossSegmentIsNew( const SEG& aSeg, const LINE* aLine ) const
{
    if( !m_glossReference )
        return true;

    // alongAt(): whether the reference has, at aEnd, a segment that is aSeg itself or lies on it.
    auto alongAt =
            [&]( const VECTOR2I& aEnd ) -> bool
            {
                const JOINT* joint = m_glossReference->FindJoint( aEnd, aLine->Layer(),
                                                                  aLine->Net() );

                if( !joint )
                    return false;

                for( ITEM* item : joint->LinkList() )
                {
                    if( !item->OfKind( ITEM::SEGMENT_T ) )
                        continue;

                    const SEG&     s = static_cast<const SEGMENT*>( item )->Seg();
                    const VECTOR2I other = ( s.A == aEnd ) ? s.B : s.A;

                    if( ( s.A == aEnd || s.B == aEnd ) && aSeg.Collinear( s )
                            && aSeg.Contains( other ) )
                    {
                        return true;
                    }
                }

                return false;
            };

    return !( alongAt( aSeg.A ) && alongAt( aSeg.B ) );
}


// AssembleUniformLine(): see the header. NODE::AssembleLine() with aAllowSegmentSizeMismatch =
// false does not stop at a width change: it walks on and only leaves the other width's points
// out, which joins the two ends of that stretch with a straight piece that does not exist. Here
// the whole line is assembled as it is (widths mixed), checked to have exactly one segment link
// per segment of its shape, and then cut down to the seed's width on both sides.
bool OPTIMIZER::AssembleUniformLine( NODE* aNode, LINKED_ITEM* aSeed, bool aStopAtLockedJoints,
                                     LINE& aLine )
{
    if( !aSeed || !aSeed->OfKind( ITEM::SEGMENT_T ) )
        return false;

    LINE full = aNode->AssembleLine( aSeed, nullptr, aStopAtLockedJoints, false, true );

    if( full.ArcCount() )
        return false;

    const SHAPE_LINE_CHAIN&           shape = full.CLine();
    const std::vector<LINKED_ITEM*>&  links = full.Links();
    int                               seedIndex = -1;

    if( (int) links.size() != shape.SegmentCount() )
        return false;

    for( int k = 0; k < shape.SegmentCount(); k++ )
    {
        if( !links[k]->OfKind( ITEM::SEGMENT_T ) )
            return false;

        const SEG& s = static_cast<const SEGMENT*>( links[k] )->Seg();
        const SEG& t = shape.CSegment( k );

        if( !( ( s.A == t.A && s.B == t.B ) || ( s.A == t.B && s.B == t.A ) ) )
            return false;

        if( links[k] == aSeed )
            seedIndex = k;
    }

    if( seedIndex < 0 )
        return false;

    const int width = aSeed->Width();
    int       lo = seedIndex;
    int       hi = seedIndex;

    while( lo > 0 && links[lo - 1]->Width() == width )
        lo--;

    while( hi + 1 < (int) links.size() && links[hi + 1]->Width() == width )
        hi++;

    aLine = LINE( full, shape.Slice( lo, hi + 1 ) );
    aLine.SetWidth( width );
    aLine.ClearLinks();

    for( int k = lo; k <= hi; k++ )
        aLine.Link( links[k] );

    return true;
}


// GlossLines(): see the header. Each line is assembled once (stopping at locked joints and at
// width changes), glossed with a fresh optimizer on aNode and, when it changed, written back:
// the segments of the old line the new shape no longer has are removed and the new shape is added,
// which links the segments it shares with the old line instead of recreating them (NODE::Add finds
// them as redundant), so untouched segments keep their board item. Skipped: lines with arcs,
// locked lines (this includes the tracks of a length-tuning pattern, which the router syncs as
// locked) and lines of a differential pair's nets.
int OPTIMIZER::GlossLines( NODE* aNode, const std::vector<LINKED_ITEM*>& aSeeds,
                           const NODE* aReference, int* aLinesChanged,
                           const std::optional<VECTOR2I>& aPreserve, int aBudget, bool* aPartial )
{
    std::set<const LINKED_ITEM*> visited;
    int                          removed = 0;
    int                          changed = 0;
    int                          budget = aBudget > 0 ? aBudget : GlossDefaultBudget;
    RULE_RESOLVER*               resolver = aNode->GetRuleResolver();

    if( aPartial )
        *aPartial = false;

    for( LINKED_ITEM* seed : aSeeds )
    {
        if( !seed || !seed->OfKind( ITEM::SEGMENT_T ) || visited.count( seed ) )
            continue;

        if( budget <= 0 )
        {
            if( aPartial )
                *aPartial = true;

            break;
        }

        // The line through the seed up to the first width change on each side, whose links are
        // exactly its segments (AssembleUniformLine), so writing it back can neither change a
        // width nor bridge a stretch of another width.
        LINE line;

        if( !AssembleUniformLine( aNode, seed, true, line ) )
        {
            visited.insert( seed );
            continue;
        }

        bool locked = false;

        for( const LINKED_ITEM* link : line.Links() )
        {
            visited.insert( link );
            locked = locked || link->IsLocked();
        }

        if( locked || line.SegmentCount() < 1 || line.ArcCount() )
            continue;

        if( resolver && resolver->DpCoupledNet( line.Net() ) )
            continue;

        OPTIMIZER optimizer( aNode );
        LINE      glossed;

        optimizer.SetEffortLevel( GLOSS );
        optimizer.SetCollisionMask( ITEM::ANY_T );
        optimizer.SetGlossReference( aReference );
        optimizer.SetGlossBudget( budget );

        if( aPreserve )
            optimizer.SetPreserveVertex( *aPreserve );

        const bool optimized = optimizer.Optimize( &line, &glossed );

        budget = optimizer.m_glossBudget;

        if( optimizer.m_glossBudgetHit && aPartial )
            *aPartial = true;

        if( !optimized || glossed.CLine().CPoints() == line.CLine().CPoints() )
            continue;

        removed += line.SegmentCount() - glossed.SegmentCount();
        changed++;

        // Keep the segments the new shape still has; remove the others.
        const SHAPE_LINE_CHAIN& shape = glossed.CLine();

        auto stillThere =
                [&]( const SEG& aSeg )
                {
                    for( int i = 0; i < shape.SegmentCount(); i++ )
                    {
                        const SEG& s = shape.CSegment( i );

                        if( ( s.A == aSeg.A && s.B == aSeg.B ) || ( s.A == aSeg.B && s.B == aSeg.A ) )
                            return true;
                    }

                    return false;
                };

        for( LINKED_ITEM* link : line.Links() )
        {
            if( link->OfKind( ITEM::SEGMENT_T )
                    && !stillThere( static_cast<SEGMENT*>( link )->Seg() ) )
            {
                aNode->Remove( static_cast<SEGMENT*>( link ) );
            }
        }

        glossed.ClearLinks();
        aNode->Add( glossed );
    }

    if( aLinesChanged )
        *aLinesChanged = changed;

    return removed;
}


// GlossChangedLines(): see the header.
int OPTIMIZER::GlossChangedLines( NODE* aNode, const std::optional<VECTOR2I>& aPreserve )
{
    NODE::ITEM_VECTOR removed, added;

    aNode->GetUpdatedItems( removed, added );

    const NODE* root = aNode;

    while( root->GetParent() )
        root = root->GetParent();

    std::vector<LINKED_ITEM*> seeds;

    for( ITEM* item : added )
    {
        if( item->OfKind( ITEM::SEGMENT_T ) )
            seeds.push_back( static_cast<LINKED_ITEM*>( item ) );
    }

    return GlossLines( aNode, seeds, root, nullptr, aPreserve );
}

}
