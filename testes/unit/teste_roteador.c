/* Testes unitários do roteador de intenção (sessiond/roteador.c) e da
 * apresentação/fundamentação (sessiond/apresentacao.c). Sem daemon, sem
 * modelo, sem rede: só código puro + um manifesto de mentira com as
 * mesmas regras de parâmetro do real. Uso: make teste-roteador */
#include "../../sessiond/roteador.h"
#include "../../sessiond/apresentacao.h"
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>

static int total, falhas;
#define checar(c, d) checar_((c) ? TRUE : FALSE, (d))
static void checar_(gboolean cond, const char *desc)
{
    total++;
    if (!cond) falhas++;
    printf("%s %s\n", cond ? "ok  " : "FALHA", desc);
}

/* ---- manifesto de mentira (mesmos nomes/regras de sessiond/manifesto_real.c) ---- */
static const char *const REAIS[] = {
    "sistema.diagnosticar", "sistema.recursos", "sistema.dispositivos", "sistema.processos_top_cpu",
    "sistema.processos_top_memoria", "tools.discover", "tools.list", "tools.probe", NULL };

static gboolean existe(const char *f, gpointer d)
{
    (void)d;
    for (int i = 0; REAIS[i]; i++) if (!strcmp(REAIS[i], f)) return TRUE;
    return FALSE;
}
static gboolean existe_nada(const char *f, gpointer d) { (void)f; (void)d; return FALSE; }

static char *validar(const char *f, const char *pj, gpointer d)
{
    (void)d;
    JsonParser *p = json_parser_new();
    char *erro = NULL;
    if (!json_parser_load_from_data(p, pj, -1, NULL) || JSON_NODE_TYPE(json_parser_get_root(p)) != JSON_NODE_OBJECT) {
        g_object_unref(p);
        return g_strdup("não é objeto");
    }
    JsonObject *o = json_node_get_object(json_parser_get_root(p));
    GList *ch = json_object_get_members(o);
    for (GList *it = ch; it && !erro; it = it->next) {
        const char *k = it->data;
        JsonNode *n = json_object_get_member(o, k);
        if (!strcmp(k, "limite") && strstr(f, "processos_top")) {
            if (json_node_get_value_type(n) != G_TYPE_INT64 || json_node_get_int(n) < 1 || json_node_get_int(n) > 100)
                erro = g_strdup("limite inválido");
        } else if (!strcmp(k, "classe") && !strcmp(f, "sistema.dispositivos")) {
            const char *v = json_node_get_value_type(n) == G_TYPE_STRING ? json_node_get_string(n) : "";
            if (strcmp(v, "rede") && strcmp(v, "bloco") && strcmp(v, "pci")) erro = g_strdup("classe inválida");
        } else if (!strcmp(k, "estado") && !strcmp(f, "tools.list")) {
            const char *v = json_node_get_value_type(n) == G_TYPE_STRING ? json_node_get_string(n) : "";
            if (strcmp(v, "DISCOVERED")) erro = g_strdup("estado fora do enum");
        } else
            erro = g_strdup_printf("parâmetro desconhecido: %s", k);
    }
    g_list_free(ch);
    g_object_unref(p);
    return erro;
}
static const RoteadorManifesto MF = { existe, validar, NULL };
static const RoteadorManifesto MF_VAZIO = { existe_nada, validar, NULL };

/* ---- classificação ---- */
static void espera_rota(const char *pedido, const char *intencao, const char *ferramenta, const char *params)
{
    Rota r;
    roteador_classificar(pedido, &r);
    gboolean ok = g_strcmp0(r.intencao, intencao) == 0 && g_strcmp0(r.ferramenta, ferramenta) == 0 &&
                  (!params || g_strcmp0(r.parametros_json, params) == 0);
    char *d = g_strdup_printf("classificar: \"%s\" -> %s / %s%s%s", pedido, intencao ? intencao : "(nenhuma)",
                              ferramenta ? ferramenta : "(sem ferramenta)", ok ? "" : "   [obtido: ", ok ? "" : (r.intencao ? r.intencao : "nenhuma"));
    checar(ok, d);
    g_free(d);
}

