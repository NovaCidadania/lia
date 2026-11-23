/* Testes da resposta final de pesquisas web: extração de texto/título/data
 * de HTML, trecho relevante, fontes, cotação, conferência da síntese do
 * modelo e resposta extrativa. Sem rede, sem modelo. Uso: make teste-sintese-web */
#include "../../sessiond/sintese_web.h"
#include "../../web/pesquisa.h"
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

/* ------------------------------------------------------------ HTML --- */
static void testes_html(void)
{
    const char *html =
        "<!DOCTYPE html><html><head><title>Linguagem Exemplar &ndash; Enciclopédia</title>"
        "<meta property=\"article:published_time\" content=\"2024-05-01T10:00:00Z\">"
        "<script>var x = '<p>não é texto</p>'; function f(){}</script><style>p{color:red}</style></head>"
        "<body><nav><a href=\"/\">Início</a> | <a href=\"/x\">Menu</a></nav>"
        "<div data-mw='{\"a\":\"<span>></span>\"}'>Cabeçalho</div>"
        "<p>A linguagem Exemplar &eacute; funcional e foi criada em 1990 por um comit&ecirc; acad&ecirc;mico.<sup>[1]</sup></p>"
        "<p>Em 1998 saiu o padr&atilde;o Exemplar 98, base de ensino e pesquisa em muitas universidades.</p>"
        "<footer>Todos os direitos reservados</footer><!-- comentário <p>oculto</p> --></body></html>";
    char *t = lia_web_html_para_texto(html, strlen(html));
    checar(strstr(t, "A linguagem Exemplar é funcional e foi criada em 1990 por um comitê acadêmico.") != NULL,
           "html: entidades decodificadas e parágrafo preservado");
    checar(!strstr(t, "não é texto") && !strstr(t, "color:red") && !strstr(t, "Menu") && !strstr(t, "direitos") && !strstr(t, "oculto"),
           "html: script, style, nav, footer e comentários ficam de fora");
    checar(!strstr(t, "</span>") && !strstr(t, "\"}") && strstr(t, "Cabeçalho"), "html: '>' dentro de atributo entre aspas não quebra a remoção de tags");
    checar(!strstr(t, "[1]"), "html: marcas de referência [1] removidas");
    checar(strchr(t, '\n') != NULL, "html: blocos viram quebras de linha (parágrafos separados)");
    g_free(t);
    char *ruim = g_strdup("<p>texto com byte inválido \xff\xfe no meio, ainda legível</p>");
    t = lia_web_html_para_texto(ruim, strlen(ruim));
    checar(g_utf8_validate(t, -1, NULL) && strstr(t, "ainda legível"), "html: UTF-8 inválido vira texto válido");
    g_free(t); g_free(ruim);

    char *titulo = lia_web_html_titulo(html);
    checar(titulo && !strcmp(titulo, "Linguagem Exemplar – Enciclopédia"), "título: <title> com entidade");
    g_free(titulo);
    char *data = lia_web_html_data(html);
    checar(data && !strcmp(data, "2024-05-01"), "data: article:published_time normalizada para AAAA-MM-DD");
    g_free(data);
    data = lia_web_html_data("<script type=\"application/ld+json\">{\"dateModified\":\"2025-02-03T12:00:00Z\"}</script>");
    checar(data && !strcmp(data, "2025-02-03"), "data: dateModified de JSON-LD");
    g_free(data);
    data = lia_web_html_data("<p>sem data</p>");
    checar(data == NULL, "data: página sem data declarada -> NULL (nunca inventada)");
}

