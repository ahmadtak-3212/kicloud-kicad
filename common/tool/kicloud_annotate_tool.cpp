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
// See include/tool/kicloud_annotate_tool.h.

#include <tool/kicloud_annotate_tool.h>

#include <algorithm>
#include <set>

#include <bitmaps.h>
#include <class_draw_panel_gal.h>
#include <eda_draw_frame.h>
#include <eda_item.h>
#include <json_common.h>
#include <preview_items/selection_area.h>
#include <preview_items/two_point_assistant.h>
#include <preview_items/two_point_geom_manager.h>
#include <tool/actions.h>
#include <tool/tool_event.h>
#include <tool/tool_manager.h>
#include <view/view.h>
#include <view/view_controls.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif


// Actions are static: as in common/tool/actions.cpp, _() only marks the strings here and the
// getters translate them.
#undef _
#define _(s) s

// Shift+C (P3.md §6.10) is the PCB editor's Add a Zone Cutout; Shift+D is free in both editors.
TOOL_ACTION KICLOUD_ACTIONS::commentBox( TOOL_ACTION_ARGS()
        .Name( "common.Kicloud.commentBox" )
        .Scope( AS_GLOBAL )
        .DefaultHotkey( MD_SHIFT + 'D' )
        .FriendlyName( _( "Add Comment Box" ) )
        .Tooltip( _( "Draw a box to comment on (nothing is added to the design)" ) )
        .ToolbarState( TOOLBAR_STATE::TOGGLE )
        .Icon( BITMAPS::add_textbox )
        .Flags( AF_ACTIVATE ) );

TOOL_ACTION KICLOUD_ACTIONS::commentPin( TOOL_ACTION_ARGS()
        .Name( "common.Kicloud.commentPin" )
        .Scope( AS_GLOBAL )
        .FriendlyName( _( "Add Comment Pin" ) )
        .Tooltip( _( "Pin a comment to a point (nothing is added to the design)" ) )
        .ToolbarState( TOOLBAR_STATE::TOGGLE )
        .Icon( BITMAPS::add_textbox )
        .Flags( AF_ACTIVATE ) );


namespace
{
const size_t MAX_UUIDS = 1000;

// The item a user sees as one thing: a footprint for its pads, a symbol for its pins and fields.
// Null for items that are not part of the design (previews, overlays, the drawing sheet).
const EDA_ITEM* topLevel( const EDA_ITEM* aItem )
{
    for( int depth = 0; aItem && depth < 8; ++depth )
    {
        const EDA_ITEM* parent = aItem->GetParent();

        if( !parent )
            return nullptr;

        switch( parent->Type() )
        {
        case PCB_T:
        case SCH_SCREEN_T:
        case SCHEMATIC_T:
            return aItem;
        default:
            aItem = parent;
        }
    }

    return nullptr;
}


void emit( const std::string& aJson )
{
#ifdef __EMSCRIPTEN__
    EM_ASM( {
        // A throwing listener must never unwind the wasm frame that called it (JSPI)
        try
        {
            if( window.kicloudAnnotate && window.kicloudAnnotate.onDone )
                window.kicloudAnnotate.onDone( UTF8ToString( $0 ) );
        }
        catch( e )
        {
            console.error( '[kicloud annotate] listener threw', e );
        }
    }, aJson.c_str() );
#else
    (void) aJson;
#endif
}
} // namespace


KICLOUD_ANNOTATE_TOOL::KICLOUD_ANNOTATE_TOOL( const std::string& aDoc,
                                              std::function<KICLOUD_ANNOTATE_PLACE()> aPlace ) :
        TOOL_INTERACTIVE( "common.Kicloud" ),
        m_doc( aDoc ),
        m_place( std::move( aPlace ) )
{
}


void KICLOUD_ANNOTATE_TOOL::setTransitions()
{
    Go( &KICLOUD_ANNOTATE_TOOL::Annotate, KICLOUD_ACTIONS::commentBox.MakeEvent() );
    Go( &KICLOUD_ANNOTATE_TOOL::Annotate, KICLOUD_ACTIONS::commentPin.MakeEvent() );
}


