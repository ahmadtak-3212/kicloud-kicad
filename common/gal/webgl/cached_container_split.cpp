/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2026 Ahmad Taka.
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

        // New vertices replace any colour or depth waiting for the same place: those land first
        if( m_colorFills.mayOverlap( gpuOffset, count ) )
            flushFills( m_colorFills, COLOR );

        if( m_depthFills.mayOverlap( gpuOffset, count ) )
            flushFills( m_depthFills, DEPTH );

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
    m_colorFills.clear();
    m_depthFills.clear();
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
    queueFill( m_colorFills, COLOR, aOffset, gpuCount, value );
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
    queueFill( m_depthFills, DEPTH, aOffset, gpuCount, value );
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


// Upload one run of a constant value: aFill.count copies of aFill.value at vertex aFill.offset of
// aBuffer, in UPLOAD_BLOCK pieces. Called by flushFills() only when queued runs overlap (rare: the
// same item recoloured twice before an upload), where each run must land in queue order. Binds and
// unbinds GL_ARRAY_BUFFER; uses m_scratch.
void CACHED_CONTAINER_SPLIT::uploadFill( const FILL& aFill, BUFFER aBuffer )
{
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
}


// KICLOUD: F-P1 (LB2): upload every fill waiting in aQueue (the colour or the depth fills) with as
// few bufferSubData calls as possible, then empty the queue.
// Called by flushAll() (each frame's Unmap(), before a resize or a GPU copy), by FinishItem() when
// new vertices may cover a waiting fill, and by queueFill() when the queue is full.
// How: the runs are put in offset order (they usually are already: KiCad recolours the groups in
// the order they were created). Runs that touch are written one after the other into m_scratch and
// sent as one upload of up to UPLOAD_BLOCK vertices; a gap between runs starts a new upload. The
// bytes on the GPU are the same as uploading each run at once. If two runs overlap (the same
// vertices filled twice before an upload), the later one must win, so the runs are then uploaded
// one by one in queue order, as before this change.
// State: the GPU buffer aBuffer is written; the queue is empty afterwards; m_scratch is reused.
void CACHED_CONTAINER_SPLIT::flushFills( FILL_QUEUE& aQueue, BUFFER aBuffer )
{
    if( aQueue.empty() )
        return;

    ensureGpuBuffers();

    std::vector<FILL>& runs = aQueue.runs;

    if( !aQueue.sorted )
    {
        // Stable: runs that start at the same offset keep their queue order
        std::vector<FILL> ordered( runs );
        std::stable_sort( ordered.begin(), ordered.end(),
                          []( const FILL& a, const FILL& b )
                          {
                              return a.offset < b.offset;
                          } );

        bool overlap = false;

        for( size_t i = 1; i < ordered.size() && !overlap; ++i )
            overlap = ordered[i].offset < ordered[i - 1].offset + ordered[i - 1].count;

        if( overlap )
        {
            for( const FILL& run : runs )
                uploadFill( run, aBuffer );

            aQueue.clear();
            return;
        }

        runs.swap( ordered );
    }

    m_scratch.resize( (size_t) UPLOAD_BLOCK * sizeof( uint32_t ) );
    uint32_t* values = reinterpret_cast<uint32_t*>( m_scratch.data() );
    unsigned int segStart = runs.front().offset;   // vertex offset of values[0]
    unsigned int segLen = 0;                        // values written into the scratch so far

    glBindBuffer( GL_ARRAY_BUFFER, m_buffers[aBuffer] );

    // Send values[0, segLen) to vertices [segStart, segStart + segLen) of the buffer
    auto upload = [&]()
    {
        if( segLen > 0 )
        {
            glBufferSubData( GL_ARRAY_BUFFER, (GLintptr) segStart * STRIDES[aBuffer],
                             (GLsizeiptr) segLen * STRIDES[aBuffer], values );
        }

        segStart += segLen;
        segLen = 0;
    };

    for( const FILL& run : runs )
    {
        // A gap: what is collected goes up, the next upload starts at this run
        if( run.offset != segStart + segLen )
        {
            upload();
            segStart = run.offset;
        }

        unsigned int left = run.count;

        while( left > 0 )
        {
            unsigned int n = std::min( left, UPLOAD_BLOCK - segLen );
            std::fill( values + segLen, values + segLen + n, run.value );
            segLen += n;
            left -= n;

            if( segLen == UPLOAD_BLOCK )
                upload();
        }
    }

    upload();
    glBindBuffer( GL_ARRAY_BUFFER, 0 );
    aQueue.clear();
}


void CACHED_CONTAINER_SPLIT::flushAll()
{
    flushVertices();
    flushFills( m_colorFills, COLOR );
    flushFills( m_depthFills, DEPTH );
}


// Queue a constant fill of aCount vertices at aOffset of aBuffer's queue aQueue with aValue (an
// RGBA colour or a float depth as raw bits). Called by SetItemColor() and SetItemDepth().
// Waiting vertices of the same place are uploaded first (they were written earlier). A run that
// continues the last queued one with the same value only extends it; otherwise a new run is added
// (KICLOUD: F-P1: runs are no longer uploaded one at a time here; flushFills() merges them).
// State: aQueue gains or extends a run and its bounds/sortedness are updated; a full queue is
// uploaded at once.
void CACHED_CONTAINER_SPLIT::queueFill( FILL_QUEUE& aQueue, BUFFER aBuffer, unsigned int aOffset,
                                        unsigned int aCount, uint32_t aValue )
{
    if( aCount == 0 )
        return;

    // Waiting vertices for the same place must land first
    if( overlapsPending( aOffset, aCount ) )
        flushVertices();

    if( !aQueue.runs.empty() )
    {
        FILL& last = aQueue.runs.back();

        if( last.value == aValue && last.offset + last.count == aOffset )
        {
            last.count += aCount;
            aQueue.end = std::max( aQueue.end, aOffset + aCount );
            return;
        }
    }

    // A full queue goes up first; it is empty (and in order) afterwards
    if( aQueue.runs.size() >= MAX_FILL_RUNS )
        flushFills( aQueue, aBuffer );

    // A run that starts before the end of the last one breaks the ascending order
    if( !aQueue.runs.empty() && aOffset < aQueue.runs.back().offset + aQueue.runs.back().count )
        aQueue.sorted = false;

    aQueue.runs.push_back( { aOffset, aCount, aValue } );
    aQueue.begin = std::min( aQueue.begin, aOffset );
    aQueue.end = std::max( aQueue.end, aOffset + aCount );
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
