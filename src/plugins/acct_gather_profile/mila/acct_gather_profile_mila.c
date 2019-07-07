/*****************************************************************************\
 *  acct_gather_profile_mila.c - slurm accounting plugin for mila
 *				     profiling.
 *****************************************************************************
 *  Author: Pierre Delaunay
 *  Copyright (C) 2019
 *
 *  Based on the HDF5 profiling plugin and Elasticsearch job completion plugin.
 *
 *  Portions Copyright (C) 2013 SchedMD LLC.
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <http://www.schedmd.com/slurmdocs/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  Slurm is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
 *
 *  This file is patterned after jobcomp_linux.c, written by Morris Jette and
 *  Copyright (C) 2002 The Regents of the University of California.
 \*****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include <unistd.h>
#include <pwd.h>
#include <math.h>

#include <nvml.h>
#include <curl/curl.h>

#include "src/common/slurm_xlator.h"
#include "src/common/fd.h"
#include "src/common/slurm_acct_gather_profile.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/slurm_protocol_defs.h"
#include "src/common/slurm_time.h"
#include "src/common/macros.h"
#include "src/slurmd/common/proctrack.h"


/*
 * These variables are required by the generic plugin interface.  If they
 * are not found in the plugin, the plugin loader will ignore it.
 *
 * plugin_name - a string giving a human-readable description of the
 * plugin.  There is no maximum length, but the symbol must refer to
 * a valid string.
 *
 * plugin_type - a string suggesting the type of the plugin or its
 * applicability to a particular form of data or method of data handling.
 * If the low-level plugin API is used, the contents of this string are
 * unimportant and may be anything.  Slurm uses the higher-level plugin
 * interface which requires this string to be of the form
 *
 *	<application>/<method>
 *
 * where <application> is a description of the intended application of
 * the plugin (e.g., "jobacct" for Slurm job completion logging) and <method>
 * is a description of how this plugin satisfies that application.  Slurm will
 * only load job completion logging plugins if the plugin_type string has a
 * prefix of "jobacct/".
 *
 * plugin_version - an unsigned 32-bit integer containing the Slurm version
 * (major.minor.micro combined into a single number).
 */
const char plugin_name[] = "AcctGatherProfile mila plugin";
const char plugin_type[] = "acct_gather_profile/mila";
const uint32_t plugin_version = SLURM_VERSION_NUMBER;

const uint32_t version = 2;
FILE* log_file = NULL;


typedef struct {
        char *host;
        char *database;
        uint32_t def;
        char *password;
        char *rt_policy;
        char *username;
} slurm_mila_conf_t;

typedef struct {
        char ** names;
        uint32_t *types;
        size_t size;
        char * name;
} table_t;

typedef struct {
    const char* name;
    float value;
} metric_t;

/* Type for handling HTTP responses */
struct http_response {
        char *message;
        size_t size;
};

union data_t{
        uint64_t u;
        double	 d;
};

static slurm_mila_conf_t mila_conf;
static uint32_t g_profile_running = ACCT_GATHER_PROFILE_NOT_SET;
static stepd_step_rec_t *g_job = NULL;

static char *datastr = NULL;
static int datastrlen = 0;

static table_t *tables = NULL;
static size_t tables_max_len = 0;
static size_t tables_cur_len = 0;

static  nvmlDevice_t* job_devices = NULL;
static size_t device_count = 0;

static void _free_tables(void)
{
        int i, j;

        debug3("[MILA]" "%s %s called", plugin_type, __func__);

        for (i = 0; i < tables_cur_len; i++) {
                table_t *table = &(tables[i]);
                for (j = 0; j < tables->size; j++)
                        xfree(table->names[j]);
                xfree(table->name);
                xfree(table->names);
                xfree(table->types);
        }
        xfree(tables);
}

static uint32_t _determine_profile(void)
{
        uint32_t profile;

        debug3("[MILA]" "%s %s called", plugin_type, __func__);
        xassert(g_job);

        if (g_profile_running != ACCT_GATHER_PROFILE_NOT_SET)
                profile = g_profile_running;
        else if (g_job->profile >= ACCT_GATHER_PROFILE_NONE)
                profile = g_job->profile;
        else
                profile = mila_conf.def;

        return profile;
}

