"""Production scheduler controls for the fault driver, without an SMB claim."""
import os
from pathlib import Path
import tempfile
import time
import unittest

from process_tree import Workflow, wait_terminal


class WorkflowTests(unittest.TestCase):
    def test_cancel_preserves_previous_output_and_allows_a_new_job(self):
        for fmt in ('v1', 'v2', 'hybrid'):
            with self.subTest(format=fmt), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                source, output = root / 'payload.bin', root / 'previous.torrent'
                source.write_bytes(b'x' * 1048576)
                with (root / 'stderr.log').open('w') as error:
                    workflow = Workflow(Path(os.environ['TC_WORKFLOW_PROOF']).resolve(),
                                        {'root': str(source), 'output': str(output), 'format': fmt, 'replace': True, 'probe': True}, error)
                    try:
                        workflow.call('scan')
                        workflow.call('create')
                        self.assertTrue(wait_terminal(workflow)['job']['state'].startswith('Succeeded'))
                        workflow.call('join')
                        original = output.read_bytes()
                        workflow.call('arm', site='read')
                        workflow.call('create')
                        deadline = time.monotonic() + 5
                        while not workflow.call('snapshot')['probe']['held']:
                            self.assertLess(time.monotonic(), deadline)
                            time.sleep(0.01)
                        workflow.call('cancel', timeout=2)
                        pending = workflow.call('snapshot')
                        self.assertEqual(pending['job']['state'], 'Cancelling')
                        self.assertEqual(pending['probe']['readers'], 1)
                        workflow.call('release')
                        self.assertEqual(wait_terminal(workflow)['job']['state'], 'Cancelled')
                        self.assertEqual(workflow.call('join')['probe']['readers'], 0)
                        self.assertEqual(output.read_bytes(), original)
                        workflow.call('create')
                        self.assertTrue(wait_terminal(workflow)['job']['state'].startswith('Succeeded'))
                        workflow.call('join')
                        self.assertEqual(output.read_bytes(), original)
                        self.assertEqual(workflow.call('clear')['remaining'], 0)
                        self.assertFalse(list(root.glob('*.tmp')) + list(root.glob('.*.tmp')))
                        workflow.finish()
                    finally:
                        if workflow.process.poll() is None:
                            workflow.process.kill()
                            workflow.process.wait(timeout=10)

    def test_delayed_open_failure_is_not_a_successful_replacement(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source, output = root / 'payload.bin', root / 'previous.torrent'
            source.write_bytes(b'x' * 1048576)
            output.write_bytes(b'previous output')
            with (root / 'stderr.log').open('w') as error:
                workflow = Workflow(Path(os.environ['TC_WORKFLOW_PROOF']).resolve(),
                                    {'root': str(source), 'output': str(output), 'format': 'hybrid', 'replace': True, 'probe': True}, error)
                try:
                    workflow.call('scan')
                    workflow.call('arm', site='open')
                    workflow.call('create')
                    deadline = time.monotonic() + 5
                    while not workflow.call('snapshot')['probe']['held']:
                        self.assertLess(time.monotonic(), deadline)
                        time.sleep(0.01)
                    source.unlink()
                    workflow.call('release')
                    result = wait_terminal(workflow)
                    self.assertEqual(result['job']['state'], 'Failed')
                    self.assertEqual(result['job']['error']['code'], 'SOURCE_MISSING')
                    self.assertEqual(workflow.call('join')['probe']['readers'], 0)
                    self.assertEqual(output.read_bytes(), b'previous output')
                    workflow.finish()
                finally:
                    if workflow.process.poll() is None:
                        workflow.process.kill()
                        workflow.process.wait(timeout=10)


if __name__ == '__main__':
    unittest.main()
