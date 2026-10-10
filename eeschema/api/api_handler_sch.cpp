/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2024 Jon Evans <jon@craftyjon.com>
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

#include <api/api_handler_sch.h>
#include <api/api_sch_utils.h>
#include <api/api_utils.h>
#include <magic_enum.hpp>
#include <sch_commit.h>
#include <sch_edit_frame.h>
#include <wx/filename.h>

#include <api/common/types/base_types.pb.h>

// KICLOUD: S4.6 what the schematic commands below use
#include <api/schematic/schematic_types.pb.h>
#include <math/util.h>
#include <project_sch.h>
#include <sch_junction.h>
#include <sch_label.h>
#include <sch_line.h>
#include <sch_no_connect.h>
#include <sch_pin.h>
#include <sch_reference_list.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <sch_symbol.h>
#include <schematic.h>
#include <connection_graph.h>
#include <sch_connection.h>
#include <tool/tool_manager.h>
#include <tools/sch_line_wire_bus_tool.h>
#include <eeschema_settings.h>
#include <ki_exception.h>
#include <cmath>
#include <deque>

using namespace kiapi::common::commands;
using kiapi::common::types::CommandStatus;
using kiapi::common::types::DocumentType;
using kiapi::common::types::ItemRequestStatus;

namespace schcmd = kiapi::schematic::commands;   // KICLOUD: S4.6


