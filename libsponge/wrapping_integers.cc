#include "wrapping_integers.hh"
// Dummy implementation of a 32-bit wrapping integer

// For Lab 2, please replace with a real implementation that passes the
// automated checks run by `make check_lab2`.

template <typename... Targs>
void DUMMY_CODE(Targs &&.../* unused */) {}

using namespace std;

constexpr uint64_t dist(uint64_t a, uint64_t b) { return a > b ? a - b : b - a; }

//! Transform an "absolute" 64-bit sequence number (zero-indexed) into a WrappingInt32
//! \param n The input absolute 64-bit sequence number
//! \param isn The initial sequence number
WrappingInt32 wrap(uint64_t n, WrappingInt32 isn) {
    return WrappingInt32{static_cast<uint32_t>(n + isn.raw_value())};
}

//! Transform a WrappingInt32 into an "absolute" 64-bit sequence number (zero-indexed)
//! \param n The relative sequence number
//! \param isn The initial sequence number
//! \param checkpoint A recent absolute 64-bit sequence number
//! \returns the 64-bit sequence number that wraps to `n` and is closest to `checkpoint`
//!
//! \note Each of the two streams of the TCP connection has its own ISN. One stream
//! runs from the local TCPSender to the remote TCPReceiver and has one ISN,
//! and the other stream runs from the remote TCPSender to the local TCPReceiver and
//! has a different ISN.
uint64_t unwrap(WrappingInt32 n, WrappingInt32 isn, uint64_t checkpoint) {
    uint32_t offset = n - isn; // absolute sequence number modulo 2^32
    uint64_t base = checkpoint & ~static_cast<uint64_t>(UINT32_MAX); // checkpoint rounded down to multiple of 2^32
    
    // Three candidates: base - 2^32 + offset, base + offset, base + 2^32 + offset
    uint64_t candidate_mid = base + offset;
    uint64_t candidate_high = base + (1ul << 32) + offset;
    
    uint64_t best_candidate = candidate_mid;
    
    // Consider candidate_low only if base >= 2^32 (to avoid underflow)
    if (base >= (1ul << 32)) {
        uint64_t candidate_low = base - (1ul << 32) + offset;
        if (dist(candidate_low, checkpoint) < dist(best_candidate, checkpoint)) {
            best_candidate = candidate_low;
        }
    }
    
    if (dist(candidate_high, checkpoint) < dist(best_candidate, checkpoint)) {
        best_candidate = candidate_high;
    }
    
    return best_candidate;
}