/*
 * USER DCE functions
 *
 * Copyright 1993 Alexandre Julliard
 *	     1996,1997 Alex Korobka
 *
 *
 * Note: Visible regions of CS_OWNDC/CS_CLASSDC window DCs 
 * have to be updated dynamically. 
 * 
 * Internal DCX flags:
 *
 * DCX_DCEEMPTY    - dce is uninitialized
 * DCX_DCEBUSY     - dce is in use
 * DCX_DCEDIRTY    - ReleaseDC() should wipe instead of caching
 * DCX_KEEPCLIPRGN - ReleaseDC() should not delete the clipping region
 * DCX_WINDOWPAINT - BeginPaint() is in effect
 */

#include <assert.h>
#include "dce.h"
#include "win.h"
#include "gdi.h"
#include "region.h"
#include "user.h"
#include "debugtools.h"
#include "windef.h"
#include "wingdi.h"
#include "wine/winbase16.h"
#include "wine/winuser16.h"

DEFAULT_DEBUG_CHANNEL(dc);

static DCE *firstDCE;
static HDC defaultDCstate;

static void DCE_DeleteClipRgn( DCE* );
static INT DCE_ReleaseDC( DCE* );


/***********************************************************************
 *           DCE_DumpCache
 */
static void DCE_DumpCache(void)
{
    DCE *dce;
    
    WIN_LockWnds();
    dce = firstDCE;
    
    DPRINTF("DCE:\n");
    while( dce )
    {
	DPRINTF("\t[0x%08x] hWnd 0x%04x, dcx %08x, %s %s\n",
	     (unsigned)dce, dce->hwndCurrent, (unsigned)dce->DCXflags, 
	     (dce->DCXflags & DCX_CACHE) ? "Cache" : "Owned", 
	     (dce->DCXflags & DCX_DCEBUSY) ? "InUse" : "" );
	dce = dce->next;
    }

    WIN_UnlockWnds();
}

/***********************************************************************
 *           DCE_AllocDCE
 *
 * Allocate a new DCE.
 */
DCE *DCE_AllocDCE( HWND hWnd, DCE_TYPE type )
{
    FARPROC16 hookProc;
    DCE * dce;
    WND* wnd;
    
    if (!(dce = HeapAlloc( GetProcessHeap(), 0, sizeof(DCE) ))) return NULL;
    if (!(dce->hDC = CreateDCA( "DISPLAY", NULL, NULL, NULL )))
    {
        HeapFree( GetProcessHeap(), 0, dce );
	return 0;
    }
    if (!defaultDCstate) defaultDCstate = GetDCState16( dce->hDC );

    wnd = WIN_FindWndPtr(hWnd);
    
    /* store DCE handle in DC hook data field */

    hookProc = GetProcAddress16( GetModuleHandle16("USER"), (LPCSTR)362 );
    SetDCHook( dce->hDC, hookProc, (DWORD)dce );

    dce->hwndCurrent = hWnd;
    dce->hClipRgn    = 0;
    dce->next        = firstDCE;
    firstDCE = dce;

    if( type != DCE_CACHE_DC ) /* owned or class DC */
    {
	dce->DCXflags = DCX_DCEBUSY;
	if( hWnd )
	{
	    if( wnd->dwStyle & WS_CLIPCHILDREN ) dce->DCXflags |= DCX_CLIPCHILDREN;
	    if( wnd->dwStyle & WS_CLIPSIBLINGS ) dce->DCXflags |= DCX_CLIPSIBLINGS;
	}
	SetHookFlags16(dce->hDC,DCHF_INVALIDATEVISRGN);
    }
    else dce->DCXflags = DCX_CACHE | DCX_DCEEMPTY;

    WIN_ReleaseWndPtr(wnd);
    
    return dce;
}


/***********************************************************************
 *           DCE_FreeDCE
 */
