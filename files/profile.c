/*
 * Profile functions
 *
 * Copyright 1993 Miguel de Icaza
 * Copyright 1996 Alexandre Julliard
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef HAVE_PWD_H
# include <pwd.h>
#endif
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winerror.h"
#include "ntddk.h"
#include "wine/winbase16.h"
#include "winreg.h"
#include "drive.h"
#include "file.h"
#include "heap.h"
#include "wine/unicode.h"
#include "wine/server.h"
#include "wine/library.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(profile);

typedef struct tagPROFILEKEY
{
    WCHAR                 *value;
    struct tagPROFILEKEY  *next;
    WCHAR                  name[1];
} PROFILEKEY;

typedef struct tagPROFILESECTION
{
    struct tagPROFILEKEY       *key;
    struct tagPROFILESECTION   *next;
    WCHAR                       name[1];
} PROFILESECTION;


typedef struct
{
    BOOL             changed;
    PROFILESECTION  *section;
    WCHAR           *dos_name;
    char            *unix_name;
    WCHAR           *filename;
    time_t           mtime;
} PROFILE;


#define N_CACHED_PROFILES 10

/* Cached profile files */
static PROFILE *MRUProfile[N_CACHED_PROFILES]={NULL};

#define CurProfile (MRUProfile[0])

/* wine.ini config file registry root */
static HKEY wine_profile_key;

#define PROFILE_MAX_LINE_LEN   1024

/* Wine profile name in $HOME directory; must begin with slash */
static const char PROFILE_WineIniName[] = "/.winerc";

/* Wine profile: the profile file being used */
static char PROFILE_WineIniUsed[MAX_PATHNAME_LEN] = "";

/* Check for comments in profile */
#define IS_ENTRY_COMMENT(str)  ((str)[0] == ';')

static const WCHAR wininiW[] = { 'w','i','n','.','i','n','i',0 };

static CRITICAL_SECTION PROFILE_CritSect = CRITICAL_SECTION_INIT("PROFILE_CritSect");

static const char hex[16] = "0123456789ABCDEF";

/***********************************************************************
 *           PROFILE_CopyEntry
 *
 * Copy the content of an entry into a buffer, removing quotes, and possibly
 * translating environment variables.
 */
static void PROFILE_CopyEntry( LPWSTR buffer, LPCWSTR value, int len,
                               int handle_env, BOOL strip_quote )
{
    WCHAR quote = '\0';
    LPCWSTR p;

    if(!buffer) return;

    if (strip_quote && ((*value == '\'') || (*value == '\"')))
    {
        if (value[1] && (value[strlenW(value)-1] == *value)) quote = *value++;
    }

    if (!handle_env)
    {
        lstrcpynW( buffer, value, len );
        if (quote && (len >= strlenW(value))) buffer[strlenW(buffer)-1] = '\0';
        return;
    }

    for (p = value; (*p && (len > 1)); *buffer++ = *p++, len-- )
    {
        if ((*p == '$') && (p[1] == '{'))
        {
            WCHAR env_val[1024];
            LPCWSTR p2 = strchrW( p, '}' );
            int copy_len;
            if (!p2) continue;  /* ignore it */
            copy_len = min( 1024, (int)(p2-p)-1 );
            strncpyW(env_val, p + 2, copy_len );
            env_val[copy_len - 1] = 0; /* ensure 0 termination */
            *buffer = 0;
            if (GetEnvironmentVariableW( env_val, buffer, len))
            {
                copy_len = strlenW( buffer );
                buffer += copy_len;
                len -= copy_len;
            }
            p = p2 + 1;
        }
    }
    if (quote && (len > 1)) buffer--;
    *buffer = '\0';
}


/***********************************************************************
 *           PROFILE_Save
 *
 * Save a profile tree to a file.
 */
static void PROFILE_Save( FILE *file, PROFILESECTION *section )
{
    PROFILEKEY *key;
    char buffer[PROFILE_MAX_LINE_LEN];

    for ( ; section; section = section->next)
    {
        if (section->name[0])
        {
            WideCharToMultiByte(CP_ACP, 0, section->name, -1, buffer, sizeof(buffer), NULL, NULL);
            fprintf( file, "\r\n[%s]\r\n", buffer );
        }
        for (key = section->key; key; key = key->next)
        {
            WideCharToMultiByte(CP_ACP, 0, key->name, -1, buffer, sizeof(buffer), NULL, NULL);
            fprintf( file, "%s", buffer );
            if (key->value)
            {
                 WideCharToMultiByte(CP_ACP, 0, key->value, -1, buffer, sizeof(buffer), NULL, NULL);
                 fprintf( file, "=%s", buffer );
            }
            fprintf( file, "\r\n" );
        }
    }
}


/***********************************************************************
 *           PROFILE_Free
 *
 * Free a profile tree.
 */
static void PROFILE_Free( PROFILESECTION *section )
{
    PROFILESECTION *next_section;
    PROFILEKEY *key, *next_key;

    for ( ; section; section = next_section)
    {
        for (key = section->key; key; key = next_key)
        {
            next_key = key->next;
            if (key->value) HeapFree( GetProcessHeap(), 0, key->value );
            HeapFree( GetProcessHeap(), 0, key );
        }
        next_section = section->next;
        HeapFree( GetProcessHeap(), 0, section );
    }
}

static inline int PROFILE_isspace(char c)
{
	if (isspace(c)) return 1;
	if (c=='\r' || c==0x1a) return 1;
	/* CR and ^Z (DOS EOF) are spaces too  (found on CD-ROMs) */
	return 0;
}


/***********************************************************************
 *           PROFILE_Load
 *
 * Load a profile tree from a file.
 */
static PROFILESECTION *PROFILE_Load( FILE *file )
{
    char buffer[PROFILE_MAX_LINE_LEN];
    char *p, *p2;
    int line = 0, len;
    PROFILESECTION *section, *first_section;
    PROFILESECTION **next_section;
    PROFILEKEY *key, *prev_key, **next_key;

    first_section = HeapAlloc( GetProcessHeap(), 0, sizeof(*section) );
    if(first_section == NULL) return NULL;
    first_section->name[0] = 0;
    first_section->key  = NULL;
    first_section->next = NULL;
    next_section = &first_section->next;
    next_key     = &first_section->key;
    prev_key     = NULL;

    while (fgets( buffer, PROFILE_MAX_LINE_LEN, file ))
    {
        line++;
        p = buffer;
        while (*p && PROFILE_isspace(*p)) p++;
        if (*p == '[')  /* section start */
        {
            if (!(p2 = strrchr( p, ']' )))
            {
                WARN("Invalid section header at line %d: '%s'\n",
		     line, p );
            }
            else
            {
                *p2 = '\0';
                p++;
                len = strlen(p);
                if (!(section = HeapAlloc( GetProcessHeap(), 0, sizeof(*section) + len * sizeof(WCHAR) )))
                    break;
                MultiByteToWideChar(CP_ACP, 0, p, -1, section->name, len + 1);
                section->key  = NULL;
                section->next = NULL;
                *next_section = section;
                next_section  = &section->next;
                next_key      = &section->key;
                prev_key      = NULL;

                TRACE("New section: %s\n", debugstr_w(section->name));

                continue;
            }
        }

        p2=p+strlen(p) - 1;
        while ((p2 > p) && ((*p2 == '\n') || PROFILE_isspace(*p2))) *p2--='\0';

        if ((p2 = strchr( p, '=' )) != NULL)
        {
            char *p3 = p2 - 1;
            while ((p3 > p) && PROFILE_isspace(*p3)) *p3-- = '\0';
            *p2++ = '\0';
            while (*p2 && PROFILE_isspace(*p2)) p2++;
        }

        if(*p || !prev_key || *prev_key->name)
        {
            len = strlen(p);
            if (!(key = HeapAlloc( GetProcessHeap(), 0, sizeof(*key) + len * sizeof(WCHAR) ))) break;
            MultiByteToWideChar(CP_ACP, 0, p, -1, key->name, len + 1);
            if (p2)
            {
                len = strlen(p2) + 1;
                key->value = HeapAlloc( GetProcessHeap(), 0, len * sizeof(WCHAR) );
                MultiByteToWideChar(CP_ACP, 0, p2, -1, key->value, len);
            }
            else key->value = NULL;

           key->next  = NULL;
           *next_key  = key;
           next_key   = &key->next;
           prev_key   = key;

           TRACE("New key: name=%s, value=%s\n",
               debugstr_w(key->name), key->value ? debugstr_w(key->value) : "(none)");
        }
    }
    return first_section;
}

