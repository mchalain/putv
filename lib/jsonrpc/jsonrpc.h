#ifndef __JSONRPC_H__
#define __JSONRPC_H__
#include <jansson.h>

#define JSONRPC_PARSE_ERROR -32700
#define JSONRPC_INVALID_REQUEST -32600
#define JSONRPC_METHOD_NOT_FOUND -32601
#define JSONRPC_INVALID_PARAMS -32602
#define JSONRPC_INTERNAL_ERROR -32603

typedef int (*jsonrpc_method_prototype)(json_t *json_params, json_t **result, void *userdata);

/**
 * description of the methods:
 * 
 * Server:
 *  The server commonly uses 'jsonrpc_handler' to receive requests and send responses.
 *  It may uses to 'jsonrpc_request' to send notification.
 * Client:
 *  The client uses 'jsonrpc_request' to send requests (and rarely notifications) and
 *  'jsonrpc_handler' to receive responses and notifications. When the client send a
 *  request, it has to use the same method table to receive the response. The table
 *  uses to send request must define the answer callback for the same method.
 * The application may not send and receive request with the same method table.
 * @param type : The meaning depends if the application is client or server:
 *  when it is used with jsonrpc_handler
 *   - 'r': request the method parses the request and returns the response.
 *   - 'n': notification the method parses the notification.
 *   - 'a': answer the method parses the response to a previous request, sended with jsonrpc_request.
 *  when it is used with jsonrpc_request
 *   - 'r': request the method generates and returns request.
 *   - 'n': notification the method generates and returns notification.
 * @param name : method name
 * @param funcptr : method callback
 * @param params_spec : parameter for all method callbacks
 * @param id : internal use for answer.
 * @param next : internal use for answer.
 *
 * @example :
 *  void *method_params;
 *  jsonrpc_method_entry_t table[] =
 *  {
 *   {'r',"hello", method_hello},
 *   {'a',"hello", method_ahello},
 * 	 {0, NULL},
 *  };
 *  ...
 *  char *output = jsonrpc_request('hello', 5, table, method_params, NULL);
 *    => metho_hello(unused, result, method_params)
 *       {
 *          result = json_pack("{ss}", "name", "client");
 *          return 0;
 *       }
 *  send(sock, output, strlen(output) + 1);
 *  while (run)
 *  {
 *    input len = recv(sock, input, sizeof(input));
 *    jsonrpc_handler(input, inputlen, table, method_params);
 *     => method_ahello(json_params, result, method_params)
 *        {
 *          json_unpack(json_params, "{si}", "clientid", &method_params->clientid);
 *          return 0;
 *        }
 *  };
 */
struct jsonrpc_method_entry_t
{
	const char type; /* 'r'=request , 'n'=notification */
	const char *name;
	jsonrpc_method_prototype funcptr;
	const char *params_spec;
	unsigned long id;
	struct jsonrpc_method_entry_t *next;
};

/**
 * @brief generate a response to a jsonRPC
 * 
 * This function unpack a string to get informations about the RPC.
 * The requested method is searched into the method_table and
 * the associated function is called.
 * The function continues with the response generation.
 *
 * @param input			string corresponding to a jsonRPC.
 * @param input_len		length of the input string.
 * @param method_table	table of methods to manage.
 * @param userdata		argument fot the callback associated to every methods.
 * 
 * @return the response string to send. The "answer" and the "notification"
 * don't generate string.
 */
char *jsonrpc_handler(const char *input, size_t input_len,
	struct jsonrpc_method_entry_t method_table[],
	void *userdata);
json_t *jsonrpc_jresponse(json_t *json_request,
	struct jsonrpc_method_entry_t method_table[],
	void *userdata);

/**
 * @brief generate a jsonRPC string
 *
 * This function search the method into the method_table.
 * It can generate only "request" or "notification".
 * To send a request the user needs to manage the answer.
 * This function will generate the request. jsonrpc_handler will manage
 * the response. To do that the method_table must contain an entry for
 * the request ("r" type) and an entry for the answer ("a" type).
 * 
 * @param method		name of the RPC to generate.
 * @param methodlen		length of the string.
 * @param method_table	table of methods to manage (same of method_handler).
 * @param userdata		argument fot the callback associated to every methods.
 * @param pid			id of the generated request.
 */
char *jsonrpc_request(const char *method, int methodlen,
		struct jsonrpc_method_entry_t method_table[],
		char *userdata, unsigned long *pid);
json_t *jsonrpc_jrequest(const char *method,
		struct jsonrpc_method_entry_t method_table[],
		char *userdata, unsigned long *pid);


json_t *jsonrpc_error_object(int code, const char *message, json_t *data);
json_t *jsonrpc_error_object_predefined(int code, json_t *data);

typedef json_t *(*jsonrpc_error_response_t)(json_t *json_id, json_t *json_error);
#define ERRORHANDLER_REQUEST 1
#define ERRORHANDLER_IGNORE 0
void jsonrpc_set_errorhandler(jsonrpc_error_response_t error_response);

#endif