static void testes_trecho(void)
{
    const char *texto =
        "Início Menu Contato\n"
        "As referências deste artigo necessitam de formatação. Por favor, utilize fontes apropriadas contendo título, autor e data.\n"
        "Haskell é uma linguagem de programação puramente funcional, de propósito geral, nomeada em homenagem ao lógico Haskell Curry.\n"
        "Receitas de bolo de cenoura com cobertura de chocolate são muito populares no Brasil, especialmente nas festas de família.\n"
        "A evolução da linguagem passou pelo padrão Haskell 98 e depois pelo Haskell 2010, com extensões implementadas no compilador GHC.\n";
    char *t = lia_web_trecho_relevante(texto, "evolução da linguagem Haskell", 1800);
    checar(strstr(t, "puramente funcional") && strstr(t, "Haskell 2010"), "trecho: abertura e parágrafo relevante entram");
    checar(!strstr(t, "bolo de cenoura"), "trecho: parágrafo sem relação com a pergunta fica de fora");
    checar(!strstr(t, "necessitam de formatação") && !strstr(t, "Menu Contato"), "trecho: aviso de manutenção e menu ficam de fora");
    g_free(t);
    t = lia_web_trecho_relevante(texto, "evolução da linguagem Haskell", 150);
    checar(g_utf8_strlen(t, -1) <= 200, "trecho: respeita o limite de caracteres");
    g_free(t);
    t = lia_web_trecho_relevante("", "x", 100);
    checar(t && !*t, "trecho: texto vazio -> vazio, nunca NULL");
    g_free(t);
    char **termos = lia_web_termos("Me conte com detalhes a evolução da linguagem Haskell?");
    checar(g_strv_contains((const char *const *)termos, "evolucao") && g_strv_contains((const char *const *)termos, "haskell") &&
           !g_strv_contains((const char *const *)termos, "conte") && !g_strv_contains((const char *const *)termos, "detalhe"),
           "termos: sem acento, sem palavras de pedido ('conte', 'detalhes')");
    g_strfreev(termos);
}

/* ---------------------------------------------------------- fontes --- */
static const char *PESQUISA =
    "{\"consulta\":\"evolução da linguagem Exemplar\",\"fonte\":\"DuckDuckGo HTML\",\"fontes_abertas\":2,\"resultados\":["
    "{\"titulo\":\"Exemplar - Enciclopédia\",\"url\":\"https://enciclopedia.exemplo/exemplar\",\"resumo\":\"resumo curto da busca\",\"aberta\":true,"
    "\"data\":\"2024-05-01\",\"conteudo\":\"A linguagem Exemplar é funcional e foi criada em 1990 por um comitê acadêmico. "
    "Em 1998 saiu o padrão Exemplar 98, usado no ensino.\"},"
    "{\"titulo\":\"Blog sem data\",\"url\":\"https://blog.exemplo/historia\",\"resumo\":\"x\",\"aberta\":true,"
    "\"conteudo\":\"O compilador principal da linguagem Exemplar ganhou suporte a concorrência em 2010, segundo a equipe do projeto.\"},"
    "{\"titulo\":\"PDF fora do ar\",\"url\":\"https://pdf.exemplo/a.pdf\",\"resumo\":\"Um resumo de busca razoavelmente longo sobre a linguagem Exemplar e sua história.\","
    "\"aberta\":false,\"erro_abertura\":\"a página respondeu HTTP 404\"},"
    "{\"titulo\":\"Não HTTPS\",\"url\":\"http://inseguro.exemplo/\",\"resumo\":\"ignorado\",\"aberta\":false}]}";

static GPtrArray *fontes_padrao(void)
{
    const char *dados[] = { PESQUISA, NULL };
    return sintese_web_fontes(dados, "evolução da linguagem Exemplar", 4);
}

