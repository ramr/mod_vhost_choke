/*  =======================================================================
 * 
 *  ~ramr
 *  <see-license-file />
 *  <insert-mit-license-here />
 *  
 *  =======================================================================
 *
 *     File:  mod_vhost_choke.c
 *
 *    Author: ~ramr
 *
 *  Summary:  Apache module - "carburator" to control the request/server
 *            mixture. Throttles requests/connections to a virtual host. 
 *
 */

/*  section:compile {{{
 *  +++++++++++++++
 *     normal:  apxs -i -a -c mod_vhost_choke.c
 *     debug:   apxs -i -a -Wc,-g -Wc,-O0 -c mod_vhost_choke.c
 *     trace:   apxs -i -a -Wc,-g -Wc,-O0 -Wc,-DVHC_DEV_DEBUG    \
 *                   -c mod_vhost_choke.c
 *
 *  }}}  -- End section:compile.
 */


/*  section:includes {{{  */
/*  ++++++++++++++++      */

/*  Include the standard header files we need.  */
#include "mod_vhost_choke.h"

#include "ap_config.h"

#include "apr_file_io.h"
#include "apr_shm.h"
#include "apr_strings.h"
#include "apr_tables.h"

#include "httpd.h"
#include "http_config.h"
#include "http_log.h"
#include "http_protocol.h"
/* #include "http_request.h"  */

/*  We need to setup the mutex permissions for most *nix platforms.  */
#if !defined(WIN32)  &&  !defined(OS2)  &&  !defined(BEOS)  &&  \
    !defined(NETWARE)
   #include "unixd.h"
   #define  AP_NEED_TO_SET_MUTEX_PERMS  "Apache should fix this"
#endif  /*  !defined({WIN32,OS2,BEOS,NETWARE})  */

/*  }}}  -- End section:includes.  */


/*  section:defines {{{  */
/*  +++++++++++++++      */

/*  Define for logging debug messages and turn on debugging.  */
#define  VHC_DEBUG   if (VHC_TRUE == gs_vhc_env_settings.debug)

/*  }}}  -- End section:defines.  */


/*  section:globals {{{  */
/*  +++++++++++++++      */

module AP_MODULE_DECLARE_DATA  vhost_choke_module;

/*  Static global variables - visibility is restricted to this file.  */
static VHC_env_settings_t  gs_vhc_env_settings = {

#ifdef  VHC_DEV_DEBUG
   .debug       = VHC_TRUE,
#else
   .debug       = VHC_FALSE,
#endif

   .http_code   = VHC_DEFAULT_HTTP_ERROR_CODE,
   .err_message = VHC_DEFAULT_ERROR_MSG
};

static pid_t                gs_mypid       = 0;
static apr_file_t          *gs_debug_file  = NULL;

static apr_uint16_t         gs_num_configs = 0;

static char                *gs_shm_lockfile = NULL;  /*  Global lockf.  */
static apr_global_mutex_t  *gs_shm_lock     = NULL;  /*  Global lock.   */

static char                *gs_shm_file = NULL;  /*  shm file name.     */
static apr_shm_t           *gs_shm      = NULL;  /*  shm segment.       */

/*  }}}  -- End section:globals.  */


/*  section:forward-decs {{{  */
/*  ++++++++++++++++++++      */

/*  Forward declarations.  */

/*  Internal helper functions.  */
static void   vhc_initialize_process_env_(apr_pool_t *pool);
static void   vhc_debug_log_(apr_pool_t *pool, char *fmt, ...);
static char  *vhc_get_vhost_name_(server_rec *srvr);
static void   vhc_generate_choked_headers_(request_rec  *req,
                                           apr_uint16_t  http_code);
static void   vhc_generate_choked_page_content_(request_rec *req,
                                                const char  *title,
                                                const char  *msg);
static char  *vhc_mktemp_(apr_pool_t *pool, const char *template);
static int    vhc_create_global_lock_(apr_pool_t *pool);
static int    vhc_create_shm_segment_(apr_pool_t *pool, apr_shm_t **shm,
                                      const char *template, char *shmfile);
static int    vhc_lock_acquire_(apr_global_mutex_t *lock,
                                int timeout_usecs);
static int    vhc_lock_release_(apr_global_mutex_t *lock);


/*  Handlers for Apache module specific directives.  */
static const char  *vhc_set_debug(cmd_parms *parms, void *unused,
                                  int flag);
static const char  *vhc_set_error_code(cmd_parms *parms, void *unused,
                                       const char *arg);
static const char  *vhc_set_error_message(cmd_parms *parms, void *unused,
                                          const char *arg);
static const char  *vhc_set_slot_limit(cmd_parms *parms, void *unused,
                                       const char *arg);
static const char  *vhc_set_burst_percent(cmd_parms *parms, void *unused,
                                          const char *arg);
static const char  *vhc_set_grace_period(cmd_parms *parms, void *unused,
                                         const char *arg);
static const char  *vhc_set_burst_flap_period(cmd_parms *parms,
                                              void *unused,
                                              const char *arg);

/*  Callbacks - hooks into Apache server/request lifecycle.  */
static apr_status_t  vhc_req_pool_cleanup_(void *arg);

static void  *vhc_create_server_config(apr_pool_t *pool, server_rec *srvr);
static int    vhc_post_config(apr_pool_t *pool, apr_pool_t *plog,
                              apr_pool_t *ptemp, server_rec *srvr);
static void  vhc_child_init(apr_pool_t *pool, server_rec *srvr);
static int   vhc_post_read_request(request_rec *req);
static int   vhc_handler(request_rec *req);
static void  vhc_register_hooks(apr_pool_t *pool);


/*  }}}  -- End section:forward-decs.  */



/*  section:internal-functions {{{  */
/*  ++++++++++++++++++++++++++      */

/*  ===================================================================  */
/*  @@@@@  Internal Helper Functions.                                    */
/*  ===================================================================  */


/**
 *   @brief   Initialize VHC process environment.
 *   @param   pool      memory pool
 *
 *   Initialize VHC process environment.
 *
 */
static void  vhc_initialize_process_env_(apr_pool_t *pool) {
   apr_status_t   status;

   /*  Get my process id.  */
   gs_mypid = getpid();

   /*  Link gs_debug_file to stderr.  */
   status = apr_file_open_stderr(&gs_debug_file, pool);
   if (!VHC_APR_STATUS_IS_SUCCESS(status) ) {
      gs_debug_file = NULL;
      ap_log_perror(APLOG_MARK, APLOG_ERR, status, pool,
                    "%s: Error opening stderr as apr file",
                    VHC_MODULE_NAME);
   }

}  /*  End of function  vhc_initialize_process_env_.  */



/**
 *   @brief   Log debug message.
 *   @param   pool  memory pool (optional - pass NULL if not available)
 *   @param   fmt   printf style format
 *   @param   ...   variable args
 *
 *   Log debugging message.
 *
 */
