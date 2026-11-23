#define _GNU_SOURCE
/* Testes de projeto/arquivos.c: confinamento ao projeto, codificações,
 * leitura, listagem, busca, prévia (diff) e escrita com desfazer.
 * Tudo num diretório temporário sob /tmp. Uso: make teste-arquivos */
#include "../../projeto/arquivos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int total, falhas;
#define checar(c, d) checar_((c) ? TRUE : FALSE, (d))
static void checar_(gboolean cond, const char *desc)
{
    total++;
    if (!cond) falhas++;
    printf("%s %s\n", cond ? "ok  " : "FALHA", desc);
}

static char *raiz;

static char *caminho(const char *rel) { return g_build_filename(raiz, rel, NULL); }

static void gravar(const char *rel, const void *d, gssize n)
{
    char *c = caminho(rel);
    char *pai = g_path_get_dirname(c);
    g_mkdir_with_parents(pai, 0755);
    g_file_set_contents(c, d, n, NULL);
    g_free(pai);
    g_free(c);
}

static GBytes *conteudo(const char *rel)
{
    char *c = caminho(rel), *d = NULL;
    gsize n = 0;
    if (!g_file_get_contents(c, &d, &n, NULL)) { g_free(c); return NULL; }
    g_free(c);
    return g_bytes_new_take(d, n);
}

static gboolean bytes_iguais(const char *rel, const void *d, gsize n)
{
    GBytes *b = conteudo(rel);
    gboolean ok = b && g_bytes_get_size(b) == n && !memcmp(g_bytes_get_data(b, NULL), d, n);
    if (b) g_bytes_unref(b);
    return ok;
}

static gboolean existe(const char *rel) { char *c = caminho(rel); gboolean e = g_file_test(c, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_SYMLINK); g_free(c); return e; }

static JsonObject *params(const char *json)
{
    JsonParser *p = json_parser_new();
    json_parser_load_from_data(p, json, -1, NULL);
    JsonObject *o = json_object_ref(json_node_get_object(json_parser_get_root(p)));
    g_object_unref(p);
    return o;
}

/* aplica e devolve o JSON de resultado (ou NULL e *erro) */
static char *aplicar(const char *f, const char *json, char **erro)
{
    JsonObject *o = params(json);
    *erro = NULL;
    char *r = proj_aplicar(f, o, erro);
    json_object_unref(o);
    return r;
}

static char *previa(const char *f, const char *json, char **erro)
{
    JsonObject *o = params(json);
    *erro = NULL;
    char *r = proj_previa(f, o, erro);
    json_object_unref(o);
    return r;
}

static char *campo(const char *json, const char *k)
{
    JsonParser *p = json_parser_new();
    char *v = NULL;
    if (json && json_parser_load_from_data(p, json, -1, NULL)) {
        JsonObject *o = json_node_get_object(json_parser_get_root(p));
        if (json_object_has_member(o, k)) {
            JsonNode *n = json_object_get_member(o, k);
            if (json_node_get_value_type(n) == G_TYPE_STRING) v = g_strdup(json_node_get_string(n));
            else if (json_node_get_value_type(n) == G_TYPE_INT64) v = g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(n));
            else if (json_node_get_value_type(n) == G_TYPE_BOOLEAN) v = g_strdup(json_node_get_boolean(n) ? "true" : "false");
        }
    }
    g_object_unref(p);
    return v;
}

static gboolean campo_igual(const char *json, const char *k, const char *esperado)
{
    char *v = campo(json, k);
    gboolean ok = v && !strcmp(v, esperado);
    if (!ok) printf("      %s = %s (esperado %s)\n", k, v ? v : "(ausente)", esperado);
    g_free(v);
    return ok;
}

