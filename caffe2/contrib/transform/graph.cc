#include "caffe2/contrib/transform/graph.h"

#include "caffe2/core/common.h"
#include "caffe2/core/logging.h"
#include "caffe2/core/net.h"
#include "caffe2/proto/caffe2.pb.h"

namespace caffe2 {

namespace transform {

Graph::Graph(const NetDef& net) : netdef_(net) {
  nodes_.clear();
  nodes_.resize(net.op_size());

  // Copy over operators
  for (int x = 0; x < net.op_size(); x++) {
    node(x).op = net.op(x);
  }

  // For any blob, which operator was the last to write to it?
  // In python, this is known as "versions".
  std::unordered_map<string, int> edge_parent;

  for (int i = 0; i < nodes_.size(); i++) {
    for (const string& blob : node(i).op.input()) {
      auto it = edge_parent.find(blob);
      if (it != edge_parent.end()) {
        int j = it->second;
        node(i).parents[j] = blob;
        node(j).children[i] = blob;
      } else {
        external_input_.insert(blob);
      }
    }
    for (const string& blob : node(i).op.output()) {
      edge_parent[blob] = i;
    }
  }

  // Traverse opposite direction to find external outputs

  // For any blob, which operator was the last to read to from it?
  std::unordered_map<string, int> edge_child;

  for (int i = nodes_.size() - 1; i >= 0; i--) {
    for (const string& blob : node(i).op.output()) {
      auto it = edge_child.find(blob);
      if (it == edge_child.end()) {
        external_output_.insert(blob);
      }
    }
    for (const string& blob : node(i).op.input()) {
      edge_child[blob] = i;
    }
  }
}

const std::vector<std::pair<string, int>> Graph::GetSubgraphInput(
    const std::vector<int>& match) {
  return GetSubgraphPerimeterHelper(true, match);
}

const std::vector<std::pair<string, int>> Graph::GetSubgraphOutput(
    const std::vector<int>& match) {
  return GetSubgraphPerimeterHelper(false, match);
}

// This helper function will either get:
//    1) a list for the blobs that write INTO a subgraph
//    2) a list of for the blobs that are written FROM a subgraph.
//
// The "from_children" flag determines if it is case 1 (true) or case 2 (false).
const std::vector<std::pair<string, int>> Graph::GetSubgraphPerimeterHelper(
    bool from_children,
    const std::vector<int>& match) {
  std::vector<std::pair<string, int>> edge_list;
  std::unordered_set<int> match_set(match.begin(), match.end());
  for (int x = 0; x < nodes_.size(); x++) {
    if (!is_node_active(x)) {
      continue;
    }
    if (!match_set.count(x)) { // x is not in subgraph
      const auto& list = from_children ? node(x).children : node(x).parents;
      for (const auto& edge : list) {
        int parent = edge.first;
        const string& blob = edge.second;
        if (match_set.count(parent)) { // but has a parent that is in subgraph
          edge_list.push_back({blob, x});
        }
      }
    }
  }
  // return the list in sorted order, to allow binary searching
  std::sort(edge_list.begin(), edge_list.end());
  return edge_list;
}

NetDef Graph::GetNetDef() {
  std::vector<bool> visited(nodes_.size(), false);

  // Copy over all the properties of the netdef we're based on
  NetDef netdef = netdef_;

  // But we're going to put in our own operators.
  netdef.clear_op();

  // Keeps track of the number of parents yet to be processed.
  std::vector<int> unchecked_parent_count;

  // We will perform a topological traversal on the nodes, but we will prefer
  // nodes that come earlier in the execution order.

  // This is a min-heap, which stores its elements in ascending order.
  // This stores the nodes in the order we process them to be in.
  // This guarantees the lowest lexicographical topological ordering.

  // This also means the original nodes will be kept in their execution order.
  std::priority_queue<int, std::vector<int>, std::greater<int>> q;

  // In our graph, G, the nodes don't have a strict ordering. But in the netdef,
  // they must (since nets are operators executed in some order).
  // How do we make sure that the order of operators in our generated netdef
  // is valid?
  // 1) The ordering of the netdef must be topologically sorted, respect to G.
  //    If A -> B is an edge in the graph G, then A must come before B in the
  //    netdef's ordering.
  // 2) No blob conflicts: If A -> B is an edge in the graph G, and A writes to
  //    blob X and B reads from blob X, then there cannot be an op that writes
  //    to blob X between A and B in the ordering.
  //
  // Perform a Topological Sort, to find an order for the Operators to be in.
  // We will keep track of the number of parents each node has.
  // We begin with an empty queue, and push in all nodes that do not have any
  // parents. Then, we keep track of all unprocessed parents for each node.
  // When a node has no more unprocessed parents, we can push it into the queue
  // to be processed. This guarantees condition 1 is satisfied.

  // TODO(benz): Currently, condition 2 is not guaranteed to be satisified.
  // However, giving each blob unique names via SSA will satisfy this condition.
  // Then, the resulting graph can be optimized with memonger.

  for (int i = 0; i < nodes_.size(); i++) {
    unchecked_parent_count.push_back(node(i).parents.size());
    if (node(i).parents.size() == 0 && is_node_active(i)) {
      q.push(i);
      visited[i] = true;
    }
  }

  while (!q.empty()) {
    int idx = q.top();
    q.pop();
    if (!is_node_active(idx)) {
      continue;
    }
    // Creates a new OperatorDef in NetDef
    auto& op = *(netdef.add_op());
    // Sets it equal to the OperatorDef at node(idx)
    op = node(idx).op;
    for (const auto& edge : node(idx).children) {
      int child = edge.first;
      if (!visited[child] && is_node_active(child)) {
        unchecked_parent_count[child]--;
        if (unchecked_parent_count[child] == 0) {
          q.push(child);
          visited[child] = true;
        }
      }
    }
  }
  return netdef;
}

void Graph::DeactivateSubgraph(std::vector<int> subgraph) {
  for (int idx : subgraph) {
    // remove all edges connected to inactive node
    for (const auto& edge : node(idx).parents) {
      int parent = edge.first;
      node(parent).children.erase(idx);
    }
    for (const auto& edge : node(idx).children) {
      int child = edge.first;
      node(child).parents.erase(idx);
    }
    // actually mark flags as false
    node(idx).active = false;
  }
}

} // namespace transform

} // namespace caffe2
