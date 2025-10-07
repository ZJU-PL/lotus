/**
 * @file CNF.cpp
 * @brief Implementation of CNF (Conjunctive Normal Form) representation and solver
 *
 * This file implements functionality for parsing, representing and managing CNF formulas.
 * CNF is a fundamental format for Boolean formulas that allows efficient SAT solving,
 * which can be used as the backend for certain SMT operations.
 *
 * The implementation provides:
 * - Parsing CNF formulas from DIMACS format files
 * - Efficient clause representation and management
 * - Variable and literal tracking
 * - Implication graph construction for conflict analysis
 * - Watch literals for efficient unit propagation
 * - Activity-based decision heuristics
 *
 * This CNF infrastructure forms the foundation for SAT-based solving approaches
 * within the SMT subsystem.
 *
 * Author: rainoftime
 * Creation Date: 2017
 */

#include "Solvers/SMT/CNF.h"
//include <algorithm>
#include <ctype.h>
//#include <set>
#include <stdio.h>
#include <stdlib.h>

#ifdef UPDEBUG
#define DB(x) x
#else
#define DB(x)
#endif

cnf::cnf(char *fname) : m_vc(0), m_cc(0), m_clauses(NULL), m_lc(0) {
  FILE *ifp;
  if ((ifp = fopen(fname, "r")) == NULL) {
    fprintf(stderr, "Cannot open file: %s\n", fname);
    exit(0);
  }

  unsigned j, k, x, clause_index = 0, max_clause_len = 1024;
  int *literals = (int *)malloc(max_clause_len * sizeof(int));

  char line[100000];
  size_t len = 100000;
  char c;

  while ((c = getc(ifp)) != EOF) {
    if (isspace(c))
      continue;
    else
      ungetc(c, ifp);
    fgets(line, len, ifp);
    if (c == 'p') {
      if (sscanf(line, "p cnf %d %d", &m_vc, &m_cc) == 2) {
        m_clauses = (int **)calloc(m_cc, sizeof(int *));
        m_cl = (unsigned *)calloc(m_cc, sizeof(unsigned));
        break;
      } else {
        fprintf(stderr, "Invalid CNF file\n");
        exit(0);
      }
    }
  }

  while ((c = getc(ifp)) != EOF && clause_index < m_cc) {
    if (isspace(c))
      continue;
    else
      ungetc(c, ifp);
    if ((c == '-') || isdigit(c)) {
      for (j = 0; fscanf(ifp, "%d", &(literals[j])), literals[j] != 0;) {
        if (++j == max_clause_len) {
          max_clause_len *= 2;
          literals = (int *)realloc(literals, max_clause_len * sizeof(int));
        }
      }
      m_clauses[clause_index] = (int *)calloc(j + 1, sizeof(int));
      bool skip_clause = false;
      for (k = 0; k <= j;) {
        bool duplicate_literal = false;
        for (x = 0; x < k; x++) {
          if (literals[x] == literals[k]) {
            duplicate_literal = true;
            break;
          } else if (literals[x] + literals[k] == 0) {
            skip_clause = true;
            break;
          }
        }
        if (skip_clause)
          break;
        if (duplicate_literal) {
          literals[k] = literals[--j];
          literals[j] = 0;
        } else {
          m_clauses[clause_index][k] = literals[k];
          k++;
        }
      }
      if (skip_clause)
        free(m_clauses[clause_index]);
      else
        m_lc += (m_cl[clause_index++] = j);
    }
    fgets(line, len, ifp);
  }

  free(literals);
  fclose(ifp);
  if (m_cc > clause_index) {
    m_cc = clause_index;
    m_clauses = (int **)realloc(m_clauses, clause_index * sizeof(int *));
  }
}

cnf::~cnf() {
  if (m_clauses) {
    for (unsigned i = 0; i < m_cc; i++)
      free(m_clauses[i]);
    free(m_clauses);
  }
  free(m_cl);
}

