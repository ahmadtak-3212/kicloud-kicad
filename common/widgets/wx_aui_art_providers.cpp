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

#ifdef __EMSCRIPTEN__
#include <cmath>            // KICLOUD: LOOK.4 std::sin/std::cos for the caption corners
#include <eda_base_frame.h> // KICLOUD: LOOK.4 VIEWER3D_FRAMENAME
#include <math/util.h>      // KICLOUD: LOOK.4 KiROUND
#endif

#ifdef __EMSCRIPTEN__
// KICLOUD: LOOK.4 (side columns and panels as cards) and LOOK.3 (pill groups on the top toolbars).
//
// The browser editor draws KiCad's toolbars and side panels like the kicloud dashboard: white
// rounded "cards" on the darker panel colour, with a small gap (a "gutter") between them. Every
// size below is in device-independent pixels (DIP: FromDIP() turns it into real pixels).
//
// Only the drawing changes, with one exception: a side toolbar is a little wider (RAIL_EXTRA), so
// its card has room round the buttons. The buttons keep their order, and a top toolbar's buttons
// keep their exact positions, so clicking (hit-testing) is unchanged. The drawing area (the board
// or schematic picture) is never painted over or clipped; only the space round it is painted.
//
// The colours come from the wx port's system colour table (tools/deps/wxWidgets/src/wasm/
// settings.cpp, set to the editor's light or dark theme tokens by B1.20), read at every paint, so a
// theme switch repaints everything in the new theme. No new colour value is written here.
namespace
{
// How much wider each button cell of a side toolbar is, on each side. The cell's height is
// unchanged, so a side toolbar shows exactly as many buttons as before.
constexpr int RAIL_EXTRA = 5;

// The gap between a side toolbar's window edge and its card. It must stay at most the toolbar's
// own outer padding (2 DIP, wxAuiToolBar's default margin): KiCad draws the small "more tools"
// triangle of a button group at the button cell's bottom-right corner (ACTION_TOOLBAR::
// OnCustomRender), and the triangle has to land inside the card.
constexpr int RAIL_INSET = 2;

constexpr int RAIL_RADIUS = 14;        // corner radius of a side toolbar's card
constexpr int RAIL_HOVER_RADIUS = 10;  // corner radius of a side toolbar button's hover/on fill
constexpr int SEPARATOR_INSET = 6;     // a side toolbar's separator line stops this far from the card edge

// The pane border of every bordered pane (the drawing area and the side panels such as the
// schematic's Properties): wxAUI reserves this much space round the pane, and DrawBorder paints it.
constexpr int PANE_BORDER = 8;

// Inside a side panel's border: the card starts PANE_BORDER - PANEL_PADDING from the outside, so
// PANEL_PADDING pixels of card colour show round the panel. A rounded corner of radius r covers a
// square corner only when the padding is at least r * (1 - 1/sqrt(2)), about 0.3 * r, so the
// radius below must stay at most PANEL_PADDING / 0.3.
constexpr int PANEL_PADDING = 4;
constexpr int PANEL_RADIUS = 12;

// The rounded top corners of a borderless captioned panel's caption (DrawCaption; since A17 the PCB
// editor's panels all have borders, so this serves any other captioned panel without one). Small, so the rounding
// stays clear of the caption's text, which starts 3 DIP from the left edge.
constexpr int CAPTION_RADIUS = 8;

// The name every KiCad frame gives the pane that holds its drawing area (pcb_edit_frame.cpp,
// sch_edit_frame.cpp, the 3D viewer, ...).
const wxString DRAWING_AREA_PANE = wxS( "DrawFrame" );

// The space between cards (the dock and toolbar background): the editor's panel colour.
wxColour gutterColour()
{
    return wxSystemSettings::GetColour( wxSYS_COLOUR_MENUBAR );
}

// A card's face: the editor's surface colour (white in light, the warm dark surface in dark).
wxColour cardColour()
{
    return wxSystemSettings::GetColour( wxSYS_COLOUR_WINDOW );
}

// A card's 1 px outline: halfway between the strong line colour and the card face, which is about
// the theme's quiet line colour (--line) without adding a colour of our own.
// KICLOUD: A21 (docs/patches.md, FEATURE_LOOKS.md section 11): when the gutter has the card's own
// colour (the light theme since A19, where both are white), only the outline tells a card from the
// space round it, so it is the strong line colour itself (BTNSHADOW, --line-strong #d6d0c5, the
// colour of the page's field borders) instead of the paler halfway colour (#eae7e2). In dark the
// gutter (#221f1c) differs from the card (#2a2723), so the outline stays the halfway colour.
// Result: the colour; reads the system colour table at every call (a theme switch takes effect at
// the next paint). No state changes.
wxColour cardLineColour()
{
    const wxColour strong = wxSystemSettings::GetColour( wxSYS_COLOUR_BTNSHADOW );
    const wxColour face = cardColour();

    if( gutterColour() == face )
        return strong;

    return wxColour( ( strong.Red() + face.Red() ) / 2, ( strong.Green() + face.Green() ) / 2,
                     ( strong.Blue() + face.Blue() ) / 2 );
}

// wxAuiManager::Repaint() moves the DC's origin to the frame's client-area origin (the menu bar's
// height) before it asks the dock art to paint, because on desktop ports a frame's client DC starts
// at the window's corner. The wx port's client DC already starts at the client area
// (src/wasm/dcclient.cpp), so the offset counted twice: every dock part (pane borders, captions,
// sashes) was painted one menu-bar height (18 px) too low, mostly hidden under the panes. While it
// exists, this object puts the origin back where the port's DC already is, and restores it when it
// goes out of scope. It changes nothing when the origin is not the client-area origin (for example
// a frame without a menu bar, or a port that no longer adds the offset).
class CLIENT_ORIGIN_FIX
{
public:
    CLIENT_ORIGIN_FIX( wxDC& aDc, wxWindow* aFrame ) :
            m_dc( aDc ),
            m_saved( aDc.GetDeviceOrigin() ),
            m_restore( false )
    {
        if( !aFrame || !aFrame->IsTopLevel() )
            return;

        // Not in the 3D viewer yet (its chrome is LOOK.9): the 3D picture is very slightly see-through
        // in the page, and its approved parity pictures include a faint line of the misplaced dock
        // drawing under it, which the fix would remove (FEATURE_LOOKS.md §5).
        if( aFrame->GetName().StartsWith( VIEWER3D_FRAMENAME ) )
            return;

        const wxPoint origin = aFrame->GetClientAreaOrigin();

        if( origin != wxPoint( 0, 0 ) && m_saved == origin )
        {
            m_dc.SetDeviceOrigin( 0, 0 );
            m_restore = true;
        }
    }

