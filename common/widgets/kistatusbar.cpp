/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2023 Mark Roszko <mark.roszko@gmail.com>
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
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

#include <wx/button.h>
#include <wx/statusbr.h>
#include <wx/gauge.h>
#include <wx/stattext.h>
#include <wx/tokenzr.h>
#include <fmt/format.h>
#include <array>
#include <ranges>
#include <widgets/kistatusbar.h>
#include <widgets/wx_html_report_box.h>
#include <widgets/bitmap_button.h>
#include <widgets/ui_common.h>
#include <pgm_base.h>
#include <background_jobs_monitor.h>
#include <notifications_manager.h>
#include <bitmaps.h>
#include <reporter.h>
#include <dialog_HTML_reporter_base.h>
#include <trace_helpers.h>
#include <wx/dcclient.h>

#ifdef __EMSCRIPTEN__
#include <wx/time.h>   // KICLOUD: LOOK.2, wxGetLocalTimeMillis for the health chip's refresh
#endif


class STATUSBAR_WARNING_REPORTER_DIALOG : public DIALOG_HTML_REPORTER
{
public:
    STATUSBAR_WARNING_REPORTER_DIALOG( wxWindow* aParent, KISTATUSBAR* aStatusBar ) :
            DIALOG_HTML_REPORTER( aParent, wxID_ANY, _( "Messages" ) ),
            m_statusBar( aStatusBar )
    {
        m_clearButton = new wxButton( this, wxID_CLEAR, _( "Clear" ) );
        m_clearButton->Bind( wxEVT_BUTTON,
                             &STATUSBAR_WARNING_REPORTER_DIALOG::onClearButtonClick, this );

        m_sdbSizer->Insert( 0, m_clearButton, 0, wxALL, 5 );
        GetSizer()->Layout();
        GetSizer()->Fit( this );
    }

private:
    void onClearButtonClick( wxCommandEvent& aEvent )
    {
        if( m_statusBar )
            m_statusBar->ClearWarningMessages();

        EndModal( wxID_CLEAR );
    }

private:
    KISTATUSBAR* m_statusBar;
    wxButton*    m_clearButton;
};


KISTATUSBAR::KISTATUSBAR( int aNumberFields, wxWindow* parent, wxWindowID id, STYLE_FLAGS aFlags ) :
        wxStatusBar( parent, id ),
        m_backgroundStopButton( nullptr ),
        m_notificationsButton( nullptr ),
        m_warningButton( nullptr ),
        m_normalFieldsCount( aNumberFields ),
        m_styleFlags( aFlags )
{
#ifdef __WXOSX__
    // we need +1 extra field on OSX to offset from the rounded corner on the right
    // OSX doesn't use resize grippers like the other platforms and the statusbar field
    // includes the rounded part
    int extraFields = 3;
#else
    int extraFields = 2;
#endif

    bool showNotification = ( m_styleFlags & NOTIFICATION_ICON );
    bool showCancel = ( m_styleFlags & CANCEL_BUTTON );
    bool showWarning = ( m_styleFlags & WARNING_ICON );

    if( showCancel )
        extraFields++;

    if( showWarning )
        extraFields++;

    if( showNotification )
        extraFields++;

    SetFieldsCount( aNumberFields + extraFields );

    m_fieldWidths.assign( aNumberFields + extraFields, -1 );

#ifdef __WXOSX__
    // offset from the right edge
    m_fieldWidths[aNumberFields + extraFields - 1] = 10;
#endif

    SetStatusWidths( aNumberFields + extraFields, m_fieldWidths.data() );

    int* styles = new int[aNumberFields + extraFields];

    for( int i = 0; i < aNumberFields + extraFields; i++ )
        styles[i] = wxSB_FLAT;

    SetStatusStyles( aNumberFields + extraFields, styles );
    delete[] styles;

    m_backgroundTxt = new wxStaticText( this, wxID_ANY, wxT( "" ), wxDefaultPosition,
                                        wxDefaultSize, wxALIGN_RIGHT | wxST_NO_AUTORESIZE );

    m_backgroundProgressBar = new wxGauge( this, wxID_ANY, 100, wxDefaultPosition, wxDefaultSize,
                                           wxGA_HORIZONTAL | wxGA_SMOOTH );

    if( showCancel )
    {
        m_backgroundStopButton = new wxButton( this, wxID_ANY, "X", wxDefaultPosition,
                                               wxDefaultSize, wxBU_EXACTFIT );
    }

    if( showNotification )
    {
        m_notificationsButton = new BITMAP_BUTTON( this, wxID_ANY, wxNullBitmap, wxDefaultPosition,
                                                   wxDefaultSize, wxBU_EXACTFIT );

        m_notificationsButton->SetPadding( 0 );
        m_notificationsButton->SetBitmap( KiBitmapBundle( BITMAPS::notifications ) );
        m_notificationsButton->SetShowBadge( true );
        m_notificationsButton->SetBitmapCentered( true );

        m_notificationsButton->Bind( wxEVT_BUTTON, &KISTATUSBAR::onNotificationsIconClick, this );
    }

    if( showWarning )
    {
        m_warningButton = new BITMAP_BUTTON( this, wxID_ANY, wxNullBitmap, wxDefaultPosition,
                                             wxDefaultSize, wxBU_EXACTFIT );

        m_warningButton->SetPadding( 0 );
        m_warningButton->SetBitmap( KiBitmapBundle( BITMAPS::small_warning ) );
        m_warningButton->SetBitmapCentered( true );
        m_warningButton->SetToolTip( _( "View load messages" ) );
        m_warningButton->Hide();

        m_warningButton->Bind( wxEVT_BUTTON, &KISTATUSBAR::onLoadWarningsIconClick, this );
    }

    Bind( wxEVT_SIZE, &KISTATUSBAR::onSize, this );
    m_backgroundProgressBar->Bind( wxEVT_LEFT_DOWN, &KISTATUSBAR::onBackgroundProgressClick, this );

    HideBackgroundProgressBar();
    Layout();
}


