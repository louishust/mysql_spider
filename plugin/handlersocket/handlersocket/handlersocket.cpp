
// vim:sw=2:ai

/*
 * Copyright (C) 2010-2011 DeNA Co.,Ltd.. All rights reserved.
 * Copyright (C) 2011 Kentoku SHIBA
 * See COPYRIGHT.txt for details.
 */

#include "config.hpp"
#include "hstcpsvr.hpp"
#include "string_util.hpp"
#include "mysql_incl.hpp"

#define DBG_LOG \
  if (dena::verbose_level >= 100) { \
    fprintf(stderr, "%s %p\n", __PRETTY_FUNCTION__, this); \
  }
#define DBG_DO(x) if (dena::verbose_level >= 100) { x; }

#define DBG_DIR(x)

using namespace dena;

static char *handlersocket_address = 0;
static char *handlersocket_port = 0;
static char *handlersocket_port_wr = 0;
static unsigned int handlersocket_epoll = 1;
unsigned int handlersocket_threads = 32;
unsigned int handlersocket_threads_wr = 1;
static unsigned int handlersocket_timeout = 30;
static unsigned int handlersocket_backlog = 32768;
static unsigned int handlersocket_sndbuf = 0;
static unsigned int handlersocket_rcvbuf = 0;
static unsigned int handlersocket_readsize = 0;
static unsigned int handlersocket_accept_balance = 0;
static unsigned int handlersocket_wrlock_timeout = 0;
static char *handlersocket_plain_secret = 0;
static char *handlersocket_plain_secret_wr = 0;
static unsigned int handlersocket_support_merge_table = 0;
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
static unsigned int handlersocket_direct_update_mode = 0;
#endif
static long long handlersocket_unlimited_boundary = 0;
static long long handlersocket_bulk_insert = 0;
static unsigned int handlersocket_bulk_insert_timeout = 0;
my_bool handlersocket_general_log;
my_bool handlersocket_slow_log;
ulonglong handlersocket_long_exec_time;
uint handlersocket_close_table_interval;
my_bool handlersocket_get_lock;
#ifdef HA_CAN_BULK_ACCESS
uint handlersocket_bulk_exec_size;
#endif

struct daemon_handlersocket_data {
  daemon_handlersocket_data()
    : hssvr_rd(NULL), hssvr_wr(NULL) {
  }
  virtual ~daemon_handlersocket_data() {
    if (hssvr_rd) delete hssvr_rd;
    if (hssvr_wr) delete hssvr_wr;
  }
  hstcpsvr_ptr hssvr_rd;
  hstcpsvr_ptr hssvr_wr;
};

static int
daemon_handlersocket_init(void *p)
{
  config conf;
  String err;
  st_plugin_int *plugin;
  if (conf.replace("use_epoll", handlersocket_epoll ? "1" : "0"))
    return -1;
  if (handlersocket_address) {
    if (conf.replace("host", handlersocket_address))
      return -1;
  }
  if (handlersocket_port) {
    if (conf.replace("port", handlersocket_port))
      return -1;
  }
  /*
   * unix domain socket
   * conf["host"] = "/";
   * conf["port"] = "/tmp/handlersocket";
   */
  if (handlersocket_threads > 0) {
    if (conf.replace("num_threads", handlersocket_threads))
      return -1;
  } else {
    if (conf.replace("num_threads", "1"))
      return -1;
  }
  if (
    conf.replace("timeout", handlersocket_timeout) ||
    conf.replace("listen_backlog", handlersocket_backlog) ||
    conf.replace("sndbuf", handlersocket_sndbuf) ||
    conf.replace("rcvbuf", handlersocket_rcvbuf) ||
    conf.replace("readsize", handlersocket_readsize) ||
    conf.replace("accept_balance", handlersocket_accept_balance) ||
    conf.replace("wrlock_timeout", handlersocket_wrlock_timeout) ||
    conf.replace("support_merge_table", handlersocket_support_merge_table) ||
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
    conf.replace("direct_update_mode", handlersocket_direct_update_mode) ||
#endif
    conf.replace("unlimited_boundary", handlersocket_unlimited_boundary) ||
    conf.replace("bulk_insert", handlersocket_bulk_insert) ||
    conf.replace("bulk_insert_timeout", handlersocket_bulk_insert_timeout)
  )
    return -1;
  daemon_handlersocket_data *ap = new daemon_handlersocket_data();
  if (!ap)
    return -1;
  if (handlersocket_port != 0 && handlersocket_port_wr != handlersocket_port) {
    if (conf.replace("port", handlersocket_port))
      goto error;
    if (handlersocket_plain_secret) {
      if (conf.replace("plain_secret", handlersocket_plain_secret))
        goto error;
    }
    ap->hssvr_rd = hstcpsvr_i::create(conf);
    ap->hssvr_rd->start_listen(&err);
  } else {
    DENA_VERBOSE(10, fprintf(stderr, "handlersocket: not listening "
      "for reads\n"));
  }
  if (handlersocket_port_wr != 0) {
    if (handlersocket_threads_wr > 0) {
      if (conf.replace("num_threads", handlersocket_threads_wr))
        goto error;
    }
    if (
      conf.replace("port", handlersocket_port_wr) ||
      conf.replace("for_write", "1") ||
      conf.replace("plain_secret", "")
    )
      goto error;
    if (handlersocket_plain_secret_wr) {
      if (conf.replace("plain_secret", handlersocket_plain_secret_wr))
        goto error;
    }
    ap->hssvr_wr = hstcpsvr_i::create(conf);
    ap->hssvr_wr->start_listen(&err);
  } else {
    DENA_VERBOSE(10, fprintf(stderr, "handlersocket: not listening "
      "for writes\n"));
  }
  plugin = static_cast<st_plugin_int *>(p);
  plugin->data = ap;
  DENA_VERBOSE(10, fprintf(stderr, "handlersocket: initialized\n"));
  return 0;

error:
  delete ap;
  return -1;
}

