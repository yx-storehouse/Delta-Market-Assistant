"""Startup navigation never needs two empty-count proofs for one Back click."""
import copy
import json
from pathlib import Path
import unittest

from run_collection_trial import CollectionTrial
from test_collection_trial import FakeSession, snapshot


class StartupSingleReadTests(unittest.TestCase):
    def run_startup(self, pages):
        session, events = FakeSession(), []
        replies = list(pages)

        def perform(step):
            session.steps.append(copy.deepcopy(step))
            if step['kind'] == 'capture':
                page = replies.pop(0)
                if step.get('expected_page'):
                    self.assertEqual(page, step['expected_page'])
                session.previous = dict(startup_page=dict(page=page, overlay='none'),
                                        frames=[dict(sha256='a' * 64)])
            elif step['kind'] == 'click' or step['kind'] == 'key' and step['key'] == 'escape':
                self.assertEqual(step['expected_before'], session.previous['startup_page']['page'])
                session.previous = {}
            else:
                self.fail('Startup sent non-navigation input: ' + step['kind'])
            return dict(passed=True)

        session.perform = perform
        CollectionTrial(session, snapshot(), 'artifacts/test.json', emit=events.append).startup()
        self.assertEqual(replies, [])
        return session, events

    def test_home_empty_back_uses_one_empty_read_and_fresh_home(self):
        s, events = self.run_startup(['skin_home', 'empty_watchlist', 'skin_home'])
        self.assertEqual([x['kind'] for x in s.steps], ['capture', 'click', 'capture', 'key', 'capture'])
        # Watchlist -> home by the game's Back key, not the on-screen button.
        self.assertEqual([x['point'] for x in s.steps if x['kind'] == 'click'], [[2310, 274]])
        self.assertEqual(s.steps[3]['key'], 'escape')
        self.assertEqual(s.previous['startup_page']['page'], 'skin_home')
        event = next(e for e in events if e['event'] == 'watchlist_empty_observed')
        self.assertEqual(event['confirmation_reads'], 1)
        self.assertFalse(event['purchase_phase_enabled'])
        self.assertFalse(any(e['event'] == 'watchlist_empty_confirmed' for e in events))

    def test_starting_on_empty_page_returns_without_count_recheck(self):
        s, _ = self.run_startup(['empty_watchlist', 'skin_home'])
        self.assertEqual([x['kind'] for x in s.steps], ['capture', 'key', 'capture'])
        self.assertEqual(s.steps[1]['expected_before'], 'empty_watchlist')

    def test_populated_watchlist_is_preserved_and_not_purchased(self):
        s, events = self.run_startup(['skin_home', 'watchlist_listings', 'skin_home'])
        self.assertEqual(len(s.steps), 5)
        self.assertTrue(any(e['event'] == 'existing_favorites_preserved' for e in events))
        self.assertFalse(any(e['event'] == 'watchlist_empty_observed' for e in events))

    def test_unknown_transition_gets_fresh_observation_not_unchecked_back(self):
        s, _ = self.run_startup(['skin_home', 'unknown', 'empty_watchlist', 'skin_home'])
        self.assertEqual([x['kind'] for x in s.steps], ['capture', 'click', 'capture', 'capture', 'key', 'capture'])
        self.assertEqual(s.steps[-2]['expected_before'], 'empty_watchlist')


if __name__ == '__main__':
    unittest.main()
