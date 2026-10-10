"""Bounded thumb-edge changes prove motion only, not stable content membership."""
import copy
import unittest

from collection_scroll import bind_card, observe_layout, rebind_after_scroll, scroll_plan, validate_card_lease
from test_collection_scroll import packet, rule, scrolled


def pair(old_top=400, old_height=62, new_top=407, new_height=61, delta=-120):
    before = packet()
    before['collection_layout']['scrollbar'] = dict(track_bounds=[1878,304,1,899],
                                                   thumb_bounds=[1878,old_top,1,old_height])
    after = scrolled(before)
    after['collection_layout']['scrollbar']['thumb_bounds'] = [1878,new_top,1,new_height]
    lease = scroll_plan(before, rule(), [], delta=delta)['steps'][0]['lease']
    return before, after, lease


class ThumbEndpointTests(unittest.TestCase):
    def test_real_run05_thumb_numbers_prove_both_endpoint_progress_only(self):
        before, after, lease = pair(316, 62, 323, 61)
        original = copy.deepcopy((before, after, lease))
        result = rebind_after_scroll(after, rule(), lease)
        self.assertEqual(result['scrollbar_shift_pixels'], 7)
        self.assertEqual(result['scrollbar_bottom_shift_pixels'], 6)
        self.assertEqual(result['height_delta'], -1)
        self.assertEqual(result['height_delta_cause'], 'unproven')
        self.assertTrue(result['content_membership_not_proven'])
        self.assertFalse(result['old_card_coordinates_reused'])
        self.assertFalse(result['collection_allowed'])
        self.assertFalse(result['page_exhausted'])
        self.assertEqual((before, after, lease), original)

    def test_up_and_down_allow_one_pixel_growth_or_shrink_with_both_ends_moving(self):
        for delta, shift in ((-120,7), (120,-7)):
            for height_delta in (-1,0,1):
                before, after, lease = pair(new_top=400+shift,new_height=62+height_delta,delta=delta)
                result = rebind_after_scroll(after, rule(), lease)
                with self.subTest(delta=delta,height_delta=height_delta):
                    self.assertEqual(result['scrollbar_shift_pixels'], shift)
                    self.assertEqual(result['scrollbar_bottom_shift_pixels'], shift+height_delta)
                    self.assertEqual(result['height_delta'], height_delta)
                    self.assertTrue(result['requires_selected_reobservation'])

    def test_exact_two_pixel_progress_requires_both_endpoints(self):
        for delta, shift, height_delta in ((-120,2,0),(-120,2,1),(120,-2,0),(120,-2,-1)):
            before, after, lease = pair(new_top=400+shift,new_height=62+height_delta,delta=delta)
            with self.subTest(delta=delta,height_delta=height_delta):
                self.assertEqual(rebind_after_scroll(after,rule(),lease)['height_delta'],height_delta)

    def test_one_stationary_or_too_small_endpoint_is_not_progress(self):
        for delta, shift, height_delta in ((-120,0,1),(-120,1,-1),(-120,1,1),(-120,2,-1),
                                          (120,0,-1),(120,-1,1),(120,-1,-1),(120,-2,1)):
            before, after, lease = pair(new_top=400+shift,new_height=62+height_delta,delta=delta)
            with self.subTest(delta=delta,shift=shift,height_delta=height_delta), \
                    self.assertRaisesRegex(ValueError,'COLLECTION_SCROLL_PROGRESS_UNPROVEN'):
                rebind_after_scroll(after,rule(),lease)

    def test_height_change_over_one_pixel_remains_rejected(self):
        for height_delta in (-3,-2,2,3):
            before, after, lease = pair(new_height=62+height_delta)
            with self.subTest(height_delta=height_delta), self.assertRaisesRegex(ValueError,'CONTENT_CHANGED'):
                rebind_after_scroll(after,rule(),lease)

    def test_reverse_direction_remains_rejected(self):
        for delta, shift in ((-120,-7),(120,7)):
            before, after, lease = pair(new_top=400+shift,delta=delta)
            with self.subTest(delta=delta), self.assertRaisesRegex(ValueError,'DIRECTION_CONFLICT'):
                rebind_after_scroll(after,rule(),lease)

    def test_x_width_and_track_geometry_remain_strict(self):
        for changed in ('x','width','track'):
            before, after, lease = pair()
            bar = after['collection_layout']['scrollbar']
            if changed == 'x':
                # A wider common track keeps both legal; only thumb x changes.
                before['collection_layout']['scrollbar']['track_bounds'][2] = 3
                bar['track_bounds'][2] = 3
                lease = scroll_plan(before,rule(),[])['steps'][0]['lease']
                bar['thumb_bounds'][0] += 1
            elif changed == 'width':
                before['collection_layout']['scrollbar']['track_bounds'][2] = 3
                bar['track_bounds'][2] = 3
                lease = scroll_plan(before,rule(),[])['steps'][0]['lease']
                bar['thumb_bounds'][2] += 1
            else:
                bar['track_bounds'][3] -= 1
            with self.subTest(changed=changed), self.assertRaises(ValueError):
                rebind_after_scroll(after,rule(),lease)

    def test_old_card_lease_is_not_carried_to_new_geometry_or_fields(self):
        before, after, lease = pair()
        old_card = bind_card(before,rule(),'c0')
        result = rebind_after_scroll(after,rule(),lease)
        self.assertEqual(result['layout'],observe_layout(after))
        self.assertEqual(result['layout']['frame_id'],after['collection_observation']['frame_id'])
        self.assertEqual(result['selectable_card_ids'],['new-l','new-r','new-bottom-l','new-bottom-r'])
        new_card = bind_card(after,rule(),'new-l')
        self.assertNotEqual(new_card['point'],old_card['point'])
        self.assertNotEqual(new_card['card']['fields_bounds'],old_card['card']['fields_bounds'])
        with self.assertRaisesRegex(ValueError,'COLLECTION_LAYOUT_CARD_MISSING'):
            validate_card_lease(after,rule(),old_card)

    def test_one_pixel_thumb_change_does_not_accept_incomplete_or_stale_frame(self):
        for changed in ('layout','frame_id','hash'):
            before, after, lease = pair()
            if changed == 'layout':after['collection_layout']['complete'] = False
            elif changed == 'frame_id':
                for section in ('collection_layout','collection_observation'):
                    after[section]['frame_id'] = before[section]['frame_id']
            else:
                for section in ('collection_layout','collection_observation'):
                    after[section]['frame_sha256'] = before[section]['frame_sha256']
                after['frames'][-1]['sha256'] = before['frames'][-1]['sha256']
            with self.subTest(changed=changed), self.assertRaises(ValueError):
                rebind_after_scroll(after,rule(),lease)


if __name__ == '__main__':
    unittest.main()
