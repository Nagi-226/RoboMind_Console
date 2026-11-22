"""Real LanChat server + Console + physical S3; isolated account database."""
import argparse, importlib.util, json, os, pathlib, socket, subprocess, tempfile, time, threading, re

ROOT=pathlib.Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('lan_peer',ROOT.parent/'LanChat-Next/scripts/test-lan-integration.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);Peer=module.Peer

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--server',required=True);parser.add_argument('--board-port',type=int,default=9000);parser.add_argument('--output',required=True);parser.add_argument('--serial-port');parser.add_argument('--serial-log');args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='real-s3-queries-') as directory:
        folder=pathlib.Path(directory);probe=socket.socket();probe.bind(('127.0.0.1',0));port=probe.getsockname()[1];probe.close()
        server=subprocess.Popen([args.server,'--port',str(port),'--data',str(folder/'data')],cwd=folder,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        peers=[];console=None;replies=[];uart=None;reader=None;chunks=[];end=threading.Event()
        try:
            for _ in range(100):
                try:p=Peer(port);p.close();break
                except OSError:time.sleep(.05)
            for nick in ('QueryUser','BoardBridge','GroupUser'):
                p=Peer(port);peers.append(p);p.send(type=0,nickname=nick,password='isolated-test-password');assert p.wait(1)['status']=='ok';p.uid=p.login(nick)['id']
            user,bridge,group_user=peers
            user.send(type=9,name='实板查询隔离验收');gid=user.wait(10)['groupId']
            group_user.send(type=13,groupId=gid);assert group_user.wait(14)['status']=='ok'
            uid=bridge.uid;bridge.close();peers.remove(bridge)
            status=folder/'status.json';stop=folder/'stop.request'
            (folder/'robomind-console.ini').write_text(f'[lanchat]\nenabled=true\nhost=127.0.0.1\nport={port}\ngroup_id={gid}\nreceive_messages=true\n')
            env=os.environ.copy();env.update(ROBOMIND_LANCHAT_USER_ID=str(uid),ROBOMIND_LANCHAT_PASSWORD='isolated-test-password')
            console=subprocess.Popen([str(ROOT/'build/dist/RoboMindConsole.exe'),'--bridge','--port',str(args.board_port),'--status-file',str(status),'--stop-file',str(stop)],cwd=folder,env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            deadline=time.monotonic()+60
            while time.monotonic()<deadline:
                try:state=json.loads(status.read_text(encoding='utf-8'))
                except (OSError,ValueError):state={}
                if state.get('display',{}).get('query_supported') and state.get('lanchat_state')=='已登录':break
                time.sleep(.1)
            else:raise AssertionError('Physical board did not connect with query capability')
            if args.serial_port:
                import serial
                uart=serial.Serial();uart.port=args.serial_port;uart.baudrate=115200;uart.timeout=.2;uart.dtr=False;uart.rts=False;uart.open()
                def capture():
                    while not end.is_set():chunks.append(uart.read(8192))
                reader=threading.Thread(target=capture);reader.start()
            for text in ('帮助','感知','链路','设备'):
                user.send(type=5,fromId=user.uid,toId=uid,msg=text)
                response=user.wait(6,fromId=uid,toId=user.uid);assert '[S3 · ' in response['msg'] and '超时' not in response['msg'],response
                assert '离线' not in response['msg'],response
                if text=='设备':assert 'PSRAM 可用' in response['msg']
                if text=='链路':assert 'Wi-Fi：已连接' in response['msg']
                replies.append(response['msg']);time.sleep(.8)
            user.send(type=16,fromId=user.uid,groupId=gid,msg='/感知')
            response=user.wait(17,fromId=uid,groupId=gid)
            other=group_user.wait(17,fromId=uid,groupId=gid);assert response['msg_id']==other['msg_id'] and '感知' in response['msg']
            replies.append(response['msg'])
            user.send(type=24,toId=uid,limit=50);history=user.wait(25)['messages']
            assert sum(m['fromId']==uid for m in history)==4,'Missing or duplicate private reply'
            deadline=time.monotonic()+3
            while time.monotonic()<deadline:
                state=json.loads(status.read_text(encoding='utf-8'))
                if state['queries']['replied']==5 and state['queries']['pending']==0:break
                time.sleep(.05)
            assert state['queries']['replied']==5 and state['queries']['invalid']==0,state['queries']
            margin=None
            if uart:
                time.sleep(.4);end.set();reader.join(timeout=2);uart.close();uart=None
                log=b''.join(chunks).decode('utf-8','replace')
                if args.serial_log:pathlib.Path(args.serial_log).write_text(log,encoding='utf-8')
                values=[int(value) for value in re.findall(r'terminal_query:.*stack_free=(\d+)',log)]
                assert values and min(values)>=1024,values
                assert not re.search(r'Guru Meditation|abort\(\)|Backtrace:',log)
                margin=min(values);print('PASS physical query TCP stack margin:',margin,'bytes')
            pathlib.Path(args.output).write_text(json.dumps({'result':'PASS','replies':replies,'query_stats':state['queries'],'frames':state['frames'],'board_peer':state['board_peer'],'tcp_stack_free':margin},ensure_ascii=False,indent=2),encoding='utf-8')
            print('PASS real LanChat server → Console → physical S3 → original private/group conversation: four queries, fresh snapshots, persisted single replies')
        finally:
            if uart:
                end.set()
                if reader:reader.join(timeout=2)
                uart.close()
            for peer in peers:peer.close()
            if console:
                stop.write_text('stop')
                try:console.wait(timeout=5)
                except subprocess.TimeoutExpired:console.kill();console.wait()
            server.terminate();server.wait(timeout=10)

if __name__=='__main__':main()
