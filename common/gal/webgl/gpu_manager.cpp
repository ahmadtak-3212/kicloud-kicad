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

#include <gal/webgl/gpu_manager.h>
#include <gal/webgl/cached_container_gpu.h>
#include <gal/webgl/cached_container_ram.h>
#include <gal/webgl/noncached_container.h>
#include <gal/webgl/shader.h>
#include <gal/webgl/utils.h>
#include <gal/webgl/vertex_item.h>

#include <core/profile.h>

#include <typeinfo>
#include <confirm.h>
#include <trace_helpers.h>

#include <emscripten/html5_webgl.h>
#include <webgl/webgl1_ext.h>

#ifdef KICAD_GAL_PROFILE
#include <core/profile.h>
#include <wx/log.h>
#endif /* KICAD_GAL_PROFILE */

using namespace KIGFX;

GPU_MANAGER* GPU_MANAGER::MakeManager( VERTEX_CONTAINER* aContainer )
{
    if( aContainer->IsCached() )
        return new GPU_CACHED_MANAGER( aContainer );
    else
        return new GPU_NONCACHED_MANAGER( aContainer );
}


GPU_MANAGER::GPU_MANAGER( VERTEX_CONTAINER* aContainer ) :
        m_isDrawing( false ),
        m_container( aContainer ),
        m_shader( nullptr ),
        m_shaderAttrib( 0 ),
        m_vertexAttrib( 0 ),
        m_colorAttrib( 0 ),
        m_depthAttrib( -1 ),
        m_enableDepthTest( true ),
        m_vao( 0 )
{
}


GPU_MANAGER::~GPU_MANAGER()
{
    // Delete VAO if it was created
    if( m_vao != 0 )
    {
        glDeleteVertexArrays( 1, &m_vao );
        m_vao = 0;
    }
}


void GPU_MANAGER::SetShader( SHADER& aShader )
{
    m_shader = &aShader;
    m_shaderAttrib = m_shader->GetAttribute( "a_shaderParams" );
    m_vertexAttrib = m_shader->GetAttribute( "a_vertex" );
    m_colorAttrib = m_shader->GetAttribute( "a_color" );
    // KICLOUD: the vertex position is split into a_vertex (x, y) and a_depth (z), so a cached
    // container can keep depth in its own buffer (convert_glsl_es3.py, docs/patches.md B1.6c)
    m_depthAttrib = m_shader->GetAttribute( "a_depth" );

    if( m_shaderAttrib == -1 )
    {
        DisplayError( nullptr, wxT( "Could not get the shader attribute location" ) );
    }

    // Create VAO for WebGL 2.0 / OpenGL ES 3.0 compatibility
    // WebGL 2.0 requires a VAO to be bound before calling glVertexAttribPointer
    if( m_vao == 0 )
    {
        glGenVertexArrays( 1, &m_vao );
    }
}


// Cached manager
GPU_CACHED_MANAGER::GPU_CACHED_MANAGER( VERTEX_CONTAINER* aContainer ) :
        GPU_MANAGER( aContainer ),
        m_buffersInitialized( false ),
        m_indicesCapacity( 0 ),
        m_totalHuge( 0 ),
        m_totalNormal( 0 ),
        m_indexBufSize( 0 ),
        m_indexBufMaxSize( 0 ),
        m_curVrangeSize( 0 ),
        m_ebo( 0 ),
        m_multiDraw( -1 ),
        m_planIndexCount( 0 ),
        m_eboCapacity( 0 ),
        m_planValid( false )
{
}


GPU_CACHED_MANAGER::~GPU_CACHED_MANAGER()
{
    if( m_ebo )
    {
        glDeleteBuffers( 1, &m_ebo );
        m_ebo = 0;
    }
}


void GPU_CACHED_MANAGER::BeginDrawing()
{
    wxASSERT( !m_isDrawing );

    m_curVrangeSize = 0;
    m_indexBufMaxSize = 0;
    m_indexBufSize = 0;
    m_vranges.clear();

    m_isDrawing = true;
}


void GPU_CACHED_MANAGER::DrawIndices( const VERTEX_ITEM* aItem )
{
    // Hot path: don't use wxASSERT
    assert( m_isDrawing );

    unsigned int offset = aItem->GetOffset();
    unsigned int size = aItem->GetSize();

    if( size == 0 )
        return;

    if( size <= 1000 )
    {
        m_totalNormal += size;
        m_vranges.emplace_back( offset, offset + size - 1, false );
        m_curVrangeSize += size;
    }
    else
    {
        m_totalHuge += size;
        m_vranges.emplace_back( offset, offset + size - 1, true );
        m_indexBufSize = std::max( m_curVrangeSize, m_indexBufSize );
        m_curVrangeSize = 0;
    }
}