KISTATUSBAR::~KISTATUSBAR()
{
    if( m_notificationsButton )
        m_notificationsButton->Unbind( wxEVT_BUTTON, &KISTATUSBAR::onNotificationsIconClick, this );

    if( m_warningButton )
        m_warningButton->Unbind( wxEVT_BUTTON, &KISTATUSBAR::onLoadWarningsIconClick, this );

    Unbind( wxEVT_SIZE, &KISTATUSBAR::onSize, this );
    m_backgroundProgressBar->Unbind( wxEVT_LEFT_DOWN, &KISTATUSBAR::onBackgroundProgressClick,
                                     this );
}


void KISTATUSBAR::onNotificationsIconClick( wxCommandEvent& aEvent )
{
    wxCHECK( m_notificationsButton, /* void */ );
    wxPoint pos = m_notificationsButton->GetScreenPosition();

    wxRect r;
    if( std::optional<int> idx = fieldIndex( FIELD::NOTIFICATION ) )
    {
        GetFieldRect( m_normalFieldsCount + *idx, r );
        pos.x += r.GetWidth();
    }

    Pgm().GetNotificationsManager().ShowList( this, pos );
}


void KISTATUSBAR::onBackgroundProgressClick( wxMouseEvent& aEvent )
{
    wxPoint pos = m_backgroundProgressBar->GetScreenPosition();

    wxRect r;
    if( std::optional<int> idx = fieldIndex( FIELD::BGJOB_GAUGE ) )
    {
        GetFieldRect( m_normalFieldsCount + *idx, r );
        pos.x += r.GetWidth();
    }

    Pgm().GetBackgroundJobMonitor().ShowList( this, pos );
}


void KISTATUSBAR::onSize( wxSizeEvent& aEvent )
{
    layoutControls();
}


void KISTATUSBAR::layoutControls()
{
    constexpr int padding = 5;

    wxRect r;
    int sbField = m_normalFieldsCount + *fieldIndex( FIELD::BGJOB_LABEL );

    if( sbField >= 0 && sbField < GetFieldsCount() )
    {
        GetFieldRect( m_normalFieldsCount + *fieldIndex( FIELD::BGJOB_LABEL ), r );
        int x = r.GetLeft();
        int y = r.GetTop();
        int textHeight = KIUI::GetTextSize( wxT( "bp" ), this ).y;

        if( r.GetHeight() > textHeight )
            y += ( r.GetHeight() - textHeight ) / 2;

        m_backgroundTxt->SetPosition( { x, y } );
        m_backgroundTxt->SetSize( r.GetWidth(), textHeight );
        updateBackgroundText();
    }

    sbField = m_normalFieldsCount + *fieldIndex( FIELD::BGJOB_GAUGE );

    if( sbField >= 0 && sbField < GetFieldsCount() )
    {
        GetFieldRect( m_normalFieldsCount + *fieldIndex( FIELD::BGJOB_GAUGE ), r );
        int x = r.GetLeft();
        int y = r.GetTop();
        int w = r.GetWidth();
        int h = r.GetHeight();
        wxSize buttonSize( 0, 0 );

        if( m_backgroundStopButton )
        {
            buttonSize = m_backgroundStopButton->GetEffectiveMinSize();
            m_backgroundStopButton->SetPosition( { x + w - buttonSize.GetWidth(), y } );
            m_backgroundStopButton->SetSize( buttonSize.GetWidth(), h );
            buttonSize.x += padding;
        }

        m_backgroundProgressBar->SetPosition( { x + padding, y } );
        m_backgroundProgressBar->SetSize( w - buttonSize.GetWidth() - padding, h );

        if( m_notificationsButton )
        {
            sbField = m_normalFieldsCount + *fieldIndex( FIELD::NOTIFICATION );

            if( sbField >= 0 && sbField < GetFieldsCount() )
            {
                GetFieldRect( m_normalFieldsCount + *fieldIndex( FIELD::NOTIFICATION ), r );
                x = r.GetLeft();
                y = r.GetTop();
                h = r.GetHeight();
                buttonSize = m_notificationsButton->GetEffectiveMinSize();
                m_notificationsButton->SetPosition( { x, y } );
                m_notificationsButton->SetSize( buttonSize.GetWidth() + 6, h );
            }
        }
    }

    if( m_warningButton )
    {
        sbField = m_normalFieldsCount + *fieldIndex( FIELD::WARNING );

        if( sbField >= 0 && sbField < GetFieldsCount() )
        {
            GetFieldRect( m_normalFieldsCount + *fieldIndex( FIELD::WARNING ), r );
            int x = r.GetLeft();
            int y = r.GetTop();
            int h = r.GetHeight();
            wxSize buttonSize = m_warningButton->GetEffectiveMinSize();
            m_warningButton->SetPosition( { x, y } );
            m_warningButton->SetSize( buttonSize.GetWidth() + 6, h );
        }
    }
}


void KISTATUSBAR::ShowBackgroundProgressBar( bool aCancellable )
{
    m_backgroundProgressBar->Show();

    if( m_backgroundStopButton )
        m_backgroundStopButton->Show( aCancellable );

    updateAuxFieldWidths();
}


void KISTATUSBAR::HideBackgroundProgressBar()
{
    m_backgroundProgressBar->Hide();

    if( m_backgroundStopButton )
        m_backgroundStopButton->Hide();

    updateAuxFieldWidths();
}


void KISTATUSBAR::SetBackgroundProgress( int aAmount )
{
    int range = m_backgroundProgressBar->GetRange();

    if( aAmount > range )
        aAmount = range;

    m_backgroundProgressBar->SetValue( aAmount );
}


void KISTATUSBAR::SetBackgroundProgressMax( int aAmount )
{
    m_backgroundProgressBar->SetRange( aAmount );
}


