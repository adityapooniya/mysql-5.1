
#include "mysql_priv.h"
#include "my_atomic.h"

HASH global_table_stats;
static pthread_mutex_t LOCK_global_table_stats;

/*
  Update global table statistics for this table and optionally tables
  linked via TABLE::next.

  SYNOPSIS
    update_table_stats()
      tablep - the table for which global table stats are updated
      follow_next - when TRUE, update global stats for tables linked
                    via TABLE::next
 */
void update_table_stats(THD *thd, TABLE *tablep, bool follow_next,
                        uint keys_dirtied)
{
  for (; tablep; tablep= tablep->next)
  {
    if (tablep->file)
      tablep->file->update_global_table_stats(thd, keys_dirtied);

    if (!follow_next)
      return;
  }
}

static void
clear_table_stats_counters(TABLE_STATS* table_stats)
{
  int x;

  for (x=0; x < MAX_INDEX_STATS; ++x)
  {
    table_stats->indexes[x].rows_inserted= 0;
    table_stats->indexes[x].rows_updated= 0;
    table_stats->indexes[x].rows_deleted= 0;
    table_stats->indexes[x].rows_read= 0;
    table_stats->indexes[x].rows_requested= 0;
    table_stats->indexes[x].rows_index_first= 0;
    table_stats->indexes[x].rows_index_next= 0;
    my_io_perf_init(&(table_stats->indexes[x].io_perf_read));
    my_io_perf_init(&(table_stats->indexes[x].io_perf_read_blob));
  }

  table_stats->n_lru= 0;
  table_stats->keys_dirtied= 0;
  table_stats->queries_used= 0;
  table_stats->rows_inserted= 0;
  table_stats->rows_updated= 0;
  table_stats->rows_deleted= 0;
  table_stats->rows_read= 0;
  table_stats->rows_requested= 0;
  table_stats->rows_index_first= 0;
  table_stats->rows_index_next= 0;

  my_io_perf_init(&table_stats->io_perf_read);
  my_io_perf_init(&table_stats->io_perf_write);
  my_io_perf_init(&table_stats->io_perf_read_blob);
  my_io_perf_init(&table_stats->io_perf_read_primary);
  my_io_perf_init(&table_stats->io_perf_read_secondary);
  table_stats->index_inserts = 0;
	memset(&table_stats->comp_stat, 0, sizeof(table_stats->comp_stat));
}

/*
  Initialize the index names in table_stats->indexes

  SYNOPSIS
    set_index_stats_names
    table_stats - object to initialize

  RETURN VALUE
    0 on success, !0 on failure

  Stats are stored for at most MAX_INDEX_KEYS and when there are more than
  (MAX_INDEX_KEYS-1) indexes then use the last entry for the extra indexes
  which gets the name "STATS_OVERFLOW".
*/
static int
set_index_stats_names(TABLE_STATS *table_stats, TABLE *table)
{
  uint x;

  table_stats->num_indexes= min(table->s->keys, MAX_INDEX_STATS);

  for (x=0; x < table_stats->num_indexes; ++x)
  {
    char const *index_name = table->s->key_info[x].name;

    if (x == (MAX_INDEX_STATS - 1) && table->s->keys > MAX_INDEX_STATS)
      index_name = "STATS_OVERFLOW";
 
    if (snprintf(table_stats->indexes[x].name, NAME_LEN+1, "%s",
                 index_name) < 0)
    {
      return -1;
    }
  }

  return 0;
}