void GPU_CACHED_MANAGER::EndDrawing()
{
    wxASSERT( m_isDrawing );

    CACHED_CONTAINER* cached = static_cast<CACHED_CONTAINER*>( m_container );

    if( cached->IsMapped() )
        cached->Unmap();

    m_indexBufSize = std::max( m_curVrangeSize, m_indexBufSize );
    m_indexBufMaxSize = std::max( 2*m_indexBufSize, m_indexBufMaxSize );

    // KICLOUD: probe WEBGL_multi_draw once; with it the CPU index buffer is not needed.
    if( m_multiDraw < 0 )
    {
        EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = emscripten_webgl_get_current_context();
        m_multiDraw = ( ctx > 0 && emscripten_webgl_enable_WEBGL_multi_draw( ctx ) ) ? 1 : 0;
    }

    if( m_enableDepthTest )
        glEnable( GL_DEPTH_TEST );
    else
        glDisable( GL_DEPTH_TEST );

    // Bind VAO first (required for WebGL 2.0 / OpenGL ES 3.0)
    glBindVertexArray( m_vao );

    // KICLOUD: the container binds its buffer(s); the split container keeps position, depth
    // and colour in separate buffers (docs/patches.md, B1.6c)
    if( m_shader != nullptr ) // Use shader if applicable
        m_shader->Use();

    cached->BindAttributes( m_vertexAttrib, m_depthAttrib, m_colorAttrib,
                            m_shader != nullptr ? m_shaderAttrib : -1 );

    PROF_TIMER cntDraw( "gl-draw-elements" );

    // KICLOUD: draw every visible range with one glMultiDrawArraysWEBGL call. PCBJam's path
    // rebuilt an index per vertex on the CPU and uploaded it every frame (hundreds of MB while
    // panning a large board) plus one glDrawArrays per large item. Adjacent ranges are merged;
    // the draw order is unchanged. See docs/patches.md (B1.6).
    //
    // Without WEBGL_multi_draw (Firefox) the same merged ranges feed a draw plan
    // (buildFallbackPlan): long ranges are drawn directly, only short ones go through the
    // element buffer, and the buffer is uploaded only when the merged ranges differ from the
    // previous frame's. See docs/patches.md (bug 38).
    int  drawCalls = 0;
    unsigned int uploadedIndices = 0;

    m_drawFirsts.clear();
    m_drawCounts.clear();

    for( const VRANGE& range : m_vranges )
    {
        GLint   first = range.m_start;
        GLsizei count = range.m_end - range.m_start + 1;

        if( !m_drawFirsts.empty() && m_drawFirsts.back() + m_drawCounts.back() == first )
            m_drawCounts.back() += count;
        else
        {
            m_drawFirsts.push_back( first );
            m_drawCounts.push_back( count );
        }
    }

    if( m_multiDraw == 1 )
    {
        if( !m_drawFirsts.empty() )
        {
            glMultiDrawArraysWEBGL( GL_TRIANGLES, m_drawFirsts.data(), m_drawCounts.data(),
                                    (GLsizei) m_drawFirsts.size() );
            drawCalls = 1;
        }
    }
    else if( !m_drawFirsts.empty() )
    {
        bool changed = buildFallbackPlan();

        if( m_planIndexCount == 0 )
            m_planValid = true;     // nothing to upload: the plan is complete as it is
        else
        {
            if( m_ebo == 0 )
            {
                glGenBuffers( 1, &m_ebo );
                m_eboCapacity = 0;
                changed = true;
            }

            glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, m_ebo );

            if( changed || !m_planValid )
            {
                // Grow the GPU buffer by half again so a slowly growing view does not
                // reallocate it every frame; otherwise overwrite the start of it in place.
                if( m_planIndexCount > m_eboCapacity )
                {
                    m_eboCapacity = m_planIndexCount + m_planIndexCount / 2;
                    glBufferData( GL_ELEMENT_ARRAY_BUFFER, m_eboCapacity * sizeof( GLuint ),
                                  nullptr, GL_DYNAMIC_DRAW );
                }

                glBufferSubData( GL_ELEMENT_ARRAY_BUFFER, 0, m_planIndexCount * sizeof( GLuint ),
                                 m_indices.get() );
                uploadedIndices = m_planIndexCount;
                m_planValid = true;
            }
        }

        for( const DRAW_STEP& step : m_fallbackPlan )
        {
            if( step.m_indexed )
            {
                glDrawElements( GL_TRIANGLES, step.m_count, GL_UNSIGNED_INT,
                                (GLvoid*) ( (uintptr_t) step.m_first * sizeof( GLuint ) ) );
            }
            else
            {
                glDrawArrays( GL_TRIANGLES, step.m_first, step.m_count );
            }

            drawCalls++;
        }

        if( m_planIndexCount > 0 )
            glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
    }

    cntDraw.Stop();

    KI_TRACE( traceGalProfile,
              "Cached manager size: VBO size %u iranges %zu merged %zu multidraw %d drawcalls %d "
              "indices uploaded %u\n",
              cached->AllItemsSize(), m_vranges.size(), m_drawFirsts.size(), m_multiDraw,
              drawCalls, uploadedIndices );
    KI_TRACE( traceGalProfile, "Timing: %s\n", cntDraw.to_string() );

    glBindBuffer( GL_ARRAY_BUFFER, 0 );
    cached->ClearDirty();

    // Deactivate vertex arrays (modern vertex attributes)
    glDisableVertexAttribArray( m_colorAttrib );
    glDisableVertexAttribArray( m_vertexAttrib );

    if( m_depthAttrib >= 0 )    // KICLOUD: B1.6c
        glDisableVertexAttribArray( m_depthAttrib );

    if( m_shader != nullptr )
    {
        glDisableVertexAttribArray( m_shaderAttrib );
        m_shader->Deactivate();
    }

    // Unbind VAO
    glBindVertexArray( 0 );

    m_isDrawing = false;
}


