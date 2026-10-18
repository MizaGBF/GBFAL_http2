#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <structmember.h>
#include <stdbool.h>
#include <pthread.h>
#include <curl/curl.h>
#include <strings.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
	#include <windows.h>
	#define sleep_ms(ms) Sleep(ms)
#else
	#include <unistd.h>
	#define sleep_ms(ms) sleep((ms) / 1000)
#endif

#define VERSION_STRING "0.2"

// Forward declarations
static PyObject* ext_version(PyObject *self, PyObject *args);
static PyObject* ext_init(PyObject *self, PyObject *args);
static PyObject* ext_close(PyObject *self, PyObject *args);
static PyObject* ext_running(PyObject *self, PyObject *args);
static PyObject* ext_queue_request(PyObject *self, PyObject *args);
static void* network_thread_func(void *arg);

typedef enum {
	METHOD_HEAD = 0,
	METHOD_GET
} RequestMethod;

typedef struct RequestNode {
	struct RequestNode* next;
	char* url;
	RequestMethod method;
	PyObject* future;
	PyObject* loop;
} RequestNode;

typedef struct {
	RequestNode* head;
	RequestNode* tail;
	pthread_mutex_t lock;
} RequestQueue;

typedef struct RequestContext {
	char *url;
	RequestMethod method;
	PyObject *future;
	PyObject *loop;

	char *body_buf;
	size_t body_len;
	size_t body_cap;

	char *content_length_buf;
	size_t content_length_len;
	size_t content_length_cap;
} RequestContext;

// global variables
static pthread_t g_worker;
static CURLM* g_multi_handle = NULL;
static RequestQueue g_queue;
static volatile bool g_running = false;
static int g_concurrency = 90;
static volatile size_t g_active_count = 0;
static char* g_user_agent = NULL;

static PyTypeObject ResponseType;

static PyStructSequence_Field ResponseFields[] = {
	{"status", "HTTP status code"},
	{"body", "Response body or header bytes"},
	{NULL}
};

static PyStructSequence_Desc ResponseDesc = {
	"Response",
	"HTTP Response object containing status and body",
	ResponseFields,
	2
};

// Queue operations
static void queue_init(void)
{
	g_queue.head = g_queue.tail = NULL;
	pthread_mutex_init(&g_queue.lock, NULL);
}

static bool queue_push(const char *url, RequestMethod method, PyObject *future, PyObject *loop)
{
	RequestNode* new_node = (RequestNode*)malloc(sizeof(RequestNode));
	if(!new_node)
	{
		return false;
	}

	new_node->url = strdup(url);
	if(!new_node->url)
	{
		free(new_node);
		return false;
	}
	new_node->method = method;
	new_node->future = future;
	new_node->loop = loop;
	new_node->next = NULL;

	pthread_mutex_lock(&g_queue.lock);
	if (g_queue.tail)
	{
		g_queue.tail->next = new_node;
		g_queue.tail = new_node;
	}
	else
	{
		g_queue.head = g_queue.tail = new_node;
	}
	pthread_mutex_unlock(&g_queue.lock);

	if(g_multi_handle)
	{
		curl_multi_wakeup(g_multi_handle);
	}
	return true;
}

static bool queue_pop(RequestNode* buffer)
{
	pthread_mutex_lock(&g_queue.lock);
	if(!g_queue.head)
	{
		pthread_mutex_unlock(&g_queue.lock);
		return false;
	}
	RequestNode* temp = g_queue.head;
	*buffer = *temp;
	g_queue.head = g_queue.head->next;
	if(!g_queue.head)
	{
		g_queue.tail = NULL;
	}
	pthread_mutex_unlock(&g_queue.lock);
	free(temp);
	return true;
}

static void queue_clear(void)
{
	RequestNode node;
	bool acquire_gil = (PyGILState_Check() == 0);
	PyGILState_STATE state;
	if(acquire_gil)
	{
		state = PyGILState_Ensure();
	}
	while(queue_pop(&node))
	{
		if(node.url)
		{
			free(node.url);
		}
		if(node.future)
		{
			Py_DECREF(node.future);
		}
		if(node.loop)
		{
			Py_DECREF(node.loop);
		}
	}
	if(acquire_gil)
	{
		PyGILState_Release(state);
	}
}

static void queue_destroy(void)
{
	queue_clear();
	pthread_mutex_destroy(&g_queue.lock);
}

