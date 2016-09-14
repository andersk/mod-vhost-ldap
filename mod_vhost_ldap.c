/* ============================================================
 * Copyright (c) 2003-2004, Ondrej Sury
 * All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * 
 */

/*
 * mod_vhost_ldap.c --- read virtual host config from LDAP directory
 */

#define CORE_PRIVATE

#include <unistd.h>

#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_log.h"
#include "http_request.h"
#include "http_vhost.h"
#include "apr_version.h"
#include "apr_ldap.h"
#include "apr_reslist.h"
#include "apr_strings.h"
#include "apr_tables.h"
#include "util_ldap.h"
#include "util_script.h"

#if !defined(APU_HAS_LDAP) && !defined(APR_HAS_LDAP)
#error mod_vhost_ldap requires APR-util to have LDAP support built in
#endif

const char USERDIR[] = "web_scripts";

#define MAX_FAILURES 5

module AP_MODULE_DECLARE_DATA vhost_ldap_module;

typedef enum {
    MVL_UNSET, MVL_DISABLED, MVL_ENABLED
} mod_vhost_ldap_status_e;

typedef struct mod_vhost_ldap_config_t {
    mod_vhost_ldap_status_e enabled;			/* Is vhost_ldap enabled? */

    /* These parameters are all derived from the VhostLDAPURL directive */
    char *url;				/* String representation of LDAP URL */

    char *host;				/* Name of the LDAP server (or space separated list) */
    int port;				/* Port of the LDAP server */
    char *basedn;			/* Base DN to do all searches from */
    int scope;				/* Scope of the search */
    char *filter;			/* Filter to further limit the search  */
    deref_options deref;		/* how to handle alias dereferening */

    char *binddn;			/* DN to bind to server (can be NULL) */
    char *bindpw;			/* Password to bind to server (can be NULL) */

    int have_deref;                     /* Set if we have found an Deref option */
    int have_ldap_url;			/* Set if we have found an LDAP url */

    int secure;				/* True if SSL connections are requested */

    char *fallback;                     /* Fallback virtual host */

} mod_vhost_ldap_config_t;

typedef struct mod_vhost_ldap_request_t {
    char *dn;				/* The saved dn from a successful search */
    char *name;				/* ServerName */
    char *home;				/* HOME */
    char *directory;			/* DocumentRoot relative to HOME/web_scripts */
    char *uid;				/* Suexec Uid */
    char *username;			/* username */
    char *gid;				/* Suexec Gid */
} mod_vhost_ldap_request_t;

char *attributes[] =
  { "scriptsVhostName", "homeDirectory", "scriptsVhostDirectory", "uidNumber", "uid", "gidNumber", 0 };

static int total_modules;

#if (APR_MAJOR_VERSION >= 1)
static APR_OPTIONAL_FN_TYPE(uldap_connection_close) *util_ldap_connection_close;
static APR_OPTIONAL_FN_TYPE(uldap_connection_find) *util_ldap_connection_find;
static APR_OPTIONAL_FN_TYPE(uldap_cache_comparedn) *util_ldap_cache_comparedn;
static APR_OPTIONAL_FN_TYPE(uldap_cache_compare) *util_ldap_cache_compare;
static APR_OPTIONAL_FN_TYPE(uldap_cache_checkuserid) *util_ldap_cache_checkuserid;
static APR_OPTIONAL_FN_TYPE(uldap_cache_getuserdn) *util_ldap_cache_getuserdn;
static APR_OPTIONAL_FN_TYPE(uldap_ssl_supported) *util_ldap_ssl_supported;

static void ImportULDAPOptFn(void)
{
    util_ldap_connection_close  = APR_RETRIEVE_OPTIONAL_FN(uldap_connection_close);
    util_ldap_connection_find   = APR_RETRIEVE_OPTIONAL_FN(uldap_connection_find);
    util_ldap_cache_comparedn   = APR_RETRIEVE_OPTIONAL_FN(uldap_cache_comparedn);
    util_ldap_cache_compare     = APR_RETRIEVE_OPTIONAL_FN(uldap_cache_compare);
    util_ldap_cache_checkuserid = APR_RETRIEVE_OPTIONAL_FN(uldap_cache_checkuserid);
    util_ldap_cache_getuserdn   = APR_RETRIEVE_OPTIONAL_FN(uldap_cache_getuserdn);
    util_ldap_ssl_supported     = APR_RETRIEVE_OPTIONAL_FN(uldap_ssl_supported);
}
#endif 