/* ------------------------------------------------------------- raiz */
static void testes_raiz(void)
{
    char *e = NULL;
    checar(!proj_definir_raiz("/", &e) && e, "raiz: '/' recusada");
    g_clear_pointer(&e, g_free);
    checar(!proj_definir_raiz("/etc", &e), "raiz: /etc recusada");
    g_clear_pointer(&e, g_free);
    checar(!proj_definir_raiz("/usr/lib", &e), "raiz: /usr/lib recusada");
    g_clear_pointer(&e, g_free);
    checar(!proj_definir_raiz("/tmp", &e), "raiz: /tmp inteiro recusado");
    g_clear_pointer(&e, g_free);
    checar(!proj_definir_raiz("/nao/existe/mesmo", &e), "raiz: diretório inexistente recusado");
    g_clear_pointer(&e, g_free);
    checar(proj_definir_raiz(raiz, &e) && !e, "raiz: diretório de projeto aceito");

    char *r = proj_resolver("../fora.txt", FALSE, FALSE, &e);
    checar(!r && e && strstr(e, "fora do projeto"), "caminho: '..' que sai do projeto é recusado");
    g_clear_pointer(&e, g_free);
    r = proj_resolver("/etc/passwd", FALSE, FALSE, &e);
    checar(!r && e, "caminho: absoluto fora do projeto é recusado");
    g_clear_pointer(&e, g_free);
    r = proj_resolver("src/../src/a.c", FALSE, FALSE, &e);
    char *esperado = caminho("src/a.c");
    checar(r && !strcmp(r, esperado), "caminho: normalizado dentro do projeto");
    g_free(r); g_free(esperado);
    char *link = caminho("para_etc");
    symlink("/etc", link);
    g_free(link);
    r = proj_resolver("para_etc/passwd", FALSE, FALSE, &e);
    checar(!r && e && strstr(e, "link simbólico"), "caminho: link simbólico que aponta para fora é recusado");
    g_clear_pointer(&e, g_free);
    unlink(caminho("para_etc"));
    gravar(".git/config", "[core]\n", -1);
    r = proj_resolver(".git/config", TRUE, FALSE, &e);
    checar(!r && e && strstr(e, "Git"), "caminho: escrita em .git/ recusada");
    g_clear_pointer(&e, g_free);
    r = proj_resolver(".git/config", FALSE, TRUE, &e);
    checar(r != NULL, "caminho: leitura de .git/config permitida");
    g_free(r);
    r = proj_resolver(".", TRUE, FALSE, &e);
    checar(!r, "caminho: a própria raiz não pode ser alterada");
    g_clear_pointer(&e, g_free);
    char *fifo = caminho("fila");
    mkfifo(fifo, 0600);
    r = proj_resolver("fila", FALSE, TRUE, &e);
    checar(!r && e && strstr(e, "especial"), "caminho: FIFO recusado");
    g_clear_pointer(&e, g_free);
    unlink(fifo);
    g_free(fifo);
}

/* ------------------------------------------------------- codificação */
static void detecta(const char *desc, const char *utf8, const char *cod, const char *esperada)
{
    char *e = NULL;
    GBytes *b = proj_codificar(utf8, cod, FALSE, &e);
    gsize n = 0;
    const guchar *d = g_bytes_get_data(b, &n);
    ProjConfianca c;
    char *det = proj_detectar_codificacao(d, n, NULL, &c);
    char *u = proj_decodificar(d, n, NULL, NULL, NULL, NULL, &e);
    /* 'esperada' pode listar equivalentes separadas por '|' (ex.: ISO-8859-1
     * e WINDOWS-1252 decodificam igual um texto sem bytes 0x80–0x9F; UHC é
     * o superconjunto do EUC-KR): o que importa é o texto sair idêntico */
    char **aceitas = g_strsplit(esperada, "|", -1);
    gboolean ok = g_strv_contains((const char *const *)aceitas, det) && u && !strcmp(u, utf8);
    g_strfreev(aceitas);
    if (!ok) printf("      detectado %s, texto %s\n", det, u && !strcmp(u, utf8) ? "igual" : "diferente");
    checar(ok, desc);
    g_free(det); g_free(u); g_free(e);
    g_bytes_unref(b);
}

