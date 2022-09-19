/*
Copyright (C) 2022- The University of Notre Dame
This software is distributed under the GNU General Public License.
See the file COPYING for details.
*/

#include "ds_manager.h"
#include "ds_manager_get.h"
#include "ds_manager_put.h"
#include "ds_manager_summarize.h"
#include "ds_schedule.h"
#include "ds_protocol.h"
#include "ds_task.h"
#include "ds_file.h"
#include "ds_resources.h"
#include "ds_worker_info.h"
#include "ds_remote_file_info.h"
#include "ds_factory_info.h"
#include "ds_task_info.h"
#include "ds_blocklist.h"
#include "ds_txn_log.h"
#include "ds_perf_log.h"

#include "cctools.h"
#include "int_sizes.h"
#include "link.h"
#include "link_auth.h"
#include "debug.h"
#include "stringtools.h"
#include "catalog_query.h"
#include "domain_name_cache.h"
#include "hash_table.h"
#include "interfaces_address.h"
#include "itable.h"
#include "list.h"
#include "macros.h"
#include "username.h"
#include "create_dir.h"
#include "xxmalloc.h"
#include "load_average.h"
#include "buffer.h"
#include "rmonitor.h"
#include "rmonitor_types.h"
#include "rmonitor_poll.h"
#include "category_internal.h"
#include "copy_stream.h"
#include "random.h"
#include "process.h"
#include "path.h"
#include "url_encode.h"
#include "jx_print.h"
#include "jx_parse.h"
#include "shell.h"

#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stddef.h>

/* Seconds between updates to the catalog. */
#define DS_UPDATE_INTERVAL 60

/* Seconds between measurement of manager local resources. */
#define DS_RESOURCE_MEASUREMENT_INTERVAL 30

/* Default value for keepalive interval in seconds. */
#define DS_DEFAULT_KEEPALIVE_INTERVAL 120

/* Default value for keepalive timeout in seconds. */
#define DS_DEFAULT_KEEPALIVE_TIMEOUT  900

/* Maximum size of standard output from task.  (If larger, send to a separate file.) */
#define MAX_TASK_STDOUT_STORAGE (1*GIGABYTE)

/* Maximum number of workers to add in a single cycle before dealing with other matters. */
#define MAX_NEW_WORKERS 10

/* How frequently to check for tasks that do not fit any worker. */
#define DS_LARGE_TASK_CHECK_INTERVAL 180000000 // 3 minutes in usecs

/* Default scheduling option, can be set prior to creating a manager. */
int ds_option_scheduler = DS_SCHEDULE_TIME;

/* Default timeout for slow workers to come back to the pool, can be set prior to creating a manager. */
double ds_option_blocklist_slow_workers_timeout = 900;

/* Forward prototypes for functions that are called out of order. */
/* Many of these should be removed if forward declaration is not needed. */

static void handle_failure(struct ds_manager *q, struct ds_worker_info *w, struct ds_task *t, ds_result_code_t fail_type);
static void remove_worker(struct ds_manager *q, struct ds_worker_info *w, ds_worker_disconnect_reason_t reason);
static int shut_down_worker(struct ds_manager *q, struct ds_worker_info *w);

static void reap_task_from_worker(struct ds_manager *q, struct ds_worker_info *w, struct ds_task *t, ds_task_state_t new_state);
static int cancel_task_on_worker(struct ds_manager *q, struct ds_task *t, ds_task_state_t new_state);
static void count_worker_resources(struct ds_manager *q, struct ds_worker_info *w);

static void find_max_worker(struct ds_manager *q);
static void update_max_worker(struct ds_manager *q, struct ds_worker_info *w);

static ds_task_state_t change_task_state( struct ds_manager *q, struct ds_task *t, ds_task_state_t new_state);

static int task_state_count( struct ds_manager *q, const char *category, ds_task_state_t state);
static int task_request_count( struct ds_manager *q, const char *category, category_allocation_t request);

static ds_msg_code_t handle_http_request( struct ds_manager *q, struct ds_worker_info *w, const char *path, time_t stoptime );
static ds_msg_code_t handle_dataswarm(struct ds_manager *q, struct ds_worker_info *w, const char *line);
static ds_msg_code_t handle_queue_status(struct ds_manager *q, struct ds_worker_info *w, const char *line, time_t stoptime);
static ds_msg_code_t handle_resource(struct ds_manager *q, struct ds_worker_info *w, const char *line);
static ds_msg_code_t handle_feature(struct ds_manager *q, struct ds_worker_info *w, const char *line);

static struct jx * queue_to_jx( struct ds_manager *q );
static struct jx * queue_lean_to_jx( struct ds_manager *q );

char *ds_monitor_wrap(struct ds_manager *q, struct ds_worker_info *w, struct ds_task *t, struct rmsummary *limits);

void ds_accumulate_task(struct ds_manager *q, struct ds_task *t);
struct category *ds_category_lookup_or_create(struct ds_manager *q, const char *name);

void ds_disable_monitoring(struct ds_manager *q);
static void aggregate_workers_resources( struct ds_manager *q, struct ds_resources *total, struct hash_table *features);
static struct ds_task *ds_wait_internal(struct ds_manager *q, int timeout, const char *tag);
static void release_all_workers( struct ds_manager *q );

/* Return the number of workers matching a given type: WORKER, STATUS, etc */

static int count_workers( struct ds_manager *q, ds_worker_type_t type )
{
	struct ds_worker_info *w;
	char* id;

	int count = 0;

	hash_table_firstkey(q->worker_table);
	while(hash_table_nextkey(q->worker_table, &id, (void**)&w)) {
		if(w->type & type) {
			count++;
		}
	}

	return count;
}

/* Round up a resource value based on the overcommit multiplier currently in effect. */

int64_t overcommitted_resource_total(struct ds_manager *q, int64_t total)
{
	int64_t r = 0;
	if(total != 0)
	{
		r = ceil(total * q->resource_submit_multiplier);
	}

	return r;
}

/* Return the number of workers available to run tasks of any size. */

int ds_manager_available_workers(struct ds_manager *q) {
	struct ds_worker_info *w;
	char* id;
	int available_workers = 0;

	hash_table_firstkey(q->worker_table);
	while(hash_table_nextkey(q->worker_table, &id, (void**)&w)) {
		if(strcmp(w->hostname, "unknown") != 0) {
			if(overcommitted_resource_total(q, w->resources->cores.total) > w->resources->cores.inuse || w->resources->disk.total > w->resources->disk.inuse || overcommitted_resource_total(q, w->resources->memory.total) > w->resources->memory.inuse){
				available_workers++;
			}
		}
	}

	return available_workers;
}

/* Returns count of workers that are running at least 1 task. */

static int workers_with_tasks(struct ds_manager *q) {
	struct ds_worker_info *w;
	char* id;
	int workers_with_tasks = 0;

	hash_table_firstkey(q->worker_table);
	while(hash_table_nextkey(q->worker_table, &id, (void**)&w)) {
		if(strcmp(w->hostname, "unknown")){
			if(itable_size(w->current_tasks)){
				workers_with_tasks++;
			}
		}
	}

	return workers_with_tasks;
}

/* Convert a link pointer into a string that can be used as a key into a hash table. */

static char * link_to_hash_key(struct link *link )
{
	return string_format("0x%p",link);
}

/*
This function sends a message to the worker and records the time the message is
successfully sent. This timestamp is used to determine when to send keepalive checks.
*/

__attribute__ (( format(printf,3,4) ))
int ds_manager_send( struct ds_manager *q, struct ds_worker_info *w, const char *fmt, ... )
{
	va_list va;
	time_t stoptime;
	buffer_t B[1];
	buffer_init(B);
	buffer_abortonfailure(B, 1);
	buffer_max(B, DS_LINE_MAX);

	va_start(va, fmt);
	buffer_putvfstring(B, fmt, va);
	va_end(va);

	debug(D_DS, "tx to %s (%s): %s", w->hostname, w->addrport, buffer_tostring(B));

	stoptime = time(0) + q->short_timeout;

	int result = link_putlstring(w->link, buffer_tostring(B), buffer_pos(B), stoptime);

	buffer_free(B);

	return result;
}

/* Handle a name message coming back from the worker, requesting the manager's project name. */

static ds_msg_code_t handle_name(struct ds_manager *q, struct ds_worker_info *w, char *line)
{
	debug(D_DS, "Sending project name to worker (%s)", w->addrport);

	//send project name (q->name) if there is one. otherwise send blank line
	ds_manager_send(q, w, "%s\n", q->name ? q->name : "");

	return DS_MSG_PROCESSED;
}

/* Handle an info message coming from the worker that provides a variety of metrics. */

static ds_msg_code_t handle_info(struct ds_manager *q, struct ds_worker_info *w, char *line)
{
	char field[DS_LINE_MAX];
	char value[DS_LINE_MAX];

	int n = sscanf(line,"info %s %[^\n]", field, value);

	if(n != 2)
		return DS_MSG_FAILURE;

	if(string_prefix_is(field, "workers_joined")) {
		w->stats->workers_joined = atoll(value);
	} else if(string_prefix_is(field, "workers_removed")) {
		w->stats->workers_removed = atoll(value);
	} else if(string_prefix_is(field, "time_send")) {
		w->stats->time_send = atoll(value);
	} else if(string_prefix_is(field, "time_receive")) {
		w->stats->time_receive = atoll(value);
	} else if(string_prefix_is(field, "time_execute")) {
		w->stats->time_workers_execute = atoll(value);
	} else if(string_prefix_is(field, "bytes_sent")) {
		w->stats->bytes_sent = atoll(value);
	} else if(string_prefix_is(field, "bytes_received")) {
		w->stats->bytes_received = atoll(value);
	} else if(string_prefix_is(field, "tasks_waiting")) {
		w->stats->tasks_waiting = atoll(value);
	} else if(string_prefix_is(field, "tasks_running")) {
		w->stats->tasks_running = atoll(value);
	} else if(string_prefix_is(field, "idle-disconnecting")) {
		remove_worker(q, w, DS_WORKER_DISCONNECT_IDLE_OUT);
		q->stats->workers_idled_out++;
	} else if(string_prefix_is(field, "end_of_resource_update")) {
		count_worker_resources(q, w);
		ds_txn_log_write_worker_resources(q, w);
	} else if(string_prefix_is(field, "worker-id")) {
		free(w->workerid);
		w->workerid = xxstrdup(value);
		ds_txn_log_write_worker(q, w, 0, 0);
	} else if(string_prefix_is(field, "worker-end-time")) {
		w->end_time = MAX(0, atoll(value));
	} else if(string_prefix_is(field, "from-factory")) {
		q->fetch_factory = 1;
		w->factory_name = xxstrdup(value);

		struct ds_factory_info *f = ds_factory_info_lookup(q, w->factory_name);
		if(f->connected_workers+1 > f->max_workers) {
			shut_down_worker(q, w);
		}
	}

	//Note we always mark info messages as processed, as they are optional.
	return DS_MSG_PROCESSED;
}

/*
A cache-update message coming from the worker means that a requested
remote transfer or command was successful, and know we know the size
of the file for the purposes of cache storage management.
*/

static int handle_cache_update( struct ds_manager *q, struct ds_worker_info *w, const char *line )
{
	char cachename[DS_LINE_MAX];
	long long size;
	long long transfer_time;
	
	if(sscanf(line,"cache-update %s %lld %lld",cachename,&size,&transfer_time)==3) {
		struct ds_remote_file_info *remote_info = hash_table_lookup(w->current_files,cachename);
		if(remote_info) {
			remote_info->size = size;
			remote_info->transfer_time = transfer_time;
		}
	}
	
	return DS_MSG_PROCESSED;
}

/*
A cache-invalid message coming from the worker means that a requested
remote transfer or command did not succeed, and the intended file is
not in the cache.  It is accompanied by a (presumably short) string
message that further explains the failure.
So, we remove the corresponding note for that worker and log the error.
We should expect to soon receive some failed tasks that were unable
set up their own input sandboxes.
*/

static int handle_cache_invalid( struct ds_manager *q, struct ds_worker_info *w, const char *line )
{
	char cachename[DS_LINE_MAX];
	int length;
	if(sscanf(line,"cache-invalid %s %d",cachename,&length)==2) {

		char *message = malloc(length+1);
		time_t stoptime = time(0) + q->long_timeout;
		
		int actual = link_read(w->link,message,length,stoptime);
		if(actual!=length) {
			free(message);
			return DS_MSG_FAILURE;
		}
		
		message[length] = 0;
		debug(D_DS,"%s (%s) invalidated %s with error: %s",w->hostname,w->addrport,cachename,message);
		free(message);
		
		struct ds_remote_file_info *remote_info = hash_table_remove(w->current_files,cachename);
		if(remote_info) ds_remote_file_info_delete(remote_info);
	}
	return DS_MSG_PROCESSED;
}

/*
A transfer-address message indicates that the worker is listening
on its own port to receive get requests from other workers.
*/

static int handle_transfer_address( struct ds_manager *q, struct ds_worker_info *w, const char *line )
{
	if(sscanf(line,"transfer-address %s %d",w->transfer_addr,&w->transfer_port)) {
		w->transfer_port_active = 1;
		return DS_MSG_PROCESSED;
	} else {
		return DS_MSG_FAILURE;
	}
}

/*
This function receives a message from worker and records the time a message is successfully
received. This timestamp is used in keepalive timeout computations.
*/

static ds_msg_code_t ds_manager_recv(struct ds_manager *q, struct ds_worker_info *w, char *line, size_t length )
{
	time_t stoptime;
	stoptime = time(0) + q->short_timeout;

	int result = link_readline(w->link, line, length, stoptime);

	if (result <= 0) {
		return DS_MSG_FAILURE;
	}

	w->last_msg_recv_time = timestamp_get();

	debug(D_DS, "rx from %s (%s): %s", w->hostname, w->addrport, line);

	char path[length];

	// Check for status updates that can be consumed here.
	if(string_prefix_is(line, "alive")) {
		result = DS_MSG_PROCESSED;
	} else if(string_prefix_is(line, "dataswarm")) {
		result = handle_dataswarm(q, w, line);
	} else if (string_prefix_is(line,"queue_status") || string_prefix_is(line, "worker_status") || string_prefix_is(line, "task_status") || string_prefix_is(line, "wable_status") || string_prefix_is(line, "resources_status")) {
		result = handle_queue_status(q, w, line, stoptime);
	} else if (string_prefix_is(line, "available_results")) {
		hash_table_insert(q->workers_with_available_results, w->hashkey, w);
		result = DS_MSG_PROCESSED;
	} else if (string_prefix_is(line, "resource")) {
		result = handle_resource(q, w, line);
	} else if (string_prefix_is(line, "feature")) {
		result = handle_feature(q, w, line);
	} else if (string_prefix_is(line, "auth")) {
		debug(D_DS|D_NOTICE,"worker (%s) is attempting to use a password, but I do not have one.",w->addrport);
		result = DS_MSG_FAILURE;
	} else if (string_prefix_is(line, "name")) {
		result = handle_name(q, w, line);
	} else if (string_prefix_is(line, "info")) {
		result = handle_info(q, w, line);
	} else if (string_prefix_is(line, "cache-update")) {
		result = handle_cache_update(q, w, line);
	} else if (string_prefix_is(line, "cache-invalid")) {
		result = handle_cache_invalid(q, w, line);
	} else if (string_prefix_is(line, "transfer-address")) {
		result = handle_transfer_address(q, w, line);
	} else if( sscanf(line,"GET %s HTTP/%*d.%*d",path)==1) {
	        result = handle_http_request(q,w,path,stoptime);
	} else {
		// Message is not a status update: return it to the user.
		result = DS_MSG_NOT_PROCESSED;
	}

	return result;
}

/*
Call ds_manager_recv and silently retry if the result indicates
an asynchronous update message like 'keepalive' or 'resource'.
*/

ds_msg_code_t ds_manager_recv_retry( struct ds_manager *q, struct ds_worker_info *w, char *line, int length )
{
	ds_msg_code_t result = DS_MSG_PROCESSED;

	do {
		result = ds_manager_recv(q, w,line,length);
	} while(result == DS_MSG_PROCESSED);

	return result;
}

/*
Compute the expected transfer rate of the manage in bytes/second,
and return the basis of that computation in *data_source.
*/

static double get_queue_transfer_rate(struct ds_manager *q, char **data_source)
{
	double queue_transfer_rate; // bytes per second
	int64_t     q_total_bytes_transferred = q->stats->bytes_sent + q->stats->bytes_received;
	timestamp_t q_total_transfer_time     = q->stats->time_send  + q->stats->time_receive;

	// Note q_total_transfer_time is timestamp_t with units of microseconds.
	if(q_total_transfer_time>1000000) {
		queue_transfer_rate = 1000000.0 * q_total_bytes_transferred / q_total_transfer_time;
		if (data_source) {
			*data_source = xxstrdup("overall queue");
		}
	} else {
		queue_transfer_rate = q->default_transfer_rate;
		if (data_source) {
			*data_source = xxstrdup("conservative default");
		}
	}

	return queue_transfer_rate;
}

/*
Select an appropriate timeout value for the transfer of a certain number of bytes.
We do not know in advance how fast the system will perform.

So do this by starting with an assumption of bandwidth taken from the worker,
from the queue, or from a (slow) default number, depending on what information is available.
The timeout is chosen to be a multiple of the expected transfer time from the assumed bandwidth.

The overall effect is to reject transfers that are 10x slower than what has been seen before.

Two exceptions are made:
- The transfer time cannot be below a configurable minimum time.
*/

int ds_manager_transfer_wait_time(struct ds_manager *q, struct ds_worker_info *w, struct ds_task *t, int64_t length)
{
	double avg_transfer_rate; // bytes per second
	char *data_source;

	if(w->total_transfer_time>1000000) {
		// Note w->total_transfer_time is timestamp_t with units of microseconds.
		avg_transfer_rate = 1000000 * w->total_bytes_transferred / w->total_transfer_time;
		data_source = xxstrdup("worker's observed");
	} else {
		avg_transfer_rate = get_queue_transfer_rate(q, &data_source);
	}

	double tolerable_transfer_rate = avg_transfer_rate / q->transfer_outlier_factor; // bytes per second

	int timeout = length / tolerable_transfer_rate;

	// An ordinary manager has a lower minimum timeout b/c it responds immediately to the manager.
	timeout = MAX(q->minimum_transfer_timeout,timeout);

	/* Don't bother printing anything for transfers of less than 1MB, to avoid excessive output. */

	if( length >= 1048576 ) {
		debug(D_DS,"%s (%s) using %s average transfer rate of %.2lf MB/s\n", w->hostname, w->addrport, data_source, avg_transfer_rate/MEGABYTE);

		debug(D_DS, "%s (%s) will try up to %d seconds to transfer this %.2lf MB file.", w->hostname, w->addrport, timeout, length/1000000.0);
	}

	free(data_source);
	return timeout;
}

/*
Remove idle workers associated with a given factory, so as to scale down
cleanly by not cancelling active work.
*/

static int factory_trim_workers(struct ds_manager *q, struct ds_factory_info *f)
{
	if (!f) return 0;
	assert(f->name);

	// Iterate through all workers and shut idle ones down
	struct ds_worker_info *w;
	char *key;
	int trimmed_workers = 0;

	struct hash_table *idle_workers = hash_table_create(0, 0);
	hash_table_firstkey(q->worker_table);
	while ( f->connected_workers - trimmed_workers > f->max_workers &&
			hash_table_nextkey(q->worker_table, &key, (void **) &w) ) {
		if ( w->factory_name &&
				!strcmp(f->name, w->factory_name) &&
				itable_size(w->current_tasks) < 1 ) {
			hash_table_insert(idle_workers, key, w);
			trimmed_workers++;
		}
	}

	hash_table_firstkey(idle_workers);
	while (hash_table_nextkey(idle_workers, &key, (void **) &w)) {
		hash_table_remove(idle_workers, key);
		hash_table_firstkey(idle_workers);
		shut_down_worker(q, w);
	}
	hash_table_delete(idle_workers);

	debug(D_DS, "Trimmed %d workers from %s", trimmed_workers, f->name);
	return trimmed_workers;
}

/*
Given a JX description of a factory, update our internal ds_factory_info
records to match that description.  If the description indicates that
we have more workers than desired, trim the workers associated with that
factory.
*/

