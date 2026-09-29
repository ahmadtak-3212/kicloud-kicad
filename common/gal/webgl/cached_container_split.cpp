/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2026 kicloud contributors.
 *
 * KICLOUD: original file for the browser WebGL GAL (docs/patches.md, B1.6c).
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

#include <gal/webgl/cached_container_split.h>
#include <gal/webgl/vertex_item.h>
#include <gal/webgl/utils.h>

#include <algorithm>
#include <cassert>
#include <climits>
#include <cstddef>
#include <cstring>

#include <wx/log.h>

using namespace KIGFX;


/**
 * Flag to enable debug output of the GAL WebGL cached container.
 *
 * Use "KICAD_GAL_CACHED_CONTAINER" to enable GAL WebGL cached container tracing.
 */
static const wxChar* const traceGalCachedContainer = wxT( "KICAD_GAL_CACHED_CONTAINER" );


/// The built item's staged vertices are kept; above this capacity an emptied stage is released.
static constexpr size_t STAGE_KEEP_VERTICES = 1024 * 1024;


CACHED_CONTAINER_SPLIT::CACHED_CONTAINER_SPLIT( unsigned int aSize ) :
        CACHED_CONTAINER( aSize ),
        m_gpuSize( 0 ),
        m_pendingBegin( UINT_MAX ),
        m_pendingEnd( 0 ),
        m_itemStart( 0 ),
        m_itemBase( 0 )
{
    glGenBuffers( BUFFER_COUNT, m_buffers );
    checkGlError( "generating vertex buffers", __FILE__, __LINE__ );
}


CACHED_CONTAINER_SPLIT::~CACHED_CONTAINER_SPLIT()
{
    if( glDeleteBuffers )
        glDeleteBuffers( BUFFER_COUNT, m_buffers );
}


void CACHED_CONTAINER_SPLIT::SetItem( VERTEX_ITEM* aItem )
{
    CACHED_CONTAINER::SetItem( aItem );

    m_itemBase = aItem->GetSize();
    m_itemStart = m_stage.size();
}


VERTEX* CACHED_CONTAINER_SPLIT::Allocate( unsigned int aSize )
{
    assert( m_item != nullptr );

    if( m_failed )
        return nullptr;

    unsigned int itemSize = m_item->GetSize();
    unsigned int newSize = itemSize + aSize;

    // reallocate() may compact the container, which moves the staged vertices (flushVertices())
    if( newSize > m_chunkSize && !reallocate( newSize ) )
    {
        m_failed = true;
        return nullptr;
    }

    m_stage.resize( m_itemStart + ( newSize - m_itemBase ) );
    m_item->setSize( newSize );
    m_dirty = true;

    return &m_stage[m_itemStart + ( itemSize - m_itemBase )];
}


void CACHED_CONTAINER_SPLIT::FinishItem()
{
    assert( m_item != nullptr );

    unsigned int itemSize = m_item->GetSize();

    if( itemSize > m_itemBase )
    {
        unsigned int gpuOffset = m_item->GetOffset() + m_itemBase;
        unsigned int count = itemSize - m_itemBase;

        // New vertices replace any colour or depth waiting for the same place
        if( overlaps( m_colorFill, gpuOffset, count ) )
            flushFill( m_colorFill, COLOR );

        if( overlaps( m_depthFill, gpuOffset, count ) )
            flushFill( m_depthFill, DEPTH );

        PENDING* last = m_pending.empty() ? nullptr : &m_pending.back();

        if( last && last->gpuOffset + last->count == gpuOffset
            && last->stageOffset + last->count == m_itemStart )
        {
            last->count += count;
        }
        else
        {
            m_pending.push_back( { gpuOffset, m_itemStart, count } );
        }

        m_pendingBegin = std::min( m_pendingBegin, gpuOffset );
        m_pendingEnd = std::max( m_pendingEnd, gpuOffset + count );
    }

    CACHED_CONTAINER::FinishItem();

    m_itemBase = 0;
    m_itemStart = m_stage.size();

    if( m_stage.size() * sizeof( VERTEX ) >= MAX_PENDING_BYTES )
        flushVertices();
}


void CACHED_CONTAINER_SPLIT::Clear()
{
    CACHED_CONTAINER::Clear();

    m_stage.clear();

    if( m_stage.capacity() > STAGE_KEEP_VERTICES )
        m_stage.shrink_to_fit();

    m_pending.clear();
    m_pendingBegin = UINT_MAX;
    m_pendingEnd = 0;
    m_itemStart = 0;
    m_itemBase = 0;
    m_colorFill.count = 0;
    m_depthFill.count = 0;
}


