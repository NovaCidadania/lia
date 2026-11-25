// Fila de tarefas.
export class Fila {
  #itens = []

  adicionar(valor, prioridade = 0) {
    this.#itens.push({ valor, prioridade })
  }

  proximo() {
    return this.#itens.shift()?.valor
  }

  get tamanho() {
    return this.#itens.length
  }
}