void KISTATUSBAR::SetBackgroundStatusText( const wxString& aTxt )
{
    m_backgroundRawText = aTxt;
    updateBackgroundText();

    // When there are multiple normal fields, the last normal field (typically used for
    // file watcher status on Windows) can visually overlap with the background job label
    // since both have variable width. Save and clear that field when showing background
    // text, and restore it when the background text is cleared.
    if( m_normalFieldsCount > 1 )
    {
        int adjacentField = m_normalFieldsCount - 1;

        if( !aTxt.empty() )
        {
            m_savedStatusText = GetStatusText( adjacentField );
            SetStatusText( wxEmptyString, adjacentField );
        }
        else if( !m_savedStatusText.empty() )
        {
            SetStatusText( m_savedStatusText, adjacentField );
            m_savedStatusText.clear();
        }
    }
}


void KISTATUSBAR::updateAuxFieldWidths()
{
    if( m_fieldWidths.empty() )
        return;

    if( std::optional<int> idx = fieldIndex( FIELD::BGJOB_LABEL ) )
        m_fieldWidths[m_normalFieldsCount + *idx] = -2;

    if( std::optional<int> idx = fieldIndex( FIELD::BGJOB_GAUGE ) )
        m_fieldWidths[m_normalFieldsCount + *idx] = 75;

    if( std::optional<int> idx = fieldIndex( FIELD::BGJOB_CANCEL ) )
    {
        m_fieldWidths[m_normalFieldsCount + *idx] =
                m_backgroundStopButton && m_backgroundStopButton->IsShown() ? 20 : 0;
    }

    if( std::optional<int> idx = fieldIndex( FIELD::WARNING ) )
    {
        m_fieldWidths[m_normalFieldsCount + *idx] =
                m_warningButton && m_warningButton->IsShown() ? 20 : 0;
    }

    if( std::optional<int> idx = fieldIndex( FIELD::NOTIFICATION ) )
    {
        m_fieldWidths[m_normalFieldsCount + *idx] =
                m_notificationsButton && m_notificationsButton->IsShown() ? 20 : 0;
    }

    SetStatusWidths( static_cast<int>( m_fieldWidths.size() ), m_fieldWidths.data() );
    layoutControls();
    updateBackgroundText();
}


void KISTATUSBAR::updateBackgroundText()
{
    wxRect r;

    if( !GetFieldRect( m_normalFieldsCount + *fieldIndex( FIELD::BGJOB_LABEL ), r ) )
        return;

    wxString text = m_backgroundRawText;

    if( !text.empty() && r.GetWidth() > 4 )
    {
        wxClientDC dc( this );
        int margin = KIUI::GetTextSize( wxT( "XX" ), this ).x;
        text = wxControl::Ellipsize( text, dc, wxELLIPSIZE_END, std::max( 0, r.GetWidth() - margin ) );
    }

    m_backgroundTxt->SetLabel( text );
}


void KISTATUSBAR::SetNotificationCount( int aCount )
{
    wxCHECK( m_notificationsButton, /* void */ );
    wxString cnt = "";

    if( aCount > 0 )
        cnt = fmt::format( "{}", aCount );

    m_notificationsButton->SetBadgeText( cnt );

    // force a repaint or it wont until it gets activity
    Refresh();
}


void KISTATUSBAR::AddWarningMessages( const wxString& aSource, const wxString& aMessages )
{
    {
        std::lock_guard<std::mutex> lock( m_warningMutex );

        wxStringTokenizer tokenizer( aMessages, wxS( "\n" ), wxTOKEN_STRTOK );

        while( tokenizer.HasMoreTokens() )
        {
            LOAD_MESSAGE msg;
            msg.message = tokenizer.GetNextToken();
            msg.severity = RPT_SEVERITY_WARNING;  // Default to warning for font substitutions
            m_warningMessages[aSource].push_back( msg );
        }
    }

    updateWarningUI();
}


void KISTATUSBAR::AddWarningMessages( const wxString& aSource, const std::vector<LOAD_MESSAGE>& aMessages )
{
    wxLogTrace( traceLibraries, "KISTATUSBAR::AddWarningMessages: this=%p, count=%zu",
                this, aMessages.size() );

    if( aMessages.empty() )
        return;

    size_t totalMessageCount = 0;

    {
        std::lock_guard<std::mutex> lock( m_warningMutex );
        m_warningMessages[aSource].insert( m_warningMessages[aSource].end(), aMessages.begin(), aMessages.end() );

        for( const auto& [source, messages] : m_warningMessages )
            totalMessageCount += messages.size();
    }

    wxLogTrace( traceLibraries, "  -> total messages now=%zu", totalMessageCount );

    // Update UI on main thread
    wxLogTrace( traceLibraries, "  -> calling CallAfter for updateWarningUI" );
    CallAfter( [this]() { updateWarningUI(); } );
}


size_t KISTATUSBAR::GetLoadWarningCount() const
{
    std::lock_guard<std::mutex> lock( m_warningMutex );

    size_t count = 0;

    for( const auto& [source, messages] : m_warningMessages )
        count += messages.size();

    return count;
}


void KISTATUSBAR::updateWarningUI()
{
    wxLogTrace( traceLibraries, "KISTATUSBAR::updateWarningUI: this=%p, m_warningButton=%p",
                this, m_warningButton );

    if( !m_warningButton )
    {
        wxLogTrace( traceLibraries, "  -> no warning button, returning early" );
        return;
    }

    size_t messageCount;
    {
        std::lock_guard<std::mutex> lock( m_warningMutex );

        messageCount = 0;

        for( const std::vector<LOAD_MESSAGE>& messages : m_warningMessages | std::views::values )
            messageCount += messages.size();
    }

    wxLogTrace( traceLibraries, "  -> message count=%zu, showing button=%s",
                messageCount, messageCount > 0 ? "true" : "false" );

    m_warningButton->Show( messageCount > 0 );
    m_warningButton->SetShowBadge( messageCount > 0 );
    updateAuxFieldWidths();

    if( messageCount > 0 )
    {
        m_warningButton->SetToolTip( wxString::Format( _( "View %zu message(s)" ), messageCount ) );

        // Show count badge on the warning button
        wxString badgeText = messageCount > 99
                ? wxString( "99+" )
                : wxString::Format( wxS( "%zu" ), messageCount );
        m_warningButton->SetBadgeText( badgeText );

        wxLogTrace( traceLibraries, "  -> badge set to '%s'", badgeText );
    }
    else
    {
        m_warningButton->SetBadgeText( wxEmptyString );
        m_warningButton->SetToolTip( _( "View messages" ) );
    }

    Layout();
    Refresh();
}


