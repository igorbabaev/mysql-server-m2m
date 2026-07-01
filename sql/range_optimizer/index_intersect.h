/**
  Index intersect
  ===============
  In its full generality the index intersect method performs intersection
  of several subsets of rowids / primary keys for table and then uses the
  the result of the intersection to access the corresponding table rows.
  The method is called index intersect rather than rowids intersect because
  in the most basic cases it is applied to the subsets of rowids extracted
  from the the tuples of indices accessed at index range scans or index
  lookus.

  When the index intersect method can be taken into consideration
  ---------------------------------------------------------------
  If the where condition contains several conjunctive sargable predicates
  over the columns of a table such that they are covered by indices and
  there is no index that covers all them then the usage of the index
  intersect method should be evaluated. Here we consider sargable predicates
  used for regular range scans as well as non-correlated IN subquery predicates
  whose subqueries return foreign keys for the table.

  When employment of in the index intersect method can be beneficial
  -----------------------------------------------------------------
  It makes sense to use the method only for big tables. Suppose we have table t
  containing 100 millions rows and a query with 3 sargable range conditions
  for columns t.a, t.b and t.c whose selectivity about 1% for each of them.
  In other words about 1 million rows satisfy each of these conditions. If to
  assume that the values in the columns are  independent then intersection
  the subsets of rowids produced by the the 3 index scan is expected to contain
  about 100*10^6 * 0.01 * 0.01 * 0.01 = 100 rows. If just any of the regular
  range scans is used to select these rows 1 million record fetches have to
  be performed in contrust with 100 row fetches when index intersect is used.
  And all of these 1 million fetches are random, so rather expensive.
  We could make them quasi sequential using sort buffer big enough to contain
  1 million rowids, but this will require extra sorting. However, when using
  the index intersect method for the gain of 10^6-100 data fetches we have to
  pay only the cost of 2 extra index scans and the cost of the operation to
  intersect rowids produced by the scans. If we assume that fetching a tuple
  from an index costs just 1/10 of the fetch of a random row we can say that
  our gain is (10^6 - 100) - 2 * 10^6 * 0.1 - (cost of rowids intersect) =
  800000 - (cost of rowids intersect). If we have enough memory for the hash
  table of the optimal size to contain 10^6 elements than the intersect of
  will cost 10^6 writes into the hash table and 2 * 10^6 lookups in it.
  Apparently this is cheaper than sorting 10^6 rowids. Thus we save at least
  on 800000 quasi sequential data fetches. If we  don't have enough memory for
  the hash table we could use temporary table to perform the intersect
  operation. Yet in this case the cost of the operation becomes much higher.

  Usage of index intersect when the values of index columns  are correlated
  -------------------------------------------------------------------------
  When the values of two columns are correlated and there ate two simple
  indexes over usage of index intersect still may be beneficial in some
  cases. It can be beneficial if the correlation if not strong. For example,
  when there are two range conditions with selectivity 1% each for two
  simple indexes, but due to correlation only 10% of rowids selected by the
  first range scan are rejected by the second range condition we still
  can observe some performance gain of using index intersect. If the table
  contains 10^6 rows the gain will be
   (10^4 - 10^6*0.01*0.1) - 10^4*0.1 - (cost of rowids intersect) =
   9000 - 4000 - (cost of rowids intersect).
  Even with a strong correlation between values of two columns the usage
  of index intersect can be beneficial. In this case the possible gain
  depends on the used ranges.
  When correlation between values of two columns are unknown we could suggest
  a possible postponed index intersect. In this mode we start just the regular
  index scan for the first range condition and count the number of rejections
  by the second range condition. If we see that this number is high enough we
  switch to the index intersect method.

  Cost of the intersect operation and inexact intersection
  --------------------------------------------------------
  The gain of index intersect depends on the efficiency of the intersect
  operation for rowids / primary keys. If we have enough memory to store
  a search structure containing all rowids from the scan with lowest expected
  cardinality then the operation can be relatively cheap. The cheapest search
  structure would be a hash table. Yet it still requires some  space for each
  rowids / primary key. Here it should be noted that we could be satisfied
  with getting some super-set of the exact intersection subset that would
  contain slightly more elements then we could save memory on storing only hash
  values instead of the elements itself. Even a better solution in this case
  would be usage of bloom filter.
  If we don't have enough memory for the needed search structure then the only
  reasonably good option is usage of sort merge.

  Index intersect of foreigh keys usage for star join queries
  -----------------------------------------------------------
  Index intersect of foreign keys can effectively reduce the number of
  accessed records of the fact table. That's why it makes sense to evaluate
  usage of this method for star join queries.

*/

#ifndef SQL_RANGE_OPTIMIZER_INDEX_INTERSECT_H_
#define SQL_RANGE_OPTIMIZER_INDEX_INTERSECT_H_

#include <assert.h>

#include "sql/range_optimizer/range_optimizer.h"

class Unique_counted;
struct TABLE;

class IndexIntersectIterator : public TableRowIterator {
 public:

  bool Init() override;
  int Read() override;

  IndexIntersectIterator(
      THD *thd, MEM_ROOT *mem_root, TABLE *table,
      unique_ptr_destroy_only<RowIterator> cpk_child,
      Mem_root_array<unique_ptr_destroy_only<RowIterator>> children);
  ~IndexIntersectIterator() override;

 private:
  /** used to perform intersect operation for rowids / primary keys */
  unique_ptr_destroy_only<Unique_counted> unique;

  /** used to get rows collected in Unique */
  unique_ptr_destroy_only<RowIterator> read_record;

  /** quick select that uses clustered primary key (NULL if none) */
  unique_ptr_destroy_only<RowIterator> m_cpk_child;

  /** range quick selects this index_intersect read consists of */
  Mem_root_array<unique_ptr_destroy_only<RowIterator>> m_children;

  MEM_ROOT *mem_root;

};

#endif  // SQL_RANGE_OPTIMIZER_INDEX_INTERSECT_H_