static bool _run_in_daemon(void)
{
        static bool set = false;
        static bool run = false;

        debug3("[MILA]" "%s %s called", plugin_type, __func__);

        if (!set) {
                set = 1;
                run = run_in_daemon("slurmstepd");
        }

        return run;
}



/* Callback to handle the HTTP response */
static size_t _write_callback(void *contents, size_t size, size_t nmemb,
                              void *userp)
{
        size_t realsize = size * nmemb;
        struct http_response *mem = (struct http_response *) userp;

        debug3("[MILA]" "%s %s called", plugin_type, __func__);

        mem->message = xrealloc(mem->message, mem->size + realsize + 1);

        memcpy(&(mem->message[mem->size]), contents, realsize);
        mem->size += realsize;
        mem->message[mem->size] = 0;

        return realsize;
}

/* Try to send data to mila */
static int _send_data(const char *data)
{
    debug("[MILA]" "%s \n", data);

        return 0;

        CURL *curl_handle = NULL;
        CURLcode res;
        struct http_response chunk;
        int rc = SLURM_SUCCESS;
        long response_code;
        static int error_cnt = 0;
        char *url = NULL;
        size_t length;

        debug3("[MILA]" "%s %s called", plugin_type, __func__);

        /*
         * Every compute node which is sampling data will try to establish a
         * different connection to the mila server. In order to reduce the
         * number of connections, every time a new sampled data comes in, it
         * is saved in the 'datastr' buffer. Once this buffer is full, then we
         * try to open the connection and send this buffer, instead of opening
         * one per sample.
         */
        if (data && ((datastrlen + strlen(data)) <= BUF_SIZE)) {
                xstrcat(datastr, data);
                length = strlen(data);
                datastrlen += length;
                if (slurm_get_debug_flags() & DEBUG_FLAG_PROFILE)
                        info("[MILA]" "%s %s: %zu bytes of data added to buffer. New buffer size: %d",
                             plugin_type, __func__, length, datastrlen);
                return rc;
        }

        DEF_TIMERS;
        START_TIMER;

        if (curl_global_init(CURL_GLOBAL_ALL) != 0) {
                error("[MILA]" "%s %s: curl_global_init: %m", plugin_type, __func__);
                rc = SLURM_ERROR;
                goto cleanup_global_init;
        } else if ((curl_handle = curl_easy_init()) == NULL) {
                error("[MILA]" "%s %s: curl_easy_init: %m", plugin_type, __func__);
                rc = SLURM_ERROR;
                goto cleanup_easy_init;
        }

        xstrfmtcat(url, "%s/write?db=%s&rp=%s&precision=s", mila_conf.host,
                   mila_conf.database, mila_conf.rt_policy);

        chunk.message = xmalloc(1);
        chunk.size = 0;

        curl_easy_setopt(curl_handle, CURLOPT_URL, url);
        if (mila_conf.password)
                curl_easy_setopt(curl_handle, CURLOPT_PASSWORD,
                                 mila_conf.password);
        curl_easy_setopt(curl_handle, CURLOPT_POST, 1);
        curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, datastr);
        curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDSIZE, strlen(datastr));
        if (mila_conf.username)
                curl_easy_setopt(curl_handle, CURLOPT_USERNAME,
                                 mila_conf.username);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, _write_callback);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *) &chunk);

        if ((res = curl_easy_perform(curl_handle)) != CURLE_OK) {
                if ((error_cnt++ % 100) == 0)
                        error("[MILA]" "%s %s: curl_easy_perform failed to send data (discarded). Reason: %s",
                              plugin_type, __func__, curl_easy_strerror(res));
                rc = SLURM_ERROR;
                goto cleanup;
        }

        if ((res = curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE,
                                     &response_code)) != CURLE_OK) {
                error("[MILA]" "%s %s: curl_easy_getinfo response code failed: %s",
                      plugin_type, __func__, curl_easy_strerror(res));
                rc = SLURM_ERROR;
                goto cleanup;
        }

        /* In general, status codes of the form 2xx indicate success,
         * 4xx indicate that MilaDB could not understand the request, and
         * 5xx indicate that the system is overloaded or significantly impaired.
         * Errors are returned in JSON.
         * https://docs.influxdata.com/mila/v0.13/concepts/api/
         */
        if (response_code >= 200 && response_code <= 205) {
                debug2("[MILA]" "%s %s: data write success", plugin_type, __func__);
                if (error_cnt > 0)
                        error_cnt = 0;
        } else {
                rc = SLURM_ERROR;
                debug2("[MILA]" "%s %s: data write failed, response code: %ld",
                       plugin_type, __func__, response_code);
                if (slurm_get_debug_flags() & DEBUG_FLAG_PROFILE) {
                        /* Strip any trailing newlines. */
                        while (chunk.message[strlen(chunk.message) - 1] == '\n')
                                chunk.message[strlen(chunk.message) - 1] = '\0';
                        info("[MILA]" "%s %s: JSON response body: %s", plugin_type,
                             __func__, chunk.message);
                }
        }