void KISTATUSBAR::ClearWarningMessages( const wxString& aSource )
{
    {
        std::lock_guard<std::mutex> lock( m_warningMutex );

        if( aSource.IsEmpty() )
            m_warningMessages.clear();
        else if( auto it = m_warningMessages.find( aSource ); it != m_warningMessages.end() )
                m_warningMessages.erase( it );
    }

    updateWarningUI();
}


void KISTATUSBAR::onLoadWarningsIconClick( wxCommandEvent& aEvent )
{
    // Copy messages under lock to avoid holding lock during modal dialog
    std::unordered_map<wxString, std::vector<LOAD_MESSAGE>> messages;
    {
        std::lock_guard<std::mutex> lock( m_warningMutex );
        messages = m_warningMessages;
    }

    if( messages.empty() )
        return;

    STATUSBAR_WARNING_REPORTER_DIALOG dlg( GetParent(), this );

    for( const std::vector<LOAD_MESSAGE>& source : std::views::values( messages ) )
        for( const LOAD_MESSAGE& msg : source )
            dlg.m_Reporter->Report( msg.message, msg.severity );

    dlg.m_Reporter->Flush();
    dlg.ShowModal();
}

void KISTATUSBAR::SetEllipsedTextField( const wxString& aText, int aFieldId )
{
    wxRect       fieldRect;
    int          width = -1;
    wxString     etext = aText;

    // Only GetFieldRect() returns the current size for variable size fields
    // Other methods return -1 for the width of these fields.
    if( GetFieldRect( aFieldId, fieldRect ) )
        width = fieldRect.GetWidth();

    if( width > 20 )
    {
        wxClientDC dc( this );

        // Gives a margin to the text to be sure it is not clamped at its end
        int margin = KIUI::GetTextSize( wxT( "XX" ), this ).x;
        etext = wxControl::Ellipsize( etext, dc, wxELLIPSIZE_MIDDLE, width - margin );
    }

    SetStatusText( etext, aFieldId );
}


std::optional<int> KISTATUSBAR::fieldIndex( FIELD aField ) const
{
    switch( aField )
    {
    case FIELD::BGJOB_LABEL:  return 0;
    case FIELD::BGJOB_GAUGE:  return 1;
    case FIELD::BGJOB_CANCEL:
    {
        if( m_styleFlags & CANCEL_BUTTON )
            return 2;

        break;
    }
    case FIELD::WARNING:
    {
        if( m_styleFlags & WARNING_ICON )
        {
            int offset = 2;

            if( m_styleFlags & CANCEL_BUTTON )
                offset++;

            return offset;
        }

        break;
    }
    case FIELD::NOTIFICATION:
    {
        if( m_styleFlags & NOTIFICATION_ICON )
        {
            int offset = 2;

            if( m_styleFlags & CANCEL_BUTTON )
                offset++;

            if( m_styleFlags & WARNING_ICON )
                offset++;

            return offset;
        }

        break;
    }
    }

    return std::nullopt;
}


#ifdef __EMSCRIPTEN__
// KICLOUD: LOOK.2, the browser editor's status bar as labels (docs/future-features/FEATURE_LOOKS.md
// section 4.2, docs/patches.md). Everything below exists only in the browser build and only acts
// once a drawing frame called EnableLabels(); see the comment at EnableLabels in kistatusbar.h for
// what the bar shows. The bar is painted on the canvas by this class (the wx port has no DOM status
// bar), with colours from the port's system colour table (the editor's theme tokens):
//   surface  wxSYS_COLOUR_BTNFACE          the bar
//   panel    wxSYS_COLOUR_MENUBAR          the units switch's track, the attention chip
//   line     wxSYS_COLOUR_INACTIVECAPTION  the hairline on top
//   strong   wxSYS_COLOUR_BTNSHADOW        the dots between counts, the attention chip's edge
//   text     wxSYS_COLOUR_WINDOWTEXT, muted wxSYS_COLOUR_GRAYTEXT
//   accent   wxSYS_COLOUR_HOTLIGHT on wxSYS_COLOUR_MENUHILIGHT (accent-soft): the calm chip

namespace
{
constexpr int LABELS_HEIGHT = 34;       // the bar's height, as in the approved mockups
constexpr int LABELS_PAD = 14;          // space at the bar's two ends
constexpr int GROUP_GAP = 14;           // space between two pieces
constexpr int ITEM_GAP = 7;             // space on each side of the dot between two counts
constexpr int CHIP_HEIGHT = 22;         // the health chip and the units switch
constexpr int CHIP_PAD = 10;            // the chip's inner space left and right
constexpr int ICON_SIZE = 12;           // the chip's check mark or warning triangle
constexpr int SEGMENT_PAD = 8;          // the inner space of one units segment
constexpr int TRACK_PAD = 2;            // between the units track and its segments
constexpr int TEXT_MAX = 280;           // a hint text never takes more than this
constexpr int MIN_PIECE = 28;           // a piece narrower than this is left out (tooltip only)
constexpr int HEALTH_PERIOD_MS = 1000;  // the chip is recomputed at most this often while idle
constexpr int HEALTH_SOON_MS = 150;     // ... and this soon after the message panel changed

const char* const UNIT_LABELS[3] = { "mm", "in", "mil" };


// The health sources registered by AddHealthSource. A function-local static, so a source
// registered by another file's start-up code never finds the list unconstructed.
std::vector<KISTATUSBAR::HEALTH_SOURCE>& healthSources()
{
    static std::vector<KISTATUSBAR::HEALTH_SOURCE> sources;
    return sources;
}


wxString dotText()
{
    return wxString::FromUTF8( "\xC2\xB7" );    // "·"
}


wxString ellipsisText()
{
    return wxString::FromUTF8( "\xE2\x80\xA6" );    // "…"
}


bool sameHealth( const std::optional<KISTATUSBAR::HEALTH>& a,
                 const std::optional<KISTATUSBAR::HEALTH>& b )
{
    if( a.has_value() != b.has_value() )
        return false;

    return !a || ( a->text == b->text && a->tooltip == b->tooltip && a->ok == b->ok
                   && a->replacesItem == b->replacesItem );
}
} // namespace


