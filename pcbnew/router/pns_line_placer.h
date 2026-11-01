/*
 * KiRouter - a push-and-(sometimes-)shove PCB router
 *
 * Copyright (C) 2013-2017 CERN
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * @author Tomasz Wlostowski <tomasz.wlostowski@cern.ch>
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

#ifndef __PNS_LINE_PLACER_H
#define __PNS_LINE_PLACER_H

#include <optional>   // KICLOUD: R3.1 (sameSideAsLastWalk)
#include <unordered_map>   // KICLOUD: R3.1b (WALK_MEMORY::sides)

#include <math/vector2d.h>

#include <geometry/shape.h>
#include <geometry/shape_line_chain.h>

#include "pns_line.h"
#include "pns_mouse_trail_tracer.h"
#include "pns_node.h"
#include "pns_placement_algo.h"
#include "pns_sizes_settings.h"
#include "pns_via.h"
#include "pns_walkaround.h"

namespace PNS {

class ROUTER;
class SHOVE;
class OPTIMIZER;
class VIA;
class SIZES_SETTINGS;
class NODE;

class FIXED_TAIL
{
public:
    FIXED_TAIL( int aLineCount = 1);
    ~FIXED_TAIL();

    struct FIX_POINT
    {
        int          layer;
        bool         placingVias;
        VECTOR2I     p;
        DIRECTION_45 direction;
    };

    struct STAGE
    {
        STAGE() :
            commit( nullptr )
        {}

        STAGE( const STAGE& aOther )
        {
            *this = aOther;
        }

        // Copy operator
        STAGE& operator=( const STAGE& aOther )
        {
            commit = aOther.commit;
            pts = aOther.pts;
            return *this;
        }

        // Move assignment operator
        STAGE& operator=( STAGE&& aOther ) noexcept
        {
            if (this != &aOther)
            {
                commit = aOther.commit;
                pts = std::move( aOther.pts );
            }

            return *this;
        }

        NODE*                  commit;
        std::vector<FIX_POINT> pts;
    };

    void Clear();
    void AddStage( const VECTOR2I& aStart, int aLayer, bool placingVias, DIRECTION_45 direction,
                   NODE* aNode );
    bool PopStage( STAGE& aStage );
    int StageCount() const;

private:
    std::vector<STAGE> m_stages;
};


/**
 * Single track placement algorithm. Interactively routes a track.
 * Applies shove and walkaround algorithms when needed.
 */

class LINE_PLACER : public PLACEMENT_ALGO
{
public:
    LINE_PLACER( ROUTER* aRouter );
    ~LINE_PLACER();

    /**
     * Start routing a single track at point aP, taking item aStartItem as anchor (unless NULL).
     */
    bool Start( const VECTOR2I& aP, ITEM* aStartItem ) override;

    /**
     * Move the end of the currently routed trace to the point \a aP, taking \a aEndItem as
     * anchor (if not NULL).
     */
    bool Move( const VECTOR2I& aP, ITEM* aEndItem ) override;

    /**
     * Commit the currently routed track to the parent node taking \a aP as the final end point
     * and \a aEndItem as the final anchor (if provided).
     *
     * @return true if route has been committed. May return false if the routing result is
     *         violating design rules.  In such cases, the track is only committed if
     *         CanViolateDRC() is on.
     */
    bool FixRoute( const VECTOR2I& aP, ITEM* aEndItem, bool aForceFinish ) override;

    std::optional<VECTOR2I> UnfixRoute() override;

    bool CommitPlacement() override;

    bool AbortPlacement() override;

    bool HasPlacedAnything() const override;

    /**
     * Enable/disable a via at the end of currently routed trace.
     */
    bool ToggleVia( bool aEnabled ) override;

    /**
     * Set the current routing layer.
     */
    bool SetLayer( int aLayer ) override;

    /**
     * Return the "head" of the line being placed, that is the volatile part that has not been
     * "fixed" yet.
     */
    const LINE& Head() const { return m_head; }

    /// KICLOUD: R3.1b: tells code outside the fork (the test hook) that LastWalkStats() exists.
#define KICLOUD_LINE_PLACER_WALK_STATS 1

