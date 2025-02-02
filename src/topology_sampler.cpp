// Copyright 2019-2022 bito project contributors.
// bito is free software under the GPLv3; see LICENSE file for details.

#include "topology_sampler.hpp"

Node::NodePtr TopologySampler::Sample(SubsplitDAGNode node, SubsplitDAG& dag,
                                      EigenConstVectorXdRef normalized_sbn_parameters,
                                      EigenConstVectorXdRef inverted_probabilities) {
  SamplingSession session({dag, normalized_sbn_parameters, inverted_probabilities});
  session.result_.AddVertex({node.Id(), node.GetBitset()});
  SampleRootward(session, node);
  SampleLeafward(session, node, SubsplitClade::Left);
  SampleLeafward(session, node, SubsplitClade::Right);
  session.result_.ConnectAllVertices();
  auto root = session.result_.FindRoot();
  if (!root.has_value()) Failwith("No root found");
  return BuildTree(session, root.value().get());
}

void TopologySampler::SetSeed(uint64_t seed) { mersenne_twister_.SetSeed(seed); }

void TopologySampler::VisitNode(SamplingSession& session, SubsplitDAGNode node,
                                Direction direction, SubsplitClade clade) {
  session.result_.AddVertex({node.Id(), node.GetBitset()});
  switch (direction) {
    case Direction::Rootward:
      SampleLeafward(session, node, SubsplitClade::Left);
      SampleLeafward(session, node, SubsplitClade::Right);
      break;
    case Direction::Leafward:
      SampleRootward(session, node);
      SampleLeafward(session, node, Bitset::Opposite(clade));
      break;
  }
}

void TopologySampler::SampleRootward(SamplingSession& session, SubsplitDAGNode node) {
  auto left = node.GetLeftRootward();
  auto right = node.GetRightRootward();
  if (left.empty() && right.empty()) {
    // reached root
    return;
  }
  auto [parent_node, parent_edge] = SampleParentNodeAndEdge(session, left, right);
  session.result_.AddLine({parent_edge.GetId(), parent_edge.GetParent(),
                           parent_edge.GetChild(), parent_edge.GetSubsplitClade()});
  VisitNode(session, parent_node, Direction::Leafward, parent_edge.GetSubsplitClade());
}

void TopologySampler::SampleLeafward(SamplingSession& session, SubsplitDAGNode node,
                                     SubsplitClade clade) {
  auto neighbors = node.GetNeighbors(Direction::Leafward, clade);
  if (neighbors.empty()) {
    // reached leaf
    return;
  }
  auto child = SampleChildNodeAndEdge(session, neighbors);
  session.result_.AddLine({child.second.GetId(), child.second.GetParent(),
                           child.second.GetChild(), child.second.GetSubsplitClade()});
  VisitNode(session, child.first, Direction::Rootward, clade);
}

std::pair<SubsplitDAGNode, ConstLineView> TopologySampler::SampleParentNodeAndEdge(
    SamplingSession& session, ConstNeighborsView left, ConstNeighborsView right) {
  std::vector<double> weights;
  weights.resize(left.size() + right.size());
  size_t i = 0;
  for (auto parent = left.begin(); parent != left.end(); ++parent)
    weights[i++] = session.inverted_probabilities_[parent.GetEdge().value_];
  for (auto parent = right.begin(); parent != right.end(); ++parent)
    weights[i++] = session.inverted_probabilities_[parent.GetEdge().value_];
  std::discrete_distribution<> distribution(weights.begin(), weights.end());
  auto sampled_index =
      static_cast<size_t>(distribution(mersenne_twister_.GetGenerator()));
  if (sampled_index < left.size()) {
    auto parent = left.begin();
    std::advance(parent, sampled_index);
    return {session.dag_.GetDAGNode(NodeId(parent.GetNodeId())),
            session.dag_.GetDAGEdge(EdgeId(parent.GetEdge()))};
  }  // else
  auto parent = right.begin();
  std::advance(parent, sampled_index - left.size());
  return {session.dag_.GetDAGNode(NodeId(parent.GetNodeId())),
          session.dag_.GetDAGEdge(EdgeId(parent.GetEdge()))};
}

std::pair<SubsplitDAGNode, ConstLineView> TopologySampler::SampleChildNodeAndEdge(
    SamplingSession& session, ConstNeighborsView neighbors) {
  std::vector<double> weights;
  weights.resize(neighbors.size());
  size_t i = 0;
  for (auto child = neighbors.begin(); child != neighbors.end(); ++child) {
    weights[i++] = session.normalized_sbn_parameters_[child.GetEdge().value_];
  }
  std::discrete_distribution<> distribution(weights.begin(), weights.end());
  i = static_cast<size_t>(distribution(mersenne_twister_.GetGenerator()));
  auto child = neighbors.begin();
  std::advance(child, i);
  return {session.dag_.GetDAGNode(NodeId(child.GetNodeId())),
          session.dag_.GetDAGEdge(EdgeId(child.GetEdge()))};
}

Node::NodePtr TopologySampler::BuildTree(SamplingSession& session,
                                         const DAGVertex& node) {
  auto left = node.GetNeighbors(Direction::Leafward, SubsplitClade::Left);
  auto right = node.GetNeighbors(Direction::Leafward, SubsplitClade::Right);
  NodeId left_id = NodeId(NoId), right_id = NodeId(NoId);
  if (!left.empty()) {
    left_id = left.begin().GetNodeId();
  }
  if (!right.empty()) {
    right_id = right.begin().GetNodeId();
  }
  if (left_id != NoId && right_id != NoId) {
    return Node::Join(BuildTree(session, session.result_.GetVertex(left_id)),
                      BuildTree(session, session.result_.GetVertex(right_id)),
                      node.GetId().value_);
  }
  if (node.IsLeaf()) {
    return Node::Leaf(node.GetId().value_);
  }
  if (node.IsRoot()) {
    Assert(left_id != NoId, "Root has no children");
    return Node::Join({BuildTree(session, session.result_.GetVertex(left_id))},
                      node.GetId().value_);
  }
  Failwith("Node can't have only one child");
}
