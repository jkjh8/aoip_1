export function defaultInputDsp() {
  return {
    trim: 0,
    hpf: { enabled: false, slope: 12, fc: 80.0 },
    eq: [
      { band: 1, enabled: false, type: 'peak',       fc: 100,  q: 0.707, gainDb: 0 },
      { band: 2, enabled: false, type: 'peak',       fc: 500,  q: 0.707, gainDb: 0 },
      { band: 3, enabled: false, type: 'peak',       fc: 2000, q: 0.707, gainDb: 0 },
      { band: 4, enabled: false, type: 'high_shelf', fc: 8000, q: 0.707, gainDb: 0 },
    ],
    gate: { enabled: false, threshold: -40, attackMs: 5,  releaseMs: 100, holdMs: 50, rangeDb: -80 },
    comp: { enabled: false, threshold: -20, ratio: 4, knee: 6, attackMs: 10, releaseMs: 100, makeupDb: 0 },
  }
}

export function defaultOutputDsp() {
  return {
    gate: { enabled: false, threshold: -40, attackMs: 5, releaseMs: 100, holdMs: 50, rangeDb: -80 },
    eq: [
      { band: 1, enabled: false, type: 'peak',       fc: 100,  q: 0.707, gainDb: 0 },
      { band: 2, enabled: false, type: 'peak',       fc: 500,  q: 0.707, gainDb: 0 },
      { band: 3, enabled: false, type: 'peak',       fc: 2000, q: 0.707, gainDb: 0 },
      { band: 4, enabled: false, type: 'high_shelf', fc: 8000, q: 0.707, gainDb: 0 },
    ],
    comp: { enabled: false, threshold: -20, ratio: 4, knee: 6, attackMs: 10, releaseMs: 100, makeupDb: 0 },
    lim:  { enabled: false, threshold: -0.5, releaseMs: 200 },
  }
}