DCE* DCE_FreeDCE( DCE *dce )
{
    DCE **ppDCE;

    if (!dce) return NULL;

    WIN_LockWnds();

    ppDCE = &firstDCE;

    while (*ppDCE && (*ppDCE != dce)) ppDCE = &(*ppDCE)->next;
    if (*ppDCE == dce) *ppDCE = dce->next;

    SetDCHook(dce->hDC, NULL, 0L);

    DeleteDC( dce->hDC );
    if( dce->hClipRgn && !(dce->DCXflags & DCX_KEEPCLIPRGN) )
	DeleteObject(dce->hClipRgn);
    HeapFree( GetProcessHeap(), 0, dce );

    WIN_UnlockWnds();
    
    return *ppDCE;
}

/***********************************************************************
 *           DCE_FreeWindowDCE
 *
 * Remove owned DCE and reset unreleased cache DCEs.
 */
void DCE_FreeWindowDCE( WND* pWnd )
{
    DCE *pDCE;

    WIN_LockWnds();
    pDCE = firstDCE;

    while( pDCE )
    {
	if( pDCE->hwndCurrent == pWnd->hwndSelf )
	{
	    if( pDCE == pWnd->dce ) /* owned or Class DCE*/
	    {
                if (pWnd->clsStyle & CS_OWNDC)	/* owned DCE*/
		{
                    pDCE = DCE_FreeDCE( pDCE );
                    pWnd->dce = NULL;
                    continue;
                }
		else if( pDCE->DCXflags & (DCX_INTERSECTRGN | DCX_EXCLUDERGN) )	/* Class DCE*/
		{
                    DCE_DeleteClipRgn( pDCE );
                    pDCE->hwndCurrent = 0;
		}
	    }
	    else
	    {
		if( pDCE->DCXflags & DCX_DCEBUSY ) /* shared cache DCE */
		{
                    /* FIXME: AFAICS we are doing the right thing here so 
                     * this should be a WARN. But this is best left as an ERR 
                     * because the 'application error' is likely to come from 
                     * another part of Wine (i.e. it's our fault after all). 
                     * We should change this to WARN when Wine is more stable
                     * (for 1.0?).
                     */
		    ERR("[%04x] GetDC() without ReleaseDC()!\n", 
			pWnd->hwndSelf);
		    DCE_ReleaseDC( pDCE );
		}

		pDCE->DCXflags &= DCX_CACHE;
		pDCE->DCXflags |= DCX_DCEEMPTY;
		pDCE->hwndCurrent = 0;
	    }
	}
	pDCE = pDCE->next;
    }
    
    WIN_UnlockWnds();
}


/***********************************************************************
 *   DCE_DeleteClipRgn
 */
static void DCE_DeleteClipRgn( DCE* dce )
{
    dce->DCXflags &= ~(DCX_EXCLUDERGN | DCX_INTERSECTRGN | DCX_WINDOWPAINT);

    if( dce->DCXflags & DCX_KEEPCLIPRGN )
	dce->DCXflags &= ~DCX_KEEPCLIPRGN;
    else
	if( dce->hClipRgn > 1 )
	    DeleteObject( dce->hClipRgn );

    dce->hClipRgn = 0;

    /* make it dirty so that the vis rgn gets recomputed next time */
    dce->DCXflags |= DCX_DCEDIRTY;
    SetHookFlags16( dce->hDC, DCHF_INVALIDATEVISRGN );
}


/***********************************************************************
 *   DCE_ReleaseDC
 */
static INT DCE_ReleaseDC( DCE* dce )
{
    if ((dce->DCXflags & (DCX_DCEEMPTY | DCX_DCEBUSY)) != DCX_DCEBUSY) return 0;

    /* restore previous visible region */

    if ((dce->DCXflags & (DCX_INTERSECTRGN | DCX_EXCLUDERGN)) &&
        (dce->DCXflags & (DCX_CACHE | DCX_WINDOWPAINT)) )
        DCE_DeleteClipRgn( dce );

    if (dce->DCXflags & DCX_CACHE)
    {
        SetDCState16( dce->hDC, defaultDCstate );
        dce->DCXflags &= ~DCX_DCEBUSY;
	if (dce->DCXflags & DCX_DCEDIRTY)
	{
	    /* don't keep around invalidated entries 
	     * because SetDCState() disables hVisRgn updates
	     * by removing dirty bit. */

	    dce->hwndCurrent = 0;
	    dce->DCXflags &= DCX_CACHE;
	    dce->DCXflags |= DCX_DCEEMPTY;
	}
    }
    return 1;
}


