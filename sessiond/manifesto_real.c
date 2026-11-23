#include "manifesto_real.h"
#include <string.h>

static const char *const CAP_LEITURA_PROCESSO[] = { "leitura_arquivo", "processo", NULL };
static const char *const CAP_LEITURA[] = { "leitura_arquivo", NULL };
static const char *const CAP_REDE[] = { "rede", NULL };
static const char *const CAP_LER_PROJETO[] = { "leitura_arquivo", NULL };
static const char *const CAP_ESCREVER_PROJETO[] = { "escrita_projeto", NULL };
static const char *const EOLS[] = { "LF", "CRLF", "CR", NULL };

static const char *const ESTADOS_TOOLS[] = {
    "DISCOVERED", "ACCESSIBLE", "PROBE_SUCCESS", "PROBE_FAILED",
    "PERMISSION_DENIED", "NEEDS_INPUT", "NO_SAFE_PROBE", NULL,
};

static const ParametroManifesto PARAM_LIST[] = {
    { .nome = "estado", .tipo = PARAM_ENUM, .obrigatorio = FALSE, .valores_enum = ESTADOS_TOOLS },
    { .nome = NULL },
};

static const ParametroManifesto PARAM_PROBE[] = {
    { .nome = "safe", .tipo = PARAM_BOOLEANO, .obrigatorio = FALSE, .valores_enum = NULL },
    { .nome = NULL },
};

static const ParametroManifesto PARAM_VAZIO[] = {
    { .nome = NULL },
};

static const ParametroManifesto PARAM_WEB_PESQUISAR[] = {
    { .nome = "consulta", .tipo = PARAM_STRING, .obrigatorio = TRUE },
    { .nome = "limite", .tipo = PARAM_INTEIRO, .obrigatorio = FALSE, .min = 1, .max = 10 },
    { .nome = "abrir_fontes", .tipo = PARAM_INTEIRO, .obrigatorio = FALSE, .min = 0, .max = 4 },
    { .nome = NULL },
};

static const ParametroManifesto PARAM_WEB_ABRIR[] = {
    { .nome = "url", .tipo = PARAM_STRING, .obrigatorio = TRUE },
    { .nome = NULL },
};

