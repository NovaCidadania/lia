/* Arquivos do projeto: a base da habilidade de codar da Lia.
 *
 * Tudo aqui opera DENTRO da raiz do projeto (proj_definir_raiz), nunca
 * fora dela: caminhos são resolvidos e conferidos (inclusive através de
 * links simbólicos), o estado interno do Git (.git/) nunca é alterado e
 * arquivos especiais (dispositivos, FIFOs, sockets) são recusados.
 *
 * Codificação: qualquer uma que o iconv do sistema conheça (o glibc traz
 * mais de mil). A detecção automática usa BOM, validação UTF-8, padrões de
 * UTF-16/32 sem BOM e, para textos de 8 bits e multibyte legados, uma
 * heurística de coerência de escrita (ver proj_detectar_codificacao). O
 * texto é sempre entregue em UTF-8; ao escrever, a codificação, o BOM e a
 * quebra de linha originais do arquivo são preservados.
 *
 * Escrita: toda alteração é calculada primeiro como PRÉVIA (diff
 * unificado, sem tocar no disco) — é ela que a pessoa aprova. Ao aplicar,
 * os arquivos originais vão para um diretório de desfazer
 * (~/.lia/projeto/desfazer/<operacao>/) e a escrita é atômica (arquivo
 * temporário + rename), preservando as permissões.
 *
 * Funções de operação devolvem JSON (string) ou NULL com *erro preenchido.
 * Não são thread-safe entre si quanto à raiz (definida uma vez, no início). */
#ifndef LIA_PROJETO_ARQUIVOS_H
#define LIA_PROJETO_ARQUIVOS_H

#include <glib.h>
#include <json-glib/json-glib.h>

#define PROJ_MAX_LEITURA_BYTES (16 * 1024 * 1024)  /* maior arquivo que é decodificado */
#define PROJ_MAX_CONTEUDO_BYTES (4 * 1024 * 1024)  /* maior conteúdo aceito numa escrita */
#define PROJ_MAX_LINHAS_LEITURA 2000
#define PROJ_MAX_PREVIA_BYTES (64 * 1024)

/* Raiz do projeto. Recusa '/', diretórios do sistema e o que não for um
 * diretório do próprio usuário. Pode ser redefinida. */
gboolean proj_definir_raiz(const char *dir, char **erro);
const char *proj_raiz(void);
/* diretório de desfazer (padrão: $HOME/.lia/projeto/desfazer) */
void proj_definir_dir_desfazer(const char *dir);

/* Caminho absoluto canônico dentro da raiz; NULL + erro se escapar dela,
 * se tocar .git/ numa escrita, etc. 'deve_existir' exige que exista. */
char *proj_resolver(const char *caminho, gboolean escrita, gboolean deve_existir, char **erro);
/* caminho relativo à raiz (para exibir) */
char *proj_relativo(const char *absoluto);

/* ----------------------------------------------------------- codificação */
typedef enum { PROJ_CONF_ALTA, PROJ_CONF_MEDIA, PROJ_CONF_BAIXA } ProjConfianca;
/* nome iconv da codificação detectada ("UTF-8", "UTF-16LE", "WINDOWS-1252",
 * "SHIFT_JIS"...) ou "binario"; *bom e *conf podem ser NULL */
char *proj_detectar_codificacao(const guchar *dados, gsize n, gboolean *bom, ProjConfianca *conf);
/* decodifica para UTF-8 (sem o BOM). 'forcada' NULL = detectar. */
char *proj_decodificar(const guchar *dados, gsize n, const char *forcada, char **usada, gboolean *bom,
                       ProjConfianca *conf, char **erro);
/* codifica UTF-8 -> 'cod' (com BOM se pedido); NULL + erro se algum
 * caractere não existir na codificação de destino */
GBytes *proj_codificar(const char *utf8, const char *cod, gboolean bom, char **erro);
/* "LF", "CRLF", "CR", "misto" ou "nenhuma" */
const char *proj_detectar_eol(const char *texto);
const char *proj_confianca_texto(ProjConfianca c);

/* ------------------------------------------------------------- leitura */
char *proj_ler(const char *caminho, gint64 linha_inicial, gint64 n_linhas, const char *codificacao, char **erro);
char *proj_listar(const char *caminho, gboolean recursivo, const char *padrao, gint64 limite, gboolean ocultos, char **erro);
char *proj_buscar(const char *texto, gboolean regex, gboolean diferenciar_maiusculas, const char *caminho,
                  const char *padrao, gint64 limite, char **erro);
char *proj_info(const char *caminho, char **erro);
/* texto inteiro (UTF-8, quebras de linha como \n) e o sha256 dos BYTES do
 * arquivo — o hash que uma edição posterior exige (sha256_esperado).
 * Recusa binários e arquivos maiores que 'max_bytes'. */
char *proj_ler_texto(const char *caminho, gsize max_bytes, char **sha256, char **erro);

/* ------------------------------------------------------------- escrita */
/* Ferramentas de escrita (nomes do manifesto): arquivo.criar, .escrever,
 * .editar, .anexar, .apagar, .mover, .copiar, .criar_pasta, .converter,
 * .permissoes, .desfazer. */
gboolean proj_ferramenta_de_escrita(const char *ferramenta);
/* Calcula a prévia (diff unificado legível, limitado) sem tocar no disco.
 * Devolve NULL + erro se a operação não puder ser feita como pedida. */
char *proj_previa(const char *ferramenta, JsonObject *params, char **erro);
/* Aplica de verdade; devolve JSON com operacao_id (para desfazer), as
 * operações feitas, o diff e onde ficaram as cópias originais. */
char *proj_aplicar(const char *ferramenta, JsonObject *params, char **erro);

/* diff unificado entre dois textos UTF-8 (exposto para testes) */
char *proj_diff(const char *rotulo, const char *antes, const char *depois, int contexto, gsize max_bytes);

#endif
