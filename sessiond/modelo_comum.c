#include "modelo.h"

void modelo_resposta_liberar(ModeloResposta *r)
{
    if (!r)
        return;
    g_free(r->resposta_texto);
    g_free(r->ferramenta_proposta);
    g_free(r->parametros_json);
    g_free(r->justificativa);
    g_free(r->erro_codigo);
    g_free(r->erro_mensagem);
}
