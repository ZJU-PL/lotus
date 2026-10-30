#ifndef TREE_DEPTH_DECOMPOSITION_H
#define TREE_DEPTH_DECOMPOSITION_H

#include "filter.h"
#include "greedy_order.h"
#include "id_func.h"
#include "id_multi_func.h"
#include "min_max.h"
#include "multi_arc.h"
#include "permutation.h"
#include "preorder.h"
#include "tiny_id_func.h"
#include "tree_node_ranking.h"
#include "tree_root.h"
#include <string>
#include <vector>

int compute_tree_depth_of_parent_array(const ArrayIDFunc<int>& parent);

int compute_tree_depth_of_order(const ArrayIDIDFunc& tail,
    const ArrayIDIDFunc& head,
    const ArrayIDIDFunc& elimination_order);

std::string format_parent_array(const ArrayIDFunc<int>& parent, int depth);

ArrayIDFunc<int> compute_parent_array_from_elimination_order(
    const ArrayIDIDFunc& tail, const ArrayIDIDFunc& head,
    const ArrayIDIDFunc& order);

inline BitIDFunc compute_in_set_function(int node_count,
    const std::vector<int>& set)
{
#ifndef NDEBUG
    for (int x : set)
        assert(0 <= x && x < node_count);
#endif

    BitIDFunc in(node_count);
    in.fill(false);
    for (int x : set) {
        assert(!in(x));
        in.set(x, true);
    }
    return in;
}

// tail, head, node_set, is_node_in_node_set use the same ids
template <class InNodeSet>
void inplace_remove_arcs_incident_to_node_set(
    ArrayIDIDFunc& tail, ArrayIDIDFunc& head,
    const InNodeSet& is_node_in_node_set)
{
    assert(tail.preimage_count() == head.preimage_count());
    assert(tail.image_count() == head.image_count());
    assert(tail.image_count() == is_node_in_node_set.preimage_count());
    assert(is_symmetric(tail, head));

    const int arc_count = tail.preimage_count();

    BitIDFunc keep_arc_flag = id_func(arc_count, [&](int a) {
        return !is_node_in_node_set(tail(a)) && !is_node_in_node_set(head(a));
    });

    int new_arc_count = count_true(keep_arc_flag);

    tail = keep_if(keep_arc_flag, new_arc_count, std::move(tail));
    head = keep_if(keep_arc_flag, new_arc_count, std::move(head));

    assert(tail.preimage_count() == head.preimage_count());
    assert(tail.image_count() == head.image_count());
    assert(tail.image_count() == is_node_in_node_set.preimage_count());
    assert(is_symmetric(tail, head));
}

// tail, head, node_set, is_node_in_node_set use local ids
// input_node_id maps local ids into global ids
template <class InNodeSet>
void inplace_remove_nodes_incident_to_node_set(
    ArrayIDIDFunc& tail, ArrayIDIDFunc& head, ArrayIDIDFunc& input_node_id,
    const InNodeSet& is_node_in_node_set)
{
    assert(tail.preimage_count() == head.preimage_count());
    assert(tail.image_count() == head.image_count());
    assert(tail.image_count() == is_node_in_node_set.preimage_count());
    assert(is_symmetric(tail, head));

    inplace_remove_arcs_incident_to_node_set(tail, head, is_node_in_node_set);

    const int node_count = tail.image_count();

    auto keep_node_flag = id_func(node_count, [&](int v) { return !is_node_in_node_set(v); });

    int new_node_count = count_true(keep_node_flag);

    ArrayIDIDFunc keep_function = compute_keep_function(keep_node_flag, new_node_count);

    tail = chain(std::move(tail), keep_function);
    head = chain(std::move(head), keep_function);
    input_node_id = keep_if(keep_node_flag, new_node_count, std::move(input_node_id));

    assert(tail.preimage_count() == head.preimage_count());
    assert(tail.image_count() == head.image_count());
    assert(is_symmetric(tail, head));
}