void CACHED_CONTAINER_SPLIT::Unmap()
{
    flushAll();
}


void CACHED_CONTAINER_SPLIT::SetItemColor( unsigned int aOffset, unsigned int aSize,
                                           const GLubyte aColor[4] )
{
    unsigned int gpuCount = aSize;

    // The item being built: its staged vertices are changed in place
    if( m_item && aSize > 0 && aOffset == m_item->GetOffset() )
    {
        for( size_t i = m_itemStart; i < m_stage.size(); ++i )
        {
            m_stage[i].r = aColor[0];
            m_stage[i].g = aColor[1];
            m_stage[i].b = aColor[2];
            m_stage[i].a = aColor[3];
        }

        gpuCount = m_itemBase;
    }

    uint32_t value;
    memcpy( &value, aColor, sizeof( value ) );
    queueFill( m_colorFill, COLOR, aOffset, gpuCount, value );
}


void CACHED_CONTAINER_SPLIT::SetItemDepth( unsigned int aOffset, unsigned int aSize,
                                           GLfloat aDepth )
{
    unsigned int gpuCount = aSize;

    if( m_item && aSize > 0 && aOffset == m_item->GetOffset() )
    {
        for( size_t i = m_itemStart; i < m_stage.size(); ++i )
            m_stage[i].z = aDepth;

        gpuCount = m_itemBase;
    }

    uint32_t value;
    memcpy( &value, &aDepth, sizeof( value ) );
    queueFill( m_depthFill, DEPTH, aOffset, gpuCount, value );
}


void CACHED_CONTAINER_SPLIT::BindAttributes( int aVertexAttrib, int aDepthAttrib,
                                             int aColorAttrib, int aShaderAttrib )
{
    glBindBuffer( GL_ARRAY_BUFFER, m_buffers[POSITION] );
    glEnableVertexAttribArray( aVertexAttrib );
    glVertexAttribPointer( aVertexAttrib, 2, GL_FLOAT, GL_FALSE, STRIDES[POSITION],
                           (GLvoid*) offsetof( POSITION_DATA, x ) );

    if( aShaderAttrib >= 0 )
    {
        glEnableVertexAttribArray( aShaderAttrib );
        glVertexAttribPointer( aShaderAttrib, SHADER_STRIDE, GL_FLOAT, GL_FALSE,
                               STRIDES[POSITION], (GLvoid*) offsetof( POSITION_DATA, shader ) );
    }

    if( aDepthAttrib >= 0 )
    {
        glBindBuffer( GL_ARRAY_BUFFER, m_buffers[DEPTH] );
        glEnableVertexAttribArray( aDepthAttrib );
        glVertexAttribPointer( aDepthAttrib, 1, GL_FLOAT, GL_FALSE, STRIDES[DEPTH], (GLvoid*) 0 );
    }

    glBindBuffer( GL_ARRAY_BUFFER, m_buffers[COLOR] );
    glEnableVertexAttribArray( aColorAttrib );
    glVertexAttribPointer( aColorAttrib, COLOR_STRIDE, GL_UNSIGNED_BYTE, GL_TRUE, STRIDES[COLOR],
                           (GLvoid*) 0 );
}