    /**
     * KICLOUD: R3.1b: what the walk of the last routing step cost, for the routing plan's
     * measurements (FEATURE_ROUTING_TOOLS.md 5.1 item 1b; read by the test hook
     * kicloudTestRouterHead, wasm/bindings/pcbnew_embind.cpp). Reset by routeStep() and added up
     * over the step (Shove mode walks around solids, shoves and may then walk again);
     * written by routeStep(), rhWalkBase(), walkInitialTrack() (walksB), walkSides() (routes),
     * rhShoveOnly(), acceptWalk() and Move() (moveMs, changedItems); reading it changes nothing.
     */
    struct WALK_STATS
    {
        int    routes = 0;         ///< WALKAROUND::Route calls (one per policy walked)
        int    walksB = 0;         ///< walks without the per-obstacle memory after one with it
        int    shoveFallbacks = 0; ///< Shove mode: the shove failed and the full walk ran
        double walkMs = 0.0;       ///< the time in rhWalkBase() (all walks, retries and choices)
        double shoveMs = 0.0;      ///< Shove mode: the time in SHOVE::Run()
        double recordMs = 0.0;     ///< the time in recordObstacleSides() after the step was shown
        double stepMs = 0.0;       ///< the whole routeStep() (walks, shove, optimizer, gloss)
        double moveMs = 0.0;       ///< the whole Move() (routeStep() and the leading ratsnest line)
        int    changedItems = 0;   ///< items the step's node adds or removes against the board
                                   ///< (ROUTER::updateView() then draws and checks each)
        int    heldHeads = 0;      ///< walks that kept the shown head over a partial walk B
        int    iterations = 0;     ///< WALKAROUND iterations over all Route calls
        int    iterationsA = 0;    ///< of those, in walks with the per-obstacle memory (walk A)
        int    retries = 0;        ///< clusters walked again with the other winding (walk A)
    };

    const WALK_STATS& LastWalkStats() const { return m_walkStats; }

    /**
     * KICLOUD: R3.1b: for the routing plan's measurements only (the test hook
     * kicloudTestRouterObstacleMemory): \a aOn false makes every walk R3.1's, as if no obstacle
     * side were remembered, so one build can be measured with and without the per-obstacle
     * memory on the same cursor path. On by default (the page never calls the hook); the sides are
     * still recorded. With it off walkInitialTrack() is R3.1's walk, so the held head, the lower
     * iteration limit and walk B are off too. Affects every LINE_PLACER (a process-wide switch),
     * read by walkInitialTrack().
     */
    static void SetObstacleMemoryForTests( bool aOn ) { s_obstacleMemoryOn = aOn; }

    /**
     * Return the "tail" of the line being placed, the part which has already wrapped around
     * and shoved some obstacles.
     */
    const LINE& Tail() const { return m_tail; }

    /**
     * Return the complete routed line.
     */
    const LINE Trace() const;

    /**
     * Return the complete routed line, as a single-member ITEM_SET.
     */
    const ITEM_SET Traces() override;

    /**
     * Return the current start of the line being placed.
     */
    const VECTOR2I& CurrentStart() const override
    {
        return m_currentStart;
    }

    /**
     * Return the current end of the line being placed. It may not be equal to the cursor
     * position due to collisions.
     */
    const VECTOR2I& CurrentEnd() const override
    {
        return m_currentEnd;
    }

    /**
     * Return the net of currently routed track.
     */
    const std::vector<NET_HANDLE> CurrentNets() const override
    {
        return std::vector<NET_HANDLE>( 1, m_currentNet );
    }

    /**
     * Return the layer of currently routed track.
     */
    int CurrentLayer() const override
    {
        return m_currentLayer;
    }

    /**
     * Return the most recent world state.
     */
    NODE* CurrentNode( bool aLoopsRemoved = false ) const override;

    /**
     * Toggle the current posture (straight/diagonal) of the trace head.
     */
    void FlipPosture() override;

    /**
     * Perform on-the-fly update of the width, via diameter & drill size from a settings class.
     *
     * Performs on-the-fly update of the width, via diameter & drill size from a settings class.
     * Used to dynamically change these parameters as the track is routed.
     */
    void UpdateSizes( const SIZES_SETTINGS& aSizes ) override;

    void SetOrthoMode( bool aOrthoMode ) override;

    bool IsPlacingVia() const override { return m_placingVia; }

