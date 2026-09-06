#!/usr/bin/env python3
"""Exercise the real Python publisher, HTTP bridge and Chromium UI using synthetic observations."""

import importlib.util
from pathlib import Path
import subprocess
import tempfile
import threading

from browser_smoke import bridge, root

spec = importlib.util.spec_from_file_location('python_demo', root / 'python_demo.py')
demo = importlib.util.module_from_spec(spec)
spec.loader.exec_module(demo)


def main():
    with tempfile.TemporaryDirectory(prefix='obs-python-fixture-') as directory:
        spool = Path(directory) / 'run'
        with demo.ObservatoryPublisher(spool, label='SYNTHETIC PYTHON FIXTURE', publish_interval=0) as publisher:
            # Retain a history before the browser opens; reloading must restore it.
            for tick in range(3):
                demo.publish_demo(publisher, tick)
            stopped = threading.Event()

            def updates():
                tick = 3
                while not stopped.wait(0.25):
                    demo.publish_demo(publisher, tick)
                    tick += 1

            worker = threading.Thread(target=updates)
            worker.start()
            server = bridge.ThreadingHTTPServer(('127.0.0.1', 0), bridge.Handler)
            server.daemon_threads = True
            server.spool = bridge.Spool(spool, 'fixture-token')
            serving = threading.Thread(target=server.serve_forever)
            serving.start()
            artifact = root / 'artifacts/local/browser-python-fixture.png'
            artifact.parent.mkdir(exist_ok=True)
            try:
                subprocess.run(['node', str(Path(__file__).with_name('browser_smoke.mjs')),
                                f'http://127.0.0.1:{server.server_port}', str(artifact), 'python'],
                               check=True, timeout=50)
                assert not (spool / 'control.txt').exists(), 'Browser must not control Python feeds'
            finally:
                stopped.set()
                worker.join()
                server.shutdown()
                server.server_close()
                serving.join()


if __name__ == '__main__':
    main()