bool CACHED_CONTAINER_SPLIT::defragmentResize( unsigned int aNewSize )
{
    wxLogTrace( traceGalCachedContainer,
                wxT( "Resizing & defragmenting container (GPU copy) from %d to %d" ),
                m_currentSize, aNewSize );

    // No shrinking if we cannot fit all the data
    if( usedSpace() > aNewSize )
        return false;

    flushAll();

    GLuint newBuffers[BUFFER_COUNT];
    glGenBuffers( BUFFER_COUNT, newBuffers );

    // Report an allocation failure as a failed resize, like the RAM container's bad_alloc
    for( int i = 0; i < 8 && glGetError() != GL_NO_ERROR; ++i )
        ;

    for( int i = 0; i < BUFFER_COUNT; ++i )
    {
        glBindBuffer( GL_ARRAY_BUFFER, newBuffers[i] );
        glBufferData( GL_ARRAY_BUFFER, (GLsizeiptr) aNewSize * STRIDES[i], nullptr,
                      GL_DYNAMIC_DRAW );
    }

    glBindBuffer( GL_ARRAY_BUFFER, 0 );

    if( glGetError() != GL_NO_ERROR )
    {
        glDeleteBuffers( BUFFER_COUNT, newBuffers );
        return false;
    }

    // Items in offset order: neighbours stay neighbours and are copied in one call
    std::vector<VERTEX_ITEM*> items;
    items.reserve( m_items.size() );

    for( VERTEX_ITEM* item : m_items )
    {
        if( item != m_item && item->GetSize() > 0 )
            items.push_back( item );
    }

    std::sort( items.begin(), items.end(),
               []( const VERTEX_ITEM* a, const VERTEX_ITEM* b )
               {
                   return a->GetOffset() < b->GetOffset();
               } );

    bool         onGpu = m_gpuSize > 0;
    unsigned int newOffset = 0;
    unsigned int runFrom = 0;
    unsigned int runTo = 0;
    unsigned int runCount = 0;

    for( VERTEX_ITEM* item : items )
    {
        if( runCount > 0 && item->GetOffset() != runFrom + runCount )
        {
            if( onGpu )
                copyVertices( m_buffers, runFrom, newBuffers, runTo, runCount );

            runCount = 0;
        }

        if( runCount == 0 )
        {
            runFrom = item->GetOffset();
            runTo = newOffset;
        }

        runCount += item->GetSize();
        item->setOffset( newOffset );
        newOffset += item->GetSize();
    }

    if( runCount > 0 && onGpu )
        copyVertices( m_buffers, runFrom, newBuffers, runTo, runCount );

    // The item being built goes last; only the vertices it had before SetItem() are on the GPU
    if( m_item && m_item->GetSize() > 0 )
    {
        if( m_itemBase > 0 && onGpu )
            copyVertices( m_buffers, m_item->GetOffset(), newBuffers, newOffset, m_itemBase );

        m_item->setOffset( newOffset );
        m_chunkOffset = newOffset;
    }

    glDeleteBuffers( BUFFER_COUNT, m_buffers );
    std::copy( newBuffers, newBuffers + BUFFER_COUNT, m_buffers );
    m_gpuSize = aNewSize;

    m_freeSpace += ( aNewSize - m_currentSize );
    m_currentSize = aNewSize;
    m_maxIndex = usedSpace();

    // Now there is only one big chunk of free memory
    m_freeChunks.clear();
    m_freeChunks.insert( std::make_pair( m_freeSpace, m_currentSize - m_freeSpace ) );

    return true;
}


void CACHED_CONTAINER_SPLIT::moveItemData( unsigned int aFrom, unsigned int aTo,
                                           unsigned int aCount )
{
    // Called by reallocate() for the item being built. Its new vertices are staged and do not
    // move; only those it had before SetItem() are copied on the GPU.
    unsigned int onGpu = std::min( aCount, m_itemBase );

    if( onGpu == 0 )
        return;

    flushAll();
    copyVertices( m_buffers, aFrom, m_buffers, aTo, onGpu );
}


void CACHED_CONTAINER_SPLIT::ensureGpuBuffers()
{
    if( m_gpuSize == m_currentSize )
        return;

    // Only the first upload gets here; defragmentResize() resizes allocated buffers
    assert( m_gpuSize == 0 );

    for( int i = 0; i < BUFFER_COUNT; ++i )
    {
        glBindBuffer( GL_ARRAY_BUFFER, m_buffers[i] );
        glBufferData( GL_ARRAY_BUFFER, (GLsizeiptr) m_currentSize * STRIDES[i], nullptr,
                      GL_DYNAMIC_DRAW );
    }

    glBindBuffer( GL_ARRAY_BUFFER, 0 );
    checkGlError( "allocating vertex buffers", __FILE__, __LINE__ );
    m_gpuSize = m_currentSize;
}


void CACHED_CONTAINER_SPLIT::flushVertices()
{
    if( !m_pending.empty() )
    {
        ensureGpuBuffers();

        for( const PENDING& pending : m_pending )
            uploadVertices( pending.gpuOffset, &m_stage[pending.stageOffset], pending.count );

        m_pending.clear();
        m_pendingBegin = UINT_MAX;
        m_pendingEnd = 0;
    }

    // Keep the staged vertices of the item being built, at the start of the stage
    if( m_itemStart > 0 )
    {
        m_stage.erase( m_stage.begin(), m_stage.begin() + m_itemStart );
        m_itemStart = 0;
    }

    if( m_stage.capacity() > STAGE_KEEP_VERTICES && m_stage.size() < STAGE_KEEP_VERTICES / 4 )
        m_stage.shrink_to_fit();
}


