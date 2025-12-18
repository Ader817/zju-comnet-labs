#include "tcp_connection.hh"

#include <iostream>

// Dummy implementation of a TCP connection

// For Lab 4, please replace with a real implementation that passes the
// automated checks run by `make check`.

template <typename... Targs>
void DUMMY_CODE(Targs &&.../* unused */) {}

using namespace std;

size_t TCPConnection::remaining_outbound_capacity() const { return _sender.stream_in().remaining_capacity(); }

size_t TCPConnection::bytes_in_flight() const { return _sender.bytes_in_flight(); }

size_t TCPConnection::unassembled_bytes() const { return _receiver.unassembled_bytes(); }

size_t TCPConnection::time_since_last_segment_received() const { return _time_since_last_segment_received; }

void TCPConnection::send_segments() {
    while (!_sender.segments_out().empty()) {
        TCPSegment seg = _sender.segments_out().front();
        _sender.segments_out().pop();

        // 填充 ACK 和窗口大小
        if (auto ackno = _receiver.ackno(); ackno.has_value()) {
            seg.header().ack = true;
            seg.header().ackno = ackno.value();
        }
        seg.header().win = static_cast<uint16_t>(_receiver.window_size());

        _segments_out.push(seg);
    }
}

void TCPConnection::segment_received(const TCPSegment &seg) {
    // ========================================
    // 第一部分：前置检查
    // ========================================
    if (!_is_active) {
        return;  // 连接已关闭，忽略所有段
    }
    
    _time_since_last_segment_received = 0;  // 重置计时器
    
    // ========================================
    // 第二部分：处理异常情况
    // ========================================
    if (seg.header().rst) {
        // RST 表示连接异常终止，立即关闭
        _receiver.stream_out().set_error();
        _sender.stream_in().set_error();
        _is_active = false;
        return;  // RST 后不再处理其他逻辑
    }

    // 忽略连接尚未建立时的无效纯 ACK 段
    if (seg.length_in_sequence_space() == 0 && seg.header().ack && 
        !_receiver.ackno().has_value() && _sender.next_seqno_absolute() == 0) {
        return;
    }

    // ========================================
    // 第三部分：核心处理 - 拆分双向信息
    // ========================================
    
    // 3.1 将数据部分（seqno, payload, SYN, FIN）交给 receiver
    _receiver.segment_received(seg);
    
    // 3.2 将 ACK 部分（ackno, window）交给 sender
    if (seg.header().ack) {
        _sender.ack_received(seg.header().ackno, seg.header().win);
        // ack_received 中会调用 fill_window 将数据放入 Sender 的发送队列
    }
    
    // ========================================
    // 第四部分：特殊状态处理
    // ========================================
    
    // 4.1 被动连接：收到 SYN，需要发送 SYN+ACK
    if (TCPState::state_summary(_receiver) == TCPReceiverStateSummary::SYN_RECV &&
        TCPState::state_summary(_sender) == TCPSenderStateSummary::CLOSED) {
        connect();  // 发送 SYN+ACK（内部会调用 send_segments）
        return;     // 提前返回，避免重复处理
    }
    
    // 4.2 被动关闭：收到 FIN 但还在通信，不需要 TIME_WAIT
    // _linger_after_streams_finish 默认为 true，默认假设我们是主动关闭通信的一方，需要 TIME_WAIT
    // 若接收到 segment 后， Receiver 进入了 FIN_RECV 状态，而 Sender 还未完全关闭（SYN_ACKED），
    // 则说明我们是被动关闭通信的一方，不需要 TIME_WAIT，需要设置 _linger_after_streams_finish 为 false
    if (TCPState::state_summary(_receiver) == TCPReceiverStateSummary::FIN_RECV &&
        TCPState::state_summary(_sender) == TCPSenderStateSummary::SYN_ACKED) {
        _linger_after_streams_finish = false;
    }
    
    // ========================================
    // 第五部分：响应处理
    // ========================================
    
    // 5.1 如果收到数据/SYN/FIN，需要回复 ACK
    if (seg.length_in_sequence_space() > 0) {
        _sender.send_empty_segment();
    }
    
    // 5.2 尝试发送更多数据（窗口可能变大了）
    _sender.fill_window();
    
    // 5.3 将所有段填充完整信息(ack, ackno, win)并发送
    send_segments();
    
    // ========================================
    // 第六部分：检查是否该关闭连接
    // ========================================
    
    // 双方都结束且不需要 linger，可以直接关闭
    if (TCPState::state_summary(_receiver) == TCPReceiverStateSummary::FIN_RECV &&
        TCPState::state_summary(_sender) == TCPSenderStateSummary::FIN_ACKED &&
        !_linger_after_streams_finish) {
        _is_active = false;
    }
}

