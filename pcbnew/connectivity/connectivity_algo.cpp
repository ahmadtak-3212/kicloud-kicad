/*
 * This program source code file is part of KICAD, a free EDA CAD application.
 *
 * Copyright (C) 2016-2018 CERN
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * @author Tomasz Wlostowski <tomasz.wlostowski@cern.ch>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you may find one here:
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 * or you may search the http://www.gnu.org website for the version 2 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */


#include <algorithm>
#include <cstdint>     // KICLOUD: R2-C uintptr_t for CN_ITEM_PTR_SET
#include <future>
#include <mutex>
#include <ranges>

#include <connectivity/connectivity_algo.h>
#include <progress_reporter.h>
#include <geometry/geometry_utils.h>
#include <board.h>
#include <board_commit.h>
#include <thread_pool.h>
#include <footprint.h>
#include <pad.h>
#include <pcb_shape.h>
#include <pcb_track.h>

#include <wx/log.h>
#include <kicloud_collab_hooks.h> // KICLOUD: L8.3a collab capture of propagated nets

#ifdef PROFILE
#include <core/profile.h>
#endif


bool CN_CONNECTIVITY_ALGO::Remove( BOARD_ITEM* aItem )
{
    bool anythingDeleted = false;
    markItemNetAsDirty( aItem );

    switch( aItem->Type() )
    {
    case PCB_FOOTPRINT_T:
        for( PAD* pad : static_cast<FOOTPRINT*>( aItem )->Pads() )
        {
            if( m_itemMap.find( pad ) != m_itemMap.end() ) // prevent double deletion
            {
                m_itemMap[pad].MarkItemsAsInvalid();
                m_itemMap.erase( pad );
                anythingDeleted = true;
            }
        }

        m_itemList.SetDirty( true );
        break;

    case PCB_PAD_T:
    case PCB_TRACE_T:
    case PCB_ARC_T:
    case PCB_VIA_T:
    case PCB_ZONE_T:
    case PCB_SHAPE_T:
        if( m_itemMap.find( aItem ) != m_itemMap.end() ) // prevent double deletion
        {
            m_itemMap[aItem].MarkItemsAsInvalid();
            m_itemMap.erase ( aItem );
            m_itemList.SetDirty( true );
            anythingDeleted = true;
        }
        break;

    default:
        return false;
    }


    // Once we delete an item, it may connect between lists, so mark both as potentially invalid
    if( anythingDeleted )
        m_itemList.SetHasInvalid( true );

    return true;
}


void CN_CONNECTIVITY_ALGO::markItemNetAsDirty( const BOARD_ITEM* aItem )
{
    if( aItem->IsConnected() )
    {
        const BOARD_CONNECTED_ITEM* citem = static_cast<const BOARD_CONNECTED_ITEM*>( aItem );
        MarkNetAsDirty( citem->GetNetCode() );
    }
    else
    {
        if( aItem->Type() == PCB_FOOTPRINT_T )
        {
            const FOOTPRINT* footprint = static_cast<const FOOTPRINT*>( aItem );

            for( PAD* pad : footprint->Pads() )
                MarkNetAsDirty( pad->GetNetCode() );
        }
    }
}


bool CN_CONNECTIVITY_ALGO::Add( BOARD_ITEM* aItem )
{
    if( !aItem->IsOnCopperLayer() )
        return false;

    auto alreadyAdded =
            [this]( BOARD_ITEM* item )
            {
                auto it = m_itemMap.find( item );

                if( it == m_itemMap.end() )
                    return false;

                // Don't be fooled by an empty ITEM_MAP_ENTRY auto-created by operator[].
                return !it->second.GetItems().empty();
            };

    switch( aItem->Type() )
    {
    case PCB_NETINFO_T:
        MarkNetAsDirty( static_cast<NETINFO_ITEM*>( aItem )->GetNetCode() );
        break;

    case PCB_FOOTPRINT_T:
    {
        if( static_cast<FOOTPRINT*>( aItem )->GetAttributes() & FP_JUST_ADDED )
            return false;

        for( PAD* pad : static_cast<FOOTPRINT*>( aItem )->Pads() )
        {
            if( alreadyAdded( pad ) )
                return false;

            add( m_itemList, pad );
        }

        break;
    }

    case PCB_PAD_T:
    {
        if( FOOTPRINT* fp = aItem->GetParentFootprint() )
        {
            if( fp->GetAttributes() & FP_JUST_ADDED )
                return false;
        }

        if( alreadyAdded( aItem ) )
            return false;

        add( m_itemList, static_cast<PAD*>( aItem ) );
        break;
    }

    case PCB_TRACE_T:
        if( alreadyAdded( aItem ) )
            return false;

        add( m_itemList, static_cast<PCB_TRACK*>( aItem ) );
        break;

    case PCB_ARC_T:
        if( alreadyAdded( aItem ) )
            return false;

        add( m_itemList, static_cast<PCB_ARC*>( aItem ) );
        break;

    case PCB_VIA_T:
        if( alreadyAdded( aItem ) )
            return false;

        add( m_itemList, static_cast<PCB_VIA*>( aItem ) );
        break;

    case PCB_SHAPE_T:
        if( alreadyAdded( aItem ) )
            return false;

        if( !IsCopperLayer( aItem->GetLayer() ) )
            return false;

        add( m_itemList, static_cast<PCB_SHAPE*>( aItem ) );
        break;

    case PCB_ZONE_T:
    {
        ZONE* zone = static_cast<ZONE*>( aItem );

        if( alreadyAdded( aItem ) )
            return false;

        m_itemMap[zone] = ITEM_MAP_ENTRY();

        // Don't check for connections on layers that only exist in the zone but
        // were disabled in the board
        BOARD* board = zone->GetBoard();
        LSET layerset = board->GetEnabledLayers() & zone->GetLayerSet();

        layerset.RunOnLayers(
                [&]( PCB_LAYER_ID layer )
                {
                    for( CN_ITEM* zitem : m_itemList.Add( zone, layer ) )
                        m_itemMap[zone].Link( zitem );
                } );

        break;
    }

    default:
        return false;
    }

    markItemNetAsDirty( aItem );

    return true;
}


