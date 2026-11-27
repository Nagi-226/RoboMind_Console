"""Real LAN server, Console, selected PC TTS and physical S3. Model fixture unless --official."""
import argparse, http.server, importlib.util, json, os, pathlib, socket, subprocess, tempfile, threading, time

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('lan_peer', ROOT.parent/'LanChat-Next/scripts/test-lan-integration.py')
peer_module = importlib.util.module_from_spec(spec); spec.loader.exec_module(peer_module); Peer = peer_module.Peer


def main():
    p = argparse.ArgumentParser(); p.add_argument('--server', required=True); p.add_argument('--output', required=True)
    p.add_argument('--official', action='store_true'); p.add_argument('--board-port', type=int, default=9000)
    p.add_argument('--voice', choices=['auto', 'windows', 'local_vits'], default='auto')
    p.add_argument('--persona', choices=['whale', 'dragon'], default='whale')
    args = p.parse_args(); model = None; report = {'result': 'RUNNING', 'real_provider': args.official, 'checks': []}
    answer = '“板端”已经在线。当前链路连接正常，中文显示和语音正在共同运行。\n这是第二段字幕，用来检查句子自动切换。最后一段回答不会因为窗口或者屏幕大小而丢失。😀'
    class Model(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args): pass
        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            question = body['messages'][-1]['content']
            text = '新问题已经接管当前语音。' if '替换成' in question else answer*3 if '长回答' in question else answer
            data = json.dumps({'choices': [{'message': {'content': text}, 'finish_reason': 'stop'}]}, ensure_ascii=False).encode()
            self.send_response(200); self.send_header('Content-Length', str(len(data))); self.end_headers(); self.wfile.write(data)
    if not args.official:
        model = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Model)
        threading.Thread(target=model.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory(prefix='whale-chat-') as directory:
        folder = pathlib.Path(directory); probe = socket.socket(); probe.bind(('127.0.0.1', 0)); port = probe.getsockname()[1]; probe.close()
        server = subprocess.Popen([str(pathlib.Path(args.server).resolve()), '--port', str(port), '--data', str(folder/'data')], cwd=folder, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        console = None; peers = []; status = folder/'status.json'; stop = folder/'stop.request'; state = {}
        def snapshot():
            try: return json.loads(status.read_text(encoding='utf-8'))
            except (OSError, ValueError): return {}
        def wait(check, seconds=20):
            deadline = time.monotonic()+seconds
            while time.monotonic() < deadline:
                current = snapshot()
                if check(current): return current
                time.sleep(.1)
            raise AssertionError('Timed out: '+json.dumps(snapshot().get('speech', {}), ensure_ascii=False))
        def nav(action):
            identifier = 'nav'+str(time.monotonic_ns())
            (folder/'navigation.request').write_text(json.dumps({'id': identifier, 'action': action, 'created_ms': round(time.time()*1000)}))
            return wait(lambda s: s.get('display', {}).get('view', {}).get('control') == identifier)
        try:
            for _ in range(100):
                try: ready = Peer(port); ready.close(); break
                except OSError: time.sleep(.05)
            for nickname in ('WhaleTester', 'WhaleBoard'):
                peer = Peer(port); peers.append(peer); peer.send(type=0, nickname=nickname, password='isolated-test-password')
                assert peer.wait(1)['status'] == 'ok'; peer.uid = peer.login(nickname)['id']
            user, bridge = peers; bridge_id = bridge.uid; bridge.close(); peers.remove(bridge)
            (folder/'robomind-console.ini').write_text(f'[lanchat]\nenabled=true\nhost=127.0.0.1\nport={port}\nreceive_messages=true\n', encoding='utf-8')
            env = os.environ.copy(); env.update(ROBOMIND_LANCHAT_USER_ID=str(bridge_id), ROBOMIND_LANCHAT_PASSWORD='isolated-test-password', ROBOMIND_AI_ENABLED='1', ROBOMIND_AI_USAGE_FILE=str(folder/'ai-usage.json'))
            env['ROBOMIND_TTS_PROVIDER'] = args.voice
            if model:
                env.update(ROBOMIND_AI_PROVIDER='openai', ROBOMIND_AI_MODEL='fixture-model', ROBOMIND_AI_ENDPOINT=f'http://127.0.0.1:{model.server_port}', ROBOMIND_AI_KEY='fixture-key', ROBOMIND_AI_REASONING_EFFORT='none')
            else:
                for key in ('ROBOMIND_AI_KEY', 'ROBOMIND_AI_ENDPOINT', 'ROBOMIND_AI_MODEL', 'ROBOMIND_AI_PROVIDER'):
                    assert env.get(key), 'Missing provider environment: '+key
            console = subprocess.Popen([str(ROOT/'build/dist/RoboMindConsole.exe'), '--bridge', '--port', str(args.board_port), '--status-file', str(status), '--stop-file', str(stop)], cwd=folder, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            state = wait(lambda s: s.get('display', {}).get('pcm_supported') and s.get('lanchat_state') == '已登录', 60)
            assert state['ai']['configured']
            character_action = 'immersion_dragon' if args.persona == 'dragon' else 'immersion'
            if args.persona == 'dragon': assert state['display']['dragon_supported']
            view = nav(character_action)['display']['view']
            assert view['immersion'] and view.get('persona', 'whale') == args.persona
            nav('volume_20')
            def ask(question):
                user.send(type=5, fromId=user.uid, toId=bridge_id, msg=question)
                user.socket.settimeout(65)
                message = user.wait(6, fromId=bridge_id, toId=user.uid)
                assert '[S3 · AI · #' in message['msg'], message
                report.setdefault('replies', []).append({'question': question, 'answer': message['msg']})
                return message
            question = '请用不超过100字概括板子的Wi-Fi、感知和内存状态，说明哪些结果目前无法判断。' if args.official else '检查中文字幕和完整语音回答。'
            reply = ask(question)
            if not args.official: assert answer in reply['msg'], 'PC lost answer characters'
            state = wait(lambda s: s.get('speech', {}).get('stage') == 'complete', 100)
            assert state['speech']['completed_segments'] >= (1 if args.official else 2) and state['speech']['pcm_bytes'] > (0 if args.official else 32000), state['speech']
            assert state['speech']['last_error'] == ''; report['completed_answer'] = state['speech']
            if args.voice != 'auto': assert state['speech']['provider'] == args.voice
            if args.persona == 'dragon' and args.voice == 'local_vits':
                assert state['speech']['dragon_voice_available'] and '优菈' in state['speech']['voice']
            report['checks'].append('PC complete single answer; '+state['speech']['voice']+'; segmented subtitles; physical PCM completion')
            if not args.official:
                time.sleep(2.1); ask('给一个长回答用于停止测试。')
                wait(lambda s: s.get('speech', {}).get('stage') == 'streaming')
                nav('stop_speech'); wait(lambda s: s.get('speech', {}).get('stage') == 'idle')
                report['checks'].append('stop_speech cancels synthesis and physical playback')
                time.sleep(2.1); ask('再给一个长回答用于替换。')
                # Use a long first answer rather than the short replacement fixture.
                wait(lambda s: s.get('speech', {}).get('stage') in ('streaming', 'complete'))
                time.sleep(2.1); ask('替换成最新问题。')
                state = wait(lambda s: s.get('speech', {}).get('stage') == 'complete', 50)
                assert state['speech']['completed_segments'] == 1
                report['checks'].append('latest question owns board speech; PC retains each original reply')
                time.sleep(2.1); ask('长回答用于退出模式。')
                wait(lambda s: s.get('speech', {}).get('stage') == 'streaming')
                if args.persona == 'dragon':
                    view = nav('immersion')['display']['view']
                    assert view['immersion'] and view['persona'] == 'whale'
                    state = wait(lambda s: s.get('speech', {}).get('stage') == 'idle')
                    if args.voice == 'local_vits': assert '派蒙' in state['speech']['voice']
                    assert nav(character_action)['display']['view']['persona'] == 'dragon'
                    report['checks'].append('switching persona cancels active speech and selects the new voice')
                assert not nav('terminal')['display']['view']['immersion']
                wait(lambda s: s.get('speech', {}).get('stage') == 'idle')
                report['checks'].append('return to five pages' if args.persona == 'dragon' else 'return to five pages cancels speech')
            user.send(type=24, toId=bridge_id, limit=50)
            history = user.wait(25)['messages']; assert sum(item['fromId'] == bridge_id for item in history) == len(report['replies'])
            state = snapshot(); assert state['ai']['failed'] == 0 and state['ai']['pending'] == 0
            report.update(result='PASS', ai=state['ai'], display=state['display'], frames=state['frames'])
            print('PASS whale chat:', '; '.join(report['checks']), flush=True)
        except Exception as error:
            report.update(result='FAIL', error=str(error), speech=snapshot().get('speech', {}), ai=snapshot().get('ai', {}))
            raise
        finally:
            pathlib.Path(args.output).write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
            for peer in peers: peer.close()
            if console:
                stop.write_text('stop')
                try: console.wait(timeout=5)
                except subprocess.TimeoutExpired: console.kill(); console.wait()
            server.terminate(); server.wait(timeout=10)
            if model: model.shutdown(); model.server_close()


if __name__ == '__main__': main()
