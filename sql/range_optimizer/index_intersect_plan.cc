#include "sql/range_optimizer/index_intersect_plan.h"

#include "sql/mem_root_array.h"
#include "sql/opt_costmodel.h"

#include "sql/range_optimizer/range_opt_param.h"
#include "sql/range_optimizer/index_range_scan_plan.h"
#include "sql/range_optimizer/tree.h"
#include "sql/join_optimizer/access_path.h"
#include "sql/range_optimizer/path_helpers.h"
#include "sql/uniques_counted.h"
#include "sql/sql_optimizer.h"  // JOIN

/** This function is used by explain for index intersect access */
void add_keys_and_lengths_index_intersection(const AccessPath *path,
                                             String *key_names,
                                             String *used_lengths) {
  bool first = true;

  /*
    For EXPLAIN compatibility with older versions, PRIMARY is always
    printed last.
  */
  for (AccessPath *child : *path->index_intersection().children) {
    if (first) {
      first = false;
    } else {
      key_names->append(',');
      used_lengths->append(',');
    }
    ::add_keys_and_lengths(child, key_names, used_lengths);
  }
  if (path->index_intersection().cpk_child) {
    key_names->append(',');
    used_lengths->append(',');
    ::add_keys_and_lengths(path->index_intersection().cpk_child,
                           key_names, used_lengths);
  }
}

Index_intersect_plan::Index_intersect_plan(RANGE_OPT_PARAM *param,
                                           double cutoff_cost)
    : m_param(param),
      m_steps(param->return_mem_root, 0),
      m_cutoff_cost(cutoff_cost),
      m_max_in_memory_size(param->table->in_use->variables.sortbuff_size),
      m_table_rows(param->table->file->stats.records),
      m_cost_model(param->table->cost_model()) {}

/** Replace this index intersect plan with a new plan */
void Index_intersect_plan::update(const Index_intersect_plan &plan) {
  m_steps.resize(plan.m_steps.size());
  for (uint i = 0; i < plan.m_steps.size(); i++) {
    m_steps[i] = plan.m_steps[i];
  }
}

