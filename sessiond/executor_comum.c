#include "executor.h"

void executor_resultado_liberar(ExecutorResultado *r)
{
    if (!r)
        return;
    g_free(r->dados_json);
    g_free(r->erro_codigo);
    g_free(r->erro_mensagem);
}
