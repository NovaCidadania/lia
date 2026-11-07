/* lia-gui-teste — testes de gui/protocolo.c (parsing, validação, sessão).
 *
 * Programa comum (libc + GLib/json-glib), sem GTK — roda sem display, via
 * `make teste-gui`. Mesmo estilo de relatório que testes/teste.sh: imprime
 * "ok   .../FALHA ..." por caso e um total no final.
 */
#include "protocolo.h"
#include <stdio.h>
#include <string.h>

static int g_total = 0, g_falhas = 0;

static void checar(int condicao, const char *descricao)
{
    g_total++;
    if (condicao) {
        printf("ok   %s\n", descricao);
    } else {
        g_falhas++;
        printf("FALHA %s\n", descricao);
    }
}

static void testar_parsing_valido(void)
{
    LiaMsg m;
    LiaMsgErro e = lia_msg_parsear(
        "{\"protocolo\":\"lia.v1\",\"tipo\":\"plano\",\"id\":\"id-1\","
        "\"em_resposta_a\":null,\"ts\":\"2026-11-07T10:00:00Z\","
        "\"dados\":{\"plano_id\":\"p1\",\"risco\":\"nenhum\"}}", &m);
    checar(e == LIA_MSG_OK, "parsear mensagem 'plano' válida");
    checar(m.tipo == LIA_MSG_PLANO, "tipo reconhecido como LIA_MSG_PLANO");
    checar(strcmp(m.id, "id-1") == 0, "id extraído corretamente");
    checar(m.em_resposta_a == NULL, "em_resposta_a null -> NULL em C");
    checar(m.dados != NULL && json_object_has_member(m.dados, "plano_id"), "dados.plano_id acessível");
    lia_msg_liberar(&m);
}

static void testar_json_invalido(void)
{
    LiaMsg m;
    checar(lia_msg_parsear("", &m) == LIA_MSG_ERRO_JSON_INVALIDO, "linha vazia -> JSON inválido");
    checar(lia_msg_parsear("{isto nao é json", &m) == LIA_MSG_ERRO_JSON_INVALIDO, "JSON malformado -> inválido");
    checar(lia_msg_parsear("[1,2,3]", &m) == LIA_MSG_ERRO_ENVELOPE_INVALIDO, "array no topo -> envelope inválido");
    checar(lia_msg_parsear("\"so uma string\"", &m) == LIA_MSG_ERRO_ENVELOPE_INVALIDO, "escalar no topo -> envelope inválido");
}

static void testar_envelope_incompleto(void)
{
    LiaMsg m;
    checar(lia_msg_parsear("{\"tipo\":\"plano\",\"id\":\"x\",\"ts\":\"t\"}", &m)
               == LIA_MSG_ERRO_ENVELOPE_INVALIDO, "sem 'protocolo' -> envelope inválido");
    checar(lia_msg_parsear("{\"protocolo\":\"lia.v1\",\"id\":\"x\",\"ts\":\"t\"}", &m)
               == LIA_MSG_ERRO_ENVELOPE_INVALIDO, "sem 'tipo' -> envelope inválido");
    checar(lia_msg_parsear("{\"protocolo\":\"lia.v1\",\"tipo\":\"plano\",\"ts\":\"t\"}", &m)
               == LIA_MSG_ERRO_ENVELOPE_INVALIDO, "sem 'id' -> envelope inválido");
    checar(lia_msg_parsear(
               "{\"protocolo\":\"lia.v1\",\"tipo\":\"plano\",\"id\":\"x\",\"ts\":\"t\",\"dados\":[1,2]}", &m)
               == LIA_MSG_ERRO_ENVELOPE_INVALIDO, "'dados' como array -> envelope inválido");
}

static void testar_protocolo_incompativel(void)
{
    LiaMsg m;
    checar(lia_msg_parsear(
               "{\"protocolo\":\"lia.v2\",\"tipo\":\"plano\",\"id\":\"x\",\"ts\":\"t\"}", &m)
               == LIA_MSG_ERRO_PROTOCOLO_INCOMPATIVEL, "protocolo 'lia.v2' -> protocolo_incompativel");
    checar(lia_msg_parsear(
               "{\"protocolo\":\"\",\"tipo\":\"plano\",\"id\":\"x\",\"ts\":\"t\"}", &m)
               == LIA_MSG_ERRO_PROTOCOLO_INCOMPATIVEL, "protocolo vazio -> protocolo_incompativel");
}

