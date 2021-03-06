/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ap_provider.h"
#include "httpd.h"
#include "http_config.h"
#include "ap_provider.h"
#include "http_core.h"
#include "http_log.h"
#include "http_protocol.h"
#include "http_request.h"
#include "util_ldap.h"

#include "mod_auth.h"

#include "apr_strings.h"
#include "apr_xlate.h"
#define APR_WANT_STRFUNC
#include "apr_want.h"
#include "apr_lib.h"

#if APR_HAVE_UNISTD_H
/* for getpid() */
#include <unistd.h>
#endif
#include <ctype.h>

#if !APR_HAS_LDAP
#error mod_authnz_ldap requires APR-util to have LDAP support built in. To fix add --with-ldap to ./configure.
#endif

static char *default_attributes[3] = { "member", "uniqueMember", NULL };

typedef struct {
    apr_pool_t *pool;               /* Pool that this config is allocated from */
#if APR_HAS_THREADS
    apr_thread_mutex_t *lock;       /* Lock for this config */
#endif

    /* These parameters are all derived from the AuthLDAPURL directive */
    char *url;                      /* String representation of the URL */

    char *host;                     /* Name of the LDAP server (or space separated list) */
    int port;                       /* Port of the LDAP server */
    char *basedn;                   /* Base DN to do all searches from */
    char *attribute;                /* Attribute to search for */
    char **attributes;              /* Array of all the attributes to return */
    int scope;                      /* Scope of the search */
    char *filter;                   /* Filter to further limit the search  */
    deref_options deref;            /* how to handle alias dereferening */
    char *binddn;                   /* DN to bind to server (can be NULL) */
    char *bindpw;                   /* Password to bind to server (can be NULL) */
    int bind_authoritative;         /* If true, will return errors when bind fails */

    int user_is_dn;                 /* If true, connection->user is DN instead of userid */
    char *remote_user_attribute;    /* If set, connection->user is this attribute instead of userid */
    int compare_dn_on_server;       /* If true, will use server to do DN compare */

    int have_ldap_url;              /* Set if we have found an LDAP url */

    apr_array_header_t *groupattr;  /* List of Group attributes identifying user members. Default:"member uniqueMember" */
    int group_attrib_is_dn;         /* If true, the group attribute is the DN, otherwise,
                                        it's the exact string passed by the HTTP client */
    char **sgAttributes;            /* Array of strings constructed (post-config) from subgroupattrs. Last entry is NULL. */
    apr_array_header_t *subgroupclasses; /* List of object classes of sub-groups. Default:"groupOfNames groupOfUniqueNames" */
    int maxNestingDepth;            /* Maximum recursive nesting depth permitted during subgroup processing. Default: 10 */

    int secure;                     /* True if SSL connections are requested */
} authn_ldap_config_t;

typedef struct {
    char *dn;                       /* The saved dn from a successful search */
    char *user;                     /* The username provided by the client */
} authn_ldap_request_t;

/* maximum group elements supported */
#define GROUPATTR_MAX_ELTS 10

module AP_MODULE_DECLARE_DATA authnz_ldap_module;

static APR_OPTIONAL_FN_TYPE(uldap_connection_close) *util_ldap_connection_close;
static APR_OPTIONAL_FN_TYPE(uldap_connection_find) *util_ldap_connection_find;
static APR_OPTIONAL_FN_TYPE(uldap_cache_comparedn) *util_ldap_cache_comparedn;
static APR_OPTIONAL_FN_TYPE(uldap_cache_compare) *util_ldap_cache_compare;
static APR_OPTIONAL_FN_TYPE(uldap_cache_check_subgroups) *util_ldap_cache_check_subgroups;
static APR_OPTIONAL_FN_TYPE(uldap_cache_checkuserid) *util_ldap_cache_checkuserid;
static APR_OPTIONAL_FN_TYPE(uldap_cache_getuserdn) *util_ldap_cache_getuserdn;
static APR_OPTIONAL_FN_TYPE(uldap_ssl_supported) *util_ldap_ssl_supported;

static apr_hash_t *charset_conversions = NULL;
static char *to_charset = NULL;           /* UTF-8 identifier derived from the charset.conv file */


/* Derive a code page ID give a language name or ID */
static char* derive_codepage_from_lang (apr_pool_t *p, char *language)
{
    int lang_len;
    char *charset;

    if (!language)          /* our default codepage */
        return apr_pstrdup(p, "ISO-8859-1");
    else
        lang_len = strlen(language);

    charset = (char*) apr_hash_get(charset_conversions, language, APR_HASH_KEY_STRING);

    if (!charset) {
        language[2] = '\0';
        charset = (char*) apr_hash_get(charset_conversions, language, APR_HASH_KEY_STRING);
    }

    if (charset) {
        charset = apr_pstrdup(p, charset);
    }

    return charset;
}

static apr_xlate_t* get_conv_set (request_rec *r)
{
    char *lang_line = (char*)apr_table_get(r->headers_in, "accept-language");
    char *lang;
    apr_xlate_t *convset;

    if (lang_line) {
        lang_line = apr_pstrdup(r->pool, lang_line);
        for (lang = lang_line;*lang;lang++) {
            if ((*lang == ',') || (*lang == ';')) {
                *lang = '\0';
                break;
            }
        }
        lang = derive_codepage_from_lang(r->pool, lang_line);

        if (lang && (apr_xlate_open(&convset, to_charset, lang, r->pool) == APR_SUCCESS)) {
            return convset;
        }
    }

    return NULL;
}


static const char* authn_ldap_xlate_password(request_rec *r,
                                             const char* sent_password)
{
    apr_xlate_t *convset = NULL;
    apr_size_t inbytes;
    apr_size_t outbytes;
    char *outbuf;

    if (charset_conversions && (convset = get_conv_set(r)) ) {
        inbytes = strlen(sent_password);
        outbytes = (inbytes+1)*3;
        outbuf = apr_pcalloc(r->pool, outbytes);

        /* Convert the password to UTF-8. */
        if (apr_xlate_conv_buffer(convset, sent_password, &inbytes, outbuf,
                                  &outbytes) == APR_SUCCESS)
            return outbuf;
    }

    return sent_password;
}


/*
 * Build the search filter, or at least as much of the search filter that
 * will fit in the buffer. We don't worry about the buffer not being able
 * to hold the entire filter. If the buffer wasn't big enough to hold the
 * filter, ldap_search_s will complain, but the only situation where this
 * is likely to happen is if the client sent a really, really long
 * username, most likely as part of an attack.
 *
 * The search filter consists of the filter provided with the URL,
 * combined with a filter made up of the attribute provided with the URL,
 * and the actual username passed by the HTTP client. For example, assume
 * that the LDAP URL is
 *
 *   ldap://ldap.airius.com/ou=People, o=Airius?uid??(posixid=*)
 *
 * Further, assume that the userid passed by the client was `userj'.  The
 * search filter will be (&(posixid=*)(uid=userj)).
 */