/**
  @brief
  Build INDEX_SCAN_INFO structure for an index scan

  @param param         Parameter from test_quick_select function
  @param idx           Index of key in param->keys used by the scan
  @param sel_root      Set of intervals for the given key
  @param index_scans   Info on index scans that can be used for intersection
  @param cufoff_cost   Discard the scans with a higher cost
  @param cpk_no        The number of the clustered primary key (if any)

  @retval  NULL if the scan can be used for intersection or out of memory
  @retval  Index scan structure for the scan specified by {idx, sel_root}

  @note
  The function evaluates the range index scan specified by the pair
  of parameters {idx,sel_arg} and if this scan can be used in index
  intersection it adds the info on this scan to the array index_scans
  and maybe removing the info on another scan added earlier to the array.
  Currently only index scans over the major component can be employed
  for index intersection. If the range index scan {idx,sel_root} uses
  intervals over more than one components it's turned down immediately.
  It's turned down immediately as well if it's the scan for the secondary
  index whose major component is the same as the major component of the
  clustered primary key.
  For each index scan over one components the cost of index retrieval
  of the index tuples the cost of  the range scan is calculated. If
  this cost is not less than cufoff_cost the index scan is turned down.
  If this cost is less than the cost of one index scan over the same
  field which has been already evaluated and added to index_scans than
  the info for the latter is replaced with the info for the index scan
  {idx,sel_root}. Otherwise the function allocates an INDEX_SCAN_INFO
  structure for the index scan {idx,sel_arg) and adds it to the array
  index_scans.
  If the evaluated index scan is not turned down the function returns
  the pointer to the corresponding info structure.
*/
static INDEX_SCAN_INFO *make_index_scan_info(
                        RANGE_OPT_PARAM *param, int idx, SEL_ROOT *sel_root,
                        Mem_root_array<INDEX_SCAN_INFO *> &index_scans,
                        double cutoff_cost, uint cpk_no) {
  INDEX_SCAN_INFO *idx_scan = 0;
  DBUG_TRACE;

  Cost_estimate cost;
  uint mrr_flags = 0, buf_size = 0;
  bool is_ror_scan, is_imerge_scan;
  THD *thd = param->table->in_use;
  uint keynr = param->real_keynr[idx];

  if (param->table->quick_key_parts[keynr] != 1) return nullptr;

  if (cpk_no != MAX_KEY && keynr != cpk_no &&
      param->table->key_info[keynr].key_part[0].fieldnr ==
      param->table->key_info[cpk_no].key_part[0].fieldnr) {
    return nullptr;
  }

  (void) check_quick_select(thd, param, idx, true, sel_root, false,
                            ORDER_NOT_RELEVANT, param->skip_records_in_range,
                            &mrr_flags, &buf_size, &cost,
                            &is_ror_scan, &is_imerge_scan);
  double idx_only_scan_cost = cost.total_cost();

  if (param->real_keynr[idx] != cpk_no &&
      idx_only_scan_cost >= cutoff_cost) {
    return nullptr;
  }

  for (uint i = 0; i < index_scans.size(); i++) {
    INDEX_SCAN_INFO *scan =  index_scans[i];
    if (param->table->key_info[keynr].key_part[0].fieldnr ==
        param->table->key_info[scan->keynr].key_part[0].fieldnr &&
        idx_only_scan_cost < scan->index_scan_cost) {
      idx_scan = scan;
      break;
    }
  }

  if (!(idx_scan != 0) &&
      !(idx_scan = new (param->return_mem_root) INDEX_SCAN_INFO)) {
    return nullptr;
  }

  if (param->real_keynr[idx] != cpk_no) {
    index_scans.push_back(idx_scan);
  }

  idx_scan->idx = idx;
  idx_scan->keynr = param->real_keynr[idx];
  idx_scan->sel_root = sel_root;
  idx_scan->records = param->table->quick_rows[idx_scan->keynr];
  idx_scan->used_key_parts = param->table->quick_key_parts[idx_scan->keynr];
  idx_scan->selectivity = (double) idx_scan->records /
                          param->table->file->stats.records;
  idx_scan->index_scan_cost = idx_only_scan_cost;
  idx_scan->mrr_flags = mrr_flags;

  return idx_scan;
}

/**
  @brief
  Calculate cost of using Unique_counted for intersection of index scans

  @param nkeys     Total number of index tuples read by index scans
  @param key_size  Size of rowids / primary keys read from the tuples
  @param first_scan_nkeys Number of tuples read at the first scan
  @param max_in_memory_size Maximal size of the memory that can be used
  @param cost_model   Cost model to calculate the cost

  @retval  Cost of using Unique_counted for intersection of index scans

  @note
  If there is enough memory for all rowids/pk read at the first index scan
  then the required cost is the sum of the cost of building a binary tree for
  these rowids and the cost checking rowids read by the other scans against
  this tree. Otherwise the the required cost is the same as the cost of using
  Unique for index union merge of the indexes participating in index intersect.
*/
static
double get_cost_of_unique_counted_use_for_intersection(
                                      uint nkeys, uint key_size,
                                      uint first_scan_nkeys,
                                      ulonglong max_in_memory_size,
                                      const Cost_model_table *cost_model) {
  double cost = 0.0;
  if ((ulonglong)first_scan_nkeys *
       ALIGN_SIZE(sizeof(TREE_ELEMENT) + key_size) < max_in_memory_size) {
    /* Merge is done in memory using one tree with first_scan_nkeys elements */
    /* Calculate # of comparisons to create the tree for the first index scan */
    double n_compares = 2 * log2_n_fact(first_scan_nkeys + 1);
    /* Add the #of comparisons to merge the keys of the remaining index scans */
    n_compares += std::log2(first_scan_nkeys) * (nkeys - first_scan_nkeys);
    if (n_compares > 0.0) cost = cost_model->key_compare_cost(n_compares);
  }
  else {
    cost = Unique::get_use_cost(nkeys, key_size + sizeof(element_count),
                                max_in_memory_size, cost_model);
  }
  return cost;
}

