"""Estoque em memória: quantidade por SKU."""


class Estoque:
    def __init__(self):
        self._itens = {}

    def adicionar(self, sku, qtd):
        self._itens[sku] = self._itens.get(sku, 0) + qtd

    def remover(self, sku, qtd):
        self._itens[sku] = self._itens.get(sku, 0) - qtd

    def quantidade(self, sku):
        return self._itens.get(sku, 0)

    def skus(self):
        return sorted(self._itens)
