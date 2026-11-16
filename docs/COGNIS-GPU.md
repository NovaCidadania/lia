# Relatório técnico: GT 750M no Cognis/Lia

## Hardware e driver

- GPU: GeForce GT 750M, GK107M/Kepler, 2048 MiB.
- Driver: Nouveau/NVK via Mesa 26.2.7.
- Dispositivo llama.cpp: `Vulkan0`.
- O módulo `nouveau` funciona quando carregado, mas ainda há blacklist em arquivos de modprobe; a persistência não foi alterada nesta etapa.

## Matriz de inferência

| Configuração | Resultado | Medição curta |
|---|---|---:|
| CPU, 0 camadas | concluído | 3,5 s; 2,49 tok/s |
| Vulkan, 1 camada | concluído | 4,4 s; 1,52 tok/s |
| Vulkan, 2 camadas | concluído | 7,3 s; 1,06 tok/s |
| Vulkan, 4 camadas | falhou | `vk::Queue::submit: ErrorDeviceLost` |
| Prompt maior, Vulkan 1 camada | concluído | 137 s |
| Prompt maior, Vulkan 2 camadas | concluído | 144 s |

Os testes usaram o mesmo Qwen3-8B Q5_K_M, contexto pequeno, uma requisição e geração curta. Os testes longos confirmaram que 1–2 camadas podem completar inferência, mas são mais lentas que CPU nesta máquina. A GPU não deve ser considerada uma aceleração comprovada para tarefas Cognis; é um modo experimental.

## Reprodução

Modo estável:

```bash
/home/zad/scl.sh
```

Modo Vulkan experimental:

```bash
LIA_DEVICE=Vulkan0 \
LIA_GPU_LAYERS=1 \
LIA_CONTEXT=2048 \
/home/zad/scl.sh
```

Não usar quatro camadas: a execução reproduz `ErrorDeviceLost`. Para confirmar o backend disponível:

```bash
llama-server --list-devices
```

CPU permanece o fallback recomendado para o Cognis/Lia.
