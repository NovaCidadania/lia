# Coding mínimo da Lia — arquitetura

Status: proposta em vigor a partir de 2026-11-24 (ADR: `docs/adr/0001-coding-minimo-runtime.md`).
Evolução no mesmo dia — contrato por operação, truncamento com estado
próprio, árvore de tarefas com evidências, trajetórias e promoção:
`docs/LIA-CODING-ENGINE.md` e `docs/adr/0002-lia-coding-engine.md`.

A Lia não tenta guardar "todo o conhecimento de programação" num modelo
pequeno. O **runtime** carrega o método de trabalho; o modelo executa
operações curtas dentro dele. O objetivo é **pouco conhecimento por
tarefa**, não zero conhecimento: o modelo ainda precisa escrever código
plausível, mas recebe um problema pequeno, contexto certo e uma prova
executável como juiz.

```
pedido
  → contrato de resultado verificável      (runtime: critérios do pedido + validação)
  → leitura seletiva do projeto            (runtime: regras; modelo só se as regras não acharem nada)
  → pequena proposta de patch              (modelo, saída estruturada; ou receita determinística)
  → verificador + diff + aprovação         (runtime + pessoa)
  → build/teste isolado como evidência     (runtime, com aprovação)
  → correção limitada ou encerramento honesto (runtime decide; modelo só corrige a partir do diagnóstico)
```

## 1. Quem decide o quê

| Responsabilidade | Runtime (determinístico) | Modelo pequeno |
|---|---|---|
| Estado da tarefa, etapas, prazo, checkpoints | **sim** | não |
| Autorização (aprovação presa a hash, proibições do pedido) | **sim** | não |
| Descobrir linguagem, comandos de teste e arquivos relevantes | **sim** (exploração, `class/def/function <símbolo>`, testes vizinhos, `.lia/validacao.json`, Makefile) | só escolhe da lista quando as regras não acham nada |
| Distinguir caminho pedido de caminho mencionado numa proibição | **sim** (`coding_caminhos_proibidos`) | não |
| Critérios de sucesso | **sim**, do pedido e da validação | pode **sugerir**; o verificador filtra |
| Patch | verificador: só arquivos lidos, trecho único, dentro do projeto e fora das proibições; uma gravação por arquivo presa ao hash lido | propõe a menor mudança (JSON com esquema) |
| Execução de build/testes | **sim**, isolada (bubblewrap, sem rede, sem /home), com aprovação | não |
| Interpretar falha | resume a saída (linhas-chave de erro) | sugere a correção |
| Declarar "concluído" | **só o runtime**: diff real + validação depois da última escrita + critérios conferidos | nunca |
| Pedido ambíguo / fora da capacidade | pergunta (escopo, conteúdo literal) ou encerra com motivo | — |

O modelo nunca decide sozinho que terminou. Compilador, testes, diff e
verificadores determinísticos fornecem a evidência.

## 2. Etapas e o que cada uma recebe

Cada chamada ao modelo recebe **só** o necessário naquele momento:

| Etapa | Entrada | Saída (esquema) | Falha → |
|---|---|---|---|
| contexto (opcional) | pedido + lista de arquivos do projeto | `{"arquivos":[≤4 da lista]}` | encerra: "não localizei o código; cite o arquivo ou a função" |
| critérios (opcional, `LIA_SESSIOND_CODIGO_CRITERIOS_MODELO=1`) | pedido + arquivos lidos | `{"criterios":[≤3]}` | segue com os critérios do pedido |
| proposta | pedido, critérios, proibições, trechos dos arquivos lidos (região dos símbolos em arquivos grandes), boas práticas recuperadas, erro anterior do verificador | `{"edicoes":[criar\|editar\|substituir_funcao],"explicacao"}` | recusa com motivo → nova tentativa (1×) → encerra |
| correção | o mesmo + resumo da falha da validação | idem | até 3 ciclos; falha idêntica = repetição sem progresso → encerra |

Toda chamada tem prazo (`LIA_SESSIOND_CODIGO_TIMEOUT_MS`, padrão 10 min),
batida de progresso visível (15 s) e cancelamento. Desconexão encerra a
tarefa com checkpoint `desconectado`; ela pode ser retomada.

## 3. Proibições no pedido

"Não leia .env", "não altere migrations/", "não execute campaign.py",
"sem tocar em logs, contatos, banco, dumps, credenciais, .git": o caminho
ou a categoria citada numa oração negativa vira padrão proibido. Mencionar
um caminho numa proibição **não** autoriza lê-lo. Arquivos de dados
citados no pedido (`sends.txt`, `.csv`, `.log`) não são lidos para
contexto: o pedido pode descrever o formato de um arquivo real sem
autorizar a leitura dele. O relatório confere o critério "nada que o
pedido proíbe foi lido ou alterado".

## 4. Pedidos ambíguos e fora da capacidade

- Pedido amplo sem arquivo/símbolo ("melhore os controllers e models"):
  esclarecimento + auditoria de escopo limitado, nada alterado.
- Escrita de arquivo sem conteúdo literal fora de um pedido de código:
  pergunta se é texto exato ou mudança de código.
