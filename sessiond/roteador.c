/* lia-sessiond — implementação de sessiond/roteador.h. Ver o cabeçalho. */
#include "roteador.h"
#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------ normalização --- */

/* minúsculas, sem acento, tudo que não é letra/dígito vira separador.
 * Devolve vetor de tokens (g_strfreev). */
static char **tokenizar(const char *texto)
{
    char *nfd = g_utf8_normalize(texto ? texto : "", -1, G_NORMALIZE_NFD);
    GString *limpo = g_string_new(NULL);
    if (nfd) {
        for (const char *p = nfd; *p; p = g_utf8_next_char(p)) {
            gunichar ch = g_utf8_get_char(p);
            if (g_unichar_ismark(ch))
                continue; /* acento combinante: descarta ("memória" -> "memoria") */
            if (ch < 128 && g_ascii_isalnum((char)ch))
                g_string_append_c(limpo, g_ascii_tolower((char)ch));
            else if (ch >= 128 && g_unichar_isalnum(ch))
                g_string_append_unichar(limpo, g_unichar_tolower(ch));
            else
                g_string_append_c(limpo, ' ');
        }
    }
    g_free(nfd);
    char **bruto = g_strsplit(limpo->str, " ", -1);
    g_string_free(limpo, TRUE);
    GPtrArray *arr = g_ptr_array_new();
    for (int i = 0; bruto[i]; i++) {
        if (*bruto[i])
            g_ptr_array_add(arr, bruto[i]);
        else
            g_free(bruto[i]);
    }
    g_free(bruto);
    g_ptr_array_add(arr, NULL);
    return (char **)g_ptr_array_free(arr, FALSE);
}

/* algum token é igual a uma das palavras exatas, ou começa por um dos radicais */
static gboolean tem(char **tokens, const char *const *exatas, const char *const *radicais)
{
    for (int i = 0; tokens[i]; i++) {
        for (int j = 0; exatas && exatas[j]; j++)
            if (strcmp(tokens[i], exatas[j]) == 0)
                return TRUE;
        for (int j = 0; radicais && radicais[j]; j++)
            if (g_str_has_prefix(tokens[i], radicais[j]))
                return TRUE;
    }
    return FALSE;
}

static gboolean tem_par(char **tokens, const char *a, const char *b)
{
    for (int i = 0; tokens[i] && tokens[i + 1]; i++)
        if (strcmp(tokens[i], a) == 0 && strcmp(tokens[i + 1], b) == 0)
            return TRUE;
    return FALSE;
}

/* "os 5 processos", "top 5", "5 maiores processos": o número que a
 * PRÓPRIA pessoa disse, na posição em que só pode ser a quantidade pedida.
 * Devolve 0 se não houver um (ou se estiver fora de 1..100 — nesse caso
 * vale o padrão do manifesto, nunca um valor ajustado por palpite). */
static int limite_explicito(char **t)
{
    static const char *const ALVOS[] = { "processo", "processos", "programas", "apps", "aplicativos", "tarefas", NULL };
    static const char *const MEIO[] = { "principais", "maiores", "primeiros", "top", NULL };
    for (int i = 0; t[i]; i++) {
        const char *num = NULL;
        if (strcmp(t[i], "top") == 0 && t[i + 1])
            num = t[i + 1];
        for (int a = 0; !num && ALVOS[a]; a++) {
            if (strcmp(t[i], ALVOS[a]) != 0 || i == 0) continue;
            num = t[i - 1];
            for (int m = 0; MEIO[m]; m++)
                if (strcmp(t[i - 1], MEIO[m]) == 0) num = i >= 2 ? t[i - 2] : NULL;
        }
        if (!num || !*num || strlen(num) > 3) continue;
        gboolean so_digitos = TRUE;
        for (const char *q = num; *q; q++) if (!g_ascii_isdigit(*q)) so_digitos = FALSE;
        if (!so_digitos) continue;
        int v = atoi(num);
        if (v >= 1 && v <= 100) return v;
    }
    return 0;
}

/* ----------------------------------------------------- classificação --- */

#define L(...) ((const char *const[]){ __VA_ARGS__, NULL })

