"""Independent local HTTP/LAN fixtures: no real provider or paid model calls."""
import contextlib
import datetime
import http.server
import json
import os
import pathlib
import socket
import subprocess
import tempfile
import threading
import time
from test_display_bridge import packet, receive, wait

ROOT = pathlib.Path(__file__).resolve().parents[1]
calls = []
mode = 'ok'


class Model(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        calls.append(body)
        if mode in ('rate', 'quota', 'unknown'):
            detail = {'rate': {'type': 'rate_limit_exceeded', 'message': 'too many requests secret-fixture-key'},
                      'quota': {'code': 'insufficient_quota', 'message': 'insufficient balance secret-fixture-key'},
                      'unknown': {'code': 1234, 'message': 'secret-fixture-key'}}[mode]
            data = json.dumps({'error': detail}).encode()
            self.send_response(429)
            self.send_header('Retry-After', '30')
        else:
            data = json.dumps({'choices': [{'message': {'content': '本地模拟回答'}}],
                               'usage': {'prompt_tokens': 10, 'completion_tokens': 20}}).encode()
            self.send_response(200)
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)


@contextlib.contextmanager
def bridge(folder, model, limit=100):
    lan = socket.socket(); lan.bind(('127.0.0.1', 0)); lan.listen(); lan.settimeout(10)
    probe = socket.socket(); probe.bind(('127.0.0.1', 0)); board_port = probe.getsockname()[1]; probe.close()
    (folder/'robomind-console.ini').write_text(f'[lanchat]\nenabled=true\nhost=127.0.0.1\nport={lan.getsockname()[1]}\nreceive_messages=true\n')
    status = folder/'status.json'
    env = dict(os.environ, ROBOMIND_LANCHAT_USER_ID='4242', ROBOMIND_LANCHAT_PASSWORD='isolated',
               ROBOMIND_AI_ENABLED='1', ROBOMIND_AI_PROVIDER='openai', ROBOMIND_AI_MODEL='fixture',
               ROBOMIND_AI_ENDPOINT=f'http://127.0.0.1:{model.server_port}', ROBOMIND_AI_KEY='secret-fixture-key',
               ROBOMIND_AI_MAX_TOKENS='777', ROBOMIND_AI_TIMEOUT_SECONDS='12', ROBOMIND_AI_DAILY_LIMIT=str(limit),
               ROBOMIND_AI_INTERVAL_SECONDS='0', ROBOMIND_AI_CONCURRENCY='1', ROBOMIND_AI_USAGE_FILE=str(folder/'usage.json'))
    stop = folder/'stop.request'
    stop.unlink(missing_ok=True)
    proc = subprocess.Popen([str(ROOT/'build/dist/RoboMindConsole.exe'), '--bridge', '--port', str(board_port),
                             '--status-file', str(status), '--stop-file', str(stop)], cwd=folder, env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    client = None
    try:
        client, _ = lan.accept(); client.settimeout(10)
        assert json.loads(receive(client))['type'] == 2
        client.sendall(packet({'type': 3, 'id': 4242}))
        def state():
            try: return json.loads(status.read_text(encoding='utf-8'))
            except (OSError, ValueError): return {}
        wait(lambda: state().get('lanchat_state') == '已登录')
        def ask(text, sender=101):
            original = {'type': 6, 'fromId': sender, 'toId': 4242, 'msg_id': str(time.time_ns()), 'msg': text}
            client.sendall(packet(original))
            while True:
                try:
                    response = json.loads(receive(client))
                except TimeoutError:
                    raise AssertionError(f'LAN reply timeout: state={state()} log='+(folder/'lanchat-sink.log').read_text(encoding='utf-8')[-1500:])
                if response['type'] == 5:
                    assert response['toId'] == sender
                    assert 'secret-fixture-key' not in response['msg']
                    return response['msg']
        yield ask, state
    finally:
        stop.write_text('stop')
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill(); proc.wait()
        if client: client.close()
        lan.close()


def run():
    global mode
    model = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Model)
    threading.Thread(target=model.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix='ai-limits-') as directory:
            root = pathlib.Path(directory)
            for category, expected in [('rate', '限流'), ('quota', '余额不足'), ('unknown', '未明确区分')]:
                folder = root/category; folder.mkdir(); mode = category
                before = len(calls)
                with bridge(folder, model) as (ask, state):
                    assert expected in ask('自由提问')
                    assert '冷却' in ask('下一题', sender=102)
                    assert '设备' in ask('设备', sender=103)
                    wait(lambda: state()['ai']['failed'] == 1)
                    assert state()['ai']['error_kind'] == {'rate': 'rate_limit', 'quota': 'quota', 'unknown': 'http_429'}[category]
                with bridge(folder, model) as (ask, state):
                    assert '冷却' in ask('重启后的下一题')
                assert len(calls) == before+1
                assert 'secret-fixture-key' not in (folder/'usage.json').read_text()
            mode = 'ok'; folder = root/'daily'; folder.mkdir(); before = len(calls)
            with bridge(folder, model, limit=1) as (ask, state):
                assert '模拟回答' in ask('第一题')
                assert '每日调用' in ask('超限问题', sender=102)
                wait(lambda: state()['ai']['daily_calls'] == 1)
                assert state()['ai']['input_tokens'] == 10 and state()['ai']['output_tokens'] == 20
                assert calls[-1]['max_tokens'] == 777
            with bridge(folder, model, limit=1) as (ask, state):
                assert '每日调用' in ask('重启不能绕过上限')
                assert '已清空' in ask('/AI 清空')
            assert len(calls) == before+1
            ledger = json.loads((folder/'usage.json').read_text())
            ledger['day'] = (datetime.date.today()-datetime.timedelta(days=1)).isoformat()
            (folder/'usage.json').write_text(json.dumps(ledger))
            with bridge(folder, model, limit=1) as (ask, state):
                assert '模拟回答' in ask('新的一天')
                wait(lambda: state()['ai']['daily_calls'] == 1)
                assert state()['ai']['input_tokens'] == 10
            print('PASS AI limits: rate/quota/unknown 429, Retry-After cooldown, restart persistence, deterministic queries, daily cap/reset, token statistics, configured output, secrets omitted')
    finally:
        model.shutdown(); model.server_close()


if __name__ == '__main__':
    run()