cleanup:
        xfree(chunk.message);
        xfree(url);
cleanup_easy_init:
        curl_easy_cleanup(curl_handle);
cleanup_global_init:
        curl_global_cleanup();

        END_TIMER;
        if (slurm_get_debug_flags() & DEBUG_FLAG_PROFILE)
                debug("[MILA]" "%s %s: took %s to send data", plugin_type, __func__,
                      TIME_STR);  fflush(stdout);

        if (data) {
                datastr = xstrdup(data);
                datastrlen = strlen(data);
        } else {
                datastr[0] = '\0';
                datastrlen = 0;
        }

        return rc;
}

// check NVML return code
int check(nvmlReturn_t error, const char *file, const char *fun, int line, const char *call_str)
{
    if (error != NVML_SUCCESS) {
        debug("[!] %s/%s:%d %s %s\n", file, fun, line, nvmlErrorString(error), call_str);
        fflush(stdout);
        return 0;
    };
    return 1;
}
#define CHK(X) check(X, __FILE__, __func__, __LINE__, #X)

/*
 * init() is called when the plugin is loaded, before any other functions
 * are called. Put global initialization here.
 */
extern int init(void)
{
    //log_file = fopen("/slurm/mila_plugin.log", "rw");
    //fprintf(log_file, "[MILA] %s %s called (v: %d)\n", plugin_type, __func__, version);
    //fflush(log_file);

        debug3("[MILA]" "%s %s called (v: %d)", plugin_type, __func__, version);

        if (!_run_in_daemon())
                return SLURM_SUCCESS;

        // NVML_INIT_FLAG_NO_GPUS
        CHK(nvmlInit());
        datastr = xmalloc(BUF_SIZE);
        return SLURM_SUCCESS;
}

extern int fini(void)
{
        debug3("[MILA]" "%s %s called", plugin_type, __func__); fflush(stdout);
        CHK(nvmlShutdown());

        _free_tables();
        xfree(datastr);
        xfree(mila_conf.host);
        xfree(mila_conf.database);
        xfree(mila_conf.password);
        xfree(mila_conf.rt_policy);
        xfree(mila_conf.username);

    fclose(log_file);
        return SLURM_SUCCESS;
}

extern void acct_gather_profile_p_conf_options(s_p_options_t **full_options,
                                               int *full_options_cnt)
{
        debug3("[MILA]" "%s %s called", plugin_type, __func__); fflush(stdout);

        s_p_options_t options[] = {
                {"ProfileMilaDBHost", S_P_STRING},
                {"ProfileMilaDBDatabase", S_P_STRING},
                {"ProfileMilaDBDefault", S_P_STRING},
                {"ProfileMilaDBPass", S_P_STRING},
                {"ProfileMilaDBRTPolicy", S_P_STRING},
                {"ProfileMilaDBUser", S_P_STRING},
                {NULL} };

        transfer_s_p_options(full_options, options, full_options_cnt);
        return;
}

