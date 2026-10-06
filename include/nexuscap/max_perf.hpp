#pragma once

namespace nexuscap {

// Liga ou desfaz o desempenho maximo neste processo. A GPU so entra no perfil
// do NexusCap.exe. A prioridade REALTIME da GPU fica de fora.
void max_perf_sync(bool on);

// Na saida do processo: desfaz timer, EcoQoS e o perfil deste exe.
void max_perf_shutdown();

// Uma vez na subida, depois de ler o ini. Registra o desfazer na saida.
void max_perf_boot(bool on);

bool max_perf_is_on();

// Paragrafo fixo do checkbox, no tom do DLSS5 For All. Sem acento (fonte Latin-1).
const char* max_perf_hint();

// Estado ao vivo: GPU+CPU, so CPU, ou desligado. Sem acento.
const char* max_perf_note();

}  // namespace nexuscap