static void update_factory(struct ds_manager *q, struct jx *j)
{
	const char *name = jx_lookup_string(j, "factory_name");
	if (!name) return;

	struct ds_factory_info *f = ds_factory_info_lookup(q,name);

	f->seen_at_catalog = 1;
	int found = 0;
	struct jx *m = jx_lookup_guard(j, "max_workers", &found);
	if (found) {
		int old_max_workers = f->max_workers;
		f->max_workers = m->u.integer_value;
		// Trim workers if max_workers reduced.
		if (f->max_workers < old_max_workers) {
			factory_trim_workers(q, f);
		}
	}
}

/*
Query the catalog to discover what factories are feeding this manager.
Update our internal state with the data returned.
*/

static void update_read_catalog_factory(struct ds_manager *q, time_t stoptime) {
	struct catalog_query *cq;
	struct jx *jexpr = NULL;
	struct jx *j;

	// Iterate through factory_table to create a query filter.
	int first_name = 1;
	buffer_t filter;
	buffer_init(&filter);
	char *factory_name = NULL;
	struct ds_factory_info *f = NULL;
	buffer_putfstring(&filter, "type == \"ds_factory\" && (");

	hash_table_firstkey(q->factory_table);
	while ( hash_table_nextkey(q->factory_table, &factory_name, (void **)&f) ) {
		buffer_putfstring(&filter, "%sfactory_name == \"%s\"", first_name ? "" : " || ", factory_name);
		first_name = 0;
		f->seen_at_catalog = 0;
	}
	buffer_putfstring(&filter, ")");
	jexpr = jx_parse_string(buffer_tolstring(&filter, NULL));
	buffer_free(&filter);

	// Query the catalog server
	debug(D_DS, "Retrieving factory info from catalog server(s) at %s ...", q->catalog_hosts);
	if ( (cq = catalog_query_create(q->catalog_hosts, jexpr, stoptime)) ) {
		// Update the table
		while((j = catalog_query_read(cq, stoptime))) {
			update_factory(q, j);
			jx_delete(j);
		}
		catalog_query_delete(cq);
	} else {
		debug(D_DS, "Failed to retrieve factory info from catalog server(s) at %s.", q->catalog_hosts);
	}

	// Remove outdated factories
	struct list *outdated_factories = list_create();
	hash_table_firstkey(q->factory_table);
	while ( hash_table_nextkey(q->factory_table, &factory_name, (void **) &f) ) {
		if ( !f->seen_at_catalog && f->connected_workers < 1 ) {
			list_push_tail(outdated_factories, f);
		}
	}
	list_clear(outdated_factories,(void*)ds_factory_info_delete);
	list_delete(outdated_factories);
}

/*
Send an update to the catalog describing the state of this manager.
*/

static void update_write_catalog(struct ds_manager *q )
{
	// Only write if we have a name.
	if (!q->name) return; 

	// Generate the manager status in an jx, and print it to a buffer.
	struct jx *j = queue_to_jx(q);
	char *str = jx_print_string(j);

	// Send the buffer.
	debug(D_DS, "Advertising manager status to the catalog server(s) at %s ...", q->catalog_hosts);
	if(!catalog_query_send_update_conditional(q->catalog_hosts, str)) {

		// If the send failed b/c the buffer is too big, send the lean version instead.
		struct jx *lj = queue_lean_to_jx(q);
		char *lstr = jx_print_string(lj);
		catalog_query_send_update(q->catalog_hosts,lstr);
		free(lstr);
		jx_delete(lj);
	}

	// Clean up.
	free(str);
	jx_delete(j);
}

/* Read from the catalog if fetch_factory is enabled. */

static void update_read_catalog(struct ds_manager *q)
{
	time_t stoptime = time(0) + 5; // Short timeout for query

	if (q->fetch_factory) {
		update_read_catalog_factory(q, stoptime);
	}
}

/* Send and receive updates from the catalog server as needed. */

static void update_catalog(struct ds_manager *q, int force_update )
{
	// Only update every last_update_time seconds.
	if(!force_update && (time(0) - q->catalog_last_update_time) < DS_UPDATE_INTERVAL)
		return;

	// If host and port are not set, pick defaults.
	if(!q->catalog_hosts) q->catalog_hosts = xxstrdup(CATALOG_HOST);

	// Update the catalog.
	update_write_catalog(q);
	update_read_catalog(q);

	q->catalog_last_update_time = time(0);
}

/* Remove all tasks and other associated state from a given worker. */

static void cleanup_worker(struct ds_manager *q, struct ds_worker_info *w)
{
	char *key, *value;
	struct ds_task *t;
	struct rmsummary *r;
	uint64_t taskid;

	if(!q || !w) return;

	hash_table_firstkey(w->current_files);
	while(hash_table_nextkey(w->current_files, &key, (void **) &value)) {
		hash_table_remove(w->current_files, key);
		free(value);
		hash_table_firstkey(w->current_files);
	}

	itable_firstkey(w->current_tasks);
	while(itable_nextkey(w->current_tasks, &taskid, (void **)&t)) {
		if (t->time_when_commit_end >= t->time_when_commit_start) {
			timestamp_t delta_time = timestamp_get() - t->time_when_commit_end;
			t->time_workers_execute_failure += delta_time;
			t->time_workers_execute_all     += delta_time;
		}

		ds_task_clean(t, 0);
		reap_task_from_worker(q, w, t, DS_TASK_READY);

		itable_firstkey(w->current_tasks);
	}

	itable_firstkey(w->current_tasks_boxes);
	while(itable_nextkey(w->current_tasks_boxes, &taskid, (void **) &r)) {
		rmsummary_delete(r);
	}

	itable_clear(w->current_tasks);
	itable_clear(w->current_tasks_boxes);
	w->finished_tasks = 0;
}

#define accumulate_stat(qs, ws, field) (qs)->field += (ws)->field

static void record_removed_worker_stats(struct ds_manager *q, struct ds_worker_info *w)
{
	struct ds_stats *qs = q->stats_disconnected_workers;
	struct ds_stats *ws = w->stats;

	accumulate_stat(qs, ws, workers_joined);
	accumulate_stat(qs, ws, workers_removed);
	accumulate_stat(qs, ws, workers_released);
	accumulate_stat(qs, ws, workers_idled_out);
	accumulate_stat(qs, ws, workers_fast_aborted);
	accumulate_stat(qs, ws, workers_blocked);
	accumulate_stat(qs, ws, workers_lost);

	accumulate_stat(qs, ws, time_send);
	accumulate_stat(qs, ws, time_receive);
	accumulate_stat(qs, ws, time_workers_execute);

	accumulate_stat(qs, ws, bytes_sent);
	accumulate_stat(qs, ws, bytes_received);

	//Count all the workers joined as removed.
	qs->workers_removed = ws->workers_joined;
}

/* Remove a worker from this master by removing all remote state, all local state, and disconnecting. */

static void remove_worker(struct ds_manager *q, struct ds_worker_info *w, ds_worker_disconnect_reason_t reason)
{
	if(!q || !w) return;

	debug(D_DS, "worker %s (%s) removed", w->hostname, w->addrport);

	if(w->type == DS_WORKER_TYPE_WORKER) {
		q->stats->workers_removed++;
	}

	ds_txn_log_write_worker(q, w, 1, reason);

	cleanup_worker(q, w);

	hash_table_remove(q->worker_table, w->hashkey);
	hash_table_remove(q->workers_with_available_results, w->hashkey);

	record_removed_worker_stats(q, w);

	if (w->factory_name) {
		struct ds_factory_info *f = ds_factory_info_lookup(q,w->factory_name);
		if(f) f->connected_workers--;
	}

	ds_worker_delete(w);

	/* update the largest worker seen */
	find_max_worker(q);

	debug(D_DS, "%d workers connected in total now", count_workers(q, DS_WORKER_TYPE_WORKER));
}

/* Gently release a worker by sending it a release message, and then removing it. */

static int release_worker(struct ds_manager *q, struct ds_worker_info *w)
{
	if(!w) return 0;


	ds_manager_send(q,w,"release\n");

	remove_worker(q, w, DS_WORKER_DISCONNECT_EXPLICIT);

	q->stats->workers_released++;

	return 1;
}

/* Check for new connections on the manager's port, and add a worker if one is there. */

static void add_worker(struct ds_manager *q)
{
	char addr[LINK_ADDRESS_MAX];
	int port;

	struct link *link = link_accept(q->manager_link, time(0) + q->short_timeout);
	if(!link) {
		return;
	}

	link_keepalive(link, 1);
	link_tune(link, LINK_TUNE_INTERACTIVE);

	if(!link_address_remote(link, addr, &port)) {
		link_close(link);
		return;
	}

	debug(D_DS,"worker %s:%d connected",addr,port);

	if(q->ssl_enabled) {
		if(link_ssl_wrap_accept(link,q->ssl_key,q->ssl_cert)) {
			debug(D_DS,"worker %s:%d completed ssl connection",addr,port);
		} else {
			debug(D_DS,"worker %s:%d failed ssl connection",addr,port);
			link_close(link);
			return;
		}
	} else {
		/* nothing to do */
	}

	if(q->password) {
		debug(D_DS,"worker %s:%d authenticating",addr,port);
		if(!link_auth_password(link,q->password,time(0)+q->short_timeout)) {
			debug(D_DS|D_NOTICE,"worker %s:%d presented the wrong password",addr,port);
			link_close(link);
			return;
		}
	}

	struct ds_worker_info *w = ds_worker_create(link);
	if(!w) {
		debug(D_NOTICE, "Cannot allocate memory for worker %s:%d.", addr, port);
		link_close(link);
		return;
	}

	w->hashkey = link_to_hash_key(link);
	w->addrport = string_format("%s:%d",addr,port);

	hash_table_insert(q->worker_table, w->hashkey, w);
}

/* Delete a single file on a remote worker. */

static void delete_worker_file( struct ds_manager *q, struct ds_worker_info *w, const char *filename, int flags, int except_flags ) {
	if(!(flags & except_flags)) {
		ds_manager_send(q,w, "unlink %s\n", filename);
		hash_table_remove(w->current_files, filename);
	}
}

/* Delete all files in a list except those that match one or more of the "except_flags" */

static void delete_worker_files( struct ds_manager *q, struct ds_worker_info *w, struct list *files, int except_flags ) {
	struct ds_file *tf;

	if(!files) return;

	list_first_item(files);
	while((tf = list_next_item(files))) {
		delete_worker_file(q, w, tf->cached_name, tf->flags, except_flags);
	}
}

/* Delete all output files of a given task. */

static void delete_task_output_files(struct ds_manager *q, struct ds_worker_info *w, struct ds_task *t)
{
	delete_worker_files(q, w, t->output_files, 0);
}

/* Delete only the uncacheable output files of a given task. */

static void delete_uncacheable_files( struct ds_manager *q, struct ds_worker_info *w, struct ds_task *t )
{
	delete_worker_files(q, w, t->input_files, DS_CACHE );
	delete_worker_files(q, w, t->output_files, DS_CACHE );
}

/* Determine the resource monitor file name that should be associated with this task. */

static char *monitor_file_name(struct ds_manager *q, struct ds_task *t, const char *ext) {
	char *dir;
	
	if(t->monitor_output_directory) {
		dir = t->monitor_output_directory;
	} else if(q->monitor_output_directory) {
		dir = q->monitor_output_directory;
	} else {
		dir = "./";
	}

	return string_format("%s/" RESOURCE_MONITOR_TASK_LOCAL_NAME "%s",
			dir, getpid(), t->taskid, ext ? ext : "");
}

/* Extract the resources consumed by a task by reading the appropriate resource monitor file. */

static void read_measured_resources(struct ds_manager *q, struct ds_task *t) {

	char *summary = monitor_file_name(q, t, ".summary");

	if(t->resources_measured) {
		rmsummary_delete(t->resources_measured);
	}

	t->resources_measured = rmsummary_parse_file_single(summary);

	if(t->resources_measured) {
		t->resources_measured->category = xxstrdup(t->category);
		t->exit_code = t->resources_measured->exit_status;

		/* cleanup noise in cores value, otherwise small fluctuations trigger new
		 * maximums */
		if(t->resources_measured->cores > 0) {
			t->resources_measured->cores = MIN(t->resources_measured->cores, ceil(t->resources_measured->cores - 0.1));
		}
	} else {
		/* if no resources were measured, then we don't overwrite the return
		 * status, and mark the task as with error from monitoring. */
		t->resources_measured = rmsummary_create(-1);
		ds_task_update_result(t, DS_RESULT_RMONITOR_ERROR);
	}

	free(summary);
}

void resource_monitor_append_report(struct ds_manager *q, struct ds_task *t)
{
	if(q->monitor_mode == DS_MON_DISABLED)
		return;

	char *summary = monitor_file_name(q, t, ".summary");

	if(q->monitor_output_directory) {
		int monitor_fd = fileno(q->monitor_file);

		struct flock lock;
		lock.l_type   = F_WRLCK;
		lock.l_start  = 0;
		lock.l_whence = SEEK_SET;
		lock.l_len    = 0;

		fcntl(monitor_fd, F_SETLKW, &lock);

		if(!t->resources_measured)
		{
			fprintf(q->monitor_file, "# Summary for task %d was not available.\n", t->taskid);
		}

		FILE *fs = fopen(summary, "r");
		if(fs) {
			copy_stream_to_stream(fs, q->monitor_file);
			fclose(fs);
		}

		fprintf(q->monitor_file, "\n");

		lock.l_type   = F_ULOCK;
		fcntl(monitor_fd, F_SETLK, &lock);
	}

	/* Remove individual summary file unless it is named specifically. */
	int keep = 0;
	if(t->monitor_output_directory)
		keep = 1;

	if(q->monitor_mode & DS_MON_FULL && q->monitor_output_directory)
		keep = 1;

	if(!keep)
		unlink(summary);

	free(summary);
}

/* Compress old time series files so as to avoid accumulating infinite resource monitoring data. */

static void resource_monitor_compress_logs(struct ds_manager *q, struct ds_task *t) {
	char *series    = monitor_file_name(q, t, ".series");
	char *debug_log = monitor_file_name(q, t, ".debug");

	char *command = string_format("gzip -9 -q %s %s", series, debug_log);

	int status;
	int rc = shellcode(command, NULL, NULL, 0, NULL, NULL, &status);

	if(rc) {
		debug(D_NOTICE, "Could no successfully compress '%s', and '%s'\n", series, debug_log);
	}

	free(series);
	free(debug_log);
	free(command);
}

/* Get all the relevant output data from a completed task, then clean up unneeded items. */

static void fetch_output_from_worker(struct ds_manager *q, struct ds_worker_info *w, int taskid)
{
	struct ds_task *t;
	ds_result_code_t result = DS_SUCCESS;

	t = itable_lookup(w->current_tasks, taskid);
	if(!t) {
		debug(D_DS, "Failed to find task %d at worker %s (%s).", taskid, w->hostname, w->addrport);
		handle_failure(q, w, t, DS_WORKER_FAILURE);
		return;
	}

	// Start receiving output...
	t->time_when_retrieval = timestamp_get();

	if(t->result == DS_RESULT_RESOURCE_EXHAUSTION) {
		result = ds_manager_get_monitor_output_file(q,w,t);
	} else {
		result = ds_manager_get_output_files(q,w,t);
	}

	if(result != DS_SUCCESS) {
		debug(D_DS, "Failed to receive output from worker %s (%s).", w->hostname, w->addrport);
		handle_failure(q, w, t, result);
	}

	if(result == DS_WORKER_FAILURE) {
		// Finish receiving output:
		t->time_when_done = timestamp_get();

		return;
	}

	delete_uncacheable_files(q,w,t);

	/* if q is monitoring, append the task summary to the single
	 * queue summary, update t->resources_used, and delete the task summary. */
	if(q->monitor_mode) {
		read_measured_resources(q, t);

		/* Further, if we got debug and series files, gzip them. */
		if(q->monitor_mode & DS_MON_FULL)
			resource_monitor_compress_logs(q, t);
	}

	// Finish receiving output.
	t->time_when_done = timestamp_get();

	ds_accumulate_task(q, t);

	// At this point, a task is completed.
	reap_task_from_worker(q, w, t, DS_TASK_RETRIEVED);

	w->finished_tasks--;
	w->total_tasks_complete++;

	// At least one task has finished without triggering fast abort, thus we
	// now have evidence that worker is not slow (e.g., it was probably the
	// previous task that was slow).
	w->fast_abort_alarm = 0;

	if(t->result == DS_RESULT_RESOURCE_EXHAUSTION) {
		if(t->resources_measured && t->resources_measured->limits_exceeded) {
			struct jx *j = rmsummary_to_json(t->resources_measured->limits_exceeded, 1);
			if(j) {
				char *str = jx_print_string(j);
				debug(D_DS, "Task %d exhausted resources on %s (%s): %s\n",
						t->taskid,
						w->hostname,
						w->addrport,
						str);
				free(str);
				jx_delete(j);
			}
		} else {
				debug(D_DS, "Task %d exhausted resources on %s (%s), but not resource usage was available.\n",
						t->taskid,
						w->hostname,
						w->addrport);
		}

		struct category *c = ds_category_lookup_or_create(q, t->category);
		category_allocation_t next = category_next_label(c, t->resource_request, /* resource overflow */ 1, t->resources_requested, t->resources_measured);

		if(next == CATEGORY_ALLOCATION_ERROR) {
			debug(D_DS, "Task %d failed given max resource exhaustion.\n", t->taskid);
		}
		else {
			debug(D_DS, "Task %d resubmitted using new resource allocation.\n", t->taskid);
			t->resource_request = next;
			change_task_state(q, t, DS_TASK_READY);
			return;
		}
	}

	/* print warnings if the task ran for a very short time (1s) and exited with common non-zero status */
	if(t->result == DS_RESULT_SUCCESS && t->time_workers_execute_last < 1000000) {
		switch(t->exit_code) {
			case(126):
				warn(D_DS, "Task %d ran for a very short time and exited with code %d.\n", t->taskid, t->exit_code);
				warn(D_DS, "This usually means that the task's command is not an executable,\n");
				warn(D_DS, "or that the worker's scratch directory is on a no-exec partition.\n");
				break;
			case(127):
				warn(D_DS, "Task %d ran for a very short time and exited with code %d.\n", t->taskid, t->exit_code);
				warn(D_DS, "This usually means that the task's command could not be found, or that\n");
				warn(D_DS, "it uses a shared library not available at the worker, or that\n");
				warn(D_DS, "it uses a version of the glibc different than the one at the worker.\n");
				break;
			case(139):
				warn(D_DS, "Task %d ran for a very short time and exited with code %d.\n", t->taskid, t->exit_code);
				warn(D_DS, "This usually means that the task's command had a segmentation fault,\n");
				warn(D_DS, "either because it has a memory access error (segfault), or because\n");
				warn(D_DS, "it uses a version of a shared library different from the one at the worker.\n");
				break;
			default:
				break;
		}
	}

	ds_task_info_add(q,t);

	resource_monitor_append_report(q, t);

	debug(D_DS, "%s (%s) done in %.02lfs total tasks %lld average %.02lfs",
			w->hostname,
			w->addrport,
			(t->time_when_done - t->time_when_commit_start) / 1000000.0,
			(long long) w->total_tasks_complete,
			w->total_task_time / w->total_tasks_complete / 1000000.0);

	return;
}

/*
Consider the set of tasks that are waiting but not running.
Cancel those that have exceeded their expressed end time,
exceeded the maximum number of retries, or other policy issues.
*/

static int expire_waiting_tasks(struct ds_manager *q)
{
	struct ds_task *t;
	int expired = 0;
	int count;

	double current_time = timestamp_get() / ONE_SECOND;
	count = task_state_count(q, NULL, DS_TASK_READY);

	while(count > 0)
	{
		count--;

		t = list_pop_head(q->ready_list);

		if(t->resources_requested->end > 0 && t->resources_requested->end <= current_time)
		{
			ds_task_update_result(t, DS_RESULT_TASK_TIMEOUT);
			change_task_state(q, t, DS_TASK_RETRIEVED);
			expired++;
		} else if(t->max_retries > 0 && t->try_count > t->max_retries) {
			ds_task_update_result(t, DS_RESULT_MAX_RETRIES);
			change_task_state(q, t, DS_TASK_RETRIEVED);
			expired++;
		} else {
			list_push_tail(q->ready_list, t);
		}
	}

	return expired;
}


