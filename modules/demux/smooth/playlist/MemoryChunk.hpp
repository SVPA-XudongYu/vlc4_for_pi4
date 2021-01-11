/*
 * MemoryChunk.hpp
 *****************************************************************************
 * Copyright (C) 2015 - VideoLAN Authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/
#ifndef MEMORYCHUNK_HPP
#define MEMORYCHUNK_HPP

#include "../../adaptive/http/Chunk.h"

namespace smooth
{
    namespace http
    {
        using namespace adaptive::http;

        class MemoryChunkSource : public AbstractChunkSource
        {
            public:
                MemoryChunkSource(block_t *);
                virtual ~MemoryChunkSource();

                virtual block_t * readBlock() override;
                virtual block_t * read(size_t) override;
                virtual bool      hasMoreData() const override;

            private:
                block_t *data;
                size_t   i_read;
        };

    }
}

#endif // MEMORYCHUNK_HPP
