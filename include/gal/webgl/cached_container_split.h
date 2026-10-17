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

#ifndef CACHED_CONTAINER_SPLIT_H_
#define CACHED_CONTAINER_SPLIT_H_

#include <gal/webgl/cached_container.h>

#include <climits>
#include <cstdint>
#include <vector>

namespace KIGFX
{
/**
 * A CACHED_CONTAINER that keeps the cached vertices on the GPU only.
 *
 * CACHED_CONTAINER_RAM keeps a CPU copy of every cached vertex (32 bytes each) beside the GPU
 * buffer. On a large board that copy is hundreds of MB of wasm memory, and growing the container
 * needs the old and the new copy at once. Nothing reads the copy back: it only exists so that
 * colour and depth changes can rewrite part of each vertex before a re-upload.
 *
 * This container splits the vertex attributes over three GPU buffers:
 *  - position and shader parameters (x, y, shader[4]), written once when an item is built;
 *  - depth (z), rewritten by SetItemDepth();
 *  - colour (rgba), rewritten by SetItemColor().
 * A colour or depth change is then a constant fill of one buffer. Only the vertices of the item
 * being built and of finished items waiting for upload (about MAX_PENDING_BYTES) are held in
 * memory. Items are moved and compacted on the GPU (glCopyBufferSubData).
 *
 * Ordering: waiting vertices and waiting colour/depth fills never cover the same vertices. Before
 * one is queued over the other, the other is uploaded first. Waiting fills of one buffer are
 * uploaded together, merged into contiguous ranges (KICLOUD: F-P1); where two of them overlap,
 * the one queued later wins, as if each had been uploaded at once.
 */
class CACHED_CONTAINER_SPLIT : public CACHED_CONTAINER
{
public:
    CACHED_CONTAINER_SPLIT( unsigned int aSize = DEFAULT_SIZE );
    ~CACHED_CONTAINER_SPLIT();

    void SetItem( VERTEX_ITEM* aItem ) override;
    void FinishItem() override;
    VERTEX* Allocate( unsigned int aSize ) override;
    void Clear() override;

    void Map() override {}
    void Unmap() override;

    bool IsMapped() const override
    {
        return true;
    }

    unsigned int GetBufferHandle() const override
    {
        return m_buffers[POSITION];
    }

    ///< There is no CPU copy of the stored vertices.
    VERTEX* GetVertices( unsigned int aOffset ) const override
    {
        return nullptr;
    }

    void SetItemColor( unsigned int aOffset, unsigned int aSize,
                       const GLubyte aColor[4] ) override;
    void SetItemDepth( unsigned int aOffset, unsigned int aSize, GLfloat aDepth ) override;

    void BindAttributes( int aVertexAttrib, int aDepthAttrib, int aColorAttrib,
                         int aShaderAttrib ) override;

protected:
    bool defragmentResize( unsigned int aNewSize ) override;
    void moveItemData( unsigned int aFrom, unsigned int aTo, unsigned int aCount ) override;

private:
    enum BUFFER
    {
        POSITION = 0,
        DEPTH,
        COLOR,
        BUFFER_COUNT
    };

    ///< Layout of the POSITION buffer
    struct POSITION_DATA
    {
        GLfloat x, y;
        GLfloat shader[4];
    };

    ///< Bytes per vertex in each buffer
    static constexpr unsigned int STRIDES[BUFFER_COUNT] = { sizeof( POSITION_DATA ),
                                                            sizeof( GLfloat ),
                                                            4 * sizeof( GLubyte ) };

    ///< Finished items are uploaded once this many bytes are waiting
    static constexpr size_t MAX_PENDING_BYTES = 8 * 1024 * 1024;

    ///< Vertices converted or filled per upload call (bounds the scratch memory)
    static constexpr unsigned int UPLOAD_BLOCK = 65536;


    ///< Vertices of finished items waiting for upload
    struct PENDING
    {
        unsigned int gpuOffset;
        unsigned int stageOffset;
        unsigned int count;
    };