/***********************************************************************
 *   DCE_InvalidateDCE
 *
 * It is called from SetWindowPos() and EVENT_MapNotify - we have to
 * mark as dirty all busy DCEs for windows that have pWnd->parent as
 * an ancestor and whose client rect intersects with specified update
 * rectangle. In addition, pWnd->parent DCEs may need to be updated if
 * DCX_CLIPCHILDREN flag is set.  */
BOOL DCE_InvalidateDCE(WND* pWnd, const RECT* pRectUpdate)
{
    WND* wndScope = WIN_LockWndPtr(pWnd->parent);
    WND *pDesktop = WIN_GetDesktop();
    BOOL bRet = FALSE;

    if( wndScope )
    {
	DCE *dce;

	TRACE("scope hwnd = %04x, (%i,%i - %i,%i)\n",
		     wndScope->hwndSelf, pRectUpdate->left,pRectUpdate->top,
		     pRectUpdate->right,pRectUpdate->bottom);
	if(TRACE_ON(dc)) 
	  DCE_DumpCache();

 	/* walk all DCEs and fixup non-empty entries */

	for (dce = firstDCE; (dce); dce = dce->next)
	{
	    if( !(dce->DCXflags & DCX_DCEEMPTY) )
	    {
		WND* wndCurrent = WIN_FindWndPtr(dce->hwndCurrent);

		if( wndCurrent )
		{
		    WND* wnd = NULL;
		    INT xoffset = 0, yoffset = 0;

                    if( (wndCurrent == wndScope) && !(dce->DCXflags & DCX_CLIPCHILDREN) )
                    {
			/* child window positions don't bother us */
                        WIN_ReleaseWndPtr(wndCurrent);
                        continue;
                    }

		    if (wndCurrent == pDesktop && !(wndCurrent->flags & WIN_NATIVE))
		    {
			/* don't bother with fake desktop */
                        WIN_ReleaseWndPtr(wndCurrent);
			continue;
		    }

		    /* check if DCE window is within the z-order scope */

		    for( wnd = WIN_LockWndPtr(wndCurrent); wnd; WIN_UpdateWndPtr(&wnd,wnd->parent))
		    {
			if( wnd == wndScope )
		 	{
			    RECT wndRect;

			    wndRect = wndCurrent->rectWindow;

			    OffsetRect( &wndRect, xoffset - wndCurrent->rectClient.left, 
						    yoffset - wndCurrent->rectClient.top);

			    if (pWnd == wndCurrent ||
				IntersectRect( &wndRect, &wndRect, pRectUpdate ))
			    { 
				if( !(dce->DCXflags & DCX_DCEBUSY) )
				{
				    /* Don't bother with visible regions of unused DCEs */

				    TRACE("\tpurged %08x dce [%04x]\n", 
						(unsigned)dce, wndCurrent->hwndSelf);

				    dce->hwndCurrent = 0;
				    dce->DCXflags &= DCX_CACHE;
				    dce->DCXflags |= DCX_DCEEMPTY;
				}
				else
				{
				    /* Set dirty bits in the hDC and DCE structs */

				    TRACE("\tfixed up %08x dce [%04x]\n", 
						(unsigned)dce, wndCurrent->hwndSelf);

				    dce->DCXflags |= DCX_DCEDIRTY;
				    SetHookFlags16(dce->hDC, DCHF_INVALIDATEVISRGN);
				    bRet = TRUE;
				}
			    }
                            WIN_ReleaseWndPtr(wnd);
			    break;
			}
			xoffset += wnd->rectClient.left;
			yoffset += wnd->rectClient.top;
		    }
		}
                WIN_ReleaseWndPtr(wndCurrent);
	    }
	} /* dce list */
        WIN_ReleaseWndPtr(wndScope);
    }
    WIN_ReleaseDesktop();
    return bRet;
}


/***********************************************************************
 *           DCE_ExcludeRgn
 * 
 *  Translate given region from the wnd client to the DC coordinates
 *  and add it to the clipping region.
 */