/**
  @brief
  Make access path for index range scan used in index intersect

  @param param         Parameter from test_quick_select function
  @param tree          SEL_TREE to extract the range intervals
  @param idx_scan      Info on the index scan

  @retval  The required access path if success
  @retval  NULL otherwise
*/
AccessPath *MakeIntersectionIndexScan(RANGE_OPT_PARAM *param,
                                      SEL_TREE *tree,
                                      INDEX_SCAN_INFO *idx_scan) {
  AccessPath *path = new (param->return_mem_root) AccessPath;

  Quick_ranges ranges(param->return_mem_root);
  unsigned used_key_parts, num_exact_key_parts;
  if (get_ranges_from_tree(param->return_mem_root, param->table,
                           param->key[idx_scan->idx], idx_scan->keynr,
                           tree->keys[idx_scan->idx],
                           MAX_REF_PARTS, &used_key_parts,
                           &num_exact_key_parts, &ranges)) {
    return nullptr;
  }
  path->type = AccessPath::INDEX_RANGE_SCAN;
  path->index_range_scan().index = idx_scan->keynr;
  path->index_range_scan().num_used_key_parts = used_key_parts;
  path->index_range_scan().used_key_part = param->key[idx_scan->idx];
  path->index_range_scan().ranges = &ranges[0];
  path->index_range_scan().num_ranges = ranges.size();
  path->index_range_scan().mrr_flags = idx_scan->mrr_flags;
  path->index_range_scan().mrr_buf_size = 0;
  path->index_range_scan().can_be_used_for_ror = false;
  path->index_range_scan().need_rows_in_rowid_order = false;
  path->index_range_scan().can_be_used_for_imerge = false;
  path->index_range_scan().reuse_handler = false;
  path->index_range_scan().geometry = false;
  path->index_range_scan().reverse = false;

  return path;
}

/**
  @brief Find the best index intersect and make access path for it

  @param param     Parameter from test_quick_select function
  @param table     Table to access by the found index intersect
  @param tree      SEL_TREE to extract the range intervals for used scans
  @param tree      Upper limit fot the cost of considered index intersects
  @param force     True if a hint forces to use an index merge access method

  @retval  Access path to the best index intersect if there is any
  @retval  NULL otherwise

  @note
  The function first extracts the information on possible index scans
  previously collected and saved in 'param'. Then it looks for the cheapest
  index intersect access to the 'table' whose cost is less than cutoff_cost.
  Finally, if it finds any the function creates an access path for this
  access. If later it turns out that this is the best method to access the table
  the iterator constructed for this access path is used at the execution of
  the query.
*/
AccessPath *get_best_index_intersect(RANGE_OPT_PARAM *param,
                                     TABLE *table, SEL_TREE *tree,
                                     double cutoff_cost, bool forced) {

  /*
    Step1: Collect single index scan SEL_ARGs and create INDEX_SCAN_INFO for
    each of them. Also find and save clustered PK scan if there is one.
  */

  INDEX_SCAN_INFO *cpk_scan = nullptr;
  uint cpk_no;

  cpk_no = ((table->file->primary_key_is_clustered()) ? table->s->primary_key
                                                      : MAX_KEY);
  Mem_root_array<INDEX_SCAN_INFO *> index_scans(param->temp_mem_root);

  for (uint idx = 0; idx < param->keys; idx++) {
    INDEX_SCAN_INFO *scan;
    if (!tree->keys[idx]) continue;
    if (!(scan = make_index_scan_info(param, idx, tree->keys[idx],
                                      index_scans, cutoff_cost, cpk_no))) {
      continue;
    }
    if (param->real_keynr[idx] == cpk_no) {
      cpk_scan = scan;
    }
  }

  uint n_scans = index_scans.size();
  if (!n_scans || ((n_scans == 1) && !cpk_scan)) return nullptr;

  /* Step2: Find the cheapest index intersect plan if there is any. */

  std::stable_sort(index_scans.begin(), index_scans.end(),
                   [] (INDEX_SCAN_INFO *a, INDEX_SCAN_INFO *b) {
                   return a->records < b->records;
	           });

  Index_intersect_plan best_plan(param, cutoff_cost);
  Index_intersect_plan cur_plan(param, cutoff_cost);

  cur_plan.find_best_index_intersect_extension(&index_scans, 0, cpk_scan,
                                               best_plan);

  uint n_child_scans = best_plan.m_steps.size();
  if (n_child_scans == 0 ||
      (n_child_scans == 1 && !best_plan.with_cpk_filter())) {
    return nullptr;
  }

  /*
    Step3: Make access paths for the index scans used in the found index
    intersect and for the index intersect proper.
  */

  AccessPath *cpk_child = nullptr;
  if (best_plan.m_steps[0].m_cpk_scan &&
      !(cpk_child = MakeIntersectionIndexScan(param, tree,
                                    best_plan.m_steps[0].m_cpk_scan))) {
      return nullptr;
  }
  auto *children =  new (param->return_mem_root)
      Mem_root_array<AccessPath *>(param->return_mem_root);
  children->resize(n_child_scans);
  for (unsigned i = 0; i < n_child_scans; ++i) {
    INDEX_SCAN_INFO *idx_scan = best_plan.m_steps[i].m_index_scan;
    if (!((*children)[i] = MakeIntersectionIndexScan(param, tree, idx_scan))) {
      return nullptr;
    }
  }

  AccessPath *intersection_path = nullptr;
  intersection_path = new (param->return_mem_root) AccessPath;
  intersection_path->type = AccessPath::INDEX_INTERSECTION;
  intersection_path->index_intersection().table = table;
  intersection_path->index_intersection().forced_by_hint = forced;
  intersection_path->index_intersection().children = children;
  intersection_path->index_intersection().cpk_child = cpk_child;
  intersection_path->set_cost(best_plan.get_cost());
  double out_rows = best_plan.m_steps[best_plan.m_steps.size()-1].m_out_rows;
  intersection_path->set_num_output_rows(out_rows);
  return intersection_path;
}


