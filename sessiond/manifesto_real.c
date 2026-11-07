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

static const ManifestoReal TABELA[] = {
    {
        .base = {
            .nome = "tools.discover", .risco = "nenhum", .requer_aprovacao = FALSE,
            .reversivel = "nao_aplicavel", .capacidades = CAP_LEITURA_PROCESSO,
            .efeitos = "lê PATH e diretórios convencionais; grava ~/.lia/maquina/inventario/ferramentas.lia e ~/.lia/humano/diagnosticos/ferramentas.md",
            .timeout_padrao_ms = 15000,
        },
        .parametros = PARAM_VAZIO,
    },
    {
        .base = {
            .nome = "tools.list", .risco = "nenhum", .requer_aprovacao = FALSE,
            .reversivel = "nao_aplicavel", .capacidades = CAP_LEITURA,
            .efeitos = "lê ~/.lia/maquina/inventario/ferramentas.lia; não escreve nada",
            .timeout_padrao_ms = 5000,
        },
        .parametros = PARAM_LIST,
    },
    {
        .base = {
            .nome = "tools.probe", .risco = "nenhum", .requer_aprovacao = FALSE,
            .reversivel = "nao_aplicavel", .capacidades = CAP_LEITURA_PROCESSO,
            .efeitos = "executa, por até poucos segundos cada, só ferramentas do catálogo de probes seguros, com argumento inofensivo e sem privilégio; atualiza o inventário",
            .timeout_padrao_ms = 45000,
        },
        .parametros = PARAM_PROBE,
    },
    {
        .base = {
            .nome = "sistema.diagnosticar", .risco = "nenhum", .requer_aprovacao = FALSE,
            .reversivel = "nao_aplicavel", .capacidades = CAP_LEITURA,
            .efeitos = "lê /etc/os-release, /proc/cpuinfo, /proc/meminfo, uname; não escreve nada",
            .timeout_padrao_ms = 5000,
        },
        .parametros = PARAM_VAZIO,
    },
    {
        .base = {
            .nome = "sistema.recursos", .risco = "nenhum", .requer_aprovacao = FALSE,
            .reversivel = "nao_aplicavel", .capacidades = CAP_LEITURA,
            .efeitos = "lê /proc/meminfo, /proc/cpuinfo, /proc/uptime; não escreve nada",
            .timeout_padrao_ms = 5000,
        },
        .parametros = PARAM_VAZIO,
    },
    {
        .base = {
            .nome = "sistema.dispositivos", .risco = "nenhum", .requer_aprovacao = FALSE,
            .reversivel = "nao_aplicavel", .capacidades = CAP_LEITURA,
            .efeitos = "lê /sys/class/net, /sys/class/block, /sys/bus/pci/devices; não escreve nada",
            .timeout_padrao_ms = 5000,
        },
        .parametros = PARAM_VAZIO,
    },
};
#define N_TABELA (sizeof(TABELA) / sizeof(TABELA[0]))

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
        }
    }
    g_list_free(chaves);

    for (const ParametroManifesto *p = m->parametros; p->nome; p++) {
        if (p->obrigatorio && (!json_object_has_member(parametros, p->nome)))
            return g_strdup_printf("parâmetro obrigatório ausente: '%s'", p->nome);
    }
    return NULL;
}
