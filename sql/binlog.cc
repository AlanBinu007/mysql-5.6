/* Copyright (c) 2009, 2016, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA */


#include "my_global.h"
#include "log.h"
#include "binlog.h"
#include "log_event.h"
#include "rpl_filter.h"
#include "rpl_rli.h"
#include "rpl_slave_commit_order_manager.h" // Commit_order_manager
#include "rpl_master.h"
#include "sql_plugin.h"
#include "rpl_handler.h"
#include "rpl_info_factory.h"
#include "rpl_utility.h"
#include "debug_sync.h"
#include "global_threads.h"
#include "sql_show.h"
#include "sql_parse.h"
#include "rpl_mi.h"
#include <list>
#include <chrono>
#include <sstream>
#include <my_stacktrace.h>
#include <boost/algorithm/string.hpp>
#include <exception>
#ifdef HAVE_RAPIDJSON
#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#else
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
using boost::property_tree::ptree;
#endif

using std::max;
using std::min;
using std::string;
using std::list;

#include "rpl_mi.h"
extern Master_info *active_mi;
extern char *opt_binlog_index_name;
extern char *opt_relaylog_index_name;
extern char *opt_applylog_index_name;
extern char *opt_apply_logname;
static bool enable_raft_plugin_save= false;
extern char server_uuid[UUID_LENGTH+1];
extern char glob_hostname[FN_REFLEN];

/* Size for IO_CACHE buffer for binlog & relay log */
ulong rpl_read_size;

#define FLAGSTR(V,F) ((V)&(F)?#F" ":"")

/**
  @defgroup Binary_Log Binary Log
  @{
 */

#define MY_OFF_T_UNDEF (~(my_off_t)0UL)

/*
  Constants required for the limit unsafe warnings suppression
 */
//seconds after which the limit unsafe warnings suppression will be activated
#define LIMIT_UNSAFE_WARNING_ACTIVATION_TIMEOUT 50
//number of limit unsafe warnings after which the suppression will be activated
#define LIMIT_UNSAFE_WARNING_ACTIVATION_THRESHOLD_COUNT 50
#define MAX_SESSION_ATTACH_TRIES 10

static ulonglong limit_unsafe_suppression_start_time= 0;
static bool unsafe_warning_suppression_is_activated= false;
static int limit_unsafe_warning_count= 0;

static handlerton *binlog_hton;
bool opt_binlog_order_commits= true;
bool opt_gtid_precommit= false;

const char *log_bin_index= 0;
const char *log_bin_basename= 0;

char *histogram_step_size_binlog_fsync = NULL;
int opt_histogram_step_size_binlog_group_commit = 1;
latency_histogram histogram_binlog_fsync;
counter_histogram histogram_binlog_group_commit;

extern my_bool opt_core_file;

const char *hlc_ts_lower_bound = "hlc_ts_lower_bound";
const char *hlc_wait_timeout_ms = "hlc_wait_timeout_ms";

MYSQL_BIN_LOG mysql_bin_log(&sync_binlog_period);
Dump_log dump_log;

static int binlog_init(void *p);
static int binlog_start_trans_and_stmt(THD *thd, Log_event *start_event);
static int binlog_close_connection(handlerton *hton, THD *thd);
static int binlog_savepoint_set(handlerton *hton, THD *thd, void *sv);
static int binlog_savepoint_rollback(handlerton *hton, THD *thd, void *sv);
static bool binlog_savepoint_rollback_can_release_mdl(handlerton *hton,
                                                      THD *thd);
static int binlog_commit(handlerton *hton, THD *thd, bool all, bool async);
static int binlog_rollback(handlerton *hton, THD *thd, bool all);
static int binlog_prepare(handlerton *hton, THD *thd, bool all, bool async);

Slow_log_throttle log_throttle_sbr_unsafe_query(
  &opt_log_throttle_sbr_unsafe_queries,
  &LOCK_log_throttle_sbr_unsafe,
  Log_throttle::LOG_THROTTLE_WINDOW_SIZE,
  slow_log_print,
  "throttle: %10lu 'sbr unsafe' warning(s) suppressed.");

#ifdef HAVE_REPLICATION
static inline bool has_commit_order_manager(THD *thd)
{
  return is_mts_worker(thd) &&
    thd->rli_slave->get_commit_order_manager() != NULL;
}
#endif


/**
  Helper class to hold a mutex for the duration of the
  block.

  Eliminates the need for explicit unlocking of mutexes on, e.g.,
  error returns.  On passing a null pointer, the sentry will not do
  anything.
 */
class Mutex_sentry
{
public:
  Mutex_sentry(mysql_mutex_t *mutex)
    : m_mutex(mutex)
  {
    if (m_mutex)
      mysql_mutex_lock(mutex);
  }

  ~Mutex_sentry()
  {
    if (m_mutex)
      mysql_mutex_unlock(m_mutex);
#ifndef DBUG_OFF
    m_mutex= 0;
#endif
  }

private:
  mysql_mutex_t *m_mutex;

  // It's not allowed to copy this object in any way
  Mutex_sentry(Mutex_sentry const&);
  void operator=(Mutex_sentry const&);
};


/**
  Print system time.
 */

static void print_system_time()
{
#ifdef __WIN__
  SYSTEMTIME utc_time;
  GetSystemTime(&utc_time);
  const long hrs=  utc_time.wHour;
  const long mins= utc_time.wMinute;
  const long secs= utc_time.wSecond;
#else
  /* Using time() instead of my_time() to avoid looping */
  const time_t curr_time= time(NULL);
  /* Calculate time of day */
  const long tmins = curr_time / 60;
  const long thrs  = tmins / 60;
  const long hrs   = thrs  % 24;
  const long mins  = tmins % 60;
  const long secs  = curr_time % 60;
#endif
  char hrs_buf[3]= "00";
  char mins_buf[3]= "00";
  char secs_buf[3]= "00";
  int base= 10;
  my_safe_itoa(base, hrs, &hrs_buf[2]);
  my_safe_itoa(base, mins, &mins_buf[2]);
  my_safe_itoa(base, secs, &secs_buf[2]);

  my_safe_printf_stderr("---------- %s:%s:%s UTC - ",
                        hrs_buf, mins_buf, secs_buf);
}


/**
  Helper class to perform a thread excursion.

  This class is used to temporarily switch to another session (THD
  structure). It will set up thread specific "globals" correctly
  so that the POSIX thread looks exactly like the session attached to.
  However, PSI_thread info is not touched as it is required to show
  the actual physial view in PFS instrumentation i.e., it should
  depict as the real thread doing the work instead of thread it switched
  to.

  On destruction, the original session (which is supplied to the
  constructor) will be re-attached automatically. For example, with
  this code, the value of @c current_thd will be the same before and
  after execution of the code.

  @code
  {
    Thread_excursion excursion(current_thd);
    for (int i = 0 ; i < count ; ++i)
      excursion.attach_to(other_thd[i]);
  }
  @endcode

  @warning The class is not designed to be inherited from.
 */

class Thread_excursion
{
public:
  Thread_excursion(THD *thd)
    : m_original_thd(thd)
  {
  }

  ~Thread_excursion() {
#ifndef EMBEDDED_LIBRARY
    if (unlikely(setup_thread_globals(m_original_thd)))
      DBUG_ASSERT(0);                           // Out of memory?!
#endif
  }

  /**
    Try to attach the POSIX thread to a session.
    - This function attaches the POSIX thread to a session
    in MAX_SESSION_ATTACH_TRIES tries when encountering
    'out of memory' error, and terminates the server after
    failed in MAX_SESSION_ATTACH_TRIES tries.

    @param[in] thd       The thd of a session
   */
  void try_to_attach_to(THD *thd)
  {
    int i= 0;
    /*
      Attach the POSIX thread to a session in MAX_SESSION_ATTACH_TRIES
      tries when encountering 'out of memory' error.
    */
    while (i < MAX_SESSION_ATTACH_TRIES)
    {
      /*
        Currently attach_to(...) returns ER_OUTOFMEMORY or 0. So
        we continue to attach the POSIX thread when encountering
        the ER_OUTOFMEMORY error. Please take care other error
        returned from attach_to(...) in future.
      */
      if (!attach_to(thd))
      {
        if (i > 0)
          sql_print_warning("Server overcomes the temporary 'out of memory' "
                            "in '%d' tries while attaching to session thread "
                            "during the group commit phase.\n", i + 1);
        break;
      }
      i++;
    }
    /*
      Terminate the server after failed to attach the POSIX thread
      to a session in MAX_SESSION_ATTACH_TRIES tries.
    */
    if (MAX_SESSION_ATTACH_TRIES == i)
    {
      print_system_time();
      my_safe_printf_stderr("%s", "[Fatal] Out of memory while attaching to "
                            "session thread during the group commit phase. "
                            "Data consistency between master and slave can "
                            "be guaranteed after server restarts.\n");
      _exit(EXIT_FAILURE);
    }
  }

private:

  /**
    Attach the POSIX thread to a session.
   */
  int attach_to(THD *thd)
  {
#ifndef EMBEDDED_LIBRARY
    if (DBUG_EVALUATE_IF("simulate_session_attach_error", 1, 0)
        || unlikely(setup_thread_globals(thd)))
    {
      /*
        Indirectly uses pthread_setspecific, which can only return
        ENOMEM or EINVAL. Since store_globals are using correct keys,
        the only alternative is out of memory.
      */
      return ER_OUTOFMEMORY;
    }
#endif /* EMBEDDED_LIBRARY */
    return 0;
  }

  int setup_thread_globals(THD *thd) const {
    int error= 0;
    THD *original_thd= my_pthread_getspecific(THD*, THR_THD);
    MEM_ROOT* original_mem_root= my_pthread_getspecific(MEM_ROOT*, THR_MALLOC);
    if ((error= my_pthread_setspecific_ptr(THR_THD, thd)))
      goto exit0;
    if ((error= my_pthread_setspecific_ptr(THR_MALLOC, &thd->mem_root)))
      goto exit1;
    if ((error= set_mysys_var(thd->mysys_var)))
      goto exit2;
    goto exit0;
exit2:
    error= my_pthread_setspecific_ptr(THR_MALLOC,  original_mem_root);
exit1:
    error= my_pthread_setspecific_ptr(THR_THD,  original_thd);
exit0:
    return error;
  }

  THD *m_original_thd;
};


/**
  Caches for non-transactional and transactional data before writing
  it to the binary log.

  @todo All the access functions for the flags suggest that the
  encapsuling is not done correctly, so try to move any logic that
  requires access to the flags into the cache.
*/
class binlog_cache_data
{
public:

  binlog_cache_data(bool trx_cache_arg,
                    my_off_t max_binlog_cache_size_arg,
                    ulong *ptr_binlog_cache_use_arg,
                    ulong *ptr_binlog_cache_disk_use_arg)
  : m_pending(0), saved_max_binlog_cache_size(max_binlog_cache_size_arg),
    ptr_binlog_cache_use(ptr_binlog_cache_use_arg),
    ptr_binlog_cache_disk_use(ptr_binlog_cache_disk_use_arg)
  {
    reset();
    flags.transactional= trx_cache_arg;
    cache_log.end_of_file= saved_max_binlog_cache_size;
  }

  int finalize(THD *thd, Log_event *end_event);
  int flush(THD *thd, my_off_t *bytes, bool *wrote_xid, bool async);
  int write_event(THD *thd, Log_event *event,
                  bool write_meta_data_event= false);

  virtual ~binlog_cache_data()
  {
    DBUG_ASSERT(is_binlog_empty());
    close_cached_file(&cache_log);
  }

  bool is_binlog_empty() const
  {
    my_off_t pos= my_b_tell(&cache_log);
    DBUG_PRINT("debug", ("%s_cache - pending: 0x%llx, bytes: %llu",
                         (flags.transactional ? "trx" : "stmt"),
                         (ulonglong) pending(), (ulonglong) pos));
    return pending() == NULL && pos == 0;
  }

  bool is_group_cache_empty() const
  {
    return group_cache.is_empty();
  }

#ifndef DBUG_OFF
  bool dbug_is_finalized() const {
    return flags.finalized;
  }
#endif

  Rows_log_event *pending() const
  {
    return m_pending;
  }

  void set_pending(Rows_log_event *const pending)
  {
    m_pending= pending;
  }

  void set_incident(void)
  {
    flags.incident= true;
  }

  bool has_incident(void) const
  {
    return flags.incident;
  }

  /**
    Sets the binlog_cache_data::Flags::flush_error flag if there
    is an error while flushing cache to the file.

    @param thd  The client thread that is executing the transaction.
  */
  void set_flush_error(THD *thd)
  {
    flags.flush_error= true;
    if(is_trx_cache())
    {
      /*
         If the cache is a transactional cache and if the write
         has failed due to ENOSPC, then my_write() would have
         set EE_WRITE error, so clear the error and create an
         equivalent server error.
      */
      if (thd->is_error())
        thd->clear_error();
      char errbuf[MYSYS_STRERROR_SIZE];
      my_error(ER_ERROR_ON_WRITE, MYF(MY_WME), my_filename(cache_log.file),
          errno, my_strerror(errbuf, sizeof(errbuf), errno));
    }
  }

  bool get_flush_error(void) const
  {
    return flags.flush_error;
  }

  bool has_xid() const {
    // There should only be an XID event if we are transactional
    DBUG_ASSERT((flags.transactional && flags.with_xid) || !flags.with_xid);
    return flags.with_xid;
  }

  bool is_trx_cache() const
  {
    return flags.transactional;
  }

  my_off_t get_byte_position() const
  {
    return my_b_tell(&cache_log);
  }

  virtual void reset()
  {
    compute_statistics();
    truncate(0);

    /*
      If IOCACHE has a file associated, change its size to 0.
      It is safer to do it here, since we are certain that one
      asked the cache to go to position 0 with truncate.
    */
    if(cache_log.file != -1)
    {
      int error= 0;
      if((error= my_chsize(cache_log.file, 0, 0, MYF(MY_WME))))
        sql_print_warning("Unable to resize binlog IOCACHE auxilary file");

      DBUG_EXECUTE_IF("show_io_cache_size",
                      {
                        ulong file_size= my_seek(cache_log.file,
                                               0L,MY_SEEK_END,MYF(MY_WME+MY_FAE));
                        sql_print_error("New size:%ld", file_size);
                      });
    }

    flags.incident= false;
    flags.with_xid= false;
    flags.immediate= false;
    flags.finalized= false;
    flags.flush_error= false;
    /*
      The truncate function calls reinit_io_cache that calls my_b_flush_io_cache
      which may increase disk_writes. This breaks the disk_writes use by the
      binary log which aims to compute the ratio between in-memory cache usage
      and disk cache usage. To avoid this undesirable behavior, we reset the
      variable after truncating the cache.
    */
    cache_log.disk_writes= 0;
    group_cache.clear();
    DBUG_ASSERT(is_binlog_empty());
  }

  /*
    Sets the write position to point at the position given. If the
    cache has swapped to a file, it reinitializes it, so that the
    proper data is added to the IO_CACHE buffer. Otherwise, it just
    does a my_b_seek.

    my_b_seek will not work if the cache has swapped, that's why
    we do this workaround.

    @param[IN]  pos the new write position.
    @param[IN]  use_reinit if the position should be reset resorting
                to reset_io_cache (which may issue a flush_io_cache
                inside)

    @return The previous write position.
   */
  my_off_t reset_write_pos(my_off_t pos, bool use_reinit)
  {
    DBUG_ENTER("reset_write_pos");
    DBUG_ASSERT(cache_log.type == WRITE_CACHE);

    my_off_t oldpos= get_byte_position();

    if (use_reinit)
      reinit_io_cache(&cache_log, WRITE_CACHE, pos, 0, 0);
    else
      my_b_seek(&cache_log, pos);

    DBUG_RETURN(oldpos);
  }

  /**
    Remove the pending event.
   */
  int remove_pending_event() {
    delete m_pending;
    m_pending= NULL;
    return 0;
  }

  /*
    Cache to store data before copying it to the binary log.
  */
  IO_CACHE cache_log;

  /**
    The group cache for this cache.
  */
  Group_cache group_cache;

protected:
  /*
    It truncates the cache to a certain position. This includes deleting the
    pending event.
   */
  void truncate(my_off_t pos)
  {
    DBUG_PRINT("info", ("truncating to position %lu", (ulong) pos));
    remove_pending_event();
    /*
      Whenever there is an error while flushing cache to file,
      the local cache will not be in a normal state and the same
      cache cannot be used without facing an assert.
      So, clear the cache if there is a flush error.
    */
    reinit_io_cache(&cache_log, WRITE_CACHE, pos, 0, get_flush_error());
    cache_log.end_of_file= saved_max_binlog_cache_size;
  }

  /**
     Flush pending event to the cache buffer.
   */
  int flush_pending_event(THD *thd) {
    if (m_pending)
    {
      m_pending->set_flags(Rows_log_event::STMT_END_F);
      if (int error= write_event(thd, m_pending))
        return error;
      thd->clear_binlog_table_maps();
    }
    return 0;
  }

  struct Flags {
    /*
      Defines if this is either a trx-cache or stmt-cache, respectively, a
      transactional or non-transactional cache.
    */
    bool transactional:1;

    /*
      This indicates that some events did not get into the cache and most likely
      it is corrupted.
    */
    bool incident:1;

    /*
      This indicates that the cache should be written without BEGIN/END.
    */
    bool immediate:1;

    /*
      This flag indicates that the buffer was finalized and has to be
      flushed to disk.
     */
    bool finalized:1;

    /*
      This indicates that the cache contain an XID event.
     */
    bool with_xid:1;

    /*
      This flag is set to 'true' when there is an error while flushing the
      I/O cache to file.
     */
    bool flush_error:1;
  } flags;

private:
  /*
    Pending binrows event. This event is the event where the rows are currently
    written.
   */
  Rows_log_event *m_pending;

  /**
    This function computes binlog cache and disk usage.
  */
  void compute_statistics()
  {
    if (!is_binlog_empty())
    {
      statistic_increment(*ptr_binlog_cache_use, &LOCK_status);
      if (cache_log.disk_writes != 0)
        statistic_increment(*ptr_binlog_cache_disk_use, &LOCK_status);
    }
  }

  /*
    Stores the values of maximum size of the cache allowed when this cache
    is configured. This corresponds to either
      . max_binlog_cache_size or max_binlog_stmt_cache_size.
  */
  my_off_t saved_max_binlog_cache_size;

  /*
    Stores a pointer to the status variable that keeps track of the in-memory
    cache usage. This corresponds to either
      . binlog_cache_use or binlog_stmt_cache_use.
  */
  ulong *ptr_binlog_cache_use;

  /*
    Stores a pointer to the status variable that keeps track of the disk
    cache usage. This corresponds to either
      . binlog_cache_disk_use or binlog_stmt_cache_disk_use.
  */
  ulong *ptr_binlog_cache_disk_use;

  binlog_cache_data& operator=(const binlog_cache_data& info);
  binlog_cache_data(const binlog_cache_data& info);
};


class binlog_stmt_cache_data
  : public binlog_cache_data
{
public:
  binlog_stmt_cache_data(bool trx_cache_arg,
                        my_off_t max_binlog_cache_size_arg,
                        ulong *ptr_binlog_cache_use_arg,
                        ulong *ptr_binlog_cache_disk_use_arg)
    : binlog_cache_data(trx_cache_arg,
                        max_binlog_cache_size_arg,
                        ptr_binlog_cache_use_arg,
                        ptr_binlog_cache_disk_use_arg)
  {
  }

  using binlog_cache_data::finalize;

  int finalize(THD *thd);
};


int
binlog_stmt_cache_data::finalize(THD *thd)
{
  if (flags.immediate)
  {
    if (int error= finalize(thd, NULL))
      return error;
  }
  else
  {
    Query_log_event
      end_evt(thd, STRING_WITH_LEN("COMMIT"), false, false, true, 0, true);
    if (int error= finalize(thd, &end_evt))
      return error;
  }
  return 0;
}


class binlog_trx_cache_data : public binlog_cache_data
{
public:
  binlog_trx_cache_data(bool trx_cache_arg,
                        my_off_t max_binlog_cache_size_arg,
                        ulong *ptr_binlog_cache_use_arg,
                        ulong *ptr_binlog_cache_disk_use_arg)
  : binlog_cache_data(trx_cache_arg,
                      max_binlog_cache_size_arg,
                      ptr_binlog_cache_use_arg,
                      ptr_binlog_cache_disk_use_arg),
    m_cannot_rollback(FALSE), before_stmt_pos(MY_OFF_T_UNDEF)
  {   }

  void reset()
  {
    DBUG_ENTER("reset");
    DBUG_PRINT("enter", ("before_stmt_pos: %llu", (ulonglong) before_stmt_pos));
    m_cannot_rollback= FALSE;
    before_stmt_pos= MY_OFF_T_UNDEF;
    binlog_cache_data::reset();
    DBUG_PRINT("return", ("before_stmt_pos: %llu", (ulonglong) before_stmt_pos));
    DBUG_VOID_RETURN;
  }

  bool cannot_rollback() const
  {
    return m_cannot_rollback;
  }

  void set_cannot_rollback()
  {
    m_cannot_rollback= TRUE;
  }

  my_off_t get_prev_position() const
  {
     return before_stmt_pos;
  }

  void set_prev_position(my_off_t pos)
  {
    DBUG_ENTER("set_prev_position");
    DBUG_PRINT("enter", ("before_stmt_pos: %llu", (ulonglong) before_stmt_pos));
    before_stmt_pos= pos;
    DBUG_PRINT("return", ("before_stmt_pos: %llu", (ulonglong) before_stmt_pos));
    DBUG_VOID_RETURN;
  }

  void restore_prev_position()
  {
    DBUG_ENTER("restore_prev_position");
    DBUG_PRINT("enter", ("before_stmt_pos: %llu", (ulonglong) before_stmt_pos));
    binlog_cache_data::truncate(before_stmt_pos);
    before_stmt_pos= MY_OFF_T_UNDEF;
    DBUG_PRINT("return", ("before_stmt_pos: %llu", (ulonglong) before_stmt_pos));
    DBUG_VOID_RETURN;
  }

  void restore_savepoint(my_off_t pos)
  {
    DBUG_ENTER("restore_savepoint");
    DBUG_PRINT("enter", ("before_stmt_pos: %llu", (ulonglong) before_stmt_pos));
    binlog_cache_data::truncate(pos);
    if (pos <= before_stmt_pos)
      before_stmt_pos= MY_OFF_T_UNDEF;
    DBUG_PRINT("return", ("before_stmt_pos: %llu", (ulonglong) before_stmt_pos));
    DBUG_VOID_RETURN;
  }

  using binlog_cache_data::truncate;

  int truncate(THD *thd, bool all);

private:
  /*
    It will be set TRUE if any statement which cannot be rolled back safely
    is put in trx_cache.
  */
  bool m_cannot_rollback;

  /*
    Binlog position before the start of the current statement.
  */
  my_off_t before_stmt_pos;

  binlog_trx_cache_data& operator=(const binlog_trx_cache_data& info);
  binlog_trx_cache_data(const binlog_trx_cache_data& info);
};

class binlog_cache_mngr {
public:
  binlog_cache_mngr(my_off_t max_binlog_stmt_cache_size_arg,
                    ulong *ptr_binlog_stmt_cache_use_arg,
                    ulong *ptr_binlog_stmt_cache_disk_use_arg,
                    my_off_t max_binlog_cache_size_arg,
                    ulong *ptr_binlog_cache_use_arg,
                    ulong *ptr_binlog_cache_disk_use_arg)
  : stmt_cache(FALSE, max_binlog_stmt_cache_size_arg,
               ptr_binlog_stmt_cache_use_arg,
               ptr_binlog_stmt_cache_disk_use_arg),
    trx_cache(TRUE, max_binlog_cache_size_arg,
              ptr_binlog_cache_use_arg,
              ptr_binlog_cache_disk_use_arg)
  {  }

  binlog_cache_data* get_binlog_cache_data(bool is_transactional)
  {
    if (is_transactional)
      return &trx_cache;
    else
      return &stmt_cache;
  }

  IO_CACHE* get_binlog_cache_log(bool is_transactional)
  {
    return (is_transactional ? &trx_cache.cache_log : &stmt_cache.cache_log);
  }

  /**
    Convenience method to check if both caches are empty.
   */
  bool is_binlog_empty() const {
    return stmt_cache.is_binlog_empty() && trx_cache.is_binlog_empty();
  }

  /*
    clear stmt_cache and trx_cache if they are not empty
  */
  void reset()
  {
    if (!stmt_cache.is_binlog_empty())
      stmt_cache.reset();
    if (!trx_cache.is_binlog_empty())
      trx_cache.reset();
  }

#ifndef DBUG_OFF
  bool dbug_any_finalized() const {
    return stmt_cache.dbug_is_finalized() || trx_cache.dbug_is_finalized();
  }
#endif

  /*
    Convenience method to flush both caches to the binary log.

    @param bytes_written Pointer to variable that will be set to the
                         number of bytes written for the flush.
    @param wrote_xid     Pointer to variable that will be set to @c
                         true if any XID event was written to the
                         binary log. Otherwise, the variable will not
                         be touched.
    @return Error code on error, zero if no error.
   */
  int flush(THD *thd, my_off_t *bytes_written, bool *wrote_xid, bool async)
  {
    my_off_t stmt_bytes= 0;
    my_off_t trx_bytes= 0;
    DBUG_ASSERT(stmt_cache.has_xid() == 0);
    if (int error= stmt_cache.flush(thd, &stmt_bytes, wrote_xid, async))
      return error;
    if (int error= trx_cache.flush(thd, &trx_bytes, wrote_xid, async))
      return error;
    *bytes_written= stmt_bytes + trx_bytes;
    return 0;
  }

  binlog_stmt_cache_data stmt_cache;
  binlog_trx_cache_data trx_cache;

private:

  binlog_cache_mngr& operator=(const binlog_cache_mngr& info);
  binlog_cache_mngr(const binlog_cache_mngr& info);
};


static binlog_cache_mngr *thd_get_cache_mngr(const THD *thd)
{
  /*
    If opt_bin_log is not set, binlog_hton->slot == -1 and hence
    thd_get_ha_data(thd, hton) segfaults.
  */
  DBUG_ASSERT(opt_bin_log);
  return (binlog_cache_mngr *)thd_get_ha_data(thd, binlog_hton);
}


/**
  Checks if the BINLOG_CACHE_SIZE's value is greater than MAX_BINLOG_CACHE_SIZE.
  If this happens, the BINLOG_CACHE_SIZE is set to MAX_BINLOG_CACHE_SIZE.
*/
void check_binlog_cache_size(THD *thd)
{
  if (binlog_cache_size > max_binlog_cache_size)
  {
    if (thd)
    {
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_BINLOG_CACHE_SIZE_GREATER_THAN_MAX,
                          ER(ER_BINLOG_CACHE_SIZE_GREATER_THAN_MAX),
                          (ulong) binlog_cache_size,
                          (ulong) max_binlog_cache_size);
    }
    else
    {
      sql_print_warning(ER_DEFAULT(ER_BINLOG_CACHE_SIZE_GREATER_THAN_MAX),
                        (ulong) binlog_cache_size,
                        (ulong) max_binlog_cache_size);
    }
    binlog_cache_size= max_binlog_cache_size;
  }
}

/**
  Checks if the BINLOG_STMT_CACHE_SIZE's value is greater than MAX_BINLOG_STMT_CACHE_SIZE.
  If this happens, the BINLOG_STMT_CACHE_SIZE is set to MAX_BINLOG_STMT_CACHE_SIZE.
*/
void check_binlog_stmt_cache_size(THD *thd)
{
  if (binlog_stmt_cache_size > max_binlog_stmt_cache_size)
  {
    if (thd)
    {
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_BINLOG_STMT_CACHE_SIZE_GREATER_THAN_MAX,
                          ER(ER_BINLOG_STMT_CACHE_SIZE_GREATER_THAN_MAX),
                          (ulong) binlog_stmt_cache_size,
                          (ulong) max_binlog_stmt_cache_size);
    }
    else
    {
      sql_print_warning(ER_DEFAULT(ER_BINLOG_STMT_CACHE_SIZE_GREATER_THAN_MAX),
                        (ulong) binlog_stmt_cache_size,
                        (ulong) max_binlog_stmt_cache_size);
    }
    binlog_stmt_cache_size= max_binlog_stmt_cache_size;
  }
}

/**
  Updates the HLC tracked by the binlog to a value greater than or equal to the
  one specified in minimum_hlc_ns global system variable
  */
void update_binlog_hlc()
{
  // Update HLC
  mysql_bin_log.update_hlc(minimum_hlc_ns);
}

/**
 Check whether binlog_hton has valid slot and enabled
*/
bool binlog_enabled()
{
	return(binlog_hton && binlog_hton->slot != HA_SLOT_UNDEF);
}

 /*
  Save position of binary log transaction cache.

  SYNPOSIS
    binlog_trans_log_savepos()

    thd      The thread to take the binlog data from
    pos      Pointer to variable where the position will be stored

  DESCRIPTION

    Save the current position in the binary log transaction cache into
    the variable pointed to by 'pos'
 */

static void
binlog_trans_log_savepos(THD *thd, my_off_t *pos)
{
  DBUG_ENTER("binlog_trans_log_savepos");
  DBUG_ASSERT(pos != NULL);
  binlog_cache_mngr *const cache_mngr= thd_get_cache_mngr(thd);
  DBUG_ASSERT(mysql_bin_log.is_open());
  *pos= cache_mngr->trx_cache.get_byte_position();
  DBUG_PRINT("return", ("position: %lu", (ulong) *pos));
  DBUG_VOID_RETURN;
}


/*
  this function is mostly a placeholder.
  conceptually, binlog initialization (now mostly done in MYSQL_BIN_LOG::open)
  should be moved here.
*/

static int binlog_init(void *p)
{
  binlog_hton= (handlerton *)p;
  binlog_hton->state=opt_bin_log ? SHOW_OPTION_YES : SHOW_OPTION_NO;
  binlog_hton->db_type=DB_TYPE_BINLOG;
  binlog_hton->savepoint_offset= sizeof(my_off_t);
  binlog_hton->close_connection= binlog_close_connection;
  binlog_hton->savepoint_set= binlog_savepoint_set;
  binlog_hton->savepoint_rollback= binlog_savepoint_rollback;
  binlog_hton->savepoint_rollback_can_release_mdl=
                                     binlog_savepoint_rollback_can_release_mdl;
  binlog_hton->commit= binlog_commit;
  binlog_hton->rollback= binlog_rollback;
  binlog_hton->prepare= binlog_prepare;
  binlog_hton->flags= HTON_NOT_USER_SELECTABLE | HTON_HIDDEN;

  latency_histogram_init(&histogram_binlog_fsync,
                         histogram_step_size_binlog_fsync);
  counter_histogram_init(&histogram_binlog_group_commit,
                         opt_histogram_step_size_binlog_group_commit);
  return 0;
}

static int binlog_close_connection(handlerton *hton, THD *thd)
{
  DBUG_ENTER("binlog_close_connection");
  binlog_cache_mngr *const cache_mngr= thd_get_cache_mngr(thd);
  DBUG_ASSERT(cache_mngr->is_binlog_empty());
  DBUG_ASSERT(cache_mngr->trx_cache.is_group_cache_empty() &&
              cache_mngr->stmt_cache.is_group_cache_empty());
  DBUG_PRINT("debug", ("Set ha_data slot %d to 0x%llx", binlog_hton->slot, (ulonglong) NULL));
  thd_set_ha_data(thd, binlog_hton, NULL);
  cache_mngr->~binlog_cache_mngr();
  my_free(cache_mngr);
  DBUG_RETURN(0);
}

static bool should_write_gtids(THD *thd) {
  DBUG_EXECUTE_IF("dbug.should_write_gtids",
  {
     const char act[]=
        "now signal should_write_gtids_begin.reached "
        "wait_for should_write_gtids_begin.done";
     DBUG_ASSERT(opt_debug_sync_timeout > 0);
     DBUG_ASSERT(!debug_sync_set_action(thd, STRING_WITH_LEN(act)));
  };);

  /*
    Return false in the situation where slave sql_thread is
    trying to generate gtid's for binlog events received from master. This
    condition is not valid when enable_gtid_mode_on_new_slave_with_old_master
    is true where the variable is used only for testing purposes with 5.1 master
    and 5.6 slave with gtids turned on.

    Note that the check thd->variables.gtid_next.type == AUTOMATIC_GROUP
    is used to ensure that a new gtid is generated for the transaction group,
    instead of using SESSION.gtid_next value.
  */
  if (thd->rli_slave &&
      thd->variables.gtid_next.type == AUTOMATIC_GROUP &&
      !enable_gtid_mode_on_new_slave_with_old_master)
    return false;
  /*
    Return true (allow gtids to be generated) in the scenario where
    read_only is false (i.e; this is a master).

    Return true in the scenario where a GTID_GROUP is being used.
  */
  bool ret= (!opt_readonly || thd->variables.gtid_next.type == GTID_GROUP);

  DBUG_EXECUTE_IF("dbug.should_write_gtids",
  {
     const char act[]=
        "now signal should_write_gtids_end.reached "
        "wait_for should_write_gtids_end.done";
     DBUG_ASSERT(opt_debug_sync_timeout > 0);
     DBUG_ASSERT(!debug_sync_set_action(thd, STRING_WITH_LEN(act)));
   };);

  return ret;
}

int binlog_cache_data::write_event(THD *thd,
                                   Log_event *ev,
                                   bool write_meta_data_event)
{
  DBUG_ENTER("binlog_cache_data::write_event");

  if (gtid_mode > 0 && thd->should_write_gtid)
  {
    Group_cache::enum_add_group_status status=
      group_cache.add_logged_group(thd, get_byte_position());
    if (status == Group_cache::ERROR)
      DBUG_RETURN(1);
    else if (status == Group_cache::APPEND_NEW_GROUP)
    {
      Gtid_log_event gtid_ev(thd, is_trx_cache());
      if (gtid_ev.write(&cache_log) != 0)
        DBUG_RETURN(1);

      /* will be used later (in master) during ordered commit to check if HLC
       * time needs to be updated */
      thd->should_update_hlc= enable_binlog_hlc;
      if (thd->should_update_hlc)
      {
        uint64_t hlc_time_ns= 0;

        /* If this is a master, then just add a placeholder (with 0 as HLC
         * timestamp). The actual commit time HLC timestamp will be updated
         * during ordered commit (binlog flush stage).
         *
         * If this is a slave, then use the commit HLC timestamp generated in
         * the master for this trx */
        if (thd->rli_slave || thd->rli_fake)
        {
          hlc_time_ns= thd->hlc_time_ns_next;
          thd->should_update_hlc= false;
        }

        Metadata_log_event metadata_ev(thd, is_trx_cache(), hlc_time_ns);

        if (thd->rli_slave || thd->rli_fake)
        {
          // When a Metadata event with Raft OpId is picked up from
          // relay log and applied, ev->apply_event in rpl_slave.cc stashes
          // the raft term and index from the event into the THD. Here we
          // pick it up to pass the Raft term and index through to the metadata
          // event of binlog/apply side. Although confusing (as to why we are
          // adding raft metadata even in non-raft cases), this is required
          // to align with current approach of how a new non-raft instance
          // is added to existing raft ring. OpId can only be present in
          // raft rings, hence the exposure of this code is to instances
          // which are tailing raft rings or raft members which are now passing
          // OpId to apply log as well. In all other cases Raft term and index
          // is expected to be -1,-1
          int64_t raft_term, raft_index;
          thd->get_trans_marker(&raft_term, &raft_index);
          if (raft_term != -1 && raft_index != -1)
            metadata_ev.set_raft_term_and_index(raft_term, raft_index);
        }
        if (metadata_ev.write(&cache_log))
          DBUG_RETURN(1);
      }
    }
  }

  if (ev != NULL)
  {
    // case: write meta data event before the real event
    // see @opt_binlog_trx_meta_data
    if (write_meta_data_event)
    {
      std::string metadata= thd->gen_trx_metadata();
      Rows_query_log_event metadata_ev(thd, metadata.c_str(),
          metadata.length());
      if (metadata_ev.write(&cache_log) != 0)
        DBUG_RETURN(1);
    }

    DBUG_EXECUTE_IF("simulate_disk_full_at_binlog_cache_write",
                    { DBUG_SET("+d,simulate_no_free_space_error"); });

    DBUG_EXECUTE_IF("simulate_tmpdir_partition_full",
                  {
                  static int count= -1;
                  count++;
                  if(count % 4 == 3 && ev->get_type_code() == WRITE_ROWS_EVENT)
                    DBUG_SET("+d,simulate_temp_file_write_error");
                  });

    if (ev->write(&cache_log) != 0)
    {
      DBUG_EXECUTE_IF("simulate_temp_file_write_error",
                      {
                        DBUG_SET("-d,simulate_temp_file_write_error");
                      });
      /*
        If the flush has failed due to ENOSPC error, set the
        flush_error flag.
      */
      if (thd->is_error() && my_errno == ENOSPC)
      {
        set_flush_error(thd);
      }
      DBUG_RETURN(1);
    }
    DBUG_EXECUTE_IF("simulate_disk_full_at_binlog_cache_write",
                    // this flag is cleared by my_write.cc but we clear it
                    // explicitly in case if the even didn't hit my_write.cc
                    // so the flag won't affect not targeted calls
                    { DBUG_SET("-d,simulate_no_free_space_error"); });
    if (ev->get_type_code() == XID_EVENT)
      flags.with_xid= true;
    if (ev->is_using_immediate_logging())
      flags.immediate= true;
  }
  DBUG_RETURN(0);
}


/**
  Checks if the given GTID exists in the Group_cache. If not, add it
  as an empty group.

  @todo Move this function into the cache class?

  @param thd THD object that owns the Group_cache
  @param cache_data binlog_cache_data object for the cache
  @param gtid GTID to check
*/
static int write_one_empty_group_to_cache(THD *thd,
                                          binlog_cache_data *cache_data,
                                          Gtid gtid)
{
  DBUG_ENTER("write_one_empty_group_to_cache");
  Group_cache *group_cache= &cache_data->group_cache;
  if (group_cache->contains_gtid(gtid))
    DBUG_RETURN(0);
  /*
    Apparently this code is not being called. We need to
    investigate if this is a bug or this code is not
    necessary. /Alfranio

    Empty groups are currently being handled in the function
    gtid_empty_group_log_and_cleanup().
  */
  DBUG_ASSERT(0); /*NOTREACHED*/
#ifdef NON_ERROR_GTID
  IO_CACHE *cache= &cache_data->cache_log;
  Group_cache::enum_add_group_status status= group_cache->add_empty_group(gtid);
  if (status == Group_cache::ERROR)
    DBUG_RETURN(1);
  DBUG_ASSERT(status == Group_cache::APPEND_NEW_GROUP);
  Gtid_specification spec= { GTID_GROUP, gtid };
  Gtid_log_event gtid_ev(thd, cache_data->is_trx_cache(), &spec);
  if (gtid_ev.write(cache) != 0)
    DBUG_RETURN(1);
#endif
  DBUG_RETURN(0);
}

/**
  Writes all GTIDs that the thread owns to the stmt/trx cache, if the
  GTID is not already in the cache.

  @todo Move this function into the cache class?

  @param thd THD object for the thread that owns the cache.
  @param cache_data The cache.
*/
static int write_empty_groups_to_cache(THD *thd, binlog_cache_data *cache_data)
{
  DBUG_ENTER("write_empty_groups_to_cache");
  if (thd->owned_gtid.sidno == -1)
  {
#ifdef HAVE_GTID_NEXT_LIST
    Gtid_set::Gtid_iterator git(&thd->owned_gtid_set);
    Gtid gtid= git.get();
    while (gtid.sidno != 0)
    {
      if (write_one_empty_group_to_cache(thd, cache_data, gtid) != 0)
        DBUG_RETURN(1);
      git.next();
      gtid= git.get();
    }
#else
    DBUG_ASSERT(0);
#endif
  }
  else if (thd->owned_gtid.sidno > 0)
    if (write_one_empty_group_to_cache(thd, cache_data, thd->owned_gtid) != 0)
      DBUG_RETURN(1);
  DBUG_RETURN(0);
}

/**
 * Update the HLC timestamp in the cache during ordered commit
 *
 * @param thd - the THD in group commit
 * @cache_data - The cache that needs to be updated with commit time HLC
 *
 * @return zero on success, non-zero on failure
 */
static int hlc_before_write_cache(THD* thd, binlog_cache_data* cache_data)
{
  /* Update commit time HLC timestamp for this trx */
  if (!thd->should_update_hlc)
    return 0;

  /* Get next HLC timestamp */
  uint64_t hlc_time_ns= mysql_bin_log.get_next_hlc();
  int result= 0;

  Metadata_log_event metadata_ev(
      thd, cache_data->is_trx_cache(), hlc_time_ns);

  if (metadata_ev.write(&cache_data->cache_log))
    result= 1;

  /* Update session tracker with hlc timestamp of this trx */
  auto tracker= thd->session_tracker.get_tracker(SESSION_RESP_ATTR_TRACKER);
  if (!result && thd->variables.response_attrs_contain_hlc &&
      tracker->is_enabled())
  {
    static LEX_CSTRING key= { STRING_WITH_LEN("hlc_ts") };
    std::string value_str= std::to_string(hlc_time_ns);
    LEX_CSTRING value= { value_str.c_str(), value_str.length() };
    tracker->mark_as_changed(thd, &key, &value);
  }

  thd->should_update_hlc= false;

  // This is used later on a master instance to update per-database applied hlc
  thd->hlc_time_ns_next= hlc_time_ns;

  return result;
}


/**

  @todo Move this function into the cache class?
 */
static int
gtid_before_write_cache(THD* thd, binlog_cache_data* cache_data)
{
  DBUG_ENTER("gtid_before_write_cache");
  int error= 0;

  DBUG_ASSERT(thd->variables.gtid_next.type != UNDEFINED_GROUP);

  if (gtid_mode == 0 || !thd->should_write_gtid)
  {
    DBUG_RETURN(0);
  }

  Group_cache* group_cache= &cache_data->group_cache;

  global_sid_lock->rdlock();

  if (thd->variables.gtid_next.type == AUTOMATIC_GROUP)
  {
    if (group_cache->generate_automatic_gno(thd) !=
        RETURN_STATUS_OK)
    {
      global_sid_lock->unlock();
      DBUG_RETURN(1);
    }
  }
  if (write_empty_groups_to_cache(thd, cache_data) != 0)
  {
    global_sid_lock->unlock();
    DBUG_RETURN(1);
  }

  global_sid_lock->unlock();

  /*
    If an automatic group number was generated, change the first event
    into a "real" one.
  */
  if (thd->variables.gtid_next.type == AUTOMATIC_GROUP)
  {
    DBUG_ASSERT(group_cache->get_n_groups() == 1);
    Cached_group *cached_group= group_cache->get_unsafe_pointer(0);
    DBUG_ASSERT(cached_group->spec.type != AUTOMATIC_GROUP);
    Gtid_log_event gtid_ev(thd, cache_data->is_trx_cache(),
                           &cached_group->spec);
    bool using_file= cache_data->cache_log.pos_in_file > 0;

    DBUG_EXECUTE_IF("simulate_tmpdir_partition_full",
                  {
                  DBUG_SET("+d,simulate_temp_file_write_error");
                  });

    my_off_t saved_position= cache_data->reset_write_pos(0, using_file);

    if (!cache_data->cache_log.error)
    {
      if (gtid_ev.write(&cache_data->cache_log))
        goto err;

      /* Update commit time HLC timestamp for this trx */
      hlc_before_write_cache(thd, cache_data);

      cache_data->reset_write_pos(saved_position, using_file);
    }

    if (cache_data->cache_log.error)
      goto err;
  }

  DBUG_RETURN(error);

err:
  DBUG_EXECUTE_IF("simulate_tmpdir_partition_full",
                {
                DBUG_SET("-d,simulate_temp_file_write_error");
                });
  /*
    If the reinit_io_cache has failed, set the flush_error flag.
  */
  if (cache_data->cache_log.error)
  {
    cache_data->set_flush_error(thd);
  }
  DBUG_RETURN(1);

}

/**
   The function logs an empty group with GTID and performs cleanup.
   Its logic wrt GTID is equivalent to one of binlog_commit().
   It's called at the end of statement execution in case binlog_commit()
   was skipped.
   Such cases are due ineffective binlogging incl an empty group
   re-execution.

   @param thd   The thread handle

   @return
    nonzero if an error pops up.
*/
int gtid_empty_group_log_and_cleanup(THD *thd)
{
  int ret= 1;
  binlog_cache_data* cache_data= NULL;

  DBUG_ENTER("gtid_empty_group_log_and_cleanup");

  Query_log_event qinfo(thd, STRING_WITH_LEN("BEGIN"), TRUE,
                          FALSE, TRUE, 0, TRUE);
  DBUG_ASSERT(!qinfo.is_using_immediate_logging());

  /*
    thd->cache_mngr is uninitialized on the first empty transaction.
  */
  if (thd->binlog_setup_trx_data())
    DBUG_RETURN(1);
  cache_data= &thd_get_cache_mngr(thd)->trx_cache;
  DBUG_PRINT("debug", ("Writing to trx_cache"));
  if (cache_data->write_event(thd, &qinfo) ||
      gtid_before_write_cache(thd, cache_data))
    goto err;

  ret= mysql_bin_log.commit(thd, true, false);

err:
  DBUG_RETURN(ret);
}

/**
  This function finalizes the cache preparing for commit or rollback.

  The function just writes all the necessary events to the cache but
  does not flush the data to the binary log file. That is the role of
  the binlog_cache_data::flush function.

  @see binlog_cache_data::flush

  @param thd                The thread whose transaction should be flushed
  @param cache_data         Pointer to the cache
  @param end_ev             The end event either commit/rollback

  @return
    nonzero if an error pops up when flushing the cache.
*/
int
binlog_cache_data::finalize(THD *thd, Log_event *end_event)
{
  DBUG_ENTER("binlog_cache_data::finalize");
  if (!is_binlog_empty())
  {
    DBUG_ASSERT(!flags.finalized);
    if (int error= flush_pending_event(thd))
      DBUG_RETURN(error);
    if (int error= write_event(thd, end_event))
      DBUG_RETURN(error);
    flags.finalized= true;
    DBUG_PRINT("debug", ("flags.finalized: %s", YESNO(flags.finalized)));
  }
  DBUG_RETURN(0);
}

/**
  Flush caches to the binary log.

  If the cache is finalized, the cache will be flushed to the binary
  log file. If the cache is not finalized, nothing will be done.

  If flushing fails for any reason, an error will be reported and the
  cache will be reset. Flushing can fail in two circumstances:

  - It was not possible to write the cache to the file. In this case,
    it does not make sense to keep the cache.

  - The cache was successfully written to disk but post-flush actions
    (such as binary log rotation) failed. In this case, the cache is
    already written to disk and there is no reason to keep it.

  @see binlog_cache_data::finalize
 */
int
binlog_cache_data::flush(THD *thd, my_off_t *bytes_written, bool *wrote_xid,
                         bool async)
{
  /*
    Doing a commit or a rollback including non-transactional tables,
    i.e., ending a transaction where we might write the transaction
    cache to the binary log.

    We can always end the statement when ending a transaction since
    transactions are not allowed inside stored functions. If they
    were, we would have to ensure that we're not ending a statement
    inside a stored function.
  */
  DBUG_ENTER("binlog_cache_data::flush");
  DBUG_PRINT("debug", ("flags.finalized: %s", YESNO(flags.finalized)));
  int error= 0;
  if (flags.finalized)
  {
    my_off_t bytes_in_cache= my_b_tell(&cache_log);
    DBUG_PRINT("debug", ("bytes_in_cache: %llu", bytes_in_cache));
    /*
      The cache is always reset since subsequent rollbacks of the
      transactions might trigger attempts to write to the binary log
      if the cache is not reset.
     */
    error= gtid_before_write_cache(thd, this);

    if (!error && enable_raft_plugin_save && !mysql_bin_log.is_apply_log) {
      error= RUN_HOOK_STRICT(raft_replication, before_flush,
                             (thd, &cache_log));

      DBUG_EXECUTE_IF("fail_binlog_flush_raft", {error= 1;});

      /*
       * before_flush hook failing is a guarantee by raft that any subsequent
       * replicate message sent to raft (through before_flush) hook fails (in
       * this group and in subsequent groups). In other words, raft will
       * initiate a step down and will not take any more writes. This
       * is necessary condition to avoid having holes or duplicates in
       * executed_gtid
       */
      if (error)
      {
        // Calling into mysql_raft plugin failed. Set commit consensus error.
        // This will ensure that if this THD's trx is allowed to proceed to
        // commit stage, then we rollback the trx
        thd->commit_consensus_error= true;
      }

      // Do post write book keeping activities
      error= mysql_bin_log.post_write(thd, this, error);
    }
    else if (!error)
    {
      /* TODO:
       * 1. Eventually, the write to binlog cache will happen through consensus
       * plugin. So, remove the call here when raft plugin is enabled
       * 2. Dynamic enabling/disabling of raft plugin is a lot trickier and not
       * handled in this diff. We have to make sure that whatever txn's are
       * inside the plugin finish flushing to binlog file before we allow new
       * txns to flush in mysql (outside the plugin)
       * 3. Check if we even need the ability to dynamically disable raft plugin
       * on a running mysql instance. Gracefull handling of disabling raft
       * plugin means we have to block new txns until all txns inside RAFT are
       * committed. Non-graceful handling would just fail all txns inside RAFT -
       * in which case the instance cannot take any more txns which is as good
       * as restarting the instance
       */
      // Continue to write to binlog in mysql when raft is not enabled
      error= mysql_bin_log.write_cache(thd, this, async);
    }

    if (error)
      thd->commit_error= THD::CE_FLUSH_ERROR;

    if (flags.with_xid && error == 0)
      *wrote_xid= true;

    /*
      Reset have to be after the if above, since it clears the
      with_xid flag
    */
    reset();
    if (bytes_written)
      *bytes_written= bytes_in_cache;
  }
  DBUG_ASSERT(!flags.finalized);
  DBUG_RETURN(error);
}

/**
  This function truncates the transactional cache upon committing or rolling
  back either a transaction or a statement.

  @param thd        The thread whose transaction should be flushed
  @param cache_mngr Pointer to the cache data to be flushed
  @param all        @c true means truncate the transaction, otherwise the
                    statement must be truncated.

  @return
    nonzero if an error pops up when truncating the transactional cache.
*/
int
binlog_trx_cache_data::truncate(THD *thd, bool all)
{
  DBUG_ENTER("binlog_trx_cache_data::truncate");
  int error=0;

  DBUG_PRINT("info", ("thd->options={ %s %s}, transaction: %s",
                      FLAGSTR(thd->variables.option_bits, OPTION_NOT_AUTOCOMMIT),
                      FLAGSTR(thd->variables.option_bits, OPTION_BEGIN),
                      all ? "all" : "stmt"));

  remove_pending_event();

  /*
    If rolling back an entire transaction or a single statement not
    inside a transaction, we reset the transaction cache.
  */
  if (ending_trans(thd, all))
  {
    if (has_incident())
      error= mysql_bin_log.write_incident(thd, true/*need_lock_log=true*/);
    reset();
  }
  /*
    If rolling back a statement in a transaction, we truncate the
    transaction cache to remove the statement.
  */
  else if (get_prev_position() != MY_OFF_T_UNDEF)
  {
    restore_prev_position();
    if (is_binlog_empty())
    {
      /*
        After restoring the previous position, we need to check if
        the cache is empty. In such case, the group cache needs to
        be cleaned up too because the GTID is removed too from the
        cache.

        So if any change happens again, the GTID must be rewritten
        and this will not happen if the group cache is not cleaned
        up.

        After integrating this with NDB, we need to check if the
        current approach is enough or the group cache needs to
        explicitly support rollback to savepoints.
      */
      group_cache.clear();
    }
  }

  thd->clear_binlog_table_maps();

  DBUG_RETURN(error);
}

static int binlog_prepare(handlerton *hton, THD *thd, bool all, bool async)
{
  /*
    do nothing.
    just pretend we can do 2pc, so that MySQL won't
    switch to 1pc.
    real work will be done in MYSQL_BIN_LOG::commit()
  */
  return 0;
}

/**
  This function is called once after each statement.

  @todo This function is currently not used any more and will
  eventually be eliminated. The real commit job is done in the
  MYSQL_BIN_LOG::commit function.

  @see MYSQL_BIN_LOG::commit

  @param hton  The binlog handlerton.
  @param thd   The client thread that executes the transaction.
  @param all   This is @c true if this is a real transaction commit, and
               @false otherwise.

  @see handlerton::commit
*/
static int binlog_commit(handlerton *hton, THD *thd, bool all, bool async)
{
  DBUG_ENTER("binlog_commit");
  /*
    Nothing to do (any more) on commit.
   */
  DBUG_RETURN(0);
}

/**
  This function is called when a transaction or a statement is rolled back.

  @internal It is necessary to execute a rollback here if the
  transaction was rolled back because of executing a ROLLBACK TO
  SAVEPOINT command, but it is not used for normal rollback since
  MYSQL_BIN_LOG::rollback is called in that case.

  @todo Refactor code to introduce a <code>MYSQL_BIN_LOG::rollback(THD
  *thd, SAVEPOINT *sv)</code> function in @c TC_LOG and have that
  function execute the necessary work to rollback to a savepoint.

  @param hton  The binlog handlerton.
  @param thd   The client thread that executes the transaction.
  @param all   This is @c true if this is a real transaction rollback, and
               @false otherwise.

  @see handlerton::rollback
*/
static int binlog_rollback(handlerton *hton, THD *thd, bool all)
{
  DBUG_ENTER("binlog_rollback");
  int error= 0;
  if (thd->lex->sql_command == SQLCOM_ROLLBACK_TO_SAVEPOINT)
    error= mysql_bin_log.rollback(thd, all);
  DBUG_RETURN(error);
}

#ifndef DBUG_OFF
static void wait_for_follower(THD* thd)
{
  const char act[]=
    "now signal group_leader_selected wait_for group_follower_added";

  DBUG_ASSERT(!debug_sync_set_action(thd, STRING_WITH_LEN(act)));
}

static void signal_leader(THD* thd)
{
  const char act[]=
    "now signal group_follower_added";

  DBUG_ASSERT(!debug_sync_set_action(thd, STRING_WITH_LEN(act)));
}

static void wait_for_leader(THD* thd)
{
  const char act[]=
    "now wait_for group_leader_selected";

  DBUG_ASSERT(!debug_sync_set_action(thd, STRING_WITH_LEN(act)));
}
#endif

bool
Stage_manager::Mutex_queue::append(THD *first)
{
  DBUG_ENTER("Stage_manager::Mutex_queue::append");
  lock();
  DBUG_PRINT("enter", ("first: 0x%llx", (ulonglong) first));
  DBUG_PRINT("info", ("m_first: 0x%llx, &m_first: 0x%llx, m_last: 0x%llx",
                       (ulonglong) m_first, (ulonglong) &m_first,
                       (ulonglong) m_last));

  DBUG_ASSERT(first->prepared_engine != NULL);
  if (unlikely(!group_prepared_engine))
    group_prepared_engine= new engine_lsn_map();

  if (!first->prepared_engine->is_empty())
    group_prepared_engine->compare_and_update(
        first->prepared_engine->get_maps());

  bool empty= (m_first == NULL);
  *m_last= first;
  DBUG_PRINT("info", ("m_first: 0x%llx, &m_first: 0x%llx, m_last: 0x%llx",
                       (ulonglong) m_first, (ulonglong) &m_first,
                       (ulonglong) m_last));
  /*
    Go to the last THD instance of the list. We expect lists to be
    moderately short. If they are not, we need to track the end of
    the queue as well.
  */
  while (first->next_to_commit)
    first= first->next_to_commit;
  m_last= &first->next_to_commit;
  DBUG_PRINT("info", ("m_first: 0x%llx, &m_first: 0x%llx, m_last: 0x%llx",
                        (ulonglong) m_first, (ulonglong) &m_first,
                        (ulonglong) m_last));
  DBUG_ASSERT(m_first || m_last == &m_first);
  DBUG_PRINT("return", ("empty: %s", YESNO(empty)));
  unlock();
  DBUG_RETURN(empty);
}

bool
Stage_manager::enroll_for(StageID stage, THD *thd, mysql_mutex_t *leave_mutex,
                          mysql_mutex_t *enter_mutex)
{
  // If the queue was empty: we're the leader for this batch
  DBUG_PRINT("debug", ("Enqueue 0x%llx to queue for stage %d",
                       (ulonglong) thd, stage));

  DBUG_ASSERT(enter_mutex);

  DBUG_EXECUTE_IF("become_group_follower",
                  {
                    if (stage == FLUSH_STAGE)
                      wait_for_leader(thd);
                  });

  bool leader= m_queue[stage].append(thd);

  DBUG_EXECUTE_IF("become_group_leader",
                  {
                    if (stage == FLUSH_STAGE)
                      wait_for_follower(thd);
                  });

  DBUG_EXECUTE_IF("become_group_follower",
                  {
                    if (stage == FLUSH_STAGE)
                      signal_leader(thd);
                  });

#ifdef HAVE_REPLICATION
  // case: we have waited for our turn and will be committing next so unregister
  if (stage == FLUSH_STAGE && has_commit_order_manager(thd))
  {
    Slave_worker *worker= dynamic_cast<Slave_worker *>(thd->rli_slave);
    Commit_order_manager *mngr= worker->get_commit_order_manager();

    mngr->unregister_trx(worker);
  }
#endif

  /*
    If the leader, lock the enter_mutex before unlocking the leave_mutex in
    order to ensure that MYSQL_BIN_LOG::lock_commits cannot acquire all the
    group commit mutexes while a group commit is only partially complete.
  */
  if (leader)
    mysql_mutex_lock(enter_mutex);

  /*
    The stage mutex can be NULL if we are enrolling for the first
    stage.
  */
  if (leave_mutex)
    mysql_mutex_unlock(leave_mutex);

  /*
    If the queue was not empty, we're a follower and wait for the
    leader to process the queue. If we were holding a mutex, we have
    to release it before going to sleep.
  */
  if (!leader)
  {
    mysql_mutex_lock(&m_lock_done);
#ifndef DBUG_OFF
    /*
      Leader can be awaiting all-clear to preempt follower's execution.
      With setting the status the follower ensures it won't execute anything
      including thread-specific code.
    */
    thd->transaction.flags.ready_preempt= 1;
    if (leader_await_preempt_status)
      mysql_cond_signal(&m_cond_preempt);
#endif
    while (thd->transaction.flags.pending)
      mysql_cond_wait(&m_cond_done, &m_lock_done);
    mysql_mutex_unlock(&m_lock_done);
  }
  return leader;
}


THD *Stage_manager::Mutex_queue::fetch_and_empty()
{
  DBUG_ENTER("Stage_manager::Mutex_queue::fetch_and_empty");
  lock();
  DBUG_PRINT("enter", ("m_first: 0x%llx, &m_first: 0x%llx, m_last: 0x%llx",
                       (ulonglong) m_first, (ulonglong) &m_first,
                       (ulonglong) m_last));
  THD *result= m_first;
  m_first= NULL;
  m_last= &m_first;
  DBUG_PRINT("info", ("m_first: 0x%llx, &m_first: 0x%llx, m_last: 0x%llx",
                       (ulonglong) m_first, (ulonglong) &m_first,
                       (ulonglong) m_last));
  DBUG_ASSERT(m_first || m_last == &m_first);
  DBUG_PRINT("return", ("result: 0x%llx", (ulonglong) result));

  /* Restore group_prepared_engine so the caller can extract it from result. */
  if (!group_prepared_engine->is_empty())
  {
    result->prepared_engine->compare_and_update(group_prepared_engine->get_maps());
    /* Reset group_prepared_engine when the queue is empty. */
    group_prepared_engine->clear();
  }

  unlock();
  DBUG_RETURN(result);
}

#ifndef DBUG_OFF
void Stage_manager::clear_preempt_status(THD *head)
{
  DBUG_ASSERT(head);

  mysql_mutex_lock(&m_lock_done);
  while(!head->transaction.flags.ready_preempt)
  {
    leader_await_preempt_status= true;
    mysql_cond_wait(&m_cond_preempt, &m_lock_done);
  }
  leader_await_preempt_status= false;
  mysql_mutex_unlock(&m_lock_done);
}
#endif

uint64_t HybridLogicalClock::get_next()
{
  uint64_t current_hlc, next_hlc;
  bool done= false;

  while (!done)
  {
    // Get the current wall clock in nanosecond precision
    uint64_t current_wall_clock=
      std::chrono::duration_cast<std::chrono::nanoseconds>(
       std::chrono::high_resolution_clock::now().time_since_epoch()).count();

    // get the 'current' internal HLC
    current_hlc= current_.load();

    // Next HLC timestamp is max of current-hlc and current-wall-clock
    next_hlc= std::max(current_hlc + 1, current_wall_clock);

    // Conditionally update the internal hlc
    done= current_.compare_exchange_strong(current_hlc, next_hlc);
  }

  return next_hlc;
}

uint64_t HybridLogicalClock::get_current()
{
  return current_.load();
}

uint64_t HybridLogicalClock::update(uint64_t minimum_hlc)
{
  uint64_t current_hlc, new_min_hlc;
  bool done= false;

  while (!done)
  {
    // get the 'current' internal HLC
    current_hlc= current_.load();

    // Next HLC timestamp is max of current-hlc, minimum-hlc
    new_min_hlc= std::max(minimum_hlc, current_hlc);

    // Conditionally update the internal hlc
    done= current_.compare_exchange_strong(current_hlc, new_min_hlc);
  }

  return new_min_hlc;
}

void HybridLogicalClock::update_database_hlc(
    const std::unordered_set<std::string> &databases, uint64_t applied_hlc) {
  std::vector<std::shared_ptr<DatabaseEntry>> entries;
  {
    std::unique_lock<std::mutex> lock(database_map_lock_);
    for (const auto &database : databases) {
      auto entry = getEntry(database);
      DBUG_ASSERT(entry);
      entries.push_back(std::move(entry));
    }
  }
  
  for (const auto& entry : entries) {
    entry->update_hlc(applied_hlc);
  }
}

void HybridLogicalClock::get_database_hlc(
    std::unordered_map<std::string, uint64_t> &applied_hlc) {
  std::unique_lock<std::mutex> lock(database_map_lock_);
  for (const auto& record : database_map_) {
    applied_hlc.emplace(record.first, record.second->max_applied_hlc());
  }
}

uint64_t
HybridLogicalClock::get_selected_database_hlc(const std::string& database) {
  std::unique_lock<std::mutex> lock(database_map_lock_);

  const auto it = database_map_.find(database);
  return it != database_map_.end() ? it->second->max_applied_hlc() : 0;
}

void HybridLogicalClock::clear_database_hlc() {
  std::unique_lock<std::mutex> lock(database_map_lock_);
  database_map_.clear();
}

bool HybridLogicalClock::wait_for_hlc_applied(THD *thd,
                                              TABLE_LIST *all_tables) {
  if (!(thd->variables.enable_block_stale_hlc_read && thd->db &&
        !thd->slave_thread)) {
    return false;
  }

  // There are some tables we do not want to perform HLC checks on, since
  // they are local to the instance and intentionally not replicated
  for (TABLE_LIST *table = all_tables; table; table = table->next_global) {
    bool good_table =
        !my_strcasecmp(&my_charset_latin1, table->db, "mysql") ||
        !my_strcasecmp(&my_charset_latin1, table->db, "information_schema") ||
        !my_strcasecmp(&my_charset_latin1, table->db, "performance_schema");
    if (good_table) {
      return false;
    }
  }

  auto it = thd->query_attrs_map.find(hlc_ts_lower_bound);
  if (it == thd->query_attrs_map.end()) {
    // No lower bound HLC ts specified, skip early
    return false;
  }
  const std::string &hlc_ts_str = it->second.c_str();

  const char *hlc_wait_timeout_str = NULL;
  it = thd->query_attrs_map.find(hlc_wait_timeout_ms);
  if (it != thd->query_attrs_map.end()) {
    hlc_wait_timeout_str = it->second.c_str();
  }

  // Behavior of this feature on reads inside of a transaction is complex
  // and not supported at this point in time.
  if (thd->in_active_multi_stmt_transaction()) {
    my_error(ER_HLC_READ_BOUND_IN_TRANSACTION, MYF(0));
    return true;
  }

  // The implementation makes the assumption that
  // allow_noncurrent_db_rw = OFF and only reads to the current database
  // are allowed
  if (thd->variables.allow_noncurrent_db_rw != 3 /* OFF */) {
    my_error(ER_INVALID_NONCURRENT_DB_RW_FOR_HLC_READ_BOUND, MYF(0),
             thd->variables.allow_noncurrent_db_rw);
    return true;
  }

  char *endptr = nullptr;
  uint64_t requested_hlc = strtoull(hlc_ts_str.c_str(), &endptr, 10);
  if (!endptr || *endptr != '\0' ||
      !HybridLogicalClock::is_valid_hlc(requested_hlc)) {
    my_error(ER_INVALID_HLC_READ_BOUND, MYF(0), hlc_ts_str.c_str());
    return true;
  }

  endptr = nullptr;
  uint64_t timeout_ms = wait_for_hlc_timeout_ms;
  if (hlc_wait_timeout_str) {
    timeout_ms = strtoull(hlc_wait_timeout_str, &endptr, 10);
    if (!endptr || *endptr != '\0') {
      my_error(ER_INVALID_HLC_WAIT_TIMEOUT, MYF(0), hlc_wait_timeout_str);
      return true;
    }
  }

  std::string db(thd->db, thd->db_length);

  uint64_t applied_hlc = mysql_bin_log.get_selected_database_hlc(db);
  if (requested_hlc > applied_hlc &&
      (timeout_ms == 0 || !wait_for_hlc_timeout_ms)) {
    my_error(ER_STALE_HLC_READ, MYF(0), requested_hlc, db.c_str());
    return true;
  }

  // Return early if the waiting feature isn't enabled
  if (!wait_for_hlc_timeout_ms) return false;

  std::shared_ptr<DatabaseEntry> entry = nullptr;
  {
    std::unique_lock<std::mutex> lock(database_map_lock_);
    auto entryIt = database_map_.find(db);
    if (entryIt == database_map_.end()) {
      entry = std::make_shared<DatabaseEntry>();
      database_map_.emplace(db, entry);
    } else {
      entry = entryIt->second;
    }
  }

  return entry->wait_for_hlc(thd, requested_hlc, timeout_ms);
}

void HybridLogicalClock::DatabaseEntry::update_hlc(uint64_t applied_hlc) {
  // CAS loop to update max_applied_hlc if less than the new applied HLC
  uint64_t original = max_applied_hlc_;
  while (original < applied_hlc &&
         !max_applied_hlc_.compare_exchange_strong(original, applied_hlc)) {
  }

  // Signal the list of waiters with requested HLCs close to the current applied
  // value
  if (wait_for_hlc_timeout_ms) {
    mysql_cond_broadcast(&cond_);
  }
}

bool HybridLogicalClock::DatabaseEntry::wait_for_hlc(THD *thd,
                                                    uint64_t requested_hlc,
                                                    uint64_t timeout_ms) {
  auto start_time = std::chrono::steady_clock::now();
  while (max_applied_hlc_ < requested_hlc) {
    // HLC is nano-second granularity, scale down to millis
    auto delta_ms = (requested_hlc - max_applied_hlc_) / 1000000ULL;

    int64_t remaining_timeout_ms = 0;
    bool sleeping = true;

    uint64_t total_duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time)
            .count();

    // Look at the delta between the current applied HLC and the requested
    // HLC. If there is a large gap, go ahead and block (sleep) on the local
    // mutex/condvar combination to stall until the database has almost caught
    // up. Once the database is almost caught up, then go ahead and block on
    // the wait queue that is released on binlog application.
    //
    // We use the delta in requested_hlc - applied_hlc as an approximation
    // for how long to wait. Seconds behind master is difficult because its
    // unclear in the sequence of lagged binlog records the unapplied HLC exists
    // and thus how long to wait
    if (delta_ms > wait_for_hlc_sleep_threshold_ms) {
      // For large deltas, go ahead and send the thread to sleep for a bit
      // Use std::min() for pathological cases where there is a low write rate
      // and thus a large gap in HLC values
      remaining_timeout_ms =
          std::min(delta_ms * wait_for_hlc_sleep_scaling_factor, 100.0);
      sleeping = true;
    } else {
      // Once the HLC is close to being applied, block on the list of waiters
      // to be released when new binlog events are applied to the database
      remaining_timeout_ms = timeout_ms - total_duration_ms;
      sleeping = false;
    }

    if (remaining_timeout_ms <= 0 || total_duration_ms > timeout_ms) {
      my_error(ER_HLC_WAIT_TIMEDOUT, MYF(0), requested_hlc);
      return true;
    }

    if (sleeping) {
      const char* save_proc_info = thd_proc_info(thd, "Waiting for database applied HLC");
      std::this_thread::sleep_for(std::chrono::milliseconds{remaining_timeout_ms});
      thd_proc_info(thd, save_proc_info);
    } else {
      struct timespec timeout;
      set_timespec_nsec(timeout, remaining_timeout_ms * 1000000ULL);
      PSI_stage_info old_stage;
      mysql_mutex_lock(&mutex_);
      thd->ENTER_COND(&cond_, &mutex_, &stage_waiting_for_hlc, &old_stage);
      thd_wait_begin(thd, THD_WAIT_FOR_HLC);

      int error = mysql_cond_timedwait(&cond_, &mutex_, &timeout);

      thd->EXIT_COND(&old_stage);
      thd_wait_end(thd);

      // When intentionally sleeping to stall, we expect a timeout
      if (error == ETIMEDOUT || error == ETIME) {
        my_error(ER_HLC_WAIT_TIMEDOUT, MYF(0), requested_hlc);
        return true;
      }
    }

    if (thd_killed(thd)) {
      my_error(ER_QUERY_INTERRUPTED, MYF(0));
      return true;
    }
  }

  // Return the total wait time back to the client (for instrumentation
  // purposes)
  auto tracker = thd->session_tracker.get_tracker(SESSION_RESP_ATTR_TRACKER);
  if (thd->variables.response_attrs_contain_hlc && tracker->is_enabled()) {
    LEX_CSTRING key = {STRING_WITH_LEN("hlc_wait_duration_ms")};
    auto time = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time);
    std::string value_str = std::to_string(time.count());
    LEX_CSTRING value = {value_str.c_str(), value_str.length()};
    tracker->mark_as_changed(thd, &key, &value);
  }

  return false;
}

/**
  Write a rollback record of the transaction to the binary log.

  For binary log group commit, the rollback is separated into three
  parts:

  1. First part consists of filling the necessary caches and
     finalizing them (if they need to be finalized). After a cache is
     finalized, nothing can be added to the cache.

  2. Second part execute an ordered flush and commit. This will be
     done using the group commit functionality in @c ordered_commit.

     Since we roll back the transaction early, we call @c
     ordered_commit with the @c skip_commit flag set. The @c
     ha_commit_low call inside @c ordered_commit will then not be
     called.

  3. Third part checks any errors resulting from the flush and handles
     them appropriately.

  @see MYSQL_BIN_LOG::ordered_commit
  @see ha_commit_low
  @see ha_rollback_low

  @param thd Session to commit
  @param all This is @c true if this is a real transaction rollback, and
             @false otherwise.

  @return Error code, or zero if there were no error.
 */

int MYSQL_BIN_LOG::rollback(THD *thd, bool all)
{
  int error= 0;
  bool stuff_logged= false;

  binlog_cache_mngr *const cache_mngr= thd_get_cache_mngr(thd);
  DBUG_ENTER("MYSQL_BIN_LOG::rollback(THD *thd, bool all)");
  DBUG_PRINT("enter", ("all: %s, cache_mngr: 0x%llx, thd->is_error: %s",
                       YESNO(all), (ulonglong) cache_mngr, YESNO(thd->is_error())));

  /*
    We roll back the transaction in the engines early since this will
    release locks and allow other transactions to start executing.

    If we are executing a ROLLBACK TO SAVEPOINT, we should only clear
    the caches since this function is called as part of the engine
    rollback.
   */
  if (thd->lex->sql_command != SQLCOM_ROLLBACK_TO_SAVEPOINT)
    if ((error= ha_rollback_low(thd, all)))
      goto end;

  /*
    If there is no cache manager, or if there is nothing in the
    caches, there are no caches to roll back, so we're trivially done.
   */
  if (cache_mngr == NULL || cache_mngr->is_binlog_empty())
    goto end;

  DBUG_PRINT("debug",
             ("all.cannot_safely_rollback(): %s, trx_cache_empty: %s",
              YESNO(thd->transaction.all.cannot_safely_rollback()),
              YESNO(cache_mngr->trx_cache.is_binlog_empty())));
  DBUG_PRINT("debug",
             ("stmt.cannot_safely_rollback(): %s, stmt_cache_empty: %s",
              YESNO(thd->transaction.stmt.cannot_safely_rollback()),
              YESNO(cache_mngr->stmt_cache.is_binlog_empty())));

  /*
    If an incident event is set we do not flush the content of the statement
    cache because it may be corrupted.
  */
  if (cache_mngr->stmt_cache.has_incident())
  {
    error= write_incident(thd, true/*need_lock_log=true*/);
    cache_mngr->stmt_cache.reset();
  }
  else if (!cache_mngr->stmt_cache.is_binlog_empty())
  {
    if ((error= cache_mngr->stmt_cache.finalize(thd)))
      goto end;
    stuff_logged= true;
  }

  if (ending_trans(thd, all))
  {
    if (trans_cannot_safely_rollback(thd))
    {
      /*
        If the transaction is being rolled back and contains changes that
        cannot be rolled back, the trx-cache's content is flushed.
      */
      Query_log_event
        end_evt(thd, STRING_WITH_LEN("ROLLBACK"), true, false, true, 0, true);
      error= cache_mngr->trx_cache.finalize(thd, &end_evt);
      stuff_logged= true;
    }
    else
    {
      /*
        If the transaction is being rolled back and its changes can be
        rolled back, the trx-cache's content is truncated.
      */
      error= cache_mngr->trx_cache.truncate(thd, all);
    }
  }
  else
  {
    /*
      If a statement is being rolled back, it is necessary to know
      exactly why a statement may not be safely rolled back as in
      some specific situations the trx-cache can be truncated.

      If a temporary table is created or dropped, the trx-cache is not
      truncated. Note that if the stmt-cache is used, there is nothing
      to truncate in the trx-cache.

      If a non-transactional table is updated and the binlog format is
      statement, the trx-cache is not truncated. The trx-cache is used
      when the direct option is off and a transactional table has been
      updated before the current statement in the context of the
      current transaction. Note that if the stmt-cache is used there is
      nothing to truncate in the trx-cache.

      If other binlog formats are used, updates to non-transactional
      tables are written to the stmt-cache and trx-cache can be safely
      truncated, if necessary.
    */
    if (thd->transaction.stmt.has_dropped_temp_table() ||
        thd->transaction.stmt.has_created_temp_table() ||
        (thd->transaction.stmt.has_modified_non_trans_table() &&
        thd->variables.binlog_format == BINLOG_FORMAT_STMT))
    {
      /*
        If the statement is being rolled back and dropped or created a
        temporary table or modified a non-transactional table and the
        statement-based replication is in use, the statement's changes
        in the trx-cache are preserved.
      */
      cache_mngr->trx_cache.set_prev_position(MY_OFF_T_UNDEF);
    }
    else
    {
      /*
        Otherwise, the statement's changes in the trx-cache are
        truncated.
      */
      error= cache_mngr->trx_cache.truncate(thd, all);
    }
  }

  DBUG_PRINT("debug", ("error: %d", error));
  if (error == 0 && stuff_logged)
    error= ordered_commit(thd, all, /* skip_commit */ true);

  if (check_write_error(thd))
  {
    /*
      "all == true" means that a "rollback statement" triggered the error and
      this function was called. However, this must not happen as a rollback
      is written directly to the binary log. And in auto-commit mode, a single
      statement that is rolled back has the flag all == false.
    */
    DBUG_ASSERT(!all);
    /*
      We reach this point if the effect of a statement did not properly get into
      a cache and need to be rolled back.
    */
    error |= cache_mngr->trx_cache.truncate(thd, all);
  }

end:
  /*
    When a statement errors out on auto-commit mode it is rollback
    implicitly, so the same should happen to its GTID.
  */
  if (!thd->in_active_multi_stmt_transaction())
    gtid_rollback(thd);

  DBUG_PRINT("return", ("error: %d", error));
  DBUG_RETURN(error);
}

/**
  @note
  How do we handle this (unlikely but legal) case:
  @verbatim
    [transaction] + [update to non-trans table] + [rollback to savepoint] ?
  @endverbatim
  The problem occurs when a savepoint is before the update to the
  non-transactional table. Then when there's a rollback to the savepoint, if we
  simply truncate the binlog cache, we lose the part of the binlog cache where
  the update is. If we want to not lose it, we need to write the SAVEPOINT
  command and the ROLLBACK TO SAVEPOINT command to the binlog cache. The latter
  is easy: it's just write at the end of the binlog cache, but the former
  should be *inserted* to the place where the user called SAVEPOINT. The
  solution is that when the user calls SAVEPOINT, we write it to the binlog
  cache (so no need to later insert it). As transactions are never intermixed
  in the binary log (i.e. they are serialized), we won't have conflicts with
  savepoint names when using mysqlbinlog or in the slave SQL thread.
  Then when ROLLBACK TO SAVEPOINT is called, if we updated some
  non-transactional table, we don't truncate the binlog cache but instead write
  ROLLBACK TO SAVEPOINT to it; otherwise we truncate the binlog cache (which
  will chop the SAVEPOINT command from the binlog cache, which is good as in
  that case there is no need to have it in the binlog).
*/

static int binlog_savepoint_set(handlerton *hton, THD *thd, void *sv)
{
  DBUG_ENTER("binlog_savepoint_set");
  int error= 1;

  String log_query;
  if (log_query.append(STRING_WITH_LEN("SAVEPOINT ")))
    DBUG_RETURN(error);
  else
    append_identifier(thd, &log_query, thd->lex->ident.str,
                      thd->lex->ident.length);

  int errcode= query_error_code(thd, thd->killed == THD::NOT_KILLED);
  Query_log_event qinfo(thd, log_query.c_ptr_safe(), log_query.length(),
                        TRUE, FALSE, TRUE, errcode);
  /*
    We cannot record the position before writing the statement
    because a rollback to a savepoint (.e.g. consider it "S") would
    prevent the savepoint statement (i.e. "SAVEPOINT S") from being
    written to the binary log despite the fact that the server could
    still issue other rollback statements to the same savepoint (i.e.
    "S").
    Given that the savepoint is valid until the server releases it,
    ie, until the transaction commits or it is released explicitly,
    we need to log it anyway so that we don't have "ROLLBACK TO S"
    or "RELEASE S" without the preceding "SAVEPOINT S" in the binary
    log.
  */
  if (!(error= mysql_bin_log.write_event(&qinfo)))
    binlog_trans_log_savepos(thd, (my_off_t*) sv);

  DBUG_RETURN(error);
}

static int binlog_savepoint_rollback(handlerton *hton, THD *thd, void *sv)
{
  DBUG_ENTER("binlog_savepoint_rollback");
  binlog_cache_mngr *const cache_mngr= thd_get_cache_mngr(thd);
  my_off_t pos= *(my_off_t*) sv;
  DBUG_ASSERT(pos != ~(my_off_t) 0);

  /*
    Write ROLLBACK TO SAVEPOINT to the binlog cache if we have updated some
    non-transactional table. Otherwise, truncate the binlog cache starting
    from the SAVEPOINT command.
  */
  if (trans_cannot_safely_rollback(thd))
  {
    String log_query;
    if (log_query.append(STRING_WITH_LEN("ROLLBACK TO ")))
      DBUG_RETURN(1);
    else
    {
      /*
        Before writing identifier to the binlog, make sure to
        quote the identifier properly so as to prevent any SQL
        injection on the slave.
      */
      append_identifier(thd, &log_query, thd->lex->ident.str,
                        thd->lex->ident.length);
    }

    int errcode= query_error_code(thd, thd->killed == THD::NOT_KILLED);
    Query_log_event qinfo(thd, log_query.c_ptr_safe(), log_query.length(),
                          TRUE, FALSE, TRUE, errcode);
    DBUG_RETURN(mysql_bin_log.write_event(&qinfo));
  }
  // Otherwise, we truncate the cache
  cache_mngr->trx_cache.restore_savepoint(pos);
  /*
    When a SAVEPOINT is executed inside a stored function/trigger we force the
    pending event to be flushed with a STMT_END_F flag and clear the table maps
    as well to ensure that following DMLs will have a clean state to start
    with. ROLLBACK inside a stored routine has to finalize possibly existing
    current row-based pending event with cleaning up table maps. That ensures
    that following DMLs will have a clean state to start with.
   */
  if (thd->in_sub_stmt)
    thd->clear_binlog_table_maps();
  if (cache_mngr->trx_cache.is_binlog_empty())
    cache_mngr->trx_cache.group_cache.clear();
  DBUG_RETURN(0);
}

/**
  Check whether binlog state allows to safely release MDL locks after
  rollback to savepoint.

  @param hton  The binlog handlerton.
  @param thd   The client thread that executes the transaction.

  @return true  - It is safe to release MDL locks.
          false - If it is not.
*/
static bool binlog_savepoint_rollback_can_release_mdl(handlerton *hton,
                                                      THD *thd)
{
  DBUG_ENTER("binlog_savepoint_rollback_can_release_mdl");
  /*
    If we have not updated any non-transactional tables rollback
    to savepoint will simply truncate binlog cache starting from
    SAVEPOINT command. So it should be safe to release MDL acquired
    after SAVEPOINT command in this case.
  */
  DBUG_RETURN(!trans_cannot_safely_rollback(thd));
}

#ifdef HAVE_REPLICATION

static int log_in_use(const char* log_name)
{
  size_t log_name_len = strlen(log_name) + 1;
  int thread_count=0;
#ifndef DBUG_OFF
  if (current_thd)
    DEBUG_SYNC(current_thd,"purge_logs_after_lock_index_before_thread_count");
#endif
  mutex_lock_all_shards(SHARDED(&LOCK_thread_count));

  Thread_iterator it= global_thread_list_begin();
  Thread_iterator end= global_thread_list_end();
  for (; it != end; ++it)
  {
    LOG_INFO* linfo;
    if ((linfo = (*it)->current_linfo))
    {
      mysql_mutex_lock(&linfo->lock);
      if(!strncmp(log_name, linfo->log_file_name, log_name_len))
      {
        thread_count++;
        sql_print_warning("file %s was not purged because it was being read "
                          "by thread number %u", log_name,
                          (*it)->thread_id());
      }
      mysql_mutex_unlock(&linfo->lock);
    }
  }

  mutex_unlock_all_shards(SHARDED(&LOCK_thread_count));
  return thread_count;
}

static bool purge_error_message(THD* thd, int res)
{
  uint errcode;

  if ((errcode= purge_log_get_error_code(res)) != 0)
  {
    my_message(errcode, ER(errcode), MYF(0));
    return TRUE;
  }
  my_ok(thd);
  return FALSE;
}

#endif /* HAVE_REPLICATION */

int check_binlog_magic(IO_CACHE* log, const char** errmsg)
{
  char magic[4];
  DBUG_ASSERT(my_b_tell(log) == 0);

  if (my_b_read(log, (uchar*) magic, sizeof(magic)))
  {
    *errmsg = "I/O error reading the header from the binary log";
    sql_print_error("%s, errno=%d, io cache code=%d", *errmsg, my_errno,
		    log->error);
    return 1;
  }
  if (memcmp(magic, BINLOG_MAGIC, sizeof(magic)))
  {
    *errmsg = "Binlog has bad magic number;  It's not a binary log file that can be used by this version of MySQL";
    return 1;
  }
  return 0;
}


File open_binlog_file(IO_CACHE *log, const char *log_file_name, const char **errmsg)
{
  File file;
  DBUG_ENTER("open_binlog_file");

  if ((file= mysql_file_open(key_file_binlog,
                             log_file_name, O_RDONLY | O_BINARY | O_SHARE,
                             MYF(MY_WME))) < 0)
  {
    sql_print_error("Failed to open log (file '%s', errno %d)",
                    log_file_name, my_errno);
    *errmsg = "Could not open log file";
    goto err;
  }
  if (init_io_cache(log, file, rpl_read_size, READ_CACHE, 0, 0,
                    MYF(MY_WME|MY_DONT_CHECK_FILESIZE)))
  {
    sql_print_error("Failed to create a cache on log (file '%s')",
                    log_file_name);
    *errmsg = "Could not open log file";
    goto err;
  }
  if (check_binlog_magic(log,errmsg))
    goto err;
  DBUG_RETURN(file);

err:
  if (file >= 0)
  {
    mysql_file_close(file, MYF(0));
    end_io_cache(log);
  }
  DBUG_RETURN(-1);
}

/**
  This function checks if binlog cache is empty.

  @param thd The client thread that executed the current statement.
  @return
    @c true if binlog cache is empty, @c false otherwise.
*/
bool
is_binlog_cache_empty(const THD* thd)
{
  binlog_cache_mngr *const cache_mngr= thd_get_cache_mngr(thd);

  return (cache_mngr ? cache_mngr->is_binlog_empty() : 1);
}

/**
  This function checks if a transactional table was updated by the
  current transaction.

  @param thd The client thread that executed the current statement.
  @return
    @c true if a transactional table was updated, @c false otherwise.
*/
bool
trans_has_updated_trans_table(const THD* thd)
{
  binlog_cache_mngr *const cache_mngr= thd_get_cache_mngr(thd);

  return (cache_mngr ? !cache_mngr->trx_cache.is_binlog_empty() : 0);
}

/**
  This function checks if a transactional table was updated by the
  current statement.

  @param ha_list Registered storage engine handler list.
  @return
    @c true if a transactional table was updated, @c false otherwise.
*/
bool
stmt_has_updated_trans_table(Ha_trx_info* ha_list)
{
  Ha_trx_info *ha_info;

  for (ha_info= ha_list; ha_info; ha_info= ha_info->next())
  {
    if (ha_info->is_trx_read_write() && ha_info->ht() != binlog_hton)
      return (TRUE);
  }
  return (FALSE);
}

/**
  This function checks if a transaction, either a multi-statement
  or a single statement transaction is about to commit or not.

  @param thd The client thread that executed the current statement.
  @param all Committing a transaction (i.e. TRUE) or a statement
             (i.e. FALSE).
  @return
    @c true if committing a transaction, otherwise @c false.
*/
bool ending_trans(THD* thd, const bool all)
{
  return (all || ending_single_stmt_trans(thd, all));
}

/**
  This function checks if a single statement transaction is about
  to commit or not.

  @param thd The client thread that executed the current statement.
  @param all Committing a transaction (i.e. TRUE) or a statement
             (i.e. FALSE).
  @return
    @c true if committing a single statement transaction, otherwise
    @c false.
*/
bool ending_single_stmt_trans(THD* thd, const bool all)
{
  return (!all && !thd->in_multi_stmt_transaction_mode());
}

/**
  This function checks if a transaction cannot be rolled back safely.

  @param thd The client thread that executed the current statement.
  @return
    @c true if cannot be safely rolled back, @c false otherwise.
*/
bool trans_cannot_safely_rollback(const THD* thd)
{
  binlog_cache_mngr *const cache_mngr= thd_get_cache_mngr(thd);

  return cache_mngr->trx_cache.cannot_rollback();
}

/**
  This function checks if current statement cannot be rollded back safely.

  @param thd The client thread that executed the current statement.
  @return
    @c true if cannot be safely rolled back, @c false otherwise.
*/
bool stmt_cannot_safely_rollback(const THD* thd)
{
  return thd->transaction.stmt.cannot_safely_rollback();
}

#ifndef EMBEDDED_LIBRARY
/**
  Execute a PURGE BINARY LOGS TO <log> command.

  @param thd Pointer to THD object for the client thread executing the
  statement.

  @param to_log Name of the last log to purge.

  @retval FALSE success
  @retval TRUE failure
*/
bool purge_master_logs(THD* thd, const char* to_log)
{
  char search_file_name[FN_REFLEN];
  if (!mysql_bin_log.is_open())
  {
    my_ok(thd);
    return FALSE;
  }

  mysql_bin_log.make_log_name(search_file_name, to_log);
  return purge_error_message(thd,
                             mysql_bin_log.purge_logs(search_file_name, false,
                                                      true/*need_lock_index=true*/,
                                                      true/*need_update_threads=true*/,
                                                      NULL, false));
}


/**
  Execute a PURGE BINARY LOGS BEFORE <date> command.

  @param thd Pointer to THD object for the client thread executing the
  statement.

  @param purge_time Date before which logs should be purged.

  @retval FALSE success
  @retval TRUE failure
*/
bool purge_master_logs_before_date(THD* thd, time_t purge_time)
{
  if (!mysql_bin_log.is_open())
  {
    my_ok(thd);
    return 0;
  }
  return purge_error_message(thd,
                             mysql_bin_log.purge_logs_before_date(purge_time,
                                                                  false));
}

/**
  Execute a PURGE RAFT LOGS TO <log-name> command.

  @param thd Pointer to THD object for the client thread executing the
  statement.

  @param to_log Name of the last log to purge.

  @retval FALSE success
  @retval TRUE failure
*/
bool purge_raft_logs(THD* thd, const char* to_log)
{
  // This is no-op when raft is not enabled
  if (!enable_raft_plugin)
    return FALSE;

  // If mysql_bin_log is not an apply log, then it represents the 'raft logs' on
  // leader. Call purge_master_logs() to handle the purge correctly
  if (!mysql_bin_log.is_apply_log)
    return purge_master_logs(thd, to_log);

  if (active_mi == NULL || active_mi->rli == NULL)
    return FALSE;

  mysql_mutex_lock(&LOCK_active_mi);
  if (active_mi->rli == NULL)
  {
    mysql_mutex_unlock(&LOCK_active_mi);
    return FALSE;
  }

  Relay_log_info* rli = active_mi->rli;

  // Lock requirement and ordering based on SQL appliers next_event loop
  mysql_mutex_lock(&rli->data_lock);
  mysql_mutex_lock(rli->relay_log.get_lock_index());

  char search_file_name[FN_REFLEN];
  mysql_bin_log.make_log_name(search_file_name, to_log);

  // Note that we pass max_log as group_relay_log_name. This is because we
  // should not purge anything that is still needed by sql appliers.
  // group_relay_log_name should be captured by 'in_use' check in
  // purge_logs(). However when sql_threads are stopped and a purge command is
  // issued, then 'in_use' check will not be sufficient and we might end up
  // deleting raft logs which are not yet applied. Hence we explicitly pass
  // 'max_log' asking purge_logs() to not purge anything at or beyond 'max_log'
  bool error= purge_error_message(
      thd,
      rli->relay_log.purge_logs(
        search_file_name,
        /*included=*/false,
        /*need_lock_index=*/false,
        /*need_update_threads=*/true,
        /*decrease_log_space=*/nullptr,
        /*auto_purge=*/false,
        rli->get_group_relay_log_name()));

  if (!error)
    error= update_relay_log_cordinates(rli);

  mysql_mutex_unlock(rli->relay_log.get_lock_index());
  mysql_mutex_unlock(&rli->data_lock);
  mysql_mutex_unlock(&LOCK_active_mi);

  return error;
}

/**
  Execute a PURGE RAFT LOGS BEFORE <date> command.

  @param thd Pointer to THD object for the client thread executing the
  statement.

  @param purge_time Date before which logs should be purged.

  @retval FALSE success
  @retval TRUE failure
*/
bool purge_raft_logs_before_date(THD* thd, time_t purge_time)
{
  // This is no-op when raft is not enabled
  if (!enable_raft_plugin)
    return FALSE;

  // If mysql_bin_log is not an apply log, then it represents the 'raft logs' on
  // leader. Call purge_master_logs_before_date() to handle the purge
  // correctly
  if (!mysql_bin_log.is_apply_log)
    return purge_master_logs_before_date(thd, purge_time);

  if (active_mi == NULL || active_mi->rli == NULL)
    return FALSE;

  mysql_mutex_lock(&LOCK_active_mi);
  if (active_mi->rli == NULL)
  {
    mysql_mutex_unlock(&LOCK_active_mi);
    return FALSE;
  }

  Relay_log_info* rli = active_mi->rli;

  // Lock requirement and ordering based SQL appliers next_event loop
  mysql_mutex_lock(&rli->data_lock);
  mysql_mutex_lock(rli->relay_log.get_lock_index());

  // Note that we pass max_log as group_relay_log_name. This is because we
  // should not purge anything that is still needed by sql appliers.
  // group_relay_log_name should be captured by 'in_use' check in
  // purge_logs(). However, when sql_threads are stopped and a purge command is
  // issued, then 'in_use' check will not be sufficient and we might end up
  // deleting raft logs which are not yet applied. Hence, we explicitly pass
  // 'max_log' asking purge_logs() to not purge anything at or beyond 'max_log'
  auto error= purge_error_message(
      thd, rli->relay_log.purge_logs_before_date(
        purge_time,
        /*auto_purge=*/false,
        /*stop_purge=*/false,
        /*need_lock_index=*/false,
        rli->get_group_relay_log_name()));

  if (!error)
    error= update_relay_log_cordinates(rli);

  mysql_mutex_unlock(rli->relay_log.get_lock_index());
  mysql_mutex_unlock(&rli->data_lock);
  mysql_mutex_unlock(&LOCK_active_mi);

  return error;
}

/**
  Updates the index file cordinates in relay log info. All required locks need
  to be acquired by the caller

  @param rli Relay log info that needs to be updated

  @retval FALSE success
  @retval TRUE failure
*/
bool update_relay_log_cordinates(Relay_log_info* rli)
{
  int error= 0;

  error= rli->relay_log.find_log_pos(
      &rli->linfo,
      rli->get_event_relay_log_name(),
      /*need_lock_index=*/false);

  if (error)
  {
    // This is a fatal error. SQL threads wont be able to read relay logs to
    // apply trxs.
    char buff[22];
    // NO_LINT_DEBUG
    sql_print_error("find_log_pos error during purge_raft_logs: %d  "
        "offset: %s, log: %s",
        error,
        llstr(rli->linfo.index_file_offset, buff),
        rli->get_group_relay_log_name());

    if (binlog_error_action == ABORT_SERVER ||
        binlog_error_action == ROLLBACK_TRX)
    {
      exec_binlog_error_action_abort("Could not update relay log position "
                                     "for applier threads. Aborting server");
    }
  }

  return error;
}

bool show_raft_status(THD* thd)
{
  Protocol *protocol= thd->protocol;
  List<Item> field_list;
  size_t max_var= 0;
  size_t max_value= 0;
  const char *errmsg = 0;
  std::vector<std::pair<std::string, std::string>> var_value_pairs;
  std::vector<std::pair<std::string, std::string>>::const_iterator itr;

  int error= RUN_HOOK_STRICT(
      raft_replication, show_raft_status, (current_thd, &var_value_pairs));
  if (error)
  {
    errmsg= "Failure to run plugin hook";
    goto err;
  }

  for (itr= var_value_pairs.begin(); itr != var_value_pairs.end(); ++itr)
  {
    max_var= std::max(max_var, itr->first.length() + 10);
    max_value= std::max(max_value, itr->second.length() + 10);
  }

  field_list.push_back(new Item_empty_string("Variable_name", max_var));
  field_list.push_back(new Item_empty_string("Value", max_value));
  if (protocol->send_result_set_metadata(
        &field_list, Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
  {
    errmsg= "Failure during protocol send metadata";
    goto err;
  }

  for (itr= var_value_pairs.begin(); itr != var_value_pairs.end(); ++itr)
  {
    protocol->prepare_for_resend();
    protocol->store(itr->first.c_str(), itr->first.length(), &my_charset_bin);
    protocol->store(itr->second.c_str(), itr->second.length(), &my_charset_bin);
    if (protocol->write())
    {
      errmsg= "Failure during protocol write";
      goto err;
    }
  }

  my_eof(thd);
  return 0;

err:
  my_error(ER_ERROR_WHEN_EXECUTING_COMMAND, MYF(0), "SHOW RAFT STATUS", errmsg);
  return 1;
}

/**
  Implement 'show raft logs' sql command
  @param thd Thread descriptor

  @retval FALSE success
  @retval TRUE failure
*/
bool show_raft_logs(THD* thd, bool with_gtid)
{
  uint length;
  char file_name_and_gtid_set_length[FN_REFLEN + 22];
  File file;
  LOG_INFO cur;
  bool exit_loop= false;
  const char *errmsg= 0;

  // Redirect to show_binlog() on leader instances
  if (!mysql_bin_log.is_apply_log)
    return show_binlogs(thd, with_gtid);

  if (active_mi == NULL || active_mi->rli == NULL)
  {
    my_error(ER_ERROR_WHEN_EXECUTING_COMMAND, MYF(0),
             "SHOW RAFT LOGS",
             "No master info or relay log info present");
    return TRUE;
  }

  mysql_mutex_lock(&LOCK_active_mi);
  if (active_mi->rli == NULL)
  {
    mysql_mutex_unlock(&LOCK_active_mi);
    my_error(ER_ERROR_WHEN_EXECUTING_COMMAND, MYF(0),
             "SHOW RAFT LOGS",
             "No relay log info present");
    return TRUE;
  }

  Relay_log_info* rli = active_mi->rli;

  // Capture the current log
  rli->relay_log.get_current_log(&cur, /*need_lock_log=*/true);

  // Prevent new files from sneaking ino the index beyond this point. We only
  // read in the index till cur.log_file_name
  mysql_mutex_lock(rli->relay_log.get_lock_index());

  IO_CACHE *index_file= rli->relay_log.get_index_file();
  Protocol *protocol= thd->protocol;

  List<Item> field_list;
  field_list.push_back(new Item_empty_string("Log_name", 255));
  field_list.push_back(
      new Item_return_int("File_size", 20, MYSQL_TYPE_LONGLONG));
   if (with_gtid)
     field_list.push_back(new Item_empty_string("Prev_gtid_set", 0));

  int error= 0;
  if (protocol->send_result_set_metadata(
        &field_list, Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
  {
    error= 1;
    errmsg= "Protocol failed to send metadata";
    goto err;
  }

  reinit_io_cache(index_file, READ_CACHE, (my_off_t) 0, 0, 0);

  /* The file ends with EOF or empty line */
  while ((length=my_b_gets(
          index_file, file_name_and_gtid_set_length, FN_REFLEN + 22)) > 1 &&
      !exit_loop)
  {
    int dir_len;
    ulonglong file_length= 0;                   // Length if open fails

    file_name_and_gtid_set_length[length - 1] = 0;
    uint gtid_set_length =
      split_file_name_and_gtid_set_length(file_name_and_gtid_set_length);
    if (gtid_set_length)
    {
      my_b_seek(index_file,
                my_b_tell(index_file) + gtid_set_length + 1);
    }

    char *fname = file_name_and_gtid_set_length;
    length = strlen(fname);
    dir_len= dirname_length(fname);
    length-= dir_len;

    protocol->prepare_for_resend();
    protocol->store(fname + dir_len, length, &my_charset_bin);

    if (!(strncmp(
            fname + dir_len, cur.log_file_name + dir_len, length)))
    {
      /* Reached the position of the current file in the index. State the size
         of this file as cur.pos and exit the loop */
      file_length= cur.pos;
      exit_loop= true;
    }
    else
    {
      /* this is an old log, open it and find the size */
      if ((file= mysql_file_open(key_file_relaylog,
                                 fname, O_RDONLY | O_SHARE | O_BINARY,
                                 MYF(0))) >= 0)
      {
        file_length= (ulonglong) mysql_file_seek(file, 0L, MY_SEEK_END, MYF(0));
        mysql_file_close(file, MYF(0));
      }
    }
    protocol->store(file_length);

    if (with_gtid)
    {
      // Protected by relay_log.LOCK_index
      auto previous_gtid_set_map = rli->relay_log.get_previous_gtid_set_map();
      Sid_map sid_map(NULL);
      Gtid_set gtid_set(&sid_map, NULL);
      auto gtid_str = previous_gtid_set_map->at(std::string(fname));
      if (!gtid_str.empty())
      {
        gtid_set.add_gtid_encoding((const uchar*)gtid_str.c_str(),
                                 gtid_str.length(), NULL);
        char *buf;
        gtid_set.to_string(&buf, &Gtid_set::commented_string_format);
        protocol->store(buf, strlen(buf), &my_charset_bin);
        free(buf);
      }
      else
      {
        protocol->store("", 0, &my_charset_bin);
      }
    }

    if (protocol->write())
    {
      error= 1;
      errmsg= "Failure in protocol write";
      goto err;
    }
  }

  if (index_file->error == -1)
  {
    error= 1;
    errmsg= "Index file error";
    goto err;
  }

err:
  mysql_mutex_unlock(rli->relay_log.get_lock_index());
  mysql_mutex_unlock(&LOCK_active_mi);
  if (errmsg)
  {
    my_error(ER_ERROR_WHEN_EXECUTING_COMMAND, MYF(0),
        "SHOW RAFT LOGS", errmsg);
  } else
  {
    my_eof(thd);
  }

  return error;
}
#endif /* EMBEDDED_LIBRARY */

/*
  Helper function to get the error code of the query to be binlogged.
 */
int query_error_code(THD *thd, bool not_killed)
{
  int error;

  if (not_killed || (thd->killed == THD::KILL_BAD_DATA))
  {
    error= thd->is_error() ? thd->get_stmt_da()->sql_errno() : 0;

    /* thd->get_stmt_da()->sql_errno() might be ER_SERVER_SHUTDOWN or
       ER_QUERY_INTERRUPTED, So here we need to make sure that error
       is not set to these errors when specified not_killed by the
       caller.
    */
    if (error == ER_SERVER_SHUTDOWN || error == ER_QUERY_INTERRUPTED)
      error= 0;
  }
  else
  {
    /* killed status for DELAYED INSERT thread should never be used */
    DBUG_ASSERT(!(thd->system_thread & SYSTEM_THREAD_DELAYED_INSERT));
    error= thd->killed_errno();
  }

  return error;
}


/**
  Copy content of 'from' file from offset to 'to' file.

  - We do the copy outside of the IO_CACHE as the cache
  buffers would just make things slower and more complicated.
  In most cases the copy loop should only do one read.

  @param from          File to copy.
  @param to            File to copy to.
  @param offset        Offset in 'from' file.


  @retval
    0    ok
  @retval
    -1    error
*/
static bool copy_file(IO_CACHE *from, IO_CACHE *to, my_off_t offset)
{
  int bytes_read;
  uchar io_buf[IO_SIZE*2];
  DBUG_ENTER("copy_file");

  mysql_file_seek(from->file, offset, MY_SEEK_SET, MYF(0));
  while(TRUE)
  {
    if ((bytes_read= (int) mysql_file_read(from->file, io_buf, sizeof(io_buf),
                                           MYF(MY_WME)))
        < 0)
      goto err;
    if (DBUG_EVALUATE_IF("fault_injection_copy_part_file", 1, 0))
      bytes_read= bytes_read/2;
    if (!bytes_read)
      break;                                    // end of file
    if (mysql_file_write(to->file, io_buf, bytes_read, MYF(MY_WME | MY_NABP)))
      goto err;
  }

  DBUG_RETURN(0);

err:
  DBUG_RETURN(1);
}


#ifdef HAVE_REPLICATION
/**
   Load data's io cache specific hook to be executed
   before a chunk of data is being read into the cache's buffer
   The fuction instantianates and writes into the binlog
   replication events along LOAD DATA processing.

   @param file  pointer to io-cache
   @retval 0 success
   @retval 1 failure
*/
int log_loaded_block(IO_CACHE* file)
{
  DBUG_ENTER("log_loaded_block");
  LOAD_FILE_INFO *lf_info;
  uint block_len;
  /* buffer contains position where we started last read */
  uchar* buffer= (uchar*) my_b_get_buffer_start(file);
  uint max_event_size= current_thd->variables.max_allowed_packet;
  lf_info= (LOAD_FILE_INFO*) file->arg;
  if (lf_info->thd->is_current_stmt_binlog_format_row())
    DBUG_RETURN(0);
  if (lf_info->last_pos_in_file != HA_POS_ERROR &&
      lf_info->last_pos_in_file >= my_b_get_pos_in_file(file))
    DBUG_RETURN(0);

  for (block_len= (uint) (my_b_get_bytes_in_buffer(file)); block_len > 0;
       buffer += min(block_len, max_event_size),
       block_len -= min(block_len, max_event_size))
  {
    lf_info->last_pos_in_file= my_b_get_pos_in_file(file);
    if (lf_info->wrote_create_file)
    {
      Append_block_log_event a(lf_info->thd, lf_info->thd->db, buffer,
                               min(block_len, max_event_size),
                               lf_info->log_delayed);
      if (mysql_bin_log.write_event(&a))
        DBUG_RETURN(1);
    }
    else
    {
      Begin_load_query_log_event b(lf_info->thd, lf_info->thd->db,
                                   buffer,
                                   min(block_len, max_event_size),
                                   lf_info->log_delayed);
      if (mysql_bin_log.write_event(&b))
        DBUG_RETURN(1);
      lf_info->wrote_create_file= 1;
    }
  }
  DBUG_RETURN(0);
}

/* Helper function for SHOW BINLOG/RELAYLOG EVENTS */
bool show_binlog_events(THD *thd, MYSQL_BIN_LOG *binary_log)
{
  Protocol *protocol= thd->protocol;
  List<Item> field_list;
  const char *errmsg = 0;
  bool ret = TRUE;
  IO_CACHE log;
  File file = -1;
  int old_max_allowed_packet= thd->variables.max_allowed_packet;
  LOG_INFO linfo(binary_log->is_relay_log);

  DBUG_ENTER("show_binlog_events");

  DBUG_ASSERT(thd->lex->sql_command == SQLCOM_SHOW_BINLOG_EVENTS ||
              thd->lex->sql_command == SQLCOM_SHOW_RELAYLOG_EVENTS);

  Format_description_log_event *description_event= new
    Format_description_log_event(3); /* MySQL 4.0 by default */

  if (binary_log->is_open())
  {
    LEX_MASTER_INFO *lex_mi= &thd->lex->mi;
    SELECT_LEX_UNIT *unit= &thd->lex->unit;
    ha_rows event_count, limit_start, limit_end;
    my_off_t pos = max<my_off_t>(BIN_LOG_HEADER_SIZE, lex_mi->pos); // user-friendly
    char search_file_name[FN_REFLEN], *name;
    const char *log_file_name = lex_mi->log_file_name;
    mysql_mutex_t *log_lock = binary_log->get_log_lock();
    Log_event* ev;

    unit->set_limit(thd->lex->current_select);
    limit_start= unit->offset_limit_cnt;
    limit_end= unit->select_limit_cnt;

    name= search_file_name;
    if (log_file_name)
      binary_log->make_log_name(search_file_name, log_file_name);
    else
      name=0;					// Find first log

    linfo.index_file_offset = 0;

    if (binary_log->find_log_pos(&linfo, name, true/*need_lock_index=true*/))
    {
      errmsg = "Could not find target log";
      goto err;
    }

    mutex_lock_shard(SHARDED(&LOCK_thread_count), thd);
    thd->current_linfo = &linfo;
    mutex_unlock_shard(SHARDED(&LOCK_thread_count), thd);

    if ((file=open_binlog_file(&log, linfo.log_file_name, &errmsg)) < 0)
      goto err;

    my_off_t end_pos;
    /*
      Acquire LOCK_log only for the duration to calculate the
      log's end position. LOCK_log should be acquired even while
      we are checking whether the log is active log or not.
    */
    mysql_mutex_lock(log_lock);
    if (binary_log->is_active(linfo.log_file_name))
    {
      LOG_INFO li;
      binary_log->get_current_log(&li, false /*LOCK_log is already acquired*/);
      end_pos= li.pos;
    }
    else
    {
      end_pos= my_b_filelength(&log);
    }
    mysql_mutex_unlock(log_lock);

    /*
      to account binlog event header size
    */
    thd->variables.max_allowed_packet += MAX_LOG_EVENT_HEADER;

    DEBUG_SYNC(thd, "after_show_binlog_event_found_file");

    /*
      open_binlog_file() sought to position 4.
      Read the first event in case it's a Format_description_log_event, to
      know the format. If there's no such event, we are 3.23 or 4.x. This
      code, like before, can't read 3.23 binlogs.
      This code will fail on a mixed relay log (one which has Format_desc then
      Rotate then Format_desc).
    */
    ev= Log_event::read_log_event(&log, (mysql_mutex_t*)0, description_event,
                                  opt_master_verify_checksum, NULL);
    if (ev)
    {
      if (ev->get_type_code() == FORMAT_DESCRIPTION_EVENT)
      {
        delete description_event;
        description_event= (Format_description_log_event*) ev;
      }
      else
        delete ev;
    }

    my_b_seek(&log, pos);

    if (!description_event->is_valid())
    {
      errmsg="Invalid Format_description event; could be out of memory";
      goto err;
    }

    for (event_count = 0;
         (ev = Log_event::read_log_event(&log, (mysql_mutex_t*) 0,
                                         description_event,
                                         opt_master_verify_checksum,
                                         NULL)); )
    {
      DEBUG_SYNC(thd, "wait_in_show_binlog_events_loop");
      if (ev->get_type_code() == FORMAT_DESCRIPTION_EVENT)
        description_event->checksum_alg= ev->checksum_alg;

      if (event_count >= limit_start &&
	  ev->net_send(protocol, linfo.log_file_name, pos))
      {
	errmsg = "Net error";
	delete ev;
	goto err;
      }

      pos = my_b_tell(&log);
      delete ev;

      if (++event_count >= limit_end || pos >= end_pos)
	break;
    }

    if (event_count < limit_end && log.error)
    {
      errmsg = "Wrong offset or I/O error";
      goto err;
    }

  }
  // Check that linfo is still on the function scope.
  DEBUG_SYNC(thd, "after_show_binlog_events");

  ret= FALSE;

err:
  delete description_event;
  if (file >= 0)
  {
    end_io_cache(&log);
    mysql_file_close(file, MYF(MY_WME));
  }

  if (errmsg)
    my_error(ER_ERROR_WHEN_EXECUTING_COMMAND, MYF(0),
             "SHOW BINLOG EVENTS", errmsg);
  else
    my_eof(thd);

  mutex_lock_shard(SHARDED(&LOCK_thread_count), thd);
  thd->current_linfo = 0;
  mutex_unlock_shard(SHARDED(&LOCK_thread_count), thd);
  thd->variables.max_allowed_packet= old_max_allowed_packet;
  DBUG_RETURN(ret);
}

/* Helper function for SHOW BINLOG CACHE */
bool show_binlog_cache(THD *thd)
{
  Protocol *protocol= thd->protocol;
  const char *errmsg = 0;
  bool ret = false;

  DBUG_ENTER("show_binlog_cache");

  DBUG_ASSERT(thd->lex->sql_command == SQLCOM_SHOW_BINLOG_CACHE);

  uint16 binlog_ver = BINLOG_VERSION;
  Format_description_log_event *description_event;

  ulong thread_id_opt = thd->lex->thread_id_opt;
  THD *opt_thd = nullptr;
  if (thread_id_opt && thread_id_opt != thd->thread_id())
  {
    opt_thd = get_opt_thread_with_data_lock(thd, thread_id_opt);
    if (!opt_thd)
      DBUG_RETURN(true);
  }

  description_event = new
    Format_description_log_event(binlog_ver); /* Binlog ver 4 by default */
  SELECT_LEX_UNIT *unit= &thd->lex->unit;
  ha_rows limit_start, limit_end;

  unit->set_limit(thd->lex->current_select);
  limit_start= unit->offset_limit_cnt;
  limit_end= unit->select_limit_cnt;

  IO_CACHE cache_snapshot;
  binlog_cache_mngr *cache_mngr = nullptr;

  if (opt_bin_log)
  {
    if (opt_thd)
      cache_mngr = thd_get_cache_mngr(opt_thd);
    else
      cache_mngr = thd_get_cache_mngr(thd);
  }
  else
    errmsg = "log-bin option is not enabled";

  if (cache_mngr && limit_end-limit_start > 0)
  {
    ha_rows event_count = 0;
    for (int b = 0; b <= 1 && !errmsg && event_count < limit_end; ++b)
    {
      // fetch the stmt and trx caches in turn
      binlog_cache_data *cache_data =
        cache_mngr->get_binlog_cache_data((bool)b);
      if (!cache_data->is_binlog_empty())
      {
        // get current write cache snapshot and work on the copy
        // so we don't block/affect the writer. The actual content
        // of the cache buffer may change and we will validate
        // later when we construct the log_event object
        cache_snapshot = cache_data->cache_log;
        reinit_io_cache(&cache_snapshot, READ_CACHE, 0, 0, 0);
        uint length = my_b_bytes_in_cache(&cache_snapshot), hdr_offs = 0;
        bool use_own_buffer = false;
        // If the cache spills to disk, we need to create an independent copy
        // of cache_snapshot and not to share the read buffer with the write
        // cache. So the temp file content is read into its own read buffer.
        if (!length && cache_data->cache_log.file != -1)
        {
          size_t cache_size = b ? binlog_cache_size : binlog_stmt_cache_size;
          if (init_io_cache(&cache_snapshot, cache_data->cache_log.file,
                             cache_size, READ_CACHE, 0L, 0, MYF(MY_WME)))
          {
            end_io_cache(&cache_snapshot);
            continue;
          }
          use_own_buffer = true;
        }

        do
        {
          while (hdr_offs < length && event_count < limit_end)
          {
            const char *ev_buf =
                (const char *)cache_snapshot.read_pos + hdr_offs;
            uint event_len= uint4korr(ev_buf + EVENT_LEN_OFFSET);

            // move to next event header
            hdr_offs += event_len;

            Log_event *ev = nullptr;
            if (hdr_offs <= length)
              ev = Log_event::read_log_event(ev_buf, event_len, &errmsg,
                                             description_event, false);

            if (ev && ev->is_valid()) // if we get an valid event, output it
            {
              if (event_count++ >= limit_start && ev->net_send(protocol))
              {
                errmsg = "Net error";
                delete ev;
                break;
              }
            }

            if (ev)
              delete ev;
          } // while()
        } while (use_own_buffer && (length= my_b_fill(&cache_snapshot)));

        // cleanup if we initiated our own copy of cache
        if (use_own_buffer)
          end_io_cache(&cache_snapshot);
      }
    } // for()
  }

  // clean up
  if (opt_thd)
    mysql_mutex_unlock(&opt_thd->LOCK_thd_data);

  delete description_event;

  if (errmsg)
    my_error(ER_ERROR_WHEN_EXECUTING_COMMAND, MYF(0),
             "SHOW BINLOG CACHE", errmsg);
  else
    my_eof(thd);

  DBUG_RETURN(ret);
}

/**
  Execute a SHOW BINLOG EVENTS statement.

  @param thd Pointer to THD object for the client thread executing the
  statement.

  @retval FALSE success
  @retval TRUE failure
*/
bool mysql_show_binlog_events(THD* thd)
{
  Protocol *protocol= thd->protocol;
  List<Item> field_list;
  DBUG_ENTER("mysql_show_binlog_events");

  DBUG_ASSERT(thd->lex->sql_command == SQLCOM_SHOW_BINLOG_EVENTS);

  Log_event::init_show_field_list(&field_list);
  if (protocol->send_result_set_metadata(&field_list,
                            Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
    DBUG_RETURN(TRUE);

  /*
    Wait for handlers to insert any pending information
    into the binlog.  For e.g. ndb which updates the binlog asynchronously
    this is needed so that the uses sees all its own commands in the binlog
  */
  ha_binlog_wait(thd);

  DBUG_RETURN(show_binlog_events(thd, &mysql_bin_log));
}

/**
  Execute a SHOW BINLOG CACHE [FOR thread_id] statement.

  @param thd Pointer to THD object for the client thread executing the
  statement.

  @retval FALSE success
  @retval TRUE failure
*/
bool mysql_show_binlog_cache(THD* thd)
{
  Protocol *protocol= thd->protocol;
  List<Item> field_list;
  DBUG_ENTER("mysql_show_binlog_cache");

  DBUG_ASSERT(thd->lex->sql_command == SQLCOM_SHOW_BINLOG_CACHE);

  Log_event::init_show_cache_field_list(&field_list);
  if (protocol->send_result_set_metadata(&field_list,
        Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
    DBUG_RETURN(TRUE);

  /*
    Wait for handlers to insert any pending information
    into the binlog.  For e.g. ndb which updates the binlog asynchronously
    this is needed so that the uses sees all its own commands in the binlog
  */
  ha_binlog_wait(thd);

  DBUG_RETURN(show_binlog_cache(thd));
}

/**
  Executes SHOW GTID_EXECUTED IN 'log_name' FROM 'log_pos' statement.
  Scans the binlog 'log_name' to build Gtid_set by adding
  previous GTIDs and all the GTIDs upto the position 'log_pos'.

  @paarm thd Pointer to the THD object for the client thread executing the
             statement.
  @retval false Success
  @retval true  Failure
*/
bool show_gtid_executed(THD *thd)
{
  DBUG_ENTER("get_gtid_executed");
  LEX *lex = thd->lex;
  // Handle empty file name
  if (!lex->mi.log_file_name)
  {
    my_error(ER_ERROR_WHEN_EXECUTING_COMMAND, MYF(0),
             "SHOW GTID_EXECUTED", "binlog file name is not specified");
    DBUG_RETURN(TRUE);
  }
  Protocol *protocol= thd->protocol;
  Sid_map sid_map(NULL);
  Gtid_set gtid_executed(&sid_map);
  char file_name[FN_REFLEN];
  mysql_bin_log.make_log_name(file_name, lex->mi.log_file_name);

  MYSQL_BIN_LOG::enum_read_gtids_from_binlog_status ret =
  mysql_bin_log.read_gtids_from_binlog(file_name, &gtid_executed,
                                       NULL, NULL, NULL, &sid_map,
                                       false, lex->mi.pos);
  if (ret == MYSQL_BIN_LOG::ERROR || ret == MYSQL_BIN_LOG::TRUNCATED)
  {
    DBUG_RETURN(TRUE);
  }
  char *gtid_executed_string = gtid_executed.to_string();
  uint gtid_executed_string_length = gtid_executed.get_string_length();

  List<Item> field_list;
  field_list.push_back(new Item_empty_string("Gtid_executed",
                                             gtid_executed_string_length));
  if (protocol->send_result_set_metadata(&field_list,
                                         Protocol::SEND_NUM_ROWS |
                                         Protocol::SEND_EOF))
    DBUG_RETURN(TRUE);
  protocol->prepare_for_resend();
  protocol->store(gtid_executed_string, &my_charset_bin);
  protocol->update_checksum();
  if (protocol->write())
    DBUG_RETURN(TRUE);

  my_free(gtid_executed_string);
  my_eof(thd);
  DBUG_RETURN(FALSE);
}
#endif /* HAVE_REPLICATION */


MYSQL_BIN_LOG::MYSQL_BIN_LOG(uint *sync_period)
  :bytes_written(0), file_id(1), open_count(1),
   sync_period_ptr(sync_period), sync_counter(0),
   m_prep_xids(0),
   binlog_end_pos(0),
   non_xid_trxs(0),
   is_relay_log(0), signal_cnt(0),
   checksum_alg_reset(BINLOG_CHECKSUM_ALG_UNDEF),
   relay_log_checksum_alg(BINLOG_CHECKSUM_ALG_UNDEF),
   engine_binlog_pos(ULONGLONG_MAX),
   previous_gtid_set(0), setup_flush_done(false)
{
  /*
    We don't want to initialize locks here as such initialization depends on
    safe_mutex (when using safe_mutex) which depends on MY_INIT(), which is
    called only in main(). Doing initialization here would make it happen
    before main().
  */
  index_file_name[0] = 0;
  engine_binlog_file[0] = 0;
  engine_binlog_max_gtid.clear();
  last_master_timestamp.store(0);
  memset(&index_file, 0, sizeof(index_file));
  memset(&purge_index_file, 0, sizeof(purge_index_file));
  memset(&crash_safe_index_file, 0, sizeof(crash_safe_index_file));
  apply_file_count.store(0);
}


/* this is called only once */

void MYSQL_BIN_LOG::cleanup()
{
  DBUG_ENTER("cleanup");
  if (inited)
  {
    inited= 0;
    close(LOG_CLOSE_INDEX|LOG_CLOSE_STOP_EVENT);
    mysql_mutex_destroy(&LOCK_log);
    mysql_mutex_destroy(&LOCK_index);
    mysql_mutex_destroy(&LOCK_commit);
    mysql_mutex_destroy(&LOCK_semisync);
    mysql_mutex_destroy(&LOCK_sync);
    mysql_mutex_destroy(&LOCK_xids);
    mysql_mutex_destroy(&LOCK_non_xid_trxs);
    mysql_mutex_destroy(&LOCK_binlog_end_pos);
    mysql_cond_destroy(&update_cond);
    my_atomic_rwlock_destroy(&m_prep_xids_lock);
    mysql_cond_destroy(&m_prep_xids_cond);
    mysql_cond_destroy(&non_xid_trxs_cond);
    stage_manager.deinit();
  }
  DBUG_VOID_RETURN;
}


void MYSQL_BIN_LOG::init_pthread_objects()
{
  MYSQL_LOG::init_pthread_objects();
  mysql_mutex_init(m_key_LOCK_index, &LOCK_index, MY_MUTEX_INIT_SLOW);
  mysql_mutex_init(m_key_LOCK_commit, &LOCK_commit, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(m_key_LOCK_semisync, &LOCK_semisync, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(m_key_LOCK_sync, &LOCK_sync, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(m_key_LOCK_xids, &LOCK_xids, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(
      m_key_LOCK_non_xid_trxs, &LOCK_non_xid_trxs, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(m_key_LOCK_binlog_end_pos, &LOCK_binlog_end_pos,
                   MY_MUTEX_INIT_FAST);
  mysql_cond_init(m_key_update_cond, &update_cond, 0);
  my_atomic_rwlock_init(&m_prep_xids_lock);
  mysql_cond_init(m_key_prep_xids_cond, &m_prep_xids_cond, NULL);
  mysql_cond_init(m_key_non_xid_trxs_cond, &non_xid_trxs_cond, NULL);
  stage_manager.init(
#ifdef HAVE_PSI_INTERFACE
                   m_key_LOCK_flush_queue,
                   m_key_LOCK_sync_queue,
                   m_key_LOCK_semisync_queue,
                   m_key_LOCK_commit_queue,
                   m_key_LOCK_done, m_key_COND_done
#endif
                   );
}

bool MYSQL_BIN_LOG::open_index_file(const char *index_file_name_arg,
                                    const char *log_name, bool need_lock_index)
{
  File index_file_nr= -1;
  DBUG_ASSERT(enable_raft_plugin || !my_b_inited(&index_file));

  if (enable_raft_plugin && my_b_inited(&index_file))
    end_io_cache(&index_file);

  /*
    First open of this class instance
    Create an index file that will hold all file names uses for logging.
    Add new entries to the end of it.
  */
  myf opt= MY_UNPACK_FILENAME;
  if (!index_file_name_arg)
  {
    index_file_name_arg= log_name;    // Use same basename for index file
    opt= MY_UNPACK_FILENAME | MY_REPLACE_EXT;
  }
  fn_format(index_file_name, index_file_name_arg, mysql_data_home,
            ".index", opt);

  if (set_crash_safe_index_file_name(index_file_name_arg))
  {
    sql_print_error("MYSQL_BIN_LOG::set_crash_safe_index_file_name failed.");
    return TRUE;
  }

  // case: crash_safe_index_file exists
  if (!my_access(crash_safe_index_file_name, F_OK))
  {
    /*
      We need move crash_safe_index_file to index_file if the index_file
      does not exist or delete it if the index_file exists when mysqld server
      restarts.
    */

    // case: index_file does not exist
    if (my_access(index_file_name, F_OK))
    {
      if (my_rename(crash_safe_index_file_name, index_file_name, MYF(MY_WME)))
      {
        sql_print_error("MYSQL_BIN_LOG::open_index_file failed to "
            "move crash_safe_index_file to index_file.");
        return TRUE;
      }

    }
    else
    {
      if (close_crash_safe_index_file() ||
          my_delete(crash_safe_index_file_name, MYF(MY_WME)))
      {
        sql_print_error("MYSQL_BIN_LOG::open_index_file failed to "
            "delete crash_safe_index_file.");
        return TRUE;
      }
    }
  }

  if ((index_file_nr= mysql_file_open(m_key_file_log_index,
                                      index_file_name,
                                      O_RDWR | O_CREAT | O_BINARY,
                                      MYF(MY_WME))) < 0 ||
       mysql_file_sync(index_file_nr, MYF(MY_WME)) ||
       init_io_cache(&index_file, index_file_nr,
                     IO_SIZE, READ_CACHE,
                     mysql_file_seek(index_file_nr, 0L, MY_SEEK_END, MYF(0)),
                                     0, MYF(MY_WME | MY_WAIT_IF_FULL)) ||
      DBUG_EVALUATE_IF("fault_injection_openning_index", 1, 0))
  {
    /*
      TODO: all operations creating/deleting the index file or a log, should
      call my_sync_dir() or my_sync_dir_by_file() to be durable.
      TODO: file creation should be done with mysql_file_create()
      not mysql_file_open().
    */
    if (index_file_nr >= 0)
      mysql_file_close(index_file_nr, MYF(0));
    return TRUE;
  }

#ifdef HAVE_REPLICATION
  /*
    Sync the index by purging any binary log file that is not registered.
    In other words, purge any binary log file that was created but not
    register in the index due to a crash.
  */
  if (set_purge_index_file_name(index_file_name_arg) ||
      open_purge_index_file(FALSE) ||
      purge_index_entry(NULL, NULL, need_lock_index) ||
      close_purge_index_file() ||
      DBUG_EVALUATE_IF("fault_injection_recovering_index", 1, 0))
  {
    sql_print_error("MYSQL_BIN_LOG::open_index_file failed to sync the index "
                    "file.");
    return TRUE;
  }
#endif

  return FALSE;
}

int MYSQL_BIN_LOG::init_index_file()
{
  char *index_file_name= NULL, *log_file_name= NULL;
  int error= 0;

  DBUG_ASSERT(enable_raft_plugin);

  myf opt= MY_UNPACK_FILENAME;
  char* apply_index_file_name_base= opt_applylog_index_name;
  if (!apply_index_file_name_base)
  {
    // Use the same base as the apply binlog file name
    apply_index_file_name_base= opt_apply_logname;
    opt= MY_UNPACK_FILENAME | MY_REPLACE_EXT;
  }

  // Create the apply index file name
  DBUG_ASSERT(apply_index_file_name_base != NULL);
  char apply_index_file[FN_REFLEN + 1];
  fn_format(
      apply_index_file,
      apply_index_file_name_base,
      mysql_data_home,
      ".index",
      opt);

  if (!my_access(apply_index_file, F_OK))
  {
    // NO_LINT_DEBUG
    sql_print_information("Binlog apply index file exists. Recovering mysqld "
                          "based on binlog apply index file: %s", opt_applylog_index_name);
    index_file_name= opt_applylog_index_name;
    log_file_name= opt_apply_logname;
    is_apply_log= true;
  }
  else
  {
    // NO_LINT_DEBUG
    sql_print_information("Binlog apply index file does not exist. Recovering "
                          "mysqld based on binlog index file: %s", opt_binlog_index_name);
    index_file_name= opt_binlog_index_name;
    log_file_name= opt_bin_logname;
  }

  if (mysql_bin_log.open_index_file(index_file_name, log_file_name, true))
  {
    // NO_LINT_DEBUG
    sql_print_error("Failed while opening index file");
    error= 1;
  }

  if (!error && is_apply_log)
  {
    uint64_t num_apply_files= 0;
    error= get_total_log_files(/*need_lock_index=*/false, &num_apply_files);
    apply_file_count.store(num_apply_files);
  }

  return error;
}

/**
  Remove logs from index that are not present on disk
  NOTE: this method will not update index with arbitrarily
  deleted logs. It will only remove entries of logs which
  are deleted from the beginning of the sequence

  @param need_lock_index        Need to lock index?
  @param need_update_threads    If we want to update the log coordinates
                                of all threads. False for relay logs,
                                true otherwise.

  @retval
   0    ok
  @retval
    LOG_INFO_IO    Got IO error while reading/writing file
    LOG_INFO_EOF	 log-index-file is empty
*/
int MYSQL_BIN_LOG::remove_deleted_logs_from_index(bool need_lock_index,
                                                  bool need_update_threads)
{
  int error;
  uint64_t no_of_log_files_purged= 0;
  uint64_t num_apply_files= 0;
  LOG_INFO log_info;

  DBUG_ENTER("remove_deleted_logs_from_index");

  if (need_lock_index)
    mysql_mutex_lock(&LOCK_index);

  if ((error=find_log_pos(&log_info, NullS, false/*need_lock_index=false*/)))
    goto err;

  while (true)
  {
    if (my_access(log_info.log_file_name, F_OK) == 0)
      break;

    int ret= find_next_log(&log_info, false/*need_lock_index=false*/);
    if (ret == LOG_INFO_EOF) {
      break;
    } else if (ret == LOG_INFO_IO) {
      sql_print_error("MYSQL_BIN_LOG::remove_deleted_logs_from_index "
                      "error while reading log index file.");
      goto err;
    }

    ++no_of_log_files_purged;
  }

  if (no_of_log_files_purged)
  {
    error= remove_logs_from_index(&log_info, need_update_threads);
    if (is_apply_log)
    {
      // Fix number of apply log file count
      if (apply_file_count >= no_of_log_files_purged)
        apply_file_count -= no_of_log_files_purged;
      else
      {
        error= get_total_log_files(need_lock_index, &num_apply_files);
        apply_file_count.store(num_apply_files);

        // NO_LINT_DEBUG
        sql_print_information("Fixed apply file count (%lu) by reading from "
            "index file.", apply_file_count.load());
      }
    }
  }

  DBUG_PRINT("info",("num binlogs deleted = %lu",no_of_log_files_purged));

err:
  if (need_lock_index)
    mysql_mutex_unlock(&LOCK_index);
  DBUG_RETURN(error);
}

/*
  Adjust the position pointer in the binary log file for all running slaves

  SYNOPSIS
    adjust_linfo_offsets()
    purge_offset	Number of bytes removed from start of log index file

  NOTES
    - This is called when doing a PURGE when we delete lines from the
      index log file

  REQUIREMENTS
    - Before calling this function, we have to ensure that no threads are
      using any binary log file before purge_offset.a

  TODO
    - Inform the slave threads that they should sync the position
      in the binary log file with flush_relay_log_info.
      Now they sync is done for next read.
*/

static void adjust_linfo_offsets(my_off_t purge_offset, bool is_relay_log)
{
  mutex_lock_all_shards(SHARDED(&LOCK_thread_count));

  Thread_iterator it= global_thread_list_begin();
  Thread_iterator end= global_thread_list_end();
  for (; it != end; ++it)
  {
    LOG_INFO* linfo = (*it)->current_linfo;
    if (linfo &&
        (!enable_raft_plugin || linfo->is_relay_log == is_relay_log))
    {
      mysql_mutex_lock(&linfo->lock);
      /*
	Index file offset can be less that purge offset only if
	we just started reading the index file. In that case
	we have nothing to adjust
      */
      if (linfo->index_file_offset < purge_offset)
	linfo->fatal = (linfo->index_file_offset != 0);
      else
	linfo->index_file_offset -= purge_offset;
      mysql_mutex_unlock(&linfo->lock);
    }
  }
  mutex_unlock_all_shards(SHARDED(&LOCK_thread_count));
}

/**
  Remove logs from index file.

  - To make crash safe, we copy the content of index file
  from index_file_start_offset recored in log_info to
  crash safe index file firstly and then move the crash
  safe index file to index file.

  @param linfo                  Store here the found log file name and
                                position to the NEXT log file name in
                                the index file.

  @param need_update_threads    If we want to update the log coordinates
                                of all threads. False for relay logs,
                                true otherwise.

  @retval
    0    ok
  @retval
    LOG_INFO_IO    Got IO error while reading/writing file
*/
int MYSQL_BIN_LOG::remove_logs_from_index(LOG_INFO* log_info,
                                          bool need_update_threads)
{
  DBUG_EXECUTE_IF("simulate_disk_full_remove_logs_from_index",
      { DBUG_SET("+d,simulate_no_free_space_error"); });
  if (open_crash_safe_index_file())
  {
    sql_print_error("MYSQL_BIN_LOG::remove_logs_from_index failed to "
                    "open the crash safe index file.");
    goto err;
  }

  if (copy_file(&index_file, &crash_safe_index_file,
                log_info->index_file_start_offset))
  {
    sql_print_error("MYSQL_BIN_LOG::remove_logs_from_index failed to "
                    "copy index file to crash safe index file.");
    goto err;
  }

  if (close_crash_safe_index_file())
  {
    sql_print_error("MYSQL_BIN_LOG::remove_logs_from_index failed to "
                    "close the crash safe index file.");
    goto err;
  }
  DBUG_EXECUTE_IF("fault_injection_copy_part_file", DBUG_SUICIDE(););

  if (move_crash_safe_index_file_to_index_file(false/*need_lock_index=false*/))
  {
    sql_print_error("MYSQL_BIN_LOG::remove_logs_from_index failed to "
                    "move crash safe index file to index file.");
    goto err;
  }

  DBUG_EXECUTE_IF("simulate_disk_full_remove_logs_from_index",
      { DBUG_SET("-d,simulate_no_free_space_error"); });

  // now update offsets in index file for running threads
  if (need_update_threads)
    adjust_linfo_offsets(log_info->index_file_start_offset, is_relay_log);
  return 0;

err:
  DBUG_EXECUTE_IF("simulate_disk_full_remove_logs_from_index", {
      DBUG_SET("-d,simulate_no_free_space_error");
      DBUG_SET("-d,simulate_file_write_error");});
  return LOG_INFO_IO;
}

/**
  Reads GTIDs from the given binlog file.

  @param filename File to read from.
  @param all_gtids If not NULL, then the GTIDs from the
  Previous_gtids_log_event and from all Gtid_log_events are stored in
  this object.
  @param prev_gtids If not NULL, then the GTIDs from the
  Previous_gtids_log_events are stored in this object.
  @param first_gtid If not NULL, then the first GTID information from the
  file will be stored in this object.
  @param last_gtid If not NULL, then the last GTID information from the
  file will be stored in this object.
  @param sid_map The sid_map object to use in the rpl_sidno generation
  of the Gtid_log_event. If lock is needed in the sid_map, the caller
  must hold it.
  @param verify_checksum Set to true to verify event checksums.
  @param max_pos Read the binlog file upto max_pos offset.
  @param max_prev_hlc The max HLC timestamp present in all previous binlog

  @retval GOT_GTIDS The file was successfully read and it contains
  both Gtid_log_events and Previous_gtids_log_events.
  @retval GOT_PREVIOUS_GTIDS The file was successfully read and it
  contains Previous_gtids_log_events but no Gtid_log_events.
  @retval NO_GTIDS The file was successfully read and it does not
  contain GTID events.
  @retval ERROR Out of memory, or the file contains GTID events
  when GTID_MODE = OFF, or the file is malformed (e.g., contains
  Gtid_log_events but no Previous_gtids_log_event).
  @retval TRUNCATED The file was truncated before the end of the
  first Previous_gtids_log_event.
*/
MYSQL_BIN_LOG::enum_read_gtids_from_binlog_status
MYSQL_BIN_LOG::read_gtids_from_binlog(const char *filename, Gtid_set *all_gtids,
                       Gtid_set *prev_gtids, Gtid *first_gtid,
                       Gtid *last_gtid,
                       Sid_map* sid_map,
                       bool verify_checksum,
                       my_off_t max_pos,
                       uint64_t *max_prev_hlc)
{
  DBUG_ENTER("read_gtids_from_binlog");
  DBUG_PRINT("info", ("Opening file %s", filename));

  /*
    Create a Format_description_log_event that is used to read the
    first event of the log.
  */
  Format_description_log_event fd_ev(BINLOG_VERSION), *fd_ev_p= &fd_ev;
  if (!fd_ev.is_valid())
    DBUG_RETURN(ERROR);

  File file;
  IO_CACHE log;
  uint64_t prev_hlc= 0;

  /*
    We assert here that both all_gtids and prev_gtids, if specified,
    uses the same sid_map as the one passed as a parameter. This is just
    to ensure that, if the sid_map needed some lock and was locked by
    the caller, the lock applies to all the GTID sets this function is
    dealing with.
  */
#ifndef DBUG_OFF
  if (all_gtids)
    DBUG_ASSERT(all_gtids->get_sid_map() == sid_map);
  if (prev_gtids)
    DBUG_ASSERT(prev_gtids->get_sid_map() == sid_map);
#endif

  const char *errmsg= NULL;
  if ((file= open_binlog_file(&log, filename, &errmsg)) < 0)
  {
    sql_print_error("%s", errmsg);
    /*
      We need to revisit the recovery procedure for relay log
      files. Currently, it is called after this routine.
      /Alfranio
    */
    DBUG_RETURN(TRUNCATED);
  }

  /*
    Seek for Previous_gtids_log_event and Gtid_log_event events to
    gather information what has been processed so far.
  */
  my_b_seek(&log, BIN_LOG_HEADER_SIZE);
  Log_event *ev= NULL;
  enum_read_gtids_from_binlog_status ret= NO_GTIDS;
  bool done= false;
  bool seen_first_gtid= false;
  while (!done &&
         (ev= Log_event::read_log_event(&log, 0, fd_ev_p,
                                        verify_checksum, NULL)) != NULL)
  {
    if (ev->log_pos > max_pos)
    {
      if (ev != fd_ev_p)
        delete ev;
      break;
    }
    DBUG_PRINT("info", ("Read event of type %s", ev->get_type_str()));
    switch (ev->get_type_code())
    {
    case FORMAT_DESCRIPTION_EVENT:
      if (fd_ev_p != &fd_ev)
        delete fd_ev_p;
      fd_ev_p= (Format_description_log_event *)ev;
      break;
    case ROTATE_EVENT:
      // do nothing; just accept this event and go to next
      break;
    case PREVIOUS_GTIDS_LOG_EVENT:
    {
      ret= GOT_PREVIOUS_GTIDS;
      // add events to sets
      Previous_gtids_log_event *prev_gtids_ev=
        (Previous_gtids_log_event *)ev;
      if (all_gtids != NULL && prev_gtids_ev->add_to_set(all_gtids) != 0)
        ret= ERROR, done= true;
      else if (prev_gtids != NULL && prev_gtids_ev->add_to_set(prev_gtids) != 0)
        ret= ERROR, done= true;
#ifndef DBUG_OFF
      char* prev_buffer= prev_gtids_ev->get_str(NULL, NULL);
      DBUG_PRINT("info", ("Got Previous_gtids from file '%s': Gtid_set='%s'.",
                          filename, prev_buffer));
      my_free(prev_buffer);
#endif
      break;
    }
    case GTID_LOG_EVENT:
    {
      DBUG_EXECUTE_IF("inject_fault_bug16502579", {
                      DBUG_PRINT("debug", ("GTID_LOG_EVENT found. Injected ret=NO_GTIDS."));
                      ret=NO_GTIDS;
                      });
      if (ret != GOT_GTIDS)
      {
        if (ret != GOT_PREVIOUS_GTIDS)
        {
          /*
            Since this routine is run on startup, there may not be a
            THD instance. Therefore, ER(X) cannot be used.
           */
          const char* msg_fmt= (current_thd != NULL) ?
                               ER(ER_BINLOG_LOGICAL_CORRUPTION) :
                               ER_DEFAULT(ER_BINLOG_LOGICAL_CORRUPTION);
          my_printf_error(ER_BINLOG_LOGICAL_CORRUPTION,
                          msg_fmt, MYF(0),
                          filename,
                          "The first global transaction identifier was read, but "
                          "no other information regarding identifiers existing "
                          "on the previous log files was found.");
          ret= ERROR, done= true;
          break;
        }
        else
          ret= GOT_GTIDS;
      }
      /*
        When all_gtids, first_gtid and last_gtid are all NULL,
        we just check if the binary log contains at least one Gtid_log_event,
        so that we can distinguish the return values GOT_GTID and
        GOT_PREVIOUS_GTIDS. We don't need to read anything else from the
        binary log.
        If all_gtids or last_gtid is requested (i.e., NOT NULL), we should
        continue to read all gtids.
        If just first_gtid was requested, we will be done after storing this
        Gtid_log_event info on it.
      */
      if (all_gtids == NULL && first_gtid == NULL && last_gtid == NULL)
      {
        ret= GOT_GTIDS, done= true;
      }
      else
      {
        Gtid_log_event *gtid_ev= (Gtid_log_event *)ev;
        rpl_sidno sidno= gtid_ev->get_sidno(sid_map);
        if (sidno < 0)
          ret= ERROR, done= true;
        else
        {
          if (all_gtids)
          {
            if (all_gtids->ensure_sidno(sidno) != RETURN_STATUS_OK)
              ret= ERROR, done= true;
            else if (all_gtids->_add_gtid(sidno, gtid_ev->get_gno()) !=
                     RETURN_STATUS_OK)
              ret= ERROR, done= true;
            DBUG_PRINT("info", ("Got Gtid from file '%s': Gtid(%d, %lld).",
                                filename, sidno, gtid_ev->get_gno()));
          }

          /* If the first GTID was requested, stores it */
          if (first_gtid && !seen_first_gtid)
          {
            first_gtid->set(sidno, gtid_ev->get_gno());
            seen_first_gtid= true;
            /* If the first_gtid was the only thing requested, we are done */
            if (all_gtids == NULL && last_gtid == NULL)
              ret= GOT_GTIDS, done= true;
          }

          if (last_gtid)
            last_gtid->set(sidno, gtid_ev->get_gno());
        }
      }
      break;
    }
    case METADATA_EVENT:
      if (unlikely(max_prev_hlc))
      {
        prev_hlc= std::max(
            prev_hlc, extract_hlc(static_cast<Metadata_log_event *>(ev)));
      }
      break;
    case ANONYMOUS_GTID_LOG_EVENT:
    default:
      // if we found any other event type without finding a
      // previous_gtids_log_event, then the rest of this binlog
      // cannot contain gtids
      if (ret != GOT_GTIDS && ret != GOT_PREVIOUS_GTIDS)
        done= true;
      break;
    }
    if (ev != fd_ev_p)
      delete ev;
    DBUG_PRINT("info", ("done=%d", done));
  }

  if (log.error < 0)
  {
    // This is not a fatal error; the log may just be truncated.

    // @todo but what other errors could happen? IO error?
    sql_print_warning("Error reading GTIDs from binary log: %d", log.error);
  }

  if (fd_ev_p != &fd_ev)
  {
    delete fd_ev_p;
    fd_ev_p= &fd_ev;
  }

  mysql_file_close(file, MYF(MY_WME));
  end_io_cache(&log);

  // Set max HLC timestamp in all previous binlogs
  if (max_prev_hlc)
    *max_prev_hlc= prev_hlc;

  DBUG_PRINT("info", ("returning %d", ret));
  DBUG_RETURN(ret);
}

uint64_t MYSQL_BIN_LOG::extract_hlc(Metadata_log_event *metadata_ev)
{
  return std::max(
      metadata_ev->get_hlc_time(), metadata_ev->get_prev_hlc_time());
}

bool MYSQL_BIN_LOG::find_first_log_not_in_gtid_set(char *binlog_file_name,
                                                   const Gtid_set *gtid_set,
                                                   Gtid *first_gtid,
                                                   const char **errmsg)
{
  DBUG_ENTER("MYSQL_BIN_LOG::find_first_log_not_in_gtid_set");

  Gtid_set previous_gtid_set(gtid_set->get_sid_map());

  mysql_mutex_lock(&LOCK_index);
  for (auto rit = previous_gtid_set_map.rbegin();
       rit != previous_gtid_set_map.rend(); ++rit)
  {

    previous_gtid_set.add_gtid_encoding((const uchar*)rit->second.c_str(),
                                        rit->second.length());

    if (previous_gtid_set.is_subset(gtid_set))
    {
      strcpy(binlog_file_name, rit->first.c_str());
      enum_read_gtids_from_binlog_status ret =
        read_gtids_from_binlog(binlog_file_name, NULL, NULL, first_gtid,
                               NULL, gtid_set->get_sid_map(),
                               opt_master_verify_checksum);
      // some rpl tests injects the error skip_writing_previous_gtids_log_event
      // intentionally. Don't pass this error to dump thread which causes
      // slave io_thread error failing the tests.
      if (ret != GOT_GTIDS && ret != GOT_PREVIOUS_GTIDS &&
          DBUG_EVALUATE_IF("skip_writing_previous_gtids_log_event", 0, 1))
      {
        *errmsg = "Error finding first GTID in the log file";
        mysql_mutex_unlock(&LOCK_index);
        DBUG_RETURN(true);
      }
      mysql_mutex_unlock(&LOCK_index);
      DBUG_RETURN(false);
    }
    previous_gtid_set.clear();
  }

  *errmsg= ER(ER_MASTER_HAS_PURGED_REQUIRED_GTIDS);
  DBUG_PRINT("error", ("'%s'", *errmsg));
  mysql_mutex_unlock(&LOCK_index);
  DBUG_RETURN(true);
}

/*
 * This is a limited version of init_gtid_sets which is only called from
 * binlog_change_to_apply. Needs to be called under LOCK_index held. LOCK_log is
 * not required because we're reading only prev gtids. The previous_gtid_set_map
 * is cleared and reinitialized from the index file contents.
 */
bool MYSQL_BIN_LOG::init_prev_gtid_sets_map()
{
  char file_name_and_gtid_set_length[FILE_AND_GTID_SET_LENGTH];
  uchar *previous_gtid_set_in_file = NULL;
  int error = 0, length;
  std::pair<Gtid_set_map::iterator, bool> iterator;
  DBUG_ENTER("MYSQL_BIN_LOG::init_prev_gtid_sets_map");

  mysql_mutex_assert_owner(&LOCK_index);

  // clear the map as it is being reset
  previous_gtid_set_map.clear();

  my_b_seek(&index_file, 0);
  while ((length=my_b_gets(&index_file, file_name_and_gtid_set_length,
                           sizeof(file_name_and_gtid_set_length))) >= 1)
  {
    file_name_and_gtid_set_length[length - 1] = 0;
    uint gtid_string_length =
      split_file_name_and_gtid_set_length(file_name_and_gtid_set_length);
    if (gtid_string_length > 0)
    {
      // Allocate gtid_string_length + 1 to include the '\n' also.
      previous_gtid_set_in_file =
        (uchar *) my_malloc(gtid_string_length + 1, MYF(MY_WME));
      if (previous_gtid_set_in_file == NULL)
      {
        // NO_LINT_DEBUG
        sql_print_error("MYSQL_BIN_LOG::init_prev_gtid_sets_map failed allocating "
                        "%u bytes", gtid_string_length + 1);
        error = 2;
        goto end;
      }
      if (my_b_read(&index_file, previous_gtid_set_in_file,
                    gtid_string_length + 1))
      {
        // NO_LINT_DEBUG
        sql_print_error("MYSQL_BINLOG::init_prev_gtid_sets_map failed because "
                        "previous gtid set of binlog %s is corrupted in "
                        "the index file", file_name_and_gtid_set_length);
        error= !index_file.error ? LOG_INFO_EOF : LOG_INFO_IO;
        my_free(previous_gtid_set_in_file);
        goto end;
      }
    }

    iterator = previous_gtid_set_map.insert(
      std::pair<string, string>(string(file_name_and_gtid_set_length),
                                string((char*)previous_gtid_set_in_file,
                                       gtid_string_length)));

    my_free(previous_gtid_set_in_file);
    previous_gtid_set_in_file = NULL;
  }

end:
  DBUG_PRINT("info", ("returning %d", error));
  DBUG_RETURN(error != 0 ? true : false);
}

bool MYSQL_BIN_LOG::init_gtid_sets(Gtid_set *all_gtids, Gtid_set *lost_gtids,
                                   Gtid *last_gtid, bool verify_checksum,
                                   bool need_lock, uint64_t *max_prev_hlc,
                                   bool startup)
{
  char file_name_and_gtid_set_length[FILE_AND_GTID_SET_LENGTH];
  uchar *previous_gtid_set_in_file = NULL;
  bool found_lost_gtids = false;
  uint last_previous_gtid_encoded_length = 0;
  int error = 0, length;
  std::string log_file_to_read;

  std::pair<Gtid_set_map::iterator, bool> iterator, save_iterator;
  previous_gtid_set_map.clear();
  log_file_to_read.clear();

  /* Initialize the sid_map to be used in read_gtids_from_binlog */
  Sid_map *sid_map= NULL;
  if (all_gtids)
    sid_map= all_gtids->get_sid_map();
  else if(lost_gtids)
    sid_map= lost_gtids->get_sid_map();

  DBUG_ENTER("MYSQL_BIN_LOG::init_gtid_sets");
  DBUG_PRINT("info", ("lost_gtids=%p; so we are recovering a %s log",
                      lost_gtids, lost_gtids == NULL ? "relay" : "binary"));

  /*
    Acquires the necessary locks to ensure that logs are not either
    removed or updated when we are reading from it.
  */
  if (need_lock)
  {
    // We don't need LOCK_log if we are only going to read the initial
    // Prevoius_gtids_log_event and ignore the Gtid_log_events.
    if (all_gtids != NULL)
      mysql_mutex_lock(&LOCK_log);
    mysql_mutex_lock(&LOCK_index);
    global_sid_lock->wrlock();
  }
  else
  {
    if (all_gtids != NULL)
      mysql_mutex_assert_owner(&LOCK_log);
    mysql_mutex_assert_owner(&LOCK_index);
    global_sid_lock->assert_some_wrlock();
  }

  my_b_seek(&index_file, 0);
  while ((length=my_b_gets(&index_file, file_name_and_gtid_set_length,
                           sizeof(file_name_and_gtid_set_length))) >= 1)
  {
    file_name_and_gtid_set_length[length - 1] = 0;
    uint gtid_string_length =
      split_file_name_and_gtid_set_length(file_name_and_gtid_set_length);
    if (gtid_string_length > 0)
    {
      // Allocate gtid_string_length + 1 to include the '\n' also.
      previous_gtid_set_in_file =
        (uchar *) my_malloc(gtid_string_length + 1, MYF(MY_WME));
      if (previous_gtid_set_in_file == NULL)
      {
        sql_print_error("MYSQL_BIN_LOG::init_gtid_sets failed allocating "
                        "%u bytes", gtid_string_length + 1);
        error = 2;
        goto end;
      }
      if (my_b_read(&index_file, previous_gtid_set_in_file,
                    gtid_string_length + 1))
      {
        sql_print_error("MYSQL_BINLOG::init_gtid_sets failed because "
                        "previous gtid set of binlog %s is corrupted in "
                        "the index file", file_name_and_gtid_set_length);
        error= !index_file.error ? LOG_INFO_EOF : LOG_INFO_IO;
        my_free(previous_gtid_set_in_file);
        goto end;
      }

      if (lost_gtids != NULL && !found_lost_gtids)
      {
        DBUG_PRINT("info", ("first binlog with gtids %s\n",
                            file_name_and_gtid_set_length));
        lost_gtids->add_gtid_encoding(previous_gtid_set_in_file,
                                      gtid_string_length);
        found_lost_gtids = true;
      }
    }

    iterator = previous_gtid_set_map.insert(
      std::pair<string, string>(string(file_name_and_gtid_set_length),
                                string((char*)previous_gtid_set_in_file,
                                       gtid_string_length)));

    if (enable_raft_plugin)
    {
      if (log_file_to_read.empty())
      {
        const std::string cur_log_file=
          file_name_and_gtid_set_length +
          dirname_length(file_name_and_gtid_set_length);

        if (strcmp(cur_log_file.c_str(), this->engine_binlog_file) == 0)
          log_file_to_read.assign(file_name_and_gtid_set_length);
      }
      else if (startup)
      {
        // Print a warning line in case of startup
        // NO_LINT_DEBUG
        sql_print_warning("Engine has seen trxs till file %s, but found "
                          "additional file %s in the index file",
                          log_file_to_read.c_str(),
                          file_name_and_gtid_set_length);
      }
    }

    if (all_gtids != NULL && gtid_string_length > 0)
    {
      last_previous_gtid_encoded_length = gtid_string_length;
      save_iterator = iterator;
    }
    my_free(previous_gtid_set_in_file);
    previous_gtid_set_in_file = NULL;
  }

  if (all_gtids != NULL && last_previous_gtid_encoded_length)
  {
    const char *last_binlog_file_with_gtids =
      save_iterator.first->first.c_str();
    DBUG_PRINT("info", ("last binlog with gtids %s\n",
                        last_binlog_file_with_gtids));

    my_off_t max_pos= ULONGLONG_MAX;

    // In raft mode, when the server is starting up, we update gtid_executed
    // only till the file position that is stored in the engine. This is because
    // anything that is not committed in the engine could get trimmed when this
    // instance rejoins the raft ring
    if (enable_raft_plugin && startup)
    {
      if (log_file_to_read.empty())
      {
        // NO_LINT_DEBUG
        sql_print_information(
            "Could not get the transaction log file name from the engine. "
            "Using the latest for initializing mysqld state");

        log_file_to_read.assign(last_binlog_file_with_gtids);

        // In innodb, engine_binlog_file is populated _only_ when there are
        // trxs to recover (i.e trxs are in prepared state) during startup
        // and engine recovery. Hence, engine_binlog_file being empty indicates
        // that engine's state is up-to-date with the latest binlog file and we
        // can include all gtids in the latest binlog file for calculating
        // executed-gtid-set
        if (strlen(engine_binlog_file) == 0)
          max_pos= ULONGLONG_MAX;
        else
          max_pos= this->first_gtid_start_pos;
      }
      else
      {
        // Initializing gtid_sets based on engine binlog position is fine since
        // idempotent recovery will fill in the holes
        max_pos= this->engine_binlog_pos;
      }

      // NO_LINT_DEBUG
      sql_print_information("Reading all gtids till file position %llu "
          "in file %s", max_pos, log_file_to_read.c_str());
    }
    else
    {
      log_file_to_read.assign(last_binlog_file_with_gtids);
      max_pos= ULONGLONG_MAX;
    }

    switch (read_gtids_from_binlog(log_file_to_read.c_str(), all_gtids, NULL,
                                   NULL, last_gtid, sid_map, verify_checksum,
                                   max_pos, max_prev_hlc))
    {
      case ERROR:
        error= 1;
        goto end;
      default:
        break;
    }
    /*
      Even though the previous gtid encoding is not null in index file, it may
      happen that the binlog is corrupted and doesn't contain previous gtid log
      event. In these cases, the encoding in the index file is considered as
      true and used to initialize the all_gtids(GLOBAL.GTID_EXECUTED).
    */
    if (all_gtids->is_empty())
    {
      all_gtids->add_gtid_encoding(
        (const uchar*)save_iterator.first->second.c_str(),
        last_previous_gtid_encoded_length);
    }
  }

end:
  if (all_gtids)
    all_gtids->dbug_print("all_gtids");
  if (lost_gtids)
    lost_gtids->dbug_print("lost_gtids");
  if (need_lock)
  {
    global_sid_lock->unlock();
    mysql_mutex_unlock(&LOCK_index);
    if (all_gtids != NULL)
      mysql_mutex_unlock(&LOCK_log);
  }
  DBUG_PRINT("info", ("returning %d", error));
  DBUG_RETURN(error != 0 ? true : false);
}

/**
 * Which rotates are initiated by the plugin and happen
 * in the raft listener queue thread?
 *
 * 1. Any rotate which is a post_append (i.e. normal rotate)
 * 2. Any rotate which the plugin asks the mysql to initiate
 * via injecting an operation in the listener queue.
 * i.e. 2.1 - No-Op messages
 *      2.2 - Config Change messages.
 */
static bool
is_rotate_in_listener_context(RaftRotateInfo *raft_rotate_info)
{
  if (!raft_rotate_info)
    return false;

  return raft_rotate_info->post_append ||
         raft_rotate_info->noop ||
         raft_rotate_info->config_change_rotate;
}

/**
  Open a (new) binlog file.

  - Open the log file and the index file. Register the new
  file name in it
  - When calling this when the file is in use, you must have a locks
  on LOCK_log and LOCK_index.

  @retval
    0	ok
  @retval
    1	error
*/

bool MYSQL_BIN_LOG::open_binlog(const char *log_name,
                                const char *new_name,
                                enum cache_type io_cache_type_arg,
                                ulong max_size_arg,
                                bool null_created_arg,
                                bool need_lock_index,
                                bool need_sid_lock,
                                Format_description_log_event *extra_description_event,
                                RaftRotateInfo *raft_rotate_info,
                                bool need_end_log_pos_lock)
{
  File file= -1;

  // lock_index must be acquired *before* sid_lock.
  DBUG_ASSERT(need_sid_lock || !need_lock_index);
  DBUG_ENTER("MYSQL_BIN_LOG::open_binlog(const char *, ...)");
  DBUG_PRINT("enter",("name: %s", log_name));

  if (init_and_set_log_file_name(log_name, new_name, LOG_BIN,
                                 io_cache_type_arg))
  {
    sql_print_error("MYSQL_BIN_LOG::open failed to generate new file name.");
    DBUG_RETURN(1);
  }

#ifdef HAVE_REPLICATION
  if (open_purge_index_file(TRUE) ||
      register_create_index_entry(log_file_name) ||
      sync_purge_index_file() ||
      DBUG_EVALUATE_IF("fault_injection_registering_index", 1, 0))
  {
    /**
      @todo: although this was introduced to appease valgrind
      when injecting emulated faults using fault_injection_registering_index
      it may be good to consider what actually happens when
      open_purge_index_file succeeds but register or sync fails.

      Perhaps we might need the code below in MYSQL_LOG_BIN::cleanup
      for "real life" purposes as well?
    */
    DBUG_EXECUTE_IF("fault_injection_registering_index", {
      if (my_b_inited(&purge_index_file))
      {
        end_io_cache(&purge_index_file);
        my_close(purge_index_file.file, MYF(0));
      }
    });

    sql_print_error("MYSQL_BIN_LOG::open failed to sync the index file.");
    DBUG_RETURN(1);
  }
  DBUG_EXECUTE_IF("crash_create_non_critical_before_update_index", DBUG_SUICIDE(););
#endif

  write_error= 0;

  /* open the main log file */
  if (MYSQL_LOG::open(
#ifdef HAVE_PSI_INTERFACE
                      m_key_file_log,
#endif
                      log_name, LOG_BIN, new_name, io_cache_type_arg))
  {
#ifdef HAVE_REPLICATION
    close_purge_index_file();
#endif
    DBUG_RETURN(1);                            /* all warnings issued */
  }

  max_size= max_size_arg;

  open_count++;

  bool write_file_name_to_index_file=0;
  bool raft_noop= raft_rotate_info && raft_rotate_info->noop;
  bool in_listener_thread= raft_rotate_info &&
                           is_rotate_in_listener_context(raft_rotate_info);

  /* This must be before goto err. */
  Format_description_log_event s(BINLOG_VERSION);

  if (!my_b_filelength(&log_file))
  {
    /*
      The binary log file was empty (probably newly created)
      This is the normal case and happens when the user doesn't specify
      an extension for the binary log files.
      In this case we write a standard header to it.
    */
    if (my_b_safe_write(&log_file, (uchar*) BINLOG_MAGIC,
                        BIN_LOG_HEADER_SIZE))
      goto err;
    bytes_written+= BIN_LOG_HEADER_SIZE;
    write_file_name_to_index_file= 1;
  }

  /*
    don't set LOG_EVENT_BINLOG_IN_USE_F for SEQ_READ_APPEND io_cache
    as we won't be able to reset it later
  */
  if (io_cache_type == WRITE_CACHE || raft_noop)
    s.flags |= LOG_EVENT_BINLOG_IN_USE_F;
  s.checksum_alg= is_relay_log ?
    /* relay-log */
    /* inherit master's A descriptor if one has been received */
    (relay_log_checksum_alg=
     (relay_log_checksum_alg != BINLOG_CHECKSUM_ALG_UNDEF) ?
     relay_log_checksum_alg :
     /* otherwise use slave's local preference of RL events verification */
     (opt_slave_sql_verify_checksum == 0) ?
     (uint8) BINLOG_CHECKSUM_ALG_OFF : binlog_checksum_options):
    /* binlog */
    binlog_checksum_options;
  DBUG_ASSERT(s.checksum_alg != BINLOG_CHECKSUM_ALG_UNDEF);
  if (!s.is_valid())
    goto err;
  s.dont_set_created= null_created_arg;
  // Since start time of listener thread is not correct,
  // we need to explicitly set event timestamp otherwise,
  // the mysqlbinlog output will show UTC-0 for FD/PGTID/MD event
  if (in_listener_thread)
    s.when.tv_sec= my_time(0);
  /* Set LOG_EVENT_RELAY_LOG_F flag for relay log's FD */
  if (!raft_noop && is_relay_log)
    s.set_relay_log_event();
  if (raft_rotate_info)
  {
    DBUG_ASSERT(raft_rotate_info->rotate_via_raft ||
                raft_rotate_info->post_append);
    s.set_raft_log_event();
  }
  if (s.write(&log_file))
    goto err;
  bytes_written+= s.data_written;
  /*
    We need to revisit this code and improve it.
    See further comments in the mysqld.
    /Alfranio
  */
  if (need_sid_lock)
    global_sid_lock->wrlock();
  else
    global_sid_lock->assert_some_wrlock();

  if (gtid_mode > 0 || !previous_gtid_set->is_empty())
  {
    Previous_gtids_log_event prev_gtids_ev(previous_gtid_set);
    if (!raft_noop && is_relay_log)
      prev_gtids_ev.set_relay_log_event();
    prev_gtids_ev.checksum_alg= s.checksum_alg;
    if (in_listener_thread)
      prev_gtids_ev.when.tv_sec= my_time(0);
    if (prev_gtids_ev.write(&log_file))
    {
      if (need_sid_lock)
        global_sid_lock->unlock();
      goto err;
    }
    bytes_written+= prev_gtids_ev.data_written;
  }

  /* If HLC is enabled, then write the current HLC value into binlog. This is
   * used during server restart to intialize the HLC clock of the instance.
   * This is a guarantee that all trx's in all of the previous binlog have a HLC
   * timestamp lower than or equal to the value seen here. Note that this
   * function (open_binlog()) should be called during server restart only after
   * initializing the local instance's HLC clock (by reading the previous binlog
   * file) */
  /*
   * In Raft mode, to keep consistent sizes of raft log files, we write a dummy HLC
   * to relay logs.
   */
  if (enable_binlog_hlc && (!is_relay_log || raft_rotate_info))
  {
    uint64_t current_hlc= 0;
    if (!is_relay_log)
    {
      current_hlc= mysql_bin_log.get_current_hlc();
    }
    Metadata_log_event metadata_ev(current_hlc);
    if (raft_rotate_info)
    {
      metadata_ev.set_raft_prev_opid(
          raft_rotate_info->rotate_opid.first,
          raft_rotate_info->rotate_opid.second);
    }

    if (in_listener_thread)
      metadata_ev.when.tv_sec= my_time(0);
    if (metadata_ev.write(&log_file))
      goto err;
    bytes_written+= metadata_ev.data_written;
  }

  if (need_sid_lock)
    global_sid_lock->unlock();

  if (extra_description_event &&
      extra_description_event->binlog_version>=4)
  {
    /*
      This is a relay log written to by the I/O slave thread.
      Write the event so that others can later know the format of this relay
      log.
      Note that this event is very close to the original event from the
      master (it has binlog version of the master, event types of the
      master), so this is suitable to parse the next relay log's event. It
      has been produced by
      Format_description_log_event::Format_description_log_event(char* buf,).
      Why don't we want to write the mi_description_event if this
      event is for format<4 (3.23 or 4.x): this is because in that case, the
      mi_description_event describes the data received from the
      master, but not the data written to the relay log (*conversion*),
      which is in format 4 (slave's).
    */
    if (extra_description_event->write(&log_file))
      goto err;
    bytes_written+= extra_description_event->data_written;
  }

  DBUG_EXECUTE_IF("delay_open_binlog", sleep(5););

  if (flush_io_cache(&log_file) ||
      mysql_file_sync(log_file.file, MYF(MY_WME)))
    goto err;

  update_binlog_end_pos(need_end_log_pos_lock);

  if (write_file_name_to_index_file)
  {
#ifdef HAVE_REPLICATION
    DBUG_EXECUTE_IF("crash_create_critical_before_update_index", DBUG_SUICIDE(););
#endif

    DBUG_ASSERT(my_b_inited(&index_file) != 0);

    /*
      The new log file name is appended into crash safe index file after
      all the content of index file is copyed into the crash safe index
      file. Then move the crash safe index file to index file.
    */
    DBUG_EXECUTE_IF("simulate_disk_full_on_open_binlog",
                    {DBUG_SET("+d,simulate_no_free_space_error");});
    if (DBUG_EVALUATE_IF("fault_injection_updating_index", 1, 0) ||
        add_log_to_index((uchar*) log_file_name, strlen(log_file_name),
                         need_lock_index, need_sid_lock))
    {
      DBUG_EXECUTE_IF("simulate_disk_full_on_open_binlog",
                      {
                        DBUG_SET("-d,simulate_file_write_error");
                        DBUG_SET("-d,simulate_no_free_space_error");
                        DBUG_SET("-d,simulate_disk_full_on_open_binlog");
                      });
      goto err;
    }

#ifdef HAVE_REPLICATION
    DBUG_EXECUTE_IF("crash_create_after_update_index", DBUG_SUICIDE(););
#endif
  }

  log_state= LOG_OPENED;

#ifdef HAVE_REPLICATION
  close_purge_index_file();
#endif

  DBUG_RETURN(0);

err:
#ifdef HAVE_REPLICATION
  if (is_inited_purge_index_file())
    purge_index_entry(NULL, NULL, need_lock_index);
  close_purge_index_file();
#endif
  if (file >= 0)
    mysql_file_close(file, MYF(0));
  end_io_cache(&log_file);
  end_io_cache(&index_file);
  my_free(name);
  name= NULL;
  log_state= LOG_CLOSED;
  if (binlog_error_action == ABORT_SERVER ||
      binlog_error_action == ROLLBACK_TRX)
  {
    exec_binlog_error_action_abort("Either disk is full or file system is read "
                                   "only while opening the binlog. Aborting the"
                                   " server.");
  }
  else
    sql_print_error("Could not use %s for logging (error %d). "
                    "Turning logging off for the whole duration of the MySQL "
                    "server process. To turn it on again: fix the cause, "
                    "shutdown the MySQL server and restart it.", name, errno);
  DBUG_RETURN(1);
}

/**
  Open an (existing) binlog file.

  - Open the log file and the index file. Register the new
  file name in it
  - When calling this when the file is in use, you must have a locks
  on LOCK_log and LOCK_index.

  @retval
    0	ok
  @retval
    1	error
*/
bool MYSQL_BIN_LOG::open_existing_binlog(
    const char *log_name,
    enum cache_type io_cache_type_arg,
    ulong max_size_arg,
    bool need_end_log_pos_lock)
{
  DBUG_ENTER("MYSQL_BIN_LOG::open_existing_binlog(const char *, ...)");
  DBUG_PRINT("enter",("name: %s", log_name));

  // This sets the cur_log_ext telling the plugin that
  // RLI initialization has happened.
  // i.e. cur_log_ext != (ulong)-1
  char existing_file[FN_REFLEN];
  if (find_existing_last_file(existing_file, log_name))
  {
    sql_print_error("MYSQL_BIN_LOG::open_existing_binlog failed to locate last file");
    DBUG_RETURN(1);
  }

  // At the end of this "log_file_name" is set to existing_file
  if (init_and_set_log_file_name(log_name, existing_file, LOG_BIN,
                                 io_cache_type_arg))
  {
    sql_print_error("MYSQL_BIN_LOG::open_existing_binlog failed to generate new file name.");
    DBUG_RETURN(1);
  }

  if (!(name= my_strdup(log_name, MYF(MY_WME))))
  {
    sql_print_error("Could not allocate name %s (error %d)", log_name, errno);
    DBUG_RETURN(1);
  }

  /* open the main log file */
  if (MYSQL_LOG::open_existing(
#ifdef HAVE_PSI_INTERFACE
                      m_key_file_log
#endif
                      ))
  {
    // NO_LINT_DEBUG
    sql_print_error("Could not open the log file %s", log_name);
    DBUG_RETURN(1);
  }

  max_size= max_size_arg;
  open_count++;

  update_binlog_end_pos(need_end_log_pos_lock);

  log_state= LOG_OPENED;
  DBUG_RETURN(0);
}

/**
  Move crash safe index file to index file.

  @param need_lock_index If true, LOCK_index will be acquired;
  otherwise it should already be held.

  @retval 0 ok
  @retval -1 error
*/
int MYSQL_BIN_LOG::move_crash_safe_index_file_to_index_file(bool need_lock_index)
{
  int error= 0;
  File fd= -1;
  DBUG_ENTER("MYSQL_BIN_LOG::move_crash_safe_index_file_to_index_file");
  int failure_trials= MYSQL_BIN_LOG::MAX_RETRIES_FOR_DELETE_RENAME_FAILURE;
  bool file_rename_status= false, file_delete_status= false;
  THD *thd= current_thd;

  if (need_lock_index)
    mysql_mutex_lock(&LOCK_index);
  else
    mysql_mutex_assert_owner(&LOCK_index);

  if (my_b_inited(&index_file))
  {
    end_io_cache(&index_file);
    if (mysql_file_close(index_file.file, MYF(0)) < 0)
    {
      error= -1;
      sql_print_error("While rebuilding index file %s: "
                      "Failed to close the index file.", index_file_name);
      /*
        Delete Crash safe index file here and recover the binlog.index
        state(index_file io_cache) from old binlog.index content.
       */
      mysql_file_delete(key_file_binlog_index, crash_safe_index_file_name,
                        MYF(0));

      goto recoverable_err;
    }

    /*
      Sometimes an outsider can lock index files for temporary viewing
      purpose. For eg: MEB locks binlog.index/relaylog.index to view
      the content of the file. During that small period of time, deletion
      of the file is not possible on some platforms(Eg: Windows)
      Server should retry the delete operation for few times instead of panicking
      immediately.
    */
    while ((file_delete_status == false) && (failure_trials > 0))
    {
      if (DBUG_EVALUATE_IF("force_index_file_delete_failure", 1, 0)) break;

      DBUG_EXECUTE_IF("simulate_index_file_delete_failure",
                  {
                    /* This simulation causes the delete to fail */
                    static char first_char= index_file_name[0];
                    index_file_name[0]= 0;
                    sql_print_information("Retrying delete");
                    if (failure_trials == 1)
                      index_file_name[0]= first_char;
                  };);
      file_delete_status = !(mysql_file_delete(key_file_binlog_index,
                                               index_file_name, MYF(MY_WME)));
      --failure_trials;
      if (!file_delete_status)
      {
        my_sleep(1000);
        /* Clear the error before retrying. */
        if (failure_trials > 0)
          thd->clear_error();
      }
    }

    if (!file_delete_status)
    {
      error= -1;
      sql_print_error("While rebuilding index file %s: "
                      "Failed to delete the existing index file. It could be "
                      "that file is being used by some other process.",
                      index_file_name);
      /*
        Delete Crash safe file index file here and recover the binlog.index
        state(index_file io_cache) from old binlog.index content.
       */
      mysql_file_delete(key_file_binlog_index, crash_safe_index_file_name,
                        MYF(0));

      goto recoverable_err;
    }
  }

  DBUG_EXECUTE_IF("crash_create_before_rename_index_file", DBUG_SUICIDE(););
  /*
    Sometimes an outsider can lock index files for temporary viewing
    purpose. For eg: MEB locks binlog.index/relaylog.index to view
    the content of the file. During that small period of time, rename
    of the file is not possible on some platforms(Eg: Windows)
    Server should retry the rename operation for few times instead of panicking
    immediately.
  */
  failure_trials = MYSQL_BIN_LOG::MAX_RETRIES_FOR_DELETE_RENAME_FAILURE;
  while ((file_rename_status == false) && (failure_trials > 0))
  {
    DBUG_EXECUTE_IF("simulate_crash_safe_index_file_rename_failure",
                {
                  /* This simulation causes the rename to fail */
                  static char first_char= index_file_name[0];
                  index_file_name[0]= 0;
                  sql_print_information("Retrying rename");
                  if (failure_trials == 1)
                    index_file_name[0]= first_char;
                };);
    file_rename_status =
        !(my_rename(crash_safe_index_file_name, index_file_name, MYF(MY_WME)));
    --failure_trials;
    if (!file_rename_status)
    {
      my_sleep(1000);
      /* Clear the error before retrying. */
      if (failure_trials > 0)
        thd->clear_error();
    }
  }
  if (!file_rename_status)
  {
    error= -1;
    sql_print_error("While rebuilding index file %s: "
                    "Failed to rename the new index file to the existing "
                    "index file.", index_file_name);
    goto fatal_err;
  }
  DBUG_EXECUTE_IF("crash_create_after_rename_index_file", DBUG_SUICIDE(););

recoverable_err:
  if ((fd= mysql_file_open(key_file_binlog_index,
                           index_file_name,
                           O_RDWR | O_CREAT | O_BINARY,
                           MYF(MY_WME))) < 0 ||
           mysql_file_sync(fd, MYF(MY_WME)) ||
           init_io_cache(&index_file, fd, IO_SIZE, READ_CACHE,
                         mysql_file_seek(fd, 0L, MY_SEEK_END, MYF(0)),
                                         0, MYF(MY_WME | MY_WAIT_IF_FULL)))
  {
    sql_print_error("After rebuilding the index file %s: "
                    "Failed to open the index file.", index_file_name);
    goto fatal_err;
  }

  if (need_lock_index)
    mysql_mutex_unlock(&LOCK_index);
  DBUG_RETURN(error);

fatal_err:
  /*
    This situation is very very rare to happen (unless there is some serious
    memory related issues like OOM) and should be treated as fatal error.
    Hence it is better to bring down the server without respecting
    'binlog_error_action' value here.
  */
  exec_binlog_error_action_abort("MySQL server failed to update the "
                                 "binlog.index file's content properly. "
                                 "It might not be in sync with available "
                                 "binlogs and the binlog.index file state is in "
                                 "unrecoverable state. Aborting the server.");
  /*
    Server is aborted in the above function.
    This is dead code to make compiler happy.
   */
  DBUG_RETURN(error);
}


/**
  Append log file name to index file.

  - To make crash safe, we copy all the content of index file
  to crash safe index file firstly and then append the log
  file name to the crash safe index file. Finally move the
  crash safe index file to index file.

  @retval
    0   ok
  @retval
    -1   error
*/
int MYSQL_BIN_LOG::add_log_to_index(uchar* log_name,
                                    int log_name_len, bool need_lock_index,
                                    bool need_sid_lock)
{
  char gtid_set_length_buffer[11];
  uchar *previous_gtid_set_buffer = NULL;
  uint gtid_set_length = 0;
  DBUG_ENTER("MYSQL_BIN_LOG::add_log_to_index");

  DBUG_EXECUTE_IF("simulate_disk_full_add_log_to_index",
      { DBUG_SET("+d,simulate_no_free_space_error"); });

  if (open_crash_safe_index_file())
  {
    sql_print_error("MYSQL_BIN_LOG::add_log_to_index failed to "
                    "open the crash safe index file.");
    goto err;
  }

  if (copy_file(&index_file, &crash_safe_index_file, 0))
  {
    sql_print_error("MYSQL_BIN_LOG::add_log_to_index failed to "
                    "copy index file to crash safe index file.");
    goto err;
  }

  if (need_sid_lock)
    global_sid_lock->wrlock();

  global_sid_lock->assert_some_wrlock();

  if (gtid_mode > 0 || !previous_gtid_set->is_empty())
  {
    previous_gtid_set_buffer = previous_gtid_set->encode(&gtid_set_length);
    int10_to_str(gtid_set_length, gtid_set_length_buffer, 10);
  }
  if (need_sid_lock)
     global_sid_lock->unlock();

  DBUG_PRINT("info", ("file_name and gtid_set %s\n %s\n", log_name,
                      previous_gtid_set_buffer));
  if (my_b_write(&crash_safe_index_file, log_name, log_name_len))
  {
    sql_print_error("MYSQL_BIN_LOG::add_log_to_index failed to "
                    "append log file name: %s, to crash "
                    "safe index file.", log_name);
    goto err;
  }

  if (gtid_set_length > 0)
  {
    if (my_b_write(&crash_safe_index_file, (uchar*) " ", 1) ||
        my_b_write(&crash_safe_index_file, (uchar*)gtid_set_length_buffer,
                   strlen(gtid_set_length_buffer)) ||
        my_b_write(&crash_safe_index_file, (uchar*) "\n", 1) ||
        my_b_write(&crash_safe_index_file,
                   (const uchar*)previous_gtid_set_buffer,
                   gtid_set_length))
    {
      sql_print_error("MYSQL_BIN_LOG::add_log_to_index failed to "
                      "append previous_gtid_set: %s, to crash "
                      "safe index file.", previous_gtid_set_buffer);
      goto err;
    }
  }

  if (my_b_write(&crash_safe_index_file, (uchar*) "\n", 1))
  {
    sql_print_error("MYSQL_BIN_LOG::add_log_to_index failed "
                    "append new_line");
  }

  previous_gtid_set_map.insert(
      std::pair<string, string>(string((char*)log_name, log_name_len),
                                string((char*)previous_gtid_set_buffer,
                                       gtid_set_length)));

  if (flush_io_cache(&crash_safe_index_file) ||
      mysql_file_sync(crash_safe_index_file.file, MYF(MY_WME)))
  {
    sql_print_error("MYSQL_BIN_LOG::add_log_to_index failed to "
                    "sync crash safe index file while appending "
                    "log file name: %s.", log_name);
    goto err;
  }

  if (close_crash_safe_index_file())
  {
    sql_print_error("MYSQL_BIN_LOG::add_log_to_index failed to "
                    "close the crash safe index file.");
    goto err;
  }

  if (move_crash_safe_index_file_to_index_file(need_lock_index))
  {
    sql_print_error("MYSQL_BIN_LOG::add_log_to_index failed to "
                    "move crash safe index file to index file.");
    goto err;
  }

  DBUG_EXECUTE_IF("simulate_disk_full_add_log_to_index",
      { DBUG_SET("-d,simulate_no_free_space_error"); });

  my_free(previous_gtid_set_buffer);

  if (enable_raft_plugin && is_apply_log)
    apply_file_count++;

  DBUG_RETURN(0);

err:
  DBUG_EXECUTE_IF("simulate_disk_full_add_log_to_index", {
      DBUG_SET("-d,simulate_no_free_space_error");
      DBUG_SET("-d,simulate_file_write_error");});
  my_free(previous_gtid_set_buffer);
  DBUG_RETURN(-1);
}

int MYSQL_BIN_LOG::get_current_log(LOG_INFO* linfo, bool need_lock_log/*true*/)
{
  if (need_lock_log)
    mysql_mutex_lock(&LOCK_log);
  int ret = raw_get_current_log(linfo);
  if (need_lock_log)
    mysql_mutex_unlock(&LOCK_log);
  return ret;
}

void MYSQL_BIN_LOG::get_current_log_without_lock_log(LOG_INFO* linfo)
{
  mysql_mutex_assert_owner(&LOCK_binlog_end_pos);
  strmake(linfo->log_file_name, binlog_file_name,
          sizeof(linfo->log_file_name)-1);
  linfo->pos = binlog_end_pos;
}

int MYSQL_BIN_LOG::raw_get_current_log(LOG_INFO* linfo)
{
  strmake(linfo->log_file_name, log_file_name, sizeof(linfo->log_file_name)-1);
  linfo->pos = my_b_safe_tell(&log_file);
  return 0;
}

bool MYSQL_BIN_LOG::check_write_error(THD *thd)
{
  DBUG_ENTER("MYSQL_BIN_LOG::check_write_error");

  bool checked= FALSE;

  if (!thd->is_error())
    DBUG_RETURN(checked);

  // Check all conditions for one that matches the expected error
  const Sql_condition *err;
  auto it= thd->get_stmt_da()->sql_conditions();
  while ((err= it++) != nullptr && !checked)
  {
    switch (err->get_sql_errno())
    {
      case ER_TRANS_CACHE_FULL:
      case ER_STMT_CACHE_FULL:
      case ER_ERROR_ON_WRITE:
      case ER_BINLOG_LOGGING_IMPOSSIBLE:
        checked= TRUE;
        break;
    }
  }

  DBUG_PRINT("return", ("checked: %s", YESNO(checked)));
  DBUG_RETURN(checked);
}

void MYSQL_BIN_LOG::set_write_error(THD *thd, bool is_transactional)
{
  DBUG_ENTER("MYSQL_BIN_LOG::set_write_error");

  write_error= 1;

  if (check_write_error(thd))
    DBUG_VOID_RETURN;

  if (my_errno == EFBIG)
  {
    if (is_transactional)
    {
      my_message(ER_TRANS_CACHE_FULL, ER(ER_TRANS_CACHE_FULL), MYF(MY_WME));
    }
    else
    {
      my_message(ER_STMT_CACHE_FULL, ER(ER_STMT_CACHE_FULL), MYF(MY_WME));
    }
  }
  else
  {
    char errbuf[MYSYS_STRERROR_SIZE];
    my_error(ER_ERROR_ON_WRITE, MYF(MY_WME), name,
             errno, my_strerror(errbuf, sizeof(errbuf), errno));
  }

  DBUG_VOID_RETURN;
}

uint split_file_name_and_gtid_set_length(char *file_name_and_gtid_set_length)
{
  char *save_ptr = NULL;
  save_ptr = strchr(file_name_and_gtid_set_length, ' ');
  if (save_ptr != NULL)
  {
    *save_ptr = 0; // replace ' ' with '\0'
    save_ptr++;
    return atol(save_ptr);
  }
  return 0;
}

int MYSQL_BIN_LOG::get_total_log_files(
    bool need_lock_index, uint64_t* num_log_files)
{
  int error= 0;
  LOG_INFO temp_log_info;
  *num_log_files= 0;

  if (need_lock_index)
    mysql_mutex_lock(&LOCK_index);

  if ((error= find_log_pos(
          &temp_log_info, /*log_name=*/NullS, /*need_lock_index=*/false)))
    goto err;

  *num_log_files = *num_log_files + 1;
  while (!(error= find_next_log(&temp_log_info, /*need_lock_index=*/false)))
    *num_log_files = *num_log_files + 1;

err:
  if (need_lock_index)
    mysql_mutex_unlock(&LOCK_index);

  if (error == LOG_INFO_EOF)
    error= 0; // EOF is not an error

  return error;
}

/**
  Find the position in the log-index-file for the given log name.

  @param[out] linfo The found log file name will be stored here, along
  with the byte offset of the next log file name in the index file.
  @param log_name Filename to find in the index file, or NULL if we
  want to read the first entry.
  @param need_lock_index If false, this function acquires LOCK_index;
  otherwise the lock should already be held by the caller.

  @note
    On systems without the truncate function the file will end with one or
    more empty lines.  These will be ignored when reading the file.

  @retval
    0			ok
  @retval
    LOG_INFO_EOF	        End of log-index-file found
  @retval
    LOG_INFO_IO		Got IO error while reading file
*/

int MYSQL_BIN_LOG::find_log_pos(LOG_INFO *linfo, const char *log_name,
                                bool need_lock_index)
{
  int error= 0;
  char *full_fname= linfo->log_file_name;
  char full_log_name[FN_REFLEN];
  char file_name_and_gtid_set_length[FILE_AND_GTID_SET_LENGTH];

  uint log_name_len= 0, fname_len= 0;
  DBUG_ENTER("find_log_pos");
  full_log_name[0]= full_fname[0]= 0;

  /*
    Mutex needed because we need to make sure the file pointer does not
    move from under our feet
  */
  if (need_lock_index)
    mysql_mutex_lock(&LOCK_index);
  else
    mysql_mutex_assert_owner(&LOCK_index);

  // extend relative paths for log_name to be searched
  if (log_name)
  {
    if(normalize_binlog_name(full_log_name, log_name, is_relay_log))
    {
      error= LOG_INFO_EOF;
      goto end;
    }
  }

  log_name_len= log_name ? (uint) strlen(full_log_name) : 0;
  DBUG_PRINT("enter", ("log_name: %s, full_log_name: %s",
                       log_name ? log_name : "NULL", full_log_name));

  /* As the file is flushed, we can't get an error here */
  my_b_seek(&index_file, (my_off_t) 0);

  for (;;)
  {
    uint length;
    my_off_t offset= my_b_tell(&index_file);

    DBUG_EXECUTE_IF("simulate_find_log_pos_error",
                    error=  LOG_INFO_EOF; break;);
    /* If we get 0 or 1 characters, this is the end of the file */
    if ((length = my_b_gets(&index_file, file_name_and_gtid_set_length,
                            FILE_AND_GTID_SET_LENGTH)) <= 1)
    {
      /* Did not find the given entry; Return not found or error */
      error= !index_file.error ? LOG_INFO_EOF : LOG_INFO_IO;
      break;
    }

    file_name_and_gtid_set_length[length - 1] = 0;
    uint gtid_string_length =
      split_file_name_and_gtid_set_length(file_name_and_gtid_set_length);
    if (gtid_string_length > 0)
    {
      my_b_seek(&index_file, my_b_tell(&index_file) + gtid_string_length + 1);
    }

    // extend relative paths and match against full path
    if (normalize_binlog_name(full_fname, file_name_and_gtid_set_length,
                              is_relay_log))
    {
      error= LOG_INFO_EOF;
      break;
    }
    fname_len= (uint) strlen(full_fname);

    // if the log entry matches, null string matching anything
    if (!log_name ||
       (log_name_len == fname_len &&
        !strncmp(full_fname, full_log_name, log_name_len)))
    {
      DBUG_PRINT("info", ("Found log file entry"));
      linfo->index_file_start_offset= offset;
      linfo->index_file_offset = my_b_tell(&index_file);
      break;
    }
    linfo->entry_index++;
  }

end:
  if (need_lock_index)
    mysql_mutex_unlock(&LOCK_index);
  DBUG_RETURN(error);
}


/**
  Find the position in the log-index-file for the given log name.

  @param[out] linfo The filename will be stored here, along with the
  byte offset of the next filename in the index file.

  @param need_lock_index If true, LOCK_index will be acquired;
  otherwise it should already be held by the caller.

  @note
    - Before calling this function, one has to call find_log_pos()
    to set up 'linfo'
    - Mutex needed because we need to make sure the file pointer does not move
    from under our feet

  @retval 0 ok
  @retval LOG_INFO_EOF End of log-index-file found
  @retval LOG_INFO_IO Got IO error while reading file
*/
int MYSQL_BIN_LOG::find_next_log(LOG_INFO* linfo, bool need_lock_index)
{
  int error= 0;
  uint length, gtid_string_length;
  char file_name_and_gtid_set_length[FILE_AND_GTID_SET_LENGTH];
  char *full_fname= linfo->log_file_name;
  DBUG_ENTER("find_next_log");

  if (need_lock_index)
    mysql_mutex_lock(&LOCK_index);
  else
    mysql_mutex_assert_owner(&LOCK_index);

  DBUG_PRINT("enter", ("index_file_offset: %llu", linfo->index_file_offset));
  /* As the file is flushed, we can't get an error here */
  my_b_seek(&index_file, linfo->index_file_offset);

  linfo->index_file_start_offset= linfo->index_file_offset;
  if ((length = my_b_gets(&index_file,
                          file_name_and_gtid_set_length,
                          FILE_AND_GTID_SET_LENGTH)) <= 1)
  {
    error = !index_file.error ? LOG_INFO_EOF : LOG_INFO_IO;
    goto err;
  }

  if (file_name_and_gtid_set_length[0] != 0)
  {
    file_name_and_gtid_set_length[length - 1] = 0;
    gtid_string_length =
      split_file_name_and_gtid_set_length(file_name_and_gtid_set_length);
    if (gtid_string_length > 0)
    {
      my_b_seek(&index_file, my_b_tell(&index_file)+ gtid_string_length + 1);
    }

    if(normalize_binlog_name(full_fname, file_name_and_gtid_set_length,
                             is_relay_log))
    {
      error= LOG_INFO_EOF;
      goto err;
    }
  }

  linfo->index_file_offset= my_b_tell(&index_file);

err:
  if (need_lock_index)
    mysql_mutex_unlock(&LOCK_index);
  DBUG_RETURN(error);
}


/**
  Removes files, as part of a RESET MASTER or RESET SLAVE statement,
  by deleting all logs refered to in the index file. Then, it starts
  writing to a new log file.

  The new index file will only contain this file.

  @param thd Thread

  @note
    If not called from slave thread, write start event to new log

  @retval
    0	ok
  @retval
    1   error
*/
bool MYSQL_BIN_LOG::reset_logs(THD* thd)
{
  LOG_INFO linfo;
  bool error=0;
  int err;
  const char* save_name;
  DBUG_ENTER("reset_logs");

  /*
    Flush logs for storage engines, so that the last transaction
    is fsynced inside storage engines.
  */
  if (ha_flush_logs(NULL))
    DBUG_RETURN(1);

  ha_reset_logs(thd);

  /*
    We need to get both locks to be sure that no one is trying to
    write to the index log file.
  */
  mysql_mutex_lock(&LOCK_log);
  mysql_mutex_lock(&LOCK_index);

  /*
    The following mutex is needed to ensure that no threads call
    'delete thd' as we would then risk missing a 'rollback' from this
    thread. If the transaction involved MyISAM tables, it should go
    into binlog even on rollback.
  */
  mutex_lock_all_shards(SHARDED(&LOCK_thread_count));

  global_sid_lock->wrlock();

  /* Save variables so that we can reopen the log */
  save_name=name;
  name=0;					// Protect against free
  close(LOG_CLOSE_TO_BE_OPENED);

  /*
    First delete all old log files and then update the index file.
    As we first delete the log files and do not use sort of logging,
    a crash may lead to an inconsistent state where the index has
    references to non-existent files.

    We need to invert the steps and use the purge_index_file methods
    in order to make the operation safe.
  */

  previous_gtid_set_map.clear();
  if ((err= find_log_pos(&linfo, NullS, false/*need_lock_index=false*/)) != 0)
  {
    uint errcode= purge_log_get_error_code(err);
    sql_print_error("Failed to locate old binlog or relay log files");
    my_message(errcode, ER(errcode), MYF(0));
    error= 1;
    goto err;
  }

  for (;;)
  {
    if ((error= my_delete_allow_opened(linfo.log_file_name, MYF(0))) != 0)
    {
      if (my_errno == ENOENT)
      {
        push_warning_printf(current_thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_LOG_PURGE_NO_FILE, ER(ER_LOG_PURGE_NO_FILE),
                            linfo.log_file_name);
        sql_print_information("Failed to delete file '%s'",
                              linfo.log_file_name);
        my_errno= 0;
        error= 0;
      }
      else
      {
        push_warning_printf(current_thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_BINLOG_PURGE_FATAL_ERR,
                            "a problem with deleting %s; "
                            "consider examining correspondence "
                            "of your binlog index file "
                            "to the actual binlog files",
                            linfo.log_file_name);
        error= 1;
        goto err;
      }
    }
    if (find_next_log(&linfo, false/*need_lock_index=false*/))
      break;
  }

  /* Start logging with a new file */
  close(LOG_CLOSE_INDEX | LOG_CLOSE_TO_BE_OPENED);
  if ((error= my_delete_allow_opened(index_file_name, MYF(0))))	// Reset (open will update)
  {
    if (my_errno == ENOENT)
    {
      push_warning_printf(current_thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_LOG_PURGE_NO_FILE, ER(ER_LOG_PURGE_NO_FILE),
                          index_file_name);
      sql_print_information("Failed to delete file '%s'",
                            index_file_name);
      my_errno= 0;
      error= 0;
    }
    else
    {
      push_warning_printf(current_thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_BINLOG_PURGE_FATAL_ERR,
                          "a problem with deleting %s; "
                          "consider examining correspondence "
                          "of your binlog index file "
                          "to the actual binlog files",
                          index_file_name);
      error= 1;
      goto err;
    }
  }

#ifdef HAVE_REPLICATION
  if (is_relay_log)
  {
    DBUG_ASSERT(active_mi != NULL);
    DBUG_ASSERT(active_mi->rli != NULL);
    (const_cast<Gtid_set *>(active_mi->rli->get_gtid_set()))->clear();
  }
  else
  {
    gtid_state->clear();
    // don't clear global_sid_map because it's used by the relay log too
    if (gtid_state->init() != 0)
      goto err;
  }
#endif

  if (!open_index_file(index_file_name, 0, false/*need_lock_index=false*/))
    if ((error= open_binlog(save_name, 0, io_cache_type,
                            max_size, false,
                            false/*need_lock_index=false*/,
                            false/*need_sid_lock=false*/,
                            NULL)))
      goto err;
  my_free((void *) save_name);

err:
  if (error == 1)
    name= const_cast<char*>(save_name);
  global_sid_lock->unlock();
  mutex_unlock_all_shards(SHARDED(&LOCK_thread_count));
  mysql_mutex_unlock(&LOCK_index);
  mysql_mutex_unlock(&LOCK_log);
  DBUG_RETURN(error);
}


/**
  Set the name of crash safe index file.

  @retval
    0   ok
  @retval
    1   error
*/
int MYSQL_BIN_LOG::set_crash_safe_index_file_name(const char *base_file_name)
{
  int error= 0;
  DBUG_ENTER("MYSQL_BIN_LOG::set_crash_safe_index_file_name");
  if (fn_format(crash_safe_index_file_name, base_file_name, mysql_data_home,
                ".index_crash_safe", MYF(MY_UNPACK_FILENAME | MY_SAFE_PATH |
                                         MY_REPLACE_EXT)) == NULL)
  {
    error= 1;
    sql_print_error("MYSQL_BIN_LOG::set_crash_safe_index_file_name failed "
                    "to set file name.");
  }
  DBUG_RETURN(error);
}


/**
  Open a (new) crash safe index file.

  @note
    The crash safe index file is a special file
    used for guaranteeing index file crash safe.
  @retval
    0   ok
  @retval
    1   error
*/
int MYSQL_BIN_LOG::open_crash_safe_index_file()
{
  int error= 0;
  File file= -1;

  DBUG_ENTER("MYSQL_BIN_LOG::open_crash_safe_index_file");

  if (!my_b_inited(&crash_safe_index_file))
  {
    if ((file= my_open(crash_safe_index_file_name, O_RDWR | O_CREAT | O_BINARY,
                       MYF(MY_WME | ME_WAITTANG))) < 0  ||
        init_io_cache(&crash_safe_index_file, file, IO_SIZE, WRITE_CACHE,
                      0, 0, MYF(MY_WME | MY_NABP | MY_WAIT_IF_FULL)))
    {
      error= 1;
      sql_print_error("MYSQL_BIN_LOG::open_crash_safe_index_file failed "
                      "to open temporary index file.");
    }
  }
  DBUG_RETURN(error);
}


/**
  Close the crash safe index file.

  @note
    The crash safe file is just closed, is not deleted.
    Because it is moved to index file later on.
  @retval
    0   ok
  @retval
    1   error
*/
int MYSQL_BIN_LOG::close_crash_safe_index_file()
{
  int error= 0;

  DBUG_ENTER("MYSQL_BIN_LOG::close_crash_safe_index_file");

  if (my_b_inited(&crash_safe_index_file))
  {
    end_io_cache(&crash_safe_index_file);
    error= my_close(crash_safe_index_file.file, MYF(0));
  }
  memset(&crash_safe_index_file, 0, sizeof(crash_safe_index_file));

  DBUG_RETURN(error);
}


/**
  Delete relay log files prior to rli->group_relay_log_name
  (i.e. all logs which are not involved in a non-finished group
  (transaction)), remove them from the index file and start on next
  relay log.

  IMPLEMENTATION

  - You must hold rli->data_lock before calling this function, since
    it writes group_relay_log_pos and similar fields of
    Relay_log_info.
  - Protects index file with LOCK_index
  - Delete relevant relay log files
  - Copy all file names after these ones to the front of the index file
  - If the OS has truncate, truncate the file, else fill it with \n'
  - Read the next file name from the index file and store in rli->linfo

  @param rli	       Relay log information
  @param included     If false, all relay logs that are strictly before
                      rli->group_relay_log_name are deleted ; if true, the
                      latter is deleted too (i.e. all relay logs
                      read by the SQL slave thread are deleted).

  @note
    - This is only called from the slave SQL thread when it has read
    all commands from a relay log and want to switch to a new relay log.
    - When this happens, we can be in an active transaction as
    a transaction can span over two relay logs
    (although it is always written as a single block to the master's binary
    log, hence cannot span over two master's binary logs).

  @retval
    0			ok
  @retval
    LOG_INFO_EOF	        End of log-index-file found
  @retval
    LOG_INFO_SEEK	Could not allocate IO cache
  @retval
    LOG_INFO_IO		Got IO error while reading file
*/

#ifdef HAVE_REPLICATION

int MYSQL_BIN_LOG::purge_first_log(Relay_log_info* rli, bool included)
{
  int error;
  char *to_purge_if_included= NULL;
  DBUG_ENTER("purge_first_log");

  DBUG_ASSERT(current_thd->system_thread == SYSTEM_THREAD_SLAVE_SQL);
  DBUG_ASSERT(is_relay_log);
  DBUG_ASSERT(is_open());
  DBUG_ASSERT(rli->slave_running == 1);
  DBUG_ASSERT(!strcmp(rli->linfo.log_file_name,rli->get_event_relay_log_name()));

  mysql_mutex_assert_owner(&rli->data_lock);

  mysql_mutex_lock(&LOCK_index);
  to_purge_if_included= my_strdup(rli->get_group_relay_log_name(), MYF(0));

  /*
    Read the next log file name from the index file and pass it back to
    the caller.
  */
  if((error=find_log_pos(&rli->linfo, rli->get_event_relay_log_name(),
                         false/*need_lock_index=false*/)) ||
     (error=find_next_log(&rli->linfo, false/*need_lock_index=false*/)))
  {
    char buff[22];
    sql_print_error("next log error: %d  offset: %s  log: %s included: %d",
                    error,
                    llstr(rli->linfo.index_file_offset,buff),
                    rli->get_event_relay_log_name(),
                    included);
    goto err;
  }

  /*
    Reset rli's coordinates to the current log.
  */
  rli->set_event_relay_log_pos(BIN_LOG_HEADER_SIZE);
  rli->set_event_relay_log_name(rli->linfo.log_file_name);

  /*
    If we removed the rli->group_relay_log_name file,
    we must update the rli->group* coordinates, otherwise do not touch it as the
    group's execution is not finished (e.g. COMMIT not executed)
  */
  if (included)
  {
    rli->set_group_relay_log_pos(BIN_LOG_HEADER_SIZE);
    rli->set_group_relay_log_name(rli->linfo.log_file_name);
    rli->notify_group_relay_log_name_update();
  }
  /*
    Store where we are in the new file for the execution thread.
    If we are in the middle of a group), then we should not store
    the position in the repository, instead in that case set a flag
    to true which indicates that a 'forced flush' is postponed due
    to transaction split across the relaylogs.
  */
  if (!rli->is_in_group())
    rli->flush_info(TRUE);
  else
    rli->force_flush_postponed_due_to_split_trans= true;

  DBUG_EXECUTE_IF("crash_before_purge_logs", DBUG_SUICIDE(););

  mysql_mutex_lock(&rli->log_space_lock);
  rli->relay_log.purge_logs(to_purge_if_included, included,
                            false/*need_lock_index=false*/,
                            false/*need_update_threads=false*/,
                            &rli->log_space_total, true);
  // Tell the I/O thread to take the relay_log_space_limit into account
  rli->ignore_log_space_limit= 0;
  mysql_mutex_unlock(&rli->log_space_lock);

  /*
    Ok to broadcast after the critical region as there is no risk of
    the mutex being destroyed by this thread later - this helps save
    context switches
  */
  mysql_cond_broadcast(&rli->log_space_cond);

  /*
   * Need to update the log pos because purge logs has been called
   * after fetching initially the log pos at the begining of the method.
   */
  if((error=find_log_pos(&rli->linfo, rli->get_event_relay_log_name(),
                         false/*need_lock_index=false*/)))
  {
    char buff[22];
    sql_print_error("next log error: %d  offset: %s  log: %s included: %d",
                    error,
                    llstr(rli->linfo.index_file_offset,buff),
                    rli->get_group_relay_log_name(),
                    included);
    goto err;
  }

  /* If included was passed, rli->linfo should be the first entry. */
  DBUG_ASSERT(!included || rli->linfo.index_file_start_offset == 0);

err:
  my_free(to_purge_if_included);
  mysql_mutex_unlock(&LOCK_index);
  DBUG_RETURN(error);
}


void MYSQL_BIN_LOG::purge_apply_logs()
{
  if (!is_apply_log)
    return;

  // No need to trigger purge if number of apply log files in the system
  // currently is lower than apply_log_retention_num
  if (apply_file_count <= apply_log_retention_num)
    return;

  time_t purge_time = my_time(0) - apply_log_retention_duration /* mins */ * 60;
  if (purge_time > 0)
  {
    ha_flush_logs(NULL);
    purge_logs_before_date(
        purge_time, /*auto_purge=*/true, /*stop_purge=*/true);
  }

  return;
}

/**
  Remove all logs before the given log from disk and from the index file.

  @param to_log	             Delete all log file name before this file.
  @param included            If true, to_log is deleted too.
  @param need_lock_index
  @param need_update_threads If we want to update the log coordinates of
                             all threads. False for relay logs, true otherwise.
  @param freed_log_space     If not null, decrement this variable of
                             the amount of log space freed
  @param auto_purge          True if this is an automatic purge.
  @param max_log             If max_log is found in the index before to_log,
                             then do not purge anything beyond that point

  @note
    If any of the logs before the deleted one is in use,
    only purge logs up to this one.

  @retval
    0			ok
  @retval
    LOG_INFO_EOF		      to_log not found
    LOG_INFO_EMFILE       Too many files opened
    LOG_INFO_IO           Got IO error while reading/writing file
    LOG_INFO_FATAL        If any other than ENOENT error from
                          mysql_file_stat() or mysql_file_delete()
*/

int MYSQL_BIN_LOG::purge_logs(const char *to_log,
                              bool included,
                              bool need_lock_index,
                              bool need_update_threads,
                              std::atomic_ullong *decrease_log_space,
                              bool auto_purge,
                              const char* max_log)
{
  int error= 0, error_index= 0,
      no_of_log_files_to_purge= 0,
      no_of_threads_locking_log= 0;
  uint64_t num_log_files= 0;
  bool exit_loop= 0;
  bool found_last_safe_file= false;
  LOG_INFO log_info;
  THD *thd= current_thd;
  std::list<std::string> delete_list;
  std::pair<std::string, uint> file_index_pair;
  std::string safe_purge_file;

  DBUG_ENTER("purge_logs");
  DBUG_PRINT("info",("to_log= %s",to_log));

  if (need_lock_index)
    mysql_mutex_lock(&LOCK_index);
  else
    mysql_mutex_assert_owner(&LOCK_index);
  if ((error=find_log_pos(&log_info, to_log, false/*need_lock_index=false*/)))
  {
    sql_print_error("MYSQL_BIN_LOG::purge_logs was called with file %s not "
                    "listed in the index.", to_log);
    goto err;
  }

  no_of_log_files_to_purge= log_info.entry_index;

  /*
    File name exists in index file; delete until we find this file
    or a file that is used.
  */
  if ((error=find_log_pos(&log_info, NullS, false/*need_lock_index=false*/)))
    goto err;

  if (enable_raft_plugin && !is_apply_log)
  {
    thd->clear_safe_purge_file();

    // Consult the plugin for file that could be deleted safely.
    // This will also allow plugin to clean up its index files and other states
    // (if any)
    file_index_pair= extract_file_index(std::string(to_log));
    if (!included && file_index_pair.second > 0)
      file_index_pair.second -= 1;

    // Nothing to purge if file index is 0
    if (!included && file_index_pair.second == 0)
      goto err;

    error= RUN_HOOK_STRICT(
        raft_replication, purge_logs, (current_thd, file_index_pair.second));

    if (error)
    {
      // NO_LINT_DEBUG
      sql_print_error("MYSQL_BIN_LOG::purge_logs raft plugin failed in "
          "purge_logs(). file-name: %s", to_log);
      goto err;
    }

    safe_purge_file= thd->get_safe_purge_file();
  }

  while ((strcmp(to_log,log_info.log_file_name) || (exit_loop=included)))
  {
    if (found_last_safe_file)
    {
      // It is not safe to delete any more files since raft needs it for
      // durability or there are peers trying to read from this file
      if(!auto_purge)
        // TODO: Converge on raft specific error codes here
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_WARN_PURGE_LOG_IS_ACTIVE,
                            ER(ER_WARN_PURGE_LOG_IS_ACTIVE),
                            log_info.log_file_name);

      break;
    }

    if(is_active(log_info.log_file_name) ||
        (enable_raft_plugin && max_log &&
         strcmp(max_log, log_info.log_file_name) == 0))
    {
      // Either the file is active or in raft mode found 'max_log' in the index.
      // Should not delete anything at or beyond 'max_log'
      if(!auto_purge)
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_WARN_PURGE_LOG_IS_ACTIVE,
                            ER(ER_WARN_PURGE_LOG_IS_ACTIVE),
                            log_info.log_file_name);
      break;
    }

    if ((no_of_threads_locking_log= log_in_use(log_info.log_file_name)))
    {
      if(!auto_purge)
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_WARN_PURGE_LOG_IN_USE,
                            ER(ER_WARN_PURGE_LOG_IN_USE),
                            log_info.log_file_name,  no_of_threads_locking_log,
                            delete_list.size(), no_of_log_files_to_purge);
      break;
    }

    delete_list.push_back(std::string(log_info.log_file_name));

    if (enable_raft_plugin && !is_apply_log && safe_purge_file.length() > 0 &&
        !strcmp(safe_purge_file.c_str(), log_info.log_file_name))
      found_last_safe_file= true;

    if (find_next_log(&log_info, false/*need_lock_index=false*/) || exit_loop)
      break;
  }

  DBUG_EXECUTE_IF("crash_purge_before_update_index", DBUG_SUICIDE(););

  /* Read each entry from the list and delete the file. */
  if ((error_index= purge_logs_in_list(delete_list, thd, decrease_log_space,
                                 false/*need_lock_index=false*/)))
    sql_print_error("MYSQL_BIN_LOG::purge_logs failed to"
                    " process registered files that would be purged.");

  DBUG_EXECUTE_IF("crash_purge_critical_before_update_index", DBUG_SUICIDE(););

  if (delete_list.size())
  {
    /* We know how many files to delete. Update index file. */
    if ((error=remove_logs_from_index(&log_info, need_update_threads)))
    {
      sql_print_error("MYSQL_BIN_LOG::purge_logs"
          " failed to update the index file");
      goto err;
    }
  }

  if (enable_raft_plugin && is_apply_log)
  {
    if (apply_file_count > delete_list.size())
      apply_file_count -= delete_list.size();
    else
    {
      // NO_LINT_DEBUG
      sql_print_information("Apply file count needs to be fixed. "
          "apply_file_count = %lu, number of deleted apply files = %lu",
          apply_file_count.load(), delete_list.size());

      error= get_total_log_files(/*need_lock_index=*/false, &num_log_files);

      apply_file_count.store(num_log_files);
      // NO_LINT_DEBUG
      sql_print_information("Fixed apply file count (%lu) by reading from "
          "index file.", apply_file_count.load());
    }
  }

  DBUG_EXECUTE_IF("crash_purge_non_critical_after_update_index", DBUG_SUICIDE(););

  // Update gtid_state->lost_gtids
  if (gtid_mode > 0 && !is_relay_log)
  {
    global_sid_lock->wrlock();
    error= init_gtid_sets(NULL,
                       const_cast<Gtid_set *>(gtid_state->get_lost_gtids()),
                       NULL,
                       opt_master_verify_checksum,
                       false/*false=don't need lock*/);
    global_sid_lock->unlock();
  }

  if (enable_raft_plugin && is_relay_log)
  {
    error= init_prev_gtid_sets_map();
  }

err:
  if (need_lock_index)
    mysql_mutex_unlock(&LOCK_index);

  error = error ? error : error_index;
  if (error && binlog_error_action == ABORT_SERVER)
  {
    exec_binlog_error_action_abort("Either disk is full or file system is read "
                                   "only while opening the binlog. Aborting the"
                                   " server.");
  }
  DBUG_RETURN(error);
}

int MYSQL_BIN_LOG::set_purge_index_file_name(const char *base_file_name)
{
  int error= 0;
  DBUG_ENTER("MYSQL_BIN_LOG::set_purge_index_file_name");
  if (fn_format(purge_index_file_name, base_file_name, mysql_data_home,
                ".~rec~", MYF(MY_UNPACK_FILENAME | MY_SAFE_PATH |
                              MY_REPLACE_EXT)) == NULL)
  {
    error= 1;
    sql_print_error("MYSQL_BIN_LOG::set_purge_index_file_name failed to set "
                      "file name.");
  }
  DBUG_RETURN(error);
}

int MYSQL_BIN_LOG::open_purge_index_file(bool destroy)
{
  int error= 0;
  File file= -1;

  DBUG_ENTER("MYSQL_BIN_LOG::open_purge_index_file");

  if (destroy)
    close_purge_index_file();

  if (!my_b_inited(&purge_index_file))
  {
    if ((file= my_open(purge_index_file_name, O_RDWR | O_CREAT | O_BINARY,
                       MYF(MY_WME | ME_WAITTANG))) < 0  ||
        init_io_cache(&purge_index_file, file, IO_SIZE,
                      (destroy ? WRITE_CACHE : READ_CACHE),
                      0, 0, MYF(MY_WME | MY_NABP | MY_WAIT_IF_FULL)))
    {
      error= 1;
      sql_print_error("MYSQL_BIN_LOG::open_purge_index_file failed to open register "
                      " file.");
    }
  }
  DBUG_RETURN(error);
}

int MYSQL_BIN_LOG::close_purge_index_file()
{
  int error= 0;

  DBUG_ENTER("MYSQL_BIN_LOG::close_purge_index_file");

  if (my_b_inited(&purge_index_file))
  {
    end_io_cache(&purge_index_file);
    error= my_close(purge_index_file.file, MYF(0));
  }
  my_delete(purge_index_file_name, MYF(0));
  memset(&purge_index_file, 0, sizeof(purge_index_file));

  DBUG_RETURN(error);
}

bool MYSQL_BIN_LOG::is_inited_purge_index_file()
{
  DBUG_ENTER("MYSQL_BIN_LOG::is_inited_purge_index_file");
  DBUG_RETURN (my_b_inited(&purge_index_file));
}

int MYSQL_BIN_LOG::sync_purge_index_file()
{
  int error= 0;
  DBUG_ENTER("MYSQL_BIN_LOG::sync_purge_index_file");

  if ((error= flush_io_cache(&purge_index_file)) ||
      (error= my_sync(purge_index_file.file, MYF(MY_WME))))
    DBUG_RETURN(error);

  DBUG_RETURN(error);
}

int MYSQL_BIN_LOG::register_purge_index_entry(const char *entry)
{
  int error= 0;
  DBUG_ENTER("MYSQL_BIN_LOG::register_purge_index_entry");

  if ((error=my_b_write(&purge_index_file, (const uchar*)entry, strlen(entry))) ||
      (error=my_b_write(&purge_index_file, (const uchar*)"\n", 1)))
    DBUG_RETURN (error);

  DBUG_RETURN(error);
}

int MYSQL_BIN_LOG::register_create_index_entry(const char *entry)
{
  DBUG_ENTER("MYSQL_BIN_LOG::register_create_index_entry");
  DBUG_RETURN(register_purge_index_entry(entry));
}

int MYSQL_BIN_LOG::purge_index_entry(THD *thd,
                                     std::atomic_ullong *decrease_log_space,
                                     bool need_lock_index)
{
  int error= 0;
  LOG_INFO log_info;
  LOG_INFO check_log_info;
  std::list<std::string> delete_list;

  DBUG_ENTER("MYSQL_BIN_LOG:purge_index_entry");

  DBUG_ASSERT(my_b_inited(&purge_index_file));

  if ((error=reinit_io_cache(&purge_index_file, READ_CACHE, 0, 0, 0)))
  {
    sql_print_error("MYSQL_BIN_LOG::purge_index_entry failed to reinit register file "
                    "for read");
    goto err;
  }

  for (;;)
  {
    uint length;

    if ((length=my_b_gets(&purge_index_file, log_info.log_file_name,
                          FN_REFLEN)) <= 1)
    {
      if (purge_index_file.error)
      {
        error= purge_index_file.error;
        sql_print_error("MYSQL_BIN_LOG::purge_index_entry error %d reading from "
                        "register file.", error);
        goto err;
      }

      /* Reached EOF */
      break;
    }

    /* Get rid of the trailing '\n' */
    log_info.log_file_name[length-1]= 0;

    if ((error= find_log_pos(&check_log_info, log_info.log_file_name,
                             need_lock_index)))
    {
      if (error != LOG_INFO_EOF)
      {
        if (thd)
        {
          push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
              ER_BINLOG_PURGE_FATAL_ERR,
              "a problem with deleting %s and "
              "reading the binlog index file",
              log_info.log_file_name);
        }
        else
        {
          sql_print_information("Failed to delete file '%s' and "
              "read the binlog index file",
              log_info.log_file_name);
        }
        break;
      }

      error= 0;
      if (!need_lock_index)
      {
        /*
           This is to avoid triggering an error in NDB.

           @todo: This is weird, what does NDB errors have to do with
           need_lock_index? Explain better or refactor /Sven
           */
        ha_binlog_index_purge_file(current_thd, log_info.log_file_name);
      }
      delete_list.push_back(std::string(log_info.log_file_name));
    }
  }

  if (!error)
    error= purge_logs_in_list(delete_list, thd, decrease_log_space,
                              need_lock_index);
err:
  DBUG_RETURN(error);
}


/**
  Deletes logs sepecified in a list if they exist on the file system
  @param delete_list         The list of log files to delete
  @param thd                 Pointer to the THD object
  @param decrease_log_space  Amount of space freed
  @param need_lock_index     Need to lock the index?

  @retval
    0                        ok
  @retval
    LOG_INFO_EMFILE	         Too many files opened
    LOG_INFO_FATAL           If any other than ENOENT error from
                             mysql_file_stat() or mysql_file_delete()
*/
int MYSQL_BIN_LOG::purge_logs_in_list(std::list<std::string>& delete_list,
                                      THD *thd,
                                      std::atomic_ullong *decrease_log_space,
                                      bool need_lock_index)
{
  MY_STAT s;
  int error= 0;

  DBUG_ENTER("MYSQL_BIN_LOG:purge_logs_in_list");

  for (auto& log_file_name: delete_list)
  {
    if (!mysql_file_stat(m_key_file_log, log_file_name.c_str(), &s, MYF(0)))
    {
      if (my_errno == ENOENT)
      {
        /*
          It's not fatal if we can't stat a log file that does not exist;
          If we could not stat, we won't delete.
        */
        if (thd)
        {
          push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                              ER_LOG_PURGE_NO_FILE, ER(ER_LOG_PURGE_NO_FILE),
                              log_file_name.c_str());
        }
        sql_print_information("Failed to execute mysql_file_stat on file '%s'",
			      log_file_name.c_str());
        my_errno= 0;
      }
      else
      {
        /*
          Other than ENOENT are fatal
        */
        if (thd)
        {
          push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                              ER_BINLOG_PURGE_FATAL_ERR,
                              "a problem with getting info on being purged %s; "
                              "consider examining correspondence "
                              "of your binlog index file "
                              "to the actual binlog files",
                              log_file_name.c_str());
        }
        else
        {
          sql_print_information("Failed to delete log file '%s'; "
                                "consider examining correspondence "
                                "of your binlog index file "
                                "to the actual binlog files",
                                log_file_name.c_str());
        }
        error= LOG_INFO_FATAL;
        break;
      }
    }
    else
    {
      DBUG_PRINT("info",("purging %s",log_file_name.c_str()));
      if (!mysql_file_delete(key_file_binlog, log_file_name.c_str(), MYF(0)))
      {
        DBUG_EXECUTE_IF("wait_in_purge_index_entry",
                        {
                            const char action[] = "now SIGNAL in_purge_index_entry WAIT_FOR go_ahead_sql";
                            DBUG_ASSERT(!debug_sync_set_action(thd, STRING_WITH_LEN(action)));
                            DBUG_SET("-d,wait_in_purge_index_entry");
                        };);

        if (decrease_log_space)
          decrease_log_space->fetch_sub(s.st_size);
      }
      else
      {
        if (my_errno == ENOENT)
        {
          if (thd)
          {
            push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                ER_LOG_PURGE_NO_FILE, ER(ER_LOG_PURGE_NO_FILE),
                                log_file_name.c_str());
          }
          sql_print_information("Failed to delete file '%s'",
                                  log_file_name.c_str());
          my_errno= 0;
        }
        else
        {
          if (thd)
          {
            push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                ER_BINLOG_PURGE_FATAL_ERR,
                "a problem with deleting %s; "
                "consider examining correspondence "
                "of your binlog index file "
                "to the actual binlog files",
                log_file_name.c_str());
          }
          else
          {
            sql_print_information("Failed to delete file '%s'; "
                "consider examining correspondence "
                "of your binlog index file "
                "to the actual binlog files",
                log_file_name.c_str());
          }
          if (my_errno == EMFILE)
          {
            DBUG_PRINT("info",
                ("my_errno: %d, set ret = LOG_INFO_EMFILE", my_errno));
            error= LOG_INFO_EMFILE;
            break;
          }
          error= LOG_INFO_FATAL;
          break;
        }
      }
    }
  }
  DBUG_RETURN(error);
}

/**
  Remove all logs before the given file date from disk and from the
  index file.

  @param thd		  Thread pointer
  @param purge_time	  Delete all log files before given date.
  @param auto_purge       True if this is an automatic purge.
  @param stop_purge       True if this is purge of apply logs and we have to
                          stop purging files based on apply_log_retention_num
  @param need_lock_index  True if LOCK_index need to be acquired
  @param max_log          If max_log is found in the index before to_log,
                          then do not purge anything beyond that point

  @note
    If any of the logs before the deleted one is in use,
    only purge logs up to this one.

  @retval
    0				ok
  @retval
    LOG_INFO_PURGE_NO_ROTATE	Binary file that can't be rotated
    LOG_INFO_FATAL              if any other than ENOENT error from
                                mysql_file_stat() or mysql_file_delete()
*/

int MYSQL_BIN_LOG::purge_logs_before_date(
    time_t purge_time,
    bool auto_purge,
    bool stop_purge,
    bool need_lock_index,
    const char* max_log)
{
  int error= 0;
  int no_of_threads_locking_log= 0;
  uint64_t no_of_log_files_purged= 0;
  bool log_is_active= false, log_is_in_use= false;
  char to_log[FN_REFLEN], copy_log_in_use[FN_REFLEN];
  LOG_INFO log_info;
  MY_STAT stat_area;
  THD *thd= current_thd;
  uint64_t max_files_to_purge= ULONG_MAX;

  DBUG_ENTER("purge_logs_before_date");

  if (need_lock_index)
    mysql_mutex_lock(&LOCK_index);
  else
    mysql_mutex_assert_owner(&LOCK_index);

  if (enable_raft_plugin && is_apply_log && stop_purge)
  {
    if (apply_file_count <= apply_log_retention_num)
      goto err;

    max_files_to_purge= apply_file_count - apply_log_retention_num;
  }

  to_log[0]= 0;

  if ((error=find_log_pos(&log_info, NullS, false/*need_lock_index=false*/)))
    goto err;

  while (!(log_is_active= is_active(log_info.log_file_name)))
  {
    if (enable_raft_plugin && max_log &&
        strcmp(max_log, log_info.log_file_name) == 0)
      break;

    if ((no_of_threads_locking_log= log_in_use(log_info.log_file_name)))
    {
      if (!auto_purge)
      {
        log_is_in_use= true;
        strcpy(copy_log_in_use, log_info.log_file_name);
      }
      break;
    }
    no_of_log_files_purged++;

    if (!mysql_file_stat(m_key_file_log,
                         log_info.log_file_name, &stat_area, MYF(0)))
    {
      if (my_errno == ENOENT)
      {
        /*
          It's not fatal if we can't stat a log file that does not exist.
        */
        my_errno= 0;
      }
      else
      {
        /*
          Other than ENOENT are fatal
        */
        if (thd)
        {
          push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                              ER_BINLOG_PURGE_FATAL_ERR,
                              "a problem with getting info on being purged %s; "
                              "consider examining correspondence "
                              "of your binlog index file "
                              "to the actual binlog files",
                              log_info.log_file_name);
        }
        else
        {
          sql_print_information("Failed to delete log file '%s'",
                                log_info.log_file_name);
        }
        error= LOG_INFO_FATAL;
        goto err;
      }
    }
    else
    {
      if (stat_area.st_mtime < purge_time)
        strmake(to_log,
                log_info.log_file_name,
                sizeof(log_info.log_file_name) - 1);
      else
        break;
    }

    if (enable_raft_plugin && is_apply_log && stop_purge &&
        (no_of_log_files_purged >= max_files_to_purge))
      break;

    if (find_next_log(&log_info, false/*need_lock_index=false*/))
      break;
  }

  if (log_is_active)
  {
    if(!auto_purge)
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_WARN_PURGE_LOG_IS_ACTIVE,
                          ER(ER_WARN_PURGE_LOG_IS_ACTIVE),
                          log_info.log_file_name);

  }

  if (log_is_in_use)
  {
    int no_of_log_files_to_purge= no_of_log_files_purged+1;
    while (strcmp(log_file_name, log_info.log_file_name))
    {
      if (mysql_file_stat(m_key_file_log, log_info.log_file_name,
                          &stat_area, MYF(0)))
      {
        if (stat_area.st_mtime < purge_time)
          no_of_log_files_to_purge++;
        else
          break;
      }
      if (find_next_log(&log_info, false/*need_lock_index=false*/))
      {
        no_of_log_files_to_purge++;
        break;
      }
    }

    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_WARN_PURGE_LOG_IN_USE,
                        ER(ER_WARN_PURGE_LOG_IN_USE),
                        copy_log_in_use, no_of_threads_locking_log,
                        no_of_log_files_purged, no_of_log_files_to_purge);
  }

  error= (to_log[0] ? purge_logs(to_log, true,
                                 false/*need_lock_index=false*/,
                                 true/*need_update_threads=true*/,
                                 nullptr, auto_purge, max_log) : 0);

err:
  if (need_lock_index)
    mysql_mutex_unlock(&LOCK_index);

  DBUG_RETURN(error);
}
#endif /* HAVE_REPLICATION */


/**
  Create a new log file name.

  @param buf		buf of at least FN_REFLEN where new name is stored

  @note
    If file name will be longer then FN_REFLEN it will be truncated
*/

void MYSQL_BIN_LOG::make_log_name(char* buf, const char* log_ident)
{
  uint dir_len = dirname_length(log_file_name);
  if (dir_len >= FN_REFLEN)
    dir_len=FN_REFLEN-1;
  strnmov(buf, log_file_name, dir_len);
  strmake(buf+dir_len, log_ident, FN_REFLEN - dir_len -1);
}


/**
  Check if we are writing/reading to the given log file.
*/

bool MYSQL_BIN_LOG::is_active(const char *log_file_name_arg)
{
  return !strcmp(log_file_name, log_file_name_arg);
}


/*
  Wrappers around new_file_impl to avoid using argument
  to control locking. The argument 1) less readable 2) breaks
  incapsulation 3) allows external access to the class without
  a lock (which is not possible with private new_file_without_locking
  method).

  @retval
    nonzero - error

*/

/* raft_rotate_info | various flags and raft specific info encapsulated
 *  into a struct
 *            | POSTAPPEND (append to log has already happened)
              | NO_OP (use consensus but special hook)
*/
int MYSQL_BIN_LOG::new_file(Format_description_log_event *extra_description_event,
                            RaftRotateInfo *raft_rotate_info)
{
  return new_file_impl(true/*need_lock_log=true*/, extra_description_event,
                       raft_rotate_info);
}

/*
  @retval
    nonzero - error
*/
int MYSQL_BIN_LOG::new_file_without_locking(Format_description_log_event *extra_description_event)
{
  return new_file_impl(false/*need_lock_log=false*/, extra_description_event);
}


/**
  Start writing to a new log file or reopen the old file.

  @param need_lock_log If true, this function acquires LOCK_log;
  otherwise the caller should already have acquired it.
  @param raft_flags  POSTAPPEND|NOOP
  POSTAPPEND - the rotate event has already
               been appended via raft and hence needs to be skipped.
  NOOP - in this option, this rotate event also acts as a Raft NOOP
         and hence needs to call a separate hook.
  @param config_change - the string payload for optional config
         change operation. This string is passed into a Metadata event
         before the RotateEvent. For non-config change rotates
         e.g. No-Op rotates and regular rotates, this should be nullptr.

  @retval 0 success
  @retval nonzero - error

  @note The new file name is stored last in the index file
*/
int MYSQL_BIN_LOG::new_file_impl(bool need_lock_log,
    Format_description_log_event *extra_description_event,
    RaftRotateInfo *raft_rotate_info)
{
  int error= 0, close_on_error= FALSE;
  char new_name[FN_REFLEN], *new_name_ptr, *old_name, *file_to_open;
  LOG_INFO info;

  // Indicates if an error occured inside raft plugin
  int consensus_error= 0;

  DBUG_ENTER("MYSQL_BIN_LOG::new_file_impl");
  if (!is_open())
  {
    DBUG_PRINT("info",("log is closed"));
    DBUG_RETURN(error);
  }

  // If this rotation is initiated from raft plugin, we
  // expect raft to be enabled, otherwise we fail early
  if (raft_rotate_info && !enable_raft_plugin)
  {
    DBUG_PRINT("info",("enable_raft_plugin is off"));
    DBUG_RETURN(1);
  }

  if (need_lock_log)
    mysql_mutex_lock(&LOCK_log);
  else
    mysql_mutex_assert_owner(&LOCK_log);
  mysql_mutex_lock(&LOCK_xids);
  /*
    We need to ensure that the number of prepared XIDs are 0.

    If m_prep_xids is not zero:
    - We wait for storage engine commit, hence decrease m_prep_xids
    - We keep the LOCK_log to block new transactions from being
      written to the binary log.
   */
  while (get_prep_xids() > 0)
  {
    DEBUG_SYNC(current_thd, "before_rotate_binlog_file");
    mysql_cond_wait(&m_prep_xids_cond, &LOCK_xids);
  }
  mysql_mutex_unlock(&LOCK_xids);

  if (opt_trim_binlog)
  {
    /* Wait for all non-xid trxs to finish */
    mysql_mutex_lock(&LOCK_non_xid_trxs);
    while (get_non_xid_trxs() > 0)
      mysql_cond_wait(&non_xid_trxs_cond, &LOCK_non_xid_trxs);
    mysql_mutex_unlock(&LOCK_non_xid_trxs);
  }

  bool no_op= raft_rotate_info && raft_rotate_info->noop;
  bool config_change_rotate= raft_rotate_info &&
                             raft_rotate_info->config_change_rotate;
  bool in_listener_thread_leader= no_op || config_change_rotate;

  // skip rotate event append implies rotate event has already
  // been appended to relay log by plugin
  bool skip_re_append= raft_rotate_info && raft_rotate_info->post_append;
  if (skip_re_append) {
    DBUG_ASSERT(is_relay_log);
  }

  // Convenience struct to pass down call tree, e.g. open_binlog
  RaftRotateInfo raft_rotate_info_tmp;
  // Raft only Temporary IO cache for Rotate Event flushing
  IO_CACHE raft_io_cache;
  // rotate events need to go through consensus so that we don't have
  // to trim previous a rotate event, i.e. into a rotated file.
  // if we are rotating an is_apply_log = true, then we are a slave
  // trying to do FLUSH BINARY LOGS, which should not have to go
  // through consensus
  bool rotate_via_raft= enable_raft_plugin && (no_op || !is_relay_log)
                        && (!is_apply_log);
  if (rotate_via_raft) {
    if (!raft_rotate_info)
      raft_rotate_info= &raft_rotate_info_tmp;
    raft_rotate_info->rotate_via_raft= rotate_via_raft; /* true */

    // post append rotates - no flush and before commit stages required
    // rotate_via_raft - flush and before commit of rotate event needs to happen
    DBUG_ASSERT(!skip_re_append);
    if (config_change_rotate)
    {
      init_io_cache(&raft_io_cache, -1,
                    /* 1000 buffer for rotate and rest of payload */
                    raft_rotate_info->config_change.size() + 1000,
                    WRITE_CACHE, 0, 0, MYF(MY_WME));
    } else
    {
      init_io_cache(&raft_io_cache, -1, 4000, WRITE_CACHE, 0, 0, MYF(MY_WME));
    }
  }

  mysql_mutex_lock(&LOCK_index);

  if (DBUG_EVALUATE_IF("expire_logs_always", 0, 1)
      && (error= ha_flush_logs(NULL)))
    goto end;

  mysql_mutex_assert_owner(&LOCK_log);
  mysql_mutex_assert_owner(&LOCK_index);

  /*
    If user hasn't specified an extension, generate a new log name
    We have to do this here and not in open as we want to store the
    new file name in the current binary log file.
  */
  // NOTE - the cur_log_ext is changed after generate_new_name,
  // although we are still writing to old file till it is closed
  // below
  new_name_ptr= new_name;
  if ((error= generate_new_name(new_name, name)))
  {
    // Use the old name if generation of new name fails.
    strcpy(new_name, name);
    close_on_error= TRUE;
    goto end;
  }

  // we have moved the cur log ext and in raft
  // this will mess with the index
  if (rotate_via_raft)
    cur_log_ext--;

  if (!skip_re_append)
  {
    /*
      We log the whole file name for log file as the user may decide
      to change base names at some point.
    */
    Rotate_log_event r(new_name+dirname_length(new_name), 0, LOG_EVENT_OFFSET,
                       is_relay_log && !no_op ? Rotate_log_event::RELAY_LOG : 0);
    /*
      The current relay-log's closing Rotate event must have checksum
      value computed with an algorithm of the last relay-logged FD event.
    */
    if (is_relay_log)
      r.checksum_alg= relay_log_checksum_alg;

    // The no-op and config change rotations happen
    // in listener thread where start_time is invalid in THD
    if (in_listener_thread_leader)
      r.when.tv_sec= my_time(0);

    if (rotate_via_raft)
    {
      // in raft mode checksum has to be turned off,
      // because the raft plugin will patch the events
      // and generate the final checksum.
      r.checksum_alg= BINLOG_CHECKSUM_ALG_OFF;

      // write the metadata log event to the cache first
      Metadata_log_event me;
      me.checksum_alg= BINLOG_CHECKSUM_ALG_OFF;
      if (in_listener_thread_leader)
        me.when.tv_sec= my_time(0);
      if (no_op)
      {
        me.set_raft_rotate_tag(Metadata_log_event::RRET_NOOP);
      }
      else if (config_change_rotate)
      {
        me.set_raft_rotate_tag(Metadata_log_event::RRET_CONFIG_CHANGE);
        me.set_raft_str(raft_rotate_info->config_change);
      }
      else
      {
        me.set_raft_rotate_tag(Metadata_log_event::RRET_SIMPLE_ROTATE);
      }

      if ((error= me.write(&raft_io_cache)))
      {
        char errbuf[MYSYS_STRERROR_SIZE];
        close_on_error= TRUE;
        my_printf_error(ER_ERROR_ON_WRITE, ER(ER_CANT_OPEN_FILE),
                    MYF(ME_FATALERROR), name,
                    errno, my_strerror(errbuf, sizeof(errbuf), errno));
        goto end;
      }
      bytes_written += me.data_written;
    }
    DBUG_ASSERT(!is_relay_log || rotate_via_raft || relay_log_checksum_alg != BINLOG_CHECKSUM_ALG_UNDEF);
    if(DBUG_EVALUATE_IF("fault_injection_new_file_rotate_event", (error=close_on_error=TRUE), FALSE) ||
       (error= r.write((rotate_via_raft) ? &(raft_io_cache) : &log_file)))
    {
      char errbuf[MYSYS_STRERROR_SIZE];
      DBUG_EXECUTE_IF("fault_injection_new_file_rotate_event", errno=2;);
      close_on_error= TRUE;
      my_printf_error(ER_ERROR_ON_WRITE, ER(ER_CANT_OPEN_FILE),
                      MYF(ME_FATALERROR), name,
                      errno, my_strerror(errbuf, sizeof(errbuf), errno));
      goto end;
    }
    bytes_written += r.data_written;
  }

  // AT THIS POINT we should block in Raft mode to replicate the Rotate event.
  if (rotate_via_raft)
  {
    RaftReplicateMsgOpType op_type=
        RaftReplicateMsgOpType::OP_TYPE_ROTATE;
    if (no_op)
      op_type= RaftReplicateMsgOpType::OP_TYPE_NOOP;
    else if (config_change_rotate)
      op_type= RaftReplicateMsgOpType::OP_TYPE_CHANGE_CONFIG;

    DBUG_EXECUTE_IF("simulate_before_flush_error_on_new_file", error= 1;);

    if (!error)
      error= RUN_HOOK_STRICT(raft_replication, before_flush,
                            (current_thd, &raft_io_cache, op_type));

    // time to safely readjust the cur_log_ext back to expected value
    if (!error)
    {
      error= RUN_HOOK_STRICT(
          raft_replication, before_commit, (current_thd, false));
      if (!error)
      {
        // If there was no error, there is a guarantee that this rotate
        // event has reached consensus. Hence this file extension can now
        // be extended. If we don't check for this error, before_commit
        // hook could have failed and then a truncation request could arrive
        // on the same rotate event/same file which would violate a raft
        // plugin invariant. Lets say the rotate event was the last event in
        // file 3. By incrementing the ext to 4, we would give the signal to
        // MySQL Raft plugin that file 3 has been rotated and therefore can
        // never be a candidate for trimming. However since we did not reach
        // consensus the rotate message in file 3 could get a TruncateOpsAfter
        // call which would trigger an assert
        cur_log_ext++;
      }
    }

    if (error)
    {
      sql_print_error("Failed to rotate binary log");
      consensus_error= 1;
      current_thd->clear_error(); // Clear previous errors first
      my_error(ER_RAFT_FILE_ROTATION_FAILED, MYF(0), 1);
      goto end;
    }
    if (!error)
    {
      // Store the rotate op_id in the raft_rotate_info for
      // open_binlog to use
      int64_t r_term, r_index;
      current_thd->get_trans_marker(&r_term, &r_index);
      raft_rotate_info->rotate_opid= std::make_pair(r_term, r_index);
    }
  }

  // Need flush before updating binlog_end_pos, otherwise dump thread
  // may give errors.
  if (error || flush_io_cache(&log_file))
  {
    error = 1;
    close_on_error = TRUE;
    goto end;
  }

  /*
    Update needs to be signalled even if there is no rotate event
    log rotation should give the waiting thread a signal to
    discover EOF and move on to the next log.
  */
  update_binlog_end_pos();

  // Let's stash the new file and pos in THD
  if (enable_raft_plugin && current_thd)
  {
    get_current_log_without_lock_log(&info);
    if (mysql_bin_log.is_apply_log)
      current_thd->set_trans_relay_log_pos(info.log_file_name, info.pos);
    else
      current_thd->set_trans_pos(info.log_file_name, info.pos, nullptr);
  }


  old_name=name;
  name=0;				// Don't free name
  close(LOG_CLOSE_TO_BE_OPENED | LOG_CLOSE_INDEX);

  if (checksum_alg_reset != BINLOG_CHECKSUM_ALG_UNDEF)
  {
    DBUG_ASSERT(!is_relay_log);
    DBUG_ASSERT(binlog_checksum_options != checksum_alg_reset);
    binlog_checksum_options= checksum_alg_reset;
  }
  /*
     Note that at this point, log_state != LOG_CLOSED (important for is_open()).
  */

  DEBUG_SYNC(current_thd, "before_rotate_binlog_file");
  /*
     new_file() is only used for rotation (in FLUSH LOGS or because size >
     max_binlog_size or max_relay_log_size).
     If this is a binary log, the Format_description_log_event at the beginning of
     the new file should have created=0 (to distinguish with the
     Format_description_log_event written at server startup, which should
     trigger temp tables deletion on slaves.
  */

  /* reopen index binlog file, BUG#34582 */
  file_to_open= index_file_name;
  error= open_index_file(index_file_name, 0, false/*need_lock_index=false*/);
  if (!error)
  {
    /* reopen the binary log file. */
    file_to_open= new_name_ptr;
    error= open_binlog(old_name, new_name_ptr, io_cache_type,
                       max_size, true/*null_created_arg=true*/,
                       false/*need_lock_index=false*/,
                       true/*need_sid_lock=true*/,
                       extra_description_event,
                       raft_rotate_info);
  }

  /* handle reopening errors */
  if (error)
  {
    char errbuf[MYSYS_STRERROR_SIZE];
    my_printf_error(ER_CANT_OPEN_FILE, ER(ER_CANT_OPEN_FILE),
                    MYF(ME_FATALERROR), file_to_open,
                    error, my_strerror(errbuf, sizeof(errbuf), error));
    close_on_error= TRUE;
  }
  my_free(old_name);

  // We do raft after commit hook only for regular rotates and
  // never for NO-OP rotates.
  // rotate_via_raft = (no_op || !is_relay) && (!is_apply)
  // therefore the condition below is equivalent to
  // (!is_relay) && (!is_apply) i.e this rotate is
  // on a binlog in the master
  // Why do we skip AFTER COMMIT for this rotate.
  // This is to prevent the same after commit notification coming
  // twice, once for this call and later for the apply thread
  // processing this NO-OP event. The double notification can
  // confuse LWM and outOfOrderTrxs computation.
  if (!error && rotate_via_raft && !no_op) {
    // not trapping return code, because this is the existing
    // pattern in most places of after_commit hook (TODO)
    (void)RUN_HOOK_STRICT(
        raft_replication, after_commit, (current_thd, false));
  }

end:

  if (error && close_on_error /* rotate or reopen failed */)
  {
    /*
      Close whatever was left opened.

      We are keeping the behavior as it exists today, ie,
      we disable logging and move on (see: BUG#51014).

      TODO: as part of WL#1790 consider other approaches:
       - kill mysql (safety);
       - try multiple locations for opening a log file;
       - switch server to protected/readonly mode
       - ...
    */
    close(LOG_CLOSE_INDEX);
    if (binlog_error_action == ABORT_SERVER ||
        binlog_error_action == ROLLBACK_TRX)
    {
      // Abort the server only if this is not a consensus error. Aborting the
      // server for consensus error is not good since it might lead to crashing
      // all instances in the ring on failure to propagate
      // rotate/no-op/config-change events
      if (!consensus_error)
        exec_binlog_error_action_abort("Either disk is full or file system is"
                                       " read only while rotating the binlog."
                                       " Aborting the server.");
    }
    else
      sql_print_error("Could not open %s for logging (error %d). "
                      "Turning logging off for the whole duration "
                      "of the MySQL server process. To turn it on "
                      "again: fix the cause, shutdown the MySQL "
                      "server and restart it.",
                      new_name_ptr, errno);
  }

  mysql_mutex_unlock(&LOCK_index);
  if (need_lock_log)
    mysql_mutex_unlock(&LOCK_log);

  DBUG_RETURN(error);
}

#ifdef HAVE_REPLICATION
/**
  Called after an event has been written to the relay log by the IO
  thread.  This flushes and possibly syncs the file (according to the
  sync options), rotates the file if it has grown over the limit, and
  finally calls signal_update().

  @note The caller must hold LOCK_log before invoking this function.

  @param mi Master_info for the IO thread.
  @param need_data_lock If true, mi->data_lock will be acquired if a
  rotation is needed.  Otherwise, mi->data_lock must be held by the
  caller.

  @retval false success
  @retval true error
*/
bool MYSQL_BIN_LOG::after_append_to_relay_log(Master_info *mi)
{
  DBUG_ENTER("MYSQL_BIN_LOG::after_append_to_relay_log");
  DBUG_PRINT("info",("max_size: %lu",max_size));

  // Check pre-conditions
  mysql_mutex_assert_owner(&LOCK_log);
  DBUG_ASSERT(is_relay_log);
  DBUG_ASSERT(current_thd->system_thread == SYSTEM_THREAD_SLAVE_IO);

  // Flush and sync
  bool error= false;
  if (flush_and_sync(0, 0) == 0)
  {
    DBUG_EXECUTE_IF ("set_max_size_zero",
                     {max_size=0;});
    // If relay log is too big, rotate
    if ((uint) my_b_append_tell(&log_file) >
        DBUG_EVALUATE_IF("rotate_slave_debug_group", 500,
                          DBUG_EVALUATE_IF("slave_skipping_gtid",
                                           870, max_size)))
    {
      mysql_mutex_lock(&mi->fde_lock);
      error= new_file_without_locking(
               mi->get_mi_descripion_event_with_no_lock());
      mysql_mutex_unlock(&mi->fde_lock);
      DBUG_EXECUTE_IF ("set_max_size_zero",
                       {
                       max_size=1073741824;
                       DBUG_SET("-d,set_max_size_zero");
                       DBUG_SET("-d,flush_after_reading_gtid_event");
                       });
    }
  }

  update_binlog_end_pos();

  DBUG_RETURN(error);
}


bool MYSQL_BIN_LOG::append_event(Log_event* ev, Master_info *mi)
{
  DBUG_ENTER("MYSQL_BIN_LOG::append");
  USER_STATS *us= current_thd ? thd_get_user_stats(current_thd) : NULL;

  // check preconditions
  DBUG_ASSERT(log_file.type == SEQ_READ_APPEND);
  DBUG_ASSERT(is_relay_log);

  mysql_mutex_unlock(&mi->data_lock);
  // acquire locks
  mysql_mutex_lock(&LOCK_log);

  // write data
  bool error = false;
  if (ev->write(&log_file) == 0)
  {
    bytes_written+= ev->data_written;
    relay_log_bytes_written += ev->data_written;
    if (us)
    {
      us->relay_log_bytes_written.inc(ev->data_written);
    }
    error= after_append_to_relay_log(mi);
  }
  else
    error= true;

  mysql_mutex_unlock(&LOCK_log);
  mysql_mutex_lock(&mi->data_lock);
  DBUG_RETURN(error);
}


bool MYSQL_BIN_LOG::append_buffer(const char* buf, uint len, Master_info *mi)
{
  DBUG_ENTER("MYSQL_BIN_LOG::append_buffer");
  // Release data_lock while writing to relay log. If slave IO thread
  // waits here for free space, we don't want SHOW SLAVE STATUS to
  // hang on mi->data_lock. Note LOCK_log mutex is sufficient to block
  // SQL thread when IO thread is updating relay log here.
  mysql_mutex_unlock(&mi->data_lock);
  mysql_mutex_lock(&LOCK_log);
  USER_STATS *us= current_thd ? thd_get_user_stats(current_thd) : NULL;

  // check preconditions
  DBUG_ASSERT(log_file.type == SEQ_READ_APPEND);
  DBUG_ASSERT(is_relay_log);

  DBUG_EXECUTE_IF("simulate_wait_for_relay_log_space",
                  {
                    const char act[]=
                      "now signal io_thread_blocked wait_for space_available";
                       DBUG_ASSERT(!debug_sync_set_action(current_thd,
                                      STRING_WITH_LEN(act)));
                    });

  // write data
  bool error= false;
  if (my_b_append(&log_file,(uchar*) buf,len) == 0)
  {
    bytes_written += len;
    relay_log_bytes_written += len;
    if (us)
    {
      us->relay_log_bytes_written.inc(len);
    }
    error= after_append_to_relay_log(mi);
  }
  else
    error= true;

  mysql_mutex_unlock(&LOCK_log);
  mysql_mutex_lock(&mi->data_lock);
  DBUG_RETURN(error);
}
#endif // ifdef HAVE_REPLICATION

bool MYSQL_BIN_LOG::flush_and_sync(bool async, const bool force)
{
  mysql_mutex_assert_owner(&LOCK_log);

  if (flush_io_cache(&log_file))
    return 1;

  std::pair<bool, bool> result= sync_binlog_file(force, async);

  return result.first;
}

void MYSQL_BIN_LOG::start_union_events(THD *thd, query_id_t query_id_param)
{
  DBUG_ASSERT(!thd->binlog_evt_union.do_union);
  thd->binlog_evt_union.do_union= TRUE;
  thd->binlog_evt_union.unioned_events= FALSE;
  thd->binlog_evt_union.unioned_events_trans= FALSE;
  thd->binlog_evt_union.first_query_id= query_id_param;
}

void MYSQL_BIN_LOG::stop_union_events(THD *thd)
{
  DBUG_ASSERT(thd->binlog_evt_union.do_union);
  thd->binlog_evt_union.do_union= FALSE;
}

bool MYSQL_BIN_LOG::is_query_in_union(THD *thd, query_id_t query_id_param)
{
  return (thd->binlog_evt_union.do_union &&
          query_id_param >= thd->binlog_evt_union.first_query_id);
}

/*
  Updates thd's position-of-next-event variables
  after a *real* write a file.
 */
void MYSQL_BIN_LOG::update_thd_next_event_pos(THD* thd)
{
  if (likely(thd != NULL))
  {
    thd->set_next_event_pos(log_file_name,
                            my_b_tell(&log_file));
  }
}

/*
  Moves the last bunch of rows from the pending Rows event to a cache (either
  transactional cache if is_transaction is @c true, or the non-transactional
  cache otherwise. Sets a new pending event.

  @param thd               a pointer to the user thread.
  @param evt               a pointer to the row event.
  @param is_transactional  @c true indicates a transactional cache,
                           otherwise @c false a non-transactional.
*/
int
MYSQL_BIN_LOG::flush_and_set_pending_rows_event(THD *thd,
                                                Rows_log_event* event,
                                                bool is_transactional)
{
  DBUG_ENTER("MYSQL_BIN_LOG::flush_and_set_pending_rows_event(event)");
  DBUG_ASSERT(mysql_bin_log.is_open());
  DBUG_PRINT("enter", ("event: 0x%lx", (long) event));

  int error= 0;
  binlog_cache_mngr *const cache_mngr= thd_get_cache_mngr(thd);

  DBUG_ASSERT(cache_mngr);

  binlog_cache_data *cache_data=
    cache_mngr->get_binlog_cache_data(is_transactional);

  DBUG_PRINT("info", ("cache_mngr->pending(): 0x%lx", (long) cache_data->pending()));

  if (Rows_log_event* pending= cache_data->pending())
  {
    /*
      Write pending event to the cache.
    */
    if (cache_data->write_event(thd, pending))
    {
      set_write_error(thd, is_transactional);
      if (check_write_error(thd) && cache_data &&
          stmt_cannot_safely_rollback(thd))
        cache_data->set_incident();
      delete pending;
      cache_data->set_pending(NULL);
      DBUG_RETURN(1);
    }

    delete pending;
  }

  cache_data->set_pending(event);

  DBUG_RETURN(error);
}

/**
  Write an event to the binary log.
*/

bool MYSQL_BIN_LOG::write_event(Log_event *event_info,
                                int force_cache_type,
                                bool write_meta_data_event)
{
  THD *thd= event_info->thd;
  bool error= 1;
  DBUG_ENTER("MYSQL_BIN_LOG::write_event(Log_event *)");

  if (thd->binlog_evt_union.do_union)
  {
    /*
      In Stored function; Remember that function call caused an update.
      We will log the function call to the binary log on function exit
    */
    thd->binlog_evt_union.unioned_events= TRUE;
    thd->binlog_evt_union.unioned_events_trans |=
      event_info->is_using_trans_cache();
    DBUG_RETURN(0);
  }

  /*
    We only end the statement if we are in a top-level statement.  If
    we are inside a stored function, we do not end the statement since
    this will close all tables on the slave. But there can be a special case
    where we are inside a stored function/trigger and a SAVEPOINT is being
    set in side the stored function/trigger. This SAVEPOINT execution will
    force the pending event to be flushed without an STMT_END_F flag. This
    will result in a case where following DMLs will be considered as part of
    same statement and result in data loss on slave. Hence in this case we
    force the end_stmt to be true.
  */
  bool const end_stmt= (thd->in_sub_stmt && thd->lex->sql_command ==
                        SQLCOM_SAVEPOINT)? true:
    (thd->locked_tables_mode && thd->lex->requires_prelocking());
  if (thd->binlog_flush_pending_rows_event(end_stmt,
                                           event_info->is_using_trans_cache()))
    DBUG_RETURN(error);

  /*
     In most cases this is only called if 'is_open()' is true; in fact this is
     mostly called if is_open() *was* true a few instructions before, but it
     could have changed since.
  */
  if (likely(is_open()))
  {
#ifdef HAVE_REPLICATION
    /*
      In the future we need to add to the following if tests like
      "do the involved tables match (to be implemented)
      binlog_[wild_]{do|ignore}_table?" (WL#1049)"
    */
    const char *local_db= event_info->get_db();
    if ((thd && !(thd->variables.option_bits & OPTION_BIN_LOG)) ||
	(thd->lex->sql_command != SQLCOM_ROLLBACK_TO_SAVEPOINT &&
         thd->lex->sql_command != SQLCOM_SAVEPOINT &&
         (!event_info->is_no_filter_event() &&
          !binlog_filter->db_ok(local_db))))
      DBUG_RETURN(0);
#endif /* HAVE_REPLICATION */

    if (force_cache_type == Log_event::EVENT_STMT_CACHE)
    {
      event_info->set_using_stmt_cache();
      event_info->set_immediate_logging();
    }
    else if (force_cache_type == Log_event::EVENT_TRANSACTIONAL_CACHE)
    {
      event_info->set_using_trans_cache();
    }

    DBUG_ASSERT(event_info->is_using_trans_cache() || event_info->is_using_stmt_cache());

    if (binlog_start_trans_and_stmt(thd, event_info))
      DBUG_RETURN(error);

    bool is_trans_cache= event_info->is_using_trans_cache();
    binlog_cache_mngr *cache_mngr= thd_get_cache_mngr(thd);
    binlog_cache_data *cache_data= cache_mngr->get_binlog_cache_data(is_trans_cache);

    DBUG_PRINT("info",("event type: %d",event_info->get_type_code()));

    /*
       No check for auto events flag here - this write method should
       never be called if auto-events are enabled.

       Write first log events which describe the 'run environment'
       of the SQL command. If row-based binlogging, Insert_id, Rand
       and other kind of "setting context" events are not needed.
    */
    if (thd)
    {
      if (!thd->is_current_stmt_binlog_format_row())
      {
        if (thd->stmt_depends_on_first_successful_insert_id_in_prev_stmt)
        {
          Intvar_log_event e(thd,(uchar) LAST_INSERT_ID_EVENT,
                             thd->first_successful_insert_id_in_prev_stmt_for_binlog,
                             event_info->event_cache_type, event_info->event_logging_type);
          if (cache_data->write_event(thd, &e))
            goto err;
        }
        if (thd->auto_inc_intervals_in_cur_stmt_for_binlog.nb_elements() > 0)
        {
          DBUG_PRINT("info",("number of auto_inc intervals: %u",
                             thd->auto_inc_intervals_in_cur_stmt_for_binlog.
                             nb_elements()));
          Intvar_log_event e(thd, (uchar) INSERT_ID_EVENT,
                             thd->auto_inc_intervals_in_cur_stmt_for_binlog.
                             minimum(), event_info->event_cache_type,
                             event_info->event_logging_type);
          if (cache_data->write_event(thd, &e))
            goto err;
        }
        if (thd->rand_used)
        {
          Rand_log_event e(thd,thd->rand_saved_seed1,thd->rand_saved_seed2,
                           event_info->event_cache_type,
                           event_info->event_logging_type);
          if (cache_data->write_event(thd, &e))
            goto err;
        }
        if (thd->user_var_events.elements)
        {
          for (uint i= 0; i < thd->user_var_events.elements; i++)
          {
            BINLOG_USER_VAR_EVENT *user_var_event;
            get_dynamic(&thd->user_var_events,(uchar*) &user_var_event, i);

            /* setting flags for user var log event */
            uchar flags= User_var_log_event::UNDEF_F;
            if (user_var_event->unsigned_flag)
              flags|= User_var_log_event::UNSIGNED_F;

            User_var_log_event e(thd,
                                 user_var_event->user_var_event->entry_name.ptr(),
                                 user_var_event->user_var_event->entry_name.length(),
                                 user_var_event->value,
                                 user_var_event->length,
                                 user_var_event->type,
                                 user_var_event->charset_number, flags,
                                 event_info->event_cache_type,
                                 event_info->event_logging_type);
            if (cache_data->write_event(thd, &e))
              goto err;
          }
        }
      }
    }

    /*
      Write the event.
    */
    if (cache_data->write_event(thd, event_info, write_meta_data_event) ||
        DBUG_EVALUATE_IF("injecting_fault_writing", true, false))
      goto err;

    /*
      After writing the event, if the trx-cache was used and any unsafe
      change was written into it, the cache is marked as cannot safely
      roll back.
    */
    if (is_trans_cache && stmt_cannot_safely_rollback(thd))
      cache_mngr->trx_cache.set_cannot_rollback();

    error= 0;

err:
    if (error)
    {
      set_write_error(thd, is_trans_cache);
      if (check_write_error(thd) && cache_data &&
          stmt_cannot_safely_rollback(thd))
        cache_data->set_incident();
    }
  }

  DBUG_RETURN(error);
}

/**
  The method executes rotation when LOCK_log is already acquired
  by the caller.

  @param force_rotate  caller can request the log rotation
  @param check_purge   is set to true if rotation took place

  @note
    If rotation fails, for instance the server was unable
    to create a new log file, we still try to write an
    incident event to the current log.

  @note The caller must hold LOCK_log when invoking this function.

  @retval
    nonzero - error in rotating routine.
*/
int MYSQL_BIN_LOG::rotate(bool force_rotate, bool* check_purge)
{
  int error= 0;
  DBUG_ENTER("MYSQL_BIN_LOG::rotate");

  DBUG_ASSERT(!is_relay_log);
  mysql_mutex_assert_owner(&LOCK_log);

  *check_purge= false;

  if (DBUG_EVALUATE_IF("force_rotate", 1, 0) || force_rotate ||
      (my_b_tell(&log_file) >= (my_off_t) max_size))
  {
    error= new_file_without_locking(NULL);
    *check_purge= true;
  }
  DBUG_RETURN(error);
}

/**
  The method executes logs purging routine.

  @retval
    nonzero - error in rotating routine.
*/
void MYSQL_BIN_LOG::purge()
{
#ifdef HAVE_REPLICATION
  if (expire_logs_days || binlog_expire_logs_seconds)
  {
    DEBUG_SYNC(current_thd, "at_purge_logs_before_date");
    time_t purge_time= my_time(0) - expire_logs_days * 24 * 60 * 60 -
                       binlog_expire_logs_seconds;
    DBUG_EXECUTE_IF("expire_logs_always",
                    { purge_time= my_time(0);});
    if (purge_time >= 0)
    {
      /*
        Flush logs for storage engines, so that the last transaction
        is fsynced inside storage engines.
      */
      ha_flush_logs(NULL);
      purge_logs_before_date(purge_time, true);
    }
  }

  // Auto purge apply logs based on retention parameters
  if (is_apply_log)
    purge_apply_logs();
#endif
}

/**
  The method is a shortcut of @c rotate() and @c purge().
  LOCK_log is acquired prior to rotate and is released after it.

  @param force_rotate  caller can request the log rotation

  @retval
    nonzero - error in rotating routine.
*/
int MYSQL_BIN_LOG::rotate_and_purge(THD* thd, bool force_rotate)
{
  int error= 0;
  DBUG_ENTER("MYSQL_BIN_LOG::rotate_and_purge");
  bool check_purge= false;

  /*
    Wait for handlerton to insert any pending information into the binlog.
    For e.g. ha_ndbcluster which updates the binlog asynchronously this is
    needed so that the user see its own commands in the binlog.
  */
  ha_binlog_wait(thd);

  DBUG_ASSERT(!is_relay_log);
  mysql_mutex_lock(&LOCK_log);
  error= rotate(force_rotate, &check_purge);
  /*
    NOTE: Run purge_logs wo/ holding LOCK_log because it does not need
          the mutex. Otherwise causes various deadlocks.
  */
  mysql_mutex_unlock(&LOCK_log);

  if (!error && check_purge)
    purge();

  DBUG_RETURN(error);
}

int rotate_binlog_file(THD *thd)
{
  int error= 0;
  DBUG_ENTER("rotate_binlog_file");
  if (mysql_bin_log.is_open())
  {
    error= mysql_bin_log.rotate_and_purge(thd, true);
  }
  DBUG_RETURN(error);
}

int MYSQL_BIN_LOG::config_change_rotate(THD* thd,
                                        std::string config_change)
{
  int error= 0;
  DBUG_ENTER("MYSQL_BIN_LOG::config_change_rotate");

  RaftRotateInfo raft_rotate_info;
  raft_rotate_info.config_change= std::move(config_change);
  raft_rotate_info.config_change_rotate= true;
  // config change can only be initiated on master/mysql_bin_log
  DBUG_ASSERT(!is_relay_log);
  error= new_file_impl(true /*need lock log*/, nullptr, &raft_rotate_info);

  DBUG_RETURN(error);
}

int raft_config_change(THD *thd, std::string config_change)
{
  int error= 0;
  DBUG_ENTER("raft_config_change");
  if (mysql_bin_log.is_open())
  {
    error= mysql_bin_log.config_change_rotate(thd, std::move(config_change));
  }
  else
  {
    // TODO - add throttled messaging here if present in 8.0
    error= 1;
  }
  DBUG_RETURN(error);
}

int handle_dump_threads(bool block)
{
  DBUG_ENTER("handle_dump_threads");
#ifdef HAVE_REPLICATION
  if (block)
    block_all_dump_threads();
  else
    unblock_all_dump_threads();
#endif
  DBUG_RETURN(0);
}

int binlog_change_to_apply()
{
  DBUG_ENTER("binlog_change_to_apply");

  if (disable_raft_log_repointing)
  {
    mysql_bin_log.is_apply_log = true;
    DBUG_RETURN(0);
  }

  int error= 0;
  LOG_INFO linfo;

  mysql_mutex_lock(mysql_bin_log.get_log_lock());
  dump_log.lock();
  mysql_bin_log.lock_index();
  mysql_bin_log.lock_binlog_end_pos();

  mysql_bin_log.close(LOG_CLOSE_INDEX);

  if (mysql_bin_log.open_index_file(opt_applylog_index_name,
                                    opt_apply_logname, false/*need_lock_index=false*/))
  {
    error= 1;
    goto err;
  }

  mysql_bin_log.is_apply_log= true;

  /*
    Configures what object is used by the current log to store processed
    gtid(s). This is necessary in the MYSQL_BIN_LOG::MYSQL_BIN_LOG to
    corretly compute the set of previous gtids.
  */
  mysql_bin_log.set_previous_gtid_set(
    const_cast<Gtid_set*>(gtid_state->get_logged_gtids()));

  // HLC is TBD
  if (mysql_bin_log.open_binlog(opt_apply_logname,
                                nullptr,
                                WRITE_CACHE,
                                max_binlog_size,
                                false,
                                false /*need_lock_index=false*/,
                                true /*need_sid_lock=true*/,
                                nullptr /*extra_description_event*/,
                                nullptr /*raft_rotate_info*/,
                                false /*need_end_lock_pos_lock*/))
  {
    error= 1;
    goto err;
  }

  dump_log.switch_log(/* relay_log= */true, /* should_lock= */false);

  // Purge all apply logs before the last log, because they
  // are from the previous epoch of being a FOLLOWER, and they
  // don't have proper Rotate events at the end.
  mysql_bin_log.raw_get_current_log(&linfo);

#ifndef EMBEDDED_LIBRARY
  if (mysql_bin_log.purge_logs(linfo.log_file_name,
                           false /* included */,
                           false /*need_lock_index=false*/,
                           true /*need_update_threads=true*/,
                           NULL /* decrease space */,
                           true /* auto purge */))
  {
    error= 1;
    goto err;
  }
#endif

  if (mysql_bin_log.init_prev_gtid_sets_map())
  {
    error= 1;
    goto err;
  }

err:

  mysql_bin_log.unlock_binlog_end_pos();
  mysql_bin_log.unlock_index();
  dump_log.unlock();
  mysql_mutex_unlock(mysql_bin_log.get_log_lock());

  DBUG_RETURN(error);
}

int binlog_change_to_binlog()
{
  DBUG_ENTER("binlog_change_to_binlog");

  if (disable_raft_log_repointing)
  {
    mysql_bin_log.is_apply_log = false;
    DBUG_RETURN(0);
  }

  int error= 0;
  uint64_t prev_hlc= 0;

 // Flush logs to ensure that storage engine has flushed and fsynced the last
 // batch of transactions. This is important because the act of switching trx
 // logs from "apply-logs-*" to "binary-logs-*" looks like a rotation to other
 // parts of the system and rotation is always a 'sync' point
 ha_flush_logs(NULL);

  mysql_mutex_lock(mysql_bin_log.get_log_lock());
  dump_log.lock();
  mysql_bin_log.lock_index();
#ifdef HAVE_REPLICATION
  active_mi->rli->relay_log.lock_binlog_end_pos();
#endif // HAVE_REPLICATION

  // Get the index file name
  std::string indexfn= mysql_bin_log.get_index_fname();

  std::pair<std::vector<std::string>, int> result;
  bool delete_apply_logs= false;
  if (indexfn.find(opt_applylog_index_name) != std::string::npos)
  {
    // This is a apply-binlog index file. Get a list of apply-binlog names from
    // the index file
    result= mysql_bin_log.get_lognames_from_index(false);
    if (result.second)
    {
      // NO_LINT_DEBUG
      sql_print_error(
          "Failed to get apply binlog filenames from the index file");
      error= 1;
      goto err;
    }
    delete_apply_logs= true;
  }

  mysql_bin_log.close(LOG_CLOSE_INDEX);

  if (mysql_bin_log.open_index_file(
        opt_binlog_index_name, opt_bin_logname, false/*need_lock_index=false*/))
  {
    error= 1;
    goto err;
  }

  global_sid_lock->wrlock();
  if (mysql_bin_log.init_gtid_sets(
       const_cast<Gtid_set *>(gtid_state->get_logged_gtids()),
       const_cast<Gtid_set *>(gtid_state->get_lost_gtids()),
       NULL/*last_gtid*/,
       opt_master_verify_checksum,
       false,
       &prev_hlc))
  {
    global_sid_lock->unlock();
    error= 1;
    goto err;
  }
  global_sid_lock->unlock();

  /*
    Configures what object is used by the current log to store processed
    gtid(s). This is necessary in the MYSQL_BIN_LOG::rotate to
    correctly compute the set of previous gtids.
  */
  mysql_bin_log.set_previous_gtid_set(
    const_cast<Gtid_set*>(gtid_state->get_logged_gtids()));

  // Update the instance's HLC clock to be greater than or equal to the HLC
  // times of trx's in all previous binlog
  mysql_bin_log.update_hlc(prev_hlc);

  if (mysql_bin_log.open_existing_binlog(opt_bin_logname,
                                         WRITE_CACHE,
                                         max_binlog_size,
                                         /* need_end_log_pos_lock= */ false))
  {
    error= 1;
    goto err;
  }

  if (!delete_apply_logs)
    goto err;

  // 1. Now delete all the apply binlogs
  // 2. Once the apply binlogs are deleted, then proceed to delete the apply
  // binlog index file
  // TODO: This needs better safety - if mysqld crashes between 1 and 2, it will
  // not be able to startup without manual intervention
  for (const auto& name : result.first)
  {
    if (my_delete(name.c_str(), MYF(MY_WME))) // #1
    {
      // NO_LINT_DEBUG
      sql_print_error("Could not delete the apply binlog file %s",
          name.c_str());
      error= 1;
      goto err;
    }
  }

  // NO_LINT_DEBUG
  sql_print_information("Deleting the apply index file %s", indexfn.c_str());
  if (my_delete(indexfn.c_str(), MYF(MY_WME))) // #2
  {
    // NO_LINT_DEBUG
    sql_print_error("Failed to delete apply index file %s", indexfn.c_str());
    error= 1;
    goto err;
  }

  // unset apply log, so that masters
  // ordered_commit understands this
  mysql_bin_log.is_apply_log = false;
  mysql_bin_log.apply_file_count.store(0);

  dump_log.switch_log(/* relay_log= */false, /* should_lock= */false);

err:

#ifdef HAVE_REPLICATION
  active_mi->rli->relay_log.unlock_binlog_end_pos();
#endif // HAVE_REPLICATION
  mysql_bin_log.unlock_index();
  dump_log.unlock();
  mysql_mutex_unlock(mysql_bin_log.get_log_lock());

  DBUG_EXECUTE_IF("crash_after_point_binlog_to_binlog", DBUG_SUICIDE(););

  DBUG_RETURN(error);
}

std::pair<std::vector<std::string>, int>
MYSQL_BIN_LOG::get_lognames_from_index(bool need_lock)
{
  LOG_INFO log_info;
  std::vector<std::string> lognames;
  int error= 0;

  if (need_lock)
    mysql_mutex_lock(&LOCK_index);

  if ((error= find_log_pos(&log_info, NullS, false/*need_lock_index=false*/)))
    goto err;

  while (true)
  {
    lognames.emplace_back(log_info.log_file_name);

    int ret= find_next_log(&log_info, false/*need_lock_index=false*/);
    if (ret == LOG_INFO_EOF) {
      break;
    } else if (ret == LOG_INFO_IO) {
      // NO_LINT_DEBUG
      sql_print_error("Could not read from log index file ");
      error= 1;
      goto err;
    }
  }

err:
  if (need_lock)
    mysql_mutex_unlock(&LOCK_index);

  return std::make_pair(std::move(lognames), error);
}

uint MYSQL_BIN_LOG::next_file_id()
{
  uint res;
  mysql_mutex_lock(&LOCK_log);
  res = file_id++;
  mysql_mutex_unlock(&LOCK_log);
  return res;
}

extern "C"
my_bool mysql_bin_log_is_open(void)
{
  return mysql_bin_log.is_open();
}

extern "C"
void mysql_bin_log_lock_commits(struct snapshot_info_st *ss_info)
{
  mysql_bin_log.lock_commits(ss_info);
}

extern "C"
void mysql_bin_log_unlock_commits(const struct snapshot_info_st *ss_info)
{
  mysql_bin_log.unlock_commits(ss_info);
}

void MYSQL_BIN_LOG::lock_commits(snapshot_info_st *ss_info)
{
  mysql_mutex_lock(&LOCK_log);
  mysql_mutex_lock(&LOCK_sync);
  mysql_mutex_lock(&LOCK_semisync);
  mysql_mutex_lock(&LOCK_commit);
  ss_info->binlog_file= std::string(log_file_name);
  ss_info->binlog_pos = my_b_tell(&log_file);
  global_sid_lock->wrlock();
  const auto gtids= gtid_state->get_logged_gtids()->to_string();
  ss_info->gtid_executed = std::string(gtids);
  my_free(gtids);

  // If HLC is enabled, then set the current HLC timestamp
  if (enable_binlog_hlc)
    ss_info->snapshot_hlc = get_current_hlc();

  global_sid_lock->unlock();
}

void MYSQL_BIN_LOG::unlock_commits(const snapshot_info_st *ss_info)
{
  global_sid_lock->wrlock();
  const auto gtids= gtid_state->get_logged_gtids()->to_string();
  assert(ss_info->binlog_file == std::string(log_file_name) &&
         ss_info->binlog_pos == my_b_tell(&log_file) &&
         ss_info->gtid_executed == std::string(gtids));
  my_free(gtids);
  global_sid_lock->unlock();
  mysql_mutex_unlock(&LOCK_commit);
  mysql_mutex_unlock(&LOCK_semisync);
  mysql_mutex_unlock(&LOCK_sync);
  mysql_mutex_unlock(&LOCK_log);
}

/**
  Calculate checksum of possibly a part of an event containing at least
  the whole common header.

  @param    buf       the pointer to trans cache's buffer
  @param    off       the offset of the beginning of the event in the buffer
  @param    event_len no-checksum length of the event
  @param    length    the current size of the buffer

  @param    crc       [in-out] the checksum

  Event size in incremented by @c BINLOG_CHECKSUM_LEN.

  @return 0 or number of unprocessed yet bytes of the event excluding
            the checksum part.
*/
  static ulong fix_log_event_crc(uchar *buf, uint off, uint event_len,
                                 uint length, ha_checksum *crc)
{
  ulong ret;
  uchar *event_begin= buf + off;
  uint16 flags= uint2korr(event_begin + FLAGS_OFFSET);

  DBUG_ASSERT(length >= off + LOG_EVENT_HEADER_LEN); //at least common header in
  int2store(event_begin + FLAGS_OFFSET, flags);
  ret= length >= off + event_len ? 0 : off + event_len - length;
  *crc= my_checksum(*crc, event_begin, event_len - ret);
  return ret;
}

/*
  Write the contents of a cache to the binary log.

  SYNOPSIS
    do_write_cache()
    cache    Cache to write to the binary log
    lock_log True if the LOCK_log mutex should be aquired, false otherwise

  DESCRIPTION
    Write the contents of the cache to the binary log. The cache will
    be reset as a READ_CACHE to be able to read the contents from it.

    Reading from the trans cache with possible (per @c binlog_checksum_options)
    adding checksum value  and then fixing the length and the end_log_pos of
    events prior to fill in the binlog cache.
*/

int MYSQL_BIN_LOG::do_write_cache(IO_CACHE *cache)
{
  DBUG_ENTER("MYSQL_BIN_LOG::do_write_cache(IO_CACHE *)");

  DBUG_EXECUTE_IF("simulate_do_write_cache_failure",
                  {
                    DBUG_SET("-d,simulate_do_write_cache_failure");
                    DBUG_RETURN(ER_ERROR_ON_WRITE);
                  });

  if (reinit_io_cache(cache, READ_CACHE, 0, 0, 0))
    DBUG_RETURN(ER_ERROR_ON_WRITE);
  uint length= my_b_bytes_in_cache(cache), group, carry, hdr_offs;
  ulong remains= 0; // part of unprocessed yet netto length of the event
  long val;
  ulong end_log_pos_inc= 0; // each event processed adds BINLOG_CHECKSUM_LEN 2 t
  uchar header[LOG_EVENT_HEADER_LEN];
  ha_checksum crc= 0, crc_0= 0; // assignments to keep compiler happy
  my_bool do_checksum= (binlog_checksum_options != BINLOG_CHECKSUM_ALG_OFF);
  uchar buf[BINLOG_CHECKSUM_LEN];

  // while there is just one alg the following must hold:
  DBUG_ASSERT(!do_checksum ||
              binlog_checksum_options == BINLOG_CHECKSUM_ALG_CRC32);

  /*
    The events in the buffer have incorrect end_log_pos data
    (relative to beginning of group rather than absolute),
    so we'll recalculate them in situ so the binlog is always
    correct, even in the middle of a group. This is possible
    because we now know the start position of the group (the
    offset of this cache in the log, if you will); all we need
    to do is to find all event-headers, and add the position of
    the group to the end_log_pos of each event.  This is pretty
    straight forward, except that we read the cache in segments,
    so an event-header might end up on the cache-border and get
    split.
  */

  group= (uint)my_b_tell(&log_file);
  DBUG_PRINT("debug", ("length: %llu, group: %llu",
                       (ulonglong) length, (ulonglong) group));
  hdr_offs= carry= 0;
  if (do_checksum)
    crc= crc_0= my_checksum(0L, NULL, 0);

  if (DBUG_EVALUATE_IF("fault_injection_crc_value", 1, 0))
    crc= crc - 1;

  do
  {
    /*
      if we only got a partial header in the last iteration,
      get the other half now and process a full header.
    */
    if (unlikely(carry > 0))
    {
      DBUG_ASSERT(carry < LOG_EVENT_HEADER_LEN);

      /* assemble both halves */
      memcpy(&header[carry], (char *)cache->read_pos,
             LOG_EVENT_HEADER_LEN - carry);

      /* fix end_log_pos */
      val=uint4korr(header + LOG_POS_OFFSET);
      val+= group +
        (end_log_pos_inc+= (do_checksum ? BINLOG_CHECKSUM_LEN : 0));
      int4store(&header[LOG_POS_OFFSET], val);

      if (do_checksum)
      {
        ulong len= uint4korr(header + EVENT_LEN_OFFSET);
        /* fix len */
        int4store(&header[EVENT_LEN_OFFSET], len + BINLOG_CHECKSUM_LEN);
      }

      /* write the first half of the split header */
      if (my_b_write(&log_file, header, carry))
        DBUG_RETURN(ER_ERROR_ON_WRITE);

      /*
        copy fixed second half of header to cache so the correct
        version will be written later.
      */
      memcpy((char *)cache->read_pos, &header[carry],
             LOG_EVENT_HEADER_LEN - carry);

      /* next event header at ... */
      hdr_offs= uint4korr(header + EVENT_LEN_OFFSET) - carry -
        (do_checksum ? BINLOG_CHECKSUM_LEN : 0);

      if (do_checksum)
      {
        DBUG_ASSERT(crc == crc_0 && remains == 0);
        crc= my_checksum(crc, header, carry);
        remains= uint4korr(header + EVENT_LEN_OFFSET) - carry -
          BINLOG_CHECKSUM_LEN;
      }
      carry= 0;
    }

    /* if there is anything to write, process it. */

    if (likely(length > 0))
    {
      /*
        process all event-headers in this (partial) cache.
        if next header is beyond current read-buffer,
        we'll get it later (though not necessarily in the
        very next iteration, just "eventually").
      */

      /* crc-calc the whole buffer */
      if (do_checksum && hdr_offs >= length)
      {

        DBUG_ASSERT(remains != 0 && crc != crc_0);

        crc= my_checksum(crc, cache->read_pos, length);
        remains -= length;
        if (my_b_write(&log_file, cache->read_pos, length))
          DBUG_RETURN(ER_ERROR_ON_WRITE);
        if (remains == 0)
        {
          int4store(buf, crc);
          if (my_b_write(&log_file, buf, BINLOG_CHECKSUM_LEN))
            DBUG_RETURN(ER_ERROR_ON_WRITE);
          crc= crc_0;
        }
      }

      while (hdr_offs < length)
      {
        /*
          partial header only? save what we can get, process once
          we get the rest.
        */

        if (do_checksum)
        {
          if (remains != 0)
          {
            /*
              finish off with remains of the last event that crawls
              from previous into the current buffer
            */
            DBUG_ASSERT(crc != crc_0);
            crc= my_checksum(crc, cache->read_pos, hdr_offs);
            int4store(buf, crc);
            remains -= hdr_offs;
            DBUG_ASSERT(remains == 0);
            if (my_b_write(&log_file, cache->read_pos, hdr_offs) ||
                my_b_write(&log_file, buf, BINLOG_CHECKSUM_LEN))
              DBUG_RETURN(ER_ERROR_ON_WRITE);
            crc= crc_0;
          }
        }

        if (hdr_offs + LOG_EVENT_HEADER_LEN > length)
        {
          carry= length - hdr_offs;
          memcpy(header, (char *)cache->read_pos + hdr_offs, carry);
          length= hdr_offs;
        }
        else
        {
          /* we've got a full event-header, and it came in one piece */
          uchar *ev= (uchar *)cache->read_pos + hdr_offs;
          uint event_len= uint4korr(ev + EVENT_LEN_OFFSET); // netto len
          uchar *log_pos= ev + LOG_POS_OFFSET;

          /* fix end_log_pos */
          val= uint4korr(log_pos) + group +
            (end_log_pos_inc += (do_checksum ? BINLOG_CHECKSUM_LEN : 0));
          int4store(log_pos, val);

	  /* fix CRC */
	  if (do_checksum)
          {
            /* fix length */
            int4store(ev + EVENT_LEN_OFFSET, event_len + BINLOG_CHECKSUM_LEN);
            remains= fix_log_event_crc(cache->read_pos, hdr_offs, event_len,
                                       length, &crc);
            if (my_b_write(&log_file, ev,
                           remains == 0 ? event_len : length - hdr_offs))
              DBUG_RETURN(ER_ERROR_ON_WRITE);
            if (remains == 0)
            {
              int4store(buf, crc);
              if (my_b_write(&log_file, buf, BINLOG_CHECKSUM_LEN))
                DBUG_RETURN(ER_ERROR_ON_WRITE);
              crc= crc_0; // crc is complete
            }
          }

          /* next event header at ... */
          hdr_offs += event_len; // incr by the netto len

          DBUG_ASSERT(!do_checksum || remains == 0 || hdr_offs >= length);
        }
      }

      /*
        Adjust hdr_offs. Note that it may still point beyond the segment
        read in the next iteration; if the current event is very long,
        it may take a couple of read-iterations (and subsequent adjustments
        of hdr_offs) for it to point into the then-current segment.
        If we have a split header (!carry), hdr_offs will be set at the
        beginning of the next iteration, overwriting the value we set here:
      */
      hdr_offs -= length;
    }

    /* Write the entire buf to the binary log file */
    if (!do_checksum)
      if (my_b_write(&log_file, cache->read_pos, length))
        DBUG_RETURN(ER_ERROR_ON_WRITE);
    cache->read_pos=cache->read_end;		// Mark buffer used up
  } while ((length= my_b_fill(cache)));

  DBUG_ASSERT(carry == 0);
  DBUG_ASSERT(!do_checksum || remains == 0);
  DBUG_ASSERT(!do_checksum || crc == crc_0);

  DBUG_RETURN(0); // All OK
}

/**
  Writes an incident event to the binary log.

  @param ev Incident event to be written
  @param need_lock_log If true, will acquire LOCK_log; otherwise the
  caller should already have acquired LOCK_log.
  @do_flush_and_sync If true, will call flush_and_sync(), rotate() and
  purge().

  @retval false error
  @retval true success
*/
bool MYSQL_BIN_LOG::write_incident(Incident_log_event *ev, bool need_lock_log,
                                   bool do_flush_and_sync)
{
  uint error= 0;
  THD *thd = ev->thd;
  USER_STATS *us= thd ? thd_get_user_stats(thd) : NULL;
  DBUG_ENTER("MYSQL_BIN_LOG::write_incident");

  if (!is_open())
    DBUG_RETURN(error);

  if (need_lock_log)
    mysql_mutex_lock(&LOCK_log);
  else
    mysql_mutex_assert_owner(&LOCK_log);

  // @todo make this work with the group log. /sven

  error= ev->write(&log_file);
  if (us)
  {
    us->binlog_bytes_written.inc(ev->data_written);
  }
  binlog_bytes_written += ev->data_written;

  if (do_flush_and_sync)
  {
    if (!error && !(error= flush_and_sync(false, false)))
    {
      bool check_purge= false;
      update_binlog_end_pos();
      error= rotate(true, &check_purge);
      if (!error && check_purge)
        purge();
    }
  }

  if (need_lock_log)
    mysql_mutex_unlock(&LOCK_log);

  DBUG_RETURN(error);
}
/**
  Creates an incident event and writes it to the binary log.

  @param thd  Thread variable
  @param ev   Incident event to be written
  @param lock If the binary lock should be locked or not

  @retval
    0    error
  @retval
    1    success
*/
bool MYSQL_BIN_LOG::write_incident(THD *thd, bool need_lock_log,
                                   bool do_flush_and_sync)
{
  DBUG_ENTER("MYSQL_BIN_LOG::write_incident");

  if (!is_open())
    DBUG_RETURN(0);

  LEX_STRING const write_error_msg=
    { C_STRING_WITH_LEN("error writing to the binary log") };
  Incident incident= INCIDENT_LOST_EVENTS;
  Incident_log_event ev(thd, incident, write_error_msg);

  DBUG_RETURN(write_incident(&ev, need_lock_log, do_flush_and_sync));
}

void MYSQL_BIN_LOG::handle_write_error(THD *thd, binlog_cache_data *cache_data)
{
  DBUG_ENTER("MYSQL_BIN_LOG::handle_write_error(THD *, binlog_cache_data *)");

  IO_CACHE *cache= &cache_data->cache_log;
  if (!write_error)
  {
    char errbuf[MYSYS_STRERROR_SIZE];
    write_error= 1;
    // NO_LINT_DEBUG
    sql_print_error(
        ER(ER_ERROR_ON_WRITE),
        name,
        errno,
        my_strerror(errbuf, sizeof(errbuf), errno));
  }

  /* TODO: Verify how this works once raft plugin's log faking is working.
   * If the flush has failed due to ENOSPC, set the flush_error flag.
   */
  if (cache->error && thd->is_error() && my_errno == ENOSPC)
  {
    cache_data->set_flush_error(thd);
  }

  thd->commit_error= THD::CE_FLUSH_ERROR;
  if (binlog_error_action != IGNORE_ERROR)
  {
    set_write_error(thd, cache_data->is_trx_cache());
  }

  /* Remove gtid from logged_gtid set if error happened. */
  if (write_error && thd->gtid_precommit)
  {
    global_sid_lock->rdlock();
    gtid_state->remove_gtid_on_failure(thd);
    global_sid_lock->unlock();
  }

  DBUG_VOID_RETURN;
}

bool MYSQL_BIN_LOG::post_write(
    THD *thd, binlog_cache_data *cache_data, int error)
{
  DBUG_ENTER("MYSQL_BIN_LOG::post_write(THD *, binlog_cache_data *, int)");

  USER_STATS *us= thd ? thd_get_user_stats(thd) : NULL;
  IO_CACHE *cache= &cache_data->cache_log;

  mysql_mutex_assert_owner(&LOCK_log);

  DBUG_ASSERT(is_open());

  if (unlikely(!is_open()))
  {
    DBUG_RETURN(false);
  }

  // TODO: Ensure that binlog cache is still valid after plugin returns
  size_t cache_size= my_b_tell(cache);
  if (cache_size == 0)
  {
    DBUG_RETURN(false);
  }

  // TODO: write_error is supposed to be set when wrting to binlog. Ensure
  // log faking does this correctly
  if (write_error || error)
  {
    handle_write_error(thd, cache_data);
    DBUG_RETURN(true);
  }

  if (us)
  {
    us->binlog_bytes_written.inc(cache_size);
  }

  binlog_bytes_written += cache_size;

  if (cache->error)
  {
    char errbuf[MYSYS_STRERROR_SIZE];
    // NO_LINT_DEBUG
    sql_print_error(
        ER(ER_ERROR_ON_READ),
        cache->file_name,
        errno,
        my_strerror(errbuf, sizeof(errbuf), errno));

    write_error= 1;
    handle_write_error(thd, cache_data);
    DBUG_RETURN(true);
  }

  if (!thd->gtid_precommit)
  {
    global_sid_lock->rdlock();
    if (gtid_state->update_on_flush(thd) != RETURN_STATUS_OK)
    {
      global_sid_lock->unlock();
      handle_write_error(thd, cache_data);
      DBUG_RETURN(true);
    }

    global_sid_lock->unlock();
  }

  // Update slave's gtid_next session variable
  if (thd->rli_slave)
  {
      thd->variables.gtid_next.set_automatic();
  }

  update_thd_next_event_pos(thd);

  DBUG_RETURN(false);
}

/**
  Write a cached log entry to the binary log.

  @param thd            Thread variable
  @param cache		The cache to copy to the binlog
  @param incident       Defines if an incident event should be created to
                        notify that some non-transactional changes did
                        not get into the binlog.
  @param prepared       Defines if a transaction is part of a 2-PC.

  @note
    We only come here if there is something in the cache.
  @note
    The thing in the cache is always a complete transaction.
  @note
    'cache' needs to be reinitialized after this functions returns.
*/

bool MYSQL_BIN_LOG::write_cache(THD *thd, binlog_cache_data *cache_data,
                                bool async)
{
  DBUG_ENTER("MYSQL_BIN_LOG::write_cache(THD *, binlog_cache_data *, bool)");

  USER_STATS *us= thd ? thd_get_user_stats(thd) : NULL;
  IO_CACHE *cache= &cache_data->cache_log;
  bool incident= cache_data->has_incident();

  DBUG_EXECUTE_IF("simulate_binlog_flush_error",
                  {
                    if (rand() % 3 == 0)
                    {
                      write_error=1;
                      thd->commit_error= THD::CE_FLUSH_ERROR;
                      DBUG_RETURN(0);
                    }
                  };);

  mysql_mutex_assert_owner(&LOCK_log);

  DBUG_ASSERT(is_open());
  if (likely(is_open()))                       // Should always be true
  {
    /*
      We only bother to write to the binary log if there is anything
      to write.
     */
    if (my_b_tell(cache) > 0)
    {
      DBUG_EXECUTE_IF("crash_before_writing_xid",
                      {
                        if ((write_error= do_write_cache(cache)))
                          DBUG_PRINT("info", ("error writing binlog cache: %d",
                                               write_error));
                        flush_and_sync(async, true);
                        DBUG_PRINT("info", ("crashing before writing xid"));
                        DBUG_SUICIDE();
                      });

      DBUG_EXECUTE_IF("fail_binlog_flush_raft",
          {
            write_error= 1;
            thd->commit_error= THD::CE_FLUSH_ERROR;
            thd->commit_consensus_error= true;
            goto err;
          });

      if ((write_error= do_write_cache(cache)))
        goto err;
      if (us)
      {
        us->binlog_bytes_written.inc(my_b_tell(cache));
      }
      binlog_bytes_written += my_b_tell(cache);

      if (incident && write_incident(thd, false/*need_lock_log=false*/,
                                     false/*do_flush_and_sync==false*/))
        goto err;

      DBUG_EXECUTE_IF("half_binlogged_transaction", DBUG_SUICIDE(););
      if (cache->error)				// Error on read
      {
        char errbuf[MYSYS_STRERROR_SIZE];
        sql_print_error(ER(ER_ERROR_ON_READ), cache->file_name,
                        errno, my_strerror(errbuf, sizeof(errbuf), errno));
        write_error=1;				// Don't give more errors
        goto err;
      }

      if (!thd->gtid_precommit)
      {
        global_sid_lock->rdlock();
        if (gtid_state->update_on_flush(thd) != RETURN_STATUS_OK)
        {
          global_sid_lock->unlock();
          goto err;
        }
        global_sid_lock->unlock();
      }
      if (thd->rli_slave)
      {
        /*
          Using gtid_next='AUTOMATIC' on a slave is not a concern because it
          doesn't generate GTIDs because of read_only setting. Setting
          gtid_next='automatic' after each events avoids hitting
          ER_GTID_NEXT_TYPE_UNDEFINED_GROUP on sql_thread when slave receives a
          transaction without GTID_NEXT set.
        */
        thd->variables.gtid_next.set_automatic();
      }
    }
    update_thd_next_event_pos(thd);
  }

  DBUG_RETURN(0);

err:
  if (!write_error)
  {
    char errbuf[MYSYS_STRERROR_SIZE];
    write_error= 1;
    sql_print_error(ER(ER_ERROR_ON_WRITE), name,
                    errno, my_strerror(errbuf, sizeof(errbuf), errno));
  }

  /*
    If the flush has failed due to ENOSPC, set the flush_error flag.
  */
  if (cache->error && thd->is_error() && my_errno == ENOSPC)
  {
    cache_data->set_flush_error(thd);
  }
  thd->commit_error= THD::CE_FLUSH_ERROR;
  if (binlog_error_action != IGNORE_ERROR)
  {
    set_write_error(thd, cache_data->is_trx_cache());
  }

  /* Remove gtid from logged_gtid set if error happened. */
  if (write_error && thd->gtid_precommit)
  {
    global_sid_lock->rdlock();
    gtid_state->remove_gtid_on_failure(thd);
    global_sid_lock->unlock();
  }
  DBUG_RETURN(1);
}


/**
  Wait until we get a signal that the relay log has been updated.

  @param[in] thd        Thread variable
  @param[in] timeout    a pointer to a timespec;
                        NULL means to wait w/o timeout.

  @retval    0          if got signalled on update
  @retval    non-0      if wait timeout elapsed

  @note
    One must have a lock on LOCK_log before calling this function.
*/

int MYSQL_BIN_LOG::wait_for_update_relay_log(THD* thd, const struct timespec *timeout)
{
  int ret= 0;
  PSI_stage_info old_stage;
  DBUG_ENTER("wait_for_update_relay_log");

  thd->ENTER_COND(&update_cond, &LOCK_log,
                  &stage_slave_has_read_all_relay_log,
                  &old_stage);

  if (!timeout)
    mysql_cond_wait(&update_cond, &LOCK_log);
  else
    ret= mysql_cond_timedwait(&update_cond, &LOCK_log,
                              const_cast<struct timespec *>(timeout));
  thd->EXIT_COND(&old_stage);

  DBUG_RETURN(ret);
}

/**
  Wait until we get a signal that the binary log has been updated.
  Applies to master only.

  NOTES
  @param[in] thd        a THD struct
  @param[in] timeout    a pointer to a timespec;
                        NULL means to wait w/o timeout.
  @retval    0          if got signalled on update
  @retval    non-0      if wait timeout elapsed
  @note
    LOCK_binlog_end_pos must be taken before calling this function.
    LOCK_binlog_end_pos is being released while the thread is waiting.
    LOCK_binlog_end_pos is released by the caller.
*/

int MYSQL_BIN_LOG::wait_for_update_bin_log(THD* thd,
                                           const struct timespec *timeout)
{
  int ret= 0;
  DBUG_ENTER("wait_for_update_bin_log");

  if (!timeout)
    mysql_cond_wait(&update_cond, &LOCK_binlog_end_pos);
  else
    ret= mysql_cond_timedwait(&update_cond, &LOCK_binlog_end_pos,
                              const_cast<struct timespec *>(timeout));
  DBUG_RETURN(ret);
}


/**
  Close the log file.

  @param exiting     Bitmask for one or more of the following bits:
          - LOG_CLOSE_INDEX : if we should close the index file
          - LOG_CLOSE_TO_BE_OPENED : if we intend to call open
                                     at once after close.
          - LOG_CLOSE_STOP_EVENT : write a 'stop' event to the log

  @note
    One can do an open on the object at once after doing a close.
    The internal structures are not freed until cleanup() is called
*/

void MYSQL_BIN_LOG::close(uint exiting)
{					// One can't set log_type here!
  DBUG_ENTER("MYSQL_BIN_LOG::close");
  DBUG_PRINT("enter",("exiting: %d", (int) exiting));
  if (log_state == LOG_OPENED)
  {
#ifdef HAVE_REPLICATION
    // In raft mode, we are disabling all STOP_EVENT addition.
    // There are primarily 3 reasons.
    // 1. T65968945 - during shutdown of the server,
    // relay logs and binlogs add STOP EVENTS. In Raft, the
    // Master still has an RLI which holds onto a STALE relay log
    // and adding a STOP EVENT during close will violate the append
    // only rule to the file, because the same relay log has been
    // usurped as a binlog by the master and appended to, changing its size
    // 2. Comments in code have shown us that STOP EVENTs are not
    // critical for relay logs and best effort in general.
    // A server can crash with kill -9 and there wont be any stop event.
    // Raft recovery code handles these scenarios and so STOP_EVENT is
    // still best effort
    // 3. In Raft on crash recovery we open_existing_binlog, which
    // will have issues because the STOP_EVENT will be in the middle of
    // the file and can confuse appliers, when we have to still keep
    // appending new entries beyond it.
    if (((exiting & LOG_CLOSE_STOP_EVENT) != 0) && !enable_raft_plugin)
    {
      Stop_log_event s;
      // the checksumming rule for relay-log case is similar to Rotate
        s.checksum_alg= is_relay_log ?
          relay_log_checksum_alg : binlog_checksum_options;
      DBUG_ASSERT(!is_relay_log ||
                  relay_log_checksum_alg != BINLOG_CHECKSUM_ALG_UNDEF);
      s.write(&log_file);
      bytes_written+= s.data_written;
      update_binlog_end_pos();
    }
#endif /* HAVE_REPLICATION */

    /* don't pwrite in a file opened with O_APPEND - it doesn't work */
    if (log_file.type == WRITE_CACHE)
    {
      my_off_t offset= BIN_LOG_HEADER_SIZE + FLAGS_OFFSET;
      my_off_t org_position= mysql_file_tell(log_file.file, MYF(0));
      uchar flags= 0;            // clearing LOG_EVENT_BINLOG_IN_USE_F
      mysql_file_pwrite(log_file.file, &flags, 1, offset, MYF(0));
      /*
        Restore position so that anything we have in the IO_cache is written
        to the correct position.
        We need the seek here, as mysql_file_pwrite() is not guaranteed to keep the
        original position on system that doesn't support pwrite().
      */
      mysql_file_seek(log_file.file, org_position, MY_SEEK_SET, MYF(0));
    }

    /* this will cleanup IO_CACHE, sync and close the file */
    MYSQL_LOG::close(exiting);
  }

  /*
    The following test is needed even if is_open() is not set, as we may have
    called a not complete close earlier and the index file is still open.
  */

  if ((exiting & LOG_CLOSE_INDEX) && my_b_inited(&index_file))
  {
    end_io_cache(&index_file);
    if (mysql_file_close(index_file.file, MYF(0)) < 0 && ! write_error)
    {
      char errbuf[MYSYS_STRERROR_SIZE];
      write_error= 1;
      sql_print_error(ER(ER_ERROR_ON_WRITE), index_file_name,
                      errno, my_strerror(errbuf, sizeof(errbuf), errno));
    }
  }
  log_state= (exiting & LOG_CLOSE_TO_BE_OPENED) ? LOG_TO_BE_OPENED : LOG_CLOSED;
  my_free(name);
  name= NULL;
  DBUG_VOID_RETURN;
}

void MYSQL_BIN_LOG::harvest_bytes_written(Relay_log_info* rli, bool need_log_space_lock)
{
#ifndef DBUG_OFF
  char buf1[22],buf2[22];
#endif
  DBUG_ENTER("harvest_bytes_written");
  if (need_log_space_lock)
    mysql_mutex_lock(&rli->log_space_lock);
  else
    mysql_mutex_assert_owner(&rli->log_space_lock);
  rli->log_space_total+= bytes_written;
  DBUG_PRINT("info",("relay_log_space: %s  bytes_written: %s",
        llstr(rli->log_space_total,buf1), llstr(bytes_written,buf2)));
  bytes_written=0;
  if (need_log_space_lock)
    mysql_mutex_unlock(&rli->log_space_lock);
  DBUG_VOID_RETURN;
}

void MYSQL_BIN_LOG::set_max_size(ulong max_size_arg)
{
  /*
    We need to take locks, otherwise this may happen:
    new_file() is called, calls open(old_max_size), then before open() starts,
    set_max_size() sets max_size to max_size_arg, then open() starts and
    uses the old_max_size argument, so max_size_arg has been overwritten and
    it's like if the SET command was never run.
  */
  DBUG_ENTER("MYSQL_BIN_LOG::set_max_size");
  mysql_mutex_lock(&LOCK_log);
  if (is_open())
    max_size= max_size_arg;
  mysql_mutex_unlock(&LOCK_log);
  DBUG_VOID_RETURN;
}


void MYSQL_BIN_LOG::signal_update()
{
  DBUG_ENTER("MYSQL_BIN_LOG::signal_update");
  signal_cnt++;
  mysql_cond_broadcast(&update_cond);
  DBUG_VOID_RETURN;
}

/*
 * Caller must hold LOCK_log mutex when the file is in use.
 */
void MYSQL_BIN_LOG::update_binlog_end_pos(bool need_lock)
{
  if (need_lock)
    lock_binlog_end_pos();
  mysql_mutex_assert_owner(&LOCK_binlog_end_pos);
  strmake(binlog_file_name, log_file_name, sizeof(binlog_file_name)-1);
  binlog_end_pos=
    is_relay_log ? my_b_append_tell(&log_file) : my_b_tell(&log_file);
  signal_update();
  if (need_lock)
    unlock_binlog_end_pos();
}

/****** transaction coordinator log for 2pc - binlog() based solution ******/

/**
  @todo
  keep in-memory list of prepared transactions
  (add to list in log(), remove on unlog())
  and copy it to the new binlog if rotated
  but let's check the behaviour of tc_log_page_waits first!
*/

int MYSQL_BIN_LOG::open_binlog(const char *opt_name)
{
  LOG_INFO log_info;
  int      error= 1;

  /*
    This function is used for 2pc transaction coordination.  Hence, it
    is never used for relay logs.
  */
  DBUG_ASSERT(!is_relay_log);
  DBUG_ASSERT(total_ha_2pc > 1 || (1 == total_ha_2pc && opt_bin_log));
  DBUG_ASSERT(opt_name && opt_name[0]);

  if (!my_b_inited(&index_file))
  {
    /* There was a failure to open the index file, can't open the binlog */
    cleanup();
    return 1;
  }

  if (using_heuristic_recover())
  {
    /* generate a new binlog to mask a corrupted one */
    open_binlog(opt_name, 0, WRITE_CACHE, max_binlog_size, false,
                true/*need_lock_index=true*/,
                true/*need_sid_lock=true*/,
                NULL);
    cleanup();
    return 1;
  }

  if ((error= find_log_pos(&log_info, NullS, true/*need_lock_index=true*/)))
  {
    if (error != LOG_INFO_EOF)
      sql_print_error("find_log_pos() failed (error: %d)", error);
    else
    {
      open_binlog_found= false;
      error= 0;
    }
    goto err;
  }

  {
    const char *errmsg;
    IO_CACHE    log;
    File        file;
    Log_event  *ev=0;
    Format_description_log_event fdle(BINLOG_VERSION);
    char        log_name[FN_REFLEN];
    my_off_t    valid_pos= 0;
    my_off_t    binlog_size;
    MY_STAT     s;

    if (! fdle.is_valid())
      goto err;

    do
    {
      strmake(log_name, log_info.log_file_name, sizeof(log_name)-1);
    } while (!(error= find_next_log(&log_info, true/*need_lock_index=true*/)));

    if (error !=  LOG_INFO_EOF)
    {
      sql_print_error("find_log_pos() failed (error: %d)", error);
      goto err;
    }

    if ((file= open_binlog_file(&log, log_name, &errmsg)) < 0)
    {
      sql_print_error("%s", errmsg);
      goto err;
    }

    my_stat(log_name, &s, MYF(0));
    binlog_size= s.st_size;

    if ((ev= Log_event::read_log_event(&log, 0, &fdle,
                                       opt_master_verify_checksum, NULL)) &&
        ev->get_type_code() == FORMAT_DESCRIPTION_EVENT &&
        ev->flags & LOG_EVENT_BINLOG_IN_USE_F)
    {
      sql_print_information("Recovering after a crash using %s", opt_name);
      valid_pos= my_b_tell(&log);

      // Get the raw filename without dirname
      const std::string cur_log_file= log_name + dirname_length(log_name);
      error= recover(
          &log, (Format_description_log_event *)ev, &valid_pos, cur_log_file);
      open_binlog_found= true;
    }
    else
    {
      /*
       * If we are here, it implies either mysqld was shutdown cleanly or
       * it was killed during binlog rotation where old binlog file was
       * closed cleanly but new binlog file was not created. In the later case,
       * the storage engine recovery must be triggered so that engine's binlog
       * coordinates (engine_binlog_file and engine_binlog_pos) are updated
       * properly.
       *
       * Note we don't need binlog recovery here since it was closed cleanly.
       * Since recovery in fb-mysql works assuming storage engine as source
       * of truth, it doesn't need the list of xids to recover.
       * We will update binlog state (GTID_SET) based on the storage engine
       * coordinates in init_slave().
       */
      HASH xids;
      my_hash_init(&xids, &my_charset_bin, TC_LOG_PAGE_SIZE/3, 0,
                   sizeof(my_xid), 0, 0, MYF(0));
      error= ha_recover(&xids, engine_binlog_file, &engine_binlog_pos,
                        &engine_binlog_max_gtid);
      my_hash_free(&xids);
      open_binlog_found= false;
    }

    delete ev;
    end_io_cache(&log);
    mysql_file_close(file, MYF(MY_WME));

    if (error)
      goto err;

    /* Trim the crashed binlog file to last valid transaction
      or event (non-transaction) base on valid_pos. */
    if (valid_pos > 0)
    {
      if ((file= mysql_file_open(key_file_binlog, log_name,
                                 O_RDWR | O_BINARY, MYF(MY_WME))) < 0)
      {
        sql_print_error("Failed to open the crashed binlog file "
                        "when master server is recovering it.");
        return -1;
      }

      /* Change binlog file size to valid_pos */
      if (valid_pos < binlog_size)
      {
        if (opt_trim_binlog)
        {
          char backup_file[FN_REFLEN];
          myf opt= MY_REPLACE_DIR | MY_UNPACK_FILENAME | MY_APPEND_EXT;
          fn_format(
              backup_file, "binlog_backup", opt_mysql_tmpdir, ".trunc", opt);

          // NO_LINT_DEBUG
          sql_print_error("Taking backup from %s to %s\n",
              log_name, backup_file);
          /* MY_HOLD_ORIGINAL_MODES prevents attempts to chown the file */
          if (my_copy(log_name, backup_file, MYF(MY_WME | MY_HOLD_ORIGINAL_MODES)))
          {
            // NO_LINT_DEBUG
            sql_print_error(
                "Could not take backup of the truncated binlog file %s",
                log_name);
          }
        }

        if (my_chsize(file, valid_pos, 0, MYF(MY_WME)))
        {
          sql_print_error("Failed to trim the crashed binlog file "
                          "when master server is recovering it.");
          mysql_file_close(file, MYF(MY_WME));
          return -1;
        }
        else
        {
          sql_print_information("Crashed binlog file %s size is %llu, "
                                "but recovered up to %llu. Binlog trimmed to %llu bytes.",
                                log_name, binlog_size, valid_pos, valid_pos);
        }
      }

      /* If raft plugin is not enabled, then clear the 'file-in-use' flag.
       * If raft plugin is in use, then the file gets recovered only later when
       * the node joins the ring at which point it might do additional trimming
       * of the last binlog file in use. */
      if (!enable_raft_plugin)
      {
        /* Clear LOG_EVENT_BINLOG_IN_USE_F */
        my_off_t offset= BIN_LOG_HEADER_SIZE + FLAGS_OFFSET;
        uchar flags= 0;
        if (mysql_file_pwrite(file, &flags, 1, offset, MYF(0)) != 1)
        {
          // NO_LINT_DEBUG
          sql_print_error("Failed to clear LOG_EVENT_BINLOG_IN_USE_F "
                          "for the crashed binlog file when master "
                          "server is recovering it.");
          mysql_file_close(file, MYF(MY_WME));
          return -1;
        }
      }
      mysql_file_close(file, MYF(MY_WME));
    } //end if
  }

err:
  return error;
}

/** This is called on shutdown, after ha_panic. */
void MYSQL_BIN_LOG::close()
{
}

/*
  Prepare the transaction in the transaction coordinator.

  This function will prepare the transaction in the storage engines
  (by calling @c ha_prepare_low) what will write a prepare record
  to the log buffers.

  @retval 0    success
  @retval 1    error
*/
int MYSQL_BIN_LOG::prepare(THD *thd, bool all, bool async)
{
  DBUG_ENTER("MYSQL_BIN_LOG::prepare");

  /*
    Set HA_IGNORE_DURABILITY to not flush the prepared record of the
    transaction to the log of storage engine (for example, InnoDB
    redo log) during the prepare phase. So that we can flush prepared
    records of transactions to the log of storage engine in a group
    right before flushing them to binary log during binlog group
    commit flush stage. Reset to HA_REGULAR_DURABILITY at the
    beginning of parsing next command.
  */
  thd->durability_property= HA_IGNORE_DURABILITY;

  int error= ha_prepare_low(thd, all, async);

  DBUG_RETURN(error);
}

/**
  Commit the transaction in the transaction coordinator.

  This function will commit the sessions transaction in the binary log
  and in the storage engines (by calling @c ha_commit_low). If the
  transaction was successfully logged (or not successfully unlogged)
  but the commit in the engines did not succed, there is a risk of
  inconsistency between the engines and the binary log.

  For binary log group commit, the commit is separated into three
  parts:

  1. First part consists of filling the necessary caches and
     finalizing them (if they need to be finalized). After this,
     nothing is added to any of the caches.

  2. Second part execute an ordered flush and commit. This will be
     done using the group commit functionality in ordered_commit.

  3. Third part checks any errors resulting from the ordered commit
     and handles them appropriately.

  @retval 0    success
  @retval 1    error, transaction was neither logged nor committed
  @retval 2    error, transaction was logged but not committed
*/
TC_LOG::enum_result MYSQL_BIN_LOG::commit(THD *thd, bool all, bool async)
{
  DBUG_ENTER("MYSQL_BIN_LOG::commit");

  binlog_cache_mngr *cache_mngr= thd_get_cache_mngr(thd);
  my_xid xid= thd->transaction.xid_state.xid.get_my_xid();
  int error= RESULT_SUCCESS;
  bool stuff_logged= false;

  DBUG_PRINT("enter", ("thd: 0x%llx, all: %s, xid: %llu, cache_mngr: 0x%llx",
                       (ulonglong) thd, YESNO(all), (ulonglong) xid,
                       (ulonglong) cache_mngr));

  /*
    No cache manager means nothing to log, but we still have to commit
    the transaction.
   */
  if (cache_mngr == NULL)
  {
    if (ha_commit_low(thd, all, async))
      DBUG_RETURN(RESULT_ABORTED);
    DBUG_RETURN(RESULT_SUCCESS);
  }

  THD_TRANS *trans= all ? &thd->transaction.all : &thd->transaction.stmt;

  DBUG_PRINT("debug", ("in_transaction: %s, no_2pc: %s, rw_ha_count: %d",
                       YESNO(thd->in_multi_stmt_transaction_mode()),
                       YESNO(trans->no_2pc),
                       trans->rw_ha_count));
  DBUG_PRINT("debug",
             ("all.cannot_safely_rollback(): %s, trx_cache_empty: %s",
              YESNO(thd->transaction.all.cannot_safely_rollback()),
              YESNO(cache_mngr->trx_cache.is_binlog_empty())));
  DBUG_PRINT("debug",
             ("stmt.cannot_safely_rollback(): %s, stmt_cache_empty: %s",
              YESNO(thd->transaction.stmt.cannot_safely_rollback()),
              YESNO(cache_mngr->stmt_cache.is_binlog_empty())));


  /*
    If there are no handlertons registered, there is nothing to
    commit. Note that DDLs are written earlier in this case (inside
    binlog_query).

    TODO: This can be a problem in those cases that there are no
    handlertons registered. DDLs are one example, but the other case
    is MyISAM. In this case, we could register a dummy handlerton to
    trigger the commit.

    Any statement that requires logging will call binlog_query before
    trans_commit_stmt, so an alternative is to use the condition
    "binlog_query called or stmt.ha_list != 0".
   */
  if (!all && trans->ha_list == 0 &&
      cache_mngr->stmt_cache.is_binlog_empty())
    DBUG_RETURN(RESULT_SUCCESS);

  /*
    If there is anything in the stmt cache, and GTIDs are enabled,
    then this is a single statement outside a transaction and it is
    impossible that there is anything in the trx cache.  Hence, we
    write any empty group(s) to the stmt cache.

    Otherwise, we write any empty group(s) to the trx cache at the end
    of the transaction.
  */
  if (!cache_mngr->stmt_cache.is_binlog_empty())
  {
    error= write_empty_groups_to_cache(thd, &cache_mngr->stmt_cache);
    if (error == 0)
    {
      if (cache_mngr->stmt_cache.finalize(thd))
        DBUG_RETURN(RESULT_ABORTED);
      stuff_logged= true;
    }
  }

  /*
    We commit the transaction if:
     - We are not in a transaction and committing a statement, or
     - We are in a transaction and a full transaction is committed.
    Otherwise, we accumulate the changes.
  */
  if (!error && !cache_mngr->trx_cache.is_binlog_empty() &&
      ending_trans(thd, all))
  {
    const bool real_trans= (all || thd->transaction.all.ha_list == 0);
    /*
      We are committing an XA transaction if it is a "real" transaction
      and have an XID assigned (because some handlerton registered). A
      transaction is "real" if either 'all' is true or the 'all.ha_list'
      is empty.

      Note: This is kind of strange since registering the binlog
      handlerton will then make the transaction XA, which is not really
      true. This occurs for example if a MyISAM statement is executed
      with row-based replication on.
   */
    if (real_trans && xid && trans->rw_ha_count > 1 && !trans->no_2pc)
    {
      Xid_log_event end_evt(thd, xid);
      if (cache_mngr->trx_cache.finalize(thd, &end_evt))
        DBUG_RETURN(RESULT_ABORTED);
    }
    else
    {
      Query_log_event end_evt(thd, STRING_WITH_LEN("COMMIT"),
                              true, FALSE, TRUE, 0, TRUE);
      if (cache_mngr->trx_cache.finalize(thd, &end_evt))
        DBUG_RETURN(RESULT_ABORTED);
    }
    stuff_logged= true;
  }

  /*
    This is part of the stmt rollback.
  */
  if (!all)
    cache_mngr->trx_cache.set_prev_position(MY_OFF_T_UNDEF);

  DBUG_PRINT("debug", ("error: %d", error));

  if (error)
    DBUG_RETURN(RESULT_ABORTED);

  /*
    Now all the events are written to the caches, so we will commit
    the transaction in the engines. This is done using the group
    commit logic in ordered_commit, which will return when the
    transaction is committed.

    If the commit in the engines fail, we still have something logged
    to the binary log so we have to report this as a "bad" failure
    (failed to commit, but logged something).
  */
  if (stuff_logged)
  {
    if (ordered_commit(thd, all, false, async))
      DBUG_RETURN(RESULT_INCONSISTENT);
  }
  else
  {
    /*
      We only set engine binlog position in ordered_commit path flush phase
      and not all transactions go through them (such as table copy in DDL).
      So in cases where a DDL statement implicitly commits earlier transaction
      and starting a new one, the new transaction could be "leaking" the
      engine binlog pos. In order to avoid that and accidentally overwrite
      binlog position with previous location, we reset it here.
    */
    thd->set_trans_pos(NULL, 0, NULL);
    if (ha_commit_low(thd, all, async))
      DBUG_RETURN(RESULT_INCONSISTENT);
  }

  DBUG_RETURN(error ? RESULT_INCONSISTENT : RESULT_SUCCESS);
}


/**
   Flush caches for session.

   @note @c set_trans_pos is called with a pointer to the file name
   that the binary log currently use and a rotation will change the
   contents of the variable.

   The position is used when calling the after_flush, after_commit,
   and after_rollback hooks, but these have been placed so that they
   occur before a rotation is executed.

   It is the responsibility of any plugin that use this position to
   copy it if they need it after the hook has returned.
 */
std::pair<int,my_off_t>
MYSQL_BIN_LOG::flush_thread_caches(THD *thd, bool async)
{
  binlog_cache_mngr *cache_mngr= thd_get_cache_mngr(thd);
  my_off_t bytes= 0;
  bool wrote_xid= false;
  Cached_group *last_group = NULL;
  // group cache is reset after flush. So last gtid in the
  // group cache should be stored before flush.
  binlog_cache_data *cache_data=
    cache_mngr->get_binlog_cache_data(true);
  if (cache_data)
  {
    Group_cache *group_cache = &cache_data->group_cache;
    if (group_cache)
    {
      last_group = group_cache->get_last_group();
    }
  }

  int error= cache_mngr->flush(thd, &bytes, &wrote_xid, async);
  if (!error && bytes > 0)
  {
    /*
      Note that set_trans_pos does not copy the file name. See
      this function documentation for more info.
    */
    thd->set_trans_pos(log_file_name, my_b_tell(&log_file), last_group);
    if (wrote_xid)
      inc_prep_xids(thd);
    else
      inc_non_xid_trxs(thd);
  }
  DBUG_PRINT("debug", ("bytes: %llu", bytes));
  return std::make_pair(error, bytes);
}


/**
  Execute the flush stage.

  @param total_bytes_var Pointer to variable that will be set to total
  number of bytes flushed, or NULL.

  @param rotate_var Pointer to variable that will be set to true if
  binlog rotation should be performed after releasing locks. If rotate
  is not necessary, the variable will not be touched.

  @return Error code on error, zero on success
 */

int
MYSQL_BIN_LOG::process_flush_stage_queue(my_off_t *total_bytes_var,
                                         bool *rotate_var,
                                         THD **out_queue_var,
                                         bool async)
{
  DBUG_ASSERT(total_bytes_var && rotate_var && out_queue_var);
  my_off_t total_bytes= 0;
  int flush_error= 1;
  int commit_consensus_error= 0;
  mysql_mutex_assert_owner(&LOCK_log);

  /*
    Fetch the entire flush queue and empty it, so that the next batch
    has a leader. We must do this before invoking ha_flush_logs(...)
    for guaranteeing to flush prepared records of transactions before
    flushing them to binary log, which is required by crash recovery.
  */
  THD *first_seen= stage_manager.fetch_queue_for(Stage_manager::FLUSH_STAGE);
  DBUG_ASSERT(first_seen != NULL);

  /* Do an explicit transaction log group write before flushing binary log
     cache to file. */
  if (!first_seen->prepared_engine->is_empty())
    ha_flush_logs(NULL, first_seen->prepared_engine);

#ifndef DBUG_OFF
  for (THD *head= first_seen ; head ; head = head->next_to_commit)
  {
    DBUG_ASSERT(head->prepared_engine->compare_lt(
          first_seen->prepared_engine->get_maps()));
  }
#endif

  DBUG_EXECUTE_IF("crash_after_flush_engine_log", DBUG_SUICIDE(););

  ulonglong thd_count = 0;
  /* Flush thread caches to binary log. */
  for (THD *head= first_seen ; head ; head = head->next_to_commit)
  {
    std::pair<int,my_off_t> result= flush_thread_caches(head, async);
    total_bytes+= result.second;
    if (flush_error == 1)
      flush_error= result.first;
    else if (result.first)
    {
      // NO_LINT_DEBUG
      sql_print_error(
          "flush stage hides errors of follower threads in the group");
    }

    /* There is a weird check above that if first thread in the group could
     * flush successfully, then every thread in the group will also flush
     * successfully. This does not look right - so for the time being, we will
     * use commit_consensus_error flag to identify if there was a error in
     * before_flush hook of raft plugin and set flush_error here. This will
     * subsequently fail the entire group
     */
    if (head->commit_consensus_error)
      commit_consensus_error= 1;

    /* Reset prepared_engine for every thd in the queue. */
    head->prepared_engine->clear();
    ++thd_count;
  }
  DBUG_ASSERT(thd_count > 0);
  DBUG_PRINT("info", ("Number of threads in group commit %llu", thd_count));
  counter_histogram_increment(&histogram_binlog_group_commit, thd_count);

  *out_queue_var= first_seen;
  *total_bytes_var= total_bytes;
  if (total_bytes > 0 && my_b_tell(&log_file) >= (my_off_t) max_size)
    *rotate_var= true;

  if (commit_consensus_error && enable_raft_plugin)
    flush_error= 1;

  return flush_error;
}


/**
  Commit a sequence of sessions.

  This function commit an entire queue of sessions starting with the
  session in @c first. If there were an error in the flushing part of
  the ordered commit, the error code is passed in and all the threads
  are marked accordingly (but not committed).

  @see MYSQL_BIN_LOG::ordered_commit

  @param thd The "master" thread
  @param first First thread in the queue of threads to commit
 */

void
MYSQL_BIN_LOG::process_commit_stage_queue(THD *thd, THD *first, bool async)
{
  mysql_mutex_assert_owner(&LOCK_commit);
  Thread_excursion excursion(thd);
#ifndef DBUG_OFF
  thd->transaction.flags.ready_preempt= 1; // formality by the leader
#endif

  for (THD *head= first ; head ; head = head->next_to_commit)
  {
    DBUG_PRINT("debug", ("Thread ID: %u, commit_error: %d, flags.pending: %s",
                         head->thread_id(), head->commit_error,
                         YESNO(head->transaction.flags.pending)));
    /*
      If flushing failed, set commit_error for the session, skip the
      transaction and proceed with the next transaction instead. This
      will mark all threads as failed, since the flush failed.

      If flush succeeded, attach to the session and commit it in the
      engines.
    */
#ifndef DBUG_OFF
    stage_manager.clear_preempt_status(head);
#endif
    /*
      Flush/Sync error should be ignored and continue
      to commit phase. And thd->commit_error cannot be
      COMMIT_ERROR at this moment.
    */
    DBUG_ASSERT(head->commit_error != THD::CE_COMMIT_ERROR);
    excursion.try_to_attach_to(head);
    bool all= head->transaction.flags.real_commit;
    if (head->transaction.flags.commit_low)
    {
      /* head is parked to have exited append() */
      DBUG_ASSERT(head->transaction.flags.ready_preempt);
      /*
        storage engine commit
      */
      if (head->commit_consensus_error)
      {
        handle_commit_consensus_error(head, async);
      }
      else if (ha_commit_low(head, all, async, false))
      {
        head->commit_error= THD::CE_COMMIT_ERROR;
      }
    }
    DBUG_PRINT("debug", ("commit_error: %d, flags.pending: %s",
                         head->commit_error,
                         YESNO(head->transaction.flags.pending)));
    /*
      Decrement the prepared XID counter after storage engine commit.
      We also need decrement the prepared XID when encountering a
      flush error or session attach error for avoiding 3-way deadlock
      among user thread, rotate thread and dump thread.
    */
    if (head->transaction.flags.xid_written)
      dec_prep_xids(head);
    else if (head->non_xid_trx)
      dec_non_xid_trxs(head);
  }
}

/**
  Process after commit for a sequence of sessions.
  @param thd The "master" thread
  @param first First thread in the queue of threads to commit
*/
void
MYSQL_BIN_LOG::process_after_commit_stage_queue(THD *thd, THD *first,
                                                bool async)
{
  Thread_excursion excursion(thd);
  int error= 0;
  THD *last_thd= nullptr;
  uint64_t max_group_hlc= 0;
  for (THD *head= first; head; head= head->next_to_commit)
  {
    if (enable_binlog_hlc && maintain_database_hlc && head->hlc_time_ns_next &&
        !head->commit_consensus_error && head->commit_error == THD::CE_NONE) {
      if (likely(!head->databases.empty()))
      {
        // Successfully committed the trx to engine. Update applied hlc for
        // all databases that this trx touches
        hlc.update_database_hlc(head->databases, head->hlc_time_ns_next);
      }
      else if (log_warnings >= 2)
      {
        // Log a error line if databases are empty. This could happen in SBR
        // NO_LINT_DEBUG
        sql_print_error("Databases were empty for this trx. HLC= %lu",
                        head->hlc_time_ns_next);
      }
      head->databases.clear();

      /* Update max group hlc */
      if (head->rli_slave || head->rli_fake)
      {
        max_group_hlc= std::max(max_group_hlc, head->hlc_time_ns_next);
      }
      head->hlc_time_ns_next= 0;
    }

    if (head->transaction.flags.run_hooks &&
        head->commit_error == THD::CE_NONE &&
        !head->commit_consensus_error)
    {
      /*
        TODO: This hook here should probably move outside/below this
              if and be the only after_commit invocation left in the
              code.
      */
      excursion.try_to_attach_to(head);
      bool all= head->transaction.flags.real_commit;

      // Call semi-sync plugin only when raft is not enabled
      if (!enable_raft_plugin)
        (void) RUN_HOOK(transaction, after_commit, (head, all));
      else
        error = error || RUN_HOOK_STRICT(
            raft_replication, after_commit, (head, all));

      /*
        When after_commit finished for the transaction, clear the run_hooks flag.
        This allow other parts of the system to check if after_commit was called.
      */
      head->transaction.flags.run_hooks= false;
      last_thd= thd;
    }
  }

  /*
   * If this is a slave, then update the local HLC to reflect the max HLC of
   * this group (as generated by master)
   */
  if (max_group_hlc != 0)
  {
    update_hlc(max_group_hlc);
  }

  if (!error &&
      last_thd &&
      opt_raft_signal_async_dump_threads == AFTER_ENGINE_COMMIT &&
      enable_raft_plugin &&
      rpl_wait_for_semi_sync_ack)
  {
    char* log_file= nullptr;
    my_off_t log_pos= 0;
    if (mysql_bin_log.is_apply_log)
      last_thd->get_trans_relay_log_pos((const char**) &log_file, &log_pos);
    else
      last_thd->get_trans_fixed_pos((const char **) &log_file, &log_pos);
    const LOG_POS_COORD coord= { log_file, log_pos };
    signal_semi_sync_ack(&coord);
  }
}


/**
  Scans the semisync queue and calls before_commit hook
  using the last thread in the queue.

  @param queue_head  Head of the semisync stage queue.

  Note that this should be used only for semisync wait.
*/
void
MYSQL_BIN_LOG::process_semisync_stage_queue(THD *queue_head)
{
    DBUG_EXECUTE_IF("before_before_commit", {
                    const char act[]= "now signal reached "
                                      "wait_for continue";
                    DBUG_ASSERT(opt_debug_sync_timeout > 0);
                    DBUG_ASSERT(!debug_sync_set_action(
                                   current_thd,
                                   STRING_WITH_LEN(act)));
                    });
   THD *last_thd = NULL;
   int error= 0;
   for (THD *thd = queue_head; thd != NULL; thd = thd->next_to_commit)
   {
      if (thd->commit_error == THD::CE_NONE)
      {
        last_thd = thd;
      }
   }

   if (last_thd)
   {
     // Since flush ordered is maintained even in the semisync stage
     // calling hook for the last valid thd is sufficient since it
     // will have the maximum binlog position.
     if (!enable_raft_plugin)
     {
       // semi-sync hook to be called only when raft is not enabled
       (void) RUN_HOOK(transaction, before_commit,
                       (last_thd, last_thd->transaction.flags.real_commit));
     }
     else
     {
       error= RUN_HOOK_STRICT(
           raft_replication, before_commit,
           (last_thd, last_thd->transaction.flags.real_commit));
     }
     DBUG_EXECUTE_IF("simulate_before_commit_error", {error= 1;});

     if (error)
       set_commit_consensus_error(queue_head);

     if (!error &&
         opt_raft_signal_async_dump_threads == AFTER_CONSENSUS &&
         enable_raft_plugin &&
         rpl_wait_for_semi_sync_ack)
     {
       char* log_file= nullptr;
       my_off_t log_pos= 0;
       if (mysql_bin_log.is_apply_log)
         last_thd->get_trans_relay_log_pos((const char**) &log_file, &log_pos);
       else
         last_thd->get_trans_fixed_pos((const char **) &log_file, &log_pos);
       const LOG_POS_COORD coord= { log_file, log_pos };
       signal_semi_sync_ack(&coord);
     }
   }
}

/**
  Sets the commit_consensus_error flag in all thd's of this group if there was
  an error in the before_commit hook

  @param queue_head  Head of the semisync stage queue.
*/
void MYSQL_BIN_LOG::set_commit_consensus_error(THD *queue_head)
{
   for (THD *thd = queue_head; thd != NULL; thd = thd->next_to_commit)
   {
     thd->commit_consensus_error= true;
   }
}

/**
  Handles commit_consensus_error by consulting commit_consensus_error_action

  @param thd Thd for which conecnsus error need to be handled
*/
void MYSQL_BIN_LOG::handle_commit_consensus_error(THD *thd, bool async)
{
  DBUG_ENTER("MYSQL_BIN_LOG::handle_commit_consensus_error");
  bool all= thd->transaction.flags.real_commit;

  /* Handle commit consensus error appropriately */
  switch (opt_commit_consensus_error_action)
  {
    case ROLLBACK_TRXS_IN_GROUP:
      /* Rollbak the trx and set commit_error in thd->commit_error
       * Also clear commit_low flag to prevent commit getting
       * triggered when the session ends. ha_rollback_low() could fail,
       * but there is nothing much we can do */
       ha_rollback_low(thd, all);

       // Remove the gtid from the logged gtids since we are rolling back the
       // trx. Do not clear owned gtid yet, as we need it for additional cleanup
       // in gtid_rollback()
       global_sid_lock->rdlock();
       gtid_state->remove_gtid_on_failure(thd, /*clear_owned_gtid=*/ false);
       global_sid_lock->unlock();

       // rollback the gtid, this updates owned gtids correctly
       gtid_rollback(thd);

       thd->commit_error= THD::CE_COMMIT_ERROR;
       thd->transaction.flags.commit_low= false;

       // Clear hlc_time since we did not commit this trx
       thd->hlc_time_ns_next= 0;

       /* Insert a error frame. TODO: See if this has to be a different
        * error code */
       thd->clear_error(); // Clear previous errors first
       my_error(ER_ERROR_DURING_COMMIT, MYF(0), 1);
       break;
     case IGNORE_COMMIT_CONSENSUS_ERROR:
       /* Ignore commit consensus error and commit to engine as usual */
       if (ha_commit_low(thd, all, async, false))
       {
         thd->commit_error= THD::CE_COMMIT_ERROR;
       }
       break;
     default:
       // Should not happen. This is here to placate the compiler
       DBUG_ASSERT(false);
  }

  DBUG_VOID_RETURN;
}

#ifndef DBUG_OFF
/** Names for the stages. */
static const char* g_stage_name[] = {
  "FLUSH",
  "SYNC",
  "SEMISYNC",
  "COMMIT",
};
#endif


/**
  Enter a stage of the ordered commit procedure.

  Entering is stage is done by:

  - Atomically enqueueing a queue of processes (which is just one for
    the first phase).

  - If the queue was empty, the thread is the leader for that stage
    and it should process the entire queue for that stage.

  - If the queue was not empty, the thread is a follower and can go
    waiting for the commit to finish.

  The function will lock the stage mutex if it was designated the
  leader for the phase.

  @param thd                Session structure
  @param stage              The stage to enter
  @param queue              Queue of threads to enqueue for the stage
  @param leave_mutex        Mutex which will be released
  @param enter_mutex        Mutex which will be acquired

  @retval true  The thread should "bail out" and go waiting for the
                commit to finish
  @retval false The thread is the leader for the stage and should do
                the processing.
*/

bool
MYSQL_BIN_LOG::change_stage(THD *thd,
                            Stage_manager::StageID stage, THD *queue,
                            mysql_mutex_t *leave_mutex,
                            mysql_mutex_t *enter_mutex)
{
  DBUG_ENTER("MYSQL_BIN_LOG::change_stage");
  DBUG_PRINT("enter", ("thd: 0x%llx, stage: %s, queue: 0x%llx",
                       (ulonglong) thd, g_stage_name[stage], (ulonglong) queue));
  DBUG_ASSERT(0 <= stage && stage < Stage_manager::STAGE_COUNTER);
  DBUG_ASSERT(enter_mutex);
  DBUG_ASSERT(queue);
  /*
    After the sessions are queued, enroll_for will acquire the enter_mutex, if
    the thread is the leader. After which, regardless of being the leader, it
    will release the leave_mutex.
  */
  if (!stage_manager.enroll_for(stage, queue, leave_mutex, enter_mutex))
  {
    DBUG_ASSERT(!thd_get_cache_mngr(thd)->dbug_any_finalized());
    DBUG_RETURN(true);
  }
  DBUG_RETURN(false);
}



/**
  Flush the I/O cache to file.

  Flush the binary log to the binlog file if any byte where written
  and signal that the binary log file has been updated if the flush
  succeeds.
*/

int
MYSQL_BIN_LOG::flush_cache_to_file(my_off_t *end_pos_var)
{
  if (flush_io_cache(&log_file))
  {
    THD *thd= current_thd;
    thd->commit_error= THD::CE_FLUSH_ERROR;
    return ER_ERROR_ON_WRITE;
  }
  *end_pos_var= my_b_tell(&log_file);
  return 0;
}


/**
  Call fsync() to sync the file to disk.
*/
std::pair<bool, bool>
MYSQL_BIN_LOG::sync_binlog_file(bool force, bool async)
{
  bool synced= false;
  ulonglong start_time, binlog_fsync_time;
  unsigned int sync_period= get_sync_period();
  if (force || (!async && (sync_period && ++sync_counter >= sync_period)))
  {
    sync_counter= 0;
    statistic_increment(binlog_fsync_count, &LOCK_status);

    /**
      On *pure non-transactional* workloads there is a small window
      in time where a concurrent rotate might be able to close
      the file before the sync is actually done. In that case,
      ignore the bad file descriptor errors.

      Transactional workloads (InnoDB) are not affected since the
      the rotation will not happen until all transactions have
      committed to the storage engine, thence decreased the XID
      counters.

      TODO: fix this properly even for non-transactional storage
            engines.
     */
    start_time = my_timer_now();
    int ret = DBUG_EVALUATE_IF("simulate_error_during_sync_binlog_file", 1,
                         mysql_file_sync(log_file.file,
                                         MYF(MY_WME | MY_IGNORE_BADFD)));
    binlog_fsync_time = my_timer_since(start_time);
    if (histogram_step_size_binlog_fsync)
      latency_histogram_increment(&histogram_binlog_fsync,
                                  binlog_fsync_time, 1);
    if (ret)
    {
      THD *thd= current_thd;
      thd->commit_error= THD::CE_SYNC_ERROR;
      return std::make_pair(true, synced);
    }
    synced= true;
  }
  return std::make_pair(false, synced);
}


/**
   Helper function executed when leaving @c ordered_commit.

   This function contain the necessary code for fetching the error
   code, doing post-commit checks, and wrapping up the commit if
   necessary.

   It is typically called when enter_stage indicates that the thread
   should bail out, and also when the ultimate leader thread finishes
   executing @c ordered_commit.

   It is typically used in this manner:
   @code
   if (enter_stage(thd, Thread_queue::FLUSH_STAGE, thd, &LOCK_log))
     return finish_commit(thd);
   @endcode

   @return Error code if the session commit failed, or zero on
   success.
 */
int
MYSQL_BIN_LOG::finish_commit(THD *thd, bool async)
{
  /*
    In some unlikely situations, it can happen that binary
    log is closed before the thread flushes it's cache.
    In that case, clear the caches before doing commit.
  */
  if (unlikely(!is_open()))
  {
    binlog_cache_mngr *cache_mngr= thd_get_cache_mngr(thd);
    if (cache_mngr)
      cache_mngr->reset();
  }
  if (thd->transaction.flags.commit_low)
  {
    const bool all= thd->transaction.flags.real_commit;
    /*
      storage engine commit
    */
    DBUG_ASSERT(thd->commit_error != THD::CE_COMMIT_ERROR);
    if (thd->commit_consensus_error)
    {
      handle_commit_consensus_error(thd, async);
    }
    else if (ha_commit_low(thd, all, async, false))
    {
      thd->commit_error= THD::CE_COMMIT_ERROR;
    }

    /*
      Decrement the prepared XID counter after storage engine commit
    */
    if (thd->transaction.flags.xid_written)
      dec_prep_xids(thd);
    else if (thd->non_xid_trx)
      dec_non_xid_trxs(thd);

    /*
      If commit succeeded, we call the after_commit hook

      TODO: This hook here should probably move outside/below this
            if and be the only after_commit invocation left in the
            code.
    */
    if ((thd->commit_error == THD::CE_NONE) && thd->transaction.flags.run_hooks)
    {
      int error= 0;

      // semi-sync plugin only called when raft is not enabled
      if (!enable_raft_plugin)
        (void) RUN_HOOK(transaction, after_commit, (thd, all));
      else
        error= RUN_HOOK_STRICT(raft_replication, after_commit, (thd, all));

      if (!error &&
          opt_raft_signal_async_dump_threads == AFTER_ENGINE_COMMIT &&
          enable_raft_plugin &&
          rpl_wait_for_semi_sync_ack)
      {
        char* log_file= nullptr;
        my_off_t log_pos= 0;
        if (mysql_bin_log.is_apply_log)
          thd->get_trans_relay_log_pos((const char**) &log_file, &log_pos);
        else
          thd->get_trans_fixed_pos((const char **) &log_file, &log_pos);
        const LOG_POS_COORD coord= { log_file, log_pos };
        signal_semi_sync_ack(&coord);
      }

      thd->transaction.flags.run_hooks= false;
    }
  }
  else if (thd->transaction.flags.xid_written)
    dec_prep_xids(thd);
  else if (thd->non_xid_trx)
    dec_non_xid_trxs(thd);

  if (!thd->gtid_precommit)
  {
    /*
     Remove committed GTID from owned_gtids, it was already logged on
     MYSQL_BIN_LOG::write_cache().
    */
    global_sid_lock->rdlock();
    gtid_state->update_on_commit(thd);
    global_sid_lock->unlock();
  }
  else
  {
    /* Reset gtid_precommit. */
    thd->gtid_precommit= false;
    /* Clear gtid owned by current THD. */
    thd->clear_owned_gtids();
    thd->variables.gtid_next.set_undefined();
  }

  /* If this is a slave, then update the local HLC to reflect the HLC of this
   * trx (as generated by master) */
  if (thd->hlc_time_ns_next != 0 && enable_binlog_hlc &&
      (thd->rli_slave || thd->rli_fake))
  {
    update_hlc(thd->hlc_time_ns_next);
  }
  thd->hlc_time_ns_next= 0;

  // Clear the raft opid that is stashed, so that if the thread
  // is reused, it does not have stale terms and indexes
  thd->clear_raft_opid();

  DBUG_ASSERT(thd->commit_error || !thd->transaction.flags.run_hooks);
  DBUG_ASSERT(!thd_get_cache_mngr(thd)->dbug_any_finalized());
  DBUG_PRINT("return", ("Thread ID: %u, commit_error: %d",
                        thd->thread_id(), thd->commit_error));

  /*
    During shutdown, forcibly disconnect thd connections for
    transactions that are in the commit pipeline
  */
  DEBUG_SYNC(thd, "commit_wait_for_shutdown");
  if (!thd->slave_thread && thd->killed == THD::KILL_CONNECTION) {
    thd->disconnect();
  }

  /*
    flush or sync errors are handled by the leader of the group
    (using binlog_error_action). Hence treat only COMMIT_ERRORs as errors.
  */
  return (thd->commit_error == THD::CE_COMMIT_ERROR);
}

/**
  Helper function to handle flush or sync stage errors.
  If binlog_error_action= ABORT_SERVER, server will be aborted
  after reporting the error to the client.
  If binlog_error_action= IGNORE_ERROR, binlog will be closed
  for the life time of the server. close() call is protected
  with LOCK_log to avoid any parallel operations on binary log.

  @param thd Thread object that faced flush/sync error
  @param need_lock_log
                       > Indicates true if LOCk_log is needed before closing
                         binlog (happens when we are handling sync error)
                       > Indicates false if LOCK_log is already acquired
                         by the thread (happens when we are handling flush
                         error)

  @return void
*/
void MYSQL_BIN_LOG::handle_binlog_flush_or_sync_error(THD *thd,
                                                      bool need_lock_log)
{
  bool commit_consensus_error= false;
  for (THD* head = thd; head; head = head->next_to_commit)
  {
    if (head->commit_consensus_error &&
        head->commit_error == THD::CE_FLUSH_ERROR)
    {
      commit_consensus_error= true;
      break;
    }
  }

  /* This trx (and the group) will be rolled back in the engine if:
   * (1) binlog_error_action is set to rollback_trx
   * (2) There was a flush error because of a commit_consensus_error (i.e flush
   * error was due to a error inside the before_flush hook of raft plugin)
   *
   * If (2) is not true, then we cannot safely rollbak the trx (either it
   * is too late OR safety of the raft consensus plugin will be violated. hence
   * we proceed and abort the server
   */
  if (binlog_error_action == ROLLBACK_TRX &&
      commit_consensus_error)
  {
    // Set commit consensus error for the entire group
    set_commit_consensus_error(thd);
    return;
  }

  char errmsg[MYSQL_ERRMSG_SIZE];
  sprintf(errmsg, "An error occurred during %s stage of the commit. "
          "'binlog_error_action' is set to '%s'.",
          thd->commit_error== THD::CE_FLUSH_ERROR ? "flush" : "sync",
          binlog_error_action == IGNORE_ERROR ?
          "IGNORE_ERROR" : "ABORT_SERVER");

  if (binlog_error_action == ABORT_SERVER ||
      binlog_error_action == ROLLBACK_TRX)
  {
    /* At this stage the error is either due to
     * (1) sync stage error
     * (2) flush stage error, but consensus error was not set - indicating that
     * the error did not happen inside raft plugin
     *
     * In both these cases we abort the server even when error_action is set to
     * rollback_trx. This is because sync happens periodically and the trx has
     * already committed to engine (so cannot rollback). We cannot safely
     * rollback flush stage errors happening outside of raft plugin.
     *
     * TODO: revisit if and when we have the ability to step down from within
     * mysql server
     */
    char err_buff[MYSQL_ERRMSG_SIZE];
    sprintf(err_buff, "%s Hence aborting the server.", errmsg);
    exec_binlog_error_action_abort(err_buff);
  }
  else
  {
    DEBUG_SYNC(thd, "before_binlog_closed_due_to_error");
    if (need_lock_log)
      mysql_mutex_lock(&LOCK_log);
    else
      mysql_mutex_assert_owner(&LOCK_log);
    /*
      It can happen that other group leader encountered
      error and already closed the binary log. So print
      error only if it is in open state. But we should
      call close() always just in case if the previous
      close did not close index file.
    */
    if (is_open())
    {
      sql_print_error("%s Hence turning logging off for the whole duration "
                      "of the MySQL server process. To turn it on again: fix "
                      "the cause, shutdown the MySQL server and restart it.",
                      errmsg);
    }
    close(LOG_CLOSE_INDEX|LOG_CLOSE_STOP_EVENT);
    /*
      If there is a write error (flush/sync stage) and if
      binlog_error_action=IGNORE_ERROR, clear the error
      and allow the commit to happen in storage engine.
    */
    if (check_write_error(thd))
      thd->clear_error();

    if (need_lock_log)
      mysql_mutex_unlock(&LOCK_log);
    DEBUG_SYNC(thd, "after_binlog_closed_due_to_error");
  }
}

int MYSQL_BIN_LOG::register_log_entities(THD *thd,
                                         int context,
                                         bool need_lock,
                                         bool is_relay_log)
{
  if (need_lock)
    mysql_mutex_lock(&LOCK_log);
  else
    mysql_mutex_assert_owner(&LOCK_log);

  Raft_replication_observer::st_setup_flush_arg arg;
  arg.log_file_cache= &log_file;
  arg.log_prefix= name;
  arg.log_name= log_file_name;
  arg.cur_log_ext= &cur_log_ext;
  arg.endpos_log_name= binlog_file_name;
  arg.endpos= &binlog_end_pos;
  arg.signal_cnt= &signal_cnt;
  arg.lock_log= &LOCK_log;
  arg.lock_index= &LOCK_index;
  arg.lock_end_pos= &LOCK_binlog_end_pos;
  arg.update_cond= &update_cond;
  arg.context= context;
  arg.is_relay_log= is_relay_log;

  int err= RUN_HOOK_STRICT(raft_replication, setup_flush, (thd, &arg));
  if (need_lock)
    mysql_mutex_unlock(&LOCK_log);
  return err;
}

void MYSQL_BIN_LOG::check_and_register_log_entities(THD *thd)
{
  mysql_mutex_assert_owner(&LOCK_log);
  if (!enable_raft_plugin_save)
    return;

  // if this is a master and raft is turned ON,
  // register the IO_cache and the appropriate locks
  // with the plugin.
  if (!mysql_bin_log.is_apply_log)
  {
    // only register once
    if (setup_flush_done)
      return;

    int err= register_log_entities(thd, 1 /* context */,
                                   false /* need lock = false */,
                                   false /* registering binlog file */);
    setup_flush_done= (err == 0);
    if (err)
    {
      // TODO. This will fatal right now as the flush stage will fail
      sql_print_error("Failed to register log entities with plugin.");
    }
    return;
  }

  // we are no more a master. We should reset
  // the variable for a future step up
  setup_flush_done= false;
}

static int register_entities_with_raft()
{
  THD *thd= current_thd;
  // First register the binlog for all servers
  // both masters and slaves
  // a Slave's mysql_bin_log will point to apply log
  // however when the slave becomes the master, its registered binlog
  // entities will be used by binlog wrapper
  int err= mysql_bin_log.register_log_entities(thd, 0, true,
      false /* binlog */);
  if (err)
  {
    sql_print_error("Failed to register binlog file entities with storage observer");
    return err;
  }

#ifdef HAVE_REPLICATION
  if (!active_mi || /* !active_mi->host[0] || */  !active_mi->rli)
  {
    // degenerate case returns SUCCESS [ TODO ]
    return 0;
  }

  // On a slave server, also register the relaylogs
  // Plugin will make that the default file to write to
  err= active_mi->rli->relay_log.register_log_entities(
      thd, 0, true /* take locks */, true /* relay log */);
  if (err)
  {
    sql_print_error("Failed to register relaylog file entities");
  }
#endif
  return err;
}

// This function is called by plugin after BinlogWrapper creation.
// It asks the server to register the binlog & relaylog file
// immediately
int ask_server_to_register_with_raft(Raft_Registration_Item item)
{
  int err= 0;
  switch(item) {
    case RAFT_REGISTER_LOCKS:
      return register_entities_with_raft();
    case RAFT_REGISTER_PATHS:
    {
      size_t llen;

      std::string s_wal_dir;
      char wal_dir[FN_REFLEN];
      if (!dirname_part(wal_dir, log_bin_basename, &llen))
      {
        sql_print_information("dirname_part for log_file_basename fails. Falling back to datadir");
        s_wal_dir.assign(mysql_real_data_home_ptr);
      } else
        s_wal_dir.assign(wal_dir);

      std::string s_log_dir;
      char log_dir[FN_REFLEN];
      if (!dirname_part(log_dir, log_error_file_ptr, &llen))
      {
        sql_print_information("dirname_part for log_error_file_ptr fails. Falling back to datadir");
        s_log_dir.assign(mysql_real_data_home_ptr);
      } else
        s_log_dir.assign(log_dir);

      THD *thd= current_thd;
      err= RUN_HOOK_STRICT(raft_replication, register_paths,
                           (thd, server_uuid, s_wal_dir, s_log_dir,
                            log_bin_basename, glob_hostname, 
                            (uint64_t)mysqld_port));
      break;
    }
    default:
      return -1;
  };
  return err;
}

/**
  Flush and commit the transaction.

  This will execute an ordered flush and commit of all outstanding
  transactions and is the main function for the binary log group
  commit logic. The function performs the ordered commit in two
  phases.

  The first phase flushes the caches to the binary log and under
  LOCK_log and marks all threads that were flushed as not pending.

  The second phase executes under LOCK_commit and commits all
  transactions in order.

  The procedure is:

  1. Queue ourselves for flushing.
  2. Grab the log lock, which might result is blocking if the mutex is
     already held by another thread.
  3. If we were not committed while waiting for the lock
     1. Fetch the queue
     2. For each thread in the queue:
        a. Attach to it
        b. Flush the caches, saving any error code
     3. Flush and sync (depending on the value of sync_binlog).
     4. Signal that the binary log was updated
  4. Release the log lock
  5. Grab the commit lock
     1. For each thread in the queue:
        a. If there were no error when flushing and the transaction shall be committed:
           - Commit the transaction, saving the result of executing the commit.
  6. Release the commit lock
  7. Call purge, if any of the committed thread requested a purge.
  8. Return with the saved error code

  @todo The use of @c skip_commit is a hack that we use since the @c
  TC_LOG Interface does not contain functions to handle
  savepoints. Once the binary log is eliminated as a handlerton and
  the @c TC_LOG interface is extended with savepoint handling, this
  parameter can be removed.

  @param thd Session to commit transaction for
  @param all   This is @c true if this is a real transaction commit, and
               @c false otherwise.
  @param skip_commit
               This is @c true if the call to @c ha_commit_low should
               be skipped (it is handled by the caller somehow) and @c
               false otherwise (the normal case).
 */
int MYSQL_BIN_LOG::ordered_commit(THD *thd, bool all, bool skip_commit,
                                  bool async)
{
  DBUG_ENTER("MYSQL_BIN_LOG::ordered_commit");
  int flush_stage_error= 0, flush_error= 0, sync_error= 0;
  my_off_t total_bytes= 0;
  bool do_rotate= false;
  THD *semisync_queue= nullptr;

  /*
    These values are used while flushing a transaction, so clear
    everything.

    Notes:

    - It would be good if we could keep transaction coordinator
      log-specific data out of the THD structure, but that is not the
      case right now.

    - Everything in the transaction structure is reset when calling
      ha_commit_low since that calls st_transaction::cleanup.
  */
  thd->transaction.flags.pending= true;
  thd->commit_error= THD::CE_NONE;
  thd->next_to_commit= NULL;
  thd->durability_property= HA_IGNORE_DURABILITY;
  thd->transaction.flags.real_commit= all;
  thd->transaction.flags.xid_written= false;
  thd->transaction.flags.commit_low= !skip_commit;
  thd->transaction.flags.run_hooks= !skip_commit;
  thd->gtid_precommit= (opt_gtid_precommit &&
              (thd->variables.gtid_next.type == AUTOMATIC_GROUP));
#ifndef DBUG_OFF
  /*
     The group commit Leader may have to wait for follower whose transaction
     is not ready to be preempted. Initially the status is pessimistic.
     Preemption guarding logics is necessary only when DBUG_ON is set.
     It won't be required for the dbug-off case as long as the follower won't
     execute any thread-specific write access code in this method, which is
     the case as of current.
  */
  thd->transaction.flags.ready_preempt= 0;
#endif

  DBUG_PRINT("enter", ("flags.pending: %s, commit_error: %d, thread_id: %u",
                       YESNO(thd->transaction.flags.pending),
                       thd->commit_error, thd->thread_id()));

  /*
    Stage #1: flushing transactions to binary log

    While flushing, we allow new threads to enter and will process
    them in due time. Once the queue was empty, we cannot reap
    anything more since it is possible that a thread entered and
    appointed itself leader for the flush phase.
  */
  DEBUG_SYNC(thd, "waiting_to_enter_flush_stage");
#ifdef HAVE_REPLICATION
  if (has_commit_order_manager(thd))
  {
    Slave_worker *worker= dynamic_cast<Slave_worker *>(thd->rli_slave);
    Commit_order_manager *mngr= worker->get_commit_order_manager();

    if (mngr->wait_for_its_turn(worker, all))
    {
      thd->commit_error= THD::CE_COMMIT_ERROR;
      DBUG_RETURN(thd->commit_error);
    }

    if (change_stage(thd, Stage_manager::FLUSH_STAGE, thd, NULL, &LOCK_log))
      DBUG_RETURN(finish_commit(thd, async));
  }
  else
#endif
  if (change_stage(thd, Stage_manager::FLUSH_STAGE, thd, NULL, &LOCK_log))
  {
    DBUG_PRINT("return", ("Thread ID: %u, commit_error: %d",
                          thd->thread_id(), thd->commit_error));
    DBUG_RETURN(finish_commit(thd, async));
  }

  enable_raft_plugin_save= enable_raft_plugin;

  check_and_register_log_entities(thd);

  THD *final_queue= NULL;
  mysql_mutex_t *leave_mutex_before_commit_stage= NULL;
  my_off_t flush_end_pos= 0;
  if (unlikely(!is_open()))
  {
    final_queue= stage_manager.fetch_queue_for(Stage_manager::FLUSH_STAGE);
    leave_mutex_before_commit_stage= &LOCK_log;
    /*
      binary log is closed, flush stage and sync stage should be
      ignored. Binlog cache should be cleared, but instead of doing
      it here, do that work in 'finish_commit' function so that
      leader and followers thread caches will be cleared.
    */
    goto commit_stage;
  }
  DEBUG_SYNC(thd, "waiting_in_the_middle_of_flush_stage");
  flush_stage_error= process_flush_stage_queue(&total_bytes, &do_rotate,
                                         &final_queue, async);

  /* If there was something written to log_file during flush stage, then flush
   * the entire group to file. Note that flush_stage_error indicates that some
   * thd in the group failed to write to log_file, but remaining threads might
   * still have written
   */
  if (total_bytes > 0)
    flush_error= flush_cache_to_file(&flush_end_pos);

  DBUG_EXECUTE_IF("crash_after_flush_binlog", DBUG_SUICIDE(););
  /*
    If the flush finished successfully, we can call the after_flush
    hook. Being invoked here, we have the guarantee that the hook is
    executed before the before/after_send_hooks on the dump thread
    preventing race conditions among these plug-ins.
  */
  if (flush_error == 0 && flush_end_pos != 0)
  {
    const char *file_name_ptr= log_file_name + dirname_length(log_file_name);
    DBUG_ASSERT(flush_end_pos != 0);

    // semi-sync to be called only when raft is not enabled
    if (!enable_raft_plugin &&
        RUN_HOOK(binlog_storage, after_flush,
                 (thd, file_name_ptr, flush_end_pos)))
    {
      sql_print_error("Failed to run 'after_flush' hooks");
      flush_error= ER_ERROR_ON_WRITE;
    }

    /*
      Stage #2: Syncing binary log file to disk
    */
    if (total_bytes > 0)
    {
      DEBUG_SYNC(thd, "before_sync_binlog_file");
      std::pair<bool, bool> result = sync_binlog_file(false, async);
      flush_error = result.first;
    }

    /*
      Update the last valid position after the after_flush hook has
      executed. Doing so guarantees that the hook is executed before
      the before/after_send_hooks on the dump thread, preventing race
      conditions between the group_commit here and the dump threads.
    */
    /*
      Update the binlog end position only after binlog fsync. Doing so
      guarantees that slave's don't up with some transactions
      that haven't made it to the disk on master because of a os
      crash or power failure just before binlog fsync.
    */
    update_binlog_end_pos();

    DBUG_EXECUTE_IF("crash_commit_after_log", DBUG_SUICIDE(););
  }

  /* simulate a write failure during commit - needed for unit test */
  DBUG_EXECUTE_IF("abort_with_io_write_error", flush_error=ER_ERROR_ON_WRITE;);

  /* skip dumping core if write failed and we are allowed to do so */
  if (flush_error == ER_ERROR_ON_WRITE && skip_core_dump_on_error)
    opt_core_file = false;

  if (flush_error || flush_stage_error)
  {
    /*
      Handle flush error (if any) after leader finishes it's flush stage.
    */
    handle_binlog_flush_or_sync_error(thd, false /* need_lock_log */);
  }

commit_stage:
  if (change_stage(thd, Stage_manager::SEMISYNC_STAGE, final_queue,
                   &LOCK_log, &LOCK_semisync))
  {
    DBUG_PRINT("return", ("Thread ID: %u, commit_error: %d",
                          thd->thread_id(), thd->commit_error));
    DBUG_RETURN(finish_commit(thd, async));
  }
  semisync_queue =
    stage_manager.fetch_queue_for(Stage_manager::SEMISYNC_STAGE);

  // The delay is very long to allow large write batches in stress tests.
  DBUG_EXECUTE_IF("emulate_async_delay", usleep(500000););

  ulonglong start_time;
  start_time = my_timer_now();
  process_semisync_stage_queue(semisync_queue);
  thd->semisync_ack_time = my_timer_since(start_time);

  leave_mutex_before_commit_stage= &LOCK_semisync;

  /*
    Stage #3: Commit all transactions in order.

    This stage is skipped if we do not need to order the commits and
    each thread have to execute the handlerton commit instead.

    Howver, since we are keeping the lock from the previous stage, we
    need to unlock it if we skip the stage.
   */
  /*
    We are delaying the handling of sync error until
    all locks are released but we should not enter into
    commit stage if binlog_error_action is ABORT_SERVER.
  */
  if (opt_binlog_order_commits &&
      (sync_error == 0 || binlog_error_action != ABORT_SERVER))
  {
    if (change_stage(thd, Stage_manager::COMMIT_STAGE,
                     semisync_queue, leave_mutex_before_commit_stage, &LOCK_commit))
    {
      DBUG_PRINT("return", ("Thread ID: %u, commit_error: %d",
                            thd->thread_id(), thd->commit_error));
      DBUG_RETURN(finish_commit(thd, async));
    }
    THD *commit_queue= stage_manager.fetch_queue_for(Stage_manager::COMMIT_STAGE);
    start_time = my_timer_now();
    process_commit_stage_queue(thd, commit_queue, async);
    thd->engine_commit_time = my_timer_since(start_time);
    mysql_mutex_unlock(&LOCK_commit);
    /*
      Process after_commit after LOCK_commit is released for avoiding
      3-way deadlock among user thread, rotate thread and dump thread.
    */
    process_after_commit_stage_queue(thd, commit_queue, async);
    final_queue= commit_queue;
  }
  else if (leave_mutex_before_commit_stage)
    mysql_mutex_unlock(leave_mutex_before_commit_stage);

  /*
    Handle sync error after we release all locks in order to avoid deadlocks
  */
  if (sync_error)
    handle_binlog_flush_or_sync_error(thd, true /* need_lock_log */);

  /* Commit done so signal all waiting threads */
  stage_manager.signal_done(final_queue);

  /*
    Finish the commit before executing a rotate, or run the risk of a
    deadlock. We don't need the return value here since it is in
    thd->commit_error, which is returned below.
  */
  (void) finish_commit(thd, async);

  /*
    If we need to rotate, we do it without commit error.
    Otherwise the thd->commit_error will be possibly reset.
   */
  if (DBUG_EVALUATE_IF("force_rotate", 1, 0) ||
      (do_rotate && thd->commit_error == THD::CE_NONE))
  {
    /*
      Do not force the rotate as several consecutive groups may
      request unnecessary rotations.

      NOTE: Run purge_logs wo/ holding LOCK_log because it does not
      need the mutex. Otherwise causes various deadlocks.
    */

    DEBUG_SYNC(thd, "ready_to_do_rotation");
    bool check_purge= false;
    mysql_mutex_lock(&LOCK_log);
    /*
      If rotate fails then depends on binlog_error_action variable
      appropriate action will be taken inside rotate call.
    */
    int error= rotate(false, &check_purge);
    mysql_mutex_unlock(&LOCK_log);

    if (error)
      thd->commit_error= THD::CE_COMMIT_ERROR;
    else if (check_purge)
      purge();
  }
  /*
    flush or sync errors are handled above (using binlog_error_action).
    Hence treat only COMMIT_ERRORs as errors.
  */
  DBUG_RETURN(thd->commit_error == THD::CE_COMMIT_ERROR);
}

/**
 * Recover raft log. This is primarily for relay logs in the raft world since
 * trx logs (binary logs or apply logs) are already recovered by mysqld as part
 * of trx log recovery. This method tries to get rid of partial trxs in the tal
 * of the raft log. Much has been borrowed from
 * MYSQL_BIN_LOG::open_binlog(const char *opt_name) and
 * MYSQL_BIN_LOG::recover(...). Refactoring the components is rather hard and
 * adds unnecessary complexity with additional params and if() {} else {}
 * branches. Hence a separate method.
 */
int MYSQL_BIN_LOG::recover_raft_log()
{
  int error= 0;
  const char *errmsg;
  IO_CACHE log;
  File file;
  Log_event *ev= 0;
  Format_description_log_event fdle(BINLOG_VERSION);
  Format_description_log_event *fdle_ev= 0;
  bool in_operation= FALSE;
  char log_name[FN_REFLEN];
  my_off_t valid_pos= 0;
  my_off_t binlog_size= 0;
  MY_STAT s;
  LOG_INFO log_info;
  bool pending_gtid= FALSE;
  bool pending_metadata= FALSE;
  bool first_metadata_seen= FALSE;
  std::string error_message;
  int status= 0;

  if (!mysql_bin_log.is_apply_log)
    goto err; // raft log already recovered as part of trx log recovery

  if (!fdle.is_valid())
  {
    error= 1;
    error_message= "BINLOG_VERSION is not valid for format descriptor";
    goto err;
  }

  if (!my_b_inited(&index_file))
  {
    error_message= "Index file is not inited in recover_raft_log";
    error= 1;
    goto err;
  }

  if ((status= find_log_pos(&log_info, NullS, true/*need_lock_index=true*/)))
  {
    if (status != LOG_INFO_EOF)
    {
      error_message= "find_log_pos() failed in recover_raft_log with error: "
        + std::to_string(error);
      error= 1;
    }
    goto err;
  }

  do
  {
    strmake(log_name, log_info.log_file_name, sizeof(log_name)-1);
  } while (!(status= find_next_log(&log_info, true/*need_lock_index=true*/)));

  if (status != LOG_INFO_EOF)
  {
    error_message= "find_log_pos() failed in recover_raft_log with error: "
      + std::to_string(error);
    error= 1;
    goto err;
  }

  if ((file= open_binlog_file(&log, log_name, &errmsg)) < 0)
  {
    error= 1;
    error_message= "open_binlog_file() failed in recover_raft_log with " +
        std::string(errmsg);
    goto err;
  }

  // Get the current size of the file
  my_stat(log_name, &s, MYF(0));
  binlog_size= s.st_size;

  // Get the format description event from relay log
  if ((ev= Log_event::read_log_event(&log, 0, &fdle,
                                     opt_master_verify_checksum, NULL)))
  {
    if (ev->get_type_code() == FORMAT_DESCRIPTION_EVENT)
      fdle_ev= (Format_description_log_event *)ev;
    else
    {
      error_message= "Could not find format description event in raft log " +
        std::string(log_name);
      error= 1;
      goto err;
    }
  }
  else
  {
    error_message= "Could not read from raft log " + std::string(log_name);
    error= 1;
    goto err;
  }

  // This logic is borrowed from MYSQL_BIN_LOG::recover() which has to do
  // additional things and refactoring it will simply add more branches. Hence
  // the code duplication
  while ((ev= Log_event::read_log_event(&log, 0, fdle_ev, TRUE, NULL))
         && ev->is_valid())
  {
    if (ev->get_type_code() == QUERY_EVENT &&
        !strcmp(((Query_log_event*)ev)->query, "BEGIN"))
    {
      pending_metadata= FALSE;
      in_operation= TRUE;
    }
    else if (is_gtid_event(ev))
    {
      pending_gtid= TRUE;
      pending_metadata= FALSE;
    }
    else if (ev->get_type_code() == XID_EVENT ||
        (ev->get_type_code() == QUERY_EVENT &&
         !strcmp(((Query_log_event*)ev)->query, "COMMIT")))
    {
      if (!in_operation)
      {
        // When we see a commit message, we should already be parsing a valid
        // transaction
        error_message= "Saw a XID/COMMIT event without a begin. Corrupted log: "
          + std::string(log_name);
        error= 1;
        delete ev;
        break;
      }
      in_operation= FALSE;
      pending_metadata= FALSE;
    }
    else if (ev->get_type_code() == METADATA_EVENT && !pending_gtid)
    {
      if (first_metadata_seen)
        pending_metadata= FALSE;
      else
        first_metadata_seen= TRUE;
    }
    else if (ev->get_type_code() == ROTATE_EVENT)
    {
      pending_metadata= FALSE;
    }


    if (!(ev->get_type_code() == METADATA_EVENT && pending_gtid))
    {
      if (!log.error && !in_operation && !is_gtid_event(ev) &&
          !pending_metadata)
      {
        valid_pos= my_b_tell(&log);
        pending_gtid= FALSE;
      }
    }

    delete ev;
  }

  delete fdle_ev;
  end_io_cache(&log);
  mysql_file_close(file, MYF(MY_WME));

  // No partial trxs found in the raft log or error parsing the log
  if (error || (valid_pos == 0 || valid_pos >= binlog_size))
    goto err;

  // NO_LINT_DEBUG
  sql_print_information("Raft log %s with a size of %llu will be trimmed to "
      "%llu bytes based on valid transactions in the file",
      log_name, binlog_size, valid_pos);

  if ((file= mysql_file_open(key_file_relaylog, log_name,
                             O_RDWR | O_BINARY, MYF(MY_WME))) < 0)
  {
    error_message= "Failed to remove partial transactions from raft log file "
      + std::string(log_name);
    error= 1;
    goto err;
  }

  if (my_chsize(file, valid_pos, 0, MYF(MY_WME)))
  {
    error_message= "Failed to remove partial transactions from raft log file "
      + std::string(log_name);
    error= 1;
  }

  mysql_file_close(file, MYF(MY_WME));

err:
  if (error && !error_message.empty())
    // NO_LINT_DEBUG
    sql_print_error("%s", error_message.c_str());

  return error;
}

/**
  MYSQLD server recovers from last crashed binlog.

  @param log           IO_CACHE of the crashed binlog.
  @param fdle          Format_description_log_event of the crashed binlog.
  @param valid_pos     The position of the last valid transaction or
                       event(non-transaction) of the crashed binlog.
  @param cur_binlog_file The current binlog file that is being recovered

  @retval
    0                  ok
  @retval
    1                  error
*/
int MYSQL_BIN_LOG::recover(IO_CACHE *log, Format_description_log_event *fdle,
                            my_off_t *valid_pos,
                            const std::string& cur_binlog_file)
{
  Log_event  *ev;
  HASH xids;
  MEM_ROOT mem_root;
  /*
    Prepared transasctions are committed by XID during recovery but we need
    to track the max GTID so we maintain a map from XID to GTID and update
    the max GTID after committing by XID
  */
  XID_TO_GTID xid_to_gtid;
  /*
    The flag is used for handling the case that a transaction
    is partially written to the binlog.
  */
  bool in_transaction= FALSE;

  /*
   * Flag to indicate if we have seen a gtid which is pending i.e the trx
   * represented by this gtid has not yet ended
   */
  bool pending_gtid= FALSE;

  /*
   * Flag to indicate if we have seen a metadata which is pending i.e the
   * operation represented by this metadata has not yet ended. This is here to
   * handle the scenario of a metadata added before a rotate event in raft
   */
  bool pending_metadata= FALSE;
  bool first_metadata_seen= FALSE;

  my_off_t first_gtid_start= 0;

  if (! fdle->is_valid() ||
      my_hash_init(&xids, &my_charset_bin, TC_LOG_PAGE_SIZE/3, 0,
                   sizeof(my_xid), 0, 0, MYF(0)))
    goto err1;

  init_alloc_root(&mem_root, TC_LOG_PAGE_SIZE, TC_LOG_PAGE_SIZE);

  while ((ev= Log_event::read_log_event(log, 0, fdle, TRUE, NULL))
         && ev->is_valid())
  {
    if (ev->get_type_code() == QUERY_EVENT &&
        !strcmp(((Query_log_event*)ev)->query, "BEGIN"))
    {
      pending_metadata= FALSE;
      in_transaction= TRUE;
    }
    else if (ev->get_type_code() == QUERY_EVENT &&
        !strcmp(((Query_log_event*)ev)->query, "COMMIT"))
    {
      DBUG_ASSERT(in_transaction == TRUE);
      pending_metadata= FALSE;
      in_transaction= FALSE;
    }
    else if (is_gtid_event(ev))
    {
      xid_to_gtid.gtid.set(((Gtid_log_event*) ev)->get_sidno(true),
                           ((Gtid_log_event*) ev)->get_gno());

      pending_gtid= TRUE;
      pending_metadata= FALSE;
      if (first_gtid_start == 0)
      {
        first_gtid_start= ev->log_pos - ev->data_written;
        this->first_gtid_start_pos= first_gtid_start;
      }
    }
    else if (ev->get_type_code() == XID_EVENT)
    {
      DBUG_ASSERT(in_transaction == TRUE);
      in_transaction= FALSE;
      pending_metadata= FALSE;
      Xid_log_event *xev=(Xid_log_event *)ev;
      xid_to_gtid.x= xev->xid;
      uchar *x= (uchar *) memdup_root(&mem_root, (uchar*) &xid_to_gtid,
                                      sizeof(xid_to_gtid));
      if (!x || my_hash_insert(&xids, x))
        goto err2;
    }
    else if (ev->get_type_code() == METADATA_EVENT && !pending_gtid)
    {
      if (first_metadata_seen)
        pending_metadata= FALSE;
      else
        first_metadata_seen= TRUE;
    }
    else if (ev->get_type_code() == ROTATE_EVENT)
    {
      pending_metadata= FALSE;
    }

    /*
      Recorded valid position for the crashed binlog file
      which did not contain incorrect events. The following
      positions increase the variable valid_pos:

      1 -
        ...
        <---> HERE IS VALID <--->
        GTID
        BEGIN
        ...
        COMMIT
        ...

      2 -
        ...
        <---> HERE IS VALID <--->
        GTID
        DDL/UTILITY
        ...

      In other words, the following positions do not increase
      the variable valid_pos:

      1 -
        GTID
        <---> HERE IS VALID <--->
        ...

      2 -
        GTID
        BEGIN
        <---> HERE IS VALID <--->
        ...

        In addition to the above, there is special handling for metadata event
        added by after gtid event and/or before rotate event
    */
    if (!(ev->get_type_code() == METADATA_EVENT && pending_gtid))
    {
      if (!log->error && !in_transaction && !is_gtid_event(ev) &&
          !pending_metadata)
      {
        *valid_pos= my_b_tell(log);
        pending_gtid= FALSE;
      }
    }

    delete ev;
  }

  if (ha_recover(&xids, engine_binlog_file, &engine_binlog_pos,
                 &engine_binlog_max_gtid))
    goto err2;

  /* If trim binlog on recover option is set, then we essentially trim
     binlog to the position that the engine thinks it has committed. Note
     that if opt_trim_binlog option is set, then engine recovery (called
     through ha_recover() above) ensures that all prepared txns are rolled
     back. There are a few things which need to be kept in mind:
     1. txns never span across two binlogs, hence it is safe to recover only
        the latest binlog file.
     2. A binlog rotation ensures that the previous binlogs and engine's
        transaction logs are flushed and made durable. Hence all previous
        transactions are made durable.

     If enable_raft_plugin is set, then we skip trimming binlog. This is because
     the trim is handled inside the raft plugin when this node rejoins the raft
     ring
  */
  if (opt_trim_binlog && !enable_raft_plugin)
  {
    if (set_valid_pos(valid_pos, cur_binlog_file, first_gtid_start))
      goto err2;
  }

  free_root(&mem_root, MYF(0));
  my_hash_free(&xids);
  return 0;

err2:
  free_root(&mem_root, MYF(0));
  my_hash_free(&xids);
err1:
  sql_print_error("Crash recovery failed. Either correct the problem "
                  "(if it's, for example, out of memory error) and restart, "
                  "or delete (or rename) binary log and start mysqld with "
                  "--tc-heuristic-recover={commit|rollback}");
  return 1;
}

/*
 * Sets the valid position in the binlog file based on engine position (i.e
 * engine binlog filename and file position)
 *
 * @param - valid_pos[out] - Valid position to set
 * @param - cur_binlog_file - The current binlog file that is being recovered
 * @param - first_gtid_start - The starting position of the first gtid event in
 * cur_binlog_file
 *
 * @return 0 on success, 1 on error
 */
int MYSQL_BIN_LOG::set_valid_pos(
    my_off_t* valid_pos,
    const std::string& cur_binlog_file,
    my_off_t first_gtid_start)
{
  int error= 0;
  std::string position=
    "Engine pos: " + std::to_string(engine_binlog_pos) +
    ", Current binlog pos: " + std::to_string(*valid_pos) +
    ", Engine binlog file: " + engine_binlog_file +
    ", Current binlog file: " + cur_binlog_file;
  // NO_LINT_DEBUG
  sql_print_information("%s", position.c_str());

  std::string info_msg=
    "Engine is ahead of binlog. Binlog will not be truncated to match engine.";

  if (cur_binlog_file.compare(engine_binlog_file) == 0)
  {
    // Case 1: Engine binlog file and current binlog files are the same.
    // Compare based only on file position
    if (*valid_pos > engine_binlog_pos)
    {
      // Binlog will be truncated to this position
      *valid_pos = engine_binlog_pos;
    }
    else if (*valid_pos < engine_binlog_pos)
    {
      // Engine is found to be ahead of the current binlog
      // NO_LINT_DEBUG
      sql_print_information("%s",info_msg.c_str());
    }
  }
  else
  {
    // Case 2: Engine and binlog file names are different. Compare based on file
    // indexes.
    const auto engine_file_pair= extract_file_index(engine_binlog_file);
    const auto cur_file_pair= extract_file_index(cur_binlog_file);

    if (engine_file_pair.first.compare(cur_file_pair.first) != 0)
    {
      // The file prefix stored in engine is different than the current file
      // prefix. We cannot trim. So give up. Note that server will fail to start
      // in this case
      // NO_LINT_DEBUG
      sql_print_information("The file prefix in engine does not match "
                            "the file prefix of the recovering binlog. There "
                            "will be no special trimming of the file");
    }
    else if (engine_file_pair.second < cur_file_pair.second)
    {
      // Engine file is lower than current binlog file. Truncate to the begin
      // position of the first gtid in the current binlog file
      *valid_pos= first_gtid_start;
    }
    else
    {
      // Engine is found to be ahead of the current binlog
      // NO_LINT_DEBUG
      sql_print_information("%s",info_msg.c_str());
    }
  }

  return error;
}

Dump_log::Dump_log()
{
#ifdef HAVE_REPLICATION
  if (enable_raft_plugin && mysql_bin_log.is_apply_log)
    log_= &active_mi->rli->relay_log;
  else
#endif
    log_= &mysql_bin_log;
}

void Dump_log::switch_log(bool relay_log, bool should_lock)
{
#ifdef HAVE_REPLICATION
  if (should_lock) log_mutex_.lock();
  mysql_mutex_assert_owner(&log_->LOCK_binlog_end_pos);
  log_->update_binlog_end_pos(/* need_lock= */false);
  DBUG_ASSERT(active_mi && active_mi->rli);
  sql_print_information(
      "Switching dump log to %s", relay_log ? "relay log" : "binlog");
  log_= relay_log ? &active_mi->rli->relay_log : &mysql_bin_log;

  // Now let's update the dump thread's linfos
  mutex_lock_all_shards(SHARDED(&LOCK_thread_count));
  Thread_iterator it= global_thread_list_begin();
  Thread_iterator end= global_thread_list_end();
  for (; it != end; ++it)
  {
    LOG_INFO* linfo = (*it)->current_linfo;
    // Case: this is a dump thread's linfo
    if (linfo && linfo->is_used_by_dump_thd)
    {
      mysql_mutex_lock(&linfo->lock);
      linfo->is_relay_log= relay_log;
      mysql_mutex_unlock(&linfo->lock);
    }
  }
  mutex_unlock_all_shards(SHARDED(&LOCK_thread_count));
  if (should_lock) log_mutex_.unlock();
#endif
}

// Given a file name of the form 'binlog-file-name.index', it extracts the
// <binlog-file-name> and <index> and returns it as a pair
// Example:
// master-bin-3306.0001 ==> Returns (master-bin-3306, 1)
// master-bin-3306.9999 ==> Returns (master-bin-3306, 9999)
std::pair<std::string, uint> MYSQL_BIN_LOG::extract_file_index(
    const std::string& file_name)
{
  char* end;
  uint pos= file_name.find_last_of('.');
  std::string prefix= file_name.substr(0, pos);
  uint index= std::strtoul(file_name.substr(pos + 1).c_str(), &end, 10);

  return std::make_pair(std::move(prefix), index);
}

Group_cache *THD::get_group_cache(bool is_transactional)
{
  DBUG_ENTER("THD::get_group_cache(bool)");

  // If opt_bin_log==0, it is not safe to call thd_get_cache_mngr
  // because binlog_hton has not been completely set up.
  DBUG_ASSERT(opt_bin_log);
  binlog_cache_mngr *cache_mngr= thd_get_cache_mngr(this);

  // cache_mngr is NULL until we call thd->binlog_setup_trx_data, so
  // we assert that this has been done.
  DBUG_ASSERT(cache_mngr != NULL);

  binlog_cache_data *cache_data=
    cache_mngr->get_binlog_cache_data(is_transactional);
  DBUG_ASSERT(cache_data != NULL);

  DBUG_RETURN(&cache_data->group_cache);
}

/*
  These functions are placed in this file since they need access to
  binlog_hton, which has internal linkage.
*/

int THD::binlog_setup_trx_data()
{
  DBUG_ENTER("THD::binlog_setup_trx_data");
  binlog_cache_mngr *cache_mngr= thd_get_cache_mngr(this);

  if (cache_mngr)
    DBUG_RETURN(0);                             // Already set up

  cache_mngr= (binlog_cache_mngr*) my_malloc(sizeof(binlog_cache_mngr), MYF(MY_ZEROFILL));
  if (!cache_mngr ||
      open_cached_file(&cache_mngr->stmt_cache.cache_log, mysql_tmpdir,
                       LOG_PREFIX, binlog_stmt_cache_size,
                       MYF(MY_WME | MY_WAIT_IF_FULL)) ||
      open_cached_file(&cache_mngr->trx_cache.cache_log, mysql_tmpdir,
                       LOG_PREFIX, binlog_cache_size,
                       MYF(MY_WME | MY_WAIT_IF_FULL))) {
    my_free(cache_mngr);
    DBUG_RETURN(1);                      // Didn't manage to set it up
  }
  DBUG_PRINT("debug", ("Set ha_data slot %d to 0x%llx", binlog_hton->slot, (ulonglong) cache_mngr));
  thd_set_ha_data(this, binlog_hton, cache_mngr);

  cache_mngr= new (thd_get_cache_mngr(this))
              binlog_cache_mngr(max_binlog_stmt_cache_size,
                                &binlog_stmt_cache_use,
                                &binlog_stmt_cache_disk_use,
                                max_binlog_cache_size,
                                &binlog_cache_use,
                                &binlog_cache_disk_use);
  DBUG_RETURN(0);
}

/**

*/
void register_binlog_handler(THD *thd, bool trx)
{
  DBUG_ENTER("register_binlog_handler");
  /*
    If this is the first call to this function while processing a statement,
    the transactional cache does not have a savepoint defined. So, in what
    follows:
      . an implicit savepoint is defined;
      . callbacks are registered;
      . binary log is set as read/write.

    The savepoint allows for truncating the trx-cache transactional changes
    fail. Callbacks are necessary to flush caches upon committing or rolling
    back a statement or a transaction. However, notifications do not happen
    if the binary log is set as read/write.
  */
  binlog_cache_mngr *cache_mngr= thd_get_cache_mngr(thd);
  if (cache_mngr->trx_cache.get_prev_position() == MY_OFF_T_UNDEF)
  {
    /*
      Set an implicit savepoint in order to be able to truncate a trx-cache.
    */
    my_off_t pos= 0;
    binlog_trans_log_savepos(thd, &pos);
    cache_mngr->trx_cache.set_prev_position(pos);

    /*
      Set callbacks in order to be able to call commmit or rollback.
    */
    if (trx)
      trans_register_ha(thd, TRUE, binlog_hton);
    trans_register_ha(thd, FALSE, binlog_hton);

    /*
      Set the binary log as read/write otherwise callbacks are not called.
    */
    thd->ha_data[binlog_hton->slot].ha_info[0].set_trx_read_write();
  }
  DBUG_VOID_RETURN;
}

/**
  Function to start a statement and optionally a transaction for the
  binary log.

  This function does three things:
    - Starts a transaction if not in autocommit mode or if a BEGIN
      statement has been seen.

    - Start a statement transaction to allow us to truncate the cache.

    - Save the currrent binlog position so that we can roll back the
      statement by truncating the cache.

      We only update the saved position if the old one was undefined,
      the reason is that there are some cases (e.g., for CREATE-SELECT)
      where the position is saved twice (e.g., both in
      select_create::prepare() and THD::binlog_write_table_map()) , but
      we should use the first. This means that calls to this function
      can be used to start the statement before the first table map
      event, to include some extra events.

  Note however that IMMEDIATE_LOGGING implies that the statement is
  written without BEGIN/COMMIT.

  @param thd         Thread variable
  @param start_event The first event requested to be written into the
                     binary log
 */
static int binlog_start_trans_and_stmt(THD *thd, Log_event *start_event)
{
  DBUG_ENTER("binlog_start_trans_and_stmt");

  /*
    Initialize the cache manager if this was not done yet.
  */
  if (thd->binlog_setup_trx_data())
    DBUG_RETURN(1);

  /*
    Retrieve the appropriated cache.
  */
  bool is_transactional= start_event->is_using_trans_cache();
  binlog_cache_mngr *cache_mngr= thd_get_cache_mngr(thd);
  binlog_cache_data *cache_data= cache_mngr->get_binlog_cache_data(is_transactional);

  /*
    If the event is requesting immediatly logging, there is no need to go
    further down and set savepoint and register callbacks.
  */
  if (start_event->is_using_immediate_logging())
  {
    thd->should_write_gtid = should_write_gtids(thd);
    DBUG_RETURN(0);
  }

  register_binlog_handler(thd, thd->in_multi_stmt_transaction_mode());

  /*
    If the cache is empty log "BEGIN" at the beginning of every transaction.
    Here, a transaction is either a BEGIN..COMMIT/ROLLBACK block or a single
    statement in autocommit mode.
  */
  if (cache_data->is_binlog_empty())
  {
    /*
      save the value of should_write_gtids() during binlog cache initialization
      since read_only may get changed in the middle of transaction which gives
      different results from should_write_gtids() causing inconsistent binlog
      data.
    */
    thd->should_write_gtid = should_write_gtids(thd);
    Query_log_event qinfo(thd, STRING_WITH_LEN("BEGIN"),
                          is_transactional, FALSE, TRUE, 0, TRUE);
    if (cache_data->write_event(thd, &qinfo))
      DBUG_RETURN(1);
  }

  DBUG_RETURN(0);
}

#ifdef HAVE_RAPIDJSON
/**
  This function generated meta data in JSON format as a comment in a rows query
  event.

  @see binlog_trx_meta_data

  @return JSON if all good, null string otherwise
*/
std::string THD::gen_trx_metadata()
{
  DBUG_ENTER("THD::gen_trx_metadata");
  DBUG_ASSERT(opt_binlog_trx_meta_data);

  rapidjson::Document doc;
  doc.SetObject();

  // case: read existing meta data received from the master
  if (rli_slave && !rli_slave->trx_meta_data_json.empty())
  {
     if (doc.Parse(rli_slave->trx_meta_data_json.c_str()).HasParseError())
     {
      // NO_LINT_DEBUG
      sql_print_error("Exception while reading meta data: %s",
                       rli_slave->trx_meta_data_json.c_str());
      DBUG_RETURN("");
    }
    // clear existing data
    rli_slave->trx_meta_data_json.clear();
  }

  // add things to the meta data
  if (!add_db_metadata(doc) || !add_time_metadata(doc))
  {
    // NO_LINT_DEBUG
    sql_print_error("Exception while adding meta data");
    DBUG_RETURN("");
  }

  // write meta data with new stuff in the binlog
  rapidjson::StringBuffer buf;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
  if (!doc.Accept(writer))
  {
    // NO_LINT_DEBUG
    sql_print_error("Error while writing meta data");
    DBUG_RETURN("");
  }
  std::string json = buf.GetString();
  boost::trim(json);

  // verify doc json document
  if (!doc.IsObject())
  {
      // NO_LINT_DEBUG
      sql_print_error("Bad JSON format after adding meta data: %s",
                      json.c_str());
      DBUG_RETURN("");
  }
  std::string comment_str= std::string("/*")
                           .append(TRX_META_DATA_HEADER)
                           .append(json)
                           .append("*/");

  DBUG_RETURN(comment_str);
}

/**
  This function adds timing information in meta data JSON of rows query event.

  @see THD::write_trx_metadata

  @param meta_data_root Property tree object which represents the JSON
  @return true if all good, false if error
*/
bool THD::add_time_metadata(rapidjson::Document &meta_data_root)
{
  DBUG_ENTER("THD::add_time_metadata");
  DBUG_ASSERT(opt_binlog_trx_meta_data);

  rapidjson::Document::AllocatorType& allocator = meta_data_root.GetAllocator();

  // get existing timestamps
  auto times= meta_data_root.FindMember("ts");
  if (times == meta_data_root.MemberEnd())
  {
    meta_data_root.AddMember("ts", rapidjson::Value().SetArray(), allocator);
    times= meta_data_root.FindMember("ts");
  }

  // add our timestamp to the array
  std::string millis=
    std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>
        (std::chrono::system_clock::now().time_since_epoch()).count());
  times->value.PushBack(
      rapidjson::Value().SetString(millis.c_str(), millis.size(), allocator),
      allocator);

  DBUG_RETURN(true);
}

bool THD::add_db_metadata(rapidjson::Document &meta_data_root)
{
  DBUG_ENTER("THD::add_db_meta_data");
  DBUG_ASSERT(opt_binlog_trx_meta_data);

  mysql_mutex_lock(&LOCK_db_metadata);
  std::string local_db_metadata= db_metadata;
  mysql_mutex_unlock(&LOCK_db_metadata);

  if (!local_db_metadata.empty())
  {
    rapidjson::Document db_metadata_root;
    // rapidjson doesn't like calling GetObject() on json non-object value
    // The local_db_metadata format should similar to the following example:
    // {"shard":"<shard_name>", "replicaset":"<replicaset_id>"}
    if (db_metadata_root.Parse(local_db_metadata.c_str()).HasParseError() ||
        !db_metadata_root.IsObject())
    {
      // NO_LINT_DEBUG
      sql_print_error("Exception while reading meta data: %s",
                       local_db_metadata.c_str());
      DBUG_RETURN(false);
    }

    // flatten DB metadata into trx metadata
    auto& allocator= meta_data_root.GetAllocator();
    for (auto& node : db_metadata_root.GetObject())
    {
      rapidjson::Value val(node.value, allocator);
      if (!meta_data_root.HasMember(node.name))
        meta_data_root.AddMember(node.name, val, allocator);
    }
  }

  DBUG_RETURN(true);
}
#else
/**
  This function generated meta data in JSON format as a comment in a rows query
  event.

  @see binlog_trx_meta_data

  @return JSON if all good, null string otherwise
*/
std::string THD::gen_trx_metadata()
{
  DBUG_ENTER("THD::gen_trx_metadata");
  DBUG_ASSERT(opt_binlog_trx_meta_data);

  ptree pt;
  std::ostringstream buf;

  // case: read existing meta data received from the master
  if (rli_slave && !rli_slave->trx_meta_data_json.empty())
  {
    std::istringstream is(rli_slave->trx_meta_data_json);
    try {
      read_json(is, pt);
    } catch (std::exception& e) {
      // NO_LINT_DEBUG
      sql_print_error("Exception while reading meta data: %s, JSON: %s",
                       e.what(), rli_slave->trx_meta_data_json.c_str());
      DBUG_RETURN("");
    }
    // clear existing data
    rli_slave->trx_meta_data_json.clear();
  }

  // add things to the meta data
  if (!add_time_metadata(pt) || !add_db_metadata(pt))
  {
    // NO_LINT_DEBUG
    sql_print_error("Error while adding meta data");
    DBUG_RETURN("");
  }

  // write meta data with new stuff in the binlog
  try {
    write_json(buf, pt, false);
  } catch (std::exception& e) {
      // NO_LINT_DEBUG
      sql_print_error("Exception while writing meta data: %s", e.what());
      DBUG_RETURN("");
  }
  std::string json = buf.str();
  boost::trim(json);

  std::string comment_str= std::string("/*")
                           .append(TRX_META_DATA_HEADER)
                           .append(json)
                           .append("*/");

  DBUG_RETURN(comment_str);
}

/**
  This function adds timing information in meta data JSON of rows query event.

  @see THD::write_trx_metadata

  @param meta_data_root Property tree object which represents the JSON
  @return true if all good, false if error
*/
bool THD::add_time_metadata(ptree &meta_data_root)
{
  DBUG_ENTER("THD::add_time_metadata");
  DBUG_ASSERT(opt_binlog_trx_meta_data);

  try {
    // get existing timestamps
    ptree timestamps= meta_data_root.get_child("ts", ptree());

    // add our timestamp to the array
    ptree timestamp;
    ulonglong millis=
      std::chrono::duration_cast<std::chrono::milliseconds>
        (std::chrono::system_clock::now().time_since_epoch()).count();
    timestamp.put("", millis);
    timestamps.push_back(std::make_pair("", timestamp));

    // update timestamps in root
    meta_data_root.erase("ts");
    meta_data_root.add_child("ts", timestamps);
  } catch (std::exception& e) {
    // NO_LINT_DEBUG
    sql_print_error("Exception while writing time meta data: %s", e.what());
    DBUG_RETURN(false);
  }

  DBUG_RETURN(true);
}

bool THD::add_db_metadata(ptree &meta_data_root)
{
  DBUG_ENTER("THD::add_db_meta_data");
  DBUG_ASSERT(opt_binlog_trx_meta_data);

  mysql_mutex_lock(&LOCK_db_metadata);
  std::string local_db_metadata= db_metadata;
  mysql_mutex_unlock(&LOCK_db_metadata);

  if (!local_db_metadata.empty())
  {
    ptree db_metadata_root;
    std::istringstream is(local_db_metadata);
    try {
      read_json(is, db_metadata_root);
    } catch (std::exception& e) {
      // NO_LINT_DEBUG
      sql_print_error("Exception while reading DB meta data: %s, JSON: %s",
                       e.what(), local_db_metadata.c_str());
      DBUG_RETURN(false);
    }
    // flatten DB metadata into trx metadata
    for (auto& node : db_metadata_root)
    {
      if (!meta_data_root.get_child_optional(node.first))
        meta_data_root.add_child(node.first, node.second);
    }
  }

  DBUG_RETURN(true);
}
#endif

/**
  This function writes a table map to the binary log.
  Note that in order to keep the signature uniform with related methods,
  we use a redundant parameter to indicate whether a transactional table
  was changed or not.
  Sometimes it will write a Rows_query_log_event into binary log before
  the table map too.

  @param table             a pointer to the table.
  @param is_transactional  @c true indicates a transactional table,
                           otherwise @c false a non-transactional.
  @param binlog_rows_query @c true indicates a Rows_query log event
                           will be binlogged before table map,
                           otherwise @c false indicates it will not
                           be binlogged.
  @return
    nonzero if an error pops up when writing the table map event
    or the Rows_query log event.
*/
int THD::binlog_write_table_map(TABLE *table, bool is_transactional,
                                bool binlog_rows_query)
{
  int error;
  DBUG_ENTER("THD::binlog_write_table_map");
  DBUG_PRINT("enter", ("table: 0x%lx  (%s: #%llu)",
                       (long) table, table->s->table_name.str,
                       table->s->table_map_id.id()));

  /* Pre-conditions */
  DBUG_ASSERT(is_current_stmt_binlog_format_row() && mysql_bin_log.is_open());
  DBUG_ASSERT(table->s->table_map_id.is_valid());

  Table_map_log_event
    the_event(this, table, table->s->table_map_id, is_transactional);

  binlog_start_trans_and_stmt(this, &the_event);

  binlog_cache_mngr *const cache_mngr= thd_get_cache_mngr(this);

  binlog_cache_data *cache_data=
    cache_mngr->get_binlog_cache_data(is_transactional);

  if (binlog_rows_query)
  {
    std::string query;
    if (opt_binlog_trx_meta_data)
      query.append(gen_trx_metadata());
    if (variables.binlog_rows_query_log_events && this->query())
      query.append(this->query(), this->query_length());

    if (!query.empty())
    {
      /* Write the Rows_query_log_event into binlog before the table map */
      Rows_query_log_event rows_query_ev(this, query.c_str(), query.length());
      if ((error= cache_data->write_event(this, &rows_query_ev)))
        DBUG_RETURN(error);
    }
  }

  if ((error= cache_data->write_event(this, &the_event)))
    DBUG_RETURN(error);

  binlog_table_maps++;
  DBUG_RETURN(0);
}

/**
  This function retrieves a pending row event from a cache which is
  specified through the parameter @c is_transactional. Respectively, when it
  is @c true, the pending event is returned from the transactional cache.
  Otherwise from the non-transactional cache.

  @param is_transactional  @c true indicates a transactional cache,
                           otherwise @c false a non-transactional.
  @return
    The row event if any.
*/
Rows_log_event*
THD::binlog_get_pending_rows_event(bool is_transactional) const
{
  Rows_log_event* rows= NULL;
  binlog_cache_mngr *const cache_mngr= thd_get_cache_mngr(this);

  /*
    This is less than ideal, but here's the story: If there is no cache_mngr,
    prepare_pending_rows_event() has never been called (since the cache_mngr
    is set up there). In that case, we just return NULL.
   */
  if (cache_mngr)
  {
    binlog_cache_data *cache_data=
      cache_mngr->get_binlog_cache_data(is_transactional);

    rows= cache_data->pending();
  }
  return (rows);
}

/**
   @param db    db name c-string to be inserted into alphabetically sorted
                THD::binlog_accessed_db_names list.

                Note, that space for both the data and the node
                struct are allocated in THD::main_mem_root.
                The list lasts for the top-level query time and is reset
                in @c THD::cleanup_after_query().
*/
void
THD::add_to_binlog_accessed_dbs(const char *db_param)
{
  char *after_db;
  /*
    binlog_accessed_db_names list is to maintain the database
    names which are referenced in a given command.
    Prior to bug 17806014 fix, 'main_mem_root' memory root used
    to store this list. The 'main_mem_root' scope is till the end
    of the query. Hence it caused increasing memory consumption
    problem in big procedures like the ones mentioned below.
    Eg: CALL p1() where p1 is having 1,00,000 create and drop tables.
    'main_mem_root' is freed only at the end of the command CALL p1()'s
    execution. But binlog_accessed_db_names list scope is only till the
    individual statements specified the procedure(create/drop statements).
    Hence the memory allocated in 'main_mem_root' was left uncleared
    until the p1's completion, even though it is not required after
    completion of individual statements.

    Instead of using 'main_mem_root' whose scope is complete query execution,
    now the memroot is changed to use 'thd->mem_root' whose scope is until the
    individual statement in CALL p1(). 'thd->mem_root' is set to 'execute_mem_root'
    in the context of procedure and it's scope is till the individual statement
    in CALL p1() and thd->memroot is equal to 'main_mem_root' in the context
    of a normal 'top level query'.

    Eg: a) create table t1(i int); => If this function is called while
           processing this statement, thd->memroot is equal to &main_mem_root
           which will be freed immediately after executing this statement.
        b) CALL p1() -> p1 contains create table t1(i int); => If this function
           is called while processing create table statement which is inside
           a stored procedure, then thd->memroot is equal to 'execute_mem_root'
           which will be freed immediately after executing this statement.
    In both a and b case, thd->memroot will be freed immediately and will not
    increase memory consumption.

    A special case(stored functions/triggers):
    Consider the following example:
    create function f1(i int) returns int
    begin
      insert into db1.t1 values (1);
      insert into db2.t1 values (2);
    end;
    When we are processing SELECT f1(), the list should contain db1, db2 names.
    Since thd->mem_root contains 'execute_mem_root' in the context of
    stored function, the mem root will be freed after adding db1 in
    the list and when we are processing the second statement and when we try
    to add 'db2' in the db1's list, it will lead to crash as db1's memory
    is already freed. To handle this special case, if in_sub_stmt is set
    (which is true incase of stored functions/triggers), we use &main_mem_root,
    if not set we will use thd->memroot which changes it's value to
    'execute_mem_root' or '&main_mem_root' depends on the context.
   */
  MEM_ROOT *db_mem_root= in_sub_stmt ? &main_mem_root : mem_root;

  if (!binlog_accessed_db_names)
    binlog_accessed_db_names= new (db_mem_root) List<char>;

  if (binlog_accessed_db_names->elements >  MAX_DBS_IN_EVENT_MTS)
  {
    push_warning_printf(this, Sql_condition::WARN_LEVEL_WARN,
                        ER_MTS_UPDATED_DBS_GREATER_MAX,
                        ER(ER_MTS_UPDATED_DBS_GREATER_MAX),
                        MAX_DBS_IN_EVENT_MTS);
    return;
  }

  after_db= strdup_root(db_mem_root, db_param);

  /*
     sorted insertion is implemented with first rearranging data
     (pointer to char*) of the links and final appending of the least
     ordered data to create a new link in the list.
  */
  if (binlog_accessed_db_names->elements != 0)
  {
    List_iterator<char> it(*get_binlog_accessed_db_names());

    while (it++)
    {
      char *swap= NULL;
      char **ref_cur_db= it.ref();
      int cmp= strcmp(after_db, *ref_cur_db);

      DBUG_ASSERT(!swap || cmp < 0);

      if (cmp == 0)
      {
        after_db= NULL;  /* dup to ignore */
        break;
      }
      else if (swap || cmp > 0)
      {
        swap= *ref_cur_db;
        *ref_cur_db= after_db;
        after_db= swap;
      }
    }
  }
  if (after_db)
    binlog_accessed_db_names->push_back(after_db, db_mem_root);
}

/*
  Tells if two (or more) tables have auto_increment columns and we want to
  lock those tables with a write lock.

  SYNOPSIS
    has_two_write_locked_tables_with_auto_increment
      tables        Table list

  NOTES:
    Call this function only when you have established the list of all tables
    which you'll want to update (including stored functions, triggers, views
    inside your statement).
*/

static bool
has_write_table_with_auto_increment(TABLE_LIST *tables)
{
  for (TABLE_LIST *table= tables; table; table= table->next_global)
  {
    /* we must do preliminary checks as table->table may be NULL */
    if (!table->placeholder() &&
        table->table->found_next_number_field &&
        (table->lock_type >= TL_WRITE_ALLOW_WRITE))
      return 1;
  }

  return 0;
}

/*
   checks if we have select tables in the table list and write tables
   with auto-increment column.

  SYNOPSIS
   has_two_write_locked_tables_with_auto_increment_and_select
      tables        Table list

  RETURN VALUES

   -true if the table list has atleast one table with auto-increment column


         and atleast one table to select from.
   -false otherwise
*/

static bool
has_write_table_with_auto_increment_and_select(TABLE_LIST *tables)
{
  bool has_select= false;
  bool has_auto_increment_tables = has_write_table_with_auto_increment(tables);
  for(TABLE_LIST *table= tables; table; table= table->next_global)
  {
     if (!table->placeholder() &&
        (table->lock_type <= TL_READ_NO_INSERT))
      {
        has_select= true;
        break;
      }
  }
  return(has_select && has_auto_increment_tables);
}

/*
  Tells if there is a table whose auto_increment column is a part
  of a compound primary key while is not the first column in
  the table definition.

  @param tables Table list

  @return true if the table exists, fais if does not.
*/

static bool
has_write_table_auto_increment_not_first_in_pk(TABLE_LIST *tables)
{
  for (TABLE_LIST *table= tables; table; table= table->next_global)
  {
    /* we must do preliminary checks as table->table may be NULL */
    if (!table->placeholder() &&
        table->table->found_next_number_field &&
        (table->lock_type >= TL_WRITE_ALLOW_WRITE)
        && table->table->s->next_number_keypart != 0)
      return 1;
  }

  return 0;
}

#ifndef DBUG_OFF
const char * get_locked_tables_mode_name(enum_locked_tables_mode locked_tables_mode)
{
   switch (locked_tables_mode)
   {
   case LTM_NONE:
     return "LTM_NONE";
   case LTM_LOCK_TABLES:
     return "LTM_LOCK_TABLES";
   case LTM_PRELOCKED:
     return "LTM_PRELOCKED";
   case LTM_PRELOCKED_UNDER_LOCK_TABLES:
     return "LTM_PRELOCKED_UNDER_LOCK_TABLES";
   default:
     return "Unknown table lock mode";
   }
}
#endif


/**
  Decide on logging format to use for the statement and issue errors
  or warnings as needed.  The decision depends on the following
  parameters:

  - The logging mode, i.e., the value of binlog_format.  Can be
    statement, mixed, or row.

  - The type of statement.  There are three types of statements:
    "normal" safe statements; unsafe statements; and row injections.
    An unsafe statement is one that, if logged in statement format,
    might produce different results when replayed on the slave (e.g.,
    INSERT DELAYED).  A row injection is either a BINLOG statement, or
    a row event executed by the slave's SQL thread.

  - The capabilities of tables modified by the statement.  The
    *capabilities vector* for a table is a set of flags associated
    with the table.  Currently, it only includes two flags: *row
    capability flag* and *statement capability flag*.

    The row capability flag is set if and only if the engine can
    handle row-based logging. The statement capability flag is set if
    and only if the table can handle statement-based logging.

  Decision table for logging format
  ---------------------------------

  The following table summarizes how the format and generated
  warning/error depends on the tables' capabilities, the statement
  type, and the current binlog_format.

     Row capable        N NNNNNNNNN YYYYYYYYY YYYYYYYYY
     Statement capable  N YYYYYYYYY NNNNNNNNN YYYYYYYYY

     Statement type     * SSSUUUIII SSSUUUIII SSSUUUIII

     binlog_format      * SMRSMRSMR SMRSMRSMR SMRSMRSMR

     Logged format      - SS-S----- -RR-RR-RR SRRSRR-RR
     Warning/Error      1 --2732444 5--5--6-- ---7--6--

  Legend
  ------

  Row capable:    N - Some table not row-capable, Y - All tables row-capable
  Stmt capable:   N - Some table not stmt-capable, Y - All tables stmt-capable
  Statement type: (S)afe, (U)nsafe, or Row (I)njection
  binlog_format:  (S)TATEMENT, (M)IXED, or (R)OW
  Logged format:  (S)tatement or (R)ow
  Warning/Error:  Warnings and error messages are as follows:

  1. Error: Cannot execute statement: binlogging impossible since both
     row-incapable engines and statement-incapable engines are
     involved.

  2. Error: Cannot execute statement: binlogging impossible since
     BINLOG_FORMAT = ROW and at least one table uses a storage engine
     limited to statement-logging.

  3. Error: Cannot execute statement: binlogging of unsafe statement
     is impossible when storage engine is limited to statement-logging
     and BINLOG_FORMAT = MIXED.

  4. Error: Cannot execute row injection: binlogging impossible since
     at least one table uses a storage engine limited to
     statement-logging.

  5. Error: Cannot execute statement: binlogging impossible since
     BINLOG_FORMAT = STATEMENT and at least one table uses a storage
     engine limited to row-logging.

  6. Error: Cannot execute row injection: binlogging impossible since
     BINLOG_FORMAT = STATEMENT.

  7. Warning: Unsafe statement binlogged in statement format since
     BINLOG_FORMAT = STATEMENT.

  In addition, we can produce the following error (not depending on
  the variables of the decision diagram):

  8. Error: Cannot execute statement: binlogging impossible since more
     than one engine is involved and at least one engine is
     self-logging.

  For each error case above, the statement is prevented from being
  logged, we report an error, and roll back the statement.  For
  warnings, we set the thd->binlog_flags variable: the warning will be
  printed only if the statement is successfully logged.

  @see THD::binlog_query

  @param[in] thd    Client thread
  @param[in] tables Tables involved in the query

  @retval 0 No error; statement can be logged.
  @retval -1 One of the error conditions above applies (1, 2, 4, 5, or 6).
*/

int THD::decide_logging_format(TABLE_LIST *tables)
{
  DBUG_ENTER("THD::decide_logging_format");
  DBUG_PRINT("info", ("query: %s", query()));
  DBUG_PRINT("info", ("variables.binlog_format: %lu",
                      variables.binlog_format));
  DBUG_PRINT("info", ("lex->get_stmt_unsafe_flags(): 0x%x",
                      lex->get_stmt_unsafe_flags()));

  reset_binlog_local_stmt_filter();

  /*
    We should not decide logging format if the binlog is closed or
    binlogging is off, or if the statement is filtered out from the
    binlog by filtering rules.
  */
  if (mysql_bin_log.is_open() && (variables.option_bits & OPTION_BIN_LOG) &&
      !(variables.binlog_format == BINLOG_FORMAT_STMT &&
        !binlog_filter->db_ok(db)))
  {
    /*
      Compute one bit field with the union of all the engine
      capabilities, and one with the intersection of all the engine
      capabilities.
    */
    handler::Table_flags flags_write_some_set= 0;
    handler::Table_flags flags_access_some_set= 0;
    handler::Table_flags flags_write_all_set=
      HA_BINLOG_ROW_CAPABLE | HA_BINLOG_STMT_CAPABLE;

    /*
       If different types of engines are about to be updated.
       For example: Innodb and Falcon; Innodb and MyIsam.
    */
    my_bool multi_write_engine= FALSE;
    /*
       If different types of engines are about to be accessed
       and any of them is about to be updated. For example:
       Innodb and Falcon; Innodb and MyIsam.
    */
    my_bool multi_access_engine= FALSE;
    /*
       Identifies if a table is changed.
    */
    my_bool is_write= FALSE;
    /*
       A pointer to a previous table that was changed.
    */
    TABLE* prev_write_table= NULL;
    /*
       A pointer to a previous table that was accessed.
    */
    TABLE* prev_access_table= NULL;
    /*
      True if at least one table is transactional.
    */
    bool write_to_some_transactional_table= false;
    /*
      True if at least one table is non-transactional.
    */
    bool write_to_some_non_transactional_table= false;
    /*
       True if all non-transactional tables that has been updated
       are temporary.
    */
    bool write_all_non_transactional_are_tmp_tables= true;
    /**
      The number of tables used in the current statement,
      that should be replicated.
    */
    uint replicated_tables_count= 0;
    /**
      The number of tables written to in the current statement,
      that should not be replicated.
      A table should not be replicated when it is considered
      'local' to a MySQL instance.
      Currently, these tables are:
      - mysql.slow_log
      - mysql.general_log
      - mysql.slave_relay_log_info
      - mysql.slave_master_info
      - mysql.slave_worker_info
      - performance_schema.*
      - TODO: information_schema.*
      In practice, from this list, only performance_schema.* tables
      are written to by user queries.
    */
    uint non_replicated_tables_count= 0;
#ifndef DBUG_OFF
    {
      DBUG_PRINT("debug", ("prelocked_mode: %s",
                           get_locked_tables_mode_name(locked_tables_mode)));
    }
#endif

    if (variables.binlog_format != BINLOG_FORMAT_ROW && tables)
    {
      /*
        DML statements that modify a table with an auto_increment column based on
        rows selected from a table are unsafe as the order in which the rows are
        fetched fron the select tables cannot be determined and may differ on
        master and slave.
       */
      if (has_write_table_with_auto_increment_and_select(tables))
        lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_WRITE_AUTOINC_SELECT);

      if (has_write_table_auto_increment_not_first_in_pk(tables))
        lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_AUTOINC_NOT_FIRST);

      /*
        A query that modifies autoinc column in sub-statement can make the
        master and slave inconsistent.
        We can solve these problems in mixed mode by switching to binlogging
        if at least one updated table is used by sub-statement
       */
      if (lex->requires_prelocking() &&
          has_write_table_with_auto_increment(lex->first_not_own_table()))
        lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_AUTOINC_COLUMNS);
    }

    /*
      Get the capabilities vector for all involved storage engines and
      mask out the flags for the binary log.
    */
    for (TABLE_LIST *table= tables; table; table= table->next_global)
    {
      if (table->placeholder())
        continue;

      handler::Table_flags const flags= table->table->file->ha_table_flags();

      DBUG_PRINT("info", ("table: %s; ha_table_flags: 0x%llx",
                          table->table_name, flags));

      if (table->table->no_replicate)
      {
        /*
          The statement uses a table that is not replicated.
          The following properties about the table:
          - persistent / transient
          - transactional / non transactional
          - temporary / permanent
          - read or write
          - multiple engines involved because of this table
          are not relevant, as this table is completely ignored.
          Because the statement uses a non replicated table,
          using STATEMENT format in the binlog is impossible.
          Either this statement will be discarded entirely,
          or it will be logged (possibly partially) in ROW format.
        */
        lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_TABLE);

        if (table->lock_type >= TL_WRITE_ALLOW_WRITE)
        {
          non_replicated_tables_count++;
          continue;
        }
      }

      replicated_tables_count++;

      my_bool trans= table->table->file->has_transactions();

      if (table->lock_type >= TL_WRITE_ALLOW_WRITE)
      {
        write_to_some_transactional_table=
          write_to_some_transactional_table || trans;

        write_to_some_non_transactional_table=
          write_to_some_non_transactional_table || !trans;

        if (prev_write_table && prev_write_table->file->ht !=
            table->table->file->ht)
          multi_write_engine= TRUE;

        if (table->table->s->tmp_table)
          lex->set_stmt_accessed_table(trans ? LEX::STMT_WRITES_TEMP_TRANS_TABLE :
                                               LEX::STMT_WRITES_TEMP_NON_TRANS_TABLE);
        else
          lex->set_stmt_accessed_table(trans ? LEX::STMT_WRITES_TRANS_TABLE :
                                               LEX::STMT_WRITES_NON_TRANS_TABLE);

        /*
         Non-transactional updates are allowed when row binlog format is
         used and all non-transactional tables are temporary.
         Binlog format is checked on THD::is_dml_gtid_compatible() method.
        */
        if (!trans)
          write_all_non_transactional_are_tmp_tables=
            write_all_non_transactional_are_tmp_tables &&
            table->table->s->tmp_table;

        flags_write_all_set &= flags;
        flags_write_some_set |= flags;
        is_write= TRUE;

        prev_write_table= table->table;

        /*
          INSERT...ON DUPLICATE KEY UPDATE on a table with more than one unique keys
          can be unsafe. Check for it if the flag is already not marked for the
          given statement.
        */
        if (!lex->is_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_INSERT_TWO_KEYS) &&
            lex->sql_command == SQLCOM_INSERT &&
            /* Duplicate key update is not supported by INSERT DELAYED */
            get_command() != COM_DELAYED_INSERT && lex->duplicates == DUP_UPDATE)
        {
          uint keys= table->table->s->keys, i= 0, unique_keys= 0;
          for (KEY* keyinfo= table->table->s->key_info;
               i < keys && unique_keys <= 1; i++, keyinfo++)
          {
            if (keyinfo->flags & HA_NOSAME)
              unique_keys++;
          }
          if (unique_keys > 1 )
            lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_INSERT_TWO_KEYS);
        }
      }
      flags_access_some_set |= flags;

      if (lex->sql_command != SQLCOM_CREATE_TABLE ||
          (lex->sql_command == SQLCOM_CREATE_TABLE &&
          (lex->create_info.options & HA_LEX_CREATE_TMP_TABLE)))
      {
        if (table->table->s->tmp_table)
          lex->set_stmt_accessed_table(trans ? LEX::STMT_READS_TEMP_TRANS_TABLE :
                                               LEX::STMT_READS_TEMP_NON_TRANS_TABLE);
        else
          lex->set_stmt_accessed_table(trans ? LEX::STMT_READS_TRANS_TABLE :
                                               LEX::STMT_READS_NON_TRANS_TABLE);
      }

      if (prev_access_table && prev_access_table->file->ht !=
          table->table->file->ht)
         multi_access_engine= TRUE;

      prev_access_table= table->table;
    }
    DBUG_ASSERT(!is_write ||
                write_to_some_transactional_table ||
                write_to_some_non_transactional_table);
    /*
      write_all_non_transactional_are_tmp_tables may be true if any
      non-transactional table was not updated, so we fix its value here.
    */
    write_all_non_transactional_are_tmp_tables=
      write_all_non_transactional_are_tmp_tables &&
      write_to_some_non_transactional_table;

    DBUG_PRINT("info", ("flags_write_all_set: 0x%llx", flags_write_all_set));
    DBUG_PRINT("info", ("flags_write_some_set: 0x%llx", flags_write_some_set));
    DBUG_PRINT("info", ("flags_access_some_set: 0x%llx", flags_access_some_set));
    DBUG_PRINT("info", ("multi_write_engine: %d", multi_write_engine));
    DBUG_PRINT("info", ("multi_access_engine: %d", multi_access_engine));

    int error= 0;
    int unsafe_flags;

    bool multi_stmt_trans= in_multi_stmt_transaction_mode();
    bool trans_table= trans_has_updated_trans_table(this);
    bool binlog_direct= variables.binlog_direct_non_trans_update;

    if (lex->is_mixed_stmt_unsafe(multi_stmt_trans, binlog_direct,
                                  trans_table, tx_isolation))
      lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_MIXED_STATEMENT);
    else if (multi_stmt_trans && trans_table && !binlog_direct &&
             lex->stmt_accessed_table(LEX::STMT_WRITES_NON_TRANS_TABLE))
      lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_NONTRANS_AFTER_TRANS);

    /*
      If more than one engine is involved in the statement and at
      least one is doing it's own logging (is *self-logging*), the
      statement cannot be logged atomically, so we generate an error
      rather than allowing the binlog to become corrupt.
    */
    if (multi_write_engine &&
        (flags_write_some_set & HA_HAS_OWN_BINLOGGING))
      my_error((error= ER_BINLOG_MULTIPLE_ENGINES_AND_SELF_LOGGING_ENGINE),
               MYF(0));
    else if (multi_access_engine && flags_access_some_set & HA_HAS_OWN_BINLOGGING)
      lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_MULTIPLE_ENGINES_AND_SELF_LOGGING_ENGINE);

    /* both statement-only and row-only engines involved */
    if ((flags_write_all_set & (HA_BINLOG_STMT_CAPABLE | HA_BINLOG_ROW_CAPABLE)) == 0)
    {
      /*
        1. Error: Binary logging impossible since both row-incapable
           engines and statement-incapable engines are involved
      */
      my_error((error= ER_BINLOG_ROW_ENGINE_AND_STMT_ENGINE), MYF(0));
    }
    /* statement-only engines involved */
    else if ((flags_write_all_set & HA_BINLOG_ROW_CAPABLE) == 0)
    {
      if (lex->is_stmt_row_injection())
      {
        /*
          4. Error: Cannot execute row injection since table uses
             storage engine limited to statement-logging
        */
        my_error((error= ER_BINLOG_ROW_INJECTION_AND_STMT_ENGINE), MYF(0));
      }
      else if (variables.binlog_format == BINLOG_FORMAT_ROW &&
               sqlcom_can_generate_row_events(this->lex->sql_command))
      {
        /*
          2. Error: Cannot modify table that uses a storage engine
             limited to statement-logging when BINLOG_FORMAT = ROW
        */
        my_error((error= ER_BINLOG_ROW_MODE_AND_STMT_ENGINE), MYF(0));
      }
      else if ((unsafe_flags= lex->get_stmt_unsafe_flags()) != 0)
      {
        /*
          3. Error: Cannot execute statement: binlogging of unsafe
             statement is impossible when storage engine is limited to
             statement-logging and BINLOG_FORMAT = MIXED.
        */
        for (int unsafe_type= 0;
             unsafe_type < LEX::BINLOG_STMT_UNSAFE_COUNT;
             unsafe_type++)
          if (unsafe_flags & (1 << unsafe_type))
            my_error((error= ER_BINLOG_UNSAFE_AND_STMT_ENGINE), MYF(0),
                     ER(LEX::binlog_stmt_unsafe_errcode[unsafe_type]));
      }
      /* log in statement format! */
    }
    /* no statement-only engines */
    else
    {
      /* binlog_format = STATEMENT */
      if (variables.binlog_format == BINLOG_FORMAT_STMT)
      {
        if (lex->is_stmt_row_injection())
        {
          /*
            6. Error: Cannot execute row injection since
               BINLOG_FORMAT = STATEMENT
          */
          my_error((error= ER_BINLOG_ROW_INJECTION_AND_STMT_MODE), MYF(0));
        }
        else if ((flags_write_all_set & HA_BINLOG_STMT_CAPABLE) == 0 &&
                 sqlcom_can_generate_row_events(this->lex->sql_command))
        {
          /*
            5. Error: Cannot modify table that uses a storage engine
               limited to row-logging when binlog_format = STATEMENT
          */
          my_error((error= ER_BINLOG_STMT_MODE_AND_ROW_ENGINE), MYF(0), "");
        }
        else if (is_write && (unsafe_flags= lex->get_stmt_unsafe_flags()) != 0)
        {
          /*
            7. Warning: Unsafe statement logged as statement due to
               binlog_format = STATEMENT
          */
          binlog_unsafe_warning_flags|= unsafe_flags;
          DBUG_PRINT("info", ("Scheduling warning to be issued by "
                              "binlog_query: '%s'",
                              ER(ER_BINLOG_UNSAFE_STATEMENT)));
          DBUG_PRINT("info", ("binlog_unsafe_warning_flags: 0x%x",
                              binlog_unsafe_warning_flags));
        }
        /* log in statement format! */
      }
      /* No statement-only engines and binlog_format != STATEMENT.
         I.e., nothing prevents us from row logging if needed. */
      else
      {
        if (lex->is_stmt_unsafe() || lex->is_stmt_row_injection()
            || (flags_write_all_set & HA_BINLOG_STMT_CAPABLE) == 0)
        {
          /* log in row format! */
          set_current_stmt_binlog_format_row_if_mixed();
        }
      }
    }

    if (non_replicated_tables_count > 0)
    {
      if ((replicated_tables_count == 0) || ! is_write)
      {
        DBUG_PRINT("info", ("decision: no logging, no replicated table affected"));
        set_binlog_local_stmt_filter();
      }
      else
      {
        if (! is_current_stmt_binlog_format_row())
        {
          my_error((error= ER_BINLOG_STMT_MODE_AND_NO_REPL_TABLES), MYF(0));
        }
        else
        {
          clear_binlog_local_stmt_filter();
        }
      }
    }
    else
    {
      clear_binlog_local_stmt_filter();
    }

    if (!error &&
        !is_dml_gtid_compatible(write_to_some_transactional_table,
                                write_to_some_non_transactional_table,
                                write_all_non_transactional_are_tmp_tables))
      error= 1;

    if (error) {
      DBUG_PRINT("info", ("decision: no logging since an error was generated"));
      DBUG_RETURN(-1);
    }

    if (is_write &&
        lex->sql_command != SQLCOM_END /* rows-event applying by slave */)
    {
      /*
        Master side of DML in the STMT format events parallelization.
        All involving table db:s are stored in a abc-ordered name list.
        In case the number of databases exceeds MAX_DBS_IN_EVENT_MTS maximum
        the list gathering breaks since it won't be sent to the slave.
      */
      for (TABLE_LIST *table= tables; table; table= table->next_global)
      {
        if (table->placeholder())
          continue;

        DBUG_ASSERT(table->table);

        if (table->table->file->referenced_by_foreign_key())
        {
          /*
             FK-referenced dbs can't be gathered currently. The following
             event will be marked for sequential execution on slave.
          */
          binlog_accessed_db_names= NULL;
          add_to_binlog_accessed_dbs("");
          break;
        }
        if (!is_current_stmt_binlog_format_row())
          add_to_binlog_accessed_dbs(table->db);
      }
    }
    DBUG_PRINT("info", ("decision: logging in %s format",
                        is_current_stmt_binlog_format_row() ?
                        "ROW" : "STATEMENT"));

    if (variables.binlog_format == BINLOG_FORMAT_ROW &&
        (lex->sql_command == SQLCOM_UPDATE ||
         lex->sql_command == SQLCOM_UPDATE_MULTI ||
         lex->sql_command == SQLCOM_DELETE ||
         lex->sql_command == SQLCOM_DELETE_MULTI))
    {
      String table_names;
      /*
        Generate a warning for UPDATE/DELETE statements that modify a
        BLACKHOLE table, as row events are not logged in row format.
      */
      for (TABLE_LIST *table= tables; table; table= table->next_global)
      {
        if (table->placeholder())
          continue;
        if (table->table->file->ht->db_type == DB_TYPE_BLACKHOLE_DB &&
            table->lock_type >= TL_WRITE_ALLOW_WRITE)
        {
            table_names.append(table->table_name);
            table_names.append(",");
        }
      }
      if (!table_names.is_empty())
      {
        bool is_update= (lex->sql_command == SQLCOM_UPDATE ||
                         lex->sql_command == SQLCOM_UPDATE_MULTI);
        /*
          Replace the last ',' with '.' for table_names
        */
        table_names.replace(table_names.length()-1, 1, ".", 1);
        push_warning_printf(this, Sql_condition::WARN_LEVEL_WARN,
                            WARN_ON_BLOCKHOLE_IN_RBR,
                            ER(WARN_ON_BLOCKHOLE_IN_RBR),
                            is_update ? "UPDATE" : "DELETE",
                            table_names.c_ptr());
      }
    }
  }
#ifndef DBUG_OFF
  else
    DBUG_PRINT("info", ("decision: no logging since "
                        "mysql_bin_log.is_open() = %d "
                        "and (options & OPTION_BIN_LOG) = 0x%llx "
                        "and binlog_format = %lu "
                        "and binlog_filter->db_ok(db) = %d",
                        mysql_bin_log.is_open(),
                        (variables.option_bits & OPTION_BIN_LOG),
                        variables.binlog_format,
                        binlog_filter->db_ok(db)));
#endif

  DBUG_RETURN(0);
}

static void log_gtid_incompatible_statements(THD *thd)
{
  if (log_gtid_unsafe_statements)
    sql_print_information("gtid unsafe query executed on db: %s Query info: %s",
                           thd->db, thd->query());
}

bool THD::is_ddl_gtid_compatible()
{
  DBUG_ENTER("THD::is_ddl_gtid_compatible");

  USER_STATS *us = thd_get_user_stats(this);

  // If @@session.sql_log_bin has been manually turned off (only
  // doable by SUPER), then no problem, we can execute any statement.
  if ((variables.option_bits & OPTION_BIN_LOG) == 0)
    DBUG_RETURN(true);

  if (lex->sql_command == SQLCOM_CREATE_TABLE &&
      !(lex->create_info.options & HA_LEX_CREATE_TMP_TABLE) &&
      lex->select_lex.item_list.elements)
  {
    log_gtid_incompatible_statements(this);
    /*
      CREATE ... SELECT (without TEMPORARY) is unsafe because if
      binlog_format=row it will be logged as a CREATE TABLE followed
      by row events, re-executed non-atomically as two transactions,
      and then written to the slave's binary log as two separate
      transactions with the same GTID.
    */
    if (us) {
      my_atomic_add32((int*)&us->n_gtid_unsafe_create_select, 1);
    }
    if (enforce_gtid_consistency && should_write_gtids(this)) {
      my_error(ER_GTID_UNSAFE_CREATE_SELECT, MYF(0));
      DBUG_RETURN(false);
    }
  }
  if ((lex->sql_command == SQLCOM_CREATE_TABLE &&
       (lex->create_info.options & HA_LEX_CREATE_TMP_TABLE) != 0) ||
      (lex->sql_command == SQLCOM_DROP_TABLE && lex->drop_temporary))
  {
    /*
      [CREATE|DROP] TEMPORARY TABLE is unsafe to execute
      inside a transaction because the table will stay and the
      transaction will be written to the slave's binary log with the
      GTID even if the transaction is rolled back.
      This includes the execution inside Functions and Triggers.
    */
    if (in_multi_stmt_transaction_mode() || in_sub_stmt)
    {
      log_gtid_incompatible_statements(this);
      if (us) {
        my_atomic_add32(
          (int*)&us->n_gtid_unsafe_create_drop_temporary_table_in_transaction,
          1);
      }
      if (enforce_gtid_consistency && should_write_gtids(this)) {
        my_error(ER_GTID_UNSAFE_CREATE_DROP_TEMPORARY_TABLE_IN_TRANSACTION,
                 MYF(0));
        DBUG_RETURN(false);
      }
    }
  }
  DBUG_RETURN(true);
}


bool
THD::is_dml_gtid_compatible(bool transactional_table,
                            bool non_transactional_table,
                            bool non_transactional_tmp_tables)
{
  DBUG_ENTER("THD::is_dml_gtid_compatible(bool, bool, bool)");

  // If @@session.sql_log_bin has been manually turned off (only
  // doable by SUPER), then no problem, we can execute any statement.
  if ((variables.option_bits & OPTION_BIN_LOG) == 0)
    DBUG_RETURN(true);

  /*
    Single non-transactional updates are allowed when not mixed
    together with transactional statements within a transaction.
    Furthermore, writing to transactional and non-transactional
    engines in a single statement is also disallowed.
    Multi-statement transactions on non-transactional tables are
    split into single-statement transactions when
    GTID_NEXT = "AUTOMATIC".

    Non-transactional updates are allowed when row binlog format is
    used and all non-transactional tables are temporary.

    The debug symbol "allow_gtid_unsafe_non_transactional_updates"
    disables the error.  This is useful because it allows us to run
    old tests that were not written with the restrictions of GTIDs in
    mind.
  */
  if (non_transactional_table &&
      (transactional_table || trans_has_updated_trans_table(this)) &&
      !(non_transactional_tmp_tables && is_current_stmt_binlog_format_row()) &&
      !DBUG_EVALUATE_IF("allow_gtid_unsafe_non_transactional_updates", 1, 0))
  {
    log_gtid_incompatible_statements(this);
    USER_STATS *us = thd_get_user_stats(this);
    if (us) {
      my_atomic_add32((int*)&us->n_gtid_unsafe_non_transactional_table, 1);
    }
    if (enforce_gtid_consistency && should_write_gtids(this)) {
      my_error(ER_GTID_UNSAFE_NON_TRANSACTIONAL_TABLE, MYF(0));
      DBUG_RETURN(false);
    }
  }

  DBUG_RETURN(true);
}

/*
  Implementation of interface to write rows to the binary log through the
  thread.  The thread is responsible for writing the rows it has
  inserted/updated/deleted.
*/

#ifndef MYSQL_CLIENT

/*
  Template member function for ensuring that there is an rows log
  event of the apropriate type before proceeding.

  PRE CONDITION:
    - Events of type 'RowEventT' have the type code 'type_code'.

  POST CONDITION:
    If a non-NULL pointer is returned, the pending event for thread 'thd' will
    be an event of type 'RowEventT' (which have the type code 'type_code')
    will either empty or have enough space to hold 'needed' bytes.  In
    addition, the columns bitmap will be correct for the row, meaning that
    the pending event will be flushed if the columns in the event differ from
    the columns suppled to the function.

  RETURNS
    If no error, a non-NULL pending event (either one which already existed or
    the newly created one).
    If error, NULL.
 */

template <class RowsEventT> Rows_log_event*
THD::binlog_prepare_pending_rows_event(TABLE* table, uint32 serv_id,
                                       size_t needed,
                                       bool is_transactional,
				       RowsEventT *hint MY_ATTRIBUTE((unused)),
                                       const uchar* extra_row_info)
{
  DBUG_ENTER("binlog_prepare_pending_rows_event");

  /* Fetch the type code for the RowsEventT template parameter */
  int const general_type_code= RowsEventT::TYPE_CODE;

  Rows_log_event* pending= binlog_get_pending_rows_event(is_transactional);

  if (unlikely(pending && !pending->is_valid()))
    DBUG_RETURN(NULL);

  /*
    Check if the current event is non-NULL and a write-rows
    event. Also check if the table provided is mapped: if it is not,
    then we have switched to writing to a new table.
    If there is no pending event, we need to create one. If there is a pending
    event, but it's not about the same table id, or not of the same type
    (between Write, Update and Delete), or not the same affected columns, or
    going to be too big, flush this event to disk and create a new pending
    event.
  */
  if (!pending ||
      pending->server_id != serv_id ||
      pending->get_table_id() != table->s->table_map_id ||
      pending->get_general_type_code() != general_type_code ||
      pending->get_data_size() + needed > opt_binlog_rows_event_max_size ||
      pending->m_row_count >= opt_binlog_rows_event_max_rows ||
      pending->read_write_bitmaps_cmp(table) == FALSE ||
      !binlog_row_event_extra_data_eq(pending->get_extra_row_data(),
                                      extra_row_info))
  {
    /* Create a new RowsEventT... */
    Rows_log_event* const
	ev= new RowsEventT(this, table, table->s->table_map_id,
                           is_transactional, extra_row_info);
    if (unlikely(!ev))
      DBUG_RETURN(NULL);
    ev->server_id= serv_id; // I don't like this, it's too easy to forget.
    /*
      flush the pending event and replace it with the newly created
      event...
    */
    if (unlikely(
        mysql_bin_log.flush_and_set_pending_rows_event(this, ev,
                                                       is_transactional)))
    {
      delete ev;
      DBUG_RETURN(NULL);
    }

    DBUG_RETURN(ev);               /* This is the new pending event */
  }
  DBUG_RETURN(pending);        /* This is the current pending event */
}

/* Declare in unnamed namespace. */
CPP_UNNAMED_NS_START

  /**
     Class to handle temporary allocation of memory for row data.

     The responsibilities of the class is to provide memory for
     packing one or two rows of packed data (depending on what
     constructor is called).

     In order to make the allocation more efficient for "simple" rows,
     i.e., rows that do not contain any blobs, a pointer to the
     allocated memory is of memory is stored in the table structure
     for simple rows.  If memory for a table containing a blob field
     is requested, only memory for that is allocated, and subsequently
     released when the object is destroyed.

   */
  class Row_data_memory {
  public:
    /**
      Build an object to keep track of a block-local piece of memory
      for storing a row of data.

      @param table
      Table where the pre-allocated memory is stored.

      @param length
      Length of data that is needed, if the record contain blobs.
     */
    Row_data_memory(TABLE *table, size_t const len1)
      : m_memory(0)
    {
#ifndef DBUG_OFF
      m_alloc_checked= FALSE;
#endif
      allocate_memory(table, len1);
      m_ptr[0]= has_memory() ? m_memory : 0;
      m_ptr[1]= 0;
    }

    Row_data_memory(TABLE *table, size_t const len1, size_t const len2)
      : m_memory(0)
    {
#ifndef DBUG_OFF
      m_alloc_checked= FALSE;
#endif
      allocate_memory(table, len1 + len2);
      m_ptr[0]= has_memory() ? m_memory        : 0;
      m_ptr[1]= has_memory() ? m_memory + len1 : 0;
    }

    ~Row_data_memory()
    {
      if (m_memory != 0 && m_release_memory_on_destruction)
        my_free(m_memory);
    }

    /**
       Is there memory allocated?

       @retval true There is memory allocated
       @retval false Memory allocation failed
     */
    bool has_memory() const {
#ifndef DBUG_OFF
      m_alloc_checked= TRUE;
#endif
      return m_memory != 0;
    }

    uchar *slot(uint s)
    {
      DBUG_ASSERT(s < sizeof(m_ptr)/sizeof(*m_ptr));
      DBUG_ASSERT(m_ptr[s] != 0);
      DBUG_ASSERT(m_alloc_checked == TRUE);
      return m_ptr[s];
    }

  private:
    void allocate_memory(TABLE *const table, size_t const total_length)
    {
      if (table->s->blob_fields == 0)
      {
        /*
          The maximum length of a packed record is less than this
          length. We use this value instead of the supplied length
          when allocating memory for records, since we don't know how
          the memory will be used in future allocations.

          Since table->s->reclength is for unpacked records, we have
          to add two bytes for each field, which can potentially be
          added to hold the length of a packed field.
        */
        size_t const maxlen= table->s->reclength + 2 * table->s->fields;

        /*
          Allocate memory for two records if memory hasn't been
          allocated. We allocate memory for two records so that it can
          be used when processing update rows as well.
        */
        if (table->write_row_record == 0)
          table->write_row_record=
            (uchar *) alloc_root(&table->mem_root, 2 * maxlen);
        m_memory= table->write_row_record;
        m_release_memory_on_destruction= FALSE;
      }
      else
      {
        m_memory= (uchar *) my_malloc(total_length, MYF(MY_WME));
        m_release_memory_on_destruction= TRUE;
      }
    }

#ifndef DBUG_OFF
    mutable bool m_alloc_checked;
#endif
    bool m_release_memory_on_destruction;
    uchar *m_memory;
    uchar *m_ptr[2];
  };

CPP_UNNAMED_NS_END

int THD::binlog_write_row(TABLE* table, bool is_trans,
                          uchar const *record,
                          const uchar* extra_row_info)
{
  DBUG_ASSERT(is_current_stmt_binlog_format_row() && mysql_bin_log.is_open());

  /*
    Pack records into format for transfer. We are allocating more
    memory than needed, but that doesn't matter.
  */
  Row_data_memory memory(table, max_row_length(table, record));
  if (!memory.has_memory())
    return HA_ERR_OUT_OF_MEM;

  uchar *row_data= memory.slot(0);

  size_t const len= pack_row(table, table->write_set, row_data, record);

  Rows_log_event* const ev=
    binlog_prepare_pending_rows_event(table, server_id, len, is_trans,
                                      static_cast<Write_rows_log_event*>(0),
                                      extra_row_info);

  if (unlikely(ev == 0))
    return HA_ERR_OUT_OF_MEM;

  return ev->add_row_data(row_data, len);
}

int THD::binlog_update_row(TABLE* table, bool is_trans,
                           const uchar *before_record,
                           const uchar *after_record,
                           const uchar* extra_row_info)
{
  DBUG_ASSERT(is_current_stmt_binlog_format_row() && mysql_bin_log.is_open());
  int error= 0;

  /**
    Save a reference to the original read and write set bitmaps.
    We will need this to restore the bitmaps at the end.
   */
  MY_BITMAP *old_read_set= table->read_set;
  MY_BITMAP *old_write_set= table->write_set;

  /**
     This will remove spurious fields required during execution but
     not needed for binlogging. This is done according to the:
     binlog-row-image option.
   */
  binlog_prepare_row_images(table, true);

  size_t const before_maxlen = max_row_length(table, before_record);
  size_t const after_maxlen  = max_row_length(table, after_record);

  Row_data_memory row_data(table, before_maxlen, after_maxlen);
  if (!row_data.has_memory())
    return HA_ERR_OUT_OF_MEM;

  uchar *before_row= row_data.slot(0);
  uchar *after_row= row_data.slot(1);

  size_t const before_size= pack_row(table, table->read_set, before_row,
                                        before_record);
  size_t const after_size= pack_row(table, table->write_set, after_row,
                                       after_record);

  /*
    Don't print debug messages when running valgrind since they can
    trigger false warnings.
   */
#ifndef HAVE_purify
  DBUG_DUMP("before_record", before_record, table->s->reclength);
  DBUG_DUMP("after_record",  after_record, table->s->reclength);
  DBUG_DUMP("before_row",    before_row, before_size);
  DBUG_DUMP("after_row",     after_row, after_size);
#endif

  Rows_log_event* const ev=
    binlog_prepare_pending_rows_event(table, server_id,
				      before_size + after_size, is_trans,
				      static_cast<Update_rows_log_event*>(0),
                                      extra_row_info);

  if (unlikely(ev == 0))
    return HA_ERR_OUT_OF_MEM;

  error= ev->add_row_data(before_row, before_size) ||
         ev->add_row_data(after_row, after_size);

  /* restore read/write set for the rest of execution */
  table->column_bitmaps_set_no_signal(old_read_set,
                                      old_write_set);

  return error;
}

int THD::binlog_delete_row(TABLE* table, bool is_trans,
                           uchar const *record,
                           const uchar* extra_row_info)
{
  DBUG_ASSERT(is_current_stmt_binlog_format_row() && mysql_bin_log.is_open());
  int error= 0;

  /**
    Save a reference to the original read and write set bitmaps.
    We will need this to restore the bitmaps at the end.
   */
  MY_BITMAP *old_read_set= table->read_set;
  MY_BITMAP *old_write_set= table->write_set;

  /**
     This will remove spurious fields required during execution but
     not needed for binlogging. This is done according to the:
     binlog-row-image option.
   */
  binlog_prepare_row_images(table, false);

  /*
     Pack records into format for transfer. We are allocating more
     memory than needed, but that doesn't matter.
  */
  Row_data_memory memory(table, max_row_length(table, record));
  if (unlikely(!memory.has_memory()))
    return HA_ERR_OUT_OF_MEM;

  uchar *row_data= memory.slot(0);

  DBUG_DUMP("table->read_set", (uchar*) table->read_set->bitmap, (table->s->fields + 7) / 8);
  size_t const len= pack_row(table, table->read_set, row_data, record);

  Rows_log_event* const ev=
    binlog_prepare_pending_rows_event(table, server_id, len, is_trans,
				      static_cast<Delete_rows_log_event*>(0),
                                      extra_row_info);

  if (unlikely(ev == 0))
    return HA_ERR_OUT_OF_MEM;

  error= ev->add_row_data(row_data, len);

  /* restore read/write set for the rest of execution */
  table->column_bitmaps_set_no_signal(old_read_set,
                                      old_write_set);

  return error;
}

void THD::binlog_prepare_row_images(TABLE *table, bool is_update)
{
  DBUG_ENTER("THD::binlog_prepare_row_images");
  /**
    Remove spurious columns. The write_set has been partially
    handled before in table->mark_columns_needed_for_update.
   */

  DBUG_PRINT_BITSET("debug", "table->read_set (before preparing): %s", table->read_set);
  THD *thd= table->in_use;

  /* Handle the read set */
  /**
    if there is a primary key in the table (ie, user declared PK or a
    non-null unique index) and we dont want to ship the entire image,
    and the handler involved supports this.
   */
  if (table->s->primary_key < MAX_KEY &&
      (thd->variables.binlog_row_image < BINLOG_ROW_IMAGE_FULL) &&
      !ha_check_storage_engine_flag(table->s->db_type(), HTON_NO_BINLOG_ROW_OPT))
  {
    /**
      Just to be sure that tmp_set is currently not in use as
      the read_set already.
    */
    DBUG_ASSERT(table->read_set != &table->tmp_set);

    bitmap_clear_all(&table->tmp_set);

    switch(thd->variables.binlog_row_image)
    {
      case BINLOG_ROW_IMAGE_MINIMAL:
        /* MINIMAL: Mark only PK */
        table->mark_columns_used_by_index_no_reset(table->s->primary_key,
                                                   &table->tmp_set);
        break;
      case BINLOG_ROW_IMAGE_NOBLOB:
        /**
          NOBLOB: Remove unnecessary BLOB fields from read_set
                  (the ones that are not part of PK).
         */
        bitmap_union(&table->tmp_set, table->read_set);
        for (Field **ptr=table->field ; *ptr ; ptr++)
        {
          Field *field= (*ptr);
          if ((field->type() == MYSQL_TYPE_BLOB) &&
              !(field->flags & PRI_KEY_FLAG))
            bitmap_clear_bit(&table->tmp_set, field->field_index);
        }
        break;
      default:
        DBUG_ASSERT(0); // impossible.
    }

    /* set the temporary read_set */
    table->column_bitmaps_set_no_signal(&table->tmp_set,
                                        table->write_set);
  }

  /* Now, handle the write set */
  if (is_update &&
      thd->variables.binlog_row_image != BINLOG_ROW_IMAGE_FULL &&
      !ha_check_storage_engine_flag(table->s->db_type(),
                                    HTON_NO_BINLOG_ROW_OPT))
  {
    /**
      Just to be sure that tmp_write_set is currently not in use as
      the write_set already.
    */
    DBUG_ASSERT(table->write_set != &table->tmp_write_set);

    bitmap_copy(&table->tmp_write_set, table->write_set);

    for (Field **ptr=table->field ; *ptr ; ptr++)
    {
      Field *field= (*ptr);
      if (bitmap_is_set(&table->tmp_write_set, field->field_index))
      {
        /* When image type is NOBLOB, we prune only BLOB fields */
        if (thd->variables.binlog_row_image == BINLOG_ROW_IMAGE_NOBLOB &&
            field->type() != MYSQL_TYPE_BLOB)
          continue;

        /* compare null bit */
        if (field->is_null() && field->is_null_in_record(table->record[1]))
            bitmap_clear_bit(&table->tmp_write_set, field->field_index);

        /* compare content, only if fields are not set to NULL */
        else if (!field->is_null() &&
                 !field->is_null_in_record(table->record[1]) &&
                 !field->cmp_binary_offset(table->s->rec_buff_length))
          bitmap_clear_bit(&table->tmp_write_set, field->field_index);
      }
    }
    table->column_bitmaps_set_no_signal(table->read_set,
                                        &table->tmp_write_set);
  }

  DBUG_PRINT_BITSET("debug", "table->read_set (after preparing): %s", table->read_set);
  DBUG_VOID_RETURN;
}


int THD::binlog_flush_pending_rows_event(bool stmt_end, bool is_transactional)
{
  DBUG_ENTER("THD::binlog_flush_pending_rows_event");
  /*
    We shall flush the pending event even if we are not in row-based
    mode: it might be the case that we left row-based mode before
    flushing anything (e.g., if we have explicitly locked tables).
   */
  if (!mysql_bin_log.is_open())
    DBUG_RETURN(0);

  /*
    Mark the event as the last event of a statement if the stmt_end
    flag is set.
  */
  int error= 0;
  if (Rows_log_event *pending= binlog_get_pending_rows_event(is_transactional))
  {
    if (stmt_end)
    {
      pending->set_flags(Rows_log_event::STMT_END_F);
      binlog_table_maps= 0;
    }

    error= mysql_bin_log.flush_and_set_pending_rows_event(this, 0,
                                                          is_transactional);
  }

  DBUG_RETURN(error);
}


void THD::binlog_reset_pending_rows_event(bool is_transactional)
{
  DBUG_ENTER("THD::binlog_reset_pending_rows_event");
  binlog_cache_mngr *const cache_mngr= thd_get_cache_mngr(this);
  if (!cache_mngr)
    DBUG_VOID_RETURN;

  binlog_cache_data *cache_data=
    cache_mngr->get_binlog_cache_data(is_transactional);
  DBUG_ASSERT(cache_data != NULL);
  cache_data->remove_pending_event();
  DBUG_ASSERT(binlog_get_pending_rows_event(is_transactional) == NULL);
  DBUG_VOID_RETURN;
}


/**
   binlog_row_event_extra_data_eq

   Comparator for two binlog row event extra data
   pointers.

   It compares their significant bytes.

   Null pointers are acceptable

   @param a
     first pointer

   @param b
     first pointer

   @return
     true if the referenced structures are equal
*/
bool
THD::binlog_row_event_extra_data_eq(const uchar* a,
                                    const uchar* b)
{
  return ((a == b) ||
          ((a != NULL) &&
           (b != NULL) &&
           (a[EXTRA_ROW_INFO_LEN_OFFSET] ==
            b[EXTRA_ROW_INFO_LEN_OFFSET]) &&
           (memcmp(a, b,
                   a[EXTRA_ROW_INFO_LEN_OFFSET]) == 0)));
}

#if !defined(DBUG_OFF) && !defined(_lint)
static const char *
show_query_type(THD::enum_binlog_query_type qtype)
{
  switch (qtype) {
  case THD::ROW_QUERY_TYPE:
    return "ROW";
  case THD::STMT_QUERY_TYPE:
    return "STMT";
  case THD::QUERY_TYPE_COUNT:
  default:
    DBUG_ASSERT(0 <= qtype && qtype < THD::QUERY_TYPE_COUNT);
  }
  static char buf[64];
  sprintf(buf, "UNKNOWN#%d", qtype);
  return buf;
}
#endif

/**
  Auxiliary function to reset the limit unsafety warning suppression.
*/
static void reset_binlog_unsafe_suppression()
{
  DBUG_ENTER("reset_binlog_unsafe_suppression");
  unsafe_warning_suppression_is_activated= false;
  limit_unsafe_warning_count= 0;
  limit_unsafe_suppression_start_time= my_getsystime()/10000000;
  DBUG_VOID_RETURN;
}

/**
  Auxiliary function to print warning in the error log.
*/
static void print_unsafe_warning_to_log(int unsafe_type, char* buf,
                                 char* query)
{
  DBUG_ENTER("print_unsafe_warning_in_log");
  sprintf(buf, ER(ER_BINLOG_UNSAFE_STATEMENT),
          ER(LEX::binlog_stmt_unsafe_errcode[unsafe_type]));
  sql_print_warning(ER(ER_MESSAGE_AND_STATEMENT), buf, query);
  DBUG_VOID_RETURN;
}

/**
  Auxiliary function to check if the warning for limit unsafety should be
  thrown or suppressed. Details of the implementation can be found in the
  comments inline.
  SYNOPSIS:
  @params
   buf         - buffer to hold the warning message text
   unsafe_type - The type of unsafety.
   query       - The actual query statement.

  TODO: Remove this function and implement a general service for all warnings
  that would prevent flooding the error log.
*/
static void do_unsafe_limit_checkout(char* buf, int unsafe_type, char* query)
{
  ulonglong now;
  DBUG_ENTER("do_unsafe_limit_checkout");
  DBUG_ASSERT(unsafe_type == LEX::BINLOG_STMT_UNSAFE_LIMIT);
  limit_unsafe_warning_count++;
  /*
    INITIALIZING:
    If this is the first time this function is called with log warning
    enabled, the monitoring the unsafe warnings should start.
  */
  if (limit_unsafe_suppression_start_time == 0)
  {
    limit_unsafe_suppression_start_time= my_getsystime()/10000000;
    print_unsafe_warning_to_log(unsafe_type, buf, query);
  }
  else
  {
    if (!unsafe_warning_suppression_is_activated)
      print_unsafe_warning_to_log(unsafe_type, buf, query);

    if (limit_unsafe_warning_count >=
        LIMIT_UNSAFE_WARNING_ACTIVATION_THRESHOLD_COUNT)
    {
      now= my_getsystime()/10000000;
      if (!unsafe_warning_suppression_is_activated)
      {
        /*
          ACTIVATION:
          We got LIMIT_UNSAFE_WARNING_ACTIVATION_THRESHOLD_COUNT warnings in
          less than LIMIT_UNSAFE_WARNING_ACTIVATION_TIMEOUT we activate the
          suppression.
        */
        if ((now-limit_unsafe_suppression_start_time) <=
                       LIMIT_UNSAFE_WARNING_ACTIVATION_TIMEOUT)
        {
          unsafe_warning_suppression_is_activated= true;
          DBUG_PRINT("info",("A warning flood has been detected and the limit \
unsafety warning suppression has been activated."));
        }
        else
        {
          /*
           there is no flooding till now, therefore we restart the monitoring
          */
          limit_unsafe_suppression_start_time= my_getsystime()/10000000;
          limit_unsafe_warning_count= 0;
        }
      }
      else
      {
        /*
          Print the suppression note and the unsafe warning.
        */
        sql_print_information("The following warning was suppressed %d times \
during the last %d seconds in the error log",
                              limit_unsafe_warning_count,
                              (int)
                              (now-limit_unsafe_suppression_start_time));
        print_unsafe_warning_to_log(unsafe_type, buf, query);
        /*
          DEACTIVATION: We got LIMIT_UNSAFE_WARNING_ACTIVATION_THRESHOLD_COUNT
          warnings in more than  LIMIT_UNSAFE_WARNING_ACTIVATION_TIMEOUT, the
          suppression should be deactivated.
        */
        if ((now - limit_unsafe_suppression_start_time) >
            LIMIT_UNSAFE_WARNING_ACTIVATION_TIMEOUT)
        {
          reset_binlog_unsafe_suppression();
          DBUG_PRINT("info",("The limit unsafety warning supression has been \
deactivated"));
        }
      }
      limit_unsafe_warning_count= 0;
    }
  }
  DBUG_VOID_RETURN;
}

/**
  Auxiliary method used by @c binlog_query() to raise warnings.

  The type of warning and the type of unsafeness is stored in
  THD::binlog_unsafe_warning_flags.
*/
void THD::issue_unsafe_warnings()
{
  char buf[MYSQL_ERRMSG_SIZE * 2];
  DBUG_ENTER("issue_unsafe_warnings");
  /*
    Ensure that binlog_unsafe_warning_flags is big enough to hold all
    bits.  This is actually a constant expression.
  */
  DBUG_ASSERT(LEX::BINLOG_STMT_UNSAFE_COUNT <=
              sizeof(binlog_unsafe_warning_flags) * CHAR_BIT);

  uint32 unsafe_type_flags= binlog_unsafe_warning_flags;

  /*
    For each unsafe_type, check if the statement is unsafe in this way
    and issue a warning.
  */
  for (int unsafe_type=0;
       unsafe_type < LEX::BINLOG_STMT_UNSAFE_COUNT;
       unsafe_type++)
  {
    if ((unsafe_type_flags & (1 << unsafe_type)) != 0)
    {
      push_warning_printf(this, Sql_condition::WARN_LEVEL_NOTE,
                          ER_BINLOG_UNSAFE_STATEMENT,
                          ER(ER_BINLOG_UNSAFE_STATEMENT),
                          ER(LEX::binlog_stmt_unsafe_errcode[unsafe_type]));
      if (log_warnings >= 2)
      {
        if (unsafe_type == LEX::BINLOG_STMT_UNSAFE_LIMIT)
          do_unsafe_limit_checkout( buf, unsafe_type, query());
        else //cases other than LIMIT unsafety
          print_unsafe_warning_to_log(unsafe_type, buf, query());
      }
    }
  }

  /*  If this statement is unsafe for SBR, then log the query to slow log */
  if (log_sbr_unsafe && opt_slow_log && lex->is_stmt_unsafe() &&
      !log_throttle_sbr_unsafe_query.log(this, true))
  {
    // Prefix the log so it can be mined later
    const char* sbr_unsafe_log_prefix = "SBR_UNSAFE: ";
    size_t prefix_len = strlen(sbr_unsafe_log_prefix);
    size_t log_len = prefix_len + query_length();

    char* log_line = (char *)my_malloc(log_len + 1, MYF(MY_WME));
    if (log_line)
    {
      memcpy(log_line, sbr_unsafe_log_prefix, prefix_len);
      memcpy(log_line + prefix_len, query(), query_length());
      log_line[log_len] = 0;
       // Now log into slow log
      bool old_enable_slow_log = enable_slow_log;
      enable_slow_log = TRUE;
      slow_log_print(this, log_line, log_len, &status_var);
      my_free(log_line);
      enable_slow_log = old_enable_slow_log; // Restore old value
    }
  }
  DBUG_VOID_RETURN;
}

/**
  Log the current query.

  The query will be logged in either row format or statement format
  depending on the value of @c current_stmt_binlog_format_row field and
  the value of the @c qtype parameter.

  This function must be called:

  - After the all calls to ha_*_row() functions have been issued.

  - After any writes to system tables. Rationale: if system tables
    were written after a call to this function, and the master crashes
    after the call to this function and before writing the system
    tables, then the master and slave get out of sync.

  - Before tables are unlocked and closed.

  @see decide_logging_format

  @retval 0 Success

  @retval nonzero If there is a failure when writing the query (e.g.,
  write failure), then the error code is returned.
*/
int THD::binlog_query(THD::enum_binlog_query_type qtype, char const *query_arg,
                      ulong query_len, bool is_trans, bool direct,
                      bool suppress_use, int errcode)
{
  DBUG_ENTER("THD::binlog_query");
  DBUG_PRINT("enter", ("qtype: %s  query: '%s'",
                       show_query_type(qtype), query_arg));
  DBUG_ASSERT(query_arg && mysql_bin_log.is_open());

  if (get_binlog_local_stmt_filter() == BINLOG_FILTER_SET)
  {
    /*
      The current statement is to be ignored, and not written to
      the binlog. Do not call issue_unsafe_warnings().
    */
    DBUG_RETURN(0);
  }

  /*
    If we are not in prelocked mode, mysql_unlock_tables() will be
    called after this binlog_query(), so we have to flush the pending
    rows event with the STMT_END_F set to unlock all tables at the
    slave side as well.

    If we are in prelocked mode, the flushing will be done inside the
    top-most close_thread_tables().
  */
  if (this->locked_tables_mode <= LTM_LOCK_TABLES)
    if (int error= binlog_flush_pending_rows_event(TRUE, is_trans))
      DBUG_RETURN(error);

  /*
    Warnings for unsafe statements logged in statement format are
    printed in three places instead of in decide_logging_format().
    This is because the warnings should be printed only if the statement
    is actually logged. When executing decide_logging_format(), we cannot
    know for sure if the statement will be logged:

    1 - sp_head::execute_procedure which prints out warnings for calls to
    stored procedures.

    2 - sp_head::execute_function which prints out warnings for calls
    involving functions.

    3 - THD::binlog_query (here) which prints warning for top level
    statements not covered by the two cases above: i.e., if not insided a
    procedure and a function.

    Besides, we should not try to print these warnings if it is not
    possible to write statements to the binary log as it happens when
    the execution is inside a function, or generaly speaking, when
    the variables.option_bits & OPTION_BIN_LOG is false.
  */
  if ((variables.option_bits & OPTION_BIN_LOG) &&
      sp_runtime_ctx == NULL && !binlog_evt_union.do_union)
    issue_unsafe_warnings();

  switch (qtype) {
    /*
      ROW_QUERY_TYPE means that the statement may be logged either in
      row format or in statement format.  If
      current_stmt_binlog_format is row, it means that the
      statement has already been logged in row format and hence shall
      not be logged again.
    */
  case THD::ROW_QUERY_TYPE:
    DBUG_PRINT("debug",
               ("is_current_stmt_binlog_format_row: %d",
                is_current_stmt_binlog_format_row()));
    if (is_current_stmt_binlog_format_row())
      DBUG_RETURN(0);
    /* Fall through */

    /*
      STMT_QUERY_TYPE means that the query must be logged in statement
      format; it cannot be logged in row format.  This is typically
      used by DDL statements.  It is an error to use this query type
      if current_stmt_binlog_format_row is row.

      @todo Currently there are places that call this method with
      STMT_QUERY_TYPE and current_stmt_binlog_format is row.  Fix those
      places and add assert to ensure correct behavior. /Sven
    */
  case THD::STMT_QUERY_TYPE:
    /*
      The MYSQL_LOG::write() function will set the STMT_END_F flag and
      flush the pending rows event if necessary.
    */
    {
      Query_log_event qinfo(this, query_arg, query_len, is_trans, direct,
                            suppress_use, errcode);
      /*
        Binlog table maps will be irrelevant after a Query_log_event
        (they are just removed on the slave side) so after the query
        log event is written to the binary log, we pretend that no
        table maps were written.
       */
      int error= mysql_bin_log.write_event(&qinfo,
                                           /* default parameter */
                                           Log_event::EVENT_INVALID_CACHE,
                                           /* write meta data before query? */
                                           opt_binlog_trx_meta_data);
      binlog_table_maps= 0;
      DBUG_RETURN(error);
    }
    break;

  case THD::QUERY_TYPE_COUNT:
  default:
    DBUG_ASSERT(0 <= qtype && qtype < QUERY_TYPE_COUNT);
  }
  DBUG_RETURN(0);
}

std::atomic<st_filenum_pos> last_acked;
mysql_mutex_t LOCK_last_acked;
mysql_cond_t COND_last_acked;
#ifdef HAVE_PSI_INTERFACE
PSI_mutex_key key_LOCK_last_acked;
PSI_cond_key key_COND_last_acked;
#endif
bool semi_sync_last_ack_inited= false;

void init_semi_sync_last_acked()
{
  char *file_name= mysql_bin_log.engine_binlog_file +
                         dirname_length(mysql_bin_log.engine_binlog_file);
  st_filenum_pos coord= {
      mysql_bin_log.extract_file_index(file_name).second,
      static_cast<uint>(std::min<ulonglong>(st_filenum_pos::max_pos,
                                            mysql_bin_log.engine_binlog_pos))};
  // case: when in raft mode we cannot init the coords without consulting the
  // plugin, so we reset the coords
  if (enable_raft_plugin)
  {
    file_name= nullptr;
    coord.file_num= coord.pos= 0;
  }

  sql_print_information(
      "[rpl_wait_for_semi_sync_ack] Last ACKed pos initialized to: %s:%u",
      file_name, coord.pos);
  last_acked= coord;
  mysql_mutex_init(key_LOCK_last_acked, &LOCK_last_acked, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_COND_last_acked, &COND_last_acked, NULL);
  semi_sync_last_ack_inited= true;
}

void destroy_semi_sync_last_acked()
{
  if (semi_sync_last_ack_inited)
  {
    mysql_mutex_destroy(&LOCK_last_acked);
    mysql_cond_destroy(&COND_last_acked);
    semi_sync_last_ack_inited= false;
  }
}

bool wait_for_semi_sync_ack(const LOG_POS_COORD *const coord,
                            NET* net, ulonglong wait_timeout_nsec)
{
  const char *file_name= coord->file_name + dirname_length(coord->file_name);
  const st_filenum_pos current= {
      mysql_bin_log.extract_file_index(file_name).second,
      static_cast<uint>(
          std::min<ulonglong>(st_filenum_pos::max_pos, coord->pos))};

  // case: Check if we're not ahead of the last acked coord and short-circuit.
  // We need special handling for overflow, since after overflow `==`
  // comparisons don't make sense
  const auto snapshot_last_acked= last_acked.load();
  if (current < snapshot_last_acked ||
      (snapshot_last_acked.pos != st_filenum_pos::max_pos &&
       current == snapshot_last_acked))
  {
    return !current_thd->killed;
  }

  ulonglong timeout= 1000000000ULL; // one sec in nanosecs
  if (wait_timeout_nsec && wait_timeout_nsec < timeout)
    timeout= wait_timeout_nsec;
  PSI_stage_info old_stage;

  mysql_mutex_lock(&LOCK_last_acked);
  current_thd->ENTER_COND(&COND_last_acked,
                          &LOCK_last_acked,
                          &stage_slave_waiting_semi_sync_ack,
                          &old_stage);
  // TODO: there is a potential race here between global vars
  // (rpl_semi_sync_mater_enabled and rpl_wait_for_semi_sync_ack) being updated
  // and this thread going to conditional sleep, that's why we're looping on a
  // timedwait. All of this should be inside the semi-sync plugin but none
  // of the plugin callbacks are called for async threads, so this is the only
  // viable workaround.
  // case: wait till this log pos is <= to the last acked log pos, or if waiting
  // is not required anymore
  while (!current_thd->killed &&
         (rpl_semi_sync_master_enabled || enable_raft_plugin) &&
         rpl_wait_for_semi_sync_ack &&
         (current > last_acked.load() ||
          // We want to keep waiting till the next binlog rotation if we've hit
          // the max pos (overflow). It's okay to load last_acked twice since we
          // have locked the mutex at this point
          last_acked.load().pos == st_filenum_pos::max_pos))
  {
    ++repl_semi_sync_master_ack_waits;
    // wait for an ack for with a timeout, then retry if applicable
    struct timespec abstime;
    set_timespec_nsec(abstime, timeout);
    int ret= mysql_cond_timedwait(&COND_last_acked, &LOCK_last_acked, &abstime);
    // flush network buffers on timeout
    if (ret == ETIMEDOUT || ret == ETIME)
      net_flush(net);
  }
  current_thd->EXIT_COND(&old_stage);

  // return true only if we're alive i.e. we came here because we received a
  // successful ACK or if an ACK is no longer required
  return !current_thd->killed;
}

void signal_semi_sync_ack(const std::string &file, uint pos)
{
  const LOG_POS_COORD coord= { (char*) file.c_str(), pos };
  signal_semi_sync_ack(&coord);
}

void signal_semi_sync_ack(const LOG_POS_COORD *const acked_coord)
{
  const char *file_name=
      acked_coord->file_name + dirname_length(acked_coord->file_name);
  const st_filenum_pos acked= {
      mysql_bin_log.extract_file_index(file_name).second,
      // NOTE: If the acked pos cannot fit in st_filenum_pos::pos then we store
      // uint_max, this way we'll never send unacked trxs because the last acked
      // pos will be stuck at position uint_max in the current binlog file until
      // a rotation happens. This can only happen when a very large trx is
      // written to the binlog, max_binlog_size is capped at 1G but that's a
      // soft limit as one could still write more that 1G of binlogs in a single
      // trx, so when we hit this the log will immediately be rotated and things
      // will be back to normal.
      static_cast<uint>(std::min<ulonglong>(st_filenum_pos::max_pos,
                                            acked_coord->pos))};

  // case: nothing to update so no signal needed, let's exit
  if (acked <= last_acked.load()) { return; }

  mysql_mutex_lock(&LOCK_last_acked);
  if (acked > last_acked.load())
  {
    last_acked= acked;
    mysql_cond_broadcast(&COND_last_acked);
  }
  mysql_mutex_unlock(&LOCK_last_acked);
}

void reset_semi_sync_last_acked()
{
  mysql_mutex_lock(&LOCK_last_acked);
  last_acked= {0, 0};
  mysql_cond_broadcast(&COND_last_acked);
  mysql_mutex_unlock(&LOCK_last_acked);
}

#ifdef HAVE_REPLICATION
void block_all_dump_threads()
{
  block_dump_threads= true;
  kill_all_dump_threads();
}

void unblock_all_dump_threads()
{
  block_dump_threads= false;
}
#endif

int trim_logged_gtid(const std::vector<std::string>& trimmed_gtids)
{
  if (trimmed_gtids.empty())
    return 0;

  global_sid_lock->rdlock();

  int error = gtid_state->remove_logged_gtid_on_trim(trimmed_gtids);

#ifdef HAVE_REPLICATION
  if (active_mi && active_mi->rli)
  {
    // Remove rli logged gtids. Note that retrieved gtid is not cleared here
    // since it is going to be updated when the next gtid is fetched
    error = active_mi->rli->remove_logged_gtids(trimmed_gtids);
  }
  else
  {
    // NO_LINT_DEBUG
    sql_print_information("active_mi or rli is not set. Hence not trimming "
                          "logged gtids from rli");
  }
#endif

  global_sid_lock->unlock();

  return error;
}

int get_committed_gtids(const std::vector<std::string>& gtids,
                         std::vector<std::string> *committed_gtids)
{
  global_sid_lock->rdlock();

  for (const auto& gtid_s : gtids)
  {
    if (gtid_s.empty())
      continue;

    Gtid gtid;
    enum_return_status st= gtid.parse(global_sid_map, gtid_s.c_str());
    if (st != RETURN_STATUS_OK)
      return st;

    if (gtid_state->get_logged_gtids()->contains_gtid(gtid))
      committed_gtids->push_back(gtid_s);
  }
  global_sid_lock->unlock();

  return 0;
}

int get_executed_gtids(std::string* const gtids) {
  char* gtid_set_buffer= NULL;
  global_sid_lock->wrlock();
  const Gtid_set* gtid_set= gtid_state->get_logged_gtids();
  if (gtid_set->to_string(&gtid_set_buffer) < 0)
  {
    global_sid_lock->unlock();
    my_free(gtid_set_buffer);
    return 1;
  }
  global_sid_lock->unlock();
  *gtids = std::string(gtid_set_buffer);
  my_free(gtid_set_buffer);
  return 0;
}

#endif /* !defined(MYSQL_CLIENT) */

struct st_mysql_storage_engine binlog_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

/** @} */

mysql_declare_plugin(binlog)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &binlog_storage_engine,
  "binlog",
  "MySQL AB",
  "This is a pseudo storage engine to represent the binlog in a transaction",
  PLUGIN_LICENSE_GPL,
  binlog_init, /* Plugin Init */
  NULL, /* Plugin Deinit */
  0x0100 /* 1.0 */,
  NULL,                       /* status variables                */
  NULL,                       /* system variables                */
  NULL,                       /* config options                  */
  0,
}
mysql_declare_plugin_end;
