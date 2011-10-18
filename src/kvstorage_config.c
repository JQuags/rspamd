/* Copyright (c) 2010, Vsevolod Stakhov
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *       * Redistributions of source code must retain the above copyright
 *         notice, this list of conditions and the following disclaimer.
 *       * Redistributions in binary form must reproduce the above copyright
 *         notice, this list of conditions and the following disclaimer in the
 *         documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL Rambler BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "kvstorage_config.h"
#include "main.h"
#include "cfg_xml.h"

#define LRU_QUEUES 32

/* Global hash of storages indexed by id */
GHashTable *storages = NULL;
/* Last used id for explicit numbering */
gint last_id = 0;

struct kvstorage_config_parser {
	enum {
		KVSTORAGE_STATE_INIT,
		KVSTORAGE_STATE_PARAM,
		KVSTORAGE_STATE_BACKEND,
		KVSTORAGE_STATE_EXPIRE,
		KVSTORAGE_STATE_ID,
		KVSTORAGE_STATE_NAME,
		KVSTORAGE_STATE_CACHE_TYPE,
		KVSTORAGE_STATE_CACHE_MAX_ELTS,
		KVSTORAGE_STATE_CACHE_MAX_MEM,
		KVSTORAGE_STATE_BACKEND_TYPE,
		KVSTORAGE_STATE_EXPIRE_TYPE,
		KVSTORAGE_STATE_ERROR
	} state;
	struct kvstorage_config *current_storage;
	memory_pool_t *pool;
	gchar *cur_elt;
};

static void
kvstorage_config_destroy (gpointer k)
{
	struct kvstorage_config				*kconf = k;

	if (kconf->name) {
		g_free (kconf->name);
	}

	if (kconf->storage) {
		rspamd_kv_storage_destroy (kconf->storage);
	}

	g_free (kconf);
}

/* Init kvstorage */
static void
kvstorage_init_callback (const gpointer key, const gpointer value, gpointer unused)
{
	struct kvstorage_config				*kconf = value;
	struct rspamd_kv_cache				*cache;
	struct rspamd_kv_backend			*backend;
	struct rspamd_kv_expire				*expire;

	switch (kconf->cache.type) {
	case KVSTORAGE_TYPE_CACHE_HASH:
		cache = rspamd_kv_hash_new ();
		break;
	case KVSTORAGE_TYPE_CACHE_RADIX:
		cache = rspamd_kv_radix_new ();
		break;
	}

	switch (kconf->backend.type) {
	case KVSTORAGE_TYPE_BACKEND_NULL:
		backend = NULL;
		break;
	}

	switch (kconf->expire.type) {
	case KVSTORAGE_TYPE_EXPIRE_LRU:
		expire = rspamd_lru_expire_new (LRU_QUEUES);
		break;
	}

	kconf->storage = rspamd_kv_storage_new (kconf->id, kconf->name, cache, backend, expire,
			kconf->cache.max_elements, kconf->cache.max_memory);
}

/* XML parse callbacks */
/* Called for open tags <foo bar="baz"> */
void kvstorage_xml_start_element (GMarkupParseContext	*context,
								const gchar         *element_name,
								const gchar        **attribute_names,
								const gchar        **attribute_values,
								gpointer             user_data,
								GError             **error)
{
	struct kvstorage_config_parser 	*kv_parser = user_data;

	switch (kv_parser->state) {
	case KVSTORAGE_STATE_INIT:
		/* Make temporary pool */
		if (kv_parser->pool != NULL) {
			memory_pool_delete (kv_parser->pool);
		}
		kv_parser->pool = memory_pool_new (memory_pool_get_size ());

		/* Create new kvstorage_config */
		kv_parser->current_storage = g_malloc0 (sizeof (struct kvstorage_config));
		kv_parser->current_storage->id = ++last_id;
		break;
	case KVSTORAGE_STATE_PARAM:
		if (g_ascii_strcasecmp (element_name, "type") == 0) {
			kv_parser->state = KVSTORAGE_STATE_CACHE_TYPE;
		}
		else if (g_ascii_strcasecmp (element_name, "max_elements") == 0) {
			kv_parser->state = KVSTORAGE_STATE_CACHE_MAX_ELTS;
		}
		else if (g_ascii_strcasecmp (element_name, "max_memory") == 0) {
			kv_parser->state = KVSTORAGE_STATE_CACHE_MAX_MEM;
		}
		else if (g_ascii_strcasecmp (element_name, "id") == 0) {
			kv_parser->state = KVSTORAGE_STATE_ID;
		}
		else if (g_ascii_strcasecmp (element_name, "name") == 0) {
			kv_parser->state = KVSTORAGE_STATE_NAME;
		}
		else if (g_ascii_strcasecmp (element_name, "backend") == 0) {
			kv_parser->state = KVSTORAGE_STATE_BACKEND;
		}
		else if (g_ascii_strcasecmp (element_name, "expire") == 0) {
			kv_parser->state = KVSTORAGE_STATE_EXPIRE;
		}
		else {
			if (*error == NULL) {
				*error = g_error_new (xml_error_quark (), XML_EXTRA_ELEMENT, "element %s is unexpected",
						element_name);
			}
			kv_parser->state = KVSTORAGE_STATE_ERROR;
		}
		kv_parser->cur_elt = memory_pool_strdup (kv_parser->pool, element_name);
		break;
	case KVSTORAGE_STATE_BACKEND:
		if (g_ascii_strcasecmp (element_name, "type") == 0) {
			kv_parser->state = KVSTORAGE_STATE_BACKEND_TYPE;
		}
		else {
			if (*error == NULL) {
				*error = g_error_new (xml_error_quark (), XML_EXTRA_ELEMENT, "element %s is unexpected in backend definition",
						element_name);
			}
			kv_parser->state = KVSTORAGE_STATE_ERROR;
		}
		break;
	case KVSTORAGE_STATE_EXPIRE:
		if (g_ascii_strcasecmp (element_name, "type") == 0) {
			kv_parser->state = KVSTORAGE_STATE_EXPIRE_TYPE;
		}
		else {
			if (*error == NULL) {
				*error = g_error_new (xml_error_quark (), XML_EXTRA_ELEMENT, "element %s is unexpected in expire definition",
						element_name);
			}
			kv_parser->state = KVSTORAGE_STATE_ERROR;
		}
		break;
	default:
		/* Do nothing at other states */
		break;
	}

}

