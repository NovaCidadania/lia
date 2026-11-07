# Protocolo de eventos — CLI e GUI

Especificação do canal compartilhado entre a CLI, a GUI e o daemon de sessão da Lia (seção 4.1 do [PLANO.md](../PLANO.md)). A GUI é uma camada fina sobre este mesmo protocolo: nenhuma lógica de execução, autorização ou auditoria é duplicada nela.

> Status: especificação para a fase **0.5**. Ainda não há daemon; a CLI hoje imprime o resultado final de cada comando (texto humano ou `--json`) sem streaming de eventos. Já existe uma primeira implementação de **cliente**: a GUI de prova de conceito (`gui/`, somente leitura — ver [GUI-POC.md](GUI-POC.md)), que interpreta este protocolo (`gui/protocolo.c`) e tenta se conectar ao socket da seção 1; sem daemon, ela fica em "modo sem servidor" (desconectada, de forma visível, nunca travada ou simulando dados). Este documento continua definindo o contrato completo para quando o daemon existir do outro lado.

## 1. Transporte

- Socket Unix local em `~/.lia/maquina/estado/sessao.sock`, modo `0600`, acessível apenas pelo usuário do sistema que é dono de `~/.lia`.
- Sem rede, sem TCP por padrão. Um transporte remoto (rede Lia, fase 10) é uma extensão explícita e separada deste protocolo, nunca o mesmo socket.
- Autenticação do cliente por credencial de socket Unix (`SO_PEERCRED`): a GUI e a CLI só conversam com um daemon que roda com o mesmo uid, ou um uid explicitamente autorizado pela pessoa.
- Enquadramento: uma mensagem JSON por linha (NDJSON), UTF-8, sem aninhamento de frames binários nesta versão.

## 2. Envelope

Toda mensagem, em qualquer direção, tem este envelope:

```json
{
  "protocolo": "lia.v1",
  "tipo": "...",
  "id": "01J...",
  "em_resposta_a": null,
  "ts": "2026-11-07T10:00:00Z",
  "dados": {}
}
```

- `id`: identificador único da mensagem (ULID/UUID), gerado por quem envia.
- `em_resposta_a`: `id` da mensagem que esta responde ou continua; `null` para o primeiro evento de uma sequência.
- `ts`: data/hora em UTC, formato RFC 3339.
- `dados`: payload específico de `tipo`, descrito abaixo.

Uma versão incompatível do protocolo é rejeitada explicitamente (`erro` com `codigo: "protocolo_incompativel"`), nunca interpretada de forma aproximada.

## 3. Tipos de mensagem

### Cliente → daemon

| `tipo` | `dados` | Efeito |
|---|---|---|
| `solicitar` | `{ "ferramenta": "tools.discover", "parametros": {...}, "modo": "simular" \| "executar" }` | Pede a execução (ou simulação) de uma ferramenta do manifesto. |
| `aprovar` | `{ "plano_id": "...", "decisao": "aprovar" \| "rejeitar", "escopo": "uma_vez" \| "sessao" }` | Resposta humana a um `plano` pendente. |
| `cancelar` | `{ "execucao_id": "..." }` | Pede cancelamento cooperativo de uma execução em andamento. |
| `assinar` | `{ "canais": ["auditoria", "execucao"] }` | Assina um fluxo de eventos (ex.: a GUI assina `auditoria` para mostrar o histórico ao vivo). |

### Daemon → cliente

| `tipo` | `dados` | Significado |
|---|---|---|
| `plano` | `{ "plano_id", "ferramenta", "parametros_normalizados", "capacidades", "risco", "efeitos", "reversivel", "requer_aprovacao": bool }` | O executor valida o manifesto e descreve o que faria, **antes** de qualquer efeito. |
| `progresso` | `{ "execucao_id", "fracao": 0.0–1.0, "mensagem": "..." }` | Atualização opcional durante uma execução longa. |
| `saida` | `{ "execucao_id", "fluxo": "stdout" \| "stderr", "trecho": "..." }` | Saída incremental de uma ferramenta que produz texto. |
| `resultado` | `{ "execucao_id", "estado": "sucesso" \| "falha" \| "cancelado", "codigo_saida": int, "dados": {...} }` | Resultado final, sempre emitido mesmo em erro ou cancelamento. |
| `auditoria` | `{ "entrada_id", "ts", "identidade", "ferramenta", "parametros_normalizados", "capacidades", "resultado_resumo", "reversao_disponivel": bool }` | Eco do registro gravado em `~/.lia/humano/auditoria/`; é sempre gravado antes de ser emitido. |
| `erro` | `{ "codigo", "mensagem", "detalhe" }` | Falha do protocolo ou do daemon (não confundir com `resultado.estado = "falha"`, que é falha da ferramenta). |

## 4. Ciclo de vida de uma ação com efeito

```
cliente                     daemon
   | solicitar(executar)  →   |
   |                       plano (risco, capacidades, requer_aprovacao)
   |  ←  plano               |
   | aprovar/rejeitar      →  |
   |                       (se aprovado) executa via executor de capacidades
   |  ←  progresso* saida*   |
   |  ←  resultado            |
   |  ←  auditoria            |
```

- `requer_aprovacao = false` só é permitido para ferramentas classificadas `leitura` sem efeito (seção 5). Qualquer outro risco exige o passo `aprovar`, mesmo que uma política do usuário responda automaticamente — a política fica registrada como a autorização usada, nunca como ausência de autorização.
- `resultado` é emitido mesmo se o cliente desconectar antes: a execução não fica "solta" sem fechamento.
- `auditoria` é sempre emitido depois de gravado em disco, nunca antes.

## 5. Erros

Códigos estáveis, para a GUI e a CLI tratarem sem regex em mensagem humana:

| `codigo` | Quando |
|---|---|
| `protocolo_incompativel` | Versão de `protocolo` não suportada. |
| `ferramenta_desconhecida` | `ferramenta` não está no manifesto carregado. |
| `parametro_invalido` | Falha de validação de tipo/limite do manifesto. |
| `capacidade_negada` | A política do usuário não concede a capacidade exigida. |
| `aprovacao_pendente` | Uma ação com efeito foi solicitada sem passar por `aprovar`. |
| `cancelado` | Execução interrompida por `cancelar`. |
| `limite_excedido` | Timeout, limite de saída ou de recursos do manifesto foi atingido. |

## 6. Relação com a CLI de hoje

Os comandos já implementados (`lia tools discover`, `lia tools list [--json]`, `lia tools probe --safe`, `lia shell`, `lia sudo`, `lia root`) continuam funcionando como processos de execução única, sem o socket. Quando o daemon existir, a CLI passa a ser um cliente fino deste protocolo (como a GUI), mantendo os mesmos nomes de ferramenta do manifesto (seção [manifesto.md](manifesto.md)) e a mesma saída `--json` como payload de `resultado.dados`.
