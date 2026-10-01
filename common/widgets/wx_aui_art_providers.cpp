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

#include <algorithm>
#include <wx/aui/aui.h>
#include <wx/aui/framemanager.h>
#include <wx/aui/auibook.h>
#include <wx/bitmap.h>
#include <wx/dc.h>
#include <wx/settings.h>

#include <kiplatform/ui.h>
#include <pgm_base.h>
#include <settings/common_settings.h>
#include <widgets/panel_notebook_base.h>
#include <widgets/wx_aui_art_providers.h>
#include <gal/color4d.h>

#if wxCHECK_VERSION( 3, 3, 0 )
wxSize WX_AUI_TOOLBAR_ART::GetToolSize( wxReadOnlyDC& aDc, wxWindow* aWindow,
                                        const wxAuiToolBarItem& aItem )
#else
wxSize WX_AUI_TOOLBAR_ART::GetToolSize( wxDC& aDc, wxWindow* aWindow,
                                        const wxAuiToolBarItem& aItem )
#endif
{
    // Based on the upstream wxWidgets implementation, but simplified for our application
    int size = aWindow->FromDIP( Pgm().GetCommonSettings()->m_Appearance.toolbar_icon_size );

    int width = size;
    int height = size;

    if( ( m_flags & wxAUI_TB_TEXT ) && !aItem.GetLabel().empty() )
    {
        aDc.SetFont( m_font );
        int tx, ty;

        if( m_textOrientation == wxAUI_TBTOOL_TEXT_BOTTOM )
        {
            aDc.GetTextExtent( wxT( "ABCDHgj" ), &tx, &ty );
            height += ty;

            if( !aItem.GetLabel().empty() )
            {
                aDc.GetTextExtent( aItem.GetLabel(), &tx, &ty );
                width = wxMax( width, tx + aWindow->FromDIP( 6 ) );
            }
        }
        else if( m_textOrientation == wxAUI_TBTOOL_TEXT_RIGHT )
        {
            width += aWindow->FromDIP( 3 ); // space between left border and bitmap
            width += aWindow->FromDIP( 3 ); // space between bitmap and text

            if( !aItem.GetLabel().empty() )
            {
                aDc.GetTextExtent( aItem.GetLabel(), &tx, &ty );
                width += tx;
                height = wxMax( height, ty );
            }
        }
    }

    if( aItem.HasDropDown() )
    {
        int dropdownWidth = GetElementSize( wxAUI_TBART_DROPDOWN_SIZE );
        width += dropdownWidth + aWindow->FromDIP( 4 );
    }

    return wxSize( width, height );
}