static void testes_codificacao(void)
{
    const guchar u8bom[] = { 0xEF, 0xBB, 0xBF, 'o', 'l', 0xC3, 0xA1, '\n' };
    gboolean bom = FALSE;
    char *c = proj_detectar_codificacao(u8bom, sizeof u8bom, &bom, NULL);
    checar(!strcmp(c, "UTF-8") && bom, "codificação: UTF-8 com BOM");
    g_free(c);
    const guchar u16[] = { 0xFF, 0xFE, 'o', 0, 'i', 0, '\n', 0 };
    c = proj_detectar_codificacao(u16, sizeof u16, &bom, NULL);
    checar(!strcmp(c, "UTF-16LE") && bom, "codificação: UTF-16LE com BOM");
    g_free(c);
    detecta("codificação: UTF-16BE sem BOM (padrão de zeros)", "texto simples em inglês, sem acento, bem comprido\n", "UTF-16BE", "UTF-16BE");
    detecta("codificação: UTF-32LE sem BOM", "linha um\nlinha dois\nlinha tres\n", "UTF-32LE", "UTF-32LE");
    detecta("codificação: ASCII puro", "int main(void) { return 0; }\n", "US-ASCII", "US-ASCII");
    detecta("codificação: UTF-8 com acentos", "programação em C é ótima\n", "UTF-8", "UTF-8");
    detecta("codificação: português em Windows-1252 (ç, ã, é)",
            "/* Função que calcula a média das notas e devolve a situação do aluno: aprovação ou reprovação. */\n", "WINDOWS-1252", "WINDOWS-1252|ISO-8859-1|ISO-8859-15");
    detecta("codificação: russo em Windows-1251", "Привет, мир! Это тестовая строка на русском языке для проверки кодировки.\n", "WINDOWS-1251", "WINDOWS-1251");
    detecta("codificação: grego em ISO-8859-7", "Καλημέρα κόσμε, αυτό είναι ένα δοκιμαστικό κείμενο στα ελληνικά.\n", "ISO-8859-7", "ISO-8859-7");
    detecta("codificação: japonês em Shift_JIS", "これは日本語のテストです。文字コードを確認します。漢字とひらがなとカタカナ。\n", "SHIFT_JIS", "SHIFT_JIS");
    detecta("codificação: chinês em GB18030", "这是一个中文测试文件，用于检查字符编码是否能够被正确识别和转换。\n", "GB18030", "GB18030");
    detecta("codificação: coreano em EUC-KR", "이것은 한국어 문자 인코딩을 확인하기 위한 테스트 문장입니다.\n", "EUC-KR", "EUC-KR|UHC|CP949");

    const guchar bin[] = { 0x7f, 'E', 'L', 'F', 2, 1, 1, 0, 0, 0, 0, 0 };
    c = proj_detectar_codificacao(bin, sizeof bin, NULL, NULL);
    checar(!strcmp(c, "binario"), "codificação: binário (ELF) reconhecido");
    g_free(c);

    char *e = NULL;
    GBytes *b = proj_codificar("abc", "CODIFICACAO-QUE-NAO-EXISTE", FALSE, &e);
    checar(!b && e && strstr(e, "desconhecida"), "codificação: nome desconhecido pelo iconv é recusado");
    g_clear_pointer(&e, g_free);
    b = proj_codificar("日本", "ISO-8859-1", FALSE, &e);
    checar(!b && e && strstr(e, "não cabe"), "codificação: caractere que não existe no destino é recusado (nunca vira '?')");
    g_clear_pointer(&e, g_free);
    b = proj_codificar("x", "EBCDIC-US", FALSE, &e);
    checar(b && g_bytes_get_size(b) == 1 && ((const guchar *)g_bytes_get_data(b, NULL))[0] == 0xA7, "codificação: EBCDIC (iconv) funciona");
    if (b) g_bytes_unref(b);

    checar(!strcmp(proj_detectar_eol("a\r\nb\r\n"), "CRLF") && !strcmp(proj_detectar_eol("a\nb"), "LF") &&
           !strcmp(proj_detectar_eol("a\rb\r"), "CR") && !strcmp(proj_detectar_eol("a\nb\r\n"), "misto") &&
           !strcmp(proj_detectar_eol("sem quebra"), "nenhuma"), "quebra de linha: LF, CRLF, CR, misto, nenhuma");
}

