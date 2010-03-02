
#include "mysql_priv.h"

HASH global_table_stats;

/*
  Incremented when global_table_stats is flushed. Allows code to cache pointers to
  hashed  elements and avoid hash searches.
*/
static int global_table_stats_version= 0;

/*
  Update global table statistics for this table and optionally tables
  linked via TABLE::next.

  SYNOPSIS
    update_table_stats()
      tablep - the table for which global table stats are updated
      follow_next - when TRUE, update global stats for tables linked
                    via TABLE::next
 */
void update_table_stats(TABLE *tablep, bool follow_next)
{
  for (; tablep; tablep= tablep->next)
  {
    if (tablep->file)
      tablep->file->update_global_table_stats();

    if (!follow_next)
      return;
  }
}

/*
  Return the global TABLE_STATS object for a table. Use the cached object
  when it is valid and update the cache fields when it is not.

  SYNOPSIS
    get_table_stats()
    table          in: table for which an object is returned
    type_of_db     in: storage engine type
    cached_stats   in/out: on entry may reference a previously allocated
                   object. On return references the object to use.
    cached_version in/out: value of global_table_stats_version at
                   which cached_stats is valid.

  NOTES
    The object referenced by cached_stats may be reallocated here because
    the old object was free'd when FLUSH TABLE STATUS was run. In that case,
    global_table_stats_version is incremented.

    LOCK_global_table_stats must be held when this is called.

  RETURN VALUE
    0 on success
*/
int
get_table_stats(TABLE *table, handlerton *engine_type,
                TABLE_STATS **cached_stats, int *cached_version)
{
  TABLE_STATS* table_stats;

  safe_mutex_assert_owner(&LOCK_global_table_stats);

  if (*cached_stats && *cached_version == global_table_stats_version)
  {
    // Previously allocated object still valid.
    return 0;
  }

  if (!table->s || !table->s->db.str || !table->s->table_name.str ||
      !table->s->table_cache_key.str || !table->s->table_cache_key.length)
  {
    sql_print_error("No key for table stats.");
    return -1;
  }

  // Gets or creates the TABLE_STATS object for this table.
  if (!(table_stats= (TABLE_STATS*)hash_search(&global_table_stats,
                                               (uchar*)table->s->table_cache_key.str,
                                               table->s->table_cache_key.length)))
  {
    if (!(table_stats= ((TABLE_STATS*)my_malloc(sizeof(TABLE_STATS),
                                                MYF(MY_WME)))))
    {
      sql_print_error("Cannot allocate memory for TABLE_STATS.");
      return -1;
    }

    DBUG_ASSERT(table->s->table_cache_key.length <= (NAME_LEN * 2 + 2));
    memcpy(table_stats->hash_key, table->s->table_cache_key.str, table->s->table_cache_key.length);
    table_stats->hash_key_len= table->s->table_cache_key.length;

    if (snprintf(table_stats->db, NAME_LEN+1, "%s", table->s->db.str) < 0 ||
        snprintf(table_stats->table, NAME_LEN+1, "%s",
                 table->s->table_name.str) < 0 ||
        snprintf(table_stats->db_table, NAME_LEN * 2 + 2, "%s.%s",
                 table->s->db.str, table->s->table_name.str) < 0)
    {
      sql_print_error("Cannot generate name for table stats.");
      my_free((char*)table_stats, 0);
      return -1;
    }
    table_stats->db_table_len= strlen(table_stats->db_table);
    table_stats->rows_inserted= 0;
    table_stats->rows_updated= 0;
    table_stats->rows_deleted= 0;
    table_stats->rows_read= 0;
    table_stats->rows_requested= 0;
    table_stats->engine_type= engine_type;

    if (my_hash_insert(&global_table_stats, (uchar*)table_stats))
    {
      // Out of memory.
      sql_print_error("Inserting table stats failed.");
      my_free((char*)table_stats, 0);
      return -1;
    }
  }
  *cached_stats= table_stats;
  *cached_version= global_table_stats_version;

  return 0;
}
  
extern "C" uchar *get_key_table_stats(TABLE_STATS *table_stats, size_t *length,
                                      my_bool not_used __attribute__((unused)))
{
  *length = table_stats->hash_key_len;
  return (uchar*)table_stats->hash_key;
}

extern "C" void free_table_stats(TABLE_STATS* table_stats)
{
  my_free((char*)table_stats, MYF(0));
}

void init_global_table_stats(void)
{
  if (hash_init(&global_table_stats, system_charset_info, max_connections,
                0, 0, (hash_get_key)get_key_table_stats,
                (hash_free_key)free_table_stats, 0)) {
    sql_print_error("Initializing global_table_stats failed.");
    unireg_abort(1);
  }
  global_table_stats_version++;
}

void free_global_table_stats(void)
{
  hash_free(&global_table_stats);
  global_table_stats_version++;
}

ST_FIELD_INFO table_stats_fields_info[]=
{
  {"TABLE_SCHEMA", NAME_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"TABLE_NAME", NAME_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"TABLE_ENGINE", NAME_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"ROWS_INSERTED", MY_INT64_NUM_DECIMAL_DIGITS , MYSQL_TYPE_LONG, 0, 0, 0},
  {"ROWS_UPDATED", MY_INT64_NUM_DECIMAL_DIGITS , MYSQL_TYPE_LONG, 0, 0, 0},
  {"ROWS_DELETED", MY_INT64_NUM_DECIMAL_DIGITS , MYSQL_TYPE_LONG, 0, 0, 0},
  {"ROWS_READ", MY_INT64_NUM_DECIMAL_DIGITS , MYSQL_TYPE_LONG, 0, 0, 0},
  {"ROWS_REQUESTED", MY_INT64_NUM_DECIMAL_DIGITS , MYSQL_TYPE_LONG, 0, 0, 0},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0}
};

// fill_query_profile_statistics_info
int fill_table_stats(THD *thd, TABLE_LIST *tables, COND *cond)
{
  DBUG_ENTER("fill_table_stats");
  TABLE* table= tables->table;

  pthread_mutex_lock(&LOCK_global_table_stats);

  for (unsigned i = 0; i < global_table_stats.records; ++i) {
    TABLE_STATS *table_stats =
      (TABLE_STATS*)hash_element(&global_table_stats, i);

    restore_record(table, s->default_values);
    table->field[0]->store(table_stats->db, strlen(table_stats->db),
                           system_charset_info);
    table->field[1]->store(table_stats->table, strlen(table_stats->table),
                           system_charset_info);

    // TODO -- this can be optimized in the future
    const char* engine= ha_resolve_storage_engine_name(table_stats->engine_type);
    table->field[2]->store(engine, strlen(engine), system_charset_info);

    table->field[3]->store((longlong)table_stats->rows_inserted, TRUE);
    table->field[4]->store((longlong)table_stats->rows_updated, TRUE);
    table->field[5]->store((longlong)table_stats->rows_deleted, TRUE);
    table->field[6]->store((longlong)table_stats->rows_read, TRUE);
    table->field[7]->store((longlong)table_stats->rows_requested, TRUE);

    if (schema_table_store_record(thd, table))
    {
      pthread_mutex_unlock(&LOCK_global_table_stats);
      DBUG_RETURN(-1);
    }
  }
  pthread_mutex_unlock(&LOCK_global_table_stats);

  DBUG_RETURN(0);
}

