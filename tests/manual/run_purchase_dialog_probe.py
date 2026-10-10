"""Open the purchase confirmation dialog once and cancel it; never buys.

User 2026-10-09: clicking the price button opens a confirmation dialog; this
probe may open it and must cancel ("可以测，点取消就行"). One lease:

1. navigate to 我的关注 (same allowlisted route as run_purchase_probe);
2. read the selected listing: it must match a configured rule, its public
   notice must be over, and the footer must show exactly one price button
   whose text equals the selected listing's price;
3. click that price button ONCE (the only purchase-area input);
4. read the dialog (one frame with a local preview image);
5. close it with Esc; only if the dialog is still there, click its single
   OCR-read 取消. 确认 is never clicked and the price button never again;
6. read again: back on the watchlist with no dialog text.

Importing this module is inert; it sends input only from main().
"""
import argparse
import base64
import json
from pathlib import Path

from collection_paths import project_root
from collection_run_config import snapshot_from_files
from purchase_observation import match_current_watchlist, normalize, read_screen
from run_purchase_probe import allowed_step, navigate, return_policy, sample_step, RETURN_POLICIES

VIEWPORT = [2560, 1440]
# Any of these means a purchase-area dialog is open. The insufficient-balance
# dialog's second button is 充值 (real-money top-up): never clicked; this
# probe closes every dialog with Esc and, only if that fails, its 取消.
DIALOG_KINDS = ('confirm_dialog', 'purchase_dialog', 'insufficient_balance', 'recharge_prompt')
ESCAPE = 0x1B


def plan_buy_click(observed):
    """The single price-button point, or ValueError naming why not."""
    screen = observed['screen']
    if screen['page'] != 'watchlist_listings' or screen['overlay'] != 'none':
        raise ValueError('PURCHASE_DIALOG_PAGE')
    if observed['decision'] != 'Match' or len(observed['candidates']) != 1:
        raise ValueError('PURCHASE_DIALOG_NO_SINGLE_MATCH:' + str(observed['decision']))
    if screen['countdown_evidence'] or screen['countdown_unread'] or screen['countdown_seconds'] is not None:
        raise ValueError('PURCHASE_DIALOG_PUBLICITY_COUNTDOWN')
    if screen['button_state'] != 'price_button' or not screen.get('price_button'):
        raise ValueError('PURCHASE_DIALOG_BUTTON_STATE:' + str(screen['button_state']))
    if screen['signals']:
        raise ValueError('PURCHASE_DIALOG_ALREADY_OPEN')
    candidate = observed['candidates'][0]
    button = screen['price_button']
    # The button's red price is often unreadable; when it does read as a
    # number it must be the selected listing's price. A real purchase must
    # also prove the price inside the confirmation dialog.
    if button['price'] is not None and button['price'] != candidate['price']:
        raise ValueError('PURCHASE_DIALOG_PRICE_MISMATCH')
    x, y, width, height = button['bounds']
    point = [int(round(x + width / 2)), int(round(y + height / 2))]
    # The footer region (1900..2450 x 1130..1305) contains the button.
    if not (1900 <= point[0] <= 2450 and 1130 <= point[1] <= 1305):
        raise ValueError('PURCHASE_DIALOG_BUTTON_BOUNDS')
    return point, dict(product=candidate['product'], condition=candidate['condition'], price=candidate['price'],
                       wear=candidate['wear'], row_index=candidate['row_index'], button=button)


def step_permitted(step, buy):
    """Session steps this probe may send: reads, the allowlisted navigation,
    and the planned price-button click exactly once. Raises otherwise."""
    purchase_click = (step.get('kind') == 'click' and step.get('expected_before') == 'watchlist_listings'
                      and buy['point'] is not None and step.get('point') == buy['point'] and not buy['used'])
    if not (allowed_step(step) or purchase_click):
        raise ValueError('PURCHASE_DIALOG_STEP_NOT_ALLOWED')
    if purchase_click:
        buy['used'] = True  # never a second press, whatever happens next
    return purchase_click


def cancel_target(screen):
    """The dialog's single 取消, as text evidence, or None."""
    hits = []
    for line in screen['regions']['dialog_text']:
        for word in line['words']:
            if normalize(word['text']) == '取消':
                hits.append(word)
    if len(hits) != 1:
        return None
    w = hits[0]
    return [int(round(w['x'] + w['width'] / 2)), int(round(w['y'] + w['height'] / 2))]


