"""The purchase delay, tuned by the result of each real press.

User 2026-10-10: "用运行页那两个设置做自动调延迟". The 运行 page's settings
(run_settings in the program's config):
- 队列已满时减延迟: queueFullTrigger (times) / queueFullStepMs: every
  queueFullTrigger results "抢购队列已满" the delay goes down one step
  (live buy01: pressed at zero + 830 ms, the queue was already full);
- 公示期加延迟: publicityTrigger / publicityStepMs: every publicityTrigger
  results "订单尚未开放购买" / "当前订单还在公示期内" (pressed before the
  unlock) the delay goes up one step.
Other results change nothing. The delay in use is kept next to the config
(purchase_delay_state.json), so it survives a restart; a new 购买延迟 on the
运行 page starts again from that value. Steps of 0 switch a rule off.
"""
import json
import os
from pathlib import Path

STATE_NAME = 'purchase_delay_state.json'
DELAY_RANGE_MS = (0.0, 60000.0)
# Tuning never goes below this (unless the 运行 page's own value is lower):
# the dialog's 0分0秒 frame must be seen before the press: the watch through
# it returns some 40-60 ms after the zero (review wf_c25b3091-e15: a delay
# tuned to 0 left every press refused as uncalibrated, never counted).
TUNED_FLOOR_MS = 160.0
READ_RETRIES = 3
DECREASE_OUTCOMES = frozenset(('queue_full',))
INCREASE_OUTCOMES = frozenset(('not_open_yet', 'still_publicity'))


def _number(value, low, high):
    return type(value) in (int, float) and low <= value <= high


class DelayTuner:
    def __init__(self, config_path):
        self.config = Path(config_path)
        self.path = self.config.parent / STATE_NAME
        self._read_settings()
        state = self._load()
        if state is None or state.get('base_ms') != self.base:
            state = dict(base_ms=self.base, delay_ms=self.base, counts=dict(decrease=0, increase=0), changes=[])
        self.state = state

    def _read_settings(self):
        run = json.loads(self.config.read_text(encoding='utf-8')).get('run_settings', {})
        base = run.get('purchaseDelayMs', 830)
        if not _number(base, *DELAY_RANGE_MS):
            raise ValueError('PURCHASE_DELAY_SETTINGS')
        self.rules = dict(
            decrease=(int(run.get('queueFullTrigger', 1)), float(run.get('queueFullStepMs', 0.0))),
            increase=(int(run.get('publicityTrigger', 1)), float(run.get('publicityStepMs', 0.0))))
        for trigger, step in self.rules.values():
            if not (1 <= trigger <= 9999 and 0.0 <= step <= 1000.0):
                raise ValueError('PURCHASE_DELAY_SETTINGS')
        self.base = float(base)
        self.floor = min(self.base, TUNED_FLOOR_MS)

    def refresh(self):
        """Before each attempt: a 购买延迟 changed on the 运行 page meanwhile
        starts over from it; the rules follow the page too."""
        old = self.base
        self._read_settings()
        if self.base != old or self.state.get('base_ms') != self.base:
            self.state = dict(base_ms=self.base, delay_ms=self.base, counts=dict(decrease=0, increase=0),
                              changes=(self.state.get('changes') or [])[-49:])

    @classmethod
    def open(cls, config_path, sleep=None):
        """(tuner, None), or (None, reason) when the config cannot be read
        after a few tries (the program may be saving it at that moment)."""
        import time
        sleep = sleep or time.sleep
        reason = None
        for attempt in range(READ_RETRIES):
            try:
                return cls(config_path), None
            except (OSError, ValueError, TypeError, AttributeError) as error:
                reason = str(error) or type(error).__name__
                if attempt + 1 < READ_RETRIES:
                    sleep(.2)
        return None, reason

    @classmethod
    def for_config(cls, config_path):
        """A tuner, or None when the config cannot be read (no tuning)."""
        return cls.open(config_path, sleep=lambda s: None)[0]

    def _load(self):
        try:
            state = json.loads(self.path.read_text(encoding='utf-8'))
        except (OSError, ValueError):
            return None
        if (not isinstance(state, dict) or not _number(state.get('delay_ms'), *DELAY_RANGE_MS)
                or not isinstance(state.get('counts'), dict)):
            return None
        return state

    def _save(self):
        part = self.path.with_suffix('.json.part')
        part.write_text(json.dumps(self.state, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        os.replace(part, self.path)

    def current(self):
        return float(self.state['delay_ms'])

    def record(self, outcome, when=None):
        """Count one real press's result; returns the change (dict) or None."""
        rule = 'decrease' if outcome in DECREASE_OUTCOMES else 'increase' if outcome in INCREASE_OUTCOMES else None
        if rule is None:
            return None
        trigger, step = self.rules[rule]
        counts = self.state['counts']
        counts[rule] = counts.get(rule, 0) + 1
        change = None
        if counts[rule] >= trigger:
            counts[rule] = 0
            before = self.current()
            after = before - step if rule == 'decrease' else before + step
            after = round(min(max(after, self.floor), DELAY_RANGE_MS[1]), 1)
            if after != before:
                self.state['delay_ms'] = after
                change = dict(outcome=outcome, rule=rule, before_ms=before, after_ms=after, time=when)
                self.state['changes'] = (self.state.get('changes') or [])[-49:] + [change]
        self._save()
        return change