static void  vhc_debug_log_(apr_pool_t *pool, char *fmt, ...) {
   va_list        ap;
   apr_status_t   status;
   char           buffer[VHC_MAX_DEBUG_MESSAGE_LEN];
   char           dtstr[APR_CTIME_LEN + 1];
   char          *msg;

   /*  Do nothing - debug is OFF or no debug file to log to.  */
   if ((VHC_FALSE == gs_vhc_env_settings.debug)  ||
       (NULL == gs_debug_file) )
      return;


   /*  This function is not really portable -- just for debugging.  */
   /*  Handle the variable args.  */
   va_start(ap, fmt);

   if (NULL == pool) {
      vsprintf(buffer, fmt, ap);
      msg = buffer;
   } 
   else
      msg = apr_pvsprintf(pool, fmt, ap);

   /*  Ignore error cases -- this is just for debug messages.  */
   status = apr_ctime(dtstr, apr_time_now() );
   status = apr_file_printf(gs_debug_file, "[%s] [vhcdebug] %s: %d - %s\n",
                                           dtstr, VHC_MODULE_NAME,
                                           gs_mypid, msg);
   if (VHC_APR_STATUS_IS_SUCCESS(status) )
      status = apr_file_flush(gs_debug_file);

   /*  End handling variable args.  */
   va_end(ap);

}  /*  End of function  vhc_debug_log_.  */



/**
 *   @brief   Returns the name of the vhost for a request.
 *   @param   srvr  server record
 *   @return  a scrubbed (null checked string) vhost name.
 *
 *   Returns the name of the vhost for a request.
 *
 */
static char  *vhc_get_vhost_name_(server_rec *srvr) {
   static char  *nullhost = "<null>";

   if ((NULL == srvr)  ||  (NULL == srvr->server_hostname) )
      return nullhost;

   /*  Valid vhost name - return it back.  */
   return srvr->server_hostname;

}  /*  End of function  vhc_get_vhost_name_.  */



/**
 *   @brief   Generate HTTP headers for the choked page (response).
 *   @param   req   request record
 *
 *   Generate HTTP headers for the choked page (response) that we send
 *   back to the client.
 *
 */
static void  vhc_generate_choked_headers_(request_rec  *req,
                                          apr_uint16_t  http_code) {

   apr_pool_t  *pool = req->pool;
   char        *status_line;
   char        *x_throttled_by;

   /*  Build the status line.  */
   status_line = apr_psprintf(pool, "%d %s", http_code,
                                    VHC_HTTP_MSG_TOO_MANY_REQUESTS);

   /*  Set content type, status and status line.  */
   ap_set_content_type(req, VHC_CHOKED_RESPONSE_CONTENT_TYPE);
   req->status      = http_code;
   req->status_line = status_line;

   /*  Build the X-Throttled-By extension header.  */
   x_throttled_by = apr_psprintf(pool, "%s/%s", VHC_MODULE_BASEPRODUCT,
                                       VHC_MODULE_VERSION);

   /*  Set the extension header.  */
   apr_table_set(req->headers_out, VHC_X_THROTTLED_BY_HEADER_NAME,
                                   x_throttled_by);

}  /*  End of function  vhc_generate_choked_headers_.  */



/**
 *   @brief   Generate the choked page content to return back to client.
 *   @param   req     request record
 *   @param   title   custom page title
 *   @param   msg     custom message
 *
 *   Generate the choked page content to return back to client.
 *
 */
static void  vhc_generate_choked_page_content_(request_rec *req,
                                               const char  *title,
                                               const char  *msg) {

   /*  Add doctype.  */
   ap_rputs(DOCTYPE_HTML_3_2, req);

   /*  Add heading - page title and body content.  */
   ap_rprintf(req, "<HTML>\n  <HEAD><TITLE>%s: %s</TITLE></HEAD>\n",
                   VHC_MODULE_BASEPRODUCT,
                   title ? title : VHC_HTTP_MSG_TOO_MANY_REQUESTS);
   ap_rprintf(req, "  <BODY><BR/><H3>%s: %s</H3><BR/>%s<BR/></BODY>\n",
                   VHC_MODULE_BASEPRODUCT,
                   title ? title : VHC_HTTP_MSG_TOO_MANY_REQUESTS,
                   msg ? msg : VHC_DEFAULT_ERROR_MSG);
   ap_rprintf(req, "</HTML>\n");

}  /*  End of function  vhc_generate_choked_page_content_.  */



/**
 *   @brief   Generate a temporary file name using the specified template.
 *   @param   pool      memory pool
 *   @param   template  file name template.
 *   @return  Name of generated temporary file name.
 *
 *   Generate a temporary file name using the specified template.
 *
 */
static char  *vhc_mktemp_(apr_pool_t *pool, const char *template) {
   const char    *default_temp_dir = VHC_DEFAULT_TEMP_DIR;
   apr_status_t   status;
   char          *tempdir;
   char          *fname;

   /*  Get temporary directory from the apr routines.  */
   status = apr_temp_dir_get((const char **) &tempdir, pool);
   if (!VHC_APR_STATUS_IS_SUCCESS(status) )
      tempdir = (char *) default_temp_dir;  /*  Fallback: Use /tmp.  */

   /*  Generate and return a temporary file using our pid.  */
   fname = apr_psprintf(pool, "%s/.%s-%s.%ld", tempdir, VHC_MODULE_NAME,
                              template, (long int) gs_mypid);

   VHC_DEBUG  vhc_debug_log_(pool, "%s: Generated tempfile '%s'",
                                   VHC_LOC, fname);

   return fname;

}  /*  End of function  vhc_mktemp_.  */



/**
 *   @brief   Create a global lock to enable exclusive access to the shm.
 *   @param   pool  memory pool
 *   @return  APR_SUCCESS on success, otherwise HTTP_INTERNAL_SERVER_ERROR
 *            if lock creation/setting permissions failed.
 *
 *   Create a global lock to enable exclusive access to the shared memory
 *   segment we use for storing counters + grace expiry time.
 *
 */
static int  vhc_create_global_lock_(apr_pool_t *pool) {
   apr_status_t  status;

   VHC_DEBUG  vhc_debug_log_(pool, "%s: Generate lock file name ...",
                                   VHC_LOC);

   /*
    *  Generate a temporary lock file using our pid. Its a global so that
    *  we can use it across the servers (children inherit this).
    */
   gs_shm_lockfile = vhc_mktemp_(pool, "lock");

   /*
    *  Create a global lock using the apr routines.
    *  Note: Depending on the OS + lock type, the lockfile may or may not
    *        be created/used.
    */
   status = apr_global_mutex_create(&gs_shm_lock,
                                    (const char *) gs_shm_lockfile,
                                    APR_LOCK_DEFAULT, pool);
   VHC_DEBUG  vhc_debug_log_(pool, "%s: Global lock create returned %d", 
                                   VHC_LOC, status);
   if (!VHC_APR_STATUS_IS_SUCCESS(status) ) {
      ap_log_perror(APLOG_MARK, APLOG_ERR, status, pool,
                    "%s: Failed to create global lock - file=%s",
                    VHC_MODULE_NAME, gs_shm_lockfile);
      return HTTP_INTERNAL_SERVER_ERROR;
   }

#ifdef AP_NEED_TO_SET_MUTEX_PERMS
   status = unixd_set_global_mutex_perms(gs_shm_lock);
   VHC_DEBUG  vhc_debug_log_(pool, "%s: unixd set lock perms returned %d", 
                                   VHC_LOC, status);
   if (!VHC_APR_STATUS_IS_SUCCESS(status) ) {
      ap_log_perror(APLOG_MARK, APLOG_ERR, status, pool,
                    "%s: Failed to set global lock perms - file=%s",
                    VHC_MODULE_NAME, gs_shm_lockfile);
      return HTTP_INTERNAL_SERVER_ERROR;
   }

#endif  /*  For  AP_NEED_TO_SET_MUTEX_PERMS.  */


   /*  All's well - return success.  */
   VHC_DEBUG  vhc_debug_log_(pool, "%s: Global lock created OK", VHC_LOC);

   return APR_SUCCESS;

}  /*  End of function  vhc_create_global_lock_.  */



