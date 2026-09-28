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

#include <gal/webgl/cached_container_ram.h>
#include <gal/webgl/vertex_manager.h>
#include <gal/webgl/vertex_item.h>
#include <gal/webgl/shader.h>
#include <gal/webgl/utils.h>

#include <confirm.h>
#include <algorithm>
#include <list>
#include <cassert>

#include <wx/log.h>
#ifdef KICAD_GAL_PROFILE
#include <core/profile.h>
#endif /* KICAD_GAL_PROFILE */

using namespace KIGFX;


/**
 * Flag to enable debug output of the GAL OpenGL cached container.
 *
 * Use "KICAD_GAL_CACHED_CONTAINER" to enable GAL OpenGL cached container tracing.
 *
 * @ingroup trace_env_vars
 */
static const wxChar* const traceGalCachedContainer = wxT( "KICAD_GAL_CACHED_CONTAINER" );


CACHED_CONTAINER_RAM::CACHED_CONTAINER_RAM( unsigned int aSize ) :
        CACHED_CONTAINER( aSize ),
        m_verticesBuffer( 0 ),
        m_gpuSize( 0 )
{
    glGenBuffers( 1, &m_verticesBuffer );
    checkGlError( "generating vertices buffer", __FILE__, __LINE__ );

    m_vertices = static_cast<VERTEX*>( malloc( aSize * VERTEX_SIZE ) );

    if( !m_vertices )
        throw std::bad_alloc();
}


CACHED_CONTAINER_RAM::~CACHED_CONTAINER_RAM()
{
    if( glDeleteBuffers )
        glDeleteBuffers( 1, &m_verticesBuffer );

    free( m_vertices );
}


void CACHED_CONTAINER_RAM::Unmap()
{
    if( !m_dirty )
        return;

    // KICLOUD: upload only the vertices that changed. PCBJam's port re-sent every vertex
    // (glBufferData of m_maxIndex vertices) on each change: zooming a large board uploaded
    // gigabytes. The GPU buffer now has the container's capacity, so it is reallocated only when
    // the container grows, and changed ranges are written in place with glBufferSubData.
    // See docs/patches.md (B1.6).
    glBindBuffer( GL_ARRAY_BUFFER, m_verticesBuffer );
    checkGlError( "binding vertices buffer", __FILE__, __LINE__ );

    if( m_gpuSize != m_currentSize )
    {
        glBufferData( GL_ARRAY_BUFFER, (GLsizeiptr) m_currentSize * VERTEX_SIZE, nullptr,
                      GL_DYNAMIC_DRAW );
        checkGlError( "allocating vertices buffer", __FILE__, __LINE__ );
        m_gpuSize = m_currentSize;
        m_dirtyAll = true;
    }

    if( m_dirtyAll )
    {
        // Everything that can be drawn: up to the end of the highest stored item, including
        // the item being edited (items can sit anywhere in a fragmented container).
        unsigned int end = m_maxIndex;

        for( const VERTEX_ITEM* item : m_items )
            end = std::max( end, item->GetOffset() + item->GetSize() );

        if( m_item )
            end = std::max( end, m_chunkOffset + m_chunkSize );

        end = std::min( end, m_currentSize );

        if( end > 0 )
            glBufferSubData( GL_ARRAY_BUFFER, 0, (GLsizeiptr) end * VERTEX_SIZE, m_vertices );
    }
    else
    {
        compactDirtyRanges();

        for( const std::pair<unsigned int, unsigned int>& range : m_dirtyRanges )
        {
            unsigned int begin = std::min( range.first, m_currentSize );
            unsigned int end = std::min( range.second, m_currentSize );

            if( end > begin )
            {
                glBufferSubData( GL_ARRAY_BUFFER, (GLintptr) begin * VERTEX_SIZE,
                                 (GLsizeiptr) ( end - begin ) * VERTEX_SIZE, &m_vertices[begin] );
            }
        }
    }

    checkGlError( "transferring vertices", __FILE__, __LINE__ );
    glBindBuffer( GL_ARRAY_BUFFER, 0 );
    checkGlError( "unbinding vertices buffer", __FILE__, __LINE__ );

    m_dirtyAll = false;
    m_dirtyRanges.clear();
}


bool CACHED_CONTAINER_RAM::defragmentResize( unsigned int aNewSize )
{
    wxLogTrace( traceGalCachedContainer,
                wxT( "Resizing & defragmenting container (memcpy) from %d to %d" ), m_currentSize,
                aNewSize );

    // No shrinking if we cannot fit all the data
    if( usedSpace() > aNewSize )
        return false;

#ifdef KICAD_GAL_PROFILE
    PROF_TIMER totalTime;
#endif /* KICAD_GAL_PROFILE */

    VERTEX* newBufferMem = static_cast<VERTEX*>( malloc( aNewSize * VERTEX_SIZE ) );

    if( !newBufferMem )
        throw std::bad_alloc();

    defragment( newBufferMem );

    // Switch to the new vertex buffer
    free( m_vertices );
    m_vertices = newBufferMem;

#ifdef KICAD_GAL_PROFILE
    totalTime.Stop();

    wxLogTrace( traceGalCachedContainer, "Defragmented container storing %d vertices / %.1f ms",
                m_currentSize - m_freeSpace, totalTime.msecs() );
#endif /* KICAD_GAL_PROFILE */

    m_freeSpace += ( aNewSize - m_currentSize );
    m_currentSize = aNewSize;

    // Now there is only one big chunk of free memory
    m_freeChunks.clear();
    m_freeChunks.insert( std::make_pair( m_freeSpace, m_currentSize - m_freeSpace ) );

    // KICLOUD: every item moved, so the whole buffer is re-sent (docs/patches.md, B1.6)
    SetDirty();

    return true;
}
