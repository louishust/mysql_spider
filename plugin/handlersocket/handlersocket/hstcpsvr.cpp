
// vim:sw=2:ai

/*
 * Copyright (C) 2010-2011 DeNA Co.,Ltd.. All rights reserved.
 * Copyright (C) 2011 Kentoku SHIBA
 * See COPYRIGHT.txt for details.
 */

#include "hstcpsvr.hpp"
#include "hstcpsvr_worker.hpp"
#include "thread.hpp"
#include "fatal.hpp"
#include "auto_ptrcontainer.hpp"

#define DBG(x)

namespace dena {

struct hstcpsvr : public hstcpsvr_i, private noncopyable {
  hstcpsvr(const config& c);
  ~hstcpsvr();
  virtual String *start_listen(String *str);
 private:
  hstcpsvr_shared_c cshared;
  volatile hstcpsvr_shared_v vshared;
  typedef worker_throbj_thread worker_thread_type;
  typedef worker_throbj_thread_ptr_list_wrap threads_type;
  threads_type threads;
  uint32_list_wrap thread_num_conns_vec;
 private:
  void stop_workers();
};

namespace {

void
check_nfile(size_t nfile)
{
  struct rlimit rl = { };
  const int r = getrlimit(RLIMIT_NOFILE, &rl);
  if (r != 0) {
    fatal_abort("check_nfile: getrlimit failed");
  }
  if (rl.rlim_cur < static_cast<rlim_t>(nfile + 1000)) {
    fprintf(stderr,
      "[Warning] handlersocket: open_files_limit is too small.\n");
  }
}

};

hstcpsvr::hstcpsvr(const config& c)
  : cshared(), vshared()
{
  vshared.shutdown = 0;
  cshared.conf = c; /* copy */
  if (cshared.conf.compare("port", "")) {
    cshared.conf.replace("port", "9999");
  }
  cshared.num_threads = cshared.conf.get_int("num_threads", 32);
  cshared.sockargs.nonblocking = cshared.conf.get_int("nonblocking", 1);
  cshared.sockargs.use_epoll = cshared.conf.get_int("use_epoll", 1);
  if (cshared.sockargs.use_epoll) {
    cshared.sockargs.nonblocking = 1;
  }
  cshared.readsize = cshared.conf.get_int("readsize", 1);
  cshared.nb_conn_per_thread = cshared.conf.get_int("conn_per_thread", 1024);
  cshared.for_write_flag = cshared.conf.get_int("for_write", 0);
  cshared.plain_secret = cshared.conf.get_str("plain_secret", "");
  cshared.require_auth = cshared.plain_secret.length();
  cshared.sockargs.set(cshared.conf);
  cshared.dbptr = database_i::create(c);
  check_nfile(cshared.num_threads * cshared.nb_conn_per_thread);
  thread_num_conns_vec.resize(cshared.num_threads);
  cshared.thread_num_conns = thread_num_conns_vec.empty()
    ? 0 : &thread_num_conns_vec[0];
}

hstcpsvr::~hstcpsvr()
{
  stop_workers();
  if (cshared.dbptr)
    delete cshared.dbptr;
}

String *
hstcpsvr::start_listen(String *str)
{
  String err;
  str->length(0);
  if (threads.size() != 0) {
    return q_append_str(str, "start_listen: already running");
  }
  if (socket_bind(cshared.listen_fd, cshared.sockargs, err) != 0) {
    q_append_str(str, "bind: ");
    return q_append_str(str, err.c_ptr_safe());
  }
  DENA_VERBOSE(20, fprintf(stderr, "bind done\n"));
  const size_t stack_size = max(
    cshared.conf.get_int("stack_size", 1 * 1024LL * 1024), 8 * 1024LL * 1024);
  for (long i = 0; i < cshared.num_threads; ++i) {
    hstcpsvr_worker_arg arg;
    arg.cshared = &cshared;
    arg.vshared = &vshared;
    arg.worker_id = i;
/*
    worker_throbj throbj(arg);
*/
    worker_throbj_thread_ptr thr =
      new worker_throbj_thread(arg, stack_size);
    threads.push_back_ptr(thr);
  }
  DENA_VERBOSE(20, fprintf(stderr, "threads created\n"));
  for (size_t i = 0; i < threads.size(); ++i) {
    threads[i]->start();
  }
  DENA_VERBOSE(20, fprintf(stderr, "threads started\n"));
  return str;
}

void
hstcpsvr::stop_workers()
{
  vshared.shutdown = 1;
  for (size_t i = 0; i < threads.size(); ++i) {
    threads[i]->join();
  }
  threads.clear();
}

hstcpsvr_ptr
hstcpsvr_i::create(const config& conf)
{
  return new hstcpsvr(conf);
}

};