/**
 *   @brief   Create a shared memory segment to use for storing vhost
 *            slot counters + grace expiry time.
 *   @param   pool      memory pool
 *   @param   shm       shared memory segment to return back
 *   @param   template  file name template.
 *   @param   shmfile   shared memory file name to return back
 *   @return  APR_SUCCESS on success, otherwise HTTP_INTERNAL_SERVER_ERROR
 *            if shm creation failed.
 *
 *   Create a shared memory segment to use for storing vhost slot counters
 *   and grace expiry time.
 *
 */
static int  vhc_create_shm_segment_(apr_pool_t *pool, apr_shm_t **shm,
                                    const char *template, char *shmfile) {
   apr_status_t     status;
   apr_size_t       shm_size;
   apr_size_t       alloc_size;
   VHC_shm_data_t  *baseaddr;

   VHC_DEBUG  vhc_debug_log_(pool, "%s: Generate %s file name ...",
                                   VHC_LOC, template);

   /*  Generate a temporary lock file using our pid.  */
   shmfile = vhc_mktemp_(pool, template);

   shm_size = (apr_size_t) (gs_num_configs * sizeof(VHC_server_config_t) );
   VHC_DEBUG  vhc_debug_log_(pool, "%s: need shm size %ld for #%d configs",
                                   VHC_LOC, (long int) shm_size,
                                   gs_num_configs);

   /*  First check if the shm segment exists and try cleaning it up.  */
   status = apr_shm_attach(shm, (const char *) shmfile, pool);
   VHC_DEBUG  vhc_debug_log_(pool, "%s: OLD shm attach check returned %d",
                                   VHC_LOC, status);

   if (VHC_APR_STATUS_IS_SUCCESS(status) ) {
      /*  Have an shm segment (since attach worked).  */
      ap_log_perror(APLOG_MARK, APLOG_WARNING, status, pool,
                    "%s: existing/old shm - file=%s, destroying it",
                    VHC_MODULE_NAME, shmfile);

      status = apr_shm_destroy(*shm);
      VHC_DEBUG  vhc_debug_log_(pool, "%s: OLD shm destroy returned %d",
                                      VHC_LOC, status);

      if (!VHC_APR_STATUS_IS_SUCCESS(status) )
         ap_log_perror(APLOG_MARK, APLOG_ERR, status, pool,
                       "%s: Failed to destroy existing/old shm",
                       VHC_MODULE_NAME);

   }  /*  If there's an existing/old shm segment.  */


   /*  Now create a shm segment using the apr routines.  */
   status = apr_shm_create(shm, shm_size, (const char *) shmfile, pool);
   VHC_DEBUG  vhc_debug_log_(pool, "%s: shm create returned %d",
                                   VHC_LOC, status);
   if (!VHC_APR_STATUS_IS_SUCCESS(status) ) {
      ap_log_perror(APLOG_MARK, APLOG_ERR, status, pool,
                    "%s: Failed to create shm segment - file=%s",
                    VHC_MODULE_NAME, shmfile);
      return HTTP_INTERNAL_SERVER_ERROR;
   }

   /*  Check if we got it all.  */
   alloc_size = apr_shm_size_get((const apr_shm_t *) *shm);
   VHC_DEBUG  vhc_debug_log_(pool, "%s: shm size %d (asked for %d)",
                                   VHC_LOC, alloc_size, shm_size);
   if (alloc_size != shm_size) {
      ap_log_perror(APLOG_MARK, APLOG_ERR, 0, pool,
                    "%s: Error allocating shm - size mismatch: %ld != %ld",
                    VHC_MODULE_NAME, (long int) alloc_size,
                    (long int) shm_size);
      return HTTP_INTERNAL_SERVER_ERROR;
   }

   /*  Get base address and initialize the shm segment.  */
   baseaddr = apr_shm_baseaddr_get((const apr_shm_t *) *shm);
   VHC_DEBUG  vhc_debug_log_(pool, "%s: shm baseaddr %ld",
                                   VHC_LOC, baseaddr);
   if (NULL == baseaddr) {
      ap_log_perror(APLOG_MARK, APLOG_ERR, 0, pool,
                    "%s: Error getting shm segment base address",
                    VHC_MODULE_NAME);
      return HTTP_INTERNAL_SERVER_ERROR;
   }

   /*  Sad. bzero got deprecated - use memset to initialize shm.   */
   memset(baseaddr, 0, alloc_size); 

   /*  All's well - return success.  */
   VHC_DEBUG  vhc_debug_log_(pool, "%s: shm segment created OK", VHC_LOC);

   return APR_SUCCESS;

}  /*  End of function  vhc_create_shm_segment_.  */



/**
 *   @brief   Try and acquire the global lock within the specified timeout
 *            period (in microseconds).
 *   @param   lock           the lock
 *   @param   timeout_usecs  timeout
 *   @return  APR_SUCCESS on success, otherwise errors.
 *
 *   Try and acquire a global lock - timing out if we are unable to acquire
 *   it within the specified time.
 *
 */
static int  vhc_lock_acquire_(apr_global_mutex_t *lock,
                              int timeout_usecs) {

   apr_status_t  status;
   apr_time_t    start_time = apr_time_now();
   apr_time_t    end_time   = start_time + apr_time_usec(timeout_usecs);

   VHC_DEBUG  vhc_debug_log_(NULL, "%s: Acquiring lock", VHC_LOC);

   while (apr_time_now() < end_time) {
      status = apr_global_mutex_trylock(lock);
      if (VHC_APR_STATUS_IS_SUCCESS(status) )
         return status;
      else if (APR_STATUS_IS_ENOTIMPL(status) )
         return apr_global_mutex_lock(lock);  /*  No trylock support,  */
                                              /*  so just wait.        */

      /*  Need to retry - sleep in 5ms blocks.  */
      VHC_DEBUG  vhc_debug_log_(NULL, "%s: Retry lock acquire in %d usecs",
                                      VHC_LOC,
                                      VHC_TRYLOCK_SLEEP_TIME_USECS);
      apr_sleep(VHC_TRYLOCK_SLEEP_TIME_USECS);

   }  /*  End of  while not timed out.  */


   /*  Got here, means we timed out - so return ETIMEDOUT.  */
   VHC_DEBUG  vhc_debug_log_(NULL, "%s: Lock acquisition timed out",
                                   VHC_LOC);

   return ETIMEDOUT;

}  /*  End of function  vhc_lock_acquire_.  */



