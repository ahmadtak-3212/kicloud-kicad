/*
 * This program source code file is part of kicloud, KiCad in the browser.
 *
 * Copyright (C) 2026 kicloud contributors.
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

// KICLOUD: L8.1 collab bridge hooks (docs/patches.md). The browser build's live-collaboration
// bridge (kicloud wasm/bindings) sets these; everywhere else they stay null and cost one
// pointer test. They let the bridge see the local undo stacks without owning them:
//   - UndoPushed: a command list was pushed onto the undo or redo stack (a new local edit, or
//     an undo/redo moving it to the other stack). The bridge records when that happened
//     relative to remote applies.
//   - BeforeUndoRedo: a command list is about to be put back into its previous state (undo,
//     redo or a rollback). The bridge reports items a remote apply changed after the list
//     was recorded (undo would overwrite a peer's edit); it does not change the outcome.
//   - ItemChangedUnnotified (L8.3a): a board item changed without a board-listener callback:
//     connectivity propagated a net to it (a cluster's net, or a zone's net to a via connected
//     only to zones), a commit's connectivity or teardrop cleanup changed, added or removed it
//     (aRemoved), or its net was removed from the board or orphaned (SanitizeNetcodes). The bridge
//     captures what a commit touched instead of diffing the whole board after every commit.
//   - DocumentModified (L8.3): the board editor's OnModify ran (after every commit, and after
//     the board setup and page settings dialogs, which change the file header without a
//     board-listener callback). The bridge checks the board settings then (COL-08).

#ifndef KICLOUD_COLLAB_HOOKS_H
#define KICLOUD_COLLAB_HOOKS_H

class EDA_BASE_FRAME;
class EDA_ITEM;
class PICKED_ITEMS_LIST;

struct KICLOUD_COLLAB_HOOKS
{
    static inline void ( *UndoPushed )( EDA_BASE_FRAME* aFrame, PICKED_ITEMS_LIST* aList,
                                        bool aRedoStack ) = nullptr;

    static inline void ( *BeforeUndoRedo )( EDA_BASE_FRAME* aFrame,
                                            PICKED_ITEMS_LIST* aList ) = nullptr;

    static inline void ( *ItemChangedUnnotified )( EDA_ITEM* aItem, bool aRemoved ) = nullptr;

    static inline void ( *DocumentModified )( EDA_BASE_FRAME* aFrame ) = nullptr;
};

#endif // KICLOUD_COLLAB_HOOKS_H