// KICLOUD: S4.6 small helpers of the schematic commands (file-local).
//
// Units: KiCad's command API gives positions in nanometres (Vector2 x_nm / y_nm), as on a board.
// A schematic counts in its own unit of 100 nm (schIUScale: 10,000 units per millimetre). Upstream
// KiCad 10's Line/LocalLabel decoding skipped that conversion (it read nanometres as schematic
// units, so 1 mm landed 100 mm away); upstream master converts with schIUScale, as these do.
namespace
{
// 1 mm = 1,000,000 nm = 10,000 schematic units (schIUScale.IU_PER_MM), so one unit is 100 nm.
constexpr double KICLOUD_NM_PER_SCH_IU = 100.0;

// A request position (nanometres) as schematic units, rounded to the nearest unit.
VECTOR2I kicloudFromNm( const kiapi::common::types::Vector2& aIn )
{
    return VECTOR2I( KiROUND( aIn.x_nm() / KICLOUD_NM_PER_SCH_IU ),
                     KiROUND( aIn.y_nm() / KICLOUD_NM_PER_SCH_IU ) );
}

// A schematic position as nanometres in a response.
void kicloudToNm( kiapi::common::types::Vector2* aOut, const VECTOR2I& aIn )
{
    aOut->set_x_nm( static_cast<int64_t>( aIn.x ) * 100 );
    aOut->set_y_nm( static_cast<int64_t>( aIn.y ) * 100 );
}

// A bad-request error with a sentence for the caller.
tl::unexpected<ApiResponseStatus> kicloudBad( const std::string& aMessage )
{
    ApiResponseStatus e;
    e.set_status( ApiStatusCode::AS_BAD_REQUEST );
    e.set_error_message( aMessage );
    return tl::unexpected( e );
}

// The schematic item kind a protobuf Any names, for the four schematic item messages KiCad 10's
// shared type table (TypeNameFromAny in common/api/api_utils.cpp) does not list: it maps board
// items only, so the shared CreateItems answered ISC_INVALID_TYPE for every schematic item.
std::optional<KICAD_T> kicloudSchTypeFromAny( const google::protobuf::Any& aAny )
{
    std::string type;

    if( !google::protobuf::Any::ParseAnyTypeUrl( aAny.type_url(), &type ) )
        return std::nullopt;

    if( type == "kiapi.schematic.types.Line" )              return SCH_LINE_T;
    if( type == "kiapi.schematic.types.LocalLabel" )        return SCH_LABEL_T;
    if( type == "kiapi.schematic.types.GlobalLabel" )       return SCH_GLOBAL_LABEL_T;
    if( type == "kiapi.schematic.types.HierarchicalLabel" ) return SCH_HIER_LABEL_T;

    return std::nullopt;
}

// Fills a new wire or label from its message, in place of KiCad 10's own Deserialize, which reads
// positions without the unit conversion, drops a label's text, and leaves a line on the graphic
// notes layer (its SchematicLayer enum has no values, so a client cannot ask for a wire).
// A Line always becomes a wire. Returns false when the Any is not the item's message.
bool kicloudDecodeSchItem( SCH_ITEM* aItem, const google::protobuf::Any& aAny, int aTextSize )
{
    if( aItem->Type() == SCH_LINE_T )
    {
        kiapi::schematic::types::Line msg;

        if( !aAny.UnpackTo( &msg ) )
            return false;

        SCH_LINE* line = static_cast<SCH_LINE*>( aItem );
        const_cast<KIID&>( line->m_Uuid ) = msg.id().value().empty() ? KIID() : KIID( msg.id().value() );
        line->SetLayer( LAYER_WIRE );
        line->SetStartPoint( kicloudFromNm( msg.start() ) );
        line->SetEndPoint( kicloudFromNm( msg.end() ) );
        return true;
    }

    // The three label messages have the same fields (id, position, text).
    auto fill = [&]( const auto& aMsg ) -> bool
    {
        SCH_LABEL_BASE* label = static_cast<SCH_LABEL_BASE*>( aItem );
        const_cast<KIID&>( label->m_Uuid ) = aMsg.id().value().empty() ? KIID() : KIID( aMsg.id().value() );
        label->SetPosition( kicloudFromNm( aMsg.position() ) );
        label->SetTextSize( VECTOR2I( aTextSize, aTextSize ) );
        label->SetText( wxString::FromUTF8( aMsg.text().text().text() ) );
        return true;
    };

    switch( aItem->Type() )
    {
    case SCH_LABEL_T:
    {
        kiapi::schematic::types::LocalLabel msg;
        return aAny.UnpackTo( &msg ) && fill( msg );
    }
    case SCH_GLOBAL_LABEL_T:
    {
        kiapi::schematic::types::GlobalLabel msg;
        return aAny.UnpackTo( &msg ) && fill( msg );
    }
    case SCH_HIER_LABEL_T:
    {
        kiapi::schematic::types::HierarchicalLabel msg;
        return aAny.UnpackTo( &msg ) && fill( msg );
    }
    default:
        return false;
    }
}

// The answer for a wire or label made by kicloudDecodeSchItem, in nanometres (KiCad 10's own
// SCH_LINE::Serialize would hit an assertion: its layer enum maps no layer).
void kicloudEncodeSchItem( const SCH_ITEM* aItem, google::protobuf::Any& aOut )
{
    if( aItem->Type() == SCH_LINE_T )
    {
        const SCH_LINE* line = static_cast<const SCH_LINE*>( aItem );
        kiapi::schematic::types::Line msg;
        msg.mutable_id()->set_value( line->m_Uuid.AsStdString() );
        kicloudToNm( msg.mutable_start(), line->GetStartPoint() );
        kicloudToNm( msg.mutable_end(), line->GetEndPoint() );
        aOut.PackFrom( msg );
        return;
    }

    auto pack = [&]( auto& aMsg )
    {
        const SCH_LABEL_BASE* label = static_cast<const SCH_LABEL_BASE*>( aItem );
        aMsg.mutable_id()->set_value( label->m_Uuid.AsStdString() );
        kicloudToNm( aMsg.mutable_position(), label->GetPosition() );
        aMsg.mutable_text()->mutable_text()->set_text( label->GetText().ToStdString( wxConvUTF8 ) );
        aOut.PackFrom( aMsg );
    };

    if( aItem->Type() == SCH_LABEL_T )
    {
        kiapi::schematic::types::LocalLabel msg;
        pack( msg );
    }
    else if( aItem->Type() == SCH_GLOBAL_LABEL_T )
    {
        kiapi::schematic::types::GlobalLabel msg;
        pack( msg );
    }
    else if( aItem->Type() == SCH_HIER_LABEL_T )
    {
        kiapi::schematic::types::HierarchicalLabel msg;
        pack( msg );
    }
}

// Degrees (0/90/180/270, any multiple of 90 including negatives) as a quarter-turn count 0..3,
// or -1 for any other angle.
int kicloudQuarterTurns( double aDegrees )
{
    double turns = aDegrees / 90.0;

    if( std::abs( turns - std::round( turns ) ) > 1e-6 )
        return -1;

    return ( ( static_cast<int>( std::lround( turns ) ) % 4 ) + 4 ) % 4;
}

// The net name KiCad's connectivity gives an item on a sheet, or "" when it has none yet.
std::string kicloudNetOf( const SCH_ITEM* aItem, const SCH_SHEET_PATH& aPath )
{
    if( SCH_CONNECTION* c = aItem->Connection( &aPath ) )
        return c->Name().ToStdString( wxConvUTF8 );

    return "";
}

std::string kicloudUtf8( const wxString& aText )
{
    return aText.ToStdString( wxConvUTF8 );
}

// One sheet instance as SchematicSheetInfo.
void kicloudPackSheet( schcmd::SchematicSheetInfo* aOut, const SCH_SHEET_PATH& aPath,
                       const SCH_SHEET_PATH& aCurrent )
{
    PackSheetPath( *aOut->mutable_sheet_path(), aPath.Path() );
    aOut->mutable_sheet_path()->set_path_human_readable( kicloudUtf8( aPath.PathHumanReadable( true ) ) );
    aOut->set_name( aPath.size() > 1 ? kicloudUtf8( aPath.Last()->GetName() ) : std::string() );

    if( const SCH_SCREEN* screen = aPath.LastScreen() )
        aOut->set_file( kicloudUtf8( wxFileName( screen->GetFileName() ).GetFullName() ) );

    aOut->set_page( kicloudUtf8( aPath.GetPageNumber() ) );
    aOut->set_current( aPath == aCurrent );
    aOut->set_kicad_path( kicloudUtf8( aPath.PathAsString() ) );
}

// A placed symbol as SchematicSymbolInfo, read on one sheet instance (references, values and
// nets depend on the instance when a sheet is used more than once).
void kicloudPackSymbol( schcmd::SchematicSymbolInfo* aOut, SCH_SYMBOL* aSymbol,
                        const SCH_SHEET_PATH& aPath )
{
    aOut->mutable_id()->set_value( aSymbol->m_Uuid.AsStdString() );
    aOut->set_reference( kicloudUtf8( aSymbol->GetRef( &aPath, false ) ) );
    aOut->set_value( kicloudUtf8( aSymbol->GetValue( false, &aPath, false ) ) );
    aOut->set_footprint( kicloudUtf8( aSymbol->GetFootprintFieldText( false, &aPath, false ) ) );
    aOut->set_lib_id( kicloudUtf8( aSymbol->GetLibId().Format().wx_str() ) );
    kicloudToNm( aOut->mutable_position(), aSymbol->GetPosition() );

    switch( aSymbol->GetOrientationProp() )
    {
    case SYMBOL_ANGLE_90:  aOut->set_rotation( 90 );  break;
    case SYMBOL_ANGLE_180: aOut->set_rotation( 180 ); break;
    case SYMBOL_ANGLE_270: aOut->set_rotation( 270 ); break;
    default:               aOut->set_rotation( 0 );   break;
    }

    aOut->set_mirror_x( aSymbol->GetMirrorX() );
    aOut->set_mirror_y( aSymbol->GetMirrorY() );
    aOut->set_unit( aSymbol->GetUnitSelection( &aPath ) );
    aOut->set_power( aSymbol->IsPower() );
    aOut->set_dnp( aSymbol->GetDNP( &aPath ) );
    aOut->set_exclude_from_bom( aSymbol->GetExcludedFromBOM( &aPath ) );
    aOut->set_exclude_from_board( aSymbol->GetExcludedFromBoard( &aPath ) );

    for( const SCH_FIELD& field : aSymbol->GetFields() )
    {
        schcmd::SchematicFieldValue* f = aOut->add_fields();
        f->set_name( kicloudUtf8( field.GetName() ) );
        f->set_value( field.GetId() == FIELD_T::REFERENCE ? aOut->reference()
                                                         : kicloudUtf8( field.GetText() ) );
    }

    for( SCH_PIN* pin : aSymbol->GetPins( &aPath ) )
    {
        schcmd::SchematicPinInfo* p = aOut->add_pins();
        p->set_number( kicloudUtf8( pin->GetShownNumber() ) );
        p->set_name( kicloudUtf8( pin->GetShownName() ) );
        kicloudToNm( p->mutable_position(), pin->GetPosition() );
        p->set_electrical_type( kicloudUtf8( pin->GetElectricalTypeName() ) );
        p->set_net( kicloudNetOf( pin, aPath ) );
    }
}

// Sets one field of a symbol on a sheet instance: Reference renames that instance, Value and
// Footprint use KiCad's own setters, another existing field gets the text, and a field the symbol
// does not have is added (hidden, at the symbol's position, as the symbol properties dialog
// adds one). The caller has staged the symbol in its commit first.
void kicloudSetField( SCH_SYMBOL* aSymbol, const SCH_SHEET_PATH& aPath, const wxString& aName,
                      const wxString& aValue )
{
    if( aName.CmpNoCase( wxS( "Reference" ) ) == 0 )
        aSymbol->SetRef( &aPath, aValue );
    else if( aName.CmpNoCase( wxS( "Value" ) ) == 0 )
        aSymbol->SetValueFieldText( aValue );
    else if( aName.CmpNoCase( wxS( "Footprint" ) ) == 0 )
        aSymbol->SetFootprintFieldText( aValue );
    else if( SCH_FIELD* field = aSymbol->GetField( aName ) )
        field->SetText( aValue );
    else
    {
        SCH_FIELD added( aSymbol, FIELD_T::USER, aName );
        added.SetText( aValue );
        added.SetVisible( false );
        added.SetPosition( aSymbol->GetPosition() );
        aSymbol->AddField( added );
    }
}
} // namespace