/* convert the .winerc file to the new format */
static void convert_config( FILE *in, const char *output_name )
{
    char buffer[PROFILE_MAX_LINE_LEN];
    char *p, *p2;
    FILE *out;

    /* create the output file, only if it doesn't exist already */
    int fd = open( output_name, O_WRONLY|O_CREAT|O_EXCL, 0666 );
    if (fd == -1)
    {
        MESSAGE( "Could not create new config file '%s': %s\n", output_name, strerror(errno) );
        ExitProcess(1);
    }

    out = fdopen( fd, "w" );
    fprintf( out, "WINE REGISTRY Version 2\n" );
    fprintf( out, ";; All keys relative to \\\\Machine\\\\Software\\\\Wine\\\\Wine\\\\Config\n\n" );
    while (fgets( buffer, PROFILE_MAX_LINE_LEN, in ))
    {
        if (buffer[strlen(buffer)-1] == '\n') buffer[strlen(buffer)-1] = 0;
        p = buffer;
        while (*p && PROFILE_isspace(*p)) p++;
        if (*p == '[')  /* section start */
        {
            if ((p2 = strrchr( p, ']' )))
            {
                *p2 = '\0';
                p++;
                fprintf( out, "[%s]\n", p );
            }
            continue;
        }

        if (*p == ';' || *p == '#')
        {
            fprintf( out, "%s\n", p );
            continue;
        }

        p2=p+strlen(p) - 1;
        while ((p2 > p) && ((*p2 == '\n') || PROFILE_isspace(*p2))) *p2--='\0';

        if ((p2 = strchr( p, '=' )) != NULL)
        {
            char *p3 = p2 - 1;
            while ((p3 > p) && PROFILE_isspace(*p3)) *p3-- = '\0';
            *p2++ = '\0';
            while (*p2 && PROFILE_isspace(*p2)) p2++;
        }

        if (!*p)
        {
            fprintf( out, "\n" );
            continue;
        }
        fputc( '"', out );
        while (*p)
        {
            if (*p == '\\') fputc( '\\', out );
            fputc( *p, out );
            p++;
        }
        fprintf( out, "\" = \"" );
        if (p2)
        {
            while (*p2)
            {
                if (*p2 == '\\') fputc( '\\', out );
                fputc( *p2, out );
                p2++;
            }
        }
        fprintf( out, "\"\n" );
    }
    fclose( out );
}


/***********************************************************************
 *           PROFILE_DeleteSection
 *
 * Delete a section from a profile tree.
 */
static BOOL PROFILE_DeleteSection( PROFILESECTION **section, LPCWSTR name )
{
    while (*section)
    {
        if ((*section)->name[0] && !strcmpiW( (*section)->name, name ))
        {
            PROFILESECTION *to_del = *section;
            *section = to_del->next;
            to_del->next = NULL;
            PROFILE_Free( to_del );
            return TRUE;
        }
        section = &(*section)->next;
    }
    return FALSE;
}


/***********************************************************************
 *           PROFILE_DeleteKey
 *
 * Delete a key from a profile tree.
 */
static BOOL PROFILE_DeleteKey( PROFILESECTION **section,
			       LPCWSTR section_name, LPCWSTR key_name )
{
    while (*section)
    {
        if ((*section)->name[0] && !strcmpiW( (*section)->name, section_name ))
        {
            PROFILEKEY **key = &(*section)->key;
            while (*key)
            {
                if (!strcmpiW( (*key)->name, key_name ))
                {
                    PROFILEKEY *to_del = *key;
                    *key = to_del->next;
                    if (to_del->value) HeapFree( GetProcessHeap(), 0, to_del->value);
                    HeapFree( GetProcessHeap(), 0, to_del );
                    return TRUE;
                }
                key = &(*key)->next;
            }
        }
        section = &(*section)->next;
    }
    return FALSE;
}


/***********************************************************************
 *           PROFILE_DeleteAllKeys
 *
 * Delete all keys from a profile tree.
 */
void PROFILE_DeleteAllKeys( LPCWSTR section_name)
{
    PROFILESECTION **section= &CurProfile->section;
    while (*section)
    {
        if ((*section)->name[0] && !strcmpiW( (*section)->name, section_name ))
        {
            PROFILEKEY **key = &(*section)->key;
            while (*key)
            {
                PROFILEKEY *to_del = *key;
		*key = to_del->next;
		if (to_del->value) HeapFree( GetProcessHeap(), 0, to_del->value);
		HeapFree( GetProcessHeap(), 0, to_del );
		CurProfile->changed =TRUE;
            }
        }
        section = &(*section)->next;
    }
}


/***********************************************************************
 *           PROFILE_Find
 *
 * Find a key in a profile tree, optionally creating it.
 */
static PROFILEKEY *PROFILE_Find( PROFILESECTION **section, LPCWSTR section_name,
                                 LPCWSTR key_name, BOOL create, BOOL create_always )
{
    LPCWSTR p;
    int seclen, keylen;

    while (PROFILE_isspace(*section_name)) section_name++;
    p = section_name + strlenW(section_name) - 1;
    while ((p > section_name) && PROFILE_isspace(*p)) p--;
    seclen = p - section_name + 1;

    while (PROFILE_isspace(*key_name)) key_name++;
    p = key_name + strlenW(key_name) - 1;
    while ((p > key_name) && PROFILE_isspace(*p)) p--;
    keylen = p - key_name + 1;

    while (*section)
    {
        if ( ((*section)->name[0])
             && (!(strncmpiW( (*section)->name, section_name, seclen )))
             && (((*section)->name)[seclen] == '\0') )
        {
            PROFILEKEY **key = &(*section)->key;

            while (*key)
            {
                /* If create_always is FALSE then we check if the keyname already exists.
                 * Otherwise we add it regardless of its existence, to allow
                 * keys to be added more then once in some cases.
                 */
                if(!create_always)
                {
                    if ( (!(strncmpiW( (*key)->name, key_name, keylen )))
                         && (((*key)->name)[keylen] == '\0') )
                        return *key;
                }
                key = &(*key)->next;
            }
            if (!create) return NULL;
            if (!(*key = HeapAlloc( GetProcessHeap(), 0, sizeof(PROFILEKEY) + strlenW(key_name) * sizeof(WCHAR) )))
                return NULL;
            strcpyW( (*key)->name, key_name );
            (*key)->value = NULL;
            (*key)->next  = NULL;
            return *key;
        }
        section = &(*section)->next;
    }
    if (!create) return NULL;
    *section = HeapAlloc( GetProcessHeap(), 0, sizeof(PROFILESECTION) + strlenW(section_name) * sizeof(WCHAR) );
    if(*section == NULL) return NULL;
    strcpyW( (*section)->name, section_name );
    (*section)->next = NULL;
    if (!((*section)->key  = HeapAlloc( GetProcessHeap(), 0,
                                        sizeof(PROFILEKEY) + strlenW(key_name) * sizeof(WCHAR) )))
    {
        HeapFree(GetProcessHeap(), 0, *section);
        return NULL;
    }
    strcpyW( (*section)->key->name, key_name );
    (*section)->key->value = NULL;
    (*section)->key->next  = NULL;
    return (*section)->key;
}


/***********************************************************************
 *           PROFILE_FlushFile
 *
 * Flush the current profile to disk if changed.
 */
static BOOL PROFILE_FlushFile(void)
{
    char *p, buffer[MAX_PATHNAME_LEN];
    const char *unix_name;
    FILE *file = NULL;
    struct stat buf;

    if(!CurProfile)
    {
        WARN("No current profile!\n");
        return FALSE;
    }

    if (!CurProfile->changed || !CurProfile->dos_name) return TRUE;
    if (!(unix_name = CurProfile->unix_name) || !(file = fopen(unix_name, "w")))
    {
        int drive = toupperW(CurProfile->dos_name[0]) - 'A';
        WCHAR *name;
        /* Try to create it in $HOME/.wine */
        /* FIXME: this will need a more general solution */
        strcpy( buffer, wine_get_config_dir() );
        p = buffer + strlen(buffer);
        *p++ = '/';
        *p = 0; /* make strlen() below happy */
        name = strrchrW( CurProfile->dos_name, '\\' ) + 1;
        WideCharToMultiByte(DRIVE_GetCodepage(drive), 0, name, -1,
                            p, sizeof(buffer) - strlen(buffer), NULL, NULL);
        file = fopen( buffer, "w" );
        unix_name = buffer;
    }

    if (!file)
    {
        WARN("could not save profile file %s\n", debugstr_w(CurProfile->dos_name));
        return FALSE;
    }

    TRACE("Saving %s into '%s'\n", debugstr_w(CurProfile->dos_name), unix_name );
    PROFILE_Save( file, CurProfile->section );
    fclose( file );
    CurProfile->changed = FALSE;
    if(!stat(unix_name,&buf))
       CurProfile->mtime=buf.st_mtime;
    return TRUE;
}


