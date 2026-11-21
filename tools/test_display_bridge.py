"""Independent fake LanChat + fake board exercise the real background Console."""
import json, os, pathlib, socket, struct, subprocess, tempfile, threading, time
from urllib.parse import parse_qsl

ROOT = pathlib.Path(__file__).resolve().parents[1]
def packet(body):
    data = body.encode() if isinstance(body, str) else json.dumps(body,ensure_ascii=False).encode()
    return struct.pack('>I',len(data))+data
def envelope(topic,payload):
    return packet(f'1|event|at_least_once|1|100|30000|robomind-s3|console|{topic}|test|{payload}')
def exact(sock,n):
    data=b''
    while len(data)<n:
        chunk=sock.recv(n-len(data))
        if not chunk: raise EOFError
        data+=chunk
    return data
def receive(sock):
    size=struct.unpack('>I',exact(sock,4))[0]
    assert 0<size<65536
    return exact(sock,size)
def wait(check):
    deadline=time.monotonic()+8
    while time.monotonic()<deadline:
        result=check()
        if result:return result
        time.sleep(.05)
    raise AssertionError('timed out')
def main():
    work=ROOT/'build/terminal-link-test';work.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(dir=work) as folder:
        folder=pathlib.Path(folder);status=folder/'status.json';stop=folder/'stop.request'
        server=socket.socket();server.bind(('127.0.0.1',0));server.listen();server.settimeout(8)
        board_port=19001
        (folder/'robomind-console.ini').write_text(f'[lanchat]\nenabled=true\nhost=127.0.0.1\nport={server.getsockname()[1]}\ntarget=0\ngroup_id=0\nreceive_messages=true\n')
        env=os.environ.copy();env.update(ROBOMIND_LANCHAT_USER_ID='4242',ROBOMIND_LANCHAT_PASSWORD='test-password')
        process=subprocess.Popen([str(ROOT/'build/dist/RoboMindConsole.exe'),'--bridge','--port',str(board_port),'--status-file',str(status),'--stop-file',str(stop)],cwd=folder,env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        board=None;chat=None
        try:
            chat,_=server.accept();chat.settimeout(8)
            login=json.loads(receive(chat));assert login['type']==2 and login['id']==4242
            chat.sendall(packet({'type':3,'id':4242}))
            def snapshot():
                try:return json.loads(status.read_text(encoding='utf-8'))
                except (OSError,json.JSONDecodeError):return {}
            wait(lambda:snapshot().get('listening'))
            board=socket.create_connection(('127.0.0.1',board_port));board.settimeout(8)
            board.sendall(envelope('ui/capabilities','version=1;text_max=384;navigation=1'))
            wait(lambda:snapshot().get('display',{}).get('terminal_ready'))
            (folder/'navigation.request').write_text(json.dumps({'id':'nav-test','action':'device','created_ms':int(time.time()*1000)}))
            while True:
                parts=receive(board).decode().split('|')
                if parts[8]=='ui/navigation':
                    assert parts[10]=='id=nav-test;action=device'
                    break
            board.sendall(envelope('ui/view_state','page=4;detail=0;count=0;unread=0;language=zh;control=nav-test'))
            wait(lambda:snapshot().get('display',{}).get('view',{}).get('control')=='nav-test')
            text='设备|连接;状态\\\n收到中文消息，请检查板端。'
            def send_message(id,text,to=4242):chat.sendall(packet({'type':6,'msg_id':str(id),'fromId':1000,'toId':to,'msg':text,'timestamp':int(time.time()*1000)}))
            def message(id):
                while True:
                    parts=receive(board).decode().split('|');assert len(parts)==11
                    if parts[8]=='ui/message':
                        values=dict(parse_qsl(parts[10].replace(';','&'),keep_blank_values=True))
                        assert values['id']==str(id)
                        return values
            def state_for(id):
                return next((v['state'] for v in snapshot().get('display',{}).get('deliveries',[]) if v['id']==str(id)),None)
            for kind,id in [('file',201),('image',202),('sticker',203)]:
                chat.sendall(packet({'type':6,'msg_id':str(id),'fromId':1000,'toId':4242,
                                    'content_type':kind,'msg':'PC attachment only',
                                    'attachment':{'attachment_id':'1000-test','file_name':'attachment.bin'}}))
            chat.sendall(packet({'type':6,'msg_id':'204','fromId':1000,'toId':4242,
                                'msg':'attachment disguised as text','attachment':{}}))
            send_message(101,text);values=message(101);assert values['text']==text
            assert all(state_for(id) is None for id in (201,202,203,204))
            ack=envelope('ui/message_ack','id=101;status=accepted')
            board.sendall(ack[:2]);board.sendall(ack[2:7]);board.sendall(ack[7:])
            wait(lambda:state_for(101)=='accepted')
            assert message(101)['text']==text  # retry before rendered
            board.sendall(envelope('ui/message_ack','id=101;status=rendered'))
            wait(lambda:state_for(101)=='rendered')
            assert snapshot()['display']['rendered']==1
            send_message(101,text);send_message(102,'wrong recipient',to=9999)
            send_message(103,'a'*385)
            wait(lambda:state_for(103)=='rejected')
            assert snapshot()['display']['pending']==0
            board.close();board=None
            wait(lambda:not snapshot().get('board_connected'))
            send_message(104,'reconnect message');wait(lambda:state_for(104)=='queued')
            board=socket.create_connection(('127.0.0.1',board_port));board.settimeout(8)
            board.sendall(envelope('ui/capabilities','version=1;text_max=384'))
            assert message(104)['text']=='reconnect message'
            board.sendall(envelope('ui/message_ack','id=104;status=rendered'))
            wait(lambda:state_for(104)=='rendered')
            assert snapshot()['display']['rendered']==2 and snapshot()['decode_failures']==0
            print('PASS display bridge: navigation/view receipt, receive-only login, Chinese/escaping, fragmented ACK, retry, dedup, recipient filter, size bound, reconnect')
        finally:
            if board:board.close()
            if chat:chat.close()
            server.close();stop.write_text('stop')
            try:assert process.wait(timeout=5)==0
            except subprocess.TimeoutExpired:process.kill();raise
if __name__=='__main__':main()
