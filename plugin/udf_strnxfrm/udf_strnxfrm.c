/* Copyright (C) 2010-2014 Kentoku Shiba

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

#include <mysql_version.h>
#include <my_global.h>
#include <mysql.h>
#include <my_sys.h>
#include <mysqld_error.h>
#include <m_string.h>

typedef struct st_udf_strnxfrm
{
  char          *ptr;
  unsigned long len;
} UDF_CASEDN_STR;

char *udf_strnxfrm(
  UDF_INIT *initid,
  UDF_ARGS *args,
  char *result,
  unsigned long *length,
  char *is_null,
  char *error
) {
  CHARSET_INFO *cs;
  if (!args->args[0]) {
    *is_null = 1;
    return NULL;
  }
  *is_null = 0;

  if (args->arg_type[1] == STRING_RESULT)
  {
    if (
      !args->args[1] ||
      !(cs = get_charset_by_name(args->args[1], MYF(0)))
    ) {
      my_error(ER_UNKNOWN_COLLATION, MYF(0),
        args->args[1] ? args->args[1] : "NULL");
      goto error;
    }
  } else {
    if (
      !args->args[1] ||
      !(cs = get_charset(*((long long *) args->args[1]), MYF(0)))
    ) {
      my_error(ER_UNKNOWN_COLLATION, MYF(0), "");
      goto error;
    }
  }

  UDF_CASEDN_STR *convstr = (UDF_CASEDN_STR *) initid->ptr;
  size_t charlen = cs->cset->numchars(
    cs, args->args[0], args->args[0] + args->lengths[0]);
  *length = cs->coll->strnxfrmlen(cs, charlen * cs->mbmaxlen);
  if (
    convstr->ptr &&
    convstr->len < *length
  ) {
#if MYSQL_VERSION_ID < 50500
    my_free(convstr->ptr, MYF(0));
#else
    my_free(convstr->ptr);
#endif
    convstr->ptr = NULL;
    convstr->len = 0;
  }
  if (!convstr->ptr)
  {
    if (
      !(convstr->ptr = my_malloc(sizeof(char) * (*length + 1),
        MYF(MY_WME)))
    ) {
      my_error(ER_OUTOFMEMORY, MYF(0));
      goto error;
    }
    convstr->len = *length;
  }
  cs->coll->strnxfrm(cs, (uchar *) convstr->ptr, *length,
    (const uchar *) args->args[0], args->lengths[0]);
  return convstr->ptr;

error:
  *error = 1;
  return NULL;
}

my_bool udf_strnxfrm_init(
  UDF_INIT *initid,
  UDF_ARGS *args,
  char *message
) {
  initid->ptr = NULL;
  if (args->arg_count != 2)
  {
    strcpy(message, "udf_strnxfrm() requires 2 arguments");
    goto error;
  }
  if (
    args->arg_type[1] != STRING_RESULT &&
    args->arg_type[1] != INT_RESULT
  ) {
    strcpy(message, "udf_strnxfrm() requires string or int for 2nd arguments");
    goto error;
  }
  args->arg_type[0] = STRING_RESULT;
  initid->maybe_null = 1;

  if (!(initid->ptr =
    my_malloc(sizeof(UDF_CASEDN_STR), MYF(MY_WME | MY_ZEROFILL))))
  {
    strcpy(message, "udf_strnxfrm() out of memory");
    goto error;
  }

  return FALSE;

error:
  return TRUE;
}

void udf_strnxfrm_deinit(
  UDF_INIT *initid
) {
  UDF_CASEDN_STR *convstr = (UDF_CASEDN_STR *) initid->ptr;
  if (convstr)
  {
    if (convstr->ptr)
    {
#if MYSQL_VERSION_ID < 50500
      my_free(convstr->ptr, MYF(0));
#else
      my_free(convstr->ptr);
#endif
    }
#if MYSQL_VERSION_ID < 50500
    my_free(convstr, MYF(0));
#else
    my_free(convstr);
#endif
  }
}
