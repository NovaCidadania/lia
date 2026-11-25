# Treino de adaptador (LoRA/QLoRA) — receita reproduzível

**Estado em 2026-11-24: nenhum modelo foi treinado nem adaptado.** Esta
máquina não tem o necessário (`python3 ferramentas/treino/lia_treino.py
verificar-ambiente` → sem GPU CUDA utilizável — a GT 750M tem 2 GB e o
driver não carrega; sem torch/transformers/peft; 15,5 GB de RAM), e ainda
não existem trajetórias verificadas suficientes (zero positivas reais).

## O que LoRA/QLoRA é e não é

- LoRA treina matrizes pequenas acopladas a camadas de um modelo **base**
  já pré-treinado; QLoRA faz o mesmo com a base quantizada em 4 bits
  durante o treino.
- O adaptador **não substitui** a base: na inferência, a base é carregada
  inteira e o adaptador por cima (`llama-server -m base.gguf --lora
  adaptador.gguf`, ou mesclado num novo GGUF). A memória de inferência é a
  da base mais um pouco.
- Não é pré-treino: o conhecimento de programação vem da base. O adaptador
  ensina o **formato e o processo** da Lia (contrato da proposta, edições
  pequenas, uso do contexto lido).

## O que conta como exemplo positivo (auditoria de 2026-11-25)

"Concluído" pela Lia = suíte **visível** passou depois da última edição. Isso
**não** prova que o requisito foi resolvido (a avaliação mostrou implementações
que passaram na suíte e reprovaram na verificação oculta). Por isso o
`lia_treino.py` separa:

| rótulo | condição | destino |
|---|---|---|
| `positivo` | aplicada, tarefa concluída com validação depois do último patch **e** requisito confirmado por `verificacao` do tipo `independente` (oráculo executado fora do agente, com `oraculo_sha256` e `referencia`) ou `revisao_humana` (revisor como papel, data, aprovado) | `treino/dev/teste.jsonl` |
| `formato` | o mesmo, mas só com a suíte visível (sem atestado) | `formato.jsonl` — serve para aprender formato/reparo; **não** é prova de requisito resolvido |
| `negativo` | recusada, cortada, não concluída, ou aplicada em tarefa que o oráculo/revisão **reprovou** (falso "concluído") | `negativos.jsonl` |

O runtime só atesta a validação visível; o atestado vem depois, de fora:
`lia_treino.py atestar TRAJ.json --saida COPIA.json --tipo independente --oraculo-sha256 H --referencia AVAL/TAREFA --passou sim`
(grava numa **cópia**; nunca sobrescreve a trajetória original).

## Partições e aptidão

A partição é por repositório (`sha256(semente|repo) % 10`: 0 teste, 1 dev,
resto treino). Com poucos repositórios ela pode sair vazia, o que **não** mede
generalização. O manifesto agora traz `particoes` (repositórios e positivos
por partição), `avisos`, `avaliacao_de_generalizacao` e `apto_para_adaptador`.
Fica "não apto" enquanto houver qualquer aviso: partição dev/teste vazia ou
sem positivos, treino com menos de `MIN_REPOS_TREINO` (5) repositórios ou
menos de `MIN_POSITIVOS_TREINO` (200) positivos confirmados (limiares fixados
no código; `construir --exigir-aptidao` sai com código 4 se não apto).

## Conjuntos congelados fora do treino

Recusa (a) enunciado igual ou parecido (Jaccard ≥ 0,6), (b) ≥ 2 identificadores
de código em comum (`normalise_phone`, `core.py`...), (c) trajetória do
**repositório** de qualquer tarefa congelada. **Limites** (também no
manifesto): não vê paráfrase semântica sem palavras em comum, tradução, a
mesma ideia em outro projeto, nem compara o código dos artefatos; os limiares
nunca foram calibrados com exemplos reais.

## Pré-requisitos (nada disso é instalado sem autorização)

1. **Dados**: ≥ algumas centenas de trajetórias **positivas** verificadas
   (`lia-trajetoria/1`, com licença e proveniência), de vários
   repositórios e linguagens; os conjuntos congelados de avaliação nunca
   entram.
2. **Base**: modelo aberto com licença compatível e bom desempenho de
   código **medido na Lia** (não "uncensored" por si só). Candidatas da
   classe 1,5–7 B: Qwen2.5-Coder-1.5B/3B/7B-Instruct (Apache-2.0 nas
   versões oficiais; conferir a licença de cada derivado). Registrar
   tokenizer e template de conversa (Qwen: chatml).
3. **Hardware**: GPU CUDA com ≥ 12 GB para QLoRA de 1,5–3 B (≥ 24 GB
   para 7 B com folga), ou máquina alugada — custo e envio de código para
   fora exigem autorização explícita.

## Receita

```sh
# 1. conjunto (aqui mesmo, sem dependências)
python3 ferramentas/treino/lia_treino.py construir \
  --entrada ~/.lia/trajetorias --saida dados/v1 \
  --base Qwen2.5-Coder-1.5B-Instruct --tokenizer Qwen2.5-Coder-1.5B-Instruct --template chatml \
  --semente 1 --congelados docs/avaliacao/*.json \
  --hiper r=16 alpha=32 dropout=0.05 lr=2e-4 epocas=2 max_len=4096
# confira dados/v1/manifesto.json (hash do conjunto, licenças, partições)

# 2. na máquina com GPU (ambiente isolado; versões registradas no manifesto do treino)
python3 -m venv venv && . venv/bin/activate
pip install torch transformers peft trl datasets bitsandbytes   # versões fixadas no manifesto
#   treino QLoRA (SFT) só com dados/v1/treino.jsonl; dev.jsonl para parada antecipada;
#   semente, versão da base (hash), hiperparâmetros e GPU/horas vão para o manifesto do treino

# 3. converter e servir (llama.cpp)
python3 convert_lora_to_gguf.py adaptador/ --base base_hf/ --outfile adaptador.gguf
llama-server -m base.gguf --lora adaptador.gguf --device none -ngl 0 -c 8192 -np 1 --port 8099

# 4. avaliar no holdout (≥ 3 execuções) contra a base e o baseline sem modelo
python3 ferramentas/avaliacao/avaliar.py --tarefas docs/avaliacao/2026-11-24-dev-holdout.json \
  --id ho-sf-1 --condicao llamacpp:8099:base+adaptador --rep 1 --temperatura 0.4 --semente 1 --saida resultados/
```

## Promoção

Só pelo critério fixado em
`docs/avaliacao/2026-11-24-criterio-de-promocao.md` (ganho no holdout
sobre a base **e** o baseline, zero "concluído" falso, zero violação), com
ADR e manifesto `lia-modelo/1` validado por
`lia_treino.py validar-modelo`. Sem ganho, o padrão não muda.
