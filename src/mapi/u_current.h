#ifndef _U_CURRENT_H_
#define _U_CURRENT_H_

#include "util/macros.h"


#if defined(MAPI_MODE_UTIL) || defined(MAPI_MODE_GLAPI) || \
    defined(MAPI_MODE_BRIDGE)

#include "glapi/glapi.h"

#define u_current_table _glapi_tls_Dispatch
#define u_current_context _glapi_tls_Context

#define u_current_get_table_internal _glapi_get_dispatch
#define u_current_get_context_internal _glapi_get_context

#define u_current_table_tsd _gl_DispatchTSD

#else /* MAPI_MODE_UTIL || MAPI_MODE_GLAPI || MAPI_MODE_BRIDGE */

struct _glapi_table;

extern __THREAD_INITIAL_EXEC struct _glapi_table *u_current_table;
extern __THREAD_INITIAL_EXEC void *u_current_context;

#endif /* MAPI_MODE_UTIL || MAPI_MODE_GLAPI || MAPI_MODE_BRIDGE */

void
u_current_set_table(const struct _glapi_table *tbl);

_GLAPI_EXPORT struct _glapi_table *
u_current_get_table_internal(void);

void
u_current_set_context(const void *ptr);

_GLAPI_EXPORT void *
u_current_get_context_internal(void);

#endif /* _U_CURRENT_H_ */
