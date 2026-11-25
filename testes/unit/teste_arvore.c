/* Árvore (DAG) de tarefas: ids, ciclos, alcance das evidências, regra de
 * promoção (filho que passa NÃO prova o pai), orçamento, repetição,
 * serialização e artefatos. Uso: make teste-arvore */
#include "../../sessiond/arvore.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int total, falhas;
static void checar(gboolean c, const char *d)
{
    total++;
    if (c) printf("ok   %s\n", d);
    else { printf("FALHA %s\n", d); falhas++; }
}

static Arvore *exemplo(void)
{
    Arvore *a = arvore_nova("codigo-teste");
    const char *const esc[] = { "src/a.c", NULL };
    g_free(arvore_adicionar(a, "raiz", NULL, NULL, "a validação passa com a correção de a.c", esc));
    g_free(arvore_adicionar(a, "contexto", "raiz", NULL, "ler o código citado", NULL));
    const char *const dep[] = { "contexto", NULL };
    g_free(arvore_adicionar(a, "proposta-1", "raiz", dep, "propor a correção", esc));
    return a;
}

int main(void)
{
    char *e;
    Arvore *a = exemplo();
    checar(a->nos->len == 3 && !arvore_validar(a), "árvore com raiz, contexto e proposta dependente é válida");

    e = arvore_adicionar(a, "raiz", NULL, NULL, "outra", NULL);
    checar(e != NULL, "id repetido / segunda raiz recusados"); g_free(e);
    e = arvore_adicionar(a, "x", "nao-existe", NULL, "x", NULL);
    checar(e && strstr(e, "não existe"), "pai inexistente recusado"); g_free(e);
    const char *const dep_ruim[] = { "fantasma", NULL };
    e = arvore_adicionar(a, "y", "raiz", dep_ruim, "y", NULL);
    checar(e && strstr(e, "dependência"), "dependência inexistente recusada"); g_free(e);
    e = arvore_adicionar(a, "a b", "raiz", NULL, "z", NULL);
    checar(e && strstr(e, "inválido"), "id com espaço recusado"); g_free(e);

    /* ciclo via dependências (só dá para criar editando/carregando): validar detecta */
    TarefaNo *ctx = arvore_no(a, "contexto");
    g_strfreev(ctx->deps);
    ctx->deps = g_strsplit("proposta-1", ",", -1);
    e = arvore_validar(a);
    checar(e && strstr(e, "ciclo"), "ciclo (contexto ↔ proposta-1) detectado");
    g_free(e);
    char *json_ciclo = arvore_para_json(a);
    Arvore *b = arvore_de_json(json_ciclo, &e);
    checar(!b && e && strstr(e, "ciclo"), "JSON com ciclo é recusado ao carregar");
    g_free(e); g_free(json_ciclo);
    g_strfreev(ctx->deps);
    ctx->deps = g_new0(char *, 1);
    checar(!arvore_validar(a), "sem a aresta, volta a ser DAG");

    /* tentativas: orçamento e repetição */
    TarefaNo *p1 = arvore_no(a, "proposta-1");
    p1->max_tentativas = 2;
    checar(!arvore_tentativa(a, "proposta-1", "h1", "art1"), "1ª tentativa registrada");
    e = arvore_tentativa(a, "proposta-1", "h1", "art1b");
    checar(e && strstr(e, "repetição sem progresso"), "tentativa idêntica (mesmo hash) é repetição sem progresso"); g_free(e);
    checar(!arvore_tentativa(a, "proposta-1", "h2", "art2"), "2ª tentativa diferente registrada");
    e = arvore_tentativa(a, "proposta-1", "h3", "art3");
    checar(e && strstr(e, "orçamento"), "3ª tentativa: orçamento esgotado"); g_free(e);
    NoTentativa *t1 = g_ptr_array_index(p1->tentativas, 0), *t2 = g_ptr_array_index(p1->tentativas, 1);
    checar(strcmp(t1->artefato, t2->artefato) && t1->seq < t2->seq, "tentativas não se sobrescrevem (artefatos e ordem próprios)");

    p1->max_tokens = 100;
    checar(arvore_consumir(a, "proposta-1", 10, 60) && !arvore_consumir(a, "proposta-1", 10, 60), "orçamento de tokens do nó estoura no limite");

    /* evidências e promoção */
    arvore_marcar(a, "contexto", NO_PASSOU, "li src/a.c");
    guint c0 = arvore_criterio(a, "raiz", "a validação \"teste\" passa depois da última edição", "pedido");
    char *arq[] = { "src/a.c", NULL }, *antes[] = { "aaaa1111aaaa2222", NULL }, *depois[] = { "bbbb1111bbbb2222", NULL };
    gint64 s1 = arvore_patch(a, "proposta-1", arq, antes, depois, "patch1");
    /* o FILHO tem validação própria que passou: ele passa... */
    arvore_validacao(a, "proposta-1", "teste", 0, TRUE, arq, depois, "log1");
    char *m = NULL;
    NoEstado ep = arvore_avaliar(a, "proposta-1", &m);
    checar(ep == NO_PASSOU, "filho com validação própria depois do patch passa");
    g_free(m);
    /* ...mas isso NÃO promove a raiz */
    arvore_conferir(a, "raiz", c0, TRUE, "conferido");
    NoEstado er = arvore_avaliar(a, "raiz", &m);
    checar(er == NO_FALHOU && strstr(m, "nenhuma validação própria"), "filho que passou NÃO prova o pai (raiz sem validação própria falha)");
    g_free(m);
    /* validação da raiz depois do patch: passa */
    gint64 v1 = arvore_validacao(a, "raiz", "teste", 0, TRUE, arq, depois, "log2");
    er = arvore_avaliar(a, "raiz", &m);
    checar(er == NO_PASSOU && v1 > s1, "raiz passa com critério conferido e validação própria depois do último patch");
    g_free(m);
    /* novo patch num filho DEPOIS da validação: a prova da raiz caduca */
    char *depois2[] = { "cccc1111cccc2222", NULL };
    arvore_patch(a, "proposta-1", arq, depois, depois2, "patch2");
    er = arvore_avaliar(a, "raiz", &m);
    checar(er == NO_FALHOU && strstr(m, "anterior ao último patch"), "patch depois da validação invalida o 'passou' da raiz");
    g_free(m);
    /* validação nova que falha: a última manda */
    arvore_validacao(a, "raiz", "teste", 1, FALSE, arq, depois2, "log3");
    er = arvore_avaliar(a, "raiz", &m);
    checar(er == NO_FALHOU && strstr(m, "última validação"), "a última validação (falhou) manda, não uma antiga que passou");
    g_free(m);
    arvore_validacao(a, "raiz", "teste", 0, TRUE, arq, depois2, "log4");
    /* critério não atendido impede mesmo com validação ok */
    arvore_conferir(a, "raiz", c0, FALSE, "não");
    er = arvore_avaliar(a, "raiz", &m);
    checar(er == NO_FALHOU && strstr(m, "critério não atendido"), "critério próprio não atendido impede o 'passou'");
    g_free(m);
    arvore_conferir(a, "raiz", c0, TRUE, "sim");
    /* dependência que não passou bloqueia */
    arvore_marcar(a, "contexto", NO_FALHOU, "não li");
    ep = arvore_avaliar(a, "proposta-1", &m);
    checar(ep == NO_BLOQUEADO, "dependência que não passou bloqueia o nó");
    g_free(m);
    arvore_marcar(a, "contexto", NO_PASSOU, "li");

    /* alcance declarado */
    TarefaNo *r = arvore_no(a, "raiz");
    NoEvidencia *ev = g_ptr_array_index(r->evidencias, r->evidencias->len - 1);
    checar(strstr(ev->alcance, "validação \"teste\" passou") && strstr(ev->alcance, "src/a.c@cccc1111cccc") &&
           strstr(ev->alcance, "depois do patch #") && strstr(ev->alcance, "vale só para"), "evidência declara alcance: comando, arquivo@hash, depois de qual patch");
    NoEvidencia *pt = g_ptr_array_index(p1->evidencias, 0);
    checar(strstr(pt->alcance, "não que está certo") != NULL, "evidência de patch não alega correção");

    /* ida e volta pelo JSON */
    char *j = arvore_para_json(a);
    b = arvore_de_json(j, &e);
    char *j2 = b ? arvore_para_json(b) : NULL;
    checar(b && j2 && !strcmp(j, j2), "serialização ida e volta é estável");
    checar(strstr(j, "\"versao\" : \"lia-arvore-tarefas/1\"") != NULL, "JSON tem versão");
    checar(!strstr(j, "int soma") && !strstr(j, "patch1\n"), "JSON guarda hashes/artefatos, não código");
    arvore_liberar(b);
    g_free(j2);
    char *velho = g_strdup("{\"versao\":\"lia-arvore-tarefas/0\",\"nos\":[]}");
    b = arvore_de_json(velho, &e);
    checar(!b && e && strstr(e, "versão"), "versão desconhecida recusada");
    g_free(e); g_free(velho);
    g_free(j);

    /* eventos JSONL */
    char *ev_txt = arvore_eventos_pendentes(a);
    gchar **linhas = g_strsplit(ev_txt, "\n", -1);
    gboolean todas_json = TRUE;
    int n_linhas = 0;
    for (int i = 0; linhas[i] && *linhas[i]; i++) {
        n_linhas++;
        if (linhas[i][0] != '{' || !strstr(linhas[i], "\"tipo\"")) todas_json = FALSE;
    }
    checar(n_linhas > 5 && todas_json, "eventos saem como JSONL (uma linha por evento)");
    char *vazio = arvore_eventos_pendentes(a);
    checar(!*vazio, "eventos pendentes esvaziam depois de lidos");
    g_free(vazio); g_strfreev(linhas); g_free(ev_txt);
    arvore_liberar(a);

    /* artefatos endereçados por conteúdo */
    char *dir = g_dir_make_tmp("lia-arvore-XXXXXX", NULL);
    char *art = g_build_filename(dir, "artefatos", NULL);
    char *h1 = artefato_gravar(art, "diff --git a/x\n", 15, &e);
    char *h2 = artefato_gravar(art, "diff --git a/x\n", 15, &e);
    char *cam = h1 ? g_build_filename(art, h1, NULL) : NULL;
    struct stat st;
    checar(h1 && h2 && !strcmp(h1, h2) && cam && stat(cam, &st) == 0 && (st.st_mode & 0777) == 0600 && st.st_size == 15,
           "artefato: sha256 do conteúdo, 0600, mesmo conteúdo não duplica");
    char *esperado = g_compute_checksum_for_string(G_CHECKSUM_SHA256, "diff --git a/x\n", -1);
    checar(h1 && !strcmp(h1, esperado), "artefato: nome é o sha256 do conteúdo");
    g_free(esperado); g_free(cam); g_free(h1); g_free(h2);
    char *cmd = g_strdup_printf("rm -rf -- '%s'", dir);
    if (g_str_has_prefix(dir, "/tmp/lia-arvore-")) { int rc = system(cmd); (void)rc; }
    g_free(cmd); g_free(art); g_free(dir);

    printf("\n%d testes, %d falhas\n", total, falhas);
    return falhas ? 1 : 0;
}
