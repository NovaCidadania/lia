"""Relatório de texto do estoque."""


def gerar_relatorio(estoque):
    linhas = ["%s: %d" % (sku, estoque.quantidade(sku)) for sku in estoque.skus()]
    return "\n".join(linhas)
