"""Durable collection attempts: prepare before input, never retry a star toggle."""
import hashlib
import json
import os
from pathlib import Path
import uuid


def candidate_key(candidate):
    # Wear/product identify conservatively. A position or refreshed price is
    # not a new identity; a collision stops rather than toggles an existing star.
    identity = {k: candidate[k] for k in ('product', 'condition', 'wear')}
    return hashlib.sha256(json.dumps(identity, sort_keys=True, ensure_ascii=False).encode()).hexdigest()


def receipt_matches(before, after, packet):
    original=before.get('favorite_before',{})
    transition_proven=original.get('favorite_warm_fraction')==0 and original.get('favorite_bright_fraction',0)>=.04
    return (
        all(before.get(k) == after.get(k) for k in ('product', 'condition', 'price', 'wear', 'row_index'))
        and before['source_frame_sha256'] != after['source_frame_sha256']
        and (packet.get('startup_page', {}).get('anchor_checks', {}).get('collection.added') is True or transition_proven)
        and packet.get('collection_selected_card', {}).get('favorite_warm_fraction', 0) >= .05
        and packet.get('collection_selected_card', {}).get('favorite_bright_fraction', 1) <= .02
    )


class CollectionJournal:
    def __init__(self, directory):
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)

    def prepare(self, candidate, evidence):
        key = candidate_key(candidate)
        document = dict(key=key, status='prepared', candidate=candidate, evidence=evidence)
        # Exclusive reservation is the durable no-repeat barrier, including
        # process exit after SendInput and before the receipt can be recorded.
        with (self.directory / (key + '.json')).open('x', encoding='utf-8') as file:
            json.dump(document, file, ensure_ascii=False, indent=2)
            file.flush()
            os.fsync(file.fileno())
        return key

    def update(self, key, status, **details):
        if status not in ('dispatched', 'input_uncertain', 'confirmed', 'confirmed_reconciliation'):
            raise ValueError('COLLECTION_JOURNAL_STATUS')
        path = self.directory / (key + '.json')
        document = json.loads(path.read_text(encoding='utf-8'))
        allowed = {
            'prepared': {'dispatched', 'input_uncertain', 'confirmed_reconciliation'},
            'dispatched': {'confirmed', 'confirmed_reconciliation'},
            'input_uncertain': {'confirmed_reconciliation'},
        }
        if status not in allowed.get(document['status'], set()):
            raise ValueError('COLLECTION_JOURNAL_TRANSITION')
        document.update(status=status, **details)
        temporary = self.directory / (key + '.' + uuid.uuid4().hex + '.tmp')
        with temporary.open('x', encoding='utf-8') as file:
            json.dump(document, file, ensure_ascii=False, indent=2)
            file.flush()
            os.fsync(file.fileno())
        os.replace(temporary, path)
        return document