#define FILTER_LENGTH MAX_STRING_LEN
static void authn_ldap_build_filter(char *filtbuf,
                             request_rec *r,
                             const char* sent_user,
                             const char* sent_filter,
                             authn_ldap_config_t *sec)
{
    char *p, *q, *filtbuf_end;
    char *user, *filter;
    apr_xlate_t *convset = NULL;
    apr_size_t inbytes;
    apr_size_t outbytes;
    char *outbuf;

    if (sent_user != NULL) {
        user = apr_pstrdup (r->pool, sent_user);
    }
    else
        return;

    if (sent_filter != NULL) {
        filter = apr_pstrdup (r->pool, sent_filter);
    }
    else
        filter = sec->filter;

    if (charset_conversions) {
        convset = get_conv_set(r);
    }

    if (convset) {
        inbytes = strlen(user);
        outbytes = (inbytes+1)*3;
        outbuf = apr_pcalloc(r->pool, outbytes);

        /* Convert the user name to UTF-8.  This is only valid for LDAP v3 */
        if (apr_xlate_conv_buffer(convset, user, &inbytes, outbuf, &outbytes) == APR_SUCCESS) {
            user = apr_pstrdup(r->pool, outbuf);
        }
    }

    /*
     * Create the first part of the filter, which consists of the
     * config-supplied portions.
     */
    apr_snprintf(filtbuf, FILTER_LENGTH, "(&(%s)(%s=", filter, sec->attribute);

    /*
     * Now add the client-supplied username to the filter, ensuring that any
     * LDAP filter metachars are escaped.
     */
    filtbuf_end = filtbuf + FILTER_LENGTH - 1;
#if APR_HAS_MICROSOFT_LDAPSDK
    for (p = user, q=filtbuf + strlen(filtbuf);
         *p && q < filtbuf_end; ) {
        if (strchr("*()\\", *p) != NULL) {
            if ( q + 3 >= filtbuf_end)
              break;  /* Don't write part of escape sequence if we can't write all of it */
            *q++ = '\\';
            switch ( *p++ )
            {
              case '*':
                *q++ = '2';
                *q++ = 'a';
                break;
              case '(':
                *q++ = '2';
                *q++ = '8';
                break;
              case ')':
                *q++ = '2';
                *q++ = '9';
                break;
              case '\\':
                *q++ = '5';
                *q++ = 'c';
                break;
                        }
        }
        else
            *q++ = *p++;
    }
#else
    for (p = user, q=filtbuf + strlen(filtbuf);
         *p && q < filtbuf_end; *q++ = *p++) {
        if (strchr("*()\\", *p) != NULL) {
            *q++ = '\\';
            if (q >= filtbuf_end) {
              break;
            }
        }
    }
#endif
    *q = '\0';

    /*
     * Append the closing parens of the filter, unless doing so would
     * overrun the buffer.
     */
    if (q + 2 <= filtbuf_end)
        strcat(filtbuf, "))");
}

static void *create_authnz_ldap_dir_config(apr_pool_t *p, char *d)
{
    authn_ldap_config_t *sec =
        (authn_ldap_config_t *)apr_pcalloc(p, sizeof(authn_ldap_config_t));

    sec->pool = p;
#if APR_HAS_THREADS
    apr_thread_mutex_create(&sec->lock, APR_THREAD_MUTEX_DEFAULT, p);
#endif
/*
    sec->authz_enabled = 1;
*/
    sec->groupattr = apr_array_make(p, GROUPATTR_MAX_ELTS,
                                    sizeof(struct mod_auth_ldap_groupattr_entry_t));
    sec->subgroupclasses = apr_array_make(p, GROUPATTR_MAX_ELTS,
                                    sizeof(struct mod_auth_ldap_groupattr_entry_t));

    sec->have_ldap_url = 0;
    sec->url = "";
    sec->host = NULL;
    sec->binddn = NULL;
    sec->bindpw = NULL;
    sec->bind_authoritative = 1;
    sec->deref = always;
    sec->group_attrib_is_dn = 1;
    sec->secure = -1;   /*Initialize to unset*/
    sec->maxNestingDepth = 10;
    sec->sgAttributes = apr_pcalloc(p, sizeof (char *) * GROUPATTR_MAX_ELTS + 1);

    sec->user_is_dn = 0;
    sec->remote_user_attribute = NULL;
    sec->compare_dn_on_server = 0;

    return sec;
}

static apr_status_t authnz_ldap_cleanup_connection_close(void *param)
{
    util_ldap_connection_t *ldc = param;
    util_ldap_connection_close(ldc);
    return APR_SUCCESS;
}


/*
 * Authentication Phase
 * --------------------
 *
 * This phase authenticates the credentials the user has sent with
 * the request (ie the username and password are checked). This is done
 * by making an attempt to bind to the LDAP server using this user's
 * DN and the supplied password.
 *
 */
static authn_status authn_ldap_check_password(request_rec *r, const char *user,
                                              const char *password)
{
    int failures = 0;
    const char **vals = NULL;
    char filtbuf[FILTER_LENGTH];
    authn_ldap_config_t *sec =
        (authn_ldap_config_t *)ap_get_module_config(r->per_dir_config, &authnz_ldap_module);

    util_ldap_connection_t *ldc = NULL;
    int result = 0;
    int remote_user_attribute_set = 0;
    const char *dn = NULL;
    const char *utfpassword;

    authn_ldap_request_t *req =
        (authn_ldap_request_t *)apr_pcalloc(r->pool, sizeof(authn_ldap_request_t));
    ap_set_module_config(r->request_config, &authnz_ldap_module, req);

/*
    if (!sec->enabled) {
        return AUTH_USER_NOT_FOUND;
    }
*/

    /*
     * Basic sanity checks before any LDAP operations even happen.
     */
    if (!sec->have_ldap_url) {
        return AUTH_GENERAL_ERROR;
    }

start_over:

    /* There is a good AuthLDAPURL, right? */
    if (sec->host) {
        ldc = util_ldap_connection_find(r, sec->host, sec->port,
                                       sec->binddn, sec->bindpw, sec->deref,
                                       sec->secure);
    }
    else {
        ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r,
                      "[%" APR_PID_T_FMT "] auth_ldap authenticate: no sec->host - weird...?", getpid());
        return AUTH_GENERAL_ERROR;
    }

    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                  "[%" APR_PID_T_FMT "] auth_ldap authenticate: using URL %s", getpid(), sec->url);

    /* Get the password that the client sent */
    if (password == NULL) {
        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                      "[%" APR_PID_T_FMT "] auth_ldap authenticate: no password specified", getpid());
        util_ldap_connection_close(ldc);
        return AUTH_GENERAL_ERROR;
    }

    if (user == NULL) {
        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                      "[%" APR_PID_T_FMT "] auth_ldap authenticate: no user specified", getpid());
        util_ldap_connection_close(ldc);
        return AUTH_GENERAL_ERROR;
    }

    /* build the username filter */
    authn_ldap_build_filter(filtbuf, r, user, NULL, sec);

    /* convert password to utf-8 */
    utfpassword = authn_ldap_xlate_password(r, password);

    /* do the user search */
    result = util_ldap_cache_checkuserid(r, ldc, sec->url, sec->basedn, sec->scope,
                                         sec->attributes, filtbuf, utfpassword,
                                         &dn, &vals);
    util_ldap_connection_close(ldc);

    /* sanity check - if server is down, retry it up to 5 times */
    if (AP_LDAP_IS_SERVER_DOWN(result)) {
        if (failures++ <= 5) {
            goto start_over;
        }
    }

    /* handle bind failure */
    if (result != LDAP_SUCCESS) {
        if (!sec->bind_authoritative) {
           ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                      "[%" APR_PID_T_FMT "] auth_ldap authenticate: "
                      "user %s authentication failed; URI %s [%s][%s] (not authoritative)",
                      getpid(), user, r->uri, ldc->reason, ldap_err2string(result));
           return AUTH_USER_NOT_FOUND;
        }

        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
                      "[%" APR_PID_T_FMT "] auth_ldap authenticate: "
                      "user %s authentication failed; URI %s [%s][%s]",
                      getpid(), user, r->uri, ldc->reason, ldap_err2string(result));

        return (LDAP_NO_SUCH_OBJECT == result) ? AUTH_USER_NOT_FOUND
#ifdef LDAP_SECURITY_ERROR
                 : (LDAP_SECURITY_ERROR(result)) ? AUTH_DENIED
