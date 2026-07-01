#ifndef SQL_RANGE_OPTIMIZER_INDEX_INTERSECT_PLAN_H_
#define SQL_RANGE_OPTIMIZER_INDEX_INTERSECT_PLAN_H_

#include "my_base.h"              // ha_rows
#include "sql/sql_array.h"
#include "sql/mem_root_array.h"
#include "sql/join_optimizer/overflow_bitset.h"

class TABLE;
class QUICK_RANGE;
class RANGE_OPT_PARAM;
class SEL_ROOT;
class SEL_TREE;
class Cost_model_table;
class AccessPath;
class String;

/** The structure containing info on a range index scan */
struct INDEX_SCAN_INFO {
  uint idx;         ///< # of used key in param->keys
  uint keynr;       ///< # of used key in table
  ha_rows records;  ///< estimate of # records this scan will return

  /** Set of intervals over key fields that will be used for row retrieval */
  SEL_ROOT *sel_root;

  /** Cost of reading all index tuples with values in sel_arg intervals */
  double index_scan_cost;

  /**
    The ranges to scan for this index. Must be allocated on the return_mem_root.
   */
  Bounds_checked_array<QUICK_RANGE *> ranges;

  /** Number of the components used for this index range scan */
  uint used_key_parts;

  /** selectivity of the range condition for this index */
  double selectivity;

  /** MRR flags used in this scan */
  uint mrr_flags;

};

/**
   The structure containing info on one step of the index intersect plan.
   The plan itself is a sequence of such steps. An index scan is used at
   each step.
*/
struct INDEX_INTERSECT_STEP {

  INDEX_SCAN_INFO *m_index_scan; ///< scan used at this step
  double m_out_rows;    ///< # of output rows after this step
  double m_selectivity; ///< selectivity of the partial plan after this step
  double m_cost;        ///< cost of the partial plan after this step

  /** Total # of rows to read from indexes used in the plan after this step */
  ha_rows m_rows_to_merge;

  /** Total cost of index scans used in the plan after this step */
  double m_index_scans_cost;

  /** PK filter scan used in the plan (if any) */
  INDEX_SCAN_INFO *m_cpk_scan;

  /** This constructor is needed for the current implementation of update */
  INDEX_INTERSECT_STEP()
    : m_index_scan(nullptr),
      m_out_rows(0.0),
      m_selectivity(0.0),
      m_cost(0.0),
      m_rows_to_merge(0),
      m_index_scans_cost(0.0),
      m_cpk_scan(nullptr) {}

  INDEX_INTERSECT_STEP(INDEX_SCAN_INFO *idx_scan)
    : m_index_scan(idx_scan) {}
};

/** Partial index intersect plan */
class Index_intersect_plan {

 public:

  const RANGE_OPT_PARAM *m_param; ///< Range optimizer parameter

  Mem_root_array<INDEX_INTERSECT_STEP> m_steps; ///< index scans of the plan

  /** The cost of any final plan cannot exceed this value */
  const double m_cutoff_cost;

  Index_intersect_plan(RANGE_OPT_PARAM *param, double cutoff_cost);

  /** Cost of a partial plan */
  double get_cost() {
    return m_steps.empty() ? m_cutoff_cost :  m_steps[m_steps.size()-1].m_cost;
  }

  /** True if the plan uses range condition on clustered primary key */
  bool with_cpk_filter() { return m_steps[0].m_cpk_scan; }

  /** Replace the plan with a new plan */
  void update(const Index_intersect_plan &plan);

  /** Add first index scan to a possible index intersect plan */
  bool add_first_index_scan(INDEX_SCAN_INFO *idx_scan,
                            INDEX_SCAN_INFO *cpk_scan,
                            Index_intersect_plan &best);

  /** Check if the extension of by the index scan produces a cheaper plan */
  bool check_index_intersect_ext(INDEX_SCAN_INFO *ext);

  /** Find the best full index intersect extension of this partial plan */
  void find_best_index_intersect_extension(
			      Mem_root_array<INDEX_SCAN_INFO *> *idx_scans,
                              uint start_pos,
                              INDEX_SCAN_INFO *cpk_scan,
                              Index_intersect_plan &best);

  /** Get adjusted number of the result rows for this index intersect plan */
  double get_adjusted_out_rows(double selectivity) {
    return m_table_rows * selectivity;
  }

 private:
  /** Maximal size of the memory to be used for the rowids/pk intersection */
  const ulonglong m_max_in_memory_size;
  const ha_rows m_table_rows;  /// Estimate of the # of rows in the table
  const Cost_model_table *const m_cost_model; /// Cost model to use
};

AccessPath *get_best_index_intersect(RANGE_OPT_PARAM *param,
                                     TABLE *table,
                                     SEL_TREE *tree, double cutoff_cost,
                                     bool forced);

void add_keys_and_lengths_index_intersection(const AccessPath *path,
                                             String *key_names,
                                             String *used_lengths);


#endif  // SQL_RANGE_OPTIMIZER_INDEX_INTERSECT_PLAN_H_
