
// vim:sw=2:ai

/*
 * Copyright (C) 2010-2011 DeNA Co.,Ltd.. All rights reserved.
 * Copyright (C) 2011 Kentoku SHIBA
 * See COPYRIGHT.txt for details.
 */

#ifndef DENA_HSTCPSVR_WORKER_HPP
#define DENA_HSTCPSVR_WORKER_HPP

#include "hstcpsvr.hpp"

namespace dena {

struct hstcpsvr_worker_i;
struct hstcpsvr_worker_arg;
typedef hstcpsvr_worker_i *hstcpsvr_worker_ptr;

struct hstcpsvr_worker_i {
  virtual ~hstcpsvr_worker_i() { }
  virtual void run() = 0;
  static hstcpsvr_worker_ptr create(const hstcpsvr_worker_arg& arg);
};

struct hstcpsvr_worker_arg {
  const hstcpsvr_shared_c *cshared;
  volatile hstcpsvr_shared_v *vshared;
  long worker_id;
  hstcpsvr_worker_arg() : cshared(0), vshared(0), worker_id(0) { }
};

struct worker_throbj {
  worker_throbj(const hstcpsvr_worker_arg& arg)
    : worker(hstcpsvr_worker_i::create(arg)) { }
  void operator ()() {
    worker->run();
  }
  hstcpsvr_worker_ptr worker;
};
typedef worker_throbj *worker_throbj_ptr;

struct worker_throbj_thread : private noncopyable {
  worker_throbj_thread(const worker_throbj& arg, size_t stack_sz = 256 * 1024)
    : obj(arg), thr(0), need_join(false), stack_size(stack_sz) { }
  ~worker_throbj_thread() {
    join();
    delete obj.worker;
  }
  void start() {
    if (!start_nothrow()) {
      fatal_abort("worker_throbj_thread::start");
    }
  }
  bool start_nothrow() {
    if (need_join) {
      return need_join; /* true */
    }
    void *const arg = this;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
      fatal_abort("pthread_attr_init");
    }
    if (pthread_attr_setstacksize(&attr, stack_size) != 0) {
      fatal_abort("pthread_attr_setstacksize");
    }
    const int r = pthread_create(&thr, &attr, thread_main, arg);
    if (pthread_attr_destroy(&attr) != 0) {
      fatal_abort("pthread_attr_destroy");
    }
    if (r != 0) {
      return need_join; /* false */
    }
    need_join = true;
    return need_join; /* true */
  }
  void join() {
    if (!need_join) {
      return;
    }
    int e = 0;
    if ((e = pthread_join(thr, 0)) != 0) {
      fatal_abort("pthread_join");
    }
    need_join = false;
  }
  worker_throbj& operator *() { return obj; }
  worker_throbj *operator ->() { return &obj; }
 private:
  static void *thread_main(void *arg) {
    worker_throbj_thread *p = static_cast<worker_throbj_thread *>(arg);
    p->obj();
    return 0;
  }
 private:
  worker_throbj obj;
  pthread_t thr;
  bool need_join;
  size_t stack_size;
 public:
  uint32 number_of_list;
};
typedef worker_throbj_thread *worker_throbj_thread_ptr;

