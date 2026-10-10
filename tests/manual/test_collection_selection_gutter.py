"""Selection geometry avoids the firearm/detail-hint region without relaxing rebind."""
import copy
import unittest
from collection_scroll import bind_card,validate_card_lease
from test_collection_scroll import packet,rule


class SelectionGutterTests(unittest.TestCase):
    def test_each_fresh_complete_card_gets_an_interior_gutter_point(self):
        p=packet()
        for raw in p['collection_layout']['cards']:
            if not all(raw['edges'].values()):continue
            lease=bind_card(p,rule(),raw['id']);x,y,w,h=lease['card']['body_bounds'];px,py=lease['point']
            self.assertTrue(x<=px<x+w and y<=py<y+h)
            self.assertLess(px,x+w//4);self.assertLess(py,y+h//3)
            self.assertEqual(lease['point_policy'],'observed_body_upper_left_gutter_v1')
            self.assertEqual(validate_card_lease(p,rule(),lease),lease)

    def test_old_center_or_shifted_point_is_not_accepted_as_new_lease(self):
        p=packet();card=p['collection_layout']['cards'][0]
        lease=bind_card(p,rule(),card['id']);x,y,w,h=lease['card']['body_bounds']
        for point in ([x+w//2,y+h//2],[lease['point'][0]+1,lease['point'][1]]):
            stale=copy.deepcopy(lease);stale['point']=point
            with self.assertRaisesRegex(ValueError,'LEASE_EXPIRED'):
                validate_card_lease(p,rule(),stale)

    def test_partial_card_remains_not_selectable(self):
        p=packet()
        partial=next(c for c in p['collection_layout']['cards'] if not all(c['edges'].values()))
        with self.assertRaisesRegex(ValueError,'PARTIAL_CARD_REQUIRES_SCROLL'):
            bind_card(p,rule(),partial['id'])


if __name__=='__main__':unittest.main()
