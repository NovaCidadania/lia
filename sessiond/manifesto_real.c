#include "manifesto_real.h"
#include <string.h>

static const char *const CAP_LEITURA_PROCESSO[] = { "leitura_arquivo", "processo", NULL };
static const char *const CAP_LEITURA[] = { "leitura_arquivo", NULL };

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

static const char *const ORDENAR_POR_VALORES[] = { "cpu", NULL };

static const ParametroManifesto PARAM_PROCESSOS_TOP_CPU[] = {
    { .nome = "limite", .tipo = PARAM_INTEIRO, .obrigatorio = FALSE, .min = 1, .max = 100 },
    { .nome = "intervalo_ms", .tipo = PARAM_INTEIRO, .obrigatorio = FALSE, .min = 100, .max = 5000 },
    { .nome = "ordenar_por", .tipo = PARAM_ENUM, .obrigatorio = FALSE, .valores_enum = ORDENAR_POR_VALORES },
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
        },
        .parametros = PARAM_VAZIO,
    },
    {
        .base = {
            .nome = "sistema.recursos", .risco = "nenhum", .requer_aprovacao = FALSE,
            .versao = "0.1.0", .idempotente = TRUE,
            .reversivel = "nao_aplicavel", .capacidades = CAP_LEITURA,
            .efeitos = "lê /proc/meminfo, /proc/cpuinfo, /proc/uptime; não escreve nada",
            .timeout_padrao_ms = 5000,
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
        },
        .parametros = PARAM_VAZIO,
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
            .reversivel = "nao_aplicavel", .capacidades = CAP_LEITURA,
            .efeitos = "lê /proc/<pid>/stat de cada processo (duas amostras, com uma pausa curta entre elas); não lê cmdline/environ; não escreve nada",
            .timeout_padrao_ms = 8000,
        },
        .parametros = PARAM_PROCESSOS_TOP_CPU,
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
