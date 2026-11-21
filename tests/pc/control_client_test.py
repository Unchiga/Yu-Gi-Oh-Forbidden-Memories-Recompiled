"""yfm_control.Game.command against a made-up game (CTest pc_control_client): no game needed.

A game that is slow to answer is waited for, not taken for one that closed
its channel; a channel the game closes says whether the game still runs
or how it ended, and ends with its log's last lines (none, or the end of
a long one); a game that does not answer in command_timeout is ended
(GameHung). notes/agent-control.md, "Transport"."""
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/pc"))
import yfm_control  # noqa: E402


def made_up_game(work: Path, serve, command_timeout: float = 5.0, exit_code: int | None = None) -> yfm_control.Game:
    """A Game whose process is a sleeping Python (or one that has ended with
    exit_code) and whose channel is served by serve(connection) on a thread."""
    listener = socket.create_server(("127.0.0.1", 0))
    port = listener.getsockname()[1]

    def accept() -> None:
        connection, _ = listener.accept()
        listener.close()
        try:
            serve(connection)
        finally:
            connection.close()

    threading.Thread(target=accept, daemon=True).start()
    game = object.__new__(yfm_control.Game)
    game.command_timeout = command_timeout
    game._hung = ""
    game._buffer = b""
    game.frame = game.vblank = 0
    game.log_path = work / "game.log"
    game.log_path.write_text("memories-pc: control: the client took none of a reply for a second (3 bytes left); "
                             "it is dropped\n")
    if exit_code is None:
        game.process = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
    else:
        game.process = subprocess.Popen([sys.executable, "-c", f"raise SystemExit({exit_code})"])
        game.process.wait()
    game.connection = socket.create_connection(("127.0.0.1", port), timeout=5)
    game.connection.settimeout(command_timeout)
    return game


def read_line(connection: socket.socket) -> bytes:
    data = b""
    while not data.endswith(b"\n"):
        chunk = connection.recv(1)
        if not chunk:
            break
        data += chunk
    return data


def main() -> None:
    with tempfile.TemporaryDirectory() as folder:
        work = Path(folder)

        # Slow, not gone: the reply comes 2 s late, in pieces.
        def slow(connection: socket.socket) -> None:
            assert read_line(connection) == b"info\n"
            time.sleep(2)
            connection.sendall(b"ok frame 5 ")
            time.sleep(0.2)
            connection.sendall(b"vblank 9\n")

        game = made_up_game(work, slow)
        started = time.monotonic()
        assert game.command("info") == "frame 5 vblank 9"
        assert time.monotonic() - started >= 1.9
        assert game.process.poll() is None
        game.process.kill()
        game.process.wait()

        # Closed by a game that runs on: the error says so, with the log.
        def drop(connection: socket.socket) -> None:
            read_line(connection)

        game = made_up_game(work, drop)
        try:
            game.command("step 1")
        except yfm_control.ControlError as error:
            text = str(error)
            assert text.startswith("the game closed the channel after 'step 1' (the game still runs)"), text
            assert "took none of a reply for a second" in text, text
        else:
            raise AssertionError("no ControlError for a closed channel")
        game.process.kill()
        game.process.wait()

        # Closed by a game that has ended: the error says how.
        game = made_up_game(work, drop, exit_code=3)
        try:
            game.command("step 1")
        except yfm_control.ControlError as error:
            assert str(error).startswith("the game closed the channel after 'step 1' (the game ended with 3)"), error
        else:
            raise AssertionError("no ControlError for a closed channel")

        # The log's end: a log that is not there, and one past the 64 KiB read.
        game.log_path = work / "missing.log"
        assert game._log_tail().startswith("(unreadable: "), game._log_tail()
        game.log_path = work / "long.log"
        game.log_path.write_bytes(b"".join(b"line %d\n" % number for number in range(20000)))   # about 190 KiB
        assert game.log_path.stat().st_size > 0x10000
        assert game._log_tail(3) == "line 19997\nline 19998\nline 19999\n", game._log_tail(3)

        # No reply in command_timeout: a hung game, which is ended.
        def silent(connection: socket.socket) -> None:
            read_line(connection)
            time.sleep(3)

        game = made_up_game(work, silent, command_timeout=0.5)
        try:
            game.command("step 1")
        except yfm_control.GameHung:
            pass
        else:
            raise AssertionError("no GameHung past command_timeout")
        assert game.process.poll() is not None
    print("control client: a slow reply is waited for; a closed channel (game running or ended), "
          "the log's end and a hung game are told apart")


if __name__ == "__main__":
    main()
