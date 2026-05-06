const _grIn  = new Map()  // ch → { gate, comp }
const _grOut = new Map()  // ch → { comp, lim }

export function handleGrLine(parts) {
  // "gr in <ch> gate <db> comp <db>"
  // "gr out <ch> gate <db> comp <db> lim <db>"
  if (parts[1] === 'in' && parts.length >= 7) {
    const ch = parseInt(parts[2], 10)
    _grIn.set(ch, { gate: parseFloat(parts[4]), comp: parseFloat(parts[6]) })
  } else if (parts[1] === 'out' && parts.length >= 9) {
    const ch = parseInt(parts[2], 10)
    _grOut.set(ch, { gate: parseFloat(parts[4]), comp: parseFloat(parts[6]), lim: parseFloat(parts[8]) })
  }
}

export function getGrSnapshot() {
  return {
    inputs:  Array.from(_grIn.entries()).map(([ch, v]) => ({ ch, ...v })),
    outputs: Array.from(_grOut.entries()).map(([ch, v]) => ({ ch, ...v })),
  }
}