#else
                 : (LDAP_INAPPROPRIATE_AUTH == result) ? AUTH_DENIED
                 : (LDAP_INVALID_CREDENTIALS == result) ? AUTH_DENIED
#ifdef LDAP_INSUFFICIENT_ACCESS
                 : (LDAP_INSUFFICIENT_ACCESS == result) ? AUTH_DENIED
#endif
#ifdef LDAP_INSUFFICIENT_RIGHTS
                 : (LDAP_INSUFFICIENT_RIGHTS == result) ? AUTH_DENIED
#endif
#endif
                 : AUTH_GENERAL_ERROR;
    }

    /* mark the user and DN */
    req->dn = apr_pstrdup(r->pool, dn);
    req->user = apr_pstrdup(r->pool, user);
    if (sec->user_is_dn) {
        r->user = req->dn;
    }

    /* add environment variables */
    if (sec->attributes && vals) {
        apr_table_t *e = r->subprocess_env;
        int i = 0;
        while (sec->attributes[i]) {
            char *str = apr_pstrcat(r->pool, AUTHN_PREFIX, sec->attributes[i], NULL);
            int j = sizeof(AUTHN_PREFIX)-1; /* string length of "AUTHENTICATE_", excluding the trailing NIL */
            while (str[j]) {
                str[j] = apr_toupper(str[j]);
                j++;
            }
            apr_table_setn(e, str, vals[i] ? vals[i] : "");

            /* handle remote_user_attribute, if set */
            if (sec->remote_user_attribute && 
                !strcmp(sec->remote_user_attribute, sec->attributes[i])) {
                r->user = (char *)apr_pstrdup(r->pool, vals[i]);
                remote_user_attribute_set = 1;
            }
            i++;
        }
    }

    /* sanity check */
    if (sec->remote_user_attribute && !remote_user_attribute_set) {
        ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r,
                  "[%" APR_PID_T_FMT "] auth_ldap authenticate: "
                  "REMOTE_USER was to be set with attribute '%s', "
                  "but this attribute was not requested for in the "
                  "LDAP query for the user. REMOTE_USER will fall "
                  "back to username or DN as appropriate.", getpid(),
                  sec->remote_user_attribute);
    }

    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                  "[%" APR_PID_T_FMT "] auth_ldap authenticate: accepting %s", getpid(), user);

    return AUTH_GRANTED;
}

static authz_status ldapuser_check_authorization(request_rec *r,
                                             const char *require_args)
{
    int result = 0;
    authn_ldap_request_t *req =
        (authn_ldap_request_t *)ap_get_module_config(r->request_config, &authnz_ldap_module);
    authn_ldap_config_t *sec =
        (authn_ldap_config_t *)ap_get_module_config(r->per_dir_config, &authnz_ldap_module);

    util_ldap_connection_t *ldc = NULL;

    const char *t;
    char *w;

    char filtbuf[FILTER_LENGTH];
    const char *dn = NULL;
    const char **vals = NULL;

    if (!sec->have_ldap_url) {
        return AUTHZ_DENIED;
    }

    if (sec->host) {
        ldc = util_ldap_connection_find(r, sec->host, sec->port,
                                       sec->binddn, sec->bindpw, sec->deref,
                                       sec->secure);
        apr_pool_cleanup_register(r->pool, ldc,
                                  authnz_ldap_cleanup_connection_close,
                                  apr_pool_cleanup_null);
    }
    else {
        ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r,
                      "[%" APR_PID_T_FMT "] auth_ldap authorize: no sec->host - weird...?", getpid());
        return AUTHZ_DENIED;
    }

    /*
     * If we have been authenticated by some other module than mod_authnz_ldap,
     * the req structure needed for authorization needs to be created
     * and populated with the userid and DN of the account in LDAP
     */

    /* Check that we have a userid to start with */
    if (!r->user) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
            "access to %s failed, reason: no authenticated user", r->uri);
        return AUTHZ_DENIED;
    }

    if (!strlen(r->user)) {
        ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r,
            "ldap authorize: Userid is blank, AuthType=%s",
            r->ap_auth_type);
    }

    if(!req) {
        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
            "ldap authorize: Creating LDAP req structure");

        /* Build the username filter */
        authn_ldap_build_filter(filtbuf, r, r->user, NULL, sec);

        /* Search for the user DN */
        result = util_ldap_cache_getuserdn(r, ldc, sec->url, sec->basedn,
             sec->scope, sec->attributes, filtbuf, &dn, &vals);

        /* Search failed, log error and return failure */
        if(result != LDAP_SUCCESS) {
            ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                "auth_ldap authorise: User DN not found, %s", ldc->reason);
            return AUTHZ_DENIED;
        }

        req = (authn_ldap_request_t *)apr_pcalloc(r->pool,
            sizeof(authn_ldap_request_t));
        ap_set_module_config(r->request_config, &authnz_ldap_module, req);
        req->dn = apr_pstrdup(r->pool, dn);
        req->user = r->user;
    }

    if (req->dn == NULL || strlen(req->dn) == 0) {
        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                      "[%" APR_PID_T_FMT "] auth_ldap authorize: "
                      "require user: user's DN has not been defined; failing authorization",
                      getpid());
        return AUTHZ_DENIED;
    }

    /*
     * First do a whole-line compare, in case it's something like
     *   require user Babs Jensen
     */
    result = util_ldap_cache_compare(r, ldc, sec->url, req->dn, sec->attribute, require_args);
    switch(result) {
        case LDAP_COMPARE_TRUE: {
            ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                          "[%" APR_PID_T_FMT "] auth_ldap authorize: "
                          "require user: authorization successful", getpid());
            return AUTHZ_GRANTED;
        }
        default: {
            ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                          "[%" APR_PID_T_FMT "] auth_ldap authorize: require user: "
                          "authorization failed [%s][%s]", getpid(),
                          ldc->reason, ldap_err2string(result));
        }
    }

    /*
     * Now break apart the line and compare each word on it
     */
    t = require_args;
    while ((w = ap_getword_conf(r->pool, &t)) && w[0]) {
        result = util_ldap_cache_compare(r, ldc, sec->url, req->dn, sec->attribute, w);
        switch(result) {
            case LDAP_COMPARE_TRUE: {
                ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                              "[%" APR_PID_T_FMT "] auth_ldap authorize: "
                              "require user: authorization successful", getpid());
                return AUTHZ_GRANTED;
            }
            default: {
                ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                              "[%" APR_PID_T_FMT "] auth_ldap authorize: "
                              "require user: authorization failed [%s][%s]",
                              getpid(), ldc->reason, ldap_err2string(result));
            }
        }
    }

    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                  "[%" APR_PID_T_FMT "] auth_ldap authorize user: authorization denied for user %s to %s",
                  getpid(), r->user, r->uri);

    return AUTHZ_DENIED;
}