API_HANDLER_SCH::API_HANDLER_SCH( SCH_EDIT_FRAME* aFrame ) :
        // KICLOUD: S4.1 pass the frame to the shared editor handler. Upstream passed none, so
        // API_HANDLER_EDITOR::checkForBusy() dereferenced a null frame and every shared command on
        // the schematic (begin/end commit, create/update/delete items, hit test) crashed.
        API_HANDLER_EDITOR( aFrame ),
        m_frame( aFrame )
{
    registerHandler<GetOpenDocuments, GetOpenDocumentsResponse>(
            &API_HANDLER_SCH::handleGetOpenDocuments );

    // KICLOUD: S4.6 the schematic commands (api/proto/schematic/schematic_commands.proto)
    registerHandler<schcmd::GetSchematicHierarchy, schcmd::SchematicHierarchyResponse>(
            &API_HANDLER_SCH::handleGetSchematicHierarchy );
    registerHandler<schcmd::GetSchematicItems, schcmd::GetSchematicItemsResponse>(
            &API_HANDLER_SCH::handleGetSchematicItems );
    registerHandler<schcmd::PlaceSymbolFromLibrary, schcmd::PlaceSymbolResponse>(
            &API_HANDLER_SCH::handlePlaceSymbolFromLibrary );
    registerHandler<schcmd::AddSchematicWires, schcmd::AddSchematicItemsResponse>(
            &API_HANDLER_SCH::handleAddSchematicWires );
    registerHandler<schcmd::AddSchematicLabel, schcmd::AddSchematicItemsResponse>(
            &API_HANDLER_SCH::handleAddSchematicLabel );
    registerHandler<schcmd::SetSymbolFields, schcmd::PlaceSymbolResponse>(
            &API_HANDLER_SCH::handleSetSymbolFields );
}


// KICLOUD: S4.6 the sheet instance a request names. The document's sheet_path may hold the KIID
// path (exact), or only path_human_readable: a readable path such as "/Power/" (with or without
// the slashes) or a bare sheet name, which must name exactly one instance. No sheet_path, or an
// empty one, means the sheet the editor shows now. Errors are bad requests that name the sheets
// a client may use. Reads only; changes nothing.
HANDLER_RESULT<SCH_SHEET_PATH> API_HANDLER_SCH::kicloudSheetFor( const DocumentSpecifier& aDocument )
{
    if( aDocument.type() != DocumentType::DOCTYPE_SCHEMATIC )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    const SCH_SHEET_PATH current = m_frame->GetCurrentSheet();

    if( !aDocument.has_sheet_path() )
        return current;

    const kiapi::common::types::SheetPath& asked = aDocument.sheet_path();
    SCH_SHEET_LIST hierarchy = m_frame->Schematic().Hierarchy();

    if( asked.path_size() > 0 )
    {
        if( std::optional<SCH_SHEET_PATH> path =
                    hierarchy.GetSheetPathByKIIDPath( UnpackSheetPath( asked ) ) )
        {
            return *path;
        }

        return kicloudBad( "the sheet path is not a sheet of this schematic" );
    }

    // "/Power/", "/Power", "Power/", "Power" all mean the same; "/" or "" is the root sheet.
    auto norm = []( wxString aPath )
    {
        aPath.Trim( true ).Trim( false );

        while( aPath.StartsWith( wxS( "/" ) ) )
            aPath = aPath.Mid( 1 );

        while( aPath.EndsWith( wxS( "/" ) ) )
            aPath.RemoveLast();

        return aPath;
    };

    const wxString want = norm( wxString::FromUTF8( asked.path_human_readable() ) );

    // no readable path at all: the sheet shown now ("/" is the root sheet, found in the loop below;
    // the list's first entry is not always the root)
    if( want.IsEmpty() && asked.path_human_readable().empty() )
        return current;

    std::vector<SCH_SHEET_PATH> byName;
    wxString                    names;

    for( const SCH_SHEET_PATH& path : hierarchy )
    {
        const wxString readable = norm( path.PathHumanReadable( true ) );

        if( readable == want )
            return path;

        if( path.size() > 1 && path.Last()->GetName() == want )
            byName.push_back( path );

        names << ( names.IsEmpty() ? wxS( "" ) : wxS( ", " ) ) << path.PathHumanReadable( true );
    }

    if( byName.size() == 1 )
        return byName.front();

    if( byName.size() > 1 )
        return kicloudBad( fmt::format( "the sheet name '{}' is used by more than one sheet; give "
                                        "its full path, one of: {}",
                                        kicloudUtf8( want ), kicloudUtf8( names ) ) );

    return kicloudBad( fmt::format( "no sheet '{}' in this schematic; its sheets are: {}",
                                    kicloudUtf8( want ), kicloudUtf8( names ) ) );
}