static void testes_classificacao(void)
{
    /* as dez perguntas de aceite */
    espera_rota("Liste os 10 processos que mais consomem CPU agora.", "processos_por_cpu", "sistema.processos_top_cpu", "{\"limite\":10}");
    espera_rota("Liste os 10 processos que mais consomem memória RAM agora.", "processos_por_memoria", "sistema.processos_top_memoria", "{\"limite\":10}");
    espera_rota("Quanto de RAM e swap estão disponíveis?", "recursos_do_sistema", "sistema.recursos", "{}");
    espera_rota("Qual é a carga média e o uptime do sistema?", "recursos_do_sistema", "sistema.recursos", "{}");
    espera_rota("Qual é o modelo da CPU e quantos núcleos existem?", "diagnostico_da_maquina", "sistema.diagnosticar", "{}");
    espera_rota("Qual kernel e distribuição estão em execução?", "diagnostico_da_maquina", "sistema.diagnosticar", "{}");
    espera_rota("Quais interfaces de rede existem?", "dispositivos_de_rede", "sistema.dispositivos", "{\"classe\":\"rede\"}");
    espera_rota("Quais dispositivos de bloco existem?", "dispositivos_de_bloco", "sistema.dispositivos", "{\"classe\":\"bloco\"}");
    espera_rota("Quais dispositivos PCI existem?", "dispositivos_pci", "sistema.dispositivos", "{\"classe\":\"pci\"}");
    espera_rota("Faça um resumo objetivo dos recursos atuais desta máquina.", "recursos_do_sistema", "sistema.recursos", "{}");
    /* a pergunta que reproduziu o defeito */
    espera_rota("liste pra mim os processos que mais consumiram memoria ram", "processos_por_memoria", "sistema.processos_top_memoria", "{}");
    espera_rota("lista pra mim os 10 processos que mais consomem cpu", "processos_por_cpu", "sistema.processos_top_cpu", "{\"limite\":10}");
    espera_rota("top 5 processos por memória", "processos_por_memoria", "sistema.processos_top_memoria", "{\"limite\":5}");
    espera_rota("os 3 maiores processos em uso de cpu", "processos_por_cpu", "sistema.processos_top_cpu", "{\"limite\":3}");
    espera_rota("mostre os 500 processos que mais usam memória", "processos_por_memoria", "sistema.processos_top_memoria", "{}"); /* fora de 1..100: padrão, nunca "ajustado" */
    espera_rota("processos que usam mais memória nos últimos 5 minutos", "processos_por_memoria", "sistema.processos_top_memoria", "{}"); /* 5 não é quantidade de processos */
    /* variações: intenção, não palavra solta */
    espera_rota("o que está devorando minha RAM?", "processos_por_memoria", "sistema.processos_top_memoria", "{}");
    espera_rota("qual o uso de memória?", "recursos_do_sistema", "sistema.recursos", "{}"); /* uso total, não ranking */
    espera_rota("o que mais consome memória aqui?", "processos_por_memoria", "sistema.processos_top_memoria", "{}");
    espera_rota("quem está usando mais cpu", "processos_por_cpu", "sistema.processos_top_cpu", "{}");
    espera_rota("quais programas estão gastando mais memória?", "processos_por_memoria", "sistema.processos_top_memoria", "{}");
    espera_rota("qual o modelo do processador?", "diagnostico_da_maquina", "sistema.diagnosticar", "{}"); /* processador != processo */
    espera_rota("quanta memória tenho livre?", "recursos_do_sistema", "sistema.recursos", "{}"); /* memória sem processo != ranking */
    espera_rota("quanto de memória está em uso no total?", "recursos_do_sistema", "sistema.recursos", "{}");
    espera_rota("mostre os discos", "dispositivos_de_bloco", "sistema.dispositivos", "{\"classe\":\"bloco\"}");
    espera_rota("que hardware tem nessa máquina?", "dispositivos", "sistema.dispositivos", "{}");
    espera_rota("placas de rede e discos", "dispositivos", "sistema.dispositivos", "{}");
    espera_rota("quais processos estão rodando?", "processos_em_geral", "sistema.processos_top_cpu", "{}");
    espera_rota("quais programas estão instalados?", "inventario_de_programas", "tools.discover", "{}");
    espera_rota("MOCK_PROPOR:tools.discover.persistir", NULL, NULL, NULL); /* "discover" não é "disco" */
    espera_rota("abra o discord", NULL, NULL, NULL);
    espera_rota("quais partições existem?", "dispositivos_de_bloco", "sistema.dispositivos", "{\"classe\":\"bloco\"}");
    espera_rota("oi, tudo bem?", NULL, NULL, NULL);
    espera_rota("explique o que é um inode", NULL, NULL, NULL);
    espera_rota("", NULL, NULL, NULL);

    Rota r;
    roteador_classificar("processos que mais usam cpu e memória", &r);
    checar(r.esclarecer && !r.ferramenta && r.mensagem, "classificar: CPU e memória juntos -> ambíguo, pede esclarecimento, sem ferramenta canônica");
    roteador_classificar("quanto espaço livre tem no disco?", &r);
    checar(r.sem_ferramenta && !r.ferramenta && r.mensagem, "classificar: espaço em disco -> intenção reconhecida SEM ferramenta (veto, não reaproveita dispositivos)");
    roteador_classificar("Liste os 10 processos que mais consomem memória RAM agora.", &r);
    gboolean so_mem = r.permitidas[0] && !strcmp(r.permitidas[0], "sistema.processos_top_memoria") && !r.permitidas[1];
    checar(so_mem && r.confianca == ROTA_CONFIANCA_ALTA, "classificar: processos por memória só admite sistema.processos_top_memoria, confiança alta");
}

/* ---- decisão ---- */
static void decide(const char *pedido, const char *fm, const char *pm, gboolean repetir, RoteadorDecisao *d)
{
    Rota r;
    roteador_classificar(pedido, &r);
    roteador_decidir(&r, pedido, fm, pm, &MF, repetir, d);
}

