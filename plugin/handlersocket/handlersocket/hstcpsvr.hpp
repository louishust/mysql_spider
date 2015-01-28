
// vim:sw=2:ai

/*
 * Copyright (C) 2010-2011 DeNA Co.,Ltd.. All rights reserved.
 * Copyright (C) 2011 Kentoku SHIBA
 * See COPYRIGHT.txt for details.
 */

#ifndef DENA_HSTCPSVR_HPP
#define DENA_HSTCPSVR_HPP

#include "mutex.hpp"
#include "auto_file.hpp"
#include "database.hpp"
#include "config.hpp"
#include "socket.hpp"

namespace dena {

struct hstcpsvr_shared_c {
  config conf;
  long num_threads;
  long nb_conn_per_thread;
  bool for_write_flag;
  bool require_auth;
  String plain_secret;
  int readsize;
  socket_args sockargs;
  auto_file listen_fd;
  database_ptr dbptr;
  volatile uint32 *thread_num_conns; /* 0 .. num_threads-1 */
  hstcpsvr_shared_c() : num_threads(0), nb_conn_per_thread(100),
    for_write_flag(false), require_auth(false), readsize(0),
    thread_num_conns(0) { }
};

struct hstcpsvr_shared_v : public mutex {
  int shutdown;
  hstcpsvr_shared_v() : shutdown(0) { }
};

struct hstcpsvr_i;
typedef hstcpsvr_i *hstcpsvr_ptr;

struct hstcpsvr_i {
  virtual ~hstcpsvr_i() { }
  virtual String *start_listen(String *err) = 0;
  static hstcpsvr_ptr create(const config& conf);
};

};

#endif