    void GetModifiedNets( std::vector<NET_HANDLE>& aNets ) const override;

    /**
     * Snaps the point \a aP to segment \a aSeg. Splits the segment in two, forming a
     * joint at \a aP and stores updated topology in node \a aNode.
     */
    bool SplitAdjacentSegments( NODE* aNode, ITEM* aSeg, const VECTOR2I& aP );

    /**
     * Snaps the point \a aP to arc \a aArc. Splits the arc in two, forming a
     * joint at \a aP and stores updated topology in node \a aNode.
     */
    bool SplitAdjacentArcs( NODE* aNode, ITEM* aArc, const VECTOR2I& aP );

private:
    /**
     * Re-route the current track to point aP. Returns true, when routing has completed
     * successfully (i.e. the trace end has reached point \a aP), and false if the trace was
     * stuck somewhere on the way. May call routeStep() repetitively due to mouse smoothing.
     *
     * @param aP ending point of current route.
     * @return true, if the routing is complete.
     */
    bool route( const VECTOR2I& aP );

    /**
     * Draw the "leading" rats nest line, which connects the end of currently routed track and
     * the nearest yet unrouted item. If the routing for current net is complete, draws nothing.
     */
    void updateLeadingRatLine();

    /**
     * Set the board to route.
     */
    void setWorld( NODE* aWorld );

    /**
     * Initialize placement of a new line with given parameters.
     */
    void initPlacement();

    /**
     * Set preferred direction of the very first track segment to be laid.
     * Used by posture switching mechanism.
     */
    void setInitialDirection( const DIRECTION_45& aDirection );

    /**
     * Searches aNode for traces concurrent to aLatest and removes them. Updated
     * topology is stored in aNode.
     */
    void removeLoops( NODE* aNode, LINE& aLatest );

    /**
     * Assemble a line starting from segment or arc aLatest, removes collinear segments
     * and redundant vertices.  If a simplification has been found, replaces the old line
     * with the simplified one in \a aNode.
     */
    void simplifyNewLine( NODE* aNode, LINKED_ITEM* aLatest );

    /**
     * Check if the head of the track intersects its tail. If so, cuts the tail up to the
     * intersecting segment and fixes the head direction to match the last segment before
     * the cut.
     *
     * @return true if the line has been changed.
     */
    bool handleSelfIntersections();

    /**
     * Deal with pull-back: reduces the tail if head trace is moved backwards wrs to the
     * current tail direction.
     *
     * @return true if the line has been changed.
     */
    bool handlePullback();

    /**
     * Moves "established" segments from the head to the tail if certain conditions are met.
     *
     * @return true, if the line has been changed.
     */
    bool mergeHead();

    /**
     * Attempt to reduce the number of segments in the tail by trying to replace a certain
     * number of latest tail segments with a direct trace leading to \a aEnd that does not
     * collide with anything.
     *
     * @param aEnd is the current routing destination point.
     * @return true if the line has been changed.
     */
    bool reduceTail( const VECTOR2I& aEnd );

    /**
     * Try to reduce the corner count of the most recent part of tail/head by merging
     * obtuse/collinear segments.
     *
     * @return true if the line has been changed.
     */
    bool optimizeTailHeadTransition();

    /**
     * Compute the head trace between the current start point (m_p_start) and point \a aP,
     * starting with direction defined in m_direction.  The trace walks around all
     * colliding solid or non-movable items.  Movable segments are ignored, as they'll be
     * handled later by the shove algorithm.
     */
    bool routeHead( const VECTOR2I& aP, LINE& aNewHead, LINE& aNewTail );

    /**
     * Perform a single routing algorithm step, for the end point \a aP.
     *
     * @param aP is the  ending point of current route.
     * @return true if the line has been changed.
     */
    void routeStep( const VECTOR2I& aP );

    ///< Route step walk around mode.
    bool rhWalkOnly( const VECTOR2I& aP, LINE& aNewHead, LINE& aNewTail );
    bool rhWalkBase( const VECTOR2I& aP, LINE& aWalkLine, int aCollisionMask, PNS::PNS_MODE aMode, bool& aViaOk );