static int mod_vhost_ldap_post_config(apr_pool_t *p, apr_pool_t *plog, apr_pool_t *ptemp, server_rec *s)
{
    module **m;
    
    /* Stolen from modules/generators/mod_cgid.c */
    total_modules = 0;
    for (m = ap_preloaded_modules; *m != NULL; m++)
      total_modules++;

    /* make sure that mod_ldap (util_ldap) is loaded */
    if (ap_find_linked_module("util_ldap.c") == NULL) {
        ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_NOERRNO, 0, s,
                     "Module mod_ldap missing. Mod_ldap (aka. util_ldap) "
                     "must be loaded in order for mod_vhost_ldap to function properly");
        return HTTP_INTERNAL_SERVER_ERROR;

    }

    ap_add_version_component(p, MOD_VHOST_LDAP_VERSION);

    return OK;
}

static void *
mod_vhost_ldap_create_server_config (apr_pool_t *p, server_rec *s)
{
    mod_vhost_ldap_config_t *conf =
	(mod_vhost_ldap_config_t *)apr_pcalloc(p, sizeof (mod_vhost_ldap_config_t));

    conf->enabled = MVL_UNSET;
    conf->have_ldap_url = 0;
    conf->have_deref = 0;
    conf->binddn = NULL;
    conf->bindpw = NULL;
    conf->deref = always;
    conf->fallback = NULL;

    return conf;
}

static void *
mod_vhost_ldap_merge_server_config(apr_pool_t *p, void *parentv, void *childv)
{
    mod_vhost_ldap_config_t *parent = (mod_vhost_ldap_config_t *) parentv;
    mod_vhost_ldap_config_t *child  = (mod_vhost_ldap_config_t *) childv;
    mod_vhost_ldap_config_t *conf =
	(mod_vhost_ldap_config_t *)apr_pcalloc(p, sizeof(mod_vhost_ldap_config_t));

    if (child->enabled == MVL_UNSET) {
	conf->enabled = parent->enabled;
    } else {
	conf->enabled = child->enabled;
    }

    if (child->have_ldap_url) {
	conf->have_ldap_url = child->have_ldap_url;
	conf->url = child->url;
	conf->host = child->host;
	conf->port = child->port;
	conf->basedn = child->basedn;
	conf->scope = child->scope;
	conf->filter = child->filter;
	conf->secure = child->secure;
    } else {
	conf->have_ldap_url = parent->have_ldap_url;
	conf->url = parent->url;
	conf->host = parent->host;
	conf->port = parent->port;
	conf->basedn = parent->basedn;
	conf->scope = parent->scope;
	conf->filter = parent->filter;
	conf->secure = parent->secure;
    }
    if (child->have_deref) {
	conf->have_deref = child->have_deref;
	conf->deref = child->deref;
    } else {
	conf->have_deref = parent->have_deref;
	conf->deref = parent->deref;
    }

    conf->binddn = (child->binddn ? child->binddn : parent->binddn);
    conf->bindpw = (child->bindpw ? child->bindpw : parent->bindpw);

    conf->fallback = (child->fallback ? child->fallback : parent->fallback);

    return conf;
}

/* 
 * Use the ldap url parsing routines to break up the ldap url into
 * host and port.
 */
