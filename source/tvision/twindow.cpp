/*------------------------------------------------------------*/
/* filename -       twindow.cpp                               */
/*                                                            */
/* function(s)                                                */
/*                  TWindow member functions                  */
/*------------------------------------------------------------*/
/*
 *      Turbo Vision - Version 2.0
 *
 *      Copyright (c) 1994 by Borland International
 *      All Rights Reserved.
 *
 */

#define Uses_TKeys
#define Uses_TWindow
#define Uses_TEvent
#define Uses_TRect
#define Uses_TFrame
#define Uses_TCommandSet
#define Uses_TScrollBar
#define Uses_opstream
#define Uses_ipstream
#include <tvision/tv.h>

#if !defined( __STRING_H )
#include <string.h>
#endif  // __STRING_H

const TPoint minWinSize = {16, 6};

TWindowInit::TWindowInit( TFrame *(*cFrame)( TRect ) ) noexcept :
    createFrame( cFrame )
{
}

TWindow::TWindow( const TRect& bounds,
                  TStringView aTitle,
                  short aNumber
                ) noexcept :
    TWindowInit( &TWindow::initFrame ),
    TGroup( bounds ),
    flags( wfMove | wfGrow | wfClose | wfZoom ),
    zoomRect( getBounds() ),
    number( aNumber ),
    palette( wpBlueWindow ),
    title( newStr( aTitle ) )
{
    state |= sfShadow;
    options |= ofSelectable | ofTopSelect;
    growMode = gfGrowAll | gfGrowRel;

    if( createFrame != 0 &&
        (frame = createFrame( getExtent() )) != 0
      )
        insert( frame );
}

TWindow::~TWindow()
{
    delete[] (char *)title;
}

void TWindow::close()
{
    if( valid( cmClose ) )
    {
        frame = 0;  // so we don't try to use the frame after it's been deleted
        destroy( this );
    }
}

void TWindow::shutDown()
{
    frame = 0;
    TGroup::shutDown();
}

TPalette& TWindow::getPalette() const
{
    static TPalette blue( cpBlueWindow, sizeof( cpBlueWindow )-1 );
    static TPalette cyan( cpCyanWindow, sizeof( cpCyanWindow )-1 );
    static TPalette gray( cpGrayWindow, sizeof( cpGrayWindow )-1 );
    static TPalette *palettes[] =
        {
        &blue,
        &cyan,
        &gray
        };
    return *(palettes[palette]);
}

const char *TWindow::getTitle( short )
{
    return title;
}

void TWindow::handleEvent( TEvent& event )
{
 TRect  limits;
 TPoint min, max;

    TGroup::handleEvent(event);
    if( event.what== evCommand )
        switch (event.message.command)
            {
            case  cmResize:
                if( (flags & (wfMove | wfGrow)) != 0 )
                    {
                    limits = owner->getExtent();
                    sizeLimits(min, max);
                    dragView( event, dragMode | (flags & (wfMove | wfGrow)),
                              limits, min, max);
                    clearEvent(event);
                    }
                break;
            case  cmClose:
                if( (flags & wfClose) != 0 &&
                    ( event.message.infoPtr == 0 || event.message.infoPtr == this )
                  )
                    {
                    clearEvent(event);
                    if( (state & sfModal) == 0 )
                        close();
                    else
                        {
                        event.what = evCommand;
                        event.message.command = cmCancel;
                        putEvent( event );
                        clearEvent( event );
                        }
                    }
                break;
            case  cmZoom:
                if( (flags & wfZoom) != 0 &&
                    (event.message.infoPtr == 0 || event.message.infoPtr == this)
                  )
                    {
                    zoom();
                    clearEvent(event);
                    }
                break;
            }
    else if( event.what == evKeyDown )
            switch (event.keyDown.keyCode)
                {
                case  kbTab:
                    focusNext(False);
                    clearEvent(event);
                    break;
                case  kbShiftTab:
                    focusNext(True);
                    clearEvent(event);
                    break;
                }
    else if( event.what == evBroadcast &&
             event.message.command == cmSelectWindowNum &&
             event.message.infoInt == number &&
             (options & ofSelectable) != 0
           )
            {
            select();
            clearEvent(event);
            }
}