static authz_status ldapgroup_check_authorization(request_rec *r,
                                             const char *require_args)
{
    int result = 0;
    authn_ldap_request_t *req =
        (authn_ldap_request_t *)ap_get_module_config(r->request_config, &authnz_ldap_module);
    authn_ldap_config_t *sec =
        (authn_ldap_config_t *)ap_get_module_config(r->per_dir_config, &authnz_ldap_module);

    util_ldap_connection_t *ldc = NULL;

    const char *t;

    char filtbuf[FILTER_LENGTH];
    const char *dn = NULL;
    const char **vals = NULL;
    struct mod_auth_ldap_groupattr_entry_t *ent;
    int i;

    if (!sec->have_ldap_url) {
        return AUTHZ_DENIED;
    }

    if (sec->host) {
        ldc = util_ldap_connection_find(r, sec->host, sec->port,
                                       sec->binddn, sec->bindpw, sec->deref,
                                       sec->secure);
        apr_pool_cleanup_register(r->pool, ldc,
                                  authnz_ldap_cleanup_connection_close,
                                  apr_pool_cleanup_null);
    }
    else {
        ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r,
                      "[%" APR_PID_T_FMT "] auth_ldap authorize: no sec->host - weird...?", getpid());
        return AUTHZ_DENIED;
    }

    /*
     * If there are no elements in the group attribute array, the default should be
     * member and uniquemember; populate the array now.
     */
    if (sec->groupattr->nelts == 0) {
        struct mod_auth_ldap_groupattr_entry_t *grp;
#if APR_HAS_THREADS
        apr_thread_mutex_lock(sec->lock);
#endif
        grp = apr_array_push(sec->groupattr);
        grp->name = "member";
        grp = apr_array_push(sec->groupattr);
        grp->name = "uniqueMember";
#if APR_HAS_THREADS
        apr_thread_mutex_unlock(sec->lock);
#endif
    }

    /*
     * If there are no elements in the sub group classes array, the default
     * should be groupOfNames and groupOfUniqueNames; populate the array now.
     */
    if (sec->subgroupclasses->nelts == 0) {
        struct mod_auth_ldap_groupattr_entry_t *grp;
#if APR_HAS_THREADS
        apr_thread_mutex_lock(sec->lock);
#endif
        grp = apr_array_push(sec->subgroupclasses);
        grp->name = "groupOfNames";
        grp = apr_array_push(sec->subgroupclasses);
        grp->name = "groupOfUniqueNames";
#if APR_HAS_THREADS
        apr_thread_mutex_unlock(sec->lock);
#endif
    }

    /*
     * If we have been authenticated by some other module than mod_auth_ldap,
     * the req structure needed for authorization needs to be created
     * and populated with the userid and DN of the account in LDAP
     */

    /* Check that we have a userid to start with */
    if (!r->user) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
            "access to %s failed, reason: no authenticated user", r->uri);
        return AUTHZ_DENIED;
    }

    if (!strlen(r->user)) {
        ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r,
            "ldap authorize: Userid is blank, AuthType=%s",
            r->ap_auth_type);
    }

    if(!req) {
        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
            "ldap authorize: Creating LDAP req structure");

        /* Build the username filter */
        authn_ldap_build_filter(filtbuf, r, r->user, NULL, sec);

        /* Search for the user DN */
        result = util_ldap_cache_getuserdn(r, ldc, sec->url, sec->basedn,
             sec->scope, sec->attributes, filtbuf, &dn, &vals);

        /* Search failed, log error and return failure */
        if(result != LDAP_SUCCESS) {
            ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                "auth_ldap authorise: User DN not found, %s", ldc->reason);
            return AUTHZ_DENIED;
        }

        req = (authn_ldap_request_t *)apr_pcalloc(r->pool,
            sizeof(authn_ldap_request_t));
        ap_set_module_config(r->request_config, &authnz_ldap_module, req);
        req->dn = apr_pstrdup(r->pool, dn);
        req->user = r->user;
    }

    ent = (struct mod_auth_ldap_groupattr_entry_t *) sec->groupattr->elts;

    if (sec->group_attrib_is_dn) {
        if (req->dn == NULL || strlen(req->dn) == 0) {
            ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                          "[%" APR_PID_T_FMT "] auth_ldap authorize: require group: "
                          "user's DN has not been defined; failing authorization for user %s",
                          getpid(), r->user);
            return AUTHZ_DENIED;
        }
    }
    else {
        if (req->user == NULL || strlen(req->user) == 0) {
            /* We weren't called in the authentication phase, so we didn't have a
             * chance to set the user field. Do so now. */
            req->user = r->user;
        }
    }

    t = require_args;

    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                  "[%" APR_PID_T_FMT "] auth_ldap authorize: require group: "
                  "testing for group membership in \"%s\"",
                  getpid(), t);

    for (i = 0; i < sec->groupattr->nelts; i++) {
        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                      "[%" APR_PID_T_FMT "] auth_ldap authorize: require group: "
                      "testing for %s: %s (%s)", getpid(),
                      ent[i].name, sec->group_attrib_is_dn ? req->dn : req->user, t);

        result = util_ldap_cache_compare(r, ldc, sec->url, t, ent[i].name,
                             sec->group_attrib_is_dn ? req->dn : req->user);
        switch(result) {
            case LDAP_COMPARE_TRUE: {
                ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                              "[%" APR_PID_T_FMT "] auth_ldap authorize: require group: "
                              "authorization successful (attribute %s) [%s][%d - %s]",
                              getpid(), ent[i].name, ldc->reason, result, ldap_err2string(result));
                return AUTHZ_GRANTED;
            }
            case LDAP_COMPARE_FALSE: {
                ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                               "[%" APR_PID_T_FMT "] auth_ldap authorise: require group \"%s\": "
                               "failed [%s][%d - %s], checking sub-groups",
                               getpid(), t, ldc->reason, result, ldap_err2string(result));

                result = util_ldap_cache_check_subgroups(r, ldc, sec->url, t, ent[i].name,
                                                         sec->group_attrib_is_dn ? req->dn : req->user,
                                                         sec->sgAttributes[0] ? sec->sgAttributes : default_attributes,
                                                         sec->subgroupclasses,
                                                         0, sec->maxNestingDepth);
                if(result == LDAP_COMPARE_TRUE) {
                    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                                   "[%" APR_PID_T_FMT "] auth_ldap authorise: require group (sub-group): "
                                   "authorisation successful (attribute %s) [%s][%d - %s]",
                                   getpid(), ent[i].name, ldc->reason, result, ldap_err2string(result));
                     return AUTHZ_GRANTED;
                }
                else {
                    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                                   "[%" APR_PID_T_FMT "] auth_ldap authorise: require group (sub-group) \"%s\": "
                                   "authorisation failed [%s][%d - %s]",
                                   getpid(), t, ldc->reason, result, ldap_err2string(result));
                }
                break;
            }
            default: {
                ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                              "[%" APR_PID_T_FMT "] auth_ldap authorize: require group \"%s\": "
                              "authorization failed [%s][%d - %s]",
                              getpid(), t, ldc->reason, result, ldap_err2string(result));
            }
        }
    }

    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                  "[%" APR_PID_T_FMT "] auth_ldap authorize group: authorization denied for user %s to %s",
                  getpid(), r->user, r->uri);

    return AUTHZ_DENIED;
}