static const char *mod_vhost_ldap_parse_url(cmd_parms *cmd, 
					    void *dummy,
					    const char *url)
{
    int result;
    apr_ldap_url_desc_t *urld;
#if (APR_MAJOR_VERSION >= 1)
    apr_ldap_err_t *result_err;
#endif

    mod_vhost_ldap_config_t *conf =
	(mod_vhost_ldap_config_t *)ap_get_module_config(cmd->server->module_config,
							&vhost_ldap_module);

    ap_log_error(APLOG_MARK, APLOG_DEBUG|APLOG_NOERRNO, 0,
	         cmd->server, "[mod_vhost_ldap.c] url parse: `%s'", 
	         url);
    
#if (APR_MAJOR_VERSION >= 1)    /* for apache >= 2.2 */
    result = apr_ldap_url_parse(cmd->pool, url, &(urld), &(result_err));
    if (result != LDAP_SUCCESS) {
        return result_err->reason;
    }
#else
    result = apr_ldap_url_parse(url, &(urld));
    if (result != LDAP_SUCCESS) {
        switch (result) {
            case LDAP_URL_ERR_NOTLDAP:
                return "LDAP URL does not begin with ldap://";
            case LDAP_URL_ERR_NODN:
                return "LDAP URL does not have a DN";
            case LDAP_URL_ERR_BADSCOPE:
                return "LDAP URL has an invalid scope";
            case LDAP_URL_ERR_MEM:
                return "Out of memory parsing LDAP URL";
            default:
                return "Could not parse LDAP URL";
        }
    }
#endif
    conf->url = apr_pstrdup(cmd->pool, url);

    ap_log_error(APLOG_MARK, APLOG_DEBUG|APLOG_NOERRNO, 0,
	         cmd->server, "[mod_vhost_ldap.c] url parse: Host: %s", urld->lud_host);
    ap_log_error(APLOG_MARK, APLOG_DEBUG|APLOG_NOERRNO, 0,
	         cmd->server, "[mod_vhost_ldap.c] url parse: Port: %d", urld->lud_port);
    ap_log_error(APLOG_MARK, APLOG_DEBUG|APLOG_NOERRNO, 0,
	         cmd->server, "[mod_vhost_ldap.c] url parse: DN: %s", urld->lud_dn);
    ap_log_error(APLOG_MARK, APLOG_DEBUG|APLOG_NOERRNO, 0,
	         cmd->server, "[mod_vhost_ldap.c] url parse: attrib: %s", urld->lud_attrs? urld->lud_attrs[0] : "(null)");
    ap_log_error(APLOG_MARK, APLOG_DEBUG|APLOG_NOERRNO, 0,
	         cmd->server, "[mod_vhost_ldap.c] url parse: scope: %s", 
	         (urld->lud_scope == LDAP_SCOPE_SUBTREE? "subtree" : 
		 urld->lud_scope == LDAP_SCOPE_BASE? "base" : 
		 urld->lud_scope == LDAP_SCOPE_ONELEVEL? "onelevel" : "unknown"));
    ap_log_error(APLOG_MARK, APLOG_DEBUG|APLOG_NOERRNO, 0,
	         cmd->server, "[mod_vhost_ldap.c] url parse: filter: %s", urld->lud_filter);

    /* Set all the values, or at least some sane defaults */
    if (conf->host) {
        char *p = apr_palloc(cmd->pool, strlen(conf->host) + strlen(urld->lud_host) + 2);
        strcpy(p, urld->lud_host);
        strcat(p, " ");
        strcat(p, conf->host);
        conf->host = p;
    }
    else {
        conf->host = urld->lud_host? apr_pstrdup(cmd->pool, urld->lud_host) : "localhost";
    }
    conf->basedn = urld->lud_dn? apr_pstrdup(cmd->pool, urld->lud_dn) : "";

    conf->scope = urld->lud_scope == LDAP_SCOPE_ONELEVEL ?
        LDAP_SCOPE_ONELEVEL : LDAP_SCOPE_SUBTREE;

    if (urld->lud_filter) {
        if (urld->lud_filter[0] == '(') {
            /* 
	     * Get rid of the surrounding parens; later on when generating the
	     * filter, they'll be put back.
             */
            conf->filter = apr_pstrdup(cmd->pool, urld->lud_filter+1);
            conf->filter[strlen(conf->filter)-1] = '\0';
        }
        else {
            conf->filter = apr_pstrdup(cmd->pool, urld->lud_filter);
        }
    }
    else {
        conf->filter = "objectClass=scriptsVhost";
    }

      /* "ldaps" indicates secure ldap connections desired
      */
    if (strncasecmp(url, "ldaps", 5) == 0)
    {
        conf->secure = 1;
        conf->port = urld->lud_port? urld->lud_port : LDAPS_PORT;
        ap_log_error(APLOG_MARK, APLOG_DEBUG|APLOG_NOERRNO, 0, cmd->server,
                     "LDAP: vhost_ldap using SSL connections");
    }
    else
    {
        conf->secure = 0;
        conf->port = urld->lud_port? urld->lud_port : LDAP_PORT;
        ap_log_error(APLOG_MARK, APLOG_DEBUG|APLOG_NOERRNO, 0, cmd->server, 
                     "LDAP: vhost_ldap not using SSL connections");
    }

    conf->have_ldap_url = 1;
#if (APR_MAJOR_VERSION < 1) /* free only required for older apr */
    apr_ldap_free_urldesc(urld);
#endif
    return NULL;
}

