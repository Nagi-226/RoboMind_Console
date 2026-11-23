"""One explicitly authorized provider call through real LAN/Console, with S3 offline."""
import argparse
import importlib.util
import json
import os
import pathlib
import socket
import subprocess
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('lan_peer', ROOT.parent/'LanChat-Next/scripts/test-lan-integration.py')
peer_module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(peer_module)
Peer = peer_module.Peer


def free_port():
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--server', required=True)
    parser.add_argument('--console', default=str(ROOT/'build/dist/RoboMindConsole.exe'))
    parser.add_argument('--output', required=True)
    parser.add_argument('--allow-paid', action='store_true')
    args = parser.parse_args()
    if not args.allow_paid:
        parser.error('This makes one real provider call; pass --allow-paid only with user authorization.')
    for field in ('ROBOMIND_AI_KEY', 'ROBOMIND_AI_ENDPOINT', 'ROBOMIND_AI_MODEL', 'ROBOMIND_AI_PROVIDER'):
        if not os.environ.get(field):
            parser.error('Missing provider environment configuration: '+field)
    with tempfile.TemporaryDirectory(prefix='provider-lan-smoke-') as directory:
        folder = pathlib.Path(directory)
        port, board_port = free_port(), free_port()
        server = subprocess.Popen([str(pathlib.Path(args.server).resolve()), '--port', str(port), '--data', str(folder/'data')],
                                  cwd=folder, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        console = None
        peers = []
        status, stop = folder/'status.json', folder/'stop.request'
        try:
            for _ in range(100):
                try:
                    probe = Peer(port); probe.close(); break
                except OSError:
                    time.sleep(.05)
            for nickname in ('ProviderSmokeUser', 'ProviderSmokeBridge'):
                peer = Peer(port); peers.append(peer)
                peer.send(type=0, nickname=nickname, password='isolated-test-password')
                assert peer.wait(1)['status'] == 'ok'
                peer.uid = peer.login(nickname)['id']
            user, bridge = peers
            bridge_id = bridge.uid
            bridge.close(); peers.remove(bridge)
            (folder/'robomind-console.ini').write_text(
                f'[lanchat]\nenabled=true\nhost=127.0.0.1\nport={port}\nreceive_messages=true\n', encoding='utf-8')
            env = os.environ.copy()
            env.update(ROBOMIND_LANCHAT_USER_ID=str(bridge_id), ROBOMIND_LANCHAT_PASSWORD='isolated-test-password', ROBOMIND_AI_ENABLED='1')
            console = subprocess.Popen([str(pathlib.Path(args.console).resolve()), '--bridge', '--port', str(board_port),
                                        '--status-file', str(status), '--stop-file', str(stop)], cwd=folder, env=env,
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            deadline = time.monotonic()+10
            state = {}
            while time.monotonic() < deadline:
                try: state = json.loads(status.read_text(encoding='utf-8'))
                except (OSError, ValueError): pass
                if state.get('lanchat_state') == '已登录': break
                time.sleep(.05)
            assert state.get('lanchat_state') == '已登录' and not state['board_connected']
            question = '先用一句话解释什么是局域网，再根据本轮实际连接状态说明板子是否在线。控制在100字内。'
            started = time.monotonic()
            user.send(type=5, fromId=user.uid, toId=bridge_id, msg=question)
            reply = None
            deadline = time.monotonic()+55
            user.socket.settimeout(1)
            while time.monotonic() < deadline:
                try:
                    reply = user.wait(6, fromId=bridge_id, toId=user.uid)
                    break
                except socket.timeout:
                    pass
            assert reply and '[S3 · AI · #' in reply['msg'], reply
            assert '局域网' in reply['msg'] and any(word in reply['msg'] for word in ('离线', '未连接', '不在线')), reply
            user.send(type=24, toId=bridge_id, limit=50)
            history = user.wait(25)['messages']
            assert sum(item['fromId'] == bridge_id for item in history) == 1
            for _ in range(30):
                state = json.loads(status.read_text(encoding='utf-8'))
                if state['ai']['replied'] == 1 and state['ai']['pending'] == 0: break
                time.sleep(.05)
            assert state['ai']['requested'] == 1 and state['ai']['replied'] == 1 and state['ai']['failed'] == 0
            result = {'result': 'PASS', 'real_provider': True, 'real_lan_server': True, 'physical_s3': False,
                      'board_connected': False, 'model': state['ai']['model'], 'question': question, 'answer': reply['msg'],
                      'elapsed_ms': round((time.monotonic()-started)*1000), 'ai': state['ai'], 'single_persisted_reply': True}
            output = pathlib.Path(args.output); output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
            print('PASS real provider + real LAN/Console: general knowledge, explicit offline state, single persisted reply')
            print(reply['msg'])
        finally:
            for peer in peers: peer.close()
            if console:
                stop.write_text('stop')
                try: console.wait(timeout=5)
                except subprocess.TimeoutExpired: console.kill(); console.wait()
            server.terminate(); server.wait(timeout=10)


if __name__ == '__main__': main()