/***********************************************************************
 *           PROFILE_ReleaseFile
 *
 * Flush the current profile to disk and remove it from the cache.
 */
static void PROFILE_ReleaseFile(void)
{
    PROFILE_FlushFile();
    PROFILE_Free( CurProfile->section );
    if (CurProfile->dos_name) HeapFree( GetProcessHeap(), 0, CurProfile->dos_name );
    if (CurProfile->unix_name) HeapFree( GetProcessHeap(), 0, CurProfile->unix_name );
    if (CurProfile->filename) HeapFree( GetProcessHeap(), 0, CurProfile->filename );
    CurProfile->changed   = FALSE;
    CurProfile->section   = NULL;
    CurProfile->dos_name  = NULL;
    CurProfile->unix_name = NULL;
    CurProfile->filename  = NULL;
    CurProfile->mtime     = 0;
}


/***********************************************************************
 *           PROFILE_Open
 *
 * Open a profile file, checking the cached file first.
 */
static BOOL PROFILE_Open( LPCWSTR filename )
{
    DOS_FULL_NAME full_name;
    char buffer[MAX_PATHNAME_LEN];
    WCHAR *newdos_name;
    WCHAR *name;
    char *p;
    FILE *file = NULL;
    int i,j;
    struct stat buf;
    PROFILE *tempProfile;

    /* First time around */

    if(!CurProfile)
       for(i=0;i<N_CACHED_PROFILES;i++)
         {
          MRUProfile[i]=HeapAlloc( GetProcessHeap(), 0, sizeof(PROFILE) );
	  if(MRUProfile[i] == NULL) break;
          MRUProfile[i]->changed=FALSE;
          MRUProfile[i]->section=NULL;
          MRUProfile[i]->dos_name=NULL;
          MRUProfile[i]->unix_name=NULL;
          MRUProfile[i]->filename=NULL;
          MRUProfile[i]->mtime=0;
         }

    /* Check for a match */

    if (strchrW( filename, '/' ) || strchrW( filename, '\\' ) ||
        strchrW( filename, ':' ))
    {
        if (!DOSFS_GetFullName( filename, FALSE, &full_name )) return FALSE;
    }
    else
    {
        static const WCHAR bkslashW[] = {'\\',0};
        WCHAR windirW[MAX_PATH];

        GetWindowsDirectoryW( windirW, MAX_PATH );
        strcatW( windirW, bkslashW );
        strcatW( windirW, filename );
        if (!DOSFS_GetFullName( windirW, FALSE, &full_name )) return FALSE;
    }

    for(i=0;i<N_CACHED_PROFILES;i++)
      {
       if ((MRUProfile[i]->filename && !strcmpW( filename, MRUProfile[i]->filename )) ||
           (MRUProfile[i]->dos_name && !strcmpW( full_name.short_name, MRUProfile[i]->dos_name )))
         {
          if(i)
            {
             PROFILE_FlushFile();
             tempProfile=MRUProfile[i];
             for(j=i;j>0;j--)
                MRUProfile[j]=MRUProfile[j-1];
             CurProfile=tempProfile;
            }
          if(!stat(CurProfile->unix_name,&buf) && CurProfile->mtime==buf.st_mtime)
             TRACE("(%s): already opened (mru=%d)\n",
                             debugstr_w(filename), i );
          else
              TRACE("(%s): already opened, needs refreshing (mru=%d)\n",
                             debugstr_w(filename), i );
	  return TRUE;
         }
      }

    /* Flush the old current profile */
    PROFILE_FlushFile();

    /* Make the oldest profile the current one only in order to get rid of it */
    if(i==N_CACHED_PROFILES)
      {
       tempProfile=MRUProfile[N_CACHED_PROFILES-1];
       for(i=N_CACHED_PROFILES-1;i>0;i--)
          MRUProfile[i]=MRUProfile[i-1];
       CurProfile=tempProfile;
      }
    if(CurProfile->filename) PROFILE_ReleaseFile();

    /* OK, now that CurProfile is definitely free we assign it our new file */
    newdos_name = HeapAlloc( GetProcessHeap(), 0, (strlenW(full_name.short_name)+1) * sizeof(WCHAR) );
    strcpyW( newdos_name, full_name.short_name );
    CurProfile->dos_name  = newdos_name;
    CurProfile->filename  = HeapAlloc( GetProcessHeap(), 0, (strlenW(filename)+1) * sizeof(WCHAR) );
    strcpyW( CurProfile->filename, filename );

    /* Try to open the profile file, first in $HOME/.wine */

    /* FIXME: this will need a more general solution */
    strcpy( buffer, wine_get_config_dir() );
    p = buffer + strlen(buffer);
    *p++ = '/';
    *p = 0; /* make strlen() below happy */
    name = strrchrW( newdos_name, '\\' ) + 1;
    WideCharToMultiByte(DRIVE_GetCodepage(full_name.drive), 0, name, -1,
                        p, sizeof(buffer) - strlen(buffer), NULL, NULL);
    if ((file = fopen( buffer, "r" )))
    {
        TRACE("(%s): found it in %s\n", debugstr_w(filename), buffer );
        CurProfile->unix_name = HeapAlloc( GetProcessHeap(), 0, strlen(buffer)+1 );
        strcpy( CurProfile->unix_name, buffer );
    }
    else
    {
        CurProfile->unix_name = HeapAlloc( GetProcessHeap(), 0, strlen(full_name.long_name)+1 );
        strcpy( CurProfile->unix_name, full_name.long_name );
        if ((file = fopen( full_name.long_name, "r" )))
            TRACE("(%s): found it in %s\n",
                             debugstr_w(filename), full_name.long_name );
    }

    if (file)
    {
        CurProfile->section = PROFILE_Load( file );
        fclose( file );
        if(!stat(CurProfile->unix_name,&buf))
           CurProfile->mtime=buf.st_mtime;
    }
    else
    {
        /* Does not exist yet, we will create it in PROFILE_FlushFile */
        WARN("profile file %s not found\n", debugstr_w(newdos_name) );
    }
    return TRUE;
}


/***********************************************************************
 *           PROFILE_GetSection
 *
 * Returns all keys of a section.
 * If return_values is TRUE, also include the corresponding values.
 */
static INT PROFILE_GetSection( PROFILESECTION *section, LPCWSTR section_name,
			       LPWSTR buffer, UINT len, BOOL handle_env,
			       BOOL return_values )
{
    PROFILEKEY *key;

    if(!buffer) return 0;

    TRACE("%s,%p,%u\n", debugstr_w(section_name), buffer, len);

    while (section)
    {
        if (section->name[0] && !strcmpiW( section->name, section_name ))
        {
            UINT oldlen = len;
            for (key = section->key; key; key = key->next)
            {
                if (len <= 2) break;
                if (!*key->name) continue;  /* Skip empty lines */
                if (IS_ENTRY_COMMENT(key->name)) continue;  /* Skip comments */
                PROFILE_CopyEntry( buffer, key->name, len - 1, handle_env, 0 );
                len -= strlenW(buffer) + 1;
                buffer += strlenW(buffer) + 1;
		if (len < 2)
		    break;
		if (return_values && key->value) {
			buffer[-1] = '=';
			PROFILE_CopyEntry ( buffer,
				key->value, len - 1, handle_env, 0 );
			len -= strlenW(buffer) + 1;
			buffer += strlenW(buffer) + 1;
                }
            }
            *buffer = '\0';
            if (len <= 1)
                /*If either lpszSection or lpszKey is NULL and the supplied
                  destination buffer is too small to hold all the strings,
                  the last string is truncated and followed by two null characters.
                  In this case, the return value is equal to cchReturnBuffer
                  minus two. */
            {
		buffer[-1] = '\0';
                return oldlen - 2;
            }
            return oldlen - len;
        }
        section = section->next;
    }
    buffer[0] = buffer[1] = '\0';
    return 0;
}

