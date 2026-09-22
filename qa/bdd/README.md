# BDD NexusCap (ponta a ponta)

Cenarios Gherkin em portugues (release **4.1.5**). Correr **na mao** no HDMI; o `ctest` so cobre o que esta marcado `@auto`.

`run_e2e.ps1` e um atalho HDMI (F12, Ctrl+M, Pass Count, FG). Nao substitui olhar Cinema vs Default no ecran.

## Ordem de release

1. Automatico: `ctest -C Release` no `build` (`@auto` em `distribuicao.feature` e `sessao_cfg.feature`).
2. Lobby: [`lobby.feature`](lobby.feature) — placas, preview, coluna SESSAO, Iniciar/Sair.
3. CFG do menu principal (nao e Ctrl+M): [`lobby_cfg.feature`](lobby_cfg.feature) — F12, RenoDX no preview, cenas, Pass Count, Salvar.
4. Sessao Ctrl+M em cada x: [`sessao_cfg.feature`](sessao_cfg.feature) — NR, Default/Natural/Cinematic, Pass Count, FG, escala.
5. Atalhos: [`atalhos.feature`](atalhos.feature) — defaults, bloqueio com menu aberto, rebind.
6. Distribuicao: [`distribuicao.feature`](distribuicao.feature) — instalador, zip portable, HDMI, reports.

So publica se todos os `@critico` dessa volta estiverem OK.

## Tags

- `@critico` — bloqueia release
- `@importante` — deve passar; se falhar, anota antes de subir
- `@atalho` — teclado
- `@hdmi` — precisa de placa e consola
- `@auto` — ja coberto por `ctest` / `--selftest-*`

## Diferenca dos dois CFGs

| Onde | Ficheiro | NR / cena / Pass Count |
|------|----------|-------------------------|
| Lobby (CFG / F12) | `lobby_cfg.feature` | preview ao vivo; Pass Count pode reiniciar o **app no lobby** |
| Captura (Ctrl+M) | `sessao_cfg.feature` | grava e **reinicia a sessao** (`--autostart`) |

## Selftests que nao substituem HDMI

```
ctest -C Release -R "unit|selftest_cfg|selftest_fg"
```

`--selftest-cfg` so confirma que o ImGui desenha. Nao clica em Cinematic nem olha a imagem.