void WX_AUI_TOOLBAR_ART::DrawButton( wxDC& aDc, wxWindow* aWindow, const wxAuiToolBarItem& aItem,
                                     const wxRect& aRect )
{
    // Based on upstream implementation
    int bmpX = 0, bmpY = 0;
    int textX = 0, textY = 0;

    const wxBitmap& bmp = aItem.GetCurrentBitmapFor( aWindow );
    const wxSize    bmpSize = bmp.IsOk() ? bmp.GetLogicalSize() : wxSize( 0, 0 );

    if( ( m_flags & wxAUI_TB_TEXT ) && !aItem.GetLabel().empty() )
    {
        aDc.SetFont( m_font );

        int textWidth = 0, textHeight = 0;
        int tx, ty;

        aDc.GetTextExtent( wxT( "ABCDHgj" ), &tx, &textHeight );
        aDc.GetTextExtent( aItem.GetLabel(), &textWidth, &ty );

        if( m_textOrientation == wxAUI_TBTOOL_TEXT_BOTTOM )
        {
            bmpX = aRect.x + ( aRect.width / 2 ) - ( bmpSize.x / 2 );

            bmpY = aRect.y + ( ( aRect.height - textHeight ) / 2 ) - ( bmpSize.y / 2 );

            textX = aRect.x + ( aRect.width / 2 ) - ( textWidth / 2 ) + 1;
            textY = aRect.y + aRect.height - textHeight - 1;
        }
        else if( m_textOrientation == wxAUI_TBTOOL_TEXT_RIGHT )
        {
            bmpX = aRect.x + aWindow->FromDIP( 3 );

            bmpY = aRect.y + ( aRect.height / 2 ) - ( bmpSize.y / 2 );

            textX = bmpX + aWindow->FromDIP( 3 ) + bmpSize.x;
            textY = aRect.y + ( aRect.height / 2 ) - ( textHeight / 2 );
        }
    }
    else
    {
        bmpX = aRect.x + ( aRect.width / 2 ) - ( bmpSize.x / 2 );
        bmpY = aRect.y + ( aRect.height / 2 ) - ( bmpSize.y / 2 );
    }

    bool isThemeDark = KIPLATFORM::UI::IsDarkTheme();

#ifdef __EMSCRIPTEN__
    // KICLOUD: the browser editor's tool shapes (B1.20, IDEAS.md #8): a round hover in the
    // panel's stronger tone, and the soft accent fill with an accent edge for an active or
    // pressed tool. The colours are the wx port's system table (src/wasm/settings.cpp, the
    // editor's theme tokens). Icon positions are unchanged.
    if( !( aItem.GetState() & wxAUI_BUTTON_STATE_DISABLED ) )
    {
        const int     state = aItem.GetState();
        const bool    active = ( state & wxAUI_BUTTON_STATE_CHECKED ) || ( state & wxAUI_BUTTON_STATE_PRESSED );
        const bool    hover = ( state & wxAUI_BUTTON_STATE_HOVER ) || aItem.IsSticky();
        const wxColour soft = wxSystemSettings::GetColour( wxSYS_COLOUR_MENUHILIGHT );
        const wxColour accent = wxSystemSettings::GetColour( wxSYS_COLOUR_HIGHLIGHT );
        wxRect        r = aRect;
        r.Deflate( 1 );
        const double  radius = std::min( r.width, r.height ) / 2.0;

        if( active || hover )
        {
            if( active )
            {
                // the edge: the accent mixed into the soft fill, so it reads as one shape
                wxColour edge( ( soft.Red() + accent.Red() ) / 2, ( soft.Green() + accent.Green() ) / 2,
                               ( soft.Blue() + accent.Blue() ) / 2 );
                aDc.SetPen( wxPen( edge ) );
                aDc.SetBrush( wxBrush( hover ? soft.ChangeLightness( isThemeDark ? 115 : 97 ) : soft ) );
            }
            else
            {
                const wxColour fill = wxSystemSettings::GetColour( wxSYS_COLOUR_INACTIVECAPTION );
                aDc.SetPen( wxPen( fill ) );
                aDc.SetBrush( wxBrush( fill ) );
            }

            aDc.DrawRoundedRectangle( r, radius );
        }
    }
#else
    if( !( aItem.GetState() & wxAUI_BUTTON_STATE_DISABLED ) )
    {
        if( aItem.GetState() & wxAUI_BUTTON_STATE_PRESSED )
        {
            aDc.SetPen( wxPen( m_highlightColour ) );
            aDc.SetBrush( wxBrush( m_highlightColour.ChangeLightness( isThemeDark ? 20 : 150 ) ) );
            aDc.DrawRectangle( aRect );
        }
        else if( ( aItem.GetState() & wxAUI_BUTTON_STATE_HOVER ) || aItem.IsSticky() )
        {
            aDc.SetPen( wxPen( m_highlightColour ) );
            aDc.SetBrush( wxBrush( m_highlightColour.ChangeLightness( isThemeDark ? 40 : 170 ) ) );

            // draw an even lighter background for checked item hovers (since
            // the hover background is the same color as the check background)
            if( aItem.GetState() & wxAUI_BUTTON_STATE_CHECKED )
                aDc.SetBrush(
                        wxBrush( m_highlightColour.ChangeLightness( isThemeDark ? 50 : 180 ) ) );

            aDc.DrawRectangle( aRect );
        }
        else if( aItem.GetState() & wxAUI_BUTTON_STATE_CHECKED )
        {
            // it's important to put this code in an else statement after the
            // hover, otherwise hovers won't draw properly for checked items
            aDc.SetPen( wxPen( m_highlightColour ) );
            aDc.SetBrush( wxBrush( m_highlightColour.ChangeLightness( isThemeDark ? 40 : 170 ) ) );
            aDc.DrawRectangle( aRect );
        }
    }
#endif

    if( bmp.IsOk() )
        aDc.DrawBitmap( bmp, bmpX, bmpY, true );

    // set the item's text color based on if it is disabled
    aDc.SetTextForeground( wxSystemSettings::GetColour( wxSYS_COLOUR_BTNTEXT ) );

    if( aItem.GetState() & wxAUI_BUTTON_STATE_DISABLED )
    {
        aDc.SetTextForeground( wxSystemSettings::GetColour( wxSYS_COLOUR_GRAYTEXT ) );
    }

    if( ( m_flags & wxAUI_TB_TEXT ) && !aItem.GetLabel().empty() )
    {
        aDc.DrawText( aItem.GetLabel(), textX, textY );
    }
}


