/*
 * Copyright 2012 Hans Leidekker for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define COBJMACROS

#include "config.h"
#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "objbase.h"
#include "wbemcli.h"

#include "wine/debug.h"
#include "wine/unicode.h"
#include "wbemprox_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(wbemprox);

struct client_security
{
    IClientSecurity IClientSecurity_iface;
};

static inline struct client_security *impl_from_IClientSecurity( IClientSecurity *iface )
{
    return CONTAINING_RECORD( iface, struct client_security, IClientSecurity_iface );
}

static HRESULT WINAPI client_security_QueryInterface(
    IClientSecurity *iface,
    REFIID riid,
    void **ppvObject )
{
    struct client_security *cs = impl_from_IClientSecurity( iface );

    TRACE("%p %s %p\n", cs, debugstr_guid( riid ), ppvObject );

    if ( IsEqualGUID( riid, &IID_IClientSecurity ) ||
         IsEqualGUID( riid, &IID_IUnknown ) )
    {
        *ppvObject = cs;
    }
    else
    {
        FIXME("interface %s not implemented\n", debugstr_guid(riid));
        return E_NOINTERFACE;
    }
    IClientSecurity_AddRef( iface );
    return S_OK;
}

static ULONG WINAPI client_security_AddRef(
    IClientSecurity *iface )
{
    FIXME("%p\n", iface);
    return 2;
}

static ULONG WINAPI client_security_Release(
    IClientSecurity *iface )
{
    FIXME("%p\n", iface);
    return 1;
}

static HRESULT WINAPI client_security_QueryBlanket(
    IClientSecurity *iface,
    IUnknown *pProxy,
    DWORD *pAuthnSvc,
    DWORD *pAuthzSvc,
    OLECHAR **pServerPrincName,
    DWORD *pAuthnLevel,
    DWORD *pImpLevel,
    void **pAuthInfo,
    DWORD *pCapabilities )
{
    FIXME("\n");
    return WBEM_E_FAILED;
}

static HRESULT WINAPI client_security_SetBlanket(
    IClientSecurity *iface,
    IUnknown *pProxy,
    DWORD AuthnSvc,
    DWORD AuthzSvc,
    OLECHAR *pServerPrincName,
    DWORD AuthnLevel,
    DWORD ImpLevel,
    void *pAuthInfo,
    DWORD Capabilities )
{
    static const OLECHAR defaultW[] =
        {'<','C','O','L','E','_','D','E','F','A','U','L','T','_','P','R','I','N','C','I','P','A','L','>',0};
    const OLECHAR *princname = (pServerPrincName == COLE_DEFAULT_PRINCIPAL) ? defaultW : pServerPrincName;

    FIXME("%p, %p, %u, %u, %s, %u, %u, %p, 0x%08x\n", iface, pProxy, AuthnSvc, AuthzSvc,
          debugstr_w(princname), AuthnLevel, ImpLevel, pAuthInfo, Capabilities);
    return WBEM_NO_ERROR;
}

static HRESULT WINAPI client_security_CopyProxy(
    IClientSecurity *iface,
    IUnknown *pProxy,
    IUnknown **ppCopy )
{
    FIXME("\n");
    return WBEM_E_FAILED;
}

static const IClientSecurityVtbl client_security_vtbl =
{
    client_security_QueryInterface,
    client_security_AddRef,
    client_security_Release,
    client_security_QueryBlanket,
    client_security_SetBlanket,
    client_security_CopyProxy
};

IClientSecurity client_security = { &client_security_vtbl };

struct wbem_services
{
    IWbemServices IWbemServices_iface;
    LONG refs;
    WCHAR *namespace;
};

static inline struct wbem_services *impl_from_IWbemServices( IWbemServices *iface )
{
    return CONTAINING_RECORD( iface, struct wbem_services, IWbemServices_iface );
}

static ULONG WINAPI wbem_services_AddRef(
    IWbemServices *iface )
{
    struct wbem_services *ws = impl_from_IWbemServices( iface );
    return InterlockedIncrement( &ws->refs );
}

static ULONG WINAPI wbem_services_Release(
    IWbemServices *iface )
{
    struct wbem_services *ws = impl_from_IWbemServices( iface );
    LONG refs = InterlockedDecrement( &ws->refs );
    if (!refs)
    {
        TRACE("destroying %p\n", ws);
        heap_free( ws->namespace );
        heap_free( ws );
    }
    return refs;
}

static HRESULT WINAPI wbem_services_QueryInterface(
    IWbemServices *iface,
    REFIID riid,
    void **ppvObject )
{
    struct wbem_services *ws = impl_from_IWbemServices( iface );

    TRACE("%p %s %p\n", ws, debugstr_guid( riid ), ppvObject );

    if ( IsEqualGUID( riid, &IID_IWbemServices ) ||
         IsEqualGUID( riid, &IID_IUnknown ) )
    {
        *ppvObject = ws;
    }
    else if ( IsEqualGUID( riid, &IID_IClientSecurity ) )
    {
        *ppvObject = &client_security;
        return S_OK;
    }
    else
    {
        FIXME("interface %s not implemented\n", debugstr_guid(riid));
        return E_NOINTERFACE;
    }
    IWbemServices_AddRef( iface );
    return S_OK;
}

static HRESULT WINAPI wbem_services_OpenNamespace(
    IWbemServices *iface,
    const BSTR strNamespace,
    LONG lFlags,
    IWbemContext *pCtx,
    IWbemServices **ppWorkingNamespace,
    IWbemCallResult **ppResult )
{
    static const WCHAR cimv2W[] = {'c','i','m','v','2',0};
    static const WCHAR defaultW[] = {'d','e','f','a','u','l','t',0};
    struct wbem_services *ws = impl_from_IWbemServices( iface );

    TRACE("%p, %s, 0x%08x, %p, %p, %p\n", iface, debugstr_w(strNamespace), lFlags,
          pCtx, ppWorkingNamespace, ppResult);

    if ((strcmpiW( strNamespace, cimv2W ) && strcmpiW( strNamespace, defaultW )) || ws->namespace)
        return WBEM_E_INVALID_NAMESPACE;

    return WbemServices_create( NULL, cimv2W, (void **)ppWorkingNamespace );
}

static HRESULT WINAPI wbem_services_CancelAsyncCall(
    IWbemServices *iface,
    IWbemObjectSink *pSink )
{
    FIXME("%p, %p\n", iface, pSink);

    IWbemObjectSink_Release( pSink );
    return S_OK;
}

static HRESULT WINAPI wbem_services_QueryObjectSink(
    IWbemServices *iface,
    LONG lFlags,
    IWbemObjectSink **ppResponseHandler )
{
    FIXME("\n");
    return WBEM_E_FAILED;
}

struct path
{
    WCHAR *class;
    UINT   class_len;
    WCHAR *filter;
    UINT   filter_len;
};

static HRESULT parse_path( const WCHAR *str, struct path **ret )
{
    struct path *path;
    const WCHAR *p = str, *q;
    UINT len;

    if (!(path = heap_alloc_zero( sizeof(*path) ))) return E_OUTOFMEMORY;

    while (*p && *p != '.') p++;

    len = p - str;
    if (!(path->class = heap_alloc( (len + 1) * sizeof(WCHAR) )))
    {
        heap_free( path );
        return E_OUTOFMEMORY;
    }
    memcpy( path->class, str, len * sizeof(WCHAR) );
    path->class[len] = 0;
    path->class_len = len;

    if (p[0] == '.' && p[1])
    {
        q = ++p;
        while (*q) q++;

        len = q - p;
        if (!(path->filter = heap_alloc( (len + 1) * sizeof(WCHAR) )))
        {
            heap_free( path->class );
            heap_free( path );
            return E_OUTOFMEMORY;
        }
        memcpy( path->filter, p, len * sizeof(WCHAR) );
        path->filter[len] = 0;
        path->filter_len = len;
    }
    *ret = path;
    return S_OK;
}

static void free_path( struct path *path )
{
    heap_free( path->class );
    heap_free( path->filter );
    heap_free( path );
}

static HRESULT create_instance_enum( const struct path *path, IEnumWbemClassObject **iter )
{
    static const WCHAR selectW[] =
        {'S','E','L','E','C','T',' ','*',' ','F','R','O','M',' ','%','s',' ',
         'W','H','E','R','E',' ','%','s',0};
    static const WCHAR select_allW[] =
        {'S','E','L','E','C','T',' ','*',' ','F','R','O','M',' ',0};
    WCHAR *query;
    HRESULT hr;
    UINT len;

    if (path->filter)
    {
        len = path->class_len + path->filter_len + SIZEOF(selectW);
        if (!(query = heap_alloc( len * sizeof(WCHAR) ))) return E_OUTOFMEMORY;
        sprintfW( query, selectW, path->class, path->filter );
    }
    else
    {
        len = path->class_len + SIZEOF(select_allW);
        if (!(query = heap_alloc( len * sizeof(WCHAR) ))) return E_OUTOFMEMORY;
        strcpyW( query, select_allW );
        strcatW( query, path->class );
    }
    hr = exec_query( query, iter );
    heap_free( query );
    return hr;
}

HRESULT get_object( const WCHAR *object_path, IWbemClassObject **obj )
{
    IEnumWbemClassObject *iter;
    struct path *path;
    HRESULT hr;

    hr = parse_path( object_path, &path );
    if (hr != S_OK) return hr;

    hr = create_instance_enum( path, &iter );
    if (hr != S_OK)
    {
        free_path( path );
        return hr;
    }
    hr = create_class_object( path->class, iter, 0, NULL, obj );
    IEnumWbemClassObject_Release( iter );
    free_path( path );
    return hr;
}

static HRESULT WINAPI wbem_services_GetObject(
    IWbemServices *iface,
    const BSTR strObjectPath,
    LONG lFlags,
    IWbemContext *pCtx,
    IWbemClassObject **ppObject,
    IWbemCallResult **ppCallResult )
{
    TRACE("%p, %s, 0x%08x, %p, %p, %p\n", iface, debugstr_w(strObjectPath), lFlags,
          pCtx, ppObject, ppCallResult);

    if (lFlags) FIXME("unsupported flags 0x%08x\n", lFlags);

    if (!strObjectPath || !strObjectPath[0])
        return create_class_object( NULL, NULL, 0, NULL, ppObject );

    return get_object( strObjectPath, ppObject );
}

static HRESULT WINAPI wbem_services_GetObjectAsync(
    IWbemServices *iface,
    const BSTR strObjectPath,
    LONG lFlags,
    IWbemContext *pCtx,
    IWbemObjectSink *pResponseHandler )
{
    FIXME("\n");
    return WBEM_E_FAILED;
}

static HRESULT WINAPI wbem_services_PutClass(
    IWbemServices *iface,
    IWbemClassObject *pObject,
    LONG lFlags,
    IWbemContext *pCtx,
    IWbemCallResult **ppCallResult )
{
    FIXME("\n");
    return WBEM_E_FAILED;
}

static HRESULT WINAPI wbem_services_PutClassAsync(
    IWbemServices *iface,
    IWbemClassObject *pObject,
    LONG lFlags,
    IWbemContext *pCtx,
    IWbemObjectSink *pResponseHandler )
{
    FIXME("\n");
    return WBEM_E_FAILED;
}

static HRESULT WINAPI wbem_services_DeleteClass(
    IWbemServices *iface,
    const BSTR strClass,
    LONG lFlags,
    IWbemContext *pCtx,
    IWbemCallResult **ppCallResult )
{
    FIXME("\n");
    return WBEM_E_FAILED;
}

static HRESULT WINAPI wbem_services_DeleteClassAsync(
    IWbemServices *iface,
    const BSTR strClass,
    LONG lFlags,
    IWbemContext *pCtx,
    IWbemObjectSink *pResponseHandler )
{
    FIXME("\n");
    return WBEM_E_FAILED;
}

static HRESULT WINAPI wbem_services_CreateClassEnum(
    IWbemServices *iface,
    const BSTR strSuperclass,
    LONG lFlags,
    IWbemContext *pCtx,
    IEnumWbemClassObject **ppEnum )
{
    FIXME("\n");
    return WBEM_E_FAILED;
}

static HRESULT WINAPI wbem_services_CreateClassEnumAsync(
    IWbemServices *iface,
    const BSTR strSuperclass,
    LONG lFlags,
    IWbemContext *pCtx,
    IWbemObjectSink *pResponseHandler )
{
    FIXME("\n");
    return WBEM_E_FAILED;
}

static HRESULT WINAPI wbem_services_PutInstance(
    IWbemServices *iface,
    IWbemClassObject *pInst,
    LONG lFlags,
    IWbemContext *pCtx,
    IWbemCallResult **ppCallResult )
{
    FIXME("\n");
    return WBEM_E_FAILED;
}

static HRESULT WINAPI wbem_services_PutInstanceAsync(
    IWbemServices *iface,
    IWbemClassObject *pInst,
    LONG lFlags,
    IWbemContext *pCtx,
    IWbemObjectSink *pResponseHandler )
{
    FIXME("\n");
    return WBEM_E_FAILED;
}

static HRESULT WINAPI wbem_services_DeleteInstance(
    IWbemServices *iface,
    const BSTR strObjectPath,
    LONG lFlags,
    IWbemContext *pCtx,
    IWbemCallResult **ppCallResult )
{
    FIXME("\n");
    return WBEM_E_FAILED;
}

static HRESULT WINAPI wbem_services_DeleteInstanceAsync(
    IWbemServices *iface,
    const BSTR strObjectPath,
    LONG lFlags,
    IWbemContext *pCtx,
    IWbemObjectSink *pResponseHandler )
{
    FIXME("\n");
    return WBEM_E_FAILED;
}

static HRESULT WINAPI wbem_services_CreateInstanceEnum(
    IWbemServices *iface,
    const BSTR strClass,
    LONG lFlags,
    IWbemContext *pCtx,
    IEnumWbemClassObject **ppEnum )
{
    struct path *path;
    HRESULT hr;

    TRACE("%p, %s, 0%08x, %p, %p\n", iface, debugstr_w(strClass), lFlags, pCtx, ppEnum);

    if (lFlags) FIXME("unsupported flags 0x%08x\n", lFlags);

    hr = parse_path( strClass, &path );
    if (hr != S_OK) return hr;

    hr = create_instance_enum( path, ppEnum );
    free_path( path );
    return hr;
}

static HRESULT WINAPI wbem_services_CreateInstanceEnumAsync(
    IWbemServices *iface,
    const BSTR strFilter,
    LONG lFlags,
    IWbemContext *pCtx,
    IWbemObjectSink *pResponseHandler )
{
    FIXME("\n");
    return WBEM_E_FAILED;
}

static HRESULT WINAPI wbem_services_ExecQuery(
    IWbemServices *iface,
    const BSTR strQueryLanguage,
    const BSTR strQuery,
    LONG lFlags,
    IWbemContext *pCtx,
    IEnumWbemClassObject **ppEnum )
{
    static const WCHAR wqlW[] = {'W','Q','L',0};

    TRACE("%p, %s, %s, 0x%08x, %p, %p\n", iface, debugstr_w(strQueryLanguage),
          debugstr_w(strQuery), lFlags, pCtx, ppEnum);

    if (!strQueryLanguage || !strQuery || !strQuery[0]) return WBEM_E_INVALID_PARAMETER;
    if (strcmpiW( strQueryLanguage, wqlW )) return WBEM_E_INVALID_QUERY_TYPE;
    return exec_query( strQuery, ppEnum );
}

static HRESULT WINAPI wbem_services_ExecQueryAsync(
    IWbemServices *iface,
    const BSTR strQueryLanguage,
    const BSTR strQuery,
    LONG lFlags,
    IWbemContext *pCtx,
    IWbemObjectSink *pResponseHandler )
{
    FIXME("\n");
    return WBEM_E_FAILED;
}

static HRESULT WINAPI wbem_services_ExecNotificationQuery(
    IWbemServices *iface,
    const BSTR strQueryLanguage,
    const BSTR strQuery,
    LONG lFlags,
    IWbemContext *pCtx,
    IEnumWbemClassObject **ppEnum )
{
    FIXME("\n");
    return WBEM_E_FAILED;
}

static HRESULT WINAPI wbem_services_ExecNotificationQueryAsync(
    IWbemServices *iface,
    const BSTR strQueryLanguage,
    const BSTR strQuery,
    LONG lFlags,
    IWbemContext *pCtx,
    IWbemObjectSink *pResponseHandler )
{
    FIXME("%p, %s, %s, 0x%08x, %p, %p\n", iface, debugstr_w(strQueryLanguage), debugstr_w(strQuery),
          lFlags, pCtx, pResponseHandler);

    IWbemObjectSink_AddRef( pResponseHandler );
    return S_OK;
}

static HRESULT WINAPI wbem_services_ExecMethod(
    IWbemServices *iface,
    const BSTR strObjectPath,
    const BSTR strMethodName,
    LONG lFlags,
    IWbemContext *pCtx,
    IWbemClassObject *pInParams,
    IWbemClassObject **ppOutParams,
    IWbemCallResult **ppCallResult )
{
    IWbemClassObject *obj;
    struct table *table;
    class_method *func;
    struct path *path;
    HRESULT hr;

    TRACE("%p, %s, %s, %08x, %p, %p, %p, %p\n", iface, debugstr_w(strObjectPath),
          debugstr_w(strMethodName), lFlags, pCtx, pInParams, ppOutParams, ppCallResult);

    if (lFlags) FIXME("flags %08x not supported\n", lFlags);

    if ((hr = get_object( strObjectPath, &obj ))) return hr;
    if ((hr = parse_path( strObjectPath, &path )) != S_OK)
    {
        IWbemClassObject_Release( obj );
        return hr;
    }
    table = grab_table( path->class );
    free_path( path );
    if (!table)
    {
        IWbemClassObject_Release( obj );
        return WBEM_E_NOT_FOUND;
    }
    hr = get_method( table, strMethodName, &func );
    release_table( table );
    if (hr != S_OK)
    {
        IWbemClassObject_Release( obj );
        return hr;
    }
    hr = func( obj, pInParams, ppOutParams );
    IWbemClassObject_Release( obj );
    return hr;
}

static HRESULT WINAPI wbem_services_ExecMethodAsync(
    IWbemServices *iface,
    const BSTR strObjectPath,
    const BSTR strMethodName,
    LONG lFlags,
    IWbemContext *pCtx,
    IWbemClassObject *pInParams,
    IWbemObjectSink *pResponseHandler )
{
    FIXME("\n");
    return WBEM_E_FAILED;
}

static const IWbemServicesVtbl wbem_services_vtbl =
{
    wbem_services_QueryInterface,
    wbem_services_AddRef,
    wbem_services_Release,
    wbem_services_OpenNamespace,
    wbem_services_CancelAsyncCall,
    wbem_services_QueryObjectSink,
    wbem_services_GetObject,
    wbem_services_GetObjectAsync,
    wbem_services_PutClass,
    wbem_services_PutClassAsync,
    wbem_services_DeleteClass,
    wbem_services_DeleteClassAsync,
    wbem_services_CreateClassEnum,
    wbem_services_CreateClassEnumAsync,
    wbem_services_PutInstance,
    wbem_services_PutInstanceAsync,
    wbem_services_DeleteInstance,
    wbem_services_DeleteInstanceAsync,
    wbem_services_CreateInstanceEnum,
    wbem_services_CreateInstanceEnumAsync,
    wbem_services_ExecQuery,
    wbem_services_ExecQueryAsync,
    wbem_services_ExecNotificationQuery,
    wbem_services_ExecNotificationQueryAsync,
    wbem_services_ExecMethod,
    wbem_services_ExecMethodAsync
};

HRESULT WbemServices_create( IUnknown *pUnkOuter, const WCHAR *namespace, LPVOID *ppObj )
{
    struct wbem_services *ws;

    TRACE("(%p,%p)\n", pUnkOuter, ppObj);

    ws = heap_alloc( sizeof(*ws) );
    if (!ws) return E_OUTOFMEMORY;

    ws->IWbemServices_iface.lpVtbl = &wbem_services_vtbl;
    ws->refs = 1;
    ws->namespace = heap_strdupW( namespace );

    *ppObj = &ws->IWbemServices_iface;

    TRACE("returning iface %p\n", *ppObj);
    return S_OK;
}