static void testes_decisao(void)
{
    RoteadorDecisao d;
    const char *P_MEM = "liste pra mim os processos que mais consumiram memoria ram";

    decide(P_MEM, "sistema.recursos", "{}", TRUE, &d);
    checar(d.veredito == ROTEADOR_CORRIGIDA && !g_strcmp0(d.ferramenta, "sistema.processos_top_memoria") && !g_strcmp0(d.parametros_json, "{}"),
           "decidir: processos por memória + modelo propõe sistema.recursos -> corrigida para processos_top_memoria");
    checar(d.motivo && strstr(d.motivo, "sistema.recursos") && strstr(d.motivo, "incompatível"), "decidir: o motivo cita a ferramenta rejeitada");
    roteador_decisao_liberar(&d);

    decide(P_MEM, "sistema.dispositivos", "{}", TRUE, &d);
    checar(!g_strcmp0(d.ferramenta, "sistema.processos_top_memoria"), "decidir: sistema.dispositivos nunca responde pergunta de processos");
    roteador_decisao_liberar(&d);

    decide(P_MEM, "sistema.processos_top_cpu", "{}", TRUE, &d);
    checar(!g_strcmp0(d.ferramenta, "sistema.processos_top_memoria"), "decidir: top_cpu nunca responde pergunta de memória");
    roteador_decisao_liberar(&d);

    decide("Liste os 10 processos que mais consomem CPU agora.", "sistema.dispositivos", "{}", TRUE, &d);
    checar(d.veredito == ROTEADOR_CORRIGIDA && !g_strcmp0(d.ferramenta, "sistema.processos_top_cpu"), "decidir: processos por CPU + sistema.dispositivos -> corrigida para top_cpu");
    roteador_decisao_liberar(&d);

    decide(P_MEM, NULL, NULL, TRUE, &d);
    checar(d.veredito == ROTEADOR_CORRIGIDA && !g_strcmp0(d.ferramenta, "sistema.processos_top_memoria"), "decidir: modelo sem proposta + intenção alta -> rota determinística");
    roteador_decisao_liberar(&d);

    decide(P_MEM, "ferramenta.que_nao_existe", "{}", TRUE, &d);
    checar(!g_strcmp0(d.ferramenta, "sistema.processos_top_memoria"), "decidir: ferramenta inventada + intenção alta -> substituída pela real");
    roteador_decisao_liberar(&d);

    decide(P_MEM, "sistema.processos_top_memoria", "{\"limite\":100}", TRUE, &d);
    checar(d.veredito == ROTEADOR_CORRIGIDA && !g_strcmp0(d.parametros_json, "{}") && strstr(d.motivo, "limite=100"),
           "decidir: limite=100 que a pessoa nunca pediu é descartado (volta ao padrão)");
    roteador_decisao_liberar(&d);

    decide("Liste os 10 processos que mais consomem memória RAM agora.", "sistema.processos_top_memoria", "{\"limite\":10}", TRUE, &d);
    checar(d.veredito == ROTEADOR_ACEITA && !g_strcmp0(d.parametros_json, "{\"limite\":10}"), "decidir: limite=10 dito pela pessoa é mantido, proposta aceita como veio");
    roteador_decisao_liberar(&d);
    decide("top 5 processos por memória", NULL, NULL, FALSE, &d);
    checar(!g_strcmp0(d.ferramenta, "sistema.processos_top_memoria") && !g_strcmp0(d.parametros_json, "{\"limite\":5}"), "decidir: sem modelo, o limite dito pela pessoa (5) vem da própria rota");
    roteador_decisao_liberar(&d);

    decide("mostre os 500 processos que mais usam memória", "sistema.processos_top_memoria", "{\"limite\":500}", TRUE, &d);
    checar(!g_strcmp0(d.ferramenta, "sistema.processos_top_memoria") && !g_strcmp0(d.parametros_json, "{}") && strstr(d.motivo, "inválidos"),
           "decidir: limite fora da faixa nunca é 'consertado' — cai no padrão do manifesto");
    roteador_decisao_liberar(&d);

    decide(P_MEM, "sistema.processos_top_memoria", "{\"limite\":\"10\",\"inventado\":true}", TRUE, &d);
    checar(!g_strcmp0(d.parametros_json, "{}"), "decidir: parâmetro com tipo errado/desconhecido -> padrão do manifesto");
    roteador_decisao_liberar(&d);

    decide("Quais interfaces de rede existem?", "sistema.dispositivos", "{}", TRUE, &d);
    checar(!g_strcmp0(d.ferramenta, "sistema.dispositivos") && !g_strcmp0(d.parametros_json, "{\"classe\":\"rede\"}"),
           "decidir: interfaces de rede -> sistema.dispositivos com classe=rede (parâmetro fixo da intenção)");
    roteador_decisao_liberar(&d);

    decide("Quais dispositivos PCI existem?", "sistema.dispositivos", "{\"classe\":\"bloco\"}", TRUE, &d);
    checar(!g_strcmp0(d.parametros_json, "{\"classe\":\"pci\"}"), "decidir: classe errada do modelo é sobreposta pela da intenção");
    roteador_decisao_liberar(&d);

    decide("quanto espaço livre tem no disco?", "sistema.dispositivos", "{}", TRUE, &d);
    checar(d.veredito == ROTEADOR_SEM_FERRAMENTA && !d.ferramenta && d.mensagem, "decidir: intenção sem ferramenta -> proposta removida, mensagem clara");
    roteador_decisao_liberar(&d);

    decide("processos que mais usam cpu e memória", "sistema.recursos", "{}", TRUE, &d);
    checar(d.veredito == ROTEADOR_ESCLARECER && !d.ferramenta && d.mensagem, "decidir: ambíguo + escolha incompatível do modelo -> pede esclarecimento");
    roteador_decisao_liberar(&d);
    decide("processos que mais usam cpu e memória", "sistema.processos_top_cpu", "{}", TRUE, &d);
    checar(d.veredito == ROTEADOR_ACEITA && !g_strcmp0(d.ferramenta, "sistema.processos_top_cpu"), "decidir: ambíguo + escolha compatível do modelo -> aceita");
    roteador_decisao_liberar(&d);

    /* sem intenção reconhecida: só validação de manifesto */
    decide("oi, tudo bem?", NULL, NULL, TRUE, &d);
    checar(d.veredito == ROTEADOR_ACEITA && !d.ferramenta, "decidir: conversa sem ferramenta -> aceita, nada proposto");
    roteador_decisao_liberar(&d);
    decide("oi, tudo bem?", "nenhuma", "{}", TRUE, &d);
    checar(d.veredito == ROTEADOR_ACEITA && !d.ferramenta, "decidir: ferramenta \"nenhuma\" == sem proposta");
    roteador_decisao_liberar(&d);
    decide("faça algo", "sudo.rm_rf", "{}", TRUE, &d);
    checar(d.veredito == ROTEADOR_REPETIR && !d.ferramenta, "decidir: ferramenta inexistente, sem rota -> pede UMA nova tentativa");
    roteador_decisao_liberar(&d);
    decide("faça algo", "sudo.rm_rf", "{}", FALSE, &d);
    checar(d.veredito == ROTEADOR_REJEITADA && !d.ferramenta && d.mensagem, "decidir: ferramenta inexistente após a tentativa -> rejeitada, nada executável");
    roteador_decisao_liberar(&d);
    decide("faça algo", "tools.list", "{\"estado\":\"BOGUS\"}", FALSE, &d);
    checar(d.veredito == ROTEADOR_REJEITADA && !d.ferramenta, "decidir: parâmetros inválidos, sem rota -> rejeitada (nunca fabrica parâmetro)");
    roteador_decisao_liberar(&d);
    decide("faça algo", "tools.list", "isto não é json", FALSE, &d);
    checar(d.veredito == ROTEADOR_REJEITADA, "decidir: parâmetros que não são JSON -> rejeitada");
    roteador_decisao_liberar(&d);
    decide("faça algo", "tools.list", "{}", TRUE, &d);
    checar(d.veredito == ROTEADOR_ACEITA && !g_strcmp0(d.ferramenta, "tools.list"), "decidir: ferramenta real e parâmetros válidos, sem rota -> aceita");
    roteador_decisao_liberar(&d);

    /* executor sem nenhuma ferramenta sistema.* (mock): o roteador não interfere */
    Rota r;
    roteador_classificar(P_MEM, &r);
    roteador_decidir(&r, P_MEM, NULL, NULL, &MF_VAZIO, TRUE, &d);
    checar(d.veredito == ROTEADOR_ACEITA && !d.ferramenta, "decidir: manifesto ativo sem as ferramentas da rota -> rota inativa, nada inventado");
    roteador_decisao_liberar(&d);
}