void CN_CONNECTIVITY_ALGO::RemoveInvalidRefs()
{
    for( CN_ITEM* item : m_itemList )
        item->RemoveInvalidRefs();
}


void CN_CONNECTIVITY_ALGO::searchConnections()
{
    std::lock_guard lock( m_mutex );
#ifdef PROFILE
    PROF_TIMER garbage_collection( "garbage-collection" );
#endif
    std::vector<CN_ITEM*> garbage;
    garbage.reserve( 1024 );

    m_parentConnectivityData->RemoveInvalidRefs();

    if( m_isLocal )
        m_globalConnectivityData->RemoveInvalidRefs();

    m_itemList.RemoveInvalidItems( garbage );

    for( CN_ITEM* item : garbage )
        delete item;

#ifdef PROFILE
    garbage_collection.Show();
    PROF_TIMER search_basic( "search-basic" );
#endif

    thread_pool& tp = GetKiCadThreadPool();
    std::vector<CN_ITEM*> dirtyItems;
    std::copy_if( m_itemList.begin(), m_itemList.end(), std::back_inserter( dirtyItems ),
                  [] ( CN_ITEM* aItem )
                  {
                      return aItem->Dirty();
                  } );

    if( m_progressReporter )
    {
        m_progressReporter->SetMaxProgress( dirtyItems.size() );

        if( !m_progressReporter->KeepRefreshing() )
            return;
    }

    if( m_itemList.IsDirty() )
    {
        std::vector<std::future<size_t>> returns( dirtyItems.size() );

        // Collect deferred net code changes to avoid data races in parallel search.
        // Vias connected to zones have their net codes updated after all parallel work
        // completes, but only if the via has no higher-priority connections (tracks, pads).
        std::vector<std::pair<CN_ITEM*, int>> deferredNetCodes;
        std::mutex deferredNetCodesMutex;

        for( size_t ii = 0; ii < dirtyItems.size(); ++ii )
        {
            returns[ii] = tp.submit_task(
                    [&dirtyItems, ii, this, &deferredNetCodes, &deferredNetCodesMutex] () ->size_t
                    {
                        if( m_progressReporter && m_progressReporter->IsCancelled() )
                            return 0;

                        CN_VISITOR visitor( dirtyItems[ii], &deferredNetCodes, &deferredNetCodesMutex );
                        m_itemList.FindNearby( dirtyItems[ii], visitor );

                        if( m_progressReporter )
                            m_progressReporter->AdvanceProgress();

                        return 1;
                    } );
        }

        for( const std::future<size_t>& ret : returns )
        {
            // Here we balance returns with a 250ms timeout to allow UI updating
            std::future_status status = ret.wait_for( std::chrono::milliseconds( 250 ) );

            while( status != std::future_status::ready )
            {
                if( m_progressReporter )
                    m_progressReporter->KeepRefreshing();

                status = ret.wait_for( std::chrono::milliseconds( 250 ) );
            }
        }

        // Apply deferred zone net changes, but only for vias that have no non-zone
        // connections.  Tracks and pads take priority over zones for net assignment;
        // cluster-based propagation will handle those vias.
        std::sort( deferredNetCodes.begin(), deferredNetCodes.end(),
                   []( const auto& a, const auto& b ) { return a.first < b.first; } );

        CN_ITEM* lastItem = nullptr;

        for( const auto& [cnItem, netCode] : deferredNetCodes )
        {
            if( cnItem == lastItem )
                continue;

            lastItem = cnItem;

            if( std::ranges::none_of( cnItem->ConnectedItems(),
                    []( const CN_ITEM* c )
                    {
                        return c->Parent()->Type() != PCB_ZONE_T;
                    } ) )
            {
                // KICLOUD: L8.3a the collab bridge captures the zone net a via took (no
                // listener hears it); this runs on every rebuild, mostly with the same net
                const int oldNetCode = cnItem->Parent()->GetNetCode();

                cnItem->Parent()->SetNetCode( netCode );

                if( KICLOUD_COLLAB_HOOKS::ItemChangedUnnotified
                        && cnItem->Parent()->GetNetCode() != oldNetCode )
                {
                    KICLOUD_COLLAB_HOOKS::ItemChangedUnnotified( cnItem->Parent(), false );
                }

                // KICLOUD: R2-A2: tell the router's board interface that an item changed without
                // a notification (see BOARD::KicloudUnnotifiedChangeCount)
                if( cnItem->Parent()->GetNetCode() != oldNetCode )
                {
                    if( BOARD* board = cnItem->Parent()->GetBoard() )
                        board->KicloudNoteChange( true );
                }
            }
        }

        if( m_progressReporter )
            m_progressReporter->KeepRefreshing();
    }

#ifdef PROFILE
        search_basic.Show();
#endif

    m_itemList.ClearDirtyFlags();
}


