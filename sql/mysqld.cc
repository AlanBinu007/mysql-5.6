/* Copyright (c) 2000, 2018, Oracle and/or its affiliates. All rights
   reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "my_global.h"                          /* NO_EMBEDDED_ACCESS_CHECKS */

#include <algorithm>
#include <functional>
#include <list>
#include <set>
#include <cmath>
#include <cstring>
#include <atomic>

#include "sql_priv.h"
#include "unireg.h"
#include <algorithm>
#include <signal.h>
#include "sql_parse.h"    // test_if_data_home_dir
#include "sql_cache.h"    // query_cache, query_cache_*
#include "sql_locale.h"   // MY_LOCALES, my_locales, my_locale_by_name
#include "sql_show.h"     // free_status_vars, add_status_vars,
                          // reset_status_vars
#include "strfunc.h"      // find_set_from_flags
#include "parse_file.h"   // File_parser_dummy_hook
#include "sql_db.h"       // my_dboptions_cache_free
                          // my_dboptions_cache_init
#include "sql_table.h"    // release_ddl_log, execute_ddl_log_recovery
#include "sql_connect.h"  // free_max_user_conn, init_max_user_conn,
                          // handle_one_connection
#include "sql_time.h"     // known_date_time_formats,
                          // get_date_time_format_str,
                          // date_time_format_make
#include "tztime.h"       // my_tz_free, my_tz_init, my_tz_SYSTEM
#include "hostname.h"     // hostname_cache_free, hostname_cache_init
#include "sql_acl.h"      // acl_free, grant_free, acl_init,
                          // grant_init
#include "sql_base.h"     // table_def_free, table_def_init,
                          // Table_cache,
                          // cached_table_definitions
#include "sql_test.h"     // mysql_print_status
#include "item_create.h"  // item_create_cleanup, item_create_init
#include "sql_servers.h"  // servers_free, servers_init
#include "init.h"         // unireg_init
#include "derror.h"       // init_errmessage
#include "des_key_file.h" // load_des_key_file
#include "sql_manager.h"  // stop_handle_manager, start_handle_manager
#include <m_ctype.h>
#include <my_dir.h>
#include <my_bit.h>
#include "rpl_gtid.h"
#include "rpl_slave.h"
#include "rpl_master.h"
#include "rpl_mi.h"
#include "rpl_filter.h"
#include <sql_common.h>
#include <my_stacktrace.h>
#include "mysqld_suffix.h"
#include "mysys_err.h"
#include "events.h"
#include "sql_audit.h"
#include "probes_mysql.h"
#include "scheduler.h"
#include "debug_sync.h"
#include "sql_callback.h"
#include "opt_trace_context.h"
#include "sql_multi_tenancy.h"
#include "native_procedure_priv.h"
#include "lf.h"

#include "global_threads.h"
#include "mysqld.h"
#include "my_default.h"

#ifdef WITH_PERFSCHEMA_STORAGE_ENGINE
#include "../storage/perfschema/pfs_server.h"
#endif /* WITH_PERFSCHEMA_STORAGE_ENGINE */
#include <mysql/psi/mysql_idle.h>
#include <mysql/psi/mysql_socket.h>
#include <mysql/psi/mysql_statement.h>
#include "mysql_com_server.h"
#include "mysql_githash.h"

#include "keycaches.h"
#include "../storage/myisam/ha_myisam.h"
#include "set_var.h"

#include "sys_vars_shared.h"

#include "rpl_injector.h"

#include "rpl_handler.h"

#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif

#ifdef TARGET_OS_LINUX
#include <mntent.h>
#include <sys/statfs.h>
#include "flashcache_ioctl.h"
#endif /* TARGET_OS_LINUX */

#include <thr_alarm.h>
#include <ft_global.h>
#include <errmsg.h>
#include "sp_rcontext.h"
#include "sp_cache.h"
#include "sql_reload.h"  // reload_acl_and_cache

#include "my_timer.h"    // my_timer_init, my_timer_deinit

#ifdef HAVE_POLL_H
#include <poll.h>
#endif

#include <xmmintrin.h>
#include "mpsc.h"

#ifdef HAVE_FESETROUND
#include <fenv.h>
#endif
#include "table_cache.h" // table_cache_manager

#ifndef EMBEDDED_LIBRARY
#include "srv_session.h"
#endif

#include <zstd.h>
#ifndef ZSTD_CLEVEL_DEFAULT
#define ZSTD_CLEVEL_DEFAULT 3
#endif

#ifdef HAVE_JEMALLOC
#ifndef EMBEDDED_LIBRARY
#include <jemalloc/jemalloc.h>

// Please see https://github.com/jemalloc/jemalloc/issues/325 to learn more
// about the functionality this change enables.
extern "C" {
#if JEMALLOC_VERSION_MAJOR > 4
  const char* malloc_conf = "prof:true,prof_active:false"
    ",prof_prefix:jeprof.out";
#else
  const char *malloc_conf = "purge:decay,prof:true,prof_active:false,"
    "prof_prefix:jeprof.out";
#endif
}

#endif
#endif

#if OPENSSL_VERSION_NUMBER >= 0x10100000L
// Function removed after OpenSSL 1.1.0
#define ERR_remove_state(x)
#endif

using std::min;
using std::max;
using std::vector;

#define mysqld_charset &my_charset_latin1

/* We have HAVE_purify below as this speeds up the shutdown of MySQL */

#if defined(HAVE_DEC_3_2_THREADS) || defined(SIGNALS_DONT_BREAK_READ) || defined(HAVE_purify) && defined(__linux__)
#define HAVE_CLOSE_SERVER_SOCK 1
#endif

extern "C" {          // Because of SCO 3.2V4.2
#include <errno.h>
#include <sys/stat.h>
#ifndef __GNU_LIBRARY__
#define __GNU_LIBRARY__       // Skip warnings in getopt.h
#endif
#include <my_getopt.h>
#ifdef HAVE_SYSENT_H
#include <sysent.h>
#endif
#ifdef HAVE_PWD_H
#include <pwd.h>        // For getpwent
#endif
#ifdef HAVE_GRP_H
#include <grp.h>
#endif
#include <my_net.h>

#if !defined(__WIN__)
#include <sys/resource.h>
#ifdef HAVE_SYS_CAPABILITY_H
#include <sys/capability.h>
#endif
#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif
#ifdef HAVE_SELECT_H
#include <select.h>
#endif
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#include <sys/utsname.h>
#endif /* __WIN__ */

#include <my_libwrap.h>

#ifdef HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif

#ifdef __WIN__
#include <crtdbg.h>
#endif

#ifdef HAVE_SOLARIS_LARGE_PAGES
#include <sys/mman.h>
#if defined(__sun__) && defined(__GNUC__) && defined(__cplusplus) \
    && defined(_XOPEN_SOURCE)
extern int getpagesizes(size_t *, int);
extern int getpagesizes2(size_t *, int);
extern int memcntl(caddr_t, size_t, int, caddr_t, int, int);
#endif /* __sun__ ... */
#endif /* HAVE_SOLARIS_LARGE_PAGES */

#ifdef _AIX41
int initgroups(const char *,unsigned int);
#endif

#if defined(__FreeBSD__) && defined(HAVE_IEEEFP_H) && !defined(HAVE_FEDISABLEEXCEPT)
#include <ieeefp.h>
#ifdef HAVE_FP_EXCEPT       // Fix type conflict
typedef fp_except fp_except_t;
#endif
#endif /* __FreeBSD__ && HAVE_IEEEFP_H && !HAVE_FEDISABLEEXCEPT */
#ifdef HAVE_SYS_FPU_H
/* for IRIX to use set_fpc_csr() */
#include <sys/fpu.h>
#endif
#ifdef HAVE_FPU_CONTROL_H
#include <fpu_control.h>
#endif
#if defined(__i386__) && !defined(HAVE_FPU_CONTROL_H)
# define fpu_control_t unsigned int
# define _FPU_EXTENDED 0x300
# define _FPU_DOUBLE 0x200
# if defined(__GNUC__) || (defined(__SUNPRO_CC) && __SUNPRO_CC >= 0x590)
#  define _FPU_GETCW(cw) asm volatile ("fnstcw %0" : "=m" (*&cw))
#  define _FPU_SETCW(cw) asm volatile ("fldcw %0" : : "m" (*&cw))
# else
#  define _FPU_GETCW(cw) (cw= 0)
#  define _FPU_SETCW(cw)
# endif
#endif

extern "C" my_bool reopen_fstreams(const char *filename,
                                   FILE *outstream, FILE *errstream);

inline void setup_fpu()
{
#if defined(__FreeBSD__) && defined(HAVE_IEEEFP_H) && !defined(HAVE_FEDISABLEEXCEPT)
  /* We can't handle floating point exceptions with threads, so disable
     this on freebsd
     Don't fall for overflow, underflow,divide-by-zero or loss of precision.
     fpsetmask() is deprecated in favor of fedisableexcept() in C99.
  */
#if defined(FP_X_DNML)
  fpsetmask(~(FP_X_INV | FP_X_DNML | FP_X_OFL | FP_X_UFL | FP_X_DZ |
        FP_X_IMP));
#else
  fpsetmask(~(FP_X_INV |             FP_X_OFL | FP_X_UFL | FP_X_DZ |
              FP_X_IMP));
#endif /* FP_X_DNML */
#endif /* __FreeBSD__ && HAVE_IEEEFP_H && !HAVE_FEDISABLEEXCEPT */

#ifdef HAVE_FEDISABLEEXCEPT
  fedisableexcept(FE_ALL_EXCEPT);
#endif

#ifdef HAVE_FESETROUND
    /* Set FPU rounding mode to "round-to-nearest" */
  fesetround(FE_TONEAREST);
#endif /* HAVE_FESETROUND */

  /*
    x86 (32-bit) requires FPU precision to be explicitly set to 64 bit
    (double precision) for portable results of floating point operations.
    However, there is no need to do so if compiler is using SSE2 for floating
    point, double values will be stored and processed in 64 bits anyway.
  */
#if defined(__i386__) && !defined(__SSE2_MATH__)
#if defined(_WIN32)
#if !defined(_WIN64)
  _control87(_PC_53, MCW_PC);
#endif /* !_WIN64 */
#else /* !_WIN32 */
  fpu_control_t cw;
  _FPU_GETCW(cw);
  cw= (cw & ~_FPU_EXTENDED) | _FPU_DOUBLE;
  _FPU_SETCW(cw);
#endif /* _WIN32 && */
#endif /* __i386__ */

#if defined(__sgi) && defined(HAVE_SYS_FPU_H)
  /* Enable denormalized DOUBLE values support for IRIX */
  union fpc_csr n;
  n.fc_word = get_fpc_csr();
  n.fc_struct.flush = 0;
  set_fpc_csr(n.fc_word);
#endif
}

} /* cplusplus */

#define MYSQL_KILL_SIGNAL SIGTERM

#include <my_pthread.h>     // For thr_setconcurency()

#ifdef SOLARIS
extern "C" int gethostname(char *name, int namelen);
#endif

extern "C" sig_handler handle_fatal_signal(int sig);
extern "C" sig_handler handle_fatal_signal_ex(int sig, siginfo_t *info,
                                              void *ctx);

#if defined(__linux__)
#define ENABLE_TEMP_POOL 1
#else
#define ENABLE_TEMP_POOL 0
#endif

class SOCKET_PACKET {

public:

  MYSQL_SOCKET new_sock;
  bool is_admin;
  bool is_unix_sock;

  SOCKET_PACKET() : is_admin(false), is_unix_sock(false) { }
};

static std::vector<mpsc_queue_t<SOCKET_PACKET>* > sockets_list;

#if !defined(EMBEDDED_LIBRARY)
#if !defined(DBUG_OFF)
static uint64_t con_threads_count= 0;
static uint64_t con_threads_new= 0;
static uint64_t con_threads_wait= 0;
static uint64_t dropped_conns= 0;
#endif
my_bool separate_conn_handling_thread= 0;
my_bool gl_socket_sharding= 0;
uint num_sharded_sockets= 1;
uint num_conn_handling_threads= 1;
static std::vector<MYSQL_SOCKET> ip_sharded_sockets;
std::atomic<uint64_t> send_q_index(0);
#endif

/* Constants */

#include <welcome_copyright_notice.h> // ORACLE_WELCOME_COPYRIGHT_NOTICE

const char *show_comp_option_name[]= {"YES", "NO", "DISABLED"};

static const char *tc_heuristic_recover_names[]=
{
  "COMMIT", "ROLLBACK", NullS
};
static TYPELIB tc_heuristic_recover_typelib=
{
  array_elements(tc_heuristic_recover_names)-1,"",
  tc_heuristic_recover_names, NULL
};

const char *first_keyword= "first", *binary_keyword= "BINARY";
const char *my_localhost= "localhost", *delayed_user= "DELAYED";

static const char *rocksdb_engine_name = "rocksdb";

bool opt_large_files= sizeof(my_off_t) > 4;
static my_bool opt_autocommit; ///< for --autocommit command-line option

/*
  Used with --help for detailed option
*/
static my_bool opt_help= 0, opt_verbose= 0;

arg_cmp_func Arg_comparator::comparator_matrix[5][2] =
{{&Arg_comparator::compare_string,     &Arg_comparator::compare_e_string},
 {&Arg_comparator::compare_real,       &Arg_comparator::compare_e_real},
 {&Arg_comparator::compare_int_signed, &Arg_comparator::compare_e_int},
 {&Arg_comparator::compare_row,        &Arg_comparator::compare_e_row},
 {&Arg_comparator::compare_decimal,    &Arg_comparator::compare_e_decimal}};

/* static variables */

#ifdef HAVE_PSI_INTERFACE
#if (defined(_WIN32) || defined(HAVE_SMEM)) && !defined(EMBEDDED_LIBRARY)
static PSI_thread_key key_thread_handle_con_namedpipes;
static PSI_cond_key key_COND_handler_count;
static PSI_rwlock_key key_rwlock_LOCK_named_pipe_full_access_group;
#endif /* _WIN32 || HAVE_SMEM && !EMBEDDED_LIBRARY */

#if defined(HAVE_SMEM) && !defined(EMBEDDED_LIBRARY)
static PSI_thread_key key_thread_handle_con_sharedmem;
#endif /* HAVE_SMEM && !EMBEDDED_LIBRARY */

#if !defined(EMBEDDED_LIBRARY)
static PSI_thread_key key_thread_handle_con_sockets;
static PSI_thread_key key_thread_handle_con_admin_sockets;
#endif /* !EMBEDDED_LIBRARY */

#ifdef __WIN__
static PSI_thread_key key_thread_handle_shutdown;
#endif /* __WIN__ */
#endif /* HAVE_PSI_INTERFACE */

/**
  Statement instrumentation key for replication.
*/
#ifdef HAVE_PSI_STATEMENT_INTERFACE
PSI_statement_info stmt_info_rpl;
#endif

/* the default log output is log tables */
static bool lower_case_table_names_used= 0;
static bool volatile select_thread_in_use, signal_thread_in_use;
/* See Bug#56666 and Bug#56760 */;
volatile bool ready_to_exit;
static my_bool opt_debugging= 0, opt_external_locking= 0, opt_console= 0;
my_bool opt_allow_multiple_engines= 0;
static my_bool opt_short_log_format= 0;
static std::atomic<bool> kill_blocked_pthreads_flag(false);
static uint wake_pthread;
static ulong killed_threads;
       ulong max_used_connections;
static char *mysqld_user, *mysqld_chroot;
static char *default_character_set_name;
static char *character_set_filesystem_name;
static char *lc_messages;
static char *lc_time_names_name;
char *my_bind_addr_str;
static char *default_collation_name;
char *default_storage_engine;
char *default_tmp_storage_engine;
static char compiled_default_collation_name[]= MYSQL_DEFAULT_COLLATION_NAME;
static bool binlog_format_used= false;

/* MySQL and RocksDB git hashes and dates */
static char git_hash[] = MYSQL_GIT_HASH;
static char git_date[] = MYSQL_GIT_DATE;
static char rocksdb_git_hash[] = ROCKSDB_GIT_HASH;
static char rocksdb_git_date[] = ROCKSDB_GIT_DATE;

LEX_STRING opt_init_connect, opt_init_slave;

static mysql_cond_t COND_thread_cache, COND_flush_thread_cache;

/* Global variables */

bool opt_bin_log, opt_ignore_builtin_innodb= 0;
bool opt_trim_binlog= 0;
my_bool opt_log, opt_slow_log, opt_log_raw;
ulonglong log_output_options;
my_bool opt_log_queries_not_using_indexes= 0;
my_bool opt_disable_working_set_size = 0;
ulong opt_log_throttle_queries_not_using_indexes= 0;
ulong opt_log_throttle_legacy_user= 0;
ulong opt_log_throttle_ddl= 0;
bool log_sbr_unsafe = 0;
ulong opt_log_throttle_sbr_unsafe_queries = 0;
bool opt_improved_dup_key_error= 0;
bool opt_error_log= IF_WIN(1,0);
bool opt_disable_networking=0, opt_skip_show_db=0;
bool opt_skip_name_resolve=0;
my_bool opt_character_set_client_handshake= 1;
bool server_id_supplied = 0;
bool opt_endinfo, using_udf_functions;
my_bool locked_in_memory;
bool opt_using_transactions;
bool volatile abort_loop;
bool volatile shutdown_in_progress;
ulong log_warnings;
uint host_cache_size;
ulonglong tmp_table_rpl_max_file_size;
ulong slave_tx_isolation;
bool enable_blind_replace= 0;
bool enable_binlog_hlc= 0;
bool maintain_database_hlc= 0;
ulong wait_for_hlc_timeout_ms = 0;
ulong wait_for_hlc_sleep_threshold_ms = 0;
double wait_for_hlc_sleep_scaling_factor = 0.75;
my_bool async_query_counter_enabled = 0;
ulong opt_commit_consensus_error_action= 0;
my_bool enable_acl_fast_lookup= 0;
my_bool use_cached_table_stats_ptr;
longlong max_digest_sample_age;
ulonglong max_tmp_disk_usage;
ulonglong tmp_table_disk_usage_period_peak = 0;
ulonglong filesort_disk_usage_period_peak = 0;
bool enable_raft_plugin= 0;
bool recover_raft_log= 0;
bool disable_raft_log_repointing= 0;
bool override_enable_raft_check= false;
ulong opt_raft_signal_async_dump_threads= 0;
ulonglong apply_log_retention_num= 0;
ulonglong apply_log_retention_duration= 0;
bool show_query_digest= false;
bool set_read_only_on_shutdown= false;
extern bool fix_read_only(sys_var *self, THD *thd, enum_var_type type);

/* write_control_level:
 * Global variable to control write throttling for short running queries and
 * abort for long running queries.
 */
ulong write_control_level;
/* Global variable to denote the maximum CPU time (specified in milliseconds)
 * limit for DML queries.
 */
uint write_cpu_limit_milliseconds;
/* Global variable to denote the frequency (specified in number of rows) of
 * checking whether DML queries exceeded the CPU time limit enforced by
 * 'write_time_check_batch'
 */
uint write_time_check_batch;

my_bool log_legacy_user;
my_bool log_ddl;
const char *opt_legacy_user_name_pattern;
Regex_list_handler *legacy_user_name_pattern;

#if defined(_WIN32) && !defined(EMBEDDED_LIBRARY)
ulong slow_start_timeout;
#endif
/*
  True if the bootstrap thread is running. Protected by LOCK_thread_count.
  Used in bootstrap() function to determine if the bootstrap thread
  has completed. Note, that we can't use 'thread_count' instead,
  since in 5.1, in presence of the Event Scheduler, there may be
  event threads running in parallel, so it's impossible to know
  what value of 'thread_count' is a sign of completion of the
  bootstrap thread.

  At the same time, we can't start the event scheduler after
  bootstrap either, since we want to be able to process event-related
  SQL commands in the init file and in --bootstrap mode.
*/
bool in_bootstrap= FALSE;
my_bool opt_bootstrap= 0;

/**
   @brief 'grant_option' is used to indicate if privileges needs
   to be checked, in which case the lock, LOCK_grant, is used
   to protect access to the grant table.
   @note This flag is dropped in 5.1
   @see grant_init()
 */
bool volatile grant_option;

my_bool opt_skip_slave_start = 0; ///< If set, slave is not autostarted
my_bool opt_reckless_slave = 0;
my_bool opt_enable_named_pipe= 0;
my_bool opt_local_infile, opt_slave_compressed_protocol;
my_bool opt_slave_compressed_event_protocol;
ulonglong opt_max_compressed_event_cache_size;
ulonglong opt_compressed_event_cache_evict_threshold;
ulong opt_slave_compression_lib;
ulonglong opt_slave_dump_thread_wait_sleep_usec;
my_bool rpl_wait_for_semi_sync_ack;
std::atomic<ulonglong> slave_lag_sla_misses{0};
ulonglong opt_slave_lag_sla_seconds;
my_bool opt_safe_user_create = 0;
my_bool opt_show_slave_auth_info;
my_bool opt_log_slave_updates= 0;
char *opt_slave_skip_errors;
char *opt_rbr_idempotent_tables;
my_bool opt_slave_allow_batching= 0;

/**
  compatibility option:
    - index usage hints (USE INDEX without a FOR clause) behave as in 5.0
*/
my_bool old_mode;

/*
  Legacy global handlerton. These will be removed (please do not add more).
*/
handlerton *heap_hton;
handlerton *myisam_hton;
handlerton *partition_hton;

/* Wait time on global realonly/commit lock */
std::atomic_ullong init_global_rolock_timer = {0};
std::atomic_ullong init_commit_lock_timer = {0};
uint opt_server_id_bits= 0;
ulong opt_server_id_mask= 0;
my_bool send_error_before_closing_timed_out_connection = TRUE;
my_bool allow_document_type = FALSE;
my_bool block_create_myisam = FALSE;
my_bool block_create_memory = FALSE;
my_bool read_only= 0, opt_readonly= 0;
char* opt_read_only_error_msg_extra;
my_bool skip_master_info_check_for_read_only_error_msg_extra;
my_bool super_read_only = 0, opt_super_readonly = 0;
my_bool use_temp_pool, relay_log_purge;
my_bool relay_log_recovery;
my_bool opt_sync_frm, opt_allow_suspicious_udfs;
my_bool opt_secure_auth= 0;
char* opt_secure_file_priv;
my_bool opt_log_slow_admin_statements= 0;
my_bool opt_log_slow_slave_statements= 0;
my_bool lower_case_file_system= 0;
my_bool opt_large_pages= 0;
my_bool opt_super_large_pages= 0;
my_bool opt_myisam_use_mmap= 0;
uint   opt_large_page_size= 0;

/* Number of times the IO thread connected to the master */
ulong relay_io_connected= 0;

/* Number of events written to the relay log by the IO thread */
ulong relay_io_events= 0;

/* Number of events executed from the relay log by the SQL thread */
ulong relay_sql_events= 0;

/* Number of bytes written to the relay log by the IO thread */
ulonglong relay_io_bytes= 0;

/* Number of events executed from the relay log by the SQL thread */
ulonglong relay_sql_bytes= 0;

/* Time the SQL thread waits for events from the IO thread */
ulonglong relay_sql_wait_time= 0;

/* Cache hit ratio when using slave_compressed_event_protocol in dump thread.
   It is updated every minute */
double comp_event_cache_hit_ratio= 0;

/* Number of times async dump threads waited for semi-sync ACK */
ulonglong repl_semi_sync_master_ack_waits= 0;

/* status variables for binlog fsync histogram */
SHOW_VAR latency_histogram_binlog_fsync[NUMBER_OF_HISTOGRAM_BINS + 1];
ulonglong histogram_binlog_fsync_values[NUMBER_OF_HISTOGRAM_BINS];

SHOW_VAR
  histogram_binlog_group_commit_var[NUMBER_OF_COUNTER_HISTOGRAM_BINS + 1];
ulonglong
  histogram_binlog_group_commit_values[NUMBER_OF_COUNTER_HISTOGRAM_BINS];

uint net_compression_level = 6;
/* 0 is means default for zstd/lz4. */
long zstd_net_compression_level = ZSTD_CLEVEL_DEFAULT;
long lz4f_net_compression_level = 0;
extern ulonglong compress_ctx_reset;
extern ulonglong compress_input_bytes;
extern ulonglong compress_output_bytes;

#if defined(ENABLED_DEBUG_SYNC)
MYSQL_PLUGIN_IMPORT uint    opt_debug_sync_timeout= 0;
#endif /* defined(ENABLED_DEBUG_SYNC) */
my_bool opt_old_style_user_limits= 0, trust_function_creators= 0;
/*
  True if there is at least one per-hour limit for some user, so we should
  check them before each query (and possibly reset counters when hour is
  changed). False otherwise.
*/
volatile bool mqh_used = 0;
my_bool opt_noacl= 0;
my_bool sp_automatic_privileges= 1;
my_bool opt_process_can_disable_bin_log = TRUE;

static const ulong DEFAULT_SRV_FATAL_SEMAPHORE_TIMEOUT = 600; /* in seconds */
ulong opt_srv_fatal_semaphore_timeout = DEFAULT_SRV_FATAL_SEMAPHORE_TIMEOUT;
ulong opt_binlog_rows_event_max_size;
ulonglong opt_binlog_rows_event_max_rows;
bool opt_log_only_query_comments = false;
bool opt_binlog_trx_meta_data = false;
bool opt_log_column_names = false;
const char *binlog_checksum_default= "NONE";
ulong binlog_checksum_options;
my_bool opt_master_verify_checksum= 0;
my_bool opt_slave_sql_verify_checksum= 1;
ulong opt_slave_check_before_image_consistency= 0;
const char *binlog_format_names[]= {"MIXED", "STATEMENT", "ROW", NullS};
my_bool enforce_gtid_consistency;
my_bool binlog_gtid_simple_recovery;
ulong binlog_error_action;
const char *binlog_error_action_list[]=
  {"IGNORE_ERROR", "ABORT_SERVER", "ROLLBACK_TRX", NullS};
my_bool log_gtid_unsafe_statements;
my_bool use_db_uuid;
my_bool skip_core_dump_on_error;
/* Controls implementation of user_table_statistics (see sys_vars.cc) */
ulong user_table_stats_control;
/* Contains the list of users with admin roles (comma separated) */
char *admin_users_list;
Regex_list_handler *admin_users_list_regex;
/* Controls collecting statistics for every SQL statement */
ulong sql_stats_control;
/* Controls collecting column statistics for every SQL statement */
ulong column_stats_control;
/* Controls collecting execution plans for every SQL statement */
ulong sql_plans_control;
/* Controls collecting MySQL findings (aka SQL conditions) */
ulong sql_findings_control;
/* Controls collecting execution plans for slow queries */
my_bool sql_plans_capture_slow_query;
/* Controls the frequency of sql plans capture */
uint  sql_plans_capture_frequency;
/* Controls collecting execution plans based on a filter on query text */
my_bool sql_plans_capture_apply_filter;
/* Controls whether the plan ID is computed from normalized execution plan */
my_bool normalized_plan_id;
/* Controls whether MySQL send an error when running duplicate statements */
uint sql_maximum_duplicate_executions;
/* Controls the mode of enforcement of duplicate executions of the same stmt */
ulong sql_duplicate_executions_control;
/* Controls num most recent data points to collect for information_schema.write_statistics */
uint write_stats_count;
/* Controls the frequency(seconds) at which write stats and replica lag stats are collected*/
ulong write_stats_frequency;
/* A replication lag higher than the value of this variable will enable throttling of write workload */
ulong write_start_throttle_lag_milliseconds;
/* A replication lag lower than the value of this variable will disable throttling of write workload */
ulong write_stop_throttle_lag_milliseconds;
/* Minimum value of the ratio (1st entity)/(2nd entity) for replication lag throttling to kick in */
double write_throttle_min_ratio;
/* Number of consecutive cycles to monitor an entity for replication lag throttling before taking action */
uint write_throttle_monitor_cycles;
/* Percent of secondaries that need to lag for overall replication topology to be considered lagging */
uint write_throttle_lag_pct_min_secondaries;
/* The frequency (seconds) at which auto throttling checks are run on a primary */
ulong write_auto_throttle_frequency;
/* Determines the step by which throttle rate probability is incremented or decremented */
uint write_throttle_rate_step;
/* Stores the (latest)value for sys_var write_throttle_patterns  */
char *latest_write_throttling_rule;
/* Stores the (latest)value for sys_var write_throttle_permissible_dimensions_in_order*/
char *latest_write_throttle_permissible_dimensions_in_order;
/* Vector of all dimensions that are permissible to be throttled in order by replication lag system*/
std::vector<enum_wtr_dimension> write_throttle_permissible_dimensions_in_order;
/* Patterns to throttle queries in case of replication lag */
GLOBAL_WRITE_THROTTLING_RULES_MAP global_write_throttling_rules;
/* Controls the width of the histogram bucket (unit: kilo-bytes) */
uint transaction_size_histogram_width;
uint write_statistics_histogram_width;
/* timestamp when replication lag check was last done */
std::atomic<time_t> last_replication_lag_check_time(0);
/* Queue to store all the entities being currently auto throttled. It is used to release entities in order
they were throttled when replication lag goes below safe threshold  */
std::list<std::pair<std::string, enum_wtr_dimension>> currently_throttled_entities;
/* Stores the info about the entity that is currently being monitored for replication lag */
WRITE_MONITORED_ENTITY currently_monitored_entity;
/* Controls whether special privileges are needed for accessing some MT tables */
my_bool mt_tables_access_control;

ulong gtid_mode;
ulong slave_gtid_info;
bool enable_gtid_mode_on_new_slave_with_old_master;
my_bool is_slave = false;
/* Set to true when slave_stats_daemon thread is in use */
std::atomic<int> slave_stats_daemon_thread_counter(0);
my_bool read_only_slave;
const char *gtid_mode_names[]=
{"OFF", "UPGRADE_STEP_1", "UPGRADE_STEP_2", "ON", NullS};
const char *slave_gtid_info_names[]=
{"OFF", "ON", "OPTIMIZED", NullS};
TYPELIB gtid_mode_typelib=
{ array_elements(gtid_mode_names) - 1, "", gtid_mode_names, NULL };

#ifdef HAVE_INITGROUPS
volatile sig_atomic_t calling_initgroups= 0; /**< Used in SIGSEGV handler. */
#endif
uint mysqld_port, test_flags, select_errors, dropping_tables, ha_open_options;
uint mysqld_port_timeout;
ulong mysqld_admin_port;
ulong delay_key_write_options;
uint protocol_version;
uint lower_case_table_names;
ulong tc_heuristic_recover= 0;
int32 num_thread_running;
int32 thread_binlog_client;
int32 thread_binlog_comp_event_client= 0;
std::atomic<unsigned long> thread_created;
ulong back_log, connect_timeout, concurrency, server_id;
ulong table_cache_size, table_def_size;
ulong table_cache_instances;
ulong table_cache_size_per_instance;
ulong what_to_log;
ulong slow_launch_time;
int32 slave_open_temp_tables;
ulong open_files_limit, max_binlog_size, max_relay_log_size;
ulong slave_trans_retries;
uint  slave_net_timeout;
ulong slave_exec_mode_options;
ulong slave_use_idempotent_for_recovery_options = 0;
ulong slave_run_triggers_for_rbr = 0;
ulonglong slave_type_conversions_options;
char *opt_rbr_column_type_mismatch_whitelist= nullptr;
ulonglong admission_control_filter;
ulonglong admission_control_wait_events;
ulonglong admission_control_yield_freq;
ulong opt_mts_slave_parallel_workers;
ulong opt_mts_dependency_replication;
ulonglong opt_mts_dependency_size;
double opt_mts_dependency_refill_threshold;
ulonglong opt_mts_dependency_max_keys;
ulong opt_mts_dependency_order_commits;
ulonglong opt_mts_dependency_cond_wait_timeout;
my_bool opt_mts_dynamic_rebalance;
double opt_mts_imbalance_threshold;
ulonglong opt_mts_pending_jobs_size_max;
ulonglong slave_rows_search_algorithms_options;
#ifndef DBUG_OFF
uint slave_rows_last_search_algorithm_used;
#endif
ulonglong binlog_bytes_written = 0;
ulonglong relay_log_bytes_written = 0;
ulong binlog_cache_size=0;
char *enable_jemalloc_hpp;
char *thread_priority_str = NULL;
ulonglong  max_binlog_cache_size=0;
ulong slave_max_allowed_packet= 0;
ulong binlog_stmt_cache_size=0;
ulonglong  max_binlog_stmt_cache_size=0;
ulong query_cache_size=0;
ulong refresh_version;  /* Increments on each reload */
query_id_t global_query_id;
my_atomic_rwlock_t global_query_id_lock;
my_atomic_rwlock_t thread_running_lock;
my_atomic_rwlock_t slave_open_temp_tables_lock;
my_atomic_rwlock_t write_query_running_lock;
ulong aborted_threads, aborted_connects;
/* status variable of the age of the latest evicted page in buffer pool */
ulong last_evicted_page_age= 0;
ulong delayed_insert_timeout, delayed_insert_limit, delayed_queue_size;
ulong delayed_insert_threads, delayed_insert_writes, delayed_rows_in_use;
ulong delayed_insert_errors,flush_time;
bool flush_only_old_table_cache_entries = false;
ulong specialflag=0;
ulong binlog_cache_use= 0, binlog_cache_disk_use= 0;
ulong binlog_stmt_cache_use= 0, binlog_stmt_cache_disk_use= 0;
ulong max_connections, max_connect_errors;
uint max_nonsuper_connections;
ulong opt_max_running_queries, opt_max_waiting_queries;
ulong opt_max_db_connections;
my_bool opt_admission_control_by_trx= 0;
char *admission_control_weights;
extern AC *db_ac;
ulong rpl_stop_slave_timeout= LONG_TIMEOUT;
my_bool rpl_slave_flow_control = 1;
my_bool rpl_skip_tx_api = 0;
my_bool log_bin_use_v1_row_events= 0;
bool thread_cache_size_specified= false;
bool host_cache_size_specified= false;
bool table_definition_cache_specified= false;
ulonglong rbr_unsafe_queries = 0;

/** Related to query throttling logic. */
uint opt_general_query_throttling_limit= 0;
uint opt_write_query_throttling_limit= 0;
int32 write_query_running= 0;
ulonglong read_queries= 0, write_queries= 0;
ulonglong total_query_rejected= 0, write_query_rejected= 0;
ulonglong object_stats_misses = 0;

Error_log_throttle err_log_throttle(Log_throttle::LOG_THROTTLE_WINDOW_SIZE,
                                    sql_print_error,
                                    "Error log throttle: %10lu 'Can't create"
                                    " thread to handle new connection'"
                                    " error(s) suppressed");

my_bool opt_log_slow_extra= FALSE;
ulonglong binlog_fsync_count = 0;
ulong opt_peak_lag_time;
ulong opt_peak_lag_sample_rate;
my_bool slave_high_priority_ddl= FALSE;
double slave_high_priority_lock_wait_timeout_double= 1.0;
ulonglong slave_high_priority_lock_wait_timeout_nsec= 1.0;
std::atomic<ulonglong> slave_high_priority_ddl_executed(0);
std::atomic<ulonglong> slave_high_priority_ddl_killed_connections(0);
my_bool log_datagram= 0;
ulong log_datagram_usecs= 0;
int log_datagram_sock= -1;

/**
  Limit of the total number of prepared statements in the server.
  Is necessary to protect the server against out-of-memory attacks.
*/
ulong max_prepared_stmt_count;
/**
  Current total number of prepared statements in the server. This number
  is exact, and therefore may not be equal to the difference between
  `com_stmt_prepare' and `com_stmt_close' (global status variables), as
  the latter ones account for all registered attempts to prepare
  a statement (including unsuccessful ones).  Prepared statements are
  currently connection-local: if the same SQL query text is prepared in
  two different connections, this counts as two distinct prepared
  statements.
*/
ulong prepared_stmt_count=0;
my_thread_id thread_id_counter=1;
std::atomic<uint64_t> total_thread_ids(0);
const my_thread_id reserved_thread_id=0;

ulong current_pid;
ulong slow_launch_threads = 0;
uint sync_binlog_period= 0, sync_relaylog_period= 0,
     sync_relayloginfo_period= 0, sync_masterinfo_period= 0,
     opt_mts_checkpoint_period, opt_mts_checkpoint_group;
ulong expire_logs_days = 0;
ulong binlog_expire_logs_seconds= 0;
/**
  Soft upper limit for number of sp_head objects that can be stored
  in the sp_cache for one connection.
*/
ulong stored_program_cache_size= 0;
/**
  Compatibility option to prevent auto upgrade of old temporals
  during certain ALTER TABLE operations.
*/
my_bool avoid_temporal_upgrade;

/* flashcache */
int cachedev_fd;
my_bool cachedev_enabled= FALSE;

const double log_10[] = {
  1e000, 1e001, 1e002, 1e003, 1e004, 1e005, 1e006, 1e007, 1e008, 1e009,
  1e010, 1e011, 1e012, 1e013, 1e014, 1e015, 1e016, 1e017, 1e018, 1e019,
  1e020, 1e021, 1e022, 1e023, 1e024, 1e025, 1e026, 1e027, 1e028, 1e029,
  1e030, 1e031, 1e032, 1e033, 1e034, 1e035, 1e036, 1e037, 1e038, 1e039,
  1e040, 1e041, 1e042, 1e043, 1e044, 1e045, 1e046, 1e047, 1e048, 1e049,
  1e050, 1e051, 1e052, 1e053, 1e054, 1e055, 1e056, 1e057, 1e058, 1e059,
  1e060, 1e061, 1e062, 1e063, 1e064, 1e065, 1e066, 1e067, 1e068, 1e069,
  1e070, 1e071, 1e072, 1e073, 1e074, 1e075, 1e076, 1e077, 1e078, 1e079,
  1e080, 1e081, 1e082, 1e083, 1e084, 1e085, 1e086, 1e087, 1e088, 1e089,
  1e090, 1e091, 1e092, 1e093, 1e094, 1e095, 1e096, 1e097, 1e098, 1e099,
  1e100, 1e101, 1e102, 1e103, 1e104, 1e105, 1e106, 1e107, 1e108, 1e109,
  1e110, 1e111, 1e112, 1e113, 1e114, 1e115, 1e116, 1e117, 1e118, 1e119,
  1e120, 1e121, 1e122, 1e123, 1e124, 1e125, 1e126, 1e127, 1e128, 1e129,
  1e130, 1e131, 1e132, 1e133, 1e134, 1e135, 1e136, 1e137, 1e138, 1e139,
  1e140, 1e141, 1e142, 1e143, 1e144, 1e145, 1e146, 1e147, 1e148, 1e149,
  1e150, 1e151, 1e152, 1e153, 1e154, 1e155, 1e156, 1e157, 1e158, 1e159,
  1e160, 1e161, 1e162, 1e163, 1e164, 1e165, 1e166, 1e167, 1e168, 1e169,
  1e170, 1e171, 1e172, 1e173, 1e174, 1e175, 1e176, 1e177, 1e178, 1e179,
  1e180, 1e181, 1e182, 1e183, 1e184, 1e185, 1e186, 1e187, 1e188, 1e189,
  1e190, 1e191, 1e192, 1e193, 1e194, 1e195, 1e196, 1e197, 1e198, 1e199,
  1e200, 1e201, 1e202, 1e203, 1e204, 1e205, 1e206, 1e207, 1e208, 1e209,
  1e210, 1e211, 1e212, 1e213, 1e214, 1e215, 1e216, 1e217, 1e218, 1e219,
  1e220, 1e221, 1e222, 1e223, 1e224, 1e225, 1e226, 1e227, 1e228, 1e229,
  1e230, 1e231, 1e232, 1e233, 1e234, 1e235, 1e236, 1e237, 1e238, 1e239,
  1e240, 1e241, 1e242, 1e243, 1e244, 1e245, 1e246, 1e247, 1e248, 1e249,
  1e250, 1e251, 1e252, 1e253, 1e254, 1e255, 1e256, 1e257, 1e258, 1e259,
  1e260, 1e261, 1e262, 1e263, 1e264, 1e265, 1e266, 1e267, 1e268, 1e269,
  1e270, 1e271, 1e272, 1e273, 1e274, 1e275, 1e276, 1e277, 1e278, 1e279,
  1e280, 1e281, 1e282, 1e283, 1e284, 1e285, 1e286, 1e287, 1e288, 1e289,
  1e290, 1e291, 1e292, 1e293, 1e294, 1e295, 1e296, 1e297, 1e298, 1e299,
  1e300, 1e301, 1e302, 1e303, 1e304, 1e305, 1e306, 1e307, 1e308
};

time_t server_start_time, flush_status_time;

char server_uuid[UUID_LENGTH+1];
const char *server_uuid_ptr;
char binlog_file_basedir[FN_REFLEN];
char binlog_index_basedir[FN_REFLEN];
char mysql_home[FN_REFLEN], pidfile_name[FN_REFLEN], system_time_zone[30];
char shutdownfile_name[FN_REFLEN];
char default_logfile_name[FN_REFLEN];
char *default_tz_name;
char log_error_file[FN_REFLEN], glob_hostname[FN_REFLEN];
char mysql_real_data_home[FN_REFLEN],
     lc_messages_dir[FN_REFLEN], reg_ext[FN_EXTLEN],
     mysql_charsets_dir[FN_REFLEN],
     *opt_init_file, *opt_tc_log_file;
char *opt_gap_lock_exception_list;
my_bool legacy_global_read_lock_mode = FALSE;
char *lc_messages_dir_ptr, *log_error_file_ptr;
char mysql_unpacked_real_data_home[FN_REFLEN];
int mysql_unpacked_real_data_home_len;
uint mysql_real_data_home_len, mysql_data_home_len= 1;
uint reg_ext_length;
const key_map key_map_empty(0);
key_map key_map_full(0);                        // Will be initialized later
char logname_path[FN_REFLEN];
char slow_logname_path[FN_REFLEN];
char gap_lock_logname_path[FN_REFLEN];
char secure_file_real_path[FN_REFLEN];

DATE_TIME_FORMAT global_date_format, global_datetime_format, global_time_format;
Time_zone *default_tz;

char *mysql_data_home= const_cast<char*>(".");
const char *mysql_real_data_home_ptr= mysql_real_data_home;
char server_version[SERVER_VERSION_LENGTH];
char *mysqld_unix_port, *opt_mysql_tmpdir;
char *mysqld_socket_umask;
ulong thread_handling;

/** name of reference on left expression in rewritten IN subquery */
const char *in_left_expr_name= "<left expr>";
/** name of additional condition */
const char *in_additional_cond= "<IN COND>";
const char *in_having_cond= "<IN HAVING>";

my_decimal decimal_zero;
/** Number of connection errors when selecting on the listening port */
ulong connection_errors_select= 0;
/** Number of connection errors when accepting sockets in the listening port. */
ulong connection_errors_accept= 0;
/** Number of connection errors from TCP wrappers. */
ulong connection_errors_tcpwrap= 0;
/** Number of connection errors from internal server errors. */
ulong connection_errors_internal= 0;
/** Number of connection errors from the server max_connection limit. */
ulong connection_errors_max_connection= 0;
/** Number of connection abort errors from the server max_connection limit  */
ulong connection_errors_max_connection_abort= 0;
/** Number of errors when reading the peer address. */
ulong connection_errors_peer_addr= 0;
/** Number of connection errors while writing to client */
ulong connection_errors_net_ER_NET_ERROR_ON_WRITE= 0;
/** Number of connection errors because of packet ordering issues */
ulong connection_errors_net_ER_NET_PACKETS_OUT_OF_ORDER= 0;
/** Number of connection errors because the packet was too larger */
ulong connection_errors_net_ER_NET_PACKET_TOO_LARGE= 0;
/** Number of connection errors while reading data from client */
ulong connection_errors_net_ER_NET_READ_ERROR= 0;
/** Number of connection errors due to read timeout */
ulong connection_errors_net_ER_NET_READ_INTERRUPTED= 0;
/** Number of connection errors due to compression errors */
ulong connection_errors_net_ER_NET_UNCOMPRESS_ERROR= 0;
/** Number of connection errors due to write timeout */
ulong connection_errors_net_ER_NET_WRITE_INTERRUPTED= 0;
/** Number of connection errors when host has insufficient privilege */
ulong connection_errors_host_not_privileged = 0;
/** Number of connection errors when host is blocked */
ulong connection_errors_host_blocked = 0;
/** Number of connection errors when acl_authenticate failed */
ulong connection_errors_acl_auth= 0;
/** Number of connection errors when out of resources */
ulong connection_errors_out_of_resources= 0;
/** Number of connection errors due to SSL */
ulong connection_errors_ssl_check= 0;
/** Number of connection errors with auth plugin */
ulong connection_errors_auth_plugin= 0;
/** Number of connection errors with authentication */
ulong connection_errors_auth= 0;
/** Number of connection errors with hand shaking */
ulong connection_errors_handshake= 0;
/** Number of connection errors when no proxy user is found */
ulong connection_errors_proxy_user= 0;
/** Number of connection errors when max global is reached */
ulong connection_errors_multi_tenancy_max_global= 0;
/** Number of connection errors when password expired */
ulong connection_errors_password_expired= 0;
/** Number of connection errors when user_conn cannot be created */
ulong connection_errors_user_conn= 0;
/** Number of connection errors when denied connection as admin */
ulong connection_errors_admin_conn_denied= 0;
/** Number of connection errors when max user connection is reached */
ulong connection_errors_max_user_connection= 0;
/** Number of connection errors when user is denied access to the database */
ulong connection_errors_access_denied= 0;

/** Number of cache misses for acl_cache */
ulong acl_cache_miss= 0;

/** Number of cache miss in acl_fast_lookup */
ulong acl_fast_lookup_miss= 0;

/** Whether acl_fast_lookup is enabled - it only gets updated when FLUSH
    PRIVILEGES is issued */
my_bool acl_fast_lookup_enabled= FALSE;

/* Number of times fb style json functions are called */
ulonglong json_extract_count = 0;
ulonglong json_contains_count = 0;
ulonglong json_valid_count = 0;
ulonglong json_func_binary_count = 0;

/* Whether sql_stats_snapshot is enabled. If a session exists with
   sql_stats_snapshot set to ON this status var is ON, otherwise it's OFF. */
my_bool sql_stats_snapshot_status = FALSE;

/* classes for comparation parsing/processing */
Eq_creator eq_creator;
Ne_creator ne_creator;
Gt_creator gt_creator;
Lt_creator lt_creator;
Ge_creator ge_creator;
Le_creator le_creator;

MYSQL_FILE *bootstrap_file;
int bootstrap_error;

Rpl_filter* rpl_filter;
Rpl_filter* binlog_filter;

struct system_variables global_system_variables;
struct system_variables max_system_variables;
struct system_status_var global_status_var;

MY_TMPDIR mysql_tmpdir_list;
MY_BITMAP temp_pool;

CHARSET_INFO *system_charset_info, *files_charset_info ;
CHARSET_INFO *national_charset_info, *table_alias_charset;
CHARSET_INFO *character_set_filesystem;
CHARSET_INFO *error_message_charset_info;

MY_LOCALE *my_default_lc_messages;
MY_LOCALE *my_default_lc_time_names;

SHOW_COMP_OPTION have_openssl;
SHOW_COMP_OPTION have_ssl, have_symlink, have_dlopen, have_query_cache;
SHOW_COMP_OPTION have_geometry, have_rtree_keys;
SHOW_COMP_OPTION have_crypt, have_compress;
SHOW_COMP_OPTION have_profiling;
SHOW_COMP_OPTION have_statement_timeout= SHOW_OPTION_DISABLED;

/* Thread specific variables */

pthread_key(MEM_ROOT**,THR_MALLOC);
pthread_key(THD*, THR_THD);
mysql_mutex_t LOCK_global_table_stats;
mysql_mutex_t LOCK_thread_created;
mysql_mutex_t LOCK_thread_count, LOCK_thd_remove;

/* Lock to protect global_sql_stats_map and global_sql_text_map structures */
mysql_mutex_t LOCK_global_sql_stats;
/* Lock to protect global_sql_plans map structure */
mysql_mutex_t LOCK_global_sql_plans;
/* Lock to protect global_active_sql map structure */
mysql_mutex_t LOCK_global_active_sql;
/* Lock to protect global_sql_findings map structure */
mysql_mutex_t LOCK_global_sql_findings;
/* Lock to protect sql_stats_snapshot */
mysql_rwlock_t LOCK_sql_stats_snapshot;
/* Lock to protect global_write_statistics map structure */
mysql_mutex_t LOCK_global_write_statistics;
/* Lock to protect global_write_throttling_rules map structure */
mysql_mutex_t LOCK_global_write_throttling_rules;
/* Lock to protect global_write_throttling_log map structure */
mysql_mutex_t LOCK_global_write_throttling_log;
/* Lock to provide exclusion in access to global_tx_size_histogram */
mysql_mutex_t LOCK_global_tx_size_histogram;
/* Lock to provide exclusion in access to global_write_stat_histogram */
mysql_mutex_t LOCK_global_write_stat_histogram;
/* Lock to be used for auto throttling based on replication lag */
mysql_mutex_t LOCK_replication_lag_auto_throttling;

#ifdef SHARDED_LOCKING
std::vector<mysql_mutex_t> LOCK_thread_count_sharded;
std::vector<mysql_mutex_t> LOCK_thd_remove_sharded;
static ShardedThreads *global_thread_list = NULL;
static bool sharded_locks_init = false;
#else
static std::set<THD*> *global_thread_list = NULL;
#endif

mysql_mutex_t
  LOCK_status, LOCK_error_log, LOCK_uuid_generator,
  LOCK_delayed_insert, LOCK_delayed_status, LOCK_delayed_create,
  LOCK_crypt,
  LOCK_global_system_variables,
  LOCK_user_conn, LOCK_slave_list, LOCK_active_mi,
  LOCK_connection_count, LOCK_error_messages;
mysql_mutex_t LOCK_sql_rand;

/**
  The below lock protects access to two global server variables:
  max_prepared_stmt_count and prepared_stmt_count. These variables
  set the limit and hold the current total number of prepared statements
  in the server, respectively. As PREPARE/DEALLOCATE rate in a loaded
  server may be fairly high, we need a dedicated lock.
*/
mysql_mutex_t LOCK_prepared_stmt_count;

/*
 The below two locks are introudced as guards (second mutex) for
  the global variables sql_slave_skip_counter and slave_net_timeout
  respectively. See fix_slave_skip_counter/fix_slave_net_timeout
  for more details
*/
mysql_mutex_t LOCK_sql_slave_skip_counter;
mysql_mutex_t LOCK_slave_net_timeout;
mysql_mutex_t LOCK_log_throttle_qni;
mysql_mutex_t LOCK_log_throttle_legacy;
mysql_mutex_t LOCK_log_throttle_ddl;
mysql_mutex_t LOCK_log_throttle_sbr_unsafe;

#ifdef HAVE_OPENSSL
mysql_mutex_t LOCK_des_key_file;
mysql_rwlock_t LOCK_use_ssl;
#endif
mysql_rwlock_t LOCK_column_statistics;
mysql_rwlock_t LOCK_grant, LOCK_sys_init_connect, LOCK_sys_init_slave;
mysql_rwlock_t LOCK_system_variables_hash;
mysql_cond_t COND_thread_count;
pthread_t signal_thread;
pthread_attr_t connection_attrib;
mysql_mutex_t LOCK_server_started;
mysql_cond_t COND_server_started;

int mysqld_server_started= 0;

File_parser_dummy_hook file_parser_dummy_hook;

/* replication parameters, if master_host is not NULL, we are a slave */
uint report_port= 0;
ulong master_retry_count=0;
char *master_info_file;
char *relay_log_info_file, *report_user, *report_password, *report_host;
char *opt_relay_logname = 0, *opt_relaylog_index_name=0;
char *opt_logname, *opt_slow_logname, *opt_bin_logname;
char *opt_apply_logname = 0;
char *opt_applylog_index_name = 0;
bool should_free_opt_apply_logname= false;
bool should_free_opt_applylog_index_name= false;
char *opt_gap_lock_logname;
/* Static variables */

static volatile sig_atomic_t kill_in_progress;


static my_bool opt_myisam_log;
static int cleanup_done;
static ulong opt_specialflag;
static char *opt_update_logname;
char *opt_binlog_index_name;
char *mysql_home_ptr, *pidfile_name_ptr, *shutdownfile_name_ptr;
char *binlog_file_basedir_ptr;
char *binlog_index_basedir_ptr;
char *per_user_session_var_default_val_ptr=0;
char *per_user_session_var_user_name_delimiter_ptr=nullptr;
static const char default_per_user_session_var_user_name_delimiter=':';
/** Initial command line arguments (count), after load_defaults().*/
static int defaults_argc;
/**
  Initial command line arguments (arguments), after load_defaults().
  This memory is allocated by @c load_defaults() and should be freed
  using @c free_defaults().
  Do not modify defaults_argc / defaults_argv,
  use remaining_argc / remaining_argv instead to parse the command
  line arguments in multiple steps.
*/
static char **defaults_argv;
/** Remaining command line arguments (count), filtered by handle_options().*/
static int remaining_argc;
/** Remaining command line arguments (arguments), filtered by handle_options().*/
static char **remaining_argv;

static int generate_apply_file_gvars();

int orig_argc;
char **orig_argv;

#if defined(HAVE_OPENSSL) && !defined(HAVE_YASSL)
bool init_rsa_keys(void);
void deinit_rsa_keys(void);
int show_rsa_public_key(THD *thd, SHOW_VAR *var, char *buff);
#endif

static std::atomic<uint> global_thread_count(0);
std::set<my_thread_id> *global_thread_id_list= NULL;

ulong max_blocked_pthreads= 0;
static ulong blocked_pthread_count= 0;
static std::list<THD*> *waiting_thd_list= NULL;
Checkable_rwlock *global_sid_lock= NULL;
Sid_map *global_sid_map= NULL;
Gtid_state *gtid_state= NULL;
extern my_bool opt_core_file;

/* Minimum HLC value for this instance. It is ensured that the next 'event' will
   get a HLC timestamp greater than this value */
ulonglong minimum_hlc_ns= 0;

/* Maximum allowed forward drift in the HLC as compared to wall clock */
ulonglong maximum_hlc_drift_ns= 0;

/* Enable query checksum validation for queries with a checksum sent */
my_bool enable_query_checksum= FALSE;

/* Enable resultset checksum validation when enabled by query attr */
my_bool enable_resultset_checksum= FALSE;

std::unique_ptr<HHWheelTimer> hhWheelTimer;

/*
  Since performance schema might not be compiled in, reference
  LF_HASH_OVERHEAD here to pull in the lf library.
*/
int dummy_lf_size= LF_HASH_OVERHEAD;

/*
  global_thread_list and waiting_thd_list are both pointers to objects
  on the heap, to avoid potential problems with running destructors atexit().
 */
static void delete_global_thread_list()
{
  delete global_thread_list;
  delete global_thread_id_list;
  delete waiting_thd_list;
  global_thread_list= NULL;
  global_thread_id_list= NULL;
  waiting_thd_list= NULL;
}

Thread_iterator global_thread_list_begin()
{
  mutex_assert_owner_all_shards(SHARDED(&LOCK_thread_count));
  return global_thread_list->begin();
}

Thread_iterator global_thread_list_end()
{
  mutex_assert_owner_all_shards(SHARDED(&LOCK_thread_count));
  return global_thread_list->end();
}

void copy_global_thread_list(std::set<THD*> *new_copy)
{
  mutex_assert_owner_all_shards(SHARDED(&LOCK_thd_remove));
  mutex_lock_all_shards(SHARDED(&LOCK_thread_count));

#ifndef SHARDED_LOCKING
  *new_copy= *global_thread_list;
#else
  std::set<THD*> tmp;
  Thread_iterator itr = global_thread_list->begin();
  Thread_iterator end = global_thread_list->end();
  for (; itr != end; ++itr) {
    tmp.insert(*itr);
  }
  *new_copy = tmp;
#endif

  mutex_unlock_all_shards(SHARDED(&LOCK_thread_count));
}

void copy_global_thread_list_sorted(
    std::set<THD *, bool (*)(const THD *, const THD *)> *new_copy)
{
  mutex_assert_owner_all_shards(SHARDED(&LOCK_thd_remove));
  mutex_lock_all_shards(SHARDED(&LOCK_thread_count));

#ifndef SHARDED_LOCKING
  for (auto thd : *global_thread_list)
    new_copy->insert(thd);
#else
  Thread_iterator itr = global_thread_list->begin();
  Thread_iterator end = global_thread_list->end();
  for (; itr != end; ++itr) {
    new_copy->insert(*itr);
  }
#endif

  mutex_unlock_all_shards(SHARDED(&LOCK_thread_count));
}

void add_global_thread(THD *thd)
{
  DBUG_PRINT("info", ("add_global_thread %p", thd));
  mutex_assert_owner_shard(SHARDED(&LOCK_thread_count), thd);
  bool have_thread= global_thread_list->find_bool(thd);
  if (!have_thread)
  {
    ++global_thread_count;
    global_thread_list->insert(thd);
  }
  // Adding the same THD twice is an error.
  DBUG_ASSERT(!have_thread);
}

void remove_global_thread(THD *thd)
{
  DBUG_PRINT("info", ("remove_global_thread %p current_linfo %p",
                      thd, thd->current_linfo));

#ifdef HAVE_REPLICATION
  /*
    It is possible when a slave connection dies after register_slave,
    it may end up leaking SLAVE_INFO structure with a dangling THD pointer
    In most cases it may be freed by the same slave with the same server_id
    when it register itself again with the same server_id, but if the
    server_id ends up being different the entry could be leaked forever and
    with an invalid THD pointer
    To address this issue, we let the THD remove itself from the slave list
  */
  unregister_slave(thd, true, true);
#endif

  mutex_lock_shard(SHARDED(&LOCK_thd_remove), thd);
  mutex_lock_shard(SHARDED(&LOCK_thread_count), thd);
  DBUG_ASSERT(thd->release_resources_done());
  /*
    Used by binlog_reset_master.  It would be cleaner to use
    DEBUG_SYNC here, but that's not possible because the THD's debug
    sync feature has been shut down at this point.
   */
  DBUG_EXECUTE_IF("sleep_after_lock_thread_count_before_delete_thd",
                  sleep(5););

  const size_t num_erased= global_thread_list->erase(thd);
  if (num_erased == 1)
    --global_thread_count;
  // Removing a THD that was never added is an error.
  DBUG_ASSERT(1 == num_erased);

  mutex_unlock_shard(SHARDED(&LOCK_thd_remove), thd);
  mutex_unlock_shard(SHARDED(&LOCK_thread_count), thd);
}

uint get_thread_count()
{
  return global_thread_count.load();
}

/**
  Check if global tmp disk usage exceeds max.

  @return
    @retval true   Usage exceeds max.
    @retval false  Usage is below max, or max is not set.
*/
bool is_tmp_disk_usage_over_max()
{
  return static_cast<longlong>(max_tmp_disk_usage) > 0 &&
    global_status_var.tmp_table_disk_usage +
    global_status_var.filesort_disk_usage > max_tmp_disk_usage;
}

void set_remaining_args(int argc, char **argv)
{
  remaining_argc= argc;
  remaining_argv= argv;
}
/*
  Multiple threads of execution use the random state maintained in global
  sql_rand to generate random numbers. sql_rnd_with_mutex use mutex
  LOCK_sql_rand to protect sql_rand across multiple instantiations that use
  sql_rand to generate random numbers.
 */
ulong sql_rnd_with_mutex()
{
  mysql_mutex_lock(&LOCK_sql_rand);
  ulong tmp=(ulong) (my_rnd(&sql_rand) * 0xffffffff); /* make all bits random */
  mysql_mutex_unlock(&LOCK_sql_rand);
  return tmp;
}

#ifdef HAVE_PSI_STATEMENT_INTERFACE
PSI_statement_info stmt_info_new_packet;
#endif

#ifndef EMBEDDED_LIBRARY
void net_before_header_psi(struct st_net *net, void *user_data, size_t /* unused: count */)
{
  THD *thd;
  thd= static_cast<THD*> (user_data);
  DBUG_ASSERT(thd != NULL);

  if (thd->m_server_idle)
  {
    /*
      The server is IDLE, waiting for the next command.
      Technically, it is a wait on a socket, which may take a long time,
      because the call is blocking.
      Disable the socket instrumentation, to avoid recording a SOCKET event.
      Instead, start explicitly an IDLE event.
    */
    MYSQL_SOCKET_SET_STATE(net->vio->mysql_socket, PSI_SOCKET_STATE_IDLE);
    MYSQL_START_IDLE_WAIT(thd->m_idle_psi, &thd->m_idle_state);
  }
}

void net_after_header_psi(struct st_net *net, void *user_data, size_t /* unused: count */, my_bool rc)
{
  THD *thd;
  thd= static_cast<THD*> (user_data);
  DBUG_ASSERT(thd != NULL);

  if (thd->m_server_idle)
  {
    /*
      The server just got data for a network packet header,
      from the network layer.
      The IDLE event is now complete, since we now have a message to process.
      We need to:
      - start a new STATEMENT event
      - start a new STAGE event, within this statement,
      - start recording SOCKET WAITS events, within this stage.
      The proper order is critical to get events numbered correctly,
      and nested in the proper parent.
    */
    MYSQL_END_IDLE_WAIT(thd->m_idle_psi);

    if (! rc)
    {
      thd->m_statement_psi= MYSQL_START_STATEMENT(&thd->m_statement_state,
                                                  stmt_info_new_packet.m_key,
                                                  thd->db, thd->db_length,
                                                  thd->charset());

      THD_STAGE_INFO(thd, stage_init);
    }

    /*
      TODO: consider recording a SOCKET event for the bytes just read,
      by also passing count here.
    */
    MYSQL_SOCKET_SET_STATE(net->vio->mysql_socket, PSI_SOCKET_STATE_ACTIVE);
    thd->m_server_idle= false;
  }
}

void init_net_server_extension(THD *thd)
{
#ifdef HAVE_PSI_INTERFACE
  /* Start with a clean state for connection events. */
  thd->m_idle_psi= NULL;
  thd->m_statement_psi= NULL;
  thd->m_server_idle= false;
  /* Hook up the NET_SERVER callback in the net layer. */
  thd->m_net_server_extension.m_user_data= thd;
  thd->m_net_server_extension.m_before_header= net_before_header_psi;
  thd->m_net_server_extension.m_after_header= net_after_header_psi;
  /* Activate this private extension for the mysqld server. */
  thd->get_net()->extension= & thd->m_net_server_extension;
#else
  thd->get_net()->extension= NULL;
#endif
}
#endif /* EMBEDDED_LIBRARY */

/**
  A log message for the error log, buffered in memory.
  Log messages are temporarily buffered when generated before the error log
  is initialized, and then printed once the error log is ready.
*/
class Buffered_log : public Sql_alloc
{
public:
  Buffered_log(enum loglevel level, const char *message);

  ~Buffered_log()
  {}

  void print(void);

private:
  /** Log message level. */
  enum loglevel m_level;
  /** Log message text. */
  String m_message;
};

/**
  Constructor.
  @param level          the message log level
  @param message        the message text
*/
Buffered_log::Buffered_log(enum loglevel level, const char *message)
  : m_level(level), m_message()
{
  m_message.copy(message, strlen(message), &my_charset_latin1);
}

/**
  Print a buffered log to the real log file.
*/
void Buffered_log::print()
{
  /*
    Since messages are buffered, they can be printed out
    of order with other entries in the log.
    Add "Buffered xxx" to the message text to prevent confusion.
  */
  switch(m_level)
  {
  case ERROR_LEVEL:
    sql_print_error("Buffered error: %s\n", m_message.c_ptr_safe());
    break;
  case WARNING_LEVEL:
    sql_print_warning("Buffered warning: %s\n", m_message.c_ptr_safe());
    break;
  case INFORMATION_LEVEL:
    /*
      Messages printed as "information" still end up in the mysqld *error* log,
      but with a [Note] tag instead of an [ERROR] tag.
      While this is probably fine for a human reading the log,
      it is upsetting existing automated scripts used to parse logs,
      because such scripts are likely to not already handle [Note] properly.
      INFORMATION_LEVEL messages are simply silenced, on purpose,
      to avoid un needed verbosity.
    */
    break;
  }
}

/**
  Collection of all the buffered log messages.
*/
class Buffered_logs
{
public:
  Buffered_logs()
  {}

  ~Buffered_logs()
  {}

  void init();
  void cleanup();

  void buffer(enum loglevel m_level, const char *msg);
  void print();
private:
  /**
    Memory root to use to store buffered logs.
    This memory root lifespan is between init and cleanup.
    Once the buffered logs are printed, they are not needed anymore,
    and all the memory used is reclaimed.
  */
  MEM_ROOT m_root;
  /** List of buffered log messages. */
  List<Buffered_log> m_list;
};

void Buffered_logs::init()
{
  init_alloc_root(&m_root, 1024, 0);
}

void Buffered_logs::cleanup()
{
  m_list.delete_elements();
  free_root(&m_root, MYF(0));
}

/**
  Add a log message to the buffer.
*/
void Buffered_logs::buffer(enum loglevel level, const char *msg)
{
  /*
    Do not let Sql_alloc::operator new(size_t) allocate memory,
    there is no memory root associated with the main() thread.
    Give explicitly the proper memory root to use to
    Sql_alloc::operator new(size_t, MEM_ROOT *) instead.
  */
  Buffered_log *log= new (&m_root) Buffered_log(level, msg);
  if (log)
    m_list.push_back(log, &m_root);
}

/**
  Print buffered log messages.
*/
void Buffered_logs::print()
{
  Buffered_log *log;
  List_iterator_fast<Buffered_log> it(m_list);
  while ((log= it++))
    log->print();
}

/** Logs reported before a logger is available. */
static Buffered_logs buffered_logs;

/**
  Error reporter that buffer log messages.
  @param level          log message level
  @param format         log message format string
*/
C_MODE_START
static void buffered_option_error_reporter(enum loglevel level,
                                           const char *format, ...)
{
  va_list args;
  char buffer[1024];

  va_start(args, format);
  my_vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  buffered_logs.buffer(level, buffer);
}


/**
  Character set and collation error reporter that prints to sql error log.
  @param level          log message level
  @param format         log message format string

  This routine is used to print character set and collation
  warnings and errors inside an already running mysqld server,
  e.g. when a character set or collation is requested for the very first time
  and its initialization does not go well for some reasons.

  Note: At early mysqld initialization stage,
  when error log is not yet available,
  we use buffered_option_error_reporter() instead,
  to print general character set subsystem initialization errors,
  such as Index.xml syntax problems, bad XML tag hierarchy, etc.
*/
static void charset_error_reporter(enum loglevel level,
                                   const char *format, ...)
{
  va_list args;
  va_start(args, format);
  vprint_msg_to_log(level, format, args);
  va_end(args);
}
C_MODE_END

static MYSQL_SOCKET unix_sock, ip_sock, admin_ip_sock;
struct rand_struct sql_rand; ///< used by sql_class.cc:THD::THD()

#ifndef EMBEDDED_LIBRARY
struct passwd *user_info;
static pthread_t select_thread, admin_select_thread;
static uint thr_kill_signal;
static bool admin_select_thread_running= false;
static uint handler_count= 0;
#endif

/* OS specific variables */

#ifdef __WIN__
#undef   getpid
#include <process.h>

static mysql_cond_t COND_handler_count;
static bool start_mode=0, use_opt_args;
static int opt_argc;
static char **opt_argv;

#if !defined(EMBEDDED_LIBRARY)
static HANDLE hEventShutdown;
static char shutdown_event_name[40];
#include "nt_servc.h"
static   NTService  Service;        ///< Service object for WinNT
#endif /* EMBEDDED_LIBRARY */
#endif /* __WIN__ */

#ifdef _WIN32
static char pipe_name[512];
#include <Aclapi.h>
#include "named_pipe.h"
static SECURITY_ATTRIBUTES saPipeSecurity;
static SECURITY_DESCRIPTOR sdPipeDescriptor;
static SECURITY_ATTRIBUTES *psaPipeSecurity;
static HANDLE hPipe = INVALID_HANDLE_VALUE;
mysql_rwlock_t LOCK_named_pipe_full_access_group;
char *named_pipe_full_access_group;
#endif

#ifndef EMBEDDED_LIBRARY
bool mysqld_embedded=0;
#else
bool mysqld_embedded=1;
#endif

my_bool plugins_are_initialized= FALSE;

#ifndef DBUG_OFF
static const char* default_dbug_option;
#endif
#ifdef HAVE_LIBWRAP
const char *libwrapName= NULL;
int allow_severity = LOG_INFO;
int deny_severity = LOG_WARNING;
#endif /* HAVE_LIBWRAP */
#ifdef HAVE_QUERY_CACHE
ulong query_cache_min_res_unit= QUERY_CACHE_MIN_RESULT_DATA_SIZE;
Query_cache query_cache;
#endif
#ifdef HAVE_SMEM
char *shared_memory_base_name= default_shared_memory_base_name;
my_bool opt_enable_shared_memory;
HANDLE smem_event_connect_request= 0;
#endif

my_bool opt_use_ssl  = 0;
char *opt_ssl_ca= NULL, *opt_ssl_capath= NULL, *opt_ssl_cert= NULL,
     *opt_ssl_cipher= NULL, *opt_ssl_key= NULL, *opt_ssl_crl= NULL,
     *opt_ssl_crlpath= NULL;

#ifdef HAVE_OPENSSL
char *des_key_file;
#ifndef EMBEDDED_LIBRARY
struct st_VioSSLFd *ssl_acceptor_fd;
#endif
#endif /* HAVE_OPENSSL */

/**
  Number of currently active user connections. Protected
  by LOCK_thread_count
*/
uint connection_count= 0;
uint nonsuper_connections= 0;
mysql_cond_t COND_connection_count;

/* Function declarations */

pthread_handler_t signal_hand(void *arg);
static int mysql_init_variables(void);
static void init_sharding_variables();
static int get_options(int *argc_ptr, char ***argv_ptr, my_bool logging);
static void add_terminator(vector<my_option> *options);
extern "C" my_bool mysqld_get_one_option(int, const struct my_option *, char *);
static void set_server_version(void);
static int init_thread_environment();
static char *get_relative_path(const char *path);
static int fix_paths(void);
void handle_connections_sockets(bool admin,
  const MYSQL_SOCKET *socket_ptr= NULL);
pthread_handler_t dedicated_conn_handling_thread(void *arg);
#if defined(HAVE_SOREUSEPORT)
pthread_handler_t soreusethread(void *arg);
#endif
#ifdef _WIN32
pthread_handler_t handle_connections_sockets_thread(void *arg);
#endif
pthread_handler_t kill_server_thread(void *arg);
static void bootstrap(MYSQL_FILE *file);
static bool read_init_file(char *file_name);
#ifdef _WIN32
pthread_handler_t handle_connections_namedpipes(void *arg);
#endif
#ifdef HAVE_SMEM
pthread_handler_t handle_connections_shared_memory(void *arg);
#endif
pthread_handler_t handle_slave(void *arg);
static void clean_up(bool print_message);
static int test_if_case_insensitive(const char *dir_name);

#ifndef EMBEDDED_LIBRARY
static bool pid_file_created= false;
static void usage(void);
static void start_signal_handler(void);
static void close_server_sock();
static void clean_up_mutexes(void);
static void wait_for_signal_thread_to_end(void);
static void create_pid_file();
static void create_shutdown_file();
static int delete_shutdown_file();
static void mysqld_exit(int exit_code) MY_ATTRIBUTE((noreturn));
#endif


#ifndef EMBEDDED_LIBRARY
/****************************************************************************
** Code to end mysqld
****************************************************************************/

static void close_connections(void)
{
#ifdef EXTRA_DEBUG
  int count=0;
#endif
  DBUG_ENTER("close_connections");

  /* Kill blocked pthreads */
  kill_blocked_pthreads();
  uint dump_thread_count= 0;
  uint dump_thread_kill_retries= 8;
  /* kill connection thread */
#if !defined(__WIN__)
  DBUG_PRINT("quit", ("waiting for select thread: 0x%lx",
                      (ulong) select_thread));
  mysql_mutex_lock(&LOCK_thread_count);

  /* the select thread will always wait for admin thread to exit
     so select_thread_in_use won't be set to false if
     admin_select_thread_running is still true */
  int kill_ret = 0, kill_admin_ret = 0;
  while (select_thread_in_use)
  {
    struct timespec abstime;
    int error;
    LINT_INIT(error);
    DBUG_PRINT("info",("Waiting for select thread"));

#ifndef DONT_USE_THR_ALARM

    if (!kill_admin_ret && admin_select_thread_running)
      kill_admin_ret = pthread_kill(admin_select_thread, thr_client_alarm);

    if (!kill_ret)
      kill_ret = pthread_kill(select_thread, thr_client_alarm);

    if (kill_ret &&
        (!admin_select_thread_running ||
         (admin_select_thread_running && kill_admin_ret)))
      break;          // allready dead
#endif
    set_timespec(abstime, 2);
    for (uint tmp=0 ; tmp < 10 && select_thread_in_use; tmp++)
    {
      error= mysql_cond_timedwait(&COND_thread_count, &LOCK_thread_count,
                                  &abstime);
      if (error != EINTR)
  break;
    }
#ifdef EXTRA_DEBUG
    if (error != 0 && !count++)
      sql_print_error("Got error %d from mysql_cond_timedwait", error);
#endif
    close_server_sock();
  }
  mysql_mutex_unlock(&LOCK_thread_count);
#endif /* __WIN__ */


  /* Abort listening to new connections */
  DBUG_PRINT("quit",("Closing sockets"));
  if (!opt_disable_networking )
  {
    if (mysql_socket_getfd(ip_sock) != INVALID_SOCKET)
    {
      (void) mysql_socket_shutdown(ip_sock, SHUT_RDWR);
      (void) mysql_socket_close(ip_sock);
      ip_sock= MYSQL_INVALID_SOCKET;
    }
#if defined(HAVE_SOREUSEPORT)
    if (gl_socket_sharding) {
      for(uint sid = 1; sid < num_sharded_sockets; sid++) {
        (void) mysql_socket_shutdown(ip_sharded_sockets[sid], SHUT_RDWR);
        (void) mysql_socket_close(ip_sharded_sockets[sid]);
        ip_sharded_sockets[sid] = MYSQL_INVALID_SOCKET;
      }
    }
#endif
    if (mysql_socket_getfd(admin_ip_sock) != INVALID_SOCKET)
    {
      (void) mysql_socket_shutdown(admin_ip_sock, SHUT_RDWR);
      (void) mysql_socket_close(admin_ip_sock);
      admin_ip_sock = MYSQL_INVALID_SOCKET;
    }
  }
#ifdef _WIN32
  if (hPipe != INVALID_HANDLE_VALUE && opt_enable_named_pipe)
  {
    HANDLE temp;
    DBUG_PRINT("quit", ("Closing named pipes") );

    /* Create connection to the handle named pipe handler to break the loop */
    if ((temp = CreateFile(pipe_name,
         GENERIC_READ | GENERIC_WRITE,
         0,
         NULL,
         OPEN_EXISTING,
         0,
         NULL )) != INVALID_HANDLE_VALUE)
    {
      WaitNamedPipe(pipe_name, 1000);
      DWORD dwMode = PIPE_READMODE_BYTE | PIPE_WAIT;
      SetNamedPipeHandleState(temp, &dwMode, NULL, NULL);
      CancelIo(temp);
      DisconnectNamedPipe(temp);
      CloseHandle(temp);
    }
  }
#endif
#ifdef HAVE_SYS_UN_H
  if (mysql_socket_getfd(unix_sock) != INVALID_SOCKET)
  {
    (void) mysql_socket_shutdown(unix_sock, SHUT_RDWR);
    (void) mysql_socket_close(unix_sock);
    (void) unlink(mysqld_unix_port);
    unix_sock= MYSQL_INVALID_SOCKET;
  }
#endif
  end_thr_alarm(0);      // Abort old alarms.

  /*
    First signal all threads that it's time to die
    This will give the threads some time to gracefully abort their
    statements and inform their clients that the server is about to die.
  */

  sql_print_information("Giving %d client threads a chance to die gracefully",
                        static_cast<int>(get_thread_count()));

  mutex_lock_all_shards(SHARDED(&LOCK_thread_count));

  Thread_iterator it= global_thread_list_begin();
  Thread_iterator end= global_thread_list_end();
  for (; it != end; ++it)
  {
    THD *tmp= *it;
    DBUG_PRINT("quit",("Informing thread %u that it's time to die",
                       tmp->thread_id()));
    /* We skip slave threads & scheduler on this first loop through. */
    if (tmp->slave_thread)
      continue;
    if (tmp->get_command() == COM_BINLOG_DUMP ||
        tmp->get_command() == COM_BINLOG_DUMP_GTID)
    {
      ++dump_thread_count;
      continue;
    }
    tmp->killed= THD::KILL_CONNECTION;
    DBUG_EXECUTE_IF("Check_dump_thread_is_alive",
                    {
                      DBUG_ASSERT(tmp->get_command() != COM_BINLOG_DUMP &&
                                  tmp->get_command() != COM_BINLOG_DUMP_GTID);
                    };);
    MYSQL_CALLBACK(thread_scheduler, post_kill_notification, (tmp));
    mysql_mutex_lock(&tmp->LOCK_thd_data);
    if (tmp->mysys_var)
    {
      tmp->mysys_var->abort=1;
      mysql_mutex_lock(&tmp->mysys_var->mutex);
      if (tmp->mysys_var->current_cond)
      {
        mysql_mutex_lock(tmp->mysys_var->current_mutex);
        mysql_cond_broadcast(tmp->mysys_var->current_cond);
        mysql_mutex_unlock(tmp->mysys_var->current_mutex);
      }
      mysql_mutex_unlock(&tmp->mysys_var->mutex);
    }
    mysql_mutex_unlock(&tmp->LOCK_thd_data);
  }
  mutex_unlock_all_shards(SHARDED(&LOCK_thread_count));

  // Cancel any timeouts for idle detached sessions.
  hhWheelTimer->cancelAll();

  Events::deinit();

  sql_print_information("Shutting down slave threads");
  end_slave();

  if (dump_thread_count)
  {
    /*
      Replication dump thread should be terminated after the clients are
      terminated. Wait for few more seconds for other sessions to end.
     */
    while (get_thread_count() > dump_thread_count && dump_thread_kill_retries)
    {
      sleep(1);
      dump_thread_kill_retries--;
    }
    mutex_lock_all_shards(SHARDED(&LOCK_thread_count));
    Thread_iterator it= global_thread_list_begin();
    Thread_iterator end= global_thread_list_end();
    for (; it != end; ++it)
    {
      THD *tmp= *it;
      DBUG_PRINT("quit",("Informing dump thread %u that it's time to die",
                         tmp->thread_id()));
      if (tmp->get_command() == COM_BINLOG_DUMP ||
          tmp->get_command() == COM_BINLOG_DUMP_GTID)
      {
        tmp->killed= THD::KILL_CONNECTION;
        MYSQL_CALLBACK(thread_scheduler, post_kill_notification, (tmp));
        mysql_mutex_lock(&tmp->LOCK_thd_data);
        if (tmp->mysys_var)
        {
          tmp->mysys_var->abort= 1;
          mysql_mutex_lock(&tmp->mysys_var->mutex);
          if (tmp->mysys_var->current_cond)
          {
            mysql_mutex_lock(tmp->mysys_var->current_mutex);
            mysql_cond_broadcast(tmp->mysys_var->current_cond);
            mysql_mutex_unlock(tmp->mysys_var->current_mutex);
          }
          mysql_mutex_unlock(&tmp->mysys_var->mutex);
        }
        mysql_mutex_unlock(&tmp->LOCK_thd_data);
      }
    }
    mutex_unlock_all_shards(SHARDED(&LOCK_thread_count));
  }
  if (get_thread_count() > 0)
    sleep(2);         // Give threads time to die

  /*
    There's a small window in which the dump threads may miss getting
    woken up. Kick them one more time to make sure because close_connection()
    below won't unblock them and the server will hang waiting for them to
    exit.
  */
  kill_all_dump_threads();

  /*
    Force remaining threads to die by closing the connection to the client
    This will ensure that threads that are waiting for a command from the
    client on a blocking read call are aborted.
  */

  sql_print_information("Forcefully disconnecting %d remaining clients",
                        static_cast<int>(get_thread_count()));

#ifndef __bsdi__ // Bug in BSDI kernel
  DBUG_PRINT("quit", ("Locking LOCK_thread_count"));
  mutex_lock_all_shards(SHARDED(&LOCK_thread_count));
  it= global_thread_list_begin();
  end= global_thread_list_end();
  for (; it != end; ++it)
  {
    THD *tmp= *it;
    if (tmp->vio_ok())
    {
      if (log_warnings)
        sql_print_warning(ER_DEFAULT(ER_FORCING_CLOSE),my_progname,
                          tmp->thread_id(),
                          (tmp->main_security_ctx.user ?
                           tmp->main_security_ctx.user : ""));
      close_connection(tmp);
    }
  }
  DBUG_PRINT("quit",("Unlocking LOCK_thread_count"));
  mutex_unlock_all_shards(SHARDED(&LOCK_thread_count));
#endif // Bug in BSDI kernel

  /* All threads has now been aborted */
  DBUG_PRINT("quit",("Waiting for threads to die (count=%u)",
                     get_thread_count()));
  mysql_mutex_lock(&LOCK_thread_count);
  while (get_thread_count() > 0)
  {
    mysql_cond_wait(&COND_thread_count, &LOCK_thread_count);
    DBUG_PRINT("quit", ("One thread died (count=%u)", get_thread_count()));
  }

  /*
    Connection threads might take a little while to go down after removing from
    global thread list. Give it some time.
  */
  while (connection_count > 0)
  {
    mysql_cond_wait(&COND_connection_count, &LOCK_thread_count);
  }
  mysql_mutex_unlock(&LOCK_thread_count);

  /*
    Connection threads might take a little while to go down after removing from
    global thread list. Give it some time.
  */
  mysql_mutex_lock(&LOCK_connection_count);
  while (connection_count > 0)
  {
    mysql_cond_wait(&COND_connection_count, &LOCK_connection_count);
  }
  mysql_mutex_unlock(&LOCK_connection_count);

  DBUG_EXECUTE_IF("commit_on_shutdown_testing", {
    THD *thd = new THD();
    thd->thread_stack = reinterpret_cast<char *>(&(thd));
    thd->store_globals();
    const char act[] = "now signal shutdown_started";
    DBUG_ASSERT(opt_debug_sync_timeout > 0);
    DBUG_ASSERT(!debug_sync_set_action(current_thd, STRING_WITH_LEN(act)));
    thd->restore_globals();
    delete thd;
  });
  close_active_mi();
  DBUG_PRINT("quit",("close_connections thread"));
  DBUG_VOID_RETURN;
}

static void plugin_shutdown_prework()
{
  if (enable_raft_plugin)
  {
    sql_print_information("Sending shutdown call to raft plugin");
    RUN_HOOK_STRICT(raft_replication, before_shutdown, (nullptr));
  }
}

#ifdef HAVE_CLOSE_SERVER_SOCK
static void close_socket(MYSQL_SOCKET *sock, const char *info)
{
  DBUG_ENTER("close_socket");
  MYSQL_SOCKET tmp_sock = *sock;
  if (mysql_socket_getfd(tmp_sock) != INVALID_SOCKET)
  {
    *sock = MYSQL_INVALID_SOCKET;
    DBUG_PRINT("info", ("calling shutdown on %s socket", info));
    (void) mysql_socket_shutdown(tmp_sock, SHUT_RDWR);
  }
  DBUG_VOID_RETURN;
}
#endif

static void close_server_sock()
{
#ifdef HAVE_CLOSE_SERVER_SOCK
  DBUG_ENTER("close_server_sock");

  close_socket(&ip_sock, "TCP/IP");
#if defined(HAVE_SOREUSEPORT)
  if (gl_socket_sharding) {
    for(uint sid = 1; sid < num_sharded_sockets; sid++)
      close_socket(&ip_sharded_sockets[sid], "SHARDED TCP/IP");
  }
#endif
  close_socket(&admin_ip_sock, "TCP/IP");
  close_socket(&unix_sock, "unix/IP");

  if (mysql_socket_getfd(unix_sock) != INVALID_SOCKET)
  {
    (void) unlink(mysqld_unix_port);
  }
  DBUG_VOID_RETURN;
#endif
}

#endif /*EMBEDDED_LIBRARY*/


void kill_mysql(void)
{
  DBUG_ENTER("kill_mysql");

#if defined(SIGNALS_DONT_BREAK_READ) && !defined(EMBEDDED_LIBRARY)
  abort_loop=1;         // Break connection loops
  close_server_sock();        // Force accept to wake up
#endif

#if defined(__WIN__)
#if !defined(EMBEDDED_LIBRARY)
  {
    if (!SetEvent(hEventShutdown))
    {
      DBUG_PRINT("error",("Got error: %ld from SetEvent",GetLastError()));
    }
    /*
      or:
      HANDLE hEvent=OpenEvent(0, FALSE, "MySqlShutdown");
      SetEvent(hEventShutdown);
      CloseHandle(hEvent);
    */
  }
#endif
#elif defined(HAVE_PTHREAD_KILL)
  if (pthread_kill(signal_thread, MYSQL_KILL_SIGNAL))
  {
    DBUG_PRINT("error",("Got error %d from pthread_kill",errno)); /* purecov: inspected */
  }
#elif !defined(SIGNALS_DONT_BREAK_READ)
  kill(current_pid, MYSQL_KILL_SIGNAL);
#endif
  DBUG_PRINT("quit",("After pthread_kill"));
  shutdown_in_progress=1;     // Safety if kill didn't work
#ifdef SIGNALS_DONT_BREAK_READ
  if (!kill_in_progress)
  {
    pthread_t tmp;
    int error;
    abort_loop=1;
    if ((error= mysql_thread_create(0, /* Not instrumented */
                                    &tmp, &connection_attrib,
                                    kill_server_thread, (void*) 0)))
      sql_print_error("Can't create thread to kill server (errno= %d).", error);
  }
#endif
  DBUG_VOID_RETURN;
}

/**
  Force server down. Kill all connections and threads and exit.

  @param  sig_ptr       Signal number that caused kill_server to be called.

  @note
    A signal number of 0 mean that the function was not called
    from a signal handler and there is thus no signal to block
    or stop, we just want to kill the server.
*/

#if !defined(__WIN__)
static void *kill_server(void *sig_ptr)
#define RETURN_FROM_KILL_SERVER return 0
#else
static void __cdecl kill_server(int sig_ptr)
#define RETURN_FROM_KILL_SERVER return
#endif
{
  DBUG_ENTER("kill_server");
#ifndef EMBEDDED_LIBRARY
  int sig=(int) (long) sig_ptr;     // This is passed a int
  // if there is a signal during the kill in progress, ignore the other
  if (kill_in_progress)       // Safety
  {
    DBUG_LEAVE;
    RETURN_FROM_KILL_SERVER;
  }
  kill_in_progress=TRUE;
  abort_loop=1;         // This should be set
  if (sig != 0) // 0 is not a valid signal number
    my_sigset(sig, SIG_IGN);                    /* purify inspected */
  if (sig == MYSQL_KILL_SIGNAL || sig == 0)
    sql_print_information(ER_DEFAULT(ER_NORMAL_SHUTDOWN),my_progname);
  else
    sql_print_error(ER_DEFAULT(ER_GOT_SIGNAL),my_progname,sig); /* purecov: inspected */

#if defined(HAVE_SMEM) && defined(__WIN__)
  /*
   Send event to smem_event_connect_request for aborting
   */
  if (opt_enable_shared_memory)
  {
    if (!SetEvent(smem_event_connect_request))
    {
      DBUG_PRINT("error",
                 ("Got error: %ld from SetEvent of smem_event_connect_request",
                  GetLastError()));
    }
  }
#endif

  /* Craete shutdown file */
  create_shutdown_file();

  plugin_shutdown_prework();

  // case: block all writes by setting read_only=1 and super_read_only=1
  if (set_read_only_on_shutdown)
  {
    THD *thd= new THD;
    thd->thread_stack= reinterpret_cast<char *>(&(thd));
    thd->store_globals();

    sql_print_information("Setting read_only=1 during shutdown");
    mysql_mutex_lock(&LOCK_global_system_variables);
    read_only= super_read_only= true;
    if (fix_read_only(nullptr, thd, OPT_GLOBAL))
      sql_print_error("Setting read_only=1 failed during shutdown");
    else
      sql_print_information("Successfully set read_only=1 during shutdown");
    mysql_mutex_unlock(&LOCK_global_system_variables);

    DBUG_EXECUTE_IF("after_shutdown_read_only", {
      const char act[]= "now signal signal.reached wait_for signal.done";
      DBUG_ASSERT(opt_debug_sync_timeout > 0);
      DBUG_ASSERT(!debug_sync_set_action(thd, STRING_WITH_LEN(act)));
    });

    thd->restore_globals();
    delete thd;
  }

  close_connections();
  if (sig != MYSQL_KILL_SIGNAL &&
      sig != 0)
    unireg_abort(1);        /* purecov: inspected */
  else
    unireg_end();

  /* purecov: begin deadcode */
  DBUG_LEAVE;                                   // Must match DBUG_ENTER()
  my_thread_end();
  pthread_exit(0);
  /* purecov: end */

  RETURN_FROM_KILL_SERVER;                      // Avoid compiler warnings

#else /* EMBEDDED_LIBRARY*/

  DBUG_LEAVE;
  RETURN_FROM_KILL_SERVER;

#endif /* EMBEDDED_LIBRARY */
}


#if defined(USE_ONE_SIGNAL_HAND)
pthread_handler_t kill_server_thread(void *arg MY_ATTRIBUTE((unused)))
{
  my_thread_init();       // Initialize new thread
  kill_server(0);
  /* purecov: begin deadcode */
  my_thread_end();
  pthread_exit(0);
  return 0;
  /* purecov: end */
}
#endif


extern "C" sig_handler print_signal_warning(int sig)
{
  if (log_warnings)
    sql_print_warning("Got signal %d from thread %u", sig,my_thread_id());
#ifdef SIGNAL_HANDLER_RESET_ON_DELIVERY
  my_sigset(sig,print_signal_warning);    /* int. thread system calls */
#endif
#if !defined(__WIN__)
  if (sig == SIGALRM)
    alarm(2);         /* reschedule alarm */
#endif
}

#ifndef EMBEDDED_LIBRARY

static void init_error_log_mutex()
{
  mysql_mutex_init(key_LOCK_error_log, &LOCK_error_log, MY_MUTEX_INIT_FAST);
}


static void clean_up_error_log_mutex()
{
  mysql_mutex_destroy(&LOCK_error_log);
}


/**
  cleanup all memory and end program nicely.

    If SIGNALS_DONT_BREAK_READ is defined, this function is called
    by the main thread. To get MySQL to shut down nicely in this case
    (Mac OS X) we have to call exit() instead if pthread_exit().

  @note
    This function never returns.
*/
void unireg_end(void)
{
  clean_up(1);
  my_thread_end();
#if defined(SIGNALS_DONT_BREAK_READ)
  exit(0);
#else
  pthread_exit(0);        // Exit is in main thread
#endif
}


extern "C" void unireg_abort(int exit_code)
{
  DBUG_ENTER("unireg_abort");

  if (opt_help)
    usage();
  if (exit_code)
    sql_print_error("Aborting\n");
  clean_up(!opt_help && (exit_code || !opt_bootstrap)); /* purecov: inspected */
  DBUG_PRINT("quit",("done with cleanup in unireg_abort"));
  mysqld_exit(exit_code);
}

#ifdef TARGET_OS_LINUX
static void cleanup_cachedev(void)
{
  pid_t pid = getpid();

  if (cachedev_enabled) {
    ioctl(cachedev_fd, FLASHCACHEDELWHITELIST, &pid);
    close(cachedev_fd);
    cachedev_fd = -1;
  }
}
#endif /* TARGET_OS_LINUX */

static void mysqld_exit(int exit_code)
{
  /*
    Important note: we wait for the signal thread to end,
    but if a kill -15 signal was sent, the signal thread did
    spawn the kill_server_thread thread, which is running concurrently.
  */
  wait_for_signal_thread_to_end();
  mysql_audit_finalize();
  clean_up_mutexes();
  clean_up_error_log_mutex();
#ifdef WITH_PERFSCHEMA_STORAGE_ENGINE
  shutdown_performance_schema();
#endif
  my_end(opt_endinfo ? MY_CHECK_ERROR | MY_GIVE_INFO : 0);

#ifdef TARGET_OS_LINUX
  cleanup_cachedev();
#endif /* TARGET_OS_LINUX */

  exit(exit_code); /* purecov: inspected */
}

#endif /* !EMBEDDED_LIBRARY */

/**
   GTID cleanup destroys objects and reset their pointer.
   Function is reentrant.
*/
void gtid_server_cleanup()
{
  delete gtid_state;
  delete global_sid_map;
  delete global_sid_lock;
  global_sid_lock= NULL;
  global_sid_map= NULL;
  gtid_state= NULL;
}

/**
   GTID initialization.

   @return true if allocation does not succeed
           false if OK
*/
bool gtid_server_init()
{
  bool res=
    (!(global_sid_lock= new Checkable_rwlock) ||
     !(global_sid_map= new Sid_map(global_sid_lock)) ||
     !(gtid_state= new Gtid_state(global_sid_lock, global_sid_map)));
  if (res)
  {
    gtid_server_cleanup();
  }
  return res;
}


void clean_up(bool print_message)
{
  DBUG_PRINT("exit",("clean_up"));
  if (cleanup_done++)
    return; /* purecov: inspected */

#ifndef EMBEDDED_LIBRARY
    Srv_session::module_deinit();
#endif

  delete db_ac;
  stop_handle_manager();

  release_ddl_log();

  memcached_shutdown();

  free_latency_histogram_sysvars(latency_histogram_binlog_fsync);
  free_counter_histogram_sysvars(histogram_binlog_group_commit_var);

  /*
    make sure that handlers finish up
    what they have that is dependent on the binlog
  */
  if ((opt_help == 0) || (opt_verbose > 0))
    sql_print_information("Binlog end");
  ha_binlog_end(current_thd);

  logger.cleanup_base();

  injector::free_instance();
  mysql_bin_log.cleanup();
  gtid_server_cleanup();

#ifdef HAVE_REPLICATION
  if (use_slave_mask)
    bitmap_free(&slave_error_mask);
#endif
  my_tz_free();
  my_dboptions_cache_free();
  ignore_db_dirs_free();
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  servers_free(1);
  acl_free(1);
  grant_free();
#endif
  query_cache_destroy();
  hostname_cache_free();
  item_user_lock_free();
  lex_free();       /* Free some memory */
  item_create_cleanup();
  if (!opt_noacl)
  {
#ifdef HAVE_DLOPEN
    udf_free();
#endif
  }
  native_procedure_destroy();
  table_def_start_shutdown();
  plugin_shutdown();
  ha_end();
  if (tc_log)
    tc_log->close();
  delegates_destroy();
  xid_cache_free();
  table_def_free();
  mdl_destroy();
  key_caches.delete_elements(free_key_cache);
  multi_keycache_free();
  free_status_vars();
  end_thr_alarm(1);     /* Free allocated memory */
  my_free_open_file_info();
  if (defaults_argv)
    free_defaults(defaults_argv);
  free_tmpdir(&mysql_tmpdir_list);
  my_free(opt_bin_logname);

  if (should_free_opt_apply_logname && opt_apply_logname)
  {
    my_free(opt_apply_logname);
    opt_apply_logname= nullptr;
    should_free_opt_apply_logname= false;
  }

  if (should_free_opt_applylog_index_name && opt_applylog_index_name)
  {
    my_free(opt_applylog_index_name);
    opt_applylog_index_name= nullptr;
    should_free_opt_applylog_index_name= false;
  }

  bitmap_free(&temp_pool);
  free_global_table_stats();
  free_global_db_stats();
  free_max_user_conn();
  free_global_sql_plans();
  free_global_active_sql();
  free_global_sql_findings();
  free_global_write_statistics();
  free_global_write_throttling_rules();
  free_global_tx_size_histogram();

#ifndef NO_EMBEDDED_ACCESS_CHECKS
  free_aux_user_table_stats();
#endif
  destroy_names();
#ifdef HAVE_REPLICATION
  end_slave_list();
  free_compressed_event_cache();
  destroy_semi_sync_last_acked();
#endif
  delete binlog_filter;
  delete rpl_filter;
  end_ssl();
  vio_end();
  my_regex_end();
#if defined(ENABLED_DEBUG_SYNC)
  /* End the debug sync facility. See debug_sync.cc. */
  debug_sync_end();
#endif /* defined(ENABLED_DEBUG_SYNC) */

  delete_pid_file(MYF(0));

  if (print_message && my_default_lc_messages && server_start_time)
    sql_print_information(ER_DEFAULT(ER_SHUTDOWN_COMPLETE),my_progname);
  cleanup_errmsgs();
  MYSQL_CALLBACK(thread_scheduler, end, ());
  mysql_client_plugin_deinit();
  finish_client_errs();
  deinit_errmessage(); // finish server errs
  DBUG_PRINT("quit", ("Error messages freed"));
  /* Tell main we are ready */
  logger.cleanup_end();
  my_atomic_rwlock_destroy(&global_query_id_lock);
  my_atomic_rwlock_destroy(&thread_running_lock);
  my_atomic_rwlock_destroy(&write_query_running_lock);
  free_charsets();
  mysql_mutex_lock(&LOCK_thread_count);
  DBUG_PRINT("quit", ("got thread count lock"));
  ready_to_exit=1;
  /* do the broadcast inside the lock to ensure that my_end() is not called */
  mysql_cond_broadcast(&COND_thread_count);
  mysql_mutex_unlock(&LOCK_thread_count);
  sys_var_end();
  delete_global_thread_list();

  my_free(const_cast<char*>(log_bin_basename));
  my_free(const_cast<char*>(log_bin_index));
#ifndef EMBEDDED_LIBRARY
  my_free(const_cast<char*>(relay_log_basename));
  my_free(const_cast<char*>(relay_log_index));
#endif
  free_list(opt_plugin_load_list_ptr);

  delete gap_lock_exceptions;
  delete legacy_user_name_pattern;
  delete admin_users_list_regex;

  if (THR_THD)
    (void) pthread_key_delete(THR_THD);

  if (THR_MALLOC)
    (void) pthread_key_delete(THR_MALLOC);

#ifdef HAVE_MY_TIMER
  hhWheelTimer.reset(nullptr);

  if (have_statement_timeout == SHOW_OPTION_YES)
    my_timer_deinitialize();

  have_statement_timeout= SHOW_OPTION_DISABLED;
#endif

#if !defined(EMBEDDED_LIBRARY)
  if (separate_conn_handling_thread) {
    for (auto ii : sockets_list) {
      delete ii;
    }
  }
#endif

  /*
    The following lines may never be executed as the main thread may have
    killed us
  */
  DBUG_PRINT("quit", ("done with cleanup"));
} /* clean_up */


#ifndef EMBEDDED_LIBRARY

/**
  This is mainly needed when running with purify, but it's still nice to
  know that all child threads have died when mysqld exits.
*/
static void wait_for_signal_thread_to_end()
{
  uint i;
  /*
    Wait up to 10 seconds for signal thread to die. We use this mainly to
    avoid getting warnings that my_thread_end has not been called
  */
  for (i= 0 ; i < 100 && signal_thread_in_use; i++)
  {
    if (pthread_kill(signal_thread, MYSQL_KILL_SIGNAL) != ESRCH)
      break;
    my_sleep(100);        // Give it time to die
  }
}


static void clean_up_mutexes()
{
  mysql_rwlock_destroy(&LOCK_grant);
  mysql_mutex_destroy(&LOCK_thread_created);
  mysql_mutex_destroy(&LOCK_thread_count);
#ifdef SHARDED_LOCKING
  if (sharded_locks_init) {
    for (uint ii = 0; ii < num_sharded_locks; ii++) {
      mysql_mutex_destroy(&LOCK_thread_count_sharded[ii]);
      mysql_mutex_destroy(&LOCK_thd_remove_sharded[ii]);
    }
  }
#endif
  mysql_mutex_destroy(&LOCK_log_throttle_qni);
  mysql_mutex_destroy(&LOCK_log_throttle_legacy);
  mysql_mutex_destroy(&LOCK_log_throttle_ddl);
  mysql_mutex_destroy(&LOCK_log_throttle_sbr_unsafe);
  mysql_mutex_destroy(&LOCK_status);
  mysql_mutex_destroy(&LOCK_delayed_insert);
  mysql_mutex_destroy(&LOCK_delayed_status);
  mysql_mutex_destroy(&LOCK_delayed_create);
  mysql_mutex_destroy(&LOCK_manager);
  mysql_mutex_destroy(&LOCK_slave_stats_daemon);
  mysql_mutex_destroy(&LOCK_crypt);
  mysql_mutex_destroy(&LOCK_user_conn);
  mysql_mutex_destroy(&LOCK_connection_count);
  mysql_mutex_destroy(&LOCK_global_table_stats);
  mysql_mutex_destroy(&LOCK_global_sql_stats);
  mysql_mutex_destroy(&LOCK_global_sql_plans);
  mysql_mutex_destroy(&LOCK_global_active_sql);
  mysql_mutex_destroy(&LOCK_global_sql_findings);
  mysql_rwlock_destroy(&LOCK_sql_stats_snapshot);
  mysql_mutex_destroy(&LOCK_global_write_statistics);
  mysql_mutex_destroy(&LOCK_global_write_throttling_rules);
  mysql_mutex_destroy(&LOCK_global_write_throttling_log);
  mysql_mutex_destroy(&LOCK_global_tx_size_histogram);
  mysql_mutex_destroy(&LOCK_global_write_stat_histogram);
  mysql_mutex_destroy(&LOCK_replication_lag_auto_throttling);
#ifdef HAVE_OPENSSL
  mysql_mutex_destroy(&LOCK_des_key_file);
  mysql_rwlock_destroy(&LOCK_use_ssl);
#endif
  mysql_mutex_destroy(&LOCK_active_mi);
  mysql_rwlock_destroy(&LOCK_sys_init_connect);
  mysql_rwlock_destroy(&LOCK_sys_init_slave);
  mysql_mutex_destroy(&LOCK_global_system_variables);
  mysql_rwlock_destroy(&LOCK_column_statistics);
  mysql_rwlock_destroy(&LOCK_system_variables_hash);
  mysql_mutex_destroy(&LOCK_uuid_generator);
  mysql_mutex_destroy(&LOCK_sql_rand);
  mysql_mutex_destroy(&LOCK_prepared_stmt_count);
  mysql_mutex_destroy(&LOCK_sql_slave_skip_counter);
  mysql_mutex_destroy(&LOCK_slave_net_timeout);
  mysql_mutex_destroy(&LOCK_error_messages);
  mysql_cond_destroy(&COND_thread_count);
  mysql_mutex_destroy(&LOCK_thd_remove);
  mysql_cond_destroy(&COND_thread_cache);
  mysql_cond_destroy(&COND_flush_thread_cache);
  mysql_cond_destroy(&COND_manager);
  mysql_cond_destroy(&COND_slave_stats_daemon);
  mysql_cond_destroy(&COND_connection_count);
#ifdef _WIN32
  mysql_rwlock_destroy(&LOCK_named_pipe_full_access_group);
#endif
}
#endif /*EMBEDDED_LIBRARY*/


/****************************************************************************
** Init IP and UNIX socket
****************************************************************************/


/**
  MY_BIND_ALL_ADDRESSES defines a special value for the bind-address option,
  which means that the server should listen to all available network addresses,
  both IPv6 (if available) and IPv4.

  Basically, this value instructs the server to make an attempt to bind the
  server socket to '::' address, and rollback to '0.0.0.0' if the attempt fails.
*/
const char *MY_BIND_ALL_ADDRESSES= "*";


#ifndef EMBEDDED_LIBRARY
static void set_ports()
{
  char  *env;
  if (!mysqld_port && !opt_disable_networking)
  {         // Get port if not from commandline
    mysqld_port= MYSQL_PORT;

    /*
      if builder specifically requested a default port, use that
      (even if it coincides with our factory default).
      only if they didn't do we check /etc/services (and, failing
      on that, fall back to the factory default of 3306).
      either default can be overridden by the environment variable
      MYSQL_TCP_PORT, which in turn can be overridden with command
      line options.
    */

#if MYSQL_PORT_DEFAULT == 0
    struct  servent *serv_ptr;
    if ((serv_ptr= getservbyname("mysql", "tcp")))
      mysqld_port= ntohs((u_short) serv_ptr->s_port); /* purecov: inspected */
#endif
    if ((env = getenv("MYSQL_TCP_PORT")))
      mysqld_port= (uint) atoi(env);    /* purecov: inspected */
  }
  if (!mysqld_unix_port)
  {
#ifdef __WIN__
    mysqld_unix_port= (char*) MYSQL_NAMEDPIPE;
#else
    mysqld_unix_port= (char*) MYSQL_UNIX_ADDR;
#endif
    if ((env = getenv("MYSQL_UNIX_PORT")))
      mysqld_unix_port= env;      /* purecov: inspected */
  }
}

/* Change to run as another user if started with --user */

static struct passwd *check_user(const char *user)
{
#if !defined(__WIN__)
  struct passwd *tmp_user_info;
  uid_t user_id= geteuid();

  // Don't bother if we aren't superuser
  if (user_id)
  {
    if (user)
    {
      /* Don't give a warning, if real user is same as given with --user */
      /* purecov: begin tested */
      tmp_user_info= getpwnam(user);
      if ((!tmp_user_info || user_id != tmp_user_info->pw_uid) &&
    log_warnings)
        sql_print_warning(
                    "One can only use the --user switch if running as root\n");
      /* purecov: end */
    }
    return NULL;
  }
  if (!user)
  {
    if (!opt_bootstrap && !opt_help)
    {
      sql_print_error("Fatal error: Please read \"Security\" section of the manual to find out how to run mysqld as root!\n");
      unireg_abort(1);
    }
    return NULL;
  }
  /* purecov: begin tested */
  if (!strcmp(user,"root"))
    return NULL;                        // Avoid problem with dynamic libraries

  if (!(tmp_user_info= getpwnam(user)))
  {
    // Allow a numeric uid to be used
    const char *pos;
    for (pos= user; my_isdigit(mysqld_charset,*pos); pos++) ;
    if (*pos)                                   // Not numeric id
      goto err;
    if (!(tmp_user_info= getpwuid(atoi(user))))
      goto err;
  }
  return tmp_user_info;
  /* purecov: end */

err:
  sql_print_error("Fatal error: Can't change to run as user '%s' ;  Please check that the user exists!\n",user);
  unireg_abort(1);

#endif
  return NULL;
}

static void set_user(const char *user, struct passwd *user_info_arg)
{
  /* purecov: begin tested */
#if !defined(__WIN__)
  DBUG_ASSERT(user_info_arg != 0);
#ifdef HAVE_INITGROUPS
  /*
    We can get a SIGSEGV when calling initgroups() on some systems when NSS
    is configured to use LDAP and the server is statically linked.  We set
    calling_initgroups as a flag to the SIGSEGV handler that is then used to
    output a specific message to help the user resolve this problem.
  */
  calling_initgroups= 1;
  initgroups((char*) user, user_info_arg->pw_gid);
  calling_initgroups= 0;
#endif
  if (setgid(user_info_arg->pw_gid) == -1)
  {
    sql_perror("setgid");
    unireg_abort(1);
  }
  if (setresuid(user_info_arg->pw_uid, user_info_arg->pw_uid, -1) == -1)
  {
    sql_perror("setuid");
    unireg_abort(1);
  }
#endif

#ifdef PR_SET_DUMPABLE
  if (opt_core_file)
  {
    /* inform kernel that process is dumpable */
    (void) prctl(PR_SET_DUMPABLE, 1);
  }
#endif
  /* purecov: end */
}


static void set_effective_user(struct passwd *user_info_arg)
{
#if !defined(__WIN__)
  DBUG_ASSERT(user_info_arg != 0);
  if (setregid((gid_t)-1, user_info_arg->pw_gid) == -1)
  {
    sql_perror("setregid");
    unireg_abort(1);
  }
  if (setreuid((uid_t)-1, user_info_arg->pw_uid) == -1)
  {
    sql_perror("setreuid");
    unireg_abort(1);
  }
#endif
}


/** Change root user if started with @c --chroot . */
static void set_root(const char *path)
{
#if !defined(__WIN__)
  if (chroot(path) == -1)
  {
    sql_perror("chroot");
    unireg_abort(1);
  }
  my_setwd("/", MYF(0));
#endif
}


static MYSQL_SOCKET create_socket(const struct addrinfo *addrinfo_list,
                                  int addr_family,
                                  struct addrinfo **use_addrinfo)
{
  MYSQL_SOCKET sock= MYSQL_INVALID_SOCKET;

  for (const struct addrinfo *cur_ai= addrinfo_list; cur_ai != NULL;
       cur_ai= cur_ai->ai_next)
  {
    if (cur_ai->ai_family != addr_family)
      continue;

    sock= mysql_socket_socket(key_socket_tcpip, cur_ai->ai_family,
                              cur_ai->ai_socktype, cur_ai->ai_protocol);

    char ip_addr[INET6_ADDRSTRLEN];

    if (vio_getnameinfo(cur_ai->ai_addr, ip_addr, sizeof (ip_addr),
                        NULL, 0, NI_NUMERICHOST))
    {
      ip_addr[0]= 0;
    }

    if (mysql_socket_getfd(sock) == INVALID_SOCKET)
    {
      sql_print_error("Failed to create a socket for %s '%s': errno: %d.",
                      (addr_family == AF_INET) ? "IPv4" : "IPv6",
                      (const char *) ip_addr,
                      (int) socket_errno);

      continue;
    }

    sql_print_information("Server socket created on IP: '%s'.",
                          (const char *) ip_addr);

    *use_addrinfo= (struct addrinfo *)cur_ai;
    return sock;
  }

  return MYSQL_INVALID_SOCKET;
}

/**
   Activate usage of a tcp port
*/
static MYSQL_SOCKET activate_tcp_port(uint port, my_bool socket_sharding= 0)
{
  int arg;
  int   ret;
  uint  waited;
  uint  this_wait;
  uint  retry;
  char port_buf[NI_MAXSERV];
  LINT_INIT(ret);

  struct addrinfo *ai;
  struct addrinfo hints;

  const char *bind_address_str= NULL;
  const char *ipv6_all_addresses= "::";
  const char *ipv4_all_addresses= "0.0.0.0";

  MYSQL_SOCKET tmp_ip_sock = MYSQL_INVALID_SOCKET;

  sql_print_information("Server hostname (bind-address): '%s'; port: %d",
                        my_bind_addr_str, port);

  // Get list of IP-addresses associated with the bind-address.

  memset(&hints, 0, sizeof (hints));
  hints.ai_flags= AI_PASSIVE;
  hints.ai_socktype= SOCK_STREAM;
  hints.ai_family= AF_UNSPEC;

  my_snprintf(port_buf, NI_MAXSERV, "%d", port);

  if (strcasecmp(my_bind_addr_str, MY_BIND_ALL_ADDRESSES) == 0)
  {
    /*
      That's the case when bind-address is set to a special value ('*'),
      meaning "bind to all available IP addresses". If the box supports
      the IPv6 stack, that means binding to '::'. If only IPv4 is available,
      bind to '0.0.0.0'.
    */

    bool ipv6_available= false;

    if (!getaddrinfo(ipv6_all_addresses, port_buf, &hints, &ai))
    {
      /*
        IPv6 might be available (the system might be able to resolve an IPv6
        address, but not be able to create an IPv6-socket). Try to create a
        dummy IPv6-socket. Do not instrument that socket by P_S.
      */

      MYSQL_SOCKET s= mysql_socket_socket(0, AF_INET6, SOCK_STREAM, 0);

      ipv6_available= mysql_socket_getfd(s) != INVALID_SOCKET;

      mysql_socket_close(s);
    }

    if (ipv6_available)
    {
      sql_print_information("IPv6 is available.");

      // Address info (ai) for IPv6 address is already set.

      bind_address_str= ipv6_all_addresses;
    }
    else
    {
      sql_print_information("IPv6 is not available.");

      // Retrieve address info (ai) for IPv4 address.

      if (getaddrinfo(ipv4_all_addresses, port_buf, &hints, &ai))
      {
        sql_perror(ER_DEFAULT(ER_IPSOCK_ERROR));
        sql_print_error("Can't start server: cannot resolve hostname!");
        unireg_abort(1);
      }

      bind_address_str= ipv4_all_addresses;
    }
  }
  else
  {
    if (getaddrinfo(my_bind_addr_str, port_buf, &hints, &ai))
    {
      sql_perror(ER_DEFAULT(ER_IPSOCK_ERROR));  /* purecov: tested */
      sql_print_error("Can't start server: cannot resolve hostname!");
      unireg_abort(1);                          /* purecov: tested */
    }

    bind_address_str= my_bind_addr_str;
  }

  // Log all the IP-addresses.
  for (struct addrinfo *cur_ai= ai; cur_ai != NULL; cur_ai= cur_ai->ai_next)
  {
    char ip_addr[INET6_ADDRSTRLEN];

    if (vio_getnameinfo(cur_ai->ai_addr, ip_addr, sizeof (ip_addr),
                        NULL, 0, NI_NUMERICHOST))
    {
      sql_print_error("Fails to print out IP-address.");
      continue;
    }

    sql_print_information("  - '%s' resolves to '%s';",
                          bind_address_str, ip_addr);
  }

  /*
    If the 'bind-address' option specifies the hostname, which resolves to
    multiple IP-address, use the following rule:
    - if there are IPv4-addresses, use the first IPv4-address
    returned by getaddrinfo();
    - if there are IPv6-addresses, use the first IPv6-address
    returned by getaddrinfo();
  */

  struct addrinfo *a = NULL;
  tmp_ip_sock= create_socket(ai, AF_INET, &a);

  if (mysql_socket_getfd(tmp_ip_sock) == INVALID_SOCKET)
    tmp_ip_sock= create_socket(ai, AF_INET6, &a);

  // Report user-error if we failed to create a socket.
  if (mysql_socket_getfd(tmp_ip_sock) == INVALID_SOCKET)
  {
    sql_perror(ER_DEFAULT(ER_IPSOCK_ERROR));  /* purecov: tested */
    unireg_abort(1);                          /* purecov: tested */
  }

  mysql_socket_set_thread_owner(tmp_ip_sock);

#ifndef __WIN__
  /*
    We should not use SO_REUSEADDR on windows as this would enable a
    user to open two mysqld servers with the same TCP/IP port.
  */
  arg= 1;
  (void) mysql_socket_setsockopt(tmp_ip_sock, SOL_SOCKET, SO_REUSEADDR,
                                 (char*)&arg,sizeof(arg));
#endif /* __WIN__ */

#if defined(HAVE_SOREUSEPORT)
  if (socket_sharding) {
    int  reuseport = 1;
    (void)mysql_socket_setsockopt(tmp_ip_sock, SOL_SOCKET, SO_REUSEPORT,
      (const void *) &reuseport, sizeof(int));
  }
#endif

#ifdef IPV6_V6ONLY
   /*
     For interoperability with older clients, IPv6 socket should
     listen on both IPv6 and IPv4 wildcard addresses.
     Turn off IPV6_V6ONLY option.

     NOTE: this will work starting from Windows Vista only.
     On Windows XP dual stack is not available, so it will not
     listen on the corresponding IPv4-address.
   */
  if (a->ai_family == AF_INET6)
  {
    arg= 0;

    if (mysql_socket_setsockopt(tmp_ip_sock, IPPROTO_IPV6, IPV6_V6ONLY,
                                (char *) &arg, sizeof (arg)))
    {
      sql_print_warning("Failed to reset IPV6_V6ONLY flag (error: %d). "
                        "The server will listen to IPv6 addresses only.",
                        (int) socket_errno);
    }
  }
#endif
  /*
    Sometimes the port is not released fast enough when stopping and
    restarting the server. This happens quite often with the test suite
    on busy Linux systems. Retry to bind the address at these intervals:
    Sleep intervals: 1, 2, 4,  6,  9, 13, 17, 22, ...
    Retry at second: 1, 3, 7, 13, 22, 35, 52, 74, ...
    Limit the sequence by mysqld_port_timeout (set --port-open-timeout=#).
  */
  for (waited= 0, retry= 1; ; retry++, waited+= this_wait)
  {
    if (((ret= mysql_socket_bind(tmp_ip_sock, a->ai_addr, a->ai_addrlen)) >= 0 )
        || (socket_errno != SOCKET_EADDRINUSE) ||
        (waited >= mysqld_port_timeout))
      break;
    sql_print_information("Retrying bind on TCP/IP port %u", port);
    this_wait= retry * retry / 3 + 1;
    sleep(this_wait);
  }
  freeaddrinfo(ai);
  if (ret < 0)
  {
    DBUG_PRINT("error",("Got error: %d from bind",socket_errno));
    sql_perror("Can't start server: Bind on TCP/IP port");
    sql_print_error("Do you already have another mysqld server running on port:"
                    " %d ?",port);
    unireg_abort(1);
  }
  if (mysql_socket_listen(tmp_ip_sock, (int)back_log) < 0)
  {
    sql_perror("Can't start server: listen() on TCP/IP port");
    sql_print_error("listen() on TCP/IP failed with error %d",
        socket_errno);
    unireg_abort(1);
  }
  return tmp_ip_sock;
}

static void network_init(void)
{
  int arg;
#ifdef HAVE_SYS_UN_H
  struct sockaddr_un  UNIXaddr;
#endif
  DBUG_ENTER("network_init");

  if (MYSQL_CALLBACK_ELSE(thread_scheduler, init, (), 0))
    unireg_abort(1);      /* purecov: inspected */

  set_ports();

  if (report_port == 0)
  {
    report_port= mysqld_port;
  }

#ifndef DBUG_OFF
  if (!opt_disable_networking)
    DBUG_ASSERT(report_port != 0);
#endif

  if (!opt_disable_networking && !opt_bootstrap)
  {
    if (mysqld_port != 0) {
      ip_sock= activate_tcp_port(mysqld_port, gl_socket_sharding);

#if defined(HAVE_SOREUSEPORT)
      if (gl_socket_sharding) {
        ip_sharded_sockets.resize(num_sharded_sockets);
        ip_sharded_sockets[0] = ip_sock;
        for(uint sid = 1; sid < num_sharded_sockets; sid++) {
          ip_sharded_sockets[sid] = activate_tcp_port(mysqld_port, 1);
        }
      }
#endif
    }

    if (mysqld_admin_port != 0)
      admin_ip_sock = activate_tcp_port(mysqld_admin_port);

    if (separate_conn_handling_thread)
    {
      sockets_list.resize(num_conn_handling_threads);
      for (uint ii = 0; ii < num_conn_handling_threads; ii++)
        sockets_list[ii] = new mpsc_queue_t<SOCKET_PACKET>;
    }
  }

#ifdef _WIN32
  /* create named pipe */
  if (Service.IsNT() && mysqld_unix_port[0] && !opt_bootstrap &&
      opt_enable_named_pipe)
  {
    hPipe= create_server_named_pipe(&psaPipeSecurity,
                global_system_variables.net_buffer_length,
                mysqld_unix_port,
                pipe_name,
                sizeof(pipe_name)-1,
                named_pipe_full_access_group);
    if (hPipe == INVALID_HANDLE_VALUE)
    {
      sql_print_error("Can't start server: failed to create named pipe");
      unireg_abort(1);
    }
  }
#endif

#if defined(HAVE_SYS_UN_H)
  /*
  ** Create the UNIX socket
  */
  if (mysqld_unix_port[0] && !opt_bootstrap)
  {
    DBUG_PRINT("general",("UNIX Socket is %s",mysqld_unix_port));

    if (strlen(mysqld_unix_port) > (sizeof(UNIXaddr.sun_path) - 1))
    {
      sql_print_error("The socket file path is too long (> %u): %s",
                      (uint) sizeof(UNIXaddr.sun_path) - 1, mysqld_unix_port);
      unireg_abort(1);
    }

    unix_sock= mysql_socket_socket(key_socket_unix, AF_UNIX, SOCK_STREAM, 0);

    if (mysql_socket_getfd(unix_sock) < 0)
    {
      sql_perror("Can't start server : UNIX Socket "); /* purecov: inspected */
      unireg_abort(1);        /* purecov: inspected */
    }

    mysql_socket_set_thread_owner(unix_sock);

    memset(&UNIXaddr, 0, sizeof(UNIXaddr));
    UNIXaddr.sun_family = AF_UNIX;
    strmov(UNIXaddr.sun_path, mysqld_unix_port);
    (void) unlink(mysqld_unix_port);
    arg= 1;
    (void) mysql_socket_setsockopt(unix_sock, SOL_SOCKET, SO_REUSEADDR, (char*)&arg,
          sizeof(arg));
    mode_t socket_umask = (mode_t) strtol(mysqld_socket_umask, nullptr, 8);
    umask(socket_umask);
    if (mysql_socket_bind(unix_sock, reinterpret_cast<struct sockaddr *> (&UNIXaddr),
                          sizeof(UNIXaddr)) < 0)
    {
      sql_perror("Can't start server : Bind on unix socket"); /* purecov: tested */
      sql_print_error("Do you already have another mysqld server running on socket: %s ?",mysqld_unix_port);
      unireg_abort(1);          /* purecov: tested */
    }
    umask(((~my_umask) & 0666));
#if defined(S_IFSOCK) && defined(SECURE_SOCKETS)
    (void) chmod(mysqld_unix_port,S_IFSOCK);  /* Fix solaris 2.6 bug */
#endif
    if (mysql_socket_listen(unix_sock, (int)back_log) < 0)
      sql_print_warning("listen() on Unix socket failed with error %d",
          socket_errno);
  }
#endif
  DBUG_PRINT("info",("server started"));
  DBUG_VOID_RETURN;
}

#endif /*!EMBEDDED_LIBRARY*/


#ifndef EMBEDDED_LIBRARY
/**
  Close a connection.

  @param thd        Thread handle.
  @param sql_errno  The error code to send before disconnect.

  @note
    For the connection that is doing shutdown, this is called twice
*/
void close_connection(THD *thd, uint sql_errno)
{
  DBUG_ENTER("close_connection");

  if (sql_errno)
    net_send_error(thd, NULL, sql_errno, ER_DEFAULT(sql_errno), NULL);

  thd->disconnect();

  MYSQL_CONNECTION_DONE((int) sql_errno, thd->thread_id());

  if (MYSQL_CONNECTION_DONE_ENABLED())
  {
    sleep(0); /* Workaround to avoid tailcall optimisation */
  }
  MYSQL_AUDIT_NOTIFY_CONNECTION_DISCONNECT(thd, sql_errno);
  DBUG_VOID_RETURN;
}
#endif /* EMBEDDED_LIBRARY */


/** Called when a thread is aborted. */
/* ARGSUSED */
extern "C" sig_handler end_thread_signal(int sig MY_ATTRIBUTE((unused)))
{
  THD *thd=current_thd;
  my_safe_printf_stderr("end_thread_signal %p", thd);
  if (thd && ! thd->bootstrap)
  {
    statistic_increment(killed_threads, &LOCK_status);
    MYSQL_CALLBACK(thread_scheduler, end_thread, (thd,0)); /* purecov: inspected */
  }
}


/*
  Rlease resources of the THD, prior to destruction.

  SYNOPSIS
    thd_release_resources()
    thd    Thread handler
*/

void thd_release_resources(THD *thd)
{
  thd->release_resources();
}

/*
  Decrease number of connections

  SYNOPSIS
    dec_connection_count()
*/

void dec_connection_count()
{
  mysql_mutex_assert_not_owner(&LOCK_thread_count);
  mysql_mutex_lock(&LOCK_thread_count);
  dec_connection_count_locked();
  mysql_mutex_unlock(&LOCK_thread_count);
}

/*
  Decrease number of connections. Assume lock.

  SYNOPSIS
    dec_connection_count_locked()
*/

void dec_connection_count_locked()
{
  mysql_mutex_assert_owner(&LOCK_thread_count);
  --connection_count;
  if (connection_count == 0) {
    mysql_cond_signal(&COND_connection_count);
    DBUG_PRINT("signal", ("Signaling COND_connection_count"));
  }
}


/**
  Delete the THD object.
 */
void destroy_thd(THD *thd)
{
  mutex_assert_not_owner_shard(SHARDED(&LOCK_thread_count), thd);
  delete thd;
}

#ifndef EMBEDDED_LIBRARY

static void mysql_pause() {
// this is not defined for other OS's/platforms.
// please implement this, if you want to use MPSC queue backoff for
// those platforms. _mm_pause is only defined on GCC.
#if defined(__GNUC__)
  _mm_pause();
#endif
}

// lockless mpsc backoff
static void do_backoff(int* num_backoffs) // num_backoffs is initialized to 0
{
  if (*num_backoffs < 10)
    mysql_pause();
  else if (*num_backoffs < 20)
    for (int i = 0; i != 50; i++)
      mysql_pause();
  else if (*num_backoffs < 22)
    pthread_yield();
  else if (*num_backoffs < 24)
    my_sleep(0);
  else if (*num_backoffs < 26)
    // 1 millisecond
    my_sleep(1000);
  else
    // 10 milliseconds
    my_sleep(10000);
  (*num_backoffs)++;
}
#endif

/**
  Block the current pthread for reuse by new connections.

  @retval false  Pthread was not blocked for reuse.
  @retval true   Pthread is to be reused by new connection.
                 (ie, caller should return, not abort with pthread_exit())
*/

static bool block_until_new_connection_halflock()
{
  mysql_mutex_assert_owner(&LOCK_thread_count);
  if (blocked_pthread_count < max_blocked_pthreads &&
      !abort_loop &&
      !kill_blocked_pthreads_flag.load(std::memory_order_relaxed))
  {
    /* Don't kill the pthread, just block it for reuse */
    DBUG_PRINT("info", ("Blocking pthread for reuse"));
    blocked_pthread_count++;

    DBUG_POP();
    /*
      mysys_var is bound to the physical thread,
      so make sure mysys_var->dbug is reset to a clean state
      before picking another session in the thread cache.
    */
    DBUG_ASSERT( ! _db_is_pushed_());

    // Block pthread
    bool kbpf = false;
    while (!(kbpf = kill_blocked_pthreads_flag.load(std::memory_order_acquire))
      && (!abort_loop) && (!wake_pthread)) {
      mysql_cond_wait(&COND_thread_cache, &LOCK_thread_count);
    }

    blocked_pthread_count--;
    if (kbpf)
      mysql_cond_signal(&COND_flush_thread_cache);
    if (wake_pthread)
    {
      THD *thd;
      wake_pthread--;
      DBUG_ASSERT(!waiting_thd_list->empty());
      thd= waiting_thd_list->front();
      waiting_thd_list->pop_front();
      mysql_mutex_unlock(&LOCK_thread_count);
      DBUG_PRINT("info", ("waiting_thd_list->pop %p", thd));

      thd->thread_stack= (char*) &thd;          // For store_globals
      (void) thd->store_globals();

#ifdef HAVE_PSI_THREAD_INTERFACE
      /*
        Create new instrumentation for the new THD job,
        and attach it to this running pthread.
      */
      PSI_thread *psi= PSI_THREAD_CALL(new_thread)
        (key_thread_one_connection, thd, thd->thread_id());
      PSI_THREAD_CALL(set_thread)(psi);
#endif

      /*
        THD::mysys_var::abort is associated with physical thread rather
        than with THD object. So we need to reset this flag before using
        this thread for handling of new THD object/connection.
      */
      thd->mysys_var->abort= 0;
      thd->thr_create_utime= thd->start_utime= my_micro_time();
      mutex_lock_shard(SHARDED(&LOCK_thread_count), thd);
      add_global_thread(thd);
      mutex_unlock_shard(SHARDED(&LOCK_thread_count), thd);

      thd->set_thread_priority();
      return true;
    }
  }
  mysql_mutex_unlock(&LOCK_thread_count);
  return false;
}


/**
  End thread for the current connection

  @param thd            Thread handler
  @param block_pthread  Block the pthread so it can be reused later.
                        Normally this is true in all cases except when we got
                        out of resources initializing the current thread

  @retval false  Signal to handle_one_connection to reuse connection

  @note If the pthread is blocked, we will wait until the pthread is
        scheduled to be reused and then return.
        If the pthread is not to be blocked, it will be ended.
*/

bool one_thread_per_connection_end(THD *thd, bool block_pthread)
{
  DBUG_ENTER("one_thread_per_connection_end");
  DBUG_PRINT("info", ("thd %p block_pthread %d", thd, (int) block_pthread));

  thd->release_resources();
  remove_global_thread(thd);

  // this is an atomic, and we are not holding LOCK_thread_count
  if (kill_blocked_pthreads_flag.load(std::memory_order_acquire))
  {
    // Do not block if we are about to shut down
    block_pthread= false;
  }

  // Clean up errors now, before possibly waiting for a new connection.
#ifndef EMBEDDED_LIBRARY
  ERR_remove_state(0);
#endif

  // reset thread priority
  block_pthread = thd->set_thread_priority(0);
  delete thd;

#ifdef HAVE_PSI_THREAD_INTERFACE
  /*
    Delete the instrumentation for the job that just completed.
  */
  PSI_THREAD_CALL(delete_current_thread)();
#endif

  mysql_mutex_lock(&LOCK_thread_count);
  if (!global_thread_count.load(std::memory_order_relaxed)) {
    mysql_cond_broadcast(&COND_thread_count);
  }
  dec_connection_count_locked();

  if (block_pthread)
    block_pthread= block_until_new_connection_halflock();
  else
    mysql_mutex_unlock(&LOCK_thread_count);

  if (block_pthread)
    DBUG_RETURN(false);                         // Pthread is reused

  /* It's safe to broadcast outside a lock (COND... is not deleted here) */
  DBUG_PRINT("signal", ("Broadcasting COND_thread_count"));
  DBUG_LEAVE;                                   // Must match DBUG_ENTER()
  my_thread_end();
  mysql_cond_broadcast(&COND_thread_count);

  pthread_exit(0);
  return false;                                 // Avoid compiler warnings
}


void kill_blocked_pthreads()
{
  kill_blocked_pthreads_flag.store(true, std::memory_order_release);
  mysql_mutex_lock(&LOCK_thread_count);
  while (blocked_pthread_count)
  {
    mysql_cond_broadcast(&COND_thread_cache);
    mysql_cond_wait(&COND_flush_thread_cache, &LOCK_thread_count);
  }
  mysql_mutex_unlock(&LOCK_thread_count);
  kill_blocked_pthreads_flag.store(false, std::memory_order_release);
}


#ifdef THREAD_SPECIFIC_SIGPIPE
/**
  Aborts a thread nicely. Comes here on SIGPIPE.

  @todo
    One should have to fix that thr_alarm know about this thread too.
*/
extern "C" sig_handler abort_thread(int sig MY_ATTRIBUTE((unused)))
{
  THD *thd=current_thd;
  DBUG_ENTER("abort_thread");
  if (thd)
    thd->killed= THD::KILL_CONNECTION;
  DBUG_VOID_RETURN;
}
#endif


/******************************************************************************
  Setup a signal thread with handles all signals.
  Because Linux doesn't support schemas use a mutex to check that
  the signal thread is ready before continuing
******************************************************************************/

#if defined(__WIN__)


/*
  On Windows, we use native SetConsoleCtrlHandler for handle events like Ctrl-C
  with graceful shutdown.
  Also, we do not use signal(), but SetUnhandledExceptionFilter instead - as it
  provides possibility to pass the exception to just-in-time debugger, collect
  dumps and potentially also the exception and thread context used to output
  callstack.
*/

static BOOL WINAPI console_event_handler( DWORD type )
{
  DBUG_ENTER("console_event_handler");
#ifndef EMBEDDED_LIBRARY
  if(type == CTRL_C_EVENT)
  {
     /*
       Do not shutdown before startup is finished and shutdown
       thread is initialized. Otherwise there is a race condition
       between main thread doing initialization and CTRL-C thread doing
       cleanup, which can result into crash.
     */
#ifndef EMBEDDED_LIBRARY
     if(hEventShutdown)
       kill_mysql();
     else
#endif
       sql_print_warning("CTRL-C ignored during startup");
     DBUG_RETURN(TRUE);
  }
#endif
  DBUG_RETURN(FALSE);
}




#ifdef DEBUG_UNHANDLED_EXCEPTION_FILTER
#define DEBUGGER_ATTACH_TIMEOUT 120
/*
  Wait for debugger to attach and break into debugger. If debugger is not attached,
  resume after timeout.
*/
static void wait_for_debugger(int timeout_sec)
{
   if(!IsDebuggerPresent())
   {
     int i;
     printf("Waiting for debugger to attach, pid=%u\n",GetCurrentProcessId());
     fflush(stdout);
     for(i= 0; i < timeout_sec; i++)
     {
       Sleep(1000);
       if(IsDebuggerPresent())
       {
         /* Break into debugger */
         __debugbreak();
         return;
       }
     }
     printf("pid=%u, debugger not attached after %d seconds, resuming\n",GetCurrentProcessId(),
       timeout_sec);
     fflush(stdout);
   }
}
#endif /* DEBUG_UNHANDLED_EXCEPTION_FILTER */

LONG WINAPI my_unhandler_exception_filter(EXCEPTION_POINTERS *ex_pointers)
{
   static BOOL first_time= TRUE;
   if(!first_time)
   {
     /*
       This routine can be called twice, typically
       when detaching in JIT debugger.
       Return EXCEPTION_EXECUTE_HANDLER to terminate process.
     */
     return EXCEPTION_EXECUTE_HANDLER;
   }
   first_time= FALSE;
#ifdef DEBUG_UNHANDLED_EXCEPTION_FILTER
   /*
    Unfortunately there is no clean way to debug unhandled exception filters,
    as debugger does not stop there(also documented in MSDN)
    To overcome, one could put a MessageBox, but this will not work in service.
    Better solution is to print error message and sleep some minutes
    until debugger is attached
  */
  wait_for_debugger(DEBUGGER_ATTACH_TIMEOUT);
#endif /* DEBUG_UNHANDLED_EXCEPTION_FILTER */
  __try
  {
    my_set_exception_pointers(ex_pointers);
    handle_fatal_signal(ex_pointers->ExceptionRecord->ExceptionCode);
  }
  __except(EXCEPTION_EXECUTE_HANDLER)
  {
    DWORD written;
    const char msg[] = "Got exception in exception handler!\n";
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),msg, sizeof(msg)-1,
      &written,NULL);
  }
  /*
    Return EXCEPTION_CONTINUE_SEARCH to give JIT debugger
    (drwtsn32 or vsjitdebugger) possibility to attach,
    if JIT debugger is configured.
    Windows Error reporting might generate a dump here.
  */
  return EXCEPTION_CONTINUE_SEARCH;
}


void my_init_signals(void)
{
  if(opt_console)
    SetConsoleCtrlHandler(console_event_handler,TRUE);

    /* Avoid MessageBox()es*/
  _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
  _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);

   /*
     Do not use SEM_NOGPFAULTERRORBOX in the following SetErrorMode (),
     because it would prevent JIT debugger and Windows error reporting
     from working. We need WER or JIT-debugging, since our own unhandled
     exception filter is not guaranteed to work in all situation
     (like heap corruption or stack overflow)
   */
  SetErrorMode(SetErrorMode(0) | SEM_FAILCRITICALERRORS
                               | SEM_NOOPENFILEERRORBOX);
  SetUnhandledExceptionFilter(my_unhandler_exception_filter);
}


static void start_signal_handler(void)
{
#ifndef EMBEDDED_LIBRARY
  // Save vm id of this process
  if (!opt_bootstrap)
    create_pid_file();
#endif /* EMBEDDED_LIBRARY */
}


static void check_data_home(const char *path)
{}

#endif /* __WIN__ */


#if BACKTRACE_DEMANGLE
#include <cxxabi.h>
extern "C" char *my_demangle(const char *mangled_name, int *status)
{
  return abi::__cxa_demangle(mangled_name, NULL, NULL, status);
}
#endif

const char *timer_in_use = "None";

ulonglong my_timer_none(void)
{
  return 0;
}
ulonglong (*my_timer_now)(void) = &my_timer_none;

struct my_timer_unit_info my_timer;

/* Sets up the data required for use of my_timer_* functions.
-Selects the best timer by high frequency, and tight resolution.
-Points my_timer_now() to the selected timer function.
-Initializes my_timer struct to contain the info for selected timer.
-Sets Timer_in_use SHOW STATUS var to the name of the timer.
*/
void init_my_timer(void)
{
  MY_TIMER_INFO all_timer_info;
  my_timer_init(&all_timer_info);

  if (all_timer_info.cycles.frequency > 1000000 &&
	all_timer_info.cycles.resolution == 1)
  {
    my_timer = all_timer_info.cycles;
    my_timer_now = &my_timer_cycles;
    timer_in_use = "Cycle";
  }
  else if (all_timer_info.nanoseconds.frequency > 1000000 &&
	all_timer_info.nanoseconds.resolution == 1)
  {
    my_timer = all_timer_info.nanoseconds;
    my_timer_now = &my_timer_nanoseconds;
    timer_in_use = "Nanosecond";
  }
  else if (all_timer_info.microseconds.frequency >= 1000000 &&
	all_timer_info.microseconds.resolution == 1)
  {
    my_timer = all_timer_info.microseconds;
    my_timer_now = &my_timer_microseconds;
    timer_in_use = "Microsecond";
  }
  else if (all_timer_info.milliseconds.frequency >= 1000 &&
	all_timer_info.milliseconds.resolution == 1)
  {
    my_timer = all_timer_info.milliseconds;
    my_timer_now = &my_timer_milliseconds;
    timer_in_use = "Millisecond";
  }
  else if (all_timer_info.ticks.frequency >= 1000 &&
	all_timer_info.ticks.resolution == 1)	/* Will probably be false */
  {
    my_timer = all_timer_info.ticks;
    my_timer_now = &my_timer_ticks;
    timer_in_use = "Tick";
  }
  else
  {
    /* None are acceptable, so leave it as "None", and fill in struct */
    my_timer.frequency = 1;	/* Avoid div-by-zero */
    my_timer.overhead = 0;	/* Since it doesn't do anything */
    my_timer.resolution = 10;	/* Another sign it's bad */
    my_timer.routine = 0;	/* None */
  }
}

/**********************************************************************
Accumulate per-table page stats helper function */
void my_page_stats_sum_atomic(page_stats_atomic_t* sum,
                              page_stats_t* page_stats)
{
  sum->n_pages_read.inc(page_stats->n_pages_read);
  sum->n_pages_read_index.inc(page_stats->n_pages_read_index);
  sum->n_pages_read_blob.inc(page_stats->n_pages_read_blob);
  sum->n_pages_written.inc(page_stats->n_pages_written);
  sum->n_pages_written_index.inc(page_stats->n_pages_written_index);
  sum->n_pages_written_blob.inc(page_stats->n_pages_written_blob);
}

/**********************************************************************
Accumulate per-table compression stats helper function */
void my_comp_stats_sum_atomic(comp_stats_atomic_t* sum,
                              comp_stats_t* comp_stats)
{
  sum->compressed.inc(comp_stats->compressed);
  sum->compressed_ok.inc(comp_stats->compressed_ok);
  sum->compressed_primary.inc(comp_stats->compressed_primary);
  sum->compressed_primary_ok.inc(comp_stats->compressed_primary_ok);
  sum->decompressed.inc(comp_stats->decompressed);
  sum->compressed_time.inc(comp_stats->compressed_time);
  sum->compressed_ok_time.inc(comp_stats->compressed_ok_time);
  sum->decompressed_time.inc(comp_stats->decompressed_time);
  sum->compressed_primary_time.inc(comp_stats->compressed_primary_time);
  sum->compressed_primary_ok_time.inc(comp_stats->compressed_primary_ok_time);
}

/**
  Create a new Histogram.

  @param current_histogram    The histogram being initialized.
  @param step_size_with_unit  Configurable system variable containing
                              step size and unit of the Histogram.
*/
void latency_histogram_init(latency_histogram* current_histogram,
                    const char *step_size_with_unit)
{
  assert (current_histogram != 0);

  double step_size_base_time = 0.0;
  current_histogram->num_bins = NUMBER_OF_HISTOGRAM_BINS;

  // this can potentially be made configurable later.
  current_histogram->step_ratio = 2.0;
  current_histogram->step_size = 0;
  char *histogram_unit = NULL;

  std::memset(current_histogram->count_per_bin,
      0, sizeof(ulonglong)*NUMBER_OF_HISTOGRAM_BINS);

  if (!step_size_with_unit)
    return;

  step_size_base_time = strtod(step_size_with_unit, &histogram_unit);
  if (histogram_unit)  {
    if (!strcmp(histogram_unit, "s"))  {
      current_histogram->step_size = microseconds_to_my_timer(
                                              step_size_base_time * 1000000.0);
    }
    else if (!strcmp(histogram_unit, "ms"))  {
      current_histogram->step_size = microseconds_to_my_timer(
                                              step_size_base_time * 1000.0);
    }
    else if (!strcmp(histogram_unit, "us"))  {
      current_histogram->step_size = microseconds_to_my_timer(
                                              step_size_base_time);
    }
    /* Special case when step size is passed to be '0' */
    else if (*histogram_unit == '\0') {
      if (step_size_base_time == 0.0) {
        current_histogram->step_size = 0;
      }
      else
        current_histogram->step_size =
          microseconds_to_my_timer(step_size_base_time);
    }
    else  {
      sql_print_error("Invalid units given to histogram step size.");
      return;
    }
  } else {
    /* NO_LINT_DEBUG */
    sql_print_error("Invalid histogram step size.");
    return;
  }
}

/**
  @param current_histogram    The histogram being initialized.
  @param step_size            Step size
*/
void counter_histogram_init(counter_histogram* current_histogram,
                            ulonglong step_size)
{
  current_histogram->num_bins = NUMBER_OF_COUNTER_HISTOGRAM_BINS;
  current_histogram->step_size = step_size;
  for (size_t i = 0; i < current_histogram->num_bins; ++i)
    (current_histogram->count_per_bin)[i] = 0;
}

/**
  Search a value in the histogram bins.

  @param current_histogram  The current histogram.
  @param value              Value to be searched.

  @return                   Returns the bin that contains this value.
                            -1 if Step Size is 0
*/
static int latency_histogram_bin_search(latency_histogram* current_histogram,
                         ulonglong value)
{
  if (current_histogram->step_size == 0 || value == 0
    || current_histogram->step_ratio <= 0.0)
    return -1;

  double dbin_no = std::log2((double)value / current_histogram->step_size) /
      std::log2(current_histogram->step_ratio);

  int ibin_no = (int)dbin_no;
  if (ibin_no < 0)
    return 0;

  return min(ibin_no, (int)current_histogram->num_bins - 1);
}

/**
  Search a value in the histogram bins.

  @param current_histogram  The current histogram.
  @param value              Value to be searched.

  @return                   Returns the bin that contains this value.
                            -1 if Step Size is 0
*/
static int counter_histogram_bin_search(counter_histogram* current_histogram,
                         const ulonglong value)
{
  if (current_histogram->step_size == 0)
    return -1;

  return min((int)(value/current_histogram->step_size),
             (int)current_histogram->num_bins-1);
}

/**
  Increment the count of a bin in Histogram.

  @param current_histogram  The current histogram.
  @param value              Value of which corresponding bin has to be found.
  @param count              Amount by which the count of a bin has to be
                            increased.

*/
void latency_histogram_increment(latency_histogram* current_histogram,
                                   ulonglong value, ulonglong count)
{
  int index = latency_histogram_bin_search(current_histogram, value);
  if (index < 0)
    return;
  my_atomic_add64((longlong*)&((current_histogram->count_per_bin)[index]),
                  count);
}

/**
  Increment the count of a bin in counter histogram.

  @param current_histogram  The current histogram.
  @param value              Current counter value.
*/
void counter_histogram_increment(counter_histogram* current_histogram,
                                 ulonglong value)
{
  int index = counter_histogram_bin_search(current_histogram, value);
  if (index >= 0)
    my_atomic_add64((longlong*)&((current_histogram->count_per_bin)[index]), 1);
}

/**
  Get the count corresponding to a bin of the Histogram.

  @param current_histogram  The current histogram.
  @param bin_num            The bin whose count has to be returned.

  @return                   Returns the count of that bin.
*/
ulonglong latency_histogram_get_count(latency_histogram* current_histogram,
                                     size_t bin_num)
{
  return (current_histogram->count_per_bin)[bin_num];
}

/**
  Validate if the string passed to the configurable histogram step size
  conforms to proper syntax.

  @param step_size_with_unit  The configurable step size string to be checked.

  @return                     1 if invalid, 0 if valid.
*/
int histogram_validate_step_size_string(const char* step_size_with_unit)
{
  if (step_size_with_unit == nullptr)
    return 0;

  int ret = 0;
  char *histogram_unit = NULL;
  double histogram_step_size = strtod(step_size_with_unit, &histogram_unit);
  if (histogram_step_size && histogram_unit)  {
    if (strcmp(histogram_unit, "ms")
        && strcmp(histogram_unit, "us")
        && strcmp(histogram_unit, "s"))
      ret = 1;
  }
  /* Special case when step size is passed to be '0' */
  else if (*histogram_unit == '\0' && histogram_step_size == 0.0)
    return 0;
  else
    ret = 1;
  return ret;
}

/**
 * Set the priority of an OS thread.
 *
 * @param thread_priority_cptr  A string of the format os_thread_id:nice_val.
 * @return                      true on success, false otherwise.
 */
bool set_thread_priority(char *thread_priority_cptr)
{
  //parse input to get thread_id and nice_val
  std::string thd_id_nice_val_str(thread_priority_cptr);

  if (thd_id_nice_val_str.empty())
  {
    return true;
  }

  std::size_t delimpos = thd_id_nice_val_str.find(':');
  if (delimpos == std::string::npos)
  {
    return false;
  }
  std::string thread_id_str(thd_id_nice_val_str, 0, delimpos);
  std::string nice_val_str(thd_id_nice_val_str, delimpos+1);
  const char *nice_val_ptr = nice_val_str.c_str();
  const char *thread_id_ptr = thread_id_str.c_str();

  char *er;
  long nice_val = strtol(nice_val_ptr, &er, 10);
  if (er != nice_val_ptr + nice_val_str.length())
      return false;

  unsigned long thread_id = strtoul(thread_id_ptr, &er, 10);
  if (er != thread_id_ptr + thread_id_str.length())
      return false;

  //Check weather nice_value is in a valid range
  if (nice_val > 19 || nice_val < -20)
  {
    /* NO_LINT_DEBUG */
    sql_print_error("Nice value %ld is outside the valid"
        "range of -19 to 20",nice_val);
    return false;
  }

  //Check weather thread_id is a valid active system_thread_id
  mutex_lock_all_shards(SHARDED(&LOCK_thd_remove));

  bool ret= false;
  Thread_iterator it= global_thread_list->begin();
  Thread_iterator end= global_thread_list->end();
  for (; it != end; ++it)
  {
    THD *thd= *it;
    if ((unsigned long)thd->system_thread_id == thread_id)
    {
      ret= thd->set_thread_priority(nice_val);
      break;
    }
  }

  mutex_unlock_all_shards(SHARDED(&LOCK_thd_remove));
  return ret;
}

/**
 * Set a capability's flags.
 * Effective capabilities of a thread are changed.
 *
 * @param capability  Capability to change. e.g. CAP_SYS_NICE
 * @param flag        Flag to set. e.g. CAP_SET, CAP_CLEAR
 * @return            true on success, false otherwise.
 */
static bool set_capability_flag(cap_value_t capability, cap_flag_value_t flag)
{
  DBUG_ENTER("set_capability_flag");

  cap_value_t cap_list[]= {capability};
  cap_t caps= cap_get_proc();
  bool ret= true;

  if (!caps ||
      cap_set_flag(caps, CAP_EFFECTIVE, 1, cap_list, flag) ||
      cap_set_proc(caps))
  {
    ret= false;
  }

  if (caps)
  {
    cap_free(caps);
  }

  DBUG_RETURN(ret);
}

static bool acquire_capability(cap_value_t capability)
{
  return set_capability_flag(capability, CAP_SET);
}

static bool drop_capability(cap_value_t capability)
{
  return set_capability_flag(capability, CAP_CLEAR);
}

/**
 * Sets a system thread priority.
 * CAP_SYS_NICE capability is temporarily acquired if it does not already
 * exist, and dropped after the setpriority() call is complete.
 *
 * @param tid  OS thread id.
 * @param pri  Priority (nice value) to set the thread to.
 * @return     true on success, false otherwise.
 */
bool set_system_thread_priority(pid_t tid, int pri)
{
  DBUG_ENTER("set_system_thread_priority");

  bool ret= true;
  acquire_capability(CAP_SYS_NICE);
  if (setpriority(PRIO_PROCESS, tid, pri) == -1)
  {
    ret= false;
  }
  drop_capability(CAP_SYS_NICE);

  DBUG_RETURN(ret);
}

bool set_current_thread_priority()
{
  return current_thd->set_thread_priority();
}

/**
  This function is called by show_innodb_latency_histgoram()
  to convert the histogram bucket ranges in system time units
  to a string and calculates units on the fly, which can be
  displayed in the output of SHOW GLOBAL STATUS.
  The string has the following form:

  <HistogramName>_<BucketLowerValue>-<BucketUpperValue><Unit>

  @param bucket_lower_display  Lower Range value of the Histogram Bucket
  @param bucket_upper_display  Upper Range value of the Histogram Bucket
  @param is_last_bucket        Flag to denote last bucket in the histogram

  @return                      The display string for the Histogram Bucket
*/
histogram_display_string
histogram_bucket_to_display_string(ulonglong bucket_lower_display,
                                   ulonglong bucket_upper_display,
                                   bool is_last_bucket)
{
  struct histogram_display_string histogram_bucket_name;

  std::string time_unit_suffix;
  ulonglong time_factor= 1;

  if ((bucket_upper_display % 1000000) == 0 &&
      (bucket_lower_display % 1000000) == 0)
  {
    time_unit_suffix= "s";
    time_factor= 1000000;
  }
  else if ((bucket_upper_display % 1000) == 0 &&
           (bucket_lower_display % 1000) == 0)
  {
    time_unit_suffix= "ms";
    time_factor= 1000;
  }
  else
  {
    time_unit_suffix= "us";
  }

  std::string bucket_display_format;
  if (is_last_bucket)
  {
    bucket_display_format= "%llu-MAX" + time_unit_suffix;
    my_snprintf(histogram_bucket_name.name, HISTOGRAM_BUCKET_NAME_MAX_SIZE,
                bucket_display_format.c_str(),
                bucket_lower_display / time_factor);
  }
  else
  {
    bucket_display_format= "%llu-%llu" + time_unit_suffix;
    my_snprintf(histogram_bucket_name.name, HISTOGRAM_BUCKET_NAME_MAX_SIZE,
                bucket_display_format.c_str(),
                bucket_lower_display / time_factor,
                bucket_upper_display / time_factor);
  }

  return histogram_bucket_name;
}

/**
   Frees old histogram bucket display strings before assigning new ones.
*/
void free_latency_histogram_sysvars(SHOW_VAR* latency_histogram_data)
{
  size_t i;
  for (i = 0; i < NUMBER_OF_HISTOGRAM_BINS; ++i)
  {
    if (latency_histogram_data[i].name)
      my_free((void*)latency_histogram_data[i].name);
  }
}

/**
   Frees old histogram bucket display strings before assigning new ones.
*/
void free_counter_histogram_sysvars(SHOW_VAR* counter_histogram_data)
{
  size_t i;
  for (i = 0; i < NUMBER_OF_COUNTER_HISTOGRAM_BINS; ++i)
  {
    if (counter_histogram_data[i].name)
      my_free((void*)counter_histogram_data[i].name);
  }
}

/**
  This function is called by the Callback function show_innodb_vars()
  to add entries into the latency_histogram_xxxx array, by forming
  the appropriate display string and fetching the histogram bin
  counts.

  @param current_histogram       Histogram whose values are currently added
                                 in the SHOW_VAR array
  @param latency_histogram_data  SHOW_VAR array for the corresponding Histogram
  @param histogram_values        Values to be exported to Innodb status.
                                 This array contains the bin counts of the
                                 respective Histograms.
*/
void prepare_latency_histogram_vars(latency_histogram* current_histogram,
                                    SHOW_VAR* latency_histogram_data,
                                    ulonglong* histogram_values)
{
  size_t i;
  ulonglong bucket_lower_display, bucket_upper_display;
  const SHOW_VAR temp_last = {NullS, NullS, SHOW_LONG};

  free_latency_histogram_sysvars(latency_histogram_data);

  ulonglong itr_step_size = current_histogram->step_size;
  for (i = 0, bucket_lower_display = 0; i < NUMBER_OF_HISTOGRAM_BINS; ++i)
  {
    bucket_upper_display =
      my_timer_to_microseconds_ulonglong(itr_step_size)
      + bucket_lower_display;

    struct histogram_display_string histogram_bucket_name =
        histogram_bucket_to_display_string(bucket_lower_display,
                                           bucket_upper_display,
                                           i == NUMBER_OF_HISTOGRAM_BINS - 1);

    const SHOW_VAR temp = {my_strdup(histogram_bucket_name.name, MYF(0)),
                           (char*) &(histogram_values[i]), SHOW_LONGLONG};
    latency_histogram_data[i] = temp;

    bucket_lower_display = bucket_upper_display;
    itr_step_size *= current_histogram->step_ratio;
  }
  latency_histogram_data[NUMBER_OF_HISTOGRAM_BINS] = temp_last;
}


/**
  Prepares counter histogram to display in SHOW STATUS

  @param current_histogram current histogram
  @param counter_histogram_data
*/
void prepare_counter_histogram_vars(counter_histogram *current_histogram,
                                    SHOW_VAR* counter_histogram_data,
                                    ulonglong* histogram_values)
{
  ulonglong bucket_lower_display=0, bucket_upper_display;
  free_counter_histogram_sysvars(counter_histogram_data);
  const SHOW_VAR temp_last = {NullS, NullS, SHOW_LONG};

  for (size_t i=0; i < NUMBER_OF_COUNTER_HISTOGRAM_BINS; ++i)
  {
    bucket_upper_display = current_histogram->step_size + bucket_lower_display;

    struct histogram_display_string histogram_bucket_name;
    my_snprintf(histogram_bucket_name.name,
                HISTOGRAM_BUCKET_NAME_MAX_SIZE, "%llu-%llu",
                bucket_lower_display, bucket_upper_display);

    const SHOW_VAR temp = {my_strdup(histogram_bucket_name.name, MYF(0)),
                           (char*) &(histogram_values[i]), SHOW_LONGLONG};
    counter_histogram_data[i] = temp;
    bucket_lower_display = bucket_upper_display;
  }
  counter_histogram_data[NUMBER_OF_COUNTER_HISTOGRAM_BINS] = temp_last;
}


#if !defined(__WIN__)
#ifndef SA_RESETHAND
#define SA_RESETHAND 0
#endif
#ifndef SA_NODEFER
#define SA_NODEFER 0
#endif

#ifndef EMBEDDED_LIBRARY

void my_init_signals(void)
{
  sigset_t set;
  struct sigaction sa;
  DBUG_ENTER("my_init_signals");

  my_sigset(THR_SERVER_ALARM,print_signal_warning); // Should never be called!

  if (!(test_flags & TEST_NO_STACKTRACE) || opt_core_file)
  {
    sa.sa_flags = SA_RESETHAND | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigprocmask(SIG_SETMASK,&sa.sa_mask,NULL);

#ifdef HAVE_STACKTRACE
    my_init_stacktrace();
#endif
#if defined(__amiga__)
    sa.sa_handler=(void(*)())handle_fatal_signal;
#else
    sa.sa_flags |= SA_SIGINFO;
    sa.sa_sigaction=handle_fatal_signal_ex;
#endif
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
#ifdef SIGBUS
    sigaction(SIGBUS, &sa, NULL);
#endif
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
  }

#ifdef HAVE_GETRLIMIT
  if (opt_core_file)
  {
    /* Change limits so that we will get a core file */
    STRUCT_RLIMIT rl;
    rl.rlim_cur = rl.rlim_max = RLIM_INFINITY;
    if (setrlimit(RLIMIT_CORE, &rl) && log_warnings)
      sql_print_warning("setrlimit could not change the size of core files to 'infinity';  We may not be able to generate a core file on signals");
  }
#endif
  (void) sigemptyset(&set);
  my_sigset(SIGPIPE,SIG_IGN);
  sigaddset(&set,SIGPIPE);
#ifndef IGNORE_SIGHUP_SIGQUIT
  sigaddset(&set,SIGQUIT);
  sigaddset(&set,SIGHUP);
#endif
  sigaddset(&set,SIGTERM);

  /* Fix signals if blocked by parents (can happen on Mac OS X) */
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sa.sa_handler = print_signal_warning;
  sigaction(SIGTERM, &sa, (struct sigaction*) 0);
  sa.sa_flags = 0;
  sa.sa_handler = print_signal_warning;
  sigaction(SIGHUP, &sa, (struct sigaction*) 0);
#ifdef SIGTSTP
  sigaddset(&set,SIGTSTP);
#endif
  sigaddset(&set,THR_SERVER_ALARM);
  if (test_flags & TEST_SIGINT)
  {
    my_sigset(thr_kill_signal, end_thread_signal);
    // May be SIGINT
    sigdelset(&set, thr_kill_signal);
  }
  else
    sigaddset(&set,SIGINT);
  sigprocmask(SIG_SETMASK,&set,NULL);
  pthread_sigmask(SIG_SETMASK,&set,NULL);
  DBUG_VOID_RETURN;
}


static void start_signal_handler(void)
{
  int error;
  pthread_attr_t thr_attr;
  DBUG_ENTER("start_signal_handler");

  (void) pthread_attr_init(&thr_attr);
#if !defined(HAVE_DEC_3_2_THREADS)
  pthread_attr_setscope(&thr_attr,PTHREAD_SCOPE_SYSTEM);
  (void) pthread_attr_setdetachstate(&thr_attr,PTHREAD_CREATE_DETACHED);

  size_t guardize= 0;
  pthread_attr_getguardsize(&thr_attr, &guardize);

#if defined(__ia64__) || defined(__ia64)
  /*
    Peculiar things with ia64 platforms - it seems we only have half the
    stack size in reality, so we have to double it here
  */
  guardize= my_thread_stack_size;
#endif

  pthread_attr_setstacksize(&thr_attr, my_thread_stack_size + guardize);
#endif

  mysql_mutex_lock(&LOCK_thread_count);
  if ((error= mysql_thread_create(key_thread_signal_hand,
                                  &signal_thread, &thr_attr, signal_hand, 0)))
  {
    sql_print_error("Can't create interrupt-thread (error %d, errno: %d)",
                    error,errno);
    exit(1);
  }
  mysql_cond_wait(&COND_thread_count, &LOCK_thread_count);
  mysql_mutex_unlock(&LOCK_thread_count);

  (void) pthread_attr_destroy(&thr_attr);
  DBUG_VOID_RETURN;
}


/** This threads handles all signals and alarms. */
/* ARGSUSED */
pthread_handler_t signal_hand(void *arg MY_ATTRIBUTE((unused)))
{
  sigset_t set;
  int sig;
  my_thread_init();       // Init new thread
  signal_thread_in_use= 1;

  /*
    Setup alarm handler
    This should actually be '+ max_number_of_slaves' instead of +10,
    but the +10 should be quite safe.
  */
  init_thr_alarm(thread_scheduler->max_threads +
     global_system_variables.max_insert_delayed_threads + 10);
  if (test_flags & TEST_SIGINT)
  {
    (void) sigemptyset(&set);     // Setup up SIGINT for debug
    (void) sigaddset(&set,SIGINT);    // For debugging
    (void) pthread_sigmask(SIG_UNBLOCK,&set,NULL);
  }
  (void) sigemptyset(&set);     // Setup up SIGINT for debug
#ifdef USE_ONE_SIGNAL_HAND
  (void) sigaddset(&set,THR_SERVER_ALARM);  // For alarms
#endif
#ifndef IGNORE_SIGHUP_SIGQUIT
  (void) sigaddset(&set,SIGQUIT);
  (void) sigaddset(&set,SIGHUP);
#endif
  (void) sigaddset(&set,SIGTERM);
  (void) sigaddset(&set,SIGTSTP);

  /* Save pid to this process (or thread on Linux) */
  if (!opt_bootstrap)
    create_pid_file();

  /*
    signal to start_signal_handler that we are ready
    This works by waiting for start_signal_handler to free mutex,
    after which we signal it that we are ready.
    At this pointer there is no other threads running, so there
    should not be any other mysql_cond_signal() calls.
  */
  mysql_mutex_lock(&LOCK_thread_count);
  mysql_cond_broadcast(&COND_thread_count);
  mysql_mutex_unlock(&LOCK_thread_count);

  /*
    Waiting for until mysqld_server_started != 0
    to ensure that all server components has been successfully
    initialized. This step is mandatory since signal processing
    could be done safely only when all server components
    has been initialized.
  */
  mysql_mutex_lock(&LOCK_server_started);
  while (!mysqld_server_started)
    mysql_cond_wait(&COND_server_started, &LOCK_server_started);
  mysql_mutex_unlock(&LOCK_server_started);

  for (;;)
  {
    int error;          // Used when debugging
    if (shutdown_in_progress && !abort_loop)
    {
      sig= SIGTERM;
      error=0;
    }
    else
      while ((error=my_sigwait(&set,&sig)) == EINTR) ;
    if (cleanup_done)
    {
      my_thread_end();
      signal_thread_in_use= 0;
      pthread_exit(0);        // Safety
      return 0;                                 // Avoid compiler warnings
    }
    switch (sig) {
    case SIGTERM:
    case SIGQUIT:
    case SIGKILL:
      sql_print_information("Got signal %d to shutdown mysqld",sig);
      /* switch to the old log message processing */
      logger.set_handlers(LOG_FILE, opt_slow_log ? LOG_FILE:LOG_NONE,
                          opt_log ? LOG_FILE:LOG_NONE, LOG_FILE);
      DBUG_PRINT("info",("Got signal: %d  abort_loop: %d",sig,abort_loop));
      if (!abort_loop)
      {
        abort_loop=1;       // mark abort for threads
#ifdef HAVE_PSI_THREAD_INTERFACE
        /* Delete the instrumentation for the signal thread */
        PSI_THREAD_CALL(delete_current_thread)();
#endif
#ifdef USE_ONE_SIGNAL_HAND
        pthread_t tmp;
        if ((error= mysql_thread_create(0, /* Not instrumented */
                                        &tmp, &connection_attrib,
                                        kill_server_thread,
                                        (void*) &sig)))
          sql_print_error("Can't create thread to kill server (errno= %d)",
                          error);
#else
        kill_server((void*) sig); // MIT THREAD has a alarm thread
#endif
      }
      break;
    case SIGHUP:
      if (!abort_loop)
      {
        int not_used;
  mysql_print_status();   // Print some debug info
  reload_acl_and_cache((THD*) 0,
           (REFRESH_LOG | REFRESH_TABLES | REFRESH_FAST |
            REFRESH_GRANT |
            REFRESH_THREADS | REFRESH_HOSTS),
           (TABLE_LIST*) 0, &not_used); // Flush logs
      }
      /* reenable logs after the options were reloaded */
      if (log_output_options & LOG_NONE)
      {
        logger.set_handlers(LOG_FILE,
                            opt_slow_log ? LOG_TABLE : LOG_NONE,
                            opt_log ? LOG_TABLE : LOG_NONE, LOG_FILE);
      }
      else
      {
        logger.set_handlers(LOG_FILE,
                            opt_slow_log ? log_output_options : LOG_NONE,
                            opt_log ? log_output_options : LOG_NONE, LOG_FILE);
      }
      break;
#ifdef USE_ONE_SIGNAL_HAND
    case THR_SERVER_ALARM:
      process_alarm(sig);     // Trigger alarms.
      break;
#endif
    default:
#ifdef EXTRA_DEBUG
      sql_print_warning("Got signal: %d  error: %d",sig,error); /* purecov: tested */
#endif
      break;          /* purecov: tested */
    }
  }
  return(0);          /* purecov: deadcode */
}

static void check_data_home(const char *path)
{}

#endif /*!EMBEDDED_LIBRARY*/
#endif  /* __WIN__*/


/**
  All global error messages are sent here where the first one is stored
  for the client.
*/
/* ARGSUSED */
extern "C" void my_message_sql(uint error, const char *str, myf MyFlags);

void my_message_sql(uint error, const char *str, myf MyFlags)
{
  THD *thd= current_thd;
  DBUG_ENTER("my_message_sql");

  DBUG_PRINT("error", ("error: %u  message: '%s'", error, str));

  DBUG_ASSERT(str != NULL);
  /*
    An error should have a valid error number (!= 0), so it can be caught
    in stored procedures by SQL exception handlers.
    Calling my_error() with error == 0 is a bug.
    Remaining known places to fix:
    - storage/myisam/mi_create.c, my_printf_error()
    TODO:
    DBUG_ASSERT(error != 0);
  */

  if (error == 0)
  {
    /* At least, prevent new abuse ... */
    DBUG_ASSERT(strncmp(str, "MyISAM table", 12) == 0);
    error= ER_UNKNOWN_ERROR;
  }

  mysql_audit_general(thd, MYSQL_AUDIT_GENERAL_ERROR, error, str);

  if (thd)
  {
    if (MyFlags & ME_FATALERROR)
      thd->is_fatal_error= 1;

    USER_STATS *us = thd_get_user_stats(thd);
    us->errors_total.inc();

    (void) thd->raise_condition(error,
                                NULL,
                                Sql_condition::WARN_LEVEL_ERROR,
                                str);
  }

  /* When simulating OOM, skip writing to error log to avoid mtr errors */
  DBUG_EXECUTE_IF("simulate_out_of_memory", DBUG_VOID_RETURN;);

  if (!thd || MyFlags & ME_NOREFRESH)
    sql_print_error("%s: %s",my_progname,str); /* purecov: inspected */
  DBUG_VOID_RETURN;
}


#ifndef EMBEDDED_LIBRARY
extern "C" void *my_str_malloc_mysqld(size_t size);
extern "C" void my_str_free_mysqld(void *ptr);
extern "C" void *my_str_realloc_mysqld(void *ptr, size_t size);

void *my_str_malloc_mysqld(size_t size)
{
  return my_malloc(size, MYF(MY_FAE));
}


void my_str_free_mysqld(void *ptr)
{
  my_free(ptr);
}

void *my_str_realloc_mysqld(void *ptr, size_t size)
{
  return my_realloc(ptr, size, MYF(MY_FAE));
}
#endif /* EMBEDDED_LIBRARY */


#ifdef __WIN__

pthread_handler_t handle_shutdown(void *arg)
{
  MSG msg;
  my_thread_init();

  /* this call should create the message queue for this thread */
  PeekMessage(&msg, NULL, 1, 65534,PM_NOREMOVE);
#if !defined(EMBEDDED_LIBRARY)
  if (WaitForSingleObject(hEventShutdown,INFINITE)==WAIT_OBJECT_0)
#endif /* EMBEDDED_LIBRARY */
     kill_server(MYSQL_KILL_SIGNAL);
  return 0;
}
#endif

const char *load_default_groups[]= {
#ifdef WITH_NDBCLUSTER_STORAGE_ENGINE
"mysql_cluster",
#endif
"mysqld","server", MYSQL_BASE_VERSION, 0, 0};

#if defined(__WIN__) && !defined(EMBEDDED_LIBRARY)
static const int load_default_groups_sz=
sizeof(load_default_groups)/sizeof(load_default_groups[0]);
#endif

#ifndef EMBEDDED_LIBRARY
/**
  This function is used to check for stack overrun for pathological
  cases of regular expressions and 'like' expressions.
  The call to current_thd is quite expensive, so we try to avoid it
  for the normal cases.
  The size of each stack frame for the wildcmp() routines is ~128 bytes,
  so checking *every* recursive call is not necessary.
 */
extern "C" int
check_enough_stack_size(int recurse_level)
{
  uchar stack_top;
  if (recurse_level % 16 != 0)
    return 0;

  THD *my_thd= current_thd;
  if (my_thd != NULL)
    return check_stack_overrun(my_thd, STACK_MIN_SIZE * 2, &stack_top);
  return 0;
}
#endif


/**
  Initialize one of the global date/time format variables.

  @param format_type    What kind of format should be supported
  @param var_ptr    Pointer to variable that should be updated

  @retval
    0 ok
  @retval
    1 error
*/

static bool init_global_datetime_format(timestamp_type format_type,
                                        DATE_TIME_FORMAT *format)
{
  /*
    Get command line option
    format->format.str is already set by my_getopt
  */
  format->format.length= strlen(format->format.str);

  if (parse_date_time_format(format_type, format))
  {
    fprintf(stderr, "Wrong date/time format specifier: %s\n",
            format->format.str);
    return true;
  }
  return false;
}

SHOW_VAR com_status_vars[]= {
  {"admin_commands",       (char*) offsetof(STATUS_VAR, com_other), SHOW_LONG_STATUS},
  {"assign_to_keycache",   (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_ASSIGN_TO_KEYCACHE]), SHOW_LONG_STATUS},
  {"alter_db",             (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_ALTER_DB]), SHOW_LONG_STATUS},
  {"alter_db_upgrade",     (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_ALTER_DB_UPGRADE]), SHOW_LONG_STATUS},
  {"alter_event",          (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_ALTER_EVENT]), SHOW_LONG_STATUS},
  {"alter_function",       (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_ALTER_FUNCTION]), SHOW_LONG_STATUS},
  {"alter_procedure",      (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_ALTER_PROCEDURE]), SHOW_LONG_STATUS},
  {"alter_server",         (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_ALTER_SERVER]), SHOW_LONG_STATUS},
  {"alter_table",          (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_ALTER_TABLE]), SHOW_LONG_STATUS},
  {"alter_tablespace",     (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_ALTER_TABLESPACE]), SHOW_LONG_STATUS},
  {"alter_user",           (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_ALTER_USER]), SHOW_LONG_STATUS},
  {"analyze",              (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_ANALYZE]), SHOW_LONG_STATUS},
  {"attach_explicit_snapshot",(char*) offsetof(STATUS_VAR,
      com_stat[(uint) SQLCOM_ATTACH_EXPLICIT_SNAPSHOT]), SHOW_LONG_STATUS},
  {"begin",                (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_BEGIN]), SHOW_LONG_STATUS},
  {"binlog",               (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_BINLOG_BASE64_EVENT]), SHOW_LONG_STATUS},
  {"call_procedure",       (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_CALL]), SHOW_LONG_STATUS},
  {"change_db",            (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_CHANGE_DB]), SHOW_LONG_STATUS},
  {"change_master",        (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_CHANGE_MASTER]), SHOW_LONG_STATUS},
  {"check",                (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_CHECK]), SHOW_LONG_STATUS},
  {"checksum",             (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_CHECKSUM]), SHOW_LONG_STATUS},
  {"commit",               (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_COMMIT]), SHOW_LONG_STATUS},
  {"create_db",            (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_CREATE_DB]), SHOW_LONG_STATUS},
  {"create_event",         (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_CREATE_EVENT]), SHOW_LONG_STATUS},
  {"create_function",      (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_CREATE_SPFUNCTION]), SHOW_LONG_STATUS},
  {"create_index",         (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_CREATE_INDEX]), SHOW_LONG_STATUS},
  {"create_procedure",     (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_CREATE_PROCEDURE]), SHOW_LONG_STATUS},
  {"create_server",        (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_CREATE_SERVER]), SHOW_LONG_STATUS},
  {"create_explicit_snapshot", (char*) offsetof(STATUS_VAR,
      com_stat[(uint) SQLCOM_CREATE_EXPLICIT_SNAPSHOT]), SHOW_LONG_STATUS},
  {"create_table",         (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_CREATE_TABLE]), SHOW_LONG_STATUS},
  {"create_trigger",       (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_CREATE_TRIGGER]), SHOW_LONG_STATUS},
  {"create_udf",           (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_CREATE_FUNCTION]), SHOW_LONG_STATUS},
  {"create_native_proc",   (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_CREATE_NPROCEDURE]), SHOW_LONG_STATUS},
  {"drop_native_proc",     (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_DROP_NPROCEDURE]), SHOW_LONG_STATUS},
  {"create_user",          (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_CREATE_USER]), SHOW_LONG_STATUS},
  {"create_view",          (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_CREATE_VIEW]), SHOW_LONG_STATUS},
  {"dealloc_sql",          (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_DEALLOCATE_PREPARE]), SHOW_LONG_STATUS},
  {"delete",               (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_DELETE]), SHOW_LONG_STATUS},
  {"delete_multi",         (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_DELETE_MULTI]), SHOW_LONG_STATUS},
  {"do",                   (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_DO]), SHOW_LONG_STATUS},
  {"drop_db",              (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_DROP_DB]), SHOW_LONG_STATUS},
  {"drop_event",           (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_DROP_EVENT]), SHOW_LONG_STATUS},
  {"drop_function",        (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_DROP_FUNCTION]), SHOW_LONG_STATUS},
  {"drop_index",           (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_DROP_INDEX]), SHOW_LONG_STATUS},
  {"drop_procedure",       (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_DROP_PROCEDURE]), SHOW_LONG_STATUS},
  {"drop_server",          (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_DROP_SERVER]), SHOW_LONG_STATUS},
  {"drop_table",           (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_DROP_TABLE]), SHOW_LONG_STATUS},
  {"drop_trigger",         (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_DROP_TRIGGER]), SHOW_LONG_STATUS},
  {"drop_user",            (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_DROP_USER]), SHOW_LONG_STATUS},
  {"drop_view",            (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_DROP_VIEW]), SHOW_LONG_STATUS},
  {"empty_query",          (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_EMPTY_QUERY]), SHOW_LONG_STATUS},
  {"execute_sql",          (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_EXECUTE]), SHOW_LONG_STATUS},
  {"find_gtid_position",   (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_FIND_GTID_POSITION]),SHOW_LONG_STATUS},
  {"flush",                (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_FLUSH]), SHOW_LONG_STATUS},
  {"get_diagnostics",      (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_GET_DIAGNOSTICS]), SHOW_LONG_STATUS},
  {"grant",                (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_GRANT]), SHOW_LONG_STATUS},
  {"show_gtid_executed",   (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_GTID_EXECUTED]), SHOW_LONG_STATUS},
  {"ha_close",             (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_HA_CLOSE]), SHOW_LONG_STATUS},
  {"ha_open",              (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_HA_OPEN]), SHOW_LONG_STATUS},
  {"ha_read",              (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_HA_READ]), SHOW_LONG_STATUS},
  {"help",                 (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_HELP]), SHOW_LONG_STATUS},
  {"insert",               (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_INSERT]), SHOW_LONG_STATUS},
  {"insert_select",        (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_INSERT_SELECT]), SHOW_LONG_STATUS},
  {"install_plugin",       (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_INSTALL_PLUGIN]), SHOW_LONG_STATUS},
  {"kill",                 (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_KILL]), SHOW_LONG_STATUS},
  {"load",                 (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_LOAD]), SHOW_LONG_STATUS},
  {"lock_tables",          (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_LOCK_TABLES]), SHOW_LONG_STATUS},
  {"optimize",             (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_OPTIMIZE]), SHOW_LONG_STATUS},
  {"preload_keys",         (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_PRELOAD_KEYS]), SHOW_LONG_STATUS},
  {"prepare_sql",          (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_PREPARE]), SHOW_LONG_STATUS},
  {"purge",                (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_PURGE]), SHOW_LONG_STATUS},
  {"purge_before_date",    (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_PURGE_BEFORE]), SHOW_LONG_STATUS},
  {"purge_gtid",           (char*) offsetof(STATUS_VAR,
    com_stat[(uint) SQLCOM_PURGE_UUID]), SHOW_LONG_STATUS},
  {"rbr_column_names",     (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_RBR_COLUMN_NAMES]), SHOW_LONG_STATUS},
  {"release_savepoint",    (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_RELEASE_SAVEPOINT]), SHOW_LONG_STATUS},
  {"release_explict_snapshot", (char*) offsetof(STATUS_VAR,
      com_stat[(uint) SQLCOM_RELEASE_EXPLICIT_SNAPSHOT]), SHOW_LONG_STATUS},
  {"rename_table",         (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_RENAME_TABLE]), SHOW_LONG_STATUS},
  {"rename_user",          (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_RENAME_USER]), SHOW_LONG_STATUS},
  {"repair",               (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_REPAIR]), SHOW_LONG_STATUS},
  {"replace",              (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_REPLACE]), SHOW_LONG_STATUS},
  {"replace_select",       (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_REPLACE_SELECT]), SHOW_LONG_STATUS},
  {"reset",                (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_RESET]), SHOW_LONG_STATUS},
  {"resignal",             (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_RESIGNAL]), SHOW_LONG_STATUS},
  {"revoke",               (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_REVOKE]), SHOW_LONG_STATUS},
  {"revoke_all",           (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_REVOKE_ALL]), SHOW_LONG_STATUS},
  {"rollback",             (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_ROLLBACK]), SHOW_LONG_STATUS},
  {"rollback_to_savepoint",(char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_ROLLBACK_TO_SAVEPOINT]), SHOW_LONG_STATUS},
  {"savepoint",            (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SAVEPOINT]), SHOW_LONG_STATUS},
  {"select",               (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SELECT]), SHOW_LONG_STATUS},
  {"set_option",           (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SET_OPTION]), SHOW_LONG_STATUS},
  {"signal",               (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SIGNAL]), SHOW_LONG_STATUS},
  {"show_binlog_events",   (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_BINLOG_EVENTS]), SHOW_LONG_STATUS},
  {"show_binlog_cache",    (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_BINLOG_CACHE]), SHOW_LONG_STATUS},
  {"show_binlogs",         (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_BINLOGS]), SHOW_LONG_STATUS},
  {"show_charsets",        (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_CHARSETS]), SHOW_LONG_STATUS},
  {"show_collations",      (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_COLLATIONS]), SHOW_LONG_STATUS},
  {"show_connection_attributes",(char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_CONNECTION_ATTRIBUTES]), SHOW_LONG_STATUS},
  {"show_create_db",       (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_CREATE_DB]), SHOW_LONG_STATUS},
  {"show_create_event",    (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_CREATE_EVENT]), SHOW_LONG_STATUS},
  {"show_create_func",     (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_CREATE_FUNC]), SHOW_LONG_STATUS},
  {"show_create_proc",     (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_CREATE_PROC]), SHOW_LONG_STATUS},
  {"show_create_table",    (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_CREATE]), SHOW_LONG_STATUS},
  {"show_create_trigger",  (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_CREATE_TRIGGER]), SHOW_LONG_STATUS},
  {"show_databases",       (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_DATABASES]), SHOW_LONG_STATUS},
  {"show_engine_logs",     (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_ENGINE_LOGS]), SHOW_LONG_STATUS},
  {"show_engine_mutex",    (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_ENGINE_MUTEX]), SHOW_LONG_STATUS},
  {"show_engine_status",   (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_ENGINE_STATUS]), SHOW_LONG_STATUS},
  {"show_engine_trans",    (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_ENGINE_TRX]), SHOW_LONG_STATUS},
  {"show_events",          (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_EVENTS]), SHOW_LONG_STATUS},
  {"show_errors",          (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_ERRORS]), SHOW_LONG_STATUS},
  {"show_fields",          (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_FIELDS]), SHOW_LONG_STATUS},
  {"show_function_code",   (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_FUNC_CODE]), SHOW_LONG_STATUS},
  {"show_function_status", (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_STATUS_FUNC]), SHOW_LONG_STATUS},
  {"show_grants",          (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_GRANTS]), SHOW_LONG_STATUS},
  {"show_keys",            (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_KEYS]), SHOW_LONG_STATUS},
  {"show_master_status",   (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_MASTER_STAT]), SHOW_LONG_STATUS},
  {"show_memory_status",   (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_MEMORY_STATUS]), SHOW_LONG_STATUS},
  {"show_open_tables",     (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_OPEN_TABLES]), SHOW_LONG_STATUS},
  {"show_plugins",         (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_PLUGINS]), SHOW_LONG_STATUS},
  {"show_privileges",      (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_PRIVILEGES]), SHOW_LONG_STATUS},
  {"show_procedure_code",  (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_PROC_CODE]), SHOW_LONG_STATUS},
  {"show_procedure_status",(char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_STATUS_PROC]), SHOW_LONG_STATUS},
  {"show_processlist",     (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_PROCESSLIST]), SHOW_LONG_STATUS},
  {"show_transaction_list",(char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_TRANSACTION_LIST]), SHOW_LONG_STATUS},
  {"show_srv_sessions",    (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_SRV_SESSIONS]), SHOW_LONG_STATUS},
  {"show_profile",         (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_PROFILE]), SHOW_LONG_STATUS},
  {"show_profiles",        (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_PROFILES]), SHOW_LONG_STATUS},
  {"show_relaylog_events", (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_RELAYLOG_EVENTS]), SHOW_LONG_STATUS},
  {"show_resource_counters",(char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_RESOURCE_COUNTERS]), SHOW_LONG_STATUS},
  {"show_slave_hosts",     (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_SLAVE_HOSTS]), SHOW_LONG_STATUS},
  {"show_slave_status",    (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_SLAVE_STAT]), SHOW_LONG_STATUS},
  {"show_status",          (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_STATUS]), SHOW_LONG_STATUS},
  {"show_storage_engines", (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_STORAGE_ENGINES]), SHOW_LONG_STATUS},
  {"show_table_status",    (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_TABLE_STATUS]), SHOW_LONG_STATUS},
  {"show_tables",          (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_TABLES]), SHOW_LONG_STATUS},
  {"show_triggers",        (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_TRIGGERS]), SHOW_LONG_STATUS},
  {"show_variables",       (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_VARIABLES]), SHOW_LONG_STATUS},
  {"show_warnings",        (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHOW_WARNS]), SHOW_LONG_STATUS},
  {"shutdown",             (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SHUTDOWN]), SHOW_LONG_STATUS},
  {"slave_start",          (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SLAVE_START]), SHOW_LONG_STATUS},
  {"slave_stop",           (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_SLAVE_STOP]), SHOW_LONG_STATUS},
  {"stmt_close",           (char*) offsetof(STATUS_VAR, com_stmt_close), SHOW_LONG_STATUS},
  {"stmt_execute",         (char*) offsetof(STATUS_VAR, com_stmt_execute), SHOW_LONG_STATUS},
  {"stmt_fetch",           (char*) offsetof(STATUS_VAR, com_stmt_fetch), SHOW_LONG_STATUS},
  {"stmt_prepare",         (char*) offsetof(STATUS_VAR, com_stmt_prepare), SHOW_LONG_STATUS},
  {"stmt_reprepare",       (char*) offsetof(STATUS_VAR, com_stmt_reprepare), SHOW_LONG_STATUS},
  {"stmt_reset",           (char*) offsetof(STATUS_VAR, com_stmt_reset), SHOW_LONG_STATUS},
  {"stmt_send_long_data",  (char*) offsetof(STATUS_VAR, com_stmt_send_long_data), SHOW_LONG_STATUS},
  {"truncate",             (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_TRUNCATE]), SHOW_LONG_STATUS},
  {"uninstall_plugin",     (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_UNINSTALL_PLUGIN]), SHOW_LONG_STATUS},
  {"unlock_tables",        (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_UNLOCK_TABLES]), SHOW_LONG_STATUS},
  {"update",               (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_UPDATE]), SHOW_LONG_STATUS},
  {"update_multi",         (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_UPDATE_MULTI]), SHOW_LONG_STATUS},
  {"xa_commit",            (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_XA_COMMIT]),SHOW_LONG_STATUS},
  {"xa_end",               (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_XA_END]),SHOW_LONG_STATUS},
  {"xa_prepare",           (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_XA_PREPARE]),SHOW_LONG_STATUS},
  {"xa_recover",           (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_XA_RECOVER]),SHOW_LONG_STATUS},
  {"xa_rollback",          (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_XA_ROLLBACK]),SHOW_LONG_STATUS},
  {"xa_start",             (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_XA_START]),SHOW_LONG_STATUS},
  {"purge_raft_log",       (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_PURGE_RAFT_LOG]), SHOW_LONG_STATUS},
  {"purge_raft_log_before_date", (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_PURGE_RAFT_LOG_BEFORE]), SHOW_LONG_STATUS},
  {"show_raftlogs",        (char*) offsetof(STATUS_VAR,
      com_stat[(uint) SQLCOM_SHOW_RAFT_LOGS]), SHOW_LONG_STATUS},
  {"show_raftstatus",      (char*) offsetof(STATUS_VAR,
      com_stat[(uint) SQLCOM_SHOW_RAFT_STATUS]), SHOW_LONG_STATUS},
  {NullS, NullS, SHOW_LONG}
};

LEX_CSTRING sql_statement_names[(uint) SQLCOM_END + 1];

void init_sql_statement_names()
{
  static LEX_CSTRING empty= { C_STRING_WITH_LEN("") };

  char *first_com= (char*) offsetof(STATUS_VAR, com_stat[0]);
  char *last_com= (char*) offsetof(STATUS_VAR, com_stat[(uint) SQLCOM_END]);
  int record_size= (char*) offsetof(STATUS_VAR, com_stat[1])
                   - (char*) offsetof(STATUS_VAR, com_stat[0]);
  char *ptr;
  uint i;
  uint com_index;

  for (i= 0; i < ((uint) SQLCOM_END + 1); i++)
    sql_statement_names[i]= empty;

  SHOW_VAR *var= &com_status_vars[0];
  while (var->name != NULL)
  {
    ptr= var->value;
    if ((first_com <= ptr) && (ptr <= last_com))
    {
      com_index= ((int)(ptr - first_com))/record_size;
      DBUG_ASSERT(com_index < (uint) SQLCOM_END);
      sql_statement_names[com_index].str= var->name;
      /* TODO: Change SHOW_VAR::name to a LEX_STRING, to avoid strlen() */
      sql_statement_names[com_index].length= strlen(var->name);
    }
    var++;
  }

  DBUG_ASSERT(strcmp(sql_statement_names[(uint) SQLCOM_SELECT].str, "select") == 0);
  DBUG_ASSERT(strcmp(sql_statement_names[(uint) SQLCOM_SIGNAL].str, "signal") == 0);

  sql_statement_names[(uint) SQLCOM_END].str= "error";
}

#ifdef HAVE_PSI_STATEMENT_INTERFACE
PSI_statement_info sql_statement_info[(uint) SQLCOM_END + 1];
PSI_statement_info com_statement_info[(uint) COM_MAX];

/**
  Initialize the command names array.
  Since we do not want to maintain a separate array,
  this is populated from data mined in com_status_vars,
  which already has one name for each command.
*/
void init_sql_statement_info()
{
  uint i;

  for (i= 0; i < ((uint) SQLCOM_END + 1); i++)
  {
    sql_statement_info[i].m_name= sql_statement_names[i].str;
    sql_statement_info[i].m_flags= 0;
  }

  /* "statement/sql/error" represents broken queries (syntax error). */
  sql_statement_info[(uint) SQLCOM_END].m_name= "error";
  sql_statement_info[(uint) SQLCOM_END].m_flags= 0;
}

void init_com_statement_info()
{
  uint index;

  for (index= 0; index < array_elements(com_statement_info); index++)
  {
    com_statement_info[index].m_name= command_name[index].str;
    com_statement_info[index].m_flags= 0;
  }

  /* "statement/abstract/query" can mutate into "statement/sql/..." */
  com_statement_info[(uint) COM_QUERY].m_flags= PSI_FLAG_MUTABLE;
  com_statement_info[(uint) COM_QUERY_ATTRS].m_flags= PSI_FLAG_MUTABLE;
}
#endif

/**
  Create the name of the default general log file

  @param[IN] buff    Location for building new string.
  @param[IN] log_ext The extension for the file (e.g .log)
  @returns Pointer to a new string containing the name
*/
static inline char *make_default_log_name(char *buff,const char* log_ext)
{
  return make_log_name(buff, default_logfile_name, log_ext);
}

/**
  Create a replication file name or base for file names.

  @param[in] opt Value of option, or NULL
  @param[in] def Default value if option value is not set.
  @param[in] ext Extension to use for the path

  @returns Pointer to string containing the full file path, or NULL if
  it was not possible to create the path.
 */
static inline const char *
rpl_make_log_name(const char *opt,
                  const char *def,
                  const char *ext)
{
  DBUG_ENTER("rpl_make_log_name");
  DBUG_PRINT("enter", ("opt: %s, def: %s, ext: %s", opt, def, ext));
  char buff[FN_REFLEN];
  const char *base= opt ? opt : def;
  unsigned int options=
    MY_REPLACE_EXT | MY_UNPACK_FILENAME | MY_SAFE_PATH;

  /* mysql_real_data_home_ptr  may be null if no value of datadir has been
     specified through command-line or througha cnf file. If that is the
     case we make mysql_real_data_home_ptr point to mysql_real_data_home
     which, in that case holds the default path for data-dir.
  */
  if(mysql_real_data_home_ptr == NULL)
    mysql_real_data_home_ptr= mysql_real_data_home;

  if (fn_format(buff, base, mysql_real_data_home_ptr, ext, options))
    DBUG_RETURN(strdup(buff));
  else
    DBUG_RETURN(NULL);
}


int init_common_variables(my_bool logging)
{
  umask(((~my_umask) & 0666));
  connection_errors_select= 0;
  connection_errors_accept= 0;
  connection_errors_tcpwrap= 0;
  connection_errors_internal= 0;
  connection_errors_max_connection= 0;
  connection_errors_peer_addr= 0;
  connection_errors_net_ER_NET_ERROR_ON_WRITE= 0;
  connection_errors_net_ER_NET_PACKETS_OUT_OF_ORDER= 0;
  connection_errors_net_ER_NET_PACKET_TOO_LARGE= 0;
  connection_errors_net_ER_NET_READ_ERROR= 0;
  connection_errors_net_ER_NET_READ_INTERRUPTED= 0;
  connection_errors_net_ER_NET_UNCOMPRESS_ERROR= 0;
  connection_errors_net_ER_NET_WRITE_INTERRUPTED= 0;

  my_decimal_set_zero(&decimal_zero); // set decimal_zero constant;
  tzset();      // Set tzname

  max_system_variables.pseudo_thread_id= (my_thread_id)~0;
  server_start_time= flush_status_time= my_time(0);

  rpl_filter= new Rpl_filter;
  binlog_filter= new Rpl_filter;
  if (!rpl_filter || !binlog_filter)
  {
    sql_perror("Could not allocate replication and binlog filters");
    return 1;
  }

  if (init_thread_environment() ||
      mysql_init_variables())
    return 1;

  if (ignore_db_dirs_init())
    return 1;

#ifdef HAVE_TZNAME
  {
    struct tm tm_tmp;
    localtime_r(&server_start_time,&tm_tmp);
    strmake(system_time_zone, tzname[tm_tmp.tm_isdst != 0 ? 1 : 0],
            sizeof(system_time_zone)-1);

 }
#endif
  /*
    We set SYSTEM time zone as reasonable default and
    also for failure of my_tz_init() and bootstrap mode.
    If user explicitly set time zone with --default-time-zone
    option we will change this value in my_tz_init().
  */
  global_system_variables.time_zone= my_tz_SYSTEM;

#ifdef HAVE_PSI_INTERFACE
  /*
    Complete the mysql_bin_log initialization.
    Instrumentation keys are known only after the performance schema initialization,
    and can not be set in the MYSQL_BIN_LOG constructor (called before main()).
  */
  mysql_bin_log.set_psi_keys(key_BINLOG_LOCK_index,
                             key_BINLOG_LOCK_commit,
                             key_BINLOG_LOCK_commit_queue,
                             key_BINLOG_LOCK_semisync,
                             key_BINLOG_LOCK_semisync_queue,
                             key_BINLOG_LOCK_done,
                             key_BINLOG_LOCK_flush_queue,
                             key_BINLOG_LOCK_log,
                             key_BINLOG_LOCK_sync,
                             key_BINLOG_LOCK_sync_queue,
                             key_BINLOG_LOCK_xids,
                             key_BINLOG_LOCK_non_xid_trxs,
                             key_BINLOG_LOCK_binlog_end_pos,
                             key_BINLOG_COND_done,
                             key_BINLOG_update_cond,
                             key_BINLOG_prep_xids_cond,
                             key_BINLOG_non_xid_trxs_cond,
                             key_file_binlog,
                             key_file_binlog_index);
#endif

  /*
    Init mutexes for the global MYSQL_BIN_LOG objects.
    As safe_mutex depends on what MY_INIT() does, we can't init the mutexes of
    global MYSQL_BIN_LOGs in their constructors, because then they would be
    inited before MY_INIT(). So we do it here.
  */
  mysql_bin_log.init_pthread_objects();

  /* TODO: remove this when my_time_t is 64 bit compatible */
  if (!IS_TIME_T_VALID_FOR_TIMESTAMP(server_start_time))
  {
    sql_print_error("This MySQL server doesn't support dates later then 2038");
    return 1;
  }

  if (gethostname(glob_hostname,sizeof(glob_hostname)) < 0)
  {
    strmake(glob_hostname, STRING_WITH_LEN("localhost"));
    sql_print_warning("gethostname failed, using '%s' as hostname",
                      glob_hostname);
    strmake(default_logfile_name, STRING_WITH_LEN("mysql"));
  }
  else
    strmake(default_logfile_name, glob_hostname,
      sizeof(default_logfile_name)-5);

  strmake(pidfile_name, default_logfile_name, sizeof(pidfile_name)-5);
  strmov(fn_ext(pidfile_name),".pid");    // Add proper extension


  /*
    The default-storage-engine entry in my_long_options should have a
    non-null default value. It was earlier intialized as
    (longlong)"MyISAM" in my_long_options but this triggered a
    compiler error in the Sun Studio 12 compiler. As a work-around we
    set the def_value member to 0 in my_long_options and initialize it
    to the correct value here.

    From MySQL 5.5 onwards, the default storage engine is InnoDB
    (except in the embedded server, where the default continues to
    be MyISAM)
  */
#ifdef EMBEDDED_LIBRARY
  default_storage_engine= const_cast<char *>("MyISAM");
#else
  default_storage_engine= const_cast<char *>("InnoDB");
#endif
  default_tmp_storage_engine= default_storage_engine;

  init_default_auth_plugin();

  /*
    Add server status variables to the dynamic list of
    status variables that is shown by SHOW STATUS.
    Later, in plugin_init, and mysql_install_plugin
    new entries could be added to that list.
  */
  if (add_status_vars(status_vars))
    return 1; // an error was already reported

#ifndef DBUG_OFF
  /*
    We have few debug-only commands in com_status_vars, only visible in debug
    builds. for simplicity we enable the assert only in debug builds

    There are 8 Com_ variables which don't have corresponding SQLCOM_ values:
    (TODO strictly speaking they shouldn't be here, should not have Com_ prefix
    that is. Perhaps Stmt_ ? Comstmt_ ? Prepstmt_ ?)

      Com_admin_commands       => com_other
      Com_stmt_close           => com_stmt_close
      Com_stmt_execute         => com_stmt_execute
      Com_stmt_fetch           => com_stmt_fetch
      Com_stmt_prepare         => com_stmt_prepare
      Com_stmt_reprepare       => com_stmt_reprepare
      Com_stmt_reset           => com_stmt_reset
      Com_stmt_send_long_data  => com_stmt_send_long_data

    With this correction the number of Com_ variables (number of elements in
    the array, excluding the last element - terminator) must match the number
    of SQLCOM_ constants.
  */
  compile_time_assert(sizeof(com_status_vars)/sizeof(com_status_vars[0]) - 1 ==
                     SQLCOM_END + 8);
#endif

  if (get_options(&remaining_argc, &remaining_argv, logging))
    return 1;
  set_server_version();

  // this can only be called after get_options
  init_sharding_variables();

  sql_print_information("%s (mysqld %s) starting as process %lu ...",
                        my_progname, server_version, (ulong) getpid());

#ifndef EMBEDDED_LIBRARY
  if (opt_help && !opt_verbose)
    unireg_abort(0);
#endif /*!EMBEDDED_LIBRARY*/

  DBUG_PRINT("info",("%s  Ver %s for %s on %s\n",my_progname,
         server_version, SYSTEM_TYPE,MACHINE_TYPE));

#ifdef HAVE_LARGE_PAGES
  /* Initialize large page size */
  if (opt_large_pages && (opt_large_page_size= my_get_large_page_size()))
  {
      DBUG_PRINT("info", ("Large page set, large_page_size = %d",
                 opt_large_page_size));
      my_use_large_pages= 1;
      my_large_page_size= opt_large_page_size;
  }
  else
  {
    opt_large_pages= 0;
    /*
       Either not configured to use large pages or Linux haven't
       been compiled with large page support
    */
  }
#endif /* HAVE_LARGE_PAGES */
#ifdef HAVE_SOLARIS_LARGE_PAGES
#define LARGE_PAGESIZE (4*1024*1024)  /* 4MB */
#define SUPER_LARGE_PAGESIZE (256*1024*1024)  /* 256MB */
  if (opt_large_pages)
  {
  /*
    tell the kernel that we want to use 4/256MB page for heap storage
    and also for the stack. We use 4 MByte as default and if the
    super-large-page is set we increase it to 256 MByte. 256 MByte
    is for server installations with GBytes of RAM memory where
    the MySQL Server will have page caches and other memory regions
    measured in a number of GBytes.
    We use as big pages as possible which isn't bigger than the above
    desired page sizes.
  */
   int nelem;
   size_t max_desired_page_size;
   if (opt_super_large_pages)
     max_desired_page_size= SUPER_LARGE_PAGESIZE;
   else
     max_desired_page_size= LARGE_PAGESIZE;
   nelem = getpagesizes(NULL, 0);
   if (nelem > 0)
   {
     size_t *pagesize = (size_t *) malloc(sizeof(size_t) * nelem);
     if (pagesize != NULL && getpagesizes(pagesize, nelem) > 0)
     {
       size_t max_page_size= 0;
       for (int i= 0; i < nelem; i++)
       {
         if (pagesize[i] > max_page_size &&
             pagesize[i] <= max_desired_page_size)
            max_page_size= pagesize[i];
       }
       free(pagesize);
       if (max_page_size > 0)
       {
         struct memcntl_mha mpss;

         mpss.mha_cmd= MHA_MAPSIZE_BSSBRK;
         mpss.mha_pagesize= max_page_size;
         mpss.mha_flags= 0;
         memcntl(NULL, 0, MC_HAT_ADVISE, (caddr_t)&mpss, 0, 0);
         mpss.mha_cmd= MHA_MAPSIZE_STACK;
         memcntl(NULL, 0, MC_HAT_ADVISE, (caddr_t)&mpss, 0, 0);
       }
     }
   }
  }
#endif /* HAVE_SOLARIS_LARGE_PAGES */

  longlong default_value;
  sys_var *var;
  /* Calculate and update default value for thread_cache_size. */
  if ((default_value= 8 + max_connections / 100) > 100)
    default_value= 100;
  var= intern_find_sys_var(STRING_WITH_LEN("thread_cache_size"));
  var->update_default(default_value);

  /* Calculate and update default value for host_cache_size. */
  if ((default_value= 128 + max_connections) > 628 &&
      (default_value= 628 + ((max_connections - 500) / 20)) > 2000)
    default_value= 2000;
  var= intern_find_sys_var(STRING_WITH_LEN("host_cache_size"));
  var->update_default(default_value);

  /* Fix thread_cache_size. */
  if (!thread_cache_size_specified &&
      (max_blocked_pthreads= 8 + max_connections / 100) > 100)
    max_blocked_pthreads= 100;

  /* Fix host_cache_size. */
  if (!host_cache_size_specified &&
      (host_cache_size= 128 + max_connections) > 628 &&
      (host_cache_size= 628 + ((max_connections - 500) / 20)) > 2000)
    host_cache_size= 2000;

  /* Fix back_log */
  if (back_log == 0 && (back_log= 50 + max_connections / 5) > 900)
    back_log= 900;

  unireg_init(opt_specialflag); /* Set up extern variabels */
  if (!(my_default_lc_messages=
        my_locale_by_name(lc_messages)))
  {
    sql_print_error("Unknown locale: '%s'", lc_messages);
    return 1;
  }
  global_system_variables.lc_messages= my_default_lc_messages;
  if (init_errmessage())  /* Read error messages from file */
    return 1;
  init_client_errs();
  mysql_client_plugin_init();
  lex_init();
  if (item_create_init())
    return 1;
  item_init();
#ifndef EMBEDDED_LIBRARY
  my_regex_init(&my_charset_latin1, check_enough_stack_size);
  my_string_stack_guard= check_enough_stack_size;
#else
  my_regex_init(&my_charset_latin1, NULL);
#endif
  /*
    Process a comma-separated character set list and choose
    the first available character set. This is mostly for
    test purposes, to be able to start "mysqld" even if
    the requested character set is not available (see bug#18743).
  */
  for (;;)
  {
    char *next_character_set_name= strchr(default_character_set_name, ',');
    if (next_character_set_name)
      *next_character_set_name++= '\0';
    if (!(default_charset_info=
          get_charset_by_csname(default_character_set_name,
                                MY_CS_PRIMARY, MYF(MY_WME))))
    {
      if (next_character_set_name)
      {
        default_character_set_name= next_character_set_name;
        default_collation_name= 0;          // Ignore collation
      }
      else
        return 1;                           // Eof of the list
    }
    else
      break;
  }

  if (default_collation_name)
  {
    CHARSET_INFO *default_collation;
    default_collation= get_charset_by_name(default_collation_name, MYF(0));
    if (!default_collation)
    {
#ifdef WITH_PERFSCHEMA_STORAGE_ENGINE
      buffered_logs.print();
      buffered_logs.cleanup();
#endif
      sql_print_error(ER_DEFAULT(ER_UNKNOWN_COLLATION), default_collation_name);
      return 1;
    }
    if (!my_charset_same(default_charset_info, default_collation))
    {
      sql_print_error(ER_DEFAULT(ER_COLLATION_CHARSET_MISMATCH),
          default_collation_name,
          default_charset_info->csname);
      return 1;
    }
    default_charset_info= default_collation;
  }
  /* Set collactions that depends on the default collation */
  global_system_variables.collation_server=  default_charset_info;
  global_system_variables.collation_database=  default_charset_info;

  if (is_supported_parser_charset(default_charset_info))
  {
    global_system_variables.collation_connection= default_charset_info;
    global_system_variables.character_set_results= default_charset_info;
    global_system_variables.character_set_client= default_charset_info;
  }
  else
  {
    sql_print_information("'%s' can not be used as client character set. "
                          "'%s' will be used as default client character set.",
                          default_charset_info->csname,
                          my_charset_latin1.csname);
    global_system_variables.collation_connection= &my_charset_latin1;
    global_system_variables.character_set_results= &my_charset_latin1;
    global_system_variables.character_set_client= &my_charset_latin1;
  }

  if (!(character_set_filesystem=
        get_charset_by_csname(character_set_filesystem_name,
                              MY_CS_PRIMARY, MYF(MY_WME))))
    return 1;
  global_system_variables.character_set_filesystem= character_set_filesystem;

  if (!(my_default_lc_time_names=
        my_locale_by_name(lc_time_names_name)))
  {
    sql_print_error("Unknown locale: '%s'", lc_time_names_name);
    return 1;
  }
  global_system_variables.lc_time_names= my_default_lc_time_names;

  /* check log options and issue warnings if needed */
  if (opt_log && opt_logname && !(log_output_options & LOG_FILE) &&
      !(log_output_options & LOG_NONE))
    sql_print_warning("Although a path was specified for the "
                      "--general-log-file option, log tables are used. "
                      "To enable logging to files use the --log-output=file option.");

  if (opt_slow_log && opt_slow_logname && !(log_output_options & LOG_FILE)
      && !(log_output_options & LOG_NONE))
    sql_print_warning("Although a path was specified for the "
                      "--slow-query-log-file option, log tables are used. "
                      "To enable logging to files use the --log-output=file option.");

  if (!opt_logname || !*opt_logname)
    opt_logname= make_default_log_name(logname_path, ".log");

  if (!opt_slow_logname || !*opt_slow_logname)
    opt_slow_logname= make_default_log_name(slow_logname_path, "-slow.log");

  if (opt_logname &&
      !is_valid_log_name(opt_logname, strlen(opt_logname)))
  {
    sql_print_error("Invalid value for --general_log_file: %s",
                    opt_logname);
    return 1;
  }

  if (opt_slow_logname &&
      !is_valid_log_name(opt_slow_logname, strlen(opt_slow_logname)))
  {
    sql_print_error("Invalid value for --slow_query_log_file: %s",
                    opt_slow_logname);
    return 1;
  }

  if (!opt_gap_lock_logname || !*opt_gap_lock_logname)
  {
    opt_gap_lock_logname= make_default_log_name(gap_lock_logname_path,
                                                "-gaplock.log");
  }

#if defined(ENABLED_DEBUG_SYNC)
  /* Initialize the debug sync facility. See debug_sync.cc. */
  if (debug_sync_init())
    return 1; /* purecov: tested */
#endif /* defined(ENABLED_DEBUG_SYNC) */

#if (ENABLE_TEMP_POOL)
  if (use_temp_pool && bitmap_init(&temp_pool,0,1024,1))
    return 1;
#else
  use_temp_pool= 0;
#endif

  if (my_dboptions_cache_init())
    return 1;

  /*
    Ensure that lower_case_table_names is set on system where we have case
    insensitive names.  If this is not done the users MyISAM tables will
    get corrupted if accesses with names of different case.
  */
  DBUG_PRINT("info", ("lower_case_table_names: %d", lower_case_table_names));
  lower_case_file_system= test_if_case_insensitive(mysql_real_data_home);
  if (!lower_case_table_names && lower_case_file_system == 1)
  {
    if (lower_case_table_names_used)
    {
      sql_print_error("The server option 'lower_case_table_names' is "
                      "configured to use case sensitive table names but the "
                      "data directory is on a case-insensitive file system "
                      "which is an unsupported combination. Please consider "
                      "either using a case sensitive file system for your data "
                      "directory or switching to a case-insensitive table name "
                      "mode.");
      return 1;
    }
    else
    {
      if (log_warnings)
  sql_print_warning("Setting lower_case_table_names=2 because file system for %s is case insensitive", mysql_real_data_home);
      lower_case_table_names= 2;
    }
  }
  else if (lower_case_table_names == 2 &&
           !(lower_case_file_system=
             (test_if_case_insensitive(mysql_real_data_home) == 1)))
  {
    if (log_warnings)
      sql_print_warning("lower_case_table_names was set to 2, even though your "
                        "the file system '%s' is case sensitive.  Now setting "
                        "lower_case_table_names to 0 to avoid future problems.",
      mysql_real_data_home);
    lower_case_table_names= 0;
  }
  else
  {
    lower_case_file_system=
      (test_if_case_insensitive(mysql_real_data_home) == 1);
  }

  /* Reset table_alias_charset, now that lower_case_table_names is set. */
  table_alias_charset= (lower_case_table_names ?
      &my_charset_utf8_tolower_ci :
      &my_charset_bin);

  /*
    Build do_table and ignore_table rules to hush
    after the resetting of table_alias_charset
  */
  if (rpl_filter->build_do_table_hash() ||
      rpl_filter->build_ignore_table_hash())
  {
    sql_print_error("An error occurred while building do_table"
                    "and ignore_table rules to hush.");
    return 1;
  }

  if (ignore_db_dirs_process_additions())
  {
    sql_print_error("An error occurred while storing ignore_db_dirs to a hash.");
    return 1;
  }

  return 0;
}


static int init_thread_environment()
{
  mysql_mutex_init(key_LOCK_thread_created,
                   &LOCK_thread_created, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_thread_count, &LOCK_thread_count, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_status, &LOCK_status, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_thd_remove,
                   &LOCK_thd_remove, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_delayed_insert,
                   &LOCK_delayed_insert, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_delayed_status,
                   &LOCK_delayed_status, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_delayed_create,
                   &LOCK_delayed_create, MY_MUTEX_INIT_SLOW);
  mysql_mutex_init(key_LOCK_manager,
                   &LOCK_manager, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_slave_stats_daemon,
                   &LOCK_slave_stats_daemon, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_crypt, &LOCK_crypt, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_user_conn, &LOCK_user_conn, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_active_mi, &LOCK_active_mi, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_global_system_variables,
                   &LOCK_global_system_variables, MY_MUTEX_INIT_FAST);
  mysql_rwlock_init(key_rwlock_LOCK_column_statistics, &LOCK_column_statistics);
  mysql_rwlock_init(key_rwlock_LOCK_system_variables_hash,
                    &LOCK_system_variables_hash);
  mysql_mutex_init(key_LOCK_prepared_stmt_count,
                   &LOCK_prepared_stmt_count, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_sql_slave_skip_counter,
                   &LOCK_sql_slave_skip_counter, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_slave_net_timeout,
                   &LOCK_slave_net_timeout, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_error_messages,
                   &LOCK_error_messages, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_uuid_generator,
                   &LOCK_uuid_generator, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_sql_rand,
                   &LOCK_sql_rand, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_connection_count,
                   &LOCK_connection_count, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_log_throttle_qni,
                   &LOCK_log_throttle_qni, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_log_throttle_legacy,
                   &LOCK_log_throttle_legacy, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_log_throttle_ddl,
                   &LOCK_log_throttle_ddl, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_log_throttle_sbr_unsafe,
                   &LOCK_log_throttle_sbr_unsafe, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_global_table_stats,
                   &LOCK_global_table_stats, MY_MUTEX_INIT_ERRCHK);
  mysql_mutex_init(key_LOCK_global_sql_stats,
                   &LOCK_global_sql_stats, MY_MUTEX_INIT_ERRCHK);
  mysql_mutex_init(key_LOCK_global_sql_plans,
                   &LOCK_global_sql_plans, MY_MUTEX_INIT_ERRCHK);
  mysql_mutex_init(key_LOCK_global_active_sql,
                   &LOCK_global_active_sql, MY_MUTEX_INIT_ERRCHK);
  mysql_mutex_init(key_LOCK_global_sql_findings,
                   &LOCK_global_sql_findings, MY_MUTEX_INIT_ERRCHK);
  mysql_rwlock_init(key_rwlock_sql_stats_snapshot, &LOCK_sql_stats_snapshot);
  mysql_mutex_init(key_LOCK_global_write_statistics,
                   &LOCK_global_write_statistics, MY_MUTEX_INIT_ERRCHK);
  mysql_mutex_init(key_LOCK_global_write_throttling_rules,
                   &LOCK_global_write_throttling_rules, MY_MUTEX_INIT_ERRCHK);
  mysql_mutex_init(key_LOCK_global_write_throttling_log,
                   &LOCK_global_write_throttling_log, MY_MUTEX_INIT_ERRCHK);
  mysql_mutex_init(key_LOCK_global_tx_size_histogram,
                   &LOCK_global_tx_size_histogram, MY_MUTEX_INIT_ERRCHK);
  mysql_mutex_init(key_LOCK_global_write_stat_histogram,
                   &LOCK_global_write_stat_histogram, MY_MUTEX_INIT_ERRCHK);
  mysql_mutex_init(key_LOCK_replication_lag_auto_throttling,
                   &LOCK_replication_lag_auto_throttling, MY_MUTEX_INIT_ERRCHK);
#ifdef HAVE_OPENSSL
  mysql_mutex_init(key_LOCK_des_key_file,
                   &LOCK_des_key_file, MY_MUTEX_INIT_FAST);
  mysql_rwlock_init(key_rwlock_LOCK_use_ssl, &LOCK_use_ssl);
#endif
  mysql_rwlock_init(key_rwlock_LOCK_sys_init_connect, &LOCK_sys_init_connect);
  mysql_rwlock_init(key_rwlock_LOCK_sys_init_slave, &LOCK_sys_init_slave);
#if defined(HAVE_PSI_INTERFACE)
  gap_lock_exceptions = new Regex_list_handler(
      key_rwlock_LOCK_gap_lock_exceptions);
  // use the normalized delimiter as legacy_user_name only has one pattern
  legacy_user_name_pattern = new Regex_list_handler(
      key_rwlock_LOCK_legacy_user_name_pattern, '|');
  // use the normalized delimiter as admin_users_list only has one pattern
  admin_users_list_regex = new Regex_list_handler(
      key_rwlock_LOCK_admin_users_list_regex, ',');

#else
  gap_lock_exceptions = new Regex_list_handler();
  // use the normalized delimiter as legacy_user_name only has one pattern
  legacy_user_name_pattern = new Regex_list_handler('|');
  // use the normalized delimiter as admin_users_list only has one pattern
  admin_users_list_regex = new Regex_list_handler(',');
#endif
  mysql_rwlock_init(key_rwlock_LOCK_grant, &LOCK_grant);
  mysql_cond_init(key_COND_thread_count, &COND_thread_count, NULL);
  mysql_cond_init(key_COND_connection_count, &COND_connection_count, NULL);
  mysql_cond_init(key_COND_thread_cache, &COND_thread_cache, NULL);
  mysql_cond_init(key_COND_flush_thread_cache, &COND_flush_thread_cache, NULL);
  mysql_cond_init(key_COND_manager, &COND_manager, NULL);
  mysql_cond_init(key_COND_slave_stats_daemon, &COND_slave_stats_daemon, NULL);
  mysql_mutex_init(key_LOCK_server_started,
                   &LOCK_server_started, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_COND_server_started, &COND_server_started, NULL);
  sp_cache_init();
#ifdef HAVE_EVENT_SCHEDULER
  Events::init_mutexes();
#endif
  /* Parameter for threads created for connections */
  (void) pthread_attr_init(&connection_attrib);
  (void) pthread_attr_setdetachstate(&connection_attrib,
             PTHREAD_CREATE_DETACHED);
  pthread_attr_setscope(&connection_attrib, PTHREAD_SCOPE_SYSTEM);

  if (pthread_key_create(&THR_THD,NULL) ||
      pthread_key_create(&THR_MALLOC,NULL))
  {
    sql_print_error("Can't create thread-keys");
    return 1;
  }
  return 0;
}

bool init_ssl()
{
#ifdef HAVE_OPENSSL
#ifndef HAVE_YASSL
#if defined(OPENSSL_IS_BORINGSSL) || OPENSSL_VERSION_NUMBER < 0x10100000L
  CRYPTO_malloc_init();
#else
  OPENSSL_malloc_init();
#endif // OPENSSL_VERSION_NUMBER
#endif
  ssl_start();
#ifndef EMBEDDED_LIBRARY
  if (opt_use_ssl)
  {
    /* having ssl_acceptor_fd != 0 signals the use of SSL */
    ssl_acceptor_fd = new_ssl_acceptor_fd();
    if (ssl_acceptor_fd != nullptr) {
      // Created SSL acceptor successfully
      have_ssl = SHOW_OPTION_YES;
    } else {
      // Disable SSL and fail
      opt_use_ssl = 0;
      have_ssl= SHOW_OPTION_DISABLED;
      return true;
    }
  }
  else
  {
    have_ssl= SHOW_OPTION_DISABLED;
  }
#else
  have_ssl= SHOW_OPTION_DISABLED;
#endif /* ! EMBEDDED_LIBRARY */
  if (des_key_file)
    load_des_key_file(des_key_file);
#ifndef HAVE_YASSL
  if (init_rsa_keys())
    return true;
#endif
#endif /* HAVE_OPENSSL */
  return false;
}

/*
 * Reinitialize the ssl_acceptor_fd
 * Returns true if there is an error
 */
bool refresh_ssl_acceptor() {
#ifdef HAVE_OPENSSL
  if (opt_use_ssl == false || ssl_acceptor_fd == nullptr) {
    // We cannot refresh ssl connection if we did not initialize
    // SSL in the first place
    sql_print_warning("Unable to refresh an uninitialzed SSL acceptor ");
    return true;
  }

  struct st_VioSSLFd *new_ssl_fd = new_ssl_acceptor_fd();
  if (new_ssl_fd != nullptr) {
    DBUG_PRINT("info", ("Updating ssl_acceptor_fd from (0x%lx) to (0x%lx)",
                        (long)ssl_acceptor_fd, (long)new_ssl_fd));
    free_vio_ssl_fd(ssl_acceptor_fd);
    ssl_acceptor_fd = new_ssl_fd;
    return false; // SUCCESS
  } else {
    sql_print_warning("Failed to refresh SSL cert");
    return true; // ERROR
  }
#endif /* HAVE_OPENSSL */
  return false;
}

struct st_VioSSLFd *new_ssl_acceptor_fd() {
#ifdef HAVE_OPENSSL
  if (opt_use_ssl == false) {
    return nullptr;
  }
#ifdef HAVE_YASSL
  /* yaSSL doesn't support CRL. */
  if (opt_ssl_crl || opt_ssl_crlpath) {
    sql_print_warning("Failed to setup SSL because yaSSL "
                      "doesn't support CRL");
    return nullptr;
  }
#endif /* HAVE_YASSL */
  enum enum_ssl_init_error error = SSL_INITERR_NOERROR;

  struct st_VioSSLFd *result = new_VioSSLAcceptorFd(
      opt_ssl_key, opt_ssl_cert, opt_ssl_ca, opt_ssl_capath, opt_ssl_cipher,
      &error, opt_ssl_crl, opt_ssl_crlpath);

  DBUG_PRINT("info", ("created new VioSSLFd: 0x%lx", (long)result));
  ERR_remove_state(0);
  if (result == nullptr) {
    sql_print_warning("Failed to setup SSL");
    sql_print_warning("SSL error: %s", sslGetErrString(error));
    return nullptr; // result
  }
  return result;
#endif /* HAVE_OPENSSL */
  return nullptr;
}

void end_ssl()
{
#ifdef HAVE_OPENSSL
#ifndef EMBEDDED_LIBRARY
  if (ssl_acceptor_fd)
  {
    free_vio_ssl_fd(ssl_acceptor_fd);
    ssl_acceptor_fd= 0;
  }
  have_ssl = SHOW_OPTION_DISABLED;
#endif /* ! EMBEDDED_LIBRARY */
#ifndef HAVE_YASSL
  deinit_rsa_keys();
#endif
#endif /* HAVE_OPENSSL */
}

/**
  Generate a UUID and save it into server_uuid variable.

  @return Retur 0 or 1 if an error occurred.
 */
static int generate_server_uuid()
{
  THD *thd;
  Item_func_uuid *func_uuid;
  String uuid;

  /*
    To be able to run this from boot, we allocate a temporary THD
   */
  if (!(thd=new THD))
  {
    sql_print_error("Failed to generate a server UUID because it is failed"
                    " to allocate the THD.");
    return 1;
  }
  thd->thread_stack= (char*) &thd;
  thd->store_globals();
  lex_start(thd);
  func_uuid= new (thd->mem_root) Item_func_uuid();
  func_uuid->fixed= 1;
  func_uuid->val_str(&uuid);
  delete thd;
  /* Remember that we don't have a THD */
  my_pthread_setspecific_ptr(THR_THD,  0);

  strncpy(server_uuid, uuid.c_ptr(), UUID_LENGTH);
  server_uuid[UUID_LENGTH]= '\0';
  return 0;
}

/**
  Save all options which was auto-generated by server-self into the given file.

  @param fname The name of the file in which the auto-generated options will b
  e saved.

  @return Return 0 or 1 if an error occurred.
 */
int flush_auto_options(const char* fname)
{
  File fd;
  IO_CACHE io_cache;
  int result= 0;

  if ((fd= my_open((const char *)fname, O_CREAT|O_RDWR, MYF(MY_WME))) < 0)
  {
    sql_print_error("Failed to create file(file: '%s', errno %d)", fname, my_errno);
    return 1;
  }

  if (init_io_cache(&io_cache, fd, IO_SIZE*2, WRITE_CACHE, 0L, 0, MYF(MY_WME)))
  {
    sql_print_error("Failed to create a cache on (file: %s', errno %d)", fname, my_errno);
    my_close(fd, MYF(MY_WME));
    return 1;
  }

  my_b_seek(&io_cache, 0L);
  my_b_printf(&io_cache, "%s\n", "[auto]");
  my_b_printf(&io_cache, "server-uuid=%s\n", server_uuid);

  if (flush_io_cache(&io_cache) || my_sync(fd, MYF(MY_WME)))
    result= 1;

  my_close(fd, MYF(MY_WME));
  end_io_cache(&io_cache);
  return result;
}

/**
  File 'auto.cnf' resides in the data directory to hold values of options that
  server evaluates itself and that needs to be durable to sustain the server
  restart. There is only a section ['auto'] in the file. All these options are
  in the section. Only one option exists now, it is server_uuid.
  Note, the user may not supply any literal value to these auto-options, and
  only allowed to trigger (re)evaluation.
  For instance, 'server_uuid' value will be evaluated and stored if there is
  no corresponding line in the file.
  Because of the specifics of the auto-options, they need a seperate storage.
  Meanwhile, it is the 'auto.cnf' that has the same structure as 'my.cnf'.

  @todo consider to implement sql-query-able persistent storage by WL#5279.
  @return Return 0 or 1 if an error occurred.
 */
static int init_server_auto_options()
{
  bool flush= false;
  char fname[FN_REFLEN];
  char *name= (char *)"auto";
  const char *groups[]= {"auto", NULL};
  char *uuid= 0;
  my_option auto_options[]= {
    {"server-uuid", 0, "", &uuid, &uuid,
      0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
  };

  DBUG_ENTER("init_server_auto_options");

  if (NULL == fn_format(fname, "auto.cnf", mysql_data_home, "",
                        MY_UNPACK_FILENAME | MY_SAFE_PATH))
    DBUG_RETURN(1);

  /* load_defaults require argv[0] is not null */
  char **argv= &name;
  int argc= 1;
  if (!check_file_permissions(fname, false))
  {
    /*
      Found a world writable file hence removing it as it is dangerous to write
      a new UUID into the same file.
     */
    my_delete(fname,MYF(MY_WME));
    sql_print_warning("World-writable config file '%s' has been removed.\n",
                      fname);
  }

  /* load all options in 'auto.cnf'. */
  if (my_load_defaults(fname, groups, &argc, &argv, NULL))
    DBUG_RETURN(1);

  /*
    Record the origial pointer allocated by my_load_defaults for free,
    because argv will be changed by handle_options
   */
  char **old_argv= argv;
  if (handle_options(&argc, &argv, auto_options, mysqld_get_one_option))
    DBUG_RETURN(1);

  DBUG_PRINT("info", ("uuid=%p=%s server_uuid=%s", uuid, uuid, server_uuid));
  if (uuid)
  {
    if (!Uuid::is_valid(uuid))
    {
      sql_print_error("The server_uuid stored in auto.cnf file is not a valid UUID.");
      goto err;
    }
    /*
      Uuid::is_valid() cannot do strict check on the length as it will be
      called by GTID::is_valid() as well (GTID = UUID:seq_no). We should
      explicitly add the *length check* here in this function.

      If UUID length is less than '36' (UUID_LENGTH), that error case would have
      got caught in above is_valid check. The below check is to make sure that
      length is not greater than UUID_LENGTH i.e., there are no extra characters
      (Garbage) at the end of the valid UUID.
    */
    if (strlen(uuid) > UUID_LENGTH)
    {
      sql_print_error("Garbage characters found at the end of the server_uuid "
                      "value in auto.cnf file. It should be of length '%d' "
                      "(UUID_LENGTH). Clear it and restart the server. ",
                      UUID_LENGTH);
      goto err;
    }
    strcpy(server_uuid, uuid);
  }
  else
  {
    DBUG_PRINT("info", ("generating server_uuid"));
    flush= TRUE;
    /* server_uuid will be set in the function */
    if (generate_server_uuid())
      goto err;
    DBUG_PRINT("info", ("generated server_uuid=%s", server_uuid));
    sql_print_warning("No existing UUID has been found, so we assume that this"
                      " is the first time that this server has been started."
                      " Generating a new UUID: %s.",
                      server_uuid);
  }
  /*
    The uuid has been copied to server_uuid, so the memory allocated by
    my_load_defaults can be freed now.
   */
  free_defaults(old_argv);

  if (flush)
    DBUG_RETURN(flush_auto_options(fname));
  DBUG_RETURN(0);
err:
  free_defaults(argv);
  DBUG_RETURN(1);
}


static bool
initialize_storage_engine(char *se_name, const char *se_kind,
                          plugin_ref *dest_plugin)
{
  LEX_STRING name= { se_name, strlen(se_name) };
  plugin_ref plugin;
  handlerton *hton;
  if ((plugin= ha_resolve_by_name(0, &name, FALSE)))
    hton= plugin_data(plugin, handlerton*);
  else
  {
    sql_print_error("Unknown/unsupported storage engine: %s", se_name);
    return true;
  }
  if (!ha_storage_engine_is_enabled(hton))
  {
    if (!opt_bootstrap)
    {
      sql_print_error("Default%s storage engine (%s) is not available",
                      se_kind, se_name);
      return true;
    }
    DBUG_ASSERT(*dest_plugin);
  }
  else
  {
    /*
      Need to unlock as global_system_variables.table_plugin
      was acquired during plugin_init()
    */
    plugin_unlock(0, *dest_plugin);
    *dest_plugin= plugin;
  }
  return false;
}


static int init_server_components()
{
  DBUG_ENTER("init_server_components");
  /*
    We need to call each of these following functions to ensure that
    all things are initialized so that unireg_abort() doesn't fail
  */
  mdl_init();
  if (table_def_init() | hostname_cache_init(host_cache_size))
    unireg_abort(1);

#ifdef HAVE_MY_TIMER
  if (my_timer_initialize())
  {
    sql_print_error("Failed to initialize timer component (errno %d).", errno);
    have_statement_timeout= SHOW_OPTION_NO;
  }
  else
    have_statement_timeout= SHOW_OPTION_YES;

  hhWheelTimer = std::unique_ptr<HHWheelTimer>(
      new HHWheelTimer(std::chrono::milliseconds(10)));
#else
  have_statement_timeout= SHOW_OPTION_NO;
#endif

  query_cache_set_min_res_unit(query_cache_min_res_unit);
  query_cache_init();
  query_cache_resize(query_cache_size);
  randominit(&sql_rand,(ulong) server_start_time,(ulong) server_start_time/2);
  setup_fpu();
  init_thr_lock();
#ifdef HAVE_REPLICATION
  init_slave_list();
  init_compressed_event_cache();
#endif

  /* Setup logs */

  /*
    Enable old-fashioned error log, except when the user has requested
    help information. Since the implementation of plugin server
    variables the help output is now written much later.
  */
  if (opt_error_log && !opt_help)
  {
    if (!log_error_file_ptr[0])
      fn_format(log_error_file, pidfile_name, mysql_data_home, ".err",
                MY_REPLACE_EXT); /* replace '.<domain>' by '.err', bug#4997 */
    else
      fn_format(log_error_file, log_error_file_ptr, mysql_data_home, ".err",
                MY_UNPACK_FILENAME | MY_SAFE_PATH);
    /*
      _ptr may have been set to my_disabled_option or "" if no argument was
      passed, but we need to show the real name in SHOW VARIABLES:
    */
    log_error_file_ptr= log_error_file;
    if (!log_error_file[0])
      opt_error_log= 0;                         // Too long file name
    else
    {
      my_bool res;
#ifndef EMBEDDED_LIBRARY
      res= reopen_fstreams(log_error_file, stdout, stderr);
#else
      res= reopen_fstreams(log_error_file, NULL, stderr);
#endif

      if (!res)
        setbuf(stderr, NULL);
    }
  }

  proc_info_hook= set_thd_stage_info;

  /*
    Parsing the performance schema command line option and
    adjusting the values for options such as "open_files_limit",
    "max_connections", and "table_cache_size" may have reported
    warnings/information messages.
    Now that the logger is finally available, and redirected
    to the proper file when the --log--error option is used,
    print the buffered messages to the log.
  */
  buffered_logs.print();
  buffered_logs.cleanup();

  /*
    Now that the logger is available, redirect character set
    errors directly to the logger
    (instead of the buffered_logs used at the server startup time).
  */
  my_charset_error_reporter= charset_error_reporter;

  if (xid_cache_init())
  {
    sql_print_error("Out of memory");
    unireg_abort(1);
  }

  /*
    initialize delegates for extension observers, errors have already
    been reported in the function
  */
  if (delegates_init())
    unireg_abort(1);

  /* need to configure logging before initializing storage engines */
  if (opt_log_slave_updates && !opt_bin_log)
  {
    sql_print_warning("You need to use --log-bin to make "
                    "--log-slave-updates work.");
  }
  if (binlog_format_used && !opt_bin_log)
    sql_print_warning("You need to use --log-bin to make "
                      "--binlog-format work.");

  /* Check that we have not let the format to unspecified at this point */
  DBUG_ASSERT((uint)global_system_variables.binlog_format <=
              array_elements(binlog_format_names)-1);

#ifdef HAVE_REPLICATION
  if (opt_log_slave_updates && replicate_same_server_id &&
      gtid_mode != GTID_MODE_ON)
  {
    if (opt_bin_log)
    {
      sql_print_error("using --replicate-same-server-id in conjunction with \
--log-slave-updates is impossible, it would lead to infinite loops in this \
server.");
      unireg_abort(1);
    }
    else
      sql_print_warning("using --replicate-same-server-id in conjunction with \
--log-slave-updates would lead to infinite loops in this server. However this \
will be ignored as the --log-bin option is not defined.");
  }
#endif

  opt_server_id_mask = ~ulong(0);
#ifdef HAVE_REPLICATION
  opt_server_id_mask = (opt_server_id_bits == 32)?
    ~ ulong(0) : (1 << opt_server_id_bits) -1;
  if (server_id != (server_id & opt_server_id_mask))
  {
    sql_print_error("server-id configured is too large to represent with"
                    "server-id-bits configured.");
    unireg_abort(1);
  }
#endif

  if (opt_bin_log)
  {
    /* Reports an error and aborts, if the --log-bin's path
       is a directory.*/
    if (opt_bin_logname &&
        opt_bin_logname[strlen(opt_bin_logname) - 1] == FN_LIBCHAR)
    {
      sql_print_error("Path '%s' is a directory name, please specify \
a file name for --log-bin option", opt_bin_logname);
      unireg_abort(1);
    }

    /* Reports an error and aborts, if the --log-bin-index's path
       is a directory.*/
    if (opt_binlog_index_name &&
        opt_binlog_index_name[strlen(opt_binlog_index_name) - 1]
        == FN_LIBCHAR)
    {
      sql_print_error("Path '%s' is a directory name, please specify \
a file name for --log-bin-index option", opt_binlog_index_name);
      unireg_abort(1);
    }

    char buf[FN_REFLEN];
    const char *ln;
    ln= mysql_bin_log.generate_name(opt_bin_logname, "-bin", 1, buf);
    if (!opt_bin_logname && !opt_binlog_index_name)
    {
      /*
        User didn't give us info to name the binlog index file.
        Picking `hostname`-bin.index like did in 4.x, causes replication to
        fail if the hostname is changed later. So, we would like to instead
        require a name. But as we don't want to break many existing setups, we
        only give warning, not error.
      */
      sql_print_warning("No argument was provided to --log-bin, and "
                        "--log-bin-index was not used; so replication "
                        "may break when this MySQL server acts as a "
                        "master and has his hostname changed!! Please "
                        "use '--log-bin=%s' to avoid this problem.", ln);
    }
    if (ln == buf)
    {
      my_free(opt_bin_logname);
      opt_bin_logname=my_strdup(buf, MYF(0));
    }

    if (generate_apply_file_gvars())
    {
      // NO_LINT_DEBUG
      sql_print_error("Failed to initialize apply log file in raft mode");
      unireg_abort(1);
    }

    if (enable_raft_plugin && !disable_raft_log_repointing)
    {
      // Initialize the right index file when raft is enabled
      if (mysql_bin_log.init_index_file())
      {
        // NO_LINT_DEBUG
        sql_print_error("Failed to initialize index file in raft mode");
        unireg_abort(1);
      }
    }
    else
    {
      if (mysql_bin_log.open_index_file(opt_binlog_index_name, ln, TRUE))
        unireg_abort(1);
    }

    /*
      Remove entries of logs from the index that were deleted from
      the file system but not from the index due to a crash.
    */
    if (mysql_bin_log.remove_deleted_logs_from_index(TRUE, TRUE) == LOG_INFO_IO)
    {
      unireg_abort(1);
    }
  }

  if (opt_bin_log)
  {
    log_bin_basename=
      rpl_make_log_name(opt_bin_logname, pidfile_name,
                        opt_bin_logname ? "" : "-bin");
    log_bin_index=
      rpl_make_log_name(opt_binlog_index_name, log_bin_basename, ".index");
    if (log_bin_basename == NULL || log_bin_index == NULL)
    {
      sql_print_error("Unable to create replication path names:"
                      " out of memory or path names too long"
                      " (path name exceeds " STRINGIFY_ARG(FN_REFLEN)
                      " or file name exceeds " STRINGIFY_ARG(FN_LEN) ").");
      unireg_abort(1);
    }
  }

#ifndef EMBEDDED_LIBRARY
  DBUG_PRINT("debug",
             ("opt_bin_logname: %s, opt_relay_logname: %s, pidfile_name: %s",
              opt_bin_logname, opt_relay_logname, pidfile_name));
  if (opt_relay_logname)
  {
    if (!enable_raft_plugin || disable_raft_log_repointing)
    {
      relay_log_basename= rpl_make_log_name(
          opt_relay_logname, pidfile_name,
          opt_relay_logname ? "" : "-relay-bin");

      relay_log_index= rpl_make_log_name(
          opt_relaylog_index_name, relay_log_basename, ".index");
    }
    else
    {
      relay_log_basename= my_strdup(log_bin_basename, MYF(0));
      relay_log_index= my_strdup(log_bin_index, MYF(0));
    }

    if (relay_log_basename == NULL || relay_log_index == NULL)
    {
      sql_print_error("Unable to create replication path names:"
                      " out of memory or path names too long"
                      " (path name exceeds " STRINGIFY_ARG(FN_REFLEN)
                      " or file name exceeds " STRINGIFY_ARG(FN_LEN) ").");
      unireg_abort(1);
    }
  }

#endif /* !EMBEDDED_LIBRARY */

  init_names();
  init_global_table_stats();
  init_global_db_stats();
  init_global_error_stats();
  init_global_sql_stats();
  reset_write_stat_histogram();

  /* call ha_init_key_cache() on all key caches to init them */
  process_key_caches(&ha_init_key_cache);

  /* Allow storage engine to give real error messages */
  if (ha_init_errors())
    DBUG_RETURN(1);

  if (opt_ignore_builtin_innodb)
    sql_print_warning("ignore-builtin-innodb is ignored "
                      "and will be removed in future releases.");
  if (gtid_server_init())
  {
    sql_print_error("Failed to initialize GTID structures.");
    unireg_abort(1);
  }

  if (plugin_init(&remaining_argc, remaining_argv,
                  (opt_noacl ? PLUGIN_INIT_SKIP_PLUGIN_TABLE : 0) |
                  (opt_help ? PLUGIN_INIT_SKIP_INITIALIZATION : 0)))
  {
    sql_print_error("Failed to initialize plugins.");
    unireg_abort(1);
  }
  plugins_are_initialized= TRUE;  /* Don't separate from init function */
  if ((total_ha_2pc - opt_bin_log  >= 2) && !opt_allow_multiple_engines)
  {
    sql_print_error(
      "Only one of RocksDB or InnoDB engines is supported at a time "
      "unless --allow-multiple-engines is specified."
      "Start mysqld with --skip-innodb (requires --default-tmp-storage"
      "-engine=MyISAM) if you're enabling rocksdb.");
    unireg_abort(1);
  }

  /* we do want to exit if there are any other unknown options */
  if (remaining_argc > 1)
  {
    int ho_error;
    struct my_option no_opts[]=
    {
      {0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
    };
    /*
      We need to eat any 'loose' arguments first before we conclude
      that there are unprocessed options.
    */
    my_getopt_skip_unknown= 0;

    if ((ho_error= handle_options(&remaining_argc, &remaining_argv, no_opts,
                                  mysqld_get_one_option)))
      unireg_abort(ho_error);
    /* Add back the program name handle_options removes */
    remaining_argc++;
    remaining_argv--;
    my_getopt_skip_unknown= TRUE;

    if (remaining_argc > 1)
    {
      fprintf(stderr, "%s: Too many arguments (first extra is '%s').\n"
              "Use --verbose --help to get a list of available options\n",
              my_progname, remaining_argv[1]);
      unireg_abort(1);
    }
  }

  if (opt_help)
    unireg_abort(0);

  /* if the errmsg.sys is not loaded, terminate to maintain behaviour */
  if (!my_default_lc_messages->errmsgs->is_loaded())
  {
    sql_print_error("Unable to read errmsg.sys file");
    unireg_abort(1);
  }

  /* We have to initialize the storage engines before CSV logging */
  if (ha_init())
  {
    sql_print_error("Can't init databases");
    unireg_abort(1);
  }

#ifdef WITH_CSV_STORAGE_ENGINE
  if (opt_bootstrap)
    log_output_options= LOG_FILE;
  else
    logger.init_log_tables();

  if (log_output_options & LOG_NONE)
  {
    /*
      Issue a warining if there were specified additional options to the
      log-output along with NONE. Probably this wasn't what user wanted.
    */
    if ((log_output_options & LOG_NONE) && (log_output_options & ~LOG_NONE))
      sql_print_warning("There were other values specified to "
                        "log-output besides NONE. Disabling slow "
                        "and general logs anyway.");
    logger.set_handlers(LOG_FILE, LOG_NONE, LOG_NONE, LOG_FILE);
  }
  else
  {
    /* fall back to the log files if tables are not present */
    LEX_STRING csv_name={C_STRING_WITH_LEN("csv")};
    if (!plugin_is_ready(&csv_name, MYSQL_STORAGE_ENGINE_PLUGIN))
    {
      /* purecov: begin inspected */
      sql_print_error("CSV engine is not present, falling back to the "
                      "log files");
      log_output_options= (log_output_options & ~LOG_TABLE) | LOG_FILE;
      /* purecov: end */
    }

    logger.set_handlers(LOG_FILE, opt_slow_log ? log_output_options:LOG_NONE,
                        opt_log ? log_output_options:LOG_NONE, LOG_FILE);
  }
#else
  logger.set_handlers(LOG_FILE, opt_slow_log ? LOG_FILE:LOG_NONE,
                      opt_log ? LOG_FILE:LOG_NONE, LOG_FILE);
#endif

  /*
    Set the default storage engines
  */
  if (initialize_storage_engine(default_storage_engine, "",
                                &global_system_variables.table_plugin))
    unireg_abort(1);
  if (initialize_storage_engine(default_tmp_storage_engine, " temp",
                                &global_system_variables.temp_table_plugin))
    unireg_abort(1);

  /*
    If RocksDB is disabled (InnoDB is enabled) then log a message so that users
    can easily figure out what is needed to enable RocksDB.
  */
  if (my_strcasecmp(&my_charset_latin1, default_storage_engine,
      rocksdb_engine_name)) {
      /* NO_LINT_DEBUG */
      sql_print_information("RocksDB storage engine is currently disabled. "
                            "To enable it please add the following options "
                            "to mysqld command-line: --skip-innodb --rocksdb "
                            "--default-storage-engine=rocksdb "
                            "--default-tmp-storage-engine=MyISAM or add these "
                            "settings to my.cnf configuration file under "
                            "[mysqld] section.");
  }

  if (total_ha_2pc > 1 || (1 == total_ha_2pc && opt_bin_log))
  {
    if (opt_bin_log)
      tc_log= &mysql_bin_log;
    else
      tc_log= &tc_log_mmap;
  }
  else
    tc_log= &tc_log_dummy;

  if (tc_log->open(opt_bin_log ? opt_bin_logname : opt_tc_log_file))
  {
    sql_print_error("Can't init tc log");
    unireg_abort(1);
  }

  if (ha_recover(0))
  {
    unireg_abort(1);
  }

  if (gtid_mode >= 1 && opt_bootstrap)
  {
    sql_print_warning("Bootstrap mode disables GTIDs. Bootstrap mode "
    "should only be used by mysql_install_db which initializes the MySQL "
    "data directory and creates system tables.");
    gtid_mode= 0;
  }
  if (gtid_mode >= 1 && !(opt_bin_log && opt_log_slave_updates))
  {
    sql_print_error("--gtid-mode=ON or UPGRADE_STEP_1 or UPGRADE_STEP_2 requires --log-bin and --log-slave-updates");
    unireg_abort(1);
  }
  if (gtid_mode >= 2 && !enforce_gtid_consistency)
  {
    sql_print_error("--gtid-mode=ON or UPGRADE_STEP_1 requires --enforce-gtid-consistency");
    unireg_abort(1);
  }
  if (gtid_mode == 1 || gtid_mode == 2)
  {
    sql_print_error("--gtid-mode=UPGRADE_STEP_1 or --gtid-mode=UPGRADE_STEP_2 are not yet supported");
    unireg_abort(1);
  }

  if (opt_bin_log)
  {
    uint64_t prev_hlc= 0;
    if (mysql_bin_log.init_gtid_sets(
          const_cast<Gtid_set *>(gtid_state->get_logged_gtids()),
          const_cast<Gtid_set *>(gtid_state->get_lost_gtids()),
          NULL/*last_gtid*/,
          opt_master_verify_checksum,
          true/*true=need lock*/,
          &prev_hlc,
          true /* startup=true */))
      unireg_abort(1);

    /*
      Configures what object is used by the current log to store processed
      gtid(s). This is necessary in the MYSQL_BIN_LOG::MYSQL_BIN_LOG to
      corretly compute the set of previous gtids.
    */
    mysql_bin_log.set_previous_gtid_set(
      const_cast<Gtid_set*>(gtid_state->get_logged_gtids()));

    // Update the instance's HLC clock to be greater than or equal to the HLC
    // times of trx's in all previous binlog
    mysql_bin_log.update_hlc(prev_hlc);

    if (enable_raft_plugin && !disable_raft_log_repointing)
    {
      /* If raft is enabled, we open an existing binlog file if it exists.
       * Note that in raft mode, the recovery of the transaction log will not
       * mark the log as 'closed'. This is because raft 'owns' the log and
       * will use it later when the node rejoins the ring */
      char* logname=
        mysql_bin_log.is_apply_log ? opt_apply_logname : opt_bin_logname;

      if (mysql_bin_log.open_binlog_found)
      {
        sql_print_information("In Raft mode open_binlog_found: %s\n",
                              ((logname) ? logname : "nullfile"));
        if (mysql_bin_log.open_existing_binlog(
            logname, WRITE_CACHE, max_binlog_size))
        {
          // NO_LINT_DEBUG
          sql_print_error("Failed to open existing binlog/apply-binlog when "
                          "raft is enabled");
          unireg_abort(1);
        }
      }
      else
      {
        sql_print_information("In Raft mode new binlog will be opened: %s\n",
                              ((logname) ? logname : "nullfile"));
        if (mysql_bin_log.open_binlog(logname, 0,
                                      WRITE_CACHE, max_binlog_size, false,
                                      true/*need_lock_index=true*/,
                                      true/*need_sid_lock=true*/,
                                      NULL))
          unireg_abort(1);

      }
    }
    else
    {
      if (mysql_bin_log.open_binlog(opt_bin_logname, 0,
                                       WRITE_CACHE, max_binlog_size, false,
                                       true/*need_lock_index=true*/,
                                       true/*need_sid_lock=true*/,
                                       NULL))
        unireg_abort(1);
    }
  }

  if (opt_bin_log)
  {
    size_t ilen, llen;
    const char* log_name = mysql_bin_log.get_log_fname();
    const char* index_name = mysql_bin_log.get_index_fname();

    if (!log_name || !dirname_part(binlog_file_basedir, log_name, &llen))
    {
      sql_print_error("Cannot get basedir for binlogs from (%s)\n",
                      log_name ? log_name : "NULL");
      unireg_abort(1);
    }
    if (!index_name || !dirname_part(binlog_index_basedir, index_name, &ilen))
    {
      sql_print_error("Cannot get basedir for binlog-index from (%s)\n",
                      index_name ? index_name : "NULL");
      unireg_abort(1);
    }
  }
  else
  {
    binlog_file_basedir[0] = '\0';
    binlog_index_basedir[0] = '\0';
  }

#ifdef HAVE_REPLICATION
  if (opt_bin_log && (expire_logs_days || binlog_expire_logs_seconds))
  {
    time_t purge_time= server_start_time - expire_logs_days * 24 * 60 * 60 -
                       binlog_expire_logs_seconds;
    if (purge_time >= 0)
      mysql_bin_log.purge_logs_before_date(purge_time, true);
  }
#endif

  if (opt_myisam_log)
    (void) mi_log(1);

#if defined(HAVE_MLOCKALL) && defined(MCL_CURRENT) && !defined(EMBEDDED_LIBRARY)
  if (locked_in_memory && !getuid())
  {
    if (setreuid((uid_t)-1, 0) == -1)
    {                        // this should never happen
      sql_perror("setreuid");
      unireg_abort(1);
    }
    if (mlockall(MCL_CURRENT))
    {
      if (log_warnings)
  sql_print_warning("Failed to lock memory. Errno: %d\n",errno);
      locked_in_memory= 0;
    }
    if (user_info)
      set_user(mysqld_user, user_info);
  }
  else
#endif
    locked_in_memory=0;

#ifdef HAVE_REPLICATION
  init_semi_sync_last_acked();
#endif

  ft_init_stopwords();

  init_max_user_conn();
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  init_aux_user_table_stats();
#endif
  init_update_queries();
  setup_datagram_socket(NULL, NULL, OPT_GLOBAL);
  DBUG_RETURN(0);
}

/**
   The global per-user session variables instance
*/
Per_user_session_variables per_user_session_variables;

#define CHECK_ERR_AND_VALIDATE_ONLY(err, validate_only) \
do { \
  if (err) \
    return false; \
  if (validate_only) \
    return true; \
} while (0)

/**
   Static method

   Set a session variable's value
   name  : the variable name
   value : the value
*/
bool Per_user_session_variables::set_val_do(sys_var *var, Item *item, THD *thd)
{
  LEX_STRING tmp;
  set_var set_v(OPT_SESSION, var, &tmp, item);

  /* validate and update */
  if (!set_v.check(thd) && !thd->is_error() && !set_v.update(thd))
    return true;

  return false;
}

/**
   Static method
*/
bool Per_user_session_variables::set_val(const std::string& name,
                                         const std::string& val,
                                         THD *thd)
{
  sys_var *var = intern_find_sys_var(name.c_str(), name.length());
  DBUG_ASSERT(var);

  /* validate only if thd is NULL */
  bool validate_only = !thd;

  const my_option *opt = var->get_option();
  int err = 0; // 0 means no error

  switch (opt->var_type & GET_TYPE_MASK) {
  case GET_ENUM:
    {
      bool valid = false;
      int type= find_type(val.c_str(), opt->typelib, FIND_TYPE_BASIC);
      if (type == 0)
      {
        /* Accept an integer representation of the enumerated item.*/
        char *endptr;
        ulong arg= strtoul(val.c_str(), &endptr, 10);
        if (!*endptr && arg < opt->typelib->count)
          valid = true;
      }
      else if (type > 0)
        valid = true;

      if (validate_only)
        return valid;

      Item_string item(val.c_str(), val.length(), thd->charset());
      return set_val_do(var, &item, thd);
    }
  case GET_BOOL: /* If argument differs from 0, enable option, else disable */
    {
      my_bool v = my_get_bool_argument(opt, val.c_str(), &err);

      CHECK_ERR_AND_VALIDATE_ONLY(err, validate_only);

      Item_int item(v);
      return set_val_do(var, &item, thd);
    }
  case GET_INT:
    {
      int v = (int) getopt_ll((char *)val.c_str(), opt, &err);

      CHECK_ERR_AND_VALIDATE_ONLY(err, validate_only);

      Item_int item(v);
      return set_val_do(var, &item, thd);
    }
  case GET_UINT:
    {
      uint v = (uint) getopt_ull((char *)val.c_str(), opt, &err);

      CHECK_ERR_AND_VALIDATE_ONLY(err, validate_only);

      Item_uint item(v);
      return set_val_do(var, &item, thd);
    }
  case GET_LONG:
    {
      long v = (long) getopt_ll((char *)val.c_str(), opt, &err);

      CHECK_ERR_AND_VALIDATE_ONLY(err, validate_only);

      Item_int item((int32)v);
      return set_val_do(var, &item, thd);
    }
  case GET_ULONG:
    {
      /* Note that the name says the type is ulong
         but the actual internal type is long */
      long v = (long) getopt_ull((char *)val.c_str(), opt, &err);

      CHECK_ERR_AND_VALIDATE_ONLY(err, validate_only);

      Item_int item((int32)v);
      return set_val_do(var, &item, thd);
    }
  case GET_LL:
    {
      longlong v = (longlong) getopt_ll((char *)val.c_str(), opt, &err);

      CHECK_ERR_AND_VALIDATE_ONLY(err, validate_only);

      Item_int item(v);
      return set_val_do(var, &item, thd);
    }
  case GET_ULL:
    {
      ulonglong v = (ulonglong) getopt_ull((char *)val.c_str(), opt, &err);

      CHECK_ERR_AND_VALIDATE_ONLY(err, validate_only);

      Item_uint item(v);
      return set_val_do(var, &item, thd);
    }
  case GET_DOUBLE:
    {
      double v = getopt_double((char *)val.c_str(), opt, &err);

      CHECK_ERR_AND_VALIDATE_ONLY(err, validate_only);

      Item_uint item(v);
      return set_val_do(var, &item, thd);
    }
  default:
    /* All other types are not supported */
    break;
  }
  return false;
}

/**
   Static method

   Validate a session variable name and its value
   name  : the variable name
   value : the value
*/
bool Per_user_session_variables::validate_val(const std::string& name,
                                              const std::string& val)
{
  return set_val(name, val, NULL);
}

/**
   Static method

   Validate and store a per user session variable segment
   users : user list have the same settings
   vars  : session variable list
*/
bool Per_user_session_variables::store(User_session_vars_sp& per_user_vars,
                                       const std::vector<std::string>& users,
                                       const std::vector<Session_var>& vars)
{
  if (users.empty() || vars.empty())
    return false;

  Session_vars_sp sp_vars = std::make_shared<Session_vars>();
  for(auto it = vars.begin(); it != vars.end(); ++it)
  {
    const char* name = it->first.c_str();
    size_t name_len = it->first.size();
    const char* val = it->second.c_str();

    /* If the name is a valid var name */
    sys_var *var = intern_find_sys_var(name, name_len);
    if (!var)
      return false;

    /* If the value is valid */
    if (!validate_val(it->first, it->second))
      return false;

    /* Insert the name value pair */
    std::pair<Session_vars_it, bool> ret =
      sp_vars->insert(std::make_pair(name, val));

    /* Duplicate entries are not allowed */
    if (!ret.second)
      return false;
  }
  DBUG_ASSERT(sp_vars->size() == vars.size());

  /* We don't validate user names so that per user session variables
     can be defined earlier and users can be added later */
  for(auto it = users.begin(); it != users.end(); ++it)
  {
    std::pair<User_session_vars_it, bool> ret =
      per_user_vars->insert(std::make_pair(*it, sp_vars));

    /* Duplicate entries are not allowed */
    if (!ret.second)
      return false;
  }
  return true;
}

/**
   Static method
*/
char Get_per_user_session_var_user_name_delimiter()
{
  if (per_user_session_var_user_name_delimiter_ptr &&
      strlen(per_user_session_var_user_name_delimiter_ptr) > 0)
  {
    return per_user_session_var_user_name_delimiter_ptr[0];
  }
  return default_per_user_session_var_user_name_delimiter;
}

bool Per_user_session_variables::init_do(User_session_vars_sp& per_user_vars,
                                         const char *sys_var_str)
{
  /* NULL and "" are valid values */
  if (!sys_var_str || !strlen(sys_var_str))
    return true;

  std::vector<std::string> users;
  users.reserve(16);

  std::vector<Session_var> vars;
  vars.reserve(32);

  std::string key, val;

  char user_name_delimiter = Get_per_user_session_var_user_name_delimiter();
  char delimiters[4] = { '\0', '=', ',', '\0' };
  delimiters[0] = user_name_delimiter;
  static const char *invalid_tokens = " \t\"\\/';";
  const char *p = sys_var_str;
  const char *prev = p;

  /* Parsing the string value, for example:
     --per-user-session-var-default-val="u1:u2:u3:k1=v1,k2=v2,k3=v3,u4:k4=v4"
  */
  while (*p)
  {
    /* Assume the user name delimiter is ':'. Get a token in the forms of
       "foo:", "bar=", "baz,", or the last one in the form of "tail" */
    while (*p && !strchr(delimiters, *p))
    {
      /* Return immediately if invalid tokens are seen */
      if (strchr(invalid_tokens, *p++))
        return false;
    }

    /* Consecutive delimiters */
    if (p == prev)
      return false;

    if (*p == user_name_delimiter)
    {
      /* Return if we have a key with no value */
      if (!key.empty())
        return false;

      if (!vars.empty())
      {
        /* Return false if we have key/values but no users, otherwise
           store the segment of per user session variables. */
        if (users.empty() || !store(per_user_vars, users, vars))
          return false;
        /* Start a new segment */
        users.clear();
        vars.clear();
      }
      users.push_back(std::string(prev, p - prev));
    }
    /* Saw a key */
    else if (*p == '=')
    {
      /* Return if we have a key with no value */
      if (!key.empty())
        return false;
      key = std::string(prev, p - prev);
    }
    /* Saw a val */
    else if (*p == ',' || *p == '\0')
    {
      /* Return false if we don't have a valid key,
         Or we have key/values but no users. */
      if (key.empty() || users.empty())
        return false;
      val = std::string(prev, p - prev);
      vars.push_back(Session_var(key, val));
      if (*p == '\0')
      {
        /* Store the segment of per user session variables */
        if (!store(per_user_vars, users, vars))
          return false;

        /* Done */
        return true;
      }
      key.clear();
    }

    /* Skip the delimiter and start a new token */
    prev = ++p;
  }
  /* Saw end of string at wrong stage */
  return false;
}

/**
   Set per user session variables for a THD
*/
bool Per_user_session_variables::set_thd(THD *thd)
{
  bool ret = true;
  std::string err_msg;

  /* Read-lock */
  mysql_rwlock_rdlock(&LOCK_per_user_session_var);

  if (per_user_session_vars)
  {
    const char *user = thd->main_security_ctx.user;
    if (user)
    {
      User_session_vars_it it = per_user_session_vars->find(user);
      if (it != per_user_session_vars->end())
      {
        /* Temporarily grant super privilege to this user if it doesn't
           have it because root privilege will be needed to set some
           session variables like binlog_format.
        */
        bool temp_super_acl = false;
        if (!(thd->security_ctx->master_access & SUPER_ACL))
        {
          thd->security_ctx->master_access |= SUPER_ACL;
          temp_super_acl = true;
        }

        Session_vars_sp vars_sp = it->second;
        DBUG_ASSERT(vars_sp->size() > 0);

        for (Session_vars_it it = vars_sp->begin();
             it != vars_sp->end(); ++it)
        {
          if (!set_val(it->first, it->second, thd))
          {
            /* Log the error and continue */
            ret = false;
            if (!err_msg.empty())
              err_msg += ",";
            err_msg += it->first + "=" + it->second;
          }
        }
        /* Remove the temporary super ACL */
        if (temp_super_acl)
          thd->security_ctx->master_access &= ~SUPER_ACL;
      }
    }
    else {
      /* The user wasn't set. This should never happen. */
      DBUG_ASSERT(false);
      ret = false;
    }
  }
  mysql_rwlock_unlock(&LOCK_per_user_session_var);

  if (!ret)
  {
    /* Log the errors into server log */
    DBUG_ASSERT(!err_msg.empty());
    static const std::string s =
      "Failed to set per-user session variables for user ";
    err_msg = s + thd->main_security_ctx.user + ":" + err_msg;
    fprintf(stderr, "[Warning] %s\n", err_msg.c_str());
  }
  return ret;
}

void Per_user_session_variables::print()
{
  char name_delimiter = Get_per_user_session_var_user_name_delimiter();

  /* Read-lock */
  mysql_rwlock_rdlock(&LOCK_per_user_session_var);

  if (per_user_session_vars)
  {
    for (auto it1 = per_user_session_vars->begin();
         it1 != per_user_session_vars->end();
         ++it1)
    {
      std::string msg = it1->first + name_delimiter;
      for (auto it2 = it1->second->begin();
           it2 != it1->second->end();
           ++it2)
      {
        if (msg.back() != name_delimiter)
          msg += ",";
        msg += it2->first + "=" + it2->second;
      }
      fprintf(stderr, "[Per-user session variables] %s\n", msg.c_str());
    }
  }
  mysql_rwlock_unlock(&LOCK_per_user_session_var);
}

bool Per_user_session_variables::init(const char *sys_var_str)
{
  /* Create a new hash table */
  User_session_vars_sp per_user_vars = std::make_shared<User_session_vars>();

  bool ret = false;
  if (init_do(per_user_vars, sys_var_str))
  {
    /* Write-lock */
    mysql_rwlock_wrlock(&LOCK_per_user_session_var);
    per_user_session_vars.swap(per_user_vars);
    mysql_rwlock_unlock(&LOCK_per_user_session_var);
    ret = true;
  }

  /* Print all the values in the hash table into log file. */
  print();

  return ret;
}

bool Per_user_session_variables::init()
{
  /* This is called during server starting time so it is safe
     to access the global pointer directly */
  if (!per_user_session_var_default_val_ptr ||
      !strlen(per_user_session_var_default_val_ptr))
  {
    return true;
  }
  return init(per_user_session_var_default_val_ptr);
}


#ifndef EMBEDDED_LIBRARY

static void create_shutdown_thread()
{
#ifdef __WIN__
  hEventShutdown=CreateEvent(0, FALSE, FALSE, shutdown_event_name);
  pthread_t hThread;
  int error;
  if ((error= mysql_thread_create(key_thread_handle_shutdown,
                                  &hThread, &connection_attrib,
                                  handle_shutdown, 0)))
    sql_print_warning("Can't create thread to handle shutdown requests"
                      " (errno= %d)", error);

  // On "Stop Service" we have to do regular shutdown
  Service.SetShutdownEvent(hEventShutdown);
#endif /* __WIN__ */
}

#endif /* EMBEDDED_LIBRARY */


#if (defined(_WIN32) || defined(HAVE_SMEM)) && !defined(EMBEDDED_LIBRARY)
static void handle_connections_methods()
{
  pthread_t hThread;
  int error;
  DBUG_ENTER("handle_connections_methods");
  if (hPipe == INVALID_HANDLE_VALUE &&
      (!have_tcpip || opt_disable_networking) &&
      !opt_enable_shared_memory)
  {
    sql_print_error("TCP/IP, --shared-memory, or --named-pipe should be configured on NT OS");
    unireg_abort(1);        // Will not return
  }

  mysql_mutex_lock(&LOCK_thread_count);
  mysql_cond_init(key_COND_handler_count, &COND_handler_count, NULL);
  handler_count=0;
  mysql_rwlock_init(key_rwlock_LOCK_named_pipe_full_access_group,
                    &LOCK_named_pipe_full_access_group);
  if (hPipe != INVALID_HANDLE_VALUE)
  {
    handler_count++;
    if ((error= mysql_thread_create(key_thread_handle_con_namedpipes,
                                    &hThread, &connection_attrib,
                                    handle_connections_namedpipes, 0)))
    {
      sql_print_warning("Can't create thread to handle named pipes"
                        " (errno= %d)", error);
      handler_count--;
    }
  }
  if (have_tcpip && !opt_disable_networking)
  {
    handler_count++;
    if ((error= mysql_thread_create(key_thread_handle_con_sockets,
                                    &hThread, &connection_attrib,
                                    handle_connections_sockets_thread, 0)))
    {
      sql_print_warning("Can't create thread to handle TCP/IP"
                        " (errno= %d)", error);
      handler_count--;
    }
  }
#ifdef HAVE_SMEM
  if (opt_enable_shared_memory)
  {
    handler_count++;
    if ((error= mysql_thread_create(key_thread_handle_con_sharedmem,
                                    &hThread, &connection_attrib,
                                    handle_connections_shared_memory, 0)))
    {
      sql_print_warning("Can't create thread to handle shared memory",
                        " (errno= %d)", error);
      handler_count--;
    }
  }
#endif

  while (handler_count > 0)
    mysql_cond_wait(&COND_handler_count, &LOCK_thread_count);
  mysql_mutex_unlock(&LOCK_thread_count);
  DBUG_VOID_RETURN;
}

void decrement_handler_count()
{
  mysql_mutex_lock(&LOCK_thread_count);
  handler_count--;
  mysql_cond_signal(&COND_handler_count);
  mysql_mutex_unlock(&LOCK_thread_count);
  my_thread_end();
}
#else
#define decrement_handler_count()
#endif /* defined(_WIN32) || defined(HAVE_SMEM) */

#if (!defined(_WIN32) && !defined(HAVE_SMEM) && !defined(EMBEDDED_LIBRARY))

pthread_handler_t handle_connections_admin_sockets_thread(void *arg)
{
  sql_print_information("admin socket thread starting");

  my_thread_init();
  handle_connections_sockets(true);

  sql_print_information("admin socket thread exiting");

  mysql_mutex_lock(&LOCK_thread_count);

  DBUG_ASSERT(admin_select_thread_running);
  admin_select_thread_running = false;
  my_thread_end();

  mysql_cond_broadcast(&COND_thread_count);
  mysql_mutex_unlock(&LOCK_thread_count);

  pthread_exit(0);
  return 0;
}

#if defined(ALTER_THREAD_NAMES)
static void alter_thread_name(const char *prefix, int sid) {
  pthread_t self = pthread_self();
  char xn[16];
  std::snprintf(xn, sizeof(xn), "%s%d", prefix, (int)sid);
  (void)pthread_setname_np(self, xn);
}
#endif

static void handle_connections_sockets_all()
{
  DBUG_ENTER("handle_connections_sockets_all");

  mysql_mutex_lock(&LOCK_thread_count);

  int error;
  // handle admin connections in a separate thread.
  if (mysql_socket_getfd(admin_ip_sock) != INVALID_SOCKET &&
      !opt_disable_networking)
  {
    DBUG_ASSERT(!admin_select_thread_running);
    admin_select_thread_running = true;
    if ((error= mysql_thread_create(key_thread_handle_con_admin_sockets,
                                    &admin_select_thread, &connection_attrib,
                                    handle_connections_admin_sockets_thread,0)))
    {
      sql_print_error("Can't create thread to handle TCP/IP for admin "
                      "(errno= %d)", error);
      admin_select_thread_running = false;
    }
  }

  pthread_t hThread;
#if defined(HAVE_SOREUSEPORT)
  if (gl_socket_sharding) {
    if (mysql_socket_getfd(ip_sock) != INVALID_SOCKET &&
        !opt_disable_networking)
    {
      for (uint tid = 1; tid < num_sharded_sockets; tid++) {
        handler_count++;
        if ((error= mysql_thread_create(key_thread_handle_con_sockets,
                      &hThread, &connection_attrib,
                      soreusethread, reinterpret_cast<void*>(tid))))
        {
          // NO_LINT_DEBUG
          sql_print_warning("Can't create thread to handle TCP/IP"
                            " (errno= %d)", error);
          handler_count--;
        }
      }
    }
#if defined(ALTER_THREAD_NAMES)
    alter_thread_name("soreuse_", 0);
#endif
  }
#endif

  if (separate_conn_handling_thread)
  {
    for (uint chid = 0; chid < num_conn_handling_threads; chid++) {
      handler_count++;
      if ((error= mysql_thread_create(key_thread_handle_con_sockets,
        &hThread, &connection_attrib,
        dedicated_conn_handling_thread, reinterpret_cast<void*>(chid))))
      {
        // NO_LINT_DEBUG
        sql_print_warning("Can't create dedicated thread to handle TCP/IP"
                                " (errno= %d)", error);
        handler_count--;
      }
    }
  }

  MYSQL_SOCKET *socket_ptr = NULL;
#if defined(HAVE_SOREUSEPORT)
  if (gl_socket_sharding) {
    socket_ptr = &ip_sharded_sockets[0];
  }
#endif

  mysql_mutex_unlock(&LOCK_thread_count);

  // handle regular connections to UNIX socket and TCP socket in main thread.
  handle_connections_sockets(false, socket_ptr);

  mysql_mutex_lock(&LOCK_thread_count);

  // waiting for admin select thread exit and all connection
  // handlers
  while (admin_select_thread_running || handler_count > 0)
    mysql_cond_wait(&COND_thread_count, &LOCK_thread_count);

  mysql_mutex_unlock(&LOCK_thread_count);
  DBUG_VOID_RETURN;
}
#endif /* !_WIN32 && !HAVE_SMEM */

#ifndef EMBEDDED_LIBRARY
#ifndef DBUG_OFF
/*
  Debugging helper function to keep the locale database
  (see sql_locale.cc) and max_month_name_length and
  max_day_name_length variable values in consistent state.
*/
static void test_lc_time_sz()
{
  DBUG_ENTER("test_lc_time_sz");
  for (MY_LOCALE **loc= my_locales; *loc; loc++)
  {
    uint max_month_len= 0;
    uint max_day_len = 0;
    for (const char **month= (*loc)->month_names->type_names; *month; month++)
    {
      set_if_bigger(max_month_len,
                    my_numchars_mb(&my_charset_utf8_general_ci,
                                   *month, *month + strlen(*month)));
    }
    for (const char **day= (*loc)->day_names->type_names; *day; day++)
    {
      set_if_bigger(max_day_len,
                    my_numchars_mb(&my_charset_utf8_general_ci,
                                   *day, *day + strlen(*day)));
    }
    if ((*loc)->max_month_name_length != max_month_len ||
        (*loc)->max_day_name_length != max_day_len)
    {
      DBUG_PRINT("Wrong max day name(or month name) length for locale:",
                 ("%s", (*loc)->name));
      DBUG_ASSERT(0);
    }
  }
  DBUG_VOID_RETURN;
}
#endif//DBUG_OFF

#ifdef TARGET_OS_LINUX
/*
 * Auto detect if we support flash cache on the host system.
 * This needs to be called before we setuid away from root
 * to avoid permission problems on opening the device node.
 */
static void init_cachedev(void)
{
  struct stat st_data_home_dir;
  struct stat st;
  struct mntent *ent;
  pid_t pid = getpid();
  FILE *mounts;
  const char *error_message= NULL;

  // disabled by default
  cachedev_fd = -1;
  cachedev_enabled= FALSE;

  if (stat(mysql_real_data_home, &st_data_home_dir) < 0)
  {
    error_message= "statfs failed";
    goto epilogue;
  }

  mounts = setmntent("/etc/mtab", "r");
  if (mounts == NULL)
  {
    error_message= "setmntent failed";
    goto epilogue;
  }

  while ((ent = getmntent(mounts)) != NULL)
  {
    if (stat(ent->mnt_dir, &st) < 0)
      continue;
    if (memcmp(&st.st_dev, &st_data_home_dir.st_dev, sizeof(dev_t)) == 0)
      break;
  }
  endmntent(mounts);

  if (ent == NULL)
  {
    error_message= "getmntent loop failed";
    goto epilogue;
  }

  cachedev_fd = open(ent->mnt_fsname, O_RDONLY);
  if (cachedev_fd < 0)
  {
    error_message= "open flash device failed";
    goto epilogue;
  }

  /* cleanup previous whitelistings */
  if (ioctl(cachedev_fd, FLASHCACHEDELALLWHITELIST, &pid) < 0)
  {
    close(cachedev_fd);
    cachedev_fd = -1;
    error_message= "ioctl failed";
  } else {
    ioctl(cachedev_fd, FLASHCACHEADDWHITELIST, &pid);
  }

epilogue:
  sql_print_information("Flashcache bypass: %s",
      (cachedev_fd > 0) ? "enabled" : "disabled");
  if (error_message)
    sql_print_information("Flashcache setup error is : %s\n", error_message);
  else
    cachedev_enabled= TRUE;

}
#endif /* TARGET_OS_LINUX */


#ifdef __WIN__
int win_main(int argc, char **argv)
#else
int mysqld_main(int argc, char **argv)
#endif
{
  /*
    Perform basic thread library and malloc initialization,
    to be able to read defaults files and parse options.
  */
  my_progname= argv[0];

#ifndef _WIN32
  // For windows, my_init() is called from the win specific mysqld_main
  if (my_init())                 // init my_sys library & pthreads
  {
    fprintf(stderr, "my_init() failed.");
    return 1;
  }
#endif

  orig_argc= argc;
  orig_argv= argv;
  my_getopt_use_args_separator= TRUE;
  my_defaults_read_login_file= FALSE;
  if (load_defaults(MYSQL_CONFIG_NAME, load_default_groups, &argc, &argv))
    return 1;
  my_getopt_use_args_separator= FALSE;
  defaults_argc= argc;
  defaults_argv= argv;
  remaining_argc= argc;
  remaining_argv= argv;

  /* Must be initialized early */
  init_my_timer();

  /* Must be initialized early for comparison of options name */
  system_charset_info= &my_charset_utf8_general_ci;

  init_sql_statement_names();
  sys_var_init();

  int ho_error;

#ifdef WITH_PERFSCHEMA_STORAGE_ENGINE
  /*
    Initialize the array of performance schema instrument configurations.
  */
  init_pfs_instrument_array();
#endif /* WITH_PERFSCHEMA_STORAGE_ENGINE */

  ho_error= handle_early_options(TRUE);

  {
    ulong requested_open_files;
    adjust_related_options(&requested_open_files);

#ifdef WITH_PERFSCHEMA_STORAGE_ENGINE
    if (ho_error == 0)
    {
      if (pfs_param.m_enabled && !opt_help && !opt_bootstrap)
      {
        /* Add sizing hints from the server sizing parameters. */
        pfs_param.m_hints.m_table_definition_cache= table_def_size;
        pfs_param.m_hints.m_table_open_cache= table_cache_size;
        pfs_param.m_hints.m_max_connections= max_connections;
	pfs_param.m_hints.m_open_files_limit= requested_open_files;
        PSI_hook= initialize_performance_schema(&pfs_param);
        if (PSI_hook == NULL)
        {
          pfs_param.m_enabled= false;
          buffered_logs.buffer(WARNING_LEVEL,
                               "Performance schema disabled (reason: init failed).");
        }
      }
    }
#else
  /*
    Other provider of the instrumentation interface should
    initialize PSI_hook here:
    - HAVE_PSI_INTERFACE is for the instrumentation interface
    - WITH_PERFSCHEMA_STORAGE_ENGINE is for one implementation
      of the interface,
    but there could be alternate implementations, which is why
    these two defines are kept separate.
  */
#endif /* WITH_PERFSCHEMA_STORAGE_ENGINE */
  }

#ifdef HAVE_PSI_INTERFACE
  /*
    Obtain the current performance schema instrumentation interface,
    if available.
  */
  if (PSI_hook)
  {
    PSI *psi_server= (PSI*) PSI_hook->get_interface(PSI_CURRENT_VERSION);
    if (likely(psi_server != NULL))
    {
      set_psi_server(psi_server);

      /*
        Now that we have parsed the command line arguments, and have initialized
        the performance schema itself, the next step is to register all the
        server instruments.
      */
      init_server_psi_keys();
      /* Instrument the main thread */
      PSI_thread *psi= PSI_THREAD_CALL(new_thread)(key_thread_main, NULL, 0);
      PSI_THREAD_CALL(set_thread)(psi);

      /*
        Now that some instrumentation is in place,
        recreate objects which were initialised early,
        so that they are instrumented as well.
      */
      my_thread_global_reinit();
    }
  }
#endif /* HAVE_PSI_INTERFACE */

  init_error_log_mutex();

  /* Set signal used to kill MySQL */
#if defined(SIGUSR2)
  thr_kill_signal= SIGUSR2;
#else
  thr_kill_signal= SIGINT;
#endif

  /* Initialize audit interface globals. Audit plugins are inited later. */
  mysql_audit_initialize();

#ifndef EMBEDDED_LIBRARY
    Srv_session::module_init();
#endif

  /*
    Perform basic logger initialization logger. Should be called after
    MY_INIT, as it initializes mutexes. Log tables are inited later.
  */
  logger.init_base();

  if (ho_error)
  {
    /*
      Parsing command line option failed,
      Since we don't have a workable remaining_argc/remaining_argv
      to continue the server initialization, this is as far as this
      code can go.
      This is the best effort to log meaningful messages:
      - messages will be printed to stderr, which is not redirected yet,
      - messages will be printed in the NT event log, for windows.
    */
    buffered_logs.print();
    buffered_logs.cleanup();
    /*
      Not enough initializations for unireg_abort()
      Using exit() for windows.
    */
    exit (ho_error);
  }

#ifdef _CUSTOMSTARTUPCONFIG_
  if (_cust_check_startup())
  {
    / * _cust_check_startup will report startup failure error * /
    exit(1);
  }
#endif

  if (init_common_variables(TRUE))
    unireg_abort(1);        // Will do exit

  /*
    All sys-var are processed so it is time handle
    the per-user session variables.
  */
  if (!per_user_session_variables.init())
  {
    fprintf(stderr, "[ERROR] init_per_user_session_variables() failed.\n");
    unireg_abort(1);
  }

  /* Delete the shutdown file if it exists */
  if (delete_shutdown_file())
  {
    /* Server will fail to start if it failed to delete shutdown file */
    fprintf(stderr, "[ERROR] Failed to delete shutdown file.\n");
    unireg_abort(1);
  }

  my_init_signals();

  size_t guardize= 0;
  int retval= pthread_attr_getguardsize(&connection_attrib, &guardize);
  DBUG_ASSERT(retval == 0);
  if (retval != 0)
    guardize= my_thread_stack_size;

#if defined(__ia64__) || defined(__ia64)
  /*
    Peculiar things with ia64 platforms - it seems we only have half the
    stack size in reality, so we have to double it here
  */
  guardize= my_thread_stack_size;
#endif

  pthread_attr_setstacksize(&connection_attrib,
                            my_thread_stack_size + guardize);

#ifdef HAVE_PTHREAD_ATTR_GETSTACKSIZE
  {
    /* Retrieve used stack size;  Needed for checking stack overflows */
    size_t stack_size= 0;
    pthread_attr_getstacksize(&connection_attrib, &stack_size);

    /* We must check if stack_size = 0 as Solaris 2.9 can return 0 here */
    if (stack_size && stack_size < (my_thread_stack_size + guardize))
    {
      if (log_warnings)
        sql_print_warning("Asked for %lu thread stack, but got %ld",
                          my_thread_stack_size + guardize, (long) stack_size);
#if defined(__ia64__) || defined(__ia64)
      my_thread_stack_size= stack_size / 2;
#else
      my_thread_stack_size= stack_size - guardize;
#endif
    }
  }
#endif

  (void) thr_setconcurrency(concurrency); // 10 by default

  select_thread=pthread_self();
  select_thread_in_use=1;

#ifdef HAVE_LIBWRAP
  libwrapName= my_progname+dirname_length(my_progname);
  openlog(libwrapName, LOG_PID, LOG_AUTH);
#endif /* HAVE_LIBWRAP */

#ifndef DBUG_OFF
  test_lc_time_sz();
  srand(time(NULL));
#endif

#ifdef TARGET_OS_LINUX
  init_cachedev();
#endif /* TARGET_OS_LINUX */

  /*
    We have enough space for fiddling with the argv, continue
  */
  check_data_home(mysql_real_data_home);
  if (my_setwd(mysql_real_data_home,MYF(MY_WME)) && !opt_help)
    unireg_abort(1);        /* purecov: inspected */

  if ((user_info= check_user(mysqld_user)))
  {
#if defined(HAVE_MLOCKALL) && defined(MCL_CURRENT)
    if (locked_in_memory) // getuid() == 0 here
      set_effective_user(user_info);
    else
#endif
      set_user(mysqld_user, user_info);
  }

  if (opt_bin_log && server_id == 0)
  {
    server_id= 1;
#ifdef EXTRA_DEBUG
    sql_print_warning("You have enabled the binary log, but you haven't set "
                      "server-id to a non-zero value: we force server id to 1; "
                      "updates will be logged to the binary log, but "
                      "connections from slaves will not be accepted.");
#endif
  }

  /*
   The subsequent calls may take a long time : e.g. innodb log read.
   Thus set the long running service control manager timeout
  */
#if defined(_WIN32) && !defined(EMBEDDED_LIBRARY)
  Service.SetSlowStarting(slow_start_timeout);
#endif

  db_ac = new AC();
  db_ac->update_max_running_queries(opt_max_running_queries);
  db_ac->update_max_waiting_queries(opt_max_waiting_queries);
  db_ac->update_queue_weights(admission_control_weights);
  if (init_server_components())
    unireg_abort(1);

  /*
    Each server should have one UUID. We will create it automatically, if it
    does not exist.
   */
  if (!opt_bootstrap)
  {
    if (init_server_auto_options())
    {
      sql_print_error("Initialzation of the server's UUID failed because it could"
                      " not be read from the auto.cnf file. If this is a new"
                      " server, the initialization failed because it was not"
                      " possible to generate a new UUID.");
      unireg_abort(1);
    }

    if (opt_bin_log)
    {
      /*
        Add server_uuid to the sid_map.  This must be done after
        server_uuid has been initialized in init_server_auto_options and
        after the binary log (and sid_map file) has been initialized in
        init_server_components().

        No error message is needed: init_sid_map() prints a message.
      */
      global_sid_lock->rdlock();
      int ret= gtid_state->init();
      global_sid_lock->unlock();
      if (ret)
        unireg_abort(1);
    }
  }

  if (init_ssl())
    unireg_abort(1);
  network_init();

#ifdef __WIN__
  if (!opt_console)
  {
    if (reopen_fstreams(log_error_file, stdout, stderr))
      unireg_abort(1);
    setbuf(stderr, NULL);
  }
#endif

  /*
   Initialize my_str_malloc(), my_str_realloc() and my_str_free()
  */
  my_str_malloc= &my_str_malloc_mysqld;
  my_str_free= &my_str_free_mysqld;
  my_str_realloc= &my_str_realloc_mysqld;


  /*
    init signals & alarm
    After this we can't quit by a simple unireg_abort
  */
  error_handler_hook= my_message_sql;
  start_signal_handler();       // Creates pidfile
  sql_print_warning_hook = sql_print_warning;

  if (mysql_rm_tmp_tables() || acl_init(opt_noacl) ||
      my_tz_init((THD *)0, default_tz_name, opt_bootstrap))
  {
    abort_loop=1;
    select_thread_in_use=0;

    (void) pthread_kill(signal_thread, MYSQL_KILL_SIGNAL);

    delete_pid_file(MYF(MY_WME));

    if (mysql_socket_getfd(unix_sock) != INVALID_SOCKET)
      unlink(mysqld_unix_port);
    exit(1);
  }

  if (!opt_noacl)
    (void) grant_init();

  if (!opt_bootstrap)
    servers_init(0);

  if (!opt_noacl)
  {
#ifdef HAVE_DLOPEN
    udf_init();
#endif
  }
  native_procedure_init();

  init_status_vars();
  /* If running with bootstrap, do not start replication. */
  if (opt_bootstrap)
    opt_skip_slave_start= 1;

  check_binlog_cache_size(NULL);
  check_binlog_stmt_cache_size(NULL);

  binlog_unsafe_map_init();

  /* If running with bootstrap, do not start replication. */
  if (!opt_bootstrap)
  {
    // Make @@slave_skip_errors show the nice human-readable value.
    set_slave_skip_errors(&opt_slave_skip_errors);

    /*
      init_slave() must be called after the thread keys are created.
    */
    if (server_id != 0)
      init_slave(); /* Ignoring errors while configuring replication. */
  }

#ifdef WITH_PERFSCHEMA_STORAGE_ENGINE
  initialize_performance_schema_acl(opt_bootstrap);
  /*
    Do not check the structure of the performance schema tables
    during bootstrap:
    - the tables are not supposed to exist yet, bootstrap will create them
    - a check would print spurious error messages
  */
  if (! opt_bootstrap)
    check_performance_schema();
#endif

  initialize_information_schema_acl();

  execute_ddl_log_recovery();

  if (Events::init(opt_noacl || opt_bootstrap))
    unireg_abort(1);

  /*
    If super_read_only = 1 and read_only = 0 set in my.cnf or startup params,
    auto update read_only = 1.
    Super users are read only meaning normal users must be read only.
  */
  if (read_only == FALSE && super_read_only == TRUE)
  {
    read_only = TRUE;
  }
  /*
    Originally assigned in get_options().
    But when super_read_only = 1 and read_only = 1 simultaneously,
    Event_db_repository::check_system_tables will return false
    as there are no write permission,
    causing 'Failed to open table mysql.event' error.
    So move here after Events::init().
  */
  opt_super_readonly= super_read_only;
  opt_readonly= read_only;

  if (opt_bootstrap)
  {
    select_thread_in_use= 0;                    // Allow 'kill' to work
    /* Signal threads waiting for server to be started */
    mysql_mutex_lock(&LOCK_server_started);
    mysqld_server_started= 1;
    mysql_cond_broadcast(&COND_server_started);
    mysql_mutex_unlock(&LOCK_server_started);

    bootstrap(mysql_stdin);
    unireg_abort(bootstrap_error ? 1 : 0);
  }
  if (opt_init_file && *opt_init_file)
  {
    if (read_init_file(opt_init_file))
      unireg_abort(1);
  }

  if (!opt_bootstrap)
  {
    // do the postponed init of raft, now that
    // binlog recovery has finished.
    raft_plugin_init();
  }

  create_shutdown_thread();
  start_handle_manager();

  // NO_LINT_DEBUG
  sql_print_information(ER_DEFAULT(ER_GIT_HASH), "MySQL", git_hash, git_date);
  // NO_LINT_DEBUG
  sql_print_information(ER_DEFAULT(ER_GIT_HASH), "RocksDB", rocksdb_git_hash,
                        rocksdb_git_date);

  sql_print_information(ER_DEFAULT(ER_STARTUP),my_progname,server_version,
                        ((mysql_socket_getfd(unix_sock) == INVALID_SOCKET) ? (char*) ""
                                                       : mysqld_unix_port),
                         mysqld_port,
                         MYSQL_COMPILATION_COMMENT);
#if defined(_WIN32) && !defined(EMBEDDED_LIBRARY)
  Service.SetRunning();
#endif


  /* Signal threads waiting for server to be started */
  mysql_mutex_lock(&LOCK_server_started);
  mysqld_server_started= 1;
  mysql_cond_broadcast(&COND_server_started);
  mysql_mutex_unlock(&LOCK_server_started);

#ifdef WITH_NDBCLUSTER_STORAGE_ENGINE
  /* engine specific hook, to be made generic */
  if (ndb_wait_setup_func && ndb_wait_setup_func(opt_ndb_wait_setup))
  {
    sql_print_warning("NDB : Tables not available after %lu seconds."
                      "  Consider increasing --ndb-wait-setup value",
                      opt_ndb_wait_setup);
  }
#endif

  MYSQL_SET_STAGE(0 ,__FILE__, __LINE__);

#if defined(_WIN32) || defined(HAVE_SMEM)
  handle_connections_methods();
#else
  handle_connections_sockets_all();
#endif /* _WIN32 || HAVE_SMEM */

  /* (void) pthread_attr_destroy(&connection_attrib); */

  DBUG_PRINT("quit",("Exiting main thread"));

#ifndef __WIN__
#ifdef EXTRA_DEBUG2
  sql_print_error("Before Lock_thread_count");
#endif
  mysql_mutex_lock(&LOCK_thread_count);
  DBUG_PRINT("quit", ("Got thread_count mutex"));
  select_thread_in_use=0;     // For close_connections
  mysql_mutex_unlock(&LOCK_thread_count);
  mysql_cond_broadcast(&COND_thread_count);
#ifdef EXTRA_DEBUG2
  sql_print_error("After lock_thread_count");
#endif
#endif /* __WIN__ */

#ifdef HAVE_PSI_THREAD_INTERFACE
  /*
    Disable the main thread instrumentation,
    to avoid recording events during the shutdown.
  */
  PSI_THREAD_CALL(delete_current_thread)();
#endif

  /* Wait until cleanup is done */
  mysql_mutex_lock(&LOCK_thread_count);
  while (!ready_to_exit)
    mysql_cond_wait(&COND_thread_count, &LOCK_thread_count);
  mysql_mutex_unlock(&LOCK_thread_count);

#if defined(__WIN__) && !defined(EMBEDDED_LIBRARY)
  if (Service.IsNT() && start_mode)
    Service.Stop();
  else
  {
    Service.SetShutdownEvent(0);
    if (hEventShutdown)
      CloseHandle(hEventShutdown);
  }
#endif
  clean_up(1);
  mysqld_exit(0);
}

#endif /* !EMBEDDED_LIBRARY */


/****************************************************************************
  Main and thread entry function for Win32
  (all this is needed only to run mysqld as a service on WinNT)
****************************************************************************/

#if defined(__WIN__) && !defined(EMBEDDED_LIBRARY)
int mysql_service(void *p)
{
  if (my_thread_init())
    return 1;

  if (use_opt_args)
    win_main(opt_argc, opt_argv);
  else
    win_main(Service.my_argc, Service.my_argv);

  my_thread_end();
  return 0;
}


/* Quote string if it contains space, else copy */

static char *add_quoted_string(char *to, const char *from, char *to_end)
{
  uint length= (uint) (to_end-to);

  if (!strchr(from, ' '))
    return strmake(to, from, length-1);
  return strxnmov(to, length-1, "\"", from, "\"", NullS);
}


/**
  Handle basic handling of services, like installation and removal.

  @param argv             Pointer to argument list
  @param servicename    Internal name of service
  @param displayname    Display name of service (in taskbar ?)
  @param file_path    Path to this program
  @param startup_option Startup option to mysqld

  @retval
    0   option handled
  @retval
    1   Could not handle option
*/

static bool
default_service_handling(char **argv,
       const char *servicename,
       const char *displayname,
       const char *file_path,
       const char *extra_opt,
       const char *account_name)
{
  char path_and_service[FN_REFLEN+FN_REFLEN+32], *pos, *end;
  const char *opt_delim;
  end= path_and_service + sizeof(path_and_service)-3;

  /* We have to quote filename if it contains spaces */
  pos= add_quoted_string(path_and_service, file_path, end);
  if (extra_opt && *extra_opt)
  {
    /*
     Add option after file_path. There will be zero or one extra option.  It's
     assumed to be --defaults-file=file but isn't checked.  The variable (not
     the option name) should be quoted if it contains a string.
    */
    *pos++= ' ';
    if (opt_delim= strchr(extra_opt, '='))
    {
      size_t length= ++opt_delim - extra_opt;
      pos= strnmov(pos, extra_opt, length);
    }
    else
      opt_delim= extra_opt;

    pos= add_quoted_string(pos, opt_delim, end);
  }
  /* We must have servicename last */
  *pos++= ' ';
  (void) add_quoted_string(pos, servicename, end);

  if (Service.got_service_option(argv, "install"))
  {
    Service.Install(1, servicename, displayname, path_and_service,
                    account_name);
    return 0;
  }
  if (Service.got_service_option(argv, "install-manual"))
  {
    Service.Install(0, servicename, displayname, path_and_service,
                    account_name);
    return 0;
  }
  if (Service.got_service_option(argv, "remove"))
  {
    Service.Remove(servicename);
    return 0;
  }
  return 1;
}


int mysqld_main(int argc, char **argv)
{
  /*
    When several instances are running on the same machine, we
    need to have an  unique  named  hEventShudown  through the
    application PID e.g.: MySQLShutdown1890; MySQLShutdown2342
  */
  int10_to_str((int) GetCurrentProcessId(),strmov(shutdown_event_name,
                                                  "MySQLShutdown"), 10);

  /* Must be initialized early for comparison of service name */
  system_charset_info= &my_charset_utf8_general_ci;

  if (my_init())
  {
    fprintf(stderr, "my_init() failed.");
    return 1;
  }

  if (Service.GetOS())  /* true NT family */
  {
    char file_path[FN_REFLEN];
    my_path(file_path, argv[0], "");          /* Find name in path */
    fn_format(file_path,argv[0],file_path,"",
        MY_REPLACE_DIR | MY_UNPACK_FILENAME | MY_RESOLVE_SYMLINKS);

    if (argc == 2)
    {
      if (!default_service_handling(argv, MYSQL_SERVICENAME, MYSQL_SERVICENAME,
           file_path, "", NULL))
  return 0;
      if (Service.IsService(argv[1]))        /* Start an optional service */
      {
  /*
    Only add the service name to the groups read from the config file
    if it's not "MySQL". (The default service name should be 'mysqld'
    but we started a bad tradition by calling it MySQL from the start
    and we are now stuck with it.
  */
  if (my_strcasecmp(system_charset_info, argv[1],"mysql"))
    load_default_groups[load_default_groups_sz-2]= argv[1];
        start_mode= 1;
        Service.Init(argv[1], mysql_service);
        return 0;
      }
    }
    else if (argc == 3) /* install or remove any optional service */
    {
      if (!default_service_handling(argv, argv[2], argv[2], file_path, "",
                                    NULL))
  return 0;
      if (Service.IsService(argv[2]))
      {
  /*
    mysqld was started as
    mysqld --defaults-file=my_path\my.ini service-name
  */
  use_opt_args=1;
  opt_argc= 2;        // Skip service-name
  opt_argv=argv;
  start_mode= 1;
  if (my_strcasecmp(system_charset_info, argv[2],"mysql"))
    load_default_groups[load_default_groups_sz-2]= argv[2];
  Service.Init(argv[2], mysql_service);
  return 0;
      }
    }
    else if (argc == 4 || argc == 5)
    {
      /*
        This may seem strange, because we handle --local-service while
        preserving 4.1's behavior of allowing any one other argument that is
        passed to the service on startup. (The assumption is that this is
        --defaults-file=file, but that was not enforced in 4.1, so we don't
        enforce it here.)
      */
      const char *extra_opt= NullS;
      const char *account_name = NullS;
      int index;
      for (index = 3; index < argc; index++)
      {
        if (!strcmp(argv[index], "--local-service"))
          account_name= "NT AUTHORITY\\LocalService";
        else
          extra_opt= argv[index];
      }

      if (argc == 4 || account_name)
        if (!default_service_handling(argv, argv[2], argv[2], file_path,
                                      extra_opt, account_name))
          return 0;
    }
    else if (argc == 1 && Service.IsService(MYSQL_SERVICENAME))
    {
      /* start the default service */
      start_mode= 1;
      Service.Init(MYSQL_SERVICENAME, mysql_service);
      return 0;
    }
  }
  /* Start as standalone server */
  Service.my_argc=argc;
  Service.my_argv=argv;
  mysql_service(NULL);
  return 0;
}
#endif


/**
  Execute all commands from a file. Used by the mysql_install_db script to
  create MySQL privilege tables without having to start a full MySQL server.
*/

static void bootstrap(MYSQL_FILE *file)
{
  DBUG_ENTER("bootstrap");

  THD *thd= new THD;
  NET* net = thd->get_net();
  thd->bootstrap=1;
  my_net_init(net,(st_vio*) 0);
  thd->max_client_packet_length= net->max_packet;
  thd->security_ctx->master_access= ~(ulong)0;

  thd->variables.pseudo_thread_id= thd->set_new_thread_id();


  in_bootstrap= TRUE;

  bootstrap_file=file;
#ifndef EMBEDDED_LIBRARY      // TODO:  Enable this
  int error;
  if ((error= mysql_thread_create(key_thread_bootstrap,
                                  &thd->real_id, &connection_attrib,
                                  handle_bootstrap,
                                  (void*) thd)))
  {
    sql_print_warning("Can't create thread to handle bootstrap (errno= %d)",
                      error);
    bootstrap_error=-1;
    DBUG_VOID_RETURN;
  }
  /* Wait for thread to die */
  mysql_mutex_lock(&LOCK_thread_count);
  while (in_bootstrap)
  {
    mysql_cond_wait(&COND_thread_count, &LOCK_thread_count);
    DBUG_PRINT("quit", ("One thread died (count=%u)", get_thread_count()));
  }
  mysql_mutex_unlock(&LOCK_thread_count);
#else
  thd->mysql= 0;
  do_handle_bootstrap(thd);
#endif

  DBUG_VOID_RETURN;
}


static bool read_init_file(char *file_name)
{
  MYSQL_FILE *file;
  DBUG_ENTER("read_init_file");
  DBUG_PRINT("enter",("name: %s",file_name));

  sql_print_information("Execution of init_file \'%s\' started.", file_name);

  if (!(file= mysql_file_fopen(key_file_init, file_name,
                               O_RDONLY, MYF(MY_WME))))
    DBUG_RETURN(TRUE);
  bootstrap(file);
  mysql_file_fclose(file, MYF(MY_WME));

  sql_print_information("Execution of init_file \'%s\' ended.", file_name);

  DBUG_RETURN(FALSE);
}


#ifndef EMBEDDED_LIBRARY

/*
   Simple scheduler that use the main thread to handle the request

   NOTES
     This is only used for debugging, when starting mysqld with
     --thread-handling=no-threads or --one-thread

     When we enter this function, LOCK_thread_count is held!
*/

void handle_connection_in_main_thread(THD *thd)
{
  mutex_lock_shard(SHARDED(&LOCK_thread_count), thd);
  max_blocked_pthreads= 0;      // Safety
  add_global_thread(thd);
  mutex_unlock_shard(SHARDED(&LOCK_thread_count), thd);
  thd->start_utime= my_micro_time();
  do_handle_one_connection(thd);
}


/*
  Scheduler that uses one thread per connection
*/

void create_thread_to_handle_connection(THD *thd)
{
  mysql_mutex_lock(&LOCK_thread_count);
  if (blocked_pthread_count >  wake_pthread)
  {
    /* Wake up blocked pthread */
    DBUG_PRINT("info", ("waiting_thd_list->push %p", thd));
    waiting_thd_list->push_back(thd);
    wake_pthread++;
    mysql_cond_signal(&COND_thread_cache);

#if !defined(DBUG_OFF)
    con_threads_wait++;
    if (con_threads_wait % 10000 == 0) {
      DBUG_PRINT("info", ("new thread being waited because "
        "blocked threads available: %lu wake: %u total wait: %" PRIu64
        ", total new: %" PRIu64,
        blocked_pthread_count, wake_pthread,
        con_threads_wait, con_threads_new));
    }
#endif
    mysql_mutex_unlock(&LOCK_thread_count);
  }
  else
  {
#if !defined(DBUG_OFF)
    con_threads_new++;
    if (con_threads_new % 10000 == 0) {
      DBUG_PRINT("info", ("new thread being created because "
        "cond not met: blocked: %lu wake: %u total wait: %" PRIu64
        ", total new: %" PRIu64,
        blocked_pthread_count, wake_pthread,
        con_threads_wait, con_threads_new));
    }
#endif
    mysql_mutex_unlock(&LOCK_thread_count);
    char error_message_buff[MYSQL_ERRMSG_SIZE];
    /* Create new thread to handle connection */
    int error;
    thread_created++;
    DBUG_PRINT("info",(("creating thread %u"), thd->thread_id()));
    thd->prior_thr_create_utime= thd->start_utime= my_micro_time();

    // hold the sharded lock before creating the thread and release
    // it after adding the thread to global thread list, otherwise
    // remove_global_thread can get called before the thread
    // has been added to global list.
    mutex_lock_shard(SHARDED(&LOCK_thread_count), thd);
    if ((error= mysql_thread_create(key_thread_one_connection,
                                    &thd->real_id, &connection_attrib,
                                    handle_one_connection,
                                    (void*) thd)))
    {
      // release the lock since we will return
      mutex_unlock_shard(SHARDED(&LOCK_thread_count), thd);

      /* purecov: begin inspected */
      DBUG_PRINT("error",
                 ("Can't create thread to handle request (error %d)",
                  error));
      if (!err_log_throttle.log(thd))
        sql_print_error("Can't create thread to handle request (errno= %d)",
                        error);
      thd->killed= THD::KILL_CONNECTION;      // Safety
      dec_connection_count();

      statistic_increment(aborted_connects,&LOCK_status);
      statistic_increment(connection_errors_internal, &LOCK_status);
      /* Can't use my_error() since store_globals has not been called. */
      my_snprintf(error_message_buff, sizeof(error_message_buff),
                  ER_THD(thd, ER_CANT_CREATE_THREAD), error);
      net_send_error(thd, NULL, ER_CANT_CREATE_THREAD, error_message_buff,
                     NULL);
      close_connection(thd);
      delete thd;
      return;
      /* purecov: end */
    }
    add_global_thread(thd);
    mutex_unlock_shard(SHARDED(&LOCK_thread_count), thd);
  }
  DBUG_PRINT("info",("Thread created"));
}



/**
  Create new thread to handle incoming connection.

    This function will create new thread to handle the incoming
    connection.  If there are idle cached threads one will be used.

    In single-threaded mode (\#define ONE_THREAD) connection will be
    handled inside this function.

  @param[in,out] thd    Thread handle of future thread.
*/

static void create_new_thread(THD *thd)
{
  DBUG_ENTER("create_new_thread");

  /*
    Don't allow too many connections. We roughly check here that we allow
    only (max_connections + 1) connections.
  */

  mysql_mutex_lock(&LOCK_thread_count);

  if (((connection_count >= max_connections + 1) &&
       (!thd->is_admin_connection())) || abort_loop)
  {
    mysql_mutex_unlock(&LOCK_thread_count);

    DBUG_PRINT("error",("Too many connections"));

#if !defined(DBUG_OFF)
    dropped_conns++;
    if (dropped_conns % 5000 == 0)
      DBUG_PRINT("info", ("too many connections %" PRIu64, dropped_conns));
#endif

    /*
      The server just accepted the socket connection from the network,
      and we already have too many connections.
      Note that the server knows nothing of the client yet,
      and in particular thd->client_capabilities has not been negotiated.
      ER_CON_COUNT_ERROR is normally associated with SQLSTATE '08004',
      but sending a SQLSTATE in the network assumes CLIENT_PROTOCOL_41.
      See net_send_error_packet().
      The error packet returned here will only contain the error code,
      with no sqlstate.
      A client expecting a SQLSTATE will not find any, and assume 'HY000'.
    */
    close_connection(thd, ER_CON_COUNT_ERROR);
    delete thd;
    statistic_increment(connection_errors_max_connection, &LOCK_status);
    DBUG_VOID_RETURN;
  }

  ++connection_count;

#if !defined(DBUG_OFF)
  con_threads_count++;

  if (con_threads_count % 100000 == 0) {
     DBUG_PRINT("info", ("create thds: %" PRIu64 ", connection_count: "
      "%u : max:%lu blocked threads available: %lu wake: %u"
      " total wait: %" PRIu64 ", total new: %" PRIu64,
       con_threads_count, connection_count,
       max_connections, blocked_pthread_count,
       wake_pthread, con_threads_wait, con_threads_new));
  }
#endif

  if (connection_count > max_used_connections)
    max_used_connections= connection_count;

  /* Start a new thread to handle connection. */
  mysql_mutex_unlock(&LOCK_thread_count);

  /*
    The initialization of thread_id is done in create_embedded_thd() for
    the embedded library.
    TODO: refactor this to avoid code duplication there
  */
  thd->variables.pseudo_thread_id= thd->set_new_thread_id();

  MYSQL_CALLBACK(thread_scheduler, add_connection, (thd));

  DBUG_VOID_RETURN;
}
#endif /* EMBEDDED_LIBRARY */


#ifdef SIGNALS_DONT_BREAK_READ
inline void kill_broken_server()
{
  /* hack to get around signals ignored in syscalls for problem OS's */
  if (mysql_get_fd(unix_sock) == INVALID_SOCKET ||
      (!opt_disable_networking && mysql_socket_getfd(ip_sock) == INVALID_SOCKET))
  {
    select_thread_in_use = 0;
    /* The following call will never return */
    kill_server((void*) MYSQL_KILL_SIGNAL);
  }
}
#define MAYBE_BROKEN_SYSCALL kill_broken_server();
#else
#define MAYBE_BROKEN_SYSCALL
#endif

  /* Handle new connections and spawn new process to handle them */

#ifndef EMBEDDED_LIBRARY

#ifdef HAVE_SYS_UN_H
static std::atomic<bool> unix_domain_socket_init(false);
#endif

void handle_connections_sockets(bool admin, const MYSQL_SOCKET *socket_ptr)
{
  MYSQL_SOCKET sock= mysql_socket_invalid();
  MYSQL_SOCKET new_sock= mysql_socket_invalid();
  uint error_count=0;
  THD *thd;
  struct sockaddr_storage cAddr;
  int ip_flags=0,socket_flags=0,admin_ip_flags=0;
  int flags=0,retval;
  st_vio *vio_tmp;
#ifdef HAVE_POLL
  int socket_count= 0;
  struct pollfd fds[2]; // for ip_sock and unix_sock or admin_ip_sock
  MYSQL_SOCKET pfs_fds[2]; // for performance schema
#define setup_fds(X) \
    fds[socket_count].fd= mysql_socket_getfd(X); \
    fds[socket_count].events= POLLIN;   \
    pfs_fds[socket_count]= X; \
    socket_count++
#else
  fd_set readFDs,clientFDs;
  uint max_used_connection = 1;
  if (admin) {
    max_used_connection += mysql_socket_getfd(admin_ip_sock));
  }
  else {
    max_used_connection +=
      max<uint>(mysql_socket_getfd(unix_sock), mysql_socket_getfd(ip_sock));
  }
  FD_ZERO(&clientFDs);
#define setup_fds(X) \
  FD_SET(mysql_socket_getfd(X), &clientFDs)
#endif

  DBUG_ENTER("handle_connections_sockets");

  (void) ip_flags;
  (void) socket_flags;
  (void) admin_ip_flags;

  MYSQL_SOCKET cur_ip_sock = (socket_ptr == NULL) ? ip_sock : *socket_ptr;
  if (admin)
  {
    DBUG_ASSERT(socket_ptr == NULL);
    if (mysql_socket_getfd(admin_ip_sock) != INVALID_SOCKET)
    {
      mysql_socket_set_thread_owner(admin_ip_sock);
      setup_fds(admin_ip_sock);
#ifdef HAVE_FCNTL
      admin_ip_flags = fcntl(mysql_socket_getfd(admin_ip_sock), F_GETFL, 0);
#endif
    }
  }
  else
  {
    if (mysql_socket_getfd(cur_ip_sock) != INVALID_SOCKET)
    {
      mysql_socket_set_thread_owner(cur_ip_sock);
      setup_fds(cur_ip_sock);
#ifdef HAVE_FCNTL
      ip_flags = fcntl(mysql_socket_getfd(cur_ip_sock), F_GETFL, 0);
#endif
    }

#ifdef HAVE_SYS_UN_H
    bool expected = false;
    // when SO_REUSEPORT is ON, only 1 of the several threads should
    // listen to UNIX_DOMAIN
    if (unix_domain_socket_init.compare_exchange_strong(expected, true)) {
      mysql_socket_set_thread_owner(unix_sock);
      setup_fds(unix_sock);
#ifdef HAVE_FCNTL
      socket_flags=fcntl(mysql_socket_getfd(unix_sock), F_GETFL, 0);
#endif
    }
#endif
  }

  DBUG_PRINT("general",("Waiting for connections."));
  MAYBE_BROKEN_SYSCALL;
  while (!abort_loop)
  {
#ifdef HAVE_POLL
    retval= poll(fds, socket_count, 5000);
#else
    readFDs=clientFDs;

    retval= select((int) max_used_connection,&readFDs,0,0,0);
#endif

    if (retval < 0)
    {
      if (socket_errno != SOCKET_EINTR)
      {
        /*
          select(2)/poll(2) failed on the listening port.
          There is not much details to report about the client,
          increment the server global status variable.
        */
        statistic_increment(connection_errors_select, &LOCK_status);
        if (!select_errors++ && !abort_loop)  /* purecov: inspected */
          sql_print_error("mysqld: Got error %d from select",socket_errno); /* purecov: inspected */
      }
      MAYBE_BROKEN_SYSCALL
      continue;
    }

    if (retval == 0) {
      // timeout
      continue;
    }

    if (abort_loop)
    {
      MAYBE_BROKEN_SYSCALL;
      break;
    }

    /* Is this a new connection request ? */
#ifdef HAVE_POLL
    for (int i= 0; i < socket_count; ++i)
    {
      if (fds[i].revents & POLLIN)
      {
        sock= pfs_fds[i];
#ifdef HAVE_FCNTL
        flags= fcntl(mysql_socket_getfd(sock), F_GETFL, 0);
#else
        flags= 0;
#endif // HAVE_FCNTL
        break;
      }
    }
#else  // HAVE_POLL
#ifdef HAVE_SYS_UN_H
    if (!admin && FD_ISSET(mysql_socket_getfd(unix_sock), &readFDs))
    {
      sock = unix_sock;
      flags= socket_flags;
    }
    else
#endif // HAVE_SYS_UN_H
    if (!admin && FD_ISSET(mysql_socket_getfd(cur_ip_sock), &readFDs))
    {
      sock = cur_ip_sock;
      flags= ip_flags;
    }
    else
    if (admin && FD_ISSET(mysql_socket_getfd(admin_ip_sock), &readFDs))
    {
      sock= admin_ip_sock;
      flags = admin_ip_flags;
    }
#endif // HAVE_POLL

#if !defined(NO_FCNTL_NONBLOCK)
    if (!(test_flags & TEST_BLOCKING))
    {
#if defined(O_NONBLOCK)
      fcntl(mysql_socket_getfd(sock), F_SETFL, flags | O_NONBLOCK);
#elif defined(O_NDELAY)
      fcntl(mysql_socket_getfd(sock), F_SETFL, flags | O_NDELAY);
#endif
    }
#endif /* NO_FCNTL_NONBLOCK */
    for (uint retry=0; retry < MAX_ACCEPT_RETRY; retry++)
    {
      size_socket length= sizeof(struct sockaddr_storage);
      new_sock= mysql_socket_accept(key_socket_client_connection, sock,
                                    (struct sockaddr *)(&cAddr), &length);
      if (mysql_socket_getfd(new_sock) != INVALID_SOCKET ||
          (socket_errno != SOCKET_EINTR && socket_errno != SOCKET_EAGAIN))
        break;
      MAYBE_BROKEN_SYSCALL;
#if !defined(NO_FCNTL_NONBLOCK)
      if (!(test_flags & TEST_BLOCKING))
      {
        if (retry == MAX_ACCEPT_RETRY - 1)
          fcntl(mysql_socket_getfd(sock), F_SETFL, flags);    // Try without O_NONBLOCK
      }
#endif
    }
#if !defined(NO_FCNTL_NONBLOCK)
    if (!(test_flags & TEST_BLOCKING))
      fcntl(mysql_socket_getfd(sock), F_SETFL, flags);
#endif
    if (mysql_socket_getfd(new_sock) == INVALID_SOCKET)
    {
      /*
        accept(2) failed on the listening port, after many retries.
        There is not much details to report about the client,
        increment the server global status variable.
      */
      statistic_increment(connection_errors_accept, &LOCK_status);
      if ((error_count++ & 255) == 0)   // This can happen often
        sql_perror("Error in accept");
      MAYBE_BROKEN_SYSCALL;
      if (socket_errno == SOCKET_ENFILE || socket_errno == SOCKET_EMFILE)
        sleep(1);       // Give other threads some time
      continue;
    }

#ifdef HAVE_LIBWRAP
    {
      if (mysql_socket_getfd(sock) == mysql_socket_getfd(ip_sock) ||
          mysql_socket_getfd(sock) == mysql_socket_getfd(admin_ip_sock))
      {
        struct request_info req;
        signal(SIGCHLD, SIG_DFL);
        request_init(&req, RQ_DAEMON, libwrapName, RQ_FILE, mysql_socket_getfd(new_sock), NULL);
        my_fromhost(&req);

        if (!my_hosts_access(&req))
        {
          /*
            This may be stupid but refuse() includes an exit(0)
            which we surely don't want...
            clean_exit() - same stupid thing ...
          */
          syslog(deny_severity, "refused connect from %s",
          my_eval_client(&req));

          /*
            C++ sucks (the gibberish in front just translates the supplied
            sink function pointer in the req structure from a void (*sink)();
            to a void(*sink)(int) if you omit the cast, the C++ compiler
            will cry...
          */
          if (req.sink)
            ((void (*)(int))req.sink)(req.fd);

          mysql_socket_shutdown(new_sock, SHUT_RDWR);
          mysql_socket_close(new_sock);
          /*
            The connection was refused by TCP wrappers.
            There are no details (by client IP) available to update the host_cache.
          */
          statistic_increment(connection_errors_tcpwrap, &LOCK_status);
          continue;
        }
      }
    }
#endif /* HAVE_LIBWRAP */

    if (!admin && separate_conn_handling_thread)  {
      SOCKET_PACKET pkt;
      pkt.new_sock = new_sock;
      pkt.is_admin = admin;
      pkt.is_unix_sock = (mysql_socket_getfd(sock)
        == mysql_socket_getfd(unix_sock));

      // round robin
      uint64_t qidx = send_q_index++;
      sockets_list[qidx % num_conn_handling_threads]->enqueue(pkt);
    } else {

    /*
    ** Don't allow too many connections
    */

    if (!(thd= new THD))
    {
      (void) mysql_socket_shutdown(new_sock, SHUT_RDWR);
      (void) mysql_socket_close(new_sock);
      statistic_increment(connection_errors_internal, &LOCK_status);
        continue;
      }

      bool is_unix_sock= (mysql_socket_getfd(sock)
        == mysql_socket_getfd(unix_sock));
      enum_vio_type vio_type= (is_unix_sock ? VIO_TYPE_SOCKET : VIO_TYPE_TCPIP);
      uint vio_flags= (is_unix_sock ? VIO_LOCALHOST : 0);

      vio_tmp= mysql_socket_vio_new(new_sock, vio_type, vio_flags);

      NET* net = thd->get_net();
      if (!vio_tmp || my_net_init(net, vio_tmp))
      {
        /*
          Only delete the temporary vio if we didn't already attach it to the
          NET object. The destructor in THD will delete any initialized net
          structure.
        */
        if (vio_tmp && net->vio != vio_tmp)
          vio_delete(vio_tmp);
        else
        {
          (void) mysql_socket_shutdown(new_sock, SHUT_RDWR);
          (void) mysql_socket_close(new_sock);
        }
        delete thd;
        statistic_increment(connection_errors_internal, &LOCK_status);
        continue;
      }
      init_net_server_extension(thd);
      if (mysql_socket_getfd(sock) == mysql_socket_getfd(unix_sock))
        thd->security_ctx->set_host((char*) my_localhost);

      if (admin)
        thd->set_admin_connection();

      create_new_thread(thd);
    }
  }
  DBUG_VOID_RETURN;
}

#if defined(HAVE_SOREUSEPORT)
pthread_handler_t soreusethread(void *arg)
{
  my_thread_init();
  uint64_t sid = reinterpret_cast<uint64_t>(arg);
  DBUG_ASSERT(sid < ip_sharded_sockets.size());
  const MYSQL_SOCKET *socket_ptr = &ip_sharded_sockets[sid];

#if defined(ALTER_THREAD_NAMES)
  alter_thread_name("soreuse_", (int)sid);
#endif

  handle_connections_sockets(false, socket_ptr);

  mysql_mutex_lock(&LOCK_thread_count);
  handler_count--;
  mysql_cond_broadcast(&COND_thread_count);
  mysql_mutex_unlock(&LOCK_thread_count);
  my_thread_end();
  return 0;
}
#endif

pthread_handler_t dedicated_conn_handling_thread(void *arg)
{
  my_thread_init();
  uint64_t sid = reinterpret_cast<uint64_t>(arg);
#if defined(ALTER_THREAD_NAMES)
  alter_thread_name("ded_conn_h_", (int)sid);
#endif

  int num_backoffs = 0;
  while (!abort_loop) {

    SOCKET_PACKET pkt;
    bool avail = sockets_list[sid]->dequeue(pkt);
    if (!avail) {
      do_backoff(&num_backoffs);
      continue;
    }

    num_backoffs = 0;
    bool is_admin = pkt.is_admin;
    bool is_unix_sock = pkt.is_unix_sock;
    MYSQL_SOCKET new_sock = pkt.new_sock;

    /*
    ** Don't allow too many connections
    */
    bool en_plugins = true;
    THD *thd = new THD(en_plugins);
    enum_vio_type vio_type= (is_unix_sock ? VIO_TYPE_SOCKET : VIO_TYPE_TCPIP);
    uint vio_flags= (is_unix_sock ? VIO_LOCALHOST : 0);

    st_vio *vio_tmp= mysql_socket_vio_new(new_sock, vio_type, vio_flags);

    NET* net = thd->get_net();
    if (!vio_tmp || my_net_init(net, vio_tmp))
    {
      /*
        Only delete the temporary vio if we didn't already attach it to the
        NET object. The destructor in THD will delete any initialized net
        structure.
      */
      if (vio_tmp && net->vio != vio_tmp)
        vio_delete(vio_tmp);
      else
      {
        (void) mysql_socket_shutdown(new_sock, SHUT_RDWR);
        (void) mysql_socket_close(new_sock);
      }
      delete thd;
      statistic_increment(connection_errors_internal, &LOCK_status);
      continue;
    }
    init_net_server_extension(thd);
    if (is_unix_sock)
      thd->security_ctx->set_host((char*) my_localhost);

    if (is_admin)
      thd->set_admin_connection();

    create_new_thread(thd);
  }

  mysql_mutex_lock(&LOCK_thread_count);
  handler_count--;

  mysql_cond_broadcast(&COND_thread_count);
  mysql_mutex_unlock(&LOCK_thread_count);
  my_thread_end();
  return 0;
}

#ifdef _WIN32
pthread_handler_t handle_connections_sockets_thread(void *arg)
{
  my_thread_init();
  handle_connections_sockets();
  decrement_handler_count();
  return 0;
}

pthread_handler_t handle_connections_namedpipes(void *arg)
{
  HANDLE hConnectedPipe;
  OVERLAPPED connectOverlapped= {0};
  THD *thd;
  TCHAR last_error_msg[256];
  my_thread_init();
  DBUG_ENTER("handle_connections_namedpipes");
  connectOverlapped.hEvent= CreateEvent(NULL, TRUE, FALSE, NULL);
  if (!connectOverlapped.hEvent)
  {
    sql_print_error("Can't create event, last error=%u", GetLastError());
    unireg_abort(1);
  }
  DBUG_PRINT("general",("Waiting for named pipe connections."));
  while (!abort_loop)
  {
    /* wait for named pipe connection */
    BOOL fConnected= ConnectNamedPipe(hPipe, &connectOverlapped);
    if (!fConnected && (GetLastError() == ERROR_IO_PENDING))
    {
        /*
          ERROR_IO_PENDING says async IO has started but not yet finished.
          GetOverlappedResult will wait for completion.
        */
        DWORD bytes;
        fConnected= GetOverlappedResult(hPipe, &connectOverlapped,&bytes, TRUE);
    }
    if (abort_loop)
      break;
    if (!fConnected)
      fConnected = GetLastError() == ERROR_PIPE_CONNECTED;
    if (!fConnected)
    {
      CloseHandle(hPipe);
      mysql_rwlock_rdlock(&LOCK_named_pipe_full_access_group);
      hPipe= CreateNamedPipe(pipe_name,
                             PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                               WRITE_DAC,
                             PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                             PIPE_UNLIMITED_INSTANCES,
                             (int) global_system_variables.net_buffer_length,
                             (int) global_system_variables.net_buffer_length,
                             NMPWAIT_USE_DEFAULT_WAIT,
                             psaPipeSecurity);
      mysql_rwlock_unlock(&LOCK_named_pipe_full_access_group);
      if (hPipe == INVALID_HANDLE_VALUE)
      {
        DWORD last_error_num= GetLastError();
        FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS |
                        FORMAT_MESSAGE_MAX_WIDTH_MASK,
                      NULL, last_error_num,
                      MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), last_error_msg,
                      sizeof(last_error_msg) / sizeof(TCHAR), NULL);
        sql_print_error("Can't create new named pipe: %s", last_error_msg);
        break;					// Abort
      }
    }
    hConnectedPipe = hPipe;
    /* create new pipe for new connection */
    mysql_rwlock_rdlock(&LOCK_named_pipe_full_access_group);
    hPipe= CreateNamedPipe(pipe_name,
                            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                              WRITE_DAC,
			                      PIPE_TYPE_BYTE |PIPE_READMODE_BYTE | PIPE_WAIT,
			                      PIPE_UNLIMITED_INSTANCES,
			                      (int) global_system_variables.net_buffer_length,
			                      (int) global_system_variables.net_buffer_length,
			                      NMPWAIT_USE_DEFAULT_WAIT,
			                      psaPipeSecurity);
    mysql_rwlock_unlock(&LOCK_named_pipe_full_access_group);
    if (hPipe == INVALID_HANDLE_VALUE)
    {
      DWORD last_error_num = GetLastError();
      FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS |
                      FORMAT_MESSAGE_MAX_WIDTH_MASK,
                    NULL, last_error_num,
                    MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), last_error_msg,
                    sizeof(last_error_msg) / sizeof(TCHAR), NULL);
      sql_print_error("Can't create new named pipe: %s", last_error_msg);
      hPipe=hConnectedPipe;
      continue;					// We have to try again
    }

    if (!(thd = new THD))
    {
      DisconnectNamedPipe(hConnectedPipe);
      CloseHandle(hConnectedPipe);
      continue;
    }
    NET* net = thd->get_net();
    if (!(net->vio= vio_new_win32pipe(hConnectedPipe)) ||
  my_net_init(net, net->vio))
    {
      close_connection(thd, ER_OUT_OF_RESOURCES);
      delete thd;
      continue;
    }
    /* Host is unknown */
    thd->security_ctx->set_host(my_strdup(my_localhost, MYF(0)));
    create_new_thread(thd);
  }
  CloseHandle(connectOverlapped.hEvent);
  DBUG_LEAVE;
  decrement_handler_count();
  return 0;
}
#endif /* _WIN32 */


#ifdef HAVE_SMEM

/**
  Thread of shared memory's service.

  @param arg                              Arguments of thread
*/
pthread_handler_t handle_connections_shared_memory(void *arg)
{
  /* file-mapping object, use for create shared memory */
  HANDLE handle_connect_file_map= 0;
  char  *handle_connect_map= 0;                 // pointer on shared memory
  HANDLE event_connect_answer= 0;
  ulong smem_buffer_length= shared_memory_buffer_length + 4;
  ulong connect_number= 1;
  char *tmp= NULL;
  char *suffix_pos;
  char connect_number_char[22], *p;
  const char *errmsg= 0;
  SECURITY_ATTRIBUTES *sa_event= 0, *sa_mapping= 0;
  my_thread_init();
  DBUG_ENTER("handle_connections_shared_memorys");
  DBUG_PRINT("general",("Waiting for allocated shared memory."));

  /*
     get enough space base-name + '_' + longest suffix we might ever send
   */
  if (!(tmp= (char *)my_malloc(strlen(shared_memory_base_name) + 32L, MYF(MY_FAE))))
    goto error;

  if (my_security_attr_create(&sa_event, &errmsg,
                              GENERIC_ALL, SYNCHRONIZE | EVENT_MODIFY_STATE))
    goto error;

  if (my_security_attr_create(&sa_mapping, &errmsg,
                             GENERIC_ALL, FILE_MAP_READ | FILE_MAP_WRITE))
    goto error;

  /*
    The name of event and file-mapping events create agree next rule:
      shared_memory_base_name+unique_part
    Where:
      shared_memory_base_name is unique value for each server
      unique_part is unique value for each object (events and file-mapping)
  */
  suffix_pos= strxmov(tmp,shared_memory_base_name,"_",NullS);
  strmov(suffix_pos, "CONNECT_REQUEST");
  if ((smem_event_connect_request= CreateEvent(sa_event,
                                               FALSE, FALSE, tmp)) == 0)
  {
    errmsg= "Could not create request event";
    goto error;
  }
  strmov(suffix_pos, "CONNECT_ANSWER");
  if ((event_connect_answer= CreateEvent(sa_event, FALSE, FALSE, tmp)) == 0)
  {
    errmsg="Could not create answer event";
    goto error;
  }
  strmov(suffix_pos, "CONNECT_DATA");
  if ((handle_connect_file_map=
       CreateFileMapping(INVALID_HANDLE_VALUE, sa_mapping,
                         PAGE_READWRITE, 0, sizeof(connect_number), tmp)) == 0)
  {
    errmsg= "Could not create file mapping";
    goto error;
  }
  if ((handle_connect_map= (char *)MapViewOfFile(handle_connect_file_map,
              FILE_MAP_WRITE,0,0,
              sizeof(DWORD))) == 0)
  {
    errmsg= "Could not create shared memory service";
    goto error;
  }

  while (!abort_loop)
  {
    /* Wait a request from client */
    WaitForSingleObject(smem_event_connect_request,INFINITE);

    /*
       it can be after shutdown command
    */
    if (abort_loop)
      goto error;

    HANDLE handle_client_file_map= 0;
    char  *handle_client_map= 0;
    HANDLE event_client_wrote= 0;
    HANDLE event_client_read= 0;    // for transfer data server <-> client
    HANDLE event_server_wrote= 0;
    HANDLE event_server_read= 0;
    HANDLE event_conn_closed= 0;
    THD *thd= 0;

    p= int10_to_str(connect_number, connect_number_char, 10);
    /*
      The name of event and file-mapping events create agree next rule:
        shared_memory_base_name+unique_part+number_of_connection
        Where:
    shared_memory_base_name is uniquel value for each server
    unique_part is unique value for each object (events and file-mapping)
    number_of_connection is connection-number between server and client
    */
    suffix_pos= strxmov(tmp,shared_memory_base_name,"_",connect_number_char,
       "_",NullS);
    strmov(suffix_pos, "DATA");
    if ((handle_client_file_map=
         CreateFileMapping(INVALID_HANDLE_VALUE, sa_mapping,
                           PAGE_READWRITE, 0, smem_buffer_length, tmp)) == 0)
    {
      errmsg= "Could not create file mapping";
      goto errorconn;
    }
    if ((handle_client_map= (char*)MapViewOfFile(handle_client_file_map,
              FILE_MAP_WRITE,0,0,
              smem_buffer_length)) == 0)
    {
      errmsg= "Could not create memory map";
      goto errorconn;
    }
    strmov(suffix_pos, "CLIENT_WROTE");
    if ((event_client_wrote= CreateEvent(sa_event, FALSE, FALSE, tmp)) == 0)
    {
      errmsg= "Could not create client write event";
      goto errorconn;
    }
    strmov(suffix_pos, "CLIENT_READ");
    if ((event_client_read= CreateEvent(sa_event, FALSE, FALSE, tmp)) == 0)
    {
      errmsg= "Could not create client read event";
      goto errorconn;
    }
    strmov(suffix_pos, "SERVER_READ");
    if ((event_server_read= CreateEvent(sa_event, FALSE, FALSE, tmp)) == 0)
    {
      errmsg= "Could not create server read event";
      goto errorconn;
    }
    strmov(suffix_pos, "SERVER_WROTE");
    if ((event_server_wrote= CreateEvent(sa_event,
                                         FALSE, FALSE, tmp)) == 0)
    {
      errmsg= "Could not create server write event";
      goto errorconn;
    }
    strmov(suffix_pos, "CONNECTION_CLOSED");
    if ((event_conn_closed= CreateEvent(sa_event,
                                        TRUE, FALSE, tmp)) == 0)
    {
      errmsg= "Could not create closed connection event";
      goto errorconn;
    }
    if (abort_loop)
      goto errorconn;
    if (!(thd= new THD))
      goto errorconn;
    /* Send number of connection to client */
    int4store(handle_connect_map, connect_number);
    if (!SetEvent(event_connect_answer))
    {
      errmsg= "Could not send answer event";
      goto errorconn;
    }
    /* Set event that client should receive data */
    if (!SetEvent(event_client_read))
    {
      errmsg= "Could not set client to read mode";
      goto errorconn;
    }
    NET* net = thd->get_net();
    if (!(net->vio= vio_new_win32shared_memory(handle_client_file_map,
                                                   handle_client_map,
                                                   event_client_wrote,
                                                   event_client_read,
                                                   event_server_wrote,
                                                   event_server_read,
                                                   event_conn_closed)) ||
                        my_net_init(net, net->vio))
    {
      close_connection(thd, ER_OUT_OF_RESOURCES);
      errmsg= 0;
      goto errorconn;
    }
    thd->security_ctx->set_host(my_strdup(my_localhost, MYF(0))); /* Host is unknown */
    create_new_thread(thd);
    connect_number++;
    continue;

errorconn:
    /* Could not form connection;  Free used handlers/memort and retry */
    if (errmsg)
    {
      char buff[180];
      strxmov(buff, "Can't create shared memory connection: ", errmsg, ".",
        NullS);
      sql_perror(buff);
    }
    if (handle_client_file_map)
      CloseHandle(handle_client_file_map);
    if (handle_client_map)
      UnmapViewOfFile(handle_client_map);
    if (event_server_wrote)
      CloseHandle(event_server_wrote);
    if (event_server_read)
      CloseHandle(event_server_read);
    if (event_client_wrote)
      CloseHandle(event_client_wrote);
    if (event_client_read)
      CloseHandle(event_client_read);
    if (event_conn_closed)
      CloseHandle(event_conn_closed);
    delete thd;
  }

  /* End shared memory handling */
error:
  if (tmp)
    my_free(tmp);

  if (errmsg)
  {
    char buff[180];
    strxmov(buff, "Can't create shared memory service: ", errmsg, ".", NullS);
    sql_perror(buff);
  }
  my_security_attr_free(sa_event);
  my_security_attr_free(sa_mapping);
  if (handle_connect_map) UnmapViewOfFile(handle_connect_map);
  if (handle_connect_file_map)  CloseHandle(handle_connect_file_map);
  if (event_connect_answer) CloseHandle(event_connect_answer);
  if (smem_event_connect_request) CloseHandle(smem_event_connect_request);
  DBUG_LEAVE;
  decrement_handler_count();
  return 0;
}
#endif /* HAVE_SMEM */
#endif /* EMBEDDED_LIBRARY */


/****************************************************************************
  Handle start options
******************************************************************************/

/**
  Process command line options flagged as 'early'.
  Some components needs to be initialized as early as possible,
  because the rest of the server initialization depends on them.
  Options that needs to be parsed early includes:
  - the performance schema, when compiled in,
  - options related to the help,
  - options related to the bootstrap
  The performance schema needs to be initialized as early as possible,
  before to-be-instrumented objects of the server are initialized.
*/
int handle_early_options(my_bool logging)
{
  int ho_error;
  vector<my_option> all_early_options;
  all_early_options.reserve(100);

  my_getopt_register_get_addr(NULL);
  /* Skip unknown options so that they may be processed later */
  my_getopt_skip_unknown= TRUE;

  /* Add the system variables parsed early */
  sys_var_add_options(&all_early_options, sys_var::PARSE_EARLY);

  /* Add the command line options parsed early */
  for (my_option *opt= my_long_early_options;
       opt->name != NULL;
       opt++)
    all_early_options.push_back(*opt);

  add_terminator(&all_early_options);

  /*
    Logs generated while parsing the command line
    options are buffered and printed later.
  */
  buffered_logs.init();
  my_getopt_error_reporter= buffered_option_error_reporter;
  my_charset_error_reporter= buffered_option_error_reporter;

  if (logging)
  {
    ho_error = handle_options_with_logging(&remaining_argc, &remaining_argv,
                                           &all_early_options[0],
                                           mysqld_get_one_option);
  }
  else
  {
    ho_error = handle_options(&remaining_argc, &remaining_argv,
                              &all_early_options[0],
                              mysqld_get_one_option);
  }

  if (ho_error == 0)
  {
    /* Add back the program name handle_options removes */
    remaining_argc++;
    remaining_argv--;
  }

  // Swap with an empty vector, i.e. delete elements and free allocated space.
  vector<my_option>().swap(all_early_options);

  return ho_error;
}

/**
  Adjust @c open_files_limit.
  Computation is  based on:
  - @c max_connections,
  - @c table_cache_size,
  - the platform max open file limit.
*/
void adjust_open_files_limit(ulong *requested_open_files)
{
  ulong limit_1;
  ulong limit_2;
  ulong limit_3;
  ulong request_open_files;
  ulong effective_open_files;

  /* MyISAM requires two file handles per table. */
  limit_1= 10 + max_connections + table_cache_size * 2;

  /*
    We are trying to allocate no less than max_connections*5 file
    handles (i.e. we are trying to set the limit so that they will
    be available).
  */
  limit_2= max_connections * 5;

  /* Try to allocate no less than 5000 by default. */
  limit_3= open_files_limit ? open_files_limit : 5000;

  request_open_files= max<ulong>(max<ulong>(limit_1, limit_2), limit_3);

  /* Notice: my_set_max_open_files() may return more than requested. */
  effective_open_files= my_set_max_open_files(request_open_files);

  if (effective_open_files < request_open_files)
  {
    char msg[1024];

    if (open_files_limit == 0)
    {
      snprintf(msg, sizeof(msg),
               "Changed limits: max_open_files: %lu (requested %lu)",
               effective_open_files, request_open_files);
      buffered_logs.buffer(WARNING_LEVEL, msg);
    }
    else
    {
      snprintf(msg, sizeof(msg),
               "Could not increase number of max_open_files to "
               "more than %lu (request: %lu)",
               effective_open_files, request_open_files);
      buffered_logs.buffer(WARNING_LEVEL, msg);
    }
  }

  open_files_limit= effective_open_files;
  if (requested_open_files)
    *requested_open_files= min<ulong>(effective_open_files, request_open_files);
}

void adjust_max_connections(ulong requested_open_files)
{
  ulong limit;

  limit= requested_open_files - 10 - TABLE_OPEN_CACHE_MIN * 2;

  if (limit < max_connections)
  {
    char msg[1024];

    snprintf(msg, sizeof(msg),
             "Changed limits: max_connections: %lu (requested %lu)",
             limit, max_connections);
    buffered_logs.buffer(WARNING_LEVEL, msg);

    max_connections= limit;
  }
}

void adjust_table_cache_size(ulong requested_open_files)
{
  ulong limit;

  limit= max<ulong>((requested_open_files - 10 - max_connections) / 2,
                    TABLE_OPEN_CACHE_MIN);

  if (limit < table_cache_size)
  {
    char msg[1024];

    snprintf(msg, sizeof(msg),
             "Changed limits: table_open_cache: %lu (requested %lu)",
             limit, table_cache_size);
    buffered_logs.buffer(WARNING_LEVEL, msg);

    table_cache_size= limit;
  }

  table_cache_size_per_instance= table_cache_size / table_cache_instances;
}

void adjust_table_def_size()
{
  longlong default_value;
  sys_var *var;

  default_value= min<longlong> (400 + table_cache_size / 2, 2000);
  var= intern_find_sys_var(STRING_WITH_LEN("table_definition_cache"));
  DBUG_ASSERT(var != NULL);
  var->update_default(default_value);

  if (! table_definition_cache_specified)
    table_def_size= default_value;
}

void adjust_related_options(ulong *requested_open_files)
{
  /* In bootstrap, disable grant tables (we are about to create them) */
  if (opt_bootstrap)
    opt_noacl= 1;

  /* The order is critical here, because of dependencies. */
  adjust_open_files_limit(requested_open_files);
  adjust_max_connections(*requested_open_files);
  adjust_table_cache_size(*requested_open_files);
  adjust_table_def_size();
}

vector<my_option> all_options;

struct my_option my_long_early_options[]=
{
  {"allow-multiple-engines", 0, "Allow multiple storage engines with "
    "transaction support (such as innodb and rocksdb) to be enabled.",
   &opt_allow_multiple_engines, &opt_allow_multiple_engines, 0, GET_BOOL,
   NO_ARG, 0, 0, 0, 0, 0, 0},
#ifndef DISABLE_GRANT_OPTIONS
  {"bootstrap", OPT_BOOTSTRAP, "Used by mysql installation scripts.", 0, 0, 0,
   GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
#endif
#ifndef DISABLE_GRANT_OPTIONS
  {"skip-grant-tables", 0,
   "Start without grant tables. This gives all users FULL ACCESS to all tables.",
   &opt_noacl, &opt_noacl, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0,
   0},
#endif
  {"help", '?', "Display this help and exit.",
   &opt_help, &opt_help, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0,
   0, 0},
  {"verbose", 'v', "Used with --help option for detailed help.",
   &opt_verbose, &opt_verbose, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"version", 'V', "Output version information and exit.", 0, 0, 0, GET_NO_ARG,
   NO_ARG, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
};

/**
  System variables are automatically command-line options (few
  exceptions are documented in sys_vars.h), so don't need
  to be listed here.
*/

struct my_option my_long_options[]=
{
#ifdef HAVE_REPLICATION
  {"abort-slave-event-count", 0,
   "Option used by mysql-test for debugging and testing of replication.",
   &abort_slave_event_count,  &abort_slave_event_count,
   0, GET_INT, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
#endif /* HAVE_REPLICATION */
  {"allow-suspicious-udfs", 0,
   "Allows use of UDFs consisting of only one symbol xxx() "
   "without corresponding xxx_init() or xxx_deinit(). That also means "
   "that one can load any function from any library, for example exit() "
   "from libc.so",
   &opt_allow_suspicious_udfs, &opt_allow_suspicious_udfs,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"ansi", 'a', "Use ANSI SQL syntax instead of MySQL syntax. This mode "
   "will also set transaction isolation level 'serializable'.", 0, 0, 0,
   GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  /*
    Because Sys_var_bit does not support command-line options, we need to
    explicitly add one for --autocommit
  */
  {"autocommit", 0, "Set default value for autocommit (0 or 1)",
   &opt_autocommit, &opt_autocommit, 0,
   GET_BOOL, OPT_ARG, 1, 0, 0, 0, 0, NULL},
  {"binlog-do-db", OPT_BINLOG_DO_DB,
   "Tells the master it should log updates for the specified database, "
   "and exclude all others not explicitly mentioned.",
   0, 0, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"binlog-ignore-db", OPT_BINLOG_IGNORE_DB,
   "Tells the master that updates to the given database should not be logged to the binary log.",
   0, 0, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"binlog-row-event-max-size", 0,
   "The maximum size of a row-based binary log event in bytes. Rows will be "
   "grouped into events smaller than this size if possible. "
   "The value has to be a multiple of 256.",
   &opt_binlog_rows_event_max_size, &opt_binlog_rows_event_max_size,
   0, GET_ULONG, REQUIRED_ARG,
   /* def_value */ 8192, /* min_value */  256, /* max_value */ ULONG_MAX,
   /* sub_size */     0, /* block_size */ 256,
   /* app_type */ 0
  },
  {"character-set-client-handshake", 0,
   "Don't ignore client side character set value sent during handshake.",
   &opt_character_set_client_handshake,
   &opt_character_set_client_handshake,
    0, GET_BOOL, NO_ARG, 1, 0, 0, 0, 0, 0},
  {"character-set-filesystem", 0,
   "Set the filesystem character set.",
   &character_set_filesystem_name,
   &character_set_filesystem_name,
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0 },
  {"character-set-server", 'C', "Set the default character set.",
   &default_character_set_name, &default_character_set_name,
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0 },
  {"chroot", 'r', "Chroot mysqld daemon during startup.",
   &mysqld_chroot, &mysqld_chroot, 0, GET_STR, REQUIRED_ARG,
   0, 0, 0, 0, 0, 0},
  {"collation-server", 0, "Set the default collation.",
   &default_collation_name, &default_collation_name,
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0 },
  {"console", OPT_CONSOLE, "Write error output on screen; don't remove the console window on windows.",
   &opt_console, &opt_console, 0, GET_BOOL, NO_ARG, 0, 0, 0,
   0, 0, 0},
  {"core-file", 0, "Write core on errors.", &opt_core_file, &opt_core_file, 0, GET_BOOL,
   NO_ARG, 0, 0, 0, 0, 0, 0},
  /* default-storage-engine should have "MyISAM" as def_value. Instead
     of initializing it here it is done in init_common_variables() due
     to a compiler bug in Sun Studio compiler. */
  {"default-storage-engine", 0, "The default storage engine for new tables",
   &default_storage_engine, 0, 0, GET_STR, REQUIRED_ARG,
   0, 0, 0, 0, 0, 0 },
  {"default-tmp-storage-engine", 0,
    "The default storage engine for new explict temporary tables",
   &default_tmp_storage_engine, 0, 0, GET_STR, REQUIRED_ARG,
   0, 0, 0, 0, 0, 0 },
  {"default-time-zone", 0, "Set the default time zone.",
   &default_tz_name, &default_tz_name,
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0 },
#ifdef HAVE_OPENSSL
  {"des-key-file", 0,
   "Load keys for des_encrypt() and des_encrypt from given file.",
   &des_key_file, &des_key_file, 0, GET_STR, REQUIRED_ARG,
   0, 0, 0, 0, 0, 0},
#endif /* HAVE_OPENSSL */
#ifdef HAVE_REPLICATION
  {"disconnect-slave-event-count", 0,
   "Option used by mysql-test for debugging and testing of replication.",
   &disconnect_slave_event_count, &disconnect_slave_event_count,
   0, GET_INT, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
#endif /* HAVE_REPLICATION */
  {"exit-info", 'T', "Used for debugging. Use at your own risk.", 0, 0, 0,
   GET_LONG, OPT_ARG, 0, 0, 0, 0, 0, 0},

  {"external-locking", 0, "Use system (external) locking (disabled by "
   "default).  With this option enabled you can run myisamchk to test "
   "(not repair) tables while the MySQL server is running. Disable with "
   "--skip-external-locking.", &opt_external_locking, &opt_external_locking,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  /* We must always support the next option to make scripts like mysqltest
     easier to do */
  {"gdb", 0,
   "Set up signals usable for debugging.",
   &opt_debugging, &opt_debugging,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
#ifdef HAVE_LARGE_PAGE_OPTION
  {"super-large-pages", 0, "Enable support for super large pages.",
   &opt_super_large_pages, &opt_super_large_pages, 0,
   GET_BOOL, OPT_ARG, 0, 0, 1, 0, 1, 0},
#endif
  {"ignore-db-dir", OPT_IGNORE_DB_DIRECTORY,
   "Specifies a directory to add to the ignore list when collecting "
   "database names from the datadir. Put a blank argument to reset "
   "the list accumulated so far.", 0, 0, 0, GET_STR, REQUIRED_ARG,
   0, 0, 0, 0, 0, 0},
  {"language", 'L',
   "Client error messages in given language. May be given as a full path. "
   "Deprecated. Use --lc-messages-dir instead.",
   &lc_messages_dir_ptr, &lc_messages_dir_ptr, 0,
   GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"lc-messages", 0,
   "Set the language used for the error messages.",
   &lc_messages, &lc_messages, 0, GET_STR, REQUIRED_ARG,
   0, 0, 0, 0, 0, 0 },
  {"lc-time-names", 0,
   "Set the language used for the month names and the days of the week.",
   &lc_time_names_name, &lc_time_names_name,
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0 },
  {"log-bin", OPT_BIN_LOG,
   "Log update queries in binary format. Optional (but strongly recommended "
   "to avoid replication problems if server's hostname changes) argument "
   "should be the chosen location for the binary log files.",
   &opt_bin_logname, &opt_bin_logname, 0, GET_STR_ALLOC,
   OPT_ARG, 0, 0, 0, 0, 0, 0},
  {"log-bin-index", 0,
   "File that holds the names for binary log files.",
   &opt_binlog_index_name, &opt_binlog_index_name, 0, GET_STR,
   REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"relay-log-index", 0,
   "File that holds the names for relay log files.",
   &opt_relaylog_index_name, &opt_relaylog_index_name, 0, GET_STR,
   REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"log-isam", OPT_ISAM_LOG, "Log all MyISAM changes to file.",
   &myisam_log_filename, &myisam_log_filename, 0, GET_STR,
   OPT_ARG, 0, 0, 0, 0, 0, 0},
  {"log-raw", 0,
   "Log to general log before any rewriting of the query. For use in debugging, not production as "
   "sensitive information may be logged.",
   &opt_log_raw, &opt_log_raw,
   0, GET_BOOL, NO_ARG, 0, 0, 1, 0, 1, 0 },
  {"log-short-format", 0,
   "Don't log extra information to update and slow-query logs.",
   &opt_short_log_format, &opt_short_log_format,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"log-tc", 0,
   "Path to transaction coordinator log (used for transactions that affect "
   "more than one storage engine, when binary log is disabled).",
   &opt_tc_log_file, &opt_tc_log_file, 0, GET_STR,
   REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
#ifdef HAVE_MMAP
  {"log-tc-size", 0, "Size of transaction coordinator log.",
   &opt_tc_log_size, &opt_tc_log_size, 0, GET_ULONG,
   REQUIRED_ARG, TC_LOG_MIN_SIZE, TC_LOG_MIN_SIZE, ULONG_MAX, 0,
   TC_LOG_PAGE_SIZE, 0},
#endif
  {"master-info-file", 0,
   "The location and name of the file that remembers the master and where "
   "the I/O replication thread is in the master's binlogs.",
   &master_info_file, &master_info_file, 0, GET_STR,
   REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"master-retry-count", OPT_MASTER_RETRY_COUNT,
   "The number of tries the slave will make to connect to the master before giving up. "
   "Deprecated option, use 'CHANGE MASTER TO master_retry_count = <num>' instead.",
   &master_retry_count, &master_retry_count, 0, GET_ULONG,
   REQUIRED_ARG, 3600*24, 0, 0, 0, 0, 0},
#ifdef HAVE_REPLICATION
  {"max-binlog-dump-events", 0,
   "Option used by mysql-test for debugging and testing of replication.",
   &max_binlog_dump_events, &max_binlog_dump_events, 0,
   GET_INT, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
#endif /* HAVE_REPLICATION */
  {"memlock", 0, "Lock mysqld in memory.", &locked_in_memory,
   &locked_in_memory, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"old-style-user-limits", 0,
   "Enable old-style user limits (before 5.0.3, user resources were counted "
   "per each user+host vs. per account).",
   &opt_old_style_user_limits, &opt_old_style_user_limits,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"port-open-timeout", 0,
   "Maximum time in seconds to wait for the port to become free. "
   "(Default: No wait).", &mysqld_port_timeout, &mysqld_port_timeout, 0,
   GET_UINT, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"replicate-do-db", OPT_REPLICATE_DO_DB,
   "Tells the slave thread to restrict replication to the specified database. "
   "To specify more than one database, use the directive multiple times, "
   "once for each database. Note that this will only work if you do not use "
   "cross-database queries such as UPDATE some_db.some_table SET foo='bar' "
   "while having selected a different or no database. If you need cross "
   "database updates to work, make sure you have 3.23.28 or later, and use "
   "replicate-wild-do-table=db_name.%.",
   0, 0, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"replicate-do-table", OPT_REPLICATE_DO_TABLE,
   "Tells the slave thread to restrict replication to the specified table. "
   "To specify more than one table, use the directive multiple times, once "
   "for each table. This will work for cross-database updates, in contrast "
   "to replicate-do-db.", 0, 0, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"replicate-ignore-db", OPT_REPLICATE_IGNORE_DB,
   "Tells the slave thread to not replicate to the specified database. To "
   "specify more than one database to ignore, use the directive multiple "
   "times, once for each database. This option will not work if you use "
   "cross database updates. If you need cross database updates to work, "
   "make sure you have 3.23.28 or later, and use replicate-wild-ignore-"
   "table=db_name.%. ", 0, 0, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"replicate-ignore-table", OPT_REPLICATE_IGNORE_TABLE,
   "Tells the slave thread to not replicate to the specified table. To specify "
   "more than one table to ignore, use the directive multiple times, once for "
   "each table. This will work for cross-database updates, in contrast to "
   "replicate-ignore-db.", 0, 0, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"replicate-rewrite-db", OPT_REPLICATE_REWRITE_DB,
   "Updates to a database with a different name than the original. Example: "
   "replicate-rewrite-db=master_db_name->slave_db_name.",
   0, 0, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
#ifdef HAVE_REPLICATION
  {"replicate-same-server-id", 0,
   "In replication, if set to 1, do not skip events having our server id. "
   "Default value is 0 (to break infinite loops in circular replication). "
   "Can't be set to 1 if --log-slave-updates is used.",
   &replicate_same_server_id, &replicate_same_server_id,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
#endif
  {"replicate-wild-do-table", OPT_REPLICATE_WILD_DO_TABLE,
   "Tells the slave thread to restrict replication to the tables that match "
   "the specified wildcard pattern. To specify more than one table, use the "
   "directive multiple times, once for each table. This will work for cross-"
   "database updates. Example: replicate-wild-do-table=foo%.bar% will "
   "replicate only updates to tables in all databases that start with foo "
   "and whose table names start with bar.",
   0, 0, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"replicate-wild-ignore-table", OPT_REPLICATE_WILD_IGNORE_TABLE,
   "Tells the slave thread to not replicate to the tables that match the "
   "given wildcard pattern. To specify more than one table to ignore, use "
   "the directive multiple times, once for each table. This will work for "
   "cross-database updates. Example: replicate-wild-ignore-table=foo%.bar% "
   "will not do updates to tables in databases that start with foo and whose "
   "table names start with bar.",
   0, 0, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"safe-user-create", 0,
   "Don't allow new user creation by the user who has no write privileges to the mysql.user table.",
   &opt_safe_user_create, &opt_safe_user_create, 0, GET_BOOL,
   NO_ARG, 0, 0, 0, 0, 0, 0},
  {"show-slave-auth-info", 0,
   "Show user and password in SHOW SLAVE HOSTS on this master.",
   &opt_show_slave_auth_info, &opt_show_slave_auth_info, 0,
   GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"skip-host-cache", OPT_SKIP_HOST_CACHE, "Don't cache host names.", 0, 0, 0,
   GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"skip-new", OPT_SKIP_NEW, "Don't use new, possibly wrong routines.",
   0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"skip-slave-start", 0,
   "If set, slave is not autostarted.", &opt_skip_slave_start,
   &opt_skip_slave_start, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"skip-stack-trace", OPT_SKIP_STACK_TRACE,
   "Don't print a stack trace on failure.", 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0,
   0, 0, 0, 0},
  {"fatal-semaphore-timeout", OPT_SRV_FATAL_SEMAPHORE_TIMEOUT,
   "Maximum number of seconds that semaphore times out in innodb.",
   &opt_srv_fatal_semaphore_timeout, &opt_srv_fatal_semaphore_timeout,
   0, GET_ULONG, REQUIRED_ARG,
   DEFAULT_SRV_FATAL_SEMAPHORE_TIMEOUT, 1, ULONG_MAX, 0, 0, 0},
#if defined(_WIN32) && !defined(EMBEDDED_LIBRARY)
  {"slow-start-timeout", 0,
   "Maximum number of milliseconds that the service control manager should wait "
   "before trying to kill the windows service during startup"
   "(Default: 15000).", &slow_start_timeout, &slow_start_timeout, 0,
   GET_ULONG, REQUIRED_ARG, 15000, 0, 0, 0, 0, 0},
#endif
#ifdef HAVE_REPLICATION
  {"sporadic-binlog-dump-fail", 0,
   "Option used by mysql-test for debugging and testing of replication.",
   &opt_sporadic_binlog_dump_fail,
   &opt_sporadic_binlog_dump_fail, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0,
   0},
#endif /* HAVE_REPLICATION */
#ifdef __WIN__
  {"standalone", 0,
  "Dummy option to start as a standalone program (NT).", 0, 0, 0, GET_NO_ARG,
   NO_ARG, 0, 0, 0, 0, 0, 0},
#endif
  {"symbolic-links", 's', "Enable symbolic link support.",
   &my_use_symdir, &my_use_symdir, 0, GET_BOOL, NO_ARG,
   /*
     The system call realpath() produces warnings under valgrind and
     purify. These are not suppressed: instead we disable symlinks
     option if compiled with valgrind support.
   */
   IF_PURIFY(0,1), 0, 0, 0, 0, 0},
  {"sysdate-is-now", 0,
   "Non-default option to alias SYSDATE() to NOW() to make it safe-replicable. "
   "Since 5.0, SYSDATE() returns a `dynamic' value different for different "
   "invocations, even within the same statement.",
   &global_system_variables.sysdate_is_now,
   0, 0, GET_BOOL, NO_ARG, 0, 0, 1, 0, 1, 0},
  {"tc-heuristic-recover", 0,
   "Decision to use in heuristic recover process. Possible values are COMMIT "
   "or ROLLBACK.", &tc_heuristic_recover, &tc_heuristic_recover,
   &tc_heuristic_recover_typelib, GET_ENUM, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
#if defined(ENABLED_DEBUG_SYNC)
  {"debug-sync-timeout", OPT_DEBUG_SYNC_TIMEOUT,
   "Enable the debug sync facility "
   "and optionally specify a default wait timeout in seconds. "
   "A zero value keeps the facility disabled.",
   &opt_debug_sync_timeout, 0,
   0, GET_UINT, OPT_ARG, 0, 0, UINT_MAX, 0, 0, 0},
#endif /* defined(ENABLED_DEBUG_SYNC) */
  {"temp-pool", 0,
#if (ENABLE_TEMP_POOL)
   "Using this option will cause most temporary files created to use a small "
   "set of names, rather than a unique name for each new file.",
#else
   "This option is ignored on this OS.",
#endif
   &use_temp_pool, &use_temp_pool, 0, GET_BOOL, NO_ARG, 1,
   0, 0, 0, 0, 0},
  {"transaction-isolation", 0,
   "Default transaction isolation level.",
   &global_system_variables.tx_isolation,
   &global_system_variables.tx_isolation, &tx_isolation_typelib,
   GET_ENUM, REQUIRED_ARG, ISO_REPEATABLE_READ, 0, 0, 0, 0, 0},
  {"transaction-read-only", 0,
   "Default transaction access mode. "
   "True if transactions are read-only.",
   &global_system_variables.tx_read_only,
   &global_system_variables.tx_read_only, 0,
   GET_BOOL, OPT_ARG, 0, 0, 0, 0, 0, 0},
  {"user", 'u', "Run mysqld daemon as user.", 0, 0, 0, GET_STR, REQUIRED_ARG,
   0, 0, 0, 0, 0, 0},
  {"plugin-load", OPT_PLUGIN_LOAD,
   "Optional semicolon-separated list of plugins to load, where each plugin is "
   "identified as name=library, where name is the plugin name and library "
   "is the plugin library in plugin_dir.",
   0, 0, 0,
   GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"plugin-load-add", OPT_PLUGIN_LOAD_ADD,
   "Optional semicolon-separated list of plugins to load, where each plugin is "
   "identified as name=library, where name is the plugin name and library "
   "is the plugin library in plugin_dir. This option adds to the list "
   "speficied by --plugin-load in an incremental way. "
   "Multiple --plugin-load-add are supported.",
   0, 0, 0,
   GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"default_authentication_plugin", OPT_DEFAULT_AUTH,
   "Defines what password- and authentication algorithm to use per default",
   0, 0, 0,
   GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"log_slow_extra", OPT_LOG_SLOW_EXTRA,
   "Print more attributes to the slow query log",
   (uchar**) &opt_log_slow_extra, (uchar**) &opt_log_slow_extra,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"trim-binlog-to-recover", OPT_TRIM_BINLOG_TO_RECOVER,
   "Trim the last binlog (if required) to the position until which the "
   "engine has successfully committed all transactions.",
   &opt_trim_binlog, &opt_trim_binlog, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
};

#ifdef HAVE_JEMALLOC
#ifndef EMBEDDED_LIBRARY
std::atomic_bool need_update_malloc_status;

static void update_malloc_status()
{
  if (!need_update_malloc_status)
    return;

  uint64_t val= 1;
  size_t len = sizeof(val);

  mallctl("epoch", &val, &len, &val, sizeof(uint64_t));
  need_update_malloc_status = false;
}

static int show_jemalloc_sizet(THD *thd, SHOW_VAR *var, char *buff,
                               const char* stat_name)
{
  size_t len, val;
  len = sizeof(val);

  var->type= SHOW_LONGLONG;
  var->value= buff;

  if (!mallctl(stat_name, &val, &len, NULL, 0))
  {
    *((ulonglong *)buff)= (ulonglong) val;
  }
  else
  {
    *((ulonglong *)buff)= 0;
  }

  /* Always return 0 to avoid worrying about error handling */
  return 0;
}

static int show_jemalloc_unsigned(THD *thd, SHOW_VAR *var, char *buff,
                                  const char* stat_name)
{
  unsigned val;
  size_t len = sizeof(val);

  var->type= SHOW_LONG;
  var->value= buff;

  if (!mallctl(stat_name, &val, &len, NULL, 0))
  {
    *((ulong *)buff)= (ulong) val;
  }
  else
  {
    *((ulong *)buff)= 0;
  }

  /* Always return 0 to avoid worrying about error handling */
  return 0;
}

static int show_jemalloc_bool(THD *thd, SHOW_VAR *var, char *buff,
                              const char* stat_name)
{
  bool val;
  size_t len = sizeof(val);

  var->type= SHOW_LONG;
  var->value= buff;

  if (!mallctl(stat_name, &val, &len, NULL, 0))
  {
    *((ulong *)buff)= (ulong) val;
  }
  else
  {
    *((ulong *)buff)= 0;
  }

  /* Always return 0 to avoid worrying about error handling */
  return 0;
}

static int show_jemalloc_arenas_narenas(THD *thd, SHOW_VAR *var, char *buff)
{
  update_malloc_status();
  return show_jemalloc_unsigned(thd, var, buff, "arenas.narenas");
}

static int show_jemalloc_opt_narenas(THD *thd, SHOW_VAR *var, char *buff)
{
  update_malloc_status();
  return show_jemalloc_sizet(thd, var, buff, "opt.narenas");
}

static int show_jemalloc_opt_tcache(THD *thd, SHOW_VAR *var, char *buff)
{
  update_malloc_status();
  return show_jemalloc_bool(thd, var, buff, "opt.tcache");
}

static int show_jemalloc_active(THD *thd, SHOW_VAR *var, char *buff)
{
  update_malloc_status();
  return show_jemalloc_sizet(thd, var, buff, "stats.active");
}

static int show_jemalloc_allocated(THD *thd, SHOW_VAR *var, char *buff)
{
  update_malloc_status();
  return show_jemalloc_sizet(thd, var, buff, "stats.allocated");
}

static int show_jemalloc_mapped(THD *thd, SHOW_VAR *var, char *buff)
{
  update_malloc_status();
  return show_jemalloc_sizet(thd, var, buff, "stats.mapped");
}

static int show_jemalloc_metadata(THD *thd, SHOW_VAR *var, char *buff)
{
  update_malloc_status();
  return show_jemalloc_sizet(thd, var, buff, "stats.metadata");
}

bool enable_jemalloc_hppfunc(char *enable_profiling_ptr)
{
  std::string epp(enable_profiling_ptr);
  std::transform(epp.begin(), epp.end(), epp.begin(), ::tolower);

  bool enable_profiling = epp.std::string::compare("on") == 0;
  bool disable_profiling = epp.std::string::compare("off") == 0;
  bool dump_profiling = epp.std::string::compare("dump") == 0;

  if (enable_profiling)
  {
    size_t val_sz = sizeof(enable_profiling);
    int ret = mallctl("prof.active", nullptr,
        nullptr, &enable_profiling, val_sz);

    if (ret != 0) {
      /* NO_LINT_DEBUG */
      sql_print_information("Unable to enable "
          "jemalloc heap profiling. Error:  %d", ret);
      return false;
    }
    return true;
  }
  if (disable_profiling)
  {
    disable_profiling = false;
    size_t val_sz = sizeof(disable_profiling);
    int ret = mallctl("prof.active", nullptr,
        nullptr, &disable_profiling , val_sz);

    if (ret != 0) {
      /* NO_LINT_DEBUG */
      sql_print_information("Unable to disable "
          "jemalloc heap profiling. Error: %d", ret);
      return false;
    }
    return true;
  }
  if (dump_profiling)
  {
    int ret = mallctl("prof.dump", nullptr, nullptr,
        nullptr, sizeof(const char *));

    if (ret != 0) {
      /* NO_LINT_DEBUG */
      sql_print_information("Unable to dump heap. Error:  %d", ret);
      return false;
    }
    return true;
  }
  return false;
}

#endif
#endif

static inline ulonglong get_super_read_only_block_microsec(
    std::atomic_ullong& lock_block_timer)
{
  ulonglong lock_wait_microsec =
    lock_block_timer.load(std::memory_order_seq_cst);
  if(lock_wait_microsec)
    lock_wait_microsec =
      (my_timer_to_microseconds_ulonglong(
        my_timer_since(lock_wait_microsec)));
  return lock_wait_microsec;
}

static int show_super_read_only_block_microsec(
  THD *thd, SHOW_VAR *var, char *buff)
{
  var->type = SHOW_LONGLONG;
  var->value = buff;
  ulonglong super_read_only_block_microsec =
    get_super_read_only_block_microsec(init_global_rolock_timer);

  ulonglong commit_lock_wait =
    get_super_read_only_block_microsec(init_commit_lock_timer);

  if(commit_lock_wait > super_read_only_block_microsec)
    super_read_only_block_microsec = commit_lock_wait;

  *((longlong *)buff) = super_read_only_block_microsec;

  return 0;
}

static int show_queries(THD *thd, SHOW_VAR *var, char *buff)
{
  var->type= SHOW_LONGLONG;
  var->value= (char *)&thd->query_id;
  return 0;
}


static int show_net_compression(THD *thd, SHOW_VAR *var, char *buff)
{
  var->type= SHOW_MY_BOOL;
  var->value= (char *)&thd->get_net()->compress;
  return 0;
}

static int show_starttime(THD *thd, SHOW_VAR *var, char *buff)
{
  var->type= SHOW_LONGLONG;
  var->value= buff;
  *((longlong *)buff)= (longlong) (thd->query_start() - server_start_time);
  return 0;
}

static int show_thread_created(THD *thd, SHOW_VAR *var, char *buff)
{
  var->type= SHOW_LONG_NOFLUSH;
  var->value= buff;
  *((long *)buff)= (long)thread_created.load();
  return 0;
}

static int show_stmt_time(THD *thd, SHOW_VAR *var, char *buff)
{
  var->type = SHOW_TIMER;
  var->value = buff;
  // if there is an in-fly query, calculate the in-fly statement time
  if (thd->stmt_start)
    *((longlong *)buff) = (longlong)(my_timer_since(thd->stmt_start));
  else
    *((longlong *)buff) = (longlong)0;
  return 0;
}

static int show_trx_time(THD *thd, SHOW_VAR *var, char *buff)
{
  var->type = SHOW_TIMER;
  var->value = buff;
  // if there is an in-fly query, calculate the transaction time including the
  // in-fly query
  if (thd->stmt_start)
    *((longlong *)buff) =
        (longlong)(thd->trx_time + my_timer_since(thd->stmt_start));
  else
    *((longlong *)buff) = (longlong)(thd->trx_time);
  return 0;
}

#ifdef ENABLED_PROFILING
static int show_flushstatustime(THD *thd, SHOW_VAR *var, char *buff)
{
  var->type= SHOW_LONGLONG;
  var->value= buff;
  *((longlong *)buff)= (longlong) (thd->query_start() - flush_status_time);
  return 0;
}
#endif

#ifdef HAVE_REPLICATION
static int show_skip_unique_check(THD *thd, SHOW_VAR *var, char *buff)
{
  var->type= SHOW_BOOL;
  var->value= buff;
  *((bool *)buff)= active_mi && active_mi->rli->skip_unique_check;
  return 0;
}

static int show_slave_running(THD *thd, SHOW_VAR *var, char *buff)
{
  var->type= SHOW_MY_BOOL;
  var->value= buff;
  *((my_bool *)buff)= (my_bool) (active_mi &&
                                 active_mi->slave_running == MYSQL_SLAVE_RUN_CONNECT &&
                                 active_mi->rli->slave_running);
  return 0;
}

static int show_slave_dependency_in_queue(THD *thd, SHOW_VAR *var, char *buff)
{
  if (active_mi && active_mi->rli && active_mi->rli->mts_dependency_replication)
  {
    var->type= SHOW_LONGLONG;
    var->value= buff;
    mysql_mutex_lock(&active_mi->rli->dep_lock);
    *((ulonglong *)buff)= active_mi->rli->dep_queue.size();
    mysql_mutex_unlock(&active_mi->rli->dep_lock);
  }
  else
    var->type= SHOW_UNDEF;
  return 0;
}

static int show_slave_dependency_in_flight(THD *thd, SHOW_VAR *var, char *buff)
{
  if (active_mi && active_mi->rli && active_mi->rli->mts_dependency_replication)
  {
    var->type= SHOW_LONGLONG;
    var->value= buff;
    mysql_mutex_lock(&active_mi->rli->dep_lock);
    *((ulonglong *)buff)= (ulonglong) active_mi->rli->num_in_flight_trx;
    mysql_mutex_unlock(&active_mi->rli->dep_lock);
  }
  else
    var->type= SHOW_UNDEF;
  return 0;
}

static
int show_slave_dependency_begin_waits(THD *thd, SHOW_VAR *var, char *buff)
{
  if (active_mi && active_mi->rli && active_mi->rli->mts_dependency_replication)
  {
    var->type= SHOW_LONGLONG;
    var->value= buff;
    *((ulonglong *)buff)= (ulonglong) active_mi->rli->begin_event_waits.load();
  }
  else
    var->type= SHOW_UNDEF;
  return 0;
}

static int show_slave_dependency_next_waits(THD *thd, SHOW_VAR *var, char *buff)
{
  if (active_mi && active_mi->rli && active_mi->rli->mts_dependency_replication)
  {
    var->type= SHOW_LONGLONG;
    var->value= buff;
    *((ulonglong *)buff)= (ulonglong) active_mi->rli->next_event_waits.load();
  }
  else
    var->type= SHOW_UNDEF;
  return 0;
}

static int show_slave_before_image_inconsistencies(THD *thd, SHOW_VAR *var,
                                                   char *buff)
{
  if (opt_slave_check_before_image_consistency)
  {
    var->type= SHOW_LONGLONG;
    var->value= buff;
    *((ulonglong *)buff)= (ulonglong) get_num_before_image_inconsistencies();
  }
  else
    var->type= SHOW_UNDEF;
  return 0;
}

static int show_slave_retried_trans(THD *thd, SHOW_VAR *var, char *buff)
{
  /*
    TODO: with multimaster, have one such counter per line in
    SHOW SLAVE STATUS, and have the sum over all lines here.
  */
  if (active_mi)
  {
    var->type= SHOW_LONG;
    var->value= buff;
    *((long *)buff)= (long)active_mi->rli->retried_trans;
  }
  else
    var->type= SHOW_UNDEF;
  return 0;
}

static int show_slave_received_heartbeats(THD *thd, SHOW_VAR *var, char *buff)
{
  if (active_mi)
  {
    var->type= SHOW_LONGLONG;
    var->value= buff;
    *((longlong *)buff)= active_mi->received_heartbeats;
  }
  else
    var->type= SHOW_UNDEF;
  return 0;
}

static int show_slave_lag_sla_misses(THD *thd, SHOW_VAR *var, char *buff)
{
  if (active_mi && active_mi->rli)
  {
    var->type= SHOW_LONGLONG;
    var->value= buff;
    *((longlong *)buff)= slave_lag_sla_misses.load();
  }
  else
    var->type= SHOW_UNDEF;
  return 0;
}

static int show_last_acked_binlog_pos(THD *thd, SHOW_VAR *var, char *buff)
{
  var->type= SHOW_UNDEF;
  if ((rpl_semi_sync_master_enabled || enable_raft_plugin) &&
      rpl_wait_for_semi_sync_ack)
  {
    const auto coord= last_acked.load();
    var->type= SHOW_CHAR;
    var->value= buff;
    char full_name[FN_REFLEN];
    full_name[0]= '\0';
    if (coord.file_num)
      sprintf(full_name, "%s.%06u", opt_bin_logname, coord.file_num);
    sprintf(buff, "%s:%u", full_name, coord.pos);
  }
  return 0;
}

static int show_slave_last_heartbeat(THD *thd, SHOW_VAR *var, char *buff)
{
  MYSQL_TIME received_heartbeat_time;
  if (active_mi)
  {
    var->type= SHOW_CHAR;
    var->value= buff;
    if (active_mi->last_heartbeat == 0)
      buff[0]='\0';
    else
    {
      thd->variables.time_zone->gmt_sec_to_TIME(&received_heartbeat_time,
        active_mi->last_heartbeat);
      my_datetime_to_str(&received_heartbeat_time, buff, 0);
    }
  }
  else
    var->type= SHOW_UNDEF;
  return 0;
}

static int show_heartbeat_period(THD *thd, SHOW_VAR *var, char *buff)
{
  DEBUG_SYNC(thd, "dsync_show_heartbeat_period");
  if (active_mi)
  {
    var->type= SHOW_CHAR;
    var->value= buff;
    sprintf(buff, "%.3f", active_mi->heartbeat_period);
  }
  else
    var->type= SHOW_UNDEF;
  return 0;
}

#ifndef DBUG_OFF
static int show_slave_rows_last_search_algorithm_used(THD *thd, SHOW_VAR *var, char *buff)
{
  uint res= slave_rows_last_search_algorithm_used;
  const char* s= ((res == Rows_log_event::ROW_LOOKUP_TABLE_SCAN) ? "TABLE_SCAN" :
                  ((res == Rows_log_event::ROW_LOOKUP_HASH_SCAN) ? "HASH_SCAN" :
                   "INDEX_SCAN"));

  var->type= SHOW_CHAR;
  var->value= buff;
  sprintf(buff, "%s", s);

  return 0;
}
#endif

#endif /* HAVE_REPLICATION */

static int show_open_tables(THD *thd, SHOW_VAR *var, char *buff)
{
  var->type= SHOW_LONG;
  var->value= buff;
  *((long *)buff)= (long)table_cache_manager.cached_tables();
  return 0;
}

static int show_prepared_stmt_count(THD *thd, SHOW_VAR *var, char *buff)
{
  var->type= SHOW_LONG;
  var->value= buff;
  mysql_mutex_lock(&LOCK_prepared_stmt_count);
  *((long *)buff)= (long)prepared_stmt_count;
  mysql_mutex_unlock(&LOCK_prepared_stmt_count);
  return 0;
}

static int show_table_definitions(THD *thd, SHOW_VAR *var, char *buff)
{
  var->type= SHOW_LONG;
  var->value= buff;
  *((long *)buff)= (long)cached_table_definitions();
  return 0;
}

static int show_latency_histogram_binlog_fsync(THD *thd, SHOW_VAR *var,
                                               char *buff)
{
  size_t i;
  for (i = 0; i < NUMBER_OF_HISTOGRAM_BINS; ++i)
    histogram_binlog_fsync_values[i] =
      latency_histogram_get_count(&histogram_binlog_fsync,i);

  prepare_latency_histogram_vars(&histogram_binlog_fsync,
                                 latency_histogram_binlog_fsync,
                                 histogram_binlog_fsync_values);
  var->type= SHOW_ARRAY;
  var->value = (char*) &latency_histogram_binlog_fsync;
  return 0;
}

static int show_histogram_binlog_group_commit(THD *thd, SHOW_VAR* var,
                                              char *buff)
{
  for (int i = 0; i < NUMBER_OF_COUNTER_HISTOGRAM_BINS; ++i)
  {
    histogram_binlog_group_commit_values[i] =
      (histogram_binlog_group_commit.count_per_bin)[i];
  }
  prepare_counter_histogram_vars(&histogram_binlog_group_commit,
                                 histogram_binlog_group_commit_var,
                                 histogram_binlog_group_commit_values);
  var->type = SHOW_ARRAY;
  var->value = (char*) &histogram_binlog_group_commit_var;
  return 0;
}
#if defined(HAVE_OPENSSL) && !defined(EMBEDDED_LIBRARY)
/* Functions relying on CTX */
static int show_ssl_ctx_sess_accept(THD *thd, SHOW_VAR *var, char *buff)
{
  var->type= SHOW_LONG;
  var->value= buff;
  *((long *)buff)= (!ssl_acceptor_fd ? 0 :
                     SSL_CTX_sess_accept(ssl_acceptor_fd->ssl_context));
  return 0;
}

static int show_ssl_ctx_sess_accept_good(THD *thd, SHOW_VAR *var, char *buff)
{
  var->type= SHOW_LONG;
  var->value= buff;
  *((long *)buff)= (!ssl_acceptor_fd ? 0 :
                     SSL_CTX_sess_accept_good(ssl_acceptor_fd->ssl_context));
  return 0;
}

static int show_ssl_ctx_sess_connect_good(THD *thd, SHOW_VAR *var, char *buff)
{
  var->type= SHOW_LONG;
  var->value= buff;
  *((long *)buff)= (!ssl_acceptor_fd ? 0 :
                     SSL_CTX_sess_connect_good(ssl_acceptor_fd->ssl_context));
  return 0;
}

static int show_ssl_ctx_sess_accept_renegotiate(THD *thd, SHOW_VAR *var, char *buff)
{
  var->type= SHOW_LONG;
  var->value= buff;
  *((long *)buff)= (!ssl_acceptor_fd ? 0 :
                     SSL_CTX_sess_accept_renegotiate(ssl_acceptor_fd->ssl_context));
  return 0;
}

static int show_ssl_ctx_sess_connect_renegotiate(THD *thd, SHOW_VAR *var, char *buff)
{
  var->type= SHOW_LONG;
  var->value= buff;
  *((long *)buff)= (!ssl_acceptor_fd ? 0 :
                     SSL_CTX_sess_connect_renegotiate(ssl_acceptor_fd->ssl_context));
  return 0;
}

static int show_ssl_ctx_sess_cb_hits(THD *thd, SHOW_VAR *var, char *buff)
{
  var->type= SHOW_LONG;
  var->value= buff;
  *((long *)buff)= (!ssl_acceptor_fd ? 0 :
                     SSL_CTX_sess_cb_hits(ssl_acceptor_fd->ssl_context));
  return 0;
}

static int show_ssl_ctx_sess_hits(THD *thd, SHOW_VAR *var, char *buff)
{
  var->type= SHOW_LONG;
  var->value= buff;
  *((long *)buff)= (!ssl_acceptor_fd ? 0 :
                     SSL_CTX_sess_hits(ssl_acceptor_fd->ssl_context));
  return 0;
}

static int show_ssl_ctx_sess_cache_full(THD *thd, SHOW_VAR *var, char *buff)
{
  var->type= SHOW_LONG;
  var->value= buff;
  *((long *)buff)= (!ssl_acceptor_fd ? 0 :
                     SSL_CTX_sess_cache_full(ssl_acceptor_fd->ssl_context));
  return 0;
}

static int show_ssl_ctx_sess_misses(THD *thd, SHOW_VAR *var, char *buff)
{
  var->type= SHOW_LONG;
  var->value= buff;
  *((long *)buff)= (!ssl_acceptor_fd ? 0 :
                     SSL_CTX_sess_misses(ssl_acceptor_fd->ssl_context));
  return 0;
}

static int show_ssl_ctx_sess_timeouts(THD *thd, SHOW_VAR *var, char *buff)
{
  var->type= SHOW_LONG;
  var->value= buff;
  *((long *)buff)= (!ssl_acceptor_fd ? 0 :
                     SSL_CTX_sess_timeouts(ssl_acceptor_fd->ssl_context));
  return 0;
}

static int show_ssl_ctx_sess_number(THD *thd, SHOW_VAR *var, char *buff)
{
  var->type= SHOW_LONG;
  var->value= buff;
  *((long *)buff)= (!ssl_acceptor_fd ? 0 :
                     SSL_CTX_sess_number(ssl_acceptor_fd->ssl_context));
  return 0;
}

static int show_ssl_ctx_sess_connect(THD *thd, SHOW_VAR *var, char *buff)
{
  var->type= SHOW_LONG;
  var->value= buff;
  *((long *)buff)= (!ssl_acceptor_fd ? 0 :
                     SSL_CTX_sess_connect(ssl_acceptor_fd->ssl_context));
  return 0;
}

static int show_ssl_ctx_sess_get_cache_size(THD *thd, SHOW_VAR *var, char *buff)
{
  var->type= SHOW_LONG;
  var->value= buff;
  *((long *)buff)= (!ssl_acceptor_fd ? 0 :
                     SSL_CTX_sess_get_cache_size(ssl_acceptor_fd->ssl_context));
  return 0;
}

static int show_ssl_ctx_get_verify_mode(THD *thd, SHOW_VAR *var, char *buff)
{
  var->type= SHOW_LONG;
  var->value= buff;
  *((long *)buff)= (!ssl_acceptor_fd ? 0 :
                     SSL_CTX_get_verify_mode(ssl_acceptor_fd->ssl_context));
  return 0;
}

static int show_ssl_ctx_get_verify_depth(THD *thd, SHOW_VAR *var, char *buff)
{
  var->type= SHOW_LONG;
  var->value= buff;
  *((long *)buff)= (!ssl_acceptor_fd ? 0 :
                     SSL_CTX_get_verify_depth(ssl_acceptor_fd->ssl_context));
  return 0;
}

static int show_ssl_ctx_get_session_cache_mode(THD *thd, SHOW_VAR *var, char *buff)
{
  var->type= SHOW_CHAR;
  if (!ssl_acceptor_fd)
    var->value= const_cast<char*>("NONE");
  else
    switch (SSL_CTX_get_session_cache_mode(ssl_acceptor_fd->ssl_context))
    {
    case SSL_SESS_CACHE_OFF:
      var->value= const_cast<char*>("OFF"); break;
    case SSL_SESS_CACHE_CLIENT:
      var->value= const_cast<char*>("CLIENT"); break;
    case SSL_SESS_CACHE_SERVER:
      var->value= const_cast<char*>("SERVER"); break;
    case SSL_SESS_CACHE_BOTH:
      var->value= const_cast<char*>("BOTH"); break;
    case SSL_SESS_CACHE_NO_AUTO_CLEAR:
      var->value= const_cast<char*>("NO_AUTO_CLEAR"); break;
    case SSL_SESS_CACHE_NO_INTERNAL_LOOKUP:
      var->value= const_cast<char*>("NO_INTERNAL_LOOKUP"); break;
    default:
      var->value= const_cast<char*>("Unknown"); break;
    }
  return 0;
}

/*
   Functions relying on SSL
   Note: In the show_ssl_* functions, we need to check if we have a
         valid vio-object since this isn't always true, specifically
         when session_status or global_status is requested from
         inside an Event.
 */
static int show_ssl_get_version(THD *thd, SHOW_VAR *var, char *buff)
{
  const NET* net = thd->get_net();
  var->type= SHOW_CHAR;
  if( thd->vio_ok() && net->vio->ssl_arg )
    var->value= const_cast<char*>(SSL_get_version((SSL*) net->vio->ssl_arg));
  else
    var->value= (char *)"";
  return 0;
}

static int show_ssl_session_reused(THD *thd, SHOW_VAR *var, char *buff)
{
  const NET* net = thd->get_net();
  var->type= SHOW_LONG;
  var->value= buff;
  if( thd->vio_ok() && net->vio->ssl_arg )
    *((long *)buff)= (long)SSL_session_reused((SSL*) net->vio->ssl_arg);
  else
    *((long *)buff)= 0;
  return 0;
}

static int show_ssl_get_default_timeout(THD *thd, SHOW_VAR *var, char *buff)
{
  const NET* net = thd->get_net();
  var->type= SHOW_LONG;
  var->value= buff;
  if( thd->vio_ok() && net->vio->ssl_arg )
    *((long *)buff)= (long)SSL_get_default_timeout((SSL*)net->vio->ssl_arg);
  else
    *((long *)buff)= 0;
  return 0;
}

static int show_ssl_get_verify_mode(THD *thd, SHOW_VAR *var, char *buff)
{
  const NET* net = thd->get_net();
  var->type= SHOW_LONG;
  var->value= buff;
  if( net->vio && net->vio->ssl_arg )
    *((long *)buff)= (long)SSL_get_verify_mode((SSL*)net->vio->ssl_arg);
  else
    *((long *)buff)= 0;
  return 0;
}

static int show_ssl_get_verify_depth(THD *thd, SHOW_VAR *var, char *buff)
{
  const NET* net = thd->get_net();
  var->type= SHOW_LONG;
  var->value= buff;
  if( thd->vio_ok() && net->vio->ssl_arg )
    *((long *)buff)= (long)SSL_get_verify_depth((SSL*)net->vio->ssl_arg);
  else
    *((long *)buff)= 0;
  return 0;
}

static int show_ssl_get_cipher(THD *thd, SHOW_VAR *var, char *buff)
{
  const NET* net = thd->get_net();
  var->type= SHOW_CHAR;
  if( thd->vio_ok() && net->vio->ssl_arg )
    var->value= const_cast<char*>(SSL_get_cipher((SSL*) net->vio->ssl_arg));
  else
    var->value= (char *)"";
  return 0;
}

static int show_ssl_get_cipher_list(THD *thd, SHOW_VAR *var, char *buff)
{
  const NET* net = thd->get_net();
  var->type= SHOW_CHAR;
  var->value= buff;
  if (thd->vio_ok() && net->vio->ssl_arg)
  {
    int i;
    const char *p;
    char *end= buff + SHOW_VAR_FUNC_BUFF_SIZE;
    for (i=0; (p= SSL_get_cipher_list((SSL*) net->vio->ssl_arg,i)) &&
               buff < end; i++)
    {
      buff= strnmov(buff, p, end-buff-1);
      *buff++= ':';
    }
    if (i)
      buff--;
  }
  *buff=0;
  return 0;
}


#ifdef HAVE_YASSL

static char *
my_asn1_time_to_string(ASN1_TIME *time, char *buf, size_t len)
{
  return yaSSL_ASN1_TIME_to_string(time, buf, len);
}

#else /* openssl */

static char *
my_asn1_time_to_string(ASN1_TIME *time, char *buf, size_t len)
{
  int n_read;
  char *res= NULL;
  BIO *bio= BIO_new(BIO_s_mem());

  if (bio == NULL)
    return NULL;

  if (!ASN1_TIME_print(bio, time))
    goto end;

  n_read= BIO_read(bio, buf, (int) (len - 1));

  if (n_read > 0)
  {
    buf[n_read]= 0;
    res= buf;
  }

end:
  BIO_free(bio);
  return res;
}

#endif

#if OPENSSL_VERSION_NUMBER < 0x10100000L
#define X509_get0_notBefore(x) X509_get_notBefore(x)
#define X509_get0_notAfter(x) X509_get_notAfter(x)
#endif

/**
  Handler function for the 'ssl_get_server_not_before' variable

  @param      thd  the mysql thread structure
  @param      var  the data for the variable
  @param[out] buf  the string to put the value of the variable into

  @return          status
  @retval     0    success
*/

static int
show_ssl_get_server_not_before(THD *thd, SHOW_VAR *var, char *buff)
{
  const NET* net = thd->get_net();
  var->type= SHOW_CHAR;
  if(thd->vio_ok() && net->vio->ssl_arg)
  {
    SSL *ssl= (SSL*) net->vio->ssl_arg;
    X509 *cert= SSL_get_certificate(ssl);
    ASN1_TIME *not_before= (ASN1_TIME *)X509_get0_notBefore(cert);

    var->value= my_asn1_time_to_string(not_before, buff,
                                       SHOW_VAR_FUNC_BUFF_SIZE);
    if (!var->value)
      return 1;
    var->value= buff;
  }
  else
    var->value= empty_c_string;
  return 0;
}


/**
  Handler function for the 'ssl_get_server_not_after' variable

  @param      thd  the mysql thread structure
  @param      var  the data for the variable
  @param[out] buf  the string to put the value of the variable into

  @return          status
  @retval     0    success
*/

static int
show_ssl_get_server_not_after(THD *thd, SHOW_VAR *var, char *buff)
{
  const NET* net = thd->get_net();
  var->type= SHOW_CHAR;
  if(thd->vio_ok() && net->vio->ssl_arg)
  {
    SSL *ssl= (SSL*) net->vio->ssl_arg;
    X509 *cert= SSL_get_certificate(ssl);
    ASN1_TIME *not_after= (ASN1_TIME *)X509_get0_notAfter(cert);

    var->value= my_asn1_time_to_string(not_after, buff,
                                       SHOW_VAR_FUNC_BUFF_SIZE);
    if (!var->value)
      return 1;
  }
  else
    var->value= empty_c_string;
  return 0;
}

#endif /* HAVE_OPENSSL && !EMBEDDED_LIBRARY */


static int get_db_ac_total_aborted_queries(THD *thd, SHOW_VAR *var,
                                           char *buff) {
  var->type = SHOW_LONGLONG;
  var->value = buff;
  *((longlong *)buff) = db_ac->get_total_aborted_queries();
  return 0;
}

static int get_db_ac_total_running_queries(THD *thd, SHOW_VAR *var,
                                           char *buff) {
  var->type = SHOW_LONG;
  var->value = buff;
  *((long *)buff) = db_ac->get_total_running_queries();
  return 0;
}

static int get_db_ac_total_timeout_queries(THD *thd, SHOW_VAR *var,
                                           char *buff) {
  var->type = SHOW_LONGLONG;
  var->value = buff;
  *((longlong *)buff) = db_ac->get_total_timeout_queries();
  return 0;
}

static int get_db_ac_total_waiting_queries(THD *thd, SHOW_VAR *var,
                                           char *buff) {
  var->type = SHOW_LONG;
  var->value = buff;
  *((long *)buff) = db_ac->get_total_waiting_queries();
  return 0;
}

static int get_db_ac_total_rejected_connections(THD *thd, SHOW_VAR *var,
                                                char *buff) {
  var->type = SHOW_LONGLONG;
  var->value = buff;
  *((longlong *)buff) = db_ac->get_total_rejected_connections();
  return 0;
}

static int show_peak_with_reset(THD *thd, SHOW_VAR *var, char *buff,
                                ulonglong &peak, ulonglong &usage)
{
  var->type = SHOW_LONGLONG;
  var->value = buff;
  if (thd->variables.reset_period_status_vars)
  {
    /* First, reset to 0 not to miss updates to usage between reading it
       and setting the peak. If update is missed then peak could be smaller
       than current usage. */
    ulonglong old_peak = my_atomic_fas64((int64 *)&peak, 0);
    *reinterpret_cast<ulonglong *>(buff) = old_peak;

    /* Now update peak with latest usage, possibly racing with others. */
    old_peak = 0;
    ulonglong new_value = my_atomic_load64((int64 *)&usage);
    while (old_peak < new_value)
    {
      /* If Compare-And-Swap is unsuccessful then old_peak is updated. */
      if (my_atomic_cas64((int64 *)&peak, (int64 *)&old_peak, new_value))
        break;
    }
  }
  else
  {
    *reinterpret_cast<ulonglong *>(buff) = my_atomic_load64((int64 *)&peak);
  }

  return 0;
}

static int show_filesort_disk_usage_period_peak(THD *thd, SHOW_VAR *var,
                                                char *buff)
{
  return show_peak_with_reset(thd, var, buff, filesort_disk_usage_period_peak,
                              global_status_var.filesort_disk_usage);
}

static int show_tmp_table_disk_usage_period_peak(THD *thd, SHOW_VAR *var,
                                                 char *buff)
{
  return show_peak_with_reset(thd, var, buff, tmp_table_disk_usage_period_peak,
                              global_status_var.tmp_table_disk_usage);
}

/*
  Variables shown by SHOW STATUS in alphabetical order
*/

SHOW_VAR status_vars[]= {
  {"acl_cache_miss",           (char*) &acl_cache_miss,         SHOW_LONG},
  {"acl_fast_lookup_enabled",  (char*) &acl_fast_lookup_enabled,SHOW_BOOL},
  {"acl_fast_lookup_miss",     (char*) &acl_fast_lookup_miss,   SHOW_LONG},
  {"Aborted_clients",          (char*) &aborted_threads,        SHOW_LONG},
  {"Aborted_connects",         (char*) &aborted_connects,       SHOW_LONG},
  {"Last_evicted_page_age",    (char*) &last_evicted_page_age,  SHOW_LONG},
  {"Binlog_bytes_written",     (char*) &binlog_bytes_written,   SHOW_LONGLONG},
  {"Binlog_cache_disk_use",    (char*) &binlog_cache_disk_use,  SHOW_LONG},
  {"Binlog_cache_use",         (char*) &binlog_cache_use,       SHOW_LONG},
  {"Binlog_fsync_count",       (char*) &binlog_fsync_count, SHOW_LONGLONG},
  {"Binlog_stmt_cache_disk_use",(char*) &binlog_stmt_cache_disk_use,  SHOW_LONG},
  {"Binlog_stmt_cache_use",    (char*) &binlog_stmt_cache_use,       SHOW_LONG},
  {"Bytes_received",           (char*) offsetof(STATUS_VAR, bytes_received), SHOW_LONGLONG_STATUS},
  {"Bytes_sent",               (char*) offsetof(STATUS_VAR, bytes_sent), SHOW_LONGLONG_STATUS},
  {"Com",                      (char*) com_status_vars, SHOW_ARRAY},
  {"Command_seconds",          (char*) offsetof(STATUS_VAR, command_time), SHOW_TIMER_STATUS},
  {"Command_slave_seconds",    (char*) &command_slave_seconds,  SHOW_TIMER},
  {"Compression",              (char*) &show_net_compression, SHOW_FUNC},
  {"Compression_context_reset", (char*) &compress_ctx_reset, SHOW_LONGLONG},
  {"Compression_input_bytes",  (char*) &compress_input_bytes, SHOW_LONGLONG},
  {"Compression_output_bytes", (char*) &compress_output_bytes, SHOW_LONGLONG},
  {"Connections",              (char*) &total_thread_ids,              SHOW_LONG_NOFLUSH},
  {"Connection_errors_accept", (char*) &connection_errors_accept, SHOW_LONG},
  {"Connection_errors_internal", (char*) &connection_errors_internal, SHOW_LONG},
  {"Connection_errors_max_connections", (char*) &connection_errors_max_connection, SHOW_LONG},
  {"Connection_errors_max_connections_abort", (char*) &connection_errors_max_connection_abort, SHOW_LONG},
  {"Connection_errors_net_ER_NET_ERROR_ON_WRITE", (char*) &connection_errors_net_ER_NET_ERROR_ON_WRITE, SHOW_LONG},
  {"Connection_errors_net_ER_NET_PACKETS_OUT_OF_ORDER", (char*) &connection_errors_net_ER_NET_PACKETS_OUT_OF_ORDER, SHOW_LONG},
  {"Connection_errors_net_ER_NET_PACKET_TOO_LARGE", (char*) &connection_errors_net_ER_NET_PACKET_TOO_LARGE, SHOW_LONG},
  {"Connection_errors_net_ER_NET_READ_ERROR", (char*) &connection_errors_net_ER_NET_READ_ERROR, SHOW_LONG},
  {"Connection_errors_net_ER_NET_READ_INTERRUPTED", (char*) &connection_errors_net_ER_NET_READ_INTERRUPTED, SHOW_LONG},
  {"Connection_errors_net_ER_NET_UNCOMPRESS_ERROR", (char*) &connection_errors_net_ER_NET_UNCOMPRESS_ERROR, SHOW_LONG},
  {"Connection_errors_net_ER_NET_WRITE_INTERRUPTED", (char*) &connection_errors_net_ER_NET_WRITE_INTERRUPTED, SHOW_LONG},
  {"Connection_errors_peer_address", (char*) &connection_errors_peer_addr, SHOW_LONG},
  {"Connection_errors_select", (char*) &connection_errors_select, SHOW_LONG},
  {"Connection_errors_tcpwrap", (char*) &connection_errors_tcpwrap, SHOW_LONG},
  {"Connection_errors_host_blocked", (char*) &connection_errors_host_blocked, SHOW_LONG},
  {"Connection_errors_host_not_privileged", (char*) &connection_errors_host_not_privileged, SHOW_LONG},
  {"Connection_errors_acl_auth", (char*) &connection_errors_acl_auth, SHOW_LONG},
  {"Connection_errors_out_of_resources", (char*) &connection_errors_out_of_resources, SHOW_LONG},
  {"Connection_errors_ssl_check", (char*) &connection_errors_ssl_check, SHOW_LONG},
  {"Connection_errors_auth_plugin", (char*) &connection_errors_auth_plugin, SHOW_LONG},
  {"Connection_errors_auth", (char*) &connection_errors_auth, SHOW_LONG},
  {"Connection_errors_handshake", (char*) &connection_errors_handshake, SHOW_LONG},
  {"Connection_errors_proxy_user", (char*) &connection_errors_proxy_user, SHOW_LONG},
  {"Connection_errors_multi_tenancy_max_global", (char*) &connection_errors_multi_tenancy_max_global, SHOW_LONG},
  {"Connection_errors_password_expired", (char*) &connection_errors_password_expired, SHOW_LONG},
  {"Connection_errors_user_conn", (char*) &connection_errors_user_conn, SHOW_LONG},
  {"Connection_errors_admin_conn_denied", (char*) &connection_errors_admin_conn_denied, SHOW_LONG},
  {"Connection_errors_max_user_connection", (char*) &connection_errors_max_user_connection, SHOW_LONG},
  {"Connection_errors_access_denied", (char*) &connection_errors_access_denied, SHOW_LONG},
  {"Created_tmp_disk_tables",  (char*) offsetof(STATUS_VAR, created_tmp_disk_tables), SHOW_LONGLONG_STATUS},
  {"Created_tmp_files",        (char*) &my_tmp_file_created, SHOW_LONG},
  {"Created_tmp_tables",       (char*) offsetof(STATUS_VAR, created_tmp_tables), SHOW_LONGLONG_STATUS},
  {"Database_admission_control_aborted_queries", (char*) &get_db_ac_total_aborted_queries, SHOW_FUNC},
  {"Database_admission_control_running_queries", (char*) &get_db_ac_total_running_queries, SHOW_FUNC},
  {"Database_admission_control_timeout_queries", (char*) &get_db_ac_total_timeout_queries, SHOW_FUNC},
  {"Database_admission_control_waiting_queries", (char*) &get_db_ac_total_waiting_queries, SHOW_FUNC},
  {"Database_admission_control_rejected_connections", (char*) &get_db_ac_total_rejected_connections, SHOW_FUNC},
  {"Delayed_errors",           (char*) &delayed_insert_errors,  SHOW_LONG},
  {"Delayed_insert_threads",   (char*) &delayed_insert_threads, SHOW_LONG_NOFLUSH},
  {"Delayed_writes",           (char*) &delayed_insert_writes,  SHOW_LONG},
  {"Exec_seconds",             (char*) offsetof(STATUS_VAR, exec_time), SHOW_TIMER_STATUS},
  {"Filesort_disk_usage",      (char*) offsetof(STATUS_VAR, filesort_disk_usage), SHOW_LONGLONG_STATUS},
  {"Filesort_disk_usage_peak", (char*) offsetof(STATUS_VAR, filesort_disk_usage_peak), SHOW_LONGLONG_STATUS},
  {"Filesort_disk_usage_period_peak", (char*) show_filesort_disk_usage_period_peak, SHOW_FUNC},
  {"Flashcache_enabled",       (char*) &cachedev_enabled,       SHOW_BOOL },
  {"Flush_commands",           (char*) &refresh_version,        SHOW_LONG_NOFLUSH},
  {"git_hash",                 (char*) git_hash, SHOW_CHAR },
  {"git_date",                 (char*) git_date, SHOW_CHAR },
  {"super_read_only_block_microsec",
                      (char*) &show_super_read_only_block_microsec, SHOW_FUNC},
  {"Handler_commit",           (char*) offsetof(STATUS_VAR, ha_commit_count), SHOW_LONGLONG_STATUS},
  {"Handler_delete",           (char*) offsetof(STATUS_VAR, ha_delete_count), SHOW_LONGLONG_STATUS},
  {"Handler_discover",         (char*) offsetof(STATUS_VAR, ha_discover_count), SHOW_LONGLONG_STATUS},
  {"Handler_external_lock",    (char*) offsetof(STATUS_VAR, ha_external_lock_count), SHOW_LONGLONG_STATUS},
  {"Handler_mrr_init",         (char*) offsetof(STATUS_VAR, ha_multi_range_read_init_count),  SHOW_LONGLONG_STATUS},
  {"Handler_prepare",          (char*) offsetof(STATUS_VAR, ha_prepare_count),  SHOW_LONGLONG_STATUS},
  {"Handler_read_first",       (char*) offsetof(STATUS_VAR, ha_read_first_count), SHOW_LONGLONG_STATUS},
  {"Handler_read_key",         (char*) offsetof(STATUS_VAR, ha_read_key_count), SHOW_LONGLONG_STATUS},
  {"Handler_read_last",        (char*) offsetof(STATUS_VAR, ha_read_last_count), SHOW_LONGLONG_STATUS},
  {"Handler_read_next",        (char*) offsetof(STATUS_VAR, ha_read_next_count), SHOW_LONGLONG_STATUS},
  {"Handler_read_prev",        (char*) offsetof(STATUS_VAR, ha_read_prev_count), SHOW_LONGLONG_STATUS},
  {"Handler_read_rnd",         (char*) offsetof(STATUS_VAR, ha_read_rnd_count), SHOW_LONGLONG_STATUS},
  {"Handler_read_rnd_next",    (char*) offsetof(STATUS_VAR, ha_read_rnd_next_count), SHOW_LONGLONG_STATUS},
  {"Handler_release_concurrency_slot", (char*) offsetof(STATUS_VAR, ha_release_concurrency_slot_count), SHOW_LONGLONG_STATUS},
  {"Handler_rollback",         (char*) offsetof(STATUS_VAR, ha_rollback_count), SHOW_LONGLONG_STATUS},
  {"Handler_savepoint",        (char*) offsetof(STATUS_VAR, ha_savepoint_count), SHOW_LONGLONG_STATUS},
  {"Handler_savepoint_rollback",(char*) offsetof(STATUS_VAR, ha_savepoint_rollback_count), SHOW_LONGLONG_STATUS},
  {"Handler_update",           (char*) offsetof(STATUS_VAR, ha_update_count), SHOW_LONGLONG_STATUS},
  {"Handler_write",            (char*) offsetof(STATUS_VAR, ha_write_count), SHOW_LONGLONG_STATUS},
#ifdef HAVE_JEMALLOC
#ifndef EMBEDDED_LIBRARY
  {"Jemalloc_arenas_narenas",  (char*) &show_jemalloc_arenas_narenas,   SHOW_FUNC},
  {"Jemalloc_opt_narenas",     (char*) &show_jemalloc_opt_narenas,      SHOW_FUNC},
  {"Jemalloc_opt_tcache",      (char*) &show_jemalloc_opt_tcache,       SHOW_FUNC},
  {"Jemalloc_stats_active",    (char*) &show_jemalloc_active,           SHOW_FUNC},
  {"Jemalloc_stats_allocated", (char*) &show_jemalloc_allocated,        SHOW_FUNC},
  {"Jemalloc_stats_mapped",    (char*) &show_jemalloc_mapped,           SHOW_FUNC},
  {"Jemalloc_stats_metadata",  (char*) &show_jemalloc_metadata,         SHOW_FUNC},
#endif
#endif
  {"Json_contains_count",      (char*) &json_contains_count,            SHOW_LONGLONG},
  {"Json_extract_count",       (char*) &json_extract_count,             SHOW_LONGLONG},
  {"Json_valid_count",         (char*) &json_valid_count,               SHOW_LONGLONG},
  {"Json_func_binary_count",   (char*) &json_func_binary_count,         SHOW_LONGLONG},
  {"Key_blocks_not_flushed",   (char*) offsetof(KEY_CACHE, global_blocks_changed), SHOW_KEY_CACHE_LONG},
  {"Key_blocks_unused",        (char*) offsetof(KEY_CACHE, blocks_unused), SHOW_KEY_CACHE_LONG},
  {"Key_blocks_used",          (char*) offsetof(KEY_CACHE, blocks_used), SHOW_KEY_CACHE_LONG},
  {"Key_read_requests",        (char*) offsetof(KEY_CACHE, global_cache_r_requests), SHOW_KEY_CACHE_LONGLONG},
  {"Key_reads",                (char*) offsetof(KEY_CACHE, global_cache_read), SHOW_KEY_CACHE_LONGLONG},
  {"Key_write_requests",       (char*) offsetof(KEY_CACHE, global_cache_w_requests), SHOW_KEY_CACHE_LONGLONG},
  {"Key_writes",               (char*) offsetof(KEY_CACHE, global_cache_write), SHOW_KEY_CACHE_LONGLONG},
  {"Last_query_cost",          (char*) offsetof(STATUS_VAR, last_query_cost), SHOW_DOUBLE_STATUS},
  {"Last_query_partial_plans", (char*) offsetof(STATUS_VAR, last_query_partial_plans), SHOW_LONGLONG_STATUS},
  {"Latency_histogram_binlog_fsync",
   (char*) &show_latency_histogram_binlog_fsync, SHOW_FUNC},
  {"histogram_binlog_group_commit",
   (char*) &show_histogram_binlog_group_commit, SHOW_FUNC},
  {"Max_used_connections",     (char*) &max_used_connections,  SHOW_LONG},
  {"Max_statement_time_exceeded",   (char*) offsetof(STATUS_VAR, max_statement_time_exceeded), SHOW_LONG_STATUS},
  {"Max_statement_time_set",        (char*) offsetof(STATUS_VAR, max_statement_time_set), SHOW_LONG_STATUS},
  {"Max_statement_time_set_failed", (char*) offsetof(STATUS_VAR, max_statement_time_set_failed), SHOW_LONG_STATUS},
  {"Non_super_connections",    (char*) &nonsuper_connections,   SHOW_INT},
  {"Not_flushed_delayed_rows", (char*) &delayed_rows_in_use,    SHOW_LONG_NOFLUSH},
  {"Object_stats_misses",      (char*) &object_stats_misses,    SHOW_LONGLONG},
  {"Open_files",               (char*) &my_file_opened,         SHOW_LONG_NOFLUSH},
  {"Open_streams",             (char*) &my_stream_opened,       SHOW_LONG_NOFLUSH},
  {"Open_table_definitions",   (char*) &show_table_definitions, SHOW_FUNC},
  {"Open_tables",              (char*) &show_open_tables,       SHOW_FUNC},
  {"Open_tables_seconds",      (char*) offsetof(STATUS_VAR, open_table_time), SHOW_TIMER_STATUS},
  {"Opened_files",             (char*) &my_file_total_opened, SHOW_LONG_NOFLUSH},
  {"Opened_tables",            (char*) offsetof(STATUS_VAR, opened_tables), SHOW_LONGLONG_STATUS},
  {"Opened_table_definitions", (char*) offsetof(STATUS_VAR, opened_shares), SHOW_LONGLONG_STATUS},
  {"Parse_seconds",            (char*) offsetof(STATUS_VAR, parse_time), SHOW_TIMER_STATUS},
  {"Pre_exec_seconds",         (char*) offsetof(STATUS_VAR, pre_exec_time), SHOW_TIMER_STATUS},
  {"Prepared_stmt_count",      (char*) &show_prepared_stmt_count, SHOW_FUNC},
#ifdef HAVE_QUERY_CACHE
  {"Qcache_free_blocks",       (char*) &query_cache.free_memory_blocks, SHOW_LONG_NOFLUSH},
  {"Qcache_free_memory",       (char*) &query_cache.free_memory, SHOW_LONG_NOFLUSH},
  {"Qcache_hits",              (char*) &query_cache.hits,       SHOW_LONG},
  {"Qcache_inserts",           (char*) &query_cache.inserts,    SHOW_LONG},
  {"Qcache_lowmem_prunes",     (char*) &query_cache.lowmem_prunes, SHOW_LONG},
  {"Qcache_not_cached",        (char*) &query_cache.refused,    SHOW_LONG},
  {"Qcache_queries_in_cache",  (char*) &query_cache.queries_in_cache, SHOW_LONG_NOFLUSH},
  {"Qcache_total_blocks",      (char*) &query_cache.total_blocks, SHOW_LONG_NOFLUSH},
#endif /*HAVE_QUERY_CACHE*/
  {"Queries",                  (char*) &show_queries,            SHOW_FUNC},
  {"Questions",                (char*) offsetof(STATUS_VAR, questions), SHOW_LONGLONG_STATUS},
  {"Rbr_unsafe_queries",       (char*) &rbr_unsafe_queries, SHOW_LONGLONG},
  {"Read_queries",             (char*) &read_queries,        SHOW_LONG},
  {"Read_requests",            (char*) offsetof(STATUS_VAR, read_requests), SHOW_LONG_STATUS},
  {"Read_seconds",             (char*) offsetof(STATUS_VAR, read_time), SHOW_TIMER_STATUS},
  {"Relay_log_bytes_written",  (char*) &relay_log_bytes_written, SHOW_LONGLONG},
  {"Relay_log_io_connected",   (char*) &relay_io_connected, SHOW_LONG},
  {"Relay_log_io_events",      (char*) &relay_io_events, SHOW_LONG},
  {"Relay_log_io_bytes",       (char*) &relay_io_bytes, SHOW_LONGLONG},
  {"Relay_log_sql_events",     (char*) &relay_sql_events, SHOW_LONG},
  {"Relay_log_sql_bytes",      (char*) &relay_sql_bytes, SHOW_LONGLONG},
  {"Relay_log_sql_wait_seconds", (char*) &relay_sql_wait_time, SHOW_TIMER},
  {"Rows_examined",            (char*) offsetof(STATUS_VAR, rows_examined), SHOW_LONG_STATUS},
  {"Rows_sent",                (char*) offsetof(STATUS_VAR, rows_sent), SHOW_LONG_STATUS},
#ifdef HAVE_REPLICATION
  {"Rpl_count_other",          (char*) &repl_event_count_other,                SHOW_LONGLONG},
  {"Rpl_count_unknown",        (char*) &repl_event_counts[UNKNOWN_EVENT],      SHOW_LONGLONG},
  {"Rpl_count_start_v3",       (char*) &repl_event_counts[START_EVENT_V3],     SHOW_LONGLONG},
  {"Rpl_count_query",          (char*) &repl_event_counts[QUERY_EVENT],        SHOW_LONGLONG},
  {"Rpl_count_stop",           (char*) &repl_event_counts[STOP_EVENT],         SHOW_LONGLONG},
  {"Rpl_count_rotate",         (char*) &repl_event_counts[ROTATE_EVENT],       SHOW_LONGLONG},
  {"Rpl_count_intvar",         (char*) &repl_event_counts[INTVAR_EVENT],       SHOW_LONGLONG},
  {"Rpl_count_load",           (char*) &repl_event_counts[LOAD_EVENT],         SHOW_LONGLONG},
  {"Rpl_count_metadata",       (char*) &repl_event_counts[METADATA_EVENT],     SHOW_LONGLONG},
  {"Rpl_count_create_file",    (char*) &repl_event_counts[CREATE_FILE_EVENT],  SHOW_LONGLONG},
  {"Rpl_count_append_block",   (char*) &repl_event_counts[APPEND_BLOCK_EVENT], SHOW_LONGLONG},
  {"Rpl_count_exec_load",      (char*) &repl_event_counts[EXEC_LOAD_EVENT],    SHOW_LONGLONG},
  {"Rpl_count_delete_file",    (char*) &repl_event_counts[DELETE_FILE_EVENT],  SHOW_LONGLONG},
  {"Rpl_count_new_load",       (char*) &repl_event_counts[NEW_LOAD_EVENT],     SHOW_LONGLONG},
  {"Rpl_count_rand",           (char*) &repl_event_counts[RAND_EVENT],         SHOW_LONGLONG},
  {"Rpl_count_user_var",       (char*) &repl_event_counts[USER_VAR_EVENT],     SHOW_LONGLONG},
  {"Rpl_count_format_description", (char*) &repl_event_counts[FORMAT_DESCRIPTION_EVENT],  SHOW_LONGLONG},
  {"Rpl_count_xid",            (char*) &repl_event_counts[XID_EVENT],          SHOW_LONGLONG},
  {"Rpl_count_begin_load_query",   (char*) &repl_event_counts[BEGIN_LOAD_QUERY_EVENT],    SHOW_LONGLONG},
  {"Rpl_count_execute_load_query", (char*) &repl_event_counts[EXECUTE_LOAD_QUERY_EVENT],  SHOW_LONGLONG},
  {"Rpl_count_table_map",      (char*) &repl_event_counts[TABLE_MAP_EVENT],    SHOW_LONGLONG},
  {"Rpl_count_pre_ga_write_rows",   (char*) &repl_event_counts[PRE_GA_WRITE_ROWS_EVENT],  SHOW_LONGLONG},
  {"Rpl_count_pre_ga_update_rows",  (char*) &repl_event_counts[PRE_GA_UPDATE_ROWS_EVENT], SHOW_LONGLONG},
  {"Rpl_count_pre_ga_delete_rows",  (char*) &repl_event_counts[PRE_GA_DELETE_ROWS_EVENT], SHOW_LONGLONG},
  {"Rpl_count_write_rows",     (char*) &repl_event_counts[WRITE_ROWS_EVENT],   SHOW_LONGLONG},
  {"Rpl_count_update_rows",    (char*) &repl_event_counts[UPDATE_ROWS_EVENT],  SHOW_LONGLONG},
  {"Rpl_count_delete_rows",    (char*) &repl_event_counts[DELETE_ROWS_EVENT],  SHOW_LONGLONG},
  {"Rpl_count_incident",       (char*) &repl_event_counts[INCIDENT_EVENT],     SHOW_LONGLONG},
  {"Rpl_seconds_other",        (char*) &repl_event_time_other,                 SHOW_TIMER},
  {"Rpl_seconds_unknown",      (char*) &repl_event_times[UNKNOWN_EVENT],       SHOW_TIMER},
  {"Rpl_seconds_start_v3",     (char*) &repl_event_times[START_EVENT_V3],      SHOW_TIMER},
  {"Rpl_seconds_query",        (char*) &repl_event_times[QUERY_EVENT],         SHOW_TIMER},
  {"Rpl_seconds_stop",         (char*) &repl_event_times[STOP_EVENT],          SHOW_TIMER},
  {"Rpl_seconds_rotate",       (char*) &repl_event_times[ROTATE_EVENT],        SHOW_TIMER},
  {"Rpl_seconds_intvar",       (char*) &repl_event_times[INTVAR_EVENT],        SHOW_TIMER},
  {"Rpl_seconds_load",         (char*) &repl_event_times[LOAD_EVENT],          SHOW_TIMER},
  {"Rpl_seconds_metadata",     (char*) &repl_event_times[METADATA_EVENT],      SHOW_TIMER},
  {"Rpl_seconds_create_file",  (char*) &repl_event_times[CREATE_FILE_EVENT],   SHOW_TIMER},
  {"Rpl_seconds_append_block", (char*) &repl_event_times[APPEND_BLOCK_EVENT],  SHOW_TIMER},
  {"Rpl_seconds_exec_load",    (char*) &repl_event_times[EXEC_LOAD_EVENT],     SHOW_TIMER},
  {"Rpl_seconds_delete_file",  (char*) &repl_event_times[DELETE_FILE_EVENT],   SHOW_TIMER},
  {"Rpl_seconds_new_load",     (char*) &repl_event_times[NEW_LOAD_EVENT],      SHOW_TIMER},
  {"Rpl_seconds_rand",         (char*) &repl_event_times[RAND_EVENT],          SHOW_TIMER},
  {"Rpl_seconds_user_var",     (char*) &repl_event_times[USER_VAR_EVENT],      SHOW_TIMER},
  {"Rpl_seconds_format_description", (char*) &repl_event_times[FORMAT_DESCRIPTION_EVENT], SHOW_TIMER},
  {"Rpl_seconds_xid",          (char*) &repl_event_times[XID_EVENT],           SHOW_TIMER},
  {"Rpl_seconds_begin_load_query",   (char*) &repl_event_times[BEGIN_LOAD_QUERY_EVENT],   SHOW_TIMER},
  {"Rpl_seconds_execute_load_query", (char*) &repl_event_times[EXECUTE_LOAD_QUERY_EVENT], SHOW_TIMER},
  {"Rpl_seconds_table_map",    (char*) &repl_event_times[TABLE_MAP_EVENT],     SHOW_TIMER},
  {"Rpl_seconds_pre_ga_write_rows",  (char*) &repl_event_times[PRE_GA_WRITE_ROWS_EVENT],  SHOW_TIMER},
  {"Rpl_seconds_pre_ga_update_rows", (char*) &repl_event_times[PRE_GA_UPDATE_ROWS_EVENT], SHOW_TIMER},
  {"Rpl_seconds_pre_ga_delete_rows", (char*) &repl_event_times[PRE_GA_DELETE_ROWS_EVENT], SHOW_TIMER},
  {"Rpl_seconds_write_rows",   (char*) &repl_event_times[WRITE_ROWS_EVENT],    SHOW_TIMER},
  {"Rpl_seconds_update_rows",  (char*) &repl_event_times[UPDATE_ROWS_EVENT],   SHOW_TIMER},
  {"Rpl_seconds_delete_rows",  (char*) &repl_event_times[DELETE_ROWS_EVENT],   SHOW_TIMER},
  {"Rpl_seconds_incident",     (char*) &repl_event_times[INCIDENT_EVENT],      SHOW_TIMER},
  {"Compressed_event_cache_hit_ratio", (char*) &comp_event_cache_hit_ratio, SHOW_DOUBLE},
  {"Rpl_semi_sync_master_ack_waits",  (char*) &repl_semi_sync_master_ack_waits, SHOW_LONGLONG},
  {"Rpl_last_semi_sync_acked_pos", (char*) &show_last_acked_binlog_pos,
    SHOW_FUNC},
#endif
  {"rocksdb_git_hash",         (char*) rocksdb_git_hash, SHOW_CHAR },
  {"rocksdb_git_date",         (char*) rocksdb_git_date, SHOW_CHAR },
  {"Select_full_join",         (char*) offsetof(STATUS_VAR, select_full_join_count), SHOW_LONGLONG_STATUS},
  {"Select_full_range_join",   (char*) offsetof(STATUS_VAR, select_full_range_join_count), SHOW_LONGLONG_STATUS},
  {"Select_range",             (char*) offsetof(STATUS_VAR, select_range_count), SHOW_LONGLONG_STATUS},
  {"Select_range_check",       (char*) offsetof(STATUS_VAR, select_range_check_count), SHOW_LONGLONG_STATUS},
  {"Select_scan",	       (char*) offsetof(STATUS_VAR, select_scan_count), SHOW_LONGLONG_STATUS},
#ifdef HAVE_REPLICATION
  {"Skip_unique_check",        (char*) &show_skip_unique_check, SHOW_FUNC},
#endif
  {"Slave_open_temp_tables",   (char*) &slave_open_temp_tables, SHOW_INT},
#ifdef HAVE_REPLICATION
  {"Slave_retried_transactions",(char*) &show_slave_retried_trans, SHOW_FUNC},
  {"Slave_heartbeat_period",   (char*) &show_heartbeat_period, SHOW_FUNC},
  {"Slave_received_heartbeats",(char*) &show_slave_received_heartbeats, SHOW_FUNC},
  {"Slave_lag_sla_misses",     (char*) &show_slave_lag_sla_misses, SHOW_FUNC},
  {"Slave_last_heartbeat",     (char*) &show_slave_last_heartbeat, SHOW_FUNC},
#ifndef DBUG_OFF
  {"Slave_rows_last_search_algorithm_used",(char*) &show_slave_rows_last_search_algorithm_used, SHOW_FUNC},
#endif
  {"Slave_running",            (char*) &show_slave_running,     SHOW_FUNC},
  {"Slave_dependency_in_queue",  (char*) &show_slave_dependency_in_queue, SHOW_FUNC},
  {"Slave_dependency_in_flight", (char*) &show_slave_dependency_in_flight, SHOW_FUNC},
  {"Slave_dependency_begin_waits", (char*) &show_slave_dependency_begin_waits, SHOW_FUNC},
  {"Slave_dependency_next_waits", (char*) &show_slave_dependency_next_waits, SHOW_FUNC},
  {"Slave_before_image_inconsistencies", (char*) &show_slave_before_image_inconsistencies, SHOW_FUNC},
	{"Slave_high_priority_ddl_executed", (char *)&slave_high_priority_ddl_executed, SHOW_LONGLONG},
	{"Slave_high_priority_ddl_killed_connections", (char *)&slave_high_priority_ddl_killed_connections, SHOW_LONGLONG},
#endif
  {"Slow_launch_threads",      (char*) &slow_launch_threads,    SHOW_LONG},
  {"Slow_queries",             (char*) offsetof(STATUS_VAR, long_query_count), SHOW_LONGLONG_STATUS},
  {"Sort_merge_passes",        (char*) offsetof(STATUS_VAR, filesort_merge_passes), SHOW_LONGLONG_STATUS},
  {"Sort_range",               (char*) offsetof(STATUS_VAR, filesort_range_count), SHOW_LONGLONG_STATUS},
  {"Sort_rows",                (char*) offsetof(STATUS_VAR, filesort_rows), SHOW_LONGLONG_STATUS},
  {"Sort_scan",                (char*) offsetof(STATUS_VAR, filesort_scan_count), SHOW_LONGLONG_STATUS},
  {"sql_stats_size",           (char*) &sql_stats_size, SHOW_LONGLONG},
  {"sql_stats_snapshot",       (char*) &sql_stats_snapshot_status, SHOW_BOOL},
#ifdef HAVE_OPENSSL
#ifndef EMBEDDED_LIBRARY
  {"Ssl_accept_renegotiates",  (char*) &show_ssl_ctx_sess_accept_renegotiate, SHOW_FUNC},
  {"Ssl_accepts",              (char*) &show_ssl_ctx_sess_accept, SHOW_FUNC},
  {"Ssl_callback_cache_hits",  (char*) &show_ssl_ctx_sess_cb_hits, SHOW_FUNC},
  {"Ssl_cipher",               (char*) &show_ssl_get_cipher, SHOW_FUNC},
  {"Ssl_cipher_list",          (char*) &show_ssl_get_cipher_list, SHOW_FUNC},
  {"Ssl_client_connects",      (char*) &show_ssl_ctx_sess_connect, SHOW_FUNC},
  {"Ssl_connect_renegotiates", (char*) &show_ssl_ctx_sess_connect_renegotiate, SHOW_FUNC},
  {"Ssl_ctx_verify_depth",     (char*) &show_ssl_ctx_get_verify_depth, SHOW_FUNC},
  {"Ssl_ctx_verify_mode",      (char*) &show_ssl_ctx_get_verify_mode, SHOW_FUNC},
  {"Ssl_default_timeout",      (char*) &show_ssl_get_default_timeout, SHOW_FUNC},
  {"Ssl_finished_accepts",     (char*) &show_ssl_ctx_sess_accept_good, SHOW_FUNC},
  {"Ssl_finished_connects",    (char*) &show_ssl_ctx_sess_connect_good, SHOW_FUNC},
  {"Ssl_session_cache_hits",   (char*) &show_ssl_ctx_sess_hits, SHOW_FUNC},
  {"Ssl_session_cache_misses", (char*) &show_ssl_ctx_sess_misses, SHOW_FUNC},
  {"Ssl_session_cache_mode",   (char*) &show_ssl_ctx_get_session_cache_mode, SHOW_FUNC},
  {"Ssl_session_cache_overflows", (char*) &show_ssl_ctx_sess_cache_full, SHOW_FUNC},
  {"Ssl_session_cache_size",   (char*) &show_ssl_ctx_sess_get_cache_size, SHOW_FUNC},
  {"Ssl_session_cache_timeouts", (char*) &show_ssl_ctx_sess_timeouts, SHOW_FUNC},
  {"Ssl_sessions_reused",      (char*) &show_ssl_session_reused, SHOW_FUNC},
  {"Ssl_used_session_cache_entries",(char*) &show_ssl_ctx_sess_number, SHOW_FUNC},
  {"Ssl_verify_depth",         (char*) &show_ssl_get_verify_depth, SHOW_FUNC},
  {"Ssl_verify_mode",          (char*) &show_ssl_get_verify_mode, SHOW_FUNC},
  {"Ssl_version",              (char*) &show_ssl_get_version, SHOW_FUNC},
  {"Ssl_server_not_before",    (char*) &show_ssl_get_server_not_before,
    SHOW_FUNC},
  {"Ssl_server_not_after",     (char*) &show_ssl_get_server_not_after,
    SHOW_FUNC},
#ifndef HAVE_YASSL
  {"Rsa_public_key",           (char*) &show_rsa_public_key, SHOW_FUNC},
#endif
#endif
#endif /* HAVE_OPENSSL */
  {"Statement_seconds",        (char*) &show_stmt_time, SHOW_FUNC},
  {"Table_locks_immediate",    (char*) &locks_immediate,        SHOW_LONG},
  {"Table_locks_waited",       (char*) &locks_waited,           SHOW_LONG},
  {"Table_open_cache_hits",    (char*) offsetof(STATUS_VAR, table_open_cache_hits), SHOW_LONGLONG_STATUS},
  {"Table_open_cache_misses",  (char*) offsetof(STATUS_VAR, table_open_cache_misses), SHOW_LONGLONG_STATUS},
  {"Table_open_cache_overflows",(char*) offsetof(STATUS_VAR, table_open_cache_overflows), SHOW_LONGLONG_STATUS},
#ifdef HAVE_MMAP
  {"Tc_log_max_pages_used",    (char*) &tc_log_max_pages_used,  SHOW_LONG},
  {"Tc_log_page_size",         (char*) &tc_log_page_size,       SHOW_LONG_NOFLUSH},
  {"Tc_log_page_waits",        (char*) &tc_log_page_waits,      SHOW_LONG},
#endif
  {"Threads_binlog_client",    (char*) &thread_binlog_client,   SHOW_INT},
  {"Threads_binlog_comp_event_client", (char*) &thread_binlog_comp_event_client, SHOW_INT},
  {"Threads_cached",           (char*) &blocked_pthread_count,    SHOW_LONG_NOFLUSH},
  {"Threads_connected",        (char*) &connection_count,       SHOW_INT},
  {"Threads_created",          (char*) &show_thread_created,   SHOW_FUNC},
  {"Threads_running",          (char*) &num_thread_running,     SHOW_INT},
  {"Timer_in_use",             (char*) &timer_in_use,           SHOW_CHAR_PTR},
  {"Tmp_table_bytes_written",  (char*) offsetof(STATUS_VAR, tmp_table_bytes_written), SHOW_LONGLONG_STATUS},
  {"Tmp_table_disk_usage",     (char*) offsetof(STATUS_VAR, tmp_table_disk_usage), SHOW_LONGLONG_STATUS},
  {"Tmp_table_disk_usage_peak", (char*) offsetof(STATUS_VAR, tmp_table_disk_usage_peak), SHOW_LONGLONG_STATUS},
  {"Tmp_table_disk_usage_period_peak", (char*) show_tmp_table_disk_usage_period_peak, SHOW_FUNC},
  {"Total_queries_rejected",   (char*) &total_query_rejected,   SHOW_LONG},
  {"Transaction_seconds",      (char*) &show_trx_time, SHOW_FUNC},
  {"Uptime",                   (char*) &show_starttime,         SHOW_FUNC},
#ifdef ENABLED_PROFILING
  {"Uptime_since_flush_status",(char*) &show_flushstatustime,   SHOW_FUNC},
#endif
  {"Write_queries",            (char*) &write_queries,          SHOW_LONG},
  {"Write_queries_rejected",   (char*) &write_query_rejected,   SHOW_LONG},
  {"Write_queries_running",    (char*) &write_query_running,    SHOW_INT},
  {NullS, NullS, SHOW_LONG}
};

void add_terminator(vector<my_option> *options)
{
  my_option empty_element=
    {0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0};
  options->push_back(empty_element);
}

#ifndef EMBEDDED_LIBRARY
static void print_version(void)
{
  set_server_version();

  printf("%s  Ver %s for %s on %s (%s)\n",my_progname,
   server_version,SYSTEM_TYPE,MACHINE_TYPE, MYSQL_COMPILATION_COMMENT);
}

/** Compares two options' names, treats - and _ the same */
static bool operator<(const my_option &a, const my_option &b)
{
  const char *sa= a.name;
  const char *sb= b.name;
  for (; *sa || *sb; sa++, sb++)
  {
    if (*sa < *sb)
    {
      if (*sa == '-' && *sb == '_')
        continue;
      else
        return true;
    }
    if (*sa > *sb)
    {
      if (*sa == '_' && *sb == '-')
        continue;
      else
        return false;
    }
  }
  DBUG_ASSERT(a.name == b.name);
  return false;
}

static void print_help()
{
  MEM_ROOT mem_root;
  init_alloc_root(&mem_root, 4096, 4096);

  all_options.pop_back();
  sys_var_add_options(&all_options, sys_var::PARSE_EARLY);
  for (my_option *opt= my_long_early_options;
       opt->name != NULL;
       opt++)
  {
    all_options.push_back(*opt);
  }
  add_plugin_options(&all_options, &mem_root);
  std::sort(all_options.begin(), all_options.end(), std::less<my_option>());
  add_terminator(&all_options);

  my_print_help(&all_options[0]);
  my_print_variables(&all_options[0]);

  free_root(&mem_root, MYF(0));
  vector<my_option>().swap(all_options);  // Deletes the vector contents.
}

static void usage(void)
{
  DBUG_ENTER("usage");
  if (!(default_charset_info= get_charset_by_csname(default_character_set_name,
                     MY_CS_PRIMARY,
               MYF(MY_WME))))
    exit(1);
  if (!default_collation_name)
    default_collation_name= (char*) default_charset_info->name;
  print_version();
  puts(ORACLE_WELCOME_COPYRIGHT_NOTICE("2000"));
  puts("Starts the MySQL database server.\n");
  printf("Usage: %s [OPTIONS]\n", my_progname);
  if (!opt_verbose)
    puts("\nFor more help options (several pages), use mysqld --verbose --help.");
  else
  {
#ifdef __WIN__
  puts("NT and Win32 specific options:\n\
  --install                     Install the default service (NT).\n\
  --install-manual              Install the default service started manually (NT).\n\
  --install service_name        Install an optional service (NT).\n\
  --install-manual service_name Install an optional service started manually (NT).\n\
  --remove                      Remove the default service from the service list (NT).\n\
  --remove service_name         Remove the service_name from the service list (NT).\n\
  --enable-named-pipe           Only to be used for the default server (NT).\n\
  --standalone                  Dummy option to start as a standalone server (NT).\
");
  puts("");
#endif
  print_defaults(MYSQL_CONFIG_NAME,load_default_groups);
  puts("");
  set_ports();

  /* Print out all the options including plugin supplied options */
  print_help();

  if (! plugins_are_initialized)
  {
    puts("\n\
Plugins have parameters that are not reflected in this list\n\
because execution stopped before plugins were initialized.");
  }

  puts("\n\
To see what values a running MySQL server is using, type\n\
'mysqladmin variables' instead of 'mysqld --verbose --help'.");
  }
  DBUG_VOID_RETURN;
}
#endif /*!EMBEDDED_LIBRARY*/

static void init_sharding_variables()
{
#ifdef SHARDED_LOCKING
  if (!gl_lock_sharding && num_sharded_locks != 1) {
    //NO_LINT_DEBUG
    sql_print_information("Setting num_sharded_locks=1 as sharding is OFF");
    num_sharded_locks = 1;
  }

  LOCK_thread_count_sharded.resize(num_sharded_locks);
  LOCK_thd_remove_sharded.resize(num_sharded_locks);
  for (uint ii = 0; ii < num_sharded_locks; ii++) {
    mysql_mutex_init(key_LOCK_thread_count,
      &LOCK_thread_count_sharded[ii], MY_MUTEX_INIT_FAST);
    mysql_mutex_init(key_LOCK_thd_remove,
      &LOCK_thd_remove_sharded[ii], MY_MUTEX_INIT_FAST);
  }
  global_thread_list= new ShardedThreads(num_sharded_locks);
  sharded_locks_init = true;
#else
  global_thread_list= new std::set<THD*>;
#endif
}

/**
  Initialize MySQL global variables to default values.

  @note
    The reason to set a lot of global variables to zero is to allow one to
    restart the embedded server with a clean environment
    It's also needed on some exotic platforms where global variables are
    not set to 0 when a program starts.

    We don't need to set variables refered to in my_long_options
    as these are initialized by my_getopt.
*/

static int mysql_init_variables(void)
{
  /* Things reset to zero */
  opt_skip_slave_start= opt_reckless_slave = 0;
  mysql_home[0]= pidfile_name[0]= shutdownfile_name[0]= log_error_file[0]= 0;
  binlog_file_basedir[0]= binlog_index_basedir[0]= 0;
  myisam_test_invalid_symlink= test_if_data_home_dir;
  opt_log= opt_slow_log= 0;
  opt_bin_log= 0;
  opt_trim_binlog= 0;
  opt_disable_networking= opt_skip_show_db=0;
  opt_skip_name_resolve= 0;
  opt_ignore_builtin_innodb= 0;
  opt_logname= opt_update_logname= opt_binlog_index_name= opt_slow_logname= 0;
  opt_applylog_index_name= 0;
  opt_gap_lock_logname= 0;
  opt_tc_log_file= (char *)"tc.log";      // no hostname in tc_log file name !
  opt_secure_auth= 0;
  opt_myisam_log= 0;
  mqh_used= 0;
  kill_in_progress= 0;
  cleanup_done= 0;
  server_id_supplied= 0;
  test_flags= select_errors= dropping_tables= ha_open_options=0;
  global_thread_count= num_thread_running= wake_pthread=0;
  kill_blocked_pthreads_flag = false;
  write_query_running= 0;
  write_queries= 0;
  read_queries= 0;
  write_query_rejected= 0;
  total_query_rejected= 0;
  object_stats_misses = 0;
  slave_open_temp_tables= 0;
  blocked_pthread_count= 0;
  opt_endinfo= using_udf_functions= 0;
  opt_using_transactions= 0;
  abort_loop= select_thread_in_use= signal_thread_in_use= 0;
  ready_to_exit= shutdown_in_progress= grant_option= 0;
  aborted_threads= aborted_connects= 0;
  delayed_insert_threads= delayed_insert_writes= delayed_rows_in_use= 0;
  delayed_insert_errors= thread_created= 0;
  specialflag= 0;
  binlog_bytes_written= 0;
  binlog_cache_use=  binlog_cache_disk_use= 0;
  binlog_fsync_count= 0;
  relay_log_bytes_written= 0;
  max_used_connections= slow_launch_threads = 0;
  mysqld_user= mysqld_chroot= opt_init_file= opt_bin_logname = 0;
  opt_apply_logname= 0;
  prepared_stmt_count= 0;
  mysqld_unix_port= opt_mysql_tmpdir= my_bind_addr_str= NullS;
  memset(&mysql_tmpdir_list, 0, sizeof(mysql_tmpdir_list));
  memset(&global_status_var, 0, sizeof(global_status_var));
  opt_large_pages= 0;
  opt_super_large_pages= 0;
  log_datagram= 0;
  log_datagram_usecs= 0;
#if defined(ENABLED_DEBUG_SYNC)
  opt_debug_sync_timeout= 0;
#endif /* defined(ENABLED_DEBUG_SYNC) */
  key_map_full.set_all();
  server_uuid[0]= 0;

  opt_log_slow_extra= FALSE;
  log_datagram_sock= -1;
  opt_peak_lag_sample_rate= 100;
  opt_srv_fatal_semaphore_timeout = DEFAULT_SRV_FATAL_SEMAPHORE_TIMEOUT;

  /* Character sets */
  system_charset_info= &my_charset_utf8_general_ci;
  files_charset_info= &my_charset_utf8_general_ci;
  national_charset_info= &my_charset_utf8_general_ci;
  table_alias_charset= &my_charset_bin;
  character_set_filesystem= &my_charset_bin;

  opt_specialflag= 0;
  unix_sock= MYSQL_INVALID_SOCKET;
  ip_sock= MYSQL_INVALID_SOCKET;
  admin_ip_sock = MYSQL_INVALID_SOCKET;
  binlog_file_basedir_ptr= binlog_file_basedir;
  binlog_index_basedir_ptr= binlog_index_basedir;
  per_user_session_var_default_val_ptr= NULL;

  mysql_home_ptr= mysql_home;
  pidfile_name_ptr= pidfile_name;
  shutdownfile_name_ptr= shutdownfile_name;
  log_error_file_ptr= log_error_file;
  lc_messages_dir_ptr= lc_messages_dir;
  protocol_version= PROTOCOL_VERSION;
  what_to_log= ~ (1L << (uint) COM_TIME);
  refresh_version= 1L;  /* Increments on each reload */
  global_query_id= thread_id_counter= 1L;
  my_atomic_rwlock_init(&global_query_id_lock);
  my_atomic_rwlock_init(&thread_running_lock);
  my_atomic_rwlock_init(&write_query_running_lock);
  strmov(server_version, MYSQL_SERVER_VERSION);

  global_thread_id_list= new std::set<my_thread_id>;
  global_thread_id_list->insert(reserved_thread_id);
  waiting_thd_list= new std::list<THD*>;
  key_caches.empty();
  if (!(dflt_key_cache= get_or_create_key_cache(default_key_cache_base.str,
                                                default_key_cache_base.length)))
  {
    sql_print_error("Cannot allocate the keycache");
    return 1;
  }
  /* set key_cache_hash.default_value = dflt_key_cache */
  multi_keycache_init();

  /* Set directory paths */
  mysql_real_data_home_len=
    strmake(mysql_real_data_home, get_relative_path(MYSQL_DATADIR),
            sizeof(mysql_real_data_home)-1) - mysql_real_data_home;
  /* Replication parameters */
  master_info_file= (char*) "master.info",
    relay_log_info_file= (char*) "relay-log.info";
  report_user= report_password = report_host= 0;  /* TO BE DELETED */
  opt_relay_logname= opt_relaylog_index_name= 0;
  log_bin_basename= NULL;
  log_bin_index= NULL;

  /* Handler variables */
  total_ha= 0;
  total_ha_2pc= 0;
  /* Variables in libraries */
  charsets_dir= 0;
  default_character_set_name= (char*) MYSQL_DEFAULT_CHARSET_NAME;
  default_collation_name= compiled_default_collation_name;
  character_set_filesystem_name= (char*) "binary";
  lc_messages= (char*) "en_US";
  lc_time_names_name= (char*) "en_US";

  /* Variables that depends on compile options */
#ifndef DBUG_OFF
  default_dbug_option=IF_WIN("d:t:i:O,\\mysqld.trace",
           "d:t:i:o,/tmp/mysqld.trace");
#endif
  opt_error_log= IF_WIN(1,0);
#ifdef ENABLED_PROFILING
    have_profiling = SHOW_OPTION_YES;
#else
    have_profiling = SHOW_OPTION_NO;
#endif

#ifdef HAVE_OPENSSL
  have_openssl = have_ssl = SHOW_OPTION_YES;
#else
  have_openssl = have_ssl = SHOW_OPTION_NO;
#endif
#ifdef HAVE_BROKEN_REALPATH
  have_symlink=SHOW_OPTION_NO;
#else
  have_symlink=SHOW_OPTION_YES;
#endif
#ifdef HAVE_DLOPEN
  have_dlopen=SHOW_OPTION_YES;
#else
  have_dlopen=SHOW_OPTION_NO;
#endif
#ifdef HAVE_QUERY_CACHE
  have_query_cache=SHOW_OPTION_YES;
#else
  have_query_cache=SHOW_OPTION_NO;
#endif
#ifdef HAVE_SPATIAL
  have_geometry=SHOW_OPTION_YES;
#else
  have_geometry=SHOW_OPTION_NO;
#endif
#ifdef HAVE_RTREE_KEYS
  have_rtree_keys=SHOW_OPTION_YES;
#else
  have_rtree_keys=SHOW_OPTION_NO;
#endif
#ifdef HAVE_CRYPT
  have_crypt=SHOW_OPTION_YES;
#else
  have_crypt=SHOW_OPTION_NO;
#endif
#ifdef HAVE_COMPRESS
  have_compress= SHOW_OPTION_YES;
#else
  have_compress= SHOW_OPTION_NO;
#endif
#ifdef HAVE_LIBWRAP
  libwrapName= NullS;
#endif
#ifdef HAVE_OPENSSL
  des_key_file = 0;
#ifndef EMBEDDED_LIBRARY
  ssl_acceptor_fd= 0;
#endif /* ! EMBEDDED_LIBRARY */
#endif /* HAVE_OPENSSL */
#ifdef HAVE_SMEM
  shared_memory_base_name= default_shared_memory_base_name;
#endif

#if defined(__WIN__)
  /* Allow Win32 users to move MySQL anywhere */
  {
    char prg_dev[LIBLEN];
    char executing_path_name[LIBLEN];
    if (!test_if_hard_path(my_progname))
    {
      // we don't want to use GetModuleFileName inside of my_path since
      // my_path is a generic path dereferencing function and here we care
      // only about the executing binary.
      GetModuleFileName(NULL, executing_path_name, sizeof(executing_path_name));
      my_path(prg_dev, executing_path_name, NULL);
    }
    else
      my_path(prg_dev, my_progname, "mysql/bin");
    strcat(prg_dev,"/../");     // Remove 'bin' to get base dir
    cleanup_dirname(mysql_home,prg_dev);
  }
#else
  const char *tmpenv;
  if (!(tmpenv = getenv("MY_BASEDIR_VERSION")))
    tmpenv = DEFAULT_MYSQL_HOME;
  (void) strmake(mysql_home, tmpenv, sizeof(mysql_home)-1);
#endif
  return 0;
}

my_bool
mysqld_get_one_option(int optid,
                      const struct my_option *opt MY_ATTRIBUTE((unused)),
                      char *argument)
{
  switch(optid) {
  case '#':
#ifndef DBUG_OFF
    DBUG_SET_INITIAL(argument ? argument : default_dbug_option);
#endif
    opt_endinfo=1;        /* unireg: memory allocation */
    break;
  case 'a':
    global_system_variables.sql_mode= MODE_ANSI;
    global_system_variables.tx_isolation= ISO_SERIALIZABLE;
    break;
  case 'b':
    strmake(mysql_home,argument,sizeof(mysql_home)-1);
    mysql_home_ptr= mysql_home;
    break;
  case 'C':
    if (default_collation_name == compiled_default_collation_name)
      default_collation_name= 0;
    break;
  case 'h':
    strmake(mysql_real_data_home,argument, sizeof(mysql_real_data_home)-1);
    /* Correct pointer set by my_getopt (for embedded library) */
    mysql_real_data_home_ptr= mysql_real_data_home;
    break;
  case 'u':
    if (!mysqld_user || !strcmp(mysqld_user, argument))
      mysqld_user= argument;
    else
      sql_print_warning("Ignoring user change to '%s' because the user was set to '%s' earlier on the command line\n", argument, mysqld_user);
    break;
  case 'L':
    WARN_DEPRECATED(NULL, "--language/-l", "'--lc-messages-dir'");
    /* Note:  fall-through */
  case OPT_LC_MESSAGES_DIRECTORY:
    strmake(lc_messages_dir, argument, sizeof(lc_messages_dir)-1);
    lc_messages_dir_ptr= lc_messages_dir;
    break;
  case OPT_BINLOG_FORMAT:
    binlog_format_used= true;
    break;
  case OPT_BINLOGGING_IMPOSSIBLE_MODE:
    WARN_DEPRECATED(NULL, "--binlogging_impossible_mode",
                    "'--binlog_error_action'");
    break;
  case OPT_SIMPLIFIED_BINLOG_GTID_RECOVERY:
    WARN_DEPRECATED(NULL, "--simplified_binlog_gtid_recovery",
                    "'--binlog_gtid_simple_recovery'");
    break;
#ifndef EMBEDDED_LIBRARY
  case 'V':
    print_version();
    exit(0);
#endif /*EMBEDDED_LIBRARY*/
  case 'W':
    if (!argument)
      log_warnings++;
    else if (argument == disabled_my_option)
      log_warnings= 0L;
    else
      log_warnings= atoi(argument);
    break;
  case 'T':
    test_flags= argument ? (uint) atoi(argument) : 0;
    opt_endinfo=1;
    break;
  case OPT_THREAD_CONCURRENCY:
    WARN_DEPRECATED_NO_REPLACEMENT(NULL, "THREAD_CONCURRENCY");
    break;
  case (int) OPT_ISAM_LOG:
    opt_myisam_log=1;
    break;
  case (int) OPT_BIN_LOG:
    opt_bin_log= MY_TEST(argument != disabled_my_option);
    break;
#ifdef HAVE_REPLICATION
  case (int)OPT_REPLICATE_IGNORE_DB:
  {
    rpl_filter->add_ignore_db(argument);
    break;
  }
  case (int)OPT_REPLICATE_DO_DB:
  {
    rpl_filter->add_do_db(argument);
    break;
  }
  case (int)OPT_REPLICATE_REWRITE_DB:
  {
    char* key = argument,*p, *val;

    if (!(p= strstr(argument, "->")))
    {
      sql_print_error("Bad syntax in replicate-rewrite-db - missing '->'!\n");
      return 1;
    }
    val= p + 2;
    while(p > argument && my_isspace(mysqld_charset, p[-1]))
      p--;
    *p= 0;
    if (!*key)
    {
      sql_print_error("Bad syntax in replicate-rewrite-db - empty FROM db!\n");
      return 1;
    }
    while (*val && my_isspace(mysqld_charset, *val))
      val++;
    if (!*val)
    {
      sql_print_error("Bad syntax in replicate-rewrite-db - empty TO db!\n");
      return 1;
    }

    rpl_filter->add_db_rewrite(key, val);
    break;
  }

  case (int)OPT_BINLOG_IGNORE_DB:
  {
    binlog_filter->add_ignore_db(argument);
    break;
  }
  case (int)OPT_BINLOG_DO_DB:
  {
    binlog_filter->add_do_db(argument);
    break;
  }
  case (int)OPT_REPLICATE_DO_TABLE:
  {
    if (rpl_filter->add_do_table_array(argument))
    {
      sql_print_error("Could not add do table rule '%s'!\n", argument);
      return 1;
    }
    break;
  }
  case (int)OPT_REPLICATE_WILD_DO_TABLE:
  {
    if (rpl_filter->add_wild_do_table(argument))
    {
      sql_print_error("Could not add do table rule '%s'!\n", argument);
      return 1;
    }
    break;
  }
  case (int)OPT_REPLICATE_WILD_IGNORE_TABLE:
  {
    if (rpl_filter->add_wild_ignore_table(argument))
    {
      sql_print_error("Could not add ignore table rule '%s'!\n", argument);
      return 1;
    }
    break;
  }
  case (int)OPT_REPLICATE_IGNORE_TABLE:
  {
    if (rpl_filter->add_ignore_table_array(argument))
    {
      sql_print_error("Could not add ignore table rule '%s'!\n", argument);
      return 1;
    }
    break;
  }
#endif /* HAVE_REPLICATION */
  case (int) OPT_MASTER_RETRY_COUNT:
    WARN_DEPRECATED(NULL, "--master-retry-count", "'CHANGE MASTER TO master_retry_count = <num>'");
    break;
  case (int) OPT_SKIP_NEW:
    opt_specialflag|= SPECIAL_NO_NEW_FUNC;
    delay_key_write_options= DELAY_KEY_WRITE_NONE;
    myisam_concurrent_insert=0;
    myisam_recover_options= HA_RECOVER_OFF;
    sp_automatic_privileges=0;
    my_use_symdir=0;
    ha_open_options&= ~(HA_OPEN_ABORT_IF_CRASHED | HA_OPEN_DELAY_KEY_WRITE);
#ifdef HAVE_QUERY_CACHE
    query_cache_size=0;
#endif
    break;
  case (int) OPT_SKIP_HOST_CACHE:
    opt_specialflag|= SPECIAL_NO_HOST_CACHE;
    break;
  case (int) OPT_SKIP_RESOLVE:
    opt_skip_name_resolve= 1;
    opt_specialflag|=SPECIAL_NO_RESOLVE;
    break;
  case (int) OPT_SKIP_STACK_TRACE:
    test_flags|=TEST_NO_STACKTRACE;
    break;
  case OPT_CONSOLE:
    if (opt_console)
      opt_error_log= 0;     // Force logs to stdout
    break;
  case OPT_BOOTSTRAP:
    opt_bootstrap=1;
    break;
  case OPT_SERVER_ID:
    server_id_supplied = 1;
    break;
  case OPT_LOWER_CASE_TABLE_NAMES:
    lower_case_table_names_used= 1;
    break;
#if defined(ENABLED_DEBUG_SYNC)
  case OPT_DEBUG_SYNC_TIMEOUT:
    /*
      Debug Sync Facility. See debug_sync.cc.
      Default timeout for WAIT_FOR action.
      Default value is zero (facility disabled).
      If option is given without an argument, supply a non-zero value.
    */
    if (!argument)
    {
      /* purecov: begin tested */
      opt_debug_sync_timeout= DEBUG_SYNC_DEFAULT_WAIT_TIMEOUT;
      /* purecov: end */
    }
    break;
#endif /* defined(ENABLED_DEBUG_SYNC) */
  case OPT_ENGINE_CONDITION_PUSHDOWN:
    /*
      The last of --engine-condition-pushdown and --optimizer_switch on
      command line wins (see get_options().
    */
    if (global_system_variables.engine_condition_pushdown)
      global_system_variables.optimizer_switch|=
        OPTIMIZER_SWITCH_ENGINE_CONDITION_PUSHDOWN;
    else
      global_system_variables.optimizer_switch&=
        ~OPTIMIZER_SWITCH_ENGINE_CONDITION_PUSHDOWN;
    break;
  case OPT_LOG_ERROR:
    /*
      "No --log-error" == "write errors to stderr",
      "--log-error without argument" == "write errors to a file".
    */
    if (argument == NULL) /* no argument */
      log_error_file_ptr= const_cast<char*>("");
    break;

  case OPT_IGNORE_DB_DIRECTORY:
    if (*argument == 0)
      ignore_db_dirs_reset();
    else
    {
      if (push_ignored_db_dir(argument))
      {
        sql_print_error("Can't start server: "
                        "cannot process --ignore-db-dir=%.*s",
                        FN_REFLEN, argument);
        return 1;
      }
    }
    break;


  case OPT_PLUGIN_LOAD:
    free_list(opt_plugin_load_list_ptr);
    /* fall through */
  case OPT_PLUGIN_LOAD_ADD:
    opt_plugin_load_list_ptr->push_back(new i_string(argument));
    break;
  case OPT_DEFAULT_AUTH:
    if (set_default_auth_plugin(argument, strlen(argument)))
    {
      sql_print_error("Can't start server: "
                      "Invalid value for --default-authentication-plugin");
      return 1;
    }
    break;
  case OPT_SECURE_AUTH:
    if (opt_secure_auth == 0)
      WARN_DEPRECATED(NULL, "pre-4.1 password hash", "post-4.1 password hash");
    break;
  case OPT_PFS_INSTRUMENT:
    {
#ifdef WITH_PERFSCHEMA_STORAGE_ENGINE
#ifndef EMBEDDED_LIBRARY

      /*
        Parse instrument name and value from argument string. Handle leading
        and trailing spaces. Also handle single quotes.

        Acceptable:
          performance_schema_instrument = ' foo/%/bar/  =  ON  '
          performance_schema_instrument = '%=OFF'
        Not acceptable:
          performance_schema_instrument = '' foo/%/bar = ON ''
          performance_schema_instrument = '%='OFF''
      */
      char *name= argument,*p= NULL, *val= NULL;
      my_bool quote= false; /* true if quote detected */
      my_bool error= true;  /* false if no errors detected */
      const int PFS_BUFFER_SIZE= 128;
      char orig_argument[PFS_BUFFER_SIZE+1];
      orig_argument[0]= 0;

      if (!argument)
        goto pfs_error;

      /* Save original argument string for error reporting */
      strncpy(orig_argument, argument, PFS_BUFFER_SIZE);

      /* Split instrument name and value at the equal sign */
      if (!(p= strchr(argument, '=')))
        goto pfs_error;

      /* Get option value */
      val= p + 1;
      if (!*val)
        goto pfs_error;

      /* Trim leading spaces and quote from the instrument name */
      while (*name && (my_isspace(mysqld_charset, *name) || (*name == '\'')))
      {
        /* One quote allowed */
        if (*name == '\'')
        {
          if (!quote)
            quote= true;
          else
            goto pfs_error;
        }
        name++;
      }

      /* Trim trailing spaces from instrument name */
      while ((p > name) && my_isspace(mysqld_charset, p[-1]))
        p--;
      *p= 0;

      /* Remove trailing slash from instrument name */
      if (p > name && (p[-1] == '/'))
        p[-1]= 0;

      if (!*name)
        goto pfs_error;

      /* Trim leading spaces from option value */
      while (*val && my_isspace(mysqld_charset, *val))
        val++;

      /* Trim trailing spaces and matching quote from value */
      p= val + strlen(val);
      while (p > val && (my_isspace(mysqld_charset, p[-1]) || p[-1] == '\''))
      {
        /* One matching quote allowed */
        if (p[-1] == '\'')
        {
          if (quote)
            quote= false;
          else
            goto pfs_error;
        }
        p--;
      }

      *p= 0;

      if (!*val)
        goto pfs_error;

      /* Add instrument name and value to array of configuration options */
      if (add_pfs_instr_to_array(name, val))
        goto pfs_error;

      error= false;

pfs_error:
      if (error)
      {
        my_getopt_error_reporter(WARNING_LEVEL,
                                 "Invalid instrument name or value for "
                                 "performance_schema_instrument '%s'",
                                 orig_argument);
        return 0;
      }
#endif /* EMBEDDED_LIBRARY */
#endif /* WITH_PERFSCHEMA_STORAGE_ENGINE */
      break;
    }
  case OPT_THREAD_CACHE_SIZE:
    thread_cache_size_specified= true;
    break;
  case OPT_HOST_CACHE_SIZE:
    host_cache_size_specified= true;
    break;
  case OPT_TABLE_DEFINITION_CACHE:
    table_definition_cache_specified= true;
    break;
  case OPT_AVOID_TEMPORAL_UPGRADE:
    WARN_DEPRECATED_NO_REPLACEMENT(NULL, "avoid_temporal_upgrade");
    break;
  case OPT_SHOW_OLD_TEMPORALS:
    WARN_DEPRECATED_NO_REPLACEMENT(NULL, "show_old_temporals");
    break;
  case OPT_TRIM_BINLOG_TO_RECOVER:
    opt_trim_binlog= 1;
    break;
    case OPT_NAMED_PIPE_FULL_ACCESS_GROUP:
#if (defined(_WIN32) || defined(HAVE_SMEM)) && !defined(EMBEDDED_LIBRARY)
      if (!is_valid_named_pipe_full_access_group(argument))
      {
        sql_print_error("Invalid value for named_pipe_full_access_group.");
        return 1;
      }
#endif  /* _WIN32 || HAVE_SMEM && !EMBEDDED_LIBRARY */
      break;
  }
  return 0;
}


/** Handle arguments for multiple key caches. */

C_MODE_START

static void*
mysql_getopt_value(const char *keyname, uint key_length,
       const struct my_option *option, int *error)
{
  if (error)
    *error= 0;
  switch (option->id) {
  case OPT_KEY_BUFFER_SIZE:
  case OPT_KEY_CACHE_BLOCK_SIZE:
  case OPT_KEY_CACHE_DIVISION_LIMIT:
  case OPT_KEY_CACHE_AGE_THRESHOLD:
  {
    KEY_CACHE *key_cache;
    if (!(key_cache= get_or_create_key_cache(keyname, key_length)))
    {
      if (error)
        *error= EXIT_OUT_OF_MEMORY;
      return 0;
    }
    switch (option->id) {
    case OPT_KEY_BUFFER_SIZE:
      return &key_cache->param_buff_size;
    case OPT_KEY_CACHE_BLOCK_SIZE:
      return &key_cache->param_block_size;
    case OPT_KEY_CACHE_DIVISION_LIMIT:
      return &key_cache->param_division_limit;
    case OPT_KEY_CACHE_AGE_THRESHOLD:
      return &key_cache->param_age_threshold;
    }
  }
  }
  return option->value;
}

static void option_error_reporter(enum loglevel level, const char *format, ...)
{
  va_list args;
  va_start(args, format);

  /* Don't print warnings for --loose options during bootstrap */
  if (level == ERROR_LEVEL || !opt_bootstrap ||
      log_warnings)
  {
    vprint_msg_to_log(level, format, args);
  }
  va_end(args);
}

C_MODE_END

static int generate_apply_file_gvars()
{
  DBUG_ENTER("generate_apply_file_gvars");

  /* Reports an error and aborts, if the --apply-log's path
     is a directory.*/
  if (opt_apply_logname &&
      opt_apply_logname[strlen(opt_apply_logname) - 1] == FN_LIBCHAR)
  {
    sql_print_error("Path '%s' is a directory name, please specify \
a file name for --apply-log option", opt_apply_logname);
    unireg_abort(1);
  }

  /* Reports an error and aborts, if the --apply-log-index's path
     is a directory.*/
  if (opt_applylog_index_name &&
      opt_applylog_index_name[strlen(opt_applylog_index_name) - 1]
      == FN_LIBCHAR)
  {
    sql_print_error("Path '%s' is a directory name, please specify \
a file name for --apply-log-index option", opt_applylog_index_name);
    unireg_abort(1);
  }

  if (opt_apply_logname && opt_applylog_index_name)
  {
    DBUG_RETURN(0);
  }

  if (!opt_apply_logname && !disable_raft_log_repointing)
  {
    /* create the apply binlog name for Raft using some common
     * rules. Replace binary with apply or -bin with -apply
     * This handles the 2 common cases of naming convention without
     * having to add these variables in all config files.
     * PROD - binary-logs-3301
     * mtr - master-bin, slave-bin
     */
    std::string tstr(opt_bin_logname);
    std::string rep_from1("binary");
    std::string rep_to1("apply");
    std::string rep_from2("-bin");

    size_t fpos = tstr.find(rep_from1);
    if (fpos != std::string::npos)
    {
      tstr.replace(fpos, rep_from1.length(), rep_to1);
      opt_apply_logname= my_strdup(tstr.c_str(), MYF(0));
      should_free_opt_apply_logname= true;
    }
    else if ((fpos = tstr.find(rep_from2)) != std::string::npos)
    {
      std::string rep_to2("-apply");
      tstr.replace(fpos, rep_from2.length(), rep_to2);
      opt_apply_logname= my_strdup(tstr.c_str(), MYF(0));
      should_free_opt_apply_logname= true;
    }
    else if (enable_raft_plugin)
    {
      sql_print_error("apply log needs to be set or follow a pattern");
      unireg_abort(1);
    }
  }
  else
  {
    // Just point apply logs to binlogs
    opt_apply_logname= my_strdup(opt_bin_logname, MYF(0));
    should_free_opt_apply_logname= true;
  }

  if (!opt_applylog_index_name && opt_apply_logname)
  {
    opt_applylog_index_name=
      const_cast<char *>(rpl_make_log_name(NULL, opt_apply_logname, ".index"));
    should_free_opt_applylog_index_name= true;
  }

  DBUG_RETURN(0);
}

/**
  Get server options from the command line,
  and perform related server initializations.
  @param [in, out] argc_ptr       command line options (count)
  @param [in, out] argv_ptr       command line options (values)
  @return 0 on success

  @todo
  - FIXME add EXIT_TOO_MANY_ARGUMENTS to "mysys_err.h" and return that code?
*/
static int get_options(int *argc_ptr, char ***argv_ptr, my_bool logging)
{
  int ho_error;

  my_getopt_register_get_addr(mysql_getopt_value);
  my_getopt_error_reporter= option_error_reporter;

  /* prepare all_options array */
  all_options.reserve(array_elements(my_long_options));
  for (my_option *opt= my_long_options;
       opt < my_long_options + array_elements(my_long_options) - 1;
       opt++)
  {
    all_options.push_back(*opt);
  }
  sys_var_add_options(&all_options, sys_var::PARSE_NORMAL);
  add_terminator(&all_options);

  /* Skip unknown options so that they may be processed later by plugins */
  my_getopt_skip_unknown= TRUE;

  if (logging)
  {
    ho_error= handle_options_with_logging(argc_ptr, argv_ptr, &all_options[0],
                                          mysqld_get_one_option);
  }
  else
  {
    ho_error= handle_options(argc_ptr, argv_ptr, &all_options[0],
                             mysqld_get_one_option);
  }

  if (ho_error)
    return ho_error;

  if (!opt_help)
    vector<my_option>().swap(all_options);  // Deletes the vector contents.

  /* Add back the program name handle_options removes */
  (*argc_ptr)++;
  (*argv_ptr)--;

  /*
    Options have been parsed. Now some of them need additional special
    handling, like custom value checking, checking of incompatibilites
    between options, setting of multiple variables, etc.
    Do them here.
  */

  if ((opt_log_slow_admin_statements || opt_log_queries_not_using_indexes ||
       opt_log_slow_slave_statements) &&
      !opt_slow_log)
    sql_print_warning("options --log-slow-admin-statements, "
                      "--log-queries-not-using-indexes and "
                      "--log-slow-slave-statements have no effect if "
                      "--slow-query-log is not set");
  if (global_system_variables.net_buffer_length >
      global_system_variables.max_allowed_packet)
  {
    sql_print_warning("net_buffer_length (%lu) is set to be larger "
                      "than max_allowed_packet (%lu). Please rectify.",
                      global_system_variables.net_buffer_length,
                      global_system_variables.max_allowed_packet);
  }

  /*
    TIMESTAMP columns get implicit DEFAULT values when
    --explicit_defaults_for_timestamp is not set.
    This behavior is deprecated now.
  */
  if (!opt_help && !global_system_variables.explicit_defaults_for_timestamp)
    sql_print_warning("TIMESTAMP with implicit DEFAULT value is deprecated. "
                      "Please use --explicit_defaults_for_timestamp server "
                      "option (see documentation for more details).");


  if (log_error_file_ptr != disabled_my_option)
    opt_error_log= 1;
  else
    log_error_file_ptr= const_cast<char*>("");

  opt_init_connect.length=strlen(opt_init_connect.str);
  opt_init_slave.length=strlen(opt_init_slave.str);

  if (global_system_variables.low_priority_updates)
    thr_upgraded_concurrent_insert_lock= TL_WRITE_LOW_PRIORITY;

  if (ft_boolean_check_syntax_string((uchar*) ft_boolean_syntax))
  {
    sql_print_error("Invalid ft-boolean-syntax string: %s\n",
                    ft_boolean_syntax);
    return 1;
  }

  if (opt_disable_networking)
    mysqld_port= 0;

  if (opt_skip_show_db)
    opt_specialflag|= SPECIAL_SKIP_SHOW_DB;

  if (myisam_flush)
    flush_time= 0;

#ifdef HAVE_REPLICATION
  if (opt_slave_skip_errors)
    add_slave_skip_errors(opt_slave_skip_errors);
#endif

  if (global_system_variables.max_join_size == HA_POS_ERROR)
    global_system_variables.option_bits|= OPTION_BIG_SELECTS;
  else
    global_system_variables.option_bits&= ~OPTION_BIG_SELECTS;

  // Synchronize @@global.autocommit on --autocommit
  const ulonglong turn_bit_on= opt_autocommit ?
    OPTION_AUTOCOMMIT : OPTION_NOT_AUTOCOMMIT;
  global_system_variables.option_bits=
    (global_system_variables.option_bits &
     ~(OPTION_NOT_AUTOCOMMIT | OPTION_AUTOCOMMIT)) | turn_bit_on;

  global_system_variables.sql_mode=
    expand_sql_mode(global_system_variables.sql_mode);
#if defined(HAVE_BROKEN_REALPATH)
  my_use_symdir=0;
  my_disable_symlinks=1;
  have_symlink=SHOW_OPTION_NO;
#else
  if (!my_use_symdir)
  {
    my_disable_symlinks=1;
    have_symlink=SHOW_OPTION_DISABLED;
  }
#endif
  if (opt_debugging)
  {
    /* Allow break with SIGINT, no core or stack trace */
    test_flags|= TEST_SIGINT | TEST_NO_STACKTRACE;
    opt_core_file = FALSE;
  }
  /* Set global MyISAM variables from delay_key_write_options */
  fix_delay_key_write(0, 0, OPT_GLOBAL);

  set_gap_lock_exception_list(0, 0, OPT_GLOBAL);
  set_legacy_user_name_pattern(0, 0, OPT_GLOBAL);
  set_admin_users_list(0, 0, OPT_GLOBAL);

#ifndef EMBEDDED_LIBRARY
  if (mysqld_chroot)
    set_root(mysqld_chroot);
#else
  thread_handling = SCHEDULER_NO_THREADS;
  max_allowed_packet= global_system_variables.max_allowed_packet;
  net_buffer_length= global_system_variables.net_buffer_length;
#endif
  if (fix_paths())
    return 1;

  /*
    Set some global variables from the global_system_variables
    In most cases the global variables will not be used
  */
  my_disable_locking= myisam_single_user= MY_TEST(opt_external_locking == 0);
  my_default_record_cache_size=global_system_variables.read_buff_size;

  global_system_variables.long_query_time= (ulonglong)
    (global_system_variables.long_query_time_double * 1e6);

  global_system_variables.lock_wait_timeout_nsec= (ulonglong)
    (global_system_variables.lock_wait_timeout_double * 1e9);

  if (opt_short_log_format)
    opt_specialflag|= SPECIAL_SHORT_LOG_FORMAT;

  if (init_global_datetime_format(MYSQL_TIMESTAMP_DATE,
                                  &global_date_format) ||
      init_global_datetime_format(MYSQL_TIMESTAMP_TIME,
                                  &global_time_format) ||
      init_global_datetime_format(MYSQL_TIMESTAMP_DATETIME,
                                  &global_datetime_format))
    return 1;

#ifdef EMBEDDED_LIBRARY
  one_thread_scheduler();
#else
  if (thread_handling <= SCHEDULER_ONE_THREAD_PER_CONNECTION)
    one_thread_per_connection_scheduler();
  else                  /* thread_handling == SCHEDULER_NO_THREADS) */
    one_thread_scheduler();
#endif

  global_system_variables.engine_condition_pushdown=
    MY_TEST(global_system_variables.optimizer_switch &
            OPTIMIZER_SWITCH_ENGINE_CONDITION_PUSHDOWN);

  return 0;
}


/*
  Create version name for running mysqld version
  We automaticly add suffixes -debug, -embedded and -log to the version
  name to make the version more descriptive.
  (MYSQL_SERVER_SUFFIX is set by the compilation environment)
*/

static void set_server_version(void)
{
  char *end= strxmov(server_version, MYSQL_SERVER_VERSION,
                     MYSQL_SERVER_SUFFIX_STR, NullS);
#ifdef EMBEDDED_LIBRARY
  end= strmov(end, "-embedded");
#endif
#ifndef DBUG_OFF
  if (!strstr(MYSQL_SERVER_SUFFIX_STR, "-debug"))
    end= strmov(end, "-debug");
#endif
#ifdef HAVE_VALGRIND
  if (SERVER_VERSION_LENGTH - (end - server_version) >
      static_cast<int>(sizeof("-valgrind")))
    end = strmov(end, "-valgrind");
#endif
#ifdef HAVE_ASAN
  if (SERVER_VERSION_LENGTH - (end - server_version) >
      static_cast<int>(sizeof("-asan")))
    end = strmov(end, "-asan");
#endif
#ifdef HAVE_LSAN
  if (SERVER_VERSION_LENGTH - (end - server_version) >
      static_cast<int>(sizeof("-lsan")))
    end = strmov(end, "-lsan");
#endif
#ifdef HAVE_UBSAN
  if (SERVER_VERSION_LENGTH - (end - server_version) >
      static_cast<int>(sizeof("-ubsan")))
    end = strmov(end, "-ubsan");
#endif
#ifdef HAVE_TSAN
  if (SERVER_VERSION_LENGTH - (end - server_version) >
      static_cast<int>(sizeof("-tsan")))
    end = strmov(end, "-tsan");
#endif
  if (opt_log || opt_slow_log || opt_bin_log)
    strmov(end, "-log");                     // This may slow down system
}


static char *get_relative_path(const char *path)
{
  if (test_if_hard_path(path) &&
      is_prefix(path,DEFAULT_MYSQL_HOME) &&
      strcmp(DEFAULT_MYSQL_HOME,FN_ROOTDIR))
  {
    path+=(uint) strlen(DEFAULT_MYSQL_HOME);
    while (*path == FN_LIBCHAR || *path == FN_LIBCHAR2)
      path++;
  }
  return (char*) path;
}


/**
  Fix filename and replace extension where 'dir' is relative to
  mysql_real_data_home.
  @return
    1 if len(path) > FN_REFLEN
*/

bool
fn_format_relative_to_data_home(char * to, const char *name,
        const char *dir, const char *extension)
{
  char tmp_path[FN_REFLEN];
  if (!test_if_hard_path(dir))
  {
    strxnmov(tmp_path,sizeof(tmp_path)-1, mysql_real_data_home,
       dir, NullS);
    dir=tmp_path;
  }
  return !fn_format(to, name, dir, extension,
        MY_APPEND_EXT | MY_UNPACK_FILENAME | MY_SAFE_PATH);
}


/**
  Test a file path to determine if the path is compatible with the secure file
  path restriction.

  @param path null terminated character string

  @return
    @retval TRUE The path is secure
    @retval FALSE The path isn't secure
*/

bool is_secure_file_path(char *path)
{
  char buff1[FN_REFLEN], buff2[FN_REFLEN];
  size_t opt_secure_file_priv_len;
  /*
    All paths are secure if opt_secure_file_priv is 0
  */
  if (!opt_secure_file_priv[0])
    return TRUE;

  opt_secure_file_priv_len= strlen(opt_secure_file_priv);

  if (strlen(path) >= FN_REFLEN)
    return FALSE;

  if (!my_strcasecmp(system_charset_info, opt_secure_file_priv, "NULL"))
    return FALSE;

  if (my_realpath(buff1, path, 0))
  {
    /*
      The supplied file path might have been a file and not a directory.
    */
    int length= (int)dirname_length(path);
    if (length >= FN_REFLEN)
      return FALSE;
    memcpy(buff2, path, length);
    buff2[length]= '\0';
    if (length == 0 || my_realpath(buff1, buff2, 0))
      return FALSE;
  }
  convert_dirname(buff2, buff1, NullS);
  if (!lower_case_file_system)
  {
    if (strncmp(opt_secure_file_priv, buff2, opt_secure_file_priv_len))
      return FALSE;
  }
  else
  {
    if (files_charset_info->coll->strnncoll(files_charset_info,
                                            (uchar *) buff2, strlen(buff2),
                                            (uchar *) opt_secure_file_priv,
                                            opt_secure_file_priv_len,
                                            TRUE))
      return FALSE;
  }
  return TRUE;
}

/**
   Generate shutdown file name, which is the pid file name with
   suffix ".shutdown"
*/
static void generate_shutdownfile_name()
{
  static const char *SHUTDOWN_SUFFIX = ".shutdown";
  strmake(shutdownfile_name, pidfile_name,
          sizeof(shutdownfile_name) - strlen(SHUTDOWN_SUFFIX) - 1);
  strmov(&shutdownfile_name[strlen(shutdownfile_name)], SHUTDOWN_SUFFIX);
}

/**
  Test a file path whether it is same as mysql data directory path.

  @param path null terminated character string

  @return
    @retval TRUE The path is different from mysql data directory.
    @retval FALSE The path is same as mysql data directory.
*/
bool is_mysql_datadir_path(const char *path)
{
  if (path == NULL)
    return false;

  char mysql_data_dir[FN_REFLEN], path_dir[FN_REFLEN];
  convert_dirname(path_dir, path, NullS);
  convert_dirname(mysql_data_dir, mysql_unpacked_real_data_home, NullS);
  size_t mysql_data_home_len= dirname_length(mysql_data_dir);
  size_t path_len = dirname_length(path_dir);

  if (path_len < mysql_data_home_len)
    return true;

  if (!lower_case_file_system)
    return(memcmp(mysql_data_dir, path_dir, mysql_data_home_len));

  return(files_charset_info->coll->strnncoll(files_charset_info,
                                            (uchar *) path_dir, path_len,
                                            (uchar *) mysql_data_dir,
                                            mysql_data_home_len,
                                            TRUE));

}

/**
  check_secure_file_priv_path : Checks path specified through
  --secure-file-priv and raises warning in following cases:
  1. If path is empty string or NULL and mysqld is not running
     with --bootstrap mode.
  2. If path can access data directory
  3. If path points to a directory which is accessible by
     all OS users (non-Windows build only)

  It throws error in following cases:

  1. If path normalization fails
  2. If it can not get stats of the directory

  @params NONE

  Assumptions :
  1. Data directory path has been normalized
  2. opt_secure_file_priv has been normalized unless it is set
     to "NULL".

  @returns Status of validation
    @retval true : Validation is successful with/without warnings
    @retval false : Validation failed. Error is raised.
*/

bool check_secure_file_priv_path()
{
  char datadir_buffer[FN_REFLEN+1]={0};
  char plugindir_buffer[FN_REFLEN+1]={0};
  char whichdir[20]= {0};
  size_t opt_plugindir_len= 0;
  size_t opt_datadir_len= 0;
  size_t opt_secure_file_priv_len= 0;
  bool warn= false;
  bool case_insensitive_fs;
#ifndef _WIN32
  MY_STAT dir_stat;
#endif

  if (!opt_secure_file_priv[0])
  {
    if (opt_bootstrap)
    {
      /*
        Do not impose --secure-file-priv restriction
        in --bootstrap mode
      */
      sql_print_information("Ignoring --secure-file-priv value as server is "
                            "running with --bootstrap.");
    }
    else
    {
      sql_print_warning("Insecure configuration for --secure-file-priv: "
                        "Current value does not restrict location of generated "
                        "files. Consider setting it to a valid, "
                        "non-empty path.");
    }
    return true;
  }

  /*
    Setting --secure-file-priv to NULL would disable
    reading/writing from/to file
  */
  if(!my_strcasecmp(system_charset_info, opt_secure_file_priv, "NULL"))
  {
    sql_print_information("--secure-file-priv is set to NULL. "
                          "Operations related to importing and exporting "
                          "data are disabled");
    return true;
  }

  /*
    Check if --secure-file-priv can access data directory
  */
  opt_secure_file_priv_len= strlen(opt_secure_file_priv);

  /*
    Adds dir seperator at the end.
    This is required in subsequent comparison
  */
  convert_dirname(datadir_buffer, mysql_unpacked_real_data_home, NullS);
  opt_datadir_len= strlen(datadir_buffer);

  case_insensitive_fs=
    (test_if_case_insensitive(datadir_buffer) == 1);

  if (!case_insensitive_fs)
  {
    if (!strncmp(datadir_buffer, opt_secure_file_priv,
          opt_datadir_len < opt_secure_file_priv_len ?
          opt_datadir_len : opt_secure_file_priv_len))
    {
      warn= true;
      strcpy(whichdir, "Data directory");
    }
  }
  else
  {
    if (!files_charset_info->coll->strnncoll(files_charset_info,
          (uchar *) datadir_buffer,
          opt_datadir_len,
          (uchar *) opt_secure_file_priv,
          opt_secure_file_priv_len,
          TRUE))
    {
      warn= true;
      strcpy(whichdir, "Data directory");
    }
  }

  /*
    Don't bother comparing --secure-file-priv with --plugin-dir
    if we already have a match against --datadir or
    --plugin-dir is not pointing to a valid directory.
  */
  if (!warn && !my_realpath(plugindir_buffer, opt_plugin_dir, 0))
  {
    convert_dirname(plugindir_buffer, plugindir_buffer, NullS);
    opt_plugindir_len= strlen(plugindir_buffer);

    if (!case_insensitive_fs)
    {
      if (!strncmp(plugindir_buffer, opt_secure_file_priv,
          opt_plugindir_len < opt_secure_file_priv_len ?
          opt_plugindir_len : opt_secure_file_priv_len))
      {
        warn= true;
        strcpy(whichdir, "Plugin directory");
      }
    }
    else
    {
      if (!files_charset_info->coll->strnncoll(files_charset_info,
          (uchar *) plugindir_buffer,
          opt_plugindir_len,
          (uchar *) opt_secure_file_priv,
          opt_secure_file_priv_len,
          TRUE))
      {
        warn= true;
        strcpy(whichdir, "Plugin directory");
      }
    }
  }


  if (warn)
    sql_print_warning("Insecure configuration for --secure-file-priv: "
                      "%s is accessible through "
                      "--secure-file-priv. Consider choosing a different "
                      "directory.", whichdir);

#ifndef _WIN32
  /*
     Check for --secure-file-priv directory's permission
  */
  if (!(my_stat(opt_secure_file_priv, &dir_stat, MYF(0))))
  {
    sql_print_error("Failed to get stat for directory pointed out "
                    "by --secure-file-priv");
    return false;
  }

  if (dir_stat.st_mode & S_IRWXO)
    sql_print_warning("Insecure configuration for --secure-file-priv: "
                      "Location is accessible to all OS users. "
                      "Consider choosing a different directory.");
#endif
  return true;
}

static int fix_paths(void)
{
  char buff[FN_REFLEN],*pos;
  bool secure_file_priv_nonempty= false;
  convert_dirname(mysql_home,mysql_home,NullS);
  /* Resolve symlinks to allow 'mysql_home' to be a relative symlink */
  my_realpath(mysql_home,mysql_home,MYF(0));
  /* Ensure that mysql_home ends in FN_LIBCHAR */
  pos=strend(mysql_home);
  if (pos[-1] != FN_LIBCHAR)
  {
    pos[0]= FN_LIBCHAR;
    pos[1]= 0;
  }
  convert_dirname(lc_messages_dir, lc_messages_dir, NullS);
  convert_dirname(mysql_real_data_home,mysql_real_data_home,NullS);
  (void) my_load_path(mysql_home,mysql_home,""); // Resolve current dir
  (void) my_load_path(mysql_real_data_home,mysql_real_data_home,mysql_home);
  (void) my_load_path(pidfile_name, pidfile_name_ptr, mysql_real_data_home);

  /* Generate shutdown file name after the pid full-path name is available */
  generate_shutdownfile_name();

  convert_dirname(opt_plugin_dir, opt_plugin_dir_ptr ? opt_plugin_dir_ptr :
                                  get_relative_path(PLUGINDIR), NullS);
  (void) my_load_path(opt_plugin_dir, opt_plugin_dir, mysql_home);
  opt_plugin_dir_ptr= opt_plugin_dir;

  my_realpath(mysql_unpacked_real_data_home, mysql_real_data_home, MYF(0));
  mysql_unpacked_real_data_home_len=
    (int) strlen(mysql_unpacked_real_data_home);
  if (mysql_unpacked_real_data_home[mysql_unpacked_real_data_home_len-1] == FN_LIBCHAR)
    --mysql_unpacked_real_data_home_len;

  char *sharedir=get_relative_path(SHAREDIR);
  if (test_if_hard_path(sharedir))
    strmake(buff,sharedir,sizeof(buff)-1);    /* purecov: tested */
  else
    strxnmov(buff,sizeof(buff)-1,mysql_home,sharedir,NullS);
  convert_dirname(buff,buff,NullS);
  (void) my_load_path(lc_messages_dir, lc_messages_dir, buff);

  /* If --character-sets-dir isn't given, use shared library dir */
  if (charsets_dir)
    strmake(mysql_charsets_dir, charsets_dir, sizeof(mysql_charsets_dir)-1);
  else
    strxnmov(mysql_charsets_dir, sizeof(mysql_charsets_dir)-1, buff,
       CHARSET_DIR, NullS);
  (void) my_load_path(mysql_charsets_dir, mysql_charsets_dir, buff);
  convert_dirname(mysql_charsets_dir, mysql_charsets_dir, NullS);
  charsets_dir=mysql_charsets_dir;

  if (init_tmpdir(&mysql_tmpdir_list, opt_mysql_tmpdir))
    return 1;
  if (!opt_mysql_tmpdir)
    opt_mysql_tmpdir= mysql_tmpdir;
#ifdef HAVE_REPLICATION
  if (!slave_load_tmpdir)
    slave_load_tmpdir= mysql_tmpdir;
#endif /* HAVE_REPLICATION */
  /*
    Convert the secure-file-priv option to system format, allowing
    a quick strcmp to check if read or write is in an allowed dir
  */
  if (opt_bootstrap)
    opt_secure_file_priv= EMPTY_STR.str;
  secure_file_priv_nonempty= opt_secure_file_priv[0] ? true : false;

  if (secure_file_priv_nonempty && strlen(opt_secure_file_priv) > FN_REFLEN)
  {
    sql_print_warning("Value for --secure-file-priv is longer than maximum "
                      "limit of %d", FN_REFLEN-1);
    return 1;
  }

  memset(buff, 0, sizeof(buff));
  if (secure_file_priv_nonempty &&
      my_strcasecmp(system_charset_info, opt_secure_file_priv, "NULL"))
  {
    int retval= my_realpath(buff, opt_secure_file_priv, MYF(MY_WME));
    if (!retval)
    {
      convert_dirname(secure_file_real_path, buff, NullS);
#ifdef WIN32
      MY_DIR *dir= my_dir(secure_file_real_path, MYF(MY_DONT_SORT+MY_WME));
      if (!dir)
      {
        retval= 1;
      }
      else
      {
        my_dirend(dir);
      }
#endif
    }

    if (retval)
    {
      char err_buffer[FN_REFLEN];
      my_snprintf(err_buffer, FN_REFLEN-1,
                  "Failed to access directory for --secure-file-priv."
                  " Please make sure that directory exists and is "
                  "accessible by MySQL Server. Supplied value : %s",
                  opt_secure_file_priv);
      err_buffer[FN_REFLEN-1]='\0';
      sql_print_error("%s", err_buffer);
      return 1;
    }
    opt_secure_file_priv= secure_file_real_path;
  }

  if (!check_secure_file_priv_path())
    return 1;

  return 0;
}

/**
  Check if file system used for databases is case insensitive.

  @param dir_name     Directory to test

  @retval
    -1  Don't know (Test failed)
  @retval
    0   File system is case sensitive
  @retval
    1   File system is case insensitive
*/

static int test_if_case_insensitive(const char *dir_name)
{
  int result= 0;
  File file;
  char buff[FN_REFLEN], buff2[FN_REFLEN];
  MY_STAT stat_info;
  DBUG_ENTER("test_if_case_insensitive");

  fn_format(buff, glob_hostname, dir_name, ".lower-test",
      MY_UNPACK_FILENAME | MY_REPLACE_EXT | MY_REPLACE_DIR);
  fn_format(buff2, glob_hostname, dir_name, ".LOWER-TEST",
      MY_UNPACK_FILENAME | MY_REPLACE_EXT | MY_REPLACE_DIR);
  mysql_file_delete(key_file_casetest, buff2, MYF(0));
  if ((file= mysql_file_create(key_file_casetest,
                               buff, 0666, O_RDWR, MYF(0))) < 0)
  {
    sql_print_warning("Can't create test file %s", buff);
    DBUG_RETURN(-1);
  }
  mysql_file_close(file, MYF(0));
  if (mysql_file_stat(key_file_casetest, buff2, &stat_info, MYF(0)))
    result= 1;          // Can access file
  mysql_file_delete(key_file_casetest, buff, MYF(MY_WME));
  DBUG_PRINT("exit", ("result: %d", result));
  DBUG_RETURN(result);
}


#ifndef EMBEDDED_LIBRARY

/**
  Create file to store pid number.
*/
static void create_pid_file()
{
  File file;
  if ((file= mysql_file_create(key_file_pid, pidfile_name, 0664,
                               O_WRONLY | O_TRUNC, MYF(MY_WME))) >= 0)
  {
    char buff[MAX_BIGINT_WIDTH + 1], *end;
    end= int10_to_str((long) getpid(), buff, 10);
    *end++= '\n';
    if (!mysql_file_write(file, (uchar*) buff, (uint) (end-buff),
                          MYF(MY_WME | MY_NABP)))
    {
      mysql_file_close(file, MYF(0));
      pid_file_created= true;
      return;
    }
    mysql_file_close(file, MYF(0));
  }
  sql_perror("Can't start server: can't create PID file");
  exit(1);
}

/**
  Create shutdown file.
*/
static void create_shutdown_file()
{
  File file;
  if ((file= mysql_file_create(key_file_shutdown, shutdownfile_name, 0664,
                               O_WRONLY | O_TRUNC, MYF(MY_WME))) >= 0)
  {
    mysql_file_close(file, MYF(0));
  }
  else
  {
    /* Log the error and continue */
    sql_perror("Can't create SHUTDOWN file.\n");
  }
}

/**
  Delete shutdown file. 0 means successful.
*/
static int delete_shutdown_file()
{
  /* If it is bootstrap or the shutdown file doesn't exist */
  if (opt_bootstrap ||
      access(shutdownfile_name, F_OK))
  {
    return 0;
  }
  /* 0 will be returned if shutdown file was deleted successfully */
  return mysql_file_delete(key_file_shutdown, shutdownfile_name,
                           MYF(MY_WME));
}

#endif /* EMBEDDED_LIBRARY */


/**
  Remove the process' pid file.

  @param  flags  file operation flags
*/

void delete_pid_file(myf flags)
{
#ifndef EMBEDDED_LIBRARY
  File file;
  if (opt_bootstrap ||
      !pid_file_created ||
      !(file= mysql_file_open(key_file_pid, pidfile_name,
                              O_RDONLY, flags)))
    return;

  /* Make sure that the pid file was created by the same process. */
  uchar buff[MAX_BIGINT_WIDTH + 1];
  size_t error= mysql_file_read(file, buff, sizeof(buff), flags);
  mysql_file_close(file, flags);
  buff[sizeof(buff) - 1]= '\0';
  if (error != MY_FILE_ERROR &&
      atol((char *) buff) == (long) getpid())
  {
    mysql_file_delete(key_file_pid, pidfile_name, flags);
    pid_file_created= false;
  }
#endif /* EMBEDDED_LIBRARY */
  return;
}


/** Clear most status variables. */
void refresh_status(THD *thd)
{
  mysql_mutex_lock(&LOCK_status);

  /* Add thread's status variabes to global status */
  add_to_status(&global_status_var, &thd->status_var);

  /* Reset thread's status variables */
  thd->refresh_status_vars();

  /* Reset some global variables */
  reset_status_vars();

  /* Reset the counters of all key caches (default and named). */
  process_key_caches(reset_key_cache_counters);
  flush_status_time= time((time_t*) 0);
  mysql_mutex_unlock(&LOCK_status);

  /*
    Set max_used_connections to the number of currently open
    connections.  Lock LOCK_thread_count out of LOCK_status to avoid
    deadlocks.  Status reset becomes not atomic, but status data is
    not exact anyway.
  */
  mysql_mutex_lock(&LOCK_thread_count);
  max_used_connections= get_thread_count() - delayed_insert_threads;
  mysql_mutex_unlock(&LOCK_thread_count);
}


/*****************************************************************************
  Instantiate variables for missing storage engines
  This section should go away soon
*****************************************************************************/

#ifdef HAVE_PSI_INTERFACE
#ifdef HAVE_MMAP
PSI_mutex_key key_PAGE_lock, key_LOCK_sync, key_LOCK_active, key_LOCK_pool;
#endif /* HAVE_MMAP */

#ifdef HAVE_OPENSSL
PSI_mutex_key key_LOCK_des_key_file;
PSI_rwlock_key key_rwlock_LOCK_use_ssl;
#endif /* HAVE_OPENSSL */

PSI_mutex_key key_BINLOG_LOCK_commit;
PSI_mutex_key key_BINLOG_LOCK_commit_queue;
PSI_mutex_key key_BINLOG_LOCK_semisync;
PSI_mutex_key key_BINLOG_LOCK_semisync_queue;
PSI_mutex_key key_BINLOG_LOCK_done;
PSI_mutex_key key_BINLOG_LOCK_flush_queue;
PSI_mutex_key key_BINLOG_LOCK_index;
PSI_mutex_key key_BINLOG_LOCK_log;
PSI_mutex_key key_BINLOG_LOCK_sync;
PSI_mutex_key key_BINLOG_LOCK_sync_queue;
PSI_mutex_key key_BINLOG_LOCK_xids;
PSI_mutex_key key_BINLOG_LOCK_binlog_end_pos;
PSI_mutex_key key_BINLOG_LOCK_non_xid_trxs;
PSI_mutex_key key_commit_order_manager_mutex;
PSI_mutex_key
  key_delayed_insert_mutex, key_hash_filo_lock, key_LOCK_active_mi,
  key_LOCK_connection_count, key_LOCK_crypt, key_LOCK_delayed_create,
  key_LOCK_delayed_insert, key_LOCK_delayed_status, key_LOCK_error_log,
  key_LOCK_gdl, key_LOCK_global_system_variables,
  key_LOCK_manager,
  key_LOCK_slave_stats_daemon,
  key_LOCK_prepared_stmt_count,
  key_LOCK_sql_slave_skip_counter,
  key_LOCK_slave_net_timeout,
  key_LOCK_server_started, key_LOCK_status,
  key_LOCK_system_variables_hash, key_LOCK_table_share, key_LOCK_thd_data,
  key_LOCK_thd_db_read_only_hash,
  key_LOCK_db_metadata, key_LOCK_thd_audit_data,
  key_LOCK_user_conn, key_LOCK_uuid_generator, key_LOG_LOCK_log,
  key_master_info_data_lock, key_master_info_run_lock,
  key_master_info_sleep_lock, key_master_info_thd_lock,
  key_master_info_fde_lock,
  key_mutex_slave_reporting_capability_err_lock, key_relay_log_info_data_lock,
  key_relay_log_info_sleep_lock, key_relay_log_info_thd_lock,
  key_relay_log_info_log_space_lock, key_relay_log_info_run_lock,
  key_mutex_slave_parallel_pend_jobs, key_mutex_mts_temp_tables_lock,
  key_mutex_slave_parallel_worker_count,
  key_mutex_slave_parallel_worker,
  key_structure_guard_mutex, key_TABLE_SHARE_LOCK_ha_data,
  key_LOCK_error_messages, key_LOG_INFO_lock, key_LOCK_thread_count,
  key_LOCK_global_table_stats,
  key_LOCK_global_sql_stats,
  key_LOCK_global_sql_plans,
  key_LOCK_global_active_sql,
  key_LOCK_global_sql_findings,
  key_LOCK_global_write_statistics,
  key_LOCK_global_write_throttling_rules,
  key_LOCK_global_write_throttling_log,
  key_LOCK_global_tx_size_histogram,
  key_LOCK_global_write_stat_histogram,
  key_LOCK_replication_lag_auto_throttling,
  key_LOCK_log_throttle_qni,
  key_LOCK_log_throttle_legacy,
  key_LOCK_log_throttle_ddl,
  key_gtid_info_run_lock,
  key_gtid_info_data_lock,
  key_gtid_info_sleep_lock,
  key_gtid_info_thd_lock,
  key_USER_CONN_LOCK_user_table_stats;
PSI_mutex_key key_LOCK_thd_remove;
PSI_mutex_key key_RELAYLOG_LOCK_commit;
PSI_mutex_key key_RELAYLOG_LOCK_commit_queue;
PSI_mutex_key key_RELAYLOG_LOCK_semisync;
PSI_mutex_key key_RELAYLOG_LOCK_semisync_queue;
PSI_mutex_key key_RELAYLOG_LOCK_done;
PSI_mutex_key key_RELAYLOG_LOCK_flush_queue;
PSI_mutex_key key_RELAYLOG_LOCK_index;
PSI_mutex_key key_RELAYLOG_LOCK_log;
PSI_mutex_key key_RELAYLOG_LOCK_sync;
PSI_mutex_key key_RELAYLOG_LOCK_sync_queue;
PSI_mutex_key key_RELAYLOG_LOCK_xids;
PSI_mutex_key key_RELAYLOG_LOCK_non_xid_trxs;
PSI_mutex_key key_RELAYLOG_LOCK_binlog_end_pos;
PSI_mutex_key key_LOCK_sql_rand;
PSI_mutex_key key_gtid_ensure_index_mutex;
PSI_mutex_key key_hlc_wait_mutex;
PSI_mutex_key key_LOCK_thread_created;
PSI_mutex_key key_LOCK_log_throttle_sbr_unsafe;
PSI_mutex_key key_LOCK_ac_node;
PSI_mutex_key key_LOCK_ac_info;

#ifdef HAVE_MY_TIMER
PSI_mutex_key key_thd_timer_mutex;
#endif


static PSI_mutex_info all_server_mutexes[]=
{
#ifdef HAVE_MMAP
  { &key_PAGE_lock, "PAGE::lock", 0},
  { &key_LOCK_sync, "TC_LOG_MMAP::LOCK_sync", 0},
  { &key_LOCK_active, "TC_LOG_MMAP::LOCK_active", 0},
  { &key_LOCK_pool, "TC_LOG_MMAP::LOCK_pool", 0},
#endif /* HAVE_MMAP */

#ifdef HAVE_OPENSSL
  { &key_LOCK_des_key_file, "LOCK_des_key_file", PSI_FLAG_GLOBAL},
#endif /* HAVE_OPENSSL */

  { &key_BINLOG_LOCK_commit, "MYSQL_BIN_LOG::LOCK_commit", 0 },
  { &key_BINLOG_LOCK_commit_queue, "MYSQL_BIN_LOG::LOCK_commit_queue", 0 },
  { &key_BINLOG_LOCK_semisync, "MYSQL_BIN_LOG::LOCK_semisync", 0 },
  { &key_BINLOG_LOCK_semisync_queue, "MYSQL_BIN_LOG::LOCK_semisync_queue", 0 },
  { &key_BINLOG_LOCK_done, "MYSQL_BIN_LOG::LOCK_done", 0 },
  { &key_BINLOG_LOCK_flush_queue, "MYSQL_BIN_LOG::LOCK_flush_queue", 0 },
  { &key_BINLOG_LOCK_index, "MYSQL_BIN_LOG::LOCK_index", 0},
  { &key_BINLOG_LOCK_log, "MYSQL_BIN_LOG::LOCK_log", 0},
  { &key_BINLOG_LOCK_sync, "MYSQL_BIN_LOG::LOCK_sync", 0},
  { &key_BINLOG_LOCK_sync_queue, "MYSQL_BIN_LOG::LOCK_sync_queue", 0 },
  { &key_BINLOG_LOCK_xids, "MYSQL_BIN_LOG::LOCK_xids", 0 },
  { &key_BINLOG_LOCK_non_xid_trxs, "MYSQL_BIN_LOG::LOCK_non_xid_trxs", 0 },
  { &key_BINLOG_LOCK_binlog_end_pos, "MYSQL_BIN_LOG::LOCK_binlog_end_pos", 0 },
  { &key_commit_order_manager_mutex, "Commit_order_manager::m_mutex", 0 },
  { &key_RELAYLOG_LOCK_commit, "MYSQL_RELAY_LOG::LOCK_commit", 0},
  { &key_RELAYLOG_LOCK_commit_queue, "MYSQL_RELAY_LOG::LOCK_commit_queue", 0 },
  { &key_RELAYLOG_LOCK_done, "MYSQL_RELAY_LOG::LOCK_done", 0 },
  { &key_RELAYLOG_LOCK_flush_queue, "MYSQL_RELAY_LOG::LOCK_flush_queue", 0 },
  { &key_RELAYLOG_LOCK_index, "MYSQL_RELAY_LOG::LOCK_index", 0},
  { &key_RELAYLOG_LOCK_log, "MYSQL_RELAY_LOG::LOCK_log", 0},
  { &key_RELAYLOG_LOCK_sync, "MYSQL_RELAY_LOG::LOCK_sync", 0},
  { &key_RELAYLOG_LOCK_sync_queue, "MYSQL_RELAY_LOG::LOCK_sync_queue", 0 },
  { &key_RELAYLOG_LOCK_xids, "MYSQL_RELAY_LOG::LOCK_xids", 0},
  { &key_RELAYLOG_LOCK_non_xid_trxs, "MYSQL_RELAY_LOG::LOCK_xids", 0},
  { &key_RELAYLOG_LOCK_binlog_end_pos,
    "MYSQL_RELAY_LOG::LOCK_binlog_end_pos", 0},
  { &key_delayed_insert_mutex, "Delayed_insert::mutex", 0},
  { &key_hash_filo_lock, "hash_filo::lock", 0},
  { &key_LOCK_active_mi, "LOCK_active_mi", PSI_FLAG_GLOBAL},
  { &key_LOCK_connection_count, "LOCK_connection_count", PSI_FLAG_GLOBAL},
  { &key_LOCK_crypt, "LOCK_crypt", PSI_FLAG_GLOBAL},
  { &key_LOCK_delayed_create, "LOCK_delayed_create", PSI_FLAG_GLOBAL},
  { &key_LOCK_delayed_insert, "LOCK_delayed_insert", PSI_FLAG_GLOBAL},
  { &key_LOCK_delayed_status, "LOCK_delayed_status", PSI_FLAG_GLOBAL},
  { &key_LOCK_error_log, "LOCK_error_log", PSI_FLAG_GLOBAL},
  { &key_LOCK_gdl, "LOCK_gdl", PSI_FLAG_GLOBAL},
  { &key_LOCK_global_system_variables, "LOCK_global_system_variables", PSI_FLAG_GLOBAL},
  { &key_LOCK_manager, "LOCK_manager", PSI_FLAG_GLOBAL},
  { &key_LOCK_slave_stats_daemon, "LOCK_slave_stats_daemon", PSI_FLAG_GLOBAL},
  { &key_LOCK_prepared_stmt_count, "LOCK_prepared_stmt_count", PSI_FLAG_GLOBAL},
  { &key_LOCK_sql_slave_skip_counter, "LOCK_sql_slave_skip_counter", PSI_FLAG_GLOBAL},
  { &key_LOCK_slave_net_timeout, "LOCK_slave_net_timeout", PSI_FLAG_GLOBAL},
  { &key_LOCK_server_started, "LOCK_server_started", PSI_FLAG_GLOBAL},
  { &key_LOCK_status, "LOCK_status", PSI_FLAG_GLOBAL},
  { &key_LOCK_system_variables_hash, "LOCK_system_variables_hash", PSI_FLAG_GLOBAL},
  { &key_LOCK_table_share, "LOCK_table_share", PSI_FLAG_GLOBAL},
  { &key_LOCK_thd_data, "THD::LOCK_thd_data", 0},
  { &key_LOCK_thd_db_read_only_hash, "THD::LOCK_thd_db_read_only_hash", 0},
  { &key_LOCK_db_metadata, "THD::LOCK_db_metadata", 0},
  { &key_LOCK_user_conn, "LOCK_user_conn", PSI_FLAG_GLOBAL},
  { &key_LOCK_uuid_generator, "LOCK_uuid_generator", PSI_FLAG_GLOBAL},
  { &key_LOCK_sql_rand, "LOCK_sql_rand", PSI_FLAG_GLOBAL},
  { &key_LOG_LOCK_log, "LOG::LOCK_log", 0},
  { &key_master_info_data_lock, "Master_info::data_lock", 0},
  { &key_master_info_run_lock, "Master_info::run_lock", 0},
  { &key_master_info_sleep_lock, "Master_info::sleep_lock", 0},
  { &key_master_info_thd_lock, "Master_info::info_thd_lock", 0},
  { &key_master_info_fde_lock, "Master_info::fde_lock", 0},
  { &key_mutex_slave_reporting_capability_err_lock, "Slave_reporting_capability::err_lock", 0},
  { &key_relay_log_info_data_lock, "Relay_log_info::data_lock", 0},
  { &key_relay_log_info_sleep_lock, "Relay_log_info::sleep_lock", 0},
  { &key_relay_log_info_thd_lock, "Relay_log_info::info_thd_lock", 0},
  { &key_relay_log_info_log_space_lock, "Relay_log_info::log_space_lock", 0},
  { &key_relay_log_info_run_lock, "Relay_log_info::run_lock", 0},
  { &key_mutex_slave_parallel_pend_jobs, "Relay_log_info::pending_jobs_lock", 0},
  { &key_mutex_slave_parallel_worker_count, "Relay_log_info::exit_count_lock", 0},
  { &key_mutex_mts_temp_tables_lock, "Relay_log_info::temp_tables_lock", 0},
  { &key_mutex_slave_parallel_worker, "Worker_info::jobs_lock", 0},
  { &key_structure_guard_mutex, "Query_cache::structure_guard_mutex", 0},
  { &key_TABLE_SHARE_LOCK_ha_data, "TABLE_SHARE::LOCK_ha_data", 0},
  { &key_LOCK_error_messages, "LOCK_error_messages", PSI_FLAG_GLOBAL},
  { &key_LOG_INFO_lock, "LOG_INFO::lock", 0},
  { &key_LOCK_thread_count, "LOCK_thread_count", PSI_FLAG_GLOBAL},
  { &key_LOCK_thd_remove, "LOCK_thd_remove", PSI_FLAG_GLOBAL},
  { &key_LOCK_log_throttle_qni, "LOCK_log_throttle_qni", PSI_FLAG_GLOBAL},
  { &key_LOCK_log_throttle_legacy, "LOCK_log_throttle_legacy", PSI_FLAG_GLOBAL},
  { &key_LOCK_log_throttle_ddl, "LOCK_log_throttle_ddl", PSI_FLAG_GLOBAL},
  { &key_LOCK_global_table_stats, "LOCK_global_table_stats", PSI_FLAG_GLOBAL},
  { &key_LOCK_global_sql_stats, "LOCK_global_sql_stats", PSI_FLAG_GLOBAL},
  { &key_LOCK_global_sql_plans, "LOCK_global_sql_plans", PSI_FLAG_GLOBAL},
  { &key_LOCK_global_active_sql, "LOCK_global_active_sql", PSI_FLAG_GLOBAL},
  { &key_LOCK_global_sql_findings, "LOCK_global_sql_findings", PSI_FLAG_GLOBAL},
  { &key_LOCK_global_write_statistics, "LOCK_global_write_statistics", PSI_FLAG_GLOBAL},
  { &key_LOCK_global_write_throttling_rules, "LOCK_global_write_throttling_rules", PSI_FLAG_GLOBAL},
  { &key_LOCK_global_write_throttling_log, "LOCK_global_write_throttling_log", PSI_FLAG_GLOBAL},
  { &key_LOCK_global_tx_size_histogram, "LOCK_global_tx_size_histogram", PSI_FLAG_GLOBAL},
  { &key_LOCK_global_write_stat_histogram, "LOCK_global_write_stat_histogram", PSI_FLAG_GLOBAL},
  { &key_LOCK_replication_lag_auto_throttling, "LOCK_replication_lag_auto_throttling", PSI_FLAG_GLOBAL},
  { &key_gtid_ensure_index_mutex, "Gtid_state", PSI_FLAG_GLOBAL},
  { &key_hlc_wait_mutex, "HybridLogicalClock", PSI_FLAG_GLOBAL},
  { &key_LOCK_thread_created, "LOCK_thread_created", PSI_FLAG_GLOBAL },
#ifdef HAVE_MY_TIMER
  { &key_thd_timer_mutex, "thd_timer_mutex", 0},
#endif
  { &key_gtid_info_run_lock, "Gtid_info::run_lock", 0},
  { &key_gtid_info_data_lock, "Gtid_info::data_lock", 0},
  { &key_gtid_info_sleep_lock, "Gtid_info::sleep_lock", 0},
  { &key_LOCK_log_throttle_sbr_unsafe, "LOCK_log_throttle_sbr_unsafe", PSI_FLAG_GLOBAL},
  { &key_USER_CONN_LOCK_user_table_stats, "USER_CONN::LOCK_user_table_stats", 0},
  { &key_LOCK_ac_node, "st_ac_node::lock", 0},
  { &key_LOCK_ac_info, "Ac_info::lock", 0},
};

PSI_rwlock_key key_rwlock_LOCK_column_statistics, key_rwlock_LOCK_grant,
  key_rwlock_LOCK_logger, key_rwlock_LOCK_sys_init_connect,
  key_rwlock_LOCK_sys_init_slave, key_rwlock_LOCK_system_variables_hash,
  key_rwlock_query_cache_query_lock, key_rwlock_global_sid_lock,
  key_rwlock_LOCK_gap_lock_exceptions,
  key_rwlock_LOCK_legacy_user_name_pattern,
  key_rwlock_LOCK_admin_users_list_regex,
  key_rwlock_NAME_ID_MAP_LOCK_name_id_map,
  key_rwlock_sql_stats_snapshot;

PSI_rwlock_key key_rwlock_Trans_delegate_lock;
PSI_rwlock_key key_rwlock_Binlog_storage_delegate_lock;
PSI_rwlock_key key_rwlock_Raft_replication_delegate_lock;
#ifdef HAVE_REPLICATION
PSI_rwlock_key key_rwlock_Binlog_transmit_delegate_lock;
PSI_rwlock_key key_rwlock_Binlog_relay_IO_delegate_lock;
#endif

PSI_rwlock_key key_rwlock_hash_filo;
PSI_rwlock_key key_rwlock_LOCK_ac;

static PSI_rwlock_info all_server_rwlocks[]=
{
#ifdef HAVE_REPLICATION
  { &key_rwlock_Binlog_transmit_delegate_lock, "Binlog_transmit_delegate::lock", PSI_FLAG_GLOBAL},
  { &key_rwlock_Binlog_relay_IO_delegate_lock, "Binlog_relay_IO_delegate::lock", PSI_FLAG_GLOBAL},
#endif
#ifdef HAVE_OPENSSL
  { &key_rwlock_LOCK_use_ssl, "LOCK_use_ssl", PSI_FLAG_GLOBAL},
#endif /* HAVE_OPENSSL */
  { &key_rwlock_LOCK_column_statistics, "LOCK_column_statistics", PSI_FLAG_GLOBAL},
  { &key_rwlock_LOCK_grant, "LOCK_grant", PSI_FLAG_GLOBAL},
  { &key_rwlock_LOCK_logger, "LOGGER::LOCK_logger", 0},
  { &key_rwlock_LOCK_sys_init_connect, "LOCK_sys_init_connect", PSI_FLAG_GLOBAL},
  { &key_rwlock_LOCK_sys_init_slave, "LOCK_sys_init_slave", PSI_FLAG_GLOBAL},
  { &key_rwlock_LOCK_gap_lock_exceptions, "LOCK_gap_lock_exceptions", PSI_FLAG_GLOBAL},
  { &key_rwlock_LOCK_legacy_user_name_pattern, "LOCK_legacy_user_name_pattern", PSI_FLAG_GLOBAL},
  { &key_rwlock_LOCK_admin_users_list_regex, "LOCK_admin_users_list_regex", PSI_FLAG_GLOBAL},
  { &key_rwlock_NAME_ID_MAP_LOCK_name_id_map, "NAME_ID_MAP::LOCK_name_id_map", 0},
  { &key_rwlock_LOCK_system_variables_hash, "LOCK_system_variables_hash", PSI_FLAG_GLOBAL},
  { &key_rwlock_query_cache_query_lock, "Query_cache_query::lock", 0},
  { &key_rwlock_global_sid_lock, "gtid_commit_rollback", PSI_FLAG_GLOBAL},
  { &key_rwlock_Trans_delegate_lock, "Trans_delegate::lock", PSI_FLAG_GLOBAL},
  { &key_rwlock_Binlog_storage_delegate_lock, "Binlog_storage_delegate::lock", PSI_FLAG_GLOBAL},
  { &key_rwlock_hash_filo, "LOCK_key_hash_filo_rwlock", PSI_FLAG_GLOBAL},
  { &key_rwlock_sql_stats_snapshot, "LOCK_sql_stats_snapshot", PSI_FLAG_GLOBAL},
#if (defined(_WIN32) || defined(HAVE_SMEM)) && !defined(EMBEDDED_LIBRARY)
  { &key_rwlock_LOCK_named_pipe_full_access_group, "LOCK_named_pipe_full_access_group", PSI_FLAG_GLOBAL},
#endif /* _WIN32 || HAVE_SMEM && !EMBEDDED_LIBRARY */
  { &key_rwlock_Raft_replication_delegate_lock,
    "Raft_replication_delegate::lock", PSI_FLAG_GLOBAL},
  { &key_rwlock_LOCK_ac, "AC::rwlock", 0},
};

#ifdef HAVE_MMAP
PSI_cond_key key_PAGE_cond, key_COND_active, key_COND_pool;
#endif /* HAVE_MMAP */

PSI_cond_key key_BINLOG_update_cond,
  key_COND_cache_status_changed, key_COND_manager, key_COND_slave_stats_daemon,
  key_COND_server_started,
  key_delayed_insert_cond, key_delayed_insert_cond_client,
  key_item_func_sleep_cond, key_master_info_data_cond,
  key_master_info_start_cond, key_master_info_stop_cond,
  key_master_info_sleep_cond,
  key_relay_log_info_data_cond, key_relay_log_info_log_space_cond,
  key_relay_log_info_start_cond, key_relay_log_info_stop_cond,
  key_relay_log_info_sleep_cond, key_cond_slave_parallel_pend_jobs,
  key_cond_slave_parallel_worker,
  key_TABLE_SHARE_cond, key_user_level_lock_cond,
  key_COND_thread_count, key_COND_thread_cache, key_COND_flush_thread_cache,
  key_gtid_info_data_cond, key_gtid_info_start_cond, key_gtid_info_stop_cond,
  key_gtid_info_sleep_cond,
  key_COND_connection_count;
PSI_cond_key key_RELAYLOG_update_cond;
PSI_cond_key key_BINLOG_COND_done;
PSI_cond_key key_RELAYLOG_COND_done;
PSI_cond_key key_BINLOG_prep_xids_cond;
PSI_cond_key key_RELAYLOG_prep_xids_cond;
PSI_cond_key key_BINLOG_non_xid_trxs_cond;
PSI_cond_key key_RELAYLOG_non_xid_trxs_cond;
PSI_cond_key key_gtid_ensure_index_cond;
PSI_cond_key key_hlc_wait_cond;
PSI_cond_key key_commit_order_manager_cond;
PSI_cond_key key_COND_ac_node;

static PSI_cond_info all_server_conds[]=
{
#if (defined(_WIN32) || defined(HAVE_SMEM)) && !defined(EMBEDDED_LIBRARY)
  { &key_COND_handler_count, "COND_handler_count", PSI_FLAG_GLOBAL},
#endif /* _WIN32 || HAVE_SMEM && !EMBEDDED_LIBRARY */
#ifdef HAVE_MMAP
  { &key_PAGE_cond, "PAGE::cond", 0},
  { &key_COND_active, "TC_LOG_MMAP::COND_active", 0},
  { &key_COND_pool, "TC_LOG_MMAP::COND_pool", 0},
#endif /* HAVE_MMAP */
  { &key_BINLOG_COND_done, "MYSQL_BIN_LOG::COND_done", 0},
  { &key_BINLOG_update_cond, "MYSQL_BIN_LOG::update_cond", 0},
  { &key_BINLOG_prep_xids_cond, "MYSQL_BIN_LOG::prep_xids_cond", 0},
  { &key_BINLOG_non_xid_trxs_cond, "MYSQL_BIN_LOG::non_xid_trxs_cond", 0},
  { &key_RELAYLOG_COND_done, "MYSQL_RELAY_LOG::COND_done", 0},
  { &key_RELAYLOG_update_cond, "MYSQL_RELAY_LOG::update_cond", 0},
  { &key_RELAYLOG_prep_xids_cond, "MYSQL_RELAY_LOG::prep_xids_cond", 0},
  { &key_RELAYLOG_non_xid_trxs_cond, "MYSQL_RELAY_LOG::non_xid_trxs_cond", 0},
  { &key_COND_cache_status_changed, "Query_cache::COND_cache_status_changed", 0},
  { &key_COND_manager, "COND_manager", PSI_FLAG_GLOBAL},
  { &key_COND_slave_stats_daemon, "COND_slave_stats_daemon", PSI_FLAG_GLOBAL},
  { &key_COND_server_started, "COND_server_started", PSI_FLAG_GLOBAL},
  { &key_delayed_insert_cond, "Delayed_insert::cond", 0},
  { &key_delayed_insert_cond_client, "Delayed_insert::cond_client", 0},
  { &key_item_func_sleep_cond, "Item_func_sleep::cond", 0},
  { &key_master_info_data_cond, "Master_info::data_cond", 0},
  { &key_master_info_start_cond, "Master_info::start_cond", 0},
  { &key_master_info_stop_cond, "Master_info::stop_cond", 0},
  { &key_master_info_sleep_cond, "Master_info::sleep_cond", 0},
  { &key_relay_log_info_data_cond, "Relay_log_info::data_cond", 0},
  { &key_relay_log_info_log_space_cond, "Relay_log_info::log_space_cond", 0},
  { &key_relay_log_info_start_cond, "Relay_log_info::start_cond", 0},
  { &key_relay_log_info_stop_cond, "Relay_log_info::stop_cond", 0},
  { &key_relay_log_info_sleep_cond, "Relay_log_info::sleep_cond", 0},
  { &key_cond_slave_parallel_pend_jobs, "Relay_log_info::pending_jobs_cond", 0},
  { &key_cond_slave_parallel_worker, "Worker_info::jobs_cond", 0},
  { &key_TABLE_SHARE_cond, "TABLE_SHARE::cond", 0},
  { &key_user_level_lock_cond, "User_level_lock::cond", 0},
  { &key_COND_thread_count, "COND_thread_count", PSI_FLAG_GLOBAL},
  { &key_COND_thread_cache, "COND_thread_cache", PSI_FLAG_GLOBAL},
  { &key_COND_flush_thread_cache, "COND_flush_thread_cache", PSI_FLAG_GLOBAL},
  { &key_gtid_ensure_index_cond, "Gtid_state", PSI_FLAG_GLOBAL},
  { &key_hlc_wait_cond, "HybridLogicalClock", PSI_FLAG_GLOBAL},
  { &key_COND_connection_count, "COND_connection_count", PSI_FLAG_GLOBAL},
#ifdef HAVE_MY_TIMER
  { &key_thread_timer_notifier, "thread_timer_notifier", PSI_FLAG_GLOBAL},
#endif
  { &key_gtid_info_data_cond, "Gtid_info::data_cond", 0},
  { &key_gtid_info_start_cond, "Gtid_info::start_cond", 0},
  { &key_gtid_info_stop_cond, "Gtid_info::stop_cond", 0},
  { &key_gtid_info_sleep_cond, "Gtid_info::sleep_cond", 0},
  { &key_commit_order_manager_cond, "Commit_order_manager::m_workers.cond", 0},
  { &key_COND_ac_node, "st_ac_node::cond", 0},
};

PSI_thread_key key_thread_bootstrap, key_thread_delayed_insert,
  key_thread_handle_manager, key_thread_handle_slave_stats_daemon, key_thread_main,
  key_thread_one_connection, key_thread_signal_hand;

#ifdef HAVE_MY_TIMER
PSI_thread_key key_thread_timer_notifier;
#endif

static PSI_thread_info all_server_threads[]=
{
#if (defined(_WIN32) || defined(HAVE_SMEM)) && !defined(EMBEDDED_LIBRARY)
  { &key_thread_handle_con_namedpipes, "con_named_pipes", PSI_FLAG_GLOBAL},
#endif /* _WIN32 || HAVE_SMEM && !EMBEDDED_LIBRARY */

#if defined(HAVE_SMEM) && !defined(EMBEDDED_LIBRARY)
  { &key_thread_handle_con_sharedmem, "con_shared_mem", PSI_FLAG_GLOBAL},
#endif /* HAVE_SMEM && !EMBEDDED_LIBRARY */

#if !defined(EMBEDDED_LIBRARY)
  { &key_thread_handle_con_sockets, "con_sockets", PSI_FLAG_GLOBAL},
#endif /* _WIN32 || HAVE_SMEM && !EMBEDDED_LIBRARY */

#if !defined(EMBEDDED_LIBRARY)
  { &key_thread_handle_con_admin_sockets, "con_admin_sockets", PSI_FLAG_GLOBAL},
#endif /* !EMBEDDED_LIBRARY */

#ifdef __WIN__
  { &key_thread_handle_shutdown, "shutdown", PSI_FLAG_GLOBAL},
#endif /* __WIN__ */

  { &key_thread_bootstrap, "bootstrap", PSI_FLAG_GLOBAL},
  { &key_thread_delayed_insert, "delayed_insert", 0},
  { &key_thread_handle_manager, "manager", PSI_FLAG_GLOBAL},
  { &key_thread_handle_slave_stats_daemon, "slave_stats_daemon", PSI_FLAG_GLOBAL},
  { &key_thread_main, "main", PSI_FLAG_GLOBAL},
  { &key_thread_one_connection, "one_connection", 0},
  { &key_thread_signal_hand, "signal_handler", PSI_FLAG_GLOBAL}
};

#ifdef HAVE_MMAP
PSI_file_key key_file_map;
#endif /* HAVE_MMAP */

PSI_file_key key_file_binlog, key_file_binlog_index, key_file_casetest,
  key_file_dbopt, key_file_des_key_file, key_file_ERRMSG, key_select_to_file,
  key_file_fileparser, key_file_frm, key_file_global_ddl_log, key_file_load,
  key_file_loadfile, key_file_log_event_data, key_file_log_event_info,
  key_file_master_info, key_file_misc, key_file_partition,
  key_file_pid, key_file_relay_log_info, key_file_send_file, key_file_tclog,
  key_file_trg, key_file_trn, key_file_init, key_file_shutdown;
PSI_file_key key_file_query_log, key_file_slow_log, key_file_gap_lock_log;
PSI_file_key key_file_relaylog, key_file_relaylog_index;

static PSI_file_info all_server_files[]=
{
#ifdef HAVE_MMAP
  { &key_file_map, "map", 0},
#endif /* HAVE_MMAP */
  { &key_file_binlog, "binlog", 0},
  { &key_file_binlog_index, "binlog_index", 0},
  { &key_file_relaylog, "relaylog", 0},
  { &key_file_relaylog_index, "relaylog_index", 0},
  { &key_file_casetest, "casetest", 0},
  { &key_file_dbopt, "dbopt", 0},
  { &key_file_des_key_file, "des_key_file", 0},
  { &key_file_ERRMSG, "ERRMSG", 0},
  { &key_select_to_file, "select_to_file", 0},
  { &key_file_fileparser, "file_parser", 0},
  { &key_file_frm, "FRM", 0},
  { &key_file_gap_lock_log, "gap_lock_log", 0},
  { &key_file_global_ddl_log, "global_ddl_log", 0},
  { &key_file_load, "load", 0},
  { &key_file_loadfile, "LOAD_FILE", 0},
  { &key_file_log_event_data, "log_event_data", 0},
  { &key_file_log_event_info, "log_event_info", 0},
  { &key_file_master_info, "master_info", 0},
  { &key_file_misc, "misc", 0},
  { &key_file_partition, "partition", 0},
  { &key_file_pid, "pid", 0},
  { &key_file_query_log, "query_log", 0},
  { &key_file_relay_log_info, "relay_log_info", 0},
  { &key_file_send_file, "send_file", 0},
  { &key_file_slow_log, "slow_log", 0},
  { &key_file_tclog, "tclog", 0},
  { &key_file_trg, "trigger_name", 0},
  { &key_file_trn, "trigger", 0},
  { &key_file_init, "init", 0},
  { &key_file_shutdown, "shutdown", 0}
};
#endif /* HAVE_PSI_INTERFACE */

PSI_stage_info stage_admission_control_enter=
  { 0, "Admission control enter", 0};
PSI_stage_info stage_admission_control_exit= { 0, "Admission control exit", 0};
PSI_stage_info stage_after_create= { 0, "After create", 0};
PSI_stage_info stage_allocating_local_table= { 0, "allocating local table", 0};
PSI_stage_info stage_alter_inplace_prepare= { 0, "preparing for alter table", 0};
PSI_stage_info stage_alter_inplace= { 0, "altering table", 0};
PSI_stage_info stage_alter_inplace_commit= { 0, "committing alter table to storage engine", 0};
PSI_stage_info stage_changing_master= { 0, "Changing master", 0};
PSI_stage_info stage_checking_master_version= { 0, "Checking master version", 0};
PSI_stage_info stage_checking_permissions= { 0, "checking permissions", 0};
PSI_stage_info stage_checking_privileges_on_cached_query= { 0, "checking privileges on cached query", 0};
PSI_stage_info stage_checking_query_cache_for_query= { 0, "checking query cache for query", 0};
PSI_stage_info stage_cleaning_up= { 0, "cleaning up", 0};
PSI_stage_info stage_closing_tables= { 0, "closing tables", 0};
PSI_stage_info stage_connecting_to_master= { 0, "Connecting to master", 0};
PSI_stage_info stage_converting_heap_to_myisam= { 0, "converting HEAP to MyISAM", 0};
PSI_stage_info stage_copying_to_group_table= { 0, "Copying to group table", 0};
PSI_stage_info stage_copying_to_tmp_table= { 0, "Copying to tmp table", 0};
PSI_stage_info stage_copy_to_tmp_table= { 0, "copy to tmp table", 0};
PSI_stage_info stage_creating_delayed_handler= { 0, "Creating delayed handler", 0};
PSI_stage_info stage_creating_sort_index= { 0, "Creating sort index", 0};
PSI_stage_info stage_creating_table= { 0, "creating table", 0};
PSI_stage_info stage_creating_tmp_table= { 0, "Creating tmp table", 0};
PSI_stage_info stage_deleting_from_main_table= { 0, "deleting from main table", 0};
PSI_stage_info stage_deleting_from_reference_tables= { 0, "deleting from reference tables", 0};
PSI_stage_info stage_discard_or_import_tablespace= { 0, "discard_or_import_tablespace", 0};
PSI_stage_info stage_end= { 0, "end", 0};
PSI_stage_info stage_executing= { 0, "executing", 0};
PSI_stage_info stage_execution_of_init_command= { 0, "Execution of init_command", 0};
PSI_stage_info stage_explaining= { 0, "explaining", 0};
PSI_stage_info stage_finished_reading_one_binlog_switching_to_next_binlog= { 0, "Finished reading one binlog; switching to next binlog", 0};
PSI_stage_info stage_flushing_relay_log_and_master_info_repository= { 0, "Flushing relay log and master info repository.", 0};
PSI_stage_info stage_flushing_relay_log_info_file= { 0, "Flushing relay-log info file.", 0};
PSI_stage_info stage_freeing_items= { 0, "freeing items", 0};
PSI_stage_info stage_fulltext_initialization= { 0, "FULLTEXT initialization", 0};
PSI_stage_info stage_got_handler_lock= { 0, "got handler lock", 0};
PSI_stage_info stage_got_old_table= { 0, "got old table", 0};
PSI_stage_info stage_init= { 0, "init", 0};
PSI_stage_info stage_insert= { 0, "insert", 0};
PSI_stage_info stage_invalidating_query_cache_entries_table= { 0, "invalidating query cache entries (table)", 0};
PSI_stage_info stage_invalidating_query_cache_entries_table_list= { 0, "invalidating query cache entries (table list)", 0};
PSI_stage_info stage_killing_slave= { 0, "Killing slave", 0};
PSI_stage_info stage_logging_slow_query= { 0, "logging slow query", 0};
PSI_stage_info stage_making_temp_file_append_before_load_data= { 0, "Making temporary file (append) before replaying LOAD DATA INFILE.", 0};
PSI_stage_info stage_making_temp_file_create_before_load_data= { 0, "Making temporary file (create) before replaying LOAD DATA INFILE.", 0};
PSI_stage_info stage_manage_keys= { 0, "manage keys", 0};
PSI_stage_info stage_master_has_sent_all_binlog_to_slave= { 0, "Master has sent all binlog to slave; waiting for binlog to be updated", 0};
PSI_stage_info stage_opening_tables= { 0, "Opening tables", 0};
PSI_stage_info stage_optimizing= { 0, "optimizing", 0};
PSI_stage_info stage_preparing= { 0, "preparing", 0};
PSI_stage_info stage_purging_old_relay_logs= { 0, "Purging old relay logs", 0};
PSI_stage_info stage_query_end= { 0, "query end", 0};
PSI_stage_info stage_queueing_master_event_to_the_relay_log= { 0, "Queueing master event to the relay log", 0};
PSI_stage_info stage_reading_event_from_the_relay_log= { 0, "Reading event from the relay log", 0};
PSI_stage_info stage_registering_slave_on_master= { 0, "Registering slave on master", 0};
PSI_stage_info stage_removing_duplicates= { 0, "Removing duplicates", 0};
PSI_stage_info stage_removing_tmp_table= { 0, "removing tmp table", 0};
PSI_stage_info stage_rename= { 0, "rename", 0};
PSI_stage_info stage_rename_result_table= { 0, "rename result table", 0};
PSI_stage_info stage_requesting_binlog_dump= { 0, "Requesting binlog dump", 0};
PSI_stage_info stage_reschedule= { 0, "reschedule", 0};
PSI_stage_info stage_searching_rows_for_update= { 0, "Searching rows for update", 0};
PSI_stage_info stage_sending_binlog_event_to_slave= { 0, "Sending binlog event to slave", 0};
PSI_stage_info stage_sending_cached_result_to_client= { 0, "sending cached result to client", 0};
PSI_stage_info stage_sending_data= { 0, "Sending data", 0};
PSI_stage_info stage_setup= { 0, "setup", 0};
PSI_stage_info stage_slave_has_read_all_relay_log= { 0, "Slave has read all relay log; waiting for the slave I/O thread to update it", 0};
PSI_stage_info stage_sorting_for_group= { 0, "Sorting for group", 0};
PSI_stage_info stage_sorting_for_order= { 0, "Sorting for order", 0};
PSI_stage_info stage_sorting_result= { 0, "Sorting result", 0};
PSI_stage_info stage_statistics= { 0, "statistics", 0};
PSI_stage_info stage_sql_thd_waiting_until_delay= { 0, "Waiting until MASTER_DELAY seconds after master executed event", 0 };
PSI_stage_info stage_storing_result_in_query_cache= { 0, "storing result in query cache", 0};
PSI_stage_info stage_storing_row_into_queue= { 0, "storing row into queue", 0};
PSI_stage_info stage_system_lock= { 0, "System lock", 0};
PSI_stage_info stage_update= { 0, "update", 0};
PSI_stage_info stage_updating= { 0, "updating", 0};
PSI_stage_info stage_updating_main_table= { 0, "updating main table", 0};
PSI_stage_info stage_updating_reference_tables= { 0, "updating reference tables", 0};
PSI_stage_info stage_upgrading_lock= { 0, "upgrading lock", 0};
PSI_stage_info stage_user_lock= { 0, "User lock", 0};
PSI_stage_info stage_user_sleep= { 0, "User sleep", 0};
PSI_stage_info stage_verifying_table= { 0, "verifying table", 0};
PSI_stage_info stage_waiting_for_commit= { 0, "waiting for commit", 0};
PSI_stage_info stage_waiting_for_admission= { 0, "waiting for admission", 0};
PSI_stage_info stage_waiting_for_readmission= { 0, "waiting for readmission", 0};
PSI_stage_info stage_waiting_for_delay_list= { 0, "waiting for delay_list", 0};
PSI_stage_info stage_waiting_for_gtid_to_be_written_to_binary_log= { 0, "waiting for GTID to be written to binary log", 0};
PSI_stage_info stage_waiting_for_handler_insert= { 0, "waiting for handler insert", 0};
PSI_stage_info stage_waiting_for_handler_lock= { 0, "waiting for handler lock", 0};
PSI_stage_info stage_waiting_for_handler_open= { 0, "waiting for handler open", 0};
PSI_stage_info stage_waiting_for_insert= { 0, "Waiting for INSERT", 0};
PSI_stage_info stage_waiting_for_master_to_send_event= { 0, "Waiting for master to send event", 0};
PSI_stage_info stage_waiting_for_master_update= { 0, "Waiting for master update", 0};
PSI_stage_info stage_waiting_for_relay_log_space= { 0, "Waiting for the slave SQL thread to free enough relay log space", 0};
PSI_stage_info stage_waiting_for_slave_mutex_on_exit= { 0, "Waiting for slave mutex on exit", 0};
PSI_stage_info stage_waiting_for_slave_thread_to_start= { 0, "Waiting for slave thread to start", 0};
PSI_stage_info stage_waiting_for_table_flush= { 0, "Waiting for table flush", 0};
PSI_stage_info stage_waiting_for_query_cache_lock= { 0, "Waiting for query cache lock", 0};
PSI_stage_info stage_waiting_for_the_next_event_in_relay_log= { 0, "Waiting for the next event in relay log", 0};
PSI_stage_info stage_waiting_for_the_slave_thread_to_advance_position= { 0, "Waiting for the slave SQL thread to advance position", 0};
PSI_stage_info stage_waiting_to_finalize_termination= { 0, "Waiting to finalize termination", 0};
PSI_stage_info stage_worker_waiting_for_its_turn_to_commit= { 0, "Waiting for preceding transaction to commit", 0};
PSI_stage_info stage_waiting_to_get_readlock= { 0, "Waiting to get readlock", 0};
PSI_stage_info stage_slave_waiting_workers_to_exit= { 0, "Waiting for workers to exit", 0};
PSI_stage_info stage_slave_waiting_worker_to_release_partition= { 0, "Waiting for Slave Worker to release partition", 0};
PSI_stage_info stage_slave_waiting_worker_to_free_events= { 0, "Waiting for Slave Workers to free pending events", 0};
PSI_stage_info stage_slave_waiting_worker_queue= { 0, "Waiting for Slave Worker queue", 0};
PSI_stage_info stage_slave_waiting_event_from_coordinator= { 0, "Waiting for an event from Coordinator", 0};
PSI_stage_info stage_slave_waiting_for_dependencies= { 0, "Waiting for dependencies to be satisfied", 0};
PSI_stage_info stage_slave_waiting_for_dependency_workers= { 0, "Waiting for dependency workers to finish", 0};
PSI_stage_info stage_waiting_for_hlc= { 0, "Waiting for database applied HLC", 0};
PSI_stage_info stage_slave_waiting_semi_sync_ack= { 0, "Waiting for an ACK from semi-sync ACKers", 0};

#ifdef HAVE_PSI_INTERFACE

PSI_stage_info *all_server_stages[]=
{
  & stage_admission_control_enter,
  & stage_admission_control_exit,
  & stage_after_create,
  & stage_allocating_local_table,
  & stage_alter_inplace_prepare,
  & stage_alter_inplace,
  & stage_alter_inplace_commit,
  & stage_changing_master,
  & stage_checking_master_version,
  & stage_checking_permissions,
  & stage_checking_privileges_on_cached_query,
  & stage_checking_query_cache_for_query,
  & stage_cleaning_up,
  & stage_closing_tables,
  & stage_connecting_to_master,
  & stage_converting_heap_to_myisam,
  & stage_copying_to_group_table,
  & stage_copying_to_tmp_table,
  & stage_copy_to_tmp_table,
  & stage_creating_delayed_handler,
  & stage_creating_sort_index,
  & stage_creating_table,
  & stage_creating_tmp_table,
  & stage_deleting_from_main_table,
  & stage_deleting_from_reference_tables,
  & stage_discard_or_import_tablespace,
  & stage_end,
  & stage_executing,
  & stage_execution_of_init_command,
  & stage_explaining,
  & stage_finished_reading_one_binlog_switching_to_next_binlog,
  & stage_flushing_relay_log_and_master_info_repository,
  & stage_flushing_relay_log_info_file,
  & stage_freeing_items,
  & stage_fulltext_initialization,
  & stage_got_handler_lock,
  & stage_got_old_table,
  & stage_init,
  & stage_insert,
  & stage_invalidating_query_cache_entries_table,
  & stage_invalidating_query_cache_entries_table_list,
  & stage_killing_slave,
  & stage_logging_slow_query,
  & stage_making_temp_file_append_before_load_data,
  & stage_making_temp_file_create_before_load_data,
  & stage_manage_keys,
  & stage_master_has_sent_all_binlog_to_slave,
  & stage_opening_tables,
  & stage_optimizing,
  & stage_preparing,
  & stage_purging_old_relay_logs,
  & stage_query_end,
  & stage_queueing_master_event_to_the_relay_log,
  & stage_reading_event_from_the_relay_log,
  & stage_registering_slave_on_master,
  & stage_removing_duplicates,
  & stage_removing_tmp_table,
  & stage_rename,
  & stage_rename_result_table,
  & stage_requesting_binlog_dump,
  & stage_reschedule,
  & stage_searching_rows_for_update,
  & stage_sending_binlog_event_to_slave,
  & stage_sending_cached_result_to_client,
  & stage_sending_data,
  & stage_setup,
  & stage_slave_has_read_all_relay_log,
  & stage_sorting_for_group,
  & stage_sorting_for_order,
  & stage_sorting_result,
  & stage_sql_thd_waiting_until_delay,
  & stage_statistics,
  & stage_storing_result_in_query_cache,
  & stage_storing_row_into_queue,
  & stage_system_lock,
  & stage_update,
  & stage_updating,
  & stage_updating_main_table,
  & stage_updating_reference_tables,
  & stage_upgrading_lock,
  & stage_user_lock,
  & stage_user_sleep,
  & stage_verifying_table,
  & stage_waiting_for_admission,
  & stage_waiting_for_readmission,
  & stage_waiting_for_delay_list,
  & stage_waiting_for_handler_insert,
  & stage_waiting_for_handler_lock,
  & stage_waiting_for_handler_open,
  & stage_waiting_for_insert,
  & stage_waiting_for_master_to_send_event,
  & stage_waiting_for_master_update,
  & stage_waiting_for_slave_mutex_on_exit,
  & stage_waiting_for_slave_thread_to_start,
  & stage_waiting_for_table_flush,
  & stage_waiting_for_query_cache_lock,
  & stage_waiting_for_the_next_event_in_relay_log,
  & stage_waiting_for_the_slave_thread_to_advance_position,
  & stage_waiting_to_finalize_termination,
  & stage_worker_waiting_for_its_turn_to_commit,
  & stage_waiting_to_get_readlock,
  & stage_waiting_for_hlc
};

PSI_socket_key key_socket_tcpip, key_socket_unix, key_socket_client_connection;

static PSI_socket_info all_server_sockets[]=
{
  { &key_socket_tcpip, "server_tcpip_socket", PSI_FLAG_GLOBAL},
  { &key_socket_unix, "server_unix_socket", PSI_FLAG_GLOBAL},
  { &key_socket_client_connection, "client_connection", 0}
};

/**
  Initialise all the performance schema instrumentation points
  used by the server.
*/
void init_server_psi_keys(void)
{
  const char* category= "sql";
  int count;

  count= array_elements(all_server_mutexes);
  mysql_mutex_register(category, all_server_mutexes, count);

  count= array_elements(all_server_rwlocks);
  mysql_rwlock_register(category, all_server_rwlocks, count);

  count= array_elements(all_server_conds);
  mysql_cond_register(category, all_server_conds, count);

  count= array_elements(all_server_threads);
  mysql_thread_register(category, all_server_threads, count);

  count= array_elements(all_server_files);
  mysql_file_register(category, all_server_files, count);

  count= array_elements(all_server_stages);
  mysql_stage_register(category, all_server_stages, count);

  count= array_elements(all_server_sockets);
  mysql_socket_register(category, all_server_sockets, count);

#ifdef HAVE_PSI_STATEMENT_INTERFACE
  init_sql_statement_info();
  count= array_elements(sql_statement_info);
  mysql_statement_register(category, sql_statement_info, count);

  category= "com";
  init_com_statement_info();

  /*
    Register [0 .. COM_QUERY - 1] as "statement/com/..."
  */
  count= (int) COM_QUERY;
  mysql_statement_register(category, com_statement_info, count);

  /*
    Register [COM_QUERY + 1 .. COM_END] as "statement/com/..."
  */
  count= (int) COM_END - (int) COM_QUERY;
  mysql_statement_register(category, & com_statement_info[(int) COM_QUERY + 1], count);

  category= "abstract";
  /*
    Register [COM_QUERY] and [COM_QUERY_ATTRS] as "statement/abstract/com_query"
  */
  mysql_statement_register(category, & com_statement_info[(int) COM_QUERY], 1);
  mysql_statement_register(category,
                           & com_statement_info[(int) COM_QUERY_ATTRS], 1);

  /*
    When a new packet is received,
    it is instrumented as "statement/abstract/new_packet".
    Based on the packet type found, it later mutates to the
    proper narrow type, for example
    "statement/abstract/query" or "statement/com/ping".
    In cases of "statement/abstract/query", SQL queries are given to
    the parser, which mutates the statement type to an even more
    narrow classification, for example "statement/sql/select".
  */
  stmt_info_new_packet.m_key= 0;
  stmt_info_new_packet.m_name= "new_packet";
  stmt_info_new_packet.m_flags= PSI_FLAG_MUTABLE;
  mysql_statement_register(category, &stmt_info_new_packet, 1);

  /*
    Statements processed from the relay log are initially instrumented as
    "statement/abstract/relay_log". The parser will mutate the statement type to
    a more specific classification, for example "statement/sql/insert".
  */
  stmt_info_rpl.m_key= 0;
  stmt_info_rpl.m_name= "relay_log";
  stmt_info_rpl.m_flags= PSI_FLAG_MUTABLE;
  mysql_statement_register(category, &stmt_info_rpl, 1);
#endif
}

#endif /* HAVE_PSI_INTERFACE */

/*
  Called whenever log_datagram is updated. This is used to initialize the
  datagram socket and connect to it. Called from init_server_components
  also.
*/
bool setup_datagram_socket(sys_var *self, THD *thd, enum_var_type type)
{
  if (log_datagram_sock >= 0)
  {
    close(log_datagram_sock);
    log_datagram_sock = -1;
  }
  if (log_datagram)
  {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, "slocket");
    log_datagram_sock = socket(AF_UNIX, SOCK_DGRAM, 0);

    if (log_datagram_sock < 0)
    {
      sql_print_information("slocket creation failed with error %d; "
                            "slocket closed", errno);
      log_datagram = 0;
      return false;
    }

    // set nonblocking
    if (fcntl(log_datagram_sock, F_SETFL, O_NONBLOCK |
              fcntl(log_datagram_sock, F_GETFL)) == -1)
    {
      log_datagram = 0;
       close(log_datagram_sock);
      log_datagram_sock = -1;
      sql_print_information("slocket set nonblocking failed with error %d; "
                            "slocket closed", errno);
      return false;
    }

    if(connect(log_datagram_sock, (sockaddr*) &addr,
               strlen(addr.sun_path) + sizeof(addr.sun_family)) < 0)
    {
      log_datagram = 0;
      close(log_datagram_sock);
      log_datagram_sock = -1;
      sql_print_information("slocket connect failed with error %d; "
                            "slocket closed", errno);
      return false;
    }
  }
  return false;
}

/*
 * Lookup table for the substitution box hash.
 * The values were generated randomly, with some additional sanity checks.
 */

uint32 sbox[256] =
  {
    0xF53E1837, 0x5F14C86B, 0x9EE3964C, 0xFA796D53,
    0x32223FC3, 0x4D82BC98, 0xA0C7FA62, 0x63E2C982,
    0x24994A5B, 0x1ECE7BEE, 0x292B38EF, 0xD5CD4E56,
    0x514F4303, 0x7BE12B83, 0x7192F195, 0x82DC7300,
    0x084380B4, 0x480B55D3, 0x5F430471, 0x13F75991,
    0x3F9CF22C, 0x2FE0907A, 0xFD8E1E69, 0x7B1D5DE8,
    0xD575A85C, 0xAD01C50A, 0x7EE00737, 0x3CE981E8,
    0x0E447EFA, 0x23089DD6, 0xB59F149F, 0x13600EC7,
    0xE802C8E6, 0x670921E4, 0x7207EFF0, 0xE74761B0,
    0x69035234, 0xBFA40F19, 0xF63651A0, 0x29E64C26,
    0x1F98CCA7, 0xD957007E, 0xE71DDC75, 0x3E729595,
    0x7580B7CC, 0xD7FAF60B, 0x92484323, 0xA44113EB,
    0xE4CBDE08, 0x346827C9, 0x3CF32AFA, 0x0B29BCF1,
    0x6E29F7DF, 0xB01E71CB, 0x3BFBC0D1, 0x62EDC5B8,
    0xB7DE789A, 0xA4748EC9, 0xE17A4C4F, 0x67E5BD03,
    0xF3B33D1A, 0x97D8D3E9, 0x09121BC0, 0x347B2D2C,
    0x79A1913C, 0x504172DE, 0x7F1F8483, 0x13AC3CF6,
    0x7A2094DB, 0xC778FA12, 0xADF7469F, 0x21786B7B,
    0x71A445D0, 0xA8896C1B, 0x656F62FB, 0x83A059B3,
    0x972DFE6E, 0x4122000C, 0x97D9DA19, 0x17D5947B,
    0xB1AFFD0C, 0x6EF83B97, 0xAF7F780B, 0x4613138A,
    0x7C3E73A6, 0xCF15E03D, 0x41576322, 0x672DF292,
    0xB658588D, 0x33EBEFA9, 0x938CBF06, 0x06B67381,
    0x07F192C6, 0x2BDA5855, 0x348EE0E8, 0x19DBB6E3,
    0x3222184B, 0xB69D5DBA, 0x7E760B88, 0xAF4D8154,
    0x007A51AD, 0x35112500, 0xC9CD2D7D, 0x4F4FB761,
    0x694772E3, 0x694C8351, 0x4A7E3AF5, 0x67D65CE1,
    0x9287DE92, 0x2518DB3C, 0x8CB4EC06, 0xD154D38F,
    0xE19A26BB, 0x295EE439, 0xC50A1104, 0x2153C6A7,
    0x82366656, 0x0713BC2F, 0x6462215A, 0x21D9BFCE,
    0xBA8EACE6, 0xAE2DF4C1, 0x2A8D5E80, 0x3F7E52D1,
    0x29359399, 0xFEA1D19C, 0x18879313, 0x455AFA81,
    0xFADFE838, 0x62609838, 0xD1028839, 0x0736E92F,
    0x3BCA22A3, 0x1485B08A, 0x2DA7900B, 0x852C156D,
    0xE8F24803, 0x00078472, 0x13F0D332, 0x2ACFD0CF,
    0x5F747F5C, 0x87BB1E2F, 0xA7EFCB63, 0x23F432F0,
    0xE6CE7C5C, 0x1F954EF6, 0xB609C91B, 0x3B4571BF,
    0xEED17DC0, 0xE556CDA0, 0xA7846A8D, 0xFF105F94,
    0x52B7CCDE, 0x0E33E801, 0x664455EA, 0xF2C70414,
    0x73E7B486, 0x8F830661, 0x8B59E826, 0xBB8AEDCA,
    0xF3D70AB9, 0xD739F2B9, 0x4A04C34A, 0x88D0F089,
    0xE02191A2, 0xD89D9C78, 0x192C2749, 0xFC43A78F,
    0x0AAC88CB, 0x9438D42D, 0x9E280F7A, 0x36063802,
    0x38E8D018, 0x1C42A9CB, 0x92AAFF6C, 0xA24820C5,
    0x007F077F, 0xCE5BC543, 0x69668D58, 0x10D6FF74,
    0xBE00F621, 0x21300BBE, 0x2E9E8F46, 0x5ACEA629,
    0xFA1F86C7, 0x52F206B8, 0x3EDF1A75, 0x6DA8D843,
    0xCF719928, 0x73E3891F, 0xB4B95DD6, 0xB2A42D27,
    0xEDA20BBF, 0x1A58DBDF, 0xA449AD03, 0x6DDEF22B,
    0x900531E6, 0x3D3BFF35, 0x5B24ABA2, 0x472B3E4C,
    0x387F2D75, 0x4D8DBA36, 0x71CB5641, 0xE3473F3F,
    0xF6CD4B7F, 0xBF7D1428, 0x344B64D0, 0xC5CDFCB6,
    0xFE2E0182, 0x2C37A673, 0xDE4EB7A3, 0x63FDC933,
    0x01DC4063, 0x611F3571, 0xD167BFAF, 0x4496596F,
    0x3DEE0689, 0xD8704910, 0x7052A114, 0x068C9EC5,
    0x75D0E766, 0x4D54CC20, 0xB44ECDE2, 0x4ABC653E,
    0x2C550A21, 0x1A52C0DB, 0xCFED03D0, 0x119BAFE2,
    0x876A6133, 0xBC232088, 0x435BA1B2, 0xAE99BBFA,
    0xBB4F08E4, 0xA62B5F49, 0x1DA4B695, 0x336B84DE,
    0xDC813D31, 0x00C134FB, 0x397A98E6, 0x151F0E64,
    0xD9EB3E69, 0xD3C7DF60, 0xD2F2C336, 0x2DDD067B,
    0xBD122835, 0xB0B3BD3A, 0xB0D54E46, 0x8641F1E4,
    0xA0B38F96, 0x51D39199, 0x37A6AD75, 0xDF84EE41,
    0x3C034CBA, 0xACDA62FC, 0x11923B8B, 0x45EF170A,
  };

uint32 my_sbox_hash(const uchar* data, ulong length) {
  ulong i;
  uint32 hash = 0;
  for(i = 0; i < length; i++) {
    hash ^= sbox[ data[i] ];
    hash *= 3;
  }
  return hash;
}

#if (defined(_WIN32) || defined(HAVE_SMEM)) && !defined(EMBEDDED_LIBRARY)
// update_named_pipe_full_access_group returns false on success, true on failure
bool update_named_pipe_full_access_group(const char *new_group_name)
{
  SECURITY_ATTRIBUTES *p_new_sa= NULL;
  const char *perror= NULL;
  TCHAR last_error_msg[256];

  // Set up security attributes to provide full access to the owner
  // and minimal read/write access to others.
  if (my_security_attr_create(&p_new_sa, &perror, NAMED_PIPE_OWNER_PERMISSIONS,
                              NAMED_PIPE_EVERYONE_PERMISSIONS) != 0)
  {
    sql_print_error("my_security_attr_create: %s", perror);
    return true;
  }
  if (new_group_name && new_group_name[0] != '\0')
  {
    if (my_security_attr_add_rights_to_group(
            p_new_sa, new_group_name,
            NAMED_PIPE_FULL_ACCESS_GROUP_PERMISSIONS))
    {
      sql_print_error("my_security_attr_add_rights_to_group failed for group: %s",
                      new_group_name);
      return false;
    }
  }

  psaPipeSecurity= p_new_sa;

  // Set the DACL for the existing "listener" named pipe instance...
  if (hPipe != INVALID_HANDLE_VALUE)
  {
    PACL pdacl= NULL;
    BOOL dacl_present_in_descriptor= FALSE;
    BOOL dacl_defaulted= FALSE;
    if (!GetSecurityDescriptorDacl(p_new_sa->lpSecurityDescriptor,
                                   &dacl_present_in_descriptor, &pdacl,
                                   &dacl_defaulted) ||
        !dacl_present_in_descriptor)
    {
      DWORD last_error_num= GetLastError();
      FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                    NULL, last_error_num,
                    MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), last_error_msg,
                    sizeof(last_error_msg) / sizeof(TCHAR), NULL);
      sql_print_error("GetSecurityDescriptorDacl failed: %s", last_error_msg);
      return true;
    }
    DWORD res =
        SetSecurityInfo(hPipe, SE_KERNEL_OBJECT,
                        DACL_SECURITY_INFORMATION, NULL, NULL, pdacl, NULL);
    if (res != ERROR_SUCCESS)
    {
      char num_buff[20];
      int10_to_str(res, num_buff, 10);
      sql_print_error("SetSecurityInfo failed to update DACL on named pipe: %s", num_buff);
      return true;
    }
  }
  return false;
}

#endif  /* _WIN32 || HAVE_SMEM && !EMBEDDED_LIBRARY */
