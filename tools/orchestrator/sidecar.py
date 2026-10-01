"""The orchestrator's voice sidecar. Runs on Windows, next to the game.

It records while the push-to-talk key is held (MOUSE4 by default, only while
the game has the focus; --always-on listens all the time instead), turns
speech into text on the GPU (faster-whisper),
watches the game's shots (orch/events), and groups both into turns on one
clock: a turn ends after a pause in speech. Each turn is written to
orch/sessions/<date>/turns/<n>.json and announced on stdout with one line,

  TURN 12 "make this guy flank left instead" shots=1 errors=0 /mnt/d/.../turns/0012.json

which is what the agent's Monitor turns into a notification. Replies the agent
writes to orch/say/*.txt are spoken with Kokoro; pressing the talk key cuts a
reply short.

Started by the agent from WSL (see tools/orchestrator/README.md):

  <venv>\\Scripts\\python.exe sidecar.py --home "D:\\Medal of Honor\\openmohaa-live\\home"

--wav file.wav feeds a recording instead of the microphone (for testing),
--list-devices shows the audio devices, --no-tts prints instead of speaking.
"""

import argparse
import warnings
import collections
import datetime
import glob
import json
import os
import queue
import re
import sys
import threading
import time


def add_cuda_dlls():
    """The CUDA libraries pip installs (nvidia-cublas-cu12, nvidia-cudnn-cu12)
    aren't on the DLL path by themselves."""
    try:
        import nvidia  # noqa: F401
    except ImportError:
        return
    for base in getattr(sys.modules["nvidia"], "__path__", []):
        for bin_dir in glob.glob(os.path.join(base, "*", "bin")):
            os.add_dll_directory(bin_dir)
            os.environ["PATH"] = bin_dir + os.pathsep + os.environ.get("PATH", "")


add_cuda_dlls()
os.environ.setdefault("HF_HUB_DISABLE_SYMLINKS_WARNING", "1")
warnings.filterwarnings("ignore", category=UserWarning)

import numpy as np  # noqa: E402

RATE = 16000
FRAME_MS = 30
FRAME = RATE * FRAME_MS // 1000

# The words the game and this project use, so Whisper spells them right.
PROMPT = ("Medal of Honor, OpenMoHAA, MOHAA, cvar, ragdoll, AI, AI paths, actor, patrol, path node, "
          "splinepath, trigger, script, thread, noclip, savegame, shader, texture, GL2, m1l1, m3l2.")

# What Whisper makes up from noise.
HALLUCINATIONS = {"", "you", "thank you.", "thanks for watching!", "thank you for watching.", ".", "bye."}

LOCAL_COMMANDS = {
    "mute": re.compile(r"^\W*(mute|stop listening)\W*$", re.I),
    "unmute": re.compile(r"^\W*(unmute|start listening)\W*$", re.I),
    "cancel": re.compile(r"^\W*(cancel that|never mind|scratch that)\W*$", re.I),
}


def now():
    return time.time()


def iso(t):
    return datetime.datetime.fromtimestamp(t).isoformat(timespec="milliseconds")


def to_wsl(path):
    """D:\\x\\y -> /mnt/d/x/y, for the agent in WSL."""
    m = re.match(r"^([A-Za-z]):[\\/](.*)$", path)
    if not m:
        return path
    return "/mnt/" + m.group(1).lower() + "/" + m.group(2).replace("\\", "/")


# Virtual-key codes for --ptt-key.
KEYS = {"mouse3": 0x04, "mouse4": 0x05, "mouse5": 0x06, "space": 0x20, "tab": 0x09, "capslock": 0x14,
        "shift": 0x10, "ctrl": 0x11, "alt": 0x12, "backquote": 0xC0, "insert": 0x2D, "home": 0x24}
KEYS.update({f"f{i}": 0x6F + i for i in range(1, 13)})
KEYS.update({c: ord(c.upper()) for c in "abcdefghijklmnopqrstuvwxyz0123456789"})


def key_code(name):
    if name.lower() not in KEYS:
        sys.exit(f"--ptt-key: unknown key {name}; one of {', '.join(sorted(KEYS))}")
    return KEYS[name.lower()]