// KICLOUD: S4.6 a top-level item (symbol, wire, label, junction, ...) of any sheet by its id, and
// the screen holding it (through aScreen). A field or pin is not top-level and is not found.
// Reads only.
SCH_ITEM* API_HANDLER_SCH::kicloudFindItem( const KIID& aId, SCH_SCREEN** aScreen )
{
    SCH_SCREENS screens( m_frame->Schematic().Root() );

    for( SCH_SCREEN* screen = screens.GetFirst(); screen; screen = screens.GetNext() )
    {
        for( SCH_ITEM* item : screen->Items() )
        {
            if( item->m_Uuid == aId )
            {
                if( aScreen )
                    *aScreen = screen;

                return item;
            }
        }
    }

    return nullptr;
}


// KICLOUD: S4.6 GetSchematicHierarchy: every sheet instance in page order (the root first), with
// the one the editor shows marked current. Reads only.
HANDLER_RESULT<schcmd::SchematicHierarchyResponse> API_HANDLER_SCH::handleGetSchematicHierarchy(
        const HANDLER_CONTEXT<schcmd::GetSchematicHierarchy>& aCtx )
{
    if( aCtx.Request.document().type() != DocumentType::DOCTYPE_SCHEMATIC )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    schcmd::SchematicHierarchyResponse response;
    SCH_SHEET_LIST hierarchy = m_frame->Schematic().Hierarchy();
    hierarchy.SortByPageNumbers();

    for( const SCH_SHEET_PATH& path : hierarchy )
        kicloudPackSheet( response.add_sheets(), path, m_frame->GetCurrentSheet() );

    return response;
}


// KICLOUD: S4.6 GetSchematicItems: one sheet's symbols (with pins, their positions and nets),
// wires, labels, junctions and no-connect flags. KiCad's connectivity is rebuilt first
// (NO_CLEANUP: nothing in the schematic changes) so the nets are current.
HANDLER_RESULT<schcmd::GetSchematicItemsResponse> API_HANDLER_SCH::handleGetSchematicItems(
        const HANDLER_CONTEXT<schcmd::GetSchematicItems>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<SCH_SHEET_PATH> sheet = kicloudSheetFor( aCtx.Request.document() );

    if( !sheet )
        return tl::unexpected( sheet.error() );

    m_frame->RecalculateConnections( nullptr, NO_CLEANUP );

    const SCH_SHEET_PATH& path = *sheet;
    SCH_SCREEN* screen = path.LastScreen();
    schcmd::GetSchematicItemsResponse response;
    kicloudPackSheet( response.mutable_sheet(), path, m_frame->GetCurrentSheet() );

    if( !screen )
        return response;

    for( SCH_ITEM* item : screen->Items() )
    {
        switch( item->Type() )
        {
        case SCH_SYMBOL_T:
            kicloudPackSymbol( response.add_symbols(), static_cast<SCH_SYMBOL*>( item ), path );
            break;

        case SCH_LINE_T:
        {
            SCH_LINE* line = static_cast<SCH_LINE*>( item );
            schcmd::SchematicWireInfo* w = response.add_wires();
            w->mutable_id()->set_value( line->m_Uuid.AsStdString() );
            kicloudToNm( w->mutable_start(), line->GetStartPoint() );
            kicloudToNm( w->mutable_end(), line->GetEndPoint() );
            w->set_kind( line->IsWire() ? "wire" : line->IsBus() ? "bus" : "line" );

            if( line->IsWire() || line->IsBus() )
                w->set_net( kicloudNetOf( line, path ) );

            break;
        }

        case SCH_LABEL_T:
        case SCH_GLOBAL_LABEL_T:
        case SCH_HIER_LABEL_T:
        {
            SCH_LABEL_BASE* label = static_cast<SCH_LABEL_BASE*>( item );
            schcmd::SchematicLabelInfo* l = response.add_labels();
            l->mutable_id()->set_value( label->m_Uuid.AsStdString() );
            l->set_text( kicloudUtf8( label->GetText() ) );
            kicloudToNm( l->mutable_position(), label->GetPosition() );
            l->set_kind( item->Type() == SCH_LABEL_T          ? "local"
                         : item->Type() == SCH_GLOBAL_LABEL_T ? "global"
                                                              : "hierarchical" );

            switch( static_cast<int>( label->GetSpinStyle() ) )
            {
            case SPIN_STYLE::UP:     l->set_rotation( 90 );  break;
            case SPIN_STYLE::LEFT:   l->set_rotation( 180 ); break;
            case SPIN_STYLE::BOTTOM: l->set_rotation( 270 ); break;
            default:                 l->set_rotation( 0 );   break;
            }

            l->set_net( kicloudNetOf( label, path ) );
            break;
        }

        case SCH_JUNCTION_T:
            kicloudToNm( response.add_junctions(), item->GetPosition() );
            break;

        case SCH_NO_CONNECT_T:
            kicloudToNm( response.add_no_connects(), item->GetPosition() );
            break;

        default:
            break;
        }
    }

    return response;
}