/* See GetPrivateProfileSectionNamesA for documentation */
static INT PROFILE_GetSectionNames( LPWSTR buffer, UINT len )
{
    LPWSTR buf;
    UINT f,l;
    PROFILESECTION *section;

    if (!buffer || !len)
        return 0;
    if (len==1) {
        *buffer='\0';
        return 0;
    }

    f=len-1;
    buf=buffer;
    section = CurProfile->section;
    while ((section!=NULL)) {
        if (section->name[0]) {
            l = strlenW(section->name)+1;
            if (l > f) {
                if (f>0) {
                    strncpyW(buf, section->name, f-1);
                    buf += f-1;
                    *buf++='\0';
                }
                *buf='\0';
                return len-2;
            }
            strcpyW(buf, section->name);
            buf += l;
            f -= l;
        }
        section = section->next;
    }
    *buf='\0';
    return buf-buffer;
}


/***********************************************************************
 *           PROFILE_GetString
 *
 * Get a profile string.
 *
 * Tests with GetPrivateProfileString16, W95a,
 * with filled buffer ("****...") and section "set1" and key_name "1" valid:
 * section	key_name	def_val		res	buffer
 * "set1"	"1"		"x"		43	[data]
 * "set1"	"1   "		"x"		43	[data]		(!)
 * "set1"	"  1  "'	"x"		43	[data]		(!)
 * "set1"	""		"x"		1	"x"
 * "set1"	""		"x   "		1	"x"		(!)
 * "set1"	""		"  x   "	3	"  x"		(!)
 * "set1"	NULL		"x"		6	"1\02\03\0\0"
 * "set1"	""		"x"		1	"x"
 * NULL		"1"		"x"		0	""		(!)
 * ""		"1"		"x"		1	"x"
 * NULL		NULL		""		0	""
 *
 *
 */
static INT PROFILE_GetString( LPCWSTR section, LPCWSTR key_name,
                              LPCWSTR def_val, LPWSTR buffer, UINT len )
{
    PROFILEKEY *key = NULL;
    static const WCHAR empty_strW[] = { 0 };

    if(!buffer) return 0;

    if (!def_val) def_val = empty_strW;
    if (key_name)
    {
	if (!key_name[0])
        {
            /* Win95 returns 0 on keyname "". Tested with Likse32 bon 000227 */
            return 0;
        }
        key = PROFILE_Find( &CurProfile->section, section, key_name, FALSE, FALSE);
        PROFILE_CopyEntry( buffer, (key && key->value) ? key->value : def_val,
                           len, FALSE, TRUE );
        TRACE("(%s,%s,%s): returning %s\n",
              debugstr_w(section), debugstr_w(key_name),
              debugstr_w(def_val), debugstr_w(buffer) );
        return strlenW( buffer );
    }
    /* no "else" here ! */
    if (section && section[0])
    {
        INT ret = PROFILE_GetSection(CurProfile->section, section, buffer, len, FALSE, FALSE);
        if (!buffer[0]) /* no luck -> def_val */
        {
            PROFILE_CopyEntry(buffer, def_val, len, FALSE, TRUE);
            ret = strlenW(buffer);
        }
        return ret;
    }
    buffer[0] = '\0';
    return 0;
}


/***********************************************************************
 *           PROFILE_SetString
 *
 * Set a profile string.
 */
static BOOL PROFILE_SetString( LPCWSTR section_name, LPCWSTR key_name,
                               LPCWSTR value, BOOL create_always )
{
    if (!key_name)  /* Delete a whole section */
    {
        TRACE("(%s)\n", debugstr_w(section_name));
        CurProfile->changed |= PROFILE_DeleteSection( &CurProfile->section,
                                                      section_name );
        return TRUE;         /* Even if PROFILE_DeleteSection() has failed,
                                this is not an error on application's level.*/
    }
    else if (!value)  /* Delete a key */
    {
        TRACE("(%s,%s)\n", debugstr_w(section_name), debugstr_w(key_name) );
        CurProfile->changed |= PROFILE_DeleteKey( &CurProfile->section,
                                                  section_name, key_name );
        return TRUE;          /* same error handling as above */
    }
    else  /* Set the key value */
    {
        PROFILEKEY *key = PROFILE_Find(&CurProfile->section, section_name,
                                        key_name, TRUE, create_always );
        TRACE("(%s,%s,%s):\n",
              debugstr_w(section_name), debugstr_w(key_name), debugstr_w(value) );
        if (!key) return FALSE;
        if (key->value)
        {
	    /* strip the leading spaces. We can safely strip \n\r and
	     * friends too, they should not happen here anyway. */
	    while (PROFILE_isspace(*value)) value++;

            if (!strcmpW( key->value, value ))
            {
                TRACE("  no change needed\n" );
                return TRUE;  /* No change needed */
            }
            TRACE("  replacing %s\n", debugstr_w(key->value) );
            HeapFree( GetProcessHeap(), 0, key->value );
        }
        else TRACE("  creating key\n" );
        key->value = HeapAlloc( GetProcessHeap(), 0, (strlenW(value)+1) * sizeof(WCHAR) );
        strcpyW( key->value, value );
        CurProfile->changed = TRUE;
    }
    return TRUE;
}


/***********************************************************************
 *           PROFILE_GetWineIniString
 *
 * Get a config string from the wine.ini file.
 */
int PROFILE_GetWineIniString( LPCWSTR section, LPCWSTR key_name,
                              LPCWSTR def, LPWSTR buffer, int len )
{
    WCHAR tmp[PROFILE_MAX_LINE_LEN];
    HKEY hkey;
    DWORD err;

    if (!(err = RegOpenKeyW( wine_profile_key, section, &hkey )))
    {
        DWORD type;
        DWORD count = sizeof(tmp);
        err = RegQueryValueExW( hkey, key_name, 0, &type, (LPBYTE)tmp, &count );
        RegCloseKey( hkey );
    }

    PROFILE_CopyEntry( buffer, err ? def : tmp, len, TRUE, TRUE );
    TRACE( "(%s,%s,%s): returning %s\n", debugstr_w(section),
           debugstr_w(key_name), debugstr_w(def), debugstr_w(buffer) );
    return strlenW(buffer);
}


/******************************************************************************
 *
 *           PROFILE_GetWineIniBool
 *
 *   Reads a boolean value from the wine.ini file.  This function attempts to
 *   be user-friendly by accepting 'n', 'N' (no), 'f', 'F' (false), or '0'
 *   (zero) for false, 'y', 'Y' (yes), 't', 'T' (true), or '1' (one) for
 *   true.  Anything else results in the return of the default value.
 *
 *   This function uses 1 to indicate true, and 0 for false.  You can check
 *   for existence by setting def to something other than 0 or 1 and
 *   examining the return value.
 */
int  PROFILE_GetWineIniBool( LPCWSTR section, LPCWSTR key_name, int def )
{
    static const WCHAR def_valueW[] = {'~',0};
    WCHAR key_value[2];
    int  retval;

    PROFILE_GetWineIniString(section, key_name, def_valueW, key_value, 2);

    switch(key_value[0]) {
    case 'n':
    case 'N':
    case 'f':
    case 'F':
    case '0':
	retval = 0;
	break;

    case 'y':
    case 'Y':
    case 't':
    case 'T':
    case '1':
	retval = 1;
	break;

    default:
	retval = def;
    }

    TRACE("(%s, %s, %s), [%c], ret %s\n", debugstr_w(section), debugstr_w(key_name),
		    def ? "TRUE" : "FALSE", key_value[0],
		    retval ? "TRUE" : "FALSE");

    return retval;
}


/***********************************************************************
 *           PROFILE_LoadWineIni
 *
 * Load the old .winerc file.
 */