/* ------------------------------------------------------------ leitura */
static void testes_leitura(void)
{
    gravar("src/main.c", "#include <stdio.h>\n\nint main(void)\n{\n    printf(\"olá\\n\");\n    return 0;\n}\n", -1);
    gravar("src/util.h", "int soma(int a, int b);\n", -1);
    gravar("docs/LEIAME.md", "# Projeto\nFunção soma.\n", -1);
    gravar(".oculto", "x\n", -1);
    const guchar binario[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 0x0d, 'I', 'H', 'D', 'R' };
    gravar("img.png", binario, sizeof binario);
    char *e = NULL;
    char *r = proj_ler("src/main.c", 3, 3, NULL, &e);
    checar(r && campo_igual(r, "conteudo", "int main(void)\n{\n    printf(\"olá\\n\");\n") && campo_igual(r, "total_linhas", "7") &&
           campo_igual(r, "truncado", "true") && campo_igual(r, "codificacao", "UTF-8"), "ler: janela de linhas, total e codificação");
    g_free(r);
    r = proj_ler("img.png", 1, 10, NULL, &e);
    checar(r && campo_igual(r, "binario", "true") && strstr(r, "PNG"), "ler: binário vira hexdump, nunca texto corrompido");
    g_free(r);
    r = proj_ler("nao_existe.c", 1, 10, NULL, &e);
    checar(!r && e && strstr(e, "não existe"), "ler: arquivo inexistente");
    g_clear_pointer(&e, g_free);
    r = proj_ler("src", 1, 10, NULL, &e);
    checar(!r && e && strstr(e, "pasta"), "ler: pasta não é lida como arquivo");
    g_clear_pointer(&e, g_free);

    r = proj_listar(".", TRUE, NULL, 100, FALSE, &e);
    checar(r && strstr(r, "src/main.c") && strstr(r, "docs/LEIAME.md") && !strstr(r, ".git") && !strstr(r, ".oculto"),
           "listar: recursivo, sem .git e sem ocultos");
    g_free(r);
    r = proj_listar(".", TRUE, "*.c", 100, FALSE, &e);
    checar(r && strstr(r, "src/main.c") && !strstr(r, "util.h"), "listar: filtro por padrão (*.c)");
    g_free(r);
    r = proj_listar(".", TRUE, NULL, 2, FALSE, &e);
    checar(r && campo_igual(r, "truncado", "true") && campo_igual(r, "total", "2"), "listar: limite respeitado e informado");
    g_free(r);

    r = proj_buscar("SOMA", FALSE, FALSE, ".", NULL, 50, &e);
    checar(r && strstr(r, "\"caminho\":\"src/util.h\",\"linha\":1") && strstr(r, "docs/LEIAME.md") && !strstr(r, "img.png"),
           "buscar: sem diferenciar maiúsculas, com arquivo e linha; binários ignorados");
    g_free(r);
    r = proj_buscar("printf\\(\"ol", TRUE, TRUE, "src", "*.c", 50, &e);
    checar(r && strstr(r, "\"linha\":5"), "buscar: expressão regular em uma pasta e padrão");
    g_free(r);
    r = proj_buscar("([", TRUE, TRUE, ".", NULL, 50, &e);
    checar(!r && e && strstr(e, "inválida"), "buscar: regex inválida é erro claro");
    g_clear_pointer(&e, g_free);

    r = proj_info("src/main.c", &e);
    checar(r && campo_igual(r, "quebra_de_linha", "LF") && campo_igual(r, "total_linhas", "7") && campo_igual(r, "binario", "false"),
           "info: tipo, quebra de linha, linhas");
    g_free(r);
}