static int
daemon_handlersocket_deinit(void *p)
{
  DENA_VERBOSE(10, fprintf(stderr, "handlersocket: terminated\n"));
  st_plugin_int *const plugin = static_cast<st_plugin_int *>(p);
  daemon_handlersocket_data *ptr =
    static_cast<daemon_handlersocket_data *>(plugin->data);
  delete ptr;
  return 0;
}

static struct st_mysql_daemon daemon_handlersocket_plugin = {
  MYSQL_DAEMON_INTERFACE_VERSION
};

static MYSQL_SYSVAR_UINT(verbose, dena::verbose_level, 0,
  "0..10000", 0, 0, 10 /* default */, 0, 10000, 0);
static MYSQL_SYSVAR_UINT(epoll, handlersocket_epoll, PLUGIN_VAR_READONLY,
  "0..1", 0, 0, 1 /* default */, 0, 1, 0);
static MYSQL_SYSVAR_STR(address, handlersocket_address,
  PLUGIN_VAR_READONLY | PLUGIN_VAR_MEMALLOC, "", NULL, NULL, NULL);
static MYSQL_SYSVAR_STR(port, handlersocket_port,
  PLUGIN_VAR_READONLY | PLUGIN_VAR_MEMALLOC, "", NULL, NULL, NULL);
static MYSQL_SYSVAR_STR(port_wr, handlersocket_port_wr,
  PLUGIN_VAR_READONLY | PLUGIN_VAR_MEMALLOC, "", NULL, NULL, NULL);
static MYSQL_SYSVAR_UINT(threads, handlersocket_threads, PLUGIN_VAR_READONLY,
  "1..3000", 0, 0, 16 /* default */, 1, 3000, 0);
static MYSQL_SYSVAR_UINT(threads_wr, handlersocket_threads_wr,
  PLUGIN_VAR_READONLY, "1..3000", 0, 0, 1 /* default */, 1, 3000, 0);
static MYSQL_SYSVAR_UINT(timeout, handlersocket_timeout, PLUGIN_VAR_READONLY,
  "30..3600", 0, 0, 300 /* default */, 30, 3600, 0);
static MYSQL_SYSVAR_UINT(backlog, handlersocket_backlog, PLUGIN_VAR_READONLY,
  "5..1000000", 0, 0, 32768 /* default */, 5, 1000000, 0);
static MYSQL_SYSVAR_UINT(sndbuf, handlersocket_sndbuf, PLUGIN_VAR_READONLY,
  "0..16777216", 0, 0, 0 /* default */, 0, 16777216, 0);
static MYSQL_SYSVAR_UINT(rcvbuf, handlersocket_rcvbuf, PLUGIN_VAR_READONLY,
  "0..16777216", 0, 0, 0 /* default */, 0, 16777216, 0);
static MYSQL_SYSVAR_UINT(readsize, handlersocket_readsize, PLUGIN_VAR_READONLY,
  "0..16777216", 0, 0, 0 /* default */, 0, 16777216, 0);
static MYSQL_SYSVAR_UINT(accept_balance, handlersocket_accept_balance,
  PLUGIN_VAR_READONLY, "0..10000", 0, 0, 0 /* default */, 0, 10000, 0);
static MYSQL_SYSVAR_UINT(wrlock_timeout, handlersocket_wrlock_timeout,
  PLUGIN_VAR_READONLY, "0..3600", 0, 0, 12 /* default */, 0, 3600, 0);
static MYSQL_SYSVAR_STR(plain_secret, handlersocket_plain_secret,
  PLUGIN_VAR_READONLY | PLUGIN_VAR_MEMALLOC, "", NULL, NULL, NULL);
static MYSQL_SYSVAR_STR(plain_secret_wr, handlersocket_plain_secret_wr,
  PLUGIN_VAR_READONLY | PLUGIN_VAR_MEMALLOC, "", NULL, NULL, NULL);
static MYSQL_SYSVAR_UINT(support_merge_table,
  handlersocket_support_merge_table, 0,
  "0..1", 0, 0, 0 /* default */, 0, 1, 0);
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
static MYSQL_SYSVAR_UINT(direct_update_mode,
  handlersocket_direct_update_mode, 0,
  "0..2", 0, 0, 0 /* default */, 0, 2, 0);
