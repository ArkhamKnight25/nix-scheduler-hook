/* Focused request-construction/policy tests for input-aware node selection.
 * These cover the pure helpers shared by all three backends; the end-to-end
 * placement behaviour is covered by the VM tests in tests.nix. */

#include "node-selection.hh"

#include <cstdlib>
#include <iostream>
#include <string>

static int failures = 0;

static void check(bool cond, const std::string & what)
{
    if (!cond) {
        std::cerr << "FAIL: " << what << std::endl;
        failures++;
    }
}

int main()
{
    /* Node-name validation (names end up inside scheduler requests). */
    check(isValidNodeName("node2"), "plain hostname is valid");
    check(isValidNodeName("gpu-node.cluster_1"), "dots, dashes and underscores are valid");
    check(!isValidNodeName(""), "empty name is invalid");
    check(!isValidNodeName("node 2"), "whitespace is invalid");
    check(!isValidNodeName("node;rm"), "shell metacharacters are invalid");
    check(!isValidNodeName("node,other"), "hostlist separators are invalid");
    check(!isValidNodeName("-leading"), "leading dash is invalid");
    check(!isValidNodeName(std::string(300, 'a')), "overlong name is invalid");

    /* Scoring policy: highest score wins, first-listed wins ties, zero
     * scores defer to the scheduler. */
    check(!pickBestNode({}).has_value(), "no scores defers to the scheduler");
    check(!pickBestNode({{"a", 0}, {"b", 0}}).has_value(), "all-zero scores defer to the scheduler");
    check(pickBestNode({{"a", 1}, {"b", 3}}) == "b", "highest score wins");
    check(pickBestNode({{"a", 3}, {"b", 1}}) == "a", "highest score wins regardless of order");
    check(pickBestNode({{"a", 2}, {"b", 2}}) == "a", "first-listed candidate wins ties");
    check(pickBestNode({{"a", 0}, {"b", 1}}) == "b", "zero-score nodes lose to any holder");

    /* PBS host pinning request construction. */
    check(pbsSelectForHost("node2") == "1:host=node2", "PBS select expression pins one chunk to the host");

    if (failures) {
        std::cerr << failures << " test(s) failed" << std::endl;
        return 1;
    }
    std::cout << "all nsh unit tests passed" << std::endl;
    return 0;
}
