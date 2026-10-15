"""Exercise production dispatcher, parser, async reload, sockets and persistence on PTYs."""
import copy
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
os.chdir(ROOT)


def request(cmd, uart=None):
    body = {'cmd': cmd}
    if uart is not None:
        body['param'] = {'uart': uart}
    with socket.create_connection(('127.0.0.1', 29608), timeout=3) as connection:
        connection.sendall(json.dumps(body).encode())
        connection.shutdown(socket.SHUT_WR)
        chunks = []
        while chunk := connection.recv(65536):
            chunks.append(chunk)
    return json.loads(b''.join(chunks))


def port(number=0, routes=None, sbus=False):
    return {'uart_num': number, 'port_config': {
        'type': 'sbus' if sbus else 'normal', 'baudrate': 100000 if sbus else 115200,
        'data_bits': 8, 'stop_bits': 2 if sbus else 1, 'parity': 'e' if sbus else 'n'},
        'routes': routes or []}


def route(proto='udp', endpoint='local', direction='rxtx', address='127.0.0.1', number=0):
    if not number:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM if proto == 'udp' else socket.SOCK_STREAM) as probe:
            probe.bind(('127.0.0.1', 0))
            number = probe.getsockname()[1]
    return {'proto': proto, 'endpoint': endpoint, 'dir': direction, 'ip': address, 'port': number}


