"""Title crop envelopes stay in memory, including failed capture packets."""
import json
from pathlib import Path
import tempfile
import unittest

from collection_live_session import ForegroundSession
from test_collection_live_session import Clock, FakeBackend, capture, packet


class TitleMemoryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.clock = Clock()
        self.backend = FakeBackend(self.clock)
        self.envelope = dict(schema='collection-title-image-v1', frame_id='fake:a', frame_sha256='a' * 64,
                             bounds=[1970,240,580,50], base64='TITLE_IMAGE_BASE64_MUST_STAY_IN_MEMORY')

    def session(self):
        return ForegroundSession('artifacts/title/session.json', root=self.root, backend=self.backend,
                                 now=self.clock.now, wait=self.clock.wait)

    def assert_no_image_persistence(self, session):
        self.assertNotIn('TITLE_IMAGE_BASE64_MUST_STAY_IN_MEMORY', json.dumps(session.report))
        for path in (self.root / 'artifacts').rglob('*'):
            if path.is_file():
                self.assertEqual(path.suffix, '.json')
                self.assertNotIn('TITLE_IMAGE_BASE64_MUST_STAY_IN_MEMORY', path.read_text('utf-8'))

    def test_title_option_is_explicit_and_crop_is_removed_before_persistence(self):
        value = packet()
        value['collection_title_image'] = self.envelope
        self.backend.replies = [value, packet('b')]
        session = self.session()
        with session:
            observed = session.perform(capture(collection_title_image=True))
            self.assertIn('--collection-title-image', observed['command'])
            self.assertEqual(session.last_title_image, self.envelope)
            self.assertNotIn('collection_title_image', session.previous)
            self.assertNotIn('collection_title_image', observed['result'])
            self.assert_no_image_persistence(session)
            second = session.perform(capture())
            self.assertNotIn('--collection-title-image', second['command'])
            self.assertIsNone(session.last_title_image)
        self.assert_no_image_persistence(session)

    def test_failed_capture_also_removes_title_envelope_before_report(self):
        value = packet()
        value.update(collection_title_image=self.envelope, ocr_passed=False)
        self.backend.replies = [value]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
            with session:
                session.perform(capture(collection_title_image=True, attempts=1))
        self.assertEqual(session.last_title_image, self.envelope)
        self.assertNotIn('collection_title_image', session.steps[-1]['result'])
        self.assertTrue(session.report['ide_restored'])
        self.assert_no_image_persistence(session)

    def test_new_frame_without_title_crop_clears_previous_crop_even_on_failure(self):
        first = packet()
        first['collection_title_image'] = self.envelope
        failed = packet('b')
        failed['ocr_passed'] = False
        self.backend.replies = [first, failed]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
            with session:
                session.perform(capture(collection_title_image=True))
                session.perform(capture(attempts=1))
        self.assertIsNone(session.last_title_image)
        self.assert_no_image_persistence(session)

    def test_preview_disabled_after_review_clears_old_frame(self):
        first = packet()
        first['preview_png_base64'] = 'OLD_PREVIEW_MUST_NOT_BE_PERSISTED'
        self.backend.replies = [first, packet('b')]
        session = self.session()
        with session:
            session.perform(capture(preview=True))
            self.assertEqual(session.last_preview, 'OLD_PREVIEW_MUST_NOT_BE_PERSISTED')
            session.perform(capture())
            self.assertIsNone(session.last_preview)
        for path in (self.root / 'artifacts').rglob('*.json'):
            self.assertNotIn('OLD_PREVIEW_MUST_NOT_BE_PERSISTED', path.read_text('utf-8'))


if __name__ == '__main__':
    unittest.main()
