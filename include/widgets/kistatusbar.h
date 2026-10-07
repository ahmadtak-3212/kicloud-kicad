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

#ifndef KISTATUSBAR_H
#define KISTATUSBAR_H

#include <kicommon.h>
#include <optional>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <widgets/report_severity.h>
#include <wx/statusbr.h>
#include <wx/settings.h> // KICLOUD: B1.20, GetDefaultAttributes below

#ifdef __EMSCRIPTEN__
#include <functional>   // KICLOUD: LOOK.2, the merged status bar's callbacks
#include <utility>
#include <wx/timer.h>

// KICLOUD: LOOK.2. Code outside the fork (the kicloud test hook in wasm/editor/kicloud_tools.cpp)
// compiles its merged-status-bar part only when this is defined, so the same hook also builds
// against a KiCad without this change.
#define KISTATUSBAR_HAS_LABELS 1
#endif

class wxGauge;
class wxButton;
class wxStaticText;
class BITMAP_BUTTON;

/**
 * KISTATUSBAR is a wxStatusBar suitable for Kicad manager.
 * It displays the fields needed by the caller, and room for 4 other fields (see kistatusbar.cpp)
 * Background text (FIELD_OFFSET_BGJOB_TEXT offset id)
 * Background gauge widget (FIELD_OFFSET_BGJOB_GAUGE offset id)
 * Background background stop button (FIELD_OFFSET_BGJOB_CANCEL offset id)
 * Background notifications button (FIELD_OFFSET_NOTIFICATION_BUTTON  offset id)
 */

/**
 * Structure to store a load message with its severity.
 */
struct LOAD_MESSAGE
{
    wxString  message;
    SEVERITY  severity;
};


class KICOMMON_API KISTATUSBAR : public wxStatusBar
{
public:
    enum STYLE_FLAGS : int
    {
        NONE_STYLE        = 0x00,
        NOTIFICATION_ICON = 0x01,
        CANCEL_BUTTON     = 0x02,
        WARNING_ICON      = 0x04,
    };

    static constexpr auto DEFAULT_STYLE =
            static_cast<STYLE_FLAGS>( NOTIFICATION_ICON | CANCEL_BUTTON );

    KISTATUSBAR( int aNumberFields, wxWindow* parent, wxWindowID id,
                 STYLE_FLAGS aFlags = DEFAULT_STYLE );

    ~KISTATUSBAR();

#ifdef __EMSCRIPTEN__
    // KICLOUD: the browser editor's status bar sits on the panel colour with muted text (B1.20,
    // IDEAS.md #8). Default attributes, not SetBackgroundColour, so a theme switch (the wx port's
    // system colour table) repaints it in the new colours.
    wxVisualAttributes GetDefaultAttributes() const override
    {
        wxVisualAttributes attrs = wxStatusBar::GetDefaultAttributes();
        attrs.colBg = wxSystemSettings::GetColour( wxSYS_COLOUR_MENUBAR );
        attrs.colFg = wxSystemSettings::GetColour( wxSYS_COLOUR_GRAYTEXT );
        return attrs;
    }

    // KICLOUD: LOOK.2, the browser editor's status bar as labels (docs/future-features/
    // FEATURE_LOOKS.md section 4.2). A drawing frame (EDA_DRAW_FRAME) turns this "labels" mode on.
    // The bar is then one 34 px row that KISTATUSBAR paints itself:
    //
    //   [health chip] [message panel items: Pads 120 · Vias 30 · ...] [hint texts]   ...   [X/Y]
    //   [dx/dy] [grid] [zoom] [mm | in | mil]  [background job, load warnings: unchanged]
    //
    // The texts are the ones KiCad already writes: the eight fields EDA_DRAW_FRAME fills with
    // SetStatusText (FIELD_* below) and the items of the message panel (EDA_MSG_PANEL), which the
    // drawing frames used to show as a second bar above this one and now hand to SetMessageItems.
    // Anything that does not fit is in a tooltip, so no text KiCad showed is lost. Colours come
    // from the wx port's system colour table, so a theme switch repaints the bar in the new theme.
    // Without labels mode (KiCad's project manager, dialogs) the bar draws as before.