extern void acct_gather_profile_p_conf_set(s_p_hashtbl_t *tbl)
{
        char *tmp = NULL;

        debug3("[MILA]" "%s %s called", plugin_type, __func__); fflush(stdout);

        mila_conf.def = ACCT_GATHER_PROFILE_ALL;
        //*
        if (tbl) {
                s_p_get_string(&mila_conf.host, "ProfileMilaDBHost", tbl);
                if (s_p_get_string(&tmp, "ProfileMilaDBDefault", tbl)) {
                        mila_conf.def =
                                acct_gather_profile_from_string(tmp);
                        if (mila_conf.def == ACCT_GATHER_PROFILE_NOT_SET)
                                fatal("[MILA]" "ProfileMilaDBDefault can not be set to %s, please specify a valid option",
                                      tmp);
                        xfree(tmp);
                }
                s_p_get_string(&mila_conf.database,
                               "ProfileMilaDBDatabase", tbl);
                s_p_get_string(&mila_conf.password,
                               "ProfileMilaDBPass", tbl);
                s_p_get_string(&mila_conf.rt_policy,
                               "ProfileMilaDBRTPolicy", tbl);
                s_p_get_string(&mila_conf.username,
                               "ProfileMilaDBUser", tbl);
        }
        //*/
        //*
        if (!mila_conf.host)
                fatal("[MILA]" "No ProfileMilaDBHost in your acct_gather.conf file. This is required to use the %s plugin",
                      plugin_type);

        if (!mila_conf.database)
                fatal("[MILA]" "No ProfileMilaDBDatabase in your acct_gather.conf file. This is required to use the %s plugin",
                      plugin_type);

        if (mila_conf.password && !mila_conf.username)
                fatal("[MILA]" "No ProfileMilaDBUser in your acct_gather.conf file. This is required if ProfileMilaDBPass is specified to use the %s plugin",
                      plugin_type);

        if (!mila_conf.rt_policy)
                fatal("[MILA]" "No ProfileMilaDBRTPolicy in your acct_gather.conf file. This is required to use the %s plugin",
                      plugin_type);
        //*/

        debug("[MILA]" "%s loaded", plugin_name); fflush(stdout);
}

extern void acct_gather_profile_p_get(enum acct_gather_profile_info info_type,
                                      void *data)
{
        uint32_t *uint32 = (uint32_t *) data;
        char **tmp_char = (char **) data;

        debug3("[MILA]" "%s %s called", plugin_type, __func__); fflush(stdout);

        switch (info_type) {
        case ACCT_GATHER_PROFILE_DIR:
                *tmp_char = xstrdup(mila_conf.host);
                break;
        case ACCT_GATHER_PROFILE_DEFAULT:
                *uint32 = mila_conf.def;
                break;
        case ACCT_GATHER_PROFILE_RUNNING:
                *uint32 = g_profile_running;
                break;
        default:
                debug2("[MILA]" "%s %s: info_type %d invalid", plugin_type,
                       __func__, info_type); fflush(stdout);
        }
}

extern int acct_gather_profile_p_node_step_start(stepd_step_rec_t* job)
{
        int rc = SLURM_SUCCESS;
        char *profile_str;

        debug3("[MILA]" "%s %s called", plugin_type, __func__); fflush(stdout);

        xassert(_run_in_daemon());

        g_job = job;
        profile_str = acct_gather_profile_to_string(g_job->profile);
        debug2("[MILA]" "%s %s: option --profile=%s", plugin_type, __func__,
               profile_str); fflush(stdout);

        /*
        List job_gres_list;     Needed by GRES plugin
        List step_gres_list;    Needed by GRES plugin
        */

        ListIterator gres_iter;
        gres_iter = list_iterator_create(gres_list);
        gres_state_t *gres_ptr;

        while ((gres_ptr = (gres_state_t *) list_next(gres_iter))) {

        }





    // Get the USED GPU!
    job_devices = NULL;
    device_count = 0;

    /*
    char **iter = job->env;

        while (*iter != NULL) {
                size_t n = strlen(*iter);

        debug3("[MILA] " "%s=%s", *iter, iter[n + 1]);
                iter += 1;
        }*/


        //g_profile_running = _determine_profile();
        return rc;
}

extern int acct_gather_profile_p_child_forked(void)
{
        debug3("[MILA]" "%s %s called", plugin_type, __func__); fflush(stdout);
        return SLURM_SUCCESS;
}

extern int acct_gather_profile_p_node_step_end(void)
{
        int rc = SLURM_SUCCESS;
        debug3("[MILA]" "%s %s called", plugin_type, __func__); fflush(stdout);

        xassert(_run_in_daemon());

        return rc;
}

