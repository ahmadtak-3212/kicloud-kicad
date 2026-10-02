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

// Original file, Copyright (c) 2026 Ahmad Taka (KICLOUD: P3-I item 5, docs/patches.md).
//
// "Place" from kicloud's Parts panel: the page names a library part, then starts KiCad's own
// placement tool (Place Footprint / Place Symbol). The tool asks its chooser for the part; the
// chooser takes the pending part instead of showing the dialog, exactly once and only within a few
// seconds, so the placement behaves as if the user had picked it in the chooser.

#ifndef KICLOUD_PLACE_H
#define KICLOUD_PLACE_H

#include <optional>
#include <string>

namespace KICLOUD_PLACE
{
enum KIND { FOOTPRINT = 0, SYMBOL = 1 };

/// The next chooser of that kind returns aLibId ("Lib:Name") instead of asking the user
void SetPending( KIND aKind, const std::string& aLibId );

/// The pending part of that kind (then cleared), if one was set in the last 10 seconds
std::optional<std::string> TakePending( KIND aKind );
} // namespace KICLOUD_PLACE

#endif // KICLOUD_PLACE_H