static void testes_fontes(void)
{
    GPtrArray *f = fontes_padrao();
    checar(f->len == 2, "fontes: com 2 páginas lidas, resumos de busca e URLs não-HTTPS ficam de fora");
    FonteWeb *a = g_ptr_array_index(f, 0), *b = g_ptr_array_index(f, 1);
    checar(a->lida && !strcmp(a->url, "https://enciclopedia.exemplo/exemplar") && !strcmp(a->data, "2024-05-01") && strstr(a->trecho, "1990"),
           "fontes: título, URL, data e trecho preservados");
    checar(b->lida && b->data == NULL, "fontes: fonte sem data fica sem data (nunca inventada)");
    char *sec = sintese_web_secao_fontes(f);
    checar(strstr(sec, "[1] Exemplar - Enciclopédia") && strstr(sec, "https://blog.exemplo/historia") &&
           strstr(sec, "data declarada pela página: 2024-05-01") && strstr(sec, "a página não declara data"),
           "fontes: seção numerada com URL e data (ou ausência dela)");
    g_free(sec);
    char *pr = sintese_web_fontes_para_prompt(f);
    checar(strstr(pr, "[1] Exemplar - Enciclopédia") && strstr(pr, "[2] Blog sem data") && !strstr(pr, "2024-05-01"),
           "prompt: fontes numeradas; a data da PÁGINA não vai (o modelo a confundia com data de evento)");
    g_free(pr);
    g_ptr_array_unref(f);

    const char *uma_lida =
        "{\"resultados\":[{\"titulo\":\"Única\",\"url\":\"https://u.exemplo/\",\"resumo\":\"r\",\"aberta\":true,"
        "\"conteudo\":\"Texto lido da única página com conteúdo suficiente para ser uma fonte.\"},"
        "{\"titulo\":\"Só resumo\",\"url\":\"https://r.exemplo/\",\"resumo\":\"Resumo de busca longo o bastante para servir de apoio à resposta.\",\"aberta\":false}]}";
    const char *d1[] = { uma_lida, NULL };
    f = sintese_web_fontes(d1, "x", 4);
    checar(f->len == 2 && !((FonteWeb *)g_ptr_array_index(f, 1))->lida, "fontes: com uma só página lida, o resumo de busca completa (marcado como não lido)");
    g_ptr_array_unref(f);

    const char *abrir = "{\"url\":\"https://doc.exemplo/manual\",\"titulo\":\"Manual\",\"conteudo\":\"Menu\\n"
                        "O manual explica que a função principal da linguagem Exemplar recebe argumentos imutáveis e devolve valores novos sempre.\"}";
    const char *d2[] = { abrir, "não é json", "{\"resultados\":[]}", NULL };
    f = sintese_web_fontes(d2, "função principal da linguagem Exemplar", 4);
    checar(f->len == 1 && !strcmp(((FonteWeb *)g_ptr_array_index(f, 0))->titulo, "Manual") && strstr(((FonteWeb *)g_ptr_array_index(f, 0))->trecho, "imutáveis"),
           "fontes: página aberta com web.abrir vira fonte com o trecho relevante; dado malformado e busca vazia são ignorados");
    g_ptr_array_unref(f);
}

/* --------------------------------------------------------- cotação --- */
static void testes_cotacao(void)
{
    const char *c = "{\"consulta\":\"dolar em reais\",\"fonte\":\"Frankfurter / European Central Bank\",\"cotacoes\":["
                    "{\"par\":\"dólar americano para real brasileiro\",\"base\":\"USD\",\"destino\":\"BRL\",\"valor\":5.6637,\"data\":\"2026-11-13\"}],"
                    "\"resultados\":[{\"titulo\":\"Frankfurter\",\"url\":\"https://api.frankfurter.dev/v1/latest?base=USD&symbols=BRL\",\"resumo\":\"x\"}]}";
    const char *d[] = { c, NULL };
    char *t = sintese_web_cotacao(d);
    checar(t && g_str_has_prefix(t, "Resposta: 1 USD = 5,6637 BRL (dólar americano para real brasileiro)"), "cotação: resposta conclusiva com valor e par");
    checar(t && strstr(t, "data de referência 2026-11-13") && strstr(t, "Frankfurter / European Central Bank") &&
           strstr(t, "https://api.frankfurter.dev/v1/latest?base=USD&symbols=BRL"), "cotação: data, fonte e URL");
    checar(t && strstr(t, "não a cotação de compra/venda"), "cotação: explica que é taxa de referência");
    g_free(t);
    const char *dois = "{\"fonte\":\"F\",\"cotacoes\":[{\"base\":\"GBP\",\"destino\":\"USD\",\"valor\":1.27,\"data\":\"2026-11-13\"},"
                       "{\"base\":\"GBP\",\"destino\":\"BRL\",\"valor\":7.2,\"data\":\"2026-11-13\"}]}";
    const char *d2[] = { dois, NULL };
    t = sintese_web_cotacao(d2);
    checar(t && strstr(t, "1 GBP = 1,2700 USD") && strstr(t, "1 GBP = 7,2000 BRL"), "cotação: dois pares numa resposta");
    g_free(t);
    const char *d3[] = { PESQUISA, NULL };
    checar(sintese_web_cotacao(d3) == NULL, "cotação: pesquisa comum não é cotação");
}