/* ---- apresentação ---- */
static void testes_apresentacao(void)
{
    gboolean trunc = TRUE;
    char *t = apresentacao_renderizar("sistema.processos_top_memoria",
        "{\"saida\":{\"ordenar_por\":\"memoria\",\"limite\":2,\"considerados\":195,\"sem_memoria_de_usuario\":153,\"memoria_total_kb\":16297868,"
        "\"processos\":[{\"pid\":659559,\"nome\":\"llama-server\",\"estado\":\"S\",\"rss_kb\":2579292,\"mem_pct_centesimos\":1583},"
        "{\"pid\":5124,\"nome\":\"fire\\u001b[31mfox\",\"estado\":\"R\",\"rss_kb\":543980,\"mem_pct_centesimos\":334}]}}", &trunc);
    checar(strstr(t, "659559") && strstr(t, "llama-server") && strstr(t, "2519") && strstr(t, "15,83") && strstr(t, "3,34"),
           "renderizar memória: PID, nome, RSS em MiB e percentual reais");
    checar(strstr(t, "195") && strstr(t, "15915 MiB") && strstr(t, "dormindo") && strstr(t, "executando"), "renderizar memória: totais e estado legível");
    checar(!strchr(t, 0x1b), "renderizar: caractere de controle num nome de processo nunca chega ao terminal/prompt");
    checar(!trunc, "renderizar: saída pequena não é marcada como truncada");
    g_free(t);

    t = apresentacao_renderizar("sistema.processos_top_cpu",
        "{\"saida\":{\"limite\":1,\"intervalo_ms\":200,\"amostrados\":348,\"processos\":[{\"pid\":77,\"nome\":\"cc1\",\"estado\":\"R\",\"cpu_pct_centesimos\":1234,\"tempo_cpu_centesimos_s\":4567}]}}", NULL);
    checar(strstr(t, "12,34") && strstr(t, "348") && strstr(t, "200 ms") && strstr(t, "cc1") && strstr(t, " 45\n"), "renderizar CPU: percentual, amostra e tempo de CPU reais");
    g_free(t);

    t = apresentacao_renderizar("sistema.recursos",
        "{\"saida\":{\"cpu\":{\"nucleos\":8,\"modelo\":\"Intel i7\"},\"memoria_kb\":{\"total\":16297868,\"livre\":831280,\"disponivel\":8622624},"
        "\"swap_kb\":{\"total\":20971512,\"livre\":18222380},\"uptime_s\":131169.04,\"uptime_segundos\":131169,"
        "\"carga_media_centesimos\":{\"1min\":73,\"5min\":80,\"15min\":48}}}", NULL);
    checar(strstr(t, "15915 MiB") && strstr(t, "8420 MiB") && strstr(t, "20479 MiB") && strstr(t, "17795 MiB"), "renderizar recursos: RAM e swap em MiB");
    checar(strstr(t, "0,73 (1 min)") && strstr(t, "0,48 (15 min)") && strstr(t, "1 dias, 12 horas e 26 minutos"), "renderizar recursos: carga média e uptime");
    g_free(t);
    t = apresentacao_renderizar("sistema.recursos", "{\"saida\":{\"cpu\":{\"nucleos\":1,\"modelo\":\"x\"},\"uptime_s\":5}}", NULL);
    checar(strstr(t, "Carga média: não disponível"), "renderizar recursos: carga ausente é dita como ausente, nunca um zero inventado");
    g_free(t);

    t = apresentacao_renderizar("sistema.diagnosticar",
        "{\"saida\":{\"distro\":\"Arch Linux\",\"kernel\":{\"sysname\":\"Linux\",\"release\":\"7.2.15-arch1-1\",\"machine\":\"x86_64\"},"
        "\"cpu\":{\"nucleos\":8,\"modelo\":\"Intel i7\"},\"memoria_kb\":{\"total\":2048,\"disponivel\":1024},\"usuario\":{\"uid\":1000,\"euid\":1000}}}", NULL);
    checar(strstr(t, "Arch Linux") && strstr(t, "Linux 7.2.15-arch1-1 (x86_64)") && strstr(t, "8 núcleos"), "renderizar diagnóstico: distro, kernel, CPU");
    g_free(t);

    t = apresentacao_renderizar("sistema.dispositivos",
        "{\"saida\":[{\"classe\":\"rede\",\"nome\":\"lo\"},{\"classe\":\"rede\",\"nome\":\"eno1\"},{\"classe\":\"pci\",\"nome\":\"0000:00:02.0\",\"vendor\":\"0x8086\",\"dispositivo\":\"0x0416\"}]}", NULL);
    checar(strstr(t, "Interfaces de rede (2)") && strstr(t, "eno1") && strstr(t, "Dispositivos PCI (1)") && strstr(t, "0x8086") && !strstr(t, "bloco"),
           "renderizar dispositivos: agrupado por classe, só as classes presentes");
    g_free(t);
    t = apresentacao_renderizar("sistema.dispositivos", "{\"saida\":[]}", NULL);
    checar(strstr(t, "Nenhum dispositivo"), "renderizar dispositivos: lista vazia é dita como vazia");
    g_free(t);

    t = apresentacao_renderizar("tools.discover", "{\"texto\":\"linha 1\\nlinha 2\"}", NULL);
    checar(strstr(t, "linha 1\nlinha 2"), "renderizar: ferramenta de texto mostra o texto");
    g_free(t);
    t = apresentacao_renderizar("x", "isto não é json", NULL);
    checar(strstr(t, "não devolveu dados"), "renderizar: JSON malformado vira aviso, nunca conteúdo inventado");
    g_free(t);
    t = apresentacao_renderizar("x", NULL, NULL);
    checar(t && *t, "renderizar: dados ausentes nunca devolvem NULL");
    g_free(t);

    GString *g = g_string_new("{\"texto\":\"");
    for (int i = 0; i < 2000; i++) g_string_append_printf(g, "item %d\\n", i);
    g_string_append(g, "\"}");
    t = apresentacao_renderizar("tools.discover", g->str, &trunc);
    checar(trunc && strlen(t) < LIA_APRESENTACAO_LIMITE_BYTES + 200 && strstr(t, "INCOMPLETA"), "renderizar: saída grande é cortada no limite e AVISA que está incompleta");
    g_free(t);
    g_string_free(g, TRUE);
}