void KISTATUSBAR::AddHealthSource( HEALTH_SOURCE aSource )
{
    healthSources().push_back( std::move( aSource ) );
}


// Turn labels mode on: from now on this class paints the bar (onLabelsPaint), shows tooltips
// (onLabelsMotion), handles clicks on the units switch (onLabelsLeftDown) and keeps the health
// chip current (onLabelsIdle, m_healthTimer). The handlers are bound to this window, so they live
// exactly as long as the bar. The field widths are re-applied in the labels layout (see
// SetStatusWidths), and the bar takes its 34 px height (DoGetBestSize); the frame lays itself out
// with that height on the posted size event.
void KISTATUSBAR::EnableLabels( std::function<void( int )> aOnUnits )
{
    if( m_labels )
        return;

    m_labels = true;
    m_onUnits = std::move( aOnUnits );

    // The bar is painted in full by onLabelsPaint: no background erase first (no flicker).
    SetBackgroundStyle( wxBG_STYLE_PAINT );

    Bind( wxEVT_PAINT, &KISTATUSBAR::onLabelsPaint, this );
    Bind( wxEVT_MOTION, &KISTATUSBAR::onLabelsMotion, this );
    Bind( wxEVT_LEAVE_WINDOW, &KISTATUSBAR::onLabelsLeave, this );
    Bind( wxEVT_LEFT_DOWN, &KISTATUSBAR::onLabelsLeftDown, this );
    Bind( wxEVT_IDLE, &KISTATUSBAR::onLabelsIdle, this );

    m_healthTimer.SetOwner( this );
    Bind( wxEVT_TIMER, &KISTATUSBAR::onHealthTimer, this, m_healthTimer.GetId() );

    SetStatusWidths( static_cast<int>( m_fieldWidths.size() ), m_fieldWidths.data() );

    InvalidateBestSize();
    SetSize( wxDefaultCoord, wxDefaultCoord, wxDefaultCoord, LABELS_HEIGHT );
    PostSizeEventToParent();
}


// Store the message panel's items (called by EDA_MSG_PANEL whenever its items change) and repaint
// when they differ. The health chip is recomputed soon after (once per burst of changes): an edit
// that changes the board usually also refreshes the message panel.
void KISTATUSBAR::SetMessageItems( const std::vector<std::pair<wxString, wxString>>& aItems )
{
    if( !m_labels || aItems == m_messageItems )
        return;

    m_messageItems = aItems;
    Refresh( false );

    if( !m_healthTimer.IsRunning() )
        m_healthTimer.StartOnce( HEALTH_SOON_MS );
}


// Select a units segment (EDA_DRAW_FRAME::DisplayUnitsMsg calls this on every status update, so
// it repaints only when the units really changed). The coordinate texts change width with the
// units, so the remembered widths of the right-hand pieces are dropped.
void KISTATUSBAR::SetUnitsChoice( int aChoice )
{
    if( !m_labels || aChoice == m_unitsChoice )
        return;

    m_unitsChoice = aChoice;
    m_stickyWidths.clear();
    Refresh( false );
}


// In labels mode the fields KiCad writes (0 to m_normalFieldsCount - 1) are not drawn in their own
// columns: field 0 spans the painted area and the others are zero wide. The background-job label
// and gauge take room only while a job runs; the load-warning and notification buttons keep their
// widths. Without labels mode this is wxStatusBar's own SetStatusWidths.
void KISTATUSBAR::SetStatusWidths( int aCount, const int aWidths[] )
{
    if( !m_labels )
    {
        wxStatusBar::SetStatusWidths( aCount, aWidths );
        return;
    }

    std::vector<int> widths( aWidths, aWidths + aCount );

    for( int i = 0; i < std::min( aCount, m_normalFieldsCount ); ++i )
        widths[i] = ( i == 0 ) ? -1 : 0;

    const bool jobRunning = m_backgroundProgressBar && m_backgroundProgressBar->IsShown();

    if( std::optional<int> idx = fieldIndex( FIELD::BGJOB_LABEL ) )
    {
        if( m_normalFieldsCount + *idx < aCount )
            widths[m_normalFieldsCount + *idx] = jobRunning ? 160 : 0;
    }

    if( std::optional<int> idx = fieldIndex( FIELD::BGJOB_GAUGE ) )
    {
        if( m_normalFieldsCount + *idx < aCount )
            widths[m_normalFieldsCount + *idx] = jobRunning ? 75 : 0;
    }

    wxStatusBar::SetStatusWidths( aCount, widths.data() );
}


// The frame places the status bar at its best height (the wx port's frame asks for it on every
// layout): 34 px in labels mode, wx's text-based height otherwise.
wxSize KISTATUSBAR::DoGetBestSize() const
{
    wxSize best = wxStatusBar::DoGetBestSize();

    if( m_labels )
        best.y = LABELS_HEIGHT;

    return best;
}


// A field's text changed. wx would repaint only that field's column, but in labels mode the texts
// are drawn elsewhere (and a longer text moves its neighbours), so the whole bar is repainted.
void KISTATUSBAR::DoUpdateStatusText( int aField )
{
    if( m_labels )
        Refresh( false );
    else
        wxStatusBar::DoUpdateStatusText( aField );
}


// The right end of the painted area: where field 0 ends and the background-job and warning fields
// (still wx's own columns, with their child controls) begin.
int KISTATUSBAR::labelsRightEdge() const
{
    wxRect r;

    if( !GetFieldRect( 0, r ) )
        return GetClientSize().x;

    return r.GetRight() + 1 + GetBorderX();
}


