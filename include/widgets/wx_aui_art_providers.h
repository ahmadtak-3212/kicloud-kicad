/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
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

#pragma once

#include <wx/aui/auibar.h>
#include <wx/aui/dockart.h>


class WX_AUI_TOOLBAR_ART : public wxAuiDefaultToolBarArt
{
public:
    WX_AUI_TOOLBAR_ART() :
            wxAuiDefaultToolBarArt()
    {
        saturateHighlightColor();
    }

    virtual ~WX_AUI_TOOLBAR_ART() = default;

#if wxCHECK_VERSION( 3, 3, 0 )
    wxSize GetToolSize( wxReadOnlyDC& aDc, wxWindow* aWindow, const wxAuiToolBarItem& aItem ) override;
#else
    wxSize GetToolSize( wxDC& aDc, wxWindow* aWindow, const wxAuiToolBarItem& aItem ) override;
#endif

    /**
     * Unfortunately we need to re-implement this to actually be able to control the size
     */
    void DrawButton( wxDC& aDc, wxWindow* aWindow, const wxAuiToolBarItem& aItem,
                     const wxRect& aRect ) override;

    void UpdateColoursFromSystem() override;

    int ShowDropDown( wxWindow* wnd, const wxAuiToolBarItemArray& items ) override;

#ifdef __EMSCRIPTEN__
    // KICLOUD: the browser editor's flat toolbars (B1.20): a plain panel background and thin
    // separators instead of wx's gradients.
    // KICLOUD: LOOK.4 a side toolbar (vertical) is drawn as a rounded card; LOOK.3 a top toolbar
    // (horizontal) draws each run of buttons between two separators as one rounded pill, and a
    // separator becomes a gap. See the .cpp for the sizes and the colours.
    void DrawBackground( wxDC& aDc, wxWindow* aWindow, const wxRect& aRect ) override;
    void DrawPlainBackground( wxDC& aDc, wxWindow* aWindow, const wxRect& aRect ) override;
    void DrawSeparator( wxDC& aDc, wxWindow* aWindow, const wxRect& aRect ) override;
#endif

private:
    void saturateHighlightColor();
};


class WX_AUI_DOCK_ART : public wxAuiDefaultDockArt
{
public:
    WX_AUI_DOCK_ART();

#ifdef __EMSCRIPTEN__
    // KICLOUD: quiet pane captions and a panel-coloured dock in the browser editor (B1.20)
    void UpdateColoursFromSystem() override;

    // KICLOUD: LOOK.4 side panels as rounded cards and a gutter round the drawing area, drawn in
    // the pane border space (the border is wider in the browser editor, see the constructor).
    void DrawBorder( wxDC& aDc, wxWindow* aWindow, const wxRect& aRect,
                     wxAuiPaneInfo& aPane ) override;

    // KICLOUD: LOOK.4 a captioned side panel without a border gets rounded top corners on its
    // caption. (A21, comment refreshed: the PCB editor's Appearance and Selection Filter panels,
    // once the example here, have a border since A17 and are cards.)
    void DrawCaption( wxDC& aDc, wxWindow* aWindow, const wxString& aText, const wxRect& aRect,
                      wxAuiPaneInfo& aPane ) override;

    // KICLOUD: LOOK.4 wx's own sash, background and pane-button drawing, painted at the place
    // wxAUI laid them out (the .cpp's CLIENT_ORIGIN_FIX explains the wx port's offset).
    void DrawSash( wxDC& aDc, wxWindow* aWindow, int aOrientation, const wxRect& aRect ) override;
    void DrawBackground( wxDC& aDc, wxWindow* aWindow, int aOrientation,
                         const wxRect& aRect ) override;
    void DrawPaneButton( wxDC& aDc, wxWindow* aWindow, int aButton, int aButtonState,
                         const wxRect& aRect, wxAuiPaneInfo& aPane ) override;
#endif
};


class WX_AUI_TAB_ART : public wxAuiGenericTabArt
{
public:
    WX_AUI_TAB_ART() :
            wxAuiGenericTabArt()
    {}

    wxAuiTabArt* Clone() override
    {
        return new WX_AUI_TAB_ART();
    }

    void DrawTab( wxDC& dc, wxWindow* wnd, const wxAuiNotebookPage& page, const wxRect& in_rect,
                  int close_button_state, wxRect* out_tab_rect, wxRect* out_button_rect, int* x_extent ) override;
};