static void testar_tipo_desconhecido(void)
{
    LiaMsg m;
    checar(lia_msg_parsear(
               "{\"protocolo\":\"lia.v1\",\"tipo\":\"apagar_tudo\",\"id\":\"x\",\"ts\":\"t\"}", &m)
               == LIA_MSG_ERRO_TIPO_DESCONHECIDO, "tipo inventado -> tipo desconhecido (rejeitado)");
}

static void testar_fora_de_ordem(void)
{
    LiaSessao s;
    lia_sessao_iniciar(&s);

    LiaMsg plano, resultado1, resultado2, progresso_apos_fim;
    lia_msg_parsear(
        "{\"protocolo\":\"lia.v1\",\"tipo\":\"plano\",\"id\":\"m1\",\"ts\":\"t\","
        "\"dados\":{\"plano_id\":\"p1\"}}", &plano);
    checar(lia_sessao_receber(&s, &plano) == LIA_SESSAO_OK, "fora de ordem: 'plano' inicial aceito");

    lia_msg_parsear(
        "{\"protocolo\":\"lia.v1\",\"tipo\":\"resultado\",\"id\":\"m2\",\"ts\":\"t\","
        "\"dados\":{\"execucao_id\":\"e1\",\"estado\":\"sucesso\"}}", &resultado1);
    checar(lia_sessao_receber(&s, &resultado1) == LIA_SESSAO_OK, "fora de ordem: primeiro 'resultado' de e1 aceito");

    lia_msg_parsear(
        "{\"protocolo\":\"lia.v1\",\"tipo\":\"resultado\",\"id\":\"m3\",\"ts\":\"t\","
        "\"dados\":{\"execucao_id\":\"e1\",\"estado\":\"sucesso\"}}", &resultado2);
    checar(lia_sessao_receber(&s, &resultado2) == LIA_SESSAO_ERRO_EXECUCAO_JA_CONCLUIDA,
           "fora de ordem: segundo 'resultado' da mesma execução é rejeitado");

    lia_msg_parsear(
        "{\"protocolo\":\"lia.v1\",\"tipo\":\"progresso\",\"id\":\"m4\",\"ts\":\"t\","
        "\"dados\":{\"execucao_id\":\"e1\",\"fracao\":0.5}}", &progresso_apos_fim);
    checar(lia_sessao_receber(&s, &progresso_apos_fim) == LIA_SESSAO_ERRO_EXECUCAO_JA_CONCLUIDA,
           "fora de ordem: 'progresso' depois do 'resultado' final é rejeitado");

    lia_msg_liberar(&plano);
    lia_msg_liberar(&resultado1);
    lia_msg_liberar(&resultado2);
    lia_msg_liberar(&progresso_apos_fim);
    lia_sessao_liberar(&s);
}

static void testar_mensagem_invalida_em_resposta_a_desconhecido(void)
{
    LiaSessao s;
    lia_sessao_iniciar(&s);

    LiaMsg m;
    lia_msg_parsear(
        "{\"protocolo\":\"lia.v1\",\"tipo\":\"resultado\",\"id\":\"m1\","
        "\"em_resposta_a\":\"id-que-nunca-existiu\",\"ts\":\"t\","
        "\"dados\":{\"execucao_id\":\"e1\"}}", &m);
    checar(lia_sessao_receber(&s, &m) == LIA_SESSAO_ERRO_RESPOSTA_DESCONHECIDA,
           "mensagem inválida: em_resposta_a aponta para id nunca visto -> rejeitada");
    lia_msg_liberar(&m);
    lia_sessao_liberar(&s);
}

static void testar_cancelamento(void)
{
    LiaSessao s;
    lia_sessao_iniciar(&s);

    checar(lia_sessao_validar_envio(&s, LIA_MSG_CANCELAR, NULL, 0) == LIA_ENVIO_RECUSADO_NADA_PARA_CANCELAR,
           "cancelamento: sem execução em andamento -> recusado localmente");

    LiaMsg progresso;
    lia_msg_parsear(
        "{\"protocolo\":\"lia.v1\",\"tipo\":\"progresso\",\"id\":\"m1\",\"ts\":\"t\","
        "\"dados\":{\"execucao_id\":\"e9\",\"fracao\":0.1}}", &progresso);
    lia_sessao_receber(&s, &progresso);

    checar(lia_sessao_validar_envio(&s, LIA_MSG_CANCELAR, NULL, 0) == LIA_ENVIO_OK,
           "cancelamento: com execução em andamento -> permitido mesmo em modo_leitura");

    lia_msg_liberar(&progresso);
    lia_sessao_liberar(&s);
}