/* --------------------------------------------------------- síntese --- */
static void testes_sintese(void)
{
    GPtrArray *f = fontes_padrao();
    int desc = -1;
    char *motivo = NULL;
    const char *bom =
        "{\"resposta\":\"A linguagem Exemplar é funcional, foi criada em 1990 e ganhou o padrão Exemplar 98 em 1998.\","
        "\"pontos\":[{\"texto\":\"Foi criada em 1990 por um comitê acadêmico.\",\"fontes\":[1]},"
        "{\"texto\":\"O padrão Exemplar 98 saiu em 1998 e é usado no ensino.\",\"fontes\":[1]},"
        "{\"texto\":\"O compilador ganhou suporte a concorrência em 2010.\",\"fontes\":[2]}],"
        "\"linha_do_tempo\":[{\"quando\":\"1990\",\"evento\":\"criação da linguagem por um comitê\",\"fontes\":[1]},"
        "{\"quando\":\"2010\",\"evento\":\"compilador ganha suporte a concorrência\",\"fontes\":[2]}],\"lacunas\":\"As fontes não dizem quem mantém o projeto hoje.\"}";
    char *t = sintese_web_renderizar(bom, f, "evolução da linguagem Exemplar", &desc, &motivo);
    checar(t && g_str_has_prefix(t, "Resposta: A linguagem Exemplar é funcional"), "síntese: começa pela resposta conclusiva");
    checar(t && strstr(t, "Pontos principais:\n  • Foi criada em 1990 por um comitê acadêmico. [1]") &&
           strstr(t, "concorrência em 2010. [2]"), "síntese: pontos com citação da fonte");
    checar(t && strstr(t, "Linha do tempo:\n  • 1990 — criação da linguagem por um comitê [1]"), "síntese: linha do tempo com ano e fonte");
    checar(t && strstr(t, "O que as fontes não esclarecem: Quem mantém o projeto hoje"), "síntese: lacunas (sem o prefixo redundante)");
    checar(t && strstr(t, "Fontes:\n  [1] Exemplar - Enciclopédia") && strstr(t, "https://blog.exemplo/historia"), "síntese: seção de fontes com URLs");
    checar(desc == 0 && motivo == NULL, "síntese: nada descartado quando tudo confere");
    g_free(t); g_free(motivo); motivo = NULL;

    const char *inventada =
        "{\"resposta\":\"A linguagem Exemplar foi criada em 1777 [1].\","
        "\"pontos\":[{\"texto\":\"A versão 9.99 saiu em 2031.\",\"fontes\":[1]},"
        "{\"texto\":\"Foi criada em 1990 por um comitê acadêmico.\",\"fontes\":[2]},"
        "{\"texto\":\"A linguagem Exemplar ganhou o padrão Exemplar 98 usado no ensino.\",\"fontes\":[7]}],"
        "\"linha_do_tempo\":[{\"quando\":\"2031\",\"evento\":\"lançamento da versão 9.99\",\"fontes\":[1]}],\"lacunas\":\"\"}";
    t = sintese_web_renderizar(inventada, f, "evolução da linguagem Exemplar", &desc, &motivo);
    checar(t && !strstr(t, "1777") && !strstr(t, "9.99") && !strstr(t, "2031"), "síntese: números que as fontes não têm nunca aparecem");
    checar(t && g_str_has_prefix(t, "Resposta: Foi criada em 1990 por um comitê acadêmico. [1]"),
           "síntese: conclusão inventada é trocada pelo fato conferido; citação errada ([2]) corrigida para a fonte que tem o número ([1])");
    checar(t && strstr(t, "padrão Exemplar 98 usado no ensino. [1]"), "síntese: citação de fonte inexistente ([7]) reatribuída pela fonte que sustenta o texto");
    checar(desc >= 2 && motivo && strstr(motivo, "conclusão do modelo foi trocada"), "síntese: descartes contados e explicados");
    g_free(t); g_free(motivo); motivo = NULL;

    const char *pontos_texto = "```json\n{\"resposta\":\"Exemplar é uma linguagem funcional criada por um comitê acadêmico.\","
                               "\"pontos\":[\"O compilador principal da linguagem ganhou suporte a concorrência.\",\"Receita de bolo de cenoura com chocolate.\"],"
                               "\"linha_do_tempo\":[],\"lacunas\":\"fato curto\"}\n```";
    t = sintese_web_renderizar(pontos_texto, f, "linguagem Exemplar", &desc, &motivo);
    checar(t && strstr(t, "ganhou suporte a concorrência. [2]"), "síntese: JSON entre cercas de código e ponto em texto simples -> fonte atribuída pelo conteúdo");
    checar(t && !strstr(t, "bolo"), "síntese: ponto que nenhuma fonte sustenta é descartado");
    checar(t && !strstr(t, "não esclarecem"), "síntese: 'lacunas' que só ecoam o formato pedido são ignoradas");
    g_free(t); g_free(motivo); motivo = NULL;

    const char *cortado = "{\"resposta\":\"A linguagem Exemplar é funcional e foi criada em 1990.\",\"pontos\":["
                          "{\"texto\":\"O padrão Exemplar 98 saiu em 1998.\",\"fontes\":[1]},{\"texto\":\"Em 2010 o compi";
    t = sintese_web_renderizar(cortado, f, "linguagem Exemplar", &desc, &motivo);
    checar(t && strstr(t, "criada em 1990") && strstr(t, "Exemplar 98 saiu em 1998. [1]") && !strstr(t, "Em 2010 o compi"),
           "síntese: saída cortada no limite de tokens aproveita só os itens completos");
    checar(motivo && strstr(motivo, "cortada"), "síntese: o corte é informado");
    g_free(t); g_free(motivo); motivo = NULL;

    t = sintese_web_renderizar("isto não é JSON", f, "x", &desc, &motivo);
    checar(t == NULL && motivo, "síntese: resposta que não é JSON -> NULL (vai para a extrativa)");
    g_free(motivo); motivo = NULL;
    t = sintese_web_renderizar("{\"resposta\":\"Não posso fornecer essa informação.\",\"pontos\":[],\"linha_do_tempo\":[],\"lacunas\":\"\"}", f, "x", &desc, &motivo);
    checar(t == NULL, "síntese: recusa sem nenhum ponto conferido -> NULL");
    g_free(motivo); motivo = NULL;
    t = sintese_web_renderizar("{\"resposta\":\"Exemplar 98 [1] e concorrência [2]\\u001b[31m.\",\"pontos\":[],\"linha_do_tempo\":[],\"lacunas\":\"\"}", f, "x", &desc, &motivo);
    checar(!t || !strchr(t, 0x1b), "síntese: caractere de controle vindo do modelo nunca chega ao terminal");
    g_free(t); g_free(motivo);
    g_ptr_array_unref(f);
}