/* ---- arquivos do projeto (projeto/arquivos.h) ---- */
static const ParametroManifesto PARAM_ARQ_LER[] = {
    { .nome = "caminho", .tipo = PARAM_STRING, .obrigatorio = TRUE },
    { .nome = "linha_inicial", .tipo = PARAM_INTEIRO, .min = 1, .max = 100000000 },
    { .nome = "linhas", .tipo = PARAM_INTEIRO, .min = 1, .max = 2000 },
    { .nome = "codificacao", .tipo = PARAM_STRING },
    { .nome = NULL },
};
static const ParametroManifesto PARAM_ARQ_LISTAR[] = {
    { .nome = "caminho", .tipo = PARAM_STRING },
    { .nome = "recursivo", .tipo = PARAM_BOOLEANO },
    { .nome = "padrao", .tipo = PARAM_STRING },
    { .nome = "limite", .tipo = PARAM_INTEIRO, .min = 1, .max = 2000 },
    { .nome = "ocultos", .tipo = PARAM_BOOLEANO },
    { .nome = NULL },
};
static const ParametroManifesto PARAM_ARQ_BUSCAR[] = {
    { .nome = "texto", .tipo = PARAM_STRING, .obrigatorio = TRUE },
    { .nome = "regex", .tipo = PARAM_BOOLEANO },
    { .nome = "diferenciar_maiusculas", .tipo = PARAM_BOOLEANO },
    { .nome = "caminho", .tipo = PARAM_STRING },
    { .nome = "padrao", .tipo = PARAM_STRING },
    { .nome = "limite", .tipo = PARAM_INTEIRO, .min = 1, .max = 500 },
    { .nome = NULL },
};
static const ParametroManifesto PARAM_ARQ_INFO[] = {
    { .nome = "caminho", .tipo = PARAM_STRING, .obrigatorio = TRUE },
    { .nome = NULL },
};
static const ParametroManifesto PARAM_ARQ_CRIAR[] = {
    { .nome = "caminho", .tipo = PARAM_STRING, .obrigatorio = TRUE },
    { .nome = "conteudo", .tipo = PARAM_STRING, .obrigatorio = TRUE },
    { .nome = "codificacao", .tipo = PARAM_STRING },
    { .nome = "quebra_de_linha", .tipo = PARAM_ENUM, .valores_enum = EOLS },
    { .nome = "bom", .tipo = PARAM_BOOLEANO },
    { .nome = "executavel", .tipo = PARAM_BOOLEANO },
    { .nome = NULL },
};
static const ParametroManifesto PARAM_ARQ_ESCREVER[] = {
    { .nome = "caminho", .tipo = PARAM_STRING, .obrigatorio = TRUE },
    { .nome = "conteudo", .tipo = PARAM_STRING, .obrigatorio = TRUE },
    { .nome = "codificacao", .tipo = PARAM_STRING },
    { .nome = "codificacao_atual", .tipo = PARAM_STRING },
    { .nome = "quebra_de_linha", .tipo = PARAM_ENUM, .valores_enum = EOLS },
    { .nome = "sha256_esperado", .tipo = PARAM_STRING },
    { .nome = NULL },
};
static const ParametroManifesto PARAM_ARQ_EDITAR[] = {
    { .nome = "caminho", .tipo = PARAM_STRING, .obrigatorio = TRUE },
    { .nome = "procurar", .tipo = PARAM_STRING, .obrigatorio = TRUE },
    { .nome = "substituir", .tipo = PARAM_STRING, .obrigatorio = TRUE },
    { .nome = "todas", .tipo = PARAM_BOOLEANO },
    { .nome = "codificacao_atual", .tipo = PARAM_STRING },
    { .nome = "sha256_esperado", .tipo = PARAM_STRING },
    { .nome = NULL },
};
static const ParametroManifesto PARAM_ARQ_ANEXAR[] = {
    { .nome = "caminho", .tipo = PARAM_STRING, .obrigatorio = TRUE },
    { .nome = "conteudo", .tipo = PARAM_STRING, .obrigatorio = TRUE },
    { .nome = "codificacao_atual", .tipo = PARAM_STRING },
    { .nome = "sha256_esperado", .tipo = PARAM_STRING },
    { .nome = NULL },
};
static const ParametroManifesto PARAM_ARQ_APAGAR[] = {
    { .nome = "caminho", .tipo = PARAM_STRING, .obrigatorio = TRUE },
    { .nome = "sha256_esperado", .tipo = PARAM_STRING },
    { .nome = NULL },
};
static const ParametroManifesto PARAM_ARQ_ORIGEM_DESTINO[] = {
    { .nome = "origem", .tipo = PARAM_STRING, .obrigatorio = TRUE },
    { .nome = "destino", .tipo = PARAM_STRING, .obrigatorio = TRUE },
    { .nome = NULL },
};
static const ParametroManifesto PARAM_ARQ_CAMINHO[] = {
    { .nome = "caminho", .tipo = PARAM_STRING, .obrigatorio = TRUE },
    { .nome = NULL },
};
static const ParametroManifesto PARAM_ARQ_CONVERTER[] = {
    { .nome = "caminho", .tipo = PARAM_STRING, .obrigatorio = TRUE },
    { .nome = "codificacao", .tipo = PARAM_STRING },
    { .nome = "codificacao_atual", .tipo = PARAM_STRING },
    { .nome = "quebra_de_linha", .tipo = PARAM_ENUM, .valores_enum = EOLS },
    { .nome = "bom", .tipo = PARAM_BOOLEANO },
    { .nome = "sha256_esperado", .tipo = PARAM_STRING },
    { .nome = NULL },
};
static const ParametroManifesto PARAM_ARQ_PERMISSOES[] = {
    { .nome = "caminho", .tipo = PARAM_STRING, .obrigatorio = TRUE },
    { .nome = "executavel", .tipo = PARAM_BOOLEANO, .obrigatorio = TRUE },
    { .nome = NULL },
};
static const ParametroManifesto PARAM_ARQ_DESFAZER[] = {
    { .nome = "operacao_id", .tipo = PARAM_STRING, .obrigatorio = TRUE },
    { .nome = NULL },
};

static const ParametroManifesto PARAM_PRATICAS[] = {
    { .nome = "tarefa", .tipo = PARAM_STRING, .obrigatorio = TRUE },
    { .nome = "linguagem", .tipo = PARAM_STRING },
    { .nome = "limite", .tipo = PARAM_INTEIRO, .min = 1, .max = 12 },
    { .nome = NULL },
};

static const char *const ORDENAR_POR_VALORES[] = { "cpu", NULL };