INT DCE_ExcludeRgn( HDC hDC, WND* wnd, HRGN hRgn )
{
  POINT  pt = {0, 0};
  DCE     *dce = firstDCE;

  while (dce && (dce->hDC != hDC)) dce = dce->next;
  if( dce )
  {
      MapWindowPoints( wnd->hwndSelf, dce->hwndCurrent, &pt, 1);
      if( dce->DCXflags & DCX_WINDOW )
      { 
	  wnd = WIN_FindWndPtr(dce->hwndCurrent);
	  pt.x += wnd->rectClient.left - wnd->rectWindow.left;
	  pt.y += wnd->rectClient.top - wnd->rectWindow.top;
          WIN_ReleaseWndPtr(wnd);
      }
  }
  else return ERROR;
  OffsetRgn(hRgn, pt.x, pt.y);

  return ExtSelectClipRgn( hDC, hRgn, RGN_DIFF );
}


/***********************************************************************
 *		GetDCEx (USER.359)
 */
HDC16 WINAPI GetDCEx16( HWND16 hwnd, HRGN16 hrgnClip, DWORD flags )
{
    return (HDC16)GetDCEx( hwnd, hrgnClip, flags );
}


/***********************************************************************
 *		GetDCEx (USER32.@)
 *
 * Unimplemented flags: DCX_LOCKWINDOWUPDATE
 *
 * FIXME: Full support for hrgnClip == 1 (alias for entire window).
 */
