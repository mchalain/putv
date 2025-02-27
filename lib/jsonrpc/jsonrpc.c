#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <pthread.h>

#include <jansson.h>
#include "jsonrpc.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#define TYPE_RECEIVE_RESPONSE 'a'
#define TYPE_SEND_RESPONSE 'r'
#define TYPE_RECEIVE_REQUEST 'r'
#define TYPE_SEND_REQUEST 'a'
#define TYPE_RECEIVE_NOTIFICATION 'n'
#define TYPE_SEND_NOTIFICATION 'n'

typedef struct jsonrpc_response_s jsonrpc_response_t;
struct jsonrpc_response_s
{
	unsigned long id;
	jsonrpc_method_prototype funcptr;
	jsonrpc_response_t *next;
};

json_t *jsonrpc_error_object(int code, const char *message, json_t *data)
{
	/* reference to data is stolen */

	json_t *json;

	if (!message)
		message = "";

	json = json_pack("{s:i,s:s}", "code", code, "message", message);
	if (data) {
		json_object_set_new(json, "data", data);
	}
	return json;
}

json_t *jsonrpc_error_object_predefined(int code, json_t *data)
{
	/* reference to data is stolen */

	const char *message = "";

	assert(-32768 <= code && code <= -32000);	// reserved for pre-defined errors

	switch (code) {
		case JSONRPC_PARSE_ERROR:
			message = "Parse Error";
			break;
		case JSONRPC_INVALID_REQUEST:
			message = "Invalid Request";
			break;
		case JSONRPC_METHOD_NOT_FOUND:
			message = "Method not found";
			break;
		case JSONRPC_INVALID_PARAMS:
			message = "Invalid params";
			break;
		case JSONRPC_INTERNAL_ERROR:
			message = "Internal error";
			break;
	}

	return jsonrpc_error_object(code, message, data);
}

json_t *jsonrpc_ignore_error_response(json_t *json_id, json_t *json_error, void *userdata)
{
	return NULL;
}

json_t *jsonrpc_request_error_response(json_t *json_id, json_t *json_error, void *userdata)
{
	/* json_error reference is stolen */

	json_t * response;

	if (json_id) {
		/** json_id may be freed with json_request and json_response **/
		json_incref(json_id);
	}

	response = json_pack("{s:s,s:o?,s:o?}",
		"jsonrpc", "2.0",
		"id", json_id,
		"error", json_error);
	return response;
}

static jsonrpc_error_response_t jsonrpc_error_response = jsonrpc_request_error_response;

json_t *jsonrpc_result_response(json_t *json_id, json_t *json_result)
{
	/*  json_result reference is stolen */

	json_t * response;

	/*  json_id shouldn't be NULL */
	if (json_id) {
		json_incref(json_id);
	} else {
		json_id = json_null();
	}

	json_result = json_result ? json_result : json_null();

	response = json_pack("{s:s,s:o,s:o}",
		"jsonrpc", "2.0",
		"id", json_id,
		"result", json_result);
	return response;
}

int jsonrpc_validate_request(json_t *json_request, const char **str_method, json_t **json_params, json_t **json_id, json_t **json_error)
{
	size_t flags = 0;
	json_error_t error;
	const char *str_version = NULL;
	int rc = 0;
	json_t *data = NULL;
	int valid_id = 0;
	json_t *json_result = NULL;

	*str_method = NULL;
	*json_params = NULL;
	*json_id = NULL;
	*json_error = NULL;

	rc = json_unpack_ex(json_request, &error, flags, "{s:s,s?s,s?o,s?o,s?o,s?o}",
		"jsonrpc", &str_version,
		"method", str_method,
		"params", json_params,
		"result", &json_result,
		"error", json_error,
		"id", json_id
	);

	if (rc==-1) {
		data = json_string(error.text);
		goto invalid;
	}

	if (json_error) {
		/** json_error may be freed by json_request and json_response **/
		json_incref(*json_error);
	}
	if (*json_id) {
		if (!json_is_string(*json_id) && !json_is_number(*json_id) && !json_is_null(*json_id)) {
			data = json_string("\"id\" MUST contain a String, Number, or NULL value if included");
			goto invalid;
		}
		if (json_is_null(*json_id))
			rc = TYPE_RECEIVE_NOTIFICATION;
		else
			rc = TYPE_RECEIVE_REQUEST;
	}
	else {
		rc = TYPE_RECEIVE_NOTIFICATION;
	}

	/*  Note that we only return json_id in the error response after we have established that it is jsonrpc/2.0 compliant */
	/*  otherwise we would be returning a non-compliant response ourselves! */
	valid_id = 1;

	if (*json_params && str_method) {
		if (!json_is_array(*json_params) && !json_is_object(*json_params) && !json_is_null(*json_params)) {
			data = json_string("\"params\" MUST be Array or Object if included");
			goto invalid;
		}
	}
	else if (*str_method == NULL) {
		rc = TYPE_RECEIVE_RESPONSE;
		if (json_result) {
			if (!json_is_array(json_result) && !json_is_object(json_result) && !json_is_null(json_result)) {
				json_decref(json_result);
				data = json_string("\"result\" MUST be Array or Object if included");
				goto invalid;
			}
			*json_params = json_result;
		}
	}

	if (0!=strcmp(str_version, "2.0")) {
		data = json_string("\"jsonrpc\" MUST be exactly \"2.0\"");
		goto invalid;
	}

	return rc;

invalid:
	if (!valid_id)
		*json_id = NULL;
	*json_error =
		jsonrpc_error_object_predefined(JSONRPC_INVALID_REQUEST, data);
	return rc;
}