void WX_AUI_TOOLBAR_ART::saturateHighlightColor()
{
#ifdef __WXOSX__
    // Use a slightly stronger highlight colour over grey toolbar backgrounds
    KIGFX::COLOR4D highlight( m_highlightColour );
    m_highlightColour = highlight.Saturate( 0.6 ).ToColour();
#endif
}


void WX_AUI_TOOLBAR_ART::UpdateColoursFromSystem()
{
    wxAuiDefaultToolBarArt::UpdateColoursFromSystem();
    saturateHighlightColor();
}


#ifdef __EMSCRIPTEN__
// KICLOUD: flat toolbars in the browser editor (B1.20). wxSYS_COLOUR_MENUBAR is the editor's panel
// colour in the wx port's table (toolbars, menu bar and dock), BTNSHADOW its strong line colour.
void WX_AUI_TOOLBAR_ART::DrawBackground( wxDC& aDc, wxWindow* aWindow, const wxRect& aRect )
{
    DrawPlainBackground( aDc, aWindow, aRect );
}


void WX_AUI_TOOLBAR_ART::DrawPlainBackground( wxDC& aDc, wxWindow* aWindow, const wxRect& aRect )
{
    const wxColour panel = wxSystemSettings::GetColour( wxSYS_COLOUR_MENUBAR );
    aDc.SetBrush( wxBrush( panel ) );
    aDc.SetPen( wxPen( panel ) );
    wxRect r = aRect;
    r.height++;
    aDc.DrawRectangle( r );
}


void WX_AUI_TOOLBAR_ART::DrawSeparator( wxDC& aDc, wxWindow* aWindow, const wxRect& aRect )
{
    const bool horizontal = !( m_flags & wxAUI_TB_VERTICAL );
    wxRect     r = aRect;

    if( horizontal )
    {
        int h = std::min( r.height, aWindow->FromDIP( 18 ) );
        r.x += r.width / 2;
        r.width = aWindow->FromDIP( 1 );
        r.y += ( r.height - h ) / 2;
        r.height = h;
    }
    else
    {
        int w = std::min( r.width, aWindow->FromDIP( 18 ) );
        r.y += r.height / 2;
        r.height = aWindow->FromDIP( 1 );
        r.x += ( r.width - w ) / 2;
        r.width = w;
    }

    const wxColour line = wxSystemSettings::GetColour( wxSYS_COLOUR_BTNSHADOW );
    aDc.SetBrush( wxBrush( line ) );
    aDc.SetPen( *wxTRANSPARENT_PEN );
    aDc.DrawRectangle( r );
}
#endif


class ToolbarCommandCapture : public wxEvtHandler
{
public:
    ToolbarCommandCapture() { m_lastId = 0; }
    int GetCommandId() const { return m_lastId; }

    bool ProcessEvent( wxEvent& evt ) override
    {
        if( evt.GetEventType() == wxEVT_MENU )
        {
            m_lastId = evt.GetId();
            return true;
        }

        if( GetNextHandler() )
            return GetNextHandler()->ProcessEvent( evt );

        return false;
    }

private:
    int m_lastId;
};