/**
 *   @brief   Release a previously acquired global lock.
 *   @param   lock           the lock
 *   @return  APR_SUCCESS on success, otherwise errors.
 *
 *   Release a previously acquired global lock.
 *
 */
static int  vhc_lock_release_(apr_global_mutex_t *lock) {

   VHC_DEBUG  vhc_debug_log_(NULL, "%s: Releasing lock", VHC_LOC);

   return  apr_global_mutex_unlock(lock); 

}  /*  End of function  vhc_lock_release_.  */



/**
  *   @brief   Check if the virtual host has capacity.
  *   @param   config   vhost config record
  *   @param   shmdata  vhost shm data
  *   @return  APR_SUCCESS if host has capacity, otherwise errors.
  *
  *   Check if a virtual host has capacity (slots available or can
  *   burst to free additional slots).
  *
  */
static int  vhc_check_vhost_capacity_(VHC_server_config_t *config,
                                      VHC_shm_data_t *shmdata) {

   apr_time_t    current_time;
   apr_time_t    flap_secs;
   apr_time_t    grace_secs;
   apr_uint16_t  burst_slots;
   apr_uint16_t  free_slots;
   apr_time_t    grace_expiry_time;


   VHC_DEBUG  vhc_debug_log_(NULL, "%s: check vhost capacity", VHC_LOC);

   /*  Check that we have enough slots for this vhost.  */
   if (shmdata->inuse_slots < config->slot_limit)
      return APR_SUCCESS;  /*  Optimized case all's good.  */

   /*  Ok - at or beyond slot limit - check if within grace period.  */
   burst_slots = ceil(config->slot_limit *
                      config->burst_settings.percent / 100.0);
   free_slots  = config->slot_limit + ((apr_uint16_t) burst_slots) -
                 shmdata->inuse_slots;
   if (free_slots <= 0)
      return VHC_HTTP_TOO_MANY_REQUESTS;  /*  Choked!!  */


   /*  Find out when the grace time expires.  */
   current_time = apr_time_now();
   grace_secs = apr_time_from_sec(config->burst_settings.grace_period);
   grace_expiry_time = current_time + grace_secs;
   if (shmdata->grace_expires_at >= 0) {
      /*  Check if grace expires at is past the flap expiry period.  */
      if (shmdata->grace_expires_at > current_time)
         grace_expiry_time = shmdata->grace_expires_at;
      else {
         /*  Use grace_expires_at if in the flap expiry period.  */
         flap_secs = apr_time_from_sec(config->burst_settings.flap_period);
         if ((shmdata->grace_expires_at + flap_secs) > current_time)
            grace_expiry_time = shmdata->grace_expires_at;
      }

   }  /*  End of if  grace_expires_at is already set.  */


   /*  And now check if we are within the grace period.  */
   if (grace_expiry_time > current_time)
      return APR_SUCCESS;  /*  Still within the grace period.  */


   /*  If we got here, we are all choked up.  */
   return VHC_HTTP_TOO_MANY_REQUESTS;  /*  Throttle this vhost.  */

}  /*  End of function  vhc_check_vhost_capacity_.  */


/*  }}}  -- End section:internal-functions.  */



/*  section:ap-directive-handlers {{{  */
/*  +++++++++++++++++++++++++++++      */

/*  ===================================================================  */
/*  @@@@@  Apache Config Directive Handlers.                             */
/*  ===================================================================  */

/*  A: Setter functions (settings for the global/entire env).  */
/*  ---------------------------------------------------------  */

/**
 *   @brief   Turn on/off writing debug messages.
 *   @param   cmd_parms  command parameters 
 *   @param   unused     unused (module config)
 *   @param   arg        directive value
 *   @return  always NULL.
 *
 *   Turn ON/OFF writing debug messages.
 *
 */
static const char  *vhc_set_debug(cmd_parms *parms, void *unused,
                                  int flag) {

   gs_vhc_env_settings.debug = flag ? VHC_TRUE : VHC_FALSE;

   VHC_DEBUG  vhc_debug_log_(NULL, "%s: Env.Debug = %d",
                                   VHC_LOC, gs_vhc_env_settings.debug);

   return NULL;

}  /*  End of function  vhc_set_debug.  */



/**
 *   @brief   Set HTTP error code to return back to clients when vhosts
 *            are "choked".
 *   @param   cmd_parms  command parameters 
 *   @param   unused     unused (module config)
 *   @param   arg        directive value
 *   @return  always NULL.
 *
 *   Set the HTTP response code (error code) to return back to clients
 *   when we throttle vhosts.
 *
 */
static const char  *vhc_set_error_code(cmd_parms *parms, void *unused,
                                       const char *arg) {

   apr_int64_t http_code = (apr_uint64_t) apr_atoi64(arg);

   if (http_code >= 0)
      gs_vhc_env_settings.http_code = (apr_uint16_t) http_code;


   VHC_DEBUG  vhc_debug_log_(NULL, "%s: Env.http_code = %d",
                                   VHC_LOC, gs_vhc_env_settings.http_code);

   return NULL;

}  /*  End of function  vhc_set_error_code.  */



/**
 *   @brief   Set the customized error message to return back to clients
 *            when vhosts are "choked".
 *   @param   cmd_parms  command parameters 
 *   @param   unused     unused (module config)
 *   @param   arg        directive value
 *   @return  always NULL.
 *
 *   Set the customized error message to send back to clients when we
 *   throttle vhosts.
 *
 */
static const char  *vhc_set_error_message(cmd_parms *parms, void *unused,
                                          const char *arg) {

   if ((arg != NULL)  &&  (strlen(arg) > 0) )
      apr_cpystrn(gs_vhc_env_settings.err_message, arg,
                  VHC_MAX_ERROR_MESSAGE_LEN);


   VHC_DEBUG  vhc_debug_log_(NULL, "%s: Env.err_message = '%s'",
                                   VHC_LOC,
                                   gs_vhc_env_settings.err_message);

   return NULL;

}  /*  End of function  vhc_set_error_message.  */



/*  B: Setter functions for individual vhosts.  */
/*  ------------------------------------------  */

/**
 *   @brief   Set the concurrent request [slot] limit for a vhost.
 *   @param   cmd_parms  command parameters 
 *   @param   unused     unused (module config)
 *   @param   arg        directive value
 *   @return  always NULL.
 *
 *   Set the slot limit or max number of concurrent requests allowed for
 *   a specific 'server' (vhost).
 *
 */