static gboolean fund(const char *resumo, const char *ev1, const char *ev2)
{
    const char *evs[] = { ev1, ev2, NULL };
    return apresentacao_resumo_fundamentado(resumo, evs, NULL);
}

static void testes_fundamentacao(void)
{
    const char *EV = "   1  659559   llama-server      dormindo    2519        15,83\n   2  5124     firefox           dormindo    531         3,34\nRAM total: 15915 MiB — 2 de 195 processos";
    const char *PED = "Liste os 10 processos que mais consomem memória RAM agora.";
    checar(fund("O llama-server (PID 659559) usa 2519 MiB, ou 15,83% da RAM.", EV, PED), "fundamentado: números copiados da evidência");
    checar(fund("O llama-server usa 15.83% e o firefox 3.34%.", EV, PED), "fundamentado: ponto decimal no lugar da vírgula");
    checar(fund("O llama-server usa cerca de 15,8% (2.519 MiB) de 15.915 MiB.", EV, PED), "fundamentado: arredondamento na última casa e separador de milhar");
    checar(fund("Dos 10 processos pedidos, 2 foram listados.", EV, PED), "fundamentado: número que veio do pedido da pessoa");
    checar(fund("1. llama-server\n2. firefox\n3) outro", EV, PED), "fundamentado: numeração de lista não conta como valor");
    checar(fund("Nenhum número aqui, só nomes: llama-server e firefox.", EV, PED), "fundamentado: resumo sem números");
    checar(!fund("O llama-server usa 4096 MiB.", EV, PED), "NÃO fundamentado: valor que a ferramenta não devolveu");
    checar(!fund("O firefox usa 33,4% da RAM.", EV, PED), "NÃO fundamentado: vírgula deslocada (3,34 virou 33,4)");
    checar(!fund("O chrome (PID 4242) usa 800 MiB.", EV, PED), "NÃO fundamentado: PID e valor inventados");
    checar(!fund("São 64 GB de RAM.", EV, PED), "NÃO fundamentado: conversão/total inventado");
    char *sus = NULL;
    const char *evs[] = { EV, PED, NULL };
    checar(!apresentacao_resumo_fundamentado("usa 4096 MiB", evs, &sus) && !g_strcmp0(sus, "4096"), "NÃO fundamentado: devolve qual número não tem origem");
    g_free(sus);
    checar(fund("O kernel é o 7.2.15-arch1-1 em x86_64.", "Kernel: Linux 7.2.15-arch1-1 (x86_64)", ""), "fundamentado: versão de kernel e nomes com dígitos colados");
    checar(fund("", EV, PED) && fund(NULL, EV, PED), "fundamentado: resumo vazio/NULL não quebra");
}