/* ------------------------------------------------------------ escrita */
static void testes_escrita(void)
{
    char *e = NULL;
    /* prévia não toca no disco */
    char *pv = previa("arquivo.criar", "{\"caminho\":\"src/novo.c\",\"conteudo\":\"int x;\\n\"}", &e);
    checar(pv && strstr(pv, "+++ b/src/novo.c") && strstr(pv, "+int x;") && !existe("src/novo.c"), "prévia: diff mostrado, nada gravado");
    g_free(pv);

    char *r = aplicar("arquivo.criar", "{\"caminho\":\"src/novo.c\",\"conteudo\":\"int x;\\n\"}", &e);
    checar(r && bytes_iguais("src/novo.c", "int x;\n", 7), "criar: arquivo novo gravado");
    char *op_criar = campo(r, "operacao_id");
    g_free(r);
    r = aplicar("arquivo.criar", "{\"caminho\":\"src/novo.c\",\"conteudo\":\"x\"}", &e);
    checar(!r && e && strstr(e, "já existe"), "criar: nunca sobrescreve");
    g_clear_pointer(&e, g_free);
    r = aplicar("arquivo.criar", "{\"caminho\":\"pasta/que/falta/a.c\",\"conteudo\":\"x\"}", &e);
    checar(!r && e && strstr(e, "não existe"), "criar: pasta inexistente é erro (não cria pastas escondido)");
    g_clear_pointer(&e, g_free);
    r = aplicar("arquivo.criar", "{\"caminho\":\"../fora.c\",\"conteudo\":\"x\"}", &e);
    checar(!r && e && strstr(e, "fora do projeto"), "criar: fora do projeto recusado");
    g_clear_pointer(&e, g_free);
    r = aplicar("arquivo.criar", "{\"caminho\":\".git/hooks/pre-commit\",\"conteudo\":\"x\"}", &e);
    checar(!r && e && strstr(e, "Git"), "criar: dentro de .git/ recusado");
    g_clear_pointer(&e, g_free);

    /* editar: trecho único, ausente, ambíguo */
    r = aplicar("arquivo.editar", "{\"caminho\":\"src/main.c\",\"procurar\":\"return 0;\",\"substituir\":\"return 1;\"}", &e);
    GBytes *b = conteudo("src/main.c");
    checar(r && b && strstr(g_bytes_get_data(b, NULL), "return 1;") && strstr(r, "-    return 0;") && strstr(r, "+    return 1;"),
           "editar: substitui o trecho e devolve o diff");
    char *op_editar = campo(r, "operacao_id");
    g_bytes_unref(b);
    g_free(r);
    r = aplicar("arquivo.editar", "{\"caminho\":\"src/main.c\",\"procurar\":\"nao existe aqui\",\"substituir\":\"x\"}", &e);
    checar(!r && e && strstr(e, "não foi encontrado"), "editar: trecho ausente é erro claro");
    g_clear_pointer(&e, g_free);
    gravar("dup.txt", "a = 1;\nb = 2;\na = 1;\n", -1);
    r = aplicar("arquivo.editar", "{\"caminho\":\"dup.txt\",\"procurar\":\"a = 1;\",\"substituir\":\"a = 3;\"}", &e);
    checar(!r && e && strstr(e, "2 vezes") && strstr(e, "linhas 1, 3"), "editar: trecho ambíguo diz quantas vezes e em que linhas");
    g_clear_pointer(&e, g_free);
    r = aplicar("arquivo.editar", "{\"caminho\":\"dup.txt\",\"procurar\":\"a = 1;\",\"substituir\":\"a = 3;\",\"todas\":true}", &e);
    checar(r && bytes_iguais("dup.txt", "a = 3;\nb = 2;\na = 3;\n", 21), "editar: todas=true substitui todas");
    g_free(r);
    r = aplicar("arquivo.editar", "{\"caminho\":\"dup.txt\",\"procurar\":\"b = 2;\",\"substituir\":\"b = 2;\"}", &e);
    checar(!r && e && strstr(e, "idêntico"), "editar: alteração sem efeito é recusada (nunca 'sucesso' sem diff)");
    g_clear_pointer(&e, g_free);

    /* CRLF e UTF-16 preservados */
    gravar("win.txt", "linha 1\r\nlinha 2\r\n", -1);
    r = aplicar("arquivo.editar", "{\"caminho\":\"win.txt\",\"procurar\":\"linha 1\\nlinha 2\",\"substituir\":\"linha 1\\nnova\\nlinha 2\"}", &e);
    checar(r && bytes_iguais("win.txt", "linha 1\r\nnova\r\nlinha 2\r\n", 24), "editar: arquivo CRLF continua CRLF (procura com \\n funciona)");
    g_free(r);
    GBytes *u16 = proj_codificar("olá mundo\n", "UTF-16LE", TRUE, &e);
    gravar("u16.txt", g_bytes_get_data(u16, NULL), (gssize)g_bytes_get_size(u16));
    g_bytes_unref(u16);
    r = aplicar("arquivo.editar", "{\"caminho\":\"u16.txt\",\"procurar\":\"mundo\",\"substituir\":\"Lia\"}", &e);
    GBytes *esperado = proj_codificar("olá Lia\n", "UTF-16LE", TRUE, &e);
    checar(r && bytes_iguais("u16.txt", g_bytes_get_data(esperado, NULL), g_bytes_get_size(esperado)), "editar: UTF-16LE com BOM continua UTF-16LE com BOM");
    g_bytes_unref(esperado);
    g_free(r);
    GBytes *l1 = proj_codificar("Ação\n", "WINDOWS-1252", FALSE, &e);
    gravar("latin.txt", g_bytes_get_data(l1, NULL), (gssize)g_bytes_get_size(l1));
    g_bytes_unref(l1);
    r = aplicar("arquivo.converter", "{\"caminho\":\"latin.txt\",\"codificacao\":\"UTF-8\"}", &e);
    checar(r && bytes_iguais("latin.txt", "Ação\n", 7), "converter: Windows-1252 → UTF-8");
    g_free(r);
    r = aplicar("arquivo.converter", "{\"caminho\":\"latin.txt\",\"quebra_de_linha\":\"CRLF\"}", &e);
    checar(r && bytes_iguais("latin.txt", "Ação\r\n", 8), "converter: LF → CRLF");
    g_free(r);

    /* sha256_esperado */
    r = aplicar("arquivo.anexar", "{\"caminho\":\"dup.txt\",\"conteudo\":\"c = 4;\\n\",\"sha256_esperado\":\"00\"}", &e);
    checar(!r && e && strstr(e, "mudou desde que foi lido"), "anexar: sha256_esperado diferente impede a escrita");
    g_clear_pointer(&e, g_free);
    r = aplicar("arquivo.anexar", "{\"caminho\":\"dup.txt\",\"conteudo\":\"c = 4;\\n\"}", &e);
    checar(r && bytes_iguais("dup.txt", "a = 3;\nb = 2;\na = 3;\nc = 4;\n", 28), "anexar: acrescenta ao fim");
    g_free(r);

    /* apagar + desfazer */
    r = aplicar("arquivo.apagar", "{\"caminho\":\"src/util.h\"}", &e);
    checar(r && !existe("src/util.h") && strstr(r, "copia_original"), "apagar: arquivo removido, cópia guardada");
    char *op_apagar = campo(r, "operacao_id");
    g_free(r);
    char *pd = g_strdup_printf("{\"operacao_id\":\"%s\"}", op_apagar);
    pv = previa("arquivo.desfazer", pd, &e);
    checar(pv && strstr(pv, "restaurar: src/util.h") && !existe("src/util.h"), "desfazer: prévia do que será restaurado");
    g_free(pv);
    r = aplicar("arquivo.desfazer", pd, &e);
    checar(r && bytes_iguais("src/util.h", "int soma(int a, int b);\n", 24), "desfazer: arquivo apagado volta igual");
    g_free(r);
    r = aplicar("arquivo.desfazer", pd, &e);
    checar(!r && e && strstr(e, "já foi desfeita"), "desfazer: a mesma operação não é desfeita duas vezes");
    g_clear_pointer(&e, g_free);
    g_free(pd);
    pd = g_strdup_printf("{\"operacao_id\":\"%s\"}", op_criar);
    r = aplicar("arquivo.desfazer", pd, &e);
    checar(r && !existe("src/novo.c"), "desfazer: arquivo criado é removido");
    g_free(r); g_free(pd);
    pd = g_strdup_printf("{\"operacao_id\":\"%s\"}", op_editar);
    r = aplicar("arquivo.desfazer", pd, &e);
    b = conteudo("src/main.c");
    checar(r && b && strstr(g_bytes_get_data(b, NULL), "return 0;"), "desfazer: edição revertida");
    g_bytes_unref(b);
    g_free(r); g_free(pd);
    r = aplicar("arquivo.desfazer", "{\"operacao_id\":\"../../etc\"}", &e);
    checar(!r && e, "desfazer: id com '..' recusado");
    g_clear_pointer(&e, g_free);

    /* pastas, mover, copiar, permissões */
    r = aplicar("arquivo.criar_pasta", "{\"caminho\":\"a/b/c\"}", &e);
    checar(r && g_file_test(caminho("a/b/c"), G_FILE_TEST_IS_DIR), "criar_pasta: cria os níveis que faltam");
    g_free(r);
    r = aplicar("arquivo.mover", "{\"origem\":\"docs/LEIAME.md\",\"destino\":\"a/b/c\"}", &e);
    checar(r && existe("a/b/c/LEIAME.md") && !existe("docs/LEIAME.md"), "mover: para dentro de uma pasta");
    char *op_mover = campo(r, "operacao_id");
    g_free(r);
    r = aplicar("arquivo.copiar", "{\"origem\":\"a/b/c/LEIAME.md\",\"destino\":\"src/main.c\"}", &e);
    checar(!r && e && strstr(e, "já existe"), "copiar: nunca sobrescreve o destino");
    g_clear_pointer(&e, g_free);
    r = aplicar("arquivo.copiar", "{\"origem\":\"a/b/c/LEIAME.md\",\"destino\":\"LEIAME-copia.md\"}", &e);
    checar(r && bytes_iguais("LEIAME-copia.md", "# Projeto\nFunção soma.\n", 25), "copiar: cópia idêntica");
    g_free(r);
    pd = g_strdup_printf("{\"operacao_id\":\"%s\"}", op_mover);
    r = aplicar("arquivo.desfazer", pd, &e);
    checar(r && existe("docs/LEIAME.md") && !existe("a/b/c/LEIAME.md"), "desfazer: mover volta ao lugar");
    g_free(r); g_free(pd);
    r = aplicar("arquivo.apagar", "{\"caminho\":\"a\"}", &e);
    checar(!r && e && strstr(e, "não está vazia"), "apagar: pasta com conteúdo nunca é apagada em massa");
    g_clear_pointer(&e, g_free);
    r = aplicar("arquivo.apagar", "{\"caminho\":\"a/b/c\"}", &e);
    checar(r && !existe("a/b/c"), "apagar: pasta vazia");
    g_free(r);
    gravar("run.sh", "#!/bin/sh\necho oi\n", -1);
    r = aplicar("arquivo.permissoes", "{\"caminho\":\"run.sh\",\"executavel\":true}", &e);
    char *rs = caminho("run.sh");
    checar(r && g_file_test(rs, G_FILE_TEST_IS_EXECUTABLE), "permissoes: torna executável");
    g_free(r);
    r = aplicar("arquivo.escrever", "{\"caminho\":\"run.sh\",\"conteudo\":\"#!/bin/sh\\necho tchau\\n\"}", &e);
    checar(r && g_file_test(rs, G_FILE_TEST_IS_EXECUTABLE) && bytes_iguais("run.sh", "#!/bin/sh\necho tchau\n", 21),
           "escrever: substitui o conteúdo e preserva as permissões");
    g_free(r); g_free(rs);
    char *alvo = caminho("src/main.c"), *lk = caminho("link.c");
    symlink(alvo, lk);
    r = aplicar("arquivo.editar", "{\"caminho\":\"link.c\",\"procurar\":\"main\",\"substituir\":\"x\"}", &e);
    checar(!r && e && strstr(e, "link simbólico"), "editar: através de link simbólico é recusado");
    g_clear_pointer(&e, g_free);
    g_free(alvo); g_free(lk);
    g_free(op_criar); g_free(op_editar); g_free(op_apagar); g_free(op_mover);
}

