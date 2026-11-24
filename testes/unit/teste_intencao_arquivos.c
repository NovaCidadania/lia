/* Pedidos em português -> plano de arquivo (sessiond/intencao_arquivos.c).
 * Sem daemon, sem modelo. Uso: make teste-intencao-arquivos */
#include "../../sessiond/intencao_arquivos.h"
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

/* plano esperado: ferramenta e pares chave=valor (string) nos parâmetros */
static void espera(const char *pedido, const char *ferramenta, const char *const *pares)
{
    char *td = NULL;
    AgentePlano *p = agente_planejar_arquivos(pedido, &td);
    gboolean ok = p && p->passos->len == 1;
    char *detalhe = NULL;
    if (ok) {
        AgentePasso *s = g_ptr_array_index(p->passos, 0);
        ok = !strcmp(s->ferramenta, ferramenta);
        JsonParser *jp = json_parser_new();
        json_parser_load_from_data(jp, s->parametros_json, -1, NULL);
        JsonObject *o = json_node_get_object(json_parser_get_root(jp));
        for (int i = 0; ok && pares && pares[i]; i += 2) {
            if (!json_object_has_member(o, pares[i])) { ok = FALSE; break; }
            JsonNode *n = json_object_get_member(o, pares[i]);
            char *v = json_node_get_value_type(n) == G_TYPE_STRING ? g_strdup(json_node_get_string(n))
                    : json_node_get_value_type(n) == G_TYPE_BOOLEAN ? g_strdup(json_node_get_boolean(n) ? "true" : "false")
                    : g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(n));
            if (strcmp(v, pares[i + 1])) ok = FALSE;
            g_free(v);
        }
        detalhe = g_strdup_printf("%s %s", s->ferramenta, s->parametros_json);
        g_object_unref(jp);
    }
    char *desc = g_strdup_printf("\"%s\" -> %s", pedido, ferramenta);
    checar(ok, desc);
    if (!ok) printf("      obtido: %s%s%s\n", detalhe ? detalhe : "(nenhum plano)", td ? " / texto: " : "", td ? td : "");
    g_free(desc); g_free(detalhe); g_free(td);
    if (p) agente_plano_liberar(p);
}

static void nada(const char *pedido)
{
    char *td = NULL;
    AgentePlano *p = agente_planejar_arquivos(pedido, &td);
    char *desc = g_strdup_printf("\"%s\" -> não é operação de arquivo", pedido);
    checar(!p && !td, desc);
    if (p) printf("      obtido: %s %s\n", ((AgentePasso *)g_ptr_array_index(p->passos, 0))->ferramenta,
                  ((AgentePasso *)g_ptr_array_index(p->passos, 0))->parametros_json);
    if (td) printf("      texto: %s\n", td);
    g_free(desc); g_free(td);
    if (p) agente_plano_liberar(p);
}

static void pergunta(const char *pedido, const char *trecho)
{
    char *td = NULL;
    AgentePlano *p = agente_planejar_arquivos(pedido, &td);
    char *desc = g_strdup_printf("\"%s\" -> pede esclarecimento", pedido);
    checar(!p && td && strstr(td, trecho), desc);
    if (!(!p && td && strstr(td, trecho))) printf("      texto: %s\n", td ? td : "(nenhum)");
    g_free(desc); g_free(td);
    if (p) agente_plano_liberar(p);
}

#define P(...) ((const char *const[]){ __VA_ARGS__, NULL })