// KICLOUD: S4.6 PlaceSymbolFromLibrary: loads the symbol from the project's symbol libraries
// (the project table and the global one, as the symbol chooser does, without error dialogs),
// places it on the requested sheet with the requested orientation and unit, sets the given
// fields, and annotates it on every instance of that sheet unless a reference is given (the
// interactive placement tool's own steps, sch_drawing_tools.cpp PlaceSymbol). Joins the client's
// open commit, else is one undo step. Answers the placed symbol with its pins and their positions.
HANDLER_RESULT<schcmd::PlaceSymbolResponse> API_HANDLER_SCH::handlePlaceSymbolFromLibrary(
        const HANDLER_CONTEXT<schcmd::PlaceSymbolFromLibrary>& aCtx )
{
    const schcmd::PlaceSymbolFromLibrary& req = aCtx.Request;

    if( req.header().document().type() != DocumentType::DOCTYPE_SCHEMATIC )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<SCH_SHEET_PATH> sheet = kicloudSheetFor( req.header().document() );

    if( !sheet )
        return tl::unexpected( sheet.error() );

    LIB_ID libId( wxString::FromUTF8( req.lib_id().library_nickname() ),
                  wxString::FromUTF8( req.lib_id().entry_name() ) );

    if( !libId.IsValid() )
        return kicloudBad( "lib_id needs both a library nickname and a symbol name" );

    LIB_SYMBOL* libSymbol = nullptr;

    try
    {
        libSymbol = m_frame->GetLibSymbol( libId, false, false );
    }
    catch( const IO_ERROR& ioe )
    {
        return kicloudBad( fmt::format( "the library '{}' could not be read: {}",
                                        req.lib_id().library_nickname(),
                                        kicloudUtf8( ioe.What() ) ) );
    }

    if( !libSymbol )
        return kicloudBad( fmt::format( "no symbol '{}' in the project's symbol libraries",
                                        kicloudUtf8( libId.Format().wx_str() ) ) );

    const int turns = kicloudQuarterTurns( req.rotation() );

    if( turns < 0 )
        return kicloudBad( "rotation must be a multiple of 90 degrees" );

    const int unit = req.unit() > 0 ? req.unit() : 1;

    if( unit > std::max( 1, libSymbol->GetUnitCount() ) )
        return kicloudBad( fmt::format( "unit {} is out of range: the symbol has {} unit(s)", unit,
                                        libSymbol->GetUnitCount() ) );

    SCH_SHEET_PATH path = *sheet;
    SCH_SCREEN*    screen = path.LastScreen();
    SCHEMATIC&     schematic = m_frame->Schematic();
    VECTOR2I       position = kicloudFromNm( req.position() );

    std::unique_ptr<SCH_SYMBOL> symbol =
            std::make_unique<SCH_SYMBOL>( *libSymbol, libId, &path, unit, 0, position, &schematic );

    static const SYMBOL_ORIENTATION_PROP angles[4] = { SYMBOL_ANGLE_0, SYMBOL_ANGLE_90,
                                                       SYMBOL_ANGLE_180, SYMBOL_ANGLE_270 };
    symbol->SetOrientationProp( angles[turns] );
    symbol->SetMirrorX( req.mirror_x() );
    symbol->SetMirrorY( req.mirror_y() );

    wxString reference = wxString::FromUTF8( req.reference() );

    for( const schcmd::SchematicFieldValue& f : req.fields() )
    {
        const wxString name = wxString::FromUTF8( f.name() );

        if( name.IsEmpty() )
            return kicloudBad( "a field needs a name" );

        if( name.CmpNoCase( wxS( "Reference" ) ) == 0 )
            reference = wxString::FromUTF8( f.value() );
        else
            kicloudSetField( symbol.get(), path, name, wxString::FromUTF8( f.value() ) );
    }

    SCH_SHEET_LIST hierarchy = schematic.Hierarchy();

    if( !reference.IsEmpty() )
    {
        symbol->SetRef( &path, reference );
    }
    else
    {
        // Annotate every instance of the target sheet, avoiding every reference in use anywhere.
        SCHEMATIC_SETTINGS& settings = schematic.Settings();
        SCH_REFERENCE_LIST  existingRefs;
        hierarchy.GetSymbols( existingRefs, SYMBOL_FILTER_ALL );
        existingRefs.SortByReferenceOnly();

        SCH_SHEET_LIST instances = hierarchy.FindAllSheetsForScreen( screen );
        instances.SortByPageNumbers();

        for( SCH_SHEET_PATH& instance : instances )
        {
            SCH_REFERENCE      newReference( symbol.get(), instance );
            SCH_REFERENCE_LIST refs;
            refs.AddItem( newReference );
            refs.SetRefDesTracker( settings.m_refDesTracker );
            refs.ReannotateByOptions( (ANNOTATE_ORDER_T) settings.m_AnnotateSortOrder,
                                      (ANNOTATE_ALGO_T) settings.m_AnnotateMethod,
                                      settings.m_AnnotateStartNum, existingRefs, false,
                                      &hierarchy );
            refs.UpdateAnnotation();

            for( size_t i = 0; i < refs.GetCount(); i++ )
                existingRefs.AddItem( refs[i] );
        }
    }

    if( m_frame->eeconfig() && m_frame->eeconfig()->m_AutoplaceFields.enable )
        symbol->AutoplaceFields( nullptr, AUTOPLACE_AUTO );

    SCH_COMMIT* commit = static_cast<SCH_COMMIT*>( getCurrentCommit( aCtx.ClientName ) );
    SCH_SYMBOL* placed = symbol.release();
    commit->Add( placed, screen );

    if( !m_activeClients.count( aCtx.ClientName ) )
        pushCurrentCommit( aCtx.ClientName, _( "Placed symbol via API" ) );

    // the pins' nets are read from KiCad's connectivity, which a pushed commit has rebuilt; inside
    // an open commit the new symbol is not on the sheet yet and its pins answer no net
    schcmd::PlaceSymbolResponse response;
    kicloudPackSymbol( response.mutable_symbol(), placed, path );
    return response;
}


// KICLOUD: S4.6 AddSchematicWires: wire (or bus) segments through the points on the requested
// sheet, then a junction wherever an end of a new segment lands on another wire's middle or where
// three or more wire ends meet (SCH_SCREEN::GetNeededJunctions; SCH_LINE_WIRE_BUS_TOOL::AddJunction
// also splits the wire there), as KiCad's wire tool finishes a wire. Joins the client's open
// commit, else is one undo step. Answers the new segments' and junctions' ids.
HANDLER_RESULT<schcmd::AddSchematicItemsResponse> API_HANDLER_SCH::handleAddSchematicWires(
        const HANDLER_CONTEXT<schcmd::AddSchematicWires>& aCtx )
{
    const schcmd::AddSchematicWires& req = aCtx.Request;

    if( req.header().document().type() != DocumentType::DOCTYPE_SCHEMATIC )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<SCH_SHEET_PATH> sheet = kicloudSheetFor( req.header().document() );

    if( !sheet )
        return tl::unexpected( sheet.error() );

    if( req.points_size() < 2 )
        return kicloudBad( "a wire needs at least two points" );

    std::vector<VECTOR2I> points;

    for( const kiapi::common::types::Vector2& p : req.points() )
        points.push_back( kicloudFromNm( p ) );

    for( size_t i = 1; i < points.size(); i++ )
    {
        if( points[i] == points[i - 1] )
            return kicloudBad( fmt::format( "points {} and {} are the same point", i - 1, i ) );
    }

    SCH_SCREEN* screen = sheet->LastScreen();
    SCH_COMMIT* commit = static_cast<SCH_COMMIT*>( getCurrentCommit( aCtx.ClientName ) );
    schcmd::AddSchematicItemsResponse response;
    std::deque<EDA_ITEM*> added;

    for( size_t i = 1; i < points.size(); i++ )
    {
        SCH_LINE* line = new SCH_LINE( points[i - 1], req.bus() ? LAYER_BUS : LAYER_WIRE );
        line->SetEndPoint( points[i] );
        m_frame->AddToScreen( line, screen );
        commit->Added( line, screen );
        added.push_back( line );
        response.add_ids()->set_value( line->m_Uuid.AsStdString() );
    }

    if( SCH_LINE_WIRE_BUS_TOOL* tool =
                m_frame->GetToolManager()->GetTool<SCH_LINE_WIRE_BUS_TOOL>() )
    {
        for( const VECTOR2I& at : screen->GetNeededJunctions( added ) )
        {
            SCH_JUNCTION* junction = tool->AddJunction( commit, screen, at );
            response.add_ids()->set_value( junction->m_Uuid.AsStdString() );
        }
    }

    if( !m_activeClients.count( aCtx.ClientName ) )
        pushCurrentCommit( aCtx.ClientName, _( "Added wires via API" ) );

    return response;
}


