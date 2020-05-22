/**
 * TeaJS - apache module. 
 * Extends TeaJS_App by adding customized initialization and (std)IO routines
 */
#include <stdlib.h>
#include <string.h>

#include "httpd.h"
#include "http_config.h"
#include "http_log.h"
#include "http_protocol.h"
#include "util_script.h"
#include "apr_pools.h"

#include "apr_base64.h"
#include "apr_strings.h"


#include "app.h"
#include "path.h"
#include "macros.h"

typedef struct {
	const char * config;
} teajs_config;

/**
 * First module declaration 
 */
extern "C" module AP_MODULE_DECLARE_DATA teajs_module; 

#ifdef APLOG_USE_MODULE
APLOG_USE_MODULE(teajs);
#endif

class TeaJS_Module : public TeaJS_App {
public:
	request_rec * request;
	
	size_t write(const char * data, size_t size) {
		return ap_rwrite(data, size, this->request);
	}
	size_t flush() {
		return ap_rflush(this->request);
	}
	
	void error(const char * data) {
	#ifdef APLOG_USE_MODULE
		ap_log_rerror(__FILE__, __LINE__, APLOG_MODULE_INDEX, APLOG_ERR, 0, this->request, "%s", data);
	#else
		ap_log_rerror(__FILE__, __LINE__, APLOG_ERR, 0, this->request, "%s", data);
	#endif
	}

	/** 
	 * Remember apache request structure and continue as usually
	 */
	void execute(request_rec * request, char ** envp) {
		this->request = request;
		this->mainfile = std::string(request->filename);
		int chdir_result = path_chdir(path_dirname(this->mainfile));
		if (chdir_result == -1) { return; }
		TeaJS_App::execute(envp);
	}
	
	void init(teajs_config * cfg) { 
		TeaJS_App::init();
		this->cfgfile = cfg->config;
	}

	/**
	 * Set a HTTP response header
	 * @param {char *} name
	 * @param {char *} value
	 */
	void header(const char * name, const char * value) {
		if (strcasecmp(name, "content-type") == 0) {
			char * ct =  (char *) apr_palloc(request->pool, strlen(value)+1);
			strcpy(ct, value);
			this->request->content_type = ct;
		} else if (strcasecmp(name, "status") == 0) {
			char * line = (char *) apr_palloc(request->pool, strlen(value)+1);
			strcpy(line, value);
			this->request->status_line = line;
			this->request->status = atoi(value);
		} else {
			char * n = (char *) apr_palloc(request->pool, strlen(name)+1);
			char * v = (char *) apr_palloc(request->pool, strlen(value)+1);
			strcpy(n, name);
			strcpy(v, value);
			apr_table_addn(this->request->headers_out, n, v);
		}
	}

protected:
	void prepare(char ** envp);

private:

	const char * instanceType() {
		return "module";
	}

	const char * executableName() {
		return "?";
	}
};

JS_METHOD(_read) {
	TeaJS_Module * app = (TeaJS_Module *) APP_PTR;
	if (args.Length() < 1) { JS_TYPE_ERROR("Invalid call format. Use 'apache.read(amount)'"); return; }
	size_t count = args[0]->IntegerValue();
	
	char * destination = new char[count];
	
	size_t read = 0;
	long part = 0;
	do {
		part = ap_get_client_block(app->request, destination+read, count-read);
		if (part<0) { break; }
		read += part;
	} while (part>0 && read<count);
	
	v8::Handle<v8::Value> result = JS_BUFFER(destination, read);
	delete[] destination;
	
	args.GetReturnValue().Set(result);
}

JS_METHOD(_flush) {
	TeaJS_Module * app = (TeaJS_Module *) APP_PTR;

	size_t result;
	result = app->flush();
	args.GetReturnValue().Set(JS_INT(result));
}
JS_METHOD(_write) {
	TeaJS_Module * app = (TeaJS_Module *) APP_PTR;
	if (args.Length() < 1) { JS_TYPE_ERROR("Invalid call format. Use 'apache.write(data)'"); return; }

	size_t result;
	if (IS_BUFFER(args[0])) {
		size_t size = 0;
		char * data = JS_BUFFER_TO_CHAR(args[0], &size);
		result = app->write(data, size);
	} else {
		v8::String::Utf8Value str(args[0]);
		result = app->write(*str, str.length());
	}
	args.GetReturnValue().Set(JS_INT(result));
}

JS_METHOD(_error) {
	TeaJS_Module * app = (TeaJS_Module *) APP_PTR;
	v8::String::Utf8Value error(args[0]);
	app->error(*error);
	args.GetReturnValue().SetUndefined();
}

JS_METHOD(_header) {
	TeaJS_Module * app = (TeaJS_Module *) APP_PTR;
	v8::String::Utf8Value name(args[0]);
	v8::String::Utf8Value value(args[1]);
	app->header(*name, *value);
	args.GetReturnValue().SetUndefined();
}

void TeaJS_Module::prepare(char ** envp) {
	TeaJS_App::prepare(envp);

	v8::HandleScope handle_scope(JS_ISOLATE);
	v8::Handle<v8::Object> g = JS_GLOBAL;
	v8::Handle<v8::Object> apache = v8::Object::New(JS_ISOLATE);
	g->Set(JS_STR("apache"), apache);
	apache->Set(JS_STR("header"), v8::FunctionTemplate::New(JS_ISOLATE, _header)->GetFunction());
	apache->Set(JS_STR("read"), v8::FunctionTemplate::New(JS_ISOLATE, _read)->GetFunction());
	apache->Set(JS_STR("write"), v8::FunctionTemplate::New(JS_ISOLATE, _write)->GetFunction());
	apache->Set(JS_STR("flush"), v8::FunctionTemplate::New(JS_ISOLATE, _flush)->GetFunction());
	apache->Set(JS_STR("error"), v8::FunctionTemplate::New(JS_ISOLATE, _error)->GetFunction());
}