    ~CLIENT_ORIGIN_FIX()
    {
        if( m_restore )
            m_dc.SetDeviceOrigin( m_saved.x, m_saved.y );
    }

private:
    wxDC&   m_dc;
    wxPoint m_saved;
    bool    m_restore;
};

// Paints a filled rectangle in one colour (no outline).
void fillRect( wxDC& aDc, const wxRect& aRect, const wxColour& aColour )
{
    aDc.SetPen( *wxTRANSPARENT_PEN );
    aDc.SetBrush( wxBrush( aColour ) );
    aDc.DrawRectangle( aRect );
}

// Paints a card: a rounded rectangle on the card colour with a 1 px outline.
void drawCard( wxDC& aDc, const wxRect& aRect, double aRadius )
{
    aDc.SetPen( wxPen( cardLineColour() ) );
    aDc.SetBrush( wxBrush( cardColour() ) );
    aDc.DrawRoundedRectangle( aRect, aRadius );
}
} // namespace
#endif

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

#ifdef __EMSCRIPTEN__
    // KICLOUD: LOOK.4 a side (vertical) toolbar's cells are wider so its card has room round the
    // buttons; the height, and so the number of buttons that fit, is unchanged. wxAuiToolBar
    // centres each cell, so the icons stay centred in the column.
    if( m_flags & wxAUI_TB_VERTICAL )
        width += 2 * aWindow->FromDIP( RAIL_EXTRA );