/*
  Finding the cheapest index intersection
  =======================================

  1. Enumeration of index intersection plans
  ------------------------------------------
  Any index intersection is determined by the subset of the set S of
  eligible index scans participating in the intersection {s[j1],...,s[jn]}.
  So when enumerating possible index intersection it's enough to evaluate
  only ordered sequences of index scans from S. The ordering function
  does not matter for enumeration. However we do not evaluate each ordered
  subset of index scans due to a cost-based pruning we use. For a better
  pruning we order the elements of S by their selectivity:
    sel(s[1]) <= ... <= sel(s[N]).
  When evaluating an intersection of index scans s[j1],...,s[jn] we
  calculate the cost of the operation. It consist of the sum the cost of
  all index only scans s[j1],...,s[jn] and the cost of the intersection
  of rowids/pk obtained from these scans r[j1],...r[jn]. The latter,
  of course, depends of algoritm we use for the intersection. But no matter
  what algorithm we use the minimal cost is reached for a linear plan of
  intersection (r[j1],...,r[jn]) such that:
     cardinality(r[j1]) <= ... <= cardinality(r[j2]).
  In a linear plan each r[lk] is intersected with the result of the previous
  intersection. (Here we do not consider algorithms that intersect some
  groups of r[jk] in parallel.)
  Evaluation of eligible index intersect plans is performed in the order
  dictated by our enumeration procedure. The procedure starts with the
  simplest plans containing only two index scans. The procedure extends
  the plans adding new index scans one by one. If after adding a new index
  scan s[jm] to an already evaluated index plan (s[j1],...,s[jk]) the
  procedure gets a more expensive plan than the plan (i[j1],..., i[jk])
  any extension of the plan (s[j1],...,s[jk],s[jm]) are not evaluated
  and the procedure moves to the evaluation of the extension by the index
  scan which follows s[jm] in the ordered sequence of eligible index scans.
  Otherwise the procedure evaluates all possible extensions of the plan
  (s[j1],...,s[jk],s[jm]).

  2. Cost-based pruning of index intersection plans
  -------------------------------------------------
  Any index intersection plan is evaluated when a new index scan s[jm] is
  added to an already built index intersection plan (s[j1],..., s[jk]).
  According to the enumeration procedure The selectivity of the added scan
  is not smaller than then the selectivity of any scan in the plan to be
  extended. If the cost of the plan (s[j1],...,s[jk],s[jm]) is not smaller
  than the cost of the plan (s[j1],...,s[jk]) than the cost of the any
  extension of the plan  (s[j1],...,s[jk],s[jm]) is not smaller than the
  cost of the plan (s[j1],...,s[jk]). Let's prove it.
  First we'll do the following observations. When we add a new scan s[jl]
  to an index intersection plan we save on the reduction of of data accesses.
  This reduction is calculated as (1-sel(s[jl])*sel((s[j1],...,s[jk])). At
  the same time we lose on index only scan for s[jl] and on the additional
  cost of intersection of rowids/pk. The difference between the gain of the
  reduction of rowids and the additional losses forms the gain of the
  extension by the scan s[jl]: G(i[jl]). If G(s[jl) is positive then then
  new plan is less expensive than the plan (s[j1],...,s[jk]). Otherwise
  it's more expensive. Thus if C is the cost of the plan (s[j1],...,s[jk])
  and C' is the cost of the plan (s[j1],...,s[jk],s[jl]) then we have
      C' = C - G(s[jl])
  Here G(s[jl]) actually depends not only on s[jl] but on {(s[j1],...,s[jk]}
  as well.
  If we have an index intersection plan (s[j1],...,s[jk]) and an index scan
  s[jl] not used in the plan then for the plan (s[j1],...,s[jl],...s[jk])
  for every scan s[ji] following the scan s[jl] in the plan the gain it
  has in the new plan G'(s[ji]) is less than the gain in the plan
  (s[j1],...,s[jk]) G(s[ji]). This is because the gain from the reduction
  of data accesses becomes smaller due to
     sel((s[j1],...,s[jk])) < to sel((s[j1],...,s[jk],s[jl])
  while the cost of rowids intersection remains the same unless it not
  becomes bigger when the operation cannot be performed in memory.
  Now we are ready to prove that our pruning procedure does not allow to miss
  evaluation the plan for the cheapest index intersection.
  Consider the index intersection P (s[j1],...,s[jk],s[jl],s[j1'],...,s[jm'])
  such that sel(s[j1])<=sel(s[jk])<=sel(s[jl])<=sel(s[j1']<=sel(s[jm']) and
  s[jl] is the first scan with negative gain. Note that our pruning procedure
  will prevent evaluation of P. Let's remove s[jl] from this plan. We get a
  new plan P' (s[j1],...,s[jk],s[j1'],...,s[jm']). The cost of this plan C(P')
  is less than the cost C(P) of the plan P: C(P') < C(P). Indeed we have
    C(P') = C(s[j1],...,s[jk]) - G'(s[j1']) ... - G'(s[jm']) and
    C(P) = C(s[j1],...,s[jk]) - G(s[jk] - G(s[j1']) ... - G(s[jm'])
  where according to our observation above for each ji' G'(s[ji']) > G(s[ji')).
  If it turns out that of some of the scans s[j1'],...,s[jm'] has a negative
  gain we take the first one of them and remove it from the plan P' getting a
  cheaper plan P": C(P") < C(P). We continue applying such removals until
  we don't have any scans following the scan s[jk] with negative gain. For
  the ultimate plan P^ we have C(P^) < C(P) and P^ won't be pruned out, so
  it will be evaluated.
  Note that the above reasoning is valid only when the plan (s[j1],...,s[jk])
  contains at least of 2 elements.
*/


