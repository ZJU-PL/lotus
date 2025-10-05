/**
 * @file SMTSampler.cpp
 * @brief Implementation of sampling techniques for SMT formulas
 *
 * This file implements two approaches for sampling satisfying models from SMT formulas:
 * 1. quick_sampler: A mutation-based approach that generates diverse models by flipping 
 *    variable assignments and exploring the solution space
 * 2. region_sampler: A bounds-based approach that samples models by determining variable 
 *    bounds and randomly selecting values within those bounds
 *
 * These sampling techniques are useful for:
 * - Test case generation
 * - Exploring the solution space of constraints
 * - Finding diverse satisfying assignments
 * - Analyzing the sensitivity of formulas to variable changes
 *
 * The implementation focuses on efficiency and diversity of the generated models.
 */

#include "Solvers/SMT/SMTSampler.h"

class quick_sampler {
  std::string input_file;

  struct timespec start_time;
  double solver_time = 0.0;
  int max_samples;
  double max_time;

  z3::context c;
  z3::optimize opt;
  std::vector<int> ind;
  std::unordered_set<int> unsat_vars;
  int epochs = 0;
  int flips = 0;
  int samples = 0;
  int solver_calls = 0;

  std::ofstream results_file;

public:
  quick_sampler(std::string input, int max_samples, double max_time)
      : input_file(input), max_samples(max_samples), max_time(max_time),
        opt(c) {}

  void run() {
    clock_gettime(CLOCK_REALTIME, &start_time);
    srand(start_time.tv_sec);
    parse_cnf();
    results_file.open(input_file + ".samples");
    while (true) {
      opt.push();
      for (int v : ind) {
        if (rand() % 2)
          opt.add(literal(v), 1);
        else
          opt.add(!literal(v), 1);
      }
      if (!solve())
        break;
      z3::model m = opt.get_model();
      opt.pop();

      sample(m);
      print_stats(false);
    }
  }

  void print_stats(bool simple) {
    struct timespec end;
    clock_gettime(CLOCK_REALTIME, &end);
    double elapsed = duration(&start_time, &end);
    std::cout << "Samples " << samples << '\n';
    std::cout << "Execution time " << elapsed << '\n';
    if (simple)
      return;
    std::cout << "Solver time: " << solver_time << '\n';
    std::cout << "Epochs " << epochs << ", Flips " << flips << ", Unsat "
              << unsat_vars.size() << ", Calls " << solver_calls << '\n';
  }

  void parse_cnf() {
    z3::expr_vector exp(c);
    std::ifstream f(input_file);
    if (!f.is_open()) {
      std::cout << "Error opening input file\n";
      abort();
    }
    std::unordered_set<int> indset;
    bool has_ind = false;
    int max_var = 0;
    std::string line;
    while (getline(f, line)) {
      std::istringstream iss(line);
      if (line.find("c ind ") == 0) {
        std::string s;
        iss >> s;
        iss >> s;
        int v;
        while (!iss.eof()) {
          iss >> v;
          if (v && indset.find(v) == indset.end()) {
            indset.insert(v);
            ind.push_back(v);
            has_ind = true;
          }
        }
      } else if (line[0] != 'c' && line[0] != 'p') {
        z3::expr_vector clause(c);
        int v;
        while (!iss.eof()) {
          iss >> v;
          if (v > 0)
            clause.push_back(literal(v));
          else if (v < 0)
            clause.push_back(!literal(-v));
          v = abs(v);
          if (!has_ind && v != 0)
            indset.insert(v);
          if (v > max_var)
            max_var = v;
        }
        exp.push_back(mk_or(clause));
      }
    }
    f.close();
    if (!has_ind) {
      for (int lit = 0; lit <= max_var; ++lit) {
        if (indset.find(lit) != indset.end()) {
          ind.push_back(lit);
        }
      }
    }
    z3::expr formula = mk_and(exp);
    opt.add(formula);
  }

