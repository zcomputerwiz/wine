/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Standalone context checks; link only with uwp_context.o and this test shim. */
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ntstatus.h"
#include "winternl.h"
#include "object.h"
#include "thread.h"
#include "uwp_context.h"
struct thread *current;
unsigned int global_error;
static int fail_alloc;
void *mem_alloc(size_t size) { if (fail_alloc) { global_error = STATUS_NO_MEMORY; return NULL; } return malloc(size); }
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } checks++; } while (0)
int main(void)
{
    WCHAR full[] = {'A','b','c'}, family[] = {'F','a','m'}, app[] = {'A','p','p'}, root[] = {'C',':','\\','x'};
    WCHAR nul[] = {'A',0,'B'};
    struct uwp_context *c;
    const struct unicode_str *v;
    unsigned int checks = 0;
    c = uwp_context_create(full,sizeof(full),family,sizeof(family),app,sizeof(app),root,sizeof(root));
    CHECK(c != NULL);
    v=uwp_context_get_package_full_name(c); CHECK(v && v->len==sizeof(full) && !memcmp(v->str,full,sizeof(full)));
    v=uwp_context_get_family_name(c); CHECK(v && v->len==sizeof(family) && !memcmp(v->str,family,sizeof(family)));
    v=uwp_context_get_app_id(c); CHECK(v && v->len==sizeof(app) && !memcmp(v->str,app,sizeof(app)));
    v=uwp_context_get_installed_root(c); CHECK(v && v->len==sizeof(root) && !memcmp(v->str,root,sizeof(root)));
    full[0]='Z'; CHECK(uwp_context_get_package_full_name(c)->str[0]=='A');
    CHECK(uwp_context_retain(c)==c); uwp_context_release(c); CHECK(uwp_context_get_app_id(c)->str[0]=='A'); uwp_context_release(c);
    CHECK(!uwp_context_retain(NULL)); uwp_context_release(NULL);
    global_error=0; CHECK(!uwp_context_create(NULL,sizeof(full),family,sizeof(family),app,sizeof(app),root,sizeof(root)) && global_error==STATUS_INVALID_PARAMETER);
    global_error=0; CHECK(!uwp_context_create(full,0,family,sizeof(family),app,sizeof(app),root,sizeof(root)) && global_error==STATUS_INVALID_PARAMETER);
    global_error=0; CHECK(!uwp_context_create(full,sizeof(full),family,0,app,sizeof(app),root,sizeof(root)) && global_error==STATUS_INVALID_PARAMETER);
    global_error=0; CHECK(!uwp_context_create(full,sizeof(full),family,sizeof(family),app,0,root,sizeof(root)) && global_error==STATUS_INVALID_PARAMETER);
    global_error=0; CHECK(!uwp_context_create(full,sizeof(full),family,sizeof(family),app,sizeof(app),root,0) && global_error==STATUS_INVALID_PARAMETER);
    global_error=0; CHECK(!uwp_context_create(full,1,family,sizeof(family),app,sizeof(app),root,sizeof(root)) && global_error==STATUS_INVALID_PARAMETER);
    global_error=0; CHECK(!uwp_context_create(nul,sizeof(nul),family,sizeof(family),app,sizeof(app),root,sizeof(root)) && global_error==STATUS_INVALID_PARAMETER);
    global_error=0; CHECK(!uwp_context_create(full,sizeof(full),nul,sizeof(nul),app,sizeof(app),root,sizeof(root)) && global_error==STATUS_INVALID_PARAMETER);
    global_error=0; CHECK(!uwp_context_create(full,sizeof(full),family,sizeof(family),nul,sizeof(nul),root,sizeof(root)) && global_error==STATUS_INVALID_PARAMETER);
    global_error=0; CHECK(!uwp_context_create(full,sizeof(full),family,sizeof(family),app,sizeof(app),nul,sizeof(nul)) && global_error==STATUS_INVALID_PARAMETER);
    global_error=0; CHECK(!uwp_context_create(full,UWP_CONTEXT_MAX_FIELD_BYTES+2,family,sizeof(family),app,sizeof(app),root,sizeof(root)) && global_error==STATUS_INVALID_PARAMETER);
    fail_alloc=1; global_error=0; CHECK(!uwp_context_create(full,sizeof(full),family,sizeof(family),app,sizeof(app),root,sizeof(root)) && global_error==STATUS_NO_MEMORY);
    printf("context checks=%u PASS\n",checks); return 0;
}
