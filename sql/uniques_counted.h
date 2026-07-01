#ifndef UNIQUES_COUNTED_INCLUDED
#define UNIQUES_COUNTED_INCLUDED

#include "sql/uniques.h"
#include <functional>

using std::function;

class TREE;

class Unique_counted : public Unique {

 private:
  /**
    Number of distinct elements returned by the call off the get() method.
    If the predicate filter not always returns true this number will be less
    than the number of distinct values added to the Uniques_counted object
  */
  ha_rows m_found_records;

 public:

  /**
    The type of the predicate 'filter'. std::function wrapper is used here
    in order to be able to employ lambda expressions to specify the predicate.
  */
  typedef std::function<bool(uint)> Filter_pred;

  /**
    The predicate to filter only the elements with certain counts.
    The filter is applied after the merge is fully completed.
  */
  Filter_pred filter;

  Unique_counted(qsort2_cmp comp_func, void *comp_func_fixed_arg, uint size_arg,
                 ulonglong max_in_memory_size_arg, Filter_pred filter_cond);

  int Merge_buffers(THD *thd, Uniq_param *param, IO_CACHE *from_file,
                     IO_CACHE *to_file, Sort_buffer sort_buffer,
                     Merge_chunk *last_chunk,
                     Merge_chunk_array chunk_array, int flag) override;

  ulong get_n_record_pointers() override;

  ha_rows get_found_records() { return m_found_records; }

  TREE *get_unique_tree();

  friend int unique_counted_write_to_ptrs(void *v_key, element_count count,
                                          void *v_unique);
  friend int count_record_pointers(void *v_key, element_count count,
                                   void *v_unique);
};

#endif  // UNIQUES_COUNTED_INCLUDED