static TABLE_STATS*
get_table_stats_by_name(const char *db_name,
                        const char *table_name,
                        const char *engine_name,
                        const char *cache_key,
                        uint cache_key_length,
                        TABLE *tbl)
{
  TABLE_STATS* table_stats;
  char local_cache_key[NAME_LEN * 2 + 2];

  DBUG_ASSERT(db_name && table_name && engine_name);
  DBUG_ASSERT(cache_key_length <= (NAME_LEN * 2 + 2));

  if (!db_name || !table_name || !engine_name)
  {
    sql_print_error("No key for table stats.");
    return NULL;
  }

  if (cache_key_length > (NAME_LEN * 2 + 2))
  {
    sql_print_error("Cache key length too long for table stats.");
    return NULL;
  }

  if (!cache_key)
  {
    size_t db_name_len= strlen(db_name);
    size_t table_name_len= strlen(table_name);

    if (db_name_len > NAME_LEN || table_name_len > NAME_LEN)
    {
      sql_print_error("Db or table name too long for table stats :%s:%s:\n",
                      db_name, table_name);
      return NULL;
    }

    cache_key = local_cache_key;
    cache_key_length = db_name_len + table_name_len + 2;

    strcpy(local_cache_key, db_name);
    strcpy(local_cache_key + db_name_len + 1, table_name);
  }

  pthread_mutex_lock(&LOCK_global_table_stats);

  // Get or create the TABLE_STATS object for this table.
  if (!(table_stats= (TABLE_STATS*)hash_search(&global_table_stats,
                                               (uchar*)cache_key,
                                               cache_key_length)))
  {
    if (!(table_stats= ((TABLE_STATS*)my_malloc(sizeof(TABLE_STATS),
                                                MYF(MY_WME)))))
    {
      sql_print_error("Cannot allocate memory for TABLE_STATS.");
      pthread_mutex_unlock(&LOCK_global_table_stats);
      return NULL;
    }

    memcpy(table_stats->hash_key, cache_key, cache_key_length);
    table_stats->hash_key_len= cache_key_length;

    if (snprintf(table_stats->db, NAME_LEN+1, "%s", db_name) < 0 ||
        snprintf(table_stats->table, NAME_LEN+1, "%s",
                 table_name) < 0)
    {
      sql_print_error("Cannot generate name for table stats.");
      my_free((char*)table_stats, 0);
      pthread_mutex_unlock(&LOCK_global_table_stats);
      return NULL;
    }

    table_stats->num_indexes= 0;
    if (tbl && set_index_stats_names(table_stats, tbl))
    {
      sql_print_error("Cannot generate name for index stats.");
      my_free((char*)table_stats, 0);
      pthread_mutex_unlock(&LOCK_global_table_stats);
      return NULL;
    }

    clear_table_stats_counters(table_stats);
    table_stats->engine_name= engine_name;

    if (my_hash_insert(&global_table_stats, (uchar*)table_stats))
    {
      // Out of memory.
      sql_print_error("Inserting table stats failed.");
      my_free((char*)table_stats, 0);
      pthread_mutex_unlock(&LOCK_global_table_stats);
      return NULL;
    }
  }
  else
  {
    /*
      Keep things in sync after create or drop index. This doesn't notice create
      followed by drop. "reset statistics" will fix that.
    */
    if (tbl && table_stats->num_indexes != min(tbl->s->keys, MAX_INDEX_STATS))
    {
      if (set_index_stats_names(table_stats, tbl))
      {
        sql_print_error("Cannot generate name for index stats.");
        pthread_mutex_unlock(&LOCK_global_table_stats);
        return NULL;
      }
    }
  }

  pthread_mutex_unlock(&LOCK_global_table_stats);

  return table_stats;
}

/*
  Return the global TABLE_STATS object for a table.

  SYNOPSIS
    get_table_stats()
    table          in: table for which an object is returned
    type_of_db     in: storage engine type

  RETURN VALUE
    TABLE_STATS structure for the requested table
    NULL on failure
*/
TABLE_STATS*
get_table_stats(TABLE *table, handlerton *engine_type)
{
  DBUG_ASSERT(table->s);
  const char* engine_name= ha_resolve_storage_engine_name(engine_type);

  if (!table->s)
  {
    sql_print_error("No key for table stats.");
    return NULL;
  }

  return get_table_stats_by_name(table->s->db.str,
                                 table->s->table_name.str,
                                 engine_name,
                                 table->s->table_cache_key.str,
                                 table->s->table_cache_key.length,
                                 table);
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
  pthread_mutex_init(&LOCK_global_table_stats, MY_MUTEX_INIT_FAST);
  if (hash_init(&global_table_stats, system_charset_info, max_connections,
                0, 0, (hash_get_key)get_key_table_stats,
                (hash_free_key)free_table_stats, 0)) {
    sql_print_error("Initializing global_table_stats failed.");
    unireg_abort(1);
  }
}

