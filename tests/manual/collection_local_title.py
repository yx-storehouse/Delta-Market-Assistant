"""Local same-frame title/catalog OCR. No expected product/config is accepted.

Only a raw title ROI explicitly emitted by the native capture may be read.
Catalog/home text uses actual detector boxes. The known single-line listing
title may use the whole freshly observed ROI as its explicitly marked bounds,
without rerunning a general text detector. Provider/route selection never uses
which text matches a configured skin.
"""
import base64
import copy
import hashlib
import io
import math
import time
from pathlib import Path

MODELS = {
    'PP-OCRv6_det_small.onnx': '090f04abcd9d9a7498bc4ebf677e4cb9bdce1fe4197ddb7e529f1ef44e1ff94f',
    'PP-OCRv6_rec_small.onnx': '6f327246b50388f3c176ae304bd95767ea6dc0c9ae92153ef8cbe210b3c14884',
    'ch_ppocr_mobile_v2.0_cls_mobile.onnx': 'e47acedf663230f8863ff1ab0e64dd2d82b838fceb5957146dab185a89d6215c',
}


def require(value, code):
    if not value:
        raise ValueError(code)


def decode_region(packet, envelope, *, schema, kind):
    from PIL import Image
    observation = packet.get('collection_observation', {})
    frames = packet.get('frames', [])
    require(isinstance(envelope, dict) and envelope.get('schema') == schema
            and envelope.get('kind') == kind and envelope.get('raw_roi') is True
            and envelope.get('same_frame') is True and envelope.get('image_file_writes') == 0
            and envelope.get('coordinate_space') == 'client_physical_px', 'LOCAL_TITLE_ENVELOPE')
    frame_id, digest = envelope.get('frame_id'), envelope.get('frame_sha256')
    require(isinstance(frame_id, str) and bool(frame_id) and isinstance(digest, str)
            and len(digest) == 64 and all(char in '0123456789abcdef' for char in digest),
            'LOCAL_TITLE_FRAME_BINDING')
    require(frames and observation.get('same_frame') is True
            and envelope.get('frame_id') == observation.get('frame_id')
            and envelope.get('frame_sha256') == observation.get('frame_sha256') == frames[-1].get('sha256'),
            'LOCAL_TITLE_FRAME_BINDING')
    box = envelope.get('bounds')
    require(isinstance(box, list) and len(box) == 4 and all(type(x) is int for x in box)
            and min(box[:2]) >= 0 and min(box[2:]) > 0
            and box[0] + box[2] <= frames[-1].get('width', 0)
            and box[1] + box[3] <= frames[-1].get('height', 0)
            and box[2] * box[3] <= 1024 * 1024, 'LOCAL_TITLE_BOUNDS')
    encoded = envelope.get('png_base64')
    require(isinstance(encoded, str) and 0 < len(encoded) <= 4 * 1024 * 1024, 'LOCAL_TITLE_IMAGE_SIZE')
    raw = base64.b64decode(encoded, validate=True)
    require(hashlib.sha256(raw).hexdigest() == envelope.get('png_sha256'), 'LOCAL_TITLE_IMAGE_HASH')
    require(raw.startswith(b'\x89PNG\r\n\x1a\n'), 'LOCAL_TITLE_IMAGE_FORMAT')
    image = Image.open(io.BytesIO(raw))
    require(list(image.size) == box[2:], 'LOCAL_TITLE_IMAGE_DIMENSIONS')
    return image.convert('RGB'), box


def decode_title(packet, envelope):
    return decode_region(packet, envelope, schema='collection-title-image-v1', kind='product_title')


def mapped_words(texts, scores, boxes, bounds):
    require(len(texts) == len(scores) == len(boxes) and len(texts) <= 32, 'LOCAL_TITLE_OUTPUT')
    words, rejected = [], []
    for text, score, points in zip(texts, scores, boxes):
        require(isinstance(text, str) and 0 < len(text) <= 128 and isinstance(score, (float, int))
                and math.isfinite(score) and 0 <= score <= 1, 'LOCAL_TITLE_OUTPUT')
        require(len(points) == 4 and all(len(point) == 2 for point in points), 'LOCAL_TITLE_BOX')
        require(all(isinstance(n, (float, int)) and math.isfinite(n) for point in points for n in point), 'LOCAL_TITLE_BOX')
        xs, ys = [p[0] for p in points], [p[1] for p in points]
        require(min(xs) >= 0 and min(ys) >= 0 and max(xs) <= bounds[2] and max(ys) <= bounds[3]
                and max(xs) > min(xs) and max(ys) > min(ys), 'LOCAL_TITLE_BOX')
        if score < .97:
            rejected.append(dict(text=text, score=score, reason='below_fixed_0.97_threshold'))
            continue
        words.append(dict(text=text, score=score, x=bounds[0] + min(xs), y=bounds[1] + min(ys),
                          width=max(xs) - min(xs), height=max(ys) - min(ys)))
    return words, rejected