def foreground_exe():
    """The file name of the program whose window has the focus (Windows)."""
    import ctypes
    from ctypes import wintypes
    user32, kernel32 = ctypes.windll.user32, ctypes.windll.kernel32
    pid = wintypes.DWORD()
    user32.GetWindowThreadProcessId(user32.GetForegroundWindow(), ctypes.byref(pid))
    handle = kernel32.OpenProcess(0x1000, False, pid.value)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not handle:
        return ""
    try:
        buf = ctypes.create_unicode_buffer(1024)
        size = wintypes.DWORD(len(buf))
        if not kernel32.QueryFullProcessImageNameW(handle, 0, buf, ctypes.byref(size)):
            return ""
        return os.path.basename(buf.value).lower()
    finally:
        kernel32.CloseHandle(handle)


class Sidecar:
    def __init__(self, args):
        self.args = args
        self.main = os.path.join(args.home, "main")
        self.orch = os.path.join(self.main, "orch")
        self.cmd_dir = os.path.join(self.orch, "cmd")
        self.say_dir = os.path.join(self.orch, "say")
        self.events_dir = os.path.join(self.orch, "events")
        for d in (self.cmd_dir, self.say_dir, self.events_dir):
            os.makedirs(d, exist_ok=True)

        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        self.session = os.path.join(self.orch, "sessions", stamp)
        self.turns_dir = os.path.join(self.session, "turns")
        os.makedirs(self.turns_dir, exist_ok=True)
        self.timeline = open(os.path.join(self.session, "timeline.jsonl"), "a", encoding="utf-8")

        self.lock = threading.Lock()
        self.utterances = queue.Queue()   # (start, end, audio) to transcribe
        self.pending = []                 # events of the turn being gathered
        self.last_activity = 0.0          # when speech last ended, or a shot came
        self.in_speech = False
        self.muted = False
        self.speaking_until = 0.0         # the mic is deaf until then
        self.turn_number = 0
        self.status = ""
        self.status_time = 0.0
        self.stop = threading.Event()
        self.ready = threading.Event()       # Whisper loaded
        self.sd = None                       # sounddevice, once speaking
        self.speaking = False
        self.wav_time = 0.0                  # --wav: seconds played
        self.ptt_holds = []                  # --ptt-test: when the key is "held"
        if args.ptt_test:
            for span in args.ptt_test.split(","):
                a, b = span.split("-")
                self.ptt_holds.append((float(a), float(b)))
        self.ptt_vk = key_code(args.ptt_key) if not args.always_on else 0
        self.focus_checked = 0.0
        self.game_focused = False
        self.audio_done = threading.Event()  # --wav played out

    # ------------------------------------------------------------ output

    def log(self, event):
        with self.lock:
            self.timeline.write(json.dumps(event) + "\n")
            self.timeline.flush()

    def emit(self, line):
        print(line, flush=True)

    def game(self, commands):
        """Console commands for the game, through its command directory."""
        name = f"sc_{time.time_ns()}"
        tmp = os.path.join(self.cmd_dir, name + ".tmp_w")
        try:
            with open(tmp, "w", encoding="utf-8", newline="\n") as f:
                f.write(commands + "\n")
            os.replace(tmp, os.path.join(self.cmd_dir, name + ".txt"))
        except OSError:
            pass

    def set_status(self, status):
        if status != self.status:
            self.status = status
            self.status_time = now()
            self.game(f"orch_status {status}")

    def idle_status(self):
        if self.muted:
            return "muted"
        return "listening" if self.args.always_on else "ready " + self.args.ptt_key.upper()

    # ------------------------------------------------------------ listening

    def key_down(self):
        if self.ptt_holds:
            return any(a <= self.wav_time < b for a, b in self.ptt_holds)
        import ctypes
        if not ctypes.windll.user32.GetAsyncKeyState(self.ptt_vk) & 0x8000:
            return False
        # Only for the game: the key means something else elsewhere.
        t = now()
        if t - self.focus_checked > 0.3:
            self.focus_checked = t
            self.game_focused = foreground_exe().startswith("openmohaa") or self.args.any_window
        return self.game_focused

    def interrupt_speech(self):
        if self.speaking and self.sd:
            self.sd.stop()
        self.speaking_until = 0.0

    def listen(self):
        if self.args.always_on:
            self.listen_always()
        else:
            self.listen_ptt()

    def listen_ptt(self):
        """Records while the talk key is held, plus a little before and after."""
        import webrtcvad
        self.ready.wait()
        vad = webrtcvad.Vad(self.args.vad)
        pre = collections.deque(maxlen=400 // FRAME_MS)   # words started just before the press
        tail_frames = 300 // FRAME_MS                     # and finished just after the release
        held = False
        tail = 0
        frames = []
        start = 0.0

        for frame in self.audio_frames():
            t = now()
            down = not self.muted and self.key_down()
            if down and not held:
                held = True
                self.in_speech = True
                start = t - len(pre) * FRAME_MS / 1000
                frames = list(pre)
                pre.clear()
                tail = 0
                self.interrupt_speech()
                self.set_status("listening")
            if not held:
                pre.append(frame)
                continue

            frames.append(frame)
            if down:
                tail = 0
                continue
            tail += 1
            if tail < tail_frames:
                continue

            held = False
            audio = np.concatenate(frames)
            frames = []
            voiced = sum(1 for i in range(0, len(audio) - FRAME + 1, FRAME)
                         if vad.is_speech(audio[i:i + FRAME].tobytes(), RATE))
            with self.lock:
                self.last_activity = t
                self.in_speech = False
            self.set_status(self.idle_status())
            # A tap, or a press with nothing said, is not an utterance.
            if voiced >= 5:
                self.utterances.put((start, t, audio))


    def audio_frames(self):
        """30 ms int16 frames from the microphone, or from --wav."""
        if self.args.wav:
            import wave
            with wave.open(self.args.wav, "rb") as w:
                if w.getframerate() != RATE or w.getnchannels() != 1 or w.getsampwidth() != 2:
                    sys.exit("--wav must be 16 kHz mono 16-bit")
                data = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
            for i in range(0, len(data) - FRAME, FRAME):
                self.wav_time = i / RATE
                yield data[i:i + FRAME]
                time.sleep(FRAME_MS / 1000)
            # Silence after it, so the last utterance ends.
            for k in range(200):
                self.wav_time = len(data) / RATE + k * FRAME_MS / 1000
                yield np.zeros(FRAME, dtype=np.int16)
                time.sleep(FRAME_MS / 1000)
            self.audio_done.set()
            return

        import sounddevice as sd
        q = queue.Queue()

        def callback(indata, frames, t, status):
            q.put(indata[:, 0].copy())

        with sd.InputStream(samplerate=RATE, channels=1, dtype="int16", blocksize=FRAME,
                            device=self.args.device, callback=callback):
            while not self.stop.is_set():
                try:
                    yield q.get(timeout=0.5)
                except queue.Empty:
                    continue

    def listen_always(self):
        """Voice activity detection decides when speech starts and ends."""
        import webrtcvad
        self.ready.wait()
        vad = webrtcvad.Vad(self.args.vad)
        ring = collections.deque(maxlen=10)    # the 300 ms before speech starts
        voiced = []
        silence = 0
        start = 0.0
        end_frames = int(self.args.end_silence * 1000 / FRAME_MS)

        for frame in self.audio_frames():
            t = now()
            deaf = self.muted or t < self.speaking_until
            if deaf:
                ring.clear()
                if self.in_speech:
                    self.in_speech = False
                    voiced = []
                continue

            speech = vad.is_speech(frame.tobytes(), RATE)
            if not self.in_speech:
                ring.append((frame, speech))
                if sum(1 for _, s in ring if s) >= 6:
                    self.in_speech = True
                    start = t - len(ring) * FRAME_MS / 1000
                    voiced = [f for f, _ in ring]
                    ring.clear()
                    silence = 0
                continue

            voiced.append(frame)
            silence = 0 if speech else silence + 1
            if silence >= end_frames:
                self.in_speech = False
                audio = np.concatenate(voiced[:len(voiced) - silence + 3])
                voiced = []
                with self.lock:
                    self.last_activity = t
                if len(audio) >= RATE * 0.3:
                    self.utterances.put((start, t - self.args.end_silence, audio))

    # ------------------------------------------------------------ speech to text

    def load_whisper(self):
        from faster_whisper import WhisperModel
        tries = [("cuda", "int8"), ("cuda", "int8_float32"), ("cpu", "int8")]
        if self.args.cpu:
            tries = tries[2:]
        for device, compute in tries:
            try:
                model = WhisperModel(self.args.model, device=device, compute_type=compute)
                # A real transcription, so a missing CUDA library shows up now.
                list(model.transcribe(np.zeros(RATE, dtype=np.float32), language="en")[0])
                self.emit(f"INFO whisper {self.args.model} on {device} ({compute})")
                return model
            except Exception as e:  # noqa: BLE001
                self.emit(f"INFO whisper on {device}/{compute} failed: {e}")
        sys.exit("no Whisper backend worked")

    def transcribe(self):
        model = self.load_whisper()
        self.ready.set()
        self.emit("READY " + ("listening" if self.args.always_on else f"push to talk: {self.args.ptt_key}"))
        self.set_status(self.idle_status())
        while not self.stop.is_set():
            try:
                start, end, audio = self.utterances.get(timeout=0.5)
            except queue.Empty:
                continue
            segments, _ = model.transcribe(audio.astype(np.float32) / 32768.0, language="en", beam_size=5,
                                           initial_prompt=PROMPT, condition_on_previous_text=False)
            text = " ".join(s.text.strip() for s in segments).strip()
            if text.lower() in HALLUCINATIONS:
                continue
            self.heard(start, end, text)

    def heard(self, start, end, text):
        self.log({"type": "speech", "start": iso(start), "end": iso(end), "text": text})

        for name, pattern in LOCAL_COMMANDS.items():
            if pattern.match(text):
                self.local_command(name)
                return

        # One line, no quotes of its own: the game splits command files on newlines.
        safe = re.sub(r"[\r\n]+", " ", text).replace('"', "'")
        self.game('orch_msg -heard "' + safe + '"')
        with self.lock:
            self.pending.append({"type": "speech", "t": start, "start": iso(start), "end": iso(end), "text": text})
            self.last_activity = max(self.last_activity, end)

    def local_command(self, name):
        if name == "mute":
            self.muted = True
            self.set_status("muted")
            self.game('orch_msg -heard "(muted)"')
        elif name == "unmute":
            self.muted = False
            self.set_status(self.idle_status())
            self.game('orch_msg -heard "(listening)"')
        elif name == "cancel":
            with self.lock:
                self.pending = []
            self.game('orch_msg -heard "(dropped)"')
        self.log({"type": "local", "command": name, "time": iso(now())})

    # ------------------------------------------------------------ shots and the console

    def watch_game(self):
        seen = set(os.path.basename(p) for p in glob.glob(os.path.join(self.events_dir, "*.json")))
        log_path = os.path.join(self.main, "qconsole.log")
        log_pos = os.path.getsize(log_path) if os.path.isfile(log_path) else 0
        tick = 0

        while not self.stop.is_set():
            t = now()
            for path in sorted(glob.glob(os.path.join(self.events_dir, "*.json"))):
                name = os.path.basename(path)
                if name in seen:
                    continue
                seen.add(name)
                try:
                    with open(path, encoding="utf-8") as f:
                        shot = json.load(f)
                except (OSError, ValueError):
                    seen.discard(name)  # still being written
                    continue
                files = {k: to_wsl(os.path.join(self.main, v.replace("/", os.sep)))
                         for k, v in shot.get("files", {}).items() if k != "savegame"}
                event = {"type": "shot", "t": t, "time": iso(t), "id": shot.get("id"),
                         "summary": shot.get("summary"), "json": to_wsl(path), "files": files,
                         "savegame": shot.get("files", {}).get("savegame")}
                self.log(event)
                with self.lock:
                    self.pending.append(event)
                    self.last_activity = max(self.last_activity, t)

            # Script errors and the like, from the console log.
            if os.path.isfile(log_path):
                size = os.path.getsize(log_path)
                if size < log_pos:
                    log_pos = 0
                if size > log_pos:
                    with open(log_path, encoding="utf-8", errors="replace") as f:
                        f.seek(log_pos)
                        chunk = f.read()
                        log_pos = f.tell()
                    for line in chunk.splitlines():
                        if re.search(r"\^~\^~\^|Script Error|ERROR:|WARNING: .*script|Couldn't (find|load)", line, re.I):
                            event = {"type": "console", "t": now(), "time": iso(now()), "line": line.strip()}
                            self.log(event)
                            with self.lock:
                                self.pending.append(event)

            # Keep the command directory tidy: the game answers every file.
            tick += 1
            if tick % 20 == 0:
                for out in glob.glob(os.path.join(self.cmd_dir, "sc_*.out")):
                    try:
                        os.remove(out)
                    except OSError:
                        pass
                with open(os.path.join(self.orch, "sidecar.alive"), "w") as f:
                    f.write(str(os.getpid()))

            self.flush_turn()
            # The agent may answer a turn by doing nothing.
            if self.status == "thinking" and now() - self.status_time > 30:
                self.set_status(self.idle_status())
            time.sleep(0.05)

    def flush_turn(self):
        with self.lock:
            if not self.pending or self.in_speech or not self.utterances.empty():
                return
            has_speech = any(e["type"] == "speech" for e in self.pending)
            wait = self.args.turn_silence if has_speech else self.args.shot_wait
            if now() - self.last_activity < wait:
                return
            if not has_speech and all(e["type"] == "console" for e in self.pending):
                # Errors on their own wait for the next real turn.
                return
            events = sorted(self.pending, key=lambda e: e["t"])
            self.pending = []

        self.turn_number += 1
        speech = " ".join(e["text"] for e in events if e["type"] == "speech")
        shots = [e for e in events if e["type"] == "shot"]
        errors = [e for e in events if e["type"] == "console"]
        turn = {
            "turn": self.turn_number,
            "start": events[0].get("time") or events[0].get("start"),
            "end": iso(now()),
            "speech": speech,
            "events": [{k: v for k, v in e.items() if k != "t"} for e in events],
        }
        path = os.path.join(self.turns_dir, f"{self.turn_number:04d}.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump(turn, f, indent=1)
        self.log({"type": "turn", "turn": self.turn_number, "time": iso(now()), "path": to_wsl(path)})
        self.set_status("thinking")
        quoted = json.dumps(speech[:160])
        self.emit(f"TURN {self.turn_number} {quoted} shots={len(shots)} errors={len(errors)} {to_wsl(path)}")

    # ------------------------------------------------------------ speaking

    def load_kokoro(self):
        from kokoro_onnx import Kokoro
        models = os.path.join(os.environ.get("LOCALAPPDATA", "."), "openmohaa-orch", "kokoro")
        os.makedirs(models, exist_ok=True)
        base = "https://github.com/thewh1teagle/kokoro-onnx/releases/download/model-files-v1.0/"
        files = {}
        for name in ("kokoro-v1.0.onnx", "voices-v1.0.bin"):
            dest = os.path.join(models, name)
            if not os.path.isfile(dest):
                import urllib.request
                self.emit(f"INFO downloading {name}")
                urllib.request.urlretrieve(base + name, dest + ".part")
                os.replace(dest + ".part", dest)
            files[name] = dest
        return Kokoro(files["kokoro-v1.0.onnx"], files["voices-v1.0.bin"])

    def speak(self):
        kokoro = None
        if not self.args.no_tts:
            try:
                kokoro = self.load_kokoro()
                self.emit(f"INFO kokoro voice {self.args.voice}")
            except Exception as e:  # noqa: BLE001
                self.emit(f"INFO kokoro failed ({e}); replies are text only")
        if kokoro:
            import sounddevice
            self.sd = sounddevice

        while not self.stop.is_set():
            items = sorted(glob.glob(os.path.join(self.say_dir, "*.txt")))
            if not items:
                time.sleep(0.1)
                continue
            for path in items:
                try:
                    with open(path, encoding="utf-8") as f:
                        text = f.read().strip()
                    os.remove(path)
                except OSError:
                    continue
                self.log({"type": "reply", "time": iso(now()), "text": text})
                if not kokoro or not text:
                    self.set_status(self.idle_status())
                    continue
                try:
                    samples, rate = kokoro.create(text, voice=self.args.voice, speed=self.args.speed, lang="en-us")
                except Exception as e:  # noqa: BLE001
                    self.emit(f"INFO speech failed: {e}")
                    continue
                length = len(samples) / rate
                if self.in_speech:
                    # They're talking: hold the reply until they let go.
                    while self.in_speech and not self.stop.is_set():
                        time.sleep(0.05)
                self.set_status("speaking")
                self.speaking = True
                self.speaking_until = now() + length + 0.3
                try:
                    self.sd.play(samples, rate)
                    self.sd.wait()
                except Exception as e:  # noqa: BLE001
                    self.emit(f"INFO playback failed: {e}")
                self.speaking = False
                if self.speaking_until:
                    self.speaking_until = now() + 0.3
                    time.sleep(0.3)
            self.set_status(self.idle_status())

    # ------------------------------------------------------------

    def run(self):
        self.emit(f"INFO session {to_wsl(self.session)}")
        threads = [threading.Thread(target=f, daemon=True)
                   for f in (self.listen, self.transcribe, self.watch_game, self.speak)]
        for t in threads:
            t.start()
        try:
            while not self.stop.is_set() and not self.audio_done.is_set():
                time.sleep(0.2)
            # --wav: let the last turn out.
            deadline = now() + 30
            while now() < deadline and (self.pending or not self.utterances.empty()):
                time.sleep(0.2)
            time.sleep(self.args.turn_silence + 0.5)
            self.stop.set()
        except KeyboardInterrupt:
            pass
        self.set_status("off")
        self.emit("STOPPED")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--home", required=True, help="the live install's home path (has main\\orch)")
    ap.add_argument("--model", default="small.en", help="Whisper model (tiny.en, base.en, small.en, medium.en)")
    ap.add_argument("--cpu", action="store_true", help="transcribe on the CPU")
    ap.add_argument("--device", default=None, help="input device (name or index); see --list-devices")
    ap.add_argument("--ptt-key", default="mouse4", help="push-to-talk key (mouse4, mouse5, v, f11, capslock...)")
    ap.add_argument("--always-on", action="store_true", help="no talk key: listen all the time")
    ap.add_argument("--any-window", action="store_true", help="the talk key works whatever has the focus")
    ap.add_argument("--ptt-test", help=argparse.SUPPRESS)  # "0.2-4.5,5.0-9.0": --wav seconds the key is held
    ap.add_argument("--vad", type=int, default=2, help="voice detection aggressiveness 0-3")
    ap.add_argument("--end-silence", type=float, default=0.7, help="seconds of quiet that end an utterance")
    ap.add_argument("--turn-silence", type=float, default=None,
                    help="seconds of quiet that end a turn (1.5 with the talk key, 2.0 always on)")
    ap.add_argument("--shot-wait", type=float, default=4.0, help="seconds a shot waits for speech")
    ap.add_argument("--voice", default="am_michael", help="Kokoro voice")
    ap.add_argument("--speed", type=float, default=1.1)
    ap.add_argument("--no-tts", action="store_true")
    ap.add_argument("--wav", help="16 kHz mono WAV to use instead of the microphone")
    ap.add_argument("--list-devices", action="store_true")
    args = ap.parse_args()

    if args.list_devices:
        import sounddevice as sd
        print(sd.query_devices())
        return
    if args.turn_silence is None:
        args.turn_silence = 2.0 if args.always_on else 1.5
    if args.device is not None and args.device.isdigit():
        args.device = int(args.device)

    sys.stdout.reconfigure(line_buffering=True)
    Sidecar(args).run()


if __name__ == "__main__":
    main()