static const char  *vhc_set_slot_limit(cmd_parms *parms, void *unused,
                                       const char *arg) {

   /*  Get the 'server' record to operate on.  */
   server_rec  *s = parms->server;

   /*  Get the vhc config for the vhost and set its "choke" limit.  */
   VHC_server_config_t *cfg = (VHC_server_config_t *)
          ap_get_module_config(s->module_config, &vhost_choke_module);

   apr_int64_t  nslots = apr_atoi64(arg);
   if ((nslots >= 0)  &&  (nslots <= VHC_MAX_SLOT_LIMIT) )
      cfg->slot_limit = (apr_uint16_t) nslots;


   VHC_DEBUG  vhc_debug_log_(NULL, "%s: %s->slot_limit = %d",
                                   VHC_LOC, vhc_get_vhost_name_(s),
                                   cfg->slot_limit);

   return NULL;

}  /*  End of function  vhc_set_slot_limit.  */



/**
 *   @brief   Set the percentage (of the vhost slot limit) that we will 
 *            allow requests burst upto for a vhost when the slot limit is
 *            reached.
 *   @param   cmd_parms  command parameters 
 *   @param   unused     unused (module config)
 *   @param   arg        directive value
 *   @return  always NULL.
 *
 *   Set the percentage (of the vhost slot limit) that we will allow
 *   a vhost to burst beyond the slot limit. Note we will allow this 
 *   percentage of additional requests for the entire grace period.
 *
 *   Example: If the slot limit for a vhost is 10 and the burst
 *            percentage is 15, then we will allow for an additional 2
 *            requests (ceil of 1.5 or 15*10/100) for the entire grace
 *            period, meaning the slot limit is capped at 12 for that grace
 *            period.
 *
 */
static const char  *vhc_set_burst_percent(cmd_parms *parms, void *unused,
                                          const char *arg) {

   /*  Get the 'server' record to operate on.  */
   server_rec  *s = parms->server;

   /*  Get the vhc config for the vhost and set its "burst" percent.  */
   VHC_server_config_t *cfg = (VHC_server_config_t *)
          ap_get_module_config(s->module_config, &vhost_choke_module);

   apr_int64_t  percent = apr_atoi64(arg);
   if (percent >= 0)
      cfg->burst_settings.percent = (apr_uint16_t) percent;


   VHC_DEBUG  vhc_debug_log_(NULL, "%s: %s->burst.percent = %d",
                                   VHC_LOC, vhc_get_vhost_name_(s),
                                   cfg->burst_settings.percent);

   return NULL;

}  /*  End of function  vhc_set_burst_percent.  */



/**
 *   @brief   Set the grace period that we will allow requests bursting
 *            for a vhost.
 *   @param   cmd_parms  command parameters 
 *   @param   unused     unused (module config)
 *   @param   arg        directive value
 *   @return  always NULL.
 *
 *   Set the grace period (in seconds) that we will allow request bursting
 *   for a vhost. The number of additional slots is governed by the burst
 *   percent setting.
 *
 *   Example: If the slot limit for a vhost is 10 and the burst
 *            percentage is 15, then we will allow for an additional 2
 *            requests (ceil of 1.5 or 15*10/100) for the entire grace
 *            period, meaning the slot limit is capped at 12 for that grace
 *            period.
 *
 */
static const char  *vhc_set_grace_period(cmd_parms *parms, void *unused,
                                         const char *arg) {

   /*  Get the 'server' record to operate on.  */
   server_rec  *s = parms->server;

   /*  Get the vhc config for the vhost and set its "grace" period.  */
   VHC_server_config_t *cfg = (VHC_server_config_t *)
          ap_get_module_config(s->module_config, &vhost_choke_module);

   apr_int64_t  nsecs = apr_atoi64(arg);
   if (nsecs >= 0) 
      cfg->burst_settings.grace_period = (apr_uint16_t) nsecs;


   VHC_DEBUG  vhc_debug_log_(NULL, "%s: %s->burst.grace_period = %d",
                                   VHC_LOC, vhc_get_vhost_name_(s),
                                   cfg->burst_settings.grace_period);

   return NULL;

}  /*  End of function  vhc_set_grace_period.  */



/**
 *   @brief   Set the flap period that controls when we will allow mxG
 *            for a vhost.
 *   @param   cmd_parms  command parameters 
 *   @param   unused     unused (module config)
 *   @param   arg        directive value
 *   @return  always NULL.
 *
 *   Set the grace period (in seconds) that we will allow request bursting
 *   for a vhost. The number of additional slots is governed by the burst
 *   percent setting.
 *
 *   Example: If the slot limit for a vhost is 10 and the burst
 *            percentage is 15, then we will allow for an additional 2
 *            requests (ceil of 1.5 or 15*10/100) for the entire grace
 *            period, meaning the slot limit is capped at 12 for that grace
 *            period.
 *
 */
static const char  *vhc_set_burst_flap_period(cmd_parms *parms,
                                              void *unused,
                                              const char *arg) {

   /*  Get the 'server' record to operate on.  */
   server_rec  *s = parms->server;

   /*  Get the vhc config for the vhost and set its flap period.  */
   VHC_server_config_t *cfg = (VHC_server_config_t *)
          ap_get_module_config(s->module_config, &vhost_choke_module);

   apr_int64_t  nsecs = apr_atoi64(arg);
   if (nsecs >= 0) 
      cfg->burst_settings.flap_period = (apr_uint16_t) nsecs;


   VHC_DEBUG  vhc_debug_log_(NULL, "%s: %s->burst.flap_period = %d",
                                   VHC_LOC, vhc_get_vhost_name_(s),
                                   cfg->burst_settings.flap_period);

   return NULL;

}  /*  End of function  vhc_set_burst_flap_period.  */

/*  }}}  -- End section:ap-directive-handlers.  */



/*  section:ap-callback-hooks {{{  */
/*  +++++++++++++++++++++++++      */

/*  ===================================================================  */
/*  @@@@@  Module callback functions.                                    */
/*  ===================================================================  */

/**
  *   @brief   Pseudo-hook into the end of the request -- when the
  *            pool cleanup happens.
  *   @param   arg  address of the request record.
  *   @return  APR_SUCCESS always.
  *
  *   Hook into the end of a request's lifecycle.
  *
  */
