# Prova de conceito da GUI — levantamento e decisão

Referenciado pelo PLANO.md §4.1 e §10.7. A GUI é uma **camada fina** sobre o
protocolo em [docs/protocolo.md](protocolo.md): não executa nada, não guarda
nenhuma lógica de autorização, só fala NDJSON com um daemon (que ainda não
existe — ver seção 5). Diferente do núcleo (`nucleo/`, `runtime/`), a GUI roda
como processo comum, com libc e bibliotecas dinâmicas normais: o contrato
"sem libc" do PLANO.md §2.1/regra 1 vale para `nucleo/`/`runtime/`, não para a
camada de apresentação, que é explicitamente separada (ver §4.1).

## 1. Levantamento local (nesta máquina, Arch Linux x86-64)

Tudo abaixo já estava instalado nesta máquina (nenhuma dependência nova foi
instalada para este levantamento — `pacman -Q`/`pkg-config` apenas consultam
o que já existe).

| Opção | Versão instalada | Linguagem nativa | Licença | Runtime/deps principais |
|---|---|---|---|---|
| SDL2 (via sdl2-compat) | 2.32.74 | C | Zlib | libSDL2, sem toolkit de widgets embutido |
| SDL3 | 3.4.18 | C | Zlib | libSDL3, idem — widgets não inclusos |
| GTK3 | 3.24.52 | C (GObject) | LGPL-2.1-or-later | glib, gobject, pango, cairo, gdk-pixbuf |
| GTK4 | 4.24.1 | C (GObject) | LGPL-2.1-or-later | glib, gobject, pango, cairo, gdk-pixbuf, graphene, (vulkan opcional) |
| Qt6 (qt6-base) | 6.12.0 | C++ (moc) | GPL-3.0 / LGPL-3.0 / comercial | QtCore, QtGui, QtWidgets, moc/qmake ou CMake |
| Qt5 | — | — | — | **não instalado** nesta máquina |
| Wayland (libwayland-client) | 1.26.0 | C | MIT | protocolo de compositor, sem widgets |
| X11 (libX11) | 1.8.14 | C | MIT/X11 | Xlib pura, sem widgets, legado |

Não há toolkit de widgets acessível de outra forma (ex.: FLTK, Dear ImGui, Tk)
instalado nesta máquina — não foram considerados, para não introduzir uma
dependência nova sem necessidade (ver §4 da tarefa original).

### Comentário por opção

- **SDL2/SDL3**: ótima portabilidade (Linux, Windows, macOS, e com caminho
  conhecido para Android/iOS/consoles), baixo overhead, mas **sem widgets nem
  acessibilidade nativos** — um campo de texto, botão ou leitor de tela
  exigiria construir (ou importar, ex. Dear ImGui) uma camada de UI própria.
  Ótimo candidato para a fase 7 do PLANO.md (controle de tela/mouse/teclado,
  captura de tela), ruim como primeira GUI de texto/aprovação acessível.
- **GTK3/GTK4**: widgets nativos completos (texto, botões, listas), **acessibilidade
  embutida** via ATK/AT-SPI (leitores de tela Linux falam com GTK nativamente),
  navegação por teclado de fábrica, renderização adaptada a DPI/escala. Escrito
  em C puro (GObject), mesma linguagem do resto do projeto (`runtime/`, `cli/`) —
  não introduz um segundo compilador/toolchain (diferente de Qt, que exige C++
  e `moc`). Portabilidade: Linux (nativo), Windows (suportado, usado por GIMP/
  Inkscape), macOS (suportado via GTK-OSX, menos polido); **sem história de
  mobile** (Android/iOS) — gap conhecido, documentado na seção 3.
- **Qt6**: a melhor portabilidade de longo prazo, inclusive mobile (Android/
  iOS oficialmente suportados) e embarcado. Trade-offs reais para esta etapa:
  C++ (segunda linguagem no projeto, hoje 100% C/Assembly), build com `moc`/
  `qmake`/CMake (o `Makefile` atual é simples, sem geração de código), e
  licenciamento LGPL-3.0/GPL-3.0/comercial — mais restritivo e mais lido com
  atenção do que a LGPL-2.1 do GTK se a Lia algum dia for distribuída de forma
  fechada ou vinculada estaticamente.
- **Wayland/X11 puros**: nível de protocolo de janela, não de widget — construir
  uma GUI direto sobre eles significa reimplementar o que GTK/Qt já resolvem
  (texto, foco, acessibilidade, DPI). Fora de escopo para esta PoC; útil só se
  um dia a Lia precisar de um compositor ou controle de tela de baixo nível
  (fase 7, controle de hardware).

## 2. Critérios e decisão

Critérios da tarefa, na ordem em que pesaram:

1. **Linux x86-64 primeiro** — todas as opções atendem.
2. **AArch64/Windows/macOS/mobile no futuro** — Qt6 vence em mobile; GTK vence
   em simplicidade de linguagem/build; SDL vence em portabilidade bruta mas
   não resolve widgets/acessibilidade.
3. **Baixo overhead** — GTK4 e SDL são comparáveis (uso de GPU, inicialização
   rápida); Qt6 é similar em runtime, mas maior em superfície de build.
4. **Acessibilidade** — GTK (ATK/AT-SPI nativo) e Qt (QAccessible nativo) atendem
   bem; SDL não atende sem trabalho extra.
5. **Teclado, mouse, telas pequenas e alta resolução** — GTK4 e Qt6 atendem de
   fábrica (escala por DPI, foco por teclado, layout responsivo); SDL exigiria
   implementar cada um desses manualmente.
6. **Integração com CLI/protocolo NDJSON** — qualquer uma serve (é só um socket
   Unix lido em um loop de eventos); GTK4 tem `GSocketClient`/`GIOStream` nativo
   no mesmo GLib que já roda o *main loop* da aplicação, o que evita misturar
   duas bibliotecas de I/O assíncrono (ex.: Qt teria `QLocalSocket`, igualmente
   nativo, então este critério não desempata GTK vs. Qt por si só).
7. **Nenhuma lógica privilegiada na GUI** — independe do toolkit; é uma decisão
   de arquitetura (seção 5), não de biblioteca.

### Decisão: **GTK4**, para esta prova de conceito

Razões decisivas: (a) C puro, mesma linguagem do resto do repositório — zero
toolchain nova além de `pkg-config`/`gcc`, que já são usados; (b) acessibilidade
e widgets completos de fábrica, exigidos explicitamente pela tarefa; (c) já
instalado nesta máquina, então a PoC pode ser validada de verdade agora, sem
pedir para instalar nada (regra da tarefa: "não instale dependências
automaticamente"); (d) licença LGPL-2.1, compatível com vinculação dinâmica
sem obrigações adicionais de distribuição de código-fonte da Lia.

**Gap aceito e documentado**: GTK não tem história de mobile. Isso não é um
problema agora (a fase 7 do PLANO.md, que trata de mobile/hardware, está longe),
mas é uma decisão a revisitar explicitamente se/quando mobile se tornar
prioridade — não um esquecimento. Se isso acontecer, a arquitetura em camadas
desta PoC (seção 5: protocolo isolado do toolkit) permite trocar GTK por Qt ou
por um frontend web/Flutter sem tocar na lógica de parsing/validação do
protocolo, porque ela já está isolada em `gui/protocolo.c`, sem nenhuma
dependência de GTK.

### Quando revisitar

- Se mobile (Android/iOS) entrar no roteiro antes do previsto → reavaliar Qt6
  ou um frontend web (a camada de protocolo já serializaria para qualquer um).
- Se a licença LGPL-2.1 do GTK se tornar um problema de distribuição → mesma
  resposta: só a camada de apresentação muda, não `gui/protocolo.c`.
- Se a fase 7 (controle de teclado/mouse/tela) chegar → SDL2/SDL3 entra como
  biblioteca adicional *para aquele propósito específico*, não como substituto
  do GTK para a GUI de conversa/aprovação.

## 3. Estrutura da PoC implementada

```
gui/
├── protocolo.h      # modelo de mensagem NDJSON (envelope + tipos da seção 3
│                    # de docs/protocolo.md) e funções puras de parsing/validação
├── protocolo.c       # implementação; zero dependência de GTK — testável sem display
├── lia-gui.c          # janela GTK4: conversa, entrada, painel de eventos,
│                    # painel de permissões, cancelar, indicador de estado
└── teste_protocolo.c  # testes de gui/protocolo.c (make teste-gui)
```

Build e execução: ver README.md e `make gui`/`make teste-gui`/`make gui-run`.

## 4. O que esta PoC explicitamente não faz

- Não executa nenhum comando real: não há daemon (`~/.lia/maquina/estado/sessao.sock`
  não existe ainda); a GUI detecta a ausência do socket e mostra "desconectado",
  sem travar nem simular uma conexão.
- Não envia `solicitar` (pedido de execução/simulação de ferramenta) — o campo
  de entrada existe na interface (para validar o layout completo pedido), mas
  está desabilitado nesta etapa ("somente leitura"); só envia `cancelar` quando
  conectado e há uma execução em andamento, e `assinar` ao conectar (para
  receber eventos — não tem efeito sobre o sistema).
- Não chama `sudo`, `pkexec`, `system`, `execve`/`exec*`, nem qualquer API
  equivalente — confirmado por teste automatizado (ver seção de testes do
  README) que varre os binários de `gui/` por essas referências.
- Não decide capacidades, risco ou aprovação: só exibe o que o daemon envia no
  evento `plano`; se não há daemon, não há nada para exibir além do estado
  "desconectado".