static void rota(Rota *out, const char *intencao, const char *descricao, RotaConfianca conf,
                  const char *ferramenta, const char *parametros, const char *const *permitidas)
{
    out->intencao = intencao;
    out->descricao = descricao;
    out->confianca = conf;
    out->ferramenta = ferramenta;
    out->parametros_json = parametros ? parametros : "{}";
    int n = 0;
    for (int i = 0; permitidas && permitidas[i] && n < ROTA_MAX_PERMITIDAS; i++)
        out->permitidas[n++] = permitidas[i];
    if (n == 0 && ferramenta)
        out->permitidas[n++] = ferramenta;
    out->permitidas[n] = NULL;
}

void roteador_classificar(const char *pedido, Rota *out)
{
    memset(out, 0, sizeof *out);
    out->parametros_json = "{}";
    char **t = tokenizar(pedido);

    /* "processador" NÃO é processo: por isso PROCESSO usa palavras exatas
     * para "processo(s)" em vez do radical "process". */
    gboolean processo = tem(t, L("processo", "processos", "app", "apps", "daemon", "daemons"),
                              L("program", "tarefa", "aplicativ", "aplicac"));
    gboolean cpu = tem(t, L("cpu", "cpus", "processamento"), L("processador"));
    gboolean memoria = tem(t, L("memoria", "ram", "rss", "memory"), NULL);
    /* só verbos/substantivos de consumo — nunca um radical curto como "com",
     * que casaria com "com"/"como" */
    gboolean consumo = tem(t, L("uso", "usa", "usam", "usando", "usado", "usados", "top", "pesado", "pesados", "pesada", "pesadas"),
                             L("consom", "consum", "utiliz", "gast", "ocup", "devor"));
    gboolean superlativo = tem(t, L("mais", "top", "maior", "maiores", "quem", "ranking"), NULL);
    gboolean quantidade = tem(t, L("quanto", "quanta", "quantos", "quantas", "total", "livre", "livres"), L("disponi"));
    gboolean swap = tem(t, L("swap"), NULL);
    gboolean uptime = tem(t, L("uptime"), L("ligad")) || tem_par(t, "tempo", "ligado") || tem_par(t, "tempo", "atividade");
    gboolean carga = tem(t, L("carga", "load", "loadavg"), NULL);
    gboolean recursos = tem(t, L("recurso", "recursos"), NULL);
    gboolean kernel = tem(t, L("kernel", "linux"), NULL);
    gboolean distro = tem(t, L("distro", "arch"), L("distribu")) || tem_par(t, "sistema", "operacional");
    gboolean nucleos = tem(t, L("core", "cores", "threads"), L("nucleo"));
    gboolean modelo = tem(t, L("modelo"), NULL);
    gboolean rede = tem(t, L("rede", "redes", "network", "wifi", "ethernet"), L("interface"));
    /* "disco"/"partição" como palavras exatas: o radical "disco" casaria com
     * "discover"/"discord", e "partic" com "participar". */
    gboolean bloco = tem(t, L("bloco", "blocos", "block", "hd", "hds", "ssd", "ssds", "nvme", "armazenamento",
                              "disco", "discos", "particao", "particoes"), NULL);
    gboolean pci = tem(t, L("pci", "placa", "placas"), NULL);
    gboolean dispositivo = tem(t, L("hardware"), L("dispositivo", "perifer"));
    gboolean espaco = tem(t, L("espaco", "cheio", "cheios", "cheia"), NULL);
    gboolean inventario = tem(t, L("inventario"), L("instalad"));

    static const char *const SO_CPU[] = { "sistema.processos_top_cpu", NULL };
    static const char *const SO_MEM[] = { "sistema.processos_top_memoria", NULL };
    static const char *const CPU_OU_MEM[] = { "sistema.processos_top_cpu", "sistema.processos_top_memoria", NULL };
    static const char *const SO_DISP[] = { "sistema.dispositivos", NULL };
    static const char *const SO_DIAG[] = { "sistema.diagnosticar", NULL };
    static const char *const SO_REC[] = { "sistema.recursos", NULL };
    static const char *const DIAG_OU_REC[] = { "sistema.diagnosticar", "sistema.recursos", NULL };
    static const char *const INVENTARIO[] = { "tools.list", "tools.discover", "tools.probe",
                                               "tools.discover.persistir", "tools.probe.persistir", NULL };

    /* 1. inventário de programas instalados — antes de "processo", porque
     * "programas instalados" não é sobre processos em execução. */
    if (inventario && !cpu && !memoria) {
        rota(out, "inventario_de_programas", "inventário de programas instalados", ROTA_CONFIANCA_MEDIA,
             "tools.discover", "{}", INVENTARIO);
        goto fim;
    }

    /* 2. processos (explícito, ou "o que/quem mais consome ...") */
    /* "o que / quem / quais ... consome(m) memória" também é um pedido de
     * ranking, mesmo sem "mais" — mas "quanto de memória está em uso" não. */
    gboolean pergunta_quem = tem(t, L("que", "quem", "quais"), NULL);
    gboolean ranking_memoria = memoria && consumo && (superlativo || pergunta_quem) && !quantidade;
    gboolean ranking_cpu = cpu && consumo && !modelo && !nucleos;
    if (processo || ranking_memoria || ranking_cpu) {
        gboolean quer_mem = memoria, quer_cpu = cpu;
        if (!processo) { quer_mem = ranking_memoria; quer_cpu = ranking_cpu; }
        if (quer_mem && !quer_cpu) {
            rota(out, "processos_por_memoria", "processos por uso de memória RAM", ROTA_CONFIANCA_ALTA,
                 "sistema.processos_top_memoria", "{}", SO_MEM);
        } else if (quer_cpu && !quer_mem) {
            rota(out, "processos_por_cpu", "processos por uso de CPU", ROTA_CONFIANCA_ALTA,
                 "sistema.processos_top_cpu", "{}", SO_CPU);
        } else if (quer_cpu && quer_mem) {
            rota(out, "processos_cpu_ou_memoria", "processos (CPU ou memória — ambíguo)", ROTA_CONFIANCA_MEDIA,
                 NULL, "{}", CPU_OU_MEM);
            out->esclarecer = TRUE;
            out->mensagem = "O pedido fala de CPU e de memória ao mesmo tempo, e cada ferramenta ordena por um só critério. "
                            "Você quer os processos ordenados por uso de CPU ou por uso de memória RAM?";
        } else {
            rota(out, "processos_em_geral", "processos em execução", ROTA_CONFIANCA_MEDIA,
                 "sistema.processos_top_cpu", "{}", CPU_OU_MEM);
        }
        int limite = limite_explicito(t);
        if (limite && out->ferramenta) {
            g_snprintf(out->parametros_buf, sizeof out->parametros_buf, "{\"limite\":%d}", limite);
            out->parametros_json = out->parametros_buf;
        }
        goto fim;
    }

    /* 3. dispositivos */
    if (rede || bloco || pci || dispositivo) {
        if (bloco && espaco) {
            rota(out, "espaco_em_disco", "espaço livre/ocupado em disco", ROTA_CONFIANCA_ALTA, NULL, "{}", NULL);
            out->sem_ferramenta = TRUE;
            out->mensagem = "Ainda não existe uma ferramenta de leitura para espaço livre/ocupado em disco "
                            "(sistema.dispositivos só lista quais dispositivos de bloco existem, sem tamanho nem uso). "
                            "Nada foi executado.";
            goto fim;
        }
        int classes = (rede ? 1 : 0) + (bloco ? 1 : 0) + (pci ? 1 : 0);
        if (classes == 1 && rede)
            rota(out, "dispositivos_de_rede", "interfaces de rede", ROTA_CONFIANCA_ALTA, "sistema.dispositivos", "{\"classe\":\"rede\"}", SO_DISP);
        else if (classes == 1 && bloco)
            rota(out, "dispositivos_de_bloco", "dispositivos de bloco", ROTA_CONFIANCA_ALTA, "sistema.dispositivos", "{\"classe\":\"bloco\"}", SO_DISP);
        else if (classes == 1 && pci)
            rota(out, "dispositivos_pci", "dispositivos PCI", ROTA_CONFIANCA_ALTA, "sistema.dispositivos", "{\"classe\":\"pci\"}", SO_DISP);
        else
            rota(out, "dispositivos", "dispositivos (rede, bloco e PCI)", ROTA_CONFIANCA_ALTA, "sistema.dispositivos", "{}", SO_DISP);
        goto fim;
    }

    /* 4. identidade da máquina x recursos atuais */
    gboolean quer_diag = kernel || distro || nucleos || (cpu && (modelo || nucleos)) || (cpu && !consumo && !carga);
    gboolean quer_rec = swap || uptime || carga || memoria || recursos;
    if (quer_diag && quer_rec) {
        rota(out, "maquina_e_recursos", "identificação da máquina e recursos", ROTA_CONFIANCA_MEDIA,
             "sistema.diagnosticar", "{}", DIAG_OU_REC);
    } else if (quer_diag) {
        rota(out, "diagnostico_da_maquina", "CPU, kernel e distribuição", ROTA_CONFIANCA_ALTA,
             "sistema.diagnosticar", "{}", SO_DIAG);
    } else if (quer_rec) {
        rota(out, "recursos_do_sistema", "RAM, swap, uptime e carga média", ROTA_CONFIANCA_ALTA,
             "sistema.recursos", "{}", SO_REC);
    }

fim:
    g_strfreev(t);
}

