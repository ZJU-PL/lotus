

#include <csignal>
#include <unistd.h>

#include "CFL/CSIndex/Tabulation.h"

#include "CFL/CSIndex/CSProgressBar.h"

static bool timeout = false;

static void alarm_handler(int param) {
    timeout = true;
}

Tabulation::Tabulation(Graph &g) : vfg(g) {
}

bool Tabulation::reach(int s, int t) {
    if (visited.count(s))
        return false;

    if (s == t)
        return true;

    visited.insert(s);
    auto& edges = vfg.out_edges(s);
    for (auto successor : edges) {
        if (is_call(s, successor)) {
            // visit the func body
            if (reach_func(successor, t))
                return true;
        } else {
            if (reach(successor, t))
                return true;
        }
    }

    return false;
}

bool Tabulation::reach_func(int s, int t) {
    if (func_visited.count(s))
        return false;
    if (s == t)
        return true;
    func_visited.insert(s);
    auto& edges = vfg.out_edges(s);
    for (auto successor : edges) {
        if (is_return(s, successor)) {
            continue;
        } else {
            if (reach_func(successor, t))
                return true;
        }
    }

    return false;
}

bool Tabulation::is_call(int s, int t) {
    return vfg.label(s, t) > 0;
}

bool Tabulation::is_return(int s, int t) {
    return vfg.label(s, t) < 0;
}

void Tabulation::traverse(int s, std::set<int>& tc) {
    if (visited.count(s))
        return;
    if (timeout)
        return;

    visited.insert(s);
    tc.insert(s);

    auto& edges = vfg.out_edges(s);
    for (auto successor : edges) {
        if (is_call(s, successor)) {
            // visit the func body
            traverse_func(successor, tc);
        } else {
            traverse(successor, tc);
        }
    }
}

void Tabulation::traverse_func(int s, std::set<int>& tc) {
    if (func_visited.count(s))
        return;
    if (timeout)
        return;

    func_visited.insert(s);
    tc.insert(s);

    auto& edges = vfg.out_edges(s);
    for (auto successor : edges) {
        if (is_return(s, successor)) {
            continue;
        } else {
            traverse_func(successor, tc);
        }
    }
}

double Tabulation::tc() {
    signal(SIGALRM, alarm_handler);
    timeout = false;
    alarm(3600 * 6);
    CSProgressBar bar(vfg.num_vertices());

    double ret = 0;
    std::map<int, std::set<int>> tc;
    for (int i = 0; i < vfg.num_vertices(); ++i) {
        visited.clear();
        func_visited.clear();
        traverse(i, tc[i]);
        ret += (tc[i].size()) * sizeof(int);
        bar.update();
    }
    return ret / 1024.0 / 1024.0;
}
