#ifndef SOS_DEBUG_H
#define SOS_DEBUG_H

#include <stdio.h>

#include "sos.h"
#include "sosd.h"

/*
 * sos_debug.h
 *
 * NOTE: A central location to switch the debug flags on and off, as
 *       well as define/re-define what happens when various debugging
 *       functions are called.
 *
 *       Include this in the .C files of the modules, it doesn't need
 *       to be in the API.
 */


/* GLOBAL
 *     +3 = VERY verbose
 *      0 = essential messages only   IMPORTANT! This *allows* sosd/sosa logging.
 *     -1 = disable logging entirely  <--- Faster, but HIDES ALL sosd/sosa logs, too!
 */
#ifndef SOS_DEBUG
#define SOS_DEBUG                -1
#endif
#define SOS_DEBUG_SHOW_LOCATION   0 

/* Daemon logging sensitivity.         (Req. SOS_DEBUG >= 0) */
#define SOSD_DAEMON_LOG          -1 
#define SOSD_ECHO_TO_STDOUT       0

#define SOSA_DEBUG_LEVEL          0

int     sos_daemon_lock_fptr;
FILE   *sos_daemon_log_fptr;


/* Defined in sosd.c ... */

#if (SOS_DEBUG < 0)

    /* Nullify the variadic debugging macros wherever they are in code: */
    #define dlog(level, ...)

#else
    /* Set the behavior of the debugging macros: */
    /* Simple debug output, no locking: */
    #define dlog(level, ...);                                           \
    if (SOS->role != SOS_ROLE_CLIENT) {                                 \
        if (SOS->role == SOS_ROLE_ANALYTICS) {                          \
            if (SOSA_DEBUG_LEVEL >= level) {                            \
                if (SOS_DEBUG_SHOW_LOCATION > 0) {                      \
                    printf("(%s:%d)", __FILE__, __LINE__ );             \
                }                                                       \
                if (SOS_WHOAMI == NULL) {                               \
                    printf("ERROR: SOS_WHOAMI == NULL !\n");            \
                    exit(EXIT_FAILURE);                                 \
                }                                                       \
                printf("[%s.%s]: ", SOS_WHOAMI, SOS_WHEREAMI);          \
                printf(__VA_ARGS__);                                    \
                if (stdout) fflush(stdout);                             \
            }                                                           \
        } else {                                                        \
            if (SOSD_DAEMON_LOG >= level) {                             \
                if (SOS_DEBUG_SHOW_LOCATION > 0) {                      \
                    if (sos_daemon_log_fptr != NULL) {                  \
                        fprintf(sos_daemon_log_fptr, "(%s:%d)",         \
                                __FILE__, __LINE__ );                   \
                    }                                                   \
                }                                                       \
                if (SOS_WHOAMI == NULL) {                               \
                    printf("ERROR: SOS_WHOAMI == NULL !\n");            \
                    exit(EXIT_FAILURE);                                 \
                }                                                       \
                if (sos_daemon_log_fptr != NULL) {                      \
                    fprintf(sos_daemon_log_fptr, "[%s.%s]: ",           \
                            SOS_WHOAMI, SOS_WHEREAMI);                  \
                    fprintf(sos_daemon_log_fptr, __VA_ARGS__);          \
                    fflush(sos_daemon_log_fptr);                        \
                }                                                       \
                if ((SOSD_DAEMON_MODE == 0) && SOSD_ECHO_TO_STDOUT) {   \
                    if (SOS_DEBUG_SHOW_LOCATION > 0) {                  \
                        printf("(" SOS_MAG);                            \
                        printf("%s" SOS_CYN ":" SOS_GRN "%d",           \
                                __FILE__, __LINE__ );                   \
                        printf(SOS_CLR ")");                            \
                    }                                                   \
                    printf("[" SOS_DIM_WHT "%s." SOS_CLR SOS_BOLD_RED   \
                            "%s" SOS_CLR                                \
                            "]: ", SOS_WHOAMI, SOS_WHEREAMI);           \
                    printf(__VA_ARGS__);                                \
                    fflush(stdout);                                     \
                }                                                       \
            }                                                           \
        }                                                               \
    } else {                                                            \
        if ((SOS->config.options != NULL) &&                            \
            (SOS->config.options->batch_environment)) {                 \
            if (SOS_DEBUG >= level && SOS->role == SOS_ROLE_CLIENT) {   \
                if (SOS_DEBUG_SHOW_LOCATION > 0) {                      \
                    printf("(%s:%d)", __FILE__, __LINE__ );             \
                }                                                       \
                if (SOS_WHOAMI == NULL) {                               \
                    printf("ERROR: SOS_WHOAMI == NULL !\n");            \
                    exit(EXIT_FAILURE);                                 \
                }                                                       \
                printf("[%s.%s]: ", SOS_WHOAMI, SOS_WHEREAMI);          \
                printf(__VA_ARGS__);                                    \
                if (stdout) fflush(stdout);                             \
            }                                                           \
        } else {                                                        \
            if (SOS_DEBUG >= level && SOS->role == SOS_ROLE_CLIENT) {   \
                if (SOS_DEBUG_SHOW_LOCATION > 0) {                      \
                    printf("(" SOS_MAG "%s" SOS_CYN ":" SOS_GRN         \
                            "%d" SOS_CLR ")", __FILE__, __LINE__ );     \
                    printf(SOS_CLR);                                    \
                }                                                       \
                if (SOS_WHOAMI == NULL) {                               \
                    printf("ERROR: SOS_WHOAMI == NULL !\n");            \
                    exit(EXIT_FAILURE);                                 \
                }                                                       \
                printf("[" SOS_DIM_WHT "%s." SOS_CLR SOS_BOLD_RED "%s"  \
                        SOS_CLR "]: ",                                  \
                        SOS_WHOAMI, SOS_WHEREAMI);                      \
                printf(__VA_ARGS__);                                    \
                if (stdout) fflush(stdout);                             \
            }                                                           \
        }                                                               \
    }


#endif //DEBUG

#endif //SOS_DEBUG_H
