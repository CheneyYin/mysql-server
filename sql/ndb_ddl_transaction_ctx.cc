/*
   Copyright (c) 2019, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

// Implements the interface defined in
#include "sql/ndb_ddl_transaction_ctx.h"

#include "sql/ndb_dd.h"
#include "sql/ndb_dd_client.h"
#include "sql/ndb_ddl_definitions.h"
#include "sql/ndb_name_util.h"
#include "sql/ndb_schema_dist.h"
#include "sql/ndb_table_guard.h"
#include "sql/ndb_thd_ndb.h"

void Ndb_DDL_transaction_ctx::log_create_table(const std::string &path_name) {
  log_ddl_stmt(Ndb_DDL_stmt::CREATE_TABLE, path_name);
}

bool Ndb_DDL_transaction_ctx::rollback_create_table(
    const Ndb_DDL_stmt &ddl_stmt) {
  DBUG_TRACE;

  /* extract info from ddl_info */
  const std::vector<std::string> &ddl_info = ddl_stmt.get_info();
  DBUG_ASSERT(ddl_info.size() == 1);
  const char *path_name = ddl_info[0].c_str();
  char db_name[FN_HEADLEN];
  char table_name[FN_HEADLEN];
  ndb_set_dbname(path_name, db_name);
  ndb_set_tabname(path_name, table_name);

  /* Prepare schema client for rollback if required */
  Thd_ndb *thd_ndb = get_thd_ndb(m_thd);
  Ndb_schema_dist_client schema_dist_client(m_thd);
  bool schema_dist_prepared = false;
  if (ddl_stmt.has_been_distributed()) {
    /* The stmt was distributed.
       So rollback should be distributed too.
       Prepare the schema client */
    schema_dist_prepared = schema_dist_client.prepare(db_name, table_name);
    if (!schema_dist_prepared) {
      /* Report the error and just drop it locally */
      thd_ndb->push_warning(
          "Failed to distribute rollback to connected servers.");
    }
  }

  DBUG_PRINT("info",
             ("Rollback : Dropping table '%s.%s'", db_name, table_name));

  /* Drop the table created during this DDL execution */
  Ndb *ndb = thd_ndb->ndb;
  if (drop_table_impl(m_thd, ndb,
                      schema_dist_prepared ? &schema_dist_client : nullptr,
                      path_name, db_name, table_name)) {
    thd_ndb->push_warning("Failed to rollback after CREATE TABLE failure.");
    return false;
  }

  return true;
}

void Ndb_DDL_transaction_ctx::log_rename_table(
    const std::string &old_db_name, const std::string &old_table_name,
    const std::string &new_db_name, const std::string &new_table_name,
    const std::string &from, const std::string &to,
    const std::string &orig_sdi) {
  log_ddl_stmt(Ndb_DDL_stmt::RENAME_TABLE, old_db_name, old_table_name,
               new_db_name, new_table_name, from, to, orig_sdi);
}

bool Ndb_DDL_transaction_ctx::rollback_rename_table(
    const Ndb_DDL_stmt &ddl_stmt) {
  DBUG_TRACE;

  /* extract info from ddl_info */
  const std::vector<std::string> &ddl_info = ddl_stmt.get_info();
  DBUG_ASSERT(ddl_info.size() == 7);
  const char *old_db_name = ddl_info[0].c_str();
  const char *old_table_name = ddl_info[1].c_str();
  const char *new_db_name = ddl_info[2].c_str();
  const char *new_table_name = ddl_info[3].c_str();
  const char *from = ddl_info[4].c_str();
  const char *to = ddl_info[5].c_str();
  m_original_sdi_for_rename = ddl_info[6];

  DBUG_PRINT("info",
             ("Rollback : Renaming table '%s.%s' to '%s.%s'", new_db_name,
              new_table_name, old_db_name, old_table_name));

  /* Load the table from NDB */
  Thd_ndb *thd_ndb = get_thd_ndb(m_thd);
  Ndb *ndb = thd_ndb->ndb;
  ndb->setDatabaseName(new_db_name);
  Ndb_table_guard ndbtab_g(ndb->getDictionary(), new_table_name);
  const NdbDictionary::Table *renamed_table;
  if (!(renamed_table = ndbtab_g.get_table())) {
    const NdbError err = ndb->getDictionary()->getNdbError();
    thd_ndb->push_ndb_error_warning(err);
    thd_ndb->push_warning("Failed to rename table during rollback.");
    return false;
  }

  bool new_table_name_is_temp = ndb_name_is_temp(new_table_name);
  bool old_table_name_is_temp = ndb_name_is_temp(old_table_name);

  /* Prepare schema client for rollback if required */
  bool real_rename = false;
  const char *real_rename_db = nullptr;
  const char *real_rename_table = nullptr;
  Ndb_schema_dist_client schema_dist_client(m_thd);
  bool distribute_rollback = false;
  if (ddl_stmt.has_been_distributed()) {
    /* This stmt was distributed. A RENAME Ndb_DDL_stmt gets marked as
       distributed only for the RENAME TABLE .. query. Copy alter won't be
       logged/handled here. */
    DBUG_ASSERT(!old_table_name_is_temp && !new_table_name_is_temp);
    real_rename = true;
    real_rename_db = new_db_name;
    real_rename_table = new_table_name;

    /* Prepare the schema client */
    if (schema_dist_client.prepare_rename(real_rename_db, real_rename_table,
                                          old_db_name, old_table_name)) {
      distribute_rollback = true;
    } else {
      /* Report the error and carry on */
      thd_ndb->push_warning(
          "Failed to distribute rollback to connected servers.");
    }
  }

  /* Decide whether the events have to be dropped and/or created. The new_name
     is the source and the old_name is the target. So, if the new_name is not
     temp, we would have to drop the events and if the old_name is not temp,
     we would have to create the events. */
  const bool drop_events = !new_table_name_is_temp;
  const bool create_events = !old_table_name_is_temp;

  /* Rename back the table.
     The rename is done from new_name to old_name as this is a rollback. */
  if (rename_table_impl(
          m_thd, ndb, distribute_rollback ? &schema_dist_client : nullptr,
          renamed_table,
          nullptr,  // table_def
          to, from, new_db_name, new_table_name, old_db_name, old_table_name,
          real_rename, real_rename_db, real_rename_table,
          false,  // real_rename_log_on_participants
          drop_events, create_events, false /*commit_alter*/)) {
    thd_ndb->push_warning("Failed to rollback rename table.");
    return false;
  }

  return true;
}

