
// vim:sw=2:ai

/*
 * Copyright (C) 2010-2011 DeNA Co.,Ltd.. All rights reserved.
 * Copyright (C) 2011 Kentoku SHIBA
 * See COPYRIGHT.txt for details.
 */

#ifndef DENA_MYSQL_INCL_HPP
#define DENA_MYSQL_INCL_HPP

#ifndef HAVE_CONFIG_H
#define HAVE_CONFIG_H
#endif

#define MYSQL_SERVER 1

#include <mysql_version.h>

#if MYSQL_VERSION_ID >= 50505
// FIXME FIXME FIXME
#define safeFree(X) my_free(X)
#define current_stmt_binlog_row_based  is_current_stmt_binlog_format_row
#define clear_current_stmt_binlog_row_based  clear_current_stmt_binlog_format_row

#else
#include "mysql_priv.h"
#endif

#endif