int WX_AUI_TOOLBAR_ART::ShowDropDown( wxWindow* wnd, const wxAuiToolBarItemArray& items )
{
    wxMenu menuPopup;
    bool   skipNextSeparator = true;

    size_t i, count = items.GetCount();
    for( i = 0; i < count; ++i )
    {
        wxAuiToolBarItem& item = items.Item( i );

        if( item.GetKind() == wxITEM_SEPARATOR )
        {
            if( !skipNextSeparator )
            {
                menuPopup.AppendSeparator();
                skipNextSeparator = true;
            }
        }
        else if( item.GetKind() == wxITEM_NORMAL || item.GetKind() == wxITEM_CHECK || item.GetKind() == wxITEM_RADIO )
        {
            wxString text = item.GetShortHelp();

            if( text.empty() )
                text = item.GetLabel();

            if( text.empty() )
                text = wxT( " " );

            wxString firstLine = text.BeforeFirst( '\n' );
            wxString accel;
            wxString label = firstLine.BeforeFirst( '\t', &accel );

            text = label;

            if( !accel.empty() )
            {
                // Remove brackets from accelerator string so it's recognized
                if( accel.starts_with( "(" ) && accel.ends_with( ")" ) )
                    accel = accel.Mid( 1, accel.size() - 2 );

                text << "\t" << accel;
            }

            bool       checked = item.GetState() & wxAUI_BUTTON_STATE_CHECKED;
            wxItemKind menuKind = wxITEM_NORMAL;

            if( ( item.GetKind() == wxITEM_CHECK || item.GetKind() == wxITEM_RADIO ) && checked )
                menuKind = static_cast<wxItemKind>( item.GetKind() );

            wxMenuItem* m = new wxMenuItem( &menuPopup, item.GetId(), text, item.GetShortHelp(), menuKind );

            if( !m->IsCheckable() )
                m->SetBitmap( item.GetBitmapBundle() );

            menuPopup.Append( m );

            if( m->IsCheckable() )
                m->Check( checked );

            skipNextSeparator = false;
        }
    }

    // find out where to put the popup menu of window items
    wxPoint pt = ::wxGetMousePosition();
    pt = wnd->ScreenToClient( pt );

    // find out the screen coordinate at the bottom of the tab ctrl
    wxRect cli_rect = wnd->GetClientRect();
    pt.y = cli_rect.y + cli_rect.height;

    ToolbarCommandCapture* cc = new ToolbarCommandCapture;
    wnd->PushEventHandler( cc );
    wnd->PopupMenu( &menuPopup, pt );
    int command = cc->GetCommandId();
    wnd->PopEventHandler( true );

    return command;
}


WX_AUI_DOCK_ART::WX_AUI_DOCK_ART() :
        wxAuiDefaultDockArt()
{
#if defined( _WIN32 )
    // Use normal control font, wx likes to use "small"
    m_captionFont = *wxNORMAL_FONT;

    // Increase the box the caption rests in size a bit
    m_captionSize = ( wxNORMAL_FONT->GetPointSize() * 7 ) / 4 + 6;
#endif

    SetColour( wxAUI_DOCKART_ACTIVE_CAPTION_TEXT_COLOUR,
               wxSystemSettings::GetColour( wxSYS_COLOUR_BTNTEXT ) );
    SetColour( wxAUI_DOCKART_INACTIVE_CAPTION_TEXT_COLOUR,
               wxSystemSettings::GetColour( wxSYS_COLOUR_BTNTEXT ) );

    // Turn off the ridiculous looking gradient
    m_gradientType = wxAUI_GRADIENT_NONE;

#ifdef __EMSCRIPTEN__
    // KICLOUD: the base class constructor ran its own version (B1.20)
    UpdateColoursFromSystem();
#endif
}


#ifdef __EMSCRIPTEN__
// KICLOUD: quiet pane captions and a panel-coloured dock in the browser editor (B1.20): the
// captions (Appearance, Properties, ...) take the panel colour with normal text, and the sashes
// and borders the panel and line colours, from the wx port's system table.
void WX_AUI_DOCK_ART::UpdateColoursFromSystem()
{
    wxAuiDefaultDockArt::UpdateColoursFromSystem();

    const wxColour panel = wxSystemSettings::GetColour( wxSYS_COLOUR_MENUBAR );
    const wxColour line = wxSystemSettings::GetColour( wxSYS_COLOUR_BTNSHADOW );
    const wxColour text = wxSystemSettings::GetColour( wxSYS_COLOUR_BTNTEXT );

    m_baseColour = panel;
    m_backgroundBrush = wxBrush( panel );
    m_sashBrush = wxBrush( panel );
    m_gripperBrush = wxBrush( panel );
    m_borderPen = wxPen( line );
    m_activeCaptionColour = panel;
    m_activeCaptionGradientColour = panel;
    m_activeCaptionTextColour = text;
    m_inactiveCaptionColour = panel;
    m_inactiveCaptionGradientColour = panel;
    m_inactiveCaptionTextColour = text;
}
#endif


void WX_AUI_TAB_ART::DrawTab( wxDC& dc, wxWindow* wnd, const wxAuiNotebookPage& page, const wxRect& in_rect,
                              int close_button_state, wxRect* out_tab_rect, wxRect* out_button_rect,
                              int* x_extent )
{
    PANEL_NOTEBOOK_BASE* panel = dynamic_cast<PANEL_NOTEBOOK_BASE*>( page.window );

    if( panel && !panel->GetClosable() )
        close_button_state = wxAUI_BUTTON_STATE_HIDDEN;

    return wxAuiGenericTabArt::DrawTab( dc, wnd, page, in_rect, close_button_state, out_tab_rect,
                                        out_button_rect, x_extent );
}
