#include "node-selection.hh"
#include "settings.hh"

#include <cctype>
#include <map>

#include <nlohmann/json.hpp>

#include <nix/store/store-api.hh>
#include <nix/store/store-open.hh>
#include <nix/util/error.hh>
#include <nix/util/fmt.hh>
#include <nix/util/logging.hh>

using json = nlohmann::json;

bool isValidNodeName(const std::string & name)
{
    if (name.empty() || name.size() > 255)
        return false;
    if (name.front() == '-' || name.front() == '.')
        return false;
    for (char c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != '_')
            return false;
    return true;
}

std::optional<std::string> pickBestNode(const std::vector<std::pair<std::string, long>> & scores)
{
    std::optional<std::string> best;
    long bestScore = 0;
    for (auto & [node, score] : scores) {
        /* Strictly greater: the earliest candidate keeps ties. */
        if (score > bestScore) {
            best = node;
            bestScore = score;
        }
    }
    return best;
}

std::string pbsSelectForHost(const std::string & node)
{
    return "1:host=" + node;
}

static std::map<std::string, std::string> parseNodeStoreAddresses()
{
    std::map<std::string, std::string> addresses;
    auto raw = ourSettings.nodeStoreAddresses.get();
    if (raw.empty())
        return addresses;
    json parsed = json::parse(raw);
    if (!parsed.is_object())
        throw nix::Error("invalid format for %s, expected a JSON dictionary", ourSettings.nodeStoreAddresses.name);
    for (auto & [node, address] : parsed.items()) {
        if (!address.is_string())
            throw nix::Error("invalid value for node %s in %s, expected a string", node, ourSettings.nodeStoreAddresses.name);
        addresses[node] = address.template get<std::string>();
    }
    return addresses;
}

static std::string storeUriForAddress(const std::string & address)
{
    if (ourSettings.sshUser.get() != "")
        return nix::fmt("ssh-ng://%s@%s:%d", ourSettings.sshUser.get(), address, ourSettings.sshPort.get());
    return nix::fmt("ssh-ng://%s:%d", address, ourSettings.sshPort.get());
}

std::optional<std::string> selectNodeForInputs(const nix::StorePathSet & inputs)
{
    using namespace nix;

    auto candidates = ourSettings.candidateNodes.get();
    if (candidates.empty() || inputs.empty())
        return std::nullopt;

    auto addresses = parseNodeStoreAddresses();

    StoreReference::Params params = {{"remote-store", ourSettings.remoteStore.get()}};
    if (ourSettings.remoteNixBinDir.get() != "")
        params["remote-program"] = ourSettings.remoteNixBinDir.get() + "/nix-daemon";

    std::vector<std::pair<std::string, long>> scores;
    std::string scoreSummary;
    auto addToSummary = [&](const std::string & entry) {
        scoreSummary += (scoreSummary.empty() ? "" : " ") + entry;
    };
    for (auto & node : candidates) {
        if (!isValidNodeName(node))
            throw Error(
                "nix-scheduler-hook: invalid node name '%s' in %s", node, ourSettings.candidateNodes.name);
        auto address = addresses.count(node) ? addresses[node] : node;
        try {
            auto store = nix::openStore(storeUriForAddress(address), params);
            auto valid = store->queryValidPaths(inputs, NoSubstitute);
            scores.emplace_back(node, static_cast<long>(valid.size()));
            addToSummary(fmt("%s=%d/%d", node, valid.size(), inputs.size()));
        } catch (std::exception & e) {
            /* A single unreachable candidate must not fail the build; it is
             * merely excluded from the ranking. */
            printError("NSH: skipping candidate node '%s': cannot query its store: %s", node, e.what());
            addToSummary(fmt("%s=unreachable", node));
        }
    }

    auto best = pickBestNode(scores);
    if (best)
        logger->log(
            lvlInfo,
            fmt("NSH: input-aware scheduling: selected node '%s' (most required inputs already present; scores: %s)",
                *best,
                scoreSummary));
    else
        logger->log(
            lvlTalkative,
            fmt("NSH: input-aware scheduling: no candidate holds any required input; deferring to the scheduler (scores: %s)",
                scoreSummary));
    return best;
}