static const ParametroManifesto PARAM_PROCESSOS_TOP_CPU[] = {
    { .nome = "limite", .tipo = PARAM_INTEIRO, .obrigatorio = FALSE, .min = 1, .max = 100 },
    { .nome = "intervalo_ms", .tipo = PARAM_INTEIRO, .obrigatorio = FALSE, .min = 100, .max = 5000 },
    { .nome = "ordenar_por", .tipo = PARAM_ENUM, .obrigatorio = FALSE, .valores_enum = ORDENAR_POR_VALORES },
    { .nome = NULL },
};

static const char *const ORDENAR_POR_MEMORIA[] = { "memoria", NULL };

static const ParametroManifesto PARAM_PROCESSOS_TOP_MEMORIA[] = {
    { .nome = "limite", .tipo = PARAM_INTEIRO, .obrigatorio = FALSE, .min = 1, .max = 100 },
    { .nome = "ordenar_por", .tipo = PARAM_ENUM, .obrigatorio = FALSE, .valores_enum = ORDENAR_POR_MEMORIA },
    { .nome = NULL },
};

static const char *const CLASSES_DISPOSITIVO[] = { "rede", "bloco", "pci", NULL };

static const ParametroManifesto PARAM_DISPOSITIVOS[] = {
    { .nome = "classe", .tipo = PARAM_ENUM, .obrigatorio = FALSE, .valores_enum = CLASSES_DISPOSITIVO },
    { .nome = NULL },
};