int main(void)
{
    /* extração */
    char **c = intencao_trechos_citados("troque `int x` por \"long x\" e 'a' por “b”, mas d'água não");
    checar(g_strv_length(c) == 4 && !strcmp(c[0], "int x") && !strcmp(c[1], "long x") && !strcmp(c[2], "a") && !strcmp(c[3], "b"),
           "citados: crases, aspas duplas, simples e tipográficas; apóstrofo no meio da palavra não conta");
    g_strfreev(c);
    c = intencao_caminhos("leia src/main.c, depois o Makefile e o .gitignore (não o v1.2 nem 3.14).");
    checar(g_strv_length(c) == 3 && !strcmp(c[0], "src/main.c") && !strcmp(c[1], "Makefile") && !strcmp(c[2], ".gitignore"),
           "caminhos: com pasta, sem extensão conhecido, oculto; versão e número não são caminho; pontuação removida");
    g_strfreev(c);

    /* leitura */
    espera("leia o arquivo src/main.c", "arquivo.ler", P("caminho", "src/main.c"));
    espera("Mostre o conteúdo de README.md", "arquivo.ler", P("caminho", "README.md"));
    espera("abra sessiond/agente.c nas linhas 100 a 150", "arquivo.ler", P("caminho", "sessiond/agente.c", "linha_inicial", "100", "linhas", "51"));
    espera("o que tem no Makefile?", "arquivo.ler", P("caminho", "Makefile"));
    espera("leia notas.txt como latin1", "arquivo.ler", P("caminho", "notas.txt", "codificacao", "ISO-8859-1"));
    espera("liste os arquivos do projeto", "arquivo.listar", P(NULL));
    espera("quais arquivos .c existem em sessiond?", "arquivo.listar", P("caminho", "sessiond", "padrao", "*.c", "recursivo", "true"));
    espera("mostre a estrutura do projeto", "arquivo.listar", P("recursivo", "true"));
    espera("procure \"TODO\" no projeto", "arquivo.buscar", P("texto", "TODO"));
    espera("onde aparece `lia_web_abrir` no código?", "arquivo.buscar", P("texto", "lia_web_abrir"));
    espera("busque 'malloc' em sessiond", "arquivo.buscar", P("texto", "malloc", "caminho", "sessiond"));
    espera("qual a codificação do arquivo docs/protocolo.md?", "arquivo.info", P("caminho", "docs/protocolo.md"));

    /* escrita */
    espera("crie o arquivo notas.txt com o conteúdo: lembrar de testar", "arquivo.criar", P("caminho", "notas.txt", "conteudo", "lembrar de testar"));
    espera("crie src/ola.c com\n```c\n#include <stdio.h>\nint main(void) { puts(\"olá\"); return 0; }\n```", "arquivo.criar",
           P("caminho", "src/ola.c", "conteudo", "#include <stdio.h>\nint main(void) { puts(\"olá\"); return 0; }\n"));
    espera("crie um arquivo vazio chamado .env.exemplo", "arquivo.criar", P("caminho", ".env.exemplo", "conteudo", ""));
    espera("crie o script run.sh executável contendo \"#!/bin/sh\"", "arquivo.criar", P("caminho", "run.sh", "conteudo", "#!/bin/sh", "executavel", "true"));
    espera("crie a pasta testes/fixtures", "arquivo.criar_pasta", P("caminho", "testes/fixtures"));
    espera("crie um diretório chamado build-extra", "arquivo.criar_pasta", P("caminho", "build-extra"));
    espera("substitua \"return 0;\" por \"return 1;\" em src/main.c", "arquivo.editar",
           P("caminho", "src/main.c", "procurar", "return 0;", "substituir", "return 1;"));
    espera("troque todas as ocorrências de `foo` por `bar` no arquivo util.c", "arquivo.editar",
           P("caminho", "util.c", "procurar", "foo", "substituir", "bar", "todas", "true"));
    espera("acrescente \"- item novo\" ao final de TODO.md", "arquivo.anexar", P("caminho", "TODO.md", "conteudo", "- item novo"));
    espera("reescreva config.ini com o conteúdo: [geral]", "arquivo.escrever", P("caminho", "config.ini", "conteudo", "[geral]"));
    espera("apague o arquivo build/lixo.o", "arquivo.apagar", P("caminho", "build/lixo.o"));
    espera("remova old.txt", "arquivo.apagar", P("caminho", "old.txt"));
    espera("renomeie src/a.c para src/b.c", "arquivo.mover", P("origem", "src/a.c", "destino", "src/b.c"));
    espera("copie README.md para docs/LEIAME.md", "arquivo.copiar", P("origem", "README.md", "destino", "docs/LEIAME.md"));
    espera("converta legado.txt para UTF-8", "arquivo.converter", P("caminho", "legado.txt", "codificacao", "UTF-8"));
    espera("converta as quebras de linha de win.txt para LF", "arquivo.converter", P("caminho", "win.txt", "quebra_de_linha", "LF"));
    espera("torne deploy.sh executável", "arquivo.permissoes", P("caminho", "deploy.sh", "executavel", "true"));
    espera("desfaça a operação 20261123-204201-a1b2c3", "arquivo.desfazer", P("operacao_id", "20261123-204201-a1b2c3"));

    /* validação e Git */
    espera("rode os testes", "validar.executar", P(NULL));
    espera("compile o projeto", "validar.executar", P("comando", "build"));
    espera("rode a validação \"lint\"", "validar.executar", P("comando", "lint"));
    espera("quais os comandos de validação do projeto?", "validar.listar", P(NULL));
    espera("git status", "git.status", P(NULL));
    espera("quais arquivos alterados no repositório?", "git.status", P(NULL));
    espera("mostre o diff de src/main.c", "git.diff", P("caminho", "src/main.c"));
    espera("mostre os 5 últimos commits", "git.log", P("limite", "5"));
    espera("qual foi o último commit?", "git.log", P("limite", "1"));
    espera("faça commit de src/main.c com a mensagem \"Corrige o cálculo\"", "git.commit", P("mensagem", "Corrige o cálculo"));
    pergunta("faça um commit", "mensagem do commit");

    /* esclarecimentos */
    pergunta("apague o arquivo", "Qual arquivo");
    pergunta("renomeie o arquivo main.c", "origem e do destino");
    pergunta("substitua 'a' por 'b'", "Em qual arquivo");
    pergunta("desfaça a última operação", "Qual operação");
    pergunta("procure no projeto", "O que devo procurar");

    /* não são operações de arquivo */
    nada("Me conte com detalhes a evolução da linguagem Haskell");
    nada("Qual o preço do dólar hoje?");
    nada("quais processos usam mais memória?");
    nada("remova o vírus do computador");
    nada("procure na internet o site oficial do python.org");
    nada("por que o sistema está lento?");
    nada("crie um programa em C que soma dois números"); /* sem caminho: geração de código, não deste planejador */
    nada("oi, tudo bem?");
    nada("quanto é 2+2?");
    nada("o que mudou na versão 3 do python?");
    nada("compile uma lista de livros sobre história");

    printf("\n%d testes, %d falhas\n", total, falhas);
    return falhas ? 1 : 0;
}
