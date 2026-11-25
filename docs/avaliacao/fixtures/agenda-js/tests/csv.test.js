import test from 'node:test'
import assert from 'node:assert/strict'
import { parseCsv } from '../src/csv.js'

test('csv simples', () => {
  assert.deepEqual(parseCsv('a;b\n1;2\n'), [['a', 'b'], ['1', '2']])
})