bool TCPConnection::active() const { return _is_active; }

size_t TCPConnection::write(const string &data) {
    size_t bytes_written = _sender.stream_in().write(data);
    _sender.fill_window();
    send_segments();
    return bytes_written;
}

//! \param[in] ms_since_last_tick number of milliseconds since the last call to this method
void TCPConnection::tick(const size_t ms_since_last_tick) {
    // ========================================
    // 第一部分：基础检查
    // ========================================
    if (!_is_active) {
        return;  // 连接已关闭，不需要处理
    }
    
    // ========================================
    // 第二部分：更新计时器
    // ========================================
    _time_since_last_segment_received += ms_since_last_tick;
    
    // ========================================
    // 第三部分：通知 Sender 时间流逝
    // ========================================
    // Sender 会处理重传逻辑（检查超时、重传段）
    _sender.tick(ms_since_last_tick);
    
    // ========================================
    // 第四部分：检查连续重传次数
    // ========================================
    // 如果重传次数过多，说明连接有严重问题，需要终止
    if (_sender.consecutive_retransmissions() > TCPConfig::MAX_RETX_ATTEMPTS) {
        // 发送 RST 段并终止连接
        _sender.send_empty_segment();
        
        // 从 sender 队列取出段，标记为 RST
        if (!_sender.segments_out().empty()) {
            TCPSegment seg = _sender.segments_out().front();
            _sender.segments_out().pop();
            seg.header().rst = true;
            _segments_out.push(seg);
        }
        
        // 标记两个流为错误状态
        _receiver.stream_out().set_error();
        _sender.stream_in().set_error();
        
        // 终止连接
        _is_active = false;
        return;
    }
    
    // ========================================
    // 第五部分：发送重传的段
    // ========================================
    // sender.tick() 可能产生了重传的段，需要填充完整信息并发送
    send_segments();
    
    // ========================================
    // 第六部分：检查 TIME_WAIT 超时
    // ========================================
    // 如果处于 TIME_WAIT 状态且超时，可以关闭连接
    if (TCPState::state_summary(_receiver) == TCPReceiverStateSummary::FIN_RECV &&
        TCPState::state_summary(_sender) == TCPSenderStateSummary::FIN_ACKED &&
        _linger_after_streams_finish &&
        _time_since_last_segment_received >= 10 * _cfg.rt_timeout) {
        _is_active = false;
    }
}

void TCPConnection::end_input_stream() {
    _sender.stream_in().end_input();
    _sender.fill_window();
    send_segments();
}

void TCPConnection::connect() {
    _sender.fill_window();
    send_segments();
}

TCPConnection::~TCPConnection() {
    try {
        if (active()) {
            cerr << "Warning: Unclean shutdown of TCPConnection\n";

            // Your code here: need to send a RST segment to the peer
            _sender.send_empty_segment();
            TCPSegment &seg = _sender.segments_out().back();
            seg.header().rst = true;
            send_segments();
        }
    } catch (const exception &e) {
        std::cerr << "Exception destructing TCP FSM: " << e.what() << std::endl;
    }
}
