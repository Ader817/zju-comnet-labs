#include "tcp_sender.hh"

#include "tcp_config.hh"

#include <algorithm>
#include <random>

using namespace std;

//! \param[in] capacity the capacity of the outgoing byte stream
//! \param[in] retx_timeout the initial amount of time to wait before retransmitting the oldest outstanding segment
//! \param[in] fixed_isn the Initial Sequence Number to use, if set (otherwise uses a random ISN)
TCPSender::TCPSender(const size_t capacity, const uint16_t retx_timeout, const std::optional<WrappingInt32> fixed_isn)
    : _isn(fixed_isn.value_or(WrappingInt32{random_device()()}))
    , _initial_retransmission_timeout{retx_timeout}
    , _stream(capacity)
    , _current_rto{retx_timeout} {}

size_t TCPSender::bytes_in_flight() const {
    size_t bytes = 0;
    for (const auto &item : _segments_in_flight) {
        bytes += item.segment.length_in_sequence_space();
    }
    return bytes;
}

void TCPSender::fill_window() {
    // 计算有效窗口大小（窗口为0时视为1）
    size_t win = effective_window_size();

    // 循环填充窗口
    while (bytes_in_flight() < win) {
        TCPSegment seg;

        // 1. 如果尚未发送SYN，则设置syn位并发送SYN段
        if (!_syn_sent) {
            seg.header().syn = true;
            seg.header().seqno = wrap(_next_seqno, _isn);
            _syn_sent = true;

            // 启动计时器
            if (_segments_in_flight.empty()) {
                _timer_running = true;
                _retransmission_timer = 0;
            }

            // 发送并追踪
            _segments_out.push(seg);
            _segments_in_flight.push_back({_next_seqno, seg});
            _next_seqno += seg.length_in_sequence_space();
            continue;
        }

        // 2. 设置seqno
        seg.header().seqno = wrap(_next_seqno, _isn);

        // 计算可发送的payload大小
        size_t remaining_window = win - bytes_in_flight();
        if (remaining_window == 0) {
            break;
        }
        size_t max_payload = min(TCPConfig::MAX_PAYLOAD_SIZE, remaining_window);

        // 从stream读取数据作为payload
        string payload = _stream.read(max_payload);
        seg.payload() = Buffer(std::move(payload));

        // 3. 若满足条件则增加FIN
        // 条件：从来没发送过FIN，输入字节流处于EOF，窗口能容纳FIN
        size_t current_seg_size = seg.length_in_sequence_space();
        if (!_fin_sent && _stream.eof() && (current_seg_size < remaining_window)) {
            seg.header().fin = true;
            _fin_sent = true;
        }

        // 4. 如果没有任何数据（没有fin，没有payload），则停止发送
        if (seg.length_in_sequence_space() == 0) {
            break;
        }

        // 5. 如果没有正在等待的数据包，则启动计时器
        if (_segments_in_flight.empty()) {
            _timer_running = true;
            _retransmission_timer = 0;
        }

        // 6. 发送数据包并追踪
        _segments_out.push(seg);
        _segments_in_flight.push_back({_next_seqno, seg});
        _next_seqno += seg.length_in_sequence_space();

        // 7. 如果设置了fin，则停止（FIN后不能再发数据）
        if (seg.header().fin) {
            break;
        }
    }
}

//! \param ackno The remote receiver's ackno (acknowledgment number)
//! \param window_size The remote receiver's advertised window size
void TCPSender::ack_received(const WrappingInt32 ackno, const uint16_t window_size) {
    // 将ackno转换为绝对序号
    uint64_t ack_abs = unwrap(ackno, _isn, _next_seqno);

    // 如果传入的ack是不可靠的（ack > next_seqno），则直接丢弃
    if (ack_abs > _next_seqno) {
        return;
    }

    // 更新窗口大小
    _window_size = window_size;

    // 遍历已发送的segments，移除已被确认的
    bool acked_new_data = false;
    while (!_segments_in_flight.empty()) {
        const auto &item = _segments_in_flight.front();
        uint64_t seg_end = item.start_seqno + item.segment.length_in_sequence_space();

        // 如果整个segment都被确认了
        if (seg_end <= ack_abs) {
            _segments_in_flight.pop_front();
            acked_new_data = true;
        } else {
            break; // 由于deque按序排列，后续segment也不会被确认
        }
    }

    // 如果确认了新数据
    if (acked_new_data) {
        // 重置RTO为初始值
        _current_rto = _initial_retransmission_timeout;
        // 重置连续重传计数器
        _consecutive_retransmissions = 0;
        // 重置计时器
        _retransmission_timer = 0;
    }

    // 如果还有未确认的数据，保持计时器运行；否则停止
    _timer_running = !_segments_in_flight.empty();

    // 调用fill_window继续发送数据
    fill_window();
}

//! \param[in] ms_since_last_tick the number of milliseconds since the last call to this method
void TCPSender::tick(const size_t ms_since_last_tick) {
    // 如果计时器未运行，直接返回
    if (!_timer_running) {
        return;
    }

    // 累加计时器
    _retransmission_timer += ms_since_last_tick;

    // 检查是否超时
    if (_retransmission_timer >= _current_rto) {
        // 重传序列号最小的段（deque的front就是最老的）
        _segments_out.push(_segments_in_flight.front().segment);

        // 如果窗口大小不为0，则进行指数退避
        if (_window_size != 0) {
            _consecutive_retransmissions++;
            _current_rto *= 2;
        }

        // 重置计时器
        _retransmission_timer = 0;
    }
}

unsigned int TCPSender::consecutive_retransmissions() const { return _consecutive_retransmissions; }

void TCPSender::send_empty_segment() {
    TCPSegment seg;
    seg.header().seqno = wrap(_next_seqno, _isn);
    _segments_out.push(seg);
}
