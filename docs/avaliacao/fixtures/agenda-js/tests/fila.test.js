import test from 'node:test'
import assert from 'node:assert/strict'
import { Fila } from '../src/fila.js'

test('ordem de chegada com a mesma prioridade', () => {
  const f = new Fila()
  f.adicionar('a')
  f.adicionar('b')
  assert.equal(f.proximo(), 'a')
  assert.equal(f.tamanho, 1)
})