void CACHED_CONTAINER_SPLIT::flushFill( FILL& aFill, BUFFER aBuffer )
{
    if( aFill.count == 0 )
        return;

    ensureGpuBuffers();

    unsigned int block = std::min( aFill.count, UPLOAD_BLOCK );
    m_scratch.resize( (size_t) block * sizeof( uint32_t ) );
    uint32_t* values = reinterpret_cast<uint32_t*>( m_scratch.data() );
    std::fill( values, values + block, aFill.value );

    glBindBuffer( GL_ARRAY_BUFFER, m_buffers[aBuffer] );

    for( unsigned int done = 0; done < aFill.count; done += block )
    {
        unsigned int n = std::min( block, aFill.count - done );
        glBufferSubData( GL_ARRAY_BUFFER, (GLintptr) ( aFill.offset + done ) * STRIDES[aBuffer],
                         (GLsizeiptr) n * STRIDES[aBuffer], values );
    }

    glBindBuffer( GL_ARRAY_BUFFER, 0 );
    aFill.count = 0;
}


void CACHED_CONTAINER_SPLIT::flushAll()
{
    flushVertices();
    flushFill( m_colorFill, COLOR );
    flushFill( m_depthFill, DEPTH );
}


void CACHED_CONTAINER_SPLIT::queueFill( FILL& aFill, BUFFER aBuffer, unsigned int aOffset,
                                        unsigned int aCount, uint32_t aValue )
{
    if( aCount == 0 )
        return;

    // Waiting vertices for the same place must land first
    if( overlapsPending( aOffset, aCount ) )
        flushVertices();

    if( aFill.count > 0 && aFill.value == aValue && aFill.offset + aFill.count == aOffset )
    {
        aFill.count += aCount;
        return;
    }

    flushFill( aFill, aBuffer );
    aFill.offset = aOffset;
    aFill.count = aCount;
    aFill.value = aValue;
}


void CACHED_CONTAINER_SPLIT::uploadVertices( unsigned int aGpuOffset, const VERTEX* aVertices,
                                             unsigned int aCount )
{
    unsigned int block = std::min( aCount, UPLOAD_BLOCK );
    m_scratch.resize( (size_t) block * ( STRIDES[POSITION] + STRIDES[DEPTH] + STRIDES[COLOR] ) );

    POSITION_DATA* positions = reinterpret_cast<POSITION_DATA*>( m_scratch.data() );
    GLfloat*       depths = reinterpret_cast<GLfloat*>( positions + block );
    GLubyte*       colors = reinterpret_cast<GLubyte*>( depths + block );

    for( unsigned int done = 0; done < aCount; done += block )
    {
        unsigned int n = std::min( block, aCount - done );

        for( unsigned int i = 0; i < n; ++i )
        {
            const VERTEX& v = aVertices[done + i];
            positions[i].x = v.x;
            positions[i].y = v.y;
            memcpy( positions[i].shader, v.shader, sizeof( v.shader ) );
            depths[i] = v.z;
            colors[4 * i] = v.r;
            colors[4 * i + 1] = v.g;
            colors[4 * i + 2] = v.b;
            colors[4 * i + 3] = v.a;
        }

        const void* data[BUFFER_COUNT] = { positions, depths, colors };

        for( int b = 0; b < BUFFER_COUNT; ++b )
        {
            glBindBuffer( GL_ARRAY_BUFFER, m_buffers[b] );
            glBufferSubData( GL_ARRAY_BUFFER, (GLintptr) ( aGpuOffset + done ) * STRIDES[b],
                             (GLsizeiptr) n * STRIDES[b], data[b] );
        }
    }

    glBindBuffer( GL_ARRAY_BUFFER, 0 );
}


void CACHED_CONTAINER_SPLIT::copyVertices( const GLuint aFrom[BUFFER_COUNT],
                                           unsigned int aFromOffset,
                                           const GLuint aTo[BUFFER_COUNT], unsigned int aToOffset,
                                           unsigned int aCount )
{
    for( int b = 0; b < BUFFER_COUNT; ++b )
    {
        glBindBuffer( GL_COPY_READ_BUFFER, aFrom[b] );
        glBindBuffer( GL_COPY_WRITE_BUFFER, aTo[b] );
        glCopyBufferSubData( GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER,
                             (GLintptr) aFromOffset * STRIDES[b], (GLintptr) aToOffset * STRIDES[b],
                             (GLsizeiptr) aCount * STRIDES[b] );
    }

    glBindBuffer( GL_COPY_READ_BUFFER, 0 );
    glBindBuffer( GL_COPY_WRITE_BUFFER, 0 );
}
