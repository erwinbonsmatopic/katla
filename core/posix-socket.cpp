/***
 * Copyright 2019 The Katla Authors
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 * http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "posix-socket.h"

#include "fmt/format.h"

#include <asm-generic/errno.h>
#include <sys/poll.h>
#include <vector>

#include <unistd.h>
#include <cstring>
#include <system_error>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>

#include <net/ethernet.h>
#include <net/if.h>

#include <arpa/inet.h>

#include <poll.h>

namespace katla {

PosixSocket::PosixSocket(ProtocolDomain protocolDomain, Type type, FrameType frameType, bool nonBlocking) :
    _protocolDomain(protocolDomain),
    _type(type),
    _frameType(frameType),
    _nonBlocking(nonBlocking)
{
}

PosixSocket::PosixSocket(ProtocolDomain protocolDomain, Type type, FrameType frameType, bool nonBlocking, int fd) :
        _fd(fd),
        _protocolDomain(protocolDomain),
        _type(type),
        _frameType(frameType),
        _nonBlocking(nonBlocking)
{
}

PosixSocket::~PosixSocket()
{
    if (_fd != -1) {
        ::close(_fd);
    }
}

outcome::result<std::array<std::shared_ptr<PosixSocket>,2>> PosixSocket::createUnnamedPair(ProtocolDomain protocolDomain, Type type, FrameType frameType, bool nonBlocking)
{
    PosixErrorCategory errorCategory;

    int mappedDomain = mapProtocolDomain(protocolDomain);
    if (mappedDomain == -1) {
        std::error_code errorCode(static_cast<int>(PosixErrorCodes::InvalidDomain), errorCategory);
        return errorCode;
    }

    int mappedType = mapType(type);
    if (mappedType == -1) {
        std::error_code errorCode(static_cast<int>(PosixErrorCodes::InvalidType), errorCategory);
        return errorCode;
    }

    if (nonBlocking) {
        mappedType |= SOCK_NONBLOCK;
    }

    int sd[2] = {-1,-1};
    int result = socketpair(mappedDomain, mappedType, 0, sd);
    if (result != 0) {
        return std::make_error_code(static_cast<std::errc>(errno));
    }

    return outcome::success(std::array<std::shared_ptr<PosixSocket>,2>{
            std::shared_ptr<PosixSocket>(new PosixSocket(protocolDomain, type, frameType, nonBlocking, sd[0])),
            std::shared_ptr<PosixSocket>(new PosixSocket(protocolDomain, type, frameType, nonBlocking, sd[1]))
    });
}

int PosixSocket::mapProtocolDomain(ProtocolDomain protocolDomain) {
    switch(protocolDomain) {
        case ProtocolDomain::Unix: return AF_UNIX;
        case ProtocolDomain::IPv4: return AF_INET;
        case ProtocolDomain::IPv6: return AF_INET6;
        case ProtocolDomain::Packet: return AF_PACKET;
        case ProtocolDomain::Can: return AF_CAN;
        case ProtocolDomain::Bluetooth: return AF_BLUETOOTH;
        case ProtocolDomain::VSock: return AF_VSOCK;
    }

    return -1;
}

int PosixSocket::mapType(Type type) {
    switch(type) {
        case Type::Stream: return SOCK_STREAM;
        case Type::Datagram: return SOCK_DGRAM;
        case Type::SequencedPacket: return SOCK_SEQPACKET;
        case Type::Raw: return SOCK_RAW;
    }

    return -1;
}

outcome::result<void> PosixSocket::bind(std::string url)
{
    if (_protocolDomain == ProtocolDomain::Packet && _type == Type::Raw) {
        auto result = create();
        if (!result) {
            return result.error();
        }

        auto nameToIndexResult = if_nametoindex(url.c_str());
        if (nameToIndexResult == 0) {
            fmt::print(stderr, "Failed finding adapter: {}: {}\n", nameToIndexResult, url);
            return std::make_error_code(static_cast<std::errc>(errno));
        }

        sockaddr_ll destAddress = {};
        destAddress.sll_family = AF_PACKET;
        destAddress.sll_protocol = htons( static_cast<uint16_t> (_frameType));
        destAddress.sll_ifindex = static_cast<int>(nameToIndexResult);
        destAddress.sll_pkttype = PACKET_MULTICAST;

        auto bindResult = ::bind(_fd, reinterpret_cast<sockaddr*>(&destAddress), sizeof(destAddress));
        if (bindResult == -1) {
            fmt::print(stderr, "Failed binding adapter: {}: {}\n", nameToIndexResult, url);
            return std::make_error_code(static_cast<std::errc>(errno));
        }

        _url = url;

        struct packet_mreq mreq = {};
        mreq.mr_ifindex = static_cast<int>(nameToIndexResult);
        mreq.mr_type = PACKET_MR_PROMISC;
        if (setsockopt(_fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) == -1) {
            fmt::print(stderr, "Failed setting socket options on: {}: {}\n", nameToIndexResult, url);
            return std::make_error_code(static_cast<std::errc>(errno));
        }
    } else if (_protocolDomain == ProtocolDomain::Unix) {
        auto result = create();
        if (!result) {
            return result.error();
        }

        if (url.length() >= 108) {
            return make_error_code(PosixErrorCodes::UnixSocketPathTooLong);
        }

        sockaddr_un bindAddress = {};
        bindAddress.sun_family = AF_UNIX;
        strncpy(bindAddress.sun_path, url.c_str(), url.length() + 1);

        auto bindResult = ::bind(_fd, reinterpret_cast<sockaddr*>(&bindAddress), sizeof(bindAddress));
        if (bindResult == -1) {
            fmt::print(stderr, "Failed binding to path: {}\n", url);
            return std::make_error_code(static_cast<std::errc>(errno));
        }
    } else {
        return make_error_code(PosixErrorCodes::OperationNotSupported);
    }

    return outcome::success();
}

outcome::result<void> PosixSocket::connect(std::string url)
{
    if (_protocolDomain == ProtocolDomain::Unix) {
        auto result = create();
        if (!result) {
            return result.error();
        }

        if (url.length() >= 108) {
            return make_error_code(PosixErrorCodes::UnixSocketPathTooLong);
        }

        sockaddr_un bindAddress = {};
        bindAddress.sun_family = AF_UNIX;
        strncpy(bindAddress.sun_path, url.c_str(), url.length() + 1);

        auto connectResult = ::connect(_fd, reinterpret_cast<sockaddr*>(&bindAddress), sizeof(bindAddress));
        if (connectResult == -1) {
            fmt::print(stderr, "Failed connecting to path: {}\n", url);
            return std::make_error_code(static_cast<std::errc>(errno));
        }

        return outcome::success();
    }

    return make_error_code(PosixErrorCodes::OperationNotSupported);
}

outcome::result<PosixSocket::WaitResult> PosixSocket::poll(std::chrono::milliseconds timeout, bool writePending)
{
    pollfd pollDescriptor {
        _fd,
        POLLIN | POLLPRI | POLLRDHUP,
        0
    };

    // If we have bytes to send we want to return early to send our bytes
    if (writePending) {
        pollDescriptor.events |= POLLOUT;
    }

    auto result = ::poll(&pollDescriptor, 1, static_cast<int>(timeout.count()));
    if (result == -1) {
        return std::make_error_code(static_cast<std::errc>(errno));
    }

    WaitResult waitResult;
    waitResult.dataToRead = (pollDescriptor.revents & POLLIN) || (pollDescriptor.revents & POLLPRI);
    waitResult.urgentDataToRead = (pollDescriptor.revents & POLLPRI);
    waitResult.writingWillNotBlock = (pollDescriptor.revents & POLLOUT);
    waitResult.readHangup = (pollDescriptor.revents & POLLRDHUP);
    waitResult.writeHangup = (pollDescriptor.revents & POLLHUP);
    waitResult.error = (pollDescriptor.revents & POLLERR);
    waitResult.invalid = (pollDescriptor.revents & POLLNVAL);

    return waitResult;
}

outcome::result<ssize_t> PosixSocket::read(const gsl::span<std::byte>& buffer)
{
    return receiveFrom(buffer);
}

outcome::result<ssize_t> PosixSocket::receiveFrom(const gsl::span<std::byte>& buffer)
{
    int flags = 0;
    if (_nonBlocking) {
        flags |= MSG_DONTWAIT;
    }

    ssize_t nbytes = ::recvfrom(_fd, buffer.data(), buffer.size(), flags, nullptr, nullptr);
    if (nbytes == -1) {
        bool wouldBlock = false;
        if (_nonBlocking && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // TODO TEST??
            nbytes = 0;
        } else {
            return std::make_error_code(static_cast<std::errc>(errno));
        }
    }

    return nbytes;
}

outcome::result<ssize_t> PosixSocket::write(const gsl::span<std::byte>& buffer)
{
    ssize_t nbytes = ::write(_fd, buffer.data(), buffer.size());

    if (nbytes == -1) {
        return std::make_error_code(static_cast<std::errc>(errno));
    }

    return nbytes;
}

outcome::result<ssize_t> PosixSocket::sendTo(std::string url, const gsl::span<std::byte>& buffer)
{
    if (_protocolDomain == ProtocolDomain::Packet && _type == Type::Raw) {
        auto result = create();
        if (!result) {
            return result.error();
        }

        auto nameToIndexResult = if_nametoindex(url.c_str());
        if (nameToIndexResult == 0) {
            fmt::print(stderr, "Failed finding adapter: {}: {}\n", nameToIndexResult, url);
            return std::make_error_code(static_cast<std::errc>(errno));
        }

        sockaddr_ll destAddress = {};
        destAddress.sll_family = AF_PACKET;
        destAddress.sll_protocol = htons( static_cast<uint16_t> (_frameType));
        destAddress.sll_ifindex = nameToIndexResult;

        std::array<uint8_t, 8> addr = {1,1,5,4,0,0};
        ::memcpy(destAddress.sll_addr, addr.data(), addr.size());
        destAddress.sll_halen = 6;

        ssize_t nbytes = ::sendto(_fd, buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr*>(&destAddress), sizeof(sockaddr_ll));
        return nbytes;

        if (nbytes == -1) {
            return std::make_error_code(static_cast<std::errc>(errno));
        }
    }
    
    return make_error_code(PosixErrorCodes::OperationNotSupported);
}

outcome::result<void> PosixSocket::close()
{
    if (_fd == -1) {
        return outcome::success();
    }

    int status = ::close(_fd);
    if (status == -1) {
        return std::make_error_code(static_cast<std::errc>(errno));
    }

    _fd = -1;

    return outcome::success();
}

outcome::result<void> PosixSocket::create()
{
    int domain = mapProtocolDomain(_protocolDomain);
    if (domain == -1) {
        return make_error_code(PosixErrorCodes::InvalidDomain);
    }

    int mappedType = mapType(_type);
    if (mappedType == -1) {
        return make_error_code(PosixErrorCodes::InvalidType);
    }

    if (_nonBlocking) {
        mappedType |= SOCK_NONBLOCK;
    }

    // Only set frame/protocol type for RAW sockets for now
    uint16_t frameType = 0;
    if (_type == Type::Raw) {
        frameType = static_cast<uint16_t> ( htons( frameType ));
    }

    _fd = socket(domain, mappedType, frameType);
    if (_fd == -1) {
        // TODO check root access
        return std::make_error_code(static_cast<std::errc>(errno));
    }

    return outcome::success();
}


}