// tail, head use local ids
// input_node_id maps local ids into global ids
inline void inplace_reorder_nodes_and_arc_in_preorder(
    ArrayIDIDFunc& tail, ArrayIDIDFunc& head, ArrayIDIDFunc& input_node_id)
{
    assert(tail.preimage_count() == head.preimage_count());
    assert(tail.image_count() == head.image_count());
    assert(tail.image_count() == input_node_id.preimage_count());
    assert(is_symmetric(tail, head));

    auto preorder = compute_preorder(compute_successor_function(tail, head));
    auto inv_preorder = inverse_permutation(preorder);
    tail = chain(std::move(tail), inv_preorder);
    head = chain(std::move(head), inv_preorder);
    input_node_id = chain(std::move(preorder), std::move(input_node_id));

    auto p = sort_arcs_first_by_tail_second_by_head(tail, head);
    tail = chain(p, std::move(tail));
    head = chain(p, std::move(head));

    assert(tail.preimage_count() == head.preimage_count());
    assert(tail.image_count() == head.image_count());
    assert(tail.image_count() == input_node_id.preimage_count());
    assert(is_symmetric(tail, head));
}

inline void inplace_reorder_nodes_and_arc_in_preorder(ArrayIDIDFunc& tail,
    ArrayIDIDFunc& head)
{
    assert(tail.preimage_count() == head.preimage_count());
    assert(tail.image_count() == head.image_count());
    assert(is_symmetric(tail, head));

    auto preorder = compute_preorder(compute_successor_function(tail, head));
    auto inv_preorder = inverse_permutation(preorder);
    tail = chain(std::move(tail), inv_preorder);
    head = chain(std::move(head), inv_preorder);

    auto p = sort_arcs_first_by_tail_second_by_head(tail, head);
    tail = chain(p, std::move(tail));
    head = chain(p, std::move(head));

    assert(tail.preimage_count() == head.preimage_count());
    assert(tail.image_count() == head.image_count());
    assert(is_symmetric(tail, head));
}

// tail, head use local ids
// input_node_id maps local ids into global ids
template <class Tail, class Head, class InputNodeID, class Callback>
void forall_connected_components_with_nodes_and_arcs_in_preorder(
    const Tail& tail, const Head& head, const InputNodeID& input_node_id,
    const Callback& callback)
{
    assert(tail.preimage_count() == head.preimage_count());
    assert(tail.image_count() == head.image_count());
    assert(tail.image_count() == input_node_id.preimage_count());
    assert(is_symmetric(tail, head));

    const int node_count = tail.image_count();
    const int arc_count = tail.preimage_count();

    BitIDFunc component_begin(node_count);
    component_begin.fill(true);
    for (int i = 0; i < arc_count; ++i) {
        if (head(i) < tail(i)) {
            component_begin.set(tail(i), false);
        }
    }

    auto on_new_component = [&](int node_begin, int node_end, int arc_begin,
                                int arc_end) {
        auto sub_node_count = node_end - node_begin;
        auto sub_arc_count = arc_end - arc_begin;

        auto sub_tail = id_id_func(sub_arc_count, sub_node_count, [&](int x) {
            return tail(arc_begin + x) - node_begin;
        });
        auto sub_head = id_id_func(sub_arc_count, sub_node_count, [&](int x) {
            return head(arc_begin + x) - node_begin;
        });
        auto sub_input_node_id = id_id_func(sub_node_count, input_node_id.image_count(),
            [&](int x) { return input_node_id(node_begin + x); });
        return callback(sub_tail, sub_head, sub_input_node_id);
    };

    int node_begin = 0;
    int arc_begin = 0;

    for (int node_end = 1; node_end < node_count; ++node_end) {
        if (component_begin(node_end)) {
            int arc_end = arc_begin;
            while (arc_end < arc_count && tail(arc_end) < node_end) {
                ++arc_end;
            }

            if(!on_new_component(node_begin, node_end, arc_begin, arc_end))
                return;

            node_begin = node_end;
            arc_begin = arc_end;
        }
    }
    on_new_component(node_begin, node_count, arc_begin, arc_count);
}

