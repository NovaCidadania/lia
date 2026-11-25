# Auditoria ao vivo da avaliação de coding da Lia

**Estado:** avaliação `independente-v1` interrompida por defeito reproduzível no harness.  
**Data da auditoria:** 2026-11-25, fuso `America/Sao_Paulo`.  
**Commit-base observado:** `5d7ebdc`.  
**Escopo:** auditar a matriz 3 concluída, o início de `independente-v1`, os eventos registrados e o fluxo do harness. Não foram alterados os repositórios de fixture nem os processos da avaliação.

## Resumo executivo

A matriz 3 terminou com **3 sucessos estritos em 24 execuções**, todos com Qwen, além de **1 falso concluído**. O llama3.2:3b teve 0/12 sucessos; o Qwen teve 3/12. Isso mostra que a Lia consegue completar algumas tarefas pequenas e bem delimitadas, mas a taxa é baixa, dependente de semente e ainda sujeita a falso positivo.

A avaliação seguinte `independente-v1` não pode ser interpretada como uma matriz normal neste momento. O avaliador Python não trata uma mensagem `proposta` que contém `plano_proposto`: só envia aprovação quando recebe evento `plano`. Em `ind-py-2`, o daemon emitiu a proposta com plano, o avaliador não a solicitou/aprovou e esperou os 40 minutos completos. O mesmo aconteceu na repetição seguinte. Esse é um defeito do harness, não evidência de falha do modelo. A matriz nova deve ser corrigida e reiniciada antes de comparar modelos.

Também foi observado um defeito de política de contexto: a regra derivada de “não leia emails.txt” bloqueou `emails_send.py` por correspondência ampla `*email*`, escondendo código-fonte relevante. Proibições sobre arquivos de dados devem ser exatas e não podem bloquear módulos por substring.

## Resultados confirmados da matriz 3

Diretório de resultados: `~/.lia/avaliacao-resultados/matriz-3`. Foram auditados 24 JSONs: 24 combinações esperadas, sem duplicatas e com JSON válido.

| Condição | Execuções | Sucesso estrito | Não concluído | Concluído falso |
|---|---:|---:|---:|---:|
| llama3.2:3b | 12 | 0 | 12 | 0 |
| Qwen2.5-Coder-7B Q5_K_M | 12 | 3 | 8 | 1 |
| **Total** | **24** | **3** | **20** | **1** |

Por tarefa, Qwen concluiu `ho-sf-1` em 1/3 sementes e `ho-le-1` em 2/3. `ho-sf-2` e `ho-le-2` ficaram em 0/3. O caso de falso concluído em `ho-sf-1` passou a suíte visível e tocou o alvo, mas o oráculo oculto reprovou: o teste gerado não cobriu os requisitos reais. Portanto, suíte verde mais teste criado não bastam como prova de conclusão.

O ciclo limitado de reparo mostrou comportamento útil de segurança: no `ho-sf-1` r3, duas edições falharam nos testes; depois o modelo repetiu uma proposta idêntica e o runtime encerrou como `NÃO concluído`, sem alegar sucesso. Porém, as tentativas rejeitadas não mantêm o conteúdo bruto em uma trilha de auditoria recuperável, o que dificulta separar erro do modelo de falso negativo do verificador.

## Auditoria de ações observadas

### `ind-py-1`, baseline sem modelo, repetição 1

O log registra: `contexto` → leitura de `inventario/precos.py` e `tests/test_precos.py` → critérios determinísticos → duas propostas idênticas sem edições → `NÃO concluído` por `proposta_invalida`, em 5 s.

**Avaliação:** fluxo curto, observável e honesto. A duplicação da tentativa sem progresso foi detectada. O baseline não deve ser interpretado como tentativa de coding pelo modelo; serve para medir o comportamento sem modelo.

### `ind-py-2`, baseline sem modelo, repetição 1