#endif

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
    //
    // KICLOUD: LOOK.4 / LOOK.3 the shapes: on a side toolbar the fill is a rounded square (radius
    // 10) the height of the cell and 2 DIP in from each side of the (wider, see GetToolSize) cell;
    // on a top toolbar it is a circle 2 DIP inside the cell, so it sits inside the button group's
    // pill (DrawBackground) with a little of the pill showing round it.
    if( !( aItem.GetState() & wxAUI_BUTTON_STATE_DISABLED ) )
    {
        const int     state = aItem.GetState();
        const bool    active = ( state & wxAUI_BUTTON_STATE_CHECKED ) || ( state & wxAUI_BUTTON_STATE_PRESSED );
        const bool    hover = ( state & wxAUI_BUTTON_STATE_HOVER ) || aItem.IsSticky();
        const wxColour soft = wxSystemSettings::GetColour( wxSYS_COLOUR_MENUHILIGHT );
        const wxColour accent = wxSystemSettings::GetColour( wxSYS_COLOUR_HIGHLIGHT );
        const bool    vertical = ( m_flags & wxAUI_TB_VERTICAL ) != 0;
        wxRect        r = aRect;
        double        radius;

        if( vertical )
        {
            r.Deflate( aWindow->FromDIP( 2 ), 0 );
            radius = std::min( aWindow->FromDIP( RAIL_HOVER_RADIUS ), std::min( r.width, r.height ) / 2 );
        }
        else
        {
            r.Deflate( aWindow->FromDIP( 2 ) );
            radius = std::min( r.width, r.height ) / 2.0;
        }

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


// KICLOUD: LOOK.4 / LOOK.3 the toolbar's background, painted before its buttons (wxAuiToolBar::
// OnPaint calls this, then DrawButton/DrawSeparator for each item).
//  - Everything is first painted in the gutter (panel) colour, as B1.20 did.
//  - A side (vertical) toolbar then gets one card: a rounded rectangle RAIL_INSET in from the
//    window's left and right edges, covering the whole column from just below the top toolbars to
//    just above the message panel.
//  - A top (horizontal) toolbar gets one pill per run of buttons: consecutive normal, check or
//    radio buttons that are on screen. A separator, a spacer, an embedded control (the Track/Via/
//    Grid/Zoom choices) or the end of the visible part ends a run; a lone button gets its own pill.
//    The pill covers exactly the buttons' cells, which wxAuiToolBar placed, so no button moves.
void WX_AUI_TOOLBAR_ART::DrawPlainBackground( wxDC& aDc, wxWindow* aWindow, const wxRect& aRect )
{
    wxRect r = aRect;
    r.height++;
    fillRect( aDc, r, gutterColour() );

    if( m_flags & wxAUI_TB_VERTICAL )
    {
        wxRect card = aRect;
        card.Deflate( aWindow->FromDIP( RAIL_INSET ) );

        if( card.width > 0 && card.height > 0 )
            drawCard( aDc, card, aWindow->FromDIP( RAIL_RADIUS ) );

        return;
    }

    wxAuiToolBar* toolbar = wxDynamicCast( aWindow, wxAuiToolBar );

    if( !toolbar )
        return;

    wxRect run;           // the cells of the current run of buttons (empty: no run open)

    auto closeRun =
            [&]()
            {
                if( !run.IsEmpty() )
                    drawCard( aDc, run, run.height / 2.0 );

                run = wxRect();
            };

    for( size_t i = 0; i < toolbar->GetToolCount(); ++i )
    {
        wxAuiToolBarItem* item = toolbar->FindToolByIndex( (int) i );
        wxSizerItem*      sizerItem = item ? item->GetSizerItem() : nullptr;
        const int         kind = item ? item->GetKind() : wxITEM_SEPARATOR;
        const bool        isButton = kind == wxITEM_NORMAL || kind == wxITEM_CHECK
                                     || kind == wxITEM_RADIO;

        if( !isButton || !sizerItem || !sizerItem->IsShown()
                || !toolbar->GetToolFitsByIndex( (int) i ) )
        {
            closeRun();
            continue;
        }

        const wxRect cell = sizerItem->GetRect();

        if( run.IsEmpty() )
            run = cell;
        else
            run.Union( cell );
    }

    closeRun();
}


// KICLOUD: LOOK.4 / LOOK.3 a separator. On a side toolbar: a 1 px line across the card, stopping
// SEPARATOR_INSET short of each card edge. On a top toolbar: nothing, so the separator's space is
// the gap between two pills (DrawPlainBackground).
void WX_AUI_TOOLBAR_ART::DrawSeparator( wxDC& aDc, wxWindow* aWindow, const wxRect& aRect )
{
    if( !( m_flags & wxAUI_TB_VERTICAL ) )
        return;

    const int left = aWindow->FromDIP( RAIL_INSET ) + aWindow->FromDIP( SEPARATOR_INSET );
    const int right = aWindow->GetClientSize().x - left;

    if( right <= left )
        return;

    wxRect r( left, aRect.y + aRect.height / 2, right - left, aWindow->FromDIP( 1 ) );
    fillRect( aDc, r, cardLineColour() );
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

    // KICLOUD: LOOK.4 a wider pane border (the default is 1 pixel): wxAUI keeps this much space round
    // every pane that has a border (KiCad's drawing area and most side panels), and DrawBorder
    // paints it as the gutter round the drawing area or as a side panel's card. It is read at every
    // layout, so it takes effect at the frame's first layout. Panes without a border (toolbars, the
    // message panel) are not affected. (KICLOUD: A21, comment refreshed: the PCB editor's
    // Appearance and Selection Filter panels have a border since A17, so they are affected.)
    //
    // The 3D viewer keeps wx's 1 px border for now (its chrome is a later step, LOOK.9): its frame
    // opens at a fixed size, so a wider border would make its 3D picture smaller, and resizing the
    // window back moves the camera by a hair. This art is created inside the frame's constructor
    // (EDA_BASE_FRAME::commonInit), after wx listed the frame as the newest top-level window, so
    // that window's name tells which frame this art belongs to.
    const wxWindowList::compatibility_iterator newest = wxTopLevelWindows.GetLast();
    const bool viewer3D = newest && newest->GetData()
                          && newest->GetData()->GetName().StartsWith( VIEWER3D_FRAMENAME );

    if( !viewer3D )
        SetMetric( wxAUI_DOCKART_PANE_BORDER_SIZE, wxWindow::FromDIP( PANE_BORDER, nullptr ) );
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
    m_activeCaptionTextColour = text;
    m_inactiveCaptionTextColour = text;

    // KICLOUD: LOOK.4 a caption is the top row of its panel's card, so it takes the card colour
    // (B1.20 used the panel colour, when the panels were flat).
    const wxColour card = cardColour();
    m_activeCaptionColour = card;
    m_activeCaptionGradientColour = card;
    m_inactiveCaptionColour = card;
    m_inactiveCaptionGradientColour = card;
}


// KICLOUD: LOOK.4 the space wxAUI keeps round a bordered pane (PANE_BORDER wide, see the
// constructor). aRect is the pane's whole rectangle including that border; the pane's own window
// covers aRect minus the border, and wxAUI has already painted the pane's caption inside it, so
// this paints only the border strips and never the inside.
//  - The drawing area's pane (named DRAWING_AREA_PANE): the strips are gutter, with a 1 px line
//    just outside the canvas. The canvas itself (a WebGL surface) is never painted over, clipped or
//    rounded: the picture inside it stays exactly as before (FEATURE_LOOKS.md §5).
//  - Any other bordered pane (Properties, Hierarchy, library trees, ...): a rounded card whose
//    edge is PANE_BORDER - PANEL_PADDING in from the outside, so PANEL_PADDING pixels of card
//    colour show round the panel. The card is painted once per strip with the strip as the clip
//    region, so its fill cannot cover the caption or the panel.
// Toolbars never have a border in KiCad; they keep wx's own drawing.
void WX_AUI_DOCK_ART::DrawBorder( wxDC& aDc, wxWindow* aWindow, const wxRect& aRect,
                                  wxAuiPaneInfo& aPane )
{
    CLIENT_ORIGIN_FIX originFix( aDc, aWindow );
    const int border = GetMetric( wxAUI_DOCKART_PANE_BORDER_SIZE );

    if( aPane.IsToolbar() || border <= 1 || aRect.width <= 2 * border || aRect.height <= 2 * border )
    {
        wxAuiDefaultDockArt::DrawBorder( aDc, aWindow, aRect, aPane );
        return;
    }

    const wxRect inner = wxRect( aRect ).Deflate( border );
    const wxRect strips[4] = {
        wxRect( aRect.x, aRect.y, aRect.width, border ),                          // top
        wxRect( aRect.x, inner.GetBottom() + 1, aRect.width, border ),            // bottom
        wxRect( aRect.x, inner.y, border, inner.height ),                         // left
        wxRect( inner.GetRight() + 1, inner.y, border, inner.height )             // right
    };

    for( const wxRect& strip : strips )
        fillRect( aDc, strip, gutterColour() );

    if( aPane.name == DRAWING_AREA_PANE )
    {
        aDc.SetPen( wxPen( cardLineColour() ) );
        aDc.SetBrush( *wxTRANSPARENT_BRUSH );
        aDc.DrawRectangle( wxRect( inner ).Inflate( 1 ) );
        return;
    }

    const int    padding = std::min( aWindow->FromDIP( PANEL_PADDING ), border - 1 );
    const wxRect card = wxRect( aRect ).Deflate( border - padding );
    const double radius = std::min( aWindow->FromDIP( PANEL_RADIUS ), ( 10 * padding ) / 3 );

    for( const wxRect& strip : strips )
    {
        aDc.SetClippingRegion( strip );
        drawCard( aDc, card, radius );
        aDc.DestroyClippingRegion();
    }
}


// KICLOUD: LOOK.4 a pane caption (the title row of a side panel: "Properties", "Appearance", ...).
// wx paints it first (card colour, see UpdateColoursFromSystem). A panel with a border sits inside
// its card (DrawBorder), so nothing more is needed. A captioned panel without a border has no room
// for a card, so its caption gets rounded top corners instead (KICLOUD: A21, comment refreshed: the
// PCB editor's Appearance and Selection Filter panels, the first users of this, have a border
// since A17, pcb_edit_frame.cpp, so they are cards and no longer come here): the small "ear" between each square
// corner and a quarter circle is repainted in the gutter colour. Only the ears are painted, so the
// caption's text is never covered (CAPTION_RADIUS is small enough to stay clear of it).
void WX_AUI_DOCK_ART::DrawCaption( wxDC& aDc, wxWindow* aWindow, const wxString& aText,
                                   const wxRect& aRect, wxAuiPaneInfo& aPane )
{
    CLIENT_ORIGIN_FIX originFix( aDc, aWindow );
    wxAuiDefaultDockArt::DrawCaption( aDc, aWindow, aText, aRect, aPane );

    if( aPane.HasBorder() || aPane.IsToolbar() || aPane.IsFloating() )
        return;

    const int radius = std::min( aWindow->FromDIP( CAPTION_RADIUS ),
                                 std::min( aRect.width, aRect.height ) / 2 );

    if( radius < 2 )
        return;

    // One ear: the corner point, then the quarter circle from one edge to the other.
    // aSide is -1 for the left corner (the circle's centre is right of the corner), +1 for the right.
    auto drawEar =
            [&]( const wxPoint& aCorner, int aSide )
            {
                const wxPoint centre( aCorner.x - aSide * radius, aCorner.y + radius );
                const int     steps = 8;
                wxPoint       points[steps + 2];

                points[0] = aCorner;

                for( int i = 0; i <= steps; ++i )
                {
                    const double angle = ( M_PI / 2.0 ) * i / steps;   // 0 = on the top edge
                    points[i + 1] = wxPoint( centre.x + aSide * KiROUND( radius * std::sin( angle ) ),
                                             centre.y - KiROUND( radius * std::cos( angle ) ) );
                }

                aDc.DrawPolygon( steps + 2, points );
            };

    aDc.SetPen( *wxTRANSPARENT_PEN );
    aDc.SetBrush( wxBrush( gutterColour() ) );
    drawEar( wxPoint( aRect.x, aRect.y ), -1 );
    drawEar( wxPoint( aRect.GetRight() + 1, aRect.y ), +1 );
}


// KICLOUD: LOOK.4 the sashes (the gaps between panes that can be dragged to resize them), the dock
// background and a caption's close button: wx's own drawing, with the DC origin fixed (see
// CLIENT_ORIGIN_FIX) so they are painted where wxAUI laid them out.
void WX_AUI_DOCK_ART::DrawSash( wxDC& aDc, wxWindow* aWindow, int aOrientation, const wxRect& aRect )
{
    CLIENT_ORIGIN_FIX originFix( aDc, aWindow );
    wxAuiDefaultDockArt::DrawSash( aDc, aWindow, aOrientation, aRect );
}


void WX_AUI_DOCK_ART::DrawBackground( wxDC& aDc, wxWindow* aWindow, int aOrientation,
                                      const wxRect& aRect )
{
    CLIENT_ORIGIN_FIX originFix( aDc, aWindow );
    wxAuiDefaultDockArt::DrawBackground( aDc, aWindow, aOrientation, aRect );
}


void WX_AUI_DOCK_ART::DrawPaneButton( wxDC& aDc, wxWindow* aWindow, int aButton, int aButtonState,
                                      const wxRect& aRect, wxAuiPaneInfo& aPane )
{
    CLIENT_ORIGIN_FIX originFix( aDc, aWindow );
    wxAuiDefaultDockArt::DrawPaneButton( aDc, aWindow, aButton, aButtonState, aRect, aPane );
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