extern int acct_gather_profile_p_task_start(uint32_t taskid)
{
        int rc = SLURM_SUCCESS;

        debug3("[MILA]" "%s %s called with %d prof", plugin_type, __func__,
               g_profile_running); fflush(stdout);

        xassert(_run_in_daemon());
        xassert(g_job);
        xassert(g_profile_running != ACCT_GATHER_PROFILE_NOT_SET);

        // ---
        if (g_profile_running <= ACCT_GATHER_PROFILE_NONE)
                return rc;

        return rc;
}

extern int acct_gather_profile_p_task_end(pid_t taskpid)
{
        debug3("[MILA]" "%s %s called", plugin_type, __func__); fflush(stdout);

        _send_data(NULL);
        return SLURM_SUCCESS;
}

extern int64_t acct_gather_profile_p_create_group(const char* name)
{
        debug3("[MILA]" "%s %s called", plugin_type, __func__); fflush(stdout);

        return 0;
}

extern int acct_gather_profile_p_create_dataset(const char* name,
                                                int64_t parent,
                                                acct_gather_profile_dataset_t
                                                *dataset)
{
        table_t * table;
        acct_gather_profile_dataset_t *dataset_loc = dataset;

        debug3("[MILA]" "%s %s called", plugin_type, __func__); fflush(stdout);

        /* compute the size of the type needed to create the table */
        if (tables_cur_len == tables_max_len) {
                if (tables_max_len == 0)
                        ++tables_max_len;

                tables_max_len *= 2;
                tables = xrealloc(tables, tables_max_len * sizeof(table_t));
        }

        table = &(tables[tables_cur_len]);
        table->name = xstrdup(name);
        table->size = 0;

        while (dataset_loc && (dataset_loc->type != PROFILE_FIELD_NOT_SET)) {
                table->names = xrealloc(table->names,
                                        (table->size+1) * sizeof(char *));
                table->types = xrealloc(table->types,
                                        (table->size+1) * sizeof(char *));
                (table->names)[table->size] = xstrdup(dataset_loc->name);
                switch (dataset_loc->type) {
                case PROFILE_FIELD_UINT64:
                        table->types[table->size] =
                                PROFILE_FIELD_UINT64;
                        break;
                case PROFILE_FIELD_DOUBLE:
                        table->types[table->size] =
                                PROFILE_FIELD_DOUBLE;
                        break;
                case PROFILE_FIELD_NOT_SET:
                        break;
                }
                table->size++;
                dataset_loc++;
        }
        ++tables_cur_len;
        return tables_cur_len - 1;
}