/* ---------------------------------------------------------- decisão --- */

void roteador_decisao_liberar(RoteadorDecisao *d)
{
    if (!d) return;
    g_free(d->ferramenta);
    g_free(d->parametros_json);
    g_free(d->motivo);
    g_free(d->mensagem);
    memset(d, 0, sizeof *d);
}

static gboolean permitida(const Rota *r, const char *ferramenta)
{
    if (!ferramenta) return FALSE;
    for (int i = 0; r->permitidas[i]; i++)
        if (strcmp(r->permitidas[i], ferramenta) == 0)
            return TRUE;
    return FALSE;
}

static char *obj_para_texto(JsonObject *obj)
{
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(n, obj);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, n);
    char *s = json_generator_to_data(gen, NULL);
    g_object_unref(gen);
    json_node_unref(n);
    return s;
}

/* Remove de 'parametros_json' todo inteiro que não aparece, como número
 * solto, no pedido da pessoa (um parâmetro numérico só vale se a pessoa
 * de fato disse aquele número). Devolve o objeto resultante em texto;
 * *descartados recebe os nomes removidos ("limite=100"), ou fica vazio.
 * Entrada que não é um objeto JSON vira "{}" com *invalido = TRUE. */
static char *filtrar_inteiros_sem_origem(const char *parametros_json, const char *pedido,
                                          GString *descartados, gboolean *invalido)
{
    *invalido = FALSE;
    JsonParser *p = json_parser_new();
    if (!parametros_json || !json_parser_load_from_data(p, parametros_json, -1, NULL) ||
        !json_parser_get_root(p) || JSON_NODE_TYPE(json_parser_get_root(p)) != JSON_NODE_OBJECT) {
        if (parametros_json && *parametros_json)
            *invalido = TRUE;
        g_object_unref(p);
        return g_strdup("{}");
    }
    JsonObject *obj = json_node_get_object(json_parser_get_root(p));
    char **tokens = tokenizar(pedido);
    GList *chaves = json_object_get_members(obj);
    for (GList *it = chaves; it; it = it->next) {
        const char *chave = it->data;
        JsonNode *n = json_object_get_member(obj, chave);
        if (JSON_NODE_TYPE(n) != JSON_NODE_VALUE || json_node_get_value_type(n) != G_TYPE_INT64)
            continue;
        char numero[32];
        g_snprintf(numero, sizeof numero, "%" G_GINT64_FORMAT, json_node_get_int(n));
        gboolean achou = FALSE;
        for (int i = 0; tokens[i]; i++)
            if (strcmp(tokens[i], numero) == 0) { achou = TRUE; break; }
        if (!achou) {
            g_string_append_printf(descartados, "%s%s=%s", descartados->len ? ", " : "", chave, numero);
            json_object_remove_member(obj, chave);
        }
    }
    g_list_free(chaves);
    g_strfreev(tokens);
    char *saida = obj_para_texto(obj);
    g_object_unref(p);
    return saida;
}

