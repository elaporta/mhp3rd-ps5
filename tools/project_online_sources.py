#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Maintained native transport transforms and reproducible protocol regression oracles."""
def adapt(name, text, module):
    replace, function, body_end = module.replace, module.function, module.body_end
    if name.endswith('/adhoc/client.cpp'):
        signature = 'std::vector<Address> resolve(const std::string &host, std::uint16_t port)'
        opening = text.index('{', text.index(signature))
        original = text[opening+1:body_end(text, opening)-1]
        text = function(text, signature, '''#if YAKUMO_NATIVE
    const auto started = Clock::now();
    const auto found = yakumo::adhoc_resolver::resolve_ipv4(host);
    const auto elapsed = std::chrono::duration_cast<milliseconds>(Clock::now()-started).count();
    Client::log(std::string("[ps5-adhoc-resolve] stage=")+found.stage+" rc="+std::to_string(found.rc)+
        " cleanup_rc="+std::to_string(found.cleanup_rc)+" elapsed_ms="+std::to_string(elapsed)+
        " timeout_us=2000000 timeout_unit=microseconds retries=1", true);
    if (!found.ok()) return {};
    return {Address::ipv4_address(found.address, port)};
#else
''' + original + '\n#endif')
        text = '#include "adhoc_resolver.hpp"\n' + text
        text = '#include "adhoc_socket.hpp"\n#define YAKUMO_SCENET (YAKUMO_NATIVE && !YAKUMO_ADHOC_BSD_FIXTURE)\n' + text
        text = '#include "adhoc_relay_ports.hpp"\n' + text
        text = '#include "adhoc_log.hpp"\n' + text
        signature = 'void Client::log(const std::string &line, bool print)'
        opening = text.index('{', text.index(signature))
        original = text[opening+1:body_end(text, opening)-1]
        text = function(text, signature, '''#if YAKUMO_NATIVE
    if (!print && !tracing()) return;
    yakumo::adhoc_log::enqueue(line, print);
    LogBuffer &buffer = log_buffer();
    std::lock_guard lock(buffer.mutex);
    buffer.lines.push_back(wall_clock() + " " + line);
    if (buffer.lines.size() > kLogLines) buffer.lines.pop_front();
#else
''' + original + '\n#endif')
        # Only this native client's relay boundary changes. The desktop client,
        # room service, server and game-visible ports retain their pinned behavior.
        if text.count('relay::init(') != 4:
            raise ValueError('Expected all four relay init paths')
        text = text.replace('relay::init(', 'online_relay_init(')
        text = replace(text, 'relay::pdp_header(', 'online_pdp_header(')
        text = replace(text, 'using Clock = std::chrono::steady_clock;', '''using Clock = std::chrono::steady_clock;
std::uint16_t relay_out_port(std::uint16_t port) {
#if YAKUMO_SCENET
    return yakumo::adhoc_relay_ports::to_wire(port);
#else
    return port;
#endif
}
std::uint16_t relay_in_port(std::uint16_t port) {
#if YAKUMO_SCENET
    return yakumo::adhoc_relay_ports::from_wire(port);
#else
    return port;
#endif
}
std::string online_relay_init(relay::InitType type, const Mac& source, std::uint16_t source_port,
    const Mac& destination, std::uint16_t destination_port) {
    return relay::init(type, source, relay_out_port(source_port), destination, relay_out_port(destination_port));
}
std::string online_pdp_header(const Mac& target, std::uint16_t port, std::uint32_t size) {
    return relay::pdp_header(target, relay_out_port(port), size);
}''')
        text = replace(text, 'datagram.port = wire::get16(in.data() + offset + 8);',
                       'datagram.port = relay_in_port(wire::get16(in.data() + offset + 8));')
        text = replace(text, 'pending.port = wire::get16(in.data() + offset + 8);',
                       'pending.port = relay_in_port(wire::get16(in.data() + offset + 8));')
        text = replace(text, 'std::to_string(wire::get16(in.data() + 8))',
                       'std::to_string(relay_in_port(wire::get16(in.data() + 8)))')
        for alias in ('close_socket','set_nonblocking'):
            text = replace(text, 'using net::'+alias+';', '#if YAKUMO_SCENET\nusing yakumo::adhoc_socket::'+alias+';\n#else\nusing net::'+alias+';\n#endif')
        tune_signature='void tune_socket(Socket s)'
        tune_open=text.index('{',text.index(tune_signature))
        tune_original=text[tune_open+1:body_end(text,tune_open)-1]
        text=function(text,tune_signature,'#if YAKUMO_SCENET\n    yakumo::adhoc_socket::tune(s);\n#else\n'+tune_original+'\n#endif')
        rtt_signature='std::optional<double> ctl_rtt_ms() const'
        rtt_open=text.index('{',text.index(rtt_signature))
        rtt_original=text[rtt_open+1:body_end(text,rtt_open)-1]
        text=function(text,rtt_signature,'#if YAKUMO_SCENET\n        return std::nullopt;\n#else\n'+rtt_original+'\n#endif')
        for original,native in (
            ('socket = ::socket(address.storage.ss_family, SOCK_STREAM, IPPROTO_TCP);','socket = yakumo::adhoc_socket::create(address.storage.ss_family, SOCK_STREAM, IPPROTO_TCP);'),
            ('if (::connect(socket, reinterpret_cast<const sockaddr *>(&address.storage), address.length) == 0) {','if (yakumo::adhoc_socket::connect(socket, reinterpret_cast<const sockaddr *>(&address.storage), address.length) == 0) {'),
            ('const auto count = ::recv(socket, buffer, static_cast<int>(sizeof(buffer)), 0);','const auto count = yakumo::adhoc_socket::recv(socket, buffer, sizeof(buffer), 0);'),
            ('const auto count = ::send(socket, output.data(), static_cast<int>(output.size()), kSendFlags);','const auto count = yakumo::adhoc_socket::send(socket, output.data(), output.size(), kSendFlags);')):
            text=replace(text,original,'\n#if YAKUMO_SCENET\n        '+native+'\n#else\n        '+original+'\n#endif')
        text=replace(text,'        if (socket == kNoSocket || !set_nonblocking(socket)) {','''        if (socket == kNoSocket) {
            Client::log("[ps5-adhoc-socket] stage=create errno="+std::to_string(socket_error()), true);
            failed = true;
            return false;
        }
        if (!set_nonblocking(socket)) {''')
        text=replace(text,'        std::vector<PollEntry> entries;','''#if YAKUMO_SCENET
        yakumo::adhoc_socket::Poller native_poller;
#endif
        std::vector<PollEntry> entries;''')
        text=replace(text,'            const int ready = poll_sockets(entries.data(), entries.size(), kPollIntervalMs);','''#if YAKUMO_SCENET
            const int ready = native_poller.wait(entries.data(), entries.size(), kPollIntervalMs);
            const bool poll_failed = ready < 0 && socket_error() != EINTR;
            if (poll_failed) {
                Client::log("[ps5-adhoc-socket] stage=poll errno="+std::to_string(socket_error()), true);
            }
#else
            const int ready = poll_sockets(entries.data(), entries.size(), kPollIntervalMs);
            const bool poll_failed = false;
#endif''')
        text=replace(text,'link.service(ready > 0 ? entries[i].revents : static_cast<short>(0));','''if (poll_failed) link.fail();
                else link.service(ready > 0 ? entries[i].revents : static_cast<short>(0));''')
        text = replace(text, '        impl_->quit = true;', '''        impl_->quit = true;
#if YAKUMO_NATIVE
        yakumo::adhoc_resolver::shutdown();
#endif''')
        text = replace(text, '        if (!set_nonblocking(socket)) {', '''        if (!set_nonblocking(socket)) {
            Client::log("[ps5-adhoc-socket] stage=nonblocking errno="+std::to_string(socket_error()), true);''')
        text = replace(text, '        if (!connect_pending(socket_error())) {', '''        if (!connect_pending(socket_error())) {
            Client::log("[ps5-adhoc-socket] connect errno="+std::to_string(socket_error()), true);''')
        text = replace(text, '                getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &length);\n                if (error != 0) {', '''                const int checked = getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &length);
                if (checked != 0 || length != sizeof(error) || error != 0) {
                    Client::log("[ps5-adhoc-socket] connect-complete rc="+std::to_string(checked)+
                        " errno="+std::to_string(socket_error())+" socket_error="+std::to_string(error), true);''')
        text=replace(text,'const int checked = getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &length);','''
#if YAKUMO_SCENET
                const int checked = yakumo::adhoc_socket::get_error(socket, &error, &length);
#else
                const int checked = getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &length);
#endif''')
    if name.endswith('/hle/hle_adhoc.cpp'):
        signature = 'void trace_line(const std::string &line)'
        opening = text.index('{', text.index(signature))
        original = text[opening+1:body_end(text, opening)-1]
        # HLE trace records are routine diagnostics, not priority errors.
        routine = replace(original, '        true);', '        false);')
        text = function(text, signature, '#if YAKUMO_NATIVE\n'+routine+'\n#else\n'+original+'\n#endif')
    if name.endswith('/adhoc/sockets.hpp'):
        text = replace(text, '        in->sin_family = AF_INET;', '''#if YAKUMO_NATIVE && !defined(__linux__) && !defined(_WIN32)
        in->sin_len = sizeof(sockaddr_in);
#endif
        in->sin_family = AF_INET;''')
    return text