TFrame *TWindow::initFrame( TRect r )
{
    return new TFrame(r);
}

void TWindow::setState( ushort aState, Boolean enable )
{
    TCommandSet windowCommands;

    TGroup::setState(aState, enable);
    if( (aState & sfSelected) != 0 )
        {
        setState(sfActive, enable);
        if( frame != 0 )
            frame->setState(sfActive,enable);
        windowCommands += cmNext;
        windowCommands += cmPrev;
        if( (flags & (wfGrow | wfMove)) != 0 )
            windowCommands += cmResize;
        if( (flags & wfClose) != 0 )
            windowCommands += cmClose;
        if( (flags & wfZoom) != 0 )
            windowCommands += cmZoom;
        if( enable != False )
            enableCommands(windowCommands);
        else
            disableCommands(windowCommands);
        }
}

TScrollBar *TWindow::standardScrollBar( ushort aOptions ) noexcept
{
    TRect r = getExtent();
    if( (aOptions & sbVertical) != 0 )
        r = TRect( r.b.x-1, r.a.y+1, r.b.x, r.b.y-1 );
    else
        r = TRect( r.a.x+2, r.b.y-1, r.b.x-2, r.b.y );

    TScrollBar *s;
    insert( s = new TScrollBar(r) );
    if( (aOptions & sbHandleKeyboard) != 0 )
        s->options |= ofPostProcess;
    return s;
}

void TWindow::sizeLimits( TPoint& min, TPoint& max )
{
    TView::sizeLimits(min, max);
    min = minWinSize;
}

static inline int range( int val, int min, int max )
{
    return val < min ? min : val > max ? max : val;
}

// zoomRect holds the bounds the window had when it was zoomed, in the owner's
// coordinates and against the owner as it was at that moment. If the owner has
// changed size since -- the terminal was resized while the window was
// maximized -- restoring the rectangle verbatim can leave the window outside
// it: locate() clamps the size against sizeLimits and takes the origin as
// given, which is what its other callers need, since moveGrow() has already
// done its own clamping and the Esc path deliberately restores bounds that may
// hang off an edge. So reconcile the two here, where the stale rectangle is.
//
// The size is clamped first, the way locate() will, and the origin is then
// slid back inside the owner; a window too large to fit is pinned at the
// owner's origin rather than dragged negative.
static TRect fittedToOwner( const TRect& r, TPoint minSize, TPoint maxSize,
                            TPoint ownerSize )
{
    int w = range( r.b.x - r.a.x, minSize.x, maxSize.x );
    int h = range( r.b.y - r.a.y, minSize.y, maxSize.y );
    int x = range( r.a.x, 0, ownerSize.x - w < 0 ? 0 : ownerSize.x - w );
    int y = range( r.a.y, 0, ownerSize.y - h < 0 ? 0 : ownerSize.y - h );
    return TRect( x, y, x + w, y + h );
}

void TWindow::zoom()
{
    TPoint minSize, maxSize;
    sizeLimits( minSize, maxSize );
    if( size != maxSize )
        {
        zoomRect = getBounds();
        TRect r( 0, 0, maxSize.x, maxSize.y );
        locate(r);
        }
    else
        {
        TRect r = owner == 0 ? zoomRect
                             : fittedToOwner( zoomRect, minSize, maxSize,
                                              owner->size );
        locate( r );
        }
}

#if !defined(NO_STREAMABLE)

void TWindow::write( opstream& os )
{
    TGroup::write( os );
    os << flags << zoomRect << number << palette;
    os << frame;
    os.writeString( title );
}

void *TWindow::read( ipstream& is )
{
    TGroup::read( is );
    is >> flags >> zoomRect >> number >> palette;
    is >> frame;
    title = is.readString();
    return this;
}

TStreamable *TWindow::build()
{
    return new TWindow( streamableInit );
}

TWindow::TWindow( StreamableInit ) noexcept :
    TWindowInit( 0 ),
    TGroup( streamableInit )
{
}

#endif