// KICLOUD (bug 38): the draw plan for browsers without WEBGL_multi_draw.
//
// Purpose: draw the merged visible ranges (m_drawFirsts / m_drawCounts, already in draw order)
// with as little CPU work and upload as possible when glMultiDrawArraysWEBGL is missing.
// Why: PCBJam's fallback wrote one index per visible vertex and uploaded the whole index array
// with glBufferData on every frame, so each pan or zoom step of a large board in Firefox cost
// tens of MB of index writes and uploads.
// How:
//  - When there are few merged ranges, each one is a glDrawArrays (no indices at all).
//  - Otherwise ranges of at least DIRECT_MIN vertices are drawn directly and only the shorter
//    ones are written to the index array; consecutive short ranges share one glDrawElements.
//    The steps keep the original order, so overlapping items are painted exactly as before.
//  - One-entry cache: if the merged ranges are the same as for the plan already built, nothing
//    is rebuilt and the element buffer on the GPU is reused. Indices only name vertex numbers,
//    so the cache stays correct when vertex data (colour, depth, position) changes in place.
// Result: true when the index array changed and must be uploaded; m_fallbackPlan,
// m_planIndexCount and the cache key are updated. m_indices grows as needed (never shrinks).
bool GPU_CACHED_MANAGER::buildFallbackPlan()
{
    // At most this many merged ranges: all of them are drawn directly
    constexpr size_t   ALL_DIRECT_MAX = 64;
    // A range at least this long is drawn directly even when there are many ranges
    constexpr GLsizei  DIRECT_MIN = 3000;

    if( m_planValid && m_planFirsts == m_drawFirsts && m_planCounts == m_drawCounts )
        return false;

    m_planFirsts = m_drawFirsts;
    m_planCounts = m_drawCounts;
    m_fallbackPlan.clear();
    m_planIndexCount = 0;

    const size_t n = m_drawFirsts.size();
    const bool   allDirect = n <= ALL_DIRECT_MAX;

    if( !allDirect )
    {
        size_t needed = 0;

        for( size_t i = 0; i < n; i++ )
        {
            if( m_drawCounts[i] < DIRECT_MIN )
                needed += m_drawCounts[i];
        }

        resizeIndices( (unsigned int) needed );
    }

    GLuint* iptr = m_indices.get();

    for( size_t i = 0; i < n; i++ )
    {
        const GLint   first = m_drawFirsts[i];
        const GLsizei count = m_drawCounts[i];

        if( allDirect || count >= DIRECT_MIN )
        {
            m_fallbackPlan.push_back( { false, first, count } );
            continue;
        }

        // A short range: append its vertex numbers and extend (or start) an indexed step
        if( m_fallbackPlan.empty() || !m_fallbackPlan.back().m_indexed )
            m_fallbackPlan.push_back( { true, (GLint) m_planIndexCount, 0 } );

        for( GLsizei v = 0; v < count; v++ )
            *iptr++ = (GLuint) ( first + v );

        m_fallbackPlan.back().m_count += count;
        m_planIndexCount += count;
    }

    // The plan changed; the caller uploads m_indices (when any are used) and marks it valid
    m_planValid = false;
    return true;
}


void GPU_CACHED_MANAGER::resizeIndices( unsigned int aNewSize )
{
    if( aNewSize > m_indicesCapacity )
    {
        m_indicesCapacity = aNewSize;
        m_indices.reset( new GLuint[m_indicesCapacity] );
    }
}