int PROFILE_LoadWineIni(void)
{
    OBJECT_ATTRIBUTES attr;
    UNICODE_STRING nameW;
    char buffer[MAX_PATHNAME_LEN];
    const char *p;
    FILE *f;
    HKEY hKeySW;
    DWORD disp;

    attr.Length = sizeof(attr);
    attr.RootDirectory = 0;
    attr.ObjectName = &nameW;
    attr.Attributes = 0;
    attr.SecurityDescriptor = NULL;
    attr.SecurityQualityOfService = NULL;

    /* make sure HKLM\\Software\\Wine\\Wine exists as non-volatile key */
    if (!RtlCreateUnicodeStringFromAsciiz( &nameW, "Machine\\Software\\Wine\\Wine" ) ||
        NtCreateKey( &hKeySW, KEY_ALL_ACCESS, &attr, 0, NULL, 0, &disp ))
    {
        ERR("Cannot create config registry key\n" );
        ExitProcess( 1 );
    }
    RtlFreeUnicodeString( &nameW );
    NtClose( hKeySW );

    if (!RtlCreateUnicodeStringFromAsciiz( &nameW, "Machine\\Software\\Wine\\Wine\\Config" ) ||
        NtCreateKey( &wine_profile_key, KEY_ALL_ACCESS, &attr, 0,
                     NULL, REG_OPTION_VOLATILE, &disp ))
    {
        ERR("Cannot create config registry key\n" );
        ExitProcess( 1 );
    }
    RtlFreeUnicodeString( &nameW );

    if (disp == REG_OPENED_EXISTING_KEY) return 1;  /* loaded by the server */

    if ((p = getenv( "HOME" )) != NULL)
    {
        lstrcpynA(buffer, p, MAX_PATHNAME_LEN - sizeof(PROFILE_WineIniName));
        strcat( buffer, PROFILE_WineIniName );
        if ((f = fopen( buffer, "r" )) != NULL)
        {
	    lstrcpynA(PROFILE_WineIniUsed,buffer,MAX_PATHNAME_LEN);

            /* convert to the new format */
            sprintf( buffer, "%s/config", wine_get_config_dir() );
            convert_config( f, buffer );
            fclose( f );

            MESSAGE( "The '%s' configuration file has been converted\n"
                     "to the new format and saved as '%s'.\n", PROFILE_WineIniUsed, buffer );
            MESSAGE( "You should verify that the contents of the new file are correct,\n"
                     "and then remove the old one and restart Wine.\n" );
            ExitProcess(0);
        }
    }
    else WARN("could not get $HOME value for config file.\n" );

    MESSAGE( "Can't open configuration file %s/config\n", wine_get_config_dir() );
    return 0;
}


/***********************************************************************
 *           PROFILE_UsageWineIni
 *
 * Explain the wine.ini file to those who don't read documentation.
 * Keep below one screenful in length so that error messages above are
 * noticed.
 */
void PROFILE_UsageWineIni(void)
{
    MESSAGE("Perhaps you have not properly edited or created "
	"your Wine configuration file.\n");
    MESSAGE("This is (supposed to be) '%s/config'\n", wine_get_config_dir());
    /* RTFM, so to say */
}


/********************* API functions **********************************/

/***********************************************************************
 *           GetProfileInt   (KERNEL.57)
 */
UINT16 WINAPI GetProfileInt16( LPCSTR section, LPCSTR entry, INT16 def_val )
{
    return GetPrivateProfileInt16( section, entry, def_val, "win.ini" );
}


/***********************************************************************
 *           GetProfileIntA   (KERNEL32.@)
 */
UINT WINAPI GetProfileIntA( LPCSTR section, LPCSTR entry, INT def_val )
{
    return GetPrivateProfileIntA( section, entry, def_val, "win.ini" );
}

/***********************************************************************
 *           GetProfileIntW   (KERNEL32.@)
 */
UINT WINAPI GetProfileIntW( LPCWSTR section, LPCWSTR entry, INT def_val )
{
    return GetPrivateProfileIntW( section, entry, def_val, wininiW );
}

/*
 * if allow_section_name_copy is TRUE, allow the copying :
 *   - of Section names if 'section' is NULL
 *   - of Keys in a Section if 'entry' is NULL
 * (see MSDN doc for GetPrivateProfileString)
 */
static int PROFILE_GetPrivateProfileString( LPCWSTR section, LPCWSTR entry,
					    LPCWSTR def_val, LPWSTR buffer,
					    UINT len, LPCWSTR filename,
					    BOOL allow_section_name_copy )
{
    int		ret;
    LPWSTR	pDefVal = NULL;

    if (!filename)
	filename = wininiW;

    TRACE("%s,%s,%s,%p,%u,%s\n", debugstr_w(section), debugstr_w(entry),
          debugstr_w(def_val), buffer, len, debugstr_w(filename));

    /* strip any trailing ' ' of def_val. */
    if (def_val)
    {
        LPCWSTR p = &def_val[strlenW(def_val)]; /* even "" works ! */

	while (p > def_val)
	{
	    p--;
	    if ((*p) != ' ')
		break;
	}
	if (*p == ' ') /* ouch, contained trailing ' ' */
	{
	    int len = (int)(p - def_val);
	    pDefVal = HeapAlloc(GetProcessHeap(), 0, (len + 1) * sizeof(WCHAR));
	    strncpyW(pDefVal, def_val, len);
	    pDefVal[len] = '\0';
	}
    }
    if (!pDefVal)
	pDefVal = (LPWSTR)def_val;

    EnterCriticalSection( &PROFILE_CritSect );

    if (PROFILE_Open( filename )) {
	if ((allow_section_name_copy) && (section == NULL))
            ret = PROFILE_GetSectionNames(buffer, len);
	else
	    /* PROFILE_GetString already handles the 'entry == NULL' case */
            ret = PROFILE_GetString( section, entry, pDefVal, buffer, len );
    } else {
       lstrcpynW( buffer, pDefVal, len );
       ret = strlenW( buffer );
    }

    LeaveCriticalSection( &PROFILE_CritSect );

    if (pDefVal != def_val) /* allocated */
	HeapFree(GetProcessHeap(), 0, pDefVal);

    TRACE("returning %s, %d\n", debugstr_w(buffer), ret);

    return ret;
}

/***********************************************************************
 *           GetPrivateProfileString   (KERNEL.128)
 */
INT16 WINAPI GetPrivateProfileString16( LPCSTR section, LPCSTR entry,
                                        LPCSTR def_val, LPSTR buffer,
                                        UINT16 len, LPCSTR filename )
{
    UNICODE_STRING sectionW, entryW, def_valW, filenameW;
    LPWSTR bufferW;
    INT16 retW, ret = 0;

    bufferW = buffer ? HeapAlloc(GetProcessHeap(), 0, len * sizeof(WCHAR)) : NULL;
    if (section) RtlCreateUnicodeStringFromAsciiz(&sectionW, section);
    else sectionW.Buffer = NULL;
    if (entry) RtlCreateUnicodeStringFromAsciiz(&entryW, entry);
    else entryW.Buffer = NULL;
    if (def_val) RtlCreateUnicodeStringFromAsciiz(&def_valW, def_val);
    else def_valW.Buffer = NULL;
    if (filename) RtlCreateUnicodeStringFromAsciiz(&filenameW, filename);
    else filenameW.Buffer = NULL;

    retW = PROFILE_GetPrivateProfileString( sectionW.Buffer, entryW.Buffer,
                                     def_valW.Buffer, bufferW, len,
                                     filenameW.Buffer, FALSE );
    if (len)
    {
        ret = WideCharToMultiByte(CP_ACP, 0, bufferW, retW + 1, buffer, len, NULL, NULL);
        if (!ret)
        {
            ret = len - 1;
            buffer[ret] = 0;
        }
        else
            ret--; /* strip terminating 0 */
    }

    RtlFreeUnicodeString(&sectionW);
    RtlFreeUnicodeString(&entryW);
    RtlFreeUnicodeString(&def_valW);
    RtlFreeUnicodeString(&filenameW);
    if (bufferW) HeapFree(GetProcessHeap(), 0, bufferW);
    return ret;
}

/***********************************************************************
 *           GetPrivateProfileStringA   (KERNEL32.@)
 */
INT WINAPI GetPrivateProfileStringA( LPCSTR section, LPCSTR entry,
				     LPCSTR def_val, LPSTR buffer,
				     UINT len, LPCSTR filename )
{
    UNICODE_STRING sectionW, entryW, def_valW, filenameW;
    LPWSTR bufferW;
    INT retW, ret = 0;

    bufferW = buffer ? HeapAlloc(GetProcessHeap(), 0, len * sizeof(WCHAR)) : NULL;
    if (section) RtlCreateUnicodeStringFromAsciiz(&sectionW, section);
    else sectionW.Buffer = NULL;
    if (entry) RtlCreateUnicodeStringFromAsciiz(&entryW, entry);
    else entryW.Buffer = NULL;
    if (def_val) RtlCreateUnicodeStringFromAsciiz(&def_valW, def_val);
    else def_valW.Buffer = NULL;
    if (filename) RtlCreateUnicodeStringFromAsciiz(&filenameW, filename);
    else filenameW.Buffer = NULL;

    retW = GetPrivateProfileStringW( sectionW.Buffer, entryW.Buffer,
                                     def_valW.Buffer, bufferW, len,
                                     filenameW.Buffer);
    if (len)
    {
        ret = WideCharToMultiByte(CP_ACP, 0, bufferW, retW + 1, buffer, len, NULL, NULL);
        if (!ret)
        {
            ret = len - 1;
            buffer[ret] = 0;
        }
        else
            ret--; /* strip terminating 0 */
    }

    RtlFreeUnicodeString(&sectionW);
    RtlFreeUnicodeString(&entryW);
    RtlFreeUnicodeString(&def_valW);
    RtlFreeUnicodeString(&filenameW);
    if (bufferW) HeapFree(GetProcessHeap(), 0, bufferW);
    return ret;
}