static const char *mod_vhost_ldap_set_enabled(cmd_parms *cmd, void *dummy, int enabled)
{
    mod_vhost_ldap_config_t *conf =
	(mod_vhost_ldap_config_t *)ap_get_module_config(cmd->server->module_config,
							&vhost_ldap_module);

    conf->enabled = (enabled) ? MVL_ENABLED : MVL_DISABLED;

    return NULL;
}

static const char *mod_vhost_ldap_set_binddn(cmd_parms *cmd, void *dummy, const char *binddn)
{
    mod_vhost_ldap_config_t *conf =
	(mod_vhost_ldap_config_t *)ap_get_module_config(cmd->server->module_config,
							&vhost_ldap_module);

    conf->binddn = apr_pstrdup(cmd->pool, binddn);
    return NULL;
}

static const char *mod_vhost_ldap_set_bindpw(cmd_parms *cmd, void *dummy, const char *bindpw)
{
    mod_vhost_ldap_config_t *conf =
	(mod_vhost_ldap_config_t *)ap_get_module_config(cmd->server->module_config,
							&vhost_ldap_module);

    conf->bindpw = apr_pstrdup(cmd->pool, bindpw);
    return NULL;
}

static const char *mod_vhost_ldap_set_deref(cmd_parms *cmd, void *dummy, const char *deref)
{
    mod_vhost_ldap_config_t *conf = 
	(mod_vhost_ldap_config_t *)ap_get_module_config (cmd->server->module_config,
							 &vhost_ldap_module);

    if (strcmp(deref, "never") == 0 || strcasecmp(deref, "off") == 0) {
        conf->deref = never;
	conf->have_deref = 1;
    }
    else if (strcmp(deref, "searching") == 0) {
        conf->deref = searching;
	conf->have_deref = 1;
    }
    else if (strcmp(deref, "finding") == 0) {
        conf->deref = finding;
	conf->have_deref = 1;
    }
    else if (strcmp(deref, "always") == 0 || strcasecmp(deref, "on") == 0) {
        conf->deref = always;
	conf->have_deref = 1;
    }
    else {
        return "Unrecognized value for VhostLDAPAliasDereference directive";
    }
    return NULL;
}

static const char *mod_vhost_ldap_set_fallback(cmd_parms *cmd, void *dummy, const char *fallback)
{
    mod_vhost_ldap_config_t *conf =
	(mod_vhost_ldap_config_t *)ap_get_module_config(cmd->server->module_config,
							&vhost_ldap_module);

    conf->fallback = apr_pstrdup(cmd->pool, fallback);
    return NULL;
}

static const char *escape(apr_pool_t *p, const char *input)
{
    static const char *const to_escape = "'\\";

    const char *x = input + strcspn(input, to_escape);
    if (*x == '\0')
        return input;
    const char *y = x;
    size_t extra = 0;
    while (*y != '\0') {
        extra++;
        size_t k = strcspn(y + 1, to_escape) + 1;
        y += k;
    }

    char *output = apr_palloc(p, y - input + extra + 1);

    memcpy(output, input, x - input);
    char *z = output + (x - input);
    while (*x != '\0') {
        *z++ = '\\';
        size_t k = strcspn(x + 1, to_escape) + 1;
        memcpy(z, x, k);
        x += k;
        z += k;
    }
    *z = '\0';

    return output;
}

static int reconfigure_directive(apr_pool_t *p,
				 server_rec *s,
				 const char *dir,
				 const char *args)
{
    ap_directive_t dir_s = { .directive = dir, .args = args, .next = NULL,
                             .line_num = 0, .filename = "VhostLDAPConf" };
    return ap_process_config_tree(s, &dir_s, p, p);
}

