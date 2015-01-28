
// vim:sw=2:ai

/*
 * Copyright (C) 2010-2011 DeNA Co.,Ltd.. All rights reserved.
 * Copyright (C) 2011 Kentoku SHIBA
 * See COPYRIGHT.txt for details.
 */

#if __linux__
#include <sys/epoll.h>
#endif

#include "hstcpsvr_worker.hpp"
#include "string_buffer.hpp"
#include "auto_ptrcontainer.hpp"
#include "string_util.hpp"
#include "escape.hpp"
#include "mysql_incl.hpp"

#define DBG_FD(x)
#define DBG_TR(x)
#define DBG_EP(x)
#define DBG_MULTI(x)

/* TODO */
#if !defined(__linux__) && !defined(__FreeBSD__) && !defined(MSG_NOSIGNAL)
#define MSG_NOSIGNAL 0
#endif

namespace dena {

struct auto_alloca_free_string_ref {
  auto_alloca_free_string_ref(string_ref *value) : value(value) { }
  ~auto_alloca_free_string_ref() {
    /* no-op if alloca() is used */
    DENA_ALLOCA_FREE(value);
  }
 private:
  string_ref *value;
};

struct epoll_event_list_wrap {
  epoll_event_list_wrap() {
    if (my_init_dynamic_array2(&epoll_event_list, sizeof(epoll_event),
      NULL, 16, 16))
      epoll_event_list_init = FALSE;
    else
      epoll_event_list_init = TRUE;
  }
  virtual ~epoll_event_list_wrap() {
    if (epoll_event_list_init) delete_dynamic(&epoll_event_list); }
  void clear() {
    if (epoll_event_list_init) epoll_event_list.elements = 0; }
  void push_back(epoll_event &e) {
    if (epoll_event_list_init) insert_dynamic(&epoll_event_list, (uchar*) &e);
    return; }
  bool resize(size_t new_size) {
    if (epoll_event_list_init) {
        if (epoll_event_list.max_element < new_size && allocate_dynamic(
          &epoll_event_list, new_size)) return TRUE;
        epoll_event_list.elements = new_size; return FALSE; }
    return TRUE; }
  size_t size() {
    return epoll_event_list_init ? epoll_event_list.elements : 0; }
  bool empty() {
    return epoll_event_list_init ? epoll_event_list.elements ?
      FALSE : TRUE : TRUE; }
  epoll_event &operator [](size_t n) {
    return ((epoll_event *) (epoll_event_list.buffer +
      epoll_event_list.size_of_element * n))[0]; }
  bool epoll_event_list_init;
  DYNAMIC_ARRAY epoll_event_list;
};

struct dbconnstate {
  string_buffer readbuf;
  string_buffer writebuf;
  prep_stmt_list_wrap prep_stmts;
  size_t resp_begin_pos;
  size_t find_nl_pos;
#ifdef HA_CAN_BULK_ACCESS
  char *line_begin;
#endif
  void reset() {
    readbuf.clear();
    writebuf.clear();
    prep_stmts.clear();
    resp_begin_pos = 0;
    find_nl_pos = 0;
  }
  dbconnstate() : resp_begin_pos(0), find_nl_pos(0) { }
};

struct hstcpsvr_conn;
struct hstcpsvr_conn_ptr_list_wrap;
typedef hstcpsvr_conn *hstcpsvr_conn_ptr;
typedef hstcpsvr_conn_ptr_list_wrap hstcpsvr_conns_type;

struct hstcpsvr_conn : public dbcallback_i {
 public:
  auto_file fd;
  sockaddr_storage addr;
  socklen_t addr_len;
  dbconnstate cstate;
  String err;
  size_t readsize;
  bool nonblocking;
  bool read_finished;
  bool write_finished;
  time_t nb_last_io;
/*
  hstcpsvr_conn_ptr *conns_iter;
*/
  bool authorized;
  uint32 number_of_list;
  dbcontext_i *dbctx;
#ifdef HA_CAN_BULK_ACCESS
  uint bulk_req_phase;
  uint bulk_req_size;
  req_hld_list *bulk_req_list;
  req_hld_list *bulk_req_current;
  uint bulk_req_current_pos;
  hstcpsvr_worker_i *bulk_req_wk;
#endif
 public:
  bool closed() const;
  bool ok_to_close() const;
  void reset();
  int accept(const hstcpsvr_shared_c& cshared);
  bool write_more(bool *more_r = 0);
  bool read_more(bool *more_r = 0);
 public:
  virtual ~hstcpsvr_conn();
  virtual void dbcb_set_prep_stmt(size_t pst_id, prep_stmt& v);
  virtual const prep_stmt *dbcb_get_prep_stmt(size_t pst_id);
  virtual void dbcb_resp_short(uint32 code, const char *msg);
  virtual void dbcb_resp_short_with_num(uint32 code, uint32 value, const char *msg);
  virtual void dbcb_resp_short_num(uint32 code, uint32 value);
  virtual void dbcb_resp_short_num64(uint32_t code, uint64_t value);
  virtual void dbcb_resp_begin(size_t num_flds);
  virtual void dbcb_resp_entry(const char *fld, size_t fldlen);
  virtual void dbcb_resp_end();
  virtual void dbcb_resp_cancel();
#ifdef HA_CAN_BULK_ACCESS
  virtual void bulk_req_prepare(hstcpsvr_worker_i *wk);
  virtual uint get_bulk_req_phase();
  virtual req_hld *get_bulk_req();
  virtual bool add_bulk_req_req();
  virtual void bulk_req_exec();
  virtual void bulk_req_finish();
#endif
 public:
  hstcpsvr_conn() : addr_len(sizeof(addr)), readsize(4096),
    nonblocking(false), read_finished(false), write_finished(false),
    nb_last_io(0), authorized(false),
#ifdef HA_CAN_BULK_ACCESS
    bulk_req_phase(0), bulk_req_size(0), bulk_req_list(NULL),
    bulk_req_current(NULL), bulk_req_current_pos(0), bulk_req_wk(NULL)
#endif
    { }
};


struct hstdbctx_holder {
  hstdbctx_holder() {}
  ~hstdbctx_holder() {
    if (dbctx)
      delete dbctx;
  }
  dbcontext_ptr dbctx;
};

struct hstcpsvr_conn_ptr_list_wrap {
  hstcpsvr_conn_ptr_list_wrap() {
    if (my_init_dynamic_array2(&hstcpsvr_conn_ptr_list,
      sizeof(hstcpsvr_conn_ptr), NULL, 16, 16))
      hstcpsvr_conn_ptr_list_init = FALSE;
    else
      hstcpsvr_conn_ptr_list_init = TRUE;
  }
  hstcpsvr_conn_ptr *begin() {
    return (hstcpsvr_conn_ptr *) hstcpsvr_conn_ptr_list.buffer; }
  const hstcpsvr_conn_ptr *begin() const {
    return (hstcpsvr_conn_ptr *) hstcpsvr_conn_ptr_list.buffer; }
  hstcpsvr_conn_ptr *end() {
    return &((hstcpsvr_conn_ptr *) hstcpsvr_conn_ptr_list.buffer)[
      hstcpsvr_conn_ptr_list.elements]; }
  const hstcpsvr_conn_ptr *end() const {
    return &((hstcpsvr_conn_ptr *) hstcpsvr_conn_ptr_list.buffer)[
      hstcpsvr_conn_ptr_list.elements]; }
  size_t size() {
    return hstcpsvr_conn_ptr_list_init ? hstcpsvr_conn_ptr_list.elements : 0; }
  size_t max_size() {
    return hstcpsvr_conn_ptr_list_init ?
      hstcpsvr_conn_ptr_list.max_element : 0; }
  bool empty() {
    return hstcpsvr_conn_ptr_list_init ? hstcpsvr_conn_ptr_list.elements ?
      FALSE : TRUE : TRUE; }
  hstcpsvr_conn_ptr front() {
    return ((hstcpsvr_conn_ptr *) hstcpsvr_conn_ptr_list.buffer)[0]; }
  const hstcpsvr_conn_ptr front() const {
    return ((hstcpsvr_conn_ptr *) hstcpsvr_conn_ptr_list.buffer)[0]; }
  hstcpsvr_conn_ptr back() {
    return ((hstcpsvr_conn_ptr *) hstcpsvr_conn_ptr_list.buffer)[
      hstcpsvr_conn_ptr_list.elements - 1]; }
  const hstcpsvr_conn_ptr back() const {
    return ((hstcpsvr_conn_ptr *) hstcpsvr_conn_ptr_list.buffer)[
      hstcpsvr_conn_ptr_list.elements - 1]; }
  void swap(hstcpsvr_conn_ptr_list_wrap& x) {
    if (hstcpsvr_conn_ptr_list_init) {
      bool tmp_init = x.hstcpsvr_conn_ptr_list_init;
      DYNAMIC_ARRAY tmp_list = x.hstcpsvr_conn_ptr_list;
      x.hstcpsvr_conn_ptr_list_init = hstcpsvr_conn_ptr_list_init;
      x.hstcpsvr_conn_ptr_list = hstcpsvr_conn_ptr_list;
      hstcpsvr_conn_ptr_list_init = tmp_init;
      hstcpsvr_conn_ptr_list = tmp_list;
      }
    }
  virtual ~hstcpsvr_conn_ptr_list_wrap() {
    if (hstcpsvr_conn_ptr_list_init) {
      clear();
      delete_dynamic(&hstcpsvr_conn_ptr_list); }
      }
  bool push_back_ptr(hstcpsvr_conn_ptr e) {
    if (hstcpsvr_conn_ptr_list_init) {
      e->number_of_list = hstcpsvr_conn_ptr_list.elements;
      if (insert_dynamic(&hstcpsvr_conn_ptr_list, (uchar*) &e)) return TRUE;
      return FALSE; }
    return TRUE; }
  void erase_ptr(hstcpsvr_conn_ptr *i) {
    hstcpsvr_conn_ptr e = *i;
    erase_ptr(e);
  }
  void erase_ptr(hstcpsvr_conn_ptr e) {
/*
    uint32 n,
      end = min(e->number_of_list, hstcpsvr_conn_ptr_list.elements - 1);
    for (n = 0; n <= end; n++)
    {
      if (((hstcpsvr_conn_ptr *) (hstcpsvr_conn_ptr_list.buffer +
        hstcpsvr_conn_ptr_list.size_of_element * (end - n)))[0] == e)
        break;
    }
    if (n > end) {
      fatal_abort("invalid target ptr");
    } else {
      if (hstcpsvr_conn_ptr_list.elements > end - n + 1)
        delete_dynamic_element(&hstcpsvr_conn_ptr_list, end - n);
      else
        hstcpsvr_conn_ptr_list.elements--;
      delete (e);
    }
*/
    hstcpsvr_conn_ptr_list.elements--;
    if (e->number_of_list < hstcpsvr_conn_ptr_list.elements)
    {
      memcpy(hstcpsvr_conn_ptr_list.buffer +
        hstcpsvr_conn_ptr_list.size_of_element * e->number_of_list,
        hstcpsvr_conn_ptr_list.buffer +
        hstcpsvr_conn_ptr_list.size_of_element *
        hstcpsvr_conn_ptr_list.elements,
        hstcpsvr_conn_ptr_list.size_of_element);
      ((hstcpsvr_conn_ptr *) (hstcpsvr_conn_ptr_list.buffer +
        hstcpsvr_conn_ptr_list.size_of_element * e->number_of_list))[0]->
        number_of_list = e->number_of_list;
    }
    delete (e);
  }
  hstcpsvr_conn_ptr &operator [](size_t n) {
    return ((hstcpsvr_conn_ptr *)
      (hstcpsvr_conn_ptr_list.buffer +
      hstcpsvr_conn_ptr_list.size_of_element * n))[0]; }
  const hstcpsvr_conn_ptr &operator [](size_t n) const {
    return ((hstcpsvr_conn_ptr *)
      (hstcpsvr_conn_ptr_list.buffer +
      hstcpsvr_conn_ptr_list.size_of_element * n))[0]; }
  void clear() {
    if (hstcpsvr_conn_ptr_list_init) {
      uint32 i;
      hstcpsvr_conn_ptr ptr;
      for (i = 0; i < hstcpsvr_conn_ptr_list.elements; i++) {
        ptr = ((hstcpsvr_conn_ptr *) (hstcpsvr_conn_ptr_list.buffer +
          hstcpsvr_conn_ptr_list.size_of_element * i))[0];
        delete ptr; }
      hstcpsvr_conn_ptr_list.elements = 0; }
    }
  bool hstcpsvr_conn_ptr_list_init;
  DYNAMIC_ARRAY hstcpsvr_conn_ptr_list;
};

struct hstcpsvr_worker : public hstcpsvr_worker_i, private noncopyable {
  hstcpsvr_worker(const hstcpsvr_worker_arg& arg);
  ~hstcpsvr_worker();
  virtual void run();
 private:
  const hstcpsvr_shared_c& cshared;
  volatile hstcpsvr_shared_v& vshared;
  long worker_id;
  hstdbctx_holder dbctx_holder;
  dbcontext_ptr dbctx;
  hstcpsvr_conns_type conns; /* conns refs dbctx */
  time_t last_check_time;
  pollfd_list_wrap pfds;
  #ifdef __linux__
  epoll_event_list_wrap events_vec;
  auto_file epoll_fd;
  #endif
  bool accept_enabled;
  int accept_balance;
  string_ref_list_wrap invalues_work;
  record_filter_list_wrap filters_work;
 private:
  int run_one_nb();
  int run_one_ep();
  void execute_lines(hstcpsvr_conn& conn, size_t conn_pos);
  void execute_line(char *start, char *finish, hstcpsvr_conn& conn);
  void do_open_index(char *start, char *finish, hstcpsvr_conn& conn);
 public:
  void do_exec_on_index(char *cmd_begin, char *cmd_end, char *start,
    char *finish, hstcpsvr_conn& conn);
 private:
  void do_authorization(char *start, char *finish, hstcpsvr_conn& conn);
};

hstcpsvr_conn::~hstcpsvr_conn()
{
#ifdef HA_CAN_BULK_ACCESS
  while (bulk_req_list)
  {
    req_hld_list *next = bulk_req_list->next;
    safeFree(bulk_req_list);
    bulk_req_list = next;
  }
#endif
}

bool
hstcpsvr_conn::closed() const
{
  return fd.get() < 0;
}

bool
hstcpsvr_conn::ok_to_close() const
{
  return write_finished || (read_finished && cstate.writebuf.size() == 0);
}

void
hstcpsvr_conn::reset()
{
  addr = sockaddr_storage();
  addr_len = sizeof(addr);
  cstate.reset();
  fd.reset();
  read_finished = false;
  write_finished = false;
}

int
hstcpsvr_conn::accept(const hstcpsvr_shared_c& cshared)
{
  reset();
  return socket_accept(cshared.listen_fd.get(), fd, cshared.sockargs, addr,
    addr_len, err);
}

bool
hstcpsvr_conn::write_more(bool *more_r)
{
  if (write_finished || cstate.writebuf.size() == 0) {
    return false;
  }
  const size_t wlen = cstate.writebuf.size();
  ssize_t len = send(fd.get(), cstate.writebuf.begin(), wlen, MSG_NOSIGNAL);
  if (len <= 0) {
    if (len == 0 || !nonblocking || errno != EWOULDBLOCK) {
      cstate.writebuf.clear();
      write_finished = true;
    }
    return false;
  }
  cstate.writebuf.erase_front(len);
    /* FIXME: reallocate memory if too large */
  if (more_r) {
    *more_r = (static_cast<size_t>(len) == wlen);
  }
  return true;
}

bool
hstcpsvr_conn::read_more(bool *more_r)
{
  if (read_finished) {
    return false;
  }
  const size_t block_size = readsize > 4096 ? readsize : 4096;
  char *wp = cstate.readbuf.make_space(block_size);
  ssize_t len;
  while ((len = read(fd.get(), wp, block_size)) <= 0) {
    if (len < 0 && (errno == EINTR || errno == EAGAIN))
      continue;
    else if (len == 0 || !nonblocking || errno != EWOULDBLOCK) {
      read_finished = true;
    }
    return false;
  }
  cstate.readbuf.space_wrote(len);
  if (more_r) {
    *more_r = (static_cast<size_t>(len) == block_size);
  }
  return true;
}

void
hstcpsvr_conn::dbcb_set_prep_stmt(size_t pst_id, prep_stmt& v)
{
  if (cstate.prep_stmts.size() <= pst_id) {
    cstate.prep_stmts.resize(pst_id + 1);
  }
  cstate.prep_stmts[pst_id] = v;
}

const prep_stmt *
hstcpsvr_conn::dbcb_get_prep_stmt(size_t pst_id)
{
  if (cstate.prep_stmts.size() <= pst_id) {
    return 0;
  }
  return &cstate.prep_stmts[pst_id];
}

void
hstcpsvr_conn::dbcb_resp_short(uint32 code, const char *msg)
{
  size_t msglen = strlen(msg);
#ifdef HA_CAN_BULK_ACCESS
  if (bulk_req_phase == 1)
  {
    req_hld *req = &bulk_req_current->req_list[bulk_req_current_pos - 1];
    req->resp_called = TRUE;
    req->error_type = 1;
    req->error_code = code;
    if (msglen >= HANDLERSOCKET_STORE_MESSAGE_SIZE)
      msglen = HANDLERSOCKET_STORE_MESSAGE_SIZE - 1;
    memcpy(req->error_msg, msg, msglen);
    req->error_msg[msglen] = '\0';
    return;
  }
#endif
  write_ui32(cstate.writebuf, code);
  if (msglen != 0) {
    cstate.writebuf.append_literal("\t1\t");
    cstate.writebuf.append(msg, msg + msglen);
  } else {
    cstate.writebuf.append_literal("\t1");
  }
  cstate.writebuf.append_literal("\n");
}

void
hstcpsvr_conn::dbcb_resp_short_with_num(uint32 code, uint32 value, const char *msg)
{
  size_t msglen = strlen(msg);
#ifdef HA_CAN_BULK_ACCESS
  if (bulk_req_phase == 1)
  {
    req_hld *req = &bulk_req_current->req_list[bulk_req_current_pos - 1];
    req->resp_called = TRUE;
    req->error_type = 2;
    req->error_code = code;
    req->error_value32 = value;
    if (msglen >= HANDLERSOCKET_STORE_MESSAGE_SIZE)
      msglen = HANDLERSOCKET_STORE_MESSAGE_SIZE - 1;
    memcpy(req->error_msg, msg, msglen);
    req->error_msg[msglen] = '\0';
    return;
  }
#endif
  write_ui32(cstate.writebuf, code);
  cstate.writebuf.append_literal("\t1\t");
  write_ui32(cstate.writebuf, value);
  if (msglen != 0) {
    cstate.writebuf.append_literal(":");
    cstate.writebuf.append(msg, msg + msglen);
  }
  cstate.writebuf.append_literal("\n");
}

void
hstcpsvr_conn::dbcb_resp_short_num(uint32 code, uint32 value)
{
#ifdef HA_CAN_BULK_ACCESS
  if (bulk_req_phase == 1)
  {
    req_hld *req = &bulk_req_current->req_list[bulk_req_current_pos - 1];
    req->resp_called = TRUE;
    req->error_type = 3;
    req->error_code = code;
    req->error_value32 = value;
    return;
  }
#endif
  write_ui32(cstate.writebuf, code);
  cstate.writebuf.append_literal("\t1\t");
  write_ui32(cstate.writebuf, value);
  cstate.writebuf.append_literal("\n");
}

void
hstcpsvr_conn::dbcb_resp_short_num64(uint32_t code, uint64_t value)
{
#ifdef HA_CAN_BULK_ACCESS
  if (bulk_req_phase == 1)
  {
    req_hld *req = &bulk_req_current->req_list[bulk_req_current_pos - 1];
    req->resp_called = TRUE;
    req->error_type = 4;
    req->error_code = code;
    req->error_value64 = value;
    return;
  }
#endif
  write_ui32(cstate.writebuf, code);
  cstate.writebuf.append_literal("\t1\t");
  write_ui64(cstate.writebuf, value);
  cstate.writebuf.append_literal("\n");
}

void
hstcpsvr_conn::dbcb_resp_begin(size_t num_flds)
{
  cstate.resp_begin_pos = cstate.writebuf.size();
  cstate.writebuf.append_literal("0\t");
  write_ui32(cstate.writebuf, num_flds);
}

void
hstcpsvr_conn::dbcb_resp_entry(const char *fld, size_t fldlen)
{
  if (fld != 0) {
    cstate.writebuf.append_literal("\t");
    escape_string(cstate.writebuf, fld, fld + fldlen);
  } else {
    static const char t[] = "\t\0";
    cstate.writebuf.append(t, t + 2);
  }
}

void
hstcpsvr_conn::dbcb_resp_end()
{
  cstate.writebuf.append_literal("\n");
  cstate.resp_begin_pos = 0;
}

void
hstcpsvr_conn::dbcb_resp_cancel()
{
  cstate.writebuf.resize(cstate.resp_begin_pos);
  cstate.resp_begin_pos = 0;
}

#ifdef HA_CAN_BULK_ACCESS
void
hstcpsvr_conn::bulk_req_prepare(hstcpsvr_worker_i *wk)
{
  DBUG_ENTER("hstcpsvr_conn::bulk_req_prepare");
  bulk_req_phase = dbctx->get_bulk_exec_phase();
  bulk_req_size = dbctx->get_bulk_exec_size();
  bulk_req_wk = wk;
  if (bulk_req_phase && !bulk_req_list)
  {
    req_hld *req_list;
    if (!(bulk_req_list = (req_hld_list *)
      my_multi_malloc(MYF(MY_WME),
        &bulk_req_list, sizeof(req_hld_list),
        &req_list, sizeof(req_hld) * bulk_req_size,
        NullS))
    ) {
      bulk_req_size = 0;
    } else {
      bulk_req_list->req_list_size = bulk_req_size;
      bulk_req_list->req_list = req_list;
      bulk_req_list->next = NULL;
    }
  }

  /* set first pos */
  bulk_req_current = bulk_req_list;
  bulk_req_current_pos = 0;
  if (!bulk_req_size)
    bulk_req_phase = 0;
  DBUG_VOID_RETURN;
}

uint
hstcpsvr_conn::get_bulk_req_phase()
{
  return bulk_req_phase;
}

req_hld *
hstcpsvr_conn::get_bulk_req()
{
  return &bulk_req_current->req_list[bulk_req_current_pos - 1];
}

bool
hstcpsvr_conn::add_bulk_req_req()
{
  DBUG_ENTER("hstcpsvr_conn::add_bulk_req_req");
  if (bulk_req_phase)
  {
    if (bulk_req_current->req_list_size == bulk_req_current_pos)
    {
      if (!bulk_req_current->next)
      {
        /* need to create additional cb_list_hld */
        req_hld *req_list;
        req_hld_list *b_req_hld;
        if (!(b_req_hld = (req_hld_list *)
          my_multi_malloc(MYF(MY_WME),
            &b_req_hld, sizeof(req_hld_list),
            &req_list, sizeof(req_hld) * bulk_req_size,
            NullS))
        ) {
          /* finish bulk execution and shift normal execution */
          bulk_req_exec();
          bulk_req_finish();
          bulk_req_size = 0;
          bulk_req_phase = 0;
          DBUG_RETURN(TRUE);
        } else {
          b_req_hld->req_list_size = bulk_req_size;
          b_req_hld->req_list = req_list;
          b_req_hld->next = NULL;
          bulk_req_current->next = b_req_hld;
        }
      }
      bulk_req_current = bulk_req_current->next;
      bulk_req_current_pos = 0;
    }
    req_hld *req = &bulk_req_current->req_list[bulk_req_current_pos];
    req->hnd = NULL;
    req->req_pos = NULL;
    req->resp_called = FALSE;
    req->error_code = 0;
    req->cmd_begin = NULL;
    bulk_req_current_pos++;
  }
  DBUG_RETURN(FALSE);
}

void
hstcpsvr_conn::bulk_req_exec()
{
  req_hld_list *req_list = bulk_req_list;
  uint req_pos = 0;
  DBUG_ENTER("hstcpsvr_conn::bulk_req_exec");
  while (req_list != bulk_req_current || req_pos < bulk_req_current_pos)
  {
    if (req_pos >= req_list->req_list_size)
    {
      req_list = req_list->next;
      req_pos = 0;
    }
    if (
      !req_list->req_list[req_pos].resp_called &&
      req_list->req_list[req_pos].hnd
    ) {
      req_list->req_list[req_pos].hnd->bulk_req_exec();
    }
    req_pos++;
  }
  DBUG_VOID_RETURN;
}

void
hstcpsvr_conn::bulk_req_finish()
{
  DBUG_ENTER("hstcpsvr_conn::bulk_req_finish");
  if (bulk_req_size)
  {
    bulk_req_phase = 2;
    req_hld_list *req_list = bulk_req_list;
    uint req_pos = 0;
    while (req_list != bulk_req_current || req_pos < bulk_req_current_pos)
    {
      if (req_pos >= req_list->req_list_size)
      {
        req_list = req_list->next;
        req_pos = 0;
      }
      req_hld *req = &req_list->req_list[req_pos];
      if (req->hnd) {
        req->hnd->info_push(INFO_KIND_BULK_ACCESS_END, NULL);
      }
      if (req->resp_called) {
        /* set message */
        switch (req->error_type)
        {
          case 1:
            dbcb_resp_short(req->error_code, req->error_msg);
            break;
          case 2:
            dbcb_resp_short_with_num(req->error_code, req->error_value32,
              req->error_msg);
            break;
          case 3:
            dbcb_resp_short_num(req->error_code, req->error_value32);
            break;
          case 4:
            dbcb_resp_short_num64(req->error_code, req->error_value64);
            break;
          default:
            break;
        }
      } else {
        if (req->hnd) {
          req->hnd->info_push(INFO_KIND_BULK_ACCESS_CURRENT, req->req_pos);
        }
        if (req->cmd_begin) {
          ((hstcpsvr_worker *) bulk_req_wk)->do_exec_on_index(
            req->cmd_begin,
            req->cmd_end,
            req->start,
            req->finish,
            *this
          );
        }
      }
      req_pos++;
    }

    if (cstate.line_begin) {
      /* clear read buffer */
      cstate.readbuf.erase_front(cstate.line_begin - cstate.readbuf.begin());
      cstate.find_nl_pos = cstate.readbuf.size();
    }
  }
  DBUG_VOID_RETURN;
}
#endif

hstcpsvr_worker::hstcpsvr_worker(const hstcpsvr_worker_arg& arg)
  : cshared(*arg.cshared), vshared(*arg.vshared), worker_id(arg.worker_id),
    dbctx(cshared.dbptr->create_context(cshared.for_write_flag)),
    last_check_time(time(0)), accept_enabled(true), accept_balance(0)
{
  dbctx_holder.dbctx = dbctx;
  #ifdef __linux__
  if (cshared.sockargs.use_epoll) {
    epoll_fd.reset(epoll_create(10));
    if (epoll_fd.get() < 0) {
      fatal_abort("epoll_create");
    }
    epoll_event ev = { };
    ev.events = EPOLLIN;
    ev.data.ptr = 0;
    if (epoll_ctl(epoll_fd.get(), EPOLL_CTL_ADD, cshared.listen_fd.get(), &ev)
      != 0) {
      fatal_abort("epoll_ctl EPOLL_CTL_ADD");
    }
    events_vec.resize(10240);
  }
  #endif
  accept_balance = cshared.conf.get_int("accept_balance", 0);
}

hstcpsvr_worker::~hstcpsvr_worker()
{
/*
  if (dbctx)
    delete dbctx;
*/
}

namespace {

struct thr_init {
  thr_init(const dbcontext_ptr& dc, volatile int& shutdown_flag) : dbctx(dc) {
    dbctx->init_thread(this, shutdown_flag);
  }
  ~thr_init() {
    dbctx->term_thread();
  }
  const dbcontext_ptr& dbctx;
};

}; // namespace

void
hstcpsvr_worker::run()
{
  thr_init initobj(dbctx, vshared.shutdown);

  #ifdef __linux__
  if (cshared.sockargs.use_epoll) {
    while (!vshared.shutdown && dbctx->check_alive()) {
      run_one_ep();
    }
  } else if (cshared.sockargs.nonblocking) {
    while (!vshared.shutdown && dbctx->check_alive()) {
      run_one_nb();
    }
  } else {
    /* UNUSED */
    fatal_abort("run_one");
  }
  #else
  while (!vshared.shutdown && dbctx->check_alive()) {
    run_one_nb();
  }
  #endif
  conns.clear();
  dbctx->close_tables_if(false);
}

int
hstcpsvr_worker::run_one_nb()
{
  size_t nfds = 0;
  /* CLIENT SOCKETS */
  for (hstcpsvr_conn_ptr *i = conns.begin();
    i != conns.end(); ++i) {
    if (pfds.size() <= nfds) {
      pfds.resize(nfds + 1);
    }
    pollfd& pfd = pfds[nfds++];
    pfd.fd = (*i)->fd.get();
    short ev = 0;
    if ((*i)->cstate.writebuf.size() != 0) {
      ev = POLLOUT;
    } else {
      ev = POLLIN;
    }
    pfd.events = pfd.revents = ev;
  }
  /* LISTENER */
  {
    const size_t cpt = cshared.nb_conn_per_thread;
    const short ev = (cpt > nfds) ? POLLIN : 0;
    if (pfds.size() <= nfds) {
      pfds.resize(nfds + 1);
    }
    pollfd& pfd = pfds[nfds++];
    pfd.fd = cshared.listen_fd.get();
    pfd.events = pfd.revents = ev;
  }
  /* POLL */
  const int npollev = poll(&pfds[0], nfds, 1 * 1000);
  dbctx->set_statistics(conns.size(), npollev);
  const time_t now = time(0);
  size_t j = 0;
  const short mask_in = ~POLLOUT;
  const short mask_out = POLLOUT | POLLERR | POLLHUP | POLLNVAL;
  /* READ */
  for (hstcpsvr_conn_ptr *i = conns.begin(); i != conns.end();
    ++i, ++j) {
    pollfd& pfd = pfds[j];
    if ((pfd.revents & mask_in) == 0) {
      continue;
    }
    hstcpsvr_conn& conn = **i;
    if (conn.read_more()) {
      if (conn.cstate.readbuf.size() > 0) {
        const char ch = conn.cstate.readbuf.begin()[0];
        if (ch == 'Q') {
          vshared.shutdown = 1;
        } else if (ch == '/') {
          conn.cstate.readbuf.clear();
          conn.cstate.find_nl_pos = 0;
          conn.cstate.writebuf.clear();
          conn.read_finished = true;
          conn.write_finished = true;
        }
      }
      conn.nb_last_io = now;
    }
  }
  /* EXECUTE */
#ifdef HA_CAN_BULK_ACCESS
  dbctx->bulk_exec_prepare(this);
#endif
  j = 0;
  for (hstcpsvr_conn_ptr *i = conns.begin(); i != conns.end();
    ++i, ++j) {
    pollfd& pfd = pfds[j];
    if ((pfd.revents & mask_in) == 0 || (*i)->cstate.readbuf.size() == 0) {
      continue;
    }
    /* conn is added to queue */
    dbctx->add_bulk_exec_conn(*i);
    if (!dbctx->reopen_tables(**i, (*i)->cstate.prep_stmts))
    {
      execute_lines(**i, j);
    }
  }
#ifdef HA_CAN_BULK_ACCESS
  dbctx->bulk_exec_finish();
#endif
  /* COMMIT */
  if (!dbctx->skip_unlock_tables())
    dbctx->unlock_tables_if();
  const bool commit_error = dbctx->get_commit_error();
  dbctx->clear_error();
  /* WRITE/CLOSE */
  j = 0;
  for (hstcpsvr_conn_ptr *i = conns.begin(); i != conns.end();
    ++j) {
    pollfd& pfd = pfds[j];
    hstcpsvr_conn& conn = **i;
    hstcpsvr_conn_ptr *icur = i;
    ++i;
    if (commit_error) {
      conn.reset();
      continue;
    }
    if ((pfd.revents & (mask_out | mask_in)) != 0) {
      if (conn.write_more()) {
        conn.nb_last_io = now;
      }
    }
    if (cshared.sockargs.timeout != 0 &&
      conn.nb_last_io + cshared.sockargs.timeout < now) {
      conn.reset();
    }
    if (conn.closed() || conn.ok_to_close()) {
      conns.erase_ptr(icur);
    }
  }
  /* ACCEPT */
  {
    pollfd& pfd = pfds[nfds - 1];
    if ((pfd.revents & mask_in) != 0) {
      hstcpsvr_conn_ptr c = new hstcpsvr_conn();
      if (!c)
        return -1;
      c->dbctx = dbctx;
      c->nonblocking = true;
      c->readsize = cshared.readsize;
      c->accept(cshared);
      if (c->fd.get() >= 0) {
        if (fcntl(c->fd.get(), F_SETFL, O_NONBLOCK) != 0) {
          fatal_abort("F_SETFL O_NONBLOCK");
        }
        c->nb_last_io = now;
        if (conns.push_back_ptr(c))
          delete c;
      } else {
        delete c;
        /* errno == 11 (EAGAIN) is not a fatal error. */
        DENA_VERBOSE(100, fprintf(stderr,
          "accept failed: errno=%d (not fatal)\n", errno));
      }
    }
  }
  DENA_VERBOSE(30, fprintf(stderr, "nb: %p nfds=%zu cns=%zu\n", this, nfds,
    conns.size()));
  if (conns.empty()) {
    dbctx->close_tables_if(false);
  } else {
    dbctx->semi_close_tables();
  }
  dbctx->set_statistics(conns.size(), 0);
  return 0;
}

#ifdef __linux__
int
hstcpsvr_worker::run_one_ep()
{
  epoll_event *const events = &events_vec[0];
  const size_t num_events = events_vec.size();
  const time_t now = time(0);
  size_t in_count = 0, out_count = 0, accept_count = 0;
  int nfds = epoll_wait(epoll_fd.get(), events, num_events, 1000);
  /* READ/ACCEPT */
  dbctx->set_statistics(conns.size(), nfds);
  for (int i = 0; i < nfds; ++i) {
    epoll_event& ev = events[i];
    if ((ev.events & EPOLLIN) == 0) {
      continue;
    }
    hstcpsvr_conn *const conn = static_cast<hstcpsvr_conn *>(ev.data.ptr);
    if (conn == 0) {
      /* listener */
      ++accept_count;
      DBG_EP(fprintf(stderr, "IN listener\n"));
      hstcpsvr_conn_ptr c = new hstcpsvr_conn();
      c->dbctx = dbctx;
      c->nonblocking = true;
      c->readsize = cshared.readsize;
      c->accept(cshared);
      if (c->fd.get() >= 0) {
        if (fcntl(c->fd.get(), F_SETFL, O_NONBLOCK) != 0) {
          fatal_abort("F_SETFL O_NONBLOCK");
        }
        epoll_event cev = { };
        cev.events = EPOLLIN | EPOLLOUT | EPOLLET;
        cev.data.ptr = c;
        c->nb_last_io = now;
        const int fd = c->fd.get();
        if (conns.push_back_ptr(c))
          delete c;
        if (epoll_ctl(epoll_fd.get(), EPOLL_CTL_ADD, fd, &cev) != 0) {
          fatal_abort("epoll_ctl EPOLL_CTL_ADD");
        }
      } else {
        delete c;
        DENA_VERBOSE(100, fprintf(stderr,
          "accept failed: errno=%d (not fatal)\n", errno));
      }
    } else {
      /* client connection */
      ++in_count;
      DBG_EP(fprintf(stderr, "IN client\n"));
      bool more_data = false;
      while (conn->read_more(&more_data)) {
        DBG_EP(fprintf(stderr, "IN client read_more\n"));
        conn->nb_last_io = now;
        if (!more_data) {
          break;
        }
      }
    }
  }
  /* EXECUTE */
#ifdef HA_CAN_BULK_ACCESS
  dbctx->bulk_exec_prepare(this);
#endif
  for (int i = 0; i < nfds; ++i) {
    epoll_event& ev = events[i];
    hstcpsvr_conn *const conn = static_cast<hstcpsvr_conn *>(ev.data.ptr);
    if ((ev.events & EPOLLIN) == 0 || conn == 0 ||
      conn->cstate.readbuf.size() == 0) {
      continue;
    }
    const char ch = conn->cstate.readbuf.begin()[0];
    if (ch == 'Q') {
      vshared.shutdown = 1;
    } else if (ch == '/') {
      conn->cstate.readbuf.clear();
      conn->cstate.find_nl_pos = 0;
      conn->cstate.writebuf.clear();
      conn->read_finished = true;
      conn->write_finished = true;
    } else {
      /* conn is added to queue */
      dbctx->add_bulk_exec_conn(conn);
      if (!dbctx->reopen_tables(*conn, conn->cstate.prep_stmts))
      {
        execute_lines(*conn, (size_t) i);
      }
    }
  }
#ifdef HA_CAN_BULK_ACCESS
  dbctx->bulk_exec_finish();
#endif
  /* COMMIT */
  if (!dbctx->skip_unlock_tables())
    dbctx->unlock_tables_if();
  const bool commit_error = dbctx->get_commit_error();
  dbctx->clear_error();
  /* WRITE */
  for (int i = 0; i < nfds; ++i) {
    epoll_event& ev = events[i];
    hstcpsvr_conn *const conn = static_cast<hstcpsvr_conn *>(ev.data.ptr);
    if (commit_error && conn != 0) {
      conn->reset();
      continue;
    }
    if ((ev.events & EPOLLOUT) == 0) {
      continue;
    }
    ++out_count;
    if (conn == 0) {
      /* listener */
      DBG_EP(fprintf(stderr, "OUT listener\n"));
    } else {
      /* client connection */
      DBG_EP(fprintf(stderr, "OUT client\n"));
      bool more_data = false;
      while (conn->write_more(&more_data)) {
        DBG_EP(fprintf(stderr, "OUT client write_more\n"));
        conn->nb_last_io = now;
        if (!more_data) {
          break;
        }
      }
    }
  }
  /* CLOSE */
  for (int i = 0; i < nfds; ++i) {
    epoll_event& ev = events[i];
    hstcpsvr_conn *const conn = static_cast<hstcpsvr_conn *>(ev.data.ptr);
    if (conn != 0 && conn->ok_to_close()) {
      DBG_EP(fprintf(stderr, "CLOSE close\n"));
      conns.erase_ptr(conn);
    }
  }
  /* TIMEOUT & cleanup */
  if (last_check_time + 10 < now) {
    for (hstcpsvr_conn_ptr *i = conns.begin();
      i < conns.end(); ) {
      DENA_VERBOSE(30, fprintf(stderr, "i=%p end=%p\n", i, conns.end()));
      hstcpsvr_conn_ptr *icur = i;
      ++i;
      if (cshared.sockargs.timeout != 0 &&
        (*icur)->nb_last_io + cshared.sockargs.timeout < now) {
        conns.erase_ptr((*icur));
      }
    }
    last_check_time = now;
    DENA_VERBOSE(25, fprintf(stderr, "ep: %p nfds=%d cns=%zu\n", this, nfds,
      conns.size()));
  }
  DENA_VERBOSE(30, fprintf(stderr, "%p in=%zu out=%zu ac=%zu, cns=%zu\n",
    this, in_count, out_count, accept_count, conns.size()));
  if (conns.empty()) {
    dbctx->close_tables_if(false);
  } else {
    dbctx->semi_close_tables();
  }
  /* STATISTICS */
  const size_t num_conns = conns.size();
  dbctx->set_statistics(num_conns, 0);
  /* ENABLE/DISABLE ACCEPT */
  if (accept_balance != 0) {
    cshared.thread_num_conns[worker_id] = num_conns;
    size_t total_num_conns = 0;
    for (long i = 0; i < cshared.num_threads; ++i) {
      total_num_conns += cshared.thread_num_conns[i];
    }
    bool e_acc = false;
    if (num_conns < 10 ||
      total_num_conns * 2 > num_conns * cshared.num_threads) {
      e_acc = true;
    }
    epoll_event ev = { };
    ev.events = EPOLLIN;
    ev.data.ptr = 0;
    if (e_acc == accept_enabled) {
    } else if (e_acc) {
      if (epoll_ctl(epoll_fd.get(), EPOLL_CTL_ADD, cshared.listen_fd.get(),
        &ev) != 0) {
        fatal_abort("epoll_ctl EPOLL_CTL_ADD");
      }
    } else {
      if (epoll_ctl(epoll_fd.get(), EPOLL_CTL_DEL, cshared.listen_fd.get(),
        &ev) != 0) {
        fatal_abort("epoll_ctl EPOLL_CTL_ADD");
      }
    }
    accept_enabled = e_acc;
  }
  return 0;
}
#endif 

void
hstcpsvr_worker::execute_lines(hstcpsvr_conn& conn, size_t conn_pos)
{
  DBG_MULTI(int cnt = 0);
  dbconnstate& cstate = conn.cstate;
  char *buf_end = cstate.readbuf.end();
  char *line_begin = cstate.readbuf.begin();
#ifdef HA_CAN_BULK_ACCESS
  if (dbctx->get_bulk_exec_phase() == 1) {
    cstate.line_begin = NULL;
  }
#endif
  dbctx->general_log(line_begin, buf_end - line_begin, conn_pos);
  char *find_pos = line_begin + cstate.find_nl_pos;
  while (true) {
    char *const nl = memchr_char(find_pos, '\n', buf_end - find_pos);
    if (nl == 0) {
      break;
    }
    char *const lf = (line_begin != nl && nl[-1] == '\r') ? nl - 1 : nl;
    DBG_MULTI(cnt++);
    dbctx->set_start_time();
    execute_line(line_begin, lf, conn);
    dbctx->slow_log(line_begin, nl - line_begin);
    find_pos = line_begin = nl + 1;
  }
  DBG_MULTI(fprintf(stderr, "cnt=%d\n", cnt));
#ifdef HA_CAN_BULK_ACCESS
  if (dbctx->get_bulk_exec_phase() == 1) {
    cstate.line_begin = line_begin;
    return;
  }
#endif
  cstate.readbuf.erase_front(line_begin - cstate.readbuf.begin());
  cstate.find_nl_pos = cstate.readbuf.size();
}

void
hstcpsvr_worker::execute_line(char *start, char *finish, hstcpsvr_conn& conn)
{
#ifdef HA_CAN_BULK_ACCESS
  conn.add_bulk_req_req();
#endif
  /* safe to modify, safe to dereference 'finish' */
  char *const cmd_begin = start;
  read_token(start, finish);
  char *const cmd_end = start;
  skip_one(start, finish);
  if (cmd_begin == cmd_end) {
    return conn.dbcb_resp_short(2, "cmd");
  }
  if (cmd_begin + 1 == cmd_end) {
    if (cmd_begin[0] == 'P') {
      if (cshared.require_auth && !conn.authorized) {
        return conn.dbcb_resp_short(3, "unauth");
      }
      return do_open_index(start, finish, conn);
    }
    if (cmd_begin[0] == 'A') {
      return do_authorization(start, finish, conn);
    }
  }
  if (cmd_begin[0] >= '0' && cmd_begin[0] <= '9') {
    if (cshared.require_auth && !conn.authorized) {
      return conn.dbcb_resp_short(3, "unauth");
    }
#ifdef HA_CAN_BULK_ACCESS
    if (conn.get_bulk_req_phase() == 1)
    {
      req_hld *req = conn.get_bulk_req();
      req->cmd_begin = cmd_begin;
      req->cmd_end = cmd_end;
      req->start = start;
      req->finish = finish;
    }
#endif
    return do_exec_on_index(cmd_begin, cmd_end, start, finish, conn);
  }
  return conn.dbcb_resp_short(2, "cmd");
}

void
hstcpsvr_worker::do_open_index(char *start, char *finish, hstcpsvr_conn& conn)
{
  const size_t pst_id = read_ui32(start, finish);
  skip_one(start, finish);
  /* dbname */
  char *const dbname_begin = start;
  read_token(start, finish);
  char *const dbname_end = start;
  skip_one(start, finish);
  /* tblname */
  char *const tblname_begin = start;
  read_token(start, finish);
  char *const tblname_end = start;
  skip_one(start, finish);
  /* idxname */
  char *const idxname_begin = start;
  read_token(start, finish);
  char *const idxname_end = start;
  skip_one(start, finish);
  /* retfields */
  char *const retflds_begin = start;
  read_token(start, finish);
  char *const retflds_end = start;
  skip_one(start, finish);
  /* filfields */
  char *const filflds_begin = start;
  read_token(start, finish);
  char *const filflds_end = start;
  dbname_end[0] = 0;
  tblname_end[0] = 0;
  idxname_end[0] = 0;
  retflds_end[0] = 0;
  filflds_end[0] = 0;
  cmd_open_args args;
  args.pst_id = pst_id;
  args.dbn = dbname_begin;
  args.tbl = tblname_begin;
  args.idx = idxname_begin;
  args.retflds = retflds_begin;
  args.filflds = filflds_begin;
  return dbctx->cmd_open(conn, args);
}

void
hstcpsvr_worker::do_exec_on_index(char *cmd_begin, char *cmd_end, char *start,
  char *finish, hstcpsvr_conn& conn)
{
  cmd_exec_args args;
  DBUG_ENTER("hstcpsvr_worker::do_exec_on_index");
  DBUG_PRINT("info",("handlersocket this=%p", this));
  DBUG_PRINT("info",("handlersocket conn=%p", &conn));
  args.start = cmd_begin;
  args.finish = finish;
  const size_t pst_id = read_ui32(cmd_begin, cmd_end);
  DBUG_PRINT("info",("handlersocket pst_id=%zu", pst_id));
  if (pst_id >= conn.cstate.prep_stmts.size()) {
    conn.dbcb_resp_short(2, "stmtnum");
    DBUG_VOID_RETURN;
  }
  args.pst = &conn.cstate.prep_stmts[pst_id];
  char *const op_begin = start;
  read_token(start, finish);
  char *const op_end = start;
  args.op = string_ref(op_begin, op_end);
  skip_one(start, finish);
  const uint32 fldnum = read_ui32(start, finish);
  string_ref *const flds = DENA_ALLOCA_ALLOCATE(string_ref, fldnum);
  auto_alloca_free_string_ref flds_autofree(flds);
  args.kvals = flds;
  args.kvalslen = fldnum;
  for (size_t i = 0; i < fldnum; ++i) {
    skip_one(start, finish);
    char *const f_begin = start;
    read_token(start, finish);
    char *const f_end = start;
    if (is_null_expression(f_begin, f_end)) {
      /* null */
      flds[i] = string_ref();
    } else {
      /* non-null */
      char *wp = f_begin;
      unescape_string(wp, f_begin, f_end);
      flds[i] = string_ref(f_begin, wp - f_begin);
    }
  }
  skip_one(start, finish);
  args.limit = read_ui32(start, finish);
  skip_one(start, finish);
  args.skip = read_ui32(start, finish);
  if (start == finish) {
    /* simple query */
    dbctx->cmd_exec(conn, args);
    DBUG_VOID_RETURN;
  }
  /* has more options */
  skip_one(start, finish);
  /* in-clause */
  if (start[0] == '@') {
    read_token(start, finish); /* '@' */
    skip_one(start, finish);
    args.invalues_keypart = read_ui32(start, finish);
    skip_one(start, finish);
    args.invalueslen = read_ui32(start, finish);
    if (args.invalueslen <= 0) {
      conn.dbcb_resp_short(2, "invalueslen");
      DBUG_VOID_RETURN;
    }
    if (invalues_work.size() < args.invalueslen) {
      invalues_work.resize(args.invalueslen);
    }
    args.invalues = &invalues_work[0];
    for (uint32_t i = 0; i < args.invalueslen; ++i) {
      skip_one(start, finish);
      char *const invalue_begin = start;
      read_token(start, finish);
      char *const invalue_end = start;
      char *wp = invalue_begin;
      unescape_string(wp, invalue_begin, invalue_end);
      invalues_work[i] = string_ref(invalue_begin, wp - invalue_begin);
    }
    skip_one(start, finish);
  }
  if (start == finish) {
    /* no more options */
    dbctx->cmd_exec(conn, args);
    DBUG_VOID_RETURN;
  }
  /* filters */
  size_t filters_count = 0;
  while (start != finish && (start[0] == 'W' || start[0] == 'F')) {
    char *const filter_type_begin = start;
    read_token(start, finish);
    char *const filter_type_end = start;
    skip_one(start, finish);
    char *const filter_op_begin = start;
    read_token(start, finish);
    char *const filter_op_end = start;
    skip_one(start, finish);
    const uint32 ff_offset = read_ui32(start, finish);
    skip_one(start, finish);
    char *const filter_val_begin = start;
    read_token(start, finish);
    char *const filter_val_end = start;
    skip_one(start, finish);
    if (filters_work.size() <= filters_count) {
      filters_work.resize(filters_count + 1);
    }
    record_filter& fi = filters_work[filters_count];
    if (filter_type_end != filter_type_begin + 1) {
      conn.dbcb_resp_short(2, "filtertype");
      DBUG_VOID_RETURN;
    }
    fi.filter_type = (filter_type_begin[0] == 'W')
      ? record_filter_type_break : record_filter_type_skip;
    const uint32 num_filflds = args.pst->get_filter_fields().size();
    if (ff_offset >= num_filflds) {
      conn.dbcb_resp_short(2, "filterfld");
      DBUG_VOID_RETURN;
    }
    fi.op = string_ref(filter_op_begin, filter_op_end);
    fi.ff_offset = ff_offset;
    if (is_null_expression(filter_val_begin, filter_val_end)) {
      /* null */
      fi.val = string_ref();
    } else {
      /* non-null */
      char *wp = filter_val_begin;
      unescape_string(wp, filter_val_begin, filter_val_end);
      fi.val = string_ref(filter_val_begin, wp - filter_val_begin);
    }
    ++filters_count;
  }
  if (filters_count > 0) {
    if (filters_work.size() <= filters_count) {
      filters_work.resize(filters_count + 1);
    }
    filters_work[filters_count].op = string_ref(); /* sentinel */
    args.filters = &filters_work[0];
  } else {
    args.filters = 0;
  }
  if (start == finish) {
    /* no modops */
    dbctx->cmd_exec(conn, args);
    DBUG_VOID_RETURN;
  }
  /* has modops */
  char *const mod_op_begin = start;
  read_token(start, finish);
  char *const mod_op_end = start;
  args.mod_op = string_ref(mod_op_begin, mod_op_end);
  const size_t num_uvals = args.pst->get_ret_fields().size();
  string_ref *const uflds = DENA_ALLOCA_ALLOCATE(string_ref, num_uvals);
  auto_alloca_free_string_ref uflds_autofree(uflds);
  for (size_t i = 0; i < num_uvals; ++i) {
    skip_one(start, finish);
    char *const f_begin = start;
    read_token(start, finish);
    char *const f_end = start;
    if (is_null_expression(f_begin, f_end)) {
      /* null */
      uflds[i] = string_ref();
    } else {
      /* non-null */
      char *wp = f_begin;
      unescape_string(wp, f_begin, f_end);
      uflds[i] = string_ref(f_begin, wp - f_begin);
    }
  }
  args.uvals = uflds;
  dbctx->cmd_exec(conn, args);
  DBUG_VOID_RETURN;
}

void
hstcpsvr_worker::do_authorization(char *start, char *finish,
  hstcpsvr_conn& conn)
{
  /* auth type */
  char *const authtype_begin = start;
  read_token(start, finish);
  char *const authtype_end = start;
  const size_t authtype_len = authtype_end - authtype_begin;
  skip_one(start, finish);
  /* key */
  char *const key_begin = start;
  read_token(start, finish);
  char *const key_end = start;
  const size_t key_len = key_end - key_begin;
  authtype_end[0] = 0;
  key_end[0] = 0;
  char *wp = key_begin;
  unescape_string(wp, key_begin, key_end);
  if (authtype_len != 1 || authtype_begin[0] != '1') {
    return conn.dbcb_resp_short(3, "authtype");
  }
  if (cshared.plain_secret.length() == key_len &&
    memcmp(cshared.plain_secret.ptr(), key_begin, key_len) == 0) {
    conn.authorized = true;
  } else {
    conn.authorized = false;
  }
  if (!conn.authorized) {
    return conn.dbcb_resp_short(3, "unauth");
  } else {
    return conn.dbcb_resp_short(0, "");
  }
}

hstcpsvr_worker_ptr
hstcpsvr_worker_i::create(const hstcpsvr_worker_arg& arg)
{
  return hstcpsvr_worker_ptr(new hstcpsvr_worker(arg));
}

};

