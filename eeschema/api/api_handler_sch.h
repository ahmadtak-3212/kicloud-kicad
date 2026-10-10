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

#ifndef KICAD_API_HANDLER_SCH_H
#define KICAD_API_HANDLER_SCH_H

#include <api/api_handler_editor.h>
#include <api/common/commands/editor_commands.pb.h>
#include <api/schematic/schematic_commands.pb.h>   // KICLOUD: S4.6 the schematic commands
#include <kiid.h>
#include <sch_sheet_path.h>                         // KICLOUD: S4.6 kicloudSheetFor's result

using namespace kiapi;
using namespace kiapi::common;

class SCH_EDIT_FRAME;
class SCH_ITEM;
class SCH_SCREEN;


class API_HANDLER_SCH : public API_HANDLER_EDITOR
{
public:
    API_HANDLER_SCH( SCH_EDIT_FRAME* aFrame );

protected:
    std::unique_ptr<COMMIT> createCommit() override;

    kiapi::common::types::DocumentType thisDocumentType() const override
    {
        return kiapi::common::types::DOCTYPE_SCHEMATIC;
    }

    bool validateDocumentInternal( const DocumentSpecifier& aDocument ) const override;

    HANDLER_RESULT<std::unique_ptr<EDA_ITEM>> createItemForType( KICAD_T aType,
                                                                 EDA_ITEM* aContainer );

    HANDLER_RESULT<types::ItemRequestStatus> handleCreateUpdateItemsInternal( bool aCreate,
            const std::string& aClientName,
            const types::ItemHeader &aHeader,
            const google::protobuf::RepeatedPtrField<google::protobuf::Any>& aItems,
            std::function<void(commands::ItemStatus, google::protobuf::Any)> aItemHandler )
            override;

    void deleteItemsInternal( std::map<KIID, ItemDeletionStatus>& aItemsToDelete,
                              const std::string& aClientName ) override;

    std::optional<EDA_ITEM*> getItemFromDocument( const DocumentSpecifier& aDocument,
                                                  const KIID& aId ) override;

private:
    HANDLER_RESULT<commands::GetOpenDocumentsResponse> handleGetOpenDocuments(
            const HANDLER_CONTEXT<commands::GetOpenDocuments>& aCtx );

    // KICLOUD: S4.6 the schematic commands of api/proto/schematic/schematic_commands.proto
    // (list sheets, read a sheet, place a library symbol, draw wires, add a label, set fields).
    HANDLER_RESULT<kiapi::schematic::commands::SchematicHierarchyResponse>
    handleGetSchematicHierarchy(
            const HANDLER_CONTEXT<kiapi::schematic::commands::GetSchematicHierarchy>& aCtx );

    HANDLER_RESULT<kiapi::schematic::commands::GetSchematicItemsResponse> handleGetSchematicItems(
            const HANDLER_CONTEXT<kiapi::schematic::commands::GetSchematicItems>& aCtx );

    HANDLER_RESULT<kiapi::schematic::commands::PlaceSymbolResponse> handlePlaceSymbolFromLibrary(
            const HANDLER_CONTEXT<kiapi::schematic::commands::PlaceSymbolFromLibrary>& aCtx );

    HANDLER_RESULT<kiapi::schematic::commands::AddSchematicItemsResponse> handleAddSchematicWires(
            const HANDLER_CONTEXT<kiapi::schematic::commands::AddSchematicWires>& aCtx );

    HANDLER_RESULT<kiapi::schematic::commands::AddSchematicItemsResponse> handleAddSchematicLabel(
            const HANDLER_CONTEXT<kiapi::schematic::commands::AddSchematicLabel>& aCtx );

    HANDLER_RESULT<kiapi::schematic::commands::PlaceSymbolResponse> handleSetSymbolFields(
            const HANDLER_CONTEXT<kiapi::schematic::commands::SetSymbolFields>& aCtx );

    // KICLOUD: S4.6 the sheet a request names (document.sheet_path: KIID path, readable path or
    // sheet name; none = the sheet shown now), or a bad-request error naming the problem.
    HANDLER_RESULT<SCH_SHEET_PATH> kicloudSheetFor( const DocumentSpecifier& aDocument );

    // KICLOUD: S4.6 the screen that holds a top-level item with this id (any sheet), or nullptr.
    SCH_ITEM* kicloudFindItem( const KIID& aId, SCH_SCREEN** aScreen );

    SCH_EDIT_FRAME* m_frame;
};


#endif //KICAD_API_HANDLER_SCH_H