struct worker_throbj_thread_ptr_list_wrap {
  worker_throbj_thread_ptr_list_wrap() {
    if (my_init_dynamic_array2(&worker_throbj_thread_ptr_list,
      sizeof(worker_throbj_thread_ptr), NULL, 16, 16))
      worker_throbj_thread_ptr_list_init = FALSE;
    else
      worker_throbj_thread_ptr_list_init = TRUE;
  }
  worker_throbj_thread_ptr *begin() {
    return (worker_throbj_thread_ptr *) worker_throbj_thread_ptr_list.buffer;
  }
  const worker_throbj_thread_ptr *begin() const {
    return (worker_throbj_thread_ptr *) worker_throbj_thread_ptr_list.buffer;
  }
  worker_throbj_thread_ptr *end() {
    return &((worker_throbj_thread_ptr *) worker_throbj_thread_ptr_list.buffer
      )[worker_throbj_thread_ptr_list.elements];
  }
  const worker_throbj_thread_ptr *end() const {
    return &((worker_throbj_thread_ptr *) worker_throbj_thread_ptr_list.buffer
      )[worker_throbj_thread_ptr_list.elements];
  }
  size_t size() {
    return worker_throbj_thread_ptr_list_init ? worker_throbj_thread_ptr_list.
      elements : 0;
  }
  size_t max_size() {
    return worker_throbj_thread_ptr_list_init ?
      worker_throbj_thread_ptr_list.max_element : 0;
  }
  bool empty() {
    return worker_throbj_thread_ptr_list_init ? worker_throbj_thread_ptr_list.
      elements ? FALSE : TRUE : TRUE;
  }
  worker_throbj_thread_ptr front() {
    return ((worker_throbj_thread_ptr *) worker_throbj_thread_ptr_list.buffer
      )[0];
  }
  const worker_throbj_thread_ptr front() const {
    return ((worker_throbj_thread_ptr *) worker_throbj_thread_ptr_list.buffer
      )[0];
  }
  worker_throbj_thread_ptr back() {
    return ((worker_throbj_thread_ptr *) worker_throbj_thread_ptr_list.buffer
      )[worker_throbj_thread_ptr_list.elements - 1];
  }
  const worker_throbj_thread_ptr back() const {
    return ((worker_throbj_thread_ptr *) worker_throbj_thread_ptr_list.buffer
      )[worker_throbj_thread_ptr_list.elements - 1];
  }
  void swap(worker_throbj_thread_ptr_list_wrap& x) {
    if (worker_throbj_thread_ptr_list_init) {
      bool tmp_init = x.worker_throbj_thread_ptr_list_init;
      DYNAMIC_ARRAY tmp_list = x.worker_throbj_thread_ptr_list;
      x.worker_throbj_thread_ptr_list_init =
        worker_throbj_thread_ptr_list_init;
      x.worker_throbj_thread_ptr_list = worker_throbj_thread_ptr_list;
      worker_throbj_thread_ptr_list_init = tmp_init;
      worker_throbj_thread_ptr_list = tmp_list;
    }
  }
  virtual ~worker_throbj_thread_ptr_list_wrap() {
    if (worker_throbj_thread_ptr_list_init) {
      clear();
      delete_dynamic(&worker_throbj_thread_ptr_list);
    }
  }
  bool push_back_ptr(worker_throbj_thread_ptr e) {
    if (worker_throbj_thread_ptr_list_init) {
      e->number_of_list = worker_throbj_thread_ptr_list.elements;
      if (insert_dynamic(&worker_throbj_thread_ptr_list, (uchar*) &e))
        return TRUE;
      return FALSE;
    }
    return TRUE;
  }
  void erase_ptr(worker_throbj_thread_ptr *i) {
    delete_dynamic_element(&worker_throbj_thread_ptr_list,
      (*i)->number_of_list);
    delete (*i);
  }
  worker_throbj_thread_ptr &operator [](size_t n) {
    return ((worker_throbj_thread_ptr *)
      (worker_throbj_thread_ptr_list.buffer +
      worker_throbj_thread_ptr_list.size_of_element * n))[0];
  }
  const worker_throbj_thread_ptr &operator [](size_t n) const {
    return ((worker_throbj_thread_ptr *)
      (worker_throbj_thread_ptr_list.buffer +
      worker_throbj_thread_ptr_list.size_of_element * n))[0];
  }
  void clear() {
    if (worker_throbj_thread_ptr_list_init) {
      uint32 i;
      worker_throbj_thread_ptr ptr;
      for (i = 0; i < worker_throbj_thread_ptr_list.elements; i++) {
        ptr = ((worker_throbj_thread_ptr *)
          (worker_throbj_thread_ptr_list.buffer +
          worker_throbj_thread_ptr_list.size_of_element * i))[0];
        delete ptr;
      }
      worker_throbj_thread_ptr_list.elements = 0;
    }
  }
  bool worker_throbj_thread_ptr_list_init;
  DYNAMIC_ARRAY worker_throbj_thread_ptr_list;
};

struct pollfd_list_wrap {
  pollfd_list_wrap() {
    if (my_init_dynamic_array2(&pollfd_list, sizeof(pollfd),
      NULL, 16, 16))
      pollfd_list_init = FALSE;
    else
      pollfd_list_init = TRUE;
  }
  virtual ~pollfd_list_wrap() {
    if (pollfd_list_init) delete_dynamic(&pollfd_list); }
  void clear() {
    if (pollfd_list_init) pollfd_list.elements = 0; }
  void push_back(pollfd &e) {
    if (pollfd_list_init) insert_dynamic(&pollfd_list, (uchar*) &e);
    return; }
  bool resize(size_t new_size) {
    if (pollfd_list_init) {
        if (pollfd_list.max_element < new_size && allocate_dynamic(
          &pollfd_list, new_size)) return TRUE;
        pollfd_list.elements = new_size; return FALSE; }
    return TRUE; }
  size_t size() {
    return pollfd_list_init ? pollfd_list.elements : 0; }
  bool empty() {
    return pollfd_list_init ? pollfd_list.elements ?
      FALSE : TRUE : TRUE; }
  pollfd &operator [](size_t n) {
    return ((pollfd *) (pollfd_list.buffer +
      pollfd_list.size_of_element * n))[0];
  }
  bool pollfd_list_init;
  DYNAMIC_ARRAY pollfd_list;
};

};

#endif