// Paint the bar (see the layout at EnableLabels in kistatusbar.h). The right-hand pieces are laid
// out first, from the right end: the units switch, zoom, grid, dx/dy and X/Y; each keeps the widest
// width it had since the last resize or units change, so the bar does not jitter while the cursor
// moves. The left-hand pieces share what is left; when it is not enough they are given room in the
// order health chip, message panel items, tool hint, constraint, message, and what does not fit is
// shortened with "…" (its full text is in its tooltip) or left out (its text is in the tooltip of
// the bar's empty space). Every piece drawn is recorded in m_pieces for tooltips and tests.
void KISTATUSBAR::onLabelsPaint( wxPaintEvent& aEvent )
{
    wxPaintDC    dc( this );
    const wxSize size = GetClientSize();

    const wxColour surface = wxSystemSettings::GetColour( wxSYS_COLOUR_BTNFACE );
    const wxColour panel = wxSystemSettings::GetColour( wxSYS_COLOUR_MENUBAR );
    const wxColour line = wxSystemSettings::GetColour( wxSYS_COLOUR_INACTIVECAPTION );
    const wxColour strong = wxSystemSettings::GetColour( wxSYS_COLOUR_BTNSHADOW );
    const wxColour text = wxSystemSettings::GetColour( wxSYS_COLOUR_WINDOWTEXT );
    const wxColour muted = wxSystemSettings::GetColour( wxSYS_COLOUR_GRAYTEXT );
    const wxColour accent = wxSystemSettings::GetColour( wxSYS_COLOUR_HOTLIGHT );
    const wxColour accentSoft = wxSystemSettings::GetColour( wxSYS_COLOUR_MENUHILIGHT );

    const wxFont font = GetFont().IsOk() ? GetFont() : *wxNORMAL_FONT;
    wxFont       bold = font;
    bold.SetWeight( wxFONTWEIGHT_BOLD );
    const wxFont mono( wxFontInfo( font.GetFractionalPointSize() ).Family( wxFONTFAMILY_TELETYPE ) );

    // the bar: surface colour with a hairline on top
    dc.SetPen( *wxTRANSPARENT_PEN );
    dc.SetBrush( wxBrush( surface ) );
    dc.DrawRectangle( 0, 0, size.x, size.y );
    dc.SetPen( wxPen( line ) );
    dc.DrawLine( 0, 0, size.x, 0 );
    dc.SetBackgroundMode( wxTRANSPARENT );

    m_pieces.clear();
    m_unitsRects.clear();

    if( m_stickyForWidth != size.x )
    {
        m_stickyWidths.clear();
        m_stickyForWidth = size.x;
    }

    const int mid = ( size.y + 1 ) / 2;   // +1: the hairline takes the top pixel

    auto field = [&]( int aField ) -> wxString
    {
        return aField < GetFieldsCount() ? GetStatusText( aField ) : wxString();
    };

    auto textWidth = [&]( const wxString& aText, const wxFont& aFont ) -> int
    {
        dc.SetFont( aFont );
        return dc.GetTextExtent( aText ).x;
    };

    // Draw aText vertically centred at x in at most aMax pixels ("…" when shortened); returns the
    // text actually drawn.
    auto drawText = [&]( const wxString& aText, const wxFont& aFont, const wxColour& aColour,
                         int aX, int aMax ) -> wxString
    {
        dc.SetFont( aFont );
        wxString shown = aText;

        if( dc.GetTextExtent( shown ).x > aMax )
            shown = wxControl::Ellipsize( shown, dc, wxELLIPSIZE_END, aMax );

        const int height = dc.GetTextExtent( wxS( "Xg" ) ).y;
        dc.SetTextForeground( aColour );
        dc.DrawText( shown, aX, mid - height / 2 );
        return shown;
    };

    // ---- right-hand pieces, from the right end --------------------------------------------------

    int right = labelsRightEdge() - LABELS_PAD;

    // the units switch: mm | in | mil on the panel-coloured track, the current one raised
    {
        int segmentWidths[3];
        int trackWidth = 2 * TRACK_PAD;

        for( int i = 0; i < 3; ++i )
        {
            segmentWidths[i] = textWidth( UNIT_LABELS[i], bold ) + 2 * SEGMENT_PAD;
            trackWidth += segmentWidths[i];
        }

        const wxRect track( right - trackWidth, mid - CHIP_HEIGHT / 2, trackWidth, CHIP_HEIGHT );

        if( track.x >= LABELS_PAD )
        {
            dc.SetPen( *wxTRANSPARENT_PEN );
            dc.SetBrush( wxBrush( panel ) );
            dc.DrawRoundedRectangle( track, CHIP_HEIGHT / 2.0 );

            int x = track.x + TRACK_PAD;

            for( int i = 0; i < 3; ++i )
            {
                const wxRect segment( x, track.y + TRACK_PAD, segmentWidths[i],
                                      CHIP_HEIGHT - 2 * TRACK_PAD );
                const bool   selected = ( i == m_unitsChoice );

                if( selected )
                {
                    dc.SetPen( *wxTRANSPARENT_PEN );
                    dc.SetBrush( wxBrush( surface ) );
                    dc.DrawRoundedRectangle( segment, segment.height / 2.0 );
                }

                const wxFont& segmentFont = selected ? bold : font;
                const int     labelWidth = textWidth( UNIT_LABELS[i], segmentFont );
                drawText( UNIT_LABELS[i], segmentFont, selected ? text : muted,
                          segment.x + ( segment.width - labelWidth ) / 2, segment.width );

                m_unitsRects.push_back( segment );
                x += segmentWidths[i];
            }

            const wxString tooltip = wxString::Format( _( "Units: %s" ), field( FIELD_UNITS ) )
                                     + wxS( "\n" ) + _( "Click mm, in or mil to change the units" );

            for( int i = 0; i < 3; ++i )
                m_pieces.push_back( { wxS( "unit" ), UNIT_LABELS[i], tooltip, m_unitsRects[i] } );

            m_pieces.push_back( { wxS( "units" ), wxS( "mm | in | mil" ), tooltip, track } );
            right = track.x - GROUP_GAP;
        }
    }

    // zoom, grid, dx/dy and X/Y (the coordinates in the monospace font, X/Y in the text colour)
    struct RIGHT_FIELD
    {
        int           field;
        const char*   kind;
        const wxFont* font;
        bool          strongText;
    };

    const RIGHT_FIELD rightFields[] = { { FIELD_ZOOM, "zoom", &font, false },
                                        { FIELD_GRID, "grid", &font, false },
                                        { FIELD_DELTA, "delta", &mono, false },
                                        { FIELD_CURSOR, "cursor", &mono, true } };

    for( const RIGHT_FIELD& rf : rightFields )
    {
        const wxString value = field( rf.field );

        if( value.IsEmpty() )
            continue;

        int& sticky = m_stickyWidths[rf.field];
        sticky = std::max( sticky, textWidth( value, *rf.font ) );

        if( right - sticky < LABELS_PAD )
            continue;   // no room: the text is in the empty space's tooltip

        const int x = right - sticky;
        drawText( value, *rf.font, rf.strongText ? text : muted, x, sticky );
        m_pieces.push_back( { rf.kind, value, value, wxRect( x, 0, sticky, size.y ) } );
        right = x - GROUP_GAP;
    }

    // ---- left-hand pieces --------------------------------------------------------------------

    const int left = LABELS_PAD;
    int       available = std::max( 0, right + GROUP_GAP - left );

    // the message panel items, without the one the health chip already shows
    std::vector<std::pair<wxString, wxString>> items;
    wxString                                   itemsTooltip;

    for( const auto& [upper, lower] : m_messageItems )
    {
        if( !itemsTooltip.IsEmpty() )
            itemsTooltip << wxS( "\n" );

        itemsTooltip << upper << wxS( ": " ) << lower;

        if( !( m_health && !m_health->replacesItem.IsEmpty() && upper == m_health->replacesItem ) )
            items.emplace_back( upper, lower );
    }

    const int spaceWidth = textWidth( wxS( " " ), font );
    const int dotWidth = textWidth( dotText(), font );
    const int ellipsisWidth = textWidth( ellipsisText(), font );
    int       itemsWidth = 0;

    for( size_t i = 0; i < items.size(); ++i )
    {
        if( i > 0 )
            itemsWidth += 2 * ITEM_GAP + dotWidth;

        itemsWidth += textWidth( items[i].first, font ) + spaceWidth
                      + textWidth( items[i].second, font );
    }

    const int chipWidth = m_health ? ICON_SIZE + 6 + textWidth( m_health->text, bold ) + 2 * CHIP_PAD
                                   : 0;

    // natural widths, then room given by priority: the health chip and the message panel items
    // (the counts) first, then the hints (tool, constraint, message), which shrink first
    enum LEFT_PIECE { CHIP, ITEMS, MESSAGE, TOOL, CONSTRAINT, LEFT_COUNT };
    int natural[LEFT_COUNT] = { chipWidth, itemsWidth,
                                std::min( TEXT_MAX, textWidth( field( FIELD_MESSAGE ), font ) ),
                                std::min( TEXT_MAX, textWidth( field( FIELD_TOOL ), font ) ),
                                std::min( TEXT_MAX, textWidth( field( FIELD_CONSTRAINT ), font ) ) };
    int given[LEFT_COUNT] = { 0, 0, 0, 0, 0 };

    for( LEFT_PIECE piece : { CHIP, ITEMS, TOOL, CONSTRAINT, MESSAGE } )
    {
        if( natural[piece] == 0 )
            continue;

        const int room = std::min( natural[piece], available - GROUP_GAP );

        if( room < std::min( natural[piece], MIN_PIECE ) )
            continue;

        given[piece] = room;
        available -= room + GROUP_GAP;
    }

    int x = left;

    // the health chip: calm (accent on accent-soft, a check mark) or attention (text on the panel
    // colour with a strong edge, a warning triangle)
    if( given[CHIP] > 0 )
    {
        const wxRect chip( x, mid - CHIP_HEIGHT / 2, given[CHIP], CHIP_HEIGHT );
        const wxColour ink = m_health->ok ? accent : text;

        dc.SetPen( m_health->ok ? *wxTRANSPARENT_PEN : wxPen( strong ) );
        dc.SetBrush( wxBrush( m_health->ok ? accentSoft : panel ) );
        dc.DrawRoundedRectangle( chip, CHIP_HEIGHT / 2.0 );

        const int iconX = chip.x + CHIP_PAD;
        const int iconY = mid - ICON_SIZE / 2;
        wxPen     iconPen( ink, 2 );
        iconPen.SetCap( wxCAP_ROUND );
        iconPen.SetJoin( wxJOIN_ROUND );
        dc.SetPen( iconPen );
        dc.SetBrush( *wxTRANSPARENT_BRUSH );

        if( m_health->ok )
        {
            const wxPoint check[3] = { { iconX + 1, iconY + 6 }, { iconX + 4, iconY + 9 },
                                       { iconX + 11, iconY + 2 } };
            dc.DrawLines( 3, check );
        }
        else
        {
            const wxPoint triangle[4] = { { iconX + 6, iconY + 1 }, { iconX + 11, iconY + 11 },
                                          { iconX + 1, iconY + 11 }, { iconX + 6, iconY + 1 } };
            dc.DrawLines( 4, triangle );
            dc.DrawLine( iconX + 6, iconY + 5, iconX + 6, iconY + 7 );
        }

        // KICLOUD: LOOK.2 fix: the room for the text ends CHIP_PAD before the chip's right edge,
        // x + width (wxRect::GetRight() is the last pixel inside, one less): with GetRight() the
        // room was one pixel short of the width measured above, and the chip read "Fully rout…"
        // as soon as the font measured the text without slack (the system-ui font, LOOK.6).
        const int textX = iconX + ICON_SIZE + 6;
        const wxString shown = drawText( m_health->text, bold, ink, textX,
                                         chip.x + chip.width - CHIP_PAD - textX );
        m_pieces.push_back( { wxS( "health" ), shown, m_health->tooltip, chip } );
        x = chip.GetRight() + 1 + GROUP_GAP;
    }

    // the message panel items: "Pads 120 · Vias 30 · ...", label muted, value in the text colour;
    // the items that do not fit end in "…" (all of them are in the tooltip)
    if( given[ITEMS] > 0 )
    {
        const int end = x + given[ITEMS];
        int       cx = x;
        wxString  shown;

        for( size_t i = 0; i < items.size(); ++i )
        {
            const int width = textWidth( items[i].first, font ) + spaceWidth
                              + textWidth( items[i].second, font );
            const int separator = ( i > 0 ) ? 2 * ITEM_GAP + dotWidth : 0;
            const bool last = ( i + 1 == items.size() );

            if( cx + separator + width > end - ( last ? 0 : ellipsisWidth + ITEM_GAP ) )
            {
                if( cx + ITEM_GAP + ellipsisWidth <= end )
                {
                    drawText( ellipsisText(), font, muted, cx + ( i > 0 ? ITEM_GAP : 0 ),
                              ellipsisWidth );
                    shown << ellipsisText();
                }

                break;
            }

            if( i > 0 )
            {
                drawText( dotText(), font, strong, cx + ITEM_GAP, dotWidth );
                cx += separator;
                shown << wxS( " " ) << dotText() << wxS( " " );
            }

            drawText( items[i].first, font, muted, cx, width );
            drawText( items[i].second, font, text,
                      cx + textWidth( items[i].first, font ) + spaceWidth, width );
            shown << items[i].first << wxS( " " ) << items[i].second;
            cx += width;
        }

        m_pieces.push_back( { wxS( "items" ), shown, itemsTooltip,
                              wxRect( x, 0, given[ITEMS], size.y ) } );
        x += given[ITEMS] + GROUP_GAP;
    }

    // the message, the tool hint ("Select item(s)") and the constraint, muted
    const std::pair<LEFT_PIECE, int> hints[] = { { MESSAGE, FIELD_MESSAGE },
                                                 { TOOL, FIELD_TOOL },
                                                 { CONSTRAINT, FIELD_CONSTRAINT } };
    const char* const hintKinds[] = { "message", "tool", "constraint" };

    for( size_t i = 0; i < 3; ++i )
    {
        const auto [piece, fieldId] = hints[i];

        if( given[piece] <= 0 )
            continue;

        const wxString value = field( fieldId );
        const wxString shown = drawText( value, font, muted, x, given[piece] );
        m_pieces.push_back( { hintKinds[i], shown, value, wxRect( x, 0, given[piece], size.y ) } );
        x += given[piece] + GROUP_GAP;
    }

    // the empty space: its tooltip has every text of the bar, so a text left out is still there
    wxString everything;

    for( int i = 0; i < std::min( GetFieldsCount(), static_cast<int>( DRAW_FRAME_FIELDS ) ); ++i )
    {
        if( !field( i ).IsEmpty() )
            everything << field( i ) << wxS( "\n" );
    }

    if( m_health )
        everything << m_health->tooltip << wxS( "\n" );

    everything << itemsTooltip;
    everything.Trim();

    m_pieces.push_back( { wxS( "rest" ), wxEmptyString, everything,
                          wxRect( 0, 0, labelsRightEdge(), size.y ) } );
}