/*
This function handles app-level failures. It remove the task from WQ and marks
the task as complete so it is returned to the application.
*/

static void handle_app_failure(struct ds_manager *q, struct ds_worker_info *w, struct ds_task *t)
{
	//remove the task from tables that track dispatched tasks.
	//and add the task to complete list so it is given back to the application.
	reap_task_from_worker(q, w, t, DS_TASK_RETRIEVED);

	/*If the failure happened after a task execution, we remove all the output
	files specified for that task from the worker's cache.  This is because the
	application may resubmit the task and the resubmitted task may produce
	different outputs. */
	if(t) {
		if(t->time_when_commit_end > 0) {
			delete_task_output_files(q,w,t);
		}
	}

	return;
}

/*
Failures happen in the manager-worker interactions. In this case,
we remove the worker and retry the tasks dispatched to it elsewhere.
*/

static void handle_worker_failure(struct ds_manager *q, struct ds_worker_info *w)
{
	remove_worker(q, w, DS_WORKER_DISCONNECT_FAILURE);
	return;
}

/*
Handle the failure of a task, taking different actions depending on whether
this is due to an application-level issue or a problem with the worker alone.
*/


static void handle_failure(struct ds_manager *q, struct ds_worker_info *w, struct ds_task *t, ds_result_code_t fail_type)
{
	if(fail_type == DS_APP_FAILURE) {
		handle_app_failure(q, w, t);
	} else {
		handle_worker_failure(q, w);
	}
	return;
}

/*
Handle the initial connection message from a worker, which reports
basic information about the hostname, operating system, and so forth.
Once this message is processed, the manager knows it is a valid connection
and can begin sending tasks and data.
*/

static ds_msg_code_t handle_dataswarm(struct ds_manager *q, struct ds_worker_info *w, const char *line)
{
	char items[4][DS_LINE_MAX];
	int worker_protocol;

	int n = sscanf(line,"dataswarm %d %s %s %s %s",&worker_protocol,items[0],items[1],items[2],items[3]);
	if(n != 5)
		return DS_MSG_FAILURE;

	if(worker_protocol!=DS_PROTOCOL_VERSION) {
		debug(D_DS|D_NOTICE,"rejecting worker (%s) as it uses protocol %d. The manager is using protocol %d.",w->addrport,worker_protocol,DS_PROTOCOL_VERSION);
		ds_block_host(q, w->hostname);
		return DS_MSG_FAILURE;
	}

	if(w->hostname) free(w->hostname);
	if(w->os)       free(w->os);
	if(w->arch)     free(w->arch);
	if(w->version)  free(w->version);

	w->hostname = strdup(items[0]);
	w->os       = strdup(items[1]);
	w->arch     = strdup(items[2]);
	w->version  = strdup(items[3]);

	w->type = DS_WORKER_TYPE_WORKER;

	q->stats->workers_joined++;
	debug(D_DS, "%d workers are connected in total now", count_workers(q, DS_WORKER_TYPE_WORKER));


	debug(D_DS, "%s (%s) running CCTools version %s on %s (operating system) with architecture %s is ready", w->hostname, w->addrport, w->version, w->os, w->arch);

	if(cctools_version_cmp(CCTOOLS_VERSION, w->version) != 0) {
		debug(D_DEBUG, "Warning: potential worker version mismatch: worker %s (%s) is version %s, and manager is version %s", w->hostname, w->addrport, w->version, CCTOOLS_VERSION);
	}


	return DS_MSG_PROCESSED;
}

/*
If the manager has requested that a file be watched with DS_WATCH,
the worker will periodically send back update messages indicating that
the file has been written to.  There are a variety of ways in which the
message could be stale (e.g. task was cancelled) so if the message does
not line up with an expected task and file, then we discard it and keep
going.
*/

static ds_result_code_t get_update( struct ds_manager *q, struct ds_worker_info *w, const char *line )
{
	int64_t taskid;
	char path[DS_LINE_MAX];
	int64_t offset;
	int64_t length;

	int n = sscanf(line,"update %"PRId64" %s %"PRId64" %"PRId64,&taskid,path,&offset,&length);
	if(n!=4) {
		debug(D_DS,"Invalid message from worker %s (%s): %s", w->hostname, w->addrport, line );
		return DS_WORKER_FAILURE;
	}

	struct ds_task *t = itable_lookup(w->current_tasks,taskid);
	if(!t) {
		debug(D_DS,"worker %s (%s) sent output for unassigned task %"PRId64, w->hostname, w->addrport, taskid);
		link_soak(w->link,length,time(0)+ds_manager_transfer_wait_time(q,w,0,length));
		return DS_SUCCESS;
	}


	time_t stoptime = time(0) + ds_manager_transfer_wait_time(q,w,t,length);

	struct ds_file *f;
	const char *local_name = 0;

	list_first_item(t->output_files);
	while((f=list_next_item(t->output_files))) {
		if(!strcmp(path,f->remote_name)) {
			local_name = f->source;
			break;
		}
	}

	if(!local_name) {
		debug(D_DS,"worker %s (%s) sent output for unwatched file %s",w->hostname,w->addrport,path);
		link_soak(w->link,length,stoptime);
		return DS_SUCCESS;
	}

	int fd = open(local_name,O_WRONLY|O_CREAT,0777);
	if(fd<0) {
		debug(D_DS,"unable to update watched file %s: %s",local_name,strerror(errno));
		link_soak(w->link,length,stoptime);
		return DS_SUCCESS;
	}

	lseek(fd,offset,SEEK_SET);
	link_stream_to_fd(w->link,fd,length,stoptime);
	ftruncate(fd,offset+length);

	if(close(fd) < 0) {
		debug(D_DS, "unable to update watched file %s: %s\n", local_name, strerror(errno));
		return DS_SUCCESS;
	}

	return DS_SUCCESS;
}

/*
Failure to store result is treated as success so we continue to retrieve the
output files of the task.
*/

static ds_result_code_t get_result(struct ds_manager *q, struct ds_worker_info *w, const char *line) {

	if(!q || !w || !line) 
		return DS_WORKER_FAILURE;

	struct ds_task *t;

	int task_status, exit_status;
	uint64_t taskid;
	int64_t output_length, retrieved_output_length;
	timestamp_t execution_time;

	int64_t actual;

	timestamp_t observed_execution_time;
	timestamp_t effective_stoptime = 0;
	time_t stoptime;

	//Format: task completion status, exit status (exit code or signal), output length, execution time, taskid

	int n = sscanf(line, "result %d %d %"SCNd64 "%"SCNd64" %" SCNd64"", &task_status, &exit_status, &output_length, &execution_time, &taskid);

	if(n < 5) {
		debug(D_DS, "Invalid message from worker %s (%s): %s", w->hostname, w->addrport, line);
		return DS_WORKER_FAILURE;
	}

	t = itable_lookup(w->current_tasks, taskid);
	if(!t) {
		debug(D_DS, "Unknown task result from worker %s (%s): no task %" PRId64" assigned to worker.  Ignoring result.", w->hostname, w->addrport, taskid);
		stoptime = time(0) + ds_manager_transfer_wait_time(q, w, 0, output_length);
		link_soak(w->link, output_length, stoptime);
		return DS_SUCCESS;
	}

	if(task_status == DS_RESULT_FORSAKEN) {
		// Delete any input files that are not to be cached.
		delete_worker_files(q, w, t->input_files, DS_CACHE );

		/* task will be resubmitted, so we do not update any of the execution stats */
		reap_task_from_worker(q, w, t, DS_TASK_READY);

		return DS_SUCCESS;
	}

	observed_execution_time = timestamp_get() - t->time_when_commit_end;

	t->time_workers_execute_last = observed_execution_time > execution_time ? execution_time : observed_execution_time;

	t->time_workers_execute_all += t->time_workers_execute_last;

	if(q->bandwidth_limit) {
		effective_stoptime = (output_length/q->bandwidth_limit)*1000000 + timestamp_get();
	}

	if(output_length <= MAX_TASK_STDOUT_STORAGE) {
		retrieved_output_length = output_length;
	} else {
		retrieved_output_length = MAX_TASK_STDOUT_STORAGE;
		fprintf(stderr, "warning: stdout of task %"PRId64" requires %2.2lf GB of storage. This exceeds maximum supported size of %d GB. Only %d GB will be retrieved.\n", taskid, ((double) output_length)/MAX_TASK_STDOUT_STORAGE, MAX_TASK_STDOUT_STORAGE/GIGABYTE, MAX_TASK_STDOUT_STORAGE/GIGABYTE);
		ds_task_update_result(t, DS_RESULT_STDOUT_MISSING);
	}

	t->output = malloc(retrieved_output_length+1);
	if(t->output == NULL) {
		fprintf(stderr, "error: allocating memory of size %"PRId64" bytes failed for storing stdout of task %"PRId64".\n", retrieved_output_length, taskid);
		//drop the entire length of stdout on the link
		stoptime = time(0) + ds_manager_transfer_wait_time(q, w, t, output_length);
		link_soak(w->link, output_length, stoptime);
		retrieved_output_length = 0;
		ds_task_update_result(t, DS_RESULT_STDOUT_MISSING);
	}

	if(retrieved_output_length > 0) {
		debug(D_DS, "Receiving stdout of task %"PRId64" (size: %"PRId64" bytes) from %s (%s) ...", taskid, retrieved_output_length, w->addrport, w->hostname);

		//First read the bytes we keep.
		stoptime = time(0) + ds_manager_transfer_wait_time(q, w, t, retrieved_output_length);
		actual = link_read(w->link, t->output, retrieved_output_length, stoptime);
		if(actual != retrieved_output_length) {
			debug(D_DS, "Failure: actual received stdout size (%"PRId64" bytes) is different from expected (%"PRId64" bytes).", actual, retrieved_output_length);
			t->output[actual] = '\0';
			return DS_WORKER_FAILURE;
		}
		debug(D_DS, "Retrieved %"PRId64" bytes from %s (%s)", actual, w->hostname, w->addrport);

		//Then read the bytes we need to throw away.
		if(output_length > retrieved_output_length) {
			debug(D_DS, "Dropping the remaining %"PRId64" bytes of the stdout of task %"PRId64" since stdout length is limited to %d bytes.\n", (output_length-MAX_TASK_STDOUT_STORAGE), taskid, MAX_TASK_STDOUT_STORAGE);
			stoptime = time(0) + ds_manager_transfer_wait_time(q, w, t, (output_length-retrieved_output_length));
			link_soak(w->link, (output_length-retrieved_output_length), stoptime);

			//overwrite the last few bytes of buffer to signal truncated stdout.
			char *truncate_msg = string_format("\n>>>>>> STDOUT TRUNCATED AFTER THIS POINT.\n>>>>>> MAXIMUM OF %d BYTES REACHED, %" PRId64 " BYTES TRUNCATED.", MAX_TASK_STDOUT_STORAGE, output_length - retrieved_output_length);
			memcpy(t->output + MAX_TASK_STDOUT_STORAGE - strlen(truncate_msg) - 1, truncate_msg, strlen(truncate_msg));
			*(t->output + MAX_TASK_STDOUT_STORAGE - 1) = '\0';
			free(truncate_msg);
		}

		timestamp_t current_time = timestamp_get();
		if(effective_stoptime && effective_stoptime > current_time) {
			usleep(effective_stoptime - current_time);
		}
	} else {
		actual = 0;
	}

	if(t->output)
		t->output[actual] = 0;

	t->result        = task_status;
	t->exit_code = exit_status;

	q->stats->time_workers_execute += t->time_workers_execute_last;

	w->finished_tasks++;

	// Convert resource_monitor status into dataswarm status if needed.
	if(q->monitor_mode) {
		if(t->exit_code == RM_OVERFLOW) {
			ds_task_update_result(t, DS_RESULT_RESOURCE_EXHAUSTION);
		} else if(t->exit_code == RM_TIME_EXPIRE) {
			ds_task_update_result(t, DS_RESULT_TASK_TIMEOUT);
		}
	}

	change_task_state(q, t, DS_TASK_WAITING_RETRIEVAL);

	return DS_SUCCESS;
}

/*
Send to this worker a request for task results.
The worker will respond with all completed tasks and updates
on watched output files.  Process those results as they come back.
*/

static ds_result_code_t get_available_results(struct ds_manager *q, struct ds_worker_info *w)
{

	//max_count == -1, tells the worker to send all available results.
	ds_manager_send(q, w, "send_results %d\n", -1);
	debug(D_DS, "Reading result(s) from %s (%s)", w->hostname, w->addrport);

	char line[DS_LINE_MAX];
	int i = 0;

	ds_result_code_t result = DS_SUCCESS; //return success unless something fails below.

	while(1) {
		ds_msg_code_t mcode;
		mcode = ds_manager_recv_retry(q, w, line, sizeof(line));
		if(mcode!=DS_MSG_NOT_PROCESSED) {
			result = DS_WORKER_FAILURE;
			break;
		}

		if(string_prefix_is(line,"result")) {
			result = get_result(q, w, line);
			if(result != DS_SUCCESS) break;
			i++;
		} else if(string_prefix_is(line,"update")) {
			result = get_update(q,w,line);
			if(result != DS_SUCCESS) break;
		} else if(!strcmp(line,"end")) {
			//Only return success if last message is end.
			break;
		} else {
			debug(D_DS, "%s (%s): sent invalid response to send_results: %s",w->hostname,w->addrport,line);
			result = DS_WORKER_FAILURE;
			break;
		}
	}

	if(result != DS_SUCCESS) {
		handle_worker_failure(q, w);
	}

	return result;
}

/*
Compute the total quantity of resources needed by all tasks in
the ready and running states.  This gives us a complete picture
of the manager's resource consumption for status reporting.
*/

static struct rmsummary  *total_resources_needed(struct ds_manager *q) {

	struct ds_task *t;

	struct rmsummary *total = rmsummary_create(0);

	/* for waiting tasks, we use what they would request if dispatched right now. */
	list_first_item(q->ready_list);
	while((t = list_next_item(q->ready_list))) {
		const struct rmsummary *s = ds_manager_task_min_resources(q, t);
		rmsummary_add(total, s);
	}

	/* for running tasks, we use what they have been allocated already. */
	char *key;
	struct ds_worker_info *w;
	hash_table_firstkey(q->worker_table);

	while(hash_table_nextkey(q->worker_table, &key, (void **) &w)) {
		if(w->resources->tag < 0) {
			continue;
		}

		total->cores  += w->resources->cores.inuse;
		total->memory += w->resources->memory.inuse;
		total->disk   += w->resources->disk.inuse;
		total->gpus   += w->resources->gpus.inuse;
	}

	return total;
}

/*
Compute the largest resource request for any task in a given category.
*/

static const struct rmsummary *largest_seen_resources(struct ds_manager *q, const char *category) {
	char *key;
	struct category *c;

	if(category) {
		c = ds_category_lookup_or_create(q, category);
		return c->max_allocation;
	} else {
		hash_table_firstkey(q->categories);
		while(hash_table_nextkey(q->categories, &key, (void **) &c)) {
			rmsummary_merge_max(q->max_task_resources_requested, c->max_allocation);
		}
		return q->max_task_resources_requested;
	}
}

/* Return true if this worker can satisfy the given resource request. */

static int check_worker_fit(struct ds_worker_info *w, const struct rmsummary *s) {

	if(w->resources->workers.total < 1)
		return 0;

	if(!s)
		return w->resources->workers.total;

	if(s->cores > w->resources->cores.largest)
		return 0;
	if(s->memory > w->resources->memory.largest)
		return 0;
	if(s->disk > w->resources->disk.largest)
		return 0;
	if(s->gpus > w->resources->gpus.largest)
		return 0;

	return w->resources->workers.total;
}

static int count_workers_for_waiting_tasks(struct ds_manager *q, const struct rmsummary *s) {

	int count = 0;

	char *key;
	struct ds_worker_info *w;
	hash_table_firstkey(q->worker_table);
	while(hash_table_nextkey(q->worker_table, &key, (void**)&w)) {
		count += check_worker_fit(w, s);
	}

	return count;
}

static void category_jx_insert_max(struct jx *j, struct category *c, const char *field, const struct rmsummary *largest) {

	double l = rmsummary_get(largest, field);
	double m = -1;
	double e = -1;

	if(c) {
		m = rmsummary_get(c->max_resources_seen, field);
		if(c->max_resources_seen->limits_exceeded) {
			e = rmsummary_get(c->max_resources_seen->limits_exceeded, field);
		}
	}

	char *field_str = string_format("max_%s", field);

	if(l > -1){
		char *max_str = string_format("%s", rmsummary_resource_to_str(field, l, 0));
		jx_insert_string(j, field_str, max_str);
		free(max_str);
	} else if(c && !category_in_steady_state(c) && e > -1) {
		char *max_str = string_format(">%s", rmsummary_resource_to_str(field, m - 1, 0));
		jx_insert_string(j, field_str, max_str);
		free(max_str);
	} else if(c && m > -1) {
		char *max_str = string_format("~%s", rmsummary_resource_to_str(field, m, 0));
		jx_insert_string(j, field_str, max_str);
		free(max_str);
	} else {
		jx_insert_string(j, field_str, "na");
	}

	free(field_str);
}

/* Create a dummy task to obtain first allocation that category would get if using largest worker */

static struct rmsummary *category_alloc_info(struct ds_manager *q, struct category *c, category_allocation_t request) {
	struct ds_task *t = ds_task_create("nop");
	ds_task_specify_category(t, c->name);
	t->resource_request = request;

	/* XXX this seems like a hack: a ds_worker is being created by malloc instead of ds_worker_create */

	struct ds_worker_info *w = malloc(sizeof(*w));
	w->resources = ds_resources_create();
	w->resources->cores.largest = q->current_max_worker->cores;
	w->resources->memory.largest = q->current_max_worker->memory;
	w->resources->disk.largest = q->current_max_worker->disk;
	w->resources->gpus.largest = q->current_max_worker->gpus;

	struct rmsummary *allocation = ds_manager_choose_resources_for_task(q, w, t);

	ds_task_delete(t);
	ds_resources_delete(w->resources);
	free(w);

	return allocation;
}

/* Convert an allocation of resources into a JX record. */

static struct jx * alloc_to_jx(struct ds_manager *q, struct category *c, struct rmsummary *resources) {
	struct jx *j = jx_object(0);

	jx_insert_double(j, "cores", resources->cores);
	jx_insert_integer(j, "memory", resources->memory);
	jx_insert_integer(j, "disk", resources->disk);
	jx_insert_integer(j, "gpus", resources->gpus);

	return j;
}

/* Convert a resource category into a JX record for reporting to the catalog. */

static struct jx * category_to_jx(struct ds_manager *q, const char *category) {
	struct ds_stats s;
	struct category *c = NULL;
	const struct rmsummary *largest = largest_seen_resources(q, category);

	c = ds_category_lookup_or_create(q, category);
	ds_get_stats_category(q, category, &s);

	if(s.tasks_waiting + s.tasks_on_workers + s.tasks_done < 1) {
		return NULL;
	}

	struct jx *j = jx_object(0);

	jx_insert_string(j,  "category",         category);
	jx_insert_integer(j, "tasks_waiting",    s.tasks_waiting);
	jx_insert_integer(j, "tasks_running",    s.tasks_running);
	jx_insert_integer(j, "tasks_on_workers", s.tasks_on_workers);
	jx_insert_integer(j, "tasks_dispatched", s.tasks_dispatched);
	jx_insert_integer(j, "tasks_done",       s.tasks_done);
	jx_insert_integer(j, "tasks_failed",     s.tasks_failed);
	jx_insert_integer(j, "tasks_cancelled",  s.tasks_cancelled);
	jx_insert_integer(j, "workers_able",	 s.workers_able);

	category_jx_insert_max(j, c, "cores",  largest);
	category_jx_insert_max(j, c, "memory", largest);
	category_jx_insert_max(j, c, "disk",   largest);
	category_jx_insert_max(j, c, "gpus",   largest);

	struct rmsummary *first_allocation = category_alloc_info(q, c, CATEGORY_ALLOCATION_FIRST);
	struct jx *jr = alloc_to_jx(q, c, first_allocation);
	rmsummary_delete(first_allocation);
	jx_insert(j, jx_string("first_allocation"), jr);