def peer_fixture(module):
    fixture = (module.UP/'profiles/mhp3rd/tests/adhoc_tests.cpp').read_text()
    for old,new in (
        ('relay::init(relay::kInitPdp, kMacB, 10000,', 'relay::init(relay::kInitPdp, kMacB, 20000,'),
        ('wire::get16(header->data() + 8) == 10000', 'wire::get16(header->data() + 8) == 20000'),
        ('relay::pdp_header(kMacA, 10000,', 'relay::pdp_header(kMacA, 20000,'),
        ('relay::pdp_header(Mac{0x02, 9, 9, 9, 9, 9}, 10000,', 'relay::pdp_header(Mac{0x02, 9, 9, 9, 9, 9}, 20000,'),
        ('relay::init(relay::kInitPtpConnect, kMacB, 30000, kMacA, 20001)', 'relay::init(relay::kInitPtpConnect, kMacB, 40000, kMacA, 30001)'),
        ('wire::get16(established->data() + 8) == 20001', 'wire::get16(established->data() + 8) == 30001'),
        ('relay::init(relay::kInitPtpListen, kMacB, 20002,', 'relay::init(relay::kInitPtpListen, kMacB, 30002,'),
        ('relay::init(relay::kInitPtpAccept, kMacB, 20002,', 'relay::init(relay::kInitPtpAccept, kMacB, 30002,'),
    ):
        fixture = module.replace(fixture, old, new)
    fixture = module.replace(fixture, '"A gets B\'s connection request");',
        '"A gets B\'s connection request");\n    check(accepted && a.ptp_info(accepted).peer_port == 30000, "PTP accept exposes the guest peer port, not wire 40000");')
    return fixture