// cURL callbacks
static size_t write_body_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	size_t total = size * nmemb;
	if(total == 0)
	{
		return 0;
	}
	RequestContext *ctx = (RequestContext*)userdata;
	if(ctx->body_len + total > ctx->body_cap)
	{
		size_t new_cap = (ctx->body_cap == 0) ? 4096 : ctx->body_cap * 2;
		while(new_cap < ctx->body_len + total)
		{
			new_cap *= 2;
		}
		char *new_buf = realloc(ctx->body_buf, new_cap);
		if(!new_buf)
		{
			return 0;
		}
		ctx->body_buf = new_buf;
		ctx->body_cap = new_cap;
	}
	memcpy(ctx->body_buf + ctx->body_len, ptr, total);
	ctx->body_len += total;
	return total;
}

static size_t header_cb(char *buffer, size_t size, size_t nitems, void *userdata)
{
	size_t total = size * nitems;
	RequestContext *ctx = (RequestContext*)userdata;

	if(total >= 5 && strncasecmp(buffer, "http/", 5) == 0)
	{
		ctx->content_length_len = 0;
	}
	else if(total >= 15 && strncasecmp(buffer, "content-length:", 15) == 0)
	{
		const char *start = buffer + 15;
		const char *end = buffer + total;
		while(start < end && (*start == ' ' || *start == '\t')) start++;
		while(end > start && (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ' || end[-1] == '\t')) end--;
		size_t val_len = end - start;

		if(val_len + 1 > ctx->content_length_cap)
		{
			size_t new_cap = val_len + 16;
			char *new_buf = realloc(ctx->content_length_buf, new_cap);
			if (new_buf) {
				ctx->content_length_buf = new_buf;
				ctx->content_length_cap = new_cap;
			}
		}
		if(ctx->content_length_buf)
		{
			memcpy(ctx->content_length_buf, start, val_len);
			ctx->content_length_buf[val_len] = '\0';
			ctx->content_length_len = val_len;
		}
	}
	return total;
}

static void free_request_context(RequestContext *ctx)
{
	if(!ctx)
	{
		return;
	}
	if(ctx->url)
	{
		free(ctx->url);
	}
	if(ctx->body_buf)
	{
		free(ctx->body_buf);
	}
	if(ctx->content_length_buf)
	{
		free(ctx->content_length_buf);
	}
	free(ctx);
}

static void on_request_complete(RequestContext *ctx, int status_code)
{
	PyGILState_STATE state = PyGILState_Ensure();

	PyObject *body_bytes = NULL;
	if(ctx->method == METHOD_GET)
	{
		body_bytes = PyBytes_FromStringAndSize(ctx->body_buf ? ctx->body_buf : "", ctx->body_len);
	}
	else
	{
		body_bytes = PyBytes_FromStringAndSize(ctx->content_length_buf ? ctx->content_length_buf : "", ctx->content_length_len);
	}

	if(!body_bytes)
	{
		PyErr_Clear();
		body_bytes = PyBytes_FromStringAndSize("", 0);
	}

	PyObject *status_obj = PyLong_FromLong(status_code);

	PyObject *resp = PyStructSequence_New(&ResponseType);
	if(resp)
	{
		PyStructSequence_SetItem(resp, 0, status_obj);
		PyStructSequence_SetItem(resp, 1, body_bytes);

		PyObject *set_result_method = PyObject_GetAttrString(ctx->future, "set_result");
		if(set_result_method)
		{
			PyObject *res = PyObject_CallMethod(ctx->loop, "call_soon_threadsafe", "OO", set_result_method, resp);
			Py_XDECREF(res);
			Py_DECREF(set_result_method);
		}
		Py_DECREF(resp);
	}
	else
	{
		Py_XDECREF(status_obj);
		Py_XDECREF(body_bytes);
	}

	Py_DECREF(ctx->future);
	Py_DECREF(ctx->loop);

	PyGILState_Release(state);
}

static void* network_thread_func(void *arg)
{
	(void)arg;

	while(g_running || g_active_count > 0)
	{
		while(g_running && g_active_count < (size_t)g_concurrency)
		{
			RequestNode node;
			if(!queue_pop(&node))
			{
				break;
			}

			RequestContext *ctx = (RequestContext*)calloc(1, sizeof(RequestContext));
			if(!ctx)
			{
				PyGILState_STATE state = PyGILState_Ensure();
				PyObject *err = PyObject_CallMethod(node.future, "set_exception", "O", PyExc_MemoryError);
				Py_XDECREF(err);
				Py_DECREF(node.future);
				Py_DECREF(node.loop);
				PyGILState_Release(state);
				free(node.url);
				continue;
			}

			ctx->url = node.url;
			ctx->method = node.method;
			ctx->future = node.future;
			ctx->loop = node.loop;

			CURL *easy = curl_easy_init();
			if(!easy)
			{
				PyGILState_STATE state = PyGILState_Ensure();
				PyObject *err = PyObject_CallMethod(node.future, "set_exception", "O", PyExc_RuntimeError);
				Py_XDECREF(err);
				Py_DECREF(ctx->future);
				Py_DECREF(ctx->loop);
				PyGILState_Release(state);
				free(ctx->url);
				free(ctx);
				continue;
			}

			curl_easy_setopt(easy, CURLOPT_URL, ctx->url);
			curl_easy_setopt(easy, CURLOPT_USERAGENT, g_user_agent);
			curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);
			curl_easy_setopt(easy, CURLOPT_PIPEWAIT, 1L);
			curl_easy_setopt(easy, CURLOPT_PRIVATE, ctx);

			if(ctx->method == METHOD_GET)
			{
				curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L);
				curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "gzip");
				curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_body_cb);
				curl_easy_setopt(easy, CURLOPT_WRITEDATA, ctx);
			}
			else
			{
				curl_easy_setopt(easy, CURLOPT_NOBODY, 1L);
				curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, header_cb);
				curl_easy_setopt(easy, CURLOPT_HEADERDATA, ctx);
			}

			curl_multi_add_handle(g_multi_handle, easy);
			g_active_count++;
		}

		int still_running = 0;
		curl_multi_perform(g_multi_handle, &still_running);

		CURLMsg *msg;
		int msgs_left = 0;
		while((msg = curl_multi_info_read(g_multi_handle, &msgs_left)))
		{
			if(msg->msg == CURLMSG_DONE)
			{
				CURL *easy = msg->easy_handle;
				RequestContext *ctx = NULL;
				curl_easy_getinfo(easy, CURLINFO_PRIVATE, &ctx);

				bool network_error = (
					msg->data.result == CURLE_COULDNT_CONNECT
					|| msg->data.result == CURLE_OPERATION_TIMEDOUT
					|| msg->data.result == CURLE_SEND_ERROR
					|| msg->data.result == CURLE_RECV_ERROR
					|| msg->data.result == CURLE_GOT_NOTHING
					|| msg->data.result == CURLE_HTTP2
				);

				if(g_running && network_error)
				{
					// Reset context buffers and retry transfer
					ctx->body_len = 0;
					ctx->content_length_len = 0;

					curl_multi_remove_handle(g_multi_handle, easy);
					sleep_ms(100);
					curl_multi_add_handle(g_multi_handle, easy);
				}
				else if(msg->data.result == CURLE_OK)
				{
					long http_code = 0;
					curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &http_code);

					on_request_complete(ctx, (int)http_code);

					curl_multi_remove_handle(g_multi_handle, easy);
					curl_easy_cleanup(easy);
					free_request_context(ctx);
					g_active_count--;
				}
				else
				{
					PyGILState_STATE state = PyGILState_Ensure();
					PyObject *err = PyObject_CallMethod(ctx->future, "set_exception", "O", PyExc_ConnectionError);
					Py_XDECREF(err);
					PyGILState_Release(state);

					curl_multi_remove_handle(g_multi_handle, easy);
					curl_easy_cleanup(easy);
					free_request_context(ctx);
					g_active_count--;
				}
			}
		}

		if(g_active_count > 0 || (g_running && g_queue.head != NULL))
		{
			int numfds = 0;
			curl_multi_poll(g_multi_handle, NULL, 0, 2, &numfds);
		}
		else if(g_running)
		{
			int numfds = 0;
			curl_multi_poll(g_multi_handle, NULL, 0, 2, &numfds);
		}
	}

	return NULL;
}