    /**
     * KICLOUD: R3.1b: the key of one obstacle in WALK_MEMORY::sides (obstacleKey()): the board
     * item it was made from and its anchor (WALKAROUND::ObstacleAnchor()). The anchor is part of
     * the key because one board item can become several router obstacles: a keepout rule area one
     * per triangle, a board edge or copper graphic one per piece; each piece mostly keeps its own
     * side (a route through the channel of a U-shaped keepout passes its two arms on opposite
     * sides; pieces whose shapes have the same bounding-box centre still share one key).
     */
    struct OBSTACLE_KEY
    {
        const BOARD_ITEM* item = nullptr;
        VECTOR2I          anchor;

        bool operator==( const OBSTACLE_KEY& aOther ) const
        {
            return item == aOther.item && anchor == aOther.anchor;
        }
    };

    /// KICLOUD: R3.1b: the hash of an OBSTACLE_KEY (the item's address mixed with the anchor).
    struct OBSTACLE_KEY_HASH
    {
        size_t operator()( const OBSTACLE_KEY& aKey ) const
        {
            size_t h = std::hash<const void*>()( aKey.item );
            const size_t a = std::hash<long long>()( ( (long long) aKey.anchor.x << 32 )
                                                     ^ (unsigned int) aKey.anchor.y );
            return h ^ ( a + 0x9e3779b97f4a7c15ULL + ( h << 6 ) + ( h >> 2 ) );
        }
    };

    /// KICLOUD: R3.1b: a lower bound of the length of R3.1's complete walk (walk B) at a cursor
    /// point, from the last step that walked it or could bound it (walkInitialTrack()).
    struct WALK_CHECK
    {
        bool     valid = false;
        double   bLowerBound = 0.0; ///< walk B to `at` was at least this long
        VECTOR2I at;                ///< the walk's end (the cursor) at that step
    };

    /**
     * KICLOUD: R3.1 stable walkaround: what the last walk of this route that the head showed
     * chose. There are two (walkMemorySlot()): one for the solids-only walk of Shove mode and one
     * for the full walk of Walk around mode (and Shove mode's fallback to it), so switching
     * between the two walks does not wipe either. Written only through acceptWalk() once the
     * walk's head is accepted (rhWalkOnly(), rhShoveOnly()), never for a walk that was then
     * refused (a collision, an unusable via). Cleared by initPlacement() (Start). It only applies
     * while the walk starts from the same point on the same layer with the same width
     * (walkMemoryFor()), so a fixing click, Backspace or a layer or width change make the next
     * step behave like the first one.
     */
    struct WALK_MEMORY
    {
        bool     valid = false;
        bool     clockwise = false; ///< the side chosen (WALKAROUND::WP_CW when true, else WP_CCW)
        LINE     walk;              ///< the whole walk (tail + head), from its start to its end
        VECTOR2I start;             ///< where that walk started (the first point of the tail)
        int      layer = -1;
        int      width = 0;

        /// KICLOUD: R3.1b per-obstacle side memory: for each obstacle (by obstacleKey()) that a
        /// shown walk of this stage passed within WALK_OBSTACLE_MEMORY_REACH widths, the side it
        /// lay on (WALKAROUND::ObstacleSide(): +1 or -1), from the latest shown walk that passed
        /// it. Filled by acceptWalk(); read by walkInitialTrack() as the walk's winding
        /// preference. No layer in the key: the memory as a whole applies to one layer
        /// (walkMemoryFor()).
        std::unordered_map<OBSTACLE_KEY, int, OBSTACLE_KEY_HASH> sides;

        /// KICLOUD: R3.1b: what walk B (R3.1's walk) was at least at the last step that walked
        /// or bounded it in this stage. A walk to a new end can be shorter by at most the
        /// distance the end moved, so while walk A stays complete and within the margin of that
        /// bound walk B cannot win and is not walked again (it cost a second walk per move).
        WALK_CHECK checked;
    };

    /// KICLOUD: R3.1b: what walkSides() chose, for walkInitialTrack()'s comparison of the walk
    /// with and without the per-obstacle side memory.
    struct WALK_CHOICE
    {
        bool   complete = false;      ///< a complete path (else a partial "hug" path)
        double length = 0.0;          ///< the complete path's length
        double cursorDist = 0.0;      ///< the partial path's end distance from the cursor
        /// the walk may differ from the one without the per-obstacle memory: the memory changed
        /// a winding, or the other side was not walked because the memory decided every winding
        bool   differsFromPlainWalk = false;