const CN_CONNECTIVITY_ALGO::CLUSTERS CN_CONNECTIVITY_ALGO::SearchClusters( CLUSTER_SEARCH_MODE aMode )
{
    return SearchClusters( aMode, ( aMode == CSM_PROPAGATE ), -1 );
}


namespace
{

// KICLOUD: R2-C: the "already in a cluster" set of SearchClusters().
// A set of CN_ITEM pointers stored in one flat, open-addressed table (linear probing, power-of-two
// capacity, never more than half full). Upstream used std::set<CN_ITEM*>, which allocates a tree
// node per insert and chases pointers on every lookup: about 60 ms per call on the largest demo
// board (~40,000 items), and SearchClusters runs on every route finish, undo and redo. This table
// does one allocation up front and a multiply + mask per lookup.
// Why not a "visited" flag or epoch on CN_ITEM itself: that needs connectivity_items.h, and
// SearchClusters is also called through const CONNECTIVITY_DATA::GetConnectedItems(), possibly
// from several threads at once on the same items; a table local to each call cannot race.
// Lifetime: lives for one SearchClusters() call; stores the pointers only, never dereferences them.
class CN_ITEM_PTR_SET
{
public:
    // Make an empty set sized for about aExpected pointers (it grows if more are inserted).
    explicit CN_ITEM_PTR_SET( size_t aExpected )
    {
        size_t capacity = 16;

        while( capacity < aExpected * 2 )
            capacity <<= 1;

        m_slots.assign( capacity, nullptr );
        m_mask = capacity - 1;
    }

    // Add aItem (not null). Returns true when it was not in the set yet, false when it already was
    // (the set is unchanged then). State: may grow (rehash) the table.
    bool Insert( CN_ITEM* aItem )
    {
        if( ( m_count + 1 ) * 2 > m_slots.size() )
            grow();

        size_t i = slot( aItem );

        while( m_slots[i] )
        {
            if( m_slots[i] == aItem )
                return false;

            i = ( i + 1 ) & m_mask;
        }

        m_slots[i] = aItem;
        m_count++;
        return true;
    }

    // True when aItem was inserted before. No state change.
    bool Contains( CN_ITEM* aItem ) const
    {
        size_t i = slot( aItem );

        while( m_slots[i] )
        {
            if( m_slots[i] == aItem )
                return true;

            i = ( i + 1 ) & m_mask;
        }

        return false;
    }

private:
    // First slot to probe for aItem: the pointer's bits mixed by a Fibonacci multiply so that
    // neighbouring allocations do not fall into neighbouring slots.
    size_t slot( CN_ITEM* aItem ) const
    {
        uint64_t h = static_cast<uint64_t>( reinterpret_cast<uintptr_t>( aItem ) );
        h *= 0x9E3779B97F4A7C15ULL;
        return static_cast<size_t>( h >> 32 ) & m_mask;
    }

    // Double the table and re-insert every pointer. Only reached when the caller's size estimate
    // was low (a connected item that is not in the item list); kept for safety.
    void grow()
    {
        std::vector<CN_ITEM*> old;
        old.swap( m_slots );
        m_slots.assign( old.size() * 2, nullptr );
        m_mask = m_slots.size() - 1;
        m_count = 0;

        for( CN_ITEM* item : old )
        {
            if( item )
                Insert( item );
        }
    }

    std::vector<CN_ITEM*> m_slots;
    size_t                m_mask = 0;
    size_t                m_count = 0;
};

} // namespace