static PyObject* ext_version(PyObject *self, PyObject *args)
{
	(void)self;
	if(!PyArg_ParseTuple(args, ""))
	{
		return NULL;
	}
	return Py_BuildValue("s", VERSION_STRING);
}

static PyObject* ext_init(PyObject *self, PyObject *args)
{
	(void)self;
	if(g_running)
	{
		PyErr_SetString(PyExc_RuntimeError, "Worker is already running");
		return NULL;
	}
	int concurrency;
	const char *user_agent = NULL;
	if(!PyArg_ParseTuple(args, "is", &concurrency, &user_agent))
	{
		return NULL;
	}
	if(concurrency < 1)
	{
		PyErr_SetString(PyExc_ValueError, "Concurrency must be greater than 0");
		return NULL;
	}

	// Duplicate the string since PyArg_ParseTuple's "s" points to temporary internal Python object memory
	free(g_user_agent);
	g_user_agent = strdup(user_agent);
	if(!g_user_agent)
	{
		PyErr_SetString(PyExc_MemoryError, "Failed to allocate memory for user agent");
		return NULL;
	}
	g_concurrency = concurrency;
	curl_global_init(CURL_GLOBAL_DEFAULT);
	queue_init();
	g_multi_handle = curl_multi_init();
	if(!g_multi_handle)
	{
		PyErr_SetString(PyExc_RuntimeError, "Failed to initialize curl_multi");
		free(g_user_agent);
		g_user_agent = NULL;
		return NULL;
	}
	curl_multi_setopt(g_multi_handle, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);

	g_running = true;
	g_active_count = 0;

	if(pthread_create(&g_worker, NULL, network_thread_func, NULL) != 0)
	{
		g_running = false;
		curl_multi_cleanup(g_multi_handle);
		g_multi_handle = NULL;
		free(g_user_agent);
		g_user_agent = NULL;
		PyErr_SetString(PyExc_RuntimeError, "Failed to create worker thread");
		return NULL;
	}

	Py_RETURN_NONE;
}