static void testes_ranking_e_frase(void)
{
    const char *J = "{\"saida\":{\"ordenar_por\":\"memoria\",\"limite\":4,\"considerados\":9,\"sem_memoria_de_usuario\":1,\"memoria_total_kb\":16297868,\"processos\":["
        "{\"pid\":1,\"nome\":\"llama-server\",\"estado\":\"S\",\"rss_kb\":2702336,\"mem_pct_centesimos\":1658},"
        "{\"pid\":2,\"nome\":\"teams-for-linux\",\"estado\":\"S\",\"rss_kb\":544768,\"mem_pct_centesimos\":335},"
        "{\"pid\":3,\"nome\":\"firefox\",\"estado\":\"S\",\"rss_kb\":518144,\"mem_pct_centesimos\":318},"
        "{\"pid\":4,\"nome\":\"code\",\"estado\":\"S\",\"rss_kb\":452608,\"mem_pct_centesimos\":278},"
        "{\"pid\":5,\"nome\":\"sh\",\"estado\":\"S\",\"rss_kb\":300000,\"mem_pct_centesimos\":184},"
        "{\"pid\":6,\"nome\":\"code\",\"estado\":\"S\",\"rss_kb\":200000,\"mem_pct_centesimos\":123}]}}";
    const char *F = "sistema.processos_top_memoria";
    char *motivo = NULL;
    checar(!apresentacao_resumo_respeita_ranking("Os que mais consomem são o \"llama-server\" e o \"code\", com 2639 MiB e 442 MiB.", F, J, &motivo),
           "ranking: citar o 1º e o 4º pulando o 2º e o 3º é REJEITADO (caso real visto com o llama3.2:3b)");
    checar(motivo && strstr(motivo, "teams-for-linux"), "ranking: o motivo diz quem foi pulado");
    g_free(motivo);
    checar(apresentacao_resumo_respeita_ranking("O maior é o llama-server, seguido de teams-for-linux e firefox.", F, J, NULL), "ranking: citar os três primeiros na ordem passa");
    checar(apresentacao_resumo_respeita_ranking("O llama-server é o que mais usa memória.", F, J, NULL), "ranking: citar só o primeiro passa");
    checar(apresentacao_resumo_respeita_ranking("llama-server, teams-for-linux, firefox e code lideram.", F, J, NULL), "ranking: nome repetido mais abaixo (code em 4º e 6º) não gera falso alarme");
    checar(apresentacao_resumo_respeita_ranking("Há vários processos listados na tabela.", F, J, NULL), "ranking: resumo sem nomes passa");
    checar(apresentacao_resumo_respeita_ranking("O llama-server shows up first.", F, J, NULL), "ranking: 'sh' dentro de outra palavra não conta como citar o processo sh");
    checar(!apresentacao_resumo_respeita_ranking("O firefox é o que mais consome.", F, J, NULL), "ranking: apontar o 3º como se fosse o primeiro é rejeitado");
    checar(apresentacao_resumo_respeita_ranking("qualquer coisa com firefox", "sistema.recursos", J, NULL), "ranking: ferramenta que não é ranking nunca é barrada por isto");

    char *t = apresentacao_frase(F, J);
    checar(strstr(t, "llama-server (PID 1), com 2639 MiB (16,58 % da RAM)") && strstr(t, "teams-for-linux (532 MiB)") && strstr(t, "firefox (506 MiB)"),
           "frase determinística (memória): 1º com PID/RSS/percentual, 2º e 3º na ordem");
    const char *ev[] = { t, J, NULL };
    char *ap = apresentacao_renderizar(F, J, NULL);
    const char *ev2[] = { ap, J, NULL };
    checar(apresentacao_resumo_fundamentado(t, ev2, NULL) && apresentacao_resumo_respeita_ranking(t, F, J, NULL),
           "frase determinística passa nas MESMAS checagens aplicadas ao modelo (números e ordem)");
    (void)ev;
    g_free(ap);
    g_free(t);
    t = apresentacao_frase("sistema.processos_top_cpu", "{\"saida\":{\"limite\":1,\"intervalo_ms\":200,\"amostrados\":3,\"processos\":[{\"pid\":77,\"nome\":\"cc1\",\"estado\":\"R\",\"cpu_pct_centesimos\":1234,\"tempo_cpu_centesimos_s\":4567}]}}");
    checar(strstr(t, "cc1 (PID 77), com 12,34 % de CPU"), "frase determinística (CPU): um único processo");
    g_free(t);
    t = apresentacao_frase("sistema.recursos", "{\"saida\":{\"memoria_kb\":{\"total\":16297868,\"livre\":831280,\"disponivel\":8622624},\"swap_kb\":{\"total\":20971512,\"livre\":18222380},\"uptime_segundos\":131169,\"carga_media_centesimos\":{\"1min\":73,\"5min\":80,\"15min\":48}}}");
    checar(strstr(t, "8420 MiB de RAM disponíveis de 15915 MiB") && strstr(t, "17795 MiB de swap livres") && strstr(t, "0,73 (1 min)") && strstr(t, "1 dias, 12 horas e 26 minutos"),
           "frase determinística (recursos): RAM, swap, carga e uptime");
    g_free(t);
    t = apresentacao_frase("sistema.diagnosticar", "{\"saida\":{\"distro\":\"Arch Linux\",\"kernel\":{\"sysname\":\"Linux\",\"release\":\"7.2.15-arch1-1\",\"machine\":\"x86_64\"},\"cpu\":{\"nucleos\":8,\"modelo\":\"Intel i7\"}}}");
    checar(strstr(t, "Arch Linux com kernel Linux 7.2.15-arch1-1 (x86_64)") && strstr(t, "Intel i7, com 8 núcleos"), "frase determinística (diagnóstico)");
    g_free(t);
    t = apresentacao_frase("sistema.dispositivos", "{\"saida\":[{\"classe\":\"rede\",\"nome\":\"lo\"},{\"classe\":\"rede\",\"nome\":\"eno1\"}]}");
    checar(strstr(t, "Existem 2 interfaces de rede: lo, eno1."), "frase determinística (dispositivos de rede)");
    g_free(t);
    t = apresentacao_frase("tools.discover", "{\"texto\":\"x\"}");
    checar(strstr(t, "nenhum valor além deles"), "frase determinística: ferramenta sem frase própria aponta para os dados, sem inventar");
    g_free(t);
    t = apresentacao_frase("sistema.recursos", "lixo");
    checar(t && *t, "frase determinística: JSON malformado nunca devolve NULL nem inventa");
    g_free(t);
}