// Show the tooltip of the piece under the pointer (the empty space's tooltip lists every text)
// and the hand cursor over the units switch.
void KISTATUSBAR::onLabelsMotion( wxMouseEvent& aEvent )
{
    const wxPoint pos = aEvent.GetPosition();
    wxString      tip;

    for( const LABEL_PIECE& piece : m_pieces )
    {
        if( piece.rect.Contains( pos ) )
        {
            tip = piece.tooltip;
            break;
        }
    }

    if( GetToolTipText() != tip )
    {
        if( tip.IsEmpty() )
            UnsetToolTip();
        else
            SetToolTip( tip );
    }

    bool overUnits = false;

    for( const wxRect& segment : m_unitsRects )
        overUnits |= segment.Contains( pos );

    SetCursor( overUnits ? wxCursor( wxCURSOR_HAND ) : wxNullCursor );
    aEvent.Skip();
}


void KISTATUSBAR::onLabelsLeave( wxMouseEvent& aEvent )
{
    SetCursor( wxNullCursor );
    aEvent.Skip();
}


// A click on a units segment asks the frame to run KiCad's units action for it (m_onUnits); the
// frame's DisplayUnitsMsg then reports the new units back through SetUnitsChoice.
void KISTATUSBAR::onLabelsLeftDown( wxMouseEvent& aEvent )
{
    for( size_t i = 0; i < m_unitsRects.size(); ++i )
    {
        if( m_unitsRects[i].Contains( aEvent.GetPosition() ) )
        {
            if( m_onUnits )
                m_onUnits( static_cast<int>( i ) );

            return;
        }
    }

    aEvent.Skip();
}