json_t *jsonrpc_validate_params(json_t *json_params, const char *params_spec)
{
	json_t *data = NULL;

	if (params_spec == NULL || strlen(params_spec)==0) {	/*  empty string means no arguments */
		if (!json_params || json_is_null(json_params)) {
			/*  no params field: OK */
		} else if (json_is_array(json_params) && json_array_size(json_params)==0) {
			/*  an empty Array: OK */
		} else {
			data = json_string("method takes no arguments");
		}
	} else if (!json_params) {		/*  non-empty string but no params field */
		data = json_string("method takes arguments but params field missing");
	} else {					/*  non-empty string and have params field */
		size_t flags = JSON_VALIDATE_ONLY;
		json_error_t error;
		int rc = json_unpack_ex(json_params, &error, flags, params_spec);
		if (rc==-1) {
			data = json_string(error.text);
		}
	}

	return data ? jsonrpc_error_object_predefined(JSONRPC_INVALID_PARAMS, data) : NULL;
}

json_t *jsonrpc_handle_request_single(json_t *json_request,
	struct jsonrpc_method_entry_t method_table[],
	void *userdata)
{
	int rc;
	json_t *json_response = NULL;
	const char *str_method;
	json_t *json_params = NULL;
	json_t *json_id = NULL;
	json_t *json_result = NULL;
	json_t *json_error = NULL;
	int is_notification = 0;
	struct jsonrpc_method_entry_t *entry;

	rc = jsonrpc_validate_request(json_request, &str_method, &json_params, &json_id, &json_error);
	is_notification = (json_id==NULL);

	for (entry=method_table; entry->name!=NULL; entry++)
	{
		if ((str_method != NULL) && (0==strcmp(entry->name, str_method)) && (entry->type == rc))
		{
			break;
		}
		else if ((entry->type == TYPE_RECEIVE_RESPONSE) && (json_id != NULL))
		{
			unsigned long id = json_integer_value(json_id);
			struct jsonrpc_method_entry_t *it = entry;
			while (it->next) {
				if (it->next->id == id)
					break;
				it = it->next;
			}
			if (it->next != NULL)
			{
				struct jsonrpc_method_entry_t *old;
				old = it->next;
				if (it->next != NULL) {
					it->next = it->next->next;
				}
				free(old);
				break;
			}
		}
	}

	if (json_error) {
		json_response = jsonrpc_error_response(json_id, json_error, userdata);
		goto done;
	}

	if (entry == NULL || entry->name == NULL) {
		json_response = jsonrpc_error_response(json_id,
				jsonrpc_error_object_predefined(JSONRPC_METHOD_NOT_FOUND, NULL), userdata);
		goto done;
	}

	if (entry->params_spec) {
		json_t *error_obj = jsonrpc_validate_params(json_params, entry->params_spec);
		if (error_obj) {
			json_response = jsonrpc_error_response(json_id, error_obj, userdata);
			goto done;
		}
	}

	if (entry->funcptr != NULL) {
		rc = entry->funcptr(json_params, &json_result, userdata);
		if (rc==0) {
			json_response = jsonrpc_result_response(json_id, json_result);
		} else {
			json_response = jsonrpc_error_response(json_id, json_result, userdata);
		}
	}

	rc = entry->type;
done:
	switch (rc) {
		case TYPE_SEND_RESPONSE:
		break;
		case TYPE_RECEIVE_RESPONSE:
		case TYPE_RECEIVE_NOTIFICATION:
		default:
		if (json_result)
			json_decref(json_result);
		if (json_response)
			json_decref(json_response);
		json_response = NULL;
		break;
	}

	return json_response;
}