command_rec mod_vhost_ldap_cmds[] = {
    AP_INIT_TAKE1("VhostLDAPURL", mod_vhost_ldap_parse_url, NULL, RSRC_CONF,
                  "URL to define LDAP connection. This should be an RFC 2255 compliant\n"
                  "URL of the form ldap://host[:port]/basedn[?attrib[?scope[?filter]]].\n"
                  "<ul>\n"
                  "<li>Host is the name of the LDAP server. Use a space separated list of hosts \n"
                  "to specify redundant servers.\n"
                  "<li>Port is optional, and specifies the port to connect to.\n"
                  "<li>basedn specifies the base DN to start searches from\n"
                  "</ul>\n"),

    AP_INIT_TAKE1 ("VhostLDAPBindDN", mod_vhost_ldap_set_binddn, NULL, RSRC_CONF,
		   "DN to use to bind to LDAP server. If not provided, will do an anonymous bind."),
    
    AP_INIT_TAKE1("VhostLDAPBindPassword", mod_vhost_ldap_set_bindpw, NULL, RSRC_CONF,
                  "Password to use to bind to LDAP server. If not provided, will do an anonymous bind."),

    AP_INIT_FLAG("VhostLDAPEnabled", mod_vhost_ldap_set_enabled, NULL, RSRC_CONF,
                 "Set to off to disable vhost_ldap, even if it's been enabled in a higher tree"),

    AP_INIT_TAKE1("VhostLDAPDereferenceAliases", mod_vhost_ldap_set_deref, NULL, RSRC_CONF,
                  "Determines how aliases are handled during a search. Can be one of the"
                  "values \"never\", \"searching\", \"finding\", or \"always\". "
                  "Defaults to always."),

    AP_INIT_TAKE1("VhostLDAPFallback", mod_vhost_ldap_set_fallback, NULL, RSRC_CONF,
		  "Set default virtual host which will be used when requested hostname"
		  "is not found in LDAP database. This option can be used to display"
		  "\"virtual host not found\" type of page."),

    {NULL}
};

