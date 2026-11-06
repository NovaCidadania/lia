# Lia

**Lia** é uma IA feita para rodar **localmente** em computadores pessoais: inferência, treinamento e memória na própria máquina, com postura **agêntica** completa — ela assume tarefas no PC, opera o sistema operacional e conversa com o hardware.

> Status: **início da implementação.** Veja o [PLANO.md](PLANO.md).

## Como usar hoje (Linux x86-64)

```sh
make            # compila build/lia e build/lia-cat (estáticos, sem libc)
make teste      # testes reais nesta máquina

build/lia tools discover        # inventário dos executáveis do sistema
build/lia tools probe --safe    # testa os que têm probe seguro conhecido
build/lia tools list --estado PROBE_SUCCESS
build/lia shell                 # shell do usuário
build/lia root --status         # root está disponível via sudo?
build/lia root                  # shell de root via sudo (você autentica)
build/lia root -- id -u         # um comando como root
```

## Princípios

1. **Local primeiro.** Nada sai da máquina sem decisão explícita do usuário.
2. **Performance acima de conveniência.** Núcleo em **Assembly**, suporte em **C**, sem dependências pesadas.
3. **Ler e interpretar dados — de verdade.** Lia parte dos bytes, não do texto. Todo arquivo do sistema (PDF, imagem, binário ELF, áudio, planilha, banco de dados) é lido por um decodificador próprio, escrito para aquele formato.
4. **A máquina é o contexto.** Lia prefere caminhos voltados ao sistema em que está instalada (syscalls, `/proc`, `/sys`, dispositivos) a interpretar texto puro.
5. **Poder total, rastreável.** Primeiro objetivo: shell, sudo e root o quanto antes (via `sudo` real, com a autenticação da pessoa). Acesso a root/sudo/Admin, entrada e saída (mouse, teclado, tela), configuração do SO e hardware — com cada ação registrada e auditável.
6. **Aprende e compartilha.** Treinamento local (adaptadores) e troca de conhecimento entre instalações.

## Arquitetura (resumo)

```
 ┌────────────────────────────────────────────────────────────┐
 │ L6  Rede Lia      compartilhamento entre instalações       │
 │ L5  Treino        ajuste fino local (LoRA/adaptadores)     │
 │ L4  Agente        shell, arquivos, input, privilégios, HW  │
 │ L3  Inferência    Ollama/Llama (início) → motor próprio    │
 │ L2  Formatos      decodificadores puros por tipo de arquivo│
 │ L1  Runtime C     estruturas, alocador, utilidades         │
 │ L0  Núcleo ASM    syscalls, I/O, memória, SIMD             │
 └────────────────────────────────────────────────────────────┘
          contexto próprio em ~/.lia  (humano/ | maquina/)
```

Detalhes de cada camada, fases e critérios de conclusão estão no [PLANO.md](PLANO.md).

## Modelo inicial

O desenvolvimento começa com um modelo **Llama open source já treinado**, servido via **Ollama** em `localhost`. Isso permite construir o núcleo, os decodificadores e o agente enquanto o motor de inferência próprio (GGUF + kernels em Assembly) é desenvolvido em paralelo.

## Plataformas

| Plataforma        | Prioridade |
|-------------------|------------|
| Linux x86-64      | 1ª         |
| Linux AArch64     | 2ª         |
| Windows x86-64    | 3ª         |
| macOS AArch64     | 4ª         |

## Contexto próprio: `~/.lia`

```
~/.lia/
├── humano/    # legível e editável por pessoas (Markdown, TOML, texto)
└── maquina/   # formatos binários próprios da Lia (índices, vetores, caches, pesos)
```

## Estrutura do repositório (planejada)

```
lia/
├── nucleo/        # L0 – Assembly (x86_64/, aarch64/)
├── runtime/       # L1 – C
├── formatos/      # L2 – um decodificador por formato
├── inferencia/    # L3
├── agente/        # L4
├── treino/        # L5
├── rede/          # L6
├── ferramentas/   # utilitários de linha de comando (lia-cat, lia-hex, ...)
├── testes/
└── docs/
```

## Licença

A definir.