// KICLOUD: S4.6 AddSchematicLabel: one net label (local, global or hierarchical) at a point of
// the requested sheet, with the schematic's default text size and the given orientation. A label
// connects to the wire or pin end it sits on. Joins the client's open commit, else is one undo
// step. Answers the label's id.
HANDLER_RESULT<schcmd::AddSchematicItemsResponse> API_HANDLER_SCH::handleAddSchematicLabel(
        const HANDLER_CONTEXT<schcmd::AddSchematicLabel>& aCtx )
{
    const schcmd::AddSchematicLabel& req = aCtx.Request;

    if( req.header().document().type() != DocumentType::DOCTYPE_SCHEMATIC )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<SCH_SHEET_PATH> sheet = kicloudSheetFor( req.header().document() );

    if( !sheet )
        return tl::unexpected( sheet.error() );

    const wxString text = wxString::FromUTF8( req.text() );

    if( wxString( text ).Trim().Trim( false ).IsEmpty() )
        return kicloudBad( "a label needs text (the net name)" );

    const int turns = kicloudQuarterTurns( req.rotation() );

    if( turns < 0 )
        return kicloudBad( "rotation must be 0, 90, 180 or 270 degrees" );

    const VECTOR2I  at = kicloudFromNm( req.position() );
    SCH_LABEL_BASE* label = nullptr;

    if( req.kind().empty() || req.kind() == "local" )
        label = new SCH_LABEL( at, text );
    else if( req.kind() == "global" )
        label = new SCH_GLOBALLABEL( at, text );
    else if( req.kind() == "hierarchical" )
        label = new SCH_HIERLABEL( at, text );
    else
        return kicloudBad( "kind must be local, global or hierarchical" );

    if( req.kind() == "global" || req.kind() == "hierarchical" )
        label->SetShape( LABEL_FLAG_SHAPE::L_BIDI );

    const int size = m_frame->Schematic().Settings().m_DefaultTextSize;
    label->SetTextSize( VECTOR2I( size, size ) );

    static const SPIN_STYLE spins[4] = { SPIN_STYLE::RIGHT, SPIN_STYLE::UP, SPIN_STYLE::LEFT,
                                         SPIN_STYLE::BOTTOM };
    label->SetSpinStyle( spins[turns] );

    SCH_SCREEN* screen = sheet->LastScreen();
    SCH_COMMIT* commit = static_cast<SCH_COMMIT*>( getCurrentCommit( aCtx.ClientName ) );
    commit->Add( label, screen );

    schcmd::AddSchematicItemsResponse response;
    response.add_ids()->set_value( label->m_Uuid.AsStdString() );

    if( !m_activeClients.count( aCtx.ClientName ) )
        pushCurrentCommit( aCtx.ClientName, _( "Added label via API" ) );

    return response;
}


