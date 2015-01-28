
// vim:sw=2:ai

/*
 * Copyright (C) 2010-2011 DeNA Co.,Ltd.. All rights reserved.
 * Copyright (C) 2011-2013 Kentoku SHIBA
 * See COPYRIGHT.txt for details.
 */

#include "mysql_incl.hpp"
#include "database.hpp"
#include "string_util.hpp"
#include "escape.hpp"

#if MYSQL_VERSION_ID >= 50505
#include "log.h"
#include "key.h" // key_copy()
#include "sql_base.h"
#include "lock.h"
#include "transaction.h"
#endif

#define DBG_KEY(x)
#define DBG_SHUT(x)
#define DBG_LOCK(x)
#define DBG_THR(x)
#define DBG_CMP(x)
#define DBG_FLD(x)
#define DBG_FILTER(x)
#define DBG_REFCNT(x)
#define DBG_KEYLEN(x)
#define DBG_DELETED

/* status variables */
unsigned long long int open_tables_count;
unsigned long long int close_tables_count;
unsigned long long int lock_tables_count;
unsigned long long int unlock_tables_count;
unsigned long long int index_exec_count;

extern unsigned int handlersocket_threads;
extern unsigned int handlersocket_threads_wr;
extern my_bool handlersocket_general_log;
extern my_bool handlersocket_slow_log;
extern ulonglong handlersocket_long_exec_time;
extern uint handlersocket_close_table_interval;
extern my_bool handlersocket_get_lock;
#ifdef HA_CAN_BULK_ACCESS
extern uint handlersocket_bulk_exec_size;
#endif

