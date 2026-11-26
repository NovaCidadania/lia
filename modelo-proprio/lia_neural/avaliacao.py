"""Avaliação comparativa no holdout sintético: modelo aleatório × modelo treinado × baselines (B0 do critério, B1 de referência)."""
import json, resource, time

import torch

from . import dados, runtime, tokenizer as tk
from .baseline import b0, b1
from .checkpoint import carregar
from .inferencia import propor
from .modelo import LiaGPT
from . import sandbox


def _med(f, itens):
    t0 = time.time()
    rs = [f(e) for e in itens]
    return rs, round(time.time() - t0, 2)


def avaliar(exemplos, checkpoint, semente_aleatorio=12345, log=print):
    tr, d = carregar(checkpoint)
    alea = LiaGPT(tr.config) if False else None
    torch.manual_seed(semente_aleatorio)
    alea = LiaGPT(tr.config)                      # mesma arquitetura, pesos aleatórios, ZERO passos de treino
    def por_modelo(m):
        return lambda e: runtime.julgar(e, propor(m, e["pedido"], e["codigo"])["proposta"])
    def por_b0(e):
        return runtime.julgar(e, b0(e["pedido"], e["codigo"]))
    def por_b1(e):
        esp = sandbox.rodar_casos(e["correto"], e["funcao"], e["visivel"]) if e["tipo"] == "editar" else []
        return runtime.julgar(e, b1(e["pedido"], e["codigo"], e["visivel"], esp) if e["tipo"] == "editar" else b0(e["pedido"], e["codigo"]))
    out = {}
    for nome, f in (("aleatorio_sem_treino", por_modelo(alea)), ("treinado_do_zero", por_modelo(tr)),
                    ("baseline_b0_primeira_inversao", por_b0), ("referencia_b1_busca_com_suite_visivel", por_b1)):
        rs, seg = _med(f, exemplos)
        out[nome] = {"metricas": runtime.agregar(rs), "segundos": seg, "resultados": rs}
        log("%-42s ocultos=%.1f%% aplica=%.1f%% formato=%.1f%% falsos_concluido=%d (%.1fs)" % (
            nome, out[nome]["metricas"]["testes_ocultos_aprovados_pct"], out[nome]["metricas"]["aplica_pct"],
            out[nome]["metricas"]["formato_valido_pct"], out[nome]["metricas"]["falsos_concluido"], seg))
    return out, tr, d


def veredito(rel):
    """aplica o critério FIXADO em docs/MODELO-PROPRIO.md (sem opinião): treinado > aleatório E > B0, falsos concluído = 0"""
    t = rel["treinado_do_zero"]["metricas"]
    a = rel["aleatorio_sem_treino"]["metricas"]
    b = rel["baseline_b0_primeira_inversao"]["metricas"]
    mais_que_aleatorio = t["testes_ocultos_aprovados_pct"] > a["testes_ocultos_aprovados_pct"]
    mais_que_b0 = t["testes_ocultos_aprovados_pct"] > b["testes_ocultos_aprovados_pct"]
    zero_falsos = t["falsos_concluido"] == 0
    return {"treinado_maior_que_aleatorio": mais_que_aleatorio, "treinado_maior_que_baseline_b0": mais_que_b0,
            "falsos_concluido_zero": zero_falsos, "capacidade_declaravel_na_tarefa_estreita": mais_que_aleatorio and mais_que_b0 and zero_falsos}