    /// The fields EDA_DRAW_FRAME writes (CreateStatusBar( 8 ), eda_draw_frame.cpp).
    enum DRAW_FRAME_FIELD : int
    {
        FIELD_MESSAGE = 0,      ///< general messages (file loading, ...)
        FIELD_ZOOM = 1,         ///< "Z 1.23"
        FIELD_CURSOR = 2,       ///< "X 12.3400  Y 5.6700"
        FIELD_DELTA = 3,        ///< "dx 1.0000  dy 2.0000  dist 2.2361"
        FIELD_GRID = 4,         ///< "grid 0.1000"
        FIELD_UNITS = 5,        ///< "mm", "inches", "mils"
        FIELD_TOOL = 6,         ///< the current tool's name, e.g. "Select item(s)"
        FIELD_CONSTRAINT = 7,   ///< e.g. "Constrain to H, V, 45"
        DRAW_FRAME_FIELDS = 8
    };

    /// What the health chip at the left end shows, e.g. "Fully routed" or "2 DRC errors".
    struct HEALTH
    {
        wxString text;          ///< the chip's text; empty = no chip
        wxString tooltip;       ///< the numbers behind it, shown on hover
        bool     ok = true;     ///< true: the calm (accent) chip; false: the attention chip
        wxString replacesItem;  ///< a message panel item the chip already shows (its upper text,
                                ///< e.g. "Unrouted"): left out of the counts, kept in tooltips
    };

    /// Computes the health chip for a frame, or std::nullopt for a frame it does not know. Health
    /// sources are registered once at start-up by code that knows the board or the schematic
    /// (wasm/bindings/pcbnew_embind.cpp, eeschema_embind.cpp), because this common class cannot.
    using HEALTH_SOURCE = std::function<std::optional<HEALTH>( wxWindow* aFrame )>;

    /// Register a health source for every labels-mode status bar. Call from start-up code only
    /// (the list is not locked; everything here runs on the UI thread).
    static void AddHealthSource( HEALTH_SOURCE aSource );

    /// One piece the bar drew, for tooltips and for the test hook (kicloud_test_status_texts).
    struct LABEL_PIECE
    {
        wxString kind;          ///< "health", "items", "message", "tool", "constraint", "zoom",
                                ///< "cursor", "delta", "grid", "unit" (one segment of the
                                ///< switch), "units" (the whole switch), "rest" (the empty space)
        wxString text;          ///< the text drawn (shortened with "…" when it did not fit)
        wxString tooltip;       ///< the full text(s), shown on hover
        wxRect   rect;          ///< where it is in the bar
    };

    /**
     * Turn labels mode on (once, right after the frame created this bar).
     *
     * @param aOnUnits called with 0 (mm), 1 (inches) or 2 (mils) when the user clicks the units
     *                 switch; the frame runs KiCad's own units action for it.
     */
    void EnableLabels( std::function<void( int )> aOnUnits );

    bool LabelsEnabled() const { return m_labels; }

    /// The message panel's items as (upper text, lower text) pairs, e.g. ("Pads", "120").
    void SetMessageItems( const std::vector<std::pair<wxString, wxString>>& aItems );

    /// Which units segment is selected: 0 mm, 1 inches, 2 mils, -1 none (other units).
    void SetUnitsChoice( int aChoice );

    /// The pieces drawn by the last paint (empty before the first paint).
    const std::vector<LABEL_PIECE>& GetLabelPieces() const { return m_pieces; }

    /// In labels mode, field 0 spans the painted area and the other drawing-frame fields get no
    /// column of their own (see kistatusbar.cpp); otherwise wxStatusBar's own behaviour.
    void SetStatusWidths( int aCount, const int aWidths[] ) override;

protected:
    wxSize DoGetBestSize() const override;
    void   DoUpdateStatusText( int aField ) override;

public:
#endif

    /**
     * Set the text in a field using wxELLIPSIZE_MIDDLE option to adjust the text size
     * to the field size.
     *
     * @note Unfortunately, setting the wxStatusBar style to wxELLIPSIZE_MIDDLE does not work.
     */
    void SetEllipsedTextField( const wxString& aText, int aFieldId );

    /**
     * Show the background progress bar.
     */
    void ShowBackgroundProgressBar( bool aCancellable = false );