template <class Tail, class Head>
ArrayIDIDFunc compute_tree_depth_order_of_tree(const Tail& tail,
    const Head& head)
{
    const int node_count = tail.image_count();
    const int arc_count = tail.preimage_count();
    (void)arc_count;
    assert(tail.preimage_count() == head.preimage_count());
    assert(tail.image_count() == head.image_count());
    assert(is_symmetric(tail, head));
    assert(2 * (node_count - 1) == arc_count);

    ArrayIDIDFunc level = compute_tree_node_ranking(compute_successor_function(tail, head));
    ArrayIDIDFunc order = identity_permutation(node_count);
    std::sort(order.begin(), order.end(),
        [&](int l, int r) { return level[l] < level[r]; });
    return order;
}

inline ArrayIDIDFunc
compute_separator_node_order(const std::vector<int>& separator_node_depth)
{
    ArrayIDIDFunc order = identity_permutation(separator_node_depth.size());
    std::sort(order.begin(), order.end(), [&](int l, int r) {
        return separator_node_depth[l] < separator_node_depth[r];
    });
    return order;
}

template <class ComputeOrderOfPart>
ArrayIDIDFunc compute_nested_disection_order_by_splitting_along_separator(
    const ArrayIDIDFunc& tail, const ArrayIDIDFunc& head,
    const std::vector<int>& separator, const ComputeOrderOfPart& compute_order_of_part)
{
    const int arc_count = tail.preimage_count();
    const int node_count = tail.image_count();
    const int separator_size = separator.size();
    (void)arc_count;

    assert(arc_count == head.preimage_count());
    assert(node_count == head.image_count());
    assert(is_symmetric(tail, head));

    ArrayIDIDMultiFunc successor = compute_successor_function(tail, head);

    BitIDFunc is_in_separator = compute_in_set_function(node_count, separator);

    ArrayIDIDFunc sub_tail = tail, sub_head = head,
                  sub_to_super = identity_permutation(node_count);

    inplace_remove_nodes_incident_to_node_set(sub_tail, sub_head, sub_to_super,
        is_in_separator);

    assert(sub_tail.preimage_count() == sub_head.preimage_count());
    assert(sub_tail.image_count() == sub_head.image_count());
    assert(sub_tail.image_count() == sub_to_super.preimage_count());
    assert(is_symmetric(tail, head));

    inplace_reorder_nodes_and_arc_in_preorder(sub_tail, sub_head, sub_to_super);

    assert(sub_tail.preimage_count() == sub_head.preimage_count());
    assert(sub_tail.image_count() == sub_head.image_count());
    assert(sub_tail.image_count() == sub_to_super.preimage_count());
    assert(is_symmetric(tail, head));

    ArrayIDFunc<int> depth(node_count);
    depth.fill(1);

    ArrayIDIDFunc order(node_count, node_count);
    int order_end = 0;

    forall_connected_components_with_nodes_and_arcs_in_preorder(
        sub_tail, sub_head, sub_to_super,
        [&](ArrayIDIDFunc comp_tail, ArrayIDIDFunc comp_head,
            ArrayIDIDFunc comp_to_super) {
            int comp_node_count = comp_tail.image_count();
            ArrayIDIDFunc comp_order = compute_order_of_part(comp_tail, comp_head);
            if(comp_order.preimage_count() == 0){
                order = ArrayIDIDFunc();
                return false;
            }

            for (int i = 0; i < comp_node_count; ++i)
                order[order_end++] = comp_to_super[comp_order[i]];
            int comp_depth = compute_tree_depth_of_order(comp_tail, comp_head, comp_order);

            for (int i = 0; i < comp_node_count; ++i)
                max_to(depth[comp_to_super[i]], comp_depth);
            return true;
        });

    if(order.preimage_count() != 0){
        std::vector<int> separator_node_depth(separator_size);

        for (int i = 0; i < separator_size; ++i) {
            int x = separator[i];
            int d = 0;
            for (int y : successor(x)) {
                if (depth[y] > d)
                    d = depth[y];
            }
            separator_node_depth[i] = d;
        }

        ArrayIDIDFunc sep_order = compute_separator_node_order(separator_node_depth);
        for (int i = 0; i < separator_size; ++i) {
            order[order_end++] = separator[sep_order(i)];
        }

        assert(order_end == node_count);
        assert(is_permutation(order));
    }
    return order;
}

