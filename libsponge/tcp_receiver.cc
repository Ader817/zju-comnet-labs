#include "tcp_receiver.hh"

// Dummy implementation of a TCP receiver

// For Lab 2, please replace with a real implementation that passes the
// automated checks run by `make check_lab2`.

template <typename... Targs>
void DUMMY_CODE(Targs &&.../* unused */) {}

using namespace std;

void TCPReceiver::segment_received(const TCPSegment &seg) {
    // Handle SYN flag
    bool syn_flag = seg.header().syn;
    if (syn_flag) {
        if (_status == Status::LISTEN) {
            _isn = seg.header().seqno;
            _status = Status::SYN_RECV;
        } else {
            // Ignore duplicate SYN
            return;
        }
    }

    if (_status == Status::LISTEN) {
        // Ignore segments before receiving SYN
        return;
    }

    // Handle payload and FIN
    uint64_t checkpoint = _reassembler.stream_out().bytes_written();
    uint64_t abs_seqno = unwrap(seg.header().seqno, _isn.value(), checkpoint);
    
    // Calculate stream index
    // If SYN is set, the payload starts at abs_seqno (SYN occupies abs_seqno - 1 conceptually)
    // Otherwise, payload starts at abs_seqno - 1 (accounting for the initial SYN)
    uint64_t stream_index = syn_flag ? abs_seqno : (abs_seqno - 1);

    _reassembler.push_substring(seg.payload().copy(), stream_index, seg.header().fin);

    if (_reassembler.stream_out().input_ended()) {
        _status = Status::FIN_RECV;
    }
}

optional<WrappingInt32> TCPReceiver::ackno() const {
    if (_status == Status::LISTEN) {
        return {};
    }
    
    uint64_t ack_index = _reassembler.stream_out().bytes_written();
    
    // Always account for SYN (占用序号 0)
    ack_index += 1;
    
    // If stream has ended, FIN has been received, account for it
    if (_status == Status::FIN_RECV) {
        ack_index += 1;
    }
    
    return wrap(ack_index, _isn.value());
}

size_t TCPReceiver::window_size() const {
    return _capacity - _reassembler.stream_out().buffer_size();
}