/**
  @brief Add info on the first index scan to the empty index intersect plan

  @param  idx_scans array of all index scans that can be used in intersection
  @param  cpk_scan info on the index scan over clustered primary key (if any)
  @param  in/out:  best the cheapest index intersect plan discovered so far

  @retval true  the info on the scan is added successfully
  @retval false otherwise

  @notes
  The function adds the first element to this plan P that is supposed to be
  empty. In the case when  cpk_scan is not null the function also checks if
  the intersection with the cpk_scan would produce the plan whose cost is
  less than the cost limit set for any index intersection. If so a reference
  to the used cpk scan is added to the first element. This plan replaces the
  plan in 'best' if its cost it's cost happens to be less than the cost of the
  latter.

  If cpk_scan is not null the intersection of the rowids/pk from the the
  index scan S and interval of cpk_scan is performed on the fly when the
  tuples of S are read. For each tuple the rowid/pk of the tuple is checked
  for the containment in one of the intervals of the cpk_scan. The cost
  of this checking is unrealistically small now if the cpk_scan has several
  elements. This cost now is taken as key_compare_cost instead of
  key_compare_cost*log(#_of_single_point_intervals+2*#_of_other_intervals).
  In the case when the number of intervals of cpk_scan is big enough it
  might happen that more beneficial would be to apply such checking to the
  result of intersection of all scans of the final best plan.
*/

