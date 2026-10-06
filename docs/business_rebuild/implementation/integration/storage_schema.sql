-- Relink Studio M10 storage target; specification only, not executed in this turn.
-- SQLite dialect. All IDs are application-generated, UTF-8 text.
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS schema_meta (
  key TEXT PRIMARY KEY NOT NULL,
  value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS runs (
  run_id TEXT PRIMARY KEY NOT NULL,
  session_id TEXT NOT NULL,
  clock_domain_id TEXT NOT NULL,
  rule_task_id TEXT NOT NULL,
  rule_revision INTEGER NOT NULL CHECK (rule_revision > 0),
  mode TEXT NOT NULL CHECK (mode IN ('Replay','Observe')),
  state TEXT NOT NULL,
  cancel_epoch INTEGER NOT NULL CHECK (cancel_epoch >= 0),
  viewport_generation INTEGER NOT NULL CHECK (viewport_generation >= 0),
  created_mono_ms INTEGER NOT NULL CHECK (created_mono_ms >= 0),
  stopped_mono_ms INTEGER,
  source_revision TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS events (
  event_id TEXT PRIMARY KEY NOT NULL,
  run_id TEXT NOT NULL REFERENCES runs(run_id),
  session_id TEXT NOT NULL,
  clock_domain_id TEXT NOT NULL,
  step_id TEXT NOT NULL,
  seq INTEGER NOT NULL CHECK (seq >= 0),
  at_mono_ms INTEGER NOT NULL CHECK (at_mono_ms >= 0),
  cancel_epoch INTEGER NOT NULL CHECK (cancel_epoch >= 0),
  viewport_generation INTEGER NOT NULL CHECK (viewport_generation >= 0),
  type TEXT NOT NULL,
  payload_json TEXT NOT NULL,
  payload_sha256 TEXT NOT NULL,
  image_ref TEXT,
  UNIQUE(run_id, seq)
);

CREATE TABLE IF NOT EXISTS attempts (
  attempt_id TEXT PRIMARY KEY NOT NULL,
  intent_id TEXT NOT NULL UNIQUE,
  run_id TEXT NOT NULL REFERENCES runs(run_id),
  rule_task_id TEXT NOT NULL,
  rule_revision INTEGER NOT NULL CHECK (rule_revision > 0),
  association_ref TEXT,
  kind TEXT NOT NULL CHECK (kind IN ('purchase','collection')),
  state TEXT NOT NULL,
  quantity INTEGER NOT NULL CHECK (quantity > 0),
  reservation_held INTEGER NOT NULL CHECK (reservation_held IN (0,1)),
  dispatch_proof TEXT NOT NULL,
  receipt_identity TEXT,
  created_event_id TEXT NOT NULL REFERENCES events(event_id),
  updated_event_id TEXT NOT NULL REFERENCES events(event_id)
);

CREATE TABLE IF NOT EXISTS receipts (
  receipt_identity TEXT NOT NULL,
  attempt_id TEXT NOT NULL REFERENCES attempts(attempt_id),
  outcome TEXT NOT NULL CHECK (outcome IN ('success','failed','unknown')),
  association TEXT NOT NULL CHECK (association IN ('confirmed','ambiguous')),
  payload_json TEXT NOT NULL,
  event_id TEXT NOT NULL REFERENCES events(event_id),
  PRIMARY KEY (receipt_identity, attempt_id)
);

CREATE TABLE IF NOT EXISTS quota_reservations (
  reservation_id TEXT PRIMARY KEY NOT NULL,
  attempt_id TEXT NOT NULL UNIQUE REFERENCES attempts(attempt_id),
  scope_id TEXT NOT NULL,
  quantity INTEGER NOT NULL CHECK (quantity > 0),
  state TEXT NOT NULL CHECK (state IN ('held','released','consumed','unresolved')),
  created_event_id TEXT NOT NULL REFERENCES events(event_id),
  resolved_event_id TEXT
);

CREATE TABLE IF NOT EXISTS ledger_transactions (
  transaction_id TEXT PRIMARY KEY NOT NULL,
  attempt_id TEXT NOT NULL UNIQUE REFERENCES attempts(attempt_id),
  terminal TEXT NOT NULL CHECK (terminal IN ('Success','Failed')),
  queue_empty INTEGER NOT NULL CHECK (queue_empty IN (0,1)),
  receipt_identity TEXT,
  event_id TEXT NOT NULL REFERENCES events(event_id)
);

CREATE UNIQUE INDEX IF NOT EXISTS events_run_seq ON events(run_id, seq);
CREATE INDEX IF NOT EXISTS events_run_time ON events(run_id, at_mono_ms, seq);
CREATE INDEX IF NOT EXISTS attempts_run_state ON attempts(run_id, state);
CREATE INDEX IF NOT EXISTS reservations_scope_state ON quota_reservations(scope_id, state);