/***********************************************************************
 *           GetPrivateProfileStringW   (KERNEL32.@)
 */
INT WINAPI GetPrivateProfileStringW( LPCWSTR section, LPCWSTR entry,
				     LPCWSTR def_val, LPWSTR buffer,
				     UINT len, LPCWSTR filename )
{
    return PROFILE_GetPrivateProfileString( section, entry, def_val,
                                            buffer, len, filename, TRUE );
}

/***********************************************************************
 *           GetProfileString   (KERNEL.58)
 */
INT16 WINAPI GetProfileString16( LPCSTR section, LPCSTR entry, LPCSTR def_val,
                                 LPSTR buffer, UINT16 len )
{
    return GetPrivateProfileString16( section, entry, def_val,
                                      buffer, len, "win.ini" );
}

/***********************************************************************
 *           GetProfileStringA   (KERNEL32.@)
 */
INT WINAPI GetProfileStringA( LPCSTR section, LPCSTR entry, LPCSTR def_val,
			      LPSTR buffer, UINT len )
{
    return GetPrivateProfileStringA( section, entry, def_val,
                                     buffer, len, "win.ini" );
}

/***********************************************************************
 *           GetProfileStringW   (KERNEL32.@)
 */
INT WINAPI GetProfileStringW( LPCWSTR section, LPCWSTR entry,
			      LPCWSTR def_val, LPWSTR buffer, UINT len )
{
    return GetPrivateProfileStringW( section, entry, def_val,
				     buffer, len, wininiW );
}

/***********************************************************************
 *           WriteProfileString   (KERNEL.59)
 */
BOOL16 WINAPI WriteProfileString16( LPCSTR section, LPCSTR entry,
                                    LPCSTR string )
{
    return WritePrivateProfileString16( section, entry, string, "win.ini" );
}

/***********************************************************************
 *           WriteProfileStringA   (KERNEL32.@)
 */
BOOL WINAPI WriteProfileStringA( LPCSTR section, LPCSTR entry,
				 LPCSTR string )
{
    return WritePrivateProfileStringA( section, entry, string, "win.ini" );
}

/***********************************************************************
 *           WriteProfileStringW   (KERNEL32.@)
 */
BOOL WINAPI WriteProfileStringW( LPCWSTR section, LPCWSTR entry,
                                     LPCWSTR string )
{
    return WritePrivateProfileStringW( section, entry, string, wininiW );
}


/***********************************************************************
 *           GetPrivateProfileInt   (KERNEL.127)
 */
UINT16 WINAPI GetPrivateProfileInt16( LPCSTR section, LPCSTR entry,
                                      INT16 def_val, LPCSTR filename )
{
    /* we used to have some elaborate return value limitation (<= -32768 etc.)
     * here, but Win98SE doesn't care about this at all, so I deleted it.
     * AFAIR versions prior to Win9x had these limits, though. */
    return (INT16)GetPrivateProfileIntA(section,entry,def_val,filename);
}

/***********************************************************************
 *           GetPrivateProfileIntA   (KERNEL32.@)
 */
UINT WINAPI GetPrivateProfileIntA( LPCSTR section, LPCSTR entry,
				   INT def_val, LPCSTR filename )
{
    char buffer[20];
    long result;

    if (!GetPrivateProfileStringA( section, entry, "",
                                   buffer, sizeof(buffer), filename ))
        return def_val;
    /* FIXME: if entry can be found but it's empty, then Win16 is
     * supposed to return 0 instead of def_val ! Difficult/problematic
     * to implement (every other failure also returns zero buffer),
     * thus wait until testing framework avail for making sure nothing
     * else gets broken that way. */
    if (!buffer[0]) return (UINT)def_val;

    /* Don't use strtol() here !
     * (returns LONG_MAX/MIN on overflow instead of "proper" overflow)
     YES, scan for unsigned format ! (otherwise compatibility error) */
    if (!sscanf(buffer, "%lu", &result)) return 0;
    return (UINT)result;
}

/***********************************************************************
 *           GetPrivateProfileIntW   (KERNEL32.@)
 *
 * FIXME: rewrite using unicode
 */
UINT WINAPI GetPrivateProfileIntW( LPCWSTR section, LPCWSTR entry,
				   INT def_val, LPCWSTR filename )
{
    LPSTR sectionA  = HEAP_strdupWtoA( GetProcessHeap(), 0, section );
    LPSTR entryA    = HEAP_strdupWtoA( GetProcessHeap(), 0, entry );
    LPSTR filenameA = HEAP_strdupWtoA( GetProcessHeap(), 0, filename );
    UINT res = GetPrivateProfileIntA(sectionA, entryA, def_val, filenameA);
    HeapFree( GetProcessHeap(), 0, sectionA );
    HeapFree( GetProcessHeap(), 0, filenameA );
    HeapFree( GetProcessHeap(), 0, entryA );
    return res;
}

/***********************************************************************
 *           GetPrivateProfileSection   (KERNEL.418)
 */
INT16 WINAPI GetPrivateProfileSection16( LPCSTR section, LPSTR buffer,
				        UINT16 len, LPCSTR filename )
{
    return GetPrivateProfileSectionA( section, buffer, len, filename );
}

/***********************************************************************
 *           GetPrivateProfileSectionW   (KERNEL32.@)
 */
INT WINAPI GetPrivateProfileSectionW( LPCWSTR section, LPWSTR buffer,
				      DWORD len, LPCWSTR filename )
{
    int		ret = 0;

    EnterCriticalSection( &PROFILE_CritSect );

    if (PROFILE_Open( filename ))
        ret = PROFILE_GetSection(CurProfile->section, section, buffer, len,
				 FALSE, TRUE);

    LeaveCriticalSection( &PROFILE_CritSect );

    return ret;
}

/***********************************************************************
 *           GetPrivateProfileSectionA   (KERNEL32.@)
 */
INT WINAPI GetPrivateProfileSectionA( LPCSTR section, LPSTR buffer,
                                      DWORD len, LPCSTR filename )
{
    UNICODE_STRING sectionW, filenameW;
    LPWSTR bufferW;
    INT retW, ret = 0;

    bufferW = buffer ? HeapAlloc(GetProcessHeap(), 0, len * sizeof(WCHAR)) : NULL;
    if (section) RtlCreateUnicodeStringFromAsciiz(&sectionW, section);
    else sectionW.Buffer = NULL;
    if (filename) RtlCreateUnicodeStringFromAsciiz(&filenameW, filename);
    else filenameW.Buffer = NULL;

    retW = GetPrivateProfileSectionW(sectionW.Buffer, bufferW, len, filenameW.Buffer);
    if (len > 2)
    {
        ret = WideCharToMultiByte(CP_ACP, 0, bufferW, retW + 2, buffer, len, NULL, NULL);
        if (ret > 2)
            ret -= 2;
        else
        {
            ret = 0;
            buffer[len-2] = 0;
            buffer[len-1] = 0;
        }
    }
    else
    {
        buffer[0] = 0;
        buffer[1] = 0;
    }

    RtlFreeUnicodeString(&sectionW);
    RtlFreeUnicodeString(&filenameW);
    if (bufferW) HeapFree(GetProcessHeap(), 0, bufferW);
    return ret;
}

/***********************************************************************
 *           GetProfileSection   (KERNEL.419)
 */
INT16 WINAPI GetProfileSection16( LPCSTR section, LPSTR buffer, UINT16 len )
{
    return GetPrivateProfileSection16( section, buffer, len, "win.ini" );
}

/***********************************************************************
 *           GetProfileSectionA   (KERNEL32.@)
 */
INT WINAPI GetProfileSectionA( LPCSTR section, LPSTR buffer, DWORD len )
{
    return GetPrivateProfileSectionA( section, buffer, len, "win.ini" );
}

/***********************************************************************
 *           GetProfileSectionW   (KERNEL32.@)
 */
INT WINAPI GetProfileSectionW( LPCWSTR section, LPWSTR buffer, DWORD len )
{
    return GetPrivateProfileSectionW( section, buffer, len, wininiW );
}


/***********************************************************************
 *           WritePrivateProfileString   (KERNEL.129)
 */
BOOL16 WINAPI WritePrivateProfileString16( LPCSTR section, LPCSTR entry,
                                           LPCSTR string, LPCSTR filename )
{
    return WritePrivateProfileStringA(section,entry,string,filename);
}

