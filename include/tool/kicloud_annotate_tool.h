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

// Original file, Copyright (c) 2026 Ahmad Taka (KICLOUD: P3-I item 4, docs/patches.md).
//
// "Add Comment Box" (and "Add Comment Pin"): KiCad's own two-point rectangle interaction (the
// selection-area preview and the size readout of the drawing tools) that adds NOTHING to the
// design: no commit, no undo entry, nothing saved. When the box is drawn, the tool reports it
// to the page (window.kicloudAnnotate.onDone in the browser build) with the items inside it, so
// kicloud's Comments panel can attach a thread to that area. Used by the PCB editor and the
// schematic editor; each frame says where the user is (layer or sheet) through a callback.

#ifndef KICLOUD_ANNOTATE_TOOL_H
#define KICLOUD_ANNOTATE_TOOL_H

#include <functional>
#include <string>

#include <math/box2.h>
#include <tool/tool_action.h>
#include <tool/tool_interactive.h>

class EDA_DRAW_FRAME;


namespace KICLOUD_ACTIONS
{
/// Draw a comment box (in the Draw Rectangle toolbar group of the PCB and schematic editors)
extern TOOL_ACTION commentBox;

/// Pin a comment to one point
extern TOOL_ACTION commentPin;

/// KICLOUD: JLC.0.2 (docs/patches.md) Open kicloud's JLCPCB tab (PCB editor only: the plugin slot at the right end of the
/// top toolbar, and Tools > External Plugins). It changes nothing in KiCad: it only tells the page, which opens the tab.
extern TOOL_ACTION jlcpcbTools;
} // namespace KICLOUD_ACTIONS


/// Where the user draws: the active board layer, or the schematic sheet shown
struct KICLOUD_ANNOTATE_PLACE
{
    std::string layer;       ///< canonical layer name ("F.Cu"), or empty
    std::string sheetPath;   ///< KiCad sheet path (SCH_SHEET_PATH::PathAsString), or empty
    std::string sheetName;
};


class KICLOUD_ANNOTATE_TOOL : public TOOL_INTERACTIVE
{
public:
    /// @param aDoc the document kind the page knows this editor by: "pcb" or "sch"
    KICLOUD_ANNOTATE_TOOL( const std::string& aDoc, std::function<KICLOUD_ANNOTATE_PLACE()> aPlace );

    void Reset( RESET_REASON aReason ) override {}

    /// The interactive loop of both actions; reports the box (or point), or a cancel
    int Annotate( const TOOL_EVENT& aEvent );

    /// KICLOUD: JLC.0.2 Tell the page to open its JLCPCB tab (page event "jlcpcb.open"); no design change
    int OpenJlcpcb( const TOOL_EVENT& aEvent );

    void setTransitions() override;

private:
    void report( const BOX2I& aBox, bool aPin );
    void reportCancel();

    std::string                              m_doc;
    std::function<KICLOUD_ANNOTATE_PLACE()> m_place;
};

#endif // KICLOUD_ANNOTATE_TOOL_H