static void testar_desconexao(void)
{
    LiaSessao s;
    lia_sessao_iniciar(&s);
    lia_sessao_conectar(&s);
    checar(s.conectado, "desconexão: sessão inicia conectada após lia_sessao_conectar");

    LiaMsg progresso;
    lia_msg_parsear(
        "{\"protocolo\":\"lia.v1\",\"tipo\":\"progresso\",\"id\":\"m1\",\"ts\":\"t\","
        "\"dados\":{\"execucao_id\":\"e9\",\"fracao\":0.1}}", &progresso);
    lia_sessao_receber(&s, &progresso);

    lia_sessao_desconectar(&s);
    checar(!s.conectado, "desconexão: flag 'conectado' cai para falso");
    checar(lia_sessao_validar_envio(&s, LIA_MSG_CANCELAR, NULL, 0) == LIA_ENVIO_OK,
           "desconexão: estado de execução pendente sobrevive à desconexão (não é apagado)");

    lia_msg_liberar(&progresso);
    lia_sessao_liberar(&s);
}

static void testar_capacidade_nao_autorizada(void)
{
    LiaSessao s;
    lia_sessao_iniciar(&s);

    checar(lia_sessao_validar_envio(&s, LIA_MSG_SOLICITAR, NULL, 0) == LIA_ENVIO_RECUSADO_SOMENTE_LEITURA,
           "capacidade não autorizada: 'solicitar' recusado localmente em modo_leitura");
    checar(lia_sessao_validar_envio(&s, LIA_MSG_APROVAR, NULL, 0) == LIA_ENVIO_RECUSADO_SOMENTE_LEITURA,
           "capacidade não autorizada: 'aprovar' recusado localmente em modo_leitura");

    const char *canais_ok[] = { "auditoria", "execucao" };
    checar(lia_sessao_validar_envio(&s, LIA_MSG_ASSINAR, canais_ok, 2) == LIA_ENVIO_OK,
           "assinar canais conhecidos ('auditoria','execucao') -> permitido");

    const char *canais_ruim[] = { "auditoria", "admin_total" };
    checar(lia_sessao_validar_envio(&s, LIA_MSG_ASSINAR, canais_ruim, 2) == LIA_ENVIO_RECUSADO_CANAL_DESCONHECIDO,
           "capacidade não autorizada: canal 'admin_total' inventado -> recusado");

    lia_sessao_liberar(&s);
}

static void testar_execucao_sem_servidor(void)
{
    /* Sem daemon (nenhum socket conectado): a sessão deve ficar em estado
     * utilizável — nunca travar nem assumir que há dados — e continuar
     * recusando envios fora do modo_leitura exatamente como se conectada. */
    LiaSessao s;
    lia_sessao_iniciar(&s);
    checar(!s.conectado, "modo sem servidor: sessão nova começa desconectada");
    checar(lia_sessao_validar_envio(&s, LIA_MSG_SOLICITAR, NULL, 0) == LIA_ENVIO_RECUSADO_SOMENTE_LEITURA,
           "modo sem servidor: validação local funciona sem nenhuma conexão");
    checar(lia_sessao_validar_envio(&s, LIA_MSG_CANCELAR, NULL, 0) == LIA_ENVIO_RECUSADO_NADA_PARA_CANCELAR,
           "modo sem servidor: cancelar sem nada pendente é recusado, não travado");
    lia_sessao_liberar(&s);
}

int main(void)
{
    testar_parsing_valido();
    testar_json_invalido();
    testar_envelope_incompleto();
    testar_protocolo_incompativel();
    testar_tipo_desconhecido();
    testar_fora_de_ordem();
    testar_mensagem_invalida_em_resposta_a_desconhecido();
    testar_cancelamento();
    testar_desconexao();
    testar_capacidade_nao_autorizada();
    testar_execucao_sem_servidor();

    printf("\n%d testes, %d falhas\n", g_total, g_falhas);
    return g_falhas == 0 ? 0 : 1;
}
