"""Real Console + independent LAN/board peers + bounded local model fixture."""
import argparse
import http.server
import json
import os
import pathlib
import queue
import socket
import subprocess
import tempfile
import threading
import time
from urllib.parse import parse_qsl
from test_display_bridge import packet, envelope, receive, wait

ROOT = pathlib.Path(__file__).resolve().parents[1]


def run(provider, console=None):
    calls = []
    mode = ['ok']
    class Model(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass
        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            calls.append((self.path, body, self.headers.get('Authorization')))
            if mode[0] == 'error':
                self.send_response(401); self.end_headers(); self.wfile.write(b'{"error":"secret-server-detail"}'); return
            if mode[0] == 'slow':
                time.sleep(.7)
            answer = '根据本轮快照，感知结果未计算，ZAO_MINI 未接入。'
            response = {'content': [{'type': 'text', 'text': answer}]} if provider == 'anthropic' else {'choices': [{'message': {'content': answer}}]}
            if mode[0]=='limited':
                if provider=='anthropic':response['stop_reason']='max_tokens'
                else:response['choices'][0]['finish_reason']='length'
            data = json.dumps(response, ensure_ascii=False).encode()
            self.send_response(200); self.send_header('Content-Length', str(len(data))); self.end_headers(); self.wfile.write(data)
    model = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Model)
    threading.Thread(target=model.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory(prefix='ai-status-test-') as directory:
        folder = pathlib.Path(directory)
        status, stop = folder/'status.json', folder/'stop.request'
        lan = socket.socket(); lan.bind(('127.0.0.1', 0)); lan.listen(); lan.settimeout(10)
        probe = socket.socket(); probe.bind(('127.0.0.1', 0)); board_port = probe.getsockname()[1]; probe.close()
        (folder/'robomind-console.ini').write_text(f'[lanchat]\nenabled=true\nhost=127.0.0.1\nport={lan.getsockname()[1]}\ngroup_id=77\nreceive_messages=true\n')
        env = os.environ.copy()
        env.update(ROBOMIND_LANCHAT_USER_ID='4242', ROBOMIND_LANCHAT_PASSWORD='isolated-password',
                   ROBOMIND_AI_ENABLED='1', ROBOMIND_AI_PROVIDER=provider, ROBOMIND_AI_ENDPOINT=f'http://127.0.0.1:{model.server_port}',
                   ROBOMIND_AI_MODEL='fixture-model', ROBOMIND_AI_KEY='isolated-ai-key', ROBOMIND_AI_REASONING_EFFORT='none',
                   ROBOMIND_AI_INTERVAL_SECONDS='0',ROBOMIND_AI_USAGE_FILE=str(folder/'ai-usage.json'))
        executable = pathlib.Path(console).resolve() if console else ROOT/'build/dist/RoboMindConsole.exe'
        proc = subprocess.Popen([str(executable), '--bridge', '--port', str(board_port), '--status-file', str(status), '--stop-file', str(stop)], cwd=folder, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        results = queue.Queue(); failures = []; requests = []; stale = threading.Event()
        chat = board = None
        try:
            chat, _ = lan.accept(); chat.settimeout(10)
            assert json.loads(receive(chat))['type'] == 2
            chat.sendall(packet({'type': 3, 'id': 4242}))
            def state():
                try: return json.loads(status.read_text(encoding='utf-8'))
                except (OSError, ValueError): return {}
            wait(lambda: state().get('lanchat_state') == '已登录')
            def read_lan():
                try:
                    while True:
                        body = json.loads(receive(chat))
                        if body['type'] in (5, 16): results.put(body)
                except (OSError, EOFError): pass
                except Exception as exc: failures.append(str(exc))
            threading.Thread(target=read_lan, daemon=True).start()
            board = socket.create_connection(('127.0.0.1', board_port)); board.settimeout(10)
            board.sendall(envelope('ui/capabilities', 'version=1;text_max=384;navigation=1;query=1'))
            wait(lambda: state().get('display', {}).get('query_supported'))
            def read_board():
                try:
                    while True:
                        parts = receive(board).decode().split('|')
                        values = dict(parse_qsl(parts[10].replace(';', '&')))
                        if parts[8] == 'ui/message': board.sendall(envelope('ui/message_ack', f"id={values['id']};status=rendered"))
                        if parts[8] != 'device/query': continue
                        requests.append(values)
                        data = dict(id=values['id'], kind=values['kind'], version='1', up='20000')
                        if values['kind'] == 'perception': data.update(sample='1', sample_ms='1000' if stale.is_set() else '19950', dv='N', dl='-', ev='N', es='-', mv='N', ms='-', ab='0', dg='0')
                        elif values['kind'] == 'link': data.update(wifi='1', ip='192.0.2.3', rssi='-58', tcp='1', tx='123', rx='8', last_rx_age='0')
                        else: data.update(heap='32000', largest='16000', minimum='24000', psram='6000000', ppm='1', zao='0')
                        body = ';'.join(f'{k}={v}' for k, v in data.items())
                        board.sendall(envelope('device/query_reply', body)); board.sendall(envelope('device/query_reply', body))
                except (OSError, EOFError): pass
                except Exception as exc: failures.append(str(exc))
            threading.Thread(target=read_board, daemon=True).start()
            counter = 100
            def ask(text, sender=1001, group=False, extra=None):
                nonlocal counter
                counter += 1
                msg = {'type': 17 if group else 6, 'fromId': sender, 'msg_id': str(counter), 'msg': text}
                msg.update({'groupId': 77} if group else {'toId': 4242}); msg.update(extra or {})
                chat.sendall(packet(msg)); return msg
            def answer(original, contains='S3 · AI'):
                msg = results.get(timeout=12)
                assert contains in msg['msg'], msg
                assert msg['type'] == (16 if original['type'] == 17 else 5)
                assert msg.get('groupId' if original['type'] == 17 else 'toId') == original.get('groupId', original['fromId'])
                return msg
            def system(body):
                return body['system'] if provider == 'anthropic' else body['messages'][0]['content']
            original = ask('现在板子各项状态怎么样？'); answer(original)
            wait(lambda: len(calls) == 1)
            path, body, auth = calls[-1]
            assert body.get('reasoning_effort') == ('none' if provider == 'openai' else None), body
            assert path == ('/v1/messages' if provider == 'anthropic' else '/v1/chat/completions')
            assert auth == 'Bearer isolated-ai-key'
            assert body['max_tokens']==1200
            assert '未计算' in system(body) and 'ZAO_MINI 未接入' in system(body)
            assert '192.0.2.3' in system(body) and 'PSRAM 可用' in system(body)
            assert len(requests) == 3
            assert 'isolated-password' not in json.dumps(body) and 'isolated-ai-key' not in json.dumps(body)
            chat.sendall(packet(original)); time.sleep(.2); assert results.empty() and len(calls) == 1
            time.sleep(2)
            stale.set(); answer(ask('那现在还能判断疲劳吗？')); assert '过期' in system(calls[-1][1]); assert len(calls[-1][1]['messages']) == (3 if provider == 'anthropic' else 4)
            stale.clear()
            answer(ask('/AI 内存情况如何？', sender=1002, group=True))
            assert len(calls[-1][1]['messages']) == (1 if provider == 'anthropic' else 2)
            before = len(calls)
            ask('普通群聊讨论', sender=1003, group=True); ask('/AI foreign', group=True, extra={'groupId':78}); ask('/AI self', sender=4242); ask('/AI file', extra={'attachment':{}, 'content_type':'file'})
            time.sleep(.2); assert results.empty() and len(calls) == before
            answer(ask('设备', sender=1004), 'S3 · 设备'); assert len(calls) == before
            answer(ask('/AI 清空')); time.sleep(2)
            answer(ask('重新问一下状态')); assert len(calls[-1][1]['messages']) == (1 if provider == 'anthropic' else 2)
            mode[0] = 'error'; error = answer(ask('接口报错测试', sender=1005))
            assert 'HTTP 401' in error['msg'] and 'secret-server-detail' not in error['msg']
            mode[0] = 'slow'; pending = ask('请解释链路', sender=1006)
            wait(lambda: len(calls) == before+3)
            answer(ask('同会话并发问题', sender=1006), '正在回答'); answer(pending)
            mode[0] = 'ok'
            board.close(); board = None; wait(lambda: not state().get('board_connected'))
            answer(ask('板子断开了吗？', sender=1007)); assert '板端离线' in system(calls[-1][1])
            mode[0]='limited';answer(ask('检查输出上限',sender=1008),'达到本次输出上限')
            wait(lambda: state()['ai']['pending'] == 0)
            assert state()['ai']['failed'] == 1 and not failures, (state(), failures)
            log = (folder/'lanchat-sink.log').read_text(encoding='utf-8')
            assert 'isolated-ai-key' not in log and 'isolated-password' not in log
            print(f'PASS {provider}: fresh/expired/offline snapshots, follow-up/reset/isolation, group routing, duplicate suppression, deterministic queries, API error, busy session, secrets omitted')
        finally:
            if board: board.close()
            if chat: chat.close()
            lan.close(); stop.write_text('stop')
            try: proc.wait(timeout=5)
            except subprocess.TimeoutExpired: proc.kill(); proc.wait()
            model.shutdown(); model.server_close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(); parser.add_argument('--provider', choices=['anthropic', 'openai', 'both'], default='both'); parser.add_argument('--console', help='Console executable to test, including an installed copy'); args = parser.parse_args()
    for provider in ('anthropic', 'openai') if args.provider == 'both' else (args.provider,): run(provider, args.console)