	struct rmsummary *max_allocation = category_alloc_info(q, c, CATEGORY_ALLOCATION_MAX);
	jr = alloc_to_jx(q, c, max_allocation);
	rmsummary_delete(max_allocation);
	jx_insert(j, jx_string("max_allocation"), jr);

	if(q->monitor_mode) {
		jr = alloc_to_jx(q, c, c->max_resources_seen);
		jx_insert(j, jx_string("max_seen"), jr);
	}

	jx_insert_integer(j, "first_allocation_count", task_request_count(q, c->name, CATEGORY_ALLOCATION_FIRST));
	jx_insert_integer(j, "max_allocation_count",   task_request_count(q, c->name, CATEGORY_ALLOCATION_MAX));

	return j;
}

/* Convert all resource categories into a JX record. */

static struct jx *categories_to_jx(struct ds_manager *q) {
	struct jx *a = jx_array(0);

	struct category *c;
	char *category_name;
	hash_table_firstkey(q->categories);
	while(hash_table_nextkey(q->categories, &category_name, (void **) &c)) {
		struct jx *j = category_to_jx(q, category_name);
		if(j) {
			jx_array_insert(a, j);
		}
	}

	//overall queue
	struct jx *j = category_to_jx(q, NULL);
	if(j) {
		jx_array_insert(a, j);
	}

	return a;
}

/*
queue_to_jx examines the overall queue status and creates
an jx expression which can be sent directly to the
user that connects via ds_status.
*/

static struct jx * queue_to_jx( struct ds_manager *q )
{
	struct jx *j = jx_object(0);
	if(!j) return 0;

	// Insert all properties from ds_stats

	struct ds_stats info;
	ds_get_stats(q,&info);

	// Add special properties expected by the catalog server
	char owner[USERNAME_MAX];
	username_get(owner);

	jx_insert_string(j,"type","ds_master");
	if(q->name) jx_insert_string(j,"project",q->name);
	jx_insert_integer(j,"starttime",(q->stats->time_when_started/1000000)); // catalog expects time_t not timestamp_t
	jx_insert_string(j,"working_dir",q->workingdir);
	jx_insert_string(j,"owner",owner);
	jx_insert_string(j,"version",CCTOOLS_VERSION);
	jx_insert_integer(j,"port",ds_port(q));
	jx_insert_integer(j,"priority",q->priority);
	jx_insert_string(j,"manager_preferred_connection",q->manager_preferred_connection);

	int use_ssl = 0;
#ifdef HAS_OPENSSL
	if(q->ssl_enabled) {
		use_ssl = 1;
	}
#endif
	jx_insert_boolean(j,"ssl",use_ssl);

	struct jx *interfaces = interfaces_of_host();
	if(interfaces) {
		jx_insert(j,jx_string("network_interfaces"),interfaces);
	}

	//send info on workers
	jx_insert_integer(j,"workers",info.workers_connected);
	jx_insert_integer(j,"workers_connected",info.workers_connected);
	jx_insert_integer(j,"workers_init",info.workers_init);
	jx_insert_integer(j,"workers_idle",info.workers_idle);
	jx_insert_integer(j,"workers_busy",info.workers_busy);
	jx_insert_integer(j,"workers_able",info.workers_able);

	jx_insert_integer(j,"workers_joined",info.workers_joined);
	jx_insert_integer(j,"workers_removed",info.workers_removed);
	jx_insert_integer(j,"workers_released",info.workers_released);
	jx_insert_integer(j,"workers_idled_out",info.workers_idled_out);
	jx_insert_integer(j,"workers_fast_aborted",info.workers_fast_aborted);
	jx_insert_integer(j,"workers_lost",info.workers_lost);

	//workers_blocked adds host names, not a count
	struct jx *blocklist = ds_blocklist_to_jx(q);
	if(blocklist) {
		jx_insert(j,jx_string("workers_blocked"), blocklist);
	}


	//send info on tasks
	jx_insert_integer(j,"tasks_waiting",info.tasks_waiting);
	jx_insert_integer(j,"tasks_on_workers",info.tasks_on_workers);
	jx_insert_integer(j,"tasks_running",info.tasks_running);
	jx_insert_integer(j,"tasks_with_results",info.tasks_with_results);
	jx_insert_integer(j,"tasks_left",q->num_tasks_left);

	jx_insert_integer(j,"tasks_submitted",info.tasks_submitted);
	jx_insert_integer(j,"tasks_dispatched",info.tasks_dispatched);
	jx_insert_integer(j,"tasks_done",info.tasks_done);
	jx_insert_integer(j,"tasks_failed",info.tasks_failed);
	jx_insert_integer(j,"tasks_cancelled",info.tasks_cancelled);
	jx_insert_integer(j,"tasks_exhausted_attempts",info.tasks_exhausted_attempts);

	// tasks_complete is deprecated, but the old ds_status expects it.
	jx_insert_integer(j,"tasks_complete",info.tasks_done);

	//send info on queue
	jx_insert_integer(j,"time_when_started",info.time_when_started);
	jx_insert_integer(j,"time_send",info.time_send);
	jx_insert_integer(j,"time_receive",info.time_receive);
	jx_insert_integer(j,"time_send_good",info.time_send_good);
	jx_insert_integer(j,"time_receive_good",info.time_receive_good);
	jx_insert_integer(j,"time_status_msgs",info.time_status_msgs);
	jx_insert_integer(j,"time_internal",info.time_internal);
	jx_insert_integer(j,"time_polling",info.time_polling);
	jx_insert_integer(j,"time_application",info.time_application);

	jx_insert_integer(j,"time_workers_execute",info.time_workers_execute);
	jx_insert_integer(j,"time_workers_execute_good",info.time_workers_execute_good);
	jx_insert_integer(j,"time_workers_execute_exhaustion",info.time_workers_execute_exhaustion);

	jx_insert_integer(j,"bytes_sent",info.bytes_sent);
	jx_insert_integer(j,"bytes_received",info.bytes_received);

	jx_insert_integer(j,"capacity_tasks",info.capacity_tasks);
	jx_insert_integer(j,"capacity_cores",info.capacity_cores);
	jx_insert_integer(j,"capacity_memory",info.capacity_memory);
	jx_insert_integer(j,"capacity_disk",info.capacity_disk);
	jx_insert_integer(j,"capacity_gpus",info.capacity_gpus);
	jx_insert_integer(j,"capacity_instantaneous",info.capacity_instantaneous);
	jx_insert_integer(j,"capacity_weighted",info.capacity_weighted);
	jx_insert_integer(j,"manager_load",info.manager_load);

	// Add the resources computed from tributary workers.
	struct ds_resources r;
	aggregate_workers_resources(q,&r,NULL);
	ds_resources_add_to_jx(&r,j);

	//add the stats per category
	jx_insert(j, jx_string("categories"), categories_to_jx(q));

	//add total resources used/needed by the queue
	struct rmsummary *total = total_resources_needed(q);
	jx_insert_integer(j,"tasks_total_cores",total->cores);
	jx_insert_integer(j,"tasks_total_memory",total->memory);
	jx_insert_integer(j,"tasks_total_disk",total->disk);
	jx_insert_integer(j,"tasks_total_gpus",total->gpus);
	rmsummary_delete(total);

	return j;
}

/*
queue_lean_to_jx examines the overall queue status and creates
an jx expression which can be sent to the catalog.
It different from queue_to_jx in that only the minimum information that
workers, ds_status and the ds_factory need.
*/

static struct jx * queue_lean_to_jx( struct ds_manager *q )
{
	struct jx *j = jx_object(0);
	if(!j) return 0;

	// Insert all properties from ds_stats

	struct ds_stats info;
	ds_get_stats(q,&info);

	//information regarding how to contact the manager
	jx_insert_string(j,"version",CCTOOLS_VERSION);
	jx_insert_string(j,"type","ds_master");
	jx_insert_integer(j,"port",ds_port(q));

	int use_ssl = 0;
#ifdef HAS_OPENSSL
	if(q->ssl_enabled) {
		use_ssl = 1;
	}
#endif
	jx_insert_boolean(j,"ssl",use_ssl);

	char owner[USERNAME_MAX];
	username_get(owner);
	jx_insert_string(j,"owner",owner);

	if(q->name) jx_insert_string(j,"project",q->name);
	jx_insert_integer(j,"starttime",(q->stats->time_when_started/1000000)); // catalog expects time_t not timestamp_t
	jx_insert_string(j,"manager_preferred_connection",q->manager_preferred_connection);



	struct jx *interfaces = interfaces_of_host();
	if(interfaces) {
		jx_insert(j,jx_string("network_interfaces"),interfaces);
	}

	//task information for general ds_status report
	jx_insert_integer(j,"tasks_waiting",info.tasks_waiting);
	jx_insert_integer(j,"tasks_running",info.tasks_running);
	jx_insert_integer(j,"tasks_complete",info.tasks_done);    // tasks_complete is deprecated, but the old ds_status expects it.

	//additional task information for ds_factory
	jx_insert_integer(j,"tasks_on_workers",info.tasks_on_workers);
	jx_insert_integer(j,"tasks_left",q->num_tasks_left);

	//capacity information the factory needs
	jx_insert_integer(j,"capacity_tasks",info.capacity_tasks);
	jx_insert_integer(j,"capacity_cores",info.capacity_cores);
	jx_insert_integer(j,"capacity_memory",info.capacity_memory);
	jx_insert_integer(j,"capacity_disk",info.capacity_disk);
	jx_insert_integer(j,"capacity_gpus",info.capacity_gpus);
	jx_insert_integer(j,"capacity_weighted",info.capacity_weighted);
	jx_insert_double(j,"manager_load",info.manager_load);

	//resources information the factory needs
	struct rmsummary *total = total_resources_needed(q);
	jx_insert_integer(j,"tasks_total_cores",total->cores);
	jx_insert_integer(j,"tasks_total_memory",total->memory);
	jx_insert_integer(j,"tasks_total_disk",total->disk);
	jx_insert_integer(j,"tasks_total_gpus",total->gpus);

	//worker information for general ds_status report
	jx_insert_integer(j,"workers",info.workers_connected);
	jx_insert_integer(j,"workers_connected",info.workers_connected);


	//additional worker information the factory needs
	struct jx *blocklist = ds_blocklist_to_jx(q);
	if(blocklist) {
		jx_insert(j,jx_string("workers_blocked"), blocklist);   //danger! unbounded field
	}

	return j;
}

/*
Send a brief human-readable index listing the data
types that can be queried via this API.
*/

static void handle_data_index( struct ds_manager *q, struct ds_worker_info *w, time_t stoptime )
{
	buffer_t buf;
	buffer_init(&buf);

	buffer_printf(&buf,"<h1>Dataswarm Data API</h1>");
        buffer_printf(&buf,"<ul>\n");
	buffer_printf(&buf,"<li> <a href=\"/queue_status\">Queue Status</a>\n");
	buffer_printf(&buf,"<li> <a href=\"/task_status\">Task Status</a>\n");
	buffer_printf(&buf,"<li> <a href=\"/worker_status\">Worker Status</a>\n");
	buffer_printf(&buf,"<li> <a href=\"/resources_status\">Resources Status</a>\n");
        buffer_printf(&buf,"</ul>\n");

	ds_manager_send(q,w,buffer_tostring(&buf),buffer_pos(&buf),stoptime);

	buffer_free(&buf);

}

/*
Process an HTTP request that comes in via a worker port.
This represents a web browser that connected directly
to the manager to fetch status data.
*/

static ds_msg_code_t handle_http_request( struct ds_manager *q, struct ds_worker_info *w, const char *path, time_t stoptime )
{
	char line[DS_LINE_MAX];

	// Consume (and ignore) the remainder of the headers.
	while(link_readline(w->link,line,DS_LINE_MAX,stoptime)) {
		if(line[0]==0) break;
	}

	ds_manager_send(q,w,"HTTP/1.1 200 OK\nConnection: close\n");
	if(!strcmp(path,"/")) {
	        // Requests to root get a simple human readable index.
		ds_manager_send(q,w,"Content-type: text/html\n\n");
		handle_data_index(q, w, stoptime );
	} else {
	        // Other requests get raw JSON data.
		ds_manager_send(q,w,"Access-Control-Allow-Origin: *\n");
		ds_manager_send(q,w,"Content-type: text/plain\n\n");
		handle_queue_status(q, w, &path[1], stoptime );
	}

	// Return success but require a disconnect now.
	return DS_MSG_PROCESSED_DISCONNECT;
}

/*
Process a queue status request which returns raw JSON.
This could come via the HTTP interface, or via a plain request.
*/

static struct jx *construct_status_message( struct ds_manager *q, const char *request ) {
	struct jx *a = jx_array(NULL);

	if(!strcmp(request, "queue_status") || !strcmp(request, "queue") || !strcmp(request, "resources_status")) {
		struct jx *j = queue_to_jx( q );
		if(j) {
			jx_array_insert(a, j);
		}
	} else if(!strcmp(request, "task_status") || !strcmp(request, "tasks")) {
		struct ds_task *t;
		uint64_t taskid;

		itable_firstkey(q->tasks);
		while(itable_nextkey(q->tasks,&taskid,(void**)&t)) {
			struct jx *j = ds_task_to_jx(q,t);
			if(j) jx_array_insert(a, j);
		}
	} else if(!strcmp(request, "worker_status") || !strcmp(request, "workers")) {
		struct ds_worker_info *w;
		struct jx *j;
		char *key;

		hash_table_firstkey(q->worker_table);
		while(hash_table_nextkey(q->worker_table,&key,(void**)&w)) {
			// If the worker has not been initialized, ignore it.
			if(!strcmp(w->hostname, "unknown")) continue;
			j = ds_worker_to_jx(w);
			if(j) {
				jx_array_insert(a, j);
			}
		}
	} else if(!strcmp(request, "wable_status") || !strcmp(request, "categories")) {
		jx_delete(a);
		a = categories_to_jx(q);
	} else {
		debug(D_WQ, "Unknown status request: '%s'", request);
		jx_delete(a);
		a = NULL;
	}

	return a;
}

/*
Handle a queue status message by composing a response and sending it.
*/


static ds_msg_code_t handle_queue_status( struct ds_manager *q, struct ds_worker_info *target, const char *line, time_t stoptime )
{
	struct link *l = target->link;

	struct jx *a = construct_status_message(q, line);
	target->type = DS_WORKER_TYPE_STATUS;

	free(target->hostname);
	target->hostname = xxstrdup("QUEUE_STATUS");

	if(!a) {
		debug(D_WQ, "Unknown status request: '%s'", line);
		return DS_MSG_FAILURE;
	}

	jx_print_link(a,l,stoptime);
	jx_delete(a);

	return DS_MSG_PROCESSED_DISCONNECT;
}

/*
Handle a resource update message from the worker by updating local structures.
*/


static ds_msg_code_t handle_resource( struct ds_manager *q, struct ds_worker_info *w, const char *line )
{
	char resource_name[DS_LINE_MAX];
	struct ds_resource r;

	int n = sscanf(line, "resource %s %"PRId64" %"PRId64" %"PRId64, resource_name, &r.total, &r.smallest, &r.largest);

	if(n == 2 && !strcmp(resource_name,"tag"))
	{
		/* Shortcut, total has the tag, as "resources tag" only sends one value */
		w->resources->tag = r.total;
	} else if(n == 4) {

		/* inuse is computed by the manager, so we save it here */
		int64_t inuse;

		if(!strcmp(resource_name,"cores")) {
			inuse = w->resources->cores.inuse;
			w->resources->cores = r;
			w->resources->cores.inuse = inuse;
		} else if(!strcmp(resource_name,"memory")) {
			inuse = w->resources->memory.inuse;
			w->resources->memory = r;
			w->resources->memory.inuse = inuse;
		} else if(!strcmp(resource_name,"disk")) {
			inuse = w->resources->disk.inuse;
			w->resources->disk = r;
			w->resources->disk.inuse = inuse;
		} else if(!strcmp(resource_name,"gpus")) {
			inuse = w->resources->gpus.inuse;
			w->resources->gpus = r;
			w->resources->gpus.inuse = inuse;
		} else if(!strcmp(resource_name,"workers")) {
			inuse = w->resources->workers.inuse;
			w->resources->workers = r;
			w->resources->workers.inuse = inuse;
		}
	} else {
		return DS_MSG_FAILURE;
	}

	return DS_MSG_PROCESSED;
}

/*
Handle a feature report from a worker, which describes properties set
manually by the user, like a particular GPU model, software installed, etc.
*/


static ds_msg_code_t handle_feature( struct ds_manager *q, struct ds_worker_info *w, const char *line )
{
	char feature[DS_LINE_MAX];
	char fdec[DS_LINE_MAX];

	int n = sscanf(line, "feature %s", feature);

	if(n != 1) {
		return DS_MSG_FAILURE;
	}

	if(!w->features)
		w->features = hash_table_create(4,0);

	url_decode(feature, fdec, DS_LINE_MAX);

	debug(D_DS, "Feature found: %s\n", fdec);

	hash_table_insert(w->features, fdec, (void **) 1);

	return DS_MSG_PROCESSED;
}

/*
Handle activity on a network connection by looking up the mapping
between the link and the ds_worker, then processing on or more
messages available.
*/

static ds_result_code_t handle_worker(struct ds_manager *q, struct link *l)
{
	char line[DS_LINE_MAX];
	struct ds_worker_info *w;

	char *key = link_to_hash_key(l);
	w = hash_table_lookup(q->worker_table, key);
	free(key);

	ds_msg_code_t mcode;
	mcode = ds_manager_recv(q, w, line, sizeof(line));

	// We only expect asynchronous status queries and updates here.

	switch(mcode) {
		case DS_MSG_PROCESSED:
			// A status message was received and processed.
			return DS_SUCCESS;
			break;

		case DS_MSG_PROCESSED_DISCONNECT:
			// A status query was received and processed, so disconnect.
			remove_worker(q, w, DS_WORKER_DISCONNECT_STATUS_WORKER);
			return DS_SUCCESS;

		case DS_MSG_NOT_PROCESSED:
			debug(D_DS, "Invalid message from worker %s (%s): %s", w->hostname, w->addrport, line);
			q->stats->workers_lost++;
			remove_worker(q, w, DS_WORKER_DISCONNECT_FAILURE);
			return DS_WORKER_FAILURE;
			break;

		case DS_MSG_FAILURE:
			debug(D_DS, "Failed to read from worker %s (%s)", w->hostname, w->addrport);
			q->stats->workers_lost++;
			remove_worker(q, w, DS_WORKER_DISCONNECT_FAILURE);
			return DS_WORKER_FAILURE;
	}

	return DS_SUCCESS;
}

/*
Construct the table of network links to be considered,
including the manager's accepting link, and one for
each active worker.
*/


static int build_poll_table(struct ds_manager *q )
{
	int n = 0;
	char *key;
	struct ds_worker_info *w;

	// Allocate a small table, if it hasn't been done yet.
	if(!q->poll_table) {
		q->poll_table = malloc(sizeof(*q->poll_table) * q->poll_table_size);
		if(!q->poll_table) {
			//if we can't allocate a poll table, we can't do anything else.
			fatal("allocating memory for poll table failed.");
		}
	}

	// The first item in the poll table is the manager link, which accepts new connections.
	q->poll_table[0].link = q->manager_link;
	q->poll_table[0].events = LINK_READ;
	q->poll_table[0].revents = 0;
	n = 1;

	// For every worker in the hash table, add an item to the poll table
	hash_table_firstkey(q->worker_table);
	while(hash_table_nextkey(q->worker_table, &key, (void **) &w)) {
		// If poll table is not large enough, reallocate it
		if(n >= q->poll_table_size) {
			q->poll_table_size *= 2;
			q->poll_table = realloc(q->poll_table, sizeof(*q->poll_table) * q->poll_table_size);
			if(q->poll_table == NULL) {
				//if we can't allocate a poll table, we can't do anything else.
				fatal("reallocating memory for poll table failed.");
			}
		}

		q->poll_table[n].link = w->link;
		q->poll_table[n].events = LINK_READ;
		q->poll_table[n].revents = 0;
		n++;
	}

	return n;
}

/*
Determine the resources to allocate for a given task when assigned to a specific worker.
*/

struct rmsummary *ds_manager_choose_resources_for_task( struct ds_manager *q, struct ds_worker_info *w, struct ds_task *t ) {

	const struct rmsummary *min = ds_manager_task_min_resources(q, t);
	const struct rmsummary *max = ds_manager_task_max_resources(q, t);

