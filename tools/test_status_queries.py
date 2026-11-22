"""Drive real Console with independent LAN/board peers; never use account DBs."""
import json, os, pathlib, queue, socket, subprocess, tempfile, threading, time
from urllib.parse import parse_qsl
from test_display_bridge import packet, envelope, receive, wait

ROOT = pathlib.Path(__file__).resolve().parents[1]

def main():
    with tempfile.TemporaryDirectory(prefix='s3-query-') as directory:
        folder = pathlib.Path(directory)
        status, stop = folder/'status.json', folder/'stop.request'
        lan = socket.socket(); lan.bind(('127.0.0.1', 0)); lan.listen(); lan.settimeout(8)
        probe = socket.socket(); probe.bind(('127.0.0.1', 0)); board_port=probe.getsockname()[1]; probe.close()
        (folder/'robomind-console.ini').write_text(f'[lanchat]\nenabled=true\nhost=127.0.0.1\nport={lan.getsockname()[1]}\ngroup_id=77\nreceive_messages=true\n')
        env=os.environ.copy(); env.update(ROBOMIND_LANCHAT_USER_ID='4242',ROBOMIND_LANCHAT_PASSWORD='isolated-test')
        process=subprocess.Popen([str(ROOT/'build/dist/RoboMindConsole.exe'),'--bridge','--port',str(board_port),'--status-file',str(status),'--stop-file',str(stop)],cwd=folder,env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        chat=board=None; results=queue.Queue(); requests=[]; failures=[]
        ignore=threading.Event(); invalid=threading.Event(); stale=threading.Event()
        try:
            chat,_=lan.accept(); chat.settimeout(8)
            assert json.loads(receive(chat))['type']==2
            chat.sendall(packet({'type':3,'id':4242}))
            def snapshot():
                try:return json.loads(status.read_text(encoding='utf-8'))
                except (OSError,ValueError):return {}
            wait(lambda:snapshot().get('lanchat_state')=='已登录')
            def read_lan():
                try:
                    while True:
                        body=json.loads(receive(chat))
                        if body['type'] in (5,16):results.put(body)
                except (OSError,EOFError):pass
                except Exception as exc:failures.append(exc)
            threading.Thread(target=read_lan,daemon=True).start()
            number=100
            def ask(text,group=False,extra=None,id=None):
                nonlocal number
                number+=1
                body={'type':17 if group else 6,'msg_id':str(id or number),'fromId':10000+number,'msg':text}
                body.update({'groupId':77} if group else {'toId':4242})
                body.update(extra or {}); chat.sendall(packet(body)); return body
            def reply(original,contains):
                body=results.get(timeout=8)
                assert body['type']==(16 if original['type']==17 else 5),body
                assert body.get('groupId' if body['type']==16 else 'toId')==original.get('groupId',original['fromId']),body
                assert contains in body['msg'],body
                assert str(original['msg_id']) in body['msg'],body
                return body
            reply(ask('帮助'),'感知')
            reply(ask('链路'),'离线')
            reply(ask('感知'),'离线')
            board=socket.create_connection(('127.0.0.1',board_port));board.settimeout(8)
            board.sendall(envelope('ui/capabilities','version=1;text_max=384;navigation=1;query=1'))
            wait(lambda:snapshot().get('display',{}).get('query_supported'))
            def read_board():
                try:
                    while True:
                        parts=receive(board).decode().split('|')
                        values=dict(parse_qsl(parts[10].replace(';','&')))
                        if parts[8]=='ui/message':board.sendall(envelope('ui/message_ack',f"id={values['id']};status=rendered"))
                        if parts[8]!='device/query':continue
                        requests.append(values)
                        if ignore.is_set():continue
                        data={'id':values['id'],'kind':values['kind'],'version':'1','up':'20000'}
                        if values['kind']=='perception':
                            data.update(sample='1',sample_ms='1000' if stale.is_set() else '19950',dv='I' if invalid.is_set() else 'N',dl='-',ev='N',es='-',mv='N',ms='-',ab='0',dg='0')
                        elif values['kind']=='link':data.update(wifi='1',ip='192.0.2.3',rssi='-58',tcp='1',tx='123',rx='8',last_rx_age='0')
                        else:data.update(heap='32000',largest='16000',minimum='24000',psram='6000000',ppm='1',zao='0')
                        body=';'.join(f'{k}={v}' for k,v in data.items())
                        board.sendall(envelope('device/query_reply',body))
                        board.sendall(envelope('device/query_reply',body)) # duplicate response
                except (OSError,EOFError):pass
                except Exception as exc:failures.append(exc)
            threading.Thread(target=read_board,daemon=True).start()
            reply(ask('感知'),'未计算')
            invalid.set();reply(ask('感知状态'),'无效');invalid.clear()
            stale.set();reply(ask('/感知',group=True),'过期');stale.clear()
            reply(ask('LINK'),'Wi-Fi')
            reply(ask('设备'),'PSRAM')
            original=ask('/帮助',group=True);reply(original,'帮助')
            before=len(requests)
            ask('/帮助',group=True,id=original['msg_id'],extra={'fromId':original['fromId']})
            ask('讨论一下感知算法');ask('感知',group=True)
            ask('帮助',extra={'fromId':4242});ask('帮助',extra={'content_type':'file','attachment':{}})
            time.sleep(.4)
            assert results.empty() and len(requests)==before,'duplicate, normal text, self or attachment triggered'
            ignore.set();reply(ask('设备'),'超时');ignore.clear()
            first=ask('帮助');reply(first,'帮助')
            reply(ask('帮助',extra={'fromId':first['fromId']}),'频繁')
            ignore.set()
            held=[ask('设备') for _ in range(4)]
            wait(lambda:snapshot().get('queries',{}).get('pending')==4)
            reply(ask('链路'),'繁忙')
            timed=[results.get(timeout=8) for _ in held]
            assert all('超时' in value['msg'] for value in timed)
            assert all(any('#'+request['msg_id']+']' in value['msg'] for value in timed) for request in held)
            ignore.clear()
            assert not failures,failures
            print('PASS queries: offline/help, private/group routing, Chinese/aliases, validity/staleness, actual snapshot, timeout, duplicate responses/messages, ordinary/self/attachment filtering')
        finally:
            if board:board.close()
            if chat:chat.close()
            lan.close();stop.write_text('stop')
            try:assert process.wait(timeout=5)==0
            except subprocess.TimeoutExpired:process.kill();raise

if __name__=='__main__':main()
