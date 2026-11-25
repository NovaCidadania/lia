/* Ciclo de coding — partes puras (sem daemon), testáveis sem modelo.
 *
 * Etapas: critérios de aceite → inspeção (leitura dos arquivos) →
 * proposta de edição pelo modelo → aprovação (arquivo.criar/escrever com
 * prévia) → validação (validar.executar com aprovação) → revisão → fim,
 * ou novo ciclo de correção (limitado). Tudo que vem do modelo, dos
 * arquivos, da saída dos testes e da configuração do projeto é DADO não
 * confiável: é validado aqui e de novo pelo daemon antes de qualquer efeito.
 *
 * Edições: o modelo só pode CRIAR arquivo que não existe ou EDITAR
 * (procurar → substituir, trecho único e não abreviado; ou substituir_funcao:
 * a função inteira, chaves balanceadas) arquivo que a Lia LEU nesta tarefa;
 * as edições de um arquivo são aplicadas em memória e viram UM
 * arquivo.escrever com sha256_esperado = hash observado na leitura — se o
 * arquivo mudar depois, a gravação é recusada. */
#ifndef LIA_SESSIOND_CODING_H
#define LIA_SESSIOND_CODING_H

#include <glib.h>
#include <json-glib/json-glib.h>

#define CODING_MAX_CICLOS 3
#define CODING_MAX_ARQUIVOS_LIDOS 8
#define CODING_MAX_BYTES_ARQUIVO (128 * 1024)
#define CODING_MAX_BYTES_PROMPT_ARQUIVOS (12 * 1024)
#define CODING_LINHAS_ARQUIVO_INTEIRO 150

/* "crie src/x.c com um programa que...", "corrija o teste que falha"... */
gboolean coding_e_pedido(const char *pedido);

typedef struct {
    char *descricao;
    char *verificacao; /* "validacao" | "contem" */
    char *caminho;     /* contem */
    char *texto;       /* contem */
} CodingCriterio;

typedef struct {
    GPtrArray *criterios;  /* CodingCriterio* */
    char **descartados;    /* critérios não verificáveis, descartados (para o relatório) */
    char **arquivos_para_ler;
    char *validacao;       /* nome de comando ou NULL */
} CodingPlano;

void coding_plano_liberar(CodingPlano *p);
/* JSON do modelo (tolerante: cercas ```, texto em volta). NULL + erro se não der */
CodingPlano *coding_ler_plano(const char *texto, char **erro);
/* critérios de aceite vêm do PEDIDO: "contem" sobre arquivo que o pedido
 * não cita é descartado (vai para 'descartados', com o motivo); arquivos
 * que DEFINEM os símbolos citados contam como citados ('tambem_citados') */
void coding_filtrar_criterios(CodingPlano *p, const char *pedido, char **tambem_citados);
/* plano mínimo sem modelo: um critério (a validação passa) e os caminhos do pedido */
CodingPlano *coding_plano_padrao(const char *pedido);

typedef struct {
    char *caminho;
    char *conteudo;   /* conteúdo final completo */
    gboolean existia;
    char *sha_lido;   /* hash observado na leitura (existia) */
    char *antes;      /* conteúdo lido (existia) */
} CodingArquivo;

void coding_arquivo_liberar(gpointer p);
/* Aplica em memória as edições propostas. 'lidos': caminho -> conteúdo;
 * 'shas': caminho -> sha256. Devolve os arquivos finais (só os que mudam)
 * e *hash: identidade da proposta (para detectar repetição). */
GPtrArray *coding_aplicar_edicoes(const char *texto_modelo, GHashTable *lidos, GHashTable *shas, char **hash, char **explicacao, char **erro);
/* idem, e 'reescreviveis': arquivos que a PRÓPRIA tarefa criou — "criar"
 * de novo sobre eles vira regravação inteira (presa ao hash lido) */
GPtrArray *coding_aplicar_edicoes_tarefa(const char *texto_modelo, GHashTable *lidos, GHashTable *shas, char **reescreviveis,
                                         char **hash, char **explicacao, char **erro);

/* Contrato por operação, conferido no runtime para TODA proposta (com ou
 * sem esquema no backend): criar = caminho + conteudo; editar = caminho +
 * procurar + substituir; substituir_funcao = caminho + funcao (só o nome) +
 * conteudo (a função inteira). Campo de outra operação ou desconhecido,
 * valor vazio, acima do limite, UTF-8 inválido ou código abreviado ("...")
 * recusam a edição. NULL = ok; senão o motivo (para o modelo e o relatório). */
char *coding_contrato_edicao(JsonObject *edicao, guint indice);
/* linha que é só reticência, comentário com reticência ("// ..."),
 * "{ ... }" ou "... resto do código" — não "...args" */
gboolean coding_conteudo_abreviado(const char *conteudo);
/* esquema JSON da proposta, por operação (anyOf com os campos obrigatórios
 * de cada uma), com no máximo 'max_edicoes' edições (1 depois de truncamento) */