#endif
static MYSQL_SYSVAR_LONGLONG(unlimited_boundary,
  handlersocket_unlimited_boundary, 0, "0..9223372036854775807LL", 0, 0,
  0 /* default */, 0, 9223372036854775807LL, 0);
static MYSQL_SYSVAR_LONGLONG(bulk_insert,
  handlersocket_bulk_insert, 0, "0..9223372036854775807LL", 0, 0,
  0 /* default */, 0, 9223372036854775807LL, 0);
static MYSQL_SYSVAR_UINT(bulk_insert_timeout,
  handlersocket_bulk_insert_timeout, 0, "0..16777216", 0, 0,
  0 /* default */, 0, 16777216, 0);
static MYSQL_SYSVAR_BOOL(general_log, handlersocket_general_log,
  PLUGIN_VAR_OPCMDARG, "on, off", NULL, NULL, FALSE);
static MYSQL_SYSVAR_BOOL(slow_log, handlersocket_slow_log,
  PLUGIN_VAR_OPCMDARG, "on, off", NULL, NULL, FALSE);
static MYSQL_SYSVAR_ULONGLONG(long_exec_time,
  handlersocket_long_exec_time, 0, "0..18446744073709551615ULL", 0, 0,
  1000000 /* default */, 0, 18446744073709551615ULL, 0);
static MYSQL_SYSVAR_UINT(close_table_interval,
  handlersocket_close_table_interval, 0, "0..16777216", 0, 0,
  30 /* default */, 0, 16777216, 0);
static MYSQL_SYSVAR_BOOL(get_lock, handlersocket_get_lock,
  PLUGIN_VAR_OPCMDARG, "on, off", NULL, NULL, TRUE);
#ifdef HA_CAN_BULK_ACCESS
static MYSQL_SYSVAR_UINT(bulk_exec_size,
  handlersocket_bulk_exec_size, 0, "0..16777216", 0, 0,
  5 /* default */, 0, 16777216, 0);
#endif


/* warning: type-punning to incomplete type might break strict-aliasing
 * rules */
static struct st_mysql_sys_var *daemon_handlersocket_system_variables[] = {
  MYSQL_SYSVAR(verbose),
  MYSQL_SYSVAR(address),
  MYSQL_SYSVAR(port),
  MYSQL_SYSVAR(port_wr),
  MYSQL_SYSVAR(epoll),
  MYSQL_SYSVAR(threads),
  MYSQL_SYSVAR(threads_wr),
  MYSQL_SYSVAR(timeout),
  MYSQL_SYSVAR(backlog),
  MYSQL_SYSVAR(sndbuf),
  MYSQL_SYSVAR(rcvbuf),
  MYSQL_SYSVAR(readsize),
  MYSQL_SYSVAR(accept_balance),
  MYSQL_SYSVAR(wrlock_timeout),
  MYSQL_SYSVAR(plain_secret),
  MYSQL_SYSVAR(plain_secret_wr),
  MYSQL_SYSVAR(support_merge_table),
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
  MYSQL_SYSVAR(direct_update_mode),
#endif
  MYSQL_SYSVAR(unlimited_boundary),
  MYSQL_SYSVAR(bulk_insert),
  MYSQL_SYSVAR(bulk_insert_timeout),
  MYSQL_SYSVAR(general_log),
  MYSQL_SYSVAR(slow_log),
  MYSQL_SYSVAR(long_exec_time),
  MYSQL_SYSVAR(close_table_interval),
  MYSQL_SYSVAR(get_lock),
#ifdef HA_CAN_BULK_ACCESS
  MYSQL_SYSVAR(bulk_exec_size),
#endif
  0
};

static SHOW_VAR hs_status_variables[] = {
  {"table_open", (char*) &open_tables_count, SHOW_LONGLONG},
  {"table_close", (char*) &close_tables_count, SHOW_LONGLONG},
  {"table_lock", (char*) &lock_tables_count, SHOW_LONGLONG},
  {"table_unlock", (char*) &unlock_tables_count, SHOW_LONGLONG},
  #if 0
  {"index_exec", (char*) &index_exec_count, SHOW_LONGLONG},
  #endif
  {NullS, NullS, SHOW_LONG}
};

static int show_hs_vars(THD *thd, SHOW_VAR *var, char *buff)
{
  var->type= SHOW_ARRAY;
  var->value= (char *) &hs_status_variables;
  return 0;
}

static SHOW_VAR daemon_handlersocket_status_variables[] = {
  {"Hs", (char*) show_hs_vars, SHOW_FUNC},
  {NullS, NullS, SHOW_LONG}
};


mysql_declare_plugin(handlersocket)
{
  MYSQL_DAEMON_PLUGIN,
  &daemon_handlersocket_plugin,
  "handlersocket",
  "higuchi dot akira at dena dot jp",
  "",
  PLUGIN_LICENSE_BSD,
  daemon_handlersocket_init,
  daemon_handlersocket_deinit,
  0x0102 /* 1.2 */,
  daemon_handlersocket_status_variables,
  daemon_handlersocket_system_variables,
  0
}
mysql_declare_plugin_end;