/***********************************************************************
 *           WritePrivateProfileStringW   (KERNEL32.@)
 */
BOOL WINAPI WritePrivateProfileStringW( LPCWSTR section, LPCWSTR entry,
					LPCWSTR string, LPCWSTR filename )
{
    BOOL ret = FALSE;

    EnterCriticalSection( &PROFILE_CritSect );

    if (PROFILE_Open( filename ))
    {
        if (!section && !entry && !string) /* documented "file flush" case */
        {
            PROFILE_FlushFile();
            PROFILE_ReleaseFile();  /* always return FALSE in this case */
        }
	else {
	    if (!section) {
		FIXME("(NULL?,%s,%s,%s)?\n",
                      debugstr_w(entry), debugstr_w(string), debugstr_w(filename));
	    } else {
		ret = PROFILE_SetString( section, entry, string, FALSE);
		PROFILE_FlushFile();
	    }
	}
    }

    LeaveCriticalSection( &PROFILE_CritSect );
    return ret;
}

/***********************************************************************
 *           WritePrivateProfileStringA   (KERNEL32.@)
 */
BOOL WINAPI WritePrivateProfileStringA( LPCSTR section, LPCSTR entry,
					LPCSTR string, LPCSTR filename )
{
    UNICODE_STRING sectionW, entryW, stringW, filenameW;
    BOOL ret;

    if (section) RtlCreateUnicodeStringFromAsciiz(&sectionW, section);
    else sectionW.Buffer = NULL;
    if (entry) RtlCreateUnicodeStringFromAsciiz(&entryW, entry);
    else entryW.Buffer = NULL;
    if (string) RtlCreateUnicodeStringFromAsciiz(&stringW, string);
    else stringW.Buffer = NULL;
    if (filename) RtlCreateUnicodeStringFromAsciiz(&filenameW, filename);
    else filenameW.Buffer = NULL;

    ret = WritePrivateProfileStringW(sectionW.Buffer, entryW.Buffer,
                                     stringW.Buffer, filenameW.Buffer);
    RtlFreeUnicodeString(&sectionW);
    RtlFreeUnicodeString(&entryW);
    RtlFreeUnicodeString(&stringW);
    RtlFreeUnicodeString(&filenameW);
    return ret;
}

/***********************************************************************
 *           WritePrivateProfileSection   (KERNEL.416)
 */
BOOL16 WINAPI WritePrivateProfileSection16( LPCSTR section,
				 	    LPCSTR string, LPCSTR filename )
{
    return WritePrivateProfileSectionA( section, string, filename );
}

/***********************************************************************
 *           WritePrivateProfileSectionW   (KERNEL32.@)
 */
BOOL WINAPI WritePrivateProfileSectionW( LPCWSTR section,
                                         LPCWSTR string, LPCWSTR filename )
{
    BOOL ret = FALSE;
    LPWSTR p;

    EnterCriticalSection( &PROFILE_CritSect );

    if (PROFILE_Open( filename )) {
        if (!section && !string)
            PROFILE_ReleaseFile();  /* always return FALSE in this case */
        else if (!string) {/* delete the named section*/
	    ret = PROFILE_SetString(section,NULL,NULL, FALSE);
	    PROFILE_FlushFile();
        } else {
	    PROFILE_DeleteAllKeys(section);
	    ret = TRUE;
	    while(*string) {
                LPWSTR buf = HeapAlloc( GetProcessHeap(), 0, (strlenW(string)+1) * sizeof(WCHAR) );
                strcpyW( buf, string );
                if((p = strchrW( buf, '='))) {
                    *p='\0';
                    ret = PROFILE_SetString( section, buf, p+1, TRUE);
                }
                HeapFree( GetProcessHeap(), 0, buf );
                string += strlenW(string)+1;
            }
            PROFILE_FlushFile();
        }
    }

    LeaveCriticalSection( &PROFILE_CritSect );
    return ret;
}

/***********************************************************************
 *           WritePrivateProfileSectionA   (KERNEL32.@)
 */
BOOL WINAPI WritePrivateProfileSectionA( LPCSTR section,
                                         LPCSTR string, LPCSTR filename)

{
    UNICODE_STRING sectionW, filenameW;
    LPWSTR stringW;
    BOOL ret;

    if (string)
    {
        INT lenA, lenW;
        LPCSTR p = string;

        while(*p) p += strlen(p) + 1;
        lenA = p - string + 1;
        lenW = MultiByteToWideChar(CP_ACP, 0, string, lenA, NULL, 0);
        if ((stringW = HeapAlloc(GetProcessHeap(), 0, lenW * sizeof(WCHAR))))
            MultiByteToWideChar(CP_ACP, 0, string, lenA, stringW, lenW);
    }
    else stringW = NULL;
    if (section) RtlCreateUnicodeStringFromAsciiz(&sectionW, section);
    else sectionW.Buffer = NULL;
    if (filename) RtlCreateUnicodeStringFromAsciiz(&filenameW, filename);
    else filenameW.Buffer = NULL;

    ret = WritePrivateProfileSectionW(sectionW.Buffer, stringW, filenameW.Buffer);

    HeapFree(GetProcessHeap(), 0, stringW);
    RtlFreeUnicodeString(&sectionW);
    RtlFreeUnicodeString(&filenameW);
    return ret;
}

/***********************************************************************
 *           WriteProfileSection   (KERNEL.417)
 */
BOOL16 WINAPI WriteProfileSection16( LPCSTR section, LPCSTR keys_n_values)
{
    return WritePrivateProfileSection16( section, keys_n_values, "win.ini");
}

/***********************************************************************
 *           WriteProfileSectionA   (KERNEL32.@)
 */
BOOL WINAPI WriteProfileSectionA( LPCSTR section, LPCSTR keys_n_values)

{
    return WritePrivateProfileSectionA( section, keys_n_values, "win.ini");
}

/***********************************************************************
 *           WriteProfileSectionW   (KERNEL32.@)
 */
BOOL WINAPI WriteProfileSectionW( LPCWSTR section, LPCWSTR keys_n_values)
{
   return WritePrivateProfileSectionW(section, keys_n_values, wininiW);
}

/***********************************************************************
 *           GetPrivateProfileSectionNames   (KERNEL.143)
 */
WORD WINAPI GetPrivateProfileSectionNames16( LPSTR buffer, WORD size,
                                             LPCSTR filename )
{
    return GetPrivateProfileSectionNamesA(buffer,size,filename);
}


/***********************************************************************
 *           GetProfileSectionNames   (KERNEL.142)
 */
WORD WINAPI GetProfileSectionNames16(LPSTR buffer, WORD size)

{
    return GetPrivateProfileSectionNamesA(buffer,size,"win.ini");
}


/***********************************************************************
 *           GetPrivateProfileSectionNamesW  (KERNEL32.@)
 *
 * Returns the section names contained in the specified file.
 * FIXME: Where do we find this file when the path is relative?
 * The section names are returned as a list of strings with an extra
 * '\0' to mark the end of the list. Except for that the behavior
 * depends on the Windows version.
 *
 * Win95:
 * - if the buffer is 0 or 1 character long then it is as if it was of
 *   infinite length.
 * - otherwise, if the buffer is to small only the section names that fit
 *   are returned.
 * - note that this means if the buffer was to small to return even just
 *   the first section name then a single '\0' will be returned.
 * - the return value is the number of characters written in the buffer,
 *   except if the buffer was too smal in which case len-2 is returned
 *
 * Win2000:
 * - if the buffer is 0, 1 or 2 characters long then it is filled with
 *   '\0' and the return value is 0
 * - otherwise if the buffer is too small then the first section name that
 *   does not fit is truncated so that the string list can be terminated
 *   correctly (double '\0')
 * - the return value is the number of characters written in the buffer
 *   except for the trailing '\0'. If the buffer is too small, then the
 *   return value is len-2
 * - Win2000 has a bug that triggers when the section names and the
 *   trailing '\0' fit exactly in the buffer. In that case the trailing
 *   '\0' is missing.
 *
 * Wine implements the observed Win2000 behavior (except for the bug).
 *
 * Note that when the buffer is big enough then the return value may be any
 * value between 1 and len-1 (or len in Win95), including len-2.
 */
DWORD WINAPI GetPrivateProfileSectionNamesW( LPWSTR buffer, DWORD size,
					     LPCWSTR filename)
{
    DWORD ret = 0;

    EnterCriticalSection( &PROFILE_CritSect );

    if (PROFILE_Open( filename ))
        ret = PROFILE_GetSectionNames(buffer, size);

    LeaveCriticalSection( &PROFILE_CritSect );

    return ret;
}


