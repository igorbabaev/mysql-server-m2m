/* Copyright (c) 2001, 2025, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef UNIQUES_INCLUDED
#define UNIQUES_INCLUDED

#include <stddef.h>
#include <sys/types.h>

#include "my_dbug.h"
#include "my_inttypes.h"
#include "my_pointer_arithmetic.h"
#include "my_sys.h"
#include "my_tree.h"           // TREE
#include "prealloced_array.h"  // Prealloced_array
#include "sql/sql_array.h"
#include "sql/sql_sort.h"  // IWYU pragma: keep

class Cost_model_table;
struct TABLE;
class Uniq_param;
class THD;

/**
   Unique -- class for unique (removing of duplicates).
   Puts all values to the TREE. If the tree becomes too big,
   it's dumped to the file. User can request sorted values, or
   just iterate through them. In the last case tree merging is performed in
   memory simultaneously with iteration, so it should be ~2-3x faster.

   Unique values can be read only from final result (not on insert) because
   duplicate values can be contained in different dumped tree files.
*/

class Unique {

 protected:
  /// Array of file pointers
  Prealloced_array<Merge_chunk, 16> file_ptrs;
  /// Max elements in memory buffer
  ulong max_elements;
  /// Memory buffer size
  ulonglong max_in_memory_size;
  /// Cache file for unique values retrieval of table read AM in executor
  IO_CACHE file;
  /// Tree to filter duplicates in memory
  TREE tree;
  /// Flush tree to disk
  bool flush();
  uchar *record_pointers;
  /// Element size
  uint size;
  /// Length of the record for an element in merge buffers
  uint rec_length;
  /// Action to flush elements from tree to file
  tree_walk_action flush_element_to_file;
  /// Action to write from tree to ptrs
  tree_walk_action action_write_to_ptrs;

  virtual int Merge_buffers(THD *thd, Uniq_param *param, IO_CACHE *from_file,
                            IO_CACHE *to_file, Sort_buffer sort_buffer,
                            Merge_chunk *last_chunk,
                            Merge_chunk_array chunk_array, int flag);

  virtual ulong get_n_record_pointers();

 public:
  ulong elements;
  Unique(qsort2_cmp comp_func, void *comp_func_fixed_arg, uint size_arg,
         ulonglong max_in_memory_size_arg);
  ~Unique();
  ulong elements_in_tree() { return tree.elements_in_tree; }

  /**
    Add new value to Unique

    @details The value is inserted either to the tree, or to the duplicate
    weedout table, depending on the mode of operation. If tree's mem buffer is
    full, it's flushed to the disk.

    @param ptr  pointer to the binary string to insert

    @returns
      false  error or duplicate
      true   the value was inserted
  */
  inline bool unique_add(void *ptr) {
    DBUG_TRACE;
    DBUG_PRINT("info", ("tree %u - %lu", tree.elements_in_tree, max_elements));
    if (tree.elements_in_tree > max_elements && flush()) return true;
    return !tree_insert(&tree, ptr, 0, tree.custom_arg);
  }

  bool get(TABLE *table);

  static double get_use_cost(uint nkeys, uint key_size,
                             ulonglong max_in_memory_size,
                             const Cost_model_table *cost_model);

  void reset();
  bool walk(tree_walk_action action, void *walk_action_arg);

  uint get_size() const { return size; }
  ulonglong get_max_in_memory_size() const { return max_in_memory_size; }
  bool is_in_memory() { return elements == 0; }

  friend int unique_write_to_file(void *v_key, element_count count,
                                  void *unique);
  friend int unique_write_to_ptrs(void *v_key, element_count count,
                                  void *unique);
  friend int unique_counted_write_to_ptrs(void *v_key, element_count count,
                                          void *v_unique);

  friend int unique_counted_write_to_file(void* v_key, element_count count,
                                          void *v_unique);
};

/**
  Unique_on_insert -- similar to above, but rejects duplicates on insert, not
  just on read of the final result.
  To achieve this values are inserted into mem tmp table which uses index to
  detect duplicate keys. When memory buffer is full, tmp table is dumped to a
  disk-based tmp table.
*/

class Unique_on_insert {
  /// Element size
  uint m_size;
  /// Duplicate weedout tmp table
  TABLE *m_table{nullptr};

 public:
  Unique_on_insert(uint size) : m_size(size) {}
  /**
    Add row id to the filter

    @param ptr pointer to the rowid

    @returns
      false  rowid successfully inserted
      true   duplicate or error
  */
  bool unique_add(void *ptr);

  /**
    Initialize duplicate filter - allocate duplicate weedout tmp table

    @returns
      false initialization succeeded
      true  an error occur
  */
  bool init();

  /**
    Reset filter - drop all rowid records

    @param reinit  Whether to restart index scan
  */
  void reset(bool reinit);

  /**
    Cleanup unique filter
  */
  void cleanup();
};

struct Merge_chunk_compare_context {
  qsort2_cmp key_compare;
  const void *key_compare_arg;
};

class Uniq_param {
 public:
  uint rec_length;           // Length of sorted records.
  uint max_keys_per_buffer;  // Max keys / buffer.
  ha_rows max_rows;          // Select limit, or HA_POS_ERROR if unlimited.

  uchar *unique_buff;
  bool not_killable;

  // The fields below are used only by Unique class.
  Merge_chunk_compare_context cmp_context;
  typedef int (*chunk_compare_fun)(Merge_chunk_compare_context *ctx,
                                   uchar *arg1, uchar *arg2);
  chunk_compare_fun compare;

  Uniq_param() { memset(this, 0, sizeof(*this)); }

  // Not copyable
  Uniq_param(const Uniq_param &) = delete;
  Uniq_param &operator=(const Uniq_param &) = delete;
};

double log2_n_fact(ulong n);

#endif  // UNIQUES_INCLUDED