template <class ComputeSeparator>
ArrayIDIDFunc compute_tree_depth_order_of_connected_graph(
    ArrayIDIDFunc tail, ArrayIDIDFunc head,
    const ComputeSeparator& compute_separator,
    int tree_depth_must_be_below)
{
    assert(tail.preimage_count() == head.preimage_count());
    assert(tail.image_count() == head.image_count());
    assert(is_symmetric(tail, head));

    const int node_count = tail.image_count();
    const int arc_count = tail.preimage_count();
    bool is_tree = (arc_count == 2 * (node_count - 1));
    bool is_clique = (arc_count == (node_count) * (node_count - 1));

    if (is_tree) {
        return compute_tree_depth_order_of_tree(std::move(tail), std::move(head));
    } else if (is_clique) {
        return identity_permutation(node_count);
    } else {
        ArrayIDIDFunc best_order = compute_greedy_order(tail, head);
        int best_order_depth = compute_tree_depth_of_order(tail, head, best_order);

        // If we computed a separator with size tree_depth_must_be_below or more, then the tree depth would also be at least tree_depth_must_be_below as the separator forms a path.
        // If we computed a separator with size best_order_depth or more, then it cannot be better than best_order_depth as the separator forms a path.
        std::vector<int> separator = compute_separator(tail, head, std::min(tree_depth_must_be_below, best_order_depth) - 1);
        if (!separator.empty()) {
            ArrayIDIDFunc nd_order = compute_nested_disection_order_by_splitting_along_separator(
                tail, head, separator,
                [&](ArrayIDIDFunc sub_tail, ArrayIDIDFunc sub_head) {
                    return compute_tree_depth_order_of_connected_graph(std::move(sub_tail), std::move(sub_head), compute_separator, std::min(tree_depth_must_be_below, best_order_depth) - 1);
                });
            if(nd_order.preimage_count() != 0){
                int nd_order_depth = compute_tree_depth_of_order(tail, head, nd_order);
                if (nd_order_depth < best_order_depth) {
                    best_order = std::move(nd_order);
                    best_order_depth = nd_order_depth;
                }
            }
        }

        if(best_order_depth > tree_depth_must_be_below)
            return ArrayIDIDFunc();
        else
            return best_order;
    }
}

template <class ComputeSeparator>
ArrayIDIDFunc compute_tree_depth_order(
    ArrayIDIDFunc tail, ArrayIDIDFunc head,
    const ComputeSeparator& compute_separator,
    int tree_depth_must_be_below)
{
    assert(tail.preimage_count() == head.preimage_count());
    assert(tail.image_count() == head.image_count());
    assert(is_symmetric(tail, head));

    const int node_count = tail.image_count();
    const int arc_count = tail.preimage_count();
    (void)arc_count;

    ArrayIDIDFunc order(node_count, node_count);
    int order_end = 0;

    ArrayIDIDFunc to_input_id = identity_permutation(node_count);
    inplace_reorder_nodes_and_arc_in_preorder(tail, head, to_input_id);
    forall_connected_components_with_nodes_and_arcs_in_preorder(
        tail, head, identity_permutation(node_count),
        [&](ArrayIDIDFunc sub_tail, ArrayIDIDFunc sub_head,
            ArrayIDIDFunc sub_to_super) {
            int sub_node_count = sub_tail.image_count();
            ArrayIDIDFunc sub_order = compute_tree_depth_order_of_connected_graph(
                std::move(sub_tail), std::move(sub_head), compute_separator,
                tree_depth_must_be_below);
            if(sub_order.preimage_count() != 0){
                for (int i = 0; i < sub_node_count; ++i)
                    order[order_end++] = sub_to_super[sub_order[i]];
                return true;
            }else{
                order = ArrayIDIDFunc();
                return false;
            }
        });
    if(order.preimage_count() != 0){
        assert(order_end == node_count);
        order = chain(order, to_input_id);
    }
    return order;
}

#endif