// While the editor is in use, recompute the health chip at most once a second, so a DRC run, an
// ERC run or a marker exclusion shows without a message panel change. Idle events come only after
// other events, so an untouched editor does no work here.
void KISTATUSBAR::onLabelsIdle( wxIdleEvent& aEvent )
{
    aEvent.Skip();

    if( wxGetLocalTimeMillis() - m_lastHealth >= HEALTH_PERIOD_MS )
        refreshHealth();
}


void KISTATUSBAR::onHealthTimer( wxTimerEvent& aEvent )
{
    refreshHealth();
}


// Ask the registered health sources for this bar's frame (the first that knows the frame wins)
// and repaint when the chip changes. A frame that is hidden (another editor tab is shown) or being
// destroyed is skipped; a source that throws counts as "no chip".
void KISTATUSBAR::refreshHealth()
{
    m_lastHealth = wxGetLocalTimeMillis();

    wxWindow* frame = GetParent();

    if( !frame || frame->IsBeingDeleted() || !IsShownOnScreen() )
        return;

    std::optional<HEALTH> health;

    for( const HEALTH_SOURCE& source : healthSources() )
    {
        try
        {
            health = source( frame );
        }
        catch( ... )
        {
            health.reset();
        }

        if( health )
            break;
    }

    if( health && health->text.IsEmpty() )
        health.reset();

    if( !sameHealth( health, m_health ) )
    {
        m_health = health;
        Refresh( false );
    }
}
#endif // __EMSCRIPTEN__