with tempfile.TemporaryDirectory(prefix='linkg-uart-test-') as temporary:
    disk = Path(temporary) / 'user.json'
    process = subprocess.Popen([str(ROOT / 'build/uart-test/uart_test_server'), str(disk)],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def control(name, value=0):
        process.stdin.write(f'{name} {value}\n')
        process.stdin.flush()
        result = process.stdout.readline().strip()
        assert result, (name, process.poll())
        return result

    def apply(config, expected='applied'):
        response = request(1013, config)
        assert response['code'] == 'ok', response
        if expected == 'none':
            assert response['data']['apply'] == 'none', response
            return response['data']
        assert response['data']['apply'] == 'applying', response
        job_id = int(response['data']['job_id'])
        started = time.monotonic()
        while time.monotonic() - started < 6:
            status = request(1014)['data']
            if status['apply']['state'] != 'applying':
                assert status['apply']['job_id'] == job_id
                assert status['apply']['state'] == expected, status
                return status
            time.sleep(.01)
        raise AssertionError('application did not complete')

    def rejects(config):
        result = request(1013, config)
        assert result['code'] == 'error', result

    try:
        assert process.stdout.readline().strip() == 'READY'
        initial = json.loads(disk.read_text())
        capabilities = request(1012)['data']
        assert capabilities['max_ports'] == 3 and capabilities['max_routes_per_port'] == 16
        assert [x['device_path'] for x in capabilities['ports']] == ['/dev/ttyS4', '/dev/ttyS3', '/dev/ttyS5']
        assert capabilities['sbus']['baudrate'] == 100000
        assert isinstance(capabilities['broadcast_addresses'], list)
        for baud in capabilities['baudrates']:
            candidate = port()
            candidate['port_config']['baudrate'] = baud
            apply([candidate])
        print('PASS capability list and all 17 software-accepted baudrates (without hardware claim)', flush=True)

        bad = [port(), port()]
        rejects(bad)
        for key, value in [('baudrate', 12345), ('data_bits', 9), ('stop_bits', 3), ('parity', 'x'), ('type', 'multicast')]:
            candidate = port()
            candidate['port_config'][key] = value
            rejects([candidate])
        for number in (1, 2):
            rejects([port(number, sbus=True)])
        candidate = port(sbus=True)
        candidate['port_config']['baudrate'] = 115200
        rejects([candidate])
        for address in ('256.1.2.3', '1.2.3', '01.2.3.4', '255.255.255.255', '224.1.1.1', '240.0.0.1', '0.1.2.3', '0.0.0.0'):
            rejects([port(routes=[route(endpoint='remote', address=address)])])
        for address in capabilities['broadcast_addresses']:
            rejects([port(routes=[route(endpoint='remote', address=address)])])
        for key, value in [('port', 0), ('port', 65536), ('port', 3.5), ('dir', 'bad'), ('proto', 'bad'), ('endpoint', 'multicast')]:
            candidate = route()
            candidate[key] = value
            rejects([port(routes=[candidate])])
        for key in ('uart_num', 'port_config', 'routes'):
            candidate = port()
            del candidate[key]
            rejects([candidate])
        candidate = port(routes=[route()])
        del candidate['routes'][0]['ip']
        rejects([candidate])
        candidate = port()
        candidate['enabled'] = True
        rejects([candidate])
        rejects([port(routes=[route(number=30100 + i) for i in range(17)])])
        apply([port(routes=[route(endpoint='remote', number=30100 + i) for i in range(16)])])
        print('PASS strict fields, enums, IP/port, SBUS constraints, 16/17 route boundary', flush=True)

        first = route()
        second = dict(first, ip='0.0.0.0')
        rejects([port(routes=[first]), port(1, routes=[second])])
        rejects([port(routes=[route(number=29600)])])
        rejects([port(routes=[route(proto='tcp', number=29608)])])
        apply([port(routes=[first, dict(first, proto='tcp')])])
        current = request(1012)['data']['uart']
        starts = control('counts').split()[1]
        apply(current, 'none')
        assert control('counts').split()[1] == starts
        print('PASS wildcard duplicate binds, protocol-specific reservations and no-op reload', flush=True)

        local = route()
        active = [port(routes=[local])]
        apply(active)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.bind(('127.0.0.1', 0))
            udp.settimeout(2)
            udp.sendto(b'from-network', ('127.0.0.1', local['port']))
            deadline = time.monotonic() + 2
            value = ''
            while time.monotonic() < deadline:
                value = control('read')
                if value == 'from-network':
                    break
                time.sleep(.01)
            assert value == 'from-network', value
            assert control('write') == '10'
            assert udp.recv(128) == b'linkg-test'
            local['dir'] = 'tx'
            active = [port(routes=[local])]
            apply(active)
            udp.sendto(b'learn-only', ('127.0.0.1', local['port']))
            time.sleep(.12)
            assert control('read') == 'EMPTY'
            control('write')
            assert udp.recv(128) == b'linkg-test'
        print('PASS real UDP/PTY bidirectional bytes and Local TX peer discovery without RX forwarding', flush=True)

        local_tcp = route(proto='tcp')
        apply([port(routes=[local_tcp])])
        with socket.create_connection(('127.0.0.1', local_tcp['port']), timeout=2) as tcp:
            tcp.sendall(b'tcp-tail')
        deadline = time.monotonic() + 2
        value = ''
        while time.monotonic() < deadline:
            value = control('read')
            if value == 'tcp-tail':
                break
            time.sleep(.01)
        assert value == 'tcp-tail', value
        with socket.create_connection(('127.0.0.1', local_tcp['port']), timeout=2) as tcp:
            tcp.settimeout(2)
            time.sleep(.05)
            assert control('write') == '10'
            assert tcp.recv(128) == b'linkg-test'
        print('PASS real TCP/PTY bytes including final readable data delivered with peer close', flush=True)

        for fault in ('init', 'start', 'open', 'before', 'after'):
            saved = json.loads(disk.read_text())
            old = request(1012)['data']['uart']
            new = copy.deepcopy(old)
            new[0]['port_config']['baudrate'] = 57600 if old[0]['port_config']['baudrate'] != 57600 else 115200
            control(fault, 1)
            apply(new, 'rolled_back')
            assert request(1012)['data']['uart'] == old
            assert json.loads(disk.read_text()) == saved
        print('PASS init/start/open failure and pre/post-rename persistence rollback', flush=True)

        for fault in ('gpio-rx', 'gpio-sbus'):
            old = request(1012)['data']['uart']
            new = copy.deepcopy(old)
            new[0]['port_config']['baudrate'] = 57600 if old[0]['port_config']['baudrate'] != 57600 else 115200
            control(fault, 1)
            apply(new, 'rolled_back')
            assert request(1012)['data']['uart'] == old
            assert request(1014)['data']['state'] in ('running', 'not_ready')
        print('PASS GPIO shutdown failures stay retryable and rollback restores the running service', flush=True)

        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as occupied:
            occupied.bind(('127.0.0.1', 0))
            apply([port(routes=[route(number=occupied.getsockname()[1])])], 'rolled_back')
        old = request(1012)['data']['uart']
        new = [port(routes=[route()])]
        control('delay', 400)
        accepted = request(1013, new)
        assert accepted['data']['apply'] == 'applying'
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            if request(1014)['data']['apply']['state'] == 'applying':
                break
        assert request(1013, new)['code'] == 'error'
        assert request(1012)['code'] == 'error'
        started = time.monotonic()
        assert request(1001)['code'] == 'ok'
        assert request(1006)['code'] == 'ok'
        assert request(1009)['code'] == 'ok'
        assert request(1011)['code'] == 'error'
        assert time.monotonic() - started < .3
        while request(1014)['data']['apply']['state'] == 'applying':
            time.sleep(.02)
        assert request(1012)['data']['uart'] == new
        print('PASS occupied sockets, duplicate SET exclusion, responsive Web reads, competing write rejection', flush=True)

        apply([port(routes=[route()])])
        baseline_fds = int(control('counts').split()[0])
        for index in range(24):
            apply([port(routes=[route()], sbus=index % 2 == 0)])
        counts = [int(x) for x in control('counts').split()]
        assert counts[0] == baseline_fds and counts[2] == 1 and counts[3] == 1, counts
        current = json.loads(disk.read_text())
        assert {k: v for k, v in current.items() if k != 'uart'} == {k: v for k, v in initial.items() if k != 'uart'}
        print('PASS 24 normal/SBUS reloads, no descriptor/Worker leak, all unrelated config preserved', flush=True)

        control('service-stop')
        stopped_old = request(1012)['data']['uart']
        stopped_failed = copy.deepcopy(stopped_old)
        stopped_failed[0]['port_config']['baudrate'] = 57600 if stopped_old[0]['port_config']['baudrate'] != 57600 else 115200
        control('gpio-sbus', 1)
        apply(stopped_failed, 'rolled_back')
        assert request(1012)['data']['uart'] == stopped_old
        stopped_status = request(1014)['data']
        assert stopped_status['state'] == 'stopped' and not stopped_status['worker_running'], stopped_status

        stopped_config = [port(routes=[route()])]
        apply(stopped_config)
        stopped_status = request(1014)['data']
        assert stopped_status['state'] == 'stopped' and not stopped_status['worker_running'], stopped_status
        control('service-start')
        assert request(1014)['data']['state'] in ('running', 'not_ready')
        print('PASS stopped-service GPIO cleanup rollback and successful Apply both preserve stopped state', flush=True)

        control('stop', 2)
        result = apply([port(routes=[route()])], 'failed')
        assert result['apply']['rollback_error'] != 0
        assert control('counts').split()[2] == '1'
        control('recover')
        control('before', 2)
        result = apply([port(routes=[route()])], 'failed')
        assert result['apply']['rollback_error'] != 0
        assert request(1012)['code'] == 'error'
        print('PASS failed join keeps live resources and failed rollback is never reported as success', flush=True)
    finally:
        if process.poll() is None:
            process.stdin.write('quit\n')
            process.stdin.flush()
            process.wait(timeout=10)
        assert process.returncode == 0, process.returncode
print('ALL UART API/runtime tests passed (ASan/UBSan enabled).')
