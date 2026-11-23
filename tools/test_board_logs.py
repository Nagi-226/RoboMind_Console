"""Real LanChat server and Console; temporary accounts; optional actual USB capture."""
import argparse, importlib.util, json, os, pathlib, socket, subprocess, tempfile, time

ROOT=pathlib.Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('lan_peers',ROOT.parent/'LanChat-Next/scripts/test-lan-integration.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);Peer=module.Peer
def wait(check,seconds=12):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        if check():return
        time.sleep(.05)
    raise AssertionError('condition timed out')
def free_port():
    with socket.socket() as s:s.bind(('127.0.0.1',0));return s.getsockname()[1]
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--server',required=True);parser.add_argument('--console',default=str(ROOT/'build/dist/RoboMindConsole.exe'));parser.add_argument('--port');parser.add_argument('--result');parser.add_argument('--qt-probe');parser.add_argument('--capture');args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='real-board-logs-') as temp:
        folder=pathlib.Path(temp);lan_port=free_port();server=subprocess.Popen([args.server,'--port',str(lan_port),'--data',str(folder/'data')],cwd=folder,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL);console=None;peers=[]
        try:
            def ready():
                try:p=Peer(lan_port);p.close();return True
                except OSError:return False
            wait(ready)
            for nickname in ('LogOperator','BoardLogs','Outside'):
                p=Peer(lan_port);peers.append(p);p.send(type=0,nickname=nickname,password='isolated-test-password');assert p.wait(1)['status']=='ok';p.uid=p.login(nickname)['id']
            operator,board,outside=peers;operator.send(type=9,name='Board diagnostics');gid=operator.wait(10)['groupId'];uid=board.uid;board.close()
            (folder/'robomind-console.ini').write_text(f'[lanchat]\nenabled=true\nhost=127.0.0.1\nport={lan_port}\ngroup_id={gid}\nreceive_messages=true\n')
            logs_dir=folder/'logs';logs_dir.mkdir(exist_ok=True)
            if not args.port:
                rows=[dict(seq=1,received_ms=int(time.time()*1000),level='E',module='camera',text='中文历史错误',wire=False,port='COM_TEST')]
                (logs_dir/'logs.snapshot.json').write_text(json.dumps(dict(rows=rows),ensure_ascii=False),encoding='utf-8')
            else:
                # 用已有满文件验证轮换边界；随后写入的行全部来自真实 USB。
                (logs_dir/'board-log-0.jsonl').write_bytes(b'x'*(5*1024*1024))
                for n in range(1,4):(logs_dir/f'board-log-{n}.jsonl').write_text('rotation-marker-'+str(n))
            env=os.environ.copy();env.update(ROBOMIND_LANCHAT_USER_ID=str(uid),ROBOMIND_LANCHAT_PASSWORD='isolated-test-password')
            cmd=[args.console,'--bridge','--port',str(free_port()),'--status-file',str(folder/'status.json'),'--stop-file',str(folder/'stop.request')]
            if args.port:cmd+=['--log-port',args.port]
            console=subprocess.Popen(cmd,cwd=folder,env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            def snapshot():
                try:return json.loads((folder/'status.json').read_text(encoding='utf-8'))
                except (OSError,ValueError):return {}
            wait(lambda:snapshot().get('lanchat_state')=='已登录')
            wait(lambda:snapshot().get('logs',{}).get('count',0)>=(3 if args.port else 1),25)
            if args.port:
                assert snapshot()['logs']['state']=='running'
                assert (logs_dir/'board-log-1.jsonl').stat().st_size==5*1024*1024
                assert (logs_dir/'board-log-3.jsonl').read_text()=='rotation-marker-2'
                assert sum(p.stat().st_size for p in logs_dir.glob('board-log-*.jsonl'))<=20*1024*1024
            def query(text,group=False):
                time.sleep(.8)  # 正常验收尊重桥端每账号 750 ms 的查询间隔。
                operator.send(type=16 if group else 5,msg=text,**({'groupId':gid} if group else {'toId':uid}))
                echo=operator.wait(17 if group else 6,msg=text)
                answer=operator.wait(17 if group else 6,fromId=uid)
                assert f"#{echo['msg_id']}]" in answer['msg'] and len(answer['msg'].encode())<=6144,answer
                return answer['msg']
            reply=query('/日志');assert '来源：板端 USB' in reply
            query('/日志 错误');query('/日志 链路',True);assert '/日志 帮助' in query('/日志 帮助')
            if args.port:
                def control(action):
                    file=folder/'logs.request';tmp=folder/'logs.tmp';tmp.write_text(json.dumps(dict(action=action,port=args.port,created_ms=int(time.time()*1000))));tmp.replace(file)
                control('stop');wait(lambda:snapshot().get('logs',{}).get('state')=='stopped');assert '历史缓存' in query('/日志')
                import serial
                blocker=serial.Serial(port=None,baudrate=115200,timeout=.1);blocker.dtr=False;blocker.rts=False;blocker.port=args.port;blocker.open()
                try:control('start');wait(lambda:snapshot().get('logs',{}).get('state')=='busy')
                finally:blocker.close()
                wait(lambda:snapshot().get('logs',{}).get('state')=='running')
                assert '采集中' in query('/日志')
            result={'real_usb':bool(args.port),'real_server':True,'rows':snapshot()['logs']['count'],'sequence':snapshot()['logs']['sequence'],'private_group_commands':True,'reply_id_and_limit':True,'stop_busy_recovery_rotation':bool(args.port)}
            if args.qt_probe and args.port:
                (folder/'stop.request').write_text('stop');console.wait(timeout=8);operator.close()
                (folder/'qt-profile').mkdir()
                command=[args.qt_probe,'--live','--profile',str(folder/'qt-profile'),'--console',args.console,'--server-port',str(lan_port),'--board-id',str(uid),'--port',args.port]
                if args.capture:command+=['--capture',args.capture]
                qt_env=os.environ.copy();qt_env['QT_QPA_PLATFORM']='offscreen'
                subprocess.run(command,env=qt_env,check=True,timeout=45);result['live_qt_panel']=True
            if args.result:pathlib.Path(args.result).write_text(json.dumps(result,indent=2),encoding='utf-8')
            print('PASS real server + Console log commands:',json.dumps(result))
        finally:
            if (folder/'qt-profile').exists():
                (folder/'qt-profile/stop.request').write_text('stop');time.sleep(.5)
            for p in peers:
                try:p.close()
                except OSError:pass
            if console:
                (folder/'stop.request').write_text('stop')
                try:console.wait(timeout=8)
                except subprocess.TimeoutExpired:console.kill();console.wait()
            server.terminate();server.wait(timeout=10)
if __name__=='__main__':main()