  void sample(z3::model& m) {
    std::unordered_set<std::string> initial_mutations;
    std::string m_string = model_string(m);
    std::cout << m_string << " STARTING\n";
    output(m_string, 0);
    opt.push();
    for (unsigned i = 0; i < ind.size(); ++i) {
      int v = ind[i];
      if (m_string[i] == '1')
        opt.add(literal(v), 1);
      else
        opt.add(!literal(v), 1);
    }

    std::unordered_map<std::string, int> mutations;
    for (unsigned i = 0; i < ind.size(); ++i) {
      if (unsat_vars.find(i) != unsat_vars.end())
        continue;
      opt.push();
      int v = ind[i];
      if (m_string[i] == '1')
        opt.add(!literal(v));
      else
        opt.add(literal(v));
      if (solve()) {
        z3::model new_model = opt.get_model();
        std::string new_string = model_string(new_model);
        if (initial_mutations.find(new_string) == initial_mutations.end()) {
          initial_mutations.insert(new_string);
          // std::cout << new_string << '\n';
          std::unordered_map<std::string, int> new_mutations;
          new_mutations[new_string] = 1;
          output(new_string, 1);
          flips += 1;
          for (auto &it : mutations) {
            if (it.second >= 6)
              continue;
            std::string candidate;
            for (unsigned j = 0; j < ind.size(); ++j) {
              bool a = m_string[j] == '1';
              bool b = it.first[j] == '1';
              bool c = new_string[j] == '1';
              if (a ^ ((a ^ b) | (a ^ c)))
                candidate += '1';
              else
                candidate += '0';
            }
            if (mutations.find(candidate) == mutations.end() &&
                new_mutations.find(candidate) == new_mutations.end()) {
              new_mutations[candidate] = it.second + 1;
              output(candidate, it.second + 1);
            }
          }
          for (auto& it : new_mutations) {
            mutations[it.first] = it.second;
          }
        } else {
          // std::cout << new_string << " repeated\n";
        }
      } else {
        std::cout << "unsat\n";
        unsat_vars.insert(i);
      }
      opt.pop();
      print_stats(true);
    }
    epochs += 1;
    opt.pop();
  }

  void output(std::string& sample, int nmut) {
    samples += 1;
    results_file << nmut << ": " << sample << '\n';
  }

  void finish() {
    print_stats(false);
    results_file.close();
    std::cout << "samples file closed!!!\n";
    exit(0);
  }

  bool solve() {
    struct timespec start;
    clock_gettime(CLOCK_REALTIME, &start);
    double elapsed = duration(&start_time, &start);
    if (elapsed > max_time) {
      std::cout << "Stopping: timeout\n";
      finish();
    }
    if (samples >= max_samples) {
      std::cout << "Stopping: samples\n";
      finish();
    }

    z3::check_result result = opt.check();
    struct timespec end;
    clock_gettime(CLOCK_REALTIME, &end);
    solver_time += duration(&start, &end);
    solver_calls += 1;

    return result == z3::sat;
  }

  std::string model_string(z3::model& model) {
    std::string s;

    for (int v : ind) {
      z3::func_decl decl(literal(v).decl());
      z3::expr b = model.get_const_interp(decl);
      if (b.bool_value() == Z3_L_TRUE) {
        s += "1";
      } else {
        s += "0";
      }
    }
    return s;
  }

  double duration(struct timespec *a, struct timespec *b) {
    return (b->tv_sec - a->tv_sec) + 1.0e-9 * (b->tv_nsec - a->tv_nsec);
  }

  z3::expr literal(int v) {
    return c.constant(c.str_symbol(std::to_string(v).c_str()), c.bool_sort());
  }
};

struct region_sampler {
  std::string path;
  std::string input_file;
  std::vector<std::string> input_files;
  //  bool filemode;

  struct timespec start_time;
  double solver_time = 0.0;
  double check_time = 0.0;
  int max_samples;
  double max_time;