        /// the walk's raw results per policy (before optimization) and, per policy, whether that
        /// result is the walk without the per-obstacle memory (walked, no winding overridden): a
        /// later walkSides() without the memory reuses it instead of walking that policy again
        WALKAROUND::RESULT raw;
        bool               plainKnown[2] = { false, false };
    };

    /**
     * KICLOUD: R3.2 glossing: the last head glossHead() glossed and its result. When the next
     * step brings the same head on the same layer, width, corner mode and world node, the result
     * is reused instead of glossing again (the cursor often moves without changing the head).
     * Walk around mode only; cleared whenever the world node changes (FixRoute, UnfixRoute,
     * SetLayer, initPlacement).
     */
    struct GLOSS_HEAD_CACHE
    {
        bool                  valid = false;
        const NODE*           node = nullptr;
        int                   layer = -1;
        int                   width = 0;
        int                   cornerMode = -1;
        std::vector<VECTOR2I> in;
        SHAPE_LINE_CHAIN      out;
    };

    /// KICLOUD: R3.1: the m_walkMemory slot of a collision mask: 0 for ITEM::SOLID_T (Shove mode's
    /// walk around solids), 1 for every other mask (the full walk).
    static int walkMemorySlot( int aCollisionMask );

    /// KICLOUD: R3.1: the remembered walk that applies to a walk of \a aInitTrack with
    /// \a aCollisionMask (same start point, layer and width), or nullptr. Changes nothing.
    const WALK_MEMORY* walkMemoryFor( int aCollisionMask, const LINE& aInitTrack ) const;

    /// KICLOUD: R3.1: the head of rhWalkBase()'s last walk is shown: keep that walk
    /// (m_pendingWalk) in its slot of m_walkMemory. R3.1b: with the sides of the obstacles near
    /// it (recordObstacleSides()) added to those the slot remembered for the same stage (same
    /// start point, layer and width; started again when WALK_OBSTACLE_MEMORY_SIZE is reached).
    /// Callers: rhWalkOnly(), rhShoveOnly().
    void acceptWalk();

    /// KICLOUD: R3.2: whether the router glosses what it lays now: the "Gloss while routing"
    /// setting is on and the mode is not Highlight collisions (whose free-angle mode and manual
    /// work glossing must not reshape). Read by every gloss of the placer.
    bool glossWhileRouting() const;

    /**
     * KICLOUD: R3.2: glosses \a aHead (a new route head) in place, only its last
     * GLOSS_HEAD_SEGMENTS segments and with a small candidate budget, so the cost of a step does
     * not grow with a long walk-around head; reuses the last result when the head did not change
     * (m_glossHeadCache). Does nothing when glossWhileRouting() is false. Callers: rhWalkOnly(),
     * rhShoveOnly(), after the head's usual optimization.
     */
    void glossHead( LINE& aHead );

    /// KICLOUD: R3.1b: what walkInitialTrack() did: SHOWN (\a aWalkFull holds the walk to show),
    /// HELD (the head shown last is kept: rhWalkBase() returns at once, without the retry from
    /// the last walk, and routeStep() keeps m_head) or FAILED (no usable walk: rhWalkBase() may
    /// retry from the last walk).
    enum class WALK_OUTCOME
    {
        SHOWN,
        HELD,
        FAILED
    };