cnf_manager::cnf_manager(cnf &m_cnf) {
  m_vars = new variable[(m_vc = m_cnf.m_vc) + 1];
  m_d_level = 1;
  m_n_decisions = m_n_conflicts = m_n_restarts = 0;
  m_var_order = (unsigned *)calloc(m_vc + 1, sizeof(unsigned));
  m_var_position = (unsigned *)calloc(m_vc + 1, sizeof(unsigned));
  int *zero = m_stack_top = (int *)calloc(m_vc + 1, sizeof(int));

  // implication lists in lieu of watch lists for binary clauses
  // temporary storage
  vector<vector<int>> imp[2];
  imp[0].resize(m_vc + 1);
  imp[1].resize(m_vc + 1);

  // mark bottom of stack with variable 0
  m_vars[*(m_stack_top++) = 0].m_d_level = 0;
  m_vars[0].m_value = _FREE;

  // create lit_pool
  int *p = m_lit_pool = (int *)calloc(
      m_lit_pool_capacity = (m_cnf.m_lc + m_cnf.m_cc) * 2, sizeof(int));
  if (m_lit_pool == NULL) {
    fprintf(stderr, "Unable to allocate %lu bytes of memory\n",
            (long unsigned)m_lit_pool_capacity * sizeof(int));
    printf("s unknown\n");
    exit(0);
  }
  m_lit_pools.push_back(m_lit_pool);

  // populate lit pool
  unsigned i, j;
  for (i = 0; i < m_cnf.m_cc; i++) {
    if (m_cnf.m_clauses[i][1] == 0) { // unit clause
      int lit = m_cnf.m_clauses[i][0];
      if (FREE(lit))
        set_literal(*(m_stack_top++) = lit, zero);
      else if (RESOLVED(lit)) {
        // printf("c contradictory unit m_clauses %d, %d\n", lit, -lit);
        printf("s unsat\n");
        exit(20);
      }
    } else if (m_cnf.m_clauses[i][2] == 0) { // binary clause
      int lit0 = m_cnf.m_clauses[i][0];
      int lit1 = m_cnf.m_clauses[i][1];
      imp[SIGN(lit0)][VAR(lit0)].push_back(lit1);
      imp[SIGN(lit1)][VAR(lit1)].push_back(lit0);
      m_vars[VAR(lit0)].m_activity[SIGN(lit0)]++;
      m_vars[VAR(lit1)].m_activity[SIGN(lit1)]++;
    } else {
      // set up watches
      WATCHLIST(m_cnf.m_clauses[i][0]).push_back(p);
      WATCHLIST(m_cnf.m_clauses[i][1]).push_back(p);
      // copy literals to lit pool
      for (j = 0; (*(p++) = m_cnf.m_clauses[i][j]); j++) {
        m_vars[VAR(*(p - 1))].m_activity[SIGN(*(p - 1))]++;
      }
    }
  }
  m_lit_pool_size = m_lit_pool_size_orig = (p - m_lit_pool);

  // binary clause implication lists
  for (i = 1; i <= m_vc; i++)
    for (j = 0; j <= 1; j++) {
      // last element is 0
      // first three elements are ([], lit, 0)
      // serve as antecedent for all implications
      // serve as conflicting clause with [] filled
      m_vars[i].m_imp[j] = (int *)calloc(imp[j][i].size() + 4, sizeof(int));
      m_vars[i].m_imp[j][1] = i * ((j == _POSI) ? 1 : -1);
      unsigned k = 3;
      for (vector<int>::iterator it = imp[j][i].begin(); it != imp[j][i].end();
           it++) {
        m_vars[i].m_imp[j][k++] = *it;
      }
    }

  // assert unit m_clauses
  assert_unit_clauses();
}

cnf_manager::~cnf_manager() {
  for (vector<int *>::iterator it = m_lit_pools.begin();
       it != m_lit_pools.end(); free(*(it++)))
    ;
  while (*(--m_stack_top))
    ;
  free(m_stack_top);
  for (unsigned i = 1; i <= m_vc; i++)
    for (unsigned j = 0; j <= 1; j++)
      free(m_vars[i].m_imp[j]);
  free(m_var_order);
  free(m_var_position);
  delete[] m_vars;
}

bool cnf_manager::assert_unit_clauses() {
  for (int *p = m_stack_top - 1; *p; p--) {
    int lit = *p;
    *p = *(--m_stack_top);
    if (!assert_literal(lit, m_lit_pool + m_lit_pool_size - 1)) {
      backtrack(m_d_level - 1);
      return false;
    }
  }
  return true;
}

inline void cnf_manager::set_literal(int lit, int *ante) {
  m_vars[VAR(lit)].m_value = SIGN(lit);
  m_vars[VAR(lit)].m_ante = ante;
  m_vars[VAR(lit)].m_d_level = m_d_level;
}

