# modelo-proprio — protótipo de modelo neural da Lia, treinado do zero

Escopo e critérios: `docs/MODELO-PROPRIO.md` (pré-registrados). **Não é um modelo de programação geral**: é uma rede
decoder-only de ~1,7 M de parâmetros que só tenta corrigir **um defeito de uma linha** em funções Python pequenas.
Pesos aleatórios na partida, dados 100 % sintéticos gerados por `lia_neural/dados.py`, nenhum peso/API/modelo de terceiros.

## Ambiente (uma vez; nada global é alterado)

```sh
python3 -m venv modelo-proprio/.venv
modelo-proprio/.venv/bin/pip install torch --index-url https://download.pytorch.org/whl/cpu   # só a biblioteca de tensores
```

O venv é ignorado pelo git. `modelo-proprio/lia-neural` usa esse venv.

## Ciclo completo (reproduzível; semente fixa)

```sh
R=~/.lia/modelo-proprio
modelo-proprio/lia-neural dados   --saida $R/dados-s1 --semente 1                       # 6000/300/300, partições por família
modelo-proprio/lia-neural treinar --dados $R/dados-s1 --saida $R/run1 --semente 1 \
    --passos 4000 --lote 24 --lr 2e-3 --threads 6 --max-segundos 6600 --max-memoria-mb 6000
modelo-proprio/lia-neural info    --checkpoint $R/run1/ultimo.pt                         # sha256, bytes, parâmetros, semente, hash do dataset
modelo-proprio/lia-neural avaliar --dados $R/dados-s1 --checkpoint $R/run1/ultimo.pt --particao teste --saida $R/run1/teste.json
modelo-proprio/lia-neural e2e-lia --dados $R/dados-s1 --checkpoint $R/run1/ultimo.pt --n 20 --saida $R/run1/e2e.json   # lia-sessiond real
```

Retomar um treino interrompido: `treinar ... --retomar $R/run1/ultimo.pt` (mesmo dataset; outro é recusado).
Inferência avulsa: `lia-neural propor --checkpoint CKPT --pedido "…" --arquivo m.py`.

## Backend experimental no lia-sessiond (opt-in)

```sh
make sessiond
LIA_SESSIOND_MODELO=lia_proprio LIA_PROPRIO_CHECKPOINT=$R/run1/ultimo.pt build/lia-sessiond
```

Sem `LIA_PROPRIO_CHECKPOINT` válido a pergunta falha com `modelo_indisponivel` (nada de outro modelo). O modelo só **propõe**
`{"edicoes":[{"acao":"editar",…}]}`; aplicar, diff, aprovação, escrita e testes são do runtime da Lia. O resumo final em texto livre é recusado
(a Lia usa o texto determinístico): o modelo não declara "concluído". Toda resposta que usa o backend mostra `Lia própria (experimental) · <arquivo> · sha256 <12 hex>`.

## Testes

```sh
make teste-modelo-proprio            # tokenizer, dataset, sandbox, runtime, checkpoint, treino/retomada, inferência, CLI (precisa do venv)
make teste-modelo-proprio-backend    # backend C com um CLI de mentira
```

## Arquivos

| arquivo | papel |
|---|---|
| `lia_neural/tokenizer.py` | bytes UTF-8 + 12 marcadores; formato nativo; parser estrito da saída |
| `lia_neural/familias.py`, `dados.py` | 36 famílias de função, mutações, gerador determinístico, partições por família, manifesto |
| `lia_neural/modelo.py` | GPT pré-norma (atenção causal, cache KV), pesos de saída compartilhados |
| `lia_neural/treino.py`, `checkpoint.py` | treino com limites e retomada; checkpoint com pesos, config, versão, semente, hash do dataset, sha256 |
| `lia_neural/sandbox.py`, `runtime.py` | execução isolada (bwrap, sem rede) do código proposto; só o runtime conclui |
| `lia_neural/baseline.py`, `avaliacao.py` | baselines determinísticos e comparação no holdout |
| `lia_neural/inferencia.py`, `e2e_lia.py` | adaptador para o prompt de coding da Lia; integração ponta a ponta com o daemon real |