  int m_samples = 0;
  int m_success = 0;
  int m_unique = 0;
  double m_sample_time = 0.0;

  z3::context c;
  z3::expr smt_formula;
  z3::expr_vector m_vars;

  // std::vector<z3::expr> smt_formulas;
  std::vector<int> lower_bounds;
  std::vector<int> upper_bounds;
  std::vector<bool> should_fix;

  std::vector<std::vector<int>> unique_models;

  region_sampler(std::string& input, int max_samples, double max_time)
      : path(input), max_samples(max_samples), max_time(max_time),
        smt_formula(c), m_vars(c) {
    struct stat info;
    if (stat(path.c_str(), &info) != 0) {
      if (info.st_mode & S_IFDIR) {
        DIR *dirp = opendir(input.c_str());
        struct dirent *dp;
        while ((dp = readdir(dirp)) != NULL) {
          std::string tmp(dp->d_name);
          input_files.push_back(tmp);
        }
      } else {
        input_files.push_back(input);
      }
    }
    input_file = input;
  }

  void parse_smt() {
    expr_vector evec = c.parse_file(input_file.c_str());
    smt_formula = mk_and(evec);
  }

  void get_bounds() {
    params p(c);
    p.set("priority", c.str_symbol("box"));
    p.set("timeout", (unsigned)15000); // 15 seconds
    // 'core_maxsat', 'wmax', 'maxres', 'pd-maxres' ?
    // p.set("maxsat_engine", c.str_symbol("maxres"));

    optimize opt_sol_min(c);
    opt_sol_min.set(p);
    opt_sol_min.add(smt_formula);

    optimize opt_sol_max(c);
    opt_sol_max.set(p);
    opt_sol_max.add(smt_formula);

    // Find min
    std::vector<optimize::handle> handlers_min;
    for (unsigned i = 0; i < m_vars.size(); i++) {
      handlers_min.push_back(opt_sol_min.minimize(m_vars[i]));
    }
    auto min_res = opt_sol_min.check();
    for (unsigned i = 0; i < m_vars.size(); i++) {
      // std::cout << m_vars[i] <<": " << opt_sol_min.upper(handlers_min[i]) <<
      // "\n";
      if (min_res == sat) {
        lower_bounds.push_back(
            opt_sol_min.upper(handlers_min[i]).get_numeral_int());
      } else {
        lower_bounds.push_back(0);
      }
    }

    // Find max
    std::vector<optimize::handle> handlers_max;
    for (unsigned i = 0; i < m_vars.size(); i++) {
      handlers_max.push_back(opt_sol_max.maximize(m_vars[i]));
    }
    auto max_res = opt_sol_max.check();
    for (unsigned i = 0; i < m_vars.size(); i++) {
      // std::cout << m_vars[i] <<": " << opt_sol_max.lower(handlers_max[i]) <<
      // "\n";
      if (max_res == sat) {
        upper_bounds.push_back(
            opt_sol_max.lower(handlers_max[i]).get_numeral_int());
      } else {
        unsigned sz = m_vars[i].get_sort().bv_size();
        // max integer number of size sz
        upper_bounds.push_back((1 << sz) - 1);
      }
    }
  }

  std::vector<int> sample_once() {
    m_samples++;
    std::vector<int> sample;
    for (unsigned i = 0; i < m_vars.size(); i++) {
      if (should_fix[i]) {
        sample.push_back(lower_bounds[i]);
      } else {
        int output =
            lower_bounds[i] +
            (rand() % static_cast<int>(upper_bounds[i] - lower_bounds[i] + 1));
        sample.push_back(output);
      }
    }
    return sample;
  }