// Split the connectivity graph into clusters: groups of items joined by connections (pads, tracks,
// arcs, vias, zone islands, shapes). Callers: PropagateNets (aMode CSM_PROPAGATE: nets ignored,
// zones excluded), the ratsnest (CSM_RATSNEST) and the connectivity check (CSM_CONNECTIVITY_CHECK),
// both of which keep a cluster inside one net, and CONNECTIVITY_DATA::GetConnectedItems (any mode,
// optionally one net only: aSingleNet >= 0; aExcludeZones leaves zone items out).
// Result: the clusters, sorted by origin net. Returns no clusters when the progress reporter was
// cancelled. State: refreshes the item connections first if the item list is dirty; otherwise
// reads only.
// KICLOUD: R2-C: same clusters as upstream with less overhead. Upstream kept the start candidates in
// a std::set<CN_ITEM*> (ordered by memory address) and the visited items in a second one; now the
// candidates are a vector in item-list order and the visited items a flat pointer table (see
// CN_ITEM_PTR_SET). Each cluster is still the connected component of its first unvisited
// candidate (connections are made in both directions, so any member reaches the same component),
// built by the same breadth-first search with the same filters. Only the order changes: clusters
// of one net now come in item-list order (the order items were added to the connectivity data),
// where upstream used memory-address order; and the sort by net is stable so that order is kept.
// Nothing reads a meaning into the order within a net (propagation treats every cluster on its own;
// a cluster's origin net comes from its pads, or its lowest-uuid item since L8.6), and the new
// order is the more reproducible one.
const CN_CONNECTIVITY_ALGO::CLUSTERS
CN_CONNECTIVITY_ALGO::SearchClusters( CLUSTER_SEARCH_MODE aMode, bool aExcludeZones, int aSingleNet )
{
    bool withinAnyNet = ( aMode != CSM_PROPAGATE );

    std::deque<CN_ITEM*> Q;

    CLUSTERS clusters;

    if( m_itemList.IsDirty() )
        searchConnections();

    // The items a cluster may start from (upstream's item_set), in item-list order.
    std::vector<CN_ITEM*> candidates;
    candidates.reserve( m_itemList.Size() );

    CN_ITEM_PTR_SET visited( m_itemList.Size() );

    auto addToSearchList =
            [&candidates, withinAnyNet, aSingleNet, &aExcludeZones]( CN_ITEM *aItem )
            {
                if( withinAnyNet && aItem->Net() <= 0 )
                    return;

                if( !aItem->Valid() )
                    return;

                if( aSingleNet >=0 && aItem->Net() != aSingleNet )
                    return;

                if( aExcludeZones && aItem->Parent()->Type() == PCB_ZONE_T )
                    return;

                candidates.push_back( aItem );
            };

    std::for_each( m_itemList.begin(), m_itemList.end(), addToSearchList );

    if( m_progressReporter && m_progressReporter->IsCancelled() )
        return CLUSTERS();

    for( CN_ITEM* root : candidates )
    {
        // Already reached from an earlier root: it belongs to that root's cluster.
        if( !visited.Insert( root ) )
            continue;

        std::shared_ptr<CN_CLUSTER> cluster = std::make_shared<CN_CLUSTER>();

        Q.clear();
        Q.push_back( root );

        while( Q.size() )
        {
            CN_ITEM* current = Q.front();

            Q.pop_front();
            cluster->Add( current );

            for( CN_ITEM* n : current->ConnectedItems() )
            {
                if( withinAnyNet && n->Net() != root->Net() )
                    continue;

                if( aExcludeZones && n->Parent()->Type() == PCB_ZONE_T )
                    continue;

                if( n->Valid() && visited.Insert( n ) )
                    Q.push_back( n );
            }
        }

        clusters.push_back( std::move( cluster ) );
    }

    if( m_progressReporter && m_progressReporter->IsCancelled() )
        return CLUSTERS();

    std::stable_sort( clusters.begin(), clusters.end(),
                      []( const std::shared_ptr<CN_CLUSTER>& a, const std::shared_ptr<CN_CLUSTER>& b )
                      {
                          return a->OriginNet() < b->OriginNet();
                      } );

    return clusters;
}