/***********************************************************************
 *           GetPrivateProfileSectionNamesA  (KERNEL32.@)
 */
DWORD WINAPI GetPrivateProfileSectionNamesA( LPSTR buffer, DWORD size,
					     LPCSTR filename)
{
    UNICODE_STRING filenameW;
    LPWSTR bufferW;
    INT retW, ret = 0;

    bufferW = buffer ? HeapAlloc(GetProcessHeap(), 0, size * sizeof(WCHAR)) : NULL;
    if (filename) RtlCreateUnicodeStringFromAsciiz(&filenameW, filename);
    else filenameW.Buffer = NULL;

    retW = GetPrivateProfileSectionNamesW(bufferW, size, filenameW.Buffer);
    if (retW && size)
    {
        ret = WideCharToMultiByte(CP_ACP, 0, bufferW, retW, buffer, size, NULL, NULL);
        if (!ret)
        {
            ret = size;
            buffer[size-1] = 0;
        }
    }

    RtlFreeUnicodeString(&filenameW);
    if (bufferW) HeapFree(GetProcessHeap(), 0, bufferW);
    return ret;
}

/***********************************************************************
 *           GetPrivateProfileStruct (KERNEL.407)
 */
BOOL16 WINAPI GetPrivateProfileStruct16(LPCSTR section, LPCSTR key,
 				        LPVOID buf, UINT16 len, LPCSTR filename)
{
    return GetPrivateProfileStructA( section, key, buf, len, filename );
}

/***********************************************************************
 *           GetPrivateProfileStructW (KERNEL32.@)
 *
 * Should match Win95's behaviour pretty much
 */
BOOL WINAPI GetPrivateProfileStructW (LPCWSTR section, LPCWSTR key,
                                      LPVOID buf, UINT len, LPCWSTR filename)
{
    BOOL	ret = FALSE;

    EnterCriticalSection( &PROFILE_CritSect );

    if (PROFILE_Open( filename )) {
        PROFILEKEY *k = PROFILE_Find ( &CurProfile->section, section, key, FALSE, FALSE);
	if (k) {
	    TRACE("value (at %p): %s\n", k->value, debugstr_w(k->value));
	    if (((strlenW(k->value) - 2) / 2) == len)
	    {
		LPWSTR end, p;
		BOOL valid = TRUE;
		WCHAR c;
		DWORD chksum = 0;

	        end  = k->value + strlenW(k->value); /* -> '\0' */
	        /* check for invalid chars in ASCII coded hex string */
	        for (p=k->value; p < end; p++)
		{
                    if (!isxdigitW(*p))
		    {
			WARN("invalid char '%x' in file %s->[%s]->%s !\n",
                             *p, debugstr_w(filename), debugstr_w(section), debugstr_w(key));
		        valid = FALSE;
		        break;
		    }
		}
		if (valid)
		{
		    BOOL highnibble = TRUE;
		    BYTE b = 0, val;
		    LPBYTE binbuf = (LPBYTE)buf;

	            end -= 2; /* don't include checksum in output data */
	            /* translate ASCII hex format into binary data */
                    for (p=k->value; p < end; p++)
            	    {
	        	c = toupperW(*p);
			val = (c > '9') ?
				(c - 'A' + 10) : (c - '0');

			if (highnibble)
		    	    b = val << 4;
			else
			{
		    	    b += val;
		    	    *binbuf++ = b; /* feed binary data into output */
		    	    chksum += b; /* calculate checksum */
			}
			highnibble ^= 1; /* toggle */
            	    }
		    /* retrieve stored checksum value */
		    c = toupperW(*p++);
		    b = ( (c > '9') ? (c - 'A' + 10) : (c - '0') ) << 4;
		    c = toupperW(*p);
		    b +=  (c > '9') ? (c - 'A' + 10) : (c - '0');
	            if (b == (chksum & 0xff)) /* checksums match ? */
                        ret = TRUE;
                }
            }
	}
    }
    LeaveCriticalSection( &PROFILE_CritSect );

    return ret;
}

/***********************************************************************
 *           GetPrivateProfileStructA (KERNEL32.@)
 */
BOOL WINAPI GetPrivateProfileStructA (LPCSTR section, LPCSTR key,
				      LPVOID buffer, UINT len, LPCSTR filename)
{
    UNICODE_STRING sectionW, keyW, filenameW;
    INT ret;

    if (section) RtlCreateUnicodeStringFromAsciiz(&sectionW, section);
    else sectionW.Buffer = NULL;
    if (key) RtlCreateUnicodeStringFromAsciiz(&keyW, key);
    else keyW.Buffer = NULL;
    if (filename) RtlCreateUnicodeStringFromAsciiz(&filenameW, filename);
    else filenameW.Buffer = NULL;

    ret = GetPrivateProfileStructW(sectionW.Buffer, keyW.Buffer, buffer, len,
                                   filenameW.Buffer);
    /* Do not translate binary data. */

    RtlFreeUnicodeString(&sectionW);
    RtlFreeUnicodeString(&keyW);
    RtlFreeUnicodeString(&filenameW);
    return ret;
}



/***********************************************************************
 *           WritePrivateProfileStruct (KERNEL.406)
 */
BOOL16 WINAPI WritePrivateProfileStruct16 (LPCSTR section, LPCSTR key,
	LPVOID buf, UINT16 bufsize, LPCSTR filename)
{
    return WritePrivateProfileStructA( section, key, buf, bufsize, filename );
}

/***********************************************************************
 *           WritePrivateProfileStructW (KERNEL32.@)
 */
BOOL WINAPI WritePrivateProfileStructW (LPCWSTR section, LPCWSTR key,
                                        LPVOID buf, UINT bufsize, LPCWSTR filename)
{
    BOOL ret = FALSE;
    LPBYTE binbuf;
    LPWSTR outstring, p;
    DWORD sum = 0;

    if (!section && !key && !buf)  /* flush the cache */
        return WritePrivateProfileStringW( NULL, NULL, NULL, filename );

    /* allocate string buffer for hex chars + checksum hex char + '\0' */
    outstring = HeapAlloc( GetProcessHeap(), 0, (bufsize*2 + 2 + 1) * sizeof(WCHAR) );
    p = outstring;
    for (binbuf = (LPBYTE)buf; binbuf < (LPBYTE)buf+bufsize; binbuf++) {
      *p++ = hex[*binbuf >> 4];
      *p++ = hex[*binbuf & 0xf];
      sum += *binbuf;
    }
    /* checksum is sum & 0xff */
    *p++ = hex[(sum & 0xf0) >> 4];
    *p++ = hex[sum & 0xf];
    *p++ = '\0';

    EnterCriticalSection( &PROFILE_CritSect );

    if (PROFILE_Open( filename )) {
        ret = PROFILE_SetString( section, key, outstring, FALSE);
        PROFILE_FlushFile();
    }

    LeaveCriticalSection( &PROFILE_CritSect );

    HeapFree( GetProcessHeap(), 0, outstring );

    return ret;
}

/***********************************************************************
 *           WritePrivateProfileStructA (KERNEL32.@)
 */
BOOL WINAPI WritePrivateProfileStructA (LPCSTR section, LPCSTR key,
					LPVOID buf, UINT bufsize, LPCSTR filename)
{
    UNICODE_STRING sectionW, keyW, filenameW;
    INT ret;

    if (section) RtlCreateUnicodeStringFromAsciiz(&sectionW, section);
    else sectionW.Buffer = NULL;
    if (key) RtlCreateUnicodeStringFromAsciiz(&keyW, key);
    else keyW.Buffer = NULL;
    if (filename) RtlCreateUnicodeStringFromAsciiz(&filenameW, filename);
    else filenameW.Buffer = NULL;

    /* Do not translate binary data. */
    ret = WritePrivateProfileStructW(sectionW.Buffer, keyW.Buffer, buf, bufsize,
                                     filenameW.Buffer);

    RtlFreeUnicodeString(&sectionW);
    RtlFreeUnicodeString(&keyW);
    RtlFreeUnicodeString(&filenameW);
    return ret;
}


/***********************************************************************
 *           WriteOutProfiles   (KERNEL.315)
 */
void WINAPI WriteOutProfiles16(void)
{
    EnterCriticalSection( &PROFILE_CritSect );
    PROFILE_FlushFile();
    LeaveCriticalSection( &PROFILE_CritSect );
}

/***********************************************************************
 *           CloseProfileUserMapping   (KERNEL32.@)
 */
BOOL WINAPI CloseProfileUserMapping(void) {
    FIXME("(), stub!\n");
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}