namespace dena {

struct database : public database_i, private noncopyable {
  database(const config& c);
  virtual ~database();
  virtual dbcontext_ptr create_context(bool for_write) volatile;
  virtual void stop() volatile;
  virtual const config& get_conf() const volatile;
 public:
  int child_running;
 private:
  config conf;
};

struct tablevec_entry {
  TABLE_LIST *table_list;
  char *dbn;
  char *tbl;
  thr_lock_type lock_type;
  TABLE *table;
  size_t refcount;
  bool modified;
  bool reopen_failed;
  long long bulk_insert_count;
  bool bulk_inserting;
  tablevec_entry() : dbn(0), tbl(0), table(0), refcount(0), modified(false),
  reopen_failed(false), bulk_insert_count(0), bulk_inserting(FALSE) { }
};

struct table_vec_wrap {
  table_vec_wrap();
  virtual ~table_vec_wrap();
  virtual void clear();
  virtual void push_back(tablevec_entry &e);
  size_t size() {
    return table_vec_init ? table_vec.elements : 0; }
  bool empty() {
    return table_vec_init ? table_vec.elements ? FALSE : TRUE : TRUE; }
  tablevec_entry &operator [](size_t n) {
    return ((tablevec_entry *) (table_vec.buffer +
      table_vec.size_of_element * n))[0]; }
  bool table_vec_init;
  DYNAMIC_ARRAY table_vec; /* tablevec_entry */
};

struct table_map_wrap {
  table_map_wrap();
  virtual ~table_map_wrap();
  virtual void clear();
  virtual bool add(String &key, size_t val);
  virtual size_t find(String &key);
  bool table_map_init;
  HASH table_map;
};

struct expr_user_lock : private noncopyable {
  expr_user_lock(THD *thd, int timeout)
    : lck_key("handlersocket_wr", 16, &my_charset_latin1),
      lck_timeout(timeout),
      lck_func_get_lock(&lck_key, &lck_timeout),
      lck_func_release_lock(&lck_key)
  {
    lck_key.fix_fields(thd, 0);
    lck_timeout.fix_fields(thd, 0);
    lck_func_get_lock.fix_fields(thd, 0);
    lck_func_release_lock.fix_fields(thd, 0);
  }
  long long get_lock() {
    return lck_func_get_lock.val_int();
  }
  long long release_lock() {
    return lck_func_release_lock.val_int();
  }
 private:
  Item_string lck_key;
  Item_int lck_timeout;
  Item_func_get_lock lck_func_get_lock;
  Item_func_release_lock lck_func_release_lock;
};
typedef expr_user_lock *expr_user_lock_ptr;

struct dbcontext : public dbcontext_i, private noncopyable {
  dbcontext(volatile database *d, bool for_write);
  virtual ~dbcontext();
  virtual void init_thread(const void *stack_botton,
    volatile int& shutdown_flag);
  virtual void term_thread();
  virtual bool check_alive();
  virtual void end_bulk_insert_if();
  virtual void lock_tables_if();
  virtual void unlock_tables_if();
  virtual bool get_commit_error();
  virtual bool skip_unlock_tables();
  virtual void clear_error();
  virtual void close_tables_if(bool semi_close);
  virtual void semi_close_tables();
  virtual size_t table_refcount(size_t tbl_id);
  virtual void table_addref(size_t tbl_id);
  virtual void table_release(size_t tbl_id);
  virtual void cmd_open(dbcallback_i& cb, const cmd_open_args& args);
  virtual void cmd_exec(dbcallback_i& cb, const cmd_exec_args& args);
  virtual void set_statistics(size_t num_conns, size_t num_active);
  virtual void general_log(const char *query, uint query_length, size_t conn_pos);
  virtual void set_start_time();
  virtual void slow_log(const char *query, uint query_length);
  virtual int reopen_tables(dbcallback_i& cb,
    prep_stmt_list_wrap &prep_stmts);
  virtual int open_fields(prep_stmt& p);
#ifdef HA_CAN_BULK_ACCESS
  virtual void bulk_exec_prepare(hstcpsvr_worker_i *wk);
  virtual uint get_bulk_exec_size();
  virtual uint get_bulk_exec_phase();
  virtual bool add_bulk_exec_conn(dbcallback_i *cb);
  virtual void bulk_exec_finish();
  virtual void set_need_bulk_exec_finish();
#endif
 private:
  int set_thread_message(const char *fmt, ...)
    __attribute__((format (printf, 2, 3)));
  bool parse_fields(TABLE *const table, const char *str,
    prep_stmt::fields_type& flds);
  void cmd_insert_internal(dbcallback_i& cb, const prep_stmt& pst,
    const string_ref *fvals, size_t fvalslen, const cmd_exec_args& args);
  void cmd_sql_internal(dbcallback_i& cb, const prep_stmt& pst,
    const string_ref *fvals, size_t fvalslen);
  void cmd_find_internal(dbcallback_i& cb, const prep_stmt& pst,
    ha_rkey_function find_flag, const cmd_exec_args& args);
  size_t calc_filter_buf_size(TABLE *table, const prep_stmt& pst,
    const record_filter *filters);
  bool fill_filter_buf(TABLE *table, const prep_stmt& pst,
    const record_filter *filters, uchar *filter_buf, size_t len);
  int check_filter(dbcallback_i& cb, TABLE *table, const prep_stmt& pst,
    const record_filter *filters, const uchar *filter_buf);
  void resp_record(dbcallback_i& cb, TABLE *const table, const prep_stmt& pst);
  void dump_record(dbcallback_i& cb, TABLE *const table, const prep_stmt& pst);
  int modify_record(dbcallback_i& cb, TABLE *const table,
    const prep_stmt& pst, const cmd_exec_args& args, char mod_op,
    size_t& modified_count);
  bool open_one_table(tablevec_entry& e);
#ifdef HA_CAN_BULK_ACCESS
  void bulk_exec_exec();
#endif
 private:
  typedef table_vec_wrap table_vec_type; /* tablevec_entry */
  typedef String table_name_type; /* std::string, std::string */
  typedef table_map_wrap table_map_type; /* table_name_type, size_t */
 private:
  volatile database *const dbref;
  bool for_write_flag;
  THD *thd;
  MYSQL_LOCK *lock;
  bool lock_failed;
  expr_user_lock_ptr user_lock;
  int user_level_lock_timeout;
  bool user_level_lock_locked;
  bool commit_error;
  bool table_closed;
  bool field_closed;
  char info_message_buf[8192]; /* char */
  table_vec_type table_vec;
  table_map_type table_map;
  uint support_merge_table;
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
  uint direct_update_mode;
  KEY_MULTI_RANGE multi_range;
  bool direct_update_flag;
  uint32_info ret_fields_info;
#endif
  long long unlimited_boundary;
  SELECT_LEX select_lex;
  Item_int *offset_limit;
  Item_int *select_limit;
  long long bulk_insert;
  bool bulk_inserting;
  uint bulk_insert_timeout;
  bool skip_unlock_for_bulk_inserting;
  bool term_bulk_insert;
  uint bulk_insert_beat;
  uint close_table_beat;
#ifdef HA_CAN_BULK_ACCESS
  uint bulk_exec_phase;
  uint bulk_exec_size;
  cb_list_hld *bulk_cb_list;
  cb_list_hld *bulk_cb_current;
  uint bulk_cb_current_pos;
  uint bulk_cb_add_count;
  cb_list_hld *bulk_cb_executed;
  uint bulk_cb_executed_pos;
  hstcpsvr_worker_i *bulk_exec_wk;
  bool need_bulk_exec_finish;
#endif
};

uint32_list_wrap::uint32_list_wrap()
{
  init();
}

uint32_list_wrap::~uint32_list_wrap()
{
  deinit();
}

void
uint32_list_wrap::init()
{
  if (my_init_dynamic_array2(&uint32_list, sizeof(uint32), NULL, 16, 16))
    uint32_list_init = FALSE;
  else
    uint32_list_init = TRUE;
}

void
uint32_list_wrap::deinit()
{
  if (uint32_list_init)
    delete_dynamic(&uint32_list);
}

void
uint32_list_wrap::clear()
{
  if (uint32_list_init)
    uint32_list.elements = 0;
}

void
uint32_list_wrap::push_back(uint32 e)
{
  if (uint32_list_init)
    insert_dynamic(&uint32_list, (uchar*) &e);
  return;
}

prep_stmt::prep_stmt()
  : dbctx(0), table_id(static_cast<size_t>(-1)),
    idxnum(static_cast<size_t>(-1)), idx(0), retflds(0), filflds(0),
    refcnt(0), reopen_failed(false)
{
}
prep_stmt::prep_stmt(dbcontext_i *c, size_t tbl, size_t idx,
  const fields_type& rf, const fields_type& ff)
  : dbctx(c), table_id(tbl), idxnum(idx), idx(0), retflds(0), filflds(0),
    refcnt(0), reopen_failed(false)
{
  if (dbctx) {
    dbctx->table_addref(table_id);
  }
  ret_fields = rf;
  filter_fields = ff;
}
prep_stmt::~prep_stmt()
{
  if (dbctx) {
    dbctx->table_release(table_id);
  }
  if (idx) {
    (*refcnt)--;
    if (!(*refcnt))
      safeFree(idx);
  }
}

prep_stmt::prep_stmt(prep_stmt& x)
  : dbctx(x.dbctx), table_id(x.table_id), idxnum(x.idxnum),
  ret_fields(x.ret_fields), filter_fields(x.filter_fields),
  idx(x.idx), retflds(x.retflds), filflds(x.filflds), refcnt(x.refcnt),
  reopen_failed(x.reopen_failed)
{
  (*refcnt)++;
  if (dbctx) {
    dbctx->table_addref(table_id);
  }
}

prep_stmt&
prep_stmt::operator =(prep_stmt& x)
{
  if (this != &x) {
    if (dbctx) {
      dbctx->table_release(table_id);
    }
    if (idx) {
      (*refcnt)--;
      if (!(*refcnt))
        safeFree(idx);
    }

    dbctx = x.dbctx;
    table_id = x.table_id;
    idxnum = x.idxnum;
    ret_fields = x.ret_fields;
    filter_fields = x.filter_fields;
    idx = x.idx;
    retflds = x.retflds;
    filflds = x.filflds;
    refcnt = x.refcnt;
    reopen_failed = x.reopen_failed;
    (*refcnt)++;
    if (dbctx) {
      dbctx->table_addref(table_id);
    }
  }
  return *this;
}

struct table_map_param {
  String key;
  size_t val;
};

prep_stmt_list_wrap::prep_stmt_list_wrap()
{
  if (my_init_dynamic_array2(&prep_stmt_list, sizeof(prep_stmt),
    NULL, 16, 16))
    prep_stmt_list_init = FALSE;
  else
    prep_stmt_list_init = TRUE;
}

prep_stmt_list_wrap::~prep_stmt_list_wrap()
{
  if (prep_stmt_list_init) {
    resize(0);
    delete_dynamic(&prep_stmt_list);
  }
}

void
prep_stmt_list_wrap::clear()
{
  if (prep_stmt_list_init) {
    resize(0);
  }
}

void
prep_stmt_list_wrap::push_back(prep_stmt &e)
{
  if (prep_stmt_list_init)
    insert_dynamic(&prep_stmt_list, (uchar*) &e);
  return;
}

bool
prep_stmt_list_wrap::resize(size_t new_size)
{
  if (prep_stmt_list_init) {
    if (prep_stmt_list.max_element < new_size && allocate_dynamic(
      &prep_stmt_list, new_size)) return TRUE;
    for (size_t i = prep_stmt_list.elements; i < new_size; i++)
    {
      prep_stmt *ptr =
        (prep_stmt *) (prep_stmt_list.buffer +
          prep_stmt_list.size_of_element * i);
      ptr->dbctx = NULL;
      ptr->table_id = (size_t) -1;
      ptr->idxnum = (size_t) -1;
      ptr->idx = 0;
      ptr->retflds = 0;
      ptr->filflds = 0;
      ptr->refcnt = 0;
      ptr->reopen_failed = false;
      ptr->ret_fields.init();
      ptr->filter_fields.init();
    }
    for (size_t i = new_size; i < prep_stmt_list.elements; i++)
    {
      prep_stmt *ptr =
        (prep_stmt *) (prep_stmt_list.buffer +
          prep_stmt_list.size_of_element * i);
      if (ptr->dbctx)
        ptr->dbctx->table_release(ptr->table_id);
      if (ptr->idx) {
        (*ptr->refcnt)--;
        if (!(*ptr->refcnt))
          safeFree(ptr->idx);
      }
      ptr->ret_fields.deinit();
      ptr->filter_fields.deinit();
    }
    prep_stmt_list.elements = new_size;
    return FALSE;
  }
  return TRUE;
}

size_t
prep_stmt_list_wrap::size()
{
  return prep_stmt_list_init ? prep_stmt_list.elements : 0;
}

bool
prep_stmt_list_wrap::empty()
{
  return prep_stmt_list_init ? prep_stmt_list.elements ?
    FALSE : TRUE : TRUE;
}

prep_stmt &
prep_stmt_list_wrap::operator [](size_t n)
{
  return ((prep_stmt *) (prep_stmt_list.buffer +
    prep_stmt_list.size_of_element * n))[0];
}

static uchar *
table_map_get_key(
  table_map_param *param,
  size_t *length,
  my_bool not_used __attribute__ ((unused))
) {
  *length = param->key.length();
  return (uchar*) param->key.ptr();
}

table_map_wrap::table_map_wrap()
{
  if (my_hash_init(&table_map, &my_charset_bin, 32, 0, 0,
    (my_hash_get_key) table_map_get_key, 0, 0))
    table_map_init = FALSE;
  else
    table_map_init = TRUE;
}

table_map_wrap::~table_map_wrap()
{
  if (table_map_init)
  {
    clear();
    my_hash_free(&table_map);
  }
}

void
table_map_wrap::clear()
{
  if (table_map_init)
  {
    table_map_param *param;
    while ((param = (table_map_param *) my_hash_element(&table_map, 0)))
    {
      my_hash_delete(&table_map, (uchar*) param);
      delete param;
    }
  }
}

bool
table_map_wrap::add(String &key, size_t val)
{
  if (table_map_init)
  {
    table_map_param *param;
          if (!(param = new table_map_param()))
      return TRUE;
    param->val = val;
    if (
      param->key.copy(key) ||
      my_hash_insert(&table_map, (uchar*) param)
    ) {
      delete param;
      return TRUE;
    }
  } else
    return TRUE;
  return FALSE;
}

size_t
table_map_wrap::find(String &key)
{
  if (table_map_init)
  {
    table_map_param *param = (table_map_param *) my_hash_search(&table_map,
      (const uchar*) key.ptr(), key.length());
    if (param)
      return param->val;
  }
  return size_t(-1);
}

table_vec_wrap::table_vec_wrap()
{
  if (my_init_dynamic_array2(&table_vec, sizeof(tablevec_entry), NULL, 16, 16))
    table_vec_init = FALSE;
  else
    table_vec_init = TRUE;
}

table_vec_wrap::~table_vec_wrap()
{
  if (table_vec_init)
    delete_dynamic(&table_vec);
}

void
table_vec_wrap::clear()
{
  if (table_vec_init) {
    for (uint i = 0; i < table_vec.elements; i++)
    {
      tablevec_entry *e =
        (tablevec_entry *) (table_vec.buffer + table_vec.size_of_element * i);
      if (e->dbn) {
        safeFree(e->dbn);
      }
    }
    table_vec.elements = 0;
  }
}

void
table_vec_wrap::push_back(tablevec_entry &e)
{
  if (table_vec_init)
    insert_dynamic(&table_vec, (uchar*) &e);
  return;
}

database::database(const config& c)
  : child_running(1)
{
  conf = c;
}

database::~database()
{
}

dbcontext_ptr
database::create_context(bool for_write) volatile
{
  return new dbcontext(this, for_write);
}

void
database::stop() volatile
{
  child_running = false;
}

const config&
database::get_conf() const volatile
{
  return const_cast<const config&>(conf);
}

database_ptr
database_i::create(const config& conf)
{
  return new database(conf);
}

dbcontext::dbcontext(volatile database *d, bool for_write)
  : dbref(d), for_write_flag(for_write), thd(0), lock(0), lock_failed(false),
    user_level_lock_timeout(0), user_level_lock_locked(false),
    commit_error(false), table_closed(false), field_closed(false),
    offset_limit(0), select_limit(0), bulk_inserting(FALSE),
    skip_unlock_for_bulk_inserting(FALSE), term_bulk_insert(FALSE),
    bulk_insert_beat(0), close_table_beat(0), need_bulk_exec_finish(FALSE)
{
  user_level_lock_timeout = d->get_conf().get_int("wrlock_timeout", 12);
  support_merge_table = d->get_conf().get_int("support_merge_table", 0);
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
  direct_update_mode = d->get_conf().get_int("direct_update_mode", 0);
#endif
  unlimited_boundary = d->get_conf().get_int("unlimited_boundary", 0);
  bulk_insert = d->get_conf().get_int("bulk_insert", 0);
  bulk_insert_timeout = d->get_conf().get_int("bulk_insert_timeout", 0);
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
  memset(&multi_range, 0, sizeof(KEY_MULTI_RANGE));
  direct_update_flag = FALSE;
#endif
  memset(&select_lex, 0, sizeof(SELECT_LEX));
  select_lex.explicit_limit = FALSE;
  user_lock = NULL;
  info_message_buf[0] = '\0';
#ifdef HA_CAN_BULK_ACCESS
  bulk_cb_list = NULL;
#endif
}

dbcontext::~dbcontext()
{
  if (user_lock)
    delete user_lock;
#ifdef HA_CAN_BULK_ACCESS
  while (bulk_cb_list)
  {
    cb_list_hld *next = bulk_cb_list->next;
    safeFree(bulk_cb_list);
    bulk_cb_list = next;
  }
#endif
}

namespace {

int
wait_server_to_start(THD *thd, volatile int& shutdown_flag)
{
  int r = 0;
  DBG_SHUT(fprintf(stderr, "HNDSOCK wsts\n"));
#if MYSQL_VERSION_ID >= 50505
  mysql_mutex_lock(&LOCK_server_started);
#else
  pthread_mutex_lock(&LOCK_server_started);
#endif
  while (!mysqld_server_started) {
    timespec abstime = { };
    set_timespec(abstime, 1);
#if MYSQL_VERSION_ID >= 50505
    mysql_cond_timedwait(&COND_server_started, &LOCK_server_started,
      &abstime);
    mysql_mutex_unlock(&LOCK_server_started);
    mysql_mutex_lock(&thd->mysys_var->mutex);
#else
    pthread_cond_timedwait(&COND_server_started, &LOCK_server_started,
      &abstime);
    pthread_mutex_unlock(&LOCK_server_started);
    pthread_mutex_lock(&thd->mysys_var->mutex);
#endif
    THD::killed_state st = thd->killed;
#if MYSQL_VERSION_ID >= 50505
    mysql_mutex_unlock(&thd->mysys_var->mutex);
#else
    pthread_mutex_unlock(&thd->mysys_var->mutex);
#endif
    DBG_SHUT(fprintf(stderr, "HNDSOCK wsts kst %d\n", (int)st));
#if MYSQL_VERSION_ID >= 50505
    mysql_mutex_lock(&LOCK_server_started);
#else
    pthread_mutex_lock(&LOCK_server_started);
#endif
    if (st != THD::NOT_KILLED) {
      DBG_SHUT(fprintf(stderr, "HNDSOCK wsts kst %d break\n", (int)st));
      r = -1;
      break;
    }
    if (shutdown_flag) {
      DBG_SHUT(fprintf(stderr, "HNDSOCK wsts kst shut break\n"));
      r = -1;
      break;
    }
  }
#if MYSQL_VERSION_ID >= 50505
  mysql_mutex_unlock(&LOCK_server_started);
#else
  pthread_mutex_unlock(&LOCK_server_started);
#endif
  DBG_SHUT(fprintf(stderr, "HNDSOCK wsts done\n"));
  return r;
}

}; // namespace

#define DENA_THR_OFFSETOF(fld) ((char *)(&thd->fld) - (char *)thd)

void
dbcontext::init_thread(const void *stack_bottom, volatile int& shutdown_flag)
{
  DBG_THR(fprintf(stderr, "HNDSOCK init thread\n"));
  {
    my_thread_init();
    thd = new THD;

#if MYSQL_VERSION_ID >= 50505
    mysql_mutex_lock(&LOCK_thread_count);
#else
    pthread_mutex_lock(&LOCK_thread_count);
#endif
    thd->thread_id = thread_id++;
#if MYSQL_VERSION_ID >= 50505
    mysql_mutex_unlock(&LOCK_thread_count);
#else
    pthread_mutex_unlock(&LOCK_thread_count);
#endif
    thd->thread_stack = (char *)stack_bottom;
    DBG_THR(fprintf(stderr,
      "thread_stack = %p sizeof(THD)=%zu sizeof(mtx)=%zu "
      "O: %zu %zu %zu %zu %zu %zu %zu\n",
      thd->thread_stack, sizeof(THD), sizeof(LOCK_thread_count),
      DENA_THR_OFFSETOF(mdl_context),
      DENA_THR_OFFSETOF(net),
      DENA_THR_OFFSETOF(LOCK_thd_data),
      DENA_THR_OFFSETOF(mysys_var),
      DENA_THR_OFFSETOF(stmt_arena),
      DENA_THR_OFFSETOF(limit_found_rows),
      DENA_THR_OFFSETOF(locked_tables_list)));
    thd->store_globals();
    thd->system_thread = static_cast<enum_thread_type>(1<<30UL);
    const NET v = { 0 };
    thd->net = v;
    if (for_write_flag) {
      #if MYSQL_VERSION_ID >= 50505
      thd->variables.option_bits |= OPTION_BIN_LOG;
      #else
      thd->options |= OPTION_BIN_LOG;
      #endif
      safeFree(thd->db);
      thd->db = 0;
      thd->db = my_strdup("handlersocket", MYF(0));
    }
/*
    my_pthread_setspecific_ptr(THR_THD, thd);
*/
    DBG_THR(fprintf(stderr, "HNDSOCK x0 %p\n", thd));
  }
  {
#if MYSQL_VERSION_ID >= 50505
    mysql_mutex_lock(&LOCK_thread_count);
#else
    pthread_mutex_lock(&LOCK_thread_count);
#endif
/*
    thd->thread_id = thread_id++;
*/
    threads.append(thd);
    ++thread_count;
#if MYSQL_VERSION_ID >= 50505
    mysql_mutex_unlock(&LOCK_thread_count);
#else
    pthread_mutex_unlock(&LOCK_thread_count);
#endif
  }

  DBG_THR(fprintf(stderr, "HNDSOCK init thread wsts\n"));
  wait_server_to_start(thd, shutdown_flag);
  DBG_THR(fprintf(stderr, "HNDSOCK init thread done\n"));

  thd_proc_info(thd, info_message_buf);
  set_thread_message("hs:listening");
  DBG_THR(fprintf(stderr, "HNDSOCK x1 %p\n", thd));

  lex_start(thd);

  offset_limit = new Item_int((ulonglong) 0);
  select_limit = new Item_int((ulonglong) 0);
  select_lex.offset_limit = offset_limit;
  select_lex.select_limit = select_limit;
  select_lex.explicit_limit = TRUE;

  if (user_lock)
    delete user_lock;
  user_lock = new expr_user_lock(thd, user_level_lock_timeout);
}

int
dbcontext::set_thread_message(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(
    info_message_buf, sizeof(info_message_buf),
    fmt, ap);
  va_end(ap);
  return n;
}

void
dbcontext::term_thread()
{
  DBG_THR(fprintf(stderr, "HNDSOCK thread end %p\n", thd));
  close_tables_if(false);
  if (offset_limit)
    delete offset_limit;
  if (select_limit)
    delete select_limit;
  select_lex.explicit_limit = FALSE;
  {
#if MYSQL_VERSION_ID >= 50505
    mysql_mutex_lock(&LOCK_thread_count);
#else
    pthread_mutex_lock(&LOCK_thread_count);
#endif
    delete thd;
    thd = 0;
    --thread_count;
#if MYSQL_VERSION_ID >= 50505
    mysql_mutex_unlock(&LOCK_thread_count);
#else
    pthread_mutex_unlock(&LOCK_thread_count);
#endif
    my_pthread_setspecific_ptr(THR_THD, 0);
    my_thread_end();
  }
}

bool
dbcontext::check_alive()
{
#if MYSQL_VERSION_ID >= 50505
  mysql_mutex_lock(&thd->mysys_var->mutex);
#else
  pthread_mutex_lock(&thd->mysys_var->mutex);
#endif
  THD::killed_state st = thd->killed;
#if MYSQL_VERSION_ID >= 50505
  mysql_mutex_unlock(&thd->mysys_var->mutex);
#else
  pthread_mutex_unlock(&thd->mysys_var->mutex);
#endif
  DBG_SHUT(fprintf(stderr, "chk HNDSOCK kst %p %p %d %zu\n", thd, &thd->killed,
    (int)st, sizeof(*thd)));
  if (st != THD::NOT_KILLED) {
    DBG_SHUT(fprintf(stderr, "chk HNDSOCK kst %d break\n", (int)st));
    return false;
  }
  return true;
}

void
dbcontext::end_bulk_insert_if()
{
  for (size_t i = 0; i < table_vec.size(); ++i) {
    tablevec_entry *tbe = &table_vec[i];
    TABLE *table = tbe->table;
    handler *hnd = table->file;
    if (tbe->bulk_inserting) {
      hnd->ha_release_auto_increment();
      hnd->ha_end_bulk_insert();
      tbe->bulk_inserting = FALSE;
    }
  }
  bulk_inserting = FALSE;
  term_bulk_insert = FALSE;
}

void
dbcontext::lock_tables_if()
{
  DBUG_ENTER("dbcontext::lock_tables_if");
  if (lock_failed) {
    DBUG_VOID_RETURN;
  }
  if (bulk_inserting)
    unlock_tables_if();
  if (for_write_flag && handlersocket_get_lock && !user_level_lock_locked) {
    if (user_lock->get_lock()) {
      user_level_lock_locked = true;
    } else {
      lock_failed = true;
      DBUG_VOID_RETURN;
    }
  }
  if (lock == 0) {
    thd->set_query_id(next_query_id());
    const size_t num_max = table_vec.size();
    #if MYSQL_VERSION_ID >= 50505
    size_t tblcount = 0;
    for (size_t i = 0; i < num_max; ++i) {
      if (table_vec[i].refcount > 0) {
        for (TABLE_LIST *table = table_vec[i].table->pos_in_table_list; table;
          table = table->next_global) {
          tblcount++;
        }
      }
    }
    #else
    size_t tblcount = num_max;
    #endif
    TABLE **const tables = DENA_ALLOCA_ALLOCATE(TABLE *, tblcount + 1);
    size_t num_open = 0;
    for (size_t i = 0; i < num_max; ++i) {
      if (table_vec[i].refcount > 0) {
        #if MYSQL_VERSION_ID >= 50505
        for (TABLE_LIST *table = table_vec[i].table->pos_in_table_list; table;
          table = table->next_global) {
          tables[num_open] = table->table;
          num_open++;
        }
        #else
        tables[num_open] = table_vec[i].table;
        num_open++;
        #endif
      }
      table_vec[i].modified = false;
    }
    #if MYSQL_VERSION_ID >= 50505
    lock = thd->lock = mysql_lock_tables(thd, &tables[0], num_open, 0);
    #else
    bool need_reopen= false;
    lock = thd->lock = mysql_lock_tables(thd, &tables[0], num_open,
      MYSQL_LOCK_NOTIFY_IF_NEED_REOPEN, &need_reopen);
    #endif
    statistic_increment(lock_tables_count, &LOCK_status);
    thd_proc_info(thd, info_message_buf);
    DENA_VERBOSE(100, fprintf(stderr, "HNDSOCK lock tables %p %p %zu %zu\n",
      thd, lock, num_max, num_open));
    if (lock == 0) {
      lock_failed = true;
      DENA_VERBOSE(10, fprintf(stderr, "HNDSOCK failed to lock tables %p\n",
        thd));
    }
    if (for_write_flag) {
      #if MYSQL_VERSION_ID >= 50505
      thd->set_current_stmt_binlog_format_row();
      #else
      thd->current_stmt_binlog_row_based = 1;
      #endif
    }
    DENA_ALLOCA_FREE(tables);
  }
  DBG_LOCK(fprintf(stderr, "HNDSOCK tblnum=%d\n", (int)tblnum));
  DBUG_VOID_RETURN;
}

void
dbcontext::unlock_tables_if()
{
  DBUG_ENTER("dbcontext::unlock_tables_if");
  if (bulk_inserting)
    end_bulk_insert_if();
  if (lock != 0) {
    DENA_VERBOSE(100, fprintf(stderr, "HNDSOCK unlock tables %p %p\n",
      thd, thd->lock));
    if (for_write_flag) {
      for (size_t i = 0; i < table_vec.size(); ++i) {
        if (table_vec[i].modified) {
          query_cache_invalidate3(thd, table_vec[i].table, 1);
        }
      }
    }
    {
      bool suc = true;
      #if MYSQL_VERSION_ID >= 50505
      suc = (trans_commit_stmt(thd) == 0);
      #else
      suc = (ha_autocommit_or_rollback(thd, 0) == 0);
      #endif
      if (!suc) {
        commit_error = true;
        DENA_VERBOSE(10, fprintf(stderr,
          "HNDSOCK unlock tables: commit failed\n"));
      }
    }
    mysql_unlock_tables(thd, lock);
    lock = thd->lock = 0;
    statistic_increment(unlock_tables_count, &LOCK_status);
  }
  if (user_level_lock_locked) {
    if (user_lock->release_lock()) {
      user_level_lock_locked = false;
    }
  }
  skip_unlock_for_bulk_inserting = FALSE;
  DBUG_VOID_RETURN;
}

bool
dbcontext::get_commit_error()
{
  return commit_error;
}

bool
dbcontext::skip_unlock_tables()
{
  return (skip_unlock_for_bulk_inserting &&
    bulk_insert_beat++ < bulk_insert_timeout);
}

void
dbcontext::clear_error()
{
  lock_failed = false;
  commit_error = false;
}

void
dbcontext::close_tables_if(bool semi_close)
{
  DBUG_ENTER("dbcontext::close_tables_if");
  unlock_tables_if();
  DENA_VERBOSE(100, fprintf(stderr, "HNDSOCK close tables\n"));
#ifdef HS_HAS_SQLCOM
  thd->lex->sql_command = SQLCOM_HS_CLOSE;
  status_var_increment(thd->status_var.com_stat[SQLCOM_HS_CLOSE]);
#endif
  if (!table_closed)
  {
    close_thread_tables(thd);
    #if MYSQL_VERSION_ID >= 50505
    thd->mdl_context.release_transactional_locks();
    #endif
    statistic_increment(close_tables_count, &LOCK_status);
  }
  if (semi_close)
    table_closed = true;
  else {
    if (!table_vec.empty()) {
      table_vec.clear();
      table_map.clear();
    }
  }
  DBUG_VOID_RETURN;
}

void
dbcontext::semi_close_tables()
{
  DBUG_ENTER("dbcontext::semi_close_tables");
  if (handlersocket_close_table_interval)
  {
    close_table_beat++;
    if (close_table_beat >= handlersocket_close_table_interval)
    {
      close_table_beat = 0;
      close_tables_if(true);
      field_closed = true;
    } else if (!table_closed) {
      field_closed = false;
    }
  }
  DBUG_VOID_RETURN;
}

size_t
dbcontext::table_refcount(size_t tbl_id)
{
  return table_vec[tbl_id].refcount;
}

void
dbcontext::table_addref(size_t tbl_id)
{
  table_vec[tbl_id].refcount += 1;
  DBG_REFCNT(fprintf(stderr, "%p %zu %zu addref\n", this, tbl_id,
    table_vec[tbl_id].refcount));
}

void
dbcontext::table_release(size_t tbl_id)
{
  table_vec[tbl_id].refcount -= 1;
  DBG_REFCNT(fprintf(stderr, "%p %zu %zu release\n", this, tbl_id,
    table_vec[tbl_id].refcount));
}

void
dbcontext::resp_record(dbcallback_i& cb, TABLE *const table,
  const prep_stmt& pst)
{
  char rwpstr_buf[64];
  String rwpstr(rwpstr_buf, sizeof(rwpstr_buf), &my_charset_bin);
  prep_stmt::fields_type& rf = (prep_stmt::fields_type&) pst.get_ret_fields();
  const size_t n = rf.size();
  for (size_t i = 0; i < n; ++i) {
    uint32 fn = rf[i];
    Field *const fld = table->field[fn];
    DBG_FLD(fprintf(stderr, "fld=%p %zu\n", fld, fn));
    if (fld->is_null()) {
      /* null */
      cb.dbcb_resp_entry(0, 0);
    } else {
      fld->val_str(&rwpstr, &rwpstr);
      const size_t len = rwpstr.length();
      if (len != 0) {
        /* non-empty */
        cb.dbcb_resp_entry(rwpstr.ptr(), rwpstr.length());
      } else {
        /* empty */
        static const char empty_str[] = "";
        cb.dbcb_resp_entry(empty_str, 0);
      }
    }
  }
}

void
dbcontext::dump_record(dbcallback_i& cb, TABLE *const table,
  const prep_stmt& pst)
{
  char rwpstr_buf[64];
  String rwpstr(rwpstr_buf, sizeof(rwpstr_buf), &my_charset_bin);
  prep_stmt::fields_type& rf = (prep_stmt::fields_type&) pst.get_ret_fields();
  const size_t n = rf.size();
  for (size_t i = 0; i < n; ++i) {
    uint32 fn = rf[i];
    Field *const fld = table->field[fn];
    if (fld->is_null()) {
      /* null */
      fprintf(stderr, "NULL");
    } else {
      fld->val_str(&rwpstr, &rwpstr);
      String s(rwpstr.ptr(), rwpstr.length(), &my_charset_bin);
      fprintf(stderr, "[%s]", s.c_ptr_safe());
    }
  }
  fprintf(stderr, "\n");
}

int
dbcontext::modify_record(dbcallback_i& cb, TABLE *const table,
  const prep_stmt& pst, const cmd_exec_args& args, char mod_op,
  size_t& modified_count)
{
  if (mod_op == 'U') {
    /* update */
    handler *const hnd = table->file;
    uchar *const buf = table->record[0];
    store_record(table, record[1]);
    prep_stmt::fields_type& rf =
      (prep_stmt::fields_type&) pst.get_ret_fields();
    const size_t n = rf.size();
    for (size_t i = 0; i < n; ++i) {
      const string_ref& nv = args.uvals[i];
      uint32 fn = rf[i];
      Field *const fld = table->field[fn];
      if (nv.begin() == 0) {
        fld->set_null();
      } else {
        fld->set_notnull();
        fld->store(nv.begin(), nv.size(), &my_charset_bin);
      }
    }
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
    if (direct_update_flag) {
      const int r = hnd->ha_direct_update_row_binlog(table->record[1], buf);
      if (r) {
        return r;
      }
    } else {
#endif
      table_vec[pst.get_table_id()].modified = true;
      const int r = hnd->ha_update_row(table->record[1], buf);
      if (r != 0 && r != HA_ERR_RECORD_IS_THE_SAME) {
        return r;
      }
      ++modified_count; /* TODO: HA_ERR_RECORD_IS_THE_SAME? */
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
    }
#endif
  } else if (mod_op == 'D') {
    /* delete */
    handler *const hnd = table->file;
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
    if (direct_update_flag) {
      const int r = hnd->ha_direct_delete_row_binlog(table->record[0]);
      if (r) {
        return r;
      }
    } else {
#endif
      table_vec[pst.get_table_id()].modified = true;
      const int r = hnd->ha_delete_row(table->record[0]);
      if (r != 0) {
        return r;
      }
      ++modified_count;
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
    }
#endif
  } else if (mod_op == '+' || mod_op == '-') {
    /* increment/decrement */
    handler *const hnd = table->file;
    uchar *const buf = table->record[0];
    store_record(table, record[1]);
    prep_stmt::fields_type& rf =
      (prep_stmt::fields_type&) pst.get_ret_fields();
    const size_t n = rf.size();
    size_t i = 0;
    for (i = 0; i < n; ++i) {
      const string_ref& nv = args.uvals[i];
      uint32 fn = rf[i];
      Field *const fld = table->field[fn];
      if (fld->is_null() || nv.begin() == 0) {
        continue;
      }
      const long long llv = atoll_nocheck(nv.begin(), nv.end());
      if (llv == 0) {
        continue;
      }
      const long long pval = fld->val_int();
      long long nval = 0;
      if (mod_op == '+') {
        /* increment */
        nval = pval + llv;
      } else {
        /* decrement */
        nval = pval - llv;
        if ((pval < 0 && nval > 0) || (pval > 0 && nval < 0)) {
          break; /* don't modify */
        }
      }
      fld->store(nval, false);
    }
    if (i == n) {
      /* modify */
      table_vec[pst.get_table_id()].modified = true;
      const int r = hnd->ha_update_row(table->record[1], buf);
      if (r != 0 && r != HA_ERR_RECORD_IS_THE_SAME) {
        return r;
      }
      ++modified_count;
    }
  }
  return 0;
}

void
dbcontext::cmd_insert_internal(dbcallback_i& cb, const prep_stmt& pst,
  const string_ref *fvals, size_t fvalslen, const cmd_exec_args& args)
{
  if (!for_write_flag) {
    return cb.dbcb_resp_short(2, "readonly");
  }
#ifdef HS_HAS_SQLCOM
  thd->lex->sql_command = SQLCOM_HS_INSERT;
  status_var_increment(thd->status_var.com_stat[SQLCOM_HS_INSERT]);
#endif
#ifdef HA_CAN_BULK_ACCESS
  uint bulk_req_phase = cb.get_bulk_req_phase();
  bool locked = (lock != 0);
#endif
  if (
#ifdef HA_CAN_BULK_ACCESS
    bulk_req_phase ||
#endif
    !bulk_inserting
  ) {
    lock_tables_if();
  }
  if (lock == 0) {
    return cb.dbcb_resp_short(1, "lock_tables");
  }
  if (pst.get_table_id() >= table_vec.size()) {
    return cb.dbcb_resp_short(2, "tblnum");
  }
  tablevec_entry *tbe = &table_vec[pst.get_table_id()];
  TABLE *const table = tbe->table;
  handler *const hnd = table->file;
  uchar *const buf = table->record[0];
#ifdef HA_CAN_BULK_ACCESS
  req_hld *bulk_req = NULL;
  if (bulk_req_phase == 1) {
    bulk_req = cb.get_bulk_req();
    bulk_req->hnd = hnd;
    if (hnd->info_push(INFO_KIND_BULK_ACCESS_BEGIN, &bulk_req->req_pos)) {
      return cb.dbcb_resp_short(2, "out of mem");
    }
  } else if (locked /* && bulk_req_phase == 0 */ && !bulk_inserting)
  {
    if (hnd->additional_lock(thd, (for_write_flag ? TL_WRITE : TL_READ))) {
      return cb.dbcb_resp_short(2, "additional_lock");
    }
  }
#endif
  bitmap_set_all(table->write_set);
/*
  empty_record(table);
*/
/*  memset(buf, 0, table->s->null_bytes); *//* clear null flags */
  restore_record(table, s->default_values);
  prep_stmt::fields_type& rf =
    (prep_stmt::fields_type&) pst.get_ret_fields();
  Field **fld = table->field;
  size_t i = 0;
  uint32 nn_fn;
  if (fvalslen > rf.size()) {
    if (dena::verbose_level >= 20) {
      time_t cur_time = (time_t) time((time_t*) 0);
      struct tm lt;
      struct tm *l_time = localtime_r(&cur_time, &lt);
      fprintf(stderr,
        "%04d%02d%02d %02d:%02d:%02d [ERROR] handlersocket: [%s]fvalslen[%zu]>rfsize[%zu]\n",
        l_time->tm_year + 1900, l_time->tm_mon + 1, l_time->tm_mday,
        l_time->tm_hour, l_time->tm_min, l_time->tm_sec,
        __func__, fvalslen, rf.size());
      fwrite(args.start, 1, args.finish - args.start, stderr);
      fprintf(stderr,"\n");
    }
    return cb.dbcb_resp_short(2, "fvalslen>rfsize");
  }
  if (
#ifdef HA_CAN_BULK_ACCESS
    !bulk_req_phase &&
#endif
    bulk_insert &&
    !tbe->bulk_inserting
  ) {
    hnd->ha_start_bulk_insert(0);
    tbe->bulk_inserting = TRUE;
    tbe->bulk_insert_count = 0;
    bulk_inserting = TRUE;
  }
  if (table->found_next_number_field)
    nn_fn = table->found_next_number_field->field_index;
  else
    nn_fn = MAX_FIELDS;
  for (; i < fvalslen; ++i) {
    uint32 fn = rf[i];
    if (fvals[i].begin() == 0) {
      fld[fn]->set_null();
    } else {
      fld[fn]->set_notnull();
      fld[fn]->store(fvals[i].begin(), fvals[i].size(), &my_charset_bin);
      if (fn == nn_fn)
        table->auto_increment_field_not_null = TRUE;
    }
  }
  table->next_number_field = table->found_next_number_field;
  int r;
#ifdef HA_CAN_BULK_ACCESS
  if (bulk_req_phase == 1) {
    r = hnd->ha_pre_write_row(buf);
  } else {
#endif
    r = hnd->ha_write_row(buf);
#ifdef HA_CAN_BULK_ACCESS
  }
#endif
  table->next_number_field = 0;
  table->auto_increment_field_not_null = FALSE;
  ulonglong insert_id = 0;
#ifdef HA_CAN_BULK_ACCESS
  if (bulk_req_phase == 1 &&
    (!r || r == HA_ERR_WRONG_COMMAND || r == ER_NOT_SUPPORTED_YET)) {
    /* pause */
    if (r) {
      set_need_bulk_exec_finish();
    }
    return;
  }
  if (bulk_req_phase != 1) {
#endif
    table_vec[pst.get_table_id()].modified = true;
    if (tbe->bulk_inserting)
    {
      insert_id = 0;
      tbe->bulk_insert_count++;
      bulk_insert_beat = 0;
      if (tbe->bulk_insert_count >= bulk_insert)
      {
        skip_unlock_for_bulk_inserting = FALSE;
        term_bulk_insert = TRUE;
      } else if (!r && !term_bulk_insert)
        skip_unlock_for_bulk_inserting = TRUE;
    } else {
      insert_id = hnd->insert_id_for_cur_row;
      hnd->ha_release_auto_increment();
    }
#ifdef HA_CAN_BULK_ACCESS
  }
#endif
  if (r) {
    if (thd->is_error())
    {
      if (dena::verbose_level >= 20) {
        time_t cur_time = (time_t) time((time_t*) 0);
        struct tm lt;
        struct tm *l_time = localtime_r(&cur_time, &lt);
        fprintf(stderr,
          "%04d%02d%02d %02d:%02d:%02d [ERROR] handlersocket: [%s][%d]%s\n",
          l_time->tm_year + 1900, l_time->tm_mon + 1, l_time->tm_mday,
          l_time->tm_hour, l_time->tm_min, l_time->tm_sec,
          __func__, r,
          #if MYSQL_VERSION_ID >= 50505
          thd->stmt_da->message()
          #else
          thd->main_da.message()
          #endif
        );
        fwrite(args.start, 1, args.finish - args.start, stderr);
        fprintf(stderr,"\n");
      }
      #if MYSQL_VERSION_ID >= 50505
      return cb.dbcb_resp_short_with_num(1, r, thd->stmt_da->message());
      #else
      return cb.dbcb_resp_short_with_num(1, r, thd->main_da.message());
      #endif
    } else {
      if (dena::verbose_level >= 20) {
        time_t cur_time = (time_t) time((time_t*) 0);
        struct tm lt;
        struct tm *l_time = localtime_r(&cur_time, &lt);
        fprintf(stderr,
          "%04d%02d%02d %02d:%02d:%02d [ERROR] handlersocket: [%s]%d\n",
          l_time->tm_year + 1900, l_time->tm_mon + 1, l_time->tm_mday,
          l_time->tm_hour, l_time->tm_min, l_time->tm_sec,
          __func__, r
        );
        fwrite(args.start, 1, args.finish - args.start, stderr);
        fprintf(stderr,"\n");
      }
      return cb.dbcb_resp_short_num(1, r);
    }
  } else if (table->found_next_number_field) {
    return cb.dbcb_resp_short_num64(0, insert_id);
  }
  return cb.dbcb_resp_short(0, "");
}

void
dbcontext::cmd_sql_internal(dbcallback_i& cb, const prep_stmt& pst,
  const string_ref *fvals, size_t fvalslen)
{
  if (fvalslen < 1) {
    return cb.dbcb_resp_short(2, "syntax");
  }
  return cb.dbcb_resp_short(2, "notimpl");
}

static size_t
prepare_keybuf(const cmd_exec_args& args, uchar *key_buf, TABLE *table,
  KEY& kinfo, size_t invalues_index)
{
  size_t kplen_sum = 0;
  DBG_KEY(fprintf(stderr, "SLOW\n"));
  for (size_t i = 0; i < args.kvalslen; ++i) {
    const KEY_PART_INFO & kpt = kinfo.key_part[i];
    string_ref kval = args.kvals[i];
    if (args.invalues_keypart >= 0 &&
      static_cast<size_t>(args.invalues_keypart) == i) {
      kval = args.invalues[invalues_index];
    }
    if (kval.begin() == 0) {
      kpt.field->set_null();
    } else {
      kpt.field->set_notnull();
    }
    kpt.field->store(kval.begin(), kval.size(), &my_charset_bin);
    kplen_sum += kpt.store_length;
    DBG_KEYLEN(fprintf(stderr, "l=%u sl=%zu\n", kpt.length,
      kpt.store_length));
  }
  key_copy(key_buf, table->record[0], &kinfo, kplen_sum);
  DBG_KEYLEN(fprintf(stderr, "sum=%zu flen=%u\n", kplen_sum,
    kinfo.key_length));
  return kplen_sum;
}

void
dbcontext::cmd_find_internal(dbcallback_i& cb, const prep_stmt& pst,
  ha_rkey_function find_flag, const cmd_exec_args& args)
{
  const bool debug_out = (verbose_level >= 100);
  bool need_resp_record = true;
#ifdef HA_CAN_BULK_ACCESS
  uint bulk_req_phase = cb.get_bulk_req_phase();
  bool locked = (lock != 0);
  req_hld *bulk_req = NULL;
  if (bulk_req_phase == 1) {
    bulk_req = cb.get_bulk_req();
  }
#endif
  char mod_op = 0;
  const string_ref& mod_op_str = args.mod_op;
  if (mod_op_str.size() != 0) {
    if (!for_write_flag) {
      return cb.dbcb_resp_short(2, "readonly");
    }
    mod_op = mod_op_str.begin()[0];
    need_resp_record = mod_op_str.size() > 1 && mod_op_str.begin()[1] == '?';
    switch (mod_op) {
    case 'U': /* update */
#ifdef HS_HAS_SQLCOM
      thd->lex->sql_command = SQLCOM_HS_UPDATE;
      status_var_increment(thd->status_var.com_stat[SQLCOM_HS_UPDATE]);
      break;
#endif
    case 'D': /* delete */
#ifdef HS_HAS_SQLCOM
      thd->lex->sql_command = SQLCOM_HS_DELETE;
      status_var_increment(thd->status_var.com_stat[SQLCOM_HS_DELETE]);
      break;
#endif
    case '+': /* increment */
    case '-': /* decrement */
#ifdef HS_HAS_SQLCOM
      thd->lex->sql_command = SQLCOM_HS_UPDATE;
      status_var_increment(thd->status_var.com_stat[SQLCOM_HS_UPDATE]);
#endif
      break;
    default:
      if (debug_out) {
        fprintf(stderr, "unknown modop: %c\n", mod_op);
      }
      return cb.dbcb_resp_short(2, "modop");
    }
  }
#ifdef HS_HAS_SQLCOM
  else {
    thd->lex->sql_command = SQLCOM_HS_READ;
    status_var_increment(thd->status_var.com_stat[SQLCOM_HS_READ]);
  }
#endif
  lock_tables_if();
  if (lock == 0) {
    return cb.dbcb_resp_short(1, "lock_tables");
  }
  if (pst.get_table_id() >= table_vec.size()) {
    return cb.dbcb_resp_short(2, "tblnum");
  }
  TABLE *const table = table_vec[pst.get_table_id()].table;
  /* keys */
  if (pst.get_idxnum() >= table->s->keys) {
    return cb.dbcb_resp_short(2, "idxnum");
  }
  KEY& kinfo = table->key_info[pst.get_idxnum()];
  if (args.kvalslen > kinfo.key_parts) {
    return cb.dbcb_resp_short(2, "kpnum");
  }
  uchar *const key_buf = DENA_ALLOCA_ALLOCATE(uchar, kinfo.key_length);
  size_t invalues_idx = 0;
  size_t kplen_sum = prepare_keybuf(args, key_buf, table, kinfo, invalues_idx);

  /* filters */
  uchar *filter_buf = 0;
  if (args.filters != 0) {
    const size_t filter_buf_len = calc_filter_buf_size(table, pst,
      args.filters);
    filter_buf = DENA_ALLOCA_ALLOCATE(uchar, filter_buf_len);
    if (!fill_filter_buf(table, pst, args.filters, filter_buf,
      filter_buf_len)) {
      return cb.dbcb_resp_short(2, "filterblob");
    }
  }
  /* handler */
/*
  table->read_set = &table->s->all_set;
*/
  bitmap_set_all(table->read_set);
  handler *const hnd = table->file;
#ifdef HA_CAN_BULK_ACCESS
  if (bulk_req_phase == 1) {
    bulk_req->hnd = hnd;
    if (hnd->info_push(INFO_KIND_BULK_ACCESS_BEGIN, &bulk_req->req_pos)) {
      return cb.dbcb_resp_short(2, "out of mem");
    }
  } else if (locked /* && bulk_req_phase == 0 */)
  {
    if (hnd->additional_lock(thd, (for_write_flag ? TL_WRITE : TL_READ))) {
      return cb.dbcb_resp_short(2, "additional_lock");
    }
  }
#endif
  if (!for_write_flag) {
    hnd->init_table_handle_for_HANDLER();
  }
#ifdef HA_CAN_BULK_ACCESS
  if (bulk_req_phase == 2) {
    hnd->ha_pre_index_or_rnd_end();
  } else {
#endif
    hnd->ha_index_or_rnd_end();
#ifdef HA_CAN_BULK_ACCESS
  }
#endif
  const uint32 limit = args.limit ? args.limit : 1;
  uint32 skip = args.skip;
  table->pos_in_table_list->select_lex = &select_lex;
  if (unlimited_boundary && !skip && unlimited_boundary <= limit)
  {
    /* unlimited */
    select_lex.explicit_limit = FALSE;
  } else {
    select_limit->value = (longlong) limit;
    offset_limit->value = (longlong) skip;
    select_lex.explicit_limit = TRUE;
  }
#ifdef HA_CAN_BULK_ACCESS
  if (bulk_req_phase == 1) {
    int r = hnd->ha_pre_index_init(pst.get_idxnum(), 1);
    if (r == HA_ERR_WRONG_COMMAND || r == ER_NOT_SUPPORTED_YET) {
      set_need_bulk_exec_finish();
      DENA_ALLOCA_FREE(filter_buf);
      DENA_ALLOCA_FREE(key_buf);
      return;
    }
  } else {
#endif
    hnd->ha_index_init(pst.get_idxnum(), 1);
#ifdef HA_CAN_BULK_ACCESS
  }
#endif
#ifdef HA_CAN_BULK_ACCESS
  if (bulk_req_phase != 1) {
#endif
    if (need_resp_record) {
      cb.dbcb_resp_begin(((prep_stmt::fields_type&)
        pst.get_ret_fields()).size());
    }
#ifdef HA_CAN_BULK_ACCESS
  }
#endif
  size_t modified_count = 0;
  int r = 0;
  bool is_first = true;
  bool in_loop = false;
  for (uint32_t cnt = 0; cnt < limit + skip;) {
    if (is_first) {
      is_first = false;
      const key_part_map kpm = (1U << args.kvalslen) - 1;
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
      if (direct_update_mode && mod_op != 0) {
        multi_range.start_key.key = key_buf;
        multi_range.start_key.length = calculate_key_len(table,
          pst.get_idxnum(), key_buf, kpm);
        multi_range.start_key.keypart_map = kpm;
        multi_range.start_key.flag = find_flag;
        if (mod_op == 'U' ||
          (
            direct_update_mode == 2 &&
            (mod_op == '+' || mod_op == '-')
          )
        ) {
          prep_stmt::fields_type& rf =
            (prep_stmt::fields_type&) pst.get_ret_fields();
          const size_t n = rf.size();
          if (mod_op == 'U') {
            for (size_t i = 0; i < n; ++i) {
              const string_ref& nv = args.uvals[i];
              uint32 fn = rf[i];
              Field *const fld = table->field[fn];
              if (nv.begin() == 0) {
                fld->set_null();
              } else {
                fld->set_notnull();
                fld->store(nv.begin(), nv.size(), &my_charset_bin);
              }
            }
          } else {
            for (size_t i = 0; i < n; ++i) {
              const string_ref& nv = args.uvals[i];
              if ((r = hnd->info_push(INFO_KIND_HS_APPEND_STRING_REF, (void *) &nv)))
                break;
            }
            if (r) {
              hnd->info_push(INFO_KIND_HS_CLEAR_STRING_REF, NULL);
              break;
            }
          }
#ifdef HA_CAN_BULK_ACCESS
          if (bulk_req_phase == 1) {
            r = hnd->ha_pre_direct_update_rows_init(direct_update_mode, &multi_range,
              1, TRUE, table->record[0]);
          } else {
#endif
            r = hnd->ha_direct_update_rows_init(direct_update_mode, &multi_range,
              1, TRUE, table->record[0]);
#ifdef HA_CAN_BULK_ACCESS
          }
#endif
          if (r) {
            if (mod_op != 'U')
              hnd->info_push(INFO_KIND_HS_CLEAR_STRING_REF, NULL);
            if (r != HA_ERR_WRONG_COMMAND) {
              break;
            } else {
              direct_update_flag = FALSE;
            }
          } else {
            direct_update_flag = TRUE;
            if (direct_update_mode == 2) {
              uint update_rows = 0;
              table_vec[pst.get_table_id()].modified = true;
              if (mod_op == '+')
                hnd->info_push(INFO_KIND_HS_INCREMENT_BEGIN, NULL);
              else if (mod_op == '-')
                hnd->info_push(INFO_KIND_HS_DECREMENT_BEGIN, NULL);
#ifdef HA_CAN_BULK_ACCESS
              if (bulk_req_phase == 1) {
                r = hnd->ha_pre_direct_update_rows(&multi_range, 1, TRUE,
                  table->record[0], &update_rows);
              } else {
#endif
                r = hnd->ha_direct_update_rows(&multi_range, 1, TRUE,
                  table->record[0], &update_rows);
#ifdef HA_CAN_BULK_ACCESS
              }
#endif
              if (mod_op == '+')
              {
                hnd->info_push(INFO_KIND_HS_INCREMENT_END, NULL);
                hnd->info_push(INFO_KIND_HS_CLEAR_STRING_REF, NULL);
              } else if (mod_op == '-')
              {
                hnd->info_push(INFO_KIND_HS_DECREMENT_END, NULL);
                hnd->info_push(INFO_KIND_HS_CLEAR_STRING_REF, NULL);
              }
              if (!r) {
#ifdef HA_CAN_BULK_ACCESS
                if (bulk_req_phase == 1) {
                  /* pause */
                  if (hnd->ha_pre_index_or_rnd_end() == ER_NOT_SUPPORTED_YET)
                    set_need_bulk_exec_finish();
                  DENA_ALLOCA_FREE(filter_buf);
                  DENA_ALLOCA_FREE(key_buf);
                  return;
                } else {
#endif
                  modified_count = update_rows;
#ifdef HA_CAN_BULK_ACCESS
                }
#endif
              }
              break;
            }
          }
        } else if (mod_op == 'D') {
#ifdef HA_CAN_BULK_ACCESS
          if (bulk_req_phase == 1) {
            r = hnd->ha_pre_direct_delete_rows_init(direct_update_mode,
              &multi_range, 1, TRUE);
          } else {
#endif
            r = hnd->ha_direct_delete_rows_init(direct_update_mode,
              &multi_range, 1, TRUE);
#ifdef HA_CAN_BULK_ACCESS
          }
#endif
          if (r) {
            if (r != HA_ERR_WRONG_COMMAND) {
              break;
            } else {
              direct_update_flag = FALSE;
            }
          } else {
            direct_update_flag = TRUE;
            if (direct_update_mode == 2) {
              uint delete_rows = 0;
              table_vec[pst.get_table_id()].modified = true;
#ifdef HA_CAN_BULK_ACCESS
              if (bulk_req_phase == 1) {
                r = hnd->ha_pre_direct_delete_rows(&multi_range, 1, TRUE,
                  &delete_rows);
              } else {
#endif
                r = hnd->ha_direct_delete_rows(&multi_range, 1, TRUE,
                  &delete_rows);
#ifdef HA_CAN_BULK_ACCESS
              }
#endif
              if (!r) {
#ifdef HA_CAN_BULK_ACCESS
                if (bulk_req_phase == 1) {
                  /* pause */
                  if (hnd->ha_pre_index_or_rnd_end() == ER_NOT_SUPPORTED_YET)
                    set_need_bulk_exec_finish();
                  DENA_ALLOCA_FREE(filter_buf);
                  DENA_ALLOCA_FREE(key_buf);
                  return;
                } else {
#endif
                  modified_count = delete_rows;
#ifdef HA_CAN_BULK_ACCESS
                }
#endif
              }
              break;
            }
          }
        }
      }
#endif
#ifdef HA_CAN_BULK_ACCESS
      if (bulk_req_phase == 1) {
        r = hnd->pre_index_read_map(key_buf, kpm, find_flag, FALSE);
        if (!r) {
          /* pause */
          if (hnd->ha_pre_index_or_rnd_end() == ER_NOT_SUPPORTED_YET)
            set_need_bulk_exec_finish();
          DENA_ALLOCA_FREE(filter_buf);
          DENA_ALLOCA_FREE(key_buf);
          return;
        }
      } else {
#endif
        r = hnd->index_read_map(table->record[0], key_buf, kpm, find_flag);
#ifdef HA_CAN_BULK_ACCESS
      }
#endif
      if (args.invalues_keypart >= 0) {
        in_loop = true;
      }
      /* pause point 4 */
    } else if (!in_loop && args.invalues_keypart >= 0) {
      if (++invalues_idx >= args.invalueslen) {
        break;
      }
      kplen_sum = prepare_keybuf(args, key_buf, table, kinfo, invalues_idx);
      const key_part_map kpm = (1U << args.kvalslen) - 1;
      r = hnd->index_read_map(table->record[0], key_buf, kpm, find_flag);
      in_loop = true;
    } else {
      switch (find_flag) {
      case HA_READ_BEFORE_KEY:
      case HA_READ_KEY_OR_PREV:
        r = hnd->index_prev(table->record[0]);
        break;
      case HA_READ_AFTER_KEY:
      case HA_READ_KEY_OR_NEXT:
        r = hnd->index_next(table->record[0]);
        break;
      case HA_READ_KEY_EXACT:
        r = hnd->index_next_same(table->record[0], key_buf, kplen_sum);
        break;
      default:
        r = HA_ERR_END_OF_FILE; /* to finish the loop */
        break;
      }
    }
    if (debug_out) {
      fprintf(stderr, "r=%d\n", r);
      if (r == 0 || r == HA_ERR_RECORD_DELETED) { 
        dump_record(cb, table, pst);
      }
    }
    int filter_res = 0;
    if (r != 0) {
      /* no-count */
    } else {
      thd->examined_row_count++;
      if (args.filters != 0 && (filter_res = check_filter(cb, table, 
        pst, args.filters, filter_buf)) != 0) {
        if (filter_res < 0) {
          break;
        }
      } else if (skip > 0) {
        --skip;
      } else {
        /* hit */
        if (need_resp_record) {
          thd->sent_row_count++;
          resp_record(cb, table, pst);
        }
        if (mod_op != 0) {
          r = modify_record(cb, table, pst, args, mod_op, modified_count);
        }
        ++cnt;
      }
      if (args.invalues_keypart >= 0 && r != 0 ) {
        in_loop = false;
        continue;
      }
    }
    if (r != 0 && r != HA_ERR_RECORD_DELETED) {
      break;
    }
  }
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
  if (mod_op && direct_update_flag && direct_update_mode == 1) {
    if (mod_op == 'U') {
      if (!r) {
        uint update_rows = 0;
        table_vec[pst.get_table_id()].modified = true;
        if (mod_op == '+')
          hnd->info_push(INFO_KIND_HS_INCREMENT_BEGIN, NULL);
        else if (mod_op == '-')
          hnd->info_push(INFO_KIND_HS_DECREMENT_BEGIN, NULL);
        r = hnd->ha_direct_update_rows(&multi_range, 1, TRUE,
          table->record[0], &update_rows);
        if (mod_op == '+')
        {
          hnd->info_push(INFO_KIND_HS_INCREMENT_END, NULL);
          hnd->info_push(INFO_KIND_HS_CLEAR_STRING_REF, NULL);
        } else if (mod_op == '-')
        {
          hnd->info_push(INFO_KIND_HS_DECREMENT_END, NULL);
          hnd->info_push(INFO_KIND_HS_CLEAR_STRING_REF, NULL);
        }
        if (!r)
          modified_count = update_rows;
      }
    } else if (mod_op == 'D') {
      if (!r) {
        uint delete_rows = 0;
        table_vec[pst.get_table_id()].modified = true;
        r = hnd->ha_direct_delete_rows(&multi_range, 1, TRUE,
          &delete_rows);
        if (!r)
          modified_count = delete_rows;
      }
    } else if (mod_op == '+' || mod_op == '-') {
      if (!r) {
      }
    }
    direct_update_flag = FALSE;
  }
#endif
#ifdef HA_CAN_BULK_ACCESS
  if (bulk_req_phase == 1) {
    if (hnd->ha_pre_index_or_rnd_end() == ER_NOT_SUPPORTED_YET)
      set_need_bulk_exec_finish();
  } else {
#endif
    hnd->ha_index_or_rnd_end();
#ifdef HA_CAN_BULK_ACCESS
  }
#endif
  if (r != 0 && r != HA_ERR_RECORD_DELETED && r != HA_ERR_KEY_NOT_FOUND &&
    r != HA_ERR_END_OF_FILE) {
    /* failed */
    if (need_resp_record) {
      /* revert dbcb_resp_begin() and dbcb_resp_entry() */
      cb.dbcb_resp_cancel();
    }
    if (thd->is_error())
    {
      if (dena::verbose_level >= 20) {
        time_t cur_time = (time_t) time((time_t*) 0);
        struct tm lt;
        struct tm *l_time = localtime_r(&cur_time, &lt);
        fprintf(stderr,
          "%04d%02d%02d %02d:%02d:%02d [ERROR] handlersocket: [%s][%d]%s\n",
          l_time->tm_year + 1900, l_time->tm_mon + 1, l_time->tm_mday,
          l_time->tm_hour, l_time->tm_min, l_time->tm_sec,
          __func__, r,
          #if MYSQL_VERSION_ID >= 50505
          thd->stmt_da->message()
          #else
          thd->main_da.message()
          #endif
        );
        fwrite(args.start, 1, args.finish - args.start, stderr);
        fprintf(stderr,"\n");
      }
      #if MYSQL_VERSION_ID >= 50505
      cb.dbcb_resp_short_with_num(1, r, thd->stmt_da->message());
      #else
      cb.dbcb_resp_short_with_num(1, r, thd->main_da.message());
      #endif
    } else {
      if (dena::verbose_level >= 20) {
        time_t cur_time = (time_t) time((time_t*) 0);
        struct tm lt;
        struct tm *l_time = localtime_r(&cur_time, &lt);
        fprintf(stderr,
          "%04d%02d%02d %02d:%02d:%02d [ERROR] handlersocket: [%s]%d\n",
          l_time->tm_year + 1900, l_time->tm_mon + 1, l_time->tm_mday,
          l_time->tm_hour, l_time->tm_min, l_time->tm_sec,
          __func__, r
        );
        fwrite(args.start, 1, args.finish - args.start, stderr);
        fprintf(stderr,"\n");
      }
      cb.dbcb_resp_short_num(1, r);
    }
  } else {
    /* succeeded */
    if (need_resp_record) {
      cb.dbcb_resp_end();
    } else {
      thd->sent_row_count = modified_count;
      cb.dbcb_resp_short_num(0, modified_count);
    }
  }
  DENA_ALLOCA_FREE(filter_buf);
  DENA_ALLOCA_FREE(key_buf);
}

size_t
dbcontext::calc_filter_buf_size(TABLE *table, const prep_stmt& pst,
  const record_filter *filters)
{
  size_t filter_buf_len = 0;
  for (const record_filter *f = filters; f->op.begin() != 0; ++f) {
    if (f->val.begin() == 0) {
      continue;
    }
    const uint32 fn =
      ((prep_stmt::fields_type&) pst.get_filter_fields())[f->ff_offset];
    filter_buf_len += table->field[fn]->pack_length();
  }
  ++filter_buf_len;
    /* Field_medium::cmp() calls uint3korr(), which may read 4 bytes.
       Allocate 1 more byte for safety. */
  return filter_buf_len;
}

bool
dbcontext::fill_filter_buf(TABLE *table, const prep_stmt& pst,
  const record_filter *filters, uchar *filter_buf, size_t len)
{
  memset(filter_buf, 0, len);
  size_t pos = 0;
  for (const record_filter *f = filters; f->op.begin() != 0; ++f) {
    if (f->val.begin() == 0) {
      continue;
    }
    const uint32 fn =
      ((prep_stmt::fields_type&) pst.get_filter_fields())[f->ff_offset];
    Field *const fld = table->field[fn];
    if ((fld->flags & BLOB_FLAG) != 0) {
      return false;
    }
    fld->store(f->val.begin(), f->val.size(), &my_charset_bin);
    const size_t packlen = fld->pack_length();
    memcpy(filter_buf + pos, fld->ptr, packlen);
    pos += packlen;
  }
  return true;
}

int
dbcontext::check_filter(dbcallback_i& cb, TABLE *table, const prep_stmt& pst,
  const record_filter *filters, const uchar *filter_buf)
{
  DBG_FILTER(fprintf(stderr, "check_filter\n"));
  size_t pos = 0;
  for (const record_filter *f = filters; f->op.begin() != 0; ++f) {
    const string_ref& op = f->op;
    const string_ref& val = f->val;
    const uint32 fn =
      ((prep_stmt::fields_type&) pst.get_filter_fields())[f->ff_offset];
    Field *const fld = table->field[fn];
    const size_t packlen = fld->pack_length();
    const uchar *const bval = filter_buf + pos;
    int cv = 0;
    if (fld->is_null()) {
      cv = (val.begin() == 0) ? 0 : -1;
    } else {
      cv = (val.begin() == 0) ? 1 : fld->cmp(bval);
    }
    DBG_FILTER(fprintf(stderr, "check_filter cv=%d\n", cv));
    bool cond = true;
    if (op.size() == 1) {
      switch (op.begin()[0]) {
      case '>':
        DBG_FILTER(fprintf(stderr, "check_filter op: >\n"));
        cond = (cv > 0);
        break;
      case '<':
        DBG_FILTER(fprintf(stderr, "check_filter op: <\n"));
        cond = (cv < 0);
        break;
      case '=':
        DBG_FILTER(fprintf(stderr, "check_filter op: =\n"));
        cond = (cv == 0);
        break;
      default:
        DBG_FILTER(fprintf(stderr, "check_filter op: unknown\n"));
        cond = false; /* FIXME: error */
        break;
      }
    } else if (op.size() == 2 && op.begin()[1] == '=') {
      switch (op.begin()[0]) {
      case '>':
        DBG_FILTER(fprintf(stderr, "check_filter op: >=\n"));
        cond = (cv >= 0);
        break;
      case '<':
        DBG_FILTER(fprintf(stderr, "check_filter op: <=\n"));
        cond = (cv <= 0);
        break;
      case '!':
        DBG_FILTER(fprintf(stderr, "check_filter op: !=\n"));
        cond = (cv != 0);
        break;
      default:
        DBG_FILTER(fprintf(stderr, "check_filter op: unknown\n"));
        cond = false; /* FIXME: error */
        break;
      }
    }
    DBG_FILTER(fprintf(stderr, "check_filter cond: %d\n", (int)cond));
    if (!cond) {
      return (f->filter_type == record_filter_type_skip) ? 1 : -1;
    }
    if (val.begin() != 0) {
      pos += packlen;
    }
  }
  return 0;
}

bool
dbcontext::open_one_table(tablevec_entry& e)
{
  TABLE_LIST *tables = e.table_list;
  const char *dbn = e.dbn;
  const char *tbl = e.tbl;
  TABLE *table = 0;
  bool refresh = true;
  const thr_lock_type lock_type = e.lock_type;
#ifdef HS_HAS_SQLCOM
  thd->lex->sql_command = SQLCOM_HS_OPEN;
  status_var_increment(thd->status_var.com_stat[SQLCOM_HS_OPEN]);
#endif
  #if MYSQL_VERSION_ID >= 50505
  tables->init_one_table(dbn, strlen(dbn), tbl, strlen(tbl), tbl,
    lock_type);
  tables->mdl_request.init(MDL_key::TABLE, dbn, tbl,
    for_write_flag ? MDL_SHARED_WRITE : MDL_SHARED_READ, MDL_TRANSACTION);
  uint flags = (
    MYSQL_OPEN_IGNORE_GLOBAL_READ_LOCK |
    MYSQL_LOCK_IGNORE_GLOBAL_READ_ONLY |
    MYSQL_OPEN_IGNORE_FLUSH |
    MYSQL_LOCK_IGNORE_TIMEOUT |
    MYSQL_OPEN_GET_NEW_TABLE |
    MYSQL_OPEN_SKIP_TEMPORARY
  );
  if (!support_merge_table)
  {
    Open_table_context ot_act(thd, flags);
    if (!open_table(thd, tables, thd->mem_root, &ot_act))
    {
      table = tables->table;
    }
  } else {
    uint counter;
    if (!open_tables(thd, &tables, &counter, flags))
    {
      table = tables->table;
    }
  }
  #else
  tables->init_one_table(dbn, tbl, lock_type);
  if (!support_merge_table)
  {
    table = open_table(thd, tables, thd->mem_root, &refresh,
      OPEN_VIEW_NO_PARSE);
  } else {
    uint counter;
    if (!open_tables(thd, &tables, &counter, OPEN_VIEW_NO_PARSE))
    {
      table = tables->table;
    }
  }
  #endif
  if (table == 0) {
    DENA_VERBOSE(20, fprintf(stderr,
      "HNDSOCK failed to open %p [%s] [%s] [%d]\n",
      thd, dbn, tbl, static_cast<int>(refresh)));
    return true;
  }
  statistic_increment(open_tables_count, &LOCK_status);
  table->reginfo.lock_type = lock_type;
  table->default_column_bitmaps();
  bitmap_set_all(table->read_set);
  bitmap_set_all(table->write_set);
  e.table = table;
  return false;
}

void
dbcontext::cmd_open(dbcallback_i& cb, const cmd_open_args& arg)
{
  size_t pst_id = arg.pst_id;
  const char *dbn = arg.dbn;
  const char *tbl = arg.tbl;
  const char *idx = arg.idx;
  const char *retflds = arg.retflds;
  const char *filflds = arg.filflds;
/*
  unlock_tables_if();
*/
  char buf[MAX_FIELD_WIDTH];
  String k(buf, MAX_FIELD_WIDTH, &my_charset_bin);
  k.length(0);
  uint32 dbn_len = strlen(dbn), tbl_len = strlen(tbl);
  if (k.reserve(dbn_len + tbl_len + 1))
  {
    return cb.dbcb_resp_short(2, "out of mem");
  }
  k.q_append(dbn, dbn_len + 1);
  k.q_append(tbl, tbl_len);
  const size_t iter = table_map.find(k);
  uint32 tblnum = 0;
  thd->clear_error();
  if (iter != size_t(-1) && !table_vec[iter].reopen_failed) {
    tblnum = iter;
    DBG_CMP(fprintf(stderr, "HNDSOCK dbn=%s tbl=%s tblnum=%d\n", dbn, tbl,
      (int)tblnum));
    #if MYSQL_VERSION_ID >= 50505
    if (!table_refcount(tblnum))
    {
#ifdef HA_CAN_BULK_ACCESS
      /* finish bulk execution and shift normal execution */
      bulk_exec_finish();
      bulk_exec_size = 0;
      bulk_exec_phase = 0;
#endif
      unlock_tables_if();
    }
    #endif
  } else {
#ifdef HA_CAN_BULK_ACCESS
    /* finish bulk execution and shift normal execution */
    bulk_exec_finish();
    bulk_exec_size = 0;
    bulk_exec_phase = 0;
#endif
    unlock_tables_if();
    if (iter != size_t(-1)) {
      if (open_one_table(table_vec[iter]))
      {
        return cb.dbcb_resp_short(1, "open_table");
      }
      table_vec[iter].reopen_failed = false;
    } else {
      tablevec_entry e;
      size_t dbn_len = strlen(dbn) + 1, tbl_len = strlen(tbl) + 1;
      if (lower_case_table_names) {
        my_casedn_str(files_charset_info, (char *) dbn);
        my_casedn_str(files_charset_info, (char *) tbl);
      }
      if (!(e.dbn = (char *)
        my_multi_malloc(MYF(MY_WME),
          &e.dbn, dbn_len,
          &e.tbl, tbl_len,
          &e.table_list, sizeof(TABLE_LIST),
          NullS))
      ) {
        return cb.dbcb_resp_short(2, "out of mem");
      }
      memcpy(e.dbn, dbn, dbn_len);
      memcpy(e.tbl, tbl, tbl_len);
      e.lock_type = for_write_flag ? TL_WRITE : TL_READ;

      if (open_one_table(e))
      {
        return cb.dbcb_resp_short(1, "open_table");
      }

      tblnum = table_vec.size();
      table_vec.push_back(e);
      if (table_map.add(k, tblnum))
      {
        return cb.dbcb_resp_short(2, "out of mem");
      }
    }
  }
  prep_stmt p;
  size_t idx_len = strlen(idx) + 1, retflds_len = strlen(retflds) + 1,
    filflds_len = strlen(filflds) + 1;
  p.dbctx = this;
  if (!(p.idx = (char *)
    my_multi_malloc(MYF(MY_WME),
      &p.idx, idx_len,
      &p.retflds, retflds_len,
      &p.filflds, filflds_len,
      &p.refcnt, sizeof(size_t),
      NullS))
  ) {
    return cb.dbcb_resp_short(2, "out of mem");
  }
  memcpy(p.idx, idx, idx_len);
  memcpy(p.retflds, retflds, retflds_len);
  memcpy(p.filflds, filflds, filflds_len);
  p.table_id = tblnum;
  *p.refcnt = 1;
  table_addref(tblnum);

  int err = open_fields(p);
  if (err)
  {
    switch (err)
    {
      case 1:
        return cb.dbcb_resp_short(2, "idxnum");
      case 2:
        return cb.dbcb_resp_short(2, "fld");
      default:
        break;
    }
  }

  cb.dbcb_set_prep_stmt(pst_id, p);
  return cb.dbcb_resp_short(0, "");
}

int
dbcontext::reopen_tables(dbcallback_i& cb, prep_stmt_list_wrap &prep_stmts)
{
  DBUG_ENTER("dbcontext::reopen_tables");
  if (table_closed)
  {
    size_t i;
    for (i = 0; i < table_vec.size(); i++)
    {
      if (open_one_table(table_vec[i]))
      {
        thd->clear_error();
        table_vec[i].reopen_failed = true;
/*
        cb.dbcb_resp_short(1, "open_table");
        table_closed = false;
        close_tables_if(true);
        return 3;
*/
      } else
        table_vec[i].reopen_failed = false;
    }
    table_closed = false;
  }
  if (field_closed)
  {
    size_t i;
    int err;
    for (i = 0; i < prep_stmts.size(); i++)
    {
      if (prep_stmts[i].idx && !prep_stmts[i].reopen_failed &&
        !table_vec[prep_stmts[i].table_id].reopen_failed)
      {
        if ((err = open_fields(prep_stmts[i])))
        {
          prep_stmts[i].reopen_failed = true;
/*
          switch (err)
          {
            case 1:
              cb.dbcb_resp_short(2, "idxnum");
              break;
            case 2:
              cb.dbcb_resp_short(2, "fld");
              break;
            default:
              break;
          }
          table_closed = false;
          close_tables_if(true);
          return err;
*/
        }
      }
    }
  }
  DBUG_RETURN(0);
}

int
dbcontext::open_fields(prep_stmt& p)
{
  uint32 tblnum = p.table_id;
  size_t idxnum = static_cast<size_t>(-1);
  DBUG_ENTER("dbcontext::open_fields");
  DBUG_PRINT("info",("handlersocket this=%p", this));
  DBUG_PRINT("info",("handlersocket prep_stmt=%p", &p));
  if (p.idx[0] >= '0' && p.idx[0] <= '9') {
    /* numeric */
    TABLE *const table = table_vec[tblnum].table;
    idxnum = atoi(p.idx);
    if (idxnum >= table->s->keys) {
      DBUG_RETURN(1);
    }
  } else {
    const char *const idx_name_to_open = p.idx[0]  == '\0' ? "PRIMARY" : p.idx;
    TABLE *const table = table_vec[tblnum].table;
    for (uint i = 0; i < table->s->keys; ++i) {
      KEY& kinfo = table->key_info[i];
      if (strcmp(kinfo.name, idx_name_to_open) == 0) {
        idxnum = i;
        break;
      }
    }
  }
  if (idxnum == size_t(-1)) {
    DBUG_RETURN(1);
  }

  prep_stmt::fields_type rf;
  prep_stmt::fields_type ff;
  if (!parse_fields(table_vec[tblnum].table, p.retflds, rf)) {
    DBUG_RETURN(2);
  }
  if (!parse_fields(table_vec[tblnum].table, p.filflds, ff)) {
    DBUG_RETURN(2);
  }
  p.idxnum = idxnum;
  p.ret_fields = rf;
  p.filter_fields = ff;
  DBUG_PRINT("info",("handlersocket ret_fields.size=%zu", p.ret_fields.size()));
#ifndef DBUG_OFF
  size_t i;
  for (i = 0; i < p.ret_fields.size(); ++i)
  {
    DBUG_PRINT("info",("handlersocket ret_fields[%zu]=%u", i, p.ret_fields[i]));
  }
#endif
  DBUG_RETURN(0);
}

bool
dbcontext::parse_fields(TABLE *const table, const char *str,
  prep_stmt::fields_type& flds)
{
  string_ref flds_sr(str, strlen(str));
  string_ref_list_wrap fldnms;
  if (flds_sr.size() != 0) {
    split(',', flds_sr, fldnms.string_ref_list);
  }
  for (size_t i = 0; i < fldnms.size(); ++i) {
    Field **fld = 0;
    size_t j = 0;
    for (fld = table->field; *fld; ++fld, ++j) {
      DBG_FLD(fprintf(stderr, "f %s\n", (*fld)->field_name));
      string_ref fn((*fld)->field_name, strlen((*fld)->field_name));
      if (fn == fldnms[i]) {
        break;
      }
    }
    if (*fld == 0) {
      DBG_FLD(fprintf(stderr, "UNKNOWN FLD %s [%s]\n", retflds,
        String(fldnms[i].begin(), fldnms[i].size(), &my_charset_bin).
          c_ptr_safe()));
      return false;
    }
    DBG_FLD(fprintf(stderr, "FLD %s %zu\n", (*fld)->field_name, j));
    flds.push_back(j);
  }
  return true;
}

enum db_write_op {
  db_write_op_none = 0,
  db_write_op_insert = 1,
  db_write_op_sql = 2,
};

void
dbcontext::cmd_exec(dbcallback_i& cb, const cmd_exec_args& args)
{
  prep_stmt& p = *args.pst;
  if (p.get_table_id() == static_cast<size_t>(-1)) {
    return cb.dbcb_resp_short(2, "stmtnum");
  }
  thd->clear_error();
  ha_rkey_function find_flag = HA_READ_KEY_EXACT;
  db_write_op wrop = db_write_op_none;
  if (args.op.size() == 1) {
    switch (args.op.begin()[0]) {
    case '=':
      find_flag = HA_READ_KEY_EXACT;
      break;
    case '>':
      find_flag = HA_READ_AFTER_KEY;
      break;
    case '<':
      find_flag = HA_READ_BEFORE_KEY;
      break;
    case '+':
      wrop = db_write_op_insert;
      break;
    case 'S':
      wrop = db_write_op_sql;
      break;
    default:
      return cb.dbcb_resp_short(2, "op");
    }
  } else if (args.op.size() == 2 && args.op.begin()[1] == '=') {
    switch (args.op.begin()[0]) {
    case '>':
      find_flag = HA_READ_KEY_OR_NEXT;
      break;
    case '<':
      find_flag = HA_READ_KEY_OR_PREV;
      break;
    default:
      return cb.dbcb_resp_short(2, "op");
    }
  } else {
    return cb.dbcb_resp_short(2, "op");
  }
  if (args.kvalslen <= 0) {
    return cb.dbcb_resp_short(2, "klen");
  }
  if (p.reopen_failed) {
    return cb.dbcb_resp_short(2, "reopen_field");
  }
  if (table_vec[p.table_id].reopen_failed) {
    return cb.dbcb_resp_short(2, "reopen_table");
  }

#ifdef HA_CAN_BULK_ACCESS
  if (
    bulk_exec_phase == 1 &&
    !(table_vec[p.table_id].table->file->ha_table_flags() &
      HA_CAN_BULK_ACCESS)
  ) {
    set_need_bulk_exec_finish();
    return;
  }
#endif
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
  ret_fields_info.info_size = p.ret_fields.size();
  ret_fields_info.info = &p.ret_fields[0];
  table_vec[p.table_id].table->file->info_push(INFO_KIND_HS_RET_FIELDS,
    &ret_fields_info);
#endif
  switch (wrop) {
  case db_write_op_none:
    return cmd_find_internal(cb, p, find_flag, args);
  case db_write_op_insert:
    return cmd_insert_internal(cb, p, args.kvals, args.kvalslen, args);
  case db_write_op_sql:
    return cmd_sql_internal(cb, p, args.kvals, args.kvalslen);
  }
}

void
dbcontext::set_statistics(size_t num_conns, size_t num_active)
{
  thd_proc_info(thd, info_message_buf);
  if (for_write_flag) {
    set_thread_message(
      "handlersocket: mode=wr, %zu conns, %zu active, %zu tables",
      num_conns, num_active, table_vec.size());
  } else {
    set_thread_message(
      "handlersocket: mode=rd, %zu conns, %zu active, %zu tables",
      num_conns, num_active, table_vec.size());
  }
}

void
dbcontext::general_log(const char *query, uint query_length, size_t conn_pos)
{
  if (handlersocket_general_log)
  {
    size_t tid_shift = conn_pos *
      (handlersocket_threads + handlersocket_threads_wr);
    thd->thread_id += tid_shift;
    general_log_write(thd, COM_QUERY, query, query_length);
    thd->thread_id -= tid_shift;
  }
}

void
dbcontext::set_start_time()
{
  thd->start_utime = thd->current_utime();
  thd->utime_after_lock = thd->start_utime;
  thd->sent_row_count = 0;
  thd->examined_row_count = 0;
  thd->enable_slow_log = handlersocket_slow_log;
}

void
dbcontext::slow_log(const char *query, uint query_length)
{
  if (thd->enable_slow_log)
  {
    ulonglong end_utime = thd->current_utime();
    if (handlersocket_long_exec_time <= end_utime - thd->start_utime)
    {
      thd->status_var.long_query_count++;
      slow_log_print(thd, query, query_length, end_utime);
    }
  }
}

#ifdef HA_CAN_BULK_ACCESS
void
dbcontext::bulk_exec_prepare(hstcpsvr_worker_i *wk)
{
  DBUG_ENTER("dbcontext::bulk_exec_prepare");
  bulk_exec_size = handlersocket_bulk_exec_size;
  if (bulk_exec_size && !bulk_cb_list)
  {
    dbcallback_i **cb_list;
    if (!(bulk_cb_list = (cb_list_hld *)
      my_multi_malloc(MYF(MY_WME),
        &bulk_cb_list, sizeof(cb_list_hld),
        &cb_list, sizeof(dbcallback_i *) * bulk_exec_size,
        NullS))
    ) {
      bulk_exec_size = 0;
    } else {
      bulk_cb_list->cb_list_size = bulk_exec_size;
      bulk_cb_list->cb_list = cb_list;
      bulk_cb_list->next = NULL;
    }
  }

  /* set first pos */
  bulk_cb_current = bulk_cb_list;
  bulk_cb_current_pos = 0;
  bulk_cb_add_count = 0;
  bulk_cb_executed = bulk_cb_list;
  bulk_cb_executed_pos = 0;
  bulk_exec_wk = wk;
  if (bulk_exec_size)
    bulk_exec_phase = 1;
  else
    bulk_exec_phase = 0;
  DBUG_VOID_RETURN;
}

uint
dbcontext::get_bulk_exec_size()
{
  return bulk_exec_size;
}

uint
dbcontext::get_bulk_exec_phase()
{
  return bulk_exec_phase;
}

bool
dbcontext::add_bulk_exec_conn(dbcallback_i *conn)
{
  DBUG_ENTER("dbcontext::add_bulk_exec_conn");
  if (need_bulk_exec_finish)
  {
    /* finish bulk execution and shift normal execution */
    bulk_exec_finish();
    bulk_exec_size = 0;
    bulk_exec_phase = 0;
    DBUG_RETURN(FALSE);
  }
  if (bulk_exec_phase)
  {
    if (bulk_cb_add_count >= bulk_exec_size)
    {
      /* bulk execution */
      bulk_exec_exec();
      bulk_cb_add_count = 0;
    }
    conn->bulk_req_prepare(bulk_exec_wk);
    if (bulk_cb_current->cb_list_size == bulk_cb_current_pos)
    {
      if (!bulk_cb_current->next)
      {
        /* need to create additional cb_list_hld */
        dbcallback_i **cb_list;
        cb_list_hld *cl_hld;
        if (!(cl_hld = (cb_list_hld *)
          my_multi_malloc(MYF(MY_WME),
            &cl_hld, sizeof(cb_list_hld),
            &cb_list, sizeof(dbcallback_i *) * bulk_exec_size,
            NullS))
        ) {
          /* finish bulk execution and shift normal execution */
          bulk_exec_finish();
          bulk_exec_size = 0;
          bulk_exec_phase = 0;
          DBUG_RETURN(TRUE);
        } else {
          cl_hld->cb_list_size = bulk_exec_size;
          cl_hld->cb_list = cb_list;
          cl_hld->next = NULL;
          bulk_cb_current->next = cl_hld;
        }
      }
      bulk_cb_current = bulk_cb_current->next;
      bulk_cb_current_pos = 0;
    }
    bulk_cb_current->cb_list[bulk_cb_current_pos] = conn;
    bulk_cb_current_pos++;
    bulk_cb_add_count++;
  }
  DBUG_RETURN(FALSE);
}

void
dbcontext::bulk_exec_exec()
{
  cb_list_hld *cl_hld = bulk_cb_executed;
  uint cl_pos = bulk_cb_executed_pos;
  DBUG_ENTER("dbcontext::bulk_exec_exec");
  while (cl_hld != bulk_cb_current || cl_pos < bulk_cb_current_pos)
  {
    if (cl_pos >= cl_hld->cb_list_size)
    {
      cl_hld = cl_hld->next;
      cl_pos = 0;
    }
    cl_hld->cb_list[cl_pos]->bulk_req_exec();
    cl_pos++;
  }
  bulk_cb_executed = cl_hld;
  bulk_cb_executed_pos = cl_pos;
  DBUG_VOID_RETURN;
}

void
dbcontext::bulk_exec_finish()
{
  DBUG_ENTER("dbcontext::bulk_exec_finish");
  if (bulk_cb_add_count)
  {
    /* bulk execution */
    bulk_exec_exec();
    bulk_cb_add_count = 0;
  }

  if (bulk_exec_size)
  {
    bulk_exec_phase = 2;
    cb_list_hld *cl_hld = bulk_cb_list;
    uint cl_pos = 0;
    while (cl_hld != bulk_cb_current || cl_pos < bulk_cb_current_pos)
    {
      if (cl_pos >= cl_hld->cb_list_size)
      {
        cl_hld = cl_hld->next;
        cl_pos = 0;
      }
      cl_hld->cb_list[cl_pos]->bulk_req_finish();
      cl_pos++;
    }
  }

  need_bulk_exec_finish = FALSE;
  DBUG_VOID_RETURN;
}

void
dbcontext::set_need_bulk_exec_finish()
{
  DBUG_ENTER("dbcontext::set_need_bulk_exec_finish");
  need_bulk_exec_finish = TRUE;
  DBUG_VOID_RETURN;
}
#endif

};

