import test from 'node:test'
import assert from 'node:assert/strict'
import { parseDataBR, formatarDataBR } from '../src/datas.js'

test('ida e volta de uma data válida', () => {
  assert.equal(formatarDataBR(parseDataBR('25/11/2026')), '25/11/2026')
})