void CN_CONNECTIVITY_ALGO::Build( BOARD* aBoard, PROGRESS_REPORTER* aReporter )
{
    // Generate CN_ZONE_LAYERs for each island on each layer of each zone
    //
    std::vector<CN_ZONE_LAYER*> zitems;

    for( ZONE* zone : aBoard->Zones() )
    {
        if( zone->IsOnCopperLayer() )
        {
            m_itemMap[zone] = ITEM_MAP_ENTRY();
            markItemNetAsDirty( zone );

            // Don't check for connections on layers that only exist in the zone but
            // were disabled in the board
            BOARD* board = zone->GetBoard();
            LSET layerset = board->GetEnabledLayers() & zone->GetLayerSet() & LSET::AllCuMask();

            layerset.RunOnLayers(
                    [&]( PCB_LAYER_ID layer )
                    {
                        for( int j = 0; j < zone->GetFilledPolysList( layer )->OutlineCount(); j++ )
                            zitems.push_back( new CN_ZONE_LAYER( zone, layer, j ) );
                    } );
        }
    }

    // Setup progress metrics
    //
    int    progressDelta = 50;
    double size = 0.0;

    size += zitems.size();        // Once for building RTrees
    size += zitems.size();        // Once for adding to connectivity
    size += aBoard->Tracks().size();
    size += aBoard->Drawings().size();

    for( FOOTPRINT* footprint : aBoard->Footprints() )
        size += footprint->Pads().size();

    size *= 1.5;                  // Our caller gets the other third of the progress bar

    progressDelta = std::max( progressDelta, (int) size / 4 );

    auto report =
            [&]( int progress )
            {
                if( aReporter && ( progress % progressDelta ) == 0 )
                {
                    aReporter->SetCurrentProgress( progress / size );
                    aReporter->KeepRefreshing( false );
                }
            };

    // Generate RTrees for CN_ZONE_LAYER items (in parallel)
    //
    thread_pool& tp = GetKiCadThreadPool();
    std::vector<std::future<size_t>> returns( zitems.size() );

    auto cache_zones =
            [aReporter]( CN_ZONE_LAYER* aZoneLayer ) -> size_t
            {
                if( aReporter && aReporter->IsCancelled() )
                    return 0;

                aZoneLayer->BuildRTree();

                if( aReporter )
                    aReporter->AdvanceProgress();

                return 1;
            };

    for( size_t ii = 0; ii < zitems.size(); ++ii )
    {
        CN_ZONE_LAYER* ptr = zitems[ii];
        returns[ii] = tp.submit_task(
            [cache_zones, ptr] { return cache_zones( ptr ); } );
    }

    for( const std::future<size_t>& ret : returns )
    {
        std::future_status status = ret.wait_for( std::chrono::milliseconds( 250 ) );

        while( status != std::future_status::ready )
        {
            if( aReporter )
                aReporter->KeepRefreshing();

            status = ret.wait_for( std::chrono::milliseconds( 250 ) );
        }

    }

    // Add CN_ZONE_LAYERS, tracks, and pads to connectivity
    //
    int ii = zitems.size();

    for( CN_ZONE_LAYER* zitem : zitems )
    {
        m_itemList.Add( zitem );
        m_itemMap[ zitem->Parent() ].Link( zitem );
        report( ++ii );
    }

    for( PCB_TRACK* tv : aBoard->Tracks() )
    {
        Add( tv );
        report( ++ii );
    }

    for( FOOTPRINT* footprint : aBoard->Footprints() )
    {
        for( PAD* pad : footprint->Pads() )
        {
            Add( pad );
            report( ++ii );
        }
    }

    for( BOARD_ITEM* drawing : aBoard->Drawings() )
    {
        if( PCB_SHAPE* shape = dynamic_cast<PCB_SHAPE*>( drawing ) )
        {
            if( shape->IsOnCopperLayer() )
                Add( shape );
        }

        report( ++ii );
    }

    if( aReporter )
    {
        aReporter->SetCurrentProgress( (double) ii / (double) size );
        aReporter->KeepRefreshing( false );
    }
}


void CN_CONNECTIVITY_ALGO::LocalBuild( const std::shared_ptr<CONNECTIVITY_DATA>& aGlobalConnectivity,
                                       const std::vector<BOARD_ITEM*>& aLocalItems )
{
    m_isLocal = true;
    m_globalConnectivityData = aGlobalConnectivity;

    for( BOARD_ITEM* item : aLocalItems )
    {
        switch( item->Type() )
        {
        case PCB_TRACE_T:
        case PCB_ARC_T:
        case PCB_VIA_T:
        case PCB_PAD_T:
        case PCB_FOOTPRINT_T:
        case PCB_SHAPE_T:
            Add( item );
            break;

        default:
            break;
        }
    }
}


