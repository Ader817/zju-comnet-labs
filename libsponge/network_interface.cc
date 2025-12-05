#include "network_interface.hh"

#include "arp_message.hh"
#include "ethernet_frame.hh"

#include <iostream>

// Dummy implementation of a network interface
// Translates from {IP datagram, next hop address} to link-layer frame, and from link-layer frame to IP datagram

// For Lab 5, please replace with a real implementation that passes the
// automated checks run by `make check_lab5`.

// You will need to add private members to the class declaration in `network_interface.hh`

template <typename... Targs>
void DUMMY_CODE(Targs &&.../* unused */) {}

using namespace std;

//! \param[in] ethernet_address Ethernet (what ARP calls "hardware") address of the interface
//! \param[in] ip_address IP (what ARP calls "protocol") address of the interface
NetworkInterface::NetworkInterface(const EthernetAddress &ethernet_address, const Address &ip_address)
    : _ethernet_address(ethernet_address), _ip_address(ip_address) {
    cerr << "DEBUG: Network interface has Ethernet address " << to_string(_ethernet_address) << " and IP address "
         << ip_address.ip() << "\n";
}

//! \param[in] dgram the IPv4 datagram to be sent
//! \param[in] next_hop the IP address of the interface to send it to (typically a router or default gateway, but may also be another host if directly connected to the same network as the destination)
//! (Note: the Address type can be converted to a uint32_t (raw 32-bit IP address) with the Address::ipv4_numeric() method.)
void NetworkInterface::send_datagram(const InternetDatagram &dgram, const Address &next_hop) {
    // convert IP address of next hop to raw 32-bit representation (used in ARP header)
    const uint32_t next_hop_ip = next_hop.ipv4_numeric();

    // if the next hop is in the arp table, send the datagram
    if (_arp_table.find(next_hop_ip) != _arp_table.end()) {
        EthernetFrame frame_out;
        frame_out.header() = {_arp_table[next_hop_ip].ethernet_address, _ethernet_address, EthernetHeader::TYPE_IPv4};
        frame_out.payload() = dgram.serialize();
        _frames_out.push(frame_out);
    } else {
        // if the next hop is not in the arp table, send an arp request
        // if the arp request has not been sent, send it
        // otherwise keep waiting for arp reply
        if (_arp_requests_sent.find(next_hop_ip) == _arp_requests_sent.end()) {
            // construct arp message
            ARPMessage arp_msg;
            arp_msg.opcode = ARPMessage::OPCODE_REQUEST;
            arp_msg.sender_ip_address = _ip_address.ipv4_numeric();
            arp_msg.sender_ethernet_address = _ethernet_address;
            arp_msg.target_ip_address = next_hop_ip;
            arp_msg.target_ethernet_address = {};

            // construct arp request ethernet frame and send it
            EthernetFrame frame_out;
            frame_out.header() = {ETHERNET_BROADCAST, _ethernet_address, EthernetHeader::TYPE_ARP};
            frame_out.payload() = arp_msg.serialize();
            _frames_out.push(frame_out);

            // record arp request into waiting list waiting for arp reply
            _arp_requests_sent[next_hop_ip] = ARP_REQUEST_TTL_MS;
        }

        // record the datagram into waiting list waiting for arp reply
        _datagrams_waiting_list[next_hop_ip].push_back(dgram);
    }
}

//! \param[in] frame the incoming Ethernet frame
optional<InternetDatagram> NetworkInterface::recv_frame(const EthernetFrame &frame) {
    // if the frame is not for us, drop it
    if (frame.header().dst != _ethernet_address && frame.header().dst != ETHERNET_BROADCAST) {
        return {};
    }

    // if the frame is ip datagram, handle it
    if (frame.header().type == EthernetHeader::TYPE_IPv4) {
        IPv4Datagram dgram;
        if (dgram.parse(frame.payload()) == ParseResult::NoError) {
            return dgram;
        }
        return {};
    }

    // if the frame is arp message, handle it
    if (frame.header().type == EthernetHeader::TYPE_ARP) {
        ARPMessage arp_msg;
        if (arp_msg.parse(frame.payload()) != ParseResult::NoError) {
            return {};
        }

        // update arp table
        _arp_table[arp_msg.sender_ip_address] = {arp_msg.sender_ethernet_address, ARP_ENTRY_TTL_MS};

        // if the arp message is a reply
        if (arp_msg.opcode == ARPMessage::OPCODE_REPLY) {
            // if the arp reply is for us, send the datagrams waiting for this arp reply
            if (arp_msg.target_ip_address == _ip_address.ipv4_numeric()) {
                for (const auto &dgram : _datagrams_waiting_list[arp_msg.sender_ip_address]) {
                    send_datagram(dgram, Address::from_ipv4_numeric(arp_msg.sender_ip_address));
                }
                _datagrams_waiting_list.erase(arp_msg.sender_ip_address);
                _arp_requests_sent.erase(arp_msg.sender_ip_address);
            }
            return {};
        }

        // if the arp message is a request
        if (arp_msg.opcode == ARPMessage::OPCODE_REQUEST) {
            // if the arp request is for us, send an arp reply
            if (arp_msg.target_ip_address == _ip_address.ipv4_numeric()) {
                ARPMessage arp_reply;
                arp_reply.opcode = ARPMessage::OPCODE_REPLY;
                arp_reply.sender_ip_address = _ip_address.ipv4_numeric();
                arp_reply.sender_ethernet_address = _ethernet_address;
                arp_reply.target_ip_address = arp_msg.sender_ip_address;
                arp_reply.target_ethernet_address = arp_msg.sender_ethernet_address;

                EthernetFrame frame_out;
                frame_out.header() = {arp_msg.sender_ethernet_address, _ethernet_address, EthernetHeader::TYPE_ARP};
                frame_out.payload() = arp_reply.serialize();
                _frames_out.push(frame_out);
            }
            return {};
        }
    }
    return {};
}

//! \param[in] ms_since_last_tick the number of milliseconds since the last call to this method
void NetworkInterface::tick(const size_t ms_since_last_tick) {
    // update arp table
    for (auto it = _arp_table.begin(); it != _arp_table.end();) {
        if (it->second.ttl <= ms_since_last_tick) {
            it = _arp_table.erase(it);
        } else {
            it->second.ttl -= ms_since_last_tick;
            ++it;
        }
    }

    // update arp requests sent
    for (auto &entry : _arp_requests_sent) {
        if (entry.second <= ms_since_last_tick) {
            // send arp request again
            ARPMessage arp_message;
            arp_message.opcode = ARPMessage::OPCODE_REQUEST;
            arp_message.sender_ip_address = _ip_address.ipv4_numeric();
            arp_message.sender_ethernet_address = _ethernet_address;
            arp_message.target_ip_address = entry.first;
            arp_message.target_ethernet_address = {};

            // construct arp request ethernet frame and send it
            EthernetFrame frame_out;
            frame_out.header() = {ETHERNET_BROADCAST, _ethernet_address, EthernetHeader::TYPE_ARP};
            frame_out.payload() = arp_message.serialize();
            _frames_out.push(frame_out);

            // record arp request into waiting list waiting for arp reply
            entry.second = ARP_REQUEST_TTL_MS;
        } else {
            entry.second -= ms_since_last_tick;
        }
    }
}