// Noncached manager
GPU_NONCACHED_MANAGER::GPU_NONCACHED_MANAGER( VERTEX_CONTAINER* aContainer ) :
        GPU_MANAGER( aContainer ),
        m_vbo( 0 )
{
}


GPU_NONCACHED_MANAGER::~GPU_NONCACHED_MANAGER()
{
    if( m_vbo )
    {
        glDeleteBuffers( 1, &m_vbo );
        m_vbo = 0;
    }
}


void GPU_NONCACHED_MANAGER::BeginDrawing()
{
    // Nothing has to be prepared
}


void GPU_NONCACHED_MANAGER::DrawIndices( const VERTEX_ITEM* aItem )
{
    wxASSERT_MSG( false, wxT( "Not implemented yet" ) );
}


void GPU_NONCACHED_MANAGER::EndDrawing()
{
#ifdef KICAD_GAL_PROFILE
    PROF_TIMER totalRealTime;
#endif /* KICAD_GAL_PROFILE */

    if( m_container->GetSize() == 0 )
        return;

    VERTEX* vertices = m_container->GetAllVertices();

    if( m_enableDepthTest )
        glEnable( GL_DEPTH_TEST );
    else
        glDisable( GL_DEPTH_TEST );

    // Bind VAO first (required for WebGL 2.0 / OpenGL ES 3.0)
    glBindVertexArray( m_vao );

    // Upload vertex data to VBO each frame
    // WebGL 2.0 does not support client-side vertex arrays — a VBO must be bound
    if( m_vbo == 0 )
        glGenBuffers( 1, &m_vbo );

    glBindBuffer( GL_ARRAY_BUFFER, m_vbo );
    glBufferData( GL_ARRAY_BUFFER, m_container->GetSize() * VERTEX_SIZE,
                  vertices, GL_STREAM_DRAW );

    // Vertex position (a_vertex) — byte offsets into VBO, not raw pointers
    // KICLOUD: a_vertex is (x, y) and a_depth is z (docs/patches.md, B1.6c)
    glEnableVertexAttribArray( m_vertexAttrib );
    glVertexAttribPointer( m_vertexAttrib, 2, GL_FLOAT, GL_FALSE, VERTEX_SIZE,
                           (GLvoid*) COORD_OFFSET );

    if( m_depthAttrib >= 0 )
    {
        glEnableVertexAttribArray( m_depthAttrib );
        glVertexAttribPointer( m_depthAttrib, 1, GL_FLOAT, GL_FALSE, VERTEX_SIZE,
                               (GLvoid*) ( COORD_OFFSET + 2 * sizeof( GLfloat ) ) );
    }

    // Vertex color (a_color) - note: normalize=GL_TRUE for unsigned bytes to [0,1]
    glEnableVertexAttribArray( m_colorAttrib );
    glVertexAttribPointer( m_colorAttrib, COLOR_STRIDE, GL_UNSIGNED_BYTE, GL_TRUE, VERTEX_SIZE,
                           (GLvoid*) COLOR_OFFSET );

    if( m_shader != nullptr ) // Use shader if applicable
    {
        m_shader->Use();
        glEnableVertexAttribArray( m_shaderAttrib );
        glVertexAttribPointer( m_shaderAttrib, SHADER_STRIDE, GL_FLOAT, GL_FALSE, VERTEX_SIZE,
                               (GLvoid*) SHADER_OFFSET );
    }

    glDrawArrays( GL_TRIANGLES, 0, m_container->GetSize() );

#ifdef KICAD_GAL_PROFILE
    wxLogTrace( traceGalProfile, wxT( "Noncached manager size: %d" ), m_container->GetSize() );
#endif /* KICAD_GAL_PROFILE */

    // Deactivate vertex arrays
    glDisableVertexAttribArray( m_colorAttrib );
    glDisableVertexAttribArray( m_vertexAttrib );

    if( m_depthAttrib >= 0 )    // KICLOUD: B1.6c
        glDisableVertexAttribArray( m_depthAttrib );

    if( m_shader != nullptr )
    {
        glDisableVertexAttribArray( m_shaderAttrib );
        m_shader->Deactivate();
    }

    glBindBuffer( GL_ARRAY_BUFFER, 0 );

    // Unbind VAO
    glBindVertexArray( 0 );

    m_container->Clear();

#ifdef KICAD_GAL_PROFILE
    totalRealTime.Stop();
    wxLogTrace( traceGalProfile, wxT( "GPU_NONCACHED_MANAGER::EndDrawing(): %.1f ms" ),
                totalRealTime.msecs() );
#endif /* KICAD_GAL_PROFILE */
}

void GPU_MANAGER::EnableDepthTest( bool aEnabled )
{
    m_enableDepthTest = aEnabled;
}