char *coding_esquema_edicoes(guint max_edicoes);

/* arquivo de código novo trivial (só o nome do método, PHP sem <?php,
 * teste sem caso de teste...): devolve o motivo, ou NULL se parece código */
char *coding_conteudo_trivial(const char *caminho, const char *conteudo);

/* Identificadores citados no pedido (Classe::metodo, nomes_com_sublinhado,
 * camelCase, `entre crases`) — o que vale procurar no código */
char **coding_simbolos(const char *pedido);
/* Percorre o projeto (sem .git, vendor/, node_modules/, ambientes virtuais,
 * saídas de build, arquivos ocultos e sensíveis) e devolve: a lista de
 * arquivos para o prompt (os que combinam com o pedido primeiro, até
 * 'max_lista') em *lista e, em *definem, os arquivos que DEFINEM os
 * símbolos citados (class/function/def/fn... <nome>). */
void coding_explorar_projeto(const char *raiz, const char *pedido, guint max_lista, char **lista, char ***definem);

/* Regra fixa (indício, não prova): strings com SQL (select/insert/update/
 * delete/where...) que recebem variável por interpolação ("... = $id") ou
 * concatenação ('...' . $id, "..." + id, f"...{id}"). Uma linha por achado:
 * "arquivo:linha — $var entra direto na SQL: <trecho>". "" se nada. */
char *coding_achados_sql(const char *arquivo, const char *conteudo, gint64 linha_inicial);

/* Receita determinística (prática seguranca.injecao / bd.consultas-
 * parametrizadas), sem modelo: na função PHP 'metodo' do arquivo, a SQL
 * montada com "$var" vira placeholder '?' e a chamada ->query($sql) da
 * mesma função passa a receber array($var, ...). Com 'criar_teste', também
 * propõe um teste PHPUnit isolado (db falso, sem banco) em tests/. Devolve
 * o MESMO formato de proposta do modelo ({"edicoes":[...],"explicacao"}),
 * que passa pelo mesmo verificador, prévia e aprovação; NULL + *motivo
 * quando a receita não se aplica com segurança. */
char *coding_receita_sql_bind(const char *arquivo, const char *conteudo, const char *metodo, gboolean criar_teste, char **motivo);

/* o pedido proíbe alterar código de produção ("não altere o model", "só o teste") */
gboolean coding_pedido_so_testes(const char *pedido);
/* caminho de arquivo de teste (tests/, *Test.php, test_*.py, *.spec.ts...) */
gboolean coding_caminho_de_teste(const char *caminho);
/* a tarefa em termos concretos para o prompt de edição (arquivo novo a
 * criar, proibições, o que um teste precisa ter) — orientação, não código */
char *coding_dicas_tarefa(const char *pedido, GHashTable *lidos);

/* pedido amplo de melhoria ("melhore os controllers e models") sem
 * símbolo nem caminho: a Lia pede o escopo, nunca edita nem pesquisa na web */
gboolean coding_pedido_generico(const char *pedido);

/* Manifests Docker (compose*.y*ml, docker-compose*, Dockerfile*): fatos
 * extraídos linha a linha, sem modelo — serviços, origem (build/imagem),
 * portas, volumes, dependências, redes, deploy; "não declarado no
 * manifest" quando a chave não existe. Riscos são inferências a partir de
 * uma linha citada. Comentários nunca viram fato. Conteúdo já chega com os
 * valores de ambiente ocultados; só NOMES de variáveis aparecem.
 * Acrescenta a 'fatos', 'riscos' ("arquivo:linha — texto\n") e aos nomes
 * de serviços em 'servicos' (separados por vírgula). */
gboolean coding_e_manifest_docker(const char *caminho);
void coding_fatos_docker(const char *arquivo, const char *conteudo, gint64 linha_inicial, GString *fatos, GString *riscos, GString *servicos);

/* resumo determinístico (serviços, dependências, portas, volumes, nº de
 * riscos) montado só a partir dos fatos acima */
char *coding_resumo_docker(const char *fatos, const char *riscos);
/* frases do texto do modelo que contradizem os fatos (dependência ou porta
 * que não existe no manifest); "" se nenhuma */
char *coding_contradicoes_docker(const char *fatos, const char *texto);

/* ---- contexto e critérios: o RUNTIME decide, o modelo só sugere ----
 *
 * Caminhos citados em instruções negativas ("não leia .env", "sem tocar
 * em migrations/", "não execute campaign.py") e categorias citadas nelas
 * (logs, contatos, banco, dumps, migrations, .git, credenciais) viram
 * padrões proibidos: nunca lidos, nunca alterados, nunca critério. */
char **coding_caminhos_proibidos(const char *pedido);
gboolean coding_caminho_proibido(char **proibidos, const char *rel);
/* a proposta ACRESCENTA ao conteúdo uma referência a caminho proibido
 * literal (abrir "sends.txt", importar campaign.py): devolve o nome, ou NULL.
 * A validação rodaria esse código sobre o arquivo real. Padrões com '*' e
 * diretórios não entram (falso positivo demais); referências que o arquivo
 * já tinha não contam. */