	struct rmsummary *limits = rmsummary_create(-1);

	rmsummary_merge_override(limits, max);

	int use_whole_worker = 1;

	struct category *c = ds_category_lookup_or_create(q, t->category);
	if(q->force_proportional_resources || c->allocation_mode == CATEGORY_ALLOCATION_MODE_FIXED) {
		double max_proportion = -1;
		if(w->resources->cores.largest > 0) {
			max_proportion = MAX(max_proportion, limits->cores / w->resources->cores.largest);
		}

		if(w->resources->memory.largest > 0) {
			max_proportion = MAX(max_proportion, limits->memory / w->resources->memory.largest);
		}

		if(w->resources->disk.largest > 0) {
			max_proportion = MAX(max_proportion, limits->disk / w->resources->disk.largest);
		}

		if(w->resources->gpus.largest > 0) {
			max_proportion = MAX(max_proportion, limits->gpus / w->resources->gpus.largest);
		}

		//if max_proportion > 1, then the task does not fit the worker for the
		//specified resources. For the unspecified resources we use the whole
		//worker as not to trigger a warning when checking for tasks that can't
		//run on any available worker.
		if (max_proportion > 1){
			use_whole_worker = 1;
		}
		else if(max_proportion > 0) {
			use_whole_worker = 0;

			// adjust max_proportion so that an integer number of tasks fit the
			// worker.
			if(q->force_proportional_resources) {
				max_proportion = 1.0/(floor(1.0/max_proportion));
			}

			/* when cores are unspecified, they are set to 0 if gpus are specified.
			 * Otherwise they get a proportion according to specified
			 * resources. Tasks will get at least one core. */
			if(q->force_proportional_resources || limits->cores < 0) {
				if(limits->gpus > 0) {
					limits->cores = 0;
				} else {
					limits->cores = MAX(1, floor(w->resources->cores.largest * max_proportion));
				}
			}

			if(limits->gpus < 0) {
				/* unspecified gpus are always 0 */
				limits->gpus = 0;
			}

			if(q->force_proportional_resources || limits->memory < 0) {
				limits->memory = MAX(1, floor(w->resources->memory.largest * max_proportion));
			}

			if(q->force_proportional_resources || limits->disk < 0) {
				limits->disk = MAX(1, floor(w->resources->disk.largest * max_proportion));
			}
		}
	}

	if(limits->cores < 1 && limits->gpus < 1 && limits->memory < 1 && limits->disk < 1) {
		/* no resource was specified, using whole worker */
		use_whole_worker = 1;
	}

	if((limits->cores > 0 && limits->cores >= w->resources->cores.largest) ||
			(limits->gpus > 0 && limits->gpus >= w->resources->gpus.largest) ||
			(limits->memory > 0 && limits->memory >= w->resources->memory.largest) ||
			(limits->disk > 0 && limits->disk >= w->resources->disk.largest)) {
		/* at least one specified resource would use the whole worker, thus
		 * using whole worker for all unspecified resources. */
		use_whole_worker = 1;
	}

	if(use_whole_worker) {
		/* default cores for tasks that define gpus is 0 */
		if(limits->cores <= 0) {
			limits->cores = limits->gpus > 0 ? 0 : w->resources->cores.largest;
		}

		/* default gpus is 0 */
		if(limits->gpus <= 0) {
			limits->gpus = 0;
		}

		if(limits->memory <= 0) {
			limits->memory = w->resources->memory.largest;
		}

		if(limits->disk <= 0) {
			limits->disk = w->resources->disk.largest;
		}
	}

	/* never go below specified min resources. */
	rmsummary_merge_max(limits, min);

	return limits;
}

/*
Start one task on a given worker by specializing the task to the worker,
sending the appropriate input files, and then sending the details of the task.
Note that the "infile" and "outfile" components of the task refer to
files that have already been uploaded into the worker's cache by the manager.
*/


static ds_result_code_t start_one_task(struct ds_manager *q, struct ds_worker_info *w, struct ds_task *t)
{
	/* wrap command at the last minute, so that we have the updated information
	 * about resources. */
	struct rmsummary *limits = ds_manager_choose_resources_for_task(q, w, t);

	char *command_line;
	if(q->monitor_mode && !t->coprocess) {
		command_line = ds_monitor_wrap(q, w, t, limits);
	} else {
		command_line = xxstrdup(t->command_line);
	}

	ds_result_code_t result = ds_manager_put_input_files(q, w, t);

	if (result != DS_SUCCESS) {
		free(command_line);
		return result;
	}

	ds_manager_send(q,w, "task %lld\n",  (long long) t->taskid);

	long long cmd_len = strlen(command_line);
	ds_manager_send(q,w, "cmd %lld\n", (long long) cmd_len);
	link_putlstring(w->link, command_line, cmd_len, time(0) + q->short_timeout);
	debug(D_DS, "%s\n", command_line);
	free(command_line);


	if(t->coprocess) {
		cmd_len = strlen(t->coprocess);
		ds_manager_send(q,w, "coprocess %lld\n", (long long) cmd_len);
		link_putlstring(w->link, t->coprocess, cmd_len, /* stoptime */ time(0) + q->short_timeout);
	}

	ds_manager_send(q,w, "category %s\n", t->category);

	ds_manager_send(q,w, "cores %s\n",  rmsummary_resource_to_str("cores", limits->cores, 0));
	ds_manager_send(q,w, "gpus %s\n",   rmsummary_resource_to_str("gpus", limits->gpus, 0));
	ds_manager_send(q,w, "memory %s\n", rmsummary_resource_to_str("memory", limits->memory, 0));
	ds_manager_send(q,w, "disk %s\n",   rmsummary_resource_to_str("disk", limits->disk, 0));

	/* Do not specify end, wall_time if running the resource monitor. We let the monitor police these resources. */
	if(q->monitor_mode == DS_MON_DISABLED) {
		if(limits->end > 0) {
			ds_manager_send(q,w, "end_time %s\n",  rmsummary_resource_to_str("end", limits->end, 0));
		}
		if(limits->wall_time > 0) {
			ds_manager_send(q,w, "wall_time %s\n", rmsummary_resource_to_str("wall_time", limits->wall_time, 0));
		}
	}

	itable_insert(w->current_tasks_boxes, t->taskid, limits);
	rmsummary_merge_override(t->resources_allocated, limits);

	/* Note that even when environment variables after resources, values for
	 * CORES, MEMORY, etc. will be set at the worker to the values of
	 * specify_*, if used. */
	char *var;
	list_first_item(t->env_list);
	while((var=list_next_item(t->env_list))) {
		ds_manager_send(q, w,"env %zu\n%s\n", strlen(var), var);
	}

	if(t->input_files) {
		struct ds_file *tf;
		list_first_item(t->input_files);
		while((tf = list_next_item(t->input_files))) {
			if(tf->type == DS_DIRECTORY) {
				ds_manager_send(q,w, "dir %s\n", tf->remote_name);
			} else {
				char remote_name_encoded[PATH_MAX];
				url_encode(tf->remote_name, remote_name_encoded, PATH_MAX);
				ds_manager_send(q,w, "infile %s %s %d\n", tf->cached_name, remote_name_encoded, tf->flags);
			}
		}
	}

	if(t->output_files) {
		struct ds_file *tf;
		list_first_item(t->output_files);
		while((tf = list_next_item(t->output_files))) {
			char remote_name_encoded[PATH_MAX];
			url_encode(tf->remote_name, remote_name_encoded, PATH_MAX);
			ds_manager_send(q,w, "outfile %s %s %d\n", tf->cached_name, remote_name_encoded, tf->flags);
		}
	}

	// ds_manager_send returns the number of bytes sent, or a number less than
	// zero to indicate errors. We are lazy here, we only check the last
	// message we sent to the worker (other messages may have failed above).
	int result_msg = ds_manager_send(q,w,"end\n");

	if(result_msg > -1)
	{
		debug(D_DS, "%s (%s) busy on '%s'", w->hostname, w->addrport, t->command_line);
		return DS_SUCCESS;
	}
	else
	{
		return DS_WORKER_FAILURE;
	}
}

static void compute_manager_load(struct ds_manager *q, int task_activity) {

	double alpha = 0.05;

	double load = q->stats->manager_load;

	if(task_activity) {
		load = load * (1 - alpha) + 1 * alpha;
	} else {
		load = load * (1 - alpha) + 0 * alpha;
	}

	q->stats->manager_load = load;
}

static void count_worker_resources(struct ds_manager *q, struct ds_worker_info *w)
{
	struct rmsummary *box;
	uint64_t taskid;

	w->resources->cores.inuse  = 0;
	w->resources->memory.inuse = 0;
	w->resources->disk.inuse   = 0;
	w->resources->gpus.inuse   = 0;

	update_max_worker(q, w);

	if(w->resources->workers.total < 1)
	{
		return;
	}

	itable_firstkey(w->current_tasks_boxes);
	while(itable_nextkey(w->current_tasks_boxes, &taskid, (void **)& box)) {
		w->resources->cores.inuse     += box->cores;
		w->resources->memory.inuse    += box->memory;
		w->resources->disk.inuse      += box->disk;
		w->resources->gpus.inuse      += box->gpus;
	}
}

static void update_max_worker(struct ds_manager *q, struct ds_worker_info *w) {
	if(!w)
		return;

	if(w->resources->workers.total < 1) {
		return;
	}

	if(q->current_max_worker->cores < w->resources->cores.largest) {
		q->current_max_worker->cores = w->resources->cores.largest;
	}

	if(q->current_max_worker->memory < w->resources->memory.largest) {
		q->current_max_worker->memory = w->resources->memory.largest;
	}

	if(q->current_max_worker->disk < w->resources->disk.largest) {
		q->current_max_worker->disk = w->resources->disk.largest;
	}

	if(q->current_max_worker->gpus < w->resources->gpus.largest) {
		q->current_max_worker->gpus = w->resources->gpus.largest;
	}
}

/* we call this function when a worker is disconnected. For efficiency, we use
 * update_max_worker when a worker sends resource updates. */
static void find_max_worker(struct ds_manager *q) {
	q->current_max_worker->cores  = 0;
	q->current_max_worker->memory = 0;
	q->current_max_worker->disk   = 0;
	q->current_max_worker->gpus   = 0;

	char *key;
	struct ds_worker_info *w;
	hash_table_firstkey(q->worker_table);
	while(hash_table_nextkey(q->worker_table, &key, (void **) &w)) {
		if(w->resources->workers.total > 0)
		{
			update_max_worker(q, w);
		}
	}
}

/*
Commit a given task to a worker by sending the task details,
then updating all auxiliary data structures to note the
assignment and the new task state.
*/


static void commit_task_to_worker(struct ds_manager *q, struct ds_worker_info *w, struct ds_task *t)
{
	t->hostname = xxstrdup(w->hostname);
	t->addrport = xxstrdup(w->addrport);

	t->time_when_commit_start = timestamp_get();
	ds_result_code_t result = start_one_task(q, w, t);
	t->time_when_commit_end = timestamp_get();

	itable_insert(w->current_tasks, t->taskid, t);

	t->worker = w;

	change_task_state(q, t, DS_TASK_RUNNING);

	t->try_count += 1;
	q->stats->tasks_dispatched += 1;

	count_worker_resources(q, w);

	if(result != DS_SUCCESS) {
		debug(D_DS, "Failed to send task %d to worker %s (%s).", t->taskid, w->hostname, w->addrport);
		handle_failure(q, w, t, result);
	}
}

/*
Collect a completed task from a worker, and then update
all auxiliary data structures to remove the association
and change the task state.
*/

static void reap_task_from_worker(struct ds_manager *q, struct ds_worker_info *w, struct ds_task *t, ds_task_state_t new_state)
{
	struct ds_worker_info *wr = t->worker;
	if(wr != w)
	{
		/* XXX this seems like a bug, should we return quickly here? */
		debug(D_DS, "Cannot reap task %d from worker. It is not being run by %s (%s)\n", t->taskid, w->hostname, w->addrport);
	} else {
		w->total_task_time += t->time_workers_execute_last;
	}

	struct rmsummary *task_box = itable_lookup(w->current_tasks_boxes, t->taskid);
	if(task_box)
		rmsummary_delete(task_box);

	itable_remove(w->current_tasks_boxes, t->taskid);
	itable_remove(w->current_tasks, t->taskid);

	t->worker = 0;

	change_task_state(q, t, new_state);

	count_worker_resources(q, w);
}

/*
Advance the state of the system by selecting one task available
to run, finding the best worker for that task, and then committing
the task to the worker.
*/

static int send_one_task( struct ds_manager *q )
{
	struct ds_task *t;
	struct ds_worker_info *w;

	timestamp_t now = timestamp_get();

	// Consider each task in the order of priority:
	list_first_item(q->ready_list);
	while( (t = list_next_item(q->ready_list))) {
		// Skip task if min requested start time not met.
		if(t->resources_requested->start > now) continue;

		// Find the best worker for the task at the head of the list
		w = ds_schedule_task_to_worker(q,t);

		// If there is no suitable worker, consider the next task.
		if(!w) continue;
		// Otherwise, remove it from the ready list and start it:
		commit_task_to_worker(q,w,t);

		return 1;
	}

	return 0;
}

/*
Advance the state of the system by finding any task that is
waiting to be retrieved, then fetch the outputs of that task,
and mark it as done.
*/

static int receive_one_task( struct ds_manager *q )
{
	struct ds_task *t;
	uint64_t taskid;

	itable_firstkey(q->tasks);
	while( itable_nextkey(q->tasks, &taskid, (void **) &t) ) {
		if( t->state==DS_TASK_WAITING_RETRIEVAL ) {
			struct ds_worker_info *w = t->worker;
			fetch_output_from_worker(q, w, taskid);
			// Shutdown worker if appropriate.
			if ( w->factory_name ) {
				struct ds_factory_info *f = ds_factory_info_lookup(q,w->factory_name);
				if ( f && f->connected_workers > f->max_workers &&
						itable_size(w->current_tasks) < 1 ) {
					debug(D_DS, "Final task received from worker %s, shutting down.", w->hostname);
					shut_down_worker(q, w);
				}
			}
			return 1;
		}
	}

	return 0;
}

/*
Sends keepalives to check if connected workers are responsive,
and ask for updates If not, removes those workers.
*/

static void ask_for_workers_updates(struct ds_manager *q) {
	struct ds_worker_info *w;
	char *key;
	timestamp_t current_time = timestamp_get();

	hash_table_firstkey(q->worker_table);
	while(hash_table_nextkey(q->worker_table, &key, (void **) &w)) {
		if(q->keepalive_interval > 0) {

			/* we have not received dataswarm message from worker yet, so we
			 * simply check again its start_time. */
			if(!strcmp(w->hostname, "unknown")){
				if ((int)((current_time - w->start_time)/1000000) >= q->keepalive_timeout) {
					debug(D_DS, "Removing worker %s (%s): hasn't sent its initialization in more than %d s", w->hostname, w->addrport, q->keepalive_timeout);
					handle_worker_failure(q, w);
				}
				continue;
			}


			// send new keepalive check only (1) if we received a response since last keepalive check AND
			// (2) we are past keepalive interval
			if(w->last_msg_recv_time > w->last_update_msg_time) {
				int64_t last_update_elapsed_time = (int64_t)(current_time - w->last_update_msg_time)/1000000;
				if(last_update_elapsed_time >= q->keepalive_interval) {
					if(ds_manager_send(q,w, "check\n")<0) {
						debug(D_DS, "Failed to send keepalive check to worker %s (%s).", w->hostname, w->addrport);
						handle_worker_failure(q, w);
					} else {
						debug(D_DS, "Sent keepalive check to worker %s (%s)", w->hostname, w->addrport);
						w->last_update_msg_time = current_time;
					}
				}
			} else {
				// we haven't received a message from worker since its last keepalive check. Check if time
				// since we last polled link for responses has exceeded keepalive timeout. If so, remove worker.
				if (q->link_poll_end > w->last_update_msg_time) {
					if ((int)((q->link_poll_end - w->last_update_msg_time)/1000000) >= q->keepalive_timeout) {
						debug(D_DS, "Removing worker %s (%s): hasn't responded to keepalive check for more than %d s", w->hostname, w->addrport, q->keepalive_timeout);
						handle_worker_failure(q, w);
					}
				}
			}
		}
	}
}

/*
If fast-abort is enabled, then look for workers that
have taken too long to execute a task, and disconnect
them, under the assumption that they are halted or faulty.
*/

static int abort_slow_workers(struct ds_manager *q)
{
	struct category *c;
	char *category_name;

	struct ds_worker_info *w;
	struct ds_task *t;
	uint64_t taskid;

	int removed = 0;

	/* optimization. If no category has a fast abort multiplier, simply return. */
	int fast_abort_flag = 0;

	hash_table_firstkey(q->categories);
	while(hash_table_nextkey(q->categories, &category_name, (void **) &c)) {
		struct ds_stats *stats = c->ds_stats;
		if(!stats) {
			/* no stats have been computed yet */
			continue;
		}

		if(stats->tasks_done < 10) {
			c->average_task_time = 0;
			continue;
		}

		c->average_task_time = (stats->time_workers_execute_good + stats->time_send_good + stats->time_receive_good) / stats->tasks_done;

		if(c->fast_abort > 0)
			fast_abort_flag = 1;
	}

	if(!fast_abort_flag)
		return 0;

	struct category *c_def = ds_category_lookup_or_create(q, "default");

	timestamp_t current = timestamp_get();

	itable_firstkey(q->tasks);
	while(itable_nextkey(q->tasks, &taskid, (void **) &t)) {
		c = ds_category_lookup_or_create(q, t->category);
		/* Fast abort deactivated for this category */
		if(c->fast_abort == 0)
			continue;

		timestamp_t runtime = current - t->time_when_commit_start;
		timestamp_t average_task_time = c->average_task_time;

		/* Not enough samples, skip the task. */
		if(average_task_time < 1)
			continue;

		double multiplier;
		if(c->fast_abort > 0) {
			multiplier = c->fast_abort;
		}
		else if(c_def->fast_abort > 0) {
			/* This category uses the default fast abort. (< 0 use default, 0 deactivate). */
			multiplier = c_def->fast_abort;
		}
		else {
			/* Fast abort also deactivated for the default category. */
			continue;
		}

		if(runtime >= (average_task_time * (multiplier + t->fast_abort_count))) {
			w = t->worker;
			if(w && (w->type == DS_WORKER_TYPE_WORKER))
			{
				debug(D_DS, "Task %d is taking too long. Removing from worker.", t->taskid);
				cancel_task_on_worker(q, t, DS_TASK_READY);
				t->fast_abort_count++;

				/* a task cannot mark two different workers as suspect */
				if(t->fast_abort_count > 1) {
					continue;
				}

				if(w->fast_abort_alarm > 0) {
					/* this is the second task in a row that triggered fast
					 * abort, therefore we have evidence that this a slow
					 * worker (rather than a task) */

					debug(D_DS, "Removing worker %s (%s): takes too long to execute the current task - %.02lf s (average task execution time by other workers is %.02lf s)", w->hostname, w->addrport, runtime / 1000000.0, average_task_time / 1000000.0);
					ds_block_host_with_timeout(q, w->hostname, ds_option_blocklist_slow_workers_timeout);
					remove_worker(q, w, DS_WORKER_DISCONNECT_FAST_ABORT);

					q->stats->workers_fast_aborted++;
					removed++;
				}

				w->fast_abort_alarm = 1;
			}
		}
	}

	return removed;
}

/* Forcibly shutdown a worker by telling it to exit, then disconnect it. */

static int shut_down_worker(struct ds_manager *q, struct ds_worker_info *w)
{
	if(!w) return 0;

	ds_manager_send(q,w,"exit\n");
	remove_worker(q, w, DS_WORKER_DISCONNECT_EXPLICIT);
	q->stats->workers_released++;

	return 1;
}

static int abort_drained_workers(struct ds_manager *q) {
	char *worker_hashkey = NULL;
	struct ds_worker_info *w = NULL;

	int removed = 0;

	hash_table_firstkey(q->worker_table);
	while(hash_table_nextkey(q->worker_table, &worker_hashkey, (void **) &w)) {
		if(w->draining && itable_size(w->current_tasks) == 0) {
			removed++;
			shut_down_worker(q, w);
		}
	}

	return removed;
}

/* Comparator function for checking if a task matches a given tag. */

