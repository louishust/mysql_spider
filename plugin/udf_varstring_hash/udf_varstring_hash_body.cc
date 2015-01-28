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
#include "mysql_version.h"
#include "sql_priv.h"
#include "probes_mysql.h"
#include "sql_class.h"
#include "udf_varstring_hash_body.h"

long long udf_varstring_hash_body(
  UDF_INIT *initid,
  UDF_ARGS *args,
  char *is_null,
  char *error
) {
  ulong nr = 1, nr2 = 4;
  uchar *arg_str = (uchar *) args->args[0];
  uint div_num, arg_len;
  THD *thd = current_thd;

  DBUG_ENTER("udf_varstring_hash_body");
  if (!arg_str)
  {
    nr ^= (nr << 1) | 1;
  } else {
    arg_len = args->lengths[0];
    CHARSET_INFO *cs = thd->variables.character_set_client;
    cs->coll->hash_sort(cs, arg_str, arg_len, &nr, &nr2);
  }

  div_num = (!args->args[1] || *((ulonglong *) args->args[1]) < 1 ? 1 :
    *((ulonglong *) args->args[1]));

  DBUG_RETURN(nr % div_num);
}

my_bool udf_varstring_hash_init_body(
  UDF_INIT *initid,
  UDF_ARGS *args,
  char *message
) {
  DBUG_ENTER("udf_varstring_hash_init_body");
  if (args->arg_count != 2)
  {
    strcpy(message, "udf_varstring_hash() requires 2 arguments");
    goto error;
  }
  if (
    args->arg_type[0] != STRING_RESULT
  ) {
    strcpy(message, "udf_varstring_hash() requires string arguments for param 1");
    goto error;
  }
  if (
    args->arg_type[1] != INT_RESULT
  ) {
    strcpy(message, "udf_varstring_hash() requires int arguments for param 2");
    goto error;
  }
  DBUG_RETURN(FALSE);

error:
  DBUG_RETURN(TRUE);
}

void udf_varstring_hash_deinit_body(
  UDF_INIT *initid
) {
  DBUG_ENTER("udf_varstring_hash_deinit_body");
  DBUG_VOID_RETURN;
}