static void testes_extrativa(void)
{
    const char *AP = "CPU: 8 núcleos — Intel i7\nRAM: 15915 MiB no total; 811 MiB livres; 8420 MiB disponíveis (52,91 % disponível)\n"
                     "Swap: 20479 MiB no total; 17795 MiB livres; 2684 MiB em uso\nUptime: 1 dias, 12 horas e 26 minutos (131169 segundos)\n"
                     "Carga média: 0,73 (1 min), 0,80 (5 min), 0,48 (15 min)\n";
    checar(apresentacao_modo_resposta("sistema.processos_top_memoria") == RESPOSTA_DETERMINISTICA &&
           apresentacao_modo_resposta("sistema.processos_top_cpu") == RESPOSTA_DETERMINISTICA &&
           apresentacao_modo_resposta("sistema.dispositivos") == RESPOSTA_DETERMINISTICA,
           "modo de resposta: ranking e listas são determinísticos (sem modelo)");
    checar(apresentacao_modo_resposta("sistema.recursos") == RESPOSTA_EXTRATIVA && apresentacao_modo_resposta("sistema.diagnosticar") == RESPOSTA_EXTRATIVA,
           "modo de resposta: recursos/diagnóstico são extrativos");
    checar(apresentacao_modo_resposta("tools.discover") == RESPOSTA_RESUMO_CONFERIDO && apresentacao_modo_resposta(NULL) == RESPOSTA_RESUMO_CONFERIDO,
           "modo de resposta: demais ferramentas usam resumo conferido");
    int n = 0;
    char *num = apresentacao_numerar_linhas(AP, &n);
    checar(n == 5 && g_str_has_prefix(num, "1. CPU: 8") && strstr(num, "\n5. Carga média"), "numerar linhas para o prompt de seleção");
    g_free(num);

    int do_modelo = -1;
    char *t = apresentacao_resposta_extrativa(AP, "Quanto de RAM e swap estão disponíveis?", NULL, 0, &do_modelo);
    checar(strstr(t, "RAM: 15915 MiB") && strstr(t, "Swap: 20479 MiB") && !strstr(t, "Uptime") && !strstr(t, "Carga") && !strstr(t, "CPU:") && do_modelo == 0,
           "extrativa sem modelo: 'RAM e swap' -> só as linhas de RAM e Swap");
    g_free(t);
    t = apresentacao_resposta_extrativa(AP, "Qual é a carga média e o uptime do sistema?", NULL, 0, NULL);
    checar(strstr(t, "Uptime: 1 dias") && strstr(t, "Carga média: 0,73") && !strstr(t, "RAM:") && !strstr(t, "Swap:"), "extrativa: 'carga média e uptime' -> só essas duas linhas");
    g_free(t);
    int sel[] = { 1, 999, -3, 1 };
    t = apresentacao_resposta_extrativa(AP, "tem swap?", sel, 4, &do_modelo);
    checar(strstr(t, "CPU: 8 núcleos") && strstr(t, "Swap:") && !strstr(t, "RAM:") && do_modelo == 1,
           "extrativa: união da escolha do modelo (linha 1) com o que foi perguntado (swap); índices fora da faixa e repetidos ignorados");
    g_free(t);
    int ruim[] = { 4 };
    t = apresentacao_resposta_extrativa(AP, "Quanto de RAM tenho?", ruim, 1, NULL);
    checar(strstr(t, "RAM: 15915") && strstr(t, "Uptime:"), "extrativa: escolha ruim do modelo nunca deixa de fora a linha perguntada (RAM)");
    g_free(t);
    t = apresentacao_resposta_extrativa(AP, "Faça um resumo objetivo dos recursos atuais desta máquina.", NULL, 0, NULL);
    checar(strstr(t, "CPU:") && strstr(t, "RAM:") && strstr(t, "Swap:") && strstr(t, "Uptime:") && strstr(t, "Carga média:"),
           "extrativa: pedido genérico, sem escolha nenhuma -> todas as linhas");
    const char *ev[] = { AP, NULL };
    checar(apresentacao_resumo_fundamentado(t, ev, NULL), "extrativa: todo número da resposta existe nos dados (por construção)");
    g_free(t);
    t = apresentacao_resposta_extrativa("", "qualquer", NULL, 0, NULL);
    checar(t && strstr(t, "nenhuma linha"), "extrativa: sem dados -> diz isso, nunca inventa");
    g_free(t);
    t = apresentacao_resposta_extrativa("Distribuição: Arch Linux\nKernel: Linux 7.2.15-arch1-1 (x86_64)\nCPU: Intel i7 — 8 núcleos\n", "Qual kernel e distribuição estão em execução?", NULL, 0, NULL);
    checar(strstr(t, "Distribuição: Arch Linux") && strstr(t, "Kernel: Linux 7.2.15") && !strstr(t, "CPU:"), "extrativa (diagnóstico): kernel e distribuição");
    g_free(t);
    t = apresentacao_resposta_extrativa("Distribuição: Arch Linux\nKernel: Linux 7.2.15-arch1-1 (x86_64)\nCPU: Intel i7 — 8 núcleos\n", "Qual é o modelo da CPU e quantos núcleos existem?", NULL, 0, NULL);
    checar(strstr(t, "CPU: Intel i7 — 8 núcleos") && !strstr(t, "Kernel"), "extrativa (diagnóstico): modelo e núcleos da CPU");
    g_free(t);

    checar(apresentacao_resumo_e_recusa("Não posso fornecer a lista de 10 processos, pois os dados fornecidos não incluem essa informação."),
           "recusa: 'não posso fornecer... não incluem' é detectada (caso real visto com o llama3.2:3b)");
    checar(apresentacao_resumo_e_recusa("Infelizmente não há dados suficientes."), "recusa: 'infelizmente / não há dados suficientes'");
    checar(!apresentacao_resumo_e_recusa("O kernel é o Linux 7.2.15.") && !apresentacao_resumo_e_recusa(NULL) && !apresentacao_resumo_e_recusa(""),
           "recusa: resposta afirmativa normal não é confundida com recusa");
}

int main(void)
{
    testes_classificacao();
    testes_decisao();
    testes_apresentacao();
    testes_fundamentacao();
    testes_ranking_e_frase();
    testes_extrativa();
    printf("\n%d testes, %d falhas\n", total, falhas);
    return falhas ? 1 : 0;
}