extern int acct_gather_profile_p_add_sample_data(int table_id, void *data,
                                                 time_t sample_time)
{
        table_t *table = &tables[table_id];
        int i = 0;
        char *str = NULL;

        debug3("[MILA]" "%s %s called", plugin_type, __func__); fflush(stdout);

        // get username
        struct passwd *pws;
        pws = getpwuid(g_job->uid);
        const char* username = pws->pw_name;

        for(; i < table->size; i++) {
                switch (table->types[i]) {
                case PROFILE_FIELD_UINT64:
                        xstrfmtcat(str, "%s,user=%s,job=%d,step=%d,task=%s,"
                                   "host=%s value=%"PRIu64" "
                                   "%"PRIu64"\n", table->names[i], username,
                                   g_job->jobid, g_job->stepid,
                                   table->name, g_job->node_name,
                                   ((union data_t*)data)[i].u,
                                   sample_time);
                        break;
                case PROFILE_FIELD_DOUBLE:
                        xstrfmtcat(str, "%s,user=%s,job=%d,step=%d,task=%s,"
                                   "host=%s value=%.2f %"PRIu64""
                                   "\n", table->names[i], username,
                                   g_job->jobid, g_job->stepid,
                                   table->name, g_job->node_name,
                                   ((union data_t*)data)[i].d,
                                   sample_time);
                        break;
                case PROFILE_FIELD_NOT_SET:
                        break;
                }
        }//*
        debug3("[MILA]" "gather GPU info %d", table_id); fflush(stdout);
        // pid_t pid = g_job->jobacct->pid;

        nvmlMemory_t mem_info;
        nvmlUtilization_t util_info;

        unsigned int encode_util = 0; float encode_avg = 0;
        unsigned int decode_util = 0; float decode_avg = 0;
        unsigned int       power = 0; float  power_avg = 0;

        float  used_memory = 0;
        float total_memory = 0;

        float    gpu_util = 0;
        float memory_util = 0;

        unsigned int sample_period = 0;

        for(int i = 0; i < device_count; ++i){
             nvmlDevice_t* device = job_devices[i];

             // Benzina uses the encode/decode
             CHK(nvmlDeviceGetEncoderUtilization(device, &encode_util, &sample_period));
             encode_avg += (float) encode_util;

             CHK(nvmlDeviceGetDecoderUtilization(device, &decode_util, &sample_period));
             decode_avg += (float) decode_util;

             CHK(nvmlDeviceGetMemoryInfo(device, &mem_info));
             used_memory += (float) mem_info.used;
             total_memory = (float) mem_info.total;

             CHK(nvmlDeviceGetPowerUsage(device, &power));
             power_avg += (float) power;

             CHK(nvmlDeviceGetUtilizationRates(device, &util_info));
             gpu_util += (float) util_info.gpu;
             memory_util  += (float) util_info.memory;
        }

        if (device_count > 0){
            encode_avg  /= (float) device_count;
            decode_avg  /= (float) device_count;
            used_memory /= (float) device_count;
            power_avg   /= (float) device_count;
            gpu_util    /= (float) device_count;
            memory_util /= (float) device_count;

            metric_t metrics[] = {
                {"encode" , encode_avg},
                {"decode" , decode_avg},
                {"used"   , used_memory},
                {"total"  , total_memory},
                {"power"  , power_avg},
                {"compute", gpu_util},
                {"mem"    , memory_util}
            };

            for(int i = 0; i < sizeof(metrics); i++){
                metric_t m = metrics[i];

                xstrfmtcat(str, "%s,user=%s,job=%d,step=%d,task=%s,"
                           "host=%s value=%.2f %"PRIu64""
                           "\n", m.name, username,
                           g_job->jobid, g_job->stepid,
                           table->name, g_job->node_name,
                           m.value,
                           sample_time);
            }
        }

        // GPU monitoring done
        // ---------*/
        _send_data(str);
        xfree(str);

        return SLURM_SUCCESS;
}

extern void acct_gather_profile_p_conf_values(List *data)
{
        config_key_pair_t *key_pair;

        debug3("[MILA]" "%s %s called", plugin_type, __func__); fflush(stdout);

        xassert(*data);

        key_pair = xmalloc(sizeof(config_key_pair_t));
        key_pair->name = xstrdup("ProfileMilaDBHost");
        key_pair->value = xstrdup(mila_conf.host);
        list_append(*data, key_pair);

        key_pair = xmalloc(sizeof(config_key_pair_t));
        key_pair->name = xstrdup("ProfileMilaDBDatabase");
        key_pair->value = xstrdup(mila_conf.database);
        list_append(*data, key_pair);

        key_pair = xmalloc(sizeof(config_key_pair_t));
        key_pair->name = xstrdup("ProfileMilaDBDefault");
        key_pair->value =
                xstrdup(acct_gather_profile_to_string(mila_conf.def));
        list_append(*data, key_pair);

        key_pair = xmalloc(sizeof(config_key_pair_t));
        key_pair->name = xstrdup("ProfileMilaDBPass");
        key_pair->value = xstrdup(mila_conf.password);
        list_append(*data, key_pair);

        key_pair = xmalloc(sizeof(config_key_pair_t));
        key_pair->name = xstrdup("ProfileMilaDBRTPolicy");
        key_pair->value = xstrdup(mila_conf.rt_policy);
        list_append(*data, key_pair);

        key_pair = xmalloc(sizeof(config_key_pair_t));
        key_pair->name = xstrdup("ProfileMilaDBUser");
        key_pair->value = xstrdup(mila_conf.username);
        list_append(*data, key_pair);

        return;

}

extern bool acct_gather_profile_p_is_active(uint32_t type)
{
        debug3("[MILA]" "%s %s called", plugin_type, __func__); fflush(stdout);
        return true;
}
