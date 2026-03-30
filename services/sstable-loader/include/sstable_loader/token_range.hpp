#pragma once
// services/sstable-loader/include/sstable_loader/token_range.hpp
//
// TokenRange + TokenRangeCalculator — Murmur3 token ring partitioning.
// C++20 port of services/sstable-loader/src/token_range.rs

#include "svckit/database.hpp"
#include <boost/asio/awaitable.hpp>
#include <cstdint>
#include <memory>
#include <vector>

namespace sstable_loader {

namespace asio = boost::asio;

// ---------------------------------------------------------------------------
// TokenRange — a half-open [start, end) range on the Murmur3 ring
// ---------------------------------------------------------------------------
struct TokenRange {
    int64_t start{0};
    int64_t end{0};

    TokenRange()                               = default;
    TokenRange(int64_t s, int64_t e) : start{s}, end{e} {}
    TokenRange(const TokenRange&)              = default;
    TokenRange(TokenRange&&)                   = default;
    TokenRange& operator=(const TokenRange&)   = default;
    TokenRange& operator=(TokenRange&&)        = default;
    ~TokenRange()                              = default;
};

// ---------------------------------------------------------------------------
// TokenRangeCalculator
//
// Rule of 5:
//   - Copy: DELETED — holds shared connection
//   - Move: allowed
// ---------------------------------------------------------------------------
class TokenRangeCalculator {
public:
    explicit TokenRangeCalculator(std::shared_ptr<svckit::ScyllaConnection> conn);

    TokenRangeCalculator(const TokenRangeCalculator&)            = delete;
    TokenRangeCalculator& operator=(const TokenRangeCalculator&) = delete;
    TokenRangeCalculator(TokenRangeCalculator&&)                 = default;
    TokenRangeCalculator& operator=(TokenRangeCalculator&&)      = default;
    ~TokenRangeCalculator()                                      = default;

    // Calculate token ranges — ranges_per_core controls parallelism granularity
    [[nodiscard]] asio::awaitable<std::vector<TokenRange>>
    calculate_ranges(uint32_t ranges_per_core) const;

private:
    std::shared_ptr<svckit::ScyllaConnection> conn_;
};

} // namespace sstable_loader
