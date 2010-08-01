/*
 *  JSAPI <-> Navigator page object
 *  Copyright (C) 2010 Andreas Öman
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>
#include <regex.h>
#include "js.h"


#include "backend/backend.h"
#include "navigator.h"
#include "misc/string.h"




/**
 *
 */
LIST_HEAD(jsroute_list, jsroute);

static struct jsroute_list jslist;


/**
 *
 */
typedef struct jsroute {
  LIST_ENTRY(jsroute) jsr_link;
  regex_t jsr_regex;
  jsval jsr_openfunc;
} jsroute_t;


/**
 *
 */
typedef struct js_page {
  int jp_refcount;

  char **jp_args;

  jsval jp_openfunc;

  prop_t *jp_loading;
  prop_t *jp_nodes;
  prop_t *jp_type;
  prop_t *jp_title;

} js_page_t;


/**
 *
 */
static void
js_page_destroy(js_page_t *jp)
{
  if(jp->jp_args)
    strvec_free(jp->jp_args);

  prop_ref_dec(jp->jp_loading);
  prop_ref_dec(jp->jp_nodes);
  prop_ref_dec(jp->jp_type);
  prop_ref_dec(jp->jp_title);
  
  free(jp);
}


/**
 *
 */
static void
js_page_release(js_page_t *jp)
{
  if(atomic_add(&jp->jp_refcount, -1) == 1)
    js_page_destroy(jp);
}


/**
 *
 */
static JSBool 
js_setTitle(JSContext *cx, JSObject *obj, jsval idval, jsval *vp)
{
  js_page_t *jp = JS_GetPrivate(cx, obj);
  prop_set_string(jp->jp_title,
		  JS_GetStringBytes(JS_ValueToString(cx, *vp)));
  return JS_TRUE;
}


/**
 *
 */
static JSBool 
js_setType(JSContext *cx, JSObject *obj, jsval idval, jsval *vp)
{
  js_page_t *jp = JS_GetPrivate(cx, obj);

  prop_set_string(jp->jp_type, JS_GetStringBytes(JS_ValueToString(cx, *vp)));
  return JS_TRUE;
}


/**
 *
 */
static JSBool 
js_setLoading(JSContext *cx, JSObject *obj, jsval idval, jsval *vp)
{ 
  js_page_t *jp = JS_GetPrivate(cx, obj);
  JSBool on;

  if(!JS_ValueToBoolean(cx, *vp, &on))
    return JS_FALSE;

  prop_set_int(jp->jp_loading, on);
  return JS_TRUE;
}


/**
 *
 */
static JSBool 
js_appendItem(JSContext *cx, JSObject *obj, uintN argc,
	      jsval *argv, jsval *rval)
{
  const char *url;
  const char *type;
  JSObject *metaobj = NULL;
  prop_t *item;

  if(!JS_ConvertArguments(cx, argc, argv, "ss/o", &url, &type, &metaobj))
    return JS_FALSE;

  item = prop_create(NULL, NULL);

  if(metaobj != NULL) {
    JSIdArray *ida;
    prop_t *metadata;
    int i;
    if((ida = JS_Enumerate(cx, metaobj)) == NULL) {
      prop_destroy(item);
      return JS_FALSE;
    }

    metadata = prop_create(item, "metadata");

    for(i = 0; i < ida->length; i++) {
      jsval name, value;
      prop_t *val;
      if(!JS_IdToValue(cx, ida->vector[i], &name))
	continue;

      if(!JSVAL_IS_STRING(name))
	continue;

      if(!JS_GetProperty(cx, metaobj, JS_GetStringBytes(JSVAL_TO_STRING(name)),
			 &value))
	continue;

      val = prop_create(metadata, JS_GetStringBytes(JSVAL_TO_STRING(name)));
      if(JSVAL_IS_INT(value)) {
	prop_set_int(val, JSVAL_TO_INT(value));
      } else if(JSVAL_IS_DOUBLE(value)) {
	double d;
	if(JS_ValueToNumber(cx, value, &d))
	  prop_set_float(val, d);
      } else {
	prop_set_string(val, JS_GetStringBytes(JS_ValueToString(cx, value)));
      }
    }
    JS_DestroyIdArray(cx, ida);
  }

  prop_set_string(prop_create(item, "url"), url);
  prop_set_string(prop_create(item, "type"), type);
  js_page_t *jp = JS_GetPrivate(cx, obj);

  if(prop_set_parent(item, jp->jp_nodes))
    prop_destroy(item);

  *rval = JSVAL_VOID;
  return JS_TRUE;
  

}


/**
 *
 */
static JSFunctionSpec page_functions[] = {
    JS_FS("appendItem",         js_appendItem,  3, 0, 0),
    JS_FS_END
};


/**
 *
 */
static JSPropertySpec page_properties[] = {
  { "title",      0, 0,         NULL, js_setTitle },
  { "type",       0, 0,         NULL, js_setType },
  { "loading",    0, 0,         NULL, js_setLoading },
  { NULL },
};

/**
 *
 */
static void
finalize(JSContext *cx, JSObject *obj)
{
  js_page_t *jp = JS_GetPrivate(cx, obj);
  js_page_release(jp);
}