static const ManifestoReal TABELA[] = {
    {
        .base = {
            /* Discovery de verdade somente leitura: cli/ferramentas.c
             * discover() é chamada com --sem-salvar (ver executor_real.c)
             * — nunca grava nada em disco. Antes desta correção (auditoria
             * pós-commit e366f2b), esta ferramenta gravava
             * ~/.lia/maquina/inventario/ferramentas.lia e
             * ~/.lia/humano/diagnosticos/ferramentas.md mesmo classificada
             * como risco=nenhum — uma inconsistência real entre o
             * manifesto e o efeito de verdade. A gravação agora é uma
             * ação separada e explícita: tools.discover.persistir, risco
             * "baixo", sempre com aprovação. */
            .nome = "tools.discover", .risco = "nenhum", .requer_aprovacao = FALSE,
            .versao = "0.2.0", .idempotente = TRUE,
            .reversivel = "nao_aplicavel", .capacidades = CAP_LEITURA_PROCESSO,
            .efeitos = "lê PATH e diretórios convencionais do sistema; não grava nada (ver tools.discover.persistir para persistir o resultado)",
            .timeout_padrao_ms = 15000,
            .somente_leitura = TRUE, .uso = "inventariar os programas/ferramentas instalados (varre o PATH)",
        },
        .parametros = PARAM_VAZIO,
    },
    {
        .base = {
            /* Persistência explícita do resultado da varredura mais
             * recente — refaz a varredura (cli/ferramentas.c não carrega
             * estado entre chamadas de processo separadas) e grava os
             * dois arquivos. Risco "baixo" (escreve fora do processo,
             * mas só no espaço de trabalho da própria Lia — inventário e
             * diagnóstico derivados, nunca conteúdo do usuário) com
             * aprovação explícita sempre exigida. */
            .nome = "tools.discover.persistir", .risco = "baixo", .requer_aprovacao = TRUE,
            .versao = "0.1.0", .idempotente = TRUE,
            .reversivel = "nao_aplicavel", .capacidades = CAP_LEITURA_PROCESSO,
            .efeitos = "lê PATH e diretórios convencionais; grava ~/.lia/maquina/inventario/ferramentas.lia e ~/.lia/humano/diagnosticos/ferramentas.md",
            .timeout_padrao_ms = 15000,
            .somente_leitura = FALSE, .uso = "gravar em disco o inventário de programas (exige aprovação)",
        },
        .parametros = PARAM_VAZIO,
    },
    {
        .base = {
            .nome = "tools.list", .risco = "nenhum", .requer_aprovacao = FALSE,
            .versao = "0.1.0", .idempotente = TRUE,
            .reversivel = "nao_aplicavel", .capacidades = CAP_LEITURA,
            .efeitos = "lê ~/.lia/maquina/inventario/ferramentas.lia; não escreve nada",
            .timeout_padrao_ms = 5000,
            .somente_leitura = TRUE, .uso = "listar o inventário de programas já gravado",
        },
        .parametros = PARAM_LIST,
    },
    {
        .base = {
            /* Mesma correção de tools.discover: probe() com --sem-salvar
             * nunca grava nada — só executa o catálogo de probes seguros
             * e reporta o resultado. */
            .nome = "tools.probe", .risco = "nenhum", .requer_aprovacao = FALSE,
            .versao = "0.2.0", .idempotente = TRUE,
            .reversivel = "nao_aplicavel", .capacidades = CAP_LEITURA_PROCESSO,
            .efeitos = "executa, por até poucos segundos cada, só ferramentas do catálogo de probes seguros, com argumento inofensivo e sem privilégio; não grava nada (ver tools.probe.persistir)",
            .timeout_padrao_ms = 45000,
            .somente_leitura = FALSE, .uso = "testar se os programas do catálogo seguro funcionam (executa cada um; exige pedido explícito)",
        },
        .parametros = PARAM_PROBE,
    },
    {
        .base = {
            .nome = "tools.probe.persistir", .risco = "baixo", .requer_aprovacao = TRUE,
            .versao = "0.1.0", .idempotente = TRUE,
            .reversivel = "nao_aplicavel", .capacidades = CAP_LEITURA_PROCESSO,
            .efeitos = "repete o probe seguro e grava ~/.lia/maquina/inventario/ferramentas.lia e ~/.lia/humano/diagnosticos/ferramentas.md",
            .timeout_padrao_ms = 45000,
            .somente_leitura = FALSE, .uso = "testar os programas do catálogo seguro e gravar o resultado (exige aprovação)",
        },
        .parametros = PARAM_PROBE,
    },
    {
        .base = {
            .nome = "sistema.diagnosticar", .risco = "nenhum", .requer_aprovacao = FALSE,
            .versao = "0.1.0", .idempotente = TRUE,
            .reversivel = "nao_aplicavel", .capacidades = CAP_LEITURA,
            .efeitos = "lê /etc/os-release, /proc/cpuinfo, /proc/meminfo, uname; não escreve nada",
            .timeout_padrao_ms = 5000,
            .somente_leitura = TRUE, .uso = "distribuição, kernel, modelo e núcleos da CPU",
        },
        .parametros = PARAM_VAZIO,
    },
    {
        .base = {
            .nome = "sistema.recursos", .risco = "nenhum", .requer_aprovacao = FALSE,
            .versao = "0.1.0", .idempotente = TRUE,
            .reversivel = "nao_aplicavel", .capacidades = CAP_LEITURA,
            .efeitos = "lê /proc/meminfo, /proc/cpuinfo, /proc/uptime, /proc/loadavg; não escreve nada",
            .timeout_padrao_ms = 5000,
            .somente_leitura = TRUE, .uso = "RAM total/livre/disponível, swap, uptime e carga média",
        },
        .parametros = PARAM_VAZIO,
    },
    {
        .base = {
            .nome = "sistema.dispositivos", .risco = "nenhum", .requer_aprovacao = FALSE,
            .versao = "0.1.0", .idempotente = TRUE,
            .reversivel = "nao_aplicavel", .capacidades = CAP_LEITURA,
            .efeitos = "lê /sys/class/net, /sys/class/block, /sys/bus/pci/devices; não escreve nada",
            .timeout_padrao_ms = 5000,
            .somente_leitura = TRUE, .uso = "interfaces de rede, dispositivos de bloco (discos) e dispositivos PCI; parâmetro opcional classe = rede|bloco|pci",
        },
        .parametros = PARAM_DISPOSITIVOS,
    },
    {
        .base = {
            /* Somente leitura: duas amostras de /proc/<pid>/stat separadas
             * por 'intervalo_ms', nunca /proc/<pid>/cmdline ou
             * /proc/<pid>/environ (nomes de processo e metadados de CPU
             * já podem revelar informação sensível — docs/THREAT-MODEL.md
             * §5.1 — argumentos completos/ambiente seriam bem mais
             * sensíveis ainda, por isso ficam fora desta primeira
             * versão). idempotente=FALSE porque o resultado depende do
             * instante da amostragem (a leitura em si não tem efeito,
             * mas repetir não reproduz os mesmos números). */
            .nome = "sistema.processos_top_cpu", .risco = "nenhum", .requer_aprovacao = FALSE,
            .versao = "0.1.0", .idempotente = FALSE,
            .reversivel = "nao_aplicavel", .capacidades = CAP_LEITURA_PROCESSO,
            .efeitos = "lê /proc/<pid>/stat de cada processo (duas amostras, com uma pausa curta entre elas); não lê cmdline/environ; não escreve nada",
            .timeout_padrao_ms = 8000,
            .somente_leitura = TRUE, .uso = "processos que mais usam CPU agora; parâmetro opcional limite (1 a 100, padrão 10)",
        },
        .parametros = PARAM_PROCESSOS_TOP_CPU,
    },
    {
        .base = {
            /* Somente leitura: uma passada por /proc/<pid>/status (Name,
             * State, VmRSS) e /proc/meminfo (MemTotal). Nunca cmdline nem
             * environ, nunca um processo filho, nunca shell — mesma
             * fronteira de sistema.processos_top_cpu (docs/THREAT-MODEL.md
             * §5.1). idempotente=FALSE: a leitura não tem efeito, mas
             * repetir não reproduz os mesmos números. Limites: no máximo
             * 8192 processos considerados (vetor estático em
             * cli/sistema.c), 100 devolvidos, 5 s de execução. */
            .nome = "sistema.processos_top_memoria", .risco = "nenhum", .requer_aprovacao = FALSE,
            .versao = "0.1.0", .idempotente = FALSE,
            .reversivel = "nao_aplicavel", .capacidades = CAP_LEITURA_PROCESSO,
            .efeitos = "lê /proc/<pid>/status de cada processo e /proc/meminfo; não lê cmdline/environ; não escreve e não inicia processos",
            .timeout_padrao_ms = 5000,
            .somente_leitura = TRUE, .uso = "processos que mais usam memória RAM agora; parâmetro opcional limite (1 a 100, padrão 10)",
        },
        .parametros = PARAM_PROCESSOS_TOP_MEMORIA,
    },
    {
        .base = {
            .nome = "web.pesquisar", .risco = "baixo", .requer_aprovacao = TRUE,
            .versao = "0.1.0", .idempotente = TRUE,
            .reversivel = "nao_aplicavel", .capacidades = CAP_REDE,
            .efeitos = "envia a consulta a um mecanismo de busca HTTPS externo e abre em paralelo até 4 das melhores fontes "
                       "(HTTPS público, redirecionamentos validados) para extrair o trecho relevante; não grava dados e limita resposta e tempo",
            .timeout_padrao_ms = 30000,
            .somente_leitura = FALSE,
            .uso = "pesquisar na internet, abrir as melhores fontes e devolver títulos, URLs, datas e o trecho relevante de cada uma; "
                   "parâmetros consulta, limite (1..10), abrir_fontes (0..4, padrão 3); a consulta sai da máquina e exige aprovação",
        },
        .parametros = PARAM_WEB_PESQUISAR,
    },
    {
        .base = {
            .nome = "web.abrir", .risco = "baixo", .requer_aprovacao = TRUE,
            .versao = "0.1.0", .idempotente = TRUE,
            .reversivel = "nao_aplicavel", .capacidades = CAP_REDE,
            .efeitos = "abre uma URL HTTPS pública (até 3 redirecionamentos, cada destino revalidado), lê texto limitado e não executa JavaScript nem grava o conteúdo",
            .timeout_padrao_ms = 20000,
            .somente_leitura = FALSE,
            .uso = "abrir uma fonte HTTPS pública indicada pela pessoa; destinos em rede privada são recusados",
        },
        .parametros = PARAM_WEB_ABRIR,
    },
    {
        .base = {
            .nome = "arquivo.ler", .risco = "nenhum", .requer_aprovacao = FALSE,
            .versao = "0.1.0", .idempotente = TRUE, .reversivel = "nao_aplicavel", .capacidades = CAP_LER_PROJETO,
            .efeitos = "lê um arquivo dentro do projeto; não grava nada",
            .timeout_padrao_ms = 15000, .somente_leitura = TRUE,
            .uso = "ler um arquivo do projeto (texto em qualquer codificação, ou hexdump se binário); caminho, linha_inicial, linhas (até 2000), codificacao opcional",
        },
        .parametros = PARAM_ARQ_LER,
    },
    {
        .base = {
            .nome = "arquivo.listar", .risco = "nenhum", .requer_aprovacao = FALSE,
            .versao = "0.1.0", .idempotente = TRUE, .reversivel = "nao_aplicavel", .capacidades = CAP_LER_PROJETO,
            .efeitos = "lista arquivos dentro do projeto (sem .git/); não grava nada",
            .timeout_padrao_ms = 15000, .somente_leitura = TRUE,
            .uso = "listar arquivos e pastas do projeto; caminho, recursivo, padrao (ex.: *.c), limite",
        },
        .parametros = PARAM_ARQ_LISTAR,
    },
    {
        .base = {
            .nome = "arquivo.buscar", .risco = "nenhum", .requer_aprovacao = FALSE,
            .versao = "0.1.0", .idempotente = TRUE, .reversivel = "nao_aplicavel", .capacidades = CAP_LER_PROJETO,
            .efeitos = "lê os arquivos de texto do projeto procurando um trecho; não grava nada",
            .timeout_padrao_ms = 15000, .somente_leitura = TRUE,
            .uso = "procurar um texto ou regex nos arquivos do projeto, com arquivo e número da linha; texto, regex, caminho, padrao",
        },
        .parametros = PARAM_ARQ_BUSCAR,
    },
    {
        .base = {
            .nome = "arquivo.info", .risco = "nenhum", .requer_aprovacao = FALSE,
            .versao = "0.1.0", .idempotente = TRUE, .reversivel = "nao_aplicavel", .capacidades = CAP_LER_PROJETO,
            .efeitos = "lê os metadados e o conteúdo de um arquivo do projeto; não grava nada",
            .timeout_padrao_ms = 15000, .somente_leitura = TRUE,
            .uso = "informações de um arquivo: tamanho, permissões, codificação, quebra de linha, tipo, sha256",
        },
        .parametros = PARAM_ARQ_INFO,
    },
    {
        .base = {
            .nome = "arquivo.criar", .risco = "medio", .requer_aprovacao = TRUE,
            .versao = "0.1.0", .idempotente = FALSE, .reversivel = "sim", .capacidades = CAP_ESCREVER_PROJETO,
            .efeitos = "cria um arquivo novo dentro do projeto (falha se já existir); guarda o original para desfazer (arquivo.desfazer); só dentro do projeto, nunca em .git/",
            .timeout_padrao_ms = 15000, .somente_leitura = FALSE,
            .uso = "criar um arquivo novo no projeto com o conteúdo dado (nunca sobrescreve); exige aprovação",
        },
        .parametros = PARAM_ARQ_CRIAR,
    },
    {
        .base = {
            .nome = "arquivo.escrever", .risco = "medio", .requer_aprovacao = TRUE,
            .versao = "0.1.0", .idempotente = FALSE, .reversivel = "sim", .capacidades = CAP_ESCREVER_PROJETO,
            .efeitos = "regrava um arquivo existente do projeto; guarda o original para desfazer (arquivo.desfazer); só dentro do projeto, nunca em .git/",
            .timeout_padrao_ms = 15000, .somente_leitura = FALSE,
            .uso = "substituir o conteúdo inteiro de um arquivo existente, preservando codificação e quebra de linha; exige aprovação",
        },
        .parametros = PARAM_ARQ_ESCREVER,
    },
    {
        .base = {
            .nome = "arquivo.editar", .risco = "medio", .requer_aprovacao = TRUE,
            .versao = "0.1.0", .idempotente = FALSE, .reversivel = "sim", .capacidades = CAP_ESCREVER_PROJETO,
            .efeitos = "altera um trecho de um arquivo do projeto; guarda o original para desfazer (arquivo.desfazer); só dentro do projeto, nunca em .git/",
            .timeout_padrao_ms = 15000, .somente_leitura = FALSE,
            .uso = "trocar um trecho exato (procurar → substituir) num arquivo; o trecho precisa ser único, ou todas=true; exige aprovação",
        },
        .parametros = PARAM_ARQ_EDITAR,
    },
    {
        .base = {
            .nome = "arquivo.anexar", .risco = "medio", .requer_aprovacao = TRUE,
            .versao = "0.1.0", .idempotente = FALSE, .reversivel = "sim", .capacidades = CAP_ESCREVER_PROJETO,
            .efeitos = "acrescenta ao fim de um arquivo do projeto; guarda o original para desfazer (arquivo.desfazer); só dentro do projeto, nunca em .git/",
            .timeout_padrao_ms = 15000, .somente_leitura = FALSE,
            .uso = "acrescentar texto ao fim de um arquivo existente; exige aprovação",
        },
        .parametros = PARAM_ARQ_ANEXAR,
    },
    {
        .base = {
            .nome = "arquivo.apagar", .risco = "alto", .requer_aprovacao = TRUE,
            .versao = "0.1.0", .idempotente = FALSE, .reversivel = "sim", .capacidades = CAP_ESCREVER_PROJETO,
            .efeitos = "apaga um arquivo ou pasta vazia do projeto; guarda o original para desfazer (arquivo.desfazer); só dentro do projeto, nunca em .git/",
            .timeout_padrao_ms = 15000, .somente_leitura = FALSE,
            .uso = "apagar um arquivo (ou pasta vazia) do projeto; nunca apaga em massa; exige aprovação",
        },
        .parametros = PARAM_ARQ_APAGAR,
    },
    {
        .base = {
            .nome = "arquivo.mover", .risco = "medio", .requer_aprovacao = TRUE,
            .versao = "0.1.0", .idempotente = FALSE, .reversivel = "sim", .capacidades = CAP_ESCREVER_PROJETO,
            .efeitos = "renomeia/move dentro do projeto; registra a operação para desfazer",
            .timeout_padrao_ms = 15000, .somente_leitura = FALSE,
            .uso = "mover ou renomear um arquivo/pasta dentro do projeto (nunca sobrescreve); exige aprovação",
        },
        .parametros = PARAM_ARQ_ORIGEM_DESTINO,
    },
    {
        .base = {
            .nome = "arquivo.copiar", .risco = "baixo", .requer_aprovacao = TRUE,
            .versao = "0.1.0", .idempotente = FALSE, .reversivel = "sim", .capacidades = CAP_ESCREVER_PROJETO,
            .efeitos = "cria uma cópia de um arquivo dentro do projeto; registra a operação para desfazer",
            .timeout_padrao_ms = 15000, .somente_leitura = FALSE,
            .uso = "copiar um arquivo dentro do projeto (nunca sobrescreve); exige aprovação",
        },
        .parametros = PARAM_ARQ_ORIGEM_DESTINO,
    },
    {
        .base = {
            .nome = "arquivo.criar_pasta", .risco = "baixo", .requer_aprovacao = TRUE,
            .versao = "0.1.0", .idempotente = FALSE, .reversivel = "sim", .capacidades = CAP_ESCREVER_PROJETO,
            .efeitos = "cria pastas dentro do projeto; registra a operação para desfazer",
            .timeout_padrao_ms = 15000, .somente_leitura = FALSE,
            .uso = "criar uma pasta (e as que faltarem) no projeto; exige aprovação",
        },
        .parametros = PARAM_ARQ_CAMINHO,
    },
    {
        .base = {
            .nome = "arquivo.converter", .risco = "medio", .requer_aprovacao = TRUE,
            .versao = "0.1.0", .idempotente = FALSE, .reversivel = "sim", .capacidades = CAP_ESCREVER_PROJETO,
            .efeitos = "regrava um arquivo do projeto em outra codificação/quebra de linha; guarda o original para desfazer (arquivo.desfazer); só dentro do projeto, nunca em .git/",
            .timeout_padrao_ms = 15000, .somente_leitura = FALSE,
            .uso = "converter a codificação (ex.: WINDOWS-1252 → UTF-8), a quebra de linha (LF/CRLF/CR) ou o BOM de um arquivo; exige aprovação",
        },
        .parametros = PARAM_ARQ_CONVERTER,
    },
    {
        .base = {
            .nome = "arquivo.permissoes", .risco = "medio", .requer_aprovacao = TRUE,
            .versao = "0.1.0", .idempotente = FALSE, .reversivel = "sim", .capacidades = CAP_ESCREVER_PROJETO,
            .efeitos = "muda só o bit de execução de um arquivo do projeto; registra a permissão anterior para desfazer",
            .timeout_padrao_ms = 15000, .somente_leitura = FALSE,
            .uso = "tornar um arquivo executável (ou não); exige aprovação",
        },
        .parametros = PARAM_ARQ_PERMISSOES,
    },
    {
        .base = {
            .nome = "arquivo.desfazer", .risco = "medio", .requer_aprovacao = TRUE,
            .versao = "0.1.0", .idempotente = FALSE, .reversivel = "nao", .capacidades = CAP_ESCREVER_PROJETO,
            .efeitos = "restaura arquivos a partir das cópias guardadas por uma operação anterior",
            .timeout_padrao_ms = 15000, .somente_leitura = FALSE,
            .uso = "desfazer uma alteração anterior de arquivo pelo operacao_id; exige aprovação",
        },
        .parametros = PARAM_ARQ_DESFAZER,
    },
    {
        .base = {
            .nome = "praticas.consultar", .risco = "nenhum", .requer_aprovacao = FALSE,
            .versao = "0.1.0", .idempotente = TRUE, .reversivel = "nao_aplicavel", .capacidades = CAP_LER_PROJETO,
            .efeitos = "consulta o catálogo local de boas práticas de programação (conhecimento/praticas); não grava nada, não usa rede",
            .timeout_padrao_ms = 5000, .somente_leitura = TRUE,
            .uso = "boas práticas de programação relevantes para uma tarefa (C, testes, refatoração, git, shell, segurança, bancos de dados...); parâmetro tarefa",
        },
        .parametros = PARAM_PRATICAS,
    },
};
#define N_TABELA (sizeof(TABELA) / sizeof(TABELA[0]))