#define CHECK_TAG(s)																													\
do {																																	\
if (g_ascii_strcasecmp (element_name, kv_parser->cur_elt) == 0) {																		\
	kv_parser->state = (s);																												\
}																																		\
else {																																	\
	if (*error == NULL) *error = g_error_new (xml_error_quark (), XML_UNMATCHED_TAG, "element %s is unexpected in this state, expected %s", element_name, kv_parser->cur_elt);	\
	kv_parser->state = KVSTORAGE_STATE_ERROR;																							\
}																																		\
} while (0)

/* Called for close tags </foo> */
void kvstorage_xml_end_element (GMarkupParseContext	*context,
								const gchar         *element_name,
								gpointer             user_data,
								GError             **error)
{
	struct kvstorage_config_parser 	*kv_parser = user_data;

	switch (kv_parser->state) {
	case KVSTORAGE_STATE_INIT:
	case KVSTORAGE_STATE_PARAM:
		if (g_ascii_strcasecmp (element_name, "keystorage") == 0) {
			/* XXX: Init actual storage */
			g_hash_table_insert (storages, &kv_parser->current_storage->id, kv_parser->current_storage);
			kv_parser->state = KVSTORAGE_STATE_INIT;
			g_markup_parse_context_pop (context);
			g_hash_table_foreach (storages, kvstorage_init_callback, NULL);
			return;
		}
		if (*error == NULL) {
			*error = g_error_new (xml_error_quark (), XML_EXTRA_ELEMENT, "end element %s is unexpected, expected start element",
					element_name);
		}
		kv_parser->state = KVSTORAGE_STATE_ERROR;
		break;
	case KVSTORAGE_STATE_ID:
	case KVSTORAGE_STATE_NAME:
	case KVSTORAGE_STATE_CACHE_TYPE:
	case KVSTORAGE_STATE_CACHE_MAX_ELTS:
	case KVSTORAGE_STATE_CACHE_MAX_MEM:
		CHECK_TAG (KVSTORAGE_STATE_PARAM);
		break;
	case KVSTORAGE_STATE_BACKEND_TYPE:
		CHECK_TAG (KVSTORAGE_STATE_BACKEND);
		break;
	case KVSTORAGE_STATE_EXPIRE_TYPE:
		CHECK_TAG (KVSTORAGE_STATE_EXPIRE);
		break;
	case KVSTORAGE_STATE_BACKEND:
		if (g_ascii_strcasecmp (element_name, "backend") == 0) {
			kv_parser->state = KVSTORAGE_STATE_PARAM;
		}
		else {
			if (*error == NULL) {
				*error = g_error_new (xml_error_quark (), XML_EXTRA_ELEMENT, "element %s is unexpected",
						element_name);
			}
			kv_parser->state = KVSTORAGE_STATE_ERROR;
		}
		break;
	case KVSTORAGE_STATE_EXPIRE:
		if (g_ascii_strcasecmp (element_name, "expire") == 0) {
			kv_parser->state = KVSTORAGE_STATE_PARAM;
		}
		else {
			if (*error == NULL) {
				*error = g_error_new (xml_error_quark (), XML_EXTRA_ELEMENT, "element %s is unexpected",
						element_name);
			}
			kv_parser->state = KVSTORAGE_STATE_ERROR;
		}
		break;
	default:
		/* Do nothing at other states */
		break;
	}
}
#undef CHECK_TAG

