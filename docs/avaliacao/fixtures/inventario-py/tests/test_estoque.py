import unittest

from inventario.estoque import Estoque


class TestEstoque(unittest.TestCase):
    def test_adicionar_e_remover(self):
        e = Estoque()
        e.adicionar("A1", 5)
        e.remover("A1", 2)
        self.assertEqual(e.quantidade("A1"), 3)

    def test_skus_ordenados(self):
        e = Estoque()
        e.adicionar("B2", 1)
        e.adicionar("A1", 1)
        self.assertEqual(e.skus(), ["A1", "B2"])


if __name__ == "__main__":
    unittest.main()
