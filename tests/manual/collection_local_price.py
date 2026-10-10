"""Fixed local numeric provider over a measured same-frame price crop.

No expected amount, threshold, rule, or historical receipt is accepted by this
recognizer. Native component measurement isolates the ink, but never assigns
digits. The model supplies every character and its actual detector box. Raw
game images remain in memory; evidence contains hashes/boxes/timings only.
"""
import copy
import math
from numbers import Real
import re
import time

from collection_local_title import decode_region, require


def rectangle(value, error):
    require(isinstance(value, list) and len(value) == 4
            and all(type(v) is int for v in value)
            and min(value[:2]) >= 0 and min(value[2:]) > 0, error)
    return value


def contains(outer, inner):
    return (outer[0] <= inner[0] and outer[1] <= inner[1]
            and inner[0] + inner[2] <= outer[0] + outer[2]
            and inner[1] + inner[3] <= outer[1] + outer[3])


def measured_crop(packet, envelope):
    image, bounds = decode_region(packet, envelope,
        schema='collection-price-image-v1', kind='price')
    measure = envelope.get('numeric_preprocess', {})
    require(measure.get('schema') == 'numeric-roi-v1'
            and measure.get('same_frame') is True and not measure.get('error')
            and measure.get('frame_id') == envelope['frame_id']
            and measure.get('source_region') == bounds
            and measure.get('source_frame_bytes_unchanged') is True
            and measure.get('image_file_writes') == 0, 'LOCAL_PRICE_MEASUREMENT')
    crop = rectangle(measure.get('crop_bounds'), 'LOCAL_PRICE_CROP')
    ink = rectangle(measure.get('ink_bounds'), 'LOCAL_PRICE_INK')
    require(contains(bounds, crop) and contains(crop, ink), 'LOCAL_PRICE_CROP')
    count = measure.get('reliable_digit_span_minimum')
    spans = measure.get('reliable_digit_span_bounds')
    require(type(count) is int and 0 <= count <= 16 and isinstance(spans, list)
            and len(spans) == count, 'LOCAL_PRICE_SPANS')
    for span in spans:
        require(contains(ink, rectangle(span, 'LOCAL_PRICE_SPANS')), 'LOCAL_PRICE_SPANS')
    x, y = crop[0] - bounds[0], crop[1] - bounds[1]
    return image.crop((x, y, x + crop[2], y + crop[3])), crop, ink, count


def price_words(texts, scores, boxes, crop, ink, minimum, scale=3):
    require(len(texts) == len(scores) == len(boxes) and 1 <= len(texts) <= 16,
            'LOCAL_PRICE_EMPTY')
    words = []
    for text, score, points in zip(texts, scores, boxes):
        require(isinstance(text, str) and 0 < len(text) <= 32
                and re.fullmatch(r'[0-9]+(?:,[0-9]{3})*', text), 'LOCAL_PRICE_TEXT')
        require(type(score) in (int, float) and math.isfinite(score) and .99 <= score <= 1,
                'LOCAL_PRICE_CONFIDENCE')
        require(isinstance(points, list) and len(points) == 4
                and all(isinstance(p, list) and len(p) == 2 for p in points)
                and all(type(v) in (int, float) and math.isfinite(v) for p in points for v in p),
                'LOCAL_PRICE_BOX')
        xs, ys = [p[0] for p in points], [p[1] for p in points]
        require(0 <= min(xs) < max(xs) <= crop[2] * scale
                and 0 <= min(ys) < max(ys) <= crop[3] * scale, 'LOCAL_PRICE_BOX')
        words.append(dict(text=text, score=score, x=crop[0] + min(xs) / scale,
            y=crop[1] + min(ys) / scale, width=(max(xs) - min(xs)) / scale,
            height=(max(ys) - min(ys)) / scale))
    words.sort(key=lambda word: word['x'])
    for a, b in zip(words, words[1:]):
        require(-2 <= b['x'] - a['x'] - a['width'] <= 8
                and abs(b['y'] + b['height']/2 - a['y'] - a['height']/2) <= 3,
                'LOCAL_PRICE_TOKEN_LAYOUT')
    require(sum(sum('0' <= ch <= '9' for ch in w['text']) for w in words) >= minimum,
            'LOCAL_PRICE_DIGITS_MISSING')
    require(min(w['x'] for w in words) <= ink[0] + 3
            and max(w['x'] + w['width'] for w in words) >= ink[0] + ink[2] - 3
            and min(w['y'] for w in words) <= ink[1] + 3
            and max(w['y'] + w['height'] for w in words) >= ink[1] + ink[3] - 3,
            'LOCAL_PRICE_INCOMPLETE')
    return words