    ///< A constant value waiting to be written over a range of the DEPTH or COLOR buffer
    struct FILL
    {
        unsigned int offset = 0;
        unsigned int count = 0;
        uint32_t     value = 0;
    };

    /**
     * KICLOUD: F-P1 (LB2): the colour or depth fills of one buffer waiting for upload.
     *
     * A recolour of the whole board (a layer or highlight change) sets the colour of every item
     * group, one SetItemColor() each. Uploading each one at once cost one bufferSubData call per
     * group (about 260,000 per recolour on the largest demo board). The fills are now collected
     * here and uploaded together by flushFills(): runs that touch become one upload, so a whole
     * board recolour needs about one call per UPLOAD_BLOCK vertices plus one per real gap.
     */
    struct FILL_QUEUE
    {
        std::vector<FILL> runs;     ///< In the order they were queued (later runs win on overlap)
        unsigned int begin = UINT_MAX;   ///< Lowest vertex offset covered by any run
        unsigned int end = 0;            ///< One past the highest vertex offset covered
        bool sorted = true;              ///< Runs are in ascending offset order without overlap

        bool empty() const { return runs.empty(); }

        ///< Whether a vertex range may touch a waiting run (bounds only: may answer true when no
        ///< run actually covers it, which only costs an earlier upload).
        bool mayOverlap( unsigned int aOffset, unsigned int aCount ) const
        {
            return !runs.empty() && aOffset < end && aOffset + aCount > begin;
        }

        void clear()
        {
            runs.clear();
            begin = UINT_MAX;
            end = 0;
            sorted = true;
        }
    };

    ///< Waiting runs above this count are uploaded at once (bounds the queue at ~12 MB)
    static constexpr size_t MAX_FILL_RUNS = 1024 * 1024;

    ///< Allocate the GPU buffers at the container's size on first use.
    void ensureGpuBuffers();

    ///< Upload the waiting vertices; keep the vertices of the item being built.
    void flushVertices();

    ///< Upload all waiting fills of one buffer (KICLOUD: F-P1: merged into contiguous uploads).
    void flushFills( FILL_QUEUE& aQueue, BUFFER aBuffer );

    ///< Cut overlapping queued runs into disjoint ones where the run queued last wins (KICLOUD: F-P1).
    static void resolveOverlaps( std::vector<FILL>& aRuns );

    ///< Upload everything that is waiting.
    void flushAll();

    ///< Queue a fill, merging it with the last queued one when it continues it.
    void queueFill( FILL_QUEUE& aQueue, BUFFER aBuffer, unsigned int aOffset, unsigned int aCount,
                    uint32_t aValue );

    ///< Split vertices into the three buffers.
    void uploadVertices( unsigned int aGpuOffset, const VERTEX* aVertices, unsigned int aCount );

    ///< Copy vertices between (or within) GPU buffer sets.
    void copyVertices( const GLuint aFrom[BUFFER_COUNT], unsigned int aFromOffset,
                       const GLuint aTo[BUFFER_COUNT], unsigned int aToOffset,
                       unsigned int aCount );

    bool overlapsPending( unsigned int aOffset, unsigned int aCount ) const
    {
        return !m_pending.empty() && aOffset < m_pendingEnd && aOffset + aCount > m_pendingBegin;
    }

    GLuint       m_buffers[BUFFER_COUNT];
    unsigned int m_gpuSize;     ///< Size of the GPU buffers in vertices (0: not allocated)

    ///< Waiting vertices of finished items, then those of the item being built
    std::vector<VERTEX>  m_stage;
    std::vector<PENDING> m_pending;
    unsigned int         m_pendingBegin;
    unsigned int         m_pendingEnd;

    unsigned int m_itemStart;   ///< Index in m_stage of the built item's first staged vertex
    unsigned int m_itemBase;    ///< Vertices the built item already had on the GPU

    FILL_QUEUE m_colorFills;
    FILL_QUEUE m_depthFills;

    std::vector<unsigned char> m_scratch;
};
} // namespace KIGFX

#endif /* CACHED_CONTAINER_SPLIT_H_ */