static authz_status ldapdn_check_authorization(request_rec *r,
                                             const char *require_args)
{
    int result = 0;
    authn_ldap_request_t *req =
        (authn_ldap_request_t *)ap_get_module_config(r->request_config, &authnz_ldap_module);
    authn_ldap_config_t *sec =
        (authn_ldap_config_t *)ap_get_module_config(r->per_dir_config, &authnz_ldap_module);

    util_ldap_connection_t *ldc = NULL;

    const char *t;

    char filtbuf[FILTER_LENGTH];
    const char *dn = NULL;
    const char **vals = NULL;

    if (!sec->have_ldap_url) {
        return AUTHZ_DENIED;
    }

    if (sec->host) {
        ldc = util_ldap_connection_find(r, sec->host, sec->port,
                                       sec->binddn, sec->bindpw, sec->deref,
                                       sec->secure);
        apr_pool_cleanup_register(r->pool, ldc,
                                  authnz_ldap_cleanup_connection_close,
                                  apr_pool_cleanup_null);
    }
    else {
        ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r,
                      "[%" APR_PID_T_FMT "] auth_ldap authorize: no sec->host - weird...?", getpid());
        return AUTHZ_DENIED;
    }

    /*
     * If we have been authenticated by some other module than mod_auth_ldap,
     * the req structure needed for authorization needs to be created
     * and populated with the userid and DN of the account in LDAP
     */

    /* Check that we have a userid to start with */
    if (!r->user) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
            "access to %s failed, reason: no authenticated user", r->uri);
        return AUTHZ_DENIED;
    }

    if (!strlen(r->user)) {
        ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r,
            "ldap authorize: Userid is blank, AuthType=%s",
            r->ap_auth_type);
    }

    if(!req) {
        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
            "ldap authorize: Creating LDAP req structure");

        /* Build the username filter */
        authn_ldap_build_filter(filtbuf, r, r->user, NULL, sec);

        /* Search for the user DN */
        result = util_ldap_cache_getuserdn(r, ldc, sec->url, sec->basedn,
             sec->scope, sec->attributes, filtbuf, &dn, &vals);

        /* Search failed, log error and return failure */
        if(result != LDAP_SUCCESS) {
            ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                "auth_ldap authorise: User DN not found, %s", ldc->reason);
            return AUTHZ_DENIED;
        }

        req = (authn_ldap_request_t *)apr_pcalloc(r->pool,
            sizeof(authn_ldap_request_t));
        ap_set_module_config(r->request_config, &authnz_ldap_module, req);
        req->dn = apr_pstrdup(r->pool, dn);
        req->user = r->user;
    }

    t = require_args;

    if (req->dn == NULL || strlen(req->dn) == 0) {
        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                      "[%" APR_PID_T_FMT "] auth_ldap authorize: "
                      "require dn: user's DN has not been defined; failing authorization",
                      getpid());
        return AUTHZ_DENIED;
    }

    result = util_ldap_cache_comparedn(r, ldc, sec->url, req->dn, t, sec->compare_dn_on_server);
    switch(result) {
        case LDAP_COMPARE_TRUE: {
            ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                          "[%" APR_PID_T_FMT "] auth_ldap authorize: "
                          "require dn: authorization successful", getpid());
            return AUTHZ_GRANTED;
        }
        default: {
            ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                          "[%" APR_PID_T_FMT "] auth_ldap authorize: "
                          "require dn \"%s\": LDAP error [%s][%s]",
                          getpid(), t, ldc->reason, ldap_err2string(result));
        }
    }


    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                  "[%" APR_PID_T_FMT "] auth_ldap authorize dn: authorization denied for user %s to %s",
                  getpid(), r->user, r->uri);

    return AUTHZ_DENIED;
}

static authz_status ldapattribute_check_authorization(request_rec *r,
                                             const char *require_args)
{
    int result = 0;
    authn_ldap_request_t *req =
        (authn_ldap_request_t *)ap_get_module_config(r->request_config, &authnz_ldap_module);
    authn_ldap_config_t *sec =
        (authn_ldap_config_t *)ap_get_module_config(r->per_dir_config, &authnz_ldap_module);

    util_ldap_connection_t *ldc = NULL;

    const char *t;
    char *w, *value;

    char filtbuf[FILTER_LENGTH];
    const char *dn = NULL;
    const char **vals = NULL;

    if (!sec->have_ldap_url) {
        return AUTHZ_DENIED;
    }

    if (sec->host) {
        ldc = util_ldap_connection_find(r, sec->host, sec->port,
                                       sec->binddn, sec->bindpw, sec->deref,
                                       sec->secure);
        apr_pool_cleanup_register(r->pool, ldc,
                                  authnz_ldap_cleanup_connection_close,
                                  apr_pool_cleanup_null);
    }
    else {
        ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r,
                      "[%" APR_PID_T_FMT "] auth_ldap authorize: no sec->host - weird...?", getpid());
        return AUTHZ_DENIED;
    }

    /*
     * If we have been authenticated by some other module than mod_auth_ldap,
     * the req structure needed for authorization needs to be created
     * and populated with the userid and DN of the account in LDAP
     */

    /* Check that we have a userid to start with */
    if (!r->user) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
            "access to %s failed, reason: no authenticated user", r->uri);
        return AUTHZ_DENIED;
    }

    if (!strlen(r->user)) {
        ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r,
            "ldap authorize: Userid is blank, AuthType=%s",
            r->ap_auth_type);
    }

    if(!req) {
        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
            "ldap authorize: Creating LDAP req structure");

        /* Build the username filter */
        authn_ldap_build_filter(filtbuf, r, r->user, NULL, sec);

        /* Search for the user DN */
        result = util_ldap_cache_getuserdn(r, ldc, sec->url, sec->basedn,
             sec->scope, sec->attributes, filtbuf, &dn, &vals);

        /* Search failed, log error and return failure */
        if(result != LDAP_SUCCESS) {
            ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                "auth_ldap authorise: User DN not found, %s", ldc->reason);
            return AUTHZ_DENIED;
        }

        req = (authn_ldap_request_t *)apr_pcalloc(r->pool,
            sizeof(authn_ldap_request_t));
        ap_set_module_config(r->request_config, &authnz_ldap_module, req);
        req->dn = apr_pstrdup(r->pool, dn);
        req->user = r->user;
    }

    if (req->dn == NULL || strlen(req->dn) == 0) {
        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                      "[%" APR_PID_T_FMT "] auth_ldap authorize: "
                      "require ldap-attribute: user's DN has not been defined; failing authorization",
                      getpid());
        return AUTHZ_DENIED;
    }

    t = require_args;
    while (t[0]) {
        w = ap_getword(r->pool, &t, '=');
        value = ap_getword_conf(r->pool, &t);

        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                      "[%" APR_PID_T_FMT "] auth_ldap authorize: checking attribute"
                      " %s has value %s", getpid(), w, value);
        result = util_ldap_cache_compare(r, ldc, sec->url, req->dn, w, value);
        switch(result) {
            case LDAP_COMPARE_TRUE: {
                ap_log_rerror(APLOG_MARK, APLOG_DEBUG,
                              0, r, "[%" APR_PID_T_FMT "] auth_ldap authorize: "
                              "require attribute: authorization successful", 
                              getpid());
                return AUTHZ_GRANTED;
            }
            default: {
                ap_log_rerror(APLOG_MARK, APLOG_DEBUG,
                              0, r, "[%" APR_PID_T_FMT "] auth_ldap authorize: "
                              "require attribute: authorization failed [%s][%s]", 
                              getpid(), ldc->reason, ldap_err2string(result));
            }
        }
    }

    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                  "[%" APR_PID_T_FMT "] auth_ldap authorize attribute: authorization denied for user %s to %s",
                  getpid(), r->user, r->uri);

    return AUTHZ_DENIED;
}