O clone permaneceu sem alterações. O daemon registrou que a pergunta virou tarefa planejada `arquivo_alterar`, um passo. O avaliador recebeu uma mensagem `proposta`, não enviou `solicitar` nem aprovação e terminou após 2.404,9 s com `sem resposta final no limite`; não houve tentativas, diff ou oráculo.

**Avaliação:** falha do harness. O tempo de 40 minutos não mede raciocínio nem desempenho da Lia.

### `ind-py-2`, baseline sem modelo, repetição 2

No checkpoint de 10:18, o mesmo caso repetia o mesmo estado: 40 minutos, uma mensagem `proposta`, nenhum evento de conclusão e daemon ainda vivo. A repetição confirma que não é uma lentidão ocasional.

### Outras tarefas baseline já registradas

Os resultados `ind-py-3`, `ind-js-1`, `ind-js-2`, `ind-js-3` e `ind-py-1` r2 encerraram em 5 s com `proposta_invalida`, após duas propostas vazias repetidas. Isso é esperado para o mock sem roteiro, embora seja importante manter a categoria `baseline sem modelo` separada das falhas de um modelo real.

No checkpoint de 10:18 havia 7 de 54 resultados JSON. Nenhuma execução Ollama/Qwen desta nova matriz havia começado; não há ainda inferência sobre os modelos neste conjunto.

## Causa técnica do timeout

Em `sessiond/sessiond.c`, `enviar_proposta_tarefa()` envia `LIA_MSG_PROPOSTA` com `plano_proposto` quando existe plano. O loop em `ferramentas/avaliacao/avaliar.py`:

- processa `m["tipo"] == "plano"` e então envia `aprovar`;
- só processa `m["tipo"] == "proposta"` como terminal quando não há `codigo` nem `plano_proposto`;
- não trata `proposta` com `plano_proposto` para mandar `solicitar` ou seguir o protocolo de aprovação.

Assim, a conexão fica aberta, mas o avaliador não avança o handshake. O runner (`independente-v1.sh`) usa `set -u`, não falha quando um caso retorna código de erro e continua; isso faz a matriz parecer avançar, mas não protege contra resultados incompletos. O diretório continha um `matriz.log` de poucas linhas e resultados individuais, sem progresso estruturado por etapa.

## Problemas priorizados

### P0 — Corrigir o protocolo do harness e invalidar execuções contaminadas

1. Tratar `proposta` com `plano_proposto` conforme o protocolo real da sessão: solicitar os passos e então aprovar/rejeitar cada plano, associando o hash e escopo corretos.
2. Acrescentar teste de integração do avaliador cobrindo a sequência `proposta(plano_proposto)` → `plano` → `aprovar` → `resultado` → `resposta`.
3. Fazer o harness encerrar imediatamente com erro explícito se receber um tipo de evento inesperado ou ficar sem progresso por um limite curto configurável; não esperar 40 minutos por um handshake ausente.
4. Marcar as duas execuções `ind-py-2` sem modelo como `harness_protocol_error`, não como falhas do modelo. Não reutilizá-las em agregações.
5. Não rodar as 54 execuções restantes com o avaliador defeituoso. Corrigir primeiro, depois criar uma nova execução identificável e repetir todas as condições, sementes e tarefas desde o começo.

### P1 — Política de contexto e integridade das medições

1. Substituir bloqueio por substring (`*email*`) por caminhos proibidos normalizados e exatos; separar claramente arquivos de dados sensíveis de módulos-fonte.
2. Preservar, opcionalmente e com permissões restritas, propostas rejeitadas de fixtures sintéticas, junto com contexto-hash, razão do verificador e tentativa. Isso permite auditar falsos negativos sem gravar código real sensível por padrão.
3. Fazer o runner usar `set -euo pipefail`, validar o código de saída e o schema do JSON antes de aceitar resultado ou imprimir `FIM`.
4. Escrever cada resultado de forma atômica e só pular arquivos já existentes após conferir tarefa, condição, semente, hash de modelo, commit-base, versão do avaliador e estado terminal.
5. Adicionar um manifesto final que confirme exatamente as 54 combinações e classifique ausências/erros; `FIM` sozinho não é evidência de completude.

