/* Copyright (C) 2011 Kentoku SHIBA

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#define MYSQL_SERVER 1
#include <my_global.h>
#include <mysql.h>
#include "udf_varstring_hash_body.h"

extern "C" {
long long udf_varstring_hash(
  UDF_INIT *initid,
  UDF_ARGS *args,
  char *is_null,
  char *error
) {
  return udf_varstring_hash_body(initid, args, is_null, error);
}

my_bool udf_varstring_hash_init(
  UDF_INIT *initid,
  UDF_ARGS *args,
  char *message
) {
  return udf_varstring_hash_init_body(initid, args, message);
}

void udf_varstring_hash_deinit(
  UDF_INIT *initid
) {
  udf_varstring_hash_deinit_body(initid);
}
}