  bool check_random_model(std::vector<int> &assignments) {
    model rand_model(c);
    for (unsigned i = 0; i < m_vars.size(); i++) {
      z3::func_decl decl = m_vars[i].decl();
      z3::expr val_i = c.bv_val(assignments[i], m_vars[i].get_sort().bv_size());
      rand_model.add_const_interp(decl, val_i);
    }

    if (rand_model.eval(smt_formula).is_true()) {
      if (unique_models.size() == 0) {
        unique_models.push_back(assignments);
        return true;
      }
      bool is_unique = true;
      for (unsigned i = 0; i < unique_models.size(); i++) {
        std::vector<int> model = unique_models[i];
        bool same_model = true;
        for (unsigned j = 0; j < m_vars.size(); j++) {
          if (model[j] != assignments[j]) {
            same_model = false;
            break;
          }
        }
        if (same_model) {
          is_unique = false;
          break;
        }
      }
      if (is_unique) {
        m_unique++;
        unique_models.push_back(assignments);
      }
      return true;
    } else {
      return false;
    }
  }

  void run() {
    //        clock_gettime(CLOCK_REALTIME, &start_time);
    //        srand(start_time.tv_sec);
    // parse_cnf();
    for (auto& file : input_files) {
      input_file = file;
      parse_smt();
      std::cout << "parse finished... \n";

      get_expr_vars(smt_formula, m_vars);
      std::cout << "get vars finished... start counting\n";

      auto init = std::chrono::high_resolution_clock::now();
      //        struct timespec start;
      //        clock_gettime(CLOCK_REALTIME, &start);
      get_bounds();
      for (unsigned i = 0; i < m_vars.size(); i++) {
        if (lower_bounds[i] == upper_bounds[i]) {
          should_fix.push_back(true);
        } else {
          should_fix.push_back(false);
        }
      }
      std::cout << "get bounds finished...\n";

      auto finish = std::chrono::high_resolution_clock::now();
      //        struct timespec end;
      //
      //
      //        clock_gettime(CLOCK_REALTIME, &end);
      solver_time +=
          std::chrono::duration<double, std::milli>(finish - init).count();

      init = std::chrono::high_resolution_clock::now();
      for (int i = 0; i < max_samples; i++) {
        if (i % 5000 == 0)
          print_stats();
        //            struct timespec samp;
        //            clock_gettime(CLOCK_REALTIME, &samp);
        auto iter_start = std::chrono::high_resolution_clock::now();
        double elapsed =
            std::chrono::duration<double, std::milli>(iter_start - init)
                .count();
        if (elapsed >= max_time) {
          std::cout << "Stopping: timeout\n";
          this->finish();
        }

        std::vector<int> sample = sample_once();
        if (check_random_model(sample)) {
          m_success++;
        }
        finish = std::chrono::high_resolution_clock::now();
        m_sample_time +=
            std::chrono::duration<double, std::milli>(finish - init).count();
      }
    }
  }

  double duration(struct timespec *a, struct timespec *b) {
    return (b->tv_sec - a->tv_sec) + 1.0e-9 * (b->tv_nsec - a->tv_nsec);
  }

  void finish() {
    std::cout << "-------Good Bye!-----\n";
    print_stats();
    // TODO: write results to some file
    exit(0);
  }

  void reset_state() {
    solver_time = 0;
    m_sample_time = 0;
    m_samples = 0;
    m_success = 0;
    unique_models.clear();
  }

  void print_stats() {

    std::cout << "solver time: " << solver_time << "\n";
    std::cout << "sample total time: " << m_sample_time << "\n";
    std::cout << "samples number: " << m_samples << "\n";
    std::cout << "samples success: " << m_success << "\n";
    std::cout << "unique modesl: " << unique_models.size() << "\n";
    std::cout << "------------------------------------------\n";

    std::ofstream of("res.log", std::ofstream::app);
    of << "solver time: " << solver_time << "\n";
    of << "sample total time: " << m_sample_time << "\n";
    of << "samples number: " << m_samples << "\n";
    of << "samples success: " << m_success << "\n";
    of << "unique modesl: " << unique_models.size() << "\n";
    of << "------------------------------------------\n";

    of.close();
  }
};