class LocalTitleRecognizer:
    def __init__(self, root, *, engine=None, fast_title=True, line_reader=None):
        require(type(fast_title) is bool, 'LOCAL_TITLE_FAST_OPTION')
        require(line_reader is None or callable(line_reader), 'LOCAL_TITLE_LINE_READER')
        started = time.perf_counter()
        self.model_hashes = {}
        self.injected = engine is not None
        if engine is None:
            from importlib.metadata import version
            from rapidocr import RapidOCR
            require(version('rapidocr') == '3.9.2', 'LOCAL_TITLE_RUNTIME_VERSION')
            directory = Path(root) / '.tools/ocr-models'
            for name, digest in MODELS.items():
                path = directory / name
                require(path.is_file() and hashlib.sha256(path.read_bytes()).hexdigest() == digest,
                        'LOCAL_TITLE_MODEL_HASH:' + name)
                self.model_hashes[name] = digest
            engine = RapidOCR(params={
                'Global.log_level': 'error', 'Global.use_cls': False,
                'Global.model_root_dir': str(directory),
                'EngineConfig.onnxruntime.intra_op_num_threads': 2,
                'EngineConfig.onnxruntime.inter_op_num_threads': 1,
                'EngineConfig.onnxruntime.use_cuda': False,
                'EngineConfig.onnxruntime.use_dml': False,
                'Det.model_path': str(directory / 'PP-OCRv6_det_small.onnx'),
                'Rec.model_path': str(directory / 'PP-OCRv6_rec_small.onnx'),
                'Cls.model_path': str(directory / 'ch_ppocr_mobile_v2.0_cls_mobile.onnx'),
                'Det.limit_type': 'max', 'Det.limit_side_len': 1280,
            })
        self.engine = engine
        self.fast_title = fast_title
        self._line_reader = line_reader or (None if self.injected else self.recognize_line)
        self._title_cache = None
        self.initialization_ms = (time.perf_counter() - started) * 1000

    def apply(self, packet):
        return self._apply_region(packet, envelope_key='collection_title_image',
            kind='product_title', schema='collection-title-image-v1', metadata_key='local_title_ocr')

    def apply_catalog(self, packet):
        return self._apply_region(packet, envelope_key='collection_catalog_image',
            kind='catalog_names', schema='collection-catalog-image-v1', metadata_key='local_catalog_ocr')

    def recognize_line(self, image):
        # Direct recognition of one known text line leaves RapidOCR.use_det
        # unchanged. Calling its generic __call__(use_det=False) would mutate
        # shared engine defaults and accidentally disable catalog/price det.
        import numpy as np
        array = self.engine.load_img(np.array(image))
        return self.engine.recognize_txt([array])

    def _listing_line(self, packet, image, bounds, metadata):
        page = packet.get('startup_page', {})
        if (not self.fast_title or self._line_reader is None or bounds != [1915, 238, 530, 54]
                or page.get('page') != 'skin_listings' or page.get('overlay') != 'none'
                or packet['frames'][-1].get('width') != 2560
                or packet['frames'][-1].get('height') != 1440):
            return None
        started = time.perf_counter()
        result = self._line_reader(image)
        texts = list(result.txts) if result.txts is not None else []
        scores = [float(value) for value in result.scores] if result.scores is not None else []
        require(len(texts) == len(scores), 'LOCAL_TITLE_LINE_OUTPUT')
        require(all(isinstance(t, str) and len(t) <= 128 for t in texts)
                and all(math.isfinite(s) and 0 <= s <= 1 for s in scores), 'LOCAL_TITLE_LINE_OUTPUT')
        # An unreadable line may use the original detector. A confidently
        # different name is returned unchanged, NEVER retried to fit a rule.
        accepted = (len(texts) == 1 and bool(texts[0].strip())
                    and not any(c in texts[0] for c in '\r\n') and scores[0] >= .97)
        metadata['single_line_attempt'] = dict(texts=texts, scores=scores,
            elapsed_ms=(time.perf_counter()-started)*1000, accepted=accepted,
            fallback_reason=None if accepted else 'line_empty_multiline_or_low_confidence',
            expected_text_supplied=False, detector_used=False)
        if not accepted:
            return None
        x, y, width, height = bounds
        return [dict(text=texts[0], score=scores[0], x=x, y=y, width=width, height=height)], []

    def _apply_region(self, packet, *, envelope_key, kind, schema, metadata_key):
        envelope = packet.get(envelope_key)
        if envelope is None:
            return
        regions = packet.get('collection_observation', {}).get('regions', [])
        targets = [region for region in regions if region.get('kind') == kind]
        require(len(targets) == 1, 'LOCAL_TITLE_REGION_COUNT')
        region = targets[0]
        original = dict(region)
        empty_error = 'LOCAL_CATALOG_EMPTY' if kind == 'catalog_names' else 'LOCAL_TITLE_EMPTY'
        metadata = dict(provider='RapidOCR3.9.2/PP-OCRv6-local-CPU', model_sha256=self.model_hashes,
                        region_kind=kind,
                        source_frame_id=envelope.get('frame_id'), source_frame_sha256=envelope.get('frame_sha256'),
                        source_png_sha256=envelope.get('png_sha256'), image_file_writes=0,
                        expected_text_supplied=False, provider_selection='explicit_before_run_not_target_match')
        start = time.perf_counter()
        try:
            image, bounds = decode_region(packet, envelope, schema=schema, kind=kind)
            # A title is invariant across listings of the same product. Reuse
            # only a successful reading of BYTE-IDENTICAL, freshly validated
            # PNG pixels at the same bounds, not the configured product name
            # or an older frame's unverified identity. Prices/wear never enter
            # this cache. Caller mutations cannot alter its private words.
            cache_key = (envelope['png_sha256'], tuple(bounds))
            cached = self._title_cache if kind == 'product_title' else None
            if cached is not None and cached['key'] == cache_key:
                words, rejected = copy.deepcopy(cached['words']), copy.deepcopy(cached['rejected'])
                metadata.update(ok=True, words=words, rejected=rejected, bounds=bounds, error=None,
                    recognition_cache_hit=True, original_read_frame_id=cached['frame_id'],
                    evidence_basis='verified_exact_current_roi_bytes_same_bounds')
                metadata['recognition_route'] = cached.get('recognition_route', 'detector_then_recognizer')
                metadata['word_bounds_basis'] = cached.get('word_bounds_basis', 'actual_detector_boxes')
                region.update(ok=True, error='', words=words, truncated=False,
                    provider=metadata['provider'], windows_observation=original,
                    evidence_basis=metadata['evidence_basis'])
                return
            metadata['recognition_cache_hit'] = False
            line = self._listing_line(packet, image, bounds, metadata) if kind == 'product_title' else None
            if line is not None:
                words, rejected = line
                metadata.update(recognition_route='known_listing_single_line_recognizer',
                                word_bounds_basis='whole_current_observed_title_roi_not_detector_boxes')
            else:
                if self.injected:
                    result = self.engine(image)
                else:
                    import numpy as np
                    result = self.engine(np.array(image), use_cls=False)
                texts = list(result.txts) if result.txts is not None else []
                scores = [float(value) for value in result.scores] if result.scores is not None else []
                boxes = result.boxes.tolist() if hasattr(result.boxes, 'tolist') else result.boxes or []
                words, rejected = mapped_words(texts, scores, boxes, bounds)
                metadata.update(recognition_route='detector_then_recognizer', word_bounds_basis='actual_detector_boxes')
            metadata.update(ok=bool(words), words=words, rejected=rejected, bounds=bounds,
                            error=None if words else empty_error)
            region.update(ok=bool(words), error='' if words else empty_error, words=words,
                          truncated=False, provider=metadata['provider'], windows_observation=original)
            if kind == 'product_title':
                self._title_cache = (dict(key=cache_key, words=copy.deepcopy(words),
                    rejected=copy.deepcopy(rejected), frame_id=envelope['frame_id'],
                    recognition_route=metadata['recognition_route'], word_bounds_basis=metadata['word_bounds_basis']) if words else None)
        except Exception as error:
            if kind == 'product_title':
                self._title_cache = None
            metadata.update(ok=False, error=str(error))
            region.update(ok=False, error=str(error), words=[], windows_observation=original)
            raise
        finally:
            metadata['elapsed_ms'] = (time.perf_counter() - start) * 1000
            packet[metadata_key] = metadata