static PyObject* ext_close(PyObject *self, PyObject *args)
{
	(void)self;
	(void)args;
	if(!g_running)
	{
		Py_RETURN_NONE;
	}

	g_running = false;

	queue_clear();

	if(g_multi_handle)
	{
		curl_multi_wakeup(g_multi_handle);
	}

	Py_BEGIN_ALLOW_THREADS
	pthread_join(g_worker, NULL);
	Py_END_ALLOW_THREADS

	if(g_multi_handle)
	{
		curl_multi_cleanup(g_multi_handle);
		g_multi_handle = NULL;
	}
	queue_destroy();
	curl_global_cleanup();
	free(g_user_agent);

	Py_RETURN_NONE;
}

static PyObject* ext_running(PyObject *self, PyObject *args)
{
	(void)self;
	(void)args;
	if(g_running)
	{
		Py_RETURN_TRUE;
	}
	else
	{
		Py_RETURN_FALSE;
	}
}

static PyObject* ext_queue_request(PyObject *self, PyObject *args)
{
	(void)self;
	if(!g_running)
	{
		PyErr_SetString(PyExc_RuntimeError, "Worker thread is not running");
		return NULL;
	}

	const char *url;
	const char *method_str;

	if(!PyArg_ParseTuple(args, "ss", &url, &method_str))
	{
		return NULL;
	}

	RequestMethod method;
	if(strcasecmp(method_str, "GET") == 0)
	{
		method = METHOD_GET;
	}
	else if (strcasecmp(method_str, "HEAD") == 0)
	{
		method = METHOD_HEAD;
	}
	else
	{
		PyErr_SetString(PyExc_ValueError, "Method must be 'GET' or 'HEAD'");
		return NULL;
	}

	PyObject *asyncio = PyImport_ImportModule("asyncio");
	if(!asyncio)
	{
		return NULL;
	}

	PyObject *loop = PyObject_CallMethod(asyncio, "get_running_loop", NULL);
	Py_DECREF(asyncio);
	if(!loop)
	{
		return NULL;
	}

	PyObject *future = PyObject_CallMethod(loop, "create_future", NULL);
	if(!future)
	{
		Py_DECREF(loop);
		return NULL;
	}

	Py_INCREF(future);
	Py_INCREF(loop);

	if(!queue_push(url, method, future, loop))
	{
		Py_DECREF(future); // Release C queue reference
		Py_DECREF(loop);   // Release C queue reference
		Py_DECREF(future); // Release local create_future reference
		Py_DECREF(loop);   // Release local loop reference
		PyErr_SetString(PyExc_MemoryError, "Failed to allocate memory for request node");
		return NULL;
	}
	Py_DECREF(loop);
	return future;
}

static PyMethodDef http2Methods[] = {
	{"version", ext_version, METH_VARARGS, "Return the version string."},
	{"init", ext_init, METH_VARARGS, "Initialize worker thread with concurrency limit."},
	{"close", ext_close, METH_NOARGS, "Shutdown worker thread."},
	{"running", ext_running, METH_NOARGS, "Check if worker thread is running."},
	{"queue_request", ext_queue_request, METH_VARARGS, "Queue an HTTP request (url, method)."},
	{NULL, NULL, 0, NULL}
};

static struct PyModuleDef http2module = {
	.m_base = PyModuleDef_HEAD_INIT,
	.m_name = "http2",
	.m_doc = "GBFAL updater libcurl HTTP/2 client",
	.m_size = -1,
	.m_methods = http2Methods,
	.m_slots = NULL,
	.m_traverse = NULL,
	.m_clear = NULL,
	.m_free = NULL
};

PyMODINIT_FUNC PyInit_http2(void)
{
	PyObject *m = PyModule_Create(&http2module);
	if(!m)
	{
		return NULL;
	}

	if(ResponseType.tp_name == NULL)
	{
		if(PyStructSequence_InitType2(&ResponseType, &ResponseDesc) < 0)
		{
			Py_DECREF(m);
			return NULL;
		}
	}
	Py_INCREF((PyObject *)&ResponseType);
	PyModule_AddObject(m, "Response", (PyObject *)&ResponseType);

	return m;
}
