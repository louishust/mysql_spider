# Copyright (C) 2011-2014 Kentoku Shiba
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

MYSQL_ADD_PLUGIN(udf_varstring_hash udf_varstring_hash.cc udf_varstring_hash_body.cc udf_varstring_hash.def
  MODULE_ONLY MODULE_OUTPUT_NAME "udf_varstring_hash")