static JSClass page_class = {
  "page", JSCLASS_HAS_PRIVATE,
  JS_PropertyStub,JS_PropertyStub,JS_PropertyStub,JS_PropertyStub,
  JS_EnumerateStub,JS_ResolveStub,JS_ConvertStub, finalize,
  JSCLASS_NO_OPTIONAL_MEMBERS
};






/**
 *
 */
static void
js_open_invoke(JSContext *cx, js_page_t *jp)
{
  jsval *argv, result;
  void *mark;
  char argfmt[10];
  int i = 0, argc;
  JSObject *obj = JS_NewObjectWithGivenProto(cx, &page_class, NULL, NULL);

  JS_SetPrivate(cx, obj, jp);
  atomic_add(&jp->jp_refcount, 1);

  JS_DefineFunctions(cx, obj, page_functions);
  JS_DefineProperties(cx, obj, page_properties);
 
  if(jp->jp_args != NULL) {
    argfmt[0] = 'o';
    while(i < 8 && jp->jp_args[i]) {
      argfmt[i+1] = 's';
      i++;
    }
    argfmt[i+1] = 0;
    argc = i+1;
    argv = JS_PushArguments(cx, &mark, argfmt, obj,
			    i > 0 ? jp->jp_args[0] : "",
			    i > 1 ? jp->jp_args[1] : "",
			    i > 2 ? jp->jp_args[2] : "",
			    i > 3 ? jp->jp_args[3] : "",
			    i > 4 ? jp->jp_args[4] : "",
			    i > 5 ? jp->jp_args[5] : "",
			    i > 6 ? jp->jp_args[6] : "",
			    i > 7 ? jp->jp_args[7] : "");

  } else {
    argv = JS_PushArguments(cx, &mark, "o", obj);
    argc = 2;
  }
  if(argv == NULL)
    return;
  JS_CallFunctionValue(cx, NULL, jp->jp_openfunc, argc, argv, &result);
  JS_PopArguments(cx, mark);
}


/**
 *
 */
static void *
js_open_trampoline(void *arg)
{
  struct js_page *jp = arg;
  
  JSContext *cx = js_newctx();
  JS_BeginRequest(cx);

  js_open_invoke(cx, jp);

  JS_EndRequest(cx);
  JS_DestroyContext(cx);

  js_page_release(jp);
  return NULL;
}



/**
 *
 */
struct nav_page *
js_page_open(struct backend *be, struct navigator *nav, 
	     const char *url, const char *view,
	     char *errbuf, size_t errlen)
{
  jsroute_t *jsr;
  regmatch_t matches[8];
  int i;
  nav_page_t *np;
  js_page_t *jp;
  prop_t *model, *meta;

  LIST_FOREACH(jsr, &jslist, jsr_link)
    if(!regexec(&jsr->jsr_regex, url, 8, matches, 0))
      break;

  if(jsr == NULL)
    return BACKEND_NOURI;

  jp = calloc(1, sizeof(js_page_t));
  jp->jp_refcount = 1;
  for(i = 1; i < 8; i++)
    if(matches[i].rm_so != -1)
      strvec_addpn(&jp->jp_args, url + matches[i].rm_so, 
		   matches[i].rm_eo - matches[i].rm_so);
  
  np = nav_page_create(nav, url, view, NAV_PAGE_DONT_CLOSE_ON_BACK);

  model = prop_create(np->np_prop_root, "model");
  meta  = prop_create(model, "metadata");

  prop_ref_inc(jp->jp_loading = prop_create(model, "loading"));
  prop_ref_inc(jp->jp_nodes   = prop_create(model, "nodes"));
  prop_ref_inc(jp->jp_type    = prop_create(model, "type"));
  prop_ref_inc(jp->jp_title   = prop_create(meta,  "title"));
  jp->jp_openfunc = jsr->jsr_openfunc;

  hts_thread_create_detached("jsOpen", js_open_trampoline, jp);
			     
  prop_set_int(jp->jp_loading, 1);
  return np;
}


/**
 *
 */
JSBool 
js_addURI(JSContext *cx, JSObject *obj, uintN argc, 
	  jsval *argv, jsval *rval)
{
  const char *str;
  
  str = JS_GetStringBytes(JS_ValueToString(cx, argv[0]));

  if(!JS_ObjectIsFunction(cx, JSVAL_TO_OBJECT(argv[1]))) {
    JS_ReportError(cx, "Argument is not a function");
    return JS_FALSE;
  }

  if(str[0] != '^') {
    int l = strlen(str);
    char *s = alloca(l + 2);
    s[0] = '^';
    memcpy(s+1, str, l+1);
    str = s;
  }
    
  jsroute_t *jsr = calloc(1, sizeof(jsroute_t));
  TRACE(TRACE_DEBUG, "JS", "Add route for %s", str);

  if(regcomp(&jsr->jsr_regex, str, REG_EXTENDED | REG_ICASE)) {
    free(jsr);
    JS_ReportError(cx, "Invalid regular expression");
    return JS_FALSE;
  }

  jsr->jsr_openfunc = argv[1];
  JS_AddNamedRoot(cx, &jsr->jsr_openfunc, "routeduri");
  LIST_INSERT_HEAD(&jslist, jsr, jsr_link);
  *rval = JSVAL_VOID;
  return JS_TRUE;
}