// KICLOUD: S4.6 SetSymbolFields: sets fields of one placed symbol, found by id or by reference
// (on the sheet the header names; with no sheet named, a reference is looked up on every sheet
// and must be unique). The symbol is staged in the commit before it changes, so undo restores it.
// Joins the client's open commit, else is one undo step. Answers the symbol as it is now.
HANDLER_RESULT<schcmd::PlaceSymbolResponse> API_HANDLER_SCH::handleSetSymbolFields(
        const HANDLER_CONTEXT<schcmd::SetSymbolFields>& aCtx )
{
    const schcmd::SetSymbolFields& req = aCtx.Request;

    if( req.header().document().type() != DocumentType::DOCTYPE_SCHEMATIC )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( req.fields_size() == 0 )
        return kicloudBad( "no fields to set" );

    SCH_SYMBOL*    symbol = nullptr;
    SCH_SCREEN*    screen = nullptr;
    SCH_SHEET_PATH path;

    if( !req.id().value().empty() )
    {
        SCH_ITEM* item = kicloudFindItem( KIID( req.id().value() ), &screen );

        if( !item || item->Type() != SCH_SYMBOL_T )
            return kicloudBad( "no symbol with that id" );

        symbol = static_cast<SCH_SYMBOL*>( item );
        HANDLER_RESULT<SCH_SHEET_PATH> sheet = kicloudSheetFor( req.header().document() );

        if( sheet && sheet->LastScreen() == screen )
        {
            path = *sheet;
        }
        else
        {
            // the first instance of the screen holding the symbol
            SCH_SHEET_LIST instances = m_frame->Schematic().Hierarchy().FindAllSheetsForScreen( screen );

            if( instances.empty() )
                return kicloudBad( "the symbol is on no sheet of the hierarchy" );

            instances.SortByPageNumbers();
            path = instances.front();
        }
    }
    else if( !req.reference().empty() )
    {
        const wxString ref = wxString::FromUTF8( req.reference() );
        std::vector<std::pair<SCH_SYMBOL*, SCH_SHEET_PATH>> found;
        SCH_SHEET_LIST hierarchy = m_frame->Schematic().Hierarchy();
        std::optional<SCH_SHEET_PATH> only;

        if( req.header().document().has_sheet_path() )
        {
            HANDLER_RESULT<SCH_SHEET_PATH> sheet = kicloudSheetFor( req.header().document() );

            if( !sheet )
                return tl::unexpected( sheet.error() );

            only = *sheet;
        }

        for( const SCH_SHEET_PATH& p : hierarchy )
        {
            if( only && !( p == *only ) )
                continue;

            for( SCH_ITEM* item : p.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
            {
                SCH_SYMBOL* s = static_cast<SCH_SYMBOL*>( item );

                if( s->GetRef( &p, false ) == ref )
                    found.emplace_back( s, p );
            }
        }

        if( found.empty() )
            return kicloudBad( fmt::format( "no symbol with the reference {}", req.reference() ) );

        if( found.size() > 1 )
            return kicloudBad( fmt::format( "the reference {} is used {} times; name the symbol "
                                            "by id or name its sheet", req.reference(),
                                            found.size() ) );

        symbol = found.front().first;
        path = found.front().second;
        screen = path.LastScreen();
    }
    else
    {
        return kicloudBad( "name the symbol by id or by reference" );
    }

    for( const schcmd::SchematicFieldValue& f : req.fields() )
    {
        if( f.name().empty() )
            return kicloudBad( "a field needs a name" );
    }

    // staged before the change, so the commit keeps the old copy for undo
    SCH_COMMIT* commit = static_cast<SCH_COMMIT*>( getCurrentCommit( aCtx.ClientName ) );
    commit->Modify( symbol, screen );

    for( const schcmd::SchematicFieldValue& f : req.fields() )
        kicloudSetField( symbol, path, wxString::FromUTF8( f.name() ), wxString::FromUTF8( f.value() ) );

    if( !m_activeClients.count( aCtx.ClientName ) )
        pushCurrentCommit( aCtx.ClientName, _( "Set symbol fields via API" ) );

    schcmd::PlaceSymbolResponse response;
    kicloudPackSymbol( response.mutable_symbol(), symbol, path );
    return response;
}


std::unique_ptr<COMMIT> API_HANDLER_SCH::createCommit()
{
    return std::make_unique<SCH_COMMIT>( m_frame );
}


bool API_HANDLER_SCH::validateDocumentInternal( const DocumentSpecifier& aDocument ) const
{
    if( aDocument.type() != DocumentType::DOCTYPE_SCHEMATIC )
        return false;

    // TODO(JE) need serdes for SCH_SHEET_PATH <> SheetPath
    return true;

    //wxString currentPath = m_frame->GetCurrentSheet().PathAsString();
    //return 0 == aDocument.sheet_path().compare( currentPath.ToStdString() );
}


HANDLER_RESULT<GetOpenDocumentsResponse> API_HANDLER_SCH::handleGetOpenDocuments(
        const HANDLER_CONTEXT<GetOpenDocuments>& aCtx )
{
    if( aCtx.Request.type() != DocumentType::DOCTYPE_SCHEMATIC )
    {
        ApiResponseStatus e;

        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    GetOpenDocumentsResponse response;
    common::types::DocumentSpecifier doc;

    wxFileName fn( m_frame->GetCurrentFileName() );

    doc.set_type( DocumentType::DOCTYPE_SCHEMATIC );
    doc.set_board_filename( fn.GetFullName() );

    response.mutable_documents()->Add( std::move( doc ) );
    return response;
}


HANDLER_RESULT<std::unique_ptr<EDA_ITEM>> API_HANDLER_SCH::createItemForType( KICAD_T aType,
        EDA_ITEM* aContainer )
{
    if( !aContainer )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "Tried to create an item in a null container" );
        return tl::unexpected( e );
    }

    if( aType == SCH_PIN_T && !dynamic_cast<SCH_SYMBOL*>( aContainer ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "Tried to create a pin in {}, which is not a symbol",
                                          aContainer->GetFriendlyName().ToStdString() ) );
        return tl::unexpected( e );
    }
    else if( aType == SCH_SYMBOL_T && !dynamic_cast<SCHEMATIC*>( aContainer ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "Tried to create a symbol in {}, which is not a "
                                          "schematic",
                                          aContainer->GetFriendlyName().ToStdString() ) );
        return tl::unexpected( e );
    }

    std::unique_ptr<EDA_ITEM> created = CreateItemForType( aType, aContainer );

    if( !created )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "Tried to create an item of type {}, which is unhandled",
                                          magic_enum::enum_name( aType ) ) );
        return tl::unexpected( e );
    }

    return created;
}


