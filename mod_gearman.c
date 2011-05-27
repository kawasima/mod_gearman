#include "httpd.h"
#include "http_config.h"
#include "http_protocol.h"
#include "http_log.h"
#include "ap_config.h"
#include "apr_uri.h"
#include <libgearman/gearman.h>

module AP_MODULE_DECLARE_DATA gearman_module;

typedef struct {
	int enabled;
	const char *host;
	int port;
} gearman_dir_config;

static void *create_gearman_dir_config(apr_pool_t *p, char *dummy)
{
	gearman_dir_config *dconf = (gearman_dir_config*) apr_palloc(p, sizeof(gearman_dir_config));
	dconf->enabled = 0;
	dconf->host = "localhost";
	dconf->port = 4730;

	return dconf;
}

static void *merge_gearman_dir_configs(apr_pool_t *p, void *basev, void *addv)
{
	gearman_dir_config *base = (gearman_dir_config*) basev;
	gearman_dir_config *add  = (gearman_dir_config*) addv;
	gearman_dir_config *new  = (gearman_dir_config*) apr_palloc(p, sizeof(gearman_dir_config));

	new->host = (add->host != NULL) ? add->host : base->host;
	new->port = (add->port != 0) ? add->port : base->port;
	new->enabled = add->enabled;

	return new;
}

static const command_rec gearman_cmds[] = 
{
	AP_INIT_FLAG("Gearman" , ap_set_flag_slot, 
		(void *)APR_OFFSETOF(gearman_dir_config, enabled), OR_ALL,
		"Gearman submit on"),
	AP_INIT_TAKE1("GearmanHost", ap_set_string_slot,
		(void *)APR_OFFSETOF(gearman_dir_config, host), OR_ALL,
		"Gearman server host"),
	AP_INIT_TAKE1("GearmanPort", ap_set_int_slot,
		(void *)APR_OFFSETOF(gearman_dir_config, port), OR_ALL,
		"Gearman server port"),
	{NULL}
};

static void submit_job(const char *job_name, char *uri, request_rec *r) {
	gearman_return_t ret;
	gearman_client_st client;
	char job_handle[GEARMAN_JOB_HANDLE_SIZE];
	size_t result_size;

	ap_log_rerror(APLOG_MARK, APLOG_ERR, r->status, r,
		"gearman submit job: method=%s, job_name=%s", r->method, job_name);
	gearman_dir_config *dconf = ap_get_module_config(r->per_dir_config, &gearman_module);
	if (gearman_client_create(&client) == NULL) {
		ap_log_rerror(APLOG_MARK, APLOG_ERR, r->status, r,
			"gearman client initialize failure.");
		return;
	}

	ret= gearman_client_add_server(&client, dconf->host, dconf->port);
	if (ret != GEARMAN_SUCCESS) {
		ap_log_rerror(APLOG_MARK, APLOG_ERR, r->status, r,
			"gearman client add server failure.");
		gearman_client_free(&client);
		return;
	}

	ret = gearman_client_do_background(&client, job_name, NULL,
		uri , strlen(uri), job_handle);
	if (ret != GEARMAN_SUCCESS) {
		ap_log_rerror(APLOG_MARK, APLOG_ERR, r->status, r,
			"gearman client add server failure.");
		gearman_client_free(&client);
		return;
	}
	gearman_client_free(&client);
}

static int gearman_request_hook(request_rec *r) {
	gearman_dir_config *dconf;
	apr_status_t rv;

	dconf = ap_get_module_config(r->per_dir_config, &gearman_module);

	if (!dconf->enabled) {
		return DECLINED;
	}

	if (r->main) {
		return DECLINED;
	}

	if (r->method_number == M_DELETE) {
		submit_job("unregister", r->uri, r);
	}
	else {
		return DECLINED;
	}

	return OK;
}

static int gearman_response_hook(ap_filter_t *f, apr_bucket_brigade *bb) {
    request_rec *r = f->r;

	// gearmanに投げる
	if(r->method_number == M_PUT) {
		submit_job("register", r->uri, r);
	}
	else if (r->method_number == M_COPY) {
		char *destination = NULL;
		apr_uri_t uri;
		destination = (char *)apr_table_get(r->headers_in, "Destination");
		if (destination == NULL) {
			return DECLINED;
		}
		if (apr_uri_parse(r->pool, destination, &uri) != APR_SUCCESS) {
			return DECLINED;
		}
		submit_job("register", uri.path, r);
	}
	else if (r->method_number == M_MOVE) {
		char *destination = NULL;
		apr_uri_t uri;
		destination = (char *)apr_table_get(r->headers_in, "Destination");
		if (destination == NULL) {
			return DECLINED;
		}
		if (apr_uri_parse(r->pool, destination, &uri) != APR_SUCCESS) {
			return DECLINED;
		}
		submit_job("unregister", r->uri, r);
		submit_job("register", uri.path, r);
	}
    return ap_pass_brigade(f->next, bb);
}

static void gearman_register_hooks(apr_pool_t *p)
{
    ap_hook_fixups(gearman_request_hook, NULL, NULL, APR_HOOK_MIDDLE);
    ap_register_output_filter("GEARMAN", gearman_response_hook, NULL, AP_FTYPE_RESOURCE);
}

/* Dispatch list for API hooks */
module AP_MODULE_DECLARE_DATA gearman_module = {
    STANDARD20_MODULE_STUFF, 
    create_gearman_dir_config,  /* create per-dir    config structures */
    merge_gearman_dir_configs,  /* merge  per-dir    config structures */
    NULL,                  /* create per-server config structures */
    NULL,                  /* merge  per-server config structures */
    gearman_cmds,          /* table of config file commands       */
    gearman_register_hooks /* register hooks                      */
};