#ifdef REUSE_CONTEXT
TeaJS_Module app;
#endif

/**
 * This is called from Apache every time request arrives
 */
static int mod_teajs_handler(request_rec *r) {
	const apr_array_header_t *arr;
	const apr_table_entry_t *elts;

	if (!r->handler || strcmp(r->handler, "teajs-script")) { return DECLINED; }

	ap_setup_client_block(r, REQUEST_CHUNKED_DECHUNK);
	ap_add_common_vars(r);
	ap_add_cgi_vars(r);

	if (r->headers_in) {
		const char *auth;
		auth = apr_table_get(r->headers_in, "Authorization");
		if (auth && auth[0] != 0 && strncmp(auth, "Basic ", 6) == 0) {
			
			char *user = NULL;
			char *pass = NULL;
			int length;

			user = (char *)apr_palloc(r->pool, apr_base64_decode_len(auth+6) + 1);
			length = apr_base64_decode(user, auth + 6);

			/* Null-terminate the string. */
			user[length] = '\0';
			
			if (user) {
				pass = strchr(user, ':');
				if (pass) {
					*pass++ = '\0';

					apr_table_setn(r->subprocess_env, "AUTH_USER", user);
					apr_table_setn(r->subprocess_env, "AUTH_PW", pass);
				}
		    }
		} 
    }
	
	/* extract the CGI environment  */
	arr = apr_table_elts(r->subprocess_env);
	elts = (const apr_table_entry_t*) arr->elts;
	
	char ** envp = new char *[arr->nelts + 1];
	envp[arr->nelts] = NULL;
	
	size_t size = 0;
	size_t len1 = 0;
	size_t len2 = 0;
	for (int i=0;i<arr->nelts;i++) {
		len1 = strlen(elts[i].key);
		len2 = strlen(elts[i].val);
		size = len1 + len2 + 2;
		
		envp[i] = new char[size];
		envp[i][size-1] = '\0';
		envp[i][len1] = '=';
		
		strncpy(envp[i], elts[i].key, len1);
		strncpy(&(envp[i][len1+1]), elts[i].val, len2);
	}

#ifndef REUSE_CONTEXT
	teajs_config * cfg = (teajs_config *) ap_get_module_config(r->server->module_config, &teajs_module);
	TeaJS_Module app;
	app.init(cfg);
#endif

	try {
		app.execute(r, envp);
	} catch (std::string e) {
		if (app.show_errors) {
			app.write(e.c_str(), e.length());
		} else {
			app.error(e.c_str());
		}
	}
	
	for (int i=0;i<arr->nelts;i++) {
		delete[] envp[i];
	}
	delete[] envp;
	
//  Ok is safer, because HTTP_INTERNAL_SERVER_ERROR overwrites any content already generated
//	if (result) {
//		return HTTP_INTERNAL_SERVER_ERROR;
//	} else {
		return OK;
//	}
}

/**
 * Module initialization 
 */
static int mod_teajs_init_handler(apr_pool_t *p, apr_pool_t *plog, apr_pool_t *ptemp, server_rec *s) {
	std::string version;
	version += "mod_teajs/";
	version += STRING(VERSION);
	ap_add_version_component(p, version.c_str());
#ifdef REUSE_CONTEXT
	teajs_config * cfg = (teajs_config *) ap_get_module_config(s->module_config, &teajs_module);
	app.init(cfg);
#endif
    return OK;
}

/**
 * Child initialization 
 * FIXME: what is this for?
 */
static void mod_teajs_child_init(apr_pool_t *p, server_rec *s) { 
}

/**
 * Register relevant hooks
 */
static void mod_teajs_register_hooks(apr_pool_t *p ) {
	ap_hook_handler(mod_teajs_handler, NULL, NULL, APR_HOOK_MIDDLE);
	ap_hook_post_config(mod_teajs_init_handler, NULL, NULL, APR_HOOK_MIDDLE);
	ap_hook_child_init(mod_teajs_child_init, NULL, NULL, APR_HOOK_MIDDLE);
}

/**
 * Create initial configuration values 
 */
static void * mod_teajs_create_config(apr_pool_t *p, server_rec *s) { 
	teajs_config * newcfg = (teajs_config *) apr_pcalloc(p, sizeof(teajs_config));
	newcfg->config = STRING(CONFIG_PATH);
	return (void *) newcfg;
}

/**
 * Callback executed for every configuration change 
 */
static const char * set_teajs_config(cmd_parms * parms, void * mconfig, const char * arg) { 
	teajs_config * cfg = (teajs_config *) ap_get_module_config(parms->server->module_config, &teajs_module);
	cfg->config = (char *) arg;
	return NULL;
}

typedef const char * (* CONFIG_HANDLER) ();
/* list of configurations */
static const command_rec mod_teajs_cmds[] = { 
	AP_INIT_TAKE1(
		"TeaJS_Config",
		(CONFIG_HANDLER) set_teajs_config,
		NULL,
		RSRC_CONF,
		"Path to TeaJS configuration file."
	),
	{NULL}
};

/**
 * Module (re-)declaration
 */
extern "C" { 
	module AP_MODULE_DECLARE_DATA teajs_module = {
		STANDARD20_MODULE_STUFF,
		NULL,
		NULL,
		mod_teajs_create_config,
		NULL,
		mod_teajs_cmds,
		mod_teajs_register_hooks,
	};
}