static authz_status ldapfilter_check_authorization(request_rec *r,
                                             const char *require_args)
{
    int result = 0;
    authn_ldap_request_t *req =
        (authn_ldap_request_t *)ap_get_module_config(r->request_config, &authnz_ldap_module);
    authn_ldap_config_t *sec =
        (authn_ldap_config_t *)ap_get_module_config(r->per_dir_config, &authnz_ldap_module);

    util_ldap_connection_t *ldc = NULL;
    const char *t;

    char filtbuf[FILTER_LENGTH];
    const char *dn = NULL;
    const char **vals = NULL;

    if (!sec->have_ldap_url) {
        return AUTHZ_DENIED;
    }

    if (sec->host) {
        ldc = util_ldap_connection_find(r, sec->host, sec->port,
                                       sec->binddn, sec->bindpw, sec->deref,
                                       sec->secure);
        apr_pool_cleanup_register(r->pool, ldc,
                                  authnz_ldap_cleanup_connection_close,
                                  apr_pool_cleanup_null);
    }
    else {
        ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r,
                      "[%" APR_PID_T_FMT "] auth_ldap authorize: no sec->host - weird...?", getpid());
        return AUTHZ_DENIED;
    }

    /*
     * If we have been authenticated by some other module than mod_auth_ldap,
     * the req structure needed for authorization needs to be created
     * and populated with the userid and DN of the account in LDAP
     */

    /* Check that we have a userid to start with */
    if (!r->user) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
            "access to %s failed, reason: no authenticated user", r->uri);
        return AUTHZ_DENIED;
    }

    if (!strlen(r->user)) {
        ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r,
            "ldap authorize: Userid is blank, AuthType=%s",
            r->ap_auth_type);
    }

    if(!req) {
        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
            "ldap authorize: Creating LDAP req structure");

        /* Build the username filter */
        authn_ldap_build_filter(filtbuf, r, r->user, NULL, sec);

        /* Search for the user DN */
        result = util_ldap_cache_getuserdn(r, ldc, sec->url, sec->basedn,
             sec->scope, sec->attributes, filtbuf, &dn, &vals);

        /* Search failed, log error and return failure */
        if(result != LDAP_SUCCESS) {
            ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                "auth_ldap authorise: User DN not found, %s", ldc->reason);
            return AUTHZ_DENIED;
        }

        req = (authn_ldap_request_t *)apr_pcalloc(r->pool,
            sizeof(authn_ldap_request_t));
        ap_set_module_config(r->request_config, &authnz_ldap_module, req);
        req->dn = apr_pstrdup(r->pool, dn);
        req->user = r->user;
    }

    if (req->dn == NULL || strlen(req->dn) == 0) {
        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                      "[%" APR_PID_T_FMT "] auth_ldap authorize: "
                      "require ldap-filter: user's DN has not been defined; failing authorization",
                      getpid());
        return AUTHZ_DENIED;
    }

    t = require_args;

    if (t[0]) {
        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                      "[%" APR_PID_T_FMT "] auth_ldap authorize: checking filter %s",
                      getpid(), t);

        /* Build the username filter */
        authn_ldap_build_filter(filtbuf, r, req->user, t, sec);

        /* Search for the user DN */
        result = util_ldap_cache_getuserdn(r, ldc, sec->url, sec->basedn,
             sec->scope, sec->attributes, filtbuf, &dn, &vals);

        /* Make sure that the filtered search returned the correct user dn */
        if (result == LDAP_SUCCESS) {
            ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                          "[%" APR_PID_T_FMT "] auth_ldap authorize: checking dn match %s",
                          getpid(), dn);
            result = util_ldap_cache_comparedn(r, ldc, sec->url, req->dn, dn,
                 sec->compare_dn_on_server);
        }

        switch(result) {
            case LDAP_COMPARE_TRUE: {
                ap_log_rerror(APLOG_MARK, APLOG_DEBUG,
                              0, r, "[%" APR_PID_T_FMT "] auth_ldap authorize: "
                              "require ldap-filter: authorization "
                              "successful", getpid());
                return AUTHZ_GRANTED;
            }
            case LDAP_FILTER_ERROR: {
                ap_log_rerror(APLOG_MARK, APLOG_DEBUG,
                              0, r, "[%" APR_PID_T_FMT "] auth_ldap authorize: "
                              "require ldap-filter: %s authorization "
                              "failed [%s][%s]", getpid(),
                              filtbuf, ldc->reason, ldap_err2string(result));
                break;
            }
            default: {
                ap_log_rerror(APLOG_MARK, APLOG_DEBUG,
                              0, r, "[%" APR_PID_T_FMT "] auth_ldap authorize: "
                              "require ldap-filter: authorization "
                              "failed [%s][%s]", getpid(),
                              ldc->reason, ldap_err2string(result));
            }
        }
    }

    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                  "[%" APR_PID_T_FMT "] auth_ldap authorize filter: authorization denied for user %s to %s",
                  getpid(), r->user, r->uri);

    return AUTHZ_DENIED;
}


/*
 * Use the ldap url parsing routines to break up the ldap url into
 * host and port.
 */
static const char *mod_auth_ldap_parse_url(cmd_parms *cmd,
                                    void *config,
                                    const char *url,
                                    const char *mode)
{
    int rc;
    apr_ldap_url_desc_t *urld;
    apr_ldap_err_t *result;

    authn_ldap_config_t *sec = config;

    rc = apr_ldap_url_parse(cmd->pool, url, &(urld), &(result));
    if (rc != APR_SUCCESS) {
        return result->reason;
    }
    sec->url = apr_pstrdup(cmd->pool, url);

    /* Set all the values, or at least some sane defaults */
    if (sec->host) {
        sec->host = apr_pstrcat(cmd->pool, urld->lud_host, " ", sec->host, NULL);
    }
    else {
        sec->host = urld->lud_host? apr_pstrdup(cmd->pool, urld->lud_host) : "localhost";
    }
    sec->basedn = urld->lud_dn? apr_pstrdup(cmd->pool, urld->lud_dn) : "";
    if (urld->lud_attrs && urld->lud_attrs[0]) {
        int i = 1;
        while (urld->lud_attrs[i]) {
            i++;
        }
        sec->attributes = apr_pcalloc(cmd->pool, sizeof(char *) * (i+1));
        i = 0;
        while (urld->lud_attrs[i]) {
            sec->attributes[i] = apr_pstrdup(cmd->pool, urld->lud_attrs[i]);
            i++;
        }
        sec->attribute = sec->attributes[0];
    }
    else {
        sec->attribute = "uid";
    }

    sec->scope = urld->lud_scope == LDAP_SCOPE_ONELEVEL ?
        LDAP_SCOPE_ONELEVEL : LDAP_SCOPE_SUBTREE;

    if (urld->lud_filter) {
        if (urld->lud_filter[0] == '(') {
            /*
             * Get rid of the surrounding parens; later on when generating the
             * filter, they'll be put back.
             */
            sec->filter = apr_pstrdup(cmd->pool, urld->lud_filter+1);
            sec->filter[strlen(sec->filter)-1] = '\0';
        }
        else {
            sec->filter = apr_pstrdup(cmd->pool, urld->lud_filter);
        }
    }
    else {
        sec->filter = "objectclass=*";
    }

    if (mode) {
        if (0 == strcasecmp("NONE", mode)) {
            sec->secure = APR_LDAP_NONE;
        }
        else if (0 == strcasecmp("SSL", mode)) {
            sec->secure = APR_LDAP_SSL;
        }
        else if (0 == strcasecmp("TLS", mode) || 0 == strcasecmp("STARTTLS", mode)) {
            sec->secure = APR_LDAP_STARTTLS;
        }
        else {
            return "Invalid LDAP connection mode setting: must be one of NONE, "
                   "SSL, or TLS/STARTTLS";
        }
    }

      /* "ldaps" indicates secure ldap connections desired
      */
    if (strncasecmp(url, "ldaps", 5) == 0)
    {
        sec->secure = APR_LDAP_SSL;
        sec->port = urld->lud_port? urld->lud_port : LDAPS_PORT;
    }
    else
    {
        sec->port = urld->lud_port? urld->lud_port : LDAP_PORT;
    }

    sec->have_ldap_url = 1;

    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0,
                 cmd->server, "[%" APR_PID_T_FMT "] auth_ldap url parse: `%s', Host: %s, Port: %d, DN: %s, attrib: %s, scope: %s, filter: %s, connection mode: %s",
                 getpid(),
                 url,
                 urld->lud_host,
                 urld->lud_port,
                 urld->lud_dn,
                 urld->lud_attrs? urld->lud_attrs[0] : "(null)",
                 (urld->lud_scope == LDAP_SCOPE_SUBTREE? "subtree" :
                  urld->lud_scope == LDAP_SCOPE_BASE? "base" :
                  urld->lud_scope == LDAP_SCOPE_ONELEVEL? "onelevel" : "unknown"),
                 urld->lud_filter,
                 sec->secure == APR_LDAP_SSL  ? "using SSL": "not using SSL"
                 );

    return NULL;
}

