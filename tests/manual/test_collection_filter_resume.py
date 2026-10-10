"""Resume known listing filters without replaying any favorite input."""
import unittest

from run_collection_trial import CollectionTrial
from test_collection_trial import FakeSession, listing, snapshot


class FilterResumeTests(unittest.TestCase):
    def session(self, first='listing_filter', after='none'):
        session = FakeSession()
        replies = [first, after]
        def perform(step):
            session.steps.append(step)
            if step['kind'] == 'capture':
                session.previous = listing(serial=len(session.steps))
                session.previous['startup_page']['overlay'] = replies.pop(0)
            elif step['kind'] == 'click':
                self.assertEqual(session.previous['startup_page']['overlay'], 'listing_filter')
                self.assertEqual(step['expected_overlay'], 'listing_filter')
                self.assertEqual(step['point'], [2340, 1355])
                session.previous = {}
            else:
                self.fail(str(step))
            return {'passed': True}
        session.perform = perform
        return session

    def test_close_known_filter_once_then_observe_closed(self):
        session = self.session()
        events = []
        trial = CollectionTrial(session, snapshot(), 'artifacts/test.json', emit=events.append)
        trial.startup()
        self.assertEqual([s['kind'] for s in session.steps], ['capture', 'click', 'capture'])
        self.assertEqual(session.previous['startup_page']['overlay'], 'none')
        self.assertTrue(any(e['event'] == 'startup_listing_filter_closed' for e in events))
        self.assertEqual(trial.summary['rows'], [])
        self.assertEqual(trial.summary['confirmed_new'], 0)

    def test_unchanged_filter_stops_without_second_click(self):
        session = self.session(after='listing_filter')
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_STARTUP_FILTER_NOT_CLOSED'):
            CollectionTrial(session, snapshot(), 'artifacts/test.json').startup()
        self.assertEqual(sum(s['kind'] == 'click' for s in session.steps), 1)

    def test_other_overlay_never_clicked(self):
        session = self.session(first='result_dialog')
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_STARTUP_UNEXPECTED_OVERLAY'):
            CollectionTrial(session, snapshot(), 'artifacts/test.json').startup()
        self.assertEqual([s['kind'] for s in session.steps], ['capture'])


if __name__ == '__main__':
    unittest.main()
