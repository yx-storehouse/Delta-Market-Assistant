"""Explicit live trial with optional last-frame preview retained in memory.

Business logic and receipts are delegated unchanged to run_collection_trial.
This diagnostic wrapper adds in-memory visual review, not a different policy.
Full-screen preview encoding is OFF for normal collection. --memory-preview
enables it for explicit visual investigation. Necessary OCR crops remain in
memory in either mode, so never redirect this wrapper's stdout to a file.
"""
import json
import subprocess
import sys
import threading

import collection_live_session as live
import run_collection_trial as trial


class MemoryReviewBackend(live.WindowsBackend):
    def __init__(self, root, *, local_title=False, local_price=False, fast_capture=False,
                 memory_preview=False, lean_text=True, reuse_capture=True,fast_actions=True,local_hotpath=True,ready_stream=True,
                 parallel_local_price=False, return_policy='ide', recognizers=None):
        if type(memory_preview) is not bool or type(lean_text) is not bool:
            raise ValueError('COLLECTION_DIAGNOSTIC_BOOLEAN_OPTION')
        policy = {} if return_policy == 'ide' else dict(return_policy=return_policy)
        super().__init__(root, persistent_capture=fast_capture, **policy)
        if type(fast_actions) is not bool:
            raise ValueError('COLLECTION_DIAGNOSTIC_BOOLEAN_OPTION')
        self.fast_collection_motion=bool(fast_actions and fast_capture)
        self.collection_motion_profile='collection_continuous' if self.fast_collection_motion else 'standard'
        self._metadata['collection_action_motion_profile']=self.collection_motion_profile
        self.memory_preview=memory_preview
        self.lean_text=lean_text
        if type(reuse_capture) is not bool:
            raise ValueError('COLLECTION_DIAGNOSTIC_BOOLEAN_OPTION')
        self.reuse_capture=bool(reuse_capture and fast_capture)
        if type(local_hotpath) is not bool:
            raise ValueError('COLLECTION_DIAGNOSTIC_BOOLEAN_OPTION')
        self.local_hotpath=bool(local_hotpath and fast_capture and self.reuse_capture)
        self._metadata['calibrated_listing_hotpath_enabled']=self.local_hotpath
        self._metadata['memory_preview_enabled']=memory_preview
        self._metadata['duplicate_native_text_omitted']=bool(lean_text and local_title)
        self._metadata['capture_resource_reuse_enabled']=self.reuse_capture
        if type(ready_stream) is not bool:
            raise ValueError('COLLECTION_DIAGNOSTIC_BOOLEAN_OPTION')
        self.ready_stream=bool(ready_stream and fast_capture and self.reuse_capture)
        self._metadata['latest_frame_stream_enabled']=self.ready_stream
        # Native pixel receipts read the latest-frame stream without OCR.
        self.pixel_receipt=self.ready_stream
        self._metadata['pixel_receipt_available']=self.pixel_receipt
        self.local_title = None
        self.local_price = None
        if recognizers is not None:
            # A later foreground segment of the same run reuses the already
            # hash-verified engines of the first; nothing else is shared.
            title, price, parallel = recognizers
            if not (local_title and local_price) or title is None or price is None or type(parallel) is not bool:
                raise ValueError('COLLECTION_RECOGNIZER_REUSE')
            self.local_title, self.local_price, self.parallel_local_price = title, price, parallel
            self._metadata.update(local_title_provider='RapidOCR3.9.2/PP-OCRv6-local-CPU',
                local_catalog_provider='RapidOCR3.9.2/PP-OCRv6-local-CPU',
                local_price_provider='RapidOCR3.9.2/PP-OCRv6-local-CPU',
                local_title_model_sha256=title.model_hashes, local_recognizers_reused=True,
                parallel_local_title_price=parallel)
            return
        if local_title:
            from collection_local_title import LocalTitleRecognizer
            self.local_title = LocalTitleRecognizer(root)
            self._metadata['local_title_provider'] = 'RapidOCR3.9.2/PP-OCRv6-local-CPU'
            self._metadata['local_catalog_provider'] = 'RapidOCR3.9.2/PP-OCRv6-local-CPU'
            self._metadata['local_title_model_sha256'] = self.local_title.model_hashes
            self._metadata['local_title_initialization_ms'] = self.local_title.initialization_ms
        if type(parallel_local_price) is not bool:
            raise ValueError('COLLECTION_DIAGNOSTIC_BOOLEAN_OPTION')
        self.parallel_local_price = False
        if local_price:
            if self.local_title is None:
                raise ValueError('LOCAL_PRICE_REQUIRES_LOCAL_MODEL')
            from collection_local_price import LocalPriceRecognizer
            model = self.local_title
            if parallel_local_price:
                # A second instance of the same hash-verified models, so the
                # same-frame title line and price crop never share an engine
                # object while they are recognized concurrently.
                from collection_local_title import LocalTitleRecognizer
                model = LocalTitleRecognizer(root)
                self.parallel_local_price = True
                self._metadata['local_price_model_sha256'] = model.model_hashes
            self.local_price = LocalPriceRecognizer(model)
            self._metadata['local_price_provider'] = 'RapidOCR3.9.2/PP-OCRv6-local-CPU'
        self._metadata['parallel_local_title_price'] = self.parallel_local_price

    def recognizers(self):
        return self.local_title, self.local_price, self.parallel_local_price

    def capture(self, command, timeout):
        command = list(command)
        if '--collection-pixel-receipt' in command:
            # Pixel-only receipt: no crops are requested or recognized.
            reply = super().capture(command, timeout)
            packet = json.loads(reply.stdout)
            packet['diagnostic_actual_command'] = command
            return subprocess.CompletedProcess(command, reply.returncode,
                json.dumps(packet, ensure_ascii=False).encode('utf-8'), reply.stderr)
        if getattr(self, 'reuse_capture', False) and '--reuse-capture-resources' not in command:
            command.append('--reuse-capture-resources')
        if (getattr(self,'ready_stream',False) and getattr(self,'reuse_capture',False)
                and '--collection-ready-stream' not in command):
            command.append('--collection-ready-stream')
        expected_page=(command[command.index('--expected-page')+1]
            if '--expected-page' in command and command.index('--expected-page')+1<len(command) else None)
        if (getattr(self,'local_hotpath',False) and expected_page=='skin_listings'
                and ('--collection-selection-preflight' in command or '--collection-receipt-reference' in command
                     or '--collection-scroll-readiness' in command)
                and '--collection-local-hotpath' not in command):
            command.append('--collection-local-hotpath')
        if getattr(self, 'memory_preview', False) and '--preview-stdout' not in command:
            command.append('--preview-stdout')
        if '--collection-layout' in command and '--collection-price-image' not in command:
            command.append('--collection-price-image')
        if getattr(self, 'local_price', None) is not None and '--collection-layout' in command:
            if '--collection-numeric-price' not in command:
                raise ValueError('LOCAL_PRICE_REQUIRES_NUMERIC_MEASUREMENT')
            command.append('--collection-local-price')
        if '--collection-observation' in command and '--collection-title-image' not in command:
            command.append('--collection-title-image')
        if '--collection-observation' in command and '--collection-catalog-image' not in command:
            command.append('--collection-catalog-image')
        if (getattr(self, 'lean_text', True) and getattr(self, 'local_title', None) is not None
                and '--collection-observation' in command and '--collection-local-text' not in command):
            command.append('--collection-local-text')
        reply = super().capture(command, timeout)
        packet = json.loads(reply.stdout)
        packet['diagnostic_actual_command'] = command
        packet['collection_diagnostic_options'] = dict(
            full_preview_requested='--preview-stdout' in command,
            native_duplicate_text_omitted='--collection-local-text' in command)
        local_title = getattr(self, 'local_title', None)
        if local_title is not None and reply.returncode == 0:
            local_price = getattr(self, 'local_price', None)
            price_errors = []
            worker = None
            if local_price is not None and getattr(self, 'parallel_local_price', False):
                def read_price():
                    try:
                        local_price.apply(packet)
                    except Exception as error:
                        price_errors.append(error)
                worker = threading.Thread(target=read_price, name='collection-local-price', daemon=True)
                worker.start()
            try:
                try:
                    local_title.apply(packet)
                    local_title.apply_catalog(packet)
                finally:
                    if worker is not None:
                        worker.join()
                if price_errors:
                    raise price_errors[0]
                if local_price is not None and worker is None:
                    local_price.apply(packet)
            except Exception as error:
                packet['local_title_error'] = str(error)
                packet['capture_passed'] = False
        return subprocess.CompletedProcess(command, reply.returncode,
            json.dumps(packet, ensure_ascii=False).encode('utf-8'), reply.stderr)