void CN_CONNECTIVITY_ALGO::propagateConnections( BOARD_COMMIT* aCommit )
{
    for( const std::shared_ptr<CN_CLUSTER>& cluster : m_connClusters )
    {
        if( cluster->IsConflicting() )
        {
            // Conflicting pads in cluster: we don't know the user's intent so best to do
            // nothing.
            wxLogTrace( wxT( "CN" ), wxT( "Conflicting pads in cluster %p; skipping propagation" ),
                        cluster.get() );
        }
        else if( cluster->HasValidNet() )
        {
            // Propagate from the origin (will be a pad if there are any, or another item if
            // there are no pads).
            int n_changed = 0;

            for( CN_ITEM* item : *cluster )
            {
                if( item->Valid() && item->CanChangeNet()
                        && item->Parent()->GetNetCode() != cluster->OriginNet() )
                {
                    MarkNetAsDirty( item->Parent()->GetNetCode() );
                    MarkNetAsDirty( cluster->OriginNet() );

                    if( aCommit )
                        aCommit->Modify( item->Parent() );

                    item->Parent()->SetNetCode( cluster->OriginNet() );

                    // KICLOUD: R2-A2: an item changed without a notification (see
                    // BOARD::KicloudUnnotifiedChangeCount); counted even inside a commit, whose
                    // Push may be the router's own
                    if( BOARD* board = item->Parent()->GetBoard() )
                        board->KicloudNoteChange( true );

                    // KICLOUD: L8.3a the collab bridge captures the new net (no listener hears it
                    // when the propagation runs without a commit: undo, a connectivity rebuild)
                    if( KICLOUD_COLLAB_HOOKS::ItemChangedUnnotified )
                        KICLOUD_COLLAB_HOOKS::ItemChangedUnnotified( item->Parent(), false );

                    n_changed++;
                }
            }

            if( n_changed )
            {
                wxLogTrace( wxT( "CN" ), wxT( "Cluster %p: net: %d %s" ),
                            cluster.get(),
                            cluster->OriginNet(),
                            (const char*) cluster->OriginNetName().c_str() );
            }
            else
            {
                wxLogTrace( wxT( "CN" ), wxT( "Cluster %p: no changeable items to propagate to" ),
                            cluster.get() );
            }
        }
        else
        {
            wxLogTrace( wxT( "CN" ), wxT( "Cluster %p: connected to unused net" ),
                        cluster.get() );
        }
    }
}


void CN_CONNECTIVITY_ALGO::PropagateNets( BOARD_COMMIT* aCommit )
{
    updateJumperPads();
    m_connClusters = SearchClusters( CSM_PROPAGATE );
    propagateConnections( aCommit );
}


void CN_CONNECTIVITY_ALGO::FillIsolatedIslandsMap( std::map<ZONE*, std::map<PCB_LAYER_ID, ISOLATED_ISLANDS>>& aMap,
                                                   bool aConnectivityAlreadyRebuilt )
{
    int progressDelta = 50;
    int ii = 0;

    progressDelta = std::max( progressDelta, (int) aMap.size() / 4 );

    if( !aConnectivityAlreadyRebuilt )
    {
        for( const auto& [ zone, islands ] : aMap )
        {
            Remove( zone );
            Add( zone );
            ii++;

            if( m_progressReporter && ( ii % progressDelta ) == 0 )
            {
                m_progressReporter->SetCurrentProgress( (double) ii / (double) aMap.size() );
                m_progressReporter->KeepRefreshing( false );
            }

            if( m_progressReporter && m_progressReporter->IsCancelled() )
                return;
        }
    }

    m_connClusters = SearchClusters( CSM_CONNECTIVITY_CHECK );

    for( auto& [ zone, zoneIslands ] : aMap )
    {
        for( auto& [ layer, layerIslands ] : zoneIslands )
        {
            if( zone->GetFilledPolysList( layer )->IsEmpty() )
                continue;

            bool notInConnectivity = true;

            for( const std::shared_ptr<CN_CLUSTER>& cluster : m_connClusters )
            {
                for( CN_ITEM* item : *cluster )
                {
                    if( item->Parent() == zone && item->GetBoardLayer() == layer )
                    {
                        CN_ZONE_LAYER* z = static_cast<CN_ZONE_LAYER*>( item );
                        notInConnectivity = false;

                        if( cluster->IsOrphaned() )
                            layerIslands.m_IsolatedOutlines.push_back( z->SubpolyIndex() );
                        else if( z->HasSingleConnection() )
                            layerIslands.m_SingleConnectionOutlines.push_back( z->SubpolyIndex() );
                    }
                }
            }

            // Non-copper zones (silk, mask, etc.) are never added to the connectivity graph,
            // so notInConnectivity is always true for them.  Without the IsCopperLayer guard
            // outline 0 of every non-copper multi-island fill gets dropped on every refill
            // (issue 24089).
            if( notInConnectivity && IsCopperLayer( layer ) )
                layerIslands.m_IsolatedOutlines.push_back( 0 );
        }
    }
}


const CN_CONNECTIVITY_ALGO::CLUSTERS& CN_CONNECTIVITY_ALGO::GetClusters()
{
    m_ratsnestClusters = SearchClusters( CSM_RATSNEST );
    return m_ratsnestClusters;
}


void CN_CONNECTIVITY_ALGO::MarkNetAsDirty( int aNet )
{
    if( aNet < 0 )
        return;

    if( (int) m_dirtyNets.size() <= aNet )
    {
        int lastNet = m_dirtyNets.size() - 1;

        if( lastNet < 0 )
            lastNet = 0;

        m_dirtyNets.resize( aNet + 1 );

        for( int i = lastNet; i < aNet + 1; i++ )
            m_dirtyNets[i] = true;
    }

    m_dirtyNets[aNet] = true;
}


