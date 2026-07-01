#include "sql/uniques_counted.h"

#include <cmath>

#include "priority_queue.h"
#include "sql/sql_class.h"

uint uniq_read_to_buffer(IO_CACHE *fromfile, Merge_chunk *merge_chunk,
                         Uniq_param *param);


int count_record_pointers(void *v_key [[maybe_unused]], element_count count,
                          void *v_unique) {
  Unique_counted *unique = static_cast<Unique_counted *>(v_unique);
  if (unique->filter(count)) {
    unique->m_found_records++;
  }
  return 0;
}

ulong Unique_counted::get_n_record_pointers() {
  if (is_in_memory()) {
    (void) tree_walk(&tree, count_record_pointers, this, left_root_right);
    return m_found_records;
  }
  return 0;
}

int unique_counted_write_to_ptrs(void *v_key, element_count count,
                                 void *v_unique) {
  uchar *key = static_cast<uchar *>(v_key);
  Unique_counted *unique = static_cast<Unique_counted *>(v_unique);
  if (unique->filter(count)) {
    memcpy(unique->record_pointers, key, unique->size);
    unique->record_pointers += unique->size;
  }
  return 0;
}

int unique_counted_write_to_file(void* v_key, element_count count,
                                 void *v_unique)
{
  uchar *key= static_cast<uchar *>(v_key);
  Unique *unique= static_cast<Unique_counted *>(v_unique);
  return (my_b_write(&unique->file, key, unique->size) ||
          my_b_write(&unique->file, reinterpret_cast<uchar *>(&count),
		     sizeof(element_count))) ? 1 : 0;
}

Unique_counted::Unique_counted(qsort2_cmp comp_func, void *comp_func_fixed_arg,
                               uint size_arg, ulonglong max_in_memory_size_arg,
                               Filter_pred filter_cond)
  : Unique(comp_func, comp_func_fixed_arg, size_arg, max_in_memory_size_arg),
    m_found_records(0), filter(filter_cond) {
  rec_length = size + sizeof(element_count);
  flush_element_to_file = unique_counted_write_to_file;
  action_write_to_ptrs = unique_counted_write_to_ptrs;
}

static int unique_write_rec_to_file(IO_CACHE *to_file, Uniq_param *param,
                                    element_count dupl_count, int flag,
                                    Unique_counted::Filter_pred filter_cond,
                                    ha_rows *found_records) {
  DBUG_TRACE;
  uint rec_length = param->rec_length;
  uint count_size = sizeof(element_count);
  uint dupl_count_ofs = rec_length - count_size;

  uint bytes_to_write = rec_length;
  if (!flag) {
    memcpy(param->unique_buff + dupl_count_ofs, &dupl_count, count_size);
  }
  else {
    if (filter_cond(dupl_count)) {
      bytes_to_write -= count_size;
      (*found_records)++;
    }
    else
      bytes_to_write = 0;
  }
  if (bytes_to_write)
  {
    DBUG_PRINT("info", ("write record at %llu len %u", my_b_tell(to_file),
                        bytes_to_write));
    if (my_b_write(to_file, param->unique_buff, bytes_to_write)) {
      return -1; /* purecov: inspected */
    }
  }

  return bytes_to_write;
} /* uniq_write_rec_to_file */