json_t *jsonrpc_jresponse(json_t *json_request,
	struct jsonrpc_method_entry_t method_table[],
	void *userdata)
{
	json_t *json_response;
	if (!json_request) {
		json_response = jsonrpc_error_response(NULL,
				jsonrpc_error_object_predefined(JSONRPC_PARSE_ERROR, NULL), userdata);
	} else if json_is_array(json_request) {
		size_t len = json_array_size(json_request);
		if (len==0) {
			json_response = jsonrpc_error_response(NULL,
					jsonrpc_error_object_predefined(JSONRPC_INVALID_REQUEST, NULL), userdata);
		} else {
			size_t k;
			json_response = NULL;
			for (k=0; k < len; k++) {
				json_t *req = json_array_get(json_request, k);
				json_t *rep = jsonrpc_handle_request_single(req, method_table, userdata);
				if (rep) {
					if (!json_response)
						json_response = json_array();
					json_array_append_new(json_response, rep);
				}
			}
		}
	} else {
		json_response = jsonrpc_handle_request_single(json_request, method_table, userdata);
	}

	return json_response;
}

char *jsonrpc_handler(const char *input, size_t input_len,
	struct jsonrpc_method_entry_t method_table[], void *userdata)
{
	json_t *request, *response;
	json_error_t error;
	char *output = NULL;

	request = json_loadb(input, input_len, 0, &error);

	if (!request) {
		fprintf(stderr, "Syntax error: line %d col %d: %s\n", error.line, error.column, error.text);
	}
	response = jsonrpc_jresponse(request, method_table, userdata);
	if (response) {
		output = json_dumps(response, JSON_INDENT(2));
		json_decref(response);
	}

	if (request) {
		json_decref(request);
	}

	return output;
}

json_t *jsonrpc_jrequest(const char *method,
		struct jsonrpc_method_entry_t method_table[], char *userdata,
		unsigned long *pid)
{
	struct jsonrpc_method_entry_t *entry;
	json_t *request = NULL;
	json_t *params = NULL;
	unsigned long id;

	for (entry=method_table; entry->name!=NULL; entry++)
	{
		if (0==strcmp(entry->name, method) && (entry->type == 'r' || entry->type == 'n')) {
			break;
		}
	}

	if (entry == NULL || entry->name==NULL) {
		goto done;
	}

	if (entry->funcptr) {
		entry->funcptr(NULL, &params, userdata);
	}
	if (entry->params_spec) {
		json_t *error_obj = jsonrpc_validate_params(params, entry->params_spec);
		if (error_obj) {
			goto done;
		}
	}

	if (entry->type == 'r')	{
		srandom(time(NULL));
		id = random();
		/**
		 * because we need to keep the id for the response
		 * we create a new entry into the table which will be destroy on the response
		 */
		struct jsonrpc_method_entry_t *entry;
		for (entry=method_table; entry->name!=NULL; entry++)
		{
			if (0==strcmp(entry->name, method) && entry->type == TYPE_SEND_REQUEST) {
				struct jsonrpc_method_entry_t *new = calloc(1, sizeof(*new));
				if (new) {
					memcpy(new, entry, sizeof(*new));
					new->id = id;
					entry->next = new;
				}
			}
		}
		if (pid != NULL)
			*pid = id;
		json_error_t error;
		request = json_pack_ex(&error, 0, "{s:s,s:i,s:s,s:o?}",
			"jsonrpc", "2.0",
			"id", id,
			"method", method,
			"params", params);
	} else {
		json_error_t error;
		request = json_pack_ex(&error, 0, "{s:s,s:s,s:o}",
			"jsonrpc", "2.0",
			"method", method,
			"params", params);
	}
	return request;
done:
	return NULL;
}

char *jsonrpc_request(const char *method, int methodlen,
		struct jsonrpc_method_entry_t method_table[], char *userdata,
		unsigned long *pid)
{
	char *output = NULL;

	json_t *request = jsonrpc_jrequest( method, method_table, userdata, pid);
	if (request)
		output = json_dumps(request, 0);

	json_decref(request);
	return output;
}

int jsonrpc_stringify(json_t *jsonrpc, char *output, size_t output_len)
{
	int ret;
	ret = json_dumpb(jsonrpc, output, output_len, 0);
	return ret;
}

void jsonrpc_set_errorhandler(jsonrpc_error_response_t error_response)
{
	if (error_response == (void *)ERRORHANDLER_REQUEST)
		jsonrpc_error_response = jsonrpc_request_error_response;
	else if (error_response != NULL)
		jsonrpc_error_response = error_response;
	else
		jsonrpc_error_response = jsonrpc_ignore_error_response;
}
