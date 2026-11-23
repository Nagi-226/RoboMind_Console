"""Authorized model calls through a real LAN server and physical S3 snapshots."""
import argparse
import importlib.util
import json
import os
import pathlib
import socket
import subprocess
import struct
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('lan_peer', ROOT.parent/'LanChat-Next/scripts/test-lan-integration.py')
module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module); Peer = module.Peer


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    parser = argparse.ArgumentParser(); parser.add_argument('--server'); parser.add_argument('--output'); parser.add_argument('--verify-result'); parser.add_argument('--board-port', type=int, default=9000); args = parser.parse_args()
    if args.verify_result:
        result=json.loads(pathlib.Path(args.verify_result).read_text(encoding='utf-8'))
        assert result['result']=='PASS' and result['ai']['requested']==3 and result['ai']['replied']==3 and result['ai']['failed']==0 and result['ai']['pending']==0
        assert len(result['replies'])==3 and result['queries']['invalid']==0
        print('PASS saved physical S3/model results and UTF-8 output')
        for item in result['replies']:print(item['answer'])
        return
    if not args.server or not args.output:parser.error('--server and --output are required for a live run')
    with tempfile.TemporaryDirectory(prefix='real-ai-s3-') as directory:
        folder = pathlib.Path(directory); probe = socket.socket(); probe.bind(('127.0.0.1', 0)); port = probe.getsockname()[1]; probe.close()
        server = subprocess.Popen([args.server, '--port', str(port), '--data', str(folder/'data')], cwd=folder, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        peers = []; console = None; status = folder/'status.json'; stop = folder/'stop.request'
        try:
            for _ in range(100):
                try: p = Peer(port); p.close(); break
                except OSError: time.sleep(.05)
            for nick in ('AiQueryUser', 'AiBoardBridge', 'AiGroupUser'):
                p = Peer(port); peers.append(p); p.send(type=0, nickname=nick, password='isolated-test-password'); assert p.wait(1)['status'] == 'ok'; p.uid = p.login(nick)['id']
            user, bridge, group_user = peers
            user.send(type=9, name='AI实板隔离验收'); gid = user.wait(10)['groupId']
            group_user.send(type=13, groupId=gid); assert group_user.wait(14)['status'] == 'ok'
            uid = bridge.uid; bridge.close(); peers.remove(bridge)
            (folder/'robomind-console.ini').write_text(f'[lanchat]\nenabled=true\nhost=127.0.0.1\nport={port}\ngroup_id={gid}\nreceive_messages=true\n')
            env = os.environ.copy(); env.update(ROBOMIND_LANCHAT_USER_ID=str(uid), ROBOMIND_LANCHAT_PASSWORD='isolated-test-password', ROBOMIND_AI_ENABLED='1')
            console = subprocess.Popen([str(ROOT/'build/dist/RoboMindConsole.exe'), '--bridge', '--port', str(args.board_port), '--status-file', str(status), '--stop-file', str(stop)], cwd=folder, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            deadline = time.monotonic()+60
            while time.monotonic() < deadline:
                try: state = json.loads(status.read_text(encoding='utf-8'))
                except (OSError, ValueError): state = {}
                if state.get('display', {}).get('query_supported') and state.get('lanchat_state') == '已登录': break
                time.sleep(.1)
            else: raise AssertionError('physical S3 did not connect')
            assert state['ai']['configured'], 'model configuration missing'
            # Peer.wait defaults to a short LAN deadline; model calls need a bounded larger wait.
            replies = []
            def response(peer, group=False):
                deadline = time.monotonic()+60
                while time.monotonic() < deadline:
                    peer.socket.settimeout(max(.1,deadline-time.monotonic()))
                    size=struct.unpack('>I',peer.exact(4))[0];assert 0<size<=256*1024
                    message=json.loads(peer.exact(size))
                    if message and message.get('type') == (17 if group else 6) and message.get('fromId') == uid:
                        assert '[S3 · AI ·' in message['msg'] and '调用失败' not in message['msg'] and '连接失败' not in message['msg'], message['msg']
                        return message
                raise AssertionError('model answer timed out')
            questions = ['请概括当前感知、链路和设备状态，尤其说明疲劳结果是否有效、ZAO_MINI是否接入，不要猜测摄像头画面。', '当前可用内存多少？能据此断言内存泄漏吗？']
            for question in questions:
                user.send(type=5, fromId=user.uid, toId=uid, msg=question)
                message = response(user); replies.append({'question': question, 'answer': message['msg']}); time.sleep(2.1)
            user.send(type=16, fromId=user.uid, groupId=gid, msg='/AI 当前链路有问题吗？列出实际数据和不能确定的部分。')
            group_reply = response(user, True); other = response(group_user, True); assert group_reply['msg_id'] == other['msg_id']
            replies.append({'question': '/AI 当前链路有问题吗？', 'answer': group_reply['msg']})
            deadline=time.monotonic()+5
            while time.monotonic()<deadline:
                state=json.loads(status.read_text(encoding='utf-8'))
                if state['ai']['replied']==3 and state['ai']['pending']==0:break
                time.sleep(.1)
            assert state['ai']['requested'] == 3 and state['ai']['replied'] == 3 and state['ai']['failed'] == 0 and state['ai']['pending'] == 0, state['ai']
            user.send(type=24, toId=uid, limit=50); history = user.wait(25)['messages']; assert sum(m['fromId'] == uid for m in history) == 2
            output = pathlib.Path(args.output); output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(json.dumps({'result': 'PASS', 'model': state['ai']['model'], 'replies': replies, 'ai': state['ai'], 'queries': state['queries'], 'frames': state['frames']}, ensure_ascii=False, indent=2), encoding='utf-8')
            print('PASS real model + real LAN server + physical S3: 3 fresh status questions, follow-up, group reply, single persisted replies')
            for item in replies: print(item['answer'])
        finally:
            for peer in peers: peer.close()
            if console:
                stop.write_text('stop')
                try: console.wait(timeout=5)
                except subprocess.TimeoutExpired: console.kill(); console.wait()
            server.terminate(); server.wait(timeout=10)


if __name__ == '__main__': main()
