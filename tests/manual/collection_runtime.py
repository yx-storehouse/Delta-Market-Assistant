"""Read-only runtime resource verification; no capture/model/Win32 initialization."""
import hashlib
from pathlib import Path

from collection_local_title import MODELS
from collection_paths import project_root


def verify_runtime(module_file=__file__, *, expected_root=None):
    root = project_root(module_file)
    if expected_root is not None and root != Path(expected_root).resolve():
        raise ValueError('COLLECTION_RUNTIME_ROOT_MISMATCH')
    required = {
        'executable': root / 'dist/RelinkStudio/RelinkStudio.exe',
        'python': root / '.tools/ocr-runtime/Scripts/python.exe',
        'ocr_worker': root / 'dist/RelinkStudio/vision/windows_ocr_worker.ps1',
    }
    resources = {}
    for name, path in required.items():
        if not path.is_file():
            raise ValueError('COLLECTION_RUNTIME_RESOURCE_MISSING:' + name)
        resources[name] = dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    models = {}
    for name, expected in MODELS.items():
        path = root / '.tools/ocr-models' / name
        if not path.is_file():
            raise ValueError('COLLECTION_RUNTIME_MODEL_MISSING:' + name)
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != expected:
            raise ValueError('COLLECTION_RUNTIME_MODEL_HASH:' + name)
        models[name] = dict(path=str(path), sha256=actual)
    return dict(schema='collection-runtime-check-v1', status='passed',
                project_root=str(root), module_path=str(Path(module_file).resolve()),
                resources=resources, models=models, models_copied=False,
                game_capture_performed=False, game_input_sent=False,
                model_initialized=False, image_file_writes=0)