    /**
     * KICLOUD: R3.1b per-obstacle side memory (docs/patches.md). One walk of rhWalkBase():
     * walks \a aInitTrack (the tail plus a new head to the cursor) around the obstacles in
     * \a aCollisionMask and picks one result. Without a remembered obstacle side for this stage
     * (walkMemoryFor(): the first step, or after a fixing click, Backspace, a layer or width
     * change) it is walkSides(), R3.1's walk. Otherwise it walks twice: walk A with the
     * remembered sides as the walk's winding preference (WALKAROUND::SetWindingPreference():
     * every obstacle the head passed on an earlier shown step is walked on that same side, and
     * only obstacles new to the route get the winding of the walk's policy), and, when the memory
     * changed the walk and walk A is not already within WALK_SIDE_SWITCH_MARGIN of the straight
     * distance, walk B without it (R3.1's walk). Walk B is taken only when it is complete and walk
     * A is not, or it is shorter by more than the margin, or, between two partial paths, it ends
     * nearer the cursor by more than the margin of \a aInitialLength; so an obstacle keeps its
     * side until the other side is clearly better, the same rule as R3.1's for the whole walk.
     * The cost is bounded: both walks stop growing at WALK_MEMORY_LENGTH_LIMIT times the
     * complete-path limit (beyond it a walk is rarely chosen); walk B reuses walk A's result for
     * each policy whose winding the memory did not change; it is not walked while walk A stays
     * complete and within the margin of a lower bound of walk B (WALK_MEMORY::checked, less the
     * distance the cursor moved since), nor when the cursor cannot be reached at all
     * (endBlocked()) and walk A's partial path ends within the margin of it; walk A stops after
     * WALK_MEMORY_ITERATION_LIMIT iterations (remembered sides can send it back and forth; a walk
     * A cut short there is never taken for the plain walk). When walk A finds nothing and walk B
     * only a partial path that passes a remembered obstacle on its other side, the head shown last
     * is held (HELD) unless it now collides, a via is being placed, or walk B ends nearer the
     * cursor by more than the margin of the head's straight length (stock KiCad shows the nearest
     * partial path, which with the memory is often a flip to the other side and back).
     * Measured in FEATURE_ROUTING_TOOLS.md 5.1 item 1b.
     * Inputs as walkSides() (whose length limits both walks keep); the result is a WALK_OUTCOME
     * (HELD only in the case above, never on the first step of a stage), with \a aWalkFull,
     * \a aWalkP and \a aSide set when SHOWN, and \a aChecked: the WALK_MEMORY::checked the shown
     * walk should carry. Reads m_head, m_tail, m_placingVia and m_currentNode (the held head's
     * collision check, endBlocked(), crossesRememberedSide()). Caller: rhWalkBase(), up to twice
     * per round. Changes m_walkStats and, for the duration of the call, \a aWalkaround's length
     * limit, iteration limit and winding preference (all put back on return); no other member.
     */
    WALK_OUTCOME walkInitialTrack( WALKAROUND& aWalkaround, const LINE& aInitTrack,
                                   const VECTOR2I& aP, int aCollisionMask, double aInitialLength,
                                   bool aRememberedSideOnly, LINE& aWalkFull, VECTOR2I& aWalkP,
                                   WALKAROUND::WALK_POLICY& aSide, WALK_CHECK& aChecked );

    /**
     * KICLOUD: R3.1 stable walkaround (docs/patches.md); R3.1's walkInitialTrack(). One walk:
     * walks \a aInitTrack around the obstacles in
     * \a aCollisionMask clockwise and counter-clockwise and picks one result, as KiCad 10 did,
     * except after a previous shown step of this route (walkMemoryFor()): the walk that goes the
     * same way as the previous one (sameSideAsLastWalk()) is kept unless the other is shorter by
     * more than WALK_SIDE_SWITCH_MARGIN (and the kept one must itself be short enough to count as
     * complete); and the winding chosen last step is walked first: when its path is so short that
     * the other side cannot beat it by the margin, the other side is not walked at all. Without a
     * previous step (the first one) it behaves exactly as before. The length limits come from
     * \a aInitialLength (the length of the cursor's own initial track, also for a retry);
     * \a aRememberedSideOnly walks only the remembered side (the retry from lastWalkTrack(); false
     * without a memory). Caller: rhWalkBase(), up to twice per round.
     * \a aObstaclePreference (KICLOUD: R3.1b): \a aWalkaround carries walkInitialTrack()'s
     * per-obstacle winding preference; when the remembered side's walk left no winding to its
     * policy, the other side (which would give the same line) is not walked. \a aReuse (R3.1b,
     * or nullptr): an earlier walkSides() of the same track whose plain per-policy results
     * (WALK_CHOICE::plainKnown) are taken instead of walking those policies again.
     * Results: true, \a aWalkFull (its shape set to the chosen walk, from the tail's start),
     * \a aWalkP (that walk's end) and \a aSide on success; false when neither side gives a usable
     * path (the outputs are then unchanged); \a aChoice (R3.1b) says what was chosen. Changes only
     * m_walkStats (the Route calls). Caller: walkInitialTrack(), once or twice.
     */
    bool walkSides( WALKAROUND& aWalkaround, const LINE& aInitTrack, const VECTOR2I& aP,
                    int aCollisionMask, double aInitialLength, bool aRememberedSideOnly,
                    bool aObstaclePreference, const WALK_CHOICE* aReuse, LINE& aWalkFull,
                    VECTOR2I& aWalkP, WALKAROUND::WALK_POLICY& aSide, WALK_CHOICE& aChoice );

