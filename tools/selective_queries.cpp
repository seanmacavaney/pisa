#include <iostream>

#include "mappable/mapper.hpp"
#include "mio/mmap.hpp"

#include "CLI/CLI.hpp"
#include "app.hpp"
#include "cursor/cursor.hpp"
#include "index_types.hpp"
#include "query/algorithm.hpp"
#include "query/queries.hpp"
#include <boost/algorithm/string/join.hpp>
#include <boost/range/adaptor/transformed.hpp>

using namespace pisa;

template <typename IndexType>
void selective_queries(
    const std::string& index_filename, std::string const& encoding, std::vector<Query> const& queries)
{
    IndexType index;
    spdlog::info("Loading index from {}", index_filename);
    mio::mmap_source m(index_filename.c_str());
    mapper::map(index, m, mapper::map_flags::warmup);

    spdlog::info("Performing {} queries", encoding);

    using boost::adaptors::transformed;
    using boost::algorithm::join;
    for (auto const& query: queries) {
        size_t and_results = and_query()(make_cursors(index, query), index.num_docs()).size();
        size_t or_results = or_query<false>()(make_cursors(index, query), index.num_docs());

        double selectiveness = double(and_results) / double(or_results);
        if (selectiveness < 0.005) {
            std::cout
                << join(query.terms | transformed([](auto d) { return std::to_string(d); }), " ")
                << '\n';
        }
    }
}

int main(int argc, const char** argv)
{
    App<arg::Index, arg::Query<arg::QueryMode::Unranked>> app{
        "Filters selective queries for a given index."};
    CLI11_PARSE(app, argc, argv);

    try {
        with_index(app.index_encoding(), app.index_filename(), [&](auto index) {
            selective_queries<decltype(index)>(
                app.index_filename(), app.index_encoding(), app.queries());
        });
    } catch (std::exception const& err) {
        spdlog::error("{}", err.what());
        return 1;
    }
}
