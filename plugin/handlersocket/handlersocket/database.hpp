
// vim:sw=2:ai

/*
 * Copyright (C) 2010-2011 DeNA Co.,Ltd.. All rights reserved.
 * Copyright (C) 2011 Kentoku SHIBA
 * See COPYRIGHT.txt for details.
 */

#ifndef DENA_DATABASE_HPP
#define DENA_DATABASE_HPP

#include "string_buffer.hpp"
#include "string_ref.hpp"
#include "config.hpp"

#ifdef HA_CAN_BULK_ACCESS
#define HANDLERSOCKET_STORE_MESSAGE_SIZE 81
#endif

namespace dena {

struct hstcpsvr_worker_i;

struct database_i;
typedef volatile database_i *database_ptr;

struct dbcontext_i;
typedef dbcontext_i *dbcontext_ptr;

struct prep_stmt_list_wrap;

#ifdef HA_CAN_BULK_ACCESS
struct req_hld;
struct req_hld_list;
struct cb_list_hld;
#endif

struct database_i {
  virtual ~database_i() { }
  virtual dbcontext_ptr create_context(bool for_write) volatile = 0;
  virtual void stop() volatile = 0;
  virtual const config& get_conf() const volatile = 0;
  static database_ptr create(const config& conf);
};

struct uint32_list_wrap {
  uint32_list_wrap();
  virtual ~uint32_list_wrap();
  virtual void init();
  virtual void deinit();
  virtual void clear();
  virtual void push_back(uint32 e);
  bool resize(size_t new_size) {
    if (uint32_list_init) {
        if (uint32_list.max_element < new_size && allocate_dynamic(
          &uint32_list, new_size)) return TRUE;
        uint32_list.elements = new_size; return FALSE; }
    return TRUE; }
  size_t size() {
    return uint32_list_init ? uint32_list.elements : 0; }
  bool empty() {
    return uint32_list_init ? uint32_list.elements ? FALSE : TRUE : TRUE; }
  uint32 &operator [](size_t n) {
    return ((uint32 *) (uint32_list.buffer +
      uint32_list.size_of_element * n))[0]; }
  uint32_list_wrap& operator =(const uint32_list_wrap& x) {
    if (x.uint32_list_init && uint32_list_init)
    {
      if (!resize(x.uint32_list.elements))
        memcpy(uint32_list.buffer, x.uint32_list.buffer,
          uint32_list.size_of_element * uint32_list.elements);
    }
    return *this;
  }
  bool uint32_list_init;
  DYNAMIC_ARRAY uint32_list;
};

struct prep_stmt {
  typedef uint32_list_wrap fields_type; /* uint32 */
 public:
  dbcontext_i *dbctx; /* must be valid while *this is alive */
  size_t table_id; /* a prep_stmt object holds a refcount of the table */
  size_t idxnum;
  fields_type ret_fields;
  fields_type filter_fields;
  char *idx;
  char *retflds;
  char *filflds;
  size_t *refcnt;
  bool reopen_failed;
 public:
  prep_stmt();
  prep_stmt(dbcontext_i *c, size_t tbl, size_t idx, const fields_type& rf,
    const fields_type& ff);
  ~prep_stmt();
  prep_stmt(prep_stmt& x);
  prep_stmt& operator =(prep_stmt& x);
 public:
  size_t get_table_id() const { return table_id; }
  size_t get_idxnum() const { return idxnum; }
  fields_type& get_ret_fields() { return ret_fields; }
  const fields_type& get_ret_fields() const { return ret_fields; }
  fields_type& get_filter_fields() { return filter_fields; }
  const fields_type& get_filter_fields() const { return filter_fields; }
};

struct dbcallback_i {
  virtual ~dbcallback_i () { }
  virtual void dbcb_set_prep_stmt(size_t pst_id, prep_stmt& v) = 0;
  virtual const prep_stmt *dbcb_get_prep_stmt(size_t pst_id) = 0;
  virtual void dbcb_resp_short(uint32 code, const char *msg) = 0;
  virtual void dbcb_resp_short_with_num(uint32 code, uint32 value, const char *msg) = 0;
  virtual void dbcb_resp_short_num(uint32 code, uint32 value) = 0;
  virtual void dbcb_resp_short_num64(uint32_t code, uint64_t value) = 0;
  virtual void dbcb_resp_begin(size_t num_flds) = 0;
  virtual void dbcb_resp_entry(const char *fld, size_t fldlen) = 0;
  virtual void dbcb_resp_end() = 0;
  virtual void dbcb_resp_cancel() = 0;
#ifdef HA_CAN_BULK_ACCESS
  virtual void bulk_req_prepare(hstcpsvr_worker_i *wk) = 0;
  virtual uint get_bulk_req_phase() = 0;
  virtual req_hld *get_bulk_req() = 0;
  virtual bool add_bulk_req_req() = 0;
  virtual void bulk_req_exec() = 0;
  virtual void bulk_req_finish() = 0;
#endif
};

enum record_filter_type {
  record_filter_type_skip = 0,
  record_filter_type_break = 1,
};

struct record_filter {
  record_filter_type filter_type;
  string_ref op;
  uint32 ff_offset; /* offset in filter_fields */
  string_ref val;
  record_filter() : filter_type(record_filter_type_skip), ff_offset(0) { }
};

struct cmd_open_args {
  size_t pst_id;
  const char *dbn;
  const char *tbl;
  const char *idx;
  const char *retflds;
  const char *filflds;
  cmd_open_args() : pst_id(0), dbn(0), tbl(0), idx(0), retflds(0),
    filflds(0) { }
};

struct cmd_exec_args {
  prep_stmt *pst;
  string_ref op;
  const string_ref *kvals;
  size_t kvalslen;
  uint32 limit;
  uint32 skip;
  string_ref mod_op;
  const string_ref *uvals; /* size must be pst->retfieelds.size() */
  const record_filter *filters;
  int invalues_keypart;
  const string_ref *invalues;
  size_t invalueslen;
  char *start;
  char *finish;
  cmd_exec_args() : pst(0), kvals(0), kvalslen(0), limit(0), skip(0),
    uvals(0), filters(0), invalues_keypart(-1), invalues(0), invalueslen(0) { }
};

struct dbcontext_i {
  virtual ~dbcontext_i() { }
  virtual void init_thread(const void *stack_bottom,
    volatile int& shutdown_flag) = 0;
  virtual void term_thread() = 0;
  virtual bool check_alive() = 0;
  virtual void end_bulk_insert_if() = 0;
  virtual void lock_tables_if() = 0;
  virtual void unlock_tables_if() = 0;
  virtual bool get_commit_error() = 0;
  virtual bool skip_unlock_tables() = 0;
  virtual void clear_error() = 0;
  virtual void close_tables_if(bool semi_close) = 0;
  virtual void semi_close_tables() = 0;
  virtual size_t table_refcount(size_t tbl_id) = 0;
  virtual void table_addref(size_t tbl_id) = 0; /* TODO: hide */
  virtual void table_release(size_t tbl_id) = 0; /* TODO: hide */
  virtual void cmd_open(dbcallback_i& cb, const cmd_open_args& args) = 0;
  virtual void cmd_exec(dbcallback_i& cb, const cmd_exec_args& args) = 0;
  virtual void set_statistics(size_t num_conns, size_t num_active) = 0;
  virtual void general_log(const char *query, uint query_length,
    size_t conn_pos) = 0;
  virtual void set_start_time() = 0;
  virtual void slow_log(const char *query, uint query_length) = 0;
  virtual int reopen_tables(dbcallback_i& cb,
    prep_stmt_list_wrap &prep_stmts) = 0;
#ifdef HA_CAN_BULK_ACCESS
  virtual void bulk_exec_prepare(hstcpsvr_worker_i *wk) = 0;
  virtual uint get_bulk_exec_size() = 0;
  virtual uint get_bulk_exec_phase() = 0;
  virtual bool add_bulk_exec_conn(dbcallback_i *cb) = 0;
  virtual void bulk_exec_finish() = 0;
  virtual void set_need_bulk_exec_finish() = 0;
#endif
};

struct prep_stmt_list_wrap {
  prep_stmt_list_wrap();
  virtual ~prep_stmt_list_wrap();
  void clear();
  void push_back(prep_stmt &e);
  bool resize(size_t new_size);
  size_t size();
  bool empty();
  prep_stmt &operator [](size_t n);
  bool prep_stmt_list_init;
  DYNAMIC_ARRAY prep_stmt_list;
};

struct record_filter_list_wrap {
  record_filter_list_wrap() {
    if (my_init_dynamic_array2(&record_filter_list, sizeof(record_filter),
      NULL, 16, 16))
      record_filter_list_init = FALSE;
    else
      record_filter_list_init = TRUE;
  }
  virtual ~record_filter_list_wrap() {
    if (record_filter_list_init) delete_dynamic(&record_filter_list); }
  void clear() {
    if (record_filter_list_init) record_filter_list.elements = 0; }
  void push_back(record_filter &e) {
    if (record_filter_list_init) insert_dynamic(&record_filter_list, (uchar*) &e);
    return; }
  bool resize(size_t new_size) {
    if (record_filter_list_init) {
        if (record_filter_list.max_element < new_size && allocate_dynamic(
          &record_filter_list, new_size)) return TRUE;
        record_filter_list.elements = new_size; return FALSE; }
    return TRUE; }
  size_t size() {
    return record_filter_list_init ? record_filter_list.elements : 0; }
  bool empty() {
    return record_filter_list_init ? record_filter_list.elements ?
      FALSE : TRUE : TRUE; }
  record_filter &operator [](size_t n) {
    return ((record_filter *) (record_filter_list.buffer +
      record_filter_list.size_of_element * n))[0]; }
  bool record_filter_list_init;
  DYNAMIC_ARRAY record_filter_list;
};

#ifdef HA_CAN_BULK_ACCESS
struct req_hld {
  /* target handler, set null if there is no target */
  handler *hnd;
  void *req_pos;

  /* param of do_exec_on_index */
  char *cmd_begin;
  char *cmd_end;
  char *start;
  char *finish;

  bool resp_called;
  /* set error_code if error is caused, default value is 0 */
  uint32 error_code;
  /* error message type */
  /*
    1:dbcb_resp_short
    2:dbcb_resp_short_with_num
    3:dbcb_resp_short_num
    4:dbcb_resp_short_num64
  */
  uint32 error_type;
  char error_msg[HANDLERSOCKET_STORE_MESSAGE_SIZE];
  uint32 error_value32;
  uint64 error_value64;
};

struct req_hld_list {
  uint req_list_size;
  req_hld *req_list;
  req_hld_list *next;
};

struct cb_list_hld {
  uint cb_list_size;
  dbcallback_i **cb_list;
  cb_list_hld *next;
};
#endif

};

extern unsigned long long int open_tables_count;
extern unsigned long long int close_tables_count;
extern unsigned long long int lock_tables_count;
extern unsigned long long int unlock_tables_count;
#if 0
extern unsigned long long int index_exec_count;
#endif

#endif