static apr_status_t  vhc_req_pool_cleanup_(void *arg) {

   request_rec          *req = (request_rec *) arg;
   apr_status_t          status;
   apr_pool_t           *pool = req->pool;
   VHC_server_config_t  *cfg;
   void                 *baseaddr;
   void                 *data;
   VHC_shm_data_t       *vhost_data;

   VHC_DEBUG  vhc_debug_log_(pool, "%s: CALLBACK req pool cleanup",
                                   VHC_LOC);

   cfg = ap_get_module_config(req->server->module_config,
                              &vhost_choke_module);
   
   VHC_DEBUG  vhc_debug_log_(pool, "%s: vhost = %s", VHC_LOC,
                                   vhc_get_vhost_name_(req->server) );

   /*  Check if we need to throttle this vhost.  */
   if (0 == cfg->slot_limit) {
      VHC_DEBUG  vhc_debug_log_(pool, "%s: DISABLED - no limit", VHC_LOC);
      return APR_SUCCESS;
   }

   /*  Ensure valid config id.  */
   if (cfg->config_id >= gs_num_configs) {
      VHC_DEBUG  vhc_debug_log_(pool, "%s: Config Id %d not valid (>= %d)",
                                      VHC_LOC, cfg->config_id,
                                      gs_num_configs);
      ap_log_error(APLOG_MARK, APLOG_ERR, 0, req->server,
                   "%s: invalid config id [%d] >= %d",
                   VHC_MODULE_NAME, cfg->config_id, gs_num_configs);
      return APR_SUCCESS;
   }


   /*  Acquire the lock.  */
   status = vhc_lock_acquire_(gs_shm_lock, VHC_MAX_LOCK_WAIT_TIME_USECS);
   VHC_DEBUG  vhc_debug_log_(pool, "%s: Lock acquistion status %d",
                                   VHC_LOC, status);
   if (!VHC_APR_STATUS_IS_SUCCESS(status) ) {
      /*  Got some error - need to log and bail out.  */
      ap_log_perror(APLOG_MARK, APLOG_ERR, status, pool,
                   "%s: pool cleanup failed to acquire shm lock - pid=%ld",
                   VHC_MODULE_NAME, (long int) getpid() );
      return APR_SUCCESS;
   }

   /*  Get shm segment base address.  */
   baseaddr = apr_shm_baseaddr_get(gs_shm);
   VHC_DEBUG  vhc_debug_log_(pool, "%s: shm addr %ld", VHC_LOC, baseaddr);

   if (NULL == baseaddr) {
      /*  Error getting shm segment base address.  */
      ap_log_error(APLOG_MARK, APLOG_ERR, 0, req->server,
                   "%s: pool cleanup error getting shm base address",
                   VHC_MODULE_NAME);

      /*  Important: Need to release the lock before returning.  */
      status = vhc_lock_release_(gs_shm_lock);
      return APR_SUCCESS;
   }

   /*  Jump to right place for the vhost data.  */
   data = baseaddr + (sizeof(VHC_shm_data_t) * cfg->config_id);
   vhost_data = (VHC_shm_data_t *) data;

   /*  Reduce the number of inuse slots if not choked.  */
   if (!apr_table_get(req->headers_out, VHC_X_THROTTLED_BY_HEADER_NAME)  &&
       (vhost_data->inuse_slots > 0) )
      vhost_data->inuse_slots--;

   VHC_DEBUG  vhc_debug_log_(pool, "%s: vhost usage = %d slots",
                                   VHC_LOC, vhost_data->inuse_slots);

   status = vhc_lock_release_(gs_shm_lock);

   return APR_SUCCESS;

}  /*  End of function  vhc_req_pool_cleanup_.  */



/**
 *   @brief   Create per-server configuration.
 *   @param   pool  memory pool
 *   @param   srvr  server record
 *
 *   Create a per-server configuration.
 *   Note: this is also called for the "default" server.
 *
 */
static void  *vhc_create_server_config(apr_pool_t *pool,
                                       server_rec *srvr) {
   VHC_server_config_t  *cfg;
   
   VHC_DEBUG  vhc_debug_log_(pool, "%s: CALLBACK - server cfg", VHC_LOC);
   VHC_DEBUG  vhc_debug_log_(pool, "%s: vhost = %s", VHC_LOC,
                                   vhc_get_vhost_name_(srvr) );

   /*  Allocate a per-server configuration record.  */
   cfg = apr_pcalloc(pool, sizeof(VHC_server_config_t) );

   /*  Intialize counters + settings.  */
   /*  memset(cfg, 0, sizeof(VHC_server_config_t) );  */

   cfg->config_id = gs_num_configs++;
   VHC_DEBUG  vhc_debug_log_(pool, "%s: num_configs=%d", VHC_LOC,
                                   gs_num_configs);

   /*  Note: default choke limit is 0 === no limit.  */
   cfg->slot_limit = VHC_DEFAULT_SLOT_LIMIT;

   cfg->burst_settings.grace_period = VHC_DEFAULT_GRACE_PERIOD;
   cfg->burst_settings.flap_period  = VHC_DEFAULT_BURST_FLAP_PERIOD;
   cfg->burst_settings.percent      = VHC_DEFAULT_BURST_PERCENT;
 
   return (void *) cfg;

}  /*  End of function  vhc_create_server_config.  */



/**
 *   @brief   Post configuration callback called in the parent process -
 *            we setup the shared memory segment + global lock in here.
 *   @param   pool   memory pool
 *   @param   plog   log memory pool
 *   @param   ptemp  temporary memory pool
 *   @param   srvr   server record
 *   @return  APR_SUCCESS on success, otherwise failure.
 *
 *   Post configuration callback called in the parent process -
 *   setup the shared memory segment + global lock.
 *
 */
static int  vhc_post_config(apr_pool_t *pool, apr_pool_t *plog,
                              apr_pool_t *ptemp, server_rec *srvr) {

   const char      *key = VHC_MODULE_BASEPRODUCT;
   void            *userdata;
   apr_status_t     status;
   
   VHC_DEBUG  vhc_debug_log_(pool, "%s: CALLBACK - post config", VHC_LOC);
   VHC_DEBUG  vhc_debug_log_(pool, "%s: vhost = %s", VHC_LOC,
                                   vhc_get_vhost_name_(srvr) );

   /*
    *  Use a custom key to check that the initialization just once.
    *  This is because this routine is called a couple of times when the
    *  parent process gets initialized (on server startup) and we don't
    *  want a double "dose" of the shm segment + lock - pthread_once!!
    */
   VHC_DEBUG  vhc_debug_log_(pool, "%s: Checking key existence", VHC_LOC);

   apr_pool_userdata_get(&userdata, key, srvr->process->pool);
   if (!userdata) {
      VHC_DEBUG  vhc_debug_log_(pool, "%s: Setting key '%s'",
                                      VHC_LOC, key);
      apr_pool_userdata_set((const void *) VHC_TRUE, key,
                            apr_pool_cleanup_null, srvr->process->pool);
      return APR_SUCCESS;
   }

   /*  Create global lock.  */
   VHC_DEBUG  vhc_debug_log_(pool, "%s: Create global lock ...", VHC_LOC);

   status = vhc_create_global_lock_(pool);
   if (!VHC_APR_STATUS_IS_SUCCESS(status) )
      return status;

   /*
    *  Okay, all setup - just create the shm and return. The shm segment 
    *  and shm file are global so as to allow the children to inherit it.
    */
   VHC_DEBUG  vhc_debug_log_(pool, "%s: Create shm segment ...", VHC_LOC);
   return  vhc_create_shm_segment_(pool, &gs_shm, "gshm", gs_shm_file);

}  /*  End of function  vhc_post_config.  */


/**
  *   @brief   Child initialization callback - on child process startup.
  *   @param   pool   memory pool
  *   @param   plog   log memory pool
  *   @param   ptemp  temporary memory pool
  *   @param   srvr   server record
  *
  *   Child initialization callback - need to reopen the global lock.
  *
  */