bool cnf_manager::assert_literal(int lit, int *ante) {
  int *newm_stack_top = m_stack_top;
  set_literal(*(newm_stack_top++) = lit, ante);
  DB(printf("%d: %d =>", m_d_level, lit);)

  while (m_stack_top < newm_stack_top) {
    // the literal resolved (as opposed to set)
    int lit = NEG(*(m_stack_top++));

    // implications via binary clauses
    int *imp_list = IMPLIST(lit);
    for (int *imp = imp_list + 3; true; imp++) {
      // implication
      if (FREE(*imp)) {
        if (*imp == 0)
          break; // end of list
        DB(printf(" %d", *imp);)
        set_literal(*(newm_stack_top++) = *imp, imp_list + 1);
        // contradiction
      } else if (RESOLVED(*imp)) {
        DB(printf(" [%d]\n", *imp);)
        m_n_conflicts++;
        m_stack_top = newm_stack_top;
        *imp_list = *imp;       // make up temporary binary clause
        learn_clause(imp_list); // for clause learning purposes
        return false;
      }
    }

    // other implications
    vector<int *> &watch_list = WATCHLIST(lit);
    for (vector<int *>::iterator it = watch_list.begin();
         it != watch_list.end(); it++) {
      // identify the two watched literals
      int *first = *it, *watch, *other_watch;
      if (*first == lit) {
        watch = first;
        other_watch = first + 1;
      } else {
        watch = first + 1;
        other_watch = first;
      }

      // clause satisfied, no need to check further
      if (SET(*other_watch))
        continue;

      // look for free/true literal
      int *p = first + 2;
      bool found = true;
      while (RESOLVED(*p))
        p++;
      if (*p == 0)
        found = false;

      // free/true literal found, swap
      if (found) {
        // watch p
        WATCHLIST(*p).push_back(first);

        // unwatch watch
        *(it--) = watch_list.back();
        watch_list.pop_back();

        // swap literals
        int x = *watch;
        *watch = *p;
        *p = x;

        // free/true variable not found, check other watch
      } else {
        // implication
        if (FREE(*other_watch)) {
          DB(printf(" %d", *other_watch);)
          set_literal(*(newm_stack_top++) = *other_watch, first + 1);

          // move implied literal to beginning of clause
          if (other_watch != first) {
            int x = *other_watch;
            *other_watch = *first;
            *first = x;
          }
          // contradiction
        } else if (RESOLVED(*other_watch)) {
          DB(printf(" [%d]\n", *other_watch);)
          m_n_conflicts++;
          m_stack_top = newm_stack_top;
          learn_clause(first);
          return false;
        }
      }
    }
  }
  DB(printf("\n");)
  return true;
}

void cnf_manager::learn_clause(int *first) {
  // contradiction in level 1, instance unsat
  if (m_d_level == 1) {
    m_a_level = 0;
    return;
  }

  // update var scores and positions
  update_scores(first);

  // clear temporary storage
  m_conflict_lits.clear();
  unsigned cur_level_lits = 0;

  // mark all literals in conflicting clause
  // push to m_tmp_conflict_lits those set prior to current m_d_level
  for (m_tmp_conflict_lits.clear(); *first; first++) {
    // drop known backbone literals
    if (m_vars[VAR(*first)].m_d_level == 1)
      continue;

    if (m_vars[VAR(*first)].m_d_level < m_d_level)
      m_tmp_conflict_lits.push_back(*first);
    else
      cur_level_lits++;
    m_vars[VAR(*first)].m_mark = true;
  }

  // generate 1-UIP conflict clause
  int lit;
  while (true) {
    // pop literal from stack as in backtrack
    lit = *(--m_stack_top);
    unsigned var = VAR(lit);
    m_vars[var].m_value = _FREE;
    if (!m_vars[var].m_mark) {
      if (m_var_position[var] < m_next_var)
        m_next_var = m_var_position[var];
      continue;
    }

    // unmark
    m_vars[var].m_mark = false;

    // if not decision, update scores for the whole m_ante clause
    if (m_vars[var].m_ante)
      update_scores(m_vars[var].m_ante - 1);

    // update m_next_var
    if (m_var_position[var] < m_next_var)
      m_next_var = m_var_position[var];

    // UIP reached
    if (cur_level_lits-- == 1)
      break;

    // else, replace with antecedent (resolution)
    for (int *ante = m_vars[var].m_ante; *ante; ante++) {
      if (m_vars[VAR(*ante)].m_mark || m_vars[VAR(*ante)].m_d_level == 1)
        continue;
      if (m_vars[VAR(*ante)].m_d_level < m_d_level)
        m_tmp_conflict_lits.push_back(*ante);
      else
        cur_level_lits++;
      m_vars[VAR(*ante)].m_mark = true;
    }
  }

  // conflict clause minimization
  // compute assertion level (m_a_level)
  // make sure front of m_conflict_lits is a literal from assertion level
  m_a_level = 1;
  deque<int>::iterator it;
  for (it = m_tmp_conflict_lits.begin(); it != m_tmp_conflict_lits.end();
       it++) {
    bool redundant = true;
    int *ante = m_vars[VAR(*it)].m_ante;
    if (ante == NULL)
      redundant = false;
    else {
      for (; *ante; ante++) {
        if (!m_vars[VAR(*ante)].m_mark) {
          redundant = false;
          break;
        }
      }
    }
    if (!redundant) {
      if (m_vars[VAR(*it)].m_d_level > m_a_level) {
        m_a_level = m_vars[VAR(*it)].m_d_level;
        m_conflict_lits.push_front(*it);
      } else {
        m_conflict_lits.push_back(*it);
      }
    }
  }

  // clear variable marks
  for (it = m_tmp_conflict_lits.begin(); it != m_tmp_conflict_lits.end();
       it++) {
    m_vars[VAR(*it)].m_mark = false;
  }

  // unique lit from current m_d_level pushed last
  m_conflict_lits.push_back(-lit);

  // add clause to litPool and set up watches
  add_clause();

  DB(printf("   [m_a_level: %d]", m_a_level);
     for (deque<int>::iterator it = m_conflict_lits.begin();
          it != m_conflict_lits.end(); it++) printf(" %d", *it);
     printf("\n");)
}

