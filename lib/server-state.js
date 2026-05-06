import { EventEmitter } from 'events';

export const stateEvents = new EventEmitter();

// Central in-memory state — keys match what the frontend receives
const _state = Object.create(null);

/**
 * Seed initial values without emitting patch events (called once at startup).
 */
export function initState(initial) {
  Object.assign(_state, initial);
}

/**
 * Update a state key. Emits 'patch' only when the value actually changes.
 * Returns true if state changed.
 */
export function setState(key, value) {
  if (JSON.stringify(_state[key]) === JSON.stringify(value)) return false;
  _state[key] = value;
  stateEvents.emit('patch', key, value);
  return true;
}

/** Return a shallow copy of the full state (safe to send to client). */
export function getState() {
  return { ..._state };
}

/** Return a single state key. */
export function getStateKey(key) {
  return _state[key];
}