static int tasktag_comparator(void *t, const void *r) {

	struct ds_task *task_in_queue = t;
	const char *tasktag = r;

	if(!task_in_queue->tag && !tasktag) {
		return 1;
	}

	if(!task_in_queue->tag || !tasktag) {
		return 0;
	}

	return !strcmp(task_in_queue->tag, tasktag);
}

/*
Cancel a specific task already running on a worker,
by sending the appropriate kill message and removing any undesired state.
*/

static int cancel_task_on_worker(struct ds_manager *q, struct ds_task *t, ds_task_state_t new_state) {

	struct ds_worker_info *w = t->worker;
	if (w) {
		//send message to worker asking to kill its task.
		ds_manager_send(q,w, "kill %d\n",t->taskid);
		debug(D_DS, "Task with id %d is aborted at worker %s (%s) and removed.", t->taskid, w->hostname, w->addrport);

		//Delete any input files that are not to be cached.
		delete_worker_files(q, w, t->input_files, DS_CACHE );

		//Delete all output files since they are not needed as the task was aborted.
		delete_worker_files(q, w, t->output_files, 0);

		//update tables.
		reap_task_from_worker(q, w, t, new_state);

		return 1;
	} else {
		change_task_state(q, t, new_state);
		return 0;
	}
}

/* Search for any one task that matches the given tag string. */

static struct ds_task *find_task_by_tag(struct ds_manager *q, const char *tasktag) {
	struct ds_task *t;
	uint64_t taskid;

	itable_firstkey(q->tasks);
	while(itable_nextkey(q->tasks, &taskid, (void**)&t)) {
		if( tasktag_comparator(t, tasktag) ) {
			return t;
		}
	}

	return NULL;
}

/*
Invalidate all remote cached files that match the given name.
Search for workers with that file, cancel any tasks using that
file, and then remove it.
*/

static void ds_invalidate_cached_file_internal(struct ds_manager *q, const char *filename) {
	char *key;
	struct ds_worker_info *w;
	hash_table_firstkey(q->worker_table);
	while(hash_table_nextkey(q->worker_table, &key, (void**)&w)) {
		if(!hash_table_lookup(w->current_files, filename))
			continue;

		struct ds_task *t;
		uint64_t taskid;

		itable_firstkey(w->current_tasks);
		while(itable_nextkey(w->current_tasks, &taskid, (void**)&t)) {
			struct ds_file *tf;
			list_first_item(t->input_files);

			while((tf = list_next_item(t->input_files))) {
				if(strcmp(filename, tf->cached_name) == 0) {
					cancel_task_on_worker(q, t, DS_TASK_READY);
					continue;
				}
			}

			while((tf = list_next_item(t->output_files))) {
				if(strcmp(filename, tf->cached_name) == 0) {
					cancel_task_on_worker(q, t, DS_TASK_READY);
					continue;
				}
			}
		}

		delete_worker_file(q, w, filename, 0, 0);
	}
}

void ds_invalidate_cached_file(struct ds_manager *q, const char *local_name, ds_file_t type) {
	struct ds_file *f = ds_file_create(local_name, local_name, type, DS_CACHE);
	ds_invalidate_cached_file_internal(q, f->cached_name);
	ds_file_delete(f);
}

/******************************************************/
/************* dataswarm public functions *************/
/******************************************************/

struct ds_manager *ds_create(int port) {
	return ds_ssl_create(port, NULL, NULL);
}

struct ds_manager *ds_ssl_create(int port, const char *key, const char *cert)
{
	struct ds_manager *q = malloc(sizeof(*q));
	if(!q) {
		fprintf(stderr, "Error: failed to allocate memory for queue.\n");
		return 0;
	}
	char *envstring;

	random_init();

	memset(q, 0, sizeof(*q));

	if(port == 0) {
		envstring = getenv("DS_PORT");
		if(envstring) {
			port = atoi(envstring);
		}
	}

	/* compatibility code */
	if (getenv("DS_LOW_PORT"))
		setenv("TCP_LOW_PORT", getenv("DS_LOW_PORT"), 0);
	if (getenv("DS_HIGH_PORT"))
		setenv("TCP_HIGH_PORT", getenv("DS_HIGH_PORT"), 0);

	q->manager_link = link_serve(port);
	if(!q->manager_link) {
		debug(D_NOTICE, "Could not create work_queue on port %i.", port);
		free(q);
		return 0;
	} else {
		char address[LINK_ADDRESS_MAX];
		link_address_local(q->manager_link, address, &q->port);
	}

	q->ssl_key = key ? strdup(key) : 0;
	q->ssl_cert = cert ? strdup(cert) : 0;

	if(q->ssl_key || q->ssl_cert) q->ssl_enabled=1;

	getcwd(q->workingdir,PATH_MAX);

	q->next_taskid = 1;

	q->ready_list = list_create();

	q->tasks          = itable_create(0);

	q->worker_table = hash_table_create(0, 0);
	q->worker_blocklist = hash_table_create(0, 0);

	q->factory_table = hash_table_create(0, 0);
	q->fetch_factory = 0;

	q->measured_local_resources = rmsummary_create(-1);
	q->current_max_worker       = rmsummary_create(-1);
	q->max_task_resources_requested = rmsummary_create(-1);

	q->stats                      = calloc(1, sizeof(struct ds_stats));
	q->stats_disconnected_workers = calloc(1, sizeof(struct ds_stats));
	q->stats_measure              = calloc(1, sizeof(struct ds_stats));

	q->workers_with_available_results = hash_table_create(0, 0);

	// The poll table is initially null, and will be created
	// (and resized) as needed by build_poll_table.
	q->poll_table_size = 8;

	q->worker_selection_algorithm = ds_option_scheduler;
	q->process_pending_check = 0;

	q->short_timeout = 5;
	q->long_timeout = 3600;

	q->stats->time_when_started = timestamp_get();
	q->time_last_large_tasks_check = timestamp_get();
	q->task_info_list = list_create();

	q->time_last_wait = 0;
	q->time_last_log_stats = 0;

	q->catalog_hosts = 0;

	q->keepalive_interval = DS_DEFAULT_KEEPALIVE_INTERVAL;
	q->keepalive_timeout = DS_DEFAULT_KEEPALIVE_TIMEOUT;

	q->monitor_mode = DS_MON_DISABLED;

	q->hungry_minimum = 10;

	q->wait_for_workers = 0;

	q->allocation_default_mode = DS_ALLOCATION_MODE_FIXED;
	q->categories = hash_table_create(0, 0);

	// The value -1 indicates that fast abort is inactive by default
	// fast abort depends on categories, thus set after them.
	ds_activate_fast_abort(q, -1);

	q->password = 0;

	q->resource_submit_multiplier = 1.0;

	q->minimum_transfer_timeout = 60;
	q->transfer_outlier_factor = 10;
	q->default_transfer_rate = 1*MEGABYTE;
	q->disk_avail_threshold = 100;

	q->manager_preferred_connection = xxstrdup("by_ip");

	if( (envstring  = getenv("DS_BANDWIDTH")) ) {
		q->bandwidth_limit = string_metric_parse(envstring);
		if(q->bandwidth_limit < 0) {
			q->bandwidth_limit = 0;
		}
	}

	ds_perf_log_write_update(q, 1);

	q->time_last_wait = timestamp_get();

	char hostname[DOMAIN_NAME_MAX];
	if(domain_name_cache_guess(hostname)) {
		debug(D_DS, "Manager advertising as %s:%d", hostname, q->port);
	}
	else {
		debug(D_DS, "Manager is listening on port %d.", q->port);
	}
	return q;
}

int ds_enable_monitoring(struct ds_manager *q, char *monitor_output_directory, int watchdog)
{
	if(!q)
		return 0;

	q->monitor_mode = DS_MON_DISABLED;
	q->monitor_exe = resource_monitor_locate(NULL);

	if(q->monitor_output_directory) {
		free(q->monitor_output_directory);
		q->monitor_output_directory = NULL;
	}

	if(!q->monitor_exe)
	{
		warn(D_WQ, "Could not find the resource monitor executable. Disabling monitoring.\n");
		return 0;
	}

	if(monitor_output_directory) {
		q->monitor_output_directory = xxstrdup(monitor_output_directory);

		if(!create_dir(q->monitor_output_directory, 0777)) {
			fatal("Could not create monitor output directory - %s (%s)", q->monitor_output_directory, strerror(errno));
		}

		q->monitor_summary_filename = string_format("%s/ds-%d.summaries", q->monitor_output_directory, getpid());
		q->monitor_file             = fopen(q->monitor_summary_filename, "a");

		if(!q->monitor_file)
		{
			fatal("Could not open monitor log file for writing: '%s'\n", q->monitor_summary_filename);
		}

	}

	if(q->measured_local_resources)
		rmsummary_delete(q->measured_local_resources);

	q->measured_local_resources = rmonitor_measure_process(getpid());
	q->monitor_mode = DS_MON_SUMMARY;

	if(watchdog) {
		q->monitor_mode |= DS_MON_WATCHDOG;
	}

	return 1;
}

int ds_enable_monitoring_full(struct ds_manager *q, char *monitor_output_directory, int watchdog) {
	int status = ds_enable_monitoring(q, monitor_output_directory, 1);

	if(status) {
		q->monitor_mode = DS_MON_FULL;

		if(watchdog) {
			q->monitor_mode |= DS_MON_WATCHDOG;
		}
	}

	return status;
}

int ds_activate_fast_abort_category(struct ds_manager *q, const char *category, double multiplier)
{
	struct category *c = ds_category_lookup_or_create(q, category);

	if(multiplier >= 1) {
		debug(D_DS, "Enabling fast abort multiplier for '%s': %3.3lf\n", category, multiplier);
		c->fast_abort = multiplier;
		return 0;
	} else if(multiplier == 0) {
		debug(D_DS, "Disabling fast abort multiplier for '%s'.\n", category);
		c->fast_abort = 0;
		return 1;
	} else {
		debug(D_DS, "Using default fast abort multiplier for '%s'.\n", category);
		c->fast_abort = -1;
		return 0;
	}
}

int ds_activate_fast_abort(struct ds_manager *q, double multiplier)
{
	return ds_activate_fast_abort_category(q, "default", multiplier);
}

int ds_port(struct ds_manager *q)
{
	char addr[LINK_ADDRESS_MAX];
	int port;

	if(!q) return 0;

	if(link_address_local(q->manager_link, addr, &port)) {
		return port;
	} else {
		return 0;
	}
}

void ds_specify_algorithm(struct ds_manager *q, ds_schedule_t algorithm)
{
	q->worker_selection_algorithm = algorithm;
}

void ds_specify_name(struct ds_manager *q, const char *name)
{
	if(q->name) free(q->name);
	if(name) {
		q->name = xxstrdup(name);
		setenv("DS_NAME", q->name, 1);
	} else {
		q->name = 0;
	}
}

const char *ds_name(struct ds_manager *q)
{
	return q->name;
}

void ds_specify_priority(struct ds_manager *q, int priority)
{
	q->priority = priority;
}

void ds_specify_num_tasks_left(struct ds_manager *q, int ntasks)
{
	if(ntasks < 1) {
		q->num_tasks_left = 0;
	}
	else {
		q->num_tasks_left = ntasks;
	}
}

void ds_specify_catalog_server(struct ds_manager *q, const char *hostname, int port)
{
	char hostport[DOMAIN_NAME_MAX + 8];
	if(hostname && (port > 0)) {
		sprintf(hostport, "%s:%d", hostname, port);
		ds_specify_catalog_servers(q, hostport);
	} else if(hostname) {
		ds_specify_catalog_servers(q, hostname);
	} else if (port > 0) {
		sprintf(hostport, "%d", port);
		setenv("CATALOG_PORT", hostport, 1);
	}
}

void ds_specify_catalog_servers(struct ds_manager *q, const char *hosts)
{
	if(hosts) {
		if(q->catalog_hosts) free(q->catalog_hosts);
		q->catalog_hosts = strdup(hosts);
		setenv("CATALOG_HOST", hosts, 1);
	}
}

void ds_specify_password( struct ds_manager *q, const char *password )
{
	q->password = xxstrdup(password);
}

int ds_specify_password_file( struct ds_manager *q, const char *file )
{
	return copy_file_to_buffer(file,&q->password,NULL)>0;
}

void ds_delete(struct ds_manager *q)
{
	if(!q) return;

	release_all_workers(q);

	ds_perf_log_write_update(q, 1);

	if(q->name) update_catalog(q,1);

	/* we call this function here before any of the structures are freed. */
	ds_disable_monitoring(q);

	if(q->catalog_hosts) free(q->catalog_hosts);

	/* XXX this may be a leak, should workers be deleted as well? */
	hash_table_delete(q->worker_table);

	hash_table_clear(q->factory_table,(void*)ds_factory_info_delete);
	hash_table_delete(q->factory_table);

	hash_table_clear(q->worker_blocklist,(void*)ds_blocklist_info_delete);
	hash_table_delete(q->worker_blocklist);

	char *key;
	struct category *c;
	hash_table_firstkey(q->categories);
	while(hash_table_nextkey(q->categories, &key, (void **) &c)) {
		category_delete(q->categories, key);
	}
	hash_table_delete(q->categories);

	list_delete(q->ready_list);
	itable_delete(q->tasks);

	hash_table_delete(q->workers_with_available_results);

	list_clear(q->task_info_list,(void*)ds_task_info_delete);
	list_delete(q->task_info_list);

	free(q->stats);
	free(q->stats_disconnected_workers);
	free(q->stats_measure);

	if(q->name)
		free(q->name);

	if(q->manager_preferred_connection)
		free(q->manager_preferred_connection);

	free(q->poll_table);
	free(q->ssl_cert);
	free(q->ssl_key);

	link_close(q->manager_link);
	if(q->perf_logfile) {
		fclose(q->perf_logfile);
	}

	if(q->txn_logfile) {
		ds_txn_log_write(q, "MANAGER END");

		if(fclose(q->txn_logfile) != 0) {
			debug(D_DS, "unable to write transactions log: %s\n", strerror(errno));
		}
	}

	rmsummary_delete(q->measured_local_resources);
	rmsummary_delete(q->current_max_worker);
	rmsummary_delete(q->max_task_resources_requested);

	free(q);
}

static void update_resource_report(struct ds_manager *q) {
	// Only measure every few seconds.
	if((time(0) - q->resources_last_update_time) < DS_RESOURCE_MEASUREMENT_INTERVAL)
		return;

	rmonitor_measure_process_update_to_peak(q->measured_local_resources, getpid());

	q->resources_last_update_time = time(0);
}

void ds_disable_monitoring(struct ds_manager *q) {
	if(q->monitor_mode == DS_MON_DISABLED)
		return;

	rmonitor_measure_process_update_to_peak(q->measured_local_resources, getpid());
	if(!q->measured_local_resources->exit_type)
		q->measured_local_resources->exit_type = xxstrdup("normal");

	if(q->monitor_mode && q->monitor_summary_filename) {
		fclose(q->monitor_file);

		char template[] = "rmonitor-summaries-XXXXXX";
		int final_fd = mkstemp(template);
		int summs_fd = open(q->monitor_summary_filename, O_RDONLY);

		if( final_fd < 0 || summs_fd < 0 ) {
			warn(D_DEBUG, "Could not consolidate resource summaries.");
			return;
		}

		/* set permissions according to user's mask. getumask is not available yet,
		   and the only way to get the value of the current mask is to change
		   it... */
		mode_t old_mask = umask(0);
		umask(old_mask);
		fchmod(final_fd, 0777 & ~old_mask  );

		FILE *final = fdopen(final_fd, "w");

		const char *user_name = getlogin();
		if(!user_name) {
			user_name = "unknown";
		}

		struct jx *extra = jx_object(
				jx_pair(jx_string("type"), jx_string("ds_manager"),
					jx_pair(jx_string("user"), jx_string(user_name),
						NULL)));

		if(q->name) {
			jx_insert_string(extra, "manager_name", q->name);
		}

		rmsummary_print(final, q->measured_local_resources, /* pprint */ 0, extra);

		copy_fd_to_stream(summs_fd, final);

		jx_delete(extra);
		close(summs_fd);

		if(fclose(final) != 0) {
			debug(D_DS, "unable to update monitor report to final destination file: %s\n", strerror(errno));
		}

		if(rename(template, q->monitor_summary_filename) < 0) {
			warn(D_DEBUG, "Could not move monitor report to final destination file.");
		}
	}

	if(q->monitor_exe)
		free(q->monitor_exe);
	if(q->monitor_output_directory)
		free(q->monitor_output_directory);
	if(q->monitor_summary_filename)
		free(q->monitor_summary_filename);
}

void ds_monitor_add_files(struct ds_manager *q, struct ds_task *t) {
	ds_task_specify_file(t, q->monitor_exe, RESOURCE_MONITOR_REMOTE_NAME, DS_INPUT, DS_CACHE);

	char *summary  = monitor_file_name(q, t, ".summary");
	ds_task_specify_file(t, summary, RESOURCE_MONITOR_REMOTE_NAME ".summary", DS_OUTPUT, DS_NOCACHE);
	free(summary);

	if(q->monitor_mode & DS_MON_FULL && (q->monitor_output_directory || t->monitor_output_directory)) {
		char *debug  = monitor_file_name(q, t, ".debug");
		char *series = monitor_file_name(q, t, ".series");

		ds_task_specify_file(t, debug, RESOURCE_MONITOR_REMOTE_NAME ".debug",   DS_OUTPUT, DS_NOCACHE);
		ds_task_specify_file(t, series, RESOURCE_MONITOR_REMOTE_NAME ".series", DS_OUTPUT, DS_NOCACHE);

		free(debug);
		free(series);
	}
}

char *ds_monitor_wrap(struct ds_manager *q, struct ds_worker_info *w, struct ds_task *t, struct rmsummary *limits)
{
	buffer_t b;
	buffer_init(&b);

	buffer_printf(&b, "-V 'task_id: %d'", t->taskid);

	if(t->category) {
		buffer_printf(&b, " -V 'category: %s'", t->category);
	}

	if(t->monitor_snapshot_file) {
		buffer_printf(&b, " --snapshot-events %s", RESOURCE_MONITOR_REMOTE_NAME_EVENTS);
	}

	if(!(q->monitor_mode & DS_MON_WATCHDOG)) {
		buffer_printf(&b, " --measure-only");
	}

	int extra_files = (q->monitor_mode & DS_MON_FULL);

	char *monitor_cmd = resource_monitor_write_command("./" RESOURCE_MONITOR_REMOTE_NAME, RESOURCE_MONITOR_REMOTE_NAME, limits, /* extra options */ buffer_tostring(&b), /* debug */ extra_files, /* series */ extra_files, /* inotify */ 0, /* measure_dir */ NULL);
	char *wrap_cmd  = string_wrap_command(t->command_line, monitor_cmd);

	buffer_free(&b);
	free(monitor_cmd);

	return wrap_cmd;
}

static double ds_task_priority(void *item) {
	assert(item);
	struct ds_task *t = item;
	return t->priority;
}

/* Put a given task on the ready list, taking into account the task priority and the queue schedule. */

static void push_task_to_ready_list( struct ds_manager *q, struct ds_task *t )
{
	int by_priority = 1;

	if(t->result == DS_RESULT_RESOURCE_EXHAUSTION) {
		/* when a task is resubmitted given resource exhaustion, we
		 * push it at the head of the list, so it gets to run as soon
		 * as possible. This avoids the issue in which all 'big' tasks
		 * fail because the first allocation is too small. */
		by_priority = 0;
	}

	if(by_priority) {
		list_push_priority(q->ready_list, ds_task_priority, t);
	} else {
		list_push_head(q->ready_list,t);
	}

	/* If the task has been used before, clear out accumulated state. */
	ds_task_clean(t,0);
}

ds_task_state_t ds_task_state( struct ds_manager *q, int taskid )
{
	struct ds_task *t = itable_lookup(q->tasks,taskid);
	if(t) {
		return t->state;
	} else {
		return DS_TASK_UNKNOWN;
	}
}