static const char *mod_auth_ldap_set_deref(cmd_parms *cmd, void *config, const char *arg)
{
    authn_ldap_config_t *sec = config;

    if (strcmp(arg, "never") == 0 || strcasecmp(arg, "off") == 0) {
        sec->deref = never;
    }
    else if (strcmp(arg, "searching") == 0) {
        sec->deref = searching;
    }
    else if (strcmp(arg, "finding") == 0) {
        sec->deref = finding;
    }
    else if (strcmp(arg, "always") == 0 || strcasecmp(arg, "on") == 0) {
        sec->deref = always;
    }
    else {
        return "Unrecognized value for AuthLDAPAliasDereference directive";
    }
    return NULL;
}

static const char *mod_auth_ldap_add_subgroup_attribute(cmd_parms *cmd, void *config, const char *arg)
{
    int i = 0;

    authn_ldap_config_t *sec = config;

    for (i = 0; sec->sgAttributes[i]; i++) {
        ;
    }
    if (i == GROUPATTR_MAX_ELTS)
        return "Too many AuthLDAPSubGroupAttribute values";

    sec->sgAttributes[i] = apr_pstrdup(cmd->pool, arg);

    return NULL;
}

static const char *mod_auth_ldap_add_subgroup_class(cmd_parms *cmd, void *config, const char *arg)
{
    struct mod_auth_ldap_groupattr_entry_t *new;

    authn_ldap_config_t *sec = config;

    if (sec->subgroupclasses->nelts > GROUPATTR_MAX_ELTS)
        return "Too many AuthLDAPSubGroupClass values";

    new = apr_array_push(sec->subgroupclasses);
    new->name = apr_pstrdup(cmd->pool, arg);

    return NULL;
}

static const char *mod_auth_ldap_set_subgroup_maxdepth(cmd_parms *cmd,
                                                       void *config,
                                                       const char *max_depth)
{
    authn_ldap_config_t *sec = config;

    sec->maxNestingDepth = atol(max_depth);

    return NULL;
}

static const char *mod_auth_ldap_add_group_attribute(cmd_parms *cmd, void *config, const char *arg)
{
    struct mod_auth_ldap_groupattr_entry_t *new;

    authn_ldap_config_t *sec = config;

    if (sec->groupattr->nelts > GROUPATTR_MAX_ELTS)
        return "Too many AuthLDAPGroupAttribute directives";

    new = apr_array_push(sec->groupattr);
    new->name = apr_pstrdup(cmd->pool, arg);

    return NULL;
}

static const char *set_charset_config(cmd_parms *cmd, void *config, const char *arg)
{
    ap_set_module_config(cmd->server->module_config, &authnz_ldap_module,
                         (void *)arg);
    return NULL;
}

static const command_rec authnz_ldap_cmds[] =
{
    AP_INIT_TAKE12("AuthLDAPURL", mod_auth_ldap_parse_url, NULL, OR_AUTHCFG,
                  "URL to define LDAP connection. This should be an RFC 2255 compliant\n"
                  "URL of the form ldap://host[:port]/basedn[?attrib[?scope[?filter]]].\n"
                  "<ul>\n"
                  "<li>Host is the name of the LDAP server. Use a space separated list of hosts \n"
                  "to specify redundant servers.\n"
                  "<li>Port is optional, and specifies the port to connect to.\n"
                  "<li>basedn specifies the base DN to start searches from\n"
                  "<li>Attrib specifies what attribute to search for in the directory. If not "
                  "provided, it defaults to <b>uid</b>.\n"
                  "<li>Scope is the scope of the search, and can be either <b>sub</b> or "
                  "<b>one</b>. If not provided, the default is <b>sub</b>.\n"
                  "<li>Filter is a filter to use in the search. If not provided, "
                  "defaults to <b>(objectClass=*)</b>.\n"
                  "</ul>\n"
                  "Searches are performed using the attribute and the filter combined. "
                  "For example, assume that the\n"
                  "LDAP URL is <b>ldap://ldap.airius.com/ou=People, o=Airius?uid?sub?(posixid=*)</b>. "
                  "Searches will\n"
                  "be done using the filter <b>(&((posixid=*))(uid=<i>username</i>))</b>, "
                  "where <i>username</i>\n"
                  "is the user name passed by the HTTP client. The search will be a subtree "
                  "search on the branch <b>ou=People, o=Airius</b>."),

    AP_INIT_TAKE1("AuthLDAPBindDN", ap_set_string_slot,
                  (void *)APR_OFFSETOF(authn_ldap_config_t, binddn), OR_AUTHCFG,
                  "DN to use to bind to LDAP server. If not provided, will do an anonymous bind."),

    AP_INIT_TAKE1("AuthLDAPBindPassword", ap_set_string_slot,
                  (void *)APR_OFFSETOF(authn_ldap_config_t, bindpw), OR_AUTHCFG,
                  "Password to use to bind to LDAP server. If not provided, will do an anonymous bind."),

    AP_INIT_FLAG("AuthLDAPBindAuthoritative", ap_set_flag_slot,
                  (void *)APR_OFFSETOF(authn_ldap_config_t, bind_authoritative), OR_AUTHCFG,
                  "Set to 'on' to return failures when user-specific bind fails - defaults to on."),

    AP_INIT_FLAG("AuthLDAPRemoteUserIsDN", ap_set_flag_slot,
                 (void *)APR_OFFSETOF(authn_ldap_config_t, user_is_dn), OR_AUTHCFG,
                 "Set to 'on' to set the REMOTE_USER environment variable to be the full "
                 "DN of the remote user. By default, this is set to off, meaning that "
                 "the REMOTE_USER variable will contain whatever value the remote user sent."),

    AP_INIT_TAKE1("AuthLDAPRemoteUserAttribute", ap_set_string_slot,
                 (void *)APR_OFFSETOF(authn_ldap_config_t, remote_user_attribute), OR_AUTHCFG,
                 "Override the user supplied username and place the "
                 "contents of this attribute in the REMOTE_USER "
                 "environment variable."),

    AP_INIT_FLAG("AuthLDAPCompareDNOnServer", ap_set_flag_slot,
                 (void *)APR_OFFSETOF(authn_ldap_config_t, compare_dn_on_server), OR_AUTHCFG,
                 "Set to 'on' to force auth_ldap to do DN compares (for the \"require dn\" "
                 "directive) using the server, and set it 'off' to do the compares locally "
                 "(at the expense of possible false matches). See the documentation for "
                 "a complete description of this option."),

    AP_INIT_ITERATE("AuthLDAPSubGroupAttribute", mod_auth_ldap_add_subgroup_attribute, NULL, OR_AUTHCFG,
                    "Attribute labels used to define sub-group (or nested group) membership in groups - "
                    "defaults to member and uniqueMember"),

    AP_INIT_ITERATE("AuthLDAPSubGroupClass", mod_auth_ldap_add_subgroup_class, NULL, OR_AUTHCFG,
                     "LDAP objectClass values used to identify sub-group instances - "
                     "defaults to groupOfNames and groupOfUniqueNames"),

    AP_INIT_TAKE1("AuthLDAPMaxSubGroupDepth", mod_auth_ldap_set_subgroup_maxdepth, NULL, OR_AUTHCFG,
                      "Maximum subgroup nesting depth to be evaluated - defaults to 10 (top-level group = 0)"),

    AP_INIT_ITERATE("AuthLDAPGroupAttribute", mod_auth_ldap_add_group_attribute, NULL, OR_AUTHCFG,
                    "A list of attribute labels used to identify the user members of groups - defaults to "
                    "member and uniquemember"),

    AP_INIT_FLAG("AuthLDAPGroupAttributeIsDN", ap_set_flag_slot,
                 (void *)APR_OFFSETOF(authn_ldap_config_t, group_attrib_is_dn), OR_AUTHCFG,
                 "If set to 'on', auth_ldap uses the DN that is retrieved from the server for"
                 "subsequent group comparisons. If set to 'off', auth_ldap uses the string"
                 "provided by the client directly. Defaults to 'on'."),

    AP_INIT_TAKE1("AuthLDAPDereferenceAliases", mod_auth_ldap_set_deref, NULL, OR_AUTHCFG,
                  "Determines how aliases are handled during a search. Can be one of the"
                  "values \"never\", \"searching\", \"finding\", or \"always\". "
                  "Defaults to always."),

    AP_INIT_TAKE1("AuthLDAPCharsetConfig", set_charset_config, NULL, RSRC_CONF,
                  "Character set conversion configuration file. If omitted, character set"
                  "conversion is disabled."),

    {NULL}
};

