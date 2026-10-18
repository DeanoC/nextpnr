#!/usr/bin/env python3
"""Run the GPU router's GPU_LEASE_FD client against a scripted gpu-run broker.

nextpnr-mistral routes an empty design with --router gpu while a fake broker
serves the other end of an AF_UNIX socketpair, as `gpu-run --phase-lease`
does (docs/gpurouter.md). Granted ids name a GPU no machine has, so no
device is ever opened: the router falls back to the CPU reference backend,
or fails with --gpu-required, with and without a device backend.
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

NEXTPNR = sys.argv[1]
ABSENT_GPU = 4096
FALLBACK = 'falling back to the CPU reference backend'
LEASE_REQUIRED = 'GPU use is required, but the GPU lease was not granted'


class Broker(threading.Thread):
    """Answer each ACQUIRE with the next scripted reply (None hangs up) and
    RELEASE with OK, recording every request."""

    def __init__(self, sock, replies):
        super().__init__(daemon=True)
        self.sock, self.replies, self.requests = sock, list(replies), []

    def run(self):
        with self.sock, self.sock.makefile('rb') as lines:
            for raw in lines:
                request = raw.decode('ascii', 'replace').rstrip('\n')
                self.requests.append(request)
                if request == 'ACQUIRE 1':
                    reply = self.replies.pop(0) if self.replies else 'ERR nothing-scripted'
                    if reply is None:
                        return
                    self.sock.sendall(reply.encode('ascii') + b'\n')
                elif request == 'RELEASE':
                    self.sock.sendall(b'OK\n')
                else:
                    self.sock.sendall(b'ERR unknown-command\n')


def route(design, replies, *args, advertise=True):
    broker_end, client_end = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
    broker = Broker(broker_end, replies)
    broker.start()
    # gpu-run exports NEXTPNR_REQUIRE_GPU=1; each case chooses --gpu-required itself
    env = {key: value for key, value in os.environ.items() if key not in ('GPU_LEASE_FD', 'NEXTPNR_REQUIRE_GPU')}
    if advertise:
        env['GPU_LEASE_FD'] = str(client_end.fileno())
    try:
        result = subprocess.run([NEXTPNR, '--device', '5CSEBA6U23I7', '--json', str(design), '--router', 'gpu', *args],
                                env=env, pass_fds=(client_end.fileno(),), capture_output=True, text=True,
                                timeout=600)
    finally:
        client_end.close()
    broker.join(timeout=60)
    if broker.is_alive():
        raise SystemExit('the fake broker never saw EOF')
    return result.returncode, result.stdout + result.stderr, broker.requests


def check(name, ok, output, requests):
    if not ok:
        raise SystemExit(f'{name}: unexpected result, broker saw {requests}:\n{output[-4000:]}')
    print('PASS', name, requests, flush=True)


with tempfile.TemporaryDirectory(prefix='nextpnr-gpu-lease-') as temporary:
    design = Path(temporary) / 'empty.json'
    design.write_text(json.dumps({'modules': {'top': {
        'attributes': {'top': '1'}, 'ports': {}, 'cells': {}, 'netnames': {}}}}), encoding='utf-8')

    # No GPU_LEASE_FD: no protocol and no new logs, even with the socket inherited
    code, out, requests = route(design, [], '--gpu-device', str(ABSENT_GPU), advertise=False)
    check('no GPU_LEASE_FD', code == 0 and requests == [] and 'GPU lease' not in out, out, requests)

    # --gpu-cpu opens no device, so it takes no lease
    code, out, requests = route(design, [], '--gpu-cpu')
    check('--gpu-cpu', code == 0 and requests == [] and 'GPU lease' not in out, out, requests)

    # A grant is used as the device as it is, in place of --gpu-device. This
    # one cannot be opened, so the router falls back to the CPU and gives the
    # GPU straight back
    code, out, requests = route(design, [f'GRANT {ABSENT_GPU}'] * 8, '--gpu-device', str(ABSENT_GPU - 1))
    check('grant', code == 0 and 'GPU lease: waited ' in out and f'granted physical id {ABSENT_GPU}' in out and
          f'using physical id {ABSENT_GPU} instead of the configured device {ABSENT_GPU - 1}' in out and
          FALLBACK in out and requests and requests == ['ACQUIRE 1', 'RELEASE'] * (len(requests) // 2),
          out, requests)

    # With --gpu-required the unusable grant is a hard error, and the lease is
    # still released as the router unwinds
    code, out, requests = route(design, [f'GRANT {ABSENT_GPU}'], '--gpu-required')
    check('grant, --gpu-required', code != 0 and f'granted physical id {ABSENT_GPU}' in out and
          'GPU use is required, but the GPU router could not open a device' in out and
          requests == ['ACQUIRE 1', 'RELEASE'], out, requests)

    # ERR, a closed socket and a malformed reply: today's CPU fallback without
    # --gpu-required, a hard error with it. Nothing was granted, so nothing is
    # released
    for name, replies, reason in (('ERR', ['ERR no-gpu-free'], 'no-gpu-free'),
                                  ('closed socket', [None], 'closed the connection'),
                                  ('malformed reply', ['GRANT one'], 'malformed reply')):
        code, out, requests = route(design, replies)
        check(name, code == 0 and 'GPU lease not granted' in out and reason in out and FALLBACK in out and
              requests == ['ACQUIRE 1'], out, requests)
        code, out, requests = route(design, replies, '--gpu-required')
        check(name + ', --gpu-required', code != 0 and LEASE_REQUIRED in out and reason in out and
              FALLBACK not in out and requests == ['ACQUIRE 1'], out, requests)