class LocalPriceRecognizer:
    def __init__(self, title_recognizer):
        # Reuse the fixed, hash-verified model already loaded before the run.
        self.model = title_recognizer

    def apply(self, packet):
        envelope = packet.get('collection_price_image')
        regions = packet.get('collection_observation', {}).get('regions', [])
        pending = [r for r in regions if r.get('kind') == 'card_fields'
                   and any(a.get('error') == 'E_LOCAL_PRICE_PENDING'
                           for a in r.get('field_attempts', []))]
        if envelope is None:
            require(not pending, 'LOCAL_PRICE_IMAGE_MISSING')
            return
        selected = packet.get('collection_selected_card', {})
        targets = [r for r in regions if r.get('kind') == 'card_fields'
                   and r.get('card_id') == envelope.get('card_id')]
        require(len(targets) == 1, 'LOCAL_PRICE_REGION_COUNT')
        region = targets[0]
        original = copy.deepcopy(region)
        metadata = dict(provider='RapidOCR3.9.2/PP-OCRv6-local-CPU',
            model_sha256=self.model.model_hashes, expected_value_supplied=False,
            provider_selection='explicit_before_run_not_price_match',
            confidence_threshold=.99, image_file_writes=0,
            source_frame_id=envelope.get('frame_id'), source_frame_sha256=envelope.get('frame_sha256'),
            source_png_sha256=envelope.get('png_sha256'), native_observation=original)
        started = time.perf_counter()
        try:
            require(selected.get('selected') is True and selected.get('same_frame') is True
                    and selected.get('card_id') == envelope.get('card_id')
                    and selected.get('frame_id') == envelope.get('frame_id')
                    and selected.get('frame_sha256') == envelope.get('frame_sha256'),
                    'LOCAL_PRICE_SELECTED_BINDING')
            layout = packet.get('collection_layout', {})
            require(layout.get('frame_id') == envelope.get('frame_id')
                    and layout.get('frame_sha256') == envelope.get('frame_sha256')
                    and layout.get('same_frame') is True, 'LOCAL_PRICE_LAYOUT_BINDING')
            cards = (layout.get('cards', []) if layout.get('complete') is True else
                     [layout.get('selected_card_proof', {}).get('card', {})])
            cards = [c for c in cards if c.get('id') == envelope.get('card_id')]
            require(len(cards) == 1 and cards[0].get('selected') is True
                    and cards[0].get('price_bounds') == envelope.get('bounds')
                    and cards[0].get('fields_bounds') == region.get('bounds')
                    and region.get('truncated') is False, 'LOCAL_PRICE_CARD_BINDING')
            condition_attempts = [a for a in region.get('field_attempts', [])
                                  if a.get('field') == 'condition_bounds']
            require(len(condition_attempts) == 1 and condition_attempts[0].get('ok') is True
                    and condition_attempts[0].get('same_frame') is True
                    and condition_attempts[0].get('frame_id') == envelope.get('frame_id'),
                    'LOCAL_PRICE_CONDITION_UNPROVEN')
            require(region.get('error') == 'E_LOCAL_PRICE_PENDING', 'LOCAL_PRICE_PROVIDER_NOT_SELECTED')
            image, crop, ink, minimum = measured_crop(packet, envelope)
            scale = 3
            from PIL import Image
            image = image.resize((image.width * scale, image.height * scale), Image.Resampling.LANCZOS)
            if self.model.injected:
                result = self.model.engine(image)
            else:
                import numpy as np
                result = self.model.engine(np.array(image), use_cls=False)
            texts = list(result.txts) if result.txts is not None else []
            raw_scores = list(result.scores) if result.scores is not None else []
            require(all(isinstance(s, Real) and not isinstance(s, bool) and math.isfinite(s)
                        for s in raw_scores), 'LOCAL_PRICE_CONFIDENCE_TYPE')
            scores = [float(s) for s in raw_scores]
            boxes = result.boxes.tolist() if hasattr(result.boxes, 'tolist') else result.boxes or []
            metadata.update(raw_texts=texts, scores=scores, detector_boxes=boxes,
                            crop_bounds=crop, ink_bounds=ink, scale=scale,
                            reliable_digit_span_minimum=minimum)
            words = price_words(texts, scores, boxes, crop, ink, minimum, scale)
            # Condition OCR remains independently verified. Never retain native
            # price letters or borrow a digit from the previous observation.
            bounds = cards[0]['condition_bounds']
            condition_words = [w for w in original.get('words', [])
                if all(type(w.get(k)) in (int, float) and math.isfinite(w[k])
                       for k in ('x', 'y', 'width', 'height'))
                and contains(bounds, [w['x'], w['y'], w['width'], w['height']])]
            require(condition_words and len(condition_words) == len(original.get('words', [])),
                    'LOCAL_PRICE_UNEXPECTED_NATIVE_WORDS')
            region.update(ok=True, error='', words=condition_words + words,
                          price_provider=metadata['provider'])
            metadata.update(ok=True, words=words)
        except Exception as error:
            metadata.update(ok=False, error=str(error))
            region.update(ok=False, error=str(error))
            raise
        finally:
            metadata['elapsed_ms'] = (time.perf_counter() - started) * 1000
            packet['local_price_ocr'] = metadata