#define FILTER_LENGTH MAX_STRING_LEN
static int mod_vhost_ldap_lookup_vhost(conn_rec *conn, const char *host, server_rec **serverp)
{
    apr_pool_t *pool = conn->pool;
    server_rec *server;
    const char *error;
    int code;
    mod_vhost_ldap_request_t *reqc;
    int failures = 0;
    const char **vals = NULL;
    char filtbuf[FILTER_LENGTH];
    mod_vhost_ldap_config_t *conf =
	(mod_vhost_ldap_config_t *)ap_get_module_config((*serverp)->module_config, &vhost_ldap_module);
    util_ldap_connection_t *ldc = NULL;
    int result = 0;
    const char *dn = NULL;
    const char *hostname = NULL;
    int is_fallback = 0;
    int sleep0 = 0;
    int sleep1 = 1;
    int sleep;
    struct berval hostnamebv, shostnamebv;

    // mod_vhost_ldap is disabled or we don't have LDAP Url
    if ((conf->enabled != MVL_ENABLED)||(!conf->have_ldap_url)) {
	return DECLINED;
    }

    if ((error = ap_init_virtual_host(pool, "", *serverp, &server)) != NULL) {
        ap_log_cerror(APLOG_MARK, APLOG_ERR|APLOG_NOERRNO, 0, conn,
		      "[mod_vhost_ldap.c]: Could not initialize a new VirtualHost: %s",
		      error);
	return HTTP_INTERNAL_SERVER_ERROR;
    }

    reqc =
	(mod_vhost_ldap_request_t *)apr_pcalloc(pool, sizeof(mod_vhost_ldap_request_t));
    memset(reqc, 0, sizeof(mod_vhost_ldap_request_t)); 

    struct request_rec *dummy_r = apr_pcalloc(pool, sizeof(request_rec));
    dummy_r->pool = pool;
    dummy_r->connection = conn;
    dummy_r->server = *serverp;
    dummy_r->request_config = ap_create_request_config(pool);
    dummy_r->per_dir_config = (*serverp)->lookup_defaults;
    dummy_r->hostname = host;
    dummy_r->request_time = apr_time_now();
    dummy_r->log = &(*serverp)->log;

start_over:

    if (conf->host) {
        ldc = util_ldap_connection_find(dummy_r, conf->host, conf->port,
					conf->binddn, conf->bindpw, conf->deref,
					conf->secure);
    }
    else {
        ap_log_cerror(APLOG_MARK, APLOG_WARNING|APLOG_NOERRNO, 0, conn,
                      "[mod_vhost_ldap.c] translate: no conf->host - weird...?");
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    hostname = host;
    if (hostname == NULL || hostname[0] == '\0')
        goto null;

fallback:

    ap_log_cerror(APLOG_MARK, APLOG_DEBUG|APLOG_NOERRNO, 0, conn,
		  "[mod_vhost_ldap.c]: translating hostname [%s]",
		  hostname);

    ber_str2bv(hostname, 0, 0, &hostnamebv);
    if (ldap_bv2escaped_filter_value(&hostnamebv, &shostnamebv) != 0)
	goto null;
    apr_snprintf(filtbuf, FILTER_LENGTH, "(&(%s)(|(scriptsVhostName=%s)(scriptsVhostAlias=%s)))", conf->filter, shostnamebv.bv_val, shostnamebv.bv_val);
    ber_memfree(shostnamebv.bv_val);

    result = util_ldap_cache_getuserdn(dummy_r, ldc, conf->url, conf->basedn, conf->scope,
				       attributes, filtbuf, &dn, &vals);

    util_ldap_connection_close(ldc);

    /* sanity check - if server is down, retry it up to 5 times */
    if (AP_LDAP_IS_SERVER_DOWN(result) ||
	(result == LDAP_TIMEOUT) ||
	(result == LDAP_CONNECT_ERROR)) {
        sleep = sleep0 + sleep1;
        ap_log_cerror(APLOG_MARK, APLOG_WARNING|APLOG_NOERRNO, 0, conn,
		      "[mod_vhost_ldap.c]: lookup failure, retry number #[%d], sleeping for [%d] seconds",
		      failures, sleep);
        if (failures++ < MAX_FAILURES) {
	    /* Back-off exponentially */
	    apr_sleep(apr_time_from_sec(sleep));
	    sleep0 = sleep1;
	    sleep1 = sleep;
            goto start_over;
        } else {
	    return HTTP_GATEWAY_TIME_OUT;
	}
    }

    if (result == LDAP_NO_SUCH_OBJECT) {
	if (strcmp(hostname, "*") != 0) {
	    if (strncmp(hostname, "*.", 2) == 0)
		hostname += 2;
	    hostname += strcspn(hostname, ".");
	    hostname = apr_pstrcat(pool, "*", hostname, (const char *)NULL);
	    ap_log_cerror(APLOG_MARK, APLOG_NOTICE|APLOG_NOERRNO, 0, conn,
		          "[mod_vhost_ldap.c] translate: "
			  "virtual host not found, trying wildcard %s",
			  hostname);
	    goto fallback;
	}

null:
	if (conf->fallback && (is_fallback++ <= 0)) {
	    ap_log_cerror(APLOG_MARK, APLOG_NOTICE|APLOG_NOERRNO, 0, conn,
			  "[mod_vhost_ldap.c] translate: "
			  "virtual host %s not found, trying fallback %s",
			  hostname, conf->fallback);
	    hostname = conf->fallback;
	    goto fallback;
	}

	ap_log_cerror(APLOG_MARK, APLOG_WARNING|APLOG_NOERRNO, 0, conn,
		      "[mod_vhost_ldap.c] translate: "
		      "virtual host %s not found",
		      hostname);

	return HTTP_BAD_REQUEST;
    }

    /* handle bind failure */
    if (result != LDAP_SUCCESS) {
        ap_log_cerror(APLOG_MARK, APLOG_WARNING|APLOG_NOERRNO, 0, conn,
                      "[mod_vhost_ldap.c] translate: "
                      "translate failed; virtual host %s [%s]",
		      hostname, ldap_err2string(result));
	return HTTP_INTERNAL_SERVER_ERROR;
    }

    /* mark the user and DN */
    reqc->dn = apr_pstrdup(pool, dn);

    /* Optimize */
    if (vals) {
	int i;
	for (i = 0; attributes[i]; i++) {

	    char *val = apr_pstrdup (pool, vals[i]);
	    /* These do not correspond to any real directives */
	    if (strcasecmp (attributes[i], "uidNumber") == 0) {
		reqc->uid = val;
		continue;
	    }
	    else if (strcasecmp (attributes[i], "uid") == 0) {
		reqc->username = val;
		continue;
	    }
	    else if (strcasecmp (attributes[i], "gidNumber") == 0) {
		reqc->gid = val;
		continue;
	    }
	    else if (strcasecmp (attributes[i], "homeDirectory") == 0) {
		reqc->home = val;
		continue;
	    }
	    else if (strcasecmp (attributes[i], "scriptsVhostDirectory") == 0) {
		reqc->directory = val;
		continue;
	    }
	    else if (strcasecmp (attributes[i], "scriptsVhostName") == 0) {
		reqc->name = val;
		continue;
	    }
	    else {
		/* This should not actually be reachable, but it's
		   good to cover all all possible cases */
                ap_log_cerror(APLOG_MARK, APLOG_DEBUG|APLOG_NOERRNO, 0, conn,
                              "Unexpected attribute %s encountered", attributes[i]);
                continue;
            }
	}
    }

    ap_log_cerror(APLOG_MARK, APLOG_DEBUG|APLOG_NOERRNO, 0, conn,
		  "[mod_vhost_ldap.c]: loaded from ldap: "
		  "scriptsVhostName: %s, "
		  "homeDirectory: %s, "
		  "scriptsVhostDirectory: %s, "
		  "uidNumber: %s, "
		  "uid: %s, "
		  "gidNumber: %s",
		  reqc->name, reqc->home, reqc->directory, reqc->uid, reqc->username, reqc->gid);

    if (reqc->name == NULL || reqc->home == NULL || reqc->directory == NULL) {
        ap_log_cerror(APLOG_MARK, APLOG_ERR|APLOG_NOERRNO, 0, conn,
                      "[mod_vhost_ldap.c] translate: "
                      "translate failed; ServerName or DocumentRoot not defined");
	return HTTP_INTERNAL_SERVER_ERROR;
    }

    if ((code = reconfigure_directive(
             pool, server, "ServerName",
             apr_pstrcat(pool, "'", escape(pool, reqc->name), "'", (const char *)NULL))) != 0)
	return code;

    char *docroot =
	strcmp(reqc->directory, ".") == 0 ?
	apr_pstrcat(pool, reqc->home, "/web_scripts", (const char *)NULL) :
	apr_pstrcat(pool, reqc->home, "/web_scripts/", reqc->directory, (const char *)NULL);
    if ((code = reconfigure_directive(
             pool, server, "DocumentRoot",
             apr_pstrcat(pool, "'", escape(pool, docroot), "'", (const char *)NULL))) != 0)
	return code;

    if (reqc->uid != NULL) {
	char *userdir_val;

        if (reqc->gid == NULL) {
            ap_log_cerror(APLOG_MARK, APLOG_ERR|APLOG_NOERRNO, 0, conn,
                          "could not get gid for uid %s", reqc->uid);
            return HTTP_INTERNAL_SERVER_ERROR;
        }

        if ((code = reconfigure_directive(
                 pool, server, "SuexecUserGroup",
                 apr_pstrcat(
                     pool, "'#", escape(pool, reqc->uid), "' '#",
                     escape(pool, reqc->gid), "'", (const char *)NULL))) != 0)
            return code;

	if ((code = reconfigure_directive(
                 pool, server, "UserDir",
                 apr_pstrcat(pool, "'", escape(pool, USERDIR), "'", (const char *)NULL))) != 0)
	    return code;

        /* Deal with ~ expansion */
        if ((code = reconfigure_directive(pool, server, "UserDir", "disabled")) != 0)
            return code;

	if (reqc->username == NULL) {
	    ap_log_cerror(APLOG_MARK, APLOG_ERR|APLOG_NOERRNO, 0, conn,
		          "could not get username for uid %s", reqc->uid);
	    return HTTP_INTERNAL_SERVER_ERROR;
	}

        userdir_val = apr_pstrcat(pool, "enabled '", escape(pool, reqc->username), "'", (const char *)NULL);

	if ((code = reconfigure_directive(pool, server, "UserDir", userdir_val)) != 0)
	    return code;
    }

    if ((code = reconfigure_directive(pool, server, "VhostLDAPEnabled", "off")) != 0)
        return code;

    ap_fixup_virtual_host(pool, *serverp, server);
    *serverp = server;

    return OK;
}

static void
mod_vhost_ldap_register_hooks (apr_pool_t * p)
{
    ap_hook_post_config(mod_vhost_ldap_post_config, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_lookup_vhost(mod_vhost_ldap_lookup_vhost, NULL, NULL, APR_HOOK_MIDDLE);
#if (APR_MAJOR_VERSION >= 1)
    ap_hook_optional_fn_retrieve(ImportULDAPOptFn,NULL,NULL,APR_HOOK_MIDDLE);
#endif
}

module AP_MODULE_DECLARE_DATA vhost_ldap_module = {
  STANDARD20_MODULE_STUFF,
  NULL,
  NULL,
  mod_vhost_ldap_create_server_config,
  mod_vhost_ldap_merge_server_config,
  mod_vhost_ldap_cmds,
  mod_vhost_ldap_register_hooks,
};
