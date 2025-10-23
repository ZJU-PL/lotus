#include "Solvers/SMT/SMTLibOptimizer.h"

#include <iostream>
#include <fstream>
#include <sstream>

SMTLibOptimizer::SMTLibOptimizer(z3::context& context, const std::string& smt_formula,
                                bool verbose, bool full_model)
    : ctx(context), smt_formula(smt_formula), verbose(verbose), full_model(full_model) {}

void SMTLibOptimizer::setFormula(const std::string& formula) {
    smt_formula = formula;
}

bool SMTLibOptimizer::findVariable(const std::string& var_name, VariableInfo& var_info) {
    std::istringstream iss(smt_formula);
    std::string line;

    while (std::getline(iss, line)) {
        if (line.find("declare-fun") == std::string::npos) continue;

        std::istringstream line_iss(line);
        std::vector<std::string> tokens;
        std::string token;

        while (line_iss >> token) tokens.push_back(token);

        if (tokens.size() >= 4 && tokens[0] == "(declare-fun" && tokens[1] == var_name) {
            var_info.name = var_name;
            var_info.found = true;

            if (tokens.size() > 3) {
                std::string sort_str = tokens.back();
                if (sort_str.back() == ')') sort_str.pop_back();
                var_info.sort = sort_str;
            }

            var_info.min_value = LLONG_MIN;
            var_info.max_value = LLONG_MAX;
            return true;
        }
    }
    return false;
}

z3::check_result SMTLibOptimizer::checkWithConstraint(const VariableInfo& var_info, long long value) {
    z3::solver solver(ctx);

    try {
        z3::expr_vector formulas = ctx.parse_string(smt_formula.c_str());
        z3::expr var_expr = ctx.int_const(var_info.name.c_str());
        z3::expr constraint = var_expr >= ctx.int_val(static_cast<int64_t>(value));

        for (unsigned i = 0; i < formulas.size(); ++i) solver.add(formulas[i]);
        solver.add(constraint);

        if (verbose) std::cout << "Checking: " << var_info.name << " >= " << value << '\n';
        return solver.check();
    } catch (const z3::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return z3::check_result::unknown;
    }
}

long long SMTLibOptimizer::binarySearchMax(const VariableInfo& var_info, long long max_bound) {
    long long min_val = 0, max_val = max_bound > 0 ? max_bound : 1000000;

    while (min_val <= max_val) {
        long long mid = min_val + (max_val - min_val) / 2;
        z3::check_result result = checkWithConstraint(var_info, mid);

        if (result == z3::check_result::sat) {
            min_val = mid + 1;
            if (verbose) std::cout << "SAT: " << var_info.name << " >= " << mid << '\n';
        } else {
            max_val = mid - 1;
            if (verbose) std::cout << (result == z3::check_result::unsat ? "UNSAT" : "UNKNOWN")
                                   << ": " << var_info.name << " >= " << mid << '\n';
        }
    }
    return max_val;
}

void SMTLibOptimizer::printSolution(const VariableInfo& var_info, long long solution, OptimizationResult result) {
    std::string msg = (result == OPT_SAT) ? "The maximum value of " :
                     (result == OPT_UNKNOWN) ? "The probable maximum value of " : "No solution found for ";
    std::cout << msg << var_info.name << " is " << solution << "." << '\n';
}

SMTLibOptimizer::OptimizationResult SMTLibOptimizer::optimizeVariable(const std::string& var_name, long long& result) {
    VariableInfo var_info = {};
    if (!findVariable(var_name, var_info)) {
        std::cerr << "Error: Variable " << var_name << " not found" << '\n';
        return OPT_ERROR;
    }

    if (verbose) std::cout << "Optimizing: " << var_name << '\n';

    long long max_value = binarySearchMax(var_info);
    result = max_value;

    z3::check_result check_result = checkWithConstraint(var_info, max_value);
    OptimizationResult opt_result = (check_result == z3::check_result::sat) ? OPT_SAT :
                                   (check_result == z3::check_result::unknown) ? OPT_UNKNOWN : OPT_UNSAT;

    printSolution(var_info, max_value, opt_result);
    return opt_result;
}

bool SMTLibOptimizer::optimizeCuts(const std::string& cuts_file) {
    std::ifstream file(cuts_file.c_str());
    if (!file) {
        std::cerr << "Error: Cannot open cuts file: " << cuts_file << '\n';
        return false;
    }

    std::string line;
    int processed = 0, total = 0;

    while (std::getline(file, line)) {
        std::istringstream iss(line);
        std::string cut_name;
        int max_bound;

        if (!(iss >> cut_name >> max_bound)) continue;

        total++;
        if (verbose) std::cout << "Processing cut: " << cut_name << '\n';

        VariableInfo var_info = {};
        if (findVariable(cut_name, var_info)) {
            processed++;
            if (verbose) std::cout << "Cut " << processed << "/" << total << ": " << cut_name << '\n';

            long long solution = binarySearchMax(var_info, max_bound);
            if (verbose) std::cout << "Cost: " << solution << '\n';
        } else if (verbose) {
            std::cout << "Warning: Cut variable " << cut_name << " not found" << '\n';
        }
    }

    if (verbose) std::cout << "Processed " << processed << "/" << total << " cuts" << '\n';
    return processed > 0;
}

bool SMTLibOptimizer::isValidFormula() {
    if (smt_formula.empty()) return false;

    try {
        ctx.parse_string(smt_formula.c_str());
        return true;
    } catch (const z3::exception&) {
        return false;
    }
}