static void testes_extrativa(void)
{
    GPtrArray *f = fontes_padrao();
    char *t = sintese_web_extrativa(f, "Me conte a história da linguagem Exemplar", "o modelo não respondeu (limite_excedido)");
    checar(t && g_str_has_prefix(t, "Resposta: A linguagem Exemplar é funcional e foi criada em 1990 por um comitê acadêmico. [1]"),
           "extrativa: conclusão é a frase de abertura mais relevante, citada");
    checar(t && strstr(t, "Pontos principais:") && strstr(t, "[2]"), "extrativa: pontos de mais de uma fonte");
    checar(t && strstr(t, "Linha do tempo:\n  • 1990") && strstr(t, "1998"), "extrativa: pergunta sobre história ganha linha do tempo em ordem");
    checar(t && strstr(t, "frases copiadas dos trechos") && strstr(t, "o modelo não respondeu (limite_excedido)"), "extrativa: diz como foi montada e por quê");
    checar(t && strstr(t, "Fontes:"), "extrativa: fontes listadas");
    g_free(t);
    GPtrArray *vazio = g_ptr_array_new_with_free_func(fonte_web_liberar);
    t = sintese_web_extrativa(vazio, "x", NULL);
    checar(t && strstr(t, "não encontrou fontes") && strstr(t, "Nada foi inventado"), "extrativa: sem fontes -> diz que não encontrou, sem inventar");
    g_free(t);
    t = sintese_web_sem_dados(vazio, "x", "requisição HTTPS falhou: tempo esgotado");
    checar(t && strstr(t, "não consegui obter dados da web para responder — requisição HTTPS falhou: tempo esgotado") && strstr(t, "Nada foi inventado"),
           "sem dados: mostra o erro real (timeout/rede)");
    g_free(t);
    g_ptr_array_unref(vazio);
    g_ptr_array_unref(f);
}

int main(void)
{
    testes_html();
    testes_trecho();
    testes_fontes();
    testes_cotacao();
    testes_sintese();
    testes_extrativa();
    printf("\n%d testes, %d falhas\n", total, falhas);
    return falhas ? 1 : 0;
}
