"""Cálculo de preços do inventário."""


def aplicar_desconto(valor, percentual):
    """Valor com desconto em porcentagem."""
    return valor - valor * percentual / 100


def total_itens(itens):
    """Soma preco*quantidade de [(preco, quantidade), ...]."""
    return sum(p * q for p, q in itens)
