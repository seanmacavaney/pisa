#include <iostream>
#include <optional>
#include <unordered_set>

#include "boost/algorithm/string/classification.hpp"
#include "boost/algorithm/string/split.hpp"
#include <boost/functional/hash.hpp>

#include "mio/mmap.hpp"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/spdlog.h"

#include "mappable/mapper.hpp"

#include "app.hpp"
#include "cursor/max_scored_cursor.hpp"
#include "index_types.hpp"
#include "io.hpp"
#include "query/algorithm.hpp"
#include "util/util.hpp"
#include "wand_data_compressed.hpp"
#include "wand_data_raw.hpp"

#include "query/algorithm.hpp"
#include "scorer/scorer.hpp"

#include "CLI/CLI.hpp"

using namespace pisa;

std::set<uint32_t> parse_tuple(std::string const& line, size_t k)
{
    std::vector<std::string> term_ids;
    boost::algorithm::split(term_ids, line, boost::is_any_of(" \t"));
    if (term_ids.size() != k) {
        throw std::runtime_error(fmt::format(
            "Wrong number of terms in line: {} (expected {} but found {})", line, k, term_ids.size()));
    }

    std::set<uint32_t> term_ids_int;
    for (auto&& term_id: term_ids) {
        try {
            term_ids_int.insert(std::stoi(term_id));
        } catch (...) {
            throw std::runtime_error(
                fmt::format("Cannot convert {} to int in line: {}", term_id, line));
        }
    }
    return term_ids_int;
}

template <typename Index, typename Wdata>
void kth_thresholds(
    Index&& index,
    Wdata&& wdata,
    const std::vector<Query>& queries,
    std::string const& type,
    ScorerParams const& scorer_params,
    uint64_t k,
    bool quantized,
    std::optional<std::string> pairs_filename,
    std::optional<std::string> triples_filename,
    bool all_pairs,
    bool all_triples)
{
    auto scorer = scorer::from_params(scorer_params, wdata);

    using Pair = std::set<uint32_t>;
    std::unordered_set<Pair, boost::hash<Pair>> pairs_set;

    using Triple = std::set<uint32_t>;
    std::unordered_set<Triple, boost::hash<Triple>> triples_set;

    std::string line;
    if (all_pairs) {
        spdlog::info("All pairs are available.");
    }
    if (pairs_filename) {
        std::ifstream pin(*pairs_filename);
        while (std::getline(pin, line)) {
            pairs_set.insert(parse_tuple(line, 2));
        }
        spdlog::info("Number of pairs loaded: {}", pairs_set.size());
    }

    if (all_triples) {
        spdlog::info("All triples are available.");
    }
    if (triples_filename) {
        std::ifstream trin(*triples_filename);
        while (std::getline(trin, line)) {
            triples_set.insert(parse_tuple(line, 3));
        }
        spdlog::info("Number of triples loaded: {}", triples_set.size());
    }

    for (auto const& query: queries) {
        float threshold = 0;

        auto terms = query.terms;
        topk_queue topk(k);
        wand_query wand_q(topk);

        for (auto&& term: terms) {
            Query query;
            query.terms.push_back(term);
            wand_q(make_max_scored_cursors(index, wdata, *scorer, query), index.num_docs());
            threshold = std::max(threshold, topk.size() == k ? topk.threshold() : 0.0F);
            topk.clear();
        }
        for (size_t i = 0; i < terms.size(); ++i) {
            for (size_t j = i + 1; j < terms.size(); ++j) {
                if (pairs_set.count({terms[i], terms[j]}) > 0 or all_pairs) {
                    Query query;
                    query.terms = {terms[i], terms[j]};
                    wand_q(make_max_scored_cursors(index, wdata, *scorer, query), index.num_docs());
                    threshold = std::max(threshold, topk.size() == k ? topk.threshold() : 0.0F);
                    topk.clear();
                }
            }
        }
        for (size_t i = 0; i < terms.size(); ++i) {
            for (size_t j = i + 1; j < terms.size(); ++j) {
                for (size_t s = j + 1; s < terms.size(); ++s) {
                    if (triples_set.count({terms[i], terms[j], terms[s]}) > 0 or all_triples) {
                        Query query;
                        query.terms = {terms[i], terms[j], terms[s]};
                        wand_q(
                            make_max_scored_cursors(index, wdata, *scorer, query), index.num_docs());
                        threshold = std::max(threshold, topk.size() == k ? topk.threshold() : 0.0F);
                        topk.clear();
                    }
                }
            }
        }
        std::cout << threshold << '\n';
    }
}

using wand_raw_index = wand_data<wand_data_raw>;
using wand_uniform_index = wand_data<wand_data_compressed<>>;
using wand_uniform_index_quantized = wand_data<wand_data_compressed<PayloadType::Quantized>>;

int main(int argc, const char** argv)
{
    spdlog::drop("");
    spdlog::set_default_logger(spdlog::stderr_color_mt(""));

    std::string query_filename;
    std::optional<std::string> pairs_filename;
    std::optional<std::string> triples_filename;
    size_t k;
    std::string index_filename;
    std::string wand_data_filename;
    bool quantized = false;

    bool all_pairs = false;
    bool all_triples = false;

    App<arg::Index, arg::WandData<arg::WandMode::Required>, arg::Query<arg::QueryMode::Ranked>, arg::Scorer>
        app{"A tool for performing threshold estimation using the k-highest impact score for each "
            "term, pair or triple of a query. Pairs and triples are only used if provided with "
            "--pairs and --triples respectively."};
    auto pairs = app.add_option(
        "-p,--pairs", pairs_filename, "A tab separated file containing all the cached term pairs");
    auto triples = app.add_option(
        "-t,--triples",
        triples_filename,
        "A tab separated file containing all the cached term triples");
    app.add_flag("--all-pairs", all_pairs, "Consider all term pairs of a query")->excludes(pairs);
    app.add_flag("--all-triples", all_triples, "Consider all term triples of a query")->excludes(triples);
    app.add_flag("--quantized", quantized, "Quantizes the scores");

    CLI11_PARSE(app, argc, argv);

    auto params = std::make_tuple(
        app.index_filename(),
        app.wand_data_path(),
        app.queries(),
        app.index_encoding(),
        app.scorer_params(),
        app.k(),
        quantized,
        pairs_filename,
        triples_filename,
        all_pairs,
        all_triples);

    try {
        IndexType::resolve(app.index_encoding()).load_and_execute(app.index_filename(), [&](auto&& index) {
            auto thresholds = [&](auto wdata) {
                kth_thresholds(
                    index,
                    wdata,
                    app.queries(),
                    app.index_encoding(),
                    app.scorer_params(),
                    app.k(),
                    quantized,
                    pairs_filename,
                    triples_filename,
                    all_pairs,
                    all_triples);
            };
            auto wdata_source = MemorySource::mapped_file(app.wand_data_path());
            if (app.is_wand_compressed()) {
                if (quantized) {
                    thresholds(wand_uniform_index_quantized(std::move(wdata_source)));
                } else {
                    thresholds(wand_uniform_index(std::move(wdata_source)));
                }
            } else {
                thresholds(wand_raw_index(std::move(wdata_source)));
            }
        });
    } catch (std::exception const& err) {
        spdlog::error("{}", err.what());
        return 1;
    }
}