/**
  Merge buffers to one buffer.

  @param thd            thread context
  @param param          Sort parameter
  @param from_file      File with source data (Merge_chunks point to this file)
  @param to_file        File to write the sorted result data.
  @param sort_buffer    Buffer for data to store up to MERGEBUFF2 sort keys.
  @param [out] last_chunk Store here Merge_chunk describing data written to
                        to_file.
  @param chunk_array    Array of chunks to merge.
  @param flag           0 - write full record, 1 - write addon/ref

  @retval 0      OK
  @retval other  error
*/
static int merge_buffers(THD *thd, Uniq_param *param, IO_CACHE *from_file,
                         IO_CACHE *to_file, Sort_buffer sort_buffer,
                         Merge_chunk *last_chunk, Merge_chunk_array chunk_array,
                         int flag, Unique_counted::Filter_pred filter_cond,
                         ha_rows *found_records) {
  int error = 0;
  uint rec_length;
  ha_rows maxcount;
  ha_rows max_rows, org_max_rows;
  my_off_t to_start_filepos;
  uchar *strpos;
  Merge_chunk *merge_chunk;
  Uniq_param::chunk_compare_fun cmp;
  Merge_chunk_compare_context *first_cmp_arg;
  DBUG_TRACE;

  thd->inc_status_sort_merge_passes();

  rec_length = param->rec_length;

  maxcount = (param->max_keys_per_buffer / chunk_array.size());
  to_start_filepos = my_b_tell(to_file);
  strpos = sort_buffer.array();
  org_max_rows = max_rows = param->max_rows;

  /* The following will fire if there is not enough space in sort_buffer */
  assert(maxcount != 0);

  assert(param->unique_buff != nullptr);

  cmp = param->compare;
  first_cmp_arg = &param->cmp_context;

  auto greater = [&cmp, &first_cmp_arg](const Merge_chunk *a,
                                        const Merge_chunk *b) {
    return cmp(first_cmp_arg, a->current_key(), b->current_key()) > 0;
  };

  Priority_queue<Merge_chunk *,
                 std::vector<Merge_chunk *, Malloc_allocator<Merge_chunk *>>,
                 decltype(greater)>
      queue(greater,
            Malloc_allocator<Merge_chunk *>(key_memory_Filesort_info_merge));

  if (queue.reserve(chunk_array.size())) return 1;

  for (merge_chunk = chunk_array.begin(); merge_chunk != chunk_array.end();
       merge_chunk++) {
    merge_chunk->set_buffer(
        strpos, strpos + (sort_buffer.size() / (chunk_array.size())));
    merge_chunk->set_max_keys(maxcount);
    const uint bytes_read = uniq_read_to_buffer(from_file, merge_chunk, param);

    if (static_cast<int>(bytes_read) == -1) return -1; /* purecov: inspected */

    strpos += bytes_read;
    merge_chunk->set_buffer_end(strpos);

    // If less data in buffers than expected
    merge_chunk->set_max_keys(merge_chunk->mem_count());
    (void)queue.push(merge_chunk);
  }

  element_count dupl_count = 0;
  uint dupl_count_ofs = rec_length - sizeof(element_count);
  const uint count_size = sizeof(element_count);

  {
    /*
       Called by Unique::get()
       Copy the first argument to param->unique_buff for unique removal.
    */
    merge_chunk = queue.top();
    memcpy(param->unique_buff, merge_chunk->current_key(), rec_length);
    memcpy(&dupl_count, param->unique_buff + dupl_count_ofs, count_size);
    merge_chunk->advance_current_key(rec_length);
    merge_chunk->decrement_mem_count();
    if (--max_rows == 0) {
      error = 0; /* purecov: inspected */
      goto end;  /* purecov: inspected */
    }
    // The top chunk may actually contain only a single element
    if (merge_chunk->mem_count() == 0) {
      if (!(error = (int)uniq_read_to_buffer(from_file, merge_chunk, param))) {
        queue.pop();
        reuse_freed_buff(merge_chunk, &queue);
      } else if (error == -1)
        return error;
    }
    queue.update_top();  // Top element has been used
  }

  while (queue.size() > 1) {
    if (thd->killed && !param->not_killable) {
      return 1; /* purecov: inspected */
    }
    for (;;) {
      merge_chunk = queue.top();
      uchar *current_key = merge_chunk->current_key();
      if (!(*cmp)(first_cmp_arg, param->unique_buff, current_key))
        goto skip_duplicate;
      int b;
      if ((b = unique_write_rec_to_file(to_file, param, dupl_count,
				   flag, filter_cond, found_records)) < 0) {
        return 1;
      }
      /* Do not decrement max_rows if (flag == 0 or b == 0) */
      if (flag || b != 0) {
        if (--max_rows == 0) {
          error = 0; /* purecov: inspected */
          goto end;  /* purecov: inspected */
        }
      }
      memcpy(param->unique_buff, merge_chunk->current_key(), rec_length);
      memcpy(&dupl_count, param->unique_buff + dupl_count_ofs, count_size);

    skip_duplicate:
      element_count cnt;
      memcpy(&cnt, param->unique_buff + dupl_count_ofs, sizeof(element_count));
      dupl_count += cnt;

      merge_chunk->advance_current_key(rec_length);
      merge_chunk->decrement_mem_count();
      if (0 == merge_chunk->mem_count()) {
        if (!(error =
                  (int)uniq_read_to_buffer(from_file, merge_chunk, param))) {
          queue.pop();
          reuse_freed_buff(merge_chunk, &queue);
          break; /* One buffer has been removed */
        } else if (error == -1)
          return error; /* purecov: inspected */
      }
      /*
        The Merge_chunk at the queue's top had one of its keys consumed, thus
        it may now rank differently in the comparison order of the queue, so:
      */
      queue.update_top();
    }
  }
  merge_chunk = queue.top();
  merge_chunk->set_buffer(sort_buffer.array(),
                          sort_buffer.array() + sort_buffer.size());
  merge_chunk->set_max_keys(param->max_keys_per_buffer);

  /*
    As we know all entries in the buffer are unique, we only have to
    check if the first one is the same as the last one we wrote.
  */
  {
    uchar *current_key = merge_chunk->current_key();
    if (!(*cmp)(first_cmp_arg, param->unique_buff, current_key)) {
      element_count cnt;
      memcpy(&cnt, current_key + dupl_count_ofs, count_size);
      dupl_count += cnt;
      merge_chunk->advance_current_key(rec_length);  // Remove duplicate
      merge_chunk->decrement_mem_count();
    }
    int b;
    if ((b = unique_write_rec_to_file(to_file, param, dupl_count,
                                      flag, filter_cond, found_records)) < 0) {
      return 1;
    }
    if (!flag || b > 0) {
      if (--max_rows == 0) {
        error = 0; /* purecov: inspected */
        goto end;  /* purecov: inspected */
      }
    }
  }

  do {

    for (uint ix = 0; ix < merge_chunk->mem_count(); ++ix) {
      uint bytes_to_write = rec_length;
      if (flag) {
        memcpy(&dupl_count, merge_chunk->current_key() + dupl_count_ofs,
               count_size);
        if (filter_cond(dupl_count)) {
          bytes_to_write -= count_size;
          (*found_records)++;
        }
        else
          bytes_to_write = 0;
      }
      if (bytes_to_write &&
          my_b_write(to_file, merge_chunk->current_key(), bytes_to_write)) {
        return 1; /* purecov: inspected */
      }
      if (!flag || bytes_to_write > 0) {
        if (--max_rows == 0) {
          error = 0; /* purecov: inspected */
          goto end;  /* purecov: inspected */
        }
      }

      merge_chunk->advance_current_key(rec_length);
    }
  } while ((error = (int)uniq_read_to_buffer(from_file, merge_chunk, param)) !=
               -1 &&
           error != 0);

end:
  last_chunk->set_rowcount(std::min(org_max_rows - max_rows, param->max_rows));
  last_chunk->set_file_position(to_start_filepos);

  return error;
} /* merge_buffers */

int Unique_counted::Merge_buffers(THD *thd, Uniq_param *param,
                                  IO_CACHE *from_file, IO_CACHE *to_file,
                                  Sort_buffer sort_buffer,
                                  Merge_chunk *last_chunk,
                                  Merge_chunk_array chunk_array, int flag) {

  return merge_buffers(thd, param, from_file, to_file, sort_buffer,
                       last_chunk, chunk_array, flag, filter, &m_found_records);
}

TREE *Unique_counted::get_unique_tree() { return &tree; }