void CN_VISITOR::checkZoneItemConnection( CN_ZONE_LAYER* aZoneLayer, CN_ITEM* aItem )
{
    PCB_LAYER_ID          layer = aZoneLayer->GetLayer();
    BOARD_CONNECTED_ITEM* item = aItem->Parent();

    if( !item->IsOnLayer( layer ) )
        return;

    auto connect =
            [&]()
            {
                // We don't propagate nets from zones, so via-zone net changes are deferred
                // and applied only if the via has no higher-priority connections (tracks, pads).
                if( aItem->Parent()->Type() == PCB_VIA_T && aItem->CanChangeNet() )
                {
                    std::lock_guard<std::mutex> lock( *m_deferredNetCodesMutex );
                    m_deferredNetCodes->emplace_back( aItem, aZoneLayer->Net() );
                }

                aZoneLayer->Connect( aItem );
                aItem->Connect( aZoneLayer );
            };

    // Try quick checks first...
    if( item->Type() == PCB_PAD_T )
    {
        PAD* pad = static_cast<PAD*>( item );

        if( pad->ConditionallyFlashed( layer )
            && pad->GetZoneLayerOverride( layer ) == ZLO_FORCE_NO_ZONE_CONNECTION )
        {
            return;
        }

        // Don't connect zones to pads on backdrilled or post-machined layers
        if( pad->IsBackdrilledOrPostMachined( layer ) )
            return;
    }
    else if( item->Type() == PCB_VIA_T )
    {
        PCB_VIA* via = static_cast<PCB_VIA*>( item );

        if( via->ConditionallyFlashed( layer )
            && via->GetZoneLayerOverride( layer ) == ZLO_FORCE_NO_ZONE_CONNECTION )
        {
            return;
        }

        // Don't connect zones to vias on backdrilled or post-machined layers
        if( via->IsBackdrilledOrPostMachined( layer ) )
            return;
    }

    for( int i = 0; i < aItem->AnchorCount(); ++i )
    {
        if( aZoneLayer->ContainsPoint( aItem->GetAnchor( i ) ) )
        {
            connect();
            return;
        }
    }

    if( item->Type() == PCB_VIA_T || item->Type() == PCB_PAD_T )
    {
        // As long as the pad/via crosses the zone layer, check for the full effective shape
        // We check for the overlapping layers above
        if( aZoneLayer->Collide( item->GetEffectiveShape( layer, FLASHING::ALWAYS_FLASHED ).get() ) )
            connect();

        return;
    }

    if( aZoneLayer->Collide( item->GetEffectiveShape( layer ).get() ) )
        connect();
}

void CN_VISITOR::checkZoneZoneConnection( CN_ZONE_LAYER* aZoneLayerA, CN_ZONE_LAYER* aZoneLayerB )
{
    // CN_ZONE_LAYER now caches its own copy of the outline, so we just check if it's non-empty.
    if( !aZoneLayerA->HasValidOutline() || !aZoneLayerB->HasValidOutline() )
        return;

    const BOX2I& boxA = aZoneLayerA->BBox();
    const BOX2I& boxB = aZoneLayerB->BBox();

    PCB_LAYER_ID layer = aZoneLayerA->GetLayer();

    if( aZoneLayerB->GetLayer() != layer )
        return;

    if( !boxA.Intersects( boxB ) )
        return;

    const SHAPE_LINE_CHAIN& outlineA = aZoneLayerA->GetOutline();

    for( int i = 0; i < outlineA.PointCount(); i++ )
    {
        const VECTOR2I& pt = outlineA.CPoint( i );

        if( !boxB.Contains( pt ) )
            continue;

        if( aZoneLayerB->ContainsPoint( pt ) )
        {
            aZoneLayerA->Connect( aZoneLayerB );
            aZoneLayerB->Connect( aZoneLayerA );
            return;
        }
    }

    const SHAPE_LINE_CHAIN& outlineB = aZoneLayerB->GetOutline();

    for( int i = 0; i < outlineB.PointCount(); i++ )
    {
        const VECTOR2I& pt = outlineB.CPoint( i );

        if( !boxA.Contains( pt ) )
            continue;

        if( aZoneLayerA->ContainsPoint( pt ) )
        {
            aZoneLayerA->Connect( aZoneLayerB );
            aZoneLayerB->Connect( aZoneLayerA );
            return;
        }
    }
}