bool Index_intersect_plan::add_first_index_scan(INDEX_SCAN_INFO *idx_scan,
                                                INDEX_SCAN_INFO *cpk_scan,
                                                Index_intersect_plan &best) {
  double cutoff_cost = m_cutoff_cost;
  INDEX_INTERSECT_STEP first_step(idx_scan);
  first_step.m_rows_to_merge = idx_scan->records;
  first_step.m_index_scans_cost = idx_scan->index_scan_cost;
  first_step.m_selectivity = idx_scan->selectivity;
  first_step.m_out_rows = idx_scan->records;
  first_step.m_cost = first_step.m_index_scans_cost;
  first_step.m_cpk_scan = nullptr;
  /*
    Add one rowid/key comparison for each row retrieved by index scan
    (it is done in IndexRangeScanIterator::row_in_ranges)
  */
  const double rid_comp_cost = m_cost_model->key_compare_cost(
                               static_cast<double>(idx_scan->records));
  double idx_scan_cost_with_cpk_filter = first_step.m_index_scans_cost +
                                         rid_comp_cost;
  if (cpk_scan && cutoff_cost <= idx_scan_cost_with_cpk_filter) {
    cpk_scan = nullptr;
  }
  if (cpk_scan) {
    first_step.m_cpk_scan = cpk_scan;
    first_step.m_cost = idx_scan_cost_with_cpk_filter;
    first_step.m_selectivity *= cpk_scan->selectivity;
    first_step.m_out_rows *= cpk_scan->selectivity;
    first_step.m_rows_to_merge = (ha_rows) first_step.m_out_rows;
  }
  m_steps.resize(m_steps.size()+1);
  m_steps[0] = first_step;

  if (cpk_scan) {
    double cost = first_step.m_cost;
    uint key_size = m_param->table->file->ref_length;
    cost +=
      get_cost_of_unique_counted_use_for_intersection(
                                         first_step.m_rows_to_merge,
                                         key_size,
                                         first_step.m_rows_to_merge,
                                         m_max_in_memory_size,
                                         m_cost_model);
    double cutoff = best.get_cost();
    if (cost <= cutoff) {
      Cost_estimate sweep_cost;
      JOIN *join = m_param->query_block->join;
      const bool is_interrupted = join && join->tables != 1;
      get_sweep_read_cost(m_param->table, first_step.m_out_rows,
                          is_interrupted, &sweep_cost);
      cost += sweep_cost.total_cost();
      if (cost < cutoff) {
        first_step.m_cost = cost;
        best.update(*this);
      }
    }
  }
  return true;
}