### P1 — Qualidade de conclusão e reparo

1. Vincular cada requisito explícito a pelo menos um teste observável. Um teste novo só conta se exercitar os casos de aceitação, não apenas existir.
2. Manter o oráculo independente como condição obrigatória de sucesso na avaliação; nunca promover uma trajetória só porque a suíte visível passa.
3. Exibir diagnóstico de falha estruturado ao modelo no ciclo seguinte (arquivo, teste, traceback e trecho mínimo), sem reenviar contexto integral nem aceitar repetição idêntica.
4. Distinguir falha de proposta, recusa do verificador, falha de compilação/teste, timeout do backend, defeito do harness e falha do oráculo nos relatórios e métricas.
5. Manter limite de ciclos e encerramento honesto. Os resultados Qwen mostram que isso evita loops e falsos relatos, mas não substitui melhorar o patch.

### P2 — Evidência e promoção de modelos

1. O conjunto `independente-v1` foi escrito e oraculado pelo mesmo mantenedor do runtime. Apesar do nome, é diagnóstico, não avaliação independente nem holdout cego.
2. Solicitar a uma pessoa independente tarefas e oráculos novos, congelados e ocultos, antes de qualquer promoção.
3. Rodar ao menos três sementes por tarefa e condição, registrar temperatura, formato de chat, backend, hash de pesos e todos os tempos; comparar com baseline sem modelo.
4. Não iniciar fine-tuning com o conjunto atual: o Qwen é um GGUF derivado de terceiros sem licença declarada, e a matriz tem poucos sucessos e um falso concluído. Exigir proveniência/licença e dados de treino com resultado de validação independente.
5. Não chamar a arquitetura de “LLM própria” ainda. O que há é runtime agentivo com modelos externos locais; nenhum modelo foi treinado nesta avaliação.

## Estado de segurança e alterações

As avaliações usam clones em `/tmp/lia-aval` e verificações ocultas fora dos repositórios. Na matriz 3, os cinco repositórios originais ficaram limpos, sem commit ou push. A avaliação `independente-v1` também opera em clones; neste checkpoint não se observou escrita no clone do caso travado.

O repositório Lia tinha alterações preexistentes não commitadas em `docs/TREINO.md`, `ferramentas/avaliacao/avaliar.py`, `ferramentas/treino/lia_treino.py` e `testes/treino/teste_pipeline.py`, além de arquivos novos da rodada. Foram preservadas; esta auditoria não editou essas fontes. O relatório é um novo documento de avaliação.

## Acompanhamento pendente

No checkpoint de 10:49, havia 13/54 resultados. Os casos baseline concluídos eram 5 s, com exceção de `ind-py-2` nas sementes 1 e 2, cada uma encerrada pelo timeout de 2.405 s sem resposta. A semente 3 acabara de começar. Como a falha de handshake repetiu-se e invalida a medição, o runner foi encerrado com `SIGTERM`; o `lia-sessiond` órfão daquela execução também foi encerrado. Não houve edição de código-fonte durante o monitoramento. O servidor `llama-server` Qwen (PID 8754) não fazia parte do grupo do avaliador e permaneceu ativo.

Os 13 JSONs existentes são resultados parciais e não formam uma matriz completa. Não interpretar nem agregar como comparação entre modelos. Antes de uma nova rodada:

1. Corrigir e testar o handshake do avaliador, inclusive a proposta com `plano_proposto`.
2. Adicionar timeout de progresso e limpeza de toda a árvore de processos, inclusive da sessão criada com `start_new_session=True`.
3. Rodar a suíte de integração do harness em um clone mínimo e confirmar que cada caso produz resultado terminal em segundos.
4. Usar um novo diretório de resultados e um identificador/versionamento novo do avaliador; não retomar estes 13 arquivos como se fossem uma matriz limpa.
5. Só então repetir as 54 combinações e realizar novos checkpoints de 30 minutos.