/* Changes task state. Returns old state */
/* State of the task. One of DS_TASK(UNKNOWN|READY|RUNNING|WAITING_RETRIEVAL|RETRIEVED|DONE) */
static ds_task_state_t change_task_state( struct ds_manager *q, struct ds_task *t, ds_task_state_t new_state ) {

	ds_task_state_t old_state = t->state;

	t->state = new_state;

	if( old_state == DS_TASK_READY ) {
		// Treat DS_TASK_READY specially, as it has the order of the tasks
		list_remove(q->ready_list, t);
	}

	// insert to corresponding table
	debug(D_DS, "Task %d state change: %s (%d) to %s (%d)\n", t->taskid, ds_task_state_string(old_state), old_state, ds_task_state_string(new_state), new_state);

	switch(new_state) {
		case DS_TASK_READY:
			ds_task_update_result(t, DS_RESULT_UNKNOWN);
			push_task_to_ready_list(q, t);
			break;
		case DS_TASK_DONE:
		case DS_TASK_CANCELED:
			/* tasks are freed when returned to user, thus we remove them from our local record */
			itable_remove(q->tasks, t->taskid);
			break;
		default:
			/* do nothing */
			break;
	}

	ds_perf_log_write_update(q, 0);
	ds_txn_log_write_task(q, t);

	return old_state;
}

static int task_in_terminal_state(struct ds_manager *q, struct ds_task *t)
{
	switch(t->state) {
		case DS_TASK_READY:
		case DS_TASK_RUNNING:
		case DS_TASK_WAITING_RETRIEVAL:
		case DS_TASK_RETRIEVED:
			return 0;
			break;
		case DS_TASK_DONE:
		case DS_TASK_CANCELED:
		case DS_TASK_UNKNOWN:
			return 1;
			break;
	}

	return 0;
}

const char *ds_result_str(ds_result_t result) {
	const char *str = NULL;

	switch(result) {
		case DS_RESULT_SUCCESS:
			str = "SUCCESS";
			break;
		case DS_RESULT_INPUT_MISSING:
			str = "INPUT_MISS";
			break;
		case DS_RESULT_OUTPUT_MISSING:
			str = "OUTPUT_MISS";
			break;
		case DS_RESULT_STDOUT_MISSING:
			str = "STDOUT_MISS";
			break;
		case DS_RESULT_SIGNAL:
			str = "SIGNAL";
			break;
		case DS_RESULT_RESOURCE_EXHAUSTION:
			str = "RESOURCE_EXHAUSTION";
			break;
		case DS_RESULT_TASK_TIMEOUT:
			str = "END_TIME";
			break;
		case DS_RESULT_UNKNOWN:
			str = "UNKNOWN";
			break;
		case DS_RESULT_FORSAKEN:
			str = "FORSAKEN";
			break;
		case DS_RESULT_MAX_RETRIES:
			str = "MAX_RETRIES";
			break;
		case DS_RESULT_TASK_MAX_RUN_TIME:
			str = "MAX_WALL_TIME";
			break;
		case DS_RESULT_DISK_ALLOC_FULL:
			str = "DISK_FULL";
			break;
		case DS_RESULT_RMONITOR_ERROR:
			str = "MONITOR_ERROR";
			break;
		case DS_RESULT_OUTPUT_TRANSFER_ERROR:
			str = "OUTPUT_TRANSFER_ERROR";
			break;
	}

	return str;
}

static struct ds_task *task_state_any(struct ds_manager *q, ds_task_state_t state) {
	struct ds_task *t;
	uint64_t taskid;

	itable_firstkey(q->tasks);
	while( itable_nextkey(q->tasks, &taskid, (void **) &t) ) {
		if( t->state==state ) return t;
	}

	return NULL;
}

static struct ds_task *task_state_any_with_tag(struct ds_manager *q, ds_task_state_t state, const char *tag) {
	struct ds_task *t;
	uint64_t taskid;

	itable_firstkey(q->tasks);
	while( itable_nextkey(q->tasks, &taskid, (void **) &t) ) {
		if( t->state==state && tasktag_comparator((void *) t, (void *) tag)) {
			return t;
		}
	}

	return NULL;
}

static int task_state_count(struct ds_manager *q, const char *category, ds_task_state_t state) {
	struct ds_task *t;
	uint64_t taskid;

	int count = 0;

	itable_firstkey(q->tasks);
	while( itable_nextkey(q->tasks, &taskid, (void **) &t) ) {
		if( t->state==state ) {
			if(!category || strcmp(category, t->category) == 0) {
				count++;
			}
		}
	}

	return count;
}

static int task_request_count( struct ds_manager *q, const char *category, category_allocation_t request) {
	struct ds_task *t;
	uint64_t taskid;

	int count = 0;

	itable_firstkey(q->tasks);
	while( itable_nextkey(q->tasks, &taskid, (void **) &t) ) {
		if(t->resource_request == request) {
			if(!category || strcmp(category, t->category) == 0) {
				count++;
			}
		}
	}

	return count;
}

static int ds_submit_internal(struct ds_manager *q, struct ds_task *t)
{
	itable_insert(q->tasks, t->taskid, t);

	/* Ensure category structure is created. */
	ds_category_lookup_or_create(q, t->category);

	change_task_state(q, t, DS_TASK_READY);

	t->time_when_submitted = timestamp_get();
	q->stats->tasks_submitted++;

	if(q->monitor_mode != DS_MON_DISABLED)
		ds_monitor_add_files(q, t);

	rmsummary_merge_max(q->max_task_resources_requested, t->resources_requested);

	return (t->taskid);
}

int ds_submit(struct ds_manager *q, struct ds_task *t)
{
	if(t->taskid > 0) {
		if(task_in_terminal_state(q, t)) {
			/* this task struct has been submitted before. We keep all the
			 * definitions, but reset all of the stats. */
			ds_task_clean(t, /* full clean */ 1);
		} else {
			fatal("Task %d has been already submitted and is not in any final state.", t->taskid);
		}
	}

	t->taskid = q->next_taskid;

	//Increment taskid. So we get a unique taskid for every submit.
	q->next_taskid++;

	return ds_submit_internal(q, t);
}

void ds_block_host_with_timeout(struct ds_manager *q, const char *hostname, time_t timeout)
{
	return ds_blocklist_block(q,hostname,timeout);
}

void ds_block_host(struct ds_manager *q, const char *hostname)
{
	ds_blocklist_block(q, hostname, -1);
}

void ds_unblock_host(struct ds_manager *q, const char *hostname)
{
	ds_blocklist_unblock(q,hostname);
}

void ds_unblock_all(struct ds_manager *q)
{
	ds_blocklist_unblock_all_by_time(q, -1);
}

static void print_password_warning( struct ds_manager *q )
{
	static int did_password_warning = 0;

	if(did_password_warning) {
		return;
	}

	if(!q->password && q->name) {
		fprintf(stderr,"warning: this dataswarm manager is visible to the public.\n");
		fprintf(stderr,"warning: you should set a password with the --password option.\n");
	}

	if(!q->ssl_enabled) {
		fprintf(stderr,"warning: using plain-text when communicating with workers.\n");
		fprintf(stderr,"warning: use encryption with a key and cert when creating the manager.\n");
	}

	did_password_warning = 1;
}

#define BEGIN_ACCUM_TIME(q, stat) {\
	if(q->stats_measure->stat != 0) {\
		fatal("Double-counting stat %s. This should not happen, and it is a dataswarm bug.");\
	} else {\
		q->stats_measure->stat = timestamp_get();\
	}\
}

#define END_ACCUM_TIME(q, stat) {\
	q->stats->stat += timestamp_get() - q->stats_measure->stat;\
	q->stats_measure->stat = 0;\
}

struct ds_task *ds_wait(struct ds_manager *q, int timeout)
{
	return ds_wait_for_tag(q, NULL, timeout);
}

struct ds_task *ds_wait_for_tag(struct ds_manager *q, const char *tag, int timeout)
{
	if(timeout == 0) {
		// re-establish old, if unintended behavior, where 0 would wait at
		// least a second. With 0, we would like the loop to be executed at
		// least once, but right now we cannot enforce that. Making it 1, we
		// guarantee that the wait loop is executed once.
		timeout = 1;
	}

	if(timeout != DS_WAITFORTASK && timeout < 0) {
		debug(D_NOTICE|D_DS, "Invalid wait timeout value '%d'. Waiting for 5 seconds.", timeout);
		timeout = 5;
	}

	return ds_wait_internal(q, timeout, tag);
}

/* return number of workers that failed */
static int poll_active_workers(struct ds_manager *q, int stoptime )
{
	BEGIN_ACCUM_TIME(q, time_polling);

	int n = build_poll_table(q);

	// We poll in at most small time segments (of a second). This lets
	// promptly dispatch tasks, while avoiding busy waiting.
	int msec = q->busy_waiting_flag ? 1000 : 0;
	if(stoptime) {
		msec = MIN(msec, (stoptime - time(0)) * 1000);
	}

	END_ACCUM_TIME(q, time_polling);

	if(msec < 0) {
		return 0;
	}

	BEGIN_ACCUM_TIME(q, time_polling);

	// Poll all links for activity.
	link_poll(q->poll_table, n, msec);
	q->link_poll_end = timestamp_get();

	END_ACCUM_TIME(q, time_polling);

	BEGIN_ACCUM_TIME(q, time_status_msgs);

	int i, j = 1;
	int workers_failed = 0;
	// Then consider all existing active workers
	for(i = j; i < n; i++) {
		if(q->poll_table[i].revents) {
			if(handle_worker(q, q->poll_table[i].link) == DS_WORKER_FAILURE) {
				workers_failed++;
			}
		}
	}

	if(hash_table_size(q->workers_with_available_results) > 0) {
		char *key;
		struct ds_worker_info *w;
		hash_table_firstkey(q->workers_with_available_results);
		while(hash_table_nextkey(q->workers_with_available_results,&key,(void**)&w)) {
			get_available_results(q, w);
			hash_table_remove(q->workers_with_available_results, key);
			hash_table_firstkey(q->workers_with_available_results);
		}
	}

	END_ACCUM_TIME(q, time_status_msgs);

	return workers_failed;
}


static int connect_new_workers(struct ds_manager *q, int stoptime, int max_new_workers)
{
	int new_workers = 0;

	// If the manager link was awake, then accept at most max_new_workers.
	// Note we are using the information gathered in poll_active_workers, which
	// is a little ugly.
	if(q->poll_table[0].revents) {
		do {
			add_worker(q);
			new_workers++;
		} while(link_usleep(q->manager_link, 0, 1, 0) && (stoptime >= time(0) && (max_new_workers > new_workers)));
	}

	return new_workers;
}


static struct ds_task *ds_wait_internal(struct ds_manager *q, int timeout, const char *tag)
{
/*
   - compute stoptime
   S time left?                              No:  return null
   - task completed?                         Yes: return completed task to user
   - update catalog if appropriate
   - retrieve workers status messages
   - tasks waiting to be retrieved?          Yes: retrieve one task and go to S.
   - tasks waiting to be dispatched?         Yes: dispatch one task and go to S.
   - send keepalives to appropriate workers
   - fast-abort workers
   - if new workers, connect n of them
   - expired tasks?                          Yes: mark expired tasks as retrieved and go to S.
   - queue empty?                            Yes: return null
   - go to S
*/
	int events = 0;
	// account for time we spend outside ds_wait
	if(q->time_last_wait > 0) {
		q->stats->time_application += timestamp_get() - q->time_last_wait;
	} else {
		q->stats->time_application += timestamp_get() - q->stats->time_when_started;
	}

	print_password_warning(q);

	// compute stoptime
	time_t stoptime = (timeout == DS_WAITFORTASK) ? 0 : time(0) + timeout;

	int result;
	struct ds_task *t = NULL;
	// time left?

	while( (stoptime == 0) || (time(0) < stoptime) ) {

		BEGIN_ACCUM_TIME(q, time_internal);
		// task completed?
		if (t == NULL)
		{
			if(tag) {
				t = task_state_any_with_tag(q, DS_TASK_RETRIEVED, tag);
			} else {
				t = task_state_any(q, DS_TASK_RETRIEVED);
			}
			if(t) {
				change_task_state(q, t, DS_TASK_DONE);

				if( t->result != DS_RESULT_SUCCESS )
				{
					q->stats->tasks_failed++;
				}

				// return completed task (t) to the user. We do not return right
				// away, and instead break out of the loop to correctly update the
				// queue time statistics.
				events++;
				END_ACCUM_TIME(q, time_internal);

				if(!q->wait_retrieve_many) {
					break;
				}
			}
		}

		// update catalog if appropriate
		if(q->name) {
			update_catalog(q,0);
		}

		if(q->monitor_mode)
			update_resource_report(q);

		END_ACCUM_TIME(q, time_internal);

		// retrieve worker status messages
		if(poll_active_workers(q, stoptime) > 0) {
			//at least one worker was removed.
			events++;
			// note we keep going, and we do not restart the loop as we do in
			// further events. This is because we give top priority to
			// returning and retrieving tasks.
		}


		q->busy_waiting_flag = 0;

		// tasks waiting to be retrieved?
		BEGIN_ACCUM_TIME(q, time_receive);
		result = receive_one_task(q);
		END_ACCUM_TIME(q, time_receive);
		if(result) {
			// retrieved at least one task
			events++;
			compute_manager_load(q, 1);
			continue;
		}

		// expired tasks
		BEGIN_ACCUM_TIME(q, time_internal);
		result = expire_waiting_tasks(q);
		END_ACCUM_TIME(q, time_internal);
		if(result) {
			// expired at least one task
			events++;
			compute_manager_load(q, 1);
			continue;
		}

		// record that there was not task activity for this iteration
		compute_manager_load(q, 0);

		if(q->wait_for_workers <= hash_table_size(q->worker_table)) {
			if(q->wait_for_workers > 0) {
				debug(D_DS, "Target number of workers reached (%d).", q->wait_for_workers);
				q->wait_for_workers = 0;
			}
			// tasks waiting to be dispatched?
			BEGIN_ACCUM_TIME(q, time_send);
			result = send_one_task(q);
			END_ACCUM_TIME(q, time_send);
			if(result) {
				// sent at least one task
				events++;
				continue;
			}
		}
		//we reach here only if no task was neither sent nor received.
		compute_manager_load(q, 1);

		// send keepalives to appropriate workers
		BEGIN_ACCUM_TIME(q, time_status_msgs);
		ask_for_workers_updates(q);
		END_ACCUM_TIME(q, time_status_msgs);

		// Kill off slow/drained workers.
		BEGIN_ACCUM_TIME(q, time_internal);
		result  = abort_slow_workers(q);
		result += abort_drained_workers(q);
		ds_blocklist_unblock_all_by_time(q, time(0));
		END_ACCUM_TIME(q, time_internal);
		if(result) {
			// removed at least one worker
			events++;
			continue;
		}

		// if new workers, connect n of them
		BEGIN_ACCUM_TIME(q, time_status_msgs);
		result = connect_new_workers(q, stoptime, MAX(q->wait_for_workers, MAX_NEW_WORKERS));
		END_ACCUM_TIME(q, time_status_msgs);
		if(result) {
			// accepted at least one worker
			events++;
			continue;
		}

		if(q->process_pending_check) {

			BEGIN_ACCUM_TIME(q, time_internal);
			int pending = process_pending();
			END_ACCUM_TIME(q, time_internal);

			if(pending) {
				events++;
				break;
			}
		}

		// return if queue is empty and something interesting already happened
		// in this wait.
		if(events > 0) {
			BEGIN_ACCUM_TIME(q, time_internal);
			int done = !task_state_any(q, DS_TASK_RUNNING) && !task_state_any(q, DS_TASK_READY) && !task_state_any(q, DS_TASK_WAITING_RETRIEVAL);
			END_ACCUM_TIME(q, time_internal);

			if(done) {
				break;
			}
		}

		timestamp_t current_time = timestamp_get();
		if(current_time - q->time_last_large_tasks_check >= DS_LARGE_TASK_CHECK_INTERVAL) {
			q->time_last_large_tasks_check = current_time;
			ds_schedule_check_for_large_tasks(q);
		}

		// if we got here, no events were triggered.
		// we set the busy_waiting flag so that link_poll waits for some time
		// the next time around.
		q->busy_waiting_flag = 1;
	}

	if(events > 0) {
		ds_perf_log_write_update(q, 1);
	}

	q->time_last_wait = timestamp_get();

	return t;
}

//check if workers' resources are available to execute more tasks
//queue should have at least q->hungry_minimum ready tasks
//@param: 	struct ds_manager* - pointer to queue
//@return: 	1 if hungry, 0 otherwise
int ds_hungry(struct ds_manager *q)
{
	//check if queue is initialized
	//return false if not
	if (q == NULL){
		return 0;
	}

	struct ds_stats qstats;
	ds_get_stats(q, &qstats);

	//if number of ready tasks is less than q->hungry_minimum, then queue is hungry
	if (qstats.tasks_waiting < q->hungry_minimum){
		return 1;
	}

	//get total available resources consumption (cores, memory, disk, gpus) of all workers of this manager
	//available = total (all) - committed (actual in use)
	int64_t workers_total_avail_cores 	= 0;
	int64_t workers_total_avail_memory 	= 0;
	int64_t workers_total_avail_disk 	= 0;
	int64_t workers_total_avail_gpus 	= 0;

	workers_total_avail_cores 	= overcommitted_resource_total(q, q->stats->total_cores) - q->stats->committed_cores;
	workers_total_avail_memory 	= overcommitted_resource_total(q, q->stats->total_memory) - q->stats->committed_memory;
	workers_total_avail_gpus	= overcommitted_resource_total(q, q->stats->total_gpus) - q->stats->committed_gpus;
	workers_total_avail_disk 	= q->stats->total_disk - q->stats->committed_disk; //never overcommit disk

	//get required resources (cores, memory, disk, gpus) of one waiting task
	int64_t ready_task_cores 	= 0;
	int64_t ready_task_memory 	= 0;
	int64_t ready_task_disk 	= 0;
	int64_t ready_task_gpus		= 0;

	struct ds_task *t;

	int count = task_state_count(q, NULL, DS_TASK_READY);

	while(count > 0)
	{
		count--;
		t = list_pop_head(q->ready_list);

		ready_task_cores  += MAX(1,t->resources_requested->cores);
		ready_task_memory += t->resources_requested->memory;
		ready_task_disk   += t->resources_requested->disk;
		ready_task_gpus   += t->resources_requested->gpus;

		list_push_tail(q->ready_list, t);
	}

	//check possible limiting factors
	//return false if required resources exceed available resources
	if (ready_task_cores > workers_total_avail_cores){
		return 0;
	}
	if (ready_task_memory > workers_total_avail_memory){
		return 0;
	}
	if (ready_task_disk > workers_total_avail_disk){
		return 0;
	}
	if (ready_task_gpus > workers_total_avail_gpus){
		return 0;
	}

	return 1;	//all good
}

int ds_shut_down_workers(struct ds_manager *q, int n)
{
	struct ds_worker_info *w;
	char *key;
	int i = 0;

	/* by default, remove all workers. */
	if(n < 1)
		n = hash_table_size(q->worker_table);

	if(!q)
		return -1;

	// send worker the "exit" msg
	hash_table_firstkey(q->worker_table);
	while(i < n && hash_table_nextkey(q->worker_table, &key, (void **) &w)) {
		if(itable_size(w->current_tasks) == 0) {
			shut_down_worker(q, w);

			/* shut_down_worker alters the table, so we reset it here. */
			hash_table_firstkey(q->worker_table);
			i++;
		}
	}

	return i;
}

int ds_specify_draining_by_hostname(struct ds_manager *q, const char *hostname, int drain_flag)
{
	char *worker_hashkey = NULL;
	struct ds_worker_info *w = NULL;

	drain_flag = !!(drain_flag);

	int workers_updated = 0;

	hash_table_firstkey(q->worker_table);
	while(hash_table_nextkey(q->worker_table, &worker_hashkey, (void *) w)) {
		if (!strcmp(w->hostname, hostname)) {
			w->draining = drain_flag;
			workers_updated++;
		}
	}

	return workers_updated;
}

/**
 * Cancel submitted task as long as it has not been retrieved through wait().
 * This returns the ds_task struct corresponding to specified task and
 * null if the task is not found.
 */
struct ds_task *ds_cancel_by_taskid(struct ds_manager *q, int taskid) {

	struct ds_task *matched_task = NULL;

	matched_task = itable_lookup(q->tasks, taskid);

	if(!matched_task) {
		debug(D_DS, "Task with id %d is not found in queue.", taskid);
		return NULL;
	}

	cancel_task_on_worker(q, matched_task, DS_TASK_CANCELED);