/* text is not nul-terminated */
void kvstorage_xml_text       (GMarkupParseContext		*context,
								const gchar         *text,
								gsize                text_len,
								gpointer             user_data,
								GError             **error)
{
	struct kvstorage_config_parser 	*kv_parser = user_data;
	gchar                           *err_str;

	switch (kv_parser->state) {
	case KVSTORAGE_STATE_INIT:
	case KVSTORAGE_STATE_PARAM:
		if (*error == NULL) {
			*error = g_error_new (xml_error_quark (), XML_EXTRA_ELEMENT, "text is unexpected, expected start element");
		}
		kv_parser->state = KVSTORAGE_STATE_ERROR;
		break;
	case KVSTORAGE_STATE_ID:
		kv_parser->current_storage->id = strtoul (text, &err_str, 10);
		if ((gsize)(err_str - text) != text_len) {
			if (*error == NULL) {
				*error = g_error_new (xml_error_quark (), XML_EXTRA_ELEMENT, "invalid number: %*s", (int)text_len, text);
			}
			kv_parser->state = KVSTORAGE_STATE_ERROR;
		}
		else {
			last_id = kv_parser->current_storage->id;
		}
		break;
	case KVSTORAGE_STATE_NAME:
		kv_parser->current_storage->name = g_malloc (text_len + 1);
		rspamd_strlcpy (kv_parser->current_storage->name, text, text_len + 1);
		break;
	case KVSTORAGE_STATE_CACHE_MAX_ELTS:
		kv_parser->current_storage->cache.max_elements = parse_limit (text, text_len);
		break;
	case KVSTORAGE_STATE_CACHE_MAX_MEM:
		kv_parser->current_storage->cache.max_memory = parse_limit (text, text_len);
		break;
	case KVSTORAGE_STATE_CACHE_TYPE:
		if (g_ascii_strncasecmp (text, "hash", MIN (text_len, sizeof ("hash") - 1)) == 0) {
			kv_parser->current_storage->cache.type = KVSTORAGE_TYPE_CACHE_HASH;
		}
		else if (g_ascii_strncasecmp (text, "radix", MIN (text_len, sizeof ("radix") - 1)) == 0) {
			kv_parser->current_storage->cache.type = KVSTORAGE_TYPE_CACHE_RADIX;
		}
		else {
			if (*error == NULL) {
				*error = g_error_new (xml_error_quark (), XML_EXTRA_ELEMENT, "invalid cache type: %*s", (int)text_len, text);
			}
			kv_parser->state = KVSTORAGE_STATE_ERROR;
		}
		break;
	case KVSTORAGE_STATE_BACKEND_TYPE:
		if (g_ascii_strncasecmp (text, "null", MIN (text_len, sizeof ("null") - 1)) == 0) {
			kv_parser->current_storage->backend.type = KVSTORAGE_TYPE_BACKEND_NULL;
		}
		else {
			if (*error == NULL) {
				*error = g_error_new (xml_error_quark (), XML_EXTRA_ELEMENT, "invalid backend type: %*s", (int)text_len, text);
			}
			kv_parser->state = KVSTORAGE_STATE_ERROR;
		}
		break;
	case KVSTORAGE_STATE_EXPIRE_TYPE:
		if (g_ascii_strncasecmp (text, "lru", MIN (text_len, sizeof ("lru") - 1)) == 0) {
			kv_parser->current_storage->expire.type = KVSTORAGE_TYPE_EXPIRE_LRU;
		}
		else {
			if (*error == NULL) {
				*error = g_error_new (xml_error_quark (), XML_EXTRA_ELEMENT, "invalid expire type: %*s", (int)text_len, text);
			}
			kv_parser->state = KVSTORAGE_STATE_ERROR;
		}
		break;
	default:
		/* Do nothing at other states */
		break;
	}

}

/* Called on error, including one set by other
* methods in the vtable. The GError should not be freed.
*/
void kvstorage_xml_error	(GMarkupParseContext		*context,
								GError              *error,
								gpointer             user_data)
{
	msg_err ("kvstorage xml parser error: %s", error->message);
}

/** Public API */

/* Init subparser of kvstorage config */
void
init_kvstorage_config (void)
{
	GMarkupParser					*parser;
	struct kvstorage_config_parser 	*kv_parser;

	if (storages == NULL) {
		storages = g_hash_table_new_full (g_int_hash, g_int_equal, NULL, kvstorage_config_destroy);
	}
	else {
		/* Create new global table */
		g_hash_table_destroy (storages);
		storages = g_hash_table_new_full (g_int_hash, g_int_equal, NULL, kvstorage_config_destroy);
	}

	/* Create and register subparser */
	parser = g_malloc0 (sizeof (GMarkupParser));
	parser->start_element = kvstorage_xml_start_element;
	parser->end_element = kvstorage_xml_end_element;
	parser->error = kvstorage_xml_error;
	parser->text = kvstorage_xml_text;

	kv_parser = g_malloc0 (sizeof (struct kvstorage_config_parser));
	kv_parser->state = KVSTORAGE_STATE_PARAM;

	register_subparser ("keystorage", XML_READ_START, parser, kv_parser);
}

/* Get configuration for kvstorage with specified ID */
struct kvstorage_config*
get_kvstorage_config (gint id)
{
	if (storages == NULL) {
		return NULL;
	}
	return g_hash_table_lookup (storages, &id);
}
