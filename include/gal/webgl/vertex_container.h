/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright 2013-2017 CERN
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * @author Maciej Suminski <maciej.suminski@cern.ch>
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

/**
 * @file vertex_container.h
 * Class to store vertices and handle transfers between system memory and GPU memory.
 */

#ifndef VERTEX_CONTAINER_H_
#define VERTEX_CONTAINER_H_

#include <gal/webgl/vertex_common.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace KIGFX
{
class VERTEX_ITEM;
class SHADER;

class VERTEX_CONTAINER
{
public:
    /**
     * Return a pointer to a new container of an appropriate type.
     */
    static VERTEX_CONTAINER* MakeContainer( bool aCached );

    virtual ~VERTEX_CONTAINER();

    /**
     * Return true if the container caches vertex data in RAM or video memory.
     * Otherwise it is a single batch draw which is later discarded.
     */
    virtual bool IsCached() const = 0;

    /**
     * Prepare the container for vertices updates.
     */
    virtual void Map() {}

    /**
     * Finish the vertices updates stage.
     */
    virtual void Unmap() {}

    /**
     * Set the item for the further actions.
     *
     * @param aItem is the item or NULL in case of finishing the item.
     */
    virtual void SetItem( VERTEX_ITEM* aItem ) = 0;

    /**
     * Clean up after adding an item.
     */
    virtual void FinishItem() {};

    /**
     * Return allocated space for the requested number of vertices associated with the
     * current item (set with SetItem()).
     *
     * The allocated space is added at the end of the chunk used by the current item and
     * may serve to store new vertices.
     *
     * @param aSize is the number of vertices to be allocated.
     * @return Pointer to the allocated space or NULL in case of failure.
     */
    virtual VERTEX* Allocate( unsigned int aSize ) = 0;

    /**
     * Erase the data related to an item.
     *
     * @param aItem is the item to be erased.
     */
    virtual void Delete( VERTEX_ITEM* aItem ) = 0;

    /**
     * Remove all data stored in the container and restores its original state.
     */
    virtual void Clear() = 0;

    /**
     * Return pointer to the vertices stored in the container.
     */
    VERTEX* GetAllVertices() const
    {
        return m_vertices;
    }

    /**
     * Return vertices stored at the specific offset.
     *
     * @param aOffset is the offset.
     */
    virtual VERTEX* GetVertices( unsigned int aOffset ) const
    {
        return &m_vertices[aOffset];
    }

    /**
     * KICLOUD: set the colour of the vertices [aOffset, aOffset + aSize). A container without a
     * CPU copy of its vertices writes them on the GPU (docs/patches.md, B1.6c).
     */
    virtual void SetItemColor( unsigned int aOffset, unsigned int aSize,
                               const GLubyte aColor[4] );

    /**
     * KICLOUD: set the depth of the vertices [aOffset, aOffset + aSize) (docs/patches.md, B1.6c).
     */
    virtual void SetItemDepth( unsigned int aOffset, unsigned int aSize, GLfloat aDepth );

    /**
     * Return amount of vertices currently stored in the container.
     */
    virtual unsigned int GetSize() const
    {
        return m_currentSize;
    }

    /**
     * Return information about the container cache state.
     *
     * @return True in case the vertices have to be reuploaded.
     */
    bool IsDirty() const
    {
        return m_dirty;
    }

    /**
     * Set the dirty flag, so vertices in the container are going to be reuploaded to the GPU on
     * the next frame.
     */
    void SetDirty()
    {
        m_dirty = true;
        m_dirtyAll = true;
    }

    // KICLOUD: record which vertices changed, so a cached container can upload only those
    // ranges instead of the whole buffer (a full re-upload per change made zooming a large board
    // push gigabytes through WebGL). See docs/patches.md (B1.6).
    void SetDirty( unsigned int aOffset, unsigned int aSize )
    {
        m_dirty = true;

        if( m_dirtyAll || aSize == 0 )
            return;

        // An item growing through successive Allocate() calls extends its own last range.
        if( !m_dirtyRanges.empty() && m_dirtyRanges.back().first == aOffset )
        {
            m_dirtyRanges.back().second = std::max( m_dirtyRanges.back().second,
                                                    aOffset + aSize );
            return;
        }

        m_dirtyRanges.emplace_back( aOffset, aOffset + aSize );

        if( m_dirtyRanges.size() >= MAX_DIRTY_RANGES )
            compactDirtyRanges();
    }

    /**
     * Clear the dirty flag to prevent reuploading vertices to the GPU memory.
     */
    void ClearDirty()
    {
        m_dirty = false;
        m_dirtyAll = false;
        m_dirtyRanges.clear();
    }

protected:
    VERTEX_CONTAINER( unsigned int aSize = DEFAULT_SIZE );

    /**
     * Return size of the used memory space.
     *
     * @return Size of the used memory space (expressed as a number of vertices).
     */
    unsigned int usedSpace() const
    {
        return m_currentSize - m_freeSpace;
    }

    ///< KICLOUD: sort the recorded dirty ranges and merge overlapping or adjacent ones in place.
    void compactDirtyRanges()
    {
        if( m_dirtyRanges.size() < 2 )
            return;

        std::sort( m_dirtyRanges.begin(), m_dirtyRanges.end() );

        size_t out = 0;

        for( size_t i = 1; i < m_dirtyRanges.size(); ++i )
        {
            if( m_dirtyRanges[i].first <= m_dirtyRanges[out].second )
                m_dirtyRanges[out].second = std::max( m_dirtyRanges[out].second,
                                                      m_dirtyRanges[i].second );
            else
                m_dirtyRanges[++out] = m_dirtyRanges[i];
        }

        m_dirtyRanges.resize( out + 1 );
    }

    ///< Free space left in the container, expressed in vertices
    unsigned int    m_freeSpace;

    ///< Current container size, expressed in vertices
    unsigned int    m_currentSize;

    ///< Store the initial size, so it can be resized to this on Clear()
    unsigned int    m_initialSize;

    ///< Actual storage memory
    VERTEX*         m_vertices;

    // Status flags
    bool            m_failed;
    bool            m_dirty;

    ///< KICLOUD: every vertex must be uploaded (set by SetDirty() without a range)
    bool            m_dirtyAll;

    ///< KICLOUD: changed vertex ranges [begin, end) since the last upload, unless m_dirtyAll
    std::vector<std::pair<unsigned int, unsigned int>> m_dirtyRanges;

    ///< KICLOUD: compact the dirty ranges once this many have been recorded
    static constexpr size_t MAX_DIRTY_RANGES = 65536;

    ///< Default initial size of a container (expressed in vertices)
    static constexpr unsigned int DEFAULT_SIZE = 1048576;
};
} // namespace KIGFX

#endif /* VERTEX_CONTAINER_H_ */
