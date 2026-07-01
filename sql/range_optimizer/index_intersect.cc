#include "sql/range_optimizer/index_intersect.h"

#include <stdio.h>

#include "my_dbug.h"
#include "sql/range_optimizer/index_range_scan.h"
#include "sql/sql_base.h"       // free_io_cache
#include "sql/sql_executor.h"   // init_table_iterator
#include "scope_guard.h"        // create_scope_guard
#include "sql/uniques_counted.h"

IndexIntersectIterator::IndexIntersectIterator(
    THD *thd, MEM_ROOT *return_mem_root, TABLE *table,
    unique_ptr_destroy_only<RowIterator> cpk_child,
    Mem_root_array<unique_ptr_destroy_only<RowIterator>> children)
    : TableRowIterator(thd, table),
      m_cpk_child(std::move(cpk_child)),
      m_children(std::move(children)),
      mem_root(return_mem_root) {}

IndexIntersectIterator::~IndexIntersectIterator() {
  DBUG_TRACE;
  if (table()->file->inited) table()->file->ha_rnd_end();
  for (unique_ptr_destroy_only<RowIterator> &quick : m_children) {
    IndexRangeScanIterator *range =
        down_cast<IndexRangeScanIterator *>(quick.get()->real_iterator());
    range->file = nullptr;
  }
  if (m_cpk_child) {
    IndexRangeScanIterator *range =
        down_cast<IndexRangeScanIterator *>(m_cpk_child.get()->real_iterator());
    table()->file->ha_extra(HA_EXTRA_DISABLE_UNIQUE_RECORD_FILTER);
    range->file = nullptr;
  }
  read_record.reset();
  free_io_cache(table());
}

void close_unique_tree_for_expansion (TREE *tree) {
  tree->flag= TREE_ONLY_DUPS;
}

/**
  @brief
  Initialize the iterator over rowids obtained from index intersection

  @note
  The function one by one performs index only scans of the ranges for
  all N indexes participating in the intersect operation, gets rowids and
  adds them to an object of Unique_counted class. After this merge intersect
  is done leaving only those rowids that have been read by all N scans.

  One of the range scans may be a scan over the clustered primary key cpk.
  In this case any primary key read by the first index scan are checked against
  the intervals of cpk scan and is sent to Unique_counted only if it hits one
  of the intervals.

  @retval false   OK
  @retval true    Error occurred
*/

bool IndexIntersectIterator::Init() {
  empty_record(table());

  handler *file = table()->file;
  DBUG_TRACE;

  /* We're going to just read rowids. */
  table()->set_keyread(true);
  table()->prepare_for_position();

  size_t sort_buffer_size = table()->in_use->variables.sortbuff_size;

  if (unique == nullptr) {
    uint n_children = m_children.size();
    auto f = [n_children] (uint n) -> bool { return n == n_children; };
    unique.reset(new (mem_root) Unique_counted (
                                  refpos_order_cmp, (void *)file,
                                  file->ref_length, sort_buffer_size,
                                  (Unique_counted::Filter_pred) f));
    if (unique == nullptr) {
      return true;
    }
  } else {
    unique->reset();
    table()->unique_result.sorted_result.reset();
    assert(!table()->unique_result.sorted_result_in_fsbuf);
    table()->unique_result.sorted_result_in_fsbuf = false;

    if (table()->unique_result.io_cache) {
      close_cached_file(table()->unique_result.io_cache);
      my_free(table()->unique_result.io_cache);
      table()->unique_result.io_cache = nullptr;
    }
  }

  assert(file->ref_length == unique->get_size());
  assert(sort_buffer_size == unique->get_max_in_memory_size());

  {
    const Key_map covering_keys_save = table()->covering_keys;
    const bool no_keyread_save = table()->no_keyread;
    auto reset_keys =
        create_scope_guard([covering_keys_save, no_keyread_save, this] {
          table()->covering_keys = covering_keys_save;
          table()->no_keyread = no_keyread_save;
        });
    table()->no_keyread = false;

    for (unique_ptr_destroy_only<RowIterator> &child : m_children) {
      /*
         For each child index only range scan has to be performed.
         Init() might reset table->key_read to false. Take care to let
         it know that index merge needs to read only index entries.
      */
      IndexRangeScanIterator *range_scan_it =
          down_cast<IndexRangeScanIterator *>(child->real_iterator());
      table()->covering_keys.set_bit(range_scan_it->index);
      if (child->Init()) return true;
      // Make sure that index only access is used.
      assert(table()->key_read == true);

      for (;;) {
        int result = child->Read();
        if (result == -1) {
          break;  // EOF.
        } else if (result != 0 || thd()->killed) {
          return true;
        }

        /*
          It there is a range condition for clustered primary key each tuple
          of the first scan is checked against the intervals of this condition.
	*/
        if (child == m_children[0]) {
          /* skip row if it would not be retrieved by clustered PK scan */
          if (m_cpk_child && !down_cast<IndexRangeScanIterator *>(
                                  m_cpk_child.get()->real_iterator())
                                  ->row_in_ranges()) {
            continue;
          }
        }

        handler *child_file = range_scan_it->file;
        child_file->position(table()->record[0]);
        if (unique->unique_add(child_file->ref)) {
          return true;
        }
      }
      /*
        All rowids / pk from the child range scan are added to unique.
        If it's the first range scan and it turns out that there is
        enough memory for all of them then the intersect operation can
        be performed completely in memory. Every rowid from the result
        of intersection has been already read. So no new rowids from
        the other scans are not needed in unique.
      */
      if (child == m_children[0] && unique->is_in_memory()) {
        close_unique_tree_for_expansion(unique->get_unique_tree());
      }
    }

  }

  /*
    All rowids are in the Unique_counted now. The next call will perform
    intersect operation and initialize table()->sort structure so it can be
    used to iterate through the rowids sequence of the result of intersection.
  */
  if (unique->get(table())) {
    return true;
  }

  /*
    Here the true number of elements in the result of intersection is set.
    It overwrites that set in Unique::get(). The number is used in the
    iterator over the result of intersection.
  */
  table()->unique_result.found_records = unique->get_found_records();

  table()->set_keyread(false);
  read_record.reset();  // Clear out any previous iterator.
  read_record = init_table_iterator(thd(), table(),
                                    /*ignore_not_found_rows=*/false,
                                    /*count_examined_rows=*/false);
  if (read_record == nullptr) return true;
  return false;
}

int IndexIntersectIterator::Read() {
  int result;
  DBUG_TRACE;

  if ((result = read_record->Read()) == -1) {
    read_record.reset();
  }

  return result;
}
