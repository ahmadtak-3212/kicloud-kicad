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

#ifndef __PNS_COMPONENT_DRAGGER_H
#define __PNS_COMPONENT_DRAGGER_H

#include <math/vector2d.h>

#include "pns_drag_algo.h"
#include "pns_item.h"   // KICLOUD: R3.5 (ITEM::PnsKind, NET_HANDLE in KICLOUD_DROP_COLLISION)

class BOARD_ITEM;

namespace PNS
{
class ROUTER;
class OPTIMIZER;

/**
 * COMPONENT_DRAGGER
 *
 * Implements component dragging algorithm.
 */
class COMPONENT_DRAGGER : public DRAG_ALGO
{
public:
    COMPONENT_DRAGGER( ROUTER* aRouter );
    ~COMPONENT_DRAGGER();


    /**
     * Function Start()
     *
     * Starts routing a single track at point aP, taking item aStartItem as anchor
     * (unless NULL). Returns true if a dragging operation has started.
     */
    bool Start( const VECTOR2I& aP, ITEM_SET& aPrimitives ) override;

    /**
     * Function Drag()
     *
     * Drags the current segment/corner/via to the point aP.
     * @return true, if dragging finished with success.
     */
    bool Drag( const VECTOR2I& aP ) override;

    /**
     * Function FixRoute()
     *
     * Checks if the result of current dragging operation is correct
     * and eventually commits it to the world.
     * @return true, if dragging finished with success.
     */
    bool FixRoute( bool aForceCommit ) override;

    /**
     * Function CurrentNode()
     *
     * Returns the most recent world state, including all
     * items changed due to dragging operation.
     */
    NODE* CurrentNode() const override;

    /**
     * Function CurrentNets()
     *
     * Returns the net(s) of currently dragged item(s).
     * Currently unused for component dragging.
     */
    const std::vector<NET_HANDLE> CurrentNets() const override
    {
        return std::vector<NET_HANDLE>();
    }

    /**
     * Function CurrentLayer()
     *
     * Returns the layer of currently dragged item(s).
     * Currently unused for component dragging.
     */
    virtual int CurrentLayer() const override
    {
        return UNDEFINED_LAYER;
    }

    /**
     * Function Traces()
     *
     * Returns the set of dragged items.
     */
    const ITEM_SET Traces() override;

    virtual PNS::DRAG_MODE Mode() const override
    {
        return PNS::DM_COMPONENT;
    }

    bool GetForceMarkObstaclesMode( bool* aDragStatus ) const override
    {
        *aDragStatus = m_dragStatus;
        return false;
    }

    /**
     * KICLOUD: R3.5 what stops the part from being dropped here: the first collision that the
     * last Drag() or FixRoute() found between a dragged item (a moved pad, a footprint graphic,
     * or a track bent to follow a pad) and the rest of the board. Filled by findCollision(); empty
     * (m_found false) when the dragged items collide with nothing, or in Highlight collisions mode
     * (which commits anyway). The board items it points to are the ones the router world was
     * built from: they live as long as the drag (the board is not edited during a drag); the
     * router's own items are not kept, because the next Drag() frees its branch.
     */
    struct KICLOUD_DROP_COLLISION
    {
        bool        m_found = false;
        bool        m_headIsLine = false;          ///< the dragged item is a track
        BOARD_ITEM* m_headParent = nullptr;        ///< the dragged pad (or shape), if any
        NET_HANDLE  m_headNet = nullptr;
        ITEM::PnsKind m_obstacleKind = ITEM::INVALID_T;
        BOARD_ITEM* m_obstacleParent = nullptr;    ///< the board item hit, if the hit has one
        NET_HANDLE  m_obstacleNet = nullptr;
    };

    /// KICLOUD: R3.5 the collision that blocks the drop at the current position (see above).
    const KICLOUD_DROP_COLLISION& KicloudDropCollision() const { return m_kicloudCollision; }

private:
    // KICLOUD: R3.5 checks every dragged item against aNode, stops at the first collision and
    // records it in m_kicloudCollision. Returns whether one was found.
    bool findCollision( NODE* aNode );

    struct DRAGGED_CONNECTION
    {
        LINE     origLine;
        SOLID*   attachedPad;
        VECTOR2I p_orig, p_next;
        VECTOR2I offset;
    };

    std::set<SOLID*>                m_solids;       // Pads being dragged
    std::set<ITEM*>                 m_fixedItems;   // Items being moved along with pads
    std::vector<DRAGGED_CONNECTION> m_conns;        // Lines being dragged with pads

    bool     m_dragStatus;
    ITEM_SET m_draggedItems;
    ITEM_SET m_initialDraggedItems;
    NODE*    m_currentNode;
    VECTOR2I m_p0;

    KICLOUD_DROP_COLLISION m_kicloudCollision;   // KICLOUD: R3.5
};

}; // namespace PNS

#endif
