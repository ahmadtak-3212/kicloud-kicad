/*
 * This program source code file is part of KICAD, a free EDA CAD application.
 *
 * Copyright (C) 2013-2020 CERN
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 * @author Maciej Suminski <maciej.suminski@cern.ch>
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

#include <layer_ids.h>
#include <page_info.h>
#include <drawing_sheet/ds_proxy_view_item.h>
#include <drawing_sheet/ds_draw_item.h>
#include <drawing_sheet/ds_data_item.h>
#include <drawing_sheet/ds_painter.h>
#include <drawing_sheet/ds_data_model.h>   // KICLOUD: F-P5
#include <title_block.h>                    // KICLOUD: F-P5
#include <wx/datetime.h>                    // KICLOUD: F-P5
#include <gal/graphics_abstraction_layer.h>
#include <project.h>
#include <view/view.h>

using namespace KIGFX;

DS_PROXY_VIEW_ITEM::DS_PROXY_VIEW_ITEM( const EDA_IU_SCALE& aIuScale, const PAGE_INFO* aPageInfo,
                                        const PROJECT* aProject, const TITLE_BLOCK* aTitleBlock,
                                        const std::map<wxString, wxString>* aProperties ) :
        EDA_ITEM( NOT_USED ), // this item is never added to a BOARD so it needs no type
        m_iuScale( aIuScale ),
        m_titleBlock( aTitleBlock ),
        m_pageInfo( aPageInfo ),
        m_pageNumber( "1" ),
        m_sheetCount( 1 ),
        m_isFirstPage( false ),
        m_project( aProject ),
        m_properties( aProperties ),
        m_colorLayer( LAYER_DRAWINGSHEET ),
        m_pageBorderColorLayer( LAYER_PAGE_LIMITS )
{
}


// KICLOUD: F-P5: frees the cached draw list (see m_drawListCache). Called when the frame replaces or
// drops its drawing sheet item (a new one is made for every page-settings change).
DS_PROXY_VIEW_ITEM::~DS_PROXY_VIEW_ITEM() = default;


// KICLOUD: F-P5 (LB7): one string made of everything buildDrawList() and the text substitution of the
// drawing sheet read: the page (size, orientation), the title block fields, this item's page
// number, sheet count, first-page flag, file/sheet/variant names, the render settings it copies
// (line width, layer name), the board properties and project text variables (with the project's
// text-variable ticker), today's date (${CURRENT_DATE}), and the drawing sheet description itself
// (DS_DATA_MODEL: margins, defaults, and per item its address, type, coordinates, repeat settings
// and text). Called by ViewDraw() on every redraw; costs microseconds against the ~5-10 ms of a
// rebuild. Equal strings mean the list built last time would come out the same.
std::string DS_PROXY_VIEW_ITEM::drawListKey( VIEW* aView ) const
{
    RENDER_SETTINGS* settings = aView->GetPainter()->GetSettings();
    wxString         key;

    key << m_pageInfo->GetTypeAsString() << '|' << m_pageInfo->GetWidthMils() << 'x'
        << m_pageInfo->GetHeightMils() << '|' << ( m_pageInfo->IsPortrait() ? 'P' : 'L' ) << '|';

    if( m_titleBlock )
    {
        key << m_titleBlock->GetTitle() << '\x1f' << m_titleBlock->GetDate() << '\x1f'
            << m_titleBlock->GetRevision() << '\x1f' << m_titleBlock->GetCompany();

        for( int i = 0; i < 9; ++i )
            key << '\x1f' << m_titleBlock->GetComment( i );
    }

    key << '|' << wxString::FromUTF8( m_pageNumber ) << '|' << m_sheetCount << '|'
        << ( m_isFirstPage ? '1' : '0' ) << '|' << wxString::FromUTF8( m_fileName ) << '|'
        << wxString::FromUTF8( m_sheetName ) << '|' << wxString::FromUTF8( m_sheetPath ) << '|'
        << wxString::FromUTF8( m_variantName ) << '|' << wxString::FromUTF8( m_variantDesc ) << '|'
        << settings->GetDrawingSheetLineWidth() << '|' << settings->GetLayerName() << '|'
        << wxDateTime::Now().FormatISODate() << '|';

    if( m_properties )
    {
        for( const auto& [name, value] : *m_properties )
            key << name << '=' << value << '\x1f';
    }

    key << '|' << wxString::Format( wxS( "%p" ), m_project );

    if( m_project )
    {
        key << '#' << m_project->GetTextVarsTicker();

        for( const auto& [name, value] : m_project->GetTextVars() )
            key << name << '=' << value << '\x1f';
    }

    DS_DATA_MODEL& model = DS_DATA_MODEL::GetTheInstance();

    key << '|' << model.GetLeftMargin() << ',' << model.GetRightMargin() << ','
        << model.GetTopMargin() << ',' << model.GetBottomMargin() << ','
        << model.m_DefaultLineWidth << ',' << model.m_DefaultTextSize.x << ','
        << model.m_DefaultTextSize.y << ',' << model.m_DefaultTextThickness << '|';

    for( DS_DATA_ITEM* item : model.GetItems() )
    {
        key << wxString::Format( wxS( "%p" ), item ) << ':' << (int) item->GetType() << ':'
            << (int) item->GetPage1Option() << ':' << item->m_Pos.m_Pos.x << ','
            << item->m_Pos.m_Pos.y << ',' << item->m_Pos.m_Anchor << ':' << item->m_End.m_Pos.x
            << ',' << item->m_End.m_Pos.y << ',' << item->m_End.m_Anchor << ':'
            << item->m_LineWidth << ':' << item->m_RepeatCount << ':' << item->m_IncrementVector.x
            << ',' << item->m_IncrementVector.y << ':' << item->m_IncrementLabel;

        if( item->GetType() == DS_DATA_ITEM::DS_TEXT )
        {
            const DS_DATA_ITEM_TEXT* text = static_cast<const DS_DATA_ITEM_TEXT*>( item );
            key << ':' << text->m_TextBase << ':' << text->m_Orient << ':' << text->m_TextSize.x
                << ',' << text->m_TextSize.y << ':' << ( text->m_Bold ? 'B' : '-' )
                << ( text->m_Italic ? 'I' : '-' ) << ':' << text->m_BoundingBoxSize.x << ','
                << text->m_BoundingBoxSize.y;
        }

        key << ';';
    }

    return std::string( key.utf8_str() );
}


const BOX2I DS_PROXY_VIEW_ITEM::ViewBBox() const
{
    BOX2I bbox;

    if( m_pageInfo )
    {
        bbox.SetOrigin( VECTOR2I( 0, 0 ) );
        bbox.SetEnd( VECTOR2I( m_iuScale.MilsToIU( m_pageInfo->GetWidthMils() ),
                               m_iuScale.MilsToIU( m_pageInfo->GetHeightMils() ) ) );
    }
    else
    {
        bbox.SetMaximum();
    }

    return bbox;
}


void DS_PROXY_VIEW_ITEM::buildDrawList( VIEW* aView,
                                        const std::map<wxString, wxString>* aProperties,
                                        DS_DRAW_ITEM_LIST* aDrawList ) const
{
    RENDER_SETTINGS* settings = aView->GetPainter()->GetSettings();
    wxString         fileName( m_fileName.c_str(), wxConvUTF8 );
    wxString         sheetName( m_sheetName.c_str(), wxConvUTF8 );
    wxString         sheetPath( m_sheetPath.c_str(), wxConvUTF8 );
    wxString         variantName( m_variantName.c_str(), wxConvUTF8 );
    wxString         variantDesc( m_variantDesc.c_str(), wxConvUTF8 );

    aDrawList->SetDefaultPenSize( (int) settings->GetDrawingSheetLineWidth() );
    aDrawList->SetIsFirstPage( m_isFirstPage );
    aDrawList->SetPageNumber( m_pageNumber );
    aDrawList->SetSheetCount( m_sheetCount );
    aDrawList->SetFileName( fileName );
    aDrawList->SetSheetName( sheetName );
    aDrawList->SetSheetPath( sheetPath );
    aDrawList->SetSheetLayer( settings->GetLayerName() );
    aDrawList->SetVariantName( variantName );
    aDrawList->SetVariantDesc( variantDesc );
    aDrawList->SetProject( m_project );
    aDrawList->SetProperties( aProperties );

    aDrawList->BuildDrawItemsList( *m_pageInfo, *m_titleBlock );
}


// KICLOUD: F-P5 (LB7): the cached draw list, rebuilt first when it is missing, when drawListKey()
// changed, or when its items are no longer the current draw items of the drawing sheet's data
// items. That last check matters because the DS_DATA_ITEMs own the draw items of whichever list
// was built last: any other list built meanwhile (a plot, another view) deleted the cached list's
// items, so their pointers must not be used again (they are only compared here, never followed).
// Called by ViewDraw() and HitTestDrawingSheetItems() with m_drawListLock held. Result: the list,
// valid until the next call. State: m_drawListCache and m_drawListCacheKey.
DS_DRAW_ITEM_LIST& DS_PROXY_VIEW_ITEM::cachedDrawList( VIEW* aView ) const
{
    std::string key = drawListKey( aView );

    if( !m_drawListCache || key != m_drawListCacheKey || !cachedItemsAreCurrent() )
    {
        m_drawListCache = std::make_unique<DS_DRAW_ITEM_LIST>( m_iuScale );
        buildDrawList( aView, m_properties, m_drawListCache.get() );
        m_drawListCacheKey = std::move( key );
    }

    return *m_drawListCache;
}


// KICLOUD: F-P5: whether the cached list holds exactly the draw items the data items own now, in
// the order BuildDrawItemsList() appends them (data items skipped by their page option own items
// of some other build and are skipped here too). Pointers are compared, never dereferenced.
// Called by cachedDrawList() only.
bool DS_PROXY_VIEW_ITEM::cachedItemsAreCurrent() const
{
    std::vector<DS_DRAW_ITEM_BASE*> cached;

    for( DS_DRAW_ITEM_BASE* item = m_drawListCache->GetFirst(); item; item = m_drawListCache->GetNext() )
        cached.push_back( item );

    size_t next = 0;

    for( DS_DATA_ITEM* dataItem : DS_DATA_MODEL::GetTheInstance().GetItems() )
    {
        if( dataItem->GetPage1Option() == FIRST_PAGE_ONLY && !m_isFirstPage )
            continue;
        else if( dataItem->GetPage1Option() == SUBSEQUENT_PAGES && m_isFirstPage )
            continue;

        for( DS_DRAW_ITEM_BASE* owned : dataItem->GetDrawItems() )
        {
            if( next >= cached.size() || cached[next] != owned )
                return false;

            ++next;
        }
    }

    return next == cached.size();
}


// Paint the drawing sheet (frame, title block, page border) through the view's GAL. Called by
// KIGFX::VIEW for every redraw of LAYER_DRAWINGSHEET.
// KICLOUD: F-P5 (LB7): the draw list is built once and kept (m_drawListCache) until drawListKey()
// changes; before, every redraw rebuilt it, including the text substitution of every title-block
// field. State: may replace m_drawListCache/m_drawListCacheKey (under m_drawListLock).
void DS_PROXY_VIEW_ITEM::ViewDraw( int aLayer, VIEW* aView ) const
{
    GAL*              gal = aView->GetGAL();
    RENDER_SETTINGS*  settings = aView->GetPainter()->GetSettings();

    std::lock_guard<std::mutex> lock( m_drawListLock );
    DS_DRAW_ITEM_LIST&          drawList = cachedDrawList( aView );

    BOX2I viewport = BOX2ISafe( aView->GetViewport() );

    // Draw the title block normally even if the view is flipped
    bool flipped = gal->IsFlippedX();

    if( flipped )
    {
        int pageWidth = m_iuScale.MilsToIU( m_pageInfo->GetWidthMils() );

        gal->Save();
        gal->Translate( VECTOR2D( pageWidth, 0 ) );
        gal->Scale( VECTOR2D( -1.0, 1.0 ) );

        int right = pageWidth - viewport.GetLeft();
        int left = right - viewport.GetWidth();
        viewport.SetOrigin( left, viewport.GetTop() );
    }

    DS_PAINTER ws_painter( gal );
    auto       ws_settings = static_cast<DS_RENDER_SETTINGS*>( ws_painter.GetSettings() );

    ws_settings->SetNormalColor( settings->GetLayerColor( m_colorLayer ) );
    ws_settings->SetSelectedColor( settings->GetLayerColor( LAYER_SELECT_OVERLAY ) );
    ws_settings->SetBrightenedColor( settings->GetLayerColor( LAYER_BRIGHTENED ) );
    ws_settings->SetPageBorderColor( settings->GetLayerColor( m_pageBorderColorLayer ) );
    ws_settings->SetDefaultFont( settings->GetDefaultFont() );

    // Draw all the components that make the drawing sheet
    for( DS_DRAW_ITEM_BASE* item = drawList.GetFirst(); item; item = drawList.GetNext() )
    {
        if( viewport.Intersects( item->GetApproxBBox() ) )
            ws_painter.Draw( item, LAYER_DRAWINGSHEET );
    }

    // Draw gray line that outlines the sheet size
    if( settings->GetShowPageLimits() )
        ws_painter.DrawBorder( m_pageInfo, m_iuScale.IU_PER_MILS );

    if( flipped )
        gal->Restore();
}


std::vector<int> DS_PROXY_VIEW_ITEM::ViewGetLayers() const
{
    std::vector<int> layer{ LAYER_DRAWINGSHEET };
    return layer;
}


// Whether aPosition (board units) is on a line, text or other item of the drawing sheet, within
// five pixels. Called by the selection tool on clicks.
// KICLOUD: F-P5: uses the cached draw list (cachedDrawList) instead of building a new one: a new
// list would also delete the cached list's items (DS_DATA_ITEMs own the draw items of the list
// built last), so every click would force the next redraw to rebuild.
bool DS_PROXY_VIEW_ITEM::HitTestDrawingSheetItems( VIEW* aView, const VECTOR2I& aPosition )
{
    int                         accuracy = (int) aView->ToWorld( 5.0 );   // five pixels at current zoom
    std::lock_guard<std::mutex> lock( m_drawListLock );
    DS_DRAW_ITEM_LIST&          drawList = cachedDrawList( aView );

    for( DS_DRAW_ITEM_BASE* item = drawList.GetFirst(); item; item = drawList.GetNext() )
    {
        if( item->HitTest( aPosition, accuracy ) )
            return true;
    }

    return false;
}
