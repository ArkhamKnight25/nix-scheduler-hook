#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nix/store/path.hh>
#include <nix/util/types.hh>

/* Input-aware node selection: policy helpers for picking the compute node
 * that already holds the largest part of a build's required input closure.
 * The pure helpers below are unit-tested in test-nsh.cc; only
 * selectNodeForInputs talks to remote stores. */

/* Conservative hostname whitelist for embedding a node name in scheduler
 * requests (Slurm required_nodes/req_nodes, PBS select). */
bool isValidNodeName(const std::string & name);

/* Pick the winning node from (name, score) pairs: highest score wins, the
 * earliest-listed node wins ties, and a winner must hold at least one input
 * (score > 0) — otherwise nullopt defers placement to the scheduler. */
std::optional<std::string> pickBestNode(const std::vector<std::pair<std::string, long>> & scores);

/* PBS select expression pinning one chunk to a specific host. */
std::string pbsSelectForHost(const std::string & node);

/* Score every candidate-nodes entry by how many of `inputs` its Nix store
 * already holds (Store::queryValidPaths over ssh-ng), and return the winner.
 * Returns nullopt — submit without a node preference — when candidate-nodes
 * is unset, inputs is empty, every node is unreachable, or every node scores
 * zero. An unreachable candidate is skipped, never fatal. */
std::optional<std::string> selectNodeForInputs(const nix::StorePathSet & inputs);