static void testes_diff(void)
{
    char *d = proj_diff("x.c", "a\nb\nc\nd\ne\nf\ng\nh\ni\nj\n", "a\nb\nc\nd\nE\nf\ng\nh\ni\nj\n", 3, 0);
    checar(d && strstr(d, "@@ -2,7 +2,7 @@\n b\n c\n d\n-e\n+E\n f\n g\n h\n"), "diff: bloco com 3 linhas de contexto e cabeçalho correto");
    g_free(d);
    d = proj_diff("n.c", "", "um\ndois\n", 3, 0);
    checar(d && strstr(d, "@@ -0,0 +1,2 @@\n+um\n+dois\n"), "diff: arquivo novo");
    g_free(d);
    GString *grande = g_string_new(NULL);
    for (int i = 0; i < 20000; i++) g_string_append_printf(grande, "linha %d\n", i);
    d = proj_diff("g.txt", "", grande->str, 3, 4096);
    checar(d && strlen(d) < 4300 && strstr(d, "prévia cortada"), "diff: prévia grande é cortada e avisa");
    g_free(d);
    g_string_free(grande, TRUE);
}

int main(void)
{
    raiz = g_dir_make_tmp("lia-arquivos-XXXXXX", NULL);
    if (!raiz || !g_str_has_prefix(raiz, "/tmp/")) { fprintf(stderr, "sem diretório temporário\n"); return 2; }
    char *desf = g_build_filename(raiz, "..", "lia-desfazer-teste", NULL);
    char *desf_c = g_canonicalize_filename(desf, NULL);
    char *desf_dir = g_strdup_printf("%s-%d", desf_c, (int)getpid());
    proj_definir_dir_desfazer(desf_dir);
    testes_raiz();
    testes_codificacao();
    testes_leitura();
    testes_escrita();
    testes_diff();
    printf("\n%d testes, %d falhas\n", total, falhas);
    char *cmd = g_strdup_printf("rm -rf -- '%s' '%s'", raiz, desf_dir);
    if (g_str_has_prefix(raiz, "/tmp/lia-arquivos-") && g_str_has_prefix(desf_dir, "/tmp/lia-desfazer-teste-")) {
        int rc = system(cmd);
        (void)rc;
    }
    g_free(cmd); g_free(desf); g_free(desf_c); g_free(desf_dir);
    return falhas ? 1 : 0;
}
