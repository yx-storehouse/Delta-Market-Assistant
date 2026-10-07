"""Verify saved live evidence without capturing any new game frame."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TX = ROOT / 'artifacts/m2_lobby_calibration'


def load(name):
    record = json.loads((TX / name).read_text(encoding='utf-8'))
    data = json.loads(record['stdout']) if 'stdout' in record else record['result']
    assert data['image_file_writes'] == 0 and not data['game_input_sent']
    assert data['ide_foreground_restored'] and data['game_in_background']
    assert data['resources_drained'] and 'preview_png_base64' not in data
    for frame in data['frames']:
        assert (frame['width'], frame['height'], frame['dpi']) == (2560, 1440, 144)
        assert not frame['near_black'] and frame['source'] == 'dxgi'
    return record, data


def main():
    # Later focus-policy fixes produce a newer release. Preserve the exact binary
    # that produced these lobby results rather than relabeling old runs as new.
    modified = hashlib.sha256((TX / 'LOBBY_CALIBRATED_FILE.exe').read_bytes()).hexdigest()
    baseline = hashlib.sha256((TX / 'baseline/release/RelinkStudio.exe').read_bytes()).hexdigest()
    original, partial = load('live_baseline.json')
    assert original['binary_sha256'] == baseline
    assert original['exit_status'] == 1 and partial['error'] == 'E_WINDOW_NOT_FOREGROUND'
    assert len(partial['frames']) == 2 and not partial['recognition_performed']
    for name in ['live_single_frame.json', 'live_preview_metadata.json', 'live_anchor_diagnostics.json']:
        record, result = load(name)
        assert record['exit_status'] == 0 and result['capture_passed'] and result['ocr_passed']
        assert result['startup_page']['page'] == 'unknown'
    frames = set()
    for name in ['live_modified_lobby_1.json', 'live_modified_lobby_2.json']:
        record, result = load(name)
        assert record['binary_sha256'] == modified and record['exit_status'] == 0
        assert record['outer_ide_restored'] and str(record['foreground_after']) == result['return_hwnd']
        assert result['capture_passed'] and result['ocr_passed'] and result['page_match_passed']
        assert result['expected_page'] == result['startup_page']['page'] == 'lobby'
        assert result['startup_page']['overlay'] == 'none' and not result['startup_page']['actions_enabled']
        assert result['startup_page']['calibration'] == 'lobby_live_sample_20261007'
        assert result['frames'][0]['sha256'] == result['ocr']['frame_sha256']
        frames.add(result['frames'][0]['sha256'])
    assert len(frames) == 2, 'Independent positive captures must not reuse a recorded frame.'
    negative, mismatch = load('live_expected_mismatch.json')
    assert negative['binary_sha256'] == modified and negative['exit_status'] == 1
    assert negative['outer_ide_restored'] and mismatch['capture_passed'] and mismatch['ocr_passed']
    assert mismatch['expected_page'] == 'warehouse' and mismatch['startup_page']['page'] == 'lobby'
    assert mismatch['page_error'] == 'E_DIAGNOSTIC_PAGE_MISMATCH' and not mismatch['page_match_passed']
    fixture = json.loads((ROOT / 'docs/business_rebuild/implementation/runtime/fixtures/lobby_live_20261007.json').read_text(encoding='utf-8'))
    source, captured = load('live_anchor_diagnostics.json')
    assert fixture['source_binary_sha256'] == source['binary_sha256']
    assert fixture['source_frame_sha256'] == captured['frames'][0]['sha256']
    words = [word for region in captured['lobby_anchor_diagnostics']['regions'] for word in region['words']]
    assert words == fixture['observation']['words'] and len(words) == 10
    result = dict(passed=True, positive_live_checks=2, expected_negative_checks=1,
        initial_foreground_failure_retained=True, initial_unknown_results_retained=True,
        recorded_frames_independent=True, source_fixture_equal=True,
        lobby_calibrated_sha256=modified, baseline_sha256=baseline,
        new_capture_performed=False, image_file_writes=0)
    output = TX / 'live_evidence_verification.json'
    output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    assert json.loads(output.read_text(encoding='utf-8')) == result
    print('LOBBY_LIVE_EVIDENCE=PASS; positive=2; expected_mismatch=1; original_failures_retained=true; new_capture=false')


if __name__ == '__main__':
    main()