def regression_client(text, revision, module):
    if revision not in (4, 5):
        raise ValueError('Only the r4/r5 negative client oracles are supported')
    replace = module.replace
    text = replace(text, '#include "adhoc_log.hpp"\n', '')
    signature = 'void Client::log(const std::string &line, bool print)'
    opening = text.index('{', text.index(signature))
    end = module.body_end(text, opening)
    body = text[opening+1:end-1]
    body = body[body.index('#else')+len('#else'):body.rindex('#endif')]
    # The accepted wrapper retains one blank line around the desktop branch.
    text = text[:opening+1] + body[1:-1] + text[end-1:]
    if revision == 4:
        text = replace(text, '#include "adhoc_relay_ports.hpp"\n', '')
        begin = text.index('std::uint16_t relay_out_port(')
        end = text.index('using std::chrono::milliseconds;', begin)
        text = text[:begin] + text[end:]
        text = text.replace('online_relay_init(', 'relay::init(').replace('online_pdp_header(', 'relay::pdp_header(')
        text = replace(text, 'relay_in_port(wire::get16(in.data() + offset + 8))', 'wire::get16(in.data() + offset + 8)') if text.count('relay_in_port(wire::get16(in.data() + offset + 8))') == 1 else text.replace('relay_in_port(wire::get16(in.data() + offset + 8))', 'wire::get16(in.data() + offset + 8)')
        text = replace(text, 'relay_in_port(wire::get16(in.data() + 8))', 'wire::get16(in.data() + 8)')
    return text
