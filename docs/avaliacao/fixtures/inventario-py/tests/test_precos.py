import unittest

from inventario.precos import aplicar_desconto, total_itens


class TestPrecos(unittest.TestCase):
    def test_desconto_simples(self):
        self.assertEqual(aplicar_desconto(100.0, 10), 90.0)

    def test_total(self):
        self.assertEqual(total_itens([(2.5, 2), (1.0, 3)]), 8.0)


if __name__ == "__main__":
    unittest.main()
