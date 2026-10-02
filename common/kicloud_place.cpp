/*
 * This program source code file is part of kicloud, KiCad in the browser.
 *
 * Copyright (C) 2026 Ahmad Taka.
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

// Original file, Copyright (c) 2026 Ahmad Taka (KICLOUD: P3-I item 5). See include/kicloud_place.h.

#include <kicloud_place.h>

#include <chrono>

namespace
{
struct PENDING
{
    std::optional<std::string>            libId;
    std::chrono::steady_clock::time_point at;
};

PENDING g_pending[2];
} // namespace


void KICLOUD_PLACE::SetPending( KIND aKind, const std::string& aLibId )
{
    g_pending[aKind].libId = aLibId;
    g_pending[aKind].at = std::chrono::steady_clock::now();
}


std::optional<std::string> KICLOUD_PLACE::TakePending( KIND aKind )
{
    PENDING& p = g_pending[aKind];
    std::optional<std::string> out;

    if( p.libId && std::chrono::steady_clock::now() - p.at < std::chrono::seconds( 10 ) )
        out = p.libId;

    p.libId.reset();
    return out;
}