static void  vhc_child_init(apr_pool_t *pool, server_rec *srvr) {

   apr_status_t  status;

   VHC_DEBUG  vhc_debug_log_(pool, "%s: Initializing child ...", VHC_LOC);

   /*  Re-initialize our environment in the child process.  */
   vhc_initialize_process_env_(pool);

   /*  Reopen the mutex in the child - reusing the lock pointer global.  */
   VHC_DEBUG  vhc_debug_log_(pool, "%s: Reopen global lock ...", VHC_LOC);
   status = apr_global_mutex_child_init(&gs_shm_lock,
                                        (const char *) gs_shm_lockfile,
                                        pool);
   VHC_DEBUG  vhc_debug_log_(pool, "%s: global lock init status %d",
                                   VHC_LOC, status);
   if (!VHC_APR_STATUS_IS_SUCCESS(status) ) {
      /*  Log error + exit as child initialization failed.  */
      ap_log_error(APLOG_MARK, APLOG_CRIT, status, srvr,
                   "%s: Failed to reopen shm lock in child - file=%s",
                   VHC_MODULE_NAME, gs_shm_lockfile);
      exit(EXIT_FAILURE);
   }
      
   VHC_DEBUG  vhc_debug_log_(pool, "%s: Child initialization OK", VHC_LOC);

}  /*  End of function  vhc_child_init.  */



/**
  *   @brief   Hook into after request is read - so that we can trap into
  *            when the request ends.
  *   @param   req  request record
  *   @return  DECLINED always.
  *
  *   Hook into after request is read, so that we can get a pseudo hook
  *   into when the request ends via a pool cleanup handler.
  *
  */
static int  vhc_post_read_request(request_rec *req) {

   apr_pool_t           *pool = req->pool;
   struct request_rec  **clone;

   VHC_DEBUG  vhc_debug_log_(pool, "%s: CALLBACK create request", VHC_LOC);
   VHC_DEBUG  vhc_debug_log_(pool, "%s: vhost = %s", VHC_LOC,
                                   vhc_get_vhost_name_(req->server) );

   /*  Dummy allocation - so that cleanups happen.  */
   clone = apr_palloc(pool, sizeof(struct request_rec *) );
   *clone = req;

   /*  Register cleanup handler.  */
   apr_pool_cleanup_register(pool, *clone, vhc_req_pool_cleanup_,
                             apr_pool_cleanup_null);

   return DECLINED;

}  /*  End of function  vhc_post_read_request.  */



/**
  *   @brief   Request content handler callback - we check here as to
  *            whether or not to choke requests to the vhost.
  *   @param   req  request record
  *   @return  APR_SUCCESS if we choked the request, DECLINED if there's
  *            enough slots or within the burst grace period,
  *            HTTP_* if an error occurred and needs to be reported
  *
  *   Request content handler callback - check whether or not to choke
  *   requests to the vhost.
  *
  */
static int  vhc_handler(request_rec *req) {

   apr_status_t          status;
   apr_pool_t           *pool = req->pool;
   VHC_server_config_t  *cfg;
   void                 *baseaddr;
   void                 *data;
   VHC_shm_data_t       *vhost_data;
   apr_uint16_t          nslots;
   char                  burst_grace[] = "(burst grace period)";

   VHC_DEBUG  vhc_debug_log_(pool, "%s: CALLBACK handler", VHC_LOC);
   VHC_DEBUG  vhc_debug_log_(pool, "%s: vhost = %s", VHC_LOC,
                                   vhc_get_vhost_name_(req->server) );

   cfg = ap_get_module_config(req->server->module_config,
                              &vhost_choke_module);
   
   /*  Check if we need to throttle this vhost.  */
   if (0 == cfg->slot_limit) {
      VHC_DEBUG  vhc_debug_log_(pool, "%s: NOT throttled slot limit=%d", 
                                      VHC_LOC, cfg->slot_limit);
      return DECLINED;
   }

   /*  Ensure valid config id.  */
   if (cfg->config_id >= gs_num_configs) {
      VHC_DEBUG  vhc_debug_log_(pool, "%s: Config Id %d not valid (>= %d)",
                                      VHC_LOC, cfg->config_id,
                                      gs_num_configs);
      ap_log_error(APLOG_MARK, APLOG_ERR, 0, req->server,
                   "%s: invalid config id [%d] >= %d",
                   VHC_MODULE_NAME, cfg->config_id, gs_num_configs);
      return HTTP_INTERNAL_SERVER_ERROR;
   }


   /*  Acquire the lock.  */
   status = vhc_lock_acquire_(gs_shm_lock, VHC_MAX_LOCK_WAIT_TIME_USECS);
   VHC_DEBUG  vhc_debug_log_(pool, "%s: Lock acquistion status %d",
                                   VHC_LOC, status);
   if (!VHC_APR_STATUS_IS_SUCCESS(status) ) {
      /*  Got some error - need to log and bail out.  */
      ap_log_error(APLOG_MARK, APLOG_ERR, status, req->server,
                   "%s: vhc_handler failed to acquire shm lock - pid=%ld",
                   VHC_MODULE_NAME, (long int) getpid() );
      return HTTP_INTERNAL_SERVER_ERROR;
   }

   /*  Get shm segment base address.  */
   baseaddr = apr_shm_baseaddr_get(gs_shm);
   VHC_DEBUG  vhc_debug_log_(pool, "%s: shm addr %ld", VHC_LOC, baseaddr);

   if (NULL == baseaddr) {
      /*  Error getting shm segment base address.  */
      ap_log_error(APLOG_MARK, APLOG_ERR, 0, req->server,
                   "%s: vhc_handler error getting shm base address",
                   VHC_MODULE_NAME);

      /*  Important: Need to release the lock before returning.  */
      status = vhc_lock_release_(gs_shm_lock);
      return HTTP_INTERNAL_SERVER_ERROR;
   }

   /*  Jump to right place for the vhost data.  */
   data = baseaddr + (sizeof(VHC_shm_data_t) * cfg->config_id);
   vhost_data = (VHC_shm_data_t *) data;

   /*  Check vhost has capacity.  */
   status = vhc_check_vhost_capacity_(cfg, vhost_data);
   if (VHC_APR_STATUS_IS_SUCCESS(status) ) {
      /*  We have enough slots for this vhost.  */
      vhost_data->inuse_slots++;

      nslots = cfg->slot_limit;
      if (vhost_data->inuse_slots <= nslots)
         burst_grace[0] = '\0';  /*  Within slot limit.  */
      else  /*  Bursting ... */
         nslots += (apr_uint16_t) (ceil(cfg->slot_limit / 100.0 *
                                        cfg->burst_settings.percent) );

      VHC_DEBUG  vhc_debug_log_(pool, "%s: vhost has capacity %d/%d %s",
                                      VHC_LOC, vhost_data->inuse_slots,
                                      nslots, burst_grace);
      status = DECLINED;

   }  /*  End of  IF we had enough slots.  */


   /*  Save off number of inuse slots for lockless access later.  */
   nslots = vhost_data->inuse_slots;

   /*  Release the previously acquired lock.  */
   vhc_lock_release_(gs_shm_lock);


   /*  DECLINED means the vhost has capacity, so just return it.  */
   if (DECLINED == status)
      return DECLINED; 

   /*  Got here, means we have no slots, return choked page.  */

   /*  Set response headers - content type, status & extension headers.  */
   vhc_generate_choked_headers_(req, gs_vhc_env_settings.http_code);

   /*  !HEAD request, so generate choked page content as well.  */
   if (!req->header_only) {
      /*  TODO: add a link to the choked page w/ the error message.  */

      /*  Send an error page w/ the customized "choked" message.     */
      vhc_generate_choked_page_content_(req,
                                        VHC_HTTP_MSG_TOO_MANY_REQUESTS,
                                        gs_vhc_env_settings.err_message);
   }


   /*  Log error if dev debugging - don't overload log on high load.  */
#ifdef VHC_DEV_DEBUG
   ap_log_error(APLOG_MARK, APLOG_ERR, 0, req->server,
                "%s: vhost %s choked - %s, inuse_slots = %d",
                   VHC_MODULE_NAME, vhc_get_vhost_name_(req->server),
                   VHC_HTTP_MSG_TOO_MANY_REQUESTS, nslots);

#endif  /*  For VHC_DEV_DEBUG.  */

   /*  Log that we returned the customized "choked" http error code.  */
   VHC_DEBUG  vhc_debug_log_(pool, "%s: vhost choked response status=%d",
                                   VHC_LOC, gs_vhc_env_settings.http_code);

   return APR_SUCCESS;
      
}  /*  End of function  vhc_handler.  */