    /**
     * Hide the background progress bar.
     */
    void HideBackgroundProgressBar();

    /**
     * Set the current progress of the progress bar.
     */
    void SetBackgroundProgress( int aAmount );

    /**
     * Set the max progress of the progress bar.
     */
    void SetBackgroundProgressMax( int aAmount );

    /**
     * Set the status text that displays next to the progress bar.
     */
    void SetBackgroundStatusText( const wxString& aTxt );

    /**
     * Set the notification count on the notifications button.
     *
     * A value of 0 will hide the count.
     */
    void SetNotificationCount( int aCount );

    /**
     * Clears all warning messages from the given source (or all sources if aSource is empty)
     */
    void ClearWarningMessages( const wxString& aSource = wxEmptyString );

    /**
     * Add warning/error messages (not thread-safe, use the std::vector<LOAD_MESSAGE> variant
     * from other threads)
     */
    void AddWarningMessages( const wxString& aSource, const wxString& aMessages );

    /**
     * Add warning/error messages thread-safely.
     * Can be called from any thread. UI update is deferred to main thread.
     */
    void AddWarningMessages( const wxString& aSource, const std::vector<LOAD_MESSAGE>& aMessages );

    /**
     * Get current message count (thread-safe).
     */
    size_t GetLoadWarningCount() const;

private:
    void onSize( wxSizeEvent& aEvent );
    void onBackgroundProgressClick( wxMouseEvent& aEvent );
    void onNotificationsIconClick( wxCommandEvent& aEvent );
    void onLoadWarningsIconClick( wxCommandEvent& aEvent );
    void updateWarningUI();  ///< Update warning button visibility and badge (main thread only)
    void updateAuxFieldWidths();
    void updateBackgroundText();
    void layoutControls();

    enum class FIELD
    {
        BGJOB_LABEL,
        BGJOB_GAUGE,
        BGJOB_CANCEL,
        WARNING,
        NOTIFICATION
    };

    std::optional<int> fieldIndex( FIELD aField ) const;

#ifdef __EMSCRIPTEN__
    // KICLOUD: LOOK.2, labels mode (see EnableLabels)
    void onLabelsPaint( wxPaintEvent& aEvent );
    void onLabelsMotion( wxMouseEvent& aEvent );
    void onLabelsLeave( wxMouseEvent& aEvent );
    void onLabelsLeftDown( wxMouseEvent& aEvent );
    void onLabelsIdle( wxIdleEvent& aEvent );
    void onHealthTimer( wxTimerEvent& aEvent );
    void refreshHealth();
    int  labelsRightEdge() const;
#endif

private:
    wxGauge*       m_backgroundProgressBar;
    wxButton*      m_backgroundStopButton;
    wxStaticText*  m_backgroundTxt;
    BITMAP_BUTTON* m_notificationsButton;
    BITMAP_BUTTON* m_warningButton;
    mutable std::mutex m_warningMutex;  ///< Protects m_warningMessages
    std::unordered_map<wxString, std::vector<LOAD_MESSAGE>> m_warningMessages;
    int            m_normalFieldsCount;
    STYLE_FLAGS    m_styleFlags;
    wxString       m_savedStatusText;       ///< Saved text from adjacent field during background jobs
    wxString       m_backgroundRawText;     ///< Unellipsized background status text
    std::vector<int> m_fieldWidths;

#ifdef __EMSCRIPTEN__
    // KICLOUD: LOOK.2, labels mode state (see EnableLabels)
    bool                                       m_labels = false;
    std::function<void( int )>                 m_onUnits;
    std::vector<std::pair<wxString, wxString>> m_messageItems;
    std::optional<HEALTH>                      m_health;
    int                                        m_unitsChoice = -1;
    std::vector<LABEL_PIECE>                   m_pieces;        ///< from the last paint
    std::vector<wxRect>                        m_unitsRects;    ///< mm, in, mil segments
    std::unordered_map<int, int>               m_stickyWidths;  ///< per right-hand field: widest yet
    int                                        m_stickyForWidth = -1;
    wxTimer                                    m_healthTimer;   ///< soon after the items change
    wxLongLong                                 m_lastHealth = 0;
#endif
};

#endif