	/* change state even if task is not running on a worker. */
	change_task_state(q, matched_task, DS_TASK_CANCELED);

	q->stats->tasks_cancelled++;

	return matched_task;
}

struct ds_task *ds_cancel_by_tasktag(struct ds_manager *q, const char* tasktag) {

	struct ds_task *matched_task = NULL;

	if (tasktag){
		matched_task = find_task_by_tag(q, tasktag);

		if(matched_task) {
			return ds_cancel_by_taskid(q, matched_task->taskid);
		}

	}

	debug(D_DS, "Task with tag %s is not found in queue.", tasktag);
	return NULL;
}

struct list * ds_cancel_all_tasks(struct ds_manager *q) {
	struct list *l = list_create();
	struct ds_task *t;
	struct ds_worker_info *w;
	uint64_t taskid;
	char *key;

	itable_firstkey(q->tasks);
	while(itable_nextkey(q->tasks, &taskid, (void**)&t)) {
		list_push_tail(l, t);
		ds_cancel_by_taskid(q, taskid);
	}

	hash_table_firstkey(q->workers_with_available_results);
	while(hash_table_nextkey(q->workers_with_available_results, &key, (void **) &w)) {
		hash_table_remove(q->workers_with_available_results, key);
		hash_table_firstkey(q->workers_with_available_results);
	}

	hash_table_firstkey(q->worker_table);
	while(hash_table_nextkey(q->worker_table, &key, (void**)&w)) {

		ds_manager_send(q,w,"kill -1\n");

		itable_firstkey(w->current_tasks);
		while(itable_nextkey(w->current_tasks, &taskid, (void**)&t)) {
			//Delete any input files that are not to be cached.
			delete_worker_files(q, w, t->input_files, DS_CACHE );

			//Delete all output files since they are not needed as the task was aborted.
			delete_worker_files(q, w, t->output_files, 0);
			reap_task_from_worker(q, w, t, DS_TASK_CANCELED);

			list_push_tail(l, t);
			q->stats->tasks_cancelled++;
			itable_firstkey(w->current_tasks);
		}
	}
	return l;
}

static void release_all_workers(struct ds_manager *q) {
	struct ds_worker_info *w;
	char *key;

	if(!q) return;

	hash_table_firstkey(q->worker_table);
	while(hash_table_nextkey(q->worker_table,&key,(void**)&w)) {
		release_worker(q, w);
		hash_table_firstkey(q->worker_table);
	}
}

int ds_empty(struct ds_manager *q)
{
	struct ds_task *t;
	uint64_t taskid;

	itable_firstkey(q->tasks);
	while( itable_nextkey(q->tasks, &taskid, (void **) &t) ) {
		int state = ds_task_state(q, taskid);

		if( state == DS_TASK_READY   )           return 0;
		if( state == DS_TASK_RUNNING )           return 0;
		if( state == DS_TASK_WAITING_RETRIEVAL ) return 0;
		if( state == DS_TASK_RETRIEVED )         return 0;
	}

	return 1;
}

void ds_specify_keepalive_interval(struct ds_manager *q, int interval)
{
	q->keepalive_interval = interval;
}

void ds_specify_keepalive_timeout(struct ds_manager *q, int timeout)
{
	q->keepalive_timeout = timeout;
}

void ds_manager_preferred_connection(struct ds_manager *q, const char *preferred_connection)
{
	free(q->manager_preferred_connection);
	assert(preferred_connection);

	if(strcmp(preferred_connection, "by_ip") && strcmp(preferred_connection, "by_hostname") && strcmp(preferred_connection, "by_apparent_ip")) {
		fatal("manager_preferred_connection should be one of: by_ip, by_hostname, by_apparent_ip");
	}

	q->manager_preferred_connection = xxstrdup(preferred_connection);
}

int ds_tune(struct ds_manager *q, const char *name, double value)
{

	if(!strcmp(name, "resource-submit-multiplier") || !strcmp(name, "asynchrony-multiplier")) {
		q->resource_submit_multiplier = MAX(value, 1.0);

	} else if(!strcmp(name, "min-transfer-timeout")) {
		q->minimum_transfer_timeout = (int)value;

	} else if(!strcmp(name, "default-transfer-rate")) {
		q->default_transfer_rate = value;

	} else if(!strcmp(name, "transfer-outlier-factor")) {
		q->transfer_outlier_factor = value;

	} else if(!strcmp(name, "fast-abort-multiplier")) {
		ds_activate_fast_abort(q, value);

	} else if(!strcmp(name, "keepalive-interval")) {
		q->keepalive_interval = MAX(0, (int)value);

	} else if(!strcmp(name, "keepalive-timeout")) {
		q->keepalive_timeout = MAX(0, (int)value);

	} else if(!strcmp(name, "short-timeout")) {
		q->short_timeout = MAX(1, (int)value);

	} else if(!strcmp(name, "long-timeout")) {
		q->long_timeout = MAX(1, (int)value);

	} else if(!strcmp(name, "category-steady-n-tasks")) {
		category_tune_bucket_size("category-steady-n-tasks", (int) value);

	} else if(!strcmp(name, "hungry-minimum")) {
		q->hungry_minimum = MAX(1, (int)value);

	} else if(!strcmp(name, "wait-for-workers")) {
		q->wait_for_workers = MAX(0, (int)value);

	} else if(!strcmp(name, "wait-retrieve-many")){
		q->wait_retrieve_many = MAX(0, (int)value);

	} else if(!strcmp(name, "force-proportional-resources")){
		q->force_proportional_resources = MAX(0, (int)value);

	} else {
		debug(D_NOTICE|D_DS, "Warning: tuning parameter \"%s\" not recognized\n", name);
		return -1;
	}

	return 0;
}

void ds_enable_process_module(struct ds_manager *q)
{
	q->process_pending_check = 1;
}

struct rmsummary ** ds_summarize_workers( struct ds_manager *q )
{
	return ds_manager_summarize_workers(q);
}

void ds_set_bandwidth_limit(struct ds_manager *q, const char *bandwidth)
{
	q->bandwidth_limit = string_metric_parse(bandwidth);
}

double ds_get_effective_bandwidth(struct ds_manager *q)
{
	double queue_bandwidth = get_queue_transfer_rate(q, NULL)/MEGABYTE; //return in MB per second
	return queue_bandwidth;
}

void ds_get_stats(struct ds_manager *q, struct ds_stats *s)
{
	struct ds_stats *qs;
	qs = q->stats;

	memcpy(s, qs, sizeof(*s));

	//info about workers
	s->workers_connected = count_workers(q, DS_WORKER_TYPE_WORKER);
	s->workers_init      = count_workers(q, DS_WORKER_TYPE_UNKNOWN);
	s->workers_busy      = workers_with_tasks(q);
	s->workers_idle      = s->workers_connected - s->workers_busy;
	// s->workers_able computed below.

	//info about tasks
	s->tasks_waiting      = task_state_count(q, NULL, DS_TASK_READY);
	s->tasks_with_results = task_state_count(q, NULL, DS_TASK_WAITING_RETRIEVAL);
	s->tasks_on_workers   = task_state_count(q, NULL, DS_TASK_RUNNING) + s->tasks_with_results;

	{
		//accumulate tasks running, from workers:
		char *key;
		struct ds_worker_info *w;
		s->tasks_running = 0;
		hash_table_firstkey(q->worker_table);
		while(hash_table_nextkey(q->worker_table, &key, (void **) &w)) {
			accumulate_stat(s, w->stats, tasks_running);
		}
		/* (see ds_get_stats_hierarchy for an explanation on the
		 * following line) */
		s->tasks_running = MIN(s->tasks_running, s->tasks_on_workers);
	}

	ds_task_info_compute_capacity(q, s);

	//info about resources
	s->bandwidth = ds_get_effective_bandwidth(q);
	struct ds_resources r;
	aggregate_workers_resources(q,&r,NULL);

	s->total_cores = r.cores.total;
	s->total_memory = r.memory.total;
	s->total_disk = r.disk.total;
	s->total_gpus = r.gpus.total;

	s->committed_cores = r.cores.inuse;
	s->committed_memory = r.memory.inuse;
	s->committed_disk = r.disk.inuse;
	s->committed_gpus = r.gpus.inuse;

	s->min_cores = r.cores.smallest;
	s->max_cores = r.cores.largest;
	s->min_memory = r.memory.smallest;
	s->max_memory = r.memory.largest;
	s->min_disk = r.disk.smallest;
	s->max_disk = r.disk.largest;
	s->min_gpus = r.gpus.smallest;
	s->max_gpus = r.gpus.largest;

	s->workers_able = count_workers_for_waiting_tasks(q, largest_seen_resources(q, NULL));
}

void ds_get_stats_hierarchy(struct ds_manager *q, struct ds_stats *s)
{
	ds_get_stats(q, s);

	char *key;
	struct ds_worker_info *w;

	/* Consider running only if reported by some hand. */
	s->tasks_running = 0;
	s->workers_connected = 0;

	hash_table_firstkey(q->worker_table);
	while(hash_table_nextkey(q->worker_table, &key, (void **) &w)) {
		accumulate_stat(s, w->stats, tasks_waiting);
		accumulate_stat(s, w->stats, tasks_running);
	}

	/* we rely on workers messages to update tasks_running. such data are
	 * attached to keepalive messages, thus tasks_running is not always
	 * current. Here we simply enforce that there can be more tasks_running
	 * that tasks_on_workers. */
	s->tasks_running = MIN(s->tasks_running, s->tasks_on_workers);

	/* Account also for workers connected directly to the manager. */
	s->workers_connected = s->workers_joined - s->workers_removed;

	s->workers_joined       += q->stats_disconnected_workers->workers_joined;
	s->workers_removed      += q->stats_disconnected_workers->workers_removed;
	s->workers_idled_out    += q->stats_disconnected_workers->workers_idled_out;
	s->workers_fast_aborted += q->stats_disconnected_workers->workers_fast_aborted;
	s->workers_lost         += q->stats_disconnected_workers->workers_lost;

	s->time_send         += q->stats_disconnected_workers->time_send;
	s->time_receive      += q->stats_disconnected_workers->time_receive;
	s->time_send_good    += q->stats_disconnected_workers->time_send_good;
	s->time_receive_good += q->stats_disconnected_workers->time_receive_good;

	s->time_workers_execute            += q->stats_disconnected_workers->time_workers_execute;
	s->time_workers_execute_good       += q->stats_disconnected_workers->time_workers_execute_good;
	s->time_workers_execute_exhaustion += q->stats_disconnected_workers->time_workers_execute_exhaustion;

	s->bytes_sent      += q->stats_disconnected_workers->bytes_sent;
	s->bytes_received  += q->stats_disconnected_workers->bytes_received;
}

void ds_get_stats_category(struct ds_manager *q, const char *category, struct ds_stats *s)
{
	struct category *c = ds_category_lookup_or_create(q, category);
	struct ds_stats *cs = c->ds_stats;
	memcpy(s, cs, sizeof(*s));

	//info about tasks
	s->tasks_waiting      = task_state_count(q, category, DS_TASK_READY);
	s->tasks_running      = task_state_count(q, category, DS_TASK_RUNNING);
	s->tasks_with_results = task_state_count(q, category, DS_TASK_WAITING_RETRIEVAL);
	s->tasks_on_workers   = s->tasks_running + s->tasks_with_results;
	s->tasks_submitted    = c->total_tasks + s->tasks_waiting + s->tasks_on_workers;

	s->workers_able  = count_workers_for_waiting_tasks(q, largest_seen_resources(q, c->name));
}

char *ds_status(struct ds_manager *q, const char *request) {
	struct jx *a = construct_status_message(q, request);

	if(!a) {
		return "[]";
	}

	char *result = jx_print_string(a);

	jx_delete(a);

	return result;
}

static void aggregate_workers_resources( struct ds_manager *q, struct ds_resources *total, struct hash_table *features)
{
	struct ds_worker_info *w;
	char *key;

	bzero(total, sizeof(struct ds_resources));

	if(hash_table_size(q->worker_table)==0) {
		return;
	}

	if(features) {
		hash_table_clear(features,0);
	}

	hash_table_firstkey(q->worker_table);
	while(hash_table_nextkey(q->worker_table,&key,(void**)&w)) {
		if(w->resources->tag < 0)
			continue;

		ds_resources_add(total,w->resources);

		if(features) {
			if(w->features) {
				char *key;
				void *dummy;
				hash_table_firstkey(w->features);
				while(hash_table_nextkey(w->features, &key, &dummy)) {
					hash_table_insert(features, key, (void **) 1);
				}
			}
		}
	}
}

int ds_specify_log(struct ds_manager *q, const char *filename)
{
	q->perf_logfile = fopen(filename, "a");
	if(q->perf_logfile) {
		ds_perf_log_write_header(q);
		ds_perf_log_write_update(q,1);
		debug(D_DS, "log enabled and is being written to %s\n", filename);
		return 1;
	} else {
		debug(D_NOTICE | D_DS, "couldn't open logfile %s: %s\n", filename, strerror(errno));
		return 0;
	}
}

int ds_specify_transactions_log(struct ds_manager *q, const char *filename)
{
	q->txn_logfile = fopen(filename, "a");
	if(q->txn_logfile) {
		debug(D_DS, "transactions log enabled and is being written to %s\n", filename);
		ds_txn_log_write_header(q);
		ds_txn_log_write(q, "MANAGER START");
		return 1;
	} else {
		debug(D_NOTICE | D_DS, "couldn't open transactions logfile %s: %s\n", filename, strerror(errno));
		return 0;
	}
}
	
void ds_accumulate_task(struct ds_manager *q, struct ds_task *t) {
	const char *name   = t->category ? t->category : "default";
	struct category *c = ds_category_lookup_or_create(q, name);

	struct ds_stats *s = c->ds_stats;

	s->bytes_sent     += t->bytes_sent;
	s->bytes_received += t->bytes_received;

	s->time_workers_execute += t->time_workers_execute_last;

	s->time_send    += t->time_when_commit_end - t->time_when_commit_start;
	s->time_receive += t->time_when_done - t->time_when_retrieval;

	s->bandwidth = (1.0*MEGABYTE*(s->bytes_sent + s->bytes_received))/(s->time_send + s->time_receive + 1);

	q->stats->tasks_done++;

	if(t->result == DS_RESULT_SUCCESS)
	{
		q->stats->time_workers_execute_good += t->time_workers_execute_last;
		q->stats->time_send_good            += t->time_when_commit_end - t->time_when_commit_end;
		q->stats->time_receive_good         += t->time_when_done - t->time_when_retrieval;

		s->tasks_done++;
		s->time_workers_execute_good += t->time_workers_execute_last;
		s->time_send_good            += t->time_when_commit_end - t->time_when_commit_end;
		s->time_receive_good         += t->time_when_done - t->time_when_retrieval;
	} else {
		s->tasks_failed++;

		if(t->result == DS_RESULT_RESOURCE_EXHAUSTION) {
			s->time_workers_execute_exhaustion += t->time_workers_execute_last;

			q->stats->time_workers_execute_exhaustion += t->time_workers_execute_last;
			q->stats->tasks_exhausted_attempts++;

			t->time_workers_execute_exhaustion += t->time_workers_execute_last;
			t->exhausted_attempts++;
		}
	}

	/* accumulate resource summary to category only if task result makes it meaningful. */
	switch(t->result) {
		case DS_RESULT_SUCCESS:
		case DS_RESULT_SIGNAL:
		case DS_RESULT_RESOURCE_EXHAUSTION:
		case DS_RESULT_TASK_MAX_RUN_TIME:
		case DS_RESULT_DISK_ALLOC_FULL:
		case DS_RESULT_OUTPUT_TRANSFER_ERROR:
			if(category_accumulate_summary(c, t->resources_measured, q->current_max_worker)) {
				ds_txn_log_write_category(q, c);
			}
			break;
		case DS_RESULT_INPUT_MISSING:
		case DS_RESULT_OUTPUT_MISSING:
		case DS_RESULT_TASK_TIMEOUT:
		case DS_RESULT_UNKNOWN:
		case DS_RESULT_FORSAKEN:
		case DS_RESULT_MAX_RETRIES:
		default:
			break;
	}
}

void ds_initialize_categories(struct ds_manager *q, struct rmsummary *max, const char *summaries_file) {
	categories_initialize(q->categories, max, summaries_file);
}

void ds_specify_max_resources(struct ds_manager *q,  const struct rmsummary *rm) {
	ds_specify_category_max_resources(q,  "default", rm);
}

void ds_specify_min_resources(struct ds_manager *q,  const struct rmsummary *rm) {
	ds_specify_category_min_resources(q,  "default", rm);
}

void ds_specify_category_max_resources(struct ds_manager *q,  const char *category, const struct rmsummary *rm) {
	struct category *c = ds_category_lookup_or_create(q, category);
	category_specify_max_allocation(c, rm);
}

void ds_specify_category_min_resources(struct ds_manager *q,  const char *category, const struct rmsummary *rm) {
	struct category *c = ds_category_lookup_or_create(q, category);
	category_specify_min_allocation(c, rm);
}

void ds_specify_category_first_allocation_guess(struct ds_manager *q,  const char *category, const struct rmsummary *rm) {
	struct category *c = ds_category_lookup_or_create(q, category);
	category_specify_first_allocation_guess(c, rm);
}

int ds_specify_category_mode(struct ds_manager *q, const char *category, ds_category_mode_t mode) {

	switch(mode) {
		case CATEGORY_ALLOCATION_MODE_FIXED:
		case CATEGORY_ALLOCATION_MODE_MAX:
		case CATEGORY_ALLOCATION_MODE_MIN_WASTE:
		case CATEGORY_ALLOCATION_MODE_MAX_THROUGHPUT:
			break;
		default:
			notice(D_DS, "Unknown category mode specified.");
			return 0;
			break;
	}

	if(!category) {
		q->allocation_default_mode = mode;
	}
	else {
		struct category *c = ds_category_lookup_or_create(q, category);
		category_specify_allocation_mode(c, (category_mode_t) mode);
		ds_txn_log_write_category(q, c);
	}

	return 1;
}

int ds_enable_category_resource(struct ds_manager *q, const char *category, const char *resource, int autolabel) {

	struct category *c = ds_category_lookup_or_create(q, category);

	return category_enable_auto_resource(c, resource, autolabel);
}

const struct rmsummary *ds_manager_task_max_resources(struct ds_manager *q, struct ds_task *t) {

	struct category *c = ds_category_lookup_or_create(q, t->category);

	return category_dynamic_task_max_resources(c, t->resources_requested, t->resource_request);
}

const struct rmsummary *ds_manager_task_min_resources(struct ds_manager *q, struct ds_task *t) {
	struct category *c = ds_category_lookup_or_create(q, t->category);

	const struct rmsummary *s = category_dynamic_task_min_resources(c, t->resources_requested, t->resource_request);

	if(t->resource_request != CATEGORY_ALLOCATION_FIRST || !q->current_max_worker) {
		return s;
	}

	// If this task is being tried for the first time, we take the minimum as
	// the minimum between what we have observed and the largest worker. This
	// is to eliminate observed outliers that would prevent new tasks to run.
	if((q->current_max_worker->cores > 0 && q->current_max_worker->cores < s->cores)
			|| (q->current_max_worker->memory > 0 && q->current_max_worker->memory < s->memory)
			|| (q->current_max_worker->disk > 0 && q->current_max_worker->disk < s->disk)
			|| (q->current_max_worker->gpus > 0 && q->current_max_worker->gpus < s->gpus)) {

		struct rmsummary *r = rmsummary_create(-1);

		rmsummary_merge_override(r, q->current_max_worker);
		rmsummary_merge_override(r, t->resources_requested);

		s = category_dynamic_task_min_resources(c, r, t->resource_request);
		rmsummary_delete(r);
	}

	return s;
}

struct category *ds_category_lookup_or_create(struct ds_manager *q, const char *name) {
	struct category *c = category_lookup_or_create(q->categories, name);

	if(!c->ds_stats) {
		c->ds_stats = calloc(1, sizeof(struct ds_stats));
		category_specify_allocation_mode(c, (category_mode_t) q->allocation_default_mode);
	}

	return c;
}

int ds_specify_min_taskid(struct ds_manager *q, int minid) {

	if(minid > q->next_taskid) {
		q->next_taskid = minid;
	}

	return q->next_taskid;
}

/* vim: set noexpandtab tabstop=4: */
