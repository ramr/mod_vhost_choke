/*  =======================================================================
 * 
 *  ~ramr
 *  <see-license-file />
 *  <insert-mit-license-here />
 *  
 *  =======================================================================
 *
 *     File:  mod_vhost_choke.h
 *
 *    Author: ~ramr
 *
 *  Summary:  Header file for apache module - the "carburator" to control
 *            request/vhost mixture. Throttles requests to a virtual host. 
 *
 */

#ifndef  _VHOST_CHOKE_H_
#define  _VHOST_CHOKE_H_  "vhost-choke.h"

/*  section:includes {{{  */
/*  ++++++++++++++++      */

/*  Include Apache header files we need here.  */
#include "httpd.h"
#include "apr.h"
#include "apr_errno.h"
#include "apr_time.h"

/*  Include the standard header files we use here.  */
#if APR_HAVE_SYS_TYPES_H
   #include <sys/types.h>
#endif  /*  APR_HAVE_SYS_TYPES_H  */

#if APR_HAVE_UNISTD_H
   #include <unistd.h>
#endif  /*  APR_HAVE_UNISTD_H  */

#if APR_HAVE_ERRNO_H
   #include <errno.h>
#endif  /*  APR_HAVE_ERRNO_H  */

#if APR_HAVE_STDIO_H
   #include <stdio.h>
#endif  /*  APR_HAVE_STDIO_H  */

#if APR_HAVE_STDLIB_H
   #include <stdlib.h>
#endif  /*  APR_HAVE_STDLIB_H  */

#if APR_HAVE_STRING_H
   #include <string.h>
#endif  /*  APR_HAVE_STRING_H  */

#if APR_HAVE_STRINGS_H
   #include <strings.h>
#endif  /*  APR_HAVE_STRINGS_H  */

/*  TODO: Check on math functions.  */
#include <math.h>   /*  For ceil  */

/*  }}}  -- End section:includes.  */


/*  section:defines {{{  */
/*  +++++++++++++++      */

/*  Define for namespace.  */
#define  VHOST_CHOKE_NAMESPACE   "vhc"

/*  Defines for module information.  */
#define  VHC_MODULE_BASEVENDOR   "~ramr"
#define  VHC_MODULE_BASEPROJECT  "github.com/ramr/mod_vhost_choke"
#define  VHC_MODULE_BASEPRODUCT  "mod_vhost_choke"
#define  VHC_MODULE_DESCRIPTION  "Apache VirtualHost Request Choke Module"

#define  VHC_MODULE_NAME         "vhost_choke"
#define  VHC_MODULE_VERSION      "0.1"


/*  Defines for string conversion.  */
#define  VHC_STR_CONVERT(str)  #str
#define  VHC_STR(str)          VHC_STR_CONVERT(str)


/*  Define for handling headers and status.  */
#define  VHC_X_THROTTLED_BY_HEADER_NAME     "X-Throttled-By"
#define  VHC_APR_STATUS_IS_SUCCESS(status)  (APR_SUCCESS == (status))

/*  Define for default temporary directory and debug message limits.  */
#define  VHC_DEFAULT_TEMP_DIR       "/tmp"
#define  VHC_MAX_DEBUG_MESSAGE_LEN  (1024 + 1)

/*  Define for getting function names.  */
#if defined(__FUNCTION__)
   #define VHC_LOC  __FUNCTION__
#else
   #define VHC_LOC  __FILE__ ":" VHC_STR(__LINE__)
#endif  /*  defined(__FUNCTION__).  */


/*  Defines for how long to wait on locks + number of tries.  */
#define  VHC_MAX_LOCK_WAIT_TIME_USECS  (100 * 1000) /*  100ms in usecs.  */
#define  VHC_TRYLOCK_SLEEP_TIME_USECS  (5 * 1000)   /*  5ms in usecs.    */

/*  Defines for apache config directive value limits.  */
#define  VHC_MAX_SLOT_LIMIT         32767
#define  VHC_MAX_ERROR_MESSAGE_LEN  (140 + 1)


/*  Defines for default HTTP code and error message.  */
#define  VHC_DEFAULT_SLOT_LIMIT       0
#define  VHC_DEFAULT_HTTP_ERROR_CODE  VHC_HTTP_TOO_MANY_REQUESTS
#define  VHC_DEFAULT_ERROR_MSG        "VirtualHost choked. Try again later."

/*  Defines for per-vhost burst settings.  */
#define  VHC_DEFAULT_BURST_PERCENT      30    /*  Burst upto 30%.  */
#define  VHC_DEFAULT_GRACE_PERIOD       10    /*  In seconds.      */
#define  VHC_DEFAULT_BURST_FLAP_PERIOD  1800  /*  In seconds.      */


/*  Defines for HTTP response code for too many requests (see RFC 6585).  */
#define  VHC_HTTP_TOO_MANY_REQUESTS         429   /*  As per RFC 6585.  */
#define  VHC_HTTP_MSG_TOO_MANY_REQUESTS     "Too Many Requests"

/*  Define for choked responses.  */
#define  VHC_CHOKED_RESPONSE_CONTENT_TYPE    "text/html"


/*  }}}  -- End section:defines.  */


/*  section:typedefs {{{  */
/*  ++++++++++++++++      */

/*  Boolean.  */
typedef  enum { VHC_FALSE = 0, VHC_TRUE = !VHC_FALSE }  VHC_boolean;

/*  Structure contain settings related to the whole module.  */
typedef struct vhc_env_settings {
   VHC_boolean   debug;          /*  Debugging flag.           */
   apr_uint16_t  http_code;      /*  HTTP response code.       */

   char          err_message[VHC_MAX_ERROR_MESSAGE_LEN /* 141 */];
                                 /*  Error message to client.  */

   char          filler[1];      /*  Filler/boundary adjust.   */

} VHC_env_settings_t, *VHC_env_settings_t_p;


/*  Structure definitions for vhost_choke server configs.  */
typedef struct  vhc_server_config {
   char         *server_hostname;   /*  The server hostname.           */

   apr_uint16_t  config_id;         /*  Unique config id.              */
   apr_uint16_t  slot_limit;        /*  Slot (or choke at) limit.      */
                                    /*  Default: 0 or no limit.        */

   /*  Structure contain burst settings related to the "bursting".  */
   struct burst_settings {
      apr_uint16_t  grace_period;   /*  Grace period for "bursting".   */
      apr_uint16_t  flap_period;    /*  Handle burst flapping.         */

      apr_uint16_t  percent;        /*  Max. allowable percentage to   */
                                    /*  burst over the choke limit.    */
      char          filler[2];      /*  Filler/boundary adjust.        */

   } burst_settings;

}  VHC_server_config_t, *VHC_server_config_t_p;


/*  Structure definitions for per-vhost shm data.  */
typedef struct  vhc_shm_data {
   apr_time_t    grace_expires_at;  /*  When grace period expires.     */
   apr_uint64_t  inuse_slots;       /*  # of slots currently in use.   */

}  VHC_shm_data_t, *VHC_shm_data_t_p;

/*  }}}  -- End section:typedefs.  */


#endif  /*  For  _VHOST_CHOKE_H_.  */



/**
 *  EOF
 */