HDC WINAPI GetDCEx( HWND hwnd, HRGN hrgnClip, DWORD flags )
{
    HDC 	hdc = 0;
    DCE * 	dce;
    WND * 	wndPtr;
    DWORD 	dcxFlags = 0;
    BOOL	bUpdateVisRgn = TRUE;
    BOOL	bUpdateClipOrigin = FALSE;

    TRACE("hwnd %04x, hrgnClip %04x, flags %08x\n", 
          hwnd, hrgnClip, (unsigned)flags);

    if (!hwnd) hwnd = GetDesktopWindow();
    if (!(wndPtr = WIN_FindWndPtr( hwnd ))) return 0;

    /* fixup flags */

    if (!wndPtr->dce) flags |= DCX_CACHE;

    if (flags & DCX_USESTYLE)
    {
	flags &= ~( DCX_CLIPCHILDREN | DCX_CLIPSIBLINGS | DCX_PARENTCLIP);

        if( wndPtr->dwStyle & WS_CLIPSIBLINGS )
            flags |= DCX_CLIPSIBLINGS;

	if ( !(flags & DCX_WINDOW) )
	{
            if (wndPtr->clsStyle & CS_PARENTDC) flags |= DCX_PARENTCLIP;

	    if (wndPtr->dwStyle & WS_CLIPCHILDREN &&
                     !(wndPtr->dwStyle & WS_MINIMIZE) ) flags |= DCX_CLIPCHILDREN;
	}
	else flags |= DCX_CACHE;
    }

    if (flags & DCX_WINDOW) 
	flags = (flags & ~DCX_CLIPCHILDREN) | DCX_CACHE;

    if (!wndPtr->parent || (wndPtr->parent->hwndSelf == GetDesktopWindow()))
        flags = (flags & ~DCX_PARENTCLIP) | DCX_CLIPSIBLINGS;
    else if( flags & DCX_PARENTCLIP )
    {
	flags |= DCX_CACHE;
	if( !(flags & (DCX_CLIPSIBLINGS | DCX_CLIPCHILDREN)) )
	    if( (wndPtr->dwStyle & WS_VISIBLE) && (wndPtr->parent->dwStyle & WS_VISIBLE) )
	    {
		flags &= ~DCX_CLIPCHILDREN;
		if( wndPtr->parent->dwStyle & WS_CLIPSIBLINGS )
		    flags |= DCX_CLIPSIBLINGS;
	    }
    }

    /* find a suitable DCE */

    dcxFlags = flags & (DCX_PARENTCLIP | DCX_CLIPSIBLINGS | DCX_CLIPCHILDREN | 
		        DCX_CACHE | DCX_WINDOW);

    if (flags & DCX_CACHE)
    {
	DCE*	dceEmpty;
	DCE*	dceUnused;

	dceEmpty = dceUnused = NULL;

	/* Strategy: First, we attempt to find a non-empty but unused DCE with
	 * compatible flags. Next, we look for an empty entry. If the cache is
	 * full we have to purge one of the unused entries.
	 */

	for (dce = firstDCE; (dce); dce = dce->next)
	{
	    if ((dce->DCXflags & (DCX_CACHE | DCX_DCEBUSY)) == DCX_CACHE )
	    {
		dceUnused = dce;

		if (dce->DCXflags & DCX_DCEEMPTY)
		    dceEmpty = dce;
		else
		if ((dce->hwndCurrent == hwnd) &&
		   ((dce->DCXflags & (DCX_CLIPSIBLINGS | DCX_CLIPCHILDREN |
				      DCX_CACHE | DCX_WINDOW | DCX_PARENTCLIP)) == dcxFlags))
		{
		    TRACE("\tfound valid %08x dce [%04x], flags %08x\n", 
					(unsigned)dce, hwnd, (unsigned)dcxFlags );
		    bUpdateVisRgn = FALSE; 
		    bUpdateClipOrigin = TRUE;
		    break;
		}
	    }
	}

	if (!dce) dce = (dceEmpty) ? dceEmpty : dceUnused;
        
        /* if there's no dce empty or unused, allocate a new one */
        if (!dce)
        {
            dce = DCE_AllocDCE( 0, DCE_CACHE_DC );
        }
    }
    else 
    {
        dce = wndPtr->dce;
	if( dce->hwndCurrent == hwnd )
	{
	    TRACE("\tskipping hVisRgn update\n");
	    bUpdateVisRgn = FALSE; /* updated automatically, via DCHook() */
	}
    }
    if (!dce)
    {
        hdc = 0;
        goto END;
    }

    if (!(flags & (DCX_INTERSECTRGN | DCX_EXCLUDERGN))) hrgnClip = 0;

    if (((flags ^ dce->DCXflags) & (DCX_INTERSECTRGN | DCX_EXCLUDERGN)) &&
        (dce->hClipRgn != hrgnClip))
    {
        /* if the extra clip region has changed, get rid of the old one */
        DCE_DeleteClipRgn( dce );
    }

    dce->hwndCurrent = hwnd;
    dce->hClipRgn = hrgnClip;
    dce->DCXflags = flags & (DCX_PARENTCLIP | DCX_CLIPSIBLINGS | DCX_CLIPCHILDREN |
                             DCX_CACHE | DCX_WINDOW | DCX_WINDOWPAINT |
                             DCX_KEEPCLIPRGN | DCX_INTERSECTRGN | DCX_EXCLUDERGN);
    dce->DCXflags |= DCX_DCEBUSY;
    dce->DCXflags &= ~DCX_DCEDIRTY;
    hdc = dce->hDC;

    if (bUpdateVisRgn) SetHookFlags16( hdc, DCHF_INVALIDATEVISRGN ); /* force update */

    if (!USER_Driver.pGetDC( hwnd, hdc, hrgnClip, flags )) hdc = 0;

    TRACE("(%04x,%04x,0x%lx): returning %04x\n", hwnd, hrgnClip, flags, hdc);
END:
    WIN_ReleaseWndPtr(wndPtr);
    return hdc;
}


/***********************************************************************
 *		GetDC (USER.66)
 */
HDC16 WINAPI GetDC16( HWND16 hwnd )
{
    return (HDC16)GetDC( hwnd );
}


/***********************************************************************
 *		GetDC (USER32.@)
 * RETURNS
 *	:Handle to DC
 *	NULL: Failure
 */
HDC WINAPI GetDC(
	     HWND hwnd /* [in] handle of window */
) {
    if (!hwnd)
        return GetDCEx( 0, 0, DCX_CACHE | DCX_WINDOW );
    return GetDCEx( hwnd, 0, DCX_USESTYLE );
}


/***********************************************************************
 *		GetWindowDC (USER.67)
 */
HDC16 WINAPI GetWindowDC16( HWND16 hwnd )
{
    return GetDCEx16( hwnd, 0, DCX_USESTYLE | DCX_WINDOW );
}