/* Sobrepõe as chaves de 'fixos' (parâmetros da própria intenção) em 'base'. */
static char *sobrepor(const char *base_json, const char *fixos_json)
{
    JsonParser *pb = json_parser_new(), *pf = json_parser_new();
    char *saida = NULL;
    if (json_parser_load_from_data(pb, base_json, -1, NULL) && json_parser_load_from_data(pf, fixos_json, -1, NULL) &&
        JSON_NODE_TYPE(json_parser_get_root(pb)) == JSON_NODE_OBJECT &&
        JSON_NODE_TYPE(json_parser_get_root(pf)) == JSON_NODE_OBJECT) {
        JsonObject *b = json_node_get_object(json_parser_get_root(pb));
        JsonObject *f = json_node_get_object(json_parser_get_root(pf));
        GList *chaves = json_object_get_members(f);
        for (GList *it = chaves; it; it = it->next)
            json_object_set_member(b, it->data, json_node_copy(json_object_get_member(f, it->data)));
        g_list_free(chaves);
        saida = obj_para_texto(b);
    }
    g_object_unref(pb);
    g_object_unref(pf);
    return saida ? saida : g_strdup(fixos_json);
}

void roteador_decidir(const Rota *r, const char *pedido,
                       const char *ferramenta_modelo, const char *parametros_modelo,
                       const RoteadorManifesto *mf, gboolean pode_repetir,
                       RoteadorDecisao *out)
{
    memset(out, 0, sizeof *out);
    if (ferramenta_modelo && (!*ferramenta_modelo || strcmp(ferramenta_modelo, "nenhuma") == 0))
        ferramenta_modelo = NULL;

    /* intenção reconhecida, mas sem ferramenta que a responda: veto */
    if (r->intencao && r->sem_ferramenta) {
        out->veredito = ROTEADOR_SEM_FERRAMENTA;
        out->motivo = g_strdup_printf("intenção '%s' reconhecida, mas nenhuma ferramenta existente a responde%s%s%s",
                                       r->intencao, ferramenta_modelo ? " — proposta do modelo ('" : "",
                                       ferramenta_modelo ? ferramenta_modelo : "", ferramenta_modelo ? "') removida" : "");
        out->mensagem = g_strdup(r->mensagem);
        return;
    }

    /* a rota só vale se o manifesto ATIVO tiver ao menos uma das
     * ferramentas compatíveis (com o executor mock, por exemplo, nenhuma
     * ferramenta sistema.* existe: o roteador não interfere). */
    gboolean rota_ativa = FALSE;
    if (r->intencao)
        for (int i = 0; r->permitidas[i]; i++)
            if (mf->existe(r->permitidas[i], mf->dados)) { rota_ativa = TRUE; break; }

    GString *descartados = g_string_new(NULL);
    gboolean params_malformados = FALSE;

    if (rota_ativa) {
        const char *escolhida = NULL;
        gboolean do_modelo = FALSE;
        if (permitida(r, ferramenta_modelo) && mf->existe(ferramenta_modelo, mf->dados)) {
            escolhida = ferramenta_modelo;
            do_modelo = TRUE;
        } else if (r->esclarecer || !r->ferramenta || !mf->existe(r->ferramenta, mf->dados)) {
            out->veredito = ROTEADOR_ESCLARECER;
            out->motivo = g_strdup_printf("intenção '%s' é ambígua e o modelo não escolheu uma das ferramentas compatíveis", r->intencao);
            out->mensagem = g_strdup(r->mensagem ? r->mensagem : "O pedido é ambíguo; pode detalhar o que você quer ver?");
            g_string_free(descartados, TRUE);
            return;
        } else {
            escolhida = r->ferramenta;
        }

        char *params = do_modelo
            ? filtrar_inteiros_sem_origem(parametros_modelo, pedido, descartados, &params_malformados)
            : g_strdup("{}");
        gboolean canonica = r->ferramenta && strcmp(escolhida, r->ferramenta) == 0;
        if (canonica && strcmp(r->parametros_json, "{}") != 0) {
            char *com_fixos = sobrepor(params, r->parametros_json);
            g_free(params);
            params = com_fixos;
        }
        gboolean params_trocados = FALSE;
        char *erro = mf->validar(escolhida, params, mf->dados);
        if (erro) {
            /* nunca "conserta" um parâmetro inválido por palpite: volta ao
             * padrão do manifesto (só os parâmetros fixos da intenção). */
            g_free(params);
            params = g_strdup(canonica ? r->parametros_json : "{}");
            params_trocados = TRUE;
        }

        GString *motivo = g_string_new(NULL);
        if (do_modelo)
            g_string_append_printf(motivo, "intenção '%s': a ferramenta proposta pelo modelo é compatível", r->intencao);
        else if (ferramenta_modelo)
            g_string_append_printf(motivo, "intenção '%s' (confiança %s): o modelo propôs '%s', que é incompatível com este pedido — substituída por '%s'",
                                    r->intencao, r->confianca == ROTA_CONFIANCA_ALTA ? "alta" : "média", ferramenta_modelo, escolhida);
        else
            g_string_append_printf(motivo, "intenção '%s' (confiança %s): o modelo não propôs ferramenta — rota determinística escolheu '%s'",
                                    r->intencao, r->confianca == ROTA_CONFIANCA_ALTA ? "alta" : "média", escolhida);
        if (descartados->len)
            g_string_append_printf(motivo, "; parâmetro numérico sem origem no pedido descartado (%s)", descartados->str);
        if (params_trocados)
            g_string_append_printf(motivo, "; parâmetros do modelo inválidos (%s) — usados os padrões do manifesto", erro);
        if (params_malformados)
            g_string_append(motivo, "; parâmetros do modelo não eram um objeto JSON — ignorados");
        g_free(erro);

        out->veredito = (do_modelo && !descartados->len && !params_trocados && !params_malformados &&
                          !(canonica && strcmp(r->parametros_json, "{}") != 0 && g_strcmp0(parametros_modelo, params) != 0))
                         ? ROTEADOR_ACEITA : ROTEADOR_CORRIGIDA;
        out->ferramenta = g_strdup(escolhida);
        out->parametros_json = params;
        out->motivo = g_string_free(motivo, FALSE);
        g_string_free(descartados, TRUE);
        return;
    }

    /* sem rota ativa: só a validação de manifesto */
    if (!ferramenta_modelo) {
        out->veredito = ROTEADOR_ACEITA;
        out->motivo = g_strdup("resposta só em texto: nenhuma ferramenta proposta, nenhuma intenção de leitura reconhecida");
        g_string_free(descartados, TRUE);
        return;
    }
    if (!mf->existe(ferramenta_modelo, mf->dados)) {
        out->veredito = pode_repetir ? ROTEADOR_REPETIR : ROTEADOR_REJEITADA;
        out->motivo = g_strdup_printf("o modelo propôs '%s', que não existe no manifesto", ferramenta_modelo);
        if (!pode_repetir)
            out->mensagem = g_strdup_printf("A proposta do modelo foi rejeitada: a ferramenta '%s' não existe. Nada foi executado.", ferramenta_modelo);
        g_string_free(descartados, TRUE);
        return;
    }
    char *params = filtrar_inteiros_sem_origem(parametros_modelo, pedido, descartados, &params_malformados);
    char *erro = params_malformados ? g_strdup("parâmetros não são um objeto JSON") : mf->validar(ferramenta_modelo, params, mf->dados);
    if (erro) {
        out->veredito = pode_repetir ? ROTEADOR_REPETIR : ROTEADOR_REJEITADA;
        out->motivo = g_strdup_printf("parâmetros propostos para '%s' são inválidos: %s", ferramenta_modelo, erro);
        if (!pode_repetir)
            out->mensagem = g_strdup_printf("A proposta do modelo foi rejeitada: parâmetros inválidos para '%s' (%s). Nada foi executado.",
                                             ferramenta_modelo, erro);
        g_free(erro);
        g_free(params);
        g_string_free(descartados, TRUE);
        return;
    }
    out->veredito = descartados->len ? ROTEADOR_CORRIGIDA : ROTEADOR_ACEITA;
    out->ferramenta = g_strdup(ferramenta_modelo);
    out->parametros_json = params;
    out->motivo = descartados->len
        ? g_strdup_printf("ferramenta do modelo válida no manifesto; parâmetro numérico sem origem no pedido descartado (%s)", descartados->str)
        : g_strdup("ferramenta e parâmetros do modelo válidos no manifesto (intenção não classificada pelo roteador)");
    g_string_free(descartados, TRUE);
}