bool CN_VISITOR::operator()( CN_ITEM* aCandidate )
{
    const BOARD_CONNECTED_ITEM* parentA = aCandidate->Parent();
    const BOARD_CONNECTED_ITEM* parentB = m_item->Parent();

    if( !aCandidate->Valid() || !m_item->Valid() )
        return true;

    if( parentA == parentB )
        return true;

    // Don't connect items in different nets that can't be changed
    if( !aCandidate->CanChangeNet() && !m_item->CanChangeNet() && aCandidate->Net() != m_item->Net() )
        return true;

    // If both m_item and aCandidate are marked dirty, they will both be searched
    // Since we are reciprocal in our connection, we arbitrarily pick one of the connections
    // to conduct the expensive search
    if( aCandidate->Dirty() && aCandidate < m_item )
        return true;

    // We should handle zone-zone connection separately
    if ( parentA->Type() == PCB_ZONE_T && parentB->Type() == PCB_ZONE_T )
    {
        checkZoneZoneConnection( static_cast<CN_ZONE_LAYER*>( m_item ),
                                 static_cast<CN_ZONE_LAYER*>( aCandidate ) );
        return true;
    }

    if( parentA->Type() == PCB_ZONE_T )
    {
        checkZoneItemConnection( static_cast<CN_ZONE_LAYER*>( aCandidate ), m_item );
        return true;
    }

    if( parentB->Type() == PCB_ZONE_T )
    {
        checkZoneItemConnection( static_cast<CN_ZONE_LAYER*>( m_item ), aCandidate );
        return true;
    }

    LSET commonLayers = parentA->GetLayerSet() & parentB->GetLayerSet();

    if( const BOARD* board = parentA->GetBoard() )
        commonLayers &= board->GetEnabledLayers();

    for( PCB_LAYER_ID layer : commonLayers )
    {
        FLASHING flashingA = FLASHING::NEVER_FLASHED;
        FLASHING flashingB = FLASHING::NEVER_FLASHED;

        if( parentA->Type() == PCB_PAD_T )
        {
            if( !static_cast<const PAD*>( parentA )->ConditionallyFlashed( layer ) )
                flashingA = FLASHING::ALWAYS_FLASHED;
        }
        else if( parentA->Type() == PCB_VIA_T )
        {
            if( !static_cast<const PCB_VIA*>( parentA )->ConditionallyFlashed( layer ) )
                flashingA = FLASHING::ALWAYS_FLASHED;
        }

        if( parentB->Type() == PCB_PAD_T )
        {
            if( !static_cast<const PAD*>( parentB )->ConditionallyFlashed( layer ) )
                flashingB = FLASHING::ALWAYS_FLASHED;
        }
        else if( parentB->Type() == PCB_VIA_T )
        {
            if( !static_cast<const PCB_VIA*>( parentB )->ConditionallyFlashed( layer ) )
                flashingB = FLASHING::ALWAYS_FLASHED;
        }

        if( parentA->GetEffectiveShape( layer, flashingA )->Collide(
                    parentB->GetEffectiveShape( layer, flashingB ).get() ) )
        {
            m_item->Connect( aCandidate );
            aCandidate->Connect( m_item );
            return true;
        }
    }

    return true;
};


void CN_CONNECTIVITY_ALGO::Clear()
{
    m_ratsnestClusters.clear();
    m_connClusters.clear();
    m_itemMap.clear();
    m_itemList.Clear();

}

void CN_CONNECTIVITY_ALGO::SetProgressReporter( PROGRESS_REPORTER* aReporter )
{
    m_progressReporter = aReporter;
}


void CN_CONNECTIVITY_ALGO::updateJumperPads()
{
    // Map of footprint -> map of pad number -> list of CN_ITEMs for pads with that number
    std::map<FOOTPRINT*, std::map<wxString, std::vector<CN_ITEM*>>> padsByFootprint;

    for( CN_ITEM* item : m_itemList )
    {
        if( !item->Valid() || item->Parent()->Type() != PCB_PAD_T )
            continue;

        auto pad = static_cast<const PAD*>( item->Parent() );

        FOOTPRINT* fp = pad->GetParentFootprint();

        padsByFootprint[fp][ pad->GetNumber() ].emplace_back( item );
    }

    for( auto& [footprint, padsMap] : padsByFootprint )
    {
        if( footprint->GetDuplicatePadNumbersAreJumpers() )
        {
            for( const std::vector<CN_ITEM*>& padsList : padsMap | std::views::values )
            {
                for( size_t i = 0; i < padsList.size(); ++i )
                {
                    for( size_t j = 1; j < padsList.size(); ++j )
                    {
                        padsList[i]->Connect( padsList[j] );
                        padsList[j]->Connect( padsList[i] );
                    }
                }
            }
        }

        for( const std::set<wxString>& group : footprint->JumperPadGroups() )
        {
            std::vector<CN_ITEM*> toConnect;

            for( const wxString& padNumber : group )
                std::ranges::copy( padsMap[padNumber], std::back_inserter( toConnect ) );

            for( size_t i = 0; i < toConnect.size(); ++i )
            {
                for( size_t j = 1; j < toConnect.size(); ++j )
                {
                    toConnect[i]->Connect( toConnect[j] );
                    toConnect[j]->Connect( toConnect[i] );
                }
            }
        }
    }
}