int KICLOUD_ANNOTATE_TOOL::Annotate( const TOOL_EVENT& aEvent )
{
    const bool            pin = aEvent.IsAction( &KICLOUD_ACTIONS::commentPin );
    EDA_DRAW_FRAME*       frame = getEditFrame<EDA_DRAW_FRAME>();
    KIGFX::VIEW*          view = getView();
    KIGFX::VIEW_CONTROLS* controls = getViewControls();

    frame->PushTool( aEvent );
    Activate();

    // The drawing tools' rectangle: the two-point geometry with its live size readout, shown
    // as the selection area (an outline only: nothing of the design is drawn or changed)
    KIGFX::PREVIEW::TWO_POINT_GEOMETRY_MANAGER geom;
    KIGFX::PREVIEW::TWO_POINT_ASSISTANT        assistant( geom, frame->GetIuScale(), frame->GetUserUnits(),
                                                          KIGFX::PREVIEW::GEOM_SHAPE::RECT );
    KIGFX::PREVIEW::SELECTION_AREA             area;

    view->Add( &area );
    view->Add( &assistant );
    view->SetVisible( &area, false );
    view->SetVisible( &assistant, false );

    controls->ShowCursor( true );
    controls->ForceCursorPosition( false );

    const wxString hint = wxGetTranslation( pin ? wxS( "Add Comment Pin: click a point, Esc to cancel" )
                                                : wxS( "Add Comment Box: click and drag to draw a box, Esc to cancel" ) );

    auto setCursor = [&]()
    {
        frame->GetCanvas()->SetCurrentCursor( KICURSOR::PENCIL );
    };

    setCursor();
    frame->DisplayToolMsg( hint );

    bool     started = false;    // the first corner is set
    bool     dragged = false;    // started with a drag: the button's release ends it
    bool     done = false;
    VECTOR2I origin;
    VECTOR2I end;

    auto update = [&]( const VECTOR2I& aEnd )
    {
        end = aEnd;
        geom.SetOrigin( origin );
        geom.SetEnd( end );
        area.SetOrigin( origin );
        area.SetEnd( end );
        view->SetVisible( &area, true );
        view->SetVisible( &assistant, true );
        view->Update( &area, KIGFX::GEOMETRY );
        view->Update( &assistant, KIGFX::GEOMETRY );
    };

    while( TOOL_EVENT* evt = Wait() )
    {
        setCursor();
        VECTOR2I cursor = controls->GetCursorPosition( !evt->DisableGridSnapping() );

        if( evt->IsCancelInteractive() || evt->IsActivate() || evt->IsClick( BUT_RIGHT ) )
        {
            break;
        }
        else if( pin && evt->IsClick( BUT_LEFT ) )
        {
            origin = end = cursor;
            done = true;
            break;
        }
        else if( pin )
        {
            evt->SetPassEvent( !evt->IsDrag( BUT_LEFT ) && !evt->IsMouseUp( BUT_LEFT ) );
        }
        else if( !started && evt->IsDrag( BUT_LEFT ) )
        {
            started = dragged = true;
            origin = frame->GetNearestGridPosition( evt->DragOrigin() );
            controls->SetAutoPan( true );
            controls->CaptureCursor( true );
            update( cursor );
        }
        else if( !started && evt->IsClick( BUT_LEFT ) )
        {
            started = true;
            origin = cursor;
            controls->SetAutoPan( true );
            controls->CaptureCursor( true );
            update( cursor );
        }
        else if( started && ( evt->IsMotion() || evt->IsDrag( BUT_LEFT ) ) )
        {
            update( cursor );
        }
        else if( started && ( ( dragged && evt->IsMouseUp( BUT_LEFT ) ) || ( !dragged && evt->IsClick( BUT_LEFT ) ) ) )
        {
            update( cursor );

            if( origin.x != end.x && origin.y != end.y )
            {
                done = true;
                break;
            }

            // A box without area: start again
            started = dragged = false;
            view->SetVisible( &area, false );
            view->SetVisible( &assistant, false );
            controls->SetAutoPan( false );
            controls->CaptureCursor( false );
        }
        else
        {
            evt->SetPassEvent();
        }
    }

    view->Remove( &area );
    view->Remove( &assistant );
    controls->SetAutoPan( false );
    controls->CaptureCursor( false );
    frame->GetCanvas()->SetCurrentCursor( KICURSOR::ARROW );
    frame->DisplayToolMsg( wxEmptyString );
    frame->PopTool( aEvent );

    if( done )
    {
        BOX2I box( origin, VECTOR2I( 0, 0 ) );
        box.SetEnd( end );
        box.Normalize();
        report( box, pin );
    }
    else
    {
        reportCancel();
    }

    return 0;
}


void KICLOUD_ANNOTATE_TOOL::report( const BOX2I& aBox, bool aPin )
{
    KIGFX::VIEW*                view = getView();
    std::vector<std::string>    uuids;
    std::set<const EDA_ITEM*>   seen;

    // The design items inside the box (a pin: the items under the point), as the user sees
    // them: a footprint or a symbol for their parts
    BOX2I query = aBox;

    if( aPin )
        query.Inflate( 1 );

    view->Query( query,
                 [&]( KIGFX::VIEW_ITEM* aViewItem ) -> bool
                 {
                     const EDA_ITEM* item = topLevel( dynamic_cast<const EDA_ITEM*>( aViewItem ) );

                     if( !item || seen.count( item ) || uuids.size() >= MAX_UUIDS )
                         return true;

                     seen.insert( item );

                     if( item->Type() == PCB_MARKER_T || item->Type() == SCH_MARKER_T )
                         return true;

                     const BOX2I bbox = item->GetBoundingBox();

                     if( aPin ? bbox.Contains( aBox.GetOrigin() ) : aBox.Contains( bbox ) )
                         uuids.push_back( item->m_Uuid.AsStdString() );

                     return true;
                 } );

    std::sort( uuids.begin(), uuids.end() );

    KICLOUD_ANNOTATE_PLACE place = m_place ? m_place() : KICLOUD_ANNOTATE_PLACE();
    nlohmann::json         j = { { "doc", m_doc },
                                 { "kind", aPin ? "pin" : "box" },
                                 { "x1", aBox.GetLeft() },
                                 { "y1", aBox.GetTop() },
                                 { "x2", aBox.GetRight() },
                                 { "y2", aBox.GetBottom() },
                                 { "uuids", uuids },
                                 { "layer", place.layer.empty() ? nlohmann::json() : nlohmann::json( place.layer ) },
                                 { "sheet", place.sheetPath.empty()
                                                    ? nlohmann::json()
                                                    : nlohmann::json( { { "path", place.sheetPath },
                                                                        { "name", place.sheetName } } ) } };

    emit( j.dump() );
}


void KICLOUD_ANNOTATE_TOOL::reportCancel()
{
    nlohmann::json j = { { "doc", m_doc }, { "cancelled", true } };
    emit( j.dump() );
}