/**
 *   @brief   Register functions to handle the specific hooks 
 *   @param   pool   memory pool
 *
 *   Registers the hooks need to throttle vhost requests. 
 */
static void  vhc_register_hooks(apr_pool_t *pool) {

   VHC_DEBUG  vhc_debug_log_(pool, "%s: CALLBACK - reg hooks", VHC_LOC);

   /*  Initialize our environment.  */
   vhc_initialize_process_env_(pool);

   /*
    *  Register the callbacks we need to hook into:
    *     - post_config:        do this module's initialization  and
    *     - child_init:         when a child starts up. 
    *     - post_read_request:  after request is read
    *     - handler:            do the real "choke" work.
    */
   ap_hook_post_config(vhc_post_config, NULL, NULL, APR_HOOK_MIDDLE);
   ap_hook_child_init(vhc_child_init, NULL, NULL, APR_HOOK_MIDDLE);
   ap_hook_post_read_request(vhc_post_read_request, NULL, NULL,
                             APR_HOOK_MIDDLE);
   ap_hook_handler(vhc_handler, NULL, NULL, APR_HOOK_REALLY_FIRST);

   VHC_DEBUG  vhc_debug_log_(pool, "%s: registered %s OK",
                                   VHC_LOC,
                                   "post_config+child_init+handler");

}  /*  End of function  vhc_register_hooks.  */

/*  }}}  -- End section:ap-callback-hooks.  */



/*  section:ap-module-globals {{{  */
/*  +++++++++++++++++++++++++      */

/*  List of mod_vhost_choke directives.  */
static const command_rec vhc_cmds[] = {
   AP_INIT_FLAG(
      "VHostChokeDebug",              /*  Directive name               */
      vhc_set_debug,                  /*  Config action routine        */
      NULL,                           /*  Argument to include in call  */
      RSRC_CONF,                      /*  Where available (*.conf)     */
      "Use On or Off for debugging (default is Off). Debug messages are "
      "written to stderr"
                                      /*  Directive description        */
   ),

   AP_INIT_TAKE1(
      "VHostChokeErrorCode",          /*  Directive name               */
      vhc_set_error_code,             /*  Config action routine        */
      NULL,                           /*  Argument to include in call  */
      RSRC_CONF,                      /*  Where available (*.conf)     */
      "Customized HTTP code to use when responding to choked requests"
                                      /*  Directive description        */
   ),

   AP_INIT_TAKE1(
      "VHostChokeErrorMessage",       /*  Directive name            */
      vhc_set_error_message,          /*  Config action routine        */
      NULL,                           /*  Argument to include in call  */
      RSRC_CONF,                      /*  Where available (*.conf)     */
      "Customized error message (140 character limit) to use when "
      "responding to choked requests"
                                      /*  Directive description        */
   ),

   AP_INIT_TAKE1(
      "VHostChokeSlotLimit",          /*  Directive name               */
      vhc_set_slot_limit,             /*  Config action routine        */
      NULL,                           /*  Argument to include in call  */
      RSRC_CONF | ACCESS_CONF,        /*  Where available (*.conf)     */
      "Request slot choke limit (range: 0-65535) for a Virtual Host "
      "(Default is 0 or unlimited slots)"
                                      /*  Directive description        */
   ),

   AP_INIT_TAKE1(
      "VHostChokeBurstPercent",       /*  Directive name            */
      vhc_set_burst_percent,          /*  Config action routine        */
      NULL,                           /*  Argument to include in call  */
      RSRC_CONF | ACCESS_CONF,        /*  Where available (*.conf)     */
      "When VHostChokeSlotLimit is reached, the burst percentage controls "
      "how many additional slots are allowed for occasional request "
      "bursts. Setting this to 0 turns off bursting (Default is 30)"
                                      /*  Directive description        */
   ),

   AP_INIT_TAKE1(
      "VHostChokeGracePeriod",        /*  Directive name             */
      vhc_set_grace_period,           /*  Config action routine        */
      NULL,                           /*  Argument to include in call  */
      RSRC_CONF | ACCESS_CONF,        /*  Where available (*.conf)     */
      "When VHostChokeSlotLimit is reached, the grace period in seconds "
      "controls how long request bursts will get additional slots. "
      "Setting this to 0 turns off bursting (Default is 30 seconds)"
                                      /*  Directive description        */
   ),

   AP_INIT_TAKE1(
      "VHostChokeBurstFlapPeriod",    /*  Directive name         */
      vhc_set_burst_flap_period,      /*  Config action routine        */
      NULL,                           /*  Argument to include in call  */
      RSRC_CONF | ACCESS_CONF,        /*  Where available (*.conf)     */
      "When VHostChokeSlotLimit is reached and bursting occured, this "
      "setting (in seconds - range: 0-65535) controls how long to wait "
      "before allowing bursts again. Prevents flapping - up/down within "
      "the burst period. Default is 1800 seconds or 30 minutes"
                                      /*  Directive description        */
   ),

   {NULL}                             /*  Last command.  */
};


/*  Module definition for VHC configuration.  */
module AP_MODULE_DECLARE_DATA  vhost_choke_module = {
   STANDARD20_MODULE_STUFF,
   NULL,                      /*  per-directory config creator     */
   NULL,                      /*  dir config merger                */
   vhc_create_server_config,  /*  server config creator            */
   NULL,                      /*  server config merger             */
   vhc_cmds,                  /*  command table                    */
   vhc_register_hooks,        /*  set up request processing hooks  */
};

/*  }}}  -- End section:ap-module-globals.  */



/**
 *  EOF
 */