static int authnz_ldap_post_config(apr_pool_t *p, apr_pool_t *plog, apr_pool_t *ptemp, server_rec *s)
{
    ap_configfile_t *f;
    char l[MAX_STRING_LEN];
    const char *charset_confname = ap_get_module_config(s->module_config,
                                                      &authnz_ldap_module);
    apr_status_t status;

    /*
    authn_ldap_config_t *sec = (authn_ldap_config_t *)
                                    ap_get_module_config(s->module_config,
                                                         &authnz_ldap_module);

    if (sec->secure)
    {
        if (!util_ldap_ssl_supported(s))
        {
            ap_log_error(APLOG_MARK, APLOG_CRIT, 0, s,
                     "LDAP: SSL connections (ldaps://) not supported by utilLDAP");
            return(!OK);
        }
    }
    */

    /* make sure that mod_ldap (util_ldap) is loaded */
    if (ap_find_linked_module("util_ldap.c") == NULL) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                     "Module mod_ldap missing. Mod_ldap (aka. util_ldap) "
                     "must be loaded in order for mod_authnz_ldap to function properly");
        return HTTP_INTERNAL_SERVER_ERROR;

    }

    if (!charset_confname) {
        return OK;
    }

    charset_confname = ap_server_root_relative(p, charset_confname);
    if (!charset_confname) {
        ap_log_error(APLOG_MARK, APLOG_ERR, APR_EBADPATH, s,
                     "Invalid charset conversion config path %s",
                     (const char *)ap_get_module_config(s->module_config,
                                                        &authnz_ldap_module));
        return HTTP_INTERNAL_SERVER_ERROR;
    }
    if ((status = ap_pcfg_openfile(&f, ptemp, charset_confname))
                != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, status, s,
                     "could not open charset conversion config file %s.",
                     charset_confname);
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    charset_conversions = apr_hash_make(p);

    while (!(ap_cfg_getline(l, MAX_STRING_LEN, f))) {
        const char *ll = l;
        char *lang;

        if (l[0] == '#') {
            continue;
        }
        lang = ap_getword_conf(p, &ll);
        ap_str_tolower(lang);

        if (ll[0]) {
            char *charset = ap_getword_conf(p, &ll);
            apr_hash_set(charset_conversions, lang, APR_HASH_KEY_STRING, charset);
        }
    }
    ap_cfg_closefile(f);

    to_charset = derive_codepage_from_lang (p, "utf-8");
    if (to_charset == NULL) {
        ap_log_error(APLOG_MARK, APLOG_ERR, status, s,
                     "could not find the UTF-8 charset in the file %s.",
                     charset_confname);
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    return OK;
}

static const authn_provider authn_ldap_provider =
{
    &authn_ldap_check_password,
};

static const authz_provider authz_ldapuser_provider =
{
    &ldapuser_check_authorization,
};
static const authz_provider authz_ldapgroup_provider =
{
    &ldapgroup_check_authorization,
};

static const authz_provider authz_ldapdn_provider =
{
    &ldapdn_check_authorization,
};

static const authz_provider authz_ldapattribute_provider =
{
    &ldapattribute_check_authorization,
};

static const authz_provider authz_ldapfilter_provider =
{
    &ldapfilter_check_authorization,
};

static void ImportULDAPOptFn(void)
{
    util_ldap_connection_close  = APR_RETRIEVE_OPTIONAL_FN(uldap_connection_close);
    util_ldap_connection_find   = APR_RETRIEVE_OPTIONAL_FN(uldap_connection_find);
    util_ldap_cache_comparedn   = APR_RETRIEVE_OPTIONAL_FN(uldap_cache_comparedn);
    util_ldap_cache_compare     = APR_RETRIEVE_OPTIONAL_FN(uldap_cache_compare);
    util_ldap_cache_checkuserid = APR_RETRIEVE_OPTIONAL_FN(uldap_cache_checkuserid);
    util_ldap_cache_getuserdn   = APR_RETRIEVE_OPTIONAL_FN(uldap_cache_getuserdn);
    util_ldap_ssl_supported     = APR_RETRIEVE_OPTIONAL_FN(uldap_ssl_supported);
    util_ldap_cache_check_subgroups = APR_RETRIEVE_OPTIONAL_FN(uldap_cache_check_subgroups);
}

static void register_hooks(apr_pool_t *p)
{
    /* Register authn provider */
    ap_register_auth_provider(p, AUTHN_PROVIDER_GROUP, "ldap",
                              AUTHN_PROVIDER_VERSION,
                              &authn_ldap_provider, AP_AUTH_INTERNAL_PER_CONF);

    /* Register authz providers */
    ap_register_auth_provider(p, AUTHZ_PROVIDER_GROUP, "ldap-user",
                              AUTHZ_PROVIDER_VERSION,
                              &authz_ldapuser_provider,
                              AP_AUTH_INTERNAL_PER_CONF);
    ap_register_auth_provider(p, AUTHZ_PROVIDER_GROUP, "ldap-group",
                              AUTHZ_PROVIDER_VERSION,
                              &authz_ldapgroup_provider,
                              AP_AUTH_INTERNAL_PER_CONF);
    ap_register_auth_provider(p, AUTHZ_PROVIDER_GROUP, "ldap-dn",
                              AUTHZ_PROVIDER_VERSION,
                              &authz_ldapdn_provider,
                              AP_AUTH_INTERNAL_PER_CONF);
    ap_register_auth_provider(p, AUTHZ_PROVIDER_GROUP, "ldap-attribute",
                              AUTHZ_PROVIDER_VERSION,
                              &authz_ldapattribute_provider,
                              AP_AUTH_INTERNAL_PER_CONF);
    ap_register_auth_provider(p, AUTHZ_PROVIDER_GROUP, "ldap-filter",
                              AUTHZ_PROVIDER_VERSION,
                              &authz_ldapfilter_provider,
                              AP_AUTH_INTERNAL_PER_CONF);

    ap_hook_post_config(authnz_ldap_post_config,NULL,NULL,APR_HOOK_MIDDLE);

    ap_hook_optional_fn_retrieve(ImportULDAPOptFn,NULL,NULL,APR_HOOK_MIDDLE);
}

module AP_MODULE_DECLARE_DATA authnz_ldap_module =
{
    STANDARD20_MODULE_STUFF,
    create_authnz_ldap_dir_config,   /* dir config creater */
    NULL,                            /* dir merger --- default is to override */
    NULL,                            /* server config */
    NULL,                            /* merge server config */
    authnz_ldap_cmds,                /* command apr_table_t */
    register_hooks                   /* register hooks */
};