def dialog_open(screen):
    return any(signal['kind'] in DIALOG_KINDS for signal in screen['signals'])


def dialog_closed(screen):
    """No dialog text and a page the classifier recognises again."""
    return not dialog_open(screen) and screen['page'] != 'unknown' and screen['overlay'] == 'none'


def dialog_texts(screen):
    return {kind: [line['text'] for line in lines] for kind, lines in screen['regions'].items()}


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--config', type=Path, default=Path.home() / 'AppData/Local/RelinkStudio/RelinkStudio/config.json')
    p.add_argument('--return-to', choices=sorted(RETURN_POLICIES), default='entry')
    args = p.parse_args()
    root = project_root(__file__)
    output = args.output.resolve()
    if not output.is_relative_to(root / 'artifacts'):
        raise ValueError('PURCHASE_DIALOG_OUTPUT_DIRECTORY')
    snapshot = snapshot_from_files(args.config, root / 'dist/RelinkStudio/catalog/skins.json')
    output.mkdir(parents=True, exist_ok=False)
    from collection_live_session import ForegroundSession
    from run_collection_observed import MemoryReviewBackend
    buy = dict(point=None, used=False)

    class DialogProbeSession(ForegroundSession):
        def perform(self, step, remaining=None):
            step_permitted(step, buy)
            return super().perform(step, remaining)

    backend = MemoryReviewBackend(root, local_title=True, local_price=True, fast_capture=True, local_hotpath=False,
                                  ready_stream=False, fast_actions=False, parallel_local_price=True,
                                  return_policy=return_policy(args.return_to))
    session = DialogProbeSession(output / 'session.json', backend=backend, timeout_seconds=60, max_steps=24,
                                 numeric_price=True, fast_settle=False)
    result = dict(mode='purchase_dialog_open_and_cancel', price_button_clicks=0, confirm_clicks=0,
                  cancel_method=None, status='blocked')

    def read(name, **extra):
        session.perform(dict(kind='capture', collection_observation=True, ui_regions=True,
                             purchase_observation=True, preview=True, **extra))
        packet = session.previous
        if session.last_preview:
            (output / (name + '.png')).write_bytes(base64.b64decode(session.last_preview))
        screen = read_screen(packet)
        result[name] = dict(page=screen['page'], overlay=screen['overlay'], button_state=screen['button_state'],
                            signals=screen['signals'], texts=dialog_texts(screen))
        return screen

    try:
        with session:
            page = navigate(session)
            if page != 'watchlist_listings':
                raise ValueError('PURCHASE_DIALOG_WATCHLIST_EMPTY')
            session.perform(sample_step(page))
            observed = match_current_watchlist(session.previous, snapshot)
            point, listing = plan_buy_click(observed)
            result.update(listing=listing, buy_point=point)
            buy['point'] = point
            session.perform(dict(kind='click', point=point, viewport=VIEWPORT, expected_before='watchlist_listings'))
            result['price_button_clicks'] = 1
            try:
                dialog = read('dialog')
                result['dialog_seen'] = dialog_open(dialog)
            except Exception as error:
                result['dialog_read_error'] = str(error) or type(error).__name__
            finally:
                # Whatever the read showed, close without confirming. Esc never
                # confirms; the dialog may not be a classified page, so the
                # backend's guarded key (foreground, idle input) is used.
                backend.key(ESCAPE)
                result['cancel_method'] = 'escape'
            session.wait(.6)
            after = read('after_escape')
            if not dialog_closed(after):
                target = cancel_target(after)
                if target is None:
                    raise ValueError('PURCHASE_DIALOG_CANCEL_NOT_FOUND')
                backend.click(target)
                result['cancel_method'] = 'cancel_button'
                result['cancel_point'] = target
                session.wait(.6)
                after = read('after_cancel')
                if not dialog_closed(after):
                    raise ValueError('PURCHASE_DIALOG_STILL_OPEN')
            result['status'] = 'passed'
    except Exception as error:
        result['error'] = str(error) or type(error).__name__
    result.update(ide_restored=session.report.get('ide_restored'), config_sha256=snapshot['config_sha256'])
    path = output / 'result.json'
    path.write_text(json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False) + '\n', encoding='utf-8')
    print(json.dumps({k: v for k, v in result.items() if k not in ('dialog', 'after_escape', 'after_cancel')},
                     ensure_ascii=False))
    return 0 if result['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