inline void cnf_manager::add_clause() {
  unsigned size = m_conflict_lits.size();

  // create new litPool if necessary
  if (m_lit_pool_size + size + 1 > m_lit_pool_capacity) {
    m_lit_pool_capacity *= 2;
    m_lit_pool = (int *)malloc(m_lit_pool_capacity * sizeof(int));
    while (m_lit_pool == NULL && m_lit_pool_capacity > m_lit_pool_size_orig) {
      m_lit_pool_capacity /= 2;
      m_lit_pool = (int *)malloc(m_lit_pool_capacity * sizeof(int));
    }
    if (m_lit_pool == NULL) {
      printf("c unable to allocate %lu bytes of memory\n",
             (long unsigned)m_lit_pool_capacity * sizeof(int));
      printf("s UNKOWN\n");
      exit(0);
    }
    m_lit_pools.push_back(m_lit_pool);
    m_lit_pool_size = 0;
  }

  // clause starts here
  m_conflict_clause = m_lit_pool + m_lit_pool_size;

  // first literal is the unique literal from current level
  m_lit_pool[m_lit_pool_size++] = m_conflict_lits.back();

  if (size > 1) {
    // add clause to list
    m_clauses.push_back(m_conflict_clause);

    // second literal is one from assertion level
    m_lit_pool[m_lit_pool_size++] = m_conflict_lits.front();

    // set up 2 watches
    WATCHLIST(m_conflict_lits.back()).push_back(m_conflict_clause);
    WATCHLIST(m_conflict_lits.front()).push_back(m_conflict_clause);

    // copy rest of literals to lit pool
    for (unsigned i = 1; i < size - 1;)
      m_lit_pool[m_lit_pool_size++] = m_conflict_lits[i++];
  }

  // end of clause
  m_lit_pool[m_lit_pool_size++] = 0;
}

void cnf_manager::update_scores(int *p) {
  for (; *p; p++) {
    unsigned v = VAR(*p);
    m_vars[v].m_activity[SIGN(*p)]++;
    unsigned it = m_var_position[v];

    // variable already at beginning
    if (it == 0)
      continue;
    unsigned score = SCORE(v);

    // order hasn't been violated
    if (score <= SCORE(m_var_order[it - 1]))
      continue;

    // promote var up the order, using binary search from [zChaff04]
    int step = 0x400, q;
    for (q = ((int)it) - step; q >= 0; q -= step)
      if (SCORE(m_var_order[q]) >= score)
        break;
    for (q += step, step >>= 1; step > 0; step >>= 1) {
      if (q - step >= 0) {
        if (SCORE(m_var_order[q - step]) < score) {
          q -= step;
        }
      }
    }

    // swap it and q
    m_var_order[it] = m_var_order[q];
    m_var_position[v] = q;
    m_var_position[m_var_order[q]] = it;
    m_var_order[q] = v;
  }
}
