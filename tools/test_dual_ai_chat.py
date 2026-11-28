"""Two real LanChat sessions, two provider credentials and one board TCP connection."""
import argparse
import base64
from collections import Counter
import http.server
import importlib.util
import json
import os
import pathlib
import queue
import select
import socket
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import parse_qsl

from test_display_bridge import envelope, receive, wait

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "peer", ROOT.parent / "LanChat-Next/scripts/test-lan-integration.py"
)
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
Peer = mod.Peer


def free_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def run(server_exe, console_exe):
    calls = queue.Queue()
    queries = []
    overlaps = []
    events = []
    hold_ds = threading.Event()
    release_ds = threading.Event()
    hold_group = threading.Event()
    release_group = threading.Event()
    hold_question_audio = threading.Event()
    question_audio_received = queue.Queue()
    group_question = "群讨论题：雨衣还是雨伞？"
    group_answers = (
        "风大就穿雨衣，腾得出手。",
        "小鲸鱼，风大确实用雨衣。无风短途可以带伞。",
        "前辈这个短途建议好。风大穿雨衣，无风短途就带伞。",
    )

    class Model(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            calls.put((self.path, self.headers.get("Authorization"), body))
            prompt = body["messages"][-1]["content"]
            answer = body["model"] + " verified"
            if prompt == "DS 慢回复":
                hold_ds.set()
                release_ds.wait(10)
            if prompt.startswith('{"earlier_turns":'):
                group = json.loads(prompt)
                turn = len(group["earlier_turns"])
                if turn == 0:
                    hold_group.set()
                    release_group.wait(10)
                answer = group_answers[turn]
            response = json.dumps({
                "choices": [{"message": {"content": answer}}]
            }).encode()
            self.send_response(200)
            self.send_header("Content-Length", str(len(response)))
            self.end_headers()
            self.wfile.write(response)

    with tempfile.TemporaryDirectory(prefix="dual-ai-") as name:
        folder = pathlib.Path(name)
        summary, stop = folder / "status.json", folder / "stop.request"
        lan_port, board_port = free_port(), free_port()
        while board_port == lan_port:
            board_port = free_port()
        model = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Model)
        threading.Thread(target=model.serve_forever, daemon=True).start()
        server = subprocess.Popen([server_exe, "--port", str(lan_port), "--data", str(folder / "data")],
                                  cwd=folder, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        console, board, user, outsider = None, None, None, None

        def snapshot():
            try:
                return json.loads(summary.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                return {}

        def register():
            def available():
                try:
                    peer = Peer(lan_port)
                    peer.close()
                    return True
                except OSError:
                    return False
            wait(available)
            ids = {}
            for nick in ("Tester", "RoboMind-S3_DS", "RoboMind-S3_GPT", "Outsider"):
                with socket_peer(lan_port) as peer:
                    peer.send(type=0, nickname=nick, password="isolated-test-password")
                    res = peer.wait(1)
                    assert res["status"] == "ok", res
                    ids[nick] = res["id"]
            return ids

        try:
            ids = register()
            ds, gpt = ids["RoboMind-S3_DS"], ids["RoboMind-S3_GPT"]
            user = Peer(lan_port)
            sender = user.login("Tester")["id"]
            user.send(type=9, hostId=sender, name="Discussion fixture")
            group = user.wait(10, status="ok")["groupId"]
            user.send(type=9, hostId=sender, name="Other group")
            other_group = user.wait(10, status="ok")["groupId"]
            (folder / "robomind-discussion.ini").write_text(
                f"[discussion]\ngroup_id={group}\nhost_id={sender}\n"
            )
            (folder / "robomind-console.ini").write_text(
                f"[lanchat]\nenabled=true\nhost=127.0.0.1\nport={lan_port}\nreceive_messages=true\n"
            )
            base = f"http://127.0.0.1:{model.server_port}"
            voice = folder / "voice-backend.json"
            trace = folder / "voices.log"
            worker = folder / "voice-worker.py"
            (folder / "voice.json").write_text("{}")
            worker.write_text(
                "import base64, json, pathlib, sys\n"
                "for line in sys.stdin:\n"
                "    request = json.loads(line)\n"
                "    with pathlib.Path('voices.log').open('a') as trace:\n"
                "        trace.write(request.get('voice', 'whale') + '\\n')\n"
                "    pcm = base64.b64encode(bytes(320)).decode()\n"
                "    print(json.dumps(dict(id=request['id'], sample_rate=16000, pcm=pcm)), flush=True)\n"
            )
            voice.write_text(json.dumps({"python": sys.executable, "worker": str(worker),
                                         "runtime": str(folder), "dragon_voice": True}))
            env = {k: v for k, v in os.environ.items()
                   if not k.startswith(("ROBOMIND_AI_", "ROBOMIND_GPT_", "ROBOMIND_LANCHAT_"))}
            env.update(ROBOMIND_LANCHAT_USER_ID=str(ds), ROBOMIND_LANCHAT_PASSWORD="isolated-test-password",
                       ROBOMIND_AI_ENABLED="1", ROBOMIND_AI_PROVIDER="openai",
                       ROBOMIND_AI_ENDPOINT=base + "/ds/v1", ROBOMIND_AI_MODEL="ds-fixture",
                       ROBOMIND_AI_KEY="ds-fixture-key", ROBOMIND_AI_INTERVAL_SECONDS="0",
                       ROBOMIND_AI_USAGE_FILE=str(folder / "ds.json"),
                       ROBOMIND_GPT_ENABLED="1", ROBOMIND_GPT_USER_ID=str(gpt),
                       ROBOMIND_GPT_PASSWORD="isolated-test-password", ROBOMIND_GPT_PROVIDER="openai",
                       ROBOMIND_GPT_ENDPOINT=base + "/gpt/v1", ROBOMIND_GPT_MODEL="gpt-fixture",
                       ROBOMIND_GPT_KEY="gpt-fixture-key", ROBOMIND_GPT_USAGE_FILE=str(folder / "gpt.json"),
                       ROBOMIND_TTS_PROVIDER="local_vits", ROBOMIND_TTS_CONFIG=str(voice))
            console = subprocess.Popen([console_exe, "--bridge", "--port", str(board_port),
                                        "--status-file", str(summary), "--stop-file", str(stop)],
                                       cwd=folder, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            wait(lambda: snapshot().get("lanchat_state") == "已登录" and
                 snapshot().get("gpt_lanchat_state") == "已登录")
            board = socket.create_connection(("127.0.0.1", board_port))
            board.settimeout(8)
            board_lock = threading.Lock()

            def send_board(topic, text):
                with board_lock:
                    board.sendall(envelope(topic, text))

            capabilities = "version=1;text_max=384;navigation=1;query=1"
            send_board("ui/capabilities", capabilities)
            wait(lambda: snapshot().get("display", {}).get("query_supported"))

            def answer_board():
                active_audio = {}

                def audio_status(id, segment, size, done):
                    send_board("ui/audio_status", f"id={id};segment={segment};received={size};"
                               f"free=65536;done={int(done)};played={size if done else 0};rendered={int(done)}")

                def read_board():
                    parts = receive(board).decode().split("|")
                    values = dict(parse_qsl(parts[10].replace(";", "&")))
                    topic = parts[8]
                    if topic == "ui/message":
                        send_board("ui/message_ack", f"id={values['id']};status=rendered")
                    if topic in ("ui/navigation", "ui/assistant_state", "ui/audio_start", "ui/audio_cancel"):
                        events.append((topic, values))
                    if topic == "ui/audio_start":
                        hold = hold_question_audio.is_set() and values["text"].startswith("群主提问")
                        active_audio[values["id"]] = (int(values["bytes"]), 0, hold)
                        audio_status(values["id"], values["seq"], 0, False)
                    if topic == "ui/audio_chunk":
                        expected, received, hold = active_audio[values["id"]]
                        received += len(base64.b64decode(values["data"]))
                        active_audio[values["id"]] = (expected, received, hold)
                        done = received == expected
                        audio_status(values["id"], values["segment"], received, done and not hold)
                        if done and hold:
                            question_audio_received.put((values["id"], values["segment"], received))
                    return values if topic == "device/query" else None

                try:
                    while True:
                        values = read_board()
                        if values is None:
                            continue
                        kind = values["kind"]
                        queries.append(kind)
                        # Match firmware: additional requests arriving while its one slot
                        # is occupied are dropped. The delay also gives both bots time to subscribe.
                        deadline = time.monotonic() + 0.20
                        while time.monotonic() < deadline:
                            ready, _, _ = select.select([board], [], [], max(0, deadline - time.monotonic()))
                            if not ready:
                                break
                            overlapping = read_board()
                            if overlapping is not None:
                                overlaps.append(overlapping["kind"])
                        data = {
                            "perception": "sample=0",
                            "link": "wifi=1;ip=192.0.2.3;rssi=-46;tcp=1;tx=23;rx=8;last_rx_age=0",
                            "device": "heap=33000;largest=16000;minimum=23000;psram=6000000;ppm=1;zao=0",
                        }
                        send_board("device/query_reply",
                                   f"id={values['id']};kind={kind};version=1;up=20000;" + data[kind])
                except (OSError, EOFError):
                    pass

            threading.Thread(target=answer_board, daemon=True).start()

            def send(to, text):
                user.send(type=5, fromId=sender, toId=to, msg=text)

            for to in (ds, gpt):
                send(to, "/链路")
            for to in (ds, gpt):
                message = user.wait(6, fromId=to, toId=sender)["msg"]
                assert "[S3 · 链路" in message and "-46 dBm" in message, message
            assert calls.empty() and queries == ["link"], queries
            queries.clear()

            first = {ds: "DS 独立问题", gpt: "GPT 独立问题"}
            for to in (ds, gpt):
                send(to, first[to])
            for to, name in ((ds, "ds"), (gpt, "gpt")):
                assert f"{name}-fixture verified" in user.wait(6, fromId=to, toId=sender)["msg"]
            asked = {call[2]["model"]: call for call in (calls.get_nowait(), calls.get_nowait())}
            for to, name in ((ds, "ds"), (gpt, "gpt")):
                path, key, body = asked[f"{name}-fixture"]
                assert path == f"/{name}/v1/chat/completions" and key == f"Bearer {name}-fixture-key"
                system, prompt = body["messages"]
                assert system["role"] == "system"
                instructions = system["content"]
                assert "-46 dBm" in instructions and "PSRAM 可用" in instructions
                assert "本轮提供的实时快照" in instructions and "不能声称看到了" in instructions
                if to == ds:
                    assert all(cue in instructions for cue in ("DeepSeek娘", "白米饭", "是哺乳动物", "鱼片"))
                    assert "GPT娘（白龙娘）" not in instructions
                else:
                    assert all(cue in instructions for cue in ("GPT娘（白龙娘）", "远古智慧龙族", "闪亮", "前辈"))
                    assert "DeepSeek娘（蓝色大肥鱼）" not in instructions
                assert prompt["content"] == first[to]
            assert Counter(queries) == {"link": 1, "perception": 1, "device": 1}, queries

            # A follow-up must contain only GPT's own conversation for the same human sender.
            time.sleep(2.05)  # The Console enforces a two-second per-conversation cooldown.
            send(gpt, "GPT 追问")
            answer = user.wait(6, fromId=gpt, toId=sender)["msg"]
            assert "gpt-fixture verified" in answer, answer
            body = calls.get_nowait()[2]
            assert [m["content"] for m in body["messages"] if m["role"] == "user"] == [first[gpt], "GPT 追问"]
            assert "GPT娘（白龙娘）" in body["messages"][0]["content"]
            assert "DeepSeek娘（蓝色大肥鱼）" not in body["messages"][0]["content"]
            wait(lambda: snapshot().get("gpt_ai", {}).get("replied") == 2)
            assert snapshot()["ai"]["replied"] == 1 and snapshot()["queries"]["timeouts"] == 0
            assert not overlaps, overlaps
            assert json.loads((folder / "ds.json").read_text())["calls"] == 1
            assert json.loads((folder / "gpt.json").read_text())["calls"] == 2
            print("PASS: two online accounts; one board; concurrent live queries; per-account replies, history, and limits")

            def show(persona, control, enabled=1):
                send_board("ui/view_state", f"page=0;immersion={enabled};persona={persona};control={control}")

            def event(start, topic, **fields):
                return wait(lambda: next((v for t, v in events[start:]
                                          if t == topic and all(v.get(k) == val for k, val in fields.items())), None))

            send_board("ui/capabilities", capabilities + ";immersion=1;dragon=1;pcm=16000")
            show("whale", "boot", enabled=0)
            wait(lambda: snapshot().get("display", {}).get("dragon_supported"))
            start = len(events)
            send(ds, "DS 慢回复")
            whale = event(start, "ui/navigation", action="immersion")["id"]
            show("whale", whale)
            old_speech = event(start, "ui/assistant_state", state="thinking")["id"]
            assert hold_ds.wait(4), "DS model did not receive the question"

            # Force an early GPT reply and a view report that does not acknowledge its switch.
            time.sleep(1.55)  # Combined with the fixture board reads, clears the GPT cooldown.
            start = len(events)
            send(gpt, "GPT 快回复")
            dragon = event(start, "ui/navigation", action="immersion_dragon")["id"]
            event(start, "ui/audio_cancel", id=old_speech)
            show("dragon", "stale-control")
            answer = user.wait(6, fromId=gpt, toId=sender)["msg"]
            assert "gpt-fixture verified" in answer, answer
            assert not any(t in ("ui/assistant_state", "ui/audio_start") for t, _ in events[start:])
            show("dragon", dragon)
            dragon_speech = event(start, "ui/assistant_state", state="thinking")["id"]
            assert dragon_speech != old_speech
            event(start, "ui/audio_start", id=dragon_speech, text="gpt-fixture verified")

            release_ds.set()
            assert "ds-fixture verified" in user.wait(6, fromId=ds, toId=sender)["msg"]
            final = wait(lambda: snapshot().get("speech", {}).get("stage") == "complete" and snapshot())
            assert final["display"]["view"]["persona"] == "dragon"
            assert final["speech"]["active_id"] == dragon_speech and final["speech"]["completed_segments"] == 1
            assert trace.read_text().splitlines() == ["dragon"]
            assert not any(t == "ui/navigation" and v["action"] == "immersion" for t, v in events[start:])

            start = len(events)
            send(ds, "/链路")
            whale = event(start, "ui/navigation", action="immersion")["id"]
            show("whale", whale)
            assert "-46 dBm" in user.wait(6, fromId=ds, toId=sender)["msg"]
            wait(lambda: snapshot().get("display", {}).get("view", {}).get("control") == whale)
            assert snapshot()["display"]["view"]["persona"] == "whale"
            print("PASS: private chat switches both personas; early answers wait for view ACK; stale replies stay in PC chat")

            assert {calls.get_nowait()[2]["messages"][-1]["content"] for _ in range(2)} == {"DS 慢回复", "GPT 快回复"}

            def roster():
                user.send(type=60, groupId=group)
                return {member["id"] for member in user.wait(61, groupId=group)["users"]}

            def group_send(peer, uid, gid, text):
                peer.send(type=16, fromId=uid, groupId=gid, msg=text)
                return peer.wait(17, fromId=uid, groupId=gid, msg=text)["msg_id"]

            outsider = Peer(lan_port)
            outsider_id = outsider.login("Outsider")["id"]
            outsider.send(type=13, id=outsider_id, groupId=group)
            outsider.wait(14, groupId=group, status="ok")
            assert roster() == {sender, ds, gpt, outsider_id}
            group_send(outsider, outsider_id, group, "/讨论 不是群主")
            group_send(user, sender, group, "/讨论 多一个成员")
            wait(lambda: snapshot().get("discussion", {}).get("last_result") == "roster_mismatch")
            assert calls.empty()
            outsider.send(type=15, id=outsider_id, groupId=group)
            outsider.wait(19, groupId=group, status="ok")
            assert roster() == {sender, ds, gpt}
            start_events, start_queries = len(events), len(queries)
            start_board_messages = snapshot()["display"]["deliveries"]
            group_send(user, sender, other_group, "/讨论 不在指定群")
            group_send(user, sender, group, "普通群消息")
            hold_question_audio.set()
            request = group_send(user, sender, group, f"/讨论 {group_question}")
            assert hold_group.wait(4)
            question_id, question_segment, question_bytes = question_audio_received.get(timeout=6)
            assert event(start_events, "ui/audio_start", id=question_id)["text"].replace("\n", "") == "群主提问：" + group_question
            group_send(user, sender, group, "/讨论 并发触发应被忽略")
            release_group.set()
            for turn, (speaker, answer) in enumerate(zip((ds, gpt, ds), group_answers), 1):
                received = user.wait(17, fromId=speaker, groupId=group)
                assert received["msg"] == f"[讨论 {turn}/3 · #{request}]\n{answer}", received

            wait(lambda: snapshot().get("discussion", {}).get("last_result") == "completed")
            transcript = []
            for role, model_name, answer, address in zip(
                ("DS", "GPT", "DS"), ("ds", "gpt", "ds"), group_answers, ("白龙前辈", "小鲸鱼", "前辈")
            ):
                route, key, body = calls.get_nowait()
                assert route == f"/{model_name}/v1/chat/completions" and key == f"Bearer {model_name}-fixture-key"
                system, prompt = body["messages"]
                assert address in system["content"] and "-46 dBm" not in system["content"]
                assert json.loads(prompt["content"]) == {"question": group_question, "earlier_turns": transcript}
                transcript.append({"speaker": role, "text": answer})
            assert calls.empty()
            assert [v["id"] for topic, v in events[start_events:] if topic == "ui/audio_start"] == [question_id]
            assert not any(topic == "ui/navigation" for topic, _ in events[start_events:])
            hold_question_audio.clear()
            send_board("ui/audio_status", f"id={question_id};segment={question_segment};received={question_bytes};"
                       f"free=65536;done=1;played={question_bytes};rendered=1")
            wait(lambda: event_index(events, start_events, "ui/audio_start", group_answers[0]) is not None)
            dragon = event(start_events, "ui/navigation", action="immersion_dragon")["id"]
            show("dragon", "wrong-group-ack")
            wait(lambda: snapshot()["display"]["view"].get("control") == "wrong-group-ack")
            assert event_index(events, start_events, "ui/audio_start", group_answers[1]) is None
            show("dragon", dragon)
            gpt_audio = wait(lambda: event_index(events, start_events, "ui/audio_start", group_answers[1]))
            whale = event(gpt_audio + 1, "ui/navigation", action="immersion")["id"]
            show("whale", whale)
            wait(lambda: event_index(events, gpt_audio + 1, "ui/audio_start", group_answers[2]))
            playback = wait(lambda: (value if (value := snapshot().get("discussion_playback", {})).get("state") == "completed" else None))
            assert playback["request_id"] == request
            assert [(r["turn"], r["persona"], r["segments"], r["rendered_segments"]) for r in playback["receipts"]] == [
                (0, "whale", 1, 1), (1, "whale", 1, 1), (2, "dragon", 1, 1), (3, "whale", 1, 1),
            ]
            assert trace.read_text().splitlines() == ["dragon", "whale", "whale", "dragon", "whale"]
            assert len(queries) == start_queries
            assert snapshot()["display"]["deliveries"] == start_board_messages
            assert json.loads((folder / "ds.json").read_text())["calls"] == 4
            assert json.loads((folder / "gpt.json").read_text())["calls"] == 4
            print("PASS: exact roster, host only, three model turns; question and replies play serially with verified role and subtitle receipts")

            # Production updates can stop Console while the board and voice worker are still connected.
            stop.write_text("stop")
            assert console.wait(timeout=6) == 0
        finally:
            release_ds.set()
            release_group.set()
            if board:
                board.close()
            if user:
                user.close()
            if outsider:
                outsider.close()
            if console and console.poll() is None:
                stop.write_text("stop")
                try:
                    console.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    console.kill()
                    console.wait()
            server.terminate()
            server.wait(timeout=8)
            model.shutdown()
            model.server_close()


class socket_peer:
    def __init__(self, port):
        self.peer = Peer(port)

    def __enter__(self):
        return self.peer

    def __exit__(self, *_):
        self.peer.close()


def event_index(events, start, topic, text):
    return next((i for i, (name, values) in enumerate(events[start:], start)
                 if name == topic and values.get("text", "").replace("\n", "") == text), None)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--console", default=ROOT / "build/dist/RoboMindConsole.exe")
    args = parser.parse_args()
    run(str(pathlib.Path(args.server).resolve()), str(pathlib.Path(args.console).resolve()))