bool Ndb_DDL_transaction_ctx::update_table_id_and_version_in_DD(
    const char *schema_name, const char *table_name, int object_id,
    int object_version) {
  DBUG_TRACE;
  Ndb_dd_client dd_client(m_thd);
  Thd_ndb *thd_ndb = get_thd_ndb(m_thd);

  /* Lock the table exclusively */
  if (!dd_client.mdl_locks_acquire_exclusive(schema_name, table_name)) {
    thd_ndb->push_warning(
        "Failed to acquire exclusive lock on table : '%s.%s' during rollback",
        schema_name, table_name);
    return false;
  }

  /* Update the table with new object id and version */
  if (!dd_client.set_object_id_and_version_in_table(
          schema_name, table_name, object_id, object_version)) {
    thd_ndb->push_warning(
        "Failed to update id and version of table : '%s.%s' during rollback",
        schema_name, table_name);
    return false;
  }

  /* commit the changes */
  dd_client.commit();

  return true;
}

bool Ndb_DDL_transaction_ctx::post_ddl_hook_rename_table(
    const Ndb_DDL_stmt &ddl_stmt) {
  DBUG_TRACE;
  DBUG_ASSERT(m_ddl_status != DDL_IN_PROGRESS);

  if (m_ddl_status == DDL_COMMITED) {
    /* DDL committed. Nothing to do */
    return true;
  }

  Thd_ndb *thd_ndb = get_thd_ndb(m_thd);
  Ndb *ndb = thd_ndb->ndb;

  /* extract info from ddl_info */
  const std::vector<std::string> &ddl_info = ddl_stmt.get_info();
  const char *db_name = ddl_info[0].c_str();
  const char *table_name = ddl_info[1].c_str();

  if (ndb_name_is_temp(table_name)) {
    /* The target table was a temp table. No need to update id and version */
    return true;
  }

  ndb->setDatabaseName(db_name);

  /* Load the table from NDB */
  Ndb_table_guard ndbtab_g(ndb->getDictionary(), table_name);
  const NdbDictionary::Table *ndb_table;
  if (!(ndb_table = ndbtab_g.get_table())) {
    const NdbError err = ndb->getDictionary()->getNdbError();
    thd_ndb->push_ndb_error_warning(err);
    thd_ndb->push_warning("Unable to load table during rollback");
    return false;
  }

  /* Update table id and version */
  if (!update_table_id_and_version_in_DD(db_name, table_name,
                                         ndb_table->getObjectId(),
                                         ndb_table->getObjectVersion())) {
    return false;
  }

  return true;
}

void Ndb_DDL_transaction_ctx::commit() {
  DBUG_TRACE;
  DBUG_ASSERT(m_ddl_status == DDL_IN_PROGRESS);
  /* The schema changes would have been already committed internally to the NDB
     by the respective handler functions that made the change. So just update
     the status of the DDL and make note of the latest stmt on which the
     Server has requested a commit. */
  m_ddl_status = DDL_COMMITED;
  m_latest_committed_stmt = m_executed_ddl_stmts.size();
}

bool Ndb_DDL_transaction_ctx::rollback() {
  DBUG_TRACE;
  DBUG_ASSERT(m_ddl_status == DDL_IN_PROGRESS);

  bool result = true;
  m_ddl_status = DDL_ROLLED_BACK;
  /* Rollback all the uncommitted DDL statements in reverse order */
  for (auto it = m_executed_ddl_stmts.rbegin();
       it != (m_executed_ddl_stmts.rend() - m_latest_committed_stmt); ++it) {
    const Ndb_DDL_stmt &ddl_stmt = *it;
    switch (ddl_stmt.get_ddl_type()) {
      case Ndb_DDL_stmt::CREATE_TABLE:
        result &= rollback_create_table(ddl_stmt);
        break;
      case Ndb_DDL_stmt::RENAME_TABLE:
        result &= rollback_rename_table(ddl_stmt);
        break;
      default:
        result = false;
        DBUG_ASSERT(false);
        break;
    }
  }
  return result;
}

bool Ndb_DDL_transaction_ctx::run_post_ddl_hooks() {
  DBUG_TRACE;
  if (m_ddl_status == DDL_EMPTY) {
    /* Nothing to run */
    return true;
  }
  DBUG_ASSERT(m_ddl_status == DDL_COMMITED || m_ddl_status == DDL_ROLLED_BACK);
  bool result = true;
  for (auto it = m_executed_ddl_stmts.begin(); it != m_executed_ddl_stmts.end();
       ++it) {
    Ndb_DDL_stmt &ddl_stmt = *it;
    switch (ddl_stmt.get_ddl_type()) {
      case Ndb_DDL_stmt::RENAME_TABLE:
        result &= post_ddl_hook_rename_table(ddl_stmt);
        break;
      default:
        break;
    }
  }
  return result;
}