char *coding_conteudo_cita_proibido(char **proibidos, const char *antes, const char *depois);
/* código-fonte, teste ou configuração de build/teste (não dados) */
gboolean coding_arquivo_de_contexto(const char *rel);
/* contexto mínimo por regras, sem modelo: quem define os símbolos, os
 * arquivos de código citados (que existem) e os testes existentes deles;
 * *a_criar: arquivos de código citados que não existem; *notas: o que foi
 * ignorado e por quê (uma linha por item) */
void coding_selecionar_contexto(const char *raiz, const char *pedido, char **definem, char ***ler, char ***a_criar, char **notas);
/* família de linguagem pela extensão ("php", "js/ts", "c#"...), ou NULL */
const char *coding_linguagem(const char *rel);
/* linguagens indicadas pelo pedido (extensões citadas, "PHPUnit", "pytest",
 * "Unity"...); vazio = não indica — quem define um símbolo citado em
 * outra linguagem fica fora do contexto */
char **coding_linguagens_do_pedido(const char *pedido);
/* contexto escolhido pelo MODELO tem relação verificável com o pedido?
 * (palavra de assunto no caminho, ou ≥ 2 no conteúdo); senão a Lia pede o
 * caminho em vez de propor sobre arquivos sem relação. Com menos de 2
 * palavras de assunto no pedido não há sinal: aceita (limite conhecido:
 * relação semântica sem palavra em comum não é vista) */
gboolean coding_contexto_relevante(const char *pedido, GHashTable *lidos, char **motivo);
/* critérios derivados do PEDIDO e da validação: validação passa; arquivos
 * a criar existem; identificadores exigidos (`crase`, CONSTANTE_X) estão no
 * diff; só testes (se o pedido proíbe produção); nada proibido tocado */
CodingPlano *coding_plano_do_pedido(const char *pedido, char **a_criar, const char *validacao);
/* filtro dos critérios SUGERIDOS pelo modelo: descarta o que viola
 * proibição, cita caminho não pedido ou dado, inventa resultado (trecho que
 * não está no pedido), cita identificador que não está no pedido nem nos
 * arquivos lidos, ou não tem relação com o pedido */
void coding_filtrar_criterios_ex(CodingPlano *p, const char *pedido, char **tambem_citados, GHashTable *lidos);
void coding_mesclar_criterios(CodingPlano *destino, CodingPlano *do_modelo);
/* conferência com o estado da tarefa (arquivos gravados e lidos, diff) */
gboolean coding_conferir_criterio_ex(const CodingCriterio *c, gboolean validacao_passou, char **tocados, char **lidos, const char *diff, char **como);
/* comando de validação configurado que o pedido nomeia, ou NULL */
char *coding_validacao_do_pedido(const char *pedido, char **nomes);
/* operação de arquivo direta só com conteúdo LITERAL (bloco ```, trecho
 * entre aspas/crases ou "com o conteúdo: ...") — o resto de um pedido de
 * código vai para o ciclo de coding, nunca para arquivo.ler/anexar */
gboolean coding_operacao_literal(const char *pedido, const char *ferramenta, const char *conteudo);
/* a validação executa código do projeto: impressão (tamanho+mtime) dos
 * arquivos que casam com as proibições, antes e depois; "" = nada mudou */
char *coding_impressao_proibidos(const char *raiz, char **proibidos);
char *coding_proibidos_alterados(const char *antes, const char *depois);
/* modelo escolhe o contexto quando as regras não acharam nada */
char *coding_prompt_contexto(const char *pedido, const char *lista_arquivos);
char **coding_ler_contexto(const char *texto, const char *lista_arquivos, char **proibidos);
/* modelo sugere critérios DEPOIS de ver os arquivos lidos */
char *coding_prompt_criterios(const char *pedido, GHashTable *lidos);

char *coding_prompt_plano(const char *pedido, const char *lista_arquivos);
/* 'simbolos': em arquivos grandes, só a região deles vai ao modelo */
char *coding_prompt_edicoes(const char *pedido, GPtrArray *criterios, const char *praticas, GHashTable *lidos,
                            const char *falha, const char *erro_anterior, int ciclo, char **simbolos);

/* critério conferido de verdade? *como: explicação curta */
gboolean coding_conferir_criterio(const CodingCriterio *c, gboolean validacao_passou, char **como);

/* checkpoint: ~/.lia/projeto/coding/<id>.json (0600), sem conteúdo de
 * arquivo nem saída de testes. 'dados' é o objeto a gravar. */
char *coding_dir_checkpoints(void);
gboolean coding_salvar_checkpoint(const char *id, JsonObject *dados, char **erro);
JsonObject *coding_carregar_checkpoint(const char *id, char **erro);
gboolean coding_id_valido(const char *id);

#endif
