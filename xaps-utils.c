/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2014 Stefan Arentz <stefan@arentz.ca>
 * Copyright (c) 2017 Frederik Schwan <frederik dot schwan at linux dot com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <config.h>
#include <lib.h>
#include <hash.h>
#include <http-client.h>
#include <http-url.h>
#include <json-parser.h>
#include <str.h>
#include <strescape.h>
#include <iostream-ssl.h>
#include <mail-storage-private.h>

#include <push-notification-plugin.h>
#include <push-notification-drivers.h>
#include <push-notification-txn-msg.h>

#include "xaps-utils.h"

#define DEFAULT_TIMEOUT_MSECS 5000
#define DEFAULT_RETRY_COUNT 6

struct xaps_config *xaps_global;

// get the real name for users who are actually an alias
const char *get_real_mbox_user(struct mail_user *muser) {
    const char *username = muser->username;
    if (xaps_global->user_lookup != NULL) {
        const char *userdb_username = mail_user_plugin_getenv(muser, xaps_global->user_lookup);
        if (userdb_username != NULL) {
            username = userdb_username;
        }
    }
    return username;
}

/* Callback needed for i_stream_add_destroy_callback() in
   push_notification_driver_ox_process_msg. */
void str_free_i(string_t *str)
{
    str_free(&str);
}



static struct xaps_raw_config *xaps_parse_config(const char *p) {
    const char **args, *key, *p2, *value;
    struct xaps_raw_config *raw_config;

    raw_config = t_new(struct xaps_raw_config, 1);
    raw_config->raw_config = p;

    hash_table_create(&raw_config->config, unsafe_data_stack_pool, 0,
                      str_hash, strcmp);

    if (p == NULL)
        return raw_config;

    args = t_strsplit_spaces(p, " ");

    for (; *args != NULL; args++) {
        p2 = strchr(*args, '=');
        if (p2 != NULL) {
            key = t_strdup_until(*args, p2);
            value = t_strdup(p2 + 1);
        } else {
            key = *args;
            value = "";
        }
        hash_table_update(raw_config->config, key, value);
    }

    return raw_config;
}

void xaps_init(struct mail_user *muser, const char *http_path, pool_t pPool) {
    const char *error, *tmp;
    struct http_client_settings http_set;
    struct ssl_iostream_settings ssl_set;
    const char *xaps_config_string;
    struct xaps_raw_config *config;

    xaps_config_string = mail_user_plugin_getenv(muser, "xaps_config");
    i_assert(xaps_config_string != NULL);

    if (xaps_global == NULL) {
        xaps_global = i_new(struct xaps_config, 1);
    }

    config = xaps_parse_config(xaps_config_string);

    /* Valid config keys: url, user_lookup, max_retries, timeout_msecs */
    tmp = hash_table_lookup(config->config, (const char *) "url");
    i_assert(tmp != NULL);


    int ret = http_url_parse(tmp, NULL, HTTP_URL_ALLOW_USERINFO_PART, pPool,
                       &xaps_global->http_url, &error);
    xaps_global->http_url->path = http_path;
    i_assert(ret == 0);

    tmp = hash_table_lookup(config->config,(const char *) "user_lookup");
    if (tmp != NULL) {
        xaps_global->user_lookup = tmp;
    }


    tmp = hash_table_lookup(config->config, (const char *) "max_retries");
    if ((tmp == NULL) ||
        (str_to_uint(tmp, &xaps_global->http_max_retries) < 0)) {
        xaps_global->http_max_retries = DEFAULT_RETRY_COUNT;
    }
    tmp = hash_table_lookup(config->config, (const char *) "timeout_msecs");
    if ((tmp == NULL) ||
        (str_to_uint(tmp, &xaps_global->http_timeout_msecs) < 0)) {
        xaps_global->http_timeout_msecs = DEFAULT_TIMEOUT_MSECS;
    }

    if (xaps_global->http_client == NULL) {
        /* This is going to use the first user's settings, but these are
           unlikely to change between users so it shouldn't matter much.
         */
        i_zero(&http_set);
        http_set.max_attempts = xaps_global->http_max_retries + 1;
        http_set.request_timeout_msecs = xaps_global->http_timeout_msecs;
        i_zero(&ssl_set);
        mail_user_init_ssl_client_settings(muser, &ssl_set);
        http_set.ssl = &ssl_set;

        xaps_global->http_client = http_client_init(&http_set);
    }
}

void push_notification_driver_xaps_deinit(struct push_notification_driver_user *duser ATTR_UNUSED) {
    if (xaps_global != NULL) {
        if (xaps_global->http_client != NULL)
            http_client_wait(xaps_global->http_client);
    }
}

void push_notification_driver_xaps_cleanup(void)
{
    if (xaps_global != NULL) {
        if (xaps_global->http_client != NULL) {
            http_client_deinit(&xaps_global->http_client);
        }
        i_free_and_null(xaps_global);
    }
}
