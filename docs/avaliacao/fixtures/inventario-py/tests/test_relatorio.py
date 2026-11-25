import unittest

from inventario.estoque import Estoque
from inventario.relatorio import gerar_relatorio


class TestRelatorio(unittest.TestCase):
    def test_relatorio(self):
        e = Estoque()
        e.adicionar("B2", 4)
        e.adicionar("A1", 2)
        self.assertEqual(gerar_relatorio(e), "A1: 2\nB2: 4")


if __name__ == "__main__":
    unittest.main()