    /// KICLOUD: R3.1b: whether \a aLine passes an obstacle remembered in \a aMemory.sides (one
    /// within WALK_OBSTACLE_MEMORY_REACH widths of it, found as recordObstacleSides() finds them)
    /// on the other side than remembered. Caller: walkInitialTrack(), for the held head. Changes
    /// nothing.
    bool crossesRememberedSide( const SHAPE_LINE_CHAIN& aLine, const WALK_MEMORY& aMemory,
                                int aCollisionMask ) const;

    /// KICLOUD: R3.1b: whether a track end of \a aTrack's width at its last point (the cursor)
    /// collides with an obstacle in \a aCollisionMask in m_currentNode, i.e. no walk can be complete
    /// there (2 um of slack, so an end just touching a clearance does not count). Caller:
    /// walkInitialTrack(), before walk B. Changes nothing.
    bool endBlocked( const LINE& aTrack, int aCollisionMask ) const;

    /// KICLOUD: R3.1b: the key of an obstacle item on \a aLayer in WALK_MEMORY::sides: the board
    /// item it was made from (a hole counts as its pad or via), which stays the same while the
    /// route runs even when Shove mode's nodes copy the item, with the anchor of that pad, via or
    /// item on the layer (OBSTACLE_KEY; an item that moves, such as a track or via another step
    /// shoved, gets a new key and loses its remembered side); no value (not remembered) for an
    /// item the router made itself or one without a shape on the layer. Pure.
    static std::optional<OBSTACLE_KEY> obstacleKey( const ITEM* aItem, int aLayer );

    /**
     * KICLOUD: R3.1b: adds to \a aMemory.sides the side of every obstacle in \a aCollisionMask
     * (other nets, in m_currentNode) within WALK_OBSTACLE_MEMORY_REACH track widths plus its
     * clearance of \a aMemory.walk, measured by WALKAROUND::ObstacleSide() at the obstacle's
     * anchor; obstacles behind the walk's start or ahead of its end are left out (side 0), as
     * are walks with arcs. Only the walk's segments from \a aFirstSegment on are searched (the
     * part the step changed: the tail before it was searched on the earlier steps of the stage);
     * the side is measured against the whole walk. Caller: acceptWalk() (only shown walks are
     * remembered). Changes only \a aMemory.
     */
    void recordObstacleSides( WALK_MEMORY& aMemory, int aCollisionMask, int aFirstSegment ) const;

    /**
     * KICLOUD: R3.1 stable walkaround: which of this step's two walks, \a aCw (clockwise) or
     * \a aCcw (counter-clockwise), goes the same way as \a aMemory, the walk the head showed on
     * the previous step: the one more of whose corners lie on it. WALKAROUND walks every obstacle
     * on the path with the same winding, so "clockwise" alone does not say which side of the
     * obstacle the head is on now: once the head has passed one obstacle (its detour is part of
     * both walks) and meets the next, the previous walk lies on neither walk's new detour and the
     * result is no preference, so the next obstacle's side is chosen by length as in KiCad 10.
     * Caller: walkInitialTrack(). Returns no value when both walks agree with it equally.
     * Changes nothing.
     */
    std::optional<WALKAROUND::WALK_POLICY> sameSideAsLastWalk( const WALK_MEMORY& aMemory,
                                                               const SHAPE_LINE_CHAIN& aCw,
                                                               const SHAPE_LINE_CHAIN& aCcw ) const;