void free_global_table_stats(void)
{
  hash_free(&global_table_stats);
  pthread_mutex_destroy(&LOCK_global_table_stats);
}

void reset_global_table_stats()
{
  pthread_mutex_lock(&LOCK_global_table_stats);

  for (unsigned i = 0; i < global_table_stats.records; ++i) {
    TABLE_STATS *table_stats =
      (TABLE_STATS*)hash_element(&global_table_stats, i);

    clear_table_stats_counters(table_stats);

    /*
      The next caller to get_table_stats will reset this. It isn't
      done here because the TABLE object is required to determine
      the index names. This can be called after drop/add index is
      done where the table has the same number of indexes but
      different index names after the DDL.
    */
    table_stats->num_indexes= 0;
  }

  pthread_mutex_unlock(&LOCK_global_table_stats);
}

ST_FIELD_INFO table_stats_fields_info[]=
{
  {"TABLE_SCHEMA", NAME_LEN, MYSQL_TYPE_STRING, 0, 0, 0, SKIP_OPEN_TABLE},
  {"TABLE_NAME", NAME_LEN, MYSQL_TYPE_STRING, 0, 0, 0, SKIP_OPEN_TABLE},
  {"TABLE_ENGINE", NAME_LEN, MYSQL_TYPE_STRING, 0, 0, 0, SKIP_OPEN_TABLE},
  {"ROWS_INSERTED", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"ROWS_UPDATED", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"ROWS_DELETED", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"ROWS_READ", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"ROWS_REQUESTED", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},

  {"COMPRESSED_PAGE_SIZE", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"COMPRESS_PADDING", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"PADDING_SAVINGS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"COMPRESS_OPS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"COMPRESS_OPS_OK", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"COMPRESS_PRIMARY_OPS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"COMPRESS_PRIMARY_OPS_OK", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"COMPRESS_USECS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"COMPRESS_OK_USECS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"COMPRESS_PRIMARY_USECS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"COMPRESS_PRIMARY_OK_USECS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"UNCOMPRESS_OPS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"UNCOMPRESS_USECS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},

  {"ROWS_INDEX_FIRST", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"ROWS_INDEX_NEXT", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},

  {"IO_READ_BYTES", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_REQUESTS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_SVC_USECS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_SVC_USECS_MAX", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_WAIT_USECS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_WAIT_USECS_MAX", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_SLOW_IOS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},

  {"IO_WRITE_BYTES", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_WRITE_REQUESTS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_WRITE_SVC_USECS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_WRITE_SVC_USECS_MAX", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_WRITE_WAIT_USECS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_WRITE_WAIT_USECS_MAX", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_WRITE_SLOW_IOS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},

  {"IO_READ_BYTES_BLOB", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_REQUESTS_BLOB", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_SVC_USECS_BLOB", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_SVC_USECS_MAX_BLOB", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_WAIT_USECS_BLOB", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_WAIT_USECS_MAX_BLOB", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_SLOW_IOS_BLOB", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},

  {"IO_READ_BYTES_PRIMARY", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_REQUESTS_PRIMARY", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_SVC_USECS_PRIMARY", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_SVC_USECS_MAX_PRIMARY", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_WAIT_USECS_PRIMARY", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_WAIT_USECS_MAX_PRIMARY", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_SLOW_IOS_PRIMARY", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},

  {"IO_READ_BYTES_SECONDARY", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_REQUESTS_SECONDARY", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_SVC_USECS_SECONDARY", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_SVC_USECS_MAX_SECONDARY", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_WAIT_USECS_SECONDARY", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_WAIT_USECS_MAX_SECONDARY", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_SLOW_IOS_SECONDARY", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},

  {"IO_INDEX_INSERTS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"KEYS_DIRTIED", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"QUERIES_USED", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},

  {"INNODB_BUFFER_POOL_PAGES", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0, 0, 0, SKIP_OPEN_TABLE},

  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0, SKIP_OPEN_TABLE}
};

void fill_table_stats_cb(const char *db,
                         const char *table,
                         my_io_perf_t *r,
                         my_io_perf_t *w,
                         my_io_perf_t *r_blob,
                         my_io_perf_t *r_primary,
                         my_io_perf_t *r_secondary,
                         comp_stat_t *comp_stat,
                         int n_lru,
                         const char *engine)
{
  TABLE_STATS *stats;

  stats= get_table_stats_by_name(db, table, engine, NULL, 0, NULL);
  if (!stats)
    return;

  /* These assignments allow for races. That is OK. */
  stats->io_perf_read = *r;
  stats->io_perf_write = *w;
  stats->io_perf_read_blob = *r_blob;
  stats->io_perf_read_primary = *r_primary;
  stats->io_perf_read_secondary = *r_secondary;
  stats->comp_stat = *comp_stat;
  stats->n_lru = n_lru;
}

int fill_table_stats(THD *thd, TABLE_LIST *tables, COND *cond)
{
  DBUG_ENTER("fill_table_stats");
  TABLE* table= tables->table;

  /* TODO(mcallaghan): figure out how to mark deleted tables */

  ha_get_table_stats(fill_table_stats_cb);

  pthread_mutex_lock(&LOCK_global_table_stats);

  for (unsigned i = 0; i < global_table_stats.records; ++i) {
    int f= 0;

    TABLE_STATS *table_stats =
      (TABLE_STATS*)hash_element(&global_table_stats, i);

    if (table_stats->rows_inserted == 0 &&
        table_stats->rows_updated == 0 &&
        table_stats->rows_deleted == 0 &&
        table_stats->rows_read == 0 &&
        table_stats->rows_requested == 0 &&
        table_stats->comp_stat.compressed == 0 &&
        table_stats->comp_stat.compressed_ok == 0 &&
        table_stats->comp_stat.compressed_usec == 0 &&
        table_stats->comp_stat.compressed_ok_usec == 0 &&
        table_stats->comp_stat.decompressed == 0 &&
        table_stats->comp_stat.decompressed_usec == 0 &&
        table_stats->io_perf_read.requests == 0 &&
        table_stats->io_perf_write.requests == 0 &&
        table_stats->io_perf_read_blob.requests == 0 &&
        table_stats->io_perf_read_primary.requests == 0 &&
        table_stats->io_perf_read_secondary.requests == 0 &&
        table_stats->n_lru == 0)
    {
      continue;
    }

    restore_record(table, s->default_values);
    table->field[f++]->store(table_stats->db, strlen(table_stats->db),
                           system_charset_info);
    table->field[f++]->store(table_stats->table, strlen(table_stats->table),
                             system_charset_info);

    table->field[f++]->store(table_stats->engine_name,
                             strlen(table_stats->engine_name),
                             system_charset_info);

    table->field[f++]->store(table_stats->rows_inserted, TRUE);
    table->field[f++]->store(table_stats->rows_updated, TRUE);
    table->field[f++]->store(table_stats->rows_deleted, TRUE);
    table->field[f++]->store(table_stats->rows_read, TRUE);
    table->field[f++]->store(table_stats->rows_requested, TRUE);

    table->field[f++]->store(table_stats->comp_stat.page_size, TRUE);
    table->field[f++]->store(table_stats->comp_stat.padding, TRUE);
    table->field[f++]->store(table_stats->comp_stat.padding_savings, TRUE);
    table->field[f++]->store(table_stats->comp_stat.compressed, TRUE);
    table->field[f++]->store(table_stats->comp_stat.compressed_ok, TRUE);
    table->field[f++]->store(table_stats->comp_stat.compressed_primary, TRUE);
    table->field[f++]->store(table_stats->comp_stat.compressed_primary_ok, TRUE);
    table->field[f++]->store(table_stats->comp_stat.compressed_usec, TRUE);
    table->field[f++]->store(table_stats->comp_stat.compressed_ok_usec, TRUE);
    table->field[f++]->store(table_stats->comp_stat.compressed_primary_usec, TRUE);
    table->field[f++]->store(table_stats->comp_stat.compressed_primary_ok_usec, TRUE);
    table->field[f++]->store(table_stats->comp_stat.decompressed, TRUE);
    table->field[f++]->store(table_stats->comp_stat.decompressed_usec, TRUE);

    table->field[f++]->store(table_stats->rows_index_first, TRUE);
    table->field[f++]->store(table_stats->rows_index_next, TRUE);

    table->field[f++]->store(table_stats->io_perf_read.bytes, TRUE);
    table->field[f++]->store(table_stats->io_perf_read.requests, TRUE);
    table->field[f++]->store(table_stats->io_perf_read.svc_usecs, TRUE);
    table->field[f++]->store(table_stats->io_perf_read.svc_usecs_max, TRUE);
    table->field[f++]->store(table_stats->io_perf_read.wait_usecs, TRUE);
    table->field[f++]->store(table_stats->io_perf_read.wait_usecs_max, TRUE);
    table->field[f++]->store(table_stats->io_perf_read.slow_ios, TRUE);

    table->field[f++]->store(table_stats->io_perf_write.bytes, TRUE);
    table->field[f++]->store(table_stats->io_perf_write.requests, TRUE);
    table->field[f++]->store(table_stats->io_perf_write.svc_usecs, TRUE);
    table->field[f++]->store(table_stats->io_perf_write.svc_usecs_max, TRUE);
    table->field[f++]->store(table_stats->io_perf_write.wait_usecs, TRUE);
    table->field[f++]->store(table_stats->io_perf_write.wait_usecs_max, TRUE);
    table->field[f++]->store(table_stats->io_perf_write.slow_ios, TRUE);

    table->field[f++]->store(table_stats->io_perf_read_blob.bytes, TRUE);
    table->field[f++]->store(table_stats->io_perf_read_blob.requests, TRUE);
    table->field[f++]->store(table_stats->io_perf_read_blob.svc_usecs, TRUE);
    table->field[f++]->store(table_stats->io_perf_read_blob.svc_usecs_max, TRUE);
    table->field[f++]->store(table_stats->io_perf_read_blob.wait_usecs, TRUE);
    table->field[f++]->store(table_stats->io_perf_read_blob.wait_usecs_max, TRUE);
    table->field[f++]->store(table_stats->io_perf_read_blob.slow_ios, TRUE);

    table->field[f++]->store(table_stats->io_perf_read_primary.bytes, TRUE);
    table->field[f++]->store(table_stats->io_perf_read_primary.requests, TRUE);
    table->field[f++]->store(table_stats->io_perf_read_primary.svc_usecs, TRUE);
    table->field[f++]->store(table_stats->io_perf_read_primary.svc_usecs_max, TRUE);
    table->field[f++]->store(table_stats->io_perf_read_primary.wait_usecs, TRUE);
    table->field[f++]->store(table_stats->io_perf_read_primary.wait_usecs_max, TRUE);
    table->field[f++]->store(table_stats->io_perf_read_primary.slow_ios, TRUE);

    table->field[f++]->store(table_stats->io_perf_read_secondary.bytes, TRUE);
    table->field[f++]->store(table_stats->io_perf_read_secondary.requests, TRUE);
    table->field[f++]->store(table_stats->io_perf_read_secondary.svc_usecs, TRUE);
    table->field[f++]->store(table_stats->io_perf_read_secondary.svc_usecs_max, TRUE);
    table->field[f++]->store(table_stats->io_perf_read_secondary.wait_usecs, TRUE);
    table->field[f++]->store(table_stats->io_perf_read_secondary.wait_usecs_max, TRUE);
    table->field[f++]->store(table_stats->io_perf_read_secondary.slow_ios, TRUE);

    table->field[f++]->store(table_stats->index_inserts, TRUE);
    table->field[f++]->store(table_stats->keys_dirtied, TRUE);
    table->field[f++]->store(table_stats->queries_used, TRUE);

    table->field[f++]->store(table_stats->n_lru, TRUE);

    if (schema_table_store_record(thd, table))
    {
      pthread_mutex_unlock(&LOCK_global_table_stats);
      DBUG_RETURN(-1);
    }
  }
  pthread_mutex_unlock(&LOCK_global_table_stats);

  DBUG_RETURN(0);
}

ST_FIELD_INFO index_stats_fields_info[]=
{
  {"TABLE_SCHEMA", NAME_LEN, MYSQL_TYPE_STRING, 0, 0, 0, SKIP_OPEN_TABLE},
  {"TABLE_NAME", NAME_LEN, MYSQL_TYPE_STRING, 0, 0, 0, SKIP_OPEN_TABLE},
  {"INDEX_NAME", NAME_LEN, MYSQL_TYPE_STRING, 0, 0, 0, SKIP_OPEN_TABLE},
  {"TABLE_ENGINE", NAME_LEN, MYSQL_TYPE_STRING, 0, 0, 0, SKIP_OPEN_TABLE},
  {"ROWS_INSERTED", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"ROWS_UPDATED", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"ROWS_DELETED", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"ROWS_READ", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"ROWS_REQUESTED", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},

  {"ROWS_INDEX_FIRST", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"ROWS_INDEX_NEXT", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},

  {"IO_READ_BYTES", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_REQUESTS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_SVC_USECS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_SVC_USECS_MAX", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_WAIT_USECS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_WAIT_USECS_MAX", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_SLOW_IOS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},

  {"IO_READ_BYTES_BLOB", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_REQUESTS_BLOB", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_SVC_USECS_BLOB", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_SVC_USECS_MAX_BLOB", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_WAIT_USECS_BLOB", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_WAIT_USECS_MAX_BLOB", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"IO_READ_SLOW_IOS_BLOB", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0, SKIP_OPEN_TABLE}
};


int fill_index_stats(THD *thd, TABLE_LIST *tables, COND *cond)
{
  DBUG_ENTER("fill_index_stats");
  TABLE* table= tables->table;

  pthread_mutex_lock(&LOCK_global_table_stats);

  for (unsigned i = 0; i < global_table_stats.records; ++i) {
    uint ix;

    TABLE_STATS *table_stats =
      (TABLE_STATS*)hash_element(&global_table_stats, i);

    for (ix=0; ix < table_stats->num_indexes; ++ix)
    {
      INDEX_STATS *index_stats= &(table_stats->indexes[ix]); 
      int f= 0;

      if (index_stats->rows_inserted == 0 &&
          index_stats->rows_updated == 0 &&
          index_stats->rows_deleted == 0 &&
          index_stats->rows_read == 0 &&
          index_stats->rows_requested == 0)
      {
        continue;
      }

      restore_record(table, s->default_values);

      table->field[f++]->store(table_stats->db, strlen(table_stats->db),
                               system_charset_info);
      table->field[f++]->store(table_stats->table, strlen(table_stats->table),
                               system_charset_info);
      table->field[f++]->store(index_stats->name, strlen(index_stats->name),
                               system_charset_info);

      table->field[f++]->store(table_stats->engine_name,
                               strlen(table_stats->engine_name),
                               system_charset_info);

      table->field[f++]->store(index_stats->rows_inserted, TRUE);
      table->field[f++]->store(index_stats->rows_updated, TRUE);
      table->field[f++]->store(index_stats->rows_deleted, TRUE);
      table->field[f++]->store(index_stats->rows_read, TRUE);
      table->field[f++]->store(index_stats->rows_requested, TRUE);

      table->field[f++]->store(index_stats->rows_index_first, TRUE);
      table->field[f++]->store(index_stats->rows_index_next, TRUE);

      table->field[f++]->store(index_stats->io_perf_read.bytes, TRUE);
      table->field[f++]->store(index_stats->io_perf_read.requests, TRUE);
      table->field[f++]->store(index_stats->io_perf_read.svc_usecs, TRUE);
      table->field[f++]->store(index_stats->io_perf_read.svc_usecs_max, TRUE);
      table->field[f++]->store(index_stats->io_perf_read.wait_usecs, TRUE);
      table->field[f++]->store(index_stats->io_perf_read.wait_usecs_max, TRUE);
      table->field[f++]->store(index_stats->io_perf_read.slow_ios, TRUE);

      table->field[f++]->store(index_stats->io_perf_read_blob.bytes, TRUE);
      table->field[f++]->store(index_stats->io_perf_read_blob.requests, TRUE);
      table->field[f++]->store(index_stats->io_perf_read_blob.svc_usecs, TRUE);
      table->field[f++]->store(index_stats->io_perf_read_blob.svc_usecs_max, TRUE);
      table->field[f++]->store(index_stats->io_perf_read_blob.wait_usecs, TRUE);
      table->field[f++]->store(index_stats->io_perf_read_blob.wait_usecs_max, TRUE);
      table->field[f++]->store(index_stats->io_perf_read_blob.slow_ios, TRUE);

      if (schema_table_store_record(thd, table))
      {
        pthread_mutex_unlock(&LOCK_global_table_stats);
        DBUG_RETURN(-1);
      }
    }
  }

  pthread_mutex_unlock(&LOCK_global_table_stats);

  DBUG_RETURN(0);
}

ST_FIELD_INFO user_stats_fields_info[]=
{
  {"USER_NAME", NAME_LEN, MYSQL_TYPE_STRING, 0, 0, 0, SKIP_OPEN_TABLE},
  {"BINLOG_BYTES_WRITTEN", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"BINLOG_EVENTS_SKIP_SET", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"BYTES_RECEIVED", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"BYTES_SENT", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"COMMANDS_DDL", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"COMMANDS_DELETE", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"COMMANDS_HANDLER", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"COMMANDS_INSERT", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"COMMANDS_OTHER", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"COMMANDS_SELECT", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"COMMANDS_TRANSACTION", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"COMMANDS_UPDATE", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"CONNECTIONS_CONCURRENT", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"CONNECTIONS_DENIED_MAX_GLOBAL", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"CONNECTIONS_DENIED_MAX_USER", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"CONNECTIONS_LOST", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"CONNECTIONS_TOTAL", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"DISK_READ_BYTES", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"DISK_READ_REQUESTS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"DISK_READ_SVC_USECS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"DISK_READ_WAIT_USECS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"DISK_READ_BYTES_BLOB", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"DISK_READ_REQUESTS_BLOB", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"DISK_READ_SVC_USECS_BLOB", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"DISK_READ_WAIT_USECS_BLOB", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"DISK_READ_BYTES_PRIMARY", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"DISK_READ_REQUESTS_PRIMARY", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"DISK_READ_SVC_USECS_PRIMARY", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"DISK_READ_WAIT_USECS_PRIMARY", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"DISK_READ_BYTES_SECONDARY", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"DISK_READ_REQUESTS_SECONDARY", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"DISK_READ_SVC_USECS_SECONDARY", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"DISK_READ_WAIT_USECS_SECONDARY", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"ERRORS_ACCESS_DENIED", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"ERRORS_TOTAL", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"KEYS_DIRTIED", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"LIMIT_WAIT_QUERIES", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"LIMIT_FAIL_TRANSACTIONS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"MICROSECONDS_CPU", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"MICROSECONDS_RECORDS_IN_RANGE", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"MICROSECONDS_WALL", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"MICROSECONDS_DDL", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"MICROSECONDS_DELETE", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"MICROSECONDS_HANDLER", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"MICROSECONDS_INSERT", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"MICROSECONDS_OTHER", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"MICROSECONDS_SELECT", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"MICROSECONDS_TRANSACTION", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"MICROSECONDS_UPDATE", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"QUERIES_EMPTY", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"RECORDS_IN_RANGE_CALLS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"ROWS_DELETED", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"ROWS_FETCHED", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"ROWS_INSERTED", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"ROWS_READ", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"ROWS_UPDATED", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"ROWS_INDEX_FIRST", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"ROWS_INDEX_NEXT", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"TRANSACTIONS_COMMIT", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"TRANSACTIONS_ROLLBACK", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"QUERIES_RUNNING", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"QUERIES_WAITING", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {"TRANSACTIONS_SLOTS_INUSE", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0, 0, 0, SKIP_OPEN_TABLE},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0, SKIP_OPEN_TABLE}
};

