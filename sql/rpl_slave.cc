/* Copyright (c) 2000, 2016, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA */


/**
  @addtogroup Replication
  @{

  @file

  @brief Code to run the io thread and the sql thread on the
  replication slave.
*/

#include "sql_priv.h"
#include "my_global.h"
#include "rpl_slave.h"
#include "sql_parse.h"                         // execute_init_command
#include "sql_table.h"                         // mysql_rm_table
#include "rpl_mi.h"
#include "rpl_rli.h"
#include "rpl_filter.h"
#include "rpl_info_factory.h"
#include "transaction.h"
#include <thr_alarm.h>
#include <my_dir.h>
#include <sql_common.h>
#include <errmsg.h>
#include <mysqld_error.h>
#include <mysys_err.h>
#include "rpl_handler.h"
#include "rpl_info_dummy.h"
#include <signal.h>
#include <mysql.h>
#include <myisam.h>

#include "sql_base.h"                           // close_thread_tables
#include "tztime.h"                             // struct Time_zone
#include "log_event.h"                          // Rotate_log_event,
                                                // Create_file_log_event,
                                                // Format_description_log_event
#include "dynamic_ids.h"
#include "rpl_rli_pdb.h"
#include "global_threads.h"

#include <thread>

#ifdef HAVE_REPLICATION

#include "rpl_tblmap.h"
#include "debug_sync.h"
#include "dependency_slave_worker.h"
#include "rpl_slave_commit_order_manager.h"    // Commit_order_manager
#include <chrono>
#include "slave_stats_daemon.h"  // stop_handle_slave_stats_daemon, start_handle_slave_stats_daemon

using std::min;
using std::max;
using namespace std::chrono;

#define FLAGSTR(V,F) ((V)&(F)?#F" ":"")

#define MAX_SLAVE_RETRY_PAUSE 5
/*
  a parameter of sql_slave_killed() to defer the killed status
*/
#define SLAVE_WAIT_GROUP_DONE 60
bool use_slave_mask = 0;
MY_BITMAP slave_error_mask;
char slave_skip_error_names[SHOW_VAR_FUNC_BUFF_SIZE];

#if OPENSSL_VERSION_NUMBER >= 0x10100000L
// Function removed after OpenSSL 1.1.0
#define ERR_remove_state(x)
#endif

static unsigned long stop_wait_timeout;
char* slave_load_tmpdir = 0;
Master_info *active_mi= 0;
my_bool replicate_same_server_id;
ulonglong relay_log_space_limit = 0;
uint rpl_receive_buffer_size = 0;
my_bool reset_seconds_behind_master = TRUE;
uint unique_check_lag_threshold;
uint unique_check_lag_reset_threshold;

const char *relay_log_index= 0;
const char *relay_log_basename= 0;

/*
  MTS load-ballancing parameter.
  Max length of one MTS Worker queue. The value also determines the size
  of Relay_log_info::gaq (see @c slave_start_workers()).
  It can be set to any value in [1, ULONG_MAX - 1] range.
*/
const ulong mts_slave_worker_queue_len_max= 16384;

/*
  Statistics go to the error log every # of seconds when --log-warnings > 1
*/
const long mts_online_stat_period= 60 * 2;


/*
  MTS load-ballancing parameter.
  Time unit in microsecs to sleep by MTS Coordinator to avoid extra thread
  signalling in the case of Worker queues are close to be filled up.
*/
const ulong mts_coordinator_basic_nap= 5;

/*
  MTS load-ballancing parameter.
  Percent of Worker queue size at which Worker is considered to become
  hungry.

  C enqueues --+                   . underrun level
               V                   "
   +----------+-+------------------+--------------+
   | empty    |.|::::::::::::::::::|xxxxxxxxxxxxxx| ---> Worker dequeues
   +----------+-+------------------+--------------+

   Like in the above diagram enqueuing to the x-d area would indicate
   actual underrruning by Worker.
*/
const ulong mts_worker_underrun_level= 10;

Slave_job_item * de_queue(Slave_jobs_queue *jobs, Slave_job_item *ret);
bool append_item_to_jobs(slave_job_item *job_item,
                         Slave_worker *w, Relay_log_info *rli);

/**
  Thread state for the SQL slave thread.
*/

class THD_SQL_slave : public THD
{
private:
  char *m_buffer;
  size_t m_buffer_size;

public:
  /**
    Constructor, used to represent slave thread state.

    @param buffer Statically-allocated buffer.
    @param size   Size of the passed buffer.
  */
  THD_SQL_slave(char *buffer, size_t size)
  : m_buffer(buffer), m_buffer_size(size)
  {
    DBUG_ASSERT(buffer && size);
    memset(m_buffer, 0, m_buffer_size);
  }

  virtual ~THD_SQL_slave()
  {
    m_buffer[0]= '\0';
  }

  void print_proc_info(const char *fmt, ...);
};


/**
  Print an information (state) string for this slave thread.

  @param fmt  Specifies how subsequent arguments are converted for output.
  @param ...  Variable number of arguments.
*/

void THD_SQL_slave::print_proc_info(const char *fmt, ...)
{
  va_list args;

  va_start(args, fmt);
  my_vsnprintf(m_buffer, m_buffer_size - 1, fmt, args);
  va_end(args);

  /* Set field directly, profiling is not useful in a slave thread anyway. */
  proc_info= m_buffer;
}


/*
  When slave thread exits, we need to remember the temporary tables so we
  can re-use them on slave start.

  TODO: move the vars below under Master_info
*/

int disconnect_slave_event_count = 0, abort_slave_event_count = 0;

static pthread_key(Master_info*, RPL_MASTER_INFO);

/* Count binlog events by type processed by the SQL slave */
ulonglong repl_event_counts[ENUM_END_EVENT] = { 0 };
ulonglong repl_event_count_other= 0;

/* Time binlog events by type processed by the SQL slave */
ulonglong repl_event_times[ENUM_END_EVENT] = { 0 };
ulonglong repl_event_time_other= 0;

enum enum_slave_reconnect_actions
{
  SLAVE_RECON_ACT_REG= 0,
  SLAVE_RECON_ACT_DUMP= 1,
  SLAVE_RECON_ACT_EVENT= 2,
  SLAVE_RECON_ACT_MAX
};

enum enum_slave_reconnect_messages
{
  SLAVE_RECON_MSG_WAIT= 0,
  SLAVE_RECON_MSG_KILLED_WAITING= 1,
  SLAVE_RECON_MSG_AFTER= 2,
  SLAVE_RECON_MSG_FAILED= 3,
  SLAVE_RECON_MSG_COMMAND= 4,
  SLAVE_RECON_MSG_KILLED_AFTER= 5,
  SLAVE_RECON_MSG_MAX
};

static const char *reconnect_messages[SLAVE_RECON_ACT_MAX][SLAVE_RECON_MSG_MAX]=
{
  {
    "Waiting to reconnect after a failed registration on master",
    "Slave I/O thread killed while waitnig to reconnect after a failed \
registration on master",
    "Reconnecting after a failed registration on master",
    "failed registering on master, reconnecting to try again, \
log '%s' at position %s",
    "COM_REGISTER_SLAVE",
    "Slave I/O thread killed during or after reconnect"
  },
  {
    "Waiting to reconnect after a failed binlog dump request",
    "Slave I/O thread killed while retrying master dump",
    "Reconnecting after a failed binlog dump request",
    "failed dump request, reconnecting to try again, log '%s' at position %s",
    "COM_BINLOG_DUMP",
    "Slave I/O thread killed during or after reconnect"
  },
  {
    "Waiting to reconnect after a failed master event read",
    "Slave I/O thread killed while waiting to reconnect after a failed read",
    "Reconnecting after a failed master event read",
    "Slave I/O thread: Failed reading log event, reconnecting to retry, \
log '%s' at position %s",
    "",
    "Slave I/O thread killed during or after a reconnect done to recover from \
failed read"
  }
};

enum enum_slave_apply_event_and_update_pos_retval
{
  SLAVE_APPLY_EVENT_AND_UPDATE_POS_OK= 0,
  SLAVE_APPLY_EVENT_AND_UPDATE_POS_APPLY_ERROR= 1,
  SLAVE_APPLY_EVENT_AND_UPDATE_POS_UPDATE_POS_ERROR= 2,
  SLAVE_APPLY_EVENT_AND_UPDATE_POS_APPEND_JOB_ERROR= 3,
  SLAVE_APPLY_EVENT_AND_UPDATE_POS_MAX
};


static int process_io_rotate(Master_info* mi, Rotate_log_event* rev);
static int process_io_create_file(Master_info* mi, Create_file_log_event* cev);
static bool wait_for_relay_log_space(Relay_log_info* rli);
static inline bool io_slave_killed(THD* thd,Master_info* mi);
static inline bool sql_slave_killed(THD* thd,Relay_log_info* rli);
static inline bool is_autocommit_off_and_infotables(THD* thd);
static int init_slave_thread(THD* thd, SLAVE_THD_TYPE thd_type);
static void print_slave_skip_errors(void);
static int safe_connect(THD* thd, MYSQL* mysql, Master_info* mi);
static int safe_reconnect(THD* thd, MYSQL* mysql, Master_info* mi,
                          bool suppress_warnings);
static int connect_to_master(THD* thd, MYSQL* mysql, Master_info* mi,
                             bool reconnect, bool suppress_warnings);
static int get_master_version_and_clock(MYSQL* mysql, Master_info* mi);
static int get_master_uuid(MYSQL *mysql, Master_info *mi);
int io_thread_init_commands(MYSQL *mysql, Master_info *mi);
static Log_event* next_event(Relay_log_info* rli);
static int queue_event(Master_info* mi,const char* buf,ulong event_len);
static void set_stop_slave_wait_timeout(unsigned long wait_timeout);
static int terminate_slave_thread(THD *thd,
                                  mysql_mutex_t *term_lock,
                                  mysql_cond_t *term_cond,
                                  volatile uint *slave_running,
                                  bool need_lock_term);
static bool check_io_slave_killed(THD *thd, Master_info *mi, const char *info);
int slave_worker_exec_job(Slave_worker * w, Relay_log_info *rli);
static int mts_event_coord_cmp(LOG_POS_COORD *id1, LOG_POS_COORD *id2);


static void log_slave_command(THD *thd)
{
  if (!thd->security_ctx || !thd->security_ctx->user)
    return;

  const char *user = (char *)thd->security_ctx->user;
  const char *host = thd->security_ctx->host_or_ip[0] ?
    thd->security_ctx->host_or_ip :
    (char *)thd->security_ctx->get_host()->ptr();
  const char *query = thd->query();

  sql_print_information("Executing slave command '%s' by user %s from host %s",
                        query, user , host);
}

/*
  Function to set the slave's max_allowed_packet based on the value
  of slave_max_allowed_packet.

    @in_param    thd    Thread handler for slave
    @in_param    mysql  MySQL connection handle
*/

static void set_slave_max_allowed_packet(THD *thd, MYSQL *mysql)
{
  DBUG_ENTER("set_slave_max_allowed_packet");
  // thd and mysql must be valid
  DBUG_ASSERT(thd && mysql);

  thd->variables.max_allowed_packet= slave_max_allowed_packet;
  NET* net = thd->get_net();
  net->max_packet_size= slave_max_allowed_packet;
  /*
    Adding MAX_LOG_EVENT_HEADER_LEN to the max_packet_size on the I/O
    thread and the mysql->option max_allowed_packet, since a
    replication event can become this much  larger than
    the corresponding packet (query) sent from client to master.
  */
  net->max_packet_size+= MAX_LOG_EVENT_HEADER;
  /*
    Skipping the setting of mysql->net.max_packet size to slave
    max_allowed_packet since this is done during mysql_real_connect.
  */
  mysql->options.max_allowed_packet=
    slave_max_allowed_packet+MAX_LOG_EVENT_HEADER;
  DBUG_VOID_RETURN;
}

/*
  Find out which replications threads are running

  SYNOPSIS
    init_thread_mask()
    mask                Return value here
    mi                  master_info for slave
    inverse             If set, returns which threads are not running

  IMPLEMENTATION
    Get a bit mask for which threads are running so that we can later restart
    these threads.

  RETURN
    mask        If inverse == 0, running threads
                If inverse == 1, stopped threads
*/

void init_thread_mask(int* mask, Master_info* mi, bool inverse)
{
  bool set_io = mi->slave_running;
  bool set_sql = mi->rli->slave_running;
  register int tmp_mask=0;
  DBUG_ENTER("init_thread_mask");

  if (set_io)
    tmp_mask |= SLAVE_IO;
  if (set_sql)
    tmp_mask |= SLAVE_SQL;
  if (inverse)
    tmp_mask^= (SLAVE_IO | SLAVE_SQL);
  *mask = tmp_mask;
  DBUG_VOID_RETURN;
}


/*
  lock_slave_threads()
*/

void lock_slave_threads(Master_info* mi)
{
  DBUG_ENTER("lock_slave_threads");

  //TODO: see if we can do this without dual mutex
  mysql_mutex_lock(&mi->run_lock);
  mysql_mutex_lock(&mi->rli->run_lock);
  DBUG_VOID_RETURN;
}


/*
  unlock_slave_threads()
*/

void unlock_slave_threads(Master_info* mi)
{
  DBUG_ENTER("unlock_slave_threads");

  //TODO: see if we can do this without dual mutex
  mysql_mutex_unlock(&mi->rli->run_lock);
  mysql_mutex_unlock(&mi->run_lock);
  DBUG_VOID_RETURN;
}

#ifdef HAVE_PSI_INTERFACE
static PSI_thread_key key_thread_slave_io, key_thread_slave_sql, key_thread_slave_worker;

static PSI_thread_info all_slave_threads[]=
{
  { &key_thread_slave_io, "slave_io", PSI_FLAG_GLOBAL},
  { &key_thread_slave_sql, "slave_sql", PSI_FLAG_GLOBAL},
  { &key_thread_slave_worker, "slave_worker", PSI_FLAG_GLOBAL}
};

static void init_slave_psi_keys(void)
{
  const char* category= "sql";
  int count;

  count= array_elements(all_slave_threads);
  mysql_thread_register(category, all_slave_threads, count);
}
#endif /* HAVE_PSI_INTERFACE */

/* Initialize slave structures */

int init_slave()
{
  DBUG_ENTER("init_slave");

  int error= 0;
  int thread_mask= SLAVE_SQL;
  Relay_log_info* rli= NULL;

  // No IO thread in raft mode
  if (!enable_raft_plugin)
    thread_mask |= SLAVE_IO;

#ifdef HAVE_PSI_INTERFACE
  init_slave_psi_keys();
#endif

  /*
    This is called when mysqld starts. Before client connections are
    accepted. However bootstrap may conflict with us if it does START SLAVE.
    So it's safer to take the lock.
  */
  mysql_mutex_lock(&LOCK_active_mi);

  if (pthread_key_create(&RPL_MASTER_INFO, NULL))
    DBUG_RETURN(1);

  if ((error= Rpl_info_factory::create_coordinators(opt_mi_repository_id, &active_mi,
                                                    opt_rli_repository_id, &rli)))
  {
    sql_print_error("Failed to create or recover replication info repository.");
    error= 1;
    goto err;
  }

  /*
    This is the startup routine and as such we try to
    configure both the SLAVE_SQL and SLAVE_IO.
  */
  if (global_init_info(
        active_mi, true, thread_mask, /*need_lock=*/true, /*startup=*/true))
  {
    sql_print_error("Failed to initialize the master info structure");
    error= 1;
    goto err;
  }

  is_slave = active_mi->host[0];
  DBUG_PRINT("info", ("init group master %s %lu  group relay %s %lu event %s %lu\n",
    rli->get_group_master_log_name(),
    (ulong) rli->get_group_master_log_pos(),
    rli->get_group_relay_log_name(),
    (ulong) rli->get_group_relay_log_pos(),
    rli->get_event_relay_log_name(),
    (ulong) rli->get_event_relay_log_pos()));

  /**
    If engine binlog max gtid is set, then update recovery_max_engine_gtid.
    recovery_max_engine_gtid is used later during slave's idempotent
    recovery/apply of binnlog events.
    engine_binlog_max_gtid is set during storage engine recovery using
    global_sid_map. However, idempotent recovery/apply uses
    rli->recovery_sid_map. Hence rli->recovery_max_engine_gtid needs to be
    initialized by hashing into rli->recovery_sid_map
   */
  if (!mysql_bin_log.engine_binlog_max_gtid.empty())
  {
    char buf[Gtid::MAX_TEXT_LENGTH + 1];

    /* Extract engine_binlog_max_gtid using global_sid_map */
    global_sid_lock->rdlock();
    mysql_bin_log.engine_binlog_max_gtid.to_string(global_sid_map, buf);
    global_sid_lock->unlock();

    /* Now set rli->recovery_max_engine_gtid (and optionally add
       it into rli->recovery_sid_map */
    rli->recovery_sid_lock->rdlock();
    rli->recovery_max_engine_gtid.parse(rli->recovery_sid_map, buf);
    rli->recovery_sid_lock->unlock();
  }

  if (active_mi->host[0] &&
      mysql_bin_log.engine_binlog_pos != ULONGLONG_MAX &&
      mysql_bin_log.engine_binlog_file[0] &&
      gtid_mode > 0 &&
      !enable_raft_plugin)
  {
    /*
      With less durable settins (sync_binlog !=1 and
      innodb_flush_log_at_trx_commit !=1), a slave with GTIDs/MTS
      may be inconsistent due to the two possible scenarios below

      1) slave's binary log is behind innodb transaction log.
      2) slave's binlary log is ahead of innodb transaction log.

      The slave_gtid_info transaction table consistently stores
      gtid information and handles scenario 1. But in case
      of scenario 2, even though the slave_gtid_info is consistent
      with innodb, slave will skip executing some transactions if it's
      GTID is logged in the binlog even though it is not commiited in
      innodb.

      Scenario 2 is handled by changing gtid_executed when a
      slave is initialized based on the binlog file and binlog position
      which are logged inside innodb trx log. When gtid_executed is set
      to an old value which is consistent with innodb, slave doesn't
      miss any transactions.
    */

    /*
     * This entire block is skipped in raft mode since the executed gtid set is
     * calculated correctly based on engine position and filename during
     * transaction log (binlog or apply-log) recovery and gtid initialization
     */
    mysql_mutex_t *log_lock = mysql_bin_log.get_log_lock();
    mysql_mutex_lock(log_lock);
    global_sid_lock->wrlock();
    char file_name[FN_REFLEN + 1];
    mysql_bin_log.make_log_name(file_name,
                                mysql_bin_log.engine_binlog_file);

    const_cast<Gtid_set *>(gtid_state->get_logged_gtids())->clear();
    MYSQL_BIN_LOG::enum_read_gtids_from_binlog_status ret =
      mysql_bin_log.read_gtids_from_binlog(file_name,
                                           const_cast<Gtid_set *>(
                                             gtid_state->get_logged_gtids()),
                                           NULL, NULL, NULL, global_sid_map,
                                           opt_master_verify_checksum,
                                           mysql_bin_log.engine_binlog_pos);
    global_sid_lock->unlock();
    // rotate writes the consistent gtid_executed as previous_gtid_log_event
    // in next binlog. This is done to avoid situations where there is a
    // slave crash immediately after executing some relay log events.
    // Those slave crashes are not safe if binlog is not rotated since the
    // gtid_executed set after crash recovery will be inconsistent with InnoDB.
    // A crash before this rotate is safe because of valid binlog file and
    // position values inside innodb trx header which will not be updated
    // until sql_thread is ready.
    bool check_purge;
    mysql_bin_log.rotate(true, &check_purge);
    mysql_mutex_unlock(log_lock);
    if (ret == MYSQL_BIN_LOG::ERROR || ret == MYSQL_BIN_LOG::TRUNCATED)
    {
      sql_print_error("Failed to read log %s up to pos %llu "
                       "to find out crash safe gtid_executed "
                       "Replication will not be setup due to "
                       "possible data inconsistency with master. ",
                       mysql_bin_log.engine_binlog_file,
                       mysql_bin_log.engine_binlog_pos);
      error = 1;
      goto err;
    }
  }

  /* If server id is not set, start_slave_thread() will say it */
  if (active_mi->host[0] && !opt_skip_slave_start)
  {
    /* same as in start_slave() cache the global var values into rli's members */
    active_mi->rli->opt_slave_parallel_workers= opt_mts_slave_parallel_workers;
    active_mi->rli->checkpoint_group= opt_mts_checkpoint_group;
    if (start_slave_threads(true/*need_lock_slave=true*/,
                            false/*wait_for_start=false*/,
                            active_mi,
                            thread_mask))
    {
      sql_print_error("Failed to create slave threads");
      error= 1;
      goto err;
    }
  }

err:
  mysql_mutex_unlock(&LOCK_active_mi);
  if (error)
    sql_print_information("Check error log for additional messages. "
                          "You will not be able to start replication until "
                          "the issue is resolved and the server restarted.");
  DBUG_RETURN(error);
}

/**
   Parse the given relay log and identify the rotate event from the master.
   Ignore the Format description event, Previous_gtid log event and ignorable
   events within the relay log. When a rotate event is found check if it is a
   rotate that is originated from the master or not based on the server_id. If
   the rotate is from slave or if it is a fake rotate event ignore the event.
   If any other events are encountered apart from the above events generate an
   error. From the rotate event extract the master's binary log name and
   position.

   @param filename
          Relay log name which needs to be parsed.

   @param[OUT] master_log_file
          Set the master_log_file to the log file name that is extracted from
          rotate event. The master_log_file should contain string of len
          FN_REFLEN.

   @param[OUT] master_log_pos
          Set the master_log_pos to the log position extracted from rotate
          event.

   @retval FOUND_ROTATE: When rotate event is found in the relay log
   @retval NOT_FOUND_ROTATE: When rotate event is not found in the relay log
   @retval ERROR: On error
 */
enum enum_read_rotate_from_relay_log_status
{ FOUND_ROTATE, NOT_FOUND_ROTATE, ERROR };

static enum_read_rotate_from_relay_log_status
read_rotate_from_relay_log(char *filename, char *master_log_file,
                           my_off_t *master_log_pos)
{
  DBUG_ENTER("read_rotate_from_relay_log");
  /*
    Create a Format_description_log_event that is used to read the
    first event of the log.
   */
  Format_description_log_event fd_ev(BINLOG_VERSION), *fd_ev_p= &fd_ev;
  DBUG_ASSERT(fd_ev.is_valid());
  IO_CACHE log;
  const char *errmsg= NULL;
  File file= open_binlog_file(&log, filename, &errmsg);
  if (file < 0)
  {
    sql_print_error("Error during --relay-log-recovery: %s", errmsg);
    DBUG_RETURN(ERROR);
  }
  my_b_seek(&log, BIN_LOG_HEADER_SIZE);
  Log_event *ev= NULL;
  bool done= false;
  enum_read_rotate_from_relay_log_status ret= NOT_FOUND_ROTATE;
  while (!done &&
         (ev= Log_event::read_log_event(&log, 0, fd_ev_p, opt_slave_sql_verify_checksum, NULL)) !=
         NULL)
  {
    DBUG_PRINT("info", ("Read event of type %s", ev->get_type_str()));
    switch (ev->get_type_code())
    {
    case FORMAT_DESCRIPTION_EVENT:
      if (fd_ev_p != &fd_ev)
        delete fd_ev_p;
      fd_ev_p= (Format_description_log_event *)ev;
      break;
    case ROTATE_EVENT:
      /*
        Check for rotate event from the master. Ignore the ROTATE event if it
        is a fake rotate event with server_id=0.
       */
      if (ev->server_id && ev->server_id != ::server_id)
      {
        Rotate_log_event *rotate_ev= (Rotate_log_event *)ev;
        DBUG_ASSERT(FN_REFLEN >= rotate_ev->ident_len + 1);
        memcpy(master_log_file, rotate_ev->new_log_ident, rotate_ev->ident_len + 1);
        *master_log_pos= rotate_ev->pos;
        ret= FOUND_ROTATE;
        done= true;
      }
      break;
    case PREVIOUS_GTIDS_LOG_EVENT:
      break;
    case IGNORABLE_LOG_EVENT:
      break;
    default:
      sql_print_error("Error during --relay-log-recovery: Could not locate "
                      "rotate event from the master.");
      ret= ERROR;
      done= true;
      break;
    }
    if (ev != fd_ev_p)
      delete ev;
  }
  if (log.error < 0)
  {
    sql_print_error("Error during --relay-log-recovery: Error reading events from relay log: %d",
                    log.error);
    DBUG_RETURN(ERROR);
  }

  if (fd_ev_p != &fd_ev)
  {
    delete fd_ev_p;
    fd_ev_p= &fd_ev;
  }

  if (mysql_file_close(file, MYF(MY_WME)))
    DBUG_RETURN(ERROR);
  if (end_io_cache(&log))
  {
    sql_print_error("Error during --relay-log-recovery: Error while freeing "
                    "IO_CACHE object");
    DBUG_RETURN(ERROR);
  }
  DBUG_RETURN(ret);
}

/**
   Reads relay logs one by one starting from the first relay log. Looks for
   the first rotate event from the master. If rotate is not found in the relay
   log search continues to next relay log. If rotate event from master is
   found then the extracted master_log_file and master_log_pos are used to set
   rli->group_master_log_name and rli->group_master_log_pos. If an error has
   occurred the error code is retuned back.

   @param rli
          Relay_log_info object to read relay log files and to set
          group_master_log_name and group_master_log_pos.

   @retval 0 On success
   @retval 1 On failure
 */
static int
find_first_relay_log_with_rotate_from_master(Relay_log_info* rli)
{
  DBUG_ENTER("find_first_relay_log_with_rotate_from_master");
  int error= 0;
  LOG_INFO linfo;
  bool got_rotate_from_master= false;
  int pos;
  char master_log_file[FN_REFLEN];
  my_off_t master_log_pos= 0;

  for (pos= rli->relay_log.find_log_pos(&linfo, NULL, true);
       !pos;
       pos= rli->relay_log.find_next_log(&linfo, true))
  {
    switch (read_rotate_from_relay_log(linfo.log_file_name, master_log_file,
                                       &master_log_pos))
    {
    case ERROR:
      error= 1;
      break;
    case FOUND_ROTATE:
      got_rotate_from_master= true;
      break;
    case NOT_FOUND_ROTATE:
      break;
    }
    if (error || got_rotate_from_master)
      break;
  }
  if (pos== LOG_INFO_IO)
  {
    error= 1;
    sql_print_error("Error during --relay-log-recovery: Could not read "
                    "relay log index file due to an IO error.");
    goto err;
  }
  if (pos== LOG_INFO_EOF)
  {
    error= 1;
    sql_print_error("Error during --relay-log-recovery: Could not locate "
                    "rotate event from master in relay log file.");
    goto err;
  }
  if (!error && got_rotate_from_master)
  {
    rli->set_group_master_log_name(master_log_file);
    rli->set_group_master_log_pos(master_log_pos);
  }
err:
  DBUG_RETURN(error);
}

/*
  Updates the master info based on the information stored in the
  relay info and ignores relay logs previously retrieved by the IO
  thread, which thus starts fetching again based on to the
  master_log_pos and master_log_name. Eventually, the old
  relay logs will be purged by the normal purge mechanism.

  When GTID's are enabled the "Retrieved GTID" set should be cleared
  so that partial read events are discarded and they are
  fetched once again

  @param mi    pointer to Master_info instance
*/
static void recover_relay_log(Master_info *mi)
{
  Relay_log_info *rli=mi->rli;
  // Set Receiver Thread's positions as per the recovered Applier Thread.
  mi->set_master_log_pos(max<ulonglong>(BIN_LOG_HEADER_SIZE,
                                        rli->get_group_master_log_pos()));
  mi->set_master_log_name(rli->get_group_master_log_name());

  sql_print_warning("Recovery from master pos %ld and file %s. "
                    "Previous relay log pos and relay log file had "
                    "been set to %lld, %s respectively.",
                    (ulong) mi->get_master_log_pos(), mi->get_master_log_name(),
                    rli->get_group_relay_log_pos(), rli->get_group_relay_log_name());

  // Start with a fresh relay log.
  rli->set_group_relay_log_name(rli->relay_log.get_log_fname());
  rli->set_event_relay_log_name(rli->relay_log.get_log_fname());
  rli->set_group_relay_log_pos(BIN_LOG_HEADER_SIZE);
  rli->set_event_relay_log_pos(BIN_LOG_HEADER_SIZE);
  /*
    Clear the retrieved GTID set so that events that are written partially
    will be fetched again.
  */
  if (gtid_mode == GTID_MODE_ON)
  {
    global_sid_lock->wrlock();
    (const_cast<Gtid_set *>(rli->get_gtid_set()))->clear();
    global_sid_lock->unlock();
  }
}


/*
  Updates the master info based on the information stored in the
  relay info and ignores relay logs previously retrieved by the IO
  thread, which thus starts fetching again based on to the
  master_log_pos and master_log_name. Eventually, the old
  relay logs will be purged by the normal purge mechanism.

  There can be a special case where rli->group_master_log_name and
  rli->group_master_log_pos are not intialized, as the sql thread was never
  started at all. In those cases all the existing relay logs are parsed
  starting from the first one and the initial rotate event that was received
  from the master is identified. From the rotate event master_log_name and
  master_log_pos are extracted and they are set to rli->group_master_log_name
  and rli->group_master_log_pos.

  In the feature, we should improve this routine in order to avoid throwing
  away logs that are safely stored in the disk. Note also that this recovery
  routine relies on the correctness of the relay-log.info and only tolerates
  coordinate problems in master.info.

  In this function, there is no need for a mutex as the caller
  (i.e. init_slave) already has one acquired.

  Specifically, the following structures are updated:

  1 - mi->master_log_pos  <-- rli->group_master_log_pos
  2 - mi->master_log_name <-- rli->group_master_log_name
  3 - It moves the relay log to the new relay log file, by
      rli->group_relay_log_pos  <-- BIN_LOG_HEADER_SIZE;
      rli->event_relay_log_pos  <-- BIN_LOG_HEADER_SIZE;
      rli->group_relay_log_name <-- rli->relay_log.get_log_fname();
      rli->event_relay_log_name <-- rli->relay_log.get_log_fname();

   If there is an error, it returns (1), otherwise returns (0).
 */
int init_recovery(Master_info* mi, const char** errmsg)
{
  DBUG_ENTER("init_recovery");

  int error= 0;
  Relay_log_info *rli= mi->rli;
  char *group_master_log_name= NULL;

  /**
     Avoid mts_recovery_groups if gtid_mode is ON.
  */
  if (rli->recovery_parallel_workers && gtid_mode == 0)
  {
    /*
      This is not idempotent and a crash after this function and before
      the recovery is actually done may lead the system to an inconsistent
      state.

      This may happen because the gap is not persitent stored anywhere
      and eventually old relay log files will be removed and further
      calculations on the gaps will be impossible.

      We need to improve this. /Alfranio.
    */
    error= mts_recovery_groups(rli);
    if (rli->mts_recovery_group_cnt)
    {
      if (gtid_mode == GTID_MODE_ON)
      {
        rli->recovery_parallel_workers= 0;
        rli->clear_mts_recovery_groups();
      }
      else
        DBUG_RETURN(error);
    }
  }

  group_master_log_name= const_cast<char *>(rli->get_group_master_log_name());
  if (!error)
  {
    if (!group_master_log_name[0])
    {
      if (rli->replicate_same_server_id)
      {
        error= 1;
        sql_print_error("Error during --relay-log-recovery: "
                        "replicate_same_server_id is in use and sql thread's "
                        "positions are not initialized, hence relay log "
                        "recovery cannot happen.");
        DBUG_RETURN(error);
      }
      error= find_first_relay_log_with_rotate_from_master(rli);
      if (error)
        DBUG_RETURN(error);
    }
    recover_relay_log(mi);
  }
  DBUG_RETURN(error);
}

/*
  Relay log recovery in the case of MTS, is handled by the following function.
  Gaps in MTS execution are filled using implicit execution of
  START SLAVE UNTIL SQL_AFTER_MTS_GAPS call. Once slave reaches a consistent
  gapless state receiver thread's positions are initialized to applier thread's
  positions and the old relay logs are discarded. This completes the recovery
  process.

  @param mi    pointer to Master_info instance.

  @retval 0 success
  @retval 1 error
*/
static inline int fill_mts_gaps_and_recover(Master_info* mi)
{
  DBUG_ENTER("fill_mts_gaps_and_recover");
  Relay_log_info *rli= mi->rli;
  int recovery_error= 0;
  rli->is_relay_log_recovery= FALSE;
  rli->until_condition= Relay_log_info::UNTIL_SQL_AFTER_MTS_GAPS;
  rli->opt_slave_parallel_workers= rli->recovery_parallel_workers;
  sql_print_information("MTS recovery: starting coordinator thread to fill MTS "
                        "gaps.");
  recovery_error= start_slave_thread(
#ifdef HAVE_PSI_INTERFACE
                                     key_thread_slave_sql,
#endif
                                     handle_slave_sql, &rli->run_lock,
                                     &rli->run_lock,
                                     &rli->start_cond,
                                     &rli->slave_running,
                                     &rli->slave_run_id,
                                     mi);

  if (recovery_error)
  {
    sql_print_warning("MTS recovery: failed to start the coordinator "
                      "thread. Check the error log for additional"
                      " details.");
    goto err;
  }
  mysql_mutex_lock(&rli->run_lock);
  mysql_cond_wait(&rli->stop_cond, &rli->run_lock);
  mysql_mutex_unlock(&rli->run_lock);
  if (rli->until_condition != Relay_log_info::UNTIL_DONE)
  {
    sql_print_warning("MTS recovery: automatic recovery failed. Either the "
                      "slave server had stopped due to an error during an "
                      "earlier session or relay logs are corrupted."
                      "Fix the cause of the slave side error and restart the "
                      "slave server or consider using RESET SLAVE.");
    goto err;
  }

  /*
    We need a mutex while we are changing master info parameters to
    keep other threads from reading bogus info
  */
  mysql_mutex_lock(&mi->data_lock);
  mysql_mutex_lock(&rli->data_lock);
  recover_relay_log(mi);

  const char* msg;
  if (rli->init_relay_log_pos(rli->get_group_relay_log_name(),
                              rli->get_group_relay_log_pos(),
                              false/*need_data_lock=false*/,
                              &msg, 0))
  {
    char llbuf[22];
    sql_print_error("Failed to open the relay log '%s' (relay_log_pos %s).",
                    rli->get_group_relay_log_name(),
                    llstr(rli->get_group_relay_log_pos(), llbuf));

    recovery_error=1;
    mysql_mutex_unlock(&mi->data_lock);
    mysql_mutex_unlock(&rli->data_lock);
    goto err;
  }
  if (mi->flush_info(true) || rli->flush_info(true))
  {
    recovery_error= 1;
    mysql_mutex_unlock(&mi->data_lock);
    mysql_mutex_unlock(&rli->data_lock);
    goto err;
  }
  rli->inited=1;
  rli->error_on_rli_init_info= false;
  mysql_mutex_unlock(&mi->data_lock);
  mysql_mutex_unlock(&rli->data_lock);
  sql_print_information("MTS recovery: completed successfully.\n");
  DBUG_RETURN(recovery_error);
err:
  /*
    If recovery failed means we failed to initialize rli object in the case
    of MTS. We should not allow the START SLAVE command to work as we do in
    the case of STS. i.e if init_recovery call fails then we set inited=0.
  */
  rli->end_info();
  rli->inited=0;
  rli->error_on_rli_init_info= true;
  DBUG_RETURN(recovery_error);
}

// TODO: sync this method with reset_slave()
int raft_reset_slave(THD *thd)
{
  DBUG_ENTER("raft_reset_slave");
  int error= 0;
  mysql_mutex_lock(&LOCK_active_mi);

  strmake(active_mi->host, "\0", sizeof(active_mi->host)-1);
  active_mi->port = 0;
  active_mi->inited= false;
  active_mi->rli->inited= false;

  remove_info(active_mi);

  // no longer a slave. will be set again during change master
  is_slave = false;

  mysql_mutex_unlock(&LOCK_active_mi);
  DBUG_RETURN(error);
}

// TODO: currently we're only setting host port
int raft_change_master(
    THD *thd,
    const std::pair<const std::string, uint>& master_instance)
{
  DBUG_ENTER("raft_change_master");
  int error= 0;

  mysql_mutex_lock(&LOCK_active_mi);

  if (!active_mi) goto end;
  strmake(active_mi->host, const_cast<char*>(master_instance.first.c_str()),
          sizeof(active_mi->host)-1);
  active_mi->port= master_instance.second;
  active_mi->set_auto_position(true);
  active_mi->inited= true;
  active_mi->flush_info(true);

  // changing to a slave. set the is_slave flag
  is_slave = true;

end:
  mysql_mutex_unlock(&LOCK_active_mi);
  DBUG_RETURN(error);
}

/**
 * This changes the name of the raft relay log
 * to binlog name
 */
int rli_relay_log_raft_reset(
    std::pair<std::string, unsigned long long> applied_log_file_pos)
{
  DBUG_ENTER("rli_relay_log_raft_reset");

  if (disable_raft_log_repointing)
    DBUG_RETURN(0);

  mysql_mutex_lock(&LOCK_active_mi);

  int error= 0;
  Master_info* mi= active_mi;
  const char *errmsg;
  LOG_INFO linfo;

  DBUG_ASSERT(mi != NULL && mi->rli != NULL);

  std::string normalized_log_name=
    std::string(binlog_file_basedir_ptr) +
    std::string(applied_log_file_pos.first.c_str() +
        dirname_length(applied_log_file_pos.first.c_str()));
  strcpy(linfo.log_file_name, normalized_log_name.c_str());
  linfo.pos = applied_log_file_pos.second;

  /*
    We need a mutex while we are changing master info parameters to
    keep other threads from reading bogus info
  */
  mysql_mutex_lock(&mi->data_lock);
  mysql_mutex_lock(&mi->rli->data_lock);

  enum_return_check check_return_mi= mi->check_info();
  enum_return_check check_return_rli= mi->rli->check_info();

  // If the master.info file does not exist, or if it exists,
  // but the inited has never happened (most likely due to an
  // error), try mi_init_info
  if (check_return_mi == REPOSITORY_DOES_NOT_EXIST ||
      !mi->inited)
  {
    // NO_LINT_DEBUG
    sql_print_information("rli_relay_log_raft_reset: Master info "
                          "repository doesn't exist or not inited."
                          " Calling mi_init_info");
    if (mi->mi_init_info())
    {
      // NO_LINT_DEBUG
      sql_print_error("rli_relay_log_raft_reset: Failed to initialize "
                      "the master info structure");
      error= 1;
      goto end;
    }
  }

  if (check_return_rli == REPOSITORY_DOES_NOT_EXIST ||
      !mi->rli->inited)
  {
    // NO_LINT_DEBUG
    sql_print_information("rli_relay_log_raft_reset: Relay log info repository"
                          " doesn't exist or not inited. Calling"
                          " global_init_info");
    if (global_init_info(mi, false, SLAVE_SQL | SLAVE_IO, false))
    {
      // NO_LINT_DEBUG
      sql_print_error("rli_relay_log_raft_reset: Failed to initialize the"
                      " relay log info structure");
      error= 1;
      goto end;
    }
  }

  mysql_mutex_lock(mi->rli->relay_log.get_log_lock());
  mi->rli->relay_log.lock_index();

  mi->rli->relay_log.close(LOG_CLOSE_INDEX);

  if (mi->rli->relay_log.open_index_file(opt_binlog_index_name,
                                        opt_bin_logname, false))
  {
    // NO_LINT_DEBUG
    sql_print_error("rli_relay_log_raft_reset: Failed to open index file");
    error= 1;
    mi->rli->relay_log.unlock_index();
    mysql_mutex_unlock(mi->rli->relay_log.get_log_lock());
    goto end;
  }

  global_sid_lock->wrlock();
  // At this point the gtid set in the RLI and the last retrieved
  // GTID are up to date with all the gtids in the last raft log
  // file
  mi->rli->relay_log.init_gtid_sets(mi->rli->get_gtid_set_nc(), NULL,
                                    mi->rli->get_last_retrieved_gtid(),
                                    opt_slave_sql_verify_checksum,
                                    false);
  global_sid_lock->unlock();

  mi->rli->relay_log.set_previous_gtid_set(mi->rli->get_gtid_set_nc());

  // TODO ( figure out disconnect between SEQ_READ_APPEND and WRITE_CACHE)
  // At the end of this
  // cur_log_ext, log_file_name, name and IO_CACHE(log_file) should all be
  // up to date
  if (mi->rli->relay_log.open_existing_binlog(opt_bin_logname,
                                    SEQ_READ_APPEND, max_binlog_size))
  {
    // NO_LINT_DEBUG
    sql_print_error("rli_relay_log_raft_reset: Failed to open binlog file");
    error= 1;
    mi->rli->relay_log.unlock_index();
    mysql_mutex_unlock(mi->rli->relay_log.get_log_lock());
    goto end;
  }

  mi->rli->relay_log.unlock_index();
  mysql_mutex_unlock(mi->rli->relay_log.get_log_lock());

  // Init the SQL thread's cursor on the relay log (this cursor came as a
  // parameter)
  if ((error= mi->rli->init_relay_log_pos(linfo.log_file_name,
                                          linfo.pos,
                                          false/*need_data_lock=true*/,
                                          &errmsg,
                                          0 /*look for a description_event*/)))
  {
    // NO_LINT_DEBUG
    sql_print_error("rli_relay_log_raft_reset: Failed to "
                    "init_relay_log_pos, errmsg: %s",
                    errmsg);
    goto end;
  }

  sql_print_information(
      "rli_relay_log_raft_reset: Relay log cursor set to: %s:%llu",
      mi->rli->get_group_relay_log_name(),
      mi->rli->get_group_relay_log_pos());

  mi->rli->inited= true;

end:

  mysql_mutex_unlock(&mi->rli->data_lock);
  mysql_mutex_unlock(&mi->data_lock);
  mysql_mutex_unlock(&LOCK_active_mi);
  DBUG_RETURN(error);
}

int global_init_info(Master_info* mi, bool ignore_if_no_info, int thread_mask,
                     bool need_lock, bool startup)
{
  DBUG_ENTER("init_info");
  DBUG_ASSERT(mi != NULL && mi->rli != NULL);
  int init_error= 0;
  enum_return_check check_return= ERROR_CHECKING_REPOSITORY;
  THD *thd= current_thd;

  /*
    We need a mutex while we are changing master info parameters to
    keep other threads from reading bogus info
  */
  if (need_lock)
  {
    mysql_mutex_lock(&mi->data_lock);
    mysql_mutex_lock(&mi->rli->data_lock);
  }

  /*
    When info tables are used and autocommit= 0 we force a new
    transaction start to avoid table access deadlocks when START SLAVE
    is executed after RESET SLAVE.
  */
  if (is_autocommit_off_and_infotables(thd))
  {
    if (trans_begin(thd))
    {
      init_error= 1;
      goto end;
    }
  }

  /*
    This takes care of the startup dependency between the master_info
    and relay_info. It initializes the master info if the SLAVE_IO
    thread is being started and the relay log info if either the
    SLAVE_SQL thread is being started or was not initialized as it is
    required by the SLAVE_IO thread.
  */
  check_return= mi->check_info();
  if (check_return == ERROR_CHECKING_REPOSITORY)
  {
    if (enable_raft_plugin)
    {
      // NO_LINT_DEBUG
      sql_print_error("global_init_info: mi repository "
                      "check returns ERROR_CHECKING_REPOSITORY");
    }
    init_error= 1;
    goto end;
  }

  // ignore_if_no_info is used to skip the init_info process if
  // master.info file is not present. Its used to skip this
  // code in the startup of mysqld.
  // Set ignore_if_no_info to always call mi_init_info
  if (!(ignore_if_no_info && check_return == REPOSITORY_DOES_NOT_EXIST))
  {
    if ((thread_mask & SLAVE_IO) != 0)
    {
      if (enable_raft_plugin)
      {
        // NO_LINT_DEBUG
        sql_print_information("global_init_info: mi_init_info called");
      }
      if (mi->mi_init_info())
      {
        if (enable_raft_plugin)
        {
          // NO_LINT_DEBUG
          sql_print_error("global_init_info: mi_init_info returned error");
        }
        init_error= 1;
      }
    }
  }

  check_return= mi->rli->check_info();
  if (check_return == ERROR_CHECKING_REPOSITORY)
  {
    if (enable_raft_plugin)
    {
      // NO_LINT_DEBUG
      sql_print_error("global_init_info: rli repository check returns"
                      " ERROR_CHECKING_REPOSITORY");
    }
    init_error= 1;
    goto end;
  }
  if (!(ignore_if_no_info && check_return == REPOSITORY_DOES_NOT_EXIST))
  {
    if (((thread_mask & SLAVE_SQL) != 0 || !(mi->rli->inited)))
    {
      if (enable_raft_plugin)
      {
        // NO_LINT_DEBUG
        sql_print_information("global_init_info: rli_init_info called");
      }
      if (mi->rli->rli_init_info(startup))
      {
        if (enable_raft_plugin)
        {
          // NO_LINT_DEBUG
          sql_print_error("global_init_info: rli_init_info returned error");
        }
        init_error= 1;
      }
    }
  }

  DBUG_EXECUTE_IF("enable_mts_worker_failure_init",
                  {DBUG_SET("+d,mts_worker_thread_init_fails");});
end:
  /*
    When info tables are used and autocommit= 0 we force transaction
    commit to avoid table access deadlocks when START SLAVE is executed
    after RESET SLAVE.
  */
  if (is_autocommit_off_and_infotables(thd))
    if (trans_commit(thd))
      init_error= 1;

  if (need_lock)
  {
    mysql_mutex_unlock(&mi->rli->data_lock);
    mysql_mutex_unlock(&mi->data_lock);
  }

  /*
    Handling MTS Relay-log recovery after successful initialization of mi and
    rli objects.

    MTS Relay-log recovery is handled by SSUG command. In order to start the
    slave applier thread rli needs to be inited and mi->rli->data_lock should
    be in released state. Hence we do the MTS recovery at this point of time
    where both conditions are satisfied.
  */
  if (!init_error && mi->rli->is_relay_log_recovery
      && mi->rli->mts_recovery_group_cnt)
    init_error= fill_mts_gaps_and_recover(mi);
  DBUG_RETURN(init_error);
}

void end_info(Master_info* mi)
{
  DBUG_ENTER("end_info");
  DBUG_ASSERT(mi != NULL && mi->rli != NULL);

  /*
    The previous implementation was not acquiring locks.  We do the same here.
    However, this is quite strange.
  */
  mi->end_info();
  mi->rli->end_info();

  DBUG_VOID_RETURN;
}

int remove_info(Master_info* mi)
{
  int error= 1;
  DBUG_ENTER("remove_info");
  DBUG_ASSERT(mi != NULL && mi->rli != NULL);

  /*
    The previous implementation was not acquiring locks.
    We do the same here. However, this is quite strange.
  */
  /*
    Reset errors (the idea is that we forget about the
    old master).
  */
  mi->clear_error();
  mi->rli->clear_error();
  mi->rli->clear_until_condition();
  mi->rli->clear_sql_delay();

  mi->end_info();
  mi->rli->end_info();

  if (mi->remove_info() || Rpl_info_factory::reset_workers(mi->rli) ||
      Rpl_info_factory::reset_gtid_infos(mi->rli) || mi->rli->remove_info())
    goto err;

  error= 0;

err:
  DBUG_RETURN(error);
}

int flush_master_info(Master_info* mi, bool force)
{
  DBUG_ENTER("flush_master_info");
  DBUG_ASSERT(mi != NULL && mi->rli != NULL);
  /*
    The previous implementation was not acquiring locks.
    We do the same here. However, this is quite strange.
  */
  /*
    With the appropriate recovery process, we will not need to flush
    the content of the current log.

    For now, we flush the relay log BEFORE the master.info file, because
    if we crash, we will get a duplicate event in the relay log at restart.
    If we change the order, there might be missing events.

    If we don't do this and the slave server dies when the relay log has
    some parts (its last kilobytes) in memory only, with, say, from master's
    position 100 to 150 in memory only (not on disk), and with position 150
    in master.info, there will be missing information. When the slave restarts,
    the I/O thread will fetch binlogs from 150, so in the relay log we will
    have "[0, 100] U [150, infinity[" and nobody will notice it, so the SQL
    thread will jump from 100 to 150, and replication will silently break.
  */
  mysql_mutex_t *log_lock= mi->rli->relay_log.get_log_lock();

  mysql_mutex_lock(log_lock);

  int err=  (mi->rli->flush_current_log() ||
             mi->flush_info(force));

  mysql_mutex_unlock(log_lock);

  DBUG_RETURN (err);
}

/**
  Convert slave skip errors bitmap into a printable string.
*/

static void print_slave_skip_errors(void)
{
  /*
    To be safe, we want 10 characters of room in the buffer for a number
    plus terminators. Also, we need some space for constant strings.
    10 characters must be sufficient for a number plus {',' | '...'}
    plus a NUL terminator. That is a max 6 digit number.
  */
  const size_t MIN_ROOM= 10;
  DBUG_ENTER("print_slave_skip_errors");
  DBUG_ASSERT(sizeof(slave_skip_error_names) > MIN_ROOM);
  DBUG_ASSERT(MAX_SLAVE_ERROR <= 999999); // 6 digits

  if (!use_slave_mask || bitmap_is_clear_all(&slave_error_mask))
  {
    /* purecov: begin tested */
    memcpy(slave_skip_error_names, STRING_WITH_LEN("OFF"));
    /* purecov: end */
  }
  else if (bitmap_is_set_all(&slave_error_mask))
  {
    /* purecov: begin tested */
    memcpy(slave_skip_error_names, STRING_WITH_LEN("ALL"));
    /* purecov: end */
  }
  else
  {
    char *buff= slave_skip_error_names;
    char *bend= buff + sizeof(slave_skip_error_names);
    int  errnum;

    for (errnum= 0; errnum < MAX_SLAVE_ERROR; errnum++)
    {
      if (bitmap_is_set(&slave_error_mask, errnum))
      {
        if (buff + MIN_ROOM >= bend)
          break; /* purecov: tested */
        buff= int10_to_str(errnum, buff, 10);
        *buff++= ',';
      }
    }
    if (buff != slave_skip_error_names)
      buff--; // Remove last ','
    if (errnum < MAX_SLAVE_ERROR)
    {
      /* Couldn't show all errors */
      buff= strmov(buff, "..."); /* purecov: tested */
    }
    *buff=0;
  }
  DBUG_PRINT("init", ("error_names: '%s'", slave_skip_error_names));
  DBUG_VOID_RETURN;
}

static void set_stop_slave_wait_timeout(unsigned long wait_timeout) {
  stop_wait_timeout = wait_timeout;
}

/**
 Change arg to the string with the nice, human-readable skip error values.
   @param slave_skip_errors_ptr
          The pointer to be changed
*/
void set_slave_skip_errors(char** slave_skip_errors_ptr)
{
  DBUG_ENTER("set_slave_skip_errors");
  print_slave_skip_errors();
  *slave_skip_errors_ptr= slave_skip_error_names;
  DBUG_VOID_RETURN;
}

/**
  Init function to set up array for errors that should be skipped for slave
*/
static void init_slave_skip_errors()
{
  DBUG_ENTER("init_slave_skip_errors");
  DBUG_ASSERT(!use_slave_mask); // not already initialized

  if (bitmap_init(&slave_error_mask,0,MAX_SLAVE_ERROR,0))
  {
    fprintf(stderr, "Badly out of memory, please check your system status\n");
    exit(1);
  }
  use_slave_mask = 1;
  DBUG_VOID_RETURN;
}

static void add_slave_skip_errors(const uint* errors, uint n_errors)
{
  DBUG_ENTER("add_slave_skip_errors");
  DBUG_ASSERT(errors);
  DBUG_ASSERT(use_slave_mask);

  for (uint i = 0; i < n_errors; i++)
  {
    const uint err_code = errors[i];
    if (err_code < MAX_SLAVE_ERROR)
       bitmap_set_bit(&slave_error_mask, err_code);
  }
  DBUG_VOID_RETURN;
}

/*
  Add errors that should be skipped for slave

  SYNOPSIS
    add_slave_skip_errors()
    arg         List of errors numbers to be added to skip, separated with ','

  NOTES
    Called from get_options() in mysqld.cc on start-up
*/

void add_slave_skip_errors(const char* arg)
{
  const char *p= NULL;
  /*
    ALL is only valid when nothing else is provided.
  */
  const uchar SKIP_ALL[]= "all";
  size_t SIZE_SKIP_ALL= strlen((const char *) SKIP_ALL) + 1;
  /*
    IGNORE_DDL_ERRORS can be combined with other parameters
    but must be the first one provided.
  */
  const uchar SKIP_DDL_ERRORS[]= "ddl_exist_errors";
  size_t SIZE_SKIP_DDL_ERRORS= strlen((const char *) SKIP_DDL_ERRORS);
  DBUG_ENTER("add_slave_skip_errors");

  // initialize mask if not done yet
  if (!use_slave_mask)
    init_slave_skip_errors();

  for (; my_isspace(system_charset_info,*arg); ++arg)
    /* empty */;
  if (!my_strnncoll(system_charset_info, (uchar*)arg, SIZE_SKIP_ALL,
                    SKIP_ALL, SIZE_SKIP_ALL))
  {
    bitmap_set_all(&slave_error_mask);
    DBUG_VOID_RETURN;
  }
  if (!my_strnncoll(system_charset_info, (uchar*)arg, SIZE_SKIP_DDL_ERRORS,
                    SKIP_DDL_ERRORS, SIZE_SKIP_DDL_ERRORS))
  {
    // DDL errors to be skipped for relaxed 'exist' handling
    const uint ddl_errors[] = {
      // error codes with create/add <schema object>
      ER_DB_CREATE_EXISTS, ER_TABLE_EXISTS_ERROR, ER_DUP_KEYNAME,
      ER_MULTIPLE_PRI_KEY,
      // error codes with change/rename <schema object>
      ER_BAD_FIELD_ERROR, ER_NO_SUCH_TABLE, ER_DUP_FIELDNAME,
      // error codes with drop <schema object>
      ER_DB_DROP_EXISTS, ER_BAD_TABLE_ERROR, ER_CANT_DROP_FIELD_OR_KEY
    };

    add_slave_skip_errors(ddl_errors,
                          sizeof(ddl_errors)/sizeof(ddl_errors[0]));
    /*
      After processing the SKIP_DDL_ERRORS, the pointer is
      increased to the position after the comma.
    */
    if (strlen(arg) > SIZE_SKIP_DDL_ERRORS + 1)
      arg+= SIZE_SKIP_DDL_ERRORS + 1;
  }
  for (p= arg ; *p; )
  {
    long err_code;
    if (!(p= str2int(p, 10, 0, LONG_MAX, &err_code)))
      break;
    if (err_code < MAX_SLAVE_ERROR)
       bitmap_set_bit(&slave_error_mask,(uint)err_code);
    while (!my_isdigit(system_charset_info,*p) && *p)
      p++;
  }
  DBUG_VOID_RETURN;
}

static void set_thd_in_use_temporary_tables(Relay_log_info *rli)
{
  TABLE *table;
  bool attach = rli->info_thd != nullptr;

  for (table= rli->save_temporary_tables ; table ; table= table->next)
  {
    /* For attach in_use needs to point to new thread,
       for detach it still needs to point to the old one
       and will be set below. */
    if (attach)
      table->in_use= rli->info_thd;

    if (table->file != NULL)
    {
      /*
        Since we are stealing opened temporary tables from one thread to another,
        we need to let the performance schema know that,
        for aggregates per thread to work properly.
      */
      table->file->unbind_psi();
      table->file->rebind_psi();
      table->file->register_tmp_table_disk_usage(attach);
    }

    if (!attach)
      table->in_use= rli->info_thd;
  }
}

int terminate_slave_threads(Master_info* mi,int thread_mask,bool need_lock_term)
{
  DBUG_ENTER("terminate_slave_threads");

  if (!mi->inited)
    DBUG_RETURN(0); /* successfully do nothing */
  int error,force_all = (thread_mask & SLAVE_FORCE_ALL);
  mysql_mutex_t *sql_lock = &mi->rli->run_lock, *io_lock = &mi->run_lock;
  mysql_mutex_t *log_lock= mi->rli->relay_log.get_log_lock();
  set_stop_slave_wait_timeout(rpl_stop_slave_timeout);

  if (thread_mask & (SLAVE_SQL|SLAVE_FORCE_ALL))
  {
    DBUG_PRINT("info",("Terminating SQL thread"));
    mi->rli->abort_slave= 1;
    if ((error=terminate_slave_thread(mi->rli->info_thd, sql_lock,
                                      &mi->rli->stop_cond,
                                      &mi->rli->slave_running,
                                      need_lock_term)) &&
        !force_all)
    {
      if (error == 1)
      {
        DBUG_RETURN(ER_STOP_SLAVE_SQL_THREAD_TIMEOUT);
      }
      DBUG_RETURN(error);
    }
    mysql_mutex_lock(log_lock);

    DBUG_PRINT("info",("Flushing relay-log info file."));
    if (current_thd)
      THD_STAGE_INFO(current_thd, stage_flushing_relay_log_info_file);

    /*
      Flushes the relay log info regardles of the sync_relay_log_info option.
    */
    if (mi->rli->flush_info(TRUE))
    {
      mysql_mutex_unlock(log_lock);
      DBUG_RETURN(ER_ERROR_DURING_FLUSH_LOGS);
    }

    mysql_mutex_unlock(log_lock);
  }
  if (thread_mask & (SLAVE_IO|SLAVE_FORCE_ALL))
  {
    DBUG_PRINT("info",("Terminating IO thread"));
    mi->abort_slave=1;
    if ((error=terminate_slave_thread(mi->info_thd,io_lock,
                                      &mi->stop_cond,
                                      &mi->slave_running,
                                      need_lock_term)) &&
        !force_all)
    {
      if (error == 1)
      {
        DBUG_RETURN(ER_STOP_SLAVE_IO_THREAD_TIMEOUT);
      }
      DBUG_RETURN(error);
    }
    mysql_mutex_lock(log_lock);

    DBUG_PRINT("info",("Flushing relay log and master info repository."));
    if (current_thd)
      THD_STAGE_INFO(current_thd, stage_flushing_relay_log_and_master_info_repository);

    /*
      Flushes the master info regardles of the sync_master_info option.
    */
    if (mi->flush_info(TRUE))
    {
      mysql_mutex_unlock(log_lock);
      DBUG_RETURN(ER_ERROR_DURING_FLUSH_LOGS);
    }

    /*
      Flushes the relay log regardles of the sync_relay_log option.
    */
    if (mi->rli->relay_log.is_open() &&
        mi->rli->relay_log.flush_and_sync(false, true))
    {
      mysql_mutex_unlock(log_lock);
      DBUG_RETURN(ER_ERROR_DURING_FLUSH_LOGS);
    }

    mysql_mutex_unlock(log_lock);
  }
  DBUG_RETURN(0);
}


/**
   Wait for a slave thread to terminate.

   This function is called after requesting the thread to terminate
   (by setting @c abort_slave member of @c Relay_log_info or @c
   Master_info structure to 1). Termination of the thread is
   controlled with the the predicate <code>*slave_running</code>.

   Function will acquire @c term_lock before waiting on the condition
   unless @c need_lock_term is false in which case the mutex should be
   owned by the caller of this function and will remain acquired after
   return from the function.

   @param term_lock
          Associated lock to use when waiting for @c term_cond

   @param term_cond
          Condition that is signalled when the thread has terminated

   @param slave_running
          Pointer to predicate to check for slave thread termination

   @param need_lock_term
          If @c false the lock will not be acquired before waiting on
          the condition. In this case, it is assumed that the calling
          function acquires the lock before calling this function.

   @retval 0 All OK, 1 on "STOP SLAVE" command timeout, ER_SLAVE_NOT_RUNNING otherwise.

   @note  If the executing thread has to acquire term_lock
          (need_lock_term is true, the negative running status does not
          represent any issue therefore no error is reported.

 */
static int
terminate_slave_thread(THD *thd,
                       mysql_mutex_t *term_lock,
                       mysql_cond_t *term_cond,
                       volatile uint *slave_running,
                       bool need_lock_term)
{
  DBUG_ENTER("terminate_slave_thread");
  if (need_lock_term)
  {
    mysql_mutex_lock(term_lock);
  }
  else
  {
    mysql_mutex_assert_owner(term_lock);
  }
  if (!*slave_running)
  {
    if (need_lock_term)
    {
      /*
        if run_lock (term_lock) is acquired locally then either
        slave_running status is fine
      */
      mysql_mutex_unlock(term_lock);
      DBUG_RETURN(0);
    }
    else
    {
      DBUG_RETURN(ER_SLAVE_NOT_RUNNING);
    }
  }
  DBUG_ASSERT(thd != 0);
  THD_CHECK_SENTRY(thd);

  /*
    Is is critical to test if the slave is running. Otherwise, we might
    be referening freed memory trying to kick it
  */

  while (*slave_running)                        // Should always be true
  {
    int error MY_ATTRIBUTE((unused));
    DBUG_PRINT("loop", ("killing slave thread"));

    mysql_mutex_lock(&thd->LOCK_thd_data);
#ifndef DONT_USE_THR_ALARM
    /*
      Error codes from pthread_kill are:
      EINVAL: invalid signal number (can't happen)
      ESRCH: thread already killed (can happen, should be ignored)
    */
    int err MY_ATTRIBUTE((unused))= pthread_kill(thd->real_id, thr_client_alarm);
    DBUG_ASSERT(err != EINVAL);
#endif
    thd->awake(THD::NOT_KILLED);
    mysql_mutex_unlock(&thd->LOCK_thd_data);

    /*
      There is a small chance that slave thread might miss the first
      alarm. To protect againts it, resend the signal until it reacts
    */
    struct timespec abstime;
    set_timespec(abstime,2);
    error= mysql_cond_timedwait(term_cond, term_lock, &abstime);
    if (stop_wait_timeout >= 2)
      stop_wait_timeout= stop_wait_timeout - 2;
    else if (*slave_running)
    {
      if (need_lock_term)
        mysql_mutex_unlock(term_lock);
      DBUG_RETURN (1);
    }
    DBUG_ASSERT(error == ETIMEDOUT || error == 0);
  }

  DBUG_ASSERT(*slave_running == 0);

  if (need_lock_term)
    mysql_mutex_unlock(term_lock);
  DBUG_RETURN(0);
}


int start_slave_thread(
#ifdef HAVE_PSI_INTERFACE
                       PSI_thread_key thread_key,
#endif
                       pthread_handler h_func, mysql_mutex_t *start_lock,
                       mysql_mutex_t *cond_lock,
                       mysql_cond_t *start_cond,
                       volatile uint *slave_running,
                       volatile ulong *slave_run_id,
                       Master_info* mi)
{
  pthread_t th;
  ulong start_id;
  int error;
  DBUG_ENTER("start_slave_thread");

  if (start_lock)
    mysql_mutex_lock(start_lock);
  if (!server_id)
  {
    if (start_cond)
      mysql_cond_broadcast(start_cond);
    if (start_lock)
      mysql_mutex_unlock(start_lock);
    sql_print_error("Server id not set, will not start slave");
    DBUG_RETURN(ER_BAD_SLAVE);
  }

  if (*slave_running)
  {
    if (start_cond)
      mysql_cond_broadcast(start_cond);
    if (start_lock)
      mysql_mutex_unlock(start_lock);
    DBUG_RETURN(ER_SLAVE_MUST_STOP);
  }
  start_id= *slave_run_id;
  DBUG_PRINT("info",("Creating new slave thread"));
  if ((error= mysql_thread_create(thread_key,
                          &th, &connection_attrib, h_func, (void*)mi)))
  {
    sql_print_error("Can't create slave thread (errno= %d).", error);
    if (start_lock)
      mysql_mutex_unlock(start_lock);
    DBUG_RETURN(ER_SLAVE_THREAD);
  }
  if (start_cond && cond_lock) // caller has cond_lock
  {
    THD* thd = current_thd;
    while (start_id == *slave_run_id && thd != NULL)
    {
      DBUG_PRINT("sleep",("Waiting for slave thread to start"));
      PSI_stage_info saved_stage= {0, "", 0};
      thd->ENTER_COND(start_cond, cond_lock,
                      & stage_waiting_for_slave_thread_to_start,
                      & saved_stage);
      /*
        It is not sufficient to test this at loop bottom. We must test
        it after registering the mutex in enter_cond(). If the kill
        happens after testing of thd->killed and before the mutex is
        registered, we could otherwise go waiting though thd->killed is
        set.
      */
      if (!thd->killed)
        mysql_cond_wait(start_cond, cond_lock);
      thd->EXIT_COND(& saved_stage);
      mysql_mutex_lock(cond_lock); // re-acquire it as exit_cond() released
      if (thd->killed)
      {
        if (start_lock)
          mysql_mutex_unlock(start_lock);
        DBUG_RETURN(thd->killed_errno());
      }
    }
  }
  if (start_lock)
    mysql_mutex_unlock(start_lock);
  DBUG_RETURN(0);
}


/*
  start_slave_threads()

  NOTES
    SLAVE_FORCE_ALL is not implemented here on purpose since it does not make
    sense to do that for starting a slave--we always care if it actually
    started the threads that were not previously running
*/

int start_slave_threads(bool need_lock_slave, bool wait_for_start,
                        Master_info* mi, int thread_mask)
{
  mysql_mutex_t *lock_io=0, *lock_sql=0, *lock_cond_io=0, *lock_cond_sql=0;
  mysql_cond_t* cond_io=0, *cond_sql=0;
  int error=0;
  DBUG_ENTER("start_slave_threads");
  DBUG_EXECUTE_IF("uninitialized_master-info_structure",
                   mi->inited= FALSE;);

  if (!mi->inited || !mi->rli->inited)
  {
    error= !mi->inited ? ER_SLAVE_MI_INIT_REPOSITORY :
                         ER_SLAVE_RLI_INIT_REPOSITORY;
    if (enable_raft_plugin)
    {
      // NO_LINT_DEBUG
      sql_print_error("start_slave_threads: error: %d mi_inited: %d",
                      error, mi->inited);
    }
    Rpl_info *info= (!mi->inited ?  mi : static_cast<Rpl_info *>(mi->rli));
    const char* prefix= current_thd ? ER(error) : ER_DEFAULT(error);
    info->report(ERROR_LEVEL, error, prefix, NULL);

    DBUG_RETURN(error);
  }

  if (need_lock_slave)
  {
    lock_io = &mi->run_lock;
    lock_sql = &mi->rli->run_lock;
  }
  if (wait_for_start)
  {
    cond_io = &mi->start_cond;
    cond_sql = &mi->rli->start_cond;
    lock_cond_io = &mi->run_lock;
    lock_cond_sql = &mi->rli->run_lock;
  }

  if ((thread_mask & SLAVE_IO) && !enable_raft_plugin)
    error= start_slave_thread(
#ifdef HAVE_PSI_INTERFACE
                              key_thread_slave_io,
#endif
                              handle_slave_io, lock_io, lock_cond_io,
                              cond_io,
                              &mi->slave_running, &mi->slave_run_id,
                              mi);
  if (!error && (thread_mask & SLAVE_SQL))
  {
    /*
      MTS-recovery gaps gathering is placed onto common execution path
      for either START-SLAVE and --skip-start-slave= 0
    */
    if (mi->rli->recovery_parallel_workers != 0 && gtid_mode == 0)
      error= mts_recovery_groups(mi->rli);
    if (!error)
      error= start_slave_thread(
#ifdef HAVE_PSI_INTERFACE
                                key_thread_slave_sql,
#endif
                                handle_slave_sql, lock_sql, lock_cond_sql,
                                cond_sql,
                                &mi->rli->slave_running, &mi->rli->slave_run_id,
                                mi);
    if (error)
      terminate_slave_threads(mi, thread_mask & SLAVE_IO, need_lock_slave);
  }
  DBUG_RETURN(error);
}

/*
  Release slave threads at time of executing shutdown.

  SYNOPSIS
    end_slave()
*/

void end_slave()
{
  DBUG_ENTER("end_slave");

  /*
    This is called when the server terminates, in close_connections().
    It terminates slave threads. However, some CHANGE MASTER etc may still be
    running presently. If a START SLAVE was in progress, the mutex lock below
    will make us wait until slave threads have started, and START SLAVE
    returns, then we terminate them here.
  */
  mysql_mutex_lock(&LOCK_active_mi);
  if (active_mi)
  {
    /*
      TODO: replace the line below with
      list_walk(&master_list, (list_walk_action)end_slave_on_walk,0);
      once multi-master code is ready.
    */
    terminate_slave_threads(active_mi,SLAVE_FORCE_ALL);
  }
  mysql_mutex_unlock(&LOCK_active_mi);
  DBUG_VOID_RETURN;
}

/**
   Free all resources used by slave threads at time of executing shutdown.
   The routine must be called after all possible users of @c active_mi
   have left.

   SYNOPSIS
     close_active_mi()

*/
void close_active_mi()
{
  mysql_mutex_lock(&LOCK_active_mi);
  if (active_mi)
  {
    end_info(active_mi);
    if (active_mi->rli)
      delete active_mi->rli;
    delete active_mi;
    active_mi= 0;
  }
  mysql_mutex_unlock(&LOCK_active_mi);
}

/**
   Check if multi-statement transaction mode and master and slave info
   repositories are set to table.

   @param THD    THD object

   @retval true  Success
   @retval false Failure
*/
static bool is_autocommit_off_and_infotables(THD* thd)
{
  DBUG_ENTER("is_autocommit_off_and_infotables");
  DBUG_RETURN((thd && thd->in_multi_stmt_transaction_mode() &&
               (opt_mi_repository_id == INFO_REPOSITORY_TABLE ||
                opt_rli_repository_id == INFO_REPOSITORY_TABLE))?
              true : false);
}

static bool io_slave_killed(THD* thd, Master_info* mi)
{
  DBUG_ENTER("io_slave_killed");

  DBUG_ASSERT(mi->info_thd == thd);
  DBUG_ASSERT(mi->slave_running); // tracking buffer overrun
  DBUG_RETURN(mi->abort_slave || abort_loop || thd->killed);
}

/**
   The function analyzes a possible killed status and makes
   a decision whether to accept it or not.
   Normally upon accepting the sql thread goes to shutdown.
   In the event of deferring decision @rli->last_event_start_time waiting
   timer is set to force the killed status be accepted upon its expiration.

   Notice Multi-Threaded-Slave behaves similarly in that when it's being
   stopped and the current group of assigned events has not yet scheduled
   completely, Coordinator defers to accept to leave its read-distribute
   state. The above timeout ensures waiting won't last endlessly, and in
   such case an error is reported.

   @param thd   pointer to a THD instance
   @param rli   pointer to Relay_log_info instance

   @return TRUE the killed status is recognized, FALSE a possible killed
           status is deferred.
*/
static bool sql_slave_killed(THD* thd, Relay_log_info* rli)
{
  bool is_parallel_warn= FALSE;

  DBUG_ENTER("sql_slave_killed");

  DBUG_ASSERT(rli->info_thd == thd);
  DBUG_ASSERT(rli->slave_running == 1);
  if (rli->sql_thread_kill_accepted)
    DBUG_RETURN(true);
  if (abort_loop || thd->killed || rli->abort_slave)
  {
    rli->sql_thread_kill_accepted= true;
    /* NOTE: In MTS mode if all workers are done and if the partial trx
       (if any) can be rollbacked safely we can accept the kill */
    bool can_rollback= rli->abort_slave &&
                       (!rli->is_mts_in_group() ||
                        (rli->mts_workers_queue_empty() &&
                         !rli->cannot_safely_rollback()));
    is_parallel_warn= (rli->is_parallel_exec() &&
                       (!can_rollback || thd->killed));
    /*
      Slave can execute stop being in one of two MTS or Single-Threaded mode.
      The modes define different criteria to accept the stop.
      In particular that relates to the concept of groupping.
      Killed Coordinator thread expects the worst so it warns on
      possible consistency issue.
    */
    if (is_parallel_warn ||
        (!rli->is_parallel_exec() &&
         thd->transaction.all.cannot_safely_rollback() && rli->is_in_group()))
    {
      char msg_stopped[]=
        "... Slave SQL Thread stopped with incomplete event group "
        "having non-transactional changes. "
        "If the group consists solely of row-based events, you can try "
        "to restart the slave with --slave-exec-mode=IDEMPOTENT, which "
        "ignores duplicate key, key not found, and similar errors (see "
        "documentation for details).";
      char msg_stopped_mts[]=
        "... The slave coordinator and worker threads are stopped, possibly "
        "leaving data in inconsistent state. A restart should "
        "restore consistency automatically, although using non-transactional "
        "storage for data or info tables or DDL queries could lead to problems. "
        "In such cases you have to examine your data (see documentation for "
        "details).";

      if (rli->abort_slave)
      {
        DBUG_PRINT("info", ("Request to stop slave SQL Thread received while "
                            "applying an MTS group or a group that "
                            "has non-transactional "
                            "changes; waiting for completion of the group ... "));

        /*
          Slave sql thread shutdown in face of unfinished group modified
          Non-trans table is handled via a timer. The slave may eventually
          give out to complete the current group and in that case there
          might be issues at consequent slave restart, see the error message.
          WL#2975 offers a robust solution requiring to store the last exectuted
          event's coordinates along with the group's coordianates
          instead of waiting with @c last_event_start_time the timer.
        */

        if (rli->last_event_start_time == 0)
          rli->last_event_start_time= my_time(0);
        rli->sql_thread_kill_accepted= difftime(my_time(0),
                                               rli->last_event_start_time) <=
                                               SLAVE_WAIT_GROUP_DONE ?
                                               FALSE : TRUE;

        DBUG_EXECUTE_IF("stop_slave_middle_group",
                        DBUG_EXECUTE_IF("incomplete_group_in_relay_log",
                                        rli->sql_thread_kill_accepted= TRUE;);); // time is over

        if (!rli->sql_thread_kill_accepted && !rli->reported_unsafe_warning)
        {
          rli->report(WARNING_LEVEL, 0,
                      !is_parallel_warn ?
                      "Request to stop slave SQL Thread received while "
                      "applying a group that has non-transactional "
                      "changes; waiting for completion of the group ... "
                      :
                      "Coordinator thread of multi-threaded slave is being "
                      "stopped in the middle of assigning a group of events; "
                      "deferring to exit until the group completion ... ");
          rli->reported_unsafe_warning= true;
        }
      }
      if (rli->sql_thread_kill_accepted)
      {
        if (rli->mts_group_status == Relay_log_info::MTS_IN_GROUP)
        {
          rli->mts_group_status= Relay_log_info::MTS_KILLED_GROUP;
        }
        if (is_parallel_warn)
          rli->report(!rli->is_error() ? ERROR_LEVEL :
                      WARNING_LEVEL,    // an error was reported by Worker
                      ER_MTS_INCONSISTENT_DATA,
                      ER(ER_MTS_INCONSISTENT_DATA),
                      msg_stopped_mts);
        else
          rli->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR,
                      ER(ER_SLAVE_FATAL_ERROR), msg_stopped);
      }
    }
  }

  if (rli->sql_thread_kill_accepted)
    rli->last_event_start_time= 0;

  DBUG_RETURN(rli->sql_thread_kill_accepted);
}


/*
  skip_load_data_infile()

  NOTES
    This is used to tell a 3.23 master to break send_file()
*/

void skip_load_data_infile(NET *net)
{
  DBUG_ENTER("skip_load_data_infile");

  (void)net_request_file(net, "/dev/null");
  (void)my_net_read(net);                               // discard response
  (void)net_write_command(net, 0, (uchar*) "", 0, (uchar*) "", 0); // ok
  DBUG_VOID_RETURN;
}


bool net_request_file(NET* net, const char* fname)
{
  DBUG_ENTER("net_request_file");
  DBUG_RETURN(net_write_command(net, 251, (uchar*) fname, strlen(fname),
                                (uchar*) "", 0));
}

/*
  From other comments and tests in code, it looks like
  sometimes Query_log_event and Load_log_event can have db == 0
  (see rewrite_db() above for example)
  (cases where this happens are unclear; it may be when the master is 3.23).
*/

const char *print_slave_db_safe(const char* db)
{
  DBUG_ENTER("*print_slave_db_safe");

  DBUG_RETURN((db ? db : ""));
}

/*
  Check if the error is caused by network.
  @param[in]   errorno   Number of the error.
  RETURNS:
  TRUE         network error
  FALSE        not network error
*/

bool is_network_error(uint errorno)
{
  if (errorno == CR_CONNECTION_ERROR ||
      errorno == CR_CONN_HOST_ERROR ||
      errorno == CR_SERVER_GONE_ERROR ||
      errorno == CR_SERVER_LOST ||
      errorno == ER_CON_COUNT_ERROR ||
      errorno == ER_SERVER_SHUTDOWN ||
      errorno == ER_NET_READ_INTERRUPTED)
    return TRUE;

  return FALSE;
}


/**
  Execute an initialization query for the IO thread.

  If there is an error, then this function calls mysql_free_result;
  otherwise the MYSQL object holds the result after this call.  If
  there is an error other than allowed_error, then this function
  prints a message and returns -1.

  @param mysql MYSQL object.
  @param query Query string.
  @param allowed_error Allowed error code, or 0 if no errors are allowed.
  @param[out] master_res If this is not NULL and there is no error, then
  mysql_store_result() will be called and the result stored in this pointer.
  @param[out] master_row If this is not NULL and there is no error, then
  mysql_fetch_row() will be called and the result stored in this pointer.

  @retval COMMAND_STATUS_OK No error.
  @retval COMMAND_STATUS_ALLOWED_ERROR There was an error and the
  error code was 'allowed_error'.
  @retval COMMAND_STATUS_ERROR There was an error and the error code
  was not 'allowed_error'.
*/
enum enum_command_status
{ COMMAND_STATUS_OK, COMMAND_STATUS_ERROR, COMMAND_STATUS_ALLOWED_ERROR };
static enum_command_status
io_thread_init_command(Master_info *mi, const char *query, int allowed_error,
                       MYSQL_RES **master_res= NULL,
                       MYSQL_ROW *master_row= NULL)
{
  DBUG_ENTER("io_thread_init_command");
  DBUG_PRINT("info", ("IO thread initialization command: '%s'", query));
  MYSQL *mysql= mi->mysql;
  int ret= mysql_real_query(mysql, query, strlen(query));
  if (io_slave_killed(mi->info_thd, mi))
  {
    sql_print_information("The slave IO thread was killed while executing "
                          "initialization query '%s'", query);
    mysql_free_result(mysql_store_result(mysql));
    DBUG_RETURN(COMMAND_STATUS_ERROR);
  }
  if (ret != 0)
  {
    int err= mysql_errno(mysql);
    mysql_free_result(mysql_store_result(mysql));
    if (!err || err != allowed_error)
    {
      mi->report(is_network_error(err) ? WARNING_LEVEL : ERROR_LEVEL, err,
                 "The slave IO thread stops because the initialization query "
                 "'%s' failed with error '%s'.",
                 query, mysql_error(mysql));
      DBUG_RETURN(COMMAND_STATUS_ERROR);
    }
    DBUG_RETURN(COMMAND_STATUS_ALLOWED_ERROR);
  }
  if (master_res != NULL)
  {
    if ((*master_res= mysql_store_result(mysql)) == NULL)
    {
      mi->report(WARNING_LEVEL, mysql_errno(mysql),
                 "The slave IO thread stops because the initialization query "
                 "'%s' did not return any result.",
                 query);
      DBUG_RETURN(COMMAND_STATUS_ERROR);
    }
    if (master_row != NULL)
    {
      if ((*master_row= mysql_fetch_row(*master_res)) == NULL)
      {
        mysql_free_result(*master_res);
        mi->report(WARNING_LEVEL, mysql_errno(mysql),
                   "The slave IO thread stops because the initialization query "
                   "'%s' did not return any row.",
                   query);
        DBUG_RETURN(COMMAND_STATUS_ERROR);
      }
    }
  }
  else
    DBUG_ASSERT(master_row == NULL);
  DBUG_RETURN(COMMAND_STATUS_OK);
}


/**
  Set user variables after connecting to the master.

  @param  mysql MYSQL to request uuid from master.
  @param  mi    Master_info to set master_uuid

  @return 0: Success, 1: Fatal error, 2: Network error.
 */
int io_thread_init_commands(MYSQL *mysql, Master_info *mi)
{
  char query[256];
  int ret= 0;
  DBUG_EXECUTE_IF("fake_5_5_version_slave", return ret;);

  sprintf(query, "SET "
                 "@slave_uuid= '%s',"
                 "@dump_thread_wait_sleep_usec= %llu",
          server_uuid, opt_slave_dump_thread_wait_sleep_usec);
  if (mysql_real_query(mysql, query, strlen(query))
      && !check_io_slave_killed(mi->info_thd, mi, NULL))
    goto err;

  mysql_free_result(mysql_store_result(mysql));
  return ret;

err:
  if (mysql_errno(mysql) && is_network_error(mysql_errno(mysql)))
  {
    mi->report(WARNING_LEVEL, mysql_errno(mysql),
               "The initialization command '%s' failed with the following"
               " error: '%s'.", query, mysql_error(mysql));
    ret= 2;
  }
  else
  {
    char errmsg[512];
    const char *errmsg_fmt=
      "The slave I/O thread stops because a fatal error is encountered "
      "when it tries to send query to master(query: %s).";

    sprintf(errmsg, errmsg_fmt, query);
    mi->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR, ER(ER_SLAVE_FATAL_ERROR),
               errmsg);
    ret= 1;
  }
  mysql_free_result(mysql_store_result(mysql));
  return ret;
}

/**
  Get master's uuid on connecting.

  @param  mysql MYSQL to request uuid from master.
  @param  mi    Master_info to set master_uuid

  @return 0: Success, 1: Fatal error, 2: Network error.
*/
static int get_master_uuid(MYSQL *mysql, Master_info *mi)
{
  const char *errmsg;
  MYSQL_RES *master_res= NULL;
  MYSQL_ROW master_row= NULL;
  int ret= 0;

  DBUG_EXECUTE_IF("dbug.before_get_MASTER_UUID",
                  {
                    const char act[]= "now wait_for signal.get_master_uuid";
                    DBUG_ASSERT(opt_debug_sync_timeout > 0);
                    DBUG_ASSERT(!debug_sync_set_action(current_thd,
                                                       STRING_WITH_LEN(act)));
                  };);

  DBUG_EXECUTE_IF("dbug.simulate_busy_io",
                  {
                    const char act[]= "now signal Reached wait_for signal.got_stop_slave";
                    DBUG_ASSERT(opt_debug_sync_timeout > 0);
                    DBUG_ASSERT(!debug_sync_set_action(current_thd,
                                                       STRING_WITH_LEN(act)));
                  };);
  if (!mysql_real_query(mysql,
                        STRING_WITH_LEN("SHOW VARIABLES LIKE 'SERVER_UUID'")) &&
      (master_res= mysql_store_result(mysql)) &&
      (master_row= mysql_fetch_row(master_res)))
  {
    if (!strcmp(::server_uuid, master_row[1]) &&
        !mi->rli->replicate_same_server_id)
    {
      errmsg= "The slave I/O thread stops because master and slave have equal "
              "MySQL server UUIDs; these UUIDs must be different for "
              "replication to work.";
      mi->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR, ER(ER_SLAVE_FATAL_ERROR),
                 errmsg);
      // Fatal error
      ret= 1;
    }
    else
    {
      if (mi->master_uuid[0] != 0 && strcmp(mi->master_uuid, master_row[1]))
        sql_print_warning("The master's UUID has changed, although this should"
                          " not happen unless you have changed it manually."
                          " The old UUID was %s.",
                          mi->master_uuid);
      strncpy(mi->master_uuid, master_row[1], UUID_LENGTH);
      mi->master_uuid[UUID_LENGTH]= 0;
    }
  }
  else if (mysql_errno(mysql))
  {
    if (is_network_error(mysql_errno(mysql)))
    {
      mi->report(WARNING_LEVEL, mysql_errno(mysql),
                 "Get master SERVER_UUID failed with error: %s",
                 mysql_error(mysql));
      ret= 2;
    }
    else
    {
      /* Fatal error */
      errmsg= "The slave I/O thread stops because a fatal error is encountered "
        "when it tries to get the value of SERVER_UUID variable from master.";
      mi->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR, ER(ER_SLAVE_FATAL_ERROR),
                 errmsg);
      ret= 1;
    }
  }
  else if (!master_row && master_res)
  {
    mi->report(WARNING_LEVEL, ER_UNKNOWN_SYSTEM_VARIABLE,
               "Unknown system variable 'SERVER_UUID' on master. "
               "A probable cause is that the variable is not supported on the "
               "master (version: %s), even though it is on the slave (version: %s)",
               mysql->server_version, server_version);
  }

  if (master_res)
    mysql_free_result(master_res);
  return ret;
}


/**
  Determine, case-sensitively, if short_string is equal to
  long_string, or a true prefix of long_string, or not a prefix.

  @retval 0 short_string is not a prefix of long_string.
  @retval 1 short_string is a true prefix of long_string (not equal).
  @retval 2 short_string is equal to long_string.
*/
static int is_str_prefix_case(const char *short_string, const char *long_string)
{
  int i;
  for (i= 0; short_string[i]; i++)
    if (my_toupper(system_charset_info, short_string[i]) !=
        my_toupper(system_charset_info, long_string[i]))
      return 0;
  return long_string[i] ? 1 : 2;
}

/*
  Note that we rely on the master's version (3.23, 4.0.14 etc) instead of
  relying on the binlog's version. This is not perfect: imagine an upgrade
  of the master without waiting that all slaves are in sync with the master;
  then a slave could be fooled about the binlog's format. This is what happens
  when people upgrade a 3.23 master to 4.0 without doing RESET MASTER: 4.0
  slaves are fooled. So we do this only to distinguish between 3.23 and more
  recent masters (it's too late to change things for 3.23).

  RETURNS
  0       ok
  1       error
  2       transient network problem, the caller should try to reconnect
*/

static int get_master_version_and_clock(MYSQL* mysql, Master_info* mi)
{
  char err_buff[MAX_SLAVE_ERRMSG];
  const char* errmsg= 0;
  int err_code= 0;
  int version_number=0;
  version_number= atoi(mysql->server_version);

  const char query_global_attr_version_four[] =
    "SELECT @@global.server_id, @@global.collation_server, @@global.time_zone";
  const char query_global_attr_older_versions[] = "SELECT @@global.server_id";

  const char* global_server_id = NULL;
  const char* global_collation_server = NULL;
  const char* global_time_zone = NULL;

  mi->ignore_checksum_alg = false;
  MYSQL_RES *master_res= 0;
  MYSQL_ROW master_row;
  DBUG_ENTER("get_master_version_and_clock");

  /*
    Free old mi_description_event (that is needed if we are in
    a reconnection).
  */
  DBUG_EXECUTE_IF("unrecognized_master_version",
                 {
                   version_number= 1;
                 };);
  mysql_mutex_lock(&mi->data_lock);
  mi->set_mi_description_event(NULL);

  if (!my_isdigit(&my_charset_bin,*mysql->server_version))
  {
    errmsg = "Master reported unrecognized MySQL version";
    err_code= ER_SLAVE_FATAL_ERROR;
    sprintf(err_buff, ER(err_code), errmsg);
  }
  else
  {
    /*
      Note the following switch will bug when we have MySQL branch 30 ;)
    */
    switch (version_number)
    {
    case 0:
    case 1:
    case 2:
      errmsg = "Master reported unrecognized MySQL version";
      err_code= ER_SLAVE_FATAL_ERROR;
      sprintf(err_buff, ER(err_code), errmsg);
      break;
    case 3:
      mi->set_mi_description_event(new
        Format_description_log_event(1, mysql->server_version));
      break;
    case 4:
      mi->set_mi_description_event(new
        Format_description_log_event(3, mysql->server_version));
      break;
    default:
      /*
        Master is MySQL >=5.0. Give a default Format_desc event, so that we can
        take the early steps (like tests for "is this a 3.23 master") which we
        have to take before we receive the real master's Format_desc which will
        override this one. Note that the Format_desc we create below is garbage
        (it has the format of the *slave*); it's only good to help know if the
        master is 3.23, 4.0, etc.
      */
      mi->set_mi_description_event(new
        Format_description_log_event(4, mysql->server_version));
      break;
    }
  }

  /*
     This does not mean that a 5.0 slave will be able to read a 5.5 master; but
     as we don't know yet, we don't want to forbid this for now. If a 5.0 slave
     can't read a 5.5 master, this will show up when the slave can't read some
     events sent by the master, and there will be error messages.
  */

  if (errmsg)
  {
    /* unlock the mutex on master info structure */
    mysql_mutex_unlock(&mi->data_lock);
    goto err;
  }

  /* as we are here, we tried to allocate the event */
  if (mi->get_mi_description_event() == NULL)
  {
    mysql_mutex_unlock(&mi->data_lock);
    errmsg= "default Format_description_log_event";
    err_code= ER_SLAVE_CREATE_EVENT_FAILURE;
    sprintf(err_buff, ER(err_code), errmsg);
    goto err;
  }

  if (mi->get_mi_description_event()->binlog_version < 4 &&
      opt_slave_sql_verify_checksum)
  {
    sql_print_warning("Found a master with MySQL server version older than "
                      "5.0. With checksums enabled on the slave, replication "
                      "might not work correctly. To ensure correct "
                      "replication, restart the slave server with "
                      "--slave_sql_verify_checksum=0.");
  }
  /*
    FD_q's (A) is set initially from RL's (A): FD_q.(A) := RL.(A).
    It's necessary to adjust FD_q.(A) at this point because in the following
    course FD_q is going to be dumped to RL.
    Generally FD_q is derived from a received FD_m (roughly FD_q := FD_m)
    in queue_event and the master's (A) is installed.
    At one step with the assignment the Relay-Log's checksum alg is set to
    a new value: RL.(A) := FD_q.(A). If the slave service is stopped
    the last time assigned RL.(A) will be passed over to the restarting
    service (to the current execution point).
    RL.A is a "codec" to verify checksum in queue_event() almost all the time
    the first fake Rotate event.
    Starting from this point IO thread will executes the following checksum
    warmup sequence  of actions:

    FD_q.A := RL.A,
    A_m^0 := master.@@global.binlog_checksum,
    {queue_event(R_f): verifies(R_f, A_m^0)},
    {queue_event(FD_m): verifies(FD_m, FD_m.A), dump(FD_q), rotate(RL),
                        FD_q := FD_m, RL.A := FD_q.A)}

    See legends definition on MYSQL_BIN_LOG::relay_log_checksum_alg
    docs lines (binlog.h).
    In above A_m^0 - the value of master's
    @@binlog_checksum determined in the upcoming handshake (stored in
    mi->checksum_alg_before_fd).


    After the warm-up sequence IO gets to "normal" checksum verification mode
    to use RL.A in

    {queue_event(E_m): verifies(E_m, RL.A)}

    until it has received a new FD_m.
  */
  mi->get_mi_description_event()->checksum_alg=
    mi->rli->relay_log.relay_log_checksum_alg;

  DBUG_ASSERT(mi->get_mi_description_event()->checksum_alg !=
              BINLOG_CHECKSUM_ALG_UNDEF);
  DBUG_ASSERT(mi->rli->relay_log.relay_log_checksum_alg !=
              BINLOG_CHECKSUM_ALG_UNDEF);

  mi->clock_diff_with_master= 0; /* The "most sensible" value */
  mysql_mutex_unlock(&mi->data_lock);

  /*
    Check that the master's server id and ours are different. Because if they
    are equal (which can result from a simple copy of master's datadir to slave,
    thus copying some my.cnf), replication will work but all events will be
    skipped.
    Do not die if SHOW VARIABLES LIKE 'SERVER_ID' fails on master (very old
    master?).
    Note: we could have put a @@SERVER_ID in the previous SELECT
    UNIX_TIMESTAMP() instead, but this would not have worked on 3.23 masters.
  */
  DBUG_EXECUTE_IF("dbug.before_get_SERVER_ID",
                  {
                    const char act[]=
                      "now "
                      "wait_for signal.get_server_id";
                    DBUG_ASSERT(opt_debug_sync_timeout > 0);
                    DBUG_ASSERT(!debug_sync_set_action(current_thd,
                                                       STRING_WITH_LEN(act)));
                  };);
  master_res= NULL;
  master_row= NULL;
  if (*mysql->server_version == '4')
  {
    if (!mysql_real_query(mysql,
                          STRING_WITH_LEN(query_global_attr_version_four)) &&
        (master_res= mysql_store_result(mysql)) &&
        (master_row= mysql_fetch_row(master_res)))
    {
      global_server_id = master_row[0];
      global_collation_server = master_row[1];
      global_time_zone = master_row[2];
    }
    else if (mysql_errno(mysql))
    {
      if (check_io_slave_killed(mi->info_thd, mi, NULL))
        goto slave_killed_err;
      else if (is_network_error(mysql_errno(mysql)))
      {
        mi->report(WARNING_LEVEL, mysql_errno(mysql),
                   "Get master SERVER_ID, COLLATION_SERVER and TIME_ZONE "
                   "failed with error: %s", mysql_error(mysql));
        goto network_err;
      }
      else if (mysql_errno(mysql) != ER_UNKNOWN_SYSTEM_VARIABLE)
      {
        /* Fatal error */
        errmsg = "The slave I/O thread stops because a fatal error is "
                 "encountered when it try to get the value of SERVER_ID, "
                 "COLLATION_SERVER and TIME_ZONE global variable from master.";
        err_code = mysql_errno(mysql);
        sprintf(err_buff, "%s Error: %s", errmsg, mysql_error(mysql));
        goto err;
      }
      else
      {
        mi->report(WARNING_LEVEL, ER_UNKNOWN_SYSTEM_VARIABLE,
                   "Unknown system variable 'SERVER_ID', 'COLLATION_SERVER' or "
                   "'TIME_ZONE' on master, maybe it is a *VERY OLD MASTER*. "
                   "*NOTE*: slave may experience inconsistency if replicated "
                   "data deals with collation.");
      }
    }
  }
  else
  {
    if (!mysql_real_query(mysql,
                          STRING_WITH_LEN(query_global_attr_older_versions)) &&
        (master_res = mysql_store_result(mysql)) &&
        (master_row = mysql_fetch_row(master_res)))
    {
      global_server_id = master_row[0];
    }
    else if (mysql_errno(mysql))
    {
      if (check_io_slave_killed(mi->info_thd, mi, NULL))
      {
        goto slave_killed_err;
      }
      else if (is_network_error(mysql_errno(mysql)))
      {
        mi->report(WARNING_LEVEL, mysql_errno(mysql),
                   "Get master SERVER_ID failed with error: %s",
                   mysql_error(mysql));
        goto network_err;
      }
      else if (mysql_errno(mysql) != ER_UNKNOWN_SYSTEM_VARIABLE)
      {
        /* Fatal error */
        errmsg = "The slave I/O thread stops because a fatal error is "
                 "encountered when it try to get the value of SERVER_ID global "
                 "variable from master.";
        err_code = mysql_errno(mysql);
        sprintf(err_buff, "%s Error: %s", errmsg, mysql_error(mysql));
        goto err;
      }
      else
      {
        mi->report(WARNING_LEVEL, ER_UNKNOWN_SYSTEM_VARIABLE,
                   "Unknown system variable 'SERVER_ID' on master, "
                   "maybe it is a *VERY OLD MASTER*. *NOTE*: slave may "
                   "experience inconsistency if replicated data deals "
                   "with collation.");
      }
    }
  }

  if (global_server_id != NULL &&
      (::server_id == (mi->master_id= strtoul(global_server_id, 0, 10))) &&
       !mi->rli->replicate_same_server_id)
  {
      errmsg = "The slave I/O thread stops because master and slave have equal "
               "MySQL server ids; these ids must be different for replication "
               "to work (or the --replicate-same-server-id option must be used "
               "on slave but this does not always make sense; please check the "
               "manual before using it).";
      err_code= ER_SLAVE_FATAL_ERROR;
      sprintf(err_buff, ER(err_code), errmsg);
      goto err;
  }
  if (mi->master_id == 0 &&
           mi->ignore_server_ids->dynamic_ids.elements > 0)
  {
    errmsg = "Slave configured with server id filtering could not detect the "
             "master server id.";
    err_code= ER_SLAVE_FATAL_ERROR;
    sprintf(err_buff, ER(err_code), errmsg);
    goto err;
  }

  /*
    Check that the master's global character_set_server and ours are the same.
    Not fatal if query fails (old master?).
    Note that we don't check for equality of global character_set_client and
    collation_connection (neither do we prevent their setting in
    set_var.cc). That's because from what I (Guilhem) have tested, the global
    values of these 2 are never used (new connections don't use them).
    We don't test equality of global collation_database either as it's is
    going to be deprecated (made read-only) in 4.1 very soon.
    The test is only relevant if master < 5.0.3 (we'll test only if it's older
    than the 5 branch; < 5.0.3 was alpha...), as >= 5.0.3 master stores
    charset info in each binlog event.
    We don't do it for 3.23 because masters <3.23.50 hang on
    SELECT @@unknown_var (BUG#7965 - see changelog of 3.23.50). So finally we
    test only if master is 4.x.
  */

  /* redundant with rest of code but safer against later additions */
  if (*mysql->server_version == '3')
    goto err;

  if (*mysql->server_version == '4' &&
      global_collation_server != NULL &&
      strcmp(global_collation_server,
             global_system_variables.collation_server->name))
  {
        errmsg = "The slave I/O thread stops because master and slave have "
                 "different values for the COLLATION_SERVER global variable. "
                 "The values must be equal for the Statement-format "
                 "replication to work";
        err_code = ER_SLAVE_FATAL_ERROR;
        sprintf(err_buff, ER(err_code), errmsg);
        goto err;
  }

  /*
    Perform analogous check for time zone. Theoretically we also should
    perform check here to verify that SYSTEM time zones are the same on
    slave and master, but we can't rely on value of @@system_time_zone
    variable (it is time zone abbreviation) since it determined at start
    time and so could differ for slave and master even if they are really
    in the same system time zone. So we are omiting this check and just
    relying on documentation. Also according to Monty there are many users
    who are using replication between servers in various time zones. Hence
    such check will broke everything for them. (And now everything will
    work for them because by default both their master and slave will have
    'SYSTEM' time zone).
    This check is only necessary for 4.x masters (and < 5.0.4 masters but
    those were alpha).
  */
  if (*mysql->server_version == '4' &&
      global_time_zone != NULL &&
      strcmp(global_time_zone,
             global_system_variables.time_zone->get_name()->ptr()))
  {
        errmsg= "The slave I/O thread stops because master and slave have \
different values for the TIME_ZONE global variable. The values must \
be equal for the Statement-format replication to work";
        err_code= ER_SLAVE_FATAL_ERROR;
        sprintf(err_buff, ER(err_code), errmsg);
        goto err;
  }

  /*
    Cleans the queries for UNIX_TIMESTAMP, server_id, collation_server and
    time_zone
    Dont free the memory before, this will free the strings too.
  */
  if (master_res)
  {
    mysql_free_result(master_res);
    master_res = NULL;
  }

  if (mi->heartbeat_period != 0.0)
  {
    char llbuf[22];
    const char query_format[]= "SET @master_heartbeat_period= %s";
    char query[sizeof(query_format) - 2 + sizeof(llbuf)];
    /*
       the period is an ulonglong of nano-secs.
    */
    llstr((ulonglong) (mi->heartbeat_period*1000000000UL), llbuf);
    sprintf(query, query_format, llbuf);

    if (mysql_real_query(mysql, query, strlen(query)))
    {
      if (check_io_slave_killed(mi->info_thd, mi, NULL))
        goto slave_killed_err;

      if (is_network_error(mysql_errno(mysql)))
      {
        mi->report(WARNING_LEVEL, mysql_errno(mysql),
                   "SET @master_heartbeat_period to master failed with error: %s",
                   mysql_error(mysql));
        mysql_free_result(mysql_store_result(mysql));
        goto network_err;
      }
      else
      {
        /* Fatal error */
        errmsg= "The slave I/O thread stops because a fatal error is encountered "
          " when it tries to SET @master_heartbeat_period on master.";
        err_code= ER_SLAVE_FATAL_ERROR;
        sprintf(err_buff, "%s Error: %s", errmsg, mysql_error(mysql));
        mysql_free_result(mysql_store_result(mysql));
        goto err;
      }
    }
    mysql_free_result(mysql_store_result(mysql));
  }

  /*
    Querying if master is capable to checksum and notifying it about own
    CRC-awareness. The master's side instant value of @@global.binlog_checksum
    is stored in the dump thread's uservar area as well as cached locally
    to become known in consensus by master and slave.
  */
  if (DBUG_EVALUATE_IF("simulate_slave_unaware_checksum", 0, 1))
  {
    int rc;
    const char query[]= "SET @master_binlog_checksum= @@global.binlog_checksum";
    master_res= NULL;
    mi->checksum_alg_before_fd= BINLOG_CHECKSUM_ALG_UNDEF; //initially undefined
    /*
      @c checksum_alg_before_fd is queried from master in this block.
      If master is old checksum-unaware the value stays undefined.
      Once the first FD will be received its alg descriptor will replace
      the being queried one.
    */
    rc= mysql_real_query(mysql, query, strlen(query));
    if (rc != 0)
    {
      mi->checksum_alg_before_fd= BINLOG_CHECKSUM_ALG_OFF;
      if (check_io_slave_killed(mi->info_thd, mi, NULL))
        goto slave_killed_err;

      if (mysql_errno(mysql) == ER_UNKNOWN_SYSTEM_VARIABLE)
      {
        // this is tolerable as OM -> NS is supported
        mi->ignore_checksum_alg = true;
        mi->report(WARNING_LEVEL, mysql_errno(mysql),
                   "Notifying master by %s failed with "
                   "error: %s", query, mysql_error(mysql));
      }
      else
      {
        if (is_network_error(mysql_errno(mysql)))
        {
          mi->report(WARNING_LEVEL, mysql_errno(mysql),
                     "Notifying master by %s failed with "
                     "error: %s", query, mysql_error(mysql));
          mysql_free_result(mysql_store_result(mysql));
          goto network_err;
        }
        else
        {
          errmsg= "The slave I/O thread stops because a fatal error is encountered "
            "when it tried to SET @master_binlog_checksum on master.";
          err_code= ER_SLAVE_FATAL_ERROR;
          sprintf(err_buff, "%s Error: %s", errmsg, mysql_error(mysql));
          mysql_free_result(mysql_store_result(mysql));
          goto err;
        }
      }
    }
    else
    {
      mysql_free_result(mysql_store_result(mysql));
      if (!mysql_real_query(mysql,
                            STRING_WITH_LEN("SELECT @master_binlog_checksum")) &&
          (master_res= mysql_store_result(mysql)) &&
          (master_row= mysql_fetch_row(master_res)) &&
          (master_row[0] != NULL))
      {
        mi->checksum_alg_before_fd= (uint8)
          find_type(master_row[0], &binlog_checksum_typelib, 1) - 1;

       DBUG_EXECUTE_IF("undefined_algorithm_on_slave",
        mi->checksum_alg_before_fd = BINLOG_CHECKSUM_ALG_UNDEF;);
       if(mi->checksum_alg_before_fd == BINLOG_CHECKSUM_ALG_UNDEF)
       {
         errmsg= "The slave I/O thread was stopped because a fatal error is encountered "
                 "The checksum algorithm used by master is unknown to slave.";
         err_code= ER_SLAVE_FATAL_ERROR;
         sprintf(err_buff, "%s Error: %s", errmsg, mysql_error(mysql));
         mysql_free_result(mysql_store_result(mysql));
         goto err;
       }

        // valid outcome is either of
        DBUG_ASSERT(mi->checksum_alg_before_fd == BINLOG_CHECKSUM_ALG_OFF ||
                    mi->checksum_alg_before_fd == BINLOG_CHECKSUM_ALG_CRC32);
      }
      else if (check_io_slave_killed(mi->info_thd, mi, NULL))
        goto slave_killed_err;
      else if (is_network_error(mysql_errno(mysql)))
      {
        mi->report(WARNING_LEVEL, mysql_errno(mysql),
                   "Get master BINLOG_CHECKSUM failed with error: %s", mysql_error(mysql));
        goto network_err;
      }
      else
      {
        errmsg= "The slave I/O thread stops because a fatal error is encountered "
          "when it tried to SELECT @master_binlog_checksum.";
        err_code= ER_SLAVE_FATAL_ERROR;
        sprintf(err_buff, "%s Error: %s", errmsg, mysql_error(mysql));
        mysql_free_result(mysql_store_result(mysql));
        goto err;
      }
    }
    if (master_res)
    {
      mysql_free_result(master_res);
      master_res= NULL;
    }
  }
  else
    mi->checksum_alg_before_fd= BINLOG_CHECKSUM_ALG_OFF;

  if (DBUG_EVALUATE_IF("simulate_slave_unaware_gtid", 0, 1))
  {
    switch (io_thread_init_command(mi, "SELECT @@GLOBAL.GTID_MODE",
                                   ER_UNKNOWN_SYSTEM_VARIABLE,
                                   &master_res, &master_row))
    {
    case COMMAND_STATUS_ERROR:
      DBUG_RETURN(2);
    case COMMAND_STATUS_ALLOWED_ERROR:
      // master is old and does not have @@GLOBAL.GTID_MODE
      mi->master_gtid_mode= 0;
      break;
    case COMMAND_STATUS_OK:
      const char *master_gtid_mode_string= master_row[0];
      bool found_valid_mode= false;
      DBUG_EXECUTE_IF("simulate_master_has_gtid_mode_on_permissive",
                      { master_gtid_mode_string= "on_permissive"; });
      DBUG_EXECUTE_IF("simulate_master_has_gtid_mode_off_permissive",
                      { master_gtid_mode_string= "off_permissive"; });
      DBUG_EXECUTE_IF("simulate_master_has_gtid_mode_on_something",
                      { master_gtid_mode_string= "on_something"; });
      DBUG_EXECUTE_IF("simulate_master_has_gtid_mode_off_something",
                      { master_gtid_mode_string= "off_something"; });
      DBUG_EXECUTE_IF("simulate_master_has_unknown_gtid_mode",
                      { master_gtid_mode_string= "Krakel Spektakel"; });
      for (int mode= 0; mode <= 3 && !found_valid_mode; mode+= 3)
      {
        switch (is_str_prefix_case(gtid_mode_typelib.type_names[mode],
                                   master_gtid_mode_string))
        {
        case 0: // is not a prefix
          break;
        case 1: // is a true prefix, i.e. not equal
          mi->report(WARNING_LEVEL, ER_UNKNOWN_ERROR,
                     "The master uses an unknown GTID_MODE '%s'. "
                     "Treating it as '%s'.",
                     master_gtid_mode_string,
                     gtid_mode_typelib.type_names[mode]);
          // fall through
        case 2: // is equal
          found_valid_mode= true;
          mi->master_gtid_mode= mode;
          break;
        }
      }
      if (!found_valid_mode)
      {
        mi->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR,
                   "The slave IO thread stops because the master has "
                   "an unknown @@GLOBAL.GTID_MODE '%s'.",
                   master_gtid_mode_string);
        mysql_free_result(master_res);
        DBUG_RETURN(1);
      }
      mysql_free_result(master_res);
      break;
    }
    if ((mi->master_gtid_mode > gtid_mode + 1 ||
        gtid_mode > mi->master_gtid_mode + 1) &&
        !enable_gtid_mode_on_new_slave_with_old_master &&
        !read_only)
    {
      mi->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR,
                 "The slave IO thread stops because the master has "
                 "@@GLOBAL.GTID_MODE %s and this server has "
                 "@@GLOBAL.GTID_MODE %s",
                 gtid_mode_names[mi->master_gtid_mode],
                 gtid_mode_names[gtid_mode]);
      DBUG_RETURN(1);
    }
    if (mi->is_auto_position() && mi->master_gtid_mode != 3)
    {
      mi->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR,
                 "The slave IO thread stops because the master has "
                 "@@GLOBAL.GTID_MODE %s and we are trying to connect "
                 "using MASTER_AUTO_POSITION.",
                 gtid_mode_names[mi->master_gtid_mode]);
      DBUG_RETURN(1);
    }
  }

err:
  if (errmsg)
  {
    if (master_res)
      mysql_free_result(master_res);
    DBUG_ASSERT(err_code != 0);
    mi->report(ERROR_LEVEL, err_code, "%s", err_buff);
    DBUG_RETURN(1);
  }

  DBUG_RETURN(0);

network_err:
  if (master_res)
    mysql_free_result(master_res);
  DBUG_RETURN(2);

slave_killed_err:
  if (master_res)
    mysql_free_result(master_res);
  DBUG_RETURN(2);
}

static bool wait_for_relay_log_space(Relay_log_info* rli)
{
  bool slave_killed=0;
  Master_info* mi = rli->mi;
  PSI_stage_info old_stage;
  THD* thd = mi->info_thd;
  DBUG_ENTER("wait_for_relay_log_space");

  mysql_mutex_lock(&rli->log_space_lock);
  thd->ENTER_COND(&rli->log_space_cond,
                  &rli->log_space_lock,
                  &stage_waiting_for_relay_log_space,
                  &old_stage);
  while (rli->log_space_limit < rli->log_space_total &&
         !(slave_killed=io_slave_killed(thd,mi)) &&
         !rli->ignore_log_space_limit)
    mysql_cond_wait(&rli->log_space_cond, &rli->log_space_lock);

  /*
    Makes the IO thread read only one event at a time
    until the SQL thread is able to purge the relay
    logs, freeing some space.

    Therefore, once the SQL thread processes this next
    event, it goes to sleep (no more events in the queue),
    sets ignore_log_space_limit=true and wakes the IO thread.
    However, this event may have been enough already for
    the SQL thread to purge some log files, freeing
    rli->log_space_total .

    This guarantees that the SQL and IO thread move
    forward only one event at a time (to avoid deadlocks),
    when the relay space limit is reached. It also
    guarantees that when the SQL thread is prepared to
    rotate (to be able to purge some logs), the IO thread
    will know about it and will rotate.

    NOTE: The ignore_log_space_limit is only set when the SQL
          thread sleeps waiting for events.

   */
  if (rli->ignore_log_space_limit)
  {
#ifndef DBUG_OFF
    {
      char llbuf1[22], llbuf2[22];
      DBUG_PRINT("info", ("log_space_limit=%s "
                          "log_space_total=%s "
                          "ignore_log_space_limit=%d "
                          "sql_force_rotate_relay=%d",
                        llstr(rli->log_space_limit,llbuf1),
                        llstr(rli->log_space_total,llbuf2),
                        (int) rli->ignore_log_space_limit,
                        (int) rli->sql_force_rotate_relay));
    }
#endif
    if (rli->sql_force_rotate_relay)
    {
      mysql_mutex_lock(&mi->data_lock);
      rotate_relay_log(mi, false/*need_log_space_lock=false*/);
      mysql_mutex_unlock(&mi->data_lock);
      rli->sql_force_rotate_relay= false;
    }

    rli->ignore_log_space_limit= false;
  }

  thd->EXIT_COND(&old_stage);
  DBUG_RETURN(slave_killed);
}


/*
  Builds a Rotate from the ignored events' info and writes it to relay log.

  The caller must hold mi->data_lock before invoking this function.

  @param thd pointer to I/O Thread's Thd.
  @param mi  point to I/O Thread metadata class.

  @return 0 if everything went fine, 1 otherwise.
*/
static int write_ignored_events_info_to_relay_log(THD *thd, Master_info *mi)
{
  Relay_log_info *rli= mi->rli;
  mysql_mutex_t *log_lock= rli->relay_log.get_log_lock();
  int error= 0;
  DBUG_ENTER("write_ignored_events_info_to_relay_log");

  DBUG_ASSERT(thd == mi->info_thd);
  mysql_mutex_assert_owner(&mi->data_lock);
  mysql_mutex_lock(log_lock);
  if (rli->ign_master_log_name_end[0])
  {
    DBUG_PRINT("info",("writing a Rotate event to track down ignored events"));
    Rotate_log_event *ev= new Rotate_log_event(rli->ign_master_log_name_end,
                                               0, rli->ign_master_log_pos_end,
                                               Rotate_log_event::DUP_NAME);
    if (mi->get_mi_description_event() != NULL)
      ev->checksum_alg= mi->get_mi_description_event()->checksum_alg;

    rli->ign_master_log_name_end[0]= 0;
    /* can unlock before writing as slave SQL thd will soon see our Rotate */
    mysql_mutex_unlock(log_lock);
    if (likely((bool)ev))
    {
      ev->server_id= 0; // don't be ignored by slave SQL thread
      if (unlikely(rli->relay_log.append_event(ev, mi) != 0))
        mi->report(ERROR_LEVEL, ER_SLAVE_RELAY_LOG_WRITE_FAILURE,
                   ER(ER_SLAVE_RELAY_LOG_WRITE_FAILURE),
                   "failed to write a Rotate event"
                   " to the relay log, SHOW SLAVE STATUS may be"
                   " inaccurate");
      rli->relay_log.harvest_bytes_written(rli, true/*need_log_space_lock=true*/);
      if (flush_master_info(mi, TRUE))
      {
        error= 1;
        sql_print_error("Failed to flush master info file.");
      }
      delete ev;
    }
    else
    {
      error= 1;
      mi->report(ERROR_LEVEL, ER_SLAVE_CREATE_EVENT_FAILURE,
                 ER(ER_SLAVE_CREATE_EVENT_FAILURE),
                 "Rotate_event (out of memory?),"
                 " SHOW SLAVE STATUS may be inaccurate");
    }
  }
  else
    mysql_mutex_unlock(log_lock);

  DBUG_RETURN(error);
}

/**
  Returns slave lag duration relative to master.
  @param mi Pointer to Master_info object for the IO thread.

  @retval pair(sec_behind_master, milli_second_behind_master)
  This function sets above values to -1 to represent nulls
*/
std::pair<longlong, longlong> get_time_lag_behind_master(Master_info *mi) {
  longlong sec_behind_master = -1;
  longlong milli_sec_behind_master = -1;
  /*
      The pseudo code to compute Seconds_Behind_Master:
        if (SQL thread is running)
        {
          if (SQL thread processed all the available relay log)
          {
            if (IO thread is running)
              print 0;
            else
              print NULL;
          }
          else
            compute Seconds_Behind_Master;
        }
        else
          print NULL;
    */

  bool sbm_is_null = false;
  bool sbm_is_zero = false;
  if (mi->rli->slave_running) {
    time_t now = time(0);
    /* Check if SQL thread is at the end of relay log
         Checking should be done using two conditions
         condition1: compare the log positions and
         condition2: compare the file names (to handle rotation case)
    */
    if (reset_seconds_behind_master &&
        (mi->get_master_log_pos() == mi->rli->get_group_master_log_pos()) &&
        (!strcmp(mi->get_master_log_name(),
                 mi->rli->get_group_master_log_name()))) {
      if (mi->slave_running == MYSQL_SLAVE_RUN_CONNECT)
        sec_behind_master = 0LL;
      else
        sec_behind_master = -1;
      sbm_is_zero = mi->slave_running == MYSQL_SLAVE_RUN_CONNECT;
      sbm_is_null = !sbm_is_zero;
    } else {
      long time_diff = ((long)(now - mi->rli->last_master_timestamp) -
                        mi->clock_diff_with_master);
      /*
        Apparently on some systems time_diff can be <0. Here are possible
        reasons related to MySQL:
        - the master is itself a slave of another master whose time is ahead.
        - somebody used an explicit SET TIMESTAMP on the master.
        Possible reason related to granularity-to-second of time functions
        (nothing to do with MySQL), which can explain a value of -1:
        assume the master's and slave's time are perfectly synchronized, and
        that at slave's connection time, when the master's timestamp is read,
        it is at the very end of second 1, and (a very short time later) when
        the slave's timestamp is read it is at the very beginning of second
        2. Then the recorded value for master is 1 and the recorded value for
        slave is 2. At SHOW SLAVE STATUS time, assume that the difference
        between timestamp of slave and rli->last_master_timestamp is 0
        (i.e. they are in the same second), then we get 0-(2-1)=-1 as a result.
        This confuses users, so we don't go below 0: hence the max().

        rli->slave_has_caughup is a special flag to say
        "consider we have caught up" to update the seconds behind master.
      */
      switch (mi->rli->slave_has_caughtup) {
      case Enum_slave_caughtup::NONE:
        sec_behind_master = -1;
        sbm_is_null = true;
        break;
      case Enum_slave_caughtup::YES:
        sec_behind_master = 0LL;
        sbm_is_zero = true;
        break;
      case Enum_slave_caughtup::NO:
        sec_behind_master = (longlong)(max(0L, time_diff));
        break;
      default:
        DBUG_ASSERT(0);
      }
    }
  } else {
    sec_behind_master = -1;
    sbm_is_null = true;
  }

  // Milli_Seconds_Behind_Master
  if (opt_binlog_trx_meta_data) {
    if (sbm_is_null)
      milli_sec_behind_master = -1;
    else if (sbm_is_zero)
      milli_sec_behind_master = 0LL;
    else {
      ulonglong now_millis =
          duration_cast<milliseconds>(system_clock::now().time_since_epoch())
              .count();
      // adjust for clock mismatch
      now_millis -= mi->clock_diff_with_master * 1000;
      milli_sec_behind_master =
          now_millis - mi->rli->last_master_timestamp_millis;
    }
  }
  return std::make_pair(sec_behind_master, milli_sec_behind_master);
}

int send_replica_statistics_to_master(MYSQL *mysql, Master_info *mi) {
  uchar buf[1024], *pos = buf;
  DBUG_ENTER("send_replica_statistics_to_master");

  int timestamp = my_time(0);
  std::pair<longlong, longlong> time_lag_behind_master =
      get_time_lag_behind_master(mi);
  int milli_sec_behind_master = max((int)time_lag_behind_master.second, 0);

  int4store(pos, server_id);
  pos += 4;
  int4store(pos, timestamp);
  pos += 4;
  int4store(pos, milli_sec_behind_master);
  pos += 4;

  if (simple_command(mysql, COM_SEND_REPLICA_STATISTICS, buf,
                     (size_t)(pos - buf), 0)) {
    DBUG_RETURN(1);
  }
  DBUG_RETURN(0);
}

int register_slave_on_master(MYSQL* mysql, Master_info *mi,
                             bool *suppress_warnings)
{
  uchar buf[1024], *pos= buf;
  uint report_host_len=0, report_user_len=0, report_password_len=0;
  DBUG_ENTER("register_slave_on_master");

  *suppress_warnings= FALSE;
  if (report_host)
    report_host_len= strlen(report_host);
  if (report_host_len > HOSTNAME_LENGTH)
  {
    sql_print_warning("The length of report_host is %d. "
                      "It is larger than the max length(%d), so this "
                      "slave cannot be registered to the master.",
                      report_host_len, HOSTNAME_LENGTH);
    DBUG_RETURN(0);
  }

  if (report_user)
    report_user_len= strlen(report_user);
  if (report_user_len > USERNAME_LENGTH)
  {
    sql_print_warning("The length of report_user is %d. "
                      "It is larger than the max length(%d), so this "
                      "slave cannot be registered to the master.",
                      report_user_len, USERNAME_LENGTH);
    DBUG_RETURN(0);
  }

  if (report_password)
    report_password_len= strlen(report_password);
  if (report_password_len > MAX_PASSWORD_LENGTH)
  {
    sql_print_warning("The length of report_password is %d. "
                      "It is larger than the max length(%d), so this "
                      "slave cannot be registered to the master.",
                      report_password_len, MAX_PASSWORD_LENGTH);
    DBUG_RETURN(0);
  }

  int4store(pos, server_id); pos+= 4;
  pos= net_store_data(pos, (uchar*) report_host, report_host_len);
  pos= net_store_data(pos, (uchar*) report_user, report_user_len);
  pos= net_store_data(pos, (uchar*) report_password, report_password_len);
  int2store(pos, (uint16) report_port); pos+= 2;
  /*
    Fake rpl_recovery_rank, which was removed in BUG#13963,
    so that this server can register itself on old servers,
    see BUG#49259.
   */
  int4store(pos, /* rpl_recovery_rank */ 0);    pos+= 4;
  /* The master will fill in master_id */
  int4store(pos, 0);                    pos+= 4;

  if (simple_command(mysql, COM_REGISTER_SLAVE, buf, (size_t) (pos- buf), 0))
  {
    if (mysql_errno(mysql) == ER_NET_READ_INTERRUPTED)
    {
      *suppress_warnings= TRUE;                 // Suppress reconnect warning
    }
    else if (!check_io_slave_killed(mi->info_thd, mi, NULL))
    {
      char buf[256];
      my_snprintf(buf, sizeof(buf), "%s (Errno: %d)", mysql_error(mysql),
                  mysql_errno(mysql));
      mi->report(ERROR_LEVEL, ER_SLAVE_MASTER_COM_FAILURE,
                 ER(ER_SLAVE_MASTER_COM_FAILURE), "COM_REGISTER_SLAVE", buf);
    }
    DBUG_RETURN(1);
  }

  DBUG_EXECUTE_IF("simulate_register_slave_killed", {
    mi->abort_slave = 1;
    DBUG_RETURN(1);
    };);

  DBUG_RETURN(0);
}


/**
  Execute a SHOW SLAVE STATUS statement.

  @param thd Pointer to THD object for the client thread executing the
  statement.

  @param mi Pointer to Master_info object for the IO thread.

  @retval FALSE success
  @retval TRUE failure
*/
bool show_slave_status(THD* thd, Master_info* mi)
{
  // TODO: fix this for multi-master
  List<Item> field_list;
  Protocol *protocol= thd->protocol;
  char *slave_sql_running_state= NULL;
  char *sql_gtid_set_buffer= NULL, *io_gtid_set_buffer= NULL;
  int sql_gtid_set_size= 0, io_gtid_set_size= 0;
  DBUG_ENTER("show_slave_status");

  if (mi != NULL)
  {
    global_sid_lock->wrlock();
    const Gtid_set* sql_gtid_set= gtid_state->get_logged_gtids();
    const Gtid_set* io_gtid_set= mi->rli->get_gtid_set();
    if ((sql_gtid_set_size= sql_gtid_set->to_string(&sql_gtid_set_buffer)) < 0 ||
        (io_gtid_set_size= io_gtid_set->to_string(&io_gtid_set_buffer)) < 0)
    {
      my_eof(thd);
      my_free(sql_gtid_set_buffer);
      my_free(io_gtid_set_buffer);
      global_sid_lock->unlock();
      DBUG_RETURN(true);
    }
    global_sid_lock->unlock();
  }

  field_list.push_back(new Item_empty_string("Slave_IO_State",
                                                     14));
  field_list.push_back(new Item_empty_string("Master_Host", mi != NULL ?
                                                     sizeof(mi->host) : 0));
  field_list.push_back(new Item_empty_string("Master_User", mi != NULL ?
                                                     mi->get_user_size() : 0));
  field_list.push_back(new Item_return_int("Master_Port", 7,
                                           MYSQL_TYPE_LONG));
  field_list.push_back(new Item_return_int("Connect_Retry", 10,
                                           MYSQL_TYPE_LONG));
  field_list.push_back(new Item_empty_string("Master_Log_File",
                                             FN_REFLEN));
  field_list.push_back(new Item_return_int("Read_Master_Log_Pos", 10,
                                           MYSQL_TYPE_LONGLONG));
  field_list.push_back(new Item_empty_string("Relay_Log_File",
                                             FN_REFLEN));
  field_list.push_back(new Item_return_int("Relay_Log_Pos", 10,
                                           MYSQL_TYPE_LONGLONG));
  field_list.push_back(new Item_empty_string("Relay_Master_Log_File",
                                             FN_REFLEN));
  field_list.push_back(new Item_empty_string("Slave_IO_Running", 3));
  field_list.push_back(new Item_empty_string("Slave_SQL_Running", 3));
  field_list.push_back(new Item_empty_string("Replicate_Do_DB", 20));
  field_list.push_back(new Item_empty_string("Replicate_Ignore_DB", 20));
  field_list.push_back(new Item_empty_string("Replicate_Do_Table", 20));
  field_list.push_back(new Item_empty_string("Replicate_Ignore_Table", 23));
  field_list.push_back(new Item_empty_string("Replicate_Wild_Do_Table", 24));
  field_list.push_back(new Item_empty_string("Replicate_Wild_Ignore_Table",
                                             28));
  field_list.push_back(new Item_return_int("Last_Errno", 4, MYSQL_TYPE_LONG));
  field_list.push_back(new Item_empty_string("Last_Symbolic_Errno", 20));
  field_list.push_back(new Item_empty_string("Last_Error", 20));
  field_list.push_back(new Item_return_int("Skip_Counter", 10,
                                           MYSQL_TYPE_LONG));
  field_list.push_back(new Item_return_int("Exec_Master_Log_Pos", 10,
                                           MYSQL_TYPE_LONGLONG));
  field_list.push_back(new Item_return_int("Relay_Log_Space", 10,
                                           MYSQL_TYPE_LONGLONG));
  field_list.push_back(new Item_empty_string("Until_Condition", 6));
  field_list.push_back(new Item_empty_string("Until_Log_File", FN_REFLEN));
  field_list.push_back(new Item_return_int("Until_Log_Pos", 10,
                                           MYSQL_TYPE_LONGLONG));
  field_list.push_back(new Item_empty_string("Master_SSL_Allowed", 7));
  field_list.push_back(new Item_empty_string("Master_SSL_CA_File", mi != NULL ?
                                             sizeof(mi->ssl_ca) : 0));
  field_list.push_back(new Item_empty_string("Master_SSL_CA_Path", mi != NULL ?
                                             sizeof(mi->ssl_capath) : 0));
  field_list.push_back(new Item_empty_string("Master_SSL_Cert", mi != NULL ?
                                             sizeof(mi->ssl_cert) : 0));
  field_list.push_back(new Item_empty_string("Master_SSL_Cipher", mi != NULL ?
                                             sizeof(mi->ssl_cipher) : 0));
  field_list.push_back(new Item_empty_string("Master_SSL_Key", mi != NULL ?
                                             sizeof(mi->ssl_key) : 0));
  field_list.push_back(new Item_return_int("Seconds_Behind_Master", 10,
                                           MYSQL_TYPE_LONGLONG));
  field_list.push_back(new Item_return_int("Lag_Peak_Over_Last_Period", 10,
                                           MYSQL_TYPE_LONGLONG));
  if (opt_binlog_trx_meta_data)
    field_list.push_back(new Item_return_int("Milli_Seconds_Behind_Master", 10,
                                             MYSQL_TYPE_LONGLONG));
  field_list.push_back(new Item_empty_string("Master_SSL_Verify_Server_Cert",
                                             3));
  field_list.push_back(new Item_return_int("Last_IO_Errno", 4, MYSQL_TYPE_LONG));
  field_list.push_back(new Item_empty_string("Last_IO_Error", 20));
  field_list.push_back(new Item_return_int("Last_SQL_Errno", 4, MYSQL_TYPE_LONG));
  field_list.push_back(new Item_empty_string("Last_SQL_Error", 20));
  field_list.push_back(new Item_empty_string("Replicate_Ignore_Server_Ids",
                                             FN_REFLEN));
  field_list.push_back(new Item_return_int("Master_Server_Id", sizeof(ulong),
                                           MYSQL_TYPE_LONG));
  field_list.push_back(new Item_empty_string("Master_UUID", UUID_LENGTH));
  field_list.push_back(new Item_empty_string("Master_Info_File",
                                             2 * FN_REFLEN));
  field_list.push_back(new Item_return_int("SQL_Delay", 10, MYSQL_TYPE_LONG));
  field_list.push_back(new Item_return_int("SQL_Remaining_Delay", 8, MYSQL_TYPE_LONG));
  field_list.push_back(new Item_empty_string("Slave_SQL_Running_State", 20));
  field_list.push_back(new Item_return_int("Master_Retry_Count", 10,
                                           MYSQL_TYPE_LONGLONG));
  field_list.push_back(new Item_empty_string("Master_Bind", mi != NULL ?
                                             sizeof(mi->bind_addr) : 0));
  field_list.push_back(new Item_empty_string("Last_IO_Error_Timestamp", 20));
  field_list.push_back(new Item_empty_string("Last_SQL_Error_Timestamp", 20));
  field_list.push_back(new Item_empty_string("Master_SSL_Crl", mi != NULL ?
                                             sizeof(mi->ssl_crl) : 0));
  field_list.push_back(new Item_empty_string("Master_SSL_Crlpath", mi != NULL ?
                                             sizeof(mi->ssl_crlpath) : 0));
  field_list.push_back(new Item_empty_string("Retrieved_Gtid_Set",
                                             io_gtid_set_size));
  field_list.push_back(new Item_empty_string("Executed_Gtid_Set",
                                             sql_gtid_set_size));
  field_list.push_back(new Item_return_int("Auto_Position", sizeof(ulong),
                                           MYSQL_TYPE_LONG));

  field_list.push_back(new Item_empty_string("Master_SSL_Actual_Cipher",
                                             mi != NULL ?
                                             sizeof(mi->ssl_actual_cipher) : 0));
  field_list.push_back(new Item_empty_string("Master_SSL_Subject",
                                             mi != NULL ?
                                             sizeof(mi->ssl_master_subject) : 0));
  field_list.push_back(new Item_empty_string("Master_SSL_Issuer",
                                             mi != NULL ?
                                             sizeof(mi->ssl_master_issuer) : 0));
  field_list.push_back(
      new Item_empty_string("Slave_Lag_Stats_Thread_Running", 3));

  if (protocol->send_result_set_metadata(&field_list,
                            Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
  {
    my_free(sql_gtid_set_buffer);
    my_free(io_gtid_set_buffer);
    DBUG_RETURN(true);
  }

  if (mi != NULL && mi->host[0])
  {
    DBUG_PRINT("info",("host is set: '%s'", mi->host));
    protocol->prepare_for_resend();

    /*
      slave_running can be accessed without run_lock but not other
      non-volotile members like mi->info_thd or rli->info_thd, for
      them either info_thd_lock or run_lock hold is required.
    */
    mysql_mutex_lock(&mi->info_thd_lock);
    protocol->store(mi->info_thd ? mi->info_thd->get_proc_info() : "", &my_charset_bin);
    mysql_mutex_unlock(&mi->info_thd_lock);

    mysql_mutex_lock(&mi->rli->info_thd_lock);
    slave_sql_running_state= const_cast<char *>(mi->rli->info_thd ? mi->rli->info_thd->get_proc_info() : "");
    mysql_mutex_unlock(&mi->rli->info_thd_lock);

    mysql_mutex_lock(&mi->data_lock);
    mysql_mutex_lock(&mi->rli->data_lock);
    mysql_mutex_lock(&mi->err_lock);
    mysql_mutex_lock(&mi->rli->err_lock);

    DEBUG_SYNC(thd, "wait_after_lock_active_mi_and_rli_data_lock_is_acquired");
    protocol->store(mi->host, &my_charset_bin);
    protocol->store(mi->get_user(), &my_charset_bin);
    protocol->store((uint32) mi->port);
    protocol->store((uint32) mi->connect_retry);
    protocol->store(mi->get_master_log_name(), &my_charset_bin);
    protocol->store((ulonglong) mi->get_master_log_pos());
    protocol->store(mi->rli->get_group_relay_log_name() +
                    dirname_length(mi->rli->get_group_relay_log_name()),
                    &my_charset_bin);
    protocol->store((ulonglong) mi->rli->get_group_relay_log_pos());
    protocol->store(mi->rli->get_group_master_log_name(), &my_charset_bin);
    protocol->store(mi->slave_running == MYSQL_SLAVE_RUN_CONNECT ?
                    "Yes" : (mi->slave_running == MYSQL_SLAVE_RUN_NOT_CONNECT ?
                             "Connecting" : "No"), &my_charset_bin);
    protocol->store(mi->rli->slave_running ? "Yes":"No", &my_charset_bin);
    protocol->store(rpl_filter->get_do_db());
    protocol->store(rpl_filter->get_ignore_db());

    char buf[256];
    String tmp(buf, sizeof(buf), &my_charset_bin);
    rpl_filter->get_do_table(&tmp);
    protocol->store(&tmp);
    rpl_filter->get_ignore_table(&tmp);
    protocol->store(&tmp);
    rpl_filter->get_wild_do_table(&tmp);
    protocol->store(&tmp);
    rpl_filter->get_wild_ignore_table(&tmp);
    protocol->store(&tmp);

    protocol->store(mi->rli->last_error().number);

    if (mi->rli->last_error().number == 0)
    {
      protocol->store("", &my_charset_bin);
    }
    else if (mi->rli->last_error().number >= EE_ERROR_FIRST &&
      mi->rli->last_error().number <= EE_ERROR_LAST)
    {
      protocol->store(EE_NAME(mi->rli->last_error().number), &my_charset_bin);
    }
    else
    {
      protocol->store("regular sql errno", &my_charset_bin);
    }

    protocol->store(mi->rli->last_error().message, &my_charset_bin);
    protocol->store((uint32) mi->rli->slave_skip_counter);
    protocol->store((ulonglong) mi->rli->get_group_master_log_pos());
    protocol->store((ulonglong) mi->rli->log_space_total);

    const char *until_type= "";

    switch (mi->rli->until_condition)
    {
    case Relay_log_info::UNTIL_NONE:
      until_type= "None";
      break;
    case Relay_log_info::UNTIL_MASTER_POS:
      until_type= "Master";
      break;
    case Relay_log_info::UNTIL_RELAY_POS:
      until_type= "Relay";
      break;
    case Relay_log_info::UNTIL_SQL_BEFORE_GTIDS:
      until_type= "SQL_BEFORE_GTIDS";
      break;
    case Relay_log_info::UNTIL_SQL_AFTER_GTIDS:
      until_type= "SQL_AFTER_GTIDS";
      break;
    case Relay_log_info::UNTIL_SQL_AFTER_MTS_GAPS:
      until_type= "SQL_AFTER_MTS_GAPS";
    case Relay_log_info::UNTIL_DONE:
      until_type= "DONE";
      break;
    default:
      DBUG_ASSERT(0);
    }
    protocol->store(until_type, &my_charset_bin);
    protocol->store(mi->rli->until_log_name, &my_charset_bin);
    protocol->store((ulonglong) mi->rli->until_log_pos);

#ifdef HAVE_OPENSSL
    protocol->store(mi->ssl? "Yes":"No", &my_charset_bin);
#else
    protocol->store(mi->ssl? "Ignored":"No", &my_charset_bin);
#endif
    protocol->store(mi->ssl_ca, &my_charset_bin);
    protocol->store(mi->ssl_capath, &my_charset_bin);
    protocol->store(mi->ssl_cert, &my_charset_bin);
    protocol->store(mi->ssl_cipher, &my_charset_bin);
    protocol->store(mi->ssl_key, &my_charset_bin);

    std::pair<longlong, longlong> time_lag_behind_master = get_time_lag_behind_master(mi);

    // store Seconds_Behind_Master
    if (time_lag_behind_master.first == -1) {
      protocol->store_null();
    } else {
      protocol->store(time_lag_behind_master.first);
    }

    // store Lag_Peak_Over_Last_Period
    if (mi->rli->slave_running) {
      time_t now = time(0);
      protocol->store((longlong)(max(0L, mi->rli->peak_lag(now))));
    } else {
      protocol->store_null();
    }

    // store Milli_Seconds_Behind_Master
    if (opt_binlog_trx_meta_data) {
      if (time_lag_behind_master.second == -1) {
        protocol->store_null();
      } else {
        protocol->store(time_lag_behind_master.second);
      }
    }

    protocol->store(mi->ssl_verify_server_cert? "Yes":"No", &my_charset_bin);

    // Last_IO_Errno
    protocol->store(mi->last_error().number);
    // Last_IO_Error
    protocol->store(mi->last_error().message, &my_charset_bin);
    // Last_SQL_Errno
    protocol->store(mi->rli->last_error().number);
    // Last_SQL_Error
    protocol->store(mi->rli->last_error().message, &my_charset_bin);
    // Replicate_Ignore_Server_Ids
    {
      char buff[FN_REFLEN];
      ulong i, cur_len;
      for (i= 0, buff[0]= 0, cur_len= 0;
           i < mi->ignore_server_ids->dynamic_ids.elements; i++)
      {
        ulong s_id, slen;
        char sbuff[FN_REFLEN];
        get_dynamic(&(mi->ignore_server_ids->dynamic_ids), (uchar*) &s_id, i);
        slen= sprintf(sbuff, (i == 0 ? "%lu" : ", %lu"), s_id);
        if (cur_len + slen + 4 > FN_REFLEN)
        {
          /*
            break the loop whenever remained space could not fit
            ellipses on the next cycle
          */
          sprintf(buff + cur_len, "...");
          break;
        }
        cur_len += sprintf(buff + cur_len, "%s", sbuff);
      }
      protocol->store(buff, &my_charset_bin);
    }
    // Master_Server_id
    protocol->store((uint32) mi->master_id);
    protocol->store(mi->master_uuid, &my_charset_bin);
    // Master_Info_File
    protocol->store(mi->get_description_info(), &my_charset_bin);
    // SQL_Delay
    protocol->store((uint32) mi->rli->get_sql_delay());
    // SQL_Remaining_Delay
    if (slave_sql_running_state == stage_sql_thd_waiting_until_delay.m_name)
    {
      time_t t= my_time(0), sql_delay_end= mi->rli->get_sql_delay_end();
      protocol->store((uint32)(t < sql_delay_end ? sql_delay_end - t : 0));
    }
    else
      protocol->store_null();
    // Slave_SQL_Running_State
    protocol->store(slave_sql_running_state, &my_charset_bin);
    // Master_Retry_Count
    protocol->store((ulonglong) mi->retry_count);
    // Master_Bind
    protocol->store(mi->bind_addr, &my_charset_bin);
    // Last_IO_Error_Timestamp
    protocol->store(mi->last_error().timestamp, &my_charset_bin);
    // Last_SQL_Error_Timestamp
    protocol->store(mi->rli->last_error().timestamp, &my_charset_bin);
    // Master_Ssl_Crl
    protocol->store(mi->ssl_crl, &my_charset_bin);
    // Master_Ssl_Crlpath
    protocol->store(mi->ssl_crlpath, &my_charset_bin);
    // Retrieved_Gtid_Set
    protocol->store(io_gtid_set_buffer, &my_charset_bin);
    // Executed_Gtid_Set
    protocol->store(sql_gtid_set_buffer, &my_charset_bin);
    // Auto_Position
    protocol->store(mi->is_auto_position() ? 1 : 0);
    // ssl xxx
    protocol->store(mi->ssl_actual_cipher, &my_charset_bin);
    protocol->store(mi->ssl_master_issuer, &my_charset_bin);
    protocol->store(mi->ssl_master_subject, &my_charset_bin);
    // slave lag stats daemon running status
    protocol->store(slave_stats_daemon_thread_counter > 0 ? "Yes" : "No",
                    &my_charset_bin);
    protocol->update_checksum();

    mysql_mutex_unlock(&mi->rli->err_lock);
    mysql_mutex_unlock(&mi->err_lock);
    mysql_mutex_unlock(&mi->rli->data_lock);
    mysql_mutex_unlock(&mi->data_lock);

    if (protocol->write())
    {
      my_free(sql_gtid_set_buffer);
      my_free(io_gtid_set_buffer);
      DBUG_RETURN(true);
    }
  }
  my_eof(thd);
  my_free(sql_gtid_set_buffer);
  my_free(io_gtid_set_buffer);
  DBUG_RETURN(false);
}


void set_slave_thread_options(THD* thd)
{
  DBUG_ENTER("set_slave_thread_options");
  /*
     It's nonsense to constrain the slave threads with max_join_size; if a
     query succeeded on master, we HAVE to execute it. So set
     OPTION_BIG_SELECTS. Setting max_join_size to HA_POS_ERROR is not enough
     (and it's not needed if we have OPTION_BIG_SELECTS) because an INSERT
     SELECT examining more than 4 billion rows would still fail (yes, because
     when max_join_size is 4G, OPTION_BIG_SELECTS is automatically set, but
     only for client threads.
  */
  ulonglong options= thd->variables.option_bits | OPTION_BIG_SELECTS;
  if (opt_log_slave_updates)
    options|= OPTION_BIN_LOG;
  else
    options&= ~OPTION_BIN_LOG;
  thd->variables.option_bits= options;
  thd->variables.completion_type= 0;

  /*
    Set autocommit= 1 when info tables are used and autocommit == 0 to
    avoid trigger asserts on mysql_execute_command(THD *thd) caused by
    info tables updates which do not commit, like Rotate, Stop and
    skipped events handling.
  */
  if (is_autocommit_off_and_infotables(thd))
  {
    thd->variables.option_bits|= OPTION_AUTOCOMMIT;
    thd->variables.option_bits&= ~OPTION_NOT_AUTOCOMMIT;
    thd->server_status|= SERVER_STATUS_AUTOCOMMIT;
  }

  DBUG_VOID_RETURN;
}

void set_slave_thread_default_charset(THD* thd, Relay_log_info const *rli)
{
  DBUG_ENTER("set_slave_thread_default_charset");

  thd->variables.character_set_client=
    global_system_variables.character_set_client;
  thd->variables.collation_connection=
    global_system_variables.collation_connection;
  thd->variables.collation_server=
    global_system_variables.collation_server;
  thd->update_charset();

  /*
    We use a const cast here since the conceptual (and externally
    visible) behavior of the function is to set the default charset of
    the thread.  That the cache has to be invalidated is a secondary
    effect.
   */
  const_cast<Relay_log_info*>(rli)->cached_charset_invalidate();
  DBUG_VOID_RETURN;
}

/*
  init_slave_thread()
*/

static int init_slave_thread(THD* thd, SLAVE_THD_TYPE thd_type)
{
  DBUG_ENTER("init_slave_thread");
#if !defined(DBUG_OFF)
  int simulate_error= 0;
#endif
  thd->system_thread= (thd_type == SLAVE_THD_WORKER) ?
    SYSTEM_THREAD_SLAVE_WORKER : (thd_type == SLAVE_THD_SQL) ?
    SYSTEM_THREAD_SLAVE_SQL : SYSTEM_THREAD_SLAVE_IO;
  thd->security_ctx->skip_grants();
  my_net_init(thd->get_net(), 0);
  thd->slave_thread = 1;
  thd->enable_slow_log= opt_log_slow_slave_statements;
  set_slave_thread_options(thd);
  thd->client_capabilities = CLIENT_LOCAL_FILES;

  thd->variables.pseudo_thread_id= thd->set_new_thread_id();

  DBUG_EXECUTE_IF("simulate_io_slave_error_on_init",
                  simulate_error|= (1 << SLAVE_THD_IO););
  DBUG_EXECUTE_IF("simulate_sql_slave_error_on_init",
                  simulate_error|= (1 << SLAVE_THD_SQL););
#if !defined(DBUG_OFF)
  if (init_thr_lock() || thd->store_globals() || simulate_error & (1<< thd_type))
#else
  if (init_thr_lock() || thd->store_globals())
#endif
  {
    DBUG_RETURN(-1);
  }

  if (thd_type == SLAVE_THD_SQL)
  {
    THD_STAGE_INFO(thd, stage_waiting_for_the_next_event_in_relay_log);
  }
  else
  {
    THD_STAGE_INFO(thd, stage_waiting_for_master_update);
  }
  thd->set_time();
  /* Do not use user-supplied timeout value for system threads. */
  thd->variables.lock_wait_timeout_nsec= LONG_TIMEOUT_NSEC;
  DBUG_RETURN(0);
}


/**
  Sleep for a given amount of time or until killed.

  @param thd        Thread context of the current thread.
  @param seconds    The number of seconds to sleep.
  @param func       Function object to check if the thread has been killed.
  @param info       The Rpl_info object associated with this sleep.

  @retval True if the thread has been killed, false otherwise.
*/
template <typename killed_func, typename rpl_info>
static inline bool slave_sleep(THD *thd, time_t seconds,
                               killed_func func, rpl_info info)
{
  bool ret;
  struct timespec abstime;
  mysql_mutex_t *lock= &info->sleep_lock;
  mysql_cond_t *cond= &info->sleep_cond;

  /* Absolute system time at which the sleep time expires. */
  set_timespec(abstime, seconds);

  mysql_mutex_lock(lock);
  thd->ENTER_COND(cond, lock, NULL, NULL);

  while (! (ret= func(thd, info)))
  {
    int error= mysql_cond_timedwait(cond, lock, &abstime);
    if (error == ETIMEDOUT || error == ETIME)
      break;
  }

  /* Implicitly unlocks the mutex. */
  thd->EXIT_COND(NULL);

  return ret;
}

static int request_dump(THD *thd, MYSQL* mysql, Master_info* mi,
                        bool *suppress_warnings)
{
  DBUG_ENTER("request_dump");

  const int BINLOG_NAME_INFO_SIZE= strlen(mi->get_master_log_name());
  int error= 1;
  size_t command_size= 0;
  enum_server_command command= mi->is_auto_position() ?
    COM_BINLOG_DUMP_GTID : COM_BINLOG_DUMP;
  uchar* command_buffer= NULL;
  ushort binlog_flags= 0;

  if (RUN_HOOK(binlog_relay_io,
               before_request_transmit,
               (thd, mi, binlog_flags)))
    goto err;

  *suppress_warnings= false;
  if (command == COM_BINLOG_DUMP_GTID)
  {
    // get set of GTIDs
    Sid_map sid_map(NULL/*no lock needed*/);
    Gtid_set gtid_executed(&sid_map);
    global_sid_lock->wrlock();
    gtid_state->dbug_print();

    /*
      We are unsure whether I/O thread retrieved the last gtid transaction
      completely or not (before it is going down because of a crash/normal
      shutdown/normal stop slave io_thread). It is possible that I/O thread
      would have retrieved and written only partial transaction events. So We
      request Master to send the last gtid event once again. We do this by
      removing the last I/O thread retrieved gtid event from
      "Retrieved_gtid_set".  Possible cases: 1) I/O thread would have
      retrieved full transaction already in the first time itself, but
      retrieving them again will not cause problem because GTID number is
      same, Hence SQL thread will not commit it again. 2) I/O thread would
      have retrieved full transaction already and SQL thread would have
      already executed it. In that case, We are not going remove last
      retrieved gtid from "Retrieved_gtid_set" otherwise we will see gaps in
      "Retrieved set". The same case is handled in the below code.  Please
      note there will be paritial transactions written in relay log but they
      will not cause any problem incase of transactional tables.  But incase
      of non-transaction tables, partial trx will create inconsistency
      between master and slave.  In that case, users need to check manually.
    */

    Gtid_set * retrieved_set= (const_cast<Gtid_set *>(mi->rli->get_gtid_set()));
    Gtid *last_retrieved_gtid= mi->rli->get_last_retrieved_gtid();

    /*
      Remove last_retrieved_gtid only if it is not part of
      executed_gtid_set
    */
    if (!last_retrieved_gtid->empty() &&
        !gtid_state->get_logged_gtids()->contains_gtid(*last_retrieved_gtid))
    {
      if (retrieved_set->_remove_gtid(*last_retrieved_gtid) != RETURN_STATUS_OK)
      {
        global_sid_lock->unlock();
        goto err;
      }
    }

    if (gtid_executed.add_gtid_set(mi->rli->get_gtid_set()) != RETURN_STATUS_OK ||
        gtid_executed.add_gtid_set(gtid_state->get_logged_gtids()) !=
        RETURN_STATUS_OK)
    {
      global_sid_lock->unlock();
      goto err;
    }
    global_sid_lock->unlock();

    // allocate buffer
    size_t encoded_data_size= gtid_executed.get_encoded_length();
    size_t allocation_size=
      ::BINLOG_FLAGS_INFO_SIZE + ::BINLOG_SERVER_ID_INFO_SIZE +
      ::BINLOG_NAME_SIZE_INFO_SIZE + BINLOG_NAME_INFO_SIZE +
      ::BINLOG_POS_INFO_SIZE + ::BINLOG_DATA_SIZE_INFO_SIZE +
      encoded_data_size + 1;
    if (!(command_buffer= (uchar *) my_malloc(allocation_size, MYF(MY_WME))))
      goto err;
    uchar* ptr_buffer= command_buffer;

    DBUG_PRINT("info", ("Do I know something about the master? (binary log's name %s - auto position %d).",
               mi->get_master_log_name(), mi->is_auto_position()));
    /*
      Note: binlog_flags is always 0.  However, in versions up to 5.6
      RC, the master would check the lowest bit and do something
      unexpected if it was set; in early versions of 5.6 it would also
      use the two next bits.  Therefore, for backward compatibility,
      if we ever start to use the flags, we should leave the three
      lowest bits unused.
    */
    int2store(ptr_buffer, binlog_flags);
    ptr_buffer+= ::BINLOG_FLAGS_INFO_SIZE;
    int4store(ptr_buffer, server_id);
    ptr_buffer+= ::BINLOG_SERVER_ID_INFO_SIZE;
    int4store(ptr_buffer, BINLOG_NAME_INFO_SIZE);
    ptr_buffer+= ::BINLOG_NAME_SIZE_INFO_SIZE;
    memset(ptr_buffer, 0, BINLOG_NAME_INFO_SIZE);
    ptr_buffer+= BINLOG_NAME_INFO_SIZE;
    int8store(ptr_buffer, 4LL);
    ptr_buffer+= ::BINLOG_POS_INFO_SIZE;

    int4store(ptr_buffer, encoded_data_size);
    ptr_buffer+= ::BINLOG_DATA_SIZE_INFO_SIZE;
    gtid_executed.encode(ptr_buffer);
    ptr_buffer+= encoded_data_size;

    command_size= ptr_buffer - command_buffer;
    DBUG_ASSERT(command_size == (allocation_size - 1));
  }
  else
  {
    size_t allocation_size= ::BINLOG_POS_OLD_INFO_SIZE +
      BINLOG_NAME_INFO_SIZE + ::BINLOG_FLAGS_INFO_SIZE +
      ::BINLOG_SERVER_ID_INFO_SIZE + 1;
    if (!(command_buffer= (uchar *) my_malloc(allocation_size, MYF(MY_WME))))
      goto err;
    uchar* ptr_buffer= command_buffer;

    int4store(ptr_buffer, mi->get_master_log_pos());
    ptr_buffer+= ::BINLOG_POS_OLD_INFO_SIZE;
    // See comment regarding binlog_flags above.
    int2store(ptr_buffer, binlog_flags);
    ptr_buffer+= ::BINLOG_FLAGS_INFO_SIZE;
    int4store(ptr_buffer, server_id);
    ptr_buffer+= ::BINLOG_SERVER_ID_INFO_SIZE;
    memcpy(ptr_buffer, mi->get_master_log_name(), BINLOG_NAME_INFO_SIZE);
    ptr_buffer+= BINLOG_NAME_INFO_SIZE;

    command_size= ptr_buffer - command_buffer;
    DBUG_ASSERT(command_size == (allocation_size - 1));
  }

  if (simple_command(mysql, command, command_buffer, command_size, 1))
  {
    /*
      Something went wrong, so we will just reconnect and retry later
      in the future, we should do a better error analysis, but for
      now we just fill up the error log :-)
    */
    if (mysql_errno(mysql) == ER_NET_READ_INTERRUPTED)
      *suppress_warnings= true;                 // Suppress reconnect warning
    else
      sql_print_error("Error on %s: %d  %s, will retry in %d secs",
                      command_name[command].str,
                      mysql_errno(mysql), mysql_error(mysql),
                      mi->connect_retry);
    goto err;
  }
  error= 0;

err:
  my_free(command_buffer);
  DBUG_RETURN(error);
}


/*
  Read one event from the master

  SYNOPSIS
    read_event()
    mysql               MySQL connection
    mi                  Master connection information
    suppress_warnings   TRUE when a normal net read timeout has caused us to
                        try a reconnect.  We do not want to print anything to
                        the error log in this case because this a anormal
                        event in an idle server.

    RETURN VALUES
    'packet_error'      Error
    number              Length of packet
*/

static ulong read_event(MYSQL* mysql, Master_info *mi, bool* suppress_warnings)
{
  ulong len= 0;
  DBUG_ENTER("read_event");

  *suppress_warnings= FALSE;
  /*
    my_real_read() will time us out
    We check if we were told to die, and if not, try reading again
  */
#ifndef DBUG_OFF
  if (disconnect_slave_event_count && !(mi->events_until_exit--))
    DBUG_RETURN(packet_error);
#endif

  len= cli_safe_read(mysql, NULL);

#ifdef HAVE_COMPRESS
  // case: event was compressed before sending, so we have to uncompress
  if (mysql->net.compress_event && len != 0 && len != packet_error &&
      mysql->net.read_pos[0] == COMP_EVENT_MAGIC_NUMBER)
    len= uncompress_event(&mysql->net, len);
#endif

  if (len == packet_error || (long) len < 1)
  {
    if (mysql_errno(mysql) == ER_NET_READ_INTERRUPTED)
    {
      /*
        We are trying a normal reconnect after a read timeout;
        we suppress prints to .err file as long as the reconnect
        happens without problems
      */
      *suppress_warnings= TRUE;
    }
    else
    {
      if (!mi->abort_slave)
      {
        sql_print_error("Error reading packet from server: %s (server_errno=%d)",
                        mysql_error(mysql), mysql_errno(mysql));
      }
    }
    DBUG_RETURN(packet_error);
  }

  /* Check if eof packet */
  if (len < 8 && mysql->net.read_pos[0] == 254)
  {
     sql_print_information("Slave: received end packet from server due to dump "
                           "thread being killed on master. Dump threads are "
                           "killed for example during master shutdown, "
                           "explicitly by a user, or when the master receives "
                           "a binlog send request from a duplicate server "
                           "UUID <%s> : Error %s", ::server_uuid,
                           mysql_error(mysql));
     DBUG_RETURN(packet_error);
  }

  DBUG_PRINT("exit", ("len: %lu  net->read_pos[4]: %d",
                      len, mysql->net.read_pos[4]));
  DBUG_RETURN(len - 1);
}


/**
  If this is a lagging slave (specified with CHANGE MASTER TO MASTER_DELAY = X), delays accordingly. Also unlocks rli->data_lock.

  Design note: this is the place to unlock rli->data_lock. The lock
  must be held when reading delay info from rli, but it should not be
  held while sleeping.

  @param ev Event that is about to be executed.

  @param thd The sql thread's THD object.

  @param rli The sql thread's Relay_log_info structure.

  @retval 0 If the delay timed out and the event shall be executed.

  @retval nonzero If the delay was interrupted and the event shall be skipped.
*/
static int sql_delay_event(Log_event *ev, THD *thd, Relay_log_info *rli)
{
  long sql_delay= rli->get_sql_delay();

  DBUG_ENTER("sql_delay_event");
  mysql_mutex_assert_owner(&rli->data_lock);
  DBUG_ASSERT(!rli->belongs_to_client());

  int type= ev->get_type_code();
  if (sql_delay && type != ROTATE_EVENT &&
      type != FORMAT_DESCRIPTION_EVENT && type != START_EVENT_V3)
  {
    // The time when we should execute the event.
    time_t sql_delay_end=
      ev->when.tv_sec + rli->mi->clock_diff_with_master + sql_delay;
    // The current time.
    time_t now= my_time(0);
    // The time we will have to sleep before executing the event.
    unsigned long nap_time= 0;
    if (sql_delay_end > now)
      nap_time= sql_delay_end - now;

    DBUG_PRINT("info", ("sql_delay= %lu "
                        "ev->when= %lu "
                        "rli->mi->clock_diff_with_master= %lu "
                        "now= %ld "
                        "sql_delay_end= %ld "
                        "nap_time= %ld",
                        sql_delay, (long) ev->when.tv_sec,
                        rli->mi->clock_diff_with_master,
                        (long)now, (long)sql_delay_end, (long)nap_time));

    if (sql_delay_end > now)
    {
      DBUG_PRINT("info", ("delaying replication event %lu secs",
                          nap_time));
      rli->start_sql_delay(sql_delay_end);
      mysql_mutex_unlock(&rli->data_lock);
      DBUG_RETURN(slave_sleep(thd, nap_time, sql_slave_killed, rli));
    }
  }

  mysql_mutex_unlock(&rli->data_lock);

  DBUG_RETURN(0);
}

/**
   a sort_dynamic function on ulong type
   returns as specified by @c qsort_cmp
*/
int ulong_cmp(ulong *id1, ulong *id2)
{
  return *id1 < *id2? -1 : (*id1 > *id2? 1 : 0);
}

/**
  Applies the given event and advances the relay log position.

  This is needed by the sql thread to execute events from the binlog,
  and by clients executing BINLOG statements.  Conceptually, this
  function does:

  @code
    ev->apply_event(rli);
    ev->update_pos(rli);
  @endcode

  It also does the following maintainance:

   - Initializes the thread's server_id and time; and the event's
     thread.

   - If !rli->belongs_to_client() (i.e., if it belongs to the slave
     sql thread instead of being used for executing BINLOG
     statements), it does the following things: (1) skips events if it
     is needed according to the server id or slave_skip_counter; (2)
     unlocks rli->data_lock; (3) sleeps if required by 'CHANGE MASTER
     TO MASTER_DELAY=X'; (4) maintains the running state of the sql
     thread (rli->thread_state).

   - Reports errors as needed.

  @param ptr_ev a pointer to a reference to the event to apply.

  @param thd The client thread that executes the event (i.e., the
  slave sql thread if called from a replication slave, or the client
  thread if called to execute a BINLOG statement).

  @param rli The relay log info (i.e., the slave's rli if called from
  a replication slave, or the client's thd->rli_fake if called to
  execute a BINLOG statement).

  @note MTS can store NULL to @c ptr_ev location to indicate
        the event is taken over by a Worker.

  @retval SLAVE_APPLY_EVENT_AND_UPDATE_POS_OK
          OK.

  @retval SLAVE_APPLY_EVENT_AND_UPDATE_POS_APPLY_ERROR
          Error calling ev->apply_event().

  @retval SLAVE_APPLY_EVENT_AND_UPDATE_POS_UPDATE_POS_ERROR
          No error calling ev->apply_event(), but error calling
          ev->update_pos().

  @retval SLAVE_APPLY_EVENT_AND_UPDATE_POS_APPEND_JOB_ERROR
          append_item_to_jobs() failed, thread was killed while waiting
          for successful enqueue on worker.
*/
enum enum_slave_apply_event_and_update_pos_retval
apply_event_and_update_pos(Log_event** ptr_ev, THD* thd, Relay_log_info* rli)
{
  int exec_res= 0;
  bool skip_event= FALSE;
  Log_event *ev= *ptr_ev;
  Log_event::enum_skip_reason reason= Log_event::EVENT_SKIP_NOT;

  DBUG_ENTER("apply_event_and_update_pos");

  DBUG_PRINT("exec_event",("%s(type_code: %d; server_id: %d)",
                           ev->get_type_str(), ev->get_type_code(),
                           ev->server_id));
  DBUG_PRINT("info", ("thd->options: %s%s; rli->last_event_start_time: %lu",
                      FLAGSTR(thd->variables.option_bits, OPTION_NOT_AUTOCOMMIT),
                      FLAGSTR(thd->variables.option_bits, OPTION_BEGIN),
                      (ulong) rli->last_event_start_time));

  /*
    Execute the event to change the database and update the binary
    log coordinates, but first we set some data that is needed for
    the thread.

    The event will be executed unless it is supposed to be skipped.

    Queries originating from this server must be skipped.  Low-level
    events (Format_description_log_event, Rotate_log_event,
    Stop_log_event) from this server must also be skipped. But for
    those we don't want to modify 'group_master_log_pos', because
    these events did not exist on the master.
    Format_description_log_event is not completely skipped.

    Skip queries specified by the user in 'slave_skip_counter'.  We
    can't however skip events that has something to do with the log
    files themselves.

    Filtering on own server id is extremely important, to ignore
    execution of events created by the creation/rotation of the relay
    log (remember that now the relay log starts with its Format_desc,
    has a Rotate etc).
  */
  /*
     Set the unmasked and actual server ids from the event
   */
  thd->server_id = ev->server_id; // use the original server id for logging
  thd->unmasked_server_id = ev->unmasked_server_id;
  thd->set_time();                            // time the query
  thd->lex->current_select= 0;
  if (!ev->when.tv_sec)
    my_micro_time_to_timeval(my_micro_time(), &ev->when);
  ev->thd = thd; // because up to this point, ev->thd == 0

  if (!(rli->is_mts_recovery() && bitmap_is_set(&rli->recovery_groups,
                                                rli->mts_recovery_index)))
  {
    reason= ev->shall_skip(rli);
  }
#ifndef DBUG_OFF
  if (rli->is_mts_recovery())
  {
    DBUG_PRINT("mts", ("Mts is recovering %d, number of bits set %d, "
                       "bitmap is set %d, index %lu.\n",
                       rli->is_mts_recovery(),
                       bitmap_bits_set(&rli->recovery_groups),
                       bitmap_is_set(&rli->recovery_groups,
                                     rli->mts_recovery_index),
                       rli->mts_recovery_index));
  }
#endif
  if (reason == Log_event::EVENT_SKIP_COUNT)
  {
    sql_slave_skip_counter= --rli->slave_skip_counter;
    skip_event= TRUE;
  }
  if (reason == Log_event::EVENT_SKIP_NOT)
  {
    my_io_perf_t start_perf_read, start_perf_read_blob,
                 start_perf_read_primary, start_perf_read_secondary;
    ulonglong init_timer;

    /* Initialize for user_statistics, see dispatch_command */
    thd->reset_user_stats_counters();
    start_perf_read = thd->io_perf_read;
    start_perf_read_blob = thd->io_perf_read_blob;
    start_perf_read_primary = thd->io_perf_read_primary;
    start_perf_read_secondary = thd->io_perf_read_secondary;

    // Sleeps if needed, and unlocks rli->data_lock.
    if (sql_delay_event(ev, thd, rli))
      DBUG_RETURN(SLAVE_APPLY_EVENT_AND_UPDATE_POS_OK);

    init_timer = my_timer_now();

    exec_res= ev->apply_event(rli);

    if (!exec_res && (ev->worker != rli))
    {
      if (rli->mts_dependency_replication)
      {
        DBUG_ASSERT(ev->worker == NULL);
        if (!ev->schedule_dep(rli))
        {
          *ptr_ev= nullptr;
          DBUG_RETURN(SLAVE_APPLY_EVENT_AND_UPDATE_POS_APPLY_ERROR);
        }
      }
      else if (ev->worker)
      {
        Slave_job_item item= {ev}, *job_item= &item;
        Slave_worker *w= (Slave_worker *) ev->worker;
        // specially marked group typically with OVER_MAX_DBS_IN_EVENT_MTS db:s
        bool need_sync= ev->is_mts_group_isolated();

        // all events except BEGIN-query must be marked with a non-NULL Worker
        DBUG_ASSERT(((Slave_worker*) ev->worker) == rli->last_assigned_worker);

        DBUG_PRINT("Log_event::apply_event:",
                   ("-> job item data %p to W_%lu", job_item->data, w->id));

        // Reset mts in-group state
        if (rli->mts_group_status == Relay_log_info::MTS_END_GROUP)
        {
          // CGAP cleanup
          for (uint i= rli->curr_group_assigned_parts.elements; i > 0; i--)
            delete_dynamic_element(&rli->
                                   curr_group_assigned_parts, i - 1);
          // reset the B-group and Gtid-group marker
          rli->curr_group_seen_begin= rli->curr_group_seen_gtid= false;
          rli->curr_group_seen_metadata= false;
          rli->last_assigned_worker= NULL;
        }
        /*
           Stroring GAQ index of the group that the event belongs to
           in the event. Deferred events are handled similarly below.
        */
        ev->mts_group_idx= rli->gaq->assigned_group_index;

        bool append_item_to_jobs_error= false;
        if (rli->curr_group_da.elements > 0)
        {
          /*
            the current event sorted out which partion the current group
            belongs to. It's time now to processed deferred array events.
          */
          for (uint i= 0; i < rli->curr_group_da.elements; i++)
          {
            Slave_job_item da_item;
            get_dynamic(&rli->curr_group_da, (uchar*) &da_item.data, i);
            DBUG_PRINT("mts", ("Assigning job %llu to worker %lu",
                      ((Log_event* )da_item.data)->log_pos, w->id));
            static_cast<Log_event*>(da_item.data)->mts_group_idx=
              rli->gaq->assigned_group_index; // similarly to above
            if (!append_item_to_jobs_error)
              append_item_to_jobs_error= append_item_to_jobs(&da_item, w, rli);
            if (append_item_to_jobs_error)
              delete static_cast<Log_event*>(da_item.data);
          }
          if (rli->curr_group_da.elements > rli->curr_group_da.max_element)
          {
            // reallocate to less mem
            rli->curr_group_da.elements= rli->curr_group_da.max_element;
            rli->curr_group_da.max_element= 0;
            freeze_size(&rli->curr_group_da); // restores max_element
          }
          rli->curr_group_da.elements= 0;
        }
        if (append_item_to_jobs_error)
          DBUG_RETURN(SLAVE_APPLY_EVENT_AND_UPDATE_POS_APPEND_JOB_ERROR);

        DBUG_PRINT("mts", ("Assigning job %llu to worker %lu\n",
                   ((Log_event* )job_item->data)->log_pos, w->id));

        /* Notice `ev' instance can be destoyed after `append()' */
        if (append_item_to_jobs(job_item, w, rli))
          DBUG_RETURN(SLAVE_APPLY_EVENT_AND_UPDATE_POS_APPEND_JOB_ERROR);
        if (need_sync)
        {
          /*
            combination of over-max db:s and end of the current group
            forces to wait for the assigned groups completion by assigned
            to the event worker.
            Indeed MTS group status could be safely set to MTS_NOT_IN_GROUP
            after wait_() returns.
            No need to know a possible error out of synchronization call.
          */
          (void) wait_for_workers_to_finish(rli);
        }

      }
      *ptr_ev= NULL; // announcing the event is passed to w-worker

      if (log_warnings > 1 &&
          rli->is_parallel_exec() && rli->mts_events_assigned % 1024 == 1)
      {
        time_t my_now= my_time(0);

        if ((my_now - rli->mts_last_online_stat) >=
            mts_online_stat_period)
        {
          sql_print_information("Multi-threaded slave statistics: "
                                "seconds elapsed = %lu; "
                                "events assigned = %llu; "
                                "worker queues filled over overrun level = %lu; "
                                "waited due a Worker queue full = %lu; "
                                "waited due the total size = %lu; "
                                "slept when Workers occupied = %lu ",
                                static_cast<unsigned long>
                                (my_now - rli->mts_last_online_stat),
                                rli->mts_events_assigned,
                                rli->mts_wq_overrun_cnt,
                                rli->mts_wq_overfill_cnt,
                                rli->wq_size_waits_cnt,
                                rli->mts_wq_no_underrun_cnt);
          rli->mts_last_online_stat= my_now;
        }
      }
    }
    else {
      ulonglong wall_time = my_timer_since(init_timer);
      /* Update counters for USER_STATS */
      bool is_other= FALSE;
      bool is_xid= FALSE;
      bool update_slave_stats= FALSE;
      if (ev->get_type_code() < ENUM_END_EVENT)
      {
        repl_event_counts[ev->get_type_code()] += 1;
        repl_event_times[ev->get_type_code()] += wall_time;
      }
      else
      {
        repl_event_count_other += 1;
        repl_event_time_other += wall_time;
      }
      /* TODO: handle WRITE_ROWS_EVENT, UPDATE_ROWS_EVENT, DELETE_ROWS_EVENT */
      switch (ev->get_type_code())
      {
        case XID_EVENT:
          is_xid= TRUE;
          update_slave_stats= TRUE;
          break;
        case QUERY_EVENT:
          update_slave_stats= TRUE;
          break;
        default:
          break;
      }

      if (update_slave_stats)
      {
        if (exec_res == 0)
        {
          rli->update_peak_lag(ev->when.tv_sec);
        }
        USER_STATS *us= thd_get_user_stats(thd);
        update_user_stats_after_statement(us, thd, wall_time, is_other, is_xid,
                                          &start_perf_read,
                                          &start_perf_read_blob,
                                          &start_perf_read_primary,
                                          &start_perf_read_secondary);
      }
    }
  }
  else
    mysql_mutex_unlock(&rli->data_lock);

  DBUG_PRINT("info", ("apply_event error = %d", exec_res));
  if (exec_res == 0)
  {
    /*
      Positions are not updated here when an XID is processed. To make
      a slave crash-safe, positions must be updated while processing a
      XID event and as such do not need to be updated here again.

      However, if the event needs to be skipped, this means that it
      will not be processed and then positions need to be updated here.

      See sql/rpl_rli.h for further details.
    */
    int error= 0;
    if (*ptr_ev &&
        (ev->get_type_code() != XID_EVENT ||
         skip_event || (rli->is_mts_recovery() && !is_gtid_event(ev) &&
         (ev->ends_group() || !rli->mts_recovery_group_seen_begin) &&
          bitmap_is_set(&rli->recovery_groups, rli->mts_recovery_index))))
    {
#ifndef DBUG_OFF
      /*
        This only prints information to the debug trace.

        TODO: Print an informational message to the error log?
      */
      static const char *const explain[] = {
        // EVENT_SKIP_NOT,
        "not skipped",
        // EVENT_SKIP_IGNORE,
        "skipped because event should be ignored",
        // EVENT_SKIP_COUNT
        "skipped because event skip counter was non-zero"
      };
      DBUG_PRINT("info", ("OPTION_BEGIN: %d; IN_STMT: %d",
                          MY_TEST(thd->variables.option_bits & OPTION_BEGIN),
                          rli->get_flag(Relay_log_info::IN_STMT)));
      DBUG_PRINT("skip_event", ("%s event was %s",
                                ev->get_type_str(), explain[reason]));
#endif

      error= ev->update_pos(rli);

#ifndef DBUG_OFF
      DBUG_PRINT("info", ("update_pos error = %d", error));
      if (!rli->belongs_to_client())
      {
        char buf[22];
        DBUG_PRINT("info", ("group %s %s",
                            llstr(rli->get_group_relay_log_pos(), buf),
                            rli->get_group_relay_log_name()));
        DBUG_PRINT("info", ("event %s %s",
                            llstr(rli->get_event_relay_log_pos(), buf),
                            rli->get_event_relay_log_name()));
      }
#endif
    }
    else
    {
      DBUG_ASSERT(*ptr_ev == ev || rli->is_parallel_exec() ||
		  (!ev->worker &&
		   (ev->get_type_code() == INTVAR_EVENT ||
		    ev->get_type_code() == RAND_EVENT ||
		    ev->get_type_code() == USER_VAR_EVENT)));

      rli->inc_event_relay_log_pos();
    }

    if (!error && rli->is_mts_recovery() &&
        ev->get_type_code() != ROTATE_EVENT &&
        ev->get_type_code() != FORMAT_DESCRIPTION_EVENT &&
        ev->get_type_code() != PREVIOUS_GTIDS_LOG_EVENT)
    {
      if (ev->starts_group())
      {
        rli->mts_recovery_group_seen_begin= true;
      }
      else if ((ev->ends_group() || !rli->mts_recovery_group_seen_begin) &&
               !is_gtid_event(ev))
      {
        rli->mts_recovery_index++;
        if (--rli->mts_recovery_group_cnt == 0)
        {
          rli->mts_recovery_index= 0;
          sql_print_information("Slave: MTS Recovery has completed at "
                                "relay log %s, position %llu "
                                "master log %s, position %llu.",
                                rli->get_group_relay_log_name(),
                                rli->get_group_relay_log_pos(),
                                rli->get_group_master_log_name(),
                                rli->get_group_master_log_pos());
          /*
             Few tests wait for UNTIL_SQL_AFTER_MTS_GAPS completion.
             Due to exisiting convention the status won't change
             prior to slave restarts.
             So making of UNTIL_SQL_AFTER_MTS_GAPS completion isdone here,
             and only in the debug build to make the test to catch the change
             despite a faulty design of UNTIL checking before execution.
          */
          if (rli->until_condition == Relay_log_info::UNTIL_SQL_AFTER_MTS_GAPS)
          {
            rli->until_condition= Relay_log_info::UNTIL_DONE;
          }
          // reset the Worker tables to remove last slave session time info
          if ((error= rli->mts_finalize_recovery()))
          {
            (void) Rpl_info_factory::reset_workers(rli);
          }
        }
        rli->mts_recovery_group_seen_begin= false;
        if (!error)
          error= rli->flush_info(true);
      }
    }

    if (error)
    {
      /*
        The update should not fail, so print an error message and
        return an error code.

        TODO: Replace this with a decent error message when merged
        with BUG#24954 (which adds several new error message).
      */
      char buf[22];
      rli->report(ERROR_LEVEL, ER_UNKNOWN_ERROR,
                  "It was not possible to update the positions"
                  " of the relay log information: the slave may"
                  " be in an inconsistent state."
                  " Stopped in %s position %s",
                  rli->get_group_relay_log_name(),
                  llstr(rli->get_group_relay_log_pos(), buf));
      DBUG_RETURN(SLAVE_APPLY_EVENT_AND_UPDATE_POS_UPDATE_POS_ERROR);
    }
  }

  DBUG_RETURN(exec_res ? SLAVE_APPLY_EVENT_AND_UPDATE_POS_APPLY_ERROR
                       : SLAVE_APPLY_EVENT_AND_UPDATE_POS_OK);
}

/**
  Let the worker applying the current group to rollback and gracefully
  finish its work before.

  @param rli The slave's relay log info.

  @param ev a pointer to the event on hold before applying this rollback
  procedure.

  @retval false The rollback succeeded.

  @retval true  There was an error while injecting events.
*/
static bool coord_handle_partial_binlogged_transaction(Relay_log_info *rli,
                                                       const Log_event *ev)
{
  DBUG_ENTER("coord_handle_partial_binlogged_transaction");
  /*
    This function is called holding the rli->data_lock.
    We must return it still holding this lock, except in the case of returning
    error.
  */
  mysql_mutex_assert_owner(&rli->data_lock);
  THD *thd= rli->info_thd;

  if (!rli->curr_group_seen_begin)
  {
    DBUG_PRINT("info",("Injecting QUERY(BEGIN) to rollback worker"));
    Log_event *begin_event= new Query_log_event(thd,
                                                STRING_WITH_LEN("BEGIN"),
                                                true, /* using_trans */
                                                false, /* immediate */
                                                true, /* suppress_use */
                                                0, /* error */
                                                true /* ignore_command */);
    ((Query_log_event*) begin_event)->db= "";
    begin_event->data_written= 0;
    begin_event->server_id= ev->server_id;
    /*
      We must be careful to avoid SQL thread increasing its position
      farther than the event that triggered this QUERY(BEGIN).
    */
    begin_event->log_pos= ev->log_pos;
    begin_event->future_event_relay_log_pos= ev->future_event_relay_log_pos;

    if (apply_event_and_update_pos(&begin_event, thd, rli) !=
        SLAVE_APPLY_EVENT_AND_UPDATE_POS_OK)
    {
      delete begin_event;
      DBUG_RETURN(true);
    }
    mysql_mutex_lock(&rli->data_lock);
  }

  DBUG_PRINT("info",("Injecting QUERY(ROLLBACK) to rollback worker"));
  Log_event *rollback_event= new Query_log_event(thd,
                                                 STRING_WITH_LEN("ROLLBACK"),
                                                 true, /* using_trans */
                                                 false, /* immediate */
                                                 true, /* suppress_use */
                                                 0, /* error */
                                                 true /* ignore_command */);
  ((Query_log_event*) rollback_event)->db= "";
  rollback_event->data_written= 0;
  rollback_event->server_id= ev->server_id;
  // Set a flag for this special ROLLBACK event so the slave worker
  // skips updating slave_gtid_info table.
  rollback_event->set_relay_log_event();
  /*
    We must be careful to avoid SQL thread increasing its position
    farther than the event that triggered this QUERY(ROLLBACK).
  */
  rollback_event->log_pos= ev->log_pos;
  rollback_event->future_event_relay_log_pos= ev->future_event_relay_log_pos;

  if (apply_event_and_update_pos(&rollback_event, thd, rli) !=
      SLAVE_APPLY_EVENT_AND_UPDATE_POS_OK)
  {
    delete rollback_event;
    DBUG_RETURN(true);
  }
  mysql_mutex_lock(&rli->data_lock);

  DBUG_RETURN(false);
}

/**
  Top-level function for executing the next event in the relay log.
  This is called from the SQL thread.

  This function reads the event from the relay log, executes it, and
  advances the relay log position.  It also handles errors, etc.

  This function may fail to apply the event for the following reasons:

   - The position specfied by the UNTIL condition of the START SLAVE
     command is reached.

   - It was not possible to read the event from the log.

   - The slave is killed.

   - An error occurred when applying the event, and the event has been
     tried slave_trans_retries times.  If the event has been retried
     fewer times, 0 is returned.

   - init_info or init_relay_log_pos failed. (These are called
     if a failure occurs when applying the event.)

   - An error occurred when updating the binlog position.

  @retval 0 The event was applied.

  @retval 1 The event was not applied.
*/
static int exec_relay_log_event(THD* thd, Relay_log_info* rli)
{
  DBUG_ENTER("exec_relay_log_event");

  /*
     We acquire this mutex since we need it for all operations except
     event execution. But we will release it in places where we will
     wait for something for example inside of next_event().
   */
  mysql_mutex_lock(&rli->data_lock);

  /*
    UNTIL_SQL_AFTER_GTIDS requires special handling since we have to check
    whether the until_condition is satisfied *before* the SQL threads goes on
    a wait inside next_event() for the relay log to grow. This is required since
    if we have already applied the last event in the waiting set but since he
    check happens only at the start of the next event we may end up waiting
    forever the next event is not available or is delayed.
  */
  if (rli->until_condition == Relay_log_info::UNTIL_SQL_AFTER_GTIDS &&
       rli->is_until_satisfied(thd, NULL))
  {
    rli->abort_slave= 1;
    mysql_mutex_unlock(&rli->data_lock);
    DBUG_RETURN(1);
  }

  Log_event *ev = next_event(rli), **ptr_ev;

  DBUG_ASSERT(rli->info_thd==thd);

  if (sql_slave_killed(thd,rli))
  {
    mysql_mutex_unlock(&rli->data_lock);
    delete ev;
    DBUG_RETURN(1);
  }
  if (ev)
  {
    enum enum_slave_apply_event_and_update_pos_retval exec_res;

    ptr_ev= &ev;
    /*
      Even if we don't execute this event, we keep the master timestamp,
      so that seconds behind master shows correct delta (there are events
      that are not replayed, so we keep falling behind).

      If it is an artificial event, or a relay log event (IO thread generated
      event) or ev->when is set to 0, or a FD from master, or a heartbeat
      event with server_id '0' then  we don't update the last_master_timestamp.
    */
    if (!(rli->is_parallel_exec() ||
          ev->is_artificial_event() || ev->is_relay_log_event() ||
          ev->when.tv_sec == 0 || ev->get_type_code() == FORMAT_DESCRIPTION_EVENT ||
          ev->server_id == 0 || ev->get_type_code() == ROTATE_EVENT ||
          ev->get_type_code() == PREVIOUS_GTIDS_LOG_EVENT))
    {
      DBUG_ASSERT(ev->get_type_code() != ROTATE_EVENT && ev->get_type_code() !=
                  PREVIOUS_GTIDS_LOG_EVENT);

      // Set the flag to say that "the slave has not yet caught up"
      rli->slave_has_caughtup= Enum_slave_caughtup::NO;
      /*
        To avoid spiky second behind master, we here keeps last_master_timestamp
        monotonic for the non-parallel execution cases. In other words, we only
        assign the new tentative_last_master_timestamp to the
        last_master_timestamp if the tentative one is bigger. If the tentative
        is too big so that it's beyond current slave time, we assign the
        current time of the slave to the last_master_timestamp.
        see @Relay_log_info::set_last_master_timestamp()
      */
      time_t tentative_last_master_timestamp=
        ev->when.tv_sec + (time_t) ev->exec_time;

      // milli seconds behind master related for non-MTS
      ulonglong tentative_last_master_ts_millis= 0;

      // case: if trx meta data is enabled and this is a rows query event with
      // trx meta data attempt to get the ts in milli secs
      if (opt_binlog_trx_meta_data &&
          ev->get_type_code() == ROWS_QUERY_LOG_EVENT &&
          static_cast<Rows_query_log_event*>(ev)->has_trx_meta_data())
      {
        tentative_last_master_ts_millis=
          static_cast<Rows_query_log_event*>(ev)->extract_last_timestamp();
      }

      rli->set_last_master_timestamp(tentative_last_master_timestamp,
                                     tentative_last_master_ts_millis);

      DBUG_ASSERT(rli->last_master_timestamp >= 0);
    }

    /*
      This tests if the position of the beginning of the current event
      hits the UNTIL barrier.
      MTS: since the master and the relay-group coordinates change
      asynchronously logics of rli->is_until_satisfied() can't apply.
      A special UNTIL_SQL_AFTER_MTS_GAPS is still deployed here
      temporarily (see is_until_satisfied todo).
    */
    if (rli->until_condition != Relay_log_info::UNTIL_NONE &&
        rli->until_condition != Relay_log_info::UNTIL_SQL_AFTER_GTIDS &&
        rli->is_until_satisfied(thd, ev))
    {
      /*
        Setting abort_slave flag because we do not want additional message about
        error in query execution to be printed.
      */
      rli->abort_slave= 1;
      mysql_mutex_unlock(&rli->data_lock);
      delete ev;
      DBUG_RETURN(1);
    }

    { /**
         The following failure injecion works in cooperation with tests
         setting @@global.debug= 'd,incomplete_group_in_relay_log'.
         Xid or Commit events are not executed to force the slave sql
         read hanging if the realy log does not have any more events.
      */
      DBUG_EXECUTE_IF("incomplete_group_in_relay_log",
                      if ((ev->get_type_code() == XID_EVENT) ||
                          ((ev->get_type_code() == QUERY_EVENT) &&
                           strcmp("COMMIT", ((Query_log_event *) ev)->query) == 0))
                      {
                        DBUG_ASSERT(thd->transaction.all.cannot_safely_rollback());
                        rli->abort_slave= 1;
                        mysql_mutex_unlock(&rli->data_lock);
                        delete ev;
                        rli->inc_event_relay_log_pos();
                        DBUG_RETURN(0);
                      };);
    }

    /*
      GTID protocol will put a FORMAT_DESCRIPTION_EVENT from the master with
      log_pos != 0 after each (re)connection if auto positioning is enabled.
      This means that the SQL thread might have already started to apply the
      current group but, as the IO thread had to reconnect, it left this
      group incomplete and will start it again from the beginning.
      So, before applying this FORMAT_DESCRIPTION_EVENT, we must let the
      worker roll back the current group and gracefully finish its work,
      before starting to apply the new (complete) copy of the group.
    */
    if (ev->get_type_code() == FORMAT_DESCRIPTION_EVENT &&
        ev->server_id != ::server_id && ev->log_pos != 0 &&
        rli->is_parallel_exec() && rli->curr_group_seen_gtid)
    {
      if (coord_handle_partial_binlogged_transaction(rli, ev))
        /*
          In the case of an error, coord_handle_partial_binlogged_transaction
          will not try to get the rli->data_lock again.
        */
        DBUG_RETURN(1);
    }

    /* ptr_ev can change to NULL indicating MTS coorinator passed to a Worker */
    exec_res= apply_event_and_update_pos(ptr_ev, thd, rli);
    /*
      Note: the above call to apply_event_and_update_pos executes
      mysql_mutex_unlock(&rli->data_lock);
    */

    /* For deferred events, the ptr_ev is set to NULL
        in Deferred_log_events::add() function.
        Hence deferred events wont be deleted here.
        They will be deleted in Deferred_log_events::rewind() funciton.
    */
    if (*ptr_ev)
    {
      DBUG_ASSERT(*ptr_ev == ev); // event remains to belong to Coordinator

      DBUG_EXECUTE_IF("dbug.calculate_sbm_after_previous_gtid_log_event",
                    {
                      if (ev->get_type_code() == PREVIOUS_GTIDS_LOG_EVENT)
                      {
                        const char act[]= "now signal signal.reached wait_for signal.done_sbm_calculation";
                        DBUG_ASSERT(opt_debug_sync_timeout > 0);
                        DBUG_ASSERT(!debug_sync_set_action(thd, STRING_WITH_LEN(act)));
                      }
                    };);
      /*
        Format_description_log_event should not be deleted because it will be
        used to read info about the relay log's format; it will be deleted when
        the SQL thread does not need it, i.e. when this thread terminates.
        ROWS_QUERY_LOG_EVENT is destroyed at the end of the current statement
        clean-up routine.
      */
      if (ev->get_type_code() != FORMAT_DESCRIPTION_EVENT &&
          ev->get_type_code() != ROWS_QUERY_LOG_EVENT)
      {
        DBUG_PRINT("info", ("Deleting the event after it has been executed"));
        delete ev;
        ev= NULL;
      }
    }

    /*
      exec_res == SLAVE_APPLY_EVENT_AND_UPDATE_POS_UPDATE_POS_ERROR
                  update_log_pos failed: this should not happen, so we
                  don't retry.
      exec_res == SLAVE_APPLY_EVENT_AND_UPDATE_POS_APPEND_JOB_ERROR
                  append_item_to_jobs() failed, this happened because
                  thread was killed while waiting for enqueue on worker.
    */
    if (exec_res >= SLAVE_APPLY_EVENT_AND_UPDATE_POS_UPDATE_POS_ERROR)
    {
      delete ev;
      DBUG_RETURN(1);
    }

    if (slave_trans_retries)
    {
      int UNINIT_VAR(temp_err);
      bool silent= false;
      if (exec_res && !is_mts_worker(thd) /* no reexecution in MTS mode */ &&
          (temp_err= rli->has_temporary_error(thd, 0, &silent)) &&
          !thd->transaction.all.cannot_safely_rollback())
      {
        const char *errmsg;
        /*
          We were in a transaction which has been rolled back because of a
          temporary error;
          let's seek back to BEGIN log event and retry it all again.
	  Note, if lock wait timeout (innodb_lock_wait_timeout exceeded)
	  there is no rollback since 5.0.13 (ref: manual).
          We have to not only seek but also
          a) init_info(), to seek back to hot relay log's start for later
          (for when we will come back to this hot log after re-processing the
          possibly existing old logs where BEGIN is: check_binlog_magic() will
          then need the cache to be at position 0 (see comments at beginning of
          init_info()).
          b) init_relay_log_pos(), because the BEGIN may be an older relay log.
        */
        if (rli->trans_retries < slave_trans_retries)
        {
          /*
            The transactions has to be rolled back before global_init_info is
            called. Because global_init_info will starts a new transaction if
            master_info_repository is TABLE.
          */
          rli->cleanup_context(thd, 1);
          /*
             We need to figure out if there is a test case that covers
             this part. \Alfranio.
          */
          if (global_init_info(rli->mi, false, SLAVE_SQL))
            sql_print_error("Failed to initialize the master info structure");
          else if (rli->init_relay_log_pos(rli->get_group_relay_log_name(),
                                           rli->get_group_relay_log_pos(),
                                           true/*need_data_lock=true*/,
                                           &errmsg, 1))
            sql_print_error("Error initializing relay log position: %s",
                            errmsg);
          else
          {
            exec_res= SLAVE_APPLY_EVENT_AND_UPDATE_POS_OK;
            /* chance for concurrent connection to get more locks */
            slave_sleep(thd, min<ulong>(rli->trans_retries, MAX_SLAVE_RETRY_PAUSE),
                        sql_slave_killed, rli);
            mysql_mutex_lock(&rli->data_lock); // because of SHOW STATUS
            if (!silent)
              rli->trans_retries++;

            rli->retried_trans++;
            mysql_mutex_unlock(&rli->data_lock);
            DBUG_PRINT("info", ("Slave retries transaction "
                                "rli->trans_retries: %lu", rli->trans_retries));
          }
        }
        else
        {
          thd->is_fatal_error= 1;
          rli->set_fatal_error();
          rli->report(ERROR_LEVEL, thd->get_stmt_da()->sql_errno(),
                      "Slave SQL thread retried transaction %lu time(s) "
                      "in vain, giving up. Consider raising the value of "
                      "the slave_transaction_retries variable.", rli->trans_retries);
        }
      }
      else if ((exec_res && !temp_err) ||
               (opt_using_transactions &&
                rli->get_group_relay_log_pos() == rli->get_event_relay_log_pos()))
      {
        /*
          Only reset the retry counter if the entire group succeeded
          or failed with a non-transient error.  On a successful
          event, the execution will proceed as usual; in the case of a
          non-transient error, the slave will stop with an error.
         */
        rli->trans_retries= 0; // restart from fresh
        DBUG_PRINT("info", ("Resetting retry counter, rli->trans_retries: %lu",
                            rli->trans_retries));
      }
    }
    if (exec_res)
      delete ev;
    DBUG_RETURN(exec_res);
  }
  mysql_mutex_unlock(&rli->data_lock);
  rli->report(ERROR_LEVEL, ER_SLAVE_RELAY_LOG_READ_FAILURE,
              ER(ER_SLAVE_RELAY_LOG_READ_FAILURE), "\
Could not parse relay log event entry. The possible reasons are: the master's \
binary log is corrupted (you can check this by running 'mysqlbinlog' on the \
binary log), the slave's relay log is corrupted (you can check this by running \
'mysqlbinlog' on the relay log), a network problem, or a bug in the master's \
or slave's MySQL code. If you want to check the master's binary log or slave's \
relay log, you will be able to know their names by issuing 'SHOW SLAVE STATUS' \
on this slave.\
");
  DBUG_RETURN(1);
}

static bool check_io_slave_killed(THD *thd, Master_info *mi, const char *info)
{
  if (io_slave_killed(thd, mi))
  {
    if (info && log_warnings)
      sql_print_information("%s", info);
    return TRUE;
  }
  return FALSE;
}

/**
  @brief Try to reconnect slave IO thread.

  @details Terminates current connection to master, sleeps for
  @c mi->connect_retry msecs and initiates new connection with
  @c safe_reconnect(). Variable pointed by @c retry_count is increased -
  if it exceeds @c mi->retry_count then connection is not re-established
  and function signals error.
  Unless @c suppres_warnings is TRUE, a warning is put in the server error log
  when reconnecting. The warning message and messages used to report errors
  are taken from @c messages array. In case @c mi->retry_count is exceeded,
  no messages are added to the log.

  @param[in]     thd                 Thread context.
  @param[in]     mysql               MySQL connection.
  @param[in]     mi                  Master connection information.
  @param[in,out] retry_count         Number of attempts to reconnect.
  @param[in]     suppress_warnings   TRUE when a normal net read timeout
                                     has caused to reconnecting.
  @param[in]     messages            Messages to print/log, see
                                     reconnect_messages[] array.

  @retval        0                   OK.
  @retval        1                   There was an error.
*/

static int try_to_reconnect(THD *thd, MYSQL *mysql, Master_info *mi,
                            uint *retry_count, bool suppress_warnings,
                            const char *messages[SLAVE_RECON_MSG_MAX])
{
  mi->slave_running= MYSQL_SLAVE_RUN_NOT_CONNECT;
  thd->proc_info= messages[SLAVE_RECON_MSG_WAIT];
#ifdef SIGNAL_WITH_VIO_SHUTDOWN
  thd->clear_active_vio();
#endif
  end_server(mysql);
  DBUG_EXECUTE_IF("simulate_no_master_reconnect",
                   {
                     return 1;
                   });
  if ((*retry_count)++)
  {
    if (*retry_count > mi->retry_count)
      return 1;                             // Don't retry forever
    slave_sleep(thd, mi->connect_retry, io_slave_killed, mi);
  }
  if (check_io_slave_killed(thd, mi, messages[SLAVE_RECON_MSG_KILLED_WAITING]))
    return 1;
  thd->proc_info = messages[SLAVE_RECON_MSG_AFTER];
  if (!suppress_warnings)
  {
    char buf[256], llbuff[22];
    my_snprintf(buf, sizeof(buf), messages[SLAVE_RECON_MSG_FAILED],
                mi->get_io_rpl_log_name(), llstr(mi->get_master_log_pos(),
                llbuff));
    /*
      Raise a warining during registering on master/requesting dump.
      Log a message reading event.
    */
    if (messages[SLAVE_RECON_MSG_COMMAND][0])
    {
      mi->report(WARNING_LEVEL, ER_SLAVE_MASTER_COM_FAILURE,
                 ER(ER_SLAVE_MASTER_COM_FAILURE),
                 messages[SLAVE_RECON_MSG_COMMAND], buf);
    }
    else
    {
      sql_print_information("%s", buf);
    }
  }
  if (safe_reconnect(thd, mysql, mi, 1) || io_slave_killed(thd, mi))
  {
    if (log_warnings)
      sql_print_information("%s", messages[SLAVE_RECON_MSG_KILLED_AFTER]);
    return 1;
  }
  return 0;
}


/**
  Slave IO thread entry point.

  @param arg Pointer to Master_info struct that holds information for
  the IO thread.

  @return Always 0.
*/
pthread_handler_t handle_slave_io(void *arg)
{
  THD *thd= NULL; // needs to be first for thread_stack
  bool thd_added= false;
  MYSQL *mysql;
  Master_info *mi = (Master_info*)arg;
  Relay_log_info *rli= mi->rli;
  char llbuff[22];
  uint retry_count;
  bool suppress_warnings;
  int ret;
  int binlog_version;
  bool slave_stats_daemon_created= false;
#ifndef DBUG_OFF
  uint retry_count_reg= 0, retry_count_dump= 0, retry_count_event= 0;
#endif
  // needs to call my_thread_init(), otherwise we get a coredump in DBUG_ stuff
  my_thread_init();
  DBUG_ENTER("handle_slave_io");

  if (enable_raft_plugin)
  {
    sql_print_information(
        "Did not start IO thread because enable_raft_plugin was ON");
    DBUG_RETURN(0);
  }

  DBUG_ASSERT(mi->inited);
  mysql= NULL ;
  retry_count= 0;

  mysql_mutex_lock(&mi->run_lock);
  /* Inform waiting threads that slave has started */
  mi->slave_run_id++;

#ifndef DBUG_OFF
  mi->events_until_exit = disconnect_slave_event_count;
#endif

  thd= new THD; // note that contructor of THD uses DBUG_ !
  THD_CHECK_SENTRY(thd);
  mysql_mutex_lock(&mi->info_thd_lock);
  mi->info_thd = thd;
  mysql_mutex_unlock(&mi->info_thd_lock);

  pthread_detach_this_thread();
  thd->thread_stack= (char*) &thd; // remember where our stack is
  mi->clear_error();
  if (init_slave_thread(thd, SLAVE_THD_IO))
  {
    mysql_cond_broadcast(&mi->start_cond);
    mysql_mutex_unlock(&mi->run_lock);
    sql_print_error("Failed during slave I/O thread initialization");
    goto err;
  }

  mutex_lock_shard(SHARDED(&LOCK_thread_count), thd);
  add_global_thread(thd);
  thd_added= true;
  mutex_unlock_shard(SHARDED(&LOCK_thread_count), thd);

  mi->slave_running = 1;
  mi->abort_slave = 0;
  mysql_mutex_unlock(&mi->run_lock);
  mysql_cond_broadcast(&mi->start_cond);

  DBUG_PRINT("master_info",("log_file_name: '%s'  position: %s",
                            mi->get_master_log_name(),
                            llstr(mi->get_master_log_pos(), llbuff)));

  /* This must be called before run any binlog_relay_io hooks */
  my_pthread_setspecific_ptr(RPL_MASTER_INFO, mi);

  if (RUN_HOOK(binlog_relay_io, thread_start, (thd, mi)))
  {
    mi->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR,
               ER(ER_SLAVE_FATAL_ERROR), "Failed to run 'thread_start' hook");
    goto err;
  }

  if (!(mi->mysql = mysql = mysql_init(NULL)))
  {
    mi->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR,
               ER(ER_SLAVE_FATAL_ERROR), "error in mysql_init()");
    goto err;
  }

  THD_STAGE_INFO(thd, stage_connecting_to_master);
  // we can get killed during safe_connect
  if (!safe_connect(thd, mysql, mi))
  {
    sql_print_information("Slave I/O thread: connected to master '%s@%s:%d',"
                          "replication started in log '%s' at position %s",
                          mi->get_user(), mi->host, mi->port,
			  mi->get_io_rpl_log_name(),
			  llstr(mi->get_master_log_pos(), llbuff));
  }
  else
  {
    sql_print_information("Slave I/O thread killed while connecting to master");
    goto err;
  }

connected:

  ++relay_io_connected;
    DBUG_EXECUTE_IF("dbug.before_get_running_status_yes",
                    {
                      const char act[]=
                        "now "
                        "wait_for signal.io_thread_let_running";
                      DBUG_ASSERT(opt_debug_sync_timeout > 0);
                      DBUG_ASSERT(!debug_sync_set_action(thd,
                                                         STRING_WITH_LEN(act)));
                    };);
    DBUG_EXECUTE_IF("dbug.calculate_sbm_after_previous_gtid_log_event",
                    {
                      /* Fake that thread started 3 mints ago */
                      thd->start_time.tv_sec-=180;
                    };);
  mysql_mutex_lock(&mi->run_lock);
  mi->slave_running= MYSQL_SLAVE_RUN_CONNECT;
  mysql_mutex_unlock(&mi->run_lock);

  thd->slave_net = &mysql->net;
  THD_STAGE_INFO(thd, stage_checking_master_version);
  ret= get_master_version_and_clock(mysql, mi);
  if (!ret)
    ret= get_master_uuid(mysql, mi);
  if (!ret)
    ret= io_thread_init_commands(mysql, mi);

  if (ret == 1)
    /* Fatal error */
    goto err;

  if (ret == 2)
  {
    if (check_io_slave_killed(mi->info_thd, mi, "Slave I/O thread killed"
                              "while calling get_master_version_and_clock(...)"))
      goto err;
    suppress_warnings= FALSE;
    /* Try to reconnect because the error was caused by a transient network problem */
    if (try_to_reconnect(thd, mysql, mi, &retry_count, suppress_warnings,
                             reconnect_messages[SLAVE_RECON_ACT_REG]))
      goto err;
    goto connected;
  }

  mysql_mutex_lock(&mi->data_lock);
  binlog_version= mi->get_mi_description_event()->binlog_version;
  mysql_mutex_unlock(&mi->data_lock);

  if (binlog_version > 1)
  {
    /*
      Register ourselves with the master.
    */
    THD_STAGE_INFO(thd, stage_registering_slave_on_master);
    if (register_slave_on_master(mysql, mi, &suppress_warnings))
    {
      if (!check_io_slave_killed(thd, mi, "Slave I/O thread killed "
                                "while registering slave on master"))
      {
        sql_print_error("Slave I/O thread couldn't register on master");
        if (try_to_reconnect(thd, mysql, mi, &retry_count, suppress_warnings,
                             reconnect_messages[SLAVE_RECON_ACT_REG]))
          goto err;
      }
      else
        goto err;
      goto connected;
    }
    DBUG_EXECUTE_IF("FORCE_SLAVE_TO_RECONNECT_REG",
      if (!retry_count_reg)
      {
        retry_count_reg++;
        sql_print_information("Forcing to reconnect slave I/O thread");
        if (try_to_reconnect(thd, mysql, mi, &retry_count, suppress_warnings,
                             reconnect_messages[SLAVE_RECON_ACT_REG]))
          goto err;
        goto connected;
      });
  }
  if (!slave_stats_daemon_created) {
    // start sending secondary lag stats to primary 
    slave_stats_daemon_created = start_handle_slave_stats_daemon();
  }

  DBUG_PRINT("info",("Starting reading binary log from master"));
  while (!io_slave_killed(thd,mi))
  {
    THD_STAGE_INFO(thd, stage_requesting_binlog_dump);
    if (request_dump(thd, mysql, mi, &suppress_warnings))
    {
      sql_print_error("Failed on request_dump()");
      if (check_io_slave_killed(thd, mi, "Slave I/O thread killed while \
requesting master dump") ||
          try_to_reconnect(thd, mysql, mi, &retry_count, suppress_warnings,
                           reconnect_messages[SLAVE_RECON_ACT_DUMP]))
        goto err;
      goto connected;
    }
    DBUG_EXECUTE_IF("FORCE_SLAVE_TO_RECONNECT_DUMP",
      if (!retry_count_dump)
      {
        retry_count_dump++;
        sql_print_information("Forcing to reconnect slave I/O thread");
        if (try_to_reconnect(thd, mysql, mi, &retry_count, suppress_warnings,
                             reconnect_messages[SLAVE_RECON_ACT_DUMP]))
          goto err;
        goto connected;
      });
    const char *event_buf;

    DBUG_ASSERT(mi->last_error().number == 0);
    while (!io_slave_killed(thd,mi))
    {
      ulong event_len;
      /*
         We say "waiting" because read_event() will wait if there's nothing to
         read. But if there's something to read, it will not wait. The
         important thing is to not confuse users by saying "reading" whereas
         we're in fact receiving nothing.
      */
      THD_STAGE_INFO(thd, stage_waiting_for_master_to_send_event);
      event_len= read_event(mysql, mi, &suppress_warnings);
      if (check_io_slave_killed(thd, mi, "Slave I/O thread killed while \
reading event"))
        goto err;
      DBUG_EXECUTE_IF("FORCE_SLAVE_TO_RECONNECT_EVENT",
        if (!retry_count_event)
        {
          retry_count_event++;
          sql_print_information("Forcing to reconnect slave I/O thread");
          if (try_to_reconnect(thd, mysql, mi, &retry_count, suppress_warnings,
                               reconnect_messages[SLAVE_RECON_ACT_EVENT]))
            goto err;
          goto connected;
        });

      if (event_len == packet_error)
      {
        uint mysql_error_number= mysql_errno(mysql);
        switch (mysql_error_number) {
        case CR_NET_PACKET_TOO_LARGE:
          sql_print_error("\
Log entry on master is longer than slave_max_allowed_packet (%lu) on \
slave. If the entry is correct, restart the server with a higher value of \
slave_max_allowed_packet",
                         slave_max_allowed_packet);
          mi->report(ERROR_LEVEL, ER_NET_PACKET_TOO_LARGE,
                     "%s", "Got a packet bigger than 'slave_max_allowed_packet' bytes");
          goto err;
        case ER_MASTER_FATAL_ERROR_READING_BINLOG:
          mi->report(ERROR_LEVEL, ER_MASTER_FATAL_ERROR_READING_BINLOG,
                     ER(ER_MASTER_FATAL_ERROR_READING_BINLOG),
                     mysql_error_number, mysql_error(mysql));
          goto err;
        case ER_OUT_OF_RESOURCES:
          sql_print_error("\
Stopping slave I/O thread due to out-of-memory error from master");
          mi->report(ERROR_LEVEL, ER_OUT_OF_RESOURCES,
                     "%s", ER(ER_OUT_OF_RESOURCES));
          goto err;
        }
        if (try_to_reconnect(thd, mysql, mi, &retry_count, suppress_warnings,
                             reconnect_messages[SLAVE_RECON_ACT_EVENT]))
          goto err;
        goto connected;
      } // if (event_len == packet_error)

      relay_io_events++;
      relay_io_bytes += event_len;

      retry_count=0;                    // ok event, reset retry counter
      THD_STAGE_INFO(thd, stage_queueing_master_event_to_the_relay_log);
      event_buf= (const char*)mysql->net.read_pos + 1;
      DBUG_PRINT("info", ("IO thread received event of type %s", Log_event::get_type_str((Log_event_type)event_buf[EVENT_TYPE_OFFSET])));
      if (RUN_HOOK(binlog_relay_io, after_read_event,
                   (thd, mi,(const char*)mysql->net.read_pos + 1,
                    event_len, &event_buf, &event_len)))
      {
        mi->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR,
                   ER(ER_SLAVE_FATAL_ERROR),
                   "Failed to run 'after_read_event' hook");
        goto err;
      }

      /* XXX: 'synced' should be updated by queue_event to indicate
         whether event has been synced to disk */
      bool synced= 0;
      if (queue_event(mi, event_buf, event_len))
      {
        mi->report(ERROR_LEVEL, ER_SLAVE_RELAY_LOG_WRITE_FAILURE,
                   ER(ER_SLAVE_RELAY_LOG_WRITE_FAILURE),
                   "could not queue event from master");
        goto err;
      }

      DBUG_EXECUTE_IF("error_before_semi_sync_reply", goto err;);

      if (RUN_HOOK(binlog_relay_io, after_queue_event,
                   (thd, mi, event_buf, event_len, synced)))
      {
        mi->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR,
                   ER(ER_SLAVE_FATAL_ERROR),
                   "Failed to run 'after_queue_event' hook");
        goto err;
      }

      mysql_mutex_lock(&mi->data_lock);
      if (flush_master_info(mi, FALSE))
      {
        mi->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR,
                   ER(ER_SLAVE_FATAL_ERROR),
                   "Failed to flush master info.");
        mysql_mutex_unlock(&mi->data_lock);
        goto err;
      }
      mysql_mutex_unlock(&mi->data_lock);

      /*
        See if the relay logs take too much space.
        We don't lock mi->rli->log_space_lock here; this dirty read saves time
        and does not introduce any problem:
        - if mi->rli->ignore_log_space_limit is 1 but becomes 0 just after (so
        the clean value is 0), then we are reading only one more event as we
        should, and we'll block only at the next event. No big deal.
        - if mi->rli->ignore_log_space_limit is 0 but becomes 1 just after (so
        the clean value is 1), then we are going into wait_for_relay_log_space()
        for no reason, but this function will do a clean read, notice the clean
        value and exit immediately.
      */
#ifndef DBUG_OFF
      {
        char llbuf1[22], llbuf2[22];
        DBUG_PRINT("info", ("log_space_limit=%s log_space_total=%s \
ignore_log_space_limit=%d",
                            llstr(rli->log_space_limit,llbuf1),
                            llstr(rli->log_space_total,llbuf2),
                            (int) rli->ignore_log_space_limit));
      }
#endif

      if (rli->log_space_limit && rli->log_space_limit <
          rli->log_space_total &&
          !rli->ignore_log_space_limit)
        if (wait_for_relay_log_space(rli))
        {
          sql_print_error("Slave I/O thread aborted while waiting for relay \
log space");
          goto err;
        }
      DBUG_EXECUTE_IF("flush_after_reading_user_var_event",
                      {
                      if (event_buf[EVENT_TYPE_OFFSET] == USER_VAR_EVENT)
                      {
                      const char act[]= "now signal Reached wait_for signal.flush_complete_continue";
                      DBUG_ASSERT(opt_debug_sync_timeout > 0);
                      DBUG_ASSERT(!debug_sync_set_action(current_thd,
                                                         STRING_WITH_LEN(act)));

                      }
                      });
      DBUG_EXECUTE_IF("stop_io_after_reading_gtid_log_event",
        if (event_buf[EVENT_TYPE_OFFSET] == GTID_LOG_EVENT)
          thd->killed= THD::KILLED_NO_VALUE;
      );
      DBUG_EXECUTE_IF("stop_io_after_reading_query_log_event",
        if (event_buf[EVENT_TYPE_OFFSET] == QUERY_EVENT)
          thd->killed= THD::KILLED_NO_VALUE;
      );
      DBUG_EXECUTE_IF("stop_io_after_reading_user_var_log_event",
        if (event_buf[EVENT_TYPE_OFFSET] == USER_VAR_EVENT)
          thd->killed= THD::KILLED_NO_VALUE;
      );
      DBUG_EXECUTE_IF("stop_io_after_reading_table_map_event",
        if (event_buf[EVENT_TYPE_OFFSET] == TABLE_MAP_EVENT)
          thd->killed= THD::KILLED_NO_VALUE;
      );
      DBUG_EXECUTE_IF("stop_io_after_reading_xid_log_event",
        if (event_buf[EVENT_TYPE_OFFSET] == XID_EVENT)
          thd->killed= THD::KILLED_NO_VALUE;
      );
      DBUG_EXECUTE_IF("stop_io_after_reading_write_rows_log_event",
        if (event_buf[EVENT_TYPE_OFFSET] == WRITE_ROWS_EVENT)
          thd->killed= THD::KILLED_NO_VALUE;
      );
      /*
        After event is flushed to relay log file, memory used
        by thread's mem_root is not required any more.
        Hence adding free_root(thd->mem_root,...) to do the
        cleanup, otherwise a long running IO thread can
        cause OOM error.
      */
      free_root(thd->mem_root, MYF(MY_KEEP_PREALLOC));
    }
  }

  // error = 0;
err:
  if (slave_stats_daemon_created) {
    // stop sending secondary lag stats to primary 
    stop_handle_slave_stats_daemon();
  }

  // print the current replication position
  sql_print_information("Slave I/O thread exiting, read up to log '%s', position %s",
                  mi->get_io_rpl_log_name(), llstr(mi->get_master_log_pos(), llbuff));
  (void) RUN_HOOK(binlog_relay_io, thread_stop, (thd, mi));
  thd->reset_query();
  thd->reset_db(NULL, 0);
  if (mysql)
  {
    /*
      Here we need to clear the active VIO before closing the
      connection with the master.  The reason is that THD::awake()
      might be called from terminate_slave_thread() because somebody
      issued a STOP SLAVE.  If that happends, the shutdown_active_vio()
      can be called in the middle of closing the VIO associated with
      the 'mysql' object, causing a crash.
    */
#ifdef SIGNAL_WITH_VIO_SHUTDOWN
    thd->clear_active_vio();
#endif
    mysql_close(mysql);
    mi->mysql=0;
  }
  mysql_mutex_lock(&mi->data_lock);
  write_ignored_events_info_to_relay_log(thd, mi);
  mysql_mutex_unlock(&mi->data_lock);
  THD_STAGE_INFO(thd, stage_waiting_for_slave_mutex_on_exit);
  mysql_mutex_lock(&mi->run_lock);
  /*
    Clean information used to start slave in order to avoid
    security issues.
  */
  mi->reset_start_info();
  /* Forget the relay log's format */
  mysql_mutex_lock(&mi->data_lock);
  mi->set_mi_description_event(NULL);
  mysql_mutex_unlock(&mi->data_lock);

  mysql_mutex_lock(&mi->info_thd_lock);
  mi->info_thd= 0;
  mysql_mutex_unlock(&mi->info_thd_lock);

  NET* net = thd->get_net();
  DBUG_ASSERT(net->buff != 0);
  net_end(net); // destructor will not free it, because net.vio is 0

  thd->release_resources();
  THD_CHECK_SENTRY(thd);
  if (thd_added)
    remove_global_thread(thd);
  delete thd;

  mi->abort_slave= 0;
  mi->slave_running= 0;
  /*
    Note: the order of the two following calls (first broadcast, then unlock)
    is important. Otherwise a killer_thread can execute between the calls and
    delete the mi structure leading to a crash! (see BUG#25306 for details)
   */
  mysql_cond_broadcast(&mi->stop_cond);       // tell the world we are done
  DBUG_EXECUTE_IF("simulate_slave_delay_at_terminate_bug38694", sleep(5););
  mysql_mutex_unlock(&mi->run_lock);
  DBUG_LEAVE;                                   // Must match DBUG_ENTER()
  my_thread_end();
  ERR_remove_state(0);
  pthread_exit(0);
  return(0);                                    // Avoid compiler warnings
}

/*
  Check the temporary directory used by commands like
  LOAD DATA INFILE.
 */
static
int check_temp_dir(char* tmp_file)
{
  int fd;
  MY_DIR *dirp;
  char tmp_dir[FN_REFLEN];
  size_t tmp_dir_size;

  DBUG_ENTER("check_temp_dir");

  /*
    Get the directory from the temporary file.
  */
  dirname_part(tmp_dir, tmp_file, &tmp_dir_size);

  /*
    Check if the directory exists.
   */
  if (!(dirp=my_dir(tmp_dir,MYF(MY_WME))))
    DBUG_RETURN(1);
  my_dirend(dirp);

  /*
    Check permissions to create a file.
   */
  //append the server UUID to the temp file name.
  char *unique_tmp_file_name= (char*)my_malloc((FN_REFLEN+TEMP_FILE_MAX_LEN)*sizeof(char), MYF(0));
  sprintf(unique_tmp_file_name, "%s%s", tmp_file, server_uuid);
  if ((fd= mysql_file_create(key_file_misc,
                             unique_tmp_file_name, CREATE_MODE,
                             O_WRONLY | O_BINARY | O_EXCL | O_NOFOLLOW,
                             MYF(MY_WME))) < 0)
  DBUG_RETURN(1);

  /*
    Clean up.
   */
  mysql_file_close(fd, MYF(0));

  mysql_file_delete(key_file_misc, unique_tmp_file_name, MYF(0));
  my_free(unique_tmp_file_name);
  DBUG_RETURN(0);
}

static std::pair<ulong, ulonglong> cleanup_worker_jobs(Slave_worker *w)
{
  ulong                   ii= 0;
  ulong                   current_event_index;
  ulong                   purge_cnt= 0;
  ulonglong               purge_size= 0;
  struct slave_job_item   job_item;
  std::vector<Log_event*> log_event_free_list;

  mysql_mutex_lock(&w->jobs_lock);

  log_event_free_list.reserve(w->jobs.avail);

  current_event_index = std::max(w->last_current_event_index,
                                 w->current_event_index);
  while (de_queue(&w->jobs, &job_item))
  {
    DBUG_ASSERT(job_item.data);

    Log_event* log_event= static_cast<Log_event*>(job_item.data);

    ii++;
    if (ii > current_event_index)
    {
      purge_size += log_event->data_written;
      purge_cnt++;
    }

    // Save the freeing for outside the mutex
    log_event_free_list.push_back(log_event);
  }

  DBUG_ASSERT(w->jobs.len == 0);

  mysql_mutex_unlock(&w->jobs_lock);

  // Do all the freeing outside the mutex since freeing causes destructors to
  // be called and some destructors acquire locks which can cause deadlock
  // scenarios if we are holding this mutex.
  for (Log_event* log_event : log_event_free_list)
  {
    delete log_event;
  }

  // Return the number and size of the purged events
  return std::make_pair(purge_cnt, purge_size);
}
/*
  Worker thread for the parallel execution of the replication events.
*/
pthread_handler_t handle_slave_worker(void *arg)
{
  THD *thd;                     /* needs to be first for thread_stack */
  bool thd_added= false;
  int error= 0;
  Slave_worker *w= (Slave_worker *) arg;
  Relay_log_info* rli= w->c_rli;
  ulong purge_cnt= 0;
  ulonglong purge_size= 0;
  /* Buffer lifetime extends across the entire runtime of the THD handle. */
  char proc_info_buf[256]= {0};

  my_thread_init();
  DBUG_ENTER("handle_slave_worker");

  thd = new THD_SQL_slave(proc_info_buf, sizeof(proc_info_buf));
  if (!thd)
  {
    sql_print_error("Failed during slave worker initialization");
    goto err;
  }
  mysql_mutex_lock(&w->info_thd_lock);
  w->info_thd= thd;
  thd->thread_stack = (char*)&thd;
  mysql_mutex_unlock(&w->info_thd_lock);

  w->info_thd->variables.tx_isolation=
    static_cast<enum_tx_isolation>(slave_tx_isolation);

  pthread_detach_this_thread();
  if (init_slave_thread(thd, SLAVE_THD_WORKER))
  {
    // todo make SQL thread killed
    sql_print_error("Failed during slave worker initialization");
    goto err;
  }
  thd->init_for_queries(w);

  mutex_lock_shard(SHARDED(&LOCK_thread_count), thd);
  add_global_thread(thd);
  thd_added= true;
  mutex_unlock_shard(SHARDED(&LOCK_thread_count), thd);

  if (w->update_is_transactional())
  {
    rli->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR,
                "Error checking if the worker repository is transactional.");
    goto err;
  }

  mysql_mutex_lock(&w->jobs_lock);
  w->running_status= Slave_worker::RUNNING;
  mysql_cond_signal(&w->jobs_cond);

  mysql_mutex_unlock(&w->jobs_lock);

  DBUG_ASSERT(thd->is_slave_error == 0);

  if (rli->mts_dependency_replication)
  {
    static_cast<Dependency_slave_worker*>(w)->start();
  }
  else
  {
    while (!error)
    {
      error= slave_worker_exec_job(w, rli);
    }
  }

#if defined(FLUSH_REP_INFO)
  // force the flush to the worker info repository.
  w->flush_info(true);
#endif

  /*
     Cleanup after an error requires clear_error() go first.
     Otherwise assert(!all) in binlog_rollback()
  */
  thd->clear_error();
  w->cleanup_context(thd, error);

  std::tie(purge_cnt, purge_size)= cleanup_worker_jobs(w);

  mysql_mutex_lock(&rli->pending_jobs_lock);
  rli->pending_jobs -= purge_cnt;
  rli->mts_pending_jobs_size -= purge_size;
  DBUG_ASSERT(rli->mts_pending_jobs_size < rli->mts_pending_jobs_size_max);

  mysql_mutex_unlock(&rli->pending_jobs_lock);

  /*
     In MTS case cleanup_after_session() has be called explicitly.
     TODO: to make worker thd be deleted before Slave_worker instance.
  */
  if (thd->rli_slave)
  {
    w->cleanup_after_session();
    thd->rli_slave= NULL;
  }
  mysql_mutex_lock(&w->jobs_lock);

  w->running_status= Slave_worker::NOT_RUNNING;
  if (log_warnings > 1)
    sql_print_information("Worker %lu statistics: "
                          "events processed = %lu "
                          "hungry waits = %lu "
                          "priv queue overfills = %llu ",
                          w->id, w->events_done, w->wq_size_waits_cnt,
                          w->jobs.waited_overfill);
  mysql_cond_signal(&w->jobs_cond);  // famous last goodbye

  mysql_mutex_unlock(&w->jobs_lock);

err:

  if (thd)
  {
    /*
       The slave code is very bad. Notice that it is missing
       several clean up calls here. I've just added what was
       necessary to avoid valgrind errors.

       /Alfranio
    */
    NET* net = thd->get_net();
    DBUG_ASSERT(net->buff != 0);
    net_end(net);

    /*
      to avoid close_temporary_tables() closing temp tables as those
      are Coordinator's burden.
    */
    thd->system_thread= NON_SYSTEM_THREAD;
    thd->release_resources();
    THD_CHECK_SENTRY(thd);
    if (thd_added)
      remove_global_thread(thd);
    delete thd;
  }

  my_thread_end();
  ERR_remove_state(0);
  pthread_exit(0);
  DBUG_RETURN(0);
}

/**
   Orders jobs by comparing relay log information.
*/

int mts_event_coord_cmp(LOG_POS_COORD *id1, LOG_POS_COORD *id2)
{
  longlong filecmp= strcmp(id1->file_name, id2->file_name);
  longlong poscmp= id1->pos - id2->pos;
  return (filecmp < 0  ? -1 : (filecmp > 0  ?  1 :
         (poscmp  < 0  ? -1 : (poscmp  > 0  ?  1 : 0))));
}

int mts_recovery_groups(Relay_log_info *rli)
{
  Log_event *ev= NULL;
  const char *errmsg= NULL;
  bool error= FALSE;
  bool flag_group_seen_begin= FALSE;
  uint recovery_group_cnt= 0;
  bool not_reached_commit= true;
  DYNAMIC_ARRAY above_lwm_jobs;
  Slave_job_group job_worker;
  IO_CACHE log;
  File file;
  LOG_INFO linfo;
  my_off_t offset= 0;
  MY_BITMAP *groups= &rli->recovery_groups;
  THD *thd= current_thd;

  DBUG_ENTER("mts_recovery_groups");

  DBUG_ASSERT(rli->slave_parallel_workers == 0);

  /*
     Although mts_recovery_groups() is reentrant it returns
     early if the previous invocation raised any bit in
     recovery_groups bitmap.
  */
  if (rli->is_mts_recovery())
    DBUG_RETURN(0);

  /*
    Save relay log position to compare with worker's position.
  */
  LOG_POS_COORD cp=
  {
    (char *) rli->get_group_master_log_name(),
    rli->get_group_master_log_pos()
  };

  Format_description_log_event fdle(BINLOG_VERSION), *p_fdle= &fdle;

  if (!p_fdle->is_valid())
    DBUG_RETURN(TRUE);

  /*
    Gathers information on valuable workers and stores it in
    above_lwm_jobs in asc ordered by the master binlog coordinates.
  */
  my_init_dynamic_array(&above_lwm_jobs, sizeof(Slave_job_group),
                        rli->recovery_parallel_workers,
                        rli->recovery_parallel_workers);

  /*
    When info tables are used and autocommit= 0 we force a new
    transaction start to avoid table access deadlocks when START SLAVE
    is executed after STOP SLAVE with MTS enabled.
  */
  if (is_autocommit_off_and_infotables(thd))
  {
    if (trans_begin(thd))
    {
      error= TRUE;
      goto err;
    }
  }

  for (uint id= 0; id < rli->recovery_parallel_workers; id++)
  {
    Slave_worker *worker=
      Rpl_info_factory::create_worker(opt_rli_repository_id, id, rli, true);

    if (!worker)
    {
      if (is_autocommit_off_and_infotables(thd))
        trans_rollback(thd);
      error= TRUE;
      goto err;
    }

    LOG_POS_COORD w_last= { const_cast<char*>(worker->get_group_master_log_name()),
                            worker->get_group_master_log_pos() };
    if (mts_event_coord_cmp(&w_last, &cp) > 0)
    {
      /*
        Inserts information into a dynamic array for further processing.
        The jobs/workers are ordered by the last checkpoint positions
        workers have seen.
      */
      job_worker.worker= worker;
      job_worker.checkpoint_log_pos= worker->checkpoint_master_log_pos;
      job_worker.checkpoint_log_name= worker->checkpoint_master_log_name;

      insert_dynamic(&above_lwm_jobs, (uchar*) &job_worker);
    }
    else
    {
      /*
        Deletes the worker because its jobs are included in the latest
        checkpoint.
      */
      delete worker;
    }
  }

  /*
    When info tables are used and autocommit= 0 we force transaction
    commit to avoid table access deadlocks when START SLAVE is executed
    after STOP SLAVE with MTS enabled.
  */
  if (is_autocommit_off_and_infotables(thd))
  {
    if (trans_commit(thd))
    {
      error= TRUE;
      goto err;
    }
  }

  /*
    In what follows, the group Recovery Bitmap is constructed.

     seek(lwm);

     while(w= next(above_lwm_w))
       do
         read G
         if G == w->last_comm
           w.B << group_cnt++;
           RB |= w.B;
            break;
         else
           group_cnt++;
        while(!eof);
        continue;
  */
  DBUG_ASSERT(!rli->recovery_groups_inited);

  if (above_lwm_jobs.elements != 0)
  {
    bitmap_init(groups, NULL, MTS_MAX_BITS_IN_GROUP, FALSE);
    rli->recovery_groups_inited= true;
    bitmap_clear_all(groups);
  }
  rli->mts_recovery_group_cnt= 0;
  for (uint it_job= 0; it_job < above_lwm_jobs.elements; it_job++)
  {
    Slave_worker *w= ((Slave_job_group *)
                      dynamic_array_ptr(&above_lwm_jobs, it_job))->worker;
    LOG_POS_COORD w_last= { const_cast<char*>(w->get_group_master_log_name()),
                            w->get_group_master_log_pos() };
    bool checksum_detected= FALSE;

    sql_print_information("Slave: MTS group recovery relay log info based on Worker-Id %lu, "
                          "group_relay_log_name %s, group_relay_log_pos %llu "
                          "group_master_log_name %s, group_master_log_pos %llu",
                          w->id,
                          w->get_group_relay_log_name(),
                          w->get_group_relay_log_pos(),
                          w->get_group_master_log_name(),
                          w->get_group_master_log_pos());

    recovery_group_cnt= 0;
    not_reached_commit= true;
    if (rli->relay_log.find_log_pos(&linfo, rli->get_group_relay_log_name(), 1))
    {
      error= TRUE;
      sql_print_error("Error looking for %s.", rli->get_group_relay_log_name());
      goto err;
    }
    offset= rli->get_group_relay_log_pos();
    for (int checking= 0 ; not_reached_commit; checking++)
    {
      if ((file= open_binlog_file(&log, linfo.log_file_name, &errmsg)) < 0)
      {
        error= TRUE;
        sql_print_error("%s", errmsg);
        goto err;
      }
      /*
        Looking for the actual relay checksum algorithm that is present in
        a FD at head events of the relay log.
      */
      if (!checksum_detected)
      {
        int i= 0;
        while (i < 4 && (ev= Log_event::read_log_event(&log,
                                                       (mysql_mutex_t*) 0,
                                                       p_fdle, 0, NULL)))
        {
          if (ev->get_type_code() == FORMAT_DESCRIPTION_EVENT)
          {
            p_fdle->checksum_alg= ev->checksum_alg;
            checksum_detected= TRUE;
          }
          delete ev;
          i++;
        }
        if (!checksum_detected)
        {
          error= TRUE;
          sql_print_error("%s", "malformed or very old relay log which "
                          "does not have FormatDescriptor");
          goto err;
        }
      }

      my_b_seek(&log, offset);

      while (not_reached_commit &&
             (ev= Log_event::read_log_event(&log, 0, p_fdle,
                                            opt_slave_sql_verify_checksum,
                                            NULL)))
      {
        DBUG_ASSERT(ev->is_valid());

        if (ev->get_type_code() == FORMAT_DESCRIPTION_EVENT)
          p_fdle->checksum_alg= ev->checksum_alg;

        if (ev->get_type_code() == ROTATE_EVENT ||
            ev->get_type_code() == FORMAT_DESCRIPTION_EVENT ||
            ev->get_type_code() == PREVIOUS_GTIDS_LOG_EVENT)
        {
          delete ev;
          ev= NULL;
          continue;
        }

        DBUG_PRINT("mts", ("Event Recoverying relay log info "
                   "group_mster_log_name %s, event_master_log_pos %llu type code %u.",
                   linfo.log_file_name, ev->log_pos, ev->get_type_code()));

        if (ev->starts_group())
        {
          flag_group_seen_begin= true;
        }
        else if ((ev->ends_group() || !flag_group_seen_begin) &&
                 !is_gtid_event(ev))
        {
          int ret= 0;
          LOG_POS_COORD ev_coord= { (char *) rli->get_group_master_log_name(),
                                      ev->log_pos };
          flag_group_seen_begin= false;
          recovery_group_cnt++;

          sql_print_information("Slave: MTS group recovery relay log info "
                                "group_master_log_name %s, "
                                "event_master_log_pos %llu.",
                                rli->get_group_master_log_name(), ev->log_pos);
          if ((ret= mts_event_coord_cmp(&ev_coord, &w_last)) == 0)
          {
#ifndef DBUG_OFF
            for (uint i= 0; i <= w->checkpoint_seqno; i++)
            {
              if (bitmap_is_set(&w->group_executed, i))
                DBUG_PRINT("mts", ("Bit %u is set.", i));
              else
                DBUG_PRINT("mts", ("Bit %u is not set.", i));
            }
#endif
            DBUG_PRINT("mts",
                       ("Doing a shift ini(%lu) end(%lu).",
                       (w->checkpoint_seqno + 1) - recovery_group_cnt,
                        w->checkpoint_seqno));

            for (uint i= (w->checkpoint_seqno + 1) - recovery_group_cnt,
                 j= 0; i <= w->checkpoint_seqno; i++, j++)
            {
              if (bitmap_is_set(&w->group_executed, i))
              {
                DBUG_PRINT("mts", ("Setting bit %u.", j));
                bitmap_fast_test_and_set(groups, j);
              }
            }
            not_reached_commit= false;
          }
          else
            DBUG_ASSERT(ret < 0);
        }
        delete ev;
        ev= NULL;
      }
      end_io_cache(&log);
      mysql_file_close(file, MYF(MY_WME));
      offset= BIN_LOG_HEADER_SIZE;
      if (not_reached_commit && rli->relay_log.find_next_log(&linfo, 1))
      {
         error= TRUE;
         sql_print_error("Error looking for file after %s.", linfo.log_file_name);
         goto err;
      }
    }

    rli->mts_recovery_group_cnt= (rli->mts_recovery_group_cnt < recovery_group_cnt ?
      recovery_group_cnt : rli->mts_recovery_group_cnt);
  }

  DBUG_ASSERT(!rli->recovery_groups_inited ||
              rli->mts_recovery_group_cnt <= groups->n_bits);

err:

  for (uint it_job= 0; it_job < above_lwm_jobs.elements; it_job++)
  {
    get_dynamic(&above_lwm_jobs, (uchar *) &job_worker, it_job);
    delete job_worker.worker;
  }

  delete_dynamic(&above_lwm_jobs);
  if (rli->mts_recovery_group_cnt == 0)
    rli->clear_mts_recovery_groups();

  DBUG_RETURN(error ? ER_MTS_RECOVERY_FAILURE : 0);
}

/**
   Processing rli->gaq to find out the low-water-mark (lwm) coordinates
   which is stored into the cental recovery table.

   @param rli            pointer to Relay-log-info of Coordinator
   @param period         period of processing GAQ, normally derived from
                         @c mts_checkpoint_period
   @param force          if TRUE then hang in a loop till some progress
   @param need_data_lock False if rli->data_lock mutex is aquired by
                         the caller.

   @return FALSE success, TRUE otherwise
*/
bool mts_checkpoint_routine(Relay_log_info *rli, ulonglong period,
                            bool force, bool need_data_lock)
{
  ulong cnt;
  bool error= FALSE;
  struct timespec curr_clock;

  DBUG_ENTER("checkpoint_routine");

#ifndef DBUG_OFF
  if (DBUG_EVALUATE_IF("check_slave_debug_group", 1, 0))
  {
    if (!rli->gaq->count_done(rli))
      DBUG_RETURN(FALSE);
  }
#endif

  /*
    rli->checkpoint_group can have two possible values due to
    two possible status of the last (being scheduled) group.
  */
  DBUG_ASSERT(!rli->gaq->full() ||
              ((rli->checkpoint_seqno == rli->checkpoint_group -1 &&
                rli->mts_group_status == Relay_log_info::MTS_IN_GROUP) ||
               rli->checkpoint_seqno == rli->checkpoint_group));

  /*
    Currently, the checkpoint routine is being called by the SQL Thread.
    For that reason, this function is called call from appropriate points
    in the SQL Thread's execution path and the elapsed time is calculated
    here to check if it is time to execute it.
  */
  set_timespec_nsec(curr_clock, 0);
  ulonglong diff= diff_timespec(curr_clock, rli->last_clock);
  if (!force && diff < period)
  {
    /*
      We do not need to execute the checkpoint now because
      the time elapsed is not enough.
    */
    DBUG_RETURN(FALSE);
  }

  do
  {
    cnt= rli->gaq->move_queue_head(&rli->workers);
#ifndef DBUG_OFF
    if (DBUG_EVALUATE_IF("check_slave_debug_group", 1, 0) &&
        cnt != opt_mts_checkpoint_period)
      sql_print_error("This an error cnt != mts_checkpoint_period");
#endif
  } while (!sql_slave_killed(rli->info_thd, rli) &&
           cnt == 0 && force &&
           !DBUG_EVALUATE_IF("check_slave_debug_group", 1, 0) &&
           (my_sleep(rli->mts_coordinator_basic_nap), 1));
  /*
    This checks how many consecutive jobs where processed.
    If this value is different than zero the checkpoint
    routine can proceed. Otherwise, there is nothing to be
    done.
  */
  if (cnt == 0)
    goto end;

  // case: rebalance workers should be called only when the current event
  // in the coordinator is a begin or gtid event
  if (!force && opt_mts_dynamic_rebalance == TRUE &&
      !rli->mts_dependency_replication &&
      !rli->curr_group_seen_begin && !rli->curr_group_seen_gtid &&
      !rli->sql_thread_kill_accepted)
  {
    rebalance_workers(rli);
  }

  if (!rli->mts_dependency_replication)
  {
    /* TODO:
       to turn the least occupied selection in terms of jobs pieces
    */
    for (uint i= 0; i < rli->workers.elements; i++)
    {
      Slave_worker *w_i;
      get_dynamic(&rli->workers, (uchar *) &w_i, i);
      set_dynamic(&rli->least_occupied_workers,
                  (uchar*) &w_i->jobs.len, w_i->id);
    };
    sort_dynamic(&rli->least_occupied_workers, (qsort_cmp) ulong_cmp);

    if (DBUG_EVALUATE_IF("skip_checkpoint_load_reset", 0, 1))
    {
      // reset the database load
      mysql_mutex_lock(&slave_worker_hash_lock);
      for (uint i= 0; i < mapping_db_to_worker.records; ++i)
      {
        db_worker_hash_entry *entry=
          (db_worker_hash_entry*) my_hash_element(&mapping_db_to_worker, i);
        entry->load= 0;
      }
      mysql_mutex_unlock(&slave_worker_hash_lock);
    }
  }

  if (need_data_lock)
    mysql_mutex_lock(&rli->data_lock);
  else
    mysql_mutex_assert_owner(&rli->data_lock);

  /*
    "Coordinator::commit_positions" {

    rli->gaq->lwm has been updated in move_queue_head() and
    to contain all but rli->group_master_log_name which
    is altered solely by Coordinator at special checkpoints.
  */
  rli->set_group_master_log_pos(rli->gaq->lwm.group_master_log_pos);
  rli->set_group_relay_log_pos(rli->gaq->lwm.group_relay_log_pos);
  DBUG_PRINT("mts", ("New checkpoint %llu %llu %s",
             rli->gaq->lwm.group_master_log_pos,
             rli->gaq->lwm.group_relay_log_pos,
             rli->gaq->lwm.group_relay_log_name));

  if (rli->gaq->lwm.group_relay_log_name[0] != 0)
    rli->set_group_relay_log_name(rli->gaq->lwm.group_relay_log_name);

  /*
     todo: uncomment notifies when UNTIL will be supported

     rli->notify_group_master_log_name_update();
     rli->notify_group_relay_log_name_update();

     Todo: optimize with if (wait_flag) broadcast
         waiter: set wait_flag; waits....; drops wait_flag;
  */

  if (gtid_mode != GTID_MODE_ON)
    error= rli->flush_info(TRUE);

  mysql_cond_broadcast(&rli->data_cond);
  if (need_data_lock)
    mysql_mutex_unlock(&rli->data_lock);

  /*
    We need to ensure that this is never called at this point when
    cnt is zero. This value means that the checkpoint information
    will be completely reset.
  */
  rli->reset_notified_checkpoint(cnt, rli->gaq->lwm.ts, rli->gaq->lwm.ts_millis,
                                 need_data_lock);

  /* end-of "Coordinator::"commit_positions" */

end:
#ifndef DBUG_OFF
  if (DBUG_EVALUATE_IF("check_slave_debug_group", 1, 0))
    DBUG_SUICIDE();
#endif
  set_timespec_nsec(rli->last_clock, 0);

  DBUG_RETURN(error);
}

/**
   Instantiation of a Slave_worker and forking out a single Worker thread.

   @param  rli  Coordinator's Relay_log_info pointer
   @param  i    identifier of the Worker

   @return 0 suppress or 1 if fails
*/
int slave_start_single_worker(Relay_log_info *rli, ulong i)
{
  int error= 0;
  pthread_t th;
  Slave_worker *w= NULL;

  mysql_mutex_assert_owner(&rli->run_lock);

  if (!(w=
        Rpl_info_factory::create_worker(opt_rli_repository_id, i, rli, false)))
  {
    sql_print_error("Failed during slave worker thread create");
    error= 1;
    goto err;
  }

  if (w->init_worker(rli, i))
  {
    sql_print_error("Failed during slave worker thread create");
    error= 1;
    goto err;
  }
  set_dynamic(&rli->workers, (uchar*) &w, i);

  if (DBUG_EVALUATE_IF("mts_worker_thread_fails", i == 1, 0) ||
      (error= mysql_thread_create(key_thread_slave_worker, &th,
                                  &connection_attrib,
                                  handle_slave_worker, (void*) w)))
  {
    sql_print_error("Failed during slave worker thread create (errno= %d)",
                    error);
    error= 1;
    goto err;
  }

  mysql_mutex_lock(&w->jobs_lock);
  if (w->running_status == Slave_worker::NOT_RUNNING)
    mysql_cond_wait(&w->jobs_cond, &w->jobs_lock);
  mysql_mutex_unlock(&w->jobs_lock);
  // Least occupied inited with zero
  insert_dynamic(&rli->least_occupied_workers, (uchar*) &w->jobs.len);

err:
  if (error && w)
  {
    delete w;
    /*
      Any failure after dynarray inserted must follow with deletion
      of just created item.
    */
    if (rli->workers.elements == i + 1)
      delete_dynamic_element(&rli->workers, i);
  }
  return error;
}

/**
   Initialization of the central rli members for Coordinator's role,
   communication channels such as Assigned Partition Hash (APH),
   and starting the Worker pool.

   @param  n   Number of configured Workers in the upcoming session.

   @return 0         success
           non-zero  as failure
*/
int slave_start_workers(Relay_log_info *rli, ulong n, bool *mts_inited)
{
  uint i;
  int error= 0;

  mysql_mutex_assert_owner(&rli->run_lock);

  if (n == 0 && rli->mts_recovery_group_cnt == 0)
  {
    reset_dynamic(&rli->workers);
    goto end;
  }

  *mts_inited= true;

  /*
    The requested through argument number of Workers can be different
     from the previous time which ended with an error. Thereby
     the effective number of configured Workers is max of the two.
  */
  rli->init_workers(max(n, rli->recovery_parallel_workers));

  // CGAP dynarray holds id:s of partitions of the Current being executed Group
  my_init_dynamic_array(&rli->curr_group_assigned_parts,
                        sizeof(db_worker_hash_entry*),
                        SLAVE_INIT_DBS_IN_GROUP, 1);
  rli->last_assigned_worker= NULL;     // associated with curr_group_assigned
  my_init_dynamic_array(&rli->curr_group_da, sizeof(Log_event*), 8, 2);
  // Least_occupied_workers array to hold items size of Slave_jobs_queue::len
  my_init_dynamic_array(&rli->least_occupied_workers, sizeof(ulong), n, 0);

  /*
     GAQ  queue holds seqno:s of scheduled groups. C polls workers in
     @c opt_mts_checkpoint_period to update GAQ (see @c next_event())
     The length of GAQ is set to be equal to checkpoint_group.
     Notice, the size matters for mts_checkpoint_routine's progress loop.
  */

  rli->gaq= new Slave_committed_queue(rli->get_group_master_log_name(),
                                      sizeof(Slave_job_group),
                                      rli->checkpoint_group, n);
  if (!rli->gaq->inited)
    return 1;

  // length of WQ is actually constant though can be made configurable
  rli->mts_slave_worker_queue_len_max= mts_slave_worker_queue_len_max;
  rli->mts_pending_jobs_size= 0;
  rli->mts_pending_jobs_size_max= ::opt_mts_pending_jobs_size_max;
  rli->mts_wq_underrun_w_id= MTS_WORKER_UNDEF;
  rli->mts_wq_excess_cnt= 0;
  rli->mts_wq_overrun_cnt= 0;
  rli->mts_wq_oversize= FALSE;
  rli->mts_coordinator_basic_nap= mts_coordinator_basic_nap;
  rli->mts_worker_underrun_level= mts_worker_underrun_level;
  rli->curr_group_seen_begin= rli->curr_group_seen_gtid= false;
  rli->curr_group_seen_metadata= false;
  rli->curr_group_isolated= FALSE;
  rli->checkpoint_seqno= 0;
  rli->mts_last_online_stat= my_time(0);
  rli->mts_group_status= Relay_log_info::MTS_NOT_IN_GROUP;

  if (init_hash_workers(n))  // MTS: mapping_db_to_worker
  {
    sql_print_error("Failed to init partitions hash");
    error= 1;
    goto err;
  }

  for (i= 0; i < n; i++)
  {
    if ((error= slave_start_single_worker(rli, i)))
      goto err;
  }

end:
  rli->slave_parallel_workers= n;
  // Effective end of the recovery right now when there is no gaps
  if (!error && rli->mts_recovery_group_cnt == 0)
  {
    if ((error= rli->mts_finalize_recovery()))
      (void) Rpl_info_factory::reset_workers(rli);
    if (!error)
      error= rli->flush_info(TRUE);
  }

err:
  return error;
}

/*
   Ending Worker threads.

   Not in case Coordinator is killed itself, it first waits for
   Workers have finished their assignements, and then updates checkpoint.
   Workers are notified with setting KILLED status
   and waited for their acknowledgment as specified by
   worker's running_status.
   Coordinator finalizes with its MTS running status to reset few objects.
*/
void slave_stop_workers(Relay_log_info *rli, bool *mts_inited)
{
  int i;
  THD *thd= rli->info_thd;
  if (!*mts_inited)
    return;
  else if (rli->slave_parallel_workers == 0)
    goto end;

  /*
    If request for stop slave is received notify worker
    to stop.
  */
  // Initialize worker exit count and max_updated_index to 0 during each stop.
  rli->exit_counter= 0;
  rli->max_updated_index= (rli->until_condition !=
                           Relay_log_info::UNTIL_NONE)?
                           rli->mts_groups_assigned:0;

  if (rli->mts_dependency_replication)
  {
    mysql_mutex_lock(&rli->dep_lock);
    // this will be used to determine if we need to deal with a worker that is
    // handling a partially enqueued trx
    bool partial= false;
    // case: until condition is specified, so we have to execute all
    // transactions in the queue
    if (unlikely(rli->until_condition != Relay_log_info::UNTIL_NONE))
    {
      // we have a partial trx if the coordinator has set the trx_queued flag,
      // this is because this flag is set to false after coordinator sees an end
      // event
      partial= rli->trx_queued;
    }
    // case: until condition is not specified
    else
    {
      // the partial check is slightly complicated in this case because we only
      // care about partial trx that has been pulled by a worker, since the
      // queue is going to be emptied next anyway, we make this check by
      // checking of the queue is empty
      partial= rli->dep_queue.empty() && rli->trx_queued;
      // let's cleanup, we can clear the queue in this case
      rli->clear_dep(false);
    }
    mysql_mutex_unlock(&rli->dep_lock);

    wait_for_dep_workers_to_finish(rli, partial);

    // case: if UNTIL is specified let's clean up after waiting for workers
    if (unlikely(rli->until_condition != Relay_log_info::UNTIL_NONE))
    {
      rli->clear_dep(true);
    }

    // set all workers as STOP_ACCEPTED, and signal blocked workers
    for (i= rli->workers.elements - 1; i >= 0; i--)
    {
      Slave_worker *w= NULL;
      get_dynamic((DYNAMIC_ARRAY*)&rli->workers, (uchar*) &w, i);
      mysql_mutex_lock(&w->jobs_lock);
      if (w->running_status != Slave_worker::RUNNING)
      {
        mysql_mutex_unlock(&w->jobs_lock);
        continue;
      }
      w->running_status= Slave_worker::STOP_ACCEPTED;

      // unblock workers waiting for new events or trxs
      mysql_mutex_lock(&w->info_thd->LOCK_thd_data);
      w->info_thd->awake(w->info_thd->killed);
      mysql_mutex_unlock(&w->info_thd->LOCK_thd_data);

      mysql_mutex_unlock(&w->jobs_lock);
    }

    thd_proc_info(thd, "Waiting for workers to exit");

    for (i= rli->workers.elements - 1; i >= 0; i--)
    {
      Slave_worker *w= NULL;
      get_dynamic((DYNAMIC_ARRAY*)&rli->workers, (uchar*) &w, i);
      mysql_mutex_lock(&w->jobs_lock);

      // wait for workers to stop running
      while (w->running_status != Slave_worker::NOT_RUNNING)
      {
        struct timespec abstime;
        set_timespec(abstime, 1);
        mysql_cond_timedwait(&w->jobs_cond, &w->jobs_lock, &abstime);
      }

      mysql_mutex_unlock(&w->jobs_lock);
    }
    rli->dependency_worker_error= false;
  }
  else
  {
    for (i= rli->workers.elements - 1; i >= 0; i--)
    {
      Slave_worker *w;
      struct slave_job_item item= {NULL}, *job_item= &item;
      get_dynamic((DYNAMIC_ARRAY*)&rli->workers, (uchar*) &w, i);
      mysql_mutex_lock(&w->jobs_lock);
      //Inform all workers to stop
      if (w->running_status != Slave_worker::RUNNING)
      {
        mysql_mutex_unlock(&w->jobs_lock);
        continue;
      }

      w->running_status= Slave_worker::STOP;
      (void) set_max_updated_index_on_stop(w, job_item, w->current_event_index);
      mysql_cond_signal(&w->jobs_cond);

      mysql_mutex_unlock(&w->jobs_lock);

      if (log_warnings > 1)
        sql_print_information("Notifying Worker %lu to exit, thd %p", w->id,
                              w->info_thd);
    }

    thd_proc_info(thd, "Waiting for workers to exit");

    for (i= rli->workers.elements - 1; i >= 0; i--)
    {
      Slave_worker *w= NULL;
      get_dynamic((DYNAMIC_ARRAY*)&rli->workers, (uchar*) &w, i);

      mysql_mutex_lock(&w->jobs_lock);
      while (w->running_status != Slave_worker::NOT_RUNNING)
      {
        PSI_stage_info old_stage;
        DBUG_ASSERT(w->running_status == Slave_worker::ERROR_LEAVING ||
                    w->running_status == Slave_worker::STOP ||
                    w->running_status == Slave_worker::STOP_ACCEPTED);

        thd->ENTER_COND(&w->jobs_cond, &w->jobs_lock,
                        &stage_slave_waiting_workers_to_exit, &old_stage);
        mysql_cond_wait(&w->jobs_cond, &w->jobs_lock);
        thd->EXIT_COND(&old_stage);
        mysql_mutex_lock(&w->jobs_lock);
      }
      mysql_mutex_unlock(&w->jobs_lock);
    }
  }

  if (thd->killed == THD::NOT_KILLED)
    (void) mts_checkpoint_routine(rli, 0, false, true/*need_data_lock=true*/); // TODO:consider to propagate an error out of the function

  for (i= rli->workers.elements - 1; i >= 0; i--)
  {
    Slave_worker *w= NULL;
    get_dynamic((DYNAMIC_ARRAY*)&rli->workers, (uchar*) &w, i);
    delete_dynamic_element(&rli->workers, i);
    delete w;
  }
  if (log_warnings > 1)
    sql_print_information("Total MTS session statistics: "
                          "events processed = %llu; "
                          "worker queues filled over overrun level = %lu; "
                          "waited due a Worker queue full = %lu; "
                          "waited due the total size = %lu; "
                          "slept when Workers occupied = %lu ",
                          rli->mts_events_assigned, rli->mts_wq_overrun_cnt,
                          rli->mts_wq_overfill_cnt, rli->wq_size_waits_cnt,
                          rli->mts_wq_no_underrun_cnt);

  DBUG_ASSERT(rli->pending_jobs == 0);
  DBUG_ASSERT(rli->mts_pending_jobs_size == 0);

end:
  rli->mts_group_status= Relay_log_info::MTS_NOT_IN_GROUP;
  destroy_hash_workers(rli);
  delete rli->gaq;
  delete_dynamic(&rli->least_occupied_workers);    // least occupied

  // Destroy buffered events of the current group prior to exit.
  for (uint i= 0; i < rli->curr_group_da.elements; i++)
    delete *(Log_event**) dynamic_array_ptr(&rli->curr_group_da, i);
  delete_dynamic(&rli->curr_group_da);             // GCDA

  delete_dynamic(&rli->curr_group_assigned_parts); // GCAP
  rli->deinit_workers();
  rli->slave_parallel_workers= 0;
  *mts_inited= false;
}


/**
  Slave SQL thread entry point.

  @param arg Pointer to Relay_log_info object that holds information
  for the SQL thread.

  @return Always 0.
*/
pthread_handler_t handle_slave_sql(void *arg)
{
  THD *thd;                     /* needs to be first for thread_stack */
  bool thd_added= false;
  char llbuff[22],llbuff1[22];
  char saved_log_name[FN_REFLEN];
  char saved_master_log_name[FN_REFLEN];
  my_off_t saved_log_pos= 0;
  my_off_t saved_master_log_pos= 0;
  my_off_t saved_skip= 0;

  Relay_log_info* rli = ((Master_info*)arg)->rli;
  const char *errmsg;
  bool mts_inited= false;
  Commit_order_manager *commit_order_mngr= NULL;

  /* Buffer lifetime extends across the entire runtime of the THD handle. */
  static char proc_info_buf[256]= {0};

  // needs to call my_thread_init(), otherwise we get a coredump in DBUG_ stuff
  my_thread_init();
  DBUG_ENTER("handle_slave_sql");

  DBUG_ASSERT(rli->inited);
  mysql_mutex_lock(&rli->run_lock);
  DBUG_ASSERT(!rli->slave_running);
  errmsg= 0;
#ifndef DBUG_OFF
  rli->events_until_exit = abort_slave_event_count;
#endif

  // note that contructor of THD uses DBUG_ !
  thd = new THD_SQL_slave(proc_info_buf, sizeof(proc_info_buf));

  thd->thread_stack = (char*)&thd; // remember where our stack is
  mysql_mutex_lock(&rli->info_thd_lock);
  rli->info_thd= thd;
  mysql_mutex_unlock(&rli->info_thd_lock);

  rli->mts_dependency_replication= opt_mts_dependency_replication;
  rli->mts_dependency_size= opt_mts_dependency_size;
  rli->mts_dependency_refill_threshold= opt_mts_dependency_refill_threshold;
  rli->mts_dependency_max_keys= opt_mts_dependency_max_keys;
  rli->mts_dependency_order_commits= opt_mts_dependency_order_commits;
  rli->mts_dependency_cond_wait_timeout= opt_mts_dependency_cond_wait_timeout;

  if (rli->mts_dependency_replication &&
      !slave_use_idempotent_for_recovery_options)
  {
    sql_print_error("mts_dependency_replication is enabled but "
        "slave_use_idempotent_for_recovery is disabled. The slave is not crash "
        "safe! Please enable slave_use_idempotent_for_recovery for crash "
        "safety.");
  }

  rli->info_thd->variables.tx_isolation=
    static_cast<enum_tx_isolation>(slave_tx_isolation);

  if (rli->mts_dependency_order_commits && rli->mts_dependency_replication &&
      rli->opt_slave_parallel_workers > 0 &&
      opt_bin_log && opt_log_slave_updates)
    commit_order_mngr=
      new Commit_order_manager(rli->opt_slave_parallel_workers);

  rli->set_commit_order_manager(commit_order_mngr);

  /* Inform waiting threads that slave has started */
  rli->slave_run_id++;
  rli->slave_running = 1;
  rli->rbr_idempotent_tables.clear();
  if (opt_rbr_idempotent_tables)
    rli->rbr_idempotent_tables = split_into_set(opt_rbr_idempotent_tables, ',');

  if (opt_rbr_column_type_mismatch_whitelist)
  {
    const auto& list=
      split_into_set(opt_rbr_column_type_mismatch_whitelist, ',');
    rli->set_rbr_column_type_mismatch_whitelist(list);
  }
  else
    rli->set_rbr_column_type_mismatch_whitelist(
        std::unordered_set<std::string>());

  rli->reported_unsafe_warning= false;
  rli->sql_thread_kill_accepted= false;

  pthread_detach_this_thread();
  if (init_slave_thread(thd, SLAVE_THD_SQL))
  {
    /*
      TODO: this is currently broken - slave start and change master
      will be stuck if we fail here
    */
    mysql_cond_broadcast(&rli->start_cond);
    mysql_mutex_unlock(&rli->run_lock);
    rli->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR,
                "Failed during slave thread initialization");
    goto err;
  }
  thd->init_for_queries(rli);
  thd->temporary_tables = rli->save_temporary_tables; // restore temp tables
  set_thd_in_use_temporary_tables(rli);   // (re)set sql_thd in use for saved temp tables

  mutex_lock_shard(SHARDED(&LOCK_thread_count), thd);
  add_global_thread(thd);
  thd_added= true;
  mutex_unlock_shard(SHARDED(&LOCK_thread_count), thd);

  /* MTS: starting the worker pool */
  if (slave_start_workers(rli, rli->opt_slave_parallel_workers, &mts_inited) != 0)
  {
    mysql_cond_broadcast(&rli->start_cond);
    mysql_mutex_unlock(&rli->run_lock);
    rli->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR,
                "Failed during slave workers initialization");
    goto err;
  }
  if (Rpl_info_factory::init_gtid_info_repository(rli))
  {
    mysql_cond_broadcast(&rli->start_cond);
    mysql_mutex_unlock(&rli->run_lock);
    rli->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR,
                "Error creating gtid_info");
    goto err;
  }
  /*
    We are going to set slave_running to 1. Assuming slave I/O thread is
    alive and connected, this is going to make Seconds_Behind_Master be 0
    i.e. "caught up". Even if we're just at start of thread. Well it's ok, at
    the moment we start we can think we are caught up, and the next second we
    start receiving data so we realize we are not caught up and
    Seconds_Behind_Master grows. No big deal.
  */
  rli->abort_slave = 0;

  /*
    Reset errors for a clean start (otherwise, if the master is idle, the SQL
    thread may execute no Query_log_event, so the error will remain even
    though there's no problem anymore). Do not reset the master timestamp
    (imagine the slave has caught everything, the STOP SLAVE and START SLAVE:
    as we are not sure that we are going to receive a query, we want to
    remember the last master timestamp (to say how many seconds behind we are
    now.
    But the master timestamp is reset by RESET SLAVE & CHANGE MASTER.
  */
  rli->clear_error();

  if (rli->update_is_transactional())
  {
    mysql_cond_broadcast(&rli->start_cond);
    mysql_mutex_unlock(&rli->run_lock);
    rli->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR,
                "Error checking if the relay log repository is transactional.");
    goto err;
  }

  if (!rli->is_transactional())
    rli->report(WARNING_LEVEL, 0,
    "If a crash happens this configuration does not guarantee that the relay "
    "log info will be consistent");

  mysql_mutex_unlock(&rli->run_lock);
  mysql_cond_broadcast(&rli->start_cond);

  DEBUG_SYNC(thd, "after_start_slave");

  //tell the I/O thread to take relay_log_space_limit into account from now on
  mysql_mutex_lock(&rli->log_space_lock);
  rli->ignore_log_space_limit= 0;
  mysql_mutex_unlock(&rli->log_space_lock);
  rli->trans_retries= 0; // start from "no error"
  DBUG_PRINT("info", ("rli->trans_retries: %lu", rli->trans_retries));

  if (rli->init_relay_log_pos(rli->get_group_relay_log_name(),
                              rli->get_group_relay_log_pos(),
                              true/*need_data_lock=true*/, &errmsg,
                              1 /*look for a description_event*/))
  {
    rli->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR,
                "Error initializing relay log position: %s", errmsg);
    goto err;
  }
  THD_CHECK_SENTRY(thd);
#ifndef DBUG_OFF
  {
    char llbuf1[22], llbuf2[22];
    DBUG_PRINT("info", ("my_b_tell(rli->cur_log)=%s rli->event_relay_log_pos=%s",
                        llstr(my_b_tell(rli->cur_log),llbuf1),
                        llstr(rli->get_event_relay_log_pos(),llbuf2)));
    DBUG_ASSERT(rli->get_event_relay_log_pos() >= BIN_LOG_HEADER_SIZE);
    /*
      Wonder if this is correct. I (Guilhem) wonder if my_b_tell() returns the
      correct position when it's called just after my_b_seek() (the questionable
      stuff is those "seek is done on next read" comments in the my_b_seek()
      source code).
      The crude reality is that this assertion randomly fails whereas
      replication seems to work fine. And there is no easy explanation why it
      fails (as we my_b_seek(rli->event_relay_log_pos) at the very end of
      init_relay_log_pos() called above). Maybe the assertion would be
      meaningful if we held rli->data_lock between the my_b_seek() and the
      DBUG_ASSERT().
    */
#ifdef SHOULD_BE_CHECKED
    DBUG_ASSERT(my_b_tell(rli->cur_log) == rli->get_event_relay_log_pos());
#endif
  }
#endif
  DBUG_ASSERT(rli->info_thd == thd);

#ifdef WITH_NDBCLUSTER_STORAGE_ENGINE
  /* engine specific hook, to be made generic */
  if (ndb_wait_setup_func && ndb_wait_setup_func(opt_ndb_wait_setup))
  {
    sql_print_warning("Slave SQL thread : NDB : Tables not available after %lu"
                      " seconds.  Consider increasing --ndb-wait-setup value",
                      opt_ndb_wait_setup);
  }
#endif

  DBUG_PRINT("master_info",("log_file_name: %s  position: %s",
                            rli->get_group_master_log_name(),
                            llstr(rli->get_group_master_log_pos(),llbuff)));
  if (log_warnings)
    sql_print_information("Slave SQL thread initialized, starting replication in \
log '%s' at position %s, relay log '%s' position: %s", rli->get_rpl_log_name(),
                    llstr(rli->get_group_master_log_pos(),llbuff),rli->get_group_relay_log_name(),
                    llstr(rli->get_group_relay_log_pos(),llbuff1));

  if (check_temp_dir(rli->slave_patternload_file))
  {
    rli->report(ERROR_LEVEL, thd->get_stmt_da()->sql_errno(),
                "Unable to use slave's temporary directory %s - %s",
                slave_load_tmpdir, thd->get_stmt_da()->message());
    goto err;
  }

  /* execute init_slave variable */
  if (opt_init_slave.length)
  {
    execute_init_command(thd, &opt_init_slave, &LOCK_sys_init_slave);
    if (thd->is_slave_error)
    {
      rli->report(ERROR_LEVEL, thd->get_stmt_da()->sql_errno(),
                  "Slave SQL thread aborted. Can't execute init_slave query");
      goto err;
    }
  }

  /*
    First check until condition - probably there is nothing to execute. We
    do not want to wait for next event in this case.
  */
  mysql_mutex_lock(&rli->data_lock);
  if (rli->slave_skip_counter)
  {
    strmake(saved_log_name, rli->get_group_relay_log_name(), FN_REFLEN - 1);
    strmake(saved_master_log_name, rli->get_group_master_log_name(), FN_REFLEN - 1);
    saved_log_pos= rli->get_group_relay_log_pos();
    saved_master_log_pos= rli->get_group_master_log_pos();
    saved_skip= rli->slave_skip_counter;
  }
  if (rli->until_condition != Relay_log_info::UNTIL_NONE &&
      rli->is_until_satisfied(thd, NULL))
  {
    mysql_mutex_unlock(&rli->data_lock);
    goto err;
  }
  mysql_mutex_unlock(&rli->data_lock);

  /* Read queries from the IO/THREAD until this thread is killed */

  while (!sql_slave_killed(thd,rli))
  {
    THD_STAGE_INFO(thd, stage_reading_event_from_the_relay_log);
    DBUG_ASSERT(rli->info_thd == thd);
    THD_CHECK_SENTRY(thd);

    if (saved_skip && rli->slave_skip_counter == 0)
    {
      sql_print_information("'SQL_SLAVE_SKIP_COUNTER=%ld' executed at "
        "relay_log_file='%s', relay_log_pos='%ld', master_log_name='%s', "
        "master_log_pos='%ld' and new position at "
        "relay_log_file='%s', relay_log_pos='%ld', master_log_name='%s', "
        "master_log_pos='%ld' ",
        (ulong) saved_skip, saved_log_name, (ulong) saved_log_pos,
        saved_master_log_name, (ulong) saved_master_log_pos,
        rli->get_group_relay_log_name(), (ulong) rli->get_group_relay_log_pos(),
        rli->get_group_master_log_name(), (ulong) rli->get_group_master_log_pos());
      saved_skip= 0;
    }

    long lag= (!rli->penultimate_master_timestamp ||
      ((reset_seconds_behind_master && (rli->mi->get_master_log_pos() ==
      rli->get_group_master_log_pos()) && (!strcmp(
      rli->mi->get_master_log_name(), rli->get_group_master_log_name()))) ||
      rli->slave_has_caughtup == Enum_slave_caughtup::YES)) ? 0 :
        max(0L, ((long)(time(0) - rli->penultimate_master_timestamp)
                  - rli->mi->clock_diff_with_master));
    // unique key checks are allowed to be disabled only with row format
    if (unique_check_lag_threshold > 0 && lag > unique_check_lag_threshold
        && thd->variables.binlog_format == BINLOG_FORMAT_ROW)
      rli->skip_unique_check= true;
    if (lag < unique_check_lag_reset_threshold)
      rli->skip_unique_check= false;

    if (exec_relay_log_event(thd,rli))
    {
      DBUG_PRINT("info", ("exec_relay_log_event() failed"));
      // do not scare the user if SQL thread was simply killed or stopped
      if (!sql_slave_killed(thd,rli))
      {
        /*
          retrieve as much info as possible from the thd and, error
          codes and warnings and print this to the error log as to
          allow the user to locate the error
        */
        uint32 const last_errno= rli->last_error().number;

        if (thd->is_error())
        {
          char const *const errmsg= thd->get_stmt_da()->message();

          DBUG_PRINT("info",
                     ("thd->get_stmt_da()->sql_errno()=%d; "
                      "rli->last_error.number=%d",
                      thd->get_stmt_da()->sql_errno(), last_errno));
          if (last_errno == 0)
          {
            /*
 	      This function is reporting an error which was not reported
 	      while executing exec_relay_log_event().
 	    */
            rli->report(ERROR_LEVEL, thd->get_stmt_da()->sql_errno(),
                        "%s", errmsg);
          }
          else if (last_errno != thd->get_stmt_da()->sql_errno())
          {
            /*
             * An error was reported while executing exec_relay_log_event()
             * however the error code differs from what is in the thread.
             * This function prints out more information to help finding
             * what caused the problem.
             */
            sql_print_error("Slave (additional info): %s Error_code: %d",
                            errmsg, thd->get_stmt_da()->sql_errno());
          }
        }

        /* Print any warnings issued */
        Diagnostics_area::Sql_condition_iterator it=
          thd->get_stmt_da()->sql_conditions();
        const Sql_condition *err;
        /*
          Added controlled slave thread cancel for replication
          of user-defined variables.
        */
        bool udf_error = false;
        while ((err= it++))
        {
          if (err->get_sql_errno() == ER_CANT_OPEN_LIBRARY)
            udf_error = true;
          sql_print_warning("Slave: %s Error_code: %d", err->get_message_text(), err->get_sql_errno());
        }
        if (udf_error)
          sql_print_error("Error loading user-defined library, slave SQL "
            "thread aborted. Install the missing library, and restart the "
            "slave SQL thread with \"SLAVE START\". We stopped at log '%s' "
            "position %s", rli->get_rpl_log_name(),
            llstr(rli->get_group_master_log_pos(), llbuff));
        else
          sql_print_error("\
Error running query, slave SQL thread aborted. Fix the problem, and restart \
the slave SQL thread with \"SLAVE START\". We stopped at log \
'%s' position %s", rli->get_rpl_log_name(),
llstr(rli->get_group_master_log_pos(), llbuff));
      }
      goto err;
    }
  }

  /* Thread stopped. Print the current replication position to the log */
  sql_print_information("Slave SQL thread exiting, replication stopped in log "
                        "'%s' at position %s",
                        rli->get_rpl_log_name(),
                        llstr(rli->get_group_master_log_pos(), llbuff));

 err:

  slave_stop_workers(rli, &mts_inited); // stopping worker pool
  rli->clear_mts_recovery_groups();

  /*
    Some events set some playgrounds, which won't be cleared because thread
    stops. Stopping of this thread may not be known to these events ("stop"
    request is detected only by the present function, not by events), so we
    must "proactively" clear playgrounds:
  */
  thd->clear_error();
  rli->cleanup_context(thd, 1);
  /*
    Some extra safety, which should not been needed (normally, event deletion
    should already have done these assignments (each event which sets these
    variables is supposed to set them to 0 before terminating)).
  */
  thd->catalog= 0;
  thd->reset_query();
  thd->reset_db(NULL, 0);

  DBUG_ASSERT(rli->num_workers_waiting == 0 && rli->num_in_flight_trx == 0);

  THD_STAGE_INFO(thd, stage_waiting_for_slave_mutex_on_exit);
  mysql_mutex_lock(&rli->run_lock);
  /* We need data_lock, at least to wake up any waiting master_pos_wait() */
  mysql_mutex_lock(&rli->data_lock);
  DBUG_ASSERT(rli->slave_running == 1); // tracking buffer overrun
  /* When master_pos_wait() wakes up it will check this and terminate */
  rli->slave_running= 0;
  /* Forget the relay log's format */
  rli->set_rli_description_event(NULL);
  /* Wake up master_pos_wait() */
  mysql_mutex_unlock(&rli->data_lock);
  DBUG_PRINT("info",("Signaling possibly waiting master_pos_wait() functions"));
  mysql_cond_broadcast(&rli->data_cond);
  rli->ignore_log_space_limit= 0; /* don't need any lock */
  /* we die so won't remember charset - re-update them on next thread start */
  rli->cached_charset_invalidate();
  rli->save_temporary_tables = thd->temporary_tables;

  /*
    TODO: see if we can do this conditionally in next_event() instead
    to avoid unneeded position re-init
  */
  thd->temporary_tables = 0; // remove tempation from destructor to close them
  NET* net = thd->get_net();
  DBUG_ASSERT(net->buff != 0);
  net_end(net); // destructor will not free it, because we are weird
  DBUG_ASSERT(rli->info_thd == thd);
  THD_CHECK_SENTRY(thd);
  mysql_mutex_lock(&rli->info_thd_lock);
  rli->info_thd= 0;
  if (commit_order_mngr)
  {
    delete commit_order_mngr;
    rli->set_commit_order_manager(NULL);
  }
  mysql_mutex_unlock(&rli->info_thd_lock);
  set_thd_in_use_temporary_tables(rli);  // (re)set info_thd in use for saved temp tables

  thd->release_resources();
  THD_CHECK_SENTRY(thd);
  if (thd_added)
    remove_global_thread(thd);
  delete thd;
 /*
  Note: the order of the broadcast and unlock calls below (first broadcast, then unlock)
  is important. Otherwise a killer_thread can execute between the calls and
  delete the mi structure leading to a crash! (see BUG#25306 for details)
 */
  mysql_cond_broadcast(&rli->stop_cond);
  DBUG_EXECUTE_IF("simulate_slave_delay_at_terminate_bug38694", sleep(5););
  mysql_mutex_unlock(&rli->run_lock);  // tell the world we are done

  DBUG_LEAVE;                            // Must match DBUG_ENTER()
  my_thread_end();
  ERR_remove_state(0);
  pthread_exit(0);
  return 0;                             // Avoid compiler warnings
}

/*
  process_io_create_file()
*/

static int process_io_create_file(Master_info* mi, Create_file_log_event* cev)
{
  int error = 1;
  ulong num_bytes;
  bool cev_not_written;
  THD *thd = mi->info_thd;
  NET *net = &mi->mysql->net;
  DBUG_ENTER("process_io_create_file");

  mysql_mutex_assert_owner(&mi->data_lock);

  if (unlikely(!cev->is_valid()))
    DBUG_RETURN(1);

  if (!rpl_filter->db_ok(cev->db))
  {
    skip_load_data_infile(net);
    DBUG_RETURN(0);
  }
  DBUG_ASSERT(cev->inited_from_old);
  thd->file_id = cev->file_id = mi->file_id++;
  thd->server_id = cev->server_id;
  cev_not_written = 1;

  if (unlikely(net_request_file(net,cev->fname)))
  {
    sql_print_error("Slave I/O: failed requesting download of '%s'",
                    cev->fname);
    goto err;
  }

  /*
    This dummy block is so we could instantiate Append_block_log_event
    once and then modify it slightly instead of doing it multiple times
    in the loop
  */
  {
    Append_block_log_event aev(thd,0,0,0,0);

    for (;;)
    {
      if (unlikely((num_bytes=my_net_read(net)) == packet_error))
      {
        sql_print_error("Network read error downloading '%s' from master",
                        cev->fname);
        goto err;
      }
      if (unlikely(!num_bytes)) /* eof */
      {
	/* 3.23 master wants it */
        net_write_command(net, 0, (uchar*) "", 0, (uchar*) "", 0);
        /*
          If we wrote Create_file_log_event, then we need to write
          Execute_load_log_event. If we did not write Create_file_log_event,
          then this is an empty file and we can just do as if the LOAD DATA
          INFILE had not existed, i.e. write nothing.
        */
        if (unlikely(cev_not_written))
          break;
        Execute_load_log_event xev(thd,0,0);
        xev.log_pos = cev->log_pos;
        if (unlikely(mi->rli->relay_log.append_event(&xev, mi) != 0))
        {
          mi->report(ERROR_LEVEL, ER_SLAVE_RELAY_LOG_WRITE_FAILURE,
                     ER(ER_SLAVE_RELAY_LOG_WRITE_FAILURE),
                     "error writing Exec_load event to relay log");
          goto err;
        }
        mi->rli->relay_log.harvest_bytes_written(mi->rli, true/*need_log_space_lock=true*/);
        break;
      }
      if (unlikely(cev_not_written))
      {
        cev->block = net->read_pos;
        cev->block_len = num_bytes;
        if (unlikely(mi->rli->relay_log.append_event(cev, mi) != 0))
        {
          mi->report(ERROR_LEVEL, ER_SLAVE_RELAY_LOG_WRITE_FAILURE,
                     ER(ER_SLAVE_RELAY_LOG_WRITE_FAILURE),
                     "error writing Create_file event to relay log");
          goto err;
        }
        cev_not_written=0;
        mi->rli->relay_log.harvest_bytes_written(mi->rli, true/*need_log_space_lock=true*/);
      }
      else
      {
        aev.block = net->read_pos;
        aev.block_len = num_bytes;
        aev.log_pos = cev->log_pos;
        if (unlikely(mi->rli->relay_log.append_event(&aev, mi) != 0))
        {
          mi->report(ERROR_LEVEL, ER_SLAVE_RELAY_LOG_WRITE_FAILURE,
                     ER(ER_SLAVE_RELAY_LOG_WRITE_FAILURE),
                     "error writing Append_block event to relay log");
          goto err;
        }
        mi->rli->relay_log.harvest_bytes_written(mi->rli, true/*need_log_space_lock=true*/);
      }
    }
  }
  error=0;
err:
  DBUG_RETURN(error);
}


/**
  Used by the slave IO thread when it receives a rotate event from the
  master.

  Updates the master info with the place in the next binary log where
  we should start reading.  Rotate the relay log to avoid mixed-format
  relay logs.

  @param mi master_info for the slave
  @param rev The rotate log event read from the master

  @note The caller must hold mi->data_lock before invoking this function.

  @retval 0 ok
  @retval 1 error
*/
static int process_io_rotate(Master_info *mi, Rotate_log_event *rev)
{
  DBUG_ENTER("process_io_rotate");
  mysql_mutex_assert_owner(&mi->data_lock);

  if (unlikely(!rev->is_valid()))
    DBUG_RETURN(1);

  /* Safe copy as 'rev' has been "sanitized" in Rotate_log_event's ctor */
  memcpy(const_cast<char *>(mi->get_master_log_name()),
         rev->new_log_ident, rev->ident_len + 1);
  mi->set_master_log_pos(rev->pos);
  DBUG_PRINT("info", ("new (master_log_name, master_log_pos): ('%s', %lu)",
                      mi->get_master_log_name(), (ulong) mi->get_master_log_pos()));
#ifndef DBUG_OFF
  /*
    If we do not do this, we will be getting the first
    rotate event forever, so we need to not disconnect after one.
  */
  if (disconnect_slave_event_count)
    mi->events_until_exit++;
#endif

  /*
    If mi_description_event is format <4, there is conversion in the
    relay log to the slave's format (4). And Rotate can mean upgrade or
    nothing. If upgrade, it's to 5.0 or newer, so we will get a Format_desc, so
    no need to reset mi_description_event now. And if it's nothing (same
    master version as before), no need (still using the slave's format).
  */
  Format_description_log_event *old_fdle= mi->get_mi_description_event();
  if (old_fdle->binlog_version >= 4)
  {
    DBUG_ASSERT(old_fdle->checksum_alg ==
                mi->rli->relay_log.relay_log_checksum_alg);
    Format_description_log_event *new_fdle= new
      Format_description_log_event(3);
    new_fdle->checksum_alg= mi->rli->relay_log.relay_log_checksum_alg;
    mi->set_mi_description_event(new_fdle);
  }
  /*
    Rotate the relay log makes binlog format detection easier (at next slave
    start or mysqlbinlog)
  */
  int ret= rotate_relay_log(mi, true/*need_log_space_lock=true*/);
  DBUG_RETURN(ret);
}

/**
  Reads a 3.23 event and converts it to the slave's format. This code was
  copied from MySQL 4.0.

  @note The caller must hold mi->data_lock before invoking this function.
*/
static int queue_binlog_ver_1_event(Master_info *mi, const char *buf,
                                    ulong event_len)
{
  const char *errmsg = 0;
  ulong inc_pos;
  bool ignore_event= 0;
  char *tmp_buf = 0;
  Relay_log_info *rli= mi->rli;
  DBUG_ENTER("queue_binlog_ver_1_event");

  mysql_mutex_assert_owner(&mi->data_lock);

  /*
    If we get Load event, we need to pass a non-reusable buffer
    to read_log_event, so we do a trick
  */
  if (buf[EVENT_TYPE_OFFSET] == LOAD_EVENT)
  {
    if (unlikely(!(tmp_buf=(char*)my_malloc(event_len+1,MYF(MY_WME)))))
    {
      mi->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR,
                 ER(ER_SLAVE_FATAL_ERROR), "Memory allocation failed");
      DBUG_RETURN(1);
    }
    memcpy(tmp_buf,buf,event_len);
    /*
      Create_file constructor wants a 0 as last char of buffer, this 0 will
      serve as the string-termination char for the file's name (which is at the
      end of the buffer)
      We must increment event_len, otherwise the event constructor will not see
      this end 0, which leads to segfault.
    */
    tmp_buf[event_len++]=0;
    int4store(tmp_buf+EVENT_LEN_OFFSET, event_len);
    buf = (const char*)tmp_buf;
  }
  /*
    This will transform LOAD_EVENT into CREATE_FILE_EVENT, ask the master to
    send the loaded file, and write it to the relay log in the form of
    Append_block/Exec_load (the SQL thread needs the data, as that thread is not
    connected to the master).
  */
  Log_event *ev=
    Log_event::read_log_event(buf, event_len, &errmsg,
                              mi->get_mi_description_event(), 0);
  if (unlikely(!ev))
  {
    sql_print_error("Read invalid event from master: '%s',\
 master could be corrupt but a more likely cause of this is a bug",
                    errmsg);
    my_free((char*) tmp_buf);
    DBUG_RETURN(1);
  }

  mi->set_master_log_pos(ev->log_pos); /* 3.23 events don't contain log_pos */
  switch (ev->get_type_code()) {
  case STOP_EVENT:
    ignore_event= 1;
    inc_pos= event_len;
    break;
  case ROTATE_EVENT:
    if (unlikely(process_io_rotate(mi,(Rotate_log_event*)ev)))
    {
      delete ev;
      DBUG_RETURN(1);
    }
    inc_pos= 0;
    break;
  case CREATE_FILE_EVENT:
    /*
      Yes it's possible to have CREATE_FILE_EVENT here, even if we're in
      queue_old_event() which is for 3.23 events which don't comprise
      CREATE_FILE_EVENT. This is because read_log_event() above has just
      transformed LOAD_EVENT into CREATE_FILE_EVENT.
    */
  {
    /* We come here when and only when tmp_buf != 0 */
    DBUG_ASSERT(tmp_buf != 0);
    inc_pos=event_len;
    ev->log_pos+= inc_pos;
    int error = process_io_create_file(mi,(Create_file_log_event*)ev);
    delete ev;
    mi->set_master_log_pos(mi->get_master_log_pos() + inc_pos);
    DBUG_PRINT("info", ("master_log_pos: %lu", (ulong) mi->get_master_log_pos()));
    my_free((char*)tmp_buf);
    DBUG_RETURN(error);
  }
  default:
    inc_pos= event_len;
    break;
  }
  if (likely(!ignore_event))
  {
    if (ev->log_pos)
      /*
         Don't do it for fake Rotate events (see comment in
      Log_event::Log_event(const char* buf...) in log_event.cc).
      */
      ev->log_pos+= event_len; /* make log_pos be the pos of the end of the event */
    if (unlikely(rli->relay_log.append_event(ev, mi) != 0))
    {
      delete ev;
      DBUG_RETURN(1);
    }
    rli->relay_log.harvest_bytes_written(rli, true/*need_log_space_lock=true*/);
  }
  delete ev;
  mi->set_master_log_pos(mi->get_master_log_pos() + inc_pos);
  DBUG_PRINT("info", ("master_log_pos: %lu", (ulong) mi->get_master_log_pos()));
  DBUG_RETURN(0);
}

/**
  Reads a 4.0 event and converts it to the slave's format. This code was copied
  from queue_binlog_ver_1_event(), with some affordable simplifications.

  @note The caller must hold mi->data_lock before invoking this function.
*/
static int queue_binlog_ver_3_event(Master_info *mi, const char *buf,
                                    ulong event_len)
{
  const char *errmsg = 0;
  ulong inc_pos;
  char *tmp_buf = 0;
  Relay_log_info *rli= mi->rli;
  DBUG_ENTER("queue_binlog_ver_3_event");

  mysql_mutex_assert_owner(&mi->data_lock);

  /* read_log_event() will adjust log_pos to be end_log_pos */
  Log_event *ev=
    Log_event::read_log_event(buf, event_len, &errmsg,
                              mi->get_mi_description_event(), 0);
  if (unlikely(!ev))
  {
    sql_print_error("Read invalid event from master: '%s',\
 master could be corrupt but a more likely cause of this is a bug",
                    errmsg);
    my_free((char*) tmp_buf);
    DBUG_RETURN(1);
  }
  switch (ev->get_type_code()) {
  case STOP_EVENT:
    goto err;
  case ROTATE_EVENT:
    if (unlikely(process_io_rotate(mi,(Rotate_log_event*)ev)))
    {
      delete ev;
      DBUG_RETURN(1);
    }
    inc_pos= 0;
    break;
  default:
    inc_pos= event_len;
    break;
  }

  if (unlikely(rli->relay_log.append_event(ev, mi) != 0))
  {
    delete ev;
    DBUG_RETURN(1);
  }
  rli->relay_log.harvest_bytes_written(rli, true/*need_log_space_lock=true*/);
  delete ev;
  mi->set_master_log_pos(mi->get_master_log_pos() + inc_pos);
err:
  DBUG_PRINT("info", ("master_log_pos: %lu", (ulong) mi->get_master_log_pos()));
  DBUG_RETURN(0);
}

/*
  queue_old_event()

  Writes a 3.23 or 4.0 event to the relay log, after converting it to the 5.0
  (exactly, slave's) format. To do the conversion, we create a 5.0 event from
  the 3.23/4.0 bytes, then write this event to the relay log.

  TODO:
    Test this code before release - it has to be tested on a separate
    setup with 3.23 master or 4.0 master
*/

static int queue_old_event(Master_info *mi, const char *buf,
                           ulong event_len)
{
  DBUG_ENTER("queue_old_event");

  mysql_mutex_assert_owner(&mi->data_lock);

  switch (mi->get_mi_description_event()->binlog_version)
  {
  case 1:
      DBUG_RETURN(queue_binlog_ver_1_event(mi,buf,event_len));
  case 3:
      DBUG_RETURN(queue_binlog_ver_3_event(mi,buf,event_len));
  default: /* unsupported format; eg version 2 */
    DBUG_PRINT("info",("unsupported binlog format %d in queue_old_event()",
                       mi->get_mi_description_event()->binlog_version));
    DBUG_RETURN(1);
  }
}

Format_description_log_event s_fdle(4);

/**
 * Mark this GTID as logged in the rli and set the master_log_file_name and
 * master_log_file_pos in mi. Finally,  flushes the master.info file
 *
 * @retval 0 Success
 * @retval 1 Some failure
 */
int update_rli_and_mi(
    const std::string& gtid_s,
    const std::pair<const std::string, unsigned long long>& master_log_pos)
{
  Master_info* mi= active_mi;
  Relay_log_info *rli= mi->rli;
  DBUG_ASSERT(mi != NULL && mi->rli != NULL);
  mysql_mutex_lock(&mi->data_lock);

  // Update the master log file name in mi, if provided
  if (!master_log_pos.first.empty()) {
    mi->set_master_log_name(master_log_pos.first.c_str());
  }
  // Update the master log file pos in mi
  mi->set_master_log_pos(master_log_pos.second);
  // Flush the master.info file
  mi->flush_info(false);

  // It is possible that this call was only done to update the master_log_pos
  // in which case an empty gtid would have been passed
  if (!gtid_s.length()) {
    mysql_mutex_unlock(&mi->data_lock);
    return 0;
  }

  global_sid_lock->rdlock();
  const char *buf = gtid_s.c_str();
  size_t event_len = gtid_s.length();

  Gtid_log_event gtid_ev(buf, event_len, &s_fdle);
  Gtid gtid = {0, 0};
  gtid.sidno= gtid_ev.get_sidno(false);
  if (gtid.sidno < 0)
  {
    global_sid_lock->unlock();
    mysql_mutex_unlock(&mi->data_lock);
    sql_print_information("could not get proper sid: %s", buf);
    return 1;
  }

  gtid.gno= gtid_ev.get_gno();

  // old_retrieved_gtid= *(mi->rli->get_last_retrieved_gtid());
  int ret= rli->add_logged_gtid(gtid.sidno, gtid.gno);
  if (!ret)
    rli->set_last_retrieved_gtid(gtid);

  global_sid_lock->unlock();
  mysql_mutex_unlock(&mi->data_lock);

  return ret;
}

/*
  queue_event()

  If the event is 3.23/4.0, passes it to queue_old_event() which will convert
  it. Otherwise, writes a 5.0 (or newer) event to the relay log. Then there is
  no format conversion, it's pure read/write of bytes.
  So a 5.0.0 slave's relay log can contain events in the slave's format or in
  any >=5.0.0 format.
*/

static int queue_event(Master_info* mi,const char* buf, ulong event_len)
{
  int error= 0;
  String error_msg;
  ulong inc_pos= 0;
  Relay_log_info *rli= mi->rli;
  mysql_mutex_t *log_lock= rli->relay_log.get_log_lock();
  ulong s_id;
  bool unlock_data_lock= TRUE;
  /*
    FD_q must have been prepared for the first R_a event
    inside get_master_version_and_clock()
    Show-up of FD:s affects checksum_alg at once because
    that changes FD_queue.
  */
  uint8 checksum_alg= mi->checksum_alg_before_fd != BINLOG_CHECKSUM_ALG_UNDEF ?
    mi->checksum_alg_before_fd :
    mi->rli->relay_log.relay_log_checksum_alg;

  char *save_buf= NULL; // needed for checksumming the fake Rotate event
  char rot_buf[LOG_EVENT_HEADER_LEN + ROTATE_HEADER_LEN + FN_REFLEN];
  char fde_buf[LOG_EVENT_MINIMAL_HEADER_LEN + FORMAT_DESCRIPTION_HEADER_LEN +
               BINLOG_CHECKSUM_ALG_DESC_LEN + BINLOG_CHECKSUM_LEN];
  Gtid gtid= { 0, 0 };
  Gtid old_retrieved_gtid= { 0, 0 };
  Log_event_type event_type= (Log_event_type)buf[EVENT_TYPE_OFFSET];

  DBUG_ASSERT(checksum_alg == BINLOG_CHECKSUM_ALG_OFF ||
              checksum_alg == BINLOG_CHECKSUM_ALG_UNDEF ||
              checksum_alg == BINLOG_CHECKSUM_ALG_CRC32);

  DBUG_ENTER("queue_event");
  /*
    FD_queue checksum alg description does not apply in a case of
    FD itself. The one carries both parts of the checksum data.
  */
  if (event_type == FORMAT_DESCRIPTION_EVENT)
  {
    checksum_alg= get_checksum_alg(buf, event_len);
  }
  else if (event_type == START_EVENT_V3)
  {
    // checksum behaviour is similar to the pre-checksum FD handling
    mi->checksum_alg_before_fd= BINLOG_CHECKSUM_ALG_UNDEF;
    mysql_mutex_lock(&mi->data_lock);
    mi->get_mi_description_event()->checksum_alg=
      mi->rli->relay_log.relay_log_checksum_alg= checksum_alg=
      BINLOG_CHECKSUM_ALG_OFF;
    mysql_mutex_unlock(&mi->data_lock);
  }

  // does not hold always because of old binlog can work with NM
  // DBUG_ASSERT(checksum_alg != BINLOG_CHECKSUM_ALG_UNDEF);

  // should hold unless manipulations with RL. Tests that do that
  // will have to refine the clause.
  DBUG_ASSERT(mi->rli->relay_log.relay_log_checksum_alg !=
              BINLOG_CHECKSUM_ALG_UNDEF);

  // Emulate the network corruption
  DBUG_EXECUTE_IF("corrupt_queue_event",
    if (event_type != FORMAT_DESCRIPTION_EVENT)
    {
      char *debug_event_buf_c = (char*) buf;
      int debug_cor_pos = rand() % (event_len - BINLOG_CHECKSUM_LEN);
      debug_event_buf_c[debug_cor_pos] =~ debug_event_buf_c[debug_cor_pos];
      DBUG_PRINT("info", ("Corrupt the event at queue_event: byte on position %d", debug_cor_pos));
      DBUG_SET("");
    }
  );

  if (!(mi->ignore_checksum_alg && (event_type == FORMAT_DESCRIPTION_EVENT ||
      event_type == ROTATE_EVENT)) &&
      event_checksum_test((uchar *) buf, event_len, checksum_alg))
  {
    error= ER_NETWORK_READ_EVENT_CHECKSUM_FAILURE;
    unlock_data_lock= FALSE;
    goto err;
  }

  mysql_mutex_lock(&mi->data_lock);

  if (mi->get_mi_description_event()->binlog_version < 4 &&
      event_type != FORMAT_DESCRIPTION_EVENT /* a way to escape */)
  {
    // Old FDE doesn't have description of the heartbeat events. Skip them.
    if (event_type == HEARTBEAT_LOG_EVENT)
      goto skip_relay_logging;
    int ret= queue_old_event(mi,buf,event_len);
    mysql_mutex_unlock(&mi->data_lock);
    DBUG_RETURN(ret);
  }

  switch (event_type) {
  case STOP_EVENT:
    /*
      We needn't write this event to the relay log. Indeed, it just indicates a
      master server shutdown. The only thing this does is cleaning. But
      cleaning is already done on a per-master-thread basis (as the master
      server is shutting down cleanly, it has written all DROP TEMPORARY TABLE
      prepared statements' deletion are TODO only when we binlog prep stmts).

      We don't even increment mi->get_master_log_pos(), because we may be just after
      a Rotate event. Btw, in a few milliseconds we are going to have a Start
      event from the next binlog (unless the master is presently running
      without --log-bin).
    */
    goto err;
  case ROTATE_EVENT:
  {
    Rotate_log_event rev(buf, checksum_alg != BINLOG_CHECKSUM_ALG_OFF ?
                         event_len - BINLOG_CHECKSUM_LEN : event_len,
                         mi->get_mi_description_event());

    if (unlikely(process_io_rotate(mi, &rev)))
    {
      error= ER_SLAVE_RELAY_LOG_WRITE_FAILURE;
      goto err;
    }
    /*
       Checksum special cases for the fake Rotate (R_f) event caused by the protocol
       of events generation and serialization in RL where Rotate of master is
       queued right next to FD of slave.
       Since it's only FD that carries the alg desc of FD_s has to apply to R_m.
       Two special rules apply only to the first R_f which comes in before any FD_m.
       The 2nd R_f should be compatible with the FD_s that must have taken over
       the last seen FD_m's (A).

       RSC_1: If OM \and fake Rotate \and slave is configured to
              to compute checksum for its first FD event for RL
              the fake Rotate gets checksummed here.
    */
    if (uint4korr(&buf[0]) == 0 && checksum_alg == BINLOG_CHECKSUM_ALG_OFF &&
        mi->rli->relay_log.relay_log_checksum_alg != BINLOG_CHECKSUM_ALG_OFF)
    {
      ha_checksum rot_crc= my_checksum(0L, NULL, 0);
      event_len += BINLOG_CHECKSUM_LEN;
      memcpy(rot_buf, buf, event_len - BINLOG_CHECKSUM_LEN);
      int4store(&rot_buf[EVENT_LEN_OFFSET],
                uint4korr(rot_buf + EVENT_LEN_OFFSET) + BINLOG_CHECKSUM_LEN);
      rot_crc= my_checksum(rot_crc, (const uchar *) rot_buf,
                           event_len - BINLOG_CHECKSUM_LEN);
      int4store(&rot_buf[event_len - BINLOG_CHECKSUM_LEN], rot_crc);
      DBUG_ASSERT(event_len == uint4korr(&rot_buf[EVENT_LEN_OFFSET]));
      DBUG_ASSERT(mi->get_mi_description_event()->checksum_alg ==
                  mi->rli->relay_log.relay_log_checksum_alg);
      /* the first one */
      DBUG_ASSERT(mi->checksum_alg_before_fd != BINLOG_CHECKSUM_ALG_UNDEF);
      save_buf= (char *) buf;
      buf= rot_buf;
    }
    else
      /*
        RSC_2: If NM \and fake Rotate \and slave does not compute checksum
        the fake Rotate's checksum is stripped off before relay-logging.
      */
      if (uint4korr(&buf[0]) == 0 && checksum_alg != BINLOG_CHECKSUM_ALG_OFF &&
          mi->rli->relay_log.relay_log_checksum_alg == BINLOG_CHECKSUM_ALG_OFF)
      {
        event_len -= BINLOG_CHECKSUM_LEN;
        memcpy(rot_buf, buf, event_len);
        int4store(&rot_buf[EVENT_LEN_OFFSET],
                  uint4korr(rot_buf + EVENT_LEN_OFFSET) - BINLOG_CHECKSUM_LEN);
        DBUG_ASSERT(event_len == uint4korr(&rot_buf[EVENT_LEN_OFFSET]));
        DBUG_ASSERT(mi->get_mi_description_event()->checksum_alg ==
                    mi->rli->relay_log.relay_log_checksum_alg);
        /* the first one */
        DBUG_ASSERT(mi->checksum_alg_before_fd != BINLOG_CHECKSUM_ALG_UNDEF);
        save_buf= (char *) buf;
        buf= rot_buf;
      }
    /*
      Now the I/O thread has just changed its mi->get_master_log_name(), so
      incrementing mi->get_master_log_pos() is nonsense.
    */
    inc_pos= 0;
    break;
  }
  case FORMAT_DESCRIPTION_EVENT:
  {
    /*
      Create an event, and save it (when we rotate the relay log, we will have
      to write this event again).
    */
    /*
      We are the only thread which reads/writes mi_description_event.
      The relay_log struct does not move (though some members of it can
      change), so we needn't any lock (no rli->data_lock, no log lock).
    */
    const char* errmsg;
    // mark it as undefined that is irrelevant anymore
    mi->checksum_alg_before_fd= BINLOG_CHECKSUM_ALG_UNDEF;
    /*
      Turn off checksum in FDE here, since the event was modified by 5.1 master
      before sending to the slave. Otherwise, event_checksum_test fails causing
      slave I/O and SQL threads to stop.
    */
    if (mi->ignore_checksum_alg && checksum_alg != BINLOG_CHECKSUM_ALG_OFF &&
        checksum_alg != BINLOG_CHECKSUM_ALG_UNDEF)
    {
      DBUG_ASSERT(event_len == sizeof(fde_buf));
      memcpy(fde_buf, buf, event_len);
      fde_buf[event_len - BINLOG_CHECKSUM_LEN - BINLOG_CHECKSUM_ALG_DESC_LEN] =
        BINLOG_CHECKSUM_ALG_OFF;
      save_buf= (char *)buf;
      buf= fde_buf;
    }
    Format_description_log_event *new_fdle=
      (Format_description_log_event*)
      Log_event::read_log_event(buf, event_len, &errmsg,
                                mi->get_mi_description_event(), 1);
    if (new_fdle == NULL)
    {
      error= ER_SLAVE_RELAY_LOG_WRITE_FAILURE;
      goto err;
    }
    if (new_fdle->checksum_alg == BINLOG_CHECKSUM_ALG_UNDEF)
      new_fdle->checksum_alg= BINLOG_CHECKSUM_ALG_OFF;
    mi->set_mi_description_event(new_fdle);

    /* installing new value of checksum Alg for relay log */
    mi->rli->relay_log.relay_log_checksum_alg= new_fdle->checksum_alg;

    /*
       Though this does some conversion to the slave's format, this will
       preserve the master's binlog format version, and number of event types.
    */
    /*
       If the event was not requested by the slave (the slave did not ask for
       it), i.e. has end_log_pos=0, we do not increment mi->get_master_log_pos()
    */
    inc_pos= uint4korr(buf+LOG_POS_OFFSET) ? event_len : 0;
    DBUG_PRINT("info",("binlog format is now %d",
                       mi->get_mi_description_event()->binlog_version));

  }
  break;

  case HEARTBEAT_LOG_EVENT:
  {
    /*
      HB (heartbeat) cannot come before RL (Relay)
    */
    char  llbuf[22];
    Heartbeat_log_event hb(buf,
                           mi->rli->relay_log.relay_log_checksum_alg
                           != BINLOG_CHECKSUM_ALG_OFF ?
                           event_len - BINLOG_CHECKSUM_LEN : event_len,
                           mi->get_mi_description_event());
    if (!hb.is_valid())
    {
      error= ER_SLAVE_HEARTBEAT_FAILURE;
      error_msg.append(STRING_WITH_LEN("inconsistent heartbeat event content;"));
      error_msg.append(STRING_WITH_LEN("the event's data: log_file_name "));
      error_msg.append(hb.get_log_ident(), (uint) strlen(hb.get_log_ident()));
      error_msg.append(STRING_WITH_LEN(" log_pos "));
      llstr(hb.log_pos, llbuf);
      error_msg.append(llbuf, strlen(llbuf));
      goto err;
    }
    mi->received_heartbeats++;
    mi->last_heartbeat= my_time(0);

    /*
      Update the last_master_timestamp if the heartbeat from the master
      has a greater timestamp value, this makes sure last_master_timestamp
      is always monotonically increasing
    */
    mysql_mutex_lock(&rli->data_lock);
    auto io_thread_file= mi->get_master_log_name();
    auto io_thread_pos= mi->get_master_log_pos();
    auto sql_thread_file= mi->rli->get_group_master_log_name();
    auto sql_thread_pos= mi->rli->get_group_master_log_pos();

    // find out if the SQL thread has caught up with the IO thread by comparing
    // their coordinates
    bool caughtup= (sql_thread_pos == io_thread_pos) &&
                   (!strcmp(sql_thread_file, io_thread_file));
    // case: update last master ts only if we've caughtup
    if (caughtup)
      rli->set_last_master_timestamp(hb.when.tv_sec, hb.when.tv_sec * 1000);
    mysql_mutex_unlock(&rli->data_lock);

    /*
      During GTID protocol, if the master skips transactions,
      a heartbeat event is sent to the slave at the end of last
      skipped transaction to update coordinates.

      I/O thread receives the heartbeat event and updates mi
      only if the received heartbeat position is greater than
      mi->get_master_log_pos(). This event is written to the
      relay log as an ignored Rotate event. SQL thread reads
      the rotate event only to update the coordinates corresponding
      to the last skipped transaction. Note that,
      we update only the positions and not the file names, as a ROTATE
      EVENT from the master prior to this will update the file name.
    */
    if (mi->is_auto_position()  && mi->get_master_log_pos() < hb.log_pos
        &&  mi->get_master_log_name() != NULL)
    {

      DBUG_ASSERT(memcmp(const_cast<char*>(mi->get_master_log_name()),
                         hb.get_log_ident(), hb.get_ident_len()) == 0);

      mi->set_master_log_pos(hb.log_pos);

      /*
         Put this heartbeat event in the relay log as a Rotate Event.
      */
      inc_pos= 0;
      memcpy(rli->ign_master_log_name_end, mi->get_master_log_name(),
             FN_REFLEN);
      rli->ign_master_log_pos_end = mi->get_master_log_pos();

      if (write_ignored_events_info_to_relay_log(mi->info_thd, mi))
        goto err;
    }

    /*
       compare local and event's versions of log_file, log_pos.

       Heartbeat is sent only after an event corresponding to the corrdinates
       the heartbeat carries.
       Slave can not have a difference in coordinates except in the only
       special case when mi->get_master_log_name(), mi->get_master_log_pos() have never
       been updated by Rotate event i.e when slave does not have any history
       with the master (and thereafter mi->get_master_log_pos() is NULL).

       TODO: handling `when' for SHOW SLAVE STATUS' snds behind
    */
    if ((memcmp(const_cast<char *>(mi->get_master_log_name()),
                hb.get_log_ident(), hb.get_ident_len())
         && mi->get_master_log_name() != NULL)
        || ((mi->get_master_log_pos() != hb.log_pos && gtid_mode == 0) ||
            /*
              When Gtid mode is on only monotocity can be claimed.
              Todo: enhance HB event with the skipped events size
              and to convert HB.pos  == MI.pos to HB.pos - HB.skip_size == MI.pos
            */
            (mi->get_master_log_pos() > hb.log_pos)))
    {
      /* missed events of heartbeat from the past */
      error= ER_SLAVE_HEARTBEAT_FAILURE;
      error_msg.append(STRING_WITH_LEN("heartbeat is not compatible with local info;"));
      error_msg.append(STRING_WITH_LEN("the event's data: log_file_name "));
      error_msg.append(hb.get_log_ident(), (uint) strlen(hb.get_log_ident()));
      error_msg.append(STRING_WITH_LEN(" log_pos "));
      llstr(hb.log_pos, llbuf);
      error_msg.append(llbuf, strlen(llbuf));
      goto err;
    }
    goto skip_relay_logging;
  }
  break;

  case PREVIOUS_GTIDS_LOG_EVENT:
  {
    /*
      This event does not have any meaning for the slave and
      was just sent to show the slave the master is making
      progress and avoid possible deadlocks.
      So at this point, the event is replaced by a rotate
      event what will make the slave to update what it knows
      about the master's coordinates.
    */
    inc_pos= 0;
    mi->set_master_log_pos(mi->get_master_log_pos() + event_len);
    memcpy(rli->ign_master_log_name_end, mi->get_master_log_name(), FN_REFLEN);
    rli->ign_master_log_pos_end= mi->get_master_log_pos();

    if (write_ignored_events_info_to_relay_log(mi->info_thd, mi))
      goto err;

    goto skip_relay_logging;
  }
  break;

  case GTID_LOG_EVENT:
  {
    if (gtid_mode == 0)
    {
      error= ER_FOUND_GTID_EVENT_WHEN_GTID_MODE_IS_OFF;
      goto err;
    }
    global_sid_lock->rdlock();
    Gtid_log_event gtid_ev(buf, checksum_alg != BINLOG_CHECKSUM_ALG_OFF ?
                           event_len - BINLOG_CHECKSUM_LEN : event_len,
                           mi->get_mi_description_event());
    gtid.sidno= gtid_ev.get_sidno(false);
    global_sid_lock->unlock();
    if (gtid.sidno < 0)
      goto err;
    gtid.gno= gtid_ev.get_gno();
    inc_pos= event_len;
  }
  break;

  case ANONYMOUS_GTID_LOG_EVENT:

  default:
    inc_pos= event_len;
  break;
  }

  /*
    Simulate an unknown ignorable log event by rewriting the write_rows log
    event and previous_gtids log event before writing them in relay log.
  */
  DBUG_EXECUTE_IF("simulate_unknown_ignorable_log_event",
    if (event_type == WRITE_ROWS_EVENT ||
        event_type == PREVIOUS_GTIDS_LOG_EVENT)
    {
      char *event_buf= const_cast<char*>(buf);
      /* Overwrite the log event type with an unknown type. */
      event_buf[EVENT_TYPE_OFFSET]= ENUM_END_EVENT + 1;
      /* Set LOG_EVENT_IGNORABLE_F for the log event. */
      int2store(event_buf + FLAGS_OFFSET,
                uint2korr(event_buf + FLAGS_OFFSET) | LOG_EVENT_IGNORABLE_F);
    }
  );

  /*
     If this event is originating from this server, don't queue it.
     We don't check this for 3.23 events because it's simpler like this; 3.23
     will be filtered anyway by the SQL slave thread which also tests the
     server id (we must also keep this test in the SQL thread, in case somebody
     upgrades a 4.0 slave which has a not-filtered relay log).

     ANY event coming from ourselves can be ignored: it is obvious for queries;
     for STOP_EVENT/ROTATE_EVENT/START_EVENT: these cannot come from ourselves
     (--log-slave-updates would not log that) unless this slave is also its
     direct master (an unsupported, useless setup!).
  */

  s_id= uint4korr(buf + SERVER_ID_OFFSET);

  /*
    If server_id_bits option is set we need to mask out irrelevant bits
    when checking server_id, but we still put the full unmasked server_id
    into the Relay log so that it can be accessed when applying the event
  */
  s_id&= opt_server_id_mask;

  if ((s_id == ::server_id && !mi->rli->replicate_same_server_id) ||
      /*
        the following conjunction deals with IGNORE_SERVER_IDS, if set
        If the master is on the ignore list, execution of
        format description log events and rotate events is necessary.
      */
      (mi->ignore_server_ids->dynamic_ids.elements > 0 &&
       mi->shall_ignore_server_id(s_id) &&
       /* everything is filtered out from non-master */
       (s_id != mi->master_id ||
        /* for the master meta information is necessary */
        (event_type != FORMAT_DESCRIPTION_EVENT &&
         event_type != ROTATE_EVENT))))
  {
    /*
      Do not write it to the relay log.
      a) We still want to increment mi->get_master_log_pos(), so that we won't
      re-read this event from the master if the slave IO thread is now
      stopped/restarted (more efficient if the events we are ignoring are big
      LOAD DATA INFILE).
      b) We want to record that we are skipping events, for the information of
      the slave SQL thread, otherwise that thread may let
      rli->group_relay_log_pos stay too small if the last binlog's event is
      ignored.
      But events which were generated by this slave and which do not exist in
      the master's binlog (i.e. Format_desc, Rotate & Stop) should not increment
      mi->get_master_log_pos().
      If the event is originated remotely and is being filtered out by
      IGNORE_SERVER_IDS it increments mi->get_master_log_pos()
      as well as rli->group_relay_log_pos.
    */
    mysql_mutex_lock(log_lock);
    if (!(s_id == ::server_id && !mi->rli->replicate_same_server_id) ||
        (event_type != FORMAT_DESCRIPTION_EVENT &&
         event_type != ROTATE_EVENT &&
         event_type != STOP_EVENT))
    {
      mi->set_master_log_pos(mi->get_master_log_pos() + inc_pos);
      memcpy(rli->ign_master_log_name_end, mi->get_master_log_name(), FN_REFLEN);
      DBUG_ASSERT(rli->ign_master_log_name_end[0]);
      rli->ign_master_log_pos_end= mi->get_master_log_pos();
    }
    rli->relay_log.update_binlog_end_pos(); // the slave SQL thread needs to re-check
    mysql_mutex_unlock(log_lock);
    DBUG_PRINT("info", ("master_log_pos: %lu, event originating from %u server, ignored",
                        (ulong) mi->get_master_log_pos(), uint4korr(buf + SERVER_ID_OFFSET)));
  }
  else
  {
    DBUG_EXECUTE_IF("flush_after_reading_gtid_event",
                    if (event_type == GTID_LOG_EVENT && gtid.gno == 4)
                      DBUG_SET("+d,set_max_size_zero");
                   );
    DBUG_EXECUTE_IF("set_append_buffer_error",
                    if (event_type == GTID_LOG_EVENT && gtid.gno == 4)
                      DBUG_SET("+d,simulate_append_buffer_error");
                   );
    /*
      Add the GTID to the retrieved set before actually appending it to relay
      log. This will ensure that if a rotation happens at this point of time the
      new GTID will be reflected as part of Previous_Gtid set and
      Retrieved_Gtid_Set will not have any gaps.
    */
    if (event_type == GTID_LOG_EVENT)
    {
      global_sid_lock->rdlock();
      old_retrieved_gtid= *(mi->rli->get_last_retrieved_gtid());
      int ret= rli->add_logged_gtid(gtid.sidno, gtid.gno);
      if (!ret)
        rli->set_last_retrieved_gtid(gtid);
      global_sid_lock->unlock();
      if (ret != 0)
      {
        mysql_mutex_unlock(log_lock);
        goto err;
      }
    }
    /* write the event to the relay log */
    if (!DBUG_EVALUATE_IF("simulate_append_buffer_error", 1, 0) &&
       likely(rli->relay_log.append_buffer(buf, event_len, mi) == 0))
    {
      mi->set_master_log_pos(mi->get_master_log_pos() + inc_pos);
      DBUG_PRINT("info", ("master_log_pos: %lu", (ulong) mi->get_master_log_pos()));
      rli->relay_log.harvest_bytes_written(rli, true/*need_log_space_lock=true*/);
    }
    else
    {
      if (event_type == GTID_LOG_EVENT)
      {
        global_sid_lock->rdlock();
        Gtid_set * retrieved_set= (const_cast<Gtid_set *>(mi->rli->get_gtid_set()));
        if (retrieved_set->_remove_gtid(gtid) != RETURN_STATUS_OK)
        {
          global_sid_lock->unlock();
          mysql_mutex_unlock(log_lock);
          goto err;
        }
        if (!old_retrieved_gtid.empty())
          rli->set_last_retrieved_gtid(old_retrieved_gtid);
        global_sid_lock->unlock();
      }
      error= ER_SLAVE_RELAY_LOG_WRITE_FAILURE;
    }
    mysql_mutex_lock(log_lock);
    rli->ign_master_log_name_end[0]= 0; // last event is not ignored
    mysql_mutex_unlock(log_lock);
    if (save_buf != NULL)
      buf= save_buf;
  }

skip_relay_logging:

err:
  if (unlock_data_lock)
    mysql_mutex_unlock(&mi->data_lock);
  DBUG_PRINT("info", ("error: %d", error));
  if (error)
    mi->report(ERROR_LEVEL, error, ER(error),
               (error == ER_SLAVE_RELAY_LOG_WRITE_FAILURE)?
               "could not queue event from master" :
               error_msg.ptr());
  DBUG_RETURN(error);
}

/**
  Hook to detach the active VIO before closing a connection handle.

  The client API might close the connection (and associated data)
  in case it encounters a unrecoverable (network) error. This hook
  is called from the client code before the VIO handle is deleted
  allows the thread to detach the active vio so it does not point
  to freed memory.

  Other calls to THD::clear_active_vio throughout this module are
  redundant due to the hook but are left in place for illustrative
  purposes.
*/

extern "C" void slave_io_thread_detach_vio()
{
#ifdef SIGNAL_WITH_VIO_SHUTDOWN
  THD *thd= current_thd;
  if (thd && thd->slave_thread)
    thd->clear_active_vio();
#endif
}

/*
  method to configure some common mysql options for connection to master
*/
void configure_master_connection_options(MYSQL* mysql, Master_info* mi)
{
  if (mi->bind_addr[0])
  {
    DBUG_PRINT("info",("bind_addr: %s", mi->bind_addr));
    mysql_options(mysql, MYSQL_OPT_BIND, mi->bind_addr);
  }

#ifdef HAVE_OPENSSL
  if (mi->ssl)
  {
    mysql_ssl_set(mysql,
                  mi->ssl_key[0]?mi->ssl_key:0,
                  mi->ssl_cert[0]?mi->ssl_cert:0,
                  mi->ssl_ca[0]?mi->ssl_ca:0,
                  mi->ssl_capath[0]?mi->ssl_capath:0,
                  mi->ssl_cipher[0]?mi->ssl_cipher:0);
#ifdef HAVE_YASSL
    mi->ssl_crl[0]= '\0';
    mi->ssl_crlpath[0]= '\0';
#endif
    mysql_options(mysql, MYSQL_OPT_SSL_CRL,
                  mi->ssl_crl[0] ? mi->ssl_crl : 0);
    mysql_options(mysql, MYSQL_OPT_SSL_CRLPATH,
                  mi->ssl_crlpath[0] ? mi->ssl_crlpath : 0);
    mysql_options(mysql, MYSQL_OPT_SSL_VERIFY_SERVER_CERT,
                  &mi->ssl_verify_server_cert);
  }
#endif

  /*
    If server's default charset is not supported (like utf16, utf32) as client
    charset, then set client charset to 'latin1' (default client charset).
  */
  if (is_supported_parser_charset(default_charset_info))
    mysql_options(mysql, MYSQL_SET_CHARSET_NAME, default_charset_info->csname);
  else
  {
    sql_print_information("'%s' can not be used as client character set. "
                          "'%s' will be used as default client character set "
                          "while connecting to master.",
                          default_charset_info->csname,
                          default_client_charset_info->csname);
    mysql_options(mysql, MYSQL_SET_CHARSET_NAME,
                  default_client_charset_info->csname);
  }


  /* This one is not strictly needed but we have it here for completeness */
  mysql_options(mysql, MYSQL_SET_CHARSET_DIR, (char *) charsets_dir);

  if (mi->is_start_plugin_auth_configured())
  {
    DBUG_PRINT("info", ("Slaving is using MYSQL_DEFAULT_AUTH %s",
                        mi->get_start_plugin_auth()));
    mysql_options(mysql, MYSQL_DEFAULT_AUTH, mi->get_start_plugin_auth());
  }

  if (mi->is_start_plugin_dir_configured())
  {
    DBUG_PRINT("info", ("Slaving is using MYSQL_PLUGIN_DIR %s",
                        mi->get_start_plugin_dir()));
    mysql_options(mysql, MYSQL_PLUGIN_DIR, mi->get_start_plugin_dir());
  }
  /* Set MYSQL_PLUGIN_DIR in case master asks for an external authentication plugin */
  else if (opt_plugin_dir_ptr && *opt_plugin_dir_ptr)
    mysql_options(mysql, MYSQL_PLUGIN_DIR, opt_plugin_dir_ptr);
}

/*
  Try to connect until successful or slave killed

  SYNPOSIS
    safe_connect()
    thd                 Thread handler for slave
    mysql               MySQL connection handle
    mi                  Replication handle

  RETURN
    0   ok
    #   Error
*/

static int safe_connect(THD* thd, MYSQL* mysql, Master_info* mi)
{
  DBUG_ENTER("safe_connect");

  DBUG_RETURN(connect_to_master(thd, mysql, mi, 0, 0));
}

/*
  SYNPOSIS
    connect_to_master()

  IMPLEMENTATION
    Try to connect until successful or slave killed or we have retried
    mi->retry_count times
*/

static int connect_to_master(THD* thd, MYSQL* mysql, Master_info* mi,
                             bool reconnect, bool suppress_warnings)
{
  int slave_was_killed= 0;
  int last_errno= -2;                           // impossible error
  ulong err_count=0;
  char llbuff[22];
  char password[MAX_PASSWORD_LENGTH + 1];
  int password_size= sizeof(password);
  DBUG_ENTER("connect_to_master");
  set_slave_max_allowed_packet(thd, mysql);
#ifndef DBUG_OFF
  mi->events_until_exit = disconnect_slave_event_count;
#endif
  ulong client_flag= CLIENT_REMEMBER_OPTIONS;
  if (opt_slave_compressed_protocol) {
    client_flag|= CLIENT_COMPRESS;              /* We will use compression */
    mysql_options(mysql, MYSQL_OPT_COMP_LIB, (void *)opt_slave_compression_lib);
  }

  if (opt_slave_compressed_event_protocol)
  {
    client_flag|= CLIENT_COMPRESS_EVENT;
    if (opt_slave_compressed_protocol)
    {
      sql_print_warning("Both slave_compressed_protocol and "
          "slave_compressed_event protocol are enabled. Disabling "
          "slave_compressed_protocol to avoid double compression.");
      client_flag&= ~CLIENT_COMPRESS;
      opt_slave_compressed_protocol= FALSE;
    }
    mysql_options(mysql, MYSQL_OPT_COMP_LIB, (void *)opt_slave_compression_lib);
  }

  mysql_options(mysql, MYSQL_OPT_CONNECT_TIMEOUT, (char *) &slave_net_timeout);
  mysql_options(mysql, MYSQL_OPT_READ_TIMEOUT, (char *) &slave_net_timeout);

  configure_master_connection_options(mysql, mi);

  if (!mi->is_start_user_configured())
    sql_print_warning("%s", ER(ER_INSECURE_CHANGE_MASTER));

  if (mi->get_password(password, &password_size))
  {
    mi->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR,
               ER(ER_SLAVE_FATAL_ERROR),
               "Unable to configure password when attempting to "
               "connect to the master server. Connection attempt "
               "terminated.");
    DBUG_RETURN(1);
  }

  const char* user= mi->get_user();
  if (user == NULL || user[0] == 0)
  {
    mi->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR,
               ER(ER_SLAVE_FATAL_ERROR),
               "Invalid (empty) username when attempting to "
               "connect to the master server. Connection attempt "
               "terminated.");
    DBUG_RETURN(1);
  }

  mysql_options(mysql, MYSQL_OPT_NET_RECEIVE_BUFFER_SIZE,
                &rpl_receive_buffer_size);

  while (!(slave_was_killed = io_slave_killed(thd,mi))
         && (reconnect ? mysql_reconnect(mysql) != 0 :
             mysql_real_connect(mysql, mi->host, user,
                                password, 0, mi->port, 0, client_flag) == 0))
  {
    /*
       SHOW SLAVE STATUS will display the number of retries which
       would be real retry counts instead of mi->retry_count for
       each connection attempt by 'Last_IO_Error' entry.
    */
    last_errno=mysql_errno(mysql);
    suppress_warnings= 0;
    mi->report(ERROR_LEVEL, last_errno,
               "error %s to master '%s@%s:%d'"
               " - retry-time: %d  retries: %lu",
               (reconnect ? "reconnecting" : "connecting"),
               mi->get_user(), mi->host, mi->port,
               mi->connect_retry, err_count + 1);
    /*
      By default we try forever. The reason is that failure will trigger
      master election, so if the user did not set mi->retry_count we
      do not want to have election triggered on the first failure to
      connect
    */
    if (++err_count == mi->retry_count)
    {
      slave_was_killed=1;
      break;
    }
    slave_sleep(thd, mi->connect_retry, io_slave_killed, mi);
  }

  if (!slave_was_killed)
  {
    mi->clear_error(); // clear possible left over reconnect error
    if (reconnect)
    {
      if (!suppress_warnings && log_warnings)
        sql_print_information("Slave: connected to master '%s@%s:%d',\
replication resumed in log '%s' at position %s", mi->get_user(),
                        mi->host, mi->port,
                        mi->get_io_rpl_log_name(),
                        llstr(mi->get_master_log_pos(),llbuff));
    }
    else
    {
      general_log_print(thd, COM_CONNECT_OUT, "%s@%s:%d",
                        mi->get_user(), mi->host, mi->port);
    }
#ifdef SIGNAL_WITH_VIO_SHUTDOWN
    thd->set_active_vio(mysql->net.vio);
#endif
  }
  if (mysql_get_ssl_cipher(mysql)) {
    strncpy(mi->ssl_actual_cipher,
            mysql_get_ssl_cipher(mysql),
            sizeof(mi->ssl_actual_cipher));
    mi->ssl_actual_cipher[sizeof(mi->ssl_actual_cipher) - 1] = 0;

    mysql_get_ssl_server_cerfificate_info(
      mysql,
      mi->ssl_master_issuer, sizeof(mi->ssl_master_issuer),
      mi->ssl_master_subject, sizeof(mi->ssl_master_subject));
  }

  mysql->reconnect= 1;
  DBUG_PRINT("exit",("slave_was_killed: %d", slave_was_killed));
  DBUG_RETURN(slave_was_killed);
}


/*
  safe_reconnect()

  IMPLEMENTATION
    Try to connect until successful or slave killed or we have retried
    mi->retry_count times
*/

static int safe_reconnect(THD* thd, MYSQL* mysql, Master_info* mi,
                          bool suppress_warnings)
{
  DBUG_ENTER("safe_reconnect");
  DBUG_RETURN(connect_to_master(thd, mysql, mi, 1, suppress_warnings));
}


/*
  Called when we notice that the current "hot" log got rotated under our feet.
*/

static IO_CACHE *reopen_relay_log(Relay_log_info *rli, const char **errmsg)
{
  DBUG_ENTER("reopen_relay_log");
  DBUG_ASSERT(rli->cur_log != &rli->cache_buf);
  DBUG_ASSERT(rli->cur_log_fd == -1);

  IO_CACHE *cur_log = rli->cur_log=&rli->cache_buf;
  if ((rli->cur_log_fd=open_binlog_file(cur_log,rli->get_event_relay_log_name(),
                                        errmsg)) <0)
    DBUG_RETURN(0);
  /*
    We want to start exactly where we was before:
    relay_log_pos       Current log pos
    pending             Number of bytes already processed from the event
  */
  rli->set_event_relay_log_pos(max<ulonglong>(rli->get_event_relay_log_pos(),
                                              BIN_LOG_HEADER_SIZE));
  my_b_seek(cur_log,rli->get_event_relay_log_pos());
  DBUG_RETURN(cur_log);
}


/**
  Reads next event from the relay log.  Should be called from the
  slave SQL thread.

  @param rli Relay_log_info structure for the slave SQL thread.

  @return The event read, or NULL on error.  If an error occurs, the
  error is reported through the sql_print_information() or
  sql_print_error() functions.
*/
static Log_event* next_event(Relay_log_info* rli)
{
  Log_event* ev;
  IO_CACHE* cur_log = rli->cur_log;
  mysql_mutex_t *log_lock = rli->relay_log.get_log_lock();
  const char* errmsg=0;
  THD* thd = rli->info_thd;
  int read_length; /* length of event read from relay log */

  DBUG_ENTER("next_event");

  DBUG_ASSERT(thd != 0);

#ifndef DBUG_OFF
  if (abort_slave_event_count && !rli->events_until_exit--)
    DBUG_RETURN(0);
#endif

  /*
    For most operations we need to protect rli members with data_lock,
    so we assume calling function acquired this mutex for us and we will
    hold it for the most of the loop below However, we will release it
    whenever it is worth the hassle,  and in the cases when we go into a
    mysql_cond_wait() with the non-data_lock mutex
  */
  mysql_mutex_assert_owner(&rli->data_lock);

  while (!sql_slave_killed(thd,rli))
  {
    /*
      We can have two kinds of log reading:
      hot_log:
        rli->cur_log points at the IO_CACHE of relay_log, which
        is actively being updated by the I/O thread. We need to be careful
        in this case and make sure that we are not looking at a stale log that
        has already been rotated. If it has been, we reopen the log.

      The other case is much simpler:
        We just have a read only log that nobody else will be updating.
    */
    bool hot_log;
    if ((hot_log = (cur_log != &rli->cache_buf)) ||
        DBUG_EVALUATE_IF("force_sql_thread_error", 1, 0))
    {
      DBUG_ASSERT(rli->cur_log_fd == -1); // foreign descriptor
      mysql_mutex_lock(log_lock);

      /*
        Reading xxx_file_id is safe because the log will only
        be rotated when we hold relay_log.LOCK_log
      */
      if (rli->relay_log.get_open_count() != rli->cur_log_old_open_count &&
          DBUG_EVALUATE_IF("force_sql_thread_error", 0, 1))
      {
        // The master has switched to a new log file; Reopen the old log file
        cur_log=reopen_relay_log(rli, &errmsg);
        mysql_mutex_unlock(log_lock);
        if (!cur_log)                           // No more log files
          goto err;
        hot_log=0;                              // Using old binary log
      }
    }
    /*
      As there is no guarantee that the relay is open (for example, an I/O
      error during a write by the slave I/O thread may have closed it), we
      have to test it.
    */
    if (!my_b_inited(cur_log) ||
        DBUG_EVALUATE_IF("force_sql_thread_error", 1, 0))
    {
      if (hot_log)
        mysql_mutex_unlock(log_lock);
      goto err;
    }
#ifndef DBUG_OFF
    {
      DBUG_PRINT("info", ("assertion skip %lu file pos %lu event relay log pos %lu file %s\n",
        (ulong) rli->slave_skip_counter, (ulong) my_b_tell(cur_log),
        (ulong) rli->get_event_relay_log_pos(),
        rli->get_event_relay_log_name()));

      /* This is an assertion which sometimes fails, let's try to track it */
      char llbuf1[22], llbuf2[22];
      DBUG_PRINT("info", ("my_b_tell(cur_log)=%s rli->event_relay_log_pos=%s",
                          llstr(my_b_tell(cur_log),llbuf1),
                          llstr(rli->get_event_relay_log_pos(),llbuf2)));

      DBUG_ASSERT(my_b_tell(cur_log) >= BIN_LOG_HEADER_SIZE);
      DBUG_ASSERT(my_b_tell(cur_log) == rli->get_event_relay_log_pos() ||
                  rli->is_parallel_exec());

      DBUG_PRINT("info", ("next_event group master %s %lu group relay %s %lu event %s %lu\n",
        rli->get_group_master_log_name(),
        (ulong) rli->get_group_master_log_pos(),
        rli->get_group_relay_log_name(),
        (ulong) rli->get_group_relay_log_pos(),
        rli->get_event_relay_log_name(),
        (ulong) rli->get_event_relay_log_pos()));
    }
#endif
    /*
      Relay log is always in new format - if the master is 3.23, the
      I/O thread will convert the format for us.
      A problem: the description event may be in a previous relay log. So if
      the slave has been shutdown meanwhile, we would have to look in old relay
      logs, which may even have been deleted. So we need to write this
      description event at the beginning of the relay log.
      When the relay log is created when the I/O thread starts, easy: the
      master will send the description event and we will queue it.
      But if the relay log is created by new_file(): then the solution is:
      MYSQL_BIN_LOG::open() will write the buffered description event.
    */
    if ((ev= Log_event::read_log_event(cur_log, 0,
                                       rli->get_rli_description_event(),
                                       opt_slave_sql_verify_checksum,
                                       &read_length)))
    {
      DBUG_ASSERT(thd==rli->info_thd);
      /*
        read it while we have a lock, to avoid a mutex lock in
        inc_event_relay_log_pos()
      */
      rli->set_future_event_relay_log_pos(my_b_tell(cur_log));
      ev->future_event_relay_log_pos= rli->get_future_event_relay_log_pos();

      if (hot_log)
        mysql_mutex_unlock(log_lock);
      relay_sql_events++;
      relay_sql_bytes += read_length;
      /*
         MTS checkpoint in the successful read branch
      */
      bool force= (rli->checkpoint_seqno > (rli->checkpoint_group - 1));
      bool period_check= opt_mts_checkpoint_period != 0 &&
                         !rli->curr_group_seen_begin &&
                         !rli->curr_group_seen_gtid;
      if (rli->is_parallel_exec() &&
          (period_check || force))
      {
        ulonglong period= static_cast<ulonglong>(opt_mts_checkpoint_period * 1000000ULL);
        mysql_mutex_unlock(&rli->data_lock);
        /*
          At this point the coordinator has is delegating jobs to workers and
          the checkpoint routine must be periodically invoked.
        */
        (void) mts_checkpoint_routine(rli, period, force, true/*need_data_lock=true*/); // TODO: ALFRANIO ERROR
        DBUG_ASSERT(!force ||
                    (force && (rli->checkpoint_seqno <= (rli->checkpoint_group - 1))) ||
                    sql_slave_killed(thd, rli));
        mysql_mutex_lock(&rli->data_lock);
      }
      DBUG_RETURN(ev);
    }
    DBUG_ASSERT(thd==rli->info_thd);
    if (opt_reckless_slave)                     // For mysql-test
      cur_log->error = 0;
    if (cur_log->error < 0)
    {
      errmsg = "slave SQL thread aborted because of I/O error";
      if (rli->mts_group_status == Relay_log_info::MTS_IN_GROUP)
        /*
          MTS group status is set to MTS_KILLED_GROUP, whenever a read event
          error happens and there was already a non-terminal event scheduled.
        */
        rli->mts_group_status= Relay_log_info::MTS_KILLED_GROUP;
      if (hot_log)
        mysql_mutex_unlock(log_lock);
      goto err;
    }
    if (!cur_log->error) /* EOF */
    {
      ulonglong wait_timer;  /* time wait for more data in binlog */

      /*
        On a hot log, EOF means that there are no more updates to
        process and we must block until I/O thread adds some and
        signals us to continue
      */
      if (hot_log)
      {
        /*
          We say in Seconds_Behind_Master that we have "caught up". Note that
          for example if network link is broken but I/O slave thread hasn't
          noticed it (slave_net_timeout not elapsed), then we'll say "caught
          up" whereas we're not really caught up. Fixing that would require
          internally cutting timeout in smaller pieces in network read, no
          thanks. Another example: SQL has caught up on I/O, now I/O has read
          a new event and is queuing it; the false "0" will exist until SQL
          finishes executing the new event; it will be look abnormal only if
          the events have old timestamps (then you get "many", 0, "many").

          Transient phases like this can be fixed with implemeting
          Heartbeat event which provides the slave the status of the
          master at time the master does not have any new update to send.
          Seconds_Behind_Master would be zero only when master has no
          more updates in binlog for slave. The heartbeat can be sent
          in a (small) fraction of slave_net_timeout. Until it's done
          rli->last_master_timestamp is temporarely (for time of
          waiting for the following event) reset whenever EOF is
          reached.
        */

        /* shows zero while it is sleeping (and until the next event
           is about to be executed).  Note, in MTS case
           Seconds_Behind_Master resetting follows slightly different
           schema where reaching EOF is not enough.  The status
           parameter is updated per some number of processed group of
           events. The number can't be greater than
           @@global.slave_checkpoint_group and anyway SBM updating
           rate does not exceed @@global.slave_checkpoint_period.
           Notice that SBM is set to a new value after processing the
           terminal event (e.g Commit) of a group.  Coordinator resets
           SBM when notices no more groups left neither to read from
           Relay-log nor to process by Workers.
        */
        if (!rli->is_parallel_exec() && reset_seconds_behind_master)
          rli->slave_has_caughtup= Enum_slave_caughtup::YES;

        DBUG_ASSERT(rli->relay_log.get_open_count() ==
                    rli->cur_log_old_open_count);

        if (rli->ign_master_log_name_end[0])
        {
          /* We generate and return a Rotate, to make our positions advance */
          DBUG_PRINT("info",("seeing an ignored end segment"));
          ev= new Rotate_log_event(rli->ign_master_log_name_end,
                                   0, rli->ign_master_log_pos_end,
                                   Rotate_log_event::DUP_NAME);
          rli->ign_master_log_name_end[0]= 0;
          mysql_mutex_unlock(log_lock);
          if (unlikely(!ev))
          {
            errmsg= "Slave SQL thread failed to create a Rotate event "
              "(out of memory?), SHOW SLAVE STATUS may be inaccurate";
            goto err;
          }
          ev->server_id= 0; // don't be ignored by slave SQL thread
          DBUG_RETURN(ev);
        }
        wait_timer = my_timer_now();
        /*
          We can, and should release data_lock while we are waiting for
          update. If we do not, show slave status will block
        */
        mysql_mutex_unlock(&rli->data_lock);

        /*
          Possible deadlock :
          - the I/O thread has reached log_space_limit
          - the SQL thread has read all relay logs, but cannot purge for some
          reason:
            * it has already purged all logs except the current one
            * there are other logs than the current one but they're involved in
            a transaction that finishes in the current one (or is not finished)
          Solution :
          Wake up the possibly waiting I/O thread, and set a boolean asking
          the I/O thread to temporarily ignore the log_space_limit
          constraint, because we do not want the I/O thread to block because of
          space (it's ok if it blocks for any other reason (e.g. because the
          master does not send anything). Then the I/O thread stops waiting
          and reads one more event and starts honoring log_space_limit again.

          If the SQL thread needs more events to be able to rotate the log (it
          might need to finish the current group first), then it can ask for one
          more at a time. Thus we don't outgrow the relay log indefinitely,
          but rather in a controlled manner, until the next rotate.

          When the SQL thread starts it sets ignore_log_space_limit to false.
          We should also reset ignore_log_space_limit to 0 when the user does
          RESET SLAVE, but in fact, no need as RESET SLAVE requires that the slave
          be stopped, and the SQL thread sets ignore_log_space_limit to 0 when
          it stops.
        */
        mysql_mutex_lock(&rli->log_space_lock);

        /*
          If we have reached the limit of the relay space and we
          are going to sleep, waiting for more events:

          1. If outside a group, SQL thread asks the IO thread
             to force a rotation so that the SQL thread purges
             logs next time it processes an event (thus space is
             freed).

          2. If in a group, SQL thread asks the IO thread to
             ignore the limit and queues yet one more event
             so that the SQL thread finishes the group and
             is are able to rotate and purge sometime soon.
         */
        if (rli->log_space_limit &&
            rli->log_space_limit < rli->log_space_total)
        {
          /* force rotation if not in an unfinished group */
          if (!rli->is_parallel_exec())
          {
            rli->sql_force_rotate_relay= !rli->is_in_group();
          }
          else
          {
            rli->sql_force_rotate_relay=
              (rli->mts_group_status != Relay_log_info::MTS_IN_GROUP);
          }
          /* ask for one more event */
          rli->ignore_log_space_limit= true;
        }

        /*
          If the I/O thread is blocked, unblock it.  Ok to broadcast
          after unlock, because the mutex is only destroyed in
          ~Relay_log_info(), i.e. when rli is destroyed, and rli will
          not be destroyed before we exit the present function.
        */
        mysql_mutex_unlock(&rli->log_space_lock);
        mysql_cond_broadcast(&rli->log_space_cond);
        // Note that wait_for_update_relay_log unlocks lock_log !

        if (rli->is_parallel_exec() &&
            (opt_mts_checkpoint_period != 0 ||
            DBUG_EVALUATE_IF("check_slave_debug_group", 1, 0)))
        {
          int ret= 0;
          struct timespec waittime;
          ulonglong period= static_cast<ulonglong>(opt_mts_checkpoint_period * 1000000ULL);
          ulong signal_cnt= rli->relay_log.signal_cnt;

          mysql_mutex_unlock(log_lock);
          do
          {
            /*
              At this point the coordinator has no job to delegate to workers.
              However, workers are executing their assigned jobs and as such
              the checkpoint routine must be periodically invoked.
            */
            (void) mts_checkpoint_routine(rli, period, false, true/*need_data_lock=true*/); // TODO: ALFRANIO ERROR
            mysql_mutex_lock(log_lock);
            // More to the empty relay-log all assigned events done so reset it.
            // Reset the flag of slave_has_caught_up
            if (rli->gaq->empty() &&
                reset_seconds_behind_master)
              rli->slave_has_caughtup= Enum_slave_caughtup::YES;

            if (DBUG_EVALUATE_IF("check_slave_debug_group", 1, 0))
              period= 10000000ULL;

            set_timespec_nsec(waittime, period);
            ret= rli->relay_log.wait_for_update_relay_log(thd, &waittime);
          } while ((ret == ETIMEDOUT || ret == ETIME) /* todo:remove */ &&
                   signal_cnt == rli->relay_log.signal_cnt && !thd->killed);
        }
        else
        {
          rli->relay_log.wait_for_update_relay_log(thd, NULL);
        }

        // re-acquire data lock since we released it earlier
        mysql_mutex_lock(&rli->data_lock);
        relay_sql_wait_time += my_timer_since_and_update(&wait_timer);
        continue;
      }
      /*
        If the log was not hot, we need to move to the next log in
        sequence. The next log could be hot or cold, we deal with both
        cases separately after doing some common initialization
      */
      end_io_cache(cur_log);
      DBUG_ASSERT(rli->cur_log_fd >= 0);
      mysql_file_close(rli->cur_log_fd, MYF(MY_WME));
      rli->cur_log_fd = -1;

      if (!enable_raft_plugin && relay_log_purge)
      {
        /*
          purge_first_log will properly set up relay log coordinates in rli.
          If the group's coordinates are equal to the event's coordinates
          (i.e. the relay log was not rotated in the middle of a group),
          we can purge this relay log too.
          We do ulonglong and string comparisons, this may be slow but
          - purging the last relay log is nice (it can save 1GB of disk), so we
          like to detect the case where we can do it, and given this,
          - I see no better detection method
          - purge_first_log is not called that often
        */
        if (rli->relay_log.purge_first_log
            (rli,
             rli->get_group_relay_log_pos() == rli->get_event_relay_log_pos()
             && !strcmp(rli->get_group_relay_log_name(),rli->get_event_relay_log_name())))
        {
          errmsg = "Error purging processed logs";
          goto err;
        }
        DBUG_PRINT("info", ("next_event group master %s %lu  group relay %s %lu event %s %lu\n",
          rli->get_group_master_log_name(),
          (ulong) rli->get_group_master_log_pos(),
          rli->get_group_relay_log_name(),
          (ulong) rli->get_group_relay_log_pos(),
          rli->get_event_relay_log_name(),
          (ulong) rli->get_event_relay_log_pos()));
      }
      else
      {
        /*
          If hot_log is set, then we already have a lock on
          LOCK_log.  If not, we have to get the lock.

          According to Sasha, the only time this code will ever be executed
          is if we are recovering from a bug.
        */
        if (rli->relay_log.find_next_log(&rli->linfo, !hot_log))
        {
          errmsg = "error switching to the next log";
          goto err;
        }
        rli->set_event_relay_log_pos(BIN_LOG_HEADER_SIZE);
        rli->set_event_relay_log_name(rli->linfo.log_file_name);
        /*
          We may update the worker here but this is not extremlly
          necessary. /Alfranio
        */
        rli->flush_info();
      }

      /* Reset the relay-log-change-notified status of  Slave Workers */
      if (rli->is_parallel_exec())
      {
        DBUG_PRINT("info", ("next_event: MTS group relay log changes to %s %lu\n",
                            rli->get_group_relay_log_name(),
                            (ulong) rli->get_group_relay_log_pos()));
        rli->reset_notified_relay_log_change();
      }

      /*
        Now we want to open this next log. To know if it's a hot log (the one
        being written by the I/O thread now) or a cold log, we can use
        is_active(); if it is hot, we use the I/O cache; if it's cold we open
        the file normally. But if is_active() reports that the log is hot, this
        may change between the test and the consequence of the test. So we may
        open the I/O cache whereas the log is now cold, which is nonsense.
        To guard against this, we need to have LOCK_log.
      */

      DBUG_PRINT("info",("hot_log: %d",hot_log));
      if (!hot_log) /* if hot_log, we already have this mutex */
        mysql_mutex_lock(log_lock);
      if (rli->relay_log.is_active(rli->linfo.log_file_name))
      {
#ifdef EXTRA_DEBUG
        if (log_warnings)
          sql_print_information("next log '%s' is currently active",
                                rli->linfo.log_file_name);
#endif
        rli->cur_log= cur_log= rli->relay_log.get_log_file();
        rli->cur_log_old_open_count= rli->relay_log.get_open_count();
        DBUG_ASSERT(rli->cur_log_fd == -1);

        /*
           When the SQL thread is [stopped and] (re)started the
           following may happen:

           1. Log was hot at stop time and remains hot at restart

              SQL thread reads again from hot_log (SQL thread was
              reading from the active log when it was stopped and the
              very same log is still active on SQL thread restart).

              In this case, my_b_seek is performed on cur_log, while
              cur_log points to relay_log.get_log_file();

           2. Log was hot at stop time but got cold before restart

              The log was hot when SQL thread stopped, but it is not
              anymore when the SQL thread restarts.

              In this case, the SQL thread reopens the log, using
              cache_buf, ie, cur_log points to &cache_buf, and thence
              its coordinates are reset.

           3. Log was already cold at stop time

              The log was not hot when the SQL thread stopped, and, of
              course, it will not be hot when it restarts.

              In this case, the SQL thread opens the cold log again,
              using cache_buf, ie, cur_log points to &cache_buf, and
              thence its coordinates are reset.

           4. Log was hot at stop time, DBA changes to previous cold
              log and restarts SQL thread

              The log was hot when the SQL thread was stopped, but the
              user changed the coordinates of the SQL thread to
              restart from a previous cold log.

              In this case, at start time, cur_log points to a cold
              log, opened using &cache_buf as cache, and coordinates
              are reset. However, as it moves on to the next logs, it
              will eventually reach the hot log. If the hot log is the
              same at the time the SQL thread was stopped, then
              coordinates were not reset - the cur_log will point to
              relay_log.get_log_file(), and not a freshly opened
              IO_CACHE through cache_buf. For this reason we need to
              deploy a my_b_seek before calling check_binlog_magic at
              this point of the code (see: BUG#55263 for more
              details).

          NOTES:
            - We must keep the LOCK_log to read the 4 first bytes, as
              this is a hot log (same as when we call read_log_event()
              above: for a hot log we take the mutex).

            - Because of scenario #4 above, we need to have a
              my_b_seek here. Otherwise, we might hit the assertion
              inside check_binlog_magic.
        */

        my_b_seek(cur_log, (my_off_t) 0);
        if (check_binlog_magic(cur_log,&errmsg))
        {
          if (!hot_log)
            mysql_mutex_unlock(log_lock);
          goto err;
        }
        if (!hot_log)
          mysql_mutex_unlock(log_lock);
        continue;
      }
      if (!hot_log)
        mysql_mutex_unlock(log_lock);
      /*
        if we get here, the log was not hot, so we will have to open it
        ourselves. We are sure that the log is still not hot now (a log can get
        from hot to cold, but not from cold to hot). No need for LOCK_log.
      */
#ifdef EXTRA_DEBUG
      if (log_warnings)
        sql_print_information("next log '%s' is not active",
                              rli->linfo.log_file_name);
#endif
      // open_binlog_file() will check the magic header
      if ((rli->cur_log_fd=open_binlog_file(cur_log,rli->linfo.log_file_name,
                                            &errmsg)) <0)
        goto err;
    }
    else
    {
      /*
        Read failed with a non-EOF error.
        TODO: come up with something better to handle this error
      */
      if (hot_log)
        mysql_mutex_unlock(log_lock);
      sql_print_error("Slave SQL thread: I/O error reading \
event(errno: %d  cur_log->error: %d)",
                      my_errno,cur_log->error);
      // set read position to the beginning of the event
      my_b_seek(cur_log,rli->get_event_relay_log_pos());
      /* otherwise, we have had a partial read */
      errmsg = "Aborting slave SQL thread because of partial event read";
      break;                                    // To end of function
    }
  }
  if (!errmsg && log_warnings)
  {
    sql_print_information("Error reading relay log event: %s",
                          "slave SQL thread was killed");
    DBUG_RETURN(0);
  }

err:
  if (errmsg)
    sql_print_error("Error reading relay log event: %s", errmsg);
  DBUG_RETURN(0);
}

/*
  Rotate a relay log (this is used only by FLUSH LOGS; the automatic rotation
  because of size is simpler because when we do it we already have all relevant
  locks; here we don't, so this function is mainly taking locks).
  Returns nothing as we cannot catch any error (MYSQL_BIN_LOG::new_file()
  is void).
*/

int rotate_relay_log(Master_info* mi, bool need_log_space_lock,
                     RaftRotateInfo *raft_rotate_info)
{
  DBUG_ENTER("rotate_relay_log");

  mysql_mutex_assert_owner(&mi->data_lock);
  DBUG_EXECUTE_IF("crash_before_rotate_relaylog", DBUG_SUICIDE(););

  Relay_log_info* rli= mi->rli;
  int error= 0;
  Format_description_log_event *fde_copy = NULL;

  /*
     We need to test inited because otherwise, new_file() will attempt to lock
     LOCK_log, which may not be inited (if we're not a slave).
  */
  if (!rli->inited)
  {
    DBUG_PRINT("info", ("rli->inited == 0"));
    goto end;
  }

  /*
   * Older FDE events with version <4 are not used during relay log rotation
   * see open_binlog()
   */
  if (mi->get_mi_description_event() &&
      mi->get_mi_description_event()->binlog_version>=4) {
    IO_CACHE tmp_file;
    if ((error=
        init_io_cache(&tmp_file, -1, 200, WRITE_CACHE, 0, 0, MYF(MY_WME)))) {
      goto end;
    }
    mysql_mutex_lock(&mi->fde_lock);
    mi->get_mi_description_event()->write(&tmp_file);
    mysql_mutex_unlock(&mi->fde_lock);
    if ((error= reinit_io_cache(&tmp_file, READ_CACHE, 0, 0, 0))) {
      goto end;
    }
    fde_copy = (Format_description_log_event*)
               Log_event::read_log_event(&tmp_file, NULL,
                                         mi->get_mi_description_event(),
                                         TRUE, NULL);
    end_io_cache(&tmp_file);
  }
  mysql_mutex_unlock(&mi->data_lock);
  /* If the relay log is closed, new_file() will do nothing. */
  error= rli->relay_log.new_file(fde_copy, raft_rotate_info);
  mysql_mutex_lock(&mi->data_lock);
  if (fde_copy)
    delete fde_copy;
  if (error != 0)
    goto end;

  /*
    We harvest now, because otherwise BIN_LOG_HEADER_SIZE will not immediately
    be counted, so imagine a succession of FLUSH LOGS  and assume the slave
    threads are started:
    relay_log_space decreases by the size of the deleted relay log, but does
    not increase, so flush-after-flush we may become negative, which is wrong.
    Even if this will be corrected as soon as a query is replicated on the
    slave (because the I/O thread will then call harvest_bytes_written() which
    will harvest all these BIN_LOG_HEADER_SIZE we forgot), it may give strange
    output in SHOW SLAVE STATUS meanwhile. So we harvest now.
    If the log is closed, then this will just harvest the last writes, probably
    0 as they probably have been harvested.
  */
  rli->relay_log.harvest_bytes_written(rli, need_log_space_lock);
end:
  DBUG_RETURN(error);
}

int rotate_relay_log_for_raft(RaftRotateInfo *rotate_info)
{
  DBUG_ENTER("rotate_relay_log_for_raft");
  Master_info *mi= active_mi;
  int error= 0;
  mysql_mutex_lock(&mi->data_lock);

  /* in case of no_op we would be starting the file name from the master
     so new_log_ident and pos wont be used */
  if (!rotate_info->noop)
  {
    memcpy(const_cast<char *>(mi->get_master_log_name()),
           rotate_info->new_log_ident.c_str(),
           rotate_info->new_log_ident.length() + 1);
    mi->set_master_log_pos(rotate_info->pos);
  }

  // pass pointer, because the absence of pointer (nullptr)
  // conveys non-raft flow.
  error= rotate_relay_log(mi, /* need_space_lock= */ true, rotate_info);

  mysql_mutex_unlock(&active_mi->data_lock);
  DBUG_RETURN(error);
}

/**
   Detects, based on master's version (as found in the relay log), if master
   has a certain bug.
   @param rli Relay_log_info which tells the master's version
   @param bug_id Number of the bug as found in bugs.mysql.com
   @param report bool report error message, default TRUE

   @param pred Predicate function that will be called with @c param to
   check for the bug. If the function return @c true, the bug is present,
   otherwise, it is not.

   @param param  State passed to @c pred function.

   @return TRUE if master has the bug, FALSE if it does not.
*/
bool rpl_master_has_bug(const Relay_log_info *rli, uint bug_id, bool report,
                        bool (*pred)(const void *), const void *param)
{
  struct st_version_range_for_one_bug {
    uint        bug_id;
    const uchar introduced_in[3]; // first version with bug
    const uchar fixed_in[3];      // first version with fix
  };
  static struct st_version_range_for_one_bug versions_for_all_bugs[]=
  {
    {24432, { 5, 0, 24 }, { 5, 0, 38 } },
    {24432, { 5, 1, 12 }, { 5, 1, 17 } },
    {33029, { 5, 0,  0 }, { 5, 0, 58 } },
    {33029, { 5, 1,  0 }, { 5, 1, 12 } },
    {37426, { 5, 1,  0 }, { 5, 1, 26 } },
  };
  const uchar *master_ver=
    rli->get_rli_description_event()->server_version_split;

  DBUG_ASSERT(sizeof(rli->get_rli_description_event()->server_version_split) == 3);

  for (uint i= 0;
       i < sizeof(versions_for_all_bugs)/sizeof(*versions_for_all_bugs);i++)
  {
    const uchar *introduced_in= versions_for_all_bugs[i].introduced_in,
      *fixed_in= versions_for_all_bugs[i].fixed_in;
    if ((versions_for_all_bugs[i].bug_id == bug_id) &&
        (memcmp(introduced_in, master_ver, 3) <= 0) &&
        (memcmp(fixed_in,      master_ver, 3) >  0) &&
        (pred == NULL || (*pred)(param)))
    {
      enum loglevel report_level= INFORMATION_LEVEL;
      if (!report)
	return TRUE;
      // a short message for SHOW SLAVE STATUS (message length constraints)
      my_printf_error(ER_UNKNOWN_ERROR, "master may suffer from"
                      " http://bugs.mysql.com/bug.php?id=%u"
                      " so slave stops; check error log on slave"
                      " for more info", MYF(0), bug_id);
      // a verbose message for the error log
      if (!ignored_error_code(ER_UNKNOWN_ERROR))
      {
        report_level= ERROR_LEVEL;
        current_thd->is_slave_error= 1;
      }
      /* In case of ignored errors report warnings only if log_warnings > 1. */
      else if (log_warnings > 1)
        report_level= WARNING_LEVEL;

      if (report_level != INFORMATION_LEVEL)
        rli->report(report_level, ER_UNKNOWN_ERROR,
                    "According to the master's version ('%s'),"
                    " it is probable that master suffers from this bug:"
                    " http://bugs.mysql.com/bug.php?id=%u"
                    " and thus replicating the current binary log event"
                    " may make the slave's data become different from the"
                    " master's data."
                    " To take no risk, slave refuses to replicate"
                    " this event and stops."
                    " We recommend that all updates be stopped on the"
                    " master and slave, that the data of both be"
                    " manually synchronized,"
                    " that master's binary logs be deleted,"
                    " that master be upgraded to a version at least"
                    " equal to '%d.%d.%d'. Then replication can be"
                    " restarted.",
                    rli->get_rli_description_event()->server_version,
                    bug_id,
                    fixed_in[0], fixed_in[1], fixed_in[2]);
      return TRUE;
    }
  }
  return FALSE;
}

/**
   BUG#33029, For all 5.0 up to 5.0.58 exclusive, and 5.1 up to 5.1.12
   exclusive, if one statement in a SP generated AUTO_INCREMENT value
   by the top statement, all statements after it would be considered
   generated AUTO_INCREMENT value by the top statement, and a
   erroneous INSERT_ID value might be associated with these statement,
   which could cause duplicate entry error and stop the slave.

   Detect buggy master to work around.
 */
bool rpl_master_erroneous_autoinc(THD *thd)
{
  if (active_mi != NULL && active_mi->rli->info_thd == thd)
  {
    Relay_log_info *rli= active_mi->rli;
    DBUG_EXECUTE_IF("simulate_bug33029", return TRUE;);
    return rpl_master_has_bug(rli, 33029, FALSE, NULL, NULL);
  }
  return FALSE;
}

/**
  a copy of active_mi->rli->slave_skip_counter, for showing in SHOW VARIABLES,
  INFORMATION_SCHEMA.GLOBAL_VARIABLES and @@sql_slave_skip_counter without
  taking all the mutexes needed to access active_mi->rli->slave_skip_counter
  properly.
*/
uint sql_slave_skip_counter;

/**
  Execute a START SLAVE statement.

  @param thd Pointer to THD object for the client thread executing the
  statement.

  @param mi Pointer to Master_info object for the slave's IO thread.

  @param net_report If true, saves the exit status into Diagnostics_area.

  @retval 0 success
  @retval 1 error
*/
int start_slave(THD* thd , Master_info* mi,  bool net_report)
{
  int slave_errno= 0;
  int thread_mask;
  DBUG_ENTER("start_slave");

  if (check_access(thd, SUPER_ACL, any_db, NULL, NULL, 0, 0))
    DBUG_RETURN(1);

  if (thd->lex->slave_connection.user ||
      thd->lex->slave_connection.password)
  {
#if defined(HAVE_OPENSSL) && !defined(EMBEDDED_LIBRARY)
    if (thd->vio_ok() && !thd->get_net()->vio->ssl_arg)
      push_warning(thd, Sql_condition::WARN_LEVEL_NOTE,
                   ER_INSECURE_PLAIN_TEXT,
                   ER(ER_INSECURE_PLAIN_TEXT));
#endif
#if !defined(HAVE_OPENSSL) && !defined(EMBEDDED_LIBRARY)
    push_warning(thd, Sql_condition::WARN_LEVEL_NOTE,
                 ER_INSECURE_PLAIN_TEXT,
                 ER(ER_INSECURE_PLAIN_TEXT));
#endif
  }

  log_slave_command(thd);

  lock_slave_threads(mi);  // this allows us to cleanly read slave_running
  // Get a mask of _stopped_ threads
  init_thread_mask(&thread_mask,mi,1 /* inverse */);
  /*
    Below we will start all stopped threads.  But if the user wants to
    start only one thread, do as if the other thread was running (as we
    don't wan't to touch the other thread), so set the bit to 0 for the
    other thread
  */
  if (thd->lex->slave_thd_opt)
    thread_mask&= thd->lex->slave_thd_opt;

  // case: no IO thread in raft mode
  if (enable_raft_plugin)
    thread_mask&= ~SLAVE_IO;

  if (thread_mask) //some threads are stopped, start them
  {
    if (global_init_info(mi, false, thread_mask))
    {
      slave_errno=ER_MASTER_INFO;
      if (enable_raft_plugin)
      {
        // NO_LINT_DEBUG
        sql_print_error("start_slave: error as global_init_info failed");
      }
    }
    else if (server_id_supplied && *mi->host)
    {
      /*
        If we will start IO thread we need to take care of possible
        options provided through the START SLAVE if there is any.
      */
      if (thread_mask & SLAVE_IO)
      {
        DBUG_ASSERT(!enable_raft_plugin);
        if (thd->lex->slave_connection.user)
        {
          mi->set_start_user_configured(true);
          mi->set_user(thd->lex->slave_connection.user);
        }
        if (thd->lex->slave_connection.password)
        {
          mi->set_start_user_configured(true);
          mi->set_password(thd->lex->slave_connection.password,
                           strlen(thd->lex->slave_connection.password));
        }
        if (thd->lex->slave_connection.plugin_auth)
          mi->set_plugin_auth(thd->lex->slave_connection.plugin_auth);
        if (thd->lex->slave_connection.plugin_dir)
          mi->set_plugin_dir(thd->lex->slave_connection.plugin_dir);
      }

      /*
        If we will start SQL thread we will care about UNTIL options If
        not and they are specified we will ignore them and warn user
        about this fact.
      */
      if (thread_mask & SLAVE_SQL)
      {
        /*
          To cache the MTS system var values and used them in the following
          runtime. The system var:s can change meanwhile but having no other
          effects.
        */
        mi->rli->opt_slave_parallel_workers= opt_mts_slave_parallel_workers;
#ifndef DBUG_OFF
        if (!DBUG_EVALUATE_IF("check_slave_debug_group", 1, 0))
#endif
          mi->rli->checkpoint_group= opt_mts_checkpoint_group;

        mysql_mutex_lock(&mi->rli->data_lock);

        if (thd->lex->mi.pos)
        {
          if (thd->lex->mi.relay_log_pos)
            slave_errno= ER_BAD_SLAVE_UNTIL_COND;
          mi->rli->until_condition= Relay_log_info::UNTIL_MASTER_POS;
          mi->rli->until_log_pos= thd->lex->mi.pos;
          /*
             We don't check thd->lex->mi.log_file_name for NULL here
             since it is checked in sql_yacc.yy
          */
          strmake(mi->rli->until_log_name, thd->lex->mi.log_file_name,
                  sizeof(mi->rli->until_log_name)-1);
        }
        else if (thd->lex->mi.relay_log_pos)
        {
          if (thd->lex->mi.pos)
            slave_errno= ER_BAD_SLAVE_UNTIL_COND;
          mi->rli->until_condition= Relay_log_info::UNTIL_RELAY_POS;
          mi->rli->until_log_pos= thd->lex->mi.relay_log_pos;
          strmake(mi->rli->until_log_name, thd->lex->mi.relay_log_name,
                  sizeof(mi->rli->until_log_name)-1);
        }
        else if (thd->lex->mi.gtid)
        {
          global_sid_lock->wrlock();
          mi->rli->clear_until_condition();
          if (mi->rli->until_sql_gtids.add_gtid_text(thd->lex->mi.gtid)
              != RETURN_STATUS_OK)
            slave_errno= ER_BAD_SLAVE_UNTIL_COND;
          else {
            mi->rli->until_condition=
              LEX_MASTER_INFO::UNTIL_SQL_BEFORE_GTIDS == thd->lex->mi.gtid_until_condition
              ? Relay_log_info::UNTIL_SQL_BEFORE_GTIDS
              : Relay_log_info::UNTIL_SQL_AFTER_GTIDS;
            if ((mi->rli->until_condition ==
               Relay_log_info::UNTIL_SQL_AFTER_GTIDS) &&
               mi->rli->opt_slave_parallel_workers != 0)
            {
              mi->rli->opt_slave_parallel_workers= 0;
              push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                                  ER_MTS_FEATURE_IS_NOT_SUPPORTED,
                                  ER(ER_MTS_FEATURE_IS_NOT_SUPPORTED),
                                  "UNTIL condtion",
                                  "Slave is started in the sequential execution mode.");
            }
          }
          global_sid_lock->unlock();
        }
        else if (thd->lex->mi.until_after_gaps)
        {
            mi->rli->until_condition= Relay_log_info::UNTIL_SQL_AFTER_MTS_GAPS;
            mi->rli->opt_slave_parallel_workers=
              mi->rli->recovery_parallel_workers;
        }
        else
          mi->rli->clear_until_condition();

        if (mi->rli->until_condition == Relay_log_info::UNTIL_MASTER_POS ||
            mi->rli->until_condition == Relay_log_info::UNTIL_RELAY_POS)
        {
          /* Preparing members for effective until condition checking */
          const char *p= fn_ext(mi->rli->until_log_name);
          char *p_end;
          if (*p)
          {
            //p points to '.'
            mi->rli->until_log_name_extension= strtoul(++p,&p_end, 10);
            /*
              p_end points to the first invalid character. If it equals
              to p, no digits were found, error. If it contains '\0' it
              means  conversion went ok.
            */
            if (p_end==p || *p_end)
              slave_errno=ER_BAD_SLAVE_UNTIL_COND;
          }
          else
            slave_errno=ER_BAD_SLAVE_UNTIL_COND;

          /* mark the cached result of the UNTIL comparison as "undefined" */
          mi->rli->until_log_names_cmp_result=
            Relay_log_info::UNTIL_LOG_NAMES_CMP_UNKNOWN;

          /* Issuing warning then started without --skip-slave-start */
          if (!opt_skip_slave_start)
            push_warning(thd, Sql_condition::WARN_LEVEL_NOTE,
                         ER_MISSING_SKIP_SLAVE,
                         ER(ER_MISSING_SKIP_SLAVE));
          if (mi->rli->opt_slave_parallel_workers != 0)
          {
            mi->rli->opt_slave_parallel_workers= 0;
            push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                                ER_MTS_FEATURE_IS_NOT_SUPPORTED,
                                ER(ER_MTS_FEATURE_IS_NOT_SUPPORTED),
                                "UNTIL condtion",
                                "Slave is started in the sequential execution mode.");
          }
        }

        mysql_mutex_unlock(&mi->rli->data_lock);
      }
      else if (thd->lex->mi.pos || thd->lex->mi.relay_log_pos || thd->lex->mi.gtid)
        push_warning(thd, Sql_condition::WARN_LEVEL_NOTE, ER_UNTIL_COND_IGNORED,
                     ER(ER_UNTIL_COND_IGNORED));

      if (!slave_errno)
        slave_errno = start_slave_threads(false/*need_lock_slave=false*/,
                                          true/*wait_for_start=true*/,
                                          mi,
                                          thread_mask);
    }
    else
      slave_errno = ER_BAD_SLAVE;
  }
  else
  {
    /* no error if all threads are already started, only a warning */
    push_warning(thd, Sql_condition::WARN_LEVEL_NOTE, ER_SLAVE_WAS_RUNNING,
                 ER(ER_SLAVE_WAS_RUNNING));
  }

  /*
    Clean up start information if there was an attempt to start
    the IO thread to avoid any security issue.
  */
  if (slave_errno &&
      (thread_mask & SLAVE_IO) == SLAVE_IO)
    mi->reset_start_info();

  unlock_slave_threads(mi);

  if (slave_errno)
  {
    if (net_report)
      my_message(slave_errno, ER(slave_errno), MYF(0));
    DBUG_RETURN(1);
  }
  else if (net_report)
    my_ok(thd);

  DBUG_RETURN(0);
}

int raft_stop_io_thread(THD *thd)
{
  int res= 0;
  thd->lex->slave_thd_opt = SLAVE_IO;
  mysql_mutex_lock(&LOCK_active_mi);
  if (!active_mi)
  {
    goto end;
  }

  res= stop_slave(thd, active_mi, false);

end:
  mysql_mutex_unlock(&LOCK_active_mi);
  return res;
}

int raft_stop_sql_thread(THD *thd)
{
  int res= 0;
  thd->lex->slave_thd_opt = SLAVE_SQL;
  mysql_mutex_lock(&LOCK_active_mi);
  if (!active_mi)
  {
    goto end;
  }

  res= stop_slave(thd, active_mi, false);

end:
  mysql_mutex_unlock(&LOCK_active_mi);
  return res;
}

int raft_start_sql_thread(THD *thd)
{
  int res= 0;
  thd->lex->slave_thd_opt = SLAVE_SQL;
  mysql_mutex_lock(&LOCK_active_mi);
  if (!active_mi)
  {
    goto end;
  }

  res= start_slave(thd, active_mi, false);

end:
  mysql_mutex_unlock(&LOCK_active_mi);
  return res;
}

/**
  Execute a STOP SLAVE statement.

  @param thd Pointer to THD object for the client thread executing the
  statement.

  @param mi Pointer to Master_info object for the slave's IO thread.

  @param net_report If true, saves the exit status into Diagnostics_area.

  @retval 0 success
  @retval 1 error
*/
int stop_slave(THD* thd, Master_info* mi, bool net_report )
{
  DBUG_ENTER("stop_slave");

  int slave_errno;
  if (!thd)
    thd = current_thd;

  if (check_access(thd, SUPER_ACL, any_db, NULL, NULL, 0, 0))
    DBUG_RETURN(1);

  log_slave_command(thd);

  THD_STAGE_INFO(thd, stage_killing_slave);
  int thread_mask;
  lock_slave_threads(mi);
  // Get a mask of _running_ threads
  init_thread_mask(&thread_mask,mi,0 /* not inverse*/);
  /*
    Below we will stop all running threads.
    But if the user wants to stop only one thread, do as if the other thread
    was stopped (as we don't wan't to touch the other thread), so set the
    bit to 0 for the other thread
  */
  if (thd->lex->slave_thd_opt)
    thread_mask &= thd->lex->slave_thd_opt;

  if (thread_mask)
  {
    slave_errno= terminate_slave_threads(mi,thread_mask,
                                         false/*need_lock_term=false*/);
  }
  else
  {
    //no error if both threads are already stopped, only a warning
    slave_errno= 0;
    // case: don't push a warning when raft is enabled and IO thread is stopped
    // (because IO thread is always stopped in raft mode)
    if (!(thd->lex->slave_thd_opt == 1 && enable_raft_plugin))
    {
      push_warning(thd, Sql_condition::WARN_LEVEL_NOTE,
                   ER_SLAVE_WAS_NOT_RUNNING, ER(ER_SLAVE_WAS_NOT_RUNNING));
    }
  }
  unlock_slave_threads(mi);

  if (slave_errno)
  {
    if ((slave_errno == ER_STOP_SLAVE_SQL_THREAD_TIMEOUT) ||
        (slave_errno == ER_STOP_SLAVE_IO_THREAD_TIMEOUT))
    {
      push_warning(thd, Sql_condition::WARN_LEVEL_NOTE, slave_errno,
                   ER(slave_errno));
      sql_print_warning("%s",ER(slave_errno));
    }
    if (net_report)
      my_message(slave_errno, ER(slave_errno), MYF(0));
    DBUG_RETURN(1);
  }
  else if (net_report)
    my_ok(thd);

  DBUG_RETURN(0);
}


/**
  Execute a RESET SLAVE statement.

  @param thd Pointer to THD object of the client thread executing the
  statement.

  @param mi Pointer to Master_info object for the slave.

  @retval 0 success
  @retval 1 error
*/
int reset_slave(THD *thd, Master_info* mi, bool purge)
{
  int thread_mask= 0, error= 0;
  uint sql_errno=ER_UNKNOWN_ERROR;
  const char* errmsg= "Unknown error occured while reseting slave";
  DBUG_ENTER("reset_slave");

  if (enable_raft_plugin && !override_enable_raft_check)
  {
    // NO_LINT_DEBUG
    sql_print_information(
        "Did not allow reset_slave as enable_raft_plugin is ON");
    my_error(ER_RAFT_OPERATION_INCOMPATIBLE, MYF(0),
        "reset slave not allowed when enable_raft_plugin is ON");
    DBUG_RETURN(1);
  }

  lock_slave_threads(mi);
  init_thread_mask(&thread_mask,mi,0 /* not inverse */);
  if (thread_mask) // We refuse if any slave thread is running
  {
    sql_errno= ER_SLAVE_MUST_STOP;
    error=1;
    goto err;
  }

  log_slave_command(thd);
  ha_reset_slave(thd);

  // delete relay logs, clear relay log coordinates
  if (purge)
  {
    if ((error= mi->rli->purge_relay_logs(thd,
                                        1 /* just reset */,
                                        &errmsg)))
    {
      sql_errno= ER_RELAY_LOG_FAIL;
      goto err;
    }
  }

  mysql_bin_log.last_master_timestamp.store(0);

  /* Clear master's log coordinates and associated information */
  DBUG_ASSERT(!mi->rli || !mi->rli->slave_running); // none writes in rli table
  mi->clear_in_memory_info(thd->lex->reset_slave_info.all);

  if (remove_info(mi))
  {
    error= 1;
    goto err;
  }

  is_slave = mi->host[0];
  (void) RUN_HOOK(binlog_relay_io, after_reset_slave, (thd, mi));
err:
  unlock_slave_threads(mi);
  if (error)
    my_error(sql_errno, MYF(0), errmsg);
  DBUG_RETURN(error);
}

/**
  Execute a CHANGE MASTER statement. MTS workers info tables data are removed
  in the successful branch (i.e. there are no gaps in the execution history).

  @param thd Pointer to THD object for the client thread executing the
  statement.

  @param mi Pointer to Master_info object belonging to the slave's IO
  thread.

  @retval FALSE success
  @retval TRUE error
*/
bool change_master(THD* thd, Master_info* mi)
{
  int thread_mask;
  const char* errmsg= 0;
  bool need_relay_log_purge= 1;
  char *var_master_log_name= NULL, *var_group_master_log_name= NULL;
  bool ret= false;
  char saved_host[HOSTNAME_LENGTH + 1], saved_bind_addr[HOSTNAME_LENGTH + 1];
  uint saved_port= 0;
  char saved_log_name[FN_REFLEN];
  my_off_t saved_log_pos= 0;
  my_bool save_relay_log_purge= relay_log_purge;
  bool mts_remove_workers= false;

  DBUG_ENTER("change_master");
  if (enable_raft_plugin && !override_enable_raft_check)
  {
    // NO_LINT_DEBUG
    sql_print_information(
        "Did not allow change_master as enable_raft_plugin is ON");
    my_error(ER_RAFT_OPERATION_INCOMPATIBLE, MYF(0),
        "change master not allowed when enable_raft_plugin is ON");
    DBUG_RETURN(1);
  }

  lock_slave_threads(mi);
  init_thread_mask(&thread_mask,mi,0 /*not inverse*/);
  LEX_MASTER_INFO* lex_mi= &thd->lex->mi;
  if (thread_mask) // We refuse if any slave thread is running
  {
    my_message(ER_SLAVE_MUST_STOP, ER(ER_SLAVE_MUST_STOP), MYF(0));
    ret= true;
    goto err;
  }
  thread_mask= SLAVE_IO | SLAVE_SQL;
  log_slave_command(thd);

  THD_STAGE_INFO(thd, stage_changing_master);
  /*
    We need to check if there is an empty master_host. Otherwise
    change master succeeds, a master.info file is created containing
    empty master_host string and when issuing: start slave; an error
    is thrown stating that the server is not configured as slave.
    (See BUG#28796).
  */
  if(lex_mi->host && !*lex_mi->host)
  {
    my_error(ER_WRONG_ARGUMENTS, MYF(0), "MASTER_HOST");
    unlock_slave_threads(mi);
    DBUG_RETURN(TRUE);
  }
  if (global_init_info(mi, false, thread_mask))
  {
    my_message(ER_MASTER_INFO, ER(ER_MASTER_INFO), MYF(0));
    ret= true;
    goto err;
  }
  if (mi->rli->mts_recovery_group_cnt)
  {
    /*
      Change-Master can't be done if there is a mts group gap.
      That requires mts-recovery which START SLAVE provides.
    */
    DBUG_ASSERT(mi->rli->recovery_parallel_workers);

    my_message(ER_MTS_CHANGE_MASTER_CANT_RUN_WITH_GAPS,
               ER(ER_MTS_CHANGE_MASTER_CANT_RUN_WITH_GAPS), MYF(0));
    ret= true;
    goto err;
  }
  else
  {
    /*
      Lack of mts group gaps makes Workers info stale
      regardless of need_relay_log_purge computation.
    */
    if (mi->rli->recovery_parallel_workers)
      mts_remove_workers= true;
  }
  /*
    We cannot specify auto position and set either the coordinates
    on master or slave. If we try to do so, an error message is
    printed out.
  */
  if (lex_mi->log_file_name != NULL || lex_mi->pos != 0 ||
      lex_mi->relay_log_name != NULL || lex_mi->relay_log_pos != 0)
  {
    /*
      Disable auto-position when master_auto_position is not specified
      in CHANGE MASTER TO command but coordinates are specified.
    */
    if (lex_mi->auto_position == LEX_MASTER_INFO::LEX_MI_UNCHANGED)
    {
      mi->set_auto_position(false);
    }
    else if (lex_mi->auto_position == LEX_MASTER_INFO::LEX_MI_ENABLE ||
             (lex_mi->auto_position != LEX_MASTER_INFO::LEX_MI_DISABLE &&
              mi->is_auto_position()))
    {
      my_message(ER_BAD_SLAVE_AUTO_POSITION,
                 ER(ER_BAD_SLAVE_AUTO_POSITION), MYF(0));
      ret= true;
      goto err;
    }
  }

  // CHANGE MASTER TO MASTER_AUTO_POSITION = 1 requires GTID_MODE = ON
  if (lex_mi->auto_position == LEX_MASTER_INFO::LEX_MI_ENABLE && gtid_mode != 3)
  {
    my_message(ER_AUTO_POSITION_REQUIRES_GTID_MODE_ON,
               ER(ER_AUTO_POSITION_REQUIRES_GTID_MODE_ON), MYF(0));
    ret= true;
    goto err;
  }

  /*
    Data lock not needed since we have already stopped the running threads,
    and we have the hold on the run locks which will keep all threads that
    could possibly modify the data structures from running
  */

  /*
    Before processing the command, save the previous state.
  */
  strmake(saved_host, mi->host, HOSTNAME_LENGTH);
  strmake(saved_bind_addr, mi->bind_addr, HOSTNAME_LENGTH);
  saved_port= mi->port;
  strmake(saved_log_name, mi->get_master_log_name(), FN_REFLEN - 1);
  saved_log_pos= mi->get_master_log_pos();

  /*
    If the user specified host or port without binlog or position,
    reset binlog's name to FIRST and position to 4.
  */

  if ((lex_mi->host && strcmp(lex_mi->host, mi->host)) ||
      (lex_mi->port && lex_mi->port != mi->port))
  {
    /*
      This is necessary because the primary key, i.e. host or port, has
      changed.

      The repository does not support direct changes on the primary key,
      so the row is dropped and re-inserted with a new primary key. If we
      don't do that, the master info repository we will end up with several
      rows.
    */
    if (mi->clean_info())
    {
      ret= true;
      goto err;
    }
    mi->master_uuid[0]= 0;
    mi->master_id= 0;
  }

  if ((lex_mi->host || lex_mi->port) && !lex_mi->log_file_name && !lex_mi->pos)
  {
    var_master_log_name= const_cast<char*>(mi->get_master_log_name());
    var_master_log_name[0]= '\0';
    mi->set_master_log_pos(BIN_LOG_HEADER_SIZE);
  }

  if (lex_mi->log_file_name)
    mi->set_master_log_name(lex_mi->log_file_name);
  if (lex_mi->pos)
  {
    mi->set_master_log_pos(lex_mi->pos);
  }
  DBUG_PRINT("info", ("master_log_pos: %lu", (ulong) mi->get_master_log_pos()));

  if (lex_mi->user || lex_mi->password)
  {
#if defined(HAVE_OPENSSL) && !defined(EMBEDDED_LIBRARY)
    if (thd->vio_ok() && !thd->get_net()->vio->ssl_arg)
      push_warning(thd, Sql_condition::WARN_LEVEL_NOTE,
                   ER_INSECURE_PLAIN_TEXT,
                   ER(ER_INSECURE_PLAIN_TEXT));
#endif
#if !defined(HAVE_OPENSSL) && !defined(EMBEDDED_LIBRARY)
    push_warning(thd, Sql_condition::WARN_LEVEL_NOTE,
                 ER_INSECURE_PLAIN_TEXT,
                 ER(ER_INSECURE_PLAIN_TEXT));
#endif
    push_warning(thd, Sql_condition::WARN_LEVEL_NOTE,
                 ER_INSECURE_CHANGE_MASTER,
                 ER(ER_INSECURE_CHANGE_MASTER));
  }

  if (lex_mi->user)
    mi->set_user(lex_mi->user);

  if (lex_mi->password)
  {
    if (mi->set_password(lex_mi->password, strlen(lex_mi->password)))
    {
      /*
        After implementing WL#5769, we should create a better error message
        to denote that the call may have failed due to an error while trying
        to encrypt/store the password in a secure key store.
      */
      my_message(ER_MASTER_INFO, ER(ER_MASTER_INFO), MYF(0));
      ret= false;
      goto err;
    }
  }
  if (lex_mi->host)
    strmake(mi->host, lex_mi->host, sizeof(mi->host)-1);
  if (lex_mi->bind_addr)
    strmake(mi->bind_addr, lex_mi->bind_addr, sizeof(mi->bind_addr)-1);
  if (lex_mi->port)
    mi->port = lex_mi->port;
  if (lex_mi->connect_retry)
    mi->connect_retry = lex_mi->connect_retry;
  if (lex_mi->retry_count_opt !=  LEX_MASTER_INFO::LEX_MI_UNCHANGED)
    mi->retry_count = lex_mi->retry_count;
  if (lex_mi->heartbeat_opt != LEX_MASTER_INFO::LEX_MI_UNCHANGED)
    mi->heartbeat_period = lex_mi->heartbeat_period;
  else
    mi->heartbeat_period= min<float>(SLAVE_MAX_HEARTBEAT_PERIOD,
                                     (slave_net_timeout/2.0));
  mi->received_heartbeats= LL(0); // counter lives until master is CHANGEd
  /*
    reset the last time server_id list if the current CHANGE MASTER
    is mentioning IGNORE_SERVER_IDS= (...)
  */
  if (lex_mi->repl_ignore_server_ids_opt == LEX_MASTER_INFO::LEX_MI_ENABLE)
    reset_dynamic(&(mi->ignore_server_ids->dynamic_ids));
  for (uint i= 0; i < lex_mi->repl_ignore_server_ids.elements; i++)
  {
    ulong s_id;
    get_dynamic(&lex_mi->repl_ignore_server_ids, (uchar*) &s_id, i);
    if (s_id == ::server_id && replicate_same_server_id)
    {
      my_error(ER_SLAVE_IGNORE_SERVER_IDS, MYF(0), static_cast<int>(s_id));
      ret= TRUE;
      goto err;
    }
    else
    {
      if (bsearch((const ulong *) &s_id,
                  mi->ignore_server_ids->dynamic_ids.buffer,
                  mi->ignore_server_ids->dynamic_ids.elements, sizeof(ulong),
                  (int (*) (const void*, const void*))
                  change_master_server_id_cmp) == NULL)
        insert_dynamic(&(mi->ignore_server_ids->dynamic_ids), (uchar*) &s_id);
    }
  }
  sort_dynamic(&(mi->ignore_server_ids->dynamic_ids), (qsort_cmp) change_master_server_id_cmp);

  if (lex_mi->ssl != LEX_MASTER_INFO::LEX_MI_UNCHANGED)
    mi->ssl= (lex_mi->ssl == LEX_MASTER_INFO::LEX_MI_ENABLE);

  if (lex_mi->sql_delay != -1)
    mi->rli->set_sql_delay(lex_mi->sql_delay);

  if (lex_mi->ssl_verify_server_cert != LEX_MASTER_INFO::LEX_MI_UNCHANGED)
    mi->ssl_verify_server_cert=
      (lex_mi->ssl_verify_server_cert == LEX_MASTER_INFO::LEX_MI_ENABLE);

  if (lex_mi->ssl_ca)
    strmake(mi->ssl_ca, lex_mi->ssl_ca, sizeof(mi->ssl_ca)-1);
  if (lex_mi->ssl_capath)
    strmake(mi->ssl_capath, lex_mi->ssl_capath, sizeof(mi->ssl_capath)-1);
  if (lex_mi->ssl_cert)
    strmake(mi->ssl_cert, lex_mi->ssl_cert, sizeof(mi->ssl_cert)-1);
  if (lex_mi->ssl_cipher)
    strmake(mi->ssl_cipher, lex_mi->ssl_cipher, sizeof(mi->ssl_cipher)-1);
  if (lex_mi->ssl_key)
    strmake(mi->ssl_key, lex_mi->ssl_key, sizeof(mi->ssl_key)-1);
  if (lex_mi->ssl_crl)
    strmake(mi->ssl_crl, lex_mi->ssl_crl, sizeof(mi->ssl_crl)-1);
  if (lex_mi->ssl_crlpath)
    strmake(mi->ssl_crlpath, lex_mi->ssl_crlpath, sizeof(mi->ssl_crlpath)-1);
#ifndef HAVE_OPENSSL
  if (lex_mi->ssl || lex_mi->ssl_ca || lex_mi->ssl_capath ||
      lex_mi->ssl_cert || lex_mi->ssl_cipher || lex_mi->ssl_key ||
      lex_mi->ssl_verify_server_cert || lex_mi->ssl_crl || lex_mi->ssl_crlpath)
    push_warning(thd, Sql_condition::WARN_LEVEL_NOTE,
                 ER_SLAVE_IGNORED_SSL_PARAMS, ER(ER_SLAVE_IGNORED_SSL_PARAMS));
#endif

  if (lex_mi->relay_log_name)
  {
    need_relay_log_purge= 0;
    char relay_log_name[FN_REFLEN];

    mi->rli->relay_log.make_log_name(relay_log_name, lex_mi->relay_log_name);
    mi->rli->set_group_relay_log_name(relay_log_name);
    mi->rli->set_event_relay_log_name(relay_log_name);
  }

  if (lex_mi->relay_log_pos)
  {
    need_relay_log_purge= 0;
    mi->rli->set_group_relay_log_pos(lex_mi->relay_log_pos);
    mi->rli->set_event_relay_log_pos(lex_mi->relay_log_pos);
  }

  /*
    If user did specify neither host nor port nor any log name nor any log
    pos, i.e. he specified only user/password/master_connect_retry, he probably
    wants replication to resume from where it had left, i.e. from the
    coordinates of the **SQL** thread (imagine the case where the I/O is ahead
    of the SQL; restarting from the coordinates of the I/O would lose some
    events which is probably unwanted when you are just doing minor changes
    like changing master_connect_retry).
    A side-effect is that if only the I/O thread was started, this thread may
    restart from ''/4 after the CHANGE MASTER. That's a minor problem (it is a
    much more unlikely situation than the one we are fixing here).
    Note: coordinates of the SQL thread must be read here, before the
    'if (need_relay_log_purge)' block which resets them.
  */
  if (!lex_mi->host && !lex_mi->port &&
      !lex_mi->log_file_name && !lex_mi->pos &&
      need_relay_log_purge)
   {
     /*
       Sometimes mi->rli->master_log_pos == 0 (it happens when the SQL thread is
       not initialized), so we use a max().
       What happens to mi->rli->master_log_pos during the initialization stages
       of replication is not 100% clear, so we guard against problems using
       max().
      */
     mi->set_master_log_pos(max<ulonglong>(BIN_LOG_HEADER_SIZE,
                                           mi->rli->get_group_master_log_pos()));
     mi->set_master_log_name(mi->rli->get_group_master_log_name());
  }

  /*
    Sets if the slave should connect to the master and look for
    GTIDs.
  */
  if (lex_mi->auto_position != LEX_MASTER_INFO::LEX_MI_UNCHANGED)
    mi->set_auto_position(
      (lex_mi->auto_position == LEX_MASTER_INFO::LEX_MI_ENABLE));

  /*
    Relay log's IO_CACHE may not be inited, if rli->inited==0 (server was never
    a slave before).
  */
  if (flush_master_info(mi, true))
  {
    my_error(ER_RELAY_LOG_INIT, MYF(0), "Failed to flush master info file");
    ret= TRUE;
    goto err;
  }
  if (need_relay_log_purge)
  {
    relay_log_purge= 1;
    THD_STAGE_INFO(thd, stage_purging_old_relay_logs);
    if (mi->rli->purge_relay_logs(thd,
                                  0 /* not only reset, but also reinit */,
                                  &errmsg))
    {
      my_error(ER_RELAY_LOG_FAIL, MYF(0), errmsg);
      ret= TRUE;
      goto err;
    }
  }
  else
  {
    const char* msg;
    relay_log_purge= 0;
    /* Relay log is already initialized */

    if (mi->rli->init_relay_log_pos(mi->rli->get_group_relay_log_name(),
                                    mi->rli->get_group_relay_log_pos(),
                                    true/*need_data_lock=true*/,
                                    &msg, 0))
    {
      my_error(ER_RELAY_LOG_INIT, MYF(0), msg);
      ret= TRUE;
      goto err;
    }
  }
  relay_log_purge= save_relay_log_purge;

  /*
    Coordinates in rli were spoilt by the 'if (need_relay_log_purge)' block,
    so restore them to good values. If we left them to ''/0, that would work;
    but that would fail in the case of 2 successive CHANGE MASTER (without a
    START SLAVE in between): because first one would set the coords in mi to
    the good values of those in rli, the set those in rli to ''/0, then
    second CHANGE MASTER would set the coords in mi to those of rli, i.e. to
    ''/0: we have lost all copies of the original good coordinates.
    That's why we always save good coords in rli.
  */
  if (need_relay_log_purge)
  {
    mi->rli->set_group_master_log_pos(mi->get_master_log_pos());
    DBUG_PRINT("info", ("master_log_pos: %lu", (ulong) mi->get_master_log_pos()));
    mi->rli->set_group_master_log_name(mi->get_master_log_name());
  }
  var_group_master_log_name=  const_cast<char *>(mi->rli->get_group_master_log_name());
  if (!var_group_master_log_name[0]) // uninitialized case
    mi->rli->set_group_master_log_pos(0);

  mysql_mutex_lock(&mi->rli->data_lock);
  mi->rli->abort_pos_wait++; /* for MASTER_POS_WAIT() to abort */
  /* Clear the errors, for a clean start */
  mi->rli->clear_error();
  mi->rli->clear_until_condition();

  sql_print_information("'CHANGE MASTER TO executed'. "
    "Previous state master_host='%s', master_port= %u, master_log_file='%s', "
    "master_log_pos= %ld, master_bind='%s'. "
    "New state master_host='%s', master_port= %u, master_log_file='%s', "
    "master_log_pos= %ld, master_bind='%s'.",
    saved_host, saved_port, saved_log_name, (ulong) saved_log_pos,
    saved_bind_addr, mi->host, mi->port, mi->get_master_log_name(),
    (ulong) mi->get_master_log_pos(), mi->bind_addr);

   is_slave = mi->host[0];
  /*
    If we don't write new coordinates to disk now, then old will remain in
    relay-log.info until START SLAVE is issued; but if mysqld is shutdown
    before START SLAVE, then old will remain in relay-log.info, and will be the
    in-memory value at restart (thus causing errors, as the old relay log does
    not exist anymore).

    Notice that the rli table is available exclusively as slave is not
    running.
  */
  DBUG_ASSERT(!mi->rli->slave_running);
  if ((ret= mi->rli->flush_info(true)))
    my_error(ER_RELAY_LOG_INIT, MYF(0), "Failed to flush relay info file.");
  mysql_cond_broadcast(&mi->data_cond);
  mysql_mutex_unlock(&mi->rli->data_lock);

err:
  unlock_slave_threads(mi);
  if (ret == FALSE)
  {
    if (!mts_remove_workers)
      my_ok(thd);
    else
      if (!Rpl_info_factory::reset_workers(mi->rli))
        my_ok(thd);
      else
        my_error(ER_MTS_RESET_WORKERS, MYF(0));
  }
  DBUG_RETURN(ret);
}

/* counter for the number of BI inconsistencies found */
ulong before_image_inconsistencies= 0;
/* table_name -> last BI inconsistency info */
std::unordered_map<std::string, before_image_mismatch>
  bi_inconsistencies;
/* mutex for counter and map */
std::mutex bi_inconsistency_lock;

void update_before_image_inconsistencies(const before_image_mismatch &mismatch)
{
  const std::lock_guard<std::mutex> lock(bi_inconsistency_lock);
  ++before_image_inconsistencies;
  bi_inconsistencies[mismatch.table]= mismatch;
}

ulong get_num_before_image_inconsistencies()
{
  const std::lock_guard<std::mutex> lock(bi_inconsistency_lock);
  return before_image_inconsistencies;
}

/**
  @} (end of group Replication)
*/
#endif /* HAVE_REPLICATION */