HANDLER_RESULT<ItemRequestStatus> API_HANDLER_SCH::handleCreateUpdateItemsInternal( bool aCreate,
        const std::string& aClientName,
        const types::ItemHeader &aHeader,
        const google::protobuf::RepeatedPtrField<google::protobuf::Any>& aItems,
        std::function<void( ItemStatus, google::protobuf::Any )> aItemHandler )
{
    ApiResponseStatus e;

    auto containerResult = validateItemHeaderDocument( aHeader );

    if( !containerResult && containerResult.error().status() == ApiStatusCode::AS_UNHANDLED )
    {
        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }
    else if( !containerResult )
    {
        e.CopyFrom( containerResult.error() );
        return tl::unexpected( e );
    }

    // KICLOUD: S4.6 the sheet the header names (document.sheet_path), not only the one shown now
    HANDLER_RESULT<SCH_SHEET_PATH> sheet = kicloudSheetFor( aHeader.document() );

    if( !sheet )
        return tl::unexpected( sheet.error() );

    SCH_SCREEN* screen = sheet->LastScreen();
    EE_RTREE& screenItems = screen->Items();

    std::map<KIID, EDA_ITEM*> itemUuidMap;

    std::for_each( screenItems.begin(), screenItems.end(),
                   [&]( EDA_ITEM* aItem )
                   {
                       itemUuidMap[aItem->m_Uuid] = aItem;
                   } );

    EDA_ITEM* container = nullptr;

    if( containerResult->has_value() )
    {
        const KIID& containerId = **containerResult;

        if( itemUuidMap.count( containerId ) )
        {
            container = itemUuidMap.at( containerId );

            if( !container )
            {
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( fmt::format(
                        "The requested container {} is not a valid schematic item container",
                        containerId.AsStdString() ) );
                return tl::unexpected( e );
            }
        }
        else
        {
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format(
                    "The requested container {} does not exist in this document",
                    containerId.AsStdString() ) );
            return tl::unexpected( e );
        }
    }

    COMMIT* commit = getCurrentCommit( aClientName );

    for( const google::protobuf::Any& anyItem : aItems )
    {
        ItemStatus status;

        // KICLOUD: S4.6 the shared type table lists board items only; the wire and label
        // messages are recognised here and decoded with kicloudDecodeSchItem below.
        std::optional<KICAD_T> kicloudType = kicloudSchTypeFromAny( anyItem );
        std::optional<KICAD_T> type = kicloudType ? kicloudType : TypeNameFromAny( anyItem );

        if( !type )
        {
            status.set_code( ItemStatusCode::ISC_INVALID_TYPE );
            status.set_error_message( fmt::format( "Could not decode a valid type from {}",
                                                   anyItem.type_url() ) );
            aItemHandler( status, anyItem );
            continue;
        }

        // KICLOUD: S4.6 a wire or label is a top-level item of the sheet: it needs no container
        // (createItemForType refuses a null one, which made every top-level create fail)
        HANDLER_RESULT<std::unique_ptr<EDA_ITEM>> creationResult =
                kicloudType ? HANDLER_RESULT<std::unique_ptr<EDA_ITEM>>(
                                      CreateItemForType( *type, nullptr ) )
                            : createItemForType( *type, container );

        if( !creationResult )
        {
            status.set_code( ItemStatusCode::ISC_INVALID_TYPE );
            status.set_error_message( creationResult.error().error_message() );
            aItemHandler( status, anyItem );
            continue;
        }

        std::unique_ptr<EDA_ITEM> item( std::move( *creationResult ) );

        // KICLOUD: S4.6 wires and labels: units, wire layer and label text (see kicloudDecodeSchItem)
        const bool decoded = kicloudType
                ? kicloudDecodeSchItem( static_cast<SCH_ITEM*>( item.get() ), anyItem,
                                        m_frame->Schematic().Settings().m_DefaultTextSize )
                : item->Deserialize( anyItem );

        if( !decoded )
        {
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format( "could not unpack {} from request",
                                              item->GetClass().ToStdString() ) );
            return tl::unexpected( e );
        }

        if( aCreate && itemUuidMap.count( item->m_Uuid ) )
        {
            status.set_code( ItemStatusCode::ISC_EXISTING );
            status.set_error_message( fmt::format( "an item with UUID {} already exists",
                                                   item->m_Uuid.AsStdString() ) );
            aItemHandler( status, anyItem );
            continue;
        }
        else if( !aCreate && !itemUuidMap.count( item->m_Uuid ) )
        {
            status.set_code( ItemStatusCode::ISC_NONEXISTENT );
            status.set_error_message( fmt::format( "an item with UUID {} does not exist",
                                                   item->m_Uuid.AsStdString() ) );
            aItemHandler( status, anyItem );
            continue;
        }

        status.set_code( ItemStatusCode::ISC_OK );
        google::protobuf::Any newItem;

        if( aCreate )
        {
            // KICLOUD: S4.6 a wire or label answers in nanometres (kicloudEncodeSchItem)
            if( kicloudType )
                kicloudEncodeSchItem( static_cast<SCH_ITEM*>( item.get() ), newItem );
            else
                item->Serialize( newItem );

            commit->Add( item.release(), screen );

            if( !m_activeClients.count( aClientName ) )
                pushCurrentCommit( aClientName, _( "Added items via API" ) );
        }
        else
        {
            EDA_ITEM* edaItem = itemUuidMap[item->m_Uuid];

            if( SCH_ITEM* schItem = dynamic_cast<SCH_ITEM*>( edaItem ) )
            {
                // KICLOUD: S4.6 staged BEFORE the change: COMMIT::Modify keeps a copy of the item
                // as it is when staged, and upstream staged it after SwapItemData, so undo
                // "restored" the new data.
                commit->Modify( schItem, screen );
                schItem->SwapItemData( static_cast<SCH_ITEM*>( item.get() ) );

                if( kicloudType )
                    kicloudEncodeSchItem( schItem, newItem );
                else
                    schItem->Serialize( newItem );
            }
            else
            {
                wxASSERT( false );
            }

            if( !m_activeClients.count( aClientName ) )
                pushCurrentCommit( aClientName, _( "Created items via API" ) );
        }

        aItemHandler( status, newItem );
    }


    return ItemRequestStatus::IRS_OK;
}


// KICLOUD: S4.6 DeleteItems on the schematic (upstream left this a TODO, so every id answered
// IDS_NONEXISTENT and nothing was deleted). Each id is looked up as a
// top-level item of any sheet (symbol, wire, label, junction, ...) and removed through the
// client's commit; ids not found stay IDS_NONEXISTENT. Joins the client's open commit, else is
// one undo step.
void API_HANDLER_SCH::deleteItemsInternal( std::map<KIID, ItemDeletionStatus>& aItemsToDelete,
                                           const std::string& aClientName )
{
    std::vector<std::pair<SCH_ITEM*, SCH_SCREEN*>> found;

    for( std::pair<const KIID, ItemDeletionStatus>& pair : aItemsToDelete )
    {
        SCH_SCREEN* screen = nullptr;

        if( SCH_ITEM* item = kicloudFindItem( pair.first, &screen ) )
        {
            found.emplace_back( item, screen );
            pair.second = ItemDeletionStatus::IDS_OK;
        }
    }

    if( found.empty() )
        return;

    COMMIT* commit = getCurrentCommit( aClientName );

    for( const auto& [item, screen] : found )
        commit->Remove( item, screen );

    if( !m_activeClients.count( aClientName ) )
        pushCurrentCommit( aClientName, _( "Deleted items via API" ) );
}


// KICLOUD: S4.6 a top-level item of any sheet by id (HitTest uses it; upstream left a TODO and
// found nothing).
std::optional<EDA_ITEM*> API_HANDLER_SCH::getItemFromDocument( const DocumentSpecifier& aDocument,
                                                               const KIID& aId )
{
    if( !validateDocument( aDocument ) )
        return std::nullopt;

    if( SCH_ITEM* item = kicloudFindItem( aId, nullptr ) )
        return item;

    return std::nullopt;
}
