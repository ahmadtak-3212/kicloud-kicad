/*
 * KiRouter - a push-and-(sometimes-)shove PCB router
 *
 * Copyright (C) 2013-2020 CERN
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

#include <memory>

#include "pns_arc.h"
#include "pns_line.h"
#include "pns_solid.h"
#include "pns_router.h"

#include "pns_component_dragger.h"
#include "pns_debug_decorator.h"

namespace PNS
{

COMPONENT_DRAGGER::COMPONENT_DRAGGER( ROUTER* aRouter ) : DRAG_ALGO( aRouter )
{
    // ensure all variables are initialized
    m_dragStatus = false;
    m_currentNode = nullptr;
}


COMPONENT_DRAGGER::~COMPONENT_DRAGGER()
{
}


bool COMPONENT_DRAGGER::Start( const VECTOR2I& aP, ITEM_SET& aPrimitives )
{
    assert( m_world );

    m_currentNode         = nullptr;
    m_initialDraggedItems = aPrimitives;
    m_p0                  = aP;

    std::unordered_set<LINKED_ITEM*> seenItems;

    auto addLinked =
            [&]( SOLID* aSolid, const JOINT* aJoint, LINKED_ITEM* aItem, VECTOR2I aOffset = {} )
            {
                if( seenItems.count( aItem ) )
                    return;

                seenItems.insert( aItem );

                // Segments that go directly between two linked pads are special-cased
                VECTOR2I otherEnd = ( aJoint->Pos() == aItem->Anchor( 0 ) ) ? aItem->Anchor( 1 )
                                                                            : aItem->Anchor( 0 );
                const JOINT* otherJoint = m_world->FindJoint( otherEnd, aItem->Layer(), aItem->Net() );

                if( otherJoint && otherJoint->LinkCount( ITEM::SOLID_T ) )
                {
                    for( ITEM* otherItem : otherJoint->LinkList() )
                    {
                        if( aPrimitives.Contains( otherItem ) )
                        {
                            m_fixedItems.insert( aItem );
                            return;
                        }
                    }
                }

                int segIndex;
                DRAGGED_CONNECTION cn;

                cn.origLine    = m_world->AssembleLine( aItem, &segIndex );
                cn.attachedPad = aSolid;
                cn.offset      = aOffset;

                // Lines that go directly between two linked pads are also special-cased
                const SHAPE_LINE_CHAIN& line = cn.origLine.CLine();
                const JOINT* jA = m_world->FindJoint( line.CPoint( 0 ), aItem->Layer(), aItem->Net() );
                const JOINT* jB = m_world->FindJoint( line.CLastPoint(), aItem->Layer(), aItem->Net() );

                wxASSERT( jA == aJoint || jB == aJoint );
                const JOINT* jSearch = ( jA == aJoint ) ? jB : jA;

                if( jSearch && jSearch->LinkCount( ITEM::SOLID_T ) )
                {
                    for( ITEM* otherItem : jSearch->LinkList() )
                    {
                        if( aPrimitives.Contains( otherItem ) )
                        {
                            for( ITEM* item : cn.origLine.Links() )
                                m_fixedItems.insert( item );

                            return;
                        }
                    }
                }

                m_conns.push_back( std::move( cn ) );
            };

    for( ITEM* item : aPrimitives.Items() )
    {
        if( item->Kind() != ITEM::SOLID_T )
            continue;

        SOLID* solid = static_cast<SOLID*>( item );

        m_solids.insert( solid );

        if( !item->IsRoutable() )
            continue;

        const JOINT* jt = m_world->FindJoint( solid->Pos(), solid );

        for( ITEM* link : jt->LinkList() )
        {
            if( link->OfKind( ITEM::SEGMENT_T | ITEM::ARC_T ) )
                addLinked( solid, jt, static_cast<LINKED_ITEM*>( link ) );
        }

        std::vector<JOINT*> extraJoints;

        m_world->QueryJoints( solid->Hull().BBox(), extraJoints, solid->Layers(),
                              ITEM::SEGMENT_T | ITEM::ARC_T );

        for( JOINT* extraJoint : extraJoints )
        {
            if( extraJoint->Net() == jt->Net() && extraJoint->LinkCount() == 1 )
            {
                LINKED_ITEM* li = static_cast<LINKED_ITEM*>( extraJoint->LinkList().front() );

                if( li->Collide( solid, m_world, solid->Layer() ) )
                    addLinked( solid, extraJoint, li, extraJoint->Pos() - solid->Pos() );
            }
        }
    }

    return true;
}


bool COMPONENT_DRAGGER::Drag( const VECTOR2I& aP )
{
    assert( m_world );

    m_world->KillChildren();
    m_currentNode = m_world->Branch();

    for( ITEM* item : m_initialDraggedItems )
        m_currentNode->Remove( item );

    m_draggedItems.Clear();

    for( SOLID* s : m_solids )
    {
        VECTOR2I               p_next = aP - m_p0 + s->Pos();
        std::unique_ptr<SOLID> snew( static_cast<SOLID*>( s->Clone() ) );
        snew->SetPos( p_next );

        m_draggedItems.Add( snew.get() );
        m_currentNode->Add( std::move( snew ) );

        if( !s->IsRoutable() )
            continue;

        for( DRAGGED_CONNECTION& l : m_conns )
        {
            if( l.attachedPad == s )
            {
                l.p_orig = s->Pos() + l.offset;
                l.p_next = p_next + l.offset;
            }
        }
    }

    for( ITEM* item : m_fixedItems )
    {
        m_currentNode->Remove( item );

        switch( item->Kind() )
        {
        case ITEM::SEGMENT_T:
        {
            SEGMENT*                 s = static_cast<SEGMENT*>( item );
            std::unique_ptr<SEGMENT> s_new( s->Clone() );

            SEG orig = s->Seg();
            s_new->SetEnds( aP - m_p0 + orig.A, aP - m_p0 + orig.B );

            m_draggedItems.Add( s_new.get() );
            m_currentNode->Add( std::move( s_new ) );

            break;
        }

        case ITEM::ARC_T:
        {
            ARC* a = static_cast<ARC*>( item );
            std::unique_ptr<ARC> a_new( a->Clone() );

            SHAPE_ARC& arc = a_new->Arc();
            arc.Move( aP - m_p0 );

            m_draggedItems.Add( a_new.get() );
            m_currentNode->Add( std::move( a_new ) );
            break;
        }

        default:
            wxFAIL_MSG( wxT( "Unexpected item type in COMPONENT_DRAGGER::m_fixedItems" ) );
        }
    }

    for( COMPONENT_DRAGGER::DRAGGED_CONNECTION& cn : m_conns )
    {
        LINE l_new( cn.origLine );
        l_new.Unmark();
        l_new.ClearLinks();
        l_new.DragCorner( cn.p_next, cn.origLine.CLine().Find( cn.p_orig ) );

        PNS_DBG( Dbg(), AddItem, &l_new, BLUE, 0, wxT( "cdrag-new-fanout" ) );
        m_draggedItems.Add( l_new );

        LINE l_orig( cn.origLine );
        m_currentNode->Remove( l_orig );
        m_currentNode->Add( l_new );
    }

    // KICLOUD: R3.5 know at every step whether this position could be dropped, so the router
    // tool can say so next to the cursor while the person is still moving (Highlight collisions
    // mode commits anyway, so nothing is checked there). The check is the one FixRoute() makes,
    // over the few items this drag moved.
    m_dragStatus = Settings().AllowDRCViolations() || !findCollision( m_currentNode );

    return true;
}


// KICLOUD: R3.5 Checks each dragged item (m_draggedItems: the moved pads and footprint shapes,
// the tracks between two of the part's pads, and every track bent to follow a pad) against
// aNode with NODE::CheckColliding, in the order they were added, and stops at the first hit:
// the same test, in the same order, as KiCad's FixRoute made with CheckColliding( ITEM_SET ).
// Records the dragged item (pad or track, its board item and net) and the item it hits (kind,
// board item, net) in m_kicloudCollision, for the router tool's message; a hole hit is recorded as
// its pad or via; an obstacle the drag itself created (another bent track of the same part) has no
// board item, only its net.
// Input: the node to check in (the drag's current branch). Result: whether a collision exists.
// State: m_kicloudCollision only.
bool COMPONENT_DRAGGER::findCollision( NODE* aNode )
{
    m_kicloudCollision = KICLOUD_DROP_COLLISION();

    if( !aNode )
        return false;

    for( const ITEM* item : m_draggedItems.CItems() )
    {
        NODE::OPT_OBSTACLE obs = aNode->CheckColliding( item );

        if( !obs )
            continue;

        m_kicloudCollision.m_found = true;
        m_kicloudCollision.m_headIsLine =
                item->OfKind( ITEM::LINE_T | ITEM::SEGMENT_T | ITEM::ARC_T );
        m_kicloudCollision.m_headParent = item->Parent();
        m_kicloudCollision.m_headNet = item->Net();

        if( const ITEM* hit = obs->m_item )
        {
            // a pad's or via's hole is an item of its own without a board item: name its pad or via
            if( !hit->Parent() && hit->ParentPadVia() )
                hit = hit->ParentPadVia();

            m_kicloudCollision.m_obstacleKind = hit->Kind();
            m_kicloudCollision.m_obstacleParent = hit->Parent();
            m_kicloudCollision.m_obstacleNet = hit->Net();
        }

        return true;
    }

    return false;
}


// Commits the drag at the position of the last Drag(): when nothing collides (or in Highlight
// collisions mode, or when the person forces it with Ctrl+click), the drag's branch becomes the
// board. Otherwise nothing changes, the drag stays where it is and false is returned.
// KICLOUD: R3.5: the collision is found by findCollision(), which also records what collides, so
// that the router tool can tell the person why the drop was refused (KiCad ended the drag
// silently and the part went back).
bool COMPONENT_DRAGGER::FixRoute( bool aForceCommit )
{
    NODE* node = CurrentNode();

    if( node )
    {
        if( Settings().AllowDRCViolations() || aForceCommit || !findCollision( node ) )
        {
            Router()->CommitRouting( node );
            return true;
        }
    }

    return false;
}


NODE* COMPONENT_DRAGGER::CurrentNode() const
{
    return m_currentNode ? m_currentNode : m_world;
}


const ITEM_SET COMPONENT_DRAGGER::Traces()
{
    return m_draggedItems;
}

}; // namespace PNS