const char * const *manifesto_real_listar_nomes(void)
{
    static const char *nomes[N_TABELA + 1];
    static gboolean pronto = FALSE;
    if (!pronto) {
        for (size_t i = 0; i < N_TABELA; i++)
            nomes[i] = TABELA[i].base.nome;
        nomes[N_TABELA] = NULL;
        pronto = TRUE;
    }
    return nomes;
}

const ManifestoReal *manifesto_real_buscar(const char *nome)
{
    if (!nome)
        return NULL;
    for (size_t i = 0; i < N_TABELA; i++)
        if (strcmp(TABELA[i].base.nome, nome) == 0)
            return &TABELA[i];
    return NULL;
}

static const ParametroManifesto *buscar_param(const ManifestoReal *m, const char *nome)
{
    for (const ParametroManifesto *p = m->parametros; p->nome; p++)
        if (strcmp(p->nome, nome) == 0)
            return p;
    return NULL;
}

char *manifesto_real_validar_parametros(const ManifestoReal *m, JsonObject *parametros)
{
    if (!parametros)
        return NULL; /* ausente == objeto vazio == tudo opcional, ok */

    GList *chaves = json_object_get_members(parametros);
    for (GList *it = chaves; it; it = it->next) {
        const char *chave = it->data;
        const ParametroManifesto *p = buscar_param(m, chave);
        if (!p) {
            g_list_free(chaves);
            return g_strdup_printf("parâmetro desconhecido: '%s'", chave);
        }
        JsonNode *n = json_object_get_member(parametros, chave);
        if (JSON_NODE_TYPE(n) != JSON_NODE_VALUE) {
            g_list_free(chaves);
            return g_strdup_printf("parâmetro '%s': tipo inválido", chave);
        }
        GType tipo_json = json_node_get_value_type(n);
        switch (p->tipo) {
        case PARAM_BOOLEANO:
            if (tipo_json != G_TYPE_BOOLEAN) {
                g_list_free(chaves);
                return g_strdup_printf("parâmetro '%s': esperado booleano", chave);
            }
            break;
        case PARAM_STRING:
            if (tipo_json != G_TYPE_STRING) {
                g_list_free(chaves);
                return g_strdup_printf("parâmetro '%s': esperado string", chave);
            }
            break;
        case PARAM_ENUM: {
            if (tipo_json != G_TYPE_STRING) {
                g_list_free(chaves);
                return g_strdup_printf("parâmetro '%s': esperado string (enum)", chave);
            }
            const char *valor = json_node_get_string(n);
            gboolean achou = FALSE;
            for (const char * const *v = p->valores_enum; v && *v; v++)
                if (strcmp(*v, valor) == 0) { achou = TRUE; break; }
            if (!achou) {
                g_list_free(chaves);
                return g_strdup_printf("parâmetro '%s': valor '%s' fora do enum permitido", chave, valor);
            }
            break;
        }
        case PARAM_INTEIRO: {
            /* json-glib representa todo literal numérico inteiro do JSON
             * como G_TYPE_INT64 — um valor com ponto decimal (ex.: 10.5)
             * vem como G_TYPE_DOUBLE e é rejeitado aqui, nunca truncado
             * silenciosamente. */
            if (tipo_json != G_TYPE_INT64) {
                g_list_free(chaves);
                return g_strdup_printf("parâmetro '%s': esperado inteiro", chave);
            }
            gint64 valor = json_node_get_int(n);
            if (valor < p->min || valor > p->max) {
                g_list_free(chaves);
                return g_strdup_printf("parâmetro '%s': valor %" G_GINT64_FORMAT
                                       " fora da faixa permitida (%" G_GINT64_FORMAT "..%" G_GINT64_FORMAT ")",
                                       chave, valor, p->min, p->max);
            }
            break;
        }
        }
    }
    g_list_free(chaves);

    for (const ParametroManifesto *p = m->parametros; p->nome; p++) {
        if (p->obrigatorio && (!json_object_has_member(parametros, p->nome)))
            return g_strdup_printf("parâmetro obrigatório ausente: '%s'", p->nome);
    }
    return NULL;
}