def main():
    if '--verify-runtime-only' in sys.argv:
        if len(sys.argv) != 2:
            raise ValueError('COLLECTION_RUNTIME_CHECK_TAKES_NO_LIVE_ARGUMENTS')
        from collection_runtime import verify_runtime
        result = verify_runtime(__file__, expected_root=live.ROOT)
        if str(trial.ROOT) != result['project_root']:
            raise ValueError('COLLECTION_RUNTIME_COORDINATOR_ROOT_MISMATCH')
        print(json.dumps(result, ensure_ascii=False))
        return 0
    requested_command = [sys.executable, *sys.argv]
    local_title = '--local-title-ocr' in sys.argv
    if local_title:
        sys.argv.remove('--local-title-ocr')
    local_price = '--local-price-ocr' in sys.argv
    if local_price:
        sys.argv.remove('--local-price-ocr')
        if not local_title or '--numeric-price' not in sys.argv:
            raise ValueError('LOCAL_PRICE_REQUIRES_LOCAL_TITLE_AND_NUMERIC_PRICE')
    fast_capture = '--fast-capture' in sys.argv
    if fast_capture:
        sys.argv.remove('--fast-capture')
    ready_stream = '--no-ready-stream' not in sys.argv
    if not ready_stream:
        sys.argv.remove('--no-ready-stream')
    memory_preview = '--memory-preview' in sys.argv
    if memory_preview:
        sys.argv.remove('--memory-preview')
    original = live.ForegroundSession
    instances = []

    class ReviewedSession(original):
        def __init__(self, *args, **kwargs):
            if kwargs.get('backend') is None:
                kwargs['backend'] = MemoryReviewBackend(live.ROOT, local_title=local_title,
                    local_price=local_price, fast_capture=fast_capture, memory_preview=memory_preview,
                    ready_stream=ready_stream)
            if fast_capture:
                kwargs['fast_settle'] = True
            super().__init__(*args, **kwargs)
            self.report['requested_runner_command'] = requested_command
            self.report['local_title_provider_selected_before_run'] = local_title
            self.report['local_catalog_provider_selected_before_run'] = local_title
            self.report['local_price_provider_selected_before_run'] = local_price
            self.report['fast_capture_selected_before_run'] = fast_capture
            self.report['memory_preview_selected_before_run'] = memory_preview
            self.report['ready_stream_selected_before_run'] = bool(ready_stream and fast_capture)
            instances.append(self)

    live.ForegroundSession = ReviewedSession
    try:
        code = trial.main()
    finally:
        live.ForegroundSession = original
    if instances:
        session = instances[-1]
        image = session.last_price_image or {}
        title_image = session.last_title_image or {}
        catalog_image = session.last_catalog_image or {}
        print(json.dumps(dict(packet='collection-memory-review-v1',
            exit_status=code, ide_restored=session.report.get('ide_restored'),
            preview_jpeg_base64=session.preview_jpeg(),
            price_png_base64=image.get('png_base64'),
            price_image_metadata={key: value for key, value in image.items() if key != 'png_base64'},
            title_png_base64=title_image.get('png_base64'),
            title_image_metadata={key: value for key, value in title_image.items() if key != 'png_base64'},
            catalog_png_base64=catalog_image.get('png_base64'),
            catalog_image_metadata={key: value for key, value in catalog_image.items() if key != 'png_base64'},
            image_file_writes=0), ensure_ascii=False))
    return code


if __name__ == '__main__':
    raise SystemExit(main())