/***********************************************************************
 *		GetWindowDC (USER32.@)
 */
HDC WINAPI GetWindowDC( HWND hwnd )
{
    return GetDCEx( hwnd, 0, DCX_USESTYLE | DCX_WINDOW );
}


/***********************************************************************
 *		ReleaseDC (USER.68)
 */
INT16 WINAPI ReleaseDC16( HWND16 hwnd, HDC16 hdc )
{
    return (INT16)ReleaseDC( hwnd, hdc );
}


/***********************************************************************
 *		ReleaseDC (USER32.@)
 *
 * RETURNS
 *	1: Success
 *	0: Failure
 */
INT WINAPI ReleaseDC( 
             HWND hwnd /* [in] Handle of window - ignored */, 
             HDC hdc   /* [in] Handle of device context */
) {
    DCE * dce;
    INT nRet = 0;

    WIN_LockWnds();
    dce = firstDCE;
    
    TRACE("%04x %04x\n", hwnd, hdc );
        
    while (dce && (dce->hDC != hdc)) dce = dce->next;

    if ( dce ) 
	if ( dce->DCXflags & DCX_DCEBUSY )
            nRet = DCE_ReleaseDC( dce );

    WIN_UnlockWnds();

    return nRet;
}

/***********************************************************************
 *		DCHook (USER.362)
 *
 * See "Undoc. Windows" for hints (DC, SetDCHook, SetHookFlags)..  
 */
BOOL16 WINAPI DCHook16( HDC16 hDC, WORD code, DWORD data, LPARAM lParam )
{
    BOOL retv = TRUE;
    DCE *dce = (DCE *)data;

    TRACE("hDC = %04x, %i\n", hDC, code);

    if (!dce) return 0;
    assert(dce->hDC == hDC);

    /* Grab the windows lock before doing anything else  */
    WIN_LockWnds();

    switch( code )
    {
      case DCHC_INVALIDVISRGN:
	   /* GDI code calls this when it detects that the
	    * DC is dirty (usually after SetHookFlags()). This
	    * means that we have to recompute the visible region.
	    */
           if( dce->DCXflags & DCX_DCEBUSY )
           {
               /* Dirty bit has been cleared by caller, set it again so that
                * pGetDC recomputes the visible region. */
               SetHookFlags16( dce->hDC, DCHF_INVALIDATEVISRGN );
               USER_Driver.pGetDC( dce->hwndCurrent, dce->hDC, dce->hClipRgn, dce->DCXflags );
           }
           else /* non-fatal but shouldn't happen */
	     WARN("DC is not in use!\n");
	   break;

      case DCHC_DELETEDC:
           /*
            * Windows will not let you delete a DC that is busy
            * (between GetDC and ReleaseDC)
            */

           if ( dce->DCXflags & DCX_DCEBUSY )
           {
               WARN("Application trying to delete a busy DC\n");
               retv = FALSE;
           }
	   break;

      default:
	   FIXME("unknown code\n");
    }

  WIN_UnlockWnds();  /* Release the wnd lock */
  return retv;
}


/**********************************************************************
 *		WindowFromDC (USER.117)
 */
HWND16 WINAPI WindowFromDC16( HDC16 hDC )
{
    return (HWND16)WindowFromDC( hDC );
}


/**********************************************************************
 *		WindowFromDC (USER32.@)
 */
HWND WINAPI WindowFromDC( HDC hDC )
{
    DCE *dce;
    HWND hwnd;

    WIN_LockWnds();
    dce = firstDCE;
    
    while (dce && (dce->hDC != hDC)) dce = dce->next;

    hwnd = dce ? dce->hwndCurrent : 0;
    WIN_UnlockWnds();
    
    return hwnd;
}


/***********************************************************************
 *		LockWindowUpdate (USER.294)
 */
BOOL16 WINAPI LockWindowUpdate16( HWND16 hwnd )
{
    return LockWindowUpdate( hwnd );
}


/***********************************************************************
 *		LockWindowUpdate (USER32.@)
 */
BOOL WINAPI LockWindowUpdate( HWND hwnd )
{
    FIXME("DCX_LOCKWINDOWUPDATE is unimplemented\n");
    return TRUE;
}