- Contexto não localizado: encerra dizendo o que falta.
- Modelo lento ou inválido: encerra com o motivo (prazo, duas propostas
  inválidas, limite de ciclos), nunca com implementação fictícia.

## 5. Dados de treino a partir do uso

Cada tarefa gera uma **trajetória** auditável — pedido, contexto lido (só
caminhos e hashes), critérios, proposta(s) com origem, decisões do
verificador, aprovações, comandos, resultado da validação. Proposta:

- formato versionado (`lia-trajetoria/1`), um arquivo por tarefa, fora do
  repositório do usuário, gravado **só com opt-in explícito**;
- **licença**: só código do próprio usuário ou com licença que permita uso
  para treino, registrada por trajetória; nada de livros ou código de
  terceiros sem licença;
- **anonimização**: sem `.env`/credenciais (já bloqueados), sem dados
  pessoais, caminhos absolutos trocados por relativos, nomes de usuário
  removidos;
- **rótulo pelo resultado real**: positiva só se a validação passou
  depois da última edição, os critérios foram conferidos e a pessoa
  aprovou o diff; propostas recusadas pelo verificador e correções viram
  exemplos negativos/de reparo, nunca positivos;
- os enunciados do conjunto de avaliação congelado **nunca** entram no
  treino.

## 6. Estratégia de modelo (sem treinar do zero)

Não treinamos um LLM do zero nesta etapa. Pré-treino útil exige ordens de
grandeza mais computação e dados do que este notebook oferece (i7 de 4ª
geração, 8 threads, 16 GB de RAM, GT 750M sem suporte prático a
treinamento moderno). Plano experimental:

1. **Base**: modelo pequeno existente com licença compatível (classe
   1–3 B para o modo offline básico; 7 B quantizado como opção local).
2. **Adaptação (LoRA/QLoRA)** com as trajetórias positivas e de reparo no
   formato exato das etapas (esquemas acima). LoRA reduz os parâmetros
   **treinados** e o tamanho do artefato distribuído, mas **não** elimina
   carregar o modelo base na inferência: o adaptador sempre roda junto da
   base.
3. **Hardware para treinar**: GPU com ≥ 12–24 GB (QLoRA de 1–3 B; 7 B com
   folga maior), alugada ou emprestada; este notebook só **roda** o
   resultado (CPU, quantizado). Registrar GPU, horas, versão da base,
   hiperparâmetros e semente.
4. **Dados mínimos** antes de tentar: algumas centenas de trajetórias
   positivas verificadas, distribuídas por linguagem (C, PHP, Python,
   TypeScript, C#), mais as de reparo.
5. **Avaliação**: sempre no conjunto congelado (§7) contra a base sem
   adaptação, os modelos locais disponíveis e o baseline sem modelo
   (receitas determinísticas + encerramento honesto).

## 7. Plano progressivo

| Fase | Entrega | Critério para avançar |
|---|---|---|
| A. Runtime determinístico com o modelo atual | contexto por regras, critérios do pedido, proibições, prazo, checkpoints coerentes, relatório com origem; contrato por operação, truncamento, árvore com evidências (feito em 2026-11-24) | regressões automatizadas passando; nenhum "concluído" falso |
| B. Conjunto de avaliação congelado | enunciados por projeto/linguagem (preciso + aberto), commit-base, comando de validação, critério de sucesso estrito; guardado com hash | enunciados congelados antes de qualquer execução |
| C. Coleta de trajetórias | opt-in, formato versionado, licença e anonimização (formato, exportação opt-in e pipeline prontos em 2026-11-24; zero trajetórias coletadas) | ≥ algumas centenas de positivas verificadas |
| D. Adaptação de modelo pequeno | LoRA/QLoRA sobre base existente | treino reprodutível (semente, versão, dados com hash) |
| E. Comparação cega | modelo adaptado × base × modelos locais (llama3.2:3b, Qwen2.5-Coder-7B) × baseline sem modelo, no conjunto congelado | relatório com sucesso estrito, falsos "concluído", tempo e retrabalho |
| F. Implantação | só se aumentar o sucesso estrito **sem** aumentar falsos "concluído" | decisão registrada em ADR |

## 8. Offline e outros backends

O funcionamento offline continua sendo o padrão (mock, Ollama ou
llama.cpp locais). Outros backends são opcionais e explícitos: escolhidos
por configuração, registrados no relatório ("Modelo: …") e sujeitos à
política de privacidade do projeto — nenhum código sai da máquina sem que
a configuração autorize esse backend.

## 9. Limites conhecidos

- A qualidade do patch ainda é do modelo. Com llama3.2:3b em CPU (~7
  tokens/s, prompt de 1,4 mil tokens em ~50 s), uma proposta leva minutos
  e frequentemente falha no verificador. O runtime garante que essa falha
  é visível e honesta, não que ela não aconteça.
- A seleção de contexto por regras depende de caminho ou símbolo no
  pedido; pedido só em linguagem natural cai na escolha pelo modelo.
- Critérios do pedido são conservadores (validação, arquivo criado,
  constante exigida, proibições); comportamento fino depende dos testes do
  projeto.