    /**
     * KICLOUD: R3.1 stable walkaround: the fallback of rhWalkBase() when the walk from the
     * current cursor position fails on both sides. Builds, into \a aInitTrack, the last
     * shown walk of this route cut at its point nearest to \a aP, plus a 45-degree trace
     * from there to \a aP, so the next walk only has to resolve the new piece instead of the whole
     * detour again (the "flopping" of gitlab #5381 and #8562, where the head gave up instead).
     * Returns false (and leaves \a aInitTrack alone) when there is no remembered walk for this
     * start point, layer, width and collision mask (walkMemoryFor()), or while a via is being placed (its position
     * is pushed out by buildInitialLine(), which this trace would not follow). Changes nothing.
     */
    bool lastWalkTrack( const VECTOR2I& aP, int aCollisionMask, LINE& aInitTrack ) const;

    bool splitHeadTail( const LINE& aNewLine, const LINE& aOldTail, LINE& aNewHead, LINE& aNewTail );
    bool cursorDistMinimum( const SHAPE_LINE_CHAIN& aL, const VECTOR2I& aCursor,  double lengthThreshold, SHAPE_LINE_CHAIN& aOut );
    bool clipAndCheckCollisions( const VECTOR2I& aP, const SHAPE_LINE_CHAIN& aL, SHAPE_LINE_CHAIN& aOut, int &thresholdDist );

    void updatePStart( const LINE& tail );

    //bool rhPostSplitHeadTail( )

    ///< Route step shove mode.
    bool rhShoveOnly( const VECTOR2I& aP, LINE& aNewHead, LINE& aNewTail );

    ///< Route step mark obstacles mode.
    bool rhMarkObstacles( const VECTOR2I& aP, LINE& aNewHead, LINE& aNewTail );

    const VIA makeVia( const VECTOR2I& aP );

    bool buildInitialLine( const VECTOR2I& aP, LINE& aHead, PNS::PNS_MODE aMode, bool aForceNoVia = false );


    DIRECTION_45   m_direction;         ///< current routing direction
    DIRECTION_45   m_initial_direction; ///< routing direction for new traces

    LINE           m_head;          ///< the volatile part of the track from the previously
                                    ///< analyzed point to the current routing destination

    LINE           m_tail;          ///< routing "tail": part of the track that has been already
                                    ///< fixed due to collisions with obstacles

    NODE*          m_world;         ///< pointer to world to search colliding items
    VECTOR2I       m_p_start;       ///< current routing start (end of tail, beginning of head)
    VECTOR2I       m_fixStart;       ///< start point of the last 'fix'

    std::optional<VECTOR2I>       m_last_p_end;

    std::unique_ptr<SHOVE> m_shove; ///< The shove engine

    NODE*          m_currentNode;   ///< Current world state
    NODE*          m_lastNode;      ///< Postprocessed world state (including marked collisions &
                                    ///< removed loops)

    SIZES_SETTINGS m_sizes;

    bool           m_placingVia;

    NET_HANDLE     m_currentNet;
    int            m_currentLayer;

    VECTOR2I       m_currentEnd;
    VECTOR2I       m_currentStart;
    LINE           m_currentTrace;

    ITEM*          m_startItem;
    ITEM*          m_endItem;

    bool           m_idle;
    bool           m_chainedPlacement;
    bool           m_orthoMode;
    bool           m_placementCorrect;

    FIXED_TAIL     m_fixedTail;
    MOUSE_TRAIL_TRACER m_mouseTrailTracer;

    // KICLOUD: R3.1 / R3.2 state of one route (see WALK_MEMORY and GLOSS_HEAD_CACHE above)
    WALK_MEMORY              m_walkMemory[2];      ///< by walkMemorySlot(): solids-only, full walk
    WALK_MEMORY              m_pendingWalk;        ///< rhWalkBase()'s walk until acceptWalk()
    int                      m_pendingWalkSlot = 1;
    int                      m_pendingWalkMask = ITEM::ANY_T; ///< KICLOUD: R3.1b: its collision mask
    int                      m_pendingWalkHeadSegment = 0;    ///< R3.1b: its first segment not in the tail
    WALK_STATS               m_walkStats;                     ///< R3.1b: LastWalkStats()
    static bool              s_obstacleMemoryOn;              ///< R3.1b: SetObstacleMemoryForTests()
    std::optional<VECTOR2I>  m_walkRetryFailedAt[2]; ///< by slot: where lastWalkTrack()'s retry failed
    GLOSS_HEAD_CACHE         m_glossHeadCache;
};

}

#endif    // __PNS_LINE_PLACER_H