/**
  @brief
  Check if the extension of this plan with the given index scan makes sense

  @param   ext The info on the index scan for extension

  @retval true if this index intersect plan is successfully extended
  @retval false otherwise

  @note
  The function evaluates the extension of this plan P with the index scan
  specified by ext. If the extension makes the index intersect plan of the size
  equal to 2 and the sum of costs of the used index scans is not less than
  cutoff_cost for P the extension is turned down, otherwise it is accepted.
  If the extension produces an index intersection plan whose size is greater
  than 2 it is accepted only in the case when the result of the extension is
  cheaper than the plan P. If the extension is accepted the info on the plan is
  updated accordingly.
  If the function extends the plan it returns true, otherwise it returns false.
*/
bool Index_intersect_plan::check_index_intersect_ext(INDEX_SCAN_INFO *ext) {

  uint plan_size = m_steps.size();

  INDEX_INTERSECT_STEP ext_step(ext);
  INDEX_INTERSECT_STEP *last_step = &m_steps[plan_size-1];
  /*
    Only those extended plans are accepted whose cost is less than cutoff_cost
  */
  double cutoff_cost = plan_size == 1 ? m_cutoff_cost : get_cost();

  ext_step.m_index_scans_cost = last_step->m_index_scans_cost +
                                ext->index_scan_cost;
  /*
    If addition of the cost of the ext index scan already makes the cost of
    the extended plan too expensive the extension is rejected.
  */
  if (ext_step.m_index_scans_cost >= cutoff_cost) return false;
  ext_step.m_cost = ext_step.m_index_scans_cost ;

  /*
    Assuming that this plan is already an index intersection plan (plan_size>=2)
    turn down the extended index intersection if it's not cheaper than the index
    intersection of this plan.
  */
  ext_step.m_rows_to_merge = last_step->m_rows_to_merge + ext->records;
  uint key_size = m_param->table->file->ref_length;
  ext_step.m_cost +=
    get_cost_of_unique_counted_use_for_intersection(ext_step.m_rows_to_merge,
                                                    key_size,
                                                    m_steps[0].m_rows_to_merge,
                                                    m_max_in_memory_size,
                                                    m_cost_model);
  if (plan_size >= 2 && ext_step.m_cost >= cutoff_cost) return false;

  ext_step.m_selectivity = last_step->m_selectivity * ext->selectivity;
  ext_step.m_out_rows = get_adjusted_out_rows(ext_step.m_selectivity) ;
  Cost_estimate sweep_cost;
  JOIN *join = m_param->query_block->join;
  const bool is_interrupted = join && join->tables != 1;
  get_sweep_read_cost(m_param->table, ext_step.m_out_rows,
                      is_interrupted, &sweep_cost);
  ext_step.m_cost += sweep_cost.total_cost();
  if (ext_step.m_cost >= cutoff_cost) return false;

  m_steps.resize(m_steps.size()+1);
  m_steps[m_steps.size() - 1] = ext_step;

  return true;
}

/**
  @brief  Find the cheapest extension of this index intersection

  @param  idx_scans array of all index scans that can be used in intersection
  @param  start_pos only scans from {idx_scans[start_pos]...} can be used
          when looking for the cheapest extensions of this plan
  @param  cpk_scan info on the index scan over clustered primary key (if any)
  @param  in/out:  best the cheapest index intersect plan discovered so far

  @notes
  The function evaluates all extensions of this plan by index scans from the set
  {idx_scans[start_pos],...,idx_scans[last]} that can be expected to produce
  the cheapest index intersection. Whenever the produced plan P happens to be
  cheaper than the best plan evaluated so far the latter is replaced with P.

  If to call this function with an empty plan P {param,cutoff_cost) it will
  evaluate all simple index intersections involving only two index scans and
  all index intersection formed by the scans referenced in the array
  idx_scans such that their cost is less than cutoff_cost and each index
  scan of which has a positive gain. The cheapest of these index intersections
  will be chosen and placed in 'best'.
  Some other index intersection plans will be evaluated additionally - those
  containing only one index scan with non positive gain and this scan is the
  last one in the evaluated plan.
  (See also the comment on enumeration and pruning of index intersection plans.)
*/

void Index_intersect_plan::find_best_index_intersect_extension(
                              Mem_root_array<INDEX_SCAN_INFO *> *idx_scans,
                              uint start_pos,
                              INDEX_SCAN_INFO *cpk_scan,
                              Index_intersect_plan &best) {

  for (uint i = start_pos; i < idx_scans->size(); i++) {
    bool success = false;
    if (m_steps.size() == 0) {
      success = add_first_index_scan((*idx_scans)[i], cpk_scan, best);
    }
    else {
      if ((success = check_index_intersect_ext((*idx_scans)[i]))) {
        if (get_cost() < best.get_cost()) best.update(*this);
      }
    }
    if (success) {
      find_best_index_intersect_extension(idx_scans, i+1, cpk_scan, best);
      m_steps.pop_back();
    }
  }
}